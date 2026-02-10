#include <Uefi.h>
#include <Base.h>

#include <Library/UefiLib.h>
#include <Library/UefiApplicationEntryPoint.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>
#include <Library/PrintLib.h>

#define LINE_MAX_CHARS  128

// Default Vendor GUID (as your menu shows)
STATIC EFI_GUID mDefaultVendorGuid = { 0x37893825, 0x3B85, 0x02D0, { 0x37, 0x89, 0x33, 0xF9, 0x00, 0x00, 0x00, 0x00 } };

STATIC VOID
WaitAnyKey(VOID)
{
  EFI_INPUT_KEY Key;
  Print(L"\nPress any key to continue...");
  while (gST->ConIn->ReadKeyStroke(gST->ConIn, &Key) == EFI_NOT_READY) {
    gBS->Stall(1000);
  }
  Print(L"\n");
}

STATIC VOID
SetTextAttr(UINTN Attr)
{
  if (gST != NULL && gST->ConOut != NULL) {
    gST->ConOut->SetAttribute(gST->ConOut, Attr);
  }
}

STATIC VOID
ClearScreen(VOID)
{
  if (gST && gST->ConOut) {
    gST->ConOut->ClearScreen(gST->ConOut);
  }
}

STATIC EFI_STATUS
GetConsoleSize(OUT UINTN *OutCols, OUT UINTN *OutRows)
{
  UINTN Cols = 0, Rows = 0;
  EFI_STATUS Status;

  if (gST == NULL || gST->ConOut == NULL || gST->ConOut->QueryMode == NULL) {
    return EFI_UNSUPPORTED;
  }

  Status = gST->ConOut->QueryMode(gST->ConOut, gST->ConOut->Mode->Mode, &Cols, &Rows);
  if (EFI_ERROR(Status)) {
    return Status;
  }

  if (OutCols) *OutCols = Cols;
  if (OutRows) *OutRows = Rows;
  return EFI_SUCCESS;
}

STATIC VOID
PrintGuidLine(IN EFI_GUID *Guid)
{
  // Print GUID in canonical form
  Print(L"%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        Guid->Data1, Guid->Data2, Guid->Data3,
        Guid->Data4[0], Guid->Data4[1],
        Guid->Data4[2], Guid->Data4[3], Guid->Data4[4], Guid->Data4[5], Guid->Data4[6], Guid->Data4[7]);
}

STATIC BOOLEAN
IsHexChar(CHAR16 C)
{
  return ((C >= L'0' && C <= L'9') ||
          (C >= L'a' && C <= L'f') ||
          (C >= L'A' && C <= L'F'));
}

STATIC CHAR16
ToUpperHex(CHAR16 C)
{
  if (C >= L'a' && C <= L'f') return (CHAR16)(C - (L'a' - L'A'));
  return C;
}

STATIC INTN
HexVal(CHAR16 C)
{
  if (C >= L'0' && C <= L'9') return (INTN)(C - L'0');
  if (C >= L'a' && C <= L'f') return (INTN)(C - L'a' + 10);
  if (C >= L'A' && C <= L'F') return (INTN)(C - L'A' + 10);
  return -1;
}

STATIC EFI_STATUS
ReadLine(IN CHAR16 *Buffer, IN UINTN BufferChars)
{
  UINTN Index = 0;
  EFI_INPUT_KEY Key;

  if (Buffer == NULL || BufferChars == 0) {
    return EFI_INVALID_PARAMETER;
  }

  Buffer[0] = L'\0';

  while (TRUE) {
    while (gST->ConIn->ReadKeyStroke(gST->ConIn, &Key) == EFI_NOT_READY) {
      gBS->Stall(1000);
    }

    if (Key.UnicodeChar == CHAR_CARRIAGE_RETURN) {
      Print(L"\n");
      Buffer[Index] = L'\0';
      return EFI_SUCCESS;
    }

    if (Key.UnicodeChar == CHAR_BACKSPACE) {
      if (Index > 0) {
        Index--;
        Buffer[Index] = L'\0';
        // erase one char on screen
        Print(L"\b \b");
      }
      continue;
    }

    // ignore non-printable
    if (Key.UnicodeChar < 0x20 || Key.UnicodeChar > 0x7E) {
      continue;
    }

    if (Index + 1 < BufferChars) {
      Buffer[Index++] = Key.UnicodeChar;
      Buffer[Index] = L'\0';
      Print(L"%c", Key.UnicodeChar);
    }
  }
}

// =============================
// GUID masked input
// Desired UI: ________-____-____-____-____________
// Type replaces '_' from left to right (skip '-')
// Enter with no input => returns empty string (OutStr[0] = L'\0')
// If partially filled => returns EFI_NOT_READY
// =============================
STATIC EFI_STATUS
ReadGuidMaskedLine(OUT CHAR16 *OutStr, IN UINTN OutChars)
{
  // template length must be 36
  STATIC CONST CHAR16 *Mask = L"________-____-____-____-____________";
  UINTN EditablePos[32];
  UINTN EditCount = 0;
  UINTN i;

  EFI_INPUT_KEY Key;
  UINTN StartCol, StartRow;
  UINTN CurEdit = 0;
  BOOLEAN AnyTyped = FALSE;

  if (OutStr == NULL || OutChars < 37) {
    return EFI_INVALID_PARAMETER;
  }

  // build editable positions
  for (i = 0; i < 36; i++) {
    if (Mask[i] == L'_') {
      EditablePos[EditCount++] = i;
    }
  }

  // init output with mask
  StrCpyS(OutStr, OutChars, Mask);

  // record cursor position BEFORE printing mask
  StartCol = gST->ConOut->Mode->CursorColumn;
  StartRow = gST->ConOut->Mode->CursorRow;

  // print mask
  Print(L"%s", OutStr);

  // move cursor to first editable
  gST->ConOut->SetCursorPosition(gST->ConOut, StartCol + EditablePos[0], StartRow);

  while (TRUE) {
    while (gST->ConIn->ReadKeyStroke(gST->ConIn, &Key) == EFI_NOT_READY) {
      gBS->Stall(1000);
    }

    if (Key.UnicodeChar == CHAR_CARRIAGE_RETURN) {
      Print(L"\n");

      if (!AnyTyped) {
        // user didn't type anything => treat as empty
        OutStr[0] = L'\0';
        return EFI_SUCCESS;
      }

      // if any '_' remains => incomplete
      for (i = 0; i < 36; i++) {
        if (OutStr[i] == L'_') {
          return EFI_NOT_READY;
        }
      }
      return EFI_SUCCESS;
    }

    if (Key.UnicodeChar == CHAR_BACKSPACE) {
      if (CurEdit > 0) {
        CurEdit--;
        AnyTyped = TRUE; // still typed, but we keep state

        // restore underscore at previous editable slot
        OutStr[EditablePos[CurEdit]] = L'_';

        // draw it
        gST->ConOut->SetCursorPosition(gST->ConOut, StartCol + EditablePos[CurEdit], StartRow);
        Print(L"_");

        // move cursor back to that slot
        gST->ConOut->SetCursorPosition(gST->ConOut, StartCol + EditablePos[CurEdit], StartRow);
      }
      continue;
    }

    // allow user to type '-' but ignore (we already have it)
    if (Key.UnicodeChar == L'-') {
      continue;
    }

    // only accept hex
    if (!IsHexChar(Key.UnicodeChar)) {
      continue;
    }

    if (CurEdit >= EditCount) {
      // already full
      continue;
    }

    AnyTyped = TRUE;

    // place uppercase hex into the next underscore slot
    OutStr[EditablePos[CurEdit]] = ToUpperHex(Key.UnicodeChar);

    // draw at that exact position
    gST->ConOut->SetCursorPosition(gST->ConOut, StartCol + EditablePos[CurEdit], StartRow);
    Print(L"%c", OutStr[EditablePos[CurEdit]]);

    CurEdit++;

    if (CurEdit < EditCount) {
      gST->ConOut->SetCursorPosition(gST->ConOut, StartCol + EditablePos[CurEdit], StartRow);
    } else {
      // end => keep cursor at end of mask
      gST->ConOut->SetCursorPosition(gST->ConOut, StartCol + 36, StartRow);
    }
  }
}

STATIC EFI_STATUS
ParseGuidString(IN CHAR16 *Str, OUT EFI_GUID *OutGuid)
{
  // Expect: 8-4-4-4-12 hex (36 chars)
  // Example: 47C7B227-C42A-11D2-8E57-00A0C969723B
  UINTN Len;
  UINTN i;
  INTN v;

  UINT32 d1 = 0;
  UINT16 d2 = 0, d3 = 0;
  UINT8  d4[8];

  if (Str == NULL || OutGuid == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Len = StrLen(Str);
  if (Len != 36) {
    return EFI_INVALID_PARAMETER;
  }

  // check hyphens
  if (Str[8] != L'-' || Str[13] != L'-' || Str[18] != L'-' || Str[23] != L'-') {
    return EFI_INVALID_PARAMETER;
  }

  // d1: 8
  for (i = 0; i < 8; i++) {
    if (!IsHexChar(Str[i])) return EFI_INVALID_PARAMETER;
    v = HexVal(Str[i]);
    d1 = (d1 << 4) | (UINT32)v;
  }

  // d2: 4 at 9..12
  for (i = 9; i < 13; i++) {
    if (!IsHexChar(Str[i])) return EFI_INVALID_PARAMETER;
    v = HexVal(Str[i]);
    d2 = (UINT16)((d2 << 4) | (UINT16)v);
  }

  // d3: 4 at 14..17
  for (i = 14; i < 18; i++) {
    if (!IsHexChar(Str[i])) return EFI_INVALID_PARAMETER;
    v = HexVal(Str[i]);
    d3 = (UINT16)((d3 << 4) | (UINT16)v);
  }

  // d4[0..1]: 2+2 at 19..22
  if (!IsHexChar(Str[19]) || !IsHexChar(Str[20]) || !IsHexChar(Str[21]) || !IsHexChar(Str[22])) {
    return EFI_INVALID_PARAMETER;
  }
  d4[0] = (UINT8)((HexVal(Str[19]) << 4) | HexVal(Str[20]));
  d4[1] = (UINT8)((HexVal(Str[21]) << 4) | HexVal(Str[22]));

  // d4[2..7]: 12 at 24..35 -> 6 bytes
  for (i = 0; i < 6; i++) {
    UINTN pos = 24 + i * 2;
    if (!IsHexChar(Str[pos]) || !IsHexChar(Str[pos + 1])) return EFI_INVALID_PARAMETER;
    d4[2 + i] = (UINT8)((HexVal(Str[pos]) << 4) | HexVal(Str[pos + 1]));
  }

  OutGuid->Data1 = d1;
  OutGuid->Data2 = d2;
  OutGuid->Data3 = d3;
  CopyMem(OutGuid->Data4, d4, sizeof(d4));
  return EFI_SUCCESS;
}

STATIC VOID
PrintHexDump(IN UINT8 *Data, IN UINTN DataSize)
{
  UINTN Offset;
  UINTN i;

  Print(L"    ");
  for (i = 0; i < 16; i++) {
    Print(L"%02x ", (UINTN)i);
  }
  Print(L"\n");

  for (Offset = 0; Offset < DataSize; Offset += 16) {
    Print(L"%02x  ", Offset & 0xFF);
    for (i = 0; i < 16; i++) {
      if (Offset + i < DataSize) {
        Print(L"%02x ", Data[Offset + i]);
      } else {
        Print(L"   ");
      }
    }
    Print(L"\n");
  }
}

STATIC VOID
PrintOneVariableDetailed(IN CHAR16 *VarName, IN EFI_GUID *VendorGuid)
{
  EFI_STATUS Status;
  UINTN DataSize = 0;
  UINT32 Attr = 0;
  UINT8 *Data = NULL;

  if (VarName == NULL || VendorGuid == NULL) {
    return;
  }

  Status = gRT->GetVariable(VarName, VendorGuid, &Attr, &DataSize, NULL);
  if (Status == EFI_BUFFER_TOO_SMALL && DataSize > 0) {
    Data = (UINT8 *)AllocateZeroPool(DataSize);
    if (Data == NULL) {
      SetTextAttr(EFI_LIGHTRED);
      Print(L"AllocateZeroPool failed\n");
      SetTextAttr(EFI_LIGHTGRAY);
      return;
    }
    Status = gRT->GetVariable(VarName, VendorGuid, &Attr, &DataSize, Data);
  }

  if (EFI_ERROR(Status)) {
    SetTextAttr(EFI_LIGHTRED);
    Print(L"GetVariable failed: %r\n", Status);
    SetTextAttr(EFI_LIGHTGRAY);
    if (Data != NULL) FreePool(Data);
    return;
  }

  SetTextAttr(EFI_LIGHTGREEN);
  Print(L"Vendor GUID: ");
  PrintGuidLine(VendorGuid);
  Print(L"\n");
  SetTextAttr(EFI_LIGHTGRAY);

  Print(L"Name: %s  Data Size: %u\n", VarName, (UINT32)DataSize);

  if (DataSize > 0 && Data != NULL) {
    PrintHexDump(Data, DataSize);
  } else {
    Print(L"(No Data)\n");
  }

  Print(L"\n");

  if (Data != NULL) {
    FreePool(Data);
  }
}

STATIC EFI_STATUS
ListOrFilterVariablesDetailed(BOOLEAN FilterByName, CHAR16 *TargetName, BOOLEAN FilterByGuid, EFI_GUID *TargetGuid, OUT UINTN *OutFound)
{
  EFI_STATUS Status;
  UINTN NameBufSize;
  CHAR16 *NameBuf = NULL;
  EFI_GUID Guid;
  UINTN Found = 0;

  NameBufSize = 1024; // bytes
  NameBuf = (CHAR16 *)AllocateZeroPool(NameBufSize);
  if (NameBuf == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  ZeroMem(&Guid, sizeof(Guid));
  NameBuf[0] = L'\0';

  while (TRUE) {
    UINTN ThisSize = NameBufSize;

    Status = gRT->GetNextVariableName(&ThisSize, NameBuf, &Guid);
    if (Status == EFI_BUFFER_TOO_SMALL) {
      FreePool(NameBuf);
      NameBufSize = ThisSize + 2 * sizeof(CHAR16);
      NameBuf = (CHAR16 *)AllocateZeroPool(NameBufSize);
      if (NameBuf == NULL) {
        return EFI_OUT_OF_RESOURCES;
      }
      continue;
    }

    if (Status == EFI_NOT_FOUND) {
      break;
    }

    if (EFI_ERROR(Status)) {
      FreePool(NameBuf);
      return Status;
    }

    if (FilterByName) {
      if (TargetName == NULL) continue;
      if (StrCmp(NameBuf, TargetName) != 0) continue;
    }

    if (FilterByGuid) {
      if (TargetGuid == NULL) continue;
      if (!CompareGuid(&Guid, TargetGuid)) continue;
    }

    PrintOneVariableDetailed(NameBuf, &Guid);
    Found++;
  }

  if (OutFound) *OutFound = Found;

  FreePool(NameBuf);
  return EFI_SUCCESS;
}

// =============================
// List-all table view (Name | DataSize | GUID) with paging
// =============================
typedef struct {
  CHAR16  *Name;
  EFI_GUID Guid;
  UINTN   DataSize;
} VAR_ITEM;

STATIC EFI_STATUS
GetVariableDataSizeQuick(IN CHAR16 *Name, IN EFI_GUID *Guid, OUT UINTN *OutSize)
{
  EFI_STATUS Status;
  UINTN Size = 0;
  UINT32 Attr = 0;

  if (OutSize) *OutSize = 0;

  Status = gRT->GetVariable(Name, Guid, &Attr, &Size, NULL);
  if (Status == EFI_BUFFER_TOO_SMALL) {
    if (OutSize) *OutSize = Size;
    return EFI_SUCCESS;
  }
  if (Status == EFI_SUCCESS) {
    if (OutSize) *OutSize = Size;
    return EFI_SUCCESS;
  }
  return Status;
}

STATIC EFI_STATUS
CollectAllVariables(OUT VAR_ITEM **OutItems, OUT UINTN *OutCount)
{
  EFI_STATUS Status;
  UINTN NameBufSize;
  CHAR16 *NameBuf = NULL;
  EFI_GUID Guid;

  VAR_ITEM *Items = NULL;
  UINTN Count = 0, Cap = 0;

  if (OutItems) *OutItems = NULL;
  if (OutCount) *OutCount = 0;

  NameBufSize = 1024;
  NameBuf = (CHAR16 *)AllocateZeroPool(NameBufSize);
  if (NameBuf == NULL) return EFI_OUT_OF_RESOURCES;

  ZeroMem(&Guid, sizeof(Guid));
  NameBuf[0] = L'\0';

  while (TRUE) {
    UINTN ThisSize = NameBufSize;

    Status = gRT->GetNextVariableName(&ThisSize, NameBuf, &Guid);
    if (Status == EFI_BUFFER_TOO_SMALL) {
      FreePool(NameBuf);
      NameBufSize = ThisSize + 2 * sizeof(CHAR16);
      NameBuf = (CHAR16 *)AllocateZeroPool(NameBufSize);
      if (NameBuf == NULL) return EFI_OUT_OF_RESOURCES;
      continue;
    }
    if (Status == EFI_NOT_FOUND) break;
    if (EFI_ERROR(Status)) { FreePool(NameBuf); return Status; }

    if (Count >= Cap) {
      UINTN NewCap = (Cap == 0) ? 128 : (Cap * 2);
      VAR_ITEM *NewItems = (VAR_ITEM *)AllocateZeroPool(sizeof(VAR_ITEM) * NewCap);
      if (NewItems == NULL) {
        FreePool(NameBuf);
        if (Items) {
          for (UINTN k = 0; k < Count; k++) { if (Items[k].Name) FreePool(Items[k].Name); }
          FreePool(Items);
        }
        return EFI_OUT_OF_RESOURCES;
      }
      if (Items) {
        CopyMem(NewItems, Items, sizeof(VAR_ITEM) * Count);
        FreePool(Items);
      }
      Items = NewItems;
      Cap = NewCap;
    }

    Items[Count].Name = AllocateCopyPool(StrSize(NameBuf), NameBuf);
    if (Items[Count].Name == NULL) {
      FreePool(NameBuf);
      for (UINTN k = 0; k < Count; k++) { if (Items[k].Name) FreePool(Items[k].Name); }
      FreePool(Items);
      return EFI_OUT_OF_RESOURCES;
    }

    CopyMem(&Items[Count].Guid, &Guid, sizeof(EFI_GUID));
    GetVariableDataSizeQuick(NameBuf, &Guid, &Items[Count].DataSize);

    Count++;
  }

  FreePool(NameBuf);

  if (OutItems) *OutItems = Items;
  if (OutCount) *OutCount = Count;
  return EFI_SUCCESS;
}

STATIC VOID
FreeAllVariables(VAR_ITEM *Items, UINTN Count)
{
  if (Items == NULL) return;
  for (UINTN i = 0; i < Count; i++) {
    if (Items[i].Name) FreePool(Items[i].Name);
  }
  FreePool(Items);
}

STATIC VOID
DrawListAllTable(VAR_ITEM *Items, UINTN Count, UINTN Top, UINTN Sel, UINTN PageRows)
{
  ClearScreen();

  SetTextAttr(EFI_LIGHTGREEN);
  Print(L"Default Vendor GUID: ");
  PrintGuidLine(&mDefaultVendorGuid);
  Print(L"\n");
  Print(L"Variable Application\n\n");
  SetTextAttr(EFI_LIGHTGRAY);

  // header
  SetTextAttr(EFI_WHITE | EFI_BACKGROUND_BLUE);
  Print(L"Variable Name                         | Data Size | Vendor GUID\n");
  SetTextAttr(EFI_LIGHTGRAY);

  // rows
  for (UINTN r = 0; r < PageRows; r++) {
    UINTN idx = Top + r;
    if (idx >= Count) {
      Print(L"\n");
      continue;
    }

    if (idx == Sel) {
      SetTextAttr(EFI_WHITE | EFI_BACKGROUND_BLUE);
    } else {
      SetTextAttr(EFI_LIGHTGRAY);
    }

    // fixed-ish formatting: name left, size right, GUID
    // name: 35 chars max (truncate)
    CHAR16 NameBuf[48];
    ZeroMem(NameBuf, sizeof(NameBuf));
    if (Items[idx].Name) {
      UINTN nlen = StrLen(Items[idx].Name);
      UINTN copy = (nlen > 35) ? 35 : nlen;
      CopyMem(NameBuf, Items[idx].Name, copy * sizeof(CHAR16));
      NameBuf[copy] = L'\0';
    }

    Print(L"%-35s | %8u | ", NameBuf, (UINT32)Items[idx].DataSize);
    PrintGuidLine(&Items[idx].Guid);
    Print(L"\n");
  }

  SetTextAttr(EFI_LIGHTGRAY);

  // footer
  UINTN Page = (Count == 0) ? 0 : (Sel / PageRows) + 1;
  UINTN PageCount = (Count == 0) ? 0 : ((Count + PageRows - 1) / PageRows);

  UINTN ShowStart = (Count == 0) ? 0 : (Top + 1);
  UINTN ShowEnd   = (Count == 0) ? 0 : ((Top + PageRows) > Count ? Count : (Top + PageRows));

  Print(L"\nTotal: %u   Page: %u/%u   Showing: %u-%u\n",
        (UINT32)Count, (UINT32)Page, (UINT32)PageCount, (UINT32)ShowStart, (UINT32)ShowEnd);
  Print(L"Keys: Up/Down  PgUp/PgDn  Home/End  ESC exit\n");
}

STATIC VOID
DoListAll(VOID)
{
  EFI_STATUS Status;
  VAR_ITEM *Items = NULL;
  UINTN Count = 0;

  UINTN Cols = 0, Rows = 0;
  UINTN PageRows = 15; // fallback
  UINTN Top = 0;
  UINTN Sel = 0;

  Status = CollectAllVariables(&Items, &Count);
  if (EFI_ERROR(Status)) {
    SetTextAttr(EFI_LIGHTRED);
    Print(L"Collect variables failed: %r\n", Status);
    SetTextAttr(EFI_LIGHTGRAY);
    WaitAnyKey();
    return;
  }

  if (!EFI_ERROR(GetConsoleSize(&Cols, &Rows)) && Rows > 10) {
    // header: 3 lines + header row(1) + footer(3) => about 7
    // keep safe margins
    UINTN usable = Rows > 9 ? (Rows - 9) : 10;
    PageRows = (usable < 5) ? 5 : usable;
  }

  while (TRUE) {
    // adjust Top so Sel always visible
    if (Sel < Top) Top = Sel;
    if (Sel >= Top + PageRows) Top = Sel - (PageRows - 1);

    DrawListAllTable(Items, Count, Top, Sel, PageRows);

    EFI_INPUT_KEY Key;
    while (gST->ConIn->ReadKeyStroke(gST->ConIn, &Key) == EFI_NOT_READY) {
      gBS->Stall(1000);
    }

    if (Key.ScanCode == SCAN_ESC) {
      break;
    }

    if (Key.ScanCode == SCAN_UP) {
      if (Sel > 0) Sel--;
      continue;
    }

    if (Key.ScanCode == SCAN_DOWN) {
      if (Count > 0 && Sel + 1 < Count) Sel++;
      continue;
    }

    if (Key.ScanCode == SCAN_PAGE_UP) {
      if (Sel >= PageRows) Sel -= PageRows;
      else Sel = 0;
      continue;
    }

    if (Key.ScanCode == SCAN_PAGE_DOWN) {
      if (Count == 0) continue;
      if (Sel + PageRows < Count) Sel += PageRows;
      else Sel = Count - 1;
      continue;
    }

    if (Key.ScanCode == SCAN_HOME) {
      Sel = 0;
      continue;
    }

    if (Key.ScanCode == SCAN_END) {
      if (Count > 0) Sel = Count - 1;
      continue;
    }
  }

  FreeAllVariables(Items, Count);
}

STATIC VOID
DoSearchByName(VOID)
{
  CHAR16 Name[LINE_MAX_CHARS];
  EFI_STATUS Status;
  UINTN Found = 0;

  ClearScreen();
  Print(L"Variable name: ");
  ReadLine(Name, LINE_MAX_CHARS);

  Print(L"\n");
  Status = ListOrFilterVariablesDetailed(TRUE, Name, FALSE, NULL, &Found);
  if (EFI_ERROR(Status)) {
    SetTextAttr(EFI_LIGHTRED);
    Print(L"Search failed: %r\n", Status);
    SetTextAttr(EFI_LIGHTGRAY);
  } else {
    Print(L"Number of variables found: %u\n", (UINT32)Found);
  }
  WaitAnyKey();
}

STATIC EFI_STATUS
PromptVendorGuidMasked(IN EFI_GUID *DefaultGuid, OUT EFI_GUID *OutGuid)
{
  CHAR16 GuidMaskStr[64]; // need >= 37
  EFI_STATUS Status;

  if (DefaultGuid == NULL || OutGuid == NULL) return EFI_INVALID_PARAMETER;

  Print(L"Vendor GUID (leave empty to use default): ");

  Status = ReadGuidMaskedLine(GuidMaskStr, sizeof(GuidMaskStr)/sizeof(GuidMaskStr[0]));
  if (Status == EFI_NOT_READY) {
    SetTextAttr(EFI_LIGHTRED);
    Print(L"GUID not complete. Please fill all hex digits.\n");
    SetTextAttr(EFI_LIGHTGRAY);
    return EFI_INVALID_PARAMETER;
  }
  if (EFI_ERROR(Status)) return Status;

  if (GuidMaskStr[0] == L'\0') {
    CopyMem(OutGuid, DefaultGuid, sizeof(EFI_GUID));
    return EFI_SUCCESS;
  }

  Status = ParseGuidString(GuidMaskStr, OutGuid);
  if (EFI_ERROR(Status)) {
    SetTextAttr(EFI_LIGHTRED);
    Print(L"Invalid GUID format.\n");
    SetTextAttr(EFI_LIGHTGRAY);
    return EFI_INVALID_PARAMETER;
  }

  return EFI_SUCCESS;
}

STATIC VOID
DoSearchByGuid(IN EFI_GUID *DefaultGuid)
{
  EFI_GUID Guid;
  EFI_STATUS Status;
  UINTN Found = 0;

  ClearScreen();
  Print(L"Search variables by vendor GUID\n\n");

  Status = PromptVendorGuidMasked(DefaultGuid, &Guid);
  if (EFI_ERROR(Status)) {
    WaitAnyKey();
    return;
  }

  Print(L"\n");
  Status = ListOrFilterVariablesDetailed(FALSE, NULL, TRUE, &Guid, &Found);
  if (EFI_ERROR(Status)) {
    SetTextAttr(EFI_LIGHTRED);
    Print(L"Search failed: %r\n", Status);
    SetTextAttr(EFI_LIGHTGRAY);
  } else {
    Print(L"Number of variables found: %u\n", (UINT32)Found);
  }
  WaitAnyKey();
}

STATIC VOID
DoDeleteVariable(VOID)
{
  CHAR16 Name[LINE_MAX_CHARS];
  EFI_GUID Guid;
  EFI_STATUS Status;

  ClearScreen();
  Print(L"Delete variable\n\n");

  Print(L"Variable name to delete: ");
  ReadLine(Name, LINE_MAX_CHARS);

  Status = PromptVendorGuidMasked(&mDefaultVendorGuid, &Guid);
  if (EFI_ERROR(Status)) {
    WaitAnyKey();
    return;
  }

  // Delete: Attributes=0, DataSize=0, Data=NULL
  Status = gRT->SetVariable(Name, &Guid, 0, 0, NULL);
  if (EFI_ERROR(Status)) {
    SetTextAttr(EFI_LIGHTRED);
    Print(L"Delete failed: %r\n", Status);
    SetTextAttr(EFI_LIGHTGRAY);
  } else {
    SetTextAttr(EFI_LIGHTGREEN);
    Print(L"Deleted OK.\n");
    SetTextAttr(EFI_LIGHTGRAY);
  }

  WaitAnyKey();
}

STATIC VOID
DoCreateVariable(VOID)
{
  // Simple create: value stored as CHAR16 string entered by user
  CHAR16 Name[LINE_MAX_CHARS];
  CHAR16 Value[LINE_MAX_CHARS];
  EFI_GUID Guid;
  EFI_STATUS Status;

  UINT32 Attr = EFI_VARIABLE_NON_VOLATILE |
                EFI_VARIABLE_BOOTSERVICE_ACCESS |
                EFI_VARIABLE_RUNTIME_ACCESS;

  ClearScreen();
  Print(L"Create new variable\n\n");

  Print(L"New variable name: ");
  ReadLine(Name, LINE_MAX_CHARS);

  Status = PromptVendorGuidMasked(&mDefaultVendorGuid, &Guid);
  if (EFI_ERROR(Status)) {
    WaitAnyKey();
    return;
  }

  Print(L"Value (stored as CHAR16 string): ");
  ReadLine(Value, LINE_MAX_CHARS);

  // store as UTF-16 including null terminator
  Status = gRT->SetVariable(
                  Name,
                  &Guid,
                  Attr,
                  (StrLen(Value) + 1) * sizeof(CHAR16),
                  Value
                );

  if (EFI_ERROR(Status)) {
    SetTextAttr(EFI_LIGHTRED);
    Print(L"Create/Set failed: %r\n", Status);
    SetTextAttr(EFI_LIGHTGRAY);
  } else {
    SetTextAttr(EFI_LIGHTGREEN);
    Print(L"Create/Set OK.\n");
    SetTextAttr(EFI_LIGHTGRAY);
  }

  WaitAnyKey();
}

STATIC VOID
ShowMenu(IN UINTN Sel)
{
  ClearScreen();

  SetTextAttr(EFI_LIGHTGREEN);
  Print(L"Default Vendor GUID: ");
  PrintGuidLine(&mDefaultVendorGuid);
  Print(L"\n");
  Print(L"Variable Application\n\n");
  SetTextAttr(EFI_LIGHTGRAY);

  // 0 List all
  // 1 Search by name
  // 2 Search by vendor GUID
  // 3 Create
  // 4 Delete
  // 5 Exit
  for (UINTN i = 0; i < 6; i++) {
    if (i == Sel) {
      SetTextAttr(EFI_WHITE | EFI_BACKGROUND_BLUE);
    } else {
      SetTextAttr(EFI_LIGHTGRAY);
    }

    switch (i) {
      case 0: Print(L"List all variables\n"); break;
      case 1: Print(L"Search variables by name\n"); break;
      case 2: Print(L"Search variables by vendor GUID\n"); break;
      case 3: Print(L"Create new variable\n"); break;
      case 4: Print(L"Delete variable\n"); break;
      case 5: Print(L"Exit\n"); break;
      default: break;
    }
  }

  SetTextAttr(EFI_LIGHTGRAY);
  Print(L"\nUse Up/Down and Enter.\n");
}

EFI_STATUS
EFIAPI
UefiMain(IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable)
{
  UINTN Sel = 0;
  EFI_INPUT_KEY Key;

  while (TRUE) {
    ShowMenu(Sel);

    while (gST->ConIn->ReadKeyStroke(gST->ConIn, &Key) == EFI_NOT_READY) {
      gBS->Stall(1000);
    }

    if (Key.ScanCode == SCAN_UP) {
      if (Sel > 0) Sel--;
      continue;
    }

    if (Key.ScanCode == SCAN_DOWN) {
      if (Sel < 5) Sel++;
      continue;
    }

    if (Key.UnicodeChar == CHAR_CARRIAGE_RETURN) {
      switch (Sel) {
        case 0: DoListAll(); break;
        case 1: DoSearchByName(); break;
        case 2: DoSearchByGuid(&mDefaultVendorGuid); break;
        case 3: DoCreateVariable(); break;
        case 4: DoDeleteVariable(); break;
        case 5: return EFI_SUCCESS;
        default: break;
      }
    }
  }
}
