// MantisCore settings, time settings, on-screen keyboard and dialogs.
#include "../engine/globals.h"
#include "mantis_internal.h"
#include <WiFi.h>
#include <SD.h>
#include <algorithm>

// ------------------------------------------------------------------ shared widgets
static void titleBar(const char *title, bool back) {
    mcCanvas.fillRect(0, 0, MC_W, 30, MC_TEAL);
    mcCanvas.drawFastHLine(0, 30, MC_W, MC_GREEN);
    if (back) mcText("< Back", 8, 15, &fonts::FreeSansBold9pt7b, MC_WHITE, middle_left);
    mcText(title, 160, 15, &fonts::FreeSansBold9pt7b, MC_WHITE, middle_center);
}

static bool button(int x, int y, int w, int h, const char *label, bool primary, bool pressed = false) {
    uint16_t fill = primary ? MC_TEAL : MC_CHARCOAL2;
    if (pressed) fill = mcBlend(MC_GREEN, fill, 90);
    mcCanvas.fillSmoothRoundRect(x, y, w, h, 10, fill);
    mcCanvas.drawRoundRect(x, y, w, h, 10, primary ? MC_GREEN : MC_TEAL);
    mcText(label, x + w / 2, y + h / 2, &fonts::FreeSansBold9pt7b, MC_WHITE, middle_center);
    return true;
}

static bool inRect(int px, int py, int x, int y, int w, int h) { return px >= x && px < x + w && py >= y && py < y + h; }

static void wrapText(const String &text, int cx, int y, int maxW, const lgfx::IFont *font, uint16_t color) {
    mcCanvas.setFont(font);
    String line, word;
    const int lh = 20; // FreeSansBold9pt7b line height
    auto flush = [&]() {
        if (line.length()) mcText(line.c_str(), cx, y, font, color, top_center);
        y += lh;
        line = "";
    };
    for (size_t i = 0; i <= text.length(); i++) {
        char c = i < text.length() ? text[i] : ' ';
        if (c == ' ' || c == '\n') {
            String trial = line.length() ? line + " " + word : word;
            if (mcCanvas.textWidth(trial.c_str()) > maxW && line.length()) {
                flush();
                line = word;
            } else line = trial;
            word = "";
            if (c == '\n') flush();
        } else word += c;
    }
    flush();
}

int mcActionSheet(const String &title, const std::vector<String> &options) {
    mcDarkenRect(0, 0, MC_W, MC_H, 110);
    int n = options.size(), h = 40 + n * 44;
    int y0 = MC_H - h;
    mcCanvas.fillSmoothRoundRect(8, y0, MC_W - 16, h + 20, 16, MC_CHARCOAL);
    mcCanvas.drawRoundRect(8, y0, MC_W - 16, h + 20, 16, MC_TEAL);
    mcText(title.c_str(), 160, y0 + 20, &fonts::FreeSansBold9pt7b, MC_GREEN, middle_center);
    for (int i = 0; i < n; i++) button(20, y0 + 38 + i * 44, MC_W - 40, 38, options[i].c_str(), i == 0);
    mcPush();
    for (;;) {
        mcPollTouch();
        if (mcTouch.released && !mcTouch.moved) {
            if (mcTouch.y < y0) return -1;
            for (int i = 0; i < n; i++)
                if (inRect(mcTouch.x, mcTouch.y, 20, y0 + 38 + i * 44, MC_W - 40, 38)) return i == n - 1 ? -1 : i;
        }
        delay(15);
    }
}

bool mcConfirm(const String &title, const String &yes, const String &no) {
    mcDarkenRect(0, 0, MC_W, MC_H, 110);
    mcCanvas.fillSmoothRoundRect(20, 40, 280, 160, 16, MC_CHARCOAL);
    mcCanvas.drawRoundRect(20, 40, 280, 160, 16, MC_TEAL);
    wrapText(title, 160, 56, 250, &fonts::FreeSansBold9pt7b, MC_WHITE);
    button(34, 148, 118, 38, no.c_str(), false);
    button(168, 148, 118, 38, yes.c_str(), true);
    mcPush();
    for (;;) {
        mcPollTouch();
        if (mcTouch.released && !mcTouch.moved) {
            if (inRect(mcTouch.x, mcTouch.y, 168, 148, 118, 38)) return true;
            if (inRect(mcTouch.x, mcTouch.y, 34, 148, 118, 38)) return false;
        }
        delay(15);
    }
}

// ------------------------------------------------------------------ keyboard
String mcKeyboard(const String &title, const String &initial, bool numeric, bool password) {
    static const char *const alpha[3] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};
    static const char *const sym[3] = {"1234567890", "-/:;()$&@\"", ".,?!'#%*+="};
    String text = initial;
    int mode = 0; // 0 lower, 1 upper, 2 symbols
    bool reveal = !password;
    struct Key { int x, y, w, h; String cap; int code; }; // code: 0 char, 1 del, 2 shift, 3 mode, 4 ok, 5 space
    std::vector<Key> keys;
    auto layout = [&]() {
        keys.clear();
        const int top = 62, kh = 40, gap = 4;
        if (numeric) {
            const char *pad = "123456789";
            for (int i = 0; i < 9; i++)
                keys.push_back({40 + (i % 3) * 82, top + (i / 3) * (kh + gap), 78, kh, String(pad[i]), 0});
            keys.push_back({40, top + 3 * (kh + gap), 78, kh, "Del", 1});
            keys.push_back({122, top + 3 * (kh + gap), 78, kh, "0", 0});
            keys.push_back({204, top + 3 * (kh + gap), 78, kh, "OK", 4});
            return;
        }
        const char *const *rows = mode == 2 ? sym : alpha;
        for (int r = 0; r < 3; r++) {
            int n = strlen(rows[r]);
            int kw = 29, rowW = n * (kw + 3);
            int x0 = r == 2 ? 46 : (MC_W - rowW) / 2;
            for (int i = 0; i < n; i++) {
                char c = rows[r][i];
                if (mode == 1) c = toupper(c);
                keys.push_back({x0 + i * (kw + 3), top + r * (kh + gap), kw, kh, String(c), 0});
            }
        }
        int y3 = top + 2 * (kh + gap);
        if (mode != 2) keys.push_back({4, y3, 38, kh, mode == 1 ? "ABC" : "abc", 2});
        keys.push_back({MC_W - 42, y3, 38, kh, "Del", 1});
        int y4 = top + 3 * (kh + gap);
        keys.push_back({4, y4, 60, kh, mode == 2 ? "abc" : "123", 3});
        keys.push_back({68, y4, 176, kh, "space", 5});
        keys.push_back({248, y4, 68, kh, "OK", 4});
    };
    layout();
    int pressedKey = -1;
    for (;;) {
        mcCanvas.fillScreen(MC_CHARCOAL);
        titleBar(title.c_str(), true);
        mcCanvas.fillSmoothRoundRect(6, 34, MC_W - 12, 24, 6, 0x0000);
        mcCanvas.drawRoundRect(6, 34, MC_W - 12, 24, 6, MC_TEAL);
        String shown = reveal ? text : String();
        if (!reveal)
            for (size_t i = 0; i < text.length(); i++) shown += '*';
        mcCanvas.setFont(&fonts::FreeSansBold9pt7b);
        while (shown.length() && mcCanvas.textWidth(shown.c_str()) > MC_W - 60) shown.remove(0, 1);
        if ((millis() / 500) & 1) shown += "_";
        mcText(shown.c_str(), 12, 46, &fonts::FreeSansBold9pt7b, MC_WHITE, middle_left);
        if (password) mcText(reveal ? "hide" : "show", MC_W - 12, 46, &fonts::Font2, MC_GREEN, middle_right);
        for (size_t i = 0; i < keys.size(); i++) {
            Key &k = keys[i];
            uint16_t fill = (int)i == pressedKey ? MC_TEAL : (k.code == 4 ? MC_PURPLE : MC_CHARCOAL2);
            mcCanvas.fillSmoothRoundRect(k.x, k.y, k.w, k.h, 6, fill);
            
            // Cast to the ultimate abstract font interface (IFont)
            const lgfx::v1::IFont* chosenFont = k.cap.length() > 1 
                ? static_cast<const lgfx::v1::IFont*>(&fonts::Font2) 
                : static_cast<const lgfx::v1::IFont*>(&fonts::FreeSansBold9pt7b);
                
            mcText(k.cap.c_str(), k.x + k.w / 2, k.y + k.h / 2, chosenFont,
                   k.code == 4 ? MC_GREEN : MC_WHITE, middle_center);
        }
        mcPush();
        mcPollTouch();
        if (mcTouch.pressed) {
            pressedKey = -1;
            for (size_t i = 0; i < keys.size(); i++)
                if (inRect(mcTouch.x, mcTouch.y, keys[i].x, keys[i].y, keys[i].w, keys[i].h)) pressedKey = i;
        }
        if (mcTouch.released) {
            int ki = pressedKey;
            pressedKey = -1;
            if (mcTouch.startY < 31 && mcTouch.startX < 90) return initial; // back = cancel
            if (password && inRect(mcTouch.startX, mcTouch.startY, MC_W - 60, 34, 54, 24)) reveal = !reveal;
            if (ki >= 0) {
                Key k = keys[ki];
                if (k.code == 0 && text.length() < 63) {
                    text += k.cap;
                    if (mode == 1) { mode = 0; layout(); }
                } else if (k.code == 1 && text.length()) text.remove(text.length() - 1);
                else if (k.code == 2) { mode = mode == 1 ? 0 : 1; layout(); }
                else if (k.code == 3) { mode = mode == 2 ? 0 : 2; layout(); }
                else if (k.code == 5) text += ' ';
                else if (k.code == 4) return text;
            }
        }
        delay(10);
    }
}


// ------------------------------------------------------------------ list screens
struct McRow {
    String label, value;
    int kind = 0; // 0 normal, 1 slider (value text ignored), 2 info
    int slider = 0;
};

static void drawList(const char *title, const std::vector<McRow> &rows, int scroll) {
    mcCanvas.fillScreen(MC_CHARCOAL);
    const int rh = 38;
    for (size_t i = 0; i < rows.size(); i++) {
        int y = 34 + (int)i * rh - scroll;
        if (y < -rh || y > MC_H) continue;
        const McRow &r = rows[i];
        mcCanvas.fillSmoothRoundRect(6, y, MC_W - 12, rh - 4, 8, MC_CHARCOAL2);
        mcText(r.label.c_str(), 16, y + (rh - 4) / 2, &fonts::FreeSansBold9pt7b, MC_WHITE, middle_left);
        if (r.kind == 1) {
            int sx = 150, sw = 150, sy = y + (rh - 4) / 2;
            mcCanvas.fillSmoothRoundRect(sx, sy - 3, sw, 6, 3, 0x0841);
            mcCanvas.fillSmoothRoundRect(sx, sy - 3, sw * r.slider / 100, 6, 3, MC_GREEN);
            mcCanvas.fillCircle(sx + sw * r.slider / 100, sy, 8, MC_WHITE);
        } else {
            mcText((r.value + (r.kind == 0 ? "  >" : "")).c_str(), MC_W - 16, y + (rh - 4) / 2, &fonts::Font2,
                   r.kind == 0 ? MC_GREEN : MC_GREY, middle_right);
        }
    }
    titleBar(title, true);
    mcPush();
}

// Generic list loop. onTap returns false to leave the screen. onSlide receives 0..100.
template <typename Build, typename Tap, typename Slide>
static void listScreen(const char *title, Build build, Tap onTap, Slide onSlide) {
    int scroll = 0, scrollAtPress = 0;
    bool sliding = false;
    mcWaitRelease();
    for (;;) {
        std::vector<McRow> rows = build();
        int maxScroll = max(0, 34 + (int)rows.size() * 38 - MC_H);
        mcPollTouch();
        if (mcTouch.pressed) scrollAtPress = scroll;
        if (mcTouch.down && mcTouch.startY > 31) {
            int idx = (mcTouch.startY - 34 + scroll) / 38;
            if (idx >= 0 && idx < (int)rows.size() && rows[idx].kind == 1 && mcTouch.startX > 130) sliding = true;
            if (sliding) onSlide(idx, constrain((mcTouch.x - 150) * 100 / 150, 0, 100));
            else if (mcTouch.moved) scroll = constrain(scrollAtPress - (mcTouch.y - mcTouch.startY), 0, maxScroll);
        }
        if (mcTouch.released) {
            bool wasSliding = sliding;
            sliding = false;
            if (!mcTouch.moved && mcTouch.startY < 31 && mcTouch.startX < 90) return;
            if (!mcTouch.moved && !wasSliding && mcTouch.startY > 31) {
                int idx = (mcTouch.startY - 34 + scroll) / 38;
                if (idx >= 0 && idx < (int)rows.size() && !onTap(idx)) return;
                mcWaitRelease();
            }
        }
        drawList(title, build(), scroll);
        delay(15);
    }
}

// ------------------------------------------------------------------ wifi
static void wifiScreen() {
    mcNetStop();
    mcLoadingBegin("Wi-Fi");
    mcLoadingFrame(-1, "Looking for networks...");
    struct Net { String ssid; int32_t rssi; bool open; };
    std::vector<Net> nets;
    WiFi.mode(WIFI_STA);
    int n = WiFi.scanNetworks();
    for (int i = 0; i < n; i++) { // results are strongest first
        String ssid = WiFi.SSID(i);
        if (ssid.isEmpty()) continue;
        bool dup = false;
        for (auto &x : nets) dup |= x.ssid == ssid;
        if (!dup && nets.size() < 12) nets.push_back({ssid, WiFi.RSSI(i), WiFi.encryptionType(i) == WIFI_AUTH_OPEN});
    }
    WiFi.scanDelete();
    listScreen(
        "Choose Wi-Fi",
        [&]() {
            std::vector<McRow> rows;
            if (mcCfg.ssid.length()) rows.push_back({String("Forget ") + mcCfg.ssid, String(""), 0, 0});
            for (auto &n : nets) {
                McRow r;
                r.label = n.ssid;
                r.value = n.rssi > -60 ? "strong" : n.rssi > -75 ? "ok" : "weak";
                if (!n.open) r.value += " *";
                rows.push_back(r);
            }
            if (nets.empty()) rows.push_back({String("No networks found"), String(""), 2, 0});
            return rows;
        },
        [&](int idx) {
            if (mcCfg.ssid.length()) {
                if (idx == 0) {
                    mcNetStop();
                    mcCfg.ssid = mcCfg.pass = "";
                    mcConfigSave();
                    return false;
                }
                idx--;
            }
            if (idx >= (int)nets.size()) return true;
            String ssid = nets[idx].ssid, pass;
            if (!nets[idx].open) {
                pass = mcKeyboard("Password for " + ssid, "", false, true);
                if (pass.isEmpty()) return true;
            }
            mcCfg.ssid = ssid;
            mcCfg.pass = pass;
            mcConfigSave();
            return false;
        },
        [](int, int) {});
    mcNetStart();
}

// ------------------------------------------------------------------ wallpaper helpers
static int countWallpapers() {
    File d = SD.open(mcCfg.customBg ? "/images/backgrounds" : "/images");
    int n = 0;
    if (d) {
        for (File f = d.openNextFile(); f; f = d.openNextFile()) {
            String nm = f.name();
            if (!f.isDirectory() && (nm.endsWith(".mg") || nm.endsWith(".MG")) && nm.indexOf("/.") < 0 && !nm.startsWith("."))
                n++;
            f.close();
        }
        d.close();
    }
    return n;
}

// ------------------------------------------------------------------ settings
void mcSettings() {
    int wallCount = countWallpapers();
    listScreen(
        "Settings",
        [&]() {
            std::vector<McRow> rows;
            String wifiVal = "Not set";
            if (mcCfg.ssid.length()) wifiVal = mcWifiUp ? mcCfg.ssid : mcCfg.ssid + String(" (offline)");
            rows.push_back({"Wi-Fi", wifiVal, 0, 0});
            rows.push_back({"Weather ZIP", mcCfg.zip.length() ? mcCfg.zip : String("Not set"), 0, 0});
            rows.push_back({"Brightness", "", 1, mcCfg.bright});
            rows.push_back({"Time & date", mcClockString(), 0, 0});
            rows.push_back({"Wallpapers", String(mcCfg.customBg ? "Custom" : "Mantis"), 0, 0});
            String wallVal = "none found";
            if (wallCount) wallVal = String(mcCfg.bgIndex % wallCount + 1) + " of " + String(wallCount);
            rows.push_back({"Next wallpaper", wallVal, 0, 0});
            rows.push_back({"Weather units", String(mcCfg.metric ? "C, km/h, mm" : "F, mph, in"), 0, 0});
            rows.push_back({"Charging mode", "", 0, 0});
            rows.push_back({"About", String("v" MANTISCORE_VERSION), 2, 0});
            return rows;
        },
        [&](int idx) {
            switch (idx) {
                case 0: wifiScreen(); break;
                case 1: {
                    String z = mcKeyboard("Weather ZIP code", mcCfg.zip, true, false);
                    z.trim();
                    if (z != mcCfg.zip && (z.length() == 5 || z.isEmpty())) {
                        mcNetStop();
                        mcCfg.zip = z;
                        mcConfigSave();
                        mcNetStart();
                    }
                    break;
                }
                case 3: mcTimeSettings(); break;
                case 4:
                    mcCfg.customBg = !mcCfg.customBg;
                    mcCfg.bgIndex = 0;
                    mcConfigSave();
                    wallCount = countWallpapers();
                    break;
                case 5:
                    if (wallCount) {
                        mcCfg.bgIndex = (mcCfg.bgIndex + 1) % wallCount;
                        mcConfigSave();
                    }
                    break;
                case 6:
                    mcNetStop();
                    mcCfg.metric = !mcCfg.metric;
                    mcConfigSave();
                    mcNetStart(); // refetch in the new units
                    break;
                case 7: mcChargeMode(); break;
                case 8: mcToast("MantisCore v" MANTISCORE_VERSION " by BasaltSoftworks", MC_GREEN, 1800); break;
            }
            return true;
        },
        [](int idx, int v) {
            if (idx != 2) return;
            mcCfg.bright = max(5, v);
            mcApplyBrightness();
            static uint32_t lastSave = 0;
            if (millis() - lastSave > 800) {
                lastSave = millis();
                mcConfigSave();
            }
        });
    mcConfigSave();
}

// ------------------------------------------------------------------ time settings
static void setTimeScreen() {
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    if (t.tm_year + 1900 < 2024) { t.tm_year = 2026 - 1900; t.tm_mon = 0; t.tm_mday = 1; }
    bool h24 = mcCfg.h24;
    int hour12 = t.tm_hour % 12 == 0 ? 12 : t.tm_hour % 12;
    bool pm = t.tm_hour >= 12;
    struct Field { const char *label; int *v; int lo, hi; };
    int year = t.tm_year + 1900, mon = t.tm_mon + 1, day = t.tm_mday, hour = h24 ? t.tm_hour : hour12, min = t.tm_min,
        ampm = pm;
    std::vector<Field> f = {{"Month", &mon, 1, 12}, {"Day", &day, 1, 31}, {"Year", &year, 2024, 2099},
                            {"Hour", &hour, h24 ? 0 : 1, h24 ? 23 : 12}, {"Min", &min, 0, 59}};
    if (!h24) f.push_back({"", &ampm, 0, 1});
    int n = f.size(), cw = (MC_W - 8) / n;
    mcWaitRelease();
    uint32_t repeatAt = 0;
    for (;;) {
        mcCanvas.fillScreen(MC_CHARCOAL);
        titleBar("Set date & time", true);
        for (int i = 0; i < n; i++) {
            int x = 4 + i * cw;
            mcText(f[i].label, x + cw / 2, 46, &fonts::Font2, MC_GREY, middle_center);
            mcCanvas.fillSmoothRoundRect(x + 4, 58, cw - 8, 34, 8, MC_CHARCOAL2);
            mcText("+", x + cw / 2, 75, &fonts::FreeSansBold12pt7b, MC_GREEN, middle_center);
            char b[8];
            if (f[i].v == &ampm) snprintf(b, sizeof(b), "%s", ampm ? "PM" : "AM");
            else if (f[i].v == &min) snprintf(b, sizeof(b), "%02d", *f[i].v);
            else snprintf(b, sizeof(b), "%d", *f[i].v);
            mcText(b, x + cw / 2, 116, &fonts::FreeSansBold12pt7b, MC_WHITE, middle_center);
            mcCanvas.fillSmoothRoundRect(x + 4, 140, cw - 8, 34, 8, MC_CHARCOAL2);
            mcText("-", x + cw / 2, 157, &fonts::FreeSansBold12pt7b, MC_GREEN, middle_center);
        }
        button(20, 190, 130, 40, "Cancel", false);
        button(170, 190, 130, 40, "Save", true);
        mcPush();
        mcPollTouch();
        bool step = mcTouch.pressed || (mcTouch.down && millis() - mcTouch.downAt > 500 && millis() > repeatAt);
        if (step && mcTouch.down) {
            repeatAt = millis() + 120;
            int i = (mcTouch.x - 4) / cw;
            if (i >= 0 && i < n) {
                int d = inRect(mcTouch.x, mcTouch.y, 0, 58, MC_W, 34) ? 1 : inRect(mcTouch.x, mcTouch.y, 0, 140, MC_W, 34) ? -1 : 0;
                if (d) {
                    int &v = *f[i].v;
                    v += d;
                    if (v > f[i].hi) v = f[i].lo;
                    if (v < f[i].lo) v = f[i].hi;
                }
            }
        }
        if (mcTouch.released && !mcTouch.moved) {
            if ((mcTouch.startY < 31 && mcTouch.startX < 90) || inRect(mcTouch.startX, mcTouch.startY, 20, 190, 130, 40)) return;
            if (inRect(mcTouch.startX, mcTouch.startY, 170, 190, 130, 40)) {
                struct tm nt = {};
                nt.tm_year = year - 1900;
                nt.tm_mon = mon - 1;
                nt.tm_mday = day;
                nt.tm_hour = h24 ? hour : (hour % 12) + (ampm ? 12 : 0);
                nt.tm_min = min;
                nt.tm_isdst = -1;
                mcApplyTimeZone();
                mcTimeToRtc(mktime(&nt)); // local -> UTC
                mcToast("Time saved", MC_GREEN, 900);
                return;
            }
        }
        delay(15);
    }
}

void mcTimeSettings() {
    listScreen(
        "Time & date",
        [&]() {
            std::vector<McRow> rows;
            rows.push_back({String("Set date & time"), mcClockString(), 0, 0});
            rows.push_back({"Time zone", String(mcTimeZones[mcCfg.tzIndex].name), 0, 0});
            rows.push_back({"24-hour clock", String(mcCfg.h24 ? "On" : "Off"), 0, 0});
            return rows;
        },
        [&](int idx) {
            if (idx == 0) setTimeScreen();
            else if (idx == 1) {
                mcCfg.tzIndex = (mcCfg.tzIndex + 1) % mcTimeZoneCount; // net task keeps its own copy of the TZ string
                mcApplyTimeZone();
                mcConfigSave();
            } else if (idx == 2) {
                mcCfg.h24 = !mcCfg.h24;
                mcConfigSave();
            }
            return true;
        },
        [](int, int) {});
}

// Engine chooser (install layout: "not enough space" -> which app to replace).
// options[0] is the heading, the last option is Cancel; the chosen option's action runs, as in Launcher.
int mantisEngineChoose(std::vector<Option> &options) {
    if (options.size() < 2) return -1;
    std::vector<int> map; // at most 3 choices + Cancel fit on screen
    std::vector<String> labels;
    for (size_t i = 1; i + 1 < options.size() && map.size() < 3; i++) {
        map.push_back(i);
        labels.push_back(options[i].label);
    }
    labels.push_back("Cancel");
    int pick = mcActionSheet("Not enough space - " + options[0].label, labels);
    if (pick < 0 || pick >= (int)map.size()) return -1;
    int idx = map[pick];
    if (options[idx].operation) options[idx].operation();
    return idx;
}
