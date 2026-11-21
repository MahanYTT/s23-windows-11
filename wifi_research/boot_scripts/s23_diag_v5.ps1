$ErrorActionPreference = 'Continue'
$script:logOff = 0L
$logPath = 'C:\Users\Public\s23_diag.txt'

function W($m) {
    try {
        $bytes = [Text.Encoding]::ASCII.GetBytes("$(Get-Date -Format 'HH:mm:ss.fff') $m`r`n")
        $fs = New-Object IO.FileStream($logPath, [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::Write, [IO.FileShare]::ReadWrite, 4096, [IO.FileOptions]::WriteThrough)
        $fs.Position = $script:logOff
        $fs.Write($bytes, 0, $bytes.Length)
        $script:logOff = $fs.Position
        $fs.Flush()
        $fs.Close()
    } catch {}
}

W "=== s23_diag_v5.ps1 started (whoami=$(& whoami 2>&1)) ==="

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class S23Disp {
    [DllImport("kernel32.dll")] public static extern uint SetThreadExecutionState(uint esFlags);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")] public static extern void mouse_event(uint dwFlags, int dx, int dy, uint dwData, IntPtr dwExtraInfo);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
public static class S23DevInst {
  [StructLayout(LayoutKind.Sequential)] public struct SP_DEVINFO_DATA { public int cbSize; public Guid ClassGuid; public int DevInst; public IntPtr Reserved; }
  [DllImport("setupapi.dll", SetLastError = true)] public static extern IntPtr SetupDiCreateDeviceInfoList(ref Guid g, IntPtr h);
  [DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Unicode)] public static extern bool SetupDiCreateDeviceInfoW(IntPtr set, string name, ref Guid g, string desc, IntPtr h, uint flags, ref SP_DEVINFO_DATA d);
  [DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Unicode)] public static extern bool SetupDiSetDeviceRegistryPropertyW(IntPtr set, ref SP_DEVINFO_DATA d, uint prop, byte[] buf, int len);
  [DllImport("setupapi.dll", SetLastError = true)] public static extern bool SetupDiCallClassInstaller(uint fn, IntPtr set, ref SP_DEVINFO_DATA d);
  [DllImport("setupapi.dll", SetLastError = true)] public static extern bool SetupDiDestroyDeviceInfoList(IntPtr set);
  [DllImport("newdev.dll", SetLastError = true, CharSet = CharSet.Unicode)] public static extern bool UpdateDriverForPlugAndPlayDevicesW(IntPtr hwnd, string hwid, string inf, uint flags, out bool reboot);
}
"@ -ErrorAction SilentlyContinue

try {
    & powercfg /change monitor-timeout-ac 0 2>&1 | Out-Null
    & powercfg /change monitor-timeout-dc 0 2>&1 | Out-Null
    & powercfg /change standby-timeout-ac 0 2>&1 | Out-Null
    & powercfg /change standby-timeout-dc 0 2>&1 | Out-Null
    & powercfg /change hibernate-timeout-ac 0 2>&1 | Out-Null
    & powercfg /change hibernate-timeout-dc 0 2>&1 | Out-Null
    & powercfg /setacvalueindex SCHEME_CURRENT SUB_VIDEO VIDEOIDLE 0 2>&1 | Out-Null
    & powercfg /setdcvalueindex SCHEME_CURRENT SUB_VIDEO VIDEOIDLE 0 2>&1 | Out-Null
    & powercfg /setactive SCHEME_CURRENT 2>&1 | Out-Null
    W "powercfg timeouts disabled."
} catch { W "powercfg err: $_" }

try {
    [void][S23Disp]::SetProcessDPIAware()
    [void][S23Disp]::SetThreadExecutionState([uint32]::Parse('80000003', 'HexNumber'))
    [void][S23Disp]::mouse_event(1, 5, 5, 0, [IntPtr]::Zero)
    [void][S23Disp]::SendMessage([IntPtr]0xFFFF, 0x0112, [IntPtr]0xF170, [IntPtr](-1))
    W "Display wake calls sent."
} catch { W "Display wake err: $_" }

function TakeShot($outFile) {
    try {
        Add-Type -AssemblyName System.Windows.Forms, System.Drawing
        $b = [System.Windows.Forms.SystemInformation]::VirtualScreen
        $bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $g.CopyFromScreen($b.Left, $b.Top, 0, 0, $bmp.Size)
        $bmp.Save($outFile, [System.Drawing.Imaging.ImageFormat]::Png)
        $g.Dispose(); $bmp.Dispose()
        W "Saved $outFile ($($b.Width)x$($b.Height))"
    } catch { W "Shot $outFile err: $_" }
}

$drv = 'C:\s23_touch_pkg'
W ("certutil Root: " + ((& certutil -addstore -f Root "$drv\s23test.cer" 2>&1) -join " | "))
W ("certutil TrustedPublisher: " + ((& certutil -addstore -f TrustedPublisher "$drv\s23test.cer" 2>&1) -join " | "))

$exists = Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.HardwareID -contains 'ROOT\S23TOUCH' }
if (-not $exists) {
    $guid = [Guid]'745a17a0-74d3-11d0-b6fe-00a0c90f57da'
    $set = [S23DevInst]::SetupDiCreateDeviceInfoList([ref]$guid, [IntPtr]::Zero)
    $d = New-Object S23DevInst+SP_DEVINFO_DATA
    $d.cbSize = [Runtime.InteropServices.Marshal]::SizeOf($d)
    $ok1 = [S23DevInst]::SetupDiCreateDeviceInfoW($set, 'S23TOUCH', [ref]$guid, 'Samsung Galaxy S23 Native Touch Screen', [IntPtr]::Zero, 1, [ref]$d)
    $e1 = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
    $hw = [Text.Encoding]::Unicode.GetBytes("ROOT\S23TOUCH`0`0")
    $ok2 = [S23DevInst]::SetupDiSetDeviceRegistryPropertyW($set, [ref]$d, 1, $hw, $hw.Length)
    $e2 = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
    $ok3 = [S23DevInst]::SetupDiCallClassInstaller(0x19, $set, [ref]$d)
    $e3 = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
    [void][S23DevInst]::SetupDiDestroyDeviceInfoList($set)
    W "CreateRootDev: create=$ok1($e1) hwid=$ok2($e2) reg=$ok3($e3)"
} else {
    W "ROOT\S23TOUCH exists: $($exists.InstanceId) status=$($exists.Status)"
}

# Launch C:\s23_sys_install.ps1 IMMEDIATELY as NT AUTHORITY\SYSTEM with AllowStartIfOnBatteries!
Remove-Item 'C:\s23_sys_done.txt' -Force -ErrorAction SilentlyContinue
try {
    $act = New-ScheduledTaskAction -Execute 'C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe' -Argument '-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File C:\s23_sys_install.ps1'
    $pr  = New-ScheduledTaskPrincipal -UserId 'NT AUTHORITY\SYSTEM' -LogonType ServiceAccount -RunLevel Highest
    $st  = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -StartWhenAvailable -ExecutionTimeLimit (New-TimeSpan -Hours 1)
    Register-ScheduledTask -TaskName 'S23SysInst' -Action $act -Principal $pr -Settings $st -Force | Out-Null
    Start-ScheduledTask -TaskName 'S23SysInst'
    W "Register+Start S23SysInst (AllowStartIfOnBatteries) OK"
} catch {
    W "Register-ScheduledTask S23SysInst err: $_ -- falling back to schtasks"
    $tr = 'C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File C:\s23_sys_install.ps1'
    W ("schtasks create: " + ((& schtasks /create /tn S23SysInst /tr $tr /sc ONCE /st 00:00 /ru SYSTEM /rl HIGHEST /f 2>&1) -join " | "))
    W ("schtasks run: " + ((& schtasks /run /tn S23SysInst 2>&1) -join " | "))
}

$sysDone = $false
for ($i = 0; $i -lt 30; $i++) {
    if (Test-Path 'C:\s23_sys_done.txt') { $sysDone = $true; break }
    Start-Sleep -Seconds 1
}
W "S23SysInst done=$sysDone"
TakeShot "C:\Users\Public\s23_shot1.png"

if (-not $sysDone) {
    W "Fallback: running takeown + icacls + UpdateDriver from s23 admin..."
    W ("pnputil add: " + ((& pnputil /add-driver "$drv\s23touch.inf" /install 2>&1) -join " | "))
    $dirs = Get-ChildItem -Path 'C:\Windows\System32\DriverStore\FileRepository' -Directory -Filter 's23touch*' -ErrorAction SilentlyContinue
    foreach ($d in $dirs) {
        W ("takeown: " + ((& takeown /F $d.FullName /A /R /D Y 2>&1) -join " | "))
        W ("icacls: " + ((& icacls $d.FullName /grant "*S-1-1-0:(OI)(CI)RX" /grant "BUILTIN\Administrators:(OI)(CI)F" /grant "NT AUTHORITY\SYSTEM:(OI)(CI)F" /T 2>&1) -join " | "))
    }
    $reboot = $false
    $okUpd = [S23DevInst]::UpdateDriverForPlugAndPlayDevicesW([IntPtr]::Zero, 'ROOT\S23TOUCH', "$drv\s23touch.inf", 5, [ref]$reboot)
    $eUpd = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
    W "Fallback UpdateDriver: ok=$okUpd err=$eUpd reboot=$reboot"
}

Start-Sleep -Seconds 3
W ("sc query s23touch: " + ((& sc.exe query s23touch 2>&1) -join " | "))
$pnp = Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.Class -match 'HID|USB|Mouse|Keyboard|Battery' -or $_.InstanceId -match 'S23|QCOM0C8B|URS|USB|HID' } | ForEach-Object { "$($_.Class)|$($_.Status)|$($_.Problem)|$($_.FriendlyName)|$($_.InstanceId)" }
W ("PnP final: " + ($pnp -join " ;; "))
TakeShot "C:\Users\Public\s23_shot2.png"
W "=== s23_diag_v5.ps1 finished (holding display awake) ==="
while ($true) {
    try {
        [void][S23Disp]::SetThreadExecutionState([uint32]::Parse('80000003', 'HexNumber'))
        [void][S23Disp]::SendMessage([IntPtr]0xFFFF, 0x0112, [IntPtr]0xF170, [IntPtr](-1))
    } catch {}
    Start-Sleep -Seconds 30
}
