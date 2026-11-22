param(
    [switch]$MinBrightness,
    [switch]$RunTests
)

$ErrorActionPreference = 'Stop'
$env:RM9_WINUSB = '1'

$dir      = $PSScriptRoot
$repoRoot = Split-Path -Parent (Split-Path -Parent $dir)
$s23link  = Join-Path $repoRoot 's23link.py'
$pkgDir   = Join-Path $dir 'pkg'

function Invoke-S23Link {
    param([Parameter(ValueFromRemainingArguments = $true)][string[]]$ArgList)
    & python $s23link @ArgList
    if ($LASTEXITCODE -ne 0) {
        throw "s23link failed ($LASTEXITCODE): $($ArgList -join ' ')"
    }
}

function Invoke-S23PS {
    param([string]$Script, [int]$Timeout = 60)
    $enc = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($Script))
    & python $s23link exec -t $Timeout "powershell -NoProfile -NonInteractive -OutputFormat Text -EncodedCommand $enc"
    if ($LASTEXITCODE -ne 0) {
        throw "Remote PowerShell failed ($LASTEXITCODE)"
    }
}

Write-Host '=== Checking S23 USB link ==='
Invoke-S23Link ping

$localSysMd5 = (Get-FileHash (Join-Path $pkgDir 's23gpu.sys') -Algorithm MD5).Hash
$localUmdMd5 = (Get-FileHash (Join-Path $pkgDir 's23gpu_umd.dll') -Algorithm MD5).Hash
Write-Host "Local s23gpu.sys MD5=$localSysMd5"

Write-Host '=== Uploading s23gpu package ==='
Invoke-S23Link put (Join-Path $pkgDir 's23gpu.sys') 'C:\s23gpu\s23gpu.sys'
Invoke-S23Link put (Join-Path $pkgDir 's23gpu.inf') 'C:\s23gpu\s23gpu.inf'
Invoke-S23Link put (Join-Path $pkgDir 's23gpu.cat') 'C:\s23gpu\s23gpu.cat'

$remoteUmdOut = (Invoke-S23PS 'if (Test-Path C:\s23gpu\s23gpu_umd.dll) { (Get-FileHash C:\s23gpu\s23gpu_umd.dll -Algorithm MD5).Hash }') -join "`n"
if ($remoteUmdOut -notmatch $localUmdMd5) {
    Invoke-S23Link put (Join-Path $pkgDir 's23gpu_umd.dll') 'C:\s23gpu\s23gpu_umd.dll'
}

Write-Host '=== Installing driver ==='
Invoke-S23Link exec -t 60 'C:\s23gpu\s23gpu_install.exe C:\s23gpu\s23gpu.inf'

Write-Host '=== Verifying System32 hash & PnP device ==='
Invoke-S23PS @"
`$sysMd5 = (Get-FileHash C:\Windows\System32\drivers\s23gpu.sys -Algorithm MD5).Hash
`$d = Get-PnpDevice -InstanceId 'ROOT\DISPLAY\0000'
`$p = Get-PnpDeviceProperty -InstanceId 'ROOT\DISPLAY\0000' -KeyName 'DEVPKEY_Device_ProblemCode'
Write-Output ("SYS_MD5=" + `$sysMd5 + " (expected $localSysMd5)")
Write-Output ("PNP_STATUS=" + `$d.Status + " PROBLEM=" + `$p.Data)
if (`$sysMd5 -ne '$localSysMd5') { throw 'System32 s23gpu.sys MD5 mismatch (cached DriverStore INF?)' }
if (`$p.Data -ne 0) {
    Get-Content C:\Users\Public\s23gpu.log -Tail 35 -ErrorAction SilentlyContinue
    Get-WinEvent -LogName 'Microsoft-Windows-DxgKrnl/Admin' -MaxEvents 5 -ErrorAction SilentlyContinue | Format-List TimeCreated, Id, Message
    throw ("Adapter failed with problem code " + `$p.Data)
}
"@

Write-Host '=== Locking single S23 AMOLED display (120 Hz) ==='
Invoke-S23Link exec -t 30 'C:\s23gpu\s23disp_only.exe'

Write-Host '=== Checking Task Manager GPU counters ==='
Invoke-S23PS @'
(Get-Counter '\GPU Engine(*)\Utilization Percentage').CounterSamples | Select-Object Path, CookedValue | Format-Table -AutoSize
(Get-Counter '\GPU Adapter Memory(*)\*').CounterSamples | Select-Object Path, CookedValue | Format-Table -AutoSize
'@

if ($MinBrightness) {
    Write-Host '=== Setting S23 display brightness to minimum (0%) ==='
    Invoke-S23PS @'
$m = Get-CimInstance -Namespace root/WMI -ClassName WmiMonitorBrightnessMethods -ErrorAction Stop
$null = $m | Invoke-CimMethod -MethodName WmiSetBrightness -Arguments @{ Timeout = [uint32]1; Brightness = [byte]0 }
Get-CimInstance -Namespace root/WMI -ClassName WmiMonitorBrightness | Select-Object Active, CurrentBrightness, Levels
'@
}

if ($RunTests) {
    Write-Host '=== Running D3D11 + OpenGL 4.6 tests ==='
    Invoke-S23Link exec -t 60 'C:\s23gpu\s23gpu_d3d11_test.exe'
    Invoke-S23Link exec -t 60 'C:\s23gpu\s23gpu_opengl_test.exe'
}

Write-Host '=== S23 GPU deploy complete ==='
