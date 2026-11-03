/** @file
  ACPI table viewer: RSDP -> XSDT/RSDT -> every table (plus DSDT and FACS
  from the FADT), with checksum check, header decode, field decode for
  FADT, MADT, MCFG, HPET, DMAR, and a raw dump of every table.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>
#include "UefiRrw.h"
#include <Guid/Acpi.h>
#include <IndustryStandard/Acpi.h>

#define MAX_TABLES  128
#define RAW_MAX     (512 * 1024)

typedef struct {
  UINT8     *Ptr;
  UINT32    Length;
  BOOLEAN   HasHeader;     // FACS has no standard header
} ACPI_ENTRY;

STATIC ACPI_ENTRY  mTables[MAX_TABLES];
STATIC UINTN       mTableCount;
STATIC BOOLEAN     mAmlReady;
STATIC UINT8       *mRsdp;

STATIC
UINT16
R16 (
  IN CONST UINT8  *P
  )
{
  return (UINT16)(P[0] | (P[1] << 8));
}

STATIC
UINT32
R32 (
  IN CONST UINT8  *P
  )
{
  return (UINT32)P[0] | ((UINT32)P[1] << 8) | ((UINT32)P[2] << 16) | ((UINT32)P[3] << 24);
}

STATIC
UINT64
R64 (
  IN CONST UINT8  *P
  )
{
  return (UINT64)R32 (P) | LShiftU64 (R32 (P + 4), 32);
}

STATIC
UINT8
Sum8 (
  IN CONST UINT8  *P,
  IN UINTN        Len
  )
{
  UINT8  S;

  S = 0;
  while (Len-- > 0) {
    S = (UINT8)(S + *P++);
  }

  return S;
}

/**
  Copy N bytes of ASCII into a CHAR16 string, replacing non-printables.
**/
STATIC
VOID
AsciiField (
  OUT CHAR16       *Out,
  IN  CONST UINT8  *In,
  IN  UINTN        N
  )
{
  UINTN  Index;

  for (Index = 0; Index < N; Index++) {
    Out[Index] = ((In[Index] >= 0x20) && (In[Index] < 0x7F)) ? (CHAR16)In[Index] : L' ';
  }

  Out[N] = L'\0';
}

STATIC
BOOLEAN
Known (
  IN UINT8  *Ptr
  )
{
  UINTN  Index;

  for (Index = 0; Index < mTableCount; Index++) {
    if (mTables[Index].Ptr == Ptr) {
      return TRUE;
    }
  }

  return FALSE;
}

STATIC
VOID
AddTable (
  IN UINT64   Address,
  IN BOOLEAN  HasHeader
  )
{
  UINT8   *Ptr;
  UINT32  Len;

  if ((Address == 0) || (Address > MAX_ADDRESS) || (mTableCount >= MAX_TABLES)) {
    return;
  }

  Ptr = (UINT8 *)(UINTN)Address;
  if (Known (Ptr)) {
    return;
  }

  Len = R32 (Ptr + 4);
  if ((Len < (HasHeader ? 36u : 8u)) || (Len > 16 * 1024 * 1024)) {
    return;
  }

  mTables[mTableCount].Ptr       = Ptr;
  mTables[mTableCount].Length    = Len;
  mTables[mTableCount].HasHeader = HasHeader;
  mTableCount++;
}

STATIC
VOID
ScanAcpi (
  VOID
  )
{
  UINT8   *Sdt;
  UINT32  Len;
  UINTN   Entry;
  UINTN   Index;
  UINT8   *Fadt;
  BOOLEAN Xsdt;

  mTableCount = 0;
  mAmlReady   = FALSE;
  mRsdp       = NULL;
  if (EFI_ERROR (EfiGetSystemConfigurationTable (&gEfiAcpi20TableGuid, (VOID **)&mRsdp)) || (mRsdp == NULL)) {
    if (EFI_ERROR (EfiGetSystemConfigurationTable (&gEfiAcpi10TableGuid, (VOID **)&mRsdp))) {
      mRsdp = NULL;
      return;
    }
  }

  Xsdt = (BOOLEAN)((mRsdp[15] >= 2) && (R64 (mRsdp + 24) != 0));
  Sdt  = Xsdt ? (UINT8 *)(UINTN)R64 (mRsdp + 24) : (UINT8 *)(UINTN)R32 (mRsdp + 16);
  if (Sdt == NULL) {
    return;
  }

  AddTable ((UINTN)Sdt, TRUE);
  Len = R32 (Sdt + 4);
  for (Entry = 36; Entry + (Xsdt ? 8 : 4) <= Len; Entry += (Xsdt ? 8 : 4)) {
    AddTable (Xsdt ? R64 (Sdt + Entry) : R32 (Sdt + Entry), TRUE);
  }

  //
  // DSDT and FACS are only referenced from the FADT.
  //
  for (Index = 0; Index < mTableCount; Index++) {
    Fadt = mTables[Index].Ptr;
    if (CompareMem (Fadt, "FACP", 4) != 0) {
      continue;
    }

    Len = mTables[Index].Length;
    if ((Len >= 148) && (R64 (Fadt + 140) != 0)) {
      AddTable (R64 (Fadt + 140), TRUE);
    } else {
      AddTable (R32 (Fadt + 40), TRUE);
    }

    if ((Len >= 140) && (R64 (Fadt + 132) != 0)) {
      AddTable (R64 (Fadt + 132), FALSE);
    } else {
      AddTable (R32 (Fadt + 36), FALSE);
    }
  }
}

//
// ---- Decoders ----------------------------------------------------------------------
//

STATIC CONST CHAR16  *mSpaceNames[] = { L"Memory", L"IO", L"PCI Config", L"EC", L"SMBus", L"CMOS", L"PCI BAR", L"IPMI", L"GPIO", L"Serial", L"PCC", L"PRM" };

STATIC
VOID
AddGas (
  IN TEXT_DOC      *Doc,
  IN CONST CHAR16  *Label,
  IN CONST UINT8   *G
  )
{
  if (R64 (G + 4) == 0) {
    DocAdd (Doc, ATTR_DIM, L"  %-18s (not present)", Label);
    return;
  }

  DocAdd (
    Doc,
    ATTR_NORMAL,
    L"  %-18s %s %lX  width %d bits, offset %d, access %d",
    Label,
    (G[0] < ARRAY_SIZE (mSpaceNames)) ? mSpaceNames[G[0]] : L"Space?",
    R64 (G + 4),
    G[1],
    G[2],
    G[3]
    );
}

STATIC
VOID
DecodeFadt (
  IN TEXT_DOC     *Doc,
  IN CONST UINT8  *T,
  IN UINT32       Len
  )
{
  STATIC CONST CHAR16  *Profiles[] = {
    L"Unspecified", L"Desktop", L"Mobile", L"Workstation", L"Enterprise Server", L"SOHO Server",
    L"Appliance PC", L"Performance Server", L"Tablet"
  };
  STATIC CONST CHAR16  *FlagNames[] = {
    L"WBINVD", L"WBINVD_FLUSH", L"PROC_C1", L"P_LVL2_UP", L"PWR_BUTTON", L"SLP_BUTTON", L"FIX_RTC",
    L"RTC_S4", L"TMR_VAL_EXT", L"DCK_CAP", L"RESET_REG_SUP", L"SEALED_CASE", L"HEADLESS", L"CPU_SW_SLP",
    L"PCI_EXP_WAK", L"USE_PLATFORM_CLOCK", L"S4_RTC_STS_VALID", L"REMOTE_POWER_ON", L"FORCE_APIC_CLUSTER",
    L"FORCE_APIC_PHYS", L"HW_REDUCED_ACPI", L"LOW_POWER_S0_IDLE", L"PERSISTENT_CPU_CACHES"
  };
  UINT32  Flags;
  UINTN   Bit;
  CHAR16  Buf[200];
  UINTN   Pos;

  if (Len < 116) {
    return;
  }

  DocAdd (Doc, ATTR_HEADER, L"Fixed ACPI Description Table");
  DocAdd (Doc, ATTR_NORMAL, L"  FACS %08X  DSDT %08X%s", R32 (T + 36), R32 (T + 40), (Len >= 148) ? L"" : L"");
  if (Len >= 148) {
    DocAdd (Doc, ATTR_NORMAL, L"  X_FACS %lX  X_DSDT %lX", R64 (T + 132), R64 (T + 140));
  }

  DocAdd (Doc, ATTR_NORMAL, L"  PM profile         %s", (T[45] < ARRAY_SIZE (Profiles)) ? Profiles[T[45]] : L"Reserved");
  DocAdd (Doc, ATTR_NORMAL, L"  SCI interrupt      %d", R16 (T + 46));
  DocAdd (Doc, ATTR_NORMAL, L"  SMI_CMD            %04X  (ACPI_ENABLE %02X, ACPI_DISABLE %02X, S4BIOS %02X, PSTATE %02X, CST %02X)", R32 (T + 48), T[52], T[53], T[54], T[55], T[95]);
  DocAdd (Doc, ATTR_NORMAL, L"  PM1a_EVT_BLK       %04X  PM1b_EVT_BLK %04X  len %d", R32 (T + 56), R32 (T + 60), T[88]);
  DocAdd (Doc, ATTR_NORMAL, L"  PM1a_CNT_BLK       %04X  PM1b_CNT_BLK %04X  len %d", R32 (T + 64), R32 (T + 68), T[89]);
  DocAdd (Doc, ATTR_NORMAL, L"  PM2_CNT_BLK        %04X  len %d", R32 (T + 72), T[90]);
  DocAdd (Doc, ATTR_NORMAL, L"  PM_TMR_BLK         %04X  len %d", R32 (T + 76), T[91]);
  DocAdd (Doc, ATTR_NORMAL, L"  GPE0_BLK           %04X  len %d", R32 (T + 80), T[92]);
  DocAdd (Doc, ATTR_NORMAL, L"  GPE1_BLK           %04X  len %d  base %d", R32 (T + 84), T[93], T[94]);
  DocAdd (Doc, ATTR_NORMAL, L"  C2 latency %d us, C3 latency %d us", R16 (T + 96), R16 (T + 98));
  DocAdd (Doc, ATTR_NORMAL, L"  Century (CMOS)     %02X   Day alarm %02X  Month alarm %02X", T[108], T[106], T[107]);
  DocAdd (
    Doc,
    ATTR_NORMAL,
    L"  IAPC_BOOT_ARCH     %04X %s%s%s%s",
    R16 (T + 109),
    (R16 (T + 109) & BIT0) ? L" LEGACY_DEVICES" : L"",
    (R16 (T + 109) & BIT1) ? L" 8042" : L"",
    (R16 (T + 109) & BIT2) ? L" VGA_NOT_PRESENT" : L"",
    (R16 (T + 109) & BIT4) ? L" PCIE_ASPM_CONTROLS" : L""
    );
  Flags = R32 (T + 112);
  Pos   = UnicodeSPrint (Buf, sizeof (Buf), L"  Flags              %08X", Flags);
  DocAdd (Doc, ATTR_NORMAL, L"%s", Buf);
  for (Bit = 0; Bit < ARRAY_SIZE (FlagNames); Bit++) {
    if (Flags & (1u << Bit)) {
      DocAdd (Doc, ATTR_DIM, L"                     bit %2d  %s", Bit, FlagNames[Bit]);
    }
  }

  (VOID)Pos;
  if (Len >= 129) {
    AddGas (Doc, L"RESET_REG", T + 116);
    DocAdd (Doc, ATTR_NORMAL, L"  RESET_VALUE        %02X", T[128]);
  }

  if (Len >= 132) {
    DocAdd (Doc, ATTR_NORMAL, L"  FADT minor version %d.%d", T[8], T[131] & 0x0F);
  }

  if (Len >= 244) {
    AddGas (Doc, L"X_PM1a_EVT_BLK", T + 148);
    AddGas (Doc, L"X_PM1a_CNT_BLK", T + 172);
    AddGas (Doc, L"X_PM_TMR_BLK", T + 208);
    AddGas (Doc, L"X_GPE0_BLK", T + 220);
  }

  if (Len >= 268) {
    AddGas (Doc, L"SLEEP_CONTROL_REG", T + 244);
    AddGas (Doc, L"SLEEP_STATUS_REG", T + 256);
  }
}

STATIC
VOID
DecodeMadt (
  IN TEXT_DOC     *Doc,
  IN CONST UINT8  *T,
  IN UINT32       Len
  )
{
  UINT32  Off;
  UINT8   Type;
  UINT8   L;
  UINTN   Cpus;
  UINTN   Enabled;
  UINT32  Flags;

  if (Len < 44) {
    return;
  }

  DocAdd (Doc, ATTR_HEADER, L"Multiple APIC Description Table");
  DocAdd (Doc, ATTR_NORMAL, L"  Local APIC address %08X   Flags %08X%s", R32 (T + 36), R32 (T + 40), (R32 (T + 40) & 1) ? L" (PC-AT dual 8259)" : L"");
  Cpus    = 0;
  Enabled = 0;
  for (Off = 44; Off + 2 <= Len; Off += L) {
    Type = T[Off];
    L    = T[Off + 1];
    if ((L < 2) || (Off + L > Len)) {
      break;
    }

    switch (Type) {
      case 0:
        Flags = R32 (T + Off + 4);
        Cpus++;
        Enabled += (Flags & 1) ? 1 : 0;
        DocAdd (Doc, (Flags & 1) ? ATTR_NORMAL : ATTR_DIM, L"  Local APIC        UID %3d  APIC ID %3d  %s", T[Off + 2], T[Off + 3], (Flags & 1) ? L"enabled" : ((Flags & 2) ? L"online capable" : L"disabled"));
        break;
      case 1:
        DocAdd (Doc, ATTR_NORMAL, L"  I/O APIC          ID %d  address %08X  GSI base %d", T[Off + 2], R32 (T + Off + 4), R32 (T + Off + 8));
        break;
      case 2:
        DocAdd (Doc, ATTR_NORMAL, L"  IRQ override      bus %d  IRQ %2d -> GSI %2d  flags %04X", T[Off + 2], T[Off + 3], R32 (T + Off + 4), R16 (T + Off + 8));
        break;
      case 3:
        DocAdd (Doc, ATTR_NORMAL, L"  NMI source        GSI %d  flags %04X", R32 (T + Off + 4), R16 (T + Off + 2));
        break;
      case 4:
        if (T[Off + 2] == 0xFF) {
          DocAdd (Doc, ATTR_NORMAL, L"  Local APIC NMI    UID all  LINT%d  flags %04X", T[Off + 5], R16 (T + Off + 3));
        } else {
          DocAdd (Doc, ATTR_NORMAL, L"  Local APIC NMI    UID %d  LINT%d  flags %04X", T[Off + 2], T[Off + 5], R16 (T + Off + 3));
        }
        break;
      case 5:
        DocAdd (Doc, ATTR_NORMAL, L"  LAPIC override    address %lX", R64 (T + Off + 4));
        break;
      case 9:
        Flags = R32 (T + Off + 8);
        Cpus++;
        Enabled += (Flags & 1) ? 1 : 0;
        DocAdd (Doc, (Flags & 1) ? ATTR_NORMAL : ATTR_DIM, L"  Local x2APIC      UID %3d  x2APIC ID %3d  %s", R32 (T + Off + 12), R32 (T + Off + 4), (Flags & 1) ? L"enabled" : ((Flags & 2) ? L"online capable" : L"disabled"));
        break;
      case 0x0A:
        DocAdd (Doc, ATTR_NORMAL, L"  x2APIC NMI        UID %d  LINT%d", R32 (T + Off + 4), T[Off + 8]);
        break;
      default:
        DocAdd (Doc, ATTR_DIM, L"  Structure type %02X, length %d", Type, L);
        break;
    }
  }

  DocAdd (Doc, ATTR_HEADER, L"  %d processor entr%s, %d enabled", Cpus, (Cpus == 1) ? L"y" : L"ies", Enabled);
}

STATIC
VOID
DecodeMcfg (
  IN TEXT_DOC     *Doc,
  IN CONST UINT8  *T,
  IN UINT32       Len
  )
{
  UINT32  Off;

  DocAdd (Doc, ATTR_HEADER, L"PCI Express Memory-mapped Configuration (ECAM)");
  for (Off = 44; Off + 16 <= Len; Off += 16) {
    DocAdd (Doc, ATTR_NORMAL, L"  Base %lX  segment %d  bus %02X-%02X", R64 (T + Off), R16 (T + Off + 8), T[Off + 10], T[Off + 11]);
  }
}

STATIC
VOID
DecodeHpet (
  IN TEXT_DOC     *Doc,
  IN CONST UINT8  *T,
  IN UINT32       Len
  )
{
  UINT32  Id;

  if (Len < 56) {
    return;
  }

  Id = R32 (T + 36);
  DocAdd (Doc, ATTR_HEADER, L"High Precision Event Timer");
  DocAdd (Doc, ATTR_NORMAL, L"  Block ID %08X: vendor %04X, %d comparators, %s, rev %d%s", Id, Id >> 16, ((Id >> 8) & 0x1F) + 1, (Id & BIT13) ? L"64-bit" : L"32-bit", Id & 0xFF, (Id & BIT15) ? L", legacy replacement" : L"");
  AddGas (Doc, L"Base address", T + 40);
  DocAdd (Doc, ATTR_NORMAL, L"  HPET number %d, min tick %d", T[52], R16 (T + 53));
}

STATIC
VOID
DecodeDmar (
  IN TEXT_DOC     *Doc,
  IN CONST UINT8  *T,
  IN UINT32       Len
  )
{
  UINT32  Off;
  UINT16  Type;
  UINT16  L;

  if (Len < 48) {
    return;
  }

  DocAdd (Doc, ATTR_HEADER, L"DMA Remapping (VT-d)");
  DocAdd (Doc, ATTR_NORMAL, L"  Host address width %d bits, flags %02X%s%s", T[36] + 1, T[37], (T[37] & 1) ? L" INTR_REMAP" : L"", (T[37] & 2) ? L" X2APIC_OPT_OUT" : L"");
  for (Off = 48; Off + 4 <= Len; Off += L) {
    Type = R16 (T + Off);
    L    = R16 (T + Off + 2);
    if ((L < 4) || (Off + L > Len)) {
      break;
    }

    switch (Type) {
      case 0:
        DocAdd (Doc, ATTR_NORMAL, L"  DRHD  segment %d  register base %lX%s", R16 (T + Off + 6), R64 (T + Off + 8), (T[Off + 4] & 1) ? L"  INCLUDE_PCI_ALL" : L"");
        break;
      case 1:
        DocAdd (Doc, ATTR_NORMAL, L"  RMRR  segment %d  %lX - %lX", R16 (T + Off + 6), R64 (T + Off + 8), R64 (T + Off + 16));
        break;
      case 2:
        DocAdd (Doc, ATTR_NORMAL, L"  ATSR  segment %d", R16 (T + Off + 6));
        break;
      case 3:
        DocAdd (Doc, ATTR_NORMAL, L"  RHSA  base %lX  proximity domain %d", R64 (T + Off + 8), R32 (T + Off + 16));
        break;
      case 4:
        DocAdd (Doc, ATTR_NORMAL, L"  ANDD  device %d", T[Off + 7]);
        break;
      default:
        DocAdd (Doc, ATTR_DIM, L"  Structure type %d, length %d", Type, L);
        break;
    }
  }
}

STATIC
VOID
AddTableBody (
  IN OUT TEXT_DOC  *Doc,
  IN ACPI_ENTRY    *E
  )
{
  CHAR16    Sig[5];
  CHAR16    Oem[7];
  CHAR16    OemTab[9];
  CHAR16    Creator[5];
  UINT8     *T;
  UINT32    Len;
  UINT32    Raw;

  T   = E->Ptr;
  Len = E->Length;
  AsciiField (Sig, T, 4);
  DocAdd (Doc, ATTR_HEADER, L"Header");
  DocAdd (Doc, ATTR_NORMAL, L"  Signature        %s", Sig);
  DocAdd (Doc, ATTR_NORMAL, L"  Address          %lX", (UINT64)(UINTN)T);
  DocAdd (Doc, ATTR_NORMAL, L"  Length           %d (%Xh)", Len, Len);
  if (E->HasHeader) {
    AsciiField (Oem, T + 10, 6);
    AsciiField (OemTab, T + 16, 8);
    AsciiField (Creator, T + 28, 4);
    DocAdd (Doc, ATTR_NORMAL, L"  Revision         %d", T[8]);
    DocAdd (Doc, (Sum8 (T, Len) == 0) ? ATTR_NORMAL : ATTR_WARN, L"  Checksum         %02X (%s)", T[9], (Sum8 (T, Len) == 0) ? L"OK" : L"BAD");
    DocAdd (Doc, ATTR_NORMAL, L"  OEM ID           %s", Oem);
    DocAdd (Doc, ATTR_NORMAL, L"  OEM Table ID     %s", OemTab);
    DocAdd (Doc, ATTR_NORMAL, L"  OEM Revision     %08X", R32 (T + 24));
    DocAdd (Doc, ATTR_NORMAL, L"  Creator ID       %s  revision %08X", Creator, R32 (T + 32));
  } else if (CompareMem (T, "FACS", 4) == 0) {
    DocAdd (Doc, ATTR_NORMAL, L"  HW signature     %08X", R32 (T + 8));
    DocAdd (Doc, ATTR_NORMAL, L"  Waking vector    %08X  X: %lX", R32 (T + 12), R64 (T + 24));
    DocAdd (Doc, ATTR_NORMAL, L"  Global lock      %08X   Flags %08X   Version %d", R32 (T + 16), R32 (T + 20), T[32]);
  }

  DocAdd (Doc, ATTR_NORMAL, L"");
  if (CompareMem (T, "FACP", 4) == 0) {
    DecodeFadt (Doc, T, Len);
  } else if (CompareMem (T, "APIC", 4) == 0) {
    DecodeMadt (Doc, T, Len);
  } else if (CompareMem (T, "MCFG", 4) == 0) {
    DecodeMcfg (Doc, T, Len);
  } else if (CompareMem (T, "HPET", 4) == 0) {
    DecodeHpet (Doc, T, Len);
  } else if (CompareMem (T, "DMAR", 4) == 0) {
    DecodeDmar (Doc, T, Len);
  } else if ((CompareMem (T, "DSDT", 4) == 0) || (CompareMem (T, "SSDT", 4) == 0)) {
    DocAdd (Doc, ATTR_DIM, L"AML definition block: raw bytes below (disassemble with iasl -d on a saved copy).");
  }

  DocAdd (Doc, ATTR_NORMAL, L"");
  Raw = (Len > RAW_MAX) ? RAW_MAX : Len;
  DocAdd (Doc, ATTR_HEADER, L"Raw data%s", (Raw < Len) ? L" (first 512 KB)" : L"");
  DocAddHex (Doc, T, Raw, 0);

}

STATIC
VOID
PrepareAml (
  VOID
  )
{
  UINT8   *Ptrs[MAX_TABLES];
  UINT32  Lens[MAX_TABLES];
  UINTN   Count;
  UINTN   Index;

  Count = 0;
  for (Index = 0; Index < mTableCount; Index++) {
    if ((CompareMem (mTables[Index].Ptr, "DSDT", 4) == 0) || (CompareMem (mTables[Index].Ptr, "SSDT", 4) == 0)) {
      Ptrs[Count] = mTables[Index].Ptr;
      Lens[Count] = mTables[Index].Length;
      Count++;
    }
  }

  AmlPrepare (Ptrs, Lens, Count);
  mAmlReady = TRUE;
}

STATIC
EFI_STATUS
SaveFile (
  IN CONST CHAR16  *Name,
  IN VOID          *Data,
  IN UINTN         Size
  )
{
  return FileSaveOnImageVolume (Name, Data, Size);
}

/**
  F2 in a DSDT/SSDT: save the raw table (.aml) and the ASL text (.dsl).
**/
STATIC
VOID
SaveAml (
  IN ACPI_ENTRY  *E,
  IN UINTN       Number
  )
{
  TEXT_DOC    Doc;
  CHAR8       *Text;
  UINTN       Size;
  UINTN       Pos;
  UINTN       Line;
  UINTN       Index;
  CHAR16      *L;
  CHAR16      Name[32];
  CHAR16      Name2[32];
  EFI_STATUS  Status;

  UnicodeSPrint (Name, sizeof (Name), L"%c%c%c%c%d.aml", E->Ptr[0], E->Ptr[1], E->Ptr[2], E->Ptr[3], Number);
  UnicodeSPrint (Name2, sizeof (Name2), L"%c%c%c%c%d.dsl", E->Ptr[0], E->Ptr[1], E->Ptr[2], E->Ptr[3], Number);
  Status = SaveFile (Name, E->Ptr, E->Length);
  if (EFI_ERROR (Status)) {
    UiStatus (ATTR_WARN, L"Save failed: %r (needs a writable FAT volume)", Status);
    return;
  }

  if (!mAmlReady) {
    PrepareAml ();
  }

  DocInit (&Doc);
  AmlDisassemble (&Doc, E->Ptr, E->Length);
  Size = 0;
  for (Line = 0; Line < Doc.Count; Line++) {
    Size += StrLen (Doc.Lines[Line].Text) + 2;
  }

  Text = AllocatePool (Size + 1);
  if (Text == NULL) {
    DocFree (&Doc);
    UiStatus (ATTR_WARN, L"Out of memory.");
    return;
  }

  Pos = 0;
  for (Line = 0; Line < Doc.Count; Line++) {
    L = Doc.Lines[Line].Text;
    for (Index = 0; L[Index] != 0; Index++) {
      Text[Pos++] = (L[Index] < 0x80) ? (CHAR8)L[Index] : '?';
    }

    Text[Pos++] = '\r';
    Text[Pos++] = '\n';
  }

  DocFree (&Doc);
  Status = SaveFile (Name2, Text, Pos);
  FreePool (Text);
  if (EFI_ERROR (Status)) {
    UiStatus (ATTR_WARN, L"Saved %s, %s failed: %r", Name, Name2, Status);
  } else {
    UiStatus (ATTR_CHANGED, L"Saved %s and %s to the root of this volume.", Name, Name2);
  }
}

STATIC
VOID
ShowTable (
  IN ACPI_ENTRY  *E
  )
{
  TEXT_DOC  Doc;
  CHAR16    Sig[5];
  CHAR16    Header[100];
  BOOLEAN        IsAml;
  BOOLEAN        Asl;
  EFI_INPUT_KEY  Key;

  AsciiField (Sig, E->Ptr, 4);
  IsAml = (BOOLEAN)((CompareMem (E->Ptr, "DSDT", 4) == 0) || (CompareMem (E->Ptr, "SSDT", 4) == 0));
  Asl   = FALSE;
  for ( ; ;) {
    DocInit (&Doc);
    if (Asl) {
      if (!mAmlReady) {
        UiStatus (ATTR_NORMAL, L"Disassembling AML...");
        UiFlush ();
        PrepareAml ();
      }

      AmlDisassemble (&Doc, E->Ptr, E->Length);
    } else {
      AddTableBody (&Doc, E);
    }

    UnicodeSPrint (
      Header,
      sizeof (Header),
      L"%s  at %lX  length %Xh%s",
      Sig,
      (UINT64)(UINTN)E->Ptr,
      E->Length,
      IsAml ? (Asl ? L"   [ASL source]  F7: decoded view" : L"   F7: ASL source") : L""
      );
    gDocF7Exit = IsAml;
    gDocFooter = IsAml ? L"F7:ASL/decoded  F2:save .aml+.dsl  /:find  F3:next  F1:help  Esc:back" : NULL;
    DocView (Asl ? L"ACPI Table - ASL" : L"ACPI Table", Header, &Doc);
    gDocF7Exit = FALSE;
    gDocFooter = NULL;
    DocFree (&Doc);
    if (gDocLastScan == SCAN_F2) {
      SaveAml (E, (UINTN)(E - mTables));
      UiReadKey (&Key);
      continue;
    }

    if (gDocLastScan != SCAN_F7) {
      break;
    }

    Asl = (BOOLEAN)!Asl;
  }
}

STATIC
VOID
BuildList (
  OUT TEXT_DOC  *Doc
  )
{
  UINTN       Index;
  ACPI_ENTRY  *E;
  CHAR16      Sig[5];
  CHAR16      Oem[7];
  CHAR16      OemTab[9];
  CHAR16      RsdpOem[7];
  BOOLEAN     Ok;

  DocInit (Doc);
  if (mRsdp == NULL) {
    DocAdd (Doc, ATTR_WARN, L"No ACPI tables published by the firmware.");
    return;
  }

  AsciiField (RsdpOem, mRsdp + 9, 6);
  DocAdd (
    Doc,
    ATTR_NORMAL,
    L" RSDP at %lX  revision %d  OEM %s  %s %lX",
    (UINT64)(UINTN)mRsdp,
    mRsdp[15],
    RsdpOem,
    ((mRsdp[15] >= 2) && (R64 (mRsdp + 24) != 0)) ? L"XSDT" : L"RSDT",
    ((mRsdp[15] >= 2) && (R64 (mRsdp + 24) != 0)) ? R64 (mRsdp + 24) : (UINT64)R32 (mRsdp + 16)
    );
  DocAdd (Doc, ATTR_NORMAL, L"");
  for (Index = 0; Index < mTableCount; Index++) {
    E = &mTables[Index];
    AsciiField (Sig, E->Ptr, 4);
    if (E->HasHeader) {
      AsciiField (Oem, E->Ptr + 10, 6);
      AsciiField (OemTab, E->Ptr + 16, 8);
      Ok = (BOOLEAN)(Sum8 (E->Ptr, E->Length) == 0);
    } else {
      StrCpyS (Oem, ARRAY_SIZE (Oem), L"      ");
      StrCpyS (OemTab, ARRAY_SIZE (OemTab), L"        ");
      Ok = TRUE;
    }

    DocAddItem (
      Doc,
      (INTN)Index,
      L" %s  %-6s  %-8s  rev %3d  %8X  %12lX  %s",
      Sig,
      Oem,
      OemTab,
      E->HasHeader ? E->Ptr[8] : E->Ptr[32],
      E->Length,
      (UINT64)(UINTN)E->Ptr,
      E->HasHeader ? (Ok ? L"OK" : L"BAD CHECKSUM") : L"-"
      );
  }
}

STATIC CONST CHAR16  *mAcpiHelp[] = {
  L"Up/Down       Select a table",
  L"Enter         Open the table: header, decoded fields, raw bytes",
  L"F7            In DSDT / SSDT: switch decoded view / ASL source",
  L"F2            In DSDT / SSDT: save SIGn.aml and SIGn.dsl to this volume",
  L"F5            Rescan"
};

VOID
AcpiViewRun (
  VOID
  )
{
  TEXT_DOC  Doc;
  UINTN     Sel;
  INTN      Pick;
  CHAR16    Header[80];

  ScanAcpi ();
  Sel = 0;
  for ( ; ;) {
    BuildList (&Doc);
    UnicodeSPrint (Header, sizeof (Header), L" Sig   OEM     Table ID  Rev      Length       Address  Checksum   (%d tables)", mTableCount);
    UiSetHelp (mAcpiHelp, ARRAY_SIZE (mAcpiHelp));
    Pick = DocSelect (L"ACPI Tables", L"Enter:open  F5:rescan  F1:help  Esc:back", Header, &Doc, &Sel);
    DocFree (&Doc);
    if (Pick == -1) {
      return;
    }

    if (Pick == -2) {
      ScanAcpi ();
      continue;
    }

    if ((Pick >= 0) && ((UINTN)Pick < mTableCount)) {
      ShowTable (&mTables[Pick]);
    }
  }
}

/**
  File > Dump All: table list and every table decoded, plus ASL for
  DSDT/SSDT.
**/
VOID
AcpiDump (
  IN OUT TEXT_DOC  *Out
  )
{
  TEXT_DOC  List;
  UINTN     Index;
  TEXT_DOC  *Saved;
  CHAR16    Sig[5];

  ScanAcpi ();
  BuildList (&List);
  DocAdd (Out, ATTR_NORMAL, L"==== ACPI Tables (%d) ====", mTableCount);
  for (Index = 0; Index < List.Count; Index++) {
    DocAdd (Out, ATTR_NORMAL, L"%s", List.Lines[Index].Text);
  }

  DocFree (&List);
  DocAdd (Out, ATTR_NORMAL, L"");
  Saved       = gDocCapture;
  gDocCapture = Out;
  for (Index = 0; Index < mTableCount; Index++) {
    ShowTable (&mTables[Index]);
  }

  gDocCapture = Saved;
  for (Index = 0; Index < mTableCount; Index++) {
    if ((CompareMem (mTables[Index].Ptr, "DSDT", 4) == 0) || (CompareMem (mTables[Index].Ptr, "SSDT", 4) == 0)) {
      if (!mAmlReady) {
        PrepareAml ();
      }

      AsciiField (Sig, mTables[Index].Ptr, 4);
      DocAdd (Out, ATTR_NORMAL, L"==== %s %d ASL ====", Sig, Index);
      AmlDisassemble (Out, mTables[Index].Ptr, mTables[Index].Length);
      DocAdd (Out, ATTR_NORMAL, L"");
    }
  }
}
