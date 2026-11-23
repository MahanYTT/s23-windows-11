"""verify_pe.py: header checks, __chkstk detection and stack-frame scan on synthetic PE32+ images."""
import contextlib
import io
import os
import tempfile
import unittest

from pe_builder import BL, MOV_X15_IMM, NOP, RET, SUB_SP_X15_UXTX4, build_pe, sub_sp_imm, verify_pe


class VerifyPeTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def write(self, name, data):
        p = os.path.join(self.tmp.name, name)
        with open(p, "wb") as f:
            f.write(data)
        return p

    def run_main(self, *argv):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = verify_pe.main(list(argv))
        return rc, out.getvalue()

    # ---- baseline
    def test_clean_driver_passes(self):
        p = self.write("ok.sys", build_pe([sub_sp_imm(0x40), NOP, RET]))
        rc, out = self.run_main(p)
        self.assertEqual(rc, 0, out)
        self.assertIn("PE VERIFICATION: PASS", out)
        self.assertIn("__chkstk call sites: 0", out)

    def test_header_failures_are_all_reported(self):
        p = self.write("bad.sys", build_pe(machine=0x8664, reloc=False, signed=False, fix_checksum=False))
        rc, out = self.run_main(p)
        self.assertEqual(rc, 1)
        for msg in ("Not ARM64", "Missing .reloc section", "Missing Authenticode", "CheckSum mismatch"):
            self.assertIn(msg, out)
        self.assertIn("PE VERIFICATION: FAIL", out)

    def test_tsaware_fails(self):
        p = self.write("ts.sys", build_pe(dll_chars=0x8160))
        rc, out = self.run_main(p)
        self.assertEqual(rc, 1)
        self.assertIn("TSAWARE", out)

    def test_not_a_pe_file(self):
        p = self.write("junk.sys", b"not a driver")
        rc, out = self.run_main(p)
        self.assertEqual(rc, 1)
        self.assertIn("FAIL", out)

    def test_unexpected_dll_import_fails(self):
        p = self.write("imp.sys", build_pe(imports={"ntoskrnl.exe": ["IoCreateDevice"], "msvcrt.dll": ["memcpy"]}))
        rc, out = self.run_main(p)
        self.assertEqual(rc, 1)
        self.assertIn("Unexpected DLL imports", out)

    # ---- __chkstk
    def test_chkstk_call_sequence_fails(self):
        p = self.write("chk.sys", build_pe([MOV_X15_IMM, BL, SUB_SP_X15_UXTX4, RET]))
        rc, out = self.run_main(p)
        self.assertEqual(rc, 1)
        self.assertIn("__chkstk call sites: 1", out)
        self.assertIn("RVA 0x1008", out)

    def test_chkstk_import_fails(self):
        p = self.write("chkimp.sys", build_pe(imports={"ntoskrnl.exe": ["IoCreateDevice", "__chkstk"]}))
        rc, out = self.run_main(p)
        self.assertEqual(rc, 1)
        self.assertIn("__chkstk imported", out)

    def test_chkstk_pattern_outside_code_is_ignored(self):
        # the same 4 bytes inside .rdata (string/constant data) must not trip the check
        img = bytearray(build_pe([RET], fix_checksum=False))
        img[0x600 + 0x1F8:0x600 + 0x1FC] = SUB_SP_X15_UXTX4.to_bytes(4, "little")
        img[0x80 + 24 + 64:0x80 + 24 + 68] = verify_pe.compute_pe_checksum(bytes(img)).to_bytes(4, "little")
        self.assertEqual(verify_pe.find_chkstk_sites(bytes(img)), [])

    # ---- stack frames
    def test_frame_scan_sums_consecutive_subs(self):
        img = build_pe([sub_sp_imm(1, lsl12=True), sub_sp_imm(0x230), NOP, sub_sp_imm(0x20), RET])
        self.assertEqual(verify_pe.find_stack_frames(img), [(0x1000, 0x1230), (0x100C, 0x20)])

    def test_large_frame_warns_but_passes(self):
        p = self.write("big.sys", build_pe([sub_sp_imm(0x800), RET]))
        rc, out = self.run_main(p)
        self.assertEqual(rc, 0, out)
        self.assertIn("WARN: stack frame of 2048 bytes", out)

    def test_frame_warn_threshold_is_configurable(self):
        p = self.write("big.sys", build_pe([sub_sp_imm(0x800), RET]))
        rc, out = self.run_main(p, "--frame-warn", "4096")
        self.assertEqual(rc, 0)
        self.assertNotIn("WARN", out)

    def test_max_frame_turns_warning_into_failure(self):
        p = self.write("big.sys", build_pe([sub_sp_imm(0x800), RET]))
        rc, out = self.run_main(p, "--max-frame", "1024")
        self.assertEqual(rc, 1)
        self.assertIn("exceeds --max-frame 1024", out)

    # ---- reference import resolution
    def test_imports_resolved_against_reference(self):
        drv = self.write("drv.sys", build_pe(imports={"ntoskrnl.exe": ["IoCreateDevice", "IoDeleteDevice"]}))
        nt_ok = self.write("nt_ok.exe", build_pe(imports={}, exports={"IoCreateDevice", "IoDeleteDevice", "KeBugCheckEx"}))
        nt_old = self.write("nt_old.exe", build_pe(imports={}, exports={"IoCreateDevice"}))
        rc, out = self.run_main(drv, nt_ok)
        self.assertEqual(rc, 0, out)
        self.assertIn("2/2 OK", out)
        rc, out = self.run_main(drv, nt_old)
        self.assertEqual(rc, 1)
        self.assertIn("Unresolved ntoskrnl.exe imports: ['IoDeleteDevice']", out)

    def test_missing_reference_is_reported_not_fatal(self):
        drv = self.write("drv.sys", build_pe())
        rc, out = self.run_main(drv, os.path.join(self.tmp.name, "nope.exe"))
        self.assertEqual(rc, 0, out)
        self.assertIn("NOTE: reference", out)

    # ---- CLI
    def test_usage_errors(self):
        self.assertEqual(self.run_main()[0], 2)
        self.assertEqual(self.run_main("x.sys", "--max-frame")[0], 2)
        self.assertEqual(self.run_main("x.sys", "--frame-warn", "abc")[0], 2)

    def test_checksum_matches_builder(self):
        img = build_pe([RET])
        stored = int.from_bytes(img[0x80 + 24 + 64:0x80 + 24 + 68], "little")
        self.assertEqual(stored, verify_pe.compute_pe_checksum(img))


if __name__ == "__main__":
    unittest.main()
