$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root 'build\BLESync.exe'
if (-not (Test-Path $exe)) { throw 'BLESync.exe missing.' }
$ini = Join-Path $root 'build\BLESync.ini'
if (-not (Test-Path $ini)) { throw 'BLESync.ini missing.' }
$text = Get-Content -Raw -Encoding UTF8 $ini
if (-not [regex]::IsMatch($text, '(?m)^StoragePath=')) { throw 'StoragePath missing.' }
if (-not [regex]::IsMatch($text, '(?m)^ScanInterval=')) { throw 'ScanInterval missing.' }
function Invoke-BLESync([string]$argument, [int]$timeoutSeconds = 10) {
  $p = Start-Process -FilePath $exe -ArgumentList $argument -PassThru -WindowStyle Hidden
  if (-not $p.WaitForExit($timeoutSeconds * 1000)) {
    Stop-Process -Id $p.Id -Force
    throw ($argument + ' timed out.')
  }
  return $p.ExitCode
}
$statusExit = Invoke-BLESync '--status' 10
if ($statusExit -ne 0) { throw ('--status returned ' + $statusExit) }
$unknownExit = Invoke-BLESync '--unknown-test-option' 10
if ($unknownExit -eq 0) { throw 'Unknown option returned success.' }
$captureExit = Invoke-BLESync '--capture' 15
if ($captureExit -notin @(0, 1)) { throw ('--capture returned unexpected code ' + $captureExit) }
$storage = 'D:\Storageredirect\BLESyncData'
$acl = (Get-Acl $storage).Access | ForEach-Object { $_.IdentityReference.Value }
if ($acl -contains 'Everyone' -or $acl -contains 'BUILTIN\Users' -or $acl -contains 'NT AUTHORITY\Authenticated Users') {
  throw 'Storage ACL grants access to ordinary users.'
}
$metadata = Join-Path $storage 'state\metadata.ini'
if (-not (Test-Path $metadata)) { throw 'metadata.ini missing.' }
$instance = Join-Path $storage 'state\instance.ini'
if (-not (Test-Path $instance)) { throw 'instance.ini missing.' }
$logPath = Join-Path $storage 'logs\BLESync.log'
if (Test-Path $logPath) {
  $log = Get-Content -Raw $logPath
  if ([regex]::IsMatch($log, '(?i)link.?key')) { throw 'Log contains Link Key text.' }
  if ([regex]::IsMatch($log, '(?i)[0-9a-f]{32,}')) { throw 'Log contains long hexadecimal text.' }
}
Write-Output ('PASS: CLI and capture checks completed; capture_exit=' + $captureExit)
