/**
 * FoxyRadio – Integrated Controller Sketch
 * ==========================================
 * Full WiFi-enabled controller for Foxydry clothes-hanger / drier.
 * Serves a simple web interface over the local network so you can raise,
 * lower, or stop the hanger from any browser or home-automation system.
 *
 * Features
 * --------
 *  • Connects to your WiFi network (credentials in config section below).
 *  • Falls back to Access-Point mode (SSID: FoxyRadio, PW: foxyradio) when
 *    the network is not reachable.
 *  • Stores one RF pulse sequence per command (UP / DOWN / STOP) in the
 *    flash file system (LittleFS) so they survive power cycles.
 *  • Provides a captive web UI to:
 *      – manually trigger UP / DOWN / STOP
 *      – start a live RF capture (arms the CC1101 receiver) and save the
 *        result under a named command slot
 *  • HTTP REST-style endpoints (usable from Home Assistant, Node-RED, etc.):
 *      GET /cmd?name=up      → send UP command
 *      GET /cmd?name=down    → send DOWN command
 *      GET /cmd?name=stop    → send STOP command
 *      GET /capture/start    → arm receiver, returns when burst detected
 *      GET /capture/save?name=up   → save last capture as "up"
 *      GET /status           → JSON status
 *
 * Wiring – ESP8266 (Wemos D1 mini / NodeMCU)
 * -------------------------------------------
 *  CC1101  →  ESP8266
 *  VCC        3.3 V
 *  GND        GND
 *  MOSI       D7  (GPIO 13)
 *  MISO       D6  (GPIO 12)
 *  SCK        D5  (GPIO 14)
 *  CSN        D8  (GPIO 15)
 *  GDO0       D1  (GPIO  5)   ← TX data output / carrier sense
 *  GDO2       D2  (GPIO  4)   ← RX demodulated data
 *
 * Wiring – ESP32
 * --------------
 *  CC1101  →  ESP32
 *  VCC        3.3 V
 *  GND        GND
 *  MOSI       GPIO 23
 *  MISO       GPIO 19
 *  SCK        GPIO 18
 *  CSN        GPIO  5
 *  GDO0       GPIO 27
 *  GDO2       GPIO 26
 *
 * Dependencies (install via Arduino Library Manager)
 *  • SmartRC-CC1101-Driver-Lib  (ELECHOUSE)
 *  • ArduinoJson                (Benoit Blanchon, version 6.x)
 *  ESP8266:  ESP8266 core (includes LittleFS, ESP8266WiFi, ESP8266WebServer)
 *  ESP32:    ESP32 core   (includes LittleFS, WiFi, WebServer)
 *
 * Serial baud rate: 115200
 */

// ---------------------------------------------------------------------------
// WiFi credentials – edit before flashing
// ---------------------------------------------------------------------------
#define WIFI_SSID     "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

// Access-point fallback settings
#define AP_SSID     "FoxyRadio"
#define AP_PASSWORD "foxyradio"

// ---------------------------------------------------------------------------
// Pin configuration – edit to match your board
// ---------------------------------------------------------------------------
#if defined(ESP8266)
  #include <ESP8266WiFi.h>
  #include <ESP8266WebServer.h>
  #define GDO0_PIN   5   // D1
  #define GDO2_PIN   4   // D2
  #define CC1101_CS  15  // D8
  ESP8266WebServer server(80);
#elif defined(ESP32)
  #include <WiFi.h>
  #include <WebServer.h>
  #define GDO0_PIN   27
  #define GDO2_PIN   26
  #define CC1101_CS   5
  WebServer server(80);
#else
  #error "Unsupported board – please add definitions for your MCU."
#endif

#include <LittleFS.h>
#include <ArduinoJson.h>
#include <ELECHOUSE_CC1101_SRC_DRV.h>

// ---------------------------------------------------------------------------
// Capture / replay parameters
// ---------------------------------------------------------------------------
#define MAX_PULSES       512
#define MIN_PULSE_US      80
#define SILENCE_US      10000
#define REPEAT_COUNT       3
#define INTER_REPEAT_US 10000

// Command file paths on LittleFS
#define CMD_UP_FILE   "/cmd_up.json"
#define CMD_DOWN_FILE "/cmd_down.json"
#define CMD_STOP_FILE "/cmd_stop.json"

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
volatile uint32_t g_pulseBuffer[MAX_PULSES];
volatile uint16_t g_pulseCount   = 0;
volatile bool     g_captureReady = false;
volatile uint32_t g_lastEdge     = 0;

bool     g_apMode       = false;
bool     g_capturing    = false;
String   g_statusMsg    = "Idle";

// ---------------------------------------------------------------------------
// Interrupt – records edge timings during RF capture
// ---------------------------------------------------------------------------
IRAM_ATTR void edgeISR() {
  uint32_t now   = micros();
  uint32_t delta = now - g_lastEdge;
  g_lastEdge = now;

  if (g_captureReady || !g_capturing) return;
  if (delta < MIN_PULSE_US)           return;
  if (g_pulseCount < MAX_PULSES)      g_pulseBuffer[g_pulseCount++] = delta;
}

// ---------------------------------------------------------------------------
// CC1101 helpers
// ---------------------------------------------------------------------------
void cc1101RxMode() {
  ELECHOUSE_cc1101.Init();
  ELECHOUSE_cc1101.setMHZ(433.92);
  ELECHOUSE_cc1101.setModulation(2);  // OOK
  ELECHOUSE_cc1101.setDRate(3.79);
  ELECHOUSE_cc1101.setRxBW(58);
  ELECHOUSE_cc1101.setCCMode(0);
  ELECHOUSE_cc1101.SetRx();
}

void cc1101TxMode() {
  ELECHOUSE_cc1101.Init();
  ELECHOUSE_cc1101.setMHZ(433.92);
  ELECHOUSE_cc1101.setModulation(2);  // OOK
  ELECHOUSE_cc1101.setDRate(3.79);
  ELECHOUSE_cc1101.setPA(10);
  ELECHOUSE_cc1101.setCCMode(0);
  ELECHOUSE_cc1101.SetTx();
}

// ---------------------------------------------------------------------------
// Persist a captured pulse array to LittleFS as JSON
// ---------------------------------------------------------------------------
bool savePulses(const char* path, const volatile uint32_t* buf, uint16_t len) {
  File f = LittleFS.open(path, "w");
  if (!f) return false;

  f.print('[');
  for (uint16_t i = 0; i < len; i++) {
    f.print(buf[i]);
    if (i < len - 1) f.print(',');
  }
  f.println(']');
  f.close();
  return true;
}

// ---------------------------------------------------------------------------
// Load a pulse array from LittleFS
// Returns number of pulses loaded, or 0 on error
// ---------------------------------------------------------------------------
uint16_t loadPulses(const char* path, uint32_t* out, uint16_t maxLen) {
  File f = LittleFS.open(path, "r");
  if (!f) return 0;

  String content = f.readString();
  f.close();

  // Simple JSON array parse (avoids heap allocation for large buffers)
  uint16_t count = 0;
  int start = content.indexOf('[') + 1;
  while (start > 0 && count < maxLen) {
    int comma = content.indexOf(',', start);
    int end   = content.indexOf(']', start);
    int sep   = (comma >= 0 && comma < end) ? comma : end;
    if (sep < 0) break;
    out[count++] = (uint32_t)content.substring(start, sep).toInt();
    if (sep == end) break;
    start = comma + 1;
  }
  return count;
}

// ---------------------------------------------------------------------------
// Transmit pulses via CC1101 GDO0 (direct OOK mode)
// ---------------------------------------------------------------------------
void transmitPulses(const uint32_t* pulses, uint16_t len) {
  cc1101TxMode();
  delayMicroseconds(200);

  for (uint16_t i = 0; i < len; i++) {
    digitalWrite(GDO0_PIN, (i % 2 == 0) ? HIGH : LOW);
    delayMicroseconds(pulses[i]);
  }
  digitalWrite(GDO0_PIN, LOW);
  ELECHOUSE_cc1101.goSleep();
}

void sendStoredCommand(const char* filePath, const char* label) {
  static uint32_t localBuf[MAX_PULSES];
  uint16_t len = loadPulses(filePath, localBuf, MAX_PULSES);
  if (len == 0) {
    g_statusMsg = String("No data for ") + label;
    return;
  }
  g_statusMsg = String("Sending ") + label;
  for (uint8_t i = 0; i < REPEAT_COUNT; i++) {
    transmitPulses(localBuf, len);
    delayMicroseconds(INTER_REPEAT_US);
  }
  g_statusMsg = String(label) + " sent OK";
}

// ---------------------------------------------------------------------------
// WiFi setup
// ---------------------------------------------------------------------------
void setupWiFi() {
  Serial.print(F("Connecting to "));
  Serial.println(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < 15000) {
    delay(250);
    Serial.print('.');
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(F("\nConnected! IP: "));
    Serial.println(WiFi.localIP());
  } else {
    Serial.println(F("\nFalling back to AP mode"));
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASSWORD);
    Serial.print(F("AP IP: "));
    Serial.println(WiFi.softAPIP());
    g_apMode = true;
  }
}

// ---------------------------------------------------------------------------
// HTTP handler – serve the main UI page
// ---------------------------------------------------------------------------
void handleRoot() {
  String ip  = g_apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  String html = F(
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>FoxyRadio</title>"
    "<style>"
    "body{font-family:sans-serif;max-width:480px;margin:40px auto;padding:0 16px;}"
    "h1{color:#c0392b;}button{display:block;width:100%;padding:14px;margin:8px 0;"
    "font-size:1.1em;border:none;border-radius:6px;cursor:pointer;color:#fff;}"
    "#btnUp{background:#27ae60;} #btnDown{background:#e67e22;}"
    "#btnStop{background:#c0392b;} #btnCapture{background:#2980b9;}"
    "#status{margin-top:16px;padding:10px;background:#f0f0f0;border-radius:4px;}"
    "</style></head><body>"
    "<h1>&#x1F9FA; FoxyRadio</h1>"
    "<p>Foxydry clothes-hanger RF controller</p>"
    "<button id='btnUp'    onclick=\"send('/cmd?name=up')\"   >&#9650; UP</button>"
    "<button id='btnDown'  onclick=\"send('/cmd?name=down')\" >&#9660; DOWN</button>"
    "<button id='btnStop'  onclick=\"send('/cmd?name=stop')\" >&#9632; STOP</button>"
    "<hr>"
    "<p><strong>Learn a new command</strong></p>"
    "<button id='btnCapture' onclick=\"startCapture()\">&#128246; Start capture</button>"
    "<select id='slotSel'><option value='up'>UP</option>"
    "<option value='down'>DOWN</option><option value='stop'>STOP</option></select>"
    "<button onclick=\"saveCapture()\" style='background:#8e44ad;margin-top:4px;'>&#128190; Save capture</button>"
    "<div id='status'>Ready</div>"
    "<script>"
    "function send(url){"
    "  fetch(url).then(r=>r.text()).then(t=>{document.getElementById('status').innerText=t;});}"
    "function startCapture(){"
    "  document.getElementById('status').innerText='Listening…';"
    "  fetch('/capture/start').then(r=>r.text()).then(t=>{"
    "    document.getElementById('status').innerText=t;});}"
    "function saveCapture(){"
    "  var slot=document.getElementById('slotSel').value;"
    "  fetch('/capture/save?name='+slot).then(r=>r.text()).then(t=>{"
    "    document.getElementById('status').innerText=t;});}"
    "</script></body></html>"
  );
  server.send(200, F("text/html"), html);
}

// ---------------------------------------------------------------------------
// HTTP handler – send command
// ---------------------------------------------------------------------------
void handleCmd() {
  if (!server.hasArg("name")) {
    server.send(400, "text/plain", "Missing ?name=");
    return;
  }
  String name = server.arg("name");
  name.toLowerCase();

  if (name == "up") {
    sendStoredCommand(CMD_UP_FILE, "UP");
  } else if (name == "down") {
    sendStoredCommand(CMD_DOWN_FILE, "DOWN");
  } else if (name == "stop") {
    sendStoredCommand(CMD_STOP_FILE, "STOP");
  } else {
    server.send(400, "text/plain", "Unknown command. Use: up / down / stop");
    return;
  }
  server.send(200, "text/plain", g_statusMsg);
}

// ---------------------------------------------------------------------------
// HTTP handler – arm RF capture, block until burst detected (max 30 s)
// ---------------------------------------------------------------------------
void handleCaptureStart() {
  // Reset state
  noInterrupts();
  g_pulseCount   = 0;
  g_captureReady = false;
  g_capturing    = true;
  interrupts();

  cc1101RxMode();
  g_statusMsg = "Waiting for RF…";

  unsigned long deadline = millis() + 30000UL;
  while (millis() < deadline) {
    // Check silence detection
    if (g_pulseCount > 0) {
      uint32_t elapsed = micros() - g_lastEdge;
      if (elapsed > SILENCE_US) {
        g_captureReady = true;
        g_capturing    = false;
        break;
      }
    }
    yield();  // keep WiFi stack alive
  }

  ELECHOUSE_cc1101.goSleep();

  if (g_captureReady && g_pulseCount > 0) {
    g_statusMsg = "Captured " + String(g_pulseCount) + " pulses. Press Save.";
    server.send(200, "text/plain", g_statusMsg);
  } else {
    g_capturing = false;
    g_statusMsg = "No signal detected (timeout)";
    server.send(200, "text/plain", g_statusMsg);
  }
}

// ---------------------------------------------------------------------------
// HTTP handler – save last capture to a named command slot
// ---------------------------------------------------------------------------
void handleCaptureSave() {
  if (!server.hasArg("name")) {
    server.send(400, "text/plain", "Missing ?name=");
    return;
  }
  if (!g_captureReady || g_pulseCount == 0) {
    server.send(400, "text/plain", "No capture available. Run /capture/start first.");
    return;
  }

  String name = server.arg("name");
  name.toLowerCase();

  const char* path = nullptr;
  if      (name == "up")   path = CMD_UP_FILE;
  else if (name == "down") path = CMD_DOWN_FILE;
  else if (name == "stop") path = CMD_STOP_FILE;
  else {
    server.send(400, "text/plain", "Unknown slot. Use: up / down / stop");
    return;
  }

  bool ok = savePulses(path, g_pulseBuffer, g_pulseCount);
  if (ok) {
    g_statusMsg = "Saved " + String(g_pulseCount) + " pulses as '" + name + "'";
    noInterrupts();
    g_pulseCount   = 0;
    g_captureReady = false;
    interrupts();
  } else {
    g_statusMsg = "Save failed (filesystem error)";
  }
  server.send(200, "text/plain", g_statusMsg);
}

// ---------------------------------------------------------------------------
// HTTP handler – JSON status
// ---------------------------------------------------------------------------
void handleStatus() {
  StaticJsonDocument<256> doc;
  doc["status"]    = g_statusMsg;
  doc["apMode"]    = g_apMode;
  doc["ip"]        = g_apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  doc["capturing"] = g_capturing;

  doc["cmds"]["up"]   = LittleFS.exists(CMD_UP_FILE);
  doc["cmds"]["down"] = LittleFS.exists(CMD_DOWN_FILE);
  doc["cmds"]["stop"] = LittleFS.exists(CMD_STOP_FILE);

  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// ---------------------------------------------------------------------------
// Arduino setup
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println(F("\nFoxyRadio – Integrated Controller"));

  // GPIO
  pinMode(GDO0_PIN, OUTPUT);
  digitalWrite(GDO0_PIN, LOW);
  pinMode(GDO2_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(GDO2_PIN), edgeISR, CHANGE);

  // Filesystem
  if (!LittleFS.begin()) {
    Serial.println(F("LittleFS mount failed – formatting…"));
    LittleFS.format();
    LittleFS.begin();
  }
  Serial.println(F("LittleFS OK"));

  // CC1101
  if (ELECHOUSE_cc1101.getCC1101()) {
    Serial.println(F("CC1101 detected OK"));
  } else {
    Serial.println(F("CC1101 NOT detected – check wiring!"));
  }
  ELECHOUSE_cc1101.Init();
  ELECHOUSE_cc1101.goSleep();

  // WiFi
  setupWiFi();

  // HTTP routes
  server.on("/",               handleRoot);
  server.on("/cmd",            handleCmd);
  server.on("/capture/start",  handleCaptureStart);
  server.on("/capture/save",   handleCaptureSave);
  server.on("/status",         handleStatus);
  server.begin();
  Serial.println(F("HTTP server started"));
}

// ---------------------------------------------------------------------------
// Arduino loop
// ---------------------------------------------------------------------------
void loop() {
  server.handleClient();
}
