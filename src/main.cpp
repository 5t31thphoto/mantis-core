// MantisCore by BasaltSoftworks - M5Stack Core2
//
// Boot flow (same as Launcher): the keyboot bootloader starts MantisCore on power-on / reset button,
// and starts the selected app directly when an app reboots itself. MantisCore shows its splash for
// 1 s (+ bootTimer s); a tap opens the home screen, otherwise the last app is started again.
#include "engine/app_registry.h"
#include "engine/globals.h"
#include "engine/sd_install.h"
#include "engine/settings.h"
#include "engine/utils.h"
#include "ui/mantis.h"
#include "ui/mantis_internal.h"
#include <LittleFS.h>
#include <M5Unified.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <nvs_flash.h>

#if defined(SET_LOOP_TASK_STACK_SIZE)
SET_LOOP_TASK_STACK_SIZE(16384) // same as Launcher (no semicolon: the macro is a function definition): the install engine runs on the loop task
#endif

// True only when otadata selects an installed OTA app, i.e. a software reboot will really start an
// app. After "Remove from device" the selection is cleared, and rebooting would just come back here.
static bool bootSelectionIsApp() {
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    const esp_partition_t *running = esp_ota_get_running_partition();
    return boot && running && boot->address != running->address && boot->type == ESP_PARTITION_TYPE_APP &&
           boot->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MIN && boot->subtype < ESP_PARTITION_SUBTYPE_APP_OTA_MAX;
}

void setup() {
    Serial.begin(115200);
    auto cfg = M5.config();
    cfg.clear_display = true;
    M5.begin(cfg);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    getFromNVS();
    saveIntoNVS(); // make sure the bootloader keys exist

    mantisSplashBegin();
    setupSdCard();

    uint32_t start = millis();
    uint32_t window = 1000 + (bootToApp ? bootTimer * 1000UL : 0);
    while (millis() - start < window) {
        mantisSplashFrame();
        M5.update();
        if (M5.Touch.getCount() > 0) {
            mcWaitRelease();
            return; // -> loop() -> home screen
        }
        delay(10);
    }
    if (launcherBootCurrentApp() && bootSelectionIsApp()) {
        M5.Display.setBrightness(0);
        releaseHeapObjectsAndReboot(); // software reset: bootloader starts the last app
    }
}

void loop() { mantisRun(); } // never returns
