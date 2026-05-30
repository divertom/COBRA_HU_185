# Configuration File Setup

## Config File Location

Device settings live in SPIFFS at:
```
/storage/config/device_config.json
```

This file is **not committed to git**. Copy the template and edit locally:

```cmd
copy storage\config\device_config.json.example storage\config\device_config.json
```

## File Structure

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

### Config portal (WiFi AP)

- The head unit broadcasts **SSID `Cobra HU`** as an **open** network (no password).
- Join from a phone without entering a passphrase.
- Rebuild and flash if you change other fields in `device_config.json` (AP settings are not read from JSON).

### BLE media fob (optional)

Set `settings.ble_mac_address` or `settings.ble_device_name` if using BLE fob pairing from config.

## Setup Instructions

1. Copy `device_config.json.example` → `device_config.json` (see above).
2. Edit `storage/config/device_config.json` as needed (BLE, display, etc.).
3. Rebuild and flash (`idf.py build flash`).

## Captive portal

After joining **Cobra HU** on a phone, the OS should open a browser to the configuration page automatically (captive portal). If it does not, browse to `http://192.168.4.1/`.

### Testing on Android

1. Build and flash firmware.
2. On the phone: **Forget** the **Cobra HU** Wi‑Fi network.
3. **Settings → Network → Private DNS → Off** (Private DNS bypasses the ESP32 DNS redirect).
4. Reconnect to **Cobra HU** (no password).
5. Wait 5–15 seconds for a **“Sign in to network”** or captive portal notification.
6. On the serial monitor, look for:
   - `SoftAP IP: 192.168.4.1`
   - `DNS started on UDP/53`
   - `HTTP GET /generate_204` (or `/gen_204`) and `Captive redirect: … -> /`
7. If no popup appears, open a browser and go to **`http://192.168.4.1`** manually.

### Testing on iOS

Same steps as Android (forget network, reconnect). iOS often probes `/hotspot-detect.html`; serial logs should show a redirect to `/`.

## Troubleshooting

### Captive portal popup does not appear

**Cause**: Android Private DNS, cached Wi‑Fi profile, or OS did not run HTTP probes yet.

**Solution**:
1. Forget **Cobra HU** and reconnect.
2. Set **Private DNS → Off** on Android.
3. Open **`http://192.168.4.1`** manually.
4. Check serial logs for `DNS answer` / `HTTP GET` lines when the phone connects.

### Error: "Failed to read config file"

**Cause**: `device_config.json` missing from SPIFFS image.

**Solution**:
1. Ensure `storage/config/device_config.json` exists locally (from the example copy).
2. Rebuild and flash the firmware.

### Config file not updating

**Cause**: SPIFFS image not reflashed after edit.

**Solution**: Rebuild and reflash after changing `storage/config/device_config.json`.

## File Paths

- **Local source (gitignored)**: `storage/config/device_config.json`
- **Template (in git)**: `storage/config/device_config.json.example`
- **SPIFFS path**: `/storage/config/device_config.json`
- **API call**: `storage_read_file("/config/device_config.json", ...)`

## Notes

- Never commit `storage/config/device_config.json` — it may contain WiFi credentials for future STA use.
- Changes require a rebuild and reflash to update SPIFFS.
