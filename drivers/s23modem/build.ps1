param(
    [string]$MsvcToolsDir   = $env:S23_MSVC_TOOLS_DIR,
    [string]$KitsRoot       = $env:S23_KITS_ROOT,
    [string]$WdkVersion     = $(if ($env:S23_WDK_VERSION) { $env:S23_WDK_VERSION } else { '10.0.26100.0' }),
    [string]$WdkBinX64      = $env:S23_WDK_BIN_X64,
    [string]$CertThumbprint = $(if ($env:S23_CERT_THUMBPRINT) { $env:S23_CERT_THUMBPRINT } else { '05DBD92557F4FC52EF2A5E5955789D8EED637C68' }),
    [string]$NtoskrnlRef    = $(if ($env:S23_NTOSKRNL_REF) { $env:S23_NTOSKRNL_REF } else { 'C:\Users\Mahan\.gemini\antigravity\brain\69c04d84-af7a-419e-9781-d650ca2201d9\scratch\ntoskrnl_phone.exe' }),
    [string]$HalRef         = $(if ($env:S23_HAL_REF) { $env:S23_HAL_REF } else { 'C:\Users\Mahan\.gemini\antigravity\brain\69c04d84-af7a-419e-9781-d650ca2201d9\scratch\phone_dlls\hal.dll' })
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
$msvcDir   = $MsvcToolsDir
$msvcBin   = Join-Path $msvcDir 'bin\Hostx64\arm64'
$msvcInc   = Join-Path $msvcDir 'include'
$msvcLib   = Join-Path $msvcDir 'lib\arm64'
$wdkInc    = Join-Path $KitsRoot "Include\$WdkVersion"
$wdkLib    = Join-Path $KitsRoot "Lib\$WdkVersion"
$wdkBinX64 = if ($WdkBinX64) { $WdkBinX64 } else { Join-Path $KitsRoot "bin\$WdkVersion\x64" }

$cl        = Join-Path $msvcBin 'cl.exe'
$link      = Join-Path $msvcBin 'link.exe'
$armasm    = Join-Path $msvcBin 'armasm64.exe'
$signtool  = Join-Path $wdkBinX64 'signtool.exe'
$thumb     = $CertThumbprint
$ntPhone   = $NtoskrnlRef
$halPhone  = $HalRef
$verifyPy  = Join-Path $dir '..\s23gpuprobe\verify_pe.py'

New-Item -ItemType Directory -Force -Path $outDir | Out-Null
Remove-Item -Recurse -Force (Join-Path $outDir '*') -ErrorAction SilentlyContinue

Write-Host '=== Assembling scm_arm64.asm ==='
& $armasm -o (Join-Path $outDir 'scm_arm64.obj') (Join-Path $dir 'scm_arm64.asm')
if ($LASTEXITCODE) { throw "armasm64 failed: $LASTEXITCODE" }

Write-Host '=== Compiling s23modem_xprt.c (ARM64 Kernel) ==='
$kmDefs = @('/D_ARM64_=1', '/DARM64=1', '/D_WIN64=1', '/DWINNT=1', '/D_WIN32_WINNT=0x0A00', '/DNTDDI_VERSION=0x0A000010', '/DPOOL_ZERO_DOWN_LEVEL_SUPPORT=1')
$kmInc  = @("/I$wdkInc\km", "/I$wdkInc\km\crt", "/I$wdkInc\shared", "/I$dir")
& $cl /nologo /c /kernel /Zl /GS- /W3 /WX /Oi /O2 @kmDefs @kmInc "/Fo$outDir\" (Join-Path $dir 's23modem_xprt.c')
if ($LASTEXITCODE) { throw "cl.exe s23modem_xprt.c failed: $LASTEXITCODE" }

Write-Host '=== Linking s23modem_xprt.sys ==='
$sysOut = Join-Path $outDir 's23modem_xprt.sys'
& $link /NOLOGO /NODEFAULTLIB /DRIVER /SUBSYSTEM:NATIVE,10.00 /MACHINE:ARM64 /ENTRY:DriverEntry /FIXED:NO /DYNAMICBASE /TSAWARE:NO /RELEASE /OPT:REF /OPT:ICF /MERGE:_TEXT=.text "/OUT:$sysOut" (Join-Path $outDir 's23modem_xprt.obj') (Join-Path $outDir 'scm_arm64.obj') (Join-Path $wdkLib 'km\arm64\ntoskrnl.lib') (Join-Path $wdkLib 'km\arm64\hal.lib') (Join-Path $wdkLib 'km\arm64\libcntpr.lib')
if ($LASTEXITCODE) { throw "link.exe s23modem_xprt.sys failed: $LASTEXITCODE" }

Write-Host '=== Signing s23modem_xprt.sys ==='
& $signtool sign /fd SHA256 /sha1 $thumb $sysOut
if ($LASTEXITCODE) { throw "signtool failed: $LASTEXITCODE" }

Write-Host '=== Verifying PE & S23 ntoskrnl.exe imports ==='
& python $verifyPy $sysOut $ntPhone $halPhone
if ($LASTEXITCODE) { throw "verify_pe.py failed: $LASTEXITCODE" }

Write-Host '=== Building s23modemd.exe (ARM64 User-Mode Static CRT) ==='
$umInc = @("/I$msvcInc", "/I$wdkInc\um", "/I$wdkInc\shared", "/I$wdkInc\ucrt", "/I$dir")
$umLibPaths = @("/LIBPATH:$msvcLib", "/LIBPATH:$wdkLib\um\arm64", "/LIBPATH:$wdkLib\ucrt\arm64")
$exeOut = Join-Path $outDir 's23modemd.exe'
& $cl /nologo /MT /O2 /W3 /WX @umInc (Join-Path $dir 's23modemd.c') "/Fo$outDir\s23modemd.obj" /link /MACHINE:ARM64 @umLibPaths kernel32.lib "/OUT:$exeOut"
if ($LASTEXITCODE) { throw "cl.exe s23modemd.exe failed: $LASTEXITCODE" }

Write-Host "BUILT & VERIFIED: $sysOut and $exeOut"
