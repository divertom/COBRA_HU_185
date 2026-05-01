@echo off
REM Build App Only (Skip Bootloader)
REM Fastest option when only app code changed

echo ========================================
echo Building App Only (Skipping Bootloader)
echo ========================================

if "%IDF_PATH%"=="" (
    echo ERROR: IDF_PATH not set!
    echo Run: C:\Users\torst\esp\v5.5.1\esp-idf\export.bat
    exit /b 1
)

REM Enable ccache
set IDF_CCACHE_ENABLE=1

REM Build app only (skips bootloader rebuild)
idf.py app-build

if %ERRORLEVEL% EQU 0 (
    echo.
    echo Build complete! Firmware: build\COBRA_HU_185.bin
) else (
    echo Build failed! Try: idf.py clean
    exit /b 1
)




