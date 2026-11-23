"""modem_5g_research/qmi_codec.py: QMUX framing, CTL/service SDU headers and TLVs (no hardware)."""
import os
import random
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "modem_5g_research"))
import qmi_codec as q  # noqa: E402


class TlvTest(unittest.TestCase):
    def test_encoding_layout(self):
        self.assertEqual(q.encode_tlvs([(0x01, b"\xAA\xBB"), (0x10, b"")]), bytes.fromhex("010200AABB 100000"))

    def test_round_trip_keeps_order_and_duplicates(self):
        tlvs = [(0x10, b"a"), (0x01, b""), (0x10, b"bb"), (0xFF, bytes(range(256)))]
        self.assertEqual(q.decode_tlvs(q.encode_tlvs(tlvs)), tlvs)

    def test_truncation_is_detected(self):
        raw = q.encode_tlvs([(0x01, b"\x01\x02\x03")])
        for cut in range(1, len(raw)):
            with self.assertRaises(q.QmiError):
                q.decode_tlvs(raw[:cut])
        self.assertEqual(q.decode_tlvs(b""), [])

    def test_length_limits(self):
        q.encode_tlvs([(0x01, bytes(0xFFFF - 3))])
        with self.assertRaises(q.QmiError):
            q.encode_tlvs([(0x01, bytes(0x10000))])
        with self.assertRaises(q.QmiError):
            q.encode_tlvs([(0x01, bytes(0x8000)), (0x02, bytes(0x8000))])   # block > u16
        with self.assertRaises(q.QmiError):
            q.encode_tlvs([(0x100, b"")])

    def test_result_tlv(self):
        tlvs = q.decode_tlvs(q.encode_tlvs([(q.RESULT_TLV, bytes.fromhex("01001A00"))]))
        self.assertEqual(q.decode_result(q.find_tlv(tlvs, q.RESULT_TLV)), (1, 0x1A))
        self.assertIsNone(q.find_tlv(tlvs, 0x10))
        with self.assertRaises(q.QmiError):
            q.decode_result(b"\x00\x00")


class MessageTest(unittest.TestCase):
    def test_ctl_header_layout(self):
        # CTL "get client id" request (msg 0x0022) for service 0x03 (NAS), txid 1
        sdu = q.encode_message(q.QMI_SVC_CTL, q.CTL_REQUEST, 1, 0x0022, [(0x01, b"\x03")])
        self.assertEqual(sdu, bytes.fromhex("00 01 2200 0400 010100 03"))
        m = q.decode_message(q.QMI_SVC_CTL, sdu)
        self.assertEqual((m["flags"], m["txid"], m["msg_id"], m["tlvs"]), (0, 1, 0x22, [(1, b"\x03")]))

    def test_service_header_has_u16_txid(self):
        sdu = q.encode_message(0x03, q.SVC_REQUEST, 0x1234, 0x0024)
        self.assertEqual(sdu, bytes.fromhex("00 3412 2400 0000"))
        self.assertEqual(q.decode_message(0x03, sdu)["txid"], 0x1234)
        with self.assertRaises(q.QmiError):
            q.encode_message(q.QMI_SVC_CTL, 0, 0x100, 0x22)        # CTL txid is u8
        with self.assertRaises(q.QmiError):
            q.encode_message(0x03, 0, 0x10000, 0x22)

    def test_tlv_length_must_match(self):
        sdu = bytearray(q.encode_message(0x03, q.SVC_RESPONSE, 7, 0x24, [(0x02, bytes(4))]))
        for bad in (sdu[:-1], sdu + b"\x00"):
            with self.assertRaises(q.QmiError):
                q.decode_message(0x03, bad)
        with self.assertRaises(q.QmiError):
            q.decode_message(0x03, sdu[:6])                        # shorter than the service header


class QmuxTest(unittest.TestCase):
    def test_frame_layout(self):
        f = q.encode_frame(q.QMI_SVC_CTL, 0, q.CTL_REQUEST, 1, 0x0022, [(0x01, b"\x03")])
        # SDU = 6-byte CTL header + 4-byte TLV; QMUX length = 5 header bytes after the I/F type + 10
        self.assertEqual(f[:6], bytes.fromhex("01 0F00 00 00 00"))
        self.assertEqual(len(f), 1 + 0x0F)

    def test_round_trip_service_indication(self):
        f = q.encode_frame(0x03, 2, q.SVC_INDICATION, 0, 0x004E, [(0x01, b"\x01"), (0x11, b"\x00\x05")], from_service=True)
        m = q.decode_frame(f)
        self.assertEqual((m["service"], m["client_id"], m["from_service"], m["flags"], m["msg_id"]),
                         (0x03, 2, True, q.SVC_INDICATION, 0x004E))
        self.assertEqual(m["tlvs"], [(0x01, b"\x01"), (0x11, b"\x00\x05")])

    def test_qmux_validation(self):
        f = q.encode_frame(0x03, 1, 0, 1, 0x20)
        for bad in (b"", f[:5], b"\x02" + f[1:], f + b"\x00", f[:-1]):
            with self.assertRaises(q.QmiError):
                q.decode_qmux(bad)
        bad_flags = bytearray(f)
        bad_flags[3] = 0x40
        with self.assertRaises(q.QmiError):
            q.decode_qmux(bytes(bad_flags))

    def test_random_round_trips(self):
        rng = random.Random(0x5323)
        for _ in range(500):
            svc = rng.choice([0, 1, 2, 3, 0x0B, 0xE1])
            tlvs = [(rng.randrange(256), bytes(rng.randrange(256) for _ in range(rng.randrange(40))))
                    for _ in range(rng.randrange(6))]
            txid = rng.randrange(256 if svc == 0 else 65536)
            f = q.encode_frame(svc, rng.randrange(256), rng.choice([0, 1, 2, 4]), txid, rng.randrange(65536), tlvs,
                               from_service=rng.random() < 0.5)
            m = q.decode_frame(f)
            self.assertEqual((m["service"], m["txid"], m["tlvs"]), (svc, txid, tlvs))
            for cut in (1, 3, len(f) // 2):
                with self.assertRaises(q.QmiError):
                    q.decode_frame(f[:-cut])


if __name__ == "__main__":
    unittest.main()
