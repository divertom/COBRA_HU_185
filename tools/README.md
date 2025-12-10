# Image Optimization Tools

This directory contains Python scripts for optimizing PNG images for embedded use in the ESP32-S3 project.

## Tools

### optimize_png.py
Optimizes PNG files to reduce file size while maintaining visual quality.

**Features:**
- Removes alpha channel (converts RGBA to RGB) to reduce size
- Maximum PNG compression
- Color quantization if needed
- Overwrites original file by default

**Usage:**
```bash
python optimize_png.py <input.png> [output.png] [max_size_kb]
```

**Example:**
```bash
# Optimize and overwrite original file
python optimize_png.py storage/Logos/logo.png

# Optimize with 64 KB target size
python optimize_png.py storage/Logos/logo.png logo.png 64

# Save to new file
python optimize_png.py storage/Logos/logo.png logo_optimized.png
```

**Output:**
- Optimized PNG file (smaller file size)
- Typically 30-50% size reduction
- Maintains visual quality

### convert_image.py
Converts PNG images to C header files with RGB565 pixel arrays.

**Features:**
- Converts PNG to RGB565 format (16-bit color)
- Generates C header file with pixel array
- Includes width/height defines
- Ready for direct inclusion in code

**Usage:**
```bash
python convert_image.py <input.png> [output.h] [array_name]
```

**Example:**
```bash
python convert_image.py ../artwork/Logos/Cobra_Logo_WoB_3D_redtext_360.png ../main/LVGL_UI/cobra_logo.h cobra_logo
```

**Output Format:**
```c
#ifndef COBRA_LOGO_H
#define COBRA_LOGO_H

#include <stdint.h>

#define COBRA_LOGO_WIDTH  360
#define COBRA_LOGO_HEIGHT 360

const uint16_t cobra_logo[] = {
  // RGB565 pixel data...
};

#endif
```

### convert_to_raw.py
Converts PNG images to raw RGB565 binary files for SPIFFS storage.

**Features:**
- Converts PNG to raw RGB565 binary format
- Optimized for SPIFFS file system
- Smaller than C arrays (no header overhead)
- Can be loaded at runtime from SPIFFS

**Usage:**
```bash
python convert_to_raw.py <input.png> [output.raw]
```

**Example:**
```bash
python convert_to_raw.py ../artwork/Logos/Cobra_Logo_WoB_3D_redtext_360.png ../storage/images/cobra_logo.raw
```

**Output:**
- Binary file in RGB565 format (2 bytes per pixel)
- Little-endian 16-bit format
- Ready to be stored in SPIFFS partition

## Requirements

- Python 3.x
- Pillow library: `pip install Pillow`

## File Size Comparison

For a 360x360 pixel image:
- **PNG (original)**: ~164 KB (varies with compression)
- **PNG (optimized)**: ~83 KB (49% reduction after optimization)
- **Raw RGB565**: 259,200 bytes (253 KB) - fixed size
- **C Header Array**: ~260 KB (includes header overhead)

## Optimization Workflow

1. **First, optimize the PNG** (recommended):
   ```bash
   python tools/optimize_png.py storage/Logos/logo.png
   ```
   This reduces file size while keeping PNG format.

2. **Then convert for your use case**:
   - For SPIFFS: `python tools/convert_to_raw.py storage/Logos/logo.png storage/images/logo.raw`
   - For C code: `python tools/convert_image.py storage/Logos/logo.png main/logo.h logo`

## When to Use Which Format

### Use C Header (convert_image.py) when:
- Image is small and used frequently
- You want compile-time inclusion
- No runtime file I/O needed
- Image is part of the firmware

### Use Raw Binary (convert_to_raw.py) when:
- Image is large
- You want to update images without recompiling
- Images are stored in SPIFFS
- You need runtime flexibility
- Multiple images need to be managed

## Notes

- RGB565 format uses 16 bits per pixel (5 bits red, 6 bits green, 5 bits blue)
- Raw files are always uncompressed (2 bytes per pixel)
- PNG files are compressed but require decompression at runtime
- For embedded systems, raw format is faster but uses more storage

