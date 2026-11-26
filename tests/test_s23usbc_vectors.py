"""tests/c/s23usbc_vectors.h must match what usb_research/gen_s23usbc_c_vectors.py renders from the
Python reference (usb_research/max77705_mailbox.py), so the C host tests stay in lock-step with it."""
import os
import subprocess
import sys
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))


class S23UsbcVectorsFreshTest(unittest.TestCase):
    def test_vector_header_is_up_to_date(self):
        gen = os.path.join(ROOT, "usb_research", "gen_s23usbc_c_vectors.py")
        r = subprocess.run([sys.executable, gen, "--check"], cwd=ROOT, capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


if __name__ == "__main__":
    unittest.main()
