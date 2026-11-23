$ErrorActionPreference = 'Stop'

$dir       = $PSScriptRoot
$outDir    = Join-Path $dir 'out'
$pkgDir    = Join-Path $dir 'pkg'
$msvcRoot  = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207'
$msvcBin   = Join-Path $msvcRoot 'bin\Hostx64\arm64'
$msvcInc   = Join-Path $msvcRoot 'include'
$msvcLib   = Join-Path $msvcRoot 'lib\arm64'
$wdkInc    = 'C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0'
$wdkLib    = 'C:\Program Files (x86)\Windows Kits\10\Lib\10.0.26100.0'
$wdkBinX64 = 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64'
$wdkBinX86 = 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x86'

$cl        = Join-Path $msvcBin 'cl.exe'
$link      = Join-Path $msvcBin 'link.exe'
$armasm    = Join-Path $msvcBin 'armasm64.exe'
$dumpbin   = Join-Path $msvcBin 'dumpbin.exe'
$signtool  = Join-Path $wdkBinX64 'signtool.exe'
$inf2cat   = Join-Path $wdkBinX86 'inf2cat.exe'
$thumb     = '05DBD92557F4FC52EF2A5E5955789D8EED637C68'

$scratch    = 'C:\Users\Mahan\.gemini\antigravity\brain\69c04d84-af7a-419e-9781-d650ca2201d9\scratch'
$ntPhone    = $(if ($env:S23_NTOSKRNL_REF) { $env:S23_NTOSKRNL_REF } else { Join-Path $scratch 'ntoskrnl_phone.exe' })
$halPhone   = $(if ($env:S23_HAL_REF)      { $env:S23_HAL_REF }      else { Join-Path $scratch 'phone_dlls\hal.dll' })
$portclsRef = $(if ($env:S23_PORTCLS_REF)  { $env:S23_PORTCLS_REF }  else { Join-Path $scratch 'phone_dlls\portcls.sys' })
$ksRef      = $(if ($env:S23_KS_REF)       { $env:S23_KS_REF }       else { Join-Path $scratch 'phone_dlls\ks.sys' })
$verifyPy   = Join-Path (Split-Path $dir -Parent) 's23gpuprobe\verify_pe.py'

New-Item -ItemType Directory -Force -Path $outDir | Out-Null
New-Item -ItemType Directory -Force -Path $pkgDir | Out-Null
Remove-Item -Recurse -Force (Join-Path $pkgDir '*') -ErrorAction SilentlyContinue

Write-Host '=== 1. Assembling scm_arm64.asm ==='
& $armasm -o (Join-Path $outDir 'scm_arm64.obj') (Join-Path $dir 'scm_arm64.asm')
if ($LASTEXITCODE) { throw "armasm64 failed: $LASTEXITCODE" }

$kmDefs = @(
    '/D_ARM64_=1', '/DARM64=1', '/D_WIN64=1', '/DWINNT=1',
    '/D_WIN32_WINNT=0x0A00', '/DNTDDI_VERSION=0x0A000010',
    '/DPOOL_ZERO_DOWN_LEVEL_SUPPORT=1'
)
$kmInc = @("/I$wdkInc\km", "/I$wdkInc\km\crt", "/I$wdkInc\shared", "/I$dir")

Write-Host '=== 2. Compiling s23aud_hw.c (ARM64 C) ==='
& $cl /nologo /c /TC /kernel /Zl /GS- /W3 /WX /Oi /O2 @kmDefs @kmInc "/Fo$outDir\s23aud_hw.obj" (Join-Path $dir 's23aud_hw.c')
if ($LASTEXITCODE) { throw "cl.exe s23aud_hw.c failed: $LASTEXITCODE" }

Write-Host '=== 3. Compiling s23audio.cpp (ARM64 C++ PortCls) ==='
& $cl /nologo /c /TP /kernel /Zl /GS- /EHs-c- /GR- /Zc:sizedDealloc- /W3 /WX /Oi /O2 @kmDefs @kmInc "/Fo$outDir\s23audio.obj" (Join-Path $dir 's23audio.cpp')
if ($LASTEXITCODE) { throw "cl.exe s23audio.cpp failed: $LASTEXITCODE" }

Write-Host '=== 4. Linking s23audio.sys ==='
$sysOut = Join-Path $outDir 's23audio.sys'
& $link /NOLOGO /NODEFAULTLIB /DRIVER /SUBSYSTEM:NATIVE,10.00 /MACHINE:ARM64 `
    /ENTRY:DriverEntry /FIXED:NO /DYNAMICBASE /TSAWARE:NO /RELEASE /OPT:REF /OPT:ICF `
    /MERGE:_TEXT=.text "/OUT:$sysOut" `
    (Join-Path $outDir 's23audio.obj') `
    (Join-Path $outDir 's23aud_hw.obj') `
    (Join-Path $outDir 'scm_arm64.obj') `
    (Join-Path $wdkLib 'km\arm64\ntoskrnl.lib') `
    (Join-Path $wdkLib 'km\arm64\hal.lib') `
    (Join-Path $wdkLib 'km\arm64\portcls.lib') `
    (Join-Path $wdkLib 'km\arm64\libcntpr.lib')
if ($LASTEXITCODE) { throw "link.exe s23audio.sys failed: $LASTEXITCODE" }

Write-Host '=== 5. Staging & Signing pkg\s23audio.sys + s23audio.cat ==='
Copy-Item $sysOut (Join-Path $pkgDir 's23audio.sys') -Force
Copy-Item (Join-Path $dir 's23audio.inf') (Join-Path $pkgDir 's23audio.inf') -Force

& $signtool sign /fd SHA256 /sha1 $thumb (Join-Path $pkgDir 's23audio.sys')
if ($LASTEXITCODE) { throw "signtool s23audio.sys failed: $LASTEXITCODE" }

& $inf2cat "/driver:$pkgDir" /os:10_NI_ARM64,10_GE_ARM64 /uselocaltime
if ($LASTEXITCODE) { throw "inf2cat failed: $LASTEXITCODE" }

& $signtool sign /fd SHA256 /sha1 $thumb (Join-Path $pkgDir 's23audio.cat')
if ($LASTEXITCODE) { throw "signtool s23audio.cat failed: $LASTEXITCODE" }

Write-Host '=== 6. Verifying PE, Zero __chkstk, & Phone Imports (ntoskrnl/hal/portcls/ks) ==='
& python $verifyPy (Join-Path $pkgDir 's23audio.sys') $ntPhone $halPhone $portclsRef $ksRef
if ($LASTEXITCODE) { throw "verify_pe.py failed: $LASTEXITCODE" }

Write-Host '=== 7. Building ARM64 User-Mode Tools (Static CRT /MT) ==='
$umInc      = @("/I$msvcInc", "/I$wdkInc\um", "/I$wdkInc\shared", "/I$wdkInc\ucrt", "/I$dir")
$umLibPaths = @("/LIBPATH:$msvcLib", "/LIBPATH:$wdkLib\um\arm64", "/LIBPATH:$wdkLib\ucrt\arm64")

& $cl /nologo /MT /O2 /W3 /WX /DUNICODE /D_UNICODE @umInc `
    (Join-Path $dir 'install_root_audio.c') "/Fo$outDir\install_root_audio.obj" `
    /link /NOLOGO /MACHINE:ARM64 /SUBSYSTEM:CONSOLE @umLibPaths `
    setupapi.lib newdev.lib cfgmgr32.lib uuid.lib kernel32.lib `
    "/OUT:$outDir\install_root_audio.exe"
if ($LASTEXITCODE) { throw "cl.exe install_root_audio failed: $LASTEXITCODE" }

& $cl /nologo /MT /EHsc /std:c++17 /O2 /W3 /WX /DUNICODE /D_UNICODE @umInc `
    (Join-Path $dir 's23audio_test.cpp') "/Fo$outDir\s23audio_test.obj" `
    /link /NOLOGO /MACHINE:ARM64 /SUBSYSTEM:CONSOLE @umLibPaths `
    ole32.lib uuid.lib avrt.lib propsys.lib kernel32.lib `
    "/OUT:$outDir\s23audio_test.exe"
if ($LASTEXITCODE) { throw "cl.exe s23audio_test failed: $LASTEXITCODE" }

$badDeps = & $dumpbin /imports (Join-Path $outDir 'install_root_audio.exe') (Join-Path $outDir 's23audio_test.exe') |
    Select-String -Pattern 'vcruntime|msvcp|ucrtbase|api-ms-win-crt'
if ($badDeps) { throw "Unexpected dynamic CRT dependency: $badDeps" }

Write-Host '=== BUILD & VERIFICATION COMPLETE ==='
Get-ChildItem $pkgDir | Select-Object Name, Length
Get-ChildItem $outDir -Filter '*.exe' | Select-Object Name, Length
