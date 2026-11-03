/** @file
  uefirrw - UEFI Resource Read & Write Tool, a text-mode hardware
  read/write utility for the UEFI Shell.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "UefiRrw.h"
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/FormBrowser2.h>
#include <Protocol/HiiDatabase.h>
#include <Guid/GlobalVariable.h>

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
  DocAdd (&Doc, ATTR_HEADER, L"   uefirrw - UEFI Resource Read & Write Tool");
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
    case UI_MOD_FILES:
      FileViewRun ();
      break;
    case UI_MOD_EDIT:
      EditViewRun ();
      break;
    default:
      break;
  }
}

/**
  File > Dump All to File: PCI, ACPI (with ASL), SMBIOS, USB and Super I/O
  into DUMPnnnn.TXT next to uefirrw.efi.
**/
STATIC
VOID
DumpAll (
  VOID
  )
{
  TEXT_DOC    Doc;
  UINT8       *Text;
  UINTN       Size;
  CHAR16      Name[32];
  CHAR16      Msg[96];
  CHAR16      Date[16];
  EFI_STATUS  Status;

  BuildDate (Date, sizeof (Date));
  DocInit (&Doc);
  DocAdd (&Doc, ATTR_NORMAL, L"uefirrw %s (built %s) full dump", UEFIRRW_VERSION, Date);
  DocAdd (&Doc, ATTR_NORMAL, L"");
  UiNotify (ATTR_HEADER, L"Dumping PCI...");
  PciDump (&Doc);
  UiNotify (ATTR_HEADER, L"Dumping ACPI...");
  AcpiDump (&Doc);
  UiNotify (ATTR_HEADER, L"Dumping SMBIOS...");
  SmbiosDump (&Doc);
  UiNotify (ATTR_HEADER, L"Dumping USB...");
  UsbDump (&Doc);
  SioDump (&Doc);
  Text = DocToAscii (&Doc, &Size);
  DocFree (&Doc);
  if (Text == NULL) {
    UiNotify (ATTR_WARN, L" Out of memory. ");
    return;
  }

  FileNextName (L"DUMP", L"TXT", Name, sizeof (Name));
  Status = FileSaveOnImageVolume (Name, Text, Size);
  FreePool (Text);
  if (EFI_ERROR (Status)) {
    UnicodeSPrint (Msg, sizeof (Msg), L" Could not save %s: %r ", Name, Status);
    UiNotify (ATTR_WARN, Msg);
  } else {
    UnicodeSPrint (Msg, sizeof (Msg), L"Saved %s\\%s (%d KB)", FileImageVolumeName (), Name, Size / 1024);
    UiNotify (ATTR_HEADER, Msg);
  }
}

STATIC
BOOLEAN
HasWord (
  IN CONST CHAR16  *Text,
  IN CONST CHAR16  *Word
  )
{
  return (BOOLEAN)(DocFindIn (Text, Word) >= 0);
}

/**
  Find the firmware's setup application among the Boot#### load options and
  start it. When the user leaves setup without resetting, it returns here.
**/
STATIC
BOOLEAN
StartSetupBootOption (
  VOID
  )
{
  CHAR16                    VarName[64];
  UINTN                     NameSize;
  EFI_GUID                  Guid;
  EFI_STATUS                Status;
  UINT8                     *Opt;
  UINTN                     OptSize;
  CHAR16                    *Desc;
  EFI_DEVICE_PATH_PROTOCOL  *Dp;
  EFI_HANDLE                Image;
  BOOLEAN                   Started;

  Started    = FALSE;
  VarName[0] = 0;
  for ( ; ;) {
    NameSize = sizeof (VarName);
    Status   = gRT->GetNextVariableName (&NameSize, VarName, &Guid);
    if (EFI_ERROR (Status)) {
      break;
    }

    if (!CompareGuid (&Guid, &gEfiGlobalVariableGuid) || (StrLen (VarName) != 8) || (StrnCmp (VarName, L"Boot", 4) != 0) ||
        (HexDigitValue (VarName[4]) < 0) || (HexDigitValue (VarName[5]) < 0) || (HexDigitValue (VarName[6]) < 0) || (HexDigitValue (VarName[7]) < 0))
    {
      continue;
    }

    Status = GetVariable2 (VarName, &gEfiGlobalVariableGuid, (VOID **)&Opt, &OptSize);
    if (EFI_ERROR (Status) || (Opt == NULL)) {
      continue;
    }

    Desc = (CHAR16 *)(Opt + 6);
    if ((OptSize > 8) && (HasWord (Desc, L"setup") || HasWord (Desc, L"uiapp")) && !HasWord (Desc, L"shell")) {
      Dp = (EFI_DEVICE_PATH_PROTOCOL *)((UINT8 *)Desc + StrSize (Desc));
      UiRestore ();
      Status = gBS->LoadImage (FALSE, gImageHandle, Dp, NULL, 0, &Image);
      if (!EFI_ERROR (Status)) {
        gBS->StartImage (Image, NULL, NULL);
        Started = TRUE;
      }

      UiInit ();
    }

    FreePool (Opt);
    if (Started) {
      break;
    }
  }

  return Started;
}

/**
  Fallback: show every HII form set through the firmware's form browser.
**/
STATIC
BOOLEAN
SendAllForms (
  VOID
  )
{
  EFI_FORM_BROWSER2_PROTOCOL  *Browser;
  EFI_HII_DATABASE_PROTOCOL   *Hii;
  EFI_HII_HANDLE              *Handles;
  UINTN                       Size;
  EFI_STATUS                  Status;

  if (EFI_ERROR (gBS->LocateProtocol (&gEfiFormBrowser2ProtocolGuid, NULL, (VOID **)&Browser)) ||
      EFI_ERROR (gBS->LocateProtocol (&gEfiHiiDatabaseProtocolGuid, NULL, (VOID **)&Hii)))
  {
    return FALSE;
  }

  Size   = 0;
  Status = Hii->ListPackageLists (Hii, EFI_HII_PACKAGE_FORMS, NULL, &Size, NULL);
  if ((Status != EFI_BUFFER_TOO_SMALL) || (Size == 0)) {
    return FALSE;
  }

  Handles = AllocatePool (Size);
  if (Handles == NULL) {
    return FALSE;
  }

  Status = Hii->ListPackageLists (Hii, EFI_HII_PACKAGE_FORMS, NULL, &Size, Handles);
  if (!EFI_ERROR (Status)) {
    UiRestore ();
    Status = Browser->SendForm (Browser, Handles, Size / sizeof (EFI_HII_HANDLE), NULL, 0, NULL, NULL);
    UiInit ();
  }

  FreePool (Handles);
  return (BOOLEAN) !EFI_ERROR (Status);
}

STATIC
VOID
ReturnToSetup (
  VOID
  )
{
  EFI_INPUT_KEY  Key;

  UiNotify (ATTR_HEADER, L"Looking for the setup application...");
  if (StartSetupBootOption () || SendAllForms ()) {
    return;
  }

  UiStatus (ATTR_WARN, L" No setup application found. Use File > Exit, then type exit in the shell. (any key) ");
  UiReadKey (&Key);
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
  // menu bar to move between modules. Exit is under File.
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

    if (Next == UI_ACT_DUMP) {
      DumpAll ();
      continue;
    }

    if (Next == UI_ACT_SETUP) {
      ReturnToSetup ();
      continue;
    }

    if ((Next == UI_ACT_WARM) || (Next == UI_ACT_COLD)) {
      UiRestore ();
      gRT->ResetSystem ((Next == UI_ACT_WARM) ? EfiResetWarm : EfiResetCold, EFI_SUCCESS, 0, NULL);
      continue;
    }

    Mod = Next;
  }

  UiRestore ();
  return EFI_SUCCESS;
}
