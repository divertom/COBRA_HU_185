# COBRA_HU_185 Firmware Build Instructions

## Prerequisites

1. **ESP-IDF v5.5.1** installed at `C:\Users\torst\esp\v5.5.1\esp-idf`
2. **Python 3.12** installed
3. **ESP-IDF tools** installed

## ⚡ Quick Start (Fastest Build)

For normal development, use incremental builds:

```cmd
build_incremental.bat
```

Or manually:
```cmd
idf.py build
```

**This only rebuilds changed files - much faster!**

## Build Steps

### Option 1: Quick Incremental Build (Recommended)

1. Open Command Prompt or PowerShell
2. Navigate to the project directory:
   ```cmd
   cd E:\Cobra\COBRA_HU_185
   ```
3. Set up ESP-IDF environment:
   ```cmd
   C:\Users\torst\esp\v5.5.1\esp-idf\export.bat
   ```
4. Run the incremental build script:
   ```cmd
   build_incremental.bat
   ```
   
   **This is the fastest option** - only rebuilds changed files!

### Option 2: Build App Only (Fastest)

If you only changed application code (not bootloader):
```cmd
build_app_only.bat
```

### Option 3: Full Build Script

For first build or when you need a clean build:
```cmd
build_firmware.bat
```

### Option 2: Manual Build

1. Open Command Prompt or PowerShell
2. Navigate to the project directory:
   ```cmd
   cd E:\Cobra\COBRA_HU_185
   ```
3. Set up ESP-IDF environment:
   ```cmd
   C:\Users\torst\esp\v5.5.1\esp-idf\export.bat
   ```
4. Set the target chip:
   ```cmd
   idf.py set-target esp32s3
   ```
5. Build the project:
   ```cmd
   idf.py build
   ```

## Firmware Location

After a successful build, the firmware image will be located at:
```
build\COBRA_HU_185.bin
```

## Flashing the Firmware

### Flash to device:
```cmd
idf.py -p COMx flash
```
(Replace `COMx` with your actual COM port, e.g., `COM3`)

### Flash and monitor:
```cmd
idf.py -p COMx flash monitor
```

## Build Output Files

- **Firmware binary**: `build\COBRA_HU_185.bin`
- **Partition table**: `build\partition_table\partition-table.bin`
- **Bootloader**: `build\bootloader\bootloader.bin`

## Build Optimization

### Avoid Frequent Rebuilds

ESP-IDF uses **incremental builds** by default. See `BUILD_OPTIMIZATION.md` for details.

**Key points:**
- ✅ Use `idf.py build` for normal development (incremental)
- ✅ Use `idf.py app-build` to skip bootloader rebuild
- ✅ Enable ccache for 5-10x faster rebuilds
- ❌ Avoid `idf.py fullclean` unless absolutely necessary
- ❌ Don't delete the `build/` directory between builds

### Enable ccache (Recommended)

Add to your ESP-IDF export script or run before building:
```cmd
set IDF_CCACHE_ENABLE=1
```

This caches compiled objects and dramatically speeds up rebuilds!

## Troubleshooting

### Build Errors
- Ensure ESP-IDF environment is properly set up
- Check that all dependencies are installed
- Verify `CMakeLists.txt` has all required components
- Try: `idf.py clean` then `idf.py build`
- Only use `idf.py fullclean` as last resort

### Build Seems Slow?
- Check if ccache is enabled (look for "ccache: enabled" in build output)
- Use `build_incremental.bat` instead of `build_firmware.bat`
- Use `build_app_only.bat` if only app code changed

### Flash Errors
- Check COM port is correct
- Ensure device is in download mode
- Try holding BOOT button while connecting

## Configuration

Before building, ensure the BLE MAC address is set in:
```
storage\config\device_config.json
```

The file should contain:
```json
{
  "settings": {
    "ble_mac_address": "FF:FF:40:00:10:26"
  }
}
```

