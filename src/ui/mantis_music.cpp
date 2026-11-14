// MantisCore music player ("MantisAmp"): MP3s from /music on the SD card, Winamp-style.
//
// Audio path (three stages, so the screen can never starve the speaker):
//   SD card --[mcSdRead task]--> compressed ring buffer (128 KB PSRAM, seconds of audio)
//           --[mcAudio task]---> libhelix MP3 decoder -> PcmSink -> M5.Speaker.playRaw (triple buffer)
//
// The SD card shares the SPI bus with the display on the Core2, so SD reads and display pushes take
// the same mutex. Only the reader task touches the card, in short 4 KB reads; the decoder only reads
// the ring buffer and never waits for the bus. The UI pushes the screen in 24-row strips and releases
// the bus between strips, so neither side holds it for more than a few milliseconds.
//
// Visuals: the decoder only copies 256 mono samples per PCM buffer, stamped with the time that
// buffer will actually be heard. The UI task (other core) runs the FFT for the spectrum analyser,
// so the analyser costs the audio path nothing and stays in step with what you hear.
//
// Sub-bass haptics: a low-pass filter (~85 Hz) on the decoded audio drives the Core2 vibration
// motor, so you feel the kick drum. Toggle with the SUB switch. Playback stops when the player closes.
#include "mantis_internal.h"
#include <MP3DecoderHelix.h>
#include <Preferences.h>
#include <SD.h>
#include <algorithm>
#include <dirent.h>
#include <math.h>

using namespace libhelix;

#define MC_LCD_BG 0x0000
#define MC_LCD_DIM 0x0160
#define MC_LCD_MID 0x0400 // half-bright LCD green
#define MC_PEAK 0xD81B    // light purple peak caps
#define MC_PANEL 0x2965
#define MC_TEAL_LT 0x0516
#define MC_TEAL_DK 0x01C7

// ------------------------------------------------------------------ shared state
static SemaphoreHandle_t busLock = nullptr;  // SD card + display (same SPI bus)
static SemaphoreHandle_t fileLock = nullptr; // audioFile, data range and ring reset (reader vs. decoder)
static TaskHandle_t audioTask = nullptr, readerTask = nullptr;
static volatile bool audioQuit = false;
static volatile bool playing = false;
static volatile int pendingOpen = -1;       // track index to open
static volatile bool pendingCut = false;    // drop audio already queued in the speaker (user action)
static volatile float pendingSeek = -1;     // 0..1
static volatile bool trackEnded = false;

static std::vector<String> tracks;          // full paths
static int current = -1;

// written by the audio task, read by the UI
static volatile uint32_t playedFrames = 0, durationFrames = 0;
static volatile uint32_t sampleRate = 44100, bitrate = 0;
static volatile uint8_t channels = 2;
static portMUX_TYPE titleMux = portMUX_INITIALIZER_UNLOCKED;
static char trackTitle[96] = "";            // guarded by titleMux

// settings
static uint8_t volumePct = 70;
static bool shuffle = false, haptic = true;
static std::vector<int> history;            // for PREV in shuffle mode
static uint8_t vib = 0;                     // current motor level

static void lockBus() { xSemaphoreTake(busLock, portMAX_DELAY); }
static void unlockBus() { xSemaphoreGive(busLock); }

// Push canvas rows [y0, y1) in short strips, giving the SD reader the bus between strips.
static void pushRows(int y0, int y1) {
    const int STRIP = 24; // 24 x 320 px = 15 KB, ~4 ms on the bus
    for (int y = y0; y < y1; y += STRIP) {
        int h = std::min(STRIP, y1 - y);
        lockBus();
        M5.Display.setClipRect(0, y, MC_W, h);
        mcCanvas.pushSprite(0, 0); // only the clipped rows are sent
        M5.Display.waitDMA();
        M5.Display.clearClipRect();
        unlockBus();
    }
}

// ------------------------------------------------------------------ compressed ring buffer
// Single producer (reader task) / single consumer (audio task), both pinned to core 0.
// Counters run freely; size is a power of two so (counter & mask) is the offset.
static uint8_t *ring = nullptr;
static uint32_t ringSize = 0;
static volatile uint32_t rHead = 0, rTail = 0;
static volatile bool fileEof = true;        // reader reached dataEnd (set after the last bytes are published)
static File audioFile;                      // guarded by fileLock
static uint32_t dataStart = 0, dataEnd = 0; // MP3 data range without ID3 tags, guarded by fileLock
static constexpr uint32_t STAGE = 4096;     // bytes per SD read
static uint8_t stage[STAGE];                // internal RAM landing buffer for SD reads

static bool ringAlloc() {
    if (ring) return true;
    ringSize = 128 * 1024;
    ring = (uint8_t *)heap_caps_malloc(ringSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ring) {
        ringSize = 32 * 1024;
        ring = (uint8_t *)heap_caps_malloc(ringSize, MALLOC_CAP_8BIT);
    }
    if (!ring) ringSize = 0;
    return ring != nullptr;
}
static void ringFree() {
    free(ring);
    ring = nullptr;
    ringSize = 0;
}

static void readerLoop(void *) {
    while (!audioQuit) {
        bool busy = false;
        xSemaphoreTake(fileLock, portMAX_DELAY);
        if (audioFile && !fileEof && ringSize - (rHead - rTail) >= STAGE) {
            lockBus();
            uint32_t pos = audioFile.position();
            int n = pos < dataEnd ? audioFile.read(stage, std::min<uint32_t>(STAGE, dataEnd - pos)) : 0;
            unlockBus();
            if (n <= 0) {
                fileEof = true;
            } else {
                uint32_t off = rHead & (ringSize - 1), first = std::min<uint32_t>(n, ringSize - off);
                memcpy(ring + off, stage, first);
                if ((uint32_t)n > first) memcpy(ring, stage + first, n - first);
                __sync_synchronize();
                rHead = rHead + n;
                busy = true;
            }
        }
        xSemaphoreGive(fileLock);
        vTaskDelay(busy ? 1 : pdMS_TO_TICKS(8));
    }
    readerTask = nullptr;
    vTaskDelete(nullptr);
}

// ------------------------------------------------------------------ analyser snapshots
// One per PCM buffer handed to the speaker: 256 mono samples + sub-bass level, stamped with the
// millis() at which that buffer starts to be heard. The UI shows the one playing right now.
static constexpr int FFT_N = 256;
static constexpr int SNAPS = 8;
struct Snap {
    volatile uint32_t at = 0;  // 0 = empty
    volatile uint32_t dur = 0; // ms
    volatile uint8_t bass = 0; // 0..255
    int16_t mono[FFT_N];
};
static Snap snaps[SNAPS];
static volatile uint32_t snapCount = 0;

static void clearSnaps() {
    for (auto &s : snaps) s.at = 0;
}

// index of the snapshot being heard at `now`, or -1
static int pickSnap(uint32_t now) {
    int best = -1;
    int32_t bestAge = INT32_MAX;
    for (int i = 0; i < SNAPS; i++) {
        uint32_t at = snaps[i].at;
        if (!at) continue;
        int32_t age = (int32_t)(now - at);
        if (age >= 0 && age < (int32_t)snaps[i].dur + 120 && age < bestAge) {
            best = i;
            bestAge = age;
        }
    }
    return best;
}

// ------------------------------------------------------------------ PCM sink -> speaker
static constexpr size_t PCM_BUF = 4096; // int16 samples per buffer (~46 ms of 44.1 kHz stereo)
static constexpr int SPK_CH = 0;

class PcmSink : public Print {
  public:
    MP3DecoderHelix *dec = nullptr;
    int16_t *buf[3] = {nullptr, nullptr, nullptr};
    size_t fill = 0;
    int idx = 0;
    float lp = 0;            // sub-bass low-pass state
    float env = 0;           // its envelope over the current buffer
    uint32_t playEnd = 0;    // millis() when the audio queued so far runs out
    uint32_t outLatency = 0; // ms of I2S DMA buffering between the speaker queue and the cone

    bool begin() {
        for (auto &b : buf) {
            if (!b) b = (int16_t *)heap_caps_malloc(PCM_BUF * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            if (!b) b = (int16_t *)heap_caps_malloc(PCM_BUF * sizeof(int16_t), MALLOC_CAP_8BIT);
            if (!b) return false;
        }
        fill = 0;
        return true;
    }
    void release() {
        for (auto &b : buf) {
            free(b);
            b = nullptr;
        }
    }
    size_t write(uint8_t) override { return 1; }
    size_t write(const uint8_t *data, size_t len) override {
        MP3FrameInfo info = dec->audioInfo();
        if (info.samprate > 0) sampleRate = info.samprate;
        if (info.nChans > 0) channels = info.nChans;
        if (info.bitrate > 0) bitrate = info.bitrate;
        const int16_t *s = (const int16_t *)data;
        size_t n = len / 2;
        float alpha = 2.0f * (float)M_PI * 85.0f / (float)sampleRate;
        for (size_t i = 0; i < n; i++) {
            int16_t v = s[i];
            buf[idx][fill++] = v;
            if (channels == 1 || (i & 1)) { // once per frame
                float mono = channels == 1 ? v : (v + s[i - 1]) * 0.5f;
                lp += (mono - lp) * alpha;
                env = std::max(env, fabsf(lp));
            }
            if (fill >= PCM_BUF) flush();
        }
        playedFrames = playedFrames + n / channels;
        return len;
    }
    void flush() {
        if (!fill) return;
        while (M5.Speaker.isPlaying(SPK_CH) == 2) { // one playing + one queued: wait for a slot
            if (audioQuit) return;
            vTaskDelay(1);
        }
        uint32_t now = millis();
        bool idle = M5.Speaker.isPlaying(SPK_CH) == 0;
        M5.Speaker.playRaw(buf[idx], fill, sampleRate, channels == 2, 1, SPK_CH, false);
        uint32_t ch = channels ? channels : 2, frames = fill / ch;
        uint32_t start = (idle || (int32_t)(playEnd - now) < 0) ? now : playEnd;
        uint32_t dur = (uint32_t)((uint64_t)frames * 1000 / (sampleRate ? sampleRate : 44100));
        playEnd = start + dur;
        snapshot(buf[idx], frames, ch, start + outLatency, dur);
        env = 0;
        fill = 0;
        idx = (idx + 1) % 3;
    }
    // drop everything queued (seek, skip): the new audio starts right away
    void cut() {
        M5.Speaker.stop(SPK_CH);
        fill = 0;
        env = 0;
        playEnd = millis();
        clearSnaps();
    }

  private:
    void snapshot(const int16_t *s, uint32_t frames, uint32_t ch, uint32_t at, uint32_t dur) {
        Snap &p = snaps[snapCount % SNAPS];
        p.at = 0; // invalid while rewritten
        uint32_t first = frames > (uint32_t)FFT_N ? (frames - FFT_N) / 2 : 0;
        for (int i = 0; i < FFT_N; i++) {
            uint32_t k = first + i;
            p.mono[i] = k >= frames ? 0 : ch == 2 ? (int16_t)((s[2 * k] + s[2 * k + 1]) / 2) : s[k];
        }
        p.bass = (uint8_t)std::min(255.0f, env / 9000.0f * 255.0f);
        p.dur = dur;
        p.at = at ? at : 1;
        snapCount = snapCount + 1;
    }
};

static PcmSink sink;
static MP3DecoderHelix decoder(sink);

// ------------------------------------------------------------------ spectrum (UI task)
// 256-point FFT of the mono snapshot -> 20 log-spaced bands for the analyser
static float hannWin[FFT_N];
static uint8_t bandLo[20], bandHi[20];

static void initAnalyser() {
    static bool ready = false;
    if (ready) return;
    for (int i = 0; i < FFT_N; i++) hannWin[i] = 0.5f - 0.5f * cosf(2 * (float)M_PI * i / (FFT_N - 1));
    for (int b = 0; b < 20; b++) { // bins 1..127, log spaced
        int lo = (int)powf(127.0f, b / 20.0f), hi = std::max(lo + 1, (int)powf(127.0f, (b + 1) / 20.0f));
        bandLo[b] = lo;
        bandHi[b] = std::min(hi, 128);
    }
    ready = true;
}

static void analyse(const int16_t *s, uint8_t *bands) {
    static float re[FFT_N], im[FFT_N];
    const int N = FFT_N;
    for (int i = 0; i < N; i++) {
        re[i] = s[i] * hannWin[i];
        im[i] = 0;
    }
    for (int i = 1, j = 0; i < N; i++) { // bit reversal
        int bit = N >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
    }
    for (int len = 2; len <= N; len <<= 1) {
        float ang = -2 * (float)M_PI / len, wr = cosf(ang), wi = sinf(ang);
        for (int i = 0; i < N; i += len) {
            float cr = 1, ci = 0;
            for (int k = 0; k < len / 2; k++) {
                float ur = re[i + k], ui = im[i + k];
                float vr = re[i + k + len / 2] * cr - im[i + k + len / 2] * ci;
                float vi = re[i + k + len / 2] * ci + im[i + k + len / 2] * cr;
                re[i + k] = ur + vr; im[i + k] = ui + vi;
                re[i + k + len / 2] = ur - vr; im[i + k + len / 2] = ui - vi;
                float t = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = t;
            }
        }
    }
    for (int b = 0; b < 20; b++) {
        float m = 0;
        for (int k = bandLo[b]; k < bandHi[b]; k++) m = std::max(m, re[k] * re[k] + im[k] * im[k]);
        float db = 10 * log10f(m + 1) - 60; // power -> dB; ~0..60 dB useful range
        bands[b] = (uint8_t)std::max(0.0f, std::min(255.0f, db * 255.0f / 60.0f));
    }
}

// ------------------------------------------------------------------ files and tags
static void scanTracks(const char *dir, int depth) {
    char p[160];
    snprintf(p, sizeof(p), "/sd%s", dir);
    DIR *d = opendir(p); // readdir: names + types without opening every file
    if (!d) return;
    std::vector<String> subdirs;
    while (struct dirent *e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        String full = String(dir) + "/" + e->d_name;
        if (e->d_type == DT_DIR) {
            if (depth < 2) subdirs.push_back(full);
        } else {
            String n = e->d_name;
            n.toLowerCase();
            if (n.endsWith(".mp3") && tracks.size() < 1000) tracks.push_back(full);
        }
    }
    closedir(d);
    for (const String &s : subdirs) scanTracks(s.c_str(), depth + 1);
}

static String baseName(const String &path) {
    int s = path.lastIndexOf('/');
    String n = path.substring(s + 1);
    if (n.length() > 4) n.remove(n.length() - 4); // ".mp3"
    return n;
}

static uint32_t syncsafe(const uint8_t *b) { return (b[0] << 21) | (b[1] << 14) | (b[2] << 7) | b[3]; }

// Reads TPE1/TIT2 from an ID3v2.3/2.4 tag. Returns the size of the tag so the decoder skips it
// (libhelix can trip over metadata). Caller holds busLock.
static uint32_t readId3(File &f, String &artist, String &title) {
    uint8_t h[10];
    f.seek(0);
    if (f.read(h, 10) != 10 || memcmp(h, "ID3", 3) != 0) return 0;
    uint32_t size = syncsafe(h + 6) + 10 + ((h[5] & 0x10) ? 10 : 0);
    uint8_t ver = h[3];
    uint32_t pos = 10, end = std::min<uint32_t>(size, 64 * 1024);
    while (pos + 10 <= end && (artist.isEmpty() || title.isEmpty())) {
        uint8_t fh[10];
        f.seek(pos);
        if (f.read(fh, 10) != 10 || fh[0] == 0) break;
        uint32_t fs = ver >= 4 ? syncsafe(fh + 4) : ((uint32_t)fh[4] << 24 | fh[5] << 16 | fh[6] << 8 | fh[7]);
        if (fs == 0 || pos + 10 + fs > size) break;
        bool isTitle = !memcmp(fh, "TIT2", 4), isArtist = !memcmp(fh, "TPE1", 4);
        if ((isTitle || isArtist) && fs > 1 && fs < 200) {
            uint8_t data[200];
            f.read(data, fs);
            String v;
            uint8_t enc = data[0];
            for (uint32_t i = 1; i < fs; i++) {
                uint8_t c = data[i];
                if (enc == 1 || enc == 2) { // UTF-16: keep plain ASCII characters
                    if (i + 1 < fs && c >= 32 && c < 127 && data[i + 1] == 0) v += (char)c;
                    else if (i + 1 < fs && data[i] == 0 && data[i + 1] >= 32 && data[i + 1] < 127) v += (char)data[i + 1];
                    i++;
                } else if (c >= 32 && c < 127) v += (char)c;
            }
            v.trim();
            if (isTitle) title = v;
            else artist = v;
        }
        pos += 10 + fs;
    }
    return size;
}

// ------------------------------------------------------------------ audio (decoder) task
static void audioLoop(void *) {
    bool fileOk = false;
    while (!audioQuit) {
        int open = pendingOpen;
        if (open >= 0) {
            pendingOpen = -1;
            if (pendingCut) {
                pendingCut = false;
                sink.cut();
            }
            String artist, title;
            xSemaphoreTake(fileLock, portMAX_DELAY);
            lockBus();
            if (audioFile) audioFile.close();
            audioFile = SD.open(tracks[open], FILE_READ);
            dataStart = dataEnd = 0;
            if (audioFile) {
                dataStart = readId3(audioFile, artist, title);
                dataEnd = audioFile.size();
                if (dataEnd > 128) { // ID3v1 tag at the end
                    uint8_t t[3];
                    audioFile.seek(dataEnd - 128);
                    if (audioFile.read(t, 3) == 3 && !memcmp(t, "TAG", 3)) dataEnd -= 128;
                }
                if (dataStart > dataEnd) dataStart = dataEnd;
                audioFile.seek(dataStart);
            }
            unlockBus();
            fileOk = (bool)audioFile;
            rHead = 0;
            rTail = 0;
            fileEof = !fileOk;
            xSemaphoreGive(fileLock);

            String shown = title.length() ? (artist.length() ? artist + " - " + title : title) : baseName(tracks[open]);
            char t[sizeof(trackTitle)];
            snprintf(t, sizeof(t), "%s", shown.c_str());
            portENTER_CRITICAL(&titleMux);
            memcpy(trackTitle, t, sizeof(trackTitle));
            portEXIT_CRITICAL(&titleMux);

            decoder.end();
            decoder.begin();
            sink.fill = 0;
            playedFrames = 0;
            durationFrames = 0;
            bitrate = 0;
            trackEnded = false;
        }
        float seek = pendingSeek;
        if (seek >= 0) {
            pendingSeek = -1;
            if (fileOk) {
                sink.cut();
                xSemaphoreTake(fileLock, portMAX_DELAY);
                lockBus();
                audioFile.seek(dataStart + (uint32_t)((dataEnd - dataStart) * seek));
                unlockBus();
                rHead = 0;
                rTail = 0;
                fileEof = false;
                xSemaphoreGive(fileLock);
                decoder.end();
                decoder.begin();
                playedFrames = (uint32_t)(durationFrames * seek);
            }
        }
        if (!playing) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (!fileOk) { // unreadable file: stop rather than spin
            playing = false;
            continue;
        }
        if (bitrate > 0 && durationFrames == 0) // estimate once the bitrate is known (exact for CBR)
            durationFrames = (uint32_t)((double)(dataEnd - dataStart) * 8.0 / bitrate * sampleRate);

        bool eof = fileEof; // read before the head: once set, every byte of the file is in the ring
        __sync_synchronize();
        uint32_t avail = rHead - rTail;
        if (avail == 0) {
            if (eof) {
                sink.flush();
                playing = false;
                trackEnded = true;
            } else {
                vTaskDelay(1); // reader is catching up (only right after open / seek)
            }
            continue;
        }
        uint32_t off = rTail & (ringSize - 1);
        uint32_t n = std::min<uint32_t>(std::min<uint32_t>(avail, ringSize - off), 1024);
        decoder.write(ring + off, n);
        rTail = rTail + n;
    }
    decoder.end();
    audioTask = nullptr;
    vTaskDelete(nullptr);
}

// ------------------------------------------------------------------ track control
static void requestOpen(int i, bool play, bool cut) {
    if (cut) pendingCut = true; // sticky until the audio task handles it
    pendingOpen = i;
    playing = play;
}
// cut: a user action (skip, pick from list) drops the audio already queued. Auto-advance at the end
// of a song does not, so the last half second of the old song still plays out.
static void openTrack(int i, bool play, bool cut = true) {
    if (tracks.empty()) return;
    i = ((i % (int)tracks.size()) + (int)tracks.size()) % (int)tracks.size();
    if (current >= 0 && current != i) {
        history.push_back(current);
        if (history.size() > 50) history.erase(history.begin());
    }
    current = i;
    requestOpen(i, play, cut);
}
static void nextTrack(bool cut = true) {
    if (tracks.empty()) return;
    if (shuffle && tracks.size() > 1) {
        int n;
        do n = random(tracks.size()); while (n == current);
        openTrack(n, true, cut);
    } else openTrack(current + 1, true, cut);
}
static void prevTrack() {
    if (tracks.empty()) return;
    if (playedFrames > sampleRate * 3) { openTrack(current, true); return; } // restart the song first
    if (shuffle && !history.empty()) {
        int p = history.back();
        history.pop_back();
        current = p;
        requestOpen(p, true, true);
        return;
    }
    openTrack(current - 1, true);
}

// level of the sub-bass being heard right now (0 when silent / paused)
static uint8_t bassNow(uint32_t now) {
    int si = pickSnap(now);
    return (si >= 0 && playing) ? snaps[si].bass : 0;
}

// called often from every UI loop while the player is open: auto-advance + haptics
static void serviceAudio() {
    if (trackEnded) {
        trackEnded = false;
        nextTrack(false);
    }
    // sub-bass haptics: kick drum -> vibration motor (only touch I2C from this task)
    uint8_t b = bassNow(millis());
    uint8_t want = 0;
    if (haptic && playing && b > 70) want = (uint8_t)std::min(255, 110 + b / 2);
    if ((want == 0) != (vib == 0) || abs((int)want - (int)vib) > 24) {
        vib = want;
        M5.Power.setVibration(vib);
    }
}

// ------------------------------------------------------------------ drawing
static void bevel(int x, int y, int w, int h, bool raised, uint16_t fill) {
    mcCanvas.fillRect(x, y, w, h, fill);
    uint16_t a = raised ? MC_TEAL_LT : MC_TEAL_DK, b = raised ? MC_TEAL_DK : MC_TEAL_LT;
    mcCanvas.drawFastHLine(x, y, w, a);
    mcCanvas.drawFastVLine(x, y, h, a);
    mcCanvas.drawFastHLine(x, y + h - 1, w, b);
    mcCanvas.drawFastVLine(x + w - 1, y, h, b);
}

static void fmtTime(uint32_t secs, char *b, size_t n) { snprintf(b, n, "%u:%02u", (unsigned)(secs / 60), (unsigned)(secs % 60)); }

// button glyphs drawn with primitives
enum { G_PREV, G_PLAY, G_PAUSE, G_NEXT, G_SHUF, G_LIST };
static void glyph(int g, int cx, int cy, uint16_t c) {
    switch (g) {
        case G_PREV:
            mcCanvas.fillRect(cx - 9, cy - 6, 2, 13, c);
            mcCanvas.fillTriangle(cx - 6, cy, cx + 1, cy - 6, cx + 1, cy + 6, c);
            mcCanvas.fillTriangle(cx + 1, cy, cx + 8, cy - 6, cx + 8, cy + 6, c);
            break;
        case G_NEXT:
            mcCanvas.fillTriangle(cx + 6, cy, cx - 1, cy - 6, cx - 1, cy + 6, c);
            mcCanvas.fillTriangle(cx - 1, cy, cx - 8, cy - 6, cx - 8, cy + 6, c);
            mcCanvas.fillRect(cx + 7, cy - 6, 2, 13, c);
            break;
        case G_PLAY: mcCanvas.fillTriangle(cx - 5, cy - 8, cx - 5, cy + 8, cx + 8, cy, c); break;
        case G_PAUSE:
            mcCanvas.fillRect(cx - 6, cy - 7, 4, 15, c);
            mcCanvas.fillRect(cx + 2, cy - 7, 4, 15, c);
            break;
        case G_SHUF: // two crossing arrows
            mcCanvas.drawLine(cx - 9, cy - 5, cx + 5, cy + 5, c);
            mcCanvas.drawLine(cx - 9, cy - 4, cx + 5, cy + 6, c);
            mcCanvas.drawLine(cx - 9, cy + 5, cx + 5, cy - 5, c);
            mcCanvas.drawLine(cx - 9, cy + 6, cx + 5, cy - 4, c);
            mcCanvas.fillTriangle(cx + 9, cy + 5, cx + 4, cy + 1, cx + 4, cy + 9, c);
            mcCanvas.fillTriangle(cx + 9, cy - 5, cx + 4, cy - 9, cx + 4, cy - 1, c);
            break;
        case G_LIST:
            for (int i = 0; i < 4; i++) mcCanvas.fillRect(cx - 9, cy - 7 + i * 4, 18, 2, c);
            break;
    }
}

// layout
static const int BTN_Y = 156, BTN_H = 32, BTN_W = 60;
static int btnX(int i) { return 4 + i * 63; }
static const int VOL_X = 8, VOL_W = 190, VOL_Y = 214;
static const int SUB_X = 216, SUB_Y = 196, SUB_W = 96, SUB_H = 40;
static const int SEEK_X = 10, SEEK_W = 300, SEEK_Y = 146;
static const int LIVE_Y0 = 18, LIVE_Y1 = BTN_Y; // rows that change every frame (LCD, analyser, seek)

static void drawPlayer(uint32_t now, const uint8_t *bands, const uint8_t *peaks, int scroll, bool beat) {
    mcCanvas.fillScreen(MC_CHARCOAL);
    // title bar
    mcCanvas.fillRect(0, 0, MC_W, 16, MC_TEAL_DK);
    for (int y = 3; y < 13; y += 3) mcCanvas.drawFastHLine(4, y, 60, MC_TEAL);
    mcText("MANTISAMP", 160, 8, &fonts::Font0, MC_WHITE, middle_center);
    for (int y = 3; y < 13; y += 3) mcCanvas.drawFastHLine(214, y, 76, MC_TEAL);
    bevel(298, 2, 18, 12, true, MC_PURPLE);
    mcText("X", 307, 8, &fonts::Font0, MC_WHITE, middle_center);

    // LCD
    bevel(4, 19, 312, 76, false, MC_LCD_BG);
    char b[24];
    uint32_t sr = sampleRate ? sampleRate : 44100;
    fmtTime(playedFrames / sr, b, sizeof(b));
    bool blink = !playing && current >= 0 && (now / 500) % 2;
    if (!blink) mcText(b, 14, 41, &fonts::FreeSansBold12pt7b, MC_GREEN, middle_left);
    if (durationFrames) {
        char d[16];
        fmtTime(durationFrames / sr, d, sizeof(d));
        snprintf(b, sizeof(b), "/ %s", d);
        mcText(b, 92, 45, &fonts::Font0, MC_LCD_MID, middle_left);
    }
    glyph(playing ? G_PLAY : G_PAUSE, 150, 40, playing ? MC_GREEN : MC_LCD_MID);
    snprintf(b, sizeof(b), "%u KBPS", (unsigned)(bitrate / 1000));
    mcText(bitrate ? b : "--- KBPS", 306, 28, &fonts::Font0, MC_GREEN, middle_right);
    snprintf(b, sizeof(b), "%u KHZ", (unsigned)(sr / 1000));
    mcText(b, 306, 39, &fonts::Font0, MC_GREEN, middle_right);
    mcText(channels == 1 ? "MONO" : "STEREO", 306, 50, &fonts::Font0, MC_GREEN, middle_right);
    // marquee
    char t[sizeof(trackTitle)];
    if (current < 0) snprintf(t, sizeof(t), "%s", "NO MP3 FILES IN /music");
    else {
        portENTER_CRITICAL(&titleMux);
        memcpy(t, trackTitle, sizeof(t));
        portEXIT_CRITICAL(&titleMux);
    }
    String line = current >= 0 ? String(current + 1) + ". " + t : String(t);
    line.toUpperCase();
    mcCanvas.setFont(&fonts::Font2);
    int tw = mcCanvas.textWidth(line.c_str());
    mcCanvas.setClipRect(10, 62, 300, 28);
    if (tw <= 300) mcText(line.c_str(), 10, 75, &fonts::Font2, MC_GREEN, middle_left);
    else {
        int span = tw + 40, off = scroll % span;
        mcText(line.c_str(), 10 - off, 75, &fonts::Font2, MC_GREEN, middle_left);
        mcText(line.c_str(), 10 - off + span, 75, &fonts::Font2, MC_GREEN, middle_left);
    }
    mcCanvas.clearClipRect();

    // spectrum analyser
    bevel(4, 98, 312, 44, false, MC_LCD_BG);
    for (int y = 104; y < 140; y += 4) mcCanvas.drawFastHLine(8, y, 304, 0x0060);
    for (int i = 0; i < 20; i++) {
        int x = 9 + i * 15, h = bands[i] * 34 / 255;
        for (int y = 0; y < h; y += 3) // segmented like an LCD
            mcCanvas.fillRect(x, 138 - y - 2, 12, 2, y > 26 ? 0xFFE0 : MC_GREEN);
        int pk = peaks[i] * 34 / 255;
        if (pk > 0) mcCanvas.fillRect(x, 138 - pk - 2, 12, 1, MC_PEAK);
    }

    // seek bar
    mcCanvas.fillRoundRect(SEEK_X, SEEK_Y, SEEK_W, 6, 3, MC_TEAL_DK);
    float frac = durationFrames ? std::min(1.0f, (float)playedFrames / durationFrames) : 0;
    int sx = SEEK_X + (int)(SEEK_W * frac);
    if (sx > SEEK_X + 2) mcCanvas.fillRoundRect(SEEK_X, SEEK_Y, sx - SEEK_X, 6, 3, MC_GREEN);
    bevel(sx - 4, SEEK_Y - 3, 9, 12, true, MC_PURPLE);

    // transport buttons
    const int gl[5] = {G_PREV, playing ? G_PAUSE : G_PLAY, G_NEXT, G_SHUF, G_LIST};
    const char *cap[5] = {"PREV", playing ? "PAUSE" : "PLAY", "NEXT", "SHUFFLE", "LIST"};
    for (int i = 0; i < 5; i++) {
        bool on = (i == 1 && playing) || (i == 3 && shuffle);
        bevel(btnX(i), BTN_Y, BTN_W, BTN_H, true, on ? MC_PURPLE : MC_TEAL);
        glyph(gl[i], btnX(i) + BTN_W / 2, BTN_Y + 12, MC_WHITE);
        mcText(cap[i], btnX(i) + BTN_W / 2, BTN_Y + 26, &fonts::Font0, on ? MC_GREEN : MC_WHITE, middle_center);
    }

    // volume
    snprintf(b, sizeof(b), "VOLUME %d%%", volumePct);
    mcText(b, VOL_X, 200, &fonts::Font0, MC_WHITE, middle_left);
    mcCanvas.fillRoundRect(VOL_X, VOL_Y, VOL_W, 8, 4, MC_TEAL_DK);
    mcCanvas.fillRoundRect(VOL_X, VOL_Y, std::max(8, VOL_W * volumePct / 100), 8, 4, MC_TEAL);
    int vx = VOL_X + VOL_W * volumePct / 100;
    bevel(vx - 5, VOL_Y - 5, 11, 18, true, MC_PURPLE);

    // sub-bass haptic switch (EQ-style ON/OFF)
    mcText("SUB-BASS", SUB_X + SUB_W / 2, SUB_Y + 4, &fonts::Font0, MC_WHITE, middle_center);
    bevel(SUB_X, SUB_Y + 12, SUB_W, 24, false, MC_LCD_BG);
    int kx = haptic ? SUB_X + 2 : SUB_X + SUB_W / 2;
    bevel(kx, SUB_Y + 14, SUB_W / 2 - 2, 20, true, haptic ? MC_PURPLE : MC_TEAL_DK);
    mcText("ON", SUB_X + SUB_W / 4 + 1, SUB_Y + 24, &fonts::Font0, haptic ? MC_GREEN : 0x632C, middle_center);
    mcText("OFF", SUB_X + 3 * SUB_W / 4, SUB_Y + 24, &fonts::Font0, haptic ? 0x632C : MC_WHITE, middle_center);
    if (beat) mcCanvas.fillCircle(SUB_X + SUB_W - 6, SUB_Y + 4, 3, MC_GREEN); // felt beat
}

// ------------------------------------------------------------------ playlist window
static void playlistScreen() {
    mcWaitRelease();
    const int rowH = 18, top = 20;
    int scroll = std::max(0, (current - 4) * rowH), scrollAtPress = 0;
    int drawnScroll = -1, drawnCurrent = -2;
    for (;;) {
        serviceAudio(); // keep auto-advance and haptics going while the list is open
        if (scroll != drawnScroll || current != drawnCurrent) { // redraw only on change: bus stays free
            drawnScroll = scroll;
            drawnCurrent = current;
            mcCanvas.fillScreen(MC_CHARCOAL);
            mcCanvas.fillRect(0, 0, MC_W, 16, MC_TEAL_DK);
            mcText("PLAYLIST", 160, 8, &fonts::Font0, MC_WHITE, middle_center);
            bevel(298, 2, 18, 12, true, MC_PURPLE);
            mcText("X", 307, 8, &fonts::Font0, MC_WHITE, middle_center);
            bevel(4, top - 2, 312, MC_H - top - 2, false, MC_LCD_BG);
            mcCanvas.setClipRect(6, top, 308, MC_H - top - 6);
            for (int i = 0; i < (int)tracks.size(); i++) {
                int y = top + 2 + i * rowH - scroll;
                if (y < top - rowH || y > MC_H) continue;
                if (i == current) mcCanvas.fillRect(6, y, 308, rowH - 1, MC_PURPLE);
                String n = String(i + 1) + ". " + baseName(tracks[i]);
                n.toUpperCase();
                mcText(n.c_str(), 10, y + rowH / 2, &fonts::Font2, i == current ? MC_WHITE : MC_GREEN, middle_left);
            }
            if (tracks.empty()) mcText("PUT .MP3 FILES IN /music", 160, 120, &fonts::Font2, MC_GREEN, middle_center);
            mcCanvas.clearClipRect();
            pushRows(0, MC_H);
        }
        mcPollTouch();
        int maxScroll = std::max(0, (int)tracks.size() * rowH - (MC_H - top - 8));
        if (mcTouch.pressed) scrollAtPress = scroll;
        if (mcTouch.down && mcTouch.moved) scroll = constrain(scrollAtPress - (mcTouch.y - mcTouch.startY), 0, maxScroll);
        if (mcTouch.released && !mcTouch.moved) {
            if (mcTouch.startY < 18 && mcTouch.startX > 260) return;
            int i = (mcTouch.startY - top - 2 + scroll) / rowH;
            if (mcTouch.startY >= top && i >= 0 && i < (int)tracks.size()) {
                openTrack(i, true);
                return;
            }
        }
        delay(15);
    }
}

// ------------------------------------------------------------------ main loop
void mcMusicPlayer() {
    if (!busLock) busLock = xSemaphoreCreateMutex();
    if (!fileLock) fileLock = xSemaphoreCreateMutex();
    if (!busLock || !fileLock) return;
    mcWaitRelease();
    mcLoadingBegin("Music");
    mcLoadingFrame(-1, "Finding your songs...");
    tracks.clear();
    scanTracks("/music", 0);
    std::sort(tracks.begin(), tracks.end(), [](const String &a, const String &b) { return a.compareTo(b) < 0; });
    history.clear();
    current = -1;

    Preferences p;
    p.begin("mantismusic", true);
    volumePct = p.getUChar("vol", 70);
    shuffle = p.getBool("shuf", false);
    haptic = p.getBool("hap", true);
    String last = p.getString("last", "");
    p.end();
    if (volumePct > 100) volumePct = 70;

    if (!sink.begin() || !ringAlloc()) {
        sink.release();
        ringFree();
        mcToast("Not enough memory for music", 0xF800, 2000);
        return;
    }
    initAnalyser();
    mcNetStop(); // keep the CPU and SD bus for audio

    // Speaker: output at 44.1 kHz (most MP3s, no resampling), ~93 ms of I2S DMA buffering so a
    // busy moment never reaches the cone, and a speaker task that outranks the decoder.
    M5.Speaker.end();
    auto spkSaved = M5.Speaker.config();
    auto spk = spkSaved;
    spk.sample_rate = 44100;
    spk.dma_buf_len = 512;
    spk.dma_buf_count = 8;
    spk.task_priority = 4;
    M5.Speaker.config(spk);
    M5.Speaker.begin();
    M5.Speaker.setVolume((uint8_t)(volumePct * 255 / 100));
    sink.outLatency = (uint32_t)((uint64_t)spk.dma_buf_len * spk.dma_buf_count * 1000 / spk.sample_rate);
    sink.playEnd = millis();

    sink.dec = &decoder;
    audioQuit = false;
    playing = false;
    pendingOpen = -1;
    pendingCut = false;
    pendingSeek = -1;
    trackEnded = false;
    rHead = 0;
    rTail = 0;
    fileEof = true;
    clearSnaps();
    vib = 0;
    // both on core 0 (UI + display on core 1); decoder outranks the SD reader
    xTaskCreatePinnedToCore(audioLoop, "mcAudio", 12288, nullptr, 3, &audioTask, 0);
    xTaskCreatePinnedToCore(readerLoop, "mcSdRead", 6144, nullptr, 2, &readerTask, 0);

    if (!tracks.empty()) {
        int start = 0;
        for (size_t i = 0; i < tracks.size(); i++)
            if (tracks[i] == last) start = i;
        openTrack(start, false); // ready, press play
    }

    uint8_t bands[20] = {}, peaks[20] = {};
    uint32_t lastFrame = 0, lastSnapAt = 0, lastCtl = 0xFFFFFFFF;
    int scroll = 0;
    bool draggingVol = false, draggingSeek = false, fullPush = true;
    int16_t mono[FFT_N];
    for (;;) {
        mcPollTouch();
        uint32_t now = millis();
        int tx = mcTouch.x, ty = mcTouch.y;
        if (mcTouch.pressed) {
            draggingVol = ty >= VOL_Y - 10 && ty <= VOL_Y + 16 && tx < VOL_X + VOL_W + 8;
            draggingSeek = ty >= SEEK_Y - 8 && ty <= SEEK_Y + 12 && durationFrames > 0;
        }
        if (mcTouch.down && draggingVol) {
            int v = constrain((tx - VOL_X) * 100 / VOL_W, 0, 100);
            if (v != volumePct) {
                volumePct = v;
                M5.Speaker.setVolume((uint8_t)(volumePct * 255 / 100));
            }
        }
        if (mcTouch.released) {
            int sx = mcTouch.startX, sy = mcTouch.startY;
            if (draggingSeek) pendingSeek = constrain((tx - SEEK_X) / (float)SEEK_W, 0.0f, 0.99f);
            else if (!draggingVol && !mcTouch.moved) {
                if (sy < 18 && sx > 260) break; // close
                if (sy >= BTN_Y && sy < BTN_Y + BTN_H) {
                    int i = (sx - 4) / 63;
                    if (i == 0) prevTrack();
                    else if (i == 1 && current >= 0) playing = !playing;
                    else if (i == 2) nextTrack();
                    else if (i == 3) shuffle = !shuffle;
                    else if (i == 4) { playlistScreen(); fullPush = true; }
                } else if (sx >= SUB_X && sy >= SUB_Y) haptic = !haptic;
                else if (sy >= 60 && sy < 95) { playlistScreen(); fullPush = true; } // tap the track title
            }
            draggingVol = draggingSeek = false;
        }
        serviceAudio();

        if (now - lastFrame >= 33) { // ~30 fps
            lastFrame = now;
            // analyser: FFT of the buffer being heard right now (runs here, never in the audio task)
            int si = playing ? pickSnap(now) : -1;
            if (si >= 0) {
                uint32_t at = snaps[si].at;
                if (at != lastSnapAt) {
                    lastSnapAt = at;
                    memcpy(mono, snaps[si].mono, sizeof(mono));
                    analyse(mono, bands);
                }
            } else {
                memset(bands, 0, sizeof(bands));
                lastSnapAt = 0;
            }
            for (int i = 0; i < 20; i++) peaks[i] = bands[i] > peaks[i] ? bands[i] : (peaks[i] > 5 ? peaks[i] - 5 : 0);
            bool beat = haptic && playing && bassNow(now) > 70;
            scroll += 1;
            drawPlayer(now, bands, peaks, scroll, beat);
            // push only what changed: the live top area every frame, the controls when they change
            uint32_t ctl = (uint32_t)playing | (uint32_t)shuffle << 1 | (uint32_t)haptic << 2 | (uint32_t)beat << 3 |
                           (uint32_t)volumePct << 8;
            if (fullPush) {
                pushRows(0, MC_H);
                fullPush = false;
                lastCtl = ctl;
            } else {
                pushRows(LIVE_Y0, LIVE_Y1);
                if (ctl != lastCtl) {
                    pushRows(LIVE_Y1, MC_H);
                    lastCtl = ctl;
                }
            }
        }
        delay(5);
    }

    // shut down: stop audio, release the speaker and motor, remember settings
    playing = false;
    audioQuit = true;
    for (int i = 0; i < 300 && (audioTask || readerTask); i++) delay(10);
    lockBus();
    if (audioFile) audioFile.close();
    unlockBus();
    M5.Power.setVibration(0);
    vib = 0;
    M5.Speaker.stop();
    M5.Speaker.end();
    M5.Speaker.config(spkSaved);
    sink.release();
    ringFree();
    p.begin("mantismusic", false);
    p.putUChar("vol", volumePct);
    p.putBool("shuf", shuffle);
    p.putBool("hap", haptic);
    if (current >= 0) p.putString("last", tracks[current]);
    p.end();
    mcWaitRelease();
    mcNetStart();
}
