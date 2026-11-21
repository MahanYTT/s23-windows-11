$ErrorActionPreference = 'Continue'
$pkg = 'C:\s23_batt_pkg'
$repo = 'C:\Windows\System32\DriverStore\FileRepository'

Write-Output "=== Installing s23batt live (whoami=$(& whoami 2>&1)) ==="

# Ensure test cert is trusted (already installed for s23touch, harmless idempotent check)
if (Test-Path "$pkg\s23test.cer") {
    & certutil -addstore -f Root "$pkg\s23test.cer" 2>&1 | Out-Null
    & certutil -addstore -f TrustedPublisher "$pkg\s23test.cer" 2>&1 | Out-Null
}

# Reset bounded 64KB kernel log
$zeros = New-Object byte[] 65536
[IO.File]::WriteAllBytes('C:\Users\Public\s23batt.log', $zeros)

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class S23BattInst {
  [StructLayout(LayoutKind.Sequential)] public struct SP_DEVINFO_DATA { public int cbSize; public Guid ClassGuid; public int DevInst; public IntPtr Reserved; }
  [DllImport("setupapi.dll", SetLastError = true)] public static extern IntPtr SetupDiCreateDeviceInfoList(ref Guid g, IntPtr h);
  [DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Unicode)] public static extern bool SetupDiCreateDeviceInfoW(IntPtr set, string name, ref Guid g, string desc, IntPtr h, uint flags, ref SP_DEVINFO_DATA d);
  [DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Unicode)] public static extern bool SetupDiSetDeviceRegistryPropertyW(IntPtr set, ref SP_DEVINFO_DATA d, uint prop, byte[] buf, int len);
  [DllImport("setupapi.dll", SetLastError = true)] public static extern bool SetupDiCallClassInstaller(uint fn, IntPtr set, ref SP_DEVINFO_DATA d);
  [DllImport("setupapi.dll", SetLastError = true)] public static extern bool SetupDiDestroyDeviceInfoList(IntPtr set);
  [DllImport("newdev.dll", SetLastError = true, CharSet = CharSet.Unicode)] public static extern bool UpdateDriverForPlugAndPlayDevicesW(IntPtr hwnd, string hwid, string inf, uint flags, out bool reboot);
}
'@

# Create ROOT\S23BATT devnode if absent (Class=Battery {72631e54-78a4-11d0-bcf7-00aa00b7b32a})
$exists = Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -match 'S23BATT' -or ($_.HardwareID -contains 'ROOT\S23BATT') }
if (-not $exists) {
    $g = [Guid]'72631e54-78a4-11d0-bcf7-00aa00b7b32a'
    $set = [S23BattInst]::SetupDiCreateDeviceInfoList([ref]$g, [IntPtr]::Zero)
    $d = New-Object S23BattInst+SP_DEVINFO_DATA
    $d.cbSize = [Runtime.InteropServices.Marshal]::SizeOf($d)
    $ok1 = [S23BattInst]::SetupDiCreateDeviceInfoW($set, 'S23BATT', [ref]$g, 'Samsung Galaxy S23 MAX77705 Battery', [IntPtr]::Zero, 1, [ref]$d)
    $e1 = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
    $hw = [Text.Encoding]::Unicode.GetBytes("ROOT\S23BATT`0`0")
    $ok2 = [S23BattInst]::SetupDiSetDeviceRegistryPropertyW($set, [ref]$d, 1, $hw, $hw.Length)
    $e2 = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
    $ok3 = [S23BattInst]::SetupDiCallClassInstaller(0x19, $set, [ref]$d)
    $e3 = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
    [void][S23BattInst]::SetupDiDestroyDeviceInfoList($set)
    Write-Output "CreateRootDev: create=$ok1($e1) hwid=$ok2($e2) reg=$ok3($e3)"
} else {
    Write-Output "ROOT\S23BATT already exists: $($exists.InstanceId) status=$($exists.Status)"
}

# Stage INF in DriverStore (without /install) & ensure World/SYSTEM/Admin RX ACLs
Write-Output ("pnputil stage: " + ((& pnputil /add-driver "$pkg\s23batt.inf" 2>&1) -join " | "))
$dirs = Get-ChildItem -Path $repo -Directory -Filter 's23batt*' -ErrorAction SilentlyContinue
if ($dirs) {
    foreach ($dir in $dirs) {
        & icacls $dir.FullName /grant "*S-1-1-0:(OI)(CI)RX" /grant "BUILTIN\Administrators:(OI)(CI)F" /grant "NT AUTHORITY\SYSTEM:(OI)(CI)F" /T 2>&1 | Out-Null
    }
}

# Bind / upgrade driver non-interactively
$reboot = $false
$okUpd = [S23BattInst]::UpdateDriverForPlugAndPlayDevicesW([IntPtr]::Zero, 'ROOT\S23BATT', "$pkg\s23batt.inf", 5, [ref]$reboot)
$eUpd = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
Write-Output "UpdateDriverForPlugAndPlayDevicesW: ok=$okUpd err=$eUpd reboot=$reboot"

Start-Sleep -Seconds 3
Write-Output "=== sc query s23batt ==="
& sc.exe query s23batt 2>&1
Write-Output "=== PnP Battery Devices ==="
Get-PnpDevice -Class Battery -ErrorAction SilentlyContinue | Select-Object Status,Problem,Service,FriendlyName,InstanceId | Format-Table -AutoSize | Out-String -Width 220
Write-Output "=== Win32_Battery ==="
Get-CimInstance Win32_Battery -ErrorAction SilentlyContinue | Format-List *
