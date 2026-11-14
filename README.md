# MantisCore — by BasaltSoftworks

The home screen for Mantis apps on the **M5Stack Core2**. Apps are merged `.bin` files on the SD card.
MantisCore shows them as icons over your wallpaper, installs each one into flash the first time it is
tapped, and starts it straight away after that. Each app's saved data is backed up to the SD card
automatically.

## Build & deploy
Zip-drop, the same as the other Mantis repos:
1. Put the payload zip in the repo root and push. On the first push, rename `github/` to `.github/` and commit it.
2. **Build & Pages** unpacks the zip, runs `pio run -e mantiscore`, and deploys `website/` to GitHub Pages. The deployed site has:
   - the web flasher, using `firmware/manifest.json` with the merged image at offset 0
   - `firmware/mantiscore-merged.bin`
   - `firmware/mantiscore-sd-card.zip`, made from `sd_card/`

To build locally, run `pio run -e mantiscore`. It writes `mantiscore-merged.bin` to the project root and fails if the app is larger than its 1.5 MB slot.

## How it boots
`platformio.ini` pins the "keyboot" Arduino libs build. Its bootloader starts MantisCore (the `app0` test partition) on power-on or the reset button, and starts the selected app when the app reboots itself. The bootloader reads `DDLB`, `LauncherOnKey` and `LauncherKeyLvl` from NVS namespace `launcher`; MantisCore writes those keys with the default values.

## SD card
| Path | What it is |
|---|---|
| `/*.bin` | Apps (merged binaries) |
| `/icons/<bin name>.mi` | App icons. Make them with `tools/mantis_icon_maker.py` |
| `/icons/<bin name>.txt` | Optional label override |
| `/images/*.mg` | Wallpapers. Custom wallpapers go in `/images/backgrounds/*.mg` |
| `/mantiscore/` | Nixie clock, weather and loading backgrounds, plus `assets.bin` (mascot, nixie glyphs, weather icons, built-in icons). Keep `assets.bin` matched to the firmware: a pack from another build is ignored, and plain drawn graphics are used instead. |
| `/backups/` | App data backups, created automatically |
| `/music/` | MP3s for the built-in Music player (subfolders up to 2 levels deep) |
| `/icons/mp3.mi` | Optional icon for the Music tile (replaces the built-in one) |
| `/icons/settings.mi` | Optional icon for the Settings tile (replaces the built-in one) |
| `/icons/order.txt` | Home screen tile order, one app name per line (`settings`, `mp3`, `strider` for the built-ins). Written by **Move**: long-press a tile, choose Move, then tap where it goes. Apps not listed go at the end |

## Strider
**Strider** is built in, next to Music. Walk or ride with the Core2 and it turns the trip into a print saved to `/images/strider/` as `.mg`, the format Mantis Gallery reads.

- **No GPS, no compass, by design.** The route is dead reckoning, like an etch-a-sketch drawn by your body. Heading comes from the gyro measured around gravity, so it works whether the Core2 is in a hand, a pocket or on a handlebar. The gyro is calibrated at the start (hold still for 2 s) and re-learned whenever you stop. Walking distance comes from steps, with stride length taken from cadence. Riding distance is estimated from road vibration: it suits the art, but it isn't a speedometer.
- **What the print is made of:** route shape, loudness along the way (mic), ground roughness (vibration above the walking bounce), Wi-Fi networks you pass (async scans; each new network is a "find"), places you stood still for 12 s or more, and the time of day at the start and the end. A seed hashed from all of that drives every choice, so the same loop on a different day gives a sibling print, not a copy.
- **Four print styles.** Swipe or use < > on the result screen, and SAVE as many as you like:
  - **TOPO**: loudness raises terrain, giving a contour map of an island that only exists because you walked it. It has hill shading, a survey grid, a scale bar and a north arrow marked "N?".
  - **RISO**: a two-ink risograph print. The route is a brush stroke (wider when you walk faster), the soundscape is a halftone, and the inks overprint slightly out of register. The sun or moon sits at its height when you set off.
  - **LIGHT**: a long exposure. Light streams off the route through a flow field seeded by the walk, coloured from the palette of the hour you left to the hour you got back.
  - **STARS**: Wi-Fi networks become stars, joined into a constellation (a minimum spanning tree) with an invented name. Night walks get a dark sky, day walks an old paper atlas.
- **Battery:** the screen switches off after 30 s and sensing carries on. Tap to wake. To end the walk, hold FINISH for about a second. A light buzz marks every 500 m walking, or every 2 km riding.
- Files are named `/images/strider/YYYYMMDD_HHMM_<style>.mg` (or `walk_<seed>_<style>.mg` when the clock isn't set). `/icons/strider.mi` replaces the drawn tile icon.
- **Preview on a PC:** `g++ -O2 -std=c++17 -I src tools/strider_preview/preview.cpp src/strider/*.cpp -o strider_preview && ./strider_preview out && python3 tools/strider_preview/mg_sheet.py out`. This simulates four walks, renders every style, checks each file against the `.mg` spec and makes a contact sheet.

## Source
- `src/main.cpp`: boot screen and boot flow.
- `src/ui/`: home screen, settings, nixie clock, loading screen, backups, Wi-Fi, weather, Music and Strider (`mantis_strider.cpp`: sensing and screens).
- `src/strider/`: Strider's walk recorder and print renderer. Plain C++ with no Arduino dependencies, so it also builds on a PC.
- `src/engine/`: the installer, partition table, app registry and backup engine, taken from Launcher (MIT licensed) with everything MantisCore doesn't use removed. `engine_glue.cpp` connects it to the UI.
- Art pipeline: `build_assets.py`, `weather_build.py` and `music_icon_build.py` write full headers to `tools/generated/`. `pack_assets.py` then moves the big pixel arrays into `sd_card/mantiscore/assets.bin` (out of flash) and writes the slim headers plus `src/ui/mantis_asset_pack.cpp`. Run it after any generator.
- `tools/`: `mantis_icon_maker.py` makes app icons. `build_assets.py` regenerates `src/ui/mantis_assets.h` and the SD backgrounds from `tools/art/`. `weather_build.py` regenerates `src/ui/mantis_weather_assets.h` and `weather.mg`; run it after `build_assets.py`, because it reuses the nixie glyph size.

## Weather screen
Tap the weather in the home-screen header to open it, and tap anywhere to go back. Data comes from Open-Meteo: the forecast API, plus the air-quality API for AQI, PM2.5, PM10, CO and ozone. It refreshes every 15 minutes, and immediately when the data is more than 10 minutes old as you open the screen. Units (°F/mph/in or °C/km/h/mm) are set in Settings → Weather units.

## Music player
**Music** is built in and sits just before Settings on the home screen. It plays MP3s from `/music`, using the libhelix decoder and the Core2 speaker via `M5.Speaker`.

- **Converting music:** `tools/mp3_converter/mantis_mp3_converter.py` is a standalone tkinter app. It needs ffmpeg. It converts any audio files into the format the player handles best: 44.1 kHz, constant bitrate, mono by default, ID3v2.3 title and artist, no cover art. Point its output at the SD card's `/music`.
- **Smooth playback:** the SD card shares the SPI bus with the screen, so the audio path is split in three. An SD reader task keeps a 128 KB buffer (PSRAM) of the MP3 topped up in short 4 KB reads. The decoder task works only from that buffer, so it never waits for the bus. The screen is pushed in 24-row strips, and only the parts that changed are sent. The spectrum analyser's FFT runs on the UI core: the decoder just tags each buffer with the time it will be heard, so the visuals and sub-bass motor stay in step with the speaker.

- **Controls:** play/pause, previous (restarts the song if it's more than 3 s in), next, shuffle, a volume slider, drag-to-seek, and a playlist (the LIST button, or tap the song title).
- **Sub-bass switch:** drives the vibration motor from the music's low end, below about 85 Hz.
- **Track info:** titles come from ID3 tags when present, otherwise from the file name.
- **Saved settings:** volume, shuffle, sub-bass and the last song are remembered.
- **Background play:** music stops when you close the player with X.
- `tools/music_icon_build.py` regenerates the default tile icon.
