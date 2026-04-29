# Called from VS Code tasks. Forwards to idf.py (after ESP-IDF env is loaded).
# Examples:  idf-cmd.ps1 build | idf-cmd.ps1 menuconfig | idf-cmd.ps1 fullclean
#            idf-cmd.ps1 rebuild   -> fullclean then build
$ErrorActionPreference = "Stop"
Set-Location -LiteralPath $PSScriptRoot
. (Join-Path $PSScriptRoot "idf-activate.ps1") -Quiet
$pass = @($args)
if ($pass.Count -eq 0) { $pass = @("build") }
if ($pass[0] -eq "rebuild") {
    & idf.py "fullclean"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & idf.py "build"
    exit $LASTEXITCODE
}
& idf.py @pass
exit $LASTEXITCODE
