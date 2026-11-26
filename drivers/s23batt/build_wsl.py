import os, struct, subprocess

import shutil


def _find_tool(env_name, candidates):
    v = os.environ.get(env_name)
    if v:
        return v
    for c in candidates:
        p = shutil.which(c)
        if p:
            return p
    return candidates[0]


CC = _find_tool("S23_CC", ["clang-18", "clang-19", "clang-17", "clang"])
LLD = _find_tool("S23_LLD", ["lld-link", "lld-link-18", "lld-link-19"])
DLLTOOL = _find_tool("S23_DLLTOOL", ["llvm-dlltool", "llvm-dlltool-18", "llvm-dlltool-19"])

# Directory of this script (works from any checkout, e.g. /mnt/c/<repo>/drivers/s23batt); S23_DRV_DIR overrides it.
DRV_DIR = os.environ.get("S23_DRV_DIR") or os.path.dirname(os.path.abspath(__file__))
OUT_DIR = f"{DRV_DIR}/out"
os.makedirs(OUT_DIR, exist_ok=True)

nt_def = """LIBRARY ntoskrnl.exe
EXPORTS
  MmMapIoSpace
  MmUnmapIoSpace
  PsCreateSystemThread
  PsTerminateSystemThread
  ObReferenceObjectByHandle
  ObfDereferenceObject
  KeWaitForSingleObject
  KeDelayExecutionThread
  KeStallExecutionProcessor
  KeInitializeEvent
  KeSetEvent
  IoCreateDevice
  IoDeleteDevice
  IoAttachDeviceToDeviceStack
  IoDetachDevice
  IoInitializeRemoveLockEx
  IoAcquireRemoveLockEx
  IoReleaseRemoveLockEx
  IoReleaseRemoveLockAndWaitEx
  IoWMIRegistrationControl
  IofCallDriver
  IofCompleteRequest
  PoCallDriver
  PoStartNextPowerIrp
  ZwCreateFile
  ZwWriteFile
  ZwClose
  RtlInitUnicodeString
"""

bc_def = """LIBRARY battc.sys
EXPORTS
  BatteryClassInitializeDevice
  BatteryClassUnload
  BatteryClassIoctl
  BatteryClassStatusNotify
  BatteryClassSystemControl
  BatteryClassQueryWmiDataBlock
"""

wmi_def = """LIBRARY wmilib.sys
EXPORTS
  WmiCompleteRequest
"""

open(f"{OUT_DIR}/ntoskrnl.def", "w").write(nt_def)
open(f"{OUT_DIR}/battc.def", "w").write(bc_def)
open(f"{OUT_DIR}/wmilib.def", "w").write(wmi_def)

subprocess.check_call([DLLTOOL, "-m", "arm64", "-d", f"{OUT_DIR}/ntoskrnl.def", "-l", f"{OUT_DIR}/ntoskrnl.lib"])
subprocess.check_call([DLLTOOL, "-m", "arm64", "-d", f"{OUT_DIR}/battc.def", "-l", f"{OUT_DIR}/battc.lib"])
subprocess.check_call([DLLTOOL, "-m", "arm64", "-d", f"{OUT_DIR}/wmilib.def", "-l", f"{OUT_DIR}/wmilib.lib"])

cc_cmd = [
    CC,
    "--target=aarch64-pc-windows-msvc",
    "-ffreestanding",
    "-fno-builtin",
    "-fno-stack-protector",
    "-mno-stack-arg-probe",
    "-O2",
    "-Wall", "-Wextra",
    "-c", f"{DRV_DIR}/s23batt.c",
    "-o", f"{OUT_DIR}/s23batt.obj",
]
print("CC:", " ".join(cc_cmd))
subprocess.check_call(cc_cmd)

sys_path = f"{OUT_DIR}/s23batt.sys"
ld_cmd = [
    LLD,
    "/NOLOGO",
    "/DRIVER:WDM",
    "/SUBSYSTEM:NATIVE,10.00",
    "/OSVERSION:10.0",
    "/BASE:0x1C0000000",
    "/DYNAMICBASE",
    "/HIGHENTROPYVA",
    "/FIXED:NO",
    "/NXCOMPAT",
    "/TSAWARE:NO",
    "/INCREMENTAL:NO",
    "/ENTRY:DriverEntry",
    "/NODEFAULTLIB",
    "/MACHINE:ARM64",
    "/OPT:REF",
    "/OPT:ICF",
    f"/OUT:{sys_path}",
    f"{OUT_DIR}/s23batt.obj",
    f"{OUT_DIR}/ntoskrnl.lib",
    f"{OUT_DIR}/battc.lib",
    f"{OUT_DIR}/wmilib.lib",
]
print("LD:", " ".join(ld_cmd))
subprocess.check_call(ld_cmd)

data = bytearray(open(sys_path, "rb").read())
pe = struct.unpack_from("<I", data, 0x3C)[0]
csum_off = pe + 24 + 64
struct.pack_into("<I", data, csum_off, 0)
s = 0
n = len(data)
if n & 1:
    data.append(0)
for i in range(0, len(data), 2):
    s += data[i] | (data[i + 1] << 8)
    s = (s & 0xFFFF) + (s >> 16)
s = (s & 0xFFFF) + (s >> 16)
csum = (s + n) & 0xFFFFFFFF
struct.pack_into("<I", data, csum_off, csum)
open(sys_path, "wb").write(data[:n])

machine = struct.unpack_from("<H", data, pe + 4)[0]
magic   = struct.unpack_from("<H", data, pe + 24)[0]
entry   = struct.unpack_from("<I", data, pe + 40)[0]
subsys  = struct.unpack_from("<H", data, pe + 24 + 68)[0]
dllch   = struct.unpack_from("<H", data, pe + 24 + 70)[0]
reloc_rva, reloc_sz = struct.unpack_from("<2I", data, pe + 24 + 112 + 5 * 8)
print(f"BUILT s23batt.sys: {n} bytes, Machine=0x{machine:04X}, Magic=0x{magic:04X}, Entry=0x{entry:X}, Subsys={subsys}, DllChars=0x{dllch:04X}, Reloc=0x{reloc_rva:X}+{reloc_sz}, CheckSum=0x{csum:08X}")
assert reloc_sz > 0, "Missing .reloc section!"
objdump = _find_tool("S23_OBJDUMP", ["aarch64-linux-gnu-objdump", "llvm-objdump"])
obj_flag = "--private-headers" if "llvm-objdump" in os.path.basename(objdump) else "-p"
out = subprocess.check_output([objdump, obj_flag, sys_path]).decode() if shutil.which(objdump) else ""
for l in out.splitlines():
    if "DLL Name:" in l or "Hint/Name" in l or (l.startswith("\t") and len(l.strip().split()) == 3 and l.strip().split()[0].isalnum()):
        print(" ", l.strip())
