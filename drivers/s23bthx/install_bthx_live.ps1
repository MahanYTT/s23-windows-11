$ErrorActionPreference = 'Continue'
$pkg = 'C:\s23_bthx_pkg'
$repo = 'C:\Windows\System32\DriverStore\FileRepository'

Write-Output "=== Installing s23bthx live (whoami=$(& whoami 2>&1)) ==="

if (Test-Path "$pkg\s23test.cer") {
    & certutil -addstore -f Root "$pkg\s23test.cer" 2>&1 | Out-Null
    & certutil -addstore -f TrustedPublisher "$pkg\s23test.cer" 2>&1 | Out-Null
}

# Ensure QCA6490 BT firmware & NVM blobs live in System32\drivers as well as Public
foreach ($fw in @('hpbtfw21.tlv', 'hpnv21g.bin')) {
    if (Test-Path "C:\Users\Public\$fw") {
        Copy-Item "C:\Users\Public\$fw" "C:\Windows\System32\drivers\$fw" -Force
        Write-Output "Staged C:\Windows\System32\drivers\$fw"
    }
}

# Reset bounded 64KB kernel log
$zeros = New-Object byte[] 65536
[IO.File]::WriteAllBytes('C:\Users\Public\s23bthx.log', $zeros)

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class S23BthxInst {
  [StructLayout(LayoutKind.Sequential)] public struct SP_DEVINFO_DATA { public int cbSize; public Guid ClassGuid; public int DevInst; public IntPtr Reserved; }
  [DllImport("setupapi.dll", SetLastError = true)] public static extern IntPtr SetupDiCreateDeviceInfoList(ref Guid g, IntPtr h);
  [DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Unicode)] public static extern bool SetupDiCreateDeviceInfoW(IntPtr set, string name, ref Guid g, string desc, IntPtr h, uint flags, ref SP_DEVINFO_DATA d);
  [DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Unicode)] public static extern bool SetupDiSetDeviceRegistryPropertyW(IntPtr set, ref SP_DEVINFO_DATA d, uint prop, byte[] buf, int len);
  [DllImport("setupapi.dll", SetLastError = true)] public static extern bool SetupDiCallClassInstaller(uint fn, IntPtr set, ref SP_DEVINFO_DATA d);
  [DllImport("setupapi.dll", SetLastError = true)] public static extern bool SetupDiDestroyDeviceInfoList(IntPtr set);
  [DllImport("newdev.dll", SetLastError = true, CharSet = CharSet.Unicode)] public static extern bool UpdateDriverForPlugAndPlayDevicesW(IntPtr hwnd, string hwid, string inf, uint flags, out bool reboot);
}
'@

$exists = Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -match 'S23BTHX' -or ($_.HardwareID -contains 'ROOT\S23BTHX') }
if (-not $exists) {
    $g = [Guid]'e0cbf06c-cd8b-4647-bb8a-263b43f0f974'
    $set = [S23BthxInst]::SetupDiCreateDeviceInfoList([ref]$g, [IntPtr]::Zero)
    $d = New-Object S23BthxInst+SP_DEVINFO_DATA
    $d.cbSize = [Runtime.InteropServices.Marshal]::SizeOf($d)
    $ok1 = [S23BthxInst]::SetupDiCreateDeviceInfoW($set, 'S23BTHX', [ref]$g, 'Samsung Galaxy S23 QCA6490 Bluetooth 5.3', [IntPtr]::Zero, 1, [ref]$d)
    $e1 = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
    $hw = [Text.Encoding]::Unicode.GetBytes("ROOT\S23BTHX`0`0")
    $ok2 = [S23BthxInst]::SetupDiSetDeviceRegistryPropertyW($set, [ref]$d, 1, $hw, $hw.Length)
    $e2 = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
    $ok3 = [S23BthxInst]::SetupDiCallClassInstaller(0x19, $set, [ref]$d)
    $e3 = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
    [void][S23BthxInst]::SetupDiDestroyDeviceInfoList($set)
    Write-Output "CreateRootDev: create=$ok1($e1) hwid=$ok2($e2) reg=$ok3($e3)"
} else {
    Write-Output "ROOT\S23BTHX already exists: $($exists.InstanceId) status=$($exists.Status)"
}

Write-Output ("pnputil stage: " + ((& pnputil /add-driver "$pkg\s23bthx.inf" 2>&1) -join " | "))
$dirs = Get-ChildItem -Path $repo -Directory -Filter 's23bthx*' -ErrorAction SilentlyContinue
if ($dirs) {
    foreach ($dir in $dirs) {
        & icacls $dir.FullName /grant "*S-1-1-0:(OI)(CI)RX" /grant "BUILTIN\Administrators:(OI)(CI)F" /grant "NT AUTHORITY\SYSTEM:(OI)(CI)F" /T 2>&1 | Out-Null
    }
}

$reboot = $false
$okUpd = [S23BthxInst]::UpdateDriverForPlugAndPlayDevicesW([IntPtr]::Zero, 'ROOT\S23BTHX', "$pkg\s23bthx.inf", 5, [ref]$reboot)
$eUpd = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
Write-Output "UpdateDriverForPlugAndPlayDevicesW: ok=$okUpd err=$eUpd reboot=$reboot"

Start-Sleep -Seconds 4
Write-Output "=== sc query s23bthx / BthMini / bthport ==="
& sc.exe query s23bthx 2>&1
& sc.exe query BthMini 2>&1
& sc.exe query bthserv 2>&1
Write-Output "=== Ensure DeviceSetupManager is out of OOBE mode (so Settings shows paired/connected devices) ==="
& reg.exe add "HKLM\SYSTEM\Setup" /v OOBEInProgress /t REG_DWORD /d 0 /f 2>&1 | Out-Null
& reg.exe add "HKLM\SYSTEM\Setup" /v OOBEInProgressDriverUpdatesPostponed /t REG_DWORD /d 0 /f 2>&1 | Out-Null
& reg.exe add "HKLM\SYSTEM\Setup\Status\ChildCompletion" /v oobeldr.exe /t REG_DWORD /d 3 /f 2>&1 | Out-Null
$lastDsm = Get-WinEvent -LogName 'Microsoft-Windows-DeviceSetupManager/Admin' -MaxEvents 10 -ErrorAction SilentlyContinue | Where-Object { $_.Id -in 100,109 } | Select-Object -First 1
if ($lastDsm -and $lastDsm.Message -match 'OOBE') {
    Write-Output "Restarting DsmSvc to exit OOBE mode..."
    & sc.exe stop DsmSvc 2>&1 | Out-Null
    Start-Sleep -Seconds 2
    & sc.exe start DsmSvc 2>&1 | Out-Null
}
& sc.exe start BluetoothUserService_478e9 2>&1 | Out-Null

Write-Output "=== PnP Bluetooth Devices ==="
Get-PnpDevice -Class Bluetooth -ErrorAction SilentlyContinue | Select-Object Status,Problem,Service,FriendlyName,InstanceId | Format-Table -AutoSize | Out-String -Width 220
