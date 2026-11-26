#!/usr/bin/env python3
"""Generate tests/c/s23usbc_vectors.h from the Python reference usb_research/max77705_mailbox.py.

The C headers drivers/s23usbc/s23usbc_opcode.h and s23usbc_pd.h are ports of that reference. The C
host tests (tests/c/test_s23usbc_opcode.c, test_s23usbc_pd.c) run the C code on the vectors below and
compare against the outputs the Python reference produced, so both implementations agree byte for
byte. tests/test_s23usbc_vectors.py fails if the committed header is not what this script renders.

  python usb_research/gen_s23usbc_c_vectors.py            # rewrite tests/c/s23usbc_vectors.h
  python usb_research/gen_s23usbc_c_vectors.py --check    # exit 1 if it is out of date
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import max77705_mailbox as mb  # noqa: E402

OUT = os.path.normpath(os.path.join(HERE, "..", "tests", "c", "s23usbc_vectors.h"))

C_TYPE = {mb.PDO_FIXED: "S23PD_PDO_FIXED", mb.PDO_BATTERY: "S23PD_PDO_BATTERY",
          mb.PDO_VARIABLE: "S23PD_PDO_VARIABLE", mb.PDO_PPS: "S23PD_PDO_PPS", "unknown": "9U"}
PDO_FIELDS = ["dual_role_power", "usb_suspend_supported", "unconstrained_power", "usb_comms_capable",
              "dual_role_data", "unchunked_ext_msg", "peak_current", "pps_power_limited",
              "voltage_mv", "max_voltage_mv", "min_voltage_mv", "max_current_ma", "max_power_mw"]
RDO_FLAGS = ["give_back", "capability_mismatch", "usb_comms_capable", "no_usb_suspend", "unchunked_ext_msg"]
RDO_VALUES = ["operating_current_ma", "limit_current_ma", "operating_power_mw", "limit_power_mw",
              "output_voltage_mv"]


def lcg(seed):
    while True:
        seed = (seed * 1103515245 + 12345) & 0xFFFFFFFF
        yield seed


def hexbytes(b, width):
    """C initializer for a uint8_t[width] array (trailing elements are zero-initialised)."""
    b = list(b)
    assert len(b) <= width
    return "{" + (", ".join(f"0x{x:02X}" for x in b) if b else "0") + "}"


def fnv1a(data):
    h = 0x811C9DC5
    for x in data:
        h = ((h ^ x) * 0x01000193) & 0xFFFFFFFF
    return h


# ---------------------------------------------------------------------------------------------
def write_vectors():
    cases = [(0x25, b"\x01", 0x00), (0x55, b"", 0x00), (0x30, bytes(range(32)), 0x00),
             (0x30, bytes(31), 0x00), (0x56, b"\x07", 0xFF), (0x37, b"\x01", 0x00), (0x37, b"\x02", 0x00),
             (0x37, b"\x04", 0x00), (0x56, b"\x07", 0x00), (0xFF, bytes(range(0xE0, 0xF0)), 0x5A),
             (0x00, b"\xAA" * 30, 0x00),
             (0x100, b"", 0x00), (0x30, bytes(33), 0x00), (0x30, b"", 0x100)]
    rows = []
    for op, data, commit in cases:
        try:
            w = mb.encode_write(op, data, commit_byte=commit)
            ok = 1
        except mb.MailboxError:
            w, ok = [], 0
        reg0, buf0 = (w[0][0], w[0][1]) if w else (0, b"")
        reg1, buf1 = (w[1][0], w[1][1]) if len(w) > 1 else (0, b"")
        rows.append(f"    {{0x{op:03X}U, 0x{commit:03X}U, {len(data)}U, {hexbytes(data, 33)}, {ok}, {len(w)}U,\n"
                    f"     0x{reg0:02X}U, {len(buf0)}U, {hexbytes(buf0, 33)}, 0x{reg1:02X}U, {len(buf1)}U, "
                    f"0x{(buf1[0] if buf1 else 0):02X}U}},")
    return ("static const S23V_WRITE k_write_vectors[] = {\n" + "\n".join(rows) + "\n};\n")


def read_vectors():
    rows = []
    for n in (0, 1, 2, 31, 32, 33, 255):
        try:
            reg, cnt = mb.read_request(n)
            ok = 1
        except mb.MailboxError:
            reg, cnt, ok = 0, 0, 0
        rows.append(f"    {{{n}U, {ok}, 0x{reg:02X}U, {cnt}U}},")
    out = "static const S23V_READREQ k_readreq_vectors[] = {\n" + "\n".join(rows) + "\n};\n\n"
    cases = [(b"\x25\x10\x20", 0x25, 2), (b"\x55", 0x55, 0), (b"\x24\x10\x20", 0x25, 2), (b"\x25\x10", 0x25, 2),
             (b"\x25\x10\x20\x30", 0x25, 2), (bytes([0x31]) + bytes(range(32)), 0x31, 32),
             (bytes([0x31]) + bytes(range(33)), 0x31, 33), (b"", 0x25, 0), (b"\x4B\x01\x02", 0x4B, 2)]
    rows = []
    for raw, op, n in cases:
        try:
            pl = mb.decode_response(raw, op, n)
            ok = 1
        except mb.MailboxError:
            pl, ok = b"", 0
        rows.append(f"    {{{hexbytes(raw, 34)}, {len(raw)}U, 0x{op:02X}U, {n}U, {ok}, {len(pl)}U}},")
    out += "static const S23V_RESP k_resp_vectors[] = {\n" + "\n".join(rows) + "\n};\n"
    return out


def status_digests():
    dec = [("CC_STATUS0", mb.decode_cc_status0, ["CCPinStat", "CCIStat", "CCVcnStat", "CCStat"]),
           ("CC_STATUS1", mb.decode_cc_status1, ["CCSBUSHORT", "VCONNOCP", "VCONNSC", "VSAFE0V", "AttachSrcErr",
                                                 "ConnStat", "Altmode"]),
           ("BC_STATUS", mb.decode_bc_status, ["VBUSDet", "PrChgTyp", "DCDTmo", "ChgTyp"]),
           ("PD_STATUS1", mb.decode_pd_status1, ["PD_DataRole", "PD_PSRDY", "FCT_ID"]),
           ("USBC_STATUS1", mb.decode_usbc_status1, ["VBADC", "UIDADC"])]
    out = ["/* FNV-1a over the decoded fields (in the order listed) of every register value 0..255 */"]
    for name, f, keys in dec:
        data = []
        for v in range(256):
            d = f(v)
            data += [d[k] for k in keys]
        out.append(f"#define S23V_DIGEST_{name} 0x{fnv1a(data):08X}U  /* {', '.join(keys)} */")
    return "\n".join(out) + "\n"


def pdo_row(raw, d, ok, reenc):
    vals = [str(d.get(k, 0)) + "U" for k in PDO_FIELDS]
    t = C_TYPE[d["type"]] if ok else "0U"
    return f"    {{0x{raw:08X}U, {ok}, {t}, {', '.join(vals)}, 0x{reenc:08X}U}},"


def pdo_vectors():
    raws = [0x0801912C, (0x3F << 24) | (2 << 20) | (180 << 10) | 200, (2 << 30) | (240 << 20) | (100 << 10) | 150,
            (1 << 30) | (420 << 20) | (60 << 10) | 60, (3 << 30) | (1 << 27) | (110 << 17) | (33 << 8) | 60,
            (3 << 30) | (1 << 28), (3 << 30) | (2 << 28), (3 << 30) | (3 << 28), 0, 0xFFFFFFFF, 0xCFFFFFFF,
            0x3FFFFFFF, 0x7FFFFFFF, 0xBFFFFFFF, 0x0002D0C8, 0x0002D12C, 0x0004B12C, 0x000640E1,
            0xC0000000, 0x00C00000]
    g = lcg(0x5323)
    raws += [next(g) for _ in range(64)]
    rows = []
    for raw in raws:
        try:
            d = mb.decode_pdo(raw)
            ok, reenc = 1, mb.encode_pdo(d)
        except mb.PdError:
            d, ok, reenc = {"type": None}, 0, 0
        rows.append(pdo_row(raw, d, ok, reenc))
    out = "static const S23V_PDO k_pdo_decode_vectors[] = {\n" + "\n".join(rows) + "\n};\n\n"

    enc = [{"type": mb.PDO_FIXED, "voltage_mv": 5025, "max_current_ma": 100},
           {"type": mb.PDO_FIXED, "voltage_mv": 5000, "max_current_ma": 10240},
           {"type": mb.PDO_FIXED, "voltage_mv": 51150, "max_current_ma": 10230, "peak_current": 7,
            "dual_role_power": 3},
           {"type": mb.PDO_FIXED, "voltage_mv": 51200, "max_current_ma": 0},
           {"type": mb.PDO_BATTERY, "max_voltage_mv": 21000, "min_voltage_mv": 3000, "max_power_mw": 255750},
           {"type": mb.PDO_BATTERY, "max_voltage_mv": 21000, "min_voltage_mv": 3000, "max_power_mw": 256000},
           {"type": mb.PDO_BATTERY, "max_voltage_mv": 21000, "min_voltage_mv": 3000, "max_power_mw": 15100},
           {"type": mb.PDO_VARIABLE, "max_voltage_mv": 12000, "min_voltage_mv": 5010, "max_current_ma": 1500},
           {"type": mb.PDO_VARIABLE, "max_voltage_mv": 12000, "min_voltage_mv": 5000, "max_current_ma": 1500},
           {"type": mb.PDO_PPS, "max_voltage_mv": 25500, "min_voltage_mv": 0, "max_current_ma": 6350,
            "pps_power_limited": 1},
           {"type": mb.PDO_PPS, "max_voltage_mv": 25600, "min_voltage_mv": 3300, "max_current_ma": 3000},
           {"type": mb.PDO_PPS, "max_voltage_mv": 11000, "min_voltage_mv": 3350, "max_current_ma": 3000},
           {"type": mb.PDO_PPS, "max_voltage_mv": 11000, "min_voltage_mv": 3300, "max_current_ma": 6400},
           {"type": mb.PDO_PPS, "max_voltage_mv": 11000, "min_voltage_mv": 3300, "max_current_ma": 3025},
           {"type": "unknown"}]
    rows = []
    for d in enc:
        try:
            raw, ok = mb.encode_pdo(d), 1
        except (mb.PdError, KeyError):
            raw, ok = 0, 0
        vals = [str(d.get(k, 0)) + "U" for k in PDO_FIELDS]
        rows.append(f"    {{{C_TYPE[d['type']]}, {', '.join(vals)}, {ok}, 0x{raw:08X}U}},")
    out += "static const S23V_PDO_ENC k_pdo_encode_vectors[] = {\n" + "\n".join(rows) + "\n};\n"
    return out


def rdo_vectors():
    F, B, V, P = mb.PDO_FIXED, mb.PDO_BATTERY, mb.PDO_VARIABLE, mb.PDO_PPS
    cases = [  # (position, type, flags[5], values{})
        (1, F, [0, 0, 1, 1, 0], {"operating_current_ma": 3000, "limit_current_ma": 3000}),
        (3, B, [1, 0, 0, 0, 0], {"operating_power_mw": 15000, "limit_power_mw": 5000}),
        (2, P, [0, 0, 0, 0, 0], {"output_voltage_mv": 9000, "operating_current_ma": 2000}),
        (7, V, [1, 1, 1, 1, 1], {"operating_current_ma": 10, "limit_current_ma": 10230}),
        (0, F, [0, 0, 0, 0, 0], {"operating_current_ma": 0, "limit_current_ma": 0}),
        (8, F, [0, 0, 0, 0, 0], {"operating_current_ma": 0, "limit_current_ma": 0}),
        (1, F, [0, 0, 0, 0, 0], {"operating_current_ma": 15, "limit_current_ma": 0}),
        (1, F, [0, 0, 0, 0, 0], {"operating_current_ma": 10240, "limit_current_ma": 0}),
        (1, P, [0, 0, 0, 0, 0], {"output_voltage_mv": 9010, "operating_current_ma": 50}),
        (1, P, [0, 0, 0, 0, 0], {"output_voltage_mv": 40940, "operating_current_ma": 6350}),
        (1, P, [0, 0, 0, 0, 0], {"output_voltage_mv": 40960, "operating_current_ma": 50}),
        (1, P, [0, 0, 0, 0, 0], {"output_voltage_mv": 5000, "operating_current_ma": 6400}),
        (4, B, [0, 0, 0, 0, 0], {"operating_power_mw": 255750, "limit_power_mw": 0}),
        (4, B, [0, 0, 0, 0, 0], {"operating_power_mw": 256000, "limit_power_mw": 0}),
        (2, F, [3, 2, 0, 0, 0], {"operating_current_ma": 2000, "limit_current_ma": 2000}),   # flags & 1
        (1, "unknown", [0, 0, 0, 0, 0], {}),
    ]
    rows = []
    for pos, t, fl, vals in cases:
        try:
            raw = mb.encode_rdo(pos, t, *fl, **vals)
            ok = 1
        except (mb.PdError, KeyError):
            raw, ok = 0, 0
        v = [str(vals.get(k, 0)) + "U" for k in RDO_VALUES]
        rows.append(f"    {{{pos}U, {C_TYPE[t]}, {{{', '.join(str(x) + 'U' for x in fl)}}}, {', '.join(v)}, "
                    f"{ok}, 0x{raw:08X}U}},")
    out = "static const S23V_RDO_ENC k_rdo_encode_vectors[] = {\n" + "\n".join(rows) + "\n};\n\n"

    dec = [((1 << 28) | (1 << 25) | (1 << 24) | (300 << 10) | 300, F), (0x20038428, P), (0, F), (1 << 28, "unknown"),
           (0xFFFFFFFF, F), (0xFFFFFFFF, B), (0xFFFFFFFF, V), (0xFFFFFFFF, P), (0x0FFFFFFF, P), (0xF0000000, B)]
    g = lcg(0x0D0)
    for i in range(32):
        dec.append((next(g), [F, B, V, P][i % 4]))
    rows = []
    for raw, t in dec:
        try:
            d = mb.decode_rdo(raw, t)
            ok = 1
        except mb.PdError:
            d, ok = {}, 0
        fl = [str(d.get(k, 0)) + "U" for k in RDO_FLAGS]
        v = [str(d.get(k, 0)) + "U" for k in RDO_VALUES]
        rows.append(f"    {{0x{raw:08X}U, {C_TYPE[t]}, {ok}, {d.get('object_position', 0)}U, {{{', '.join(fl)}}}, "
                    f"{', '.join(v)}}},")
    out += "static const S23V_RDO_DEC k_rdo_decode_vectors[] = {\n" + "\n".join(rows) + "\n};\n"
    return out


HEADER = """/*
 * GENERATED by usb_research/gen_s23usbc_c_vectors.py from usb_research/max77705_mailbox.py.
 * Do not edit by hand: regenerate with  python usb_research/gen_s23usbc_c_vectors.py
 * (tests/test_s23usbc_vectors.py fails when this file is stale).
 */
#ifndef S23USBC_VECTORS_H
#define S23USBC_VECTORS_H

#include <stdint.h>

typedef struct { unsigned opcode, commit; uint8_t data_len; uint8_t data[33]; int ok; uint8_t nwrites;
                 uint8_t reg0, len0; uint8_t buf0[33]; uint8_t reg1, len1, commit_val; } S23V_WRITE;
typedef struct { unsigned read_len; int ok; uint8_t reg; unsigned count; } S23V_READREQ;
typedef struct { uint8_t raw[34]; uint8_t raw_len; unsigned opcode; unsigned read_len; int ok;
                 unsigned payload_len; } S23V_RESP;
typedef struct { uint32_t raw; int ok; unsigned type;
                 unsigned dual_role_power, usb_suspend_supported, unconstrained_power, usb_comms_capable,
                          dual_role_data, unchunked_ext_msg, peak_current, pps_power_limited;
                 uint32_t voltage_mv, max_voltage_mv, min_voltage_mv, max_current_ma, max_power_mw;
                 uint32_t reencoded; } S23V_PDO;
typedef struct { unsigned type;
                 unsigned dual_role_power, usb_suspend_supported, unconstrained_power, usb_comms_capable,
                          dual_role_data, unchunked_ext_msg, peak_current, pps_power_limited;
                 uint32_t voltage_mv, max_voltage_mv, min_voltage_mv, max_current_ma, max_power_mw;
                 int ok; uint32_t raw; } S23V_PDO_ENC;
typedef struct { unsigned position, type; unsigned flags[5];
                 uint32_t operating_current_ma, limit_current_ma, operating_power_mw, limit_power_mw,
                          output_voltage_mv;
                 int ok; uint32_t raw; } S23V_RDO_ENC;
typedef struct { uint32_t raw; unsigned type; int ok; unsigned position; unsigned flags[5];
                 uint32_t operating_current_ma, limit_current_ma, operating_power_mw, limit_power_mw,
                          output_voltage_mv; } S23V_RDO_DEC;

"""


def render():
    # Each test defines S23V_WANT_OPCODE and/or S23V_WANT_PD before including, so it only instantiates
    # the tables it uses (unused static const tables would trip -Wunused-const-variable under -Werror).
    parts = [HEADER, "/* ---- OPCODE mailbox (encode_write / read_request / decode_response) ---- */\n",
             "#ifdef S23V_WANT_OPCODE\n", write_vectors(), "\n", read_vectors(), "\n", status_digests(),
             "#endif /* S23V_WANT_OPCODE */\n\n",
             "/* ---- USB PD 3.0 PDO / RDO (decode_pdo / encode_pdo / encode_rdo / decode_rdo) ---- */\n",
             "#ifdef S23V_WANT_PD\n#include \"s23usbc/s23usbc_pd.h\"\n\n", pdo_vectors(), "\n", rdo_vectors(),
             "#endif /* S23V_WANT_PD */\n", "\n#endif /* S23USBC_VECTORS_H */\n"]
    return "".join(parts)


def main(argv):
    text = render()
    if "--check" in argv:
        try:
            with open(OUT, encoding="utf-8") as f:
                cur = f.read().replace("\r\n", "\n")
        except OSError:
            cur = None
        if cur != text:
            print(f"{OUT} is out of date; run python usb_research/gen_s23usbc_c_vectors.py")
            return 1
        print(f"{OUT} is up to date")
        return 0
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    print(f"wrote {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
