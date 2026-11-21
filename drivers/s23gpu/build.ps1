$ErrorActionPreference = 'Stop'

$dir      = 'C:\Users\Mahan\Documents\windows-on-android\s23_dm1q\drivers\s23gpu'
$outDir   = Join-Path $dir 'out'
$pkgDir   = Join-Path $dir 'pkg'
$msvcBin  = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207\bin\Hostx64\arm64'
$msvcInc  = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207\include'
$msvcLib  = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207\lib\arm64'
$wdkInc   = 'C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0'
$wdkLib   = 'C:\Program Files (x86)\Windows Kits\10\Lib\10.0.26100.0'
$wdkBinX86 = 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x86'
$wdkBinX64 = 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64'

$cl       = Join-Path $msvcBin 'cl.exe'
$link     = Join-Path $msvcBin 'link.exe'
$armasm   = Join-Path $msvcBin 'armasm64.exe'
$dumpbin  = Join-Path $msvcBin 'dumpbin.exe'
$inf2cat  = Join-Path $wdkBinX86 'inf2cat.exe'
$signtool = Join-Path $wdkBinX64 'signtool.exe'
$thumb    = '05DBD92557F4FC52EF2A5E5955789D8EED637C68'

New-Item -ItemType Directory -Force -Path $outDir, $pkgDir | Out-Null
Remove-Item -Force (Join-Path $outDir '*'), (Join-Path $pkgDir '*') -ErrorAction SilentlyContinue

Write-Host '=== Assembling scm_arm64.asm ==='
& $armasm -o (Join-Path $outDir 'scm_arm64.obj') (Join-Path $dir 'scm_arm64.asm')
if ($LASTEXITCODE) { throw "armasm64 failed: $LASTEXITCODE" }

Write-Host '=== Compiling WDDM KMD (ARM64) ==='
$kmDefs = @('/D_ARM64_=1', '/DARM64=1', '/D_WIN64=1', '/DWINNT=1', '/D_WIN32_WINNT=0x0A00', '/DNTDDI_VERSION=0x0A000010', '/DPOOL_ZERO_DOWN_LEVEL_SUPPORT=1')
$kmInc  = @("/I$wdkInc\km", "/I$wdkInc\km\crt", "/I$wdkInc\shared")
$kmSrc  = @('log.c', 'eng.c', 'msm.c', 'hw.c', 'ddi.c') | ForEach-Object { Join-Path $dir $_ }

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

& $cl /nologo /LD /MT /O2 /W3 /WX @umInc (Join-Path $dir 's23gpu_umd.c') "/Fo$outDir\s23gpu_umd.obj" /link /MACHINE:ARM64 @umLibPaths "/DEF:$(Join-Path $dir 's23gpu_umd.def')" kernel32.lib "/OUT:$outDir\s23gpu_umd.dll"
if ($LASTEXITCODE) { throw "cl.exe umd failed: $LASTEXITCODE" }

Write-Host '=== Packaging & Signing ==='
Copy-Item $sysOut (Join-Path $pkgDir 's23gpu.sys') -Force
Copy-Item (Join-Path $outDir 's23gpu_umd.dll') (Join-Path $pkgDir 's23gpu_umd.dll') -Force
Copy-Item (Join-Path $dir 's23gpu.inf') (Join-Path $pkgDir 's23gpu.inf') -Force

& $signtool sign /fd SHA256 /sha1 $thumb (Join-Path $pkgDir 's23gpu.sys') (Join-Path $pkgDir 's23gpu_umd.dll')
if ($LASTEXITCODE) { throw "signtool .sys/.dll failed: $LASTEXITCODE" }

& $inf2cat "/driver:$pkgDir" /os:10_NI_ARM64,10_GE_ARM64
if ($LASTEXITCODE) { throw "inf2cat failed: $LASTEXITCODE" }

& $signtool sign /fd SHA256 /sha1 $thumb (Join-Path $pkgDir 's23gpu.cat')
if ($LASTEXITCODE) { throw "signtool .cat failed: $LASTEXITCODE" }

Write-Host '=== Verifying PE Headers, Imports & Catalog ==='
& $dumpbin /headers (Join-Path $pkgDir 's23gpu.sys') | Select-String -Pattern 'machine|checksum|subsystem|DLL characteristics'
& $dumpbin /imports (Join-Path $pkgDir 's23gpu.sys') | Select-String -Pattern '\.sys|\.exe|\.dll'

& $dumpbin /headers (Join-Path $outDir 's23gpu_test.exe') (Join-Path $outDir 's23gpu_install.exe') (Join-Path $pkgDir 's23gpu_umd.dll') | Select-String -Pattern 'machine|Dump of file'
python (Join-Path $dir '..\s23gpuprobe\verify_pe.py') (Join-Path $pkgDir 's23gpu.sys') 'C:\Users\Mahan\.gemini\antigravity\brain\69c04d84-af7a-419e-9781-d650ca2201d9\scratch\ntoskrnl_phone.exe' 'C:\Users\Mahan\.gemini\antigravity\brain\69c04d84-af7a-419e-9781-d650ca2201d9\scratch\hal.dll'
if ($LASTEXITCODE) { throw "verify_pe.py failed: $LASTEXITCODE" }

$sigs = Get-AuthenticodeSignature (Join-Path $pkgDir 's23gpu.sys'), (Join-Path $pkgDir 's23gpu_umd.dll'), (Join-Path $pkgDir 's23gpu.cat')
$sigs | Format-Table Path, Status, @{N='Thumbprint';E={$_.SignerCertificate.Thumbprint}}, @{N='Signer';E={$_.SignerCertificate.Subject}}
foreach ($s in $sigs) {
    if (-not $s.SignerCertificate -or $s.SignerCertificate.Thumbprint -ne $thumb) {
        throw "Unexpected signer on $($s.Path)"
    }
}
