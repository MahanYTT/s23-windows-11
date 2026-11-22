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
    $full = "`$ProgressPreference = 'SilentlyContinue'; " + $Script
    $enc = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($full))
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & python $s23link exec -t $Timeout "powershell -NoProfile -NonInteractive -EncodedCommand $enc"
    $code = $LASTEXITCODE
    $ErrorActionPreference = $prevEap
    if ($code -ne 0) {
        throw "Remote PowerShell failed ($code)"
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

Write-Host '=== Configuring Adreno 740 UMD DWM-only & 2x2 quad scissor defaults ==='
Invoke-S23PS @"
New-Item -ItemType Directory -Force -Path 'C:\topaz','C:\ProgramData\topaz' | Out-Null
Remove-Item 'C:\ProgramData\topaz\umd.log.enable','C:\ProgramData\topaz\umd-*.log' -Force -ErrorAction SilentlyContinue
Set-Content -Path 'C:\topaz\umd.dwmonly' -Value '1' -Encoding ascii
Set-Content -Path 'C:\ProgramData\topaz\umd.env' -Value "FD_MESA_DEBUG=sysmem`r`nTOPAZ_SCFIX=1`r`nTOPAZ_STRIPS=0`r`nTOPAZ_NOPRESENTWAIT=0`r`nTOPAZ_UPDFLUSH=0`r`nTOPAZ_UPDSYNC=1`r`nTOPAZ_SVSWZ=1`r`nTOPAZ_SVINFO=1`r`nTOPAZ_DEMOTE=1`r`n" -Encoding ascii
[Environment]::SetEnvironmentVariable('TOPAZ_SCFIX', `$null, 'Machine')
[Environment]::SetEnvironmentVariable('TOPAZ_NOPRESENTWAIT', `$null, 'Machine')
if (Test-Path 'C:\Windows\System32\s23gpu_umd.dll') {
    `$curUmd = (Get-FileHash 'C:\Windows\System32\s23gpu_umd.dll' -Algorithm MD5).Hash
    if (`$curUmd -ne '$localUmdMd5') {
        Get-ChildItem 'C:\Windows\System32\s23gpu_umd.dll.old*' -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
        `$bak = "C:\Windows\System32\s23gpu_umd.dll.old." + [Guid]::NewGuid().ToString('N').Substring(0, 8)
        Move-Item 'C:\Windows\System32\s23gpu_umd.dll' `$bak -Force
        Copy-Item 'C:\s23gpu\s23gpu_umd.dll' 'C:\Windows\System32\s23gpu_umd.dll' -Force
    }
}
if (Test-Path 'C:\Windows\System32\drivers\s23gpu.sys') {
    `$curSys = (Get-FileHash 'C:\Windows\System32\drivers\s23gpu.sys' -Algorithm MD5).Hash
    if (`$curSys -ne '$localSysMd5') {
        Get-ChildItem 'C:\Windows\System32\drivers\s23gpu.sys.old*' -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
        `$sbak = "C:\Windows\System32\drivers\s23gpu.sys.old." + [Guid]::NewGuid().ToString('N').Substring(0, 8)
        Move-Item 'C:\Windows\System32\drivers\s23gpu.sys' `$sbak -Force -ErrorAction SilentlyContinue
        Copy-Item 'C:\s23gpu\s23gpu.sys' 'C:\Windows\System32\drivers\s23gpu.sys' -Force
    }
}
"@

Write-Host '=== Installing driver ==='
Invoke-S23Link exec -t 60 'C:\s23gpu\s23gpu_install.exe C:\s23gpu\s23gpu.inf'

Write-Host '=== Verifying System32 hash & PnP device ==='
Invoke-S23PS @"
`$sysMd5 = (Get-FileHash C:\Windows\System32\drivers\s23gpu.sys -Algorithm MD5).Hash
if (`$sysMd5 -ne '$localSysMd5') {
    Get-ChildItem 'C:\Windows\System32\drivers\s23gpu.sys.old*' -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
    `$sbak = "C:\Windows\System32\drivers\s23gpu.sys.old." + [Guid]::NewGuid().ToString('N').Substring(0, 8)
    Move-Item 'C:\Windows\System32\drivers\s23gpu.sys' `$sbak -Force -ErrorAction SilentlyContinue
    Copy-Item 'C:\s23gpu\s23gpu.sys' 'C:\Windows\System32\drivers\s23gpu.sys' -Force
    Disable-PnpDevice -InstanceId 'ROOT\DISPLAY\0000' -Confirm:`$false
    Enable-PnpDevice -InstanceId 'ROOT\DISPLAY\0000' -Confirm:`$false
    `$sysMd5 = (Get-FileHash C:\Windows\System32\drivers\s23gpu.sys -Algorithm MD5).Hash
}
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
