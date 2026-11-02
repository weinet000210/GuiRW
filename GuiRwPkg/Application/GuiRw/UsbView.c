/** @file
  USB viewer: a tree of host controllers, root hub ports, hubs and devices,
  built from the USB I/O handles the firmware's USB bus driver created and
  their device paths. Each device opens a page with its connection info,
  device descriptor, the full configuration descriptor set (interfaces,
  endpoints, HID, IAD, SuperSpeed companion), hub port status, and raw data.

  Only standard read requests (GET_DESCRIPTOR, hub GET_STATUS) are issued.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "GuiRw.h"
#include <Protocol/UsbIo.h>
#include <Protocol/Usb2HostController.h>
#include <Protocol/PciIo.h>
#include <Protocol/DevicePath.h>
#include <Library/DevicePathLib.h>
#include <IndustryStandard/Pci.h>

#define MAX_HC        16
#define MAX_DEV       128
#define MAX_DEPTH     8
#define HC_TAG_BASE   10000
#define SPEED_UNKNOWN 0xFF
#define USB_TIMEOUT   1000

typedef struct {
  EFI_HANDLE                  Handle;
  EFI_USB2_HC_PROTOCOL        *Hc;
  EFI_DEVICE_PATH_PROTOCOL    *Dp;
  UINTN                       DpSize;   // without the end node
  CHAR16                      Name[48];
  CHAR16                      Pci[48];
  UINT8                       MaxSpeed;
  UINT8                       Ports;
} USB_HC;

typedef struct {
  UINTN                       Hc;
  UINTN                       Depth;
  UINT8                       Port[MAX_DEPTH];  // 0-based port per tier
  EFI_HANDLE                  Handle;
  EFI_USB_IO_PROTOCOL         *UsbIo;
  EFI_DEVICE_PATH_PROTOCOL    *Dp;
  UINT8                       Desc[18];
  UINTN                       Interfaces;
  UINT8                       Speed;
  UINT8                       HubPorts;
  BOOLEAN                     IsHub;
  BOOLEAN                     SsHub;
  UINT8                       IfClass;   // class of the first interface
  CHAR16                      Product[64];
} USB_DEV;

STATIC USB_HC   mHc[MAX_HC];
STATIC UINTN    mHcCount;
STATIC USB_DEV  mDev[MAX_DEV];
STATIC UINTN    mDevCount;

STATIC CONST CHAR16  *mSpeedNames[] = { L"Full Speed (12 Mbps)", L"Low Speed (1.5 Mbps)", L"High Speed (480 Mbps)", L"SuperSpeed (5 Gbps)" };
STATIC CONST CHAR16  *mSpeedShort[] = { L"FS", L"LS", L"HS", L"SS" };

STATIC
CONST CHAR16 *
ClassName (
  IN UINT8  Class
  )
{
  switch (Class) {
    case 0x00: return L"Defined at Interface level";
    case 0x01: return L"Audio";
    case 0x02: return L"Communications (CDC)";
    case 0x03: return L"HID";
    case 0x05: return L"Physical";
    case 0x06: return L"Image";
    case 0x07: return L"Printer";
    case 0x08: return L"Mass Storage";
    case 0x09: return L"Hub";
    case 0x0A: return L"CDC Data";
    case 0x0B: return L"Smart Card";
    case 0x0D: return L"Content Security";
    case 0x0E: return L"Video";
    case 0x0F: return L"Personal Healthcare";
    case 0x10: return L"Audio/Video";
    case 0x11: return L"Billboard";
    case 0x12: return L"USB Type-C Bridge";
    case 0xDC: return L"Diagnostic Device";
    case 0xE0: return L"Wireless Controller";
    case 0xEF: return L"Miscellaneous";
    case 0xFE: return L"Application Specific";
    case 0xFF: return L"Vendor Specific";
    default:   return L"Unknown";
  }
}

STATIC
CONST CHAR16 *
SpeedName (
  IN UINT8  Speed
  )
{
  return (Speed < ARRAY_SIZE (mSpeedNames)) ? mSpeedNames[Speed] : L"Unknown";
}

STATIC
UINT8
SpeedFromRootStatus (
  IN UINT16  Status
  )
{
  if (Status & USB_PORT_STAT_SUPER_SPEED) {
    return EFI_USB_SPEED_SUPER;
  }

  if (Status & USB_PORT_STAT_HIGH_SPEED) {
    return EFI_USB_SPEED_HIGH;
  }

  if (Status & USB_PORT_STAT_LOW_SPEED) {
    return EFI_USB_SPEED_LOW;
  }

  return EFI_USB_SPEED_FULL;
}

//
// ---- USB requests --------------------------------------------------------------
//

STATIC
EFI_STATUS
UsbIn (
  IN  EFI_USB_IO_PROTOCOL  *UsbIo,
  IN  UINT8                RequestType,
  IN  UINT8                Request,
  IN  UINT16               Value,
  IN  UINT16               Index,
  OUT VOID                 *Data,
  IN  UINT16               Length
  )
{
  EFI_USB_DEVICE_REQUEST  Req;
  UINT32                  UsbStatus;

  Req.RequestType = RequestType;
  Req.Request     = Request;
  Req.Value       = Value;
  Req.Index       = Index;
  Req.Length      = Length;
  return UsbIo->UsbControlTransfer (UsbIo, &Req, EfiUsbDataIn, USB_TIMEOUT, Data, Length, &UsbStatus);
}

STATIC
EFI_STATUS
HubPortStatus (
  IN  USB_DEV  *Dev,
  IN  UINT8    Port,       // 0-based
  OUT UINT16   *Status
  )
{
  UINT16      Buf[2];
  EFI_STATUS  Rc;

  Rc = UsbIn (Dev->UsbIo, 0xA3, 0x00, 0, (UINT16)(Port + 1), Buf, sizeof (Buf));
  if (!EFI_ERROR (Rc)) {
    *Status = Buf[0];
  }

  return Rc;
}

STATIC
UINT8
SpeedFromHubStatus (
  IN USB_DEV  *Hub,
  IN UINT16   Status
  )
{
  if (Hub->SsHub) {
    return EFI_USB_SPEED_SUPER;
  }

  if (Status & BIT10) {
    return EFI_USB_SPEED_HIGH;
  }

  if (Status & BIT9) {
    return EFI_USB_SPEED_LOW;
  }

  return EFI_USB_SPEED_FULL;
}

/**
  Fetch string descriptor Index in the device's first language.
**/
STATIC
BOOLEAN
GetUsbString (
  IN  EFI_USB_IO_PROTOCOL  *UsbIo,
  IN  UINT8                Index,
  OUT CHAR16               *Buf,
  IN  UINTN                BufSize
  )
{
  UINT16      *Langs;
  UINT16      LangBytes;
  CHAR16      *Str;
  EFI_STATUS  Status;

  Buf[0] = L'\0';
  if (Index == 0) {
    return FALSE;
  }

  Status = UsbIo->UsbGetSupportedLanguages (UsbIo, &Langs, &LangBytes);
  if (EFI_ERROR (Status) || (LangBytes < 2)) {
    return FALSE;
  }

  Str    = NULL;
  Status = UsbIo->UsbGetStringDescriptor (UsbIo, Langs[0], Index, &Str);
  if (EFI_ERROR (Status) || (Str == NULL)) {
    return FALSE;
  }

  StrnCpyS (Buf, BufSize / sizeof (CHAR16), Str, BufSize / sizeof (CHAR16) - 1);
  FreePool (Str);
  return TRUE;
}

//
// ---- Discovery -----------------------------------------------------------------
//

STATIC
VOID
ScanControllers (
  VOID
  )
{
  EFI_HANDLE           *Handles;
  UINTN                Count;
  UINTN                Index;
  USB_HC               *H;
  EFI_PCI_IO_PROTOCOL  *PciIo;
  UINT8                ClassCode[3];
  UINT16               Ids[2];
  UINTN                Seg;
  UINTN                Bus;
  UINTN                Dev;
  UINTN                Fn;
  UINT8                MaxSpeed;
  UINT8                Ports;
  UINT8                Is64;
  CONST CHAR16         *Kind;

  mHcCount = 0;
  if (EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiUsb2HcProtocolGuid, NULL, &Count, &Handles))) {
    return;
  }

  for (Index = 0; (Index < Count) && (mHcCount < MAX_HC); Index++) {
    H = &mHc[mHcCount];
    ZeroMem (H, sizeof (*H));
    H->Handle = Handles[Index];
    if (EFI_ERROR (gBS->HandleProtocol (H->Handle, &gEfiUsb2HcProtocolGuid, (VOID **)&H->Hc))) {
      continue;
    }

    if (EFI_ERROR (gBS->HandleProtocol (H->Handle, &gEfiDevicePathProtocolGuid, (VOID **)&H->Dp))) {
      continue;
    }

    H->DpSize = GetDevicePathSize (H->Dp) - END_DEVICE_PATH_LENGTH;
    if (!EFI_ERROR (H->Hc->GetCapability (H->Hc, &MaxSpeed, &Ports, &Is64))) {
      H->MaxSpeed = MaxSpeed;
      H->Ports    = Ports;
    }

    Kind = L"USB Host Controller";
    if (!EFI_ERROR (gBS->HandleProtocol (H->Handle, &gEfiPciIoProtocolGuid, (VOID **)&PciIo))) {
      PciIo->Pci.Read (PciIo, EfiPciIoWidthUint8, PCI_CLASSCODE_OFFSET, 3, ClassCode);
      PciIo->Pci.Read (PciIo, EfiPciIoWidthUint16, 0, 2, Ids);
      switch (ClassCode[0]) {
        case 0x00: Kind = L"UHCI Host Controller";
          break;
        case 0x10: Kind = L"OHCI Host Controller";
          break;
        case 0x20: Kind = L"EHCI Host Controller";
          break;
        case 0x30: Kind = L"xHCI Host Controller";
          break;
        default:
          break;
      }

      if (!EFI_ERROR (PciIo->GetLocation (PciIo, &Seg, &Bus, &Dev, &Fn))) {
        UnicodeSPrint (H->Pci, sizeof (H->Pci), L"PCI %04X:%02X:%02X.%X  [%04X:%04X]", Seg, Bus, Dev, Fn, Ids[0], Ids[1]);
      }
    }

    StrCpyS (H->Name, ARRAY_SIZE (H->Name), Kind);
    mHcCount++;
  }

  FreePool (Handles);
}

STATIC
VOID
ScanDevices (
  VOID
  )
{
  EFI_HANDLE                *Handles;
  UINTN                     Count;
  UINTN                     Index;
  UINTN                     HcIdx;
  UINTN                     Other;
  EFI_DEVICE_PATH_PROTOCOL  *Dp;
  EFI_DEVICE_PATH_PROTOCOL  *Node;
  EFI_USB_IO_PROTOCOL       *UsbIo;
  USB_DEV                   Tmp;
  USB_DEV                   *D;
  UINT8                     Hub[8];
  EFI_USB_INTERFACE_DESCRIPTOR  IfDesc;

  mDevCount = 0;
  if (EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiUsbIoProtocolGuid, NULL, &Count, &Handles))) {
    return;
  }

  for (Index = 0; Index < Count; Index++) {
    if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiUsbIoProtocolGuid, (VOID **)&UsbIo)) ||
        EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiDevicePathProtocolGuid, (VOID **)&Dp)))
    {
      continue;
    }

    //
    // Which controller is this device under?
    //
    for (HcIdx = 0; HcIdx < mHcCount; HcIdx++) {
      if ((GetDevicePathSize (Dp) > mHc[HcIdx].DpSize) && (CompareMem (Dp, mHc[HcIdx].Dp, mHc[HcIdx].DpSize) == 0)) {
        break;
      }
    }

    if (HcIdx == mHcCount) {
      continue;
    }

    ZeroMem (&Tmp, sizeof (Tmp));
    Tmp.Hc = HcIdx;
    Node   = (EFI_DEVICE_PATH_PROTOCOL *)((UINT8 *)Dp + mHc[HcIdx].DpSize);
    while (!IsDevicePathEnd (Node) &&
           (DevicePathType (Node) == MESSAGING_DEVICE_PATH) &&
           (DevicePathSubType (Node) == MSG_USB_DP) &&
           (Tmp.Depth < MAX_DEPTH))
    {
      Tmp.Port[Tmp.Depth++] = ((USB_DEVICE_PATH *)Node)->ParentPortNumber;
      Node                  = NextDevicePathNode (Node);
    }

    if (Tmp.Depth == 0) {
      continue;
    }

    //
    // One USB I/O handle exists per interface: merge them into one device.
    //
    for (Other = 0; Other < mDevCount; Other++) {
      if ((mDev[Other].Hc == Tmp.Hc) && (mDev[Other].Depth == Tmp.Depth) &&
          (CompareMem (mDev[Other].Port, Tmp.Port, Tmp.Depth) == 0))
      {
        break;
      }
    }

    if (Other < mDevCount) {
      mDev[Other].Interfaces++;
      continue;
    }

    if (mDevCount == MAX_DEV) {
      break;
    }

    D             = &mDev[mDevCount];
    *D            = Tmp;
    D->Handle     = Handles[Index];
    D->UsbIo      = UsbIo;
    D->Dp         = Dp;
    D->Interfaces = 1;
    D->Speed      = SPEED_UNKNOWN;
    UsbIo->UsbGetDeviceDescriptor (UsbIo, (EFI_USB_DEVICE_DESCRIPTOR *)D->Desc);
    D->IfClass = D->Desc[4];
    if (!EFI_ERROR (UsbIo->UsbGetInterfaceDescriptor (UsbIo, &IfDesc))) {
      if (D->Desc[4] == 0x00) {
        D->IfClass = IfDesc.InterfaceClass;
      }
    }

    D->IsHub = (BOOLEAN)(D->Desc[4] == 0x09);
    if (D->IsHub) {
      D->SsHub = (BOOLEAN)(ReadUnaligned16 ((UINT16 *)&D->Desc[2]) >= 0x0300);
      ZeroMem (Hub, sizeof (Hub));
      if (!EFI_ERROR (UsbIn (UsbIo, 0xA0, 0x06, (UINT16)((D->SsHub ? 0x2A : 0x29) << 8), 0, Hub, sizeof (Hub)))) {
        D->HubPorts = Hub[2];
      }
    }

    if (!GetUsbString (UsbIo, D->Desc[15], D->Product, sizeof (D->Product))) {
      StrCpyS (D->Product, ARRAY_SIZE (D->Product), ClassName (D->IfClass));
    }

    mDevCount++;
  }

  FreePool (Handles);
}

/**
  Device speeds come from the port each device hangs off.
**/
STATIC
VOID
ResolveSpeeds (
  VOID
  )
{
  UINTN                Index;
  UINTN                Parent;
  USB_DEV              *D;
  EFI_USB_PORT_STATUS  Root;
  UINT16               Status;

  for (Index = 0; Index < mDevCount; Index++) {
    D = &mDev[Index];
    if (D->Depth == 1) {
      if (!EFI_ERROR (mHc[D->Hc].Hc->GetRootHubPortStatus (mHc[D->Hc].Hc, D->Port[0], &Root))) {
        D->Speed = SpeedFromRootStatus (Root.PortStatus);
      }

      continue;
    }

    for (Parent = 0; Parent < mDevCount; Parent++) {
      if ((mDev[Parent].Hc == D->Hc) && (mDev[Parent].Depth == D->Depth - 1) &&
          (CompareMem (mDev[Parent].Port, D->Port, D->Depth - 1) == 0))
      {
        if (mDev[Parent].IsHub && !EFI_ERROR (HubPortStatus (&mDev[Parent], D->Port[D->Depth - 1], &Status))) {
          D->Speed = SpeedFromHubStatus (&mDev[Parent], Status);
        }

        break;
      }
    }
  }
}

STATIC
INTN
FindChild (
  IN UINTN        Hc,
  IN CONST UINT8  *Chain,
  IN UINTN        Depth,
  IN UINT8        Port
  )
{
  UINTN  Index;

  for (Index = 0; Index < mDevCount; Index++) {
    if ((mDev[Index].Hc == Hc) && (mDev[Index].Depth == Depth + 1) &&
        ((Depth == 0) || (CompareMem (mDev[Index].Port, Chain, Depth) == 0)) &&
        (mDev[Index].Port[Depth] == Port))
    {
      return (INTN)Index;
    }
  }

  return -1;
}

//
// ---- Tree ------------------------------------------------------------------------
//

STATIC
VOID
AddPorts (
  IN TEXT_DOC     *Doc,
  IN UINTN        Hc,
  IN CONST UINT8  *Chain,
  IN UINTN        Depth,
  IN UINTN        PortCount,
  IN USB_DEV      *HubDev
  )
{
  UINTN                Port;
  UINTN                Last;
  INTN                 Child;
  UINTN                Indent;
  USB_DEV              *D;
  EFI_USB_PORT_STATUS  Root;
  UINT16               Status;
  BOOLEAN              Connected;
  CONST CHAR16         *Speed;

  //
  // When a hub would not report its port count, cover every port a child
  // device is known to sit on.
  //
  Last = PortCount;
  for (Port = 0; Port < 256; Port++) {
    if ((FindChild (Hc, Chain, Depth, (UINT8)Port) >= 0) && (Port + 1 > Last)) {
      Last = Port + 1;
    }
  }

  Indent = 4 + Depth * 4;
  for (Port = 0; Port < Last; Port++) {
    Child = FindChild (Hc, Chain, Depth, (UINT8)Port);
    if (Child >= 0) {
      D     = &mDev[Child];
      Speed = (D->Speed < ARRAY_SIZE (mSpeedShort)) ? mSpeedShort[D->Speed] : L"--";
      DocAddItem (
        Doc,
        Child,
        L"%*sPort %d: [%04X:%04X] %s  (%s, %s)",
        Indent,
        L"",
        Port + 1,
        ReadUnaligned16 ((UINT16 *)&D->Desc[8]),
        ReadUnaligned16 ((UINT16 *)&D->Desc[10]),
        D->Product,
        D->IsHub ? L"Hub" : ClassName (D->IfClass),
        Speed
        );
      if (D->IsHub && (D->Depth < MAX_DEPTH)) {
        AddPorts (Doc, Hc, D->Port, D->Depth, D->HubPorts, D);
      }

      continue;
    }

    Connected = FALSE;
    if (HubDev == NULL) {
      if (!EFI_ERROR (mHc[Hc].Hc->GetRootHubPortStatus (mHc[Hc].Hc, (UINT8)Port, &Root))) {
        Connected = (BOOLEAN)((Root.PortStatus & USB_PORT_STAT_CONNECTION) != 0);
      }
    } else if (!EFI_ERROR (HubPortStatus (HubDev, (UINT8)Port, &Status))) {
      Connected = (BOOLEAN)((Status & BIT0) != 0);
    }

    DocAdd (
      Doc,
      Connected ? ATTR_CHANGED : ATTR_DIM,
      L"%*sPort %d: %s",
      Indent,
      L"",
      Port + 1,
      Connected ? L"Device connected (not enumerated by firmware)" : L"No device connected"
      );
  }
}

STATIC
VOID
BuildTree (
  OUT TEXT_DOC  *Doc
  )
{
  UINTN  Hc;

  DocInit (Doc);
  if (mHcCount == 0) {
    DocAdd (Doc, ATTR_WARN, L" No USB host controller (EFI_USB2_HC_PROTOCOL) found. ");
    return;
  }

  for (Hc = 0; Hc < mHcCount; Hc++) {
    DocAddItem (Doc, (INTN)(HC_TAG_BASE + Hc), L"%s  %s", mHc[Hc].Name, mHc[Hc].Pci);
    DocAdd (Doc, ATTR_HEADER, L"  Root Hub  (%d ports, max %s)", mHc[Hc].Ports, SpeedName (mHc[Hc].MaxSpeed));
    AddPorts (Doc, Hc, NULL, 0, mHc[Hc].Ports, NULL);
    DocAdd (Doc, ATTR_NORMAL, L"");
  }
}

//
// ---- Detail pages --------------------------------------------------------------
//

STATIC
VOID
AddStrField (
  IN TEXT_DOC             *Doc,
  IN EFI_USB_IO_PROTOCOL  *UsbIo,
  IN CONST CHAR16         *Name,
  IN UINT8                Index
  )
{
  CHAR16  Str[128];

  if ((Index != 0) && GetUsbString (UsbIo, Index, Str, sizeof (Str))) {
    DocAdd (Doc, ATTR_NORMAL, L"  %-22s %02Xh  \"%s\"", Name, Index, Str);
  } else {
    DocAdd (Doc, ATTR_NORMAL, L"  %-22s %02Xh", Name, Index);
  }
}

STATIC
VOID
AddConfig (
  IN TEXT_DOC             *Doc,
  IN EFI_USB_IO_PROTOCOL  *UsbIo,
  IN UINT8                *Cfg,
  IN UINTN                Total,
  IN BOOLEAN              SuperSpeed
  )
{
  UINTN         Off;
  UINT8         Len;
  UINT8         Type;
  UINT8         *P;
  CONST CHAR16  *XferType[] = { L"Control", L"Isochronous", L"Bulk", L"Interrupt" };

  for (Off = 0; Off + 2 <= Total; Off += Len) {
    P    = Cfg + Off;
    Len  = P[0];
    Type = P[1];
    if ((Len < 2) || (Off + Len > Total)) {
      break;
    }

    DocAdd (Doc, ATTR_NORMAL, L"");
    switch (Type) {
      case 0x02:
        DocAdd (Doc, ATTR_HEADER, L"Configuration Descriptor");
        DocAdd (Doc, ATTR_NORMAL, L"  %-22s %04Xh", L"wTotalLength", ReadUnaligned16 ((UINT16 *)&P[2]));
        DocAdd (Doc, ATTR_NORMAL, L"  %-22s %02Xh", L"bNumInterfaces", P[4]);
        DocAdd (Doc, ATTR_NORMAL, L"  %-22s %02Xh", L"bConfigurationValue", P[5]);
        AddStrField (Doc, UsbIo, L"iConfiguration", P[6]);
        DocAdd (
          Doc,
          ATTR_NORMAL,
          L"  %-22s %02Xh  %s%s",
          L"bmAttributes",
          P[7],
          (P[7] & BIT6) ? L"Self Powered" : L"Bus Powered",
          (P[7] & BIT5) ? L", Remote Wakeup" : L""
          );
        DocAdd (Doc, ATTR_NORMAL, L"  %-22s %02Xh  (%d mA)", L"bMaxPower", P[8], P[8] * (SuperSpeed ? 8 : 2));
        break;

      case 0x04:
        DocAdd (Doc, ATTR_HEADER, L"Interface Descriptor  (Interface %d, Alternate %d)", P[2], P[3]);
        DocAdd (Doc, ATTR_NORMAL, L"  %-22s %02Xh", L"bInterfaceNumber", P[2]);
        DocAdd (Doc, ATTR_NORMAL, L"  %-22s %02Xh", L"bAlternateSetting", P[3]);
        DocAdd (Doc, ATTR_NORMAL, L"  %-22s %02Xh", L"bNumEndpoints", P[4]);
        DocAdd (Doc, ATTR_NORMAL, L"  %-22s %02Xh  %s", L"bInterfaceClass", P[5], ClassName (P[5]));
        DocAdd (Doc, ATTR_NORMAL, L"  %-22s %02Xh", L"bInterfaceSubClass", P[6]);
        DocAdd (Doc, ATTR_NORMAL, L"  %-22s %02Xh", L"bInterfaceProtocol", P[7]);
        AddStrField (Doc, UsbIo, L"iInterface", P[8]);
        break;

      case 0x05:
        DocAdd (Doc, ATTR_HEADER, L"Endpoint Descriptor  (EP %d %s)", P[2] & 0x0F, (P[2] & BIT7) ? L"IN" : L"OUT");
        DocAdd (Doc, ATTR_NORMAL, L"  %-22s %02Xh  EP %d %s", L"bEndpointAddress", P[2], P[2] & 0x0F, (P[2] & BIT7) ? L"IN" : L"OUT");
        DocAdd (Doc, ATTR_NORMAL, L"  %-22s %02Xh  %s", L"bmAttributes", P[3], XferType[P[3] & 3]);
        DocAdd (Doc, ATTR_NORMAL, L"  %-22s %04Xh  (%d bytes)", L"wMaxPacketSize", ReadUnaligned16 ((UINT16 *)&P[4]), ReadUnaligned16 ((UINT16 *)&P[4]) & 0x7FF);
        DocAdd (Doc, ATTR_NORMAL, L"  %-22s %02Xh", L"bInterval", P[6]);
        break;

      case 0x0B:
        DocAdd (Doc, ATTR_HEADER, L"Interface Association Descriptor");
        DocAdd (Doc, ATTR_NORMAL, L"  %-22s %02Xh", L"bFirstInterface", P[2]);
        DocAdd (Doc, ATTR_NORMAL, L"  %-22s %02Xh", L"bInterfaceCount", P[3]);
        DocAdd (Doc, ATTR_NORMAL, L"  %-22s %02Xh  %s", L"bFunctionClass", P[4], ClassName (P[4]));
        DocAdd (Doc, ATTR_NORMAL, L"  %-22s %02Xh", L"bFunctionSubClass", P[5]);
        DocAdd (Doc, ATTR_NORMAL, L"  %-22s %02Xh", L"bFunctionProtocol", P[6]);
        if (Len > 7) {
          AddStrField (Doc, UsbIo, L"iFunction", P[7]);
        }

        break;

      case 0x21:
        DocAdd (Doc, ATTR_HEADER, L"HID Descriptor");
        if (Len >= 9) {
          DocAdd (Doc, ATTR_NORMAL, L"  %-22s %04Xh", L"bcdHID", ReadUnaligned16 ((UINT16 *)&P[2]));
          DocAdd (Doc, ATTR_NORMAL, L"  %-22s %02Xh", L"bCountryCode", P[4]);
          DocAdd (Doc, ATTR_NORMAL, L"  %-22s %02Xh", L"bNumDescriptors", P[5]);
          DocAdd (Doc, ATTR_NORMAL, L"  %-22s %02Xh  %s", L"bDescriptorType", P[6], (P[6] == 0x22) ? L"Report" : L"");
          DocAdd (Doc, ATTR_NORMAL, L"  %-22s %04Xh  (%d bytes)", L"wDescriptorLength", ReadUnaligned16 ((UINT16 *)&P[7]), ReadUnaligned16 ((UINT16 *)&P[7]));
        }

        break;

      case 0x30:
        DocAdd (Doc, ATTR_HEADER, L"SuperSpeed Endpoint Companion Descriptor");
        if (Len >= 6) {
          DocAdd (Doc, ATTR_NORMAL, L"  %-22s %02Xh", L"bMaxBurst", P[2]);
          DocAdd (Doc, ATTR_NORMAL, L"  %-22s %02Xh", L"bmAttributes", P[3]);
          DocAdd (Doc, ATTR_NORMAL, L"  %-22s %04Xh", L"wBytesPerInterval", ReadUnaligned16 ((UINT16 *)&P[4]));
        }

        break;

      default:
        DocAdd (Doc, ATTR_HEADER, L"Descriptor Type %02Xh  (%d bytes)", Type, Len);
        DocAddHex (Doc, P, Len, Off);
        break;
    }
  }
}

STATIC
VOID
ShowDevice (
  IN USB_DEV  *D
  )
{
  TEXT_DOC                   Doc;
  CHAR16                     Header[160];
  CHAR16                     Path[64];
  CHAR16                     *DpText;
  UINTN                      Pos;
  UINTN                      Index;
  UINT8                      *Dd;
  EFI_USB_CONFIG_DESCRIPTOR  Cfg9;
  UINT8                      *Cfg;
  UINT16                     Total;
  UINT16                     Bcd;
  UINT16                     Status;

  Dd = D->Desc;
  DocInit (&Doc);
  UnicodeSPrint (
    Header,
    sizeof (Header),
    L"[%04X:%04X] %s",
    ReadUnaligned16 ((UINT16 *)&Dd[8]),
    ReadUnaligned16 ((UINT16 *)&Dd[10]),
    D->Product
    );

  Pos = UnicodeSPrint (Path, sizeof (Path), L"Root port %d", D->Port[0] + 1);
  for (Index = 1; Index < D->Depth; Index++) {
    Pos += UnicodeSPrint (Path + Pos, sizeof (Path) - Pos * sizeof (CHAR16), L" > hub port %d", D->Port[Index] + 1);
  }

  DocAdd (&Doc, ATTR_HEADER, L"Connection");
  DocAdd (&Doc, ATTR_NORMAL, L"  %-22s %s  %s", L"Host Controller", mHc[D->Hc].Name, mHc[D->Hc].Pci);
  DocAdd (&Doc, ATTR_NORMAL, L"  %-22s %s", L"Port Path", Path);
  DocAdd (&Doc, ATTR_NORMAL, L"  %-22s %s", L"Speed", SpeedName (D->Speed));
  DocAdd (&Doc, ATTR_NORMAL, L"  %-22s %d", L"Interfaces (handles)", D->Interfaces);
  DpText = ConvertDevicePathToText (D->Dp, TRUE, TRUE);
  if (DpText != NULL) {
    DocAdd (&Doc, ATTR_NORMAL, L"  %-22s %s", L"Device Path", DpText);
    FreePool (DpText);
  }

  Bcd = ReadUnaligned16 ((UINT16 *)&Dd[2]);
  DocAdd (&Doc, ATTR_NORMAL, L"");
  DocAdd (&Doc, ATTR_HEADER, L"Device Descriptor");
  DocAdd (&Doc, ATTR_NORMAL, L"  %-22s %02Xh", L"bLength", Dd[0]);
  DocAdd (&Doc, ATTR_NORMAL, L"  %-22s %02Xh", L"bDescriptorType", Dd[1]);
  DocAdd (&Doc, ATTR_NORMAL, L"  %-22s %04Xh  (USB %x.%02x)", L"bcdUSB", Bcd, Bcd >> 8, Bcd & 0xFF);
  DocAdd (&Doc, ATTR_NORMAL, L"  %-22s %02Xh  %s", L"bDeviceClass", Dd[4], ClassName (Dd[4]));
  DocAdd (&Doc, ATTR_NORMAL, L"  %-22s %02Xh", L"bDeviceSubClass", Dd[5]);
  DocAdd (&Doc, ATTR_NORMAL, L"  %-22s %02Xh", L"bDeviceProtocol", Dd[6]);
  DocAdd (&Doc, ATTR_NORMAL, L"  %-22s %02Xh  (%d bytes)", L"bMaxPacketSize0", Dd[7], (Bcd >= 0x0300) ? (1 << Dd[7]) : Dd[7]);
  DocAdd (&Doc, ATTR_NORMAL, L"  %-22s %04Xh", L"idVendor", ReadUnaligned16 ((UINT16 *)&Dd[8]));
  DocAdd (&Doc, ATTR_NORMAL, L"  %-22s %04Xh", L"idProduct", ReadUnaligned16 ((UINT16 *)&Dd[10]));
  DocAdd (&Doc, ATTR_NORMAL, L"  %-22s %04Xh", L"bcdDevice", ReadUnaligned16 ((UINT16 *)&Dd[12]));
  AddStrField (&Doc, D->UsbIo, L"iManufacturer", Dd[14]);
  AddStrField (&Doc, D->UsbIo, L"iProduct", Dd[15]);
  AddStrField (&Doc, D->UsbIo, L"iSerialNumber", Dd[16]);
  DocAdd (&Doc, ATTR_NORMAL, L"  %-22s %02Xh", L"bNumConfigurations", Dd[17]);

  //
  // Full configuration descriptor set of the active configuration.
  //
  Cfg = NULL;
  if (!EFI_ERROR (D->UsbIo->UsbGetConfigDescriptor (D->UsbIo, &Cfg9))) {
    Total = Cfg9.TotalLength;
    Cfg   = AllocateZeroPool (Total);
    if ((Cfg != NULL) && (Total >= 9) &&
        !EFI_ERROR (UsbIn (D->UsbIo, 0x80, 0x06, (UINT16)((0x02 << 8) | (Cfg9.ConfigurationValue ? Cfg9.ConfigurationValue - 1 : 0)), 0, Cfg, Total)) &&
        (Cfg[1] == 0x02))
    {
      AddConfig (&Doc, D->UsbIo, Cfg, Total, (BOOLEAN)(D->Speed == EFI_USB_SPEED_SUPER));
    } else {
      DocAdd (&Doc, ATTR_NORMAL, L"");
      DocAdd (&Doc, ATTR_DIM, L"Configuration descriptor set could not be read.");
      if (Cfg != NULL) {
        FreePool (Cfg);
        Cfg = NULL;
      }
    }
  }

  if (D->IsHub) {
    DocAdd (&Doc, ATTR_NORMAL, L"");
    DocAdd (&Doc, ATTR_HEADER, L"Hub  (%d downstream ports%s)", D->HubPorts, D->SsHub ? L", SuperSpeed" : L"");
    for (Index = 0; Index < D->HubPorts; Index++) {
      if (EFI_ERROR (HubPortStatus (D, (UINT8)Index, &Status))) {
        DocAdd (&Doc, ATTR_DIM, L"  Port %d: status unavailable", Index + 1);
        continue;
      }

      DocAdd (
        &Doc,
        ATTR_NORMAL,
        L"  Port %d: %04Xh  %s%s%s%s",
        Index + 1,
        Status,
        (Status & BIT0) ? L"Connected" : L"Empty",
        (Status & BIT1) ? L", Enabled" : L"",
        (Status & BIT3) ? L", Over-current" : L"",
        ((Status & BIT0) && !D->SsHub) ? ((Status & BIT10) ? L", High Speed" : ((Status & BIT9) ? L", Low Speed" : L", Full Speed")) : L""
        );
    }
  }

  DocAdd (&Doc, ATTR_NORMAL, L"");
  DocAdd (&Doc, ATTR_HEADER, L"Raw Device Descriptor");
  DocAddHex (&Doc, Dd, 18, 0);
  if (Cfg != NULL) {
    DocAdd (&Doc, ATTR_HEADER, L"Raw Configuration Descriptor Set (%d bytes)", Total);
    DocAddHex (&Doc, Cfg, Total, 0);
    FreePool (Cfg);
  }

  DocView (L"USB Device", Header, &Doc);
  DocFree (&Doc);
}

STATIC
VOID
ShowController (
  IN UINTN  Hc
  )
{
  TEXT_DOC             Doc;
  CHAR16               *DpText;
  UINTN                Port;
  EFI_USB_PORT_STATUS  Root;
  UINT16               St;

  DocInit (&Doc);
  DocAdd (&Doc, ATTR_HEADER, L"Host Controller");
  DocAdd (&Doc, ATTR_NORMAL, L"  %-22s %s", L"Type", mHc[Hc].Name);
  DocAdd (&Doc, ATTR_NORMAL, L"  %-22s %s", L"Location", mHc[Hc].Pci);
  DocAdd (&Doc, ATTR_NORMAL, L"  %-22s %s", L"Max Speed", SpeedName (mHc[Hc].MaxSpeed));
  DocAdd (&Doc, ATTR_NORMAL, L"  %-22s %d", L"Root Hub Ports", mHc[Hc].Ports);
  DpText = ConvertDevicePathToText (mHc[Hc].Dp, TRUE, TRUE);
  if (DpText != NULL) {
    DocAdd (&Doc, ATTR_NORMAL, L"  %-22s %s", L"Device Path", DpText);
    FreePool (DpText);
  }

  DocAdd (&Doc, ATTR_NORMAL, L"");
  DocAdd (&Doc, ATTR_HEADER, L"Root Hub Port Status");
  for (Port = 0; Port < mHc[Hc].Ports; Port++) {
    if (EFI_ERROR (mHc[Hc].Hc->GetRootHubPortStatus (mHc[Hc].Hc, (UINT8)Port, &Root))) {
      DocAdd (&Doc, ATTR_DIM, L"  Port %2d: status unavailable", Port + 1);
      continue;
    }

    St = Root.PortStatus;
    DocAdd (
      &Doc,
      ATTR_NORMAL,
      L"  Port %2d: %04Xh  %s%s%s%s%s",
      Port + 1,
      St,
      (St & USB_PORT_STAT_CONNECTION) ? L"Connected" : L"Empty",
      (St & USB_PORT_STAT_ENABLE) ? L", Enabled" : L"",
      (St & USB_PORT_STAT_OVERCURRENT) ? L", Over-current" : L"",
      (St & USB_PORT_STAT_OWNER) ? L", Companion owned" : L"",
      (St & USB_PORT_STAT_CONNECTION) ? ((St & USB_PORT_STAT_SUPER_SPEED) ? L", SuperSpeed" : ((St & USB_PORT_STAT_HIGH_SPEED) ? L", High Speed" : ((St & USB_PORT_STAT_LOW_SPEED) ? L", Low Speed" : L", Full Speed"))) : L""
      );
  }

  DocView (L"USB Host Controller", mHc[Hc].Name, &Doc);
  DocFree (&Doc);
}

VOID
UsbViewRun (
  VOID
  )
{
  TEXT_DOC  Doc;
  UINTN     Sel;
  INTN      Pick;
  CHAR16    Header[96];

  Sel  = 0;
  Pick = -2;
  for ( ; ;) {
    if (Pick == -2) {
      UiStatus (ATTR_DIM, L"Scanning USB...");
      UiFlush ();
      ScanControllers ();
      ScanDevices ();
      ResolveSpeeds ();
    }

    BuildTree (&Doc);
    UnicodeSPrint (Header, sizeof (Header), L"%d host controller(s), %d device(s)", mHcCount, mDevCount);
    Pick = DocSelect (
             L"USB Devices",
             L"Up/Down/PgUp/PgDn: select   Enter: details   F5: rescan   Esc: back",
             Header,
             &Doc,
             &Sel
             );
    DocFree (&Doc);
    if (Pick == -1) {
      return;
    }

    if (Pick == -2) {
      continue;
    }

    if (Pick >= HC_TAG_BASE) {
      ShowController ((UINTN)(Pick - HC_TAG_BASE));
    } else {
      ShowDevice (&mDev[Pick]);
    }
  }
}
