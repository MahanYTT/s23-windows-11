#!/usr/bin/env python3
"""Offline integrity check of a local s23_dm1q_firmware_dump/ directory (read-only, no device access).

  python scripts/verify_firmware_dump.py DUMP_DIR [--sums FILE] [--write-sums FILE] [--images-only] [--include-audio]

1. File hashes: compares every file against a sha256sum-style list (default DUMP_DIR/SHA256SUMS.txt:
   "<sha256>  <relative/path>"). Reports mismatches, missing files and files not in the list.
   --write-sums FILE creates such a list from the current tree instead (FILE must be outside the repo
   or ignored; the dump itself must never be committed).
2. Firmware images: every *.mdt (split PIL image + .bNN segments), *.elf and *.mbn under the GPU and
   modem/DSP folders is parsed as ELF32/ELF64:
     - ELF header / program-header table bounds,
     - each segment present with the size given by p_filesz (split images: DUMP/foo.bNN),
     - Qualcomm hash segment (p_flags bits 24..26 == 2): the SHA-256 or SHA-384 digest of every
       non-empty segment (segment 0 = ELF header + program headers) must appear in it,
     - loadable span (PT_LOAD, non-hash) for the ground-truth report.
   This checks internal consistency only; it does not verify signatures. Files that are not ELF
   (e.g. encrypted containers) are reported as SKIP and only covered by the SHA256 list.
   03_audio_adsp_haptics/ is skipped unless --include-audio is given.

Exit code: 0 = everything consistent, 1 = at least one problem, 2 = usage error.
"""
import hashlib
import json
import os
import struct
import sys

PT_LOAD = 1
SEG_TYPE_HASH = 2
IMAGE_DIRS_DEFAULT_SKIP = ("03_audio_adsp_haptics",)
DIGESTS = ("sha256", "sha384")


class ElfError(ValueError):
    pass


def parse_elf(data):
    """-> dict(cls, machine, entry, flags, phdrs=[dict(type, offset, paddr, filesz, memsz, flags)])"""
    if len(data) < 52 or data[:4] != b"\x7fELF":
        raise ElfError("not an ELF file")
    cls, endian = data[4], data[5]
    if endian != 1:
        raise ElfError("big-endian ELF not supported")
    if cls == 1:
        _, machine, _, entry, phoff, _, flags, _, phentsize, phnum = struct.unpack_from("<HHIIIIIHHH", data, 16)
        want = 32
    elif cls == 2:
        if len(data) < 64:
            raise ElfError("truncated ELF64 header")
        _, machine, _, entry, phoff, _, flags, _, phentsize, phnum = struct.unpack_from("<HHIQQQIHHH", data, 16)
        want = 56
    else:
        raise ElfError(f"unknown ELF class {cls}")
    if phnum and phentsize < want:
        raise ElfError(f"program header entry size {phentsize} < {want}")
    if phoff + phnum * phentsize > len(data):
        raise ElfError("program header table extends past the end of the file")
    phdrs = []
    for i in range(phnum):
        o = phoff + i * phentsize
        if cls == 1:
            p_type, p_offset, _, p_paddr, p_filesz, p_memsz, p_flags, _ = struct.unpack_from("<8I", data, o)
        else:
            p_type, p_flags, p_offset, _, p_paddr, p_filesz, p_memsz, _ = struct.unpack_from("<IIQQQQQQ", data, o)
        phdrs.append(dict(type=p_type, offset=p_offset, paddr=p_paddr, filesz=p_filesz, memsz=p_memsz, flags=p_flags))
    return dict(cls=cls, machine=machine, entry=entry, flags=flags, phoff=phoff, phentsize=phentsize, phdrs=phdrs)


def seg_type(phdr):
    return (phdr["flags"] >> 24) & 7


def headers_size(elf):
    return elf["phoff"] + len(elf["phdrs"]) * elf["phentsize"]


def load_segments(path):
    """-> (elf, [bytes or None per segment], problems). Split images (*.mdt) take segment i from
    <base>.bNN when present, otherwise from the .mdt itself (header and hash segments live there)."""
    with open(path, "rb") as f:
        data = f.read()
    elf = parse_elf(data)
    split = path.lower().endswith(".mdt")
    base = path[:-4]
    segs, problems = [], []
    for i, p in enumerate(elf["phdrs"]):
        if p["filesz"] == 0:
            segs.append(b"")
            continue
        part = f"{base}.b{i:02d}"
        if split and os.path.exists(part):
            with open(part, "rb") as f:
                blob = f.read()
        elif p["offset"] + p["filesz"] <= len(data):
            blob = data[p["offset"]:p["offset"] + p["filesz"]]
        else:
            segs.append(None)
            problems.append(f"segment {i}: missing ({os.path.basename(part) if split else 'beyond end of file'})")
            continue
        if len(blob) != p["filesz"]:
            problems.append(f"segment {i}: {len(blob)} bytes, p_filesz says {p['filesz']}")
        segs.append(blob)
    return elf, segs, problems, data


def check_image(path):
    """-> report dict for one ELF/MDT image (see module docstring)."""
    rep = dict(path=path, ok=True, problems=[])
    try:
        with open(path, "rb") as f:
            magic = f.read(4)
    except OSError as e:
        rep.update(ok=False, problems=[str(e)])
        return rep
    if magic != b"ELF":
        # e.g. encrypted TZ application containers; their integrity is covered by the SHA256 list
        rep.update(skipped=f"not an ELF image (magic {magic.hex()})")
        return rep
    try:
        elf, segs, problems, raw = load_segments(path)
    except (OSError, ElfError, struct.error) as e:
        rep.update(ok=False, problems=[str(e)])
        return rep
    rep["problems"] += problems
    rep.update(elf_class=32 if elf["cls"] == 1 else 64, machine=elf["machine"], entry=elf["entry"],
               segments=len(elf["phdrs"]))
    loads = [p for p in elf["phdrs"] if p["type"] == PT_LOAD and p["memsz"] and seg_type(p) != SEG_TYPE_HASH]
    if loads:
        lo = min(p["paddr"] for p in loads)
        hi = max(p["paddr"] + p["memsz"] for p in loads)
        rep.update(load_lo=lo, load_hi=hi, load_span=hi - lo)
    hidx = [i for i, p in enumerate(elf["phdrs"]) if seg_type(p) == SEG_TYPE_HASH]
    if hidx and segs[hidx[0]]:
        hseg = segs[hidx[0]]
        verified, failed, algo = 0, [], None
        for i, p in enumerate(elf["phdrs"]):
            if i == hidx[0] or p["filesz"] == 0 or segs[i] is None:
                continue
            blob = raw[:headers_size(elf)] if i == 0 and p["type"] != PT_LOAD else segs[i]
            hit = next((a for a in DIGESTS if hashlib.new(a, blob).digest() in hseg), None)
            if hit:
                verified += 1
                algo = hit
            else:
                failed.append(i)
        rep.update(hash_algo=algo, hash_verified=verified, hash_failed=failed)
        if failed:
            rep["problems"].append(f"segment hash mismatch: {failed}")
    else:
        rep["hash_verified"] = None   # no hash segment (e.g. plain ELF)
    rep["ok"] = not rep["problems"]
    return rep


def iter_files(root):
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        for fn in sorted(filenames):
            full = os.path.join(dirpath, fn)
            yield os.path.relpath(full, root).replace(os.sep, "/"), full


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def parse_sums(text):
    """sha256sum format -> {relpath: hexdigest}; '*' binary marker and './' prefix are accepted."""
    out = {}
    for n, line in enumerate(text.splitlines(), 1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split(None, 1)
        if len(parts) != 2 or len(parts[0]) != 64:
            raise ValueError(f"line {n}: not '<sha256>  <path>'")
        name = parts[1].lstrip("*")
        if name.startswith("./"):
            name = name[2:]
        out[name.replace("\\", "/")] = parts[0].lower()
    return out


def check_sums(root, sums_path):
    """-> dict(mismatch=[...], missing=[...], extra=[...], ok=int)"""
    with open(sums_path, encoding="utf-8") as f:
        expected = parse_sums(f.read())
    sums_rel = os.path.relpath(os.path.abspath(sums_path), os.path.abspath(root)).replace(os.sep, "/")
    actual = {rel: full for rel, full in iter_files(root) if rel != sums_rel}
    res = dict(mismatch=[], missing=[], extra=sorted(set(actual) - set(expected)), ok=0)
    for rel, digest in sorted(expected.items()):
        if rel not in actual:
            res["missing"].append(rel)
        elif sha256_file(actual[rel]) != digest:
            res["mismatch"].append(rel)
        else:
            res["ok"] += 1
    return res


def write_sums(root, out_path):
    out_abs = os.path.abspath(out_path)
    lines = [f"{sha256_file(full)}  {rel}" for rel, full in iter_files(root) if os.path.abspath(full) != out_abs]
    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    return len(lines)


def find_images(root, include_audio=False):
    for rel, full in iter_files(root):
        top = rel.split("/", 1)[0]
        if not include_audio and top in IMAGE_DIRS_DEFAULT_SKIP:
            continue
        if rel.lower().endswith((".mdt", ".elf", ".mbn")):
            yield rel, full


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    opts = {"--sums": None, "--write-sums": None}
    flags = {"--images-only": False, "--include-audio": False, "--json": False}
    pos = []
    while argv:
        a = argv.pop(0)
        if a in opts:
            if not argv:
                print(f"error: {a} needs a file name")
                return 2
            opts[a] = argv.pop(0)
        elif a in flags:
            flags[a] = True
        elif a.startswith("-"):
            print(__doc__)
            return 2
        else:
            pos.append(a)
    if len(pos) != 1 or not os.path.isdir(pos[0]):
        print(__doc__)
        return 2
    root = pos[0]
    if opts["--write-sums"]:
        n = write_sums(root, opts["--write-sums"])
        print(f"wrote {n} hashes to {opts['--write-sums']}")
        return 0

    failed = False
    report = {"images": []}
    if not flags["--images-only"]:
        sums = opts["--sums"] or os.path.join(root, "SHA256SUMS.txt")
        if not os.path.exists(sums):
            print(f"SHA256 list: {sums} not found (create one with --write-sums, outside the repo)")
            failed = True
        else:
            r = check_sums(root, sums)
            report["sums"] = r
            print(f"SHA256 list: {r['ok']} ok, {len(r['mismatch'])} mismatch, {len(r['missing'])} missing, "
                  f"{len(r['extra'])} not listed")
            for k in ("mismatch", "missing"):
                for p in r[k]:
                    print(f"  {k.upper()}: {p}")
            failed |= bool(r["mismatch"] or r["missing"])
    for rel, full in find_images(root, flags["--include-audio"]):
        rep = check_image(full)
        rep["path"] = rel
        report["images"].append(rep)
        span = (f" load 0x{rep['load_lo']:08X}..0x{rep['load_hi']:08X} ({rep['load_span']:#x})"
                if "load_lo" in rep else "")
        hv = rep.get("hash_verified")
        hashes = "" if hv is None else f" hashes {hv} ok/{len(rep.get('hash_failed', []))} bad ({rep.get('hash_algo')})"
        if rep.get("skipped"):
            print(f"SKIP {rel}: {rep['skipped']}")
            continue
        print(f"{'OK  ' if rep['ok'] else 'FAIL'} {rel}: ELF{rep.get('elf_class', '?')} "
              f"{rep.get('segments', '?')} segs{span}{hashes}")
        for p in rep["problems"]:
            print(f"       {p}")
        failed |= not rep["ok"]
    if flags["--json"]:
        print(json.dumps(report, indent=1, default=str))
    print("FIRMWARE DUMP: " + ("FAIL" if failed else "OK"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
