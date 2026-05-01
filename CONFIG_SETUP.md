# Configuration File Setup

## Config File Location

The BLE Media Fob requires a configuration file at:
```
/storage/config/device_config.json
```

This file is stored in SPIFFS and contains the BLE MAC address of the media fob.

## File Structure

The config file should have this structure:

```json
{
  "device_name": "COBRA_HU_185",
  "version": "1.0.0",
  "wifi": {
    "ssid": "YourWiFiSSID",
    "password": "YourWiFiPassword"
  },
  "display": {
    "brightness": 80,
    "timeout": 30
  },
  "settings": {
    "language": "en",
    "timezone": "UTC",
    "ble_mac_address": "FF:FF:40:00:10:26"
  }
}
```

## Setup Instructions

### Option 1: Pre-flash (Recommended)

1. Copy `device_config.json.example` to `device_config.json`:
   ```cmd
   copy storage\config\device_config.json.example storage\config\device_config.json
   ```

2. Edit `storage\config\device_config.json` and set your BLE MAC address:
   ```json
   "ble_mac_address": "FF:FF:40:00:10:26"
   ```

3. Rebuild and flash the firmware - the config file will be included in SPIFFS

### Option 2: Runtime Creation

The firmware will automatically create a default config file if it doesn't exist, but you'll need to:
1. Flash the firmware
2. The default config will be created on first boot
3. Update the MAC address in the created file (requires SPIFFS write access)

## Finding Your Media Fob MAC Address

To find your media fob's MAC address:

1. **Using a BLE scanner app** on your phone:
   - Install a BLE scanner app (e.g., "nRF Connect", "BLE Scanner")
   - Scan for devices
   - Look for your media fob device
   - Note the MAC address (format: XX:XX:XX:XX:XX:XX)

2. **Using ESP32 BLE scan**:
   - The `Wireless.c` code includes BLE scanning functionality
   - Check serial monitor output during BLE scan

3. **From device documentation**:
   - Check the media fob's documentation or label

## Troubleshooting

### Error: "Failed to read config file"

**Cause**: Config file doesn't exist in SPIFFS

**Solution**:
1. Ensure `storage/config/device_config.json` exists in your project
2. Rebuild and flash the firmware
3. The file will be included in the SPIFFS image

### Error: "Failed to create default config file"

**Cause**: SPIFFS not initialized or no write access

**Solution**:
1. Check that `storage_init()` is called before BLE initialization
2. Verify SPIFFS partition is properly configured in `partitions.csv`
3. Check SPIFFS mount status in logs

### Config file not updating

**Cause**: File is read-only in SPIFFS or needs rebuild

**Solution**:
1. Update `storage/config/device_config.json` in source
2. Rebuild and reflash firmware
3. Or use SPIFFS write functions to update at runtime

## File Paths

- **Source file**: `storage/config/device_config.json`
- **SPIFFS path**: `/storage/config/device_config.json`
- **API call**: `storage_read_file("/config/device_config.json", ...)`
  - Storage_Manager prepends `/storage`, so this becomes `/storage/config/device_config.json`

## Notes

- The config file is included in the SPIFFS image during build
- Changes to the source file require a rebuild and reflash
- The firmware will create a default config file if missing (on first boot)
- Make sure to set the correct BLE MAC address for your media fob




