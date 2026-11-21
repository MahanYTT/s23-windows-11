
import re, subprocess
subprocess.run(["dtc", "-I", "dtb", "-O", "dts", "-o", "/tmp/s23_live.dts",
    "/mnt/c/Users/Mahan/Documents/windows-on-android/s23_dm1q/backups/live_fdt.dtb"],
    stderr=subprocess.DEVNULL, check=True)
txt = open("/tmp/s23_live.dts").read()
mm = re.search(r'\n\tmemory\s*\{([^}]+)\}', txt)
print("=== S23 memory ===")
if mm: print(mm.group(1).strip())
m = re.search(r'\n\treserved-memory\s*\{(.*?)\n\t\};', txt, re.S)
print("\n=== S23 reserved-memory (sorted) ===")
regs = []
for nm, blk in re.findall(r'([\w@\-,]+)\s*\{([^}]*)\}', m.group(1)):
    r = re.search(r'reg\s*=\s*<([^>]+)>', blk)
    if r:
        v = [int(x, 16) for x in r.group(1).split()]
        for j in range(0, len(v), 4):
            if j+3 < len(v):
                b = (v[j]<<32)|v[j+1]; s = (v[j+2]<<32)|v[j+3]
                regs.append((b, s, nm, "no-map" in blk))
for b, s, nm, no in sorted(regs):
    print(f"  0x{b:09x}..0x{b+s:09x}  sz=0x{s:08x}  {'NOMAP' if no else 'resv '}  {nm}")
