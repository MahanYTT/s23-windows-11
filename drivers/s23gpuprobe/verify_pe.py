"""PE32+ ARM64, /TSAWARE:NO, .reloc, CheckSum, Authenticode & phone ntoskrnl.exe import verifier."""
import hashlib
import os
import struct
import sys


def compute_pe_checksum(data: bytes) -> int:
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    csum_off = pe + 24 + 64
    buf = bytearray(data)
    struct.pack_into("<I", buf, csum_off, 0)
    n = len(buf)
    if n & 1:
        buf.append(0)
    s = 0
    for i in range(0, len(buf), 2):
        s += buf[i] | (buf[i + 1] << 8)
        s = (s & 0xFFFF) + (s >> 16)
    s = (s & 0xFFFF) + (s >> 16)
    return (s + n) & 0xFFFFFFFF


def rva_to_off(data: bytes, sections, rva: int) -> int:
    for va, vsize, raw_ptr, raw_sz in sections:
        span = max(vsize, raw_sz)
        if va <= rva < va + span:
            return raw_ptr + (rva - va)
    raise ValueError(f"Unmapped RVA 0x{rva:X}")


def parse_sections(data: bytes):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    n_sec = struct.unpack_from("<H", data, pe + 6)[0]
    opt_sz = struct.unpack_from("<H", data, pe + 20)[0]
    sec_off = pe + 24 + opt_sz
    secs = []
    for i in range(n_sec):
        off = sec_off + i * 40
        vsize, va, raw_sz, raw_ptr = struct.unpack_from("<4I", data, off + 8)
        secs.append((va, vsize, raw_ptr, raw_sz))
    return secs


def get_imports(data: bytes):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    imp_rva, imp_sz = struct.unpack_from("<2I", data, pe + 24 + 112 + 1 * 8)
    secs = parse_sections(data)
    out = {}
    if not imp_rva or not imp_sz:
        return out
    off = rva_to_off(data, secs, imp_rva)
    while True:
        orig_first, _, _, name_rva, first_thunk = struct.unpack_from("<5I", data, off)
        if not name_rva and not first_thunk:
            break
        n_off = rva_to_off(data, secs, name_rva)
        dll = data[n_off : data.index(b"\0", n_off)].decode("ascii").lower()
        thunk_rva = orig_first if orig_first else first_thunk
        t_off = rva_to_off(data, secs, thunk_rva)
        syms = []
        while True:
            val = struct.unpack_from("<Q", data, t_off)[0]
            if val == 0:
                break
            if val & (1 << 63):
                syms.append(f"ORD#{val & 0xFFFF}")
            else:
                hint_off = rva_to_off(data, secs, val & 0x7FFFFFFF)
                sym = data[hint_off + 2 : data.index(b"\0", hint_off + 2)].decode("ascii")
                syms.append(sym)
            t_off += 8
        out[dll] = syms
        off += 20
    return out


def get_exports(data: bytes):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    exp_rva, exp_sz = struct.unpack_from("<2I", data, pe + 24 + 112 + 0 * 8)
    secs = parse_sections(data)
    if not exp_rva or not exp_sz:
        return set()
    off = rva_to_off(data, secs, exp_rva)
    num_names = struct.unpack_from("<I", data, off + 24)[0]
    names_rva = struct.unpack_from("<I", data, off + 32)[0]
    n_off = rva_to_off(data, secs, names_rva)
    exports = set()
    for i in range(num_names):
        s_rva = struct.unpack_from("<I", data, n_off + i * 4)[0]
        s_off = rva_to_off(data, secs, s_rva)
        exports.add(data[s_off : data.index(b"\0", s_off)].decode("ascii"))
    return exports


def main():
    if len(sys.argv) < 2:
        print("Usage: verify_pe.py <driver.sys> [ntoskrnl.exe] [extra_dll.sys ...]")
        sys.exit(2)
    sys_path = sys.argv[1]
    nt_path = sys.argv[2] if len(sys.argv) >= 3 else None
    extra_refs = sys.argv[3:] if len(sys.argv) >= 4 else []

    data = open(sys_path, "rb").read()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    assert data[pe : pe + 4] == b"PE\0\0", "Invalid PE signature"
    machine = struct.unpack_from("<H", data, pe + 4)[0]
    magic = struct.unpack_from("<H", data, pe + 24)[0]
    stored_csum = struct.unpack_from("<I", data, pe + 24 + 64)[0]
    calc_csum = compute_pe_checksum(data)
    subsys = struct.unpack_from("<H", data, pe + 24 + 68)[0]
    dll_chars = struct.unpack_from("<H", data, pe + 24 + 70)[0]
    tsaware = bool(dll_chars & 0x8000)
    sec_off, sec_sz = struct.unpack_from("<2I", data, pe + 24 + 112 + 4 * 8)
    reloc_rva, reloc_sz = struct.unpack_from("<2I", data, pe + 24 + 112 + 5 * 8)

    md5 = hashlib.md5(data).hexdigest()
    print(f"FILE: {sys_path} ({len(data)} bytes, MD5={md5})")
    print(f"  Machine=0x{machine:04X} (ARM64=0xAA64), Magic=0x{magic:04X} (PE32+=0x020B)")
    print(f"  Subsystem={subsys} (NATIVE=1), DllChars=0x{dll_chars:04X}, TSAWARE={tsaware}")
    print(f"  Reloc=0x{reloc_rva:X}+{reloc_sz}, AuthSig=0x{sec_off:X}+{sec_sz}")
    print(f"  CheckSum stored=0x{stored_csum:08X} calc=0x{calc_csum:08X} match={stored_csum == calc_csum}")

    assert machine == 0xAA64, "Not ARM64"
    assert magic == 0x020B, "Not PE32+"
    assert subsys == 1, "Not NATIVE subsystem"
    assert not tsaware, "TSAWARE must be False for ARM64 WDM driver"
    assert reloc_sz > 0, "Missing .reloc section"
    assert sec_off > 0 and sec_sz > 0, "Missing Authenticode Security Data Directory (DD[4])"
    assert stored_csum == calc_csum, "PE CheckSum mismatch!"

    imports = get_imports(data)
    for dll, syms in imports.items():
        print(f"  Imports from {dll} ({len(syms)}): {', '.join(syms)}")

    ref_map = {}
    if nt_path and os.path.exists(nt_path):
        ref_map["ntoskrnl.exe"] = nt_path
    for rp in extra_refs:
        if os.path.exists(rp):
            bn = os.path.basename(rp).lower()
            if bn.endswith("_phone.sys"):
                bn = bn[:-10] + ".sys"
            ref_map[bn] = rp

    allowed = set(["ntoskrnl.exe"]) | set(ref_map.keys())
    unexpected = [d for d in imports.keys() if d not in allowed]
    assert not unexpected, f"Unexpected DLL imports: {unexpected}"

    for dll, syms in imports.items():
        ref_path = ref_map.get(dll)
        if ref_path and os.path.exists(ref_path):
            ref_data = open(ref_path, "rb").read()
            ref_pe = struct.unpack_from("<I", ref_data, 0x3C)[0]
            ref_mach = struct.unpack_from("<H", ref_data, ref_pe + 4)[0]
            exports = get_exports(ref_data)
            missing = [s for s in syms if s not in exports]
            print(
                f"  Ref {dll} check (Machine=0x{ref_mach:04X}, {len(exports)} exports): "
                f"{len(syms) - len(missing)}/{len(syms)} OK, missing={missing}"
            )
            assert ref_mach == 0xAA64, f"Reference {dll} is not ARM64"
            assert not missing, f"Unresolved {dll} imports: {missing}"

    print("PE VERIFICATION: PASS")


if __name__ == "__main__":
    main()
