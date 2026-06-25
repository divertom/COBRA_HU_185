# SPIFFS userdata (`storage/`)

Contents of [`storage/`](../storage/) are packed into the **SPIFFS** image for partition **`userdata`** (see root [`CMakeLists.txt`](../CMakeLists.txt): `spiffs_create_partition_image`).

Firmware mounts this partition at **`/storage`** (`Storage_Manager`). LVGL accesses the same tree via filesystem drive **`A:`**.

## What belongs in `storage/`

Only files the device needs at runtime:

| Path | Source |
|------|--------|
| `boot/cobra_boot.bin` | [`artwork/boot/cobra_boot.png`](../artwork/boot/cobra_boot.png) |
| `Logos/Cobra_text.bin` | [`artwork/Logos/Cobra_text.png`](../artwork/Logos/Cobra_text.png) |
| `config/device_config.json` | Copy from [`config/device_config.json.example`](../config/device_config.json.example) (gitignored) |

Do **not** put docs, `.gitkeep`, or config templates under `storage/` — they waste flash.

## Raster images (generated)

Raster graphics come from **`artwork/`**. On each build CMake runs [`tools/convert_artwork.py`](../tools/convert_artwork.py), which mirrors `artwork/**/*.png` → `storage/**/*.bin` and **removes orphan `.bin` files** with no matching source.

Edit sources in **`artwork/`** only; do not hand-edit generated `*.bin` under `storage/`.

Force reconversion:

```bash
python tools/convert_artwork.py --src artwork --dst storage --force
```

## Flashing

`idf.py build` regenerates binaries and SPIFFS; **`idf.py flash`** flashes `userdata` when `FLASH_IN_PROJECT` is enabled. App-only flash does **not** update SPIFFS.
