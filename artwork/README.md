# Artwork (source graphics)

Place **editable** raster artwork here. At build time these files are converted into LVGL **8.x** `.bin` images under [`storage/`](../storage/) and packaged into the **SPIFFS** (`userdata`) partition.

## Conventions

- **Boot splash** (required): [`boot/cobra_boot.png`](boot/cobra_boot.png) → `storage/boot/cobra_boot.bin` → `A:/boot/cobra_boot.bin`.
- **Clock wordmark** (required): [`Logos/Cobra_text.png`](Logos/Cobra_text.png) → `storage/Logos/Cobra_text.bin`.

Only add PNGs here that firmware loads from SPIFFS. Orphan `.bin` files under `storage/` are removed on build.

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
