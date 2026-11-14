// Mantis Strider print renderer.
//
// Every print is built from the same walk data, read four ways:
//   route shape (dead reckoning), loudness and ground roughness along it, Wi-Fi networks found,
//   places you stood still, time of day at start and end, and a seed hashed from all of it.
// Nothing is random in the sense of "unrelated to the walk": the seed comes from the route and the
// networks, so the same loop on the same day gives sibling prints, and a different day differs.
//
// Pipeline: compose (rotate the route to its long axis, fit it to the frame, resample at ~1 px,
// smooth) -> style (fields, strokes, inks) -> 8-bit RGB working image -> ordered-dither to RGB565.
#include "strider_art.h"
#include <algorithm>
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace strider {
namespace {

constexpr int W = ART_W, H = ART_H, NPX = W * H;
constexpr int MAXP = 5000;   // resampled route points
constexpr int MAXRAW = 4400; // transformed raw samples (device keeps <= 4096)
constexpr int FLOW_W = 41, FLOW_H = 31;
constexpr float PI_F = 3.14159265f;

void (*g_yield)() = nullptr;
inline void yieldNow() {
    if (g_yield) g_yield();
}

struct Col {
    float r, g, b;
};
inline Col hex(uint32_t h) { return {((h >> 16) & 255) / 255.f, ((h >> 8) & 255) / 255.f, (h & 255) / 255.f}; }
inline float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
inline Col mixc(Col a, Col b, float t) { return {lerpf(a.r, b.r, t), lerpf(a.g, b.g, t), lerpf(a.b, b.b, t)}; }
inline Col scalec(Col a, float k) { return {a.r * k, a.g * k, a.b * k}; }
inline float smoothstep(float a, float b, float x) {
    float t = clamp01((x - a) / (b - a));
    return t * t * (3 - 2 * t);
}

// route point in screen space; u = 0..1 along the walk's time
struct PP {
    float x, y, u;
    float sound, rough, rf, pace;
    float tx, ty;
};
// finds and stops in screen space
enum MarkKind : uint8_t { MK_WIFI, MK_SOUND, MK_STOP, MK_TURN };
struct Mark {
    float x, y, v, u;
    uint32_t id;
    uint8_t kind;
};

struct Mem {
    uint8_t img[NPX * 3];
    float fa[NPX];
    float fb[NPX];
    PP pts[MAXP];
    PP raw[MAXRAW];
    Mark marks[FIND_MAX + STOP_MAX + 16];
    float flow[FLOW_W * FLOW_H * 2];
    float hist[2048];
    // star chart scratch
    int sIdx[FIND_MAX + STOP_MAX + 16], sFrom[FIND_MAX + STOP_MAX + 16];
    float sBest[FIND_MAX + STOP_MAX + 16];
    bool sIn[FIND_MAX + STOP_MAX + 16];
};

// ------------------------------------------------------------------ hashing / noise
inline uint32_t hashu(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
inline float hashf(uint32_t x) { return (hashu(x) & 0xFFFFFF) / 16777215.f; }

struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed ? seed : 0x1234567u) {}
    uint32_t u() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    float f() { return (u() >> 8) * (1.f / 16777216.f); }
    float range(float a, float b) { return a + (b - a) * f(); }
};

float vnoise(float x, float y, uint32_t seed) {
    int ix = (int)floorf(x), iy = (int)floorf(y);
    float fx = x - ix, fy = y - iy;
    auto g = [&](int i, int j) { return hashf(seed ^ hashu((uint32_t)(i * 374761393) ^ (uint32_t)(j * 668265263))); };
    float a = g(ix, iy), b = g(ix + 1, iy), c = g(ix, iy + 1), d = g(ix + 1, iy + 1);
    float ux = fx * fx * (3 - 2 * fx), uy = fy * fy * (3 - 2 * fy);
    return a + (b - a) * ux + (c - a) * uy + (a - b - c + d) * ux * uy;
}
float fbm(float x, float y, uint32_t seed, int oct) {
    float a = 0, amp = 0.5f, fr = 1, norm = 0;
    for (int i = 0; i < oct; i++) {
        a += amp * vnoise(x * fr, y * fr, seed + (uint32_t)i * 0x9E37u);
        norm += amp;
        amp *= 0.5f;
        fr *= 2.03f;
    }
    return a / norm;
}

// ------------------------------------------------------------------ time of day
enum Tod { TOD_NIGHT, TOD_DAWN, TOD_DAY, TOD_DUSK };
int todClass(float tod) {
    float h = tod * 24.f;
    if (h < 5.5f || h >= 21.f) return TOD_NIGHT;
    if (h < 8.5f) return TOD_DAWN;
    if (h < 17.5f) return TOD_DAY;
    return TOD_DUSK;
}

// ------------------------------------------------------------------ 3x5 font
// bits: row-major, 5 columns per row; returns the glyph width (0 = space/unknown -> 3)
int glyph(char c, uint32_t &bits) {
    static const char *const rows[][6] = {
        {"A", ".#.", "#.#", "###", "#.#", "#.#"}, {"B", "##.", "#.#", "##.", "#.#", "##."},
        {"C", ".##", "#..", "#..", "#..", ".##"}, {"D", "##.", "#.#", "#.#", "#.#", "##."},
        {"E", "###", "#..", "##.", "#..", "###"}, {"F", "###", "#..", "##.", "#..", "#.."},
        {"G", ".##", "#..", "#.#", "#.#", ".##"}, {"H", "#.#", "#.#", "###", "#.#", "#.#"},
        {"I", "###", ".#.", ".#.", ".#.", "###"}, {"J", "..#", "..#", "..#", "#.#", ".#."},
        {"K", "#.#", "#.#", "##.", "#.#", "#.#"}, {"L", "#..", "#..", "#..", "#..", "###"},
        {"M", "#...#", "##.##", "#.#.#", "#...#", "#...#"}, {"N", "#..#", "##.#", "#.##", "#..#", "#..#"},
        {"O", ".#.", "#.#", "#.#", "#.#", ".#."}, {"P", "##.", "#.#", "##.", "#..", "#.."},
        {"Q", ".#.", "#.#", "#.#", "##.", ".##"}, {"R", "##.", "#.#", "##.", "#.#", "#.#"},
        {"S", ".##", "#..", ".#.", "..#", "##."}, {"T", "###", ".#.", ".#.", ".#.", ".#."},
        {"U", "#.#", "#.#", "#.#", "#.#", "###"}, {"V", "#.#", "#.#", "#.#", "#.#", ".#."},
        {"W", "#...#", "#...#", "#.#.#", "##.##", "#...#"}, {"X", "#.#", "#.#", ".#.", "#.#", "#.#"},
        {"Y", "#.#", "#.#", ".#.", ".#.", ".#."}, {"Z", "###", "..#", ".#.", "#..", "###"},
        {"0", "###", "#.#", "#.#", "#.#", "###"}, {"1", ".#.", "##.", ".#.", ".#.", "###"},
        {"2", "##.", "..#", ".#.", "#..", "###"}, {"3", "##.", "..#", ".#.", "..#", "##."},
        {"4", "#.#", "#.#", "###", "..#", "..#"}, {"5", "###", "#..", "##.", "..#", "##."},
        {"6", ".##", "#..", "###", "#.#", "###"}, {"7", "###", "..#", ".#.", ".#.", ".#."},
        {"8", "###", "#.#", "###", "#.#", "###"}, {"9", "###", "#.#", "###", "..#", "##."},
        {".", "...", "...", "...", "...", ".#."}, {",", "...", "...", "...", ".#.", "#.."},
        {":", "...", ".#.", "...", ".#.", "..."}, {"-", "...", "...", "###", "...", "..."},
        {"/", "..#", "..#", ".#.", "#..", "#.."}, {"#", "#.#", "###", "#.#", "###", "#.#"},
        {"*", "...", "...", ".#.", "...", "..."}, {"%", "#.#", "..#", ".#.", "#..", "#.#"},
        {"?", "##.", "..#", ".#.", "...", ".#."}, {"'", ".#.", ".#.", "...", "...", "..."},
        {"+", "...", ".#.", "###", ".#.", "..."}, {"(", ".#.", "#..", "#..", "#..", ".#."},
        {")", ".#.", "..#", "..#", "..#", ".#."}, {"^", ".#.", "#.#", "...", "...", "..."},
        {"_", "...", "...", "...", "...", "###"}, {"=", "...", "###", "...", "###", "..."},
    };
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    for (auto &g : rows)
        if (g[0][0] == c) {
            uint32_t b = 0;
            int wd = (int)strlen(g[1]);
            for (int r = 0; r < 5; r++)
                for (int k = 0; k < wd; k++)
                    if (g[1 + r][k] == '#') b |= 1u << (r * 5 + k);
            bits = b;
            return wd;
        }
    bits = 0;
    return 3; // space and anything unknown
}
int textWidth(const char *s, int scale) {
    int w = 0;
    uint32_t b;
    for (; *s; s++) w += glyph(*s, b) + 1;
    return w ? (w - 1) * scale : 0;
}
template <typename Plot> void drawText(int x, int y, const char *s, int scale, Plot plot) {
    for (; *s; s++) {
        uint32_t b;
        int wd = glyph(*s, b);
        for (int r = 0; r < 5; r++)
            for (int k = 0; k < wd; k++)
                if (b & (1u << (r * 5 + k)))
                    for (int dy = 0; dy < scale; dy++)
                        for (int dx = 0; dx < scale; dx++) plot(x + k * scale + dx, y + r * scale + dy);
        x += (wd + 1) * scale;
    }
}

// ------------------------------------------------------------------ 8-bit RGB working image
enum Blend { NORMAL, MULTIPLY, ADD, SCREEN };

struct Img {
    uint8_t *p;
    void fill(Col c) {
        uint8_t r = (uint8_t)(clamp01(c.r) * 255 + .5f), g = (uint8_t)(clamp01(c.g) * 255 + .5f),
                b = (uint8_t)(clamp01(c.b) * 255 + .5f);
        for (int i = 0; i < NPX; i++) p[i * 3] = r, p[i * 3 + 1] = g, p[i * 3 + 2] = b;
    }
    Col get(int i) const { return {p[i * 3] / 255.f, p[i * 3 + 1] / 255.f, p[i * 3 + 2] / 255.f}; }
    void set(int i, Col c) {
        p[i * 3] = (uint8_t)(clamp01(c.r) * 255 + .5f);
        p[i * 3 + 1] = (uint8_t)(clamp01(c.g) * 255 + .5f);
        p[i * 3 + 2] = (uint8_t)(clamp01(c.b) * 255 + .5f);
    }
    void put(int x, int y, Col c, float a, Blend m = NORMAL) {
        if ((unsigned)x >= (unsigned)W || (unsigned)y >= (unsigned)H || a <= 0.002f) return;
        if (a > 1) a = 1;
        int i = y * W + x;
        Col d = get(i);
        Col o;
        switch (m) {
            case MULTIPLY: o = {d.r * (1 - a * (1 - c.r)), d.g * (1 - a * (1 - c.g)), d.b * (1 - a * (1 - c.b))}; break;
            case ADD: o = {d.r + c.r * a, d.g + c.g * a, d.b + c.b * a}; break;
            case SCREEN:
                o = {1 - (1 - d.r) * (1 - c.r * a), 1 - (1 - d.g) * (1 - c.g * a), 1 - (1 - d.b) * (1 - c.b * a)};
                break;
            default: o = mixc(d, c, a);
        }
        set(i, o);
    }
    void disc(float cx, float cy, float r, Col c, float a, Blend m = NORMAL) {
        int x0 = (int)floorf(cx - r - 1), x1 = (int)ceilf(cx + r + 1), y0 = (int)floorf(cy - r - 1),
            y1 = (int)ceilf(cy + r + 1);
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++) {
                float d = sqrtf((x - cx) * (x - cx) + (y - cy) * (y - cy));
                put(x, y, c, a * clamp01(r + 0.5f - d), m);
            }
    }
    void ring(float cx, float cy, float r, float wdt, Col c, float a, Blend m = NORMAL) {
        int x0 = (int)floorf(cx - r - wdt - 1), x1 = (int)ceilf(cx + r + wdt + 1), y0 = (int)floorf(cy - r - wdt - 1),
            y1 = (int)ceilf(cy + r + wdt + 1);
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++) {
                float d = fabsf(sqrtf((x - cx) * (x - cx) + (y - cy) * (y - cy)) - r);
                put(x, y, c, a * clamp01(wdt * 0.5f + 0.5f - d), m);
            }
    }
    void line(float x0, float y0, float x1, float y1, float r, Col c, float a, Blend m = NORMAL) {
        int bx0 = (int)floorf(fminf(x0, x1) - r - 1), bx1 = (int)ceilf(fmaxf(x0, x1) + r + 1);
        int by0 = (int)floorf(fminf(y0, y1) - r - 1), by1 = (int)ceilf(fmaxf(y0, y1) + r + 1);
        float dx = x1 - x0, dy = y1 - y0, l2 = dx * dx + dy * dy;
        for (int y = by0; y <= by1; y++)
            for (int x = bx0; x <= bx1; x++) {
                float t = l2 > 0 ? clamp01(((x - x0) * dx + (y - y0) * dy) / l2) : 0;
                float ex = x - (x0 + dx * t), ey = y - (y0 + dy * t);
                put(x, y, c, a * clamp01(r + 0.5f - sqrtf(ex * ex + ey * ey)), m);
            }
    }
    void text(int x, int y, const char *s, int scale, Col c, float a = 1, Blend m = NORMAL) {
        drawText(x, y, s, scale, [&](int px, int py) { put(px, py, c, a, m); });
    }
};

// ------------------------------------------------------------------ float fields
void fclear(float *f) { memset(f, 0, sizeof(float) * NPX); }
inline void splat(float *f, float x, float y, float w) {
    int ix = (int)floorf(x), iy = (int)floorf(y);
    float fx = x - ix, fy = y - iy;
    if (ix < 0 || iy < 0 || ix >= W - 1 || iy >= H - 1) return;
    float *p = f + iy * W + ix;
    p[0] += w * (1 - fx) * (1 - fy);
    p[1] += w * fx * (1 - fy);
    p[W] += w * (1 - fx) * fy;
    p[W + 1] += w * fx * fy;
}
inline float sample(const float *f, float x, float y) {
    int ix = (int)floorf(x), iy = (int)floorf(y);
    if (ix < 0) ix = 0, x = 0;
    if (iy < 0) iy = 0, y = 0;
    if (ix >= W - 1) ix = W - 2, x = (float)(W - 1);
    if (iy >= H - 1) iy = H - 2, y = (float)(H - 1);
    float fx = x - ix, fy = y - iy;
    const float *p = f + iy * W + ix;
    return (p[0] * (1 - fx) + p[1] * fx) * (1 - fy) + (p[W] * (1 - fx) + p[W + 1] * fx) * fy;
}
// three box passes ~ gaussian (sigma ~ r * 1.0); edges clamp
void boxLine(float *d, int n, int stride, int r, float *tmp) {
    for (int i = 0; i < n; i++) tmp[i] = d[i * stride];
    float acc = 0;
    int w = 2 * r + 1;
    for (int i = -r; i <= r; i++) acc += tmp[i < 0 ? 0 : i >= n ? n - 1 : i];
    for (int i = 0; i < n; i++) {
        d[i * stride] = acc / w;
        int add = i + r + 1, sub = i - r;
        acc += tmp[add >= n ? n - 1 : add] - tmp[sub < 0 ? 0 : sub];
    }
}
void blur(float *f, int r) {
    if (r < 1) return;
    float tmp[W > H ? W : H];
    for (int pass = 0; pass < 3; pass++) {
        for (int y = 0; y < H; y++) boxLine(f + y * W, W, 1, r, tmp);
        for (int x = 0; x < W; x++) boxLine(f + x, H, W, r, tmp);
        yieldNow();
    }
}
float fmax(const float *f) {
    float m = 0;
    for (int i = 0; i < NPX; i++)
        if (f[i] > m) m = f[i];
    return m;
}
void fscale(float *f, float k) {
    for (int i = 0; i < NPX; i++) f[i] *= k;
}
// stroke coverage into a mask (max), so overlapping segments never double up
void strokeMask(float *mask, float x0, float y0, float x1, float y1, float r) {
    int bx0 = (int)floorf(fminf(x0, x1) - r - 1), bx1 = (int)ceilf(fmaxf(x0, x1) + r + 1);
    int by0 = (int)floorf(fminf(y0, y1) - r - 1), by1 = (int)ceilf(fmaxf(y0, y1) + r + 1);
    if (bx0 < 0) bx0 = 0;
    if (by0 < 0) by0 = 0;
    if (bx1 > W - 1) bx1 = W - 1;
    if (by1 > H - 1) by1 = H - 1;
    float dx = x1 - x0, dy = y1 - y0, l2 = dx * dx + dy * dy;
    for (int y = by0; y <= by1; y++)
        for (int x = bx0; x <= bx1; x++) {
            float t = l2 > 0 ? clamp01(((x - x0) * dx + (y - y0) * dy) / l2) : 0;
            float ex = x - (x0 + dx * t), ey = y - (y0 + dy * t);
            float c = clamp01(r + 0.5f - sqrtf(ex * ex + ey * ey));
            float &m = mask[y * W + x];
            if (c > m) m = c;
        }
}
void discMask(float *mask, float cx, float cy, float r, float k = 1) {
    int x0 = (int)floorf(cx - r - 1), x1 = (int)ceilf(cx + r + 1), y0 = (int)floorf(cy - r - 1), y1 = (int)ceilf(cy + r + 1);
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            if ((unsigned)x >= (unsigned)W || (unsigned)y >= (unsigned)H) continue;
            float c = k * clamp01(r + 0.5f - sqrtf((x - cx) * (x - cx) + (y - cy) * (y - cy)));
            float &m = mask[y * W + x];
            if (c > m) m = c;
        }
}
void ringMask(float *mask, float cx, float cy, float r, float wdt) {
    int x0 = (int)floorf(cx - r - wdt - 1), x1 = (int)ceilf(cx + r + wdt + 1), y0 = (int)floorf(cy - r - wdt - 1),
        y1 = (int)ceilf(cy + r + wdt + 1);
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            if ((unsigned)x >= (unsigned)W || (unsigned)y >= (unsigned)H) continue;
            float d = fabsf(sqrtf((x - cx) * (x - cx) + (y - cy) * (y - cy)) - r);
            float c = clamp01(wdt * 0.5f + 0.5f - d);
            float &m = mask[y * W + x];
            if (c > m) m = c;
        }
}
void composite(Img &img, const float *mask, Col c, float a, Blend m, int ox = 0, int oy = 0) {
    yieldNow();
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int sx = x - ox, sy = y - oy;
            if ((unsigned)sx >= (unsigned)W || (unsigned)sy >= (unsigned)H) continue;
            float v = mask[sy * W + sx];
            if (v > 0.002f) img.put(x, y, c, a * v, m);
        }
}

// ------------------------------------------------------------------ words
const char *const MONTHS[12] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
void fmtDate(const Walk &w, char *b, size_t n) {
    if (w.year < 2000 || w.month < 1 || w.month > 12) {
        snprintf(b, n, "UNDATED");
        return;
    }
    snprintf(b, n, "%d %s %d  %02d:%02d", w.day, MONTHS[w.month - 1], w.year, w.hour, w.minute);
}
void fmtDist(float m, char *b, size_t n) {
    if (m < 1000) snprintf(b, n, "%d M", (int)(m + 0.5f));
    else if (m < 10000) snprintf(b, n, "%.1f KM", m / 1000.f);
    else snprintf(b, n, "%d KM", (int)(m / 1000.f + 0.5f));
}
void fmtDur(float s, char *b, size_t n) {
    int mins = (int)(s / 60.f + 0.5f);
    if (mins < 60) snprintf(b, n, "%d MIN", mins);
    else snprintf(b, n, "%d H %02d", mins / 60, mins % 60);
}
void fmtThousands(uint32_t v, char *b, size_t n) {
    if (v >= 1000000) snprintf(b, n, "%u,%03u,%03u", (unsigned)(v / 1000000), (unsigned)(v / 1000 % 1000), (unsigned)(v % 1000));
    else if (v >= 1000) snprintf(b, n, "%u,%03u", (unsigned)(v / 1000), (unsigned)(v % 1000));
    else snprintf(b, n, "%u", (unsigned)v);
}
// "3.2 KM  4,120 STEPS  48 MIN" (walk) / "18 KM RIDE  1 H 05" (ride)
void fmtStats(const Walk &w, char *b, size_t n) {
    char d[16], t[16], s[16];
    fmtDist(w.distance, d, sizeof d);
    fmtDur(w.t, t, sizeof t);
    if (w.mode == MODE_RIDE) snprintf(b, n, "%s RIDE  %s", d, t);
    else {
        fmtThousands(w.steps, s, sizeof s);
        snprintf(b, n, "%s  %s STEPS  %s", d, s, t);
    }
}
// invented place names, from the walk's seed
void makeName(uint32_t seed, char *b, size_t n) {
    static const char *const A[] = {"AL", "BER", "CAL", "DOR", "EL", "FAR", "GAL", "HAL", "IR", "KAS", "LYR", "MAR",
                                    "NOR", "OR", "PEL", "RAS", "SEL", "TAL", "UL", "VEL", "ZAR", "AS", "EN", "MOR"};
    static const char *const B[] = {"A", "E", "I", "O", "U", "IA", "EO", "AU"};
    static const char *const C[] = {"RIS", "LON", "THA", "NIS", "DRA", "VEN", "MIR", "TUS", "LIS", "RAN", "XA", "NE"};
    uint32_t h = hashu(seed ^ 0xA5A5u);
    snprintf(b, n, "%s%s%s", A[h % 24], B[(h >> 8) % 8], C[(h >> 12) % 12]);
}

// ------------------------------------------------------------------ composition
struct Comp {
    PP *p = nullptr;
    int n = 0;
    Mark *m = nullptr;
    int nm = 0;
    float pxPerM = 1;
    float avgSound = 0, avgRough = 0;
    uint32_t seed = 1;
};

// Rotate the route onto its long axis (start on the left), fit it into box (bw x bh) centred at
// (cx, cy), resample at ~1 px and smooth. minExtentM keeps a short stroll from being blown up.
void compose(Mem &m, const Walk &w, Comp &c, float cx, float cy, float bw, float bh, float minExtentM) {
    c.seed = walkSeed(w);
    c.p = m.pts;
    c.m = m.marks;
    // raw points (a walk with no movement becomes a small circle: a print of standing still)
    int nr = 0;
    float totalT = w.t > 1 ? w.t : 1;
    if (w.n >= 2) {
        int stepIn = 1 + w.n / MAXRAW;
        for (int i = 0; i < w.n && nr < MAXRAW; i += stepIn) {
            const Sample &s = w.samples[i];
            PP &q = m.raw[nr++];
            q.x = s.x, q.y = s.y, q.u = clamp01(s.t / totalT);
            q.sound = s.sound / 255.f, q.rough = s.rough / 255.f, q.rf = s.rf / 255.f, q.pace = s.pace / 255.f;
        }
    }
    if (nr < 2) {
        nr = 0;
        float snd = w.envCount ? w.soundSum / w.envCount : 0.3f;
        for (int i = 0; i <= 48; i++) {
            PP &q = m.raw[nr++];
            float a = i * 2 * PI_F / 48;
            q.x = cosf(a) * 1.5f, q.y = sinf(a) * 1.5f, q.u = i / 48.f;
            q.sound = snd, q.rough = 0, q.rf = 0, q.pace = 0;
        }
    }
    // principal axis
    double mx = 0, my = 0;
    for (int i = 0; i < nr; i++) mx += m.raw[i].x, my += m.raw[i].y;
    mx /= nr, my /= nr;
    double sxx = 0, syy = 0, sxy = 0;
    for (int i = 0; i < nr; i++) {
        double dx = m.raw[i].x - mx, dy = m.raw[i].y - my;
        sxx += dx * dx, syy += dy * dy, sxy += dx * dy;
    }
    float th = 0.5f * atan2f((float)(2 * sxy), (float)(sxx - syy));
    float ca = cosf(-th), sa = sinf(-th);
    auto rot = [&](float x, float y, float &ox, float &oy) {
        float dx = x - (float)mx, dy = y - (float)my;
        ox = dx * ca - dy * sa;
        oy = dx * sa + dy * ca;
    };
    float sx0, sy0;
    rot(m.raw[0].x, m.raw[0].y, sx0, sy0);
    if (sx0 > 0) ca = -ca, sa = -sa; // half turn so the walk starts on the left (keeps left/right turns)
    float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
    for (int i = 0; i < nr; i++) {
        float x, y;
        rot(m.raw[i].x, m.raw[i].y, x, y);
        m.raw[i].x = x, m.raw[i].y = y;
        minx = fminf(minx, x), maxx = fmaxf(maxx, x), miny = fminf(miny, y), maxy = fmaxf(maxy, y);
    }
    float ex = fmaxf(maxx - minx, 1e-3f), ey = fmaxf(maxy - miny, 1e-3f);
    float scale = fminf(bw / fmaxf(ex, minExtentM * bw / fmaxf(bw, bh)), bh / fmaxf(ey, minExtentM * bh / fmaxf(bw, bh)));
    c.pxPerM = scale;
    float ox = (minx + maxx) * 0.5f, oy = (miny + maxy) * 0.5f;
    // to screen (y up -> y down)
    for (int i = 0; i < nr; i++) {
        m.raw[i].x = cx + (m.raw[i].x - ox) * scale;
        m.raw[i].y = cy - (m.raw[i].y - oy) * scale;
    }
    auto toScreen = [&](float x, float y, float &px, float &py) {
        float rx, ry;
        rot(x, y, rx, ry);
        px = cx + (rx - ox) * scale;
        py = cy - (ry - oy) * scale;
    };
    // resample along arc length
    float len = 0;
    for (int i = 1; i < nr; i++) len += hypotf(m.raw[i].x - m.raw[i - 1].x, m.raw[i].y - m.raw[i - 1].y);
    float sp = fmaxf(0.8f, len / (MAXP - 2));
    int n = 0;
    m.pts[n++] = m.raw[0];
    float carry = 0;
    for (int i = 1; i < nr && n < MAXP - 1; i++) {
        const PP &a = m.raw[i - 1], &b = m.raw[i];
        float seg = hypotf(b.x - a.x, b.y - a.y);
        float d = sp - carry;
        while (d <= seg && n < MAXP - 1) {
            float t = d / seg;
            PP q;
            q.x = lerpf(a.x, b.x, t), q.y = lerpf(a.y, b.y, t), q.u = lerpf(a.u, b.u, t);
            q.sound = lerpf(a.sound, b.sound, t), q.rough = lerpf(a.rough, b.rough, t);
            q.rf = lerpf(a.rf, b.rf, t), q.pace = lerpf(a.pace, b.pace, t);
            m.pts[n++] = q;
            d += sp;
        }
        carry = seg - (d - sp);
    }
    m.pts[n++] = m.raw[nr - 1];
    // smooth positions (+-2 px) and attributes (+-8 px): dead reckoning is jittery, ink isn't
    for (int i = 0; i < n; i++) m.raw[i % MAXRAW] = m.pts[i];
    if (n <= MAXRAW) {
        int kp = (int)fmaxf(1, 2.0f / sp), ka = (int)fmaxf(2, 8.0f / sp);
        for (int i = 0; i < n; i++) {
            float x = 0, y = 0, cnt = 0;
            for (int j = i - kp; j <= i + kp; j++) {
                int k = j < 0 ? 0 : j >= n ? n - 1 : j;
                x += m.raw[k].x, y += m.raw[k].y, cnt++;
            }
            if (i > 0 && i < n - 1) m.pts[i].x = x / cnt, m.pts[i].y = y / cnt;
            float s = 0, r = 0, f = 0, pc = 0;
            cnt = 0;
            for (int j = i - ka; j <= i + ka; j++) {
                int k = j < 0 ? 0 : j >= n ? n - 1 : j;
                s += m.raw[k].sound, r += m.raw[k].rough, f += m.raw[k].rf, pc += m.raw[k].pace, cnt++;
            }
            m.pts[i].sound = s / cnt, m.pts[i].rough = r / cnt, m.pts[i].rf = f / cnt, m.pts[i].pace = pc / cnt;
        }
    }
    // tangents, averages
    float ss = 0, rs = 0;
    for (int i = 0; i < n; i++) {
        int a = i > 0 ? i - 1 : i, b = i < n - 1 ? i + 1 : i;
        float dx = m.pts[b].x - m.pts[a].x, dy = m.pts[b].y - m.pts[a].y, l = hypotf(dx, dy);
        m.pts[i].tx = l > 0 ? dx / l : 1, m.pts[i].ty = l > 0 ? dy / l : 0;
        ss += m.pts[i].sound, rs += m.pts[i].rough;
    }
    c.n = n;
    c.avgSound = ss / n;
    c.avgRough = rs / n;
    // marks
    int nm = 0;
    for (int i = 0; i < w.nFinds; i++) {
        const Find &f = w.finds[i];
        Mark &k = m.marks[nm++];
        toScreen(f.x, f.y, k.x, k.y);
        k.u = clamp01(f.t / totalT);
        k.id = f.id;
        if (f.kind == FIND_WIFI) {
            k.kind = MK_WIFI;
            k.v = clamp01((f.rssi + 95) / 60.f);
        } else {
            k.kind = MK_SOUND;
            k.v = clamp01(f.rssi / 127.f);
        }
        // spread finds made at the same spot so they read as a cluster, not one blob
        float a = hashf(f.id) * 2 * PI_F, r = 1.5f + 5.f * hashf(f.id ^ 0x55u);
        k.x += cosf(a) * r, k.y += sinf(a) * r;
    }
    for (int i = 0; i < w.nStops; i++) {
        const Stop &s = w.stops[i];
        Mark &k = m.marks[nm++];
        toScreen(s.x, s.y, k.x, k.y);
        k.u = clamp01(s.t / totalT);
        k.id = (uint32_t)i;
        k.kind = MK_STOP;
        k.v = clamp01(0.25f + logf(s.dur / STOP_MIN_SEC) / logf(30.f));
    }
    c.nm = nm;
}

// sharpest turns along the route, as extra stars when few networks were found
int addTurnMarks(Mem &m, Comp &c, int want) {
    int added = 0;
    float sp = c.n > 1 ? hypotf(c.p[1].x - c.p[0].x, c.p[1].y - c.p[0].y) : 1;
    const int win = (int)fmaxf(3, 12.f / fmaxf(sp, 0.1f)); // compare headings ~12 px either side
    for (int pass = 0; pass < want; pass++) {
        float best = 0.30f;
        int bi = -1;
        for (int i = win; i < c.n - win; i += 2) {
            const PP &a = c.p[i - win], &b = c.p[i + win];
            float turn = 1 - (a.tx * b.tx + a.ty * b.ty); // 0 straight .. 2 reversal
            if (turn <= best) continue;
            bool near = false;
            for (int k = 0; k < c.nm; k++)
                if (fabsf(c.m[k].x - c.p[i].x) + fabsf(c.m[k].y - c.p[i].y) < 22) near = true;
            if (near) continue;
            best = turn, bi = i;
        }
        if (bi < 0 || c.nm >= FIND_MAX + STOP_MAX + 16) break;
        Mark &k = m.marks[c.nm++];
        k.x = c.p[bi].x, k.y = c.p[bi].y, k.u = c.p[bi].u, k.id = (uint32_t)bi * 2654435761u;
        k.kind = MK_TURN;
        k.v = clamp01(best / 2);
        added++;
    }
    return added;
}

float niceMetres(float target) {
    static const float N[] = {5, 10, 20, 25, 50, 100, 200, 250, 500, 1000, 2000, 2500, 5000, 10000, 20000, 50000};
    float best = N[0];
    for (float v : N)
        if (fabsf(logf(v / target)) < fabsf(logf(best / target))) best = v;
    return best;
}
void fmtScale(float m, char *b, size_t n) {
    if (m >= 1000) snprintf(b, n, "%g KM", m / 1000.f);
    else snprintf(b, n, "%g M", m);
}

// ================================================================== TOPO
// Loudness along the route raises land: quiet stretches are lowland, loud ones ridges, places
// with many Wi-Fi networks rise as hills, rough ground roughens the terrain. A contour map of
// an island that only exists because you walked it.
void styleTopo(Mem &m, const Walk &w, Comp &c) {
    compose(m, w, c, W * 0.5f, H * 0.47f, W - 70.f, H - 78.f, 30.f);
    Img img{m.img};
    int tc = todClass(w.todStart);
    struct Pal {
        uint32_t paper, waterDeep, waterShallow, ramp[7], contour, trail, mark;
    };
    static const Pal P[4] = {
        // night: blueprint
        {0x0E2036, 0x0A1828, 0x16304C, {0x1B3A5A, 0x1F4A6C, 0x24587C, 0x2B678C, 0x33779B, 0x3C87AA, 0x4A98B8}, 0xA6DCFF,
         0xFFC24B, 0xE8F4FF},
        // dawn
        {0xF3E7DC, 0xA9B9CC, 0xCBD6DE, {0xF1DDD0, 0xEAC9BC, 0xDDAFAA, 0xC99AA6, 0xAE88A6, 0x8F7AA2, 0x6E6C98}, 0x5E4766,
         0xD9452B, 0x3A2A44},
        // day: classic survey map
        {0xEFE7D0, 0x9CC3D2, 0xC6DFE2, {0xE6E2C2, 0xD3DFAF, 0xB4CF92, 0xCFC48A, 0xC9A36A, 0xB07D4E, 0x8E5A38}, 0x6E5034,
         0xC4262E, 0x2F2418},
        // dusk
        {0xF0DCC2, 0x8E9DB6, 0xB9C2CC, {0xEFCFA6, 0xE6B17F, 0xD68D62, 0xBA6A5C, 0x93525E, 0x6D415E, 0x48345A}, 0x4E2C3C,
         0x1F6F8F, 0x2A1A26},
    };
    const Pal &p = P[tc];
    Col paper = hex(p.paper);

    // ---- elevation: sharp ridge + broad massif from the route, Wi-Fi hills, rough texture
    float *E = m.fa, *T = m.fb;
    fclear(E);
    for (int i = 0; i < c.n; i++) splat(E, c.p[i].x, c.p[i].y, 0.25f + 0.9f * c.p[i].sound + 0.25f * c.p[i].rough);
    memcpy(T, E, sizeof(float) * NPX);
    blur(E, 6);
    blur(T, 18);
    float me = fmax(E), mt = fmax(T);
    for (int i = 0; i < NPX; i++) E[i] = 0.42f * E[i] / (me + 1e-6f) + 0.58f * T[i] / (mt + 1e-6f);
    fclear(T);
    int wifi = 0;
    for (int k = 0; k < c.nm; k++)
        if (c.m[k].kind == MK_WIFI) splat(T, c.m[k].x, c.m[k].y, 0.5f + c.m[k].v), wifi++;
    if (wifi) {
        blur(T, 7);
        float mw = fmax(T);
        for (int i = 0; i < NPX; i++) E[i] += 0.32f * T[i] / (mw + 1e-6f);
    }
    float mx = fmax(E);
    float tex = 0.10f + 0.22f * c.avgRough;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            float n = fbm(x * 0.035f, y * 0.035f, c.seed, 4) - 0.5f;
            E[y * W + x] = E[y * W + x] / (mx + 1e-6f) + n * tex;
        }
    const float sea = 0.13f;
    const int LV = 10;
    auto level = [&](int i) { return E[i] < sea ? -1 : (int)((E[i] - sea) / (1 - sea + 0.05f) * LV); };

    // ---- colour: hypsometric tints with hillshade, water with depth tint
    const int inset = 6;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int i = y * W + x;
            if (x < inset || y < inset || x >= W - inset || y >= H - inset) {
                img.set(i, paper);
                continue;
            }
            float e = E[i];
            Col col;
            if (e < sea) {
                col = mixc(hex(p.waterDeep), hex(p.waterShallow), smoothstep(-0.05f, sea, e));
                if (tc != TOD_NIGHT && ((x + y) % 7 == 0) && e < sea - 0.03f) col = mixc(col, hex(p.waterDeep), 0.25f);
            } else {
                float f = clamp01((e - sea) / (1 - sea) * 6.f);
                int a = (int)f;
                if (a > 5) a = 5;
                col = mixc(hex(p.ramp[a]), hex(p.ramp[a + 1]), f - a);
                int xl = x > 0 ? i - 1 : i, xr = x < W - 1 ? i + 1 : i, yu = y > 0 ? i - W : i, yd = y < H - 1 ? i + W : i;
                float gx = (E[xr] - E[xl]) * 18, gy = (E[yd] - E[yu]) * 18;
                float shade = clamp01(0.55f + (gx * 0.7f + gy * 0.7f) * -1.0f);
                col = scalec(col, 0.80f + 0.36f * shade);
            }
            img.set(i, col);
        }
    // ---- grid (survey squares)
    float gridM = niceMetres(70.f / c.pxPerM), gpx = gridM * c.pxPerM;
    Col contour = hex(p.contour);
    if (gpx > 18)
        for (float gx = fmodf(W * 0.5f, gpx); gx < W - inset; gx += gpx)
            for (int y = inset; y < H - inset; y++) img.put((int)gx, y, contour, 0.10f);
    if (gpx > 18)
        for (float gy = fmodf(H * 0.47f, gpx); gy < H - inset; gy += gpx)
            for (int x = inset; x < W - inset; x++) img.put(x, (int)gy, contour, 0.10f);
    // ---- contours: every level, index contour every 5th, shoreline strongest
    for (int y = inset; y < H - inset - 1; y++)
        for (int x = inset; x < W - inset - 1; x++) {
            int i = y * W + x, a = level(i), b = level(i + 1), d = level(i + W);
            if (a == b && a == d) continue;
            int hi = a > b ? a : b;
            hi = hi > d ? hi : d;
            float al = hi == 0 ? 0.9f : (hi % 5 == 0 ? 0.75f : 0.38f);
            img.put(x, y, contour, al);
        }
    // ---- trail: halo, then a dashed line
    Col trail = hex(p.trail), mark = hex(p.mark);
    fclear(T);
    for (int i = 1; i < c.n; i++) strokeMask(T, c.p[i - 1].x, c.p[i - 1].y, c.p[i].x, c.p[i].y, 1.6f);
    composite(img, T, paper, 0.55f, NORMAL);
    fclear(T);
    float acc = 0;
    for (int i = 1; i < c.n; i++) {
        float seg = hypotf(c.p[i].x - c.p[i - 1].x, c.p[i].y - c.p[i - 1].y);
        acc += seg;
        if (fmodf(acc, 7.f) < 4.5f) strokeMask(T, c.p[i - 1].x, c.p[i - 1].y, c.p[i].x, c.p[i].y, 0.75f);
    }
    composite(img, T, trail, 1.0f, NORMAL);
    // ---- marks: stops (rest dots), Wi-Fi (survey triangles), sound (asterisks)
    for (int k = 0; k < c.nm; k++) {
        const Mark &q = c.m[k];
        if (q.kind == MK_STOP) {
            img.disc(q.x, q.y, 2.6f + 2.5f * q.v, paper, 0.8f);
            img.disc(q.x, q.y, 1.4f + 1.8f * q.v, mark, 0.9f);
        } else if (q.kind == MK_WIFI) {
            float s = 2.2f + 1.6f * q.v;
            for (int yy = (int)(q.y - s - 1); yy <= (int)(q.y + s + 1); yy++)
                for (int xx = (int)(q.x - s - 1); xx <= (int)(q.x + s + 1); xx++) {
                    float cov = 0;
                    for (int sy = 0; sy < 2; sy++)
                        for (int sx = 0; sx < 2; sx++) {
                            float px = xx + 0.25f + sx * 0.5f - q.x, py = yy + 0.25f + sy * 0.5f - q.y;
                            if (py < s * 0.6f && py > -s && fabsf(px) < (py + s) * 0.62f) cov += 0.25f;
                        }
                    img.put(xx, yy, mark, cov * 0.85f);
                }
        } else if (q.kind == MK_SOUND) {
            for (int a = 0; a < 3; a++) {
                float an = a * PI_F / 3 + 0.3f, r = 2.5f + 2 * q.v;
                img.line(q.x - cosf(an) * r, q.y - sinf(an) * r, q.x + cosf(an) * r, q.y + sinf(an) * r, 0.35f, trail, 0.85f);
            }
        }
    }
    const PP &s0 = c.p[0], &s1 = c.p[c.n - 1];
    img.disc(s0.x, s0.y, 4.2f, paper, 0.9f);
    img.ring(s0.x, s0.y, 3.0f, 1.3f, trail, 1);
    img.disc(s1.x, s1.y, 5.2f, paper, 0.9f);
    img.ring(s1.x, s1.y, 4.2f, 1.0f, trail, 1);
    img.disc(s1.x, s1.y, 2.0f, trail, 1);

    // ---- neatline, cartouche, scale bar, a north arrow that admits it doesn't know
    for (int x = inset; x < W - inset; x++) img.put(x, inset, contour, 0.9f), img.put(x, H - inset - 1, contour, 0.9f);
    for (int y = inset; y < H - inset; y++) img.put(inset, y, contour, 0.9f), img.put(W - inset - 1, y, contour, 0.9f);
    for (int x = inset - 3; x < W - inset + 3; x++) img.put(x, inset - 3, contour, 0.45f), img.put(x, H - inset + 2, contour, 0.45f);
    for (int y = inset - 3; y < H - inset + 3; y++) img.put(inset - 3, y, contour, 0.45f), img.put(W - inset + 2, y, contour, 0.45f);

    char name[24], title[40], date[40], stats[48];
    makeName(c.seed, name, sizeof name);
    static const char *const FORM[] = {"ISLE OF %s", "%s RANGE", "MOUNT %s", "%s HIGHLANDS"};
    snprintf(title, sizeof title, FORM[(c.seed >> 3) % 4], name);
    fmtDate(w, date, sizeof date);
    fmtStats(w, stats, sizeof stats);
    int bw = std::max(textWidth(title, 2), std::max(textWidth(date, 1), textWidth(stats, 1))) + 12;
    int bx = inset + 6, by = H - inset - 6 - 34;
    for (int y = by; y < by + 34; y++)
        for (int x = bx; x < bx + bw; x++) img.put(x, y, paper, 0.9f);
    for (int x = bx; x < bx + bw; x++) img.put(x, by, contour, 0.9f), img.put(x, by + 33, contour, 0.9f);
    for (int y = by; y < by + 34; y++) img.put(bx, y, contour, 0.9f), img.put(bx + bw - 1, y, contour, 0.9f);
    img.text(bx + 6, by + 5, title, 2, mark);
    img.text(bx + 6, by + 18, date, 1, contour);
    img.text(bx + 6, by + 25, stats, 1, contour);
    // scale bar
    float sm = niceMetres(56.f / c.pxPerM), spx = sm * c.pxPerM;
    if (spx > 12 && spx < 140) {
        int sx = (int)(W - inset - 10 - spx), sy = H - inset - 16;
        for (int k = 0; k < 4; k++) {
            int a = sx + (int)(spx * k / 4), b = sx + (int)(spx * (k + 1) / 4);
            for (int x = a; x < b; x++)
                for (int y = sy; y < sy + 3; y++) img.put(x, y, k % 2 ? paper : mark, 1);
        }
        for (int x = sx; x < sx + (int)spx; x++) img.put(x, sy - 1, mark, 1), img.put(x, sy + 3, mark, 1);
        char sl[16];
        fmtScale(sm, sl, sizeof sl);
        img.text(sx + (int)spx - textWidth(sl, 1), sy + 6, sl, 1, mark);
        img.text(sx, sy + 6, "0", 1, mark);
    }
    int nx = W - inset - 16, ny = inset + 10;
    img.line(nx, ny + 14, nx, ny, 0.6f, mark, 1);
    img.line(nx, ny, nx - 3, ny + 5, 0.6f, mark, 1);
    img.line(nx, ny, nx + 3, ny + 5, 0.6f, mark, 1);
    img.text(nx - 3, ny + 17, "N?", 1, mark);
}

// ================================================================== RISO
// Two inks on warm paper. The route is a brush stroke (wider where you walked faster), the
// soundscape a halftone, Wi-Fi rings and resting dots on top, inks a little out of register so
// they overprint into a third colour. A sun or moon sits at the height it had when you set off.
void styleRiso(Mem &m, const Walk &w, Comp &c) {
    compose(m, w, c, W * 0.47f, H * 0.5f, W - 64.f, H - 70.f, 30.f);
    Img img{m.img};
    int tc = todClass(w.todStart);
    struct Inks {
        uint32_t paper, a, b;
    };
    static const Inks I[4][2] = {
        {{0xE7E3EE, 0xFF48B0, 0x3B3B8F}, {0xEDE6DE, 0x00A95C, 0x2E2A6B}}, // night: pink/midnight, green/midnight
        {{0xF6EEE4, 0xFFB511, 0x00838A}, {0xF6EEE4, 0xFF7477, 0x0078BF}}, // dawn: sunflower/teal, coral/blue
        {{0xF4F0E6, 0xFF48B0, 0x0078BF}, {0xF2EEE2, 0xFFE800, 0x0078BF}}, // day: pink/blue, yellow/blue
        {{0xF3ECE0, 0xFF6C2F, 0x3D5588}, {0xF3ECE0, 0xF15060, 0x5D3F8A}}, // dusk: orange/federal, red/violet
    };
    const Inks &ink = I[tc][(c.seed >> 5) & 1];
    float *A = m.fa, *B = m.fb;

    // ---- ink B: halftone of the soundscape (sound + Wi-Fi density around the route)
    fclear(A);
    for (int i = 0; i < c.n; i++) splat(A, c.p[i].x, c.p[i].y, 0.12f + c.p[i].sound + 0.35f * c.p[i].rf);
    blur(A, 15);
    float ma = fmax(A);
    fscale(A, 1.f / (ma + 1e-6f));
    fclear(B);
    const float cell = 6.0f, ang = 0.2618f; // 15 degrees
    float cs = cosf(ang), sn = sinf(ang);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            float u = x * cs + y * sn, v = -x * sn + y * cs;
            float cu = (floorf(u / cell) + 0.5f) * cell, cv = (floorf(v / cell) + 0.5f) * cell;
            float px = cu * cs - cv * sn, py = cu * sn + cv * cs;
            float s = sample(A, px, py);
            float r = cell * 0.62f * sqrtf(clamp01(s * 1.15f - 0.04f));
            float d = hypotf(u - cu, v - cv);
            B[y * W + x] = clamp01(r - d + 0.5f);
        }
    // key line of the route in ink B, and resting places as solid dots
    for (int i = 1; i < c.n; i++) strokeMask(B, c.p[i - 1].x, c.p[i - 1].y, c.p[i].x, c.p[i].y, 0.55f);
    for (int k = 0; k < c.nm; k++)
        if (c.m[k].kind == MK_STOP) discMask(B, c.m[k].x, c.m[k].y, 2.2f + 3.f * c.m[k].v);

    // ---- ink A: brush-stroke route, Wi-Fi rings, sun/moon
    fclear(A);
    for (int i = 1; i < c.n; i++) {
        const PP &q = c.p[i];
        float jitter = (hashf((uint32_t)i * 2654435761u ^ c.seed) - 0.5f) * q.rough * 1.6f;
        float r = 1.1f + 3.6f * q.pace + 0.8f * q.sound + jitter;
        strokeMask(A, c.p[i - 1].x, c.p[i - 1].y, q.x, q.y, r < 0.7f ? 0.7f : r);
    }
    for (int k = 0; k < c.nm; k++)
        if (c.m[k].kind == MK_WIFI) ringMask(A, c.m[k].x, c.m[k].y, 2.5f + 3.5f * c.m[k].v, 1.1f);
        else if (c.m[k].kind == MK_SOUND) discMask(A, c.m[k].x, c.m[k].y, 1.5f + 2.f * c.m[k].v);
    // sun height from the time of day: high at noon, on the horizon at 6 and 18, a moon at night
    float h = w.todStart * 24.f, alt = sinf((h - 6.f) / 12.f * PI_F);
    float sunX = W - 46.f, sunY = 34.f + (1 - fmaxf(alt, 0)) * 34.f, sunR = 17.f;
    if (alt > -0.05f) discMask(A, sunX, sunY, sunR, 0.95f);
    else {
        ringMask(B, sunX, 40.f, sunR, 2.2f);
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                float d1 = hypotf(x - sunX, y - 40.f), d2 = hypotf(x - sunX - 7.f, y - 36.f);
                if (d1 < sunR && d2 > sunR - 3.f) {
                    float &v = A[y * W + x];
                    v = fmaxf(v, clamp01(sunR + 0.5f - d1) * clamp01(d2 - (sunR - 3.f) + 0.5f));
                }
            }
    }
    // ---- labels in ink B
    char date[40], stats[48], ed[24];
    fmtDate(w, date, sizeof date);
    fmtStats(w, stats, sizeof stats);
    snprintf(ed, sizeof ed, "NO. %04X", (unsigned)(c.seed & 0xFFFF));
    drawText(14, H - 30, "STRIDER", 2, [&](int x, int y) {
        if ((unsigned)x < (unsigned)W && (unsigned)y < (unsigned)H) B[y * W + x] = 1;
    });
    drawText(14, H - 17, date, 1, [&](int x, int y) {
        if ((unsigned)x < (unsigned)W && (unsigned)y < (unsigned)H) B[y * W + x] = 1;
    });
    drawText(14, H - 10, stats, 1, [&](int x, int y) {
        if ((unsigned)x < (unsigned)W && (unsigned)y < (unsigned)H) B[y * W + x] = 1;
    });
    drawText(W - 14 - textWidth(ed, 1), H - 10, ed, 1, [&](int x, int y) {
        if ((unsigned)x < (unsigned)W && (unsigned)y < (unsigned)H) A[y * W + x] = 1;
    });

    // ---- print: grain, dropouts, misregistration, multiply overprint
    Col paper = hex(ink.paper), ca = hex(ink.a), cb = hex(ink.b);
    int oax = 2, oay = 1; // ink A plate is a little off
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int i = y * W + x;
            float g = fbm(x * 0.5f, y * 0.5f, c.seed ^ 0x77u, 2);
            float va = 0;
            int sx = x - oax, sy = y - oay;
            if ((unsigned)sx < (unsigned)W && (unsigned)sy < (unsigned)H) va = A[sy * W + sx];
            float vb = B[i];
            va *= 0.78f + 0.3f * g;
            vb *= 0.80f + 0.28f * fbm(x * 0.5f + 40, y * 0.5f, c.seed ^ 0x99u, 2);
            if (hashf((uint32_t)i ^ c.seed) < 0.018f) va *= 0.3f;
            if (hashf((uint32_t)i * 7u ^ c.seed) < 0.018f) vb *= 0.3f;
            va = clamp01(va), vb = clamp01(vb);
            Col o = paper;
            float pg = (hashf((uint32_t)i * 13u ^ c.seed) - 0.5f) * 0.025f; // paper tooth
            o = {o.r + pg, o.g + pg, o.b + pg};
            o = {o.r * (1 - va * (1 - ca.r)), o.g * (1 - va * (1 - ca.g)), o.b * (1 - va * (1 - ca.b))};
            o = {o.r * (1 - vb * (1 - cb.r)), o.g * (1 - vb * (1 - cb.g)), o.b * (1 - vb * (1 - cb.b))};
            img.set(i, o);
        }
}

// ================================================================== LIGHT
// A long exposure: light leaks off the route and drifts through a flow field seeded by the walk.
// Louder places shed more, brighter light; Wi-Fi networks flare as stars; resting places glow.
// Colour runs from the palette of the hour you left to the palette of the hour you got back.
struct Ramp {
    uint32_t c[5];
};
const Ramp LIGHT_RAMPS[4] = {
    {{0x04050B, 0x0F1E5C, 0x2B6CD0, 0x86E6FF, 0xFFFFFF}}, // night
    {{0x0B0610, 0x3D1A4F, 0xC2468A, 0xFFB088, 0xFFF6EA}}, // dawn
    {{0x050F0D, 0x0D4A46, 0x17A68B, 0xB8F56C, 0xFFFFF0}}, // day
    {{0x0A0510, 0x4A1550, 0xC8365E, 0xFF9C3C, 0xFFF4C4}}, // dusk
};
Col rampAt(const Ramp &r, float v) {
    v = clamp01(v) * 4;
    int a = (int)v;
    if (a > 3) a = 3;
    return mixc(hex(r.c[a]), hex(r.c[a + 1]), v - a);
}

void styleLight(Mem &m, const Walk &w, Comp &c) {
    compose(m, w, c, W * 0.5f, H * 0.48f, W - 60.f, H - 62.f, 30.f);
    Img img{m.img};
    Rng rng(c.seed ^ 0x11u);
    float *D = m.fa, *T = m.fb;
    fclear(D);
    fclear(T);
    // flow field: curl of noise on a coarse grid
    float fs = 0.012f + 0.006f * hashf(c.seed);
    for (int gy = 0; gy < FLOW_H; gy++)
        for (int gx = 0; gx < FLOW_W; gx++) {
            float x = gx * (W / (float)(FLOW_W - 1)), y = gy * (H / (float)(FLOW_H - 1)), e = 2.f;
            float n1 = fbm((x + e) * fs, y * fs, c.seed, 3), n2 = fbm((x - e) * fs, y * fs, c.seed, 3);
            float n3 = fbm(x * fs, (y + e) * fs, c.seed, 3), n4 = fbm(x * fs, (y - e) * fs, c.seed, 3);
            float vx = (n3 - n4), vy = -(n1 - n2), l = hypotf(vx, vy) + 1e-6f;
            m.flow[(gy * FLOW_W + gx) * 2] = vx / l, m.flow[(gy * FLOW_W + gx) * 2 + 1] = vy / l;
        }
    auto flowAt = [&](float x, float y, float &vx, float &vy) {
        float gx = clamp01(x / W) * (FLOW_W - 1), gy = clamp01(y / H) * (FLOW_H - 1);
        int ix = (int)gx, iy = (int)gy;
        if (ix > FLOW_W - 2) ix = FLOW_W - 2;
        if (iy > FLOW_H - 2) iy = FLOW_H - 2;
        float fx = gx - ix, fy = gy - iy;
        const float *a = m.flow + (iy * FLOW_W + ix) * 2, *b = a + 2, *cc = a + FLOW_W * 2, *d = cc + 2;
        vx = (a[0] * (1 - fx) + b[0] * fx) * (1 - fy) + (cc[0] * (1 - fx) + d[0] * fx) * fy;
        vy = (a[1] * (1 - fx) + b[1] * fx) * (1 - fy) + (cc[1] * (1 - fx) + d[1] * fx) * fy;
    };
    // particles
    int P = c.n * 2;
    if (P < 2200) P = 2200;
    if (P > 6000) P = 6000;
    float drift = 0.9f + 1.4f * c.avgSound;
    for (int k = 0; k < P; k++) {
        if ((k & 255) == 0) yieldNow();
        int i;
        do i = (int)(rng.f() * c.n); while (rng.f() > 0.25f + 0.75f * c.p[i < c.n ? i : c.n - 1].sound && rng.f() > 0.15f);
        if (i >= c.n) i = c.n - 1;
        const PP &q = c.p[i];
        float x = q.x + rng.range(-1.2f, 1.2f), y = q.y + rng.range(-1.2f, 1.2f);
        float dir = rng.f() < 0.5f ? -1.f : 1.f;
        float tx = q.tx * dir, ty = q.ty * dir;
        int life = (int)(24 + rng.f() * (40 + 90 * q.sound) * drift);
        float inten = (0.10f + 0.45f * q.sound + 0.12f * q.rough) * 0.55f;
        for (int s = 0; s < life; s++) {
            float fx, fy;
            flowAt(x, y, fx, fy);
            float mixAmt = (float)s / life;
            float vx = tx * (1 - mixAmt) + fx * mixAmt * 1.25f, vy = ty * (1 - mixAmt) + fy * mixAmt * 1.25f;
            float l = hypotf(vx, vy) + 1e-6f;
            x += vx / l * 0.85f, y += vy / l * 0.85f;
            if (x < 1 || y < 1 || x >= W - 2 || y >= H - 2) break;
            float a = inten * (1 - mixAmt) * (1 - mixAmt);
            splat(D, x, y, a);
            splat(T, x, y, a * q.u);
        }
    }
    // the route itself: the brightest line
    for (int i = 0; i < c.n; i++) {
        float a = 0.55f + 0.7f * c.p[i].sound;
        splat(D, c.p[i].x, c.p[i].y, a);
        splat(T, c.p[i].x, c.p[i].y, a * c.p[i].u);
    }
    // marks
    for (int k = 0; k < c.nm; k++) {
        const Mark &q = c.m[k];
        if (q.kind == MK_STOP) {
            float R = 5 + 9 * q.v;
            for (int y = (int)(q.y - R); y <= (int)(q.y + R); y++)
                for (int x = (int)(q.x - R); x <= (int)(q.x + R); x++) {
                    float d = hypotf(x - q.x, y - q.y) / R;
                    if (d < 1 && (unsigned)x < (unsigned)W && (unsigned)y < (unsigned)H) {
                        float a = 0.22f * (1 - d) * (1 - d);
                        D[y * W + x] += a, T[y * W + x] += a * q.u;
                    }
                }
        } else if (q.kind == MK_WIFI) {
            float L = 4 + 9 * q.v;
            for (int s = -(int)L; s <= (int)L; s++) {
                float a = 0.30f * (1 - fabsf((float)s) / L);
                a *= a;
                splat(D, q.x + s, q.y, a), splat(T, q.x + s, q.y, a * q.u);
                splat(D, q.x, q.y + s, a), splat(T, q.x, q.y + s, a * q.u);
            }
            splat(D, q.x, q.y, 1.2f), splat(T, q.x, q.y, 1.2f * q.u);
        } else if (q.kind == MK_SOUND) {
            float R = 6 + 7 * q.v;
            for (int s = 0; s < 64; s++) {
                float an = s * 2 * PI_F / 64;
                splat(D, q.x + cosf(an) * R, q.y + sinf(an) * R, 0.09f);
                splat(T, q.x + cosf(an) * R, q.y + sinf(an) * R, 0.09f * q.u);
            }
        }
    }
    // exposure from the 99th percentile of lit pixels
    float *hist = m.hist; // (8 KB: kept off the stack)
    int hn = 0;
    for (int s = 0; s < 6000 && hn < 2048; s++) {
        int i = (int)(rng.f() * NPX);
        if (D[i] > 1e-4f) hist[hn++] = D[i];
    }
    float p99 = 1;
    if (hn > 10) {
        // partial selection: find the value with ~1% above it
        int want = hn - 1 - hn / 100;
        for (int a = 0; a <= want; a++) { // selection sort up to `want` (hn <= 2048, cheap enough)
            int best = a;
            for (int b = a + 1; b < hn; b++)
                if (hist[b] < hist[best]) best = b;
            float t = hist[a];
            hist[a] = hist[best];
            hist[best] = t;
            if (a > want - 1) break;
        }
        p99 = hist[want];
    }
    float k = 1.9f / (p99 + 1e-6f);
    const Ramp &r0 = LIGHT_RAMPS[todClass(w.todStart)], &r1 = LIGHT_RAMPS[todClass(w.todEnd)];
    // bloom: park the per-pixel time (0..1) in the image buffer, then reuse T for a blurred copy of D
    for (int i = 0; i < NPX; i++) m.img[i] = (uint8_t)(D[i] > 1e-5f ? clamp01(T[i] / D[i]) * 255 : 0);
    memcpy(T, D, sizeof(float) * NPX);
    blur(T, 5);
    // colour pixel i lives at bytes 3i..3i+2 >= i: going backwards, every parked byte is read
    // before a colour write can reach it
    for (int y = H - 1; y >= 0; y--)
        for (int x = W - 1; x >= 0; x--) {
            int i = y * W + x;
            float d = D[i] + 0.9f * T[i], v = 1 - expf(-d * k);
            float u = m.img[i] / 255.f;
            Col col = mixc(rampAt(r0, v), rampAt(r1, v), u);
            float dx = (x - W * 0.5f) / (W * 0.5f), dy = (y - H * 0.5f) / (H * 0.5f);
            float vig = 1 - 0.30f * (dx * dx + dy * dy);
            img.set(i, scalec(col, vig));
        }
    char date[40], stats[48], line[96];
    fmtDate(w, date, sizeof date);
    fmtStats(w, stats, sizeof stats);
    snprintf(line, sizeof line, "%s   %s", date, stats);
    img.text(10, H - 12, line, 1, rampAt(r1, 0.62f), 0.8f);
    img.text(10, 8, "STRIDER", 1, rampAt(r0, 0.62f), 0.8f);
}

// ================================================================== STARS
// A star chart: the Wi-Fi networks you passed are the stars (brighter = stronger signal), joined
// by a minimum spanning tree into a constellation; your route is the milky way through it,
// loudness colours its nebula. Quiet places with few networks borrow stars from your sharpest
// turns and resting places. Night walks get a dark sky, daytime walks an old paper atlas.
void styleStars(Mem &m, const Walk &w, Comp &c) {
    const float CX = W * 0.5f, CY = H * 0.5f + 4, R = 104.f;
    compose(m, w, c, CX, CY, 148.f, 148.f, 30.f);
    Img img{m.img};
    Rng rng(c.seed ^ 0x5151u);
    bool dark = todClass(w.todStart) == TOD_NIGHT || todClass(w.todStart) == TOD_DUSK;
    Col bg0 = dark ? hex(0x03050E) : hex(0xEFE6D2), bg1 = dark ? hex(0x0D1838) : hex(0xE4D7BC);
    Col ink = dark ? hex(0x8FB2E8) : hex(0x23345F), gold = dark ? hex(0xE9D9A2) : hex(0x8A5A1E);
    for (int y = 0; y < H; y++) {
        Col row = mixc(bg0, bg1, (float)y / H);
        for (int x = 0; x < W; x++) {
            float n = (hashf((uint32_t)(y * W + x) ^ c.seed) - 0.5f) * (dark ? 0.012f : 0.03f);
            img.set(y * W + x, {row.r + n, row.g + n, row.b + n});
        }
    }
    // nebula (milky way) around the route, tinted by loudness
    float *A = m.fa, *B = m.fb;
    fclear(A);
    fclear(B);
    for (int i = 0; i < c.n; i++) {
        splat(A, c.p[i].x, c.p[i].y, 0.3f + c.p[i].rf);
        splat(B, c.p[i].x, c.p[i].y, 0.1f + c.p[i].sound);
    }
    blur(A, 12);
    blur(B, 20);
    float ma = fmax(A), mb = fmax(B);
    Col n1 = dark ? hex(0x3A2C7A) : hex(0x6E86B8), n2 = dark ? hex(0xB04A7C) : hex(0xC98A5A);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int i = y * W + x;
            float a = A[i] / (ma + 1e-6f), b = B[i] / (mb + 1e-6f);
            float t = fbm(x * 0.04f, y * 0.04f, c.seed ^ 0x3u, 3);
            float amt = (0.55f * a + 0.45f * b) * (0.55f + 0.7f * t);
            Col nc = mixc(n1, n2, clamp01(b * 1.2f - a * 0.3f));
            img.put(x, y, nc, clamp01(amt) * (dark ? 0.55f : 0.32f), dark ? SCREEN : MULTIPLY);
        }
    // field stars
    int bgStars = dark ? 330 : 150;
    for (int k = 0; k < bgStars; k++) {
        float x = rng.f() * W, y = rng.f() * H, b = rng.f();
        b = b * b * b;
        if (dark) img.put((int)x, (int)y, hex(0xDDE6FF), 0.25f + 0.75f * b, SCREEN);
        else img.put((int)x, (int)y, ink, 0.25f + 0.5f * b);
        if (b > 0.7f) img.disc(x, y, 0.8f, dark ? hex(0xDDE6FF) : ink, 0.5f, dark ? SCREEN : NORMAL);
    }
    // chart: outer circle with degree ticks, inner circles, spokes
    img.ring(CX, CY, R, 0.9f, ink, 0.75f);
    img.ring(CX, CY, R + 5, 0.6f, ink, 0.5f);
    for (int d = 0; d < 360; d += 5) {
        float an = d * PI_F / 180, len = d % 30 == 0 ? 5.f : (d % 10 == 0 ? 3.f : 1.8f);
        img.line(CX + cosf(an) * R, CY + sinf(an) * R, CX + cosf(an) * (R + len), CY + sinf(an) * (R + len), 0.3f, ink, 0.7f);
    }
    for (int ring = 1; ring <= 2; ring++)
        for (int s = 0; s < 180; s += 2) {
            float a0 = s * PI_F / 90, a1 = (s + 1) * PI_F / 90, rr = R * ring / 3;
            img.line(CX + cosf(a0) * rr, CY + sinf(a0) * rr, CX + cosf(a1) * rr, CY + sinf(a1) * rr, 0.25f, ink, 0.22f);
        }
    for (int d = 0; d < 360; d += 30) {
        float an = d * PI_F / 180;
        img.line(CX + cosf(an) * R * 0.12f, CY + sinf(an) * R * 0.12f, CX + cosf(an) * R, CY + sinf(an) * R, 0.25f, ink, 0.12f);
    }
    img.text((int)CX - 3, (int)(CY - R - 13), "N?", 1, ink, 0.8f);
    // route: dotted
    float acc = 0;
    for (int i = 1; i < c.n; i++) {
        acc += hypotf(c.p[i].x - c.p[i - 1].x, c.p[i].y - c.p[i - 1].y);
        if (acc >= 4.5f) {
            acc = 0;
            img.disc(c.p[i].x, c.p[i].y, 0.55f, dark ? hex(0xCFE0FF) : ink, 0.55f, dark ? SCREEN : NORMAL);
        }
    }
    // the stars
    int stars = 0;
    for (int k = 0; k < c.nm; k++)
        if (c.m[k].kind == MK_WIFI) stars++;
    bool sparse = stars < 12;
    if (sparse) addTurnMarks(m, c, 14 - stars);
    int *idx = m.sIdx, ns = 0;
    for (int k = 0; k < c.nm && ns < FIND_MAX + STOP_MAX + 16; k++)
        if (c.m[k].kind == MK_WIFI || (sparse && c.m[k].kind != MK_SOUND)) idx[ns++] = k;
    // constellation: minimum spanning tree (Prim), long edges dropped
    if (ns >= 2) {
        bool *in = m.sIn;
        float *best = m.sBest;
        int *from = m.sFrom;
        for (int i = 0; i < ns; i++) best[i] = 1e9f, from[i] = -1, in[i] = false;
        best[0] = 0;
        for (int it = 0; it < ns; it++) {
            int u = -1;
            for (int i = 0; i < ns; i++)
                if (!in[i] && (u < 0 || best[i] < best[u])) u = i;
            in[u] = true;
            if (from[u] >= 0 && best[u] < 70.f) {
                const Mark &a = c.m[idx[u]], &b = c.m[idx[from[u]]];
                img.line(a.x, a.y, b.x, b.y, 0.45f, gold, dark ? 0.6f : 0.75f);
            }
            for (int i = 0; i < ns; i++) {
                if (in[i]) continue;
                float d = hypotf(c.m[idx[i]].x - c.m[idx[u]].x, c.m[idx[i]].y - c.m[idx[u]].y);
                if (d < best[i]) best[i] = d, from[i] = u;
            }
        }
    }
    for (int s = 0; s < ns; s++) {
        const Mark &q = c.m[idx[s]];
        float r = 0.9f + 1.9f * q.v;
        float temp = hashf(q.id ^ 0xBEEFu);
        Col sc = dark ? (temp < 0.33f ? hex(0xBFD4FF) : temp < 0.8f ? hex(0xFFFFFF) : hex(0xFFE2A8)) : ink;
        if (dark) {
            img.disc(q.x, q.y, r * 3.2f, sc, 0.10f, SCREEN);
            img.disc(q.x, q.y, r * 1.7f, sc, 0.22f, SCREEN);
            if (q.v > 0.65f) {
                img.line(q.x - r * 6, q.y, q.x + r * 6, q.y, 0.3f, sc, 0.35f, SCREEN);
                img.line(q.x, q.y - r * 6, q.x, q.y + r * 6, 0.3f, sc, 0.35f, SCREEN);
            }
            img.disc(q.x, q.y, r, sc, 1);
        } else {
            img.disc(q.x, q.y, r + 1.2f, bg0, 1);
            img.disc(q.x, q.y, r, sc, 1);
        }
    }
    // sound events: small nebulae; stops (when not used as stars): little open circles
    for (int k = 0; k < c.nm; k++) {
        const Mark &q = c.m[k];
        if (q.kind == MK_SOUND) img.ring(q.x, q.y, 3 + 3 * q.v, 0.6f, gold, 0.5f);
    }
    // name
    char name[24], title[40], date[40], stats[64], tag[24];
    makeName(c.seed, name, sizeof name);
    static const char *const SUF[] = {"MINOR", "MAJOR", "BOREALIS", "AUSTRALIS", "NOVA", "OBSCURA"};
    snprintf(title, sizeof title, "%s", name);
    fmtDate(w, date, sizeof date);
    fmtStats(w, stats, sizeof stats);
    snprintf(tag, sizeof tag, "%d STARS", ns);
    img.text(10, 10, title, 2, gold);
    img.text(10, 23, SUF[(c.seed >> 7) % 6], 1, gold, 0.85f);
    img.text(W - 10 - textWidth(date, 1), 10, date, 1, ink, 0.85f);
    img.text(W - 10 - textWidth(tag, 1), 18, tag, 1, ink, 0.85f);
    img.text(10, H - 12, stats, 1, ink, 0.85f);
}

// ------------------------------------------------------------------ output
void toRgb565(const Mem &m, uint16_t *out) {
    static const uint8_t BAYER[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    yieldNow();
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int i = y * W + x;
            float d = BAYER[y & 3][x & 3] / 16.f - 0.5f + 1.f / 32;
            int r = (int)(m.img[i * 3] / 255.f * 31 + d + 0.5f), g = (int)(m.img[i * 3 + 1] / 255.f * 63 + d + 0.5f),
                b = (int)(m.img[i * 3 + 2] / 255.f * 31 + d + 0.5f);
            r = r < 0 ? 0 : r > 31 ? 31 : r;
            g = g < 0 ? 0 : g > 63 ? 63 : g;
            b = b < 0 ? 0 : b > 31 ? 31 : b;
            out[i] = (uint16_t)(r << 11 | g << 5 | b);
        }
}

} // namespace

const char *styleName(int style) {
    static const char *const N[STYLE_COUNT] = {"TOPO", "RISO", "LIGHT", "STARS"};
    return style >= 0 && style < STYLE_COUNT ? N[style] : "?";
}

int styleDefault(const Walk &w) {
    uint32_t s = walkSeed(w);
    switch (todClass(w.todStart)) {
        case TOD_NIGHT: return w.rfSeen >= 6 ? STYLE_STARS : STYLE_LIGHT;
        case TOD_DUSK: return (s & 3) == 0 ? STYLE_STARS : STYLE_LIGHT;
        case TOD_DAWN: return (s & 1) ? STYLE_RISO : STYLE_LIGHT;
        default: return (s & 1) ? STYLE_TOPO : STYLE_RISO;
    }
}

size_t artMemBytes() { return sizeof(Mem); }

void artSetYield(void (*fn)()) { g_yield = fn; }

void artRender(const Walk &w, int style, uint16_t *out565, void *mem) {
    Mem &m = *(Mem *)mem;
    Comp c;
    switch (style) {
        case STYLE_RISO: styleRiso(m, w, c); break;
        case STYLE_LIGHT: styleLight(m, w, c); break;
        case STYLE_STARS: styleStars(m, w, c); break;
        default: styleTopo(m, w, c); break;
    }
    toRgb565(m, out565);
}

void artText565(uint16_t *buf, int x, int y, const char *s, int scale, uint16_t color) {
    drawText(x, y, s, scale, [&](int px, int py) {
        if ((unsigned)px < (unsigned)W && (unsigned)py < (unsigned)H) buf[py * W + px] = color;
    });
}

} // namespace strider
