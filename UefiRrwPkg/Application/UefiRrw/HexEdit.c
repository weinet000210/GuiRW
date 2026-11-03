/** @file
  Generic 256-byte page hex editor (byte / word / dword views) used by the
  PCI, memory and IO views.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "UefiRrw.h"

#define PAGE_SIZE_BYTES  256
#define ROW_BYTES        16
#define FIRST_DATA_ROW   4
#define ADDR_COL         1

typedef struct {
  UINT8      Data[PAGE_SIZE_BYTES];
  BOOLEAN    Valid[PAGE_SIZE_BYTES];
  UINTN      Width;               // 1, 2 or 4
  UINTN      Cursor;              // byte offset in page, aligned to Width
  BOOLEAN    Editing;
  CHAR16     EditBuf[9];
  UINTN      EditLen;
} HEX_STATE;

STATIC
UINTN
DataColumn (
  IN HEX_EDIT_SPACE  *Space,
  IN UINTN           Width,
  IN UINTN           ByteInRow
  )
{
  UINTN  Unit;

  Unit = ByteInRow / Width;
  return ADDR_COL + Space->AddrDigits + 2 + Unit * (Width * 2 + 1);
}

STATIC
UINTN
AsciiColumn (
  IN HEX_EDIT_SPACE  *Space,
  IN UINTN           Width
  )
{
  return DataColumn (Space, Width, ROW_BYTES - Width) + Width * 2 + 3;
}

STATIC
VOID
ReadPage (
  IN     HEX_EDIT_SPACE  *Space,
  IN OUT HEX_STATE       *St
  )
{
  UINTN       Off;
  UINTN       Index;
  UINT64      Value;
  UINT64      Addr;
  EFI_STATUS  Status;

  for (Off = 0; Off < PAGE_SIZE_BYTES; Off += St->Width) {
    Addr   = Space->Base + Off;
    Status = EFI_DEVICE_ERROR;
    Value  = 0;
    if (Addr + St->Width - 1 <= Space->Limit) {
      Status = Space->Read (Space->Context, Addr, St->Width, &Value);
    }

    for (Index = 0; Index < St->Width; Index++) {
      St->Valid[Off + Index] = (BOOLEAN) !EFI_ERROR (Status);
      St->Data[Off + Index]  = (UINT8)RShiftU64 (Value, Index * 8);
    }
  }
}

STATIC
UINT64
UnitValue (
  IN HEX_STATE  *St,
  IN UINTN      Off
  )
{
  UINT64  Value;
  UINTN   Index;

  Value = 0;
  for (Index = St->Width; Index > 0; Index--) {
    Value = LShiftU64 (Value, 8) | St->Data[Off + Index - 1];
  }

  return Value;
}

STATIC
VOID
DrawCell (
  IN HEX_EDIT_SPACE  *Space,
  IN HEX_STATE       *St,
  IN UINTN           Off
  )
{
  UINTN  Row;
  UINTN  Col;
  UINTN  Attr;

  Row  = FIRST_DATA_ROW + Off / ROW_BYTES;
  Col  = DataColumn (Space, St->Width, Off % ROW_BYTES);
  Attr = (Off == St->Cursor) ? ATTR_HILITE : ATTR_NORMAL;

  if ((Off == St->Cursor) && St->Editing) {
    UiPrintAt (Col, Row, ATTR_EDIT, L"%-*s", St->Width * 2, St->EditBuf);
    return;
  }

  if (!St->Valid[Off]) {
    UiPrintAt (Col, Row, (Off == St->Cursor) ? ATTR_HILITE : ATTR_DIM, L"%.*s", St->Width * 2, L"????????");
    return;
  }

  switch (St->Width) {
    case 1:
      UiPrintAt (Col, Row, Attr, L"%02x", St->Data[Off]);
      break;
    case 2:
      UiPrintAt (Col, Row, Attr, L"%04lx", UnitValue (St, Off));
      break;
    default:
      UiPrintAt (Col, Row, Attr, L"%08lx", UnitValue (St, Off));
      break;
  }
}

STATIC
VOID
DrawPage (
  IN HEX_EDIT_SPACE  *Space,
  IN HEX_STATE       *St
  )
{
  UINTN   Row;
  UINTN   Off;
  UINTN   Index;
  CHAR16  Ascii[ROW_BYTES + 1];
  UINT8   Ch;
  CHAR16  *WidthName;

  WidthName = (St->Width == 1) ? L"Byte" : ((St->Width == 2) ? L"Word" : L"DWord");
  UiFillLine (1, ATTR_NORMAL);
  UiPrintAt (1, 1, ATTR_HEADER, L"%s", Space->Title);
  UiFillLine (2, ATTR_NORMAL);
  UiPrintAt (
    1,
    2,
    ATTR_NORMAL,
    L"Page %0*lx - %0*lx   View: %s%s",
    Space->AddrDigits,
    Space->Base,
    Space->AddrDigits,
    Space->Base + PAGE_SIZE_BYTES - 1,
    WidthName,
    (Space->Write == NULL) ? L"   [read-only]" : L""
    );

  //
  // Column header
  //
  UiFillLine (3, ATTR_NORMAL);
  for (Index = 0; Index < ROW_BYTES; Index += St->Width) {
    UiPrintAt (DataColumn (Space, St->Width, Index), 3, ATTR_HEADER, L"%02x", Index);
  }

  if (St->Width == 1) {
    UiPrintAt (AsciiColumn (Space, St->Width), 3, ATTR_HEADER, L"0123456789ABCDEF");
  }

  for (Row = 0; Row < PAGE_SIZE_BYTES / ROW_BYTES; Row++) {
    UiFillLine (FIRST_DATA_ROW + Row, ATTR_NORMAL);
    UiPrintAt (
      ADDR_COL,
      FIRST_DATA_ROW + Row,
      ATTR_HEADER,
      L"%0*lx",
      Space->AddrDigits,
      Space->Base + Row * ROW_BYTES
      );
    for (Index = 0; Index < ROW_BYTES; Index += St->Width) {
      DrawCell (Space, St, Row * ROW_BYTES + Index);
    }

    if (St->Width == 1) {
      for (Index = 0; Index < ROW_BYTES; Index++) {
        Off          = Row * ROW_BYTES + Index;
        Ch           = St->Data[Off];
        Ascii[Index] = (St->Valid[Off] && (Ch >= 0x20) && (Ch < 0x7F)) ? (CHAR16)Ch : L'.';
      }

      Ascii[ROW_BYTES] = L'\0';
      UiPrintAt (AsciiColumn (Space, St->Width), FIRST_DATA_ROW + Row, ATTR_NORMAL, L"%s", Ascii);
    }
  }
}

STATIC
VOID
DrawCursorInfo (
  IN HEX_EDIT_SPACE  *Space,
  IN HEX_STATE       *St
  )
{
  UINTN  Row;

  Row = FIRST_DATA_ROW + PAGE_SIZE_BYTES / ROW_BYTES + 1;
  UiFillLine (Row, ATTR_NORMAL);
  if (St->Valid[St->Cursor]) {
    UiPrintAt (
      1,
      Row,
      ATTR_NORMAL,
      L"Address %0*lx   Value %0*lx",
      Space->AddrDigits,
      Space->Base + St->Cursor,
      St->Width * 2,
      UnitValue (St, St->Cursor)
      );
  } else {
    UiPrintAt (1, Row, ATTR_NORMAL, L"Address %0*lx   Value <unreadable>", Space->AddrDigits, Space->Base + St->Cursor);
  }
}

STATIC
VOID
MoveCursor (
  IN OUT HEX_STATE  *St,
  IN     INTN       Delta
  )
{
  INTN  NewPos;

  NewPos = (INTN)St->Cursor + Delta;
  if ((NewPos >= 0) && (NewPos < PAGE_SIZE_BYTES)) {
    St->Cursor = (UINTN)NewPos;
  }
}

STATIC
VOID
CommitEdit (
  IN     HEX_EDIT_SPACE  *Space,
  IN OUT HEX_STATE       *St
  )
{
  UINT64      Value;
  UINT64      ReadBack;
  UINTN       Index;
  EFI_STATUS  Status;
  UINT64      Addr;
  CHAR16      Ch;

  St->Editing = FALSE;
  if (St->EditLen == 0) {
    return;
  }

  Value = 0;
  for (Index = 0; Index < St->EditLen; Index++) {
    Ch    = St->EditBuf[Index];
    Value = LShiftU64 (Value, 4) | (UINT64)((Ch <= L'9') ? (Ch - L'0') : (CharToUpper (Ch) - L'A' + 10));
  }

  Addr = Space->Base + St->Cursor;
  if (Space->ConfirmWrites) {
    CHAR16  Question[96];

    UnicodeSPrint (Question, sizeof (Question), L"Write %0*lx to %0*lx?", St->Width * 2, Value, Space->AddrDigits, Addr);
    if (!UiConfirm (Question)) {
      UiStatus (ATTR_NORMAL, L"Write cancelled.");
      return;
    }
  }

  Status = Space->Write (Space->Context, Addr, St->Width, Value);
  if (EFI_ERROR (Status)) {
    UiStatus (ATTR_WARN, L"Write failed: %r", Status);
    return;
  }

  ReadBack = 0;
  Status   = Space->Read (Space->Context, Addr, St->Width, &ReadBack);
  for (Index = 0; Index < St->Width; Index++) {
    St->Valid[St->Cursor + Index] = (BOOLEAN) !EFI_ERROR (Status);
    St->Data[St->Cursor + Index]  = (UINT8)RShiftU64 (ReadBack, Index * 8);
  }

  if (EFI_ERROR (Status)) {
    UiStatus (ATTR_WARN, L"Wrote %0*lx, read-back failed: %r", St->Width * 2, Value, Status);
  } else if (ReadBack != Value) {
    UiStatus (ATTR_CHANGED, L"Wrote %0*lx, read back %0*lx (register masked bits)", St->Width * 2, Value, St->Width * 2, ReadBack);
  } else {
    UiStatus (ATTR_NORMAL, L"Wrote %0*lx to %0*lx, verified.", St->Width * 2, Value, Space->AddrDigits, Addr);
  }
}

VOID
HexEditRun (
  IN OUT HEX_EDIT_SPACE  *Space
  )
{
  HEX_STATE      St;
  EFI_INPUT_KEY  Key;
  UINT64         Target;
  UINTN          OldCursor;
  BOOLEAN        Redraw;
  CHAR16         Ch;

  ZeroMem (&St, sizeof (St));
  St.Width = 1;

  St.Cursor = Space->StartOffset & (PAGE_SIZE_BYTES - 1);
  UiFrame (
    L"Hex Editor",
    Space->AllowGoto
    ? L"Arrows:move  PgUp/PgDn:page  G:goto  W:width  F5:refresh  0-F:edit  Esc:back"
    : L"Arrows:move  PgUp/PgDn:page  W:width  F5:refresh  0-F:edit  Esc:back"
    );
  ReadPage (Space, &St);
  Redraw = TRUE;

  for ( ; ;) {
    if (Redraw) {
      DrawPage (Space, &St);
      Redraw = FALSE;
    }

    DrawCursorInfo (Space, &St);
    UiReadKey (&Key);
    OldCursor = St.Cursor;
    Ch        = Key.UnicodeChar;

    //
    // In-place editing
    //
    if (St.Editing) {
      if (Key.ScanCode == SCAN_ESC) {
        St.Editing = FALSE;
      } else if (Ch == CHAR_BACKSPACE) {
        if (St.EditLen > 0) {
          St.EditBuf[--St.EditLen] = L'\0';
        }
      } else if (Ch == CHAR_CARRIAGE_RETURN) {
        CommitEdit (Space, &St);
      } else if (((Ch >= L'0') && (Ch <= L'9')) || ((CharToUpper (Ch) >= L'A') && (CharToUpper (Ch) <= L'F'))) {
        St.EditBuf[St.EditLen++] = Ch;
        St.EditBuf[St.EditLen]   = L'\0';
        if (St.EditLen == St.Width * 2) {
          CommitEdit (Space, &St);
          if (!St.Editing) {
            MoveCursor (&St, (INTN)St.Width);
          }
        }
      }

      DrawCell (Space, &St, OldCursor);
      if (St.Cursor != OldCursor) {
        DrawCell (Space, &St, St.Cursor);
      }

      if (St.Width == 1) {
        Redraw = TRUE;  // refresh ASCII column
      }

      continue;
    }

    switch (Key.ScanCode) {
      case SCAN_ESC:
        return;
      case SCAN_UP:
        MoveCursor (&St, -ROW_BYTES);
        break;
      case SCAN_DOWN:
        MoveCursor (&St, ROW_BYTES);
        break;
      case SCAN_LEFT:
        MoveCursor (&St, -(INTN)St.Width);
        break;
      case SCAN_RIGHT:
        MoveCursor (&St, (INTN)St.Width);
        break;
      case SCAN_HOME:
        St.Cursor = 0;
        break;
      case SCAN_END:
        St.Cursor = PAGE_SIZE_BYTES - St.Width;
        break;
      case SCAN_PAGE_UP:
        if (Space->Base >= PAGE_SIZE_BYTES) {
          Space->Base -= PAGE_SIZE_BYTES;
          ReadPage (Space, &St);
          Redraw = TRUE;
        }

        break;
      case SCAN_PAGE_DOWN:
        if (Space->Base + PAGE_SIZE_BYTES <= Space->Limit) {
          Space->Base += PAGE_SIZE_BYTES;
          ReadPage (Space, &St);
          Redraw = TRUE;
        }

        break;
      case SCAN_F5:
        ReadPage (Space, &St);
        Redraw = TRUE;
        UiStatus (ATTR_NORMAL, L"Refreshed.");
        break;
      default:
        break;
    }

    if ((Ch == L'w') || (Ch == L'W')) {
      St.Width  = (St.Width == 4) ? 1 : St.Width * 2;
      St.Cursor = St.Cursor & ~(St.Width - 1);
      ReadPage (Space, &St);
      Redraw = TRUE;
    } else if ((Ch == L'g') || (Ch == L'G')) {
      if (Space->AllowGoto) {
        Target = Space->Base + St.Cursor;
        if (UiPromptHex (L"Go to address", Space->AddrDigits, &Target)) {
          if (Target > Space->Limit) {
            UiStatus (ATTR_WARN, L"Address out of range (max %lx).", Space->Limit);
          } else {
            Space->Base = Target & ~(UINT64)(PAGE_SIZE_BYTES - 1);
            St.Cursor   = ((UINTN)Target & (PAGE_SIZE_BYTES - 1)) & ~(St.Width - 1);
            ReadPage (Space, &St);
            Redraw = TRUE;
          }
        }
      }
    } else if ((Ch == L'r') || (Ch == L'R')) {
      ReadPage (Space, &St);
      Redraw = TRUE;
    } else if (((Ch >= L'0') && (Ch <= L'9')) || ((CharToUpper (Ch) >= L'A') && (CharToUpper (Ch) <= L'F'))) {
      if (Space->Write == NULL) {
        UiStatus (ATTR_WARN, L"This space is read-only.");
      } else {
        St.Editing    = TRUE;
        St.EditBuf[0] = Ch;
        St.EditBuf[1] = L'\0';
        St.EditLen    = 1;
        DrawCell (Space, &St, St.Cursor);
      }

      continue;
    }

    if (!Redraw && (St.Cursor != OldCursor)) {
      DrawCell (Space, &St, OldCursor);
      DrawCell (Space, &St, St.Cursor);
    }
  }
}
