// MantisCore backups: copies each app's data partitions (SPIFFS/LittleFS/FAT) to /backups/<app>/<label>.bin.
// Runs a few milliseconds at a time from the home-screen loop, so the UI stays responsive and the SD/LCD
// SPI bus is never shared between tasks. Unchanged partitions (same CRC as last backup) are skipped.
#include "../engine/app_registry.h"
#include "../engine/backup_manager.h"
#include "../engine/globals.h"
#include "mantis_internal.h"
#include <Preferences.h>
#include <SD.h>
#include <esp_flash.h>
#include <esp_partition.h>
#include <esp_heap_caps.h>
#include <esp_rom_crc.h>


struct BkJob {
    String appNum, appName, label, type;
    uint32_t addr = 0, size = 0;
    bool force = false;
};
enum class BkPhase { Idle, Hash, Copy };

static std::vector<BkJob> jobs;
static BkPhase phase = BkPhase::Idle;
static uint32_t pos = 0, crc = 0;
static File out;
static uint8_t *chunk = nullptr;
static const size_t CHUNK = 16384;

static String crcKey(const BkJob &j) { return "c" + j.appNum.substring(0, 8) + j.label.substring(0, 6); }
static String jobDir(const BkJob &j) {
    String n = j.appName;
    for (size_t i = 0; i < n.length(); i++)
        if (!isalnum((unsigned char)n[i]) && n[i] != '-' && n[i] != '_') n.setCharAt(i, '_');
    return "/backups/" + n;
}
static String jobPath(const BkJob &j) { return jobDir(j) + "/" + j.label + ".bin"; }

static void queueApp(const String &label, const String &name, bool onlyNew, bool force) {
    String appNum = loadAppNumForLabel(label.c_str());
    if (appNum.isEmpty()) return;
    BackupInstallInfo info = loadInstalledFromConfig(appNum);
    Preferences p;
    p.begin("mcbk", true);
    for (const BackupPartitionInfo &bp : info.partitions) {
        const esp_partition_t *part =
            esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, bp.label.c_str());
        if (!part) continue;
        BkJob j;
        j.appNum = appNum;
        j.appName = name;
        j.label = bp.label;
        j.type = bp.type;
        j.addr = part->address;
        j.size = part->size;
        j.force = force;
        if (onlyNew && p.isKey(crcKey(j).c_str())) continue;
        jobs.push_back(j);
    }
    p.end();
}

void mcBackupQueueAfterApp() {
    if (!sdcardMounted) return;
    for (const LauncherAppMetadata &app : launcherListInstalledApps()) {
        bool wasRunning = app.name == lastInstalledApp;
        queueApp(app.label, app.name, !wasRunning, false); // last app always, others only if never saved
    }
}

bool mcBackupBusy() { return !jobs.empty(); }

static void finishJob(bool ok) {
    if (out) out.close();
    BkJob &j = jobs.front();
    String path = jobPath(j);
    if (phase == BkPhase::Copy) {
        if (ok) {
            SD.remove(path);
            ok = SD.rename(path + ".tmp", path);
        } else {
            SD.remove(path + ".tmp");
        }
    }
    if (ok) {
        if (phase == BkPhase::Copy) updateInstalledBackupPath(j.appNum, j.label, path);
        Preferences p;
        p.begin("mcbk", false);
        p.putUInt(crcKey(j).c_str(), crc);
        p.end();
    }
    jobs.erase(jobs.begin());
    phase = BkPhase::Idle;
}

void mcBackupStep(uint32_t budgetMs) {
    if (jobs.empty()) return;
    if (!chunk) chunk = (uint8_t *)heap_caps_malloc(CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!chunk) return;
    uint32_t t0 = millis();
    while (!jobs.empty() && millis() - t0 < budgetMs) {
        BkJob &j = jobs.front();
        if (phase == BkPhase::Idle) {
            phase = BkPhase::Hash;
            pos = 0;
            crc = 0;
        }
        size_t n = min((uint32_t)CHUNK, j.size - pos);
        if (esp_flash_read(nullptr, chunk, j.addr + pos, n) != ESP_OK) {
            finishJob(false);
            continue;
        }
        if (phase == BkPhase::Hash) {
            crc = esp_rom_crc32_le(crc, chunk, n);
            pos += n;
            if (pos >= j.size) {
                Preferences p;
                p.begin("mcbk", true);
                bool same = p.getUInt(crcKey(j).c_str(), 0) == crc && SD.exists(jobPath(j));
                p.end();
                if (same && !j.force) {
                    finishJob(true); // nothing changed since the last backup
                    continue;
                }
                SD.mkdir("/backups");
                SD.mkdir(jobDir(j));
                out = SD.open(jobPath(j) + ".tmp", FILE_WRITE);
                if (!out) {
                    finishJob(false);
                    continue;
                }
                phase = BkPhase::Copy;
                pos = 0;
            }
        } else {
            if (out.write(chunk, n) != n) {
                finishJob(false);
                continue;
            }
            pos += n;
            if (pos >= j.size) finishJob(true);
        }
    }
}

void mcBackupAbort() {
    if (jobs.empty()) return;
    if (out) out.close();
    if (phase == BkPhase::Copy) SD.remove(jobPath(jobs.front()) + ".tmp");
    jobs.clear();
    phase = BkPhase::Idle;
}

static void runBlocking(const String &title) {
    if (jobs.empty()) return;
    mcLoadingBegin(title);
    uint64_t total = 0;
    for (const BkJob &j : jobs) total += (uint64_t)j.size * 2; // hash + copy
    uint64_t base = 0;
    while (!jobs.empty()) {
        size_t before = jobs.size();
        uint32_t jsize = jobs.front().size;
        mcBackupStep(40);
        uint64_t done = base + (phase == BkPhase::Copy ? jsize + pos : pos);
        if (jobs.size() != before) base += (uint64_t)jsize * 2;
        mcLoadingFrame(total ? (float)((double)min(done, total) / (double)total) : -1, "Saving your progress...");
    }
}

bool mcBackupAppBlocking(const String &label, const String &appName) {
    mcBackupAbort();
    queueApp(label, appName, false, false);
    runBlocking(appName);
    return true;
}

void mcBackupFinishBlocking(const String &title) { runBlocking(title); }
