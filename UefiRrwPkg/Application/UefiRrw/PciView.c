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
  Space.ConfirmWrites = FALSE;
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


//
// ---- Capability / header decode (F4) ----------------------------------------------
//

STATIC CONST CHAR16  *mCapNames[] = {
  NULL, L"Power Management", L"AGP", L"VPD", L"Slot ID", L"MSI", L"CompactPCI Hot Swap",
  L"PCI-X", L"HyperTransport", L"Vendor Specific", L"Debug Port", L"CompactPCI Resource",
  L"PCI Hot-Plug", L"Bridge Subsystem ID", L"AGP 8x", L"Secure Device", L"PCI Express",
  L"MSI-X", L"SATA Config", L"Advanced Features", L"Enhanced Allocation", L"Flattening Portal Bridge",
};

STATIC CONST CHAR16  *mExtCapNames[] = {
  NULL, L"Advanced Error Reporting", L"Virtual Channel", L"Device Serial Number", L"Power Budgeting",
  L"RC Link Declaration", L"RC Internal Link Control", L"RC Event Collector Assoc", L"MFVC",
  L"Virtual Channel (MFVC)", L"RCRB Header", L"Vendor Specific (VSEC)", L"Config Access Correlation",
  L"Access Control Services", L"ARI", L"ATS", L"SR-IOV", L"MR-IOV", L"Multicast", L"Page Request",
  L"Reserved (AMD)", L"Resizable BAR", L"Dynamic Power Allocation", L"TPH Requester", L"LTR",
  L"Secondary PCIe", L"PMUX", L"PASID", L"LN Requester", L"DPC", L"L1 PM Substates", L"PTM",
  L"M-PCIe", L"FRS Queueing", L"Readiness Time Reporting", L"DVSEC", L"VF Resizable BAR",
  L"Data Link Feature", L"Physical Layer 16.0 GT/s", L"Lane Margining at Receiver", L"Hierarchy ID",
  L"NPEM", L"Physical Layer 32.0 GT/s", L"Alternate Protocol", L"SFI", L"Shadow Functions", L"DOE",
  L"Device 3", L"IDE", L"Physical Layer 64.0 GT/s", L"Flit Logging", L"Flit Perf Measurement",
  L"Flit Error Injection",
};

STATIC CONST CHAR16  *mSpeedNames[] = { L"?", L"2.5 GT/s", L"5.0 GT/s", L"8.0 GT/s", L"16.0 GT/s", L"32.0 GT/s", L"64.0 GT/s" };
STATIC CONST CHAR16  *mPins[]       = { L"INTA", L"INTB", L"INTC", L"INTD" };
STATIC CONST CHAR16  *mAspmNames[]  = { L"none", L"L0s", L"L1", L"L0s+L1" };

STATIC
UINT32
Rd (
  IN PCI_DEV_ENTRY  *E,
  IN UINT32         Reg,
  IN UINTN          Width
  )
{
  UINT64  Val;

  if (EFI_ERROR (CfgRead (E->RootBridge, E->Bus, E->Dev, E->Func, Reg, Width, &Val))) {
    return (Width == 4) ? 0xFFFFFFFF : ((Width == 2) ? 0xFFFF : 0xFF);
  }

  return (UINT32)Val;
}

STATIC
CONST CHAR16 *
SpeedName (
  IN UINTN  Code
  )
{
  return (Code < ARRAY_SIZE (mSpeedNames)) ? mSpeedNames[Code] : L"?";
}

STATIC
VOID
AddBits (
  IN TEXT_DOC      *Doc,
  IN CONST CHAR16  *Label,
  IN UINT32        Value,
  IN CONST CHAR16  **Names,
  IN UINTN         Count
  )
{
  CHAR16  Buf[256];
  UINTN   Pos;
  UINTN   Bit;

  Pos = UnicodeSPrint (Buf, sizeof (Buf), L"    %-22s %08X", Label, Value);
  for (Bit = 0; Bit < Count; Bit++) {
    if ((Names[Bit] != NULL) && ((Value & (1u << Bit)) != 0) && (Pos < 200)) {
      Pos += UnicodeSPrint (Buf + Pos, sizeof (Buf) - Pos * sizeof (CHAR16), L"  %s", Names[Bit]);
    }
  }

  DocAdd (Doc, ((Value != 0) && (Count > 0)) ? ATTR_CHANGED : ATTR_NORMAL, L"%s", Buf);
}

STATIC CONST CHAR16  *mUeNames[32] = {
  NULL, NULL, NULL, NULL, L"DataLinkProtocol", L"SurpriseDown", NULL, NULL, NULL, NULL, NULL, NULL,
  L"PoisonedTLP", L"FlowCtlProtocol", L"CompletionTimeout", L"CompleterAbort", L"UnexpectedCompletion",
  L"ReceiverOverflow", L"MalformedTLP", L"ECRC", L"UnsupportedRequest", L"ACSViolation",
  L"UncorrInternal", L"MCBlockedTLP", L"AtomicOpEgressBlocked", L"TLPPrefixBlocked", L"PoisonedTLPEgress",
};

STATIC CONST CHAR16  *mCeNames[16] = {
  L"ReceiverError", NULL, NULL, NULL, NULL, NULL, L"BadTLP", L"BadDLLP", L"ReplayNumRollover",
  NULL, NULL, NULL, L"ReplayTimerTimeout", L"AdvisoryNonFatal", L"CorrInternal", L"HeaderLogOverflow",
};

STATIC
VOID
DecodePcie (
  IN TEXT_DOC       *Doc,
  IN PCI_DEV_ENTRY  *E,
  IN UINT32         P
  )
{
  UINT32  Cap;
  UINT32  DevCap;
  UINT32  DevCtl;
  UINT32  DevSta;
  UINT32  LnkCap;
  UINT32  LnkCtl;
  UINT32  LnkSta;
  UINT32  SltCap;
  UINT32  SltSta;
  UINT32  LnkCap2;
  UINT32  LnkCtl2;
  UINT32  DevCap2;
  UINT32  DevCtl2;
  UINTN   Type;
  UINTN   Bit;
  CHAR16  Speeds[80];
  UINTN   Pos;

  Cap     = Rd (E, P + 2, 2);
  DevCap  = Rd (E, P + 4, 4);
  DevCtl  = Rd (E, P + 8, 2);
  DevSta  = Rd (E, P + 0x0A, 2);
  LnkCap  = Rd (E, P + 0x0C, 4);
  LnkCtl  = Rd (E, P + 0x10, 2);
  LnkSta  = Rd (E, P + 0x12, 2);
  Type    = (Cap >> 4) & 0x0F;

  DocAdd (
    Doc,
    ATTR_NORMAL,
    L"    Version %d, %s%s",
    Cap & 0x0F,
    ((Type < ARRAY_SIZE (mPcieTypes)) && (mPcieTypes[Type] != NULL)) ? mPcieTypes[Type] : L"Unknown type",
    (Cap & BIT8) ? L", slot implemented" : L""
    );
  DocAdd (
    Doc,
    ATTR_NORMAL,
    L"    DevCap  %08X  MaxPayload supported %d B%s",
    DevCap,
    128 << (DevCap & 7),
    (DevCap & BIT28) ? L", FLR" : L""
    );
  DocAdd (
    Doc,
    ATTR_NORMAL,
    L"    DevCtl  %04X      MaxPayload %d B, MaxReadReq %d B%s%s",
    DevCtl,
    128 << ((DevCtl >> 5) & 7),
    128 << ((DevCtl >> 12) & 7),
    (DevCtl & BIT4) ? L", RelaxedOrd" : L"",
    (DevCtl & BIT11) ? L", NoSnoop" : L""
    );
  DocAdd (
    Doc,
    (DevSta & 0x0F) ? ATTR_CHANGED : ATTR_NORMAL,
    L"    DevSta  %04X      %s%s%s%s%s",
    DevSta,
    (DevSta & BIT0) ? L"CorrErr " : L"",
    (DevSta & BIT1) ? L"NonFatalErr " : L"",
    (DevSta & BIT2) ? L"FatalErr " : L"",
    (DevSta & BIT3) ? L"UnsuppReq " : L"",
    (DevSta & 0x0F) ? L"" : L"no errors"
    );

  if ((Type != 9) && (Type != 0x0A)) {
    DocAdd (
      Doc,
      ATTR_NORMAL,
      L"    LnkCap  %08X  Port %d, max %s x%d, ASPM %s",
      LnkCap,
      LnkCap >> 24,
      SpeedName (LnkCap & 0x0F),
      (LnkCap >> 4) & 0x3F,
      mAspmNames[(LnkCap >> 10) & 3]
      );
    DocAdd (
      Doc,
      ATTR_NORMAL,
      L"    LnkCtl  %04X      ASPM %s%s%s",
      LnkCtl,
      mAspmNames[LnkCtl & 3],
      (LnkCtl & BIT4) ? L", LINK DISABLED" : L"",
      (LnkCtl & BIT6) ? L", common clock" : L""
      );
    DocAdd (
      Doc,
      ATTR_HEADER,
      L"    LnkSta  %04X      %s x%d%s%s%s",
      LnkSta,
      ((LnkSta >> 4) & 0x3F) ? SpeedName (LnkSta & 0x0F) : L"link down",
      (LnkSta >> 4) & 0x3F,
      (LnkSta & BIT11) ? L", training" : L"",
      (LnkSta & BIT12) ? L", slot clock" : L"",
      (LnkSta & BIT13) ? L", DL active" : L""
      );
  }

  if (Cap & BIT8) {
    SltCap = Rd (E, P + 0x14, 4);
    SltSta = Rd (E, P + 0x1A, 2);
    DocAdd (
      Doc,
      ATTR_NORMAL,
      L"    SltCap  %08X  Slot #%d%s, SltSta %04X: %s",
      SltCap,
      SltCap >> 19,
      (SltCap & BIT6) ? L", hot-plug capable" : L"",
      SltSta,
      (SltSta & BIT6) ? L"card present" : L"empty"
      );
  }

  if ((Cap & 0x0F) >= 2) {
    DevCap2 = Rd (E, P + 0x24, 4);
    DevCtl2 = Rd (E, P + 0x28, 2);
    LnkCap2 = Rd (E, P + 0x2C, 4);
    LnkCtl2 = Rd (E, P + 0x30, 2);
    DocAdd (
      Doc,
      ATTR_NORMAL,
      L"    DevCap2 %08X  LTR %s, DevCtl2 %04X: LTR %s",
      DevCap2,
      (DevCap2 & BIT11) ? L"supported" : L"no",
      DevCtl2,
      (DevCtl2 & BIT10) ? L"enabled" : L"off"
      );
    if ((Type != 9) && (Type != 0x0A) && (LnkCap2 != 0)) {
      Pos       = 0;
      Speeds[0] = L'\0';
      for (Bit = 1; Bit <= 6; Bit++) {
        if (LnkCap2 & (1u << Bit)) {
          Pos += UnicodeSPrint (Speeds + Pos, sizeof (Speeds) - Pos * sizeof (CHAR16), L" %s", SpeedName (Bit));
        }
      }

      DocAdd (Doc, ATTR_NORMAL, L"    LnkCap2 %08X  speeds:%s", LnkCap2, Speeds);
      DocAdd (Doc, ATTR_NORMAL, L"    LnkCtl2 %04X      target %s", LnkCtl2, SpeedName (LnkCtl2 & 0x0F));
    }
  }
}

STATIC
VOID
DecodeCap (
  IN TEXT_DOC       *Doc,
  IN PCI_DEV_ENTRY  *E,
  IN UINT8          Id,
  IN UINT32         P
  )
{
  UINT32  V;
  UINT32  Ctl;
  STATIC CONST CHAR16  *DStates[] = { L"D0", L"D1", L"D2", L"D3hot" };

  switch (Id) {
    case 0x01:
      V   = Rd (E, P + 2, 2);
      Ctl = Rd (E, P + 4, 2);
      DocAdd (
        Doc,
        ATTR_NORMAL,
        L"    PMC %04X: version %d%s%s, PME from %s%s%s%s%s%s",
        V,
        V & 7,
        (V & BIT9) ? L", D1" : L"",
        (V & BIT10) ? L", D2" : L"",
        (V & BIT11) ? L"D0 " : L"",
        (V & BIT12) ? L"D1 " : L"",
        (V & BIT13) ? L"D2 " : L"",
        (V & BIT14) ? L"D3hot " : L"",
        (V & BIT15) ? L"D3cold" : L"",
        (V & 0xF800) ? L"" : L"none"
        );
      DocAdd (
        Doc,
        ATTR_HEADER,
        L"    PMCSR %04X: state %s%s%s%s",
        Ctl,
        DStates[Ctl & 3],
        (Ctl & BIT3) ? L", NoSoftReset" : L"",
        (Ctl & BIT8) ? L", PME enabled" : L"",
        (Ctl & BIT15) ? L", PME status" : L""
        );
      break;
    case 0x05:
      Ctl = Rd (E, P + 2, 2);
      DocAdd (
        Doc,
        (Ctl & BIT0) ? ATTR_HEADER : ATTR_NORMAL,
        L"    Control %04X: %s, %d of %d vectors, %s%s",
        Ctl,
        (Ctl & BIT0) ? L"ENABLED" : L"disabled",
        1 << ((Ctl >> 4) & 7),
        1 << ((Ctl >> 1) & 7),
        (Ctl & BIT7) ? L"64-bit" : L"32-bit",
        (Ctl & BIT8) ? L", per-vector mask" : L""
        );
      if (Ctl & BIT7) {
        DocAdd (Doc, ATTR_NORMAL, L"    Address %08X%08X  Data %04X", Rd (E, P + 8, 4), Rd (E, P + 4, 4), Rd (E, P + 0x0C, 2));
      } else {
        DocAdd (Doc, ATTR_NORMAL, L"    Address %08X  Data %04X", Rd (E, P + 4, 4), Rd (E, P + 8, 2));
      }

      break;
    case 0x11:
      Ctl = Rd (E, P + 2, 2);
      V   = Rd (E, P + 4, 4);
      DocAdd (
        Doc,
        (Ctl & BIT15) ? ATTR_HEADER : ATTR_NORMAL,
        L"    Control %04X: %s, %d vectors%s",
        Ctl,
        (Ctl & BIT15) ? L"ENABLED" : L"disabled",
        (Ctl & 0x7FF) + 1,
        (Ctl & BIT14) ? L", function masked" : L""
        );
      DocAdd (Doc, ATTR_NORMAL, L"    Table BAR%d + %Xh", V & 7, V & ~7u);
      V = Rd (E, P + 8, 4);
      DocAdd (Doc, ATTR_NORMAL, L"    PBA   BAR%d + %Xh", V & 7, V & ~7u);
      break;
    case 0x10:
      DecodePcie (Doc, E, P);
      break;
    case 0x09:
      DocAdd (Doc, ATTR_NORMAL, L"    Length %d bytes", Rd (E, P + 2, 1));
      break;
    case 0x0D:
      V = Rd (E, P + 4, 4);
      DocAdd (Doc, ATTR_NORMAL, L"    Subsystem %04X:%04X", V & 0xFFFF, V >> 16);
      break;
    default:
      DocAdd (Doc, ATTR_DIM, L"    %08X %08X", Rd (E, P, 4), Rd (E, P + 4, 4));
      break;
  }
}

STATIC
VOID
DecodeExtCap (
  IN TEXT_DOC       *Doc,
  IN PCI_DEV_ENTRY  *E,
  IN UINT16         Id,
  IN UINT32         P
  )
{
  UINT32  V;
  UINT32  Ctl;
  UINTN   Count;
  UINTN   Index;
  UINTN   Bit;
  CHAR16  Sizes[160];
  UINTN   Pos;

  switch (Id) {
    case 0x0001:
      AddBits (Doc, L"Uncorrectable status", Rd (E, P + 4, 4), mUeNames, 32);
      AddBits (Doc, L"Uncorrectable mask", Rd (E, P + 8, 4), NULL, 0);
      AddBits (Doc, L"Uncorrectable severity", Rd (E, P + 0x0C, 4), NULL, 0);
      AddBits (Doc, L"Correctable status", Rd (E, P + 0x10, 4), mCeNames, 16);
      AddBits (Doc, L"Correctable mask", Rd (E, P + 0x14, 4), NULL, 0);
      AddBits (Doc, L"Cap & control", Rd (E, P + 0x18, 4), NULL, 0);
      break;
    case 0x0003:
      DocAdd (Doc, ATTR_HEADER, L"    Serial number %08X%08X", Rd (E, P + 8, 4), Rd (E, P + 4, 4));
      break;
    case 0x000B:
      V = Rd (E, P + 4, 4);
      DocAdd (Doc, ATTR_NORMAL, L"    VSEC ID %04X rev %d, length %d", V & 0xFFFF, (V >> 16) & 0xF, V >> 20);
      break;
    case 0x0023:
      V = Rd (E, P + 4, 4);
      DocAdd (Doc, ATTR_NORMAL, L"    DVSEC vendor %04X rev %d, length %d, ID %04X", V & 0xFFFF, (V >> 16) & 0xF, V >> 20, Rd (E, P + 8, 2));
      break;
    case 0x0010:
      Ctl = Rd (E, P + 8, 2);
      DocAdd (
        Doc,
        (Ctl & BIT0) ? ATTR_HEADER : ATTR_NORMAL,
        L"    VF %s, NumVFs %d of %d (initial %d), offset %d stride %d, VF device %04X",
        (Ctl & BIT0) ? L"ENABLED" : L"disabled",
        Rd (E, P + 0x10, 2),
        Rd (E, P + 0x0E, 2),
        Rd (E, P + 0x0C, 2),
        Rd (E, P + 0x14, 2),
        Rd (E, P + 0x16, 2),
        Rd (E, P + 0x1A, 2)
        );
      break;
    case 0x0015:
    case 0x0024:
      Count = (Rd (E, P + 8, 4) >> 5) & 7;
      for (Index = 0; (Index < Count) && (Index < 6); Index++) {
        V   = Rd (E, P + 4 + (UINT32)Index * 8, 4);
        Ctl = Rd (E, P + 8 + (UINT32)Index * 8, 4);
        Pos = 0;
        Sizes[0] = L'\0';
        for (Bit = 4; Bit < 32; Bit++) {
          if ((V & (1u << Bit)) && (Pos < 140)) {
            Pos += UnicodeSPrint (Sizes + Pos, sizeof (Sizes) - Pos * sizeof (CHAR16), (Bit - 4 >= 10) ? L" %dG" : L" %dM", (Bit - 4 >= 10) ? (1u << (Bit - 14)) : (1u << (Bit - 4)));
          }
        }

        DocAdd (
          Doc,
          ATTR_NORMAL,
          L"    BAR%d current %d MB, supported:%s",
          Ctl & 7,
          1u << ((Ctl >> 8) & 0x3F),
          Sizes
          );
      }

      break;
    case 0x0018:
      V = Rd (E, P + 4, 4);
      DocAdd (
        Doc,
        ATTR_NORMAL,
        L"    Max snoop latency %d x 32^%d ns, max no-snoop %d x 32^%d ns",
        V & 0x3FF,
        (V >> 10) & 7,
        (V >> 16) & 0x3FF,
        (V >> 26) & 7
        );
      break;
    case 0x001E:
      V   = Rd (E, P + 4, 4);
      Ctl = Rd (E, P + 8, 4);
      DocAdd (
        Doc,
        ATTR_NORMAL,
        L"    Supported:%s%s%s%s   Enabled:%s%s%s%s%s",
        (V & BIT0) ? L" PCI-PM_L1.2" : L"",
        (V & BIT1) ? L" PCI-PM_L1.1" : L"",
        (V & BIT2) ? L" ASPM_L1.2" : L"",
        (V & BIT3) ? L" ASPM_L1.1" : L"",
        (Ctl & BIT0) ? L" PCI-PM_L1.2" : L"",
        (Ctl & BIT1) ? L" PCI-PM_L1.1" : L"",
        (Ctl & BIT2) ? L" ASPM_L1.2" : L"",
        (Ctl & BIT3) ? L" ASPM_L1.1" : L"",
        (Ctl & 0x0F) ? L"" : L" none"
        );
      break;
    case 0x000E:
      V = Rd (E, P + 4, 2);
      DocAdd (Doc, ATTR_NORMAL, L"    Next function number %d", (V >> 8) & 0xFF);
      break;
    case 0x0019:
      AddBits (Doc, L"Lane error status", Rd (E, P + 8, 4), NULL, 0);
      break;
    default:
      DocAdd (Doc, ATTR_DIM, L"    %08X %08X %08X", Rd (E, P + 4, 4), Rd (E, P + 8, 4), Rd (E, P + 0x0C, 4));
      break;
  }
}

STATIC
VOID
AddBars (
  IN TEXT_DOC       *Doc,
  IN PCI_DEV_ENTRY  *E,
  IN UINTN          Count
  )
{
  UINTN   Index;
  UINT32  Lo;
  UINT32  Hi;

  for (Index = 0; Index < Count; Index++) {
    Lo = Rd (E, 0x10 + (UINT32)Index * 4, 4);
    if (Lo == 0) {
      DocAdd (Doc, ATTR_DIM, L"  BAR%d  unused", Index);
      continue;
    }

    if (Lo & BIT0) {
      DocAdd (Doc, ATTR_NORMAL, L"  BAR%d  IO     %04X", Index, Lo & ~3u);
    } else if (((Lo >> 1) & 3) == 2) {
      Hi = (Index + 1 < Count) ? Rd (E, 0x10 + (UINT32)(Index + 1) * 4, 4) : 0;
      DocAdd (Doc, ATTR_NORMAL, L"  BAR%d  MEM64  %08X%08X%s", Index, Hi, Lo & ~0xFu, (Lo & BIT3) ? L"  prefetchable" : L"");
      Index++;
    } else {
      DocAdd (Doc, ATTR_NORMAL, L"  BAR%d  MEM32  %08X%s", Index, Lo & ~0xFu, (Lo & BIT3) ? L"  prefetchable" : L"");
    }
  }
}

STATIC
VOID
ShowPciInfo (
  IN PCI_DEV_ENTRY  *E
  )
{
  TEXT_DOC  Doc;
  CHAR16    Header[100];
  UINT32    Cmd;
  UINT32    Sts;
  UINT32    V;
  UINT8     Ptr;
  UINT8     Id;
  UINTN     Guard;
  UINT32    Ext;
  UINT32    ExtPtr;
  UINT16    ExtId;
  UINTN     Type;

  DocInit (&Doc);
  Type = E->HeaderType & 0x7F;
  Cmd  = Rd (E, 4, 2);
  Sts  = Rd (E, 6, 2);
  DocAdd (&Doc, ATTR_HEADER, L"Header");
  DocAdd (&Doc, ATTR_NORMAL, L"  Vendor:Device  %04X:%04X   Revision %02X   Class %02X%02X%02X %s", E->VendorId, E->DeviceId, Rd (E, 8, 1), E->BaseClass, E->SubClass, E->ProgIf, ClassName (E->BaseClass, E->SubClass));
  DocAdd (&Doc, ATTR_NORMAL, L"  Header type    %02X (%s%s)", E->HeaderType, (Type == 0) ? L"device" : ((Type == 1) ? L"PCI bridge" : L"CardBus bridge"), (E->HeaderType & 0x80) ? L", multi-function" : L"");
  DocAdd (
    &Doc,
    ATTR_NORMAL,
    L"  Command %04X:  %s%s%s%s%s%s",
    Cmd,
    (Cmd & BIT0) ? L"IO " : L"",
    (Cmd & BIT1) ? L"MEM " : L"",
    (Cmd & BIT2) ? L"BusMaster " : L"",
    (Cmd & BIT6) ? L"ParityErr " : L"",
    (Cmd & BIT8) ? L"SERR " : L"",
    (Cmd & BIT10) ? L"INTxDisable" : L""
    );
  DocAdd (
    &Doc,
    (Sts & 0xF900) ? ATTR_CHANGED : ATTR_NORMAL,
    L"  Status  %04X:  %s%s%s%s%s%s%s",
    Sts,
    (Sts & BIT3) ? L"INTx " : L"",
    (Sts & BIT4) ? L"CapList " : L"",
    (Sts & BIT8) ? L"MasterDataParity " : L"",
    (Sts & BIT11) ? L"SigTargetAbort " : L"",
    (Sts & BIT12) ? L"RcvTargetAbort " : L"",
    (Sts & BIT13) ? L"RcvMasterAbort " : L"",
    (Sts & (BIT14 | BIT15)) ? L"SERR/ParityErr" : L""
    );
  V = Rd (E, 0x3C, 2);
  DocAdd (&Doc, ATTR_NORMAL, L"  Interrupt      line %02X, pin %s", V & 0xFF, ((V >> 8) >= 1 && (V >> 8) <= 4) ? mPins[(V >> 8) - 1] : L"none");
  if (Type == 0) {
    V = Rd (E, 0x2C, 4);
    DocAdd (&Doc, ATTR_NORMAL, L"  Subsystem      %04X:%04X", V & 0xFFFF, V >> 16);
  }

  DocAdd (&Doc, ATTR_NORMAL, L"");
  DocAdd (&Doc, ATTR_HEADER, L"Base Address Registers");
  AddBars (&Doc, E, (Type == 0) ? 6 : ((Type == 1) ? 2 : 1));
  if (Type == 1) {
    DocAdd (&Doc, ATTR_NORMAL, L"");
    DocAdd (&Doc, ATTR_HEADER, L"Bridge");
    DocAdd (&Doc, ATTR_NORMAL, L"  Bus  primary %02X  secondary %02X  subordinate %02X", Rd (E, 0x18, 1), Rd (E, 0x19, 1), Rd (E, 0x1A, 1));
    V = Rd (E, 0x1C, 2);
    DocAdd (&Doc, ATTR_NORMAL, L"  IO window       %04X - %04X", (V & 0xF0) << 8, ((V >> 8) & 0xF0) << 8 | 0xFFF);
    V = Rd (E, 0x20, 4);
    DocAdd (&Doc, ATTR_NORMAL, L"  Memory window   %08X - %08X", (V & 0xFFF0) << 16, (V & 0xFFF00000) | 0xFFFFF);
    V = Rd (E, 0x24, 4);
    DocAdd (&Doc, ATTR_NORMAL, L"  Prefetch window %08X%08X - %08X%08X", Rd (E, 0x28, 4), (V & 0xFFF0) << 16, Rd (E, 0x2C, 4), (V & 0xFFF00000) | 0xFFFFF);
    V = Rd (E, 0x3E, 2);
    DocAdd (&Doc, ATTR_NORMAL, L"  Bridge control  %04X%s%s", V, (V & BIT3) ? L"  VGA" : L"", (V & BIT6) ? L"  SECONDARY BUS RESET" : L"");
  }

  DocAdd (&Doc, ATTR_NORMAL, L"");
  DocAdd (&Doc, ATTR_HEADER, L"Capabilities");
  if ((Sts & BIT4) == 0) {
    DocAdd (&Doc, ATTR_DIM, L"  (none)");
  } else {
    Ptr = (UINT8)Rd (E, (Type == 2) ? 0x14 : 0x34, 1) & 0xFC;
    for (Guard = 0; (Ptr >= 0x40) && (Guard < 48); Guard++) {
      V  = Rd (E, Ptr, 2);
      Id = (UINT8)V;
      DocAdd (&Doc, EFI_TEXT_ATTR (EFI_WHITE, EFI_BLUE), L"  [%02X] %02X  %s", Ptr, Id, ((Id < ARRAY_SIZE (mCapNames)) && (mCapNames[Id] != NULL)) ? mCapNames[Id] : L"Unknown");
      DecodeCap (&Doc, E, Id, Ptr);
      Ptr = (UINT8)(V >> 8) & 0xFC;
    }
  }

  Ext = Rd (E, 0x100, 4);
  if ((Ext != 0xFFFFFFFF) && (Ext != 0) && (E->PcieType != 0xFF)) {
    DocAdd (&Doc, ATTR_NORMAL, L"");
    DocAdd (&Doc, ATTR_HEADER, L"Extended Capabilities");
    ExtPtr = 0x100;
    for (Guard = 0; (ExtPtr >= 0x100) && (ExtPtr < 0x1000) && (Guard < 64); Guard++) {
      Ext   = Rd (E, ExtPtr, 4);
      if ((Ext == 0) || (Ext == 0xFFFFFFFF)) {
        break;
      }

      ExtId = (UINT16)Ext;
      DocAdd (&Doc, EFI_TEXT_ATTR (EFI_WHITE, EFI_BLUE), L"  [%03X] %04X v%d  %s", ExtPtr, ExtId, (Ext >> 16) & 0xF, ((ExtId < ARRAY_SIZE (mExtCapNames)) && (mExtCapNames[ExtId] != NULL)) ? mExtCapNames[ExtId] : L"Unknown");
      DecodeExtCap (&Doc, E, ExtId, ExtPtr);
      ExtPtr = (Ext >> 20) & 0xFFC;
    }
  }

  UnicodeSPrint (Header, sizeof (Header), L"PCI %04X:%02X:%02X.%X  %04X:%04X  %s", E->Segment, E->Bus, E->Dev, E->Func, E->VendorId, E->DeviceId, ClassName (E->BaseClass, E->SubClass));
  DocView (L"PCI Device Info", Header, &Doc);
  DocFree (&Doc);
}

STATIC CONST CHAR16  *mPciHelp[] = {
  L"Up/Down       Select a device",
  L"PgUp/PgDn     Page up / down",
  L"Home/End      First / last device",
  L"Enter         Config space editor (256 B / 4 KB)",
  L"F4            Device info: header, BARs, capabilities decoded",
  L"F5            Rescan the PCI bus",
  L"F6            Switch list view / PCIe tree view"
};

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
  INTN      Tag;
  CHAR16    Header[64];

  Sel = 0;
  for ( ; ;) {
    BuildPciTree (&Doc);
    UnicodeSPrint (Header, sizeof (Header), L"%d device(s), %d root bridge(s)", mDevCount, mRootCount);
    UiSetHelp (mPciHelp, ARRAY_SIZE (mPciHelp));
    Pick = DocSelect (
             L"PCI Tree",
             L"Enter:config  F4:info  F5:rescan  F6:list  F1:help  Esc:back",
             Header,
             &Doc,
             &Sel
             );
    Tag = ((Pick == -4) && (Sel < Doc.Count)) ? Doc.Lines[Sel].Tag : -1;
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

    if (Pick == -4) {
      if (Tag >= 0) {
        ShowPciInfo (&mDevs[Tag]);
      }

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
      UiFrame (L"PCI Devices", L"Enter:config  F4:info  F5:rescan  F6:tree  F1:help  Esc:back");
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

    UiSetHelp (mPciHelp, ARRAY_SIZE (mPciHelp));
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
      case SCAN_F4:
        if (mDevCount > 0) {
          ShowPciInfo (&mDevs[Sel]);
          Redraw = TRUE;
        }

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

/**
  File > Dump All: device list, PCIe tree and every device's info page.
**/
VOID
PciDump (
  IN OUT TEXT_DOC  *Out
  )
{
  TEXT_DOC       Tree;
  UINTN          Index;
  PCI_DEV_ENTRY  *E;
  TEXT_DOC       *Saved;

  ScanPci ();
  DocAdd (Out, ATTR_NORMAL, L"==== PCI Devices (%d) ====", mDevCount);
  DocAdd (Out, ATTR_NORMAL, L" Seg  Bus Dev Fn  Vendor Device  Class   Type");
  for (Index = 0; Index < mDevCount; Index++) {
    E = &mDevs[Index];
    DocAdd (
      Out,
      ATTR_NORMAL,
      L" %04X  %02X  %02X  %X   %04X   %04X    %02X%02X%02X  %s",
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

  DocAdd (Out, ATTR_NORMAL, L"");
  DocAdd (Out, ATTR_NORMAL, L"==== PCIe Tree ====");
  BuildPciTree (&Tree);
  for (Index = 0; Index < Tree.Count; Index++) {
    DocAdd (Out, ATTR_NORMAL, L"%s", Tree.Lines[Index].Text);
  }

  DocFree (&Tree);
  DocAdd (Out, ATTR_NORMAL, L"");
  Saved       = gDocCapture;
  gDocCapture = Out;
  for (Index = 0; Index < mDevCount; Index++) {
    DocAdd (Out, ATTR_NORMAL, L"---- %04X:%02X:%02X.%X ----", mDevs[Index].Segment, mDevs[Index].Bus, mDevs[Index].Dev, mDevs[Index].Func);
    ShowPciInfo (&mDevs[Index]);
  }

  gDocCapture = Saved;
}
