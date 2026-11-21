#!/bin/bash
set -e
cd ~/mu_s23/Mu-Silicium
sed -i 's/{"DXE Heap",           0xBAB00000, 0x0D1C0000/{"DXE Heap",           0xBAE00000, 0x04500000/' Platforms/Samsung/dm1qPkg/Library/MemoryMapLib/MemoryMapLib.c
source ~/mu_s23/venv/bin/activate
python3 build_uefi.py -d dm1q -r RELEASE
cp Mu-dm1q.img /mnt/c/Users/Mahan/Documents/windows-on-android/s23_dm1q/Mu-dm1q-v4.img
ls -lh /mnt/c/Users/Mahan/Documents/windows-on-android/s23_dm1q/Mu-dm1q-v4.img
