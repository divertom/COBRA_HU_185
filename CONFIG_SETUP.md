# Configuration File Setup

## Config File Location

Device settings live in SPIFFS at:
```
/storage/config/device_config.json
```

This file is **not committed to git**. Copy the template and edit locally:

```cmd
copy config\device_config.json.example storage\config\device_config.json
```

## File Structure

```json
{
  "device_name": "COBRA_HU_185",
  "version": "1.0.0",
  "service_portal": {
    "ap_ssid": "Cobra HU Service"
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

### Service portal (WiFi AP)

- The head unit broadcasts a **service portal** SoftAP (not home Wi‑Fi / internet). Default SSID **`Cobra HU Service`**, configured in `service_portal.ap_ssid`.
- The network is **open** (no password).
- Join from a phone, then open `http://192.168.4.1/` if the captive portal does not appear automatically.
- Rebuild and flash after changing `storage/config/device_config.json` (or edit on-device SPIFFS and reboot).
- Legacy configs may still use `wifi.ssid`; `service_portal.ap_ssid` takes precedence when both are set.

### BLE media fob (optional)

Set `settings.ble_mac_address` or `settings.ble_device_name` if using BLE fob pairing from config.

### Units and TPMS (Service portal)

- **Units** (`settings.temp_unit`, `settings.pressure_unit`): `C` or `F`, and `kpa` or `psi`. Edited on the Service page; saved to `device_config.json`.
- **TPMS sensors** are stored separately in `/storage/config/tpms_sensors.json` (not in git). Each entry has a BLE MAC and tire position (`NO`, `LF`, `RF`, `LR`, `RR`).

On the Service page (**TPMS Sensors**):

1. **Start Scan** — discovers new wheel BLE advertisers and adds them to the list (default position `NO`).
2. **Stop Scan** — stops adding new sensors; known sensors still update from BLE scan.
3. Assign a tire from the dropdown (only one sensor per position; the previous holder becomes `NO`).
4. **Forget** removes a sensor from the list and SPIFFS.
5. For multiple **tsTPMS** modules, **Stop Scan** starts automatic round-robin GATT reads (~15 s per module). Optional **Set live** chooses which MAC is polled first (shown as **Live**); the row currently connected shows **Reading**.
6. BLE flow charts and architecture: [docs/tpms-ble-flow.md](docs/tpms-ble-flow.md).

Status colors: green = seen in the last 2 minutes; white = seen since boot within 5 hours; gray = not seen since boot or older than 5 hours. Telemetry: green = stable reading, yellow = changing, `--` = no data.

### TPMS persistence vs flash

- **Reboot** keeps sensors and tire assignments in SPIFFS (`/storage/config/tpms_sensors.json`).
- **Full `idf.py flash`** rebuilds the `userdata` SPIFFS image from the repo `storage/` folder. Runtime-discovered sensors are **not** kept unless you copy `tpms_sensors.json` into `storage/config/` before building.
- After changing firmware or the service portal web assets, flash the device, then use **reboot** (not a full userdata reflash) to verify that sensors and tire positions persist.

## Setup Instructions

1. Copy `device_config.json.example` → `device_config.json` (see above).
2. Edit `storage/config/device_config.json` as needed (BLE, display, etc.).
3. Rebuild and flash (`idf.py build flash`).

## Captive portal

After joining the **service portal** Wi‑Fi (default **`Cobra HU Service`**) on a phone, the OS should open a browser to the configuration page automatically (captive portal). If it does not, browse to `http://192.168.4.1/`.

### Testing on Android

1. Build and flash firmware.
2. On the phone: **Forget** the service portal Wi‑Fi network (e.g. **Cobra HU Service**).
3. **Settings → Network → Private DNS → Off** (Private DNS bypasses the ESP32 DNS redirect).
4. Reconnect to the service portal AP (no password).
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
1. Forget the service portal network and reconnect.
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
- **Template (in git)**: `config/device_config.json.example`
- **SPIFFS path**: `/storage/config/device_config.json`
- **API call**: `storage_read_file("/config/device_config.json", ...)`

## Notes

- Never commit `storage/config/device_config.json` — it may contain WiFi credentials for future STA use.
- Changes require a rebuild and reflash to update SPIFFS.
