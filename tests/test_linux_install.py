"""Unit tests for native Linux installation helper & distro-agnostic Clang driver resolution."""
import os
import shutil
import struct
import sys
import tempfile
import unittest
import uuid

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO_ROOT, "scripts", "linux"))
import install_s23_linux as inst  # noqa: E402


def _has_hivex():
    try:
        import hivex  # noqa: F401
        return True
    except ImportError:
        return False


class SafetyGuardrailsTest(unittest.TestCase):
    def test_refuses_sdb_and_sde(self):
        for bad in ("/dev/sdb", "/dev/sdb1", "/dev/sde", "sde2", "/dev/mmcblk0"):
            with self.assertRaises(inst.SafetyError):
                inst.assert_safe_target(bad)

    def test_allows_win_partition_or_dir(self):
        inst.assert_safe_target("/dev/sda43")
        inst.assert_safe_target("/mnt/s23_win")

    def test_validate_android_boot_image(self):
        with tempfile.NamedTemporaryFile(delete=False) as f:
            f.write(b"BADMAGIC" + b"\x00" * 4096)
            bad_path = f.name
        self.addCleanup(os.unlink, bad_path)
        with self.assertRaises(ValueError):
            inst.validate_android_boot_image(bad_path)

        with tempfile.NamedTemporaryFile(delete=False) as f:
            f.write(b"ANDROID!" + b"\x00" * 4096)
            good_path = f.name
        self.addCleanup(os.unlink, good_path)
        self.assertEqual(inst.validate_android_boot_image(good_path), 4104)


class BcdAndHiveEncodingTest(unittest.TestCase):
    def test_gpt_bcd_locator_layout(self):
        blob = inst.make_gpt_bcd_locator(inst.ESP_PART_GUID, inst.DEFAULT_DISK_GUID)
        self.assertEqual(len(blob), 88)
        self.assertEqual(struct.unpack_from("<I", blob, 16)[0], 6)
        self.assertEqual(struct.unpack_from("<I", blob, 24)[0], 72)
        self.assertEqual(blob[32:48], uuid.UUID(inst.ESP_PART_GUID).bytes_le)
        self.assertEqual(blob[48:56], b"\x00" * 8)
        self.assertEqual(blob[56:72], uuid.UUID(inst.DEFAULT_DISK_GUID).bytes_le)

    def test_badmemorylist_mpss_dsm_pfns(self):
        blob = inst.encode_badmemorylist_pfns()
        expected_pages = inst.MPSS_DSM_END_PFN - inst.MPSS_DSM_START_PFN + 1
        self.assertEqual(expected_pages, 13056)  # 51 MiB @ 4 KiB
        self.assertEqual(len(blob), 104448)
        self.assertEqual(struct.unpack_from("<Q", blob, 0)[0], 0xD4D00)
        self.assertEqual(struct.unpack_from("<Q", blob, len(blob) - 8)[0], 0xD7FFF)

    @unittest.skipUnless(_has_hivex(), "python-hivex only available on Linux")
    def test_hivex_patches_bcd_and_sanpolicy_against_real_hives(self):
        import hivex  # type: ignore

        bcd_tpl = "/mnt/c/Windows/System32/config/BCD-Template"
        if not os.path.isfile(bcd_tpl):
            self.skipTest("BCD-Template not mounted")
        with tempfile.TemporaryDirectory() as tmpd:
            dst = os.path.join(tmpd, "BCD")
            shutil.copy2(bcd_tpl, dst)
            stats = inst.patch_bcd_hive(dst, inst.DEFAULT_DISK_GUID)
            self.assertGreaterEqual(stats["bootmgr"], 1)
            h = hivex.Hivex(dst)
            objs = h.node_get_child(h.root(), "Objects")
            bm = h.node_get_child(objs, inst.BCD_BOOTMGR_GUID)
            elems = h.node_get_child(bm, "Elements")
            _, badmem = h.node_get_value(h.node_get_child(elems, "1700000a"), "Element")
            self.assertEqual(len(badmem), 104448)
            _, dev_loc = h.node_get_value(h.node_get_child(elems, "11000001"), "Element")
            self.assertEqual(dev_loc, inst.make_gpt_bcd_locator(inst.ESP_PART_GUID, inst.DEFAULT_DISK_GUID))


if __name__ == "__main__":
    unittest.main()
