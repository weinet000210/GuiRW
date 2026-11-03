/** @file
  Text-mode screen layer.

  All drawing goes into a shadow cell buffer (character + attribute per cell).
  UiFlush(), called before every key read, compares it with what is already
  on screen and sends only the changed spans:

  - GOP backend (preferred): uefirrw draws glyphs itself with the edk2 8x19
    font, integer-scaled so the grid fills the panel at its native resolution
    (no dependence on how the firmware console maps text modes, and nothing
    is mirrored to a serial redirection console).
  - ConOut backend (fallback when no GOP): changed spans are written with
    SimpleTextOut, grouped by attribute.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "UefiRrw.h"
#include <Protocol/GraphicsOutput.h>
#include <Uefi/UefiInternalFormRepresentation.h>

#define GLYPH_W      EFI_GLYPH_WIDTH
#define GLYPH_H      EFI_GLYPH_HEIGHT
#define MAX_COLS     255
#define MAX_ROWS     128
#define MIN_COLS     100
#define MIN_ROWS     25
#define ATTR_UNDRAWN 0xFF

extern EFI_NARROW_GLYPH  gUefiRrwGlyphs[];
extern UINTN             gUefiRrwGlyphCount;

typedef struct {
  CHAR16    Ch;
  UINT8     Attr;
} UI_CELL;

UINTN  gScreenCols = 80;
UINTN  gScreenRows = 25;

STATIC UI_CELL                        *mCells;      // what the views want
STATIC UI_CELL                        *mDrawn;      // what is on screen
STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL   *mGop;
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *mRowBuf;     // one text row of pixels
STATIC UINTN                          mScale = 1;
STATIC UINTN                          mOriginX;
STATIC UINTN                          mOriginY;
STATIC CONST UINT8                    *mGlyph[128];

STATIC INT32    mSavedMode = -1;
STATIC UINTN    mSavedAttr;
STATIC BOOLEAN  mSavedCursor;

//
// Standard VGA 16-color palette, indexed by EFI color value.
//
STATIC CONST EFI_GRAPHICS_OUTPUT_BLT_PIXEL  mPalette[16] = {
  //  Blue  Green Red  Reserved
  { 0x00, 0x00, 0x00, 0 }, // BLACK
  { 0x98, 0x00, 0x00, 0 }, // BLUE
  { 0x00, 0x98, 0x00, 0 }, // GREEN
  { 0x98, 0x98, 0x00, 0 }, // CYAN
  { 0x00, 0x00, 0x98, 0 }, // RED
  { 0x98, 0x00, 0x98, 0 }, // MAGENTA
  { 0x00, 0x98, 0x98, 0 }, // BROWN
  { 0xA8, 0xA8, 0xA8, 0 }, // LIGHTGRAY
  { 0x50, 0x50, 0x50, 0 }, // DARKGRAY
  { 0xFF, 0x50, 0x50, 0 }, // LIGHTBLUE
  { 0x50, 0xFF, 0x50, 0 }, // LIGHTGREEN
  { 0xFF, 0xFF, 0x50, 0 }, // LIGHTCYAN
  { 0x50, 0x50, 0xFF, 0 }, // LIGHTRED
  { 0xFF, 0x50, 0xFF, 0 }, // LIGHTMAGENTA
  { 0x50, 0xFF, 0xFF, 0 }, // YELLOW
  { 0xFF, 0xFF, 0xFF, 0 }, // WHITE
};

STATIC
VOID
BuildGlyphMap (
  VOID
  )
{
  UINTN  Index;

  for (Index = 0; Index < gUefiRrwGlyphCount; Index++) {
    if ((gUefiRrwGlyphs[Index].UnicodeWeight >= 0x20) && (gUefiRrwGlyphs[Index].UnicodeWeight < 0x7F)) {
      mGlyph[gUefiRrwGlyphs[Index].UnicodeWeight] = gUefiRrwGlyphs[Index].GlyphCol1;
    }
  }
}

/**
  Largest integer scale that still leaves at least MIN_COLS x MIN_ROWS cells.
**/
STATIC
UINTN
AutoScale (
  IN UINTN  HRes,
  IN UINTN  VRes
  )
{
  UINTN  Scale;

  for (Scale = 8; Scale > 1; Scale--) {
    if ((HRes / (GLYPH_W * Scale) >= MIN_COLS) && (VRes / (GLYPH_H * Scale) >= MIN_ROWS)) {
      return Scale;
    }
  }

  return 1;
}

STATIC
VOID
FillScreenPixels (
  IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Color
  )
{
  mGop->Blt (
          mGop,
          &Color,
          EfiBltVideoFill,
          0,
          0,
          0,
          0,
          mGop->Mode->Info->HorizontalResolution,
          mGop->Mode->Info->VerticalResolution,
          0
          );
}

/**
  (Re)compute the grid for the current GOP resolution and scale, and
  reallocate the cell buffers. Marks everything undrawn.
**/
STATIC
BOOLEAN
SetupGrid (
  VOID
  )
{
  UINTN  HRes;
  UINTN  VRes;
  UINTN  Count;
  UINTN  Index;

  if (mGop != NULL) {
    HRes        = mGop->Mode->Info->HorizontalResolution;
    VRes        = mGop->Mode->Info->VerticalResolution;
    gScreenCols = MIN (HRes / (GLYPH_W * mScale), MAX_COLS);
    gScreenRows = MIN (VRes / (GLYPH_H * mScale), MAX_ROWS);
    mOriginX    = (HRes - gScreenCols * GLYPH_W * mScale) / 2;
    mOriginY    = (VRes - gScreenRows * GLYPH_H * mScale) / 2;
    if (mRowBuf != NULL) {
      FreePool (mRowBuf);
    }

    mRowBuf = AllocatePool (gScreenCols * GLYPH_W * mScale * GLYPH_H * mScale * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL));
    if (mRowBuf == NULL) {
      return FALSE;
    }

    FillScreenPixels (mPalette[EFI_BLUE]);
  }

  if (mCells != NULL) {
    FreePool (mCells);
  }

  if (mDrawn != NULL) {
    FreePool (mDrawn);
  }

  Count  = gScreenCols * gScreenRows;
  mCells = AllocatePool (Count * sizeof (UI_CELL));
  mDrawn = AllocatePool (Count * sizeof (UI_CELL));
  if ((mCells == NULL) || (mDrawn == NULL)) {
    return FALSE;
  }

  for (Index = 0; Index < Count; Index++) {
    mCells[Index].Ch   = L' ';
    mCells[Index].Attr = (UINT8)ATTR_NORMAL;
    mDrawn[Index].Ch   = 0;
    mDrawn[Index].Attr = ATTR_UNDRAWN;
  }

  return TRUE;
}

STATIC
VOID
SetupConOutMode (
  VOID
  )
{
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *Out;
  INT32                            Mode;
  INT32                            BestMode;
  UINTN                            Cols;
  UINTN                            Rows;
  UINTN                            BestArea;

  Out      = gST->ConOut;
  BestMode = Out->Mode->Mode;
  BestArea = 0;
  for (Mode = 0; Mode < Out->Mode->MaxMode; Mode++) {
    if (EFI_ERROR (Out->QueryMode (Out, (UINTN)Mode, &Cols, &Rows))) {
      continue;
    }

    if ((Cols > MAX_COLS) || (Rows > MAX_ROWS)) {
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
}

VOID
UiInit (
  VOID
  )
{
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *Out;
  EFI_STATUS                       Status;

  Out          = gST->ConOut;
  mSavedMode   = Out->Mode->Mode;
  mSavedAttr   = (UINTN)Out->Mode->Attribute;
  mSavedCursor = Out->Mode->CursorVisible;
  Out->EnableCursor (Out, FALSE);
  Out->SetAttribute (Out, ATTR_NORMAL);
  Out->ClearScreen (Out);

  BuildGlyphMap ();

  Status = gBS->HandleProtocol (gST->ConsoleOutHandle, &gEfiGraphicsOutputProtocolGuid, (VOID **)&mGop);
  if (EFI_ERROR (Status)) {
    Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&mGop);
  }

  if (EFI_ERROR (Status) || (mGop == NULL) || (mGop->Mode == NULL) || (mGop->Mode->Info == NULL)) {
    mGop = NULL;
  }

  if (mGop != NULL) {
    mScale = AutoScale (mGop->Mode->Info->HorizontalResolution, mGop->Mode->Info->VerticalResolution);
  } else {
    SetupConOutMode ();
  }

  if (!SetupGrid ()) {
    //
    // Out of memory with GOP buffers: fall back to plain ConOut.
    //
    mGop = NULL;
    SetupConOutMode ();
    SetupGrid ();
  }
}

VOID
UiRestore (
  VOID
  )
{
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *Out;

  Out = gST->ConOut;
  if (mGop != NULL) {
    FillScreenPixels (mPalette[EFI_BLACK]);
  }

  Out->SetAttribute (Out, mSavedAttr);
  if ((mSavedMode >= 0) && (mSavedMode != Out->Mode->Mode)) {
    Out->SetMode (Out, (UINTN)mSavedMode);
  }

  Out->ClearScreen (Out);
  Out->EnableCursor (Out, mSavedCursor);
}

/**
  Cycle the GOP scale (F2). Returns TRUE when the grid changed; the caller
  must redraw everything.
**/
BOOLEAN
UiCycleScale (
  VOID
  )
{
  UINTN  HRes;
  UINTN  VRes;
  UINTN  Max;
  UINTN  Next;

  if (mGop == NULL) {
    return FALSE;
  }

  HRes = mGop->Mode->Info->HorizontalResolution;
  VRes = mGop->Mode->Info->VerticalResolution;

  //
  // Largest scale that still fits the minimum 80x24 layout.
  //
  for (Max = 8; Max > 1; Max--) {
    if ((HRes / (GLYPH_W * Max) >= 80) && (VRes / (GLYPH_H * Max) >= 24)) {
      break;
    }
  }

  Next = (mScale >= Max) ? 1 : mScale + 1;
  if (Next == mScale) {
    return FALSE;
  }

  mScale = Next;
  return SetupGrid ();
}

VOID
UiModeText (
  OUT CHAR16  *Buf,
  IN  UINTN   BufSize
  )
{
  if (mGop != NULL) {
    UnicodeSPrint (
      Buf,
      BufSize,
      L"%dx%d  %dx%d  font x%d",
      mGop->Mode->Info->HorizontalResolution,
      mGop->Mode->Info->VerticalResolution,
      gScreenCols,
      gScreenRows,
      mScale
      );
  } else {
    UnicodeSPrint (Buf, BufSize, L"text %dx%d", gScreenCols, gScreenRows);
  }
}

//
// ---- Flush -----------------------------------------------------------------
//

STATIC
VOID
GopDrawSpan (
  IN UINTN  Row,
  IN UINTN  First,
  IN UINTN  Last
  )
{
  UINTN                          Cells;
  UINTN                          PixW;
  UINTN                          PixH;
  UINTN                          Col;
  UINTN                          Y;
  UINTN                          X;
  UINTN                          Sx;
  UINTN                          Sy;
  UI_CELL                        *Cell;
  CONST UINT8                    *Glyph;
  UINT8                          Bits;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Fg;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Bg;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Line;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Pixel;

  Cells = Last - First + 1;
  PixW  = Cells * GLYPH_W * mScale;
  PixH  = GLYPH_H * mScale;

  for (Col = First; Col <= Last; Col++) {
    Cell  = &mCells[Row * gScreenCols + Col];
    Fg    = mPalette[Cell->Attr & 0x0F];
    Bg    = mPalette[(Cell->Attr >> 4) & 0x07];
    Glyph = (Cell->Ch < 128) ? mGlyph[Cell->Ch] : NULL;
    if (Glyph == NULL) {
      Glyph = mGlyph[L'?'];
      if (Cell->Ch == L' ') {
        Glyph = mGlyph[L' '];
      }
    }

    for (Y = 0; Y < GLYPH_H; Y++) {
      Bits = (Glyph != NULL) ? Glyph[Y] : 0;
      Line = mRowBuf + (Y * mScale) * PixW + (Col - First) * GLYPH_W * mScale;
      for (X = 0; X < GLYPH_W; X++) {
        Pixel = (Bits & (0x80 >> X)) ? Fg : Bg;
        for (Sx = 0; Sx < mScale; Sx++) {
          Line[X * mScale + Sx] = Pixel;
        }
      }

      //
      // Replicate the scanline for vertical scaling.
      //
      for (Sy = 1; Sy < mScale; Sy++) {
        CopyMem (Line + Sy * PixW, Line, GLYPH_W * mScale * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL));
      }
    }
  }

  mGop->Blt (
          mGop,
          mRowBuf,
          EfiBltBufferToVideo,
          0,
          0,
          mOriginX + First * GLYPH_W * mScale,
          mOriginY + Row * PixH,
          PixW,
          PixH,
          PixW * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL)
          );
}

STATIC
VOID
ConOutDrawSpan (
  IN UINTN  Row,
  IN UINTN  First,
  IN UINTN  Last
  )
{
  CHAR16   Buf[MAX_COLS + 1];
  UINTN    Col;
  UINTN    Start;
  UINTN    Len;
  UINT8    Attr;
  UI_CELL  *Line;

  //
  // Never write the bottom-right cell: it would scroll the console.
  //
  if ((Row == gScreenRows - 1) && (Last == gScreenCols - 1)) {
    if (Last == First) {
      return;
    }

    Last--;
  }

  Line = &mCells[Row * gScreenCols];
  Col  = First;
  while (Col <= Last) {
    Start = Col;
    Attr  = Line[Col].Attr;
    Len   = 0;
    while ((Col <= Last) && (Line[Col].Attr == Attr)) {
      Buf[Len++] = (Line[Col].Ch >= 0x20) ? Line[Col].Ch : L' ';
      Col++;
    }

    Buf[Len] = L'\0';
    gST->ConOut->SetAttribute (gST->ConOut, Attr);
    gST->ConOut->SetCursorPosition (gST->ConOut, Start, Row);
    gST->ConOut->OutputString (gST->ConOut, Buf);
  }
}

VOID
UiFlush (
  VOID
  )
{
  UINTN  Row;
  UINTN  Col;
  UINTN  First;
  UINTN  Last;
  UINTN  Base;

  for (Row = 0; Row < gScreenRows; Row++) {
    Base  = Row * gScreenCols;
    First = MAX_UINTN;
    Last  = 0;
    for (Col = 0; Col < gScreenCols; Col++) {
      if ((mCells[Base + Col].Ch != mDrawn[Base + Col].Ch) ||
          (mCells[Base + Col].Attr != mDrawn[Base + Col].Attr))
      {
        if (First == MAX_UINTN) {
          First = Col;
        }

        Last = Col;
      }
    }

    if (First == MAX_UINTN) {
      continue;
    }

    if (mGop != NULL) {
      GopDrawSpan (Row, First, Last);
    } else {
      ConOutDrawSpan (Row, First, Last);
    }

    CopyMem (&mDrawn[Base + First], &mCells[Base + First], (Last - First + 1) * sizeof (UI_CELL));
  }
}

//
// ---- Drawing into the shadow buffer ----------------------------------------
//

VOID
UiClear (
  VOID
  )
{
  UINTN  Row;

  for (Row = 0; Row < gScreenRows; Row++) {
    UiFillLine (Row, ATTR_NORMAL);
  }
}

VOID
UiFillLine (
  IN UINTN  Row,
  IN UINTN  Attr
  )
{
  UINTN    Col;
  UI_CELL  *Line;

  if (Row >= gScreenRows) {
    return;
  }

  Line = &mCells[Row * gScreenCols];
  for (Col = 0; Col < gScreenCols; Col++) {
    Line[Col].Ch   = L' ';
    Line[Col].Attr = (UINT8)Attr;
  }
}

VOID
UiPut (
  IN UINTN         Col,
  IN UINTN         Row,
  IN UINTN         Attr,
  IN CONST CHAR16  *Text
  )
{
  UI_CELL  *Cell;

  if (Row >= gScreenRows) {
    return;
  }

  Cell = &mCells[Row * gScreenCols];
  while ((*Text != L'\0') && (Col < gScreenCols)) {
    Cell[Col].Ch   = *Text++;
    Cell[Col].Attr = (UINT8)Attr;
    Col++;
  }
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
  UiPut (Col, Row, Attr, Buf);
}

/**
  Draw the title bar (row 0) and help bar (last row), clear the body.
**/
//
// ---- Menu bar (F10) ---------------------------------------------------------------
//

INTN   gUiJump    = -1;
INTN   gUiCurrent = UI_MOD_PCI;

STATIC CONST CHAR16  *mBarItems[] = {
  L"PCI", L"Memory", L"IO", L"MSR", L"ACPI", L"SMBIOS", L"USB", L"COM", L"Help"
};

STATIC CONST CHAR16  *mHelpItems[] = { L"About", L"Font Size", L"Exit" };

STATIC
UINTN
BarCol (
  IN UINTN  Item
  )
{
  UINTN  Col;
  UINTN  Index;

  Col = 1;
  for (Index = 0; Index < Item; Index++) {
    Col += StrLen (mBarItems[Index]) + 2;
  }

  return Col;
}

/**
  Draw row 0. Sel >= 0 highlights that entry (menu active); otherwise the
  current module is shown in white.
**/
VOID
UiDrawMenuBar (
  IN INTN  Sel
  )
{
  UINTN  Index;
  UINTN  Attr;

  UiFillLine (0, ATTR_TITLE);
  for (Index = 0; Index < ARRAY_SIZE (mBarItems); Index++) {
    if ((INTN)Index == Sel) {
      Attr = ATTR_HILITE;
    } else if ((Sel < 0) && ((INTN)Index == gUiCurrent)) {
      Attr = EFI_TEXT_ATTR (EFI_WHITE, EFI_DARKGRAY);
    } else {
      Attr = ATTR_TITLE;
    }

    UiPrintAt (BarCol (Index), 0, Attr, L" %s ", mBarItems[Index]);
  }
}

/**
  Interactive menu bar over whatever is on screen. The screen is restored
  when the menu closes.

  @return UI_MOD_* chosen, or -1 when cancelled
**/
INTN
UiTopMenu (
  VOID
  )
{
  UI_CELL        *Saved;
  UINTN          Size;
  INTN           Sel;
  INTN           Sub;
  INTN           Result;
  UINTN          Index;
  UINTN          Col;
  EFI_INPUT_KEY  Key;
  BOOLEAN        Open;

  Size  = gScreenCols * gScreenRows * sizeof (UI_CELL);
  Saved = AllocateCopyPool (Size, mCells);
  Sel   = ((gUiCurrent >= 0) && (gUiCurrent < (INTN)ARRAY_SIZE (mBarItems))) ? gUiCurrent : 0;
  Sub   = 0;
  Open  = FALSE;
  Result = -1;
  for ( ; ;) {
    if (Saved != NULL) {
      CopyMem (mCells, Saved, Size);
    }

    UiDrawMenuBar (Sel);
    if (Open) {
      Col = BarCol ((UINTN)Sel);
      for (Index = 0; Index < ARRAY_SIZE (mHelpItems); Index++) {
        UiPrintAt (Col, 1 + Index, ((INTN)Index == Sub) ? ATTR_HILITE : ATTR_TITLE, L" %-12s ", mHelpItems[Index]);
      }
    }

    UiStatus (ATTR_DIM, L"Left/Right: choose   Enter: open   Esc: close menu");
    UiFlush ();
    gBS->WaitForEvent (1, &gST->ConIn->WaitForKey, &Index);
    if (EFI_ERROR (gST->ConIn->ReadKeyStroke (gST->ConIn, &Key))) {
      continue;
    }

    if ((Key.ScanCode == SCAN_ESC) || (Key.ScanCode == SCAN_F10)) {
      if (Open) {
        Open = FALSE;
        continue;
      }

      break;
    }

    if (Key.ScanCode == SCAN_LEFT) {
      Sel  = (Sel == 0) ? (INTN)ARRAY_SIZE (mBarItems) - 1 : Sel - 1;
      Open = FALSE;
    } else if (Key.ScanCode == SCAN_RIGHT) {
      Sel  = (Sel + 1) % (INTN)ARRAY_SIZE (mBarItems);
      Open = FALSE;
    } else if (Open && (Key.ScanCode == SCAN_UP)) {
      Sub = (Sub == 0) ? (INTN)ARRAY_SIZE (mHelpItems) - 1 : Sub - 1;
    } else if (Open && (Key.ScanCode == SCAN_DOWN)) {
      Sub = (Sub + 1) % (INTN)ARRAY_SIZE (mHelpItems);
    } else if ((Key.UnicodeChar == CHAR_CARRIAGE_RETURN) || (Key.ScanCode == SCAN_DOWN)) {
      if (Sel != UI_MOD_HELP) {
        if (Key.UnicodeChar == CHAR_CARRIAGE_RETURN) {
          Result = Sel;
          break;
        }
      } else if (!Open) {
        Open = TRUE;
        Sub  = 0;
      } else {
        Result = UI_MOD_ABOUT + Sub;
        break;
      }
    }
  }

  if (Saved != NULL) {
    CopyMem (mCells, Saved, Size);
    FreePool (Saved);
  }

  return Result;
}

VOID
UiFrame (
  IN CONST CHAR16  *Title,
  IN CONST CHAR16  *Help
  )
{
  UINTN  Row;
  UINTN  Len;

  UiDrawMenuBar (-1);
  Len = StrLen (Title);
  if (Len + 2 < gScreenCols) {
    UiPrintAt (gScreenCols - Len - 2, 0, ATTR_TITLE, L"%s", Title);
  }

  for (Row = 1; Row < gScreenRows - 1; Row++) {
    UiFillLine (Row, ATTR_NORMAL);
  }

  UiFillLine (gScreenRows - 1, ATTR_TITLE);
  UiPrintAt (1, gScreenRows - 1, ATTR_TITLE, L"%s", Help);
  if (StrLen (Help) + 14 < gScreenCols) {
    UiPrintAt (gScreenCols - 11, gScreenRows - 1, ATTR_TITLE, L"F10: Menu");
  }
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
  UiPut (1, gScreenRows - 2, Attr, Buf);
}

/**
  Wait for a key, at most TimeoutMs (0 = forever). F10 opens the menu bar
  here, so every screen gets it; picking an entry sets gUiJump and returns
  Esc so each screen unwinds back to the dispatcher.

  @retval FALSE  timed out
**/
BOOLEAN
UiReadKeyTimeout (
  OUT EFI_INPUT_KEY  *Key,
  IN  UINTN          TimeoutMs
  )
{
  UINTN      Index;
  EFI_EVENT  Events[2];
  EFI_EVENT  Timer;
  UINTN      Count;
  INTN       Pick;

  if (gUiJump >= 0) {
    Key->ScanCode    = SCAN_ESC;
    Key->UnicodeChar = CHAR_NULL;
    return TRUE;
  }

  Timer = NULL;
  Count = 1;
  Events[0] = gST->ConIn->WaitForKey;
  if ((TimeoutMs != 0) && !EFI_ERROR (gBS->CreateEvent (EVT_TIMER, 0, NULL, NULL, &Timer))) {
    gBS->SetTimer (Timer, TimerRelative, MultU64x32 (TimeoutMs, 10000));
    Events[1] = Timer;
    Count     = 2;
  }

  for ( ; ;) {
    UiFlush ();
    gBS->WaitForEvent (Count, Events, &Index);
    if (Index == 1) {
      gBS->CloseEvent (Timer);
      return FALSE;
    }

    if (EFI_ERROR (gST->ConIn->ReadKeyStroke (gST->ConIn, Key))) {
      continue;
    }

    if (Key->ScanCode == SCAN_F10) {
      Pick = UiTopMenu ();
      if (Pick < 0) {
        continue;
      }

      gUiJump          = Pick;
      Key->ScanCode    = SCAN_ESC;
      Key->UnicodeChar = CHAR_NULL;
    }

    if (Timer != NULL) {
      gBS->CloseEvent (Timer);
    }

    return TRUE;
  }
}

VOID
UiReadKey (
  OUT EFI_INPUT_KEY  *Key
  )
{
  UiReadKeyTimeout (Key, 0);
}

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
  F2 cycles the font scale (GOP backend only).
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
  EFI_INPUT_KEY  Key;
  BOOLEAN        Redraw;

  Sel    = (Initial < Count) ? Initial : 0;
  Redraw = TRUE;
  for ( ; ;) {
    if (Redraw) {
      UiFrame (Title, L"Up/Down: select   Enter: open   Esc: back");
      Redraw = FALSE;
    }

    for (Index = 0; Index < Count; Index++) {
      UiPrintAt (
        4,
        3 + Index,
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
