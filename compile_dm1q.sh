#!/bin/bash
set -e
cd ~/mu_s23/Mu-Silicium
sed -i 's/NoMap, /AddMem,/g' Platforms/Samsung/dm1qPkg/Library/MemoryMapLib/MemoryMapLib.c
source ~/mu_s23/venv/bin/activate
python3 build_uefi.py -d dm1q -r RELEASE
ls -lh Mu-dm1q.img
cp Mu-dm1q.img /mnt/c/Users/Mahan/Documents/windows-on-android/s23_dm1q/Mu-dm1q.img
echo "=== MU-DM1Q.IMG BUILT SUCCESSFULLY! ==="
