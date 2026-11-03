/** @file
  File helpers: the volume uefirrw.efi was loaded from, whole-file read and
  write, free file names, shell mapping names, and text export of a doc.

  Copyright (c) 2026, weinet. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "UefiRrw.h"
#include <Protocol/LoadedImage.h>
#include <Protocol/Shell.h>
#include <Protocol/DevicePath.h>
#include <Library/DevicePathLib.h>

EFI_STATUS
FileOpenImageRoot (
  OUT EFI_FILE_PROTOCOL  **Root
  )
{
  EFI_STATUS                       Status;
  EFI_LOADED_IMAGE_PROTOCOL        *Image;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;

  Status = gBS->HandleProtocol (gImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Image);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->HandleProtocol (Image->DeviceHandle, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return Fs->OpenVolume (Fs, Root);
}

/**
  Replace Name in Dir with Size bytes of Data.
**/
EFI_STATUS
FileWriteAll (
  IN EFI_FILE_PROTOCOL  *Dir,
  IN CONST CHAR16       *Name,
  IN VOID               *Data,
  IN UINTN              Size
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *File;

  if (!EFI_ERROR (Dir->Open (Dir, &File, (CHAR16 *)Name, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0))) {
    File->Delete (File);
  }

  Status = Dir->Open (Dir, &File, (CHAR16 *)Name, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = File->Write (File, &Size, Data);
  if (!EFI_ERROR (Status)) {
    Status = File->Flush (File);
  }

  File->Close (File);
  return Status;
}

/**
  Read at most MaxSize bytes of Name. *Data is allocated (free with FreePool).
**/
EFI_STATUS
FileReadAll (
  IN  EFI_FILE_PROTOCOL  *Dir,
  IN  CONST CHAR16       *Name,
  IN  UINTN              MaxSize,
  OUT UINT8              **Data,
  OUT UINTN              *Size
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *File;
  UINT64             Len;

  *Data = NULL;
  *Size = 0;
  Status = Dir->Open (Dir, &File, (CHAR16 *)Name, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  File->SetPosition (File, 0xFFFFFFFFFFFFFFFFULL);
  File->GetPosition (File, &Len);
  File->SetPosition (File, 0);
  if (Len > MaxSize) {
    Len = MaxSize;
  }

  *Data = AllocatePool ((UINTN)Len + 2);
  if (*Data == NULL) {
    File->Close (File);
    return EFI_OUT_OF_RESOURCES;
  }

  *Size  = (UINTN)Len;
  Status = File->Read (File, Size, *Data);
  File->Close (File);
  if (EFI_ERROR (Status)) {
    FreePool (*Data);
    *Data = NULL;
    *Size = 0;
  }

  return Status;
}

EFI_STATUS
FileSaveOnImageVolume (
  IN CONST CHAR16  *Name,
  IN VOID          *Data,
  IN UINTN         Size
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *Root;

  Status = FileOpenImageRoot (&Root);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = FileWriteAll (Root, Name, Data, Size);
  Root->Close (Root);
  return Status;
}

/**
  First PrefixNNNN.Ext that does not exist yet on the image volume.
**/
VOID
FileNextName (
  IN  CONST CHAR16  *Prefix,
  IN  CONST CHAR16  *Ext,
  OUT CHAR16        *Name,
  IN  UINTN         NameSize
  )
{
  EFI_FILE_PROTOCOL  *Root;
  EFI_FILE_PROTOCOL  *File;
  UINTN              Index;

  UnicodeSPrint (Name, NameSize, L"%s0000.%s", Prefix, Ext);
  if (EFI_ERROR (FileOpenImageRoot (&Root))) {
    return;
  }

  for (Index = 0; Index < 10000; Index++) {
    UnicodeSPrint (Name, NameSize, L"%s%04d.%s", Prefix, Index, Ext);
    if (EFI_ERROR (Root->Open (Root, &File, Name, EFI_FILE_MODE_READ, 0))) {
      break;
    }

    File->Close (File);
  }

  Root->Close (Root);
}

/**
  Shell mapping name ("FS0:") of a file system handle, or NULL when not
  running under the UEFI Shell.
**/
CONST CHAR16 *
FileVolumeName (
  IN EFI_HANDLE  Handle
  )
{
  STATIC CHAR16             Buf[32];
  EFI_SHELL_PROTOCOL        *Shell;
  EFI_DEVICE_PATH_PROTOCOL  *Dp;
  CONST CHAR16              *Map;
  UINTN                     Index;

  if (EFI_ERROR (gBS->LocateProtocol (&gEfiShellProtocolGuid, NULL, (VOID **)&Shell))) {
    return NULL;
  }

  Dp = DevicePathFromHandle (Handle);
  if (Dp == NULL) {
    return NULL;
  }

  Map = Shell->GetMapFromDevicePath (&Dp);
  if (Map == NULL) {
    return NULL;
  }

  //
  // The list looks like "FS0:;BLK1:"; keep the first FSn: entry.
  //
  for (Index = 0; (Map[Index] != 0) && (Map[Index] != L';') && (Index < ARRAY_SIZE (Buf) - 1); Index++) {
    Buf[Index] = Map[Index];
  }

  Buf[Index] = 0;
  return Buf;
}

CONST CHAR16 *
FileImageVolumeName (
  VOID
  )
{
  EFI_LOADED_IMAGE_PROTOCOL  *Image;
  CONST CHAR16               *Name;

  if (EFI_ERROR (gBS->HandleProtocol (gImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Image))) {
    return L"";
  }

  Name = FileVolumeName (Image->DeviceHandle);
  return (Name != NULL) ? Name : L"";
}

/**
  The doc as plain ASCII text with CR LF line ends (non-ASCII becomes '?').
**/
UINT8 *
DocToAscii (
  IN  TEXT_DOC  *Doc,
  OUT UINTN     *Size
  )
{
  UINTN   Total;
  UINTN   Line;
  UINTN   Pos;
  UINT8   *Out;
  CHAR16  *T;

  Total = 0;
  for (Line = 0; Line < Doc->Count; Line++) {
    Total += StrLen (Doc->Lines[Line].Text) + 2;
  }

  Out = AllocatePool (Total + 1);
  if (Out == NULL) {
    *Size = 0;
    return NULL;
  }

  Pos = 0;
  for (Line = 0; Line < Doc->Count; Line++) {
    for (T = Doc->Lines[Line].Text; *T != 0; T++) {
      Out[Pos++] = (*T < 0x80) ? (UINT8)*T : '?';
    }

    Out[Pos++] = '\r';
    Out[Pos++] = '\n';
  }

  *Size = Pos;
  return Out;
}
