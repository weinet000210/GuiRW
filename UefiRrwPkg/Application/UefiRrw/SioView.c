/** @file
  Super I/O: probe the LPC config ports 2Eh and 4Eh for Nuvoton/Winbond,
  Fintek, ITE, SMSC/Microchip and ASPEED BMC (AST2400/2500/2600) parts, list their logical devices, and
  edit any logical device's config registers.

  Config mode is entered before and left after every access, so the chip is
  never left unlocked while you look at the screen.

  Copyright (c) 2026, weinet. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "UefiRrw.h"

#define SIO_NUVOTON  1
#define SIO_FINTEK   2
#define SIO_ITE      3
#define SIO_SMSC     4
#define SIO_ASPEED   5
#define MAX_CHIPS    2
#define MAX_LDN      0x20

typedef struct {
  UINT16    Port;
  UINT8     Kind;
  UINT16    Id;
  UINT8     Rev;
} SIO_CHIP;

typedef struct {
  SIO_CHIP  *Chip;
  UINT8     Ldn;
} SIO_CTX;

STATIC SIO_CHIP  mChips[MAX_CHIPS];
STATIC UINTN     mChipCount;

STATIC CONST CHAR16  *mIteLdn[] = {
  NULL, L"Serial port 1", L"Serial port 2", L"Parallel port", L"Environment controller",
  L"KBC (keyboard)", L"KBC (mouse)", L"GPIO", NULL, NULL, L"CIR"
};

STATIC CONST CHAR16  *mNuvotonLdn[] = {
  L"FDC", L"Parallel port", L"UART A", L"UART B", NULL, L"KBC", L"CIR", L"GPIO 6-8",
  L"WDT1 / GPIO 0", L"GPIO 1-8", L"ACPI", L"Hardware monitor", L"Vendor", L"WDT / Vendor",
  L"CIR wake-up", L"GPIO push-pull / open-drain", NULL, NULL, NULL, NULL, L"Port 80 UART",
  NULL, L"Deep sleep"
};

STATIC CONST CHAR16  *mAspeedLdn[] = {
  NULL, NULL, L"SUART1", L"SUART2", L"System wake-up control", L"KBC", NULL, L"GPIO",
  NULL, NULL, NULL, L"SUART3", L"SUART4", L"iLPC2AHB", L"Mailbox"
};

STATIC
VOID
SioEnter (
  IN SIO_CHIP  *C
  )
{
  switch (C->Kind) {
    case SIO_ITE:
      IoWrite8 (C->Port, 0x87);
      IoWrite8 (C->Port, 0x01);
      IoWrite8 (C->Port, 0x55);
      IoWrite8 (C->Port, (C->Port == 0x2E) ? 0x55 : 0xAA);
      break;
    case SIO_SMSC:
      IoWrite8 (C->Port, 0x55);
      break;
    case SIO_ASPEED:
      IoWrite8 (C->Port, 0xA5);
      IoWrite8 (C->Port, 0xA5);
      break;
    default:
      IoWrite8 (C->Port, 0x87);
      IoWrite8 (C->Port, 0x87);
      break;
  }
}

STATIC
UINT8
SioReg (
  IN SIO_CHIP  *C,
  IN UINT8     Reg
  )
{
  IoWrite8 (C->Port, Reg);
  return IoRead8 (C->Port + 1);
}

STATIC
VOID
SioSetReg (
  IN SIO_CHIP  *C,
  IN UINT8     Reg,
  IN UINT8     Value
  )
{
  IoWrite8 (C->Port, Reg);
  IoWrite8 (C->Port + 1, Value);
}

STATIC
VOID
SioExit (
  IN SIO_CHIP  *C
  )
{
  if (C->Kind == SIO_ITE) {
    SioSetReg (C, 0x02, 0x02);
  } else {
    IoWrite8 (C->Port, 0xAA);
  }
}

STATIC
BOOLEAN
SioProbe (
  IN  UINT16    Port,
  OUT SIO_CHIP  *C
  )
{
  UINT16  Id;

  ZeroMem (C, sizeof (*C));
  C->Port = Port;

  //
  // ASPEED BMC Super I/O: A5h A5h unlocks, AAh locks. Lock first so a chip
  // the firmware left unlocked does not answer every sequence; an ITE part
  // left unlocked is recognised by its ID below instead.
  //
  IoWrite8 (Port, 0xAA);
  C->Kind = SIO_ASPEED;
  SioEnter (C);
  Id = (UINT16)((SioReg (C, 0x20) << 8) | SioReg (C, 0x21));
  if ((Id != 0xFFFF) && (Id != 0x0000) && ((Id >> 8) < 0x85 || (Id >> 8) > 0x88)) {
    SioSetReg (C, 0x07, 0x02);
    if (SioReg (C, 0x07) == 0x02) {
      C->Id  = Id;
      C->Rev = 0;
      SioExit (C);
      //
      // A Nuvoton/Winbond/Fintek part does not unlock on A5h; make sure
      // this is not one that was already unlocked.
      //
      if ((UINT16)((SioReg (C, 0x20) << 8) | SioReg (C, 0x21)) == 0xFFFF) {
        return TRUE;
      }
    }
  }

  SioExit (C);

  //
  // Nuvoton / Winbond / Fintek: 87h 87h.
  //
  C->Kind = SIO_NUVOTON;
  SioEnter (C);
  Id = (UINT16)((SioReg (C, 0x20) << 8) | SioReg (C, 0x21));
  if ((Id != 0xFFFF) && (Id != 0x0000)) {
    if (((SioReg (C, 0x23) << 8) | SioReg (C, 0x24)) == 0x1934) {
      C->Kind = SIO_FINTEK;
      C->Rev  = 0;
    } else {
      C->Rev = (UINT8)(Id & 0x0F);
    }

    C->Id = Id;
    SioExit (C);
    return TRUE;
  }

  SioExit (C);

  //
  // ITE: 87h 01h 55h 55h (2Eh) / 87h 01h 55h AAh (4Eh).
  //
  C->Kind = SIO_ITE;
  SioEnter (C);
  Id = (UINT16)((SioReg (C, 0x20) << 8) | SioReg (C, 0x21));
  if (((Id >> 8) == 0x85) || ((Id >> 8) == 0x86) || ((Id >> 8) == 0x87) || ((Id >> 8) == 0x88)) {
    C->Id  = Id;
    C->Rev = SioReg (C, 0x22) & 0x0F;
    SioExit (C);
    return TRUE;
  }

  SioExit (C);

  //
  // SMSC / Microchip: 55h, one-byte device ID at 20h.
  //
  C->Kind = SIO_SMSC;
  SioEnter (C);
  Id = SioReg (C, 0x20);
  if ((Id != 0xFF) && (Id != 0x00)) {
    C->Id  = Id;
    C->Rev = SioReg (C, 0x21);
    SioExit (C);
    return TRUE;
  }

  IoWrite8 (C->Port, 0xAA);
  return FALSE;
}

STATIC
CONST CHAR16 *
VendorName (
  IN SIO_CHIP  *C
  )
{
  switch (C->Kind) {
    case SIO_NUVOTON: return L"Nuvoton/Winbond";
    case SIO_FINTEK:  return L"Fintek";
    case SIO_ITE:     return L"ITE";
    case SIO_ASPEED:  return L"ASPEED BMC";
    default:          return L"SMSC/Microchip";
  }
}

STATIC
CONST CHAR16 *
LdnName (
  IN SIO_CHIP  *C,
  IN UINT8     Ldn
  )
{
  CONST CHAR16  *Name;

  Name = NULL;
  if ((C->Kind == SIO_ITE) && (Ldn < ARRAY_SIZE (mIteLdn))) {
    Name = mIteLdn[Ldn];
  } else if ((C->Kind == SIO_ASPEED) && (Ldn < ARRAY_SIZE (mAspeedLdn))) {
    Name = mAspeedLdn[Ldn];
  } else if ((C->Kind == SIO_NUVOTON) && (Ldn < ARRAY_SIZE (mNuvotonLdn))) {
    Name = mNuvotonLdn[Ldn];
  }

  return (Name != NULL) ? Name : L"";
}

STATIC
EFI_STATUS
SioHexRead (
  IN  VOID    *Context,
  IN  UINT64  Address,
  IN  UINTN   Width,
  OUT UINT64  *Value
  )
{
  SIO_CTX  *X;
  UINTN    Index;

  X = (SIO_CTX *)Context;
  SioEnter (X->Chip);
  SioSetReg (X->Chip, 0x07, X->Ldn);
  *Value = 0;
  for (Index = 0; Index < Width; Index++) {
    *Value |= LShiftU64 (SioReg (X->Chip, (UINT8)(Address + Index)), 8 * Index);
  }

  SioExit (X->Chip);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
SioHexWrite (
  IN VOID    *Context,
  IN UINT64  Address,
  IN UINTN   Width,
  IN UINT64  Value
  )
{
  SIO_CTX  *X;
  UINTN    Index;

  X = (SIO_CTX *)Context;
  SioEnter (X->Chip);
  SioSetReg (X->Chip, 0x07, X->Ldn);
  for (Index = 0; Index < Width; Index++) {
    SioSetReg (X->Chip, (UINT8)(Address + Index), (UINT8)RShiftU64 (Value, 8 * Index));
  }

  SioExit (X->Chip);
  return EFI_SUCCESS;
}

STATIC
VOID
BuildDoc (
  OUT TEXT_DOC  *Doc
  )
{
  UINTN     Index;
  UINT8     Ldn;
  SIO_CHIP  *C;
  UINT8     Act;
  UINT16    Base1;
  UINT16    Base2;
  UINT8     Irq;
  UINT8     Irq2;

  DocInit (Doc);
  mChipCount = 0;
  if (SioProbe (0x2E, &mChips[mChipCount])) {
    mChipCount++;
  }

  if (SioProbe (0x4E, &mChips[mChipCount])) {
    mChipCount++;
  }

  if (mChipCount == 0) {
    DocAdd (Doc, ATTR_WARN, L"No Super I/O answered on 2Eh/4Eh (Nuvoton/Winbond, Fintek, ITE, SMSC, ASPEED sequences).");
    DocAdd (Doc, ATTR_NORMAL, L"Many laptops and servers have none, or keep it behind the EC/BMC.");
    return;
  }

  for (Index = 0; Index < mChipCount; Index++) {
    C = &mChips[Index];
    if (Index > 0) {
      DocAdd (Doc, ATTR_NORMAL, L"");
    }

    if (C->Kind == SIO_ITE) {
      DocAddItemAttr (Doc, (INTN)(Index * 256 + 0xFF), ATTR_HEADER, L"%s IT%04X  rev %X  at %02Xh   (Enter: global registers)", VendorName (C), C->Id, C->Rev, C->Port);
    } else if (C->Kind == SIO_SMSC) {
      DocAddItemAttr (Doc, (INTN)(Index * 256 + 0xFF), ATTR_HEADER, L"%s  device ID %02Xh  rev %02Xh  at %02Xh   (Enter: global registers)", VendorName (C), C->Id, C->Rev, C->Port);
    } else {
      DocAddItemAttr (Doc, (INTN)(Index * 256 + 0xFF), ATTR_HEADER, L"%s  chip ID %04Xh  at %02Xh   (Enter: global registers)", VendorName (C), C->Id, C->Port);
    }

    DocAdd (Doc, ATTR_DIM, L"  LDN  Active  Base 60h  Base 62h  IRQ 70h  IRQ 72h  Function");
    SioEnter (C);
    for (Ldn = 0; Ldn < MAX_LDN; Ldn++) {
      SioSetReg (C, 0x07, Ldn);
      if (SioReg (C, 0x07) != Ldn) {
        continue;
      }

      Act   = SioReg (C, 0x30);
      Base1 = (UINT16)((SioReg (C, 0x60) << 8) | SioReg (C, 0x61));
      Base2 = (UINT16)((SioReg (C, 0x62) << 8) | SioReg (C, 0x63));
      Irq   = SioReg (C, 0x70);
      Irq2  = SioReg (C, 0x72);
      if ((Act == 0xFF) && (Base1 == 0xFFFF) && (Irq == 0xFF)) {
        continue;
      }

      DocAddItemAttr (
        Doc,
        (INTN)(Index * 256 + Ldn),
        (Act & 1) ? ATTR_NORMAL : ATTR_DIM,
        L"  %02Xh  %s     %04Xh     %04Xh     %3d      %3d     %s",
        Ldn,
        (Act & 1) ? L"yes" : L"no ",
        Base1,
        Base2,
        Irq & 0x0F,
        Irq2 & 0x0F,
        LdnName (C, Ldn)
        );
    }

    SioExit (C);
  }
}

VOID
SioViewRun (
  VOID
  )
{
  STATIC CONST CHAR16  *Help[] = {
    L"Up/Down       Choose a chip or logical device (LDN)",
    L"Enter         Edit that LDN's config registers (00h-FFh)",
    L"F5            Probe 2Eh/4Eh again",
    L"",
    L"Registers below 30h are global and shared by every LDN.",
    L"Config mode is entered and left around every access."
  };
  TEXT_DOC        Doc;
  UINTN           Sel;
  INTN            Pick;
  SIO_CTX         Ctx;
  HEX_EDIT_SPACE  Space;
  CHAR16          Title[96];

  Sel = 0;
  BuildDoc (&Doc);
  for ( ; ;) {
    UiSetHelp (Help, ARRAY_SIZE (Help));
    Pick = DocSelect (L"Super I/O", L"Enter:edit LDN  F5:rescan  F1:help  Esc:back", L"Chips on 2Eh/4Eh and their logical devices", &Doc, &Sel);
    if (Pick == -1) {
      break;
    }

    if (Pick == -2) {
      DocFree (&Doc);
      BuildDoc (&Doc);
      continue;
    }

    if ((Pick < 0) || ((UINTN)(Pick / 256) >= mChipCount)) {
      continue;
    }

    Ctx.Chip = &mChips[Pick / 256];
    Ctx.Ldn  = (UINT8)(((Pick % 256) == 0xFF) ? 0 : (Pick % 256));
    UnicodeSPrint (
      Title,
      sizeof (Title),
      L"Super I/O %Xh  LDN %02Xh  %s",
      Ctx.Chip->Port,
      Ctx.Ldn,
      ((Pick % 256) == 0xFF) ? L"(global registers)" : LdnName (Ctx.Chip, Ctx.Ldn)
      );
    ZeroMem (&Space, sizeof (Space));
    Space.Title      = Title;
    Space.Context    = &Ctx;
    Space.Read       = SioHexRead;
    Space.Write      = SioHexWrite;
    Space.Base       = 0;
    Space.Limit      = 0xFF;
    Space.AddrDigits = 2;
    HexEditRun (&Space);
    if (gUiJump >= 0) {
      break;
    }

    DocFree (&Doc);
    BuildDoc (&Doc);
  }

  DocFree (&Doc);
}

/**
  File > Dump All: Super I/O chips and logical devices.
**/
VOID
SioDump (
  IN OUT TEXT_DOC  *Out
  )
{
  TEXT_DOC  List;
  UINTN     Index;

  BuildDoc (&List);
  DocAdd (Out, ATTR_NORMAL, L"==== Super I/O ====");
  for (Index = 0; Index < List.Count; Index++) {
    DocAdd (Out, ATTR_NORMAL, L"%s", List.Lines[Index].Text);
  }

  DocFree (&List);
  DocAdd (Out, ATTR_NORMAL, L"");
}
