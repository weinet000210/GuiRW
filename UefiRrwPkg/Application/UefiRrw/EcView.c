/** @file
  Embedded Controller RAM editor through the ACPI EC interface
  (ACPI 6.5 section 12.3): read command 80h, write command 81h on the
  command/status port, address and data on the data port.

  Copyright (c) 2026, weinet. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "UefiRrw.h"

#define EC_OBF          0x01
#define EC_IBF          0x02
#define EC_TIMEOUT_US   20000

typedef struct {
  UINT16    Data;
  UINT16    Cmd;
} EC_PORTS;

STATIC
BOOLEAN
EcWait (
  IN EC_PORTS  *P,
  IN UINT8     Mask,
  IN UINT8     Want
  )
{
  UINTN  Us;

  for (Us = 0; Us < EC_TIMEOUT_US; Us += 10) {
    if ((IoRead8 (P->Cmd) & Mask) == Want) {
      return TRUE;
    }

    gBS->Stall (10);
  }

  return FALSE;
}

STATIC
EFI_STATUS
EcRead8 (
  IN  EC_PORTS  *P,
  IN  UINT8     Addr,
  OUT UINT8     *Value
  )
{
  //
  // Drop a stale byte the EC may have left in its output buffer.
  //
  if (IoRead8 (P->Cmd) & EC_OBF) {
    IoRead8 (P->Data);
  }

  if (!EcWait (P, EC_IBF, 0)) {
    return EFI_TIMEOUT;
  }

  IoWrite8 (P->Cmd, 0x80);
  if (!EcWait (P, EC_IBF, 0)) {
    return EFI_TIMEOUT;
  }

  IoWrite8 (P->Data, Addr);
  if (!EcWait (P, EC_OBF, EC_OBF)) {
    return EFI_TIMEOUT;
  }

  *Value = IoRead8 (P->Data);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EcWrite8 (
  IN EC_PORTS  *P,
  IN UINT8     Addr,
  IN UINT8     Value
  )
{
  if (!EcWait (P, EC_IBF, 0)) {
    return EFI_TIMEOUT;
  }

  IoWrite8 (P->Cmd, 0x81);
  if (!EcWait (P, EC_IBF, 0)) {
    return EFI_TIMEOUT;
  }

  IoWrite8 (P->Data, Addr);
  if (!EcWait (P, EC_IBF, 0)) {
    return EFI_TIMEOUT;
  }

  IoWrite8 (P->Data, Value);
  return EcWait (P, EC_IBF, 0) ? EFI_SUCCESS : EFI_TIMEOUT;
}

STATIC
EFI_STATUS
EcHexRead (
  IN  VOID    *Context,
  IN  UINT64  Address,
  IN  UINTN   Width,
  OUT UINT64  *Value
  )
{
  UINTN       Index;
  UINT8       Byte;
  EFI_STATUS  Status;

  *Value = 0;
  for (Index = 0; Index < Width; Index++) {
    Status = EcRead8 ((EC_PORTS *)Context, (UINT8)(Address + Index), &Byte);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    *Value |= LShiftU64 (Byte, 8 * Index);
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EcHexWrite (
  IN VOID    *Context,
  IN UINT64  Address,
  IN UINTN   Width,
  IN UINT64  Value
  )
{
  UINTN       Index;
  EFI_STATUS  Status;

  for (Index = 0; Index < Width; Index++) {
    Status = EcWrite8 ((EC_PORTS *)Context, (UINT8)(Address + Index), (UINT8)RShiftU64 (Value, 8 * Index));
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  return EFI_SUCCESS;
}

VOID
EcViewRun (
  VOID
  )
{
  STATIC UINT64   DataPort = 0x62;
  STATIC UINT64   CmdPort  = 0x66;
  HEX_EDIT_SPACE  Space;
  EC_PORTS        Ports;
  UINT8           Status;
  UINT8           Probe;
  CHAR16          Title[80];

  UiFrame (L"Embedded Controller", L"Enter: accept   Esc: back");
  UiPrintAt (2, 2, ATTR_NORMAL, L"ACPI EC interface: read 80h / write 81h on the command port, then address and data.");
  UiPrintAt (2, 3, ATTR_NORMAL, L"Most boards use data port 62h and command/status port 66h. Press Enter to keep them.");
  UiPrintAt (2, 5, ATTR_WARN, L" Writing EC RAM can change fan, battery and power behaviour at once. ");
  if (!UiPromptHex (L"EC data port", 4, &DataPort)) {
    return;
  }

  if (!UiPromptHex (L"EC command/status port", 4, &CmdPort)) {
    return;
  }

  Ports.Data = (UINT16)DataPort;
  Ports.Cmd  = (UINT16)CmdPort;
  Status     = IoRead8 (Ports.Cmd);
  if ((Status == 0xFF) || EFI_ERROR (EcRead8 (&Ports, 0, &Probe))) {
    UiPrintAt (2, 7, ATTR_WARN, L" No EC answered on %Xh/%Xh (status %02Xh). ", Ports.Data, Ports.Cmd, Status);
    UiPrintAt (2, 8, ATTR_NORMAL, L"Check the _CRS of the PNP0C09 device in the DSDT (ACPI, F7) for the real ports.");
    UiStatus (ATTR_DIM, L"Press any key.");
    {
      EFI_INPUT_KEY  Key;

      UiReadKey (&Key);
    }

    return;
  }

  UnicodeSPrint (Title, sizeof (Title), L"EC RAM  (data %Xh, cmd %Xh)", Ports.Data, Ports.Cmd);
  ZeroMem (&Space, sizeof (Space));
  Space.Title      = Title;
  Space.Context    = &Ports;
  Space.Read       = EcHexRead;
  Space.Write      = EcHexWrite;
  Space.Base       = 0;
  Space.Limit      = 0xFF;
  Space.AddrDigits = 2;
  HexEditRun (&Space);
}
