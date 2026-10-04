/*
 * --------------------------------------------------------------------------------------------------------------------
 * ESP32-S3 RFID + WiFi + RGB LED (雙向溝通版)
 * 邏輯：刷卡 -> 黃燈(等待) -> 接收 Python 結果 -> 綠燈(通過)/紅燈(失敗)
 * --------------------------------------------------------------------------------------------------------------------
 */

#include <SPI.h>
#include <MFRC522.h>
#include <WiFi.h>
#include <FastLED.h>

// --- WiFi 設定（實際值放在 secrets.h，不進版控）---
#include "secrets.h"
const char* ssid     = WIFI_SSID;        // WiFi 名稱
const char* password = WIFI_PASSWORD;    // WiFi 密碼
const char* host     = SERVER_HOST;      // Python 電腦 IPv4 位址
const int   port     = SERVER_PORT;      // Python Port

// --- ESP32-S3 RFID 腳位 ---
#define SCK_PIN  36
#define MISO_PIN 37
#define MOSI_PIN 35
#define SS_PIN   4
#define RST_PIN  6

// --- RGB LED 設定 ---
#define LED_PIN     38    
#define NUM_LEDS    1
#define BRIGHTNESS  50    
#define LED_TYPE    WS2812
#define COLOR_ORDER GRB   

CRGB leds[NUM_LEDS];      
MFRC522 rfid(SS_PIN, RST_PIN); 
byte nuidPICC[4];

void setup() { 
  Serial.begin(115200);
  delay(1000);

  // --- 1. 初始化 LED ---
  FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS);
  FastLED.setBrightness(BRIGHTNESS);
  leds[0] = CRGB::Blue; // 連接 WiFi 時亮藍色
  FastLED.show();

  // --- 2. 連接 WiFi ---
  Serial.println();
  Serial.print("正在連接 WiFi: ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    leds[0] = (leds[0] == CRGB::Blue) ? CRGB::Black : CRGB::Blue;
    FastLED.show();
  }
  
  leds[0] = CRGB::Black; // 連線成功後關燈
  FastLED.show();

  Serial.println("\n[OK] WiFi 已連線");
  Serial.print("ESP32 IP 位址: ");
  Serial.println(WiFi.localIP());

  // --- 3. 初始化 RFID ---
  // 開發板需選 ESP32S3 Dev Module，SPI 腳位才會對應正確
  SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, SS_PIN); 
  rfid.PCD_Init(); 

  // 診斷訊息
  byte v = rfid.PCD_ReadRegister(rfid.VersionReg);
  Serial.print(F("RC522 晶片版本 (HEX): ")); Serial.println(v, HEX);
  if (v == 0x00 || v == 0xFF) {
      Serial.println(F("[ERR] 錯誤：讀不到 RC522，請檢查接線！"));
      leds[0] = CRGB::Red; 
      FastLED.show();
  } else {
      Serial.println(F("[OK] RC522 就緒，請刷卡..."));
  }
  Serial.println(F("-----------------------------"));
}
 
void loop() {
  // 檢查新卡片
  if ( ! rfid.PICC_IsNewCardPresent()) return;
  if ( ! rfid.PICC_ReadCardSerial()) return;
    
  Serial.println(F("偵測到新卡片！"));

  // 轉換 UID
  String uidString = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
     if(rfid.uid.uidByte[i] < 0x10) uidString += "0"; 
     uidString += String(rfid.uid.uidByte[i], HEX);
     if (i < rfid.uid.size - 1) uidString += " "; 
  }
  uidString.toUpperCase(); 

  Serial.print("讀取 UID: ");
  Serial.println(uidString);

  // --- 送出卡號並等待辨識結果 ---
  processVerification(uidString);

  // 停止卡片通訊
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();

  // 休息一下，避免連續讀取
  Serial.println("-----------------------------");
  delay(1000); 
  
  // 關燈，準備下一次
  leds[0] = CRGB::Black; 
  FastLED.show();
}

// --- 核心邏輯：傳送並等待回應 ---
void processVerification(String data) {
  WiFiClient client;
  
  // 1. 嘗試連線 Python Server
  if (!client.connect(host, port)) {
    Serial.println("[ERR] 連線失敗！找不到 Python Server");
    // 連不上網路 -> 紅燈閃爍
    for(int i=0; i<3; i++){
      leds[0] = CRGB::Red; FastLED.show(); delay(200);
      leds[0] = CRGB::Black; FastLED.show(); delay(200);
    }
    return;
  }

  // 2. 傳送 UID
  client.print(data);
  Serial.println("UID 已傳送，等待 Python 辨識...");

  // 狀態：處理中，亮黃燈
  leds[0] = CRGB::Yellow;
  FastLED.show();

  // 3. 等待回應 (設定超時 10秒，因為人臉辨識需要時間)
  unsigned long timeout = millis();
  while (client.available() == 0) {
    if (millis() - timeout > 10000) { 
      Serial.println("[ERR] 等待逾時 (Python 沒反應)");
      client.stop();
      // 超時 -> 亮紅色
      leds[0] = CRGB::Red;
      FastLED.show();
      delay(2000);
      return;
    }
  }

  // 4. 讀取 Python 回傳的結果
  String response = client.readString(); // 讀取所有回傳字串
  response.trim(); // 去除前後空白
  
  Serial.print("收到回應: ");
  Serial.println(response);

  // 5. 判斷燈號
  if (response == "PASS") {
    Serial.println("[OK] 驗證通過！(亮綠燈)");
    leds[0] = CRGB::Green;
  } else {
    // 包含 FAIL, ERROR 或其他
    Serial.println("[ERR] 驗證失敗/錯誤！(亮紅燈)");
    leds[0] = CRGB::Red;
  }
  FastLED.show();

  // 6. 維持燈號 3 秒讓使用者看到
  delay(3000); 
  
  client.stop();
}