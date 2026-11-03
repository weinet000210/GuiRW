/** @file
  x86 IO port space hex editor (0x0000 - 0xFFFF).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "UefiRrw.h"

STATIC
EFI_STATUS
IoHexRead (
  IN  VOID    *Context,
  IN  UINT64  Address,
  IN  UINTN   Width,
  OUT UINT64  *Value
  )
{
  switch (Width) {
    case 1:
      *Value = IoRead8 ((UINTN)Address);
      break;
    case 2:
      *Value = IoRead16 ((UINTN)Address);
      break;
    default:
      *Value = IoRead32 ((UINTN)Address);
      break;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
IoHexWrite (
  IN VOID    *Context,
  IN UINT64  Address,
  IN UINTN   Width,
  IN UINT64  Value
  )
{
  switch (Width) {
    case 1:
      IoWrite8 ((UINTN)Address, (UINT8)Value);
      break;
    case 2:
      IoWrite16 ((UINTN)Address, (UINT16)Value);
      break;
    default:
      IoWrite32 ((UINTN)Address, (UINT32)Value);
      break;
  }

  return EFI_SUCCESS;
}

VOID
IoViewRun (
  VOID
  )
{
  STATIC UINT64   LastPort = 0;
  HEX_EDIT_SPACE  Space;
  UINT64          Port;

  UiFrame (L"IO Space", L"Enter a port. Esc: back");
  UiPrintAt (2, 2, ATTR_NORMAL, L"The page view reads all 256 ports of the page.");
  UiPrintAt (2, 3, ATTR_NORMAL, L"Some ports have read side effects (e.g. 60h keyboard data). Every write asks first.");
  UiPrintAt (2, 5, ATTR_HEADER, L"Examples: 70 (CMOS), 80 (POST code), CF8 (PCI config), 3F8 (COM1)");

  Port = LastPort;
  if (!UiPromptHex (L"Start port", 4, &Port)) {
    return;
  }

  if (Port > 0xFFFF) {
    Port = 0xFFFF;
  }

  LastPort = Port;

  ZeroMem (&Space, sizeof (Space));
  Space.Title         = L"IO Space";
  Space.Read          = IoHexRead;
  Space.Write         = IoHexWrite;
  Space.Base          = Port & ~(UINT64)0xFF;
  Space.Limit         = 0xFFFF;
  Space.StartOffset   = (UINTN)(Port & 0xFF);
  Space.AllowGoto     = TRUE;
  Space.ConfirmWrites = TRUE;
  Space.AddrDigits    = 4;
  HexEditRun (&Space);
}
