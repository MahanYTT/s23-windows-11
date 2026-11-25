<#
.SYNOPSIS
    Installs and starts the signed s23perf.sys v2.0.0.0 dynamic DVFS + TSENS governor driver.
#>
param(
    [ValidateSet('balanced', 'powersave', 'performance')]
    [string]$Profile = 'balanced',
    [switch]$DemandStart,
    [int]$PollMs = 100,
    [int]$SoftTempC = 68,
    [int]$HardTempC = 75,
    [int]$CritTempC = 80
)

$ErrorActionPreference = 'Continue'
$drvDir = $PSScriptRoot
$pkgSys = Join-Path $drvDir 'pkg\s23perf.sys'
$pkgCer = Join-Path $drvDir 'pkg\s23test.cer'
$dstSys = 'C:\Windows\System32\drivers\s23perf.sys'
$logFile = 'C:\Users\Public\s23perf.log'

if (-not (Test-Path $pkgSys)) {
    throw "Signed binary not found at $pkgSys. Build with WSL python3 build_wsl.py and sign with sign.ps1 first."
}

if (Test-Path $pkgCer) {
    & certutil.exe -addstore -f Root $pkgCer 2>&1 | Out-Null
    & certutil.exe -addstore -f TrustedPublisher $pkgCer 2>&1 | Out-Null
}

& sc.exe stop s23perf 2>&1 | Out-Null
Start-Sleep -Milliseconds 500

Copy-Item $pkgSys $dstSys -Force
Write-Output "Staged $dstSys ($((Get-Item $dstSys).Length) bytes)"

# Reset bounded 64 KB kernel log
[IO.File]::WriteAllBytes($logFile, (New-Object byte[] 65536))

$startType = if ($DemandStart) { 'demand' } else { 'auto' }
& sc.exe delete s23perf 2>&1 | Out-Null
Start-Sleep -Milliseconds 300
& sc.exe create s23perf type= kernel start= $startType binPath= "\SystemRoot\System32\drivers\s23perf.sys" DisplayName= "S23 EPSS CPUfreq & TSENS Governor" 2>&1

$profMap = @{ balanced = 0; powersave = 1; performance = 2 }
$regBase = 'HKLM\SYSTEM\CurrentControlSet\Services\s23perf\Parameters'
& reg.exe add $regBase /v Mode /t REG_DWORD /d 2 /f 2>&1 | Out-Null
& reg.exe add $regBase /v Profile /t REG_DWORD /d $profMap[$Profile] /f 2>&1 | Out-Null
& reg.exe add $regBase /v PollMs /t REG_DWORD /d $PollMs /f 2>&1 | Out-Null
& reg.exe add $regBase /v SoftTempC /t REG_DWORD /d $SoftTempC /f 2>&1 | Out-Null
& reg.exe add $regBase /v HardTempC /t REG_DWORD /d $HardTempC /f 2>&1 | Out-Null
& reg.exe add $regBase /v CritTempC /t REG_DWORD /d $CritTempC /f 2>&1 | Out-Null

Write-Output "=== Starting s23perf (Mode=2, Profile=$Profile, start=$startType) ==="
& sc.exe start s23perf 2>&1
Start-Sleep -Milliseconds 500
& sc.exe query s23perf 2>&1

$ctl = Join-Path $drvDir 's23perfctl.ps1'
if (Test-Path $ctl) {
    Write-Output "=== Live Telemetry ==="
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $ctl status
}
