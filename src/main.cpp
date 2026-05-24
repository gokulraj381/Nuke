#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <time.h>

// -------- OLED CONFIG --------
static const int SCREEN_WIDTH = 128;
static const int SCREEN_HEIGHT = 64;
static const int OLED_RESET = -1;
static const uint8_t OLED_ADDR = 0x3C;

// ESP32-C3 SuperMini common I2C pins (change if your board uses others)
static const int I2C_SDA_PIN = 8;
static const int I2C_SCL_PIN = 9;

// -------- PLANT SYSTEM PINS --------
// Moisture sensor analog output pin (ADC-capable). GPIO0 is common on ESP32-C3.
static const int SOIL_SENSOR_PIN = 0;

// Pump control pin. Drive a MOSFET/relay, never the pump directly.
static const int PUMP_PIN = 5;
static const bool RELAY_ACTIVE_LOW = true;

// Manual force-water button (connect button to GND and this pin).
static const int MANUAL_BUTTON_PIN = 4;

// Optional tank level switch (set to false if you do not use one).
static const bool USE_TANK_SENSOR = false;
static const int TANK_SENSOR_PIN = 1;

// -------- WIFI CONFIG --------
// Leave WIFI_SSID empty to run AP-only mode.
static const char* WIFI_SSID = "";
static const char* WIFI_PASSWORD = "";
static const char* AP_SSID = "PlantBuddy-C3";
static const char* AP_PASSWORD = "plantbuddy123";

// -------- CALIBRATION (TUNE THESE) --------
// Raw ADC value when probe is in DRY soil
static int sensorDry = 4005;
// Raw ADC value when probe is in WET soil
static int sensorWet = 1921;

// Watering behavior
static int moistureLowThreshold = 35;
static int moistureTargetAfterWater = 55;
static unsigned long pumpRunMs = 700;
static unsigned long pumpCooldownMs = 90000;
static unsigned long manualPumpMs = 600;
static bool autoWaterEnabled = true;

// Sampling and display
static const unsigned long SENSOR_SAMPLE_MS = 2000;
static const unsigned long DISPLAY_REFRESH_MS = 120;

static const unsigned long BUTTON_DEBOUNCE_MS = 60;

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
uint8_t oledAddrDetected = OLED_ADDR;
WebServer server(80);
Preferences prefs;

unsigned long lastSampleMs = 0;
unsigned long lastDisplayMs = 0;
unsigned long pumpStartedMs = 0;
unsigned long lastWateringMs = 0;
unsigned long lastButtonChangeMs = 0;

bool pumpIsOn = false;
bool pumpManualMode = false;
bool buttonState = HIGH;
bool lastButtonState = HIGH;
bool tankLow = false;

int rawReading = 0;
int moisturePct = 0;
bool moistureSensorConnected = true;
uint8_t moistureValidStreak = 0;
uint8_t moistureInvalidStreak = 0;
int moistureHistory[5] = {0, 0, 0, 0, 0};
uint8_t moistureHistoryCount = 0;
uint8_t moistureHistoryIndex = 0;
String wifiIp = "0.0.0.0";
unsigned long sunflowerAnimStartMs = 0;
static const bool USE_BACKUP_ANIMATION = false;
String wateredDaysCsv = "";
String lastWateredAt = "Never";
String lastWateringMode = "-";
unsigned long waterEvents = 0;

int clampInt(int value, int lo, int hi) {
  if (value < lo) return lo;
  if (value > hi) return hi;
  return value;
}

int pumpOutputLevel(bool on) {
  if (RELAY_ACTIVE_LOW) {
    return on ? LOW : HIGH;
  }
  return on ? HIGH : LOW;
}

unsigned long clampULong(unsigned long value, unsigned long lo, unsigned long hi) {
  if (value < lo) return lo;
  if (value > hi) return hi;
  return value;
}

String uptimeStamp() {
  unsigned long totalMin = millis() / 60000UL;
  unsigned long hours = totalMin / 60UL;
  unsigned long minutes = totalMin % 60UL;
  return String("Uptime ") + String(hours) + "h " + String(minutes) + "m";
}

bool hasValidClock() {
  return time(nullptr) > 1700000000;
}

String currentDayStamp() {
  if (!hasValidClock()) {
    return String("Session-Day-") + String((millis() / 86400000UL) + 1UL);
  }

  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  char buff[16];
  strftime(buff, sizeof(buff), "%Y-%m-%d", &t);
  return String(buff);
}

String currentDateTimeStamp() {
  if (!hasValidClock()) {
    return uptimeStamp();
  }

  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  char buff[24];
  strftime(buff, sizeof(buff), "%Y-%m-%d %H:%M", &t);
  return String(buff);
}

bool csvContainsToken(const String& csv, const String& token) {
  if (csv.length() == 0 || token.length() == 0) return false;

  int start = 0;
  while (start <= csv.length()) {
    int comma = csv.indexOf(',', start);
    if (comma < 0) comma = csv.length();
    String piece = csv.substring(start, comma);
    piece.trim();
    if (piece == token) return true;
    if (comma >= csv.length()) break;
    start = comma + 1;
  }
  return false;
}

void trimWateredDaysToMax(int maxDays) {
  if (maxDays < 1) return;

  int tokens = 0;
  for (int i = 0; i < wateredDaysCsv.length(); i++) {
    if (wateredDaysCsv[i] == ',') tokens++;
  }
  if (wateredDaysCsv.length() > 0) tokens++;

  while (tokens > maxDays) {
    int comma = wateredDaysCsv.indexOf(',');
    if (comma < 0) {
      wateredDaysCsv = "";
      break;
    }
    wateredDaysCsv = wateredDaysCsv.substring(comma + 1);
    tokens--;
  }
}

void saveWaterHistory() {
  prefs.begin("plant", false);
  prefs.putString("waterDays", wateredDaysCsv);
  prefs.putString("lastWAt", lastWateredAt);
  prefs.putString("lastWMode", lastWateringMode);
  prefs.putULong("waterCnt", waterEvents);
  prefs.end();
}

void recordWateringEvent(const char* mode) {
  String day = currentDayStamp();
  if (!csvContainsToken(wateredDaysCsv, day)) {
    if (wateredDaysCsv.length() > 0) wateredDaysCsv += ",";
    wateredDaysCsv += day;
    trimWateredDaysToMax(20);
  }

  waterEvents++;
  lastWateredAt = currentDateTimeStamp();
  lastWateringMode = String(mode);
  saveWaterHistory();
}

bool canWaterNow() {
  if (!USE_TANK_SENSOR) return true;
  return !tankLow;
}

void loadSettings() {
  prefs.begin("plant", true);
  sensorDry = prefs.getInt("sensorDry", sensorDry);
  sensorWet = prefs.getInt("sensorWet", sensorWet);
  moistureLowThreshold = prefs.getInt("lowThr", moistureLowThreshold);
  moistureTargetAfterWater = prefs.getInt("targetThr", moistureTargetAfterWater);
  pumpRunMs = prefs.getULong("pumpRun", pumpRunMs);
  pumpCooldownMs = prefs.getULong("cooldown", pumpCooldownMs);
  manualPumpMs = prefs.getULong("manualRun", manualPumpMs);
  autoWaterEnabled = prefs.getBool("autoOn", autoWaterEnabled);
  wateredDaysCsv = prefs.getString("waterDays", wateredDaysCsv);
  lastWateredAt = prefs.getString("lastWAt", lastWateredAt);
  lastWateringMode = prefs.getString("lastWMode", lastWateringMode);
  waterEvents = prefs.getULong("waterCnt", waterEvents);
  prefs.end();

  moistureLowThreshold = clampInt(moistureLowThreshold, 5, 95);
  moistureTargetAfterWater = clampInt(moistureTargetAfterWater, moistureLowThreshold + 1, 100);
  sensorDry = clampInt(sensorDry, 0, 4095);
  sensorWet = clampInt(sensorWet, 0, 4095);
  pumpRunMs = clampULong(pumpRunMs, 200, 30000);
  pumpCooldownMs = clampULong(pumpCooldownMs, 5000, 600000);
  manualPumpMs = clampULong(manualPumpMs, 200, 30000);
  trimWateredDaysToMax(20);
}

void saveSettings() {
  prefs.begin("plant", false);
  prefs.putInt("sensorDry", sensorDry);
  prefs.putInt("sensorWet", sensorWet);
  prefs.putInt("lowThr", moistureLowThreshold);
  prefs.putInt("targetThr", moistureTargetAfterWater);
  prefs.putULong("pumpRun", pumpRunMs);
  prefs.putULong("cooldown", pumpCooldownMs);
  prefs.putULong("manualRun", manualPumpMs);
  prefs.putBool("autoOn", autoWaterEnabled);
  prefs.end();
}

int readMoisturePercent() {
  rawReading = analogRead(SOIL_SENSOR_PIN);

  // Store recent samples to detect floating/disconnected behavior.
  moistureHistory[moistureHistoryIndex] = rawReading;
  moistureHistoryIndex = (moistureHistoryIndex + 1) % 5;
  if (moistureHistoryCount < 5) moistureHistoryCount++;

  int minRaw = 4095;
  int maxRaw = 0;
  int railCount = 0;
  for (uint8_t i = 0; i < moistureHistoryCount; i++) {
    int v = moistureHistory[i];
    if (v < minRaw) minRaw = v;
    if (v > maxRaw) maxRaw = v;
    if (v <= 40 || v >= 4055) railCount++;
  }

  int span = maxRaw - minRaw;
  bool railInvalid = (rawReading <= 20 || rawReading >= 4075);
  bool unstableInvalid = (moistureHistoryCount >= 3) && (span > 700);
  bool historyInvalid = (moistureHistoryCount >= 3) && (railCount > 0);
  bool sampleLooksDisconnected = railInvalid || unstableInvalid || historyInvalid;

  // Hysteresis: require repeated evidence before toggling connected state.
  if (sampleLooksDisconnected) {
    moistureInvalidStreak = (moistureInvalidStreak < 255) ? (moistureInvalidStreak + 1) : 255;
    moistureValidStreak = 0;
  } else {
    moistureValidStreak = (moistureValidStreak < 255) ? (moistureValidStreak + 1) : 255;
    moistureInvalidStreak = 0;
  }

  if (moistureInvalidStreak >= 2) moistureSensorConnected = false;
  if (moistureValidStreak >= 3) moistureSensorConnected = true;

  if (!moistureSensorConnected) {
    moisturePct = 0;

    if (USE_TANK_SENSOR) {
      // LOW means tank is empty when using INPUT_PULLUP + float switch to GND.
      tankLow = (digitalRead(TANK_SENSOR_PIN) == LOW);
    }
    return moisturePct;
  }

  // Convert raw ADC to percent where 0 = dry and 100 = wet.
  int pct = map(rawReading, sensorDry, sensorWet, 0, 100);
  moisturePct = clampInt(pct, 0, 100);

  if (USE_TANK_SENSOR) {
    // LOW means tank is empty when using INPUT_PULLUP + float switch to GND.
    tankLow = (digitalRead(TANK_SENSOR_PIN) == LOW);
  }

  return moisturePct;
}

void setPump(bool on) {
  bool wasOn = pumpIsOn;
  pumpIsOn = on;
  digitalWrite(PUMP_PIN, pumpOutputLevel(on));
  if (on && !wasOn) {
    pumpStartedMs = millis();
    lastWateringMs = pumpStartedMs;
    recordWateringEvent(pumpManualMode ? "manual" : "auto");
  }
}

void maybeStartAutoWatering() {
  if (!autoWaterEnabled) return;
  if (pumpIsOn) return;
  if (!moistureSensorConnected) return;
  if (!canWaterNow()) return;

  unsigned long now = millis();
  if (now - lastWateringMs < pumpCooldownMs) return;

  if (moisturePct < moistureLowThreshold) {
    pumpManualMode = false;
    setPump(true);
    Serial.println("[AUTO] Moisture low. Starting watering cycle.");
  }
}

void startManualWatering() {
  if (pumpIsOn) return;
  if (!canWaterNow()) {
    Serial.println("[MANUAL] Watering blocked. Tank appears empty.");
    return;
  }

  pumpManualMode = true;
  setPump(true);
  Serial.println("[MANUAL] Manual watering started.");
}

void maybeStopPump() {
  if (!pumpIsOn) return;

  unsigned long now = millis();
  unsigned long activeRunMs = pumpManualMode ? manualPumpMs : pumpRunMs;
  bool timeReached = (now - pumpStartedMs) >= activeRunMs;

  // Stop early if target moisture reached.
  bool moistureRecovered = moisturePct >= moistureTargetAfterWater;
  if (pumpManualMode) {
    moistureRecovered = false;
  }

  if (timeReached || moistureRecovered) {
    setPump(false);
    pumpManualMode = false;
    Serial.println("[AUTO] Stopping pump.");
  }
}

void handleManualButton() {
  bool current = digitalRead(MANUAL_BUTTON_PIN);
  unsigned long now = millis();

  if (current != lastButtonState) {
    lastButtonChangeMs = now;
    lastButtonState = current;
  }

  if ((now - lastButtonChangeMs) > BUTTON_DEBOUNCE_MS && current != buttonState) {
    buttonState = current;
    if (buttonState == LOW) {
      startManualWatering();
    }
  }
}

void startWifi() {
  WiFi.mode(WIFI_AP_STA);

  bool startedSta = false;
  if (strlen(WIFI_SSID) > 0) {
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    unsigned long startMs = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - startMs) < 10000) {
      delay(250);
    }
    startedSta = (WiFi.status() == WL_CONNECTED);
  }

  bool startedAp = WiFi.softAP(AP_SSID, AP_PASSWORD);

  if (startedSta) {
    wifiIp = WiFi.localIP().toString();
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    Serial.print("[WIFI] STA connected, IP: ");
    Serial.println(wifiIp);
  } else if (startedAp) {
    wifiIp = WiFi.softAPIP().toString();
    Serial.print("[WIFI] AP ready, IP: ");
    Serial.println(wifiIp);
    Serial.print("[WIFI] AP SSID: ");
    Serial.println(AP_SSID);
  } else {
    wifiIp = "0.0.0.0";
    Serial.println("[WIFI] Failed to start Wi-Fi.");
  }
}

String statusJson() {
  String out = "{";
  out += "\"moisture\":" + String(moisturePct) + ",";
  out += "\"sensorConnected\":" + String(moistureSensorConnected ? "true" : "false") + ",";
  out += "\"raw\":" + String(rawReading) + ",";
  out += "\"pumpOn\":" + String(pumpIsOn ? "true" : "false") + ",";
  out += "\"manualMode\":" + String(pumpManualMode ? "true" : "false") + ",";
  out += "\"autoWater\":" + String(autoWaterEnabled ? "true" : "false") + ",";
  out += "\"lowThreshold\":" + String(moistureLowThreshold) + ",";
  out += "\"targetThreshold\":" + String(moistureTargetAfterWater) + ",";
  out += "\"pumpRunMs\":" + String((unsigned long)pumpRunMs) + ",";
  out += "\"cooldownMs\":" + String((unsigned long)pumpCooldownMs) + ",";
  out += "\"manualRunMs\":" + String((unsigned long)manualPumpMs) + ",";
  out += "\"sensorDry\":" + String(sensorDry) + ",";
  out += "\"sensorWet\":" + String(sensorWet) + ",";
  out += "\"tankLow\":" + String(tankLow ? "true" : "false") + ",";
  out += "\"waterEvents\":" + String((unsigned long)waterEvents) + ",";
  out += "\"waterDays\":\"" + wateredDaysCsv + "\",";
  out += "\"lastWateredAt\":\"" + lastWateredAt + "\",";
  out += "\"lastWateringMode\":\"" + lastWateringMode + "\",";
  out += "\"ip\":\"" + wifiIp + "\"";
  out += "}";
  return out;
}

void serveDashboard() {
  static const char* PAGE = R"HTML(
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Plant Buddy C3</title>
  <style>
    :root{--bg:#eef3ea;--ink:#1f2b20;--muted:#5b6d5e;--panel:#fffefb;--line:#d6e1d1;--mint:#6ec49f;--leaf:#2f8b68;--earth:#a36a3e;--danger:#c45050;--ok:#2a8f64}
    *{box-sizing:border-box}
    body{margin:0;min-height:100vh;font-family:'Trebuchet MS','Gill Sans','Segoe UI Variable',sans-serif;color:var(--ink);background:radial-gradient(1000px 520px at -12% -18%,#9dd9b8 0%,transparent 72%),radial-gradient(950px 500px at 110% -10%,#f3c584 0%,transparent 72%),radial-gradient(800px 420px at 50% 110%,#9fc7e8 0%,transparent 70%),linear-gradient(160deg,#f4f9f0 0%,#eef7ef 45%,#eaf0f8 100%)}
    .wrap{max-width:980px;margin:0 auto;padding:20px 14px 26px}
    .hero{padding:18px 18px 14px;border:1px solid #d3e1d0;border-radius:18px;background:linear-gradient(135deg,rgba(255,255,255,.95),rgba(244,255,248,.88));box-shadow:0 18px 34px rgba(24,54,41,.14)}
    .title{display:flex;align-items:center;justify-content:space-between;gap:8px;flex-wrap:wrap}
    h1{margin:0;font-size:1.65rem;letter-spacing:.4px}
    .sub{margin-top:6px;color:var(--muted);font-size:.95rem}
    .badge{display:inline-flex;align-items:center;gap:8px;padding:8px 12px;border-radius:999px;background:linear-gradient(120deg,#e8fff4,#f3fff9);border:1px solid #bfe8d6;font-size:.86rem}
    .dot{width:10px;height:10px;border-radius:50%;background:var(--ok);box-shadow:0 0 0 0 rgba(42,143,100,.45);animation:pulse 1.8s infinite}
    @keyframes pulse{0%{box-shadow:0 0 0 0 rgba(42,143,100,.45)}70%{box-shadow:0 0 0 11px rgba(42,143,100,0)}100%{box-shadow:0 0 0 0 rgba(42,143,100,0)}}
    .grid{display:grid;grid-template-columns:1.4fr 1fr;gap:14px;margin-top:14px}
    .stack{display:grid;gap:14px}
    .card{padding:14px;border-radius:16px;border:1px solid #d6e2d5;background:linear-gradient(145deg,rgba(255,255,255,.94),rgba(250,255,247,.90));box-shadow:0 12px 24px rgba(32,54,42,.12)}
    .card h2{margin:0;font-size:1.02rem}
    .card-head{display:flex;align-items:center;justify-content:space-between;gap:8px;margin-bottom:10px}
    .tiny-btn{padding:6px 10px;border-radius:9px;border:1px solid #cfe1ce;background:#f2f8f1;color:#35553e;font-size:.75rem;font-weight:700;cursor:pointer}
    .meter{display:grid;grid-template-columns:110px 1fr;gap:12px;align-items:center}
    .ring{width:104px;height:104px;border-radius:50%;background:conic-gradient(var(--mint) calc(var(--pct)*1%),#d8e2f0 0);display:grid;place-items:center;transition:all .35s ease}
    .ring-inner{width:78px;height:78px;border-radius:50%;display:grid;place-items:center;background:#fff;border:1px solid #e1eadf;font-size:1.1rem;font-weight:700}
    .statline{display:flex;justify-content:space-between;gap:8px;padding:8px 0;border-bottom:1px dashed #d8e3d7}
    .statline:last-child{border-bottom:0}
    .k{color:var(--muted)}
    .v{font-weight:700}
    .actions{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:8px}
    button{border:0;border-radius:11px;padding:11px 12px;font-weight:700;cursor:pointer;transition:.18s transform,.18s filter}
    button:hover{transform:translateY(-1px);filter:brightness(1.02)}
    .primary{background:linear-gradient(130deg,var(--leaf),#2f74a9);color:#fff}
    .alt{background:linear-gradient(130deg,#edf7ec,#e8f1fb);color:#284432;border:1px solid #cfe0d4}
    .warn{background:linear-gradient(130deg,var(--earth),#cb7f55);color:#fff}
    .history-days{display:flex;flex-wrap:wrap;gap:6px;min-height:30px}
    .chip{padding:5px 9px;border-radius:999px;background:#eef5ea;border:1px solid #d4e3d2;font-size:.82rem}
    .empty{color:#7b8e80;font-size:.9rem}
    .calendar-head{display:flex;justify-content:space-between;align-items:center;gap:8px;margin:10px 0 6px}
    .calendar-grid{display:grid;grid-template-columns:repeat(7,minmax(0,1fr));gap:6px}
    .cal-dow{font-size:.72rem;color:#607465;text-align:center;padding:2px 0}
    .cal-day{height:30px;border-radius:8px;display:grid;place-items:center;font-size:.78rem;border:1px solid #d6e3d5;background:#f7faf6;color:#324b39}
    .cal-watered{background:#cdebd0;border-color:#9fd7a5;color:#215a2d;font-weight:700}
    .cal-dry{background:#f5d9d9;border-color:#e3b3b3;color:#7e2f2f}
    .cal-future{background:#f2f4f1;border-color:#e3e8e0;color:#9aa89b}
    .demo-row{display:flex;justify-content:space-between;align-items:center;gap:8px;margin-top:10px}
    .switch{position:relative;width:46px;height:26px;display:inline-block}
    .switch input{opacity:0;width:0;height:0}
    .slider{position:absolute;cursor:pointer;inset:0;background:#d1ddd0;transition:.2s;border-radius:999px}
    .slider:before{content:"";position:absolute;height:20px;width:20px;left:3px;top:3px;background:white;transition:.2s;border-radius:50%}
    .switch input:checked + .slider{background:#6ca572}
    .switch input:checked + .slider:before{transform:translateX(20px)}
    .logs{margin-top:10px;max-height:130px;overflow:auto;border:1px dashed #d4e2d2;border-radius:10px;padding:8px;background:#fbfdfb}
    .log-item{font-size:.82rem;color:#35533d;padding:4px 0;border-bottom:1px dotted #d8e4d8}
    .log-item:last-child{border-bottom:0}
    .fields{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:8px}
    label{display:block;margin-bottom:4px;font-size:.8rem;color:#405443}
    input{width:100%;padding:9px;border-radius:10px;border:1px solid #cfe0cd;background:#fff}
    .footer-note{margin-top:10px;font-size:.82rem;color:#6a7f70}
    .focus-hidden{display:none !important}
    .fullscreen-card{position:fixed !important;inset:10px !important;z-index:9999 !important;overflow:auto !important;margin:0 !important;padding:18px !important;border-radius:18px !important;box-shadow:0 30px 50px rgba(24,35,27,.25) !important}
    .focus-exit{position:fixed;right:18px;top:16px;z-index:10000;border:0;border-radius:999px;padding:8px 14px;background:#213225;color:#fff;font-weight:700;cursor:pointer;display:none}
    body.focus-active .focus-exit{display:inline-block}
    @media (max-width:860px){.grid{grid-template-columns:1fr}.fields{grid-template-columns:1fr 1fr}}
    @media (max-width:560px){.fields{grid-template-columns:1fr}.actions{grid-template-columns:1fr}}
  </style>
</head>
<body>
<div class="wrap">
  <section class="hero">
    <div class="title">
      <div>
        <h1>Plant Buddy Dashboard</h1>
        <div class="sub">Live moisture, watering controls, and watering-day history</div>
      </div>
      <div class="badge"><span class="dot"></span><span id="ip">Waiting for device...</span></div>
    </div>
  </section>

  <section class="grid">
    <div class="stack">
      <div class="card focusable-card" id="statusCard">
        <div class="card-head">
          <h2>Live Plant Status</h2>
          <button id="btnStatusFullscreen" class="tiny-btn">Fullscreen</button>
        </div>
        <div class="meter">
          <div class="ring" id="ring" style="--pct:0"><div class="ring-inner"><span id="mo">--%</span></div></div>
          <div>
            <div class="statline"><span class="k">Pump</span><span class="v" id="pump">--</span></div>
            <div class="statline"><span class="k">Tank</span><span class="v" id="tank">--</span></div>
            <div class="statline"><span class="k">Raw ADC</span><span class="v" id="raw">--</span></div>
            <div class="statline"><span class="k">Auto Water</span><span class="v" id="autoState">--</span></div>
          </div>
        </div>
      </div>

      <div class="card focusable-card" id="historyCard">
        <div class="card-head">
          <h2>Watering History</h2>
          <button id="btnHistoryFullscreen" class="tiny-btn">Fullscreen</button>
        </div>
        <div class="statline"><span class="k">Last Watered</span><span class="v" id="lastWatered">Never</span></div>
        <div class="statline"><span class="k">Last Mode</span><span class="v" id="lastMode">-</span></div>
        <div class="statline"><span class="k">Total Watering Events</span><span class="v" id="waterCount">0</span></div>
        <div style="margin-top:10px" class="k">Days Watered</div>
        <div class="history-days" id="daysWrap"><span class="empty">No watering day recorded yet.</span></div>
        <div class="calendar-head">
          <div class="k" id="calendarTitle">This Month</div>
          <div class="k">Green = watered, Red = not watered</div>
        </div>
        <div class="calendar-grid" id="calendarGrid"></div>
        <div class="demo-row">
          <span class="k">Demo presentation logs</span>
          <label class="switch">
            <input id="demoToggle" type="checkbox">
            <span class="slider"></span>
          </label>
        </div>
        <div class="logs" id="logsWrap"><div class="empty">No log entries yet.</div></div>
      </div>
    </div>

    <div class="stack">
      <div class="card">
        <h2>Quick Actions</h2>
        <div class="actions">
          <button id="btnWater" class="warn">Water Now</button>
          <button id="btnAuto" class="alt">Toggle Auto</button>
          <button id="btnDry" class="alt">Set Dry = Current</button>
          <button id="btnWet" class="alt">Set Wet = Current</button>
        </div>
      </div>

      <div class="card">
        <h2>Control Tuning</h2>
        <div class="fields">
          <div><label>Low Threshold %</label><input id="low" type="number"></div>
          <div><label>Stop Target %</label><input id="target" type="number"></div>
          <div><label>Pump Run ms</label><input id="run" type="number"></div>
          <div><label>Cooldown ms</label><input id="cool" type="number"></div>
          <div><label>Manual Run ms</label><input id="manual" type="number"></div>
          <div><label>Dry ADC</label><input id="dry" type="number"></div>
          <div><label>Wet ADC</label><input id="wet" type="number"></div>
        </div>
        <p><button id="btnSave" class="primary">Save Settings</button></p>
        <div class="footer-note">Tip: use a lower run time with higher cooldown for gentler watering.</div>
      </div>
    </div>
  </section>

</div>
<button id="btnExitFocus" class="focus-exit">Exit Fullscreen</button>

<script>
async function j(path){const r=await fetch(path,{method:'GET'});return r.json();}
async function p(path){await fetch(path,{method:'POST'});}
function setText(id,v){document.getElementById(id).textContent=v;}
function setVal(id,v){document.getElementById(id).value=v;}
function n(id){return parseInt(document.getElementById(id).value||'0',10);}
let auto=true;
let demoMode=false;

function enterFocus(cardId){
  document.body.classList.add('focus-active');
  document.querySelectorAll('.focusable-card').forEach(el=>{
    if(el.id===cardId){
      el.classList.add('fullscreen-card');
      el.classList.remove('focus-hidden');
    } else {
      el.classList.add('focus-hidden');
      el.classList.remove('fullscreen-card');
    }
  });
}

function exitFocus(){
  document.body.classList.remove('focus-active');
  document.querySelectorAll('.focusable-card').forEach(el=>{
    el.classList.remove('focus-hidden');
    el.classList.remove('fullscreen-card');
  });
}

function ymd(d){
  const y=d.getFullYear();
  const m=String(d.getMonth()+1).padStart(2,'0');
  const day=String(d.getDate()).padStart(2,'0');
  return `${y}-${m}-${day}`;
}

function monthLabel(d){
  return d.toLocaleString(undefined,{month:'long',year:'numeric'});
}

function buildDemoData(baseDate){
  const year=baseDate.getFullYear();
  const month=baseDate.getMonth();
  const countDays=new Date(year,month+1,0).getDate();
  const watered=[];
  const logs=[];

  for(let day=1; day<=countDays; day++){
    if(day%3===0 || day===2 || day===17 || day===countDays-1){
      const dt=new Date(year,month,day);
      const key=ymd(dt);
      watered.push(key);
      const mode=(day%2===0)?'auto':'manual';
      logs.push(`${key} 07:${String((day*3)%60).padStart(2,'0')} - Watered (${mode})`);
    }
  }

  return { watered, logs: logs.slice(-18).reverse() };
}

function renderCalendar(daysCsv){
  const grid=document.getElementById('calendarGrid');
  grid.innerHTML='';

  const now=new Date();
  document.getElementById('calendarTitle').textContent=monthLabel(now);

  const dows=['Sun','Mon','Tue','Wed','Thu','Fri','Sat'];
  dows.forEach(d=>{
    const el=document.createElement('div');
    el.className='cal-dow';
    el.textContent=d;
    grid.appendChild(el);
  });

  let wateredSet=new Set((daysCsv||'').split(',').map(s=>s.trim()).filter(Boolean));
  if(demoMode){
    wateredSet=new Set(buildDemoData(now).watered);
  }

  const year=now.getFullYear();
  const month=now.getMonth();
  const firstDow=new Date(year,month,1).getDay();
  const dayCount=new Date(year,month+1,0).getDate();
  const today=now.getDate();

  for(let i=0;i<firstDow;i++){
    const blank=document.createElement('div');
    grid.appendChild(blank);
  }

  for(let day=1; day<=dayCount; day++){
    const cell=document.createElement('div');
    const dt=new Date(year,month,day);
    const key=ymd(dt);
    cell.className='cal-day';
    cell.textContent=String(day);

    if(day>today){
      cell.classList.add('cal-future');
    } else if(wateredSet.has(key)){
      cell.classList.add('cal-watered');
    } else {
      cell.classList.add('cal-dry');
    }
    grid.appendChild(cell);
  }
}

function renderLogs(status){
  const wrap=document.getElementById('logsWrap');
  wrap.innerHTML='';

  let logs=[];
  if(demoMode){
    logs=buildDemoData(new Date()).logs;
  } else {
    const days=(status.waterDays||'').split(',').map(s=>s.trim()).filter(Boolean).slice(-12).reverse();
    logs=days.map((d,i)=>`${d} - Watered (${i===0 ? (status.lastWateringMode||'auto') : 'auto'})`);
    if(status.lastWateredAt && status.lastWateredAt!=='Never'){
      logs.unshift(`${status.lastWateredAt} - Last watering (${status.lastWateringMode||'-'})`);
    }
  }

  if(!logs.length){
    const e=document.createElement('div');
    e.className='empty';
    e.textContent='No log entries yet.';
    wrap.appendChild(e);
    return;
  }

  logs.forEach(item=>{
    const row=document.createElement('div');
    row.className='log-item';
    row.textContent=item;
    wrap.appendChild(row);
  });
}

function renderDays(daysCsv){
  const wrap=document.getElementById('daysWrap');
  wrap.innerHTML='';
  const days=(daysCsv||'').split(',').map(s=>s.trim()).filter(Boolean);
  if(!days.length){
    const e=document.createElement('span');
    e.className='empty';
    e.textContent='No watering day recorded yet.';
    wrap.appendChild(e);
    return;
  }
  days.slice().reverse().forEach(d=>{
    const chip=document.createElement('span');
    chip.className='chip';
    chip.textContent=d;
    wrap.appendChild(chip);
  });
}

async function refresh(){
  const s=await j('/api/status');
  auto=s.autoWater;
  setText('mo', s.sensorConnected ? (s.moisture + '%') : 'NC');
  setText('raw', s.raw);
  setText('pump', s.pumpOn ? (s.manualMode ? 'ON (manual)' : 'ON (auto)') : 'OFF');
  setText('tank', s.tankLow ? 'LOW' : 'OK');
  setText('autoState', s.autoWater ? 'Enabled' : 'Disabled');
  setText('ip', 'IP ' + s.ip);
  setText('lastWatered', s.lastWateredAt || 'Never');
  setText('lastMode', s.lastWateringMode || '-');
  setText('waterCount', demoMode ? '18 (demo)' : String(s.waterEvents || 0));
  renderDays(s.waterDays || '');
  renderCalendar(s.waterDays || '');
  renderLogs(s);
  document.getElementById('ring').style.setProperty('--pct', s.sensorConnected ? Math.max(0, Math.min(100, s.moisture || 0)) : 0);

  setVal('low', s.lowThreshold);
  setVal('target', s.targetThreshold);
  setVal('run', s.pumpRunMs);
  setVal('cool', s.cooldownMs);
  setVal('manual', s.manualRunMs);
  setVal('dry', s.sensorDry);
  setVal('wet', s.sensorWet);
}

document.getElementById('btnWater').onclick=()=>p('/api/water');
document.getElementById('btnAuto').onclick=()=>p('/api/auto?enabled='+(auto?0:1));
document.getElementById('btnDry').onclick=()=>p('/api/calibrate?mode=dry');
document.getElementById('btnWet').onclick=()=>p('/api/calibrate?mode=wet');
document.getElementById('btnSave').onclick=()=>p('/api/settings?low='+n('low')+'&target='+n('target')+'&run='+n('run')+'&cool='+n('cool')+'&manual='+n('manual')+'&dry='+n('dry')+'&wet='+n('wet'));
document.getElementById('demoToggle').onchange=(e)=>{demoMode=e.target.checked; refresh();};
document.getElementById('btnStatusFullscreen').onclick=()=>enterFocus('statusCard');
document.getElementById('btnHistoryFullscreen').onclick=()=>enterFocus('historyCard');
document.getElementById('btnExitFocus').onclick=()=>exitFocus();

setInterval(refresh, 2000);
refresh();
</script>
</body>
</html>
)HTML";

  server.send(200, "text/html", PAGE);
}

void setupHttpServer() {
  server.on("/", HTTP_GET, serveDashboard);

  server.on("/api/status", HTTP_GET, []() {
    server.send(200, "application/json", statusJson());
  });

  server.on("/api/water", HTTP_POST, []() {
    startManualWatering();
    server.send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/api/auto", HTTP_POST, []() {
    if (server.hasArg("enabled")) {
      autoWaterEnabled = (server.arg("enabled").toInt() != 0);
      saveSettings();
    }
    server.send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/api/calibrate", HTTP_POST, []() {
    if (server.hasArg("mode")) {
      String mode = server.arg("mode");
      if (mode == "dry") sensorDry = rawReading;
      if (mode == "wet") sensorWet = rawReading;
      saveSettings();
    }
    server.send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/api/settings", HTTP_POST, []() {
    if (server.hasArg("low")) moistureLowThreshold = clampInt(server.arg("low").toInt(), 5, 95);
    if (server.hasArg("target")) moistureTargetAfterWater = clampInt(server.arg("target").toInt(), moistureLowThreshold + 1, 100);
    if (server.hasArg("run")) pumpRunMs = clampULong((unsigned long)server.arg("run").toInt(), 200, 30000);
    if (server.hasArg("cool")) pumpCooldownMs = clampULong((unsigned long)server.arg("cool").toInt(), 5000, 600000);
    if (server.hasArg("manual")) manualPumpMs = clampULong((unsigned long)server.arg("manual").toInt(), 200, 30000);
    if (server.hasArg("dry")) sensorDry = clampInt(server.arg("dry").toInt(), 0, 4095);
    if (server.hasArg("wet")) sensorWet = clampInt(server.arg("wet").toInt(), 0, 4095);
    saveSettings();
    server.send(200, "application/json", "{\"ok\":true}");
  });

  server.begin();
  Serial.println("[HTTP] Dashboard server started.");
}

bool initOledDisplay() {
  const uint8_t candidates[] = {OLED_ADDR, 0x3D};
  for (uint8_t i = 0; i < 2; i++) {
    if (display.begin(SSD1306_SWITCHCAPVCC, candidates[i])) {
      oledAddrDetected = candidates[i];
      return true;
    }
  }
  return false;
}

void drawPlantTypeWallpaperBackup() {
  const unsigned long typeMs = 1400;
  const unsigned long holdMs = 3000;
  const unsigned long cycleMs = typeMs + holdMs;
  const unsigned long growMs = 1800;
  unsigned long elapsed = (millis() - sunflowerAnimStartMs) % cycleMs;

  int growPx = 0;
  if (elapsed < growMs) {
    growPx = (int)((elapsed * 30UL) / growMs);
  } else {
    growPx = 30;
  }

  int groundY = 61;
  int stemBaseY = groundY - 1;
  int stemTopY = stemBaseY - growPx;
  int stemX = 64;

  display.clearDisplay();

  // Ground line and subtle grass pixels.
  display.drawFastHLine(0, groundY, SCREEN_WIDTH, SSD1306_WHITE);
  for (int x = 3; x < SCREEN_WIDTH; x += 7) {
    display.drawPixel(x, groundY - 1 - (x % 2), SSD1306_WHITE);
  }

  // Simple plant growth animation (stem + leaves + top bud).
  display.drawLine(stemX, stemBaseY, stemX, stemTopY, SSD1306_WHITE);
  if (growPx > 8) {
    display.drawLine(stemX, stemBaseY - 8, stemX - 10, stemBaseY - 13, SSD1306_WHITE);
  }
  if (growPx > 14) {
    display.drawLine(stemX, stemBaseY - 15, stemX + 11, stemBaseY - 20, SSD1306_WHITE);
  }
  if (growPx > 20) {
    display.drawLine(stemX, stemBaseY - 21, stemX - 9, stemBaseY - 27, SSD1306_WHITE);
  }
  display.fillCircle(stemX, stemTopY - 2, 2, SSD1306_WHITE);

  // Type-style reveal for the only text: "nuke".
  int textChars = 0;
  if (elapsed < typeMs) {
    textChars = 1 + (int)(elapsed / 350UL);
    textChars = clampInt(textChars, 1, 4);
  } else {
    textChars = 4;
  }
  const char* fullText = "nuke";
  char textBuf[5] = {0};
  for (int i = 0; i < textChars; i++) {
    textBuf[i] = fullText[i];
  }

  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(40, 8);
  display.print(textBuf);

  // Blinking cursor to enhance typing effect.
  bool inHold = (elapsed >= typeMs);
  if (!inHold && ((elapsed / 250UL) % 2UL) == 0) {
    int cursorX = 40 + (textChars * 12);
    display.fillRect(cursorX, 24, 8, 2, SSD1306_WHITE);
  }
}

void drawFantasticPlantAnimation(unsigned long elapsed) {
  const unsigned long sunflowerMs = 1700;
  const unsigned long fadeOutMs = 800;
  const unsigned long zoomMs = 1200;
  const unsigned long holdMs = 1400;
  const unsigned long winkFaceMs = 2400;
  const unsigned long cycleMs = sunflowerMs + fadeOutMs + zoomMs + holdMs + winkFaceMs;
  elapsed = elapsed % cycleMs;

  int cx = 64;
  int cy = 30;
  int groundY = 61;

  display.clearDisplay();

  if (elapsed < sunflowerMs) {
    // Phase 1: sunflower face only.
    display.drawFastHLine(0, groundY, SCREEN_WIDTH, SSD1306_WHITE);
    for (int x = 2; x < SCREEN_WIDTH; x += 8) {
      display.drawPixel(x, groundY - 1 - (x % 2), SSD1306_WHITE);
    }
    display.drawLine(cx, groundY - 1, cx, cy + 12, SSD1306_WHITE);
    display.drawLine(cx, cy + 18, cx - 10, cy + 13, SSD1306_WHITE);
    display.drawLine(cx, cy + 24, cx + 10, cy + 19, SSD1306_WHITE);

    int petalDist = 14;
    int petalR = 3;
    for (int i = 0; i < 12; i++) {
      float a = (6.28318f * i) / 12.0f;
      int px = cx + (int)(cos(a) * petalDist);
      int py = cy + (int)(sin(a) * petalDist);
      display.fillCircle(px, py, petalR, SSD1306_WHITE);
    }
    display.fillCircle(cx, cy, 10, SSD1306_WHITE);
    display.fillCircle(cx, cy, 8, SSD1306_BLACK);
    display.fillCircle(cx - 4, cy - 2, 1, SSD1306_WHITE);
    display.fillCircle(cx + 4, cy - 2, 1, SSD1306_WHITE);
    display.drawLine(cx - 4, cy + 4, cx, cy + 6, SSD1306_WHITE);
    display.drawLine(cx, cy + 6, cx + 4, cy + 4, SSD1306_WHITE);
  } else if (elapsed < sunflowerMs + fadeOutMs) {
    // Phase 2: sunflower smoothly fades/shrinks out, no text.
    display.drawFastHLine(0, groundY, SCREEN_WIDTH, SSD1306_WHITE);
    display.drawLine(cx, groundY - 1, cx, cy + 12, SSD1306_WHITE);

    float t = (float)(elapsed - sunflowerMs) / (float)fadeOutMs;
    float e = t * t * (3.0f - 2.0f * t);
    int petalDist = 14 - (int)(e * 12.0f);
    int petalR = 3 - (int)(e * 2.0f);
    int petalsVisible = 12 - (int)(e * 12.0f);
    if (petalR < 1) petalR = 1;
    if (petalsVisible < 0) petalsVisible = 0;

    for (int i = 0; i < petalsVisible; i++) {
      float a = (6.28318f * i) / 12.0f;
      int px = cx + (int)(cos(a) * petalDist);
      int py = cy + (int)(sin(a) * petalDist);
      display.fillCircle(px, py, petalR, SSD1306_WHITE);
    }

    int faceR = 10 - (int)(e * 9.0f);
    if (faceR > 0) {
      display.fillCircle(cx, cy, faceR, SSD1306_WHITE);
      if (faceR > 1) {
        display.fillCircle(cx, cy, faceR - 1, SSD1306_BLACK);
      }
    }
  } else if (elapsed < sunflowerMs + fadeOutMs + zoomMs + holdMs) {
    // Phase 3 + 4: nuke zooms in and holds with center reveal (smooth, no size jumps).
    float zoomT = 1.0f;
    if (elapsed < sunflowerMs + fadeOutMs + zoomMs) {
      zoomT = (float)(elapsed - sunflowerMs - fadeOutMs) / (float)zoomMs;
      zoomT = zoomT * zoomT * (3.0f - 2.0f * zoomT);
    }

    int textSize = 4;
    int fullW = 6 * 4 * textSize;  // 6 px per char * 4 chars * size
    int fullH = 8 * textSize;
    int targetX = (SCREEN_WIDTH - fullW) / 2;
    int targetY = (SCREEN_HEIGHT - fullH) / 2;

    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(textSize);
    display.setCursor(targetX, targetY);
    display.print("nuke");

    // Reveal from center outward for smooth pseudo-zoom effect.
    int revealW = clampInt((int)(fullW * zoomT), 1, fullW);
    int revealH = clampInt((int)(fullH * zoomT), 1, fullH);
    int revealX = targetX + (fullW - revealW) / 2;
    int revealY = targetY + (fullH - revealH) / 2;

    if (revealX > 0) {
      display.fillRect(0, 0, revealX, SCREEN_HEIGHT, SSD1306_BLACK);
    }
    int rightX = revealX + revealW;
    if (rightX < SCREEN_WIDTH) {
      display.fillRect(rightX, 0, SCREEN_WIDTH - rightX, SCREEN_HEIGHT, SSD1306_BLACK);
    }
    if (revealY > 0) {
      display.fillRect(revealX, 0, revealW, revealY, SSD1306_BLACK);
    }
    int bottomY = revealY + revealH;
    if (bottomY < SCREEN_HEIGHT) {
      display.fillRect(revealX, bottomY, revealW, SCREEN_HEIGHT - bottomY, SSD1306_BLACK);
    }
  } else {
    // Phase 5: sunflower face only, with one blinking eye.
    unsigned long faceElapsed = elapsed - (sunflowerMs + fadeOutMs + zoomMs + holdMs);
    const unsigned long blinkPeriodMs = 900;
    const unsigned long blinkClosedMs = 170;
    bool rightEyeClosed = ((faceElapsed % blinkPeriodMs) < blinkClosedMs);

    int faceCx = 64;
    int faceCy = 31;
    int petalDist = 17;
    int petalR = 4;

    for (int i = 0; i < 14; i++) {
      float a = (6.28318f * i) / 14.0f;
      int px = faceCx + (int)(cos(a) * petalDist);
      int py = faceCy + (int)(sin(a) * petalDist);
      display.fillCircle(px, py, petalR, SSD1306_WHITE);
    }

    display.fillCircle(faceCx, faceCy, 12, SSD1306_WHITE);
    display.fillCircle(faceCx, faceCy, 10, SSD1306_BLACK);

    // Left eye always open.
    display.fillCircle(faceCx - 4, faceCy - 2, 1, SSD1306_WHITE);

    // Right eye blinks.
    if (rightEyeClosed) {
      display.drawFastHLine(faceCx + 2, faceCy - 2, 4, SSD1306_WHITE);
    } else {
      display.fillCircle(faceCx + 4, faceCy - 2, 1, SSD1306_WHITE);
    }

    // Smile
    display.drawLine(faceCx - 4, faceCy + 4, faceCx, faceCy + 6, SSD1306_WHITE);
    display.drawLine(faceCx, faceCy + 6, faceCx + 4, faceCy + 4, SSD1306_WHITE);
  }
}

void drawMoistureSunflowerAnimation() {
  const int displayMoisturePct = moisturePct;
  unsigned long elapsed = millis() - sunflowerAnimStartMs;
  bool rightEyeClosed = ((elapsed % 1200UL) < 180UL);
  int bob = (int)((elapsed / 220UL) % 2UL);

  int faceCx = 96;
  int faceCy = 30 + bob;
  int petalDist = 13;
  int petalR = 3;

  display.clearDisplay();

  // Left area: moisture value text.
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(4, 8);
  display.print("Moist");
  display.setTextSize(2);
  display.setCursor(4, 24);
  if (moistureSensorConnected) {
    display.print(displayMoisturePct);
    display.print('%');
  } else {
    display.print("NC");
  }

  // Right area: sunflower face.
  for (int i = 0; i < 12; i++) {
    float a = (6.28318f * i) / 12.0f;
    int px = faceCx + (int)(cos(a) * petalDist);
    int py = faceCy + (int)(sin(a) * petalDist);
    display.fillCircle(px, py, petalR, SSD1306_WHITE);
  }

  display.fillCircle(faceCx, faceCy, 10, SSD1306_WHITE);
  display.fillCircle(faceCx, faceCy, 8, SSD1306_BLACK);

  // Left eye always open.
  display.fillCircle(faceCx - 3, faceCy - 2, 1, SSD1306_WHITE);

  // Right eye blinks.
  if (rightEyeClosed) {
    display.drawFastHLine(faceCx + 1, faceCy - 2, 4, SSD1306_WHITE);
  } else {
    display.fillCircle(faceCx + 3, faceCy - 2, 1, SSD1306_WHITE);
  }

  // Smile.
  display.drawLine(faceCx - 3, faceCy + 3, faceCx, faceCy + 5, SSD1306_WHITE);
  display.drawLine(faceCx, faceCy + 5, faceCx + 3, faceCy + 3, SSD1306_WHITE);
}

void drawUI() {
  const unsigned long wallpaperCycleMs = 1700UL + 800UL + 1200UL + 1400UL + 2400UL;
  const unsigned long moistureFaceMs = 5000UL;
  const unsigned long fullCycleMs = wallpaperCycleMs + moistureFaceMs;
  unsigned long uiElapsed = (millis() - sunflowerAnimStartMs) % fullCycleMs;

  if (USE_BACKUP_ANIMATION) {
    drawPlantTypeWallpaperBackup();
  } else if (uiElapsed < wallpaperCycleMs) {
    drawFantasticPlantAnimation(uiElapsed);
  } else {
    drawMoistureSunflowerAnimation();
  }
  display.display();
}

void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(PUMP_PIN, OUTPUT);
  digitalWrite(PUMP_PIN, pumpOutputLevel(false));
  pinMode(MANUAL_BUTTON_PIN, INPUT_PULLUP);
  if (USE_TANK_SENSOR) {
    pinMode(TANK_SENSOR_PIN, INPUT_PULLUP);
  }

  // ESP32 default ADC resolution is 12-bit (0..4095)
  analogReadResolution(12);

  loadSettings();

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  if (!initOledDisplay()) {
    Serial.println("SSD1306 init failed. Check wiring/address.");
    while (true) {
      delay(1000);
    }
  }
  Serial.print("[OLED] Ready at 0x");
  if (oledAddrDetected < 16) {
    Serial.print("0");
  }
  Serial.println(oledAddrDetected, HEX);

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("Plant Buddy Booting...");
  display.display();

  sunflowerAnimStartMs = millis();

  // Read initial sample and allow immediate watering decision if needed.
  readMoisturePercent();
  lastWateringMs = millis() - pumpCooldownMs;

  startWifi();
  setupHttpServer();

  Serial.println("Plant monitoring and watering system started.");
}

void loop() {
  server.handleClient();
  handleManualButton();

  unsigned long now = millis();

  if (now - lastSampleMs >= SENSOR_SAMPLE_MS) {
    lastSampleMs = now;
    int pct = readMoisturePercent();

    Serial.print("Moisture: ");
    Serial.print(pct);
    Serial.print("% | Raw: ");
    Serial.print(rawReading);
    Serial.print(" | Sensor: ");
    Serial.print(moistureSensorConnected ? "OK" : "NC");
    Serial.print(" | Pump: ");
    Serial.print(pumpIsOn ? "ON" : "OFF");
    Serial.print(" | GPIO5: ");
    Serial.print(digitalRead(PUMP_PIN) == HIGH ? "HIGH" : "LOW");
    Serial.print(" | Auto: ");
    Serial.println(autoWaterEnabled ? "ON" : "OFF");

    maybeStartAutoWatering();
  }

  maybeStopPump();

  if (now - lastDisplayMs >= DISPLAY_REFRESH_MS) {
    lastDisplayMs = now;
    drawUI();
  }
}
