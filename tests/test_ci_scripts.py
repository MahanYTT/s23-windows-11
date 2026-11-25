"""scripts/ci: repo hygiene rules and the driver stack check."""
import contextlib
import io
import os
import sys
import tempfile
import unittest

from pe_builder import BL, MOV_X15_IMM, RET, SUB_SP_X15_UXTX4, build_pe, sub_sp_imm

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "scripts", "ci"))
import check_driver_stack  # noqa: E402
import check_repo_hygiene  # noqa: E402

MB = 1048576


class HygieneTest(unittest.TestCase):
    def violations(self, paths, sizes=None):
        sizes = sizes or {}
        return [p for p, _ in check_repo_hygiene.find_violations(paths, lambda p: sizes.get(p, 100), 5 * MB)]

    def test_sources_and_docs_are_fine(self):
        ok = ["README.md", "drivers/s23bthx/s23bthx.c", "drivers/s23gpu/build.ps1", "s23link.py",
              "drivers/s23touch/s23touch.inf", "gpu_research/s23_gpu_bringup/s23_gpu_acpi.asl",
              "drivers/s23gpu/scm_arm64.asm", "drivers/s23gpu/mesa_s23_a740.patch", ".gitignore"]
        self.assertEqual(self.violations(ok), [])

    def test_binaries_and_firmware_are_rejected(self):
        bad = ["drivers/s23bthx/pkg/s23bthx.sys", "out/tool.EXE", "x/umd.dll", "a.obj", "a.pdb", "MEMORY.DMP",
               "s23batt.log", "fw/a740_sqe.fw", "fw/gmu.bin", "modem.mbn", "Mu-dm1q.img", "fw/zap.elf",
               "fw/zap.elf.b00", "pkg/s23touch.cat", "pkg/s23test.cer", "dsdt.aml", "nvm.tlv", "dump.tar.gz"]
        self.assertEqual(self.violations(bad), bad)

    def test_large_files_are_rejected(self):
        self.assertEqual(self.violations(["big.md", "small.md"], {"big.md": 6 * MB, "small.md": MB}), ["big.md"])

    def test_missing_file_is_ignored(self):
        self.assertEqual(check_repo_hygiene.find_violations(["gone.c"], lambda p: None, MB), [])


class DriverStackCheckTest(unittest.TestCase):
    def run_check(self, img, *opts):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "drv.sys")
            with open(p, "wb") as f:
                f.write(img)
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                rc = check_driver_stack.main(list(opts) + [p])
        return rc, out.getvalue()

    def test_clean_driver(self):
        rc, out = self.run_check(build_pe([sub_sp_imm(0x60), RET], signed=False))
        self.assertEqual(rc, 0, out)
        self.assertIn("OK (0 frame warning(s))", out)

    def test_chkstk_fails_even_unsigned(self):
        rc, out = self.run_check(build_pe([MOV_X15_IMM, BL, SUB_SP_X15_UXTX4, RET], signed=False))
        self.assertEqual(rc, 1)
        self.assertIn("::error", out)

    def test_large_frame_is_a_warning(self):
        rc, out = self.run_check(build_pe([sub_sp_imm(0x500), RET], signed=False))
        self.assertEqual(rc, 0)
        self.assertIn("::warning", out)
        rc, out = self.run_check(build_pe([sub_sp_imm(0x500), RET], signed=False), "--frame-warn", "2048")
        self.assertNotIn("::warning", out)

    def test_empty_reloc_fails(self):
        rc, out = self.run_check(build_pe([RET], signed=False, reloc=False))
        self.assertEqual(rc, 1)
        self.assertIn(".reloc", out)

    def test_usage(self):
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(check_driver_stack.main([]), 2)
            self.assertEqual(check_driver_stack.main(["--frame-warn"]), 2)


if __name__ == "__main__":
    unittest.main()
