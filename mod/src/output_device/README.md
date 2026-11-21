# Output device selector

## 功能

- 在 Apple Music 的 `VolumeButton` 左側加一個「輸出裝置」按鈕，flyout 內容為：
  - `Default`：跟隨 Windows 預設裝置（共享模式）
  - 每個 active render endpoint（共享模式）
  - `[Exclusive] <裝置>`：指定裝置並開啟獨占模式
  - `[ASIO] <驅動>`：使用已安裝的 64 位元 ASIO 驅動
- 選擇時立即保存裝置和模式。重新開啟 Apple Music 或重啟 Broker 後，會恢復上次選擇。WASAPI 裝置未連接時暫用 Windows 預設輸出，下次開流時再嘗試所選裝置。
- 本 Fork 暫停時釋放實體獨占設備，恢復時重新查詢選定／預設設備；設備忙碌時保留音樂佇列並自動重試。
- 菜單底部提供 Apple 無損音源上限 48 / 96 / 192 kHz，預設 96。選擇上限內最高的現有無損版本，不做本機重採樣；儲存設定後，重新打開歌曲生效。這個設定獨立於獨占模式與相容格式轉換開關。

## 實作

| 檔案 | 所在 process | 內容 |
|---|---|---|
| `OutputDeviceUi.cpp` | `AppleMusic.exe`（`am-exclusive-ui.dll`） | 保存選擇，並以 `OutputSelectionChanged` 通知 Broker。Controller 重連時重新同步最新選擇。 |
| `EndpointRedirect.cpp` | `AMPLibraryAgent.exe`（`am-exclusive-hook.dll`） | 替換 `IMMDeviceEnumerator` vtable 的 `GetDefaultAudioEndpoint` slot，把 Apple 取得的預設 render endpoint 換成選定裝置 |
| `SelectedEndpoint.h` | UI、Broker 和 Agent 共用 | `am-exclusive.ini` 的 `output_selection` 一次保存模式和完整裝置 ID。各處理程序透過依使用者 SID 區分的共享記憶體讀取同一筆選擇。 |
| `OutputDeviceLog.h` | 兩者共用 | 寫入 DLL 旁的 `am-exclusive-output-device-ui.log` / `am-exclusive-output-device-hook.log` |
| `CMakeLists.txt` | — | 把上述 source 加進 `am_exclusive_ui` / `am_exclusive_hook`，並為 UI link `uuid` |

兩個 DLL 都在 static initializer 中建立 thread 啟動。

音源上限使用 IPC 2.3 的 `SetSourceSampleRateLimit`，Broker 寫入
`am-exclusive.ini` 的 `max_source_sample_rate`，並在 status 中回傳。
原 2.x 訊息長度不變，使用原先保留的六個位元組，其中兩個用於確認序號。
收到匹配 UI PID、序號與設定值的確認前，Controller 每秒重送；重連後也
保留尚未確認的設定。舊版 Broker 的菜單項不可操作。

## 修改的既有檔案

- `mod/CMakeLists.txt`：結尾 `include` 本目錄的 `CMakeLists.txt`
- `mod/src/Broker.cpp` 啟動時恢復所選模式及裝置。IPC 2.4 的 Broker 狀態包含所選路徑，暫停或暫時使用其他裝置時仍會保留。
