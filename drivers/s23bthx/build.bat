@echo off
setlocal

set WDK_ROOT=C:\Program Files (x86)\Windows Kits\10
set WDK_VERSION=10.0.26100.0

if "%PROCESSOR_ARCHITECTURE%"=="ARM64" (
    set WDK_ARCH=ARM64
) else (
    set WDK_ARCH=x64
)

echo Building S23Bthx driver...

"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\cl.exe" /c /Gz /kernel /O2 /Oi /W4 /DWINNT=1 /D_WIN32_WINNT=0x0A00 /DWINVER=0x0A00 /D_ARM64_ /DKMDF_VERSION_MAJOR=1 /DKMDF_VERSION_MINOR=33 /I"%WDK_ROOT%\Include\%WDK_VERSION%\km" /I"%WDK_ROOT%\Include\%WDK_VERSION%\km\crt" /I"%WDK_ROOT%\Include\%WDK_VERSION%\shared" /I"%WDK_ROOT%\Include\wdf\kmdf\1.33" /I"..\.." s23bthx.c s23bthx_uart.c s23bthx_ibs.c

if %ERRORLEVEL% NEQ 0 (
    echo Compile failed!
    exit /b %ERRORLEVEL%
)

"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\link.exe" /SUBSYSTEM:NATIVE /DRIVER /ENTRY:FxDriverEntry /NODEFAULTLIB /MERGE:_TEXT=.text /MERGE:_PAGE=PAGE /MERGE:.rdata=.text /OUT:s23bthx.sys s23bthx.obj s23bthx_uart.obj s23bthx_ibs.obj "%WDK_ROOT%\Lib\%WDK_VERSION%\km\%WDK_ARCH%\ntoskrnl.lib" "%WDK_ROOT%\Lib\%WDK_VERSION%\km\%WDK_ARCH%\hal.lib" "%WDK_ROOT%\Lib\%WDK_VERSION%\km\%WDK_ARCH%\wmilib.lib" "%WDK_ROOT%\Lib\wdf\kmdf\%WDK_ARCH%\1.33\WdfLdr.lib" "%WDK_ROOT%\Lib\wdf\kmdf\%WDK_ARCH%\1.33\WdfDriverEntry.lib" "%WDK_ROOT%\Lib\%WDK_VERSION%\km\%WDK_ARCH%\BufferOverflowK.lib"

if %ERRORLEVEL% NEQ 0 (
    echo Link failed!
    exit /b %ERRORLEVEL%
)

echo Success!
