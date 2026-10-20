// MantisCore SD installer. Install path taken verbatim from Launcher's sd_functions.cpp
// (flashRawFromSd .. updateFromSD); prompts removed: saved data is always restored and the
// app's own SPIFFS payload is always installed.
#include "sd_install.h"
#include "backup_manager.h"
#include "display.h"
#include "esp_log.h"
#include "idf/idf_update.h"
#include "idf/launcher_platform.h"
#include "install_shared.h"
#include "littlefs_patch.h"
#include "partition_install_layout.h"
#include "partition_table_model.h"
#include "ram_profile.h"
#include "settings.h"
#include "utils.h"
#include <algorithm>
#include <esp_app_format.h>
#include <esp_image_format.h>
#include <esp_partition.h>
#include "globals.h"
#include <memory>

// MantisCore polls its own input, so there is no input task to pause.
static inline void pauseSdInstallInput() {}
static inline void resumeSdInstallInput() {}

// Core2: SD shares the LCD SPI bus (SCK 18, MISO 38, MOSI 23, CS 4), same as Mantis Gallery.
bool setupSdCard() {
    if (sdcardMounted) return true;
    SPI.begin(18, 38, 23, 4);
    sdcardMounted = SD.begin(4, SPI, 25000000) || SD.begin(4, SPI, 4000000);
    launcherConsolePrintln(sdcardMounted ? "SDCARD mounted successfully" : "Failed to mount SDCARD");
    return sdcardMounted;
}

static String installedAppNameFromPath(const String &path) { return launcherInstallAppDisplayName(path); }

static bool flashRawFromSd(
    File &file, uint32_t sourceOffset, size_t imageSize, const LauncherPartitionEntry &target, bool appImage
) {
    if (!file.seek(sourceOffset)) {
        launcherConsolePrintf(
            "flashRawFromSd: initial seek to 0x%08X failed, retrying\n", (unsigned)sourceOffset
        );
        return false;
    }
    progressHandler(0, imageSize);
    if (!launcherRawUpdateBegin(target.offset, target.size, imageSize, appImage)) {
        launcherConsolePrintf(
            "flashRawFromSd: launcherRawUpdateBegin failed target=0x%08X/0x%08X image=0x%08X (%s)\n",
            (unsigned)target.offset,
            (unsigned)target.size,
            (unsigned)imageSize,
            launcherUpdateLastErrorName()
        );
        return false;
    }

    constexpr size_t bufferSize = 4096;
    std::unique_ptr<uint8_t[]> buf(new (std::nothrow) uint8_t[bufferSize]);
    if (!buf) {
        launcherRawUpdateEnd();
        return false;
    }

    size_t written = 0;
    while (written < imageSize) {
        size_t toRead = min(bufferSize, imageSize - written);
        int bytesRead = file.readBytes(reinterpret_cast<char *>(buf.get()), toRead);
        if (bytesRead <= 0) {
            launcherDelayMs(20);
            if (!file.seek(sourceOffset + written)) {
                launcherConsolePrintf(
                    "SD read failed at offset 0x%08X (re-seek failed)\n", (unsigned)(sourceOffset + written)
                );
                launcherRawUpdateEnd();
                return false;
            }
            bytesRead = file.readBytes(reinterpret_cast<char *>(buf.get()), toRead);
        }
        if (bytesRead <= 0) {
            launcherConsolePrintf("SD read failed at offset 0x%08X\n", (unsigned)(sourceOffset + written));
            launcherRawUpdateEnd();
            return false;
        }
        if (launcherRawUpdateWrite(buf.get(), bytesRead) != static_cast<size_t>(bytesRead)) {
            launcherConsolePrintf(
                "Flash write failed at partition offset 0x%08X (%s)\n",
                (unsigned)written,
                launcherUpdateLastErrorName()
            );
            launcherRawUpdateEnd();
            return false;
        }
        written += bytesRead;
        progressHandler(written, imageSize);
        launcherDelayMs(1);
    }
    if (!launcherRawUpdateEnd()) {
        launcherConsolePrintf(
            "flashRawFromSd: launcherRawUpdateEnd failed (%s)\n", launcherUpdateLastErrorName()
        );
        return false;
    }

    if (!appImage) {
        String patchError;
        if (!launcherPatchReducedLittlefsSuperblocks(target, &patchError)) {
            launcherConsolePrintf(
                "LittleFS patch failed after SD copy label=%s offset=0x%08X size=0x%08X: %s\n",
                target.label,
                target.offset,
                target.size,
                patchError.c_str()
            );
            return false;
        }
    }

    return true;
}

static bool readSdBytes(File &file, uint32_t offset, void *buffer, size_t len) {
    if (!file.seek(offset)) return false;
    return file.readBytes(reinterpret_cast<char *>(buffer), len) == len;
}

static uint32_t readLe32(const uint8_t *bytes) {
    return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
}

static String readPartitionLabel(const uint8_t *entry) {
    char label[17] = {0};
    memcpy(label, entry + 12, 16);
    label[16] = '\0';
    return String(label);
}

static uint32_t
boundedSdPartitionPayload(File &file, uint32_t offset, uint32_t declaredSize, uint32_t maxSize) {
    if (offset == 0 || file.size() <= offset || declaredSize == 0) return 0;
    uint32_t availableSize = file.size() - offset;
    return launcherPartitionBoundedPayloadSize(declaredSize, 0, maxSize, availableSize);
}

// A partition table entry can be present without actually carrying payload (e.g. the
// image was built with an empty/erased data partition). Treat it as empty when its
// first bytes are all-0xFF or all-0x00, so callers don't size/copy for data that isn't there.
static bool sdPartitionIsEmpty(File &file, uint32_t offset) {
    if (offset == 0 || file.size() <= offset) return true;
    uint8_t buffer[16];
    uint32_t len = std::min<uint32_t>(sizeof(buffer), file.size() - offset);
    if (!readSdBytes(file, offset, buffer, len)) return true;
    bool allFF = true, allZero = true;
    for (uint32_t i = 0; i < len; ++i) {
        if (buffer[i] != 0xFF) allFF = false;
        if (buffer[i] != 0x00) allZero = false;
    }
    return allFF || allZero;
}

static bool measureSdEspImage(File &file, uint32_t imageOffset, uint32_t &imageSize) {
    esp_image_header_t header;
    if (!readSdBytes(file, imageOffset, &header, sizeof(header))) return false;
    if (header.magic != ESP_IMAGE_HEADER_MAGIC || header.segment_count == 0 ||
        header.segment_count > ESP_IMAGE_MAX_SEGMENTS) {
        return false;
    }

    uint32_t cursor = imageOffset + sizeof(header);
    const uint32_t fileSize = file.size();
    if (cursor > fileSize) return false;

    for (uint8_t i = 0; i < header.segment_count; ++i) {
        uint8_t segmentHeader[sizeof(esp_image_segment_header_t)];
        if (!readSdBytes(file, cursor, segmentHeader, sizeof(segmentHeader))) return false;
        const uint32_t segmentSize = readLe32(segmentHeader + 4);
        cursor += sizeof(segmentHeader);
        if (segmentSize > fileSize || cursor > fileSize - segmentSize) return false;
        cursor += segmentSize;
    }

    uint32_t end = launcherAlignUp(cursor, 16) + 1;
    if (header.hash_appended) end += ESP_IMAGE_HASH_LEN;
    end = launcherAlignUp(end, 16);
    if (end <= imageOffset || end > fileSize) return false;

    imageSize = end - imageOffset;
    return true;
}

static uint32_t effectiveSdAppSize(File &file, uint32_t appOffset, uint32_t fallbackSize) {
    uint32_t measuredSize = 0;
    if (measureSdEspImage(file, appOffset, measuredSize)) {
        if (fallbackSize == 0 || measuredSize < fallbackSize) {
            launcherConsolePrintf(
                "Measured SD app image at 0x%06X: 0x%06X (%u bytes), fallback was 0x%06X\n",
                appOffset,
                measuredSize,
                measuredSize,
                fallbackSize
            );
            return measuredSize;
        }
    }
    return fallbackSize;
}

static bool installFromSdDynamic(
    File &file, const String &path, uint32_t appSize, uint32_t appOffset,
    std::vector<LauncherInstallDataPartition> &dataPartitions
) {
    String error;
    LauncherPartitionTable table;
    if (!launcherPartitionReadCurrent(table, &error)) {
        displayError(error.length() ? error : "Partition read failed");
        return false;
    }

    if (appSize == 0 || appOffset + appSize > file.size()) {
        displayError("Invalid app image");
        return false;
    }

    String appLabel = launcherPartitionNextAppLabel(table, installedAppNameFromPath(path));
    LauncherPartitionEntry appEntry;

    if (!launcherSelectInstallLayout(table, appSize, appLabel, dataPartitions, appEntry, error)) {
        launcherConsolePrintf("SD install layout failed: %s\n", error.c_str());
        displayError(error.length() ? error : "No install space");
        return false;
    }
    if (!launcherPartitionValidate(table, &error)) {
        displayError(error.length() ? error : "Invalid table");
        return false;
    }

    String appNum = generateAppNum(path);
    bool shouldRestore = false;
    BackupInstallInfo prevInstall = loadInstalledFromConfig(appNum);
    if (!prevInstall.appNum.isEmpty()) {
        std::vector<String> restorable;
        for (const auto &bp : prevInstall.partitions) {
            if (!bp.lastBackupPath.isEmpty() && SDM.exists(bp.lastBackupPath)) restorable.push_back(bp.label);
        }
        if (!restorable.empty()) shouldRestore = true; // MantisCore: always bring back saved progress
    }

    pauseSdInstallInput();
    bool success = false;
    prog_handler = 0;
    if (!flashRawFromSd(file, appOffset, appSize, appEntry, true)) {
        displayError(String("APP: ") + launcherUpdateLastErrorName());
        goto DONE;
    }

    for (const auto &dp : dataPartitions) {
        if (!dp.hasEntry || dp.copySize == 0) continue;
        if (shouldRestore) {
            bool willRestore = false;
            for (const auto &bp : prevInstall.partitions) {
                if (bp.label == dp.label && !bp.lastBackupPath.isEmpty() && SDM.exists(bp.lastBackupPath)) {
                    willRestore = true;
                    break;
                }
            }
            if (willRestore) continue;
        }
        const char *typeStr = dp.subtype == 0x81 ? "FAT" : dp.subtype == 0x83 ? "LittleFS" : "SPIFFS";
        displayRedStripe(String("Installing ") + typeStr);
        prog_handler = 1;
        const uint32_t copySize = dp.copySize > dp.entry.size ? dp.entry.size : dp.copySize;
        if (!flashRawFromSd(file, dp.sourceOffset, copySize, dp.entry, false)) {
            displayError(String(typeStr) + ": " + launcherUpdateLastErrorName());
            goto DONE;
        }
    }

    displayRedStripe("Writing table");
    if (!launcherPartitionWriteGeneratedTable(table, &error)) {
        displayError(error.length() ? error : "Table failed");
        goto DONE;
    }

    displayRedStripe("Setting boot");
    if (!launcherPartitionSetOtaBoot(table, appEntry.subtype, &error)) {
        displayError(error.length() ? error : "Boot failed");
        goto DONE;
    }

    if (shouldRestore) {
        for (const auto &bp : prevInstall.partitions) {
            if (!bp.lastBackupPath.isEmpty() && SDM.exists(bp.lastBackupPath)) {
                // Prefer direct write using the freshly written partition table entry,
                // since the IDF partition cache still reflects the old table.
                bool restored = false;
                for (const auto &dp : dataPartitions) {
                    if (dp.label == bp.label && dp.hasEntry) {
                        restored = restorePartitionFromBackupDirect(
                            bp.label.c_str(), bp.lastBackupPath.c_str(), dp.entry.offset, dp.entry.size
                        );
                        break;
                    }
                }
                if (!restored) {
                    restored = restorePartitionFromBackup(bp.label.c_str(), bp.lastBackupPath.c_str());
                }
                if (!restored) { log_w("[Restore] Failed: %s", bp.label.c_str()); }
            }
        }
    }

    {
        std::vector<String> fatLabels;
        String registeredSpiffsLabel;
        for (const auto &dp : dataPartitions) {
            if (!dp.hasEntry) continue;
            if (dp.subtype == 0x81) fatLabels.push_back(dp.label);
            else registeredSpiffsLabel = dp.label;
        }
        launcherSaveInstalledAppMetadata(
            table, appEntry, path, installedAppNameFromPath(path), fatLabels, registeredSpiffsLabel
        );
        saveIntoNVS();

        BackupInstallInfo bkInfo;
        bkInfo.appNum = appNum;
        bkInfo.sdFilepath = path;
        bkInfo.appName = installedAppNameFromPath(path);
        for (const auto &dp : dataPartitions) {
            if (!dp.hasEntry) continue;
            BackupPartitionInfo part;
            part.label = dp.label;
            part.type = dp.subtype == 0x81 ? "FAT" : dp.subtype == 0x83 ? "LittleFS" : "SPIFFS";
            for (const auto &bp : prevInstall.partitions) {
                if (bp.label == part.label && !bp.lastBackupPath.isEmpty()) {
                    part.lastBackupPath = bp.lastBackupPath;
                    break;
                }
            }
            bkInfo.partitions.push_back(part);
        }
        saveInstalledToConfig(bkInfo);
    }

    success = true;

DONE:
    resumeSdInstallInput();
    return success;
}

/***************************************************************************************
** Function name: updateFromSD
** Description:   this function analyse the .bin and calls installFromSdDynamic
***************************************************************************************/
void updateFromSD(const String &path) {
    uint8_t partitionEntry[LAUNCHER_PARTITION_ENTRY_SIZE];
    uint32_t app_size = 0;
    uint32_t app_offset = 0;
    std::vector<LauncherInstallDataPartition> dataPartitions;

    File file = SDM.open(path);

    if (!file) goto Exit;
    if (!file.seek(0x8000)) goto Exit;
    file.read(partitionEntry, 16);

    if (partitionEntry[0] != 0xAA || partitionEntry[1] != 0x50 || partitionEntry[2] != 0x01) {
        app_size = effectiveSdAppSize(file, 0, file.size());
        if (!installFromSdDynamic(file, path, app_size, 0, dataPartitions)) { goto Exit; }
        file.close();

        return (void)releaseHeapObjectsAndReboot();
    } else {
        if (!file.seek(0x8000)) goto Exit;
        for (int i = 0; i < LAUNCHER_PARTITION_TABLE_SIZE; i += LAUNCHER_PARTITION_ENTRY_SIZE) {
            if (!file.seek(0x8000 + i)) goto Exit;
            if (file.read(partitionEntry, LAUNCHER_PARTITION_ENTRY_SIZE) != LAUNCHER_PARTITION_ENTRY_SIZE)
                goto Exit;
            if (partitionEntry[0] == 0xEB && partitionEntry[1] == 0xEB) break;
            if (partitionEntry[0] == 0xFF && partitionEntry[1] == 0xFF) break;

            if (partitionEntry[0x02] == 0x00 &&
                (partitionEntry[0x03] == 0x00 || partitionEntry[0x03] == 0x10 ||
                 partitionEntry[0x03] == 0x20) &&
                app_size == 0) {
                uint32_t declared_app_size = readLe32(partitionEntry + 0x08);
                app_offset = readLe32(partitionEntry + 0x04);
                if (file.size() < (declared_app_size + app_offset)) {
                    app_size = file.size() - app_offset;
                    launcherConsolePrintf(
                        "Using SD app tail size at 0x%06X: 0x%06X (%u bytes), declared partition was "
                        "0x%06X\n",
                        app_offset,
                        app_size,
                        app_size,
                        declared_app_size
                    );
                } else {
                    app_size = declared_app_size;
                    app_size = effectiveSdAppSize(file, app_offset, app_size);
                }
            }

            if (partitionEntry[0x02] == 0x01 && (partitionEntry[3] == 0x82 || partitionEntry[3] == 0x83)) {
                LauncherInstallDataPartition dp;
                dp.subtype = partitionEntry[3];
                dp.sourceOffset = readLe32(partitionEntry + 0x04);
                const uint32_t declaredSize = readLe32(partitionEntry + 0x08);
                String declaredLabel = readPartitionLabel(partitionEntry);
                dp.label = declaredLabel.isEmpty() ? "spiffs" : declaredLabel;
                const bool partitionEmpty = sdPartitionIsEmpty(file, dp.sourceOffset);
                // accept data partitions as they are if not "spiffs", unless there's no actual
                // payload to justify the declared size
                if (partitionEmpty && declaredSize <= LAUNCHER_DEFAULT_SPIFFS_THRESHOLD) {
                    dp.partitionSize = LAUNCHER_DEFAULT_SPIFFS_SIZE;
                } else if (dp.label != "spiffs" && declaredSize > LAUNCHER_DEFAULT_SPIFFS_SIZE) {
                    dp.partitionSize = declaredSize;
                } else if (declaredSize > LAUNCHER_DEFAULT_SPIFFS_THRESHOLD) {
                    dp.partitionSize = LAUNCHER_INSTALL_USE_REMAINING_SPIFFS_SIZE;
                } else {
                    dp.partitionSize = LAUNCHER_DEFAULT_SPIFFS_SIZE;
                }
                dp.copySize = partitionEmpty
                                  ? 0
                                  : boundedSdPartitionPayload(
                                        file,
                                        dp.sourceOffset,
                                        declaredSize,
                                        dp.partitionSize == LAUNCHER_INSTALL_USE_REMAINING_SPIFFS_SIZE
                                            ? declaredSize
                                            : dp.partitionSize
                                    );
                if (file.size() < dp.sourceOffset) {
                    dp.copySize = 0;
                    launcherConsolePrintf(
                        "Found SPIFFS table entry without payload: create 0x%06X, copy 0\n", dp.partitionSize
                    );
                } else if (partitionEmpty) {
                    launcherConsolePrintf(
                        "Found empty %s partition at 0x%06X: create 0x%06X, copy 0\n",
                        dp.label.c_str(),
                        dp.sourceOffset,
                        dp.partitionSize
                    );
                }
                dataPartitions.push_back(dp);
            }

            if (partitionEntry[0x02] == 0x01 && partitionEntry[3] == 0x81) {
                LauncherInstallDataPartition dp;
                dp.subtype = 0x81;
                dp.label = readPartitionLabel(partitionEntry);
                uint32_t declaredSize = readLe32(partitionEntry + 0x08);
                dp.sourceOffset = readLe32(partitionEntry + 0x04);
                uint32_t availableSize =
                    dp.sourceOffset != 0 && file.size() > dp.sourceOffset ? file.size() - dp.sourceOffset : 0;
                LauncherPartitionPayloadPlan payload =
                    launcherPartitionFatPayloadPlan(dp.label.c_str(), declaredSize, 0, availableSize);
                dp.partitionSize = payload.partitionSize;
                dp.copySize = payload.copySize;
                dataPartitions.push_back(dp);
                launcherConsolePrintf(
                    "Found FAT %s at 0x%06X: create 0x%06X, copy 0x%06X of declared 0x%06X\n",
                    dp.label.c_str(),
                    dp.sourceOffset,
                    payload.partitionSize,
                    payload.copySize,
                    declaredSize
                );
            }
        }

        prog_handler = 0; // Install flash update

        log_i("Appsize: %d", app_size);
        log_i("Data partitions: %d", dataPartitions.size());

        if (!installFromSdDynamic(file, path, app_size, app_offset, dataPartitions)) { goto Exit; }
        displayMsg("Complete");

        return (void)releaseHeapObjectsAndReboot();
    }
Exit:
    displayError("Update Error.");
}

