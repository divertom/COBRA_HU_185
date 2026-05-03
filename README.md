# COBRA_HU_185 Project

This is a copy of the ESP32-S3-LCD-1.85-Test project with the following functionality removed:
- Audio (PCM5101 driver and audio playback)
- Microphone (MIC_Driver and speech recognition)
- Touch input and gesture recognition

## Remaining Functionality

- LCD Display (ST77916)
- LVGL Graphics Library
- SD Card support
- I2C drivers
- PCF85063 RTC
- QMI8658 IMU
- Battery monitoring
- Power key handling
- Wireless connectivity
- EXIO (TCA9554PWR)
- SPIFFS file system for persistent storage

## Build Instructions

1. Open the project in ESP-IDF
2. Run `idf.py build` to build the project
3. Run `idf.py flash` to flash to device

## RTC time (serial)

Wall time is kept on the **PCF85063** (I2C RTC). The clock page reads that chip every second.

To set the date and time from the **serial terminal** (e.g. `idf.py monitor`), send a full line ending with Enter:

```text
settime:HHMMYYYYMMDD
```

There must be **exactly 12 digits** after the colon, with no spaces:

| Part | Digits | Meaning |
|------|--------|---------|
| `HHMM` | 4 | Hour (00–23), minute (00–59) |
| `YYYYMMDD` | 8 | Four-digit year, two-digit month, two-digit day (ISO order) |

**Example:** `settime:143020260502` sets **2026-05-02** at **14:30:00** (seconds are always set to **0**).

The chip stores the year as an offset from **1970**, so valid years are **1970–2069**. On success or error, the firmware prints a short line to the console (e.g. `settime: OK …` or a validation message).

## Image Optimization Tools

The project includes Python tools in the `tools/` directory for optimizing PNG images for embedded use:

### optimize_png.py
Optimizes PNG files to reduce file size while maintaining quality. **Use this first** before converting images.

**Usage:**
```bash
python tools/optimize_png.py <input.png> [output.png] [max_size_kb]
```

**Example:**
```bash
# Optimize and overwrite original (recommended)
python tools/optimize_png.py storage/Logos/Cobra_Logo_WoB_3D_redtext_360.png

# Optimize with target size limit
python tools/optimize_png.py storage/Logos/logo.png logo.png 64
```

**Benefits:**
- Reduces PNG file size by 30-50%
- Removes alpha channel (RGBA → RGB)
- Maximum compression
- Overwrites original by default

### convert_image.py
Converts PNG images to C header files with RGB565 arrays for direct inclusion in code.

**Usage:**
```bash
python tools/convert_image.py <input.png> [output.h] [array_name]
```

**Example:**
```bash
python tools/convert_image.py artwork/Logos/Cobra_Logo_WoB_3D_redtext_360.png main/LVGL_UI/cobra_logo.h cobra_logo
```

**Output:**
- Creates a C header file with RGB565 pixel data
- Includes width/height defines
- Ready to include in your code: `#include "cobra_logo.h"`

### convert_to_raw.py
Converts PNG images to raw RGB565 binary files for SPIFFS storage.

**Usage:**
```bash
python tools/convert_to_raw.py <input.png> [output.raw]
```

**Example:**
```bash
python tools/convert_to_raw.py artwork/Logos/Cobra_Logo_WoB_3D_redtext_360.png storage/images/cobra_logo.raw
```

**Output:**
- Creates a binary file in RGB565 format
- Place in `storage/` directory to be included in SPIFFS partition
- Smaller file size than PNG (2 bytes per pixel)

### Requirements
- Python 3.x
- Pillow library: `pip install Pillow`

### Optimized Images
The following images have been optimized and are available:
- `storage/images/cobra_logo.raw` - Raw RGB565 format for SPIFFS
- `main/LVGL_UI/cobra_logo.h` - C header file for direct code inclusion

## Persistent Storage (SPIFFS)

The project includes a SPIFFS partition for storing files like images and configurations.

### Storage Directory Structure
- `storage/images/` - Image files (optimized .raw files)
- `storage/Logos/` - Boot and UI logos (PNG or raw). The clock wordmark is **`storage/Logos/Cobra_text.png`** (mirror of `artwork/Logos/Cobra_text.png`). Use **RGBA** PNGs only (do **not** run **`tools/optimize_png.py`** here — it flattens alpha onto white). Keep source width modest (on the order of **~440 px**) so decoding and zoom stay reliable on-chip. LVGL loads it as `A:/Logos/Cobra_text.png` → `/storage/Logos/...`.
- `storage/config/` - Configuration files (JSON, TXT, etc.)

### Adding Files to SPIFFS
1. Place files in the `storage/` directory
2. Build the project: `idf.py build`
3. Flash the project: `idf.py flash`

App assets from `storage/` are packaged as **`userdata.bin`** and flashed to the **`userdata`** SPIFFS partition. The **`model`** partition is reserved for ESP-SR speech models (`srmodels.bin`); flashing both images to one address caused `esptool` **overlap at 0x394000**.

> IMPORTANT - SPIFFS is NOT updated by app-only flash.
> If you change anything under `storage/` (including `storage/Logos/Cobra_text.png`), you MUST run a full `idf.py flash`. `idf.py app-flash` and the VS Code "Flash app only" button only write the application partition, leaving stale files on **`userdata`**. Symptom: the clock wordmark renders as a large white block (stale oversized PNG still on flash).
>
> To re-flash **only** the app-storage SPIFFS after `idf.py build`, use the `userdata` offset from `build/flash_args` (with the default `partitions.csv` in this repo it is **`0x957000`**):
>
> ```bash
> python -m esptool --chip esp32s3 -b 460800 --before default_reset --after hard_reset \
>     write_flash 0x957000 build/userdata.bin
> ```

### Accessing Files from Code
Files stored in SPIFFS can be accessed using standard file I/O:
```c
FILE* f = fopen("/storage/images/cobra_logo.raw", "rb");
// Read and use the file
fclose(f);
```

## UI Layout Iteration

### Quick Pillow preview (no flash needed)

`tools/preview_clock.py` renders a 360x360 PNG approximation of `Page_Clock`
using the same D-DIN font and `storage/Logos/Cobra_text.png` as the device.
Edit constants/code in `main/Page_Clock/Page_Clock.c`, mirror the change in
the same-named constants at the top of the script, then:

```bash
python tools/preview_clock.py --time 10:38 --ampm AM --day WED --date "MAY 28"
# writes build/preview_clock.png
```

Clock fonts (SIL OFL) live under `tools/fonts/`:

- `D-DINCondensed-Bold.ttf` — time digits (`HH:MM`) at **115 px** (see `font_ddin_115.c`).
- `D-DINCondensed-Regular.ttf` — AM/PM **32 px**, DAY/DATE captions **20 px**, weekday / month-day **44 px**,
  bottom credit **15 px** (`font_ddin_reg_32.c`, `font_ddin_reg_20.c`, `font_ddin_reg_44.c`, `font_ddin_reg_15.c`).

Only `idf.py flash` is needed once you are happy with the preview.

### SquareLine Studio (optional WYSIWYG)

For more involved screens you can author layouts in SquareLine Studio
(free for personal use, commercial seats sold separately):

1. Install SquareLine Studio (`squareline.io`).
2. Create a new project: 360x360, round, target **LVGL 8.3** (matches
   `components/lvgl__lvgl`).
3. Import assets:
   - Image: `artwork/Logos/Cobra_text.png` (RGBA)
   - Fonts: `tools/fonts/D-DINCondensed-Bold.ttf` (115 px time) and
     `tools/fonts/D-DINCondensed-Regular.ttf` (32 / 20 / 44 px as above), bpp 4
     (matches the generated `main/Fonts/font_ddin_*.c`).
4. Build the screen with the same primitives we use today (labels for time and
   captions, an `lv_img` for the wordmark, two thin rectangles for the rules).
5. Export and either:
   - copy the generated `ui_*.c` into a new page, or
   - use it as a visual reference and transcribe alignment numbers into
     `main/Page_Clock/Page_Clock.c`.

The Pillow preview stays useful for fast tweaks; SquareLine is heavier-weight
authoring.

## Notes

- All audio, microphone, touch input, and gesture dependencies have been removed from idf_component.yml
- Image optimization tools reduce file sizes significantly (RGB565 = 2 bytes/pixel vs PNG compression)

## UX Navigation

The firmware now includes a full-screen page navigation framework driven by the Bluetooth remote.

- Horizontal navigation (Back/Forward):
  - `Boot Logo -> Clock -> Speed -> Acceleration -> Weather`
  - Circular wrap is enabled in both directions.
- Vertical navigation (Volume+/Volume-):
  - Selects subpages inside the active page.
- Boot behavior:
  - Boot logo is shown for 5 seconds on startup.
  - After timeout, the last selected page/subpage is restored.
- Persistence:
  - Active page/subpage is saved in SPIFFS and restored after reboot.

### Navigation Module Layout

The implementation is organized by page and a central navigation module:

- `main/UI_Navigation/` - page registry/order, navigation state, persistence, event processing
- `main/Page_BootLogo/`
- `main/Page_Clock/`
- `main/Page_Speed/`
- `main/Page_Acceleration/`
- `main/Page_Weather/`

To reorder pages or insert a new page, edit the page descriptor table in `main/UI_Navigation/UI_Navigation.c`.

## Bluetooth Remote Reconnect Behavior

To improve reconnect reliability after reboot:

- The BLE client now reconnects to the remote by either:
  - remote name match (`SmartRemote`), or
  - bonded device address match from NVS.
- Bonded devices are loaded during BLE init and refreshed after authentication complete.
- This avoids cases where a previously paired remote does not reconnect because its advertised name is missing or changed.

