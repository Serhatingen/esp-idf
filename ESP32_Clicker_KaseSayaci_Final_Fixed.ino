#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <LittleFS.h>
#include <RTClib.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_NeoPixel.h>

#define I2C_SDA 7
#define I2C_SCL 6
#define BUTTON_PIN 3
#define LED_PIN 4
#define LED_COUNT 4
#define SPEAKER_PIN 5
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_ADDR 0x3C
#define AUDIO_CHANNEL 0
#define AUDIO_RES_BITS 8

// Normal takvim istediginiz icin 05:00 kesimi kapatildi.
const int CLUB_DAY_CUTOFF_HOUR = 0;
const unsigned long BUTTON_DEBOUNCE_MS = 18;
const unsigned long LONG_PRESS_MS = 3000;
const unsigned long DEBUG_PRESS_MS = 10000;
const unsigned long INFO_SCREEN_MS = 1600;
const unsigned long DEBUG_SCREEN_MS = 5000;
const unsigned long PORTAL_AUTO_OFF_MS = 120000;
const unsigned long STARTUP_LOADING_MS = 3000;
const unsigned long HEARTBEAT_IDLE_MS = 3000;
const unsigned long HEARTBEAT_CONNECTED_MS = 1000;
const unsigned long CLIENT_CHECK_MS = 500;

RTC_DS3231 rtc;
WebServer web(80);
DNSServer dns;
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
Adafruit_NeoPixel pixels(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

String deviceId;
String apName;

bool fsOk = false;
bool rtcOk = false;
bool oledOk = false;
bool apActive = false;

unsigned long portalLastActivityAt = 0;
unsigned long infoUntilMs = 0;
unsigned long debugUntilMs = 0;
unsigned long invertUntilMs = 0;
unsigned long lastHeartbeatMs = 0;
unsigned long lastClientCheckMs = 0;

bool portalClientConnected = false;
bool lastPortalClientConnected = false;

uint32_t dailyEntryCount = 0;
String currentBusinessDate = "";
String currentBusinessMonth = "";
uint32_t lastShownEntryCount = 0;

enum ScreenMode {
  SCREEN_NONE,
  SCREEN_LOADING,
  SCREEN_OK,
  SCREEN_PORTAL,
  SCREEN_DEBUG
};
ScreenMode currentScreen = SCREEN_NONE;

String formatDate(const DateTime& dt) {
  char buf[11];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d", dt.year(), dt.month(), dt.day());
  return String(buf);
}

String formatMonth(const DateTime& dt) {
  char buf[8];
  snprintf(buf, sizeof(buf), "%04d-%02d", dt.year(), dt.month());
  return String(buf);
}

String formatTime(const DateTime& dt) {
  char buf[9];
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d", dt.hour(), dt.minute(), dt.second());
  return String(buf);
}

String formatDateTime(const DateTime& dt) {
  return formatDate(dt) + " " + formatTime(dt);
}

String dayNameTr(uint8_t dow) {
  switch (dow) {
    case 0: return "Pazar";
    case 1: return "Pazartesi";
    case 2: return "Sali";
    case 3: return "Carsamba";
    case 4: return "Persembe";
    case 5: return "Cuma";
    case 6: return "Cumartesi";
    default: return "?";
  }
}

String csvEscape(String s) {
  s.replace("\"", "\"\"");
  return "\"" + s + "\"";
}

String htmlEscape(String s) {
  s.replace("&", "&amp;");
  s.replace("<", "&lt;");
  s.replace(">", "&gt;");
  s.replace("\"", "&quot;");
  s.replace("'", "&#39;");
  return s;
}

bool isRtcTimeValid(const DateTime& t) {
  if (t.year() < 2024 || t.year() > 2099) return false;
  if (t.month() < 1 || t.month() > 12) return false;
  if (t.day() < 1 || t.day() > 31) return false;
  if (t.hour() > 23) return false;
  if (t.minute() > 59) return false;
  if (t.second() > 59) return false;
  return true;
}

bool rtcTimeReady() {
  if (!rtcOk) return false;
  return isRtcTimeValid(rtc.now());
}

DateTime getBusinessDateTime(const DateTime& nowLocal) {
  // Normal takvim: is gunu kaydirma yok.
  return nowLocal;
}

int getBusinessHour(const DateTime& nowLocal) {
  return nowLocal.hour();
}

String getDailyLogPathForNow(const DateTime& nowLocal) {
  DateTime business = getBusinessDateTime(nowLocal);
  return "/day_" + formatDate(business) + ".csv";
}

String getMonthlySummaryPathForNow(const DateTime& nowLocal) {
  DateTime business = getBusinessDateTime(nowLocal);
  return "/month_" + formatMonth(business) + "_summary.csv";
}

String getMonthFromDailyPath(const String& path) {
  if (!path.startsWith("/day_") || path.length() < 15) return "";
  return path.substring(5, 12);
}

String getDateFromDailyPath(const String& path) {
  if (!path.startsWith("/day_") || path.length() < 15) return "";
  return path.substring(5, 15);
}

String normalizePath(String path) {
  path.trim();
  if (!path.startsWith("/")) path = "/" + path;
  if (path.startsWith("/day_") && path.endsWith(".csv")) return path;
  if (path.startsWith("/month_") && path.endsWith("_summary.csv")) return path;
  return "";
}

void clearPixels() {
  for (int i = 0; i < LED_COUNT; i++) pixels.setPixelColor(i, 0);
  pixels.show();
}

void setAllPixels(uint8_t r, uint8_t g, uint8_t b) {
  for (int i = 0; i < LED_COUNT; i++) pixels.setPixelColor(i, pixels.Color(r, g, b));
  pixels.show();
}

void startupLedAnimation() {
  clearPixels();
  pixels.setPixelColor(1, pixels.Color(0, 0, 10));
  pixels.setPixelColor(2, pixels.Color(0, 0, 10));
  pixels.show();
  delay(60);
  clearPixels();
  pixels.setPixelColor(0, pixels.Color(0, 0, 6));
  pixels.setPixelColor(1, pixels.Color(0, 10, 18));
  pixels.setPixelColor(2, pixels.Color(0, 10, 18));
  pixels.setPixelColor(3, pixels.Color(0, 0, 6));
  pixels.show();
  delay(70);
  clearPixels();
}

void clickLedAnimation() {
  clearPixels();
  pixels.setPixelColor(0, pixels.Color(0, 10, 0));
  pixels.show();
  delay(10);
  pixels.setPixelColor(1, pixels.Color(0, 18, 0));
  pixels.show();
  delay(10);
  pixels.setPixelColor(2, pixels.Color(0, 24, 0));
  pixels.show();
  delay(10);
  pixels.setPixelColor(3, pixels.Color(8, 32, 8));
  pixels.show();
  delay(18);
  clearPixels();
}

void errorLedAnimation() {
  setAllPixels(24, 0, 0);
  delay(45);
  clearPixels();
  delay(25);
  setAllPixels(12, 0, 0);
  delay(35);
  clearPixels();
}

void portalOpenLedAnimation() {
  clearPixels();
  pixels.setPixelColor(0, pixels.Color(0, 0, 8));
  pixels.setPixelColor(3, pixels.Color(0, 0, 8));
  pixels.show();
  delay(35);
  pixels.setPixelColor(1, pixels.Color(0, 8, 18));
  pixels.setPixelColor(2, pixels.Color(0, 8, 18));
  pixels.show();
  delay(45);
  clearPixels();
}

void portalIdleLedBreath() {
  static unsigned long lastStep = 0;
  static int level = 1;
  static int delta = 1;

  if (millis() - lastStep < 80) return;

  lastStep = millis();
  level += delta;
  if (level >= 10) delta = -1;
  if (level <= 1) delta = 1;

  pixels.setPixelColor(0, pixels.Color(0, 0, level));
  pixels.setPixelColor(1, pixels.Color(0, 0, level + 2));
  pixels.setPixelColor(2, pixels.Color(0, 0, level + 2));
  pixels.setPixelColor(3, pixels.Color(0, 0, level));
  pixels.show();
}

void portalConnectedLedPulse() {
  static unsigned long lastStep = 0;
  static bool phase = false;

  if (millis() - lastStep < 180) return;

  lastStep = millis();
  phase = !phase;

  if (phase) {
    pixels.setPixelColor(0, pixels.Color(0, 10, 0));
    pixels.setPixelColor(1, pixels.Color(8, 16, 0));
    pixels.setPixelColor(2, pixels.Color(8, 16, 0));
    pixels.setPixelColor(3, pixels.Color(0, 10, 0));
  } else {
    pixels.setPixelColor(0, pixels.Color(0, 2, 0));
    pixels.setPixelColor(1, pixels.Color(2, 5, 0));
    pixels.setPixelColor(2, pixels.Color(2, 5, 0));
    pixels.setPixelColor(3, pixels.Color(0, 2, 0));
  }
  pixels.show();
}

void flashSuccess() { clickLedAnimation(); }
void flashError() { errorLedAnimation(); }
void flashPortal() { portalOpenLedAnimation(); }

void audioInit() {
  ledcAttachPin(SPEAKER_PIN, AUDIO_CHANNEL);
  ledcSetup(AUDIO_CHANNEL, 2000, AUDIO_RES_BITS);
  ledcWriteTone(AUDIO_CHANNEL, 0);
}

void beepTone(int freq, int durationMs) {
  ledcWriteTone(AUDIO_CHANNEL, freq);
  delay(durationMs);
  ledcWriteTone(AUDIO_CHANNEL, 0);
  delay(4);
}

void soundClick() {
  beepTone(1047, 28);
  delay(6);
  beepTone(1319, 34);
}

void soundStartupHappy() {
  beepTone(880, 60);
  delay(12);
  beepTone(1175, 55);
  delay(12);
  beepTone(1568, 70);
}

void soundConnected() {
  beepTone(988, 45);
  delay(8);
  beepTone(1319, 45);
  delay(8);
  beepTone(1760, 55);
}

void soundHeartbeatIdle() {
  beepTone(110, 14);
}

void soundHeartbeatConnected() {
  beepTone(988, 18);
  delay(10);
  beepTone(1319, 20);
}

void soundDebug() {
  beepTone(740, 40);
  delay(10);
  beepTone(880, 45);
  delay(10);
  beepTone(988, 55);
}

void oledOn() {
  if (!oledOk) return;
  display.ssd1306_command(SSD1306_DISPLAYON);
  delay(2);
}

void oledOff() {
  if (!oledOk) return;
  display.clearDisplay();
  display.display();
  display.ssd1306_command(SSD1306_DISPLAYOFF);
}

void ensureHeaderIfNeeded(const String& path, const String& headerLine) {
  if (!fsOk) return;
  if (!LittleFS.exists(path)) {
    File f = LittleFS.open(path, FILE_WRITE);
    if (!f) return;
    f.println(headerLine);
    f.close();
  }
}

bool appendLine(const String& path, const String& line) {
  File f = LittleFS.open(path, FILE_APPEND);
  if (!f) return false;
  f.println(line);
  f.close();
  return true;
}

uint32_t countEventsInFile(const String& path) {
  if (!fsOk) return 0;
  if (!LittleFS.exists(path)) return 0;

  File f = LittleFS.open(path, FILE_READ);
  if (!f) return 0;

  uint32_t count = 0;
  bool firstLine = true;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;
    if (firstLine) {
      firstLine = false;
      continue;
    }
    count++;
  }
  f.close();
  return count;
}

void syncDailyCountersFromRtc() {
  if (!rtcTimeReady()) {
    dailyEntryCount = 0;
    currentBusinessDate = "";
    currentBusinessMonth = "";
    return;
  }

  DateTime business = getBusinessDateTime(rtc.now());
  currentBusinessDate = formatDate(business);
  currentBusinessMonth = formatMonth(business);
  dailyEntryCount = countEventsInFile(getDailyLogPathForNow(rtc.now()));
}

uint32_t countTodayEntries() {
  if (!rtcTimeReady()) return 0;

  DateTime business = getBusinessDateTime(rtc.now());
  String nowBusinessDate = formatDate(business);
  if (nowBusinessDate != currentBusinessDate) {
    syncDailyCountersFromRtc();
  }
  return dailyEntryCount;
}

uint32_t countMonthEntriesByMonthString(const String& monthStr) {
  if (!fsOk) return 0;

  File root = LittleFS.open("/");
  if (!root) return 0;

  uint32_t total = 0;
  File file = root.openNextFile();
  while (file) {
    String name = file.name();
    if (!file.isDirectory() && name.startsWith("/day_") && getMonthFromDailyPath(name) == monthStr) {
      total += countEventsInFile(name);
    }
    file = root.openNextFile();
  }

  root.close();
  return total;
}

uint32_t countCurrentMonthEntries() {
  if (!rtcTimeReady()) return 0;
  DateTime business = getBusinessDateTime(rtc.now());
  return countMonthEntriesByMonthString(formatMonth(business));
}

bool adjustRtcFromLocalParts(int year, int month, int day, int hour, int minute, int second) {
  if (!rtcOk) return false;
  DateTime dt(year, month, day, hour, minute, second);
  if (!isRtcTimeValid(dt)) return false;

  rtc.adjust(dt);
  syncDailyCountersFromRtc();
  lastShownEntryCount = dailyEntryCount;
  return true;
}

bool rebuildMonthlySummary(const String& businessMonth, const String& monthlyPath, const String& currentDay) {
  String monthlyContent = "business_date,total_entries\n";

  File root = LittleFS.open("/");
  if (!root) return false;

  File file = root.openNextFile();
  while (file) {
    String name = file.name();
    if (!file.isDirectory() && name.startsWith("/day_") && getMonthFromDailyPath(name) == businessMonth) {
      String d = getDateFromDailyPath(name);
      uint32_t countForDay = (d == currentDay) ? dailyEntryCount : countEventsInFile(name);
      monthlyContent += d + "," + String(countForDay) + "\n";
    }
    file = root.openNextFile();
  }
  root.close();

  File mf = LittleFS.open(monthlyPath, FILE_WRITE);
  if (!mf) return false;
  mf.print(monthlyContent);
  mf.close();
  return true;
}

bool appendEvent(const char* eventType) {
  if (!fsOk) return false;
  if (!rtcTimeReady()) return false;

  DateTime nowLocal = rtc.now();
  DateTime business = getBusinessDateTime(nowLocal);
  String businessDate = formatDate(business);
  String businessMonth = formatMonth(business);

  if (businessDate != currentBusinessDate || businessMonth != currentBusinessMonth) {
    syncDailyCountersFromRtc();
  }

  String dailyPath = getDailyLogPathForNow(nowLocal);
  String monthlyPath = getMonthlySummaryPathForNow(nowLocal);

  ensureHeaderIfNeeded(dailyPath, "rtc_datetime,business_date,business_day_name,business_hour,event_type,device_id,unix_time");
  ensureHeaderIfNeeded(monthlyPath, "business_date,total_entries");

  String line;
  line += csvEscape(formatDateTime(nowLocal));
  line += "," + csvEscape(businessDate);
  line += "," + csvEscape(dayNameTr(business.dayOfTheWeek()));
  line += "," + String(getBusinessHour(nowLocal));
  line += "," + csvEscape(String(eventType));
  line += "," + csvEscape(deviceId);
  line += "," + String((uint32_t)nowLocal.unixtime());

  if (!appendLine(dailyPath, line)) return false;

  dailyEntryCount++;
  currentBusinessDate = businessDate;
  currentBusinessMonth = businessMonth;
  lastShownEntryCount = dailyEntryCount;

  return rebuildMonthlySummary(businessMonth, monthlyPath, businessDate);
}

void renderLoadingScreen() {
  if (!oledOk) return;
  oledOn();
  display.invertDisplay(false);
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(2);
  display.setCursor(18, 12);
  display.print("LOADING");
  display.setTextSize(1);
  display.setCursor(26, 40);
  display.print("Kase Baslatiliyor");
  display.display();
}

void renderOkScreen(uint32_t entryCount, bool invertNow) {
  if (!oledOk) return;
  oledOn();
  display.clearDisplay();
  display.invertDisplay(invertNow);
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(3);
  display.setCursor(36, 8);
  display.print("OK");
  display.drawLine(10, 38, 118, 38, SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(6, 44);
  display.print("GUN:");
  display.print(entryCount);
  display.setCursor(68, 44);
  display.print("AY:");
  display.print(countCurrentMonthEntries());
  display.display();
}

void renderPortalScreen(bool invertNow) {
  if (!oledOk) return;
  oledOn();
  display.clearDisplay();
  display.invertDisplay(invertNow);
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("WiFi PORTAL ACIK");
  display.setCursor(0, 12);
  display.print(apName);
  display.setCursor(0, 24);
  display.print(WiFi.softAPIP().toString());
  display.setCursor(0, 36);
  display.print(portalClientConnected ? "DURUM: BAGLI" : "DURUM: BEKLIYOR");
  display.setCursor(0, 48);
  display.print("GUN:");
  display.print(countTodayEntries());
  display.setCursor(64, 48);
  display.print("AY:");
  display.print(countCurrentMonthEntries());
  display.display();
}

void renderDebugScreen(bool invertNow) {
  if (!oledOk) return;
  oledOn();
  display.clearDisplay();
  display.invertDisplay(invertNow);
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("DEBUG ");
  display.print(deviceId);
  display.setCursor(0, 10);
  display.print("RTC:");
  display.print(rtcTimeReady() ? "OK" : "ERR");
  display.setCursor(62, 10);
  display.print("WiFi:");
  if (apActive) display.print(portalClientConnected ? "BAGLI" : "AP");
  else display.print("OFF");
  display.setCursor(0, 20);
  if (rtcTimeReady()) display.print(formatDate(rtc.now()));
  else display.print("RTC AYARSIZ");
  display.setCursor(0, 30);
  if (rtcTimeReady()) display.print(formatTime(rtc.now()));
  else display.print("--:--:--");
  display.setCursor(0, 40);
  display.print("GUN:");
  display.print(countTodayEntries());
  display.setCursor(64, 40);
  display.print("AY:");
  display.print(countCurrentMonthEntries());
  display.setCursor(0, 54);
  display.print("CLI:");
  display.print(WiFi.softAPgetStationNum());
  display.display();
}

void triggerInvertFlash(unsigned long ms) {
  invertUntilMs = millis() + ms;
}

void showDebugScreenNow() {
  debugUntilMs = millis() + DEBUG_SCREEN_MS;
  triggerInvertFlash(300);
  soundDebug();
  oledOn();
}

void showLoadingScreen3s() {
  currentScreen = SCREEN_LOADING;
  renderLoadingScreen();
  startupLedAnimation();
  soundStartupHappy();
  delay(STARTUP_LOADING_MS);
}

void updateScreen() {
  bool invertNow = (millis() < invertUntilMs);

  if (millis() < debugUntilMs) {
    currentScreen = SCREEN_DEBUG;
    renderDebugScreen(invertNow);
    return;
  }

  if (apActive) {
    currentScreen = SCREEN_PORTAL;
    renderPortalScreen(invertNow);
    return;
  }

  if (millis() < infoUntilMs) {
    currentScreen = SCREEN_OK;
    renderOkScreen(lastShownEntryCount, invertNow);
    return;
  }

  currentScreen = SCREEN_NONE;
  if (oledOk) {
    display.invertDisplay(false);
    oledOff();
  }
}

void touchPortalActivity() {
  portalLastActivityAt = millis();
}

String buildDailyRows() {
  if (!fsOk) return "<tr><td colspan='3'>LittleFS yok</td></tr>";

  String rows;
  File root = LittleFS.open("/");
  if (!root) return "<tr><td colspan='3'>Klasor acilamadi</td></tr>";

  File file = root.openNextFile();
  bool found = false;
  while (file) {
    String name = file.name();
    if (!file.isDirectory() && name.startsWith("/day_")) {
      found = true;
      rows += "<tr>";
      rows += "<td>" + htmlEscape(name.substring(5, 15)) + "</td>";
      rows += "<td>" + String(countEventsInFile(name)) + "</td>";
      rows += "<td><a href='/download?file=" + name + "'>Indir</a></td>";
      rows += "</tr>";
    }
    file = root.openNextFile();
  }
  root.close();

  if (!found) rows = "<tr><td colspan='3'>Gunluk log yok</td></tr>";
  return rows;
}

String buildMonthlyRows() {
  if (!fsOk) return "<tr><td colspan='4'>LittleFS yok</td></tr>";

  String rows;
  File root = LittleFS.open("/");
  if (!root) return "<tr><td colspan='4'>Klasor acilamadi</td></tr>";

  File file = root.openNextFile();
  bool found = false;
  while (file) {
    String name = file.name();
    if (!file.isDirectory() && name.startsWith("/month_") && name.endsWith("_summary.csv")) {
      found = true;
      String monthStr = name.substring(7, 14);
      rows += "<tr>";
      rows += "<td>" + htmlEscape(monthStr) + "</td>";
      rows += "<td>" + String(countMonthEntriesByMonthString(monthStr)) + "</td>";
      rows += "<td><a href='/download?file=" + name + "'>Ozet CSV</a></td>";
      rows += "<td><a href='/download-month-bulk?month=" + monthStr + "'>Toplu Indir</a></td>";
      rows += "</tr>";
    }
    file = root.openNextFile();
  }
  root.close();

  if (!found) rows = "<tr><td colspan='4'>Aylik log yok</td></tr>";
  return rows;
}

String buildAllLogsCombined() {
  String out = "file_name,content\n";
  File root = LittleFS.open("/");
  if (!root) return out;

  File file = root.openNextFile();
  while (file) {
    String name = file.name();
    if (!file.isDirectory() && (name.startsWith("/day_") || name.startsWith("/month_"))) {
      File f = LittleFS.open(name, FILE_READ);
      if (f) {
        while (f.available()) {
          String line = f.readStringUntil('\n');
          line.trim();
          if (line.length() == 0) continue;
          out += csvEscape(name) + "," + csvEscape(line) + "\n";
        }
        f.close();
      }
    }
    file = root.openNextFile();
  }

  root.close();
  return out;
}

String buildMonthBulkCsv(const String& monthStr) {
  String out = "source_file,rtc_datetime,business_date,business_day_name,business_hour,event_type,device_id,unix_time\n";

  File root = LittleFS.open("/");
  if (!root) return out;

  File file = root.openNextFile();
  while (file) {
    String name = file.name();
    if (!file.isDirectory() && name.startsWith("/day_") && getMonthFromDailyPath(name) == monthStr) {
      File f = LittleFS.open(name, FILE_READ);
      if (f) {
        bool firstLine = true;
        while (f.available()) {
          String line = f.readStringUntil('\n');
          line.trim();
          if (line.length() == 0) continue;
          if (firstLine) {
            firstLine = false;
            continue;
          }
          out += name.substring(1) + "," + line + "\n";
        }
        f.close();
      }
    }
    file = root.openNextFile();
  }

  root.close();
  return out;
}

void registerValidClick() {
  if (appendEvent("valid")) {
    flashSuccess();
    soundClick();
    infoUntilMs = millis() + INFO_SCREEN_MS;
    triggerInvertFlash(180);
    oledOn();
  } else {
    flashError();
  }
}

void setupWebRoutes() {
  web.on("/sync-time", HTTP_GET, []() {
    touchPortalActivity();
    if (!web.hasArg("y") || !web.hasArg("mo") || !web.hasArg("day") ||
        !web.hasArg("h") || !web.hasArg("mi") || !web.hasArg("s")) {
      web.send(400, "application/json", "{\"ok\":false,\"msg\":\"eksik parametre\"}");
      return;
    }

    int y = web.arg("y").toInt();
    int mo = web.arg("mo").toInt();
    int d = web.arg("day").toInt();
    int h = web.arg("h").toInt();
    int mi = web.arg("mi").toInt();
    int s = web.arg("s").toInt();

    bool ok = adjustRtcFromLocalParts(y, mo, d, h, mi, s);
    if (ok) {
      triggerInvertFlash(400);
      soundConnected();
      web.send(200, "application/json", "{\"ok\":true}");
    } else {
      web.send(400, "application/json", "{\"ok\":false,\"msg\":\"gecersiz zaman\"}");
    }
  });

  web.on("/", HTTP_GET, []() {
    touchPortalActivity();

    bool rtcValid = rtcTimeReady();
    String nowStr = rtcValid ? formatDateTime(rtc.now()) : "AYARSIZ";
    String currentDaily = rtcValid ? getDailyLogPathForNow(rtc.now()) : "RTC ayarsiz";
    String currentMonth = rtcValid ? getMonthlySummaryPathForNow(rtc.now()) : "RTC ayarsiz";
    uint32_t todayCount = rtcValid ? countTodayEntries() : 0;
    uint32_t monthCount = rtcValid ? countCurrentMonthEntries() : 0;

    String html;
    html += "<!doctype html><html><head>";
    html += "<meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
    html += "<title>Kase Logger</title>";
    html += "<style>";
    html += "body{font-family:Arial;background:#111;color:#fff;margin:0;padding:16px}";
    html += ".box{max-width:1000px;margin:auto}";
    html += ".card{background:#1b1b1b;border:1px solid #333;border-radius:14px;padding:16px;margin-bottom:14px}";
    html += "table{width:100%;border-collapse:collapse}";
    html += "th,td{padding:10px;border-bottom:1px solid #333;text-align:left}";
    html += "a{color:#8ecbff;text-decoration:none}";
    html += "button{padding:10px 14px;border:none;border-radius:10px;background:#1f7aff;color:#fff;font-weight:bold;cursor:pointer}";
    html += ".danger{background:#a32626}";
    html += ".ok{color:#77e58b}";
    html += ".bad{color:#ff8686}";
    html += "</style></head><body><div class='box'>";

    html += "<div class='card'><h2>Kase Logger</h2>";
    html += "<p><b>Device ID:</b> " + htmlEscape(deviceId) + "</p>";
    html += "<p><b>SSID:</b> " + htmlEscape(apName) + "</p>";
    html += "<p><b>IP:</b> " + WiFi.softAPIP().toString() + "</p>";
    html += "<p><b>RTC:</b> <span class='" + String(rtcValid ? "ok" : "bad") + "'>" + String(rtcValid ? "OK" : "AYARSIZ") + "</span></p>";
    html += "<p><b>Saat:</b> " + htmlEscape(nowStr) + "</p>";
    html += "<p><b>Bugunku Giris:</b> " + String(todayCount) + "</p>";
    html += "<p><b>Bu Ay Giris:</b> " + String(monthCount) + "</p>";
    html += "<p><b>Aktif Gunluk:</b> " + htmlEscape(currentDaily) + "</p>";
    html += "<p><b>Aktif Aylik:</b> " + htmlEscape(currentMonth) + "</p>";
    html += "<p><button onclick='syncTimeNow()'>Saati telefonumdan esitle</button></p>";
    html += "<p><a href='/log-now'>Test log yaz</a></p>";
    html += "<p><a href='/download-all-logs'>Tum loglari tek dosyada indir</a></p>";
    html += "</div>";

    html += "<div class='card'><h3>Aylik Dosyalar</h3>";
    html += "<table><thead><tr><th>Ay</th><th>Toplam</th><th>Ozet</th><th>Toplu</th></tr></thead><tbody>";
    html += buildMonthlyRows();
    html += "</tbody></table></div>";

    html += "<div class='card'><h3>Gunluk Dosyalar</h3>";
    html += "<table><thead><tr><th>Tarih</th><th>Giris</th><th>Indir</th></tr></thead><tbody>";
    html += buildDailyRows();
    html += "</tbody></table></div>";

    html += "<div class='card'>";
    html += "<form action='/delete-all' method='POST' onsubmit='return confirm(\"Tum loglar silinsin mi?\")'>";
    html += "<button class='danger' type='submit'>Tum loglari sil</button>";
    html += "</form></div>";

    html += "<script>";
    html += "async function syncTimeNow(){";
    html += " const n = new Date();";
    html += " const url = `/sync-time?y=${n.getFullYear()}&mo=${n.getMonth()+1}&day=${n.getDate()}&h=${n.getHours()}&mi=${n.getMinutes()}&s=${n.getSeconds()}`;";
    html += " try {";
    html += "  const r = await fetch(url,{cache:'no-store'});";
    html += "  const j = await r.json();";
    html += "  if(j.ok){location.reload();} else {alert('Saat senkronu basarisiz');}";
    html += " } catch(e){ alert('Saat senkronu basarisiz'); }";
    html += "}";
    html += "</script>";

    html += "</div></body></html>";
    web.send(200, "text/html; charset=utf-8", html);
  });

  web.on("/download", HTTP_GET, []() {
    touchPortalActivity();
    if (!web.hasArg("file")) {
      web.send(400, "text/plain", "file gerekli");
      return;
    }

    String path = normalizePath(web.arg("file"));
    if (path.length() == 0) {
      web.send(400, "text/plain", "gecersiz dosya");
      return;
    }
    if (!LittleFS.exists(path)) {
      web.send(404, "text/plain", "dosya yok");
      return;
    }

    File f = LittleFS.open(path, FILE_READ);
    if (!f) {
      web.send(500, "text/plain", "dosya acilamadi");
      return;
    }

    web.sendHeader("Content-Disposition", "attachment; filename=\"" + path.substring(1) + "\"");
    web.streamFile(f, "text/csv");
    f.close();
  });

  web.on("/download-month-bulk", HTTP_GET, []() {
    touchPortalActivity();
    if (!web.hasArg("month")) {
      web.send(400, "text/plain", "month gerekli");
      return;
    }

    String monthStr = web.arg("month");
    String csv = buildMonthBulkCsv(monthStr);
    web.sendHeader("Content-Disposition", "attachment; filename=\"bulk_" + monthStr + ".csv\"");
    web.send(200, "text/csv", csv);
  });

  web.on("/download-all-logs", HTTP_GET, []() {
    touchPortalActivity();
    String csv = buildAllLogsCombined();
    web.sendHeader("Content-Disposition", "attachment; filename=\"all_logs.csv\"");
    web.send(200, "text/csv", csv);
  });

  web.on("/delete-all", HTTP_POST, []() {
    touchPortalActivity();

    File root = LittleFS.open("/");
    if (root) {
      File file = root.openNextFile();
      while (file) {
        String name = file.name();
        if (!file.isDirectory()) LittleFS.remove(name);
        file = root.openNextFile();
      }
      root.close();
    }

    syncDailyCountersFromRtc();
    lastShownEntryCount = dailyEntryCount;
    web.sendHeader("Location", "/", true);
    web.send(302, "text/plain", "");
  });

  web.on("/log-now", HTTP_GET, []() {
    touchPortalActivity();
    if (appendEvent("manual_test")) {
      lastShownEntryCount = countTodayEntries();
      infoUntilMs = millis() + INFO_SCREEN_MS;
      triggerInvertFlash(250);
      soundClick();
    }
    web.sendHeader("Location", "/", true);
    web.send(302, "text/plain", "");
  });

  web.onNotFound([]() {
    touchPortalActivity();
    web.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/", true);
    web.send(302, "text/plain", "");
  });
}

void startPortal() {
  if (apActive) return;

  WiFi.mode(WIFI_AP);
  WiFi.softAP(apName.c_str(), "clicker123");
  dns.start(53, "*", WiFi.softAPIP());
  web.begin();

  apActive = true;
  portalClientConnected = false;
  lastPortalClientConnected = false;
  touchPortalActivity();

  flashPortal();
  triggerInvertFlash(180);
  oledOn();
}

void stopPortal() {
  if (!apActive) return;

  dns.stop();
  web.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);

  apActive = false;
  portalClientConnected = false;
  lastPortalClientConnected = false;

  if (oledOk) display.invertDisplay(false);
  oledOff();
  clearPixels();
}

void updatePortalClientStatus() {
  if (!apActive) return;
  if (millis() - lastClientCheckMs < CLIENT_CHECK_MS) return;

  lastClientCheckMs = millis();
  portalClientConnected = (WiFi.softAPgetStationNum() > 0);

  if (portalClientConnected != lastPortalClientConnected) {
    lastPortalClientConnected = portalClientConnected;
    triggerInvertFlash(400);
    soundConnected();
  }
}

void updatePortalButtonLogic() {
  static bool lastReading = HIGH;
  static bool stableState = HIGH;
  static unsigned long lastDebounceMs = 0;
  static unsigned long pressedAt = 0;
  static bool longPressHandled = false;
  static bool debugPressHandled = false;

  bool reading = digitalRead(BUTTON_PIN);

  if (reading != lastReading) {
    lastDebounceMs = millis();
    lastReading = reading;
  }

  if (millis() - lastDebounceMs > BUTTON_DEBOUNCE_MS) {
    if (reading != stableState) {
      stableState = reading;
      if (stableState == LOW) {
        pressedAt = millis();
        longPressHandled = false;
        debugPressHandled = false;
      } else {
        if (!longPressHandled && !debugPressHandled) {
          touchPortalActivity();
        }
      }
    }
  }

  if (stableState == LOW && !debugPressHandled && millis() - pressedAt >= DEBUG_PRESS_MS) {
    debugPressHandled = true;
    longPressHandled = true;
    showDebugScreenNow();
  } else if (stableState == LOW && !longPressHandled && millis() - pressedAt >= LONG_PRESS_MS) {
    longPressHandled = true;
    stopPortal();
    flashPortal();
  }
}

void updateActiveModeButtonLogic() {
  static bool lastReading = HIGH;
  static bool stableState = HIGH;
  static unsigned long lastDebounceMs = 0;
  static unsigned long pressedAt = 0;
  static bool longPressHandled = false;
  static bool debugPressHandled = false;

  bool reading = digitalRead(BUTTON_PIN);

  if (reading != lastReading) {
    lastDebounceMs = millis();
    lastReading = reading;
  }

  if (millis() - lastDebounceMs > BUTTON_DEBOUNCE_MS) {
    if (reading != stableState) {
      stableState = reading;
      if (stableState == LOW) {
        pressedAt = millis();
        longPressHandled = false;
        debugPressHandled = false;
      } else {
        if (!longPressHandled && !debugPressHandled) {
          registerValidClick();
        }
      }
    }
  }

  if (stableState == LOW && !debugPressHandled && millis() - pressedAt >= DEBUG_PRESS_MS) {
    debugPressHandled = true;
    longPressHandled = true;
    showDebugScreenNow();
  } else if (stableState == LOW && !longPressHandled && millis() - pressedAt >= LONG_PRESS_MS) {
    longPressHandled = true;
    startPortal();
  }
}

void updateHeartbeat() {
  if (!apActive) return;

  unsigned long interval = portalClientConnected ? HEARTBEAT_CONNECTED_MS : HEARTBEAT_IDLE_MS;
  if (millis() - lastHeartbeatMs >= interval) {
    lastHeartbeatMs = millis();
    if (portalClientConnected) soundHeartbeatConnected();
    else soundHeartbeatIdle();
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);

  fsOk = LittleFS.begin(true);
  rtcOk = rtc.begin();

  uint64_t mac = ESP.getEfuseMac();
  uint32_t shortMac = (uint32_t)(mac & 0xFFFFFFULL);
  deviceId = "S2-" + String(shortMac, HEX);
  deviceId.toUpperCase();
  apName = "Kase-" + deviceId;

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  audioInit();
  pixels.begin();
  pixels.setBrightness(24);
  clearPixels();

  Wire.begin(I2C_SDA, I2C_SCL);
  oledOk = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  if (oledOk) {
    display.clearDisplay();
    display.display();
  }
  oledOff();

  if (rtcOk) {
    DateTime now = rtc.now();
    if (rtc.lostPower() || !isRtcTimeValid(now)) {
      rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    }
  }

  setupWebRoutes();

  if (fsOk && rtcTimeReady()) {
    ensureHeaderIfNeeded(getDailyLogPathForNow(rtc.now()),
      "rtc_datetime,business_date,business_day_name,business_hour,event_type,device_id,unix_time");
    ensureHeaderIfNeeded(getMonthlySummaryPathForNow(rtc.now()), "business_date,total_entries");
  }

  syncDailyCountersFromRtc();
  lastShownEntryCount = dailyEntryCount;

  showLoadingScreen3s();
}

void loop() {
  if (apActive) {
    dns.processNextRequest();
    web.handleClient();
    updatePortalClientStatus();
    updatePortalButtonLogic();
    updateHeartbeat();
    updateScreen();

    if (millis() < debugUntilMs) {
      clearPixels();
    } else if (portalClientConnected) {
      portalConnectedLedPulse();
    } else {
      portalIdleLedBreath();
    }

    if (millis() - portalLastActivityAt >= PORTAL_AUTO_OFF_MS) {
      stopPortal();
    }

    delay(2);
    return;
  }

  updateActiveModeButtonLogic();
  updateScreen();

  if (millis() < debugUntilMs) {
    clearPixels();
  }

  if (!apActive && millis() > infoUntilMs && millis() > debugUntilMs) {
    oledOff();
    clearPixels();
  }

  delay(1);
}
