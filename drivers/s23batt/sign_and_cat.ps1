$ErrorActionPreference = 'Stop'

$drvDir = 'C:\Users\Mahan\Documents\windows-on-android\s23_dm1q\drivers\s23batt'
$pkgDir = Join-Path $drvDir 'pkg'
$stageDir = Join-Path $drvDir 'cat_stage'
New-Item -ItemType Directory -Force -Path $pkgDir | Out-Null
if (Test-Path $stageDir) { Remove-Item -Recurse -Force $stageDir }
New-Item -ItemType Directory -Force -Path $stageDir | Out-Null

$cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq 'CN=S23TouchTest2020' -and $_.HasPrivateKey } | Select-Object -First 1
if (-not $cert) {
    throw "CN=S23TouchTest2020 cert not found in Cert:\CurrentUser\My"
}
Write-Output "Using cert: $($cert.Thumbprint) ($($cert.Subject))"

$cerPath = Join-Path $pkgDir 's23test.cer'
Export-Certificate -Cert $cert -FilePath $cerPath -Force | Out-Null

Copy-Item (Join-Path $drvDir 's23batt.inf') (Join-Path $stageDir 's23batt.inf') -Force
Copy-Item (Join-Path $drvDir 'out\s23batt.sys') (Join-Path $stageDir 's23batt.sys') -Force

$stageSys = Join-Path $stageDir 's23batt.sys'
$sigSys = Set-AuthenticodeSignature -FilePath $stageSys -Certificate $cert -HashAlgorithm SHA256
Write-Output "Signed s23batt.sys: Status=$($sigSys.Status)"

$catPath = Join-Path $pkgDir 's23batt.cat'
if (Test-Path $catPath) { Remove-Item $catPath -Force }
New-FileCatalog -Path $stageDir -CatalogFilePath $catPath -CatalogVersion 2 | Out-Null

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class CatAugment {
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
            byte[] hwVal = Utf16Z("root\\s23batt");
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

$nMembers = [CatAugment]::Augment($catPath)
Write-Output "Augmented $nMembers catalog members"

$sigCat = Set-AuthenticodeSignature -FilePath $catPath -Certificate $cert -HashAlgorithm SHA256
Write-Output "Signed s23batt.cat: Status=$($sigCat.Status)"

Copy-Item (Join-Path $stageDir 's23batt.inf') (Join-Path $pkgDir 's23batt.inf') -Force
Copy-Item $stageSys (Join-Path $pkgDir 's23batt.sys') -Force

$catTest = Test-FileCatalog -CatalogFilePath $catPath -Path $stageDir -Detailed
Write-Output "Test-FileCatalog Status: $($catTest.Status)"
Get-ChildItem $pkgDir | Select-Object Name, Length
