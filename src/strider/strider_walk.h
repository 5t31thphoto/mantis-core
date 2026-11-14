// Mantis Strider: the walk recorder. Plain C++ (no Arduino), so the art can be previewed on a PC.
//
// A walk is a dead-reckoned pen trace (no GPS, no compass): every step (or, when riding, every
// metre of estimated travel) moves the pen along the current heading. Each recorded point also
// carries what the environment felt like there: loudness, ground roughness, Wi-Fi density and pace.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace strider {

constexpr int ART_W = 320;
constexpr int ART_H = 240;

enum Mode : uint8_t { MODE_WALK = 0, MODE_RIDE = 1 };

struct Sample {
    float x, y;  // metres, dead reckoning (y up)
    float t;     // seconds since start
    uint8_t sound, rough, rf, pace; // 0..255 at this point
};

enum FindKind : uint8_t { FIND_WIFI = 0, FIND_SOUND = 1 };
struct Find {
    float x, y, t;
    uint32_t id;  // BSSID hash (Wi-Fi) or event hash (sound)
    int8_t rssi;  // dBm for Wi-Fi, loudness for sound
    uint8_t kind;
};

struct Stop {
    float x, y, t, dur; // where you stood still, and for how long (s)
};

constexpr int FIND_MAX = 160;
constexpr int STOP_MAX = 48;
constexpr float STOP_MIN_SEC = 12.0f;

struct Walk {
    Mode mode = MODE_WALK;
    // when: local time of day 0..1 at start/end; calendar date for the label (year 0 = unknown)
    float todStart = 0.5f, todEnd = 0.5f;
    int year = 0, month = 0, day = 0, hour = 0, minute = 0;

    // pen state
    float x = 0, y = 0, heading = 0, t = 0, distance = 0;
    uint32_t steps = 0;

    // recorded trace (storage is owned by the caller; thinned 2:1 whenever it fills up)
    Sample *samples = nullptr;
    int n = 0, cap = 0, keepEvery = 1, skip = 0;

    Find finds[FIND_MAX];
    int nFinds = 0;
    Stop stops[STOP_MAX];
    int nStops = 0;

    // environment right now, 0..1 (set by the sensors)
    float envSound = 0, envRough = 0, envRf = 0, envPace = 0;
    float soundSum = 0, soundPeak = 0, roughSum = 0;
    uint32_t envCount = 0;
    uint32_t rfSeen = 0, rfHash = 0;

    // stillness
    bool still = false;
    float stillSince = 0;
};

void walkBegin(Walk &w, Sample *storage, int cap, Mode mode, float startTod);
void walkSetEnv(Walk &w, float sound, float rough, float rf, float pace); // 0..1 each
void walkAdvance(Walk &w, float metres, float heading);                   // move the pen
void walkTick(Walk &w, float t, bool moving);                             // time + stillness
bool walkAddFind(Walk &w, uint8_t kind, uint32_t id, int rssi);           // false if a repeat
void walkEnd(Walk &w, float endTod);
uint32_t walkSeed(const Walk &w); // fingerprint of route + places: drives every random choice in the art

} // namespace strider
