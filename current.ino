/*=== PART 1/3: Includes + Globals + Setup ==================================*/
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include "garbage_truck.h"
#include <time.h>
#include <map>
#include <vector>
#include <algorithm>
#include <math.h>

// ---- WIFI ----
static const char* WIFI_SSID = "Plumbing";
static const char* WIFI_PASS = "2hphdwdFwkpr";

// ---- JSON host/path ----
// https://raw.githubusercontent.com/e77/bin-tracker/main/15b.json
static const char* JSON_HOST = "raw.githubusercontent.com";
static const char* JSON_PATH = "/e77/bin-tracker/main/15b.json";

// ---- Retry intervals ----
static const unsigned long HOURLY_REFRESH_MS = 60UL * 60UL * 1000UL;  // 1 hour

TFT_eSPI tft;

// ---- Data model ----
struct BinData {
    String lastUpdated;                 // optional
    String today;                       // optional (we default to device date)

    std::vector<String> dates;          // all dates, sorted asc
    std::map<String, std::vector<String>> schedule; // date -> list of stream names

    // Optional per-stream lists (for debugging / future use)
    std::vector<String> refuseDates;
    std::vector<String> recyclingDates;
    std::vector<String> gardenDates;
    std::vector<String> foodDates;
};

BinData g_data;
String  g_nextDate;           // next upcoming collection date, ISO "YYYY-MM-DD"
String  g_identifier = "15B"; // footer ID label

unsigned long g_lastFetchMillis   = 0;
unsigned long g_lastFetchAttempt  = 0;
bool          g_bootTriedInitialFetch = false;

// ---- Forward declarations ----
bool   nowLocalUK(struct tm& out);
time_t nowUtc();
String todayISO_UK();
String fmtHumanDateUK_fromISO(const String& iso);
int    daysBetweenISO(const String& d1, const String& d2);
String pickNextDateFrom(const std::vector<String>& sortedDates, const String& todayISO);
bool   parseScheduleFromJson(const String& payload, BinData& out);

void   drawUI(const BinData& data, const String& nextDate, bool forceRedAlert=false);
void   drawFooter(const String& idTag);
void   pollSerial();
bool   refreshDataRaw(bool verbose);
uint16_t pulsingRedColor(float intensity);
int    bannerTypeForDate(const String& nextDateISO);
void   drawBanner(int type);
String prettyBin(const String& s);
void   drawBinIconFlat(const String& binName, int x, int y);

// ---- Small helpers ----
static void showCenteredSafe(const String& msg, uint16_t color=TFT_WHITE) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(color, TFT_BLACK);
  tft.drawString(msg, tft.width()/2, tft.height()/2, 2);
}

bool nowLocalUK(struct tm& out) {
  time_t nowSec = time(nullptr);
  if (nowSec < 1700000000) return false;
  struct tm tmp;
  localtime_r(&nowSec, &tmp);
  out = tmp;
  return true;
}

time_t nowUtc() {
  return time(nullptr);
}

String todayISO_UK() {
  struct tm lt{};
  if (!nowLocalUK(lt)) return "";
  char buf[11];
  strftime(buf,sizeof(buf),"%Y-%m-%d",&lt);
  return String(buf);
}

int daysBetweenISO(const String& d1, const String& d2) {
  if (d1.length()<10 || d2.length()<10) return 0;
  struct tm t1{}, t2{};
  t1.tm_year = d1.substring(0,4).toInt() - 1900;
  t1.tm_mon  = d1.substring(5,7).toInt() - 1;
  t1.tm_mday = d1.substring(8,10).toInt();
  t2.tm_year = d2.substring(0,4).toInt() - 1900;
  t2.tm_mon  = d2.substring(5,7).toInt() - 1;
  t2.tm_mday = d2.substring(8,10).toInt();
  time_t tt1 = mktime(&t1);
  time_t tt2 = mktime(&t2);
  if (tt1<=0 || tt2<=0) return 0;
  long diff = (long)((tt1 - tt2) / 86400);
  return (int)diff;
}

String pickNextDateFrom(const std::vector<String>& sortedDates, const String& todayISO) {
    if (sortedDates.empty()) return "";
    String best = "";
    for (const auto& d : sortedDates) {
        if (d >= todayISO) {
            if (best == "" || d < best) best = d;
        }
    }
    if (best == "" && !sortedDates.empty()) best = sortedDates.front();
    return best;
}

String fmtHumanDateUK_fromISO(const String& iso) {
  if (iso.length()<10) return iso;
  struct tm t{};
  t.tm_year = iso.substring(0,4).toInt()-1900;
  t.tm_mon  = iso.substring(5,7).toInt()-1;
  t.tm_mday = iso.substring(8,10).toInt();
  time_t tt = mktime(&t);
  if (tt <= 0) return iso;
  char buf[32];
  strftime(buf,sizeof(buf),"%a %d %b",&t);
  return String(buf);
}

// ---- JSON parsing for your date-keyed schedule ----
bool parseScheduleFromJson(const String& payload, BinData& out) {
    StaticJsonDocument<8192> doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (err) {
        Serial.print("[ERROR] JSON parse failed: ");
        Serial.println(err.c_str());
        return false;
    }

    JsonObjectConst root = doc.as<JsonObjectConst>();

    // identifier -> use in footer if present
    if (root["identifier"].is<const char*>()) {
        g_identifier = (const char*)root["identifier"];
    }

    if (root["lastUpdated"].is<const char*>()) {
        out.lastUpdated = (const char*)root["lastUpdated"];
    } else {
        out.lastUpdated = "";
    }

    if (root["today"].is<const char*>()) {
        out.today = (const char*)root["today"];
    } else {
        out.today = todayISO_UK();
    }

    out.dates.clear();
    out.schedule.clear();
    out.refuseDates.clear();
    out.recyclingDates.clear();
    out.gardenDates.clear();
    out.foodDates.clear();

    // Preferred: date-keyed schedule
    JsonObjectConst sched = root["schedule"].as<JsonObjectConst>();
    if (!sched.isNull()) {
        for (JsonPairConst kv : sched) {
            const char* dateKey = kv.key().c_str();
            if (!dateKey) continue;
            String dateStr = String(dateKey);

            out.dates.push_back(dateStr);
            std::vector<String>& streams = out.schedule[dateStr];

            JsonArrayConst arr = kv.value().as<JsonArrayConst>();
            if (!arr.isNull()) {
                for (JsonVariantConst v : arr) {
                    if (!v.is<const char*>()) continue;
                    String s = String((const char*)v);
                    streams.push_back(s);

                    String lower = s; lower.toLowerCase();
                    if (lower.indexOf("refuse")>=0 || lower.indexOf("rubbish")>=0 || lower.indexOf("landfill")>=0) {
                        out.refuseDates.push_back(dateStr);
                    } else if (lower.indexOf("recycl")>=0) {
                        out.recyclingDates.push_back(dateStr);
                    } else if (lower.indexOf("garden")>=0) {
                        out.gardenDates.push_back(dateStr);
                    } else if (lower.indexOf("food")>=0) {
                        out.foodDates.push_back(dateStr);
                    }
                }
            }
        }
    }

    // sort & dedupe
    auto uniqSort = [](std::vector<String>& v) {
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
    };

    uniqSort(out.dates);
    uniqSort(out.refuseDates);
    uniqSort(out.recyclingDates);
    uniqSort(out.gardenDates);
    uniqSort(out.foodDates);

    for (auto& kv : out.schedule) {
        auto& v = kv.second;
        uniqSort(v);
    }

    Serial.print("[JSON] Parsed schedule, dates=");
    Serial.println((int)out.dates.size());
    return true;
}

// ---- NTP/time init ----
bool ntpSyncOnce() {
  configTzTime("GMT0BST,M3.5.0/1,M10.5.0/2",
               "pool.ntp.org", "time.google.com", "time.cloudflare.com");
  for (int i=0;i<10;i++) {
    delay(300);
    time_t t = time(nullptr);
    if (t > 1700000000) return true;
  }
  return false;
}

// ---- Setup ----
void setup() {
  Serial.begin(115200);
  delay(100);

  tft.init();
  tft.setRotation(0);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(1);
  tft.setTextDatum(MC_DATUM);

  showCenteredSafe("POWER ON", TFT_WHITE); delay(250);
  showCenteredSafe("Connecting WiFi...", TFT_WHITE);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(250);
  }

  if (WiFi.status() == WL_CONNECTED) {
    showCenteredSafe("WiFi OK", TFT_GREEN); delay(250);
  } else {
    showCenteredSafe("WiFi FAIL", TFT_YELLOW); delay(400);
  }

  showCenteredSafe("Syncing time...", TFT_WHITE);
  if (ntpSyncOnce()) {
    showCenteredSafe("Time OK", TFT_GREEN); delay(250);
  } else {
    showCenteredSafe("Time FAIL", TFT_YELLOW); delay(400);
  }

  g_bootTriedInitialFetch = false;
}
/*=== END PART 1/3 ===========================================================*/

/*=== PART 2/3: Networking + UI helpers =====================================*/

// ---- Networking ----
String fetchJsonHttpsInsecure(const char* host, const char* path) {
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(15000);

  const int httpsPort = 443;
  if (!client.connect(host, httpsPort)) {
    Serial.println("[ERROR] HTTPS connect failed");
    return "";
  }

  client.print(String("GET ") + path + " HTTP/1.1\r\n" +
               "Host: " + host + "\r\n" +
               "User-Agent: ESP32S3-BinTracker/1.0\r\n" +
               "Connection: close\r\n\r\n");

  String statusLine = client.readStringUntil('\n');
  statusLine.trim();
  Serial.print("[HTTPS] "); Serial.println(statusLine);

  // skip headers
  while (client.connected()) {
    String h = client.readStringUntil('\n');
    if (h == "\r" || h.length() == 0) break;
  }

  String payload;
  while (client.available()) {
    payload += client.readString();
  }
  client.stop();
  payload.trim();
  Serial.printf("[INFO] HTTPS Fetch OK, %u bytes\n", (unsigned)payload.length());
  return payload;
}

String fetchJsonRawTCP(const char* host, const char* path) {
  WiFiClient client;
  const int httpPort = 80;

  if (!client.connect(host, httpPort)) {
    Serial.println("[ERROR] HTTP connect failed");
    return "";
  }

  client.print(String("GET ") + path + " HTTP/1.1\r\n" +
               "Host: " + host + "\r\n" +
               "User-Agent: ESP32S3-BinTracker/1.0\r\n" +
               "Connection: close\r\n\r\n");

  String statusLine = client.readStringUntil('\n');
  statusLine.trim();
  Serial.print("[HTTP] "); Serial.println(statusLine);

  bool redirected = statusLine.startsWith("HTTP/1.1 301") ||
                    statusLine.startsWith("HTTP/1.1 302");

  // skip headers
  while (client.connected()) {
    String h = client.readStringUntil('\n');
    if (h == "\r" || h.length() == 0) break;
  }

  String payload;
  while (client.available()) {
    payload += client.readString();
  }
  client.stop();
  payload.trim();
  Serial.printf("[INFO] HTTP Fetch got %u bytes\n", (unsigned)payload.length());

  if (payload.length() == 0 || redirected) {
    Serial.println("[WARN] HTTP empty/redirect → HTTPS fallback");
    return fetchJsonHttpsInsecure(host, path);
  }

  return payload;
}

bool refreshDataRaw(bool verbose) {
  if (verbose) showCenteredSafe("Fetching JSON...", TFT_WHITE);
  String payload = fetchJsonRawTCP(JSON_HOST, JSON_PATH);
  if (payload.length()==0) {
    if (verbose) showCenteredSafe("Fetch failed", TFT_YELLOW);
    return false;
  }

  // Trim to first '{' just in case
  int bracePos = payload.indexOf('{');
  if (bracePos > 0) payload = payload.substring(bracePos);

  BinData tmp;
  if (!parseScheduleFromJson(payload, tmp)) {
    if (verbose) showCenteredSafe("Parse failed", TFT_RED);
    return false;
  }

  String today = todayISO_UK();
  String nextD = pickNextDateFrom(tmp.dates, today);

  g_data = tmp;
  g_nextDate = nextD;
  g_lastFetchMillis = millis();

  if (verbose) {
    showCenteredSafe("Schedule updated", TFT_GREEN);
  }
  Serial.printf("[INFO] Next date: %s (today=%s)\n", g_nextDate.c_str(), today.c_str());
  return true;
}

// ---- UI helpers ----
uint16_t pulsingRedColor(float intensity) {
  uint8_t r = 80 + (uint8_t)(intensity * 175);
  return tft.color565(r, 0, 0);
}

String prettyBin(const String& s) {
  if (s.equalsIgnoreCase("rubbish"))   return "RUBBISH";
  if (s.equalsIgnoreCase("refuse"))    return "REFUSE";
  if (s.equalsIgnoreCase("recycling")) return "RECYCLING";
  if (s.equalsIgnoreCase("garden"))    return "GARDEN";
  if (s.equalsIgnoreCase("food"))      return "FOOD";
  return s;
}

// Proper little wheelie bin with lid & wheels
void drawBinIconFlat(const String& binName, int x, int y) {
  String lower = binName;
  lower.toLowerCase();

  uint16_t bodyCol;
  if (lower.indexOf("refuse")>=0 || lower.indexOf("rubbish")>=0 || lower.indexOf("landfill")>=0) {
    bodyCol = tft.color565(20, 20, 20);      // near-black
  } else if (lower.indexOf("recycl")>=0) {
    bodyCol = tft.color565(0, 0, 200);       // blue
  } else if (lower.indexOf("garden")>=0) {
    bodyCol = tft.color565(110, 60, 10);     // brown
  } else if (lower.indexOf("food")>=0) {
    bodyCol = tft.color565(0, 100, 0);       // dark green
  } else {
    bodyCol = TFT_WHITE;
  }

  const int w  = 40;
  const int h  = 46;
  const int lidH = 8;
  const int wheelR = 3;

  // main body
  tft.fillRoundRect(x, y+lidH, w, h-lidH-4, 5, bodyCol);
  tft.drawRoundRect(x, y+lidH, w, h-lidH-4, 5, TFT_BLACK);

  // lid slightly wider
  int lidX = x - 2;
  int lidW = w + 4;
  tft.fillRect(lidX, y, lidW, lidH, bodyCol);
  tft.drawRect(lidX, y, lidW, lidH, TFT_BLACK);

  // handle line
  tft.drawFastHLine(lidX + 6, y+2, lidW-12, TFT_BLACK);

  // wheels
  int wheelY = y + h - wheelR;
  tft.fillCircle(x + 6, wheelY, wheelR, TFT_DARKGREY);
  tft.fillCircle(x + w - 6, wheelY, wheelR, TFT_DARKGREY);
}

// 0 = no upcoming / blue, 1 = yellow, 2 = red
int bannerTypeForDate(const String& nextDateISO) {
  if (nextDateISO.length() < 10) return 0;
  struct tm lt{};
  if (!nowLocalUK(lt)) return 0;

  String today = todayISO_UK();
  int diffNext = daysBetweenISO(nextDateISO, today); // next - today
  int hour = lt.tm_hour;

  if (diffNext > 1) return 0;   // far future → treat as blue
  if (diffNext == 1) {
    if (hour < 15) return 1;    // day before, before 3pm → yellow
    return 2;                   // day before, after 3pm → red
  }
  if (diffNext == 0) return 2;  // today → red

  return 0;                     // past → blue/none
}

// Simple vector truck icon so we don't depend on a bitmap format
void drawTruckIcon(int cx, int cy) {
  const int w  = 80;   // overall width
  const int h  = 40;   // overall height

  int x0 = cx - w/2;
  int y0 = cy - h/2;

  // Trailer (bin)
  int trailerW = 48;
  int trailerH = 24;
  int trailerX = x0;
  int trailerY = y0 + 4;
  uint16_t trailerCol = tft.color565(0, 120, 0);   // green body
  tft.fillRoundRect(trailerX, trailerY, trailerW, trailerH, 4, trailerCol);
  tft.drawRoundRect(trailerX, trailerY, trailerW, trailerH, 4, TFT_BLACK);

  // Cab
  int cabW = 22;
  int cabH = 20;
  int cabX = trailerX + trailerW;
  int cabY = y0 + 8;
  uint16_t cabCol = tft.color565(200, 200, 200);
  tft.fillRoundRect(cabX, cabY, cabW, cabH, 4, cabCol);
  tft.drawRoundRect(cabX, cabY, cabW, cabH, 4, TFT_BLACK);

  // Window
  int winW = 10;
  int winH = 10;
  int winX = cabX + 4;
  int winY = cabY + 3;
  tft.fillRect(winX, winY, winW, winH, TFT_CYAN);
  tft.drawRect(winX, winY, winW, winH, TFT_BLACK);

  // Divider between cab and trailer
  tft.drawFastVLine(trailerX + trailerW, trailerY, trailerH, TFT_BLACK);

  // Ground line (optional)
  int groundY = trailerY + trailerH + 6;

  // Wheels
  int wheelR = 5;
  int wheelY = groundY;
  int wheel1X = trailerX + 10;
  int wheel2X = trailerX + trailerW - 10;
  int wheel3X = cabX + cabW/2;

  uint16_t tyreCol = TFT_BLACK;
  uint16_t hubCol  = TFT_DARKGREY;
  tft.fillCircle(wheel1X, wheelY, wheelR, tyreCol);
  tft.fillCircle(wheel2X, wheelY, wheelR, tyreCol);
  tft.fillCircle(wheel3X, wheelY, wheelR, tyreCol);
  tft.fillCircle(wheel1X, wheelY, wheelR-2, hubCol);
  tft.fillCircle(wheel2X, wheelY, wheelR-2, hubCol);
  tft.fillCircle(wheel3X, wheelY, wheelR-2, hubCol);
}



void drawBanner(int type) {
  if (type == 0) return;
  uint16_t bg = (type==1)?TFT_YELLOW:tft.color565(220,30,30);
  tft.fillRect(0,0,tft.width(),34,bg);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor((type==1)?TFT_BLACK:TFT_WHITE, bg);
  tft.drawString((type==1)?"TOMORROW!":"PUT BINS OUT!", tft.width()/2, 10, 2);
}

void drawFooter(const String& idTag) {
  int footerH = 18;
  int footerY = tft.height() - footerH - 16;
  tft.setTextDatum(BC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(idTag, tft.width()/2, footerY - 1, 1);
}

void drawTruckTest() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Truck test", tft.width()/2, 18, 2);

  int truckX = (tft.width()  - garbageTruckWidth)  / 2;
  int truckY = (tft.height() - garbageTruckHeight) / 2;
   int cx = tft.width()  / 2;
    int cy = tft.height() / 2;
    drawTruckIcon(cx, cy);
}



void drawUI(const BinData& data, const String& nextDate, bool /*forceRedAlert*/) {
  tft.fillScreen(TFT_BLACK);

  String todayISO = todayISO_UK();
  struct tm lt{};
  bool haveTime = nowLocalUK(lt);
  int hour = haveTime ? lt.tm_hour : 0;

  // ---- Determine previous collection date ----
  String prevDate = "";
  for (const auto& d : data.dates) {
    if (d < todayISO && (prevDate=="" || d > prevDate)) {
      prevDate = d;
    }
  }
  int diffPrev = (prevDate.length()>=10) ? daysBetweenISO(todayISO, prevDate) : 0;
  bool showBringBack = (nextDate == todayISO && hour >= 6);

  // ---- If no nextDate, show "no collection" state ----
  if (!showBringBack && nextDate.length() < 10) {
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("No collections due", tft.width()/2, tft.height()/2 - 8, 2);
    tft.drawString("Check schedule",     tft.width()/2, tft.height()/2 + 10, 2);
    return;
  }

  // ---- Bring-back screen (day after collection, after 06:00) ----
  if (showBringBack) {
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("Bring bins back in", tft.width()/2, 18, 2);

    int truckX = (tft.width()  - garbageTruckWidth)  / 2;
    int truckY = (tft.height() - garbageTruckHeight) / 2;
   int cx = tft.width()  / 2;
    int cy = tft.height() / 2;
    drawTruckIcon(cx, cy);

    tft.setTextDatum(BC_DATUM);
    tft.drawString("Please return to storage", tft.width()/2, tft.height()-30, 2);
    return;
  }

  // ---- Normal "next collection" screen ----
  int bType = bannerTypeForDate(nextDate);
  if (bType == 2) {
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(TFT_BLACK, TFT_RED);
    tft.fillRect(0, 0, tft.width(), 32, TFT_RED);
    tft.drawString("PUT BINS OUT", tft.width()/2, 16, 2);
  } else if (bType == 1) {
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(TFT_BLACK, TFT_YELLOW);
    tft.fillRect(0, 0, tft.width(), 32, TFT_YELLOW);
    tft.drawString("Bins out tomorrow", tft.width()/2, 16, 2);
  }

  const int hasBanner = (bType != 0);
  const int topPad   = hasBanner ? 60 : 36;
  const int gapTitle = 24;
  const int gapRow   = 22;

  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Next collection", tft.width()/2, topPad, 2);
  tft.drawString(fmtHumanDateUK_fromISO(nextDate), tft.width()/2, topPad+gapTitle, 4);

  // Look up bins for this date
  auto it = data.schedule.find(nextDate);
  if (it == data.schedule.end() || it->second.empty()) {
    String nextReal = pickNextDateFrom(data.dates, nextDate);
    String nextHuman = fmtHumanDateUK_fromISO(nextReal);

    String msg = "NO COLLECTION";
    int rectW = msg.length()*10 + 14;
    int rectH = 20;
    int yMid = topPad + gapTitle + 50;

    tft.fillRoundRect(tft.width()/2 - rectW/2, yMid - rectH/2, rectW, rectH, 6, TFT_YELLOW);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_BLACK, TFT_YELLOW);
    tft.drawString(msg, tft.width()/2, yMid, 2);

    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("NEXT: " + nextHuman, tft.width()/2, yMid + 26, 2);
    return;
  }

  // Draw up to 3 bins
  std::vector<String> bins = it->second;
  int n = (int)bins.size(); if (n>3) n=3;

  const int iconW = 40;
  const int iconH = 46;
  const int colGap = 24;
  int totalWidth = n*iconW + (n-1)*colGap;
  int startX = (tft.width() - totalWidth)/2;

  int iconTopY  = topPad + gapTitle + gapRow + 16;
  int labelTopY = iconTopY + iconH - 6;

  for (int i=0;i<n;++i) {
    int x = startX + i*(iconW+colGap);
    drawBinIconFlat(bins[i], x, iconTopY);

    String label = prettyBin(bins[i]);
    int xCenter = x + iconW/2;
    int rectW = label.length()*10 + 14;
    int rectH = 18;
    tft.fillRoundRect(xCenter - rectW/2, labelTopY - rectH/2, rectW, rectH, 6, TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(label, xCenter, labelTopY, 2);
  }
}
/*=== END PART 2/3 ===========================================================*/

/*=== PART 3/3: Serial commands + loop ======================================*/

void pollSerial() {
  static String cmd;
  while (Serial.available()) {
    char c = Serial.read();
    if (c=='\n' || c=='\r') {
      cmd.trim();
      if (cmd.length()) {
        Serial.printf("[CMD] %s\n", cmd.c_str());
        if (cmd.equalsIgnoreCase("TEST FETCH")) {
          showCenteredSafe("Manual fetch...", TFT_WHITE);
          if (refreshDataRaw(true)) {
            drawUI(g_data, g_nextDate, false);
            drawFooter(g_identifier);
          }
        } else if (cmd.equalsIgnoreCase("TEST BLUE")) {
          // Simulate "no upcoming collection" state → blue ring
          g_nextDate = "";
          drawUI(g_data, g_nextDate, false);
          drawFooter(g_identifier);
        } else if (cmd.equalsIgnoreCase("TEST YELLOW")) {
          // Force nextDate to tomorrow
          time_t t = time(nullptr) + 24*3600;
          struct tm tm2{}; localtime_r(&t,&tm2);
          char buf[11]; strftime(buf,sizeof(buf),"%Y-%m-%d",&tm2);
          g_nextDate = String(buf);
          drawUI(g_data, g_nextDate, false);
          drawFooter(g_identifier);
        } else if (cmd.equalsIgnoreCase("TEST RED")) {
          // Force nextDate to today
          g_nextDate = todayISO_UK();
          drawUI(g_data, g_nextDate, true);
          drawFooter(g_identifier);

               } else if (cmd.equalsIgnoreCase("TEST TRUCK")) {
          // Show truck screen regardless of date/time logic
          drawTruckTest();
          drawFooter(g_identifier);



        } else if (cmd.equalsIgnoreCase("TEST NORMAL")) {
          // Return to real schedule (exit test mode)
          showCenteredSafe("Refreshing...", TFT_WHITE);
          if (refreshDataRaw(true)) {
            drawUI(g_data, g_nextDate, false);
            drawFooter(g_identifier);
          }
        } else {
          Serial.println("[CMD] Unknown");
        }
      }
      cmd="";
    } else {
      cmd += c;
    }
  }
}

void loop() {
  pollSerial();

  static uint32_t lastTick1s = 0;
  static uint32_t lastHourlyCheck = 0;
  uint32_t nowMs = millis();

  // Boot-time first fetch
  if (!g_bootTriedInitialFetch) {
    g_bootTriedInitialFetch = true;
    showCenteredSafe("Fetching JSON...", TFT_WHITE);
    g_lastFetchAttempt = nowMs;
    if (refreshDataRaw(true)) {
      drawUI(g_data, g_nextDate, false);
      drawFooter(g_identifier);
    } else {
      showCenteredSafe("Fetch failed\nRetry in 1h", TFT_YELLOW);
    }
  }

    // 1-second tick: clock only
  if (nowMs - lastTick1s > 1000) {
    lastTick1s = nowMs;

    struct tm lt{};
    if (nowLocalUK(lt)) {
      char buf[32];
      strftime(buf,sizeof(buf), "%a %d %b %H:%M:%S", &lt);

      int footerH = 18;
      int footerY = tft.height() - footerH - 16;

      tft.setTextDatum(BC_DATUM);
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.fillRect(0, footerY, tft.width(), footerH, TFT_BLACK);
      tft.drawString(String(buf), tft.width()/2, footerY + footerH - 3, 1);
    }
  }

  // ---- STATUS RING (imminent only, smooth breathing) ----
  static uint32_t lastRingUpdate = 0;
  if (nowMs - lastRingUpdate > 80) {   // update ~12.5 times/sec
    lastRingUpdate = nowMs;

    String todayISO = todayISO_UK();
    struct tm lt2{};
    bool haveTime = nowLocalUK(lt2);
    int hour = haveTime ? lt2.tm_hour : 0;

    bool haveUpcoming = (g_nextDate.length() >= 10);

    // Truck state = collection today after 06:00
    bool bringBackState = (haveUpcoming && g_nextDate == todayISO && hour >= 6);

    int bType = bannerTypeForDate(g_nextDate);
    uint16_t ringColor = TFT_BLUE;
    bool drawRing = true;

    if (bringBackState) {
      ringColor = TFT_CYAN;
    } else if (bType == 1) {
      ringColor = TFT_YELLOW;
    } else if (bType == 2) {
      float phase = (millis()%1200)/1200.0f;
      float intensity = 0.5f + 0.5f*sinf(phase*2.0f*3.14159f);
      ringColor = pulsingRedColor(intensity);  // breathing red
    } else {
      if (!haveUpcoming) {
        ringColor = TFT_BLUE;      // TEST BLUE / no upcoming date
      } else {
        drawRing = false;          // next collection is far away → no ring
      }
    }

    if (drawRing) {
      int cx = tft.width()/2;
      int cy = tft.height()/2;
      int rOuter = min(tft.width(), tft.height())/2 - 2;
      int thickness = 3;
      for (int i=0;i<thickness;i++) {
        tft.drawCircle(cx, cy, rOuter - i, ringColor);
      }
    }
  }


  // Hourly JSON refresh
  if (nowMs - lastHourlyCheck > HOURLY_REFRESH_MS) {
    lastHourlyCheck = nowMs;
    Serial.println("[LOOP] Hourly refresh...");
    g_lastFetchAttempt = nowMs;
    if (refreshDataRaw(false)) {
      drawUI(g_data, g_nextDate, false);
      drawFooter(g_identifier);
    } else {
      Serial.println("[LOOP] Refresh failed");
    }
  }
}

/*=== END PART 3/3 ===========================================================*/
