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

## Source
- `src/main.cpp`: boot screen and boot flow.
- `src/ui/`: home screen, settings, nixie clock, loading screen, backups, Wi-Fi and weather.
- `src/engine/`: the installer, partition table, app registry and backup engine, taken from Launcher (MIT licensed) with everything MantisCore doesn't use removed. `engine_glue.cpp` connects it to the UI.
- Art pipeline: `build_assets.py`, `weather_build.py` and `music_icon_build.py` write full headers to `tools/generated/`. `pack_assets.py` then moves the big pixel arrays into `sd_card/mantiscore/assets.bin` (out of flash) and writes the slim headers plus `src/ui/mantis_asset_pack.cpp`. Run it after any generator.
- `tools/`: `mantis_icon_maker.py` makes app icons. `build_assets.py` regenerates `src/ui/mantis_assets.h` and the SD backgrounds from `tools/art/`. `weather_build.py` regenerates `src/ui/mantis_weather_assets.h` and `weather.mg`; run it after `build_assets.py`, because it reuses the nixie glyph size.

## Weather screen
Tap the weather in the home-screen header to open it, and tap anywhere to go back. Data comes from Open-Meteo: the forecast API, plus the air-quality API for AQI, PM2.5, PM10, CO and ozone. It refreshes every 15 minutes, and immediately when the data is more than 10 minutes old as you open the screen. Units (°F/mph/in or °C/km/h/mm) are set in Settings → Weather units.

## Music player
**Music** is built in and sits just before Settings on the home screen. It plays MP3s from `/music`, using the libhelix decoder and the Core2 speaker via `M5.Speaker`.

- **Controls:** play/pause, previous (restarts the song if it's more than 3 s in), next, shuffle, a volume slider, drag-to-seek, and a playlist (the LIST button, or tap the song title).
- **Sub-bass switch:** drives the vibration motor from the music's low end, below about 85 Hz.
- **Track info:** titles come from ID3 tags when present, otherwise from the file name.
- **Saved settings:** volume, shuffle, sub-bass and the last song are remembered.
- **Background play:** music stops when you close the player with X.
- `tools/music_icon_build.py` regenerates the default tile icon.
