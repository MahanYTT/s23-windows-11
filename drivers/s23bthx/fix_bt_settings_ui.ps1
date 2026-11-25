<#
.SYNOPSIS
    Fixes Windows 11 Settings -> Bluetooth & devices not displaying paired or connected devices.
.DESCRIPTION
    When Windows Setup leaves OOBEInProgressDriverUpdatesPostponed=1 or ChildCompletion\oobeldr.exe=1,
    DeviceSetupManager (DsmSvc) starts in OOBE mode with 0 active worktime, leaving DeviceContainers
    with empty names/categories/icons. Clearing those stale flags and restarting DsmSvc restores full
    Bluetooth Settings UI device visibility without rebooting or disconnecting paired peripherals.
#>
$ErrorActionPreference = 'Continue'

Write-Output "=== Checking HKLM\SYSTEM\Setup OOBE flags ==="
Get-ItemProperty 'HKLM:\SYSTEM\Setup' -ErrorAction SilentlyContinue |
    Select-Object OOBEInProgress, OOBEInProgressDriverUpdatesPostponed, SetupType, SystemSetupInProgress |
    Format-List
Get-ItemProperty 'HKLM:\SYSTEM\Setup\Status\ChildCompletion' -ErrorAction SilentlyContinue | Format-List

Write-Output "=== Clearing stale OOBE flags ==="
& reg.exe add "HKLM\SYSTEM\Setup" /v OOBEInProgress /t REG_DWORD /d 0 /f 2>&1
& reg.exe add "HKLM\SYSTEM\Setup" /v OOBEInProgressDriverUpdatesPostponed /t REG_DWORD /d 0 /f 2>&1
& reg.exe add "HKLM\SYSTEM\Setup\Status\ChildCompletion" /v oobeldr.exe /t REG_DWORD /d 3 /f 2>&1

Write-Output "=== Restarting DsmSvc & starting user Bluetooth services ==="
& sc.exe stop DsmSvc 2>&1
Start-Sleep -Seconds 3
& sc.exe start DsmSvc 2>&1

Get-Service -Name 'BluetoothUserService_*','DeviceAssociationBrokerSvc_*' -ErrorAction SilentlyContinue | ForEach-Object {
    if ($_.Status -ne 'Running') {
        & sc.exe start $_.Name 2>&1
    }
}

Write-Output "Waiting 15s for DeviceSetupManager to service containers..."
Start-Sleep -Seconds 15

Write-Output "=== DeviceSetupManager/Admin recent events ==="
Get-WinEvent -LogName 'Microsoft-Windows-DeviceSetupManager/Admin' -MaxEvents 12 -ErrorAction SilentlyContinue |
    Format-Table TimeCreated, Id, Message -Wrap

Write-Output "=== WinRT Bluetooth Paired AEPs & DeviceContainers ==="
Add-Type -AssemblyName System.Runtime.WindowsRuntime
[void][Windows.Devices.Enumeration.DeviceInformation,Windows.Devices.Enumeration,ContentType=WindowsRuntime]
[void][Windows.Devices.Enumeration.DeviceInformationKind,Windows.Devices.Enumeration,ContentType=WindowsRuntime]
$asTask = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
    $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and
    $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1'
})[0]
function Await($op, $type) {
    $t = $asTask.MakeGenericMethod($type).Invoke($null, @($op))
    $t.Wait()
    return $t.Result
}

$dcProps = [System.Collections.Generic.List[string]]@(
    'System.ItemNameDisplay','System.Devices.Connected','System.Devices.Paired','System.Devices.LocalMachine'
)
$dcs = Await ([Windows.Devices.Enumeration.DeviceInformation]::FindAllAsync('', $dcProps, [Windows.Devices.Enumeration.DeviceInformationKind]::DeviceContainer)) ([Windows.Devices.Enumeration.DeviceInformationCollection])
foreach ($dc in $dcs) {
    $paired = $dc.Properties['System.Devices.Paired']
    $conn   = $dc.Properties['System.Devices.Connected']
    if ($paired -eq $true -or $dc.Name) {
        Write-Output ("  Container: Name='{0}' Paired={1} Connected={2} Id={3}" -f $dc.Name, $paired, $conn, $dc.Id)
    }
}
