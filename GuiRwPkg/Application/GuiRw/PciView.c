/** @file
  PCI device list and configuration space editor
  (via EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL, so segments and ECAM work).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "GuiRw.h"
#include <Protocol/PciRootBridgeIo.h>
#include <IndustryStandard/Acpi.h>
#include <IndustryStandard/Pci.h>

#define MAX_PCI_DEVICES  512

typedef struct {
  EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL    *RootBridge;
  UINT32                             Segment;
  UINT8                              Bus;
  UINT8                              Dev;
  UINT8                              Func;
  UINT16                             VendorId;
  UINT16                             DeviceId;
  UINT8                              BaseClass;
  UINT8                              SubClass;
  UINT8                              ProgIf;
  UINT8                              HeaderType;
} PCI_DEV_ENTRY;

STATIC PCI_DEV_ENTRY  *mDevs;
STATIC UINTN          mDevCount;

typedef struct {
  UINT8           BaseClass;
  INT16           SubClass;     // -1 = any
  CONST CHAR16    *Name;
} PCI_CLASS_NAME;

STATIC CONST PCI_CLASS_NAME  mClassNames[] = {
  { 0x00, -1,   L"Unclassified"            },
  { 0x01, 0x01, L"IDE Controller"          },
  { 0x01, 0x06, L"SATA Controller"         },
  { 0x01, 0x08, L"NVMe Controller"         },
  { 0x01, -1,   L"Mass Storage"            },
  { 0x02, 0x00, L"Ethernet Controller"     },
  { 0x02, -1,   L"Network Controller"      },
  { 0x03, 0x00, L"VGA Controller"          },
  { 0x03, -1,   L"Display Controller"      },
  { 0x04, 0x03, L"HD Audio"                },
  { 0x04, -1,   L"Multimedia"              },
  { 0x05, -1,   L"Memory Controller"       },
  { 0x06, 0x00, L"Host Bridge"             },
  { 0x06, 0x01, L"ISA Bridge"              },
  { 0x06, 0x04, L"PCI-to-PCI Bridge"       },
  { 0x06, -1,   L"Bridge"                  },
  { 0x07, -1,   L"Communication"           },
  { 0x08, -1,   L"System Peripheral"       },
  { 0x09, -1,   L"Input Device"            },
  { 0x0C, 0x03, L"USB Controller"          },
  { 0x0C, 0x05, L"SMBus Controller"        },
  { 0x0C, -1,   L"Serial Bus"              },
  { 0x0D, -1,   L"Wireless"                },
  { 0x10, -1,   L"Encryption"              },
  { 0x11, -1,   L"Signal Processing"       },
  { 0x12, -1,   L"Processing Accelerator"  },
  { 0x13, -1,   L"Non-Essential Instrument"},
};

STATIC
CONST CHAR16 *
ClassName (
  IN UINT8  BaseClass,
  IN UINT8  SubClass
  )
{
  UINTN  Index;

  for (Index = 0; Index < ARRAY_SIZE (mClassNames); Index++) {
    if ((mClassNames[Index].BaseClass == BaseClass) &&
        ((mClassNames[Index].SubClass < 0) || (mClassNames[Index].SubClass == SubClass)))
    {
      return mClassNames[Index].Name;
    }
  }

  return L"Other";
}

STATIC
UINT64
PciAddress (
  IN UINT8   Bus,
  IN UINT8   Dev,
  IN UINT8   Func,
  IN UINT32  Reg
  )
{
  UINT64  Addr;

  Addr = EFI_PCI_ADDRESS (Bus, Dev, Func, 0);
  if (Reg < 0x100) {
    Addr |= Reg;
  } else {
    Addr |= LShiftU64 (Reg, 32);   // ExtendedRegister field
  }

  return Addr;
}

STATIC
EFI_STATUS
CfgRead (
  IN  EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL  *Rb,
  IN  UINT8                            Bus,
  IN  UINT8                            Dev,
  IN  UINT8                            Func,
  IN  UINT32                           Reg,
  IN  UINTN                            Width,
  OUT UINT64                           *Value
  )
{
  EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH  W;

  *Value = 0;
  W      = (Width == 1) ? EfiPciWidthUint8 : ((Width == 2) ? EfiPciWidthUint16 : EfiPciWidthUint32);
  return Rb->Pci.Read (Rb, W, PciAddress (Bus, Dev, Func, Reg), 1, Value);
}

STATIC
VOID
BusRange (
  IN  EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL  *Rb,
  OUT UINTN                            *Min,
  OUT UINTN                            *Max
  )
{
  EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR  *Desc;
  VOID                               *Config;

  *Min = 0;
  *Max = 255;
  if (EFI_ERROR (Rb->Configuration (Rb, &Config)) || (Config == NULL)) {
    return;
  }

  Desc = (EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR *)Config;
  while (Desc->Desc == ACPI_ADDRESS_SPACE_DESCRIPTOR) {
    if (Desc->ResType == ACPI_ADDRESS_SPACE_TYPE_BUS) {
      *Min = (UINTN)Desc->AddrRangeMin;
      *Max = (UINTN)Desc->AddrRangeMax;
      if (*Max > 255) {
        *Max = 255;
      }

      return;
    }

    Desc++;
  }
}

STATIC
VOID
ScanPci (
  VOID
  )
{
  EFI_STATUS                       Status;
  EFI_HANDLE                       *Handles;
  UINTN                            HandleCount;
  UINTN                            HIndex;
  EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL  *Rb;
  UINTN                            BusMin;
  UINTN                            BusMax;
  UINTN                            Bus;
  UINTN                            Dev;
  UINTN                            Func;
  UINT64                           Id;
  UINT64                           ClassReg;
  UINT64                           Header;
  PCI_DEV_ENTRY                    *E;

  mDevCount = 0;
  if (mDevs == NULL) {
    mDevs = AllocateZeroPool (sizeof (PCI_DEV_ENTRY) * MAX_PCI_DEVICES);
    if (mDevs == NULL) {
      return;
    }
  }

  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiPciRootBridgeIoProtocolGuid, NULL, &HandleCount, &Handles);
  if (EFI_ERROR (Status)) {
    return;
  }

  for (HIndex = 0; HIndex < HandleCount; HIndex++) {
    if (EFI_ERROR (gBS->HandleProtocol (Handles[HIndex], &gEfiPciRootBridgeIoProtocolGuid, (VOID **)&Rb))) {
      continue;
    }

    BusRange (Rb, &BusMin, &BusMax);
    for (Bus = BusMin; Bus <= BusMax; Bus++) {
      for (Dev = 0; Dev <= PCI_MAX_DEVICE; Dev++) {
        for (Func = 0; Func <= PCI_MAX_FUNC; Func++) {
          if (EFI_ERROR (CfgRead (Rb, (UINT8)Bus, (UINT8)Dev, (UINT8)Func, 0, 4, &Id))) {
            break;
          }

          if (((UINT16)Id == 0xFFFF) || ((UINT16)Id == 0x0000)) {
            if (Func == 0) {
              break;     // no device: skip remaining functions
            }

            continue;
          }

          CfgRead (Rb, (UINT8)Bus, (UINT8)Dev, (UINT8)Func, PCI_HEADER_TYPE_OFFSET, 1, &Header);
          CfgRead (Rb, (UINT8)Bus, (UINT8)Dev, (UINT8)Func, PCI_REVISION_ID_OFFSET, 4, &ClassReg);

          if (mDevCount < MAX_PCI_DEVICES) {
            E             = &mDevs[mDevCount++];
            E->RootBridge = Rb;
            E->Segment    = Rb->SegmentNumber;
            E->Bus        = (UINT8)Bus;
            E->Dev        = (UINT8)Dev;
            E->Func       = (UINT8)Func;
            E->VendorId   = (UINT16)Id;
            E->DeviceId   = (UINT16)RShiftU64 (Id, 16);
            E->ProgIf     = (UINT8)RShiftU64 (ClassReg, 8);
            E->SubClass   = (UINT8)RShiftU64 (ClassReg, 16);
            E->BaseClass  = (UINT8)RShiftU64 (ClassReg, 24);
            E->HeaderType = (UINT8)Header;
          }

          if ((Func == 0) && ((Header & HEADER_TYPE_MULTI_FUNCTION) == 0)) {
            break;   // single-function device
          }
        }
      }
    }
  }

  FreePool (Handles);
}

//
// Hex editor callbacks: Address = config register offset (0..0xFFF).
//
STATIC
EFI_STATUS
PciHexRead (
  IN  VOID    *Context,
  IN  UINT64  Address,
  IN  UINTN   Width,
  OUT UINT64  *Value
  )
{
  PCI_DEV_ENTRY  *E;

  E = (PCI_DEV_ENTRY *)Context;
  return CfgRead (E->RootBridge, E->Bus, E->Dev, E->Func, (UINT32)Address, Width, Value);
}

STATIC
EFI_STATUS
PciHexWrite (
  IN VOID    *Context,
  IN UINT64  Address,
  IN UINTN   Width,
  IN UINT64  Value
  )
{
  PCI_DEV_ENTRY                          *E;
  EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH  W;

  E = (PCI_DEV_ENTRY *)Context;
  W = (Width == 1) ? EfiPciWidthUint8 : ((Width == 2) ? EfiPciWidthUint16 : EfiPciWidthUint32);
  return E->RootBridge->Pci.Write (E->RootBridge, W, PciAddress (E->Bus, E->Dev, E->Func, (UINT32)Address), 1, &Value);
}

STATIC
VOID
OpenDevice (
  IN PCI_DEV_ENTRY  *E
  )
{
  HEX_EDIT_SPACE  Space;
  CHAR16          Title[128];
  UINT64          Probe;
  UINT64          Limit;

  //
  // Extended config space is only usable when offset 0x100 is reachable.
  //
  Limit = 0xFF;
  if (!EFI_ERROR (CfgRead (E->RootBridge, E->Bus, E->Dev, E->Func, 0x100, 4, &Probe)) &&
      ((UINT32)Probe != 0xFFFFFFFF))
  {
    Limit = 0xFFF;
  }

  UnicodeSPrint (
    Title,
    sizeof (Title),
    L"PCI %04x:%02x:%02x.%x  %04x:%04x  %s  (config space %s)",
    E->Segment,
    E->Bus,
    E->Dev,
    E->Func,
    E->VendorId,
    E->DeviceId,
    ClassName (E->BaseClass, E->SubClass),
    (Limit == 0xFFF) ? L"4 KB" : L"256 B"
    );

  ZeroMem (&Space, sizeof (Space));
  Space.Title         = Title;
  Space.Context       = E;
  Space.Read          = PciHexRead;
  Space.Write         = PciHexWrite;
  Space.Base          = 0;
  Space.Limit         = Limit;
  Space.AllowGoto     = (BOOLEAN)(Limit == 0xFFF);
  Space.ConfirmWrites = TRUE;
  Space.AddrDigits    = 3;
  HexEditRun (&Space);
}

VOID
PciViewRun (
  VOID
  )
{
  UINTN          Sel;
  UINTN          Top;
  UINTN          Visible;
  UINTN          Index;
  UINTN          Row;
  EFI_INPUT_KEY  Key;
  PCI_DEV_ENTRY  *E;
  BOOLEAN        Redraw;

  ScanPci ();
  Sel    = 0;
  Top    = 0;
  Redraw = TRUE;

  for ( ; ;) {
    Visible = gScreenRows - 6;
    if (Redraw) {
      UiFrame (L"PCI Devices", L"Up/Down/PgUp/PgDn: select   Enter: config space   F5: rescan   Esc: back");
      UiPrintAt (1, 1, ATTR_HEADER, L"%d device(s) found", mDevCount);
      UiPrintAt (1, 2, ATTR_HEADER, L" Seg  Bus Dev Fn  Vendor Device  Class     Type");
      Redraw = FALSE;
    }

    if (mDevCount == 0) {
      UiPrintAt (2, 4, ATTR_WARN, L"No PCI root bridge / devices found.");
    }

    if (Sel < Top) {
      Top = Sel;
    } else if (Sel >= Top + Visible) {
      Top = Sel - Visible + 1;
    }

    for (Row = 0; Row < Visible; Row++) {
      Index = Top + Row;
      UiFillLine (3 + Row, ATTR_NORMAL);
      if (Index >= mDevCount) {
        continue;
      }

      E = &mDevs[Index];
      UiPrintAt (
        1,
        3 + Row,
        (Index == Sel) ? ATTR_HILITE : ATTR_NORMAL,
        L" %04x  %02x  %02x  %x   %04x   %04x    %02x%02x%02x  %-28s",
        E->Segment,
        E->Bus,
        E->Dev,
        E->Func,
        E->VendorId,
        E->DeviceId,
        E->BaseClass,
        E->SubClass,
        E->ProgIf,
        ClassName (E->BaseClass, E->SubClass)
        );
    }

    UiReadKey (&Key);
    switch (Key.ScanCode) {
      case SCAN_ESC:
        return;
      case SCAN_UP:
        if (Sel > 0) {
          Sel--;
        }

        break;
      case SCAN_DOWN:
        if (Sel + 1 < mDevCount) {
          Sel++;
        }

        break;
      case SCAN_PAGE_UP:
        Sel = (Sel > Visible) ? Sel - Visible : 0;
        break;
      case SCAN_PAGE_DOWN:
        if (mDevCount > 0) {
          Sel = (Sel + Visible < mDevCount) ? Sel + Visible : mDevCount - 1;
        }

        break;
      case SCAN_HOME:
        Sel = 0;
        break;
      case SCAN_END:
        Sel = (mDevCount > 0) ? mDevCount - 1 : 0;
        break;
      case SCAN_F5:
        ScanPci ();
        Sel    = 0;
        Top    = 0;
        Redraw = TRUE;
        break;
      default:
        break;
    }

    if ((Key.UnicodeChar == CHAR_CARRIAGE_RETURN) && (mDevCount > 0)) {
      OpenDevice (&mDevs[Sel]);
      Redraw = TRUE;
    }
  }
}
