# Run elevated on PC once to replace WinUSB's blocklisted GUID {DEE824EF-...}
# on the live S23 MI_00 device with {A523F001-729B-4A0E-9C14-B7117D33A817} and restart WinUSB.
$guid = '{A523F001-729B-4A0E-9C14-B7117D33A817}'
$devs = Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -match '^USB\\VID_1209&PID_0004&MI_00\\' }
foreach ($d in $devs) {
    $reg = "HKLM:\SYSTEM\CurrentControlSet\Enum\$($d.InstanceId)\Device Parameters"
    Set-ItemProperty -Path $reg -Name 'DeviceInterfaceGUIDs' -Type MultiString -Value @($guid)
    Write-Output "Updated $reg -> $guid"
    pnputil /restart-device $d.InstanceId
}
Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -match 'VID_1209' } | Format-Table Status,Problem,Service,InstanceId -AutoSize
