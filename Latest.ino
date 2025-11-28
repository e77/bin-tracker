/*=== PART 1/4: Includes + Globals + Setup ==================================*/
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include "garbage_truck.h"   // <- you confirmed filename = A (garbage_truck.h)
#include <time.h>
#include <map>
#include <vector>
#include <algorithm>


// ---- WIFI (update if you change credentials) ----
static const char* WIFI_SSID = "";
static const char* WIFI_PASS = "";

// ---- JSON host/path (raw TCP HTTP via jsDelivr; no TLS) ----
static const char* JSON_HOST = "raw.githubusercontent.com";
static const char* JSON_PATH = "/e77/bin-tracker/main/15b.json";

// ---- Retry intervals ----
static const unsigned long JSON_RETRY_INTERVAL_MS = 15UL * 60UL * 1000UL; // 15 minutes
static const unsigned long HOURLY_REFRESH_MS = 60UL * 60UL * 1000UL;      // 1 hour

// ---- Globals ----
TFT_eSPI tft;

struct BinData {
  // date -> bins
  std::map<String, std::vector<String>> schedule;
  std::vector<String> dates;
};

BinData g_data;
String  g_nextDate;           // ISO "YYYY-MM-DD"
String  g_identifier = "15B"; // footer ID label

unsigned long g_lastFetchMillis = 0;
unsigned long g_lastFetchAttempt = 0;
bool          g_bootTriedInitialFetch = false;

// ---- Forward declarations ----
bool nowLocalUK(struct tm& out);
time_t nowUtc();
String todayISO_UK();
void drawUI(const BinData& data, const String& nextDate, bool forceRedAlert=false);
void drawFooter(const String& idTag);
void pollSerial();


bool refreshDataRaw(bool verbose);
uint16_t pulsingRedColor(float intensity);
int bannerTypeForDate(const String& nextDateISO);
void drawBanner(int type);
String fmtHumanDateUK_fromISO(const String& iso);
String pickNextDateFrom(const std::vector<String>& sortedDates,
                        const String& todayISO);
bool parseScheduleFromJson(const String& payload, BinData& out);




String pickNextDateFrom(const std::vector<String>& sortedDates,
                        const String& todayISO) {
    if (sortedDates.empty()) {
        return "";
    }

    String best = "";
    for (const auto& d : sortedDates) {
        if (d >= todayISO) {
            if (best == "" || d < best) {
                best = d;
            }
        }
    }

    // If no date is >= today, just return the first one
    if (best == "" && !sortedDates.empty()) {
        best = sortedDates.front();
    }

    return best;
}

bool parseScheduleFromJson(const String& payload, BinData& out) {
    StaticJsonDocument<8192> doc;   // plenty for your JSON size

    DeserializationError err = deserializeJson(doc, payload);
    if (err) {
        Serial.print("[ERROR] JSON parse failed: ");
        Serial.println(err.c_str());
        return false;
    }

    JsonObject root = doc.as<JsonObject>();

    // ---- Basic fields ----
    if (root.containsKey("lastUpdated"))
        out.lastUpdated = (const char*)root["lastUpdated"];

    if (root.containsKey("today"))
        out.today = (const char*)root["today"];

    // ---- Helper Lambda for arrays ----
    auto loadArray = [&](std::vector<String>& target, JsonArrayConst arr) {
        target.clear();
        for (JsonVariantConst v : arr) {
            if (v.is<const char*>()) {
                target.push_back(String((const char*)v));
            }
        }
    };

    // ---- Load each waste stream (only if present) ----
    if (root.containsKey("refuse"))
        loadArray(out.refuseDates, root["refuse"].as<JsonArrayConst>());

    if (root.containsKey("recycling"))
        loadArray(out.recyclingDates, root["recycling"].as<JsonArrayConst>());

    if (root.containsKey("garden"))
        loadArray(out.gardenDates, root["garden"].as<JsonArrayConst>());

    if (root.containsKey("food"))
        loadArray(out.foodDates, root["food"].as<JsonArrayConst>());

    return true;
}



// ---- Boot helper (safe centered text) ----
static void showCenteredSafe(const String& msg, uint16_t color=TFT_WHITE) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(color, TFT_BLACK);
  tft.drawString(msg, tft.width()/2, tft.height()/2, 2);
}

// ---- NTP/time ----
bool ntpSyncOnce() {
  configTzTime("GMT0BST,M3.5.0/1,M10.5.0/2", "pool.ntp.org", "time.google.com", "time.cloudflare.com");
  for (int i=0;i<10;i++) {
    delay(300);
    time_t t = time(nullptr);
    if (t > 1700000000) return true; // synced
  }
  return false;
}

// ---- Setup ----
void setup() {
  Serial.begin(115200);
  delay(50);

  tft.init();
  tft.setRotation(0);
  tft.fillScreen(TFT_BLACK);

  showCenteredSafe("POWER ON", TFT_WHITE); delay(300);
  showCenteredSafe("Connecting WiFi...", TFT_WHITE);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis()-start < 15000) {
    delay(200);
  }
  if (WiFi.status() == WL_CONNECTED) {
    showCenteredSafe("WiFi OK", TFT_GREEN); delay(300);
  } else {
    showCenteredSafe("WiFi FAILED", TFT_RED); delay(600);
  }

  showCenteredSafe("Syncing time...", TFT_WHITE);
  if (ntpSyncOnce()) {
    showCenteredSafe("Time OK", TFT_GREEN); delay(250);
  } else {
    showCenteredSafe("Time FAIL", TFT_YELLOW); delay(400);
  }

  // first fetch will run from loop()
}
/*=== END PART 1/4 ===========================================================*/
/*=== PART 2/4: Networking (raw TCP) + Parser + Formatting ===================*/
/*=== REPLACE fetchJsonRawTCP WITH THIS (adds redirect detect) ===============*/

String fetchJsonHttpsInsecure(const char* host, const char* path) {
  WiFiClientSecure client;
  client.setInsecure();                 // skip cert checks
  client.setTimeout(15000);             // longer TLS timeout for S3

  const int httpsPort = 443;
  if (!client.connect(host, httpsPort)) {
    Serial.println("[ERROR] HTTPS connect failed");
    return "";
  }

  client.print(String("GET ") + path + " HTTP/1.1\r\n" +
               "Host: " + host + "\r\n" +
               "User-Agent: ESP32S3-BinTracker/1.0\r\n" +
               "Connection: close\r\n\r\n");

  // read status + headers
  String statusLine = client.readStringUntil('\n');
  statusLine.trim();
  Serial.print("[HTTPS] "); Serial.println(statusLine);

  // skip headers
  while (client.connected()) {
    String h = client.readStringUntil('\n');
    if (h == "\r" || h.length() == 0) break;
  }

  // body
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
    Serial.println("[ERROR] Raw TCP Connect Failed");
    return "";
  }

  client.print(String("GET ") + path + " HTTP/1.1\r\n" +
               "Host: " + host + "\r\n" +
               "User-Agent: ESP32S3-BinTracker/1.0\r\n" +
               "Connection: close\r\n\r\n");

  // ---- read status line ----
  String statusLine = client.readStringUntil('\n');
  statusLine.trim();
  Serial.print("[HTTP] "); Serial.println(statusLine);

  bool redirectedToHttps = false;

  // ---- read headers ----
  while (client.connected()) {
    String h = client.readStringUntil('\n');
    if (h == "\r" || h.length() == 0) break;

    h.trim();
    if (h.startsWith("Location:")) {
      if (h.indexOf("https://") >= 0) redirectedToHttps = true;
    }
  }

  // ---- read body ----
  String payload;
  while (client.available()) {
    payload += client.readString();
  }
  client.stop();
  payload.trim();
  Serial.printf("[INFO] HTTP Fetch got %u bytes\n", (unsigned)payload.length());

  // If empty OR redirect to https, fallback to insecure HTTPS
  if (payload.length() == 0 || redirectedToHttps) {
    Serial.println("[WARN] HTTP empty/redirect → HTTPS fallback");
    return fetchJsonHttpsInsecure(host, path);
  }

  return payload;
}


bool refreshDataRaw(bool verbose) {
  if (verbose) showCenteredSafe("Fetching JSON...", TFT_WHITE);
  String payload = fetchJsonRawTCP(JSON_HOST, JSON_PATH);
  if (payload.length()==0) { if (verbose) showCenteredSafe("Fetch failed", TFT_YELLOW); return false; }

  BinData tmp;
  if (!parseScheduleFromJson(payload, tmp)) { if (verbose) showCenteredSafe("Parse failed", TFT_RED); return false; }

  String today = todayISO_UK();
  String nextD = pickNextDateFrom(tmp.dates, today);

  g_data = tmp;
  g_nextDate = nextD;
  g_lastFetchMillis = millis();

  if (verbose) showCenteredSafe("Schedule updated", TFT_GREEN);
  Serial.printf("[INFO] Next date: %s (today=%s)\n", g_nextDate.c_str(), today.c_str());
  return true;
}

// ---- Formatting helpers ----
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
/*=== END PART 2/4 ===========================================================*/
/*=== PART 3/4: UI (Banner/Icons/Truck) ======================================*/
uint16_t pulsingRedColor(float intensity) {
  uint8_t r = 80 + (uint8_t)(intensity * 175);
  return tft.color565(r, 0, 0);
}

String prettyBin(const String& s) {
  if (s.equalsIgnoreCase("rubbish"))   return "RUBBISH";
  if (s.equalsIgnoreCase("recycling")) return "RECYCLING";
  if (s.equalsIgnoreCase("garden"))    return "GARDEN";
  if (s.equalsIgnoreCase("food"))      return "FOOD";
  return s;
}

void drawBinIconFlat(const String& type, int x, int y) {
  uint16_t col = TFT_WHITE;
  if (type.equalsIgnoreCase("rubbish"))   col = tft.color565(30,30,30);
  if (type.equalsIgnoreCase("recycling")) col = tft.color565(20,90,200);
  if (type.equalsIgnoreCase("garden"))    col = tft.color565(130, 80, 20);
  if (type.equalsIgnoreCase("food"))      col = tft.color565(20,160,40);
  tft.fillRoundRect(x, y, 46, 46, 8, col);
  tft.drawRoundRect(x, y, 46, 46, 8, TFT_BLACK);
}

int bannerTypeForDate(const String& nextDateISO) {
  // 0 none, 1 yellow "tomorrow", 2 red "tomorrow after 4pm"
  struct tm lt{};
  if (!nowLocalUK(lt)) return 0;
  // build today's ISO and tomorrow's ISO
  char buf[11];
  strftime(buf,sizeof(buf),"%Y-%m-%d",&lt);
  String today(buf);

  time_t t = time(nullptr);
  t += 24*3600;
  struct tm tm2{};
  localtime_r(&t,&tm2);
  char buf2[11];
  strftime(buf2,sizeof(buf2),"%Y-%m-%d",&tm2);
  String tomorrow(buf2);

  if (nextDateISO == tomorrow) {
    if (lt.tm_hour >= 16) return 2; // red after 4pm
    return 1; // yellow before
  }
  return 0;
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
  // Clock (with seconds) is drawn from loop each second
  // Draw ID above bottom edge
  tft.setTextDatum(BR_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("ID: " + idTag, tft.width()-6, tft.height()-6, 2);
}

void drawUI(const BinData& data, const String& nextDate, bool /*forceRedAlert*/) {
  bool showBringBack = false;
  struct tm lt{};
  if (nowLocalUK(lt)) {
    String todayISO = todayISO_UK();
    if (todayISO == nextDate && lt.tm_hour >= 6) showBringBack = true;
  }

  int bType = bannerTypeForDate(nextDate);
  tft.fillScreen(TFT_BLACK);

  int cx = tft.width()/2, cy = tft.height()/2;
  if (showBringBack) {
    for (int i=0;i<3;i++) tft.drawCircle(cx, cy, min(cx, cy)-2-i, TFT_CYAN);
  } else if (bType == 1) {
    for (int i=0;i<3;i++) tft.drawCircle(cx, cy, min(cx, cy)-2-i, TFT_YELLOW);
  } else if (bType == 0) {
    for (int i=0;i<3;i++) tft.drawCircle(cx, cy, min(cx, cy)-2-i, tft.color565(80,80,80));
  }
  // (red pulsing border drawn in loop)

  // Banner / header
  if (showBringBack) {
    tft.fillRect(0,0,tft.width(),34,TFT_CYAN);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(TFT_BLACK, TFT_CYAN);
    tft.drawString("BRING BINS BACK IN", tft.width()/2, 14, 2);
  } else {
    drawBanner(bType);
  }

  const int hasBanner = (showBringBack || bType!=0);
  const int topPad = hasBanner ? 60 : 36;
  const int gapTitle = 24;
  const int gapRow = 22;

  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Next collection", tft.width()/2, topPad, 2);
  tft.drawString(showBringBack ? "Today" : fmtHumanDateUK_fromISO(nextDate), tft.width()/2, topPad+gapTitle, 4);

  if (showBringBack) {
    // Place truck in middle area
    int truckX = (tft.width() - garbageTruckWidth) / 2;
    int iconTopY = topPad + gapTitle + gapRow + 16;
    int truckY = iconTopY - 6;
    tft.pushImage(truckX, truckY, garbageTruckWidth, garbageTruckHeight, garbageTruckBitmap);

    // Optional extra line under truck
    // tft.setTextDatum(TC_DATUM);
    // tft.drawString("Please return bins", tft.width()/2, truckY + garbageTruckHeight + 14, 2);
    return;
  }

  // If no collection for chosen date, display announcement + next known
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

  // Icons + labels (max 3)
  std::vector<String> bins = it->second;
  int n = (int)bins.size(); if (n>3) n=3;

  const int iconSize = 46;
  const int colGap   = 24;
  int totalWidth = n*iconSize + (n-1)*colGap;
  int startX = (tft.width() - totalWidth)/2;

  bool ICONS_UP = true;
  int iconTopY  = topPad + gapTitle + gapRow + (ICONS_UP?16:20);
  int labelTopY = iconTopY + iconSize + 2;

  for (int i=0;i<n;++i) {
    int x = startX + i*(iconSize+colGap);
    drawBinIconFlat(bins[i], x, iconTopY);
  }
  for (int i=0;i<n;++i) {
    String label = prettyBin(bins[i]);
    int xCenter = startX + i*(iconSize+colGap) + iconSize/2;
    int rectW = label.length()*10 + 14;
    int rectH = 18;
    tft.fillRoundRect(xCenter - rectW/2, labelTopY - rectH/2, rectW, rectH, 6, TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(label, xCenter, labelTopY, 2);
  }
}
/*=== END PART 3/4 ===========================================================*/
/*=== PART 4/4: Loop + Serial + Time Helpers ================================*/
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
          if (refreshDataRaw(true)) { drawUI(g_data, g_nextDate, false); drawFooter(g_identifier); }
        } else if (cmd.equalsIgnoreCase("TEST BLUE")) {
          // Force bring-back preview: set nextDate to today
          String today = todayISO_UK();
          if (g_data.schedule.find(today)==g_data.schedule.end()) {
            g_data.schedule[today] = std::vector<String>{"recycling"};
            g_data.dates.push_back(today);
            std::sort(g_data.dates.begin(), g_data.dates.end());
          }
          g_nextDate = today;
          drawUI(g_data, g_nextDate, false);
          drawFooter(g_identifier);
        } else if (cmd.equalsIgnoreCase("TEST YELLOW")) {
          // Tomorrow banner simulation
          time_t t = time(nullptr) + 24*3600;
          struct tm tm2{}; localtime_r(&t,&tm2);
          char buf[11]; strftime(buf,sizeof(buf),"%Y-%m-%d",&tm2);
          g_nextDate = String(buf);
          drawUI(g_data, g_nextDate, false);
          drawFooter(g_identifier);
        } else if (cmd.equalsIgnoreCase("TEST RED")) {
          // Force red pulsing (UI redraw + loop will animate border)
          time_t t = time(nullptr) + 24*3600;
          struct tm tm2{}; localtime_r(&t,&tm2);
          tm2.tm_hour = 18; // after 4pm threshold
          char buf[11]; strftime(buf,sizeof(buf),"%Y-%m-%d",&tm2);
          g_nextDate = String(buf);
          drawUI(g_data, g_nextDate, false);
          drawFooter(g_identifier);
        } else {
          Serial.println("[INFO] Unknown command");
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

  // Boot-time first fetch (one-shot)
  if (!g_bootTriedInitialFetch) {
    g_bootTriedInitialFetch = true;

    showCenteredSafe("Resolving host...", TFT_WHITE); delay(250);
    showCenteredSafe("Fetching JSON...", TFT_WHITE);

    g_lastFetchAttempt = nowMs;
    if (refreshDataRaw(true)) {
      drawUI(g_data, g_nextDate, false);
      drawFooter(g_identifier);
    } else {
      showCenteredSafe("Fetch failed\nRetry in 15m", TFT_YELLOW);
    }
  }

  // 1-second tick: footer clock + red border pulse + yellow border ensure
  if (nowMs - lastTick1s > 1000) {
    lastTick1s = nowMs;

    struct tm lt{};
    if (nowLocalUK(lt)) {
      char buf[32];
      strftime(buf,sizeof(buf), "%a %d %b %H:%M:%S", &lt);
      tft.setTextDatum(BC_DATUM);
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.drawString(buf, tft.width()/2, tft.height()-22, 2);
    }

    // Red pulse
    if (bannerTypeForDate(g_nextDate) == 2) {
      float t = (millis() % 2000) / 1000.0f;
      float intensity = (t<1.0f)? t : (2.0f - t);
      uint16_t col = pulsingRedColor(intensity);
      int cx = tft.width()/2, cy = tft.height()/2;
      for (int i=0;i<3;i++) tft.drawCircle(cx, cy, min(cx, cy)-2-i, col);
    }
    // Yellow ensure
    if (bannerTypeForDate(g_nextDate) == 1) {
      int cx = tft.width()/2, cy = tft.height()/2;
      for (int i=0;i<3;i++) tft.drawCircle(cx, cy, min(cx, cy)-2-i, TFT_YELLOW);
    }
  }

  // Hourly refresh (silent)
  if (nowMs - lastHourlyCheck > HOURLY_REFRESH_MS) {
    lastHourlyCheck = nowMs;
    if (millis() - g_lastFetchMillis >= HOURLY_REFRESH_MS) {
      if (refreshDataRaw(false)) { drawUI(g_data, g_nextDate, false); drawFooter(g_identifier); }
    }
  }

  // Retry on failure every 15 minutes
  bool neverFetched = (g_lastFetchMillis == 0);
  bool stale = (!neverFetched) && (nowMs - g_lastFetchMillis > HOURLY_REFRESH_MS);
  bool needRetry = neverFetched || stale;
  bool canAttempt = (nowMs - g_lastFetchAttempt) >= JSON_RETRY_INTERVAL_MS;

  if (needRetry && canAttempt) {
    g_lastFetchAttempt = nowMs;
    Serial.println("[INFO] Retry → fetch JSON");
    if (refreshDataRaw(false)) {
      drawUI(g_data, g_nextDate, false);
      drawFooter(g_identifier);
    } else {
      Serial.println("[WARN] Retry failed; will try again later.");
    }
  }
}

// ---- Time helpers ----
bool nowLocalUK(struct tm& out) {
  time_t t = time(nullptr);
  if (t < 1700000000) return false;
  localtime_r(&t, &out);
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
/*=== END PART 4/4 ===========================================================*/
