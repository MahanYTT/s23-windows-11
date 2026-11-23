#!/bin/bash
set -e
if ! python3 -c "import hivex" 2>/dev/null; then
  echo mahan | sudo -S apt-get install -y -qq python3-hivex >/dev/null 2>&1
fi
python3 - << 'PYEOF'
import hivex, uuid, shutil
src = "/mnt/c/Users/Mahan/Documents/windows-on-android/win11_arm64/clean_bcd"
dst = "/mnt/c/Users/Mahan/Documents/windows-on-android/s23_dm1q/deploy/BCD_s23_gpt"
shutil.copy2(src, dst)
s23_disk = uuid.UUID("98101B32-BBE2-4BF2-A06E-2BB33D000C2A").bytes_le
s23_esp  = uuid.UUID("11223344-5566-7788-99AA-BBCCDDEE0042").bytes_le
s23_win  = uuid.UUID("11223344-5566-7788-99AA-BBCCDDEE0043").bytes_le
def make_gpt_blob(part_guid_le, disk_guid_le):
    b = bytearray(88)
    b[16:20] = b"\x06\x00\x00\x00"  # Partition device
    b[24:28] = b"\x48\x00\x00\x00"  # 72 bytes
    b[32:48] = part_guid_le
    b[48:56] = b"\x00"*8            # GPT style = 0
    b[56:72] = disk_guid_le
    return bytes(b)
esp_blob = make_gpt_blob(s23_esp, s23_disk)
win_blob = make_gpt_blob(s23_win, s23_disk)
h = hivex.Hivex(dst, write=True)
root = h.root()
objs = h.node_get_child(root, "Objects")
for obj in h.node_children(objs):
    guid = h.node_name(obj)
    elems = h.node_get_child(obj, "Elements")
    if not elems: continue
    # Check description (12000004)
    desc_node = h.node_get_child(elems, "12000004")
    desc = ""
    if desc_node:
        _, val = h.node_get_value(desc_node, "Element")
        desc = val.decode("utf-16-le", errors="ignore").rstrip("\x00")
    print(f"Object {guid}: desc='{desc}'")
    for el in h.node_children(elems):
        ename = h.node_name(el)
        t, val = h.node_get_value(el, "Element")
        print(f"  Element {ename}: type={t} len={len(val)}")
        if ename in ("11000001", "21000001"):
            new_val = esp_blob if "Boot Manager" in desc else win_blob
            h.node_set_value(el, {"key": "Element", "t": t, "value": new_val})
            print(f"    -> Updated {ename} to {'ESP' if 'Boot Manager' in desc else 'WIN'} explicit 88B GPT locator!")
h.commit(dst)
print("BCD_s23_gpt committed cleanly via libhivex!")
PYEOF
