/*===Includes + Globals + Setup ==================================*/
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


// ---- WIFI (update if you change credentials) ----
static const char* WIFI_SSID = "Plumbing";
static const char* WIFI_PASS = "2hphdwdFwkpr";

// ---- JSON endpoint ----
// Host + path of your GitHub raw JSON (or wherever you serve it)
static const char* JSON_HOST = "raw.githubusercontent.com";
static const char* JSON_PATH = "/e77/bin-tracker/main/15b.json";

// ---- Retry intervals ----
static const unsigned long JSON_RETRY_INTERVAL_MS = 15UL * 60UL * 1000UL; // 15 minutes
static const unsigned long HOURLY_REFRESH_MS = 60UL * 60UL * 1000UL;      // 1 hour

// ---- Globals ----
TFT_eSPI tft;

struct BinData {
    // Raw fields from JSON (optional, for future use)
    String lastUpdated;                 // e.g. "2025-11-27T20:15:00Z"
    String today;                       // server's idea of "today" in ISO YYYY-MM-DD

    // Per-stream date lists (ISO YYYY-MM-DD sorted strings)
    std::vector<String> refuseDates;    // general waste / refuse
    std::vector<String> recyclingDates;
    std::vector<String> gardenDates;
    std::vector<String> foodDates;

    // Derived fields used by the UI code
    std::vector<String> dates;          // all unique dates across all streams, sorted
    std::map<String, std::vector<String>> schedule; // date -> list of stream names
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

    // ---- Build derived schedule + master date list ----
    out.schedule.clear();
    out.dates.clear();

    auto addStream = [&](const char* streamName, const std::vector<String>& src) {
        for (const auto& d : src) {
            // Append stream name to that date's list
            auto& vec = out.schedule[d];
            // avoid duplicate stream entries for same date
            bool have = false;
            for (const auto& s : vec) {
                if (s == streamName) { have = true; break; }
            }
            if (!have) vec.push_back(String(streamName));

            // ensure d is present in master dates list
            bool seen = false;
            for (const auto& existing : out.dates) {
                if (existing == d) { seen = true; break; }
            }
            if (!seen) out.dates.push_back(d);
        }
    };

    addStream("refuse",    out.refuseDates);
    addStream("recycling", out.recyclingDates);
    addStream("garden",    out.gardenDates);
    addStream("food",      out.foodDates);

    // keep dates sorted so pickNextDateFrom works correctly
    std::sort(out.dates.begin(), out.dates.end());

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
  configTime(0, 0, "pool.ntp.org");   // UTC
  Serial.println("[TIME] Syncing via NTP...");
  time_t nowSec = 0;
  int retries = 0;
  while (retries < 15) {
    nowSec = time(nullptr);
    if (nowSec > 1700000000) {
      Serial.println("[TIME] NTP sync OK");
      return true;
    }
    retries++;
    delay(200);
  }
  Serial.println("[TIME] NTP sync FAILED");
  return false;
}

bool nowLocalUK(struct tm& out) {
  time_t nowSec = time(nullptr);
  if (nowSec < 1700000000) return false;

  // UK time zone, with DST rules baked into TZ string
  setenv("TZ", "GMT0BST,M3.5.0/1,M10.5.0/2", 1);
  tzset();

  struct tm* lt = localtime(&nowSec);
  if (!lt) return false;
  out = *lt;
  return true;
}


// ---- TFT Boot + WiFi + Time init ----
void setup() {
  Serial.begin(115200);
  delay(100);

  tft.init();
  tft.setRotation(0);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(1);
  tft.setTextDatum(MC_DATUM);

  showCenteredSafe("POWER ON", TFT_WHITE); delay(300);
  showCenteredSafe("Connecting WiFi...", TFT_WHITE);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(250);
  }

  if (WiFi.status() == WL_CONNECTED) {
    showCenteredSafe("WiFi OK", TFT_GREEN); delay(300);
  } else {
    showCenteredSafe("WiFi FAILED", TFT_RED); delay(600);
  }

  showCenteredSafe("Syncing time...", TFT_WHITE);
  if (ntpSyncOnce()) {
    showCenteredSafe("Time OK", TFT_GREEN); delay(300);
  } else {
    showCenteredSafe("Time FAIL", TFT_RED); delay(600);
  }

  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Bin Tracker", tft.width()/2, tft.height()/2, 4);

  g_lastFetchMillis = 0;
  g_lastFetchAttempt = 0;
  g_bootTriedInitialFetch = false;

  // first fetch will run from loop()
}
/*=== END PART 1/4 ===========================================================*/
/*=== PART 2/4: Networking (raw TCP) + Parser + Formatting ===================*/

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
               "User-Agent: ESP32BinTracker\r\n" +
               "Connection: close\r\n\r\n");

  // read headers first, look for redirect or 200 OK
  String statusLine = client.readStringUntil('\n');
  statusLine.trim();
  Serial.print("[HTTPS] Status: ");
  Serial.println(statusLine);

  if (!statusLine.startsWith("HTTP/1.1 200")) {
    // Could add 301/302 handling here if you want
    Serial.println("[HTTPS] Non-200, aborting");
    return "";
  }

  // Skip remaining headers
  while (client.connected()) {
    String header = client.readStringUntil('\n');
    if (header == "\r" || header.length() == 0) break;
  }

  // Read body
  String body;
  while (client.available()) {
    body += client.readString();
  }
  Serial.println("[HTTPS] Body length = " + String(body.length()));
  return body;
}

// Legacy raw TCP version kept for reference / fallback
String fetchJsonRawTCP(const char* host, const char* path) {
  WiFiClient client;
  const int httpPort = 80;

  if (!client.connect(host, httpPort)) {
    Serial.println("[ERROR] Raw TCP Connect Failed");
    return "";
  }

  client.print(String("GET ") + path + " HTTP/1.1\r\n" +
               "Host: " + host + "\r\n" +
               "User-Agent: ESP32BinTracker\r\n" +
               "Connection: close\r\n\r\n");

  String headers;
  while (client.connected()) {
    String line = client.readStringUntil('\n');
    if (line == "\r" || line.length() == 0) break;  // end of headers
    headers += line + "\n";
  }

  String json;
  while (client.available()) {
    json += client.readString();
  }

  Serial.print("[RAW] JSON length = ");
  Serial.println(json.length());
  return json;
}

// Choose which fetcher you want here:
String fetchJson() {
  // For GitHub raw (HTTPS)
  return fetchJsonHttpsInsecure(JSON_HOST, JSON_PATH);
  // Or: return fetchJsonRawTCP(JSON_HOST, JSON_PATH);
}

bool refreshDataRaw(bool verbose) {
  if (verbose) showCenteredSafe("Fetching JSON...", TFT_WHITE);

  // IMPORTANT: Use HTTPS helper, not raw TCP
  String payload = fetchJsonHttpsInsecure(JSON_HOST, JSON_PATH);

  if (payload.length() == 0) {
    if (verbose) showCenteredSafe("Fetch failed", TFT_YELLOW);
    return false;
  }

  // Debug: log start of payload so we can see if it's JSON or HTML
  Serial.println("[DEBUG] First 160 chars of payload:");
  Serial.println(payload.substring(0, 160));

  // Trim off any junk before the first '{' (BOM, whitespace, etc.)
  int bracePos = payload.indexOf('{');
  if (bracePos > 0) {
    payload = payload.substring(bracePos);
  }

  BinData tmp;
  if (!parseScheduleFromJson(payload, tmp)) {
    Serial.println("[ERROR] JSON parse still failing after trim");
    if (verbose) showCenteredSafe("Parse failed", TFT_RED);
    return false;
  }

  String today = todayISO_UK();
  String nextD = pickNextDateFrom(tmp.dates, today);

  g_data = tmp;
  g_nextDate = nextD;
  g_lastFetchMillis = millis();

  if (verbose) {
    String human = fmtHumanDateUK_fromISO(nextD);
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("Next:", tft.width()/2, tft.height()/2 - 10, 2);
    tft.drawString(human, tft.width()/2, tft.height()/2 + 10, 4);
    delay(700);
  }

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
/*=== END PART 2/4 ===================================================*/
/*=== PART 3/4: UI (Banner/Icons/Truck) ======================================*/
uint16_t pulsingRedColor(float intensity) {
  uint8_t r = 80 + (uint8_t)(intensity * 175);
  return tft.color565(r, 0, 0);
}

// 0 = none, 1 = yellow, 2 = red
int bannerTypeForDate(const String& nextDateISO) {
  String today = todayISO_UK();
  if (today.length()<10 || nextDateISO.length()<10) return 0;
  if (today == nextDateISO) return 2;

  struct tm tNext{};
  tNext.tm_year = nextDateISO.substring(0,4).toInt()-1900;
  tNext.tm_mon  = nextDateISO.substring(5,7).toInt()-1;
  tNext.tm_mday = nextDateISO.substring(8,10).toInt();
  time_t nextT = mktime(&tNext);

  struct tm tToday{};
  if (!nowLocalUK(tToday)) return 0;
  time_t todayT = mktime(&tToday);

  int diffDays = (int)((nextT - todayT) / 86400);
  if (diffDays == 1) return 1;
  return 0;
}

void drawBanner(int type) {
  if (type==0) return;

  int barH = 32;
  int y0 = 0;
  uint16_t col = (type==2) ? TFT_RED : TFT_YELLOW;
  uint16_t txt = TFT_BLACK;

  tft.fillRect(0,y0,tft.width(),barH,col);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(txt,col);

  String msg = (type==2) ? "PUT BINS OUT" : "Bins out tomorrow";
  tft.drawString(msg, tft.width()/2, y0 + barH/2, 2);
}

void drawFooter(const String& idTag) {
  struct tm lt{};
  if (!nowLocalUK(lt)) return;

  char buf[32];
  strftime(buf,sizeof(buf), "%H:%M", &lt);

  int footerH = 16;
  int y = tft.height() - footerH;
  tft.fillRect(0, y, tft.width(), footerH, TFT_BLACK);

  tft.setTextDatum(BC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(String(buf), tft.width() - 2, tft.height()-1, 1);

  tft.setTextDatum(BL_DATUM);
  tft.drawString(idTag, 2, tft.height()-1, 1);
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
  } else if (bType == 2) {
    for (int i=0;i<3;i++) tft.drawCircle(cx, cy, min(cx, cy)-2-i, TFT_RED);
  }

  if (showBringBack) {
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("Bring bins back in", tft.width()/2, 12, 2);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("Today", tft.width()/2, tft.height()/2 - 8, 4);
    tft.setTextDatum(BC_DATUM);
    tft.drawString("Please return to storage", tft.width()/2, tft.height()-18, 2);

    tft.drawRoundRect(6, 6, tft.width()-12, tft.height()-12, 10, TFT_CYAN);
    tft.drawRoundRect(8, 8, tft.width()-16, tft.height()-16, 10, TFT_CYAN);

    int truckX = (tft.width() - garbageTruckWidth) / 2;
    int truckY = tft.height()/2 - garbageTruckHeight - 6;
    tft.pushImage(truckX, truckY, garbageTruckWidth, garbageTruckHeight, garbageTruckBitmap);
    return;
  }

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

  if (showBringBack) {
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(TFT_BLACK, TFT_CYAN);
    tft.fillRect(0, 0, tft.width(), 32, TFT_CYAN);
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
  int labelTopY = iconTopY + iconSize + 12;

  for (int i=0;i<n;i++) {
    int xCenter = startX + i*(iconSize+colGap) + iconSize/2;

    uint16_t col;
    String label;

    String b = bins[i];
    b.toLowerCase();
    if (b.indexOf("refuse")>=0 || b.indexOf("rubbish")>=0 || b.indexOf("landfill")>=0) {
      col = TFT_DARKGREY;
      label = "Refuse";
    } else if (b.indexOf("recycling")>=0) {
      col = TFT_GREEN;
      label = "Recycling";
    } else if (b.indexOf("garden")>=0) {
      col = TFT_OLIVE;
      label = "Garden";
    } else if (b.indexOf("food")>=0) {
      col = TFT_BROWN;
      label = "Food";
    } else {
      col = TFT_WHITE;
      label = b;
    }

    tft.fillRoundRect(xCenter-iconSize/2, iconTopY, iconSize, iconSize, 8, col);
    tft.drawRoundRect(xCenter-iconSize/2, iconTopY, iconSize, iconSize, 8, TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_BLACK, col);
    tft.drawString(String(i+1), xCenter, iconTopY+iconSize/2, 4);

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
          String today = todayISO_UK();
          String tomorrow = today;
          if (today.length()==10) {
            struct tm t{};
            t.tm_year = today.substring(0,4).toInt()-1900;
            t.tm_mon  = today.substring(5,7).toInt()-1;
            t.tm_mday = today.substring(8,10).toInt();
            time_t tt = mktime(&t) + 86400;
            struct tm* nxt = localtime(&tt);
            char buf[11];
            strftime(buf,sizeof(buf),"%Y-%m-%d",nxt);
            tomorrow = String(buf);
          }
          g_nextDate = tomorrow;
          drawUI(g_data, g_nextDate, false);
          drawFooter(g_identifier);
        } else if (cmd.equalsIgnoreCase("TEST RED")) {
          String today = todayISO_UK();
          g_nextDate = today;
          drawUI(g_data, g_nextDate, true);
          drawFooter(g_identifier);
        } else {
          Serial.println("[CMD] Unknown");
        }
      }
      cmd = "";
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
      tft.fillRect(0, tft.height()-14, tft.width(), 14, TFT_BLACK);
      tft.drawString(String(buf), tft.width()/2, tft.height()-1, 1);
    }

    // Red pulse border if collection today
    String todayISO = todayISO_UK();
    if (todayISO == g_nextDate) {
      float phase = (millis()%1000)/1000.0f;
      float intensity = 0.5f + 0.5f*sinf(phase*2*3.14159f);
      uint16_t col = pulsingRedColor(intensity);
      tft.drawRoundRect(0,0,tft.width(),tft.height(), 10, col);
      tft.drawRoundRect(2,2,tft.width()-4,tft.height()-4, 10, col);
    }
  }

  // Hourly check: re-fetch JSON
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
