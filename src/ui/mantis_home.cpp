// MantisCore home screen, nixie clock and charging mode.
#include "../engine/app_registry.h"
#include "../engine/globals.h"
#include "../engine/sd_install.h"
#include "mantis.h"
#include "mantis_assets.h"
#include "mantis_internal.h"
#include <SD.h>
#include <algorithm>
#include <driver/gpio.h>
#include <esp_sleep.h>


struct McApp {
    String file;      // "/Calc.bin" ("" = settings tile)
    String name;      // "Calc" (matches the engine's installed-app name)
    String title;     // label under the icon
    String installed; // partition label when installed
    McIcon icon;
};

static std::vector<McApp> apps;
static uint16_t *wall = nullptr;
static int page = 0;
static const int PER_PAGE = 8;
static int batteryPct = -1;
static bool charging = false;
static uint32_t lastBatteryRead = 0;

// Friendly names for the apps that ship with MantisCore; anything else uses its file name
// (or /icons/<name>.txt if present).
static String friendlyTitle(const String &name) {
    static const char *const map[][2] = {
        {"Calc", "Calculator"}, {"chat", "Chat"},       {"device-radar", "Radar"}, {"gallery", "Gallery"},
        {"llm", "Mantis AI"},   {"mantisCAM", "Camera"}, {"planet", "Planet"},      {"space", "Space"},
        {"studio", "Studio"},   {"synapse", "Synapse"},
    };
    for (auto &m : map)
        if (name.equalsIgnoreCase(m[0])) return m[1];
    File f = SD.open("/icons/" + name + ".txt");
    if (f) {
        String t = f.readStringUntil('\n');
        f.close();
        t.trim();
        if (t.length()) return t;
    }
    return name;
}

static int pageCount() { return max(1, (int)((apps.size() + PER_PAGE - 1) / PER_PAGE)); }

static void freeApps() {
    for (McApp &a : apps) mcFreeIcon(a.icon);
    apps.clear();
}

static void scanApps() {
    freeApps();
    std::vector<LauncherAppMetadata> installed = launcherListInstalledApps();
    File root = SD.open("/");
    if (root) {
        for (File f = root.openNextFile(); f; f = root.openNextFile()) {
            String n = f.name();
            bool isDir = f.isDirectory();
            f.close();
            int slash = n.lastIndexOf('/');
            if (slash >= 0) n = n.substring(slash + 1);
            if (isDir || n.startsWith(".") || (!n.endsWith(".bin") && !n.endsWith(".BIN"))) continue;
            McApp a;
            a.file = "/" + n;
            a.name = launcherAppNameFromFile(n);
            a.title = friendlyTitle(a.name);
            for (const LauncherAppMetadata &m : installed)
                if (m.name == a.name) a.installed = m.label;
            apps.push_back(a);
        }
        root.close();
    }
    std::sort(apps.begin(), apps.end(), [](const McApp &x, const McApp &y) {
        return x.title.compareTo(y.title) < 0 ? true : false;
    });
    for (McApp &a : apps) mcLoadIcon(("/icons/" + a.name + ".mi").c_str(), a.icon);
    McApp s;
    s.title = "Settings";
    apps.push_back(s); // always last
    page = min(page, pageCount() - 1);
}

static void loadWallpaper() {
    if (!wall) wall = (uint16_t *)ps_malloc(MC_W * MC_H * 2);
    if (!wall) return;
    const char *dir = mcCfg.customBg ? "/images/backgrounds" : "/images";
    std::vector<String> files;
    File d = SD.open(dir);
    if (d) {
        for (File f = d.openNextFile(); f; f = d.openNextFile()) {
            String n = f.name();
            bool isDir = f.isDirectory();
            f.close();
            int slash = n.lastIndexOf('/');
            if (slash >= 0) n = n.substring(slash + 1);
            if (!isDir && !n.startsWith(".") && (n.endsWith(".mg") || n.endsWith(".MG"))) files.push_back(n);
        }
        d.close();
    }
    std::sort(files.begin(), files.end());
    if (!files.empty()) {
        String path = String(dir) + "/" + files[mcCfg.bgIndex % files.size()];
        if (mcLoadMg(path.c_str(), wall)) return;
    }
    // fallback: charcoal-to-teal gradient
    for (int y = 0; y < MC_H; y++) {
        uint16_t c = mcBlend(MC_TEAL, MC_CHARCOAL, (uint8_t)(y * 120 / MC_H));
        for (int x = 0; x < MC_W; x++) wall[y * MC_W + x] = c;
    }
}

// ------------------------------------------------------------------ drawing
static void iconXY(int slot, int &x, int &y) {
    x = (slot % 4) * 80 + 8;
    y = MC_HEADER_H + 8 + (slot / 4) * 102;
}

static void drawFallbackIcon(const McApp &a, int x, int y) {
    uint16_t base = (a.name.length() & 1) ? MC_PURPLE : MC_TEAL;
    mcCanvas.fillSmoothRoundRect(x, y, 64, 64, 14, base);
    mcCanvas.fillSmoothRoundRect(x + 3, y + 3, 58, 30, 11, mcBlend(MC_WHITE, base, 28));
    mcCanvas.drawRoundRect(x, y, 64, 64, 14, mcBlend(MC_GREEN, base, 90));
    char ch[2] = {(char)toupper(a.title.length() ? a.title[0] : '?'), 0};
    mcText(ch, x + 32, y + 33, &fonts::FreeSansBold12pt7b, MC_GREEN, middle_center, true);
}

static void drawPage(int p, int dx) {
    for (int i = 0; i < PER_PAGE; i++) {
        int idx = p * PER_PAGE + i;
        if (idx >= (int)apps.size()) break;
        int x, y;
        iconXY(i, x, y);
        x += dx;
        if (x < -80 || x > MC_W) continue;
        McApp &a = apps[idx];
        if (a.file.isEmpty()) mcBlitAlpha(x, y, 64, 64, SETTINGS_ICON_RGB, SETTINGS_ICON_A);
        else if (a.icon.rgb) mcBlitAlpha(x, y, 64, 64, a.icon.rgb, a.icon.a);
        else drawFallbackIcon(a, x, y);
        mcText(a.title.c_str(), x + 32, y + 78, &fonts::Font2, MC_WHITE, middle_center, true);
    }
}

static void drawWeatherIcon(int x, int y, int code) {
    bool sun = code == 0 || code == 1 || code == 2;
    bool cloud = code >= 2;
    if (sun) {
        mcCanvas.fillCircle(x + (cloud ? 4 : 7), y + (cloud ? 4 : 7), 4, 0xFE60);
    }
    if (cloud) {
        mcCanvas.fillCircle(x + 6, y + 9, 4, MC_WHITE);
        mcCanvas.fillCircle(x + 11, y + 8, 5, MC_WHITE);
        mcCanvas.fillRect(x + 4, y + 9, 10, 4, MC_WHITE);
    }
    if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82) || code >= 95) {
        for (int i = 0; i < 3; i++) mcCanvas.drawLine(x + 5 + i * 3, y + 14, x + 4 + i * 3, y + 16, 0x5D9F);
    } else if ((code >= 71 && code <= 77) || code == 85 || code == 86) {
        for (int i = 0; i < 3; i++) mcCanvas.drawPixel(x + 5 + i * 3, y + 15, MC_WHITE);
    }
}

static void drawHeader() {
    mcDarkenRect(0, 0, MC_W, MC_HEADER_H, 80);
    mcCanvas.drawFastHLine(0, MC_HEADER_H, MC_W, mcBlend(MC_TEAL, 0, 160));
    mcText(mcClockString().c_str(), 8, MC_HEADER_H / 2 + 1, &fonts::FreeSansBold9pt7b, MC_WHITE, middle_left);
    int x = MC_W - 6;
    if (batteryPct >= 0) { // battery glyph
        x -= 22;
        mcCanvas.drawRoundRect(x, 6, 19, 10, 2, MC_WHITE);
        mcCanvas.fillRect(x + 19, 9, 2, 4, MC_WHITE);
        uint16_t c = batteryPct <= 15 && !charging ? 0xF800 : MC_GREEN;
        mcCanvas.fillRect(x + 2, 8, max(1, 15 * batteryPct / 100), 6, c);
        if (charging) mcText("+", x - 3, 11, &fonts::Font0, MC_GREEN, middle_right);
        x -= 12;
    }
    McWeather wx;
    mcWeatherSnapshot(wx);
    if (wx.valid) {
        char t[8];
        snprintf(t, sizeof(t), "%d", (int)lroundf(wx.temp));
        mcCanvas.drawCircle(x - 2, 7, 2, MC_WHITE); // degree mark (Font2 has no degree glyph)
        x -= 6;
        mcText(t, x, MC_HEADER_H / 2 + 1, &fonts::Font2, MC_WHITE, middle_right);
        mcCanvas.setFont(&fonts::Font2);
        x -= mcCanvas.textWidth(t) + 20;
        drawWeatherIcon(x, 2, wx.code);
    } else if (mcCfg.ssid.length() && mcNetBusy) {
        mcText("...", x, MC_HEADER_H / 2 + 1, &fonts::Font2, MC_GREY, middle_right);
    }
}

static void render(int dx) {
    mcBlitRGB(0, 0, MC_W, MC_H, wall);
    drawPage(page, dx);
    if (dx > 0 && page > 0) drawPage(page - 1, dx - MC_W);
    if (dx < 0 && page < pageCount() - 1) drawPage(page + 1, dx + MC_W);
    drawHeader();
    int pc = pageCount();
    if (pc > 1)
        for (int i = 0; i < pc; i++) {
            int cx = 160 - (pc - 1) * 7 + i * 14;
            if (i == page) mcCanvas.fillCircle(cx, 232, 4, MC_GREEN);
            else mcCanvas.fillCircle(cx, 232, 3, mcBlend(MC_WHITE, 0, 150));
        }
    if (mcBackupBusy()) mcCanvas.fillCircle(MC_W - 8, 232, 3, MC_TEAL); // quiet "saving" dot
    mcPush();
}

static int hitIcon(int x, int y) {
    for (int i = 0; i < PER_PAGE; i++) {
        int ix, iy;
        iconXY(i, ix, iy);
        if (x >= ix - 6 && x < ix + 70 && y >= iy - 4 && y < iy + 90) {
            int idx = page * PER_PAGE + i;
            return idx < (int)apps.size() ? idx : -1;
        }
    }
    return -1;
}

// ------------------------------------------------------------------ actions
static void reclaimDisplay() { mcApplyBrightness(); }

static void launchApp(McApp &a) {
    mcBackupFinishBlocking(a.title); // never leave a half-written backup behind
    mcLoadingBegin(a.title);
    if (a.installed.length()) {
        uint32_t t0 = millis();
        while (millis() - t0 < 900) mcLoadingFrame((millis() - t0) / 900.0f, "Starting...");
        mcLoadingFrame(1.0f, "Starting...");
        delay(250);
        mcNetStop();
        launcherBootAppByLabel(a.installed.c_str()); // reboots into the app on success
    } else {
        mcLoadingFrame(0, "Installing for the first time...");
        mcNetStop();
        updateFromSD(a.file); // installs, restores any saved progress, then reboots into the app
    }
    // only reached on failure
    reclaimDisplay();
    mcNetStart();
}

static void removeApp(McApp &a) {
    if (!mcConfirm("Remove " + a.title + "?\nYour progress stays saved on the SD card.", "Remove", "Keep")) return;
    mcBackupFinishBlocking(a.title);
    mcBackupAppBlocking(a.installed, a.name);
    mcLoadingBegin(a.title);
    mcLoadingFrame(-1, "Removing app...");
    mcNetStop();
    launcherDeleteAppByLabel(a.installed.c_str()); // frees + optimizes flash, then reboots
    reclaimDisplay();
    mcNetStart();
}

static void appMenu(McApp &a) {
    if (a.file.isEmpty()) return;
    if (a.installed.isEmpty()) {
        mcToast(a.title + " installs on first launch");
        return;
    }
    if (mcActionSheet(a.title, {"Remove from device", "Cancel"}) == 0) removeApp(a);
}

// ------------------------------------------------------------------ home loop
void mantisRun() {
    mcConfigLoad();
    mcGfxInit();
    reclaimDisplay();
    mcApplyTimeZone();
    mcTimeFromRtc();
    if (!sdcardMounted) setupSdCard();
    scanApps();
    loadWallpaper();
    mcBackupQueueAfterApp();
    mcNetStart();

    uint32_t lastRender = 0, lastInput = millis();
    int dimLevel = 0; // 0 awake, 1 dim, 2 off
    bool dragging = false;
    for (;;) {
        mcPollTouch();
        uint32_t now = millis();
        if (mcTouch.pressed) {
            lastInput = now;
            if (dimLevel) { // first touch only wakes the screen
                dimLevel = 0;
                mcApplyBrightness();
                mcWaitRelease();
                continue;
            }
        }
        if (!dimLevel && now - lastInput > 45000) {
            dimLevel = 1;
            M5.Display.setBrightness(max(8, mcCfg.bright * 255 / 100 / 5));
        } else if (dimLevel == 1 && now - lastInput > 120000) {
            dimLevel = 2;
            M5.Display.setBrightness(0);
        }
        if (mcNtpPending) {
            mcNtpPending = false;
            mcTimeToRtc(time(nullptr));
        }
        if (now - lastBatteryRead > 15000 || lastBatteryRead == 0) {
            lastBatteryRead = now;
            batteryPct = M5.Power.getBatteryLevel();
            charging = mcIsCharging();
        }

        bool inHeader = mcTouch.startY < MC_HEADER_H + 4;
        int dx = mcTouch.x - mcTouch.startX;
        if (mcTouch.down) {
            if (!inHeader && mcTouch.moved && abs(dx) > abs(mcTouch.y - mcTouch.startY)) dragging = true;
            if (dragging) {
                if ((page == 0 && dx > 0) || (page == pageCount() - 1 && dx < 0)) dx /= 3; // rubber band
                render(dx);
                lastRender = now;
                continue;
            }
            if (!mcTouch.moved && !mcTouch.longFired && now - mcTouch.downAt > 600) {
                mcTouch.longFired = true;
                if (inHeader && mcTouch.startX < 120) {
                    mcWaitRelease();
                    mcTimeSettings();
                    render(0);
                } else if (!inHeader) {
                    int idx = hitIcon(mcTouch.startX, mcTouch.startY);
                    if (idx >= 0) {
                        mcWaitRelease();
                        appMenu(apps[idx]);
                        render(0);
                    }
                }
                continue;
            }
        }
        if (mcTouch.released) {
            if (dragging) {
                dragging = false;
                int target = page;
                if (dx < -60 && page < pageCount() - 1) target = page + 1;
                if (dx > 60 && page > 0) target = page - 1;
                int end = (target - page) * -MC_W;
                for (int s = 1; s <= 6; s++) render(dx + (end - dx) * s / 6);
                page = target;
                render(0);
            } else if (!mcTouch.moved && !mcTouch.longFired) {
                if (inHeader && mcTouch.startX < 120) {
                    mcNixieClock();
                    render(0);
                } else if (inHeader && mcTouch.startX >= 160) { // weather / battery side of the header
                    mcWeatherScreen();
                    render(0);
                } else if (!inHeader) {
                    int idx = hitIcon(mcTouch.startX, mcTouch.startY);
                    if (idx >= 0) {
                        McApp &a = apps[idx];
                        if (a.file.isEmpty()) {
                            mcSettings();
                            loadWallpaper();
                            mcApplyTimeZone();
                        } else {
                            launchApp(a);
                        }
                        render(0);
                    }
                }
            }
            lastInput = millis();
        }
        if (!mcTouch.down && mcBackupBusy()) mcBackupStep(20);
        if (dimLevel < 2 && millis() - lastRender > 1000) {
            render(0);
            lastRender = millis();
        }
        delay(mcBackupBusy() ? 1 : 15);
    }
}

// ------------------------------------------------------------------ nixie clock
static uint16_t *nixieBg = nullptr;

void mcNixieClock() {
    if (!nixieBg) {
        nixieBg = (uint16_t *)ps_malloc(MC_W * MC_H * 2);
        if (nixieBg && !mcLoadMg("/mantiscore/nixie.mg", nixieBg)) {
            for (int i = 0; i < MC_W * MC_H; i++) nixieBg[i] = 0x0841;
        }
    }
    if (!nixieBg) return;
    mcWaitRelease();
    const int GSZ = NIXIE_GW * NIXIE_GH;
    int shown[5] = {-1, -1, -1, -1, -1}, prev[5] = {-1, -1, -1, -1, -1};
    uint32_t changedAt[5] = {0, 0, 0, 0, 0};
    uint8_t flicker[5] = {255, 255, 255, 255, 255};
    for (;;) {
        mcPollTouch();
        if (mcTouch.released && !mcTouch.moved) break;
        uint32_t now = millis();
        time_t t = time(nullptr);
        struct tm lt;
        localtime_r(&t, &lt);
        int h = lt.tm_hour;
        if (!mcCfg.h24) h = h % 12 == 0 ? 12 : h % 12;
        int want[5] = {h / 10, h % 10, 10, lt.tm_min / 10, lt.tm_min % 10};
        if (!mcCfg.h24 && h < 10) want[0] = -1; // leading tube stays cold
        for (int i = 0; i < 5; i++) {
            if (want[i] != shown[i]) {
                prev[i] = shown[i];
                shown[i] = want[i];
                changedAt[i] = now;
            }
            if (random(1000) < 4) flicker[i] = 170 + random(60); // rare flicker
            else if (flicker[i] < 255) flicker[i] = min(255, flicker[i] + 25);
        }
        mcBlitRGB(0, 0, MC_W, MC_H, nixieBg);
        struct timeval tv;
        gettimeofday(&tv, nullptr);
        for (int i = 0; i < 5; i++) {
            int x = NIXIE_TUBE_XY[i][0], y = NIXIE_TUBE_XY[i][1];
            if (i == 2) { // colon breathes with the seconds
                float ph = (tv.tv_usec / 1000000.0f);
                uint8_t lvl = (uint8_t)(70 + 185 * (ph < 0.5f ? 1.0f : (1.0f - ph) * 2));
                mcBlitAdd(x, y, NIXIE_GW, NIXIE_GH, NIXIE_GLYPHS + 10 * GSZ, lvl);
                continue;
            }
            float f = min(1.0f, (now - changedAt[i]) / 300.0f); // cross-fade between digits
            if (prev[i] >= 0 && f < 1.0f)
                mcBlitAdd(x, y, NIXIE_GW, NIXIE_GH, NIXIE_GLYPHS + prev[i] * GSZ, (uint8_t)(255 * (1 - f)));
            if (shown[i] >= 0)
                mcBlitAdd(x, y, NIXIE_GW, NIXIE_GH, NIXIE_GLYPHS + shown[i] * GSZ, (uint8_t)(f * flicker[i]));
        }
        mcPush();
        if (mcNtpPending) {
            mcNtpPending = false;
            mcTimeToRtc(time(nullptr));
        }
        delay(20);
    }
}

// ------------------------------------------------------------------ charging mode
static void drawCharge() {
    int pct = max(0, (int)M5.Power.getBatteryLevel());
    bool chg = mcIsCharging();
    mcCanvas.fillScreen(0x0000);
    const int bx = 90, by = 70, bw = 130, bh = 64;
    mcCanvas.drawRoundRect(bx, by, bw, bh, 10, MC_WHITE);
    mcCanvas.drawRoundRect(bx + 1, by + 1, bw - 2, bh - 2, 9, MC_WHITE);
    mcCanvas.fillRoundRect(bx + bw, by + 20, 8, 24, 3, MC_WHITE);
    int fw = (bw - 12) * pct / 100;
    if (fw > 4) mcCanvas.fillSmoothRoundRect(bx + 6, by + 6, fw, bh - 12, 6, pct <= 15 ? 0xF800 : (chg ? MC_GREEN : MC_TEAL));
    char t[8];
    snprintf(t, sizeof(t), "%d%%", pct);
    mcText(t, 160, 160, &fonts::FreeSansBold12pt7b, MC_WHITE, middle_center);
    mcText(chg ? "Charging" : "On battery", 160, 190, &fonts::Font2, chg ? MC_GREEN : MC_GREY, middle_center);
    mcText(mcClockString().c_str(), 160, 36, &fonts::FreeSansBold12pt7b, MC_GREY, middle_center);
    mcText("Tap to wake  -  hold to exit", 160, 224, &fonts::Font2, 0x632C, middle_center);
    mcPush();
}

void mcChargeMode() {
    mcWaitRelease();
    mcNetStop();
    M5.Display.setBrightness(40);
    drawCharge();
    uint32_t shownAt = millis(), lastDraw = millis();
    for (;;) {
        mcPollTouch();
        if (mcTouch.down && !mcTouch.moved && millis() - mcTouch.downAt > 1200) break; // hold to exit
        if (mcTouch.pressed) {
            shownAt = millis();
            M5.Display.setBrightness(40);
            drawCharge();
        }
        if (millis() - lastDraw > 5000) {
            lastDraw = millis();
            drawCharge();
        }
        if (!mcTouch.down && millis() - shownAt > 8000) {
            M5.Display.setBrightness(0);
            // light sleep until the touch panel (INT on GPIO39) or a 30 s timer wakes us
            gpio_wakeup_enable(GPIO_NUM_39, GPIO_INTR_LOW_LEVEL);
            esp_sleep_enable_gpio_wakeup();
            esp_sleep_enable_timer_wakeup(30ULL * 1000000ULL);
            esp_light_sleep_start();
            if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO) {
                shownAt = millis();
                M5.Display.setBrightness(40);
                drawCharge();
                mcWaitRelease(); // the waking tap is not a command
            }
        }
        delay(20);
    }
    mcWaitRelease();
    mcApplyBrightness();
    mcNetStart();
}
