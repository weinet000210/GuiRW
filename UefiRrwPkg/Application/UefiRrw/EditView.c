/** @file
  Plain text editor for files on any FAT volume.

  ASCII / 8-bit files are kept byte for byte (each byte is one character),
  UTF-16 files (FF FE byte order mark) are kept as UTF-16, and the line end
  style (CR LF or LF) is preserved on save.

  Copyright (c) 2026, weinet. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "UefiRrw.h"

#define EDIT_MAX_FILE  (4 * 1024 * 1024)

typedef struct {
  CHAR16    *T;
  UINTN     Len;
  UINTN     Cap;
} ELINE;

STATIC ELINE    *mL;
STATIC UINTN    mCount;
STATIC UINTN    mCap;
STATIC BOOLEAN  mCrlf;
STATIC BOOLEAN  mUtf16;
STATIC BOOLEAN  mDirty;
STATIC CHAR16   mFind[64];

STATIC
BOOLEAN
LineReserve (
  IN OUT ELINE  *L,
  IN     UINTN  Need
  )
{
  CHAR16  *N;
  UINTN   Cap;

  if (Need + 1 <= L->Cap) {
    return TRUE;
  }

  Cap = MAX (Need + 1, L->Cap * 2);
  Cap = MAX (Cap, 16);
  N   = AllocatePool (Cap * sizeof (CHAR16));
  if (N == NULL) {
    return FALSE;
  }

  if (L->T != NULL) {
    CopyMem (N, L->T, L->Len * sizeof (CHAR16));
    FreePool (L->T);
  }

  N[L->Len] = 0;
  L->T      = N;
  L->Cap    = Cap;
  return TRUE;
}

/**
  Insert an empty line at At.
**/
STATIC
ELINE *
InsertLine (
  IN UINTN  At
  )
{
  ELINE  *N;

  if (mCount == mCap) {
    N = AllocateZeroPool (MAX (64, mCap * 2) * sizeof (ELINE));
    if (N == NULL) {
      return NULL;
    }

    if (mL != NULL) {
      CopyMem (N, mL, mCount * sizeof (ELINE));
      FreePool (mL);
    }

    mL   = N;
    mCap = MAX (64, mCap * 2);
  }

  CopyMem (&mL[At + 1], &mL[At], (mCount - At) * sizeof (ELINE));
  ZeroMem (&mL[At], sizeof (ELINE));
  mCount++;
  LineReserve (&mL[At], 0);
  return &mL[At];
}

STATIC
VOID
DeleteLine (
  IN UINTN  At
  )
{
  if (mL[At].T != NULL) {
    FreePool (mL[At].T);
  }

  CopyMem (&mL[At], &mL[At + 1], (mCount - At - 1) * sizeof (ELINE));
  mCount--;
}

STATIC
VOID
FreeAll (
  VOID
  )
{
  UINTN  Index;

  for (Index = 0; Index < mCount; Index++) {
    if (mL[Index].T != NULL) {
      FreePool (mL[Index].T);
    }
  }

  if (mL != NULL) {
    FreePool (mL);
  }

  mL     = NULL;
  mCount = 0;
  mCap   = 0;
}

STATIC
VOID
LoadText (
  IN UINT8  *Data,
  IN UINTN  Size
  )
{
  UINTN   Pos;
  UINTN   Count;
  CHAR16  Ch;
  ELINE   *L;

  mCrlf  = FALSE;
  mUtf16 = (BOOLEAN)((Size >= 2) && (Data[0] == 0xFF) && (Data[1] == 0xFE));
  L      = InsertLine (0);
  Pos    = mUtf16 ? 2 : 0;
  Count  = mUtf16 ? (Size & ~(UINTN)1) : Size;
  while ((Pos < Count) && (L != NULL)) {
    if (mUtf16) {
      Ch   = (CHAR16)(Data[Pos] | (Data[Pos + 1] << 8));
      Pos += 2;
    } else {
      Ch = Data[Pos++];
    }

    if (Ch == L'\n') {
      if ((L->Len > 0) && (L->T[L->Len - 1] == L'\r')) {
        L->T[--L->Len] = 0;
        mCrlf          = TRUE;
      }

      L = InsertLine (mCount);
      continue;
    }

    if (LineReserve (L, L->Len + 1)) {
      L->T[L->Len++] = Ch;
      L->T[L->Len]   = 0;
    }
  }
}

STATIC
EFI_STATUS
SaveText (
  IN EFI_FILE_PROTOCOL  *Dir,
  IN CONST CHAR16       *Name
  )
{
  UINTN       Total;
  UINTN       Index;
  UINTN       Pos;
  UINTN       K;
  UINT8       *Buf;
  UINTN       Unit;
  EFI_STATUS  Status;

  Unit  = mUtf16 ? 2 : 1;
  Total = 2;
  for (Index = 0; Index < mCount; Index++) {
    Total += (mL[Index].Len + 2) * Unit;
  }

  Buf = AllocatePool (Total);
  if (Buf == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Pos = 0;
  if (mUtf16) {
    Buf[Pos++] = 0xFF;
    Buf[Pos++] = 0xFE;
  }

  for (Index = 0; Index < mCount; Index++) {
    for (K = 0; K <= mL[Index].Len + 1; K++) {
      CHAR16  Ch;

      if (K < mL[Index].Len) {
        Ch = mL[Index].T[K];
      } else if (Index + 1 == mCount) {
        break;
      } else if (K == mL[Index].Len) {
        if (!mCrlf) {
          continue;
        }

        Ch = L'\r';
      } else {
        Ch = L'\n';
      }

      Buf[Pos++] = (UINT8)Ch;
      if (mUtf16) {
        Buf[Pos++] = (UINT8)(Ch >> 8);
      }
    }
  }

  Status = FileWriteAll (Dir, Name, Buf, Pos);
  FreePool (Buf);
  if (!EFI_ERROR (Status)) {
    mDirty = FALSE;
  }

  return Status;
}

/**
  Ask Y/N/Esc. Reads keys directly so a pending menu jump does not answer.
  @return 'y', 'n' or 0 for cancel
**/
STATIC
CHAR16
AskSave (
  VOID
  )
{
  EFI_INPUT_KEY  Key;
  UINTN          Index;

  for ( ; ;) {
    UiStatus (ATTR_EDIT, L"Save changes?  Y = save   N = discard   Esc = keep editing");
    UiFlush ();
    gBS->WaitForEvent (1, &gST->ConIn->WaitForKey, &Index);
    if (EFI_ERROR (gST->ConIn->ReadKeyStroke (gST->ConIn, &Key))) {
      continue;
    }

    if (Key.ScanCode == SCAN_ESC) {
      return 0;
    }

    if ((Key.UnicodeChar == L'y') || (Key.UnicodeChar == L'Y')) {
      return L'y';
    }

    if ((Key.UnicodeChar == L'n') || (Key.UnicodeChar == L'N')) {
      return L'n';
    }
  }
}

VOID
EditFile (
  IN EFI_FILE_PROTOCOL  *Dir,
  IN CONST CHAR16       *Name,
  IN CONST CHAR16       *Shown
  )
{
  STATIC CONST CHAR16  *Help[] = {
    L"Arrows        Move the cursor",
    L"Home/End      Start / end of line",
    L"PgUp/PgDn     Page up / down",
    L"Enter         Split the line",
    L"Backspace/Del Delete before / at the cursor",
    L"Ctrl+Y        Delete the whole line",
    L"F2 or Ctrl+S  Save",
    L"Ctrl+F        Find (case-insensitive), F3 find next",
    L"Esc           Close (asks to save when changed)"
  };
  UINT8          *Data;
  UINTN          Size;
  EFI_STATUS     Status;
  BOOLEAN        IsNew;
  UINTN          Cy;
  UINTN          Cx;
  UINTN          Top;
  UINTN          Left;
  UINTN          Visible;
  UINTN          Width;
  UINTN          Row;
  UINTN          Col;
  UINTN          Index;
  UINTN          Step;
  INTN           Found;
  INTN           Jump;
  ELINE          *L;
  ELINE          *N;
  CHAR16         Buf[260];
  CHAR16         Title[96];
  CONST CHAR16   *Msg;
  CHAR16         MsgBuf[96];
  CHAR16         Answer;
  EFI_INPUT_KEY  Key;

  FreeAll ();
  mDirty = FALSE;
  Status = FileReadAll (Dir, Name, EDIT_MAX_FILE, &Data, &Size);
  IsNew  = (BOOLEAN)(Status == EFI_NOT_FOUND);
  if (EFI_ERROR (Status) && !IsNew) {
    UnicodeSPrint (MsgBuf, sizeof (MsgBuf), L" Cannot open %s: %r ", Shown, Status);
    UiNotify (ATTR_WARN, MsgBuf);
    return;
  }

  if (IsNew) {
    mCrlf  = TRUE;
    mUtf16 = FALSE;
    InsertLine (0);
  } else {
    LoadText (Data, Size);
    FreePool (Data);
  }

  Cy   = 0;
  Cx   = 0;
  Top  = 0;
  Left = 0;
  Msg  = IsNew ? L"New file." : NULL;
  UnicodeSPrint (Title, sizeof (Title), L"Text Editor - %s", Shown);
  UiFrame (Title, L"F2:save  Ctrl+F:find  F3:next  Ctrl+Y:delete line  F1:help  Esc:close");
  for ( ; ;) {
    Visible = gScreenRows - 5;
    Width   = gScreenCols;
    if (Cy >= mCount) {
      Cy = mCount - 1;
    }

    L = &mL[Cy];
    if (Cx > L->Len) {
      Cx = L->Len;
    }

    if (Cy < Top) {
      Top = Cy;
    } else if (Cy >= Top + Visible) {
      Top = Cy - Visible + 1;
    }

    if (Cx < Left) {
      Left = (Cx > 8) ? Cx - 8 : 0;
    } else if (Cx >= Left + Width - 1) {
      Left = Cx - Width + 9;
    }

    UiFillLine (1, ATTR_NORMAL);
    UiPrintAt (
      1,
      1,
      ATTR_HEADER,
      L"%s%s   Ln %d/%d  Col %d   %s %s",
      Shown,
      mDirty ? L" *" : L"",
      Cy + 1,
      mCount,
      Cx + 1,
      mUtf16 ? L"UTF-16" : L"ASCII",
      mCrlf ? L"CRLF" : L"LF"
      );
    for (Row = 0; Row < Visible; Row++) {
      UiFillLine (2 + Row, ATTR_NORMAL);
      if (Top + Row >= mCount) {
        continue;
      }

      N = &mL[Top + Row];
      for (Col = 0; (Col < Width) && (Left + Col < N->Len); Col++) {
        Buf[Col] = N->T[Left + Col];
        if (Buf[Col] < 0x20) {
          Buf[Col] = L' ';
        }
      }

      Buf[Col] = 0;
      UiPrintN (0, 2 + Row, ATTR_NORMAL, Buf, Width);
      if (Top + Row == Cy) {
        Buf[0] = ((Cx < N->Len) && (N->T[Cx] >= 0x20)) ? N->T[Cx] : L' ';
        Buf[1] = 0;
        UiPut (Cx - Left, 2 + Row, ATTR_HILITE, Buf);
      }
    }

    if (Msg != NULL) {
      UiStatus (ATTR_WARN, L"%s", Msg);
      Msg = NULL;
    } else {
      UiStatus (ATTR_DIM, L"");
    }

    UiSetHelp (Help, ARRAY_SIZE (Help));
    UiReadKey (&Key);

    if (Key.ScanCode == SCAN_ESC) {
      if (mDirty) {
        Jump    = gUiJump;
        gUiJump = -1;
        Answer  = AskSave ();
        if (Answer == 0) {
          continue;               // stay; a pending menu pick is dropped
        }

        if (Answer == L'y') {
          Status = SaveText (Dir, Name);
          if (EFI_ERROR (Status)) {
            UnicodeSPrint (MsgBuf, sizeof (MsgBuf), L" Save failed: %r ", Status);
            Msg = MsgBuf;
            continue;
          }
        }

        gUiJump = Jump;
      }

      break;
    }

    if ((Key.ScanCode == SCAN_F2) || (Key.UnicodeChar == 0x13)) {
      Status = SaveText (Dir, Name);
      UnicodeSPrint (MsgBuf, sizeof (MsgBuf), EFI_ERROR (Status) ? L" Save failed: %r " : L"Saved.", Status);
      Msg = MsgBuf;
      continue;
    }

    if ((Key.UnicodeChar == 0x06) || (Key.ScanCode == SCAN_F3)) {
      if ((Key.UnicodeChar == 0x06) || (mFind[0] == 0)) {
        if (!UiPromptText (L"Find", mFind, ARRAY_SIZE (mFind))) {
          continue;
        }
      }

      Found = -1;
      for (Step = 0; Step <= mCount; Step++) {
        Index = (Cy + Step) % mCount;
        Found = DocFindIn (mL[Index].T + ((Step == 0) ? MIN (Cx + 1, mL[Index].Len) : 0), mFind);
        if (Found >= 0) {
          Cx = (UINTN)Found + ((Step == 0) ? MIN (Cx + 1, mL[Index].Len) : 0);
          Cy = Index;
          break;
        }
      }

      if (Found < 0) {
        Msg = L"Not found.";
      }

      continue;
    }

    switch (Key.ScanCode) {
      case SCAN_UP:
        if (Cy > 0) {
          Cy--;
        }

        continue;
      case SCAN_DOWN:
        if (Cy + 1 < mCount) {
          Cy++;
        }

        continue;
      case SCAN_LEFT:
        if (Cx > 0) {
          Cx--;
        } else if (Cy > 0) {
          Cy--;
          Cx = mL[Cy].Len;
        }

        continue;
      case SCAN_RIGHT:
        if (Cx < L->Len) {
          Cx++;
        } else if (Cy + 1 < mCount) {
          Cy++;
          Cx = 0;
        }

        continue;
      case SCAN_HOME:
        Cx = 0;
        continue;
      case SCAN_END:
        Cx = L->Len;
        continue;
      case SCAN_PAGE_UP:
        Cy  = (Cy > Visible) ? Cy - Visible : 0;
        Top = (Top > Visible) ? Top - Visible : 0;
        continue;
      case SCAN_PAGE_DOWN:
        Cy  = MIN (Cy + Visible, mCount - 1);
        Top = Top + Visible;
        if (Top + Visible > mCount) {
          Top = (mCount > Visible) ? mCount - Visible : 0;
        }

        continue;
      case SCAN_DELETE:
        if (Cx < L->Len) {
          CopyMem (&L->T[Cx], &L->T[Cx + 1], (L->Len - Cx) * sizeof (CHAR16));
          L->Len--;
          mDirty = TRUE;
        } else if (Cy + 1 < mCount) {
          N = &mL[Cy + 1];
          if (LineReserve (L, L->Len + N->Len)) {
            CopyMem (&L->T[L->Len], N->T, (N->Len + 1) * sizeof (CHAR16));
            L->Len += N->Len;
            DeleteLine (Cy + 1);
            mDirty = TRUE;
          }
        }

        continue;
      default:
        break;
    }

    if (Key.UnicodeChar == CHAR_BACKSPACE) {
      if (Cx > 0) {
        CopyMem (&L->T[Cx - 1], &L->T[Cx], (L->Len - Cx + 1) * sizeof (CHAR16));
        L->Len--;
        Cx--;
        mDirty = TRUE;
      } else if (Cy > 0) {
        N  = &mL[Cy - 1];
        Cx = N->Len;
        if (LineReserve (N, N->Len + L->Len)) {
          CopyMem (&N->T[N->Len], L->T, (L->Len + 1) * sizeof (CHAR16));
          N->Len += L->Len;
          DeleteLine (Cy);
          Cy--;
          mDirty = TRUE;
        }
      }

      continue;
    }

    if (Key.UnicodeChar == CHAR_CARRIAGE_RETURN) {
      N = InsertLine (Cy + 1);
      if (N == NULL) {
        continue;
      }

      L = &mL[Cy];               // InsertLine may move the array
      if (LineReserve (N, L->Len - Cx)) {
        CopyMem (N->T, &L->T[Cx], (L->Len - Cx + 1) * sizeof (CHAR16));
        N->Len   = L->Len - Cx;
        L->Len   = Cx;
        L->T[Cx] = 0;
      }

      Cy++;
      Cx     = 0;
      mDirty = TRUE;
      continue;
    }

    if (Key.UnicodeChar == 0x19) {
      if (mCount > 1) {
        DeleteLine (Cy);
      } else {
        L->Len  = 0;
        L->T[0] = 0;
      }

      Cx     = 0;
      mDirty = TRUE;
      continue;
    }

    if ((Key.UnicodeChar == CHAR_TAB) || ((Key.UnicodeChar >= 0x20) && (Key.UnicodeChar != 0x7F) && (mUtf16 || (Key.UnicodeChar < 0x100)))) {
      if (LineReserve (L, L->Len + 1)) {
        CopyMem (&L->T[Cx + 1], &L->T[Cx], (L->Len - Cx + 1) * sizeof (CHAR16));
        L->T[Cx] = Key.UnicodeChar;
        L->Len++;
        Cx++;
        mDirty = TRUE;
      }
    }
  }

  FreeAll ();
}

/**
  File > Text Editor: open (or create) a file on the volume uefirrw.efi was
  loaded from.
**/
VOID
EditViewRun (
  VOID
  )
{
  STATIC CHAR16      Name[128] = L"NOTES.TXT";
  EFI_FILE_PROTOCOL  *Root;
  CHAR16             Shown[160];

  UiFrame (L"Text Editor", L"Enter: open   Esc: back");
  UiPrintAt (2, 2, ATTR_NORMAL, L"Type a file name on %s (sub-folders with \\, e.g. EFI\\BOOT\\STARTUP.NSH).", FileImageVolumeName ());
  UiPrintAt (2, 3, ATTR_NORMAL, L"A file that does not exist yet is created when you save.");
  UiPrintAt (2, 4, ATTR_NORMAL, L"Files on other volumes: File > File Browser, then Enter on the file.");
  if (!UiPromptText (L"File name", Name, ARRAY_SIZE (Name))) {
    return;
  }

  if (EFI_ERROR (FileOpenImageRoot (&Root))) {
    UiNotify (ATTR_WARN, L" The volume uefirrw.efi was loaded from cannot be opened. ");
    return;
  }

  UnicodeSPrint (Shown, sizeof (Shown), L"%s\\%s", FileImageVolumeName (), Name);
  EditFile (Root, Name, Shown);
  Root->Close (Root);
}
