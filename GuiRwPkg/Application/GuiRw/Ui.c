/** @file
  Text-mode screen helpers built on EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "GuiRw.h"

UINTN  gScreenCols = 80;
UINTN  gScreenRows = 25;

STATIC INT32    mSavedMode = -1;
STATIC UINTN    mSavedAttr;
STATIC BOOLEAN  mSavedCursor;

/**
  Pick the largest text mode (prefer >= 100 columns), hide the cursor.
**/
VOID
UiInit (
  VOID
  )
{
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *Out;
  INT32                            Mode;
  INT32                            BestMode;
  UINTN                            Cols;
  UINTN                            Rows;
  UINTN                            BestArea;

  Out          = gST->ConOut;
  mSavedMode   = Out->Mode->Mode;
  mSavedAttr   = (UINTN)Out->Mode->Attribute;
  mSavedCursor = Out->Mode->CursorVisible;

  BestMode = Out->Mode->Mode;
  BestArea = 0;
  for (Mode = 0; Mode < Out->Mode->MaxMode; Mode++) {
    if (EFI_ERROR (Out->QueryMode (Out, (UINTN)Mode, &Cols, &Rows))) {
      continue;
    }

    //
    // Keep it readable: cap at 160x50 so huge GOP modes don't give tiny text.
    //
    if ((Cols > 160) || (Rows > 50)) {
      continue;
    }

    if (Cols * Rows > BestArea) {
      BestArea = Cols * Rows;
      BestMode = Mode;
    }
  }

  if (BestMode != Out->Mode->Mode) {
    Out->SetMode (Out, (UINTN)BestMode);
  }

  if (EFI_ERROR (Out->QueryMode (Out, (UINTN)Out->Mode->Mode, &gScreenCols, &gScreenRows))) {
    gScreenCols = 80;
    gScreenRows = 25;
  }

  Out->EnableCursor (Out, FALSE);
  Out->SetAttribute (Out, ATTR_NORMAL);
  Out->ClearScreen (Out);
}

VOID
UiRestore (
  VOID
  )
{
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *Out;

  Out = gST->ConOut;
  Out->SetAttribute (Out, mSavedAttr);
  Out->ClearScreen (Out);
  if ((mSavedMode >= 0) && (mSavedMode != Out->Mode->Mode)) {
    Out->SetMode (Out, (UINTN)mSavedMode);
  }

  Out->EnableCursor (Out, mSavedCursor);
}

VOID
UiClear (
  VOID
  )
{
  gST->ConOut->SetAttribute (gST->ConOut, ATTR_NORMAL);
  gST->ConOut->ClearScreen (gST->ConOut);
}

/**
  Fill a whole row with spaces in the given attribute.
  The last cell of the last row is skipped to avoid scrolling the console.
**/
VOID
UiFillLine (
  IN UINTN  Row,
  IN UINTN  Attr
  )
{
  CHAR16  Line[256];
  UINTN   Len;
  UINTN   Index;

  Len = gScreenCols;
  if (Row == gScreenRows - 1) {
    Len--;
  }

  if (Len > ARRAY_SIZE (Line) - 1) {
    Len = ARRAY_SIZE (Line) - 1;
  }

  for (Index = 0; Index < Len; Index++) {
    Line[Index] = L' ';
  }

  Line[Len] = L'\0';
  gST->ConOut->SetAttribute (gST->ConOut, Attr);
  gST->ConOut->SetCursorPosition (gST->ConOut, 0, Row);
  gST->ConOut->OutputString (gST->ConOut, Line);
}

STATIC
VOID
UiPutClipped (
  IN UINTN   Col,
  IN UINTN   Row,
  IN UINTN   Attr,
  IN CHAR16  *Text
  )
{
  UINTN  Max;

  if ((Col >= gScreenCols) || (Row >= gScreenRows)) {
    return;
  }

  Max = gScreenCols - Col;
  if (Row == gScreenRows - 1) {
    Max--;
  }

  if (StrLen (Text) > Max) {
    Text[Max] = L'\0';
  }

  gST->ConOut->SetAttribute (gST->ConOut, Attr);
  gST->ConOut->SetCursorPosition (gST->ConOut, Col, Row);
  gST->ConOut->OutputString (gST->ConOut, Text);
}

VOID
EFIAPI
UiPrintAt (
  IN UINTN         Col,
  IN UINTN         Row,
  IN UINTN         Attr,
  IN CONST CHAR16  *Fmt,
  ...
  )
{
  VA_LIST  Args;
  CHAR16   Buf[512];

  VA_START (Args, Fmt);
  UnicodeVSPrint (Buf, sizeof (Buf), Fmt, Args);
  VA_END (Args);
  UiPutClipped (Col, Row, Attr, Buf);
}

/**
  Draw the title bar (row 0) and help bar (last row), clear the body.
**/
VOID
UiFrame (
  IN CONST CHAR16  *Title,
  IN CONST CHAR16  *Help
  )
{
  UINTN  Row;

  UiFillLine (0, ATTR_TITLE);
  UiPrintAt (1, 0, ATTR_TITLE, L"GuiRW %s  |  %s", GUIRW_VERSION, Title);
  for (Row = 1; Row < gScreenRows - 1; Row++) {
    UiFillLine (Row, ATTR_NORMAL);
  }

  UiFillLine (gScreenRows - 1, ATTR_TITLE);
  UiPrintAt (1, gScreenRows - 1, ATTR_TITLE, L"%s", Help);
}

/**
  Status line sits just above the help bar.
**/
VOID
EFIAPI
UiStatus (
  IN UINTN         Attr,
  IN CONST CHAR16  *Fmt,
  ...
  )
{
  VA_LIST  Args;
  CHAR16   Buf[512];

  VA_START (Args, Fmt);
  UnicodeVSPrint (Buf, sizeof (Buf), Fmt, Args);
  VA_END (Args);
  UiFillLine (gScreenRows - 2, ATTR_NORMAL);
  UiPutClipped (1, gScreenRows - 2, Attr, Buf);
}

VOID
UiReadKey (
  OUT EFI_INPUT_KEY  *Key
  )
{
  UINTN  Index;

  for ( ; ;) {
    gBS->WaitForEvent (1, &gST->ConIn->WaitForKey, &Index);
    if (!EFI_ERROR (gST->ConIn->ReadKeyStroke (gST->ConIn, Key))) {
      return;
    }
  }
}

STATIC
INTN
HexDigitValue (
  IN CHAR16  Ch
  )
{
  if ((Ch >= L'0') && (Ch <= L'9')) {
    return Ch - L'0';
  }

  if ((Ch >= L'a') && (Ch <= L'f')) {
    return Ch - L'a' + 10;
  }

  if ((Ch >= L'A') && (Ch <= L'F')) {
    return Ch - L'A' + 10;
  }

  return -1;
}

/**
  Prompt for a hex number on the status line.

  @retval TRUE   user pressed Enter (Value updated when digits were typed)
  @retval FALSE  user pressed Esc
**/
BOOLEAN
UiPromptHex (
  IN  CONST CHAR16  *Prompt,
  IN  UINTN         MaxDigits,
  IN OUT UINT64     *Value
  )
{
  CHAR16         Buf[17];
  UINTN          Len;
  EFI_INPUT_KEY  Key;
  UINT64         Result;
  UINTN          Index;

  if (MaxDigits > 16) {
    MaxDigits = 16;
  }

  Len    = 0;
  Buf[0] = L'\0';
  for ( ; ;) {
    UiStatus (ATTR_EDIT, L"%s (hex, Enter=OK, Esc=cancel): %s_", Prompt, Buf);
    UiReadKey (&Key);
    if (Key.ScanCode == SCAN_ESC) {
      UiStatus (ATTR_NORMAL, L"");
      return FALSE;
    }

    if (Key.UnicodeChar == CHAR_CARRIAGE_RETURN) {
      break;
    }

    if (Key.UnicodeChar == CHAR_BACKSPACE) {
      if (Len > 0) {
        Buf[--Len] = L'\0';
      }

      continue;
    }

    if ((HexDigitValue (Key.UnicodeChar) >= 0) && (Len < MaxDigits)) {
      Buf[Len++] = Key.UnicodeChar;
      Buf[Len]   = L'\0';
    }
  }

  UiStatus (ATTR_NORMAL, L"");
  if (Len == 0) {
    return TRUE;
  }

  Result = 0;
  for (Index = 0; Index < Len; Index++) {
    Result = LShiftU64 (Result, 4) | (UINT64)HexDigitValue (Buf[Index]);
  }

  *Value = Result;
  return TRUE;
}

BOOLEAN
UiConfirm (
  IN CONST CHAR16  *Question
  )
{
  EFI_INPUT_KEY  Key;

  UiStatus (ATTR_WARN, L"%s  [Y/N]", Question);
  for ( ; ;) {
    UiReadKey (&Key);
    if ((Key.UnicodeChar == L'y') || (Key.UnicodeChar == L'Y')) {
      UiStatus (ATTR_NORMAL, L"");
      return TRUE;
    }

    if ((Key.UnicodeChar == L'n') || (Key.UnicodeChar == L'N') || (Key.ScanCode == SCAN_ESC)) {
      UiStatus (ATTR_NORMAL, L"");
      return FALSE;
    }
  }
}

/**
  Vertical menu. Returns the chosen index, or -1 on Esc.
**/
INTN
UiMenu (
  IN CONST CHAR16  *Title,
  IN CONST CHAR16  **Items,
  IN UINTN         Count,
  IN UINTN         Initial
  )
{
  UINTN          Sel;
  UINTN          Index;
  UINTN          Top;
  UINTN          Left;
  EFI_INPUT_KEY  Key;

  Sel  = (Initial < Count) ? Initial : 0;
  Top  = 3;
  Left = 4;
  UiFrame (Title, L"Up/Down: select   Enter: open   Esc: back/exit");
  for ( ; ;) {
    for (Index = 0; Index < Count; Index++) {
      UiPrintAt (
        Left,
        Top + Index,
        (Index == Sel) ? ATTR_HILITE : ATTR_NORMAL,
        L" %-40s ",
        Items[Index]
        );
    }

    UiReadKey (&Key);
    switch (Key.ScanCode) {
      case SCAN_UP:
        Sel = (Sel == 0) ? Count - 1 : Sel - 1;
        break;
      case SCAN_DOWN:
        Sel = (Sel + 1) % Count;
        break;
      case SCAN_HOME:
        Sel = 0;
        break;
      case SCAN_END:
        Sel = Count - 1;
        break;
      case SCAN_ESC:
        return -1;
      default:
        break;
    }

    if (Key.UnicodeChar == CHAR_CARRIAGE_RETURN) {
      return (INTN)Sel;
    }

    //
    // Hotkey: first character of an item (digits/letters).
    //
    if (Key.UnicodeChar != CHAR_NULL) {
      for (Index = 0; Index < Count; Index++) {
        if (CharToUpper (Items[Index][0]) == CharToUpper (Key.UnicodeChar)) {
          return (INTN)Index;
        }
      }
    }
  }
}
