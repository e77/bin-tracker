#pragma once
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <SPIFFS.h>
#include <Preferences.h>

/**
 * Lightweight helper that can live alongside the existing sketch and own the
 * "configuration fetch" lifecycle.  It can be compiled into a minimal
 * bootloader/launcher sketch that only brings up Wi-Fi, reads the desired Git
 * branch/path from NVS, downloads a JSON payload from GitHub, writes it to
 * SPIFFS, and then returns control to the main application.
 *
 * Typical flow:
 *   1. Bootloader calls begin() to start Wi-Fi + SPIFFS + NVS.
 *   2. If pollNeeded() returns true, call fetchLatest() to download the
 *      configuration JSON from the configured GitHub path.
 *   3. Call persistIfNew() to store the freshly downloaded payload to SPIFFS
 *      and update the "version" marker in NVS.
 *   4. Jump to the main application (or signal over serial that new config is
 *      present).
 */
class OtaConfigLoader {
public:
    struct Settings {
        String gitHost       = "raw.githubusercontent.com";
        String repoPath      = "/e77/bin-tracker/main/34.json"; // default
        uint32_t pollMinutes = 15;                                // background polling interval
        bool useHttps        = true;
    };

    explicit OtaConfigLoader(const Settings& cfg = Settings()) : settings(cfg) {}

    bool begin(const char* ssid, const char* pass) {
        if (!SPIFFS.begin(true)) {
            Serial.println("[OTA] SPIFFS mount failed");
            return false;
        }
        prefs.begin("ota-config", false);
        WiFi.mode(WIFI_STA);
        WiFi.begin(ssid, pass);
        unsigned long start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
            delay(200);
        }
        return WiFi.status() == WL_CONNECTED;
    }

    /**
     * Returns true when it's time to poll Git for updates.  Polling is based on
     * the last attempt timestamp stored in NVS so the bootloader can stay tiny
     * while still throttling network usage if it is invoked frequently.
     */
    bool pollNeeded() {
        uint32_t last = prefs.getULong("last_poll", 0);
        uint32_t now = millis();
        uint32_t interval = settings.pollMinutes * 60UL * 1000UL;
        if (now - last > interval) {
            prefs.putULong("last_poll", now);
            return true;
        }
        return false;
    }

    /**
     * Allow issuing a single-line serial command (e.g. "UPDATE" or
     * "BRANCH main") to trigger immediate refresh or to change the tracked
     * branch at runtime without reflashing firmware.
     */
    void handleSerialCommand(const String& cmd) {
        if (cmd.equalsIgnoreCase("UPDATE")) {
            manualUpdateRequested = true;
        } else if (cmd.startsWith("BRANCH ")) {
            settings.repoPath = cmd.substring(7);
            prefs.putString("repo_path", settings.repoPath);
            manualUpdateRequested = true;
        }
    }

    /**
     * Download the JSON payload from GitHub.  This uses the raw content host so
     * it can be reused by both the bootloader and the main sketch.  The caller
     * is responsible for calling persistIfNew() after a successful fetch.
     */
    String fetchLatest() {
        WiFiClientSecure client;
        client.setInsecure();
        const int port = settings.useHttps ? 443 : 80;
        if (!client.connect(settings.gitHost.c_str(), port)) {
            Serial.println("[OTA] connect failed");
            return "";
        }
        String req = String("GET ") + settings.repoPath + " HTTP/1.1\r\n" +
                     "Host: " + settings.gitHost + "\r\n" +
                     "User-Agent: OTA-Boot/1.0\r\n" +
                     "Connection: close\r\n\r\n";
        client.print(req);
        String status = client.readStringUntil('\n');
        if (!status.startsWith("HTTP/1.1 200")) {
            Serial.print("[OTA] bad status: ");
            Serial.println(status);
            return "";
        }
        // skip headers
        while (client.connected()) {
            String h = client.readStringUntil('\n');
            if (h == "\r" || h.length() == 0) break;
        }
        String payload;
        while (client.available()) payload += client.readString();
        payload.trim();
        Serial.printf("[OTA] downloaded %u bytes\n", (unsigned)payload.length());
        return payload;
    }

    /**
     * Writes the fetched JSON to SPIFFS and only bumps the stored version if
     * the payload has changed.  The main sketch can later mount SPIFFS and read
     * /config.json as a drop-in replacement for the hard-coded path.
     */
    bool persistIfNew(const String& payload) {
        if (payload.isEmpty()) return false;
        String prior = readCached();
        if (prior == payload) {
            Serial.println("[OTA] no change; skip write");
            return false;
        }
        File f = SPIFFS.open("/config.json", "w");
        if (!f) {
            Serial.println("[OTA] open /config.json failed");
            return false;
        }
        f.print(payload);
        f.close();
        prefs.putUInt("version", prefs.getUInt("version", 0) + 1);
        Serial.println("[OTA] config persisted to SPIFFS");
        return true;
    }

    String readCached() {
        if (!SPIFFS.exists("/config.json")) return "";
        File f = SPIFFS.open("/config.json", "r");
        if (!f) return "";
        String s = f.readString();
        f.close();
        return s;
    }

    bool isManualUpdateRequested() const { return manualUpdateRequested; }

private:
    Settings settings;
    Preferences prefs;
    bool manualUpdateRequested = false;
};

/**
 * Minimal example bootloader loop.
 *
 * void setup() {
 *   OtaConfigLoader::Settings cfg;
 *   cfg.repoPath = "/your-org/your-repo/main/device-config.json";
 *   loader = new OtaConfigLoader(cfg);
 *   loader->begin("wifi-ssid", "wifi-pass");
 * }
 *
 * void loop() {
 *   static String cmd;
 *   while (Serial.available()) {
 *     char c = Serial.read();
 *     if (c == '\n') { loader->handleSerialCommand(cmd); cmd = ""; }
 *     else { cmd += c; }
 *   }
 *   if (loader->isManualUpdateRequested() || loader->pollNeeded()) {
 *     String json = loader->fetchLatest();
 *     loader->persistIfNew(json);
 *   }
 *   delay(500);
 * }
 */
