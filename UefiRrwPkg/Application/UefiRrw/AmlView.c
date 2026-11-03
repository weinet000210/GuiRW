/** @file
  AML disassembler: turns DSDT/SSDT definition blocks into ASL source text
  (classic ASL 1.0 operator syntax), written from the ACPI specification's
  AML grammar (ACPI 6.5, chapter 20).

  Method invocations in AML carry no argument count, so a silent first pass
  over every DSDT/SSDT collects Method and External declarations; the
  display pass looks names up against that table using the ACPI namespace
  search rules.

  Copyright (c) 2026, weinet. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "UefiRrw.h"

#define AML_MAX_PATH   128         // 32 name segments
#define AML_MAX_DEPTH  96
#define AML_LINE_MAX   300

typedef struct {
  CHAR16    *B;
  UINTN     Len;
  UINTN     Cap;
} SB;

typedef struct {
  CHAR8    Path[AML_MAX_PATH];
  UINT8    Len;
  UINT8    Args;
} AML_METHOD;

typedef struct {
  UINT8           Op;
  CONST CHAR16    *Name;
  CONST CHAR8     *Args;   // a TermArg, s SuperName, t Target, n NameString, b/w/d constant
} AML_OP;

STATIC UINT8       *mBase;
STATIC UINT8       *mP;
STATIC BOOLEAN     mSilent;
STATIC BOOLEAN     mPrepass;
STATIC TEXT_DOC    *mDoc;
STATIC UINTN       mDepth;
STATIC CHAR8       mScope[AML_MAX_PATH];
STATIC UINTN       mScopeLen;
STATIC BOOLEAN     mElseIf;      // next If prints as ElseIf
STATIC BOOLEAN     mUuidHint;    // operands of LEqual/LNotEqual: 16-byte buffer = ToUUID
STATIC AML_METHOD  *mMethods;
STATIC UINTN       mMethodCount;
STATIC UINTN       mMethodCap;

STATIC CONST AML_OP  mOps[] = {
  { 0x70, L"Store",                  "as"     },
  { 0x71, L"RefOf",                  "s"      },
  { 0x72, L"Add",                    "aat"    },
  { 0x73, L"Concatenate",            "aat"    },
  { 0x74, L"Subtract",               "aat"    },
  { 0x75, L"Increment",              "s"      },
  { 0x76, L"Decrement",              "s"      },
  { 0x77, L"Multiply",               "aat"    },
  { 0x78, L"Divide",                 "aatt"   },
  { 0x79, L"ShiftLeft",              "aat"    },
  { 0x7A, L"ShiftRight",             "aat"    },
  { 0x7B, L"And",                    "aat"    },
  { 0x7C, L"NAnd",                   "aat"    },
  { 0x7D, L"Or",                     "aat"    },
  { 0x7E, L"NOr",                    "aat"    },
  { 0x7F, L"XOr",                    "aat"    },
  { 0x80, L"Not",                    "at"     },
  { 0x81, L"FindSetLeftBit",         "at"     },
  { 0x82, L"FindSetRightBit",        "at"     },
  { 0x83, L"DerefOf",                "a"      },
  { 0x84, L"ConcatenateResTemplate", "aat"    },
  { 0x85, L"Mod",                    "aat"    },
  { 0x86, L"Notify",                 "sa"     },
  { 0x87, L"SizeOf",                 "s"      },
  { 0x88, L"Index",                  "aat"    },
  { 0x8A, L"CreateDWordField",       "aan"    },
  { 0x8B, L"CreateWordField",        "aan"    },
  { 0x8C, L"CreateByteField",        "aan"    },
  { 0x8D, L"CreateBitField",         "aan"    },
  { 0x8E, L"ObjectType",             "s"      },
  { 0x8F, L"CreateQWordField",       "aan"    },
  { 0x90, L"LAnd",                   "aa"     },
  { 0x91, L"LOr",                    "aa"     },
  { 0x92, L"LNot",                   "a"      },
  { 0x93, L"LEqual",                 "aa"     },
  { 0x94, L"LGreater",               "aa"     },
  { 0x95, L"LLess",                  "aa"     },
  { 0x96, L"ToBuffer",               "at"     },
  { 0x97, L"ToDecimalString",        "at"     },
  { 0x98, L"ToHexString",            "at"     },
  { 0x99, L"ToInteger",              "at"     },
  { 0x9C, L"ToString",               "aat"    },
  { 0x9D, L"CopyObject",             "as"     },
  { 0x9E, L"Mid",                    "aaat"   },
  { 0x9F, L"Continue",               ""       },
  { 0xA3, L"Noop",                   ""       },
  { 0xA4, L"Return",                 "a"      },
  { 0xA5, L"Break",                  ""       },
  { 0xCC, L"BreakPoint",             ""       },
  { 0x06, L"Alias",                  "nn"     }
};

STATIC CONST AML_OP  mExtOps[] = {
  { 0x01, L"Mutex",      "nb"     },
  { 0x02, L"Event",      "n"      },
  { 0x12, L"CondRefOf",  "st"     },
  { 0x13, L"CreateField", "aaan"  },
  { 0x1F, L"LoadTable",  "aaaaaa" },
  { 0x20, L"Load",       "st"     },
  { 0x21, L"Stall",      "a"      },
  { 0x22, L"Sleep",      "a"      },
  { 0x23, L"Acquire",    "sw"     },
  { 0x24, L"Signal",     "s"      },
  { 0x25, L"Wait",       "sa"     },
  { 0x26, L"Reset",      "s"      },
  { 0x27, L"Release",    "s"      },
  { 0x28, L"FromBCD",    "at"     },
  { 0x29, L"ToBCD",      "at"     },
  { 0x30, L"Revision",   ""       },
  { 0x31, L"Debug",      ""       },
  { 0x32, L"Fatal",      "bda"    },
  { 0x33, L"Timer",      ""       },
  { 0x88, L"DataRegion", "naaa"   }
};

STATIC CONST CHAR16  *mRegionSpace[] = {
  L"SystemMemory", L"SystemIO", L"PCI_Config", L"EmbeddedControl", L"SMBus", L"SystemCMOS",
  L"PciBarTarget", L"IPMI", L"GeneralPurposeIo", L"GenericSerialBus", L"PCC", L"PRM", L"FFixedHW"
};
STATIC CONST CHAR16  *mAccess[]   = { L"AnyAcc", L"ByteAcc", L"WordAcc", L"DWordAcc", L"QWordAcc", L"BufferAcc", L"AnyAcc", L"AnyAcc" };
STATIC CONST CHAR16  *mUpdate[]   = { L"Preserve", L"WriteAsOnes", L"WriteAsZeros", L"Preserve" };
STATIC CONST CHAR16  *mObjType[]  = {
  L"UnknownObj", L"IntObj", L"StrObj", L"BuffObj", L"PkgObj", L"FieldUnitObj", L"DeviceObj", L"EventObj",
  L"MethodObj", L"MutexObj", L"OpRegionObj", L"PowerResObj", L"ProcessorObj", L"ThermalZoneObj", L"BuffFieldObj", L"DDBHandleObj"
};
STATIC CONST CHAR16  *mMatchOp[]  = { L"MTR", L"MEQ", L"MLE", L"MLT", L"MGE", L"MGT" };

STATIC BOOLEAN ParseArg (UINT8 *End, SB *Out, BOOLEAN AllowInvoke);
STATIC BOOLEAN ParseTermList (UINT8 *End, UINTN Indent);

//
// ---- String builder --------------------------------------------------------
//

STATIC
VOID
SbPutN (
  IN OUT SB          *S,
  IN     CONST CHAR16  *Str,
  IN     UINTN         N
  )
{
  CHAR16  *Grown;
  UINTN   NewCap;

  if (S->Len + N + 1 > S->Cap) {
    NewCap = MAX (S->Cap * 2, S->Len + N + 64);
    Grown  = ReallocatePool (S->Cap * sizeof (CHAR16), NewCap * sizeof (CHAR16), S->B);
    if (Grown == NULL) {
      return;
    }

    S->B   = Grown;
    S->Cap = NewCap;
  }

  CopyMem (S->B + S->Len, Str, N * sizeof (CHAR16));
  S->Len        += N;
  S->B[S->Len]   = L'\0';
}

STATIC
VOID
SbPut (
  IN OUT SB            *S,
  IN     CONST CHAR16  *Str
  )
{
  SbPutN (S, Str, StrLen (Str));
}

STATIC
VOID
EFIAPI
SbFmt (
  IN OUT SB            *S,
  IN     CONST CHAR16  *Fmt,
  ...
  )
{
  VA_LIST  Args;
  CHAR16   Tmp[256];

  VA_START (Args, Fmt);
  UnicodeVSPrint (Tmp, sizeof (Tmp), Fmt, Args);
  VA_END (Args);
  SbPut (S, Tmp);
}

/**
  Append Src, putting Spaces spaces after every newline in it.
**/
STATIC
VOID
SbPutIndented (
  IN OUT SB      *S,
  IN     SB      *Src,
  IN     UINTN   Spaces
  )
{
  UINTN  Start;
  UINTN  Index;
  UINTN  K;

  if (Src->B == NULL) {
    return;
  }

  Start = 0;
  for (Index = 0; Index <= Src->Len; Index++) {
    if ((Index == Src->Len) || (Src->B[Index] == L'\n')) {
      SbPutN (S, Src->B + Start, Index - Start);
      if (Index < Src->Len) {
        SbPut (S, L"\n");
        for (K = 0; K < Spaces; K++) {
          SbPut (S, L" ");
        }
      }

      Start = Index + 1;
    }
  }
}

STATIC
VOID
SbFree (
  IN OUT SB  *S
  )
{
  if (S->B != NULL) {
    FreePool (S->B);
  }

  ZeroMem (S, sizeof (*S));
}

STATIC
CONST CHAR16 *
SbStr (
  IN SB  *S
  )
{
  return (S->B != NULL) ? S->B : L"";
}

//
// ---- Output ----------------------------------------------------------------
//

/**
  Emit text (may hold newlines) at Indent levels of four spaces.
**/
STATIC
VOID
Emit (
  IN UINTN         Indent,
  IN UINT8         Attr,
  IN CONST CHAR16  *Text
  )
{
  CHAR16  Line[AML_LINE_MAX + 1];
  UINTN   Pos;
  UINTN   Start;
  UINTN   Index;
  UINTN   Len;

  if (mSilent || (mDoc == NULL)) {
    return;
  }

  Len   = StrLen (Text);
  Start = 0;
  for (Index = 0; Index <= Len; Index++) {
    if ((Index == Len) || (Text[Index] == L'\n')) {
      for (Pos = 0; (Pos < Indent * 4) && (Pos < AML_LINE_MAX); Pos++) {
        Line[Pos] = L' ';
      }

      while ((Start < Index) && (Pos < AML_LINE_MAX)) {
        if (Text[Start] != L'\r') {
          Line[Pos++] = Text[Start];
        }

        Start++;
      }

      Line[Pos] = L'\0';
      DocAdd (mDoc, Attr, L"%s", Line);
      Start = Index + 1;
    }
  }
}

STATIC
VOID
EFIAPI
EmitFmt (
  IN UINTN         Indent,
  IN UINT8         Attr,
  IN CONST CHAR16  *Fmt,
  ...
  )
{
  VA_LIST  Args;
  CHAR16   Tmp[AML_LINE_MAX];

  if (mSilent) {
    return;
  }

  VA_START (Args, Fmt);
  UnicodeVSPrint (Tmp, sizeof (Tmp), Fmt, Args);
  VA_END (Args);
  Emit (Indent, Attr, Tmp);
}

//
// ---- Method table ----------------------------------------------------------
//

STATIC
VOID
AddMethod (
  IN CONST CHAR8  *Path,
  IN UINTN        Len,
  IN UINT8        Args
  )
{
  UINTN       Index;
  AML_METHOD  *Grown;
  UINTN       NewCap;

  if ((Len == 0) || (Len > AML_MAX_PATH)) {
    return;
  }

  for (Index = 0; Index < mMethodCount; Index++) {
    if ((mMethods[Index].Len == Len) && (CompareMem (mMethods[Index].Path, Path, Len) == 0)) {
      mMethods[Index].Args = Args;
      return;
    }
  }

  if (mMethodCount == mMethodCap) {
    NewCap = (mMethodCap == 0) ? 256 : mMethodCap * 2;
    Grown  = ReallocatePool (mMethodCap * sizeof (AML_METHOD), NewCap * sizeof (AML_METHOD), mMethods);
    if (Grown == NULL) {
      return;
    }

    mMethods   = Grown;
    mMethodCap = NewCap;
  }

  CopyMem (mMethods[mMethodCount].Path, Path, Len);
  mMethods[mMethodCount].Len  = (UINT8)Len;
  mMethods[mMethodCount].Args = Args;
  mMethodCount++;
}

STATIC
INTN
FindExact (
  IN CONST CHAR8  *Path,
  IN UINTN        Len
  )
{
  UINTN  Index;

  for (Index = 0; Index < mMethodCount; Index++) {
    if ((mMethods[Index].Len == Len) && (CompareMem (mMethods[Index].Path, Path, Len) == 0)) {
      return mMethods[Index].Args;
    }
  }

  return -1;
}

/**
  Argument count of the method a name refers to, or -1 when it names no
  known method.
**/
STATIC
INTN
LookupMethod (
  IN CONST CHAR8  *Abs,
  IN UINTN        AbsLen,
  IN BOOLEAN      Single
  )
{
  CHAR8  Try[AML_MAX_PATH];
  UINTN  S;
  UINTN  Index;
  INTN   Args;

  if (AbsLen < 4) {
    return -1;
  }

  if (!Single) {
    return FindExact (Abs, AbsLen);
  }

  //
  // Single segment: search the current scope, then each parent, up to root.
  //
  S = mScopeLen;
  for ( ; ;) {
    if (S + 4 <= AML_MAX_PATH) {
      CopyMem (Try, mScope, S);
      CopyMem (Try + S, Abs + AbsLen - 4, 4);
      Args = FindExact (Try, S + 4);
      if (Args >= 0) {
        return Args;
      }
    }

    if (S < 4) {
      break;
    }

    S -= 4;
  }

  for (Index = 0; Index < mMethodCount; Index++) {
    if (CompareMem (mMethods[Index].Path + mMethods[Index].Len - 4, Abs + AbsLen - 4, 4) == 0) {
      return mMethods[Index].Args;
    }
  }

  return -1;
}

//
// ---- Primitive readers -----------------------------------------------------
//

STATIC
BOOLEAN
ReadPkgValue (
  IN  UINT8   *End,
  OUT UINT32  *Value
  )
{
  UINT8   Lead;
  UINTN   Count;
  UINTN   Index;
  UINT32  Len;

  if (mP >= End) {
    return FALSE;
  }

  Lead  = *mP++;
  Count = Lead >> 6;
  if (Count == 0) {
    *Value = Lead & 0x3F;
    return TRUE;
  }

  if (mP + Count > End) {
    return FALSE;
  }

  Len = Lead & 0x0F;
  for (Index = 0; Index < Count; Index++) {
    Len |= (UINT32)(*mP++) << (4 + 8 * Index);
  }

  *Value = Len;
  return TRUE;
}

STATIC
BOOLEAN
ReadPkgLength (
  IN  UINT8  *End,
  OUT UINT8  **PkgEnd
  )
{
  UINT8   *Start;
  UINT32  Len;

  Start = mP;
  if (!ReadPkgValue (End, &Len)) {
    return FALSE;
  }

  if ((Start + Len > End) || (Start + Len < mP)) {
    return FALSE;
  }

  *PkgEnd = Start + Len;
  return TRUE;
}

STATIC
BOOLEAN
IsLeadNameChar (
  IN UINT8  C
  )
{
  return (BOOLEAN)((C == '_') || ((C >= 'A') && (C <= 'Z')));
}

STATIC
BOOLEAN
IsNameStart (
  IN UINT8  C
  )
{
  return (BOOLEAN)(IsLeadNameChar (C) || (C == '\\') || (C == '^') || (C == 0x2E) || (C == 0x2F));
}

STATIC
VOID
PutSeg (
  IN OUT SB           *Out,
  IN     CONST UINT8  *Seg
  )
{
  CHAR16  S[5];
  UINTN   Len;
  UINTN   Index;

  for (Index = 0; Index < 4; Index++) {
    S[Index] = ((Seg[Index] >= 0x20) && (Seg[Index] < 0x7F)) ? (CHAR16)Seg[Index] : L'?';
  }

  Len = 4;
  while ((Len > 1) && (S[Len - 1] == L'_')) {
    Len--;
  }

  S[Len] = L'\0';
  SbPut (Out, S);
}

/**
  NameString. Abs receives the absolute path as concatenated 4-byte
  segments; Single is TRUE for a bare one-segment name (search rules apply).
**/
STATIC
BOOLEAN
ParseName (
  IN     UINT8    *End,
  IN OUT SB       *Out,
  OUT    CHAR8    *Abs,
  OUT    UINTN    *AbsLen,
  OUT    BOOLEAN  *Single
  )
{
  BOOLEAN  Root;
  UINTN    Parents;
  UINTN    Count;
  UINTN    Index;
  UINTN    Len;

  Root    = FALSE;
  Parents = 0;
  if ((mP < End) && (*mP == '\\')) {
    Root = TRUE;
    SbPut (Out, L"\\");
    mP++;
  } else {
    while ((mP < End) && (*mP == '^')) {
      Parents++;
      SbPut (Out, L"^");
      mP++;
    }
  }

  if (mP >= End) {
    return FALSE;
  }

  if (*mP == 0x2E) {
    Count = 2;
    mP++;
  } else if (*mP == 0x2F) {
    mP++;
    if (mP >= End) {
      return FALSE;
    }

    Count = *mP++;
  } else if (*mP == 0x00) {
    Count = 0;
    mP++;
  } else if (IsLeadNameChar (*mP)) {
    Count = 1;
  } else {
    return FALSE;
  }

  if (mP + Count * 4 > End) {
    return FALSE;
  }

  if (Root) {
    Len = 0;
  } else {
    Len = (Parents * 4 > mScopeLen) ? 0 : mScopeLen - Parents * 4;
    CopyMem (Abs, mScope, Len);
  }

  for (Index = 0; Index < Count; Index++) {
    if (Index > 0) {
      SbPut (Out, L".");
    }

    PutSeg (Out, mP);
    if (Len + 4 <= AML_MAX_PATH) {
      CopyMem (Abs + Len, mP, 4);
      Len += 4;
    }

    mP += 4;
  }

  *AbsLen = Len;
  *Single = (BOOLEAN)(!Root && (Parents == 0) && (Count == 1));
  return TRUE;
}

STATIC
BOOLEAN
ParseNewName (
  IN     UINT8  *End,
  IN OUT SB     *Out,
  OUT    CHAR8  *Abs,
  OUT    UINTN  *AbsLen
  )
{
  BOOLEAN  Single;

  return ParseName (End, Out, Abs, AbsLen, &Single);
}

/**
  An integer constant opcode, consumed. FALSE (nothing consumed) otherwise.
**/
STATIC
BOOLEAN
ReadConst (
  IN  UINT8   *End,
  OUT UINT64  *Value
  )
{
  UINTN  Size;

  if (mP >= End) {
    return FALSE;
  }

  switch (*mP) {
    case 0x00: *Value = 0; mP++; return TRUE;
    case 0x01: *Value = 1; mP++; return TRUE;
    case 0xFF: *Value = MAX_UINT64; mP++; return TRUE;
    case 0x0A: Size = 1; break;
    case 0x0B: Size = 2; break;
    case 0x0C: Size = 4; break;
    case 0x0E: Size = 8; break;
    default:
      return FALSE;
  }

  if (mP + 1 + Size > End) {
    return FALSE;
  }

  *Value = 0;
  CopyMem (Value, mP + 1, Size);
  mP += 1 + Size;
  return TRUE;
}

//
// ---- Resource templates ----------------------------------------------------
//

STATIC
BOOLEAN
RtValid (
  IN UINT8  *P,
  IN UINTN  Len
  )
{
  UINTN  Off;
  UINTN  DLen;

  Off = 0;
  while (Off < Len) {
    if (P[Off] & 0x80) {
      if (Off + 3 > Len) {
        return FALSE;
      }

      DLen = 3 + ReadUnaligned16 ((UINT16 *)(P + Off + 1));
    } else {
      DLen = 1 + (P[Off] & 7);
      if ((P[Off] & 0x78) == 0x78) {
        return (BOOLEAN)(Off + DLen == Len);
      }
    }

    Off += DLen;
  }

  return FALSE;
}

STATIC
VOID
RtList16 (
  IN OUT SB      *Out,
  IN     UINT16  Mask
  )
{
  UINTN    Bit;
  BOOLEAN  First;

  First = TRUE;
  SbPut (Out, L" {");
  for (Bit = 0; Bit < 16; Bit++) {
    if (Mask & (1 << Bit)) {
      SbFmt (Out, First ? L"%d" : L",%d", Bit);
      First = FALSE;
    }
  }

  SbPut (Out, L"}");
}

STATIC
VOID
RtResSource (
  IN OUT SB     *Out,
  IN     UINT8  *D,
  IN     UINTN  DLen,
  IN     UINTN  Off
  )
{
  UINTN  Index;

  SbPut (Out, L"\"");
  for (Index = Off; (Index < DLen) && (D[Index] != 0); Index++) {
    SbFmt (Out, L"%c", (CHAR16)D[Index]);
  }

  SbPut (Out, L"\"");
}

/**
  Address space descriptors (Word/DWord/QWord): Size is 2, 4 or 8.
**/
STATIC
VOID
RtAddress (
  IN OUT SB     *Out,
  IN     UINT8  *D,
  IN     UINTN  DLen,
  IN     UINTN  Size
  )
{
  CONST CHAR16  *Prefix;
  CONST CHAR16  *Kind;
  UINT8         Type;
  UINT8         Gen;
  UINT8         Spec;
  UINT64        V[5];
  UINTN         Index;
  CONST CHAR16  *Cache[] = { L"NonCacheable", L"Cacheable", L"WriteCombining", L"Prefetchable" };
  CONST CHAR16  *IoRng[] = { L"EntireRange", L"NonISAOnlyRanges", L"ISAOnlyRanges", L"EntireRange" };

  Prefix = (Size == 2) ? L"Word" : ((Size == 4) ? L"DWord" : L"QWord");
  if (DLen < 6 + 5 * Size) {
    SbFmt (Out, L"%sSpace ()  /* truncated */", Prefix);
    return;
  }

  Type = D[3];
  Gen  = D[4];
  Spec = D[5];
  for (Index = 0; Index < 5; Index++) {
    V[Index] = 0;
    CopyMem (&V[Index], D + 6 + Index * Size, Size);
  }

  Kind = (Type == 0) ? L"Memory" : ((Type == 1) ? L"IO" : ((Type == 2) ? L"BusNumber" : L"Space"));
  SbFmt (
    Out,
    L"%s%s (%s, ",
    Prefix,
    Kind,
    (Gen & 1) ? L"ResourceConsumer" : L"ResourceProducer"
    );
  if (Type == 0) {
    SbFmt (
      Out,
      L"%s, %s, %s, %s, %s,",
      (Gen & 2) ? L"SubDecode" : L"PosDecode",
      (Gen & 4) ? L"MinFixed" : L"MinNotFixed",
      (Gen & 8) ? L"MaxFixed" : L"MaxNotFixed",
      Cache[(Spec >> 1) & 3],
      (Spec & 1) ? L"ReadWrite" : L"ReadOnly"
      );
  } else if (Type == 1) {
    SbFmt (
      Out,
      L"%s, %s, %s, %s,",
      (Gen & 4) ? L"MinFixed" : L"MinNotFixed",
      (Gen & 8) ? L"MaxFixed" : L"MaxNotFixed",
      (Gen & 2) ? L"SubDecode" : L"PosDecode",
      IoRng[Spec & 3]
      );
  } else if (Type == 2) {
    SbFmt (
      Out,
      L"%s, %s, %s,",
      (Gen & 4) ? L"MinFixed" : L"MinNotFixed",
      (Gen & 8) ? L"MaxFixed" : L"MaxNotFixed",
      (Gen & 2) ? L"SubDecode" : L"PosDecode"
      );
  } else {
    SbFmt (Out, L"0x%02X, 0x%02X, 0x%02X,", Type, Gen, Spec);
  }

  for (Index = 0; Index < 5; Index++) {
    SbFmt (Out, L" 0x%0*lX%s", Size * 2, V[Index], (Index < 4) ? L"," : L"");
  }

  if (DLen > 6 + 5 * Size + 1) {
    SbFmt (Out, L", 0x%02X, ", D[6 + 5 * Size]);
    RtResSource (Out, D, DLen, 6 + 5 * Size + 1);
  }

  SbPut (Out, L")");
}

STATIC
VOID
RtDecode (
  IN OUT SB     *Out,
  IN     UINT8  *P,
  IN     UINTN  Len
  )
{
  UINTN   Off;
  UINTN   DLen;
  UINT8   *D;
  UINT8   Tag;
  UINT8   F;
  UINTN   Index;
  UINT16  W;

  SbPut (Out, L"ResourceTemplate ()\n{");
  for (Off = 0; Off < Len; Off += DLen) {
    D = P + Off;
    if (D[0] & 0x80) {
      DLen = 3 + ReadUnaligned16 ((UINT16 *)(D + 1));
      Tag  = D[0] & 0x7F;
    } else {
      DLen = 1 + (D[0] & 7);
      Tag  = (D[0] >> 3) & 0x0F;
      if (Tag == 0x0F) {
        break;
      }
    }

    SbPut (Out, L"\n    ");
    if (!(D[0] & 0x80)) {
      switch (Tag) {
        case 0x04:
          W = ReadUnaligned16 ((UINT16 *)(D + 1));
          if (DLen >= 4) {
            F = D[3];
            SbFmt (
              Out,
              L"IRQ (%s, %s, %s, )",
              (F & 1) ? L"Edge" : L"Level",
              (F & 8) ? L"ActiveLow" : L"ActiveHigh",
              (F & 0x10) ? L"Shared" : L"Exclusive"
              );
          } else {
            SbPut (Out, L"IRQNoFlags ()");
          }

          RtList16 (Out, W);
          break;
        case 0x05:
          {
            CONST CHAR16  *Speed[] = { L"Compatibility", L"TypeA", L"TypeB", L"TypeF" };
            CONST CHAR16  *Xfer[]  = { L"Transfer8", L"Transfer8_16", L"Transfer16", L"Transfer16" };

            F = D[2];
            SbFmt (Out, L"DMA (%s, %s, %s, )", Speed[(F >> 5) & 3], (F & 4) ? L"BusMaster" : L"NotBusMaster", Xfer[F & 3]);
            RtList16 (Out, D[1]);
          }
          break;
        case 0x06:
          if (DLen >= 2) {
            SbFmt (Out, L"StartDependentFn (0x%02X, 0x%02X)", D[1] & 3, (D[1] >> 2) & 3);
          } else {
            SbPut (Out, L"StartDependentFnNoPri ()");
          }

          break;
        case 0x07:
          SbPut (Out, L"EndDependentFn ()");
          break;
        case 0x08:
          SbFmt (
            Out,
            L"IO (%s, 0x%04X, 0x%04X, 0x%02X, 0x%02X, )",
            (D[1] & 1) ? L"Decode16" : L"Decode10",
            ReadUnaligned16 ((UINT16 *)(D + 2)),
            ReadUnaligned16 ((UINT16 *)(D + 4)),
            D[6],
            D[7]
            );
          break;
        case 0x09:
          SbFmt (Out, L"FixedIO (0x%04X, 0x%02X, )", ReadUnaligned16 ((UINT16 *)(D + 1)) & 0x3FF, D[3]);
          break;
        case 0x0A:
          SbFmt (
            Out,
            L"FixedDMA (0x%04X, 0x%04X, Width%dbit, )",
            ReadUnaligned16 ((UINT16 *)(D + 1)),
            ReadUnaligned16 ((UINT16 *)(D + 3)),
            8 << D[5]
            );
          break;
        case 0x0E:
          SbPut (Out, L"VendorShort () {");
          for (Index = 1; Index < DLen; Index++) {
            SbFmt (Out, L"%s0x%02X", (Index > 1) ? L", " : L"", D[Index]);
          }

          SbPut (Out, L"}");
          break;
        default:
          SbFmt (Out, L"/* small descriptor type 0x%X, %d bytes */", Tag, DLen);
          break;
      }

      continue;
    }

    switch (Tag) {
      case 0x01:
        SbFmt (
          Out,
          L"Memory24 (%s, 0x%04X, 0x%04X, 0x%04X, 0x%04X, )",
          (D[3] & 1) ? L"ReadWrite" : L"ReadOnly",
          ReadUnaligned16 ((UINT16 *)(D + 4)),
          ReadUnaligned16 ((UINT16 *)(D + 6)),
          ReadUnaligned16 ((UINT16 *)(D + 8)),
          ReadUnaligned16 ((UINT16 *)(D + 10))
          );
        break;
      case 0x02:
        {
          CONST CHAR16  *Space;

          Space = (D[3] < 0x0B) ? mRegionSpace[D[3]] : ((D[3] == 0x7F) ? L"FFixedHW" : L"0x??");
          SbFmt (
            Out,
            L"Register (%s, 0x%02X, 0x%02X, 0x%016lX, 0x%02X, )",
            Space,
            D[4],
            D[5],
            ReadUnaligned64 ((UINT64 *)(D + 7)),
            D[6]
            );
        }
        break;
      case 0x05:
        SbFmt (
          Out,
          L"Memory32 (%s, 0x%08X, 0x%08X, 0x%08X, 0x%08X, )",
          (D[3] & 1) ? L"ReadWrite" : L"ReadOnly",
          ReadUnaligned32 ((UINT32 *)(D + 4)),
          ReadUnaligned32 ((UINT32 *)(D + 8)),
          ReadUnaligned32 ((UINT32 *)(D + 12)),
          ReadUnaligned32 ((UINT32 *)(D + 16))
          );
        break;
      case 0x06:
        SbFmt (
          Out,
          L"Memory32Fixed (%s, 0x%08X, 0x%08X, )",
          (D[3] & 1) ? L"ReadWrite" : L"ReadOnly",
          ReadUnaligned32 ((UINT32 *)(D + 4)),
          ReadUnaligned32 ((UINT32 *)(D + 8))
          );
        break;
      case 0x07:
        RtAddress (Out, D, DLen, 4);
        break;
      case 0x08:
        RtAddress (Out, D, DLen, 2);
        break;
      case 0x0A:
        RtAddress (Out, D, DLen, 8);
        break;
      case 0x09:
        F = D[3];
        SbFmt (
          Out,
          L"Interrupt (%s, %s, %s, %s, ,, ) {",
          (F & 1) ? L"ResourceConsumer" : L"ResourceProducer",
          (F & 2) ? L"Edge" : L"Level",
          (F & 4) ? L"ActiveLow" : L"ActiveHigh",
          (F & 8) ? L"Shared" : L"Exclusive"
          );
        for (Index = 0; (Index < D[4]) && (5 + Index * 4 + 4 <= DLen); Index++) {
          SbFmt (Out, L"%s0x%08X", (Index > 0) ? L", " : L"", ReadUnaligned32 ((UINT32 *)(D + 5 + Index * 4)));
        }

        SbPut (Out, L"}");
        break;
      case 0x0C:
        if (DLen >= 23) {
          UINT16  Flags;
          UINT16  PinOff;
          UINT16  SrcOff;

          Flags  = ReadUnaligned16 ((UINT16 *)(D + 7));
          PinOff = ReadUnaligned16 ((UINT16 *)(D + 14));
          SrcOff = ReadUnaligned16 ((UINT16 *)(D + 17));
          if (D[4] == 0) {
            CONST CHAR16  *Pol[] = { L"ActiveHigh", L"ActiveLow", L"ActiveBoth", L"ActiveHigh" };

            SbFmt (
              Out,
              L"GpioInt (%s, %s, %s, PullCfg 0x%02X, 0x%04X, ",
              (Flags & 1) ? L"Edge" : L"Level",
              Pol[(Flags >> 1) & 3],
              (Flags & 8) ? L"Shared" : L"Exclusive",
              D[9],
              ReadUnaligned16 ((UINT16 *)(D + 12))
              );
          } else {
            SbFmt (Out, L"GpioIo (%s, PullCfg 0x%02X, 0x%04X, ", (Flags & 8) ? L"Shared" : L"Exclusive", D[9], ReadUnaligned16 ((UINT16 *)(D + 12)));
          }

          RtResSource (Out, D, DLen, SrcOff);
          SbPut (Out, L", ) {");
          for (Index = PinOff; (Index + 2 <= SrcOff) && (Index + 2 <= DLen); Index += 2) {
            SbFmt (Out, L"%s0x%04X", (Index > PinOff) ? L", " : L"", ReadUnaligned16 ((UINT16 *)(D + Index)));
          }

          SbPut (Out, L"}");
        } else {
          SbPut (Out, L"/* GPIO descriptor (truncated) */");
        }

        break;
      case 0x0E:
        if ((DLen >= 12) && (D[5] == 1) && (DLen >= 18)) {
          SbFmt (
            Out,
            L"I2cSerialBusV2 (0x%04X, %s, 0x%08X, AddressingMode%dBit, ",
            ReadUnaligned16 ((UINT16 *)(D + 16)),
            (D[6] & 1) ? L"DeviceInitiated" : L"ControllerInitiated",
            ReadUnaligned32 ((UINT32 *)(D + 12)),
            (D[7] & 1) ? 10 : 7
            );
          RtResSource (Out, D, DLen, 18);
          SbPut (Out, L", )");
        } else if ((DLen >= 21) && (D[5] == 2)) {
          SbFmt (
            Out,
            L"SpiSerialBusV2 (0x%04X, %s, %d, Mode%d, 0x%08X, ",
            ReadUnaligned16 ((UINT16 *)(D + 19)),
            (D[7] & 2) ? L"PolarityHigh" : L"PolarityLow",
            D[16],
            ((D[17] & 1) << 1) | (D[18] & 1),
            ReadUnaligned32 ((UINT32 *)(D + 12))
            );
          RtResSource (Out, D, DLen, 21);
          SbPut (Out, L", )");
        } else if ((DLen >= 22) && (D[5] == 3)) {
          SbFmt (Out, L"UartSerialBusV2 (0x%08X, DataBits%d, ", ReadUnaligned32 ((UINT32 *)(D + 12)), 5 + ((D[7] >> 4) & 7));
          RtResSource (Out, D, DLen, 22);
          SbPut (Out, L", )");
        } else {
          SbFmt (Out, L"/* SerialBus type %d, %d bytes */", (DLen > 5) ? D[5] : 0, DLen);
        }

        break;
      default:
        SbFmt (Out, L"/* large descriptor 0x%02X, %d bytes */", D[0], DLen);
        break;
    }
  }

  SbPut (Out, L"\n}");
}

//
// ---- Data objects ----------------------------------------------------------
//

STATIC
BOOLEAN
ParseBuffer (
  IN     UINT8  *End,
  IN OUT SB     *Out
  )
{
  UINT8   *PkgEnd;
  UINT64  Size;
  BOOLEAN IsConst;
  SB      SizeStr;
  UINTN   Count;
  UINTN   Index;
  UINT8   *Bytes;

  mP++;
  if (!ReadPkgLength (End, &PkgEnd)) {
    return FALSE;
  }

  ZeroMem (&SizeStr, sizeof (SizeStr));
  IsConst = ReadConst (PkgEnd, &Size);
  if (!IsConst && !ParseArg (PkgEnd, &SizeStr, TRUE)) {
    SbFree (&SizeStr);
    return FALSE;
  }

  Bytes = mP;
  Count = (UINTN)(PkgEnd - mP);
  mP    = PkgEnd;
  if (mSilent) {
    SbFree (&SizeStr);
    return TRUE;
  }

  if (IsConst && (Size == Count) && (Count >= 2) && RtValid (Bytes, Count)) {
    RtDecode (Out, Bytes, Count);
    SbFree (&SizeStr);
    return TRUE;
  }

  if (mUuidHint && IsConst && (Size == 16) && (Count == 16)) {
    SbFmt (
      Out,
      L"ToUUID (\"%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x\")",
      ReadUnaligned32 ((UINT32 *)Bytes),
      ReadUnaligned16 ((UINT16 *)(Bytes + 4)),
      ReadUnaligned16 ((UINT16 *)(Bytes + 6)),
      Bytes[8], Bytes[9], Bytes[10], Bytes[11], Bytes[12], Bytes[13], Bytes[14], Bytes[15]
      );
    return TRUE;
  }

  if (IsConst) {
    SbFmt (Out, L"Buffer (0x%02lX)", Size);
  } else {
    SbFmt (Out, L"Buffer (%s)", SbStr (&SizeStr));
  }

  SbFree (&SizeStr);
  if (Count == 0) {
    SbPut (Out, L" {}");
    return TRUE;
  }

  SbPut (Out, L"\n{");
  for (Index = 0; Index < Count; Index++) {
    if ((Index % 8) == 0) {
      SbFmt (Out, L"\n    /* %04X */  ", Index);
    }

    SbFmt (Out, L"0x%02X%s", Bytes[Index], (Index + 1 < Count) ? L", " : L"");
  }

  SbPut (Out, L"\n}");
  return TRUE;
}

STATIC
BOOLEAN
ParsePackage (
  IN     UINT8    *End,
  IN OUT SB       *Out,
  IN     BOOLEAN  Var
  )
{
  UINT8   *PkgEnd;
  SB      Elem;
  SB      Num;
  BOOLEAN First;

  mP++;
  if (!ReadPkgLength (End, &PkgEnd)) {
    return FALSE;
  }

  ZeroMem (&Num, sizeof (Num));
  if (Var) {
    if (!ParseArg (PkgEnd, &Num, TRUE)) {
      SbFree (&Num);
      return FALSE;
    }
  } else {
    if (mP >= PkgEnd) {
      return FALSE;
    }

    SbFmt (&Num, L"0x%02X", *mP++);
  }

  SbFmt (Out, L"Package (%s)", SbStr (&Num));
  SbFree (&Num);
  if (mP >= PkgEnd) {
    SbPut (Out, L" {}");
    mP = PkgEnd;
    return TRUE;
  }

  SbPut (Out, L"\n{");
  First = TRUE;
  while (mP < PkgEnd) {
    ZeroMem (&Elem, sizeof (Elem));
    if (!ParseArg (PkgEnd, &Elem, FALSE)) {
      SbFree (&Elem);
      SbPut (Out, L"\n    /* undecodable element */");
      break;
    }

    SbPut (Out, First ? L"\n    " : L",\n    ");
    SbPutIndented (Out, &Elem, 4);
    SbFree (&Elem);
    First = FALSE;
  }

  mP = PkgEnd;
  SbPut (Out, L"\n}");
  return TRUE;
}

STATIC
BOOLEAN
ParseString (
  IN     UINT8  *End,
  IN OUT SB     *Out
  )
{
  UINT8  *S;

  mP++;
  S = mP;
  while ((mP < End) && (*mP != 0)) {
    mP++;
  }

  if (mP >= End) {
    return FALSE;
  }

  SbPut (Out, L"\"");
  for ( ; S < mP; S++) {
    if ((*S == '"') || (*S == '\\')) {
      SbFmt (Out, L"\\%c", (CHAR16)*S);
    } else if ((*S >= 0x20) && (*S < 0x7F)) {
      SbFmt (Out, L"%c", (CHAR16)*S);
    } else {
      SbFmt (Out, L"\\x%02X", *S);
    }
  }

  SbPut (Out, L"\"");
  mP++;
  return TRUE;
}

//
// ---- Expressions -----------------------------------------------------------
//

/**
  Generic operator: Name (args) by its argument pattern.
**/
STATIC
BOOLEAN
ParseGeneric (
  IN     UINT8         *End,
  IN OUT SB            *Out,
  IN     CONST AML_OP  *Op
  )
{
  CONST CHAR8  *A;
  SB           Arg[8];
  BOOLEAN      Null[8];
  UINTN        N;
  UINTN        Last;
  UINTN        Index;
  BOOLEAN      Ok;
  CHAR8        Abs[AML_MAX_PATH];
  UINTN        AbsLen;
  UINT64       Value;
  BOOLEAN      Uuid;

  ZeroMem (Arg, sizeof (Arg));
  N  = 0;
  Ok = TRUE;
  Uuid = (BOOLEAN)((StrCmp (Op->Name, L"LEqual") == 0) || (StrCmp (Op->Name, L"LNotEqual") == 0));
  for (A = Op->Args; *A != '\0' && N < 8; A++, N++) {
    Null[N]   = FALSE;
    mUuidHint = Uuid;
    switch (*A) {
      case 'a':
      case 's':
        Ok = ParseArg (End, &Arg[N], TRUE);
        break;
      case 't':
        if ((mP < End) && (*mP == 0x00)) {
          mP++;
          Null[N] = TRUE;
        } else {
          Ok = ParseArg (End, &Arg[N], TRUE);
        }

        break;
      case 'n':
        Ok = ParseNewName (End, &Arg[N], Abs, &AbsLen);
        break;
      case 'b':
      case 'w':
      case 'd':
        Index = (*A == 'b') ? 1 : ((*A == 'w') ? 2 : 4);
        if (mP + Index > End) {
          Ok = FALSE;
          break;
        }

        Value = 0;
        CopyMem (&Value, mP, Index);
        mP += Index;
        SbFmt (&Arg[N], L"0x%0*lX", Index * 2, Value);
        break;
      default:
        Ok = FALSE;
        break;
    }

    mUuidHint = FALSE;
    if (!Ok) {
      N++;
      break;
    }
  }

  mUuidHint = FALSE;

  if (Ok) {
    SbPut (Out, Op->Name);
    if (N > 0) {
      Last = N;
      while ((Last > 0) && Null[Last - 1]) {
        Last--;
      }

      SbPut (Out, L" (");
      for (Index = 0; Index < Last; Index++) {
        if (Index > 0) {
          SbPut (Out, L", ");
        }

        SbPutIndented (Out, &Arg[Index], 0);
      }

      SbPut (Out, L")");
    }
  }

  for (Index = 0; Index < N; Index++) {
    SbFree (&Arg[Index]);
  }

  return Ok;
}

STATIC
BOOLEAN
ParseArgInner (
  IN     UINT8    *End,
  IN OUT SB       *Out,
  IN     BOOLEAN  AllowInvoke
  )
{
  UINT8    Op;
  UINT8    Ext;
  UINTN    Index;
  UINT64   Value;
  CHAR8    Abs[AML_MAX_PATH];
  UINTN    AbsLen;
  BOOLEAN  Single;
  INTN     Args;
  SB       A[6];
  UINT8    M1;
  UINT8    M2;
  BOOLEAN  Ok;

  if (mP >= End) {
    return FALSE;
  }

  Op = *mP;
  switch (Op) {
    case 0x00:
      mP++;
      SbPut (Out, L"Zero");
      return TRUE;
    case 0x01:
      mP++;
      SbPut (Out, L"One");
      return TRUE;
    case 0xFF:
      mP++;
      SbPut (Out, L"Ones");
      return TRUE;
    case 0x0A:
    case 0x0B:
    case 0x0C:
    case 0x0E:
      Index = (Op == 0x0A) ? 2 : ((Op == 0x0B) ? 4 : ((Op == 0x0C) ? 8 : 16));
      if (!ReadConst (End, &Value)) {
        return FALSE;
      }

      SbFmt (Out, L"0x%0*lX", Index, Value);
      return TRUE;
    case 0x0D:
      return ParseString (End, Out);
    case 0x11:
      return ParseBuffer (End, Out);
    case 0x12:
      return ParsePackage (End, Out, FALSE);
    case 0x13:
      return ParsePackage (End, Out, TRUE);
    default:
      break;
  }

  if ((Op >= 0x60) && (Op <= 0x67)) {
    mP++;
    SbFmt (Out, L"Local%d", Op - 0x60);
    return TRUE;
  }

  if ((Op >= 0x68) && (Op <= 0x6E)) {
    mP++;
    SbFmt (Out, L"Arg%d", Op - 0x68);
    return TRUE;
  }

  if (IsNameStart (Op)) {
    if (!ParseName (End, Out, Abs, &AbsLen, &Single)) {
      return FALSE;
    }

    if (!AllowInvoke) {
      return TRUE;
    }

    Args = LookupMethod (Abs, AbsLen, Single);
    if (Args < 0) {
      return TRUE;
    }

    SbPut (Out, L" (");
    for (Index = 0; Index < (UINTN)Args; Index++) {
      ZeroMem (&A[0], sizeof (SB));
      if (!ParseArg (End, &A[0], TRUE)) {
        SbFree (&A[0]);
        return FALSE;
      }

      if (Index > 0) {
        SbPut (Out, L", ");
      }

      SbPutIndented (Out, &A[0], 0);
      SbFree (&A[0]);
    }

    SbPut (Out, L")");
    return TRUE;
  }

  if (Op == 0x5B) {
    if (mP + 1 >= End) {
      return FALSE;
    }

    Ext = mP[1];
    for (Index = 0; Index < ARRAY_SIZE (mExtOps); Index++) {
      if (mExtOps[Index].Op == Ext) {
        mP += 2;
        return ParseGeneric (End, Out, &mExtOps[Index]);
      }
    }

    return FALSE;
  }

  if ((Op == 0x92) && (mP + 1 < End) && (mP[1] >= 0x93) && (mP[1] <= 0x95)) {
    CONST AML_OP  Combo[] = {
      { 0, L"LNotEqual",     "aa" },
      { 0, L"LLessEqual",    "aa" },
      { 0, L"LGreaterEqual", "aa" }
    };

    Ext = mP[1];
    mP += 2;
    return ParseGeneric (End, Out, &Combo[Ext - 0x93]);
  }

  if (Op == 0x89) {
    mP++;
    ZeroMem (A, sizeof (A));
    Ok = ParseArg (End, &A[0], TRUE);
    if (Ok && (mP < End)) {
      M1 = *mP++;
      Ok = ParseArg (End, &A[1], TRUE);
      if (Ok && (mP < End)) {
        M2 = *mP++;
        Ok = ParseArg (End, &A[2], TRUE) && ParseArg (End, &A[3], TRUE);
        if (Ok) {
          SbFmt (Out, L"Match (%s, %s, ", SbStr (&A[0]), (M1 < 6) ? mMatchOp[M1] : L"M??");
          SbFmt (Out, L"%s, %s, ", SbStr (&A[1]), (M2 < 6) ? mMatchOp[M2] : L"M??");
          SbFmt (Out, L"%s, %s)", SbStr (&A[2]), SbStr (&A[3]));
        }
      } else {
        Ok = FALSE;
      }
    } else {
      Ok = FALSE;
    }

    for (Index = 0; Index < 4; Index++) {
      SbFree (&A[Index]);
    }

    return Ok;
  }

  for (Index = 0; Index < ARRAY_SIZE (mOps); Index++) {
    if (mOps[Index].Op == Op) {
      mP++;
      return ParseGeneric (End, Out, &mOps[Index]);
    }
  }

  return FALSE;
}

STATIC
BOOLEAN
ParseArg (
  IN     UINT8    *End,
  IN OUT SB       *Out,
  IN     BOOLEAN  AllowInvoke
  )
{
  BOOLEAN  Ok;

  if (mDepth >= AML_MAX_DEPTH) {
    return FALSE;
  }

  mDepth++;
  if ((mP < End) && (*mP != 0x11)) {
    mUuidHint = FALSE;
  }

  Ok = ParseArgInner (End, Out, AllowInvoke);
  mDepth--;
  return Ok;
}

//
// ---- Statements ------------------------------------------------------------
//

STATIC
BOOLEAN
ParseFieldList (
  IN UINT8  *End,
  IN UINTN  Indent
  )
{
  UINT32  Bits;
  UINT32  Width;
  UINT8   Type;
  SB      S;
  CHAR8   Abs[AML_MAX_PATH];
  UINTN   AbsLen;
  BOOLEAN Single;

  Bits = 0;
  while (mP < End) {
    switch (*mP) {
      case 0x00:
        mP++;
        if (!ReadPkgValue (End, &Width)) {
          return FALSE;
        }

        Bits += Width;
        if ((Bits % 8) == 0) {
          EmitFmt (Indent, ATTR_NORMAL, L"Offset (0x%02X)%s", Bits / 8, (mP < End) ? L"," : L"");
        } else {
          EmitFmt (Indent, ATTR_NORMAL, L"    , %d%s", Width, (mP < End) ? L"," : L"");
        }

        break;
      case 0x01:
        if (mP + 3 > End) {
          return FALSE;
        }

        Type = mP[1];
        EmitFmt (Indent, ATTR_NORMAL, L"AccessAs (%s, 0x%02X),", mAccess[Type & 7], mP[2]);
        mP += 3;
        break;
      case 0x03:
        if (mP + 4 > End) {
          return FALSE;
        }

        EmitFmt (Indent, ATTR_NORMAL, L"AccessAs (%s, 0x%02X, 0x%02X),", mAccess[mP[1] & 7], mP[2], mP[3]);
        mP += 4;
        break;
      case 0x02:
        mP++;
        ZeroMem (&S, sizeof (S));
        if ((mP < End) && (*mP == 0x11)) {
          if (!ParseBuffer (End, &S)) {
            SbFree (&S);
            return FALSE;
          }
        } else if (!ParseName (End, &S, Abs, &AbsLen, &Single)) {
          SbFree (&S);
          return FALSE;
        }

        SbPut (&S, L"),");
        if (!mSilent) {
          SB  Line;

          ZeroMem (&Line, sizeof (Line));
          SbPut (&Line, L"Connect (");
          SbPutIndented (&Line, &S, 0);
          Emit (Indent, ATTR_NORMAL, SbStr (&Line));
          SbFree (&Line);
        }

        SbFree (&S);
        break;
      default:
        if ((mP + 4 > End) || !IsLeadNameChar (*mP)) {
          return FALSE;
        }

        ZeroMem (&S, sizeof (S));
        PutSeg (&S, mP);
        mP += 4;
        if (!ReadPkgValue (End, &Width)) {
          SbFree (&S);
          return FALSE;
        }

        Bits += Width;
        SbPut (&S, L",");
        EmitFmt (Indent, ATTR_NORMAL, L"%-7s %d%s", SbStr (&S), Width, (mP < End) ? L"," : L"");
        SbFree (&S);
        break;
    }
  }

  return TRUE;
}

STATIC
CONST CHAR16 *
FieldFlags (
  IN  UINT8   Flags,
  OUT CHAR16  *Buf,
  IN  UINTN   Size
  )
{
  UnicodeSPrint (
    Buf,
    Size,
    L"%s, %s, %s",
    mAccess[Flags & 0x0F & 7],
    (Flags & 0x10) ? L"Lock" : L"NoLock",
    mUpdate[(Flags >> 5) & 3]
    );
  return Buf;
}

/**
  Enter a named scope: Abs becomes the current scope for the body.
**/
STATIC
BOOLEAN
Block (
  IN UINT8        *PkgEnd,
  IN UINTN        Indent,
  IN CONST CHAR8  *Abs,
  IN UINTN        AbsLen
  )
{
  CHAR8    Saved[AML_MAX_PATH];
  UINTN    SavedLen;
  BOOLEAN  Ok;

  SavedLen = mScopeLen;
  CopyMem (Saved, mScope, mScopeLen);
  if (Abs != NULL) {
    CopyMem (mScope, Abs, AbsLen);
    mScopeLen = AbsLen;
  }

  Emit (Indent, ATTR_NORMAL, L"{");
  Ok = ParseTermList (PkgEnd, Indent + 1);
  Emit (Indent, ATTR_NORMAL, L"}");
  CopyMem (mScope, Saved, SavedLen);
  mScopeLen = SavedLen;
  mP        = PkgEnd;
  return Ok;
}

STATIC
BOOLEAN
EisaId (
  IN  UINT32  V,
  OUT CHAR16  *Out
  )
{
  UINT16  Hi;
  UINTN   Index;
  CHAR16  C;

  Hi = (UINT16)(((V & 0xFF) << 8) | ((V >> 8) & 0xFF));
  for (Index = 0; Index < 3; Index++) {
    C = (CHAR16)(((Hi >> (10 - 5 * Index)) & 0x1F) + 0x40);
    if ((C < L'A') || (C > L'Z')) {
      return FALSE;
    }

    Out[Index] = C;
  }

  UnicodeSPrint (Out + 3, 5 * sizeof (CHAR16), L"%02X%02X", (V >> 16) & 0xFF, (V >> 24) & 0xFF);
  return TRUE;
}

STATIC
BOOLEAN
ParseStatement (
  IN UINT8  *End,
  IN UINTN  Indent
  )
{
  UINT8    Op;
  UINT8    Ext;
  UINT8    *PkgEnd;
  SB       S;
  SB       V;
  SB       Line;
  CHAR8    Abs[AML_MAX_PATH];
  UINTN    AbsLen;
  CHAR8    Abs2[AML_MAX_PATH];
  UINTN    AbsLen2;
  BOOLEAN  Single;
  BOOLEAN  Ok;
  UINT8    Flags;
  UINT64   Value;
  CHAR16   Buf[64];
  CHAR16   Id[9];
  UINT8    *Save;

  ZeroMem (&S, sizeof (S));
  ZeroMem (&V, sizeof (V));
  ZeroMem (&Line, sizeof (Line));
  Op  = *mP;
  Ext = (mP + 1 < End) ? mP[1] : 0;
  Ok  = TRUE;

  if ((Op == 0x10) || ((Op == 0x5B) && ((Ext == 0x82) || (Ext == 0x85) || (Ext == 0x83) || (Ext == 0x84)))) {
    //
    // Scope / Device / ThermalZone / Processor / PowerResource
    //
    mP += (Op == 0x10) ? 1 : 2;
    if (!ReadPkgLength (End, &PkgEnd) || !ParseNewName (PkgEnd, &S, Abs, &AbsLen)) {
      SbFree (&S);
      return FALSE;
    }

    if (Op == 0x10) {
      SbFmt (&Line, L"Scope (%s)", SbStr (&S));
    } else if (Ext == 0x82) {
      SbFmt (&Line, L"Device (%s)", SbStr (&S));
    } else if (Ext == 0x85) {
      SbFmt (&Line, L"ThermalZone (%s)", SbStr (&S));
    } else if (Ext == 0x83) {
      if (mP + 6 > PkgEnd) {
        Ok = FALSE;
      } else {
        SbFmt (&Line, L"Processor (%s, 0x%02X, 0x%08X, 0x%02X)", SbStr (&S), mP[0], ReadUnaligned32 ((UINT32 *)(mP + 1)), mP[5]);
        mP += 6;
      }
    } else {
      if (mP + 3 > PkgEnd) {
        Ok = FALSE;
      } else {
        SbFmt (&Line, L"PowerResource (%s, 0x%02X, 0x%04X)", SbStr (&S), mP[0], ReadUnaligned16 ((UINT16 *)(mP + 1)));
        mP += 3;
      }
    }

    if (Ok) {
      Emit (Indent, ATTR_HEADER, SbStr (&Line));
      Ok = Block (PkgEnd, Indent, Abs, AbsLen);
    }

    SbFree (&S);
    SbFree (&Line);
    mP = PkgEnd;
    return TRUE;
  }

  switch (Op) {
    case 0x14:
      mP++;
      if (!ReadPkgLength (End, &PkgEnd) || !ParseNewName (PkgEnd, &S, Abs, &AbsLen) || (mP >= PkgEnd)) {
        SbFree (&S);
        return FALSE;
      }

      Flags = *mP++;
      if (mPrepass) {
        AddMethod (Abs, AbsLen, Flags & 7);
        SbFree (&S);
        mP = PkgEnd;
        return TRUE;
      }

      if ((Flags >> 4) != 0) {
        EmitFmt (Indent, ATTR_HEADER, L"Method (%s, %d, %s, %d)", SbStr (&S), Flags & 7, (Flags & 8) ? L"Serialized" : L"NotSerialized", Flags >> 4);
      } else {
        EmitFmt (Indent, ATTR_HEADER, L"Method (%s, %d, %s)", SbStr (&S), Flags & 7, (Flags & 8) ? L"Serialized" : L"NotSerialized");
      }

      SbFree (&S);
      Block (PkgEnd, Indent, Abs, AbsLen);
      return TRUE;

    case 0xA0:
    case 0xA2:
      if ((Op == 0xA0) && (mP + 2 < End)) {
        UINT8  *Save3;
        UINT8  *IfEnd2;
        UINT8  *Q;

        Save3 = mP;
        mP++;
        if (ReadPkgLength (End, &IfEnd2) && (mP < IfEnd2) && (*mP == 0x00) && (mP + 1 < IfEnd2) && (mP[1] == 0x15)) {
          mP++;
          Q = mP;
          while ((mP < IfEnd2) && (*mP == 0x15) && ParseStatement (IfEnd2, Indent)) {
          }

          if (mP == IfEnd2) {
            return TRUE;
          }

          (VOID)Q;
        }

        mP = Save3;
      }

      mP++;
      if (!ReadPkgLength (End, &PkgEnd) || !ParseArg (PkgEnd, &S, TRUE)) {
        SbFree (&S);
        return FALSE;
      }

      SbPut (&Line, (Op == 0xA2) ? L"While (" : (mElseIf ? L"ElseIf (" : L"If ("));
      mElseIf = FALSE;
      SbPutIndented (&Line, &S, 0);
      SbPut (&Line, L")");
      Emit (Indent, ATTR_NORMAL, SbStr (&Line));
      SbFree (&S);
      SbFree (&Line);
      Block (PkgEnd, Indent, NULL, 0);
      return TRUE;

    case 0xA1:
      mP++;
      if (!ReadPkgLength (End, &PkgEnd)) {
        return FALSE;
      }

      //
      // Else { If (...) {...} [Else {...}] } reads as ElseIf.
      //
      if ((mP < PkgEnd) && (*mP == 0xA0)) {
        UINT8  *Save2;
        UINT8  *IfEnd;
        UINT8  *ElseEnd;

        Save2 = mP;
        mP++;
        if (ReadPkgLength (PkgEnd, &IfEnd)) {
          ElseEnd = IfEnd;
          if ((IfEnd < PkgEnd) && (*IfEnd == 0xA1)) {
            mP = IfEnd + 1;
            if (!ReadPkgLength (PkgEnd, &ElseEnd)) {
              ElseEnd = NULL;
            }
          }

          if (ElseEnd == PkgEnd) {
            mP      = Save2;
            mElseIf = TRUE;
            ParseTermList (PkgEnd, Indent);
            mElseIf = FALSE;
            mP      = PkgEnd;
            return TRUE;
          }
        }

        mP = Save2;
      }

      Emit (Indent, ATTR_NORMAL, L"Else");
      Block (PkgEnd, Indent, NULL, 0);
      return TRUE;

    case 0x08:
      mP++;
      if (!ParseNewName (End, &S, Abs, &AbsLen)) {
        SbFree (&S);
        return FALSE;
      }

      if ((AbsLen >= 4) && (mP < End) && (*mP == 0x0C) &&
          ((CompareMem (Abs + AbsLen - 4, "_HID", 4) == 0) || (CompareMem (Abs + AbsLen - 4, "_CID", 4) == 0)))
      {
        Save = mP;
        if (ReadConst (End, &Value) && EisaId ((UINT32)Value, Id)) {
          EmitFmt (Indent, ATTR_NORMAL, L"Name (%s, EisaId (\"%s\"))  // 0x%08X", SbStr (&S), Id, (UINT32)Value);
          SbFree (&S);
          return TRUE;
        }

        mP = Save;
      }

      if (!ParseArg (End, &V, FALSE)) {
        SbFree (&S);
        SbFree (&V);
        return FALSE;
      }

      SbFmt (&Line, L"Name (%s, ", SbStr (&S));
      SbPutIndented (&Line, &V, 0);
      SbPut (&Line, L")");
      Emit (Indent, ATTR_NORMAL, SbStr (&Line));
      SbFree (&S);
      SbFree (&V);
      SbFree (&Line);
      return TRUE;

    case 0x15:
      mP++;
      if (!ParseNewName (End, &S, Abs, &AbsLen) || (mP + 2 > End)) {
        SbFree (&S);
        return FALSE;
      }

      if (mP[0] == 8) {
        AddMethod (Abs, AbsLen, mP[1] & 7);
      }

      if (mP[0] == 8) {
        EmitFmt (Indent, ATTR_DIM, L"External (%s, MethodObj)    // %d Arguments", SbStr (&S), mP[1] & 7);
      } else {
        EmitFmt (Indent, ATTR_DIM, L"External (%s, %s)", SbStr (&S), (mP[0] < 16) ? mObjType[mP[0]] : L"UnknownObj");
      }

      mP += 2;
      SbFree (&S);
      return TRUE;

    default:
      break;
  }

  if (Op == 0x5B) {
    switch (Ext) {
      case 0x80:
        mP += 2;
        if (!ParseNewName (End, &S, Abs, &AbsLen) || (mP >= End)) {
          SbFree (&S);
          return FALSE;
        }

        Flags = *mP++;
        if (Flags < ARRAY_SIZE (mRegionSpace)) {
          SbFmt (&Line, L"OperationRegion (%s, %s, ", SbStr (&S), mRegionSpace[Flags]);
        } else {
          SbFmt (&Line, L"OperationRegion (%s, 0x%02X, ", SbStr (&S), Flags);
        }

        Ok = ParseArg (End, &V, TRUE);
        if (Ok) {
          SbPutIndented (&Line, &V, 0);
          SbFree (&V);
          SbPut (&Line, L", ");
          Ok = ParseArg (End, &V, TRUE);
          if (Ok) {
            SbPutIndented (&Line, &V, 0);
            SbPut (&Line, L")");
            Emit (Indent, ATTR_NORMAL, SbStr (&Line));
          }
        }

        SbFree (&S);
        SbFree (&V);
        SbFree (&Line);
        return Ok;

      case 0x81:
      case 0x86:
      case 0x87:
        mP += 2;
        if (!ReadPkgLength (End, &PkgEnd) || !ParseName (PkgEnd, &S, Abs, &AbsLen, &Single)) {
          SbFree (&S);
          return FALSE;
        }

        if (Ext == 0x81) {
          SbFmt (&Line, L"Field (%s, ", SbStr (&S));
        } else {
          if (!ParseName (PkgEnd, &V, Abs2, &AbsLen2, &Single)) {
            Ok = FALSE;
          } else {
            SbFmt (&Line, (Ext == 0x86) ? L"IndexField (%s, %s, " : L"BankField (%s, %s, ", SbStr (&S), SbStr (&V));
            SbFree (&V);
            if (Ext == 0x87) {
              Ok = ParseArg (PkgEnd, &V, TRUE);
              if (Ok) {
                SbPutIndented (&Line, &V, 0);
                SbPut (&Line, L", ");
              }
            }
          }
        }

        if (Ok && (mP < PkgEnd)) {
          Flags = *mP++;
          SbFmt (&Line, L"%s)", FieldFlags (Flags, Buf, sizeof (Buf)));
          Emit (Indent, ATTR_NORMAL, SbStr (&Line));
          Emit (Indent, ATTR_NORMAL, L"{");
          if (!ParseFieldList (PkgEnd, Indent + 1)) {
            Emit (Indent + 1, ATTR_WARN, L"/* undecodable field list */");
          }

          Emit (Indent, ATTR_NORMAL, L"}");
        } else {
          Ok = FALSE;
        }

        SbFree (&S);
        SbFree (&V);
        SbFree (&Line);
        mP = PkgEnd;
        return Ok;

      default:
        break;
    }
  }

  //
  // Anything else is an expression used as a statement.
  //
  if (!ParseArg (End, &S, TRUE)) {
    SbFree (&S);
    return FALSE;
  }

  Emit (Indent, ATTR_NORMAL, SbStr (&S));
  SbFree (&S);
  return TRUE;
}

STATIC
BOOLEAN
ParseTermList (
  IN UINT8  *End,
  IN UINTN  Indent
  )
{
  UINT8  *Start;

  if (mDepth >= AML_MAX_DEPTH) {
    mP = End;
    return FALSE;
  }

  mDepth++;
  while (mP < End) {
    Start = mP;
    if (!ParseStatement (End, Indent) || (mP <= Start)) {
      EmitFmt (
        Indent,
        ATTR_WARN,
        L"/* cannot decode AML at offset 0x%X (opcode 0x%02X); %d byte(s) skipped */",
        (UINTN)(Start - mBase),
        *Start,
        (UINTN)(End - Start)
        );
      mP = End;
      mDepth--;
      return FALSE;
    }
  }

  mDepth--;
  return TRUE;
}

//
// ---- Public ----------------------------------------------------------------
//

/**
  Collect Method and External declarations from every definition block.
**/
VOID
AmlPrepare (
  IN UINT8   **Tables,
  IN UINT32  *Lengths,
  IN UINTN   Count
  )
{
  UINTN  Index;

  mMethodCount = 0;
  mSilent      = TRUE;
  mPrepass     = TRUE;
  mDoc         = NULL;
  for (Index = 0; Index < Count; Index++) {
    if (Lengths[Index] <= 36) {
      continue;
    }

    mBase     = Tables[Index];
    mP        = mBase + 36;
    mScopeLen = 0;
    mDepth    = 0;
    ParseTermList (mBase + Lengths[Index], 0);
  }

  mSilent  = FALSE;
  mPrepass = FALSE;
}

/**
  Disassemble one DSDT/SSDT into Doc.
**/
VOID
AmlDisassemble (
  IN OUT VOID      *DocPtr,
  IN     UINT8     *Table,
  IN     UINT32    Length
  )
{
  TEXT_DOC  *Doc;

  CHAR16  Sig[5];
  CHAR16  Oem[7];
  CHAR16  OemTab[9];
  UINTN   Index;

  Doc = (TEXT_DOC *)DocPtr;
  if (Length < 36) {
    DocAdd (Doc, ATTR_WARN, L"Table too short.");
    return;
  }

  for (Index = 0; Index < 4; Index++) {
    Sig[Index] = (CHAR16)Table[Index];
  }

  Sig[4] = L'\0';
  for (Index = 0; Index < 6; Index++) {
    Oem[Index] = (Table[10 + Index] >= 0x20) ? (CHAR16)Table[10 + Index] : L' ';
  }

  Oem[6] = L'\0';
  for (Index = 0; Index < 8; Index++) {
    OemTab[Index] = (Table[16 + Index] >= 0x20) ? (CHAR16)Table[16 + Index] : L' ';
  }

  OemTab[8] = L'\0';

  mDoc      = Doc;
  mSilent   = FALSE;
  mPrepass  = FALSE;
  mBase     = Table;
  mP        = Table + 36;
  mScopeLen = 0;
  mDepth    = 0;
  DocAdd (Doc, ATTR_DIM, L"/*");
  DocAdd (Doc, ATTR_DIM, L" * uefirrw AML disassembly (ASL 1.0 syntax), %d bytes of AML", Length - 36);
  DocAdd (Doc, ATTR_DIM, L" * Method argument counts come from every DSDT/SSDT on this system.");
  DocAdd (Doc, ATTR_DIM, L" */");
  DocAdd (Doc, ATTR_HEADER, L"DefinitionBlock (\"\", \"%s\", %d, \"%s\", \"%s\", 0x%08X)", Sig, Table[8], Oem, OemTab, ReadUnaligned32 ((UINT32 *)(Table + 24)));
  DocAdd (Doc, ATTR_NORMAL, L"{");
  ParseTermList (Table + Length, 1);
  DocAdd (Doc, ATTR_NORMAL, L"}");
  mDoc = NULL;
}
