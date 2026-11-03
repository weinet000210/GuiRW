/** @file
  COM port test: a polled 16550 terminal on 3F8/2F8/3E8/2E8 (or any IO
  base). Upper window shows the byte stream as ASCII, lower window as hex;
  sent bytes are yellow, received bytes white. Includes an internal
  loopback self-test that needs no cable.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "UefiRrw.h"
#include <Protocol/SerialIo.h>
#include <Protocol/SuperIo.h>
#include <Protocol/DevicePath.h>
#include <Library/DevicePathLib.h>

//
// 16550 registers
//
#define UART_RBR  0
#define UART_THR  0
#define UART_DLL  0
#define UART_IER  1
#define UART_DLM  1
#define UART_IIR  2
#define UART_FCR  2
#define UART_LCR  3
#define UART_MCR  4
#define UART_LSR  5
#define UART_MSR  6
#define UART_SCR  7

#define LSR_DR    BIT0
#define LSR_OE    BIT1
#define LSR_PE    BIT2
#define LSR_FE    BIT3
#define LSR_THRE  BIT5

#define UART_CLOCK  115200

#define BUF_MAX       65536
#define DIR_TX        0x100
#define POLL_US       200
#define REDRAW_TICKS  100      // about 20 ms between redraws while data flows
#define HEX_PER_ROW   16
#define HEXIN_MAX     96

typedef struct {
  UINT8    Dll;
  UINT8    Dlm;
  UINT8    Lcr;
  UINT8    Ier;
  UINT8    Mcr;
  BOOLEAN  Fifo;
} UART_SAVE;

STATIC CONST UINT32  mBauds[] = { 9600, 19200, 38400, 57600, 115200 };
STATIC CONST UINT16  mStdPorts[] = { 0x3F8, 0x2F8, 0x3E8, 0x2E8 };

//
// Stream buffer: low byte = data, DIR_TX set for bytes we sent.
//
STATIC UINT16  *mBuf;
STATIC UINTN   mLen;
STATIC UINT32  *mLineStart;    // scratch: start index of each display line

STATIC UINT16   mBase;
STATIC UINTN    mBaudIdx;
STATIC BOOLEAN  mShowTx;
STATIC UINTN    mEnterMode;    // 0 = CR, 1 = CR+LF, 2 = LF
STATIC BOOLEAN  mHexInput;
STATIC CHAR16   mHexIn[HEXIN_MAX + 1];
STATIC UINTN    mHexInLen;
STATIC UINTN    mTxCount;
STATIC UINTN    mRxCount;
STATIC UINTN    mOe;
STATIC UINTN    mFe;
STATIC UINTN    mPe;
STATIC UINTN    mScrollA;      // lines above the live bottom (ASCII window)
STATIC UINTN    mScrollH;      // same for the hex window
STATIC CHAR16   mNote[120];
STATIC UINTN    mNoteAttr;

STATIC
UINT8
UartIn (
  IN UINTN  Reg
  )
{
  return IoRead8 (mBase + Reg);
}

STATIC
VOID
UartOut (
  IN UINTN  Reg,
  IN UINT8  Value
  )
{
  IoWrite8 (mBase + Reg, Value);
}

/**
  A 16550 keeps whatever is written to its scratch register; an empty IO
  decode reads back FFh.
**/
STATIC
BOOLEAN
UartPresent (
  IN UINT16  Base
  )
{
  UINT8    Saved;
  BOOLEAN  Ok;

  if (IoRead8 (Base + UART_LSR) == 0xFF) {
    return FALSE;
  }

  Saved = IoRead8 (Base + UART_SCR);
  IoWrite8 (Base + UART_SCR, 0x55);
  Ok = (BOOLEAN)(IoRead8 (Base + UART_SCR) == 0x55);
  IoWrite8 (Base + UART_SCR, 0xAA);
  Ok = (BOOLEAN)(Ok && (IoRead8 (Base + UART_SCR) == 0xAA));
  IoWrite8 (Base + UART_SCR, Saved);
  return Ok;
}

/**
  IO base of a Serial I/O instance, from the EFI_SIO_PROTOCOL of the
  Super I/O device it sits on. 0 when the firmware does not say.
**/
STATIC
UINT16
SerialIoBase (
  IN EFI_DEVICE_PATH_PROTOCOL  *Path
  )
{
  EFI_HANDLE                Sio;
  EFI_SIO_PROTOCOL          *SioProto;
  ACPI_RESOURCE_HEADER_PTR  Res;
  UINT8                     *P;
  UINTN                     Guard;
  UINT8                     Tag;
  UINTN                     Len;

  if (EFI_ERROR (gBS->LocateDevicePath (&gEfiSioProtocolGuid, &Path, &Sio)) ||
      EFI_ERROR (gBS->HandleProtocol (Sio, &gEfiSioProtocolGuid, (VOID **)&SioProto)) ||
      EFI_ERROR (SioProto->GetResources (SioProto, &Res)) ||
      (Res.SmallHeader == NULL))
  {
    return 0;
  }

  P = (UINT8 *)Res.SmallHeader;
  for (Guard = 0; Guard < 64; Guard++) {
    if (P[0] & BIT7) {
      // large item: 1-byte tag, 2-byte length
      P += 3 + (P[1] | (P[2] << 8));
      continue;
    }

    Tag = (P[0] >> 3) & 0x0F;
    Len = P[0] & 0x07;
    if (Tag == 0x0F) {
      break;                                  // end tag
    }

    if (Tag == 0x08) {
      return (UINT16)(P[2] | (P[3] << 8));    // IO port descriptor: minimum base
    }

    if (Tag == 0x09) {
      return (UINT16)((P[1] | (P[2] << 8)) & 0x3FF);   // fixed IO
    }

    P += 1 + Len;
  }

  return 0;
}

/**
  The firmware's Serial I/O instance for this port, if it has one. Its IO
  base comes from the Super I/O resources when published; otherwise an ACPI
  PNP0501 UART whose UID matches the usual COMn order is taken to be it.
**/
STATIC
EFI_HANDLE
FirmwareSerialHandle (
  IN UINT16  Base
  )
{
  EFI_HANDLE                *Handles;
  UINTN                     Count;
  UINTN                     Index;
  UINTN                     Std;
  EFI_DEVICE_PATH_PROTOCOL  *Path;
  EFI_DEVICE_PATH_PROTOCOL  *Node;
  ACPI_HID_DEVICE_PATH      *Acpi;
  EFI_HANDLE                Found;
  UINT16                    SioBase;

  for (Std = 0; Std < ARRAY_SIZE (mStdPorts); Std++) {
    if (mStdPorts[Std] == Base) {
      break;
    }
  }

  if (EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiSerialIoProtocolGuid, NULL, &Count, &Handles))) {
    return NULL;
  }

  Found = NULL;
  for (Index = 0; (Index < Count) && (Found == NULL); Index++) {
    if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiDevicePathProtocolGuid, (VOID **)&Path))) {
      continue;
    }

    SioBase = SerialIoBase (Path);
    if (SioBase != 0) {
      if (SioBase == Base) {
        Found = Handles[Index];
      }

      continue;
    }

    if (Std == ARRAY_SIZE (mStdPorts)) {
      continue;
    }

    for (Node = Path; !IsDevicePathEnd (Node); Node = NextDevicePathNode (Node)) {
      if ((DevicePathType (Node) == ACPI_DEVICE_PATH) && (DevicePathSubType (Node) == ACPI_DP)) {
        Acpi = (ACPI_HID_DEVICE_PATH *)Node;
        if ((Acpi->HID == EISA_PNP_ID (0x0501)) && (Acpi->UID == Std)) {
          Found = Handles[Index];
          break;
        }
      }
    }
  }

  FreePool (Handles);
  return Found;
}

STATIC
VOID
UartSave (
  OUT UART_SAVE  *S
  )
{
  S->Lcr = UartIn (UART_LCR);
  S->Ier = UartIn (UART_IER);
  S->Mcr = UartIn (UART_MCR);
  S->Fifo = (BOOLEAN)((UartIn (UART_IIR) & 0xC0) != 0);
  UartOut (UART_LCR, S->Lcr | BIT7);
  S->Dll = UartIn (UART_DLL);
  S->Dlm = UartIn (UART_DLM);
  UartOut (UART_LCR, S->Lcr);
}

STATIC
VOID
UartRestore (
  IN UART_SAVE  *S
  )
{
  UartOut (UART_LCR, BIT7);
  UartOut (UART_DLL, S->Dll);
  UartOut (UART_DLM, S->Dlm);
  UartOut (UART_LCR, S->Lcr & 0x7F);
  UartOut (UART_FCR, S->Fifo ? 0x07 : 0x00);
  UartOut (UART_IER, S->Ier);
  UartOut (UART_MCR, S->Mcr);
}

STATIC
VOID
UartSetup (
  VOID
  )
{
  UINT16  Divisor;

  Divisor = (UINT16)(UART_CLOCK / mBauds[mBaudIdx]);
  UartOut (UART_IER, 0x00);
  UartOut (UART_LCR, BIT7);
  UartOut (UART_DLL, (UINT8)Divisor);
  UartOut (UART_DLM, (UINT8)(Divisor >> 8));
  UartOut (UART_LCR, 0x03);                   // 8N1
  UartOut (UART_FCR, 0x07);                   // FIFO on, clear RX/TX
  UartOut (UART_MCR, 0x03);                   // DTR, RTS
}

STATIC
VOID
BufPush (
  IN UINT16  Entry
  )
{
  if (mLen == BUF_MAX) {
    CopyMem (mBuf, mBuf + BUF_MAX / 2, (BUF_MAX / 2) * sizeof (UINT16));
    mLen = BUF_MAX / 2;
  }

  mBuf[mLen++] = Entry;
}

STATIC
BOOLEAN
UartSend (
  IN UINT8  Byte
  )
{
  UINTN  Wait;

  for (Wait = 0; (UartIn (UART_LSR) & LSR_THRE) == 0; Wait++) {
    if (Wait > 20000) {
      // ~20 ms: transmitter stuck (no clock, or flow control in hardware)
      return FALSE;
    }

    gBS->Stall (1);
  }

  UartOut (UART_THR, Byte);
  BufPush ((UINT16)(Byte | DIR_TX));
  mTxCount++;
  return TRUE;
}

/**
  Drain the receive FIFO into the buffer.

  @return number of bytes received
**/
STATIC
UINTN
UartPoll (
  VOID
  )
{
  UINT8  Lsr;
  UINTN  Got;

  Got = 0;
  while (Got < 4096) {
    Lsr = UartIn (UART_LSR);
    if (Lsr & LSR_OE) {
      mOe++;
    }

    if (Lsr & LSR_PE) {
      mPe++;
    }

    if (Lsr & LSR_FE) {
      mFe++;
    }

    if ((Lsr & LSR_DR) == 0) {
      break;
    }

    BufPush (UartIn (UART_RBR));
    mRxCount++;
    Got++;
  }

  return Got;
}

//
// ---- Rendering -------------------------------------------------------------------
//

STATIC
BOOLEAN
Visible (
  IN UINT16  Entry
  )
{
  return (BOOLEAN)(mShowTx || ((Entry & DIR_TX) == 0));
}

/**
  ASCII layout: wrap at Width, new line on CR, LF, or CR LF.
  Fills mLineStart; returns the number of lines.
**/
STATIC
UINTN
LayoutAscii (
  IN UINTN  Width
  )
{
  UINTN   Index;
  UINTN   Lines;
  UINTN   Col;
  UINT8   Ch;
  UINT8   Prev;

  Lines            = 1;
  mLineStart[0]    = 0;
  Col              = 0;
  Prev             = 0;
  for (Index = 0; Index < mLen; Index++) {
    if (!Visible (mBuf[Index])) {
      continue;
    }

    Ch = (UINT8)mBuf[Index];
    if ((Ch == '\n') && (Prev == '\r')) {
      mLineStart[Lines - 1] = (UINT32)(Index + 1);
      Prev                  = Ch;
      continue;
    }

    Prev = Ch;
    if ((Ch == '\r') || (Ch == '\n')) {
      mLineStart[Lines++] = (UINT32)(Index + 1);
      Col                 = 0;
      continue;
    }

    if (Col == Width) {
      mLineStart[Lines++] = (UINT32)Index;
      Col                 = 0;
    }

    Col++;
  }

  return Lines;
}

STATIC
VOID
DrawAsciiLine (
  IN UINTN  Row,
  IN UINTN  Start,
  IN UINTN  Width
  )
{
  UINTN   Index;
  UINTN   Col;
  UINT8   Ch;
  CHAR16  Str[2];
  UINTN   Attr;

  Str[1] = L'\0';
  Col    = 0;
  for (Index = Start; (Index < mLen) && (Col < Width); Index++) {
    if (!Visible (mBuf[Index])) {
      continue;
    }

    Ch = (UINT8)mBuf[Index];
    if ((Ch == '\r') || (Ch == '\n')) {
      break;
    }

    Attr = (mBuf[Index] & DIR_TX) ? ATTR_HEADER : EFI_TEXT_ATTR (EFI_WHITE, EFI_BLUE);
    if ((Ch < 0x20) || (Ch >= 0x7F)) {
      Str[0] = L'.';
      Attr   = (mBuf[Index] & DIR_TX) ? EFI_TEXT_ATTR (EFI_BROWN, EFI_BLUE) : ATTR_DIM;
    } else {
      Str[0] = (CHAR16)Ch;
    }

    UiPut (1 + Col, Row, Attr, Str);
    Col++;
  }
}

/**
  Hex layout: at most 16 bytes a row, a new row whenever the direction
  changes. Fills mLineStart; returns the number of rows.
**/
STATIC
UINTN
LayoutHex (
  VOID
  )
{
  UINTN   Index;
  UINTN   Rows;
  UINTN   InRow;
  UINT16  Dir;

  Rows  = 0;
  InRow = 0;
  Dir   = 0xFFFF;
  for (Index = 0; Index < mLen; Index++) {
    if (!Visible (mBuf[Index])) {
      continue;
    }

    if ((InRow == HEX_PER_ROW) || ((mBuf[Index] & DIR_TX) != Dir)) {
      mLineStart[Rows++] = (UINT32)Index;
      InRow              = 0;
      Dir                = mBuf[Index] & DIR_TX;
    }

    InRow++;
  }

  return Rows;
}

STATIC
VOID
DrawHexLine (
  IN UINTN  Row,
  IN UINTN  Start
  )
{
  UINTN    Index;
  UINTN    Count;
  UINT8    Ch;
  UINT16   Dir;
  CHAR16   Cell[4];
  UINTN    Attr;
  UINTN    Offset;

  Dir    = mBuf[Start] & DIR_TX;
  Attr   = Dir ? ATTR_HEADER : EFI_TEXT_ATTR (EFI_WHITE, EFI_BLUE);
  //
  // Offset = how many bytes of this direction came before this row.
  //
  Offset = 0;
  for (Index = 0; Index < Start; Index++) {
    if ((mBuf[Index] & DIR_TX) == Dir) {
      Offset++;
    }
  }

  UiPrintAt (1, Row, Dir ? ATTR_HEADER : ATTR_NORMAL, L"%s %06X:", Dir ? L"TX" : L"RX", Offset);
  Count = 0;
  for (Index = Start; (Index < mLen) && (Count < HEX_PER_ROW); Index++) {
    if (!Visible (mBuf[Index])) {
      continue;
    }

    if ((mBuf[Index] & DIR_TX) != Dir) {
      break;
    }

    Ch = (UINT8)mBuf[Index];
    UnicodeSPrint (Cell, sizeof (Cell), L"%02X", Ch);
    UiPut (12 + Count * 3, Row, Attr, Cell);
    Cell[0] = ((Ch >= 0x20) && (Ch < 0x7F)) ? (CHAR16)Ch : L'.';
    Cell[1] = L'\0';
    UiPut (12 + HEX_PER_ROW * 3 + 2 + Count, Row, Attr, Cell);
    Count++;
  }
}

STATIC
VOID
Draw (
  IN BOOLEAN  Full
  )
{
  UINTN         AsciiTop;
  UINTN         AsciiRows;
  UINTN         HexTop;
  UINTN         HexRows;
  UINTN         Lines;
  UINTN         First;
  UINTN         Row;
  UINTN         Width;
  UINT8         Msr;
  UINT8         Lsr;
  CONST CHAR16  *EnterName[] = { L"CR", L"CR+LF", L"LF" };

  if (Full) {
    UiFrame (
      L"COM Port Test",
      L"Tab:ASCII/Hex  F2:Baud  F3:Clear  F4:Loopback  F5:TX echo  F6:Enter  PgUp/PgDn/End  Esc:Back"
      );
  }

  //
  // Rows: 1 info, 2 ASCII title, ASCII body, hex title, hex body,
  // rows-3 input line, rows-2 status, rows-1 help.
  //
  AsciiTop  = 3;
  AsciiRows = (gScreenRows - 7) / 2;
  HexTop    = AsciiTop + AsciiRows + 1;
  HexRows   = gScreenRows - 3 - HexTop;
  Width     = gScreenCols - 2;

  UiFillLine (1, ATTR_NORMAL);
  UiPrintAt (
    1,
    1,
    ATTR_HEADER,
    L"Port %03Xh  %d 8N1  Enter=%s  TX echo %s  Input %s",
    mBase,
    mBauds[mBaudIdx],
    EnterName[mEnterMode],
    mShowTx ? L"ON" : L"OFF",
    mHexInput ? L"HEX" : L"ASCII"
    );

  UiFillLine (2, ATTR_TITLE);
  UiPrintAt (1, 2, ATTR_TITLE, L"ASCII   (yellow = sent, white = received)%s", mScrollA ? L"   [scrolled - End: live]" : L"");
  Lines = LayoutAscii (Width);
  if (mScrollA > ((Lines > AsciiRows) ? Lines - AsciiRows : 0)) {
    mScrollA = (Lines > AsciiRows) ? Lines - AsciiRows : 0;
  }

  First = (Lines > AsciiRows + mScrollA) ? Lines - AsciiRows - mScrollA : 0;
  for (Row = 0; Row < AsciiRows; Row++) {
    UiFillLine (AsciiTop + Row, ATTR_NORMAL);
    if ((First + Row < Lines) && (mLen > 0)) {
      DrawAsciiLine (AsciiTop + Row, mLineStart[First + Row], Width);
    }
  }

  UiFillLine (HexTop - 1, ATTR_TITLE);
  UiPrintAt (1, HexTop - 1, ATTR_TITLE, L"HEX     dir offset:  00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F%s", mScrollH ? L"   [scrolled]" : L"");
  Lines = LayoutHex ();
  if (mScrollH > ((Lines > HexRows) ? Lines - HexRows : 0)) {
    mScrollH = (Lines > HexRows) ? Lines - HexRows : 0;
  }

  First = (Lines > HexRows + mScrollH) ? Lines - HexRows - mScrollH : 0;
  for (Row = 0; Row < HexRows; Row++) {
    UiFillLine (HexTop + Row, ATTR_NORMAL);
    if (First + Row < Lines) {
      DrawHexLine (HexTop + Row, mLineStart[First + Row]);
    }
  }

  UiFillLine (gScreenRows - 3, ATTR_NORMAL);
  if (mHexInput) {
    UiPrintAt (1, gScreenRows - 3, ATTR_EDIT, L"Hex bytes to send (Enter sends): %s_", mHexIn);
  } else if (mNote[0] != L'\0') {
    UiPrintAt (1, gScreenRows - 3, mNoteAttr, L"%s", mNote);
  } else {
    UiPrintAt (1, gScreenRows - 3, ATTR_DIM, L"Type to send. Tab switches to hex input for raw bytes.");
  }

  Lsr = UartIn (UART_LSR);
  Msr = UartIn (UART_MSR);
  UiStatus (
    ATTR_DIM,
    L"LSR %02X  MSR %02X %s%s%s%s  TX %d  RX %d  Overrun %d  Framing %d  Parity %d",
    Lsr,
    Msr,
    (Msr & BIT4) ? L"CTS " : L"",
    (Msr & BIT5) ? L"DSR " : L"",
    (Msr & BIT6) ? L"RI " : L"",
    (Msr & BIT7) ? L"DCD" : L"",
    mTxCount,
    mRxCount,
    mOe,
    mFe,
    mPe
    );
}

//
// ---- Actions ---------------------------------------------------------------------
//

STATIC
VOID
SetNote (
  IN UINTN         Attr,
  IN CONST CHAR16  *Text
  )
{
  mNoteAttr = Attr;
  StrCpyS (mNote, ARRAY_SIZE (mNote), Text);
}

STATIC
VOID
LoopbackTest (
  VOID
  )
{
  STATIC CONST CHAR8  Pattern[] = "uefirrw loopback 0123456789 \x00\x55\xAA\xFF";
  UINT8               Mcr;
  UINTN               Index;
  UINTN               Wait;
  UINTN               Bad;
  UINT8               Msr1;
  UINT8               Msr0;
  CHAR16              Msg[120];

  Mcr = UartIn (UART_MCR);
  UartOut (UART_MCR, 0x10);                   // loopback, all outputs off
  UartOut (UART_FCR, 0x07);
  while (UartIn (UART_LSR) & LSR_DR) {
    UartIn (UART_RBR);
  }

  Bad = 0;
  for (Index = 0; Index < sizeof (Pattern) - 1; Index++) {
    for (Wait = 0; ((UartIn (UART_LSR) & LSR_THRE) == 0) && (Wait < 20000); Wait++) {
      gBS->Stall (1);
    }

    UartOut (UART_THR, (UINT8)Pattern[Index]);
    for (Wait = 0; ((UartIn (UART_LSR) & LSR_DR) == 0) && (Wait < 20000); Wait++) {
      gBS->Stall (1);
    }

    if (((UartIn (UART_LSR) & LSR_DR) == 0) || (UartIn (UART_RBR) != (UINT8)Pattern[Index])) {
      Bad++;
    }
  }

  //
  // In loopback DTR->DSR, RTS->CTS, OUT1->RI, OUT2->DCD.
  //
  Msr0 = UartIn (UART_MSR) & 0xF0;
  UartOut (UART_MCR, 0x1F);
  gBS->Stall (100);
  Msr1 = UartIn (UART_MSR) & 0xF0;
  UartOut (UART_MCR, Mcr);
  UartIn (UART_MSR);                          // clear delta bits

  UnicodeSPrint (
    Msg,
    sizeof (Msg),
    L"Loopback %s: data %d/%d bytes OK, modem lines %s (MSR %02X -> %02X)",
    ((Bad == 0) && (Msr0 == 0) && (Msr1 == 0xF0)) ? L"PASS" : L"FAIL",
    sizeof (Pattern) - 1 - Bad,
    sizeof (Pattern) - 1,
    ((Msr0 == 0) && (Msr1 == 0xF0)) ? L"OK" : L"FAIL",
    Msr0,
    Msr1
    );
  SetNote (((Bad == 0) && (Msr0 == 0) && (Msr1 == 0xF0)) ? EFI_TEXT_ATTR (EFI_LIGHTGREEN, EFI_BLUE) : ATTR_WARN, Msg);
}

STATIC
VOID
SendHexInput (
  VOID
  )
{
  UINTN   Index;
  INTN    Hi;
  INTN    Digit;
  UINTN   Sent;
  CHAR16  Msg[64];

  Hi   = -1;
  Sent = 0;
  for (Index = 0; Index <= mHexInLen; Index++) {
    Digit = (Index < mHexInLen) ? HexDigitValue (mHexIn[Index]) : -1;
    if (Digit >= 0) {
      if (Hi < 0) {
        Hi = Digit;
      } else {
        UartSend ((UINT8)((Hi << 4) | Digit));
        Sent++;
        Hi = -1;
      }
    } else if (Hi >= 0) {
      // a single digit followed by a separator
      UartSend ((UINT8)Hi);
      Sent++;
      Hi = -1;
    }
  }

  UnicodeSPrint (Msg, sizeof (Msg), L"Sent %d byte(s).", Sent);
  SetNote (ATTR_NORMAL, Msg);
  mHexInLen = 0;
  mHexIn[0] = L'\0';
}

STATIC
VOID
SendEnter (
  VOID
  )
{
  if (mEnterMode != 2) {
    UartSend ('\r');
  }

  if (mEnterMode != 0) {
    UartSend ('\n');
  }
}

/**
  @retval TRUE   leave the terminal
**/
STATIC
BOOLEAN
HandleKey (
  IN EFI_INPUT_KEY  *Key
  )
{
  CHAR16  Msg[64];

  switch (Key->ScanCode) {
    case SCAN_ESC:
      return TRUE;
    case SCAN_F2:
      mBaudIdx = (mBaudIdx + 1) % ARRAY_SIZE (mBauds);
      UartSetup ();
      UnicodeSPrint (Msg, sizeof (Msg), L"Baud rate set to %d.", mBauds[mBaudIdx]);
      SetNote (ATTR_NORMAL, Msg);
      return FALSE;
    case SCAN_F3:
      mLen      = 0;
      mTxCount  = 0;
      mRxCount  = 0;
      mOe       = 0;
      mFe       = 0;
      mPe       = 0;
      mScrollA  = 0;
      mScrollH  = 0;
      SetNote (ATTR_NORMAL, L"Cleared.");
      return FALSE;
    case SCAN_F4:
      SetNote (ATTR_NORMAL, L"Running loopback test...");
      Draw (FALSE);
      UiFlush ();
      LoopbackTest ();
      return FALSE;
    case SCAN_F5:
      mShowTx = (BOOLEAN)!mShowTx;
      return FALSE;
    case SCAN_F6:
      mEnterMode = (mEnterMode + 1) % 3;
      return FALSE;
    case SCAN_PAGE_UP:
      mScrollA += (gScreenRows - 7) / 2;
      mScrollH += (gScreenRows - 7) / 2;
      return FALSE;
    case SCAN_PAGE_DOWN:
      mScrollA = (mScrollA > (gScreenRows - 7) / 2) ? mScrollA - (gScreenRows - 7) / 2 : 0;
      mScrollH = (mScrollH > (gScreenRows - 7) / 2) ? mScrollH - (gScreenRows - 7) / 2 : 0;
      return FALSE;
    case SCAN_END:
      mScrollA = 0;
      mScrollH = 0;
      return FALSE;
    default:
      break;
  }

  if (Key->UnicodeChar == CHAR_TAB) {
    mHexInput = (BOOLEAN)!mHexInput;
    return FALSE;
  }

  if (mHexInput) {
    if (Key->UnicodeChar == CHAR_CARRIAGE_RETURN) {
      SendHexInput ();
    } else if (Key->UnicodeChar == CHAR_BACKSPACE) {
      if (mHexInLen > 0) {
        mHexIn[--mHexInLen] = L'\0';
      }
    } else if (((HexDigitValue (Key->UnicodeChar) >= 0) || (Key->UnicodeChar == L' ')) && (mHexInLen < HEXIN_MAX)) {
      mHexIn[mHexInLen++] = Key->UnicodeChar;
      mHexIn[mHexInLen]   = L'\0';
    }

    return FALSE;
  }

  if (Key->UnicodeChar == CHAR_CARRIAGE_RETURN) {
    SendEnter ();
  } else if ((Key->UnicodeChar != 0) && (Key->UnicodeChar < 0x80)) {
    // printable characters, Backspace (08h) and Ctrl codes go out as-is
    if (!UartSend ((UINT8)Key->UnicodeChar)) {
      SetNote (ATTR_WARN, L"Transmit timeout: THR never emptied.");
    }
  }

  mScrollA = 0;
  mScrollH = 0;
  return FALSE;
}

STATIC
VOID
Terminal (
  VOID
  )
{
  UART_SAVE      Saved;
  EFI_INPUT_KEY  Key;
  BOOLEAN        Dirty;
  UINTN          Ticks;
  EFI_HANDLE     FwSerial;

  FwSerial   = NULL;
  mBuf       = AllocatePool (BUF_MAX * sizeof (UINT16));
  mLineStart = AllocatePool ((BUF_MAX + 1) * sizeof (UINT32));
  if ((mBuf == NULL) || (mLineStart == NULL)) {
    goto Done;
  }

  mLen      = 0;
  mTxCount  = 0;
  mRxCount  = 0;
  mOe       = 0;
  mFe       = 0;
  mPe       = 0;
  mScrollA  = 0;
  mScrollH  = 0;
  mHexInLen = 0;
  mHexIn[0] = L'\0';
  mHexInput = FALSE;
  mNote[0]  = L'\0';

  //
  // The firmware's terminal driver polls its UART and would turn received
  // bytes into keystrokes. Detach it while we own the port.
  //
  FwSerial = FirmwareSerialHandle (mBase);
  if (FwSerial != NULL) {
    if (!EFI_ERROR (gBS->DisconnectController (FwSerial, NULL, NULL))) {
      SetNote (ATTR_WARN, L"Firmware console on this port is paused while testing; it comes back on Esc.");
    } else {
      FwSerial = NULL;
      SetNote (ATTR_WARN, L"Firmware serial console could not be paused: received bytes may be taken by it.");
    }
  }

  UartSave (&Saved);
  UartSetup ();
  Draw (TRUE);
  Dirty = FALSE;
  Ticks = 0;
  for ( ; ;) {
    if (UartPoll () != 0) {
      Dirty = TRUE;
    }

    if (!EFI_ERROR (gST->ConIn->ReadKeyStroke (gST->ConIn, &Key))) {
      if (HandleKey (&Key)) {
        break;
      }

      UartPoll ();
      Draw (FALSE);
      UiFlush ();
      Dirty = FALSE;
      Ticks = 0;
      continue;
    }

    if (Dirty && (++Ticks >= REDRAW_TICKS)) {
      Draw (FALSE);
      UiFlush ();
      Dirty = FALSE;
      Ticks = 0;
    }

    gBS->Stall (POLL_US);
  }

  UartRestore (&Saved);
  if (FwSerial != NULL) {
    gBS->ConnectController (FwSerial, NULL, NULL, TRUE);
  }

Done:
  if (mBuf != NULL) {
    FreePool (mBuf);
    mBuf = NULL;
  }

  if (mLineStart != NULL) {
    FreePool (mLineStart);
    mLineStart = NULL;
  }
}

VOID
ComViewRun (
  VOID
  )
{
  CHAR16        Items[6][64];
  CONST CHAR16  *Ptrs[6];
  UINTN         Index;
  INTN          Choice;
  UINTN         Last;
  UINT64        Value;

  mBaudIdx   = ARRAY_SIZE (mBauds) - 1;     // 115200
  mShowTx    = TRUE;
  mEnterMode = 0;
  Last       = 0;
  for ( ; ;) {
    for (Index = 0; Index < ARRAY_SIZE (mStdPorts); Index++) {
      UnicodeSPrint (
        Items[Index],
        sizeof (Items[Index]),
        L"COM%d   %03Xh   %s",
        Index + 1,
        mStdPorts[Index],
        UartPresent (mStdPorts[Index]) ? L"Present" : L"Not found"
        );
      Ptrs[Index] = Items[Index];
    }

    StrCpyS (Items[4], 64, L"Custom IO base...");
    StrCpyS (Items[5], 64, L"Back");
    Ptrs[4] = Items[4];
    Ptrs[5] = Items[5];

    Choice = UiMenu (L"COM Port Test", Ptrs, 6, Last);
    if ((Choice < 0) || (Choice == 5)) {
      return;
    }

    Last = (UINTN)Choice;
    if (Choice == 4) {
      Value = 0x3F8;
      if (!UiPromptHex (L"UART IO base", 4, &Value) || (Value > 0xFFF8)) {
        continue;
      }

      mBase = (UINT16)Value;
    } else {
      mBase = mStdPorts[Choice];
    }

    Terminal ();
  }
}
