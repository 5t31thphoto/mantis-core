// MantisCore engine glue: globals, NVS settings shared with the keyboot bootloader,
// reboot, and the display/menu hooks the engine calls (drawn by the MantisCore UI).
#include "display.h"
#include "globals.h"
#include "idf/launcher_platform.h"
#include "nvs_helpers.h"
#include "settings.h"
#include "utils.h"
#include <WiFi.h>
#include <nvs.h>

int prog_handler = 0;
bool sdcardMounted = false;
String lastInstalledApp;
bool bootToApp = true;   // same defaults as Launcher
uint8_t bootTimer = 4;
bool autoBackup = true;
bool DDLB = false;
int LauncherOnKey = -1;
bool LauncherKeyLvl = false;

// Same namespace and keys Launcher uses; the bootloader reads DDLB / LauncherOnKey / LauncherKeyLvl.
bool saveIntoNVS() {
    lnvs::Handle nvsHandle("launcher", true);
    if (!nvsHandle) return false;
    nvs_handle_t h = nvsHandle.raw();
    bool ok = true;
    ok &= lnvs::setBool(h, "bootToApp", bootToApp);
    ok &= nvs_set_u8(h, "bootTimer", bootTimer) == ESP_OK;
    ok &= lnvs::setBool(h, "DDLB", DDLB);
    ok &= lnvs::setInt(h, "LauncherOnKey", LauncherOnKey);
    ok &= lnvs::setBool(h, "LauncherKeyLvl", LauncherKeyLvl);
    ok &= lnvs::setBool(h, "autoBackup", autoBackup);
    ok &= lnvs::setString(h, "last_app", lastInstalledApp.c_str());
    nvsHandle.commit();
    return ok;
}

bool getFromNVS() {
    lnvs::Handle nvsHandle("launcher", false);
    if (!nvsHandle) return false;
    nvs_handle_t h = nvsHandle.raw();
    lnvs::getBool(h, "bootToApp", bootToApp);
    nvs_get_u8(h, "bootTimer", &bootTimer);
    lnvs::getBool(h, "autoBackup", autoBackup);
    lastInstalledApp = lnvs::getString(h, "last_app", 64);
    if (bootTimer > 5) bootTimer = 4;
    return true;
}

bool releaseHeapObjectsAndReboot(void) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    SD.end();
    launcherConsoleFlush();
    ESP.restart();
    return true;
}

// ---- UI hooks (implemented in src/ui/mantis_core.cpp / mantis_settings.cpp)
void mantisEngineStatus(const String &text);
void mantisEngineError(const String &text);
void mantisEngineMessage(const String &text);
void mantisEngineProgress(size_t progress, size_t total);
int mantisEngineChoose(std::vector<Option> &options);

void displayRedStripe(const String &text, uint16_t, uint16_t, bool) { mantisEngineStatus(text); }
void displayMsg(String txt, bool) { mantisEngineMessage(txt); }
void displayError(String txt, bool) { mantisEngineError(txt); }
void progressHandler(size_t progress, size_t total) {
    vTaskDelay(pdMS_TO_TICKS(2)); // keep yielding while flashing, as Launcher does
    mantisEngineProgress(progress, total);
}
int loopOptions(std::vector<Option> &options) { return mantisEngineChoose(options); }
