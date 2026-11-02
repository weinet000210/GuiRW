/** @file
  Line documents: a growable list of text lines with an attribute and an
  optional tag per line, plus two generic screens built on them:

  - DocView: read-only scrollable text (detail pages).
  - DocSelect: a list where selectable lines (tag >= 0) can be picked.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "GuiRw.h"

#define DOC_LINE_MAX  320

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
  UINTN          Top;
  UINTN          HScroll;
  UINTN          Visible;
  UINTN          Row;
  UINTN          MaxTop;
  EFI_INPUT_KEY  Key;

  Top     = 0;
  HScroll = 0;
  UiFrame (Title, L"Up/Down/PgUp/PgDn/Home/End: scroll   Left/Right: pan   Esc: back");
  for ( ; ;) {
    Visible = gScreenRows - 4;
    MaxTop  = (Doc->Count > Visible) ? Doc->Count - Visible : 0;
    if (Top > MaxTop) {
      Top = MaxTop;
    }

    UiFillLine (1, ATTR_NORMAL);
    if (Header != NULL) {
      UiPrintAt (1, 1, ATTR_HEADER, L"%s", Header);
    }

    for (Row = 0; Row < Visible; Row++) {
      if (Top + Row < Doc->Count) {
        DrawDocLine (2 + Row, &Doc->Lines[Top + Row], Doc->Lines[Top + Row].Attr, HScroll);
      } else {
        UiFillLine (2 + Row, ATTR_NORMAL);
      }
    }

    UiStatus (ATTR_DIM, L"Line %d-%d of %d", Doc->Count ? Top + 1 : 0, MIN (Top + Visible, Doc->Count), Doc->Count);
    UiReadKey (&Key);
    switch (Key.ScanCode) {
      case SCAN_UP:
        if (Top > 0) {
          Top--;
        }

        break;
      case SCAN_DOWN:
        if (Top < MaxTop) {
          Top++;
        }

        break;
      case SCAN_PAGE_UP:
        Top = (Top > Visible) ? Top - Visible : 0;
        break;
      case SCAN_PAGE_DOWN:
        Top = MIN (Top + Visible, MaxTop);
        break;
      case SCAN_HOME:
        Top     = 0;
        HScroll = 0;
        break;
      case SCAN_END:
        Top = MaxTop;
        break;
      case SCAN_LEFT:
        HScroll = (HScroll > 8) ? HScroll - 8 : 0;
        break;
      case SCAN_RIGHT:
        if (HScroll < DOC_LINE_MAX - 8) {
          HScroll += 8;
        }

        break;
      case SCAN_ESC:
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

  Top = 0;
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
    UiReadKey (&Key);
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
      case SCAN_ESC:
        *Sel = (UINTN)Cur;
        return -1;
      default:
        break;
    }
  }
}
