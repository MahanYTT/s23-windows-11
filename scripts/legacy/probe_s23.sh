#!/bin/sh
# Samsung Galaxy S23 (dm1q / SM8550) Read-Only Probe Script (for bare-metal Alpine Linux)
set -u
OUT="${1:-/tmp/s23_probe}"
mkdir -p "$OUT"
echo "=== 1. System & Kernel ===" | tee "$OUT/summary.txt"
uname -a | tee -a "$OUT/summary.txt"
cat /etc/os-release 2>/dev/null | tee -a "$OUT/summary.txt"
cat /proc/cmdline > "$OUT/cmdline.txt" 2>/dev/null || true
echo "CMDLINE: $(cat "$OUT/cmdline.txt")" | tee -a "$OUT/summary.txt"
cat /proc/device-tree/model 2>/dev/null | tr '\0' '\n' | tee -a "$OUT/summary.txt"
cat /proc/device-tree/compatible 2>/dev/null | tr '\0' ' ' | tee -a "$OUT/summary.txt"
echo "" >> "$OUT/summary.txt"
echo "RAM:" >> "$OUT/summary.txt"
free -h | tee -a "$OUT/summary.txt"
echo "=== 2. Storage & UFS LUNs ===" | tee -a "$OUT/summary.txt"
lsblk -o NAME,MAJ:MIN,SIZE,TYPE,FSTYPE,MOUNTPOINT,PARTLABEL,PARTTYPE > "$OUT/lsblk.txt" 2>&1 || lsblk > "$OUT/lsblk.txt" 2>&1
cat "$OUT/lsblk.txt" | tee -a "$OUT/summary.txt"
ls -la /dev/disk/by-partlabel/ /dev/block/by-name/ > "$OUT/by_name.txt" 2>&1 || true
for d in /dev/sda /dev/sdb /dev/sdc /dev/sdd /dev/sde /dev/sdf; do
  [ -b "$d" ] || continue
  bn=$(basename "$d")
  echo "--- $d ($(blockdev --getsize64 "$d" 2>/dev/null) bytes, sec=$(blockdev --getss "$d" 2>/dev/null)) ---" >> "$OUT/gpt_info.txt"
  dd if="$d" of="$OUT/gpt_${bn}.bin" bs=4096 count=8 2>/dev/null || true
  sgdisk -p "$d" >> "$OUT/gpt_info.txt" 2>&1 || fdisk -l "$d" >> "$OUT/gpt_info.txt" 2>&1 || true
done
echo "=== 3. Live DTB, Display, PCIe, I2C/SPI Touch, USB, Gunyah/KVM ===" | tee -a "$OUT/summary.txt"
[ -r /sys/firmware/fdt ] && cp /sys/firmware/fdt "$OUT/live_fdt.dtb" 2>/dev/null || true
lspci -nnvv > "$OUT/lspci.txt" 2>&1 || true
lsusb > "$OUT/lsusb.txt" 2>&1 || true
ls -la /dev/kvm /dev/gunyah /dev/dri /dev/fb* /dev/input/event* > "$OUT/devs.txt" 2>&1 || true
cat /proc/bus/input/devices > "$OUT/input_devices.txt" 2>&1 || true
dmesg > "$OUT/dmesg.txt" 2>&1 || true
cat /proc/iomem > "$OUT/iomem.txt" 2>&1 || true
find /sys/class/drm /sys/class/graphics -maxdepth 3 -ls > "$OUT/drm_fb.txt" 2>&1 || true
echo "=== 4. Current Bootloader / Mounts ===" | tee -a "$OUT/summary.txt"
mount > "$OUT/mounts.txt" 2>&1 || true
df -h > "$OUT/df.txt" 2>&1 || true
tar -czf /tmp/s23_probe.tar.gz -C "$(dirname "$OUT")" "$(basename "$OUT")"
echo "DONE! Bundle saved at /tmp/s23_probe.tar.gz ($(ls -lh /tmp/s23_probe.tar.gz | awk '{print $5}'))"
