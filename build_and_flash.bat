@echo off
REM Build and Flash Script for COBRA_HU_185
REM This script builds the firmware and flashes it to COM7

echo ========================================
echo Building and Flashing COBRA_HU_185
echo ========================================

REM Setup ESP-IDF environment
echo Setting up ESP-IDF environment...
call C:\Users\torst\esp\v5.5.1\esp-idf\export.bat
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: Failed to set up ESP-IDF environment!
    echo Please ensure ESP-IDF is properly installed.
    pause
    exit /b 1
)

REM Enable ccache
set IDF_CCACHE_ENABLE=1

REM Build the project
echo.
echo Building firmware...
idf.py build
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: Build failed!
    pause
    exit /b 1
)

REM Flash to device (COM7)
echo.
echo Flashing firmware to COM7...
idf.py -p COM7 flash

if %ERRORLEVEL% EQU 0 (
    echo.
    echo ========================================
    echo Flash Successful!
    echo ========================================
    echo.
    echo To monitor the device output:
    echo   idf.py -p COM7 monitor
    echo.
) else (
    echo.
    echo ========================================
    echo Flash Failed!
    echo ========================================
    echo Please check:
    echo   1. Device is connected to COM7
    echo   2. Device is in download mode (hold BOOT button)
    echo   3. Drivers are installed correctly
    echo.
    pause
    exit /b 1
)

pause




