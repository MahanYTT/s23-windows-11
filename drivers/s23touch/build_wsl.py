import os, struct, subprocess

# Directory of this script (works from any checkout, e.g. /mnt/c/<repo>/drivers/s23touch); S23_DRV_DIR overrides it.
DRV_DIR = os.environ.get("S23_DRV_DIR") or os.path.dirname(os.path.abspath(__file__))
OUT_DIR = f"{DRV_DIR}/out"
os.makedirs(OUT_DIR, exist_ok=True)

# 1. Generate ARM64 import libraries for ntoskrnl.exe and hidclass.sys via llvm-dlltool
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
  KeQueryPerformanceCounter
  KeInitializeSpinLock
  KeAcquireSpinLockRaiseToDpc
  KeReleaseSpinLock
  KeInitializeEvent
  KeSetEvent
  IofCallDriver
  IofCompleteRequest
  IoAcquireCancelSpinLock
  IoReleaseCancelSpinLock
  PoCallDriver
  PoStartNextPowerIrp
  ZwCreateFile
  ZwWriteFile
  ZwClose
  RtlInitUnicodeString
"""

hc_def = """LIBRARY hidclass.sys
EXPORTS
  HidRegisterMinidriver
"""

open(f"{OUT_DIR}/ntoskrnl.def", "w").write(nt_def)
open(f"{OUT_DIR}/hidclass.def", "w").write(hc_def)

subprocess.check_call(["llvm-dlltool", "-m", "arm64", "-d", f"{OUT_DIR}/ntoskrnl.def", "-l", f"{OUT_DIR}/ntoskrnl.lib"])
subprocess.check_call(["llvm-dlltool", "-m", "arm64", "-d", f"{OUT_DIR}/hidclass.def", "-l", f"{OUT_DIR}/hidclass.lib"])

# 2. Compile s23touch.c -> s23touch.obj for ARM64 Windows MSVC ABI
cc_cmd = [
    "clang-18",
    "--target=aarch64-pc-windows-msvc",
    "-ffreestanding",
    "-fno-builtin",
    "-fno-stack-protector",
    "-mno-stack-arg-probe",
    "-O2",
    "-Wall", "-Wextra",
    "-c", f"{DRV_DIR}/s23touch.c",
    "-o", f"{OUT_DIR}/s23touch.obj",
]
print("CC:", " ".join(cc_cmd))
subprocess.check_call(cc_cmd)

# 3. Link s23touch.sys (WDM kernel driver, ARM64, no CRT)
sys_path = f"{OUT_DIR}/s23touch.sys"
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
    f"{OUT_DIR}/s23touch.obj",
    f"{OUT_DIR}/ntoskrnl.lib",
    f"{OUT_DIR}/hidclass.lib",
]
print("LD:", " ".join(ld_cmd))
subprocess.check_call(ld_cmd)

# 4. Compute & write valid PE CheckSum (required by Windows MmLoadSystemImage!)
data = bytearray(open(sys_path, "rb").read())
pe = struct.unpack_from("<I", data, 0x3C)[0]
csum_off = pe + 24 + 64
struct.pack_into("<I", data, csum_off, 0)
# Standard Windows PE checksum
s = 0
n = len(data)
if n & 1:
    data.append(0)
for i in range(0, len(data), 2):
    s += data[i] | (data[i+1] << 8)
    s = (s & 0xFFFF) + (s >> 16)
s = (s & 0xFFFF) + (s >> 16)
csum = (s + n) & 0xFFFFFFFF
struct.pack_into("<I", data, csum_off, csum)
open(sys_path, "wb").write(data[:n])

# 5. Verify PE headers & imports
machine = struct.unpack_from("<H", data, pe + 4)[0]
magic   = struct.unpack_from("<H", data, pe + 24)[0]
entry   = struct.unpack_from("<I", data, pe + 40)[0]
subsys  = struct.unpack_from("<H", data, pe + 24 + 68)[0]
dllch   = struct.unpack_from("<H", data, pe + 24 + 70)[0]
print(f"BUILT s23touch.sys: {n} bytes, Machine=0x{machine:04X}, Magic=0x{magic:04X}, Entry=0x{entry:X}, Subsys={subsys}, DllChars=0x{dllch:04X}, CheckSum=0x{csum:08X}")
out = subprocess.check_output(["aarch64-linux-gnu-objdump", "-p", sys_path]).decode()
for l in out.splitlines():
    if "DLL Name:" in l or "Hint/Name" in l or (l.startswith("\t") and len(l.strip().split()) == 3 and l.strip().split()[0].isalnum()):
        print(" ", l.strip())
