# OTA config loader + status carousel sketches

This repo currently ships the main firmware in `current.ino`. To keep the
existing sketch untouched, the OTA/bootstrap and UI enhancements are provided as
separate headers you can include from a small wrapper sketch.

## Bootloader-style OTA config fetcher
- **File:** `ota_config_loader.h`
- **Purpose:** Bring up Wi-Fi, pull JSON configuration from a Git-hosted path
  (e.g., the raw view of a GitHub file), and write it to `/config.json` on
  SPIFFS. The helper records the last poll time and version marker in NVS so it
  can throttle requests.
- **Usage outline:**
  1. Instantiate `OtaConfigLoader` with `Settings` that point to your repo path
     (e.g., `/org/repo/branch/device-config.json`).
  2. Call `begin(ssid, pass)` in `setup()` to mount SPIFFS + NVS and join Wi-Fi.
  3. In `loop()`, call `pollNeeded()` or watch for the serial command `UPDATE`
     to decide when to call `fetchLatest()` and `persistIfNew()`.
  4. Once the config is written to SPIFFS, the main app can read `/config.json`
     instead of the hard-coded JSON URL.
- **Serial overrides:** A `BRANCH <path>` command updates the tracked Git path
  at runtime without reflashing (the value is stored in NVS).

## Screen carousel + API-driven extras
- **File:** `status_carousel.h`
- **Purpose:** Provide a `StatusCarousel` that rotates through different status
  panels (full-screen clock, weather, heating status). Each panel is a small
  class implementing `StatusScreen::render`.
- **Usage outline:**
  1. Create `StatusCarousel carousel(10);` and add screens like `new
     FullClockScreen`, `new WeatherScreen(fetchMetOffice)`, etc.
  2. Call `carousel.forceRender(tft);` after boot, then `carousel.tick(tft);` in
     the existing 1-second loop to advance every 10 seconds (configurable).
  3. Optional helpers `fetchMetOffice` and `fetchHomeAssistantClimate` show how
     to call external APIs for weather and central heating data.

Both headers are self-contained and avoid edits to `current.ino`, keeping the
existing workflow stable while enabling OTA configuration and richer display
modes.
