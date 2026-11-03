/** @file
  MSR read/write view (boot processor only).

  Unsupported MSRs raise #GP. While this view runs, a #GP handler is
  registered through EFI_CPU_ARCH_PROTOCOL that skips the faulting RDMSR /
  WRMSR instruction, so probing an unknown MSR reports an error instead of
  hanging the machine.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "UefiRrw.h"
#include <Protocol/Cpu.h>

#define MAX_MSR_ENTRIES  48

typedef struct {
  UINT32          Index;
  CONST CHAR16    *Name;
  BOOLEAN         Valid;
  UINT64          Value;
} MSR_ENTRY;

STATIC MSR_ENTRY  mMsrs[MAX_MSR_ENTRIES] = {
  { 0x00000010, L"IA32_TIME_STAMP_COUNTER" },
  { 0x00000017, L"IA32_PLATFORM_ID"        },
  { 0x0000001B, L"IA32_APIC_BASE"          },
  { 0x0000003A, L"IA32_FEATURE_CONTROL"    },
  { 0x0000008B, L"IA32_BIOS_SIGN_ID"       },
  { 0x000000CE, L"MSR_PLATFORM_INFO"       },
  { 0x000000FE, L"IA32_MTRRCAP"            },
  { 0x00000198, L"IA32_PERF_STATUS"        },
  { 0x00000199, L"IA32_PERF_CTL"           },
  { 0x0000019C, L"IA32_THERM_STATUS"       },
  { 0x000001A0, L"IA32_MISC_ENABLE"        },
  { 0x000001A2, L"MSR_TEMPERATURE_TARGET"  },
  { 0x00000200, L"IA32_MTRR_PHYSBASE0"     },
  { 0x00000201, L"IA32_MTRR_PHYSMASK0"     },
  { 0x00000277, L"IA32_PAT"                },
  { 0x000002FF, L"IA32_MTRR_DEF_TYPE"      },
  { 0xC0000080, L"IA32_EFER"               },
  { 0xC0000081, L"IA32_STAR"               },
  { 0xC0000100, L"IA32_FS_BASE"            },
  { 0xC0000101, L"IA32_GS_BASE"            },
};
STATIC UINTN      mMsrCount = 20;

STATIC EFI_CPU_ARCH_PROTOCOL  *mCpu;
STATIC BOOLEAN                mGuardInstalled;
STATIC volatile BOOLEAN       mFaulted;

STATIC
VOID
EFIAPI
GpFaultHandler (
  IN     EFI_EXCEPTION_TYPE  ExceptionType,
  IN OUT EFI_SYSTEM_CONTEXT  SystemContext
  )
{
  UINT8  *Ip;

  Ip = (UINT8 *)(UINTN)SystemContext.SystemContextX64->Rip;
  if ((Ip[0] == 0x0F) && ((Ip[1] == 0x32) || (Ip[1] == 0x30))) {
    SystemContext.SystemContextX64->Rip += 2;   // skip RDMSR / WRMSR
    mFaulted                             = TRUE;
  }
}

STATIC
VOID
GuardInstall (
  VOID
  )
{
  if (mCpu == NULL) {
    gBS->LocateProtocol (&gEfiCpuArchProtocolGuid, NULL, (VOID **)&mCpu);
  }

  if ((mCpu != NULL) && !mGuardInstalled) {
    mGuardInstalled = (BOOLEAN) !EFI_ERROR (mCpu->RegisterInterruptHandler (mCpu, EXCEPT_X64_GP_FAULT, GpFaultHandler));
  }
}

STATIC
VOID
GuardRemove (
  VOID
  )
{
  if ((mCpu != NULL) && mGuardInstalled) {
    mCpu->RegisterInterruptHandler (mCpu, EXCEPT_X64_GP_FAULT, NULL);
    mGuardInstalled = FALSE;
  }
}

STATIC
EFI_STATUS
SafeReadMsr (
  IN  UINT32  Index,
  OUT UINT64  *Value
  )
{
  if (!mGuardInstalled) {
    return EFI_UNSUPPORTED;   // never probe without the guard
  }

  mFaulted = FALSE;
  *Value   = AsmReadMsr64 (Index);
  return mFaulted ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}

STATIC
EFI_STATUS
SafeWriteMsr (
  IN UINT32  Index,
  IN UINT64  Value
  )
{
  if (!mGuardInstalled) {
    return EFI_UNSUPPORTED;
  }

  mFaulted = FALSE;
  AsmWriteMsr64 (Index, Value);
  return mFaulted ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}

STATIC
VOID
ReadAll (
  VOID
  )
{
  UINTN  Index;

  for (Index = 0; Index < mMsrCount; Index++) {
    mMsrs[Index].Valid = (BOOLEAN) !EFI_ERROR (SafeReadMsr (mMsrs[Index].Index, &mMsrs[Index].Value));
  }
}

STATIC
VOID
CpuBrand (
  OUT CHAR16  *Buf,
  IN  UINTN   BufSize
  )
{
  UINT32  Regs[12];
  UINT32  MaxExt;
  CHAR8   Brand[49];
  UINTN   Index;
  CHAR8   *Start;

  AsmCpuid (0x80000000, &MaxExt, NULL, NULL, NULL);
  if (MaxExt < 0x80000004) {
    UnicodeSPrint (Buf, BufSize, L"(brand string not available)");
    return;
  }

  for (Index = 0; Index < 3; Index++) {
    AsmCpuid (
      0x80000002 + (UINT32)Index,
      &Regs[Index * 4],
      &Regs[Index * 4 + 1],
      &Regs[Index * 4 + 2],
      &Regs[Index * 4 + 3]
      );
  }

  CopyMem (Brand, Regs, 48);
  Brand[48] = '\0';
  Start     = Brand;
  while (*Start == ' ') {
    Start++;
  }

  UnicodeSPrint (Buf, BufSize, L"%a", Start);
}

STATIC
VOID
DrawBits (
  IN UINTN   Row,
  IN UINT64  Value
  )
{
  INTN    Bit;
  CHAR16  Line[80];
  UINTN   Pos;

  UiPrintAt (1, Row, ATTR_HEADER, L"Bits  63      56 55      48 47      40 39      32 31      24 23      16 15       8 7        0");
  Pos = 0;
  for (Bit = 63; Bit >= 0; Bit--) {
    Line[Pos++] = (RShiftU64 (Value, (UINTN)Bit) & 1) ? L'1' : L'0';
    if (((Bit % 8) == 0) && (Bit != 0)) {
      Line[Pos++] = L' ';
      Line[Pos++] = L' ';
    }
  }

  Line[Pos] = L'\0';
  UiFillLine (Row + 1, ATTR_NORMAL);
  UiPrintAt (7, Row + 1, ATTR_NORMAL, L"%s", Line);
}

VOID
MsrViewRun (
  VOID
  )
{
  UINTN          Sel;
  UINTN          Top;
  UINTN          Visible;
  UINTN          Row;
  UINTN          Index;
  EFI_INPUT_KEY  Key;
  MSR_ENTRY      *M;
  UINT64         Input;
  EFI_STATUS     Status;
  CHAR16         Brand[64];
  CHAR16         Question[96];
  BOOLEAN        Redraw;

  GuardInstall ();
  CpuBrand (Brand, sizeof (Brand));
  ReadAll ();

  Sel    = 0;
  Top    = 0;
  Redraw = TRUE;
  for ( ; ;) {
    Visible = gScreenRows - 9;
    if (Redraw) {
      UiFrame (L"MSR (boot processor)", L"Up/Down:select  F5:re-read  A:add MSR  W:write  Esc:back");
      UiPrintAt (1, 1, ATTR_HEADER, L"CPU: %s", Brand);
      if (!mGuardInstalled) {
        UiStatus (ATTR_WARN, L"#GP guard unavailable (no CPU Arch protocol): MSR access disabled.");
      }

      Redraw = FALSE;
    }

    if (Sel < Top) {
      Top = Sel;
    } else if (Sel >= Top + Visible) {
      Top = Sel - Visible + 1;
    }

    UiPrintAt (1, 2, ATTR_HEADER, L" MSR        Name                           Value");
    for (Row = 0; Row < Visible; Row++) {
      Index = Top + Row;
      UiFillLine (3 + Row, ATTR_NORMAL);
      if (Index >= mMsrCount) {
        continue;
      }

      M = &mMsrs[Index];
      if (M->Valid) {
        UiPrintAt (1, 3 + Row, (Index == Sel) ? ATTR_HILITE : ATTR_NORMAL, L" %08x   %-30s %016lx ", M->Index, M->Name, M->Value);
      } else {
        UiPrintAt (1, 3 + Row, (Index == Sel) ? ATTR_HILITE : ATTR_DIM, L" %08x   %-30s <#GP: not supported> ", M->Index, M->Name);
      }
    }

    M = &mMsrs[Sel];
    if (M->Valid) {
      DrawBits (gScreenRows - 5, M->Value);
    } else {
      UiFillLine (gScreenRows - 5, ATTR_NORMAL);
      UiFillLine (gScreenRows - 4, ATTR_NORMAL);
    }

    UiReadKey (&Key);
    switch (Key.ScanCode) {
      case SCAN_ESC:
        GuardRemove ();
        return;
      case SCAN_UP:
        if (Sel > 0) {
          Sel--;
        }

        break;
      case SCAN_DOWN:
        if (Sel + 1 < mMsrCount) {
          Sel++;
        }

        break;
      case SCAN_F5:
        ReadAll ();
        UiStatus (ATTR_NORMAL, L"Re-read %d MSRs.", mMsrCount);
        break;
      default:
        break;
    }

    if ((Key.UnicodeChar == L'a') || (Key.UnicodeChar == L'A')) {
      if (mMsrCount >= MAX_MSR_ENTRIES) {
        UiStatus (ATTR_WARN, L"List is full.");
        continue;
      }

      Input = 0;
      if (UiPromptHex (L"MSR index", 8, &Input)) {
        M        = &mMsrs[mMsrCount];
        M->Index = (UINT32)Input;
        M->Name  = L"(user)";
        M->Valid = (BOOLEAN) !EFI_ERROR (SafeReadMsr (M->Index, &M->Value));
        Sel      = mMsrCount++;
        UiStatus (M->Valid ? ATTR_NORMAL : ATTR_WARN, M->Valid ? L"Added MSR %x." : L"MSR %x raised #GP.", M->Index);
      }
    } else if ((Key.UnicodeChar == L'w') || (Key.UnicodeChar == L'W')) {
      M     = &mMsrs[Sel];
      Input = M->Value;
      if (UiPromptHex (L"New 64-bit value", 16, &Input)) {
        UnicodeSPrint (Question, sizeof (Question), L"Write %016lx to MSR %08x (%s)?", Input, M->Index, M->Name);
        if (UiConfirm (Question)) {
          Status = SafeWriteMsr (M->Index, Input);
          M->Valid = (BOOLEAN) !EFI_ERROR (SafeReadMsr (M->Index, &M->Value));
          if (EFI_ERROR (Status)) {
            UiStatus (ATTR_WARN, L"WRMSR %x raised #GP: value rejected.", M->Index);
          } else {
            UiStatus (ATTR_NORMAL, L"Wrote MSR %x, read back %016lx.", M->Index, M->Value);
          }
        }
      }
    }
  }
}
