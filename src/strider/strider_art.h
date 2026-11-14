// Mantis Strider: turns a recorded walk into a 320x240 print. Plain C++ (previewable on a PC).
#pragma once
#include "strider_walk.h"

namespace strider {

enum Style : uint8_t {
    STYLE_TOPO = 0, // sound becomes terrain: a contour map of what you heard, with your trail on it
    STYLE_RISO,     // two-ink risograph: brush-stroke route, halftone soundscape, misregistered overprint
    STYLE_LIGHT,    // long exposure: light streaks flowing off the route, coloured by time along it
    STYLE_STARS,    // star chart: Wi-Fi networks you passed become stars, joined into a constellation
    STYLE_COUNT
};

const char *styleName(int style);      // "TOPO", ...
int styleDefault(const Walk &w);       // the style that suits this walk (time of day, places)
size_t artMemBytes();                  // scratch memory artRender needs (about 1 MB; PSRAM on device)

// Renders the print. out565: 320*240 native RGB565 (== the .mg pixel order on a little-endian CPU).
void artRender(const Walk &w, int style, uint16_t *out565, void *mem);

// Called now and then during a render (a second or two on the device), e.g. to feed a watchdog.
void artSetYield(void (*fn)());

// Small 3x5 pixel font used on the prints, exposed for the app's own labels.
void artText565(uint16_t *buf, int x, int y, const char *s, int scale, uint16_t color);

} // namespace strider
