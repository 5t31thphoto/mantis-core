// MantisCore: the only Launcher build constant the engine uses.
// (Launcher's version also defined display/pin macros such as FP, RES, ROTATION and TFT_*, which
// could collide with M5GFX identifiers; MantisCore has no use for them.)
#ifndef _PRE_COMPILER
#define _PRE_COMPILER

#ifndef LAUNCHER_DEFAULT_SPIFFS_THRESHOLD
#define LAUNCHER_DEFAULT_SPIFFS_THRESHOLD 0xC00000 // 12Mb
#endif

#endif // _PRE_COMPILER
