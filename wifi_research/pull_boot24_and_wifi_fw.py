import os, subprocess

OUT_DIR = r"C:\Users\Mahan\Documents\windows-on-android\s23_dm1q\wifi_research\boot24_logs"
FW_DIR  = r"C:\Users\Mahan\Documents\windows-on-android\s23_dm1q\wifi_research\s23_vendor_wifi_fw"
os.makedirs(OUT_DIR, exist_ok=True)
os.makedirs(FW_DIR, exist_ok=True)

def sh(cmd, timeout=60):
    r = subprocess.run(["adb", "shell", cmd], capture_output=True, text=True, timeout=timeout)
    return (r.stdout + r.stderr).strip()

print("=== 1. Mounting Windows partition read-only to pull s23pcieprobe.log ===")
WIM_DIR = r"C:\Users\Mahan\Documents\windows-on-android\s23_dm1q\wimlib_Tools"
sh("mkdir -p /tmp/wimlib/bin /tmp/wimlib/lib /tmp/win_ro && umount /tmp/win_ro 2>/dev/null || true")
for f in ["ntfs-3g", "ntfsfix"]:
    subprocess.run(["adb", "push", os.path.join(WIM_DIR, "bin", f), f"/tmp/wimlib/bin/{f}"], capture_output=True)
for root, _, files in os.walk(os.path.join(WIM_DIR, "lib")):
    for f in files:
        subprocess.run(["adb", "push", os.path.join(root, f), f"/tmp/wimlib/lib/{f}"], capture_output=True)
sh("chmod 755 /tmp/wimlib/bin/* /tmp/wimlib/lib/*")
print("Mount RO:", sh("/tmp/wimlib/lib/ld-musl-aarch64.so.1 --library-path /tmp/wimlib/lib /tmp/wimlib/bin/ntfs-3g /dev/block/by-name/win /tmp/win_ro -o ro"))

for f in ["s23pcieprobe.log", "s23batt.log", "s23touch.log", "s23_sys.txt", "s23_agent.log"]:
    local = os.path.join(OUT_DIR, f)
    r = subprocess.run(["adb", "pull", f"/tmp/win_ro/Users/Public/{f}", local], capture_output=True, text=True)
    print(f"Pull {f}: rc={r.returncode} ({r.stdout.strip()})")

sh("umount /tmp/win_ro 2>/dev/null || true")

probe_log = os.path.join(OUT_DIR, "s23pcieprobe.log")
if os.path.exists(probe_log):
    print("\n=== s23pcieprobe.log (FILE_WRITE_THROUGH) ===")
    raw = open(probe_log, "rb").read().rstrip(b"\x00").decode("ascii", errors="replace")
    print(raw)

print("\n=== 2. Locating S23 stock QCA6490 / WCN6856 firmware in /vendor ===")
print(sh("""
mkdir -p /tmp/vendor_ro
if ! grep -q '/tmp/vendor_ro' /proc/mounts; then
    dmsetup remove_all 2>/dev/null || true
    lptools_new --super /dev/block/by-name/super --slot 0 --suffix _a --map 2>/dev/null || true
    for dev in /dev/block/mapper/vendor_a /dev/block/mapper/vendor /dev/block/dm-*; do
        if [ -b "$dev" ]; then
            mount -o ro "$dev" /tmp/vendor_ro 2>/dev/null && break
        fi
    done
fi
find /tmp/vendor_ro /vendor -iname "*bdwlan*" -o -iname "*wlan*" -o -iname "*qca6490*" -o -iname "*amss.bin*" -o -iname "*m3.bin*" -o -iname "*Data.msc*" -o -iname "*regdb*" 2>/dev/null | head -n 100
"""))
