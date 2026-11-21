# build_s23_qcwlan_pkgs.ps1
# Repeatable offline builder for Samsung Galaxy S23 (dm1q / SM8550-AC) QCA6490 Wi-Fi 6E packages:
#   1. wifi_research\qca6490_bringup\pkg_s23_qcwlan_intx\  (Awake-pinned + Legacy INTx, MSISupported=0)
#   2. wifi_research\qca6490_bringup\pkg_s23_qcwlan_msi\   (Awake-pinned + MSI test,    MSISupported=1)
#
# Uses downloaded_qcwlan8280\extracted\* (preserving Qualcomm's Windows-ABI wlanfw20.mbn, m320.bin,
# Data20.msc, and signed qcwlan8280.sys), injects S23's stock bdwlan.elf + bdwlang.elf (and bdwlan.elf.g),
# patches qcwlan8280.inf (UTF-16LE BOM preserved), rebuilds & augments qcwlan8280.cat (WinTrust v2 SHA-256),
# signs with CN=S23TouchTest2020, and validates with Test-FileCatalog.

$ErrorActionPreference = 'Stop'

$root       = 'C:\Users\Mahan\Documents\windows-on-android\s23_dm1q'
$srcPkg     = Join-Path $root 'downloaded_qcwlan8280\extracted'
$s23FwDir   = Join-Path $root 'wifi_research\s23_vendor_wifi_fw\qca6490'
$outBase    = Join-Path $root 'wifi_research\qca6490_bringup'
$s23BdElf   = Join-Path $s23FwDir 'bdwlan.elf'
$s23BdGElf  = Join-Path $s23FwDir 'bdwlang.elf'

foreach ($p in @($srcPkg, $s23BdElf, $s23BdGElf)) {
    if (-not (Test-Path $p)) {
        throw "Required source missing: $p"
    }
}

$cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object {
    $_.Subject -eq 'CN=S23TouchTest2020' -and $_.HasPrivateKey
} | Select-Object -First 1

if (-not $cert) {
    throw "Signing certificate CN=S23TouchTest2020 not found in Cert:\CurrentUser\My!"
}
Write-Output "Using certificate: $($cert.Subject) Thumbprint=$($cert.Thumbprint)"

# Verify S23 board data hashes before packaging
$bdHash  = (Get-FileHash -Algorithm MD5 $s23BdElf).Hash.ToLowerInvariant()
$bdgHash = (Get-FileHash -Algorithm MD5 $s23BdGElf).Hash.ToLowerInvariant()
Write-Output "S23 bdwlan.elf  : $((Get-Item $s23BdElf).Length) B, MD5=$bdHash"
Write-Output "S23 bdwlang.elf : $((Get-Item $s23BdGElf).Length) B, MD5=$bdgHash"
if ($bdHash -ne '916a72b57f9fc2c6140ea982fb60d9a2') {
    throw "Unexpected S23 bdwlan.elf MD5: $bdHash"
}

if (-not ('QcwlanCatAugment' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class QcwlanCatAugment {
    public const uint CRYPTCAT_OPEN_EXISTING = 0x00000002;
    public const uint CRYPTCAT_ATTR_FLAGS    = 0x10010001;

    [DllImport("wintrust.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr CryptCATOpen(string pwszFileName, uint fdwOpenFlags, IntPtr hProv, uint dwPublicVersion, uint dwEncodingType);
    [DllImport("wintrust.dll", SetLastError = true)]
    static extern bool CryptCATClose(IntPtr hCatalog);
    [DllImport("wintrust.dll", SetLastError = true)]
    static extern bool CryptCATPersistStore(IntPtr hCatalog);
    [DllImport("wintrust.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr CryptCATPutCatAttrInfo(IntPtr hCatalog, string pwszReferenceTag, uint dwAttrTypeAndAction, uint cbData, byte[] pbData);
    [DllImport("wintrust.dll", SetLastError = true)]
    static extern IntPtr CryptCATEnumerateMember(IntPtr hCatalog, IntPtr pPrevMember);
    [DllImport("wintrust.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr CryptCATPutAttrInfo(IntPtr hCatalog, IntPtr pCatMember, string pwszReferenceTag, uint dwAttrTypeAndAction, uint cbData, byte[] pbData);

    static byte[] Utf16Z(string s) { return Encoding.Unicode.GetBytes(s + "\0"); }

    public static int Augment(string catPath) {
        IntPtr hCat = CryptCATOpen(catPath, CRYPTCAT_OPEN_EXISTING, IntPtr.Zero, 0, 0);
        if (hCat == IntPtr.Zero || hCat == new IntPtr(-1))
            throw new Exception("CryptCATOpen failed: " + Marshal.GetLastWin32Error());
        int count = 0;
        try {
            byte[] osVal = Utf16Z("10_ARM64,10_X64");
            if (CryptCATPutCatAttrInfo(hCat, "OS", CRYPTCAT_ATTR_FLAGS, (uint)osVal.Length, osVal) == IntPtr.Zero)
                throw new Exception("PutCatAttr OS failed: " + Marshal.GetLastWin32Error());

            byte[] hw1 = Utf16Z("pci\\ven_17cb&dev_1103");
            if (CryptCATPutCatAttrInfo(hCat, "HWID1", CRYPTCAT_ATTR_FLAGS, (uint)hw1.Length, hw1) == IntPtr.Zero)
                throw new Exception("PutCatAttr HWID1 failed: " + Marshal.GetLastWin32Error());

            byte[] hw2 = Utf16Z("pci\\ven_17cb&dev_1103&subsys_010817cb");
            if (CryptCATPutCatAttrInfo(hCat, "HWID2", CRYPTCAT_ATTR_FLAGS, (uint)hw2.Length, hw2) == IntPtr.Zero)
                throw new Exception("PutCatAttr HWID2 failed: " + Marshal.GetLastWin32Error());

            byte[] osAttr = Utf16Z("2:10.0");
            IntPtr member = IntPtr.Zero;
            while ((member = CryptCATEnumerateMember(hCat, member)) != IntPtr.Zero) {
                if (CryptCATPutAttrInfo(hCat, member, "OSAttr", CRYPTCAT_ATTR_FLAGS, (uint)osAttr.Length, osAttr) == IntPtr.Zero)
                    throw new Exception("PutAttr OSAttr failed: " + Marshal.GetLastWin32Error());
                count++;
            }
            if (!CryptCATPersistStore(hCat))
                throw new Exception("CryptCATPersistStore failed: " + Marshal.GetLastWin32Error());
        } finally {
            CryptCATClose(hCat);
        }
        return count;
    }
}
'@
}

function Build-QcWlanPackage {
    param(
        [Parameter(Mandatory = $true)][string]$PkgName,
        [Parameter(Mandatory = $true)][int]$MsiSupported
    )

    $dstDir   = Join-Path $outBase $PkgName
    $stageDir = Join-Path $outBase ("_stage_" + $PkgName)
    if (Test-Path $dstDir)   { Remove-Item -Recurse -Force $dstDir }
    if (Test-Path $stageDir) { Remove-Item -Recurse -Force $stageDir }
    New-Item -ItemType Directory -Force -Path $dstDir   | Out-Null
    New-Item -ItemType Directory -Force -Path $stageDir | Out-Null

    # 1. Copy all Qualcomm extracted package files EXCEPT original qcwlan8280.cat
    Get-ChildItem -File $srcPkg | Where-Object { $_.Name -ne 'qcwlan8280.cat' } | ForEach-Object {
        Copy-Item $_.FullName (Join-Path $stageDir $_.Name) -Force
    }

    # 2. Replace bdwlan.elf, bdwlang.elf, and bdwlan.elf.g with Samsung Galaxy S23 stock board data
    Copy-Item $s23BdElf  (Join-Path $stageDir 'bdwlan.elf')   -Force
    Copy-Item $s23BdGElf (Join-Path $stageDir 'bdwlang.elf')  -Force
    Copy-Item $s23BdGElf (Join-Path $stageDir 'bdwlan.elf.g') -Force

    # 3. Patch qcwlan8280.inf (preserve UTF-16LE with BOM)
    $infPath = Join-Path $stageDir 'qcwlan8280.inf'
    $infText = [System.IO.File]::ReadAllText($infPath, [System.Text.Encoding]::Unicode)

    # 3a. Remove PciD3ColdSupported.HW from [QcWlan_LITHIUM.ndi.NTARM64.HW]
    $r1 = [regex]::Replace($infText, 'Needs\s*=\s*PciASPMOptIn\.HW\s*,\s*PciD3ColdSupported\.HW', 'Needs   = PciASPMOptIn.HW')
    if ($r1 -eq $infText) { throw "Failed to patch PciD3ColdSupported.HW in $PkgName" }
    $infText = $r1

    # 3b. Force D0PCEnable = 0 (default already 0; also set explicit HKR value)
    $r2 = [regex]::Replace(
        $infText,
        '(\[D0PC\.reg\]\r?\n)',
        "`$1HKR, , D0PCEnable, 0, `"0`"`r`n"
    )
    if ($r2 -eq $infText) { throw "Failed to patch [D0PC.reg] in $PkgName" }
    $infText = $r2

    # 3c. Force wowEnable = 0
    $r3 = [regex]::Replace($infText, '(HKR,\s*,\s*wowEnable,\s*0x00010001,\s*)1', '${1}0')
    if ($r3 -eq $infText) { throw "Failed to patch wowEnable in $PkgName" }
    $infText = $r3

    # 3d. Force S0IdleEnable = 0
    $r4 = [regex]::Replace($infText, '(HKR,\s*,\s*S0IdleEnable,\s*0x00010001,\s*)1', '${1}0')
    if ($r4 -eq $infText) { throw "Failed to patch S0IdleEnable in $PkgName" }
    $infText = $r4

    # 3e. Force DmaRemappingCompatible = 0 (SMMU-500 Stage-1 identity pass-through active for SID 0x1401)
    $r5 = [regex]::Replace($infText, '(HKR,Parameters,DmaRemappingCompatible,0x00010001,)1', '${1}0')
    if ($r5 -eq $infText) { throw "Failed to patch DmaRemappingCompatible in $PkgName" }
    $infText = $r5

    # 3f. Set MSISupported = 0 (INTx) or 1 (MSI)
    $r6 = [regex]::Replace(
        $infText,
        '(HKR,\s*"Interrupt Management\\MessageSignaledInterruptProperties",\s*MSISupported,\s*0x00010001,\s*)\d+',
        "`${1}$MsiSupported"
    )
    if ($MsiSupported -eq 0 -and $r6 -eq $infText) { throw "Failed to patch MSISupported in $PkgName" }
    $infText = $r6

    $utf16LeBom = New-Object System.Text.UnicodeEncoding($false, $true)
    [System.IO.File]::WriteAllText($infPath, $infText, $utf16LeBom)

    # 4. Generate WinTrust v2 SHA-256 catalog, augment with ARM64 OS/HWID attributes, and sign
    $catPath = Join-Path $dstDir 'qcwlan8280.cat'
    New-FileCatalog -Path $stageDir -CatalogFilePath $catPath -CatalogVersion 2 | Out-Null

    $nMembers = [QcwlanCatAugment]::Augment($catPath)
    $sigCat = Set-AuthenticodeSignature -FilePath $catPath -Certificate $cert -HashAlgorithm SHA256
    if ($sigCat.Status -ne 'Valid' -and $sigCat.Status -ne 'UnknownError') {
        throw "Unexpected Authenticode status for $PkgName catalog: $($sigCat.Status)"
    }

    # 5. Copy staged package files into final destination folder
    Get-ChildItem -File $stageDir | ForEach-Object {
        Copy-Item $_.FullName (Join-Path $dstDir $_.Name) -Force
    }

    # 6. Validate catalog integrity against final package folder
    $catTest = Test-FileCatalog -CatalogFilePath $catPath -Path $dstDir -FilesToSkip 'qcwlan8280.cat' -Detailed
    if ($catTest.Status -ne 'Valid') {
        throw "Test-FileCatalog FAILED for $PkgName : $($catTest.Status)"
    }

    Remove-Item -Recurse -Force $stageDir

    $catMd5 = (Get-FileHash -Algorithm MD5 $catPath).Hash.ToLowerInvariant()
    $infMd5 = (Get-FileHash -Algorithm MD5 (Join-Path $dstDir 'qcwlan8280.inf')).Hash.ToLowerInvariant()
    Write-Output "=== BUILT $PkgName (MSISupported=$MsiSupported) ==="
    Write-Output "  Members=$($nMembers) Test-FileCatalog=$($catTest.Status) HashAlg=$($catTest.HashAlgorithm)"
    Write-Output "  qcwlan8280.inf MD5=$infMd5"
    Write-Output "  qcwlan8280.cat MD5=$catMd5 ($((Get-Item $catPath).Length) B) SigThumb=$($sigCat.SignerCertificate.Thumbprint)"
}

Build-QcWlanPackage -PkgName 'pkg_s23_qcwlan_intx' -MsiSupported 0
Build-QcWlanPackage -PkgName 'pkg_s23_qcwlan_msi'  -MsiSupported 1
