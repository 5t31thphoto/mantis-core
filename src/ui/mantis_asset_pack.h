// MantisCore art that lives on the SD card (/mantiscore/assets.bin) instead of in flash.
#pragma once
#include <stdint.h>

struct McAssetEntry {
    const char *name;
    const void **ptr;   // filled by mcAssetsLoad(), stays nullptr if the pack is missing or stale
    uint32_t bytes;     // expected size, checked against the pack
};
extern const McAssetEntry mcAssetTable[];
extern const int mcAssetCount;
extern const uint32_t mcAssetPackId;

bool mcAssetsLoad(); // call once the SD card is mounted; false = drawn fallbacks are used
