$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root 'build\BLESync.exe'
if (-not (Test-Path $exe)) { throw 'BLESync.exe missing; run build.bat first.' }
$ini = Join-Path $root 'build\BLESync.ini'
if (-not (Test-Path $ini)) { throw 'BLESync.ini missing.' }
$text = Get-Content -Raw -Encoding UTF8 $ini
if ($text -notmatch '(?m)^StoragePath=') { throw 'StoragePath missing.' }
if ($text -notmatch '(?m)^ScanInterval=') { throw 'ScanInterval missing.' }
function Invoke-BLESync([string]$argument, [int]$timeoutSeconds = 10) {
  $p = Start-Process -FilePath $exe -ArgumentList $argument -PassThru -WindowStyle Hidden
  if (-not $p.WaitForExit($timeoutSeconds * 1000)) { Stop-Process -Id $p.Id -Force; throw ($argument + ' timed out.') }
  return $p.ExitCode
}
$statusExit = Invoke-BLESync '--status' 10
if ($statusExit -ne 0) { throw ('--status returned ' + $statusExit) }
$unknownExit = Invoke-BLESync '--unknown-test-option' 10
if ($unknownExit -eq 0) { throw 'Unknown option returned success.' }
$captureExit = Invoke-BLESync '--capture' 15
if ($captureExit -notin @(0, 1)) { throw ('--capture returned unexpected code ' + $captureExit) }
$storage = 'D:\Storageredirect\BLESyncData'
if (Test-Path $storage) {
  $files = Get-ChildItem -Recurse -File $storage
  if ($captureExit -eq 0 -and -not ($files.Name -contains 'devices.regdata')) { throw 'devices.regdata missing after capture.' }
}
Write-Output ('PASS: CLI and capture checks completed; capture_exit=' + $captureExit)
