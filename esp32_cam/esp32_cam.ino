#include "esp_camera.h"
#include <WiFi.h>
#include "esp_http_server.h" // 引入標準 HTTP Server 庫

// ===========================
// 1. WiFi 設定
// ===========================
// 實際值放在 secrets.h，不進版控
#include "secrets.h"
const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;

// ===========================
// 2. AI Thinker ESP32-CAM 腳位定義
// ===========================
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

// 宣告函式
void startCameraServer();

// ===========================
// 3. Setup 初始化
// ===========================
void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println();

  // --- 相機參數配置 (依照範例的嚴謹寫法) ---
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;       // 使用 20MHz 較穩定
  config.pixel_format = PIXFORMAT_JPEG; // 串流或拍照都用 JPEG
  
  // --- PSRAM 判斷與品質設定 ---
  if(psramFound()){
    // 如果有外部記憶體 (AI-Thinker 通常有 4MB PSRAM)
    config.frame_size = FRAMESIZE_UXGA; // 可以開到 1600x1200
    config.jpeg_quality = 10;           // 品質 (10-63)，越小越好
    config.fb_count = 2;                // 雙緩衝區，速度較快
    config.grab_mode = CAMERA_GRAB_LATEST; 
  } else {
    // 如果沒有 PSRAM，降低規格以防當機
    config.frame_size = FRAMESIZE_SVGA;
    config.jpeg_quality = 12;
    config.fb_count = 1;
    config.fb_location = CAMERA_FB_IN_DRAM;
  }

  // --- 初始化相機 ---
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("❌ 相機初始化失敗，錯誤碼: 0x%x", err);
    return;
  }

  // --- 感光元件微調 (針對 OV2640/OV3660) ---
  sensor_t * s = esp_camera_sensor_get();
  if (s->id.PID == OV3660_PID) {
    s->set_vflip(s, 1);       // 垂直翻轉
    s->set_brightness(s, 1);  // 稍微調亮
    s->set_saturation(s, -2); // 降低飽和度
  }
  // 為了傳輸速度，這裡將預設解析度設為 VGA (640x480)，Python 處理較快
  s->set_framesize(s, FRAMESIZE_VGA); 

  // --- 連接 WiFi ---
  WiFi.begin(ssid, password);
  WiFi.setSleep(false); // 關閉 WiFi 休眠以獲得最佳反應速度

  Serial.print("正在連接 WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  Serial.println("✅ WiFi 已連線");

  // --- 啟動伺服器 ---
  startCameraServer();

  Serial.print("相機準備就緒！Python 請連線到: http://");
  Serial.print(WiFi.localIP());
  Serial.println("/capture");
}

// ===========================
// 4. Loop (什麼都不用做)
// ===========================
void loop() {
  // 伺服器在背景執行，這裡只要讓 CPU 休息即可
  delay(10000);
}

// ===========================
// 5. 伺服器處理邏輯 (取代原本 loop 內的程式)
// ===========================

// 拍照處理函式
esp_err_t capture_handler(httpd_req_t *req) {
    camera_fb_t * fb = NULL;
    esp_err_t res = ESP_OK;
    
    // 1. 取得影像
    fb = esp_camera_fb_get();
    if (!fb) {
        Serial.println("❌ 拍照失敗 (Camera capture failed)");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    
    // 2. 設定 HTTP Header
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    // 3. 發送圖片數據
    // 如果圖片大於一定程度，分段傳送會比較安全，不過這裏用單次傳送通常沒問題
    res = httpd_resp_send(req, (const char *)fb->buf, fb->len);

    // 4. 先記下大小再歸還 frame buffer（歸還後就不能再讀 fb）
    size_t len = fb->len;
    esp_camera_fb_return(fb);

    Serial.printf("📸 照片已傳送 (大小: %u bytes)\n", len);
    return res;
}

// 啟動 Web Server
void startCameraServer() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;

    httpd_handle_t camera_httpd = NULL;

    // 設定 URI 處理器
    httpd_uri_t capture_uri = {
        .uri       = "/capture",
        .method    = HTTP_GET,
        .handler   = capture_handler,
        .user_ctx  = NULL
    };

    Serial.printf("正在啟動 Web Server...\n");
    if (httpd_start(&camera_httpd, &config) == ESP_OK) {
        // 註冊 /capture 路徑
        httpd_register_uri_handler(camera_httpd, &capture_uri);
        Serial.println("✅ 伺服器啟動成功");
    } else {
        Serial.println("❌ 伺服器啟動失敗");
    }
}