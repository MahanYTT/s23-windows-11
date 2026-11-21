$ErrorActionPreference = 'Continue'
$script:logOff = 0L
$logPath = 'C:\Users\Public\s23_sys.txt'

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

W "=== s23_sys_install_v5.ps1 started (whoami=$(& whoami 2>&1)) ==="

# 0. Permanently disable Windows boot-time disk checking (autochk / chkdsk) on C:
W ("chkntfs /x C:: " + ((& chkntfs.exe /x C: 2>&1) -join " | "))
& reg.exe add 'HKLM\SYSTEM\CurrentControlSet\Control\Session Manager' /v BootExecute /t REG_MULTI_SZ /d 'autocheck autochk /k:C *' /f 2>&1 | Out-Null
W "BootExecute set to skip C: autochk"

# 0b. Auto-run read-only s23disp_diag.sys (ActionMask=0x00) to capture live DISP_CC/MDSS/DSI0 state on every boot!
try {
    $dispSys = 'C:\Users\Public\s23disp_diag.sys'
    $dispLog = 'C:\Users\Public\s23disp_diag.log'
    if (Test-Path $dispSys) {
        if (-not (Test-Path $dispLog) -or ((Get-Item $dispLog).Length -lt 65536)) {
            [IO.File]::WriteAllBytes($dispLog, (New-Object byte[] 65536))
        }
        & sc.exe create s23disp_diag type= kernel start= demand binPath= $dispSys 2>&1 | Out-Null
        & reg.exe add 'HKLM\SYSTEM\CurrentControlSet\Services\s23disp_diag\Parameters' /v ActionMask /t REG_DWORD /d 0 /f 2>&1 | Out-Null
        $dOut = (& sc.exe start s23disp_diag 2>&1) -join " | "
        W "s23disp_diag read-only run: $dOut"
    }
} catch { W "s23disp_diag err: $_" }

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class S23SysDev {
  [DllImport("newdev.dll", SetLastError = true, CharSet = CharSet.Unicode)]
  public static extern bool UpdateDriverForPlugAndPlayDevicesW(IntPtr hwnd, string hwid, string inf, uint flags, out bool reboot);
}
"@ -ErrorAction SilentlyContinue

$drv = 'C:\s23_touch_pkg'
$repo = 'C:\Windows\System32\DriverStore\FileRepository'

# 1. Only upgrade ROOT\S23TOUCH if v1.0.0.13 is NOT already running!
$dev = Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -match 'S23TOUCH' }
$needUpd = $true
try {
    $rawCheck = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes('C:\Users\Public\s23touch.log'))
    if ($dev -and ($dev | Where-Object { $_.Status -eq 'OK' }) -and $rawCheck.Contains('v1.0.0.13')) { $needUpd = $false }
} catch {}

if ($needUpd) {
    W ("pnputil add-driver: " + ((& pnputil /add-driver "$drv\s23touch.inf" /install 2>&1) -join " | "))
    $dirs = Get-ChildItem -Path $repo -Directory -Filter 's23touch*' -ErrorAction SilentlyContinue
    if ($dirs) {
        foreach ($d in $dirs) {
            & icacls $d.FullName /grant "*S-1-1-0:(OI)(CI)RX" /grant "BUILTIN\Administrators:(OI)(CI)F" /grant "NT AUTHORITY\SYSTEM:(OI)(CI)F" /T 2>&1 | Out-Null
        }
    }
    $reboot = $false
    $ok = [S23SysDev]::UpdateDriverForPlugAndPlayDevicesW([IntPtr]::Zero, 'ROOT\S23TOUCH', "$drv\s23touch.inf", 5, [ref]$reboot)
    $err = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
    W "UpdateDriverForPlugAndPlayDevicesW (SYSTEM): ok=$ok err=$err reboot=$reboot"
} else {
    W "s23touch.sys v1.0.0.13 ALREADY running cleanly at boot! Skipping reload."
}

W ("sc query s23touch: " + ((& sc.exe query s23touch 2>&1) -join " | "))

# 1b. Ensure ROOT\S23BATT (s23batt.sys v1.0.0.1) is installed and running
$battDev = Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -match 'S23BATT' -and $_.Status -eq 'OK' }
if (-not $battDev -and (Test-Path 'C:\s23_batt_pkg\install_batt_live.ps1')) {
    W "Installing s23batt.sys v1.0.0.1 via install_batt_live.ps1..."
    $bOut = (& powershell.exe -NoProfile -ExecutionPolicy Bypass -File 'C:\s23_batt_pkg\install_batt_live.ps1' 2>&1) -join " | "
    W "s23batt install result: $bOut"
} else {
    W "s23batt.sys v1.0.0.1 ALREADY running cleanly at boot!"
}

# Pre-allocate 64KB C:\Users\Public\s23pcieprobe.log if not already 65536 bytes
try {
    $pLog = 'C:\Users\Public\s23pcieprobe.log'
    if (-not (Test-Path $pLog) -or ((Get-Item $pLog).Length -lt 65536)) {
        [IO.File]::WriteAllBytes($pLog, (New-Object byte[] 65536))
    }
} catch {}

# Signal s23_diag.ps1 now so it never triggers Fallback driver reload
Set-Content -Path 'C:\s23_sys_done.txt' -Value 'DONE' -Force -ErrorAction SilentlyContinue

# Wait 3s for s23touch.sys v1.0.0.13 worker to finish MAX77705 COM_USB + PTN3222 POR/tune + eUSB2 + QMP + QSCRATCH
Start-Sleep -Seconds 3

# 2. Ensure RoleSwitchMode=2 and non-blocklisted WinUSB DeviceInterfaceGUIDs {A523F001-729B-4A0E-9C14-B7117D33A817}
$F = 'HKLM\SYSTEM\CurrentControlSet\Control\USBFN'
$extProp = '9200000000010500010088000000070000002A0044006500760069006300650049006E007400650072006600610063006500470055004900440073000000500000007B00410035003200330046003000300031002D0037003200390042002D0034004100300045002D0039004300310034002D004200370031003100370044003300330041003800310037007D0000000000'
foreach ($p in @("$F\Configurations\S23", "$F\Configurations\Default", "$F\Interfaces\GENERICUSBFN")) {
    & reg.exe add $p /v MSOSExtendedPropertyDescriptor /t REG_BINARY /d $extProp /f 2>&1 | Out-Null
}
$K = 'HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\QCOM0C8B\0\Device Parameters'
$curRole = (Get-ItemProperty -Path 'HKLM:\SYSTEM\CurrentControlSet\Enum\ACPI\QCOM0C8B\0\Device Parameters' -Name RoleSwitchMode -ErrorAction SilentlyContinue).RoleSwitchMode
W "RoleSwitchMode=$curRole (MSOSExtendedPropertyDescriptor -> A523F001)"
if ($curRole -ne 2) {
    & reg.exe add $K /v RoleSwitchMode /t REG_DWORD /d 2 /f 2>&1 | Out-Null
}

# 3. Cleanly restart ACPI\QCOM0C8B\0 while NO GenericUSBFn handle is open and DBM_EN=0
$pnpOut = 'C:\Users\Public\pnp_restart.txt'
Remove-Item $pnpOut -Force -ErrorAction SilentlyContinue
$proc = Start-Process -FilePath 'pnputil.exe' -ArgumentList '/restart-device', 'ACPI\QCOM0C8B\0' -RedirectStandardOutput $pnpOut -NoNewWindow -PassThru
$exited = $proc.WaitForExit(15000)
if (-not $exited) {
    try { $proc.Kill() } catch {}
    W "pnputil restart QCOM0C8B TIMED OUT (>15s), killed"
} else {
    $po = ""
    try { $po = ((Get-Content $pnpOut -ErrorAction SilentlyContinue) -join " | ") } catch {}
    W "pnputil restart QCOM0C8B rc=$($proc.ExitCode): $po"
}
Start-Sleep -Seconds 2

# 4. Permanently re-register S23UsbAgent with AllowStartIfOnBatteries & start immediately!
Set-Content -Path 'C:\s23_v13_ready.txt' -Value 'READY' -Force -ErrorAction SilentlyContinue
try {
    $actA = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument '-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File C:\s23_usb_agent.ps1'
    $trgA = New-ScheduledTaskTrigger -AtStartup
    $prA  = New-ScheduledTaskPrincipal -UserId 'NT AUTHORITY\SYSTEM' -LogonType ServiceAccount -RunLevel Highest
    $stA  = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -StartWhenAvailable -ExecutionTimeLimit (New-TimeSpan -Days 30)
    Register-ScheduledTask -TaskName 'S23UsbAgent' -Action $actA -Trigger $trgA -Principal $prA -Settings $stA -Force | Out-Null
    W "Registered S23UsbAgent with AllowStartIfOnBatteries + DontStopIfGoingOnBatteries"
} catch { W "Register-ScheduledTask S23UsbAgent err: $_" }

$runningAgent = Get-CimInstance Win32_Process -ErrorAction SilentlyContinue | Where-Object { $_.CommandLine -match 's23_usb_agent\.ps1' }
if ($runningAgent) {
    W ("S23UsbAgent ALREADY running pid=" + (($runningAgent | ForEach-Object { $_.ProcessId }) -join ","))
} else {
    try {
        Start-ScheduledTask -TaskName 'S23UsbAgent'
        W "Start-ScheduledTask S23UsbAgent OK"
    } catch {
        W "Start-ScheduledTask err: $_ -- launching directly via Start-Process"
        Start-Process -FilePath 'powershell.exe' -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-WindowStyle', 'Hidden', '-File', 'C:\s23_usb_agent.ps1' -WindowStyle Hidden
    }
}

$pnp = Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.Class -match 'HID|USB|Mouse|Keyboard|Battery' -or $_.InstanceId -match 'S23|QCOM0C8B|URS|USBFN|UFX|HID' } | ForEach-Object { "$($_.Class)|$($_.Status)|$($_.Problem)|$($_.FriendlyName)|$($_.InstanceId)" }
W ("PnP: " + ($pnp -join " ;; "))
W "=== s23_sys_install_v5.ps1 finished ==="
