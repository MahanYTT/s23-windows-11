param(
    [string]$MsvcToolsDir   = $env:S23_MSVC_TOOLS_DIR,
    [string]$KitsRoot       = $env:S23_KITS_ROOT,
    [string]$WdkVersion     = $(if ($env:S23_WDK_VERSION) { $env:S23_WDK_VERSION } else { '10.0.26100.0' }),
    [string]$WdfInc         = $env:S23_WDF_INC,
    [string]$WdfLib         = $env:S23_WDF_LIB,
    [string]$WdkBinX86      = $env:S23_WDK_BIN_X86,
    [string]$WdkBinX64      = $env:S23_WDK_BIN_X64,
    [string]$CertThumbprint = $(if ($env:S23_CERT_THUMBPRINT) { $env:S23_CERT_THUMBPRINT } else { '05DBD92557F4FC52EF2A5E5955789D8EED637C68' })
)
$ErrorActionPreference = 'Stop'

function Find-MsvcToolsDir {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $vs = $null
        try { $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.ARM64 -property installationPath 2>$null | Select-Object -First 1 } catch {}
        if ($vs) {
            $verFile = Join-Path $vs 'VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt'
            if (Test-Path $verFile) {
                $tools = Join-Path $vs ('VC\Tools\MSVC\' + (Get-Content $verFile -TotalCount 1).Trim())
                if (Test-Path (Join-Path $tools 'bin\Hostx64\arm64\cl.exe')) { return $tools }
            }
        }
    }
    return 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207'
}

function Find-KitsRoot {
    try {
        $root = (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots' -Name KitsRoot10 -ErrorAction Stop).KitsRoot10
        if ($root -and (Test-Path $root)) { return $root }
    } catch {}
    return 'C:\Program Files (x86)\Windows Kits\10\'
}

if (-not $MsvcToolsDir) { $MsvcToolsDir = Find-MsvcToolsDir }
if (-not $KitsRoot)     { $KitsRoot     = Find-KitsRoot }

$dir       = $PSScriptRoot
$outDir    = Join-Path $dir 'out'
$pkgDir    = Join-Path $dir 'pkg'
$modemDir  = Join-Path (Split-Path $dir -Parent) 's23modem'
$msvcRoot  = $MsvcToolsDir
$msvcBin   = Join-Path $msvcRoot 'bin\Hostx64\arm64'
$msvcInc   = Join-Path $msvcRoot 'include'
$msvcLib   = Join-Path $msvcRoot 'lib\arm64'
$wdkInc    = Join-Path $KitsRoot "Include\$WdkVersion"
$wdkLib    = Join-Path $KitsRoot "Lib\$WdkVersion"
$wdfInc    = if ($WdfInc) { $WdfInc } elseif (Test-Path (Join-Path $wdkInc 'wdf\kmdf\1.33')) { Join-Path $wdkInc 'wdf\kmdf\1.33' } else { Join-Path $KitsRoot 'Include\wdf\kmdf\1.33' }
$wdfLib    = if ($WdfLib) { $WdfLib } elseif (Test-Path (Join-Path $wdkLib 'wdf\kmdf\arm64\1.33')) { Join-Path $wdkLib 'wdf\kmdf\arm64\1.33' } else { Join-Path $KitsRoot 'Lib\wdf\kmdf\arm64\1.33' }
$wdkBinX64 = if ($WdkBinX64) { $WdkBinX64 } else { Join-Path $KitsRoot "bin\$WdkVersion\x64" }
$wdkBinX86 = if ($WdkBinX86) { $WdkBinX86 } else { Join-Path $KitsRoot "bin\$WdkVersion\x86" }

$cl        = Join-Path $msvcBin 'cl.exe'
$link      = Join-Path $msvcBin 'link.exe'
$signtool  = Join-Path $wdkBinX64 'signtool.exe'
$inf2cat   = Join-Path $wdkBinX86 'inf2cat.exe'
$thumb     = $CertThumbprint

$scratch    = 'C:\Users\Mahan\.gemini\antigravity\brain\69c04d84-af7a-419e-9781-d650ca2201d9\scratch'
$ntPhone    = $(if ($env:S23_NTOSKRNL_REF) { $env:S23_NTOSKRNL_REF } else { Join-Path $scratch 'ntoskrnl_phone.exe' })
$halPhone   = $(if ($env:S23_HAL_REF)      { $env:S23_HAL_REF }      else { Join-Path $scratch 'phone_dlls\hal.dll' })
$wdfldrRef  = $(if ($env:S23_WDFLDR_REF)   { $env:S23_WDFLDR_REF }   else { Join-Path $scratch 'phone_dlls\wdfldr.sys' })
$verifyPy   = Join-Path (Split-Path $dir -Parent) 's23gpuprobe\verify_pe.py'

New-Item -ItemType Directory -Force -Path $outDir | Out-Null
New-Item -ItemType Directory -Force -Path $pkgDir | Out-Null
Remove-Item -Recurse -Force (Join-Path $pkgDir '*') -ErrorAction SilentlyContinue

$kmDefs = @(
    '/D_ARM64_=1', '/DARM64=1', '/D_WIN64=1', '/DWINNT=1',
    '/D_WIN32_WINNT=0x0A00', '/DNTDDI_VERSION=0x0A000010',
    '/DPOOL_ZERO_DOWN_LEVEL_SUPPORT=1',
    '/DKMDF_VERSION_MAJOR=1', '/DKMDF_VERSION_MINOR=33', '/DKMDF_MINIMUM_VERSION_REQUIRED=33'
)
$kmInc = @(
    "/I$wdkInc\km", "/I$wdkInc\km\crt", "/I$wdkInc\shared", "/I$wdfInc",
    "/I$wdkInc\km\netcx\kmdf\adapter\2.0",
    "/I$wdkInc\shared\netcx\shared\1.0",
    "/I$wdkInc\km\mbbcx\1.0",
    "/I$dir", "/I$modemDir"
)

Write-Host '=== 1. Compiling s23mbb.c (ARM64 KMDF 1.33 + NetAdapterCx 2.0 + MbbCx 1.0) ==='
& $cl /nologo /c /TC /kernel /Zl /GS- /W3 /WX /Oi /O2 @kmDefs @kmInc "/Fo$outDir\s23mbb.obj" (Join-Path $dir 's23mbb.c')
if ($LASTEXITCODE) { throw "cl.exe s23mbb.c failed: $LASTEXITCODE" }

Write-Host '=== 2. Linking s23mbb.sys ==='
$sysOut = Join-Path $outDir 's23mbb.sys'
& $link /NOLOGO /NODEFAULTLIB /DRIVER /SUBSYSTEM:NATIVE,10.00 /MACHINE:ARM64 `
    /ENTRY:FxDriverEntry /FIXED:NO /DYNAMICBASE /TSAWARE:NO /RELEASE /OPT:REF /OPT:ICF `
    /MERGE:_TEXT=.text `
    /INCLUDE:_NetAdapterCx_BIND_INFO /INCLUDE:_MbbCx_BIND_INFO `
    "/OUT:$sysOut" `
    (Join-Path $outDir 's23mbb.obj') `
    (Join-Path $wdfLib 'wdfdriverentry.lib') `
    (Join-Path $wdfLib 'wdfldr.lib') `
    (Join-Path $wdkLib 'km\arm64\netcx\kmdf\adapter\2.0\netadaptercxstub.lib') `
    (Join-Path $wdkLib 'km\arm64\mbbcx\1.0\mbbcxstub.lib') `
    (Join-Path $wdkLib 'km\arm64\BufferOverflowFastFailK.lib') `
    (Join-Path $wdkLib 'km\arm64\ntoskrnl.lib') `
    (Join-Path $wdkLib 'km\arm64\hal.lib') `
    (Join-Path $wdkLib 'km\arm64\libcntpr.lib')
if ($LASTEXITCODE) { throw "link.exe s23mbb.sys failed: $LASTEXITCODE" }

Write-Host '=== 3. Staging & Signing pkg\s23mbb.sys + s23mbb.cat ==='
Copy-Item $sysOut (Join-Path $pkgDir 's23mbb.sys') -Force
Copy-Item (Join-Path $dir 's23mbb.inf') (Join-Path $pkgDir 's23mbb.inf') -Force

& $signtool sign /fd SHA256 /sha1 $thumb (Join-Path $pkgDir 's23mbb.sys')
if ($LASTEXITCODE) { throw "signtool s23mbb.sys failed: $LASTEXITCODE" }

& $inf2cat "/driver:$pkgDir" /os:10_NI_ARM64,10_GE_ARM64 /uselocaltime
if ($LASTEXITCODE) { throw "inf2cat failed: $LASTEXITCODE" }

& $signtool sign /fd SHA256 /sha1 $thumb (Join-Path $pkgDir 's23mbb.cat')
if ($LASTEXITCODE) { throw "signtool s23mbb.cat failed: $LASTEXITCODE" }

Write-Host '=== 4. Verifying PE, Zero __chkstk, & Phone Imports ==='
& python $verifyPy (Join-Path $pkgDir 's23mbb.sys') $ntPhone $halPhone $wdfldrRef
if ($LASTEXITCODE) { throw "verify_pe.py failed: $LASTEXITCODE" }

Write-Host '=== 5. Building install_root_mbb.exe (ARM64 User-Mode Static CRT) ==='
$umInc      = @("/I$msvcInc", "/I$wdkInc\um", "/I$wdkInc\shared", "/I$wdkInc\ucrt", "/I$dir")
$umLibPaths = @("/LIBPATH:$msvcLib", "/LIBPATH:$wdkLib\um\arm64", "/LIBPATH:$wdkLib\ucrt\arm64")
& $cl /nologo /MT /O2 /W3 /WX /DUNICODE /D_UNICODE @umInc `
    (Join-Path $dir 'install_root_mbb.c') "/Fo$outDir\install_root_mbb.obj" `
    /link /NOLOGO /MACHINE:ARM64 /SUBSYSTEM:CONSOLE @umLibPaths `
    setupapi.lib newdev.lib cfgmgr32.lib uuid.lib kernel32.lib `
    "/OUT:$outDir\install_root_mbb.exe"
if ($LASTEXITCODE) { throw "cl.exe install_root_mbb failed: $LASTEXITCODE" }

Write-Host "BUILT & VERIFIED: $pkgDir\s23mbb.sys, $pkgDir\s23mbb.cat, and $outDir\install_root_mbb.exe"
