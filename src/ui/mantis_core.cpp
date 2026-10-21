// MantisCore core: graphics helpers, images, settings, time, touch, loading screen, engine hooks, splash.
#include "mantis.h"
#include "mantis_assets.h"
#include "mantis_internal.h"
#include <Preferences.h>
#include <SD.h>
#include <sys/time.h>
#include <time.h>

M5Canvas mcCanvas(&M5.Display);
McConfig mcCfg;
McTouch mcTouch;

// ------------------------------------------------------------------ graphics
bool mcGfxInit() {
    static bool ready = false;
    if (ready) return true;
    M5.Display.setRotation(1); // landscape 320x240, same as the Mantis apps
    mcCanvas.setPsram(true);
    mcCanvas.setColorDepth(16);
    ready = mcCanvas.createSprite(MC_W, MC_H) != nullptr;
    mcApplyBrightness();
    return ready;
}

void mcPush() {
    mcCanvas.pushSprite(0, 0);
    M5.Display.waitDMA(); // never leave a transfer running into a flash write
}

uint16_t mcBlend(uint16_t fg, uint16_t bg, uint8_t a) {
    if (a == 255) return fg;
    if (a == 0) return bg;
    uint32_t inv = 255 - a;
    uint32_t r = (((fg >> 11) & 31) * a + ((bg >> 11) & 31) * inv) / 255;
    uint32_t g = (((fg >> 5) & 63) * a + ((bg >> 5) & 63) * inv) / 255;
    uint32_t b = ((fg & 31) * a + (bg & 31) * inv) / 255;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static inline bool clip(int &x, int &y, int &w, int &h, int &sx, int &sy) {
    sx = sy = 0;
    if (x < 0) { sx = -x; w += x; x = 0; }
    if (y < 0) { sy = -y; h += y; y = 0; }
    if (x + w > MC_W) w = MC_W - x;
    if (y + h > MC_H) h = MC_H - y;
    return w > 0 && h > 0;
}

void mcDarkenRect(int x, int y, int w, int h, uint8_t keep) {
    int sx, sy;
    if (!clip(x, y, w, h, sx, sy)) return;
    uint16_t *buf = mcBuf();
    for (int j = 0; j < h; j++) {
        uint16_t *p = buf + (y + j) * MC_W + x;
        for (int i = 0; i < w; i++) p[i] = mcSwap(mcBlend(mcSwap(p[i]), 0, keep) );
    }
}

void mcBlitRGB(int x, int y, int w, int h, const uint16_t *px) {
    int ow = w, sx, sy;
    if (!clip(x, y, w, h, sx, sy)) return;
    uint16_t *buf = mcBuf();
    for (int j = 0; j < h; j++) {
        const uint16_t *src = px + (sy + j) * ow + sx;
        uint16_t *dst = buf + (y + j) * MC_W + x;
        for (int i = 0; i < w; i++) dst[i] = mcSwap(src[i]);
    }
}

void mcBlitAlpha(int x, int y, int w, int h, const uint16_t *px, const uint8_t *a) {
    int ow = w, sx, sy;
    if (!clip(x, y, w, h, sx, sy)) return;
    uint16_t *buf = mcBuf();
    for (int j = 0; j < h; j++) {
        int so = (sy + j) * ow + sx;
        uint16_t *dst = buf + (y + j) * MC_W + x;
        for (int i = 0; i < w; i++) {
            uint8_t al = a[so + i];
            if (al) dst[i] = mcSwap(mcBlend(px[so + i], mcSwap(dst[i]), al));
        }
    }
}

void mcBlitAdd(int x, int y, int w, int h, const uint16_t *px, uint8_t level) {
    int ow = w, sx, sy;
    if (!clip(x, y, w, h, sx, sy)) return;
    uint16_t *buf = mcBuf();
    for (int j = 0; j < h; j++) {
        const uint16_t *src = px + (sy + j) * ow + sx;
        uint16_t *dst = buf + (y + j) * MC_W + x;
        for (int i = 0; i < w; i++) {
            uint16_t s = pgm_read_word(src + i);
            if (!s) continue;
            uint16_t d = mcSwap(dst[i]);
            uint32_t r = ((d >> 11) & 31) + ((((s >> 11) & 31) * level) >> 8);
            uint32_t g = ((d >> 5) & 63) + ((((s >> 5) & 63) * level) >> 8);
            uint32_t b = (d & 31) + (((s & 31) * level) >> 8);
            dst[i] = mcSwap((uint16_t)(((r > 31 ? 31 : r) << 11) | ((g > 63 ? 63 : g) << 5) | (b > 31 ? 31 : b)));
        }
    }
}

void mcDrawMascot(int frame, int x, int y, int scale) {
    frame = ((frame % MASCOT_FRAMES) + MASCOT_FRAMES) % MASCOT_FRAMES;
    uint16_t *buf = mcBuf();
    const uint8_t *base = MASCOT_PIX + (size_t)frame * MASCOT_H * MASCOT_STRIDE;
    for (int row = 0; row < MASCOT_H; row++) {
        for (int col = 0; col < MASCOT_W; col++) {
            uint8_t b = pgm_read_byte(base + row * MASCOT_STRIDE + (col >> 1));
            uint8_t idx = (col & 1) ? (b & 0x0F) : (b >> 4);
            if (!idx) continue;
            uint16_t c = mcSwap(pgm_read_word(MASCOT_PAL + idx));
            for (int dy = 0; dy < scale; dy++) {
                int py = y + row * scale + dy;
                if (py < 0 || py >= MC_H) continue;
                for (int dx = 0; dx < scale; dx++) {
                    int pxx = x + col * scale + dx;
                    if (pxx >= 0 && pxx < MC_W) buf[py * MC_W + pxx] = c;
                }
            }
        }
    }
}

void mcText(const char *s, int x, int y, const lgfx::IFont *font, uint16_t color, uint8_t datum, bool shadow) {
    mcCanvas.setFont(font);
    mcCanvas.setTextDatum(datum);
    if (shadow) {
        mcCanvas.setTextColor(0x0000);
        mcCanvas.drawString(s, x + 1, y + 1);
    }
    mcCanvas.setTextColor(color);
    mcCanvas.drawString(s, x, y);
}

// ------------------------------------------------------------------ images
bool mcLoadMg(const char *path, uint16_t *dst) {
    File f = SD.open(path);
    if (!f) return false;
    uint8_t hdr[8];
    bool ok = f.size() == 153608 && f.read(hdr, 8) == 8 && memcmp(hdr, "MG01", 4) == 0 && hdr[4] == 0x40 &&
              hdr[5] == 0x01 && hdr[6] == 0xF0 && hdr[7] == 0x00;
    if (ok) ok = f.read((uint8_t *)dst, 153600) == 153600; // little-endian RGB565 == native order
    f.close();
    return ok;
}

bool mcLoadIcon(const char *path, McIcon &out) {
    File f = SD.open(path);
    if (!f) return false;
    uint8_t hdr[8];
    bool ok = f.read(hdr, 8) == 8 && memcmp(hdr, "MI01", 4) == 0 && hdr[4] == 64 && hdr[5] == 0 && hdr[6] == 64 &&
              hdr[7] == 0;
    if (ok) {
        out.rgb = (uint16_t *)ps_malloc(64 * 64 * 2);
        out.a = (uint8_t *)ps_malloc(64 * 64);
        ok = out.rgb && out.a && f.read((uint8_t *)out.rgb, 8192) == 8192 && f.read(out.a, 4096) == 4096;
        if (!ok) mcFreeIcon(out);
    }
    f.close();
    return ok;
}

void mcFreeIcon(McIcon &icon) {
    free(icon.rgb);
    free(icon.a);
    icon.rgb = nullptr;
    icon.a = nullptr;
}

// ------------------------------------------------------------------ settings
void mcConfigLoad() {
    Preferences p;
    p.begin("mantis", true);
    mcCfg.ssid = p.getString("ssid", "");
    mcCfg.pass = p.getString("pass", "");
    mcCfg.zip = p.getString("zip", "");
    mcCfg.bright = p.getUChar("bright", 70);
    mcCfg.tzIndex = p.getUChar("tz", 3);
    mcCfg.h24 = p.getBool("h24", false);
    mcCfg.customBg = p.getBool("custbg", false);
    mcCfg.bgIndex = p.getUShort("bgidx", 0);
    mcCfg.metric = p.getBool("metric", false);
    p.end();
    if (mcCfg.tzIndex >= mcTimeZoneCount) mcCfg.tzIndex = 3;
    if (mcCfg.bright < 5 || mcCfg.bright > 100) mcCfg.bright = 70;
}

void mcConfigSave() {
    Preferences p;
    p.begin("mantis", false);
    p.putString("ssid", mcCfg.ssid);
    p.putString("pass", mcCfg.pass);
    p.putString("zip", mcCfg.zip);
    p.putUChar("bright", mcCfg.bright);
    p.putUChar("tz", mcCfg.tzIndex);
    p.putBool("h24", mcCfg.h24);
    p.putBool("custbg", mcCfg.customBg);
    p.putUShort("bgidx", mcCfg.bgIndex);
    p.putBool("metric", mcCfg.metric);
    p.end();
}

void mcApplyBrightness() { M5.Display.setBrightness((uint8_t)(mcCfg.bright * 255 / 100)); }

// ------------------------------------------------------------------ time
const McTimeZone mcTimeZones[] = {
    {"Hawaii",       "HST10"                         },
    {"Alaska",       "AKST9AKDT,M3.2.0,M11.1.0"      },
    {"Pacific",      "PST8PDT,M3.2.0,M11.1.0"        },
    {"Mountain",     "MST7MDT,M3.2.0,M11.1.0"        },
    {"Arizona",      "MST7"                          },
    {"Central",      "CST6CDT,M3.2.0,M11.1.0"        },
    {"Eastern",      "EST5EDT,M3.2.0,M11.1.0"        },
    {"Atlantic",     "AST4ADT,M3.2.0,M11.1.0"        },
    {"UTC",          "UTC0"                          },
    {"London",       "GMT0BST,M3.5.0/1,M10.5.0"      },
    {"Central Eur.", "CET-1CEST,M3.5.0,M10.5.0/3"    },
    {"India",        "IST-5:30"                      },
    {"Japan",        "JST-9"                         },
    {"Sydney",       "AEST-10AEDT,M10.1.0,M4.1.0/3"  },
};
const int mcTimeZoneCount = sizeof(mcTimeZones) / sizeof(mcTimeZones[0]);

void mcApplyTimeZone() {
    setenv("TZ", mcTimeZones[mcCfg.tzIndex].posix, 1);
    tzset();
}

static time_t utcMktime(struct tm *t) { // newlib has no timegm()
    setenv("TZ", "UTC0", 1);
    tzset();
    time_t r = mktime(t);
    mcApplyTimeZone();
    return r;
}

void mcTimeFromRtc() {
    if (!M5.Rtc.isEnabled()) return;
    auto dt = M5.Rtc.getDateTime();
    if (dt.date.year < 2024) return; // RTC never set
    struct tm t = {};
    t.tm_year = dt.date.year - 1900;
    t.tm_mon = dt.date.month - 1;
    t.tm_mday = dt.date.date;
    t.tm_hour = dt.time.hours;
    t.tm_min = dt.time.minutes;
    t.tm_sec = dt.time.seconds;
    struct timeval tv = {utcMktime(&t), 0};
    settimeofday(&tv, nullptr);
}

void mcTimeToRtc(time_t utc) {
    struct timeval tv = {utc, 0};
    settimeofday(&tv, nullptr);
    if (!M5.Rtc.isEnabled()) return;
    struct tm t;
    gmtime_r(&utc, &t);
    auto dt = M5.Rtc.getDateTime(); // same type as setDateTime() takes
    dt.date.year = t.tm_year + 1900;
    dt.date.month = t.tm_mon + 1;
    dt.date.date = t.tm_mday;
    dt.date.weekDay = t.tm_wday;
    dt.time.hours = t.tm_hour;
    dt.time.minutes = t.tm_min;
    dt.time.seconds = t.tm_sec;
    M5.Rtc.setDateTime(dt);
}

String mcClockString(bool withSeconds) {
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    char buf[16];
    if (mcCfg.h24) {
        strftime(buf, sizeof(buf), withSeconds ? "%H:%M:%S" : "%H:%M", &t);
    } else {
        int h = t.tm_hour % 12;
        if (h == 0) h = 12;
        if (withSeconds) snprintf(buf, sizeof(buf), "%d:%02d:%02d %s", h, t.tm_min, t.tm_sec, t.tm_hour < 12 ? "AM" : "PM");
        else snprintf(buf, sizeof(buf), "%d:%02d %s", h, t.tm_min, t.tm_hour < 12 ? "AM" : "PM");
    }
    return String(buf);
}

bool mcIsCharging() { return (int)M5.Power.isCharging() == 1; } // 0 discharging, 1 charging, 2 unknown


// ------------------------------------------------------------------ touch
void mcPollTouch() {
    M5.update();
    bool now = M5.Touch.getCount() > 0;
    mcTouch.pressed = mcTouch.released = false;
    if (now) {
        auto d = M5.Touch.getDetail();
        int x = constrain((int)d.x, 0, MC_W - 1), y = constrain((int)d.y, 0, MC_H + 39);
        if (!mcTouch.down) {
            mcTouch.pressed = true;
            mcTouch.startX = x;
            mcTouch.startY = y;
            mcTouch.downAt = millis();
            mcTouch.moved = false;
            mcTouch.longFired = false;
        }
        mcTouch.x = x;
        mcTouch.y = y;
        if (abs(x - mcTouch.startX) > 10 || abs(y - mcTouch.startY) > 10) mcTouch.moved = true;
    } else if (mcTouch.down) {
        mcTouch.released = true;
    }
    mcTouch.down = now;
}

bool mcWaitRelease(uint32_t timeoutMs) {
    uint32_t t0 = millis();
    do {
        mcPollTouch();
        if (!mcTouch.down) return true;
        delay(10);
    } while (millis() - t0 < timeoutMs);
    return false;
}

// ------------------------------------------------------------------ loading screen
static uint16_t *loadingBg = nullptr;
static bool loadingBgTried = false;
static String loadingTitle, loadingStatus;
static float loadingProgress = -1;
static uint32_t loadingLastFrame = 0;
static bool loadingActive = false;

void mcLoadingBegin(const String &title) {
    mcGfxInit();
    if (!loadingBgTried) {
        loadingBgTried = true;
        loadingBg = (uint16_t *)ps_malloc(MC_W * MC_H * 2);
        if (loadingBg && !mcLoadMg("/mantiscore/loading.mg", loadingBg)) {
            free(loadingBg);
            loadingBg = nullptr;
        }
    }
    loadingTitle = title;
    loadingStatus = "";
    loadingProgress = -1;
    loadingActive = true;
    loadingLastFrame = 0;
    mcLoadingFrame(-1, "Getting ready...");
}

void mcLoadingFrame(float progress, const String &status) {
    if (!loadingActive) mcLoadingBegin("MantisCore");
    loadingProgress = progress;
    if (status.length()) loadingStatus = status;
    uint32_t now = millis();
    if (now - loadingLastFrame < 45 && progress < 1.0f) return;
    loadingLastFrame = now;

    if (loadingBg) {
        mcBlitRGB(0, 0, MC_W, MC_H, loadingBg);
        mcDarkenRect(0, 0, MC_W, MC_H, 110);
    } else {
        mcCanvas.fillScreen(MC_CHARCOAL);
    }
    mcCanvas.fillSmoothRoundRect(30, 18, 260, 210, 14, MC_CHARCOAL);
    mcCanvas.drawRoundRect(30, 18, 260, 210, 14, MC_TEAL);
    int frame = progress >= 1.0f ? 15 : (int)((now / 90) % 15);
    mcDrawMascot(frame, 160 - MASCOT_W, 26, 2);
    mcText(loadingTitle.c_str(), 160, 168, &fonts::FreeSansBold9pt7b, MC_WHITE, middle_center);
    // progress bar
    const int bx = 50, by = 186, bw = 220, bh = 12;
    mcCanvas.fillSmoothRoundRect(bx, by, bw, bh, 6, 0x0841);
    if (progress >= 0) {
        int fw = (int)(bw * (progress > 1 ? 1 : progress));
        if (fw > 6) mcCanvas.fillSmoothRoundRect(bx, by, fw, bh, 6, MC_GREEN);
    } else {
        int seg = 60, pos = (int)((now / 8) % (bw + seg)) - seg;
        int x0 = max(bx, bx + pos), x1 = min(bx + bw, bx + pos + seg);
        if (x1 - x0 > 6) mcCanvas.fillSmoothRoundRect(x0, by, x1 - x0, bh, 6, MC_GREEN);
    }
    mcCanvas.drawRoundRect(bx - 1, by - 1, bw + 2, bh + 2, 7, MC_TEAL);
    mcText(loadingStatus.c_str(), 160, 212, &fonts::Font2, MC_GREY, middle_center);
    mcPush();
}

void mcToast(const String &text, uint16_t color, uint32_t ms) {
    mcGfxInit();
    mcCanvas.setFont(&fonts::FreeSansBold9pt7b);
    int w = min(300, (int)mcCanvas.textWidth(text.c_str()) + 32);
    int x = (MC_W - w) / 2, y = 190;
    mcCanvas.fillSmoothRoundRect(x, y, w, 34, 17, MC_CHARCOAL);
    mcCanvas.drawRoundRect(x, y, w, 34, 17, color);
    mcText(text.c_str(), 160, y + 17, &fonts::FreeSansBold9pt7b, MC_WHITE, middle_center);
    mcPush();
    delay(ms);
}

// ------------------------------------------------------------------ engine hooks (called from engine/engine_glue.cpp)
static String friendlyStatus(const String &s) {
    // Engine wording -> kid-friendly wording
    if (s.startsWith("Backing up")) return "Saving your progress...";
    if (s.startsWith("Optimizing")) return "Tidying up storage...";
    if (s.startsWith("Removing")) return "Removing app...";
    if (s.startsWith("Clearing") || s.startsWith("Writing table")) return "Almost done...";
    return s;
}
void mantisEngineStatus(const String &text) { mcLoadingFrame(loadingProgress, friendlyStatus(text)); }
void mantisEngineProgress(size_t progress, size_t total) {
    mcLoadingFrame(total ? (float)progress / (float)total : -1, "");
}
void mantisEngineError(const String &text) { mcToast(text, 0xF800, 2500); }
void mantisEngineMessage(const String &text) {
    if (text == "Backup saved") return; // routine, keep quiet
    mcToast(friendlyStatus(text), MC_TEAL, 1200);
}

// ------------------------------------------------------------------ splash
static uint32_t splashStart = 0, splashLast = 0;

void mantisSplashBegin() {
    mcConfigLoad();
    mcGfxInit();
    splashStart = millis();
    splashLast = 0;
    mantisSplashFrame();
}

void mantisSplashFrame() {
    uint32_t now = millis();
    if (now - splashLast < 70) return;
    splashLast = now;
    mcCanvas.fillScreen(MC_CHARCOAL);
    // soft teal glow behind the mascot
    for (int r = 70; r > 0; r -= 10) mcCanvas.fillCircle(160, 92, r, mcBlend(MC_TEAL, MC_CHARCOAL, 90 - r));
    mcDrawMascot((int)((now - splashStart) / 90) % 15, 160 - MASCOT_W, 26, 2);
    mcText("MantisCore", 160, 176, &fonts::FreeSansBold12pt7b, MC_GREEN, middle_center, true);
    mcText("by BasaltSoftworks", 160, 202, &fonts::Font2, MC_GREY, middle_center);
    uint8_t pulse = (uint8_t)(128 + 127 * sinf(now / 300.0f));
    mcText("Tap to open", 160, 226, &fonts::Font2, mcBlend(MC_WHITE, MC_CHARCOAL, pulse), middle_center);
    mcPush();
}
