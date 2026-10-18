/*
   ESP32 Stage-Talkback / Switched Mic + Battery Level Monitor

   Supported mixers:
     - Behringer X32 / Midas M32      (OSC/UDP, port 10023)
     - Behringer X Air / Midas MR     (OSC/UDP, port 10024)
     - Behringer WING / WING Rack     (OSC/UDP, port 2223)

   Logic: while talkback is active, the selected channel is removed from
   Main (FOH) and from the selected buses. On release everything is restored.

   Libraries: Adafruit NeoPixel (Library Manager). Everything else is part
   of the ESP32 Arduino core.
*/

#include <WiFi.h>
#include <WiFiUdp.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <Preferences.h>
#include <Adafruit_NeoPixel.h>
#include <ESPmDNS.h>

// ---------- LED SETTINGS (RGBW) ----------
#define LED_PIN      15
#define NUM_LEDS     3

Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_GRBW + NEO_KHZ800);

const uint32_t COLOR_OFF = strip.Color(0, 0, 0, 0);
uint32_t lastShownColor = 0xFFFFFFFF;   // avoids redundant strip.show() calls

// ---------- HARDWARE PINS ----------
const int buttonPin = 4;
// Reset pins: holding ANY of them for 3 s erases all settings.
// GPIO 0 = BOOT button on the dev board, GPIO 16 = external button.
const int resetPins[] = {0, 16};
const int resetPinCount = sizeof(resetPins) / sizeof(resetPins[0]);

bool isResetPressed() {
  for (int i = 0; i < resetPinCount; i++) {
    if (digitalRead(resetPins[i]) == LOW) return true;
  }
  return false;
}
#define BATTERY_PIN       34   // ADC1 pin, 10k + 10k voltage divider
#define BATTERY_DIVIDER   2.0  // adjust after comparing with a multimeter (e.g. 2.05)

// ---------- MIXER MODELS ----------
enum MixerModel {
  MIXER_X32  = 0,
  MIXER_XAIR = 1,
  MIXER_WING = 2
};

// ---------- LANGUAGE TRANSLATIONS ----------
enum Language { LANG_EN = 0, LANG_DE = 1, LANG_UA = 2, LANG_RU = 3 };
int currentLang = LANG_UA;

struct Translations {
  const char* title;
  const char* selectWifi;
  const char* manualSsid;
  const char* wifiPass;
  const char* mixerModelLabel;
  const char* mixerIp;
  const char* channel;
  const char* btnModeLabel;
  const char* btnModePTT;
  const char* btnModeLatch;
  const char* ledBrightnessLabel;
  const char* selectLang;
  const char* busSelectTitle;
  const char* btnSave;
  const char* msgSaved;
  const char* msgRebooting;
};

const Translations txt[] = {
  // 0: ENGLISH
  {
    "Talkback Settings", "Select Wi-Fi Network:", "Or enter SSID manually:", "Wi-Fi Password:",
    "Mixer Model:", "Mixer IP Address:", "Channel Number:", "Button Mode:",
    "Push-To-Talk (Hold to Speak)", "Latch (Toggle On/Off)", "LED Brightness (%):",
    "Interface Language:", "Select Buses to MUTE during Talkback:", "Save & Reboot", "Saved!", "ESP32 is rebooting..."
  },
  // 1: GERMAN
  {
    "Talkback Einstellungen", "Wi-Fi-Netzwerk auswählen:", "Oder SSID manuell eingeben:", "Wi-Fi-Passwort:",
    "Mischpult Modell:", "Mischpult IP-Adresse:", "Kanalnummer:", "Tastenmodus:",
    "Push-To-Talk (Gedrückt halten)", "Latch (Umschalten An/Aus)", "LED-Helligkeit (%):",
    "Schnittstellensprache:", "Busse zum STUMMSCHALTEN bei Talkback wählen:", "Speichern & Neustarten", "Gespeichert!", "ESP32 startet neu..."
  },
  // 2: UKRAINIAN
  {
    "Налаштування Talkback", "Оберіть Wi-Fi мережу:", "Або введіть SSID вручну:", "Пароль Wi-Fi:",
    "Модель мікшерного пульта:", "IP адреса мікшерного пульта:", "Номер каналу:", "Режим роботи кнопки:",
    "Push-To-Talk (Утримувати для розмови)", "Latch (Перемикач Увімк/Вимк)", "Яскравість світлодіодів (%):",
    "Мова інтерфейсу:", "Оберіть Bus шини, які ЗАГЛУШИТИ при Talkback:", "Зберегти та Перезавантажити", "Збережено!", "ESP32 перезавантажується..."
  },
  // 3: RUSSIAN
  {
    "Настройки Talkback", "Выберите Wi-Fi сеть:", "Или введите SSID вручную:", "Пароль Wi-Fi:",
    "Модель микшерного пульта:", "IP адрес микшерного пульта:", "Номер канала:", "Режим кнопки:",
    "Push-To-Talk (Удержание)", "Latch (Переключатель Вкл/Выкл)", "Яркость светодиодов (%):",
    "Язык интерфейса:", "Выберите Bus шины, которые ЗАГЛУШИТЬ при Talkback:", "Сохранить и Перезагрузить", "Сохранено!", "Перезагрузка..."
  }
};

// ---------- SETTINGS ----------
Preferences preferences;

String wifi_ssid = "";
String wifi_pass = "";
String mixer_ip_str = "";
int mixer_model = MIXER_X32;
int channel = 0;
int button_mode = 0;
int led_brightness = 30;
bool bus_mute_flags[16] = {0};

bool use_static_ip = false;
String static_ip_str = "";
String gateway_str = "";
String subnet_str = "255.255.255.0";
String mdns_name = "talkback";

String color_standby_hex = "#0000FF";
String color_active_hex  = "#00FF00";

// ---------- RUNTIME STATE ----------
IPAddress mixerIP;
bool mixerIPValid = false;

bool mixer_online = false;
unsigned long lastPingTime = 0;
unsigned long lastReplyTime = 0;
const unsigned long pingInterval = 3000;

WiFiUDP udp;
WebServer server(80);
DNSServer dnsServer;

bool isAPMode = false;
bool serverStarted = false;
String wifiOptionsHtml = "";   // cached Wi-Fi scan (AP mode only)

bool lastButtonState = HIGH;
bool stableButtonState = HIGH;
bool latchActiveState = false;
bool isTalkbackActive = false;
unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 40;

unsigned long resetHoldStart = 0;
bool resetHandled = false;

unsigned long lastWifiCheck = 0;
const unsigned long wifiCheckInterval = 3000;
bool wifiWasConnected = false;
unsigned long wifiConnectStart = 0;
const unsigned long wifiTimeout = 20000;

// Number of times every OSC command is sent (UDP has no delivery guarantee)
const int OSC_PASSES = 2;

// X Air: bus sends are muted by setting the level to -inf, so the original
// level of every selected send is read from the mixer right before muting.
float xairSavedLevel[16];
bool  xairLevelKnown[16];

// ---------- MIXER-SPECIFIC HELPERS ----------
int getMixerPort(int model) {
  switch (model) {
    case MIXER_XAIR: return 10024;
    case MIXER_WING: return 2223;
    case MIXER_X32:
    default:         return 10023;
  }
}

int getMaxBuses(int model) {
  switch (model) {
    case MIXER_XAIR: return 6;
    default:         return 16;
  }
}

int getMaxChannels(int model) {
  switch (model) {
    case MIXER_XAIR: return 16;
    case MIXER_WING: return 40;
    case MIXER_X32:
    default:         return 32;
  }
}

// ---------- GENERAL HELPERS ----------
String htmlEscape(const String& s) {
  String o;
  o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    switch (c) {
      case '&':  o += "&amp;";  break;
      case '<':  o += "&lt;";   break;
      case '>':  o += "&gt;";   break;
      case '\'': o += "&#39;";  break;
      case '"':  o += "&quot;"; break;
      default:   o += c;
    }
  }
  return o;
}

uint32_t hexToColor(String hex) {
  if (hex.startsWith("#")) hex.remove(0, 1);
  long number = strtol(hex.c_str(), NULL, 16);
  long r = (number >> 16) & 0xFF;
  long g = (number >> 8) & 0xFF;
  long b = number & 0xFF;
  return strip.Color(r, g, b, 0);
}

bool isValidHexColor(const String& s) {
  if (s.length() != 7 || s[0] != '#') return false;
  for (int i = 1; i < 7; i++) {
    if (!isxdigit((unsigned char)s[i])) return false;
  }
  return true;
}

// ---------- BATTERY ----------
void getBatteryStatus(float &voltage, int &percentage) {
  long summV = 0;
  for (int i = 0; i < 20; i++) {
    summV += analogReadMilliVolts(BATTERY_PIN);
    delay(2);
  }
  float avgVoltsOnPin = (summV / 20.0) / 1000.0;
  voltage = avgVoltsOnPin * BATTERY_DIVIDER;

  // Rough linear estimate for a single Li-Ion cell (3.3 V - 4.2 V)
  percentage = (int)(((voltage - 3.3) / (4.2 - 3.3)) * 100.0);
  if (percentage > 100) percentage = 100;
  if (percentage < 0) percentage = 0;
}

// ---------- LED CONTROL ----------
void updateLedBrightness(int percent) {
  uint8_t scaled = map(constrain(percent, 0, 100), 0, 100, 0, 255);
  strip.setBrightness(scaled);
  lastShownColor = 0xFFFFFFFF;
  strip.show();
}

void setAllLeds(uint32_t color) {
  if (color == lastShownColor) return;
  lastShownColor = color;
  for (int i = 0; i < NUM_LEDS; i++) {
    strip.setPixelColor(i, color);
  }
  strip.show();
}

void setBreathingLed(uint8_t r, uint8_t g, uint8_t b, float speed = 0.0025) {
  float breath = (exp(sin(millis() * speed)) - 0.36787944) * 108.0;
  uint8_t red   = (r * breath) / 255;
  uint8_t green = (g * breath) / 255;
  uint8_t blue  = (b * breath) / 255;
  setAllLeds(strip.Color(red, green, blue, 0));
}

void updateLedState() {
  if (isAPMode || WiFi.status() != WL_CONNECTED) return;

  if (!mixerIPValid || !mixer_online) {
    if ((millis() / 500) % 2 == 0) {
      setAllLeds(strip.Color(255, 0, 255, 0));
    } else {
      setAllLeds(COLOR_OFF);
    }
    return;
  }

  setAllLeds(hexToColor(isTalkbackActive ? color_active_hex : color_standby_hex));
}

// ---------- OSC ----------
bool canSend() {
  return !isAPMode && WiFi.status() == WL_CONNECTED && mixerIPValid;
}

// Writes the null-padded OSC address, returns the padded length
static int oscWriteAddress(uint8_t* packet, const char* address) {
  int len = strlen(address);
  memcpy(packet, address, len);
  return ((len + 4) / 4) * 4;
}

static void sendUdpPacket(const uint8_t* data, int len) {
  udp.beginPacket(mixerIP, getMixerPort(mixer_model));
  udp.write(data, len);
  udp.endPacket();
}

// Integer argument (",i") - used by X32 / X Air / WING for on/off and assign parameters
void sendOSCInt(const char* address, int32_t value) {
  if (!canSend()) return;
  uint8_t packet[64];
  memset(packet, 0, sizeof(packet));

  int offset = oscWriteAddress(packet, address);
  packet[offset]     = ',';
  packet[offset + 1] = 'i';
  offset += 4;

  packet[offset]     = (value >> 24) & 0xFF;
  packet[offset + 1] = (value >> 16) & 0xFF;
  packet[offset + 2] = (value >> 8)  & 0xFF;
  packet[offset + 3] =  value        & 0xFF;
  sendUdpPacket(packet, offset + 4);
}

// Float argument (",f") - used for levels
void sendOSCFloat(const char* address, float value) {
  if (!canSend()) return;
  uint8_t packet[64];
  memset(packet, 0, sizeof(packet));

  int offset = oscWriteAddress(packet, address);
  packet[offset]     = ',';
  packet[offset + 1] = 'f';
  offset += 4;

  uint32_t bits;
  memcpy(&bits, &value, 4);
  packet[offset]     = (bits >> 24) & 0xFF;
  packet[offset + 1] = (bits >> 16) & 0xFF;
  packet[offset + 2] = (bits >> 8)  & 0xFF;
  packet[offset + 3] =  bits        & 0xFF;
  sendUdpPacket(packet, offset + 4);
}

// Address without arguments = "give me the current value"
void sendOSCQuery(const char* address) {
  if (!canSend()) return;
  uint8_t packet[64];
  memset(packet, 0, sizeof(packet));
  int len = oscWriteAddress(packet, address);
  sendUdpPacket(packet, len);
}

void sendMixerPing() {
  if (mixer_model == MIXER_WING) {
    sendOSCQuery("/ch/1/name");
  } else {
    sendOSCQuery("/xinfo");
  }
}

// Parses "/ch/NN/mix/MM/level ,f <value>" replies (X Air send levels)
void parseXAirReply(const uint8_t* buf, int len) {
  char addr[40];
  int n = 0;
  while (n < len && buf[n] && n < 39) {
    addr[n] = (char)buf[n];
    n++;
  }
  addr[n] = 0;
  if (n < 7 || strcmp(addr + n - 6, "/level") != 0) return;

  int ch = 0, bus = 0;
  if (sscanf(addr, "/ch/%d/mix/%d/level", &ch, &bus) != 2) return;
  if (ch != channel || bus < 1 || bus > 16) return;

  int tagPos = ((n + 4) / 4) * 4;
  if (tagPos + 8 > len) return;
  if (buf[tagPos] != ',' || buf[tagPos + 1] != 'f') return;

  int v = tagPos + 4;
  uint32_t bits = ((uint32_t)buf[v] << 24) | ((uint32_t)buf[v + 1] << 16) |
                  ((uint32_t)buf[v + 2] << 8) | (uint32_t)buf[v + 3];
  float level;
  memcpy(&level, &bits, 4);

  xairSavedLevel[bus - 1] = level;
  xairLevelKnown[bus - 1] = true;
}

// Reads every pending UDP packet: marks the mixer as online and parses replies
void pollUdp() {
  while (udp.parsePacket() > 0) {
    uint8_t buf[128];
    int len = udp.read(buf, sizeof(buf));
    mixer_online = true;
    lastReplyTime = millis();
    if (len > 0 && mixer_model == MIXER_XAIR) {
      parseXAirReply(buf, len);
    }
  }
}

void checkMixerResponse() {
  if (isAPMode || WiFi.status() != WL_CONNECTED || !mixerIPValid) {
    mixer_online = false;
    return;
  }

  pollUdp();

  if (millis() - lastReplyTime > pingInterval * 2) {
    mixer_online = false;
  }

  if (millis() - lastPingTime >= pingInterval) {
    lastPingTime = millis();
    sendMixerPing();
  }
}

// ---------- TALKBACK LOGIC ----------

// Asks the mixer for the current level of every selected X Air send and waits
// (max 150 ms) for the answers. Unanswered sends fall back to 0 dB (0.75).
void captureXAirLevels() {
  int maxBus = getMaxBuses(mixer_model);
  char addr[40];

  for (int i = 0; i < maxBus; i++) {
    xairLevelKnown[i] = false;
    if (!bus_mute_flags[i]) continue;
    snprintf(addr, sizeof(addr), "/ch/%02d/mix/%02d/level", channel, i + 1);
    sendOSCQuery(addr);
  }

  unsigned long start = millis();
  while (millis() - start < 150) {
    pollUdp();
    bool allKnown = true;
    for (int i = 0; i < maxBus; i++) {
      if (bus_mute_flags[i] && !xairLevelKnown[i]) allKnown = false;
    }
    if (allKnown) break;
    delay(1);
  }

  for (int i = 0; i < maxBus; i++) {
    if (bus_mute_flags[i] && !xairLevelKnown[i]) xairSavedLevel[i] = 0.75f;
  }
}

// enable = true -> channel goes to Main, false -> removed from Main
void sendMainAssign(bool enable) {
  char addr[40];
  switch (mixer_model) {
    case MIXER_XAIR:
      snprintf(addr, sizeof(addr), "/ch/%02d/mix/lr", channel);
      break;
    case MIXER_WING:
      snprintf(addr, sizeof(addr), "/ch/%d/main/1/on", channel);
      break;
    case MIXER_X32:
    default:
      snprintf(addr, sizeof(addr), "/ch/%02d/mix/st", channel);
      break;
  }
  sendOSCInt(addr, enable ? 1 : 0);
}

// enable = true -> sends restored, false -> sends switched off
void sendBusSends(bool enable) {
  int maxBus = getMaxBuses(mixer_model);
  char addr[40];

  for (int i = 0; i < maxBus; i++) {
    if (!bus_mute_flags[i]) continue;

    switch (mixer_model) {
      case MIXER_XAIR:
        snprintf(addr, sizeof(addr), "/ch/%02d/mix/%02d/level", channel, i + 1);
        sendOSCFloat(addr, enable ? xairSavedLevel[i] : 0.0f);
        break;
      case MIXER_WING:
        snprintf(addr, sizeof(addr), "/ch/%d/send/%d/on", channel, i + 1);
        sendOSCInt(addr, enable ? 1 : 0);
        break;
      case MIXER_X32:
      default:
        snprintf(addr, sizeof(addr), "/ch/%02d/mix/%02d/on", channel, i + 1);
        sendOSCInt(addr, enable ? 1 : 0);
        break;
    }
    delay(1);
  }
}

void setTalkback(bool active) {
  bool wasActive = isTalkbackActive;
  isTalkbackActive = active;
  updateLedState();

  if (channel <= 0 || !canSend()) return;

  if (active && !wasActive && mixer_model == MIXER_XAIR) {
    captureXAirLevels();
  }

  for (int pass = 0; pass < OSC_PASSES; pass++) {
    sendMainAssign(!active);
    sendBusSends(!active);
    if (pass < OSC_PASSES - 1) delay(10);
  }
}

// ---------- AJAX STATUS ----------
void handleApiStatus() {
  float voltage = 0.0;
  int percentage = 0;
  getBatteryStatus(voltage, percentage);

  String json = "{";
  json += "\"talkback\":" + String(isTalkbackActive ? "true" : "false") + ",";
  json += "\"rssi\":" + String(WiFi.RSSI()) + ",";
  json += "\"mixer_online\":" + String(mixer_online ? "true" : "false") + ",";
  json += "\"battery_pct\":" + String(percentage) + ",";
  json += "\"battery_v\":" + String(voltage, 2);
  json += "}";
  server.send(200, "application/json; charset=utf-8", json);
}

// ---------- WEB INTERFACE ----------
const char PAGE_STYLE_SCRIPT[] PROGMEM = R"rawliteral(
<style>
body{font-family:Arial,sans-serif;margin:20px;background:#1a1a1a;color:#fff;}
input[type=text],input[type=number],input[type=password],select{width:100%;padding:10px;margin:8px 0;box-sizing:border-box;border-radius:5px;border:none;}
input[type=range]{width:100%;margin:10px 0;}
input[type=color]{width:100%;height:40px;border:none;border-radius:5px;margin:8px 0;cursor:pointer;}
input[type=submit]{background:#007bff;color:#fff;font-weight:bold;cursor:pointer;padding:12px;border:none;border-radius:5px;width:100%;margin-top:10px;}
.card{background:#2a2a2a;padding:20px;border-radius:10px;max-width:420px;margin:auto;} a{color:#007bff;}
.pass-box{position:relative;display:flex;align-items:center;}
.pass-box input{padding-right:45px;}
.toggle-btn{position:absolute;right:10px;background:none;border:none;color:#aaa;cursor:pointer;font-size:18px;padding:5px;}
.range-container{display:flex;align-items:center;justify-content:space-between;gap:10px;}
.range-val{font-weight:bold;min-width:40px;text-align:right;}
.bus-grid{display:grid;grid-template-columns:repeat(4,1fr);gap:8px;margin:10px 0 15px 0;}
.bus-item{background:#3a3a3a;border-radius:5px;padding:8px;text-align:center;font-size:12px;font-weight:bold;cursor:pointer;user-select:none;}
.bus-item input{display:none;}
.bus-item:has(input:checked){background:#dc3545;color:#fff;}
.status-box{background:#333;padding:12px;border-radius:5px;margin-bottom:15px;}
.badge{padding:3px 8px;border-radius:3px;font-weight:bold;font-size:12px;}
.bg-online{background:#28a745;color:#fff;} .bg-offline{background:#dc3545;color:#fff;}
.row-inline{display:flex;gap:10px;align-items:center;}
.battery-box{background:#222;padding:10px;border-radius:8px;margin-top:10px;}
.progress-bg{width:100%;background:#444;height:18px;border-radius:9px;overflow:hidden;margin-top:5px;border:1px solid #555;}
.progress-fill{height:100%;width:0%;background:#28a745;transition:width 0.4s,background 0.4s;}
</style>
<script>
function togglePass(){var p=document.getElementById('pass-field'),b=document.getElementById('toggle-btn');
  if(p.type==='password'){p.type='text';b.innerHTML='\uD83D\uDD13';}else{p.type='password';b.innerHTML='\uD83D\uDD12';}}
function updateBrightVal(v){document.getElementById('bright-val').innerText=v+'%';}
function updateModel(){
  var m=parseInt(document.getElementById('mixer-select').value);
  var maxBus=(m===1)?6:16;
  var maxCh=(m===0)?32:((m===1)?16:40);
  for(var i=1;i<=16;i++){var el=document.getElementById('bus-box-'+i);if(el){el.style.display=(i<=maxBus)?'block':'none';}}
  var c=document.getElementById('ch-input');c.max=maxCh;c.placeholder='1-'+maxCh;
}
function toggleStaticIp(){
  var chk=document.getElementById('use-static').checked;
  document.getElementById('static-box').style.display=chk?'block':'none';
}
setInterval(function(){
  if(!document.getElementById('mixer-status'))return;
  fetch('/api/status').then(function(r){return r.json();}).then(function(d){
    var st=document.getElementById('mixer-status');
    if(d.mixer_online){st.innerText='ONLINE';st.className='badge bg-online';}
    else{st.innerText='OFFLINE / UNREACHABLE';st.className='badge bg-offline';}
    document.getElementById('wifi-rssi').innerText=d.rssi+' dBm';
    document.getElementById('tb-state').innerText=d.talkback?'TALKBACK ACTIVE':'LIVE (FOH)';
    document.getElementById('bat-pct').innerText=d.battery_pct+'%';
    document.getElementById('bat-v').innerText=d.battery_v.toFixed(2)+' V';
    var bar=document.getElementById('bat-bar');
    bar.style.width=d.battery_pct+'%';
    if(d.battery_pct>50)bar.style.backgroundColor='#28a745';
    else if(d.battery_pct>20)bar.style.backgroundColor='#ffc107';
    else bar.style.backgroundColor='#dc3545';
  }).catch(function(e){});
},2000);
window.onload=function(){updateModel();toggleStaticIp();};
</script>
)rawliteral";

void handleRoot() {
  if (isAPMode && server.hostHeader() != "5.5.5.5") {
    server.sendHeader("Location", "http://5.5.5.5/", true);
    server.send(302, "text/plain", "");
    return;
  }

  const Translations& t = txt[currentLang];

  String html;
  html.reserve(10000);
  html += "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<title>" + String(t.title) + "</title>";
  html += FPSTR(PAGE_STYLE_SCRIPT);
  html += "</head><body>";
  html += "<div class='card'><h2>" + String(t.title) + "</h2>";

  if (!isAPMode) {
    html += "<div class='status-box'>";
    html += "<p style='margin:3px 0;'><b>Wi-Fi:</b> " + WiFi.localIP().toString() + " (<span id='wifi-rssi'>" + String(WiFi.RSSI()) + " dBm</span>)</p>";
    html += "<p style='margin:3px 0;'><b>Mixer Link:</b> <span id='mixer-status' class='badge " + String(mixer_online ? "bg-online" : "bg-offline") + "'>" + String(mixer_online ? "ONLINE" : "OFFLINE / UNREACHABLE") + "</span></p>";
    html += "<p style='margin:3px 0;'><b>Mode:</b> <span id='tb-state'>" + String(isTalkbackActive ? "TALKBACK ACTIVE" : "LIVE (FOH)") + "</span></p>";
    html += "<div class='battery-box'>";
    html += "<div style='display:flex;justify-content:space-between;font-size:13px;'><b>Battery:</b> <span id='bat-pct'>0%</span> (<span id='bat-v'>0.00 V</span>)</div>";
    html += "<div class='progress-bg'><div id='bat-bar' class='progress-fill'></div></div>";
    html += "</div>";
    html += "<p style='margin:8px 0 0 0;font-size:12px;'><b>Host:</b> <a href='http://" + htmlEscape(mdns_name) + ".local' target='_blank'>http://" + htmlEscape(mdns_name) + ".local</a></p>";
    html += "</div>";
  }

  html += "<form action='/save' method='POST'>";

  // Language
  html += "<label>" + String(t.selectLang) + "</label><select name='lang'>";
  html += "<option value='0'" + String(currentLang == LANG_EN ? " selected" : "") + ">English</option>";
  html += "<option value='1'" + String(currentLang == LANG_DE ? " selected" : "") + ">Deutsch</option>";
  html += "<option value='2'" + String(currentLang == LANG_UA ? " selected" : "") + ">Українська</option>";
  html += "<option value='3'" + String(currentLang == LANG_RU ? " selected" : "") + ">Русский</option>";
  html += "</select>";

  // Wi-Fi (the network list is only available in setup/AP mode)
  if (isAPMode && wifiOptionsHtml.length() > 0) {
    html += "<label>" + String(t.selectWifi) + "</label><select name='ssid'>" + wifiOptionsHtml + "</select>";
    html += "<label>" + String(t.manualSsid) + "</label><input type='text' name='manual_ssid' placeholder='SSID'>";
  } else {
    html += "<label>Wi-Fi SSID:</label><input type='text' name='manual_ssid' placeholder='SSID' value='" + htmlEscape(isAPMode ? String("") : wifi_ssid) + "'>";
  }

  html += "<label>" + String(t.wifiPass) + "</label>";
  html += "<div class='pass-box'>";
  html += "<input type='password' id='pass-field' name='password' placeholder='(unchanged)'>";
  html += "<button type='button' id='toggle-btn' class='toggle-btn' onclick='togglePass()'>&#128274;</button>";
  html += "</div>";

  // mDNS and static IP
  html += "<label>mDNS Hostname (.local)</label>";
  html += "<input type='text' name='mdns' value='" + htmlEscape(mdns_name) + "' placeholder='talkback'>";

  html += "<div style='margin:10px 0;'>";
  html += "<label><input type='checkbox' id='use-static' name='use_static' value='1'" + String(use_static_ip ? " checked" : "") + " onchange='toggleStaticIp()'> Use Static IP for ESP32</label>";
  html += "</div>";

  html += "<div id='static-box' style='display:none;'>";
  html += "<label>ESP32 Static IP</label><input type='text' name='static_ip' placeholder='192.168.1.50' value='" + htmlEscape(static_ip_str) + "'>";
  html += "<label>Gateway (Router IP)</label><input type='text' name='gateway' placeholder='192.168.1.1' value='" + htmlEscape(gateway_str) + "'>";
  html += "<label>Subnet Mask</label><input type='text' name='subnet' placeholder='255.255.255.0' value='" + htmlEscape(subnet_str) + "'>";
  html += "</div>";

  // Mixer
  html += "<label>" + String(t.mixerModelLabel) + "</label>";
  html += "<select name='model' id='mixer-select' onchange='updateModel()'>";
  html += "<option value='0'" + String(mixer_model == MIXER_X32 ? " selected" : "") + ">Behringer X32 / Midas M32</option>";
  html += "<option value='1'" + String(mixer_model == MIXER_XAIR ? " selected" : "") + ">Behringer X Air / Midas MR</option>";
  html += "<option value='2'" + String(mixer_model == MIXER_WING ? " selected" : "") + ">Behringer WING / WING Rack</option>";
  html += "</select>";

  html += "<label>" + String(t.mixerIp) + "</label>";
  html += "<input type='text' name='mixerip' placeholder='192.168.1.100' value='" + htmlEscape(mixer_ip_str) + "'>";

  String channelVal = (channel > 0) ? String(channel) : "";
  html += "<label>" + String(t.channel) + "</label>";
  html += "<input type='number' id='ch-input' name='channel' min='1' max='" + String(getMaxChannels(mixer_model)) + "' value='" + channelVal + "'>";

  // Button mode
  html += "<label>" + String(t.btnModeLabel) + "</label><select name='btnmode'>";
  html += "<option value='0'" + String(button_mode == 0 ? " selected" : "") + ">" + String(t.btnModePTT) + "</option>";
  html += "<option value='1'" + String(button_mode == 1 ? " selected" : "") + ">" + String(t.btnModeLatch) + "</option>";
  html += "</select>";

  // Brightness
  html += "<label>" + String(t.ledBrightnessLabel) + "</label>";
  html += "<div class='range-container'>";
  html += "<input type='range' name='brightness' min='0' max='100' value='" + String(led_brightness) + "' oninput='updateBrightVal(this.value)'>";
  html += "<span id='bright-val' class='range-val'>" + String(led_brightness) + "%</span>";
  html += "</div>";

  // Colors
  html += "<div class='row-inline'>";
  html += "<div style='flex:1;'><label>Standby (FOH Live)</label><input type='color' name='col_standby' value='" + color_standby_hex + "'></div>";
  html += "<div style='flex:1;'><label>Active (Talkback)</label><input type='color' name='col_active' value='" + color_active_hex + "'></div>";
  html += "</div>";

  // Buses to mute
  html += "<label><b>" + String(t.busSelectTitle) + "</b></label>";
  html += "<div class='bus-grid'>";
  for (int i = 0; i < 16; i++) {
    String chk = bus_mute_flags[i] ? " checked" : "";
    html += "<label class='bus-item' id='bus-box-" + String(i + 1) + "'>";
    html += "<input type='checkbox' name='bus" + String(i + 1) + "' value='1'" + chk + ">";
    html += "BUS " + String(i + 1);
    html += "</label>";
  }
  html += "</div>";

  html += "<input type='submit' value='" + String(t.btnSave) + "'>";
  html += "</form></div></body></html>";

  server.send(200, "text/html; charset=utf-8", html);
}

// ---------- SAVE SETTINGS ----------
void sendSaveError(const String& message) {
  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1.0'></head>";
  html += "<body style='background:#1a1a1a;color:#fff;font-family:Arial;text-align:center;padding-top:50px;'>";
  html += "<h2>" + htmlEscape(message) + "</h2><p><a style='color:#007bff' href='/'>&larr; Back</a></p></body></html>";
  server.send(400, "text/html; charset=utf-8", html);
}

void handleSave() {
  // --- validate first, so nothing is changed on bad input ---
  String newMixerIp = server.arg("mixerip");
  newMixerIp.trim();
  if (newMixerIp.length() > 0) {
    IPAddress testIp;
    if (!testIp.fromString(newMixerIp)) {
      sendSaveError("Invalid mixer IP address");
      return;
    }
  }

  bool newUseStatic = (server.arg("use_static") == "1");
  String newStaticIp = server.arg("static_ip");  newStaticIp.trim();
  String newGateway  = server.arg("gateway");    newGateway.trim();
  String newSubnet   = server.arg("subnet");     newSubnet.trim();
  if (newUseStatic) {
    IPAddress a, b, c;
    if (!a.fromString(newStaticIp) || !b.fromString(newGateway) || !c.fromString(newSubnet)) {
      sendSaveError("Invalid static IP / gateway / subnet");
      return;
    }
  }

  // --- Wi-Fi ---
  String newSsid = server.arg("manual_ssid");
  newSsid.trim();
  if (newSsid.length() == 0) newSsid = server.arg("ssid");
  newSsid.trim();
  if (newSsid.length() == 0) newSsid = wifi_ssid;

  String newPass = server.arg("password");
  // Empty password field + same network = keep the stored password
  if (newPass.length() == 0 && newSsid == wifi_ssid) newPass = wifi_pass;

  // --- apply ---
  wifi_ssid = newSsid;
  wifi_pass = newPass;
  mixer_ip_str = newMixerIp;

  mixer_model = constrain(server.arg("model").toInt(), 0, 2);
  channel = server.arg("channel").toInt();
  if (channel < 0) channel = 0;
  if (channel > getMaxChannels(mixer_model)) channel = getMaxChannels(mixer_model);
  button_mode = constrain(server.arg("btnmode").toInt(), 0, 1);
  led_brightness = constrain(server.arg("brightness").toInt(), 0, 100);
  currentLang = constrain(server.arg("lang").toInt(), 0, 3);

  use_static_ip = newUseStatic;
  static_ip_str = newStaticIp;
  gateway_str   = newGateway;
  subnet_str    = newSubnet;
  mdns_name     = server.arg("mdns");
  mdns_name.trim();
  if (mdns_name.length() == 0) mdns_name = "talkback";

  color_standby_hex = server.arg("col_standby");
  color_active_hex  = server.arg("col_active");
  if (!isValidHexColor(color_standby_hex)) color_standby_hex = "#0000FF";
  if (!isValidHexColor(color_active_hex))  color_active_hex  = "#00FF00";

  for (int i = 0; i < 16; i++) {
    bus_mute_flags[i] = (server.arg("bus" + String(i + 1)) == "1");
  }

  if (!preferences.begin("settings", false)) {
    sendSaveError("Cannot open flash storage");
    return;
  }
  preferences.putString("ssid", wifi_ssid);
  preferences.putString("pass", wifi_pass);
  preferences.putInt("model", mixer_model);
  size_t written = preferences.putString("mixerip", mixer_ip_str);
  preferences.putInt("channel", channel);
  preferences.putInt("btnmode", button_mode);
  preferences.putInt("brightness", led_brightness);
  preferences.putInt("lang", currentLang);

  preferences.putBool("usestatic", use_static_ip);
  preferences.putString("staticip", static_ip_str);
  preferences.putString("gateway", gateway_str);
  preferences.putString("subnet", subnet_str);
  preferences.putString("mdns", mdns_name);

  preferences.putString("c_standby", color_standby_hex);
  preferences.putString("c_active", color_active_hex);

  preferences.putBytes("busmutes", bus_mute_flags, sizeof(bus_mute_flags));
  preferences.end();

  // Read back, so problems with flash storage are visible in the Serial Monitor
  preferences.begin("settings", true);
  String check = preferences.getString("mixerip", "<none>");
  preferences.end();
  Serial.printf("SAVE mixerip='%s' written=%d readback='%s'\n",
                mixer_ip_str.c_str(), (int)written, check.c_str());

  const Translations& t = txt[currentLang];
  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<meta http-equiv='refresh' content='8;url=/'>";
  html += "</head><body style='background:#1a1a1a;color:#fff;font-family:Arial;text-align:center;padding-top:50px;'>";
  html += "<h2>" + String(t.msgSaved) + "</h2><p>" + String(t.msgRebooting) + "</p></body></html>";
  server.send(200, "text/html; charset=utf-8", html);

  delay(1500);
  ESP.restart();
}

// ---------- SERVER, ACCESS POINT, WI-FI ----------
void setupServerRoutes() {
  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/api/status", handleApiStatus);
  server.onNotFound(handleRoot);
}

void startServerOnce() {
  if (serverStarted) return;
  server.begin();
  serverStarted = true;
}

// One scan when the setup AP starts (a scan on every page load would block the button)
void scanNetworksOnce() {
  wifiOptionsHtml = "";
  int n = WiFi.scanNetworks();
  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) continue;
    String sel = (ssid == wifi_ssid) ? " selected" : "";
    wifiOptionsHtml += "<option value='" + htmlEscape(ssid) + "'" + sel + ">" + htmlEscape(ssid) + " (" + String(WiFi.RSSI(i)) + " dBm)</option>";
  }
  WiFi.scanDelete();
}

void startAPMode() {
  isAPMode = true;
  WiFi.mode(WIFI_AP_STA);
  scanNetworksOnce();

  IPAddress apIP(5, 5, 5, 5);
  IPAddress netMsk(255, 255, 255, 0);
  WiFi.softAPConfig(apIP, apIP, netMsk);
  WiFi.softAP("ESP32-Talkback-Setup");
  dnsServer.start(53, "*", apIP);
  startServerOnce();
}

void handleWiFiConnection() {
  if (isAPMode) return;

  unsigned long now = millis();

  if (WiFi.status() != WL_CONNECTED) {
    if (wifiWasConnected) {
      wifiWasConnected = false;
      wifiConnectStart = now;
    }

    setBreathingLed(255, 180, 0, 0.003);

    if (now - wifiConnectStart >= wifiTimeout) {
      WiFi.disconnect(true);
      startAPMode();
      return;
    }

    if (now - lastWifiCheck >= wifiCheckInterval) {
      lastWifiCheck = now;
      WiFi.reconnect();
    }

  } else {
    if (!wifiWasConnected) {
      wifiWasConnected = true;
      Serial.printf("WiFi connected. IP: %s  (http://%s.local)\n",
                    WiFi.localIP().toString().c_str(), mdns_name.c_str());

      for (int i = 0; i < 3; i++) {
        setAllLeds(hexToColor(color_standby_hex));
        delay(100);
        setAllLeds(COLOR_OFF);
        delay(100);
      }

      MDNS.begin(mdns_name.c_str());
      udp.stop();
      udp.begin(getMixerPort(mixer_model));
      updateLedState();
    }
  }
}

// ---------- LOAD SETTINGS ----------
void loadSettings() {
  preferences.begin("settings", true);
  wifi_ssid = preferences.getString("ssid", "");
  wifi_pass = preferences.getString("pass", "");
  mixer_model = preferences.getInt("model", MIXER_X32);
  mixer_ip_str = preferences.getString("mixerip", "");
  channel = preferences.getInt("channel", 0);
  button_mode = preferences.getInt("btnmode", 0);
  led_brightness = preferences.getInt("brightness", 30);
  currentLang = preferences.getInt("lang", LANG_UA);

  use_static_ip = preferences.getBool("usestatic", false);
  static_ip_str = preferences.getString("staticip", "");
  gateway_str   = preferences.getString("gateway", "");
  subnet_str    = preferences.getString("subnet", "255.255.255.0");
  mdns_name     = preferences.getString("mdns", "talkback");

  color_standby_hex = preferences.getString("c_standby", "#0000FF");
  color_active_hex  = preferences.getString("c_active", "#00FF00");

  size_t bytesRead = preferences.getBytes("busmutes", bus_mute_flags, sizeof(bus_mute_flags));
  if (bytesRead != sizeof(bus_mute_flags)) {
    memset(bus_mute_flags, 0, sizeof(bus_mute_flags));
  }
  preferences.end();

  // Sanitize (also handles values saved by older versions with other mixer models)
  if (mixer_model < 0 || mixer_model > 2) mixer_model = MIXER_X32;
  if (channel < 0 || channel > getMaxChannels(mixer_model)) channel = 0;
  button_mode = constrain(button_mode, 0, 1);
  led_brightness = constrain(led_brightness, 0, 100);
  if (currentLang < 0 || currentLang > 3) currentLang = LANG_UA;
  if (!isValidHexColor(color_standby_hex)) color_standby_hex = "#0000FF";
  if (!isValidHexColor(color_active_hex))  color_active_hex  = "#00FF00";
  if (mdns_name.length() == 0) mdns_name = "talkback";

  mixer_ip_str.trim();
  mixerIPValid = (mixer_ip_str.length() > 0) && mixerIP.fromString(mixer_ip_str);

  for (int i = 0; i < 16; i++) {
    xairSavedLevel[i] = 0.75f;
    xairLevelKnown[i] = false;
  }

  Serial.printf("LOAD ssid='%s' model=%d mixerip='%s' channel=%d\n",
                wifi_ssid.c_str(), mixer_model, mixer_ip_str.c_str(), channel);
}

// ---------- SETUP ----------
void setup() {
  Serial.begin(115200);

  pinMode(buttonPin, INPUT_PULLUP);
  for (int i = 0; i < resetPinCount; i++) {
    pinMode(resetPins[i], INPUT_PULLUP);
  }

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  loadSettings();

  strip.begin();
  updateLedBrightness(led_brightness);
  setAllLeds(COLOR_OFF);

  setupServerRoutes();

  if (wifi_ssid == "") {
    startAPMode();
  } else {
    WiFi.persistent(false);   // credentials are kept in Preferences, no extra flash wear
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);

    if (use_static_ip && static_ip_str.length() > 0) {
      IPAddress ip, gw, sn;
      if (ip.fromString(static_ip_str) && gw.fromString(gateway_str) && sn.fromString(subnet_str)) {
        WiFi.config(ip, gw, sn);
      }
    }

    WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());
    wifiConnectStart = millis();
    startServerOnce();
  }
}

// ---------- LOOP ----------
void loop() {
  // 1. Reset pins (hold for 3 seconds to erase all settings)
  if (isResetPressed()) {
    if (resetHoldStart == 0) {
      resetHoldStart = millis();
    } else if (millis() - resetHoldStart > 3000 && !resetHandled) {
      resetHandled = true;
      preferences.begin("settings", false);
      preferences.clear();
      preferences.end();

      for (int i = 0; i < 10; i++) {
        setAllLeds(strip.Color(255, 255, 255, 255));
        delay(100);
        setAllLeds(COLOR_OFF);
        delay(100);
      }
      ESP.restart();
    }
  } else {
    resetHoldStart = 0;
  }

  // 2. Setup (AP) mode
  if (isAPMode) {
    dnsServer.processNextRequest();
    server.handleClient();
    setBreathingLed(0, 255, 0, 0.002);
    return;
  }

  // 3. Wi-Fi manager
  handleWiFiConnection();

  // 4. Web server
  server.handleClient();

  // 5. Mixer connection check + LEDs
  checkMixerResponse();
  updateLedState();

  // 6. Button logic
  if (WiFi.status() == WL_CONNECTED) {
    bool reading = digitalRead(buttonPin);

    if (reading != lastButtonState) {
      lastDebounceTime = millis();
    }

    if ((millis() - lastDebounceTime) > debounceDelay) {
      if (reading != stableButtonState) {
        stableButtonState = reading;

        if (button_mode == 0) {
          // Push-To-Talk
          setTalkback(stableButtonState == LOW);
        } else {
          // Latch
          if (stableButtonState == LOW) {
            latchActiveState = !latchActiveState;
            setTalkback(latchActiveState);
          }
        }
      }
    }
    lastButtonState = reading;
  }
}
