#include "esp_camera.h"
#include <WiFi.h>
#include "esp_http_server.h"
#include "FS.h"
#include "SD.h"
#include "SPI.h"

const char* ssid     = "Rak";
const char* password = "@1212@#1";

// ── Camera pins (XIAO ESP32-S3 Sense) ──────────────────────────
#define PWDN_GPIO_NUM   -1
#define RESET_GPIO_NUM  -1
#define XCLK_GPIO_NUM   15
#define SIOD_GPIO_NUM    4
#define SIOC_GPIO_NUM    5
#define Y9_GPIO_NUM     16
#define Y8_GPIO_NUM     17
#define Y7_GPIO_NUM     18
#define Y6_GPIO_NUM     12
#define Y5_GPIO_NUM     10
#define Y4_GPIO_NUM      8
#define Y3_GPIO_NUM      9
#define Y2_GPIO_NUM     11
#define VSYNC_GPIO_NUM   6
#define HREF_GPIO_NUM    7
#define PCLK_GPIO_NUM   13

// ── SD card CS pin ──────────────────────────────────────────────
#define SD_CS_PIN       21

httpd_handle_t server = NULL;
int imageCounter      = 0;

// ================================================================
//  SD CARD INIT
// ================================================================
bool initSDCard() {
  if (!SD.begin(SD_CS_PIN)) {
    Serial.println("SD Card: Mount FAILED");
    return false;
  }
  uint8_t cardType = SD.cardType();
  if (cardType == CARD_NONE) {
    Serial.println("SD Card: No card inserted");
    return false;
  }
  Serial.printf("SD Card: OK  (%.1f MB)\n", (float)SD.cardSize() / (1024 * 1024));
  return true;
}

// ================================================================
//  SAVE JPEG TO SD
// ================================================================
bool savePhoto(camera_fb_t* fb) {
  char path[32];
  snprintf(path, sizeof(path), "/photo_%04d.jpg", imageCounter++);

  File file = SD.open(path, FILE_WRITE);
  if (!file) {
    Serial.printf("SD: Failed to open %s\n", path);
    return false;
  }
  file.write(fb->buf, fb->len);
  file.close();
  Serial.printf("SD: Saved %s  (%u bytes)\n", path, fb->len);
  return true;
}

// ================================================================
//  ROOT PAGE HANDLER  →  GET  /
// ================================================================
static const char INDEX_HTML[] = R"rawliteral(
<!DOCTYPE html><html><head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32-S3 Cam</title>
<style>
  body{margin:0;background:#111;display:flex;flex-direction:column;
       align-items:center;justify-content:center;min-height:100vh;
       font-family:sans-serif;color:#eee;}
  img{max-width:100%;border:2px solid #333;border-radius:8px;}
  button{margin-top:18px;padding:14px 40px;font-size:1.1rem;
         background:#e63946;border:none;border-radius:8px;
         color:#fff;cursor:pointer;transition:opacity .2s;}
  button:active{opacity:.7;}
  #status{margin-top:10px;font-size:.9rem;color:#aaa;min-height:1.4em;}
</style></head><body>
<h2 style="margin-bottom:12px">ESP32-S3 Live Stream</h2>
<img src="/stream" />
<button onclick="capture()">Capture & Save</button>
<div id="status"></div>
<script>
async function capture(){
  document.getElementById('status').textContent = 'Saving...';
  try{
    const r = await fetch('/capture');
    const j = await r.json();
    document.getElementById('status').textContent =
      j.ok ? 'Saved: ' + j.file : 'Error: ' + j.error;
  }catch(e){
    document.getElementById('status').textContent = 'Request failed';
  }
}
</script></body></html>
)rawliteral";

static esp_err_t index_handler(httpd_req_t* req) {
  httpd_resp_set_type(req, "text/html");
  httpd_resp_sendstr(req, INDEX_HTML);
  return ESP_OK;
}

// ================================================================
//  MJPEG STREAM HANDLER  →  GET  /stream
// ================================================================
static esp_err_t stream_handler(httpd_req_t* req) {
  camera_fb_t* fb = NULL;
  httpd_resp_set_type(req, "multipart/x-mixed-replace; boundary=frame");

  while (true) {
    fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("Stream: capture failed");
      return ESP_FAIL;
    }
    httpd_resp_send_chunk(req, "--frame\r\n", 9);
    httpd_resp_send_chunk(req, "Content-Type: image/jpeg\r\n\r\n", 28);
    httpd_resp_send_chunk(req, (const char*)fb->buf, fb->len);
    httpd_resp_send_chunk(req, "\r\n", 2);
    esp_camera_fb_return(fb);
    delay(50);
  }
  return ESP_OK;
}

// ================================================================
//  CAPTURE HANDLER  →  GET  /capture
// ================================================================
static esp_err_t capture_handler(httpd_req_t* req) {
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  bool saved = savePhoto(fb);
  esp_camera_fb_return(fb);

  httpd_resp_set_type(req, "application/json");
  if (saved) {
    char msg[64];
    snprintf(msg, sizeof(msg), "{\"ok\":true,\"file\":\"photo_%04d.jpg\"}", imageCounter - 1);
    httpd_resp_sendstr(req, msg);
  } else {
    httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"SD write failed\"}");
  }
  return ESP_OK;
}

// ================================================================
//  START SERVER
// ================================================================
void startCameraServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;

  httpd_uri_t index_uri = {
    .uri      = "/",
    .method   = HTTP_GET,
    .handler  = index_handler,
    .user_ctx = NULL
  };

  httpd_uri_t stream_uri = {
    .uri      = "/stream",
    .method   = HTTP_GET,
    .handler  = stream_handler,
    .user_ctx = NULL
  };

  httpd_uri_t capture_uri = {
    .uri      = "/capture",
    .method   = HTTP_GET,
    .handler  = capture_handler,
    .user_ctx = NULL
  };

  if (httpd_start(&server, &config) == ESP_OK) {
    httpd_register_uri_handler(server, &index_uri);
    httpd_register_uri_handler(server, &stream_uri);
    httpd_register_uri_handler(server, &capture_uri);
    Serial.println("HTTP server started");
  }
}

// ================================================================
//  SETUP
// ================================================================
void setup() {
  Serial.begin(115200);
  Serial.println("\nStarting ESP32-S3 Camera + SD...");

  // Camera config
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;
  config.xclk_freq_hz = 10000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size   = FRAMESIZE_QQVGA;
  config.jpeg_quality = 15;
  config.fb_count     = 1;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init FAILED: 0x%x\n", err);
    return;
  }
  Serial.println("Camera: OK");

  // SD card
  initSDCard();

  // WiFi
  WiFi.begin(ssid, password);
  Serial.print("WiFi connecting");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi: connected");

  startCameraServer();

  Serial.println("\n=== READY ===");
  Serial.printf("Open browser : http://%s\n",          WiFi.localIP().toString().c_str());
  Serial.printf("Capture URL  : http://%s/capture\n",  WiFi.localIP().toString().c_str());
  Serial.printf("Stream URL   : http://%s/stream\n",   WiFi.localIP().toString().c_str());
}

// ================================================================
//  LOOP
// ================================================================
void loop() {
  delay(1000);
}
