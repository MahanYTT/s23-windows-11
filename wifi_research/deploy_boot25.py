import glob, hashlib, os, subprocess, time

WIM_DIR   = r"C:\Users\Mahan\Documents\windows-on-android\win11_arm64\twrp_wimlib"
SCRATCH   = r"C:\Users\Mahan\.gemini\antigravity\brain\9c658663-7a3d-4ac0-af0f-1d608772d9d0\scratch"
BATT_DIR  = r"C:\Users\Mahan\Documents\windows-on-android\s23_dm1q\drivers\s23batt"
PROBE_SYS = r"C:\Users\Mahan\Documents\windows-on-android\s23_dm1q\drivers\s23pcieprobe\out\s23pcieprobe.sys"
WIFI_DRV  = r"C:\Users\Mahan\Documents\windows-on-android\s23_dm1q\downloaded_qcwlan8280\extracted"
WIFI_FW   = r"C:\Users\Mahan\Documents\windows-on-android\s23_dm1q\wifi_research\s23_vendor_wifi_fw"

def sh(c):
    env = os.environ.copy()
    env["MSYS_NO_PATHCONV"] = "1"
    r = subprocess.run(["adb", "shell", c], capture_output=True, text=True, env=env)
    if r.returncode != 0:
        raise RuntimeError(f"Command failed ({r.returncode}): {c}\n{r.stdout}\n{r.stderr}")
    return (r.stdout + r.stderr).strip()

def push(local_path, remote_path):
    env = os.environ.copy()
    env["MSYS_NO_PATHCONV"] = "1"
    r = subprocess.run(["adb", "push", local_path, remote_path], capture_output=True, text=True, env=env)
    if r.returncode != 0:
        raise RuntimeError(f"Push failed: {local_path} -> {remote_path}\n{r.stdout}\n{r.stderr}")

def md5_local(p):
    return hashlib.md5(open(p, "rb").read()).hexdigest().lower()

print("=== 1. Push musl ntfs-3g & ntfsfix ===")
sh("mkdir -p /tmp/wimlib/bin /tmp/wimlib/lib /tmp/win_rw && umount /tmp/win_ro 2>/dev/null || true && umount /tmp/win_rw 2>/dev/null || true")
push(os.path.join(WIM_DIR, "bin", "ntfs-3g"), "/tmp/wimlib/bin/ntfs-3g")
push(os.path.join(WIM_DIR, "usr", "bin", "ntfsfix"), "/tmp/wimlib/bin/ntfsfix")
for sub in ["lib", os.path.join("usr", "lib")]:
    d = os.path.join(WIM_DIR, sub)
    if os.path.isdir(d):
        for f in os.listdir(d):
            p = os.path.join(d, f)
            if os.path.isfile(p) and ".so" in f:
                push(p, f"/tmp/wimlib/lib/{f}")
sh("chmod 755 /tmp/wimlib/bin/* /tmp/wimlib/lib/*")

print("=== 2. Run ntfsfix -d and mount /tmp/win_rw RW ===")
print(sh("/tmp/wimlib/lib/ld-musl-aarch64.so.1 --library-path /tmp/wimlib/lib /tmp/wimlib/bin/ntfsfix -d /dev/block/by-name/win"))
print(sh("/tmp/wimlib/lib/ld-musl-aarch64.so.1 --library-path /tmp/wimlib/lib /tmp/wimlib/bin/ntfs-3g /dev/block/by-name/win /tmp/win_rw -o rw,remove_hiberfile"))
mounts = sh("grep /tmp/win_rw /proc/mounts")
print("MOUNT:", mounts)
assert " rw," in mounts, f"Not mounted RW! {mounts}"

print("=== 3. Pre-allocate 64 KB zeroed logs and clear stale signals ===")
sh("""
rm -f /tmp/win_rw/s23_v13_ready.txt /tmp/win_rw/s23_sys_done.txt
mkdir -p /tmp/win_rw/Users/Public /tmp/win_rw/s23_batt_pkg /tmp/win_rw/s23_wifi_pkg/s23_stock_fw
for f in s23touch.log s23batt.log s23pcieprobe.log s23_sys.txt s23_diag.txt s23_agent.log; do
    dd if=/dev/zero of=/tmp/win_rw/Users/Public/$f bs=65536 count=1 conv=notrunc 2>/dev/null
    truncate -s 65536 /tmp/win_rw/Users/Public/$f
done
""")

print("=== 4. Push files and verify read-back MD5s ===")
deploy_pairs = [
    (os.path.join(SCRATCH, "s23_sys_install.ps1"), "/tmp/win_rw/s23_sys_install.ps1"),
    (PROBE_SYS, "/tmp/win_rw/Users/Public/s23pcieprobe.sys"),
    (os.path.join(BATT_DIR, "install_batt_live.ps1"), "/tmp/win_rw/s23_batt_pkg/install_batt_live.ps1"),
    (os.path.join(BATT_DIR, "pkg", "s23batt.sys"), "/tmp/win_rw/s23_batt_pkg/s23batt.sys"),
    (os.path.join(BATT_DIR, "pkg", "s23batt.inf"), "/tmp/win_rw/s23_batt_pkg/s23batt.inf"),
    (os.path.join(BATT_DIR, "pkg", "s23batt.cat"), "/tmp/win_rw/s23_batt_pkg/s23batt.cat"),
    (os.path.join(BATT_DIR, "pkg", "s23test.cer"), "/tmp/win_rw/s23_batt_pkg/s23test.cer"),
]

for f in sorted(os.listdir(WIFI_DRV)):
    lp = os.path.join(WIFI_DRV, f)
    if os.path.isfile(lp):
        deploy_pairs.append((lp, f"/tmp/win_rw/s23_wifi_pkg/{f}"))

for f in sorted(os.listdir(WIFI_FW)):
    lp = os.path.join(WIFI_FW, f)
    if os.path.isfile(lp):
        deploy_pairs.append((lp, f"/tmp/win_rw/s23_wifi_pkg/s23_stock_fw/{f}"))

qca6490_dir = os.path.join(WIFI_FW, "qca6490")
for f in sorted(os.listdir(qca6490_dir)):
    lp = os.path.join(qca6490_dir, f)
    if os.path.isfile(lp):
        deploy_pairs.append((lp, f"/tmp/win_rw/s23_wifi_pkg/s23_stock_fw/{f}"))

for lp, rp in deploy_pairs:
    push(lp, rp)

sh("sync")

# Batch verify MD5s
remote_paths = " ".join(f'"{rp}"' for _, rp in deploy_pairs)
md5_out = sh(f"md5sum {remote_paths}")
remote_md5s = {}
for line in md5_out.splitlines():
    parts = line.strip().split(None, 1)
    if len(parts) == 2:
        remote_md5s[parts[1].strip()] = parts[0].strip().lower()

for lp, rp in deploy_pairs:
    exp = md5_local(lp)
    got = remote_md5s.get(rp, "")
    assert exp == got, f"MD5 mismatch for {rp}: expected {exp}, got {got}"

print(f"Verified read-back MD5s for all {len(deploy_pairs)} deployed files!")

print("=== 5. Sync and clean unmount ===")
sh("sync && umount /tmp/win_rw && sync")
rem = sh("grep /tmp/win_rw /proc/mounts || true")
assert rem == "", f"Still mounted: {rem}"
print("Clean unmount verified. Rebooting S23 into Windows 11...")
subprocess.run(["adb", "reboot"], check=True)
