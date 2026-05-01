@echo off
REM Quick Incremental Build Script
REM Use this for normal development - fastest build option

echo ========================================
echo Incremental Build (Fast)
echo ========================================

if "%IDF_PATH%"=="" (
    echo ERROR: IDF_PATH not set!
    echo Run: C:\Users\torst\esp\v5.5.1\esp-idf\export.bat
    exit /b 1
)

REM Enable ccache
set IDF_CCACHE_ENABLE=1

REM Quick incremental build
idf.py build

if %ERRORLEVEL% EQU 0 (
    echo.
    echo Build complete! Firmware: build\COBRA_HU_185.bin
) else (
    echo Build failed! Try: idf.py clean
    exit /b 1
)




