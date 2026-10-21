// Shared internals for MantisCore UI modules
#pragma once
#include <Arduino.h>
#include <M5Unified.h>
#include <vector>

// ---- brand palette (RGB565)
#define MC_CHARCOAL 0x2125 // #24262A
#define MC_CHARCOAL2 0x31A7 // #34363A
#define MC_TEAL 0x038E     // #007373
#define MC_PURPLE 0x580B   // #5D005D
#define MC_GREEN 0x07E0    // #00FF00
#define MC_WHITE 0xFFFF
#define MC_GREY 0xA534

#define MC_W 320
#define MC_H 240
#define MC_HEADER_H 22

// ---- frame buffer (PSRAM canvas, stores byte-swapped RGB565)
extern M5Canvas mcCanvas;
bool mcGfxInit();
void mcPush();                                   // push canvas to panel
inline uint16_t *mcBuf() { return (uint16_t *)mcCanvas.getBuffer(); }
inline uint16_t mcSwap(uint16_t c) { return (uint16_t)((c << 8) | (c >> 8)); }
uint16_t mcBlend(uint16_t fg, uint16_t bg, uint8_t a); // normal (unswapped) RGB565
void mcDarkenRect(int x, int y, int w, int h, uint8_t keep); // keep: 0..255 brightness
void mcBlitRGB(int x, int y, int w, int h, const uint16_t *px);                      // opaque
void mcBlitAlpha(int x, int y, int w, int h, const uint16_t *px, const uint8_t *a);  // per-pixel alpha
void mcBlitAdd(int x, int y, int w, int h, const uint16_t *px, uint8_t level);       // additive light
void mcDrawMascot(int frame, int x, int y, int scale);
void mcText(const char *s, int x, int y, const lgfx::IFont *font, uint16_t color, uint8_t datum,
            bool shadow = false);

// ---- images
bool mcLoadMg(const char *path, uint16_t *dst);       // 320x240 .mg -> unswapped RGB565
struct McIcon { uint16_t *rgb = nullptr; uint8_t *a = nullptr; };
bool mcLoadIcon(const char *path, McIcon &out);       // 64x64 .mi
void mcFreeIcon(McIcon &icon);

// ---- settings (NVS namespace "mantis")
struct McConfig {
    String ssid, pass, zip;
    uint8_t bright = 70;   // 5..100 %
    uint8_t tzIndex = 3;   // see mcTimeZones
    bool h24 = false;
    bool customBg = false; // false: /images, true: /images/backgrounds
    uint16_t bgIndex = 0;
    bool metric = false;   // weather units: false = F / mph / in, true = C / km/h / mm
};
extern McConfig mcCfg;
void mcConfigLoad();
void mcConfigSave();
struct McTimeZone { const char *name; const char *posix; };
extern const McTimeZone mcTimeZones[];
extern const int mcTimeZoneCount;
void mcApplyTimeZone();
void mcTimeFromRtc();          // RTC (UTC) -> system clock
void mcTimeToRtc(time_t utc);  // system clock + RTC
String mcClockString(bool withSeconds = false);
void mcApplyBrightness();

// ---- input
struct McTouch {
    bool down = false, pressed = false, released = false; // pressed/released are edges this poll
    int x = 0, y = 0, startX = 0, startY = 0;
    uint32_t downAt = 0;
    bool moved = false;      // moved beyond tap slop since press
    bool longFired = false;  // set by caller once a long-press was handled
};
extern McTouch mcTouch;
void mcPollTouch();
bool mcWaitRelease(uint32_t timeoutMs = 3000);

// ---- loading screen / engine hooks
void mcLoadingBegin(const String &title);
void mcLoadingFrame(float progress, const String &status); // progress < 0 = indeterminate
void mcToast(const String &text, uint16_t color = MC_TEAL, uint32_t ms = 1800);

// ---- network (background task)
// Plain data (no Strings) so the UI can take a consistent copy while the net task updates it.
struct McWeather {
    bool valid = false, metric = false, isDay = true;
    time_t updated = 0;                          // when the forecast was fetched (UTC)
    float temp = 0, feels = 0, precip = 0, pressure = 0, wind = 0, gust = 0, uv = 0;
    int humidity = 0, code = -1, cloud = 0, windDir = 0;
    float hi = 0, lo = 0, uvMax = 0;
    int popMax = 0;                              // today's highest chance of rain, %
    char sunrise[6] = "", sunset[6] = "";        // local "HH:MM"
    int hours = 0;                               // valid entries in the next-12-hours arrays
    float hourT[12] = {};
    int hourPop[12] = {};
    bool aqiValid = false;
    int aqi = 0;                                 // US AQI 0-500
    float pm25 = 0, pm10 = 0, co = 0, o3 = 0;    // ug/m3
    char place[28] = "";
};
extern volatile bool mcNetBusy;
extern volatile bool mcWifiUp;
extern volatile bool mcNtpPending; // set by net task, consumed by UI (writes RTC)
void mcWeatherSnapshot(McWeather &out);
void mcNetStart();                 // (re)start connect + NTP + weather task
void mcNetStop();
void mcNetRefreshWeather();        // fetch again soon without reconnecting
void mcWeatherScreen();            // full-screen weather instrument

// ---- backups
void mcBackupQueueAfterApp();      // queue last-run app + apps never backed up
bool mcBackupBusy();
void mcBackupStep(uint32_t budgetMs);
bool mcBackupAppBlocking(const String &label, const String &appName); // with loading screen
void mcBackupAbort();
bool mcIsCharging();

// ---- screens
void mcNixieClock();
void mcChargeMode();
void mcSettings();
void mcTimeSettings();
String mcKeyboard(const String &title, const String &initial, bool numeric, bool password);
int mcActionSheet(const String &title, const std::vector<String> &options); // -1 = dismissed
bool mcConfirm(const String &title, const String &yes, const String &no);
void mcBackupFinishBlocking(const String &title);
