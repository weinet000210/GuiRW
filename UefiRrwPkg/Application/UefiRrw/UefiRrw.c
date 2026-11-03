/** @file
  uefirrw - a text-mode, RW-Everything style hardware read/write utility
  for the UEFI Shell.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "UefiRrw.h"

STATIC CONST CHAR16  *mMainItems[] = {
  L"1. PCI Devices / Config Space",
  L"2. Memory / MMIO",
  L"3. IO Space",
  L"4. MSR",
  L"5. SMBIOS",
  L"6. USB Devices",
  L"7. COM Port Test",
  L"Q. Quit",
};

EFI_STATUS
EFIAPI
UefiMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  INTN   Choice;
  UINTN  Last;

  //
  // Disable the watchdog: an interactive tool may sit idle for a long time.
  //
  gBS->SetWatchdogTimer (0, 0, 0, NULL);
  UiInit ();

  Last = 0;
  for ( ; ;) {
    Choice = UiMenu (L"Main Menu", mMainItems, ARRAY_SIZE (mMainItems), Last);
    if ((Choice < 0) || (Choice == 7)) {
      break;
    }

    Last = (UINTN)Choice;
    switch (Choice) {
      case 0:
        PciViewRun ();
        break;
      case 1:
        MemViewRun ();
        break;
      case 2:
        IoViewRun ();
        break;
      case 3:
        MsrViewRun ();
        break;
      case 4:
        SmbiosViewRun ();
        break;
      case 5:
        UsbViewRun ();
        break;
      case 6:
        ComViewRun ();
        break;
      default:
        break;
    }
  }

  UiRestore ();
  return EFI_SUCCESS;
}
