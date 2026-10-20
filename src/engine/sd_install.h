#pragma once
#include <Arduino.h>
#include <SD.h>
#include <SPI.h>

bool setupSdCard();
void updateFromSD(const String &path); // installs a merged .bin from SD, then reboots into it
