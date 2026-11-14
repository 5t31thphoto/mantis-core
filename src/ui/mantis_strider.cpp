// MantisCore built-in app: Strider. Walk or ride with the Core2, get a generative print.
//
// There is no GPS and no compass: the route is dead reckoning, an etch-a-sketch drawn by your body.
//   Heading  = gyro rate about gravity (works however the Core2 is held, pocketed or mounted),
//              bias calibrated at the start and re-learned whenever you stand still.
//   Distance = walking: step peaks in vertical acceleration x a stride from cadence.
//              riding: travel estimated from road vibration (artistic, not a speedometer).
//   Environment along the way: loudness (mic), ground roughness (vibration above the walking
//   bounce), Wi-Fi networks (async scans, each new network is a "find"), places you stood still.
// When you finish, src/strider renders the walk as a print (TOPO / RISO / LIGHT / STARS); every
// style you SAVE is written to /images/strider/*.mg (Mantis .mg: MG01, 320x240, RGB565 LE).
//
// Everything runs on the UI task: IMU, touch and the vibration motor share the internal I2C bus
// and the SD card shares the display's SPI bus, so one task owns them all.
#include "../strider/strider_art.h"
#include "../strider/strider_walk.h"
#include "mantis_internal.h"
#include <Preferences.h>
#include <SD.h>
#include <WiFi.h>
#include <algorithm>
#include <math.h>
#include <time.h>

using namespace strider;

namespace {

constexpr int SAMPLE_CAP = 4096; // recorded points (thinned 2:1 when full, so any length fits)
constexpr float DEG2RAD = 0.01745329252f;
const char *const PRINT_DIR = "/images/strider";

#define ST_BG 0x0841      // near-black navy
#define ST_PANEL 0x10A3
#define ST_TRAIL 0x47F9   // mint
#define ST_TRAIL2 0x05BF  // teal-blue
#define ST_DIM 0x4228
#define ST_AMBER 0xFD20

// ------------------------------------------------------------------ memory
Walk *s_walk = nullptr;
Sample *s_samples = nullptr;
uint16_t *s_print = nullptr; // 320x240 native RGB565
void *s_artMem = nullptr;

bool allocAll() {
    if (!s_walk) s_walk = (Walk *)heap_caps_malloc(sizeof(Walk), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_samples) s_samples = (Sample *)heap_caps_malloc(sizeof(Sample) * SAMPLE_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_print) s_print = (uint16_t *)heap_caps_malloc(ART_W * ART_H * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_artMem) s_artMem = heap_caps_malloc(artMemBytes(), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return s_walk && s_samples && s_print && s_artMem;
}
void freeAll() {
    free(s_walk), free(s_samples), free(s_print), free(s_artMem);
    s_walk = nullptr, s_samples = nullptr, s_print = nullptr, s_artMem = nullptr;
}

// ------------------------------------------------------------------ recording state
Mode s_mode = MODE_WALK;
bool s_recording = false, s_paused = false;
uint32_t s_activeMs = 0, s_lastClock = 0; // walk clock, pauses excluded
float activeSec() { return s_activeMs / 1000.f; }

// ------------------------------------------------------------------ IMU: heading, steps, roughness
struct ImuState {
    bool ok = false;
    float ax = 0, ay = 0, az = 0, gx = 0, gy = 0, gz = 0; // last raw sample (to spot new data)
    uint32_t lastUs = 0;
    float t = 0;                                         // sensor clock, s
    float ux = 0, uy = 0, uz = 1;                        // gravity ("up") in device frame
    float bx = 0, by = 0, bz = 0;                        // gyro bias, rad/s
    float heading = 0;                                   // rad, CCW
    float vLp = 0, env = 0.15f;                          // vertical accel (g) and its envelope
    bool armed = false;
    float lastStepT = -10, cadence = 0;                  // steps per minute
    float magFast = 1, magSlow = 1;                      // |a| low-passes
    float roughAcc = 0, vibAcc = 0, winT = 0;
    int winN = 0;
    float rough = 0, vib = 0;                            // 0..1 / g rms
    float stillT = 0, lastMoveT = -100;
    // calibration accumulators
    double cgx = 0, cgy = 0, cgz = 0;
    int cn = 0;
} imu;

void onStep(float interval) {
    imu.lastMoveT = imu.t;
    if (s_mode != MODE_WALK || !s_recording || s_paused) return;
    float stride = 0.74f;
    if (interval > 0.3f && interval < 1.6f) {
        float spm = 60.f / interval;
        stride = fminf(0.90f, fmaxf(0.55f, 0.98f - spm * 0.0022f)); // faster cadence, shorter steps
        imu.cadence = imu.cadence * 0.8f + spm * 0.2f;
    }
    s_walk->steps++;
    walkAdvance(*s_walk, stride, imu.heading);
}

// returns true when a new sample was processed
bool imuTick() {
    if (!imu.ok) return false;
    M5.Imu.update();
    auto d = M5.Imu.getImuData();
    if (d.accel.x == imu.ax && d.accel.y == imu.ay && d.accel.z == imu.az && d.gyro.x == imu.gx && d.gyro.y == imu.gy &&
        d.gyro.z == imu.gz)
        return false; // nothing new
    imu.ax = d.accel.x, imu.ay = d.accel.y, imu.az = d.accel.z;
    imu.gx = d.gyro.x, imu.gy = d.gyro.y, imu.gz = d.gyro.z;
    uint32_t us = micros();
    float dt = imu.lastUs ? (uint32_t)(us - imu.lastUs) / 1e6f : 0.01f;
    imu.lastUs = us;
    if (dt <= 0) return false;
    if (dt > 0.2f) dt = 0.2f;
    imu.t += dt;

    float ax = imu.ax, ay = imu.ay, az = imu.az, mag = sqrtf(ax * ax + ay * ay + az * az);
    // gravity direction (tau 0.8 s)
    float k = dt / (0.8f + dt);
    imu.ux += (ax - imu.ux) * k, imu.uy += (ay - imu.uy) * k, imu.uz += (az - imu.uz) * k;
    float ul = sqrtf(imu.ux * imu.ux + imu.uy * imu.uy + imu.uz * imu.uz);
    float upx = 0, upy = 0, upz = 1;
    if (ul > 0.3f) upx = imu.ux / ul, upy = imu.uy / ul, upz = imu.uz / ul;
    // heading: gyro rate about gravity
    float gxr = imu.gx * DEG2RAD - imu.bx, gyr = imu.gy * DEG2RAD - imu.by, gzr = imu.gz * DEG2RAD - imu.bz;
    imu.heading += (gxr * upx + gyr * upy + gzr * upz) * dt;
    // steps: peaks of vertical acceleration, adaptive threshold, 0.27 s refractory (<= 220 spm)
    float vert = ax * upx + ay * upy + az * upz - 1.0f;
    imu.vLp += (vert - imu.vLp) * (dt / (0.05f + dt));
    imu.env = fmaxf(fabsf(imu.vLp), imu.env * expf(-dt * 0.5f));
    float thr = fmaxf(0.05f, 0.42f * imu.env);
    if (!imu.armed && imu.vLp < -thr * 0.4f) imu.armed = true;
    if (imu.armed && imu.vLp > thr && imu.t - imu.lastStepT > 0.27f) {
        imu.armed = false;
        float iv = imu.t - imu.lastStepT;
        imu.lastStepT = imu.t;
        onStep(iv);
    }
    // roughness: vibration faster than the walking bounce (> ~5 Hz); vib: all motion (> ~0.2 Hz)
    imu.magFast += (mag - imu.magFast) * (dt / (0.03f + dt));
    imu.magSlow += (mag - imu.magSlow) * (dt / (1.0f + dt));
    float hp = mag - imu.magFast, mv = mag - imu.magSlow;
    imu.roughAcc += hp * hp, imu.vibAcc += mv * mv, imu.winN++, imu.winT += dt;
    if (imu.winT >= 0.5f) {
        float r = sqrtf(imu.roughAcc / imu.winN), v = sqrtf(imu.vibAcc / imu.winN);
        imu.rough = imu.rough * 0.6f + fminf(1.f, r / 0.22f) * 0.4f;
        imu.vib = imu.vib * 0.6f + v * 0.4f;
        imu.roughAcc = imu.vibAcc = imu.winT = 0;
        imu.winN = 0;
        if (s_mode == MODE_RIDE && imu.vib > 0.045f) imu.lastMoveT = imu.t;
    }
    // gyro bias: re-learn slowly whenever the device is properly still
    float gm = sqrtf(gxr * gxr + gyr * gyr + gzr * gzr);
    if (fabsf(mag - 1.f) < 0.04f && gm < 0.06f) imu.stillT += dt;
    else imu.stillT = 0;
    if (imu.stillT > 1.0f) {
        float a = fminf(1.f, dt * 0.4f);
        imu.bx += (imu.gx * DEG2RAD - imu.bx) * a, imu.by += (imu.gy * DEG2RAD - imu.by) * a,
            imu.bz += (imu.gz * DEG2RAD - imu.bz) * a;
    }
    return true;
}

bool imuMoving() { return imu.t - imu.lastMoveT < 3.0f; }

// ------------------------------------------------------------------ microphone: loudness + loud moments
struct MicState {
    bool ok = false, pending = false;
    int16_t buf[256];
    float level = 0, slow = 0.3f, lastEvent = -100;
} mic;

void micTick() {
    if (!mic.ok || M5.Mic.isRecording()) return;
    if (mic.pending) {
        double acc = 0;
        for (int i = 0; i < 256; i++) acc += (double)mic.buf[i] * mic.buf[i];
        float rms = sqrtf((float)(acc / 256)) / 32768.f;
        float db = 20.f * log10f(rms + 1e-7f);
        float lvl = fminf(1.f, fmaxf(0.f, (db + 62.f) / 44.f)); // -62 dBFS .. -18 dBFS
        mic.level = mic.level * 0.7f + lvl * 0.3f;
        mic.slow += (mic.level - mic.slow) * 0.002f; // ~8 s
        if (s_recording && !s_paused && lvl > 0.6f && lvl > mic.slow + 0.3f && imu.t - mic.lastEvent > 4.f) {
            mic.lastEvent = imu.t;
            walkAddFind(*s_walk, FIND_SOUND, (uint32_t)(imu.t * 1000.f) * 2654435761u, (int)(lvl * 127));
        }
    }
    mic.pending = M5.Mic.record(mic.buf, 256, 16000);
}

// ------------------------------------------------------------------ Wi-Fi: async scans, new networks are finds
struct RfState {
    bool on = false, scanning = false;
    uint32_t lastScan = 0;
    int count = 0;
    uint32_t seen[1024];
    int nSeen = 0;
} rf;

bool rfFirstSighting(uint32_t h) {
    if (!h) h = 1;
    for (int i = 0, k = h & 1023; i < 1024; i++, k = (k + 1) & 1023) {
        if (rf.seen[k] == h) return false;
        if (!rf.seen[k]) {
            if (rf.nSeen > 900) return true; // table nearly full: still a sighting, just not remembered
            rf.seen[k] = h;
            rf.nSeen++;
            return true;
        }
    }
    return true;
}

void rfTick() {
    if (!rf.on) return;
    uint32_t now = millis();
    if (rf.scanning) {
        int n = WiFi.scanComplete();
        if (n == WIFI_SCAN_RUNNING) {
            if (now - rf.lastScan > 9000) { // stuck: drop it
                WiFi.scanDelete();
                rf.scanning = false;
            }
            return;
        }
        rf.scanning = false;
        if (n >= 0) {
            rf.count = n;
            int added = 0;
            for (int i = 0; i < n; i++) {
                uint8_t *b = WiFi.BSSID(i);
                if (!b) continue;
                uint32_t h = 2166136261u;
                for (int k = 0; k < 6; k++) h = (h ^ b[k]) * 16777619u;
                if (rfFirstSighting(h) && s_recording && !s_paused && added < 12) {
                    walkAddFind(*s_walk, FIND_WIFI, h, WiFi.RSSI(i));
                    added++;
                }
            }
        }
        WiFi.scanDelete();
        return;
    }
    uint32_t gap = imuMoving() ? 10000 : 30000;
    if (now - rf.lastScan >= gap) {
        rf.lastScan = now;
        rf.scanning = WiFi.scanNetworks(true, true, false, 110) == WIFI_SCAN_RUNNING;
    }
}

// ------------------------------------------------------------------ sensors on/off
void sensorsStart() {
    imu = ImuState();
    imu.ok = M5.Imu.isEnabled();
    M5.Speaker.end(); // the Core2 mic and speaker share I2S
    auto mc = M5.Mic.config();
    mc.sample_rate = 16000;
    mc.stereo = false;
    M5.Mic.config(mc);
    mic = MicState();
    mic.ok = M5.Mic.begin();
    memset(rf.seen, 0, sizeof(rf.seen));
    rf.nSeen = 0;
    rf.count = 0;
    rf.scanning = false;
    rf.lastScan = millis() - 8000; // first scan right away
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(false);
    rf.on = true;
}
void sensorsStop() {
    if (rf.on) {
        WiFi.scanDelete();
        WiFi.mode(WIFI_OFF);
        rf.on = false;
    }
    if (mic.ok) {
        for (int i = 0; i < 50 && M5.Mic.isRecording(); i++) delay(2);
        M5.Mic.end();
        mic.ok = false;
    }
    M5.Power.setVibration(0);
}

// everything that has to keep running, whatever the screen is doing
uint32_t s_lastEnv = 0, s_lastRide = 0, s_vibUntil = 0;
float s_nextMilestone = 500;
void buzz(uint8_t level, uint32_t ms) {
    M5.Power.setVibration(level);
    s_vibUntil = millis() + ms;
}
void senseTick() {
    for (int i = 0; i < 4 && imuTick(); i++) {
    }
    micTick();
    rfTick();
    uint32_t now = millis();
    if (s_vibUntil && (int32_t)(now - s_vibUntil) >= 0) {
        M5.Power.setVibration(0);
        s_vibUntil = 0;
    }
    if (!s_recording) return;
    if (!s_paused) s_activeMs += now - s_lastClock;
    s_lastClock = now;
    // riding: the pen moves while the bike vibrates
    if (s_mode == MODE_RIDE && now - s_lastRide >= 250) {
        float dt = (now - s_lastRide) / 1000.f;
        s_lastRide = now;
        if (dt > 1) dt = 0.25f;
        if (!s_paused && imu.vib > 0.045f) {
            float speed = 2.5f + 7.0f * fminf(1.f, fmaxf(0.f, (imu.vib - 0.045f) / 0.35f)); // m/s, a guess by design
            walkAdvance(*s_walk, speed * dt, imu.heading);
            imu.cadence = speed; // shown as pace
        }
    }
    if (now - s_lastEnv >= 250) {
        s_lastEnv = now;
        float pace = s_mode == MODE_WALK ? fminf(1.f, fmaxf(0.f, (imu.cadence - 70.f) / 70.f)) : imu.cadence / 9.5f;
        if (!imuMoving()) pace *= 0.5f;
        if (!s_paused) {
            walkSetEnv(*s_walk, mic.level, imu.rough, fminf(1.f, rf.count / 25.f), pace);
            walkTick(*s_walk, activeSec(), imuMoving());
        }
        if (s_walk->distance >= s_nextMilestone) { // a nudge every 500 m walking / 2 km riding
            s_nextMilestone += s_mode == MODE_WALK ? 500.f : 2000.f;
            buzz(200, 140);
        }
    }
}

// ------------------------------------------------------------------ drawing helpers
void pushWithSensing() { // push in strips, sensing between them, so steps aren't missed
    for (int y = 0; y < MC_H; y += 40) {
        M5.Display.setClipRect(0, y, MC_W, 40);
        mcCanvas.pushSprite(0, 0);
        M5.Display.waitDMA();
        M5.Display.clearClipRect();
        senseTick();
    }
}
void button(int x, int y, int w, int h, const char *label, uint16_t fill, uint16_t text = MC_WHITE) {
    mcCanvas.fillSmoothRoundRect(x, y, w, h, 10, fill);
    mcText(label, x + w / 2, y + h / 2 + 1, &fonts::FreeSansBold9pt7b, text, middle_center);
}
bool hit(int x, int y, int w, int h) {
    return mcTouch.startX >= x && mcTouch.startX < x + w && mcTouch.startY >= y && mcTouch.startY < y + h;
}
void closeX() {
    mcCanvas.fillSmoothCircle(MC_W - 18, 16, 11, MC_PURPLE);
    mcText("X", MC_W - 18, 17, &fonts::Font2, MC_WHITE, middle_center);
}
void fmtClock(float s, char *b, size_t n) {
    int t = (int)s;
    if (t >= 3600) snprintf(b, n, "%d:%02d:%02d", t / 3600, t / 60 % 60, t % 60);
    else snprintf(b, n, "%d:%02d", t / 60, t % 60);
}
void fmtKm(float m, char *b, size_t n) {
    if (m < 1000) snprintf(b, n, "%d m", (int)m);
    else snprintf(b, n, "%.2f km", m / 1000.f);
}

// ------------------------------------------------------------------ time
bool nowLocal(struct tm &lt) {
    time_t t = time(nullptr);
    if (t < 1704067200) return false; // clock not set
    localtime_r(&t, &lt);
    return true;
}
float todNow() {
    struct tm lt;
    if (!nowLocal(lt)) return 0.5f;
    return (lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec) / 86400.f;
}

// ------------------------------------------------------------------ prints on the card
int countPrints(String &newest) {
    int n = 0;
    newest = "";
    File d = SD.open(PRINT_DIR);
    if (!d) return 0;
    for (File f = d.openNextFile(); f; f = d.openNextFile()) {
        String nm = f.name();
        bool dir = f.isDirectory();
        f.close();
        int s = nm.lastIndexOf('/');
        if (s >= 0) nm = nm.substring(s + 1);
        if (dir || nm.startsWith(".") || !(nm.endsWith(".mg") || nm.endsWith(".MG"))) continue;
        n++;
        if (nm.compareTo(newest) > 0) newest = nm; // dated names sort by time
    }
    d.close();
    return n;
}

bool savePrint(int style, String &outPath) {
    if (!SD.exists("/images")) SD.mkdir("/images");
    if (!SD.exists(PRINT_DIR)) SD.mkdir(PRINT_DIR);
    char st[8];
    snprintf(st, sizeof st, "%s", styleName(style));
    for (char *p = st; *p; p++) *p = (char)tolower(*p);
    char base[80];
    const Walk &w = *s_walk;
    if (w.year >= 2024)
        snprintf(base, sizeof base, "%s/%04d%02d%02d_%02d%02d_%s", PRINT_DIR, w.year, w.month, w.day, w.hour, w.minute, st);
    else snprintf(base, sizeof base, "%s/walk_%08lx_%s", PRINT_DIR, (unsigned long)walkSeed(w), st);
    String path = String(base) + ".mg";
    for (int k = 2; SD.exists(path) && k < 100; k++) path = String(base) + "_" + k + ".mg";
    File f = SD.open(path, FILE_WRITE);
    if (!f) return false;
    static const uint8_t HDR[8] = {0x4D, 0x47, 0x30, 0x31, 0x40, 0x01, 0xF0, 0x00}; // "MG01", 320, 240 (LE)
    bool ok = f.write(HDR, 8) == 8;
    const uint8_t *px = (const uint8_t *)s_print; // RGB565, little-endian == native order
    static uint8_t chunk[4096];
    for (size_t off = 0; ok && off < (size_t)ART_W * ART_H * 2; off += sizeof chunk) {
        size_t n = std::min(sizeof chunk, (size_t)ART_W * ART_H * 2 - off);
        memcpy(chunk, px + off, n); // PSRAM -> internal RAM for the SD driver
        ok = f.write(chunk, n) == n;
    }
    f.close();
    if (ok) {
        File chk = SD.open(path);
        ok = chk && chk.size() == 153608;
        if (chk) chk.close();
    }
    if (!ok) SD.remove(path);
    else outPath = path;
    return ok;
}

// ================================================================== start screen
// returns false to leave the app
bool startScreen() {
    String newest;
    int prints = countPrints(newest);
    bool haveBg = newest.length() && mcLoadMg((String(PRINT_DIR) + "/" + newest).c_str(), s_print);
    mcWaitRelease();
    bool dirty = true;
    for (;;) {
        if (dirty) {
            dirty = false;
            if (haveBg) {
                mcBlitRGB(0, 0, MC_W, MC_H, s_print);
                mcDarkenRect(0, 0, MC_W, MC_H, 70);
            } else {
                for (int y = 0; y < MC_H; y++) mcCanvas.drawFastHLine(0, y, MC_W, mcBlend(0x18A9, ST_BG, (uint8_t)(y * 255 / MC_H)));
            }
            mcText("STRIDER", 20, 34, &fonts::FreeSansBold18pt7b, MC_WHITE, middle_left, true);
            mcText("Walk or ride. Your route, sounds and the", 21, 62, &fonts::Font2, MC_GREY, middle_left, true);
            mcText("networks you pass become a print.", 21, 78, &fonts::Font2, MC_GREY, middle_left, true);
            closeX();
            // mode switch
            const int mx = 20, my = 100, mw = 280, mh = 40;
            mcCanvas.fillSmoothRoundRect(mx, my, mw, mh, 12, ST_PANEL);
            int sel = s_mode == MODE_WALK ? 0 : 1;
            mcCanvas.fillSmoothRoundRect(mx + 3 + sel * (mw / 2), my + 3, mw / 2 - 6, mh - 6, 10, MC_TEAL);
            mcText("WALK", mx + mw / 4, my + mh / 2 + 1, &fonts::FreeSansBold9pt7b, sel == 0 ? MC_WHITE : MC_GREY, middle_center);
            mcText("RIDE", mx + 3 * mw / 4, my + mh / 2 + 1, &fonts::FreeSansBold9pt7b, sel == 1 ? MC_WHITE : MC_GREY, middle_center);
            button(20, 152, 280, 48, "START", MC_PURPLE);
            char b[64];
            snprintf(b, sizeof b, "%d print%s in /images/strider", prints, prints == 1 ? "" : "s");
            mcText(b, 160, 218, &fonts::Font0, MC_GREY, middle_center, true);
            if (time(nullptr) < 1704067200)
                mcText("Clock not set: prints will be undated", 160, 230, &fonts::Font0, ST_AMBER, middle_center, true);
            mcPush();
        }
        mcPollTouch();
        if (mcTouch.released && !mcTouch.moved) {
            if (mcTouch.startY < 34 && mcTouch.startX > MC_W - 44) return false;
            if (hit(20, 100, 140, 40)) s_mode = MODE_WALK, dirty = true;
            else if (hit(160, 100, 140, 40)) s_mode = MODE_RIDE, dirty = true;
            else if (hit(20, 152, 280, 48)) return true;
        }
        delay(15);
    }
}

// ================================================================== calibration
// Two still seconds to learn the gyro bias. Gives up waiting after 10 s and uses what it has.
bool calibrate() {
    uint32_t t0 = millis(), stillSince = millis(), lastDraw = 0;
    imu.cgx = imu.cgy = imu.cgz = 0;
    imu.cn = 0;
    mcWaitRelease();
    for (;;) {
        senseTick();
        uint32_t now = millis();
        float gm = sqrtf(imu.gx * imu.gx + imu.gy * imu.gy + imu.gz * imu.gz) * DEG2RAD;
        float mag = sqrtf(imu.ax * imu.ax + imu.ay * imu.ay + imu.az * imu.az);
        bool still = gm < 0.25f && fabsf(mag - 1.f) < 0.08f;
        if (!still) {
            stillSince = now;
            imu.cgx = imu.cgy = imu.cgz = 0;
            imu.cn = 0;
        } else {
            imu.cgx += imu.gx * DEG2RAD, imu.cgy += imu.gy * DEG2RAD, imu.cgz += imu.gz * DEG2RAD;
            imu.cn++;
        }
        float prog = fminf(1.f, (now - stillSince) / 2000.f);
        if (prog >= 1.f || now - t0 > 10000 || !imu.ok) break;
        if (now - lastDraw > 60) {
            lastDraw = now;
            mcCanvas.fillScreen(ST_BG);
            closeX();
            mcText("Hold still", 160, 70, &fonts::FreeSansBold12pt7b, MC_WHITE, middle_center);
            mcText("Learning the gyro so your lines run true", 160, 96, &fonts::Font2, MC_GREY, middle_center);
            mcCanvas.drawArc(160, 160, 34, 30, 0, 360, ST_PANEL);
            if (prog > 0) mcCanvas.fillArc(160, 160, 34, 30, 270, 270 + (int)(360 * prog), ST_TRAIL);
            mcText(still ? "..." : "moving", 160, 160, &fonts::Font2, still ? ST_TRAIL : ST_AMBER, middle_center);
            mcPush();
        }
        mcPollTouch();
        if (mcTouch.released && mcTouch.startY < 34 && mcTouch.startX > MC_W - 44) return false;
        delay(4);
    }
    if (imu.cn > 20) imu.bx = imu.cgx / imu.cn, imu.by = imu.cgy / imu.cn, imu.bz = imu.cgz / imu.cn;
    imu.heading = 0;
    return true;
}

// ================================================================== walking
void drawLive(bool holdingFinish, float holdFrac) {
    const Walk &w = *s_walk;
    mcCanvas.fillScreen(ST_BG);
    // stats
    char a[24], b[24], c[24];
    fmtKm(w.distance, a, sizeof a);
    if (s_mode == MODE_WALK) snprintf(b, sizeof b, "%lu", (unsigned long)w.steps);
    else snprintf(b, sizeof b, "~%.0f km/h", imuMoving() ? imu.cadence * 3.6f : 0.f);
    fmtClock(activeSec(), c, sizeof c);
    const char *lab[3] = {s_mode == MODE_WALK ? "DISTANCE" : "DISTANCE ~", s_mode == MODE_WALK ? "STEPS" : "PACE ~", "TIME"};
    const char *val[3] = {a, b, c};
    for (int i = 0; i < 3; i++) {
        int x = 8 + i * 104;
        mcText(lab[i], x, 9, &fonts::Font0, MC_GREY, middle_left);
        mcText(val[i], x, 27, &fonts::FreeSansBold12pt7b, MC_WHITE, middle_left);
    }
    // trail panel
    const int px = 4, py = 44, pw = 312, ph = 150;
    mcCanvas.fillSmoothRoundRect(px, py, pw, ph, 10, ST_PANEL);
    int n = w.n;
    float minx = w.x, maxx = w.x, miny = w.y, maxy = w.y;
    for (int i = 0; i < n; i++) {
        minx = fminf(minx, w.samples[i].x), maxx = fmaxf(maxx, w.samples[i].x);
        miny = fminf(miny, w.samples[i].y), maxy = fmaxf(maxy, w.samples[i].y);
    }
    float ex = fmaxf(maxx - minx, 20.f), ey = fmaxf(maxy - miny, 20.f);
    float sc = fminf((pw - 24) / ex, (ph - 24) / ey);
    float cx = (minx + maxx) / 2, cy = (miny + maxy) / 2;
    auto sx = [&](float x) { return (int)(px + pw / 2 + (x - cx) * sc); };
    auto sy = [&](float y) { return (int)(py + ph / 2 - (y - cy) * sc); };
    int step = n > 700 ? n / 700 : 1;
    int lx = sx(w.samples[0].x), ly = sy(w.samples[0].y);
    for (int i = step; i < n; i += step) {
        int x = sx(w.samples[i].x), y = sy(w.samples[i].y);
        uint16_t col = mcBlend(ST_TRAIL, ST_TRAIL2, (uint8_t)(i * 255 / n));
        mcCanvas.drawLine(lx, ly, x, y, col);
        mcCanvas.drawLine(lx + 1, ly, x + 1, y, col);
        lx = x, ly = y;
    }
    int hx = sx(w.x), hy = sy(w.y);
    mcCanvas.drawLine(lx, ly, hx, hy, ST_TRAIL);
    for (int i = 0; i < w.nFinds; i++) {
        const Find &f = w.finds[i];
        mcCanvas.fillCircle(sx(f.x), sy(f.y), f.kind == FIND_WIFI ? 2 : 3, f.kind == FIND_WIFI ? 0xC81F : ST_AMBER);
    }
    for (int i = 0; i < w.nStops; i++) mcCanvas.drawCircle(sx(w.stops[i].x), sy(w.stops[i].y), 4, MC_WHITE);
    mcCanvas.fillCircle(sx(w.samples[0].x), sy(w.samples[0].y), 3, MC_GREEN);
    // you are here + heading
    mcCanvas.fillSmoothCircle(hx, hy, 5, MC_WHITE);
    mcCanvas.drawLine(hx, hy, hx + (int)(cosf(imu.heading) * 14), hy - (int)(sinf(imu.heading) * 14), MC_WHITE);
    char sb[48];
    if (s_paused) mcText("PAUSED", px + pw / 2, py + 16, &fonts::FreeSansBold9pt7b, ST_AMBER, middle_center);
    // footer: sound meter, networks, buttons
    mcText("SOUND", 8, 205, &fonts::Font0, MC_GREY, middle_left);
    for (int i = 0; i < 10; i++) {
        bool on = mic.level * 10 > i;
        mcCanvas.fillRect(8 + i * 7, 213, 5, 9, on ? (i > 7 ? ST_AMBER : ST_TRAIL) : ST_PANEL);
    }
    snprintf(sb, sizeof sb, "WI-FI %lu", (unsigned long)w.rfSeen);
    mcText(sb, 8, 232, &fonts::Font0, MC_GREY, middle_left);
    button(84, 200, 100, 36, s_paused ? "RESUME" : "PAUSE", s_paused ? MC_TEAL : ST_PANEL);
    mcCanvas.fillSmoothRoundRect(190, 200, 126, 36, 10, holdingFinish ? MC_CHARCOAL2 : MC_PURPLE);
    if (holdingFinish) mcCanvas.fillSmoothRoundRect(190, 200, (int)(126 * holdFrac) + 1, 36, 10, MC_PURPLE);
    mcText(holdingFinish ? "HOLD..." : "FINISH", 253, 219, &fonts::FreeSansBold9pt7b, MC_WHITE, middle_center);
}

// returns true when the walk was finished (false = nothing recorded / left)
bool walkScreen() {
    struct tm lt;
    Walk &w = *s_walk;
    if (nowLocal(lt)) w.year = lt.tm_year + 1900, w.month = lt.tm_mon + 1, w.day = lt.tm_mday, w.hour = lt.tm_hour, w.minute = lt.tm_min;
    else w.year = w.month = w.day = w.hour = w.minute = 0;
    walkBegin(w, s_samples, SAMPLE_CAP, s_mode, todNow());
    s_recording = true;
    s_paused = false;
    s_activeMs = 0;
    s_lastClock = s_lastEnv = s_lastRide = millis();
    s_nextMilestone = s_mode == MODE_WALK ? 500.f : 2000.f;
    buzz(160, 120);

    uint32_t lastDraw = 0, lastInput = millis(), holdStart = 0;
    bool dim = false, holding = false;
    mcWaitRelease();
    for (;;) {
        senseTick();
        mcPollTouch();
        uint32_t now = millis();
        if (mcTouch.pressed) {
            lastInput = now;
            if (dim) { // first touch only wakes the screen
                dim = false;
                mcApplyBrightness();
                while (mcTouch.down) {
                    senseTick();
                    mcPollTouch();
                    delay(4);
                }
                lastDraw = 0;
                continue;
            }
            if (hit(190, 200, 126, 40)) holding = true, holdStart = now;
        }
        if (holding) {
            if (!mcTouch.down) holding = false; // let go too early
            else if (now - holdStart >= 1200) break; // finished
        }
        if (mcTouch.released && !mcTouch.moved && hit(84, 200, 100, 40)) {
            s_paused = !s_paused;
            buzz(120, 60);
            lastDraw = 0;
        }
        if (!dim && now - lastInput > 30000) { // pocket mode: screen off, sensing carries on
            dim = true;
            M5.Display.setBrightness(2);
        }
        if (!dim && (now - lastDraw >= (holding ? 50u : 400u))) {
            lastDraw = now;
            drawLive(holding, holding ? (now - holdStart) / 1200.f : 0);
            pushWithSensing();
        }
        delay(3);
    }
    s_recording = false;
    walkEnd(w, todNow());
    buzz(220, 200);
    mcApplyBrightness();
    return true;
}

// ================================================================== result: develop, flip styles, save
void developing(int style) {
    mcCanvas.fillScreen(ST_BG);
    mcText("Developing", 160, 104, &fonts::FreeSansBold12pt7b, MC_WHITE, middle_center);
    char b[32];
    snprintf(b, sizeof b, "%s print", styleName(style));
    mcText(b, 160, 132, &fonts::Font2, ST_TRAIL, middle_center);
    mcPush();
}

void drawResult(int style, bool toolbar, const bool *saved) {
    mcBlitRGB(0, 0, MC_W, MC_H, s_print);
    if (!toolbar) return;
    mcDarkenRect(0, 186, MC_W, 54, 60);
    button(6, 194, 40, 40, "<", ST_PANEL);
    button(154, 194, 40, 40, ">", ST_PANEL);
    char b[24];
    snprintf(b, sizeof b, "%s %d/%d", styleName(style), style + 1, (int)STYLE_COUNT);
    mcText(b, 100, 214, &fonts::FreeSansBold9pt7b, MC_WHITE, middle_center);
    button(200, 194, 56, 40, saved[style] ? "SAVED" : "SAVE", saved[style] ? MC_TEAL : MC_PURPLE);
    button(260, 194, 54, 40, "DONE", ST_PANEL);
}

void resultScreen() {
    int style = styleDefault(*s_walk);
    bool saved[STYLE_COUNT] = {};
    bool anySaved = false, toolbar = true, dirty = true;
    int rendered = -1;
    mcWaitRelease();
    for (;;) {
        if (rendered != style) {
            developing(style);
            artSetYield([] { vTaskDelay(1); });
            artRender(*s_walk, style, s_print, s_artMem);
            artSetYield(nullptr);
            rendered = style;
            dirty = true;
        }
        if (dirty) {
            dirty = false;
            drawResult(style, toolbar, saved);
            mcPush();
        }
        mcPollTouch();
        if (mcTouch.released) {
            int dx = mcTouch.x - mcTouch.startX;
            if (mcTouch.moved && abs(dx) > 50) { // swipe between styles
                style = (style + (dx < 0 ? 1 : STYLE_COUNT - 1)) % STYLE_COUNT;
            } else if (!mcTouch.moved) {
                if (!toolbar || mcTouch.startY < 186) {
                    toolbar = !toolbar;
                    dirty = true;
                } else if (hit(6, 190, 44, 50)) style = (style + STYLE_COUNT - 1) % STYLE_COUNT;
                else if (hit(150, 190, 48, 50)) style = (style + 1) % STYLE_COUNT;
                else if (hit(200, 190, 56, 50) && !saved[style]) {
                    String path;
                    if (savePrint(style, path)) {
                        saved[style] = anySaved = true;
                        buzz(150, 80);
                        int s = path.lastIndexOf('/');
                        drawResult(style, toolbar, saved);
                        mcToast("Saved " + path.substring(s + 1), MC_TEAL, 1600);
                    } else {
                        mcToast("Couldn't save to the SD card", 0xF800, 2000);
                    }
                    dirty = true;
                } else if (hit(260, 190, 56, 50)) {
                    if (anySaved || mcConfirm("Leave without saving?\nThis walk's print will be lost.", "Leave", "Stay"))
                        return;
                    dirty = true;
                }
            }
        }
        // the vibration motor is switched off by senseTick, which isn't running here
        if (s_vibUntil && (int32_t)(millis() - s_vibUntil) >= 0) {
            M5.Power.setVibration(0);
            s_vibUntil = 0;
        }
        delay(15);
    }
}

} // namespace

// ================================================================== entry
void mcStrider() {
    mcWaitRelease();
    if (!allocAll()) {
        freeAll();
        mcToast("Not enough memory for Strider", 0xF800, 2000);
        return;
    }
    Preferences p;
    p.begin("strider", true);
    s_mode = p.getUChar("mode", MODE_WALK) == MODE_RIDE ? MODE_RIDE : MODE_WALK;
    p.end();
    mcNetStop(); // Strider scans Wi-Fi itself and never connects
    for (;;) {
        if (!startScreen()) break;
        sensorsStart();
        bool go = calibrate();
        if (go && walkScreen()) {
            sensorsStop();
            resultScreen();
        } else {
            sensorsStop();
        }
    }
    sensorsStop();
    p.begin("strider", false);
    p.putUChar("mode", s_mode);
    p.end();
    freeAll();
    mcApplyBrightness();
    mcWaitRelease();
    mcNetStart();
}

// Home-screen icon for the Strider tile (used when /icons/strider.mi is absent): a looping route
// on a night-blue tile, with a few stars where networks were found.
bool mcStriderIcon(McIcon &icon) {
    icon.rgb = (uint16_t *)ps_malloc(64 * 64 * 2);
    icon.a = (uint8_t *)ps_malloc(64 * 64);
    if (!icon.rgb || !icon.a) {
        mcFreeIcon(icon);
        return false;
    }
    auto rr = [](float x, float y) { // rounded-rect coverage, radius 14
        float qx = fmaxf(fabsf(x - 31.5f) - (32 - 14), 0.f), qy = fmaxf(fabsf(y - 31.5f) - (32 - 14), 0.f);
        return fminf(1.f, fmaxf(0.f, 14.5f - sqrtf(qx * qx + qy * qy)));
    };
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            int i = y * 64 + x;
            icon.a[i] = (uint8_t)(rr(x, y) * 255);
            icon.rgb[i] = mcBlend(0x2A4F, 0x0886, (uint8_t)(y * 4)); // night gradient
        }
    // route: a wobbly loop, drawn as a glowing line
    for (int s = 0; s < 900; s++) {
        float t = s / 900.f * 6.2832f;
        float r = 17 + 4 * sinf(3 * t + 0.6f) + 2.5f * sinf(5 * t);
        float x = 32 + cosf(t) * r * 1.05f, y = 33 + sinf(t) * r * 0.8f;
        for (int dy = -2; dy <= 2; dy++)
            for (int dx = -2; dx <= 2; dx++) {
                int px = (int)(x + dx), py = (int)(y + dy);
                if (px < 0 || py < 0 || px > 63 || py > 63) continue;
                float d = sqrtf((px + 0.5f - x) * (px + 0.5f - x) + (py + 0.5f - y) * (py + 0.5f - y));
                uint8_t a = (uint8_t)(fmaxf(0.f, 1.f - d / 2.2f) * 200);
                uint16_t &c = icon.rgb[py * 64 + px];
                c = mcBlend(d < 0.9f ? 0xDFFC : ST_TRAIL, c, a);
            }
    }
    const int st[4][2] = {{18, 18}, {47, 22}, {44, 47}, {24, 44}};
    for (auto &s : st)
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                uint16_t &c = icon.rgb[(s[1] + dy) * 64 + s[0] + dx];
                c = mcBlend(MC_WHITE, c, dx == 0 && dy == 0 ? 255 : (dx == 0 || dy == 0) ? 150 : 40);
            }
    return true;
}
