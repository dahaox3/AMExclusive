# Output device selector

## 功能

- 在 Apple Music 的 `VolumeButton` 左側加一個「輸出裝置」按鈕，flyout 內容為：
  - `Default`：跟隨 Windows 預設裝置（共享模式）
  - 每個 active render endpoint（共享模式）
  - `[Exclusive] <裝置>`：指定裝置並開啟獨占模式
- 選定的裝置在下一首歌開始播放時生效；裝置不存在時改用 Windows 預設裝置。
- 獨占模式開關不會跨 broker 重啟保存，每次啟動都是關閉狀態。

## 實作

| 檔案 | 所在 process | 內容 |
|---|---|---|
| `OutputDeviceUi.cpp` | `AppleMusic.exe`（`am-exclusive-ui.dll`） | 每 1.5 秒檢查 `MainWindow` / `MiniPlayerWindow`，按鈕不在就重新插入；以 `Role::Controller` 連線 broker，送出 `SetEnabled`，並讀取 status 來顯示勾選狀態 |
| `EndpointRedirect.cpp` | `AMPLibraryAgent.exe`（`am-exclusive-hook.dll`） | 替換 `IMMDeviceEnumerator` vtable 的 `GetDefaultAudioEndpoint` slot，把 Apple 取得的預設 render endpoint 換成選定裝置 |
| `SelectedEndpoint.h` | 兩者共用 | 透過 `Local\AMExclusive.OutputDevice.SelectedEndpoint.<SID>` named shared memory 傳遞選定的 endpoint ID |
| `OutputDeviceLog.h` | 兩者共用 | 寫入 DLL 旁的 `am-exclusive-output-device-ui.log` / `am-exclusive-output-device-hook.log` |
| `CMakeLists.txt` | — | 把上述 source 加進 `am_exclusive_ui` / `am_exclusive_hook`，並為 UI link `uuid` |

兩個 DLL 都在 static initializer 中建立 thread 啟動。

## 修改的既有檔案

- `mod/CMakeLists.txt`：結尾 `include` 本目錄的 `CMakeLists.txt`
- `mod/src/Broker.cpp`：`wmain` 初始化後，將 `enabledIntent` 設為 `0`、`state` 設為 `Off`
