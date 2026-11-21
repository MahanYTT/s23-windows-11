import os, struct, subprocess

DRV_DIR = "/mnt/c/Users/Mahan/Documents/windows-on-android/s23_dm1q/drivers/s23batt"
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

subprocess.check_call(["llvm-dlltool", "-m", "arm64", "-d", f"{OUT_DIR}/ntoskrnl.def", "-l", f"{OUT_DIR}/ntoskrnl.lib"])
subprocess.check_call(["llvm-dlltool", "-m", "arm64", "-d", f"{OUT_DIR}/battc.def", "-l", f"{OUT_DIR}/battc.lib"])
subprocess.check_call(["llvm-dlltool", "-m", "arm64", "-d", f"{OUT_DIR}/wmilib.def", "-l", f"{OUT_DIR}/wmilib.lib"])

cc_cmd = [
    "clang-18",
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
    "lld-link",
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
out = subprocess.check_output(["aarch64-linux-gnu-objdump", "-p", sys_path]).decode()
for l in out.splitlines():
    if "DLL Name:" in l or "Hint/Name" in l or (l.startswith("\t") and len(l.strip().split()) == 3 and l.strip().split()[0].isalnum()):
        print(" ", l.strip())
