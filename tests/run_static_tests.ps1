$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root 'build\BLESync.exe'
if (-not (Test-Path $exe)) { throw 'build\BLESync.exe does not exist; run build.bat first.' }
$ini = Join-Path $root 'build\BLESync.ini'
if (-not (Test-Path $ini)) { throw 'build\BLESync.ini does not exist.' }
$text = Get-Content -Raw -Encoding UTF8 $ini
if ($text -notmatch '(?m)^StoragePath=') { throw 'StoragePath missing from INI.' }
if ($text -notmatch '(?m)^ScanInterval=') { throw 'ScanInterval missing from INI.' }
$help = Start-Process -FilePath $exe -ArgumentList '--unknown-test-option' -Wait -PassThru -WindowStyle Hidden
if ($help.ExitCode -eq 0) { throw 'Unknown option unexpectedly returned success.' }
Write-Output 'PASS: build artifact and conservative CLI error path'
