//
// ===============================
//  VariableTool.c
//  目的：
//    1) 列出所有 UEFI 變數 (Variable Name / Data Size / Vendor GUID) - 表格 UI + 翻頁
//    2) 依變數名搜尋 (跨所有 GUID)
//    3) 依 Vendor GUID 搜尋（GUID 輸入採用 mask：________-____-____-____-____________）
//    4) 建立變數 (SetVariable)
//    5) 刪除變數 (SetVariable with Attr=0, Size=0, Data=NULL)
//
//  教科書重點：
//    - UEFI Variable 走 Runtime Services (gRT)
//    - 列舉變數用 GetNextVariableName()
//    - 查變數內容/大小用 GetVariable()
//    - 建立/修改/刪除用 SetVariable()
//    - UI 鍵盤輸入用 SimpleTextIn Protocol 的 ReadKeyStroke()
//    - UI 顯示/游標控制用 SimpleTextOut Protocol
// ===============================
//

#include <Uefi.h>                               // UEFI 核心型別/常數/協定宣告 (EFI_STATUS 等)
#include <Base.h>                               // EDK2 基礎型別/巨集 (UINTN/BOOLEAN 等)

#include <Library/UefiLib.h>                    // Print() 等便利輸出函式
#include <Library/UefiApplicationEntryPoint.h>  // UefiMain() 入口點宣告
#include <Library/UefiRuntimeServicesTableLib.h>// gRT (Runtime Services Table) 全域指標
#include <Library/UefiBootServicesTableLib.h>   // gBS (Boot Services Table) 全域指標

#include <Library/MemoryAllocationLib.h>        // AllocateZeroPool/FreePool 等
#include <Library/BaseMemoryLib.h>              // CopyMem/ZeroMem 等
#include <Library/BaseLib.h>                    // CompareGuid 等基礎 helper
#include <Library/PrintLib.h>                   // （此檔主要用 Print()，仍保留）

#define LINE_MAX_CHARS  128                     // 一般輸入緩衝區最大字元數（含 '\0'）

//
// Default Vendor GUID：你畫面最上面顯示的預設 GUID
// 結構 EFI_GUID：
//   Data1: UINT32
//   Data2: UINT16
//   Data3: UINT16
//   Data4: UINT8[8]
//
STATIC EFI_GUID mDefaultVendorGuid = {
  0x37893825,                                   // Data1
  0x3B85,                                       // Data2
  0x02D0,                                       // Data3
  { 0x37, 0x89, 0x33, 0xF9, 0x00, 0x00, 0x00, 0x00 } // Data4[0..7]
};

//
// =======================================
// WaitAnyKey()
// 教科書層：
//   - 用途：暫停畫面，等待使用者按任意鍵，避免訊息閃過
//   - 依賴：gST->ConIn->ReadKeyStroke()
//   - 常見回傳：
//       EFI_NOT_READY  : 沒有按鍵可讀（非錯誤）
//       EFI_SUCCESS    : 讀到按鍵
// =======================================
//
STATIC VOID
WaitAnyKey(VOID)
{
  EFI_INPUT_KEY Key;                            // UEFI 鍵盤輸入結構（含 ScanCode / UnicodeChar）

  Print(L"\nPress any key to continue...");     // Print(): UefiLib 提供的 Unicode 輸出

  //
  // ReadKeyStroke() 如果沒有鍵按下會回 EFI_NOT_READY，
  // 所以用 while 持續輪詢 + Stall() 小睡，避免 CPU 忙等 100%
  //
  while (gST->ConIn->ReadKeyStroke(gST->ConIn, &Key) == EFI_NOT_READY) {
    gBS->Stall(1000);                           // Stall(微秒)：這裡 1000us = 1ms
  }

  Print(L"\n");                                 // 換行
}

//
// =======================================
// SetTextAttr()
// 教科書層：
//   - 用途：設定文字顏色/背景（SetAttribute）
//   - Attr 由 EFI_xxx 顏色常數 OR 起來
//   - 例如 EFI_WHITE | EFI_BACKGROUND_BLUE
// =======================================
//
STATIC VOID
SetTextAttr(UINTN Attr)                         // Attr: 想設定的字/背景屬性
{
  //
  // 防呆：確保 System Table / ConOut 存在
  // （一般 UEFI App 幾乎都存在，但寫工具最好保守）
  //
  if (gST != NULL && gST->ConOut != NULL) {     // if：避免 NULL pointer crash
    gST->ConOut->SetAttribute(                  // SetAttribute(This, Attribute)
      gST->ConOut,                              // This：文字輸出協定實例
      Attr                                     // Attribute：顏色屬性
    );
  }
}

//
// =======================================
// ClearScreen()
// 教科書層：
//   - 用途：清畫面（ConOut->ClearScreen）
// =======================================
//
STATIC VOID
ClearScreen(VOID)
{
  if (gST && gST->ConOut) {                     // if：確保 ConOut 存在
    gST->ConOut->ClearScreen(gST->ConOut);      // ClearScreen(This)
  }
}

//
// =======================================
// GetConsoleSize()
// 教科書層：
//   - 用途：取得目前主控台（文字模式）欄數/列數
//   - 方法：ConOut->QueryMode(ModeNumber, &Cols, &Rows)
//   - 你遇到的 Columns 編譯錯誤原因：
//       EFI_SIMPLE_TEXT_OUTPUT_MODE 沒有 Columns 欄位，
//       正確做法是 QueryMode()。
//   - 常見回傳：
//       EFI_SUCCESS / EFI_UNSUPPORTED / EFI_DEVICE_ERROR
// =======================================
//
STATIC EFI_STATUS
GetConsoleSize(OUT UINTN *OutCols, OUT UINTN *OutRows) // OUT：輸出欄數/列數
{
  UINTN Cols = 0, Rows = 0;                      // 暫存變數：QueryMode 會填入
  EFI_STATUS Status;                             // UEFI 函式回傳狀態碼

  //
  // 防呆：若 ConOut/QueryMode 不存在，代表此環境不支援文字模式查詢
  //
  if (gST == NULL || gST->ConOut == NULL || gST->ConOut->QueryMode == NULL) {
    return EFI_UNSUPPORTED;                      // 表示不支援此功能
  }

  //
  // QueryMode(This, ModeNumber, &Cols, &Rows)
  // 參數說明：
  //   This      : EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL*
  //   ModeNumber: 要查詢的模式（此處使用目前模式 gST->ConOut->Mode->Mode）
  //   Columns   : OUT 欄數
  //   Rows      : OUT 列數
  //
  Status = gST->ConOut->QueryMode(
    gST->ConOut,                                 // This
    gST->ConOut->Mode->Mode,                     // ModeNumber（目前模式）
    &Cols,                                       // OUT Columns
    &Rows                                        // OUT Rows
  );

  if (EFI_ERROR(Status)) {                       // EFI_ERROR(): 判斷是否為錯誤碼（最高 bit ）
    return Status;                               // 若失敗直接回傳，交給上層決定 fallback
  }

  if (OutCols) *OutCols = Cols;                  // 若呼叫者給了指標才寫入（避免 NULL）
  if (OutRows) *OutRows = Rows;

  return EFI_SUCCESS;                            // 成功
}

//
// =======================================
// PrintGuidLine()
// 教科書層：
//   - 用途：以「標準 GUID 格式」輸出
//     xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
//   - 注意：這只是輸出，不做 endian 轉換，因為 EFI_GUID 欄位已是分好欄
// =======================================
//
STATIC VOID
PrintGuidLine(IN EFI_GUID *Guid)                 // IN：要輸出的 GUID
{
  // Print(): %08x 等格式輸出
  Print(L"%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        Guid->Data1, Guid->Data2, Guid->Data3,
        Guid->Data4[0], Guid->Data4[1],
        Guid->Data4[2], Guid->Data4[3], Guid->Data4[4],
        Guid->Data4[5], Guid->Data4[6], Guid->Data4[7]);
}

//
// =======================================
// IsHexChar()
// 教科書層：
//   - 用途：判斷是否為 0-9 / a-f / A-F
//   - 給 GUID mask 輸入使用
// =======================================
//
STATIC BOOLEAN
IsHexChar(CHAR16 C)                             // CHAR16：UEFI 用 UTF-16 字元型別
{
  return ((C >= L'0' && C <= L'9') ||
          (C >= L'a' && C <= L'f') ||
          (C >= L'A' && C <= L'F'));
}

//
// =======================================
// ToUpperHex()
// 教科書層：
//   - 用途：把 a-f 轉成 A-F，讓顯示統一
// =======================================
//
STATIC CHAR16
ToUpperHex(CHAR16 C)
{
  if (C >= L'a' && C <= L'f') return (CHAR16)(C - (L'a' - L'A')); // ASCII 差值轉大寫
  return C;                                     // 不是小寫 a-f 就原樣回傳
}

//
// =======================================
// HexVal()
// 教科書層：
//   - 用途：把單一 hex 字元轉成數值 0..15
//   - 失敗回 -1
// =======================================
//
STATIC INTN
HexVal(CHAR16 C)
{
  if (C >= L'0' && C <= L'9') return (INTN)(C - L'0');           // '0'..'9'
  if (C >= L'a' && C <= L'f') return (INTN)(C - L'a' + 10);      // 'a'..'f'
  if (C >= L'A' && C <= L'F') return (INTN)(C - L'A' + 10);      // 'A'..'F'
  return -1;                                                     // 非 hex 字元
}

//
// =======================================
// ReadLine()
// 教科書層：
//   - 用途：一般「直覺式」文字輸入（不用 mask）
//   - 特性：
//       - Enter 結束
//       - Backspace 退格並刪除畫面字元
//       - 忽略不可顯示字元
//   - BufferChars 以「CHAR16 字元數」計算（不是 bytes）
// =======================================
//
STATIC EFI_STATUS
ReadLine(IN CHAR16 *Buffer, IN UINTN BufferChars)
{
  UINTN Index = 0;                               // 目前輸入到 Buffer 的位置
  EFI_INPUT_KEY Key;                             // 讀鍵盤用

  if (Buffer == NULL || BufferChars == 0) {      // 防呆：沒有 buffer 或長度 0
    return EFI_INVALID_PARAMETER;                // 參數錯誤
  }

  Buffer[0] = L'\0';                             // 先設空字串（避免垃圾值）

  while (TRUE) {                                 // 無限迴圈，直到 Enter return
    while (gST->ConIn->ReadKeyStroke(gST->ConIn, &Key) == EFI_NOT_READY) {
      gBS->Stall(1000);                          // 沒鍵可讀就等一下（避免忙等）
    }

    if (Key.UnicodeChar == CHAR_CARRIAGE_RETURN) {// Enter 鍵（\r）
      Print(L"\n");                               // 讓游標換到下一行
      Buffer[Index] = L'\0';                      // 結尾補 '\0'
      return EFI_SUCCESS;                         // 成功回傳
    }

    if (Key.UnicodeChar == CHAR_BACKSPACE) {     // Backspace 鍵
      if (Index > 0) {                            // if：有字才可以退
        Index--;                                  // 指標往回一格
        Buffer[Index] = L'\0';                    // 讓字串提前結束
        Print(L"\b \b");                          // 典型 console 退格擦除：回退->空白->回退
      }
      continue;                                   // 跳過後續處理
    }

    // 忽略不可列印 ASCII 範圍之外（簡化 UI）
    if (Key.UnicodeChar < 0x20 || Key.UnicodeChar > 0x7E) {
      continue;
    }

    // 確保 buffer 還有空間（要留 1 格 '\0'）
    if (Index + 1 < BufferChars) {
      Buffer[Index++] = Key.UnicodeChar;          // 寫入字元後 Index++
      Buffer[Index] = L'\0';                      // 立即維持 NUL 結尾
      Print(L"%c", Key.UnicodeChar);              // 回顯到畫面
    }
  }
}

//
// =======================================
// ParseGuidString()
// 教科書層：
//   - 用途：把 "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" 解析成 EFI_GUID
//   - 驗證：長度必須 36，且 '-' 在固定位置
//   - 常見錯誤回傳：EFI_INVALID_PARAMETER
// =======================================
//
STATIC EFI_STATUS
ParseGuidString(IN CHAR16 *Str, OUT EFI_GUID *OutGuid)
{
  UINTN Len;                                      // 字串長度
  UINTN i;                                        // 迴圈用
  INTN v;                                         // hex 值暫存

  UINT32 d1 = 0;                                  // Data1 組合用
  UINT16 d2 = 0, d3 = 0;                          // Data2/Data3 組合用
  UINT8  d4[8];                                   // Data4 8 bytes

  if (Str == NULL || OutGuid == NULL) {           // 防呆：輸入/輸出不能為 NULL
    return EFI_INVALID_PARAMETER;
  }

  Len = StrLen(Str);                              // StrLen：CHAR16 字串長度
  if (Len != 36) {                                // GUID 字串固定 36
    return EFI_INVALID_PARAMETER;
  }

  // 檢查 '-' 位置必須固定
  if (Str[8]  != L'-' || Str[13] != L'-' ||
      Str[18] != L'-' || Str[23] != L'-') {
    return EFI_INVALID_PARAMETER;
  }

  // Data1：8 個 hex 字元
  for (i = 0; i < 8; i++) {
    if (!IsHexChar(Str[i])) return EFI_INVALID_PARAMETER; // 若不是 hex，格式錯
    v = HexVal(Str[i]);                                   // 轉成 0..15
    d1 = (d1 << 4) | (UINT32)v;                            // 左移 4 bit + 新 nibble
  }

  // Data2：第 9..12 位（4 hex）
  for (i = 9; i < 13; i++) {
    if (!IsHexChar(Str[i])) return EFI_INVALID_PARAMETER;
    v = HexVal(Str[i]);
    d2 = (UINT16)((d2 << 4) | (UINT16)v);
  }

  // Data3：第 14..17 位（4 hex）
  for (i = 14; i < 18; i++) {
    if (!IsHexChar(Str[i])) return EFI_INVALID_PARAMETER;
    v = HexVal(Str[i]);
    d3 = (UINT16)((d3 << 4) | (UINT16)v);
  }

  // Data4[0..1]：第 19..22 位（4 hex => 2 bytes）
  if (!IsHexChar(Str[19]) || !IsHexChar(Str[20]) ||
      !IsHexChar(Str[21]) || !IsHexChar(Str[22])) {
    return EFI_INVALID_PARAMETER;
  }
  d4[0] = (UINT8)((HexVal(Str[19]) << 4) | HexVal(Str[20])); // byte0
  d4[1] = (UINT8)((HexVal(Str[21]) << 4) | HexVal(Str[22])); // byte1

  // Data4[2..7]：第 24..35 位（12 hex => 6 bytes）
  for (i = 0; i < 6; i++) {
    UINTN pos = 24 + i * 2;                          // 每次取兩個 hex
    if (!IsHexChar(Str[pos]) || !IsHexChar(Str[pos + 1])) return EFI_INVALID_PARAMETER;
    d4[2 + i] = (UINT8)((HexVal(Str[pos]) << 4) | HexVal(Str[pos + 1]));
  }

  // 寫入 OutGuid
  OutGuid->Data1 = d1;
  OutGuid->Data2 = d2;
  OutGuid->Data3 = d3;
  CopyMem(OutGuid->Data4, d4, sizeof(d4));           // CopyMem：把 d4[] 複製到 GUID

  return EFI_SUCCESS;
}

//
// =======================================
// ReadGuidMaskedLine()
// 教科書層：
//   - 用途：GUID 輸入「mask 模式」：先顯示 ________-____-____-____-____________
//   - 你要的行為：
//       1) 使用者輸入 hex 時，從左到右逐格把 '_' 替換掉
//       2) 會自動跳過 '-' 位置（'-' 已經固定顯示）
//       3) Backspace 會把上一格改回 '_' 並把游標回到那格
//       4) 直接 Enter（完全沒輸入）=> OutStr = 空字串 => 上層視為使用 Default GUID
//       5) 若只填一半按 Enter => 回 EFI_NOT_READY（代表未完成）
//   - 參數：
//       OutStr  : OUT，回傳 mask 被填完的 GUID 字串 or 空字串
//       OutChars: OutStr 的字元容量（必須 >= 37，含 '\0'）
// =======================================
//
STATIC EFI_STATUS
ReadGuidMaskedLine(OUT CHAR16 *OutStr, IN UINTN OutChars)
{
  STATIC CONST CHAR16 *Mask = L"________-____-____-____-____________"; // 36 chars
  UINTN EditablePos[32];                           // GUID 可編輯 hex 位數共 32 個
  UINTN EditCount = 0;                             // 實際收集到的可編輯位置數
  UINTN i;

  EFI_INPUT_KEY Key;                               // 鍵盤輸入
  UINTN StartCol, StartRow;                        // mask 印出前的游標位置（用來定位覆寫）
  UINTN CurEdit = 0;                               // 目前填到第幾個可編輯位
  BOOLEAN AnyTyped = FALSE;                        // 是否曾輸入過任何有效字元

  if (OutStr == NULL || OutChars < 37) {           // GUID+NUL 至少 37 CHAR16
    return EFI_INVALID_PARAMETER;
  }

  // 建立「可編輯位置表」：找出 Mask 中每個 '_' 的 index
  for (i = 0; i < 36; i++) {
    if (Mask[i] == L'_') {                         // '_' 表示這格可填 hex
      EditablePos[EditCount++] = i;                // 記錄可填位置
    }
  }

  // 初始化輸出字串為 mask
  StrCpyS(OutStr, OutChars, Mask);                 // StrCpyS：安全版複製（含長度檢查）

  // 記錄印出 mask 之前的游標位置（之後覆寫每格要用）
  StartCol = gST->ConOut->Mode->CursorColumn;      // CursorColumn：游標欄（只讀）
  StartRow = gST->ConOut->Mode->CursorRow;         // CursorRow：游標列（只讀）

  // 先把整條 mask 印出來（畫面上看到 ________-____...）
  Print(L"%s", OutStr);

  // 把游標移到第一個可輸入的位置（第一個 '_'）
  gST->ConOut->SetCursorPosition(
    gST->ConOut,
    StartCol + EditablePos[0],                     // 欄：起始欄 + 可編輯 offset
    StartRow                                       // 列：同一列
  );

  while (TRUE) {
    // 輪詢讀鍵盤：沒鍵就 EFI_NOT_READY
    while (gST->ConIn->ReadKeyStroke(gST->ConIn, &Key) == EFI_NOT_READY) {
      gBS->Stall(1000);
    }

    // Enter：結束輸入
    if (Key.UnicodeChar == CHAR_CARRIAGE_RETURN) {
      Print(L"\n");                                // 換行，避免游標停在 mask 那行

      if (!AnyTyped) {                             // 代表使用者完全沒輸入任何 hex
        OutStr[0] = L'\0';                         // 直接回空字串
        return EFI_SUCCESS;                        // 上層會把空視為用 default GUID
      }

      // 如果還有 '_' 沒被填完，代表 GUID 尚未完成
      for (i = 0; i < 36; i++) {
        if (OutStr[i] == L'_') {
          return EFI_NOT_READY;                    // 用這個狀態告訴上層：未完成
        }
      }

      return EFI_SUCCESS;                          // 32 hex 都填完 => 成功
    }

    // Backspace：退回一格並恢復 '_'
    if (Key.UnicodeChar == CHAR_BACKSPACE) {
      if (CurEdit > 0) {                           // 有填過才可以退回
        CurEdit--;                                 // 回到上一個可編輯位置

        AnyTyped = TRUE;                           // 仍視為使用過輸入模式（只是回退）

        // 將那格改回 '_'
        OutStr[EditablePos[CurEdit]] = L'_';

        // 游標移到那格並印 '_' 覆寫原字元
        gST->ConOut->SetCursorPosition(
          gST->ConOut,
          StartCol + EditablePos[CurEdit],
          StartRow
        );
        Print(L"_");

        // 再把游標留在那格（等待重新輸入）
        gST->ConOut->SetCursorPosition(
          gST->ConOut,
          StartCol + EditablePos[CurEdit],
          StartRow
        );
      }
      continue;                                    // Backspace 處理完跳過後續
    }

    // 若使用者按 '-'，直接忽略（因為 '-' 已固定在 mask）
    if (Key.UnicodeChar == L'-') {
      continue;
    }

    // 只允許輸入 hex 字元
    if (!IsHexChar(Key.UnicodeChar)) {
      continue;
    }

    // 若已填滿 32 格，忽略多餘輸入
    if (CurEdit >= EditCount) {
      continue;
    }

    AnyTyped = TRUE;                               // 記錄：使用者至少輸入過 1 個 hex

    // 寫入 OutStr 指定可編輯位置（轉大寫統一顯示）
    OutStr[EditablePos[CurEdit]] = ToUpperHex(Key.UnicodeChar);

    // 游標移到那格並印出該字元，覆蓋原本的 '_'
    gST->ConOut->SetCursorPosition(
      gST->ConOut,
      StartCol + EditablePos[CurEdit],
      StartRow
    );
    Print(L"%c", OutStr[EditablePos[CurEdit]]);

    CurEdit++;                                     // 前進到下一格可編輯位置

    if (CurEdit < EditCount) {
      // 把游標移到下一格可編輯 '_' 的位置
      gST->ConOut->SetCursorPosition(
        gST->ConOut,
        StartCol + EditablePos[CurEdit],
        StartRow
      );
    } else {
      // 全部填滿：游標移到 mask 結尾（位置 36）
      gST->ConOut->SetCursorPosition(
        gST->ConOut,
        StartCol + 36,
        StartRow
      );
    }
  }
}

//
// =======================================
// PrintHexDump()
// 教科書層：
//   - 用途：把 Data bytes 用 16 bytes/row 印出
//   - 注意：目前只有「Detailed 印變數」才用到；ListAll 表格模式不會呼叫
// =======================================
//
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

//
// =======================================
// PrintOneVariableDetailed()
// 教科書層：
//   - 用途：用 GetVariable() 讀出變數內容並印出 hex dump
//   - 流程：
//       1) 先用 Data=NULL 呼叫 GetVariable() 取得大小
//          常見回傳：EFI_BUFFER_TOO_SMALL，並回傳需要的 DataSize
//       2) AllocateZeroPool(DataSize)
//       3) 再呼叫 GetVariable() 取得真資料
//   - 常見回傳碼：
//       EFI_SUCCESS
//       EFI_NOT_FOUND             : 變數不存在
//       EFI_BUFFER_TOO_SMALL      : 需要更大緩衝區
//       EFI_INVALID_PARAMETER     : 參數錯
//       EFI_SECURITY_VIOLATION    : 權限/安全限制（平台政策）
// =======================================
//
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

  // 第一次呼叫：Data=NULL 取得大小
  Status = gRT->GetVariable(
    VarName,                                      // IN  VariableName
    VendorGuid,                                   // IN  VendorGuid
    &Attr,                                        // OUT Attributes
    &DataSize,                                    // IN/OUT DataSize：輸入為 0，輸出為所需大小
    NULL                                          // OUT Data：NULL 代表只問大小
  );

  if (Status == EFI_BUFFER_TOO_SMALL && DataSize > 0) {
    Data = (UINT8 *)AllocateZeroPool(DataSize);   // 配置資料緩衝
    if (Data == NULL) {
      SetTextAttr(EFI_LIGHTRED);
      Print(L"AllocateZeroPool failed\n");
      SetTextAttr(EFI_LIGHTGRAY);
      return;
    }

    // 第二次呼叫：真正取資料
    Status = gRT->GetVariable(
      VarName,
      VendorGuid,
      &Attr,
      &DataSize,                                  // IN/OUT：輸入 buffer 大小，輸出實際大小
      Data                                        // OUT：資料寫入此 buffer
    );
  }

  if (EFI_ERROR(Status)) {
    SetTextAttr(EFI_LIGHTRED);
    Print(L"GetVariable failed: %r\n", Status);   // %r：印 EFI_STATUS 文字
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

//
// =======================================
// ListOrFilterVariablesDetailed()
// 教科書層：
//   - 用途：列舉所有變數（可依 Name / GUID 過濾），並用 Detailed 模式印出（含 hex dump）
//   - 核心 API：GetNextVariableName()
//     參數：
//       VariableNameSize (IN/OUT) : 輸入 buffer bytes，輸出實際 bytes
//       VariableName (IN/OUT)     : 輸入上一個名稱，輸出下一個名稱
//       VendorGuid (IN/OUT)       : 同上
//   - 常見回傳：
//       EFI_SUCCESS
//       EFI_NOT_FOUND        : 列到最後
//       EFI_BUFFER_TOO_SMALL : NameBuf 不夠大，需要擴充
// =======================================
//
STATIC EFI_STATUS
ListOrFilterVariablesDetailed(
  BOOLEAN FilterByName,                           // IN：是否啟用「以名稱過濾」
  CHAR16 *TargetName,                             // IN：目標名稱（FilterByName=TRUE 時使用）
  BOOLEAN FilterByGuid,                           // IN：是否啟用「以 GUID 過濾」
  EFI_GUID *TargetGuid,                           // IN：目標 GUID（FilterByGuid=TRUE 時使用）
  OUT UINTN *OutFound                             // OUT：找到幾筆
)
{
  EFI_STATUS Status;
  UINTN NameBufSize;
  CHAR16 *NameBuf = NULL;
  EFI_GUID Guid;
  UINTN Found = 0;

  NameBufSize = 1024;                             // bytes（注意：GetNextVariableName 要的是 bytes）
  NameBuf = (CHAR16 *)AllocateZeroPool(NameBufSize);
  if (NameBuf == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  ZeroMem(&Guid, sizeof(Guid));
  NameBuf[0] = L'\0';                             // 起始：空字串代表「從第一個變數開始」

  while (TRUE) {
    UINTN ThisSize = NameBufSize;

    Status = gRT->GetNextVariableName(
      &ThisSize,                                  // IN/OUT：buffer bytes
      NameBuf,                                    // IN/OUT：上一個名稱 -> 下一個名稱
      &Guid                                       // IN/OUT：上一個 GUID -> 下一個 GUID
    );

    if (Status == EFI_BUFFER_TOO_SMALL) {
      // buffer 不夠大：釋放舊 buffer，配置更大（ThisSize 已是所需大小）
      FreePool(NameBuf);
      NameBufSize = ThisSize + 2 * sizeof(CHAR16);// 多留一點空間（保守）
      NameBuf = (CHAR16 *)AllocateZeroPool(NameBufSize);
      if (NameBuf == NULL) {
        return EFI_OUT_OF_RESOURCES;
      }
      // continue：重新回到 while，會用同一個 NameBuf/Guid 再呼叫一次
      continue;
    }

    if (Status == EFI_NOT_FOUND) {
      break;                                      // 列舉完成
    }

    if (EFI_ERROR(Status)) {
      FreePool(NameBuf);
      return Status;
    }

    // ========== 名稱過濾 ==========
    if (FilterByName) {
      if (TargetName == NULL) {
        continue;                                 // 沒目標名就跳過
      }
      if (StrCmp(NameBuf, TargetName) != 0) {     // StrCmp：字串比較
        continue;                                 // 不相等 -> 不符合
      }
    }

    // ========== GUID 過濾 ==========
    if (FilterByGuid) {
      if (TargetGuid == NULL) {
        continue;
      }
      if (!CompareGuid(&Guid, TargetGuid)) {      // CompareGuid：GUID 完整比對
        continue;
      }
    }

    // 符合條件：印出變數（含 hex dump）
    PrintOneVariableDetailed(NameBuf, &Guid);
    Found++;
  }

  if (OutFound != NULL) {
    *OutFound = Found;
  }

  FreePool(NameBuf);
  return EFI_SUCCESS;
}

//
// =======================================
// List-all 表格模式：只顯示 3 欄
//   Variable Name | Data Size | Vendor GUID
// 並支援翻頁/捲動
// =======================================
//

typedef struct {
  CHAR16  *Name;                                  // 變數名（動態配置）
  EFI_GUID Guid;                                  // Vendor GUID
  UINTN   DataSize;                               // 變數資料大小（bytes）
} VAR_ITEM;

//
// =======================================
// GetVariableDataSizeQuick()
// 教科書層：
//   - 用途：快速取得變數 DataSize（只問大小，不拿資料）
//   - 典型用法：GetVariable(Data=NULL) => EFI_BUFFER_TOO_SMALL 並回 DataSize
// =======================================
//
STATIC EFI_STATUS
GetVariableDataSizeQuick(IN CHAR16 *Name, IN EFI_GUID *Guid, OUT UINTN *OutSize)
{
  EFI_STATUS Status;
  UINTN Size = 0;                                 // IN/OUT：一開始 0，回傳所需大小
  UINT32 Attr = 0;                                // OUT：屬性（此處只是為了符合介面）

  if (OutSize) *OutSize = 0;

  Status = gRT->GetVariable(Name, Guid, &Attr, &Size, NULL);

  if (Status == EFI_BUFFER_TOO_SMALL) {           // 最常見：代表 Size 取得成功
    if (OutSize) *OutSize = Size;
    return EFI_SUCCESS;                           // 我們把它視為成功（因為目的只是拿大小）
  }

  if (Status == EFI_SUCCESS) {                    // 有些實作可能允許 Size=0 直接成功
    if (OutSize) *OutSize = Size;
    return EFI_SUCCESS;
  }

  return Status;                                  // 其他狀態：NOT_FOUND / SECURITY_VIOLATION...
}

//
// =======================================
// CollectAllVariables()
// 教科書層：
//   - 用途：把系統所有變數收集到一個動態陣列（VAR_ITEM[]）
//   - 核心：GetNextVariableName() 逐筆列舉
//   - 記憶體策略：
//       - Items 動態擴容（128 -> 256 -> 512...）
//       - Name 用 AllocateCopyPool(StrSize(NameBuf)) 複製一份保存
// =======================================
//
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

    // 若 Items 空間不夠，擴容
    if (Count >= Cap) {
      UINTN NewCap = (Cap == 0) ? 128 : (Cap * 2);
      VAR_ITEM *NewItems = (VAR_ITEM *)AllocateZeroPool(sizeof(VAR_ITEM) * NewCap);
      if (NewItems == NULL) {
        FreePool(NameBuf);
        if (Items) {
          for (UINTN k = 0; k < Count; k++) {
            if (Items[k].Name) FreePool(Items[k].Name);
          }
          FreePool(Items);
        }
        return EFI_OUT_OF_RESOURCES;
      }

      if (Items) {
        CopyMem(NewItems, Items, sizeof(VAR_ITEM) * Count); // 搬移舊資料
        FreePool(Items);
      }
      Items = NewItems;
      Cap = NewCap;
    }

    // 複製 NameBuf（注意：NameBuf 會被下一次 GetNextVariableName 覆蓋）
    Items[Count].Name = AllocateCopyPool(StrSize(NameBuf), NameBuf);
    if (Items[Count].Name == NULL) {
      FreePool(NameBuf);
      for (UINTN k = 0; k < Count; k++) { if (Items[k].Name) FreePool(Items[k].Name); }
      FreePool(Items);
      return EFI_OUT_OF_RESOURCES;
    }

    // 保存 GUID
    CopyMem(&Items[Count].Guid, &Guid, sizeof(EFI_GUID));

    // 取得資料大小
    GetVariableDataSizeQuick(NameBuf, &Guid, &Items[Count].DataSize);

    Count++;
  }

  FreePool(NameBuf);

  if (OutItems) *OutItems = Items;
  if (OutCount) *OutCount = Count;

  return EFI_SUCCESS;
}

//
// =======================================
// FreeAllVariables()
// 教科書層：
//   - 用途：釋放 CollectAllVariables() 配置的所有資源
// =======================================
//
STATIC VOID
FreeAllVariables(VAR_ITEM *Items, UINTN Count)
{
  if (Items == NULL) return;
  for (UINTN i = 0; i < Count; i++) {
    if (Items[i].Name) FreePool(Items[i].Name);
  }
  FreePool(Items);
}

//
// =======================================
// DrawListAllTable()
// 教科書層：
//   - 用途：畫出表格 UI（含 header、rows、footer）
//   - Top：目前畫面第一列對應的資料 index
//   - Sel：目前選取列對應的資料 index
//   - PageRows：畫面一次能顯示幾列資料
// =======================================
//
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

  // header：反白顯示
  SetTextAttr(EFI_WHITE | EFI_BACKGROUND_BLUE);
  Print(L"Variable Name                         | Data Size | Vendor GUID\n");
  SetTextAttr(EFI_LIGHTGRAY);

  // rows：逐列印出
  for (UINTN r = 0; r < PageRows; r++) {
    UINTN idx = Top + r;                          // idx：資料陣列 index

    if (idx >= Count) {                           // 如果超出資料總筆數
      Print(L"\n");                               // 印空行保持 UI 完整
      continue;
    }

    // 反白：目前選取列
    if (idx == Sel) {
      SetTextAttr(EFI_WHITE | EFI_BACKGROUND_BLUE);
    } else {
      SetTextAttr(EFI_LIGHTGRAY);
    }

    // Name 欄位：截斷成 35 字元（避免超出欄寬）
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

  // footer：顯示總數、頁數、顯示範圍
  UINTN Page = (Count == 0) ? 0 : (Sel / PageRows) + 1;
  UINTN PageCount = (Count == 0) ? 0 : ((Count + PageRows - 1) / PageRows);

  UINTN ShowStart = (Count == 0) ? 0 : (Top + 1);
  UINTN ShowEnd   = (Count == 0) ? 0 : ((Top + PageRows) > Count ? Count : (Top + PageRows));

  Print(L"\nTotal: %u   Page: %u/%u   Showing: %u-%u\n",
        (UINT32)Count, (UINT32)Page, (UINT32)PageCount, (UINT32)ShowStart, (UINT32)ShowEnd);
  Print(L"Keys: Up/Down  PgUp/PgDn  Home/End  ESC exit\n");
}

//
// =======================================
// DoListAll()
// 教科書層：
//   - 用途：
//       1) CollectAllVariables() 收集資料
//       2) 計算 PageRows（依螢幕高度）
//       3) 進入事件迴圈，讀鍵盤控制 Sel/Top
//   - 支援按鍵：
//       Up/Down / PageUp/PageDown / Home/End / ESC
// =======================================
//
STATIC VOID
DoListAll(VOID)
{
  EFI_STATUS Status;
  VAR_ITEM *Items = NULL;
  UINTN Count = 0;

  UINTN Cols = 0, Rows = 0;
  UINTN PageRows = 15;                            // fallback 預設可顯示列數
  UINTN Top = 0;                                  // 畫面第一列對應的 index
  UINTN Sel = 0;                                  // 選取列對應的 index

  Status = CollectAllVariables(&Items, &Count);
  if (EFI_ERROR(Status)) {
    SetTextAttr(EFI_LIGHTRED);
    Print(L"Collect variables failed: %r\n", Status);
    SetTextAttr(EFI_LIGHTGRAY);
    WaitAnyKey();
    return;
  }

  // 嘗試依螢幕高度估算 PageRows
  if (!EFI_ERROR(GetConsoleSize(&Cols, &Rows)) && Rows > 10) {
    UINTN usable = Rows > 9 ? (Rows - 9) : 10;     // 扣掉 header/footer 的保留空間
    PageRows = (usable < 5) ? 5 : usable;          // 最少 5 行避免太小不好用
  }

  while (TRUE) {
    // 保證 Sel 永遠在 Top..Top+PageRows-1 可視範圍內
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

//
// =======================================
// DoSearchByName()
// 教科書層：
//   - 用途：輸入變數名，跨所有 GUID 搜尋並印出詳細資料（含 hex dump）
//   - 注意：UEFI Variable 名稱是 CHAR16（Unicode），但此工具用 ReadLine 限制可印字元
// =======================================
//
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

//
// =======================================
// PromptVendorGuidMasked()
// 教科書層：
//   - 用途：統一「Vendor GUID mask 輸入」流程（給 Search/Create/Delete 共用）
//   - 行為：
//       - 使用 ReadGuidMaskedLine() 顯示 mask 並輸入
//       - 若完全沒輸入（OutStr == ""）=> OutGuid = DefaultGuid
//       - 若未填滿就 Enter => 提示錯誤並回 EFI_INVALID_PARAMETER
//       - 若填滿但格式不合法 => ParseGuidString 失敗提示
// =======================================
//
STATIC EFI_STATUS
PromptVendorGuidMasked(IN EFI_GUID *DefaultGuid, OUT EFI_GUID *OutGuid)
{
  CHAR16 GuidMaskStr[64];                         // 緩衝（>=37）
  EFI_STATUS Status;

  if (DefaultGuid == NULL || OutGuid == NULL) return EFI_INVALID_PARAMETER;

  Print(L"Vendor GUID (leave empty to use default): ");

  Status = ReadGuidMaskedLine(GuidMaskStr, sizeof(GuidMaskStr)/sizeof(GuidMaskStr[0]));
  if (Status == EFI_NOT_READY) {                  // 代表有輸入但未填滿
    SetTextAttr(EFI_LIGHTRED);
    Print(L"GUID not complete. Please fill all hex digits.\n");
    SetTextAttr(EFI_LIGHTGRAY);
    return EFI_INVALID_PARAMETER;
  }
  if (EFI_ERROR(Status)) return Status;

  // 完全沒輸入 => 使用預設 GUID
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

//
// =======================================
// DoSearchByGuid()
// 教科書層：
//   - 用途：
//       1) 顯示 "Search variables by vendor GUID"
//       2) 呼叫 PromptVendorGuidMasked() 取得 GUID
//       3) 用 ListOrFilterVariablesDetailed(FilterByGuid=TRUE) 逐筆印出（含 hex dump）
// =======================================
//
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

//
// =======================================
// DoDeleteVariable()
// 教科書層：
//   - 用途：刪除變數
//   - UEFI 規則：刪除變數的方法是 SetVariable(Name, Guid, 0, 0, NULL)
//     也就是：Attributes=0、DataSize=0、Data=NULL
//   - 常見回傳：
//       EFI_SUCCESS
//       EFI_NOT_FOUND
//       EFI_SECURITY_VIOLATION
//       EFI_WRITE_PROTECTED
//       EFI_OUT_OF_RESOURCES
// =======================================
//
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

  Status = gRT->SetVariable(
    Name,                                         // IN VariableName
    &Guid,                                        // IN VendorGuid
    0,                                            // IN Attributes = 0 (刪除語意)
    0,                                            // IN DataSize = 0
    NULL                                          // IN Data = NULL
  );

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

//
// =======================================
// DoCreateVariable()
// 教科書層：
//   - 用途：建立/設定變數（把使用者輸入的 Value 當作 CHAR16 字串存入）
//   - SetVariable 參數：
//       Name, Guid,
//       Attr：NV | BS | RT（常見三合一）
//       DataSize：bytes（(StrLen+1)*sizeof(CHAR16)）
//       Data：指向 Value buffer
//   - 常見回傳：
//       EFI_SUCCESS / EFI_INVALID_PARAMETER / EFI_OUT_OF_RESOURCES
//       EFI_WRITE_PROTECTED / EFI_SECURITY_VIOLATION
// =======================================
//
STATIC VOID
DoCreateVariable(VOID)
{
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

  Status = gRT->SetVariable(
    Name,                                         // IN VariableName
    &Guid,                                        // IN VendorGuid
    Attr,                                         // IN Attributes
    (StrLen(Value) + 1) * sizeof(CHAR16),         // IN DataSize：含 '\0'
    Value                                         // IN Data：指向 UTF-16 字串
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

//
// =======================================
// ShowMenu()
// 教科書層：
//   - 用途：畫主選單（用顏色反白目前選擇 Sel）
// =======================================
//
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

//
// =======================================
// UefiMain()
// 教科書層：
//   - UEFI Application 入口點
//   - 事件迴圈：
//       1) ShowMenu(Sel)
//       2) ReadKeyStroke 等待按鍵
//       3) Up/Down 改 Sel
//       4) Enter 執行對應功能
// =======================================
//
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
