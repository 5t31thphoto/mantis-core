// MantisCore weather instrument: full-screen weather on the workbench chassis.
//  - nixie tubes: temperature (or the air-quality number on the AIR page)
//  - CRT: auto-cycling pages NOW / TODAY / WIND / SUN / AIR with icons and plain words
//  - LED readouts: PM2.5, PM10, CO, OZONE, UV, RAIN, WIND, HUMIDITY with green/amber/red lamps
//  - waveform display: the next 12 hours of temperature with rain-chance bars
// Tap anywhere to go back.
#include "mantis_assets.h"
#include "mantis_internal.h"
#include "mantis_weather_assets.h"
#include <math.h>
#include <time.h>

#define MC_AMBER 0xFD20
#define MC_RED 0xF8A2
#define MC_DIMGREEN 0x0320
#define LVL_OK 0
#define LVL_WARN 1
#define LVL_BAD 2

static uint16_t *wxBg = nullptr;

// ------------------------------------------------------------------ small drawing helpers
static void degreeMark(int x, int y, int r, uint16_t c) { mcCanvas.drawCircle(x + r, y + r, r, c); }

// text plus a drawn degree mark (bitmap fonts have no reliable degree glyph)
static void textDeg(const char *s, int x, int y, const lgfx::IFont *f, uint16_t c, uint8_t datum, int r = 1) {
    mcCanvas.setFont(f);
    int w = mcCanvas.textWidth(s), h = mcCanvas.fontHeight();
    int left = x;
    if (datum == middle_center || datum == top_center) left = x - (w + 2 * r + 2) / 2;
    else if (datum == middle_right) left = x - (w + 2 * r + 2);
    int top = (datum == top_center || datum == top_left) ? y : y - h / 2;
    mcText(s, left, top, f, c, top_left);
    degreeMark(left + w + 1, top + 1, r, c);
}

static uint16_t lampColor(int lvl) { return lvl == LVL_OK ? MC_GREEN : lvl == LVL_WARN ? MC_AMBER : MC_RED; }

static void lamp(int cx, int cy, int lvl, bool on) {
    uint16_t c = on ? lampColor(lvl) : 0x2104;
    if (on) mcCanvas.fillCircle(cx, cy, 3, mcBlend(c, 0x0000, 70)); // glow
    mcCanvas.fillCircle(cx, cy, 2, c);
    if (on) mcCanvas.drawPixel(cx - 1, cy - 1, MC_WHITE);
}

// ------------------------------------------------------------------ meaning of the numbers
static int iconFor(int code) {
    if (code == 0 || code == 1) return WX_SUN;
    if (code == 2) return WX_PARTLY;
    if (code == 3) return WX_CLOUD;
    if (code == 45 || code == 48) return WX_FOG;
    if ((code >= 51 && code <= 57) || code == 61 || code == 80) return WX_RAIN;
    if ((code >= 63 && code <= 67) || code == 81 || code == 82) return WX_HEAVY;
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) return WX_SNOW;
    if (code >= 95) return WX_STORM;
    return WX_CLOUD;
}
static void wordsFor(int code, bool isDay, const char *&a, const char *&b) {
    b = "";
    if (code == 0) a = isDay ? "SUNNY" : "CLEAR";
    else if (code == 1) { a = "MOSTLY"; b = isDay ? "SUNNY" : "CLEAR"; }
    else if (code == 2) { a = "PARTLY"; b = "CLOUDY"; }
    else if (code == 3) a = "CLOUDY";
    else if (code == 45 || code == 48) a = "FOGGY";
    else if (code >= 51 && code <= 57) a = "DRIZZLE";
    else if (code == 61 || code == 80) { a = "LIGHT"; b = "RAIN"; }
    else if ((code >= 63 && code <= 65) || code == 81) a = "RAIN";
    else if (code == 82 || code == 65) { a = "HEAVY"; b = "RAIN"; }
    else if (code == 66 || code == 67) { a = "ICY"; b = "RAIN"; }
    else if ((code >= 71 && code <= 77) || code == 85 || code == 86) a = "SNOW";
    else if (code >= 95) { a = "THUNDER"; b = "STORM"; }
    else a = "--";
}
static bool isRainy(int c) { return (c >= 51 && c <= 67) || (c >= 80 && c <= 82); }
static bool isStormy(int c) { return c >= 95; }

static const char *aqiWord(int aqi) {
    if (aqi <= 50) return "GOOD";
    if (aqi <= 100) return "OKAY";
    if (aqi <= 150) return "SENSITIVE";
    if (aqi <= 200) return "UNHEALTHY";
    if (aqi <= 300) return "VERY BAD";
    return "HAZARD";
}
static int aqiLevel(int aqi) { return aqi <= 50 ? LVL_OK : aqi <= 100 ? LVL_WARN : LVL_BAD; }
static const char *compass(int deg) {
    static const char *const d[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    return d[((deg % 360) + 360 + 22) % 360 / 45];
}
static void fmtTime(const char *hhmm, char *out, size_t n) { // "18:31" -> "6:31P" / "18:31"
    if (strlen(hhmm) < 5) { snprintf(out, n, "--"); return; }
    int h = atoi(hhmm), m = atoi(hhmm + 3);
    if (mcCfg.h24) snprintf(out, n, "%02d:%02d", h, m);
    else snprintf(out, n, "%d:%02d%c", h % 12 == 0 ? 12 : h % 12, m, h < 12 ? 'A' : 'P');
}

// ------------------------------------------------------------------ nixie tubes
static void drawTubes(int value, bool show, uint8_t flick) {
    const int GSZ = NIXIE_GW * NIXIE_GH;
    int glyph[4] = {-1, -1, -1, -1}; // -1 off, 0-9 digit, 10 minus
    if (show) { // centred in the 4 tubes, e.g. " 72 ", "-12 ", "105 "
        int v = value < 0 ? -value : value;
        if (v > 999) v = 999;
        int digits[4], n = 0;
        do {
            digits[n++] = v % 10;
            v /= 10;
        } while (v && n < 3);
        int len = n + (value < 0 ? 1 : 0);
        int pos = (4 - len) / 2;
        if (value < 0) glyph[pos++] = 10;
        while (n) glyph[pos++] = digits[--n];
    }
    for (int i = 0; i < 4; i++) {
        if (glyph[i] < 0) continue;
        const uint16_t *g = glyph[i] == 10 ? WX_NIXIE_MINUS : NIXIE_GLYPHS + glyph[i] * GSZ;
        mcBlitAdd(WX_TUBE_XY[i][0], WX_TUBE_XY[i][1], NIXIE_GW, NIXIE_GH, g, i == 2 ? flick : 255);
    }
}

static void drawPlate(const char *label, bool deg, bool metric) {
    int x0 = WX_PLATE[0], y0 = WX_PLATE[1], x1 = WX_PLATE[2], y1 = WX_PLATE[3];
    int cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
    char t[32];
    snprintf(t, sizeof(t), "%s", label);
    mcCanvas.setFont(&fonts::Font0);
    int w = mcCanvas.textWidth(t) + (deg ? 12 : 0);
    int left = cx - w / 2;
    mcText(t, left + 1, cy + 1, &fonts::Font0, 0x4208, middle_left); // engraved shadow
    mcText(t, left, cy, &fonts::Font0, 0x18C3, middle_left);
    if (deg) {
        int dx = left + mcCanvas.textWidth(t) + 2;
        degreeMark(dx, cy - 4, 1, 0x18C3);
        mcText(metric ? "C" : "F", dx + 4, cy, &fonts::Font0, 0x18C3, middle_left);
    }
}

// ------------------------------------------------------------------ LED readouts
static void cell(int i, const char *label, const char *value, int lvl, bool on) {
    int x0 = WX_CELLS[i][0], y0 = WX_CELLS[i][1], x1 = WX_CELLS[i][2];
    mcText(label, (x0 + x1) / 2 + 1, y0 + 6, &fonts::Font0, 0x8C71, middle_center);
    lamp(x0 + 4, y0 + 15, lvl, on);
    mcText(value, x1 - 3, y0 + 15, &fonts::Font0, on ? lampColor(lvl) : 0x632C, middle_right);
}

static void fmtNum(char *b, size_t n, float v, bool ok, int decimalsBelow10 = 1) {
    if (!ok) snprintf(b, n, "--");
    else if (v < 10 && decimalsBelow10) snprintf(b, n, "%.1f", v);
    else snprintf(b, n, "%d", (int)lroundf(v));
}

static void drawCells(const McWeather &w) {
    char b[12];
    bool a = w.aqiValid;
    fmtNum(b, sizeof(b), w.pm25, a);
    cell(0, "PM2.5", b, w.pm25 <= 12 ? LVL_OK : w.pm25 <= 35 ? LVL_WARN : LVL_BAD, a);
    fmtNum(b, sizeof(b), w.pm10, a);
    cell(1, "PM10", b, w.pm10 <= 54 ? LVL_OK : w.pm10 <= 154 ? LVL_WARN : LVL_BAD, a);
    fmtNum(b, sizeof(b), w.co, a, 0);
    cell(2, "CO", b, w.co <= 4000 ? LVL_OK : w.co <= 10000 ? LVL_WARN : LVL_BAD, a);
    fmtNum(b, sizeof(b), w.o3, a, 0);
    cell(3, "OZONE", b, w.o3 <= 100 ? LVL_OK : w.o3 <= 160 ? LVL_WARN : LVL_BAD, a);
    fmtNum(b, sizeof(b), w.uv, w.valid);
    cell(4, "UV", b, w.uv < 3 ? LVL_OK : w.uv < 6 ? LVL_WARN : LVL_BAD, w.valid);
    if (!w.valid) snprintf(b, sizeof(b), "--");
    else if (w.precip <= 0) snprintf(b, sizeof(b), "0");
    else if (!w.metric && w.precip < 1) snprintf(b, sizeof(b), ".%02d", (int)lroundf(w.precip * 100) % 100);
    else fmtNum(b, sizeof(b), w.precip, true);
    float heavy = w.metric ? 7.6f : 0.3f;
    cell(5, "RAIN", b, w.precip <= 0 ? LVL_OK : w.precip < heavy ? LVL_WARN : LVL_BAD, w.valid);
    fmtNum(b, sizeof(b), w.wind, w.valid, 0);
    float windWarn = w.metric ? 24 : 15, windBad = w.metric ? 40 : 25;
    cell(6, "WIND", b, w.wind < windWarn ? LVL_OK : w.wind < windBad ? LVL_WARN : LVL_BAD, w.valid);
    if (w.valid) snprintf(b, sizeof(b), "%d%%", w.humidity);
    else snprintf(b, sizeof(b), "--");
    cell(7, "HUMID", b, (w.humidity >= 25 && w.humidity <= 65) ? LVL_OK : LVL_WARN, w.valid);
}

// ------------------------------------------------------------------ 12-hour waveform
static void drawWave(const McWeather &w, uint32_t now) {
    int x0 = WX_WAVE[0] + 2, y0 = WX_WAVE[1] + 2, x1 = WX_WAVE[2] - 2, y1 = WX_WAVE[3] - 2;
    for (int gx = x0; gx <= x1; gx += 10) mcCanvas.drawFastVLine(gx, y0, y1 - y0, 0x0141);
    for (int gy = y0; gy <= y1; gy += 8) mcCanvas.drawFastHLine(x0, gy, x1 - x0, 0x0141);
    mcText("12H", x0 + 1, y0 + 4, &fonts::Font0, MC_DIMGREEN, middle_left);
    if (!w.valid || w.hours < 2) {
        mcText("NO DATA", (x0 + x1) / 2, (y0 + y1) / 2 + 3, &fonts::Font0, MC_DIMGREEN, middle_center);
        return;
    }
    float lo = w.hourT[0], hi = w.hourT[0];
    for (int i = 1; i < w.hours; i++) { lo = min(lo, w.hourT[i]); hi = max(hi, w.hourT[i]); }
    if (hi - lo < 4) { float m = (hi + lo) / 2; lo = m - 2; hi = m + 2; }
    int gTop = y0 + 10, gBot = y1 - 7, n = w.hours;
    // rain chance bars along the bottom
    for (int i = 0; i < n; i++) {
        int bx = x0 + (x1 - x0) * i / (n - 1);
        int bh = (y1 - gTop) * w.hourPop[i] / 100 / 3;
        if (bh > 0) mcCanvas.fillRect(bx - 1, y1 - bh, 3, bh, 0x2B5F);
    }
    int px = 0, py = 0;
    for (int i = 0; i < n; i++) {
        int x = x0 + (x1 - x0) * i / (n - 1);
        int y = gBot - (int)((w.hourT[i] - lo) / (hi - lo) * (gBot - gTop));
        if (i) {
            mcCanvas.drawLine(px, py, x, y, MC_GREEN);
            mcCanvas.drawLine(px, py + 1, x, y + 1, MC_DIMGREEN);
        }
        px = x;
        py = y;
    }
    // sweeping "scope" dot
    int k = (now / 250) % n;
    int sx = x0 + (x1 - x0) * k / (n - 1);
    int sy = gBot - (int)((w.hourT[k] - lo) / (hi - lo) * (gBot - gTop));
    mcCanvas.fillCircle(sx, sy, 2, MC_WHITE);
    char b[8];
    snprintf(b, sizeof(b), "%d", (int)lroundf(hi));
    textDeg(b, x1 - 1, y0 + 4, &fonts::Font0, MC_GREEN, middle_right);
}

// ------------------------------------------------------------------ CRT pages
enum { PG_NOW, PG_TODAY, PG_WIND, PG_SUN, PG_AIR, PG_COUNT };

static void crtIcon(int icon, int x, int y, bool isDay) {
    if (icon == WX_SUN && !isDay) { // clear night: moon
        mcCanvas.fillCircle(x + 16, y + 16, 11, 0xEF5B);
        mcCanvas.fillCircle(x + 22, y + 11, 10, 0x0081); // CRT glass colour bites the crescent
        return;
    }
    mcBlitAlpha(x, y, WX_ICON, WX_ICON, WX_ICON_RGB + icon * WX_ICON * WX_ICON, WX_ICON_A + icon * WX_ICON * WX_ICON);
}

static void drawCrt(const McWeather &w, int page, uint32_t now) {
    const int X = WX_CRT_X, Y = WX_CRT_Y, W = WX_CRT_W, H = WX_CRT_H, cx = X + W / 2;
    const uint16_t G = MC_GREEN;
    char b[24];
    if (!w.valid) {
        const char *msg[4] = {"NO DATA", mcCfg.ssid.isEmpty() ? "SET WI-FI" : (mcWifiUp ? "LOADING" : "NO WI-FI"),
                              mcCfg.zip.isEmpty() ? "& ZIP IN" : "", mcCfg.zip.isEmpty() ? "SETTINGS" : ""};
        for (int i = 0; i < 4; i++) mcText(msg[i], cx, Y + 18 + i * 12, &fonts::Font0, G, middle_center);
    } else if (page == PG_NOW) {
        crtIcon(iconFor(w.code), cx - 16, Y + 3, w.isDay);
        if (isStormy(w.code) && (now / 120) % 25 < 3) // lightning flash
            mcBlitAdd(cx - WX_BOLT / 2, Y + 4, WX_BOLT, WX_BOLT, WX_BOLT_FRAMES + ((now / 120) % 3) * WX_BOLT * WX_BOLT, 255);
        const char *a, *c;
        wordsFor(w.code, w.isDay, a, c);
        mcText(a, cx, Y + 41, &fonts::Font0, G, middle_center);
        if (*c) mcText(c, cx, Y + 51, &fonts::Font0, G, middle_center);
        snprintf(b, sizeof(b), "FEELS %d", (int)lroundf(w.feels));
        textDeg(b, cx, Y + 63, &fonts::Font0, MC_AMBER, middle_center);
    } else if (page == PG_TODAY) {
        mcText("TODAY", cx, Y + 7, &fonts::Font0, G, middle_center);
        snprintf(b, sizeof(b), "HI %d", (int)lroundf(w.hi));
        textDeg(b, cx, Y + 21, &fonts::Font0, MC_AMBER, middle_center);
        snprintf(b, sizeof(b), "LO %d", (int)lroundf(w.lo));
        textDeg(b, cx, Y + 32, &fonts::Font0, 0x5D9F, middle_center);
        snprintf(b, sizeof(b), "RAIN %d%%", w.popMax);
        mcText(b, cx, Y + 45, &fonts::Font0, G, middle_center);
        snprintf(b, sizeof(b), "UV MAX %d", (int)lroundf(w.uvMax));
        mcText(b, cx, Y + 56, &fonts::Font0, G, middle_center);
        if (w.cloud >= 100) snprintf(b, sizeof(b), "OVERCAST");
        else snprintf(b, sizeof(b), "CLOUD %d%%", w.cloud);
        mcText(b, cx, Y + 67, &fonts::Font0, MC_DIMGREEN, middle_center);
    } else if (page == PG_WIND) {
        const int r = 15, ccy = Y + 21;
        mcCanvas.drawCircle(cx, ccy, r, MC_DIMGREEN);
        mcText("N", cx, ccy - r - 1, &fonts::Font0, G, middle_center);
        // arrow points where the wind is blowing TO
        float a = (w.windDir + 180) * (float)M_PI / 180.0f;
        int tx = cx + (int)(sinf(a) * (r - 3)), ty = ccy - (int)(cosf(a) * (r - 3));
        int bx = cx - (int)(sinf(a) * (r - 5)), by = ccy + (int)(cosf(a) * (r - 5));
        mcCanvas.drawLine(bx, by, tx, ty, MC_AMBER);
        mcCanvas.drawLine(bx + 1, by, tx + 1, ty, MC_AMBER);
        mcCanvas.fillCircle(tx, ty, 2, MC_AMBER);
        snprintf(b, sizeof(b), "%d %s", (int)lroundf(w.wind), w.metric ? "KM/H" : "MPH");
        mcText(b, cx, Y + 45, &fonts::Font0, G, middle_center);
        snprintf(b, sizeof(b), "FROM %s", compass(w.windDir));
        mcText(b, cx, Y + 56, &fonts::Font0, G, middle_center);
        snprintf(b, sizeof(b), "GUST %d", (int)lroundf(w.gust));
        mcText(b, cx, Y + 67, &fonts::Font0, MC_DIMGREEN, middle_center);
    } else if (page == PG_SUN) {
        char t[10];
        mcCanvas.fillCircle(cx, Y + 13, 6, 0xFE60);
        for (int i = 0; i < 8; i++) {
            float a = i * (float)M_PI / 4 + now / 900.0f;
            mcCanvas.drawLine(cx + (int)(cosf(a) * 8), Y + 13 + (int)(sinf(a) * 8), cx + (int)(cosf(a) * 11),
                              Y + 13 + (int)(sinf(a) * 11), 0xFE60);
        }
        fmtTime(w.sunrise, t, sizeof(t));
        mcText("SUNRISE", cx, Y + 31, &fonts::Font0, MC_DIMGREEN, middle_center);
        mcText(t, cx, Y + 41, &fonts::Font0, G, middle_center);
        fmtTime(w.sunset, t, sizeof(t));
        mcText("SUNSET", cx, Y + 54, &fonts::Font0, MC_DIMGREEN, middle_center);
        mcText(t, cx, Y + 64, &fonts::Font0, G, middle_center);
    } else { // PG_AIR
        mcText("AIR", cx, Y + 8, &fonts::Font0, G, middle_center);
        mcText("QUALITY", cx, Y + 18, &fonts::Font0, G, middle_center);
        mcText(aqiWord(w.aqi), cx, Y + 34, &fonts::Font0, lampColor(aqiLevel(w.aqi)), middle_center);
        // AQI scale bar with marker
        const int bx = X + 5, bw = W - 10, by = Y + 46;
        const uint16_t seg[6] = {0x07E0, 0xFFE0, 0xFD20, 0xF800, 0x8010, 0x7800};
        for (int i = 0; i < 6; i++) mcCanvas.fillRect(bx + bw * i / 6, by, bw / 6 + 1, 4, seg[i]);
        int mx = bx + min(bw - 1, bw * min(w.aqi, 300) / 300);
        mcCanvas.fillTriangle(mx, by + 5, mx - 3, by + 10, mx + 3, by + 10, MC_WHITE);
        snprintf(b, sizeof(b), "AQI %d", w.aqi);
        mcText(b, cx, Y + 66, &fonts::Font0, G, middle_center);
    }
    // page dots
    if (w.valid) {
        int pages = w.aqiValid ? PG_COUNT : PG_COUNT - 1;
        for (int i = 0; i < pages; i++) mcCanvas.fillRect(cx - pages * 3 + i * 6, Y + H - 3, 3, 2, i == page ? G : MC_DIMGREEN);
    }
    if (w.valid && page == PG_NOW && isRainy(w.code)) // rain streaks over the glass
        mcBlitAdd(X, Y, W, H, WX_RAIN_FRAMES + ((now / 110) % 4) * W * H, 200);
    // scanlines + slight flicker
    uint8_t keep = 205 + ((now / 70) % 7 == 0 ? 20 : 0);
    for (int y = Y + 1; y < Y + H; y += 2) mcDarkenRect(X, y, W, 1, keep);
}

// ------------------------------------------------------------------ header
static void drawHeader(const McWeather &w) {
    mcText("LOCAL CONDITIONS", 6, WX_HEADER_H / 2 + 1, &fonts::Font0, MC_AMBER, middle_left);
    if (w.place[0]) {
        char p[20];
        snprintf(p, sizeof(p), "%.18s", w.place); // room left before the update time
        mcText(p, 106, WX_HEADER_H / 2 + 1, &fonts::Font0, MC_WHITE, middle_left);
    }
    char b[32] = "NO DATA";
    int lvl = LVL_BAD;
    if (w.valid && w.updated > 0) {
        struct tm t;
        localtime_r(&w.updated, &t);
        int h = t.tm_hour;
        if (mcCfg.h24) snprintf(b, sizeof(b), "UPDATED %02d:%02d", h, t.tm_min);
        else snprintf(b, sizeof(b), "UPDATED %d:%02d%s", h % 12 == 0 ? 12 : h % 12, t.tm_min, h < 12 ? "A" : "P");
        long age = (long)(time(nullptr) - w.updated);
        lvl = age < 30 * 60 ? LVL_OK : LVL_WARN;
    }
    mcText(b, MC_W - 14, WX_HEADER_H / 2 + 1, &fonts::Font0, 0xA534, middle_right);
    lamp(MC_W - 7, WX_HEADER_H / 2, lvl, true);
}

// ------------------------------------------------------------------ screen loop
void mcWeatherScreen() {
    if (!wxBg) {
        wxBg = (uint16_t *)ps_malloc(MC_W * MC_H * 2);
        if (wxBg && !mcLoadMg("/mantiscore/weather.mg", wxBg)) {
            for (int i = 0; i < MC_W * MC_H; i++) wxBg[i] = 0x10A2;
        }
    }
    if (!wxBg) return;
    mcWaitRelease();
    McWeather w;
    mcWeatherSnapshot(w);
    if (!w.valid || time(nullptr) - w.updated > 10 * 60) mcNetRefreshWeather(); // fresh data while you look
    uint32_t pageStart = millis(), lastSnap = 0;
    int page = PG_NOW;
    uint8_t flick = 255;
    for (;;) {
        mcPollTouch();
        if (mcTouch.released && !mcTouch.moved) break;
        uint32_t now = millis();
        if (now - lastSnap > 1000) {
            lastSnap = now;
            mcWeatherSnapshot(w);
        }
        if (now - pageStart > 5000) {
            pageStart = now;
            page = (page + 1) % PG_COUNT;
            if (page == PG_AIR && !w.aqiValid) page = PG_NOW;
        }
        if (random(1000) < 5) flick = 160 + random(60);
        else if (flick < 255) flick = min(255, flick + 30);

        mcBlitRGB(0, 0, MC_W, MC_H, wxBg);
        drawHeader(w);
        bool air = page == PG_AIR && w.aqiValid;
        drawTubes(air ? w.aqi : (int)lroundf(w.temp), w.valid, flick);
        drawPlate(air ? "AIR QUALITY  AQI" : "TEMPERATURE", !air && w.valid, w.metric);
        drawCrt(w, w.valid ? page : 0, now);
        drawCells(w);
        drawWave(w, now);
        mcPush();
        if (mcNtpPending) {
            mcNtpPending = false;
            mcTimeToRtc(time(nullptr));
        }
        delay(25);
    }
    mcWaitRelease();
}
