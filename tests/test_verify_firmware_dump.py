"""scripts/verify_firmware_dump.py with small synthetic ELF / split-MDT fixtures (no real firmware)."""
import contextlib
import hashlib
import io
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "scripts"))
import verify_firmware_dump as vfd  # noqa: E402

HDR_FLAGS = 7 << 24     # Qualcomm segment type 7: ELF header + program headers
HASH_FLAGS = 2 << 24    # segment type 2: hash segment
CODE_FLAGS = 0x08000005


def build_elf32(payloads, paddr=0x8A800000, algo="sha384", corrupt_hash=False):
    """ELF32 image: seg0 = headers (PT_NULL), one PT_LOAD per payload, last = hash segment with the
    digests of seg0 and every payload. -> (bytes, [(offset, size)] per segment)"""
    nseg = len(payloads) + 2
    phoff, phentsize = 52, 32
    hdr_size = phoff + nseg * phentsize
    offs, cur = [], 0x1000
    for p in payloads:
        offs.append(cur)
        cur += (len(p) + 0xFFF) & ~0xFFF
    hash_off = cur

    def headers(hash_len):
        eh = b"\x7fELF" + bytes([1, 1, 1]) + bytes(9)
        eh += struct.pack("<HHIIIIIHHHHHH", 2, 164, 1, paddr, phoff, 0, 0, 52, phentsize, nseg, 40, 0, 0)
        ph = struct.pack("<8I", 0, 0, 0, 0, hdr_size, 0, HDR_FLAGS, 0)
        pa = paddr
        for p, o in zip(payloads, offs):
            ph += struct.pack("<8I", 1, o, pa, pa, len(p), len(p) + 0x100, CODE_FLAGS, 0x1000)
            pa += (len(p) + 0x100 + 0xFFF) & ~0xFFF
        ph += struct.pack("<8I", 0, hash_off, 0, 0, hash_len, hash_len, HASH_FLAGS, 0x1000)
        return eh + ph

    dlen = hashlib.new(algo).digest_size
    hash_len = 40 + dlen * (len(payloads) + 1)
    hdr = headers(hash_len)
    digests = [hashlib.new(algo, hdr).digest()] + [hashlib.new(algo, p).digest() for p in payloads]
    if corrupt_hash:
        digests[-1] = bytes(dlen)
    hseg = bytes(40) + b"".join(digests)
    img = bytearray(hash_off + len(hseg))
    img[:len(hdr)] = hdr
    for p, o in zip(payloads, offs):
        img[o:o + len(p)] = p
    img[hash_off:] = hseg
    segs = [(0, len(hdr))] + list(zip(offs, map(len, payloads))) + [(hash_off, len(hseg))]
    return bytes(img), segs


def write_split(directory, name, img, segs):
    """PIL-style split: name.mdt = headers + hash segment, name.bNN = each segment."""
    hdr_end = segs[0][1]
    # program-header offsets refer to the full image, so the verifier takes every segment from .bNN
    with open(os.path.join(directory, name + ".mdt"), "wb") as f:
        f.write(img[:hdr_end])
    for i, (o, n) in enumerate(segs):
        with open(os.path.join(directory, f"{name}.b{i:02d}"), "wb") as f:
            f.write(img[o:o + n])


def wfile(path, data):
    with open(path, "wb") as f:
        f.write(data)


class FixtureMixin:
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = self.tmp.name

    def path(self, *parts):
        p = os.path.join(self.root, *parts)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        return p


class ParseElfTest(unittest.TestCase):
    def test_elf32_headers(self):
        img, _ = build_elf32([b"A" * 100, b"B" * 5000])
        e = vfd.parse_elf(img)
        self.assertEqual((e["cls"], e["machine"], e["entry"]), (1, 164, 0x8A800000))
        self.assertEqual(len(e["phdrs"]), 4)
        self.assertEqual(vfd.seg_type(e["phdrs"][-1]), vfd.SEG_TYPE_HASH)

    def test_elf64_headers(self):
        eh = b"\x7fELF" + bytes([2, 1, 1]) + bytes(9)
        eh += struct.pack("<HHIQQQIHHHHHH", 2, 183, 1, 0x1000, 64, 0, 0, 64, 56, 1, 64, 0, 0)
        ph = struct.pack("<IIQQQQQQ", 1, 0x08000005, 0x200, 0x1000, 0x1000, 0x10, 0x20, 0x1000)
        img = eh + ph + bytes(0x200 - 120) + b"x" * 0x10
        e = vfd.parse_elf(img)
        self.assertEqual(e["cls"], 2)
        self.assertEqual(e["phdrs"][0]["paddr"], 0x1000)
        self.assertEqual(e["phdrs"][0]["memsz"], 0x20)

    def test_rejects_bad_input(self):
        for bad in (b"", b"MZ" + bytes(60), b"\x7fELF" + bytes([3, 1]) + bytes(60),
                    b"\x7fELF" + bytes([1, 2]) + bytes(60)):
            with self.assertRaises(vfd.ElfError):
                vfd.parse_elf(bad)
        img, _ = build_elf32([b"A"])
        with self.assertRaises(vfd.ElfError):
            vfd.parse_elf(img[:60])   # program header table cut off


class CheckImageTest(FixtureMixin, unittest.TestCase):
    def test_single_file_elf_ok(self):
        img, _ = build_elf32([b"code" * 300, b"data" * 900])
        p = self.path("01_gpu", "fw.elf")
        wfile(p, img)
        r = vfd.check_image(p)
        self.assertTrue(r["ok"], r["problems"])
        self.assertEqual(r["hash_verified"], 3)
        self.assertEqual(r["hash_algo"], "sha384")
        self.assertEqual(r["load_lo"], 0x8A800000)

    def test_sha256_hash_segment(self):
        img, _ = build_elf32([b"x" * 64], algo="sha256")
        p = self.path("fw.mbn")
        wfile(p, img)
        r = vfd.check_image(p)
        self.assertTrue(r["ok"], r["problems"])
        self.assertEqual(r["hash_algo"], "sha256")

    def test_split_mdt_ok_and_load_span(self):
        img, segs = build_elf32([b"a" * 0x1800, b"b" * 0x300])
        d = os.path.dirname(self.path("04", "modem_image", "x"))
        write_split(d, "modem", img, segs)
        r = vfd.check_image(os.path.join(d, "modem.mdt"))
        self.assertTrue(r["ok"], r["problems"])
        self.assertEqual(r["hash_verified"], 3)
        self.assertEqual((r["load_lo"], r["load_hi"]), (0x8A800000, 0x8A800000 + 0x2000 + 0x300 + 0x100))

    def test_corrupted_segment_is_detected(self):
        img, segs = build_elf32([b"a" * 0x100, b"b" * 0x100])
        d = os.path.dirname(self.path("m", "x"))
        write_split(d, "modem", img, segs)
        with open(os.path.join(d, "modem.b02"), "r+b") as f:
            f.write(b"X")
        r = vfd.check_image(os.path.join(d, "modem.mdt"))
        self.assertFalse(r["ok"])
        self.assertEqual(r["hash_failed"], [2])

    def test_missing_and_short_segments_are_detected(self):
        img, segs = build_elf32([b"a" * 0x100, b"b" * 0x100])
        d = os.path.dirname(self.path("m", "x"))
        write_split(d, "modem", img, segs)
        os.remove(os.path.join(d, "modem.b01"))
        with open(os.path.join(d, "modem.b02"), "r+b") as f:
            f.truncate(0x80)
        r = vfd.check_image(os.path.join(d, "modem.mdt"))
        self.assertFalse(r["ok"])
        text = " ".join(r["problems"])
        self.assertIn("segment 1: missing", text)
        self.assertIn("segment 2: 128 bytes", text)

    def test_wrong_digest_in_hash_segment(self):
        img, _ = build_elf32([b"a" * 10, b"b" * 10], corrupt_hash=True)
        p = self.path("fw.elf")
        wfile(p, img)
        r = vfd.check_image(p)
        self.assertFalse(r["ok"])
        self.assertEqual(r["hash_failed"], [2])

    def test_non_elf_container_is_skipped(self):
        p = self.path("apnhlos_image", "prov.mdt")
        wfile(p, bytes.fromhex("10e35b1e20130724") + os.urandom(64))
        r = vfd.check_image(p)
        self.assertIn("skipped", r)
        self.assertTrue(r["ok"])


class SumsTest(FixtureMixin, unittest.TestCase):
    def test_write_then_verify_roundtrip(self):
        wfile(self.path("01_gpu", "a.fw"), b"alpha")
        wfile(self.path("04", "b.bin"), b"beta")
        sums = os.path.join(self.root, "SHA256SUMS.txt")
        self.assertEqual(vfd.write_sums(self.root, sums), 2)
        r = vfd.check_sums(self.root, sums)
        self.assertEqual((r["ok"], r["mismatch"], r["missing"], r["extra"]), (2, [], [], []))

    def test_mismatch_missing_and_extra(self):
        wfile(self.path("a.fw"), b"alpha")
        wfile(self.path("b.fw"), b"beta")
        sums = os.path.join(self.root, "SHA256SUMS.txt")
        vfd.write_sums(self.root, sums)
        wfile(self.path("a.fw"), b"ALPHA")
        os.remove(self.path("b.fw"))
        wfile(self.path("c.fw"), b"new")
        r = vfd.check_sums(self.root, sums)
        self.assertEqual((r["mismatch"], r["missing"], r["extra"]), (["a.fw"], ["b.fw"], ["c.fw"]))

    def test_parse_sums_formats(self):
        h = "a" * 64
        self.assertEqual(vfd.parse_sums(f"{h}  ./x/y.bin\n{h} *z.bin\n# comment\n\n"),
                         {"x/y.bin": h, "z.bin": h})
        with self.assertRaises(ValueError):
            vfd.parse_sums("nothex  file\n")


class CliTest(FixtureMixin, unittest.TestCase):
    def run_cli(self, *argv):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = vfd.main(list(argv))
        return rc, out.getvalue()

    def make_dump(self):
        img, segs = build_elf32([b"m" * 0x300, b"n" * 0x40])
        d = os.path.dirname(self.path("04_modem_5g_dsp_apnhlos", "modem_image", "x"))
        write_split(d, "modem", img, segs)
        wfile(self.path("03_audio_adsp_haptics", "bad.elf"), b"\x7fELF" + bytes(10))  # skipped by default
        sums = os.path.join(self.root, "SHA256SUMS.txt")
        vfd.write_sums(self.root, sums)

    def test_full_check_ok(self):
        self.make_dump()
        rc, out = self.run_cli(self.root)
        self.assertEqual(rc, 0, out)
        self.assertIn("FIRMWARE DUMP: OK", out)
        self.assertIn("modem.mdt", out)
        self.assertNotIn("03_audio", out)

    def test_audio_folder_only_with_flag(self):
        self.make_dump()
        rc, out = self.run_cli(self.root, "--images-only", "--include-audio")
        self.assertEqual(rc, 1)
        self.assertIn("03_audio_adsp_haptics/bad.elf", out)

    def test_missing_sums_file_fails(self):
        self.make_dump()
        os.remove(os.path.join(self.root, "SHA256SUMS.txt"))
        rc, out = self.run_cli(self.root)
        self.assertEqual(rc, 1)
        self.assertIn("not found", out)
        rc, _ = self.run_cli(self.root, "--images-only")
        self.assertEqual(rc, 0)

    def test_tampered_file_fails(self):
        self.make_dump()
        with open(self.path("04_modem_5g_dsp_apnhlos", "modem_image", "modem.b01"), "r+b") as f:
            f.write(b"Z")
        rc, out = self.run_cli(self.root)
        self.assertEqual(rc, 1)
        self.assertIn("MISMATCH", out)
        self.assertIn("FAIL", out)

    def test_usage_errors(self):
        self.assertEqual(self.run_cli()[0], 2)
        self.assertEqual(self.run_cli(os.path.join(self.root, "nope"))[0], 2)
        self.assertEqual(self.run_cli(self.root, "--sums")[0], 2)
        self.assertEqual(self.run_cli(self.root, "--bogus")[0], 2)


if __name__ == "__main__":
    unittest.main()
