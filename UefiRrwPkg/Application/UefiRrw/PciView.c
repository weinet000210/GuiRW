/** @file
  PCI device list and configuration space editor
  (via EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL, so segments and ECAM work).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "UefiRrw.h"
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
  UINT8                              SecBus;       // bridges only
  UINT8                              SubBus;
  UINT8                              PcieType;     // 0xFF: no PCIe capability
  UINT8                              LinkSpeed;    // LnkSta, 0 = none
  UINT8                              LinkWidth;
  UINT8                              MaxSpeed;     // LnkCap
  UINT8                              MaxWidth;
  UINTN                              RootIndex;
} PCI_DEV_ENTRY;

typedef struct {
  EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL    *Rb;
  UINT32                             Segment;
  UINTN                              BusMin;
  UINTN                              BusMax;
} PCI_ROOT_ENTRY;

#define MAX_PCI_ROOTS  32

STATIC PCI_ROOT_ENTRY  mRoots[MAX_PCI_ROOTS];
STATIC UINTN           mRootCount;

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

/**
  Bridge bus numbers and PCI Express port type / link state.
**/
STATIC
VOID
ProbeExtra (
  IN OUT PCI_DEV_ENTRY  *E
  )
{
  UINT64  Val;
  UINT8   Ptr;
  UINTN   Guard;
  UINT16  Lnk;

  E->PcieType  = 0xFF;
  E->SecBus    = 0;
  E->SubBus    = 0;
  E->LinkSpeed = 0;
  E->LinkWidth = 0;
  E->MaxSpeed  = 0;
  E->MaxWidth  = 0;
  if (((E->HeaderType & 0x7F) == 1) || ((E->HeaderType & 0x7F) == 2)) {
    CfgRead (E->RootBridge, E->Bus, E->Dev, E->Func, 0x19, 1, &Val);
    E->SecBus = (UINT8)Val;
    CfgRead (E->RootBridge, E->Bus, E->Dev, E->Func, 0x1A, 1, &Val);
    E->SubBus = (UINT8)Val;
  }

  CfgRead (E->RootBridge, E->Bus, E->Dev, E->Func, PCI_PRIMARY_STATUS_OFFSET, 2, &Val);
  if ((Val & EFI_PCI_STATUS_CAPABILITY) == 0) {
    return;
  }

  CfgRead (E->RootBridge, E->Bus, E->Dev, E->Func, ((E->HeaderType & 0x7F) == 2) ? 0x14 : PCI_CAPBILITY_POINTER_OFFSET, 1, &Val);
  Ptr = (UINT8)Val & 0xFC;
  for (Guard = 0; (Ptr >= 0x40) && (Guard < 48); Guard++) {
    CfgRead (E->RootBridge, E->Bus, E->Dev, E->Func, Ptr, 2, &Val);
    if ((UINT8)Val == EFI_PCI_CAPABILITY_ID_PCIEXP) {
      CfgRead (E->RootBridge, E->Bus, E->Dev, E->Func, Ptr + 2, 2, &Val);
      E->PcieType = (UINT8)((Val >> 4) & 0x0F);
      if ((E->PcieType == 9) || (E->PcieType == 0x0A)) {
        return;     // integrated endpoint / event collector: no link
      }

      CfgRead (E->RootBridge, E->Bus, E->Dev, E->Func, Ptr + 0x0C, 2, &Val);
      E->MaxSpeed = (UINT8)(Val & 0x0F);
      E->MaxWidth = (UINT8)((Val >> 4) & 0x3F);
      CfgRead (E->RootBridge, E->Bus, E->Dev, E->Func, Ptr + 0x12, 2, &Val);
      Lnk          = (UINT16)Val;
      E->LinkSpeed = (UINT8)(Lnk & 0x0F);
      E->LinkWidth = (UINT8)((Lnk >> 4) & 0x3F);
      return;
    }

    Ptr = (UINT8)(Val >> 8) & 0xFC;
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

  mDevCount  = 0;
  mRootCount = 0;
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
    if (mRootCount < MAX_PCI_ROOTS) {
      mRoots[mRootCount].Rb      = Rb;
      mRoots[mRootCount].Segment = Rb->SegmentNumber;
      mRoots[mRootCount].BusMin  = BusMin;
      mRoots[mRootCount].BusMax  = BusMax;
      mRootCount++;
    }

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
            E->RootIndex  = mRootCount - 1;
            ProbeExtra (E);
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

STATIC CONST CHAR16  *mPcieTypes[] = {
  L"Endpoint", L"Legacy Endpoint", NULL, NULL, L"Root Port", L"Switch Upstream",
  L"Switch Downstream", L"PCIe-to-PCI Bridge", L"PCI-to-PCIe Bridge",
  L"RC Integrated Endpoint", L"RC Event Collector",
};

STATIC
BOOLEAN
IsBridge (
  IN PCI_DEV_ENTRY  *E
  )
{
  return (BOOLEAN)((((E->HeaderType & 0x7F) == 1) || ((E->HeaderType & 0x7F) == 2)) && (E->SecBus > E->Bus));
}

/**
  TRUE when some bridge under the same root bridge forwards to Bus.
**/
STATIC
BOOLEAN
BusIsSecondary (
  IN UINTN  Root,
  IN UINT8  Bus
  )
{
  UINTN  Index;

  for (Index = 0; Index < mDevCount; Index++) {
    if ((mDevs[Index].RootIndex == Root) && IsBridge (&mDevs[Index]) && (mDevs[Index].SecBus == Bus)) {
      return TRUE;
    }
  }

  return FALSE;
}

STATIC
VOID
AddTreeNode (
  IN TEXT_DOC  *Doc,
  IN UINTN     Index,
  IN UINTN     Depth
  )
{
  PCI_DEV_ENTRY  *E;
  CHAR16         Link[48];
  CHAR16         Range[24];
  CONST CHAR16   *Type;
  UINTN          Child;

  E       = &mDevs[Index];
  Link[0] = L'\0';
  if ((E->PcieType != 0xFF) && (E->MaxWidth != 0)) {
    if (E->LinkWidth == 0) {
      UnicodeSPrint (Link, sizeof (Link), L"  link down (max Gen%d x%d)", E->MaxSpeed, E->MaxWidth);
    } else {
      UnicodeSPrint (Link, sizeof (Link), L"  Gen%d x%d (max Gen%d x%d)", E->LinkSpeed, E->LinkWidth, E->MaxSpeed, E->MaxWidth);
    }
  }

  Range[0] = L'\0';
  if (IsBridge (E)) {
    UnicodeSPrint (Range, sizeof (Range), L"  Bus %02X-%02X", E->SecBus, E->SubBus);
  }

  Type = NULL;
  if ((E->PcieType != 0xFF) && (E->PcieType < ARRAY_SIZE (mPcieTypes))) {
    Type = mPcieTypes[E->PcieType];
  }

  DocAddItem (
    Doc,
    (INTN)Index,
    L"%*s%s %02X:%02X.%X  %04X:%04X  %s%s%s%s%s%s",
    2 + Depth * 4,
    L"",
    IsBridge (E) ? L"[-]" : L" - ",
    E->Bus,
    E->Dev,
    E->Func,
    E->VendorId,
    E->DeviceId,
    ClassName (E->BaseClass, E->SubClass),
    (Type != NULL) ? L"  [" : L"",
    (Type != NULL) ? Type : L"",
    (Type != NULL) ? L"]" : L"",
    Range,
    Link
    );

  if (!IsBridge (E) || (Depth >= 16)) {
    return;
  }

  for (Child = 0; Child < mDevCount; Child++) {
    if ((Child != Index) && (mDevs[Child].RootIndex == E->RootIndex) && (mDevs[Child].Bus == E->SecBus)) {
      AddTreeNode (Doc, Child, Depth + 1);
    }
  }
}

STATIC
VOID
BuildPciTree (
  OUT TEXT_DOC  *Doc
  )
{
  UINTN  Root;
  UINTN  Index;

  DocInit (Doc);
  for (Root = 0; Root < mRootCount; Root++) {
    DocAdd (
      Doc,
      ATTR_HEADER,
      L" PCI Root Bridge  Segment %04X  Bus %02X-%02X",
      mRoots[Root].Segment,
      mRoots[Root].BusMin,
      mRoots[Root].BusMax
      );
    for (Index = 0; Index < mDevCount; Index++) {
      if ((mDevs[Index].RootIndex == Root) && !BusIsSecondary (Root, mDevs[Index].Bus)) {
        AddTreeNode (Doc, Index, 0);
      }
    }

    DocAdd (Doc, ATTR_NORMAL, L"");
  }
}

/**
  @retval TRUE   switch back to the list (F6)
  @retval FALSE  leave the PCI screen (Esc)
**/
STATIC
BOOLEAN
PciTreeRun (
  VOID
  )
{
  TEXT_DOC  Doc;
  UINTN     Sel;
  INTN      Pick;
  CHAR16    Header[64];

  Sel = 0;
  for ( ; ;) {
    BuildPciTree (&Doc);
    UnicodeSPrint (Header, sizeof (Header), L"%d device(s), %d root bridge(s)", mDevCount, mRootCount);
    Pick = DocSelect (
             L"PCI Tree",
             L"Up/Down/PgUp/PgDn: select   Enter: config space   F5: rescan   F6: list view   Esc: back",
             Header,
             &Doc,
             &Sel
             );
    DocFree (&Doc);
    if (Pick == -1) {
      return FALSE;
    }

    if (Pick == -3) {
      return TRUE;
    }

    if (Pick == -2) {
      ScanPci ();
      Sel = 0;
      continue;
    }

    OpenDevice (&mDevs[Pick]);
  }
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
      UiFrame (L"PCI Devices", L"Up/Down/PgUp/PgDn: select   Enter: config space   F5: rescan   F6: tree view   Esc: back");
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
      case SCAN_F6:
        if (!PciTreeRun ()) {
          return;
        }

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
