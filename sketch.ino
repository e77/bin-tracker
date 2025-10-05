/************** PART 1 / 4 — START ********************************************
 * Bin Tracker – ESP32-S3 + GC9A01 Round TFT (TFT_eSPI) + GitHub JSON
 * - Manual NTP via UDP + UK DST (BST)
 * - Startup centered messages + Demo
 * - Banner/border: Yellow if tomorrow (<16:00), Red if tomorrow (>=16:00)
 * - Flat 46×46 wheelie-bin icons (no symbols) + labels under icons (Layout D)
 * - Footer (date/time + ID)
 * - Night backlight OFF 00:00–06:00 unless within 24h
 * - Serial Test Harness: TEST DEMO/NORMAL/YELLOW/RED/DATE/JSON/NET/REFRESH/ICONS A|C|D
 * - JSON refresh: at startup, then hourly
 *****************************************************************************/

#include <Arduino.h>

// ---------- Forward declarations ----------
struct BinData;
bool   parseISODate(const String& s, tm& out);
String fmtHumanDate(const tm& tmin);
String fmtHumanDateUK_fromISO(const String& iso);
String prettyBin(const String& binType);
void   drawFooter(const String& identifier);
int    bannerTypeForDate(const String& nextDate);
void   drawBanner(int type);
void   drawUI(const BinData& data, const String& nextDate, bool forceRedAlert);
long   secondsUntilDateStartLocal_UK(const String& iso);
String todayISO_UK();
String tomorrowISO_UK();
bool   nowLocalUK(struct tm& out);
bool   ntpSyncOnce(uint32_t timeoutMs = 1500);
bool   fetchJson(String& out);
bool   parseSchedule(const String& json, BinData& dataOut);
String findNextDate(const std::vector<String>& sortedDates, const String& today);
void   applyNightModeBacklight(const String& nextDate);
// Serial harness
void   handleCommand(const String& line);
void   pollSerial();
// Data refresh
bool   refreshData(bool showErrors);
// ------------------------------------------

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiUdp.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <vector>
#include <map>
#include <algorithm>
#include <time.h>
#include <limits.h>

TFT_eSPI tft;  // Ensure your User_Setup in TFT_eSPI is configured for GC9A01 round TFT

// ----------- USER CONFIG -----------
const char* WIFI_SSID     = "Plumbing_IoT";
const char* WIFI_PASSWORD = "2hphdwdFwkpr";
const char* JSON_URL      = "https://raw.githubusercontent.com/e77/bin-tracker/main/15b.json";

#define BACKLIGHT_PIN 40    // HIGH = ON, LOW = OFF
const int NIGHT_START_HOUR = 0;   // 00:00
const int NIGHT_END_HOUR   = 6;   // 06:00
// -----------------------------------

// ----------- Globals -----------
struct BinData {
  String identifier;
  String collectionDay;
  std::map<String, String, std::less<>> colourByType;
  std::vector<String> dates;
  std::map<String, std::vector<String>, std::less<>> schedule;
};

BinData g_data;                 // current schedule
String  g_nextDate;             // next collection date ISO (YYYY-MM-DD)
String  g_identifier = "15B";   // overridden by JSON if provided

// Test harness overrides (cleared on reset)
int    g_bannerOverride = -1;  // -1 none, 0 none, 1 yellow, 2 red
String g_todayOverride  = "";  // "" = live clock, else YYYY-MM-DD
bool   g_forceDemo      = false;
bool   g_forceNormal    = false; // explicitly show normal

// Icon style (kept for future expansion): 0 = current flat vector bins
int    g_iconStyle      = 0;

// JSON fetch status tracking (for TEST JSON)
String   g_lastFetchStatus = "No fetch yet";
time_t   g_lastFetchUtc    = 0;
uint32_t g_lastFetchMillis = 0;  // for hourly refresh control

// NTP
WiFiUDP udp;
const char*  NTP_SERVER     = "pool.ntp.org";
const int    NTP_PORT       = 123;
const int    LOCAL_UDP_PORT = 2390;
const uint32_t NTP_TO_UNIX  = 2208988800UL; // seconds between 1900 and 1970
time_t   g_lastUtc       = 0;
uint32_t g_lastUtcMillis = 0;
// -------------------------------

/******************************************************************************
 * User-Friendly Centered Startup Messages
 ******************************************************************************/
void showCentered(const char* msg, int yOffset = 0, int font = 2, uint16_t color = TFT_WHITE) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(color, TFT_BLACK);
  tft.drawString(msg, tft.width()/2, (tft.height()/2) + yOffset, font);
  delay(800);
}

void showCenteredTwoLines(const char* l1, const char* l2, int font = 2, uint16_t color = TFT_WHITE) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(color, TFT_BLACK);
  tft.drawString(l1, tft.width()/2, (tft.height()/2) - 12, font);
  tft.drawString(l2, tft.width()/2, (tft.height()/2) + 12, font);
  delay(800);
}

/******************************************************************************
 * Backlight Control (ON/OFF)
 ******************************************************************************/
void setBacklight(bool on) { pinMode(BACKLIGHT_PIN, OUTPUT); digitalWrite(BACKLIGHT_PIN, on ? HIGH : LOW); }

/******************************************************************************
 * Manual NTP (UDP)
 ******************************************************************************/
bool ntpSyncOnce(uint32_t timeoutMs) {
  IPAddress ntpIP;
  if (!WiFi.hostByName(NTP_SERVER, ntpIP)) return false;
  uint8_t packet[48] = {0};
  packet[0] = 0b11100011; // LI, VN, Mode=3 (client)
  udp.begin(LOCAL_UDP_PORT);
  udp.beginPacket(ntpIP, NTP_PORT);
  udp.write(packet, 48);
  udp.endPacket();
  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    int len = udp.parsePacket();
    if (len >= 48) {
      udp.read(packet, 48);
      uint32_t secs1900 = ((uint32_t)packet[40] << 24) |
                          ((uint32_t)packet[41] << 16) |
                          ((uint32_t)packet[42] << 8)  |
                          (uint32_t)packet[43];
      time_t unixSecs = (time_t)(secs1900 - NTP_TO_UNIX);
      g_lastUtc = unixSecs;
      g_lastUtcMillis = millis();
      return true;
    }
    delay(10);
  }
  return false;
}

time_t nowUtc() {
  if (g_lastUtc == 0) return 0;
  uint32_t elapsed = millis() - g_lastUtcMillis;
  return g_lastUtc + (elapsed / 1000);
}
/************** PART 1 / 4 — END **********************************************/

/************** PART 2 / 4 — START ********************************************
 * UK DST helpers, local time helpers, JSON, schedule utils
 *****************************************************************************/

static time_t timegm_stub(struct tm* t) { return mktime(t); }

int lastSundayDayOfMonth(int year, int month) {
  tm tt = {}; tt.tm_year = year - 1900; tt.tm_mon = month - 1; tt.tm_mday = 31;
  mktime(&tt);
  int back = tt.tm_wday; // 0..6 (Sun..Sat)
  return 31 - back;
}

bool isBST_UK(time_t utc) {
  tm ut = {}; gmtime_r(&utc, &ut);
  int year = ut.tm_year + 1900;
  int marLastSun = lastSundayDayOfMonth(year, 3);
  int octLastSun = lastSundayDayOfMonth(year, 10);

  tm start = {}; start.tm_year = year - 1900; start.tm_mon = 2; start.tm_mday = marLastSun; start.tm_hour = 1;
  tm endt  = {}; endt.tm_year  = year - 1900; endt.tm_mon  = 9; endt.tm_mday  = octLastSun; endt.tm_hour  = 1;

  time_t bstStartUTC = timegm_stub(&start);
  time_t bstEndUTC   = timegm_stub(&endt);

  return (utc >= bstStartUTC && utc < bstEndUTC);
}

int  ukOffsetForUTC(time_t utc) { return isBST_UK(utc) ? 3600 : 0; }
void localtime_uk(time_t utc, struct tm* out) { time_t adj = utc + ukOffsetForUTC(utc); gmtime_r(&adj, out); }

bool nowLocalUK(struct tm& out) {
  time_t utc = nowUtc(); if (utc == 0) return false;
  localtime_uk(utc, &out); return true;
}

String todayISO_UK() {
  if (g_todayOverride.length()) return g_todayOverride;
  struct tm lt{}; if (!nowLocalUK(lt)) return "";
  char buf[11]; strftime(buf, sizeof(buf), "%Y-%m-%d", &lt); return String(buf);
}

String tomorrowISO_UK() {
  if (g_todayOverride.length()) {
    tm t{}; if (!parseISODate(g_todayOverride, t)) return "";
    time_t localEpoch = timegm_stub(&t);
    int off = ukOffsetForUTC(localEpoch);
    time_t utc = localEpoch - off + 24*3600;
    tm lt{}; localtime_uk(utc, &lt);
    char buf[11]; strftime(buf, sizeof(buf), "%Y-%m-%d", &lt);
    return String(buf);
  }
  time_t utc = nowUtc(); if (utc == 0) return "";
  utc += 24 * 3600;
  struct tm lt{}; localtime_uk(utc, &lt);
  char buf[11]; strftime(buf, sizeof(buf), "%Y-%m-%d", &lt);
  return String(buf);
}

long secondsUntilDateStartLocal_UK(const String& iso) {
  time_t utcNow = nowUtc(); if (utcNow == 0) return LONG_MAX;
  struct tm tgtLocal{}; if (!parseISODate(iso, tgtLocal)) return LONG_MAX;
  tgtLocal.tm_hour = 0; tgtLocal.tm_min = 0; tgtLocal.tm_sec = 0;
  time_t guessUTC = timegm_stub(&tgtLocal);
  int off = ukOffsetForUTC(guessUTC);
  time_t targetUTC = guessUTC - off;
  return (long)difftime(targetUTC, utcNow);
}

/******************************************************************************
 * JSON fetch + parse
 ******************************************************************************/
bool fetchJson(String& out) {
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, JSON_URL)) return false;
  int code = http.GET();

  time_t nowU = nowUtc(); g_lastFetchUtc = nowU; g_lastFetchMillis = millis();
  if (code != HTTP_CODE_OK) {
    out = "ERR" + String(code);
    g_lastFetchStatus = "ERR" + String(code);
    http.end(); return false;
  }
  out = http.getString();
  g_lastFetchStatus = "OK";
  http.end();
  return true;
}

bool parseSchedule(const String& json, BinData& dataOut) {
  StaticJsonDocument<16 * 1024> doc;
  if (deserializeJson(doc, json)) return false;

  dataOut.identifier     = doc["identifier"] | "";
  dataOut.collectionDay  = doc["collection_day"] | "";

  if (dataOut.identifier.length()) g_identifier = dataOut.identifier;

  dataOut.colourByType.clear();
  if (doc.containsKey("bin_colours")) {
    for (JsonPair kv : doc["bin_colours"].as<JsonObject>()) {
      dataOut.colourByType[kv.key().c_str()] = kv.value().as<const char*>();
    }
  }

  dataOut.dates.clear();
  dataOut.schedule.clear();
  for (JsonPair kv : doc["schedule"].as<JsonObject>()) {
    String date = kv.key().c_str();
    dataOut.dates.push_back(date);
    std::vector<String> bins;
    for (JsonVariant v : kv.value().as<JsonArray>()) {
      bins.push_back(String(v.as<const char*>()));
    }
    dataOut.schedule[date] = bins;
  }

  std::sort(dataOut.dates.begin(), dataOut.dates.end());
  return true;
}

String findNextDate(const std::vector<String>& sortedDates, const String& today) {
  for (auto& d : sortedDates) { if (d >= today) return d; }
  return "";
}

String fmtHumanDateUK_fromISO(const String& iso) {
  tm t{}; if (!parseISODate(iso, t)) return iso;
  time_t localEpoch = timegm_stub(&t);
  int off = ukOffsetForUTC(localEpoch);
  time_t utc = localEpoch - off;
  tm loc{}; localtime_uk(utc, &loc);
  return fmtHumanDate(loc);
}
/************** PART 3 / 4 — START ********************************************
 * Time helpers (defs), flat 46×46 icons, banner, footer, main UI (Layout D)
 *****************************************************************************/

bool parseISODate(const String& s, tm& out) {
  if (s.length() != 10) return false;
  int y = s.substring(0,4).toInt();
  int m = s.substring(5,7).toInt();
  int d = s.substring(8,10).toInt();
  memset(&out, 0, sizeof(tm));
  out.tm_year = y - 1900; out.tm_mon = m - 1; out.tm_mday = d;
  return true;
}
String fmtHumanDate(const tm& tmin) { char buf[32]; strftime(buf, sizeof(buf), "%a %d %b", &tmin); return String(buf); }

/******************************************************************************
 * Labels
 ******************************************************************************/
String prettyBin(const String& binType) {
  if (binType.equalsIgnoreCase("rubbish"))   return "RUBBISH";
  if (binType.equalsIgnoreCase("recycling")) return "RECYCLING";
  if (binType.equalsIgnoreCase("food"))      return "FOOD";
  if (binType.equalsIgnoreCase("garden"))    return "GARDEN";
  return binType;
}

/******************************************************************************
 * 46×46 Flat Colour Wheelie Bin Icons (No Symbols)
 ******************************************************************************/
static inline void drawWheelieBinFlat(int x, int y, uint16_t bodyColor, uint16_t lidColor) {
  // Body
  tft.fillRoundRect(x+6, y+10, 34, 30, 5, bodyColor);
  // Lid
  tft.fillRoundRect(x+4, y+6,  38,  6, 3, lidColor);
  // Wheels
  tft.fillCircle(x+14, y+44, 4, TFT_BLACK);
  tft.fillCircle(x+32, y+44, 4, TFT_BLACK);
  // Outline
  tft.drawRoundRect(x+6, y+10, 34, 30, 5, TFT_BLACK);
  tft.drawRoundRect(x+4, y+6,  38,  6, 3, TFT_BLACK);
}

void drawIconFlat_Rubbish46(int x, int y)   { drawWheelieBinFlat(x, y, tft.color565(40,40,40), tft.color565(90,90,90)); }
void drawIconFlat_Recycling46(int x, int y) { drawWheelieBinFlat(x, y, tft.color565(0,80,200), tft.color565(0,120,255)); }
void drawIconFlat_Food46(int x, int y)      { drawWheelieBinFlat(x, y, tft.color565(40,150,60), tft.color565(60,190,90)); }
void drawIconFlat_Garden46(int x, int y)    { drawWheelieBinFlat(x, y, tft.color565(150,90,40), tft.color565(180,120,60)); }

/******************************************************************************
 * Flat Icon Dispatcher
 ******************************************************************************/
void drawBinIconFlat(const String& binType, int x, int y) {
  if (binType.equalsIgnoreCase("rubbish"))        drawIconFlat_Rubbish46(x,y);
  else if (binType.equalsIgnoreCase("recycling")) drawIconFlat_Recycling46(x,y);
  else if (binType.equalsIgnoreCase("food"))      drawIconFlat_Food46(x,y);
  else if (binType.equalsIgnoreCase("garden"))    drawIconFlat_Garden46(x,y);
}

/******************************************************************************
 * Banner & Border
 * Returns: 0 = none, 1 = yellow "TOMORROW!", 2 = red "PUT BINS OUT!"
 ******************************************************************************/
int bannerTypeForDate(const String& nextDate) {
  if (g_bannerOverride != -1) return g_bannerOverride;
  String tomorrow = tomorrowISO_UK();
  if (nextDate != tomorrow) return 0;
  struct tm lt{}; bool got = nowLocalUK(lt); int hour = got ? lt.tm_hour : 12;
  return (hour < 16) ? 1 : 2;
}

void drawBanner(int type) {
  if (type == 0) return;
  int w = tft.width();
  if (type == 1) {
    tft.fillRect(0, 0, w, 34, TFT_YELLOW);
    tft.setTextColor(TFT_BLACK, TFT_YELLOW);
    tft.setTextDatum(TC_DATUM);
    tft.drawString("TOMORROW", w/2, 10, 2);
  } else {
    tft.fillRect(0, 0, w, 34, TFT_RED);
    tft.setTextColor(TFT_WHITE, TFT_RED);
    tft.setTextDatum(TC_DATUM);
    tft.drawString("PUT BINS OUT", w/2, 10, 2);
  }
}

/******************************************************************************
 * Footer Rendering (Date/Time + ID)
 ******************************************************************************/
void drawFooter(const String& identifier) {
  struct tm lt{};
  if (nowLocalUK(lt)) {
    char buf[32];
    strftime(buf, sizeof(buf), "%a %d %b %H:%M", &lt);
    tft.setTextDatum(BC_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(buf, tft.width()/2, tft.height() - 22, 2);
  }
  if (identifier.length()) {
    tft.setTextDatum(BC_DATUM);
    tft.setTextColor(tft.color565(160,160,160), TFT_BLACK);
    tft.drawString("ID: " + identifier, tft.width()/2, tft.height() - 6, 1);
  }
}

/******************************************************************************
 * FINAL UI LAYOUT (46×46 Icons + Labels Close Underneath, Centered + Pulsing Red Alert)
 ******************************************************************************/
// --- Pulsing red for alert (helper stays same) ---
uint16_t pulsingRedColor(float intensity) {
  uint8_t r = 50 + (uint8_t)(intensity * 205);
  return tft.color565(r, 0, 0);
}

/******************************************************************************
 * FINAL UI LAYOUT (Adjusted 2px down, blinking border comes from loop now)
 ******************************************************************************/
void drawUI(const BinData& data, const String& nextDate, bool /*forceRedAlert*/) {
  int bType = bannerTypeForDate(nextDate);

  tft.fillScreen(TFT_BLACK);

  // Draw static banner/header
  drawBanner(bType);

  // Header text
  String dateHuman = fmtHumanDateUK_fromISO(nextDate);
  const int hasBanner = (bType != 0);
  const int topPad   = hasBanner ? 56 : 34;
  const int gapTitle = 22;
  const int gapRow   = 22;

  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Next collection", tft.width()/2, topPad, 2);
  tft.drawString(dateHuman,        tft.width()/2, topPad + gapTitle, 4);

  auto it = data.schedule.find(nextDate);
  if (it == data.schedule.end() || it->second.empty()) {
    tft.drawString("No collection", tft.width()/2, topPad + gapTitle + 60, 2);
    return;
  }

  std::vector<String> bins = it->second;
  int n = (int)bins.size(); if (n > 3) n = 3;

  // Icon row
  const int iconSize = 46;
  const int colGap   = 24;
  int totalWidth = n*iconSize + (n-1)*colGap;
  int startX = (tft.width() - totalWidth)/2;

  // Adjust icon vertical placement here:
bool ICONS_UP = true;  // <--- set to false if you prefer them lower
int iconTopY  = topPad + gapTitle + gapRow + (ICONS_UP ? 16 : 20);
int labelTopY = iconTopY + iconSize + 3; //4x gap between icons

  for (int i=0; i<n; ++i) {
    int x = startX + i*(iconSize + colGap);
    drawBinIconFlat(bins[i], x, iconTopY);
  }

  for (int i=0; i<n; ++i) {
    String label = prettyBin(bins[i]);
    int xCenter = startX + i*(iconSize + colGap) + iconSize/2;

    int approxCharW = 10;
    int rectW = label.length()*approxCharW + 14;
    int rectH = 18;

    tft.fillRoundRect(xCenter - rectW/2, labelTopY - rectH/2, rectW, rectH, 6, TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(label, xCenter, labelTopY, 2);
  }
}


/************** PART 4 / 4 — START ********************************************
 * Night mode, demo, test screens, refresh, serial, setup, loop
 *****************************************************************************/

void applyNightModeBacklight(const String& nextDate) {
  long secsToNext = secondsUntilDateStartLocal_UK(nextDate);
  bool within24h  = (secsToNext <= 24L*3600L) && (secsToNext >= 0);

  struct tm lt{}; bool got = nowLocalUK(lt);
  int hour = got ? lt.tm_hour : 12;

  bool inNightWindow = (hour >= NIGHT_START_HOUR && hour < NIGHT_END_HOUR);
  bool shouldDim = (inNightWindow && !within24h);
  setBacklight(!shouldDim);
}

/******************************************************************************
 * Demo Mode — Single Bins Normal -> Single Bins Alert
 ******************************************************************************/
void runDemo() {
  String today = todayISO_UK();
  std::vector<String> bins = {"rubbish", "recycling", "food", "garden"};

  // Phase 1: normal (no banner)
  g_bannerOverride = 0;
  for (auto& b : bins) {
    BinData temp = g_data; temp.schedule.clear(); temp.schedule[today] = { b };
    drawUI(temp, today, false); drawFooter(g_identifier); delay(500);
  }

  // Phase 2: alert (red)
  g_bannerOverride = 2;
  for (auto& b : bins) {
    BinData temp = g_data; temp.schedule.clear(); temp.schedule[today] = { b };
    drawUI(temp, today, false); drawFooter(g_identifier); delay(500);
  }
  g_bannerOverride = -1; // clear override
}

/******************************************************************************
 * Test Screens
 ******************************************************************************/
void showNetStatus() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  String ssid = WiFi.SSID();
  String ip   = WiFi.localIP().toString();
  long rssi   = WiFi.RSSI();
  tft.drawString("NET STATUS", 10, 10, 2);
  tft.drawString("SSID: " + ssid, 10, 34, 2);
  tft.drawString("IP:   " + ip,   10, 56, 2);
  tft.drawString("RSSI: " + String(rssi) + " dBm", 10, 78, 2);
}

void showJsonStatus() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.drawString("JSON STATUS", 10, 10, 2);
  tft.drawString("Last: " + g_lastFetchStatus, 10, 34, 2);

  if (g_lastFetchUtc != 0) {
    tm lt{}; localtime_uk(g_lastFetchUtc, &lt);
    char buf[32]; strftime(buf, sizeof(buf), "%a %d %b %H:%M", &lt);
    tft.drawString("When: " + String(buf), 10, 56, 2);
  } else {
    tft.drawString("When: (never)", 10, 56, 2);
  }
}

/******************************************************************************
 * Data Refresh (fetch + parse + update nextDate)
 ******************************************************************************/
bool refreshData(bool showErrors) {
  String payload;
  if (!fetchJson(payload) || payload.startsWith("ERR")) {
    if (showErrors) showCenteredTwoLines("Fetch FAILED", payload.c_str(), 2, TFT_RED);
    return false;
  }
  BinData newData;
  if (!parseSchedule(payload, newData)) {
    if (showErrors) showCentered("Parse FAILED!", 0, 2, TFT_RED);
    return false;
  }
  g_data = newData;
  String today = todayISO_UK();
  g_nextDate   = findNextDate(g_data.dates, today);
  return true;
}

/******************************************************************************
 * Serial Test Harness
 * Commands:
 *   TEST DEMO
 *   TEST NORMAL
 *   TEST YELLOW
 *   TEST RED
 *   TEST DATE YYYY-MM-DD
 *   TEST JSON
 *   TEST NET
 *   TEST REFRESH
 *   TEST ICONS A|C|D   (kept for future; currently flat icons only)
 ******************************************************************************/
void handleCommand(const String& line) {
  String cmd = line; cmd.trim();
  if (!cmd.length()) return;
  String u = cmd; u.toUpperCase();

  if (u == "TEST DEMO") {
    g_forceDemo = true; g_bannerOverride = -1; g_forceNormal = false;
    runDemo();
    g_forceDemo = false;
    drawUI(g_data, g_nextDate, false); drawFooter(g_identifier);
  }
  else if (u == "TEST NORMAL") {
    g_bannerOverride = -1; g_forceNormal = true; g_forceDemo = false;
    drawUI(g_data, g_nextDate, false); drawFooter(g_identifier);
  }
  else if (u == "TEST YELLOW") {
    g_bannerOverride = 1; g_forceNormal = false; g_forceDemo = false;
    drawUI(g_data, g_nextDate, false); drawFooter(g_identifier);
  }
  else if (u == "TEST RED") {
    g_bannerOverride = 2; g_forceNormal = false; g_forceDemo = false;
    drawUI(g_data, g_nextDate, false); drawFooter(g_identifier);
  }
  else if (u.startsWith("TEST DATE ")) {
    String date = cmd.substring(10); date.trim();
    tm t{};
    if (parseISODate(date, t)) {
      g_todayOverride = date;
      String today = todayISO_UK();
      g_nextDate = findNextDate(g_data.dates, today);
      g_bannerOverride = -1; g_forceDemo = false;
      drawUI(g_data, g_nextDate, false); drawFooter(g_identifier);
    } else {
      showCenteredTwoLines("Bad date", "Use YYYY-MM-DD");
    }
  }
  else if (u == "TEST JSON") {
    showJsonStatus();
  }
  else if (u == "TEST NET") {
    showNetStatus();
  }
  else if (u == "TEST REFRESH") {
    if (refreshData(true)) {
      showCentered("Refreshed", 0);
      drawUI(g_data, g_nextDate, false); drawFooter(g_identifier);
    }
  }
  else if (u.startsWith("TEST ICONS ")) {
    if (u.endsWith(" A")) g_iconStyle = 0;   // future hook
    else if (u.endsWith(" C")) g_iconStyle = 0;
    else if (u.endsWith(" D")) g_iconStyle = 0;
    drawUI(g_data, g_nextDate, false); drawFooter(g_identifier);
  }
  else {
    showCenteredTwoLines("Unknown cmd", cmd.c_str());
  }
}

void pollSerial() {
  static String buf;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (buf.length()) { handleCommand(buf); buf = ""; }
    } else {
      buf += c;
      if (buf.length() > 200) buf.remove(0); // safety
    }
  }
}

/******************************************************************************
 * Setup
 ******************************************************************************/
void setup() {
  Serial.begin(115200);

  setBacklight(true);

  tft.init(); tft.setRotation(0);
  tft.fillScreen(TFT_BLACK);
  showCentered("Bin Tracker", -20);
  showCentered("Initialising...", 10);

  // WiFi connect
  showCentered("Connecting WiFi...", 0);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 40) { delay(500); tries++; }
  if (WiFi.status() != WL_CONNECTED) { showCentered("WiFi FAILED!", 0, 2, TFT_RED); return; }
  showCenteredTwoLines("WiFi OK", WiFi.localIP().toString().c_str());

  // NTP sync
  showCentered("Syncing Time...", 0);
  if (!ntpSyncOnce()) { showCentered("Time FAILED!", 0, 2, TFT_RED); }
  else {
    struct tm lt{}; nowLocalUK(lt); char buf[16];
    strftime(buf, sizeof(buf), "%H:%M", &lt);
    showCenteredTwoLines("Time OK", buf);
  }

  // JSON (startup)
  showCentered("Fetching Schedule...", 0);
  if (!refreshData(true)) return;
  showCentered("Fetch OK", 0);

  // Demo (quick)
  runDemo();

  // Draw real
  drawUI(g_data, g_nextDate, false);
  drawFooter(g_identifier);
  applyNightModeBacklight(g_nextDate);
}

/******************************************************************************
 * Loop
 ******************************************************************************/
void loop() {
  pollSerial();

  static uint32_t lastMinute = 0;
  static uint32_t lastColonBlink = 0;
  static bool colonVisible = true;

  uint32_t nowMs = millis();

  // ✅ Update footer clock colon every 500ms
  if (nowMs - lastColonBlink > 500) {
    colonVisible = !colonVisible;
    lastColonBlink = nowMs;

    // Redraw footer with colon toggled
    struct tm lt{};
    if (nowLocalUK(lt)) {
      char buf[32];
      strftime(buf, sizeof(buf), colonVisible ? "%a %d %b %H:%M" : "%a %d %b %H %M", &lt);
      tft.setTextDatum(BC_DATUM);
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.drawString(buf, tft.width()/2, tft.height() - 22, 2);
    }
  }

  // ✅ Pulsing red border in alert mode
  if (bannerTypeForDate(g_nextDate) == 2) {
    float t = (millis() % 4000) / 2000.0;
    float intensity = (t < 1.0) ? t : (2.0 - t);
    uint16_t borderCol = pulsingRedColor(intensity);

    int cx = tft.width()/2, cy = tft.height()/2;
    for (int i=0;i<3;i++) tft.drawCircle(cx, cy, min(cx, cy) - 2 - i, borderCol);
  }

  // ✅ Once-per-minute updates
  if (nowMs - lastMinute > 60000) {
    lastMinute = nowMs;

    if (millis() - g_lastFetchMillis >= 3600000UL) {
      if (refreshData(false)) {
        drawUI(g_data, g_nextDate, false);
        drawFooter(g_identifier);
      }
    }

    if (g_nextDate.length() > 0) {
      applyNightModeBacklight(g_nextDate);
      drawFooter(g_identifier);
    }
  }
}

/************** PART 4 / 4 — END **********************************************/
