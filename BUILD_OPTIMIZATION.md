# ESP-IDF Build Optimization Guide

## Ways to Avoid Rebuilding ESP-IDF Frequently

ESP-IDF already uses **incremental builds** by default, but here are several strategies to minimize rebuilds:

### 1. ✅ Use Incremental Builds (Default)

ESP-IDF's build system (CMake/Ninja) automatically does incremental builds:
- Only rebuilds files that changed
- Only rebuilds components that changed
- Reuses compiled objects from previous builds

**Just use:**
```cmd
idf.py build
```

**Avoid:**
```cmd
idf.py fullclean  # Only use when absolutely necessary!
```

### 2. 🚀 Enable ccache (Compiler Cache)

ccache caches compiled objects across builds, dramatically speeding up rebuilds:

#### Enable ccache:

1. **Install ccache** (if not already installed):
   - Windows: Download from https://ccache.dev/download.html
   - Or use: `choco install ccache` (if you have Chocolatey)

2. **Set environment variable**:
   ```cmd
   set IDF_CCACHE_ENABLE=1
   ```
   
   Or add to your ESP-IDF export script permanently.

3. **Verify ccache is enabled**:
   ```cmd
   idf.py build
   ```
   Look for: `ccache: enabled`

#### Benefits:
- First build: Normal speed
- Subsequent builds: **5-10x faster** for unchanged files
- ESP-IDF components cached separately from your code

### 3. 📝 Build Only What Changed

#### Build specific components:
```cmd
idf.py build main          # Only rebuild main component
idf.py build BLE_Media_Fob # Only rebuild BLE component
```

#### Build without bootloader:
```cmd
idf.py app-build           # Skip bootloader rebuild
```

### 4. 🔧 Avoid Unnecessary Rebuilds

#### Don't modify these unless necessary:
- `sdkconfig` - Only change when needed
- `CMakeLists.txt` - Only when adding/removing files
- `partitions.csv` - Only when changing partition layout

#### When you DO need a clean build:
```cmd
idf.py clean          # Clean app only (keeps bootloader)
idf.py fullclean      # Full clean (use sparingly!)
```

### 5. ⚡ Parallel Builds

ESP-IDF automatically uses parallel builds. You can control parallelism:

```cmd
idf.py build -j 8     # Use 8 parallel jobs (default is auto-detected)
```

### 6. 📦 Keep ESP-IDF Stable

- **Don't update ESP-IDF** unless you need new features
- **Pin ESP-IDF version** in your project documentation
- **Use managed components** (`idf_component.yml`) for dependencies

### 7. 🎯 Build Workflow Best Practices

#### Daily Development:
```cmd
# First build of the day
idf.py build

# After making code changes
idf.py build          # Incremental - only rebuilds changed files

# After changing sdkconfig
idf.py build          # Will reconfigure and rebuild affected components
```

#### When Things Go Wrong:
```cmd
# Try incremental clean first
idf.py clean
idf.py build

# Only if absolutely necessary
idf.py fullclean
idf.py build
```

### 8. 📊 Understanding Build Times

**Typical build times (with ccache):**
- First build: 2-5 minutes
- Incremental (code change): 10-30 seconds
- Incremental (sdkconfig change): 30-60 seconds
- Full rebuild: 2-5 minutes

**Without ccache:**
- First build: 2-5 minutes
- Incremental (code change): 30-60 seconds
- Incremental (sdkconfig change): 1-2 minutes
- Full rebuild: 2-5 minutes

### 9. 🔍 Check What Will Be Rebuilt

Before building, see what changed:
```cmd
idf.py build --dry-run    # Shows what would be built (if supported)
```

Or just build - it will show what's being rebuilt.

### 10. 💾 Build Artifacts Location

Build artifacts are in `build/` directory:
- Compiled objects: `build/main/`, `build/components/`
- Final firmware: `build/COBRA_HU_185.bin`
- Bootloader: `build/bootloader/bootloader.bin`

**Keep the build directory** - don't delete it between builds!

## Quick Reference

| Command | When to Use | Speed |
|---------|-------------|-------|
| `idf.py build` | Normal development | Fast (incremental) |
| `idf.py app-build` | App changes only | Faster (skips bootloader) |
| `idf.py build main` | Only main component | Fastest |
| `idf.py clean` | Build issues | Medium |
| `idf.py fullclean` | Major changes | Slow (full rebuild) |

## Recommended Setup

1. **Enable ccache** (one-time setup)
2. **Use `idf.py build`** for normal development
3. **Only use `fullclean`** when absolutely necessary
4. **Keep build directory** between builds

## Troubleshooting

### Build seems slow?
- Check if ccache is enabled: Look for "ccache: enabled" in build output
- Verify ccache is working: `ccache -s` (shows cache statistics)

### Getting strange build errors?
- Try: `idf.py clean` then `idf.py build`
- Only use `fullclean` as last resort

### ESP-IDF components rebuilding unnecessarily?
- Check `sdkconfig` for changes
- Verify `CMakeLists.txt` hasn't changed
- Check component dependencies in `idf_component.yml`




