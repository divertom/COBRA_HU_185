# Dot-source in PowerShell:  . ./idf-activate.ps1
# Sets IDF_PATH and PATH via ESP-IDF's export.ps1. Expects IDF 5.5.x (see sdkconfig: CONFIG_IDF_INIT_VERSION).
param(
    [switch] $Quiet
)
$ErrorActionPreference = "Stop"

function Get-IdfRoot {
    if ($env:IDF_PATH) {
        $p = $env:IDF_PATH
        if (Test-Path (Join-Path $p "tools\idf.py")) { return $p }
    }
    $candidates = @(
        (Join-Path $env:USERPROFILE "esp\v5.5.1\esp-idf"),
        (Join-Path $env:USERPROFILE "esp\esp-idf")
    )
    foreach ($c in $candidates) {
        if (Test-Path (Join-Path $c "tools\idf.py")) { return $c }
    }
    if (Test-Path "C:\Espressif\frameworks") {
        $dirs = Get-ChildItem "C:\Espressif\frameworks" -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -like "esp-idf-v*" } |
        Sort-Object { $_.Name } -Descending
        foreach ($d in $dirs) {
            $c = $d.FullName
            if (Test-Path (Join-Path $c "tools\idf.py")) { return $c }
        }
    }
    $null
}

$root = Get-IdfRoot
if (-not $root) {
    $msg = @"
ESP-IDF not found. This repo targets ESP32-S3 with IDF 5.5.1 (see sdkconfig).
  1) https://dl.espressif.com/dl/esp-idf/  (Windows installer, pick v5.5.x)
  2) Or: git clone -b v5.5.1 --recursive https://github.com/espressif/esp-idf.git $env:USERPROFILE\esp\esp-idf
     then in that folder:  .\install.ps1   and   . .\export.ps1
After install, run:  . .\idf-activate.ps1
"@
    [Console]::Error.WriteLine($msg)
    exit 1
}

$export = Join-Path $root "export.ps1"
if (-not (Test-Path $export)) {
    [Console]::Error.WriteLine("Missing $export")
    exit 1
}
# export.ps1 prints activation text to stderr; do not treat that as a fatal error.
$ErrorActionPreference = "Continue"
. $export
$ErrorActionPreference = "Stop"
if (-not $Quiet) {
    Write-Host "ESP-IDF ready: IDF_PATH=$env:IDF_PATH" -ForegroundColor Green
}
