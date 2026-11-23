import hashlib
import os
import subprocess

ADB = r"C:\Users\Mahan\AppData\Local\Microsoft\WinGet\Packages\Google.PlatformTools_Microsoft.Winget.Source_8wekyb3d8bbwe\platform-tools\adb.exe"
BASE = r"C:\Users\Mahan\Documents\windows-on-android\s23_dm1q"
WRITES = [
    (
        os.path.join(BASE, r"wifi_research\boot_now_inspect\SYSTEM_aclfix"),
        "/tmp/win_rw/Windows/System32/config/SYSTEM",
    ),
    (
        os.path.join(BASE, r"wifi_research\boot_scripts\s23_fix_acl.cmd"),
        "/tmp/win_rw/s23_fix_acl.cmd",
    ),
]


def md5_local(p):
    return hashlib.md5(open(p, "rb").read()).hexdigest()


def adb(*args, check=True):
    env = os.environ.copy()
    env["MSYS_NO_PATHCONV"] = "1"
    r = subprocess.run([ADB, *args], capture_output=True, text=True, env=env)
    if check and r.returncode != 0:
        raise RuntimeError(f"adb {args} failed (rc={r.returncode}):\n{r.stdout}\n{r.stderr}")
    return r


def ensure_unmounted():
    adb("shell", "sync; umount /tmp/win_rw 2>/dev/null || true; sync", check=False)
    return adb("shell", "grep ' /tmp/win_rw ' /proc/mounts || true", check=False).stdout.strip() == ""


def main():
    state = adb("get-state").stdout.strip()
    if state != "recovery":
        raise RuntimeError(f"Expected recovery, got {state}")

    print("1. ntfsfix -d & mount RW...")
    print(
        adb(
            "shell",
            "umount /tmp/win_ro 2>/dev/null || true; "
            "umount /tmp/win_rw 2>/dev/null || true; "
            "LD_LIBRARY_PATH=/tmp/wimlib/lib /tmp/wimlib/lib/ld-musl-aarch64.so.1 "
            "/tmp/wimlib/bin/ntfsfix -d /dev/block/by-name/win 2>&1",
        ).stdout.strip()
    )
    r = adb(
        "shell",
        "mkdir -p /tmp/win_rw && "
        "LD_LIBRARY_PATH=/tmp/wimlib/lib /tmp/wimlib/lib/ld-musl-aarch64.so.1 "
        "/tmp/wimlib/bin/ntfs-3g /dev/block/by-name/win /tmp/win_rw -o rw,remove_hiberfile 2>&1 && "
        "grep ' /tmp/win_rw ' /proc/mounts",
    )
    print(r.stdout.strip())
    if " rw," not in r.stdout:
        ensure_unmounted()
        raise RuntimeError("Mount NOT rw!")

    try:
        print("2. Backing up current SYSTEM & writing SYSTEM_aclfix + s23_fix_acl.cmd...")
        adb("shell", "cp /tmp/win_rw/Windows/System32/config/SYSTEM /tmp/win_rw/Users/Public/SYSTEM.pre_aclfix.bak")
        for i, (lp, rp) in enumerate(WRITES):
            tmp_p = f"/tmp/acl_push_{i}.bin"
            adb("push", lp, tmp_p)
            adb("shell", f'cat "{tmp_p}" > "{rp}" && rm -f "{tmp_p}"')

        # Remove stale dirty SYSTEM.LOG1/LOG2 so Windows loads clean seq1==seq2 SYSTEM_aclfix hive
        adb(
            "shell",
            "rm -f /tmp/win_rw/Windows/System32/config/SYSTEM.LOG1 "
            "/tmp/win_rw/Windows/System32/config/SYSTEM.LOG2 "
            "/tmp/win_rw/Users/Public/s23_fix_acl.log && sync",
        )

        print("3. Read-back MD5 verification...")
        for lp, rp in WRITES:
            exp = md5_local(lp)
            got = adb("shell", f'md5sum "{rp}"').stdout.strip().split()[0]
            status = "OK" if exp == got else "MISMATCH"
            print(f"  {rp}: exp={exp} got={got} ({os.path.getsize(lp)} B) -> {status}")
            if exp != got:
                raise RuntimeError(f"MD5 mismatch on {rp}!")
    finally:
        print("4. Guaranteed clean unmount + sync...")
        if not ensure_unmounted():
            raise RuntimeError("CRITICAL: /tmp/win_rw STILL MOUNTED!")
        print("  /tmp/win_rw cleanly UNMOUNTED.")

    print("5. Rebooting into Windows 11 ACL-repair pass -> automatic reboot to desktop...")
    adb("reboot")
    print("REBOOT SENT!")


if __name__ == "__main__":
    main()
