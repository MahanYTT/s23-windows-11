@echo off
set LOG=C:\Users\Public\s23_fix_acl.log
echo === s23_fix_acl.cmd started %DATE% %TIME% === > %LOG%

echo [1] Root, C:\s23*, C:\Windows, C:\Windows\System32 (non-recursive) >> %LOG%
icacls C:\ /grant "*S-1-5-18:(OI)(CI)F" /grant "*S-1-5-32-544:(OI)(CI)F" /grant "*S-1-5-32-545:(OI)(CI)RX" /grant "*S-1-5-11:(OI)(CI)RX" /grant "*S-1-5-19:(OI)(CI)RX" /grant "*S-1-5-20:(OI)(CI)RX" /grant "*S-1-15-2-1:(OI)(CI)RX" /grant "*S-1-15-2-2:(OI)(CI)RX" /grant "*S-1-1-0:RX" /C /Q >> %LOG% 2>&1
icacls C:\s23_diag.ps1 /grant "*S-1-5-32-545:RX" /grant "*S-1-1-0:RX" /C /Q >> %LOG% 2>&1
icacls C:\s23_sys_install.ps1 /grant "*S-1-5-32-545:RX" /grant "*S-1-1-0:RX" /C /Q >> %LOG% 2>&1
icacls C:\s23_usb_agent.ps1 /grant "*S-1-5-32-545:RX" /grant "*S-1-1-0:RX" /C /Q >> %LOG% 2>&1
icacls C:\s23_touch_pkg /grant "*S-1-5-32-545:(OI)(CI)RX" /grant "*S-1-1-0:(OI)(CI)RX" /T /C /Q >> %LOG% 2>&1
icacls C:\s23_batt_pkg /grant "*S-1-5-32-545:(OI)(CI)RX" /grant "*S-1-1-0:(OI)(CI)RX" /T /C /Q >> %LOG% 2>&1
icacls C:\s23_wifi_pkg /grant "*S-1-5-32-545:(OI)(CI)RX" /grant "*S-1-1-0:(OI)(CI)RX" /T /C /Q >> %LOG% 2>&1
icacls C:\Windows /grant "*S-1-5-32-545:(OI)(CI)RX" /grant "*S-1-5-11:(OI)(CI)RX" /grant "*S-1-5-19:(OI)(CI)RX" /grant "*S-1-5-20:(OI)(CI)RX" /grant "*S-1-15-2-1:(OI)(CI)RX" /grant "*S-1-15-2-2:(OI)(CI)RX" /C /Q >> %LOG% 2>&1
icacls C:\Windows\System32 /grant "*S-1-5-32-545:(OI)(CI)RX" /grant "*S-1-5-11:(OI)(CI)RX" /grant "*S-1-5-19:(OI)(CI)RX" /grant "*S-1-5-20:(OI)(CI)RX" /grant "*S-1-15-2-1:(OI)(CI)RX" /grant "*S-1-15-2-2:(OI)(CI)RX" /C /Q >> %LOG% 2>&1

echo [2] ServiceProfiles (LocalService + NetworkService) >> %LOG%
icacls C:\Windows\ServiceProfiles /grant "*S-1-5-18:(OI)(CI)F" /grant "*S-1-5-32-544:(OI)(CI)F" /grant "*S-1-5-19:(OI)(CI)RX" /grant "*S-1-5-20:(OI)(CI)RX" /grant "*S-1-5-32-545:RX" /C /Q >> %LOG% 2>&1
icacls C:\Windows\ServiceProfiles\LocalService /grant "*S-1-5-18:(OI)(CI)F" /grant "*S-1-5-32-544:(OI)(CI)F" /grant "*S-1-5-19:(OI)(CI)F" /T /C /Q >> %LOG% 2>&1
icacls C:\Windows\ServiceProfiles\NetworkService /grant "*S-1-5-18:(OI)(CI)F" /grant "*S-1-5-32-544:(OI)(CI)F" /grant "*S-1-5-20:(OI)(CI)F" /T /C /Q >> %LOG% 2>&1

echo [3] Writable Windows runtime dirs >> %LOG%
icacls C:\Windows\Temp /grant "*S-1-5-18:(OI)(CI)F" /grant "*S-1-5-32-544:(OI)(CI)F" /grant "*S-1-5-32-545:(OI)(CI)M" /grant "*S-1-5-19:(OI)(CI)F" /grant "*S-1-5-20:(OI)(CI)F" /grant "*S-1-1-0:(OI)(CI)M" /T /C /Q >> %LOG% 2>&1
icacls C:\Windows\System32\winevt /grant "*S-1-5-18:(OI)(CI)F" /grant "*S-1-5-32-544:(OI)(CI)F" /grant "*S-1-5-19:(OI)(CI)F" /grant "*S-1-5-20:(OI)(CI)RX" /grant "*S-1-5-32-545:(OI)(CI)RX" /T /C /Q >> %LOG% 2>&1
icacls C:\Windows\System32\LogFiles /grant "*S-1-5-18:(OI)(CI)F" /grant "*S-1-5-32-544:(OI)(CI)F" /grant "*S-1-5-19:(OI)(CI)F" /grant "*S-1-5-20:(OI)(CI)F" /grant "*S-1-5-32-545:(OI)(CI)RX" /T /C /Q >> %LOG% 2>&1
icacls C:\Windows\System32\Tasks /grant "*S-1-5-18:(OI)(CI)F" /grant "*S-1-5-32-544:(OI)(CI)F" /grant "*S-1-5-19:(OI)(CI)RX" /grant "*S-1-5-20:(OI)(CI)RX" /grant "*S-1-5-32-545:(OI)(CI)RX" /T /C /Q >> %LOG% 2>&1
icacls C:\Windows\System32\catroot2 /grant "*S-1-5-18:(OI)(CI)F" /grant "*S-1-5-32-544:(OI)(CI)F" /grant "*S-1-5-20:(OI)(CI)F" /grant "*S-1-5-19:(OI)(CI)F" /grant "*S-1-5-32-545:(OI)(CI)RX" /T /C /Q >> %LOG% 2>&1
icacls C:\Windows\System32\spool /grant "*S-1-5-18:(OI)(CI)F" /grant "*S-1-5-32-544:(OI)(CI)F" /grant "*S-1-5-19:(OI)(CI)F" /grant "*S-1-5-20:(OI)(CI)F" /grant "*S-1-5-32-545:(OI)(CI)RX" /T /C /Q >> %LOG% 2>&1
icacls C:\Windows\System32\sru /grant "*S-1-5-18:(OI)(CI)F" /grant "*S-1-5-32-544:(OI)(CI)F" /grant "*S-1-5-19:(OI)(CI)F" /grant "*S-1-5-20:(OI)(CI)F" /T /C /Q >> %LOG% 2>&1

echo [4] C:\Users (Public, Default, s23) >> %LOG%
icacls C:\Users /grant "*S-1-5-18:(OI)(CI)F" /grant "*S-1-5-32-544:(OI)(CI)F" /grant "*S-1-5-32-545:RX" /grant "*S-1-5-11:RX" /grant "*S-1-1-0:RX" /grant "*S-1-15-2-1:RX" /grant "*S-1-15-2-2:RX" /C /Q >> %LOG% 2>&1
icacls C:\Users\Public /grant "*S-1-5-18:(OI)(CI)F" /grant "*S-1-5-32-544:(OI)(CI)F" /grant "*S-1-5-32-545:(OI)(CI)F" /grant "*S-1-1-0:(OI)(CI)F" /grant "*S-1-5-19:(OI)(CI)F" /grant "*S-1-5-20:(OI)(CI)F" /T /C /Q >> %LOG% 2>&1
icacls C:\Users\Default /grant "*S-1-5-18:(OI)(CI)F" /grant "*S-1-5-32-544:(OI)(CI)F" /grant "*S-1-5-32-545:(OI)(CI)RX" /grant "*S-1-1-0:(OI)(CI)RX" /T /C /Q >> %LOG% 2>&1
icacls C:\Users\s23 /grant "*S-1-5-18:(OI)(CI)F" /grant "*S-1-5-32-544:(OI)(CI)F" /grant "*S-1-5-21-90484302-2165722077-966411361-1000:(OI)(CI)F" /grant "*S-1-15-2-1:(OI)(CI)RX" /grant "*S-1-15-2-2:(OI)(CI)RX" /T /C /Q >> %LOG% 2>&1
icacls C:\Users\s23\AppData\Local\Packages /grant "*S-1-15-2-1:(OI)(CI)F" /grant "*S-1-15-2-2:(OI)(CI)F" /T /C /Q >> %LOG% 2>&1

echo [5] C:\ProgramData >> %LOG%
icacls C:\ProgramData /grant "*S-1-5-18:(OI)(CI)F" /grant "*S-1-5-32-544:(OI)(CI)F" /grant "*S-1-5-32-545:(OI)(CI)M" /grant "*S-1-5-11:(OI)(CI)M" /grant "*S-1-5-19:(OI)(CI)F" /grant "*S-1-5-20:(OI)(CI)F" /grant "*S-1-15-2-1:(OI)(CI)RX" /grant "*S-1-15-2-2:(OI)(CI)RX" /T /C /Q >> %LOG% 2>&1

echo [6] C:\Windows recursive RX >> %LOG%
icacls C:\Windows /grant "*S-1-5-32-545:(OI)(CI)RX" /grant "*S-1-5-11:(OI)(CI)RX" /grant "*S-1-5-19:(OI)(CI)RX" /grant "*S-1-5-20:(OI)(CI)RX" /grant "*S-1-15-2-1:(OI)(CI)RX" /grant "*S-1-15-2-2:(OI)(CI)RX" /T /C /Q >> %LOG% 2>&1

echo [7] Program Files recursive RX >> %LOG%
icacls "C:\Program Files" /grant "*S-1-5-32-545:(OI)(CI)RX" /grant "*S-1-5-11:(OI)(CI)RX" /grant "*S-1-5-19:(OI)(CI)RX" /grant "*S-1-5-20:(OI)(CI)RX" /grant "*S-1-15-2-1:(OI)(CI)RX" /grant "*S-1-15-2-2:(OI)(CI)RX" /T /C /Q >> %LOG% 2>&1
icacls "C:\Program Files (x86)" /grant "*S-1-5-32-545:(OI)(CI)RX" /grant "*S-1-5-11:(OI)(CI)RX" /grant "*S-1-5-19:(OI)(CI)RX" /grant "*S-1-5-20:(OI)(CI)RX" /grant "*S-1-15-2-1:(OI)(CI)RX" /grant "*S-1-15-2-2:(OI)(CI)RX" /T /C /Q >> %LOG% 2>&1

echo [8] Resetting SetupType=0, SystemSetupInProgress=0, CmdLine="" >> %LOG%
reg add HKLM\SYSTEM\Setup /v SetupType /t REG_DWORD /d 0 /f >> %LOG% 2>&1
reg add HKLM\SYSTEM\Setup /v SystemSetupInProgress /t REG_DWORD /d 0 /f >> %LOG% 2>&1
reg add HKLM\SYSTEM\Setup /v CmdLine /t REG_SZ /d "" /f >> %LOG% 2>&1

echo === s23_fix_acl.cmd FINISHED %DATE% %TIME% === >> %LOG%
exit 0
