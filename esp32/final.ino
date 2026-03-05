/*
  ESP32 Fertilizer Mixer + Soil Sensor RS485 (Full Version)

  Features:
  - SoftAP + Captive Portal + DNS
  - LittleFS: index.html, tailwind.js, qrcode.js
  - WebSocket: /ws, /socket, :81/
  - Pumps: N / P / K + Buzzer + LCD 20x4 (I2C)
  - Soil Sensor RS485 (Modbus RTU): N / P / K / pH
  - Smart Mixing: Recipe NPK - Soil NPK → needNPK → scale to batch volume
  - Status LEDs:
      * GREEN: System ready (ON = OK)
      * RED:   OFF = idle, ON = mixing, BLINK = STOP/E-STOP
  - Buttons:
      * BTN1: Select recipe
      * BTN2: Confirm/Start mixing
      * BTN3: Short: change page, Long: E-STOP
      * BTN4: STOP ALL (short press)
*/

struct Recipe;
enum class OwnerX : unsigned char;
struct PumpState;
struct Btn;

#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include "FS.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ================== CONFIG: Soil sensor test mode ==================
#define ENABLE_SOIL_SENSOR_TEST 0   // 1 = print sensor test on Serial, 0 = off

// ================== WiFi / Portal ==================
const char* AP_SSID = "Fertilizer-Mixer";
const char* AP_PASS = "mix12345";
const char* FW_VERSION = "mixer-fw-v6";
IPAddress apIP(192,168,4,1), netMsk(255,255,255,0);

// ================== Buttons ==================
#define BTN1_ACTIVE_LOW 1   // BTN1 (GPIO32) select recipe
#define BTN2_ACTIVE_LOW 1   // BTN2 (GPIO33) confirm/start mixing
#define BTN3_ACTIVE_LOW 1   // BTN3 (GPIO13) short: page, long: E-STOP
#define BTN4_ACTIVE_LOW 1   // BTN4 (GPIO14) STOP ALL (short press)

#define DEBUG_BTN 1

const int BTN1_PIN = 32;
const int BTN2_PIN = 33;
const int BTN3_PIN = 13;
const int BTN4_PIN = 14;

const unsigned long DEBOUNCE_MS = 10;
const unsigned long LONG_MS     = 1200;

// ================== Buzzer ==================
const int  PIN_BUZZER         = 12;
const bool BUZZER_ACTIVE_HIGH = true;
static inline void _bzWrite(bool on){
  if (BUZZER_ACTIVE_HIGH) digitalWrite(PIN_BUZZER, on ? HIGH : LOW);
  else                     digitalWrite(PIN_BUZZER, on ? LOW  : HIGH);
}

// ================== Status LEDs ==================
const int PIN_LED_GREEN = 5;   // GREEN – system ready
const int PIN_LED_RED   = 2;   // RED   – mixing/error (can use onboard LED)

const bool LED_ACTIVE_HIGH = true;

static inline void ledWrite(int pin, bool on){
  if (LED_ACTIVE_HIGH) digitalWrite(pin, on ? HIGH : LOW);
  else                 digitalWrite(pin, on ? LOW  : HIGH);
}

// latched error status for RED blinking
bool errorLatched = false;

// ================== Purge-all state (sequential N->P->K) ==================
// Declared here so emergencyStop() can cancel it.
struct PurgeAllJob {
  bool    active = false;
  float   ml     = 0.0f;
  uint8_t step   = 0; // 0=idle,1=N,2=P,3=K
};
PurgeAllJob purgeAll;

// suppress the generic mix_done triple-beep/reset for special jobs (purge)
bool suppressNextMixDone = false;

// one-shot beep helper (purge done)
uint32_t oneBeepOffAt = 0;

// ================== Pumps / IO ==================
const int PIN_PUMP_N = 25;
const int PIN_PUMP_P = 26;
const int PIN_PUMP_K = 27;
const bool RELAY_ACTIVE_HIGH = true;

// ---- Pump calibration (ml/s) ----
// Defaults (used when no calibration stored)
const float DEFAULT_PUMP_N_ML_PER_S = 1.8f;
const float DEFAULT_PUMP_P_ML_PER_S = 1.6f;
const float DEFAULT_PUMP_K_ML_PER_S = 2.1f;

// Runtime (loaded from NVS/Preferences, can be updated via API/WS)
float pumpRateN = DEFAULT_PUMP_N_ML_PER_S;
float pumpRateP = DEFAULT_PUMP_P_ML_PER_S;
float pumpRateK = DEFAULT_PUMP_K_ML_PER_S;

Preferences calibPrefs;
static const char* CALIB_NS = "pumpcal";

static inline float _clampRate(float v, float defV){
  if(!isfinite(v) || v < 0.05f || v > 50.0f) return defV;
  return v;
}
static inline float getPumpRate(char p){
  p = toupper((unsigned char)p);
  if(p=='N') return pumpRateN;
  if(p=='P') return pumpRateP;
  if(p=='K') return pumpRateK;
  return DEFAULT_PUMP_N_ML_PER_S;
}
static void loadPumpCalibration(){
  calibPrefs.begin(CALIB_NS, true);
  pumpRateN = _clampRate(calibPrefs.getFloat("N", DEFAULT_PUMP_N_ML_PER_S), DEFAULT_PUMP_N_ML_PER_S);
  pumpRateP = _clampRate(calibPrefs.getFloat("P", DEFAULT_PUMP_P_ML_PER_S), DEFAULT_PUMP_P_ML_PER_S);
  pumpRateK = _clampRate(calibPrefs.getFloat("K", DEFAULT_PUMP_K_ML_PER_S), DEFAULT_PUMP_K_ML_PER_S);
  calibPrefs.end();
  Serial.printf("[CALIB] loaded N=%.3f P=%.3f K=%.3f (ml/s)\n",(double)pumpRateN,(double)pumpRateP,(double)pumpRateK);
}
static bool savePumpCalibration(char p, float mlps){
  p = toupper((unsigned char)p);
  if(p!='N' && p!='P' && p!='K') return false;
  mlps = _clampRate(mlps, getPumpRate(p));
  calibPrefs.begin(CALIB_NS, false);
  char key[2] = {p, 0};
  bool ok = calibPrefs.putFloat(key, mlps) > 0;
  calibPrefs.end();
  if(ok){
    if(p=='N') pumpRateN = mlps;
    if(p=='P') pumpRateP = mlps;
    if(p=='K') pumpRateK = mlps;
    Serial.printf("[CALIB] saved %c=%.3f (ml/s)\n", p, (double)mlps);
  }
  return ok;
}
static void resetPumpCalibration(){
  // reset runtime
  pumpRateN = DEFAULT_PUMP_N_ML_PER_S;
  pumpRateP = DEFAULT_PUMP_P_ML_PER_S;
  pumpRateK = DEFAULT_PUMP_K_ML_PER_S;
  // clear stored keys
  calibPrefs.begin(CALIB_NS, false);
  calibPrefs.remove("N");
  calibPrefs.remove("P");
  calibPrefs.remove("K");
  calibPrefs.end();
  Serial.println("[CALIB] reset to defaults");
}
static String pumpCalibrationJson(){
  StaticJsonDocument<128> d;
  d["N"] = pumpRateN;
  d["P"] = pumpRateP;
  d["K"] = pumpRateK;
  String s; serializeJson(d,s);
  return s;
}
// ================== Persistent Mixer Config (NVS) ==================
// These values MUST survive power cycle, per dashboard requirements.
static const char* CFG_NS = "mixcfg";

// default values (safe-ish)
static const float DEFAULT_LINE_HEAD_ML = 40.0f;   // shared for N/P/K
static const float DEFAULT_SPARE_PCT    = 10.0f;   // line spare percent used by dashboard calc

Preferences cfgPrefs;
float cfgLineHeadMl = DEFAULT_LINE_HEAD_ML; // shared
float cfgSparePct   = DEFAULT_SPARE_PCT;

static inline float _clampLineMl(float v){
  if(!isfinite(v) || v < 1.0f || v > 5000.0f) return DEFAULT_LINE_HEAD_ML;
  return v;
}
static inline float _clampPct(float v){
  if(!isfinite(v) || v < 0.0f || v > 100.0f) return DEFAULT_SPARE_PCT;
  return v;
}

static void loadMixerConfig(){
  cfgPrefs.begin(CFG_NS, true);
  cfgLineHeadMl = _clampLineMl(cfgPrefs.getFloat("line_ml", DEFAULT_LINE_HEAD_ML));
  cfgSparePct   = _clampPct   (cfgPrefs.getFloat("spare_pct", DEFAULT_SPARE_PCT));
  cfgPrefs.end();
  Serial.printf("[CFG] loaded line_ml=%.2f spare_pct=%.2f\n",(double)cfgLineHeadMl,(double)cfgSparePct);
}
static void saveMixerConfig(float lineMl, float sparePct){
  cfgLineHeadMl = _clampLineMl(lineMl);
  cfgSparePct   = _clampPct(sparePct);
  cfgPrefs.begin(CFG_NS, false);
  cfgPrefs.putFloat("line_ml",   cfgLineHeadMl);
  cfgPrefs.putFloat("spare_pct", cfgSparePct);
  cfgPrefs.end();
  Serial.printf("[CFG] saved  line_ml=%.2f spare_pct=%.2f\n",(double)cfgLineHeadMl,(double)cfgSparePct);
}

static String mixerConfigJson(){
  StaticJsonDocument<256> d;
  d["line_ml"]   = cfgLineHeadMl;
  d["spare_pct"] = cfgSparePct;
  String s; serializeJson(d,s);
  return s;
}

// ================== Persistent Notifications (Plots) ==================
// Store plot states + recent logs on ESP32 (NVS) so multiple devices can see the same history.
// Data format (stored as one JSON string under key "data"):
// {
//   "plots": {
//     "<plotName>": {"round":1,"done":false,"ts":1700,"crop":"...",
//                    "soil":{"N":0,"P":0,"K":0},
//                    "dose":{"N":0,"P":0,"K":0,"T":0}}
//   },
//   "logs": [ {record}, ... ]   // newest first, capped
// }
static const char* NOTIFY_NS = "notify";
static const char* NOTIFY_KEY = "data";
static const int   NOTIFY_MAX_LOGS = 40;
Preferences notifyPrefs;

// ================== UI State (persist last changed UI values) ==================
// Stored in NVS so any phone/browser sees the same last UI settings.
// Schema is flexible JSON (string) sent from dashboard.
static const char* UI_NS  = "ui";
static const char* UI_KEY = "state";
static const size_t UI_STATE_MAX = 4096; // bytes (keep small)
Preferences uiPrefs;
static String uiStateCache = "{}";

static void uiStateLoad(){
  uiPrefs.begin(UI_NS, true);
  uiStateCache = uiPrefs.getString(UI_KEY, "{}");
  uiPrefs.end();
  if(uiStateCache.length()==0) uiStateCache="{}";
  if(uiStateCache.length() > UI_STATE_MAX) uiStateCache = "{}";
}
static bool uiStateSave(const String& json){
  String s = json;
  if(s.length()==0) s="{}";
  if(s.length() > UI_STATE_MAX) return false;
  uiPrefs.begin(UI_NS, false);
  bool ok = uiPrefs.putString(UI_KEY, s) > 0;
  uiPrefs.end();
  if(ok) uiStateCache = s;
  return ok;
}
static String uiStateGet(){
  if(uiStateCache.length()==0) uiStateCache="{}";
  return uiStateCache;
}



// ---- UTF-8 helpers (avoid cutting Thai chars into �) ----
static inline bool _isUtf8Cont(uint8_t c){ return (c & 0xC0) == 0x80; }

// Drop invalid UTF-8 sequences; keep ASCII + valid UTF-8 bytes.
static String _utf8Clean(const String& in){
  const uint8_t* b = (const uint8_t*)in.c_str();
  size_t n = in.length();
  String out; out.reserve(n);
  size_t i=0;
  while(i<n){
    uint8_t c=b[i];
    if(c < 0x80){ out += (char)c; i++; continue; }
    size_t need=0;
    if((c & 0xE0) == 0xC0) need=2;
    else if((c & 0xF0) == 0xE0) need=3;
    else if((c & 0xF8) == 0xF0) need=4;
    else { i++; continue; } // invalid lead byte
    if(i + need > n) break;
    bool ok=true;
    for(size_t k=1;k<need;k++){ if(!_isUtf8Cont(b[i+k])){ ok=false; break; } }
    if(!ok){ i++; continue; }
    out.concat(in.substring(i, i+need));
    i += need;
  }
  return out;
}

// Truncate by byte-length, but never cut inside a UTF-8 character.
static String _utf8TruncBytes(const String& in, size_t maxBytes){
  if(in.length() <= maxBytes) return in;
  const uint8_t* b = (const uint8_t*)in.c_str();
  size_t n = in.length();
  size_t i=0;
  String out; out.reserve(maxBytes);
  while(i<n){
    uint8_t c=b[i];
    size_t need=1;
    if(c < 0x80) need=1;
    else if((c & 0xE0) == 0xC0) need=2;
    else if((c & 0xF0) == 0xE0) need=3;
    else if((c & 0xF8) == 0xF0) need=4;
    else { // invalid
      if(out.length()+1 > maxBytes) break;
      out += '?'; i++; continue;
    }
    if(i+need > n) break;
    // ensure continuation bytes valid
    bool ok=true;
    for(size_t k=1;k<need;k++){ if(!_isUtf8Cont(b[i+k])){ ok=false; break; } }
    if(!ok){
      if(out.length()+1 > maxBytes) break;
      out += '?'; i++; continue;
    }
    if(out.length()+need > maxBytes) break;
    out.concat(in.substring(i, i+need));
    i += need;
  }
  return out;
}

static String _normPlotName(const String& in){
  // trim + collapse whitespace (ASCII spaces)
  String s = _utf8Clean(in);
  s.trim();

  String out; out.reserve(s.length());
  bool prevSpace=false;
  for(size_t i=0;i<s.length();++i){
    char c = s[i];
    bool isSpace = (c==' ' || c=='\t' || c=='\n' || c=='\r');
    if(isSpace){
      if(!prevSpace){ out += ' '; prevSpace=true; }
    } else {
      out += c; prevSpace=false;
    }
  }
  out.trim();

  // remove decoration markers sometimes appended by UI (e.g., skipped marker ♦/◆)
  while(out.endsWith("♦") || out.endsWith("◆")){
    out.remove(out.length()-1);
    out.trim();
  }

  if(out.length()==0) out = String("ไม่ระบุแปลง");

  // IMPORTANT: limit by bytes, but keep UTF-8 intact
  // 64 bytes ~ 21 Thai chars (enough for typical plot names, avoids truncating "ไม่ระบุแปลง")
  out = _utf8TruncBytes(out, 64);

  return out;
}

static String _loadNotifyJson(){
  notifyPrefs.begin(NOTIFY_NS, true);
  String s = notifyPrefs.getString(NOTIFY_KEY, "");
  notifyPrefs.end();
  if(s.length()==0){
    // minimal empty structure
    StaticJsonDocument<64> d;
    d.createNestedObject("plots");
    d.createNestedArray("logs");
    String out; serializeJson(d,out);
    return out;
  }
  return s;
}

static void _saveNotifyJson(const String& s){
  notifyPrefs.begin(NOTIFY_NS, false);
  notifyPrefs.putString(NOTIFY_KEY, s);
  notifyPrefs.end();
}

static bool _parseNotify(DynamicJsonDocument& d){
  String s = _loadNotifyJson();
  auto err = deserializeJson(d, s);
  if(err){
    d.clear();
    d.createNestedObject("plots");
    d.createNestedArray("logs");
    return false;
  }
  if(!d.containsKey("plots")) d.createNestedObject("plots");
  if(!d.containsKey("logs"))  d.createNestedArray("logs");
  return true;
}

static void _commitNotify(DynamicJsonDocument& d){
  String out; serializeJson(d,out);
  _saveNotifyJson(out);
}

static void notifyUpsertLog(const JsonVariantConst& recIn){
  DynamicJsonDocument d(8192);
  _parseNotify(d);

  JsonObject plots = d["plots"].as<JsonObject>();
  JsonArray  logs  = d["logs"].as<JsonArray>();

  String plotName = _normPlotName(String((const char*)(recIn["plotName"] | "")));
  String crop     = String((const char*)(recIn["crop"] | ""));
  if(crop.length()>24) crop = crop.substring(0,24);
  int round        = (int)(recIn["round"] | 1);
  int roundTotal   = (int)(recIn["roundTotal"] | 3);
  bool done        = (bool)(recIn["done"] | false);
  uint32_t ts      = (uint32_t)(recIn["ts"] | (uint32_t)(millis()/1000));
  if(roundTotal<=0) roundTotal=3;
  if(round<0) round=0;
  if(round>roundTotal) round=roundTotal;
  if(round>=roundTotal) done = true;

  // Build a compact record to store
  StaticJsonDocument<512> r;
  r["plotName"]  = plotName;
  r["crop"]      = crop;
  r["round"]     = round;
  r["roundTotal"] = roundTotal;
  r["done"]      = done;
  r["ts"]        = ts;
  // soilBefore (accept both n/p/k and N/P/K from UI)
  JsonObject soil = r.createNestedObject("soil");
  soil["N"] = (int)(recIn["soilBefore"]["n"] | recIn["soilBefore"]["N"] | recIn["soil"]["n"] | recIn["soil"]["N"] | 0);
  soil["P"] = (int)(recIn["soilBefore"]["p"] | recIn["soilBefore"]["P"] | recIn["soil"]["p"] | recIn["soil"]["P"] | 0);
  soil["K"] = (int)(recIn["soilBefore"]["k"] | recIn["soilBefore"]["K"] | recIn["soil"]["k"] | recIn["soil"]["K"] | 0);

  // dose pumped (accept both total/T; if missing, compute N+P+K)
  JsonObject dose = r.createNestedObject("dose");
  double dn = (double)(recIn["dosePumped"]["N"] | recIn["dose"]["N"] | 0.0);
  double dp = (double)(recIn["dosePumped"]["P"] | recIn["dose"]["P"] | 0.0);
  double dk = (double)(recIn["dosePumped"]["K"] | recIn["dose"]["K"] | 0.0);
  double dt = (double)(recIn["dosePumped"]["total"] | recIn["dosePumped"]["T"] | recIn["dose"]["total"] | recIn["dose"]["T"] | (dn+dp+dk));
  dose["N"] = dn;
  dose["P"] = dp;
  dose["K"] = dk;
  dose["T"] = dt;

  // Update plot state
  JsonObject st = plots[plotName].to<JsonObject>();
  st["round"] = round;
  st["roundTotal"] = roundTotal;
  st["done"]  = done;
  st["ts"]    = ts;
  st["crop"]  = crop;
  st["soil"]  = soil;
  st["dose"]  = dose;

  // Push to logs (newest first)
  JsonArray newLogs = d.createNestedArray("_tmp");
  newLogs.add(r);
  for(JsonVariant v : logs){
    if((int)newLogs.size() >= NOTIFY_MAX_LOGS) break;
    // avoid duplicate identical top entry
    newLogs.add(v);
  }
  d.remove("logs");
  d["logs"] = newLogs;
  d.remove("_tmp");

  _commitNotify(d);
}

static bool notifyDeletePlot(const String& rawName){
  String normName = _normPlotName(rawName);

  DynamicJsonDocument d(8192);
  _parseNotify(d);
  JsonObject plots = d["plots"].as<JsonObject>();

  // Choose actual key in storage (may include legacy suffixes)
  String keyName = normName;

  // Backward compatibility: older UI versions appended ♦/◆ into stored keys.
  String legacy1 = normName + "♦";
  String legacy2 = normName + "◆";

  if(!plots.containsKey(keyName)){
    if(plots.containsKey(legacy1)) keyName = legacy1;
    else if(plots.containsKey(legacy2)) keyName = legacy2;
    else return false;
  }

  // remove plot state
  plots.remove(keyName);

  // filter logs by normalized plotName (so legacy keys still delete logs)
  JsonArray logs = d["logs"].as<JsonArray>();
  JsonArray kept = d.createNestedArray("_tmp");
  for(JsonVariant v : logs){
    const char* pn = v["plotName"] | "";
    String pnNorm = _normPlotName(String(pn));
    if(pnNorm != normName) kept.add(v);
  }
  d.remove("logs");
  d["logs"] = kept;
  d.remove("_tmp");

  _commitNotify(d);
  return true;
}

static String notifyPlotsJson(){
  DynamicJsonDocument d(8192);
  _parseNotify(d);
  JsonObject plots = d["plots"].as<JsonObject>();
  StaticJsonDocument<4096> out;
  JsonArray arr = out.createNestedArray("plots");
  for(JsonPair kv : plots){
    JsonObject st = kv.value().as<JsonObject>();
    JsonObject o = arr.createNestedObject();
    o["plotName"] = kv.key().c_str();
    o["round"] = (int)(st["round"] | 0);
    o["roundTotal"] = (int)(st["roundTotal"] | 3);
    o["done"]  = (bool)(st["done"] | false);
    o["ts"]    = (uint32_t)(st["ts"] | 0);
    o["crop"]  = (const char*)(st["crop"] | "");
  }
  out["ok"] = true;
  String s; serializeJson(out,s);
  return s;
}

static String notifyPlotDetailJson(const String& rawName){
  String plotName = _normPlotName(rawName);
  DynamicJsonDocument d(8192);
  _parseNotify(d);
  JsonObject plots = d["plots"].as<JsonObject>();
  // Backward compatibility: older UI versions appended ♦/◆ into stored keys.
  String legacy1 = plotName + "♦";
  String legacy2 = plotName + "◆";
  if(!plots.containsKey(plotName)){
    if(plots.containsKey(legacy1)) plotName = legacy1;
    else if(plots.containsKey(legacy2)) plotName = legacy2;
    else return String("{\"ok\":false,\"error\":\"not_found\"}");
  }
  StaticJsonDocument<2048> out;
  out["ok"] = true;
  out["plotName"] = plotName;
  out["state"] = plots[plotName];
  String s; serializeJson(out,s);
  return s;
}


// ================== LCD ==================
const uint32_t DISPLAY_REFRESH_MS = 120;

// Volume for BTN2 hardware mixing
#define CONFIRM_MIX_VOL_ML 30

// scale N/P/K to batch volume
#define SCALE_BTN_BY_VOLUME 1

// Use soil correction (recipe - soil) when mixing by buttons (Smart mode)
#define USE_SOIL_CORRECTION_FOR_BTN 1

// default manual duration (sec) for REST/manual
#define MANUAL_DEFAULT_SEC 3

// ================== RS485 Soil Sensor (Modbus RTU) ==================
const int RS485_RX_PIN     = 17;  // RO -> GPIO17
const int RS485_TX_PIN     = 16;  // DI -> GPIO16
const int RS485_DE_RE_PIN  = 4;   // DE+RE -> GPIO4

const uint8_t  SOIL_SENSOR_ID   = 0x01;
const uint32_t SOIL_BAUD        = 4800;
const uint8_t  SOIL_BYTE_FORMAT = SERIAL_8N1;

const uint32_t SOIL_POLL_INTERVAL_MS = 2000;

// register map
const uint16_t REG_SOIL_PH = 0x0003;   // pH * 10
const uint16_t REG_SOIL_N  = 0x0004;   // N
const uint16_t REG_SOIL_P  = 0x0005;   // P
const uint16_t REG_SOIL_K  = 0x0006;   // K

// pH calibration
const float PH_SLOPE  = 1.0f;
const float PH_OFFSET = 0.0f;

// ================== Globals ==================
DNSServer dns;
AsyncWebServer server80(80);
AsyncWebServer server81(81);

AsyncWebSocket ws80("/ws");
AsyncWebSocket ws80_alt("/socket");
AsyncWebSocket ws81_root("/");

struct PumpState {
  bool  on        = false;
  float targetMl  = 0;
  float dispensedMl = 0;
  float rateMlPerS  = 0;
  int   pin         = -1;
};

PumpState pumpN{false, 0, 0, 0, PIN_PUMP_N};
PumpState pumpP{false, 0, 0, 0, PIN_PUMP_P};
PumpState pumpK{false, 0, 0, 0, PIN_PUMP_K};

unsigned long lastMsN=0, lastMsP=0, lastMsK=0;

struct Btn {
  int pin;
  bool activeLow;
  uint8_t last=0;
  uint8_t cur=0;
  uint32_t t=0;
  bool changed=false;
};

Btn btn1{BTN1_PIN, BTN1_ACTIVE_LOW};
Btn btn2{BTN2_PIN, BTN2_ACTIVE_LOW};
Btn btn3{BTN3_PIN, BTN3_ACTIVE_LOW};
Btn btn4{BTN4_PIN, BTN4_ACTIVE_LOW};

uint8_t LCD_ADDR = 0x27;
LiquidCrystal_I2C* lcd = nullptr;
bool lcdReady = false;
uint8_t displayPage = 0;
uint32_t lastDisplayMs = 0;
String _rowCache[4] = {"","","",""};

// raw soil values (LCD & logic)
float soilN  = 40;
float soilP  = 12;
float soilK  = 30;
float soilPH = 6.0f;

// averaged soil values (dashboard)
float soilN_avg  = soilN;
float soilP_avg  = soilP;
float soilK_avg  = soilK;
float soilPH_avg = soilPH;
bool  soilAvgInit = false;

// smoothing factors
const float SOIL_FILTER_ALPHA_NPK = 0.3f;
const float SOIL_FILTER_ALPHA_PH  = 0.2f;

struct Recipe { const char* key; const char* name; int n,p,k; const char* ph; };
Recipe RECIPES[] = {
  {"rice","RICE",120,60,60,"5.5-6.5"},
  {"spinach","SPINACH",80,40,80,"6.0-7.0"},
  {"chili","CHILI",120,90,120,"5.5-6.8"},
  {"corn","CORN",160,80,80,"5.5-7.0"},
  {"cassava","CASSAVA",80,40,120,"4.5-6.5"},
  {"sugarcane","SUGARCANE",150,60,120,"5.5-7.5"},
  {"rubber","RUBBER",120,80,100,"4.5-5.5"},
  {"oilpalm","OIL PALM",180,90,200,"4.5-6.0"},
  {"durian","DURIAN",150,50,150,"5.5-6.5"},
  {"mango","MANGO",100,50,100,"6.0-7.0"},
};
const int RECIPE_COUNT = sizeof(RECIPES)/sizeof(RECIPES[0]);
const Recipe* recipeByIndex(int idx){
  return (idx>=0 && idx<RECIPE_COUNT) ? &RECIPES[idx] : nullptr;
}
int pendingIdx = 0;

struct UITarget {
  float n=0, p=0, k=0;
  float vol_ml=0;
  String ph="";
  bool valid=false;
} ui;

enum class OwnerX : unsigned char { None=0, Btn, Web };
OwnerX activeOwner = OwnerX::None;
const bool ALLOW_PREEMPT = false;


// ================== PRO HARDENING (state, timeouts, diag, token, log) ==================
// NOTE: Arduino IDE auto-generates function prototypes near the top of the sketch.
// Using an enum type in function signatures can break compilation if the enum is declared later.
// To keep this sketch robust without reordering the whole file, we represent job state as uint8_t.
static const uint8_t JS_IDLE     = 0;
static const uint8_t JS_MIXING   = 1;
static const uint8_t JS_PURGING  = 2;
static const uint8_t JS_MANUAL   = 3;
static const uint8_t JS_STOPPING = 4;
static const uint8_t JS_ERROR    = 5;

static uint8_t jobState = JS_IDLE;

static uint32_t mixDeadlineMs   = 0;   // 0 = none
static uint32_t purgeDeadlineMs = 0;   // 0 = none

static String lastError = "";

// Simple session token (optional). Backward compatible: by default we do NOT require token.
static String sessionToken = "";
static const bool REQUIRE_TOKEN = false;

// ----- Ring buffer event log (last ~40 events) -----
static const uint8_t LOG_CAP = 40;
static String logBuf[LOG_CAP];
static uint8_t logHead = 0;
static uint8_t logCount = 0;

static void logEvent(const String& s){
  uint32_t now = millis();
  String line = String(now) + "ms " + s;
  logBuf[logHead] = line;
  logHead = (uint8_t)((logHead + 1) % LOG_CAP);
  if(logCount < LOG_CAP) logCount++;
  // also mirror to Serial (lightweight)
  Serial.println("[EV] " + line);
}

// NOTE: name chosen to avoid collision with any existing identifiers in user's sketch
static const char* jobStateName(uint8_t st){
  switch(st){
    case JS_IDLE:     return "IDLE";
    case JS_MIXING:   return "MIXING";
    case JS_PURGING:  return "PURGING";
    case JS_MANUAL:   return "MANUAL";
    case JS_STOPPING: return "STOPPING";
    case JS_ERROR:    return "ERROR";
    default: return "UNKNOWN";
  }
}

static void setState(uint8_t st, const char* why=""){
  jobState = st;
  if(why && *why) logEvent(String("state=") + jobStateName(st) + " why=" + why);
  else logEvent(String("state=") + jobStateName(st));
}

static void setError(const char* err){
  lastError = err ? String(err) : String("error");
  errorLatched = true;
  setState(JS_ERROR, lastError.c_str());
}

// Forward declaration: helper lives later in the original sketch; required for strict C++ ordering.
static String paramS(AsyncWebServerRequest* r, const char* key, const String& def);

static bool tokenOk(AsyncWebServerRequest* r){
  if(!REQUIRE_TOKEN) return true;
  // Accept ?token=... (query) or header X-Token
  String t = paramS(r, "token", "");
  if(!t.length() && r->hasHeader("X-Token")) t = r->getHeader("X-Token")->value();
  return t.length() && t == sessionToken;
}

static bool tokenOkJson(const JsonDocument& d){
  if(!REQUIRE_TOKEN) return true;
  const char* t = d["token"] | d["session_token"] | "";
  return (t && *t) && (sessionToken.length() && sessionToken == String(t));
}

bool     wsClientConnected = false;
uint32_t lastPush = 0, lastStep = 0;

struct ManualJob {
  PumpState*    ps      = nullptr;
  unsigned long until   = 0;
  float         prevRate= 0;
  bool          prevOn  = false;
  unsigned long* lastPtr= nullptr;
  float         defRate = 0;
} manual;

// Stop an active manual job for a specific pump (used by web calibration "หยุด")
// Returns true if it stopped a running manual job for that pump.
bool stopManualJobForPump(char pumpChar){
  pumpChar = toupper((unsigned char)pumpChar);
  PumpState* ps = nullptr;
  if(pumpChar=='N') ps=&pumpN;
  else if(pumpChar=='P') ps=&pumpP;
  else if(pumpChar=='K') ps=&pumpK;
  else return false;

  if(manual.ps != ps) return false; // only stop if this pump is currently the manual job

  // Restore previous state (if it was running before manual), otherwise stop.
  if(!manual.prevOn) pumpStop(*manual.ps);
  else{
    manual.ps->on = true;
    manual.ps->rateMlPerS = manual.prevRate;
    setPumpPin(manual.ps->pin, true);
  }
  manual.ps = nullptr;
  wsBroadcast();
  Serial.printf("[MANUAL] stop request pump=%c\n", pumpChar);
  return true;
}

bool     wasMixing    = false;
bool     pendingReset = false;
uint32_t resetAt      = 0;

// ===== Buzzer triple beep =====
struct BuzzSeq { bool active=false; uint8_t step=0; uint32_t nextAt=0; } buzz;
void buzzerOn()  { _bzWrite(true); }
void buzzerOff() { _bzWrite(false); }
void buzzerStartTriple(){ buzz.active=true; buzz.step=0; buzz.nextAt=0; }
void buzzerUpdate(){
  if(!buzz.active) return;
  uint32_t now = millis();
  if ((long)(now - buzz.nextAt) < 0) return;
  switch (buzz.step) {
    case 0: buzzerOn();  buzz.nextAt = now + 120; buzz.step = 1; break;
    case 1: buzzerOff(); buzz.nextAt = now + 120; buzz.step = 2; break;
    case 2: buzzerOn();  buzz.nextAt = now + 120; buzz.step = 3; break;
    case 3: buzzerOff(); buzz.nextAt = now + 120; buzz.step = 4; break;
    case 4: buzzerOn();  buzz.nextAt = now + 120; buzz.step = 5; break;
    case 5: buzzerOff(); buzz.active = false; break;
  }
}

// ================== Utils ==================
String mimeOf(const String& p){
  if(p.endsWith(".html")) return "text/html; charset=utf-8";
  if(p.endsWith(".js"))   return "application/javascript; charset=utf-8";
  if(p.endsWith(".css"))  return "text/css; charset=utf-8";
  if(p.endsWith(".png"))  return "image/png";
  if(p.endsWith(".jpg"))  return "image/jpeg";
  if(p.endsWith(".svg"))  return "image/svg+xml";
  return "text/plain; charset=utf-8";
}
void serveFile(AsyncWebServerRequest* req, String path){
  if(path.endsWith("/")) path += "index.html";
  if(!LittleFS.exists(path)){ req->send(404,"text/plain","Not Found"); return; }
  auto *res = req->beginResponse(LittleFS, path, mimeOf(path));
  if(path.endsWith(".js")) res->addHeader("Cache-Control","max-age=3600");
  if(path.endsWith(".html")) { res->addHeader("Cache-Control","no-store"); res->addHeader("Pragma","no-cache"); }
  req->send(res);
}

void captiveLanding(AsyncWebServerRequest* req){
  String html = "<!doctype html><html><head>"
                "<meta http-equiv='refresh' content='0;url=http://"+apIP.toString()+"/'>"
                "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                "</head><body>Connecting...</body></html>";
  req->send(200, "text/html; charset=utf-8", html);
}

uint8_t detectI2CAddress(){
  uint8_t a[2]={0x27,0x3F};
  for(uint8_t i: a){
    Wire.beginTransmission(i);
    if(!Wire.endTransmission()) return i;
  }
  return 0x27;
}

String fit20(const String &s){ return s.length()<=20 ? s : s.substring(0,20); }
void setRow(uint8_t row,const String& text){
  if(!lcdReady) return;
  String t=fit20(text);
  if(_rowCache[row]==t) return;
  _rowCache[row]=t;
  lcd->setCursor(0,row);
  lcd->print(t);
  for(int i=t.length(); i<20; ++i) lcd->print(' ');
}
String kv(String label, String value){
  int fixed=label.length()+3;
  if(fixed>=20){
    label=label.substring(0,max(0,20-3));
    fixed=label.length()+3;
  }
  int avail=20-fixed;
  if(avail<(int)value.length()) value=value.substring(0,avail);
  return label+" : "+value;
}

float totalTargetMl(){
  return max(0.0f,pumpN.targetMl)+max(0.0f,pumpP.targetMl)+max(0.0f,pumpK.targetMl);
}
float totalDispensedMl(){
  return max(0.0f,pumpN.dispensedMl)+max(0.0f,pumpP.dispensedMl)+max(0.0f,pumpK.dispensedMl);
}
void drawProgress(uint8_t row){
  float tgt=totalTargetMl(), cur=totalDispensedMl();
  if(tgt<=0.0f){ setRow(row,"IDLE"); return; }
  float r=cur/tgt; r = r<0?0:(r>1?1:r);
  int filled=(int)round(r*10.0f);
  String bar="["; for(int i=0;i<10;i++) bar+=(i<filled?(char)255:'-'); bar+="]";
  char pct[8]; snprintf(pct,sizeof(pct),"%3d%%",(int)round(r*100.0f));
  int spaces=max(0,20-(int)bar.length()-1-(int)strlen(pct));
  String line=bar;
  for(int i=0;i<spaces;i++) line+=' ';
  line+=' ';
  line+=pct;
  setRow(row,line);
}

static float paramF(AsyncWebServerRequest* r, const char* k, float def=0){
  if(r->hasParam(k,true))  return r->getParam(k,true)->value().toFloat();
  if(r->hasParam(k,false)) return r->getParam(k,false)->value().toFloat();
  return def;
}
static int paramI(AsyncWebServerRequest* r, const char* k, int def=0){
  return (int)round(paramF(r,k,(float)def));
}
static String paramS(AsyncWebServerRequest* r, const char* k, const String& def=""){
  if(r->hasParam(k,true))  return r->getParam(k,true)->value();
  if(r->hasParam(k,false)) return r->getParam(k,false)->value();
  return def;
}

// ================== Pump control ==================
void setPumpPin(int pin,bool on){
  if(RELAY_ACTIVE_HIGH) digitalWrite(pin,on?HIGH:LOW);
  else                  digitalWrite(pin,on?LOW:HIGH);
}
void pumpStop(PumpState &p){
  p.on=false;
  p.rateMlPerS=0;
  setPumpPin(p.pin,false);
}
void pumpStart(PumpState &p,float rate){
  if(rate<=0){
    if(&p==&pumpN)      rate=getPumpRate('N');
    else if(&p==&pumpP) rate=getPumpRate('P');
    else                rate=getPumpRate('K');
  }
  p.on=true;
  p.rateMlPerS=rate;
  setPumpPin(p.pin,true);
}

void updatePumpDispensed(PumpState &p,unsigned long &lastMs,float rateDefault){
  unsigned long now=millis();
  if(lastMs==0) lastMs=now;
  if(!p.on){ lastMs=now; return; }

  float dt=(now-lastMs)/1000.0f;
  lastMs=now;
  float rate=(p.rateMlPerS>0?p.rateMlPerS:rateDefault);
  p.dispensedMl += rate*dt;

  bool isManualThisPump = (manual.ps == &p);

  if(!isManualThisPump && p.targetMl>0){
    if(p.dispensedMl >= p.targetMl){
      p.dispensedMl = p.targetMl;
      pumpStop(p);
    }
  }
}

// ================== Mixing logic ==================
void uiSetTargets(float n,float p,float k,float vol,const char* ph){
  ui.n=n; ui.p=p; ui.k=k; ui.vol_ml=vol; ui.ph=ph?String(ph):""; ui.valid=true;
}
bool mixingActive(){
  return (pumpN.on||pumpP.on||pumpK.on) ||
         ((pumpN.targetMl>0||pumpP.targetMl>0||pumpK.targetMl>0) &&
          (totalDispensedMl()<totalTargetMl()));
}
bool canStartJobX(OwnerX who){
  if(!mixingActive()) return true;
  if(activeOwner==who) return true;
  return ALLOW_PREEMPT;
}

// Smart mixing: (recipe - soil) → needNPK → scaled to batch volume
void startMixByRecipe(const Recipe& r, float volumeMl, OwnerX who){
  if(!canStartJobX(who)) return;

  errorLatched = false;   // new mixing round clears previous error
  activeOwner = who;

  // 1) required nutrients: recipe - soil
  float needN = max(0, r.n - (int)soilN);
  float needP = max(0, r.p - (int)soilP);
  float needK = max(0, r.k - (int)soilK);

  // if soil already sufficient
  if(needN <= 0 && needP <= 0 && needK <= 0){
    uiSetTargets(0,0,0,volumeMl,r.ph);
    pumpN.targetMl = pumpP.targetMl = pumpK.targetMl = 0;
    pumpStop(pumpN); pumpStop(pumpP); pumpStop(pumpK);
    return;
  }

  // 2) display formula after soil correction on LCD (TGT page)
  uiSetTargets(needN, needP, needK, volumeMl, r.ph);

  // 3) use needN/needP/needK as ratio, scale to batch volume
  float addN = needN;
  float addP = needP;
  float addK = needK;

#if SCALE_BTN_BY_VOLUME
  {
    float sum = addN + addP + addK;
    if (volumeMl > 0 && sum > 0) {
      float s = volumeMl / sum;
      auto clip = [](float x){ return (x < 0.05f) ? 0.0f : x; };
      addN = clip(addN * s);
      addP = clip(addP * s);
      addK = clip(addK * s);
    } else {
      addN = addP = addK = 0;
    }
  }
#endif

  // 4) assign pump targets (ml)
  pumpN.targetMl=addN; pumpN.dispensedMl=0;
  pumpP.targetMl=addP; pumpP.dispensedMl=0;
  pumpK.targetMl=addK; pumpK.dispensedMl=0;

  if(addN>0) pumpStart(pumpN,0); else pumpStop(pumpN);
  if(addP>0) pumpStart(pumpP,0); else pumpStop(pumpP);
  if(addK>0) pumpStart(pumpK,0); else pumpStop(pumpK);

  // Pro: overall mixing timeout (prevents stuck pumps)
  {
    float tN = (addN>0)? (addN / max(0.01f, getPumpRate('N'))) : 0.0f;
    float tP = (addP>0)? (addP / max(0.01f, getPumpRate('P'))) : 0.0f;
    float tK = (addK>0)? (addK / max(0.01f, getPumpRate('K'))) : 0.0f;
    float tMax = max(tN, max(tP, tK));
    mixDeadlineMs = millis() + (uint32_t)((tMax + 8.0f) * 1000.0f); // + buffer
  }
  lastError = "";
  setState(JS_MIXING, (who==OwnerX::Btn?"mix_btn":"mix_web"));
}

void emergencyStop(const char* reason=""){
  // Pro: record reason + state
  if(reason && *reason){ lastError = String(reason); }
  setState(JS_STOPPING, reason && *reason ? reason : "stop");
  // cancel any special jobs
  purgeAll.active = false;
  purgeAll.step   = 0;
  if(manual.ps){
    // stop manual job immediately and don't restore previous state
    pumpStop(*manual.ps);
    manual.ps = nullptr;
  }
  buzz.active = false;
  oneBeepOffAt = 0;
  buzzerOff();

  pumpStop(pumpN); pumpStop(pumpP); pumpStop(pumpK);
  pumpN.targetMl=pumpP.targetMl=pumpK.targetMl=0;
  pumpN.dispensedMl=pumpP.dispensedMl=pumpK.dispensedMl=0;
  ui.valid=false;
  activeOwner=OwnerX::None;

  // latch error → red LED blinking
  errorLatched = true;
  setState(JS_ERROR, lastError.length()? lastError.c_str() : "stopped");
}


// ================== WebSocket helpers ==================
void wsSendAll(const String& s){
  ws80.textAll(s);
  ws80_alt.textAll(s);
  ws81_root.textAll(s);
}
void wsBroadcast(){
  StaticJsonDocument<768> doc;

  // averaged values for dashboard
  doc["n"]  = soilN_avg;
  doc["p"]  = soilP_avg;
  doc["k"]  = soilK_avg;
  doc["ph"] = soilPH_avg;

  // raw values for debug
  doc["n_raw"]  = soilN;
  doc["p_raw"]  = soilP;
  doc["k_raw"]  = soilK;
  doc["ph_raw"] = soilPH;

  // pump calibration rates (ml/s)
  auto jc=doc.createNestedObject("calib"); jc["N"]=pumpRateN; jc["P"]=pumpRateP; jc["K"]=pumpRateK;
  auto cfg=doc.createNestedObject("cfg"); cfg["line_ml"]=cfgLineHeadMl; cfg["spare_pct"]=cfgSparePct;
  doc["error"]=errorLatched;
  doc["state"] = jobStateName(jobState);
  doc["owner"] = (activeOwner==OwnerX::Btn?"BTN":(activeOwner==OwnerX::Web?"WEB":"NONE"));
  if(lastError.length()) doc["last_error"] = lastError;
  doc["token_required"] = REQUIRE_TOKEN;

  doc["pumps_on"] = (pumpN.on||pumpP.on||pumpK.on);
  doc["manual_active"] = (manual.ps!=nullptr);

  auto jN=doc.createNestedObject("pumpN"); jN["on"]=pumpN.on; jN["targetMl"]=pumpN.targetMl; jN["dispensedMl"]=pumpN.dispensedMl;
  auto jP=doc.createNestedObject("pumpP"); jP["on"]=pumpP.on; jP["targetMl"]=pumpP.targetMl; jP["dispensedMl"]=pumpP.dispensedMl;
  auto jK=doc.createNestedObject("pumpK"); jK["on"]=pumpK.on; jK["targetMl"]=pumpK.targetMl; jK["dispensedMl"]=pumpK.dispensedMl;
  const char* ownerStr = (activeOwner==OwnerX::Btn? "BTN" :
                         (activeOwner==OwnerX::Web? "WEB" : "NONE"));
  doc["owner"]=ownerStr;
  String out; serializeJson(doc,out);
  wsSendAll(out);
}
void wsAck(AsyncWebSocketClient* c,const char* status,const char* msg){
  StaticJsonDocument<160> d;
  d["status"]=status;
  d["msg"]=msg;
  String s; serializeJson(d,s);
  if(c) c->text(s);
}

// ================== Purge-all job logic (sequential N->P->K) ==================
// index.html sends {command:'purge_all', ml:<mL>}.
// We run N -> P -> K sequentially to avoid duplicated runs (dashboard fallback).
static inline void buzzerBeepOnce(uint16_t ms=120){
  buzzerOn();
  oneBeepOffAt = millis() + (uint32_t)ms;
}

static inline bool pumpFinished(const PumpState& p){
  return (!p.on) && (p.targetMl>0) && (p.dispensedMl >= p.targetMl - 0.0001f);
}

static void purgeAllStart(float ml){
  purgeAll.active = true;
  purgeAll.ml = ml;
  purgeAll.step = 1;

  setState(JS_PURGING, "purge_all_start");
  purgeDeadlineMs = millis() + (uint32_t)(
    (ml / max(0.01f, getPumpRate('N')) +
     ml / max(0.01f, getPumpRate('P')) +
     ml / max(0.01f, getPumpRate('K')) + 5.0f) * 1000.0f
  );
  suppressNextMixDone = true; // purge completion shouldn't trigger mix_done triple beep

  // set targets for ALL lines (so mixingActive stays true across steps)
  pumpN.targetMl = ml; pumpN.dispensedMl = 0; pumpStop(pumpN);
  pumpP.targetMl = ml; pumpP.dispensedMl = 0; pumpStop(pumpP);
  pumpK.targetMl = ml; pumpK.dispensedMl = 0; pumpStop(pumpK);

  // start with N
  pumpStart(pumpN, 0);
  uiSetTargets(ml, ml, ml, ml*3.0f, "PURGE");
}

static void purgeAllUpdate(){
  if(!purgeAll.active) return;

  if(purgeAll.step==1 && pumpFinished(pumpN)){
    pumpStart(pumpP, 0);
    purgeAll.step = 2;
    return;
  }
  if(purgeAll.step==2 && pumpFinished(pumpP)){
    pumpStart(pumpK, 0);
    purgeAll.step = 3;
    return;
  }
  if(purgeAll.step==3 && pumpFinished(pumpK)){
    // done
    purgeAll.active = false;
    purgeAll.step = 0;

    // purge done -> single beep
    buzzerBeepOnce(120);

    // stop the generic mix_done triple-beep/reset for this transition
    suppressNextMixDone = true;

    // notify dashboard (it listens for purge_all_done)
    {
      StaticJsonDocument<128> d;
      d["status"] = "ok";
      d["msg"]    = "purge_all_done";
      String s; serializeJson(d,s);
      wsSendAll(s);
    }

    purgeDeadlineMs = 0;
    lastError = "";
    setState(JS_IDLE, "purge_done");

    // clear values so UI doesn't keep stale targets
    pumpN.targetMl=pumpP.targetMl=pumpK.targetMl=0;
    pumpN.dispensedMl=pumpP.dispensedMl=pumpK.dispensedMl=0;
    ui.valid=false;
    wsBroadcast();
  }
}

// ================== Manual job ==================
bool beginManualJob(char pumpChar, float rate, int seconds){
  pumpChar = toupper((unsigned char)pumpChar);

  PumpState* ps=nullptr;
  unsigned long* last=nullptr;
  float defRate=0;

  if(pumpChar=='N'){ ps=&pumpN; last=&lastMsN; defRate=getPumpRate('N'); }
  else if(pumpChar=='P'){ ps=&pumpP; last=&lastMsP; defRate=getPumpRate('P'); }
  else if(pumpChar=='K'){ ps=&pumpK; last=&lastMsK; defRate=getPumpRate('K'); }
  else return false;

  if(manual.ps){
    if(!manual.prevOn) pumpStop(*manual.ps);
    else{
      manual.ps->on=true;
      manual.ps->rateMlPerS=manual.prevRate;
      setPumpPin(manual.ps->pin,true);
    }
    manual.ps=nullptr;
  }

  manual.prevOn   = ps->on;
  manual.prevRate = ps->rateMlPerS;
  manual.ps       = ps;
  manual.lastPtr  = last;
  manual.defRate  = defRate;
  manual.until    = millis() + (unsigned long)constrain(seconds,1,30)*1000UL;

  lastError = "";
  setState(JS_MANUAL, "manual_start");

  *last = millis();
  pumpStart(*ps, rate>0 ? rate : defRate);
  Serial.printf("[MANUAL] pump=%c rate=%.2f sec=%d\n", pumpChar, (double)(rate>0?rate:defRate), seconds);
  return true;
}

// ================== Modbus RTU (RS485) helpers ==================
uint16_t modbusCRC16(const uint8_t* data, uint16_t len){
  uint16_t crc=0xFFFF;
  for(uint16_t pos=0; pos<len; pos++){
    crc ^= (uint16_t)data[pos];
    for(int i=0;i<8;i++){
      if(crc & 0x0001){
        crc >>= 1;
        crc ^= 0xA001;
      }else{
        crc >>= 1;
      }
    }
  }
  return crc;
}

void rs485SetTx(bool tx){
  digitalWrite(RS485_DE_RE_PIN, tx ? HIGH : LOW);
  delayMicroseconds(50);
}

bool modbusReadHoldingRegs(uint8_t addr,uint16_t reg,uint16_t count,
                           uint16_t* outBuf,size_t outBufSize){
  if(count==0 || outBufSize<count) return false;

  uint8_t req[8];
  req[0]=addr;
  req[1]=0x03;
  req[2]=reg>>8;
  req[3]=reg&0xFF;
  req[4]=count>>8;
  req[5]=count&0xFF;
  uint16_t crc = modbusCRC16(req,6);
  req[6]=crc & 0xFF;
  req[7]=crc >> 8;

  while(Serial2.available()) Serial2.read();

  rs485SetTx(true);
  Serial2.write(req,sizeof(req));
  Serial2.flush();
  rs485SetTx(false);

  const uint32_t timeout=100;
  uint8_t resp[64];
  uint16_t idx=0;
  uint32_t start=millis();

  while((millis()-start)<timeout && idx<sizeof(resp)){
    if(Serial2.available()){
      resp[idx++] = Serial2.read();
    }
  }
  if(idx<5) return false;

  uint16_t respCrc = (resp[idx-1]<<8) | resp[idx-2];
  uint16_t calcCrc = modbusCRC16(resp, idx-2);
  if(respCrc!=calcCrc) return false;
  if(resp[0]!=addr)    return false;
  if(resp[1]!=0x03)    return false;

  uint8_t byteCount = resp[2];
  if(byteCount != count*2) return false;

  for(uint16_t i=0;i<count;i++){
    uint8_t hi = resp[3 + i*2];
    uint8_t lo = resp[4 + i*2];
    outBuf[i] = (hi<<8) | lo;
  }
  return true;
}

// ================== Soil sensor hook (RS485) ==================
void updateSoilFromSensors(){
  static uint32_t lastPoll=0;
  uint32_t now=millis();
  if(now-lastPoll < SOIL_POLL_INTERVAL_MS) return;
  lastPoll = now;

  uint16_t buf[8];
  bool updatedPH  = false;
  bool updatedNPK = false;

  // pH
  if(modbusReadHoldingRegs(SOIL_SENSOR_ID, REG_SOIL_PH, 1, buf, 8)){
    float phRaw = buf[0] / 10.0f;
    float phCal = PH_SLOPE * phRaw + PH_OFFSET;
    if(phCal>0.1f && phCal<14.5f){
      soilPH   = phCal;
      updatedPH = true;
    }
  }

  // NPK
  if(modbusReadHoldingRegs(SOIL_SENSOR_ID, REG_SOIL_N, 3, buf, 8)){
    float n = buf[0];
    float p = buf[1];
    float k = buf[2];

    if(n>=0 && n<10000){ soilN=n; updatedNPK=true; }
    if(p>=0 && p<10000){ soilP=p; updatedNPK=true; }
    if(k>=0 && k<10000){ soilK=k; updatedNPK=true; }
  }

  // update averages
  if(!soilAvgInit){
    soilN_avg  = soilN;
    soilP_avg  = soilP;
    soilK_avg  = soilK;
    soilPH_avg = soilPH;
    soilAvgInit = true;
  }else{
    if(updatedPH){
      soilPH_avg = soilPH_avg +
                   SOIL_FILTER_ALPHA_PH * (soilPH - soilPH_avg);
    }
    if(updatedNPK){
      soilN_avg = soilN_avg +
                  SOIL_FILTER_ALPHA_NPK * (soilN - soilN_avg);
      soilP_avg = soilP_avg +
                  SOIL_FILTER_ALPHA_NPK * (soilP - soilP_avg);
      soilK_avg = soilK_avg +
                  SOIL_FILTER_ALPHA_NPK * (soilK - soilK_avg);
    }
  }
}

// ================== Soil sensor TEST (optional) ==================
#if ENABLE_SOIL_SENSOR_TEST
void soilSensorTestOnce() {
  static uint32_t lastTest = 0;
  uint32_t now = millis();
  if (now - lastTest < 2000) return;
  lastTest = now;

  uint16_t buf[4];

  Serial.println("----- SENSOR TEST -----");

  if (modbusReadHoldingRegs(SOIL_SENSOR_ID, 0x0003, 1, buf, 1)) {
    float phRaw = buf[0] / 10.0f;
    Serial.print("pH_raw (reg 0x0003): ");
    Serial.println(phRaw, 1);
  } else {
    Serial.println("pH_raw: read FAIL");
  }

  if (modbusReadHoldingRegs(SOIL_SENSOR_ID, 0x0004, 3, buf, 3)) {
    uint16_t Nraw = buf[0];
    uint16_t Praw = buf[1];
    uint16_t Kraw = buf[2];
    Serial.print("N_raw: "); Serial.print(Nraw);
    Serial.print("  P_raw: "); Serial.print(Praw);
    Serial.print("  K_raw: "); Serial.println(Kraw);
  } else {
    Serial.println("NPK_raw: read FAIL");
  }

  Serial.print("soilPH(main): "); Serial.print(soilPH, 2);
  Serial.print("  soilN(main): "); Serial.print(soilN);
  Serial.print("  soilP(main): "); Serial.print(soilP);
  Serial.print("  soilK(main): "); Serial.println(soilK);
  Serial.print("soilPH_avg: "); Serial.print(soilPH_avg, 2);
  Serial.print("  soilN_avg: "); Serial.print(soilN_avg);
  Serial.print("  soilP_avg: "); Serial.print(soilP_avg);
  Serial.print("  soilK_avg: "); Serial.println(soilK_avg);
  Serial.println();
}
#endif

// ================== WebSocket event ==================
void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
               AwsEventType type, void *arg, uint8_t *data, size_t len){
  if(type==WS_EVT_CONNECT){
    wsClientConnected=true;
    Serial.printf("[WS] client #%u connected\n", client->id());
    wsBroadcast();
    return;
  }
  if(type==WS_EVT_DISCONNECT){
    Serial.printf("[WS] client #%u disconnected\n", client->id());
    return;
  }
  if(type!=WS_EVT_DATA) return;

  AwsFrameInfo *info=(AwsFrameInfo*)arg;
  if(!(info->final && info->index==0 && info->len==len && info->opcode==WS_TEXT)) return;

  StaticJsonDocument<512> doc;
  if(deserializeJson(doc, data, len)){ wsAck(client,"error","bad_json"); return; }

  const char* cmd = doc["command"] | "";

  // Pro: optional token guard (backward compatible by default)
  if(!tokenOkJson(doc)){
    wsAck(client,"error","unauthorized");
    return;
  }

  // ----- CONFIG (persistent on ESP32) -----
  if(strcmp(cmd,"get_config")==0 || strcmp(cmd,"config_get")==0){
    StaticJsonDocument<256> r;
    r["type"]="config";
    r["line_ml"]=cfgLineHeadMl;
    r["spare_pct"]=cfgSparePct;
    // also include calib for convenience
    auto jc=r.createNestedObject("calib"); jc["N"]=pumpRateN; jc["P"]=pumpRateP; jc["K"]=pumpRateK;
    String out; serializeJson(r,out);
    client->text(out);
    return;
  }
  else if(strcmp(cmd,"set_config")==0 || strcmp(cmd,"config_set")==0){
    float lineMl = doc["line_ml"] | doc["lineHeadMl"] | (float)cfgLineHeadMl;
    float spare  = doc["spare_pct"] | doc["sparePct"] | (float)cfgSparePct;
    saveMixerConfig(lineMl, spare);
    wsAck(client,"ok","config_saved");
    wsBroadcast();
    return;
  }
  // ----- STOP (emergency) -----
  else if(strcmp(cmd,"stop_all")==0 || strcmp(cmd,"emergency_stop")==0 || strcmp(cmd,"stop_mixing")==0 || strcmp(cmd,"stop")==0){
    emergencyStop("ws_stop");
    wsAck(client,"ok","stopped");
    wsBroadcast();
    return;
  }
  // ----- PURGE ALL (flush all pumps simultaneously) -----
  else if(strcmp(cmd,"purge_all")==0 || strcmp(cmd,"flush_all")==0){
    float ml = doc["ml"] | doc["volume_ml"] | doc["amount_ml"] | 0.0f;
    if(ml <= 0.0f){ wsAck(client,"error","bad_ml"); return; }
    if(!canStartJobX(OwnerX::Web)){ wsAck(client,"busy","owned_by_other"); return; }
    if(purgeAll.active){ wsAck(client,"busy","purge_in_progress"); return; }
    errorLatched = false;
    activeOwner = OwnerX::Web;

    // run sequentially N -> P -> K
    purgeAllStart(ml);

    wsAck(client,"ok","purge_all_started");
    wsBroadcast();
    return;
  }

  // ----- START MIXING (normal + quick_mix) -----
  if(strcmp(cmd,"start_mixing")==0){

    float addN = doc["addN"] | 0.0f;
    float addP = doc["addP"] | 0.0f;
    float addK = doc["addK"] | 0.0f;
    float mixV = doc["mixVolume"] | 0.0f;

    const char* source = doc["source"] | "";

    if(strcmp(source,"quick_mix")==0){
      JsonVariant fracV   = doc["frac"];
      JsonVariant targetV = doc["target"];

      if(fracV.is<JsonObject>() && mixV>0.0f){
        float fN = fracV["N"] | fracV["n"] | 0.0f;
        float fP = fracV["P"] | fracV["p"] | 0.0f;
        float fK = fracV["K"] | fracV["k"] | 0.0f;
        addN = mixV * fN;
        addP = mixV * fP;
        addK = mixV * fK;
      }

      if((addN<=0.0f && addP<=0.0f && addK<=0.0f) && targetV.is<JsonObject>()){
        addN = targetV["N"] | targetV["n"] | 0.0f;
        addP = targetV["P"] | targetV["p"] | 0.0f;
        addK = targetV["K"] | targetV["k"] | 0.0f;
        if(mixV<=0.0f){
          float sumT = addN+addP+addK;
          if(sumT>0.0f) mixV=sumT;
        }
      }
    }

    float sum = addN + addP + addK;
    if(mixV>0.0f && sum>0.0f){
      float s = mixV / sum;
      auto clip = [](float x){ return (x<0.05f)?0.0f:x; };
      addN = clip(addN*s);
      addP = clip(addP*s);
      addK = clip(addK*s);
    }

    if(addN<=0.0f && addP<=0.0f && addK<=0.0f){
      wsAck(client,"error","zero_mix");
      return;
    }

    if(!canStartJobX(OwnerX::Web)){ wsAck(client,"busy","owned_by_other"); return; }
    errorLatched = false;          // new web mixing clears error
    activeOwner = OwnerX::Web;

    pumpN.targetMl = addN; pumpN.dispensedMl=0; if(addN>0) pumpStart(pumpN,0); else pumpStop(pumpN);
    pumpP.targetMl = addP; pumpP.dispensedMl=0; if(addP>0) pumpStart(pumpP,0); else pumpStop(pumpP);
    pumpK.targetMl = addK; pumpK.dispensedMl=0; if(addK>0) pumpStart(pumpK,0); else pumpStop(pumpK);

    uiSetTargets(addN,addP,addK,mixV,"");
    // Pro: overall mixing timeout (prevents stuck pumps)
    {
      float tN = (addN>0)? (addN / max(0.01f, getPumpRate('N'))) : 0.0f;
      float tP = (addP>0)? (addP / max(0.01f, getPumpRate('P'))) : 0.0f;
      float tK = (addK>0)? (addK / max(0.01f, getPumpRate('K'))) : 0.0f;
      float tMax = max(tN, max(tP, tK));
      mixDeadlineMs = millis() + (uint32_t)((tMax + 8.0f) * 1000.0f); // + buffer
    }
    lastError = "";
    setState(JS_MIXING, "mix_start");
    wsAck(client,"ok","mix_started");
  }
  else if(strcmp(cmd,"manual")==0){
    String pump = doc["pump"] | "";
    char   pch  = pump.length()? toupper((unsigned char)pump[0]) : 'N';
    float rate = (doc["rate_ml_per_s"] | 0.0f) ? (float)doc["rate_ml_per_s"]
               : (doc["ml_per_s"]     | 0.0f) ? (float)doc["ml_per_s"]
               : (doc["rate"]         | 0.0f) ? (float)doc["rate"] : 0.0f;
    int seconds = doc["seconds"] | doc["sec"] | doc["duration"] | MANUAL_DEFAULT_SEC;

    if(beginManualJob(pch,rate,seconds)) wsAck(client,"ok","manual_started");
    else wsAck(client,"error","pump?");
  }
  else if(strcmp(cmd,"stop_pump")==0 || strcmp(cmd,"stop_manual")==0){
    // Stop ONLY the pump that is currently running as a manual job (calibration)
    String pump = doc["pump"] | "";
    char   pch  = pump.length()? toupper((unsigned char)pump[0]) : 'N';
    if(stopManualJobForPump(pch)) wsAck(client,"ok","manual_stopped");
    else wsAck(client,"error","no_manual");
  }
  else if(strncmp(cmd,"prime",5)==0 || strncmp(cmd,"purge",5)==0){
    size_t L = strlen(cmd);
    char pch = (L>0)? toupper((unsigned char)cmd[L-1]) : 'N';
    float rate = (doc["rate"] | 0.0f) ? (float)doc["rate"]
               : (doc["ml_per_s"] | 0.0f) ? (float)doc["ml_per_s"]
               : (doc["rate_ml_per_s"] | 0.0f) ? (float)doc["rate_ml_per_s"]
               : (pch=='N' && (doc["rateN"]|0.0f)) ? (float)doc["rateN"]
               : (pch=='P' && (doc["rateP"]|0.0f)) ? (float)doc["rateP"]
               : (pch=='K' && (doc["rateK"]|0.0f)) ? (float)doc["rateK"]
               : 0.0f;
    int seconds = doc["seconds"] | doc["sec"] | doc["duration"] | MANUAL_DEFAULT_SEC;

    if(beginManualJob(pch,rate,seconds)) wsAck(client,"ok","manual_started");
    else wsAck(client,"error","pump?");
  }
  else if(strcmp(cmd,"set_calib")==0 || strcmp(cmd,"set_calibration")==0 || strcmp(cmd,"calib_set")==0){
    String pump = doc["pump"] | "";
    char pch = pump.length()? toupper((unsigned char)pump[0]) : 'N';
    float mlps = (doc["mlps"] | 0.0f) ? (float)doc["mlps"]
               : (doc["ml_per_s"] | 0.0f) ? (float)doc["ml_per_s"]
               : (doc["rate_ml_per_s"] | 0.0f) ? (float)doc["rate_ml_per_s"]
               : (doc["rate"] | 0.0f) ? (float)doc["rate"] : 0.0f;

    if(mlps > 0.0f && savePumpCalibration(pch, mlps)){
      wsAck(client,"ok","calib_saved");
      wsBroadcast();
    }else{
      wsAck(client,"error","calib_bad");
    }
  }
  else if(strcmp(cmd,"get_calib")==0 || strcmp(cmd,"get_calibration")==0){
    StaticJsonDocument<192> d;
    d["type"]="calib";
    d["N"]=pumpRateN; d["P"]=pumpRateP; d["K"]=pumpRateK;
    String s; serializeJson(d,s);
    client->text(s);
  }

}

// ================== LCD render ==================
void renderLCD(){
  if(!lcdReady) return;
  uint32_t now=millis();
  if(now-lastDisplayMs<DISPLAY_REFRESH_MS) return;
  lastDisplayMs=now;

  const Recipe* rSel = recipeByIndex(pendingIdx);
  String cropName = rSel? String(rSel->name) : "-";

  if(displayPage==0){
    setRow(0,"SMART SOIL Fertilizer-Mixer");
    setRow(1, kv("SSID", AP_SSID));
    setRow(2, kv("IP",   apIP.toString()));
    setRow(3, kv("WS",   wsClientConnected? "CONNECTED":"WAITING"));
  }else if(displayPage==1){
    setRow(0,"Soil LIVE");

    char vNPK[16];
    snprintf(vNPK,sizeof(vNPK),"%d/%d/%d",(int)soilN,(int)soilP,(int)soilK);

    char vPH[8];
    snprintf(vPH,sizeof(vPH),"%.1f",soilPH);

    String lineNPK = "NPK :" + String(vNPK);
    String linePH  = "pH :" + String(vPH);

    setRow(1, lineNPK);
    setRow(2, linePH);
    setRow(3, kv("Select", cropName));
  }else{
    String head = cropName + " " + String((int)CONFIRM_MIX_VOL_ML) + "ML";
    setRow(0, kv("TGT", head));

    if(ui.valid){
      char tnpk[16]; snprintf(tnpk,sizeof(tnpk),"%d/%d/%d",(int)ui.n,(int)ui.p,(int)ui.k);
      setRow(1, kv("NPK", tnpk));
      setRow(2, kv("PH",  ui.ph.length()? ui.ph : "-"));
    }else if(rSel){
      char tnpk[16]; snprintf(tnpk,sizeof(tnpk),"%d/%d/%d",rSel->n,rSel->p,rSel->k);
      setRow(1, kv("NPK", tnpk));
      setRow(2, kv("PH",  String(rSel->ph)));
    }else{
      setRow(1, kv("NPK","-/-/-"));
      setRow(2, kv("PH","-"));
    }

    if(mixingActive()) drawProgress(3);
    else setRow(3,"IDLE");
  }
}

// ================== Buttons ==================
void btnInit(){
  auto setMode = [](int pin,bool activeLow){
    pinMode(pin, activeLow ? INPUT_PULLUP : INPUT_PULLDOWN);
  };
  setMode(btn1.pin, btn1.activeLow);
  setMode(btn2.pin, btn2.activeLow);
  setMode(btn3.pin, btn3.activeLow);
  setMode(btn4.pin, btn4.activeLow);
}
static inline uint8_t readPressed(const Btn& b){
  int raw = digitalRead(b.pin);
  return b.activeLow ? (raw==LOW) : (raw==HIGH);
}
void btnPoll(Btn& b){
  uint8_t logical = readPressed(b);
  uint32_t now=millis();
  if(logical!=b.cur && now-b.t>DEBOUNCE_MS){
    b.last=b.cur; b.cur=logical; b.t=now; b.changed=true;
  }else{
    b.changed=false;
  }
}
static inline bool pressedDown(const Btn& b){ return b.changed && b.last==0 && b.cur==1; }
static inline bool pressedUp  (const Btn& b){ return b.changed && b.last==1 && b.cur==0; }

#if DEBUG_BTN
  #define DBG(x) do{ Serial.println(x); }while(0)
#else
  #define DBG(x) do{}while(0)
#endif

// ================== Setup / Loop ==================
void setup(){
  Serial.begin(115200);
  loadPumpCalibration();
  loadMixerConfig();
  uiStateLoad();

  // Pro: session token (optional)
  {
    uint32_t r1 = esp_random();
    uint32_t r2 = esp_random();
    char buf[17];
    snprintf(buf, sizeof(buf), "%08X%08X", (unsigned)r1, (unsigned)r2);
    sessionToken = String(buf);
  }
  logEvent(String("boot fw=") + FW_VERSION + " token_required=" + (REQUIRE_TOKEN?"1":"0"));

  pinMode(PIN_PUMP_N,OUTPUT);
  pinMode(PIN_PUMP_P,OUTPUT);
  pinMode(PIN_PUMP_K,OUTPUT);
  setPumpPin(PIN_PUMP_N,false);
  setPumpPin(PIN_PUMP_P,false);
  setPumpPin(PIN_PUMP_K,false);

  pinMode(PIN_BUZZER, OUTPUT);
  buzzerOff();

  // LEDs
  pinMode(PIN_LED_GREEN, OUTPUT);
  pinMode(PIN_LED_RED,   OUTPUT);
  ledWrite(PIN_LED_GREEN, false);
  ledWrite(PIN_LED_RED,   false);

  // RS485
  pinMode(RS485_DE_RE_PIN, OUTPUT);
  rs485SetTx(false);
  Serial2.begin(SOIL_BAUD, SOIL_BYTE_FORMAT, RS485_RX_PIN, RS485_TX_PIN);

  LittleFS.begin();

  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(apIP, apIP, netMsk);
  WiFi.softAP(AP_SSID, AP_PASS);
  WiFi.setHostname("fertilizer");

  if(MDNS.begin("fertilizer")){
    MDNS.addService("http","tcp",80);
  }

  dns.setErrorReplyCode(DNSReplyCode::NoError);
  dns.start(53, "*", apIP);
  Serial.print("AP IP: "); Serial.println(apIP);

  // captive portal probes
  server80.on("/generate_204",           HTTP_GET, captiveLanding);
  server80.on("/gen_204",                HTTP_GET, captiveLanding);
  server80.on("/hotspot-detect.html",    HTTP_GET, captiveLanding);
  server80.on("/success.txt",            HTTP_GET, captiveLanding);
  server80.on("/connecttest.txt",        HTTP_GET, captiveLanding);
  server80.on("/ncsi.txt",               HTTP_GET, captiveLanding);
  server80.on("/canonical.html",         HTTP_GET, captiveLanding);
  server80.on("/wpad.dat",               HTTP_GET, captiveLanding);
  server80.on("/detectportal.html",      HTTP_GET, captiveLanding);
  server80.on("/success",                HTTP_GET, captiveLanding);
  server80.on("/library/test/success.html", HTTP_GET, captiveLanding);
  server80.on("/check_network_status.txt",  HTTP_GET, captiveLanding);

  server80.on("/ok", HTTP_GET, [](AsyncWebServerRequest* r){
    r->send(200,"text/plain; charset=utf-8","OK");
  });

  server80.on("/ls", HTTP_GET, [](AsyncWebServerRequest* r){
    String s="FILES:\n";
    File root = LittleFS.open("/");
    File f = root.openNextFile();
    while(f){
      s += " - " + String(f.name()) + " (" + String(f.size()) + ")\n";
      f = root.openNextFile();
    }
    r->send(200,"text/plain; charset=utf-8",s);
  });

  // REST manual/prime/purge backup
  server80.on("/api/manual", HTTP_ANY, [](AsyncWebServerRequest* r){
    String pump = paramS(r,"pump","N");
    char   pch  = pump.length()?toupper((unsigned char)pump[0]):'N';
    float rate  = paramF(r,"rate",paramF(r,"ml_per_s",paramF(r,"rate_ml_per_s",0)));
    int   sec   = constrain(paramI(r,"seconds",paramI(r,"sec",paramI(r,"duration",MANUAL_DEFAULT_SEC))),1,30);
    bool ok = beginManualJob(pch,rate,sec);
    r->send(ok?200:400,"application/json; charset=utf-8", ok?"{\"ok\":true}":"{\"ok\":false}");
  });
  server80.on("/api/prime", HTTP_ANY, [](AsyncWebServerRequest* r){
    String pump = paramS(r,"pump","N");
    char   pch  = pump.length()?toupper((unsigned char)pump[0]):'N';
    float rate  = paramF(r,"rate",paramF(r,"ml_per_s",paramF(r,"rate_ml_per_s",0)));
    int   sec   = constrain(paramI(r,"seconds",paramI(r,"sec",paramI(r,"duration",MANUAL_DEFAULT_SEC))),1,30);
    bool ok = beginManualJob(pch,rate,sec);
    r->send(ok?200:400,"application/json; charset=utf-8", ok?"{\"ok\":true}":"{\"ok\":false}");
  });
  server80.on("/api/purge", HTTP_ANY, [](AsyncWebServerRequest* r){
    String pump = paramS(r,"pump","N");
    char   pch  = pump.length()?toupper((unsigned char)pump[0]):'N';
    float rate  = paramF(r,"rate",paramF(r,"ml_per_s",paramF(r,"rate_ml_per_s",0)));
    int   sec   = constrain(paramI(r,"seconds",paramI(r,"sec",paramI(r,"duration",MANUAL_DEFAULT_SEC))),1,30);
    bool ok = beginManualJob(pch,rate,sec);
    r->send(ok?200:400,"application/json; charset=utf-8", ok?"{\"ok\":true}":"{\"ok\":false}");
  });



  // Persistent config API (stored on ESP32 NVS)
  // GET  /api/config -> {"line_ml":..,"spare_pct":..,"calib":{...}}
  // POST /api/config?line_ml=40&spare_pct=10 -> save
  server80.on("/api/config", HTTP_ANY, [](AsyncWebServerRequest* r){
    if(r->method()==HTTP_POST || r->hasParam("line_ml", true) || r->hasParam("spare_pct", true) ||
       r->hasParam("line_ml", false) || r->hasParam("spare_pct", false)){
      float lineMl = paramF(r,"line_ml", cfgLineHeadMl);
      float spare  = paramF(r,"spare_pct", cfgSparePct);
      saveMixerConfig(lineMl, spare);
    }
    // respond
    StaticJsonDocument<512> d;
    d["line_ml"] = cfgLineHeadMl;
    d["spare_pct"] = cfgSparePct;
    d["fw"] = FW_VERSION;
    auto jc=d.createNestedObject("calib"); jc["N"]=pumpRateN; jc["P"]=pumpRateP; jc["K"]=pumpRateK;
    String out; serializeJson(d,out);
    r->send(200,"application/json; charset=utf-8", out);
  });

  // ================== PRO APIs ==================
  // GET /api/token  -> {"token":"...","required":false}
  server80.on("/api/token", HTTP_GET, [](AsyncWebServerRequest* r){
    StaticJsonDocument<128> d;
    d["token"] = sessionToken;
    d["required"] = REQUIRE_TOKEN;
    d["fw"] = FW_VERSION;
    String out; serializeJson(d,out);
    r->send(200,"application/json; charset=utf-8", out);
  });

  // GET /api/diag -> system snapshot (JSON)
  server80.on("/api/diag", HTTP_GET, [](AsyncWebServerRequest* r){
    if(!tokenOk(r)){ r->send(401,"application/json; charset=utf-8", R"({"ok":false,"error":"unauthorized"})"); return; }
    StaticJsonDocument<768> d;
    d["ok"] = true;
    d["fw"] = FW_VERSION;
    d["uptime_ms"] = (uint32_t)millis();
    d["heap_free"] = (uint32_t)ESP.getFreeHeap();
    d["state"] = jobStateName(jobState);
    d["owner"] = (activeOwner==OwnerX::Btn?"BTN":(activeOwner==OwnerX::Web?"WEB":"NONE"));
    d["error"] = errorLatched;
    if(lastError.length()) d["last_error"] = lastError;

    d["ws_connected"] = wsClientConnected;
    d["ip"] = apIP.toString();
    d["ssid"] = AP_SSID;

    // deadlines (0 means none)
    d["mix_deadline_ms"] = mixDeadlineMs;
    d["purge_deadline_ms"] = purgeDeadlineMs;

    // pumps
    auto pN=d.createNestedObject("pumpN"); pN["on"]=pumpN.on; pN["t"]=pumpN.targetMl; pN["d"]=pumpN.dispensedMl; pN["rate"]=getPumpRate('N');
    auto pP=d.createNestedObject("pumpP"); pP["on"]=pumpP.on; pP["t"]=pumpP.targetMl; pP["d"]=pumpP.dispensedMl; pP["rate"]=getPumpRate('P');
    auto pK=d.createNestedObject("pumpK"); pK["on"]=pumpK.on; pK["t"]=pumpK.targetMl; pK["d"]=pumpK.dispensedMl; pK["rate"]=getPumpRate('K');

    d["manual_active"] = (manual.ps!=nullptr);
    d["purge_active"] = purgeAll.active;

    String out; serializeJson(d,out);
    r->send(200,"application/json; charset=utf-8", out);
  });

  // GET /api/log -> plain text ring buffer (latest first)
  server80.on("/api/log", HTTP_GET, [](AsyncWebServerRequest* r){
    if(!tokenOk(r)){ r->send(401,"text/plain; charset=utf-8","unauthorized"); return; }
    String s;
    s.reserve(2048);
    s += "log_count=" + String(logCount) + "\n";
    // output newest -> oldest
    for(int i=0;i<logCount;i++){
      int idx = (int)logHead - 1 - i;
      while(idx < 0) idx += LOG_CAP;
      s += logBuf[idx];
      s += "\n";
    }
    r->send(200,"text/plain; charset=utf-8", s);
  });

  // POST /api/log/clear
  server80.on("/api/log/clear", HTTP_POST, [](AsyncWebServerRequest* r){
    if(!tokenOk(r)){ r->send(401,"application/json; charset=utf-8", R"({"ok":false,"error":"unauthorized"})"); return; }
    for(uint8_t i=0;i<LOG_CAP;i++) logBuf[i]="";
    logHead=0; logCount=0;
    r->send(200,"application/json; charset=utf-8", R"({"ok":true})");
  });

  // ================== UI State API (persist last UI values) ==================
  // GET  /api/ui_state                     -> return stored UI state JSON
  // POST /api/ui_state  (JSON body)        -> save UI state JSON (<= 4KB)
  server80.on("/api/ui_state", HTTP_GET, [](AsyncWebServerRequest* r){
    r->send(200, "application/json; charset=utf-8", uiStateGet());
  });

  server80.on(
    "/api/ui_state",
    HTTP_POST,
    [](AsyncWebServerRequest* r){
      // response sent in onBody
    },
    NULL,
    [](AsyncWebServerRequest* r, uint8_t* data, size_t len, size_t index, size_t total){
      if(index==0){
        r->_tempObject = new String();
      }
      String* body = (String*)r->_tempObject;
      if(body){
        body->reserve(total + 8);
        body->concat((const char*)data, len);
      }
      if(index + len == total){
        String payload = body ? *body : "";
        if(body){ delete body; r->_tempObject = NULL; }
        if(payload.length() > UI_STATE_MAX){
          r->send(413, "application/json; charset=utf-8", R"({"ok":false,"error":"too_large"})");
          return;
        }
        // basic JSON validation (optional)
        DynamicJsonDocument jd(2048);
        auto err = deserializeJson(jd, payload);
        if(err){
          r->send(400, "application/json; charset=utf-8", R"({"ok":false,"error":"bad_json"})");
          return;
        }
        String compact;
        serializeJson(jd, compact);
        bool ok = uiStateSave(compact);
        r->send(ok?200:500, "application/json; charset=utf-8", ok?"{\"ok\":true}":"{\"ok\":false}");
      }
    }
  );

// ================== Notifications API (plots / rounds) ==================
  // GET  /api/notify/plots                     -> list plots
  // GET  /api/notify/plot?name=...              -> plot state + last info
  // POST /api/notify/log  (JSON body)           -> upsert a log + update plot state
  // POST /api/notify/delete?name=...            -> delete plot + its logs
  server80.on("/api/notify/plots", HTTP_GET, [](AsyncWebServerRequest* r){
    r->send(200, "application/json; charset=utf-8", notifyPlotsJson());
  });

  server80.on("/api/notify/plot", HTTP_GET, [](AsyncWebServerRequest* r){
    String name = paramS(r, "name", paramS(r, "plot", ""));
    r->send(200, "application/json; charset=utf-8", notifyPlotDetailJson(name));
  });

  server80.on("/api/notify/delete", HTTP_ANY, [](AsyncWebServerRequest* r){
    if(r->method()!=HTTP_POST && r->method()!=HTTP_GET){
      r->send(405, "application/json; charset=utf-8", "{\"ok\":false,\"error\":\"method\"}");
      return;
    }
    String name = paramS(r, "name", paramS(r, "plot", ""));
    bool ok = notifyDeletePlot(name);
    r->send(ok?200:404, "application/json; charset=utf-8", ok?"{\"ok\":true}":"{\"ok\":false,\"error\":\"not_found\"}");
  });

  // body handler for /api/notify/log
  server80.on(
    "/api/notify/log",
    HTTP_POST,
    [](AsyncWebServerRequest* r){
      // response is sent in onBody
    },
    NULL,
    [](AsyncWebServerRequest* r, uint8_t* data, size_t len, size_t index, size_t total){
      if(index==0){
        r->_tempObject = new String();
      }
      String* body = (String*)r->_tempObject;
      if(body){
        body->reserve(total + 8);
        body->concat((const char*)data, len);
      }
      if(index + len == total){
        String payload = body ? *body : "";
        if(body){ delete body; r->_tempObject = NULL; }

        DynamicJsonDocument jd(1024);
        auto err = deserializeJson(jd, payload);
        if(err){
          r->send(400, "application/json; charset=utf-8", "{\"ok\":false,\"error\":\"bad_json\"}");
          return;
        }
        notifyUpsertLog(jd.as<JsonVariantConst>());
        r->send(200, "application/json; charset=utf-8", "{\"ok\":true}");
      }
    }
  );

// --- เริ่มส่วนที่เพิ่มใหม่ (Check Storage) ---
  server80.on("/api/diag/storage", HTTP_GET, [](AsyncWebServerRequest *r){
      StaticJsonDocument<256> doc;
      
      // ข้อมูล RAM
      doc["heap_free"] = ESP.getFreeHeap();
      doc["sketch_size"] = ESP.getSketchSize();
      doc["sketch_free"] = ESP.getFreeSketchSpace();
      
      // ข้อมูลพื้นที่เก็บไฟล์ (LittleFS)
      doc["fs_total"] = LittleFS.totalBytes();
      doc["fs_used"] = LittleFS.usedBytes();
      
      String out;
      serializeJson(doc, out);
      r->send(200, "application/json; charset=utf-8", out);
  });
  // --- จบส่วนที่เพิ่มใหม่ ---
  server80.on("/api/version", HTTP_GET, [](AsyncWebServerRequest* r){
    DynamicJsonDocument d(128);
    d["fw"] = FW_VERSION;
    d["ip"] = apIP.toString();
    String out; serializeJson(d,out);
    r->send(200, "application/json", out);
  });

  // Emergency stop API
  // POST/GET /api/stop -> stop all pumps immediately
  server80.on("/api/stop", HTTP_ANY, [](AsyncWebServerRequest* r){
    emergencyStop("http_stop");
    wsBroadcast();
    r->send(200,"application/json; charset=utf-8", "{\"ok\":true}");
  });

  // Purge/flush all pumps simultaneously (each pump dispenses the same ml)
  // POST/GET /api/purge_all?ml=50
  server80.on("/api/purge_all", HTTP_ANY, [](AsyncWebServerRequest* r){
    float ml = paramF(r,"ml", paramF(r,"volume_ml", 0));
    if(ml<=0){
      r->send(400,"application/json; charset=utf-8", "{\"ok\":false,\"error\":\"bad_ml\"}");
      return;
    }
    if(!canStartJobX(OwnerX::Web)){
      r->send(409,"application/json; charset=utf-8", "{\"ok\":false,\"error\":\"busy\"}");
      return;
    }
    errorLatched=false;
    activeOwner=OwnerX::Web;

    pumpN.targetMl = ml; pumpN.dispensedMl=0; pumpStart(pumpN,0);
    pumpP.targetMl = ml; pumpP.dispensedMl=0; pumpStart(pumpP,0);
    pumpK.targetMl = ml; pumpK.dispensedMl=0; pumpStart(pumpK,0);

    uiSetTargets(ml,ml,ml,ml*3.0f,"PURGE");
    wsBroadcast();
    r->send(200,"application/json; charset=utf-8", "{\"ok\":true}");
  });


  // Pump calibration API (stored in NVS) — works even if WebSocket is blocked
  // GET  /api/calib                           -> {"ok":true,"calib":{...}}
  // POST /api/calib?pump=N&mlps=1.2          -> save one
  // POST /api/calib?N=1.2&P=...&K=...        -> save many
  // POST /api/calib?reset=1                  -> reset to defaults
  server80.on("/api/calib", HTTP_ANY, [](AsyncWebServerRequest* r){
    bool changed = false;

    // reset
    if(r->hasParam("reset", true) || r->hasParam("reset", false)){
      resetPumpCalibration();
      changed = true;
    } else {
      // single-pump
      String pumpS = paramS(r,"pump","");
      float mlps = paramF(r,"mlps", paramF(r,"ml_per_s", paramF(r,"rate", paramF(r,"rate_ml_per_s", NAN))));
      if(pumpS.length() && isfinite(mlps)){
        changed = savePumpCalibration(pumpS[0], mlps);
      } else {
        // multi
        float n = paramF(r,"N", paramF(r,"n", NAN));
        float p = paramF(r,"P", paramF(r,"p", NAN));
        float k = paramF(r,"K", paramF(r,"k", NAN));
        if(isfinite(n)) changed |= savePumpCalibration('N', n);
        if(isfinite(p)) changed |= savePumpCalibration('P', p);
        if(isfinite(k)) changed |= savePumpCalibration('K', k);
        // no params -> just return current
      }
    }

    if(changed) wsBroadcast();

    StaticJsonDocument<256> d;
    d["ok"] = true;
    auto jc = d.createNestedObject("calib");
    jc["N"] = pumpRateN;
    jc["P"] = pumpRateP;
    jc["K"] = pumpRateK;
    String out; serializeJson(d,out);
    r->send(200, "application/json; charset=utf-8", out);
  });


  // /calendar.ics
  server80.on("/calendar.ics", HTTP_GET, [](AsyncWebServerRequest *req){
    String title = paramS(req,"title","Soilmix");
    String s = paramS(req,"s","");
    String e = paramS(req,"e","");
    String alarm = paramS(req,"alarm1","-PT15M");
    String ics =
      "BEGIN:VCALENDAR\r\n"
      "VERSION:2.0\r\n"
      "PRODID:-//Soilmix//ESP32//TH\r\n"
      "BEGIN:VEVENT\r\n"
      "UID:" + String(millis()) + "@soilmix\r\n"
      "DTSTAMP:" + s + "\r\n"
      "DTSTART:" + s + "\r\n"
      "DTEND:"   + e + "\r\n"
      "SUMMARY:" + title + "\r\n"
      "BEGIN:VALARM\r\n"
      "TRIGGER:" + alarm + "\r\n"
      "ACTION:DISPLAY\r\n"
      "DESCRIPTION:" + title + "\r\n"
      "END:VALARM\r\n"
      "END:VEVENT\r\n"
      "END:VCALENDAR\r\n";
    auto *res = req->beginResponse(200,"text/calendar; charset=utf-8",ics);
    res->addHeader("Content-Disposition","attachment; filename=soilmix-event.ics");
    req->send(res);
  });

  server80.on("/", HTTP_GET, [](AsyncWebServerRequest* req){
    if(req->host()!=apIP.toString()){ captiveLanding(req); return; }
    serveFile(req,"/index.html");
  });
  server80.on("/tailwind.js", HTTP_GET, [](AsyncWebServerRequest* req){
    serveFile(req,"/tailwind.js");
  });
  server80.onNotFound([](AsyncWebServerRequest* req){
    if(req->host()!=apIP.toString()){ captiveLanding(req); return; }
    serveFile(req, req->url());
  });

  ws80.onEvent(onWsEvent);
  ws80_alt.onEvent(onWsEvent);
  ws81_root.onEvent(onWsEvent);
  server80.addHandler(&ws80);
  server80.addHandler(&ws80_alt);
  server81.addHandler(&ws81_root);

  server80.begin();
  server81.begin();

  btnInit();
  Wire.begin(21,22);
  LCD_ADDR = detectI2CAddress();
  lcd = new LiquidCrystal_I2C(LCD_ADDR,20,4);
  lcd->init();
  lcd->backlight();
  lcdReady=true;

  // system ready → green ON
  ledWrite(PIN_LED_GREEN, true);

  renderLCD();
  wsBroadcast();
}

void loop(){
  dns.processNextRequest();

  updateSoilFromSensors();
#if ENABLE_SOIL_SENSOR_TEST
  soilSensorTestOnce();
#endif

  if(millis()-lastStep>50){
    lastStep=millis();
    updatePumpDispensed(pumpN,lastMsN,getPumpRate('N'));
    updatePumpDispensed(pumpP,lastMsP,getPumpRate('P'));
    updatePumpDispensed(pumpK,lastMsK,getPumpRate('K'));

    // keep purge_all running sequentially (N -> P -> K)
    purgeAllUpdate();

    bool nowMixing = mixingActive();

    // ----- Pro: safety timeouts -----
    uint32_t nowMs = millis();
    if(nowMixing && mixDeadlineMs>0 && (int32_t)(nowMs - mixDeadlineMs) >= 0){
      logEvent("timeout MIX -> emergencyStop");
      emergencyStop("timeout_mix");
      mixDeadlineMs = 0;
    }
    if(purgeAll.active && purgeDeadlineMs>0 && (int32_t)(nowMs - purgeDeadlineMs) >= 0){
      logEvent("timeout PURGE -> emergencyStop");
      emergencyStop("timeout_purge");
      purgeDeadlineMs = 0;
    }

    // ----- RED LED status -----
    if (nowMixing) {
      // mixing → red ON
      ledWrite(PIN_LED_RED, true);
    } else {
      if (errorLatched) {
        // error/STOP → slow blink
        static uint32_t lastBlink = 0;
        static bool redOn = false;
        uint32_t t = millis();
        if (t - lastBlink > 500) {
          lastBlink = t;
          redOn = !redOn;
          ledWrite(PIN_LED_RED, redOn);
        }
      } else {
        // idle normal → red OFF
        ledWrite(PIN_LED_RED, false);
      }
    }
    // --------------------------

    if(wasMixing && !nowMixing){
      if(suppressNextMixDone){
        suppressNextMixDone = false; // swallow this transition (purge done)
      } else {
      // emit mix_done event with final snapshot before resetting values
      {
        StaticJsonDocument<384> e;
        e["msg"] = "mix_done";
        auto eN = e.createNestedObject("pumpN"); eN["targetMl"]=pumpN.targetMl; eN["dispensedMl"]=pumpN.dispensedMl;
        auto eP = e.createNestedObject("pumpP"); eP["targetMl"]=pumpP.targetMl; eP["dispensedMl"]=pumpP.dispensedMl;
        auto eK = e.createNestedObject("pumpK"); eK["targetMl"]=pumpK.targetMl; eK["dispensedMl"]=pumpK.dispensedMl;
        e["owner"] = (activeOwner==OwnerX::Btn?"BTN":(activeOwner==OwnerX::Web?"WEB":"NONE"));
        String es; serializeJson(e, es);
        wsSendAll(es);
      }
      mixDeadlineMs = 0;
      if(!errorLatched && !purgeAll.active && !manual.ps) setState(JS_IDLE, "mix_done");
      buzzerStartTriple();
      pendingReset=true;
      resetAt = millis()+2500;
      }
    }
    if(pendingReset && !nowMixing && (long)(millis()-resetAt)>=0){
      pumpN.targetMl=pumpP.targetMl=pumpK.targetMl=0;
      pumpN.dispensedMl=pumpP.dispensedMl=pumpK.dispensedMl=0;
      ui.valid=false;
      wsBroadcast();
      pendingReset=false;
    }
    wasMixing=nowMixing;

    if(!nowMixing && activeOwner!=OwnerX::None) activeOwner=OwnerX::None;
  }

  // manual job
  if(manual.ps){
    updatePumpDispensed(*manual.ps,*manual.lastPtr,manual.defRate);
    if((long)(millis()-manual.until)>=0){
      if(!manual.prevOn) pumpStop(*manual.ps);
      else{
        manual.ps->on=true;
        manual.ps->rateMlPerS=manual.prevRate;
        setPumpPin(manual.ps->pin,true);
      }
      manual.ps=nullptr;
      wsBroadcast();
      Serial.println("[MANUAL] done");
      if(!mixingActive() && !purgeAll.active) setState(JS_IDLE, "manual_done");
    }
  }

  if(millis()-lastPush>300){
    lastPush=millis();
    wsBroadcast();
  }

  // buttons
  btnPoll(btn1); btnPoll(btn2); btnPoll(btn3); btnPoll(btn4);

  if(pressedDown(btn1)){
    pendingIdx = (pendingIdx+1) % RECIPE_COUNT;
    DBG("[BTN1] next crop");
  }
  if(pressedDown(btn2)){
    DBG("[BTN2] confirm start");
    if(const Recipe* r = recipeByIndex(pendingIdx)){
      if(canStartJobX(OwnerX::Btn)) startMixByRecipe(*r,CONFIRM_MIX_VOL_ML,OwnerX::Btn);
    }
  }
  if(pressedDown(btn3)){
    DBG("[BTN3] pressed");
    uint32_t t0=millis(); bool didLong=false;
    while(readPressed(btn3)){
      if(millis()-t0>LONG_MS){
        emergencyStop("BTN3 LONG");
        DBG("[BTN3] LONG -> E-STOP");
        didLong=true;
        break;
      }
      delay(5);
    }
    if(!didLong){
      displayPage = (displayPage+1)%3;
      DBG("[BTN3] short -> next page");
    }
  }

  // STOP ALL button
  if(pressedDown(btn4)){
    DBG("[BTN4] STOP ALL");
    emergencyStop("BTN4 STOP");
    buzzerStartTriple();
  }

  buzzerUpdate();
  // turn off one-shot beep (used by purge_all_done)
  if(!buzz.active && oneBeepOffAt && (long)(millis()-oneBeepOffAt)>=0){
    buzzerOff();
    oneBeepOffAt = 0;
  }
  renderLCD();
  delay(1);
}