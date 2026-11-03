/** @file
  File browser: every FAT volume, its folders and files. Enter opens a
  folder, a text file in the editor, or any other file in a hex view.

  Copyright (c) 2026, weinet. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "UefiRrw.h"
#include <Guid/FileInfo.h>
#include <Guid/FileSystemInfo.h>
#include <Library/DevicePathLib.h>

#define MAX_ENTRIES   2048
#define HEX_MAX_FILE  (1024 * 1024)

typedef struct {
  CHAR16      *Name;
  UINT64      Size;
  BOOLEAN     IsDir;
  EFI_TIME    Time;
} FILE_ENTRY;

STATIC EFI_HANDLE  *mVols;
STATIC UINTN       mVolCount;
STATIC FILE_ENTRY  mEnt[MAX_ENTRIES];
STATIC UINTN       mEntCount;

STATIC
VOID
FreeEntries (
  VOID
  )
{
  UINTN  Index;

  for (Index = 0; Index < mEntCount; Index++) {
    FreePool (mEnt[Index].Name);
  }

  mEntCount = 0;
}

STATIC
INTN
NoCaseCmp (
  IN CONST CHAR16  *A,
  IN CONST CHAR16  *B
  )
{
  CHAR16  Ca;
  CHAR16  Cb;

  for ( ; ; A++, B++) {
    Ca = ((*A >= L'a') && (*A <= L'z')) ? (CHAR16)(*A - 32) : *A;
    Cb = ((*B >= L'a') && (*B <= L'z')) ? (CHAR16)(*B - 32) : *B;
    if ((Ca != Cb) || (Ca == 0)) {
      return (INTN)Ca - (INTN)Cb;
    }
  }
}

STATIC
INTN
CompareEntry (
  IN FILE_ENTRY  *A,
  IN FILE_ENTRY  *B
  )
{
  if (A->IsDir != B->IsDir) {
    return A->IsDir ? -1 : 1;
  }

  return NoCaseCmp (A->Name, B->Name);
}

/**
  Read Dir into mEnt[], folders first, then by name.
**/
STATIC
VOID
ReadDir (
  IN EFI_FILE_PROTOCOL  *Dir
  )
{
  UINT8          Buf[SIZE_OF_EFI_FILE_INFO + 512 * sizeof (CHAR16)];
  EFI_FILE_INFO  *Info;
  UINTN          Size;
  UINTN          I;
  UINTN          J;
  FILE_ENTRY     T;

  FreeEntries ();
  Dir->SetPosition (Dir, 0);
  Info = (EFI_FILE_INFO *)Buf;
  while (mEntCount < MAX_ENTRIES) {
    Size = sizeof (Buf);
    if (EFI_ERROR (Dir->Read (Dir, &Size, Info)) || (Size == 0)) {
      break;
    }

    if ((StrCmp (Info->FileName, L".") == 0) || (StrCmp (Info->FileName, L"..") == 0)) {
      continue;
    }

    mEnt[mEntCount].Name  = AllocateCopyPool (StrSize (Info->FileName), Info->FileName);
    mEnt[mEntCount].Size  = Info->FileSize;
    mEnt[mEntCount].IsDir = (BOOLEAN)((Info->Attribute & EFI_FILE_DIRECTORY) != 0);
    mEnt[mEntCount].Time  = Info->ModificationTime;
    if (mEnt[mEntCount].Name != NULL) {
      mEntCount++;
    }
  }

  for (I = 1; I < mEntCount; I++) {
    T = mEnt[I];
    for (J = I; (J > 0) && (CompareEntry (&mEnt[J - 1], &T) > 0); J--) {
      mEnt[J] = mEnt[J - 1];
    }

    mEnt[J] = T;
  }
}

STATIC
BOOLEAN
LooksLikeText (
  IN UINT8  *Data,
  IN UINTN  Size
  )
{
  UINTN  Index;
  UINTN  Bad;

  if ((Size >= 2) && (Data[0] == 0xFF) && (Data[1] == 0xFE)) {
    return TRUE;
  }

  Bad = 0;
  for (Index = 0; Index < MIN (Size, 8192); Index++) {
    if (Data[Index] == 0) {
      return FALSE;
    }

    if ((Data[Index] < 0x20) && (Data[Index] != '\r') && (Data[Index] != '\n') && (Data[Index] != '\t') && (Data[Index] != 0x0C) && (Data[Index] != 0x1A)) {
      Bad++;
    }
  }

  return (BOOLEAN)(Bad * 50 <= MIN (Size, 8192));
}

STATIC
VOID
HexViewFile (
  IN EFI_FILE_PROTOCOL  *Dir,
  IN CONST CHAR16       *Name,
  IN CONST CHAR16       *Shown
  )
{
  UINT8       *Data;
  UINTN       Size;
  EFI_STATUS  Status;
  TEXT_DOC    Doc;
  CHAR16      Header[200];
  CHAR16      Msg[96];

  Status = FileReadAll (Dir, Name, HEX_MAX_FILE, &Data, &Size);
  if (EFI_ERROR (Status)) {
    UnicodeSPrint (Msg, sizeof (Msg), L" Cannot read %s: %r ", Name, Status);
    UiNotify (ATTR_WARN, Msg);
    return;
  }

  DocInit (&Doc);
  DocAddHex (&Doc, Data, Size, 0);
  FreePool (Data);
  UnicodeSPrint (Header, sizeof (Header), L"%s   %d bytes%s", Shown, Size, (Size == HEX_MAX_FILE) ? L" (first 1 MB)" : L"");
  DocView (L"File (hex)", Header, &Doc);
  DocFree (&Doc);
}

STATIC
VOID
OpenFile (
  IN EFI_FILE_PROTOCOL  *Dir,
  IN CONST CHAR16       *Name,
  IN CONST CHAR16       *Shown,
  IN BOOLEAN            ForceHex
  )
{
  UINT8    *Data;
  UINTN    Size;
  BOOLEAN  Text;

  Text = FALSE;
  if (!ForceHex && !EFI_ERROR (FileReadAll (Dir, Name, 8192, &Data, &Size))) {
    Text = (BOOLEAN)((Size == 0) || LooksLikeText (Data, Size));
    FreePool (Data);
  }

  if (Text) {
    EditFile (Dir, Name, Shown);
  } else {
    HexViewFile (Dir, Name, Shown);
  }
}

/**
  Browse one volume. Returns when the user leaves its root with Esc.
**/
STATIC
VOID
BrowseVolume (
  IN EFI_HANDLE  Handle,
  IN CONST CHAR16 *VolName
  )
{
  STATIC CONST CHAR16  *Help[] = {
    L"Enter         Open the folder, or the file (text: editor, other: hex)",
    L"F4            Hex view of the highlighted file",
    L"F6            New text file in this folder",
    L"F5            Refresh",
    L"Esc           Parent folder / back to the volume list"
  };
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_FILE_PROTOCOL                *Root;
  EFI_FILE_PROTOCOL                *Dir;
  EFI_FILE_PROTOCOL                *Sub;
  CHAR16                           Path[512];
  CHAR16                           Shown[600];
  CHAR16                           Header[600];
  CHAR16                           NewName[96];
  TEXT_DOC                         Doc;
  UINTN                            Sel;
  UINTN                            Index;
  UINTN                            Len;
  INTN                             Pick;
  FILE_ENTRY                       *E;
  BOOLEAN                          Hex;

  if (EFI_ERROR (gBS->HandleProtocol (Handle, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs)) || EFI_ERROR (Fs->OpenVolume (Fs, &Root))) {
    UiNotify (ATTR_WARN, L" Cannot open this volume. ");
    return;
  }

  StrCpyS (Path, ARRAY_SIZE (Path), L"\\");
  Sel = 0;
  for ( ; ;) {
    if (EFI_ERROR (Root->Open (Root, &Dir, Path, EFI_FILE_MODE_READ, 0))) {
      StrCpyS (Path, ARRAY_SIZE (Path), L"\\");
      if (EFI_ERROR (Root->Open (Root, &Dir, Path, EFI_FILE_MODE_READ, 0))) {
        break;
      }
    }

    ReadDir (Dir);
    DocInit (&Doc);
    DocAdd (&Doc, ATTR_DIM, L"        Size  Modified          Name");
    for (Index = 0; Index < mEntCount; Index++) {
      E = &mEnt[Index];
      if (E->IsDir) {
        DocAddItemAttr (&Doc, (INTN)Index, ATTR_HEADER, L"       <DIR>  %04d-%02d-%02d %02d:%02d  %s\\", E->Time.Year, E->Time.Month, E->Time.Day, E->Time.Hour, E->Time.Minute, E->Name);
      } else {
        DocAddItem (&Doc, (INTN)Index, L" %11ld  %04d-%02d-%02d %02d:%02d  %s", E->Size, E->Time.Year, E->Time.Month, E->Time.Day, E->Time.Hour, E->Time.Minute, E->Name);
      }
    }

    if (mEntCount == 0) {
      DocAdd (&Doc, ATTR_DIM, L"  (empty)");
    }

    UnicodeSPrint (Header, sizeof (Header), L"%s%s   %d item(s)", VolName, Path, mEntCount);
    UiSetHelp (Help, ARRAY_SIZE (Help));
    Pick = DocSelect (L"File Browser", L"Enter:open  F4:hex  F6:new file  F5:refresh  F1:help  Esc:up", Header, &Doc, &Sel);
    DocFree (&Doc);

    if (Pick == -1) {
      Dir->Close (Dir);
      if (gUiJump >= 0) {
        break;
      }

      Len = StrLen (Path);
      if (Len <= 1) {
        break;
      }

      //
      // Up one level.
      //
      if (Path[Len - 1] == L'\\') {
        Path[--Len] = 0;
      }

      while ((Len > 0) && (Path[Len - 1] != L'\\')) {
        Path[--Len] = 0;
      }

      Sel = 0;
      continue;
    }

    if (Pick == -3) {
      NewName[0] = 0;
      if (UiPromptText (L"New file name", NewName, ARRAY_SIZE (NewName))) {
        UnicodeSPrint (Shown, sizeof (Shown), L"%s%s%s", VolName, Path, NewName);
        EditFile (Dir, NewName, Shown);
      }
    } else if (((Pick >= 0) || (Pick == -4)) && (mEntCount > 0)) {
      Hex = (BOOLEAN)(Pick == -4);
      if (Hex) {
        Pick = (Sel >= 1) ? (INTN)(Sel - 1) : -1;
      }

      if ((Pick >= 0) && ((UINTN)Pick < mEntCount)) {
        E = &mEnt[Pick];
        if (E->IsDir) {
          if (EFI_ERROR (Dir->Open (Dir, &Sub, E->Name, EFI_FILE_MODE_READ, 0))) {
            UiNotify (ATTR_WARN, L" Cannot open that folder. ");
          } else {
            Sub->Close (Sub);
            StrCatS (Path, ARRAY_SIZE (Path), E->Name);
            StrCatS (Path, ARRAY_SIZE (Path), L"\\");
            Sel = 0;
          }
        } else {
          UnicodeSPrint (Shown, sizeof (Shown), L"%s%s%s", VolName, Path, E->Name);
          OpenFile (Dir, E->Name, Shown, Hex);
        }
      }
    }

    Dir->Close (Dir);
    if (gUiJump >= 0) {
      break;
    }
  }

  FreeEntries ();
  Root->Close (Root);
}

VOID
FileViewRun (
  VOID
  )
{
  STATIC CONST CHAR16  *Help[] = {
    L"Enter         Open the volume",
    L"F5            Rescan volumes",
    L"Esc           Back"
  };
  TEXT_DOC                         Doc;
  UINTN                            Sel;
  INTN                             Pick;
  UINTN                            Index;
  CONST CHAR16                     *Map;
  CHAR16                           *DpText;
  CHAR16                           Names[64][16];
  CHAR16                           Label[40];
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_FILE_PROTOCOL                *Root;
  UINT8                            InfoBuf[SIZE_OF_EFI_FILE_SYSTEM_INFO + 64 * sizeof (CHAR16)];
  EFI_FILE_SYSTEM_INFO             *Info;
  UINTN                            Size;
  UINT64                           TotalMb;

  Sel = 0;
  for ( ; ;) {
    if (mVols != NULL) {
      FreePool (mVols);
      mVols = NULL;
    }

    mVolCount = 0;
    gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleFileSystemProtocolGuid, NULL, &mVolCount, &mVols);
    DocInit (&Doc);
    DocAdd (&Doc, ATTR_DIM, L" Volume  Label            Size      Device path");
    for (Index = 0; (Index < mVolCount) && (Index < ARRAY_SIZE (Names)); Index++) {
      Map = FileVolumeName (mVols[Index]);
      if (Map != NULL) {
        StrCpyS (Names[Index], ARRAY_SIZE (Names[Index]), Map);
      } else {
        UnicodeSPrint (Names[Index], sizeof (Names[Index]), L"VOL%d:", Index);
      }

      Label[0] = 0;
      TotalMb  = 0;
      if (!EFI_ERROR (gBS->HandleProtocol (mVols[Index], &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs)) && !EFI_ERROR (Fs->OpenVolume (Fs, &Root))) {
        Info = (EFI_FILE_SYSTEM_INFO *)InfoBuf;
        Size = sizeof (InfoBuf);
        if (!EFI_ERROR (Root->GetInfo (Root, &gEfiFileSystemInfoGuid, &Size, Info))) {
          StrnCpyS (Label, ARRAY_SIZE (Label), Info->VolumeLabel, ARRAY_SIZE (Label) - 1);
          TotalMb = RShiftU64 (Info->VolumeSize, 20);
        }

        Root->Close (Root);
      }

      DpText = ConvertDevicePathToText (DevicePathFromHandle (mVols[Index]), TRUE, TRUE);
      DocAddItem (&Doc, (INTN)Index, L" %-7s %-16s %6ld MB  %s", Names[Index], Label, TotalMb, (DpText != NULL) ? DpText : L"");
      if (DpText != NULL) {
        FreePool (DpText);
      }
    }

    if (mVolCount == 0) {
      DocAdd (&Doc, ATTR_WARN, L" No file system found. ");
    }

    UiSetHelp (Help, ARRAY_SIZE (Help));
    Pick = DocSelect (L"File Browser", L"Enter:open  F5:rescan  F1:help  Esc:back", L"FAT volumes", &Doc, &Sel);
    DocFree (&Doc);
    if (Pick == -1) {
      break;
    }

    if ((Pick >= 0) && ((UINTN)Pick < mVolCount)) {
      BrowseVolume (mVols[Pick], Names[Pick]);
      if (gUiJump >= 0) {
        break;
      }
    }
  }
}
