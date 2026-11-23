"""Builds tiny synthetic PE32+ images for the verify_pe.py tests (no real driver binaries needed)."""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "drivers", "s23gpuprobe"))
import verify_pe  # noqa: E402

PE_OFF = 0x80
OPT_SIZE = 240
TEXT_RVA, TEXT_RAW = 0x1000, 0x400
RDATA_RVA, RDATA_RAW = 0x2000, 0x600
SECT_RAW_SIZE = 0x200


def _rdata(imports, exports, dll_name):
    """-> (bytes, import_dir_rva, import_dir_size, export_dir_rva, export_dir_size)"""
    buf = bytearray()

    def put(b):
        off = len(buf)
        buf.extend(b)
        return RDATA_RVA + off

    def align8():
        while len(buf) % 8:
            buf.append(0)

    imp_rva = imp_sz = exp_rva = exp_sz = 0
    if imports:
        descs = []
        for dll, syms in imports.items():
            name_rva = put(dll.encode() + b"\0")
            hint_rvas = [put(b"\0\0" + s.encode() + b"\0") for s in syms]
            align8()
            ilt_rva = put(b"".join(struct.pack("<Q", r) for r in hint_rvas) + struct.pack("<Q", 0))
            descs.append((ilt_rva, name_rva))
        align8()
        imp_rva = put(b"".join(struct.pack("<5I", ilt, 0, 0, name, ilt) for ilt, name in descs) + bytes(20))
        imp_sz = 20 * (len(descs) + 1)
    if exports:
        name_rvas = [put(s.encode() + b"\0") for s in sorted(exports)]
        dll_rva = put(dll_name.encode() + b"\0")
        align8()
        names_tbl = put(b"".join(struct.pack("<I", r) for r in name_rvas))
        exp_rva = put(struct.pack("<IIHHIIIIIII", 0, 0, 0, 0, dll_rva, 1, len(exports), len(exports), 0, names_tbl, 0))
        exp_sz = 40
    return bytes(buf), imp_rva, imp_sz, exp_rva, exp_sz


def build_pe(code_words=(), imports=None, exports=None, machine=0xAA64, subsystem=1, dll_chars=0x0160,
             reloc=True, signed=True, fix_checksum=True, dll_name="ntoskrnl.exe"):
    imports = {"ntoskrnl.exe": ["IoCreateDevice"]} if imports is None else imports
    rdata, imp_rva, imp_sz, exp_rva, exp_sz = _rdata(imports, exports, dll_name)
    assert len(rdata) <= SECT_RAW_SIZE
    text = b"".join(struct.pack("<I", w) for w in code_words) or struct.pack("<I", 0xD65F03C0)  # ret
    assert len(text) <= SECT_RAW_SIZE

    img = bytearray(RDATA_RAW + SECT_RAW_SIZE)
    img[0:2] = b"MZ"
    struct.pack_into("<I", img, 0x3C, PE_OFF)
    img[PE_OFF:PE_OFF + 4] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", img, PE_OFF + 4, machine, 2, 0, 0, 0, OPT_SIZE, 0x22)
    opt = PE_OFF + 24
    struct.pack_into("<H", img, opt, 0x20B)
    struct.pack_into("<I", img, opt + 16, TEXT_RVA)                     # AddressOfEntryPoint
    struct.pack_into("<Q", img, opt + 24, 0x140000000)                  # ImageBase
    struct.pack_into("<II", img, opt + 32, 0x1000, 0x200)               # Section/FileAlignment
    struct.pack_into("<I", img, opt + 56, 0x3000)                       # SizeOfImage
    struct.pack_into("<I", img, opt + 60, 0x400)                        # SizeOfHeaders
    struct.pack_into("<HH", img, opt + 68, subsystem, dll_chars)
    struct.pack_into("<I", img, opt + 108, 16)                          # NumberOfRvaAndSizes
    dd = opt + 112
    struct.pack_into("<II", img, dd + 0 * 8, exp_rva, exp_sz)
    struct.pack_into("<II", img, dd + 1 * 8, imp_rva, imp_sz)
    if signed:
        struct.pack_into("<II", img, dd + 4 * 8, len(img), 8)           # Authenticode blob "after" the image
    if reloc:
        struct.pack_into("<II", img, dd + 5 * 8, RDATA_RVA + 0x1F0, 8)
    sec = opt + OPT_SIZE
    struct.pack_into("<8sIIII12xI", img, sec, b".text", len(text), TEXT_RVA, SECT_RAW_SIZE, TEXT_RAW, 0x60000020)
    struct.pack_into("<8sIIII12xI", img, sec + 40, b".rdata", max(len(rdata), 1), RDATA_RVA, SECT_RAW_SIZE, RDATA_RAW, 0x40000040)
    img[TEXT_RAW:TEXT_RAW + len(text)] = text
    img[RDATA_RAW:RDATA_RAW + len(rdata)] = rdata
    if fix_checksum:
        struct.pack_into("<I", img, opt + 64, verify_pe.compute_pe_checksum(bytes(img)))
    return bytes(img)


# ARM64 instruction encodings used by the tests
def sub_sp_imm(imm, lsl12=False):
    """sub sp, sp, #imm{, lsl #12}"""
    return 0xD10003FF | (1 << 22 if lsl12 else 0) | ((imm & 0xFFF) << 10)


SUB_SP_X15_UXTX4 = 0xCB2F73FF   # follows every `bl __chkstk`
MOV_X15_IMM = 0xD280000F        # mov x15, #0 (placeholder)
BL = 0x94000000                 # bl . (placeholder)
RET = 0xD65F03C0
NOP = 0xD503201F
