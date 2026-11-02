/** @file
  GuiRW - a text-mode, RW-Everything style hardware read/write utility
  for the UEFI Shell.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "GuiRw.h"

STATIC CONST CHAR16  *mMainItems[] = {
  L"1. PCI Devices / Config Space",
  L"2. Memory / MMIO",
  L"3. IO Space",
  L"4. MSR",
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
    if ((Choice < 0) || (Choice == 4)) {
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
      default:
        break;
    }
  }

  UiRestore ();
  return EFI_SUCCESS;
}
