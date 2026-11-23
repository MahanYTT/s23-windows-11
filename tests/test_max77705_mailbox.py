"""usb_research/max77705_mailbox.py: OPCODE mailbox framing and 0x25 status-register bit fields
as specified in usb_research/S23_USBC_ARCHITECTURE.md (pure byte logic, no hardware)."""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "usb_research"))
import max77705_mailbox as mb  # noqa: E402


class MailboxFramingTest(unittest.TestCase):
    def test_aperture_constants_match_spec(self):
        self.assertEqual((mb.REG_OPCODE, mb.REG_END_DATA, mb.REG_OPCODE_READ, mb.REG_OPCODE_READ_END),
                         (0x21, 0x41, 0x51, 0x71))
        self.assertEqual(mb.MAX_WRITE_DATA, 32)   # 0x22..0x41
        self.assertEqual(mb.MAX_READ_DATA, 32)    # 0x52..0x71

    def test_short_write_gets_commit_write(self):
        # OPCODE_READ_SBU (0x25): write [0x01] (len 1)
        self.assertEqual(mb.encode_write(0x25, b"\x01"), [(0x21, b"\x25\x01"), (0x41, b"\x00")])

    def test_opcode_only_write(self):
        self.assertEqual(mb.encode_write(0x55), [(0x21, b"\x55"), (0x41, b"\x00")])

    def test_write_reaching_end_register_needs_no_commit(self):
        data = bytes(range(32))
        w = mb.encode_write(0x30, data)
        self.assertEqual(w, [(0x21, b"\x30" + data)])
        self.assertEqual(0x21 + len(w[0][1]) - 1, 0x41)

    def test_write_one_short_of_end_still_commits(self):
        w = mb.encode_write(0x30, bytes(31))
        self.assertEqual(len(w), 2)
        self.assertEqual(w[1][0], 0x41)

    def test_commit_byte_is_configurable(self):
        # the spec leaves the exact commit byte to be confirmed against Samsung's driver
        self.assertEqual(mb.encode_write(0x56, b"\x07", commit_byte=0xFF)[1], (0x41, b"\xff"))

    def test_invalid_writes(self):
        for args in [(0x100,), (-1,), (0x30, bytes(33))]:
            with self.assertRaises(mb.MailboxError):
                mb.encode_write(*args)
        with self.assertRaises(mb.MailboxError):
            mb.encode_write(0x30, b"", commit_byte=256)

    def test_read_request_and_response(self):
        self.assertEqual(mb.read_request(2), (0x51, 3))
        self.assertEqual(mb.decode_response(b"\x25\x10\x20", 0x25, 2), b"\x10\x20")   # sbu1, sbu2
        self.assertEqual(mb.decode_response(b"\x55", 0x55, 0), b"")
        self.assertEqual(mb.read_request(32), (0x51, 33))
        with self.assertRaises(mb.MailboxError):
            mb.read_request(33)

    def test_response_validation(self):
        with self.assertRaises(mb.MailboxError):
            mb.decode_response(b"\x24\x10\x20", 0x25, 2)      # wrong echo
        with self.assertRaises(mb.MailboxError):
            mb.decode_response(b"\x25\x10", 0x25, 2)          # truncated
        with self.assertRaises(mb.MailboxError):
            mb.decode_response(b"\x25\x10\x20\x30", 0x25, 2)  # too long

    def test_response_ready_bit(self):
        self.assertTrue(mb.response_ready(0x80))
        self.assertTrue(mb.response_ready(0xFF))
        self.assertFalse(mb.response_ready(0x7F))


class StatusRegisterTest(unittest.TestCase):
    def test_cc_status0_fields(self):
        self.assertEqual(mb.decode_cc_status0(0b10_11_1_101),
                         {"CCPinStat": 2, "CCIStat": 3, "CCVcnStat": 1, "CCStat": 5})
        self.assertEqual(mb.decode_cc_status0(0), {"CCPinStat": 0, "CCIStat": 0, "CCVcnStat": 0, "CCStat": 0})

    def test_cc_status1_fields(self):
        d = mb.decode_cc_status1(0b01_1_0_1_0_1_1)
        self.assertEqual(d, {"CCSBUSHORT": 1, "VCONNOCP": 1, "VCONNSC": 0, "VSAFE0V": 1,
                             "AttachSrcErr": 0, "ConnStat": 1, "Altmode": 1})

    def test_bc_status_fields(self):
        self.assertEqual(mb.decode_bc_status(0b1_0_101_1_10), {"VBUSDet": 1, "PrChgTyp": 5, "DCDTmo": 1, "ChgTyp": 2})

    def test_pd_status1_fields(self):
        self.assertEqual(mb.decode_pd_status1(0b1_00_1_1010), {"PD_DataRole": 1, "PD_PSRDY": 1, "FCT_ID": 0xA})

    def test_every_byte_round_trips_through_its_fields(self):
        for v in range(256):
            c = mb.decode_cc_status0(v)
            self.assertEqual((c["CCPinStat"] << 6) | (c["CCIStat"] << 4) | (c["CCVcnStat"] << 3) | c["CCStat"], v)
            b = mb.decode_bc_status(v)
            self.assertEqual((b["VBUSDet"] << 7) | (b["PrChgTyp"] << 3) | (b["DCDTmo"] << 2) | b["ChgTyp"], v & 0xBF)

    def test_out_of_range_register_value(self):
        for f in (mb.decode_cc_status0, mb.decode_cc_status1, mb.decode_bc_status, mb.decode_pd_status1):
            with self.assertRaises(mb.MailboxError):
                f(256)


class PowerDeliveryTest(unittest.TestCase):
    """USB PD 3.0 PDO/RDO bit layouts (generic PD, not MAX77705-specific)."""

    def test_fixed_pdo_5v_3a(self):
        p = mb.decode_pdo(0x0801912C)                     # 5 V / 3 A, unconstrained power
        self.assertEqual((p["type"], p["voltage_mv"], p["max_current_ma"], p["unconstrained_power"]),
                         (mb.PDO_FIXED, 5000, 3000, 1))
        self.assertEqual(mb.encode_pdo(p), 0x0801912C)

    def test_fixed_pdo_flags(self):
        p = mb.decode_pdo((0x3F << 24) | (2 << 20) | (180 << 10) | 200)   # all flags, peak 2, 9 V / 2 A
        self.assertEqual([p[k] for k in ("dual_role_power", "usb_suspend_supported", "unconstrained_power",
                                         "usb_comms_capable", "dual_role_data", "unchunked_ext_msg")], [1] * 6)
        self.assertEqual((p["peak_current"], p["voltage_mv"], p["max_current_ma"]), (2, 9000, 2000))

    def test_variable_and_battery_pdo(self):
        v = mb.decode_pdo((2 << 30) | (240 << 20) | (100 << 10) | 150)
        self.assertEqual((v["type"], v["max_voltage_mv"], v["min_voltage_mv"], v["max_current_ma"]),
                         (mb.PDO_VARIABLE, 12000, 5000, 1500))
        b = mb.decode_pdo((1 << 30) | (420 << 20) | (60 << 10) | 60)
        self.assertEqual((b["type"], b["max_voltage_mv"], b["min_voltage_mv"], b["max_power_mw"]),
                         (mb.PDO_BATTERY, 21000, 3000, 15000))
        for p in (v, b):
            self.assertEqual(mb.decode_pdo(mb.encode_pdo(p)), p)

    def test_pps_apdo(self):
        raw = (3 << 30) | (1 << 27) | (110 << 17) | (33 << 8) | 60        # 3.3-11 V, 3 A, power limited
        p = mb.decode_pdo(raw)
        self.assertEqual((p["type"], p["min_voltage_mv"], p["max_voltage_mv"], p["max_current_ma"], p["pps_power_limited"]),
                         (mb.PDO_PPS, 3300, 11000, 3000, 1))
        self.assertEqual(mb.encode_pdo(p), raw)
        with self.assertRaises(mb.PdError):
            mb.decode_pdo((3 << 30) | (1 << 28))                           # APDO type 1 not defined in PD 3.0 SPR

    def test_fixed_rdo(self):
        r = mb.encode_rdo(1, mb.PDO_FIXED, usb_comms_capable=1, no_usb_suspend=1,
                          operating_current_ma=3000, limit_current_ma=3000)
        self.assertEqual(r, (1 << 28) | (1 << 25) | (1 << 24) | (300 << 10) | 300)
        d = mb.decode_rdo(r, mb.PDO_FIXED)
        self.assertEqual((d["object_position"], d["operating_current_ma"], d["limit_current_ma"], d["give_back"]),
                         (1, 3000, 3000, 0))

    def test_battery_and_pps_rdo(self):
        r = mb.encode_rdo(3, mb.PDO_BATTERY, give_back=1, operating_power_mw=15000, limit_power_mw=5000)
        self.assertEqual(mb.decode_rdo(r, mb.PDO_BATTERY)["limit_power_mw"], 5000)
        r = mb.encode_rdo(2, mb.PDO_PPS, output_voltage_mv=9000, operating_current_ma=2000)
        self.assertEqual(r, 0x20038428)
        d = mb.decode_rdo(r, mb.PDO_PPS)
        self.assertEqual((d["object_position"], d["output_voltage_mv"], d["operating_current_ma"]), (2, 9000, 2000))

    def test_rdo_flags_round_trip(self):
        r = mb.encode_rdo(7, mb.PDO_VARIABLE, 1, 1, 1, 1, 1, operating_current_ma=10, limit_current_ma=10230)
        d = mb.decode_rdo(r, mb.PDO_VARIABLE)
        self.assertEqual([d[k] for k in ("give_back", "capability_mismatch", "usb_comms_capable",
                                         "no_usb_suspend", "unchunked_ext_msg")], [1] * 5)
        self.assertEqual((d["object_position"], d["limit_current_ma"]), (7, 10230))

    def test_rejects_invalid_values(self):
        bad = [lambda: mb.encode_rdo(0, mb.PDO_FIXED, operating_current_ma=0, limit_current_ma=0),
               lambda: mb.encode_rdo(8, mb.PDO_FIXED, operating_current_ma=0, limit_current_ma=0),
               lambda: mb.encode_rdo(1, mb.PDO_FIXED, operating_current_ma=15, limit_current_ma=0),     # not 10 mA steps
               lambda: mb.encode_rdo(1, mb.PDO_FIXED, operating_current_ma=10240, limit_current_ma=0),  # > 10 bits
               lambda: mb.encode_rdo(1, mb.PDO_PPS, output_voltage_mv=9010, operating_current_ma=50),
               lambda: mb.encode_rdo(1, "unknown"),
               lambda: mb.encode_pdo({"type": mb.PDO_FIXED, "voltage_mv": 5025, "max_current_ma": 100}),
               lambda: mb.decode_rdo(0, mb.PDO_FIXED),                                                    # position 0
               lambda: mb.decode_rdo(1 << 28, "unknown"),
               lambda: mb.decode_pdo(1 << 32)]
        for f in bad:
            with self.assertRaises(mb.PdError):
                f()

    def test_every_fixed_pdo_round_trips(self):
        for volt in range(0, 1024, 37):
            for cur in range(0, 1024, 41):
                raw = (volt << 10) | cur
                self.assertEqual(mb.encode_pdo(mb.decode_pdo(raw)), raw)


if __name__ == "__main__":
    unittest.main()
