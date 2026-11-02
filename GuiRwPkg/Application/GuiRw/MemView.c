/** @file
  Physical memory / MMIO hex editor.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "GuiRw.h"

STATIC
EFI_STATUS
MemHexRead (
  IN  VOID    *Context,
  IN  UINT64  Address,
  IN  UINTN   Width,
  OUT UINT64  *Value
  )
{
  switch (Width) {
    case 1:
      *Value = MmioRead8 ((UINTN)Address);
      break;
    case 2:
      *Value = MmioRead16 ((UINTN)Address);
      break;
    default:
      *Value = MmioRead32 ((UINTN)Address);
      break;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
MemHexWrite (
  IN VOID    *Context,
  IN UINT64  Address,
  IN UINTN   Width,
  IN UINT64  Value
  )
{
  switch (Width) {
    case 1:
      MmioWrite8 ((UINTN)Address, (UINT8)Value);
      break;
    case 2:
      MmioWrite16 ((UINTN)Address, (UINT16)Value);
      break;
    default:
      MmioWrite32 ((UINTN)Address, (UINT32)Value);
      break;
  }

  return EFI_SUCCESS;
}

VOID
MemViewRun (
  VOID
  )
{
  STATIC UINT64   LastAddress = 0;
  HEX_EDIT_SPACE  Space;
  UINT64          Address;

  UiFrame (L"Memory", L"Enter a physical address. Esc: back");
  UiPrintAt (2, 2, ATTR_NORMAL, L"Reads/writes go straight to physical memory or MMIO (identity mapped).");
  UiPrintAt (2, 3, ATTR_NORMAL, L"Reading unmapped or side-effect MMIO can hang or reset the system.");
  UiPrintAt (2, 5, ATTR_HEADER, L"Examples: E0000000 (PCIe ECAM on many chipsets), FED00000 (HPET), FEE00000 (local APIC)");

  Address = LastAddress;
  if (!UiPromptHex (L"Start address", 16, &Address)) {
    return;
  }

  LastAddress = Address;

  ZeroMem (&Space, sizeof (Space));
  Space.Title         = L"Memory / MMIO";
  Space.Read          = MemHexRead;
  Space.Write         = MemHexWrite;
  Space.Base          = Address & ~(UINT64)0xFF;
  Space.Limit         = MAX_UINT64;
  Space.StartOffset   = (UINTN)(Address & 0xFF);
  Space.AllowGoto     = TRUE;
  Space.ConfirmWrites = FALSE;
  Space.AddrDigits    = 16;
  HexEditRun (&Space);
}
