# Artwork (source graphics)

Place **editable** raster artwork here. At build time these files are converted into LVGL **8.x** `.bin` images under [`storage/`](../storage/) and packaged into the **SPIFFS** (`userdata`) partition.

## Conventions

- **Boot splash** (required for cold boot): [`boot/cobra_boot.png`](boot/cobra_boot.png) → `storage/boot/cobra_boot.bin` → load in firmware as `A:/boot/cobra_boot.bin`.
- **Other UI images**: any subfolder (e.g. `Logos/`, `ReadMe/`) — same basename, extension becomes `.bin`.

## Supported formats

- **Input**: `.png`, `.jpg`, `.jpeg`, `.bmp` (anything Pillow can open).
- **Output**: LVGL binary (header + pixels). Opaque images use `LV_IMG_CF_TRUE_COLOR` (RGB565, **byte order matches** `CONFIG_LV_COLOR_16_SWAP=y`). Images with transparency use `LV_IMG_CF_TRUE_COLOR_ALPHA`.
- **SVG**: not converted by default (skipped with a note). Add a raster export or extend the script if you need vectors.

## Manual conversion

Normally you do **not** run this by hand — CMake runs [`tools/convert_artwork.py`](../tools/convert_artwork.py) before SPIFFS image generation.

Force everything to rebuild:

```bash
python tools/convert_artwork.py --src artwork --dst storage --force
```

Python dependency: `pip install -r tools/requirements-artwork.txt` (ESP-IDF’s Python environment is fine).

## Do not

- Edit generated `.bin` files under `storage/` by hand — they are overwritten on the next successful build.
