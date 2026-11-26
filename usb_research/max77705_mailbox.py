"""Reference encoder/decoder for the MAX77705 USBC/PDIC OPCODE mailbox (I2C slave 0x25).

Pure byte-level logic derived from S23_USBC_ARCHITECTURE.md (section 2, "AP-to-MCU OPCODE Mailbox
Protocol" and the 0x25 register map). It performs no I/O and is not used by any driver; it exists so a
future s23usbc/s23pmic implementation can be checked against host unit tests
(tests/test_max77705_mailbox.py).

Only what the specification states is encoded here:
  * write:  [opcode, data...] starting at REG_OPCODE (0x21), aperture 0x21..0x41 inclusive;
            if the write does not reach 0x41, a separate write of REG_END_DATA (0x41) commits it.
            The value of that commit byte is NOT confirmed by the spec (it says to verify 0x00 against
            Samsung's max77705_usbc_opcode_write()), so it is a parameter with default 0x00.
  * read:   1 + read_length bytes from REG_OPCODE_READ (0x51), byte 0 echoes the opcode,
            response buffer 0x51..0x71 inclusive.
  * status registers: bit fields only. The meaning of individual field values (e.g. which CCStat value
    is UFP vs DFP) is not given by the spec and is therefore returned raw, not interpreted.
"""

SLAVE_ADDR = 0x25
REG_UIC_INT = 0x02
REG_CC_INT = 0x03
REG_PD_INT = 0x04
REG_BC_STATUS = 0x08
REG_CC_STATUS0 = 0x0A
REG_CC_STATUS1 = 0x0B
REG_PD_STATUS1 = 0x0D
REG_OPCODE = 0x21          # opcode byte, data follows at 0x22
REG_END_DATA = 0x41        # end of the write aperture / commit register
REG_OPCODE_READ = 0x51     # response: echoed opcode, payload follows
REG_OPCODE_READ_END = 0x71

MAX_WRITE_DATA = REG_END_DATA - REG_OPCODE                 # 32 data bytes (0x22..0x41)
MAX_READ_DATA = REG_OPCODE_READ_END - REG_OPCODE_READ       # 32 payload bytes (0x52..0x71)

UIC_INT_APCMDRESI = 1 << 7  # OPCODE response ready


class MailboxError(ValueError):
    pass


def encode_write(opcode, data=b"", commit_byte=0x00):
    """-> list of (register, bytes) I2C block writes to slave 0x25, in order."""
    if not 0 <= opcode <= 0xFF:
        raise MailboxError("opcode must be one byte")
    data = bytes(data)
    if len(data) > MAX_WRITE_DATA:
        raise MailboxError(f"payload of {len(data)} bytes exceeds the {MAX_WRITE_DATA}-byte mailbox")
    if not 0 <= commit_byte <= 0xFF:
        raise MailboxError("commit byte must be one byte")
    frame = bytes([opcode]) + data
    writes = [(REG_OPCODE, frame)]
    last_reg = REG_OPCODE + len(frame) - 1
    if last_reg < REG_END_DATA:
        writes.append((REG_END_DATA, bytes([commit_byte])))
    return writes


def read_request(read_length):
    """-> (register, byte count) of the bulk read that fetches a response with read_length payload bytes."""
    if not 0 <= read_length <= MAX_READ_DATA:
        raise MailboxError(f"read length must be 0..{MAX_READ_DATA}")
    return REG_OPCODE_READ, 1 + read_length


def decode_response(raw, opcode, read_length):
    """Validate a raw response read from 0x51 and return its payload."""
    raw = bytes(raw)
    _, expected = read_request(read_length)
    if len(raw) != expected:
        raise MailboxError(f"expected {expected} bytes, got {len(raw)}")
    if raw[0] != opcode:
        raise MailboxError(f"response echoes opcode 0x{raw[0]:02X}, expected 0x{opcode:02X}")
    return raw[1:]


def response_ready(uic_int):
    """REG_UIC_INT (0x02) bit 7 APCmdResI: an OPCODE response can be read."""
    return bool(uic_int & UIC_INT_APCMDRESI)


def _byte(v):
    if not 0 <= v <= 0xFF:
        raise MailboxError("register value must be one byte")
    return v


def decode_cc_status0(v):
    """REG_CC_STATUS0 (0x0A): CCPinStat[7:6], CCIStat[5:4], CCVcnStat[3], CCStat[2:0] (raw values)."""
    v = _byte(v)
    return {"CCPinStat": (v >> 6) & 3, "CCIStat": (v >> 4) & 3, "CCVcnStat": (v >> 3) & 1, "CCStat": v & 7}


def decode_cc_status1(v):
    """REG_CC_STATUS1 (0x0B): CCSBUSHORT[7:6], VCONNOCP[5], VCONNSC[4], VSAFE0V[3], AttachSrcErr[2],
    ConnStat[1], Altmode[0]."""
    v = _byte(v)
    return {"CCSBUSHORT": (v >> 6) & 3, "VCONNOCP": (v >> 5) & 1, "VCONNSC": (v >> 4) & 1, "VSAFE0V": (v >> 3) & 1,
            "AttachSrcErr": (v >> 2) & 1, "ConnStat": (v >> 1) & 1, "Altmode": v & 1}


def decode_bc_status(v):
    """REG_BC_STATUS (0x08): VBUSDet[7], PrChgTyp[5:3], DCDTmo[2], ChgTyp[1:0] (raw values)."""
    v = _byte(v)
    return {"VBUSDet": (v >> 7) & 1, "PrChgTyp": (v >> 3) & 7, "DCDTmo": (v >> 2) & 1, "ChgTyp": v & 3}


def decode_usbc_status1(v):
    """REG_USBC_STATUS1 (0x06): VBADC[7:4] (VBUS voltage bucket), UIDADC[2:0] (raw values)."""
    v = _byte(v)
    return {"VBADC": (v >> 4) & 0xF, "UIDADC": v & 7}


def decode_pd_status1(v):
    """REG_PD_STATUS1 (0x0D): PD_DataRole[7], PD_PSRDY[4], FCT_ID[3:0]."""
    v = _byte(v)
    return {"PD_DataRole": (v >> 7) & 1, "PD_PSRDY": (v >> 4) & 1, "FCT_ID": v & 0xF}


# ---------------------------------------------------------------------------------------------------
# USB Power Delivery 3.0 data objects (USB PD Rev 3.0, section 6.4.1 Power Data Objects and 6.4.2
# Request Data Object). Generic PD encoding only - nothing MAX77705-specific is assumed here.
#   Source PDO type (bits 31:30): 00 Fixed, 01 Battery, 10 Variable, 11 Augmented (APDO);
#   APDO type (bits 29:28): 00 Programmable Power Supply (PPS).
# Units: Fixed/Variable/Battery voltage 50 mV, current 10 mA, power 250 mW;
#        PPS voltage 100 mV, current 50 mA; PPS RDO output voltage 20 mV, operating current 50 mA.
# ---------------------------------------------------------------------------------------------------

PDO_FIXED, PDO_BATTERY, PDO_VARIABLE, PDO_PPS = "fixed", "battery", "variable", "pps"
MAX_SPR_OBJECT_POSITION = 7   # a Source_Capabilities message carries at most 7 PDOs in SPR mode


class PdError(ValueError):
    pass


def _u32(v):
    if not 0 <= v <= 0xFFFFFFFF:
        raise PdError("data object must be a 32-bit value")
    return v


def _field(value, unit, bits, name):
    if value % unit:
        raise PdError(f"{name}={value} is not a multiple of {unit}")
    q = value // unit
    if not 0 <= q < (1 << bits):
        raise PdError(f"{name}={value} does not fit {bits} bits of {unit}")
    return q


def decode_pdo(v):
    """Source PDO -> dict(type=..., values in mV / mA / mW, flags)."""
    v = _u32(v)
    kind = v >> 30
    if kind == 0:
        return {"type": PDO_FIXED,
                "dual_role_power": (v >> 29) & 1, "usb_suspend_supported": (v >> 28) & 1,
                "unconstrained_power": (v >> 27) & 1, "usb_comms_capable": (v >> 26) & 1,
                "dual_role_data": (v >> 25) & 1, "unchunked_ext_msg": (v >> 24) & 1,
                "peak_current": (v >> 20) & 3,
                "voltage_mv": ((v >> 10) & 0x3FF) * 50, "max_current_ma": (v & 0x3FF) * 10}
    if kind == 1:
        return {"type": PDO_BATTERY, "max_voltage_mv": ((v >> 20) & 0x3FF) * 50,
                "min_voltage_mv": ((v >> 10) & 0x3FF) * 50, "max_power_mw": (v & 0x3FF) * 250}
    if kind == 2:
        return {"type": PDO_VARIABLE, "max_voltage_mv": ((v >> 20) & 0x3FF) * 50,
                "min_voltage_mv": ((v >> 10) & 0x3FF) * 50, "max_current_ma": (v & 0x3FF) * 10}
    if (v >> 28) & 3 == 0:
        return {"type": PDO_PPS, "pps_power_limited": (v >> 27) & 1,
                "max_voltage_mv": ((v >> 17) & 0xFF) * 100, "min_voltage_mv": ((v >> 8) & 0xFF) * 100,
                "max_current_ma": (v & 0x7F) * 50}
    raise PdError(f"APDO type {(v >> 28) & 3} is not defined in USB PD 3.0 (SPR)")


def encode_pdo(p):
    """Inverse of decode_pdo (keys as returned by it; flags default to 0)."""
    t = p.get("type")
    if t == PDO_FIXED:
        v = (0 << 30) | (p.get("dual_role_power", 0) & 1) << 29 | (p.get("usb_suspend_supported", 0) & 1) << 28
        v |= (p.get("unconstrained_power", 0) & 1) << 27 | (p.get("usb_comms_capable", 0) & 1) << 26
        v |= (p.get("dual_role_data", 0) & 1) << 25 | (p.get("unchunked_ext_msg", 0) & 1) << 24
        v |= (p.get("peak_current", 0) & 3) << 20
        return v | _field(p["voltage_mv"], 50, 10, "voltage_mv") << 10 | _field(p["max_current_ma"], 10, 10, "max_current_ma")
    if t in (PDO_BATTERY, PDO_VARIABLE):
        last = (_field(p["max_power_mw"], 250, 10, "max_power_mw") if t == PDO_BATTERY
                else _field(p["max_current_ma"], 10, 10, "max_current_ma"))
        return ((1 if t == PDO_BATTERY else 2) << 30 | _field(p["max_voltage_mv"], 50, 10, "max_voltage_mv") << 20
                | _field(p["min_voltage_mv"], 50, 10, "min_voltage_mv") << 10 | last)
    if t == PDO_PPS:
        return (3 << 30 | (p.get("pps_power_limited", 0) & 1) << 27
                | _field(p["max_voltage_mv"], 100, 8, "max_voltage_mv") << 17
                | _field(p["min_voltage_mv"], 100, 8, "min_voltage_mv") << 8
                | _field(p["max_current_ma"], 50, 7, "max_current_ma"))
    raise PdError(f"unknown PDO type {t!r}")


def _rdo_common(v):
    return {"object_position": (v >> 28) & 0xF, "give_back": (v >> 27) & 1, "capability_mismatch": (v >> 26) & 1,
            "usb_comms_capable": (v >> 25) & 1, "no_usb_suspend": (v >> 24) & 1, "unchunked_ext_msg": (v >> 23) & 1}


def decode_rdo(v, pdo_type):
    """Request Data Object -> dict. The layout depends on the type of the PDO it references."""
    v = _u32(v)
    d = _rdo_common(v)
    if d["object_position"] == 0:
        raise PdError("object position 0 is reserved")
    if pdo_type in (PDO_FIXED, PDO_VARIABLE):
        d.update(operating_current_ma=((v >> 10) & 0x3FF) * 10, limit_current_ma=(v & 0x3FF) * 10)
    elif pdo_type == PDO_BATTERY:
        d.update(operating_power_mw=((v >> 10) & 0x3FF) * 250, limit_power_mw=(v & 0x3FF) * 250)
    elif pdo_type == PDO_PPS:
        d.update(output_voltage_mv=((v >> 9) & 0x7FF) * 20, operating_current_ma=(v & 0x7F) * 50)
    else:
        raise PdError(f"unknown PDO type {pdo_type!r}")
    return d


def encode_rdo(object_position, pdo_type, give_back=0, capability_mismatch=0, usb_comms_capable=0,
               no_usb_suspend=0, unchunked_ext_msg=0, **values):
    """Build a Request Data Object for the PDO at 1-based object_position.
    fixed/variable: operating_current_ma, limit_current_ma (max, or min when give_back)
    battery:        operating_power_mw, limit_power_mw
    pps:            output_voltage_mv, operating_current_ma"""
    if not 1 <= object_position <= MAX_SPR_OBJECT_POSITION:
        raise PdError(f"object position must be 1..{MAX_SPR_OBJECT_POSITION}")
    v = (object_position << 28 | (give_back & 1) << 27 | (capability_mismatch & 1) << 26
         | (usb_comms_capable & 1) << 25 | (no_usb_suspend & 1) << 24 | (unchunked_ext_msg & 1) << 23)
    if pdo_type in (PDO_FIXED, PDO_VARIABLE):
        return (v | _field(values["operating_current_ma"], 10, 10, "operating_current_ma") << 10
                | _field(values["limit_current_ma"], 10, 10, "limit_current_ma"))
    if pdo_type == PDO_BATTERY:
        return (v | _field(values["operating_power_mw"], 250, 10, "operating_power_mw") << 10
                | _field(values["limit_power_mw"], 250, 10, "limit_power_mw"))
    if pdo_type == PDO_PPS:
        return (v | _field(values["output_voltage_mv"], 20, 11, "output_voltage_mv") << 9
                | _field(values["operating_current_ma"], 50, 7, "operating_current_ma"))
    raise PdError(f"unknown PDO type {pdo_type!r}")
