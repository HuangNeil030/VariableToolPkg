# VariableToolPkg
VariableTool README（筆記版）
1. 這支工具在做什麼

VariableTool 是一個 UEFI Shell Application，用來操作 UEFI NVRAM 變數（UEFI Variables）：

List all variables（表格）：只列出
Variable Name | Data Size | Vendor GUID（可翻頁/捲動）

Search by name（詳細輸出）：輸入變數名稱，搜尋所有 GUID，印出變數內容（hex dump）

Search by vendor GUID（mask 輸入）：輸入 GUID（用 ________-____-.... mask），找同 GUID 的所有變數（印詳細）

Create new variable：建立變數（以 CHAR16 字串儲存 value）

Delete variable：刪除指定變數（Name + Vendor GUID）

核心概念：UEFI Variable Services 都在 Runtime Services (gRT)

2. 依賴協定/全域表
| 物件            | 來源                            | 用途                                  |
| ------------- | ----------------------------- | ----------------------------------- |
| `gRT`         | `UefiRuntimeServicesTableLib` | Get/Set/GetNextVariableName（變數操作核心） |
| `gBS`         | `UefiBootServicesTableLib`    | Stall（UI 輪詢鍵盤時避免忙等）、Allocate 等      |
| `gST->ConIn`  | System Table                  | 鍵盤輸入 `ReadKeyStroke()`              |
| `gST->ConOut` | System Table                  | 清畫面、設定顏色、游標定位、QueryMode 取欄列數        |

3. 功能與流程總覽
3.1 List all variables（表格模式）

目標： 快速列出所有變數，顯示三欄，不做 hex dump。

流程：

CollectAllVariables()

用 GetNextVariableName() 一筆一筆列舉

每筆用 GetVariable(Data=NULL) 快速拿 DataSize

保存到 VAR_ITEM[]（Name/GUID/DataSize）

DoListAll() 進入 UI 事件迴圈

Up/Down / PgUp/PgDn / Home/End / Esc

DrawListAllTable() 每次重畫畫面

關鍵 API：

gRT->GetNextVariableName()

gRT->GetVariable()（只問大小）

gST->ConIn->ReadKeyStroke()

gST->ConOut->ClearScreen() / SetCursorPosition() / QueryMode()

3.2 Search variables by name（詳細模式）

目標： 輸入變數名稱（Name），跨 GUID 搜尋，印詳細（含 Data hex dump）。

流程：

ReadLine() 讀取 Name

ListOrFilterVariablesDetailed(FilterByName=TRUE)

每個符合者呼叫 PrintOneVariableDetailed()

關鍵 API：

gRT->GetNextVariableName() 列舉所有變數

StrCmp() 比對名稱

gRT->GetVariable() 兩段式拿資料（先問大小再取資料）

3.3 Search variables by vendor GUID（mask 輸入 + 詳細模式）

目標： 進入畫面後顯示 GUID mask：
________-____-____-____-____________
使用者輸入 hex 時，從左到右逐格把 _ 替換掉（自動略過 -）。

流程：

PromptVendorGuidMasked()

ReadGuidMaskedLine()：負責 mask UI + 輸入行為

空輸入 => 使用 Default GUID

未填滿就 Enter => 視為錯誤（not complete）

填滿 => ParseGuidString() 轉 EFI_GUID

ListOrFilterVariablesDetailed(FilterByGuid=TRUE) 列舉並過濾 GUID

PrintOneVariableDetailed() 印出內容

3.4 Create new variable（Name + Vendor GUID mask + Value）

目標： 建立/更新變數。

流程：

ReadLine() 輸入 Name

PromptVendorGuidMasked() 輸入/使用 Default GUID（mask 行為一致）

ReadLine() 輸入 Value（本工具以 CHAR16 字串存入）

gRT->SetVariable() 寫入

屬性 Attributes：

EFI_VARIABLE_NON_VOLATILE

EFI_VARIABLE_BOOTSERVICE_ACCESS

EFI_VARIABLE_RUNTIME_ACCESS

3.5 Delete variable（Name + Vendor GUID mask）

目標： 刪除變數（UEFI 標準刪除語意）。

刪除規則（很重要）：
呼叫 SetVariable() 並且：

Attributes = 0

DataSize = 0

Data = NULL

4. 核心 UEFI API 使用方法（必背）
4.1 GetNextVariableName()：列舉所有變數

用途： 取得下一個 Variable Name + Vendor GUID（像 iterator）。
EFI_STATUS
(EFIAPI *GET_NEXT_VARIABLE_NAME)(
  IN OUT UINTN    *VariableNameSize,
  IN OUT CHAR16   *VariableName,
  IN OUT EFI_GUID *VendorGuid
);
用法要點：

VariableNameSize 是 bytes（不是 CHAR16 數量）

第一次列舉：VariableName = L""，VendorGuid = {0}

回傳：

EFI_SUCCESS：拿到下一筆

EFI_NOT_FOUND：列舉完畢

EFI_BUFFER_TOO_SMALL：buffer 不夠，VariableNameSize 會回報需要大小 → 重新 Allocate 再呼叫

典型 pattern：

allocate buffer

while loop

buffer too small -> reallocate

not found -> break

4.2 GetVariable()：取得變數 Attributes / Data / Size
EFI_STATUS
(EFIAPI *GET_VARIABLE)(
  IN     CHAR16   *VariableName,
  IN     EFI_GUID *VendorGuid,
  OUT    UINT32   *Attributes OPTIONAL,
  IN OUT UINTN    *DataSize,
  OUT    VOID     *Data
);
兩段式拿資料（標準寫法）：

Data=NULL 先問大小

常見回傳：EFI_BUFFER_TOO_SMALL（代表大小已回傳到 DataSize）

Allocate(DataSize) 再呼叫一次取得資料

常見回傳碼：

EFI_NOT_FOUND：變數不存在

EFI_BUFFER_TOO_SMALL：buffer 太小（正常流程會遇到）

EFI_SECURITY_VIOLATION / EFI_WRITE_PROTECTED：受平台安全/鎖定限制

4.3 SetVariable()：建立/更新/刪除變數

EFI_STATUS
(EFIAPI *SET_VARIABLE)(
  IN CHAR16   *VariableName,
  IN EFI_GUID *VendorGuid,
  IN UINT32   Attributes,
  IN UINTN    DataSize,
  IN VOID     *Data
);

建立/更新

Attributes 常用三合一：NV | BS | RT

DataSize 用 bytes

Data 指向資料

刪除（UEFI 標準語意）

Status = gRT->SetVariable(Name, &Guid, 0, 0, NULL);

常見回傳碼：

EFI_SUCCESS

EFI_INVALID_PARAMETER

EFI_OUT_OF_RESOURCES（NVRAM 空間不足）

EFI_WRITE_PROTECTED / EFI_SECURITY_VIOLATION

常見回傳碼：

EFI_SUCCESS

EFI_INVALID_PARAMETER

EFI_OUT_OF_RESOURCES（NVRAM 空間不足）

EFI_WRITE_PROTECTED / EFI_SECURITY_VIOLATION

5. UI / 鍵盤輸入用到的函數
5.1 ReadKeyStroke()：輪詢鍵盤
EFI_STATUS
(EFIAPI *EFI_INPUT_READ_KEY)(
  IN  EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This,
  OUT EFI_INPUT_KEY                  *Key
);
行為：

沒鍵可讀：EFI_NOT_READY

有鍵：EFI_SUCCESS，Key 會帶：

Key.ScanCode（方向鍵、PgUp、Esc…）

Key.UnicodeChar（一般字元、Enter、Backspace）

典型 UI loop：

while(ReadKeyStroke == NOT_READY) Stall(1000)

5.2 QueryMode()：取得畫面欄列數（修你之前 Columns 編譯問題）
EFI_STATUS
(EFIAPI *EFI_TEXT_QUERY_MODE)(
  IN  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
  IN  UINTN                           ModeNumber,
  OUT UINTN                           *Columns,
  OUT UINTN                           *Rows
);
6. GUID mask 輸入（你的重點功能）
6.1 介面行為規格（你要的）

初始顯示：
________-____-____-____-____________

輸入 hex：

從最左開始依序把 _ 替換成你輸入的字元

自動跳過 -

Backspace：

回到上一個 hex 位，把它恢復 _

Enter：

完全沒輸入 → 視為「使用 Default GUID」

有輸入但未填滿 → 視為錯誤（not complete）

6.2 相關函式

ReadGuidMaskedLine()：mask UI + 逐格替換/回退

ParseGuidString()：把完成的字串轉 EFI_GUID

PromptVendorGuidMasked()：統一 Search/Create/Delete 的 GUID 輸入流程

7. 編譯/執行（提醒）

你之前遇到的 Columns 編譯錯誤：
不要用 gST->ConOut->Mode->Columns（UEFI MODE 沒這欄）
改用 QueryMode()。

Variable 讀寫在不同平台差異很大：

OVMF（QEMU）可能比較開放

真機 BIOS 可能很多變數被鎖、Secure Boot 影響、寫入受限
____________________________________________________________
cd /d D:\BIOS\MyWorkSpace\edk2

edksetup.bat Rebuild

chcp 65001

set PYTHONUTF8=1

set PYTHONIOENCODING=utf-8

rmdir /s /q Build\VariableToolPkg

build -p VariableToolPkg\VariableToolPkg.dsc -a X64 -t VS2019 -b DEBUG
