param(
    # Driver directory (defaults to the folder of this script, i.e. drivers\s23keys of the current checkout)
    [string]$DriverDir = $PSScriptRoot
)
$ErrorActionPreference = 'Stop'

$drvDir = $DriverDir
$pkgDir = Join-Path $drvDir 'pkg'
$stageDir = Join-Path $drvDir 'cat_stage'
New-Item -ItemType Directory -Force -Path $pkgDir | Out-Null
if (Test-Path $stageDir) { Remove-Item -Recurse -Force $stageDir }
New-Item -ItemType Directory -Force -Path $stageDir | Out-Null

$cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object {
    $_.Subject -eq 'CN=S23TouchTest2020' -and $_.HasPrivateKey
} | Select-Object -First 1

if (-not $cert) {
    throw "Certificate CN=S23TouchTest2020 not found!"
}
Write-Output "Using cert: $($cert.Thumbprint) ($($cert.Subject))"

$cerPath = Join-Path $pkgDir 's23test.cer'
Export-Certificate -Cert $cert -FilePath $cerPath -Force | Out-Null

Copy-Item (Join-Path $drvDir 's23keys.inf') (Join-Path $stageDir 's23keys.inf') -Force
Copy-Item (Join-Path $drvDir 'out\s23keys.sys') (Join-Path $stageDir 's23keys.sys') -Force

$stageSys = Join-Path $stageDir 's23keys.sys'
$sigSys = Set-AuthenticodeSignature -FilePath $stageSys -Certificate $cert -HashAlgorithm SHA256
Write-Output "Signed s23keys.sys: Status=$($sigSys.Status)"
if ($sigSys.Status -ne 'Valid' -and $sigSys.Status -ne 'UnknownError') {
    throw "Unexpected Authenticode status for s23keys.sys: $($sigSys.Status)"
}

$catPath = Join-Path $pkgDir 's23keys.cat'
if (Test-Path $catPath) { Remove-Item $catPath -Force }
New-FileCatalog -Path $stageDir -CatalogFilePath $catPath -CatalogVersion 2 | Out-Null

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class KeysCatAugment {
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
            byte[] hwVal = Utf16Z("root\\s23keys");
            if (CryptCATPutCatAttrInfo(hCat, "HWID1", CRYPTCAT_ATTR_FLAGS, (uint)hwVal.Length, hwVal) == IntPtr.Zero)
                throw new Exception("PutCatAttr HWID1 failed: " + Marshal.GetLastWin32Error());
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

$nMembers = [KeysCatAugment]::Augment($catPath)
Write-Output "Augmented $nMembers catalog members with OSAttr=2:10.0, OS=10_ARM64, HWID1=root\s23keys"

$sigCat = Set-AuthenticodeSignature -FilePath $catPath -Certificate $cert -HashAlgorithm SHA256
Write-Output "Signed s23keys.cat: Status=$($sigCat.Status)"
if ($sigCat.Status -ne 'Valid' -and $sigCat.Status -ne 'UnknownError') {
    throw "Unexpected Authenticode status for s23keys.cat: $($sigCat.Status)"
}

Copy-Item (Join-Path $stageDir 's23keys.inf') (Join-Path $pkgDir 's23keys.inf') -Force
Copy-Item $stageSys (Join-Path $pkgDir 's23keys.sys') -Force

$catTest = Test-FileCatalog -CatalogFilePath $catPath -Path $stageDir -Detailed
Write-Output "Test-FileCatalog Status: $($catTest.Status)"
if ($catTest.Status -ne 'Valid') {
    throw "Catalog validation failed: $($catTest.Status)"
}
Remove-Item -Recurse -Force $stageDir
Get-ChildItem $pkgDir | Select-Object Name, Length
