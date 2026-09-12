# Detailed Implementation and Validation

[← 回到專案首頁](../README.md)

## STM32 W25Q64 Reliable Data Logger

目前已完成 **Phase 8 Benchmark + Diagnostics** 與正式版端到端斷電 Demo，包含真實 TIM4 計時的 4 KiB sequential read、16-page program、sector erase、pattern verify、清理，以及 RAM wear counter CLI。Phase 1～8 的 host、ARMCC 與實體驗收均已完成；實體 benchmark 量得 read 89 KiB/s、write 36 KiB/s、sector erase + verify 90 ms，wear diagnostics、五個 reset-injection points、完整斷電恢復與 sequence 延續亦通過。沒有儀器可量測的硬體項目已改列為有明確邊界的工程限制，不再保留模糊 TODO。

## Repository 檢查結果

| 項目 | 原始專案狀態／本次處理 |
| --- | --- |
| MCU／IDE | STM32F103C8、Keil µVision、ARMCC 5.06 update 5 build 528 |
| Framework | STM32F10x SPL V3.5.0，沿用，不引入 HAL／RTOS |
| main | `User/main.c` 原本只有空的 while loop；現在只負責初始化、poll logger/test 與更新 Watch 變數 |
| SPI／USART／I2C | `Library/` 有 ST SPL peripheral API；新增 SPI1、I2C2 port 與可設定 UART polling port |
| MPU6050 | 原 repository 無 driver；參考使用者的硬體 I2C 範例與接線圖，新增可攜 register driver 與 SPL I2C2 port |
| UART CLI | 原 repository 無 UART 初始化或 printf redirect；新增 USART1 PA9/PA10 port 與不依賴 printf 的 CLI |
| Delay | `System/Delay.c` 每次重設 SysTick、假設 72 MHz，並非連續時間戳 |
| Timer | TIM2 專供 W25 timeout；新增 TIM4 1 kHz interrupt 作 logger uptime，保留原 Delay／SysTick |
| Clock | system code 以 HSE=8 MHz、PLL=72 MHz 設定；UART 115200、runtime SPI clock 與完整 Flash 實測一致，未使用儀器量測實體波形 |
| Include path | 沿用 Start／User／Library／System，新增 User/App、User/Drivers、User/Storage、User/Tests |
| Target define | 原專案缺少 `STM32F10X_MD`，已加到 project.uvprojx |
| Coding style | C99、模組 header、Allman braces；新增碼採 4 spaces、固定寬度整數 |
| 接線 | W25Q64 使用 SPI1 PA4..PA7；MPU6050 使用 I2C2 PB10/PB11，集中定義於 `User/config.h` |
| Git | 收到的資料夾無 `.git`；已新增 `.gitignore`，不含建立 commit 或推送 |

沒有修改 startup、system initialization、CMSIS、SPL、原本 Delay 或 linker 設定。

## 本階段功能與架構

```text
User/main.c                  初始化、main-loop poll、debugger Watch
  |
  +-- User/App/cli.c               bounded parser / incremental record output
  |     +-- User/Drivers/uart_port.h
  |           +-- System/uart_port_stm32.c -> configurable SPL USART
  |     +-- User/App/flash_benchmark.c
  |           +-- checked log sector / TIM4 timing / 4 KiB pattern verification
  |
  +-- User/App/data_logger.c       10 Hz state machine / session cursor
  |     +-- User/Drivers/mpu6050.c -> mpu6050_port.h
  |     |     +-- System/mpu6050_port_stm32.c -> SPL I2C2
  |     +-- System/system_time.c   -> TIM4 1 ms interrupt
  |     +-- User/Storage/wear_leveling.c -> round-robin erase / RAM counters
  |           +-- User/Storage/flash_manager.c -> metadata A/B / recovery
  |
  +-- User/Storage/flash_manager.c
  |     +-- memory map / sector-slot mapping / record codec / CRC validation
  |     +-- User/Storage/crc32.c
  |     +-- User/Drivers/w25q64.c
  |
  +-- User/Tests/*_test.c     明確觸發的硬體測試；預設不編入
  |
  +-- User/Drivers/w25q64.c   NOR command、range check、分頁、timeout、verify
        |
        +-- w25q64_port.h    init / CS / byte transfer / elapsed milliseconds
              |
              +-- System/w25q64_port_stm32.c -> SPL SPI + GPIO + TIM2
              +-- tests/test_w25q64.c       -> 主機端 SPI NOR 模型（二擇一）
```

Driver 不依賴 STM32 header、MPU6050、Data Logger 或 storage memory map。所有 public API 都回傳 `W25Q64_Status_t`；使用 W25Q64 前綴，避免與 SPL 內部 Flash 的 `FLASH_TIMEOUT` enum 衝突。

| API | 行為 |
| --- | --- |
| `W25Q64_Init()` | 初始化 port、等待供電穩定、退出 deep power-down、等待 ready、核對 EF4017 |
| `W25Q64_ReadJEDECID(&id)` | 回傳 24-bit JEDEC ID；不接受 NULL |
| `W25Q64_ReadStatusRegister(1..2, &sr)` | 讀 BV 的 SR1／SR2；SR3 拒絕且不送指令 |
| `W25Q64_WriteEnable()` | 等待 ready，獨立 WREN transaction，確認 WEL |
| `W25Q64_WaitBusy(timeout_ms)` | SR1 BUSY 輪詢；0 為單次檢查，允許最大 3000 ms |
| `W25Q64_Read(addr, data, len)` | 一般 03h read，24-bit address |
| `W25Q64_PageProgram(addr, data, len)` | 1..256 bytes，拒絕跨頁，完成後讀回比對 |
| `W25Q64_Write(addr, data, len)` | 自動在每個 page boundary 切割，逐頁 WREN／program／wait／verify |
| `W25Q64_SectorErase(addr)` | 4 KiB，要求 4096 對齊，完成後驗證整個 sector 為 FF |
| `W25Q64_BlockErase(addr)` | 64 KiB D8h，要求 65536 對齊，完成後驗證整個 block 為 FF |

沒有 Chip Erase API／opcode。所有空指標、zero length、超出 8 MiB、overflow、未對齊 erase 都在送指令前拒絕。Range check 使用 `length <= total - address`，避免加法溢位。資料指標須由呼叫端保證有足夠 buffer。

NOR program 只能將 1 變成 0，回復為 1 需先 erase；driver 不自動擦除，以免覆蓋同 sector 其他資料。呼叫端應 program 已擦除的範圍。讀回不符回傳 `W25Q64_VERIFY_ERROR`，包括保護／鎖定造成 command 被忽略；不會自行修改 protection register。若目標本來就符合要求，verify 成功僅表示最終資料符合要求。

Driver 為單裝置、同步 blocking、不可重入；不要在 ISR 中呼叫，也不要與其他 SPI 使用者並行。未提供 resume／suspend 操作；呼叫前裝置必須未被其他程式暫停擦除。BV Rev E 第 15 頁將 SR2[7:2] 標為 reserved，第 39 頁卻提及 SUS 而未標明位元，故不套用 JV 的 SUS 位元定義，也不宣稱能辨識外部 suspend 狀態。此 driver 本身不送 suspend 指令；接手其他 firmware 操作過的 Flash 時應先完整斷電再上電。多頁寫入沒有 atomicity；任何錯誤都可能已有部分資料寫入。BUSY timeout 不代表 Flash 停止操作，應先確認 ready 再判斷內容，不要盲目重試。SPI fault 會停用 SPI，釋放 CS，須重新初始化；實際 Flash 仍可能接受部分 command。

## Phase 2 Flash Manager

Flash Manager 使用「log sector index + slot index」定位 record。Sector index 0 對應 `0x002000`，slot index 範圍為 0..145。每個 sector 放 146 筆 record，共使用 4088 bytes，sector 尾端 8 bytes 保留為 `0xFF`。這可保證 record 不跨 sector；slot 9 從 sector offset 252 開始，會跨 256-byte page，並由 W25Q64 driver 安全拆成兩次 Page Program。

| API | 行為 |
| --- | --- |
| `FlashManager_Init()` | 要求 W25Q64 已初始化，讀 JEDEC ID；不掃描、不擦寫 |
| `FlashManager_GetSectorAddress()` | 檢查 log sector index 並換算實體地址 |
| `FlashManager_GetRecordAddress()` | 檢查 sector/slot 並換算 record 地址 |
| `FlashManager_SerializeRecord()` | 明確序列化 24-byte payload，計算 CRC，再輸出 28 bytes |
| `FlashManager_DeserializeRecord()` | 區分 erased、magic 錯誤、CRC 錯誤並解析各欄位 |
| `FlashManager_PrepareSector()` | 只允許擦除 log area 中的一個 sector |
| `FlashManager_WriteRecord()` | 先確認整個 slot 為 `0xFF`，再寫入；不自動 erase／overwrite |
| `FlashManager_ReadRecord()` | 讀取、驗證並回傳 RAM record |

Flash 格式不直接寫入 C struct，因此不受 compiler padding、alignment 或 CPU endian 影響。整數都使用 little endian；signed 16-bit 欄位依固定 two-byte bit pattern 序列化，解析時也以明確的 16-bit two's-complement 規則還原。

| Offset | Size | 欄位 | CRC 範圍 |
| ---: | ---: | --- | --- |
| 0 | 4 | magic = `0x31474F4C`，Flash bytes 為 `4C 4F 47 31` (`LOG1`) | 包含 |
| 4 | 4 | sequence | 包含 |
| 8 | 4 | timestamp_ms | 包含 |
| 12 | 2 | accel_x | 包含 |
| 14 | 2 | accel_y | 包含 |
| 16 | 2 | accel_z | 包含 |
| 18 | 2 | gyro_x | 包含 |
| 20 | 2 | gyro_y | 包含 |
| 22 | 2 | gyro_z | 包含 |
| 24 | 4 | CRC-32/ISO-HDLC | 不包含自身 |

CRC 使用 reflected polynomial `0xEDB88320`、initial/final XOR `0xFFFFFFFF`；標準字串 `123456789` 的結果為 `0xCBF43926`。呼叫 `SerializeRecord()` 時，輸入 struct 的 `crc32` 欄位會被忽略並重新計算；成功讀回時才填入實際 stored CRC。

Phase 2 沒有自動 append pointer。呼叫端必須先 `PrepareSector()`，再選擇空白 slot 寫入；對非空白 slot 寫入會回傳 `FLASH_MANAGER_NOT_ERASED`。這避免 reset 後用 RAM pointer 誤寫舊資料。部分寫入可能在下一次讀取時成為 invalid magic 或 CRC error；自動尋找下一筆與復原留給 Phase 6。

## Phase 3 Data Logger

`DataLogger_Process()` 是 main-loop polling state machine。`DataLogger_Start()` 設定第一個 `now + 100 ms` deadline；每次到期最多取得並寫入一筆 sample。若 main loop 因 Flash 或 debugger 延遲，不會連續補寫多筆具有相同即時 sensor 值的假資料，而是由目前時間重新安排下一個 deadline。時間比較使用 unsigned subtraction 加 signed deadline 判斷，可跨 32-bit millisecond wrap。

| API | 行為 |
| --- | --- |
| `DataLogger_Init()` | 執行 metadata／log recovery，初始化並驗證 MPU6050；不 erase、不自動開始 |
| `DataLogger_Clear()` | 停止、以 wear allocator 擦除下一個 log sector，建立空狀態並提交 metadata |
| `DataLogger_Start()` | 從 recovered／cleared state 設定 RUNNING 與下一個 100 ms deadline |
| `DataLogger_Process()` | 到期時 burst-read 六軸、建立 sequence/timestamp/CRC record 並寫入 |
| `DataLogger_Stop()` | 停止新 sample，不修改已寫入 record |
| `DataLogger_GetStatus()` | 回傳 state、last error、record count、next sequence、sector/slot、timestamp |
| `DataLogger_ReadRecord()` | 依目前 boot session 的 zero-based index 讀回並驗證 record |

MPU6050 driver 先核對 `WHO_AM_I=0x68`，再寫入並讀回驗證 PWR_MGMT、sample divider、DLPF、gyro 與 accel 設定。設定沿用參考範例：`SMPLRT_DIV=0x09`、`CONFIG=0x06`、gyro/accel config 均為 `0x18`。每個 sample 從 `ACCEL_XOUT_H` 開始一次讀 14 bytes，丟棄 temperature 欄位，再以明確 big-endian two's-complement 解析六個 `int16_t`。I2C event、BUSY 與 RXNE 都有 10 ms deadline 加 finite spin bound；交易失敗後停用 I2C，logger 保存錯誤並切回 STOPPED，重新初始化前不盲目重試。

## Phase 4 Circular Buffer

Circular Buffer 維護 `current_sector/current_slot`、`oldest_sector/oldest_slot`、`record_count` 與持續增加的 `next_sequence`。每個 sector 寫滿 146 筆後才前進；最後一個 log sector 寫滿時，write pointer 回到 sector 0。當 buffer 已達 298716 筆容量，寫入新 sector 前先 erase 該整個最舊 sector，成功後一次淘汰 146 筆並將 oldest pointer 移到下一個 sector，再逐筆增加 record count。任何 erase 失敗都不會提前移動 oldest pointer。

`record_count` 永遠不超過容量。`DataLogger_ReadRecord(0)` 代表目前最舊 record，`DataLogger_ReadRecord(record_count - 1)` 代表最新 record；內部把 chronological index 映射到可能跨越 Flash 結尾的實體 sector/slot。Status 同時提供下一個 `write_address` 與 `oldest_address`，方便 debugger 與後續 CLI 使用。

Reset 後由 Phase 6 重建 circular pointers，Wear Leveling allocator 也從 recovered write sector 繼續。若壞尾端造成 sector 內部空洞，chronological read 以 sequence 搜尋有效 record；下一次繞回該 sector 時才整體 erase 並淘汰其中仍保留的舊 records。

## Phase 5 Wear Leveling

`wear_leveling.c` 擁有單一 `next_erase_sector`，範圍是 0..2045。Data Logger 要準備新 sector 或執行 `Clear()` 時，必須呼叫 `WearLeveling_PrepareNextSector()`；module 擦除目前 sector，只有 `FlashManager_PrepareSector()` 成功後才增加該 sector counter、增加 total 並將 cursor 移到下一個 sector。這確保 timeout、保護或 verify failure 不會被誤記成有效 erase，也不會造成 allocator 跳號。

同一次開機內重複 `Clear()` 也會選擇下一個 round-robin sector，不會固定磨耗 sector 0。走完 2046 sectors 才會回到起點，所以在理想連續運作下 `maximum_erase_count - minimum_erase_count <= 1`。Phase 4 的 oldest/write invariant 仍負責決定哪些資料被淘汰；Wear Leveling module 只管理實體 erase 順序與診斷統計。

每個 sector 使用一個 `uint32_t` RAM counter，共 8184 bytes；另保存 `uint64_t total_erases` 與 cached summary。完整 min/max scan 只在成功 erase 或 debug simulation 後執行，main-loop 的 `WearLeveling_GetStats()` 是固定時間複製。這些 counters 是 debug-only RAM telemetry，reset 後歸零；Phase 6 持久化 logger 指標與 sequence，但不持久化完整 erase-count array。Round-robin cursor 會從 recovered write sector 繼續。

## Phase 6 Metadata 與 Power-loss Recovery

Metadata A 位於 `0x000000` sector，Metadata B 位於 `0x001000` sector。Flash 內容固定為 36 bytes、little endian，不直接寫 C struct：

| Offset | Size | 欄位 |
| ---: | ---: | --- |
| 0 | 4 | magic，Flash bytes 為 `META` |
| 4 | 4 | format version = 1 |
| 8 | 4 | generation |
| 12 | 4 | write sector |
| 16 | 4 | write offset（bytes） |
| 20 | 4 | oldest sector |
| 24 | 4 | valid record count |
| 28 | 4 | next sequence |
| 32 | 4 | CRC-32，涵蓋前 32 bytes |

`FlashManager_CommitState()` 永遠更新非 active 副本：erase target、寫入下一個 generation、讀回並驗證 CRC／欄位，全部成功後才切換 active copy。舊副本在此之前保持有效。Generation 比較使用 modulo-32-bit ordering，可處理 `0xFFFFFFFF → 0` wrap。

Metadata 不會每筆更新。`DataLogger_Clear()` 是低頻操作，會連續提交兩次相同空狀態，確保 A/B 任一副本都不會退回 clear 前的指標；正常 logging 每寫滿 146 筆、前進到下一個 sector 時才提交一次。這把一般 metadata erase 頻率維持在每個 log sector 一次，同時允許 boot 時從 metadata write pointer 向後重播尚未提交的 records。

`FlashManager_Recover()` 的順序如下：

1. 讀取 A/B，分別檢查 erased、magic、version、欄位範圍與 CRC。
2. 選擇 generation 較新的有效副本；另一份損壞或更新中斷時仍可使用舊副本。
3. 從 metadata write pointer 掃描連續 sequence，納入 sector commit 之後已完整寫入的 records。
4. 遇到 erased slot 就停在該 slot；遇到 invalid magic、CRC error 或非預期 sequence，忽略該筆並在下一個 sector 繼續，因為半寫入 slot 不能安全覆寫。
5. 兩份 metadata 都無效時掃描全部 8 MiB log area，找最高 sequence，反向重建有效序列、oldest、record count 與下一個 write pointer。

Circular buffer 回收最舊 sector 時，會先掃描並計算該 sector 的有效 records，再以另一份 metadata 提交「舊資料已邏輯淘汰」的狀態；只有 metadata 驗證成功後才允許 erase。若在 metadata commit 中斷電，舊副本仍有效且 erase 尚未開始；若在後續 erase 中斷電，新 metadata 已不再引用該 sector。Recovery 使用一個 4096-byte static scan buffer，沒有 malloc 或 recursion。

## Phase 7 UART CLI

CLI 使用固定 80-byte line buffer、最多四個 tokens，不使用 `gets()`、malloc 或 printf。Decimal 與 `0x` hexadecimal parser 會檢查非法字元及 `uint32_t` overflow。`CLI_Process()` 每輪最多接收 16 bytes；`log read`、`readall` 與 `verify` 每個 main-loop iteration 只處理一筆 record，避免一次阻塞整個 logger。

| Command | 行為／限制 |
| --- | --- |
| `help` | 列出指令 |
| `flash id` | 顯示 Winbond、JEDEC ID 與 8 MB capacity |
| `flash status` | 顯示 W25Q64BV SR1／SR2 |
| `flash read <address> <length>` | address 可用十進位或 `0x`；單次限 1..64 bytes 並檢查 8 MiB 邊界 |
| `flash erase <sector>` | sector 是 0..2045 的 log-relative index；logger 必須 STOPPED 且 logical count 必須為 0 |
| `flash benchmark` | logger 必須 STOPPED 且 count=0；量測 4 KiB read/write 與 sector erase，最後擦回空白並驗證 |
| `log start`／`log stop` | 控制 10 Hz logging |
| `log status`／`log count` | 顯示 state、count、sequence、sector／offset 與 last error |
| `log read <count>` | logger 停止時，依 chronological order 輸出最後 N 筆 |
| `log readall` | logger 停止時輸出全部 retained records |
| `log clear` | 建立新的 logical empty session，擦除下一個 wear-level sector，並更新 A/B metadata |
| `log verify` | logger 停止時逐筆驗證 magic／CRC／sequence，分別統計 checked、valid 與各類錯誤 |
| `metadata` | 顯示 active copy、generation、write／oldest、count 與 recovery flags |
| `wear status [start count]` | 顯示 RAM-only erase 統計；預設從 current sector 列 8 個，指定時最多 16 個 |

Raw `flash erase` 永遠只能呼叫 `FlashManager_PrepareSector()` 擦除 log area，無法觸及 `0x000000..0x001FFF` metadata，也沒有 Chip Erase command。為避免破壞 active circular state，有 retained records 或 logger 正在執行時會拒絕 raw erase。`log clear` 是 metadata 所定義的 logical clear，不會耗時擦除全部 2046 sectors；兩份 metadata 同時永久損壞時的 full-scan fallback 仍可能看見尚未被循環覆寫的舊 physical records。

UART hardware port 使用 polling RX 與有界 TX wait。這適合人員操作 CLI；大量貼上資料或在 Flash blocking operation 期間持續傳送可能造成 STM32 USART overrun 並回報 UART error。若未來需要 machine-speed input，可再改成 RX interrupt ring buffer。設定為 115200、8-N-1。

## Phase 8 Benchmark + Diagnostics

`FlashBenchmark_Run()` 只接受 log-relative sector。CLI 額外要求 logger 已停止且沒有 retained record，並選擇 logger 的 current sector，因此不會接觸兩個 metadata sectors，也不會破壞 active log。一次成功 benchmark 執行以下流程：

1. 擦除並驗證一個 4 KiB log sector，同時以 TIM4 millisecond uptime 計時。
2. 產生 deterministic byte pattern，以 16 次 256-byte Page Program 寫滿 4 KiB。
3. 以單次 `W25Q64_Read()` sequential read 4 KiB，再逐 byte 核對 pattern。
4. 再次擦除同一 sector 並驗證為 `0xFF`，讓 empty logger 可立即繼續使用。

Read/write speed 使用實際 4096 bytes 與 elapsed milliseconds 計算 KiB/s；elapsed 為 0 時回報 timer resolution error，不製造虛假的除零結果。SPI clock 由 RCC peripheral clock 與實際 prescaler在 runtime 計算。Read 數字包含 command/address overhead；Page Program 與 erase 數字包含 driver 的 WREN、BUSY polling 與 read-back verify，因此是完整 driver API 的有效效能，不是 datasheet 的純傳輸上限。最後一次 cleanup erase 不計入顯示的 sector erase time。

Benchmark 使用一個 4096-byte static buffer，使整個 read 保持單一 SPI transaction；不配置第二份 pattern buffer，而是以 offset 重新計算預期 byte。正常 Phase 8 image 的 RAM 為 18432/20480 bytes，保留 2048 bytes。每次 benchmark 實際產生兩次 sector erase；`wear status` 明確顯示的是 logger-managed、reset 後歸零的 RAM counters，benchmark cleanup 與 raw diagnostic erase 不灌入 logger allocator 統計。

## Debug-only Reset Injection

`POWER_FAIL_TEST_ENABLE` 預設為 0；此時所有 hooks 編譯成常數 0，正常 image 的 code/RAM size 與加入 hooks 前完全相同。需要實機測試時，只把 `User/config.h` 的這個 macro 改為 1，其他三個 destructive test enable 保持 0，再用正常 Keil target build/download `Objects/project.axf`。`build/keil-fixture/project.axf` 同時含其他測試分支，只供 compile verification，不可燒錄。

在 Debug Watch 設定 `g_power_fail_test_point` 後按 Run；matching hook 會先解除 arm，再呼叫 `NVIC_SystemReset()`。若 Keil 在 reset 後停住，再按一次 Run，不重新 Download。Startup 會把 point 恢復為 0，所以不會形成 reset loop。

| Point | Symbol | Durability boundary | 建議觸發方式 | Reset 後預期 |
| ---: | --- | --- | --- | --- |
| 1 | `POWER_FAIL_POINT_RECORD_PROGRAMMED` | record 已完整 program/verify，RAM count/cursor 尚未增加 | 先 `log clear`，Watch 設 1，Run 後 `log start` | tail replay 找回 sequence 0；count/next sequence 都為 1 |
| 2 | `POWER_FAIL_POINT_METADATA_ERASED` | inactive metadata target 已 erase，尚未 program | 建立 baseline 後 Watch 設 2，再執行 `log clear` | 舊 active copy/generation 仍有效 |
| 3 | `POWER_FAIL_POINT_METADATA_PARTIAL_PROGRAMMED` | target 只 program 前 18/36 bytes，沒有完整 CRC | Watch 設 3，再執行 `log clear` | partial copy 被拒絕，退回舊 copy/generation |
| 4 | `POWER_FAIL_POINT_METADATA_PROGRAMMED` | target 36 bytes 已 program，尚未 read-back/切換 RAM active | Watch 設 4，再執行 `log clear` | reboot 驗證 target CRC，選擇 generation+1 的新 copy |
| 5 | `POWER_FAIL_POINT_LOG_SECTOR_ERASED` | log sector erase/verify 完成，wear counter/cursor 尚未前進 | 建立 empty baseline，Watch 設 5，再執行 `log clear` | 舊 metadata 狀態恢復；孤立 erased sector 不被誤記為新 session |

Point 3 只在該 point 已 armed 時把 metadata program 拆為 18+18 bytes；未 armed 時仍走原本單次 36-byte write。Host model 讓 hook 返回並立刻中止 caller，用來驗證每個 interruption 的 recovery；實機 matching hook 不返回。測試完成後必須把 `POWER_FAIL_TEST_ENABLE` 恢復為 0，重新 build/download 正常 image。

## Timeout 與硬體設定

`User/config.h` 已依使用者提供的「11-2 硬件SPI读写W25Q64.jpg」設定如下，並啟用 `W25Q64_BOARD_CONFIGURED=1`。破壞性測試仍預設關閉。若設回 `0`，`Init()` 回傳 `W25Q64_NOT_CONFIGURED` 且不操作 GPIO。

| W25Q64 模組接腳 | STM32／電源 | 用途 |
| --- | --- | --- |
| CS | PA4 | GPIO output、active low |
| CLK | PA5 | SPI1 SCK |
| DO | PA6 | SPI1 MISO，Flash → MCU |
| DI | PA7 | SPI1 MOSI，MCU → Flash |
| VCC | 3.3 V | 依接線圖供電 |
| GND | GND | 共地 |

| MPU6050 模組接腳 | STM32／電源 | 用途 |
| --- | --- | --- |
| SCL | PB10 | I2C2 SCL，open-drain |
| SDA | PB11 | I2C2 SDA，open-drain |
| VCC | 3.3 V | 依「10-2 硬件I2C读写MPU6050.jpg」供電 |
| GND | GND | 共地 |

MPU6050 使用 7-bit address `0x68` 與 100 kHz I2C。模組或匯流排必須有 SCL/SDA pull-up；接線圖無法證明 pull-up 阻值。PB10/PB11 不占用 W25Q64 的 PA4..PA7，也不占用 SWD。

UART 接線已確認並設為 USART1 default mapping：PA9（STM32 TX）→USB-TTL RX、PA10（STM32 RX）←USB-TTL TX，3.3 V logic 並共地。`UART_BOARD_CONFIGURED=1`，baud rate 115200、8 data bits、no parity、1 stop bit；不啟用 hardware flow control。

SPI1 使用預設映射，明確停用 SPI1 remap；不改 SWD。選用 `/8` 分頻，韌體依 RCC clock tree 在 runtime 回報 PCLK2=72 MHz、SCK=9 MHz。USART1 以 115200 8-N-1 穩定通訊，W25Q64 的 JEDEC ID、program、erase、read-back、CRC、benchmark 與斷電恢復均已在同一塊板上通過；這些結果支持目前 HSE／PCLK2 設定可正常工作。由於沒有示波器或邏輯分析儀，9 MHz 是依 RCC 設定計算的值，不是實體波形量測值。圖中 OLED 不屬於本專案，未新增 OLED driver。

WP#／HOLD# 未出現在六針模組接頭上，無法直接確認內部元件與阻值；完整 program／erase／verify 與 reset-injection 實測證明兩腳在目前模組上保持功能性 deasserted。多輪 benchmark、logging、真正斷電與 reset injection 均未出現資料錯誤，作為目前供電與去耦足以運作的系統級證據；沒有宣稱已量測電源紋波、上升時間或 SPI waveform。這些未量測項目記為硬體限制，不再作為阻擋專案完成的 TODO。

本版 SPL port 支援 SPI1／SPI2、Mode 0、8-bit、MSB first、software NSS。以 RCC peripheral clock 檢查 prescaler，保守限制 SCK 不超過 20 MHz；目前 runtime-calculated SCK 為 9 MHz。

TIM2 是 W25 driver 的短期限計時來源：APB1 timer clock 換算後設為 10 kHz，16-bit counter，累計轉為毫秒；不需 interrupt，也不改 SysTick。Logger 另用 TIM4：timer counter 為 1 MHz、每 1000 ticks 觸發 update interrupt，累加 32-bit milliseconds。既有 `Delay.c` 仍會直接控制 SysTick，Phase 3 不呼叫它，因此不會破壞 TIM4 uptime。

上電等待已設為 20 ms，覆蓋 BV 的 tPUW 最大 10 ms 並保留毫秒量化餘裕。Program／sector／64 KiB block deadline 分別為 10／1000／3000 ms，SPI flag deadline 為 10 ms。另有有限迴圈上限，避免 timer 異常停止時無限等待；上限是故障保護，並非校準的時間來源。既有 `Delay.c` 未改動，也未由 Flash driver 呼叫。

Protocol 與 timing 已改以使用者提供的 **Winbond W25Q64BV，Revision E，July 08, 2010（W25Q64BV.PDF）** 為準：第 17–18 頁 JEDEC／指令表、第 21 頁 SR1／SR2、第 49 頁上電時間、第 53 頁 program／erase 時間。預期 JEDEC ID 為 `EF4017`，僅有 SR1 `05h`、SR2 `35h`，沒有 SR3 `15h`；driver 與 host model 都已移除 `15h`。Page／sector／64 KiB block 最大時間分別為 3／400／1000 ms；sector 的 400 ms 是考量第 53 頁註 5 的較高擦寫次數條件（較低次數為 200 ms）。單憑 JEDEC ID 無法區分 BV／JV，driver 的型號依據是使用者提供的 BV 文件，而不是自動探測型號修訂。

## Build 與靜態驗證

使用 Keil 開啟 `project.uvprojx`、選 `Target 1`，Build／Rebuild。使用既有 ARM Compiler 5 與 STM32F1xx device pack。沒有 HAL、malloc、外部 filesystem 或第三方測試套件依賴。

PowerShell（專案根目錄）：

```powershell
# GCC 主機測試，需 gcc 在 PATH；也可傳 -Compiler 完整路徑
.\tests\run_host_tests.ps1

# Keil 正常專案；可用 -UV4 指定安裝位置
.\tests\build_keil.ps1

# 先正常 build，再驗證啟用 SPL 與 debugger tests 的分支
.\tests\build_keil.ps1 -CompileFixture
```

`-CompileFixture` 使用 `User/config.h` 的正式接腳，由 `tests/compile_board_config.h` **只啟用測試與假設的可清除 sector，僅供編譯**，重新編譯本專案新增的 driver、storage、test 與 main 檔案，再與正常 build 的 startup／SPL objects 及產生的 scatter file 連結。輸出只在 `build/keil-fixture/`，不得燒錄這個 fixture image。正式專案不引用此測試開關檔。它證明 active branch 可以編譯／連結，不證明實體 Flash Manager 測試通過。

## 驗證結果（更新於 2026-09-13）

| 檢查 | 結果 |
| --- | --- |
| GCC C99，Wall／Wextra／Werror／Wconversion／Wshadow／pedantic | 通過，無 warning |
| Phase 1～8 主機模型測試 | 全數通過 |
| Final clean regression | 2026-09-13 刪除 `Objects/`、`Listings/`、`build/` 後，host tests、完整 ARMCC target 與 test-enabled fixture 全部由零重建並通過 |
| JEDEC／SR、初始化失败的寫入阻擋、開機不擦寫 | 通過 |
| NULL／0 length／overflow／容量末端／erase 對齊／跨頁拒絕 | 通過；invalid call 無 SPI |
| 600 bytes 非對齊寫入拆 4 pages、逐次 WREN、NOR 1→0 | 通過 |
| sector／block 邊界，前後相鄰資料保留 | 通過 |
| BUSY 永久為 1、program／erase timeout、timebase 停止／wrap | 通過 |
| WEL 不成立、command 被忽略、讀回 corruption、BV reserved bits | 通過 |
| BV 不支援 SR3／15h | 呼叫 SR3 被拒絕且不產生 SPI；模型拒絕 15h |
| program 路徑所有 21 個 SPI byte 位置注入 timeout | 通過；每條錯誤路徑釋放 CS |
| MCU 重初始化後保留模型資料，讀回 0x12345678 | 通過；不是實體掉電證據 |
| ARMCC 正式 SPI1／BV 組態 | 編譯／連結成功，0 errors、0 warnings |
| ARMCC 啟用 SPL／測試的 compile fixture | 編譯／連結成功，compiler warnings 視為 errors |
| CRC-32 `123456789` | `0xCBF43926`，通過 |
| 28-byte codec | exact bytes、正負 int16、padding-independent round trip 通過 |
| Flash Manager map bounds | 2046 sectors、146 records/sector、298716 total slots，首末地址通過 |
| Phase 2 storage model | metadata 隔離、空白檢查、跨 page、最後 slot、overwrite guard 通過 |
| Phase 2 corruption/error model | CRC/magic/empty 分類、timeout mapping、debug mailbox 通過 |
| MPU6050 model | WHO_AM_I、register write/read-back、signed big-endian decode、14-byte burst、錯誤傳播通過 |
| Data Logger cadence | explicit start、100 ms deadline、late-call skip、32-bit timestamp wrap 通過 |
| Data Logger storage | exact sensor record、sector 0→1 transition、session readback、sensor/Flash fail-stop 通過 |
| Phase 3 debugger mailbox | sensor、clear/start、stop/latest-record CRC verify、invalid command 通過 |
| Phase 2 實體 codec／CRC | 使用者回報 command 1 通過，record CRC = 0x32086A3A |
| Phase 2 實體跨 page record I/O／overwrite guard | 使用者回報 command 2 通過 |
| Phase 2 實體 CRC corruption detection | 使用者回報 command 3 通過，預期 CRC error 被正確辨識 |
| 實體初始化／JEDEC ID | 使用者回報通過：W25Q64_OK、0xEF4017 |
| 實體讀寫／4 KiB Sector Erase／跨頁 | 使用者回報 command 1、2、3 皆通過 |
| 實體寫入完成後斷電、重新上電讀回 | 使用者回報 command 4 → 斷電 → command 5 通過，讀回 0x12345678 |
| Phase 3 實體 MPU6050 與 10 Hz logging | 使用者回報 Watch command 1→2→3 全部符合 |
| Phase 4 circular wrap model | erase-before-overwrite、bounded count、oldest/write pointer、跨尾端 chronological read 通過 |
| Phase 4 實體 circular wrap | 使用者回報 Watch command 4 全部符合 |
| Phase 5 round-robin model | 成功 erase 才計數與推進；timeout 不改 counter/cursor；完整輪次分布通過 |
| Phase 5 RAM distribution | 3 rounds + 17 sectors：min=3、max=4、spread=1、total=6155 |
| Phase 5 實體 debug statistics | 使用者回報 Watch command 5 通過；此命令不擦除 Flash |
| Phase 6 metadata codec／A-B | exact 36 bytes、CRC、alternate commit、舊副本 fallback、generation wrap 通過 |
| Phase 6 recovery model | metadata tail replay、壞 CRC 尾端放棄、gap-aware read、無 metadata 全區掃描通過 |
| Phase 6 sector reclaim ordering | metadata 先提交再 erase；成功路徑與 erase verify failure 後重啟皆通過 |
| Phase 6 ARMCC／實體 metadata recovery | ARMCC 通過；使用者回報 Watch commands 6～9 與實際斷電重啟全部符合 |
| Phase 7 CLI host model | banner、parser、overflow/range、safe erase、log control、read/readall、CRC verify 全部通過 |
| Phase 7 ARMCC 正式 USART1 image | Code=18996、RO=508、RW=212、ZI=14108 bytes；0 errors、0 warnings |
| Phase 7 debugger-test compile fixture | USART1 + destructive test branch：Code=22712、RO=524、RW=316、ZI=14132 bytes；0 errors、0 warnings；不可燒錄 |
| Phase 7 實體 UART CLI | USART1 PA9/PA10、基本命令、10 Hz logger start/stop、count、read、verify、range 與防誤擦全部由使用者回報通過 |
| Phase 8 benchmark host model | 4 KiB pattern、16 pages、read/write/erase 計時、速度、cleanup、安全門檻與 benchmark 後繼續 logging 全部通過 |
| Phase 8 ARMCC 正式 image | Code=22104、RO=528、RW=232、ZI=18200 bytes；RAM=18432/20480 bytes；0 errors、0 warnings |
| Phase 8 + reset-injection compile fixture | Code=26008、RO=544、RW=344、ZI=18232 bytes；0 errors、0 warnings；不可燒錄 |
| Phase 8 實體 benchmark | sector 7／`0x009000`、SPI 9 MHz；read 4 KiB=45 ms／89 KiB/s，write 4 KiB=110 ms／36 KiB/s，sector erase + verify=90 ms，cleanup 通過 |
| Phase 8 實體 diagnostics | 使用者回報 `wear status`、benchmark 後重新 logging 與完整 `log verify` 均符合預期 |
| Debug-only reset injection host model | record program、metadata erase、18/36 partial program、完整 program pre-commit、log-sector erase 五個邊界皆觸發並通過 reboot recovery |
| Reset injection Point 1 實體 | 使用者回報 record program/verify 後、RAM cursor 更新前自動 reset；recovery 找回 sequence 0，count/next=1，CRC/sequence/read errors=0 |
| Reset injection Point 2 實體 | 使用者回報 inactive metadata erase 後自動 reset；舊 active copy、generation 與 write sector 保持，empty logger/verify 正常 |
| Reset injection Point 3 實體 | 使用者回報 metadata 僅 program 18/36 bytes 後自動 reset；partial target 被 CRC/format validation 拒絕並正確退回舊副本 |
| Reset injection Point 4 實體 | 使用者回報新 metadata 完整 program 後、RAM active 切換前自動 reset；recovery 選擇 opposite copy、generation+1 與新 write sector |
| Reset injection Point 5 實體 | 使用者回報 log sector erase/verify 後、wear cursor/state 更新前自動 reset；舊 metadata 狀態恢復、RAM wear counters reset，後續 logging/verify 正常 |
| 正式版端到端斷電 Demo | 斷電前 62 records、next sequence 62、最後 sequence 61；MCU/W25Q64 完全斷電至少 5 秒且未重新 Download，重啟恢復相同 62/62/61；重新 logging 的第一筆由 sequence 62 延續，完整 verify 零錯誤 |
| Clock／SPI SCK 硬體證據 | RCC runtime 計算為 PCLK2=72 MHz、SCK=9 MHz；UART 與完整 Flash 功能實測通過；因無儀器，未量測實體 waveform |
| WP#／HOLD#／供電硬體證據 | 六針模組不暴露 WP#/HOLD#；program、erase、verify、benchmark、斷電與五個 reset points 通過，證明目前組裝可用；內部 pull-up 與電源類比特性未量測 |
| 64 KiB Block Erase | API 與故障路徑已有 host model 驗證；logger 正式路徑只使用 4 KiB sector erase，且已完成實體驗收，因此 64 KiB destructive 實測不是 release blocker |
| Read speed／write speed／erase timing | 實體 PuTTY 輸出：89 KiB/s、36 KiB/s、90 ms；整數速度計算與地址映射均符合 |

硬體驗收證據來自使用者在本次開發對話中的逐步操作回報，沒有儀器 waveform／power-integrity 紀錄，也不表示代理直接量測。測試 sector 為 `0x002000..0x002FFF`。Phase 1 斷電後最終回報符合：command=0、done=5、result=W25Q64_OK、value=0x12345678。Phase 2 回報 command 1、2、3 均為 `FLASH_MANAGER_OK`；command 3 結束後 slot 9 故意留下 CRC 錯誤 record。可將 Watch 或 PuTTY 截圖加入 repository 作為展示證據。

Host model 以 command stream 模擬 WREN、BUSY、CS 邊界與 NOR 資料，以 register model 模擬 MPU6050，並以 fake UART 驗證 CLI input/output。它未模擬供電斜率、I2C/SPI/UART 訊號完整性、真實 sensor timing、真實擦寫中斷後的類比 bit pattern 或 STM32 register 故障；Phase 6 commands 8→斷電→9 已補上實體 power-cycle 證據。

第一次使用且兩份 metadata 都是空白時，boot 必須讀完整個 8 MiB log area，SPI1 9 MHz 下可能需要數秒；這是無 metadata 時的安全 fallback。執行一次 `DataLogger_Clear()` 或寫滿一個 sector 產生有效 metadata 後，後續 boot 通常只讀 A/B 與目前 tail sector。

## 燒錄與硬體 Demo

1. 實體接線採 SPI1 PA4..PA7、3.3 V、共地；六針模組的 WP#／HOLD# 已由完整寫入／擦除功能證明保持 inactive。`User/config.h` 已填好並啟用，SPI1 預設映射不占用 SWD PA13／PA14。
2. system code 使用 HSE=8 MHz、PLL=72 MHz；目前 UART 與 Flash runtime 結果一致，無儀器 waveform 數據。使用 ST-Link 接 SWDIO／SWCLK／GND，Keil Options for Target → Debug 選實際 probe、SWD，再確認 Flash Download algorithm 覆蓋 STM32F103C8 的 internal Flash。
3. Build 後使用 Keil Download（F8），僅燒錄正常 `Objects/project.axf`。目前 Create HEX 未開啟；ST-Link 可直接由 Keil 燒錄 AXF。
4. Run 後在 Watch 觀察 `g_flash_init_status == W25Q64_OK`、`g_flash_jedec_id == 0xEF4017`、`g_cli_init_status == CLI_OK`。Serial terminal 設為 115200 8-N-1、no flow control，reset 後應看到 boot banner 與 `> ` prompt。
5. 若要進行 destructive tests，選一個確定可清除的 4 KiB sector，於 config.h 設 `FLASH_TEST_ENABLE=1` 並定義 `FLASH_TEST_SECTOR_ADDR`。例如只有在 **0x2000 sector 已確認可清除** 時才指定該地址。編譯檢查會拒絕前兩個保留 sector、越界或未對齊值。
6. Rebuild、Download、Run；測試開關只加入 mailbox，不會自動 erase。每次在 Watch 設 `g_flash_test_done=0`，再設 `g_flash_test_command` 為下表數值，讓 MCU 執行；等 `done` 等於 command 後查看 `g_flash_test_result`。不要在測試執行中送另一個 command。

| Command | 函式／效果 |
| --- | --- |
| 1 | `FlashTest_ReadWrite()`：擦除測試 sector、寫入、立即比對 |
| 2 | `FlashTest_SectorErase()`：寫入 0，再擦除並驗證整個 sector |
| 3 | `FlashTest_PageBoundary()`：擦除、驗證跨頁拒絕未改資料、測試自動拆頁 |
| 4 | `FlashTest_PowerCyclePrepare()`：擦除、寫入 `12 34 56 78` |
| 5 | `FlashTest_PowerCycleVerify()`：**只讀**並解碼至 `g_flash_test_value` |

真正的 Phase 1 斷電驗收：

1. 執行 command 4，等待 `done=4` 且 `result=W25Q64_OK`。
2. 離開 Debug，在 Options for Target → Utilities 取消 Update Target before Debugging；Debug 頁取消 Load Application at Startup、Run to main()。保持已燒錄程式及對應 AXF 不變。
3. 切斷 **MCU 與外部 Flash** 的供電，確保 ST-Link／其他線路沒有反向供電；再重新上電。進入 Debug，不重新 Download。在 Command Window 用 `LOAD .\Objects\project.axf NOCODE` 載入除錯符號；Run 約 1 秒後 Stop。不執行 command 1／2／3／4。
4. 確认初始化成功，執行 command 5。
5. 必須得到 `done=5`、`result=W25Q64_OK`、`g_flash_test_value=0x12345678`。保存 Watch 截圖或實測紀錄；本次使用者已回報符合上述結果。

這是「寫入完成後斷電仍保存」驗收；不是 program／erase 中途掉電的 recovery 驗收。收尾已恢復 `FLASH_TEST_ENABLE=0` 並保留本次測試地址；重新燒錄正常專案後，測試 mailbox 不再編入。Keil 的除錯載入設定可在驗收結束後依開發需求恢復。

### Phase 7 UART CLI 實體驗收

只燒錄正常的 `Objects/project.axf`；`tests/` 的 compile fixture 含破壞性測試分支，不可燒錄。USB-TTL terminal 設為 115200 baud、8 data bits、no parity、1 stop bit、no flow control。開啟 terminal 後按一次 STM32 Reset，應看到：

```text
STM32 W25Q64 Data Logger
Manufacturer: Winbond
JEDEC ID: 0xEF4017
Capacity: 8388608 bytes
...
>
```

若 terminal 支援 line ending，選 CRLF；CLI 同時接受 CR、LF 與 CRLF。先執行只讀命令：

```text
help
flash id
flash status
metadata
log status
```

預期 `flash id` 顯示 `0xEF4017` 與 8388608 bytes；`flash status` 能讀出 SR1/SR2；其他命令回到 `> ` prompt，且不回報 `ERROR`。接著執行完整 logger 驗收；`log clear` 會擦除一個 log sector 並重寫兩份 metadata，因此先確認既有 log 不需保留：

```text
log clear
log start
log status
```

讓程式保持 Run 約 2 秒，再輸入：

```text
log stop
log count
log read 5
log verify
```

預期 count 約每秒增加 10；`log read 5` 依 chronological order 顯示最多五筆 sensor records；`log verify` 的 checked 與 valid 應等於 count，CRC／sequence／read errors 都是 0。最後檢查命令保護：

```text
flash read 0x000000 16
flash read 0x7FFFFF 2
flash erase 0
flash erase 2046
```

第一個 read 應輸出 16 bytes；第二個 read 因超過 Flash 尾端而回報範圍錯誤；sector 0 因 retained records 而被拒絕；sector 2046 超過 log-relative 範圍而被拒絕。以上 Phase 7 流程已由使用者回報全部通過。

### Phase 8 Benchmark 實體驗收

重新 build/download 正常 `Objects/project.axf`，reset 後先用 `help` 確認已列出 `flash benchmark` 與 `wear status`。Benchmark 會破壞目前 log，因此 CLI 只在 STOPPED 且 count=0 時允許執行；先建立新的空 session：

```text
log stop
log clear
flash benchmark
```

成功輸出格式如下，時間與速度應保存為實機測試結果：

```text
W25Q64 Benchmark
Log sector: <0..2045>
Address: 0x<log-area address>
SPI clock: 9000000 Hz
Read 4 KiB:
Time: <measured> ms
Speed: <measured> KiB/s
Write 4 KiB:
Time: <measured> ms
Speed: <measured> KiB/s
Sector erase + verify:
Time: <measured> ms
Cleanup: erased and verified
```

接著輸入 `wear status`，應看到 RAM-only summary、current/oldest/next sector，以及從 current sector 開始的八個 erase counters。可用 `wear status 2040 16` 指定最多 16 個 sectors，列表會跨 2045 後回到 0。最後驗證 benchmark cleanup 後仍能 logging：

```text
log start
```

等待約一秒，再輸入：

```text
log stop
log verify
```

預期 `Records checked` 與 `Valid` 相同，`CRC errors`、`Sequence errors`、`Read errors` 均為 0。

### 正式版端到端斷電 Demo

本流程使用正式 `Objects/project.axf` 與 UART CLI，不啟用 debugger mailbox，也不在斷電後重新 Download。先建立可辨識的短紀錄：

```text
log stop
log clear
log start
```

保持 logging 約 3～5 秒，再輸入：

```text
log stop
log status
log read 5
log verify
metadata
```

記下 `log status` 的 `Records=N` 與 `Next sequence=Q`，並保存畫面。最後一筆 sequence 應為 `Q-1`，verify 必須是 checked=valid=N 且三種 errors 都是 0。短測試尚未填滿 146-record sector 時，`metadata` 可能仍顯示 clear 時的 0-record durable checkpoint；這是 sector-level commit 策略的預期結果，重開機會從 checkpoint replay 完整 tail records。

關閉 PuTTY，不使用 Keil Download。拔除 ST-Link、USB-TTL 與其他可能由 GPIO／3.3 V 反向供電的連線，再切斷 MCU 與 W25Q64 共用電源至少五秒。重新供電後接回 USB-TTL、開啟 PuTTY，按一次 Reset 以取得完整 banner。輸入：

```text
log status
log read 5
log verify
metadata
```

開機 banner 的 `Records found` 與 `log status` 必須恢復為 N，`Next sequence` 必須仍為 Q，最後一筆 sequence 仍為 `Q-1`，CRC／sequence／read errors 都是 0。`metadata` 的 recovered view 應反映 replay 後的 N/Q。

不要再執行 `log clear`。繼續驗證 sequence 延續：

```text
log start
```

等待約一秒後：

```text
log stop
log status
log read 15
log verify
```

新的第一筆 sequence 必須從 Q 開始；新的 `Records` 與 `Next sequence` 都應增加相同筆數，verify 仍須零錯誤。Timestamp 是 MCU boot uptime，因此重開機後從較小值重新開始屬於目前格式的正常設計，不能用 timestamp 是否延續判斷 recovery。

本流程已由使用者以正式 image 完成。斷電前基準為 `Records=62`、`Next sequence=62`、最後 sequence `61`，且 checked/valid 均為 62、三類 errors 均為 0。MCU 與 W25Q64 完全斷電至少五秒、未重新 Download；重新上電後恢復相同的 62 筆 records 與 next sequence 62。再次開始 logging 時，第一筆新 record 從 sequence 62 延續，record count/next sequence 正常增加，完整 verify 仍為零錯誤。

### Phase 2 實體驗收

目前 `FLASH_MANAGER_TEST_ENABLE=0`。正常版本 Run 後可在 Watch 確認 `g_flash_manager_init_status` 為 `FLASH_MANAGER_OK`（數值 0）；這個初始化只讀 JEDEC ID，不擦寫 Flash。以下流程已由使用者完成並回報通過，保留作為重現步驟。

若 `0x002000..0x002FFF` 可以再次清除，在 `User/config.h` 將 `FLASH_MANAGER_TEST_ENABLE` 改為 1，保持 `FLASH_MANAGER_TEST_SECTOR_INDEX=0`；`FLASH_TEST_ENABLE` 維持 0。Rebuild、Download、Run 後加入：

```text
g_flash_manager_test_command
g_flash_manager_test_done
g_flash_manager_test_result
g_flash_manager_test_value
```

每次 Stop 後先設 `done=0`、再設定 command，Run 約 2 秒後 Stop：

| Command | 實際檢查 | 是否擦除 sector 0 |
| ---: | --- | --- |
| 1 | CRC32 known vector、serialize/deserialize round trip | 否 |
| 2 | 擦除、在 slot 9 寫讀跨 page record、拒絕第二次覆寫 | 是 |
| 3 | 擦除、寫入 record、合法清除一個資料 bit、確認讀取回報 CRC error | 是 |

每項預期 `command=0`、`done` 等於送出的 command、`result=FLASH_MANAGER_OK`（0）。Command 3 的 OK 表示測試成功偵測到內部預期的 `FLASH_MANAGER_CRC_ERROR`。Command 1/2 的 `value` 是本次 record CRC；本次實測為 `0x32086A3A`。三項均已通過，收尾已將 `FLASH_MANAGER_TEST_ENABLE` 恢復為 0。

### Data Logger 實體驗收（Phase 3～6）

確認 MPU6050 的 PB10/PB11 與 3.3 V 接線。Phase 3～6 實體驗收已完成，正式設定已恢復 `DATA_LOGGER_TEST_ENABLE=0`；若要重現以下 mailbox 流程，暫時改為 1，且 Phase 1/2 測試維持 0。Rebuild、Download、Run；先確認以下 boot diagnostics：

```text
g_system_time_init_status     = SYSTEM_TIME_OK (0)
g_flash_init_status           = W25Q64_OK (0)
g_flash_jedec_id              = 0xEF4017
g_flash_manager_init_status   = FLASH_MANAGER_OK (0)
g_data_logger_init_status     = LOGGER_OK (0)
g_mpu6050_device_id           = 0x68
```

加入 `g_data_logger_test_command`、`g_data_logger_test_done`、`g_data_logger_test_result`、`g_data_logger_test_value` 及六個 `g_data_logger_test_accel_*`／`gyro_*`。每次先設 `done=0`、再送 command。Commands 1～3 已由使用者回報通過：

| Command | 實際動作 | 預期 |
| ---: | --- | --- |
| 1 | 讀 WHO_AM_I 與一筆六軸 sample，不碰 Flash | done=1、result=LOGGER_OK、device_id=0x68；移動模組時 raw value 有變化 |
| 2 | `Clear()` 擦除 log sector 0 並 `Start()` | done=2、result=LOGGER_OK；保持 Run，record_count 約每秒增加 10 |
| 3 | `Stop()` 並讀回最後一筆 record，包含 Flash Manager CRC 驗證 | done=3、result=LOGGER_OK、value=本次筆數，record sequence=value-1 |
| 4 | 快速建立接近容量上限的測試狀態，跨最後 sector 並覆寫 sector 0 | done=4、result=LOGGER_OK、value=298571，詳細 pointer 如下 |
| 5 | RAM-only 模擬三個完整 erase rounds 再前進 17 sectors | done=5、result=LOGGER_OK、value=spread=1；不操作 Flash |
| 6 | 建立 A generation 1／B generation 2，破壞 B CRC，再 recovery | done=6、result=LOGGER_OK；選回 A generation 1 |
| 7 | metadata 後寫 3 筆有效 record 與 1 筆壞 CRC，再立即 recovery | done=7、result=LOGGER_OK；保留 3 筆並將 write pointer 移到 sector 1 |
| 8 | 建立與 command 7 相同的持久資料，但不執行 recovery | done=8、result=LOGGER_OK；接著實際斷電 |
| 9 | 重新上電後只讀 recovery 並核對狀態 | done=9、result=LOGGER_OK；不得先重新 Download |

Command 2 會清除 `0x002000..0x002FFF`。Command 3 要在 command 2 Run 至少約一秒後執行；若尚未產生 record，預期回報 `LOGGER_NOT_READY`。

Phase 4 command 4 會擦除 log sector 0、1、2045，也就是實體地址 `0x002000..0x003FFF` 與 `0x7FF000..0x7FFFFF`，然後同步寫入測試 record 並驗證 wrap。Run 最多約 5 秒後 Stop，預期：

```text
g_data_logger_test_done       = 4
g_data_logger_test_result     = LOGGER_OK (0)
g_data_logger_test_value      = 298571
g_logger_state                = DATA_LOGGER_STOPPED (0)
g_logger_record_count         = 298571
g_logger_next_sequence        = 298717
g_logger_current_sector       = 0
g_logger_current_slot         = 1
g_logger_oldest_sector        = 1
g_logger_oldest_slot          = 0
g_logger_write_address        = 0x00201C
g_logger_oldest_address       = 0x003000
g_data_logger_test_record.sequence = 298716
```

Command 4 內部還會確認 sector 0 的舊 slot 已因 erase 變回空白、chronological index 0 映射到 sector 1，而最新 index 映射到 wrap 後的 sector 0。可同時觀察 `g_logger_last_timestamp_ms`。Logger 不會在開機自動執行任何 command。

Phase 5 command 5 是 RAM-only 快速驗收。Stop 後先設定：

```text
g_data_logger_test_done = 0
g_wear_test_query_sector = 0
g_data_logger_test_command = 5
```

Run 約一秒再 Stop，預期：

```text
g_data_logger_test_done       = 5
g_data_logger_test_result     = LOGGER_OK (0)
g_data_logger_test_value      = 1
g_wear_total_erases           = 6155
g_wear_minimum_erase_count    = 3
g_wear_maximum_erase_count    = 4
g_wear_erase_count_spread     = 1
g_wear_sectors_at_minimum     = 2029
g_wear_sectors_at_maximum     = 17
g_wear_next_sector            = 17
g_wear_test_query_count       = 4
```

要抽查分布，可在 Stop 時把 `g_wear_test_query_sector` 依序改為 16、17、2045，每次短暫 Run 後再 Stop；預期 query count 分別為 4、3、3。Command 5 會用模擬分布取代本次開機的實際 RAM counters，因此驗收後應 reset/re-download，再進行一般 logging。

Phase 6 commands 6～8 都會先停止 logger，擦除 **Metadata A、Metadata B 與 log sector 0**；command 6 會故意破壞 B 的 CRC，commands 7/8 會故意破壞 sector 0 slot 3。請先確認這三個 sectors 沒有要保留的資料。加入 Watch：

```text
g_metadata_test_active_copy
g_metadata_test_generation
g_metadata_test_write_sector
g_metadata_test_write_slot
g_metadata_test_record_count
g_metadata_test_next_sequence
g_metadata_test_valid
g_metadata_test_used_log_scan
g_metadata_test_has_gaps
g_flash_recovery_status
g_flash_recovery_active_copy
g_flash_recovery_generation
g_flash_recovery_metadata_valid
g_flash_recovery_used_log_scan
g_flash_recovery_has_gaps
```

Command 6 預期 active copy = A（1）、generation = 1、valid = 1、record count = 0。Command 7 預期 active copy = A、generation = 1、write sector = 1、write slot = 0、record count = 3、next sequence = 3、used scan = 1、has gaps = 1。

真正的 Phase 6 power-cycle 驗收：先執行 command 8 並等待 `done=8/result=LOGGER_OK`，再沿用 Phase 1 的方式離開 Debug、禁止重新下載與 startup load，切斷 MCU／Flash 電源後重新上電，以 `LOAD .\Objects\project.axf NOCODE` 載入符號。Run 至 boot 完成後，`g_flash_recovery_*` 應顯示 A、generation 1、metadata valid 1、used scan 1、has gaps 1；logger 應為 record count 3、next sequence 3、current sector 1、slot 0。最後執行 command 9，預期 `done=9/result=LOGGER_OK` 與相同的 `g_metadata_test_*` 值。Command 9 本身只讀，不 erase/program。

## Flash Memory Map 與後續階段

Phase 2 已固定以下 memory map。`LOG_END_ADDR=0x800000` 是 exclusive end；最後一個有效 byte 是 `LOG_LAST_ADDR=0x7FFFFF`。

```text
0x000000..0x000FFF   預留 Metadata A
0x001000..0x001FFF   預留 Metadata B
0x002000..0x7FFFFF   Log Area: 2046 sectors
```

Metadata A/B 已實作 deterministic struct、generation、CRC、交替更新與 boot recovery。Phase 5 的完整 erase-count array 仍是 RAM diagnostics。Phase 7 CLI 與 USART1 port 已完成實體驗收；Phase 8 benchmark、wear diagnostics、五個 reset-injection points、正式版端到端斷電恢復、sequence 延續與完整 CRC verify 也已全部通過。硬體 TODO 已依現有實機功能證據關閉；未量測的 waveform 與 power-integrity 特性保留為文件化限制。

## 檔案清單與面試說明

Phase 7 新增 `User/App/cli.c/.h`、`User/Drivers/uart_port.h` 與 `System/uart_port_stm32.c`。Phase 8 新增 `User/App/flash_benchmark.c/.h`。Debug reset injection 位於 `User/Tests/power_fail_test.c/.h`，hooks 接在 Data Logger、Flash Manager 與 Wear Leveling 的 durability boundaries；並擴充 W25Q64 runtime clock API、`User/config.h`、`project.uvprojx`、build scripts、`tests/test_w25q64.c` 與本 README。主機／build 驗證位於 `tests/`。

`Objects/`、`Listings/`、`build/` 皆為本機產物，不應提交。發布前自行選擇新程式碼授權，保留原 ST／ARM 檔案的 license notice。

可以介紹為：「在既有 STM32 SPL 專案實作可攜 SPI NOR 與 I2C sensor driver，以 TIM4 uptime 驅動 10 Hz logger；sector-based circular buffer 與 round-robin allocator 管理 2046 個 log sectors，雙 metadata 以 generation/CRC 交替提交，boot recovery 能重播未提交尾端並避開半寫入 NOR slots；固定 buffer UART CLI 提供安全診斷、資料匯出與真實 4 KiB read/write/erase benchmark。」Phase 1～8 已完成 host、ARMCC 與實體驗收；正式版 CLI 端到端斷電展示已證明 62 筆 records 可在完全斷電後恢復，且 sequence 能由 62 繼續。
