"""Generates qmi_vectors.inc for tests/c/test_qmi_tlv.c (run by tests/c/run_c_tests.py).

Every vector is produced and judged by the Python reference codec modem_5g_research/qmi_codec.py:
the hand-written layouts from tests/test_qmi_codec.py, the same seeded random round trips
(seed 0x5323), their truncations and random single-byte corruptions. The C codec
(drivers/s23modem/qmi_tlv.h) must accept exactly what Python accepts, decode the same fields and
re-encode the identical bytes.
"""
import importlib.util
import os
import random

KIND_TLVS, KIND_SDU, KIND_QMUX, KIND_FRAME = 0, 1, 2, 3


def _load_codec(root):
    path = os.path.join(root, "modem_5g_research", "qmi_codec.py")
    spec = importlib.util.spec_from_file_location("s23_qmi_codec_ref", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class _Vectors:
    def __init__(self, q):
        self.q = q
        self.data = bytearray()
        self.vecs = []
        self.tlvs = []

    def _blob(self, b):
        off = self.data.find(b) if len(b) >= 8 else -1   # reuse identical bytes (e.g. truncation prefixes)
        if off < 0:
            off = len(self.data)
            self.data += b
        return off

    def _tlv_rows(self, base, tlv_bytes, decoded):
        first = len(self.tlvs)
        off = 0
        for t, v in decoded:
            self.tlvs.append((t, base + off + 3, len(v)))
            off += 3 + len(v)
        assert off == len(tlv_bytes)
        return first, len(decoded)

    def add(self, kind, b, svc=0):
        q = self.q
        b = bytes(b)
        off = self._blob(b)
        row = dict(kind=kind, svc=svc, ok=0, off=off, len=len(b), q_svc=0, q_client=0, q_from=0,
                   flags=0, txid=0, msg_id=0, tlv_first=0, tlv_count=0)
        try:
            if kind == KIND_TLVS:
                d = q.decode_tlvs(b)
                row["tlv_first"], row["tlv_count"] = self._tlv_rows(off, b, d)
            elif kind == KIND_SDU:
                m = q.decode_message(svc, b)
                hdr = q.CTL_HEADER_LEN if svc == q.QMI_SVC_CTL else q.SVC_HEADER_LEN
                row.update(flags=m["flags"], txid=m["txid"], msg_id=m["msg_id"])
                row["tlv_first"], row["tlv_count"] = self._tlv_rows(off + hdr, b[hdr:], m["tlvs"])
            elif kind == KIND_QMUX:
                d = q.decode_qmux(b)
                row.update(q_svc=d["service"], q_client=d["client_id"], q_from=int(d["from_service"]))
            else:
                m = q.decode_frame(b)
                hdr = q.QMUX_HEADER_LEN + (q.CTL_HEADER_LEN if m["service"] == q.QMI_SVC_CTL else q.SVC_HEADER_LEN)
                row.update(q_svc=m["service"], q_client=m["client_id"], q_from=int(m["from_service"]),
                           flags=m["flags"], txid=m["txid"], msg_id=m["msg_id"])
                row["tlv_first"], row["tlv_count"] = self._tlv_rows(off + hdr, b[hdr:], m["tlvs"])
            row["ok"] = 1
        except q.QmiError:
            row["ok"] = 0
        self.vecs.append(row)
        return row["ok"]


def _build(q):
    v = _Vectors(q)
    # tests/test_qmi_codec.py hand-written layouts
    v.add(KIND_TLVS, q.encode_tlvs([(0x01, b"\xAA\xBB"), (0x10, b"")]))
    v.add(KIND_TLVS, q.encode_tlvs([(0x10, b"a"), (0x01, b""), (0x10, b"bb"), (0xFF, bytes(range(256)))]))
    raw = q.encode_tlvs([(0x01, b"\x01\x02\x03")])
    for cut in range(0, len(raw)):
        v.add(KIND_TLVS, raw[:cut])
    v.add(KIND_TLVS, q.encode_tlvs([(q.RESULT_TLV, bytes.fromhex("01001A00"))]))
    v.add(KIND_SDU, q.encode_message(q.QMI_SVC_CTL, q.CTL_REQUEST, 1, 0x0022, [(0x01, b"\x03")]), q.QMI_SVC_CTL)
    v.add(KIND_SDU, q.encode_message(0x03, q.SVC_REQUEST, 0x1234, 0x0024), 0x03)
    sdu = q.encode_message(0x03, q.SVC_RESPONSE, 7, 0x24, [(0x02, bytes(4))])
    for bad in (sdu, sdu[:-1], sdu + b"\x00", sdu[:6]):
        v.add(KIND_SDU, bad, 0x03)
    f = q.encode_frame(q.QMI_SVC_CTL, 0, q.CTL_REQUEST, 1, 0x0022, [(0x01, b"\x03")])
    v.add(KIND_FRAME, f)
    v.add(KIND_FRAME, q.encode_frame(0x03, 2, q.SVC_INDICATION, 0, 0x004E, [(0x01, b"\x01"), (0x11, b"\x00\x05")],
                                     from_service=True))
    f = q.encode_frame(0x03, 1, 0, 1, 0x20)
    bad_flags = bytearray(f)
    bad_flags[3] = 0x40
    for b in (f, b"", f[:5], b"\x02" + f[1:], f + b"\x00", f[:-1], bytes(bad_flags)):
        v.add(KIND_QMUX, b)
        v.add(KIND_FRAME, b)

    # the same seeded random round trips as test_random_round_trips, plus truncations / corruptions
    rng = random.Random(0x5323)
    mut = random.Random(0x5324)
    for _ in range(500):
        svc = rng.choice([0, 1, 2, 3, 0x0B, 0xE1])
        tlvs = [(rng.randrange(256), bytes(rng.randrange(256) for _ in range(rng.randrange(40))))
                for _ in range(rng.randrange(6))]
        txid = rng.randrange(256 if svc == 0 else 65536)
        f = q.encode_frame(svc, rng.randrange(256), rng.choice([0, 1, 2, 4]), txid, rng.randrange(65536), tlvs,
                           from_service=rng.random() < 0.5)
        assert v.add(KIND_FRAME, f) == 1
        for cut in (1, 3, len(f) // 2):
            assert v.add(KIND_FRAME, f[:-cut]) == 0
        for _ in range(2):
            b = bytearray(f)
            i = mut.randrange(len(b))
            b[i] ^= 1 << mut.randrange(8)
            v.add(KIND_FRAME, bytes(b))
        sdu_off = q.QMUX_HEADER_LEN
        v.add(KIND_SDU, f[sdu_off:], svc)
        v.add(KIND_TLVS, f[sdu_off + (q.CTL_HEADER_LEN if svc == 0 else q.SVC_HEADER_LEN):])
    return v


def extract(outdir, h):
    q = _load_codec(h["ROOT"])
    v = _build(q)
    lines = ["/* generated by tests/c/extract_qmi_vectors.py from modem_5g_research/qmi_codec.py -- do not edit */",
             f"#define QV_N_VECS {len(v.vecs)}U", f"#define QV_N_TLVS {len(v.tlvs)}U",
             f"#define QV_N_OK {sum(r['ok'] for r in v.vecs)}U",
             "static const uint8_t qv_data[] = {"]
    for i in range(0, len(v.data), 24):
        lines.append("    " + ",".join(f"0x{x:02X}" for x in v.data[i:i + 24]) + ",")
    lines.append("    0x00 };")
    lines.append("static const qv_tlv_t qv_tlvs[] = {")
    for t, off, n in v.tlvs:
        lines.append(f"    {{0x{t:02X}U, {off}U, {n}U}},")
    lines.append("    {0U, 0U, 0U} };")
    lines.append("static const qv_vec_t qv_vecs[] = {")
    for r in v.vecs:
        lines.append("    {{{kind}U, 0x{svc:02X}U, {ok}U, {off}U, {len}U, 0x{q_svc:02X}U, 0x{q_client:02X}U, {q_from}U, "
                     "0x{flags:02X}U, 0x{txid:04X}U, 0x{msg_id:04X}U, {tlv_first}U, {tlv_count}U}},".format(**r))
    lines.append("};")
    with open(os.path.join(outdir, "qmi_vectors.inc"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
