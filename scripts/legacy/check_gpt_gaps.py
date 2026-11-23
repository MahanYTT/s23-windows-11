
import subprocess, os, struct
bak = "/mnt/c/Users/Mahan/Documents/windows-on-android/s23_dm1q/backups"
# Create a sparse 128MB or exact-size sparse file for sdb..sdf and sda in /tmp and run gdisk 'v' on each!
for d in ("sda", "sdb", "sdc", "sdd", "sde", "sdf"):
    g = open(f"{bak}/gpt_{d}.bin", "rb").read()
    my, alt, fu, lu, pel, ne, es = struct.unpack_from("<QQQQQII", g, 0x1000+24)
    # Primary entries end at pel + (ne*es)//4096 - 1 = 2 + (ne*128)//4096 - 1
    pri_end = pel + (ne * es + 4095) // 4096 - 1
    bak_need = (ne * es + 4095) // 4096
    bak_start = alt - bak_need
    print(f"{d}: my={my} alt={alt} n_ents={ne} ({bak_need} LBAs) | pri_ents={pel}..{pri_end} vs FirstUsable={fu} (gap={fu-pri_end-1}) | LastUsable={lu} vs bak_ents={bak_start}..{alt-1} (gap={bak_start-lu-1})")
