#pragma once
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <vector>
#include <functional>

/**
 * Drop-in carousel helper for rotating through multiple status screens.  The
 * screens are kept in their own lightweight structs so they can be slotted into
 * the existing draw loop without changing the main sketch.  Each screen exposes
 * a render() function that draws immediately onto the provided TFT instance.
 */
class StatusScreen {
public:
    virtual ~StatusScreen() = default;
    virtual const char* name() const = 0;
    virtual void render(TFT_eSPI& tft) = 0;
};

class FullClockScreen : public StatusScreen {
public:
    const char* name() const override { return "clock"; }
    void render(TFT_eSPI& tft) override {
        tft.fillScreen(TFT_BLACK);
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        time_t now = time(nullptr);
        if (now < 1700000000) {
            tft.drawString("No NTP", tft.width()/2, tft.height()/2, 4);
            return;
        }
        struct tm lt{};
        localtime_r(&now, &lt);
        char dateBuf[32];
        strftime(dateBuf, sizeof(dateBuf), "%a %d %b", &lt);
        char timeBuf[16];
        strftime(timeBuf, sizeof(timeBuf), "%H:%M", &lt);
        tft.drawString(timeBuf, tft.width()/2, tft.height()/2 - 8, 6);
        tft.drawString(dateBuf, tft.width()/2, tft.height()/2 + 24, 4);
    }
};

class WeatherScreen : public StatusScreen {
public:
    struct Reading { String summary; float tempC = NAN; };
    explicit WeatherScreen(std::function<Reading()> provider) : fetch(provider) {}
    const char* name() const override { return "weather"; }
    void render(TFT_eSPI& tft) override {
        tft.fillScreen(TFT_BLACK);
        tft.setTextDatum(TC_DATUM);
        tft.setTextColor(TFT_CYAN, TFT_BLACK);
        Reading r = fetch();
        if (isnan(r.tempC)) {
            tft.drawString("Weather unavailable", tft.width()/2, 20, 2);
            return;
        }
        tft.drawString("Weather", tft.width()/2, 8, 2);
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.drawString(String(r.tempC, 1) + " C", tft.width()/2, tft.height()/2 - 8, 6);
        tft.drawString(r.summary, tft.width()/2, tft.height()/2 + 24, 2);
    }
private:
    std::function<Reading()> fetch;
};

class HeatingScreen : public StatusScreen {
public:
    struct Status { bool on = false; float targetC = NAN; };
    explicit HeatingScreen(std::function<Status()> provider) : fetch(provider) {}
    const char* name() const override { return "heating"; }
    void render(TFT_eSPI& tft) override {
        tft.fillScreen(TFT_BLACK);
        tft.setTextDatum(MC_DATUM);
        Status s = fetch();
        uint16_t col = s.on ? TFT_ORANGE : TFT_DARKGREY;
        tft.setTextColor(col, TFT_BLACK);
        String line1 = s.on ? "Heating ON" : "Heating OFF";
        tft.drawString(line1, tft.width()/2, tft.height()/2 - 12, 4);
        if (!isnan(s.targetC)) {
            tft.drawString(String("Target ") + String(s.targetC,1) + " C",
                           tft.width()/2, tft.height()/2 + 18, 2);
        }
    }
private:
    std::function<Status()> fetch;
};

/**
 * Simple controller that rotates through registered screens every N seconds.
 * The caller can hook this into the existing 1-second tick; only the new files
 * change so current.ino stays untouched.
 */
class StatusCarousel {
public:
    explicit StatusCarousel(uint32_t dwellSeconds = 10)
        : dwellMs(dwellSeconds * 1000UL) {}

    void addScreen(StatusScreen* screen) { screens.push_back(screen); }

    void tick(TFT_eSPI& tft) {
        if (screens.empty()) return;
        uint32_t now = millis();
        if (now - lastSwitchMs > dwellMs) {
            lastSwitchMs = now;
            index = (index + 1) % screens.size();
            screens[index]->render(tft);
        }
    }

    void forceRender(TFT_eSPI& tft) {
        if (screens.empty()) return;
        screens[index]->render(tft);
        lastSwitchMs = millis();
    }

private:
    std::vector<StatusScreen*> screens;
    size_t index = 0;
    uint32_t dwellMs;
    uint32_t lastSwitchMs = 0;
};

/**
 * Example data providers using public APIs.  These are left as standalone
 * helpers so the main sketch can opt in selectively.
 */
inline WeatherScreen::Reading fetchMetOffice(const char* url) {
    WeatherScreen::Reading r;
    HTTPClient http;
    if (!http.begin(url)) return r;
    int code = http.GET();
    if (code != HTTP_CODE_OK) { http.end(); return r; }
    DynamicJsonDocument doc(2048);
    DeserializationError err = deserializeJson(doc, http.getStream());
    if (!err) {
        r.tempC = doc["properties"]["timeSeries"][0]["screenTemperature"].as<float>();
        r.summary = doc["properties"]["timeSeries"][0]["significantWeatherCode"].as<String>();
    }
    http.end();
    return r;
}

inline HeatingScreen::Status fetchHomeAssistantClimate(const char* url, const char* token) {
    HeatingScreen::Status s;
    HTTPClient http;
    http.begin(url);
    http.addHeader("Authorization", String("Bearer ") + token);
    http.addHeader("Content-Type", "application/json");
    int code = http.GET();
    if (code != HTTP_CODE_OK) { http.end(); return s; }
    DynamicJsonDocument doc(1024);
    if (!deserializeJson(doc, http.getStream())) {
        s.on = doc["state"].as<String>() == "heat";
        s.targetC = doc["attributes"]["temperature"].as<float>();
    }
    http.end();
    return s;
}
