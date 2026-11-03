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
  Print at most N characters of Str.
**/
VOID
UiPrintN (
  IN UINTN         Col,
  IN UINTN         Row,
  IN UINTN         Attr,
  IN CONST CHAR16  *Str,
  IN UINTN         N
  )
{
  CHAR16  Buf[512];
  UINTN   Len;

  Len = MIN (MIN (StrLen (Str), N), ARRAY_SIZE (Buf) - 1);
  CopyMem (Buf, Str, Len * sizeof (CHAR16));
  Buf[Len] = L'\0';
  UiPut (Col, Row, Attr, Buf);
}

//
// ---- Menu bar (F10) ---------------------------------------------------------------
//

INTN   gUiJump    = -1;

STATIC CONST CHAR16  **mHelpLines;
STATIC UINTN         mHelpCount;

/**
  Set the key list that F1 shows for the current screen.
**/
VOID
UiSetHelp (
  IN CONST CHAR16  **Lines,
  IN UINTN         Count
  )
{
  mHelpLines = Lines;
  mHelpCount = Count;
}

/**
  F1: a box listing the current screen's keys over the screen, restored on
  any key.
**/
VOID
UiShowHelp (
  VOID
  )
{
  STATIC CONST CHAR16  *Global[] = {
    L"F1          This help",
    L"F10         Menu bar (File Bus Space Tables Devices Help)",
    L"F12         Screenshot (SCRnnnn.BMP next to uefirrw.efi)",
    L"Esc         Back / close"
  };
  UI_CELL        *Saved;
  UINTN          Size;
  UINTN          Width;
  UINTN          Height;
  UINTN          Left;
  UINTN          Top;
  UINTN          Index;
  UINTN          Row;
  EFI_INPUT_KEY  Key;

  Size  = gScreenCols * gScreenRows * sizeof (UI_CELL);
  Saved = AllocateCopyPool (Size, mCells);
  Width = 0;
  for (Index = 0; Index < mHelpCount; Index++) {
    Width = MAX (Width, StrLen (mHelpLines[Index]));
  }

  for (Index = 0; Index < ARRAY_SIZE (Global); Index++) {
    Width = MAX (Width, StrLen (Global[Index]));
  }

  Width  = MIN (Width + 4, gScreenCols - 2);
  Height = MIN (mHelpCount + ARRAY_SIZE (Global) + 4, gScreenRows - 2);
  Left   = (gScreenCols - Width) / 2;
  Top    = (gScreenRows - Height) / 2;
  for (Row = 0; Row < Height; Row++) {
    UiPrintAt (Left, Top + Row, ATTR_TITLE, L"%-*s", Width, L"");
  }

  UiPrintAt (Left + 2, Top, ATTR_TITLE, L"Keys");
  Row = Top + 1;
  for (Index = 0; (Index < mHelpCount) && (Row < Top + Height - 1); Index++, Row++) {
    UiPrintN (Left + 2, Row, ATTR_TITLE, mHelpLines[Index], Width - 4);
  }

  Row++;
  for (Index = 0; (Index < ARRAY_SIZE (Global)) && (Row < Top + Height - 1); Index++, Row++) {
    UiPrintN (Left + 2, Row, ATTR_TITLE, Global[Index], Width - 4);
  }

  UiFlush ();
  gBS->WaitForEvent (1, &gST->ConIn->WaitForKey, &Index);
  gST->ConIn->ReadKeyStroke (gST->ConIn, &Key);
  if (Saved != NULL) {
    CopyMem (mCells, Saved, Size);
    FreePool (Saved);
  }

  UiFlush ();
}
INTN   gUiCurrent = UI_MOD_PCI;

typedef struct {
  CONST CHAR16    *Name;
  INTN            Id;
} UI_MENU_ITEM;

#define UI_ACT_KEYS      100
#define UI_ACT_SAVETEXT  101
#define UI_ACT_SHOT      102

STATIC CONST UI_MENU_ITEM  mFileItems[] = {
  { L"Save Screen as Text", UI_ACT_SAVETEXT },
  { L"Screenshot     F12",  UI_ACT_SHOT     },
  { L"Dump All to File",    UI_ACT_DUMP     },
  { L"File Browser",        UI_MOD_FILES    },
  { L"Text Editor",         UI_MOD_EDIT     },
  { L"Return to Setup",     UI_ACT_SETUP    },
  { L"Warm Reset",          UI_ACT_WARM     },
  { L"Cold Reset",          UI_ACT_COLD     },
  { L"Exit",                UI_MOD_EXIT     }
};
STATIC CONST UI_MENU_ITEM  mBusItems[] = {
  { L"PCI",         UI_MOD_PCI },
  { L"USB",         UI_MOD_USB }
};
STATIC CONST UI_MENU_ITEM  mSpaceItems[] = {
  { L"Memory",      UI_MOD_MEM  },
  { L"IO",          UI_MOD_IO   },
  { L"MSR",         UI_MOD_MSR  },
  { L"CMOS",        UI_MOD_CMOS },
  { L"EC",          UI_MOD_EC   }
};
STATIC CONST UI_MENU_ITEM  mTableItems[] = {
  { L"ACPI",        UI_MOD_ACPI   },
  { L"SMBIOS",      UI_MOD_SMBIOS }
};
STATIC CONST UI_MENU_ITEM  mDeviceItems[] = {
  { L"Super I/O",   UI_MOD_SIO },
  { L"COM Port",    UI_MOD_COM }
};
STATIC CONST UI_MENU_ITEM  mHelpItems[] = {
  { L"Keys       F1", UI_ACT_KEYS  },
  { L"About",         UI_MOD_ABOUT }
};

typedef struct {
  CONST CHAR16          *Name;
  CONST UI_MENU_ITEM    *Items;
  UINTN                 Count;
} UI_MENU;

STATIC CONST UI_MENU  mMenus[] = {
  { L"File",    mFileItems,   ARRAY_SIZE (mFileItems)   },
  { L"Bus",     mBusItems,    ARRAY_SIZE (mBusItems)    },
  { L"Space",   mSpaceItems,  ARRAY_SIZE (mSpaceItems)  },
  { L"Tables",  mTableItems,  ARRAY_SIZE (mTableItems)  },
  { L"Devices", mDeviceItems, ARRAY_SIZE (mDeviceItems) },
  { L"Help",    mHelpItems,   ARRAY_SIZE (mHelpItems)   }
};

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
    Col += StrLen (mMenus[Index].Name) + 2;
  }

  return Col;
}

/**
  Menu index and item index holding module Id, FALSE when none does.
**/
STATIC
BOOLEAN
FindItem (
  IN  INTN   Id,
  OUT UINTN  *Menu,
  OUT UINTN  *Item
  )
{
  UINTN  M;
  UINTN  I;

  for (M = 0; M < ARRAY_SIZE (mMenus); M++) {
    for (I = 0; I < mMenus[M].Count; I++) {
      if (mMenus[M].Items[I].Id == Id) {
        *Menu = M;
        *Item = I;
        return TRUE;
      }
    }
  }

  return FALSE;
}

/**
  Draw row 0. Sel >= 0 highlights that menu (menu active); otherwise the
  menu holding the current screen is shown in white.
**/
VOID
UiDrawMenuBar (
  IN INTN  Sel
  )
{
  UINTN  Index;
  UINTN  Attr;
  UINTN  CurMenu;
  UINTN  CurItem;

  if (!FindItem (gUiCurrent, &CurMenu, &CurItem)) {
    CurMenu = (UINTN)-1;
  }

  UiFillLine (0, ATTR_TITLE);
  for (Index = 0; Index < ARRAY_SIZE (mMenus); Index++) {
    if ((INTN)Index == Sel) {
      Attr = ATTR_HILITE;
    } else if ((Sel < 0) && (Index == CurMenu)) {
      Attr = EFI_TEXT_ATTR (EFI_WHITE, EFI_DARKGRAY);
    } else {
      Attr = ATTR_TITLE;
    }

    UiPrintAt (BarCol (Index), 0, Attr, L" %s ", mMenus[Index].Name);
  }
}

/**
  Show Msg on the status line for a moment.
**/
VOID
UiNotify (
  IN UINTN         Attr,
  IN CONST CHAR16  *Msg
  )
{
  UiStatus (Attr, L"%s", Msg);
  UiFlush ();
  gBS->Stall (1200 * 1000);
}

STATIC
VOID
ReportSave (
  IN EFI_STATUS    Status,
  IN CONST CHAR16  *Name
  )
{
  CHAR16  Msg[96];

  if (EFI_ERROR (Status)) {
    UnicodeSPrint (Msg, sizeof (Msg), L" Could not save %s: %r ", Name, Status);
    UiNotify (ATTR_WARN, Msg);
  } else {
    UnicodeSPrint (Msg, sizeof (Msg), L"Saved %s\\%s", FileImageVolumeName (), Name);
    UiNotify (ATTR_HEADER, Msg);
  }
}

/**
  Save the text of the screen as SCRnnnn.TXT.
**/
VOID
UiSaveScreenText (
  VOID
  )
{
  UINT8       *Buf;
  UINTN       Pos;
  UINTN       Row;
  UINTN       Col;
  UINTN       End;
  CHAR16      Ch;
  CHAR16      Name[32];
  EFI_STATUS  Status;

  Buf = AllocatePool (gScreenRows * (gScreenCols + 2));
  if (Buf == NULL) {
    return;
  }

  Pos = 0;
  for (Row = 0; Row < gScreenRows; Row++) {
    End = gScreenCols;
    while ((End > 0) && (mCells[Row * gScreenCols + End - 1].Ch == L' ')) {
      End--;
    }

    for (Col = 0; Col < End; Col++) {
      Ch         = mCells[Row * gScreenCols + Col].Ch;
      Buf[Pos++] = ((Ch >= 0x20) && (Ch < 0x7F)) ? (UINT8)Ch : '?';
    }

    Buf[Pos++] = '\r';
    Buf[Pos++] = '\n';
  }

  FileNextName (L"SCR", L"TXT", Name, sizeof (Name));
  Status = FileSaveOnImageVolume (Name, Buf, Pos);
  FreePool (Buf);
  ReportSave (Status, Name);
}

/**
  Save the screen as a 24-bit SCRnnnn.BMP, drawn from the cell buffer with
  the built-in font at 1:1 (8x19 per cell), so it works with or without GOP.
**/
VOID
UiScreenshot (
  VOID
  )
{
  UINTN        W;
  UINTN        H;
  UINTN        Stride;
  UINTN        Size;
  UINT8        *Bmp;
  UINT8        *Px;
  UINTN        Row;
  UINTN        Col;
  UINTN        Y;
  UINTN        X;
  UI_CELL      *Cell;
  CONST UINT8  *Glyph;
  UINT8        Bits;
  CONST EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *C;
  CHAR16       Name[32];
  EFI_STATUS   Status;

  W      = gScreenCols * GLYPH_W;
  H      = gScreenRows * GLYPH_H;
  Stride = (W * 3 + 3) & ~(UINTN)3;
  Size   = 54 + Stride * H;
  Bmp    = AllocateZeroPool (Size);
  if (Bmp == NULL) {
    return;
  }

  Bmp[0] = 'B';
  Bmp[1] = 'M';
  WriteUnaligned32 ((UINT32 *)(Bmp + 2), (UINT32)Size);
  WriteUnaligned32 ((UINT32 *)(Bmp + 10), 54);
  WriteUnaligned32 ((UINT32 *)(Bmp + 14), 40);
  WriteUnaligned32 ((UINT32 *)(Bmp + 18), (UINT32)W);
  WriteUnaligned32 ((UINT32 *)(Bmp + 22), (UINT32)H);
  WriteUnaligned16 ((UINT16 *)(Bmp + 26), 1);
  WriteUnaligned16 ((UINT16 *)(Bmp + 28), 24);
  WriteUnaligned32 ((UINT32 *)(Bmp + 34), (UINT32)(Stride * H));
  for (Row = 0; Row < gScreenRows; Row++) {
    for (Col = 0; Col < gScreenCols; Col++) {
      Cell  = &mCells[Row * gScreenCols + Col];
      Glyph = (Cell->Ch < 128) ? mGlyph[Cell->Ch] : NULL;
      if ((Glyph == NULL) && (Cell->Ch != L' ')) {
        Glyph = mGlyph[L'?'];
      }

      for (Y = 0; Y < GLYPH_H; Y++) {
        Bits = (Glyph != NULL) ? Glyph[Y] : 0;
        //
        // BMP rows run bottom-up.
        //
        Px = Bmp + 54 + (H - 1 - (Row * GLYPH_H + Y)) * Stride + Col * GLYPH_W * 3;
        for (X = 0; X < GLYPH_W; X++) {
          C       = (Bits & (0x80 >> X)) ? &mPalette[Cell->Attr & 0x0F] : &mPalette[(Cell->Attr >> 4) & 0x07];
          Px[0]   = C->Blue;
          Px[1]   = C->Green;
          Px[2]   = C->Red;
          Px     += 3;
        }
      }
    }
  }

  FileNextName (L"SCR", L"BMP", Name, sizeof (Name));
  Status = FileSaveOnImageVolume (Name, Bmp, Size);
  FreePool (Bmp);
  ReportSave (Status, Name);
}

/**
  Interactive menu bar over whatever is on screen: Left/Right picks a menu,
  its list drops down, Up/Down picks an entry. The screen is restored when
  the menu closes. Keys, Save Screen and Screenshot run in place.

  @return UI_MOD_* / UI_ACT_* chosen, or -1 when cancelled
**/
INTN
UiTopMenu (
  VOID
  )
{
  UI_CELL        *Saved;
  UINTN          Size;
  INTN           Sel;
  UINTN          Sub;
  INTN           Result;
  UINTN          Index;
  UINTN          Col;
  UINTN          Width;
  UINTN          CurMenu;
  UINTN          CurItem;
  INTN           Id;
  EFI_INPUT_KEY  Key;

  Size  = gScreenCols * gScreenRows * sizeof (UI_CELL);
  Saved = AllocateCopyPool (Size, mCells);
  if (FindItem (gUiCurrent, &CurMenu, &CurItem)) {
    Sel = (INTN)CurMenu;
    Sub = CurItem;
  } else {
    Sel = 0;
    Sub = 0;
  }

  Result = -1;
  for ( ; ;) {
    if (Saved != NULL) {
      CopyMem (mCells, Saved, Size);
    }

    UiDrawMenuBar (Sel);
    Col   = BarCol ((UINTN)Sel);
    Width = 0;
    for (Index = 0; Index < mMenus[Sel].Count; Index++) {
      Width = MAX (Width, StrLen (mMenus[Sel].Items[Index].Name));
    }

    if (Col + Width + 4 > gScreenCols) {
      Col = gScreenCols - Width - 4;
    }

    UiPrintAt (Col, 1, ATTR_TITLE, L"%-*s", Width + 4, L"");
    for (Index = 0; Index < mMenus[Sel].Count; Index++) {
      UiPrintAt (Col, 2 + Index, (Index == Sub) ? ATTR_HILITE : ATTR_TITLE, L"  %-*s  ", Width, mMenus[Sel].Items[Index].Name);
    }

    UiPrintAt (Col, 2 + mMenus[Sel].Count, ATTR_TITLE, L"%-*s", Width + 4, L"");
    UiStatus (ATTR_DIM, L"Left/Right: menu   Up/Down: item   Enter: open   Esc: close");
    UiFlush ();
    gBS->WaitForEvent (1, &gST->ConIn->WaitForKey, &Index);
    if (EFI_ERROR (gST->ConIn->ReadKeyStroke (gST->ConIn, &Key))) {
      continue;
    }

    if ((Key.ScanCode == SCAN_ESC) || (Key.ScanCode == SCAN_F10)) {
      break;
    }

    if (Key.ScanCode == SCAN_LEFT) {
      Sel = (Sel == 0) ? (INTN)ARRAY_SIZE (mMenus) - 1 : Sel - 1;
      Sub = 0;
    } else if (Key.ScanCode == SCAN_RIGHT) {
      Sel = (Sel + 1) % (INTN)ARRAY_SIZE (mMenus);
      Sub = 0;
    } else if (Key.ScanCode == SCAN_UP) {
      Sub = (Sub == 0) ? mMenus[Sel].Count - 1 : Sub - 1;
    } else if (Key.ScanCode == SCAN_DOWN) {
      Sub = (Sub + 1) % mMenus[Sel].Count;
    } else if (Key.UnicodeChar == CHAR_CARRIAGE_RETURN) {
      Id = mMenus[Sel].Items[Sub].Id;
      if ((Id == UI_ACT_KEYS) || (Id == UI_ACT_SAVETEXT) || (Id == UI_ACT_SHOT)) {
        if (Saved != NULL) {
          CopyMem (mCells, Saved, Size);
        }

        UiFlush ();
        if (Id == UI_ACT_KEYS) {
          UiShowHelp ();
        } else if (Id == UI_ACT_SAVETEXT) {
          UiSaveScreenText ();
        } else {
          UiScreenshot ();
        }

        break;
      }

      Result = Id;
      break;
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
  } else if (StrLen (Help) + 3 < gScreenCols) {
    UiPrintAt (gScreenCols - 2, gScreenRows - 1, ATTR_TITLE, L" ");
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

    if (Key->ScanCode == SCAN_F1) {
      UiShowHelp ();
      continue;
    }

    if (Key->ScanCode == SCAN_F12) {
      UiScreenshot ();
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
