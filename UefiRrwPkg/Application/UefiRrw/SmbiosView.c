/** @file
  SMBIOS viewer: entry point summary, a list of every structure (type,
  handle, name), and a detail page per structure with decoded fields,
  its string set and a raw dump of the formatted area.

  Decoding follows the DMTF SMBIOS Reference Specification (DSP0134).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "UefiRrw.h"
#include <IndustryStandard/SmBios.h>
#include <Guid/SmBios.h>

#define MAX_STRUCTS  1024

typedef struct {
  UINT8     *Ptr;     // start of formatted area
  UINTN     Total;    // formatted area + string set (incl. double NUL)
} SMB_STRUCT;

STATIC SMB_STRUCT  mStructs[MAX_STRUCTS];
STATIC UINTN       mStructCount;
STATIC CHAR16      mEntryInfo[160];

//
// ---- Name tables -------------------------------------------------------------
//

STATIC CONST CHAR16  *mTypeNames[] = {
  L"BIOS Information",                       // 0
  L"System Information",                     // 1
  L"Baseboard Information",                  // 2
  L"System Enclosure or Chassis",            // 3
  L"Processor Information",                  // 4
  L"Memory Controller Information",          // 5
  L"Memory Module Information",              // 6
  L"Cache Information",                      // 7
  L"Port Connector Information",             // 8
  L"System Slots",                           // 9
  L"On Board Devices Information",           // 10
  L"OEM Strings",                            // 11
  L"System Configuration Options",           // 12
  L"BIOS Language Information",              // 13
  L"Group Associations",                     // 14
  L"System Event Log",                       // 15
  L"Physical Memory Array",                  // 16
  L"Memory Device",                          // 17
  L"32-Bit Memory Error Information",        // 18
  L"Memory Array Mapped Address",            // 19
  L"Memory Device Mapped Address",           // 20
  L"Built-in Pointing Device",               // 21
  L"Portable Battery",                       // 22
  L"System Reset",                           // 23
  L"Hardware Security",                      // 24
  L"System Power Controls",                  // 25
  L"Voltage Probe",                          // 26
  L"Cooling Device",                         // 27
  L"Temperature Probe",                      // 28
  L"Electrical Current Probe",               // 29
  L"Out-of-Band Remote Access",              // 30
  L"Boot Integrity Services Entry Point",    // 31
  L"System Boot Information",                // 32
  L"64-Bit Memory Error Information",        // 33
  L"Management Device",                      // 34
  L"Management Device Component",            // 35
  L"Management Device Threshold Data",       // 36
  L"Memory Channel",                         // 37
  L"IPMI Device Information",                // 38
  L"System Power Supply",                    // 39
  L"Additional Information",                 // 40
  L"Onboard Devices Extended Information",   // 41
  L"Management Controller Host Interface",   // 42
  L"TPM Device",                             // 43
  L"Processor Additional Information",       // 44
  L"Firmware Inventory Information",         // 45
  L"String Property",                        // 46
};

STATIC CONST CHAR16  *mChassisTypes[] = {
  NULL, L"Other", L"Unknown", L"Desktop", L"Low Profile Desktop", L"Pizza Box",
  L"Mini Tower", L"Tower", L"Portable", L"Laptop", L"Notebook", L"Hand Held",
  L"Docking Station", L"All in One", L"Sub Notebook", L"Space-saving", L"Lunch Box",
  L"Main Server Chassis", L"Expansion Chassis", L"SubChassis", L"Bus Expansion Chassis",
  L"Peripheral Chassis", L"RAID Chassis", L"Rack Mount Chassis", L"Sealed-case PC",
  L"Multi-system Chassis", L"Compact PCI", L"Advanced TCA", L"Blade", L"Blade Enclosure",
  L"Tablet", L"Convertible", L"Detachable", L"IoT Gateway", L"Embedded PC", L"Mini PC",
  L"Stick PC",
};

STATIC CONST CHAR16  *mChassisStates[] = {
  NULL, L"Other", L"Unknown", L"Safe", L"Warning", L"Critical", L"Non-recoverable",
};

STATIC CONST CHAR16  *mChassisSecurity[] = {
  NULL, L"Other", L"Unknown", L"None", L"External Interface Locked Out", L"External Interface Enabled",
};

STATIC CONST CHAR16  *mWakeUpTypes[] = {
  L"Reserved", L"Other", L"Unknown", L"APM Timer", L"Modem Ring", L"LAN Remote",
  L"Power Switch", L"PCI PME#", L"AC Power Restored",
};

STATIC CONST CHAR16  *mBoardTypes[] = {
  NULL, L"Unknown", L"Other", L"Server Blade", L"Connectivity Switch",
  L"System Management Module", L"Processor Module", L"I/O Module", L"Memory Module",
  L"Daughter Board", L"Motherboard", L"Processor/Memory Module", L"Processor/IO Module",
  L"Interconnect Board",
};

STATIC CONST CHAR16  *mProcTypes[] = {
  NULL, L"Other", L"Unknown", L"Central Processor", L"Math Processor", L"DSP Processor",
  L"Video Processor",
};

STATIC CONST CHAR16  *mEccTypes[] = {
  NULL, L"Other", L"Unknown", L"None", L"Parity", L"Single-bit ECC", L"Multi-bit ECC", L"CRC",
};

STATIC CONST CHAR16  *mCacheTypes[] = {
  NULL, L"Other", L"Unknown", L"Instruction", L"Data", L"Unified",
};

STATIC CONST CHAR16  *mCacheAssoc[] = {
  NULL, L"Other", L"Unknown", L"Direct Mapped", L"2-way Set-Associative",
  L"4-way Set-Associative", L"Fully Associative", L"8-way Set-Associative",
  L"16-way Set-Associative", L"12-way Set-Associative", L"24-way Set-Associative",
  L"32-way Set-Associative", L"48-way Set-Associative", L"64-way Set-Associative",
  L"20-way Set-Associative",
};

STATIC CONST CHAR16  *mSlotUsage[] = {
  NULL, L"Other", L"Unknown", L"Available", L"In Use", L"Unavailable",
};

STATIC CONST CHAR16  *mSlotWidth[] = {
  NULL, L"Other", L"Unknown", L"8 bit", L"16 bit", L"32 bit", L"64 bit", L"128 bit",
  L"x1", L"x2", L"x4", L"x8", L"x12", L"x16", L"x32",
};

STATIC CONST CHAR16  *mArrayLocation[] = {
  NULL, L"Other", L"Unknown", L"System Board or Motherboard", L"ISA Add-on Card",
  L"EISA Add-on Card", L"PCI Add-on Card", L"MCA Add-on Card", L"PCMCIA Add-on Card",
  L"Proprietary Add-on Card", L"NuBus",
};

STATIC CONST CHAR16  *mArrayUse[] = {
  NULL, L"Other", L"Unknown", L"System Memory", L"Video Memory", L"Flash Memory",
  L"Non-volatile RAM", L"Cache Memory",
};

STATIC CONST CHAR16  *mMemFormFactors[] = {
  NULL, L"Other", L"Unknown", L"SIMM", L"SIP", L"Chip", L"DIP", L"ZIP",
  L"Proprietary Card", L"DIMM", L"TSOP", L"Row of Chips", L"RIMM", L"SODIMM",
  L"SRIMM", L"FB-DIMM", L"Die", L"CAMM",
};

STATIC CONST CHAR16  *mMemTypes[] = {
  NULL, L"Other", L"Unknown", L"DRAM", L"EDRAM", L"VRAM", L"SRAM", L"RAM", L"ROM",
  L"FLASH", L"EEPROM", L"FEPROM", L"EPROM", L"CDRAM", L"3DRAM", L"SDRAM", L"SGRAM",
  L"RDRAM", L"DDR", L"DDR2", L"DDR2 FB-DIMM", NULL, NULL, NULL, L"DDR3", L"FBD2",
  L"DDR4", L"LPDDR", L"LPDDR2", L"LPDDR3", L"LPDDR4", L"Logical non-volatile device",
  L"HBM", L"HBM2", L"DDR5", L"LPDDR5", L"HBM3",
};

STATIC CONST CHAR16  *mMemTypeDetail[] = {
  L"Reserved", L"Other", L"Unknown", L"Fast-paged", L"Static column", L"Pseudo-static",
  L"RAMBUS", L"Synchronous", L"CMOS", L"EDO", L"Window DRAM", L"Cache DRAM",
  L"Non-volatile", L"Registered (Buffered)", L"Unbuffered (Unregistered)", L"LRDIMM",
};

STATIC CONST CHAR16  *mOnboardTypes[] = {
  NULL, L"Other", L"Unknown", L"Video", L"SCSI Controller", L"Ethernet", L"Token Ring",
  L"Sound", L"PATA Controller", L"SATA Controller", L"SAS Controller",
  L"Wireless LAN", L"Bluetooth", L"WWAN", L"eMMC", L"NVMe Controller", L"UFS Controller",
};

STATIC CONST CHAR16  *mBiosChar[32] = {
  NULL, NULL, L"Unknown", L"BIOS Characteristics Not Supported", L"ISA supported",
  L"MCA supported", L"EISA supported", L"PCI supported", L"PC Card (PCMCIA) supported",
  L"Plug and Play supported", L"APM supported", L"BIOS is upgradeable (Flash)",
  L"BIOS shadowing allowed", L"VL-VESA supported", L"ESCD support available",
  L"Boot from CD supported", L"Selectable boot supported", L"BIOS ROM is socketed",
  L"Boot from PC Card supported", L"EDD specification supported",
  L"Int 13h - NEC 9800 1.2 MB floppy", L"Int 13h - Toshiba 1.2 MB floppy",
  L"Int 13h - 5.25\" 360 KB floppy", L"Int 13h - 5.25\" 1.2 MB floppy",
  L"Int 13h - 3.5\" 720 KB floppy", L"Int 13h - 3.5\" 2.88 MB floppy",
  L"Int 5h - Print screen service", L"Int 9h - 8042 keyboard services",
  L"Int 14h - Serial services", L"Int 17h - Printer services",
  L"Int 10h - CGA/Mono video services", L"NEC PC-98",
};

STATIC CONST CHAR16  *mBiosExt1[8] = {
  L"ACPI supported", L"USB Legacy supported", L"AGP supported", L"I2O boot supported",
  L"LS-120 SuperDisk boot supported", L"ATAPI ZIP drive boot supported",
  L"1394 boot supported", L"Smart battery supported",
};

STATIC CONST CHAR16  *mBiosExt2[8] = {
  L"BIOS Boot Specification supported", L"Function key-initiated network boot supported",
  L"Targeted content distribution supported", L"UEFI Specification supported",
  L"SMBIOS table describes a virtual machine", L"Manufacturing mode supported",
  L"Manufacturing mode enabled", NULL,
};

STATIC CONST CHAR16  *mProcChar[16] = {
  NULL, L"Unknown", L"64-bit Capable", L"Multi-Core", L"Hardware Thread",
  L"Execute Protection", L"Enhanced Virtualization", L"Power/Performance Control",
  L"128-bit Capable", L"Arm64 SoC ID", NULL, NULL, NULL, NULL, NULL, NULL,
};

STATIC CONST CHAR16  *mProcStatus[8] = {
  L"Unknown", L"CPU Enabled", L"CPU Disabled by User (BIOS Setup)",
  L"CPU Disabled by BIOS (POST Error)", L"CPU Idle, Waiting to be Enabled",
  L"Reserved", L"Reserved", L"Other",
};

#define ENUM_NAME(Table, Value) \
  ((((UINTN)(Value)) < ARRAY_SIZE (Table)) && ((Table)[(Value)] != NULL) ? (Table)[(Value)] : L"Unknown")

STATIC
CONST CHAR16 *
TypeName (
  IN UINT8  Type
  )
{
  if (Type < ARRAY_SIZE (mTypeNames)) {
    return mTypeNames[Type];
  }

  if (Type == 126) {
    return L"Inactive";
  }

  if (Type == 127) {
    return L"End-of-Table";
  }

  return (Type >= 128) ? L"OEM Specific" : L"Unknown";
}

//
// ---- Table discovery ---------------------------------------------------------
//

STATIC
UINTN
StructTotalLength (
  IN UINT8  *Ptr,
  IN UINT8  *End
  )
{
  UINT8  *Str;

  Str = Ptr + Ptr[1];
  while (Str + 1 < End) {
    if ((Str[0] == 0) && (Str[1] == 0)) {
      return (UINTN)(Str + 2 - Ptr);
    }

    Str++;
  }

  return 0;
}

STATIC
BOOLEAN
LoadTables (
  VOID
  )
{
  SMBIOS_TABLE_3_0_ENTRY_POINT  *Ep3;
  SMBIOS_TABLE_ENTRY_POINT      *Ep2;
  UINT8                         *Ptr;
  UINT8                         *End;
  UINTN                         Total;

  mStructCount = 0;
  Ep3          = NULL;
  Ep2          = NULL;
  EfiGetSystemConfigurationTable (&gEfiSmbios3TableGuid, (VOID **)&Ep3);
  EfiGetSystemConfigurationTable (&gEfiSmbiosTableGuid, (VOID **)&Ep2);

  if (Ep3 != NULL) {
    Ptr = (UINT8 *)(UINTN)Ep3->TableAddress;
    End = Ptr + Ep3->TableMaximumSize;
    UnicodeSPrint (
      mEntryInfo,
      sizeof (mEntryInfo),
      L"SMBIOS %d.%d.%d (64-bit entry @ %p)  Table @ %08lX  Max size %d",
      Ep3->MajorVersion,
      Ep3->MinorVersion,
      Ep3->DocRev,
      Ep3,
      Ep3->TableAddress,
      Ep3->TableMaximumSize
      );
  } else if (Ep2 != NULL) {
    Ptr = (UINT8 *)(UINTN)Ep2->TableAddress;
    End = Ptr + Ep2->TableLength;
    UnicodeSPrint (
      mEntryInfo,
      sizeof (mEntryInfo),
      L"SMBIOS %d.%d (32-bit entry @ %p)  Table @ %08X  Length %d  Structures %d",
      Ep2->MajorVersion,
      Ep2->MinorVersion,
      Ep2,
      Ep2->TableAddress,
      Ep2->TableLength,
      Ep2->NumberOfSmbiosStructures
      );
  } else {
    return FALSE;
  }

  while ((Ptr + sizeof (SMBIOS_STRUCTURE) <= End) && (mStructCount < MAX_STRUCTS)) {
    if (Ptr[1] < sizeof (SMBIOS_STRUCTURE)) {
      break;
    }

    Total = StructTotalLength (Ptr, End);
    if (Total == 0) {
      break;
    }

    mStructs[mStructCount].Ptr   = Ptr;
    mStructs[mStructCount].Total = Total;
    mStructCount++;
    if (Ptr[0] == 127) {
      break;
    }

    Ptr += Total;
  }

  return TRUE;
}

//
// ---- Field helpers -------------------------------------------------------------
//

typedef struct {
  UINT8       *P;
  UINTN       Len;     // formatted length
  TEXT_DOC    *Doc;
} SMB_CTX;

STATIC
BOOLEAN
Has (
  IN SMB_CTX  *C,
  IN UINTN    Offset,
  IN UINTN    Size
  )
{
  return (BOOLEAN)(Offset + Size <= C->Len);
}

STATIC
UINT8
B (
  IN SMB_CTX  *C,
  IN UINTN    Offset
  )
{
  return C->P[Offset];
}

STATIC
UINT16
W (
  IN SMB_CTX  *C,
  IN UINTN    Offset
  )
{
  return ReadUnaligned16 ((UINT16 *)(C->P + Offset));
}

STATIC
UINT32
D (
  IN SMB_CTX  *C,
  IN UINTN    Offset
  )
{
  return ReadUnaligned32 ((UINT32 *)(C->P + Offset));
}

STATIC
UINT64
Q (
  IN SMB_CTX  *C,
  IN UINTN    Offset
  )
{
  return ReadUnaligned64 ((UINT64 *)(C->P + Offset));
}

/**
  Pointer to string N (1-based) of the structure, or NULL.
**/
STATIC
CHAR8 *
SmbString (
  IN SMB_CTX  *C,
  IN UINT8    Number
  )
{
  CHAR8  *Str;

  if (Number == 0) {
    return NULL;
  }

  Str = (CHAR8 *)(C->P + C->Len);
  while (*Str != '\0') {
    if (--Number == 0) {
      return Str;
    }

    Str += AsciiStrLen (Str) + 1;
  }

  return NULL;
}

STATIC
VOID
EFIAPI
Field (
  IN SMB_CTX       *C,
  IN CONST CHAR16  *Name,
  IN CONST CHAR16  *Fmt,
  ...
  )
{
  VA_LIST  Args;
  CHAR16   Value[200];

  VA_START (Args, Fmt);
  UnicodeVSPrint (Value, sizeof (Value), Fmt, Args);
  VA_END (Args);
  DocAdd (C->Doc, ATTR_NORMAL, L"  %-32s %s", Name, Value);
}

STATIC
VOID
FStr (
  IN SMB_CTX       *C,
  IN UINTN         Offset,
  IN CONST CHAR16  *Name
  )
{
  CHAR8  *Str;

  if (!Has (C, Offset, 1)) {
    return;
  }

  Str = SmbString (C, B (C, Offset));
  if (Str != NULL) {
    Field (C, Name, L"%a", Str);
  } else {
    Field (C, Name, L"(none)");
  }
}

STATIC
VOID
FByte (
  IN SMB_CTX       *C,
  IN UINTN         Offset,
  IN CONST CHAR16  *Name
  )
{
  if (Has (C, Offset, 1)) {
    Field (C, Name, L"%02Xh (%d)", B (C, Offset), B (C, Offset));
  }
}

STATIC
VOID
FWord (
  IN SMB_CTX       *C,
  IN UINTN         Offset,
  IN CONST CHAR16  *Name
  )
{
  if (Has (C, Offset, 2)) {
    Field (C, Name, L"%04Xh (%d)", W (C, Offset), W (C, Offset));
  }
}

STATIC
VOID
FHandle (
  IN SMB_CTX       *C,
  IN UINTN         Offset,
  IN CONST CHAR16  *Name
  )
{
  if (Has (C, Offset, 2)) {
    if ((W (C, Offset) == 0xFFFF) || (W (C, Offset) == 0xFFFE)) {
      Field (C, Name, L"%04Xh (not provided)", W (C, Offset));
    } else {
      Field (C, Name, L"%04Xh", W (C, Offset));
    }
  }
}

STATIC
VOID
FMhz (
  IN SMB_CTX       *C,
  IN UINTN         Offset,
  IN CONST CHAR16  *Name,
  IN CONST CHAR16  *Unit
  )
{
  if (Has (C, Offset, 2)) {
    if (W (C, Offset) == 0) {
      Field (C, Name, L"Unknown");
    } else {
      Field (C, Name, L"%d %s", W (C, Offset), Unit);
    }
  }
}

STATIC
VOID
FEnum (
  IN SMB_CTX             *C,
  IN UINTN               Offset,
  IN CONST CHAR16        *Name,
  IN CONST CHAR16* CONST *Table,
  IN UINTN               Count,
  IN UINT8               Mask
  )
{
  UINT8         Value;
  CONST CHAR16  *Text;

  if (!Has (C, Offset, 1)) {
    return;
  }

  Value = B (C, Offset) & Mask;
  Text  = ((Value < Count) && (Table[Value] != NULL)) ? Table[Value] : L"Unknown";
  Field (C, Name, L"%02Xh  %s", B (C, Offset), Text);
}

#define FENUM(C, Off, Name, Table)  FEnum ((C), (Off), (Name), (Table), ARRAY_SIZE (Table), 0xFF)

STATIC
VOID
FBits (
  IN SMB_CTX             *C,
  IN UINT64              Value,
  IN UINTN               Bits,
  IN CONST CHAR16* CONST *Names
  )
{
  UINTN  Bit;

  for (Bit = 0; Bit < Bits; Bit++) {
    if (((RShiftU64 (Value, Bit) & 1) != 0) && (Names[Bit] != NULL)) {
      DocAdd (C->Doc, ATTR_NORMAL, L"      [x] %s", Names[Bit]);
    }
  }
}

STATIC
VOID
FUuid (
  IN SMB_CTX       *C,
  IN UINTN         Offset,
  IN CONST CHAR16  *Name
  )
{
  UINT8  *U;

  if (!Has (C, Offset, 16)) {
    return;
  }

  //
  // First three fields are little-endian (SMBIOS 2.6+).
  //
  U = C->P + Offset;
  Field (
    C,
    Name,
    L"%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
    ReadUnaligned32 ((UINT32 *)U),
    ReadUnaligned16 ((UINT16 *)(U + 4)),
    ReadUnaligned16 ((UINT16 *)(U + 6)),
    U[8],
    U[9],
    U[10],
    U[11],
    U[12],
    U[13],
    U[14],
    U[15]
    );
}

/**
  Print a size in KB with a friendly unit.
**/
STATIC
VOID
FSizeKb (
  IN SMB_CTX       *C,
  IN CONST CHAR16  *Name,
  IN UINT64        Kb
  )
{
  if ((Kb >= SIZE_1MB) && ((Kb & (SIZE_1MB - 1)) == 0)) {
    Field (C, Name, L"%ld GB", RShiftU64 (Kb, 20));
  } else if ((Kb >= SIZE_1KB) && ((Kb & (SIZE_1KB - 1)) == 0)) {
    Field (C, Name, L"%ld MB", RShiftU64 (Kb, 10));
  } else {
    Field (C, Name, L"%ld KB", Kb);
  }
}

STATIC
UINT64
CacheSizeKb (
  IN UINT32   Raw,
  IN BOOLEAN  Is32
  )
{
  UINT32  Gran;
  UINT32  Val;

  if (Is32) {
    Gran = (Raw & BIT31) ? 64 : 1;
    Val  = Raw & 0x7FFFFFFF;
  } else {
    Gran = (Raw & BIT15) ? 64 : 1;
    Val  = Raw & 0x7FFF;
  }

  return MultU64x32 (Val, Gran);
}

//
// ---- Per-type decoders -------------------------------------------------------------
//

STATIC
VOID
DecodeType0 (
  IN SMB_CTX  *C
  )
{
  UINT16  Ext;

  FStr (C, 0x04, L"Vendor");
  FStr (C, 0x05, L"BIOS Version");
  if (Has (C, 0x06, 2)) {
    Field (C, L"BIOS Starting Address Segment", L"%04Xh", W (C, 0x06));
  }

  FStr (C, 0x08, L"BIOS Release Date");
  if (Has (C, 0x09, 1)) {
    if (B (C, 0x09) == 0xFF) {
      Field (C, L"BIOS ROM Size", L"FFh (see Extended BIOS ROM Size)");
    } else {
      Field (C, L"BIOS ROM Size", L"%d KB", (B (C, 0x09) + 1) * 64);
    }
  }

  if (Has (C, 0x0A, 8)) {
    Field (C, L"BIOS Characteristics", L"%016lXh", Q (C, 0x0A));
    FBits (C, Q (C, 0x0A), 32, mBiosChar);
  }

  if (Has (C, 0x12, 1)) {
    Field (C, L"BIOS Characteristics Ext Byte 1", L"%02Xh", B (C, 0x12));
    FBits (C, B (C, 0x12), 8, mBiosExt1);
  }

  if (Has (C, 0x13, 1)) {
    Field (C, L"BIOS Characteristics Ext Byte 2", L"%02Xh", B (C, 0x13));
    FBits (C, B (C, 0x13), 8, mBiosExt2);
  }

  if (Has (C, 0x15, 1)) {
    Field (C, L"System BIOS Release", L"%d.%d", B (C, 0x14), B (C, 0x15));
  }

  if (Has (C, 0x17, 1)) {
    if ((B (C, 0x16) == 0xFF) && (B (C, 0x17) == 0xFF)) {
      Field (C, L"Embedded Controller Firmware", L"Not supported");
    } else {
      Field (C, L"Embedded Controller Firmware", L"%d.%d", B (C, 0x16), B (C, 0x17));
    }
  }

  if (Has (C, 0x18, 2)) {
    Ext = W (C, 0x18);
    Field (C, L"Extended BIOS ROM Size", L"%d %s", Ext & 0x3FFF, ((Ext >> 14) == 1) ? L"GB" : L"MB");
  }
}

STATIC
VOID
DecodeType1 (
  IN SMB_CTX  *C
  )
{
  FStr (C, 0x04, L"Manufacturer");
  FStr (C, 0x05, L"Product Name");
  FStr (C, 0x06, L"Version");
  FStr (C, 0x07, L"Serial Number");
  FUuid (C, 0x08, L"UUID");
  FENUM (C, 0x18, L"Wake-up Type", mWakeUpTypes);
  FStr (C, 0x19, L"SKU Number");
  FStr (C, 0x1A, L"Family");
}

STATIC
VOID
DecodeType2 (
  IN SMB_CTX  *C
  )
{
  UINT8  Flags;
  UINTN  Index;

  FStr (C, 0x04, L"Manufacturer");
  FStr (C, 0x05, L"Product");
  FStr (C, 0x06, L"Version");
  FStr (C, 0x07, L"Serial Number");
  FStr (C, 0x08, L"Asset Tag");
  if (Has (C, 0x09, 1)) {
    Flags = B (C, 0x09);
    Field (C, L"Feature Flags", L"%02Xh", Flags);
    if (Flags & BIT0) {
      DocAdd (C->Doc, ATTR_NORMAL, L"      [x] Hosting board (motherboard)");
    }

    if (Flags & BIT1) {
      DocAdd (C->Doc, ATTR_NORMAL, L"      [x] Requires at least one daughter board");
    }

    if (Flags & BIT2) {
      DocAdd (C->Doc, ATTR_NORMAL, L"      [x] Removable");
    }

    if (Flags & BIT3) {
      DocAdd (C->Doc, ATTR_NORMAL, L"      [x] Replaceable");
    }

    if (Flags & BIT4) {
      DocAdd (C->Doc, ATTR_NORMAL, L"      [x] Hot swappable");
    }
  }

  FStr (C, 0x0A, L"Location in Chassis");
  FHandle (C, 0x0B, L"Chassis Handle");
  FENUM (C, 0x0D, L"Board Type", mBoardTypes);
  if (Has (C, 0x0E, 1)) {
    Field (C, L"Number of Contained Object Handles", L"%d", B (C, 0x0E));
    for (Index = 0; Index < B (C, 0x0E) && Has (C, 0x0F + Index * 2, 2); Index++) {
      Field (C, L"  Contained Handle", L"%04Xh", W (C, 0x0F + Index * 2));
    }
  }
}

STATIC
VOID
DecodeType3 (
  IN SMB_CTX  *C
  )
{
  UINTN  SkuOff;

  FStr (C, 0x04, L"Manufacturer");
  if (Has (C, 0x05, 1)) {
    Field (
      C,
      L"Type",
      L"%02Xh  %s%s",
      B (C, 0x05),
      ENUM_NAME (mChassisTypes, B (C, 0x05) & 0x7F),
      (B (C, 0x05) & 0x80) ? L", lock present" : L""
      );
  }

  FStr (C, 0x06, L"Version");
  FStr (C, 0x07, L"Serial Number");
  FStr (C, 0x08, L"Asset Tag Number");
  FENUM (C, 0x09, L"Boot-up State", mChassisStates);
  FENUM (C, 0x0A, L"Power Supply State", mChassisStates);
  FENUM (C, 0x0B, L"Thermal State", mChassisStates);
  FENUM (C, 0x0C, L"Security Status", mChassisSecurity);
  if (Has (C, 0x0D, 4)) {
    Field (C, L"OEM-defined", L"%08Xh", D (C, 0x0D));
  }

  if (Has (C, 0x11, 1)) {
    if (B (C, 0x11) == 0) {
      Field (C, L"Height", L"Unspecified");
    } else {
      Field (C, L"Height", L"%dU", B (C, 0x11));
    }
  }

  FByte (C, 0x12, L"Number of Power Cords");
  FByte (C, 0x13, L"Contained Element Count");
  FByte (C, 0x14, L"Contained Element Record Length");
  if (Has (C, 0x14, 1)) {
    SkuOff = 0x15 + (UINTN)B (C, 0x13) * B (C, 0x14);
    FStr (C, SkuOff, L"SKU Number");
  }
}

STATIC
VOID
DecodeType4 (
  IN SMB_CTX  *C
  )
{
  UINT8   Volt;
  UINT8   Status;
  UINT32  Eax;

  FStr (C, 0x04, L"Socket Designation");
  FENUM (C, 0x05, L"Processor Type", mProcTypes);
  FByte (C, 0x06, L"Processor Family");
  FStr (C, 0x07, L"Processor Manufacturer");
  if (Has (C, 0x08, 8)) {
    Eax = D (C, 0x08);
    Field (C, L"Processor ID", L"%08X %08X", D (C, 0x0C), Eax);
    Field (
      C,
      L"  Signature",
      L"Family %Xh, Model %Xh, Stepping %Xh",
      ((Eax >> 8) & 0xF) + (((Eax >> 8) & 0xF) == 0xF ? ((Eax >> 20) & 0xFF) : 0),
      ((Eax >> 4) & 0xF) | ((Eax >> 12) & 0xF0),
      Eax & 0xF
      );
  }

  FStr (C, 0x10, L"Processor Version");
  if (Has (C, 0x11, 1)) {
    Volt = B (C, 0x11);
    if (Volt & BIT7) {
      Field (C, L"Voltage", L"%d.%d V", (Volt & 0x7F) / 10, (Volt & 0x7F) % 10);
    } else {
      Field (
        C,
        L"Voltage",
        L"%02Xh  %s%s%s",
        Volt,
        (Volt & BIT0) ? L"5V " : L"",
        (Volt & BIT1) ? L"3.3V " : L"",
        (Volt & BIT2) ? L"2.9V" : L""
        );
    }
  }

  FMhz (C, 0x12, L"External Clock", L"MHz");
  FMhz (C, 0x14, L"Max Speed", L"MHz");
  FMhz (C, 0x16, L"Current Speed", L"MHz");
  if (Has (C, 0x18, 1)) {
    Status = B (C, 0x18);
    Field (
      C,
      L"Status",
      L"%02Xh  Socket %s, %s",
      Status,
      (Status & BIT6) ? L"Populated" : L"Unpopulated",
      mProcStatus[Status & 7]
      );
  }

  FByte (C, 0x19, L"Processor Upgrade");
  FHandle (C, 0x1A, L"L1 Cache Handle");
  FHandle (C, 0x1C, L"L2 Cache Handle");
  FHandle (C, 0x1E, L"L3 Cache Handle");
  FStr (C, 0x20, L"Serial Number");
  FStr (C, 0x21, L"Asset Tag");
  FStr (C, 0x22, L"Part Number");
  FByte (C, 0x23, L"Core Count");
  FByte (C, 0x24, L"Core Enabled");
  FByte (C, 0x25, L"Thread Count");
  if (Has (C, 0x26, 2)) {
    Field (C, L"Processor Characteristics", L"%04Xh", W (C, 0x26));
    FBits (C, W (C, 0x26), 16, mProcChar);
  }

  FWord (C, 0x28, L"Processor Family 2");
  FWord (C, 0x2A, L"Core Count 2");
  FWord (C, 0x2C, L"Core Enabled 2");
  FWord (C, 0x2E, L"Thread Count 2");
  FWord (C, 0x30, L"Thread Enabled");
}

STATIC
VOID
DecodeType7 (
  IN SMB_CTX  *C
  )
{
  UINT16                     Cfg;
  STATIC CONST CHAR16* CONST Loc[]  = { L"Internal", L"External", L"Reserved", L"Unknown" };
  STATIC CONST CHAR16* CONST Mode[] = { L"Write Through", L"Write Back", L"Varies with Memory Address", L"Unknown" };

  FStr (C, 0x04, L"Socket Designation");
  if (Has (C, 0x05, 2)) {
    Cfg = W (C, 0x05);
    Field (C, L"Cache Configuration", L"%04Xh", Cfg);
    Field (C, L"  Level", L"L%d", (Cfg & 7) + 1);
    Field (C, L"  Socketed", L"%s", (Cfg & BIT3) ? L"Yes" : L"No");
    Field (C, L"  Location", L"%s", Loc[(Cfg >> 5) & 3]);
    Field (C, L"  Enabled", L"%s", (Cfg & BIT7) ? L"Yes" : L"No");
    Field (C, L"  Operational Mode", L"%s", Mode[(Cfg >> 8) & 3]);
  }

  if (Has (C, 0x07, 2)) {
    FSizeKb (C, L"Maximum Cache Size", CacheSizeKb (W (C, 0x07), FALSE));
  }

  if (Has (C, 0x09, 2)) {
    FSizeKb (C, L"Installed Size", CacheSizeKb (W (C, 0x09), FALSE));
  }

  if (Has (C, 0x0B, 2)) {
    Field (C, L"Supported SRAM Type", L"%04Xh", W (C, 0x0B));
  }

  if (Has (C, 0x0D, 2)) {
    Field (C, L"Current SRAM Type", L"%04Xh", W (C, 0x0D));
  }

  if (Has (C, 0x0F, 1)) {
    if (B (C, 0x0F) == 0) {
      Field (C, L"Cache Speed", L"Unknown");
    } else {
      Field (C, L"Cache Speed", L"%d ns", B (C, 0x0F));
    }
  }

  FENUM (C, 0x10, L"Error Correction Type", mEccTypes);
  FENUM (C, 0x11, L"System Cache Type", mCacheTypes);
  FENUM (C, 0x12, L"Associativity", mCacheAssoc);
  if (Has (C, 0x13, 4)) {
    FSizeKb (C, L"Maximum Cache Size 2", CacheSizeKb (D (C, 0x13), TRUE));
  }

  if (Has (C, 0x17, 4)) {
    FSizeKb (C, L"Installed Cache Size 2", CacheSizeKb (D (C, 0x17), TRUE));
  }
}

STATIC
VOID
DecodeType8 (
  IN SMB_CTX  *C
  )
{
  FStr (C, 0x04, L"Internal Reference Designator");
  FByte (C, 0x05, L"Internal Connector Type");
  FStr (C, 0x06, L"External Reference Designator");
  FByte (C, 0x07, L"External Connector Type");
  FByte (C, 0x08, L"Port Type");
}

STATIC
VOID
DecodeType9 (
  IN SMB_CTX  *C
  )
{
  FStr (C, 0x04, L"Slot Designation");
  FByte (C, 0x05, L"Slot Type");
  FENUM (C, 0x06, L"Slot Data Bus Width", mSlotWidth);
  FENUM (C, 0x07, L"Current Usage", mSlotUsage);
  FByte (C, 0x08, L"Slot Length");
  FWord (C, 0x09, L"Slot ID");
  FByte (C, 0x0B, L"Slot Characteristics 1");
  FByte (C, 0x0C, L"Slot Characteristics 2");
  if (Has (C, 0x11, 1)) {
    Field (
      C,
      L"Segment:Bus:Dev.Fn",
      L"%04X:%02X:%02X.%X",
      W (C, 0x0D),
      B (C, 0x0F),
      B (C, 0x10) >> 3,
      B (C, 0x10) & 7
      );
  }
}

STATIC
VOID
DecodeStringList (
  IN SMB_CTX       *C,
  IN CONST CHAR16  *CountName
  )
{
  UINT8  Index;
  CHAR8  *Str;

  if (!Has (C, 0x04, 1)) {
    return;
  }

  Field (C, CountName, L"%d", B (C, 0x04));
  for (Index = 1; Index <= B (C, 0x04); Index++) {
    Str = SmbString (C, Index);
    DocAdd (C->Doc, ATTR_NORMAL, L"      %2d: %a", Index, (Str != NULL) ? Str : "(missing)");
  }
}

STATIC
VOID
DecodeType13 (
  IN SMB_CTX  *C
  )
{
  FByte (C, 0x04, L"Installable Languages");
  if (Has (C, 0x05, 1)) {
    Field (C, L"Language Format", L"%s", (B (C, 0x05) & BIT0) ? L"Abbreviated" : L"Long");
  }

  FStr (C, 0x15, L"Current Language");
}

STATIC
VOID
DecodeType16 (
  IN SMB_CTX  *C
  )
{
  FENUM (C, 0x04, L"Location", mArrayLocation);
  FENUM (C, 0x05, L"Use", mArrayUse);
  FENUM (C, 0x06, L"Memory Error Correction", mEccTypes);
  if (Has (C, 0x07, 4)) {
    if ((D (C, 0x07) == 0x80000000) && Has (C, 0x0F, 8)) {
      FSizeKb (C, L"Maximum Capacity", RShiftU64 (Q (C, 0x0F), 10));
    } else {
      FSizeKb (C, L"Maximum Capacity", D (C, 0x07));
    }
  }

  FHandle (C, 0x0B, L"Memory Error Information Handle");
  FWord (C, 0x0D, L"Number of Memory Devices");
}

STATIC
VOID
DecodeType17 (
  IN SMB_CTX  *C
  )
{
  UINT16  Size;
  UINT16  Detail;
  UINTN   Bit;

  FHandle (C, 0x04, L"Physical Memory Array Handle");
  FHandle (C, 0x06, L"Memory Error Information Handle");
  if (Has (C, 0x08, 2)) {
    Field (C, L"Total Width", (W (C, 0x08) == 0xFFFF) ? L"Unknown" : L"%d bits", W (C, 0x08));
  }

  if (Has (C, 0x0A, 2)) {
    Field (C, L"Data Width", (W (C, 0x0A) == 0xFFFF) ? L"Unknown" : L"%d bits", W (C, 0x0A));
  }

  if (Has (C, 0x0C, 2)) {
    Size = W (C, 0x0C);
    if (Size == 0) {
      Field (C, L"Size", L"No Module Installed");
    } else if (Size == 0xFFFF) {
      Field (C, L"Size", L"Unknown");
    } else if ((Size == 0x7FFF) && Has (C, 0x1C, 4)) {
      FSizeKb (C, L"Size", LShiftU64 (D (C, 0x1C) & 0x7FFFFFFF, 10));
    } else if (Size & BIT15) {
      FSizeKb (C, L"Size", Size & 0x7FFF);
    } else {
      FSizeKb (C, L"Size", LShiftU64 (Size, 10));
    }
  }

  FENUM (C, 0x0E, L"Form Factor", mMemFormFactors);
  FByte (C, 0x0F, L"Device Set");
  FStr (C, 0x10, L"Device Locator");
  FStr (C, 0x11, L"Bank Locator");
  FENUM (C, 0x12, L"Memory Type", mMemTypes);
  if (Has (C, 0x13, 2)) {
    Detail = W (C, 0x13);
    Field (C, L"Type Detail", L"%04Xh", Detail);
    for (Bit = 1; Bit < 16; Bit++) {
      if (Detail & (1 << Bit)) {
        DocAdd (C->Doc, ATTR_NORMAL, L"      [x] %s", mMemTypeDetail[Bit]);
      }
    }
  }

  if (Has (C, 0x15, 2)) {
    if ((W (C, 0x15) == 0xFFFF) && Has (C, 0x54, 4)) {
      Field (C, L"Speed", L"%d MT/s", D (C, 0x54));
    } else {
      FMhz (C, 0x15, L"Speed", L"MT/s");
    }
  }

  FStr (C, 0x17, L"Manufacturer");
  FStr (C, 0x18, L"Serial Number");
  FStr (C, 0x19, L"Asset Tag");
  FStr (C, 0x1A, L"Part Number");
  if (Has (C, 0x1B, 1)) {
    if ((B (C, 0x1B) & 0x0F) == 0) {
      Field (C, L"Rank", L"Unknown");
    } else {
      Field (C, L"Rank", L"%d", B (C, 0x1B) & 0x0F);
    }
  }

  if (Has (C, 0x20, 2)) {
    if ((W (C, 0x20) == 0xFFFF) && Has (C, 0x58, 4)) {
      Field (C, L"Configured Memory Speed", L"%d MT/s", D (C, 0x58));
    } else {
      FMhz (C, 0x20, L"Configured Memory Speed", L"MT/s");
    }
  }

  FMhz (C, 0x22, L"Minimum Voltage", L"mV");
  FMhz (C, 0x24, L"Maximum Voltage", L"mV");
  FMhz (C, 0x26, L"Configured Voltage", L"mV");
  FByte (C, 0x28, L"Memory Technology");
  FWord (C, 0x2B, L"Module Manufacturer ID");
  FWord (C, 0x2D, L"Module Product ID");
  FWord (C, 0x2F, L"Memory Subsystem Ctrl Mfr ID");
  FWord (C, 0x31, L"Memory Subsystem Ctrl Product ID");
}

STATIC
VOID
DecodeType19 (
  IN SMB_CTX  *C
  )
{
  if (!Has (C, 0x0F, 1)) {
    return;
  }

  if ((D (C, 0x04) == 0xFFFFFFFF) && Has (C, 0x17, 8)) {
    Field (C, L"Starting Address", L"%016lXh", Q (C, 0x0F));
    Field (C, L"Ending Address", L"%016lXh", Q (C, 0x17));
    FSizeKb (C, L"Range Size", RShiftU64 (Q (C, 0x17) - Q (C, 0x0F) + 1, 10));
  } else {
    Field (C, L"Starting Address", L"%016lXh", LShiftU64 (D (C, 0x04), 10));
    Field (C, L"Ending Address", L"%016lXh", LShiftU64 (D (C, 0x08), 10) + 0x3FF);
    FSizeKb (C, L"Range Size", (UINT64)D (C, 0x08) - D (C, 0x04) + 1);
  }

  FHandle (C, 0x0C, L"Memory Array Handle");
  FByte (C, 0x0E, L"Partition Width");
}

STATIC
VOID
DecodeType32 (
  IN SMB_CTX  *C
  )
{
  if (Has (C, 0x0A, 1)) {
    Field (C, L"Boot Status", L"%02Xh  %s", B (C, 0x0A), (B (C, 0x0A) == 0) ? L"No errors detected" : L"See specification");
  }
}

STATIC
VOID
DecodeType41 (
  IN SMB_CTX  *C
  )
{
  FStr (C, 0x04, L"Reference Designation");
  if (Has (C, 0x05, 1)) {
    Field (
      C,
      L"Device Type",
      L"%02Xh  %s, %s",
      B (C, 0x05),
      ENUM_NAME (mOnboardTypes, B (C, 0x05) & 0x7F),
      (B (C, 0x05) & BIT7) ? L"Enabled" : L"Disabled"
      );
  }

  FByte (C, 0x06, L"Device Type Instance");
  if (Has (C, 0x0A, 1)) {
    Field (
      C,
      L"Segment:Bus:Dev.Fn",
      L"%04X:%02X:%02X.%X",
      W (C, 0x07),
      B (C, 0x09),
      B (C, 0x0A) >> 3,
      B (C, 0x0A) & 7
      );
  }
}

//
// ---- Screens -------------------------------------------------------------------
//

STATIC
VOID
ShowStruct (
  IN UINTN  Index
  )
{
  TEXT_DOC  Doc;
  SMB_CTX   Ctx;
  CHAR16    Header[160];
  UINT8     Type;
  CHAR8     *Str;
  UINTN     Count;

  Ctx.P   = mStructs[Index].Ptr;
  Ctx.Len = Ctx.P[1];
  Ctx.Doc = &Doc;
  Type    = Ctx.P[0];
  DocInit (&Doc);

  UnicodeSPrint (
    Header,
    sizeof (Header),
    L"Type %d (%02Xh)  %s   Handle %04Xh   Length %02Xh   @ %p",
    Type,
    Type,
    TypeName (Type),
    ReadUnaligned16 ((UINT16 *)(Ctx.P + 2)),
    Ctx.P[1],
    Ctx.P
    );

  DocAdd (&Doc, ATTR_HEADER, L"Decoded fields");
  switch (Type) {
    case 0: DecodeType0 (&Ctx);
      break;
    case 1: DecodeType1 (&Ctx);
      break;
    case 2: DecodeType2 (&Ctx);
      break;
    case 3: DecodeType3 (&Ctx);
      break;
    case 4: DecodeType4 (&Ctx);
      break;
    case 7: DecodeType7 (&Ctx);
      break;
    case 8: DecodeType8 (&Ctx);
      break;
    case 9: DecodeType9 (&Ctx);
      break;
    case 11: DecodeStringList (&Ctx, L"Count");
      break;
    case 12: DecodeStringList (&Ctx, L"Count");
      break;
    case 13: DecodeType13 (&Ctx);
      break;
    case 16: DecodeType16 (&Ctx);
      break;
    case 17: DecodeType17 (&Ctx);
      break;
    case 19: DecodeType19 (&Ctx);
      break;
    case 32: DecodeType32 (&Ctx);
      break;
    case 41: DecodeType41 (&Ctx);
      break;
    default:
      DocAdd (&Doc, ATTR_DIM, L"  (no decoder for this type; see raw data)");
      break;
  }

  DocAdd (&Doc, ATTR_NORMAL, L"");
  DocAdd (&Doc, ATTR_HEADER, L"Strings");
  Count = 0;
  while ((Str = SmbString (&Ctx, (UINT8)(Count + 1))) != NULL) {
    DocAdd (&Doc, ATTR_NORMAL, L"  %2d: %a", Count + 1, Str);
    Count++;
    if (Count >= 255) {
      break;
    }
  }

  if (Count == 0) {
    DocAdd (&Doc, ATTR_DIM, L"  (none)");
  }

  DocAdd (&Doc, ATTR_NORMAL, L"");
  DocAdd (&Doc, ATTR_HEADER, L"Raw data (formatted area, %d bytes)", Ctx.Len);
  DocAddHex (&Doc, Ctx.P, Ctx.Len, 0);
  if (mStructs[Index].Total > Ctx.Len) {
    DocAdd (&Doc, ATTR_HEADER, L"Raw data (string set, %d bytes)", mStructs[Index].Total - Ctx.Len);
    DocAddHex (&Doc, Ctx.P + Ctx.Len, mStructs[Index].Total - Ctx.Len, Ctx.Len);
  }

  DocView (L"SMBIOS Structure", Header, &Doc);
  DocFree (&Doc);
}

STATIC CONST CHAR16  *mSmbiosHelp[] = {
  L"Up/Down       Select a structure",
  L"Enter         Decoded fields, strings and raw bytes",
  L"F5            Reload the tables"
};

VOID
SmbiosViewRun (
  VOID
  )
{
  TEXT_DOC       Doc;
  UINTN          Index;
  UINTN          Sel;
  INTN           Pick;
  UINT8          *P;
  EFI_INPUT_KEY  Key;

  if (!LoadTables ()) {
    UiFrame (L"SMBIOS", L"Esc: back");
    UiPrintAt (2, 3, ATTR_WARN, L" No SMBIOS table found in the EFI configuration table. ");
    do {
      UiReadKey (&Key);
    } while (Key.ScanCode != SCAN_ESC);

    return;
  }

  Sel = 0;
  for ( ; ;) {
    DocInit (&Doc);
    DocAdd (&Doc, ATTR_HEADER, L" Type  Handle  Len  Description");
    for (Index = 0; Index < mStructCount; Index++) {
      P = mStructs[Index].Ptr;
      DocAddItem (
        &Doc,
        (INTN)Index,
        L" %3d   %04X    %02X   %s",
        P[0],
        ReadUnaligned16 ((UINT16 *)(P + 2)),
        P[1],
        TypeName (P[0])
        );
    }

    UiSetHelp (mSmbiosHelp, ARRAY_SIZE (mSmbiosHelp));
    Pick = DocSelect (
             L"SMBIOS",
             L"Enter:open  F5:reload  F1:help  Esc:back",
             mEntryInfo,
             &Doc,
             &Sel
             );
    DocFree (&Doc);
    if (Pick == -1) {
      return;
    }

    if (Pick == -2) {
      LoadTables ();
      continue;
    }

    ShowStruct ((UINTN)Pick);
  }
}
