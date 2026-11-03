/** @file
  uefirrw - a text-mode, RW-Everything style hardware read/write utility
  for the UEFI Shell.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "UefiRrw.h"

#define UEFIRRW_AUTHOR  L"weinet"
#define UEFIRRW_MAIL    L"weinet0002@gmail.com"
#define UEFIRRW_URL     L"https://github.com/weinet000210/uefirrw/releases"

/**
  __DATE__ ("Nov  3 2026") as 2026-11-03.
**/
STATIC
VOID
BuildDate (
  OUT CHAR16  *Out,
  IN  UINTN   OutSize
  )
{
  STATIC CONST CHAR8  Months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
  CONST CHAR8         *D;
  UINTN               Month;
  UINTN               Day;

  D = __DATE__;
  for (Month = 0; Month < 12; Month++) {
    if ((D[0] == Months[Month * 3]) && (D[1] == Months[Month * 3 + 1]) && (D[2] == Months[Month * 3 + 2])) {
      break;
    }
  }

  Day = ((D[4] == ' ') ? 0 : (D[4] - '0') * 10) + (D[5] - '0');
  UnicodeSPrint (Out, OutSize, L"%c%c%c%c-%02d-%02d", D[7], D[8], D[9], D[10], Month + 1, Day);
}

STATIC
VOID
ShowAbout (
  VOID
  )
{
  TEXT_DOC  Doc;
  CHAR16    Date[16];
  CHAR16    Mode[64];

  BuildDate (Date, sizeof (Date));
  UiModeText (Mode, sizeof (Mode));
  DocInit (&Doc);
  DocAdd (&Doc, ATTR_NORMAL, L"");
  DocAdd (&Doc, ATTR_HEADER, L"   uefirrw - RW-Everything style hardware utility for UEFI");
  DocAdd (&Doc, ATTR_NORMAL, L"");
  DocAdd (&Doc, ATTR_NORMAL, L"   Version      %s", UEFIRRW_VERSION);
  DocAdd (&Doc, ATTR_NORMAL, L"   Build date   %s", Date);
  DocAdd (&Doc, ATTR_NORMAL, L"   Author       %s", UEFIRRW_AUTHOR);
  DocAdd (&Doc, ATTR_NORMAL, L"   Mail         %s", UEFIRRW_MAIL);
  DocAdd (&Doc, ATTR_NORMAL, L"   Download     %s", UEFIRRW_URL);
  DocAdd (&Doc, ATTR_NORMAL, L"   Display      %s", Mode);
  DocAdd (&Doc, ATTR_NORMAL, L"");
  DocAdd (&Doc, ATTR_NORMAL, L"   Developed collaboratively with AI. Free to copy, use, modify and");
  DocAdd (&Doc, ATTR_NORMAL, L"   redistribute under the BSD-2-Clause-Patent license.");
  DocAdd (&Doc, ATTR_NORMAL, L"");
  DocAdd (&Doc, ATTR_WARN, L" WARNING: writes go straight to hardware with no confirmation. ");
  DocAdd (&Doc, ATTR_WARN, L" Use at your own risk; the authors accept no liability.        ");
  DocView (L"About", NULL, &Doc);
  DocFree (&Doc);
}

STATIC
VOID
RunModule (
  IN INTN  Mod
  )
{
  switch (Mod) {
    case UI_MOD_PCI:
      PciViewRun ();
      break;
    case UI_MOD_MEM:
      MemViewRun ();
      break;
    case UI_MOD_IO:
      IoViewRun ();
      break;
    case UI_MOD_MSR:
      MsrViewRun ();
      break;
    case UI_MOD_ACPI:
      AcpiViewRun ();
      break;
    case UI_MOD_SMBIOS:
      SmbiosViewRun ();
      break;
    case UI_MOD_USB:
      UsbViewRun ();
      break;
    case UI_MOD_COM:
      ComViewRun ();
      break;
    case UI_MOD_CMOS:
      CmosViewRun ();
      break;
    case UI_MOD_SIO:
      SioViewRun ();
      break;
    case UI_MOD_EC:
      EcViewRun ();
      break;
    default:
      break;
  }
}

EFI_STATUS
EFIAPI
UefiMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  INTN  Mod;
  INTN  Next;

  //
  // Disable the watchdog: an interactive tool may sit idle for a long time.
  //
  gBS->SetWatchdogTimer (0, 0, 0, NULL);
  UiInit ();

  //
  // Like RU: start on the PCI list; F10 (or Esc out of a screen) opens the
  // menu bar to move between modules. Exit is under Help.
  //
  Mod = UI_MOD_PCI;
  for ( ; ;) {
    gUiCurrent = Mod;
    RunModule (Mod);
    if (gUiJump >= 0) {
      Next    = gUiJump;
      gUiJump = -1;
    } else {
      Next = UiTopMenu ();
      if (Next < 0) {
        continue;            // menu closed: back into the same module
      }
    }

    if (Next == UI_MOD_EXIT) {
      break;
    }

    if (Next == UI_MOD_ABOUT) {
      ShowAbout ();
      continue;
    }

    Mod = Next;
  }

  UiRestore ();
  return EFI_SUCCESS;
}
