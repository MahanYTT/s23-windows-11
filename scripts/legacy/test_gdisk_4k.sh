#!/bin/bash
set -e
truncate -s $((5120 * 4096)) /tmp/test_sdb.img
dd if=/mnt/c/Users/Mahan/Documents/windows-on-android/s23_dm1q/backups/gpt_sdb.bin of=/tmp/test_sdb.img bs=4096 conv=notrunc 2>/dev/null
LOOP=$(losetup -b 4096 -f --show /tmp/test_sdb.img)
printf "r\nd\nw\ny\n" | gdisk "$LOOP" >/dev/null 2>&1 || true
printf "x\nj\n\nk\n\nv\nw\ny\n" | gdisk "$LOOP"
losetup -d "$LOOP"
python3 - << 'PYEOF'
import struct
g = open("/tmp/test_sdb.img", "rb").read()
my, alt, fu, lu = struct.unpack_from("<QQQQ", g, 0x1000+24)
pel, ne, es = struct.unpack_from("<QII", g, 0x1000+72)
b_off = 5119 * 4096
b_my, b_alt, b_fu, b_lu = struct.unpack_from("<QQQQ", g, b_off+24)
b_pel, b_ne, b_es = struct.unpack_from("<QII", g, b_off+72)
print(f"AFTER gdisk x->j->k->w:")
print(f"  PRI: my={my} alt={alt} usable={fu}..{lu} pel={pel} n_ents={ne}")
print(f"  BAK: my={b_my} alt={b_alt} usable={b_fu}..{b_lu} pel={b_pel} n_ents={b_ne}")
PYEOF
