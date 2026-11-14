// Mantis Strider print preview on a PC: simulates a few walks and renders every style to .mg.
//   g++ -O2 -std=c++17 -I src tools/strider_preview/preview.cpp src/strider/*.cpp -o /tmp/strider_preview
//   /tmp/strider_preview out_dir && python3 tools/strider_preview/mg_sheet.py out_dir
#include "strider/strider_art.h"
#include "strider/strider_walk.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <vector>

using namespace strider;

static uint32_t rng = 12345;
static float frand() {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return (rng >> 8) / 16777216.f;
}

struct Sim {
    Walk w;
    std::vector<Sample> buf;
    float t = 0, heading = 0, gyroDrift = 0;
    Sim(Mode m, float tod, int y, int mo, int d, int h, int mi) : buf(4096) {
        w.year = y, w.month = mo, w.day = d, w.hour = h, w.minute = mi;
        walkBegin(w, buf.data(), (int)buf.size(), m, tod);
    }
    // n steps turning `turn` radians in total, with environment levels
    void leg(int n, float turn, float sound, float rough, float rf, float pace, float stride = 0.75f, float dt = 0.55f) {
        for (int i = 0; i < n; i++) {
            heading += turn / n + (frand() - 0.5f) * 0.05f + gyroDrift;
            t += dt;
            float s = sound + (frand() - 0.5f) * 0.15f;
            walkSetEnv(w, s < 0 ? 0 : s, rough * (0.7f + 0.6f * frand()), rf, pace);
            walkTick(w, t, true);
            w.steps++;
            walkAdvance(w, stride, heading);
            if (sound > 0.75f && frand() < 0.01f) walkAddFind(w, FIND_SOUND, (uint32_t)(t * 1000), (int)(sound * 127));
            if (rf > 0 && frand() < rf * 0.06f) walkAddFind(w, FIND_WIFI, (uint32_t)(frand() * 4e9f), -95 + (int)(frand() * 60));
        }
    }
    void stop(float sec) {
        walkTick(w, t, false);
        t += sec;
        walkTick(w, t, false);
    }
};

static void writeMg(const char *path, const uint16_t *px) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    const uint8_t hdr[8] = {'M', 'G', '0', '1', 0x40, 0x01, 0xF0, 0x00};
    fwrite(hdr, 1, 8, f);
    fwrite(px, 2, ART_W * ART_H, f); // little-endian host
    fclose(f);
}

int main(int argc, char **argv) {
    const char *out = argc > 1 ? argv[1] : "strider_out";
    mkdir(out, 0755);
    void *mem = malloc(artMemBytes());
    static uint16_t px[ART_W * ART_H];
    printf("art scratch: %zu bytes\n", artMemBytes());

    std::vector<Sim *> sims;
    // 1: midday park loop with a cafe stop
    {
        auto *s = new Sim(MODE_WALK, 12.4f / 24, 2026, 11, 14, 12, 24);
        s->leg(180, 0.3f, 0.25f, 0.2f, 0.0f, 0.5f);
        s->leg(60, 1.6f, 0.3f, 0.2f, 0.1f, 0.5f);
        s->leg(140, -0.4f, 0.55f, 0.15f, 0.5f, 0.55f);
        s->stop(240);
        s->leg(90, 1.7f, 0.85f, 0.1f, 0.8f, 0.4f);
        s->leg(200, 0.5f, 0.3f, 0.5f, 0.1f, 0.6f);
        s->leg(80, 1.4f, 0.2f, 0.6f, 0.0f, 0.65f);
        s->leg(160, 0.2f, 0.25f, 0.3f, 0.05f, 0.55f);
        sims.push_back(s);
    }
    // 2: city grid at dusk: right-angle turns, many networks, loud junctions
    {
        auto *s = new Sim(MODE_WALK, 18.6f / 24, 2026, 11, 13, 18, 36);
        float turns[] = {1.57f, -1.57f, 1.57f, 1.57f, -1.57f, 1.57f, 1.57f, -1.57f, -1.57f, 1.57f};
        for (int k = 0; k < 10; k++) {
            s->leg(70 + (int)(frand() * 60), 0.05f, 0.45f, 0.1f, 0.9f, 0.6f);
            s->leg(3, turns[k], 0.9f, 0.1f, 0.9f, 0.3f);
            if (k == 4) s->stop(90);
        }
        sims.push_back(s);
    }
    // 3: night trail: long, quiet, meandering, few networks, gyro drift spirals it
    {
        auto *s = new Sim(MODE_WALK, 22.5f / 24, 2026, 11, 12, 22, 30);
        s->gyroDrift = 0.0015f;
        for (int k = 0; k < 14; k++) s->leg(110, (frand() - 0.5f) * 3.0f, 0.12f + (k == 6 ? 0.7f : 0), 0.7f, k < 2 ? 0.4f : 0, 0.45f);
        s->stop(400);
        s->leg(120, 2.5f, 0.1f, 0.8f, 0.0f, 0.4f);
        sims.push_back(s);
    }
    // 4: dawn ride: long sweeping road, gravel section, village in the middle
    {
        auto *s = new Sim(MODE_RIDE, 6.8f / 24, 2026, 11, 11, 6, 48);
        s->leg(400, 1.2f, 0.4f, 0.2f, 0.0f, 0.8f, 5.0f, 1.0f);
        s->leg(300, -2.4f, 0.3f, 0.9f, 0.0f, 0.5f, 3.5f, 1.0f);
        s->leg(150, 0.6f, 0.7f, 0.3f, 0.9f, 0.6f, 4.0f, 1.0f);
        s->stop(300);
        s->leg(500, 2.2f, 0.45f, 0.25f, 0.05f, 0.9f, 5.5f, 1.0f);
        sims.push_back(s);
    }
    const char *names[] = {"park_noon", "city_dusk", "trail_night", "ride_dawn"};
    for (size_t k = 0; k < sims.size(); k++) {
        Walk &w = sims[k]->w;
        walkEnd(w, (w.todStart * 24 + w.t / 3600.f) / 24);
        printf("%s: %d samples, %.0f m, %d finds, %d stops, default %s\n", names[k], w.n, w.distance, w.nFinds, w.nStops,
               styleName(styleDefault(w)));
        for (int st = 0; st < STYLE_COUNT; st++) {
            artRender(w, st, px, mem);
            char path[256];
            snprintf(path, sizeof path, "%s/%s_%d_%s.mg", out, names[k], st, styleName(st));
            writeMg(path, px);
        }
    }
    // standing still: no movement at all
    {
        Sim s(MODE_WALK, 15.f / 24, 2026, 11, 14, 15, 0);
        s.stop(60);
        walkEnd(s.w, 15.02f / 24);
        artRender(s.w, STYLE_TOPO, px, mem);
        char path[256];
        snprintf(path, sizeof path, "%s/still_0_TOPO.mg", out);
        writeMg(path, px);
    }
    free(mem);
    return 0;
}
