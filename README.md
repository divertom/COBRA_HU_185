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
- `storage/config/` - Configuration files (JSON, TXT, etc.)

### Adding Files to SPIFFS
1. Place files in the `storage/` directory
2. Build the project: `idf.py build`
3. Flash the project: `idf.py flash`

The SPIFFS image is automatically generated during build and flashed to the `model` partition.

### Accessing Files from Code
Files stored in SPIFFS can be accessed using standard file I/O:
```c
FILE* f = fopen("/storage/images/cobra_logo.raw", "rb");
// Read and use the file
fclose(f);
```

## Notes

- All audio, microphone, touch input, and gesture dependencies have been removed from idf_component.yml
- Image optimization tools reduce file sizes significantly (RGB565 = 2 bytes/pixel vs PNG compression)

