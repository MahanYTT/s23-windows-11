$ErrorActionPreference = 'Stop'

$dir      = 'C:\Users\Mahan\Documents\windows-on-android\s23_dm1q\drivers\s23audprobe'
$outDir   = Join-Path $dir 'out'
$msvcBin  = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207\bin\Hostx64\arm64'
$wdkInc   = 'C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0'
$wdkLib   = 'C:\Program Files (x86)\Windows Kits\10\Lib\10.0.26100.0'
$wdkBinX64 = 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64'

$cl       = Join-Path $msvcBin 'cl.exe'
$link     = Join-Path $msvcBin 'link.exe'
$armasm   = Join-Path $msvcBin 'armasm64.exe'
$signtool = Join-Path $wdkBinX64 'signtool.exe'
$thumb    = '05DBD92557F4FC52EF2A5E5955789D8EED637C68'
$ntPhone  = 'C:\Users\Mahan\.gemini\antigravity\brain\69c04d84-af7a-419e-9781-d650ca2201d9\scratch\ntoskrnl_phone.exe'
$halPhone = 'C:\Users\Mahan\.gemini\antigravity\brain\69c04d84-af7a-419e-9781-d650ca2201d9\scratch\phone_dlls\hal.dll'
$verifyPy = 'C:\Users\Mahan\Documents\windows-on-android\s23_dm1q\drivers\s23gpuprobe\verify_pe.py'

New-Item -ItemType Directory -Force -Path $outDir | Out-Null
Remove-Item -Recurse -Force (Join-Path $outDir '*') -ErrorAction SilentlyContinue

Write-Host '=== Assembling scm_arm64.asm ==='
& $armasm -o (Join-Path $outDir 'scm_arm64.obj') (Join-Path $dir 'scm_arm64.asm')
if ($LASTEXITCODE) { throw "armasm64 failed: $LASTEXITCODE" }

Write-Host '=== Compiling s23audprobe.c (ARM64) ==='
$kmDefs = @('/D_ARM64_=1', '/DARM64=1', '/D_WIN64=1', '/DWINNT=1', '/D_WIN32_WINNT=0x0A00', '/DNTDDI_VERSION=0x0A000010', '/DPOOL_ZERO_DOWN_LEVEL_SUPPORT=1')
$kmInc  = @("/I$wdkInc\km", "/I$wdkInc\km\crt", "/I$wdkInc\shared")
& $cl /nologo /c /kernel /Zl /GS- /W3 /WX /Oi /O2 @kmDefs @kmInc "/Fo$outDir\" (Join-Path $dir 's23audprobe.c')
if ($LASTEXITCODE) { throw "cl.exe failed: $LASTEXITCODE" }

Write-Host '=== Linking s23audprobe.sys ==='
$sysOut = Join-Path $outDir 's23audprobe.sys'
& $link /NOLOGO /NODEFAULTLIB /DRIVER /SUBSYSTEM:NATIVE,10.00 /MACHINE:ARM64 /ENTRY:DriverEntry /FIXED:NO /DYNAMICBASE /TSAWARE:NO /RELEASE /OPT:REF /OPT:ICF /MERGE:_TEXT=.text "/OUT:$sysOut" (Join-Path $outDir 's23audprobe.obj') (Join-Path $outDir 'scm_arm64.obj') (Join-Path $wdkLib 'km\arm64\ntoskrnl.lib') (Join-Path $wdkLib 'km\arm64\hal.lib') (Join-Path $wdkLib 'km\arm64\libcntpr.lib')
if ($LASTEXITCODE) { throw "link.exe failed: $LASTEXITCODE" }

Write-Host '=== Signing s23audprobe.sys ==='
& $signtool sign /fd SHA256 /sha1 $thumb $sysOut
if ($LASTEXITCODE) { throw "signtool failed: $LASTEXITCODE" }

Write-Host '=== Verifying PE & S23 ntoskrnl.exe imports ==='
& python $verifyPy $sysOut $ntPhone $halPhone
if ($LASTEXITCODE) { throw "verify_pe.py failed: $LASTEXITCODE" }

Write-Host "BUILT & VERIFIED: $sysOut"
