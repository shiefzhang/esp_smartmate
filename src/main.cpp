#include <Arduino.h>
#include <EEPROM.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266HTTPUpdateServer.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <LittleFS.h>
#include <time.h>
#define BLINKER_PRINT Serial
#define BLINKER_WIFI
#define BLINKER_ALIGENIE_MULTI_OUTLET
#include <Blinker.h>
#include <RCSwitch.h>

// HW-364A 板载 OLED 通常使用软件 I2C：clock=D6(GPIO12), data=D5(GPIO14)。
constexpr uint8_t RF_TX_PIN = D7;       // GPIO13
constexpr uint8_t RF_RX_BUILTIN_OLED_PIN = D2;  // GPIO4，板载 OLED 不占用 D2
constexpr uint8_t OLED_CLOCK_PIN = D6;  // GPIO12
constexpr uint8_t OLED_DATA_PIN = D5;   // GPIO14

constexpr uint16_t EEPROM_SIZE = 512;
constexpr uint32_t SETTINGS_MAGIC = 0x4330A11C;
constexpr uint32_t OLD_SETTINGS_MAGIC = 0x4330A11B;
constexpr uint8_t RF_REPEAT_TRANSMIT = 8;
constexpr uint16_t SCREEN_WIDTH = 128;
constexpr uint16_t SCREEN_HEIGHT = 64;
constexpr unsigned long WIFI_JOIN_TIMEOUT_MS = 15000UL;
constexpr unsigned long OLED_STATUS_REFRESH_MS = 15000UL;
constexpr unsigned long OLED_TITLE_REFRESH_MS = 60000UL;
constexpr unsigned long TIME_SYNC_INTERVAL_MS = 3600000UL;
constexpr unsigned long TIME_SYNC_RETRY_MS = 60000UL;
constexpr uint8_t VOICE_OUTLET_COUNT = 3;
constexpr uint8_t RF_SLOT_COUNT = 12;

const char* AP_SSID = "ESP8266-433-Setup";
const char* AP_PASSWORD = "433remote";
const char* TIME_SYNC_URL = "http://worldtimeapi.org/api/timezone/Asia/Shanghai";
const char* NTP_SERVER_1 = "ntp.aliyun.com";
const char* NTP_SERVER_2 = "pool.ntp.org";
const char* NTP_SERVER_3 = "time.windows.com";
const char* RF_CODES_PATH = "/rf_codes.txt";

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

Settings settings;
RfSlot rfSlots[RF_SLOT_COUNT];
RCSwitch rf = RCSwitch();
ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;

String lastMessage = "Booting";
RfCode lastReceived = {0, 0, 0, 0};
unsigned long lastReceivedAt = 0;
uint8_t learnSlot = 0;
unsigned long learnDeadline = 0;
unsigned long lastOledSystemRefresh = 0;
unsigned long lastOledTitleRefresh = 0;
unsigned long lastTimeSyncAttempt = 0;
unsigned long lastTimeSyncAt = 0;
uint32_t syncedLocalEpoch = 0;
bool voiceStarted = false;
bool voiceOutletState[VOICE_OUTLET_COUNT + 1] = {false};
bool timeSynced = false;
bool oledReady = false;
bool fsReady = false;
uint8_t rfRxPin = RF_RX_BUILTIN_OLED_PIN;
uint8_t oledSdaPin = OLED_DATA_PIN;
uint8_t oledSclPin = OLED_CLOCK_PIN;
uint8_t oledAddress = 0x3C;

void drawStatus();

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

bool hasWifiCredentials() {
  return settings.wifiSsid[0] != '\0';
}

bool hasBlinkerAuth() {
  return settings.blinkerAuth[0] != '\0';
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
  pinMode(OLED_DATA_PIN, INPUT_PULLUP);
}

void oledSdaLow() {
  pinMode(OLED_DATA_PIN, OUTPUT);
  digitalWrite(OLED_DATA_PIN, LOW);
}

void oledSclHigh() {
  pinMode(OLED_CLOCK_PIN, INPUT_PULLUP);
}

void oledSclLow() {
  pinMode(OLED_CLOCK_PIN, OUTPUT);
  digitalWrite(OLED_CLOCK_PIN, LOW);
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

void oledWriteByte(uint8_t value) {
  for (uint8_t i = 0; i < 8; i++) {
    if (value & 0x80) oledSdaHigh();
    else oledSdaLow();
    oledDelay();
    oledSclHigh();
    oledDelay();
    oledSclLow();
    value <<= 1;
  }

  // Ignore ACK so HW-364A boards with weak pullups still receive display data.
  oledSdaHigh();
  oledDelay();
  oledSclHigh();
  oledDelay();
  oledSclLow();
}

void oledCommand(uint8_t command) {
  oledStart();
  oledWriteByte(0x78);
  oledWriteByte(0x00);
  oledWriteByte(command);
  oledStop();
}

void oledData(uint8_t data) {
  oledStart();
  oledWriteByte(0x78);
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
  Serial.printf("OLED title page 0: '%s' len=%u synced=%u\n",
                title.c_str(), title.length(), timeSynced ? 1 : 0);
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

  drawTitle();

  for (uint8_t clearPage = 1; clearPage <= 5; clearPage++) {
    oledClearPage(clearPage);
  }

  uint8_t page = 2;

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

void setupOled() {
  Serial.printf("Starting HW-364A OLED via SSD1306 SW I2C: clock GPIO%u, data GPIO%u\n",
                gpioNumber(OLED_CLOCK_PIN), gpioNumber(OLED_DATA_PIN));
  oledBegin();
  oledReady = true;
  oledSdaPin = OLED_DATA_PIN;
  oledSclPin = OLED_CLOCK_PIN;
  oledAddress = 0x3C;
  rfRxPin = RF_RX_BUILTIN_OLED_PIN;
  oledDrawTextLine(0, "STARTING...");
}

void saveSettings() {
  EEPROM.put(0, settings);
  EEPROM.commit();
}

void initDefaultSettings() {
  memset(&settings, 0, sizeof(settings));
  settings.magic = SETTINGS_MAGIC;
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

void setupLittleFs() {
  fsReady = LittleFS.begin();
  if (!fsReady) {
    Serial.println("LittleFS mount failed, formatting");
    LittleFS.format();
    fsReady = LittleFS.begin();
  }
  Serial.printf("LittleFS: %s\n", fsReady ? "mounted" : "mount failed");
  loadRfSlots();
}

void loadSettings() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(0, settings);
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
  }
}

bool hasCode(uint8_t index) {
  return index < RF_SLOT_COUNT && rfSlots[index].code.value != 0 && rfSlots[index].code.bits != 0;
}

void sendRawCode(uint32_t value, uint8_t bits, uint8_t protocol, uint16_t pulseLength) {
  rf.setProtocol(protocol);
  rf.setPulseLength(pulseLength);
  rf.setRepeatTransmit(RF_REPEAT_TRANSMIT);
  rf.send(value, bits);
}

bool transmitSavedSlot(uint8_t slot, const String& source) {
  if (slot < 1 || slot > RF_SLOT_COUNT || !hasCode(slot - 1)) {
    lastMessage = source + " slot " + String(slot) + " empty";
    drawStatus();
    return false;
  }

  const RfCode& code = rfSlots[slot - 1].code;
  sendRawCode(code.value, code.bits, code.protocol, code.pulseLength);
  lastMessage = source + " slot " + String(slot);
  drawStatus();
  return true;
}

void sendCode(uint8_t index) {
  if (!transmitSavedSlot(index + 1, "HTTP")) {
    server.send(400, "text/plain; charset=utf-8", "该灯位还没有学习/录入 433 编码");
    return;
  }
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

void appendPageStart(String& page, const String& title) {
  page += F("<!doctype html><html lang='zh-CN'><head><meta charset='utf-8'>");
  page += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
  page += F("<title>");
  page += title;
  page += F("</title><style>");
  page += F(":root{font-family:system-ui,-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;color:#172033;background:#f4f6fa}");
  page += F("body{margin:0}.wrap{max-width:900px;margin:0 auto;padding:20px}.top{display:flex;justify-content:space-between;gap:14px;align-items:flex-start;margin-bottom:16px}");
  page += F("h1{font-size:24px;margin:0 0 6px}h2{font-size:18px;margin:0 0 12px}.muted{color:#667085;font-size:14px}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(210px,1fr));gap:12px}");
  page += F(".card{background:#fff;border:1px solid #e5e7ef;border-radius:8px;padding:16px;box-shadow:0 1px 2px rgba(16,24,40,.04)}");
  page += F(".btn{display:inline-flex;align-items:center;justify-content:center;box-sizing:border-box;width:100%;min-height:42px;border:0;border-radius:8px;background:#2563eb;color:#fff;font-weight:700;font-size:15px;text-decoration:none;padding:0 12px}");
  page += F(".btn.secondary{background:#eef2ff;color:#273a8a}.btn.warn{background:#fff7ed;color:#9a3412;border:1px solid #fed7aa}.meta{font-size:13px;color:#667085;margin-top:10px;word-break:break-all;line-height:1.5}");
  page += F("form{display:grid;grid-template-columns:repeat(5,1fr);gap:8px;align-items:end}.field label{display:block;font-size:12px;color:#667085;margin-bottom:4px}");
  page += F("input,select{box-sizing:border-box;width:100%;height:38px;border:1px solid #d0d5dd;border-radius:6px;padding:0 9px;font-size:14px;background:#fff}.span2{grid-column:span 2}");
  page += F(".status{display:grid;grid-template-columns:repeat(auto-fit,minmax(160px,1fr));gap:8px}.kv{background:#f8fafc;border-radius:6px;padding:10px}.kv b{display:block;font-size:12px;color:#667085;margin-bottom:4px}");
  page += F(".inline-actions{display:flex;gap:8px;flex-wrap:wrap}.inline-actions .btn{width:auto;min-width:110px}");
  page += F("@media(max-width:700px){.top{display:block}form{grid-template-columns:1fr 1fr}.span2{grid-column:1/-1}}");
  page += F("</style></head><body><main class='wrap'>");
}

void appendPageEnd(String& page) {
  page += F("</main></body></html>");
}

void appendWifiForm(String& page) {
  page += F("<section class='card' style='margin-bottom:14px'><h2>网络设置</h2>");
  page += F("<form method='post' action='/wifi'>");
  page += F("<div class='field span2'><label>选择 Wi-Fi</label><select name='ssid'>");

  int networkCount = WiFi.scanNetworks();
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

  page += F("</select></div>");
  page += F("<div class='field span2'><label>密码</label><input name='password' type='password' maxlength='64' placeholder='留空表示无密码'></div>");
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
    page += F("<p class='muted inline-actions'><a class='btn secondary' href='/config'>重新配置 Wi-Fi</a><a class='btn secondary' href='/voice-config'>配置天猫精灵</a></p>");
  }
  page += F("</section>");

  page += F("<section class='grid'>");
  for (uint8_t i = 0; i < 3; i++) {
    const RfCode& code = settings.codes[i];
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
  page += F("<div class='field'><label>码位 1-3</label><input name='slot' value='1' inputmode='numeric'></div>");
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
  page += F("<div class='field'><label>slot 1-12</label><input name='slot' value='1' inputmode='numeric'></div>");
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

void handleRoot() {
  server.send(200, "text/html; charset=utf-8", renderMainPage());
}

void handleConfig() {
  server.send(200, "text/html; charset=utf-8", renderConfigPage());
}

void handleVoiceConfig() {
  server.send(200, "text/html; charset=utf-8", renderVoiceConfigPage());
}

void handleSend() {
  uint8_t slot = parseSlot();
  if (slot == 0) {
    server.send(400, "text/plain; charset=utf-8", "slot 必须是 1-3");
    return;
  }
  sendCode(slot - 1);
}

void handleLearn() {
  uint8_t slot = parseSlot();
  if (slot == 0) {
    server.send(400, "text/plain; charset=utf-8", "slot 必须是 1-3");
    return;
  }
  learnSlot = slot;
  learnDeadline = millis() + 15000UL;
  lastMessage = "Learning slot " + String(slot);
  drawStatus();
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

void handleSet() {
  uint8_t slot = parseSlot();
  if (slot == 0 || !server.hasArg("value")) {
    server.send(400, "text/plain; charset=utf-8", "需要 slot 和 value");
    return;
  }

  RfCode& code = settings.codes[slot - 1];
  code.value = strtoul(server.arg("value").c_str(), nullptr, 10);
  code.bits = server.hasArg("bits") ? server.arg("bits").toInt() : 24;
  code.protocol = server.hasArg("protocol") ? server.arg("protocol").toInt() : 1;
  code.pulseLength = server.hasArg("pulse") ? server.arg("pulse").toInt() : 350;
  saveSettings();

  lastMessage = "Saved slot " + String(slot);
  drawStatus();
  server.sendHeader("Location", "/", true);
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

  sendRawCode(value, bits, protocol, pulse);
  lastMessage = "TX test " + String(value);
  drawStatus();
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

void handleClear() {
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
    server.send(400, "text/plain; charset=utf-8", "slot must be 1-12");
    return;
  }
  sendCode(slot - 1);
}

void handleLearnV2() {
  uint8_t slot = parseSlot();
  if (slot == 0) {
    server.send(400, "text/plain; charset=utf-8", "slot must be 1-12");
    return;
  }
  learnSlot = slot;
  learnDeadline = millis() + 15000UL;
  lastMessage = "Learning slot " + String(slot);
  drawStatus();
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

void handleSetV2() {
  uint8_t slot = parseSlot();
  if (slot == 0 || !server.hasArg("value")) {
    server.send(400, "text/plain; charset=utf-8", "slot and value are required");
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

  rfSlot.code.value = strtoul(server.arg("value").c_str(), nullptr, 10);
  rfSlot.code.bits = server.hasArg("bits") ? server.arg("bits").toInt() : 24;
  rfSlot.code.protocol = server.hasArg("protocol") ? server.arg("protocol").toInt() : 1;
  rfSlot.code.pulseLength = server.hasArg("pulse") ? server.arg("pulse").toInt() : 350;
  if (rfSlot.code.bits == 0) rfSlot.code.bits = 24;
  if (rfSlot.code.protocol == 0) rfSlot.code.protocol = 1;
  if (rfSlot.code.pulseLength == 0) rfSlot.code.pulseLength = 350;
  saveRfSlots();

  lastMessage = "Saved slot " + String(slot);
  drawStatus();
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

void handleClearV2() {
  uint8_t slot = parseSlot();
  if (slot != 0) {
    setDefaultRfSlot(slot - 1);
    saveRfSlots();
    lastMessage = "Cleared slot " + String(slot);
    drawStatus();
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "");
    return;
  }

  initDefaultRfSlots();
  saveRfSlots();
  lastMessage = "Codes cleared";
  drawStatus();
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

void handleVoiceSave() {
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
  saveSettings();
  lastMessage = auth.length() == 0 ? "Voice disabled" : "Voice auth saved";
  drawStatus();

  server.send(200, "text/html; charset=utf-8",
              "<!doctype html><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
              "<p>已保存天猫精灵配置。请重启设备或重新上电，使 Blinker 连接生效。</p>"
              "<p><a href='/'>返回首页</a></p>");
}

void aligeniePowerState(const String& state, uint8_t num) {
  BLINKER_LOG("AliGenie outlet: ", num, ", state: ", state);

  if (num >= 1 && num <= VOICE_OUTLET_COUNT) {
    transmitSavedSlot(num, "Voice");
    voiceOutletState[num] = (state == BLINKER_CMD_ON);
    BlinkerAliGenie.powerState(voiceOutletState[num] ? "on" : "off", num);
  } else {
    BlinkerAliGenie.powerState(state, num);
  }
  BlinkerAliGenie.print();
}

void aligenieQuery(int32_t queryCode, uint8_t num) {
  BLINKER_LOG("AliGenie query outlet: ", num, ", code: ", queryCode);

  if (num >= 1 && num <= VOICE_OUTLET_COUNT) {
    BlinkerAliGenie.powerState(voiceOutletState[num] ? "on" : "off", num);
  } else {
    BlinkerAliGenie.powerState("off", num);
  }
  BlinkerAliGenie.print();
}

void blinkerDataRead(const String& data) {
  BLINKER_LOG("Blinker data: ", data);
}

void setupVoice() {
  if (!wifiConnected() || !hasBlinkerAuth()) return;

  Blinker.begin(settings.blinkerAuth, settings.wifiSsid, settings.wifiPassword);
  Blinker.attachData(blinkerDataRead);
  BlinkerAliGenie.attachPowerState(aligeniePowerState);
  BlinkerAliGenie.attachQuery(aligenieQuery);
  voiceStarted = true;
  lastMessage = "Voice enabled";
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
  delay(200);

  setupOled();

  loadSettings();
  setupLittleFs();
  rf.enableTransmit(RF_TX_PIN);
  rf.enableReceive(digitalPinToInterrupt(rfRxPin));

  setupWifi();
  setupVoice();
  server.on("/", handleRoot);
  server.on("/config", handleConfig);
  server.on("/voice-config", handleVoiceConfig);
  server.on("/wifi", HTTP_POST, handleWifiSave);
  server.on("/voice", HTTP_POST, handleVoiceSave);
  server.on("/send", handleSendV2);
  server.on("/learn", handleLearnV2);
  server.on("/set", handleSetV2);
  server.on("/tx", handleTx);
  server.on("/clear", handleClearV2);
  httpUpdater.setup(&server, "/update", "admin", AP_PASSWORD);
  server.begin();

  drawStatus();
}

void loop() {
  server.handleClient();
  if (voiceStarted) {
    Blinker.run();
  }

  updateWorldTimeIfNeeded();

  if (millis() - lastOledTitleRefresh >= OLED_TITLE_REFRESH_MS) {
    drawTitle();
  }

  if (wifiConnected() && millis() - lastOledSystemRefresh >= OLED_STATUS_REFRESH_MS) {
    drawSystemStatus();
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

      if (learnSlot != 0) {
        rfSlots[learnSlot - 1].code = received;
        saveRfSlots();
        lastMessage = "Learned slot " + String(learnSlot);
        learnSlot = 0;
      }
      drawStatus();
    }
    rf.resetAvailable();
  }
}
