import os
import shutil
import struct
import subprocess


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

if os.name == "nt" and not (shutil.which(CC) and shutil.which(LLD) and shutil.which(DLLTOOL)):
    _wsl_script = os.path.abspath(__file__).replace("\\", "/")
    if len(_wsl_script) >= 2 and _wsl_script[1] == ":":
        _wsl_script = f"/mnt/{_wsl_script[0].lower()}{_wsl_script[2:]}"
    raise SystemExit(subprocess.call(["wsl", "python3", _wsl_script]))

DRV_DIR = os.environ.get("S23_DRV_DIR") or os.path.dirname(os.path.abspath(__file__))
OUT_DIR = f"{DRV_DIR}/out"
os.makedirs(OUT_DIR, exist_ok=True)

nt_def = """LIBRARY ntoskrnl.exe
EXPORTS
  MmMapIoSpace
  MmUnmapIoSpace
  KeStallExecutionProcessor
  KeInitializeSpinLock
  KeAcquireSpinLockRaiseToDpc
  KeReleaseSpinLock
  IoCreateDevice
  IoDeleteDevice
  IoCreateSymbolicLink
  IoDeleteSymbolicLink
  IofCompleteRequest
  RtlInitUnicodeString
  ZwCreateFile
  ZwWriteFile
  ZwOpenKey
  ZwQueryValueKey
  ZwClose
"""

with open(f"{OUT_DIR}/ntoskrnl.def", "w", encoding="utf-8") as f:
    f.write(nt_def)

subprocess.check_call([
    DLLTOOL, "-m", "arm64",
    "-d", f"{OUT_DIR}/ntoskrnl.def",
    "-l", f"{OUT_DIR}/ntoskrnl.lib",
])

cc_cmd = [
    CC,
    "--target=aarch64-pc-windows-msvc",
    "-ffreestanding",
    "-fno-builtin",
    "-fno-stack-protector",
    "-mno-stack-arg-probe",
    "-O2",
    "-Wall", "-Wextra", "-Werror",
    "-c", f"{DRV_DIR}/s23usbc.c",
    "-o", f"{OUT_DIR}/s23usbc.obj",
]
print("CC:", " ".join(cc_cmd))
subprocess.check_call(cc_cmd)

sys_path = f"{OUT_DIR}/s23usbc.sys"
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
    f"{OUT_DIR}/s23usbc.obj",
    f"{OUT_DIR}/ntoskrnl.lib",
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
with open(sys_path, "wb") as f:
    f.write(data[:n])

machine = struct.unpack_from("<H", data, pe + 4)[0]
reloc_rva, reloc_sz = struct.unpack_from("<2I", data, pe + 24 + 112 + 5 * 8)
print(
    f"BUILT s23usbc.sys: {n} bytes, Machine=0x{machine:04X}, "
    f"Reloc=0x{reloc_rva:X}+{reloc_sz}, CheckSum=0x{csum:08X}"
)
assert machine == 0xAA64, f"Expected ARM64 machine 0xAA64, got 0x{machine:04X}"
assert reloc_sz > 0, "Missing .reloc section!"
