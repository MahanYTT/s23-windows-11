# Builds s23gpu.sys (ARM64 WDDM KMD), the user-mode test tools and the signed driver package.
#
# Every tool path is auto-detected; each can be overridden by a parameter or an environment variable:
#   -MsvcToolsDir  / S23_MSVC_TOOLS_DIR   ...\VC\Tools\MSVC\<ver>        (default: vswhere, newest VS with the ARM64 toolset)
#   -KitsRoot      / S23_KITS_ROOT        ...\Windows Kits\10\           (default: registry KitsRoot10)
#   -WdkVersion    / S23_WDK_VERSION      SDK/WDK version folder          (default: 10.0.26100.0)
#   -CertThumbprint/ S23_CERT_THUMBPRINT  test-signing certificate        (default: the CN=S23TouchTest2020 cert below)
#   -MesaWslDir    / S23_MESA_WSL_DIR     Mesa checkout inside WSL        (default: $HOME/mesa_s23 of the WSL user)
#   -NtoskrnlRef   / S23_NTOSKRNL_REF     the phone's ntoskrnl.exe for verify_pe.py import checks
#   -HalRef        / S23_HAL_REF          the phone's hal.dll for verify_pe.py import checks
# If detection fails, the original development-machine paths are used as a fallback.
param(
    [string]$MsvcToolsDir   = $env:S23_MSVC_TOOLS_DIR,
    [string]$KitsRoot       = $env:S23_KITS_ROOT,
    [string]$WdkVersion     = $(if ($env:S23_WDK_VERSION) { $env:S23_WDK_VERSION } else { '10.0.26100.0' }),
    [string]$CertThumbprint = $(if ($env:S23_CERT_THUMBPRINT) { $env:S23_CERT_THUMBPRINT } else { '05DBD92557F4FC52EF2A5E5955789D8EED637C68' }),
    [string]$MesaWslDir     = $env:S23_MESA_WSL_DIR,
    [string]$NtoskrnlRef    = $(if ($env:S23_NTOSKRNL_REF) { $env:S23_NTOSKRNL_REF } else { 'C:\Users\Mahan\.gemini\antigravity\brain\69c04d84-af7a-419e-9781-d650ca2201d9\scratch\ntoskrnl_phone.exe' }),
    [string]$HalRef         = $(if ($env:S23_HAL_REF) { $env:S23_HAL_REF } else { 'C:\Users\Mahan\.gemini\antigravity\brain\69c04d84-af7a-419e-9781-d650ca2201d9\scratch\hal.dll' })
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

# Windows path -> WSL path (wslpath when available, else the default /mnt/<drive> automount)
function ConvertTo-WslPath([string]$Path) {
    $full = [System.IO.Path]::GetFullPath($Path)
    try {
        $p = & wsl -e wslpath -a -u $full 2>$null
        if (-not $LASTEXITCODE -and $p) { return ($p | Select-Object -First 1).Trim() }
    } catch {}
    return '/mnt/' + $full.Substring(0, 1).ToLowerInvariant() + $full.Substring(2).Replace('\', '/')
}

if (-not $MsvcToolsDir) { $MsvcToolsDir = Find-MsvcToolsDir }
if (-not $KitsRoot)     { $KitsRoot     = Find-KitsRoot }
if (-not $MesaWslDir) {
    $wslHome = $null
    try { $wslHome = (& wsl -e sh -c 'echo $HOME' 2>$null | Select-Object -First 1) } catch {}
    $MesaWslDir = if ($wslHome) { $wslHome.Trim() + '/mesa_s23' } else { '/home/mahan/mesa_s23' }
}

$dir      = $PSScriptRoot
$outDir   = Join-Path $dir 'out'
$pkgDir   = Join-Path $dir 'pkg'
$msvcBin  = Join-Path $MsvcToolsDir 'bin\Hostx64\arm64'
$msvcInc  = Join-Path $MsvcToolsDir 'include'
$msvcLib  = Join-Path $MsvcToolsDir 'lib\arm64'
$wdkInc   = Join-Path $KitsRoot "Include\$WdkVersion"
$wdkLib   = Join-Path $KitsRoot "Lib\$WdkVersion"
$wdkBinX86 = Join-Path $KitsRoot "bin\$WdkVersion\x86"
$wdkBinX64 = Join-Path $KitsRoot "bin\$WdkVersion\x64"

$cl       = Join-Path $msvcBin 'cl.exe'
$link     = Join-Path $msvcBin 'link.exe'
$armasm   = Join-Path $msvcBin 'armasm64.exe'
$dumpbin  = Join-Path $msvcBin 'dumpbin.exe'
$inf2cat  = Join-Path $wdkBinX86 'inf2cat.exe'
$signtool = Join-Path $wdkBinX64 'signtool.exe'
$thumb    = $CertThumbprint

foreach ($tool in $cl, $link, $armasm, $dumpbin, $inf2cat, $signtool) {
    if (-not (Test-Path $tool)) { throw "Build tool not found: $tool (set -MsvcToolsDir / -KitsRoot / -WdkVersion)" }
}
Write-Host "MSVC: $MsvcToolsDir"
Write-Host "Kits: $KitsRoot ($WdkVersion)"
Write-Host "Mesa: $MesaWslDir (WSL)"

New-Item -ItemType Directory -Force -Path $outDir, $pkgDir | Out-Null
Remove-Item -Recurse -Force (Join-Path $outDir '*'), (Join-Path $pkgDir '*') -ErrorAction SilentlyContinue
$outWsl = ConvertTo-WslPath $outDir

$wslMesaDll = "$MesaWslDir/build_d3d10/src/gallium/targets/d3d10umd/libgallium_d3d10.dll"
$mesaUmd = Join-Path $outDir 'mesa_d3d10_arm64.dll'
wsl bash -lc "test -f '$wslMesaDll' && cp '$wslMesaDll' '$outWsl/mesa_d3d10_arm64.dll'" 2>$null
if ($LASTEXITCODE) { throw "Failed to copy Mesa UMD from WSL ($wslMesaDll)" }

Write-Host '=== Assembling scm_arm64.asm ==='
& $armasm -o (Join-Path $outDir 'scm_arm64.obj') (Join-Path $dir 'scm_arm64.asm')
if ($LASTEXITCODE) { throw "armasm64 failed: $LASTEXITCODE" }

Write-Host '=== Compiling WDDM KMD (ARM64) ==='
$kmDefs = @('/D_ARM64_=1', '/DARM64=1', '/D_WIN64=1', '/DWINNT=1', '/D_WIN32_WINNT=0x0A00', '/DNTDDI_VERSION=0x0A000010', '/DPOOL_ZERO_DOWN_LEVEL_SUPPORT=1')
$kmInc  = @("/I$wdkInc\km", "/I$wdkInc\km\crt", "/I$wdkInc\shared")
$kmSrc  = @('log.c', 'eng.c', 'disp.c', 'msm.c', 'hw.c', 'ddi.c') | ForEach-Object { Join-Path $dir $_ }

& $cl /nologo /c /kernel /Zl /GS /W3 /WX /Oi /O2 /Zi @kmDefs @kmInc "/Fo$outDir\" "/Fd$outDir\s23gpu.pdb" @kmSrc
if ($LASTEXITCODE) { throw "cl.exe KMD failed: $LASTEXITCODE" }

Write-Host '=== Linking s23gpu.sys ==='
$objs = Get-ChildItem $outDir -Filter '*.obj' | ForEach-Object { $_.FullName }
$kmLibs = @(
    (Join-Path $wdkLib 'km\arm64\displib.lib'),
    (Join-Path $wdkLib 'km\arm64\ntoskrnl.lib'),
    (Join-Path $wdkLib 'km\arm64\hal.lib'),
    (Join-Path $wdkLib 'km\arm64\libcntpr.lib'),
    (Join-Path $wdkLib 'km\arm64\bufferoverflowfastfailk.lib'),
    (Join-Path $wdkLib 'km\arm64\ntstrsafe.lib')
)
$sysOut = Join-Path $outDir 's23gpu.sys'
& $link /NOLOGO /NODEFAULTLIB /DRIVER /SUBSYSTEM:NATIVE,10.00 /MACHINE:ARM64 /ENTRY:GsDriverEntry /TSAWARE:NO /RELEASE /OPT:REF /OPT:ICF /MERGE:_TEXT=.text "/OUT:$sysOut" @objs @kmLibs
if ($LASTEXITCODE) { throw "link.exe KMD failed: $LASTEXITCODE" }

Write-Host '=== Building User-Mode Tools (ARM64) ==='
$umInc = @("/I$msvcInc", "/I$wdkInc\um", "/I$wdkInc\shared", "/I$wdkInc\ucrt", "/I$dir")
$umLibPaths = @("/LIBPATH:$msvcLib", "/LIBPATH:$wdkLib\um\arm64", "/LIBPATH:$wdkLib\ucrt\arm64")

& $cl /nologo /MT /O2 /W3 @umInc (Join-Path $dir 's23gpu_test.c') "/Fo$outDir\s23gpu_test.obj" /link /MACHINE:ARM64 @umLibPaths gdi32.lib kernel32.lib "/OUT:$outDir\s23gpu_test.exe"
if ($LASTEXITCODE) { throw "cl.exe test failed: $LASTEXITCODE" }

& $cl /nologo /MT /O2 /W3 @umInc (Join-Path $dir 's23gpu_install.c') "/Fo$outDir\s23gpu_install.obj" /link /MACHINE:ARM64 @umLibPaths setupapi.lib newdev.lib kernel32.lib "/OUT:$outDir\s23gpu_install.exe"
if ($LASTEXITCODE) { throw "cl.exe install failed: $LASTEXITCODE" }

& $cl /nologo /MT /O2 /W3 @umInc (Join-Path $dir 's23gpu_d3d11_test.c') "/Fo$outDir\s23gpu_d3d11_test.obj" /link /MACHINE:ARM64 @umLibPaths d3d11.lib dxgi.lib dxguid.lib d3dcompiler.lib user32.lib gdi32.lib kernel32.lib "/OUT:$outDir\s23gpu_d3d11_test.exe"
if ($LASTEXITCODE) { throw "cl.exe d3d11_test failed: $LASTEXITCODE" }

& $cl /nologo /MT /O2 /W3 @umInc (Join-Path $dir 's23gpu_opengl_test.c') "/Fo$outDir\s23gpu_opengl_test.obj" /link /MACHINE:ARM64 @umLibPaths opengl32.lib user32.lib gdi32.lib kernel32.lib "/OUT:$outDir\s23gpu_opengl_test.exe"
if ($LASTEXITCODE) { throw "cl.exe opengl_test failed: $LASTEXITCODE" }

& $cl /nologo /MT /O2 /W3 @umInc (Join-Path $dir 's23disp_only.c') "/Fo$outDir\s23disp_only.obj" /link /MACHINE:ARM64 @umLibPaths user32.lib wtsapi32.lib advapi32.lib kernel32.lib "/OUT:$outDir\s23disp_only.exe"
if ($LASTEXITCODE) { throw "cl.exe s23disp_only failed: $LASTEXITCODE" }

Write-Host "Using Mesa Freedreno UMD: $mesaUmd"
Copy-Item $mesaUmd (Join-Path $outDir 's23gpu_umd.dll') -Force

$wslWglDll = "$MesaWslDir/build_d3d10/src/gallium/targets/wgl/libgallium_wgl.dll"
$wslGlDll  = "$MesaWslDir/build_d3d10/src/gallium/targets/libgl-gdi/opengl32.dll"
wsl bash -lc "if test -f '$wslWglDll' && test -f '$wslGlDll'; then cp '$wslWglDll' '$outWsl/libgallium_wgl.dll' && cp '$wslGlDll' '$outWsl/opengl32.dll'; fi" 2>$null
if (Test-Path (Join-Path $outDir 'libgallium_wgl.dll')) {
    & $signtool sign /fd SHA256 /sha1 $thumb (Join-Path $outDir 'libgallium_wgl.dll') (Join-Path $outDir 'opengl32.dll')
    if ($LASTEXITCODE) { throw "signtool OpenGL DLLs failed: $LASTEXITCODE" }
}

$outX64Dir = Join-Path $outDir 'x64'
New-Item -ItemType Directory -Force -Path $outX64Dir | Out-Null
$wslWglX64 = "$MesaWslDir/build_x64/src/gallium/targets/wgl/libgallium_wgl.dll"
$wslGlX64  = "$MesaWslDir/build_x64/src/gallium/targets/libgl-gdi/opengl32.dll"
wsl bash -lc "if test -f '$wslWglX64' && test -f '$wslGlX64'; then cp '$wslWglX64' '$outWsl/x64/libgallium_wgl.dll' && cp '$wslGlX64' '$outWsl/x64/opengl32.dll'; fi" 2>$null
if (Test-Path (Join-Path $outX64Dir 'libgallium_wgl.dll')) {
    & $signtool sign /fd SHA256 /sha1 $thumb (Join-Path $outX64Dir 'libgallium_wgl.dll') (Join-Path $outX64Dir 'opengl32.dll')
    if ($LASTEXITCODE) { throw "signtool x64 OpenGL DLLs failed: $LASTEXITCODE" }
}

Write-Host '=== Packaging & Signing ==='
Copy-Item $sysOut (Join-Path $pkgDir 's23gpu.sys') -Force
Copy-Item (Join-Path $outDir 's23gpu_umd.dll') (Join-Path $pkgDir 's23gpu_umd.dll') -Force
Copy-Item (Join-Path $outDir 'libgallium_wgl.dll') (Join-Path $pkgDir 'libgallium_wgl.dll') -Force
Copy-Item (Join-Path $dir 's23gpu.inf') (Join-Path $pkgDir 's23gpu.inf') -Force

& $signtool sign /fd SHA256 /sha1 $thumb (Join-Path $pkgDir 's23gpu.sys') (Join-Path $pkgDir 's23gpu_umd.dll') (Join-Path $pkgDir 'libgallium_wgl.dll')
if ($LASTEXITCODE) { throw "signtool .sys/.dll failed: $LASTEXITCODE" }

& $inf2cat "/driver:$pkgDir" /os:10_NI_ARM64,10_GE_ARM64
if ($LASTEXITCODE) { throw "inf2cat failed: $LASTEXITCODE" }

& $signtool sign /fd SHA256 /sha1 $thumb (Join-Path $pkgDir 's23gpu.cat')
if ($LASTEXITCODE) { throw "signtool .cat failed: $LASTEXITCODE" }

Write-Host '=== Verifying PE Headers, Imports & Catalog ==='
& $dumpbin /headers (Join-Path $pkgDir 's23gpu.sys') | Select-String -Pattern 'machine|checksum|subsystem|DLL characteristics'
& $dumpbin /imports (Join-Path $pkgDir 's23gpu.sys') | Select-String -Pattern '\.sys|\.exe|\.dll'

& $dumpbin /headers (Join-Path $outDir 's23gpu_test.exe') (Join-Path $outDir 's23gpu_install.exe') (Join-Path $outDir 's23gpu_d3d11_test.exe') (Join-Path $pkgDir 's23gpu_umd.dll') | Select-String -Pattern 'machine|Dump of file'
python (Join-Path $dir '..\s23gpuprobe\verify_pe.py') (Join-Path $pkgDir 's23gpu.sys') $NtoskrnlRef $HalRef
if ($LASTEXITCODE) { throw "verify_pe.py failed: $LASTEXITCODE" }

$sigs = Get-AuthenticodeSignature (Join-Path $pkgDir 's23gpu.sys'), (Join-Path $pkgDir 's23gpu_umd.dll'), (Join-Path $pkgDir 's23gpu.cat')
$sigs | Format-Table Path, Status, @{N='Thumbprint';E={$_.SignerCertificate.Thumbprint}}, @{N='Signer';E={$_.SignerCertificate.Subject}}
foreach ($s in $sigs) {
    if (-not $s.SignerCertificate -or $s.SignerCertificate.Thumbprint -ne $thumb) {
        throw "Unexpected signer on $($s.Path)"
    }
}
