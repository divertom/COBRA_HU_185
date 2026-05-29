# Storage Directory (SPIFFS source tree)

Contents of this directory are turned into the **SPIFFS** image for partition **`userdata`** (see root [`CMakeLists.txt`](../CMakeLists.txt): `spiffs_create_partition_image`).

Firmware mounts this partition at **`/storage`** (`Storage_Manager`). LVGL accesses the same tree via filesystem drive **`A:`** (`lvgl_spiffs_assets_fs_register`).

## Raster images (generated)

Raster graphics come from **`artwork/`**. On each project build CMake runs:

1. **`tools/convert_artwork.py`** — mirrors folder structure under `storage/` as `.bin` files (LVGL 8 compatible).
2. **SPIFFS image generation** — packs `storage/` into `userdata.bin` (depends on the conversion step).

Do **not** commit or hand-edit generated `*.bin` files under `storage/` (they are typically gitignored). Edit sources in **`artwork/`** only.

To force reconversion without a full clean:

```bash
python tools/convert_artwork.py --src artwork --dst storage --force
```

Install tooling dependency in the ESP-IDF Python env if needed:

```bash
pip install -r tools/requirements-artwork.txt
```

## Other files

Non-image payloads (JSON, placeholders, `.gitkeep`, etc.) are copied into SPIFFS as-is; the converter ignores non-supported extensions.

## Flashing

A normal **`idf.py build`** regenerates binaries and SPIFFS; **`idf.py flash`** flashes firmware including the userdata image when **`FLASH_IN_PROJECT`** is enabled.

## Capacity

SPIFFS partition size is defined by your **`partitions.csv`** / **`sdkconfig`**. Keep total `storage/` size under that budget.
