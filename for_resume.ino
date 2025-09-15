#include "esp_camera.h"
#include <WiFi.h>
#include <ESP32Servo.h>
#include <WebSocketsServer.h>

// ========= WiFi =========
const char* ssid     = "WIFI";
const char* password = "WIFI-Password";

// ========= Camera Model (AI-Thinker) =========
#define CAMERA_MODEL_AI_THINKER
#if defined(CAMERA_MODEL_AI_THINKER)
#define PWDN_GPIO_NUM  32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM  0
#define SIOD_GPIO_NUM  26
#define SIOC_GPIO_NUM  27
#define Y9_GPIO_NUM    35
#define Y8_GPIO_NUM    34
#define Y7_GPIO_NUM    39
#define Y6_GPIO_NUM    36
#define Y5_GPIO_NUM    21
#define Y4_GPIO_NUM    19
#define Y3_GPIO_NUM    18
#define Y2_GPIO_NUM    5
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM  23
#define PCLK_GPIO_NUM  22
#define LED_GPIO_NUM   4
#endif

// ========= Servers =========
WiFiServer server(80);           // HTTP
WebSocketsServer webSocket(81);  // Controls

// ========= Servos =========
// NOTE: GPIO12 is a boot strapping pin. If you see weird boots, move to 14/15.
Servo panServo, tiltServo;
#define PAN_SERVO_PIN  12
#define TILT_SERVO_PIN 13
volatile int panAngle  = 90;
volatile int tiltAngle = 90;

// ========= Camera =========
bool startCamera() {
  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_1;  // avoid conflicts with Servo lib
  config.ledc_timer   = LEDC_TIMER_1;

  config.pin_d0 = Y2_GPIO_NUM;  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM; config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM; config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM; config.pin_reset = RESET_GPIO_NUM;

  config.xclk_freq_hz = 20000000;       // stable
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size   = FRAMESIZE_QVGA; // start modest (try VGA/SVGA later)
  config.jpeg_quality = 12;             // 10..15 good range
  config.fb_count     = 2;
  config.fb_location  = CAMERA_FB_IN_PSRAM;
  config.grab_mode    = CAMERA_GRAB_LATEST;

  return esp_camera_init(&config) == ESP_OK;
}

// ========= WebSocket (servo control) =========
void webSocketEvent(uint8_t, WStype_t type, uint8_t *payload, size_t len) {
  if (type != WStype_TEXT) return;
  String cmd((char*)payload, len);

  if (cmd == "up"    && tiltAngle >  60) tiltAngle -= 5;
  if (cmd == "down"  && tiltAngle < 120) tiltAngle += 5;
  if (cmd == "left"  && panAngle  >  60) panAngle  -= 5;
  if (cmd == "right" && panAngle  < 120) panAngle  += 5;
  if (cmd == "center") { panAngle = 90; tiltAngle = 90; }
}

// ========= Continuous servo writer task =========
void servoTask(void *pv) {
  for (;;) {
    panServo.write(panAngle);
    tiltServo.write(tiltAngle);
    vTaskDelay(20 / portTICK_PERIOD_MS);
  }
}

// ========= HTTP helpers =========
static inline void sendHeader(WiFiClient &c, const char* status, const char* type, bool nocache = true) {
  c.printf("HTTP/1.1 %s\r\n", status);
  c.printf("Content-Type: %s\r\n", type);
  if (nocache) c.print("Cache-Control: no-store, no-cache, must-revalidate\r\nPragma: no-cache\r\n");
  c.print("Connection: close\r\n\r\n");
}

// Root page: **stream and controls on the SAME page**
void handleRoot(WiFiClient &c) {
  IPAddress ip = WiFi.localIP();
  sendHeader(c, "200 OK", "text/html");
  c.printf(
    "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<style>body{font-family:sans-serif}button{font-size:18px;margin:6px;padding:10px 14px}</style>"
    "<h2>ESP32-CAM Pan/Tilt (Live)</h2>"
    "<div><img id='vid' src='/stream' style='max-width:100%%;border:1px solid #ccc'></div>"
    "<h3>Controls</h3>"
    "<div>"
      "<button onclick=\"ws.send('up')\">Up</button>"
      "<button onclick=\"ws.send('down')\">Down</button>"
      "<button onclick=\"ws.send('left')\">Left</button>"
      "<button onclick=\"ws.send('right')\">Right</button>"
      "<button onclick=\"ws.send('center')\">Center</button>"
    "</div>"
    "<p>WS: <span id=s>...</span></p>"
    "<script>"
      "const ws=new WebSocket('ws://%u.%u.%u.%u:81/');"
      "ws.onopen=()=>s.textContent='connected';"
      "ws.onclose=()=>s.textContent='closed';"
      "document.addEventListener('keydown', e=>{"
        "if(e.key==='ArrowUp')ws.send('up');"
        "if(e.key==='ArrowDown')ws.send('down');"
        "if(e.key==='ArrowLeft')ws.send('left');"
        "if(e.key==='ArrowRight')ws.send('right');"
        "if(e.key==='c')ws.send('center');"
      "});"
    "</script>",
    ip[0], ip[1], ip[2], ip[3]
  );
}

// Snapshot (optional / fallback)
void handleJpg(WiFiClient &c) {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) { sendHeader(c, "503 Service Unavailable", "text/plain"); c.print("No frame"); return; }
  sendHeader(c, "200 OK", "image/jpeg");
  c.write(fb->buf, fb->len);
  esp_camera_fb_return(fb);
}

// ========= Non-blocking MJPEG stream via its own task =========
void streamClientTask(void *pv) {
  // take ownership of client passed on heap
  WiFiClient *cp = (WiFiClient*)pv;
  WiFiClient client = std::move(*cp);
  delete cp;

  client.print("HTTP/1.1 200 OK\r\n");
  client.print("Content-Type: multipart/x-mixed-replace; boundary=frame\r\n");
  client.print("Cache-Control: no-store\r\nPragma: no-cache\r\nConnection: close\r\n\r\n");

  while (client.connected()) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) break;

    client.print("--frame\r\n");
    client.print("Content-Type: image/jpeg\r\n");
    client.print("Content-Length: "); client.print(fb->len); client.print("\r\n\r\n");
    client.write(fb->buf, fb->len);
    client.print("\r\n");
    esp_camera_fb_return(fb);

    delay(20);
    yield(); // keep Wi-Fi stack fed
  }

  client.stop();
  vTaskDelete(NULL);
}

String readRequestLine(WiFiClient &c) {
  String line; unsigned long t0 = millis();
  while (c.connected() && (millis()-t0) < 1500) {
    if (c.available()) {
      char ch = c.read();
      if (ch == '\n') break;
      if (ch != '\r') line += ch;
    }
    delay(1);
  }
  return line; // e.g., "GET / HTTP/1.1"
}

void handleHttp() {
  WiFiClient c = server.available();
  if (!c) return;

  // brief wait for the first line
  unsigned long t0 = millis();
  while (c.connected() && !c.available() && (millis()-t0) < 800) delay(1);
  if (!c.available()) { c.stop(); return; }

  String req = readRequestLine(c);
  if (req.startsWith("GET / ")) {
    handleRoot(c);
    while (c.connected() && c.available()) c.read();
    c.stop();
  } else if (req.startsWith("GET /jpg")) {
    handleJpg(c);
    while (c.connected() && c.available()) c.read();
    c.stop();
  } else if (req.startsWith("GET /stream")) {
    // hand the connection off to its own task so main loop stays responsive
    WiFiClient *heapClient = new WiFiClient(std::move(c));
    xTaskCreatePinnedToCore(streamClientTask, "mjpeg", 8192, heapClient, 1, NULL, 1);
    // do NOT touch 'c' after this point
  } else {
    sendHeader(c, "404 Not Found", "text/plain");
    c.print("Not found");
    while (c.connected() && c.available()) c.read();
    c.stop();
  }
}

// ========= Setup / Loop =========
void setup() {
  Serial.begin(115200);
  pinMode(LED_GPIO_NUM, OUTPUT);
  digitalWrite(LED_GPIO_NUM, LOW);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);     // keep Wi-Fi responsive
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) { Serial.print("."); delay(500); }
  Serial.printf("\nWiFi IP: http://%s\n", WiFi.localIP().toString().c_str());

  if (!startCamera()) Serial.println("Camera init failed (enable PSRAM in Tools).");

  server.begin();
  server.setNoDelay(true);
  webSocket.begin();
  webSocket.onEvent(webSocketEvent);

  // Attach servos
  panServo.setPeriodHertz(50);
  tiltServo.setPeriodHertz(50);
  panServo.attach(PAN_SERVO_PIN, 1000, 2000);
  tiltServo.attach(TILT_SERVO_PIN, 1000, 2000);
  panServo.write(panAngle);
  tiltServo.write(tiltAngle);

  // Smooth, independent servo updates
  xTaskCreatePinnedToCore(servoTask, "ServoTask", 2048, NULL, 1, NULL, 1);
}

void loop() {
  handleHttp();      // serve root/jpg, spawn stream tasks
  webSocket.loop();  // stays responsive while streaming
  delay(1);
}


