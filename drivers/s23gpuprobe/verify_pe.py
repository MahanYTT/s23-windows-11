"""PE32+ ARM64, /TSAWARE:NO, .reloc, CheckSum, Authenticode, __chkstk/stack-frame & phone ntoskrnl.exe import verifier.

Usage: verify_pe.py <driver.sys> [ntoskrnl.exe] [extra_dll.sys ...] [--frame-warn BYTES] [--max-frame BYTES]

Exit code 0 = PASS, 1 = FAIL, 2 = usage error.

Stack checks (the ARM64 kernel stack is only 24 KB, an overflow is bugcheck 0x50/0x7F):
  * FAIL if the driver imports __chkstk or contains the ARM64 __chkstk call sequence
    (`bl __chkstk` is always followed by `sub sp, sp, x15, uxtx #4`), i.e. some function has a frame
    larger than one page.  MSVC links __chkstk statically from libcntpr.lib, so the import table alone
    is not enough.
  * WARN for every function prologue that reserves more than --frame-warn bytes (default 1024) with
    `sub sp, sp, #imm` (this also catches clang builds that use -mno-stack-arg-probe).
  * FAIL instead of WARN when --max-frame is given and a frame exceeds it.
"""
import hashlib
import os
import struct
import sys

IMAGE_SCN_MEM_EXECUTE = 0x20000000
# `sub sp, sp, x15, uxtx #4`: emitted right after every `bl __chkstk` by MSVC and clang for ARM64 Windows.
ARM64_SUB_SP_X15_UXTX4 = 0xCB2F73FF
# `sub sp, sp, #imm{, lsl #12}` (64-bit SUB immediate, Rd = Rn = SP): (word & MASK) == VALUE
ARM64_SUB_SP_IMM_MASK = 0xFF8003FF
ARM64_SUB_SP_IMM_VALUE = 0xD10003FF
DEFAULT_FRAME_WARN = 1024


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


def parse_section_headers(data: bytes):
    """-> [(name, va, vsize, raw_ptr, raw_sz, characteristics)]"""
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    n_sec = struct.unpack_from("<H", data, pe + 6)[0]
    opt_sz = struct.unpack_from("<H", data, pe + 20)[0]
    sec_off = pe + 24 + opt_sz
    out = []
    for i in range(n_sec):
        off = sec_off + i * 40
        name = data[off : off + 8].rstrip(b"\0").decode("ascii", "replace")
        vsize, va, raw_sz, raw_ptr = struct.unpack_from("<4I", data, off + 8)
        chars = struct.unpack_from("<I", data, off + 36)[0]
        out.append((name, va, vsize, raw_ptr, raw_sz, chars))
    return out


def iter_code_words(data: bytes):
    """yield (rva, insn) for every 4-byte aligned word of the executable sections' file data"""
    for _name, va, vsize, raw_ptr, raw_sz, chars in parse_section_headers(data):
        if not chars & IMAGE_SCN_MEM_EXECUTE:
            continue
        n = min(vsize or raw_sz, raw_sz, max(0, len(data) - raw_ptr)) & ~3
        for i in range(0, n, 4):
            yield va + i, struct.unpack_from("<I", data, raw_ptr + i)[0]


def find_chkstk_sites(data: bytes):
    """RVAs of `sub sp, sp, x15, uxtx #4` (the instruction that follows every `bl __chkstk`)"""
    return [rva for rva, w in iter_code_words(data) if w == ARM64_SUB_SP_X15_UXTX4]


def find_stack_frames(data: bytes):
    """[(rva, bytes)] for every run of consecutive `sub sp, sp, #imm{, lsl #12}` instructions"""
    frames = []
    run_rva, run_size, prev_rva = None, 0, None
    for rva, w in iter_code_words(data):
        if (w & ARM64_SUB_SP_IMM_MASK) == ARM64_SUB_SP_IMM_VALUE:
            imm = (w >> 10) & 0xFFF
            size = imm << 12 if (w >> 22) & 1 else imm
            if run_rva is not None and prev_rva == rva - 4:
                run_size += size
            else:
                if run_rva is not None:
                    frames.append((run_rva, run_size))
                run_rva, run_size = rva, size
            prev_rva = rva
    if run_rva is not None:
        frames.append((run_rva, run_size))
    return frames


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


def parse_args(argv):
    """-> (positional args, frame_warn, max_frame); raises ValueError on bad options"""
    pos, frame_warn, max_frame = [], DEFAULT_FRAME_WARN, None
    i = 0
    while i < len(argv):
        a = argv[i]
        if a in ("--frame-warn", "--max-frame"):
            if i + 1 >= len(argv) or not argv[i + 1].isdigit():
                raise ValueError(f"{a} needs a byte count")
            if a == "--frame-warn":
                frame_warn = int(argv[i + 1])
            else:
                max_frame = int(argv[i + 1])
            i += 2
            continue
        pos.append(a)
        i += 1
    if not pos:
        raise ValueError("missing <driver.sys>")
    return pos, frame_warn, max_frame


def verify(sys_path, nt_path=None, extra_refs=(), frame_warn=DEFAULT_FRAME_WARN, max_frame=None):
    """print the report; -> list of failure messages (empty = PASS)"""
    fails = []

    def check(cond, msg):
        if not cond:
            fails.append(msg)
            print(f"  FAIL: {msg}")
        return cond

    with open(sys_path, "rb") as f:
        data = f.read()
    if not check(len(data) >= 0x40 and data[:2] == b"MZ", "Not an MZ/PE file"):
        return fails
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if not check(pe + 24 + 112 + 16 * 8 <= len(data) and data[pe : pe + 4] == b"PE\0\0", "Invalid PE signature"):
        return fails
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

    check(machine == 0xAA64, "Not ARM64")
    check(magic == 0x020B, "Not PE32+")
    check(subsys == 1, "Not NATIVE subsystem")
    check(not tsaware, "TSAWARE must be False for ARM64 WDM driver")
    check(reloc_sz > 0, "Missing .reloc section")
    check(sec_off > 0 and sec_sz > 0, "Missing Authenticode Security Data Directory (DD[4])")
    check(stored_csum == calc_csum, "PE CheckSum mismatch!")

    imports = get_imports(data)
    for dll, syms in imports.items():
        print(f"  Imports from {dll} ({len(syms)}): {', '.join(syms)}")

    # ---- kernel stack safety
    chk_imports = [f"{dll}!{s}" for dll, syms in imports.items() for s in syms if "chkstk" in s.lower()]
    check(not chk_imports, f"__chkstk imported ({', '.join(chk_imports)}): a stack frame exceeds one page")
    sites = find_chkstk_sites(data) if machine == 0xAA64 else []
    print(f"  __chkstk call sites: {len(sites)}")
    check(not sites, "__chkstk call sequence (sub sp, sp, x15, uxtx #4) at RVA "
          + ", ".join(f"0x{r:X}" for r in sites[:8]) + (" ..." if len(sites) > 8 else "")
          + ": move large locals off the 24 KB ARM64 kernel stack")
    frames = sorted(find_stack_frames(data), key=lambda t: -t[1]) if machine == 0xAA64 else []
    if frames:
        print(f"  Largest stack frame: {frames[0][1]} bytes at RVA 0x{frames[0][0]:X} ({len(frames)} frames scanned)")
    for rva, size in frames:
        if max_frame is not None and size > max_frame:
            check(False, f"stack frame of {size} bytes at RVA 0x{rva:X} exceeds --max-frame {max_frame}")
        elif size > frame_warn:
            print(f"  WARN: stack frame of {size} bytes at RVA 0x{rva:X} (> {frame_warn})")

    # ---- imports against the phone's own binaries
    ref_map = {}
    declared_refs = set()
    if nt_path:
        if os.path.exists(nt_path):
            ref_map["ntoskrnl.exe"] = nt_path
        else:
            print(f"  NOTE: reference {nt_path} not found, ntoskrnl.exe imports not resolved")
    for rp in extra_refs:
        bn = os.path.basename(rp).lower()
        if bn.endswith("_phone.sys"):
            bn = bn[:-10] + ".sys"
        declared_refs.add(bn)
        if os.path.exists(rp):
            ref_map[bn] = rp
        else:
            print(f"  NOTE: reference {rp} not found")

    allowed = set(["ntoskrnl.exe"]) | set(ref_map.keys()) | declared_refs
    unexpected = [d for d in imports.keys() if d not in allowed]
    check(not unexpected, f"Unexpected DLL imports: {unexpected}")

    for dll, syms in imports.items():
        ref_path = ref_map.get(dll)
        if ref_path and os.path.exists(ref_path):
            with open(ref_path, "rb") as f:
                ref_data = f.read()
            ref_pe = struct.unpack_from("<I", ref_data, 0x3C)[0]
            ref_mach = struct.unpack_from("<H", ref_data, ref_pe + 4)[0]
            exports = get_exports(ref_data)
            missing = [s for s in syms if s not in exports]
            print(
                f"  Ref {dll} check (Machine=0x{ref_mach:04X}, {len(exports)} exports): "
                f"{len(syms) - len(missing)}/{len(syms)} OK, missing={missing}"
            )
            check(ref_mach == 0xAA64, f"Reference {dll} is not ARM64")
            check(not missing, f"Unresolved {dll} imports: {missing}")
    return fails


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    try:
        pos, frame_warn, max_frame = parse_args(argv)
    except ValueError as e:
        print(f"error: {e}")
        print("Usage: verify_pe.py <driver.sys> [ntoskrnl.exe] [extra_dll.sys ...] [--frame-warn BYTES] [--max-frame BYTES]")
        return 2
    sys_path = pos[0]
    nt_path = pos[1] if len(pos) >= 2 else None
    extra_refs = pos[2:]
    try:
        fails = verify(sys_path, nt_path, extra_refs, frame_warn, max_frame)
    except (OSError, struct.error, ValueError, UnicodeDecodeError) as e:
        fails = [f"cannot parse {sys_path}: {e}"]
        print(f"  FAIL: {fails[0]}")
    if fails:
        print(f"PE VERIFICATION: FAIL ({len(fails)} problem{'s' if len(fails) != 1 else ''})")
        return 1
    print("PE VERIFICATION: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
