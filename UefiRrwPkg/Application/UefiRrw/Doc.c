/** @file
  Line documents: a growable list of text lines with an attribute and an
  optional tag per line, plus two generic screens built on them:

  - DocView: read-only scrollable text (detail pages).
  - DocSelect: a list where selectable lines (tag >= 0) can be picked.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "UefiRrw.h"

#define DOC_LINE_MAX  320

STATIC CHAR16  mFind[64];

/**
  Read a line of text on the status line. FALSE on Esc.
**/
STATIC
BOOLEAN
PromptText (
  IN     CONST CHAR16  *Prompt,
  IN OUT CHAR16        *Buf,
  IN     UINTN         Max
  )
{
  EFI_INPUT_KEY  Key;
  UINTN          Len;

  Len = StrLen (Buf);
  for ( ; ;) {
    UiStatus (ATTR_EDIT, L"%s: %s_   (Enter=find, Esc=cancel)", Prompt, Buf);
    UiReadKey (&Key);
    if (Key.ScanCode == SCAN_ESC) {
      return FALSE;
    }

    if (Key.UnicodeChar == CHAR_CARRIAGE_RETURN) {
      return (BOOLEAN)(Len > 0);
    }

    if (Key.UnicodeChar == CHAR_BACKSPACE) {
      if (Len > 0) {
        Buf[--Len] = L'\0';
      }
    } else if ((Key.UnicodeChar >= 0x20) && (Key.UnicodeChar < 0x7F) && (Len + 1 < Max)) {
      Buf[Len++] = Key.UnicodeChar;
      Buf[Len]   = L'\0';
    }
  }
}

STATIC
CHAR16
Lower (
  IN CHAR16  C
  )
{
  return ((C >= L'A') && (C <= L'Z')) ? (CHAR16)(C + 32) : C;
}

/**
  Case-insensitive search. Returns the column of the match or -1.
**/
STATIC
INTN
FindIn (
  IN CONST CHAR16  *Text,
  IN CONST CHAR16  *Pat
  )
{
  UINTN  Pos;
  UINTN  K;

  for (Pos = 0; Text[Pos] != 0; Pos++) {
    for (K = 0; (Pat[K] != 0) && (Text[Pos + K] != 0) && (Lower (Text[Pos + K]) == Lower (Pat[K])); K++) {
    }

    if (Pat[K] == 0) {
      return (INTN)Pos;
    }
  }

  return -1;
}

BOOLEAN       gDocF7Exit;     // DocView returns on F7 (gDocLastScan = SCAN_F7)
UINT16        gDocLastScan;
CONST CHAR16  *gDocFooter;    // DocView footer override, NULL = default

VOID
DocInit (
  OUT TEXT_DOC  *Doc
  )
{
  ZeroMem (Doc, sizeof (*Doc));
}

VOID
DocFree (
  IN OUT TEXT_DOC  *Doc
  )
{
  UINTN  Index;

  for (Index = 0; Index < Doc->Count; Index++) {
    FreePool (Doc->Lines[Index].Text);
  }

  if (Doc->Lines != NULL) {
    FreePool (Doc->Lines);
  }

  ZeroMem (Doc, sizeof (*Doc));
}

STATIC
VOID
DocAppend (
  IN OUT TEXT_DOC  *Doc,
  IN UINTN         Attr,
  IN INTN          Tag,
  IN CONST CHAR16  *Text
  )
{
  DOC_LINE  *Grown;
  UINTN     NewCap;

  if (Doc->Count == Doc->Capacity) {
    NewCap = (Doc->Capacity == 0) ? 64 : Doc->Capacity * 2;
    Grown  = ReallocatePool (Doc->Capacity * sizeof (DOC_LINE), NewCap * sizeof (DOC_LINE), Doc->Lines);
    if (Grown == NULL) {
      return;
    }

    Doc->Lines    = Grown;
    Doc->Capacity = NewCap;
  }

  Doc->Lines[Doc->Count].Text = AllocateCopyPool (StrSize (Text), Text);
  if (Doc->Lines[Doc->Count].Text == NULL) {
    return;
  }

  Doc->Lines[Doc->Count].Attr = (UINT8)Attr;
  Doc->Lines[Doc->Count].Tag  = Tag;
  Doc->Count++;
}

VOID
EFIAPI
DocAdd (
  IN OUT TEXT_DOC  *Doc,
  IN UINTN         Attr,
  IN CONST CHAR16  *Fmt,
  ...
  )
{
  VA_LIST  Args;
  CHAR16   Buf[DOC_LINE_MAX];

  VA_START (Args, Fmt);
  UnicodeVSPrint (Buf, sizeof (Buf), Fmt, Args);
  VA_END (Args);
  DocAppend (Doc, Attr, -1, Buf);
}

VOID
EFIAPI
DocAddItem (
  IN OUT TEXT_DOC  *Doc,
  IN INTN          Tag,
  IN CONST CHAR16  *Fmt,
  ...
  )
{
  VA_LIST  Args;
  CHAR16   Buf[DOC_LINE_MAX];

  VA_START (Args, Fmt);
  UnicodeVSPrint (Buf, sizeof (Buf), Fmt, Args);
  VA_END (Args);
  DocAppend (Doc, ATTR_NORMAL, Tag, Buf);
}

VOID
EFIAPI
DocAddItemAttr (
  IN OUT TEXT_DOC  *Doc,
  IN INTN          Tag,
  IN UINTN         Attr,
  IN CONST CHAR16  *Fmt,
  ...
  )
{
  VA_LIST  Args;
  CHAR16   Buf[DOC_LINE_MAX];

  VA_START (Args, Fmt);
  UnicodeVSPrint (Buf, sizeof (Buf), Fmt, Args);
  VA_END (Args);
  DocAppend (Doc, Attr, Tag, Buf);
}

//
// DocSelect options: a non-zero timeout makes it return -5 when no key
// arrives in time; KeepTop keeps the scroll position across calls.
//
UINTN    gDocTimeoutMs = 0;
BOOLEAN  gDocKeepTop   = FALSE;
STATIC UINTN  mLastTop;

/**
  Hex + ASCII dump, 16 bytes per line, offsets relative to Base.
**/
VOID
DocAddHex (
  IN OUT TEXT_DOC  *Doc,
  IN CONST UINT8   *Data,
  IN UINTN         Size,
  IN UINTN         Base
  )
{
  CHAR16  Line[100];
  UINTN   Offset;
  UINTN   Index;
  UINTN   Pos;
  UINT8   Byte;

  for (Offset = 0; Offset < Size; Offset += 16) {
    Pos = UnicodeSPrint (Line, sizeof (Line), L"  %04X: ", Base + Offset);
    for (Index = 0; Index < 16; Index++) {
      if (Offset + Index < Size) {
        Pos += UnicodeSPrint (Line + Pos, sizeof (Line) - Pos * sizeof (CHAR16), L"%02X ", Data[Offset + Index]);
      } else {
        Pos += UnicodeSPrint (Line + Pos, sizeof (Line) - Pos * sizeof (CHAR16), L"   ");
      }
    }

    Line[Pos++] = L' ';
    for (Index = 0; (Index < 16) && (Offset + Index < Size); Index++) {
      Byte        = Data[Offset + Index];
      Line[Pos++] = ((Byte >= 0x20) && (Byte < 0x7F)) ? (CHAR16)Byte : L'.';
    }

    Line[Pos] = L'\0';
    DocAppend (Doc, ATTR_NORMAL, -1, Line);
  }
}

STATIC
VOID
DrawDocLine (
  IN UINTN     Row,
  IN DOC_LINE  *Line,
  IN UINTN     Attr,
  IN UINTN     HScroll
  )
{
  UINTN  Len;

  UiFillLine (Row, Attr);
  Len = StrLen (Line->Text);
  if (HScroll < Len) {
    UiPrintAt (0, Row, Attr, L"%s", Line->Text + HScroll);
  }
}

/**
  Scrollable read-only viewer. Body occupies rows 2 .. rows-3; row 1 holds
  an optional fixed header line.
**/
VOID
DocView (
  IN CONST CHAR16  *Title,
  IN CONST CHAR16  *Header,
  IN TEXT_DOC      *Doc
  )
{
  STATIC CONST CHAR16  *Help[] = {
    L"Up/Down       Move the line cursor",
    L"PgUp/PgDn     Page up / down",
    L"Home/End      First / last line",
    L"Left/Right    Scroll the text sideways to read long lines",
    L"Enter         Show the current line in full (wrapped)",
    L"/ or Ctrl+F   Find text (case-insensitive)",
    L"F3 or n       Find next"
  };
  CONST CHAR16   *Msg;
  UINTN          Step;
  UINTN          Line;
  INTN           Col;
  UINTN          Top;
  UINTN          Cur;
  UINTN          HScroll;
  UINTN          Visible;
  UINTN          Row;
  UINTN          Len;
  UINTN          Pos;
  UINTN          Chunk;
  EFI_INPUT_KEY  Key;

  Top     = 0;
  Cur     = 0;
  HScroll = 0;
  Msg     = NULL;
  gDocLastScan = SCAN_NULL;
  UiFrame (Title, (gDocFooter != NULL) ? gDocFooter : L"Up/Down:line  Left/Right:scroll  Enter:full line  /:find  F3:next  F1:help  Esc:back");
  for ( ; ;) {
    Visible = gScreenRows - 4;
    if (Doc->Count == 0) {
      Cur = 0;
    } else if (Cur >= Doc->Count) {
      Cur = Doc->Count - 1;
    }

    if (Cur < Top) {
      Top = Cur;
    } else if (Cur >= Top + Visible) {
      Top = Cur - Visible + 1;
    }

    UiFillLine (1, ATTR_NORMAL);
    if (Header != NULL) {
      UiPrintAt (1, 1, ATTR_HEADER, L"%s", Header);
    }

    for (Row = 0; Row < Visible; Row++) {
      if (Top + Row < Doc->Count) {
        DrawDocLine (2 + Row, &Doc->Lines[Top + Row], (Top + Row == Cur) ? ATTR_HILITE : Doc->Lines[Top + Row].Attr, HScroll);
      } else {
        UiFillLine (2 + Row, ATTR_NORMAL);
      }
    }

    Len = (Doc->Count > 0) ? StrLen (Doc->Lines[Cur].Text) : 0;
    if (Msg != NULL) {
      UiStatus (ATTR_WARN, L"%s", Msg);
      Msg = NULL;
    } else
    UiStatus (
      ATTR_DIM,
      L"Line %d of %d   Col %d%s",
      Doc->Count ? Cur + 1 : 0,
      Doc->Count,
      HScroll + 1,
      (Len > HScroll + gScreenCols) ? L"   more ->  (Right or Enter)" : ((HScroll > 0) ? L"   <- (Left)" : L"")
      );
    UiSetHelp (Help, ARRAY_SIZE (Help));
    UiReadKey (&Key);
    if ((Key.UnicodeChar == L'/') || (Key.UnicodeChar == 0x06) || (Key.ScanCode == SCAN_F3) || (Key.UnicodeChar == L'n')) {
      if ((Key.UnicodeChar == L'/') || (Key.UnicodeChar == 0x06) || (mFind[0] == 0)) {
        if (!PromptText (L"Find", mFind, ARRAY_SIZE (mFind))) {
          continue;
        }
      }

      Col = -1;
      Line = Cur;
      for (Step = 1; Step <= Doc->Count; Step++) {
        Line = (Cur + Step) % Doc->Count;
        Col  = FindIn (Doc->Lines[Line].Text, mFind);
        if (Col >= 0) {
          break;
        }
      }

      if (Col < 0) {
        Msg = L"Not found.";
      } else {
        if ((Line <= Cur) && (Step > 0)) {
          Msg = L"Search wrapped to the top.";
        }

        Cur = Line;
        if (((UINTN)Col < HScroll) || ((UINTN)Col + StrLen (mFind) > HScroll + gScreenCols - 2)) {
          HScroll = ((UINTN)Col > 16) ? ((UINTN)Col - 16) & ~(UINTN)15 : 0;
        }

        if ((Cur < Top) || (Cur >= Top + Visible)) {
          Top = (Cur > Visible / 2) ? Cur - Visible / 2 : 0;
        }
      }

      continue;
    }

    if ((Key.UnicodeChar == CHAR_CARRIAGE_RETURN) && (Doc->Count > 0)) {
      //
      // Wrap the current line over the bottom of the screen.
      //
      Chunk = gScreenCols - 2;
      Row   = 2 + Visible - MIN ((Len + Chunk - 1) / Chunk, Visible);
      for (Pos = 0; (Pos < Len) && (Row < 2 + Visible); Pos += Chunk, Row++) {
        UiFillLine (Row, ATTR_EDIT);
        UiPrintN (1, Row, ATTR_EDIT, Doc->Lines[Cur].Text + Pos, Chunk);
      }

      UiStatus (ATTR_DIM, L"Full line shown. Press any key.");
      UiReadKey (&Key);
      continue;
    }

    switch (Key.ScanCode) {
      case SCAN_UP:
        if (Cur > 0) {
          Cur--;
        }

        break;
      case SCAN_DOWN:
        Cur++;
        break;
      case SCAN_PAGE_UP:
        Cur = (Cur > Visible) ? Cur - Visible : 0;
        Top = (Top > Visible) ? Top - Visible : 0;
        break;
      case SCAN_PAGE_DOWN:
        Cur += Visible;
        Top += Visible;
        if ((Doc->Count > Visible) && (Top > Doc->Count - Visible)) {
          Top = Doc->Count - Visible;
        }

        break;
      case SCAN_HOME:
        Cur     = 0;
        HScroll = 0;
        break;
      case SCAN_END:
        Cur = (Doc->Count > 0) ? Doc->Count - 1 : 0;
        break;
      case SCAN_LEFT:
        HScroll = (HScroll > 16) ? HScroll - 16 : 0;
        break;
      case SCAN_RIGHT:
        if (HScroll + 16 < DOC_LINE_MAX) {
          HScroll += 16;
        }

        break;
      case SCAN_F7:
      case SCAN_F2:
        if (gDocF7Exit) {
          gDocLastScan = Key.ScanCode;
          return;
        }

        break;
      case SCAN_ESC:
        gDocLastScan = SCAN_ESC;
        return;
      default:
        break;
    }
  }
}

STATIC
INTN
NextSelectable (
  IN TEXT_DOC  *Doc,
  IN INTN      From,
  IN INTN      Step
  )
{
  INTN  Index;

  for (Index = From; (Index >= 0) && (Index < (INTN)Doc->Count); Index += Step) {
    if (Doc->Lines[Index].Tag >= 0) {
      return Index;
    }
  }

  return -1;
}

/**
  List screen. Lines with Tag >= 0 are selectable.

  @param[in,out] Sel  line index of the selection (kept across calls)

  @retval >= 0  tag of the line picked with Enter
  @retval -1    Esc
  @retval -2    F5 (caller should rebuild the document)
  @retval -3    F6 (caller-defined, e.g. switch view)
  @retval -4    F4 (caller-defined; *Sel is the highlighted line)
**/
INTN
DocSelect (
  IN CONST CHAR16  *Title,
  IN CONST CHAR16  *Help,
  IN CONST CHAR16  *Header,
  IN TEXT_DOC      *Doc,
  IN OUT UINTN     *Sel
  )
{
  UINTN          Top;
  UINTN          Visible;
  UINTN          Row;
  INTN           Cur;
  INTN           Next;
  UINTN          Step;
  EFI_INPUT_KEY  Key;

  Cur = (INTN)*Sel;
  if ((Cur >= (INTN)Doc->Count) || (Cur < 0) || (Doc->Lines[Cur].Tag < 0)) {
    Cur = NextSelectable (Doc, 0, 1);
  }

  Top = gDocKeepTop ? mLastTop : 0;
  UiFrame (Title, Help);
  for ( ; ;) {
    Visible = gScreenRows - 4;
    if (Cur >= 0) {
      if ((UINTN)Cur < Top) {
        Top = (UINTN)Cur;
      } else if ((UINTN)Cur >= Top + Visible) {
        Top = (UINTN)Cur - Visible + 1;
      }
    }

    UiFillLine (1, ATTR_NORMAL);
    if (Header != NULL) {
      UiPrintAt (1, 1, ATTR_HEADER, L"%s", Header);
    }

    for (Row = 0; Row < Visible; Row++) {
      if (Top + Row < Doc->Count) {
        DrawDocLine (
          2 + Row,
          &Doc->Lines[Top + Row],
          ((INTN)(Top + Row) == Cur) ? ATTR_HILITE : Doc->Lines[Top + Row].Attr,
          0
          );
      } else {
        UiFillLine (2 + Row, ATTR_NORMAL);
      }
    }

    UiStatus (ATTR_DIM, L"");
    mLastTop = Top;
    if (!UiReadKeyTimeout (&Key, gDocTimeoutMs)) {
      *Sel = (Cur >= 0) ? (UINTN)Cur : 0;
      return -5;
    }

    if (Key.UnicodeChar == CHAR_CARRIAGE_RETURN) {
      if (Cur >= 0) {
        *Sel = (UINTN)Cur;
        return Doc->Lines[Cur].Tag;
      }

      continue;
    }

    if (Cur < 0) {
      if (Key.ScanCode == SCAN_ESC) {
        return -1;
      }

      if (Key.ScanCode == SCAN_F5) {
        return -2;
      }

      if (Key.ScanCode == SCAN_F6) {
        return -3;
      }

      if (Key.ScanCode == SCAN_F4) {
        return -4;
      }

      if (Key.ScanCode == SCAN_F3) {
        return -6;
      }

      continue;
    }

    switch (Key.ScanCode) {
      case SCAN_UP:
        Next = NextSelectable (Doc, Cur - 1, -1);
        if (Next >= 0) {
          Cur = Next;
        } else if (Top > 0) {
          Top--;
        }

        break;
      case SCAN_DOWN:
        Next = NextSelectable (Doc, Cur + 1, 1);
        if (Next >= 0) {
          Cur = Next;
        }

        break;
      case SCAN_PAGE_UP:
        for (Step = 0; Step < Visible; Step++) {
          Next = NextSelectable (Doc, Cur - 1, -1);
          if (Next < 0) {
            break;
          }

          Cur = Next;
        }

        break;
      case SCAN_PAGE_DOWN:
        for (Step = 0; Step < Visible; Step++) {
          Next = NextSelectable (Doc, Cur + 1, 1);
          if (Next < 0) {
            break;
          }

          Cur = Next;
        }

        break;
      case SCAN_HOME:
        Cur = NextSelectable (Doc, 0, 1);
        Top = 0;
        break;
      case SCAN_END:
        Cur = NextSelectable (Doc, (INTN)Doc->Count - 1, -1);
        break;
      case SCAN_F5:
        *Sel = (UINTN)Cur;
        return -2;
      case SCAN_F6:
        *Sel = (UINTN)Cur;
        return -3;
      case SCAN_F4:
        *Sel = (UINTN)Cur;
        return -4;
      case SCAN_F3:
        *Sel = (UINTN)Cur;
        return -6;
      case SCAN_ESC:
        *Sel = (UINTN)Cur;
        return -1;
      default:
        break;
    }
  }
}
