/** @file
  GuiRW shared declarations.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef GUIRW_H_
#define GUIRW_H_

#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/IoLib.h>

#define GUIRW_VERSION  L"0.3.0"

//
// Colors
//
#define ATTR_NORMAL   EFI_TEXT_ATTR (EFI_LIGHTGRAY, EFI_BLUE)
#define ATTR_TITLE    EFI_TEXT_ATTR (EFI_BLACK, EFI_LIGHTGRAY)
#define ATTR_HILITE   EFI_TEXT_ATTR (EFI_WHITE, EFI_CYAN)
#define ATTR_HEADER   EFI_TEXT_ATTR (EFI_YELLOW, EFI_BLUE)
#define ATTR_EDIT     EFI_TEXT_ATTR (EFI_BLACK, EFI_YELLOW)
#define ATTR_WARN     EFI_TEXT_ATTR (EFI_WHITE, EFI_RED)
#define ATTR_DIM      EFI_TEXT_ATTR (EFI_DARKGRAY, EFI_BLUE)
#define ATTR_CHANGED  EFI_TEXT_ATTR (EFI_LIGHTRED, EFI_BLUE)

//
// Ui.c
//
extern UINTN  gScreenCols;
extern UINTN  gScreenRows;

VOID
UiInit (
  VOID
  );

VOID
UiRestore (
  VOID
  );

VOID
UiClear (
  VOID
  );

VOID
UiFrame (
  IN CONST CHAR16  *Title,
  IN CONST CHAR16  *Help
  );

VOID
EFIAPI
UiStatus (
  IN UINTN         Attr,
  IN CONST CHAR16  *Fmt,
  ...
  );

VOID
EFIAPI
UiPrintAt (
  IN UINTN         Col,
  IN UINTN         Row,
  IN UINTN         Attr,
  IN CONST CHAR16  *Fmt,
  ...
  );

VOID
UiFillLine (
  IN UINTN  Row,
  IN UINTN  Attr
  );

VOID
UiFlush (
  VOID
  );

BOOLEAN
UiCycleScale (
  VOID
  );

VOID
UiModeText (
  OUT CHAR16  *Buf,
  IN  UINTN   BufSize
  );

VOID
UiReadKey (
  OUT EFI_INPUT_KEY  *Key
  );

BOOLEAN
UiPromptHex (
  IN  CONST CHAR16  *Prompt,
  IN  UINTN         MaxDigits,
  IN OUT UINT64     *Value
  );

BOOLEAN
UiConfirm (
  IN CONST CHAR16  *Question
  );

INTN
UiMenu (
  IN CONST CHAR16  *Title,
  IN CONST CHAR16  **Items,
  IN UINTN         Count,
  IN UINTN         Initial
  );

//
// HexEdit.c - generic 256-byte page hex editor over an abstract register space.
//
typedef EFI_STATUS (*HEX_READ_FN)(
  IN  VOID    *Context,
  IN  UINT64  Address,
  IN  UINTN   Width,
  OUT UINT64  *Value
  );

typedef EFI_STATUS (*HEX_WRITE_FN)(
  IN VOID    *Context,
  IN UINT64  Address,
  IN UINTN   Width,
  IN UINT64  Value
  );

typedef struct {
  CONST CHAR16    *Title;
  VOID            *Context;
  HEX_READ_FN     Read;
  HEX_WRITE_FN    Write;            // NULL = read-only
  UINT64          Base;             // first address of the page
  UINTN           StartOffset;      // initial cursor offset within the page
  UINT64          Limit;            // highest valid address (inclusive)
  BOOLEAN         AllowGoto;        // G key lets the user jump to an address
  BOOLEAN         ConfirmWrites;    // ask before every write
  UINTN           AddrDigits;       // digits shown for the row address
} HEX_EDIT_SPACE;

VOID
HexEditRun (
  IN OUT HEX_EDIT_SPACE  *Space
  );

//
// Views
//
VOID
PciViewRun (
  VOID
  );

VOID
MemViewRun (
  VOID
  );

VOID
IoViewRun (
  VOID
  );

VOID
MsrViewRun (
  VOID
  );

//
// Doc.c
//
typedef struct {
  CHAR16    *Text;
  UINT8     Attr;
  INTN      Tag;      // >= 0: selectable item id
} DOC_LINE;

typedef struct {
  DOC_LINE    *Lines;
  UINTN       Count;
  UINTN       Capacity;
} TEXT_DOC;

VOID
DocInit (
  OUT TEXT_DOC  *Doc
  );

VOID
DocFree (
  IN OUT TEXT_DOC  *Doc
  );

VOID
EFIAPI
DocAdd (
  IN OUT TEXT_DOC  *Doc,
  IN UINTN         Attr,
  IN CONST CHAR16  *Fmt,
  ...
  );

VOID
EFIAPI
DocAddItem (
  IN OUT TEXT_DOC  *Doc,
  IN INTN          Tag,
  IN CONST CHAR16  *Fmt,
  ...
  );

VOID
DocAddHex (
  IN OUT TEXT_DOC  *Doc,
  IN CONST UINT8   *Data,
  IN UINTN         Size,
  IN UINTN         Base
  );

VOID
DocView (
  IN CONST CHAR16  *Title,
  IN CONST CHAR16  *Header,
  IN TEXT_DOC      *Doc
  );

INTN
DocSelect (
  IN CONST CHAR16  *Title,
  IN CONST CHAR16  *Help,
  IN CONST CHAR16  *Header,
  IN TEXT_DOC      *Doc,
  IN OUT UINTN     *Sel
  );

VOID
SmbiosViewRun (
  VOID
  );

VOID
UsbViewRun (
  VOID
  );

#endif
