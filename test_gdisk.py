
import os, struct, uuid, subprocess
bak = "/mnt/c/Users/Mahan/Documents/windows-on-android/s23_dm1q/backups"
for d in ("sda", "sdb", "sdc", "sdd", "sde", "sdf"):
    g = open(f"{bak}/gpt_{d}.bin", "rb").read()
    my, alt, fu, lu = struct.unpack_from("<QQQQ", g, 0x1000+24)
    dguid = uuid.UUID(bytes_le=g[0x1000+56:0x1000+72])
    pel, ne, es = struct.unpack_from("<QII", g, 0x1000+72)
    p1_s, p1_e = struct.unpack_from("<QQ", g, 0x2000+32)
    p1_n = g[0x2000+56:0x2000+128].decode("utf-16-le", errors="ignore").rstrip("\x00")
    # find last used partition
    last_i, last_s, last_e, last_n = 0, 0, 0, ""
    for i in range(ne):
        e = g[0x2000+i*128 : 0x2000+(i+1)*128]
        if e[:16] == b"\x00"*16: continue
        s, en = struct.unpack_from("<QQ", e, 32)
        last_i, last_s, last_e, last_n = i+1, s, en, e[56:128].decode("utf-16-le", errors="ignore").rstrip("\x00")
    print(f"{d}: alt={alt:8d} usable={fu}..{lu:8d} pel={pel} n_ents={ne:2d} | P1({p1_n})={p1_s}..{p1_e} | P{last_i}({last_n})={last_s}..{last_e}")
# Now test gdisk on a sparse image of sdb (5120 LBAs * 4096 = 20 MB) to see what gdisk 'v' and 'x -> j / k' actually do!
test_img = "/tmp/test_sdb.img"
with open(test_img, "wb") as f:
    f.truncate(5120 * 4096)
    g = open(f"{bak}/gpt_sdb.bin", "rb").read()
    f.seek(0)
    f.write(g)
r = subprocess.run(["gdisk", test_img], input="v\nq\n", capture_output=True, text=True)
print("\n=== gdisk 'v' on stock Samsung sdb GPT ===")
print(r.stdout)
