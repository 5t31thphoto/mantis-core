// MantisCore engine display hooks: engine status/progress goes to the MantisCore loading screen.
#pragma once
#include "globals.h"

void displayRedStripe(const String &text, uint16_t fgcolor = 0xFFFF, uint16_t bgcolor = 0xF800, bool keepAwake = false);
void displayMsg(String txt, bool waitKeyPress = false);
void displayError(String txt, bool waitKeyPress = false);
void progressHandler(size_t progress, size_t total);
int loopOptions(std::vector<Option> &options); // shown as a MantisCore action sheet
