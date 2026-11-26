<#
.SYNOPSIS
    First-boot Windows 11 ARM64 driver, certificate, and power setup for S23 (`dm1q`).
    Invoked automatically by C:\Windows\Setup\Scripts\SetupComplete.cmd or manually as SYSTEM/Admin.
#>
$ErrorActionPreference = 'Continue'

$drvRoot = 'C:\s23_drivers'
$binDir  = 'C:\s23_drivers\bin'
$s23Dir  = 'C:\s23'
Write-Output '=== S23 First-Boot Provisioning ==='

# 1. Disable Windows automatic chkdsk scan, ensure testsigning on, and disable display idle-off timeout (prevents UEFI RTC black-screen trap)
& chkntfs /x C:
& bcdedit /set testsigning on 2>&1 | Out-Null
& powercfg /change monitor-timeout-ac 0
& powercfg /change monitor-timeout-dc 0
& powercfg /change standby-timeout-ac 0
& powercfg /change standby-timeout-dc 0

# 2. Import test-signing certificate (CN=S23TouchTest2020) if present
$certs = Get-ChildItem -Path $drvRoot -Filter '*.cer' -Recurse -ErrorAction SilentlyContinue
if ($certs) {
    $cer = $certs[0].FullName
    Write-Output "Importing cert: $cer"
    & certutil -addstore -f Root $cer | Out-Null
    & certutil -addstore -f TrustedPublisher $cer | Out-Null
}

# 3. Generate missing .cat files natively (e.g. if driver was signed on Linux via osslsigncode without inf2cat)
$certObj = Get-ChildItem Cert:\CurrentUser\My, Cert:\LocalMachine\My -ErrorAction SilentlyContinue |
    Where-Object { $_.Subject -eq 'CN=S23TouchTest2020' -and $_.HasPrivateKey } | Select-Object -First 1

$infs = Get-ChildItem -Path $drvRoot -Filter '*.inf' -Recurse -ErrorAction SilentlyContinue
foreach ($inf in $infs) {
    $pkgDir = $inf.DirectoryName
    if ($inf.BaseName -eq 's23gpu') {
        $umd = Join-Path $pkgDir 's23gpu_umd.dll'
        $wgl = Join-Path $pkgDir 'libgallium_wgl.dll'
        if (-not (Test-Path $umd) -or -not (Test-Path $wgl)) {
            Write-Output 'Skipping s23gpu.inf (drop Mesa Freedreno s23gpu_umd.dll & libgallium_wgl.dll alongside it first)'
            continue
        }
    }
    $catPath = Join-Path $pkgDir ($inf.BaseName + '.cat')
    if (-not (Test-Path $catPath) -and $certObj) {
        New-FileCatalog -Path $pkgDir -CatalogFilePath $catPath -CatalogVersion 2 | Out-Null
        Set-AuthenticodeSignature -FilePath $catPath -Certificate $certObj -HashAlgorithm SHA256 | Out-Null
    }
    Write-Output "Installing INF: $($inf.FullName)"
    & pnputil /add-driver $inf.FullName /install
}

# 4. Install s23modemd.exe to System32, register s23_fix_acl.cmd if present, and create Desktop SMS shortcut if staged
$aclCmd = Join-Path $drvRoot 's23_fix_acl.cmd'
if (Test-Path $aclCmd) {
    New-Item -ItemType Directory -Force -Path $s23Dir | Out-Null
    Copy-Item $aclCmd (Join-Path $s23Dir 's23_fix_acl.cmd') -Force
    & schtasks /create /tn 'S23FixAcl' /tr 'C:\s23\s23_fix_acl.cmd' /sc onstart /ru SYSTEM /f 2>&1 | Out-Null
}

$modemdSrc = Join-Path $drvRoot 's23modem\s23modemd.exe'
if (-not (Test-Path $modemdSrc)) {
    $modemdSrc = Join-Path $binDir 's23modemd.exe'
}
if (Test-Path $modemdSrc) {
    Copy-Item $modemdSrc 'C:\Windows\System32\s23modemd.exe' -Force
    Write-Output 'Installed s23modemd.exe to C:\Windows\System32\s23modemd.exe'
}

$smsScript = Join-Path $drvRoot 's23sms.ps1'
if (-not (Test-Path $smsScript)) {
    $smsScript = Join-Path $binDir 's23sms.ps1'
}
if (Test-Path $smsScript) {
    New-Item -ItemType Directory -Force -Path 'C:\s23mbb_pkg' | Out-Null
    Copy-Item $smsScript 'C:\s23mbb_pkg\s23sms.ps1' -Force
    $desk = 'C:\Users\Public\Desktop\S23 SMS.lnk'
    $ws = New-Object -ComObject WScript.Shell
    $lnk = $ws.CreateShortcut($desk)
    $lnk.TargetPath = 'C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe'
    $lnk.Arguments = '-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -STA -File C:\s23mbb_pkg\s23sms.ps1'
    $lnk.WorkingDirectory = 'C:\s23mbb_pkg'
    $lnk.Save()
    Write-Output "Created Desktop shortcut: $desk"
}

# 5. Stage s23perfctl.ps1 and fix_bt_settings_ui.ps1 into C:\s23 if present in C:\s23_drivers\bin
New-Item -ItemType Directory -Force -Path $s23Dir | Out-Null
foreach ($helperScript in @('s23perfctl.ps1', 'fix_bt_settings_ui.ps1')) {
    $srcPath = Join-Path $binDir $helperScript
    if (-not (Test-Path $srcPath)) {
        $alt = Get-ChildItem -Path $drvRoot -Filter $helperScript -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($alt) { $srcPath = $alt.FullName }
    }
    if (Test-Path $srcPath) {
        $dstPath = Join-Path $s23Dir $helperScript
        Copy-Item $srcPath $dstPath -Force
        Write-Output "Staged $helperScript -> $dstPath"
    }
}

# 6. Install and configure s23perf.sys resident DVFS & 41-sensor TSENS thermal governor service (Mode=2)
$perfSysSrc = 'C:\s23_drivers\s23perf.sys'
if (-not (Test-Path $perfSysSrc)) {
    $perfSysSrc = 'C:\s23_drivers\s23perf\s23perf.sys'
}
if (Test-Path $perfSysSrc) {
    Copy-Item $perfSysSrc 'C:\Windows\System32\drivers\s23perf.sys' -Force
    & sc.exe create s23perf type= kernel start= auto binPath= "System32\drivers\s23perf.sys" 2>&1 | Out-Null
    & sc.exe config s23perf type= kernel start= auto binPath= "System32\drivers\s23perf.sys" 2>&1 | Out-Null
    $perfParamKey = 'HKLM:\SYSTEM\CurrentControlSet\Services\s23perf\Parameters'
    New-Item -Path $perfParamKey -Force | Out-Null
    Set-ItemProperty -Path $perfParamKey -Name 'Mode'      -Type DWord -Value 2
    Set-ItemProperty -Path $perfParamKey -Name 'Profile'   -Type DWord -Value 0
    Set-ItemProperty -Path $perfParamKey -Name 'PollMs'    -Type DWord -Value 100
    Set-ItemProperty -Path $perfParamKey -Name 'SoftTempC' -Type DWord -Value 68
    Set-ItemProperty -Path $perfParamKey -Name 'HardTempC' -Type DWord -Value 75
    Set-ItemProperty -Path $perfParamKey -Name 'CritTempC' -Type DWord -Value 80
    Write-Output 'Configured s23perf kernel service (Mode=2, Profile=0, PollMs=100, SoftTempC=68, HardTempC=75, CritTempC=80)'
}

# 7. Clear OOBE Bluetooth Settings UI lockout registry flags (DsmSvc container metadata unlock)
New-Item -Path 'HKLM:\SYSTEM\Setup' -Force -ErrorAction SilentlyContinue | Out-Null
Set-ItemProperty -Path 'HKLM:\SYSTEM\Setup' -Name 'OOBEInProgress' -Type DWord -Value 0 -ErrorAction SilentlyContinue
Set-ItemProperty -Path 'HKLM:\SYSTEM\Setup' -Name 'OOBEInProgressDriverUpdatesPostponed' -Type DWord -Value 0 -ErrorAction SilentlyContinue
New-Item -Path 'HKLM:\SYSTEM\Setup\Status\ChildCompletion' -Force -ErrorAction SilentlyContinue | Out-Null
Set-ItemProperty -Path 'HKLM:\SYSTEM\Setup\Status\ChildCompletion' -Name 'oobeldr.exe' -Type DWord -Value 3 -ErrorAction SilentlyContinue
Write-Output 'Cleared OOBE Bluetooth Settings UI lockout flags (OOBEInProgress=0, OOBEInProgressDriverUpdatesPostponed=0, ChildCompletion\oobeldr.exe=3)'

# 8. Invoke root-enumerated devnode helper executables if present under C:\s23_drivers\bin
foreach ($rootHelper in @('install_root_audio.exe', 'install_root_mbb.exe', 's23gpu_install.exe')) {
    $exePath = Join-Path $binDir $rootHelper
    if (-not (Test-Path $exePath)) {
        $altExe = Get-ChildItem -Path $drvRoot -Filter $rootHelper -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($altExe) { $exePath = $altExe.FullName }
    }
    if (Test-Path $exePath) {
        if ($rootHelper -eq 's23gpu_install.exe') {
            $gpuUmd = Get-ChildItem -Path $drvRoot -Filter 's23gpu_umd.dll' -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
            if (-not $gpuUmd) {
                Write-Output 'Skipping s23gpu_install.exe (Mesa s23gpu_umd.dll not staged yet)'
                continue
            }
        }
        Write-Output "Invoking root devnode helper: $exePath"
        & $exePath
    }
}

# 9. Optional post-provisioning reboot when S23_FIRSTBOOT_REBOOT=1
if ($env:S23_FIRSTBOOT_REBOOT -eq '1') {
    Write-Output 'Rebooting system to finalize S23 driver provisioning...'
    & shutdown.exe /r /t 5 /c "S23 First-Boot Provisioning Reboot"
}

Write-Output '=== S23 First-Boot Provisioning Complete ==='
