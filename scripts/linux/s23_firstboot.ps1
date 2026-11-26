<#
.SYNOPSIS
    First-boot Windows 11 ARM64 driver, certificate, and power setup for S23 (`dm1q`).
    Invoked automatically by C:\Windows\Setup\Scripts\SetupComplete.cmd or manually as SYSTEM/Admin.
#>
$ErrorActionPreference = 'Continue'

$drvRoot = 'C:\s23_drivers'
Write-Output '=== S23 First-Boot Provisioning ==='

# 1. Disable Windows automatic chkdsk scan and display idle-off timeout (prevents UEFI RTC black-screen trap)
& chkntfs /x C:
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

# 4. Install s23modemd.exe to System32 and create Desktop SMS shortcut if staged
$modemdSrc = Join-Path $drvRoot 's23modem\s23modemd.exe'
if (Test-Path $modemdSrc) {
    Copy-Item $modemdSrc 'C:\Windows\System32\s23modemd.exe' -Force
    Write-Output 'Installed s23modemd.exe to C:\Windows\System32\s23modemd.exe'
}

$smsScript = Join-Path $drvRoot 's23sms.ps1'
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

Write-Output '=== S23 First-Boot Provisioning Complete ==='
