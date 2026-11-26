# Native Linux Installation & Development Guide (CachyOS / Arch / Debian / Fedora)

You do **not** need a Windows host PC or Windows VM to partition the S23, pull device firmware, apply Windows 11 ARM64, configure the EFI boot chain (`BCD` / `SanPolicy=4`), flash UEFI to `sda25`, or control live Windows over USB-C (`s23link.py`).

Only **compiling** the MSVC/WDK drivers (`s23gpu`, `s23audio`, `s23modem`, `s23mbb`) from source requires Windows WDK — and [`.github/workflows/build-drivers.yml`](../.github/workflows/build-drivers.yml) builds and test-signs all of them automatically on GitHub Actions so Linux users can download the `s23-drivers-arm64` artifact directly (or fork/trigger the workflow via `gh workflow run build-drivers.yml`).

---

## 1. Host Packages

Check what is already installed:

```bash
python3 scripts/linux/install_s23_linux.py check-deps
```

**CachyOS / Arch:**

```bash
sudo pacman -S --needed android-tools wimlib ntfs-3g dosfstools gptfdisk python-hivex libusb p7zip clang lld llvm acpica openssl
```

*(Optional AUR package `osslsigncode` if you want to Authenticode-sign self-compiled Clang `.sys` drivers locally on Linux.)*

**Debian / Ubuntu:**

```bash
sudo apt-get install -y android-tools-adb wimtools ntfs-3g dosfstools gdisk python3-hivex libusb-1.0-0 p7zip-full clang-18 lld-18 llvm-18 acpica-tools osslsigncode openssl
```

**Fedora:**

```bash
sudo dnf install -y android-tools wimlib-utils ntfs-3g dosfstools gdisk python3-hivex libusb1 p7zip-plugins clang lld llvm acpica-tools osslsigncode openssl
```

Install the `udev` rule so `s23link.py` works without `sudo` when the S23 is running Windows:

```bash
sudo cp scripts/linux/99-s23-winusb.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules && sudo udevadm trigger
```

---

## 2. Getting Drivers (Zero-Windows Option vs. Local Clang Build)

### Option A: Cloud-built ARM64 bundle (all drivers, no Windows PC needed)
Trigger or download the `s23-drivers-arm64` artifact from the **`Build ARM64 Drivers`** GitHub Actions workflow ([`build-drivers.yml`](../.github/workflows/build-drivers.yml)):

```bash
gh workflow run build-drivers.yml
# wait for completion, then:
gh run download --name s23-drivers-arm64 --dir ./dist_s23_drivers
```

This contains all signed `.sys` drivers (`s23touch`, `s23batt`, `s23keys`, `s23bthx`, `s23perf`, `s23audio`, `s23mbb`, `s23modem_xprt`, `s23gpu`), `s23modemd.exe`, `s23sms.ps1`, `s23perfctl.ps1`, `s23test.cer`, and `s23_firstboot.ps1`.

### Option B: Build & sign the Clang drivers locally on Linux

```bash
# Build + __chkstk/.reloc verify + osslsigncode sign into drivers/*/pkg/:
python3 scripts/linux/build_and_sign_linux.py

# Or build + verify only (no osslsigncode needed):
python3 scripts/linux/build_and_sign_linux.py --no-sign
```

Individual drivers also support distro-agnostic invocation:

```bash
python3 drivers/s23touch/build_wsl.py
```

ACPI SSDTs build directly on Linux:

```bash
iasl -tc gpu_research/s23_gpu_bringup/s23_gpu_acpi.asl
```

---

## 3. Installing Windows 11 ARM64 onto the S23 (from Lineage Recovery)

> [!CAUTION]
> Never write to `sdb` or `sde` — they are power-on write-protected bootloader LUNs. `install_s23_linux.py` blocks `sdb`/`sde` targets and restricts boot flashing strictly to `sda25` (`/dev/block/by-name/boot`).

Boot the S23 into **Lineage Recovery** (`adb devices` shows `recovery`), then run:

### Step 1: Backup stock partitions & GPT

```bash
python3 scripts/linux/install_s23_linux.py backup --out ./backups
```

This saves `boot_android.img` (`sda25`), `recovery.img` (`sda27`), and `sda_gpt.bin`.

### Step 2: Pull your own phone's firmware (strictly `-o ro`)

```bash
python3 scripts/linux/install_s23_linux.py pull-fw
```

Extracts OEM-signed GPU, modem/5G/DSP, audio/haptics, and Wi-Fi/BT firmware into their gitignored `s23_vendor_*_fw/` directories so TrustZone rollback fuses match your exact device.

### Step 3: Partition `sda` (`userdata` -> `esp` + `win`)

Back up Android `/data` first! Shrinking `sda41` (`userdata`) creates:
- `sda42` (`esp`, 512 MiB, FAT32, GUID `11223344-5566-7788-99AA-BBCCDDEE0042`)
- `sda43` (`win`, NTFS, GUID `11223344-5566-7788-99AA-BBCCDDEE0043`)

```bash
python3 scripts/linux/install_s23_linux.py partition --win-size-gb 64 --confirm-repartition
```

### Step 4: Expose `esp` + `win` over USB Mass Storage & deploy Windows 11 ARM64

```bash
python3 scripts/linux/install_s23_linux.py enable-msc
# Inspect newly attached /dev/sdX1 (esp) and /dev/sdX2 (win) via `lsblk -o NAME,SIZE,FSTYPE,LABEL`
sudo mkdir -p /mnt/s23_esp
sudo mount /dev/sdX1 /mnt/s23_esp

sudo python3 scripts/linux/install_s23_linux.py deploy-windows \
  --iso /path/to/Win11_ARM64.iso \
  --win-target /dev/sdX2 \
  --esp-dir /mnt/s23_esp \
  --drivers-dir ./dist_s23_drivers

sync
sudo umount /mnt/s23_esp
python3 scripts/linux/install_s23_linux.py disable-msc
```

`deploy-windows` applies the Windows 11 ARM64 WIM via `wimlib-imagex`, sets `SanPolicy = 4` in `SYSTEM` via `python-hivex`, writes explicit 88-byte GPT partition locators, `testsigning = Yes`, `IgnoreAllFailures`, and the 51 MiB MPSS DSM `badmemorylist` (`0xd4d00..0xd7fff`) into `BCD`, and stages drivers/firmware + `SetupComplete.cmd` (`s23_firstboot.ps1`).

### Step 5: Flash Mu-Silicium UEFI boot image to `sda25` (`boot`)

```bash
python3 scripts/linux/install_s23_linux.py flash-boot --image /path/to/Mu-dm1q.img
adb reboot
```

To boot back into Android / Evolution X later from Lineage Recovery:

```bash
python3 scripts/linux/install_s23_linux.py restore-android --image ./backups/boot_android.img
adb reboot
```

---

## 4. Controlling Live Windows from Linux (`s23link.py`)

Once Windows is running on the S23 and USB-C PC-Link is active, `s23link.py` automatically uses `libusb-1.0` (`VID 1209`, `PID 0001..0004`) on Linux:

```bash
python3 s23link.py ping
python3 s23link.py exec "ver"
python3 s23link.py put ./local_file C:\\s23_drivers\\local_file
```
