<#
.SYNOPSIS
    Control & telemetry CLI for s23perf.sys v2.0.0.0 (\\.\S23Perf).
.EXAMPLE
    .\s23perfctl.ps1 status
    .\s23perfctl.ps1 lut
    .\s23perfctl.ps1 profile performance
    .\s23perfctl.ps1 profile fixed -Idx0 12 -Idx1 15 -Idx2 17
    .\s23perfctl.ps1 watch
#>
param(
    [Parameter(Position = 0)]
    [ValidateSet('status', 'lut', 'profile', 'watch')]
    [string]$Command = 'status',

    [Parameter(Position = 1)]
    [ValidateSet('balanced', 'powersave', 'performance', 'fixed')]
    [string]$ProfileName = 'balanced',

    [int]$Idx0 = -1,
    [int]$Idx1 = -1,
    [int]$Idx2 = -1,
    [int]$SoftTempC = 0,
    [int]$HardTempC = 0,
    [int]$CritTempC = 0,
    [int]$PollMs = 0
)

$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class S23PerfIoctl {
    public const uint IOCTL_GET_TELEMETRY = 0x80006000;
    public const uint IOCTL_SET_POLICY    = 0x8000A004;
    public const uint IOCTL_GET_LUT       = 0x80006008;

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    public static extern IntPtr CreateFileW(
        string lpFileName, uint dwDesiredAccess, uint dwShareMode,
        IntPtr lpSecurityAttributes, uint dwCreationDisposition,
        uint dwFlagsAndAttributes, IntPtr hTemplateFile);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool DeviceIoControl(
        IntPtr hDevice, uint dwIoControlCode,
        byte[] lpInBuffer, uint nInBufferSize,
        byte[] lpOutBuffer, uint nOutBufferSize,
        out uint lpBytesReturned, IntPtr lpOverlapped);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool CloseHandle(IntPtr hObject);
}
'@

function Open-S23Perf {
    param([uint32]$Access)
    $h = [S23PerfIoctl]::CreateFileW('\\.\S23Perf', $Access, 3, [IntPtr]::Zero, 3, 0x80, [IntPtr]::Zero)
    if ($h -eq [IntPtr]::new(-1) -or $h -eq [IntPtr]::Zero) {
        $err = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
        throw "Failed to open \\.\S23Perf (Win32 error $err). Ensure s23perf.sys v2.0.0.0 is running in Mode=2."
    }
    return $h
}

function Read-Telemetry {
    param([IntPtr]$Handle)
    $out = New-Object byte[] 216
    [uint32]$ret = 0
    if (-not [S23PerfIoctl]::DeviceIoControl($Handle, [S23PerfIoctl]::IOCTL_GET_TELEMETRY, $null, 0, $out, 216, [ref]$ret, [IntPtr]::Zero)) {
        $err = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
        throw "IOCTL_S23PERF_GET_TELEMETRY failed (Win32 error $err)"
    }
    $profNames = @('Balanced', 'Powersave', 'Performance', 'Fixed')
    $thermNames = @('Normal', 'SoftThrottle', 'HardThrottle', 'Critical', 'Failsafe')
    $domNames = @('Silver (CPU0..2)', 'Gold   (CPU3..6)', 'Prime  (CPU7)   ')

    $ver     = [BitConverter]::ToUInt32($out, 0)
    $mode    = [BitConverter]::ToUInt32($out, 4)
    $prof    = [BitConverter]::ToUInt32($out, 8)
    $pollMs  = [BitConverter]::ToUInt32($out, 12)
    $samples = [BitConverter]::ToUInt64($out, 16)

    $doms = for ($d = 0; $d -lt 3; $d++) {
        $o = 24 + $d * 48
        [PSCustomObject]@{
            Domain       = $domNames[$d]
            LutCount     = [BitConverter]::ToUInt32($out, $o + 0)
            NonBoostMax  = [BitConverter]::ToUInt32($out, $o + 4)
            VoteIdx      = [BitConverter]::ToUInt32($out, $o + 8)
            GrantedIdx   = [BitConverter]::ToUInt32($out, $o + 12)
            HwMHz        = [math]::Round([BitConverter]::ToUInt32($out, $o + 16) / 1000.0, 1)
            TargetMHz    = [math]::Round([BitConverter]::ToUInt32($out, $o + 20) / 1000.0, 1)
            Mv           = [BitConverter]::ToUInt32($out, $o + 24)
            LoadPct      = [BitConverter]::ToUInt32($out, $o + 28)
            MinMHz       = [math]::Round([BitConverter]::ToUInt32($out, $o + 32) / 1000.0, 1)
            MaxMHz       = [math]::Round([BitConverter]::ToUInt32($out, $o + 36) / 1000.0, 1)
            LmhEvents    = [BitConverter]::ToUInt32($out, $o + 44)
        }
    }

    $tOff = 24 + 3 * 48
    $tValid = [BitConverter]::ToUInt32($out, $tOff + 0)
    $tMin   = [BitConverter]::ToInt32($out, $tOff + 4) / 10.0
    $tMax   = [BitConverter]::ToInt32($out, $tOff + 8) / 10.0
    $tBank  = [BitConverter]::ToUInt32($out, $tOff + 12)
    $tSens  = [BitConverter]::ToUInt32($out, $tOff + 16)
    $tState = [BitConverter]::ToUInt32($out, $tOff + 20)
    $softC  = [BitConverter]::ToInt32($out, $tOff + 24) / 10.0
    $hardC  = [BitConverter]::ToInt32($out, $tOff + 28) / 10.0
    $critC  = [BitConverter]::ToInt32($out, $tOff + 32) / 10.0

    [PSCustomObject]@{
        Version      = ("0x{0:X8}" -f $ver)
        Mode         = $mode
        Profile      = if ($prof -lt $profNames.Count) { $profNames[$prof] } else { "$prof" }
        PollMs       = $pollMs
        Samples      = $samples
        ThermalState = if ($tState -lt $thermNames.Count) { $thermNames[$tState] } else { "$tState" }
        TsensValid   = $tValid
        TsensMinC    = $tMin
        TsensMaxC    = $tMax
        HotSensor    = "tsens$tBank[$tSens]"
        ThresholdsC  = "$softC / $hardC / $critC"
        Domains      = $doms
    }
}

$access = if ($Command -eq 'profile') { [Convert]::ToUInt32('C0000000', 16) } else { [Convert]::ToUInt32('80000000', 16) }
$h = Open-S23Perf -Access $access
try {
    switch ($Command) {
        'status' {
            $t = Read-Telemetry $h
            Write-Output ("s23perf ABI={0} Mode={1} Profile={2} Poll={3}ms Samples={4}" -f $t.Version, $t.Mode, $t.Profile, $t.PollMs, $t.Samples)
            Write-Output ("TSENS: {0}/41 valid | min={1:F1}C max={2:F1}C ({3}) | State={4} (Soft/Hard/Crit={5}C)" -f $t.TsensValid, $t.TsensMinC, $t.TsensMaxC, $t.HotSensor, $t.ThermalState, $t.ThresholdsC)
            $t.Domains | Format-Table Domain, LoadPct, HwMHz, TargetMHz, Mv, VoteIdx, GrantedIdx, MinMHz, MaxMHz, LmhEvents -AutoSize
        }
        'lut' {
            $names = @('Silver (CPU0..2)', 'Gold (CPU3..6)', 'Prime (CPU7)')
            for ($d = 0; $d -lt 3; $d++) {
                $inBuf = [BitConverter]::GetBytes([uint32]$d)
                $outBuf = New-Object byte[] 496
                [uint32]$ret = 0
                if (-not [S23PerfIoctl]::DeviceIoControl($h, [S23PerfIoctl]::IOCTL_GET_LUT, $inBuf, 4, $outBuf, 496, [ref]$ret, [IntPtr]::Zero)) {
                    throw "IOCTL_S23PERF_GET_LUT failed"
                }
                $cnt = [BitConverter]::ToUInt32($outBuf, 4)
                $nb  = [BitConverter]::ToUInt32($outBuf, 8)
                Write-Output "=== Domain ${d}: $($names[$d]) ($cnt entries, non-boost max idx=$nb) ==="
                $rows = for ($i = 0; $i -lt $cnt; $i++) {
                    $o = 16 + $i * 12
                    [PSCustomObject]@{
                        Idx   = $i
                        MHz   = [math]::Round([BitConverter]::ToUInt32($outBuf, $o) / 1000.0, 1)
                        mV    = [BitConverter]::ToUInt32($outBuf, $o + 4)
                        Src   = $outBuf[$o + 8]
                        LVal  = $outBuf[$o + 9]
                        Cores = $outBuf[$o + 10]
                    }
                }
                $rows | Format-Table -AutoSize
            }
        }
        'profile' {
            $profMap = @{ balanced = 0; powersave = 1; performance = 2; fixed = 3 }
            $req = New-Object byte[] 32
            [BitConverter]::GetBytes([uint32]$profMap[$ProfileName]).CopyTo($req, 0)
            $m = @($Idx0, $Idx1, $Idx2)
            for ($i = 0; $i -lt 3; $i++) {
                $v = if ($m[$i] -ge 0) { [uint32]$m[$i] } else { [uint32]::MaxValue }
                [BitConverter]::GetBytes($v).CopyTo($req, 4 + $i * 4)
            }
            [BitConverter]::GetBytes([uint32]$SoftTempC).CopyTo($req, 16)
            [BitConverter]::GetBytes([uint32]$HardTempC).CopyTo($req, 20)
            [BitConverter]::GetBytes([uint32]$CritTempC).CopyTo($req, 24)
            [BitConverter]::GetBytes([uint32]$PollMs).CopyTo($req, 28)

            [uint32]$ret = 0
            if (-not [S23PerfIoctl]::DeviceIoControl($h, [S23PerfIoctl]::IOCTL_SET_POLICY, $req, 32, $null, 0, [ref]$ret, [IntPtr]::Zero)) {
                $err = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
                throw "IOCTL_S23PERF_SET_POLICY failed (Win32 error $err)"
            }
            Write-Output "Updated s23perf policy -> Profile=$ProfileName"
            Start-Sleep -Milliseconds 250
            $t = Read-Telemetry $h
            $t.Domains | Format-Table Domain, LoadPct, HwMHz, TargetMHz, Mv, VoteIdx, GrantedIdx -AutoSize
        }
        'watch' {
            while ($true) {
                $t = Read-Telemetry $h
                Clear-Host
                Write-Output ("s23perf [{0}] Profile={1} Thermal={2} Max={3:F1}C ({4}) Samples={5}" -f (Get-Date -Format 'HH:mm:ss'), $t.Profile, $t.ThermalState, $t.TsensMaxC, $t.HotSensor, $t.Samples)
                $t.Domains | Format-Table Domain, LoadPct, HwMHz, TargetMHz, Mv, VoteIdx, GrantedIdx, LmhEvents -AutoSize
                Start-Sleep -Seconds 1
            }
        }
    }
} finally {
    [void][S23PerfIoctl]::CloseHandle($h)
}
