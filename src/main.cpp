#include <Arduino.h>
#include <EEPROM.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266HTTPUpdateServer.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <IRrecv.h>
#include <IRsend.h>
#include <IRutils.h>
#include <LittleFS.h>
#include <new>
#include <time.h>
#define BLINKER_PRINT Serial
#define BLINKER_WIFI
#define BLINKER_ALIGENIE_MULTI_OUTLET
#define BLINKER_DUEROS_MULTI_OUTLET
void blinkerInboundAliGenie(const char* data);
void blinkerInboundAliGenieParsed();
#include <Blinker.h>
#include <RCSwitch.h>

#ifndef ENABLE_IR_AC_PRESETS
#define ENABLE_IR_AC_PRESETS 0
#endif

#if ENABLE_IR_AC_PRESETS
#include <IRac.h>
#endif

// HW-364A 板载 OLED 通常使用软件 I2C：clock=D6(GPIO12), data=D5(GPIO14)。
constexpr uint8_t RF_TX_PIN = D7;       // GPIO13
constexpr uint8_t RF_RX_BUILTIN_OLED_PIN = D2;  // GPIO4，板载 OLED 不占用 D2
constexpr uint8_t RF_RX_ALT_PIN = D5;   // GPIO14，新板 OLED 占用 D2 时使用
constexpr uint8_t IR_TX_PIN = D0;       // GPIO16，接 HW-477 发射模块 S
constexpr uint8_t IR_RX_PIN = D4;       // GPIO2，接独立红外接收头 S，避免占用 OLED D5/D6 探测线
constexpr uint8_t OLED_CLOCK_PIN = D6;  // GPIO12
constexpr uint8_t OLED_DATA_PIN = D5;   // GPIO14

constexpr uint16_t EEPROM_SIZE = 512;
constexpr uint32_t SETTINGS_MAGIC = 0x4330A11D;
constexpr uint32_t PREVIOUS_SETTINGS_MAGIC = 0x4330A11C;
constexpr uint32_t OLD_SETTINGS_MAGIC = 0x4330A11B;
constexpr uint8_t RF_REPEAT_TRANSMIT = 8;
constexpr uint8_t DEFAULT_VOICE_RF_REPEAT_TRANSMIT = 12;
constexpr uint8_t MIN_VOICE_RF_REPEAT_TRANSMIT = 4;
constexpr uint8_t MAX_VOICE_RF_REPEAT_TRANSMIT = 40;
constexpr uint16_t SCREEN_WIDTH = 128;
constexpr uint16_t SCREEN_HEIGHT = 64;
constexpr unsigned long WIFI_JOIN_TIMEOUT_MS = 15000UL;
constexpr unsigned long OLED_STATUS_REFRESH_MS = 15000UL;
constexpr unsigned long OLED_TITLE_REFRESH_MS = 60000UL;
constexpr unsigned long TIME_SYNC_INTERVAL_MS = 3600000UL;
constexpr unsigned long TIME_SYNC_RETRY_MS = 60000UL;
constexpr uint8_t VOICE_OUTLET_COUNT = 6;
constexpr uint8_t RF_SLOT_COUNT = 6;
constexpr uint8_t IR_SLOT_COUNT = 6;
constexpr uint8_t LOG_LINE_COUNT = 40;
constexpr uint16_t LOG_LINE_MAX = 120;
constexpr uint16_t IR_RAW_MAX = 256;
constexpr uint16_t IR_CAPTURE_BUFFER_SIZE = 512;
constexpr uint8_t IR_TIMEOUT_MS = 50;
constexpr uint16_t IR_DEFAULT_FREQUENCY_KHZ = 38;
constexpr uint32_t RF_SELF_TEST_VALUE = 0x5AA55AUL;
constexpr uint8_t RF_SELF_TEST_BITS = 24;
constexpr uint8_t RF_SELF_TEST_PROTOCOL = 1;
constexpr uint16_t RF_SELF_TEST_PULSE = 350;
constexpr unsigned long RF_SELF_TEST_WAIT_MS = 1500UL;

const char* AP_SSID = "ESP8266-433-Setup";
const char* AP_PASSWORD = "433remote";
const char* TIME_SYNC_URL = "http://worldtimeapi.org/api/timezone/Asia/Shanghai";
const char* NTP_SERVER_1 = "ntp.aliyun.com";
const char* NTP_SERVER_2 = "pool.ntp.org";
const char* NTP_SERVER_3 = "time.windows.com";
const char* RF_CODES_PATH = "/rf_codes.txt";
const char* IR_CODES_PATH = "/ir_codes.txt";
const char* SETTINGS_BACKUP_PATH = "/settings.txt";

struct RfCode {
  uint32_t value;
  uint8_t bits;
  uint8_t protocol;
  uint16_t pulseLength;
};

struct Settings {
  uint32_t magic;
  char wifiSsid[33];
  char wifiPassword[65];
  char blinkerAuth[65];
  uint8_t voiceRfRepeat;
  RfCode codes[3];
};

struct PreviousSettings {
  uint32_t magic;
  char wifiSsid[33];
  char wifiPassword[65];
  char blinkerAuth[65];
  RfCode codes[3];
};

struct OldSettings {
  uint32_t magic;
  char wifiSsid[33];
  char wifiPassword[65];
  RfCode codes[3];
};

struct RfSlot {
  char name[17];
  RfCode code;
};

struct IrSlot {
  char name[17];
  uint16_t frequency;
  uint16_t length;
  uint16_t* raw;
  bool preset;
  decode_type_t protocol;
  int16_t model;
  bool power;
  uint8_t mode;
  uint8_t temp;
  uint8_t fan;
};

Settings settings;
RfSlot rfSlots[RF_SLOT_COUNT];
IrSlot irSlots[IR_SLOT_COUNT];
RCSwitch rf = RCSwitch();
IRsend irsend(IR_TX_PIN);
#if ENABLE_IR_AC_PRESETS
IRac irac(IR_TX_PIN);
#endif
IRrecv irrecv(IR_RX_PIN, IR_CAPTURE_BUFFER_SIZE, IR_TIMEOUT_MS, true);
decode_results irResults;
ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;

String lastMessage = "Booting";
RfCode lastReceived = {0, 0, 0, 0};
unsigned long lastReceivedAt = 0;
uint8_t learnSlot = 0;
unsigned long learnDeadline = 0;
bool rfLearning = false;
bool learnedCodeReady = false;
RfCode learnedCode = {0, 0, 0, 0};
bool irLearning = false;
bool learnedIrReady = false;
uint16_t learnedIrFrequency = IR_DEFAULT_FREQUENCY_KHZ;
uint16_t learnedIrLength = 0;
uint16_t learnedIrRaw[IR_RAW_MAX] = {0};
unsigned long lastOledSystemRefresh = 0;
unsigned long lastOledTitleRefresh = 0;
unsigned long lastTimeSyncAttempt = 0;
unsigned long lastTimeSyncAt = 0;
wl_status_t lastOledWifiStatus = WL_IDLE_STATUS;
IPAddress lastOledIp;
uint32_t syncedLocalEpoch = 0;
bool voiceStarted = false;
bool voiceOutletState[VOICE_OUTLET_COUNT + 1] = {false};
uint32_t aliGenieCommandCount = 0;
uint32_t lastAliGenieCommandAt = 0;
uint32_t aliGenieRawCount = 0;
uint32_t aliGenieParsedCount = 0;
uint32_t aliGenieParsedBeforeRaw = 0;
bool timeSynced = false;
bool oledReady = false;
bool fsReady = false;
uint8_t rfRxPin = RF_RX_BUILTIN_OLED_PIN;
uint8_t oledSdaPin = OLED_DATA_PIN;
uint8_t oledSclPin = OLED_CLOCK_PIN;
uint8_t oledAddress = 0x3C;
char logLines[LOG_LINE_COUNT][LOG_LINE_MAX];
uint8_t logLineNext = 0;
uint8_t logLineUsed = 0;

void drawStatus();
void saveSettingsBackup();
bool restoreSettingsBackup();

const uint8_t FONT_5X7[][5] PROGMEM = {
  {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},{0x14,0x7F,0x14,0x7F,0x14},
  {0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},{0x36,0x49,0x55,0x22,0x50},{0x00,0x05,0x03,0x00,0x00},
  {0x00,0x1C,0x22,0x41,0x00},{0x00,0x41,0x22,0x1C,0x00},{0x14,0x08,0x3E,0x08,0x14},{0x08,0x08,0x3E,0x08,0x08},
  {0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x60,0x60,0x00,0x00},{0x20,0x10,0x08,0x04,0x02},
  {0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},{0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},
  {0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
  {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E},{0x00,0x36,0x36,0x00,0x00},{0x00,0x56,0x36,0x00,0x00},
  {0x08,0x14,0x22,0x41,0x00},{0x14,0x14,0x14,0x14,0x14},{0x00,0x41,0x22,0x14,0x08},{0x02,0x01,0x51,0x09,0x06},
  {0x32,0x49,0x79,0x41,0x3E},{0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
  {0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},{0x3E,0x41,0x49,0x49,0x7A},
  {0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},{0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},
  {0x7F,0x40,0x40,0x40,0x40},{0x7F,0x02,0x0C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
  {0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},{0x46,0x49,0x49,0x49,0x31},
  {0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},{0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},
  {0x63,0x14,0x08,0x14,0x63},{0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43},{0x00,0x7F,0x41,0x41,0x00},
  {0x02,0x04,0x08,0x10,0x20},{0x00,0x41,0x41,0x7F,0x00},{0x04,0x02,0x01,0x02,0x04},{0x40,0x40,0x40,0x40,0x40}
};

bool wifiConnected() {
  return WiFi.status() == WL_CONNECTED;
}

void addLog(const char* format, ...) {
  char message[LOG_LINE_MAX - 16];
  va_list args;
  va_start(args, format);
  vsnprintf(message, sizeof(message), format, args);
  va_end(args);

  uint32_t totalSeconds = timeSynced
      ? syncedLocalEpoch + (millis() - lastTimeSyncAt) / 1000UL
      : millis() / 1000UL;
  uint8_t hours = timeSynced
      ? (totalSeconds % 86400UL) / 3600UL
      : (totalSeconds / 3600UL) % 100UL;
  snprintf(logLines[logLineNext],
           LOG_LINE_MAX,
           "%02u:%02u:%02u %s",
           hours,
           static_cast<uint8_t>((totalSeconds / 60UL) % 60UL),
           static_cast<uint8_t>(totalSeconds % 60UL),
           message);
  Serial.println(logLines[logLineNext]);
  logLineNext = (logLineNext + 1) % LOG_LINE_COUNT;
  if (logLineUsed < LOG_LINE_COUNT) logLineUsed++;
}

void blinkerInboundAliGenie(const char* data) {
  uint32_t rawNumber = ++aliGenieRawCount;
  aliGenieParsedBeforeRaw = aliGenieParsedCount;
  addLog("AliGenie RAW #%lu %s",
         static_cast<unsigned long>(rawNumber),
         data != nullptr && data[0] != '\0' ? data : "<empty>");
}

void blinkerInboundAliGenieParsed() {
  if (aliGenieParsedCount == aliGenieParsedBeforeRaw) {
    addLog("AliGenie rejected RAW #%lu no callback",
           static_cast<unsigned long>(aliGenieRawCount));
  }
}

bool hasWifiCredentials() {
  return settings.wifiSsid[0] != '\0';
}

bool hasBlinkerAuth() {
  return settings.blinkerAuth[0] != '\0';
}

uint8_t normalizedVoiceRfRepeat(uint8_t repeat) {
  if (repeat < MIN_VOICE_RF_REPEAT_TRANSMIT || repeat > MAX_VOICE_RF_REPEAT_TRANSMIT) {
    return DEFAULT_VOICE_RF_REPEAT_TRANSMIT;
  }
  return repeat;
}

IPAddress deviceIP() {
  return wifiConnected() ? WiFi.localIP() : WiFi.softAPIP();
}

uint8_t gpioNumber(uint8_t pin) {
  switch (pin) {
    case D0: return 16;
    case D1: return 5;
    case D2: return 4;
    case D3: return 0;
    case D4: return 2;
    case D5: return 14;
    case D6: return 12;
    case D7: return 13;
    case D8: return 15;
    default: return pin;
  }
}

void oledSdaHigh() {
  pinMode(oledSdaPin, INPUT_PULLUP);
}

void oledSdaLow() {
  pinMode(oledSdaPin, OUTPUT);
  digitalWrite(oledSdaPin, LOW);
}

void oledSclHigh() {
  pinMode(oledSclPin, INPUT_PULLUP);
}

void oledSclLow() {
  pinMode(oledSclPin, OUTPUT);
  digitalWrite(oledSclPin, LOW);
}

void oledDelay() {
  delayMicroseconds(4);
}

void oledStart() {
  oledSdaHigh();
  oledSclHigh();
  oledDelay();
  oledSdaLow();
  oledDelay();
  oledSclLow();
}

void oledStop() {
  oledSdaLow();
  oledDelay();
  oledSclHigh();
  oledDelay();
  oledSdaHigh();
  oledDelay();
}

bool oledWriteByte(uint8_t value) {
  for (uint8_t i = 0; i < 8; i++) {
    if (value & 0x80) oledSdaHigh();
    else oledSdaLow();
    oledDelay();
    oledSclHigh();
    oledDelay();
    oledSclLow();
    value <<= 1;
  }

  oledSdaHigh();
  oledDelay();
  oledSclHigh();
  oledDelay();
  bool ack = digitalRead(oledSdaPin) == LOW;
  oledSclLow();
  return ack;
}

void oledCommand(uint8_t command) {
  oledStart();
  oledWriteByte(oledAddress << 1);
  oledWriteByte(0x00);
  oledWriteByte(command);
  oledStop();
}

void oledData(uint8_t data) {
  oledStart();
  oledWriteByte(oledAddress << 1);
  oledWriteByte(0x40);
  oledWriteByte(data);
  oledStop();
}

void oledSetPageColumn(uint8_t page, uint8_t column) {
  oledCommand(0xB0 + page);
  oledCommand(0x00 + (column & 0x0F));
  oledCommand(0x10 + (column >> 4));
}

void oledClear() {
  for (uint8_t page = 0; page < 8; page++) {
    oledSetPageColumn(page, 0);
    for (uint8_t col = 0; col < 128; col++) {
      oledData(0x00);
    }
  }
}

void oledClearPage(uint8_t page) {
  oledSetPageColumn(page, 0);
  for (uint8_t col = 0; col < 128; col++) {
    oledData(0x00);
  }
}

char oledPrintable(char c) {
  if (c >= 'a' && c <= 'z') return c - 32;
  if (c < 32 || c > 95) return ' ';
  return c;
}

void oledDrawChar(char c) {
  c = oledPrintable(c);
  const uint8_t* glyph = FONT_5X7[c - 32];
  for (uint8_t i = 0; i < 5; i++) {
    oledData(pgm_read_byte(&glyph[i]));
  }
  oledData(0x00);
}

void oledDrawText(uint8_t page, uint8_t column, const String& text) {
  oledSetPageColumn(page, column);
  uint8_t maxChars = (128 - column) / 6;
  for (uint8_t i = 0; i < text.length() && i < maxChars; i++) {
    oledDrawChar(text[i]);
  }
}

void oledDrawTextLine(uint8_t page, const String& text) {
  oledClearPage(page);
  oledDrawText(page, 0, text);
}

String formatKb(uint32_t bytes) {
  return String((bytes + 512) / 1024) + "K";
}

String uptimeText() {
  unsigned long seconds = millis() / 1000UL;
  unsigned int hours = seconds / 3600UL;
  unsigned int minutes = (seconds % 3600UL) / 60UL;
  unsigned int secs = seconds % 60UL;

  char buffer[12];
  snprintf(buffer, sizeof(buffer), "%02u:%02u:%02u", hours % 100, minutes, secs);
  return String(buffer);
}

bool extractJsonLong(const String& payload, const char* key, long& value) {
  String marker = String("\"") + key + "\":";
  int pos = payload.indexOf(marker);
  if (pos < 0) return false;
  pos += marker.length();
  while (pos < static_cast<int>(payload.length()) && payload[pos] == ' ') pos++;

  int end = pos;
  if (end < static_cast<int>(payload.length()) && payload[end] == '-') end++;
  while (end < static_cast<int>(payload.length()) && isDigit(payload[end])) end++;
  if (end <= pos) return false;

  value = payload.substring(pos, end).toInt();
  return true;
}

String formatLocalTime(uint32_t epochSeconds) {
  const char* weekdays[] = {"THU", "FRI", "SAT", "SUN", "MON", "TUE", "WED"};
  const uint16_t daysBeforeMonth[] = {
    0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
  };
  uint32_t days = epochSeconds / 86400UL;
  uint8_t weekday = days % 7;
  uint32_t secondsOfDay = epochSeconds % 86400UL;

  uint16_t year = 1970;
  while (true) {
    bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
    uint16_t yearDays = leap ? 366 : 365;
    if (days < yearDays) break;
    days -= yearDays;
    year++;
  }

  bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
  uint8_t month = 12;
  for (uint8_t m = 1; m <= 12; m++) {
    uint16_t start = daysBeforeMonth[m - 1];
    if (leap && m > 2) start++;
    uint16_t next = (m == 12) ? (leap ? 366 : 365) : daysBeforeMonth[m];
    if (leap && m + 1 > 2) next++;
    if (days >= start && days < next) {
      month = m;
      days -= start;
      break;
    }
  }

  uint8_t day = days + 1;
  uint8_t hour = secondsOfDay / 3600UL;
  uint8_t minute = (secondsOfDay % 3600UL) / 60UL;

  char buffer[22];
  snprintf(buffer, sizeof(buffer), "%04u-%02u-%02u %s %02u:%02u",
           year, month, day, weekdays[weekday], hour, minute);
  return String(buffer);
}

String localTimeTitle() {
  if (!timeSynced) {
    return "UP " + uptimeText();
  }
  uint32_t elapsed = (millis() - lastTimeSyncAt) / 1000UL;
  return formatLocalTime(syncedLocalEpoch + elapsed);
}

bool setSyncedUtcTime(uint32_t utcEpoch, const char* source) {
  syncedLocalEpoch = utcEpoch + 8UL * 3600UL;
  lastTimeSyncAt = millis();
  timeSynced = true;
  Serial.printf("Time synced by %s: utc=%lu local=%lu title=%s\n",
                source,
                static_cast<unsigned long>(utcEpoch),
                static_cast<unsigned long>(syncedLocalEpoch),
                localTimeTitle().c_str());
  return true;
}

bool syncNtpTime() {
  if (!wifiConnected()) return false;

  Serial.printf("NTP sync start: %s, %s, %s\n", NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3);
  configTime(0, 0, NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3);

  for (uint8_t i = 0; i < 20; i++) {
    time_t now = time(nullptr);
    if (now > 1600000000) {
      return setSyncedUtcTime(static_cast<uint32_t>(now), "NTP");
    }
    delay(150);
    yield();
  }

  Serial.println("NTP sync failed: timeout");
  return false;
}

bool syncWorldTimeApi() {
  if (!wifiConnected()) {
    Serial.println("Time sync skipped: WiFi not connected");
    return false;
  }

  WiFiClient client;
  HTTPClient http;
  http.setTimeout(3000);
  if (!http.begin(client, TIME_SYNC_URL)) {
    Serial.println("Time sync failed: http.begin");
    return false;
  }

  Serial.printf("Time sync GET %s\n", TIME_SYNC_URL);
  int code = http.GET();
  Serial.printf("Time sync HTTP code: %d %s\n", code, http.errorToString(code).c_str());
  if (code != HTTP_CODE_OK) {
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();
  Serial.printf("Time sync payload length: %u\n", payload.length());

  long unixTime = 0;
  long rawOffset = 0;
  long dstOffset = 0;
  if (!extractJsonLong(payload, "unixtime", unixTime)) {
    Serial.println("Time sync failed: missing unixtime");
    return false;
  }
  extractJsonLong(payload, "raw_offset", rawOffset);
  extractJsonLong(payload, "dst_offset", dstOffset);

  Serial.printf("Time sync parsed: unix=%ld raw=%ld dst=%ld local=%lu\n",
                unixTime, rawOffset, dstOffset, static_cast<unsigned long>(unixTime + rawOffset + dstOffset));
  syncedLocalEpoch = static_cast<uint32_t>(unixTime + rawOffset + dstOffset);
  lastTimeSyncAt = millis();
  timeSynced = true;
  Serial.printf("Time synced by worldtimeapi: %s\n", localTimeTitle().c_str());
  return true;
}

bool syncTime() {
  if (syncWorldTimeApi()) {
    return true;
  }

  Serial.println("Worldtimeapi failed, trying NTP fallback");
  return syncNtpTime();
}

void updateWorldTimeIfNeeded() {
  if (!wifiConnected()) return;

  unsigned long now = millis();
  unsigned long interval = timeSynced ? TIME_SYNC_INTERVAL_MS : TIME_SYNC_RETRY_MS;
  if (lastTimeSyncAttempt != 0 && now - lastTimeSyncAttempt < interval) {
    return;
  }

  lastTimeSyncAttempt = now;
  if (syncTime()) {
    drawStatus();
  }
}

String heapStatusLine1() {
  String line = "HEAP ";
  line += formatKb(ESP.getFreeHeap());
  line += " FRAG ";
  line += String(ESP.getHeapFragmentation());
  line += "%";
  return line;
}

String cpuWifiStatusLine1() {
  String line = "CPU ";
  line += String(ESP.getCpuFreqMHz());
  line += "M RSSI ";
  if (wifiConnected()) {
    line += String(WiFi.RSSI());
    line += " CH ";
    line += String(WiFi.channel());
  } else {
    line += "--";
  }
  return line;
}

String flashStatusLine1() {
  String line = "FLASH ";
  line += formatKb(ESP.getFlashChipRealSize());
  line += " FW ";
  line += formatKb(ESP.getSketchSize());
  return line;
}

String flashStatusLine2() {
  String line = "FREE ";
  line += formatKb(ESP.getFreeSketchSpace());
  line += " ID ";
  line += String(ESP.getChipId(), HEX);
  return line;
}

void drawSystemStatus() {
  uint8_t page = (millis() / OLED_STATUS_REFRESH_MS) % 2;
  if (page == 0) {
    oledDrawTextLine(6, heapStatusLine1());
    oledDrawTextLine(7, cpuWifiStatusLine1());
  } else {
    oledDrawTextLine(6, flashStatusLine1());
    oledDrawTextLine(7, flashStatusLine2());
  }
  lastOledSystemRefresh = millis();
}

void drawTitle() {
  if (!oledReady) return;

  String title = localTimeTitle();
  oledDrawTextLine(0, title);
  lastOledTitleRefresh = millis();
}

void oledBegin() {
  oledSdaHigh();
  oledSclHigh();
  delay(50);
  const uint8_t initCommands[] = {
    0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40,
    0x8D, 0x14, 0x20, 0x02, 0xA1, 0xC8, 0xDA, 0x12,
    0x81, 0xCF, 0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6,
    0x2E, 0xAF
  };
  for (uint8_t i = 0; i < sizeof(initCommands); i++) {
    oledCommand(initCommands[i]);
  }
  oledClear();
}

struct OledBusCandidate {
  uint8_t sda;
  uint8_t scl;
  const char* label;
};

const OledBusCandidate OLED_BUS_CANDIDATES[] = {
  {OLED_DATA_PIN, OLED_CLOCK_PIN, "HW-364A D5 SDA / D6 SCL"},
  {OLED_CLOCK_PIN, OLED_DATA_PIN, "HW-364A reversed D6 SDA / D5 SCL"},
  {D2, D1, "NodeMCU D2 SDA / D1 SCL"},
  {D1, D2, "NodeMCU reversed D1 SDA / D2 SCL"},
};

bool oledProbeAddress(uint8_t address) {
  oledStart();
  bool ack = oledWriteByte(address << 1);
  oledStop();
  return ack;
}

bool detectOledBus() {
  const uint8_t addresses[] = {0x3C, 0x3D};
  Serial.println("OLED probe start");
  for (uint8_t i = 0; i < sizeof(OLED_BUS_CANDIDATES) / sizeof(OLED_BUS_CANDIDATES[0]); i++) {
    oledSdaPin = OLED_BUS_CANDIDATES[i].sda;
    oledSclPin = OLED_BUS_CANDIDATES[i].scl;
    oledSdaHigh();
    oledSclHigh();
    delay(2);

    bool idleSdaHigh = digitalRead(oledSdaPin) == HIGH;
    bool idleSclHigh = digitalRead(oledSclPin) == HIGH;
    Serial.printf("OLED probe bus idle: %-32s SDA GPIO%u=%s SCL GPIO%u=%s\n",
                  OLED_BUS_CANDIDATES[i].label,
                  gpioNumber(oledSdaPin),
                  idleSdaHigh ? "HIGH" : "LOW",
                  gpioNumber(oledSclPin),
                  idleSclHigh ? "HIGH" : "LOW");
    if (!idleSdaHigh || !idleSclHigh) {
      Serial.println("OLED probe bus skipped: line held low");
      continue;
    }

    for (uint8_t j = 0; j < sizeof(addresses); j++) {
      oledAddress = addresses[j];
      bool ack = oledProbeAddress(oledAddress);
      Serial.printf("OLED probe: %-32s addr 0x%02X SDA GPIO%u SCL GPIO%u -> %s\n",
                    OLED_BUS_CANDIDATES[i].label,
                    oledAddress,
                    gpioNumber(oledSdaPin),
                    gpioNumber(oledSclPin),
                    ack ? "ACK" : "NOACK");
      if (ack) {
        Serial.printf("OLED detected: %s, address 0x%02X (SDA GPIO%u, SCL GPIO%u)\n",
                      OLED_BUS_CANDIDATES[i].label,
                      oledAddress,
                      gpioNumber(oledSdaPin),
                      gpioNumber(oledSclPin));
        return true;
      }
    }
  }

  oledSdaPin = OLED_DATA_PIN;
  oledSclPin = OLED_CLOCK_PIN;
  oledAddress = 0x3C;
  Serial.printf("OLED probe found no ACK; fallback to SDA GPIO%u, SCL GPIO%u, address 0x%02X\n",
                gpioNumber(oledSdaPin),
                gpioNumber(oledSclPin),
                oledAddress);
  return false;
}

void selectRfRxPin() {
  if (rfRxPin == oledSdaPin || rfRxPin == oledSclPin) {
    Serial.printf("RF RX GPIO%u conflicts with OLED, moving RF RX to GPIO%u\n",
                  gpioNumber(rfRxPin),
                  gpioNumber(RF_RX_ALT_PIN));
    rfRxPin = RF_RX_ALT_PIN;
  }
}

String htmlEscape(const String& text) {
  String escaped;
  escaped.reserve(text.length());
  for (uint16_t i = 0; i < text.length(); i++) {
    char c = text[i];
    if (c == '&') escaped += F("&amp;");
    else if (c == '<') escaped += F("&lt;");
    else if (c == '>') escaped += F("&gt;");
    else if (c == '"') escaped += F("&quot;");
    else escaped += c;
  }
  return escaped;
}

void drawStatus() {
  if (!oledReady) return;

  lastOledWifiStatus = WiFi.status();
  lastOledIp = deviceIP();

  drawTitle();

  for (uint8_t clearPage = 1; clearPage <= 5; clearPage++) {
    oledClearPage(clearPage);
  }

  uint8_t page = 1;

  if (wifiConnected()) {
    String ssidLine = "SSID: " + WiFi.SSID();
    String ipLine = "IP: " + WiFi.localIP().toString();
    oledDrawText(page++, 0, ssidLine);
    oledDrawText(page++, 0, ipLine);
  } else {
    oledDrawText(page++, 0, "SETUP AP");
    String ssidLine = String("SSID: ") + AP_SSID;
    String passLine = String("PASS: ") + AP_PASSWORD;
    String ipLine = "IP: " + WiFi.softAPIP().toString();
    oledDrawText(page++, 0, ssidLine);
    oledDrawText(page++, 0, passLine);
    oledDrawText(page++, 0, ipLine);
  }

  String lastLine = "Last: " + lastMessage;
  oledDrawText(page++, 0, lastLine);
  if (lastReceived.value != 0) {
    String rxLine = "RX: " + String(lastReceived.value);
    oledDrawText(page++, 0, rxLine);
  } else if (voiceStarted) {
    oledDrawText(page++, 0, "VOICE: TMALL OK");
  }

  if (wifiConnected()) {
    drawSystemStatus();
  } else {
    oledClearPage(7);
    lastOledSystemRefresh = millis();
  }
}

void showOledEventLine(uint8_t page, const String& message) {
  lastMessage = message;
  if (!oledReady) return;
  oledDrawTextLine(page, message);
}

void setupOled() {
  bool detected = detectOledBus();
  if (!detected) {
    Serial.println("OLED probe retry after power settle");
    delay(350);
    detected = detectOledBus();
  }
  Serial.printf("Starting OLED via SSD1306 SW I2C: SDA GPIO%u, SCL GPIO%u, address 0x%02X, detected=%u\n",
                gpioNumber(oledSdaPin),
                gpioNumber(oledSclPin),
                oledAddress,
                detected ? 1 : 0);
  oledBegin();
  oledReady = true;
  rfRxPin = RF_RX_BUILTIN_OLED_PIN;
  selectRfRxPin();
  oledDrawTextLine(0, "STARTING...");
}

void saveSettings() {
  EEPROM.put(0, settings);
  EEPROM.commit();
  saveSettingsBackup();
}

void initDefaultSettings() {
  memset(&settings, 0, sizeof(settings));
  settings.magic = SETTINGS_MAGIC;
  settings.voiceRfRepeat = DEFAULT_VOICE_RF_REPEAT_TRANSMIT;
  for (uint8_t i = 0; i < 3; i++) {
    settings.codes[i].bits = 24;
    settings.codes[i].protocol = 1;
    settings.codes[i].pulseLength = 350;
  }
}

void setDefaultRfSlot(uint8_t index) {
  if (index >= RF_SLOT_COUNT) return;

  memset(&rfSlots[index], 0, sizeof(RfSlot));
  snprintf(rfSlots[index].name, sizeof(rfSlots[index].name), "Slot %u", index + 1);
  rfSlots[index].code.bits = 24;
  rfSlots[index].code.protocol = 1;
  rfSlots[index].code.pulseLength = 350;
}

void initDefaultRfSlots() {
  for (uint8_t i = 0; i < RF_SLOT_COUNT; i++) {
    setDefaultRfSlot(i);
  }
}

void releaseIrSlotRaw(IrSlot& irSlot) {
  delete[] irSlot.raw;
  irSlot.raw = nullptr;
  irSlot.length = 0;
}

bool setIrSlotRaw(IrSlot& irSlot, const uint16_t* raw, uint16_t length) {
  releaseIrSlotRaw(irSlot);
  if (raw == nullptr || length == 0) return true;

  irSlot.raw = new (std::nothrow) uint16_t[length];
  if (irSlot.raw == nullptr) {
    lastMessage = "IR save no memory";
    drawStatus();
    return false;
  }
  memcpy(irSlot.raw, raw, length * sizeof(uint16_t));
  irSlot.length = length;
  return true;
}

bool irPresetEnabled(const IrSlot& irSlot) {
#if ENABLE_IR_AC_PRESETS
  return irSlot.preset;
#else
  (void)irSlot;
  return false;
#endif
}

void setDefaultIrSlot(uint8_t index) {
  if (index >= IR_SLOT_COUNT) return;

  releaseIrSlotRaw(irSlots[index]);
  memset(&irSlots[index], 0, sizeof(IrSlot));
  snprintf(irSlots[index].name, sizeof(irSlots[index].name), "IR %u", index + 1);
  irSlots[index].frequency = IR_DEFAULT_FREQUENCY_KHZ;
  irSlots[index].protocol = decode_type_t::UNKNOWN;
  irSlots[index].model = -1;
  irSlots[index].power = true;
  irSlots[index].mode = 1;
  irSlots[index].temp = 26;
  irSlots[index].fan = 0;
}

void initDefaultIrSlots() {
  for (uint8_t i = 0; i < IR_SLOT_COUNT; i++) {
    setDefaultIrSlot(i);
  }
}

String fieldAt(const String& line, uint8_t wanted) {
  uint8_t field = 0;
  int start = 0;
  while (start <= static_cast<int>(line.length())) {
    int end = line.indexOf('|', start);
    if (end < 0) end = line.length();
    if (field == wanted) return line.substring(start, end);
    field++;
    start = end + 1;
  }
  return "";
}

void saveSettingsBackup() {
  if (!fsReady) return;

  File file = LittleFS.open(SETTINGS_BACKUP_PATH, "w");
  if (!file) {
    Serial.println("LittleFS save failed: settings backup file open");
    return;
  }

  file.print(F("wifiSsid|"));
  file.println(settings.wifiSsid);
  file.print(F("wifiPassword|"));
  file.println(settings.wifiPassword);
  file.print(F("blinkerAuth|"));
  file.println(settings.blinkerAuth);
  file.print(F("voiceRfRepeat|"));
  file.println(settings.voiceRfRepeat);
  file.close();
  Serial.println("LittleFS saved settings backup");
}

bool restoreSettingsBackup() {
  if (!fsReady || !LittleFS.exists(SETTINGS_BACKUP_PATH)) return false;

  File file = LittleFS.open(SETTINGS_BACKUP_PATH, "r");
  if (!file) {
    Serial.println("LittleFS load failed: settings backup file open");
    return false;
  }

  char backupSsid[sizeof(settings.wifiSsid)] = {0};
  char backupPassword[sizeof(settings.wifiPassword)] = {0};
  char backupAuth[sizeof(settings.blinkerAuth)] = {0};
  uint8_t backupVoiceRfRepeat = 0;
  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    String key = fieldAt(line, 0);
    String value = fieldAt(line, 1);
    if (key == F("wifiSsid")) {
      value.toCharArray(backupSsid, sizeof(backupSsid));
    } else if (key == F("wifiPassword")) {
      value.toCharArray(backupPassword, sizeof(backupPassword));
    } else if (key == F("blinkerAuth")) {
      value.toCharArray(backupAuth, sizeof(backupAuth));
    } else if (key == F("voiceRfRepeat")) {
      backupVoiceRfRepeat = normalizedVoiceRfRepeat(value.toInt());
    }
  }
  file.close();

  bool changed = false;
  if (settings.wifiSsid[0] == '\0' && backupSsid[0] != '\0') {
    strncpy(settings.wifiSsid, backupSsid, sizeof(settings.wifiSsid) - 1);
    strncpy(settings.wifiPassword, backupPassword, sizeof(settings.wifiPassword) - 1);
    changed = true;
  }
  if (settings.blinkerAuth[0] == '\0' && backupAuth[0] != '\0') {
    strncpy(settings.blinkerAuth, backupAuth, sizeof(settings.blinkerAuth) - 1);
    changed = true;
  }
  if (backupVoiceRfRepeat != 0 && settings.voiceRfRepeat != backupVoiceRfRepeat) {
    settings.voiceRfRepeat = backupVoiceRfRepeat;
    changed = true;
  }

  if (changed) {
    settings.magic = SETTINGS_MAGIC;
    EEPROM.put(0, settings);
    EEPROM.commit();
    Serial.println("LittleFS restored settings backup to EEPROM");
  } else {
    Serial.println("LittleFS settings backup checked");
  }
  return changed;
}

uint16_t parseIrRawList(const String& text, uint16_t* out, uint16_t maxLen) {
  uint16_t count = 0;
  int start = 0;
  while (start <= static_cast<int>(text.length()) && count < maxLen) {
    int end = text.indexOf(',', start);
    if (end < 0) end = text.length();
    String token = text.substring(start, end);
    token.trim();
    uint16_t value = token.toInt();
    if (value > 0) {
      out[count++] = value;
    }
    start = end + 1;
  }
  return count;
}

#if ENABLE_IR_AC_PRESETS
decode_type_t acProtocolForBrand(const String& brand) {
  if (brand == F("daikin")) return decode_type_t::DAIKIN;
  if (brand == F("gree")) return decode_type_t::GREE;
  if (brand == F("kelon")) return decode_type_t::KELON;
  if (brand == F("hisense")) return decode_type_t::KELON;
  if (brand == F("midea")) return decode_type_t::MIDEA;
  if (brand == F("hualing")) return decode_type_t::MIDEA;
  if (brand == F("xiaomi")) return decode_type_t::MIDEA;
  return decode_type_t::UNKNOWN;
}

String brandForAcProtocol(decode_type_t protocol) {
  switch (protocol) {
    case decode_type_t::DAIKIN: return F("大金");
    case decode_type_t::GREE: return F("格力");
    case decode_type_t::KELON: return F("科龙/海信");
    case decode_type_t::MIDEA: return F("美的/华凌/小米");
    default: return F("未知");
  }
}

stdAc::opmode_t acModeFromValue(uint8_t value) {
  switch (value) {
    case 0: return stdAc::opmode_t::kAuto;
    case 1: return stdAc::opmode_t::kCool;
    case 2: return stdAc::opmode_t::kHeat;
    case 3: return stdAc::opmode_t::kDry;
    case 4: return stdAc::opmode_t::kFan;
    default: return stdAc::opmode_t::kCool;
  }
}

stdAc::fanspeed_t acFanFromValue(uint8_t value) {
  switch (value) {
    case 0: return stdAc::fanspeed_t::kAuto;
    case 1: return stdAc::fanspeed_t::kLow;
    case 2: return stdAc::fanspeed_t::kMedium;
    case 3: return stdAc::fanspeed_t::kHigh;
    case 4: return stdAc::fanspeed_t::kMax;
    default: return stdAc::fanspeed_t::kAuto;
  }
}

String acModeName(uint8_t value) {
  switch (value) {
    case 0: return F("自动");
    case 1: return F("制冷");
    case 2: return F("制热");
    case 3: return F("除湿");
    case 4: return F("送风");
    default: return F("制冷");
  }
}

String acFanName(uint8_t value) {
  switch (value) {
    case 0: return F("自动");
    case 1: return F("低风");
    case 2: return F("中风");
    case 3: return F("高风");
    case 4: return F("最大");
    default: return F("自动");
  }
}

bool sendAcPreset(decode_type_t protocol, int16_t model, bool power, uint8_t mode,
                  uint8_t temp, uint8_t fan, const String& source) {
  if (!IRac::isProtocolSupported(protocol)) {
    lastMessage = source + " AC unsupported";
    Serial.printf("AC preset unsupported: protocol=%d\n", static_cast<int>(protocol));
    drawStatus();
    return false;
  }

  bool ok = irac.sendAc(protocol,
                        model,
                        power,
                        power ? acModeFromValue(mode) : stdAc::opmode_t::kOff,
                        temp,
                        true,
                        acFanFromValue(fan),
                        stdAc::swingv_t::kAuto,
                        stdAc::swingh_t::kOff,
                        false,
                        false,
                        false,
                        false,
                        false,
                        false,
                        true);
  lastMessage = ok ? (source + " AC " + brandForAcProtocol(protocol)) : (source + " AC failed");
  Serial.printf("AC preset TX: protocol=%d brand=%s model=%d power=%u mode=%u temp=%u fan=%u ok=%u\n",
                static_cast<int>(protocol),
                brandForAcProtocol(protocol).c_str(),
                model,
                power ? 1 : 0,
                mode,
                temp,
                fan,
                ok ? 1 : 0);
  drawStatus();
  return ok;
}
#endif

void saveRfSlots() {
  if (!fsReady) return;

  File file = LittleFS.open(RF_CODES_PATH, "w");
  if (!file) {
    Serial.println("LittleFS save failed: rf code file open");
    return;
  }

  for (uint8_t i = 0; i < RF_SLOT_COUNT; i++) {
    file.print(i + 1);
    file.print('|');
    file.print(rfSlots[i].name);
    file.print('|');
    file.print(rfSlots[i].code.value);
    file.print('|');
    file.print(rfSlots[i].code.bits);
    file.print('|');
    file.print(rfSlots[i].code.protocol);
    file.print('|');
    file.println(rfSlots[i].code.pulseLength);
  }
  file.close();
  Serial.println("LittleFS saved rf codes");
}

void saveIrSlots() {
  if (!fsReady) return;

  File file = LittleFS.open(IR_CODES_PATH, "w");
  if (!file) {
    Serial.println("LittleFS save failed: ir code file open");
    return;
  }

  for (uint8_t i = 0; i < IR_SLOT_COUNT; i++) {
    file.print(i + 1);
    file.print('|');
    file.print(irSlots[i].name);
    file.print('|');
    file.print(irSlots[i].frequency);
    file.print('|');
    file.print(irSlots[i].length);
    file.print('|');
    for (uint16_t j = 0; irSlots[i].raw != nullptr && j < irSlots[i].length; j++) {
      if (j != 0) file.print(',');
      file.print(irSlots[i].raw[j]);
    }
    file.print('|');
    file.print(irPresetEnabled(irSlots[i]) ? 1 : 0);
    file.print('|');
    file.print(static_cast<int>(irSlots[i].protocol));
    file.print('|');
    file.print(irSlots[i].model);
    file.print('|');
    file.print(irSlots[i].power ? 1 : 0);
    file.print('|');
    file.print(irSlots[i].mode);
    file.print('|');
    file.print(irSlots[i].temp);
    file.print('|');
    file.print(irSlots[i].fan);
    file.println();
  }
  file.close();
  Serial.println("LittleFS saved ir codes");
}

void importLegacyRfCodes() {
  initDefaultRfSlots();
  for (uint8_t i = 0; i < 3 && i < RF_SLOT_COUNT; i++) {
    rfSlots[i].code = settings.codes[i];
  }
  saveRfSlots();
}

void loadRfSlots() {
  initDefaultRfSlots();
  if (!fsReady) {
    importLegacyRfCodes();
    return;
  }

  if (!LittleFS.exists(RF_CODES_PATH)) {
    Serial.println("LittleFS rf code file missing, importing EEPROM slots");
    importLegacyRfCodes();
    return;
  }

  File file = LittleFS.open(RF_CODES_PATH, "r");
  if (!file) {
    Serial.println("LittleFS load failed: rf code file open");
    importLegacyRfCodes();
    return;
  }

  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    int slot = fieldAt(line, 0).toInt();
    if (slot < 1 || slot > RF_SLOT_COUNT) continue;

    RfSlot& rfSlot = rfSlots[slot - 1];
    String name = fieldAt(line, 1);
    name.trim();
    if (name.length() == 0) name = "Slot " + String(slot);
    memset(rfSlot.name, 0, sizeof(rfSlot.name));
    name.toCharArray(rfSlot.name, sizeof(rfSlot.name));
    rfSlot.code.value = strtoul(fieldAt(line, 2).c_str(), nullptr, 10);
    rfSlot.code.bits = fieldAt(line, 3).toInt();
    rfSlot.code.protocol = fieldAt(line, 4).toInt();
    rfSlot.code.pulseLength = fieldAt(line, 5).toInt();
    if (rfSlot.code.bits == 0) rfSlot.code.bits = 24;
    if (rfSlot.code.protocol == 0) rfSlot.code.protocol = 1;
    if (rfSlot.code.pulseLength == 0) rfSlot.code.pulseLength = 350;
  }
  file.close();
  Serial.println("LittleFS loaded rf codes");
}

void loadIrSlots() {
  initDefaultIrSlots();
  if (!fsReady) return;

  if (!LittleFS.exists(IR_CODES_PATH)) {
    Serial.println("LittleFS ir code file missing");
    saveIrSlots();
    return;
  }

  File file = LittleFS.open(IR_CODES_PATH, "r");
  if (!file) {
    Serial.println("LittleFS load failed: ir code file open");
    return;
  }

  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    int slot = fieldAt(line, 0).toInt();
    if (slot < 1 || slot > IR_SLOT_COUNT) continue;

    IrSlot& irSlot = irSlots[slot - 1];
    String name = fieldAt(line, 1);
    name.trim();
    if (name.length() == 0) name = "IR " + String(slot);
    memset(irSlot.name, 0, sizeof(irSlot.name));
    name.toCharArray(irSlot.name, sizeof(irSlot.name));
    irSlot.frequency = fieldAt(line, 2).toInt();
    if (irSlot.frequency == 0) irSlot.frequency = IR_DEFAULT_FREQUENCY_KHZ;
    uint16_t rawBuffer[IR_RAW_MAX] = {0};
    uint16_t parsedLength = parseIrRawList(fieldAt(line, 4), rawBuffer, IR_RAW_MAX);
    uint16_t storedLength = fieldAt(line, 3).toInt();
    if (storedLength > 0 && storedLength < parsedLength) parsedLength = storedLength;
    if (!setIrSlotRaw(irSlot, rawBuffer, parsedLength)) continue;
    String preset = fieldAt(line, 5);
    if (preset.length() > 0) {
#if ENABLE_IR_AC_PRESETS
      irSlot.preset = preset.toInt() != 0;
      irSlot.protocol = static_cast<decode_type_t>(fieldAt(line, 6).toInt());
      irSlot.model = fieldAt(line, 7).length() > 0 ? fieldAt(line, 7).toInt() : -1;
      irSlot.power = fieldAt(line, 8).length() == 0 || fieldAt(line, 8).toInt() != 0;
      irSlot.mode = fieldAt(line, 9).length() > 0 ? fieldAt(line, 9).toInt() : 1;
      irSlot.temp = fieldAt(line, 10).length() > 0 ? fieldAt(line, 10).toInt() : 26;
      irSlot.fan = fieldAt(line, 11).length() > 0 ? fieldAt(line, 11).toInt() : 0;
#else
      irSlot.preset = false;
#endif
    }
  }
  file.close();
  Serial.println("LittleFS loaded ir codes");
}

void setupLittleFs() {
  fsReady = LittleFS.begin();
  if (!fsReady) {
    Serial.println("LittleFS mount failed, formatting");
    LittleFS.format();
    fsReady = LittleFS.begin();
  }
  Serial.printf("LittleFS: %s\n", fsReady ? "mounted" : "mount failed");
  if (fsReady) {
    restoreSettingsBackup();
    if (!LittleFS.exists(SETTINGS_BACKUP_PATH)) {
      saveSettingsBackup();
    }
  }
  loadRfSlots();
  loadIrSlots();
}

void loadSettings() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(0, settings);
  if (settings.magic == PREVIOUS_SETTINGS_MAGIC) {
    PreviousSettings previousSettings;
    EEPROM.get(0, previousSettings);
    initDefaultSettings();
    strncpy(settings.wifiSsid, previousSettings.wifiSsid, sizeof(settings.wifiSsid) - 1);
    strncpy(settings.wifiPassword, previousSettings.wifiPassword, sizeof(settings.wifiPassword) - 1);
    strncpy(settings.blinkerAuth, previousSettings.blinkerAuth, sizeof(settings.blinkerAuth) - 1);
    memcpy(settings.codes, previousSettings.codes, sizeof(settings.codes));
    saveSettings();
    return;
  }

  if (settings.magic == OLD_SETTINGS_MAGIC) {
    OldSettings oldSettings;
    EEPROM.get(0, oldSettings);
    initDefaultSettings();
    strncpy(settings.wifiSsid, oldSettings.wifiSsid, sizeof(settings.wifiSsid) - 1);
    strncpy(settings.wifiPassword, oldSettings.wifiPassword, sizeof(settings.wifiPassword) - 1);
    memcpy(settings.codes, oldSettings.codes, sizeof(settings.codes));
    saveSettings();
    return;
  }

  if (settings.magic != SETTINGS_MAGIC) {
    initDefaultSettings();
    saveSettings();
    return;
  }

  uint8_t repeat = normalizedVoiceRfRepeat(settings.voiceRfRepeat);
  if (settings.voiceRfRepeat != repeat) {
    settings.voiceRfRepeat = repeat;
    saveSettings();
  }
}

bool hasCode(uint8_t index) {
  return index < RF_SLOT_COUNT && rfSlots[index].code.value != 0 && rfSlots[index].code.bits != 0;
}

bool hasIrCode(uint8_t index) {
  return index < IR_SLOT_COUNT && (irPresetEnabled(irSlots[index]) || (irSlots[index].raw != nullptr && irSlots[index].length > 0));
}

uint32_t sendRawCode(uint32_t value, uint8_t bits, uint8_t protocol, uint16_t pulseLength, uint8_t repeatTransmit = RF_REPEAT_TRANSMIT) {
  uint32_t startedAt = millis();
  rf.setProtocol(protocol);
  rf.setPulseLength(pulseLength);
  rf.setRepeatTransmit(repeatTransmit);
  rf.send(value, bits);
  return millis() - startedAt;
}

uint8_t rfRepeatForSource(const String& source) {
  return (source == F("AliGenie") || source == F("DuerOS")) ? normalizedVoiceRfRepeat(settings.voiceRfRepeat) : RF_REPEAT_TRANSMIT;
}

bool transmitSavedSlot(uint8_t slot, const String& source) {
  if (slot < 1 || slot > RF_SLOT_COUNT || !hasCode(slot - 1)) {
    addLog("RF TX saved source=%s slot=%u empty", source.c_str(), slot);
    showOledEventLine(5, source + " slot " + String(slot) + " empty");
    return false;
  }

  const RfCode& code = rfSlots[slot - 1].code;
  uint8_t repeatTransmit = rfRepeatForSource(source);
  uint32_t elapsed = sendRawCode(code.value, code.bits, code.protocol, code.pulseLength, repeatTransmit);
  addLog("RF TX source=%s slot=%u value=%lu repeat=%u elapsed=%lums",
         source.c_str(),
         slot,
         static_cast<unsigned long>(code.value),
         repeatTransmit,
         static_cast<unsigned long>(elapsed));
  showOledEventLine(5, source + " 433 OK " + String(slot));
  return true;
}

bool transmitSavedIrSlot(uint8_t slot, const String& source) {
  if (slot < 1 || slot > IR_SLOT_COUNT || !hasIrCode(slot - 1)) {
    lastMessage = source + " IR " + String(slot) + " empty";
    drawStatus();
    return false;
  }

  const IrSlot& code = irSlots[slot - 1];
  if (irPresetEnabled(code)) {
#if ENABLE_IR_AC_PRESETS
    return sendAcPreset(code.protocol,
                        code.model,
                        code.power,
                        code.mode,
                        code.temp,
                        code.fan,
                        source + " IR " + String(slot));
#endif
  }

  if (code.raw == nullptr || code.length == 0) {
    lastMessage = source + " IR " + String(slot) + " empty";
    drawStatus();
    return false;
  }

  irsend.sendRaw(code.raw, code.length, code.frequency);
  lastMessage = source + " IR " + String(slot);
  Serial.printf("IR TX slot=%u name=%s len=%u freq=%u GPIO%u\n",
                slot,
                code.name,
                code.length,
                code.frequency,
                gpioNumber(IR_TX_PIN));
  drawStatus();
  return true;
}

void sendCode(uint8_t index) {
  if (!transmitSavedSlot(index + 1, "HTTP")) {
    server.send(400, "text/plain; charset=utf-8", "该灯位还没有学习/录入 433 编码");
    return;
  }
  server.sendHeader("Location", "/#rf", true);
  server.send(302, "text/plain", "");
}

static const char APP_CSS[] PROGMEM = R"rawliteral(
:root{font-family:system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;color-scheme:light;--bg:#f4f6fa;--panel:#fff;--panel2:#f8fafc;--text:#172033;--muted:#667085;--line:#e5e7ef;--primary:#2563eb;--primaryText:#fff;--soft:#eef2ff;--softText:#273a8a;--warn:#fff7ed;--warnText:#9a3412;--shadow:0 1px 2px rgba(16,24,40,.05)}
:root[data-scheme=dark]{color-scheme:dark;--bg:#12151b;--panel:#1b2029;--panel2:#242b36;--text:#edf1f7;--muted:#aab4c2;--line:#303846;--primary:#4f8cff;--primaryText:#fff;--soft:#253553;--softText:#cfe0ff;--warn:#3a291b;--warnText:#ffc08a;--shadow:none}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text)}a{color:inherit}.wrap{max-width:1120px;margin:0 auto;padding:20px}.top{display:flex;justify-content:space-between;gap:16px;align-items:flex-start;margin-bottom:16px}.title h1{font-size:24px;line-height:1.2;margin:0 0 6px}.muted{color:var(--muted);font-size:14px;line-height:1.5}.toolbar{display:flex;gap:8px;align-items:center;flex-wrap:wrap;justify-content:flex-end}.tabs{display:flex;gap:6px;margin:0 0 14px;border-bottom:1px solid var(--line);overflow:auto}.tab{appearance:none;border:0;background:transparent;color:var(--muted);font-weight:700;padding:12px 14px;border-bottom:2px solid transparent;white-space:nowrap;cursor:pointer}.tab.active{color:var(--primary);border-color:var(--primary)}.panel{display:none}.panel.active{display:block}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:12px}.card{background:var(--panel);border:1px solid var(--line);border-radius:8px;padding:16px;box-shadow:var(--shadow);margin-bottom:14px}.card h2{font-size:18px;margin:0 0 12px}.card h3{font-size:15px;margin:0 0 10px}.status{display:grid;grid-template-columns:repeat(auto-fit,minmax(170px,1fr));gap:8px}.kv{background:var(--panel2);border-radius:6px;padding:10px;min-height:58px}.kv b{display:block;font-size:12px;color:var(--muted);margin-bottom:4px}.meta{font-size:13px;color:var(--muted);margin-top:10px;word-break:break-all;line-height:1.5}.actions,.inline-actions{display:flex;gap:8px;flex-wrap:wrap}.btn{display:inline-flex;align-items:center;justify-content:center;min-height:40px;border:0;border-radius:8px;background:var(--primary);color:var(--primaryText);font-weight:700;font-size:14px;text-decoration:none;padding:0 13px;cursor:pointer}.btn.secondary{background:var(--soft);color:var(--softText)}.btn.warn{background:var(--warn);color:var(--warnText);border:1px solid color-mix(in srgb,var(--warnText) 28%,transparent)}form{display:grid;grid-template-columns:repeat(6,1fr);gap:8px;align-items:end}.field{min-width:0}.field label{display:block;font-size:12px;color:var(--muted);margin-bottom:4px}input,select{width:100%;height:38px;border:1px solid var(--line);border-radius:6px;padding:0 9px;font-size:14px;background:var(--panel);color:var(--text)}input[type=file]{padding:7px;height:auto}.span2{grid-column:span 2}.span3{grid-column:span 3}.span6{grid-column:1/-1}.slot-grid{grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:8px}.slot-card{display:flex;flex-direction:column;gap:5px;padding:10px;margin-bottom:8px}.slot-card h3{font-size:14px;margin:0}.slot-card .meta{font-size:12px;line-height:1.35;margin-top:2px}.slot-card .actions{gap:6px}.slot-card .actions .btn{flex:1;min-width:64px;min-height:32px;font-size:13px;padding:0 9px}.section-head{display:flex;justify-content:space-between;gap:12px;align-items:center;margin-bottom:10px}@media(max-width:720px){.wrap{padding:14px}.top{display:block}.toolbar{justify-content:flex-start;margin-top:10px}form{grid-template-columns:1fr 1fr}.span2,.span3,.span6{grid-column:1/-1}.tabs{position:sticky;top:0;background:var(--bg);z-index:2}.btn{width:100%}.actions .btn,.inline-actions .btn{width:auto}.slot-grid{grid-template-columns:repeat(auto-fit,minmax(135px,1fr));gap:8px}}
.voice-flow{display:grid;grid-template-columns:repeat(5,1fr);gap:8px;align-items:stretch}.voice-step{position:relative;background:var(--panel2);border:1px solid var(--line);border-radius:8px;padding:12px;min-height:116px}.voice-step b{display:inline-flex;align-items:center;justify-content:center;width:24px;height:24px;border-radius:50%;background:var(--primary);color:var(--primaryText);font-size:13px;margin-bottom:8px}.voice-step strong{display:block;font-size:14px;margin-bottom:5px}.voice-step small{display:block;color:var(--muted);font-size:12px;line-height:1.45}.voice-step:not(:last-child)::after{content:"";position:absolute;right:-8px;top:50%;width:8px;height:2px;background:var(--line)}.voice-map{display:grid;grid-template-columns:repeat(auto-fit,minmax(160px,1fr));gap:8px}@media(max-width:840px){.voice-flow{grid-template-columns:1fr}.voice-step{min-height:auto}.voice-step:not(:last-child)::after{display:none}}
.toast{position:fixed;left:50%;bottom:18px;z-index:20;max-width:min(92vw,420px);transform:translateX(-50%) translateY(18px);opacity:0;background:var(--text);color:var(--panel);border-radius:8px;padding:10px 14px;font-size:14px;font-weight:700;box-shadow:0 10px 28px rgba(16,24,40,.22);pointer-events:none;transition:opacity .16s ease,transform .16s ease}.toast.show{opacity:1;transform:translateX(-50%) translateY(0)}.btn.busy{opacity:.72;pointer-events:none;position:relative}.btn.busy::after{content:"";width:14px;height:14px;margin-left:8px;border:2px solid currentColor;border-right-color:transparent;border-radius:50%;animation:spin .75s linear infinite}form.is-submitting{opacity:.88}@keyframes spin{to{transform:rotate(360deg)}}
.log-view{margin:0;max-height:520px;overflow:auto;background:var(--panel2);border:1px solid var(--line);border-radius:6px;padding:12px;font:12px/1.55 ui-monospace,SFMono-Regular,Consolas,"Liberation Mono",monospace;white-space:pre-wrap;word-break:break-word}
)rawliteral";

static const char APP_JS[] PROGMEM = R"rawliteral(
(function () {
  window.onerror = function (message, source, line) {
    if (window.console && console.log) console.log("ESP UI error", message, source, line);
  };
  function log() {
    if (window.console && console.log) console.log.apply(console, arguments);
  }
  function all(selector) {
    return Array.prototype.slice.call(document.querySelectorAll(selector));
  }
  function hasClass(element, name) {
    return element && (" " + element.className + " ").indexOf(" " + name + " ") >= 0;
  }
  function addClass(element, name) {
    if (element && !hasClass(element, name)) element.className += (element.className ? " " : "") + name;
  }
  function removeClass(element, name) {
    if (element) element.className = (" " + element.className + " ").replace(" " + name + " ", " ").replace(/^\s+|\s+$/g, "");
  }
  function toggleClass(element, name, on) {
    if (on) addClass(element, name);
    else removeClass(element, name);
  }
  function closest(element, selector) {
    while (element && element !== document) {
      var matcher = element.matches || element.msMatchesSelector || element.webkitMatchesSelector;
      if (matcher && matcher.call(element, selector)) return element;
      element = element.parentNode;
    }
    return null;
  }
  function readStorage(name, fallback) {
    try {
      return localStorage.getItem(name) || fallback;
    } catch (error) {
      return fallback;
    }
  }
  function writeStorage(name, value) {
    try {
      localStorage.setItem(name, value);
    } catch (error) {
    }
  }

  var root = document.documentElement;
  var media = window.matchMedia ? window.matchMedia("(prefers-color-scheme: dark)") : null;
  var scheme = readStorage("scheme", "system");
  function applyScheme(value) {
    var dark = value === "dark" || (value === "system" && media && media.matches);
    root.setAttribute("data-scheme", dark ? "dark" : "light");
  }
  var schemeSelect = document.querySelector("[data-scheme]");
  if (schemeSelect) {
    schemeSelect.value = scheme;
    schemeSelect.onchange = function () {
      writeStorage("scheme", schemeSelect.value);
      applyScheme(schemeSelect.value);
    };
  }
  applyScheme(scheme);
  if (media) {
    var onSchemeChange = function () {
      applyScheme(readStorage("scheme", "system"));
    };
    if (media.addEventListener) media.addEventListener("change", onSchemeChange);
    else if (media.addListener) media.addListener(onSchemeChange);
  }

  var tabs = all("[data-tab]");
  var panels = all("[data-panel]");
  log("ESP UI boot", "tabs", tabs.length, "panels", panels.length, "hash", location.hash);
  function hasPanel(name) {
    for (var i = 0; i < panels.length; i++) {
      if (panels[i].getAttribute("data-panel") === name) return true;
    }
    return false;
  }
  function activate(name) {
    if (!name || !hasPanel(name)) name = "system";
    log("tab activate", name);
    for (var i = 0; i < tabs.length; i++) {
      toggleClass(tabs[i], "active", tabs[i].getAttribute("data-tab") === name);
    }
    for (var j = 0; j < panels.length; j++) {
      toggleClass(panels[j], "active", panels[j].getAttribute("data-panel") === name);
    }
    if (location.hash.slice(1) !== name && history.replaceState) history.replaceState(null, "", "#" + name);
  }
  for (var t = 0; t < tabs.length; t++) {
    tabs[t].onclick = function () {
      activate(this.getAttribute("data-tab"));
    };
  }
  activate(location.hash.slice(1) || "system");
  window.onhashchange = function () {
    activate(location.hash.slice(1) || "system");
  };

  var toggles = all("[data-toggle-password]");
  for (var p = 0; p < toggles.length; p++) {
    toggles[p].onchange = function () {
      var form = closest(this, "form") || document;
      var input = form.querySelector("input[data-password]");
      if (input) input.type = this.checked ? "text" : "password";
    };
  }

  var logView = document.querySelector("[data-log-view]");
  var logStatus = document.querySelector("[data-log-status]");
  var logButton = document.querySelector("[data-refresh-logs]");
  var clearLogButton = document.querySelector("[data-clear-logs]");
  function refreshLogs() {
    if (!logView || !window.fetch) {
      if (location.hash.slice(1) === "logs") location.href = "/logs";
      return;
    }
    if (logStatus) logStatus.textContent = "\u6b63\u5728\u5237\u65b0...";
    fetch("/logs?ts=" + Date.now(), { cache: "no-store" })
      .then(function (response) { return response.text(); })
      .then(function (text) {
        logView.textContent = text || "\u6682\u65e0\u65e5\u5fd7";
        logView.scrollTop = logView.scrollHeight;
        if (logStatus) {
          var lines = text ? text.replace(/\s+$/g, "").split("\n").filter(function (line) { return line.length > 0; }).length : 0;
          logStatus.textContent = "\u5df2\u5237\u65b0\uff0c\u5171 " + lines + " \u6761";
        }
      })
      .catch(function () {
        if (logStatus) logStatus.textContent = "\u5237\u65b0\u5931\u8d25";
      });
  }
  if (logButton) logButton.onclick = refreshLogs;
  if (clearLogButton) clearLogButton.onclick = function () {
    if (!window.confirm("\u786e\u5b9a\u6e05\u9664\u5168\u90e8\u65e5\u5fd7\uff1f")) return;
    if (logStatus) logStatus.textContent = "\u6b63\u5728\u6e05\u9664...";
    fetch("/logs/clear", { method: "POST", cache: "no-store" })
      .then(function (response) {
        if (!response.ok) throw new Error("clear failed");
        logView.textContent = "\u6682\u65e0\u65e5\u5fd7";
        if (logStatus) logStatus.textContent = "\u5df2\u6e05\u9664\uff0c\u5171 0 \u6761";
      })
      .catch(function () {
        if (logStatus) logStatus.textContent = "\u6e05\u9664\u5931\u8d25";
      });
  };
  if (logView && location.hash.slice(1) === "logs") refreshLogs();

  var toast = document.createElement("div");
  toast.className = "toast";
  document.body.appendChild(toast);
  var toastTimer = 0;
  function showBusy(text) {
    toast.textContent = text;
    addClass(toast, "show");
    clearTimeout(toastTimer);
    toastTimer = setTimeout(function () {
      removeClass(toast, "show");
    }, 4500);
  }
  function markBusy(element, text) {
    if (!element || hasClass(element, "busy")) return;
    element.setAttribute("data-original-text", (element.textContent || "").replace(/^\s+|\s+$/g, ""));
    addClass(element, "busy");
    element.setAttribute("aria-busy", "true");
    element.textContent = text;
  }
  document.onclick = function (event) {
    event = event || window.event;
    var link = closest(event.target || event.srcElement, "a.btn");
    if (!link) return;
    var href = link.getAttribute("href") || "";
    if (!href || href.charAt(0) === "#" || href.indexOf("javascript:") === 0 || link.target) return;
    if ((link.getAttribute("onclick") || "").indexOf("confirm") >= 0) return;
    markBusy(link, "\u5904\u7406\u4e2d...");
    showBusy("\u5df2\u70b9\u51fb\uff0c\u6b63\u5728\u6267\u884c...");
  };
  document.onsubmit = function (event) {
    event = event || window.event;
    var form = event.target || event.srcElement;
    if (!form || hasClass(form, "is-submitting")) return;
    addClass(form, "is-submitting");
    var button = form.querySelector("button[type=submit],input[type=submit],.btn");
    markBusy(button, "\u63d0\u4ea4\u4e2d...");
    var isUpload = ((form.enctype || "").indexOf("multipart/form-data") >= 0);
    showBusy(isUpload ? "\u6b63\u5728\u4e0a\u4f20\uff0c\u8bf7\u4e0d\u8981\u65ad\u7535..." : "\u6b63\u5728\u63d0\u4ea4...");
  };

  function getJson(url, done) {
    if (!window.XMLHttpRequest) return;
    var xhr = new XMLHttpRequest();
    xhr.open("GET", url, true);
    xhr.onreadystatechange = function () {
      if (xhr.readyState !== 4 || xhr.status !== 200) return;
      try {
        done(JSON.parse(xhr.responseText));
      } catch (error) {
      }
    };
    xhr.send(null);
  }
  var learnState = document.querySelector("[data-rf-learn-state]");
  var countdown = document.querySelector("[data-rf-countdown]");
  var pending = document.querySelector("[data-rf-pending]");
  function updateRfStatus() {
    if (!learnState || !countdown || !pending) return;
    getJson("/rf-status?ts=" + Date.now(), function (data) {
      learnState.textContent = data.learning ? "\u7b49\u5f85\u63a5\u6536" : (data.ready ? "\u5df2\u63a5\u6536\uff0c\u5f85\u4fdd\u5b58" : "\u7a7a\u95f2");
      countdown.textContent = data.remaining || 0;
      pending.textContent = data.ready ? (data.value + " / " + data.bits + " bits") : "\u6682\u65e0";
    });
  }
  if (learnState) {
    updateRfStatus();
    setInterval(updateRfStatus, 1000);
  }

  var irLearnState = document.querySelector("[data-ir-learn-state]");
  var irCountdown = document.querySelector("[data-ir-countdown]");
  var irPending = document.querySelector("[data-ir-pending]");
  function updateIrStatus() {
    if (!irLearnState || !irCountdown || !irPending) return;
    getJson("/ir-status?ts=" + Date.now(), function (data) {
      irLearnState.textContent = data.learning ? "\u7b49\u5f85\u63a5\u6536" : (data.ready ? "\u5df2\u63a5\u6536\uff0c\u5f85\u4fdd\u5b58" : "\u7a7a\u95f2");
      irCountdown.textContent = data.remaining || 0;
      irPending.textContent = data.ready ? (data.length + " pulses / " + data.frequency + " kHz") : "\u6682\u65e0";
    });
  }
  if (irLearnState) {
    updateIrStatus();
    setInterval(updateIrStatus, 1000);
  }
})();
)rawliteral";

void appendPageStart(String& page, const String& title) {
  page += F("<!doctype html><html lang='zh-CN'><head><meta charset='utf-8'>");
  page += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
  page += F("<title>");
  page += title;
  page += F("</title><link rel='stylesheet' href='/app.css'></head><body><main class='wrap'>");
}

void appendPageEnd(String& page) {
  page += F("</main><script src='/app.js'></script></body></html>");
}

void appendWifiSsidField(String& page, const char* spanClass) {
  page += F("<div class='field ");
  page += spanClass;
  page += F("'><label>选择 Wi-Fi</label>");
  bool scan = server.hasArg("scan") && server.arg("scan") == F("1");
  if (!scan) {
    page += F("<input name='ssid' maxlength='32' value='");
    page += htmlEscape(wifiConnected() ? WiFi.SSID() : String(settings.wifiSsid));
    page += F("' placeholder='输入 Wi-Fi 名称'>");
    page += F("<div class='meta'><a href='/?tab=system&scan=1#system'>扫描 Wi-Fi</a></div></div>");
    return;
  }

  Serial.printf("WiFi scan requested heap=%u frag=%u\n", ESP.getFreeHeap(), ESP.getHeapFragmentation());
  int networkCount = WiFi.scanNetworks();
  page += F("<select name='ssid'>");
  if (networkCount <= 0) {
    page += F("<option value=''>未扫描到 Wi-Fi，请刷新重试</option>");
  } else {
    for (int i = 0; i < networkCount; i++) {
      String ssid = WiFi.SSID(i);
      page += F("<option value='");
      page += htmlEscape(ssid);
      page += F("'");
      if (ssid == String(settings.wifiSsid)) page += F(" selected");
      page += F(">");
      page += htmlEscape(ssid);
      page += F(" (");
      page += String(WiFi.RSSI(i));
      page += F(" dBm");
      if (WiFi.encryptionType(i) == ENC_TYPE_NONE) page += F(", open");
      page += F(")</option>");
    }
  }
  page += F("</select><div class='meta'><a href='/?tab=system#system'>改为手动输入</a></div></div>");
}

void appendWifiForm(String& page) {
  page += F("<section class='card' style='margin-bottom:14px'><h2>网络设置</h2>");
  page += F("<form method='post' action='/wifi'>");
  appendWifiSsidField(page, "span2");
  page += F("<div class='field span2'><label>密码</label><input name='password' type='password' maxlength='64' placeholder='留空表示无密码' data-password><label class='muted' style='display:flex;gap:6px;align-items:center;margin-top:8px'><input type='checkbox' data-toggle-password style='width:auto;height:auto'>显示密码</label></div>");
  page += F("<button class='btn' type='submit'>连接</button>");
  page += F("</form><p class='muted'>提交后模块会保存配置并尝试加入所选 Wi-Fi；连接成功后 OLED 会显示 SSID 和本机 IP。</p></section>");
}

void appendVoiceForm(String& page) {
  page += F("<section class='card' style='margin-bottom:14px'><h2>天猫精灵</h2>");
  page += F("<form method='post' action='/voice'>");
  page += F("<div class='field span2'><label>Blinker 设备密钥</label><input name='auth' maxlength='64' value='");
  page += htmlEscape(String(settings.blinkerAuth));
  page += F("' placeholder='在 Blinker App 创建 WiFi 设备后获得'></div>");
  page += F("<button class='btn' type='submit'>保存</button>");
  page += F("</form><p class='muted'>保存密钥并确保设备已连入 Wi-Fi 后，重启设备或重新上电。天猫精灵通过 Blinker 绑定后，插孔 1/2/3 会分别发射网页中保存的 1/2/3 码位。</p></section>");
}

String selectedTab() {
  String tab = server.hasArg("tab") ? server.arg("tab") : "system";
  tab.trim();
  if (tab == F("system") || tab == F("voice") || tab == F("baidu") || tab == F("rf") || tab == F("ir") || tab == F("logs")) {
    return tab;
  }
  return F("system");
}

bool isSelectedTab(const char* name) {
  return selectedTab() == String(name);
}

void appendTabLink(String& page, const char* tab, const char* label) {
  page += F("<a class='tab");
  if (isSelectedTab(tab)) page += F(" active");
  page += F("' href='/?tab=");
  page += tab;
  page += F("#");
  page += tab;
  page += F("' data-tab='");
  page += tab;
  page += F("'>");
  page += label;
  page += F("</a>");
}

void appendTabShell(String& page) {
  page += F("<div class='top'><div class='title'><h1>ESP SmartMate</h1><div class='muted'>ESP8266 433 网关控制台</div></div>");
  page += F("<div class='toolbar'><label class='muted'>配色</label><select data-scheme><option value='system'>跟随系统</option><option value='light'>浅色</option><option value='dark'>深色</option></select></div></div>");
  page += F("<nav class='tabs' aria-label='页面切换'>");
  appendTabLink(page, "system", "系统信息");
  appendTabLink(page, "voice", "天猫精灵");
  appendTabLink(page, "baidu", "百度智慧屏");
  appendTabLink(page, "rf", "433 射频");
  appendTabLink(page, "ir", "空调红外");
  appendTabLink(page, "logs", "日志");
  page += F("</nav>");
}

void appendNewWifiForm(String& page) {
  page += F("<section class='card'><h2>Wi-Fi 切换</h2><form method='post' action='/wifi'>");
  appendWifiSsidField(page, "span3");
  page += F("<div class='field span2'><label>密码</label><input name='password' type='password' maxlength='64' placeholder='开放网络可留空' data-password><label class='muted' style='display:flex;gap:6px;align-items:center;margin-top:8px'><input type='checkbox' data-toggle-password style='width:auto;height:auto'>显示密码</label></div><button class='btn' type='submit'>连接</button></form>");
  page += F("<p class='muted'>提交后设备会保存新 Wi-Fi 并尝试连接；连接成功后 OLED 会显示新的 SSID 和 IP。</p></section>");
}

void appendOtaPanel(String& page) {
  page += F("<section class='card'><h2>固件升级</h2><form method='POST' action='/update' enctype='multipart/form-data'>");
  page += F("<div class='field span3'><label>firmware.bin</label><input name='update' type='file' accept='.bin'></div><button class='btn warn' type='submit'>上传固件</button></form>");
  page += F("<p class='muted'>也可以打开 <a href='/update'>/update</a> 使用原生 OTA 页面；认证：admin / ");
  page += AP_PASSWORD;
  page += F("</p></section>");
}

void appendSystemActionsPanel(String& page) {
  page += F("<section class='card'><h2>系统操作</h2><div class='actions'>");
  page += F("<a class='btn warn' href='/reboot' onclick='return confirm(\"确定要重启 ESP8266 吗？\")'>重启设备</a>");
  page += F("</div><p class='muted'>重启后 Wi-Fi 和语音服务会重新连接，浏览器需要等待设备重新上线。</p></section>");
}

void appendSystemPanel(String& page) {
  page += F("<section class='panel");
  if (isSelectedTab("system")) page += F(" active");
  page += F("' data-panel='system'><section class='card'><div class='section-head'><h2>系统信息</h2><span class='muted'>http://");
  page += deviceIP().toString();
  page += F("</span></div><div class='status'>");
  page += F("<div class='kv'><b>Wi-Fi</b>");
  page += wifiConnected() ? F("已连接") : F("配置热点");
  page += F("</div><div class='kv'><b>SSID</b>");
  page += wifiConnected() ? htmlEscape(WiFi.SSID()) : String(AP_SSID);
  page += F("</div><div class='kv'><b>IP</b>");
  page += deviceIP().toString();
  page += F("</div><div class='kv'><b>运行时间</b>");
  page += uptimeText();
  page += F("</div><div class='kv'><b>时间</b>");
  page += htmlEscape(localTimeTitle());
  page += F("</div><div class='kv'><b>堆内存</b>");
  page += formatKb(ESP.getFreeHeap());
  page += F(" / FRAG ");
  page += String(ESP.getHeapFragmentation());
  page += F("%</div><div class='kv'><b>Flash</b>");
  page += formatKb(ESP.getFlashChipRealSize());
  page += F(" / FW ");
  page += formatKb(ESP.getSketchSize());
  page += F("</div><div class='kv'><b>LittleFS</b>");
  page += fsReady ? F("已挂载") : F("挂载失败");
  page += F("</div><div class='kv'><b>OLED</b>");
  if (oledReady) {
    page += F("0x");
    page += String(oledAddress, HEX);
    page += F(" SDA GPIO");
    page += String(gpioNumber(oledSdaPin));
    page += F(" SCL GPIO");
    page += String(gpioNumber(oledSclPin));
  } else {
    page += F("未检测到");
  }
  page += F("</div><div class='kv'><b>最近消息</b>");
  page += htmlEscape(lastMessage);
  page += F("</div></div></section>");
  appendSystemActionsPanel(page);
  appendNewWifiForm(page);
  appendOtaPanel(page);
  page += F("</section>");
}

void appendVoicePanel(String& page) {
  page += F("<section class='panel");
  if (isSelectedTab("voice")) page += F(" active");
  page += F("' data-panel='voice'><section class='card'><h2>天猫精灵状态</h2><div class='status'>");
  page += F("<div class='kv'><b>Blinker</b>");
  page += voiceStarted ? F("已启用") : (hasBlinkerAuth() ? F("已配置，等待重启") : F("未配置"));
  page += F("</div><div class='kv'><b>Wi-Fi</b>");
  page += wifiConnected() ? F("已连接") : F("未连接");
  page += F("</div><div class='kv'><b>映射</b>插座 1/2/3 -> 码位 1/2/3</div></div></section>");
  page += F("<section class='card'><h2>Blinker 配置</h2><form method='post' action='/voice'>");
  page += F("<div class='field span3'><label>Blinker 设备密钥</label><input name='auth' maxlength='64' value='");
  page += htmlEscape(String(settings.blinkerAuth));
  page += F("' placeholder='在 Blinker App 创建 WiFi 设备后复制'></div><button class='btn' type='submit'>保存</button></form>");
  page += F("<p class='muted'>保存后请重启或重新上电，使 Blinker 连接生效。</p></section></section>");
}

void appendRfPanel(String& page) {
  page += F("<section class='panel");
  if (isSelectedTab("rf")) page += F(" active");
  page += F("' data-panel='rf'><section class='card'><h2>射频状态</h2><div class='status'>");
  page += F("<div class='kv'><b>433 接收脚</b>GPIO");
  page += String(gpioNumber(rfRxPin));
  page += F("</div><div class='kv'><b>433 发射脚</b>GPIO");
  page += String(gpioNumber(RF_TX_PIN));
  page += F("</div><div class='kv'><b>最近接收</b>");
  if (lastReceived.value == 0) {
    page += F("暂无");
  } else {
    page += String(lastReceived.value);
    page += F(" / ");
    page += String(lastReceived.bits);
    page += F(" bits");
  }
  page += F("</div></div><div class='actions' style='margin-top:12px'><a class='btn warn' href='/clear' onclick='return confirm(\"清除全部 433 码位？\")'>清除全部码位</a></div></section><section class='grid'>");

  for (uint8_t i = 0; i < RF_SLOT_COUNT; i++) {
    const RfCode& code = rfSlots[i].code;
    page += F("<article class='card slot-card'><h3>");
    page += htmlEscape(String(rfSlots[i].name));
    page += F("</h3><div class='actions'><a class='btn' href='/send?slot=");
    page += String(i + 1);
    page += F("'>发射</a><a class='btn secondary' href='/learn?slot=");
    page += String(i + 1);
    page += F("'>学习</a></div><div class='meta'>slot=");
    page += String(i + 1);
    if (hasCode(i)) {
      page += F("<br>value=");
      page += String(code.value);
      page += F("<br>bits=");
      page += String(code.bits);
      page += F(", protocol=");
      page += String(code.protocol);
      page += F(", pulse=");
      page += String(code.pulseLength);
    } else {
      page += F("<br>空码位");
    }
    page += F("<br><a href='/clear?slot=");
    page += String(i + 1);
    page += F("'>清除此码位</a></div></article>");
  }

  page += F("</section><section class='card'><h2>手动录入码位</h2><form method='get' action='/set'>");
  page += F("<div class='field'><label>码位 1-6</label><input name='slot' value='1' inputmode='numeric'></div><div class='field'><label>名称</label><input name='name' maxlength='16'></div>");
  page += F("<div class='field'><label>value</label><input name='value' inputmode='numeric'></div><div class='field'><label>bits</label><input name='bits' value='24' inputmode='numeric'></div>");
  page += F("<div class='field'><label>protocol</label><input name='protocol' value='1' inputmode='numeric'></div><div class='field'><label>pulse</label><input name='pulse' value='350' inputmode='numeric'></div><button class='btn warn' type='submit'>保存</button></form></section>");
  page += F("<section class='card'><h2>433 发射测试</h2><form method='get' action='/tx'><div class='field span3'><label>value</label><input name='value' inputmode='numeric'></div>");
  page += F("<div class='field'><label>bits</label><input name='bits' value='24' inputmode='numeric'></div><div class='field'><label>protocol</label><input name='protocol' value='1' inputmode='numeric'></div><div class='field'><label>pulse</label><input name='pulse' value='350' inputmode='numeric'></div><button class='btn' type='submit'>测试发射</button></form></section></section>");
}

void appendRfPanelV2(String& page) {
  page += F("<section class='panel");
  if (isSelectedTab("rf")) page += F(" active");
  page += F("' data-panel='rf'><section class='card'><h2>433 学习</h2><div class='status'>");
  page += F("<div class='kv'><b>学习状态</b><span data-rf-learn-state>");
  page += rfLearning ? F("等待接收") : (learnedCodeReady ? F("已接收，待保存") : F("空闲"));
  page += F("</span></div><div class='kv'><b>倒计时</b><span data-rf-countdown data-remaining='");
  page += rfLearning ? String((learnDeadline > millis()) ? (learnDeadline - millis() + 999UL) / 1000UL : 0) : String(0);
  page += F("'>");
  page += rfLearning ? String((learnDeadline > millis()) ? (learnDeadline - millis() + 999UL) / 1000UL : 0) : String(0);
  page += F("</span>s</div><div class='kv'><b>待保存码</b><span data-rf-pending>");
  if (learnedCodeReady) {
    page += String(learnedCode.value);
    page += F(" / ");
    page += String(learnedCode.bits);
    page += F(" bits");
  } else {
    page += F("暂无");
  }
  page += F("</span></div></div><div class='actions' style='margin-top:12px'><a class='btn' href='/learn'>开始学习 15 秒</a></div>");
  page += F("<p class='muted'>点击学习后触发遥控器。收到 433 固定码后会暂存在这里，再输入 slot 保存。</p></section>");

  page += F("<section class='card'><h2>保存到码位</h2><form method='get' action='/set'>");
  page += F("<div class='field'><label>码位 1-6</label><input name='slot' value='1' inputmode='numeric'></div>");
  page += F("<div class='field span2'><label>名称</label><input name='name' maxlength='16'></div>");
  page += F("<button class='btn warn' type='submit'>保存待保存码</button></form>");
  page += F("<p class='muted'>不填写 value 时，会把上面“待保存码”写入指定 slot。</p></section>");

  page += F("<section class='card'><h2>射频状态</h2><div class='status'>");
  page += F("<div class='kv'><b>433 接收脚</b>GPIO");
  page += String(gpioNumber(rfRxPin));
  page += F("</div><div class='kv'><b>433 发射脚</b>GPIO");
  page += String(gpioNumber(RF_TX_PIN));
  page += F("</div><div class='kv'><b>最近接收</b>");
  if (lastReceived.value == 0) {
    page += F("暂无");
  } else {
    page += String(lastReceived.value);
    page += F(" / ");
    page += String(lastReceived.bits);
    page += F(" bits");
  }
  page += F("</div></div><div class='actions' style='margin-top:12px'><a class='btn warn' href='/clear' onclick='return confirm(\"清除全部 433 码位？\")'>清除全部码位</a></div></section>");

  page += F("<section class='card'><h2>闭环自检</h2>");
  page += F("<p class='muted'>使用本机 433 发射模块发送固定测试码，再用本机接收模块等待接收。成功说明发射、接收、频率和 RX 中断链路基本正常。</p>");
  page += F("<div class='actions'><a class='btn secondary' href='/rf-self-test'>发射并接收测试码</a></div></section>");

  page += F("<section class='grid slot-grid'>");
  for (uint8_t i = 0; i < RF_SLOT_COUNT; i++) {
    const RfCode& code = rfSlots[i].code;
    page += F("<article class='card slot-card'><h3>");
    page += htmlEscape(String(rfSlots[i].name));
    page += F("</h3><div class='actions'><a class='btn' href='/send?slot=");
    page += String(i + 1);
    page += F("'>发射</a></div><div class='meta'>slot=");
    page += String(i + 1);
    if (hasCode(i)) {
      page += F("<br>value=");
      page += String(code.value);
      page += F("<br>bits=");
      page += String(code.bits);
      page += F(", protocol=");
      page += String(code.protocol);
      page += F(", pulse=");
      page += String(code.pulseLength);
    } else {
      page += F("<br>空码位");
    }
    page += F("<br><a href='/clear?slot=");
    page += String(i + 1);
    page += F("'>清除此码位</a></div></article>");
  }
  page += F("</section>");

  page += F("<section class='card'><h2>手动录入码位</h2><form method='get' action='/set'>");
  page += F("<div class='field'><label>码位 1-6</label><input name='slot' value='1' inputmode='numeric'></div><div class='field'><label>名称</label><input name='name' maxlength='16'></div>");
  page += F("<div class='field'><label>value</label><input name='value' inputmode='numeric'></div><div class='field'><label>bits</label><input name='bits' value='24' inputmode='numeric'></div>");
  page += F("<div class='field'><label>protocol</label><input name='protocol' value='1' inputmode='numeric'></div><div class='field'><label>pulse</label><input name='pulse' value='350' inputmode='numeric'></div><button class='btn warn' type='submit'>保存</button></form></section>");
  page += F("<section class='card'><h2>433 发射测试</h2><form method='get' action='/tx'><div class='field span3'><label>value</label><input name='value' inputmode='numeric'></div>");
  page += F("<div class='field'><label>bits</label><input name='bits' value='24' inputmode='numeric'></div><div class='field'><label>protocol</label><input name='protocol' value='1' inputmode='numeric'></div><div class='field'><label>pulse</label><input name='pulse' value='350' inputmode='numeric'></div><button class='btn' type='submit'>测试发射</button></form></section></section>");
}

void appendVoicePanelV2(String& page) {
  page += F("<section class='panel");
  if (isSelectedTab("voice")) page += F(" active");
  page += F("' data-panel='voice'>");
  page += F("<section class='card'><h2>天猫精灵状态</h2><div class='status'>");
  page += F("<div class='kv'><b>Blinker</b>");
  page += voiceStarted ? F("已启用") : (hasBlinkerAuth() ? F("已配置，等待重启") : F("未配置"));
  page += F("</div><div class='kv'><b>Wi-Fi</b>");
  page += wifiConnected() ? F("已连接") : F("未连接");
  page += F("</div><div class='kv'><b>设备类型</b>多路插座</div>");
  page += F("<div class='kv'><b>433 repeat</b>");
  page += String(normalizedVoiceRfRepeat(settings.voiceRfRepeat));
  page += F("</div><div class='kv'><b>语音映射</b>插座 1/2/3 -> 433，插座 4/5/6 -> 红外 1/2/3</div></div></section>");

  page += F("<section class='card'><h2>Blinker 配置</h2><form method='post' action='/voice'>");
  page += F("<div class='field span3'><label>Blinker 设备密钥</label><input name='auth' maxlength='64' value='");
  page += htmlEscape(String(settings.blinkerAuth));
  page += F("' placeholder='在 Blinker App 创建 WiFi 设备后复制'></div>");
  page += F("<div class='field'><label>语音 433 repeat</label><input name='repeat' value='");
  page += String(normalizedVoiceRfRepeat(settings.voiceRfRepeat));
  page += F("' inputmode='numeric'></div>");
  page += F("<button class='btn' type='submit'>保存</button></form>");
  page += F("<p class='muted'>切换型 433 设备建议从 12 开始试，仍漏发再提高；保存密钥后请重启或重新上电，使 Blinker 连接生效。</p></section>");

  page += F("<section class='card'><h2>接入天猫精灵步骤</h2>");
  page += F("<div class='voice-flow'>");
  page += F("<div class='voice-step'><b>1</b><strong>准备 433 码位</strong><small>先在 433 射频页保存码位 1、2、3。天猫精灵插座 1/2/3 会调用这三个码位。</small></div>");
  page += F("<div class='voice-step'><b>2</b><strong>Blinker 创建设备</strong><small>打开 Blinker App，新增 WiFi 设备，复制设备密钥到本页并保存。</small></div>");
  page += F("<div class='voice-step'><b>3</b><strong>重启 ESP8266</strong><small>设备需要重新启动后才会用密钥连接 Blinker 云端。</small></div>");
  page += F("<div class='voice-step'><b>4</b><strong>绑定天猫精灵</strong><small>在天猫精灵 App 添加智能家居技能，绑定 Blinker 账号并同步设备。</small></div>");
  page += F("<div class='voice-step'><b>5</b><strong>语音控制</strong><small>对天猫精灵说“打开插座 1”或“关闭插座 1”，设备会发射码位 1。</small></div>");
  page += F("</div></section>");

  page += F("<section class='card'><h2>示意图</h2><div class='voice-flow'>");
  page += F("<div class='voice-step'><b>A</b><strong>433 遥控器</strong><small>学习原始遥控信号，保存到本机码位。</small></div>");
  page += F("<div class='voice-step'><b>B</b><strong>ESP SmartMate</strong><small>接收天猫精灵经 Blinker 下发的插座指令。</small></div>");
  page += F("<div class='voice-step'><b>C</b><strong>433 发射模块</strong><small>把对应码位重新发射给灯、门禁或插座。</small></div>");
  page += F("<div class='voice-step'><b>D</b><strong>被控设备</strong><small>执行原遥控器对应的动作。</small></div>");
  page += F("<div class='voice-step'><b>E</b><strong>天猫精灵</strong><small>语音入口，负责识别“打开/关闭插座 N”。</small></div>");
  page += F("</div><p class='muted'>433 固定码通常只有一个触发码，所以“打开”和“关闭”会发送同一个码位。</p></section>");

  page += F("<section class='card'><h2>语音命令示例</h2><div class='voice-map'>");
  page += F("<div class='kv'><b>插座 1</b>打开/关闭插座 1<br>发送 433 slot 1</div>");
  page += F("<div class='kv'><b>插座 2</b>打开/关闭插座 2<br>发送 433 slot 2</div>");
  page += F("<div class='kv'><b>插座 3</b>打开/关闭插座 3<br>发送 433 slot 3</div>");
  page += F("<div class='kv'><b>插座 4</b>打开/关闭插座 4<br>发送红外 slot 1，例如空调开机</div>");
  page += F("<div class='kv'><b>插座 5</b>打开/关闭插座 5<br>发送红外 slot 2，例如空调关机</div>");
  page += F("<div class='kv'><b>插座 6</b>打开/关闭插座 6<br>发送红外 slot 3，例如空调模式</div>");
  page += F("</div></section></section>");
}

void appendBaiduPanel(String& page) {
  page += F("<section class='panel");
  if (isSelectedTab("baidu")) page += F(" active");
  page += F("' data-panel='baidu'>");
  page += F("<section class='card'><h2>百度智慧屏状态</h2><div class='status'>");
  page += F("<div class='kv'><b>Blinker / DuerOS</b>");
  page += voiceStarted ? F("已启用") : (hasBlinkerAuth() ? F("已配置，等待重启") : F("未配置"));
  page += F("</div><div class='kv'><b>Wi-Fi</b>");
  page += wifiConnected() ? F("已连接") : F("未连接");
  page += F("</div><div class='kv'><b>设备类型</b>多路插座</div>");
  page += F("<div class='kv'><b>433 repeat</b>");
  page += String(normalizedVoiceRfRepeat(settings.voiceRfRepeat));
  page += F("</div><div class='kv'><b>语音映射</b>插座 1/2/3 -> 433，插座 4/5/6 -> 红外 1/2/3</div></div></section>");

  page += F("<section class='card'><h2>Blinker 配置</h2><form method='post' action='/baidu'>");
  page += F("<div class='field span3'><label>Blinker 设备密钥</label><input name='auth' maxlength='64' value='");
  page += htmlEscape(String(settings.blinkerAuth));
  page += F("' placeholder='与天猫精灵共用同一个 Blinker WiFi 设备密钥'></div>");
  page += F("<div class='field'><label>语音 433 repeat</label><input name='repeat' value='");
  page += String(normalizedVoiceRfRepeat(settings.voiceRfRepeat));
  page += F("' inputmode='numeric'></div>");
  page += F("<button class='btn' type='submit'>保存</button></form>");
  page += F("<p class='muted'>百度智慧屏/小度通过 Blinker 的 DuerOS 接入。切换型 433 设备建议从 12 开始试；保存后请重启或重新上电，使 Blinker 连接生效。</p></section>");

  page += F("<section class='card'><h2>接入百度智慧屏步骤</h2>");
  page += F("<div class='voice-flow'>");
  page += F("<div class='voice-step'><b>1</b><strong>准备 433 码位</strong><small>先在 433 射频页保存 slot 1、2、3。百度智慧屏的插座 1/2/3 会调用这些码位。</small></div>");
  page += F("<div class='voice-step'><b>2</b><strong>Blinker 创建设备</strong><small>在 Blinker App 创建 WiFi 设备，复制设备密钥到本页并保存。</small></div>");
  page += F("<div class='voice-step'><b>3</b><strong>重启 ESP8266</strong><small>固件启动后会同时注册天猫精灵和 DuerOS 多路插座回调。</small></div>");
  page += F("<div class='voice-step'><b>4</b><strong>绑定小度</strong><small>在小度/百度智慧屏 App 的智能家居里添加 Blinker，并同步设备。</small></div>");
  page += F("<div class='voice-step'><b>5</b><strong>语音控制</strong><small>对小度说“打开插座 1”或“关闭插座 1”，设备会发射 433 slot 1。</small></div>");
  page += F("</div></section>");

  page += F("<section class='card'><h2>示意图</h2><div class='voice-flow'>");
  page += F("<div class='voice-step'><b>A</b><strong>百度智慧屏</strong><small>识别“小度小度，打开插座 N”的语音指令。</small></div>");
  page += F("<div class='voice-step'><b>B</b><strong>Blinker 云端</strong><small>把 DuerOS 指令转发给在线的 ESP8266。</small></div>");
  page += F("<div class='voice-step'><b>C</b><strong>ESP SmartMate</strong><small>收到插座编号后查找对应 433 slot。</small></div>");
  page += F("<div class='voice-step'><b>D</b><strong>433 发射模块</strong><small>重新发射已学习的固定码。</small></div>");
  page += F("<div class='voice-step'><b>E</b><strong>被控设备</strong><small>执行原遥控器的动作。</small></div>");
  page += F("</div><p class='muted'>如果天猫精灵已经能用，百度智慧屏通常只需要在小度 App 中绑定同一个 Blinker 账号并同步设备。</p></section>");

  page += F("<section class='card'><h2>语音命令示例</h2><div class='voice-map'>");
  page += F("<div class='kv'><b>插座 1</b>小度小度，打开插座 1<br>发送 433 slot 1</div>");
  page += F("<div class='kv'><b>插座 2</b>小度小度，关闭插座 2<br>发送 433 slot 2</div>");
  page += F("<div class='kv'><b>插座 3</b>小度小度，打开插座 3<br>发送 433 slot 3</div>");
  page += F("<div class='kv'><b>插座 4</b>小度小度，打开插座 4<br>发送红外 slot 1，例如空调开机</div>");
  page += F("<div class='kv'><b>插座 5</b>小度小度，关闭插座 5<br>发送红外 slot 2，例如空调关机</div>");
  page += F("<div class='kv'><b>插座 6</b>小度小度，打开插座 6<br>发送红外 slot 3</div>");
  page += F("</div></section></section>");
}

void appendIrPanel(String& page) {
  page += F("<section class='panel");
  if (isSelectedTab("ir")) page += F(" active");
  page += F("' data-panel='ir'><section class='card'><h2>空调红外学习</h2><div class='status'>");
  page += F("<div class='kv'><b>学习状态</b><span data-ir-learn-state>");
  page += irLearning ? F("等待接收") : (learnedIrReady ? F("已接收，待保存") : F("空闲"));
  page += F("</span></div><div class='kv'><b>倒计时</b><span data-ir-countdown>");
  page += irLearning ? String((learnDeadline > millis()) ? (learnDeadline - millis() + 999UL) / 1000UL : 0) : String(0);
  page += F("</span>s</div><div class='kv'><b>待保存红外码</b><span data-ir-pending>");
  if (learnedIrReady) {
    page += String(learnedIrLength);
    page += F(" pulses / ");
    page += String(learnedIrFrequency);
    page += F(" kHz");
  } else {
    page += F("暂无");
  }
  page += F("</span></div></div><div class='actions' style='margin-top:12px'><a class='btn' href='/ir-learn'>开始学习 15 秒</a></div>");
  page += F("<p class='muted'>照片中的 HW-477 是红外发射模块：S 接 D0/GPIO16，+ 接 3V，- 接 GND。记录空调遥控器还需要另接红外接收头：S 接 D4/GPIO2，VCC 接 3V，GND 接 GND。</p></section>");

  page += F("<section class='card'><h2>保存到红外 slot</h2><form method='get' action='/ir-set'>");
  page += F("<div class='field'><label>slot 1-6</label><input name='slot' value='1' inputmode='numeric'></div>");
  page += F("<div class='field span2'><label>名称</label><input name='name' maxlength='16' placeholder='例如 空调开机'></div>");
  page += F("<button class='btn warn' type='submit'>保存待保存红外码</button></form>");
  page += F("<p class='muted'>空调遥控器每个按键通常是一整套状态，请分别学习“开机 26 度制冷”、“关机”等命令。</p></section>");

#if ENABLE_IR_AC_PRESETS
  page += F("<section class='card'><h2>常用品牌空调预设</h2>");
  page += F("<form method='get' action='/ir-preset-test'>");
  page += F("<div class='field'><label>品牌</label><select name='brand'><option value='xiaomi'>小米</option><option value='daikin'>大金</option><option value='kelon'>科龙</option><option value='hisense'>海信</option><option value='hualing'>华凌</option><option value='gree'>格力</option><option value='midea'>美的</option></select></div>");
  page += F("<div class='field'><label>电源</label><select name='power'><option value='1'>开机</option><option value='0'>关机</option></select></div>");
  page += F("<div class='field'><label>模式</label><select name='mode'><option value='1'>制冷</option><option value='2'>制热</option><option value='3'>除湿</option><option value='4'>送风</option><option value='0'>自动</option></select></div>");
  page += F("<div class='field'><label>温度</label><input name='temp' value='26' inputmode='numeric'></div>");
  page += F("<div class='field'><label>风速</label><select name='fan'><option value='0'>自动</option><option value='1'>低</option><option value='2'>中</option><option value='3'>高</option><option value='4'>最大</option></select></div>");
  page += F("<button class='btn' type='submit'>测试发送</button></form>");
  page += F("<form method='get' action='/ir-preset-save' style='margin-top:10px'>");
  page += F("<div class='field'><label>slot 1-6</label><input name='slot' value='1' inputmode='numeric'></div>");
  page += F("<div class='field span2'><label>名称</label><input name='name' maxlength='16' placeholder='例如 空调开机'></div>");
  page += F("<div class='field'><label>品牌</label><select name='brand'><option value='xiaomi'>小米</option><option value='daikin'>大金</option><option value='kelon'>科龙</option><option value='hisense'>海信</option><option value='hualing'>华凌</option><option value='gree'>格力</option><option value='midea'>美的</option></select></div>");
  page += F("<div class='field'><label>电源</label><select name='power'><option value='1'>开机</option><option value='0'>关机</option></select></div>");
  page += F("<div class='field'><label>模式</label><select name='mode'><option value='1'>制冷</option><option value='2'>制热</option><option value='3'>除湿</option><option value='4'>送风</option><option value='0'>自动</option></select></div>");
  page += F("<div class='field'><label>温度</label><input name='temp' value='26' inputmode='numeric'></div>");
  page += F("<div class='field'><label>风速</label><select name='fan'><option value='0'>自动</option><option value='1'>低</option><option value='2'>中</option><option value='3'>高</option><option value='4'>最大</option></select></div>");
  page += F("<button class='btn warn' type='submit'>保存预设到 slot</button></form>");
  page += F("<p class='muted'>先测试，确认空调响应后再保存。小米/华凌按美的协议测试，海信按科龙兼容协议测试；不兼容时请使用上面的红外学习保存 raw 码。</p></section>");
#else
  page += F("<section class='card'><h2>常用品牌空调预设</h2><p class='muted'>当前固件未启用空调预设，以降低 RAM 和固件体积；需要时在 platformio.ini 中设置 ENABLE_IR_AC_PRESETS=1 后重新编译。</p></section>");
#endif

  page += F("<section class='card'><h2>红外状态</h2><div class='status'>");
  page += F("<div class='kv'><b>红外发射</b>GPIO");
  page += String(gpioNumber(IR_TX_PIN));
  page += F("</div><div class='kv'><b>红外接收</b>GPIO");
  page += String(gpioNumber(IR_RX_PIN));
  page += F("</div><div class='kv'><b>天猫映射</b>插座 4/5/6 -> IR slot 1/2/3</div>");
  page += F("<div class='kv'><b>频率</b>默认 38 kHz</div></div>");
  page += F("<div class='actions' style='margin-top:12px'><a class='btn warn' href='/ir-clear' onclick='return confirm(\"清除全部红外 slot？\")'>清除全部红外码</a></div></section>");

  page += F("<section class='grid slot-grid'>");
  for (uint8_t i = 0; i < IR_SLOT_COUNT; i++) {
    const IrSlot& code = irSlots[i];
    page += F("<article class='card slot-card'><h3>");
    page += htmlEscape(String(code.name));
    page += F("</h3><div class='actions'><a class='btn' href='/ir-send?slot=");
    page += String(i + 1);
    page += F("'>发射</a></div><div class='meta'>slot=");
    page += String(i + 1);
    if (irPresetEnabled(code)) {
#if ENABLE_IR_AC_PRESETS
      page += F("<br>preset=");
      page += htmlEscape(brandForAcProtocol(code.protocol));
      page += F("<br>");
      page += code.power ? F("开机 ") : F("关机 ");
      if (code.power) {
        page += htmlEscape(acModeName(code.mode));
        page += F(" ");
        page += String(code.temp);
        page += F("C ");
        page += htmlEscape(acFanName(code.fan));
      }
#endif
    } else if (hasIrCode(i)) {
      page += F("<br>len=");
      page += String(code.length);
      page += F(" pulses<br>freq=");
      page += String(code.frequency);
      page += F(" kHz");
    } else {
      page += F("<br>空红外码");
    }
    page += F("<br><a href='/ir-clear?slot=");
    page += String(i + 1);
    page += F("'>清除此红外码</a></div></article>");
  }
  page += F("</section></section>");
}

void appendLogsPanel(String& page) {
  page += F("<section class='panel");
  if (isSelectedTab("logs")) page += F(" active");
  page += F("' data-panel='logs'><section class='card'><div class='section-head'><h2>运行日志</h2><div class='actions'><button class='btn secondary' type='button' data-refresh-logs>刷新</button><button class='btn warn' type='button' data-clear-logs>清除</button><a class='btn secondary' href='/logs' target='_blank'>纯文本</a></div></div><div class='muted' data-log-status>最近 ");
  page += String(logLineUsed);
  page += F(" 条</div><pre class='log-view' data-log-view>");
  for (uint8_t i = 0; i < logLineUsed; i++) {
    uint8_t index = (logLineNext + LOG_LINE_COUNT - logLineUsed + i) % LOG_LINE_COUNT;
    page += htmlEscape(String(logLines[index]));
    page += '\n';
  }
  page += F("</pre></section></section>");
}

String renderDashboardPage() {
  String page;
  page.reserve(4096);
  appendPageStart(page, F("ESP SmartMate"));
  appendTabShell(page);
  String tab = selectedTab();
  if (tab == F("voice")) {
    appendVoicePanelV2(page);
  } else if (tab == F("baidu")) {
    appendBaiduPanel(page);
  } else if (tab == F("rf")) {
    appendRfPanelV2(page);
  } else if (tab == F("ir")) {
    appendIrPanel(page);
  } else if (tab == F("logs")) {
    appendLogsPanel(page);
  } else {
    appendSystemPanel(page);
  }
  appendPageEnd(page);
  Serial.printf("Dashboard rendered tab=%s len=%u heap=%u frag=%u\n",
                tab.c_str(),
                page.length(),
                ESP.getFreeHeap(),
                ESP.getHeapFragmentation());
  return page;
}

String renderPage() {
  String page;
  page.reserve(11000);
  appendPageStart(page, F("ESP8266 433 状态"));

  page += F("<div class='top'><div><h1>ESP8266 433 状态</h1><div class='muted'>网络配置、设备状态、433 接收/发射测试</div></div>");
  page += F("<div class='muted'>访问地址: http://");
  page += deviceIP().toString();
  page += F("<br>Last: ");
  page += htmlEscape(lastMessage);
  page += F("</div></div>");

  if (!wifiConnected()) {
    appendWifiForm(page);
  }

  page += F("<section class='card' style='margin-bottom:14px'><h2>设备状态</h2><div class='status'>");
  page += F("<div class='kv'><b>Wi-Fi</b>");
  page += wifiConnected() ? F("已连接") : F("配置热点");
  page += F("</div><div class='kv'><b>SSID</b>");
  page += wifiConnected() ? htmlEscape(WiFi.SSID()) : String(AP_SSID);
  page += F("</div><div class='kv'><b>IP</b>");
  page += deviceIP().toString();
  page += F("</div><div class='kv'><b>433 接收</b>");
  if (lastReceived.value == 0) {
    page += F("暂无");
  } else {
    page += String(lastReceived.value);
    page += F(" / ");
    page += String(lastReceived.bits);
    page += F(" bits");
  }
  page += F("</div><div class='kv'><b>天猫精灵</b>");
  if (voiceStarted) {
    page += F("已启用");
  } else if (hasBlinkerAuth()) {
    page += F("待连接/需重启");
  } else {
    page += F("未配置");
  }
  page += F("</div><div class='kv'><b>OLED</b>");
  if (oledReady) {
    page += F("0x");
    page += String(oledAddress, HEX);
    page += F(" SDA GPIO");
    page += String(gpioNumber(oledSdaPin));
    page += F(" SCL GPIO");
    page += String(gpioNumber(oledSclPin));
  } else {
    page += F("未检测到");
  }
  page += F("</div><div class='kv'><b>433 接收脚</b>GPIO");
  page += String(gpioNumber(rfRxPin));
  page += F("</div></div>");
  if (wifiConnected()) {
    page += F("<p class='muted inline-actions'><a class='btn secondary' href='/config'>重新配置 Wi-Fi</a><a class='btn secondary' href='/voice-config'>配置天猫精灵</a><a class='btn secondary' href='/baidu-config'>配置百度智慧屏</a></p>");
  }
  page += F("</section>");

  page += F("<section class='grid'>");
  for (uint8_t i = 0; i < RF_SLOT_COUNT; i++) {
    const RfCode& code = rfSlots[i].code;
    page += F("<article class='card'><h2>");
    page += String(i + 1);
    page += F(" 号码位</h2><a class='btn' href='/send?slot=");
    page += String(i + 1);
    page += F("'>发射保存码</a><a class='btn secondary' style='margin-top:8px' href='/learn?slot=");
    page += String(i + 1);
    page += F("'>学习接收码</a><div class='meta'>");
    if (hasCode(i)) {
      page += F("value=");
      page += String(code.value);
      page += F("<br>bits=");
      page += String(code.bits);
      page += F(", protocol=");
      page += String(code.protocol);
      page += F(", pulse=");
      page += String(code.pulseLength);
    } else {
      page += F("还没有编码，请点击学习或手动录入。");
    }
    page += F("</div></article>");
  }
  page += F("</section>");

  page += F("<section class='card' style='margin-top:14px'><h2>手动录入保存码</h2>");
  page += F("<form method='get' action='/set'>");
  page += F("<div class='field'><label>码位 1-6</label><input name='slot' value='1' inputmode='numeric'></div>");
  page += F("<div class='field'><label>value</label><input name='value' inputmode='numeric'></div>");
  page += F("<div class='field'><label>bits</label><input name='bits' value='24' inputmode='numeric'></div>");
  page += F("<div class='field'><label>protocol</label><input name='protocol' value='1' inputmode='numeric'></div>");
  page += F("<div class='field'><label>pulse</label><input name='pulse' value='350' inputmode='numeric'></div>");
  page += F("<button class='btn warn' type='submit'>保存</button></form>");
  page += F("<p class='muted'>学习模式：点击“学习接收码”后 15 秒内触发 433 遥控器，模块会保存接收到的固定码。</p></section>");

  page += F("<section class='card' style='margin-top:14px'><h2>433 发射测试</h2>");
  page += F("<form method='get' action='/tx'>");
  page += F("<div class='field span2'><label>value</label><input name='value' inputmode='numeric'></div>");
  page += F("<div class='field'><label>bits</label><input name='bits' value='24' inputmode='numeric'></div>");
  page += F("<div class='field'><label>protocol</label><input name='protocol' value='1' inputmode='numeric'></div>");
  page += F("<div class='field'><label>pulse</label><input name='pulse' value='350' inputmode='numeric'></div>");
  page += F("<button class='btn' type='submit'>发射测试</button></form></section>");

  appendPageEnd(page);
  return page;
}

String renderVoiceConfigPage() {
  String page;
  page.reserve(5200);
  appendPageStart(page, F("ESP8266 天猫精灵配置"));
  page += F("<div class='top'><div><h1>天猫精灵配置</h1><div class='muted'>通过 Blinker/AliGenie 把语音开关映射到 433 码位</div></div>");
  page += F("<div class='muted'>状态: ");
  page += voiceStarted ? F("已启用") : (hasBlinkerAuth() ? F("待连接/需重启") : F("未配置"));
  page += F("<br>Wi-Fi: ");
  page += wifiConnected() ? F("已连接") : F("未连接");
  page += F("</div></div>");
  appendVoiceForm(page);
  page += F("<section class='card'><h2>语音映射</h2>");
  page += F("<p class='muted'>天猫精灵识别为多路插座后，“打开插孔1/2/3”或“关闭插孔1/2/3”都会发射对应码位。433 固定码本身通常只有一个触发码，所以开和关都会发送同一个保存码。</p>");
  page += F("</section>");
  appendPageEnd(page);
  return page;
}

String renderConfigPage() {
  String page;
  page.reserve(6000);
  appendPageStart(page, F("ESP8266 Wi-Fi 配置"));
  page += F("<div class='top'><div><h1>Wi-Fi 配置</h1><div class='muted'>选择新的 Wi-Fi 并保存到设备</div></div>");
  page += F("<div class='muted'>");
  if (wifiConnected()) {
    page += F("当前 SSID: ");
    page += htmlEscape(WiFi.SSID());
    page += F("<br>当前 IP: ");
    page += WiFi.localIP().toString();
  } else {
    page += F("热点 SSID: ");
    page += AP_SSID;
    page += F("<br>热点密码: ");
    page += AP_PASSWORD;
    page += F("<br>IP: ");
    page += WiFi.softAPIP().toString();
  }
  page += F("</div></div>");
  appendWifiForm(page);
  appendVoiceForm(page);
  appendPageEnd(page);
  return page;
}

String renderMainPage() {
  String page;
  page.reserve(16000);
  appendPageStart(page, F("ESP8266 433"));

  page += F("<div class='top'><div><h1>ESP8266 433</h1><div class='muted'>Wi-Fi, 433 code manager, LittleFS and OTA</div></div>");
  page += F("<div class='muted'>URL: http://");
  page += deviceIP().toString();
  page += F("<br>Last: ");
  page += htmlEscape(lastMessage);
  page += F("</div></div>");

  if (!wifiConnected()) {
    appendWifiForm(page);
  }

  page += F("<section class='card' style='margin-bottom:14px'><h2>Status</h2><div class='status'>");
  page += F("<div class='kv'><b>Wi-Fi</b>");
  page += wifiConnected() ? F("Connected") : F("Setup AP");
  page += F("</div><div class='kv'><b>SSID</b>");
  page += wifiConnected() ? htmlEscape(WiFi.SSID()) : String(AP_SSID);
  page += F("</div><div class='kv'><b>IP</b>");
  page += deviceIP().toString();
  page += F("</div><div class='kv'><b>Last RX</b>");
  if (lastReceived.value == 0) {
    page += F("None");
  } else {
    page += String(lastReceived.value);
    page += F(" / ");
    page += String(lastReceived.bits);
    page += F(" bits");
  }
  page += F("</div><div class='kv'><b>Voice</b>");
  page += voiceStarted ? F("Ready") : (hasBlinkerAuth() ? F("Configured, reboot") : F("Not configured"));
  page += F("</div><div class='kv'><b>LittleFS</b>");
  page += fsReady ? F("Mounted") : F("Failed");
  page += F("</div><div class='kv'><b>OTA</b><a href='/update'>/update</a><br>admin / ");
  page += AP_PASSWORD;
  page += F("</div><div class='kv'><b>RF RX GPIO</b>");
  page += String(gpioNumber(rfRxPin));
  page += F("</div></div>");
  page += F("<p class='muted inline-actions'><a class='btn secondary' href='/config'>Wi-Fi</a><a class='btn secondary' href='/voice-config'>Voice</a><a class='btn secondary' href='/update'>OTA Update</a><a class='btn warn' href='/clear'>Clear All Codes</a></p>");
  page += F("</section>");

  page += F("<section class='grid'>");
  for (uint8_t i = 0; i < RF_SLOT_COUNT; i++) {
    const RfCode& code = rfSlots[i].code;
    page += F("<article class='card'><h2>");
    page += htmlEscape(String(rfSlots[i].name));
    page += F("</h2><a class='btn' href='/send?slot=");
    page += String(i + 1);
    page += F("'>Send</a><a class='btn secondary' style='margin-top:8px' href='/learn?slot=");
    page += String(i + 1);
    page += F("'>Learn</a><div class='meta'>slot=");
    page += String(i + 1);
    if (hasCode(i)) {
      page += F("<br>value=");
      page += String(code.value);
      page += F("<br>bits=");
      page += String(code.bits);
      page += F(", protocol=");
      page += String(code.protocol);
      page += F(", pulse=");
      page += String(code.pulseLength);
    } else {
      page += F("<br>empty");
    }
    page += F("<br><a href='/clear?slot=");
    page += String(i + 1);
    page += F("'>clear this slot</a></div></article>");
  }
  page += F("</section>");

  page += F("<section class='card' style='margin-top:14px'><h2>Save RF Code</h2>");
  page += F("<form method='get' action='/set'>");
  page += F("<div class='field'><label>slot 1-6</label><input name='slot' value='1' inputmode='numeric'></div>");
  page += F("<div class='field'><label>name</label><input name='name' maxlength='16'></div>");
  page += F("<div class='field'><label>value</label><input name='value' inputmode='numeric'></div>");
  page += F("<div class='field'><label>bits</label><input name='bits' value='24' inputmode='numeric'></div>");
  page += F("<div class='field'><label>protocol</label><input name='protocol' value='1' inputmode='numeric'></div>");
  page += F("<div class='field'><label>pulse</label><input name='pulse' value='350' inputmode='numeric'></div>");
  page += F("<button class='btn warn' type='submit'>Save</button></form>");
  page += F("<p class='muted'>Learn mode waits 15 seconds for a 433 fixed code. Voice control still maps AliGenie outlet 1/2/3 to slots 1/2/3.</p></section>");

  page += F("<section class='card' style='margin-top:14px'><h2>433 TX Test</h2>");
  page += F("<form method='get' action='/tx'>");
  page += F("<div class='field span2'><label>value</label><input name='value' inputmode='numeric'></div>");
  page += F("<div class='field'><label>bits</label><input name='bits' value='24' inputmode='numeric'></div>");
  page += F("<div class='field'><label>protocol</label><input name='protocol' value='1' inputmode='numeric'></div>");
  page += F("<div class='field'><label>pulse</label><input name='pulse' value='350' inputmode='numeric'></div>");
  page += F("<button class='btn' type='submit'>Send Test</button></form></section>");

  appendPageEnd(page);
  return page;
}

uint8_t parseSlot() {
  if (!server.hasArg("slot")) return 0;
  int slot = server.arg("slot").toInt();
  if (slot < 1 || slot > RF_SLOT_COUNT) return 0;
  return static_cast<uint8_t>(slot);
}

uint8_t parseBoundedArg(const char* name, uint8_t fallback, uint8_t minimum, uint8_t maximum) {
  if (!server.hasArg(name)) return fallback;
  int value = server.arg(name).toInt();
  if (value < minimum) value = minimum;
  if (value > maximum) value = maximum;
  return static_cast<uint8_t>(value);
}

#if ENABLE_IR_AC_PRESETS
decode_type_t parseAcProtocolArg() {
  String brand = server.arg("brand");
  brand.trim();
  brand.toLowerCase();
  return acProtocolForBrand(brand);
}

String parseAcNameArg(decode_type_t protocol, bool power, uint8_t mode, uint8_t temp) {
  String name = server.hasArg("name") ? server.arg("name") : "";
  name.trim();
  if (name.length() == 0) {
    name = brandForAcProtocol(protocol);
    name += power ? F(" ") : F(" off");
    if (power) {
      name += acModeName(mode);
      name += F(" ");
      name += String(temp);
      name += F("C");
    }
  }
  return name;
}
#endif

void sendChunk(PGM_P text) {
  server.sendContent_P(text);
  yield();
}

void sendChunk(const String& text) {
  server.sendContent(text);
  yield();
}

void sendEscapedChunk(const String& text) {
  String escaped = htmlEscape(text);
  server.sendContent(escaped);
  yield();
}

void sendKv(PGM_P label, const String& value) {
  sendChunk(PSTR("<div class='kv'><b>"));
  sendChunk(label);
  sendChunk(PSTR("</b>"));
  sendEscapedChunk(value);
  sendChunk(PSTR("</div>"));
}

void sendTabLinkLite(const char* tab, PGM_P label) {
  sendChunk(PSTR("<a class='tab"));
  if (isSelectedTab(tab)) sendChunk(PSTR(" active"));
  sendChunk(PSTR("' href='/?tab="));
  sendChunk(String(tab));
  sendChunk(PSTR("#"));
  sendChunk(String(tab));
  sendChunk(PSTR("' data-tab='"));
  sendChunk(String(tab));
  sendChunk(PSTR("'>"));
  sendChunk(label);
  sendChunk(PSTR("</a>"));
}

void sendDashboardStartLite() {
  sendChunk(PSTR("<!doctype html><html lang='zh-CN'><head><meta charset='utf-8'>"
                 "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                 "<title>ESP SmartMate</title><link rel='stylesheet' href='/app.css'></head>"
                 "<body><main class='wrap'>"));
  sendChunk(PSTR("<div class='top'><div class='title'><h1>ESP SmartMate</h1>"
                 "<div class='muted'>ESP8266 433 网关控制台</div></div>"
                 "<div class='toolbar'><label class='muted'>配色</label>"
                 "<select data-scheme><option value='system'>跟随系统</option>"
                 "<option value='light'>浅色</option><option value='dark'>深色</option></select></div></div>"));
  sendChunk(PSTR("<nav class='tabs' aria-label='页面切换'>"));
  sendTabLinkLite("system", PSTR("系统信息"));
  sendTabLinkLite("voice", PSTR("天猫精灵"));
  sendTabLinkLite("baidu", PSTR("百度智慧屏"));
  sendTabLinkLite("rf", PSTR("433 射频"));
  sendTabLinkLite("ir", PSTR("空调红外"));
  sendTabLinkLite("logs", PSTR("日志"));
  sendChunk(PSTR("</nav>"));
}

void sendDashboardEndLite() {
  sendChunk(PSTR("</main><script src='/app.js'></script></body></html>"));
}

void sendWifiFormLite() {
  sendChunk(PSTR("<section class='card'><h2>Wi-Fi 切换</h2><form method='post' action='/wifi'>"
                 "<div class='field span3'><label>Wi-Fi 名称</label><input name='ssid' maxlength='32' value='"));
  sendEscapedChunk(wifiConnected() ? WiFi.SSID() : String(settings.wifiSsid));
  sendChunk(PSTR("' placeholder='输入 Wi-Fi 名称'><div class='meta'><a href='/?tab=system&scan=1#system'>扫描 Wi-Fi</a></div></div>"));
  if (server.hasArg("scan") && server.arg("scan") == F("1")) {
    Serial.printf("WiFi scan requested heap=%u frag=%u\n", ESP.getFreeHeap(), ESP.getHeapFragmentation());
    int networkCount = WiFi.scanNetworks();
    sendChunk(PSTR("<div class='field span3'><label>扫描结果</label><select onchange='var s=this.form.ssid;if(s)s.value=this.value'>"));
    for (int i = 0; i < networkCount; i++) {
      String ssid = WiFi.SSID(i);
      sendChunk(PSTR("<option value='"));
      sendEscapedChunk(ssid);
      sendChunk(PSTR("'>"));
      sendEscapedChunk(ssid);
      sendChunk(PSTR(" ("));
      sendChunk(String(WiFi.RSSI(i)));
      sendChunk(PSTR(" dBm)</option>"));
    }
    if (networkCount <= 0) sendChunk(PSTR("<option value=''>未扫描到 Wi-Fi</option>"));
    sendChunk(PSTR("</select></div>"));
  }
  sendChunk(PSTR("<div class='field span2'><label>密码</label><input name='password' type='password' maxlength='64' "
                 "placeholder='开放网络可留空' data-password><label class='muted' style='display:flex;gap:6px;align-items:center;margin-top:8px'>"
                 "<input type='checkbox' data-toggle-password style='width:auto;height:auto'>显示密码</label></div>"
                 "<button class='btn' type='submit'>连接</button></form></section>"));
}

void sendSystemPanelLite() {
  sendChunk(PSTR("<section class='panel active' data-panel='system'><section class='card'><div class='section-head'><h2>系统信息</h2><span class='muted'>http://"));
  sendChunk(deviceIP().toString());
  sendChunk(PSTR("</span></div><div class='status'>"));
  sendKv(PSTR("Wi-Fi"), wifiConnected() ? F("已连接") : F("配置热点"));
  sendKv(PSTR("SSID"), wifiConnected() ? WiFi.SSID() : String(AP_SSID));
  sendKv(PSTR("IP"), deviceIP().toString());
  sendKv(PSTR("运行时间"), uptimeText());
  sendKv(PSTR("时间"), localTimeTitle());
  sendKv(PSTR("堆内存"), formatKb(ESP.getFreeHeap()) + F(" / FRAG ") + String(ESP.getHeapFragmentation()) + F("%"));
  sendKv(PSTR("Flash"), formatKb(ESP.getFlashChipRealSize()) + F(" / FW ") + formatKb(ESP.getSketchSize()));
  sendKv(PSTR("LittleFS"), fsReady ? F("已挂载") : F("挂载失败"));
  String oled = oledReady ? (String("0x") + String(oledAddress, HEX) + F(" SDA GPIO") + String(gpioNumber(oledSdaPin)) + F(" SCL GPIO") + String(gpioNumber(oledSclPin))) : String(F("未检测到"));
  sendKv(PSTR("OLED"), oled);
  sendKv(PSTR("最近消息"), lastMessage);
  sendChunk(PSTR("</div></section><section class='card'><h2>系统操作</h2><div class='actions'>"
                 "<a class='btn warn' href='/reboot' onclick='return confirm(\"确定要重启 ESP8266 吗？\")'>重启设备</a>"
                 "</div></section>"));
  sendWifiFormLite();
  sendChunk(PSTR("<section class='card'><h2>固件升级</h2><form method='POST' action='/update' enctype='multipart/form-data'>"
                 "<div class='field span3'><label>firmware.bin</label><input name='update' type='file' accept='.bin'></div>"
                 "<button class='btn warn' type='submit'>上传固件</button></form></section></section>"));
}

void sendVoicePanelLite(bool baidu) {
  const char* tab = baidu ? "baidu" : "voice";
  sendChunk(PSTR("<section class='panel active' data-panel='"));
  sendChunk(String(tab));
  sendChunk(PSTR("'><section class='card'><h2>"));
  sendChunk(baidu ? PSTR("百度智慧屏") : PSTR("天猫精灵"));
  sendChunk(PSTR("</h2><div class='status'>"));
  sendKv(PSTR("Blinker"), voiceStarted ? F("已启用") : (hasBlinkerAuth() ? F("已配置，等待重启") : F("未配置")));
  sendKv(PSTR("Wi-Fi"), wifiConnected() ? F("已连接") : F("未连接"));
  sendKv(PSTR("433 repeat"), String(normalizedVoiceRfRepeat(settings.voiceRfRepeat)));
  sendKv(PSTR("语音映射"), F("插座 1/2/3 -> 433，插座 4/5/6 -> 红外 1/2/3"));
  sendChunk(PSTR("</div></section><section class='card'><h2>Blinker 配置</h2><form method='post' action='"));
  sendChunk(baidu ? PSTR("/baidu") : PSTR("/voice"));
  sendChunk(PSTR("'><div class='field span3'><label>Blinker 设备密钥</label><input name='auth' maxlength='64' value='"));
  sendEscapedChunk(String(settings.blinkerAuth));
  sendChunk(PSTR("' placeholder='Blinker WiFi 设备密钥'></div><div class='field'><label>语音 433 repeat</label><input name='repeat' value='"));
  sendChunk(String(normalizedVoiceRfRepeat(settings.voiceRfRepeat)));
  sendChunk(PSTR("' inputmode='numeric'></div><button class='btn' type='submit'>保存</button></form>"
                 "<p class='muted'>切换型 433 设备建议从 12 开始试；保存密钥后请重启设备，使 Blinker 连接生效。</p></section>"
                 "<section class='card'><h2>常用命令</h2><div class='voice-map'>"
                 "<div class='kv'><b>插座 1</b>发送 433 slot 1</div>"
                 "<div class='kv'><b>插座 2</b>发送 433 slot 2</div>"
                 "<div class='kv'><b>插座 3</b>发送 433 slot 3</div>"
                 "<div class='kv'><b>插座 4</b>发送红外 slot 1</div>"
                 "<div class='kv'><b>插座 5</b>发送红外 slot 2</div>"
                 "<div class='kv'><b>插座 6</b>发送红外 slot 3</div>"
                 "</div></section></section>"));
}

void sendRfPanelLite() {
  sendChunk(PSTR("<section class='panel active' data-panel='rf'><section class='card'><h2>433 学习</h2><div class='status'>"));
  sendKv(PSTR("学习状态"), rfLearning ? F("等待接收") : (learnedCodeReady ? F("已接收，待保存") : F("空闲")));
  sendKv(PSTR("倒计时"), String(rfLearning && learnDeadline > millis() ? (learnDeadline - millis() + 999UL) / 1000UL : 0) + F("s"));
  sendKv(PSTR("待保存码"), learnedCodeReady ? (String(learnedCode.value) + F(" / ") + String(learnedCode.bits) + F(" bits")) : String(F("暂无")));
  sendChunk(PSTR("</div><div class='actions' style='margin-top:12px'><a class='btn' href='/learn'>开始学习 15 秒</a>"
                 "<a class='btn secondary' href='/rf-self-test'>闭环自检</a></div></section>"
                 "<section class='card'><h2>保存到码位</h2><form method='get' action='/set'>"
                 "<div class='field'><label>slot 1-6</label><input name='slot' value='1' inputmode='numeric'></div>"
                 "<div class='field span2'><label>名称</label><input name='name' maxlength='16'></div>"
                 "<button class='btn warn' type='submit'>保存待保存码</button></form></section>"
                 "<section class='grid slot-grid'>"));
  for (uint8_t i = 0; i < RF_SLOT_COUNT; i++) {
    sendChunk(PSTR("<article class='card slot-card'><h3>"));
    sendEscapedChunk(String(rfSlots[i].name));
    sendChunk(PSTR("</h3><div class='actions'><a class='btn' href='/send?slot="));
    sendChunk(String(i + 1));
    sendChunk(PSTR("'>发射</a></div><div class='meta'>slot="));
    sendChunk(String(i + 1));
    if (hasCode(i)) {
      sendChunk(PSTR("<br>value="));
      sendChunk(String(rfSlots[i].code.value));
      sendChunk(PSTR("<br>bits="));
      sendChunk(String(rfSlots[i].code.bits));
    } else {
      sendChunk(PSTR("<br>空码位"));
    }
    sendChunk(PSTR("<br><a href='/clear?slot="));
    sendChunk(String(i + 1));
    sendChunk(PSTR("'>清除此码位</a></div></article>"));
  }
  sendChunk(PSTR("</section></section>"));
}

void sendIrPanelLite() {
  sendChunk(PSTR("<section class='panel active' data-panel='ir'><section class='card'><h2>空调红外</h2><div class='status'>"));
  sendKv(PSTR("学习状态"), irLearning ? F("等待接收") : (learnedIrReady ? F("已接收，待保存") : F("空闲")));
  sendKv(PSTR("倒计时"), String(irLearning && learnDeadline > millis() ? (learnDeadline - millis() + 999UL) / 1000UL : 0) + F("s"));
  sendKv(PSTR("待保存红外码"), learnedIrReady ? (String(learnedIrLength) + F(" pulses")) : String(F("暂无")));
  sendChunk(PSTR("</div><div class='actions' style='margin-top:12px'><a class='btn' href='/ir-learn'>开始学习 15 秒</a></div></section>"
                 "<section class='card'><h2>保存学习码</h2><form method='get' action='/ir-set'>"
                 "<div class='field'><label>slot 1-6</label><input name='slot' value='1' inputmode='numeric'></div>"
                 "<div class='field span2'><label>名称</label><input name='name' maxlength='16'></div>"
                 "<button class='btn warn' type='submit'>保存</button></form></section>"));
#if ENABLE_IR_AC_PRESETS
  sendChunk(PSTR("<section class='card'><h2>空调预设</h2><form method='get' action='/ir-preset-test'>"
                 "<div class='field'><label>品牌</label><select name='brand'><option value='gree'>格力</option><option value='midea'>美的/华凌/小米</option><option value='daikin'>大金</option><option value='kelon'>科龙/海信</option></select></div>"
                 "<div class='field'><label>电源</label><select name='power'><option value='1'>开机</option><option value='0'>关机</option></select></div>"
                 "<div class='field'><label>模式</label><select name='mode'><option value='1'>制冷</option><option value='2'>制热</option><option value='3'>除湿</option><option value='4'>送风</option><option value='0'>自动</option></select></div>"
                 "<div class='field'><label>温度</label><input name='temp' value='26' inputmode='numeric'></div>"
                 "<button class='btn' type='submit'>测试发送</button></form>"
                 "<form method='get' action='/ir-preset-save' style='margin-top:10px'>"
                 "<div class='field'><label>slot</label><input name='slot' value='1' inputmode='numeric'></div>"
                 "<div class='field'><label>品牌</label><select name='brand'><option value='gree'>格力</option><option value='midea'>美的/华凌/小米</option><option value='daikin'>大金</option><option value='kelon'>科龙/海信</option></select></div>"
                 "<div class='field'><label>电源</label><select name='power'><option value='1'>开机</option><option value='0'>关机</option></select></div>"
                 "<div class='field'><label>模式</label><select name='mode'><option value='1'>制冷</option><option value='2'>制热</option><option value='3'>除湿</option><option value='4'>送风</option><option value='0'>自动</option></select></div>"
                 "<div class='field'><label>温度</label><input name='temp' value='26' inputmode='numeric'></div>"
                 "<button class='btn warn' type='submit'>保存预设</button></form></section>"
                 ));
#else
  sendChunk(PSTR("<section class='card'><h2>空调预设</h2><p class='muted'>当前固件未启用空调预设，以降低 RAM 和固件体积。</p></section>"));
#endif
  sendChunk(PSTR("<section class='grid slot-grid'>"));
  for (uint8_t i = 0; i < IR_SLOT_COUNT; i++) {
    sendChunk(PSTR("<article class='card slot-card'><h3>"));
    sendEscapedChunk(String(irSlots[i].name));
    sendChunk(PSTR("</h3><div class='actions'><a class='btn' href='/ir-send?slot="));
    sendChunk(String(i + 1));
    sendChunk(PSTR("'>发射</a></div><div class='meta'>slot="));
    sendChunk(String(i + 1));
    if (irPresetEnabled(irSlots[i])) sendChunk(PSTR("<br>预设空调"));
    else if (irSlots[i].length > 0) sendChunk(String("<br>raw ") + String(irSlots[i].length) + F(" pulses"));
    else sendChunk(PSTR("<br>空红外码"));
    sendChunk(PSTR("<br><a href='/ir-clear?slot="));
    sendChunk(String(i + 1));
    sendChunk(PSTR("'>清除此码</a></div></article>"));
  }
  sendChunk(PSTR("</section></section>"));
}

void sendLogsPanelLite() {
  sendChunk(PSTR("<section class='panel active' data-panel='logs'><section class='card'><div class='section-head'><h2>运行日志</h2><div class='actions'><button class='btn secondary' type='button' data-refresh-logs>刷新</button><button class='btn warn' type='button' data-clear-logs>清除</button><a class='btn secondary' href='/logs' target='_blank'>纯文本</a></div></div><div class='muted' data-log-status>最近 "));
  sendChunk(String(logLineUsed));
  sendChunk(PSTR(" 条</div><pre class='log-view' data-log-view>"));
  for (uint8_t i = 0; i < logLineUsed; i++) {
    uint8_t index = (logLineNext + LOG_LINE_COUNT - logLineUsed + i) % LOG_LINE_COUNT;
    sendEscapedChunk(String(logLines[index]));
    sendChunk(PSTR("\n"));
  }
  sendChunk(PSTR("</pre></section></section>"));
}

void sendDashboardPageLite() {
  String tab = selectedTab();
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html; charset=utf-8", "");
  sendDashboardStartLite();
  if (tab == F("voice")) sendVoicePanelLite(false);
  else if (tab == F("baidu")) sendVoicePanelLite(true);
  else if (tab == F("rf")) sendRfPanelLite();
  else if (tab == F("ir")) sendIrPanelLite();
  else if (tab == F("logs")) sendLogsPanelLite();
  else sendSystemPanelLite();
  sendDashboardEndLite();
  server.sendContent("");
  Serial.printf("Dashboard streamed tab=%s heap=%u frag=%u\n",
                tab.c_str(),
                ESP.getFreeHeap(),
                ESP.getHeapFragmentation());
}

void handleRoot() {
  Serial.printf("HTTP / tab=%s scan=%s heap=%u frag=%u\n",
                selectedTab().c_str(),
                server.hasArg("scan") ? server.arg("scan").c_str() : "0",
                ESP.getFreeHeap(),
                ESP.getHeapFragmentation());
  sendDashboardPageLite();
}

void handleAppCss() {
  server.send_P(200, "text/css; charset=utf-8", APP_CSS);
}

void handleAppJs() {
  server.send_P(200, "application/javascript; charset=utf-8", APP_JS);
}

void handleLogs() {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/plain; charset=utf-8", "");
  for (uint8_t i = 0; i < logLineUsed; i++) {
    uint8_t index = (logLineNext + LOG_LINE_COUNT - logLineUsed + i) % LOG_LINE_COUNT;
    server.sendContent(logLines[index]);
    server.sendContent("\n");
    yield();
  }
  server.sendContent("");
}

void handleLogsClear() {
  memset(logLines, 0, sizeof(logLines));
  logLineNext = 0;
  logLineUsed = 0;
  server.send(204, "text/plain", "");
}

void handleRfStatus() {
  unsigned long remaining = 0;
  if (rfLearning && learnDeadline > millis()) {
    remaining = (learnDeadline - millis() + 999UL) / 1000UL;
  }

  String json;
  json.reserve(180);
  json += F("{\"learning\":");
  json += rfLearning ? F("true") : F("false");
  json += F(",\"remaining\":");
  json += String(remaining);
  json += F(",\"ready\":");
  json += learnedCodeReady ? F("true") : F("false");
  json += F(",\"value\":");
  json += String(learnedCode.value);
  json += F(",\"bits\":");
  json += String(learnedCode.bits);
  json += F(",\"protocol\":");
  json += String(learnedCode.protocol);
  json += F(",\"pulse\":");
  json += String(learnedCode.pulseLength);
  json += F("}");
  server.send(200, "application/json; charset=utf-8", json);
}

void handleIrStatus() {
  unsigned long remaining = 0;
  if (irLearning && learnDeadline > millis()) {
    remaining = (learnDeadline - millis() + 999UL) / 1000UL;
  }

  String json;
  json.reserve(150);
  json += F("{\"learning\":");
  json += irLearning ? F("true") : F("false");
  json += F(",\"remaining\":");
  json += String(remaining);
  json += F(",\"ready\":");
  json += learnedIrReady ? F("true") : F("false");
  json += F(",\"length\":");
  json += String(learnedIrLength);
  json += F(",\"frequency\":");
  json += String(learnedIrFrequency);
  json += F("}");
  server.send(200, "application/json; charset=utf-8", json);
}

void handleConfig() {
  server.sendHeader("Location", "/#system", true);
  server.send(302, "text/plain", "");
}

void handleVoiceConfig() {
  server.sendHeader("Location", "/#voice", true);
  server.send(302, "text/plain", "");
}

void handleBaiduConfig() {
  server.sendHeader("Location", "/#baidu", true);
  server.send(302, "text/plain", "");
}

void handleReboot() {
  lastMessage = "Reboot requested";
  drawStatus();
  server.send(200, "text/html; charset=utf-8",
              "<!doctype html><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
              "<p>设备正在重启，请等待 10-20 秒后刷新页面。</p>"
              "<p><a href='/'>返回首页</a></p>");
  delay(350);
  ESP.restart();
}

void handleIrLearn() {
  rfLearning = false;
  learnSlot = 0;
  irLearning = true;
  learnedIrReady = false;
  learnedIrLength = 0;
  learnedIrFrequency = IR_DEFAULT_FREQUENCY_KHZ;
  memset(learnedIrRaw, 0, sizeof(learnedIrRaw));
  learnDeadline = millis() + 15000UL;
  irrecv.resume();
  lastMessage = "Learning IR";
  drawStatus();
  server.sendHeader("Location", "/#ir", true);
  server.send(302, "text/plain", "");
}

void handleIrSet() {
  uint8_t slot = parseSlot();
  if (slot == 0) {
    server.send(400, "text/plain; charset=utf-8", "slot must be 1-6");
    return;
  }
  if (!learnedIrReady || learnedIrLength == 0) {
    server.send(400, "text/plain; charset=utf-8", "no learned IR code is ready; click learn first");
    return;
  }

  IrSlot& irSlot = irSlots[slot - 1];
  if (server.hasArg("name")) {
    String name = server.arg("name");
    name.trim();
    if (name.length() > 0) {
      memset(irSlot.name, 0, sizeof(irSlot.name));
      name.toCharArray(irSlot.name, sizeof(irSlot.name));
    }
  }

  irSlot.frequency = learnedIrFrequency;
  if (!setIrSlotRaw(irSlot, learnedIrRaw, learnedIrLength)) {
    server.send(500, "text/plain; charset=utf-8", "not enough memory to save IR code");
    return;
  }
  irSlot.preset = false;
  irSlot.protocol = decode_type_t::UNKNOWN;
  irSlot.model = -1;
  saveIrSlots();

  learnedIrReady = false;
  learnedIrLength = 0;
  lastMessage = "Saved IR " + String(slot);
  drawStatus();
  server.sendHeader("Location", "/#ir", true);
  server.send(302, "text/plain", "");
}

#if ENABLE_IR_AC_PRESETS
void handleIrPresetTest() {
  decode_type_t protocol = parseAcProtocolArg();
  if (protocol == decode_type_t::UNKNOWN) {
    server.send(400, "text/plain; charset=utf-8", "unknown AC brand");
    return;
  }

  bool power = !server.hasArg("power") || server.arg("power").toInt() != 0;
  uint8_t mode = parseBoundedArg("mode", 1, 0, 4);
  uint8_t temp = parseBoundedArg("temp", 26, 16, 30);
  uint8_t fan = parseBoundedArg("fan", 0, 0, 4);
  if (!sendAcPreset(protocol, -1, power, mode, temp, fan, "HTTP test")) {
    server.send(400, "text/plain; charset=utf-8", "AC protocol is not supported by this firmware");
    return;
  }
  server.sendHeader("Location", "/#ir", true);
  server.send(302, "text/plain", "");
}

void handleIrPresetSave() {
  uint8_t slot = parseSlot();
  if (slot == 0) {
    server.send(400, "text/plain; charset=utf-8", "slot must be 1-6");
    return;
  }

  decode_type_t protocol = parseAcProtocolArg();
  if (protocol == decode_type_t::UNKNOWN) {
    server.send(400, "text/plain; charset=utf-8", "unknown AC brand");
    return;
  }

  bool power = !server.hasArg("power") || server.arg("power").toInt() != 0;
  uint8_t mode = parseBoundedArg("mode", 1, 0, 4);
  uint8_t temp = parseBoundedArg("temp", 26, 16, 30);
  uint8_t fan = parseBoundedArg("fan", 0, 0, 4);
  String name = parseAcNameArg(protocol, power, mode, temp);

  IrSlot& irSlot = irSlots[slot - 1];
  memset(irSlot.name, 0, sizeof(irSlot.name));
  name.toCharArray(irSlot.name, sizeof(irSlot.name));
  irSlot.frequency = IR_DEFAULT_FREQUENCY_KHZ;
  releaseIrSlotRaw(irSlot);
  irSlot.preset = true;
  irSlot.protocol = protocol;
  irSlot.model = -1;
  irSlot.power = power;
  irSlot.mode = mode;
  irSlot.temp = temp;
  irSlot.fan = fan;
  saveIrSlots();

  lastMessage = "Saved AC preset " + String(slot);
  drawStatus();
  server.sendHeader("Location", "/#ir", true);
  server.send(302, "text/plain", "");
}
#endif

void handleIrSend() {
  uint8_t slot = parseSlot();
  if (slot == 0) {
    server.send(400, "text/plain; charset=utf-8", "slot must be 1-6");
    return;
  }
  if (!transmitSavedIrSlot(slot, "HTTP")) {
    server.send(400, "text/plain; charset=utf-8", "IR slot is empty");
    return;
  }
  server.sendHeader("Location", "/#ir", true);
  server.send(302, "text/plain", "");
}

void handleIrClear() {
  uint8_t slot = parseSlot();
  if (slot != 0) {
    setDefaultIrSlot(slot - 1);
    saveIrSlots();
    lastMessage = "Cleared IR " + String(slot);
    drawStatus();
    server.sendHeader("Location", "/#ir", true);
    server.send(302, "text/plain", "");
    return;
  }

  initDefaultIrSlots();
  saveIrSlots();
  lastMessage = "IR codes cleared";
  drawStatus();
  server.sendHeader("Location", "/#ir", true);
  server.send(302, "text/plain", "");
}

void handleSend() {
  uint8_t slot = parseSlot();
  if (slot == 0) {
    server.send(400, "text/plain; charset=utf-8", "slot 必须是 1-6");
    return;
  }
  sendCode(slot - 1);
}

void handleLearn() {
  uint8_t slot = parseSlot();
  if (slot == 0) {
    server.send(400, "text/plain; charset=utf-8", "slot 必须是 1-6");
    return;
  }
  learnSlot = slot;
  learnDeadline = millis() + 15000UL;
  lastMessage = "Learning slot " + String(slot);
  drawStatus();
  server.sendHeader("Location", "/#rf", true);
  server.send(302, "text/plain", "");
}

void handleSet() {
  uint8_t slot = parseSlot();
  if (slot == 0 || !server.hasArg("value")) {
    server.send(400, "text/plain; charset=utf-8", "需要 slot 和 value");
    return;
  }

  RfCode& code = rfSlots[slot - 1].code;
  code.value = strtoul(server.arg("value").c_str(), nullptr, 10);
  code.bits = server.hasArg("bits") ? server.arg("bits").toInt() : 24;
  code.protocol = server.hasArg("protocol") ? server.arg("protocol").toInt() : 1;
  code.pulseLength = server.hasArg("pulse") ? server.arg("pulse").toInt() : 350;
  saveRfSlots();

  lastMessage = "Saved slot " + String(slot);
  drawStatus();
  server.sendHeader("Location", "/#rf", true);
  server.send(302, "text/plain", "");
}

void handleTx() {
  if (!server.hasArg("value")) {
    server.send(400, "text/plain; charset=utf-8", "需要 value");
    return;
  }

  uint32_t value = strtoul(server.arg("value").c_str(), nullptr, 10);
  uint8_t bits = server.hasArg("bits") ? server.arg("bits").toInt() : 24;
  uint8_t protocol = server.hasArg("protocol") ? server.arg("protocol").toInt() : 1;
  uint16_t pulse = server.hasArg("pulse") ? server.arg("pulse").toInt() : 350;
  if (value == 0 || bits == 0) {
    server.send(400, "text/plain; charset=utf-8", "value 和 bits 必须有效");
    return;
  }

  uint32_t elapsed = sendRawCode(value, bits, protocol, pulse);
  addLog("RF TX source=Web value=%lu repeat=%u elapsed=%lums",
         static_cast<unsigned long>(value),
         RF_REPEAT_TRANSMIT,
         static_cast<unsigned long>(elapsed));
  lastMessage = "TX test " + String(value);
  drawStatus();
  server.sendHeader("Location", "/#rf", true);
  server.send(302, "text/plain", "");
}

void handleRfSelfTest() {
  rfLearning = false;
  learnSlot = 0;
  rf.resetAvailable();
  delay(10);

  Serial.printf("RF self-test TX: value=%lu bits=%u protocol=%u pulse=%u TX GPIO%u RX GPIO%u\n",
                static_cast<unsigned long>(RF_SELF_TEST_VALUE),
                RF_SELF_TEST_BITS,
                RF_SELF_TEST_PROTOCOL,
                RF_SELF_TEST_PULSE,
                gpioNumber(RF_TX_PIN),
                gpioNumber(rfRxPin));

  sendRawCode(RF_SELF_TEST_VALUE, RF_SELF_TEST_BITS, RF_SELF_TEST_PROTOCOL, RF_SELF_TEST_PULSE);

  bool gotCode = false;
  RfCode received = {0, 0, 0, 0};
  unsigned long start = millis();
  while (millis() - start < RF_SELF_TEST_WAIT_MS) {
    if (rf.available()) {
      received.value = rf.getReceivedValue();
      received.bits = rf.getReceivedBitlength();
      received.protocol = rf.getReceivedProtocol();
      received.pulseLength = rf.getReceivedDelay();
      rf.resetAvailable();
      if (received.value != 0) {
        gotCode = true;
        break;
      }
    }
    delay(5);
    yield();
  }

  if (gotCode) {
    lastReceived = received;
    lastReceivedAt = millis();
    bool matched = received.value == RF_SELF_TEST_VALUE && received.bits == RF_SELF_TEST_BITS;
    lastMessage = matched ? "RF self-test OK" : "RF self-test got other";
    Serial.printf("RF self-test RX: value=%lu bits=%u protocol=%u pulse=%u matched=%u\n",
                  static_cast<unsigned long>(received.value),
                  received.bits,
                  received.protocol,
                  received.pulseLength,
                  matched ? 1 : 0);
  } else {
    lastMessage = "RF self-test no RX";
    Serial.printf("RF self-test failed: no RX within %lu ms\n",
                  static_cast<unsigned long>(RF_SELF_TEST_WAIT_MS));
  }

  drawStatus();
  server.sendHeader("Location", "/#rf", true);
  server.send(302, "text/plain", "");
}

void handleClear() {
  initDefaultRfSlots();
  saveRfSlots();
  for (uint8_t i = 0; i < 3; i++) {
    memset(&settings.codes[i], 0, sizeof(RfCode));
    settings.codes[i].bits = 24;
    settings.codes[i].protocol = 1;
    settings.codes[i].pulseLength = 350;
  }
  saveSettings();
  lastMessage = "Codes cleared";
  drawStatus();
  server.send(200, "text/plain; charset=utf-8", "已清除所有 433 编码，返回首页重新学习");
}

void handleSendV2() {
  uint8_t slot = parseSlot();
  if (slot == 0) {
    server.send(400, "text/plain; charset=utf-8", "slot must be 1-6");
    return;
  }
  sendCode(slot - 1);
}

void handleLearnV2() {
  learnSlot = 0;
  rfLearning = true;
  irLearning = false;
  learnedCodeReady = false;
  learnedCode = {0, 0, 0, 0};
  learnDeadline = millis() + 15000UL;
  lastMessage = "Learning RF";
  drawStatus();
  server.sendHeader("Location", "/#rf", true);
  server.send(302, "text/plain", "");
}

void handleSetV2() {
  uint8_t slot = parseSlot();
  if (slot == 0) {
    server.send(400, "text/plain; charset=utf-8", "slot must be 1-6");
    return;
  }

  RfSlot& rfSlot = rfSlots[slot - 1];
  if (server.hasArg("name")) {
    String name = server.arg("name");
    name.trim();
    if (name.length() > 0) {
      memset(rfSlot.name, 0, sizeof(rfSlot.name));
      name.toCharArray(rfSlot.name, sizeof(rfSlot.name));
    }
  }

  bool hasManualValue = server.hasArg("value") && server.arg("value").length() > 0;
  if (hasManualValue) {
    rfSlot.code.value = strtoul(server.arg("value").c_str(), nullptr, 10);
    rfSlot.code.bits = server.hasArg("bits") ? server.arg("bits").toInt() : 24;
    rfSlot.code.protocol = server.hasArg("protocol") ? server.arg("protocol").toInt() : 1;
    rfSlot.code.pulseLength = server.hasArg("pulse") ? server.arg("pulse").toInt() : 350;
  } else if (learnedCodeReady) {
    rfSlot.code = learnedCode;
    learnedCodeReady = false;
    learnedCode = {0, 0, 0, 0};
  } else {
    server.send(400, "text/plain; charset=utf-8", "no learned code is ready; click learn first or enter value manually");
    return;
  }

  if (rfSlot.code.bits == 0) rfSlot.code.bits = 24;
  if (rfSlot.code.protocol == 0) rfSlot.code.protocol = 1;
  if (rfSlot.code.pulseLength == 0) rfSlot.code.pulseLength = 350;
  saveRfSlots();

  lastMessage = "Saved slot " + String(slot);
  drawStatus();
  server.sendHeader("Location", "/#rf", true);
  server.send(302, "text/plain", "");
}

void handleClearV2() {
  uint8_t slot = parseSlot();
  if (slot != 0) {
    setDefaultRfSlot(slot - 1);
    saveRfSlots();
    lastMessage = "Cleared slot " + String(slot);
    drawStatus();
    server.sendHeader("Location", "/#rf", true);
    server.send(302, "text/plain", "");
    return;
  }

  initDefaultRfSlots();
  saveRfSlots();
  lastMessage = "Codes cleared";
  drawStatus();
  server.sendHeader("Location", "/#rf", true);
  server.send(302, "text/plain", "");
}

void handleVoiceSave() {
  bool baiduRequest = server.uri() == "/baidu";
  if (!server.hasArg("auth")) {
    server.send(400, "text/plain; charset=utf-8", "需要 Blinker 设备密钥");
    return;
  }

  String auth = server.arg("auth");
  auth.trim();
  if (auth.length() > 64) {
    server.send(400, "text/plain; charset=utf-8", "Blinker 设备密钥长度不合法");
    return;
  }

  memset(settings.blinkerAuth, 0, sizeof(settings.blinkerAuth));
  auth.toCharArray(settings.blinkerAuth, sizeof(settings.blinkerAuth));
  if (server.hasArg("repeat")) {
    settings.voiceRfRepeat = parseBoundedArg("repeat",
                                             DEFAULT_VOICE_RF_REPEAT_TRANSMIT,
                                             MIN_VOICE_RF_REPEAT_TRANSMIT,
                                             MAX_VOICE_RF_REPEAT_TRANSMIT);
  } else {
    settings.voiceRfRepeat = normalizedVoiceRfRepeat(settings.voiceRfRepeat);
  }
  saveSettings();
  lastMessage = auth.length() == 0 ? "Voice disabled" : (baiduRequest ? "Baidu auth saved" : "Voice auth saved");
  addLog("Voice config saved auth=%u repeat=%u",
         auth.length() > 0 ? 1 : 0,
         normalizedVoiceRfRepeat(settings.voiceRfRepeat));
  drawStatus();

  String page;
  page.reserve(360);
  page += F("<!doctype html><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>");
  page += F("<p>已保存");
  page += baiduRequest ? F("百度智慧屏") : F("天猫精灵");
  page += F("配置。请重启设备或重新上电，使 Blinker 连接生效。</p><p><a href='/");
  page += baiduRequest ? F("#baidu") : F("#voice");
  page += F("'>返回首页</a></p>");
  server.send(200, "text/html; charset=utf-8", page);
}

void aligeniePowerState(const String& state, uint8_t num) {
  aliGenieParsedCount++;
  uint32_t startedAt = millis();
  uint32_t gap = lastAliGenieCommandAt == 0 ? 0 : startedAt - lastAliGenieCommandAt;
  lastAliGenieCommandAt = startedAt;
  uint32_t commandNumber = ++aliGenieCommandCount;
  BLINKER_LOG("AliGenie outlet: ", num, ", state: ", state);
  addLog("AliGenie RX #%lu num=%u state=%s gap=%lums",
         static_cast<unsigned long>(commandNumber),
         num,
         state.c_str(),
         static_cast<unsigned long>(gap));
  showOledEventLine(4, "AliGenie " + String(num) + " " + state);
  yield();

  bool sent = false;
  if (num >= 1 && num <= VOICE_OUTLET_COUNT) {
    if (num <= 3) {
      sent = transmitSavedSlot(num, "AliGenie");
    } else {
      sent = transmitSavedIrSlot(num - 3, "AliGenie");
    }
    voiceOutletState[num] = (state == BLINKER_CMD_ON);
    BlinkerAliGenie.powerState(voiceOutletState[num] ? "on" : "off", num);
  } else {
    addLog("AliGenie invalid num=%u state=%s", num, state.c_str());
    BlinkerAliGenie.powerState(state, num);
  }
  BlinkerAliGenie.print();
  addLog("AliGenie done #%lu sent=%u elapsed=%lums",
         static_cast<unsigned long>(commandNumber),
         sent ? 1 : 0,
         static_cast<unsigned long>(millis() - startedAt));
}

void aligenieQuery(int32_t queryCode, uint8_t num) {
  aliGenieParsedCount++;
  BLINKER_LOG("AliGenie query outlet: ", num, ", code: ", queryCode);
  addLog("AliGenie query num=%u code=%ld state=%u",
         num,
         static_cast<long>(queryCode),
         (num >= 1 && num <= VOICE_OUTLET_COUNT && voiceOutletState[num]) ? 1 : 0);

  if (num >= 1 && num <= VOICE_OUTLET_COUNT) {
    BlinkerAliGenie.powerState(voiceOutletState[num] ? "on" : "off", num);
  } else {
    BlinkerAliGenie.powerState("off", num);
  }
  BlinkerAliGenie.print();
}

void duerPowerState(const String& state, uint8_t num) {
  BLINKER_LOG("DuerOS outlet: ", num, ", state: ", state);
  addLog("DuerOS power num=%u state=%s", num, state.c_str());

  if (num >= 1 && num <= VOICE_OUTLET_COUNT) {
    if (num <= 3) {
      transmitSavedSlot(num, "DuerOS");
    } else {
      transmitSavedIrSlot(num - 3, "DuerOS");
    }
    voiceOutletState[num] = (state == BLINKER_CMD_ON);
    BlinkerDuerOS.powerState(voiceOutletState[num] ? "on" : "off", num);
  } else {
    addLog("DuerOS invalid num=%u state=%s", num, state.c_str());
    BlinkerDuerOS.powerState(state, num);
  }
  BlinkerDuerOS.print();
}

void duerQuery(int32_t queryCode, uint8_t num) {
  BLINKER_LOG("DuerOS query outlet: ", num, ", code: ", queryCode);
  addLog("DuerOS query num=%u code=%ld state=%u",
         num,
         static_cast<long>(queryCode),
         (num >= 1 && num <= VOICE_OUTLET_COUNT && voiceOutletState[num]) ? 1 : 0);

  if (num >= 1 && num <= VOICE_OUTLET_COUNT) {
    BlinkerDuerOS.powerState(voiceOutletState[num] ? "on" : "off", num);
  } else {
    BlinkerDuerOS.powerState("off", num);
  }
  BlinkerDuerOS.print();
}

void blinkerDataRead(const String& data) {
  BLINKER_LOG("Blinker data: ", data);
  addLog("Blinker data %s", data.c_str());
}

void setupVoice() {
  if (!wifiConnected() || !hasBlinkerAuth()) {
    addLog("Voice setup skipped wifi=%u auth=%u", wifiConnected() ? 1 : 0, hasBlinkerAuth() ? 1 : 0);
    return;
  }

  Blinker.begin(settings.blinkerAuth, settings.wifiSsid, settings.wifiPassword);
  Blinker.attachData(blinkerDataRead);
  BlinkerAliGenie.attachPowerState(aligeniePowerState);
  BlinkerAliGenie.attachQuery(aligenieQuery);
  BlinkerDuerOS.attachPowerState(duerPowerState);
  BlinkerDuerOS.attachQuery(duerQuery);
  voiceStarted = true;
  lastMessage = "Voice enabled: AliGenie + DuerOS";
  addLog("Voice setup started ssid=%s", settings.wifiSsid);
  drawStatus();
}

bool connectToSavedWifi(unsigned long timeoutMs) {
  if (!hasWifiCredentials()) return false;

  WiFi.mode(WIFI_STA);
  WiFi.begin(settings.wifiSsid, settings.wifiPassword);
  lastMessage = "Connecting WiFi";
  drawStatus();

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(300);
  }

  if (wifiConnected()) {
    lastMessage = "WiFi connected";
    return true;
  }

  WiFi.disconnect();
  return false;
}

void startSetupAp() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  lastMessage = "Setup AP";
  drawStatus();
}

void handleWifiSave() {
  if (!server.hasArg("ssid")) {
    server.send(400, "text/plain; charset=utf-8", "需要选择 SSID");
    return;
  }

  String ssid = server.arg("ssid");
  String password = server.hasArg("password") ? server.arg("password") : "";
  ssid.trim();
  password.trim();
  if (ssid.length() == 0 || ssid.length() > 32 || password.length() > 64) {
    server.send(400, "text/plain; charset=utf-8", "SSID 或密码长度不合法");
    return;
  }

  memset(settings.wifiSsid, 0, sizeof(settings.wifiSsid));
  memset(settings.wifiPassword, 0, sizeof(settings.wifiPassword));
  ssid.toCharArray(settings.wifiSsid, sizeof(settings.wifiSsid));
  password.toCharArray(settings.wifiPassword, sizeof(settings.wifiPassword));
  saveSettings();

  server.send(200, "text/html; charset=utf-8",
              "<!doctype html><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
              "<p>已保存 Wi-Fi，模块正在连接。请查看 OLED 上显示的 SSID 和 IP 地址，然后用浏览器访问新的 IP。</p>");

  delay(500);
  if (connectToSavedWifi(WIFI_JOIN_TIMEOUT_MS)) {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    lastMessage = "WiFi connected";
    updateWorldTimeIfNeeded();
  } else {
    startSetupAp();
    lastMessage = "WiFi failed";
  }
  drawStatus();
}

void setupWifi() {
  if (!connectToSavedWifi(WIFI_JOIN_TIMEOUT_MS)) {
    startSetupAp();
  } else {
    updateWorldTimeIfNeeded();
  }
}

void setup() {
  Serial.begin(115200);
  delay(800);
  addLog("Boot start chip=%06X", ESP.getChipId());

  setupOled();

  loadSettings();
  setupLittleFs();
  rf.enableTransmit(RF_TX_PIN);
  rf.enableReceive(digitalPinToInterrupt(rfRxPin));
  irsend.begin();
  irrecv.enableIRIn();

  setupWifi();
  setupVoice();
  server.on("/", handleRoot);
  server.on("/app.css", handleAppCss);
  server.on("/app.js", handleAppJs);
  server.on("/logs", handleLogs);
  server.on("/logs/clear", HTTP_POST, handleLogsClear);
  server.on("/rf-status", handleRfStatus);
  server.on("/ir-status", handleIrStatus);
  server.on("/config", handleConfig);
  server.on("/voice-config", handleVoiceConfig);
  server.on("/baidu-config", handleBaiduConfig);
  server.on("/reboot", handleReboot);
  server.on("/wifi", HTTP_POST, handleWifiSave);
  server.on("/voice", HTTP_POST, handleVoiceSave);
  server.on("/baidu", HTTP_POST, handleVoiceSave);
  server.on("/send", handleSendV2);
  server.on("/learn", handleLearnV2);
  server.on("/set", handleSetV2);
  server.on("/tx", handleTx);
  server.on("/rf-self-test", handleRfSelfTest);
  server.on("/clear", handleClearV2);
  server.on("/ir-learn", handleIrLearn);
  server.on("/ir-set", handleIrSet);
#if ENABLE_IR_AC_PRESETS
  server.on("/ir-preset-test", handleIrPresetTest);
  server.on("/ir-preset-save", handleIrPresetSave);
#endif
  server.on("/ir-send", handleIrSend);
  server.on("/ir-clear", handleIrClear);
  httpUpdater.setup(&server, "/update", "admin", AP_PASSWORD);
  server.begin();
  addLog("HTTP server started ip=%s voice=%u rfRepeat=%u voiceRfRepeat=%u",
         deviceIP().toString().c_str(),
         voiceStarted ? 1 : 0,
         RF_REPEAT_TRANSMIT,
         normalizedVoiceRfRepeat(settings.voiceRfRepeat));

  drawStatus();
}

void updateOledWifiStatusIfChanged() {
  if (!oledReady) return;

  wl_status_t currentStatus = WiFi.status();
  IPAddress currentIp = deviceIP();
  if (currentStatus != lastOledWifiStatus || currentIp != lastOledIp) {
    drawStatus();
  }
}

void loop() {
  server.handleClient();
  if (voiceStarted) {
    Blinker.run();
  }

  updateOledWifiStatusIfChanged();
  updateWorldTimeIfNeeded();

  if (millis() - lastOledTitleRefresh >= OLED_TITLE_REFRESH_MS) {
    drawTitle();
  }

  if (wifiConnected() && millis() - lastOledSystemRefresh >= OLED_STATUS_REFRESH_MS) {
    drawSystemStatus();
  }

  if (rfLearning && millis() > learnDeadline) {
    rfLearning = false;
    lastMessage = learnedCodeReady ? "RF learned" : "Learn timeout";
    drawStatus();
  }

  if (irLearning && millis() > learnDeadline) {
    irLearning = false;
    lastMessage = learnedIrReady ? "IR learned" : "IR learn timeout";
    drawStatus();
  }

  if (learnSlot != 0 && millis() > learnDeadline) {
    learnSlot = 0;
    lastMessage = "Learn timeout";
    drawStatus();
  }

  if (rf.available()) {
    RfCode received;
    received.value = rf.getReceivedValue();
    received.bits = rf.getReceivedBitlength();
    received.protocol = rf.getReceivedProtocol();
    received.pulseLength = rf.getReceivedDelay();

    if (received.value != 0) {
      lastReceived = received;
      lastReceivedAt = millis();
      lastMessage = "RX " + String(received.value);
      Serial.printf("Received: value=%lu bits=%u protocol=%u pulse=%u\n",
                    static_cast<unsigned long>(received.value), received.bits,
                    received.protocol, received.pulseLength);

      if (rfLearning) {
        learnedCode = received;
        learnedCodeReady = true;
        rfLearning = false;
        lastMessage = "RF learned " + String(received.value);
      } else if (learnSlot != 0) {
        rfSlots[learnSlot - 1].code = received;
        saveRfSlots();
        lastMessage = "Learned slot " + String(learnSlot);
        learnSlot = 0;
      }
      drawStatus();
    }
    rf.resetAvailable();
  }

  if (irrecv.decode(&irResults)) {
    uint16_t rawLength = getCorrectedRawLength(&irResults);
    Serial.printf("IR RX: type=%s bits=%u rawLen=%u overflow=%u\n",
                  typeToString(irResults.decode_type, false).c_str(),
                  irResults.bits,
                  rawLength,
                  irResults.overflow ? 1 : 0);

    if (irLearning) {
      uint16_t copyLength = rawLength;
      if (copyLength > IR_RAW_MAX) {
        copyLength = IR_RAW_MAX;
        Serial.printf("IR RX truncated: rawLen=%u max=%u\n", rawLength, IR_RAW_MAX);
      }

      uint16_t* rawArray = resultToRawArray(&irResults);
      if (rawArray != nullptr && copyLength > 0) {
        memcpy(learnedIrRaw, rawArray, copyLength * sizeof(uint16_t));
        learnedIrLength = copyLength;
        learnedIrFrequency = IR_DEFAULT_FREQUENCY_KHZ;
        learnedIrReady = true;
        irLearning = false;
        lastMessage = "IR learned " + String(copyLength);
        drawStatus();
      }
      delete[] rawArray;
    }

    irrecv.resume();
  }
}
