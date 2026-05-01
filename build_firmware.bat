@echo off
REM ESP-IDF Build Script for COBRA_HU_185
REM Make sure ESP-IDF environment is set up before running this script
REM This script uses incremental builds for faster compilation

echo ========================================
echo Building COBRA_HU_185 Firmware
echo ========================================

REM Check if ESP-IDF is set up
if "%IDF_PATH%"=="" (
    echo ERROR: IDF_PATH not set!
    echo Please run ESP-IDF setup script first:
    echo   For ESP-IDF v5.x: C:\Users\torst\esp\v5.5.1\esp-idf\export.bat
    exit /b 1
)

REM Enable ccache if available (speeds up rebuilds significantly)
if "%IDF_CCACHE_ENABLE%"=="" (
    echo Enabling ccache for faster builds...
    set IDF_CCACHE_ENABLE=1
)

REM Set target (if not already set)
echo Checking target...
idf.py set-target esp32s3

REM Check if build directory exists
if exist "build" (
    echo Using incremental build (faster - only rebuilds changed files)
    echo To force a clean build, use: idf.py fullclean
) else (
    echo First build - this will take longer
)

REM Build the project (incremental by default)
echo Building project...
idf.py build

if %ERRORLEVEL% EQU 0 (
    echo.
    echo ========================================
    echo Build Successful!
    echo ========================================
    echo.
    echo Firmware image location:
    echo   build\COBRA_HU_185.bin
    echo.
    echo To flash the firmware:
    echo   idf.py -p COMx flash
    echo.
    echo To flash and monitor:
    echo   idf.py -p COMx flash monitor
    echo.
    echo Build optimization tips:
    echo   - Use 'idf.py build' for incremental builds (fast)
    echo   - Use 'idf.py app-build' to skip bootloader rebuild
    echo   - Use 'idf.py clean' if you have build issues
    echo   - Only use 'idf.py fullclean' when absolutely necessary
    echo.
) else (
    echo.
    echo ========================================
    echo Build Failed!
    echo ========================================
    echo Try: idf.py clean
    echo Then: idf.py build
    echo.
    echo Please check the error messages above.
    exit /b 1
)

