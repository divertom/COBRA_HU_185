# Run while connected to the service portal AP (192.168.4.1).
# SSID is service_portal.ap_ssid in device_config.json (default "Cobra HU Service").
# Usage: .\tools\verify_portal.ps1

$ErrorActionPreference = 'Stop'
$Base = 'http://192.168.4.1'

function Test-Json {
    param([string]$Path, [string]$Method = 'GET', [string]$Body = $null)
    $params = @{
        Uri             = "$Base$Path"
        Method          = $Method
        TimeoutSec      = 8
        UseBasicParsing = $true
    }
    if ($Body) {
        $params['ContentType'] = 'application/json'
        $params['Body'] = $Body
    }
    $r = Invoke-WebRequest @params
    return ($r.Content | ConvertFrom-Json)
}

Write-Host "=== Cobra service portal verification ===" -ForegroundColor Cyan

# Captive / root page
$root = Invoke-WebRequest -Uri "$Base/" -TimeoutSec 8 -UseBasicParsing
if ($root.StatusCode -ne 200) { throw "Root page failed: $($root.StatusCode)" }
Write-Host "[OK] GET / ($($root.RawContentLength) bytes)" -ForegroundColor Green

# TPMS API
$tpms = Test-Json '/api/tpms'
if (-not $tpms.ok) { throw "GET /api/tpms failed" }
Write-Host "[OK] GET /api/tpms (scan_active=$($tpms.scan_active), sensors=$($tpms.sensors.Count))" -ForegroundColor Green

# Units API
$units = Test-Json '/api/settings/units'
if (-not $units.ok) { throw "GET /api/settings/units failed" }
Write-Host "[OK] GET /api/settings/units (temp=$($units.temp_unit), pressure=$($units.pressure_unit))" -ForegroundColor Green

# Scan start/stop (no sensors required)
$scanOn = Test-Json '/api/tpms/scan' 'POST' '{"active":true}'
if (-not $scanOn.ok) { throw "POST scan start failed" }
Write-Host "[OK] POST /api/tpms/scan active=true" -ForegroundColor Green
Start-Sleep -Seconds 2
$scanOff = Test-Json '/api/tpms/scan' 'POST' '{"active":false}'
if (-not $scanOff.ok) { throw "POST scan stop failed" }
Write-Host "[OK] POST /api/tpms/scan active=false" -ForegroundColor Green

# RTC
$rtc = Test-Json '/api/rtc'
if (-not $rtc.ok) { throw "GET /api/rtc failed" }
Write-Host "[OK] GET /api/rtc" -ForegroundColor Green

Write-Host "`nAll portal HTTP checks passed." -ForegroundColor Cyan
