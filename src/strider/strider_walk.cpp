#include "strider_walk.h"
#include <math.h>
#include <string.h>

namespace strider {

static uint32_t mix32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

static uint8_t u8(float v) {
    if (v <= 0) return 0;
    if (v >= 1) return 255;
    return (uint8_t)(v * 255.0f + 0.5f);
}

static void pushSample(Walk &w) {
    if (!w.samples || w.cap < 4) return;
    if (++w.skip < w.keepEvery && w.n > 0) return;
    w.skip = 0;
    if (w.n >= w.cap) { // full: keep every other point, record half as often from now on
        int k = 0;
        for (int i = 0; i < w.n; i += 2) w.samples[k++] = w.samples[i];
        w.n = k;
        w.keepEvery *= 2;
    }
    Sample &s = w.samples[w.n++];
    s.x = w.x;
    s.y = w.y;
    s.t = w.t;
    s.sound = u8(w.envSound);
    s.rough = u8(w.envRough);
    s.rf = u8(w.envRf);
    s.pace = u8(w.envPace);
}

void walkBegin(Walk &w, Sample *storage, int cap, Mode mode, float startTod) {
    int y = w.year, mo = w.month, d = w.day, h = w.hour, mi = w.minute; // date is set by the caller first
    memset((void *)&w, 0, sizeof(w)); // (no big temporary on the stack)
    w.keepEvery = 1;
    w.year = y, w.month = mo, w.day = d, w.hour = h, w.minute = mi;
    w.mode = mode;
    w.samples = storage;
    w.cap = cap;
    w.todStart = w.todEnd = startTod;
    pushSample(w); // the starting point
}

void walkSetEnv(Walk &w, float sound, float rough, float rf, float pace) {
    w.envSound = sound;
    w.envRough = rough;
    w.envRf = rf;
    w.envPace = pace;
    w.soundSum += sound;
    w.roughSum += rough;
    w.envCount++;
    if (sound > w.soundPeak) w.soundPeak = sound;
}

void walkAdvance(Walk &w, float metres, float heading) {
    if (metres <= 0) return;
    w.heading = heading;
    w.x += cosf(heading) * metres;
    w.y += sinf(heading) * metres;
    w.distance += metres;
    pushSample(w);
}

void walkTick(Walk &w, float t, bool moving) {
    w.t = t;
    if (!moving) {
        if (!w.still) {
            w.still = true;
            w.stillSince = t;
        }
        return;
    }
    if (w.still) {
        w.still = false;
        float dur = t - w.stillSince;
        if (dur >= STOP_MIN_SEC && w.nStops < STOP_MAX) w.stops[w.nStops++] = {w.x, w.y, w.stillSince, dur};
    }
}

bool walkAddFind(Walk &w, uint8_t kind, uint32_t id, int rssi) {
    if (kind == FIND_WIFI)
        for (int i = 0; i < w.nFinds; i++)
            if (w.finds[i].kind == FIND_WIFI && w.finds[i].id == id) return false;
    if (kind == FIND_WIFI) {
        w.rfSeen++;
        w.rfHash ^= mix32(id + (uint32_t)(rssi + 128) * 0x9E3779B1u);
    }
    if (w.nFinds >= FIND_MAX) return kind == FIND_WIFI; // counted in the fingerprint, no room to draw it
    Find &f = w.finds[w.nFinds++];
    f.x = w.x;
    f.y = w.y;
    f.t = w.t;
    f.id = id;
    f.rssi = (int8_t)(rssi < -127 ? -127 : rssi > 127 ? 127 : rssi);
    f.kind = kind;
    return true;
}

void walkEnd(Walk &w, float endTod) {
    walkTick(w, w.t, true); // close an open stop
    w.todEnd = endTod;
    if (w.n == 0 || w.samples[w.n - 1].x != w.x || w.samples[w.n - 1].y != w.y) {
        w.skip = w.keepEvery; // always keep the final point
        pushSample(w);
    }
}

uint32_t walkSeed(const Walk &w) {
    uint32_t h = 0xC0FFEEu ^ (uint32_t)w.mode;
    int step = w.n > 96 ? w.n / 96 : 1;
    for (int i = 0; i < w.n; i += step) {
        int xi = (int)floorf(w.samples[i].x * 2.0f), yi = (int)floorf(w.samples[i].y * 2.0f);
        h = mix32(h ^ (uint32_t)xi * 0x9E3779B1u ^ (uint32_t)yi * 0x85EBCA77u ^ w.samples[i].sound);
    }
    h = mix32(h ^ w.steps * 0x27d4eb2fu ^ (uint32_t)(w.distance * 10));
    h ^= mix32(w.rfHash + (uint32_t)w.nStops);
    return h ? h : 1;
}

} // namespace strider
