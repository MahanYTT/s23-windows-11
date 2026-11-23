"""Reference codec for QMI messages over QMUX (pure byte logic, no I/O, not used by any driver).

Building block for a future user-mode modem daemon (s23modemd.exe); tested in tests/test_qmi_codec.py.
All multi-byte fields are little-endian.

QMUX frame:   I/F type (u8, 0x01) | length (u16, everything after the I/F type byte) |
              control flags (u8, 0x00 = from control point, 0x80 = from service) | service (u8) |
              client id (u8) | SDU
CTL SDU   (service 0x00): flags (u8) | transaction id (u8)  | message id (u16) | TLV length (u16) | TLVs
Service SDU (others)     : flags (u8) | transaction id (u16) | message id (u16) | TLV length (u16) | TLVs
TLV:                       type (u8) | length (u16) | value

SDU flags: CTL uses 0x00 request, 0x01 response, 0x02 indication;
           services use 0x00 request, 0x02 response, 0x04 indication.
The standard result TLV (type 0x02) holds result (u16, 0 = success) and error (u16).
"""
import struct

QMUX_IF_TYPE = 0x01
QMUX_FROM_CONTROL_POINT = 0x00
QMUX_FROM_SERVICE = 0x80
QMI_SVC_CTL = 0x00

CTL_REQUEST, CTL_RESPONSE, CTL_INDICATION = 0x00, 0x01, 0x02
SVC_REQUEST, SVC_RESPONSE, SVC_INDICATION = 0x00, 0x02, 0x04

RESULT_TLV = 0x02
QMUX_HEADER_LEN = 6          # if type + length + flags + service + client
CTL_HEADER_LEN = 6           # flags + txid(u8) + msgid + tlv length
SVC_HEADER_LEN = 7           # flags + txid(u16) + msgid + tlv length
TLV_HEADER_LEN = 3
MAX_U16 = 0xFFFF


class QmiError(ValueError):
    pass


def _check_u(v, bits, name):
    if not isinstance(v, int) or not 0 <= v < (1 << bits):
        raise QmiError(f"{name}={v!r} does not fit u{bits}")
    return v


# ---- TLVs ----

def encode_tlvs(tlvs):
    """[(type, bytes)] -> bytes. Order and duplicates are preserved exactly."""
    out = bytearray()
    for t, value in tlvs:
        value = bytes(value)
        _check_u(t, 8, "TLV type")
        if len(value) > MAX_U16:
            raise QmiError(f"TLV 0x{t:02X} value of {len(value)} bytes exceeds u16 length")
        out += struct.pack("<BH", t, len(value)) + value
    if len(out) > MAX_U16:
        raise QmiError("TLV block exceeds the u16 message length")
    return bytes(out)


def decode_tlvs(buf):
    """bytes -> [(type, bytes)]; raises QmiError on a truncated header or value."""
    buf = bytes(buf)
    out, off = [], 0
    while off < len(buf):
        if len(buf) - off < TLV_HEADER_LEN:
            raise QmiError(f"truncated TLV header at offset {off}")
        t, n = struct.unpack_from("<BH", buf, off)
        off += TLV_HEADER_LEN
        if len(buf) - off < n:
            raise QmiError(f"TLV 0x{t:02X} claims {n} bytes, only {len(buf) - off} left")
        out.append((t, buf[off:off + n]))
        off += n
    return out


def find_tlv(tlvs, t):
    """First value with type t, or None."""
    return next((v for tt, v in tlvs if tt == t), None)


def decode_result(value):
    """Result TLV (0x02) value -> (result, error)."""
    if value is None or len(value) != 4:
        raise QmiError("result TLV must be 4 bytes")
    return struct.unpack("<HH", value)


# ---- SDU (message) ----

def encode_message(service, flags, txid, msg_id, tlvs=()):
    """QMI SDU for a CTL (service 0) or regular service message."""
    _check_u(service, 8, "service")
    _check_u(flags, 8, "flags")
    _check_u(msg_id, 16, "message id")
    body = encode_tlvs(tlvs)
    if service == QMI_SVC_CTL:
        return struct.pack("<BBHH", flags, _check_u(txid, 8, "CTL transaction id"), msg_id, len(body)) + body
    return struct.pack("<BHHH", flags, _check_u(txid, 16, "transaction id"), msg_id, len(body)) + body


def decode_message(service, sdu):
    """SDU -> dict(flags, txid, msg_id, tlvs). The TLV length must match the SDU exactly."""
    sdu = bytes(sdu)
    hdr = CTL_HEADER_LEN if service == QMI_SVC_CTL else SVC_HEADER_LEN
    if len(sdu) < hdr:
        raise QmiError(f"SDU of {len(sdu)} bytes is shorter than the {hdr}-byte header")
    if service == QMI_SVC_CTL:
        flags, txid, msg_id, n = struct.unpack_from("<BBHH", sdu, 0)
    else:
        flags, txid, msg_id, n = struct.unpack_from("<BHHH", sdu, 0)
    if n != len(sdu) - hdr:
        raise QmiError(f"TLV length field {n} != {len(sdu) - hdr} bytes present")
    return {"flags": flags, "txid": txid, "msg_id": msg_id, "tlvs": decode_tlvs(sdu[hdr:])}


# ---- QMUX ----

def encode_qmux(service, client_id, sdu, from_service=False):
    sdu = bytes(sdu)
    _check_u(service, 8, "service")
    _check_u(client_id, 8, "client id")
    length = QMUX_HEADER_LEN - 1 + len(sdu)
    if length > MAX_U16:
        raise QmiError("QMUX frame exceeds the u16 length")
    flags = QMUX_FROM_SERVICE if from_service else QMUX_FROM_CONTROL_POINT
    return struct.pack("<BHBBB", QMUX_IF_TYPE, length, flags, service, client_id) + sdu


def decode_qmux(frame):
    """frame -> dict(service, client_id, from_service, sdu). Exact length match is required."""
    frame = bytes(frame)
    if len(frame) < QMUX_HEADER_LEN:
        raise QmiError(f"QMUX frame of {len(frame)} bytes is shorter than its header")
    if_type, length, flags, service, client_id = struct.unpack_from("<BHBBB", frame, 0)
    if if_type != QMUX_IF_TYPE:
        raise QmiError(f"I/F type 0x{if_type:02X}, expected 0x01")
    if length != len(frame) - 1:
        raise QmiError(f"QMUX length {length} != {len(frame) - 1} bytes present")
    if flags not in (QMUX_FROM_CONTROL_POINT, QMUX_FROM_SERVICE):
        raise QmiError(f"unknown QMUX control flags 0x{flags:02X}")
    return {"service": service, "client_id": client_id, "from_service": flags == QMUX_FROM_SERVICE,
            "sdu": frame[QMUX_HEADER_LEN:]}


def encode_frame(service, client_id, flags, txid, msg_id, tlvs=(), from_service=False):
    """Convenience: QMUX frame around one message."""
    return encode_qmux(service, client_id, encode_message(service, flags, txid, msg_id, tlvs), from_service)


def decode_frame(frame):
    q = decode_qmux(frame)
    m = decode_message(q["service"], q["sdu"])
    m.update(service=q["service"], client_id=q["client_id"], from_service=q["from_service"])
    return m
