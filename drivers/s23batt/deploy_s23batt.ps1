$ProgressPreference = 'SilentlyContinue'
$ErrorActionPreference = 'Stop'
$env:RM9_WINUSB = '1'

$dir = $PSScriptRoot
$pkgDir = Join-Path $dir 'pkg'
$s23link = Join-Path (Split-Path -Parent (Split-Path -Parent $dir)) 's23link.py'

Write-Host '=== Checking S23 USB link ==='
& python $s23link ping
if ($LASTEXITCODE -ne 0) { throw "s23link ping failed ($LASTEXITCODE)" }

$localSysMd5 = (Get-FileHash (Join-Path $pkgDir 's23batt.sys') -Algorithm MD5).Hash
Write-Host "Local s23batt.sys MD5=$localSysMd5"

Write-Host '=== Uploading s23batt v1.0.0.2 package ==='
& python $s23link exec 'cmd /c if not exist C:\s23_batt_pkg mkdir C:\s23_batt_pkg'
& python $s23link put (Join-Path $pkgDir 's23batt.sys') 'C:\s23_batt_pkg\s23batt.sys'
& python $s23link put (Join-Path $pkgDir 's23batt.inf') 'C:\s23_batt_pkg\s23batt.inf'
& python $s23link put (Join-Path $pkgDir 's23batt.cat') 'C:\s23_batt_pkg\s23batt.cat'
if (Test-Path (Join-Path $pkgDir 's23test.cer')) {
    & python $s23link put (Join-Path $pkgDir 's23test.cer') 'C:\s23_batt_pkg\s23test.cer'
}
& python $s23link put (Join-Path $dir 'install_batt_live.ps1') 'C:\s23_batt_pkg\install_batt_live.ps1'

Write-Host '=== Installing & Upgrading s23batt on S23 ==='
$installCmd = @'
$pkg = 'C:\s23_batt_pkg'
$repo = 'C:\Windows\System32\DriverStore\FileRepository'

# Reset 64KB log
$zeros = New-Object byte[] 65536
[IO.File]::WriteAllBytes('C:\Users\Public\s23batt.log', $zeros)

# Add driver to DriverStore
& pnputil /add-driver "$pkg\s23batt.inf" /install 2>&1 | Out-String | Write-Output

# Replace driver binary directly in all DriverStore\FileRepository\s23batt* folders
Disable-PnpDevice -InstanceId 'ROOT\S23BATT\0000','ROOT\S23BATT' -Confirm:$false -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 800
$dirs = Get-ChildItem -Path $repo -Directory -Filter 's23batt*' -ErrorAction SilentlyContinue
if ($dirs) {
    foreach ($d in $dirs) {
        & icacls $d.FullName /grant "*S-1-1-0:(OI)(CI)F" /grant "BUILTIN\Administrators:(OI)(CI)F" /grant "NT AUTHORITY\SYSTEM:(OI)(CI)F" /T 2>&1 | Out-Null
        Copy-Item "$pkg\s23batt.sys" (Join-Path $d.FullName 's23batt.sys') -Force -ErrorAction SilentlyContinue
    }
}
$targetSys = 'C:\Windows\System32\drivers\s23batt.sys'
if (Test-Path $targetSys) {
    Copy-Item "$pkg\s23batt.sys" $targetSys -Force -ErrorAction SilentlyContinue
}
Enable-PnpDevice -InstanceId 'ROOT\S23BATT\0000','ROOT\S23BATT' -Confirm:$false -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 800

& powershell.exe -NoProfile -ExecutionPolicy Bypass -File 'C:\s23_batt_pkg\install_batt_live.ps1'
'@
$enc = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($installCmd))
& python $s23link exec -t 60 "powershell -NoProfile -EncodedCommand $enc"

Write-Host '=== Checking S23 Battery & Charging Telemetry ==='
Start-Sleep -Seconds 3
& python $s23link exec 'powershell -NoProfile -Command "Get-Content C:\Users\Public\s23batt.log -Tail 25; (Get-Counter ''\Battery Status(*)\*'' -ErrorAction SilentlyContinue).CounterSamples | Select-Object Path,CookedValue"'
