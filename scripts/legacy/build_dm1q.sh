#!/bin/bash
set -e
cd ~/mu_s23/Mu-Silicium
echo "=== 1. Create Binaries/dm1q from dm2q (100% matched S8DYI BOOT.MXF.2.1.1-00218-KAILUA-1) ==="
rm -rf Binaries/dm1q
cp -a Binaries/dm2q Binaries/dm1q
echo "=== 2. Create Platforms/Samsung/dm1qPkg from dm2qPkg + b5qPkg Windows fixes ==="
rm -rf Platforms/Samsung/dm1qPkg
cp -a Platforms/Samsung/dm2qPkg Platforms/Samsung/dm1qPkg
mv Platforms/Samsung/dm1qPkg/dm2q.dec Platforms/Samsung/dm1qPkg/dm1q.dec
mv Platforms/Samsung/dm1qPkg/dm2q.dsc Platforms/Samsung/dm1qPkg/dm1q.dsc
mv Platforms/Samsung/dm1qPkg/dm2q.fdf Platforms/Samsung/dm1qPkg/dm1q.fdf
rm -rf Platforms/Samsung/dm1qPkg/FdtBlob
# Replace dm2q -> dm1q in dsc/fdf/Include/Library
python3 - << 'PYEOF'
import os, glob
pkg = os.path.expanduser("~/mu_s23/Mu-Silicium/Platforms/Samsung/dm1qPkg")
b5q = os.path.expanduser("~/mu_s23/Mu-Silicium/Platforms/Samsung/b5qPkg")
# 2a. APRIORI.inc and DXE.inc from b5qPkg (has IortDtDxe at line 4 + open-source DiskIoDxe/PartitionDxe), pointing to Binaries/dm1q!
for inc in ("APRIORI.inc", "DXE.inc"):
    txt = open(os.path.join(b5q, "Include", inc)).read().replace("Binaries/b5q/", "Binaries/dm1q/")
    open(os.path.join(pkg, "Include", inc), "w").write(txt)
raw_inc = open(os.path.join(pkg, "Include", "RAW.inc")).read().replace("Binaries/dm2q/", "Binaries/dm1q/")
open(os.path.join(pkg, "Include", "RAW.inc"), "w").write(raw_inc)
# 2b. dm1q.dsc: SM8550-AC, SimpleFbDxe 1080x2340x32, PcdPlatformType="WP", ACPI enabled!
dsc = open(os.path.join(pkg, "dm1q.dsc")).read()
dsc = dsc.replace("dm2q", "dm1q").replace("Galaxy S23+", "Galaxy S23")
dsc = dsc.replace("89B7F732-FC96-4A80-8D03-A6B654F63AFA", "4C8D9B1E-3F7A-4A52-9C11-D1A2E5B7C811")
dsc = dsc.replace('PcdPlatformType|"LA"', 'PcdPlatformType|"WP"')
dsc = dsc.replace("#dm1q/AcpiTables.inf", "dm1q/AcpiTables.inf")
if "dm1q/AcpiTables.inf" not in dsc:
    dsc += "\n[Components]\n  dm1q/AcpiTables.inf\n"
open(os.path.join(pkg, "dm1q.dsc"), "w").write(dsc)
# 2c. dm1q.fdf: take b5q.fdf (ACPI enabled, IortDtDxe included, DtPlatformDxe commented out) and replace b5q -> dm1q
fdf = open(os.path.join(b5q, "b5q.fdf")).read().replace("b5q", "dm1q")
open(os.path.join(pkg, "dm1q.fdf"), "w").write(fdf)
# 2d. Exact dm1q MemoryMapLib.c (merging b5q Windows carveouts + dm1q's exact xbl_config uefiplat.cfg!)
mmap_c = r'''#include <Library/MemoryMapLib.h>
STATIC
EFI_MEMORY_REGION_DESCRIPTOR
gMemoryDescriptor[] = {
  // Name, Address, Length, HobOption, ResourceType, ResourceAttribute, MemoryType, ArmAttribute
  // DDR Regions (exact Samsung Galaxy S23 dm1q S911BXXS8DYI3 + Gunyah/Windows carveouts)
  {"HYP",                0x80000000, 0x00A00000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK_XN},
  {"CPUSYS VM",          0x80A00000, 0x00400000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK_XN},
  {"NOMAP Tail",         0x80E00000, 0x00400000, NoMap,  MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"RSRV0",              0x81200000, 0x00800000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN},
  {"XBL DT",             0x81A00000, 0x00040000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN},
  {"XBL Ramdump",        0x81A40000, 0x001C0000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"AOP",                0x81C00000, 0x000F4000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"Chip Info",          0x81CF4000, 0x00001000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN},
  {"SMEM",               0x81D00000, 0x00200000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"ADSP MHI",           0x81F00000, 0x00020000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN},
  {"TZ STATS",           0x82700000, 0x00100000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"RSRV1",              0x86E00000, 0x02A00000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK_XN},
  {"PIL Reserved",       0x89800000, 0x19280000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"Display Demura",     0xA2A80000, 0x02B00000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_THROUGH_XN},
  {"RSRV2",              0xA5580000, 0x00B80000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK_XN},
  {"DBI Dump",           0xA6100000, 0x00F00000, NoHob,  MMAP_IO, INITIALIZED, Conv,   UNCACHED_UNBUFFERED_XN},
  {"FD Reserved",        0xA7000000, 0x00100000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK},
  {"UEFI FD",            0xA7100000, 0x00300000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK},
  {"UEFI FD Reserved",   0xA7400000, 0x00200000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK},
  {"CPU Vectors",        0xA7600000, 0x00001000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK},
  {"Info Blk",           0xA7601000, 0x00001000, AddMem, SYS_MEM, SYS_MEM_CAP, RtData, WRITE_BACK_XN},
  {"MMU PageTables",     0xA7602000, 0x00003000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK_XN},
  {"Log Buffer",         0xA7605000, 0x00008000, AddMem, SYS_MEM, SYS_MEM_CAP, RtData, WRITE_BACK_XN},
  {"UEFI Stack",         0xA760D000, 0x00040000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK_XN},
  {"SEC Heap",           0xA764D000, 0x0008C000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK_XN},
  {"Sched Heap",         0xA76D9000, 0x00400000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK_XN},
  {"FV Region",          0xA7AD9000, 0x00400000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK_XN},
  {"UEFI RESV",          0xA7ED9000, 0x00127000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK_XN},
  {"Kernel",             0xA8000000, 0x10000000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN},
  {"Display Reserved",   0xB8000000, 0x02B00000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_THROUGH_XN},
  {"DXE Heap",           0xBAB00000, 0x0D1C0000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN},
  {"CPUCP FW",           0xD8140000, 0x001C0000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, UNCACHED_UNBUFFERED_XN},
  {"QTEE",               0xD8300000, 0x00500000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"TZApps Reserved",    0xD8800000, 0x08A00000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"RSRV3",              0xEEB00000, 0x04B00000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK_XN},
  {"NOMAP High",         0xFF800000, 0x00800000, NoMap,  MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  // Other Memory Regions
  {"IMEM Base",          0x14680000, 0x0002A000, NoHob,  MMAP_IO, INITIALIZED, Conv,   NS_DEVICE},
  {"IMEM Cookie Base",   0x146AA000, 0x00016000, AddDev, MMAP_IO, INITIALIZED, Conv,   NS_DEVICE},
  // Register Regions
  {"IPC_ROUTER_TOP",     0x00400000, 0x00100000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"SECURITY CONTROL",   0x00780000, 0x00007000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"QUP",                0x00800000, 0x00300000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"PRNG_CFG_PRNG",      0x010C0000, 0x0000C000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"CRYPTO0 CRYPTO",     0x01DC0000, 0x00040000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"CORE_TOP_CSR",       0x01F00000, 0x00100000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"PERIPH_SS",          0x08800000, 0x00100000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"USB",                0x0A600000, 0x00200000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"AOSS",               0x0B000000, 0x04000000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"TLMM",               0x0F000000, 0x01000000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"SMMU",               0x15000000, 0x00200000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"APSS_HM",            0x17000000, 0x02000000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE}
};
VOID
GetMemoryMap (
  OUT EFI_MEMORY_REGION_DESCRIPTOR **MemoryDescriptor,
  OUT UINT8                         *MemoryDescriptorCount)
{
  *MemoryDescriptor      = gMemoryDescriptor;
  *MemoryDescriptorCount = ARRAY_SIZE (gMemoryDescriptor);
}
'''
open(os.path.join(pkg, "Library", "MemoryMapLib", "MemoryMapLib.c"), "w").write(mmap_c)
# 2e. Fix Platform.py or any dm2q references in Library/*.inf
for root, _, files in os.walk(pkg):
    for fn in files:
        if fn.endswith((".inf", ".c", ".h", ".py")):
            p = os.path.join(root, fn)
            t = open(p, errors="ignore").read()
            if "dm2q" in t:
                open(p, "w").write(t.replace("dm2q", "dm1q"))
print("dm1qPkg created cleanly!")
PYEOF
echo "=== 3. Create Silicium-ACPI/Platforms/Samsung/dm1q from b5q + PPTT ==="
rm -rf Silicium-ACPI/Platforms/Samsung/dm1q
cp -a Silicium-ACPI/Platforms/Samsung/b5q Silicium-ACPI/Platforms/Samsung/dm1q
# Add Kailua/PPTT.aml to AcpiTables.inf for clean 8-core ARM64 topology in Windows 11 24H2!
cat << 'EOF' > Silicium-ACPI/Platforms/Samsung/dm1q/AcpiTables.inf
[Defines]
  INF_VERSION                    = 0x00010005
  BASE_NAME                      = AcpiTables
  FILE_GUID                      = 7E374E25-8E01-4FEE-87F2-390C23C606CD
  MODULE_TYPE                    = USER_DEFINED
  VERSION_STRING                 = 1.0
[Binaries]
  ASL|DSDT.aml
  ASL|Common/SSDT.aml
  ASL|Kailua/APIC.aml
  ASL|Kailua/DBG2.aml
  ASL|Kailua/FACP.aml
  ASL|Kailua/GTDT.aml
  ASL|Kailua/PPTT.aml
EOF
echo "=== 4. Create Resources/DTBs/dm1q.dtb and Resources/Configs/dm1q.toml ==="
dtc -I dtb -O dts Resources/DTBs/dm2q.dtb 2>/dev/null | \
  sed 's/Samsung DM2Q PROJECT/Samsung DM1Q PROJECT/g; s/DM2_S6E3FAC_AMB655AY01/DM1_S6E3FAC_AMB606AW01/g' | \
  dtc -I dts -O dtb -o Resources/DTBs/dm1q.dtb - 2>/dev/null
sed 's/dm2q/dm1q/g' Resources/Configs/dm2q.toml > Resources/Configs/dm1q.toml
echo "=== 5. Build Mu-dm1q.img (RELEASE) ==="
source ~/mu_s23/venv/bin/activate
python3 build_uefi.py -d dm1q -r RELEASE
ls -lh Mu-dm1q.img
cp Mu-dm1q.img /mnt/c/Users/Mahan/Documents/windows-on-android/s23_dm1q/Mu-dm1q.img
echo "BUILD_COMPLETE!"
