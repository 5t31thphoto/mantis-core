// MantisCore engine globals: the handful of Launcher globals the install/backup engine uses.
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h> // as in Launcher's globals.h: lets PlatformIO find LittleFS for M5GFX
#include <SD.h>
#include <WString.h>
#include <cstdint>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <functional>
#include <vector>
#include "pre_compiler.h"

#define SDM SD
#define SDM_SD
#ifndef NO_COLOR
#define NO_COLOR 0x0001
#endif

struct Option {
    String label;
    std::function<void()> operation;
    uint16_t color;
    Option(String lbl = "", const std::function<void()> &op = nullptr, uint16_t color = NO_COLOR)
        : label(lbl), operation(op), color(color) {}
};

extern int prog_handler;         // 0 - Flash, 1 - SPIFFS
extern bool sdcardMounted;
extern String lastInstalledApp;  // display name of the last app booted
extern bool bootToApp;           // boot screen auto-starts the last app
extern uint8_t bootTimer;        // extra seconds on the boot screen
extern bool autoBackup;
// Read by the keyboot bootloader from NVS namespace "launcher" (see saveIntoNVS()).
extern bool DDLB;
extern int LauncherOnKey;
extern bool LauncherKeyLvl;
