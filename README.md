# RFID＋人臉雙因子門禁系統

中央大學「嵌入式系統設計」分組專題（2025，2 人）。刷 RFID 卡後，系統會拍下刷卡者的照片，比對「這張卡登記的人臉」與「現場拍到的人臉」，兩者一致才通過。

## 系統架構

```
[ESP32-S3]──SPI──[MFRC522 RFID 讀卡機]
    │  └──單線──[WS2812 RGB LED]
    │ TCP Socket (port 8080)：送出卡號 → 收到 PASS / FAIL / ERROR
    ▼
[Python 伺服器] ── HTTP GET /capture ──▶ [ESP32-CAM + OV2640]
    ├ face_recognition：卡號對應人臉 1:1 比對
    ├ SQLite：使用者資料、刷卡紀錄
    └ Flask (port 5000)：人員管理、刷卡紀錄與照片查詢
```

### 流程
1. 刷卡 → ESP32-S3 讀到 UID，透過 TCP 傳給伺服器，LED 轉**黃燈**（等待中）
2. 伺服器向 ESP32-CAM 取得一張 JPEG
3. 用 `face_recognition` 比對此卡號登記的人臉與現場照片（tolerance 0.5）
4. 回傳 `PASS` / `FAIL` / `ERROR` → LED 顯示**綠燈**或**紅燈**
5. 每次刷卡都寫入 SQLite 紀錄，並保存現場照片

## 硬體

| 元件 | 介面 | 說明 |
|---|---|---|
| ESP32-S3 | — | 讀卡端主控 |
| MFRC522 | SPI（SCK 36 / MISO 37 / MOSI 35 / SS 4 / RST 6） | 13.56 MHz RFID 讀卡機；開機讀 `VersionReg` 確認接線 |
| WS2812 | GPIO 38 | 狀態燈（藍：連 WiFi、黃：等待、綠：通過、紅：失敗） |
| ESP32-CAM（AI-Thinker） | OV2640：SCCB 設定 + 8-bit 平行影像 | 提供 `/capture` HTTP API |

## 實作重點
- **SPI 周邊驅動與自我檢查**：開機讀取 MFRC522 的版本暫存器，讀到 `0x00` / `0xFF` 代表 SPI 沒接好，亮紅燈提示
- **記憶體配置**：偵測到 PSRAM 時使用較高解析度與雙 frame buffer，沒有則降規格並把 frame buffer 放在 DRAM
- **逾時與錯誤處理**：ESP32 端等待辨識結果 10 秒逾時；伺服器向相機取像 3 秒逾時，相機離線時回傳 `ERROR`
- **開機自我檢查**：伺服器啟動時先確認相機連線狀態

## 已知限制與可改進之處
- ESP32-S3 等待結果時使用阻塞式迴圈與 `delay()`，可改成以 `millis()` 實作的非阻塞狀態機或 FreeRTOS task
- 只用 MIFARE UID 識別卡片，UID 可被複製；Socket 為明文傳輸、管理頁沒有登入驗證
- 伺服器一次只處理一個刷卡連線
- 裝置 IP 需手動設定，可改用 mDNS

## 執行方式

### ESP32
1. 兩個 sketch 資料夾中，把 `secrets.example.h` 複製成 `secrets.h` 並填入 WiFi 與伺服器 IP
2. Arduino IDE 開發板選 **ESP32S3 Dev Module**（讀卡端）與 **AI Thinker ESP32-CAM**（相機端）
3. 讀卡端需安裝函式庫 `MFRC522`、`FastLED`

### Python 伺服器
```bash
cd server
pip install -r requirements.txt
set ESP32_CAM_URL=http://<相機 IP>/capture
python server.py
```
瀏覽 `http://127.0.0.1:5000` 進入管理頁。

> 網頁模板（`templates/`）未包含在此 repo 中。
