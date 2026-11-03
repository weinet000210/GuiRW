/** @file
  CMOS / RTC RAM editor: 00h-7Fh through ports 70h/71h, 80h-FFh (the upper
  bank on most chipsets) through 72h/73h. Every access is a byte access.

  Copyright (c) 2026, weinet. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "UefiRrw.h"

STATIC
UINT8
CmosRead8 (
  IN UINT8  Index
  )
{
  if (Index < 0x80) {
    IoWrite8 (0x70, Index);
    return IoRead8 (0x71);
  }

  IoWrite8 (0x72, Index);
  return IoRead8 (0x73);
}

STATIC
VOID
CmosWrite8 (
  IN UINT8  Index,
  IN UINT8  Value
  )
{
  if (Index < 0x80) {
    IoWrite8 (0x70, Index);
    IoWrite8 (0x71, Value);
  } else {
    IoWrite8 (0x72, Index);
    IoWrite8 (0x73, Value);
  }
}

STATIC
EFI_STATUS
CmosHexRead (
  IN  VOID    *Context,
  IN  UINT64  Address,
  IN  UINTN   Width,
  OUT UINT64  *Value
  )
{
  UINTN  Index;

  *Value = 0;
  for (Index = 0; Index < Width; Index++) {
    *Value |= LShiftU64 (CmosRead8 ((UINT8)(Address + Index)), 8 * Index);
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
CmosHexWrite (
  IN VOID    *Context,
  IN UINT64  Address,
  IN UINTN   Width,
  IN UINT64  Value
  )
{
  UINTN  Index;

  for (Index = 0; Index < Width; Index++) {
    CmosWrite8 ((UINT8)(Address + Index), (UINT8)RShiftU64 (Value, 8 * Index));
  }

  return EFI_SUCCESS;
}

VOID
CmosViewRun (
  VOID
  )
{
  HEX_EDIT_SPACE  Space;

  ZeroMem (&Space, sizeof (Space));
  Space.Title       = L"CMOS  (00-7F: 70h/71h   80-FF: 72h/73h)";
  Space.Read        = CmosHexRead;
  Space.Write       = CmosHexWrite;
  Space.Base        = 0;
  Space.Limit       = 0xFF;
  Space.AddrDigits  = 2;
  HexEditRun (&Space);
}
