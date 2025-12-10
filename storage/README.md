# Storage Directory

This directory contains files that will be uploaded to the ESP32-S3 device's SPIFFS partition.

## Directory Structure

- `images/` - Image files (PNG, JPG, etc.) for display on the device
- `config/` - Configuration files (JSON, TXT, etc.) for device settings

## How to Upload Files

### Method 1: Using ESP-IDF mkspiffs tool

1. Build the SPIFFS image:
```bash
idf.py spiffsgen
```

2. Flash the SPIFFS partition:
```bash
idf.py spiffs-flash
```

### Method 2: Using mkspiffs tool directly

1. Install mkspiffs (if not already installed):
```bash
pip install mkspiffs
```

2. Create SPIFFS image:
```bash
mkspiffs -c storage -b 4096 -p 256 -s 0x5C0000 build/storage.bin
```

3. Flash to device:
```bash
esptool.py --chip esp32s3 --port COMx write_flash 0x290000 build/storage.bin
```

### Method 3: Using ESP-IDF component (recommended)

The project is configured to automatically create a SPIFFS image during build.
Run:
```bash
idf.py build
idf.py flash
```

## Notes

- Maximum file size depends on your SPIFFS partition size (currently 5900K)
- File names are case-sensitive
- Use forward slashes (/) in file paths, not backslashes
- Total size of all files must fit within the partition size

