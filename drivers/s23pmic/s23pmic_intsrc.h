/*
 * s23pmic_intsrc.h - Pure C11 MAX77705 top-level interrupt demux + SE8 slave address map/policy.
 *
 * Dependency-free (<stdint.h>/<stddef.h> only). Shared by a future drivers/s23pmic/s23pmic.c and
 * tests/c/test_s23pmic_intsrc.c.
 *
 * Every constant is taken from repo documentation (cited as path:line):
 *   usb_research/S23_USBC_ARCHITECTURE.md
 *     :18  SE8 slaves 0x66 / 0x25 / 0x69 / 0x36 / 0x62 and PCA9481 0x57; GPIO133 IRQ
 *     :23  INTSRC 0x22 fan-out: CHG BIT0, FG BIT2, MUIC/USBC BIT3
 *     :33  7-bit addresses derived from 8-bit (0xCC, 0x4A, 0xD2, 0x6C, 0xC4)
 *     :36  INTSRC 0x22 on 0x66, INTSRC_MASK 0x23; on BIT3 a 4-byte read from 0x02 on 0x25,
 *          then status 0x06..0x0D
 *     :52-53 OPCODE write mailbox 0x21..0x41, response 0x51..0x71
 *     :90  charger CHG_CNFG_00 0xB7 .. CNFG_10 0xC1
 *     :92,359 PCA9481 (0x57) must stay in standby until Phase 5
 *   audio_sensors_research/S23_AUDIO_HAPTICS_SENSORS_MAP.md
 *     :142 0x66 PMICID1=0x00, PMICREV=0x01, INTSRC=0x22; :146 0x62 = Debug; :147 IRQ GPIO133
 *
 * NOT encoded on purpose (no repo source):
 *   - INTSRC bits other than 0/2/3 (reported as "unknown", never dispatched).
 *   - INTSRC_MASK (0x23) bit polarity: the demux takes a software subscription mask instead.
 *   - 0x3D: listed in the track brief, but no repo document names a device at 0x3D on SE8.
 *     It is kept in the table as UNVERIFIED and every transfer to it is denied.
 */
#ifndef S23PMIC_INTSRC_H
#define S23PMIC_INTSRC_H

#include <stdint.h>
#include <stddef.h>

/* ---- 7-bit slave addresses on QUPv3_I2C_HUB SE8 ---- */
#define S23PMIC_ADDR_PMIC        0x66u /* 0xCC >> 1 top-level PMIC      (ARCH.md:33) */
#define S23PMIC_ADDR_USBC        0x25u /* 0x4A >> 1 USBC/PDIC/MUIC      (ARCH.md:33) */
#define S23PMIC_ADDR_CHG         0x69u /* 0xD2 >> 1 switching charger   (ARCH.md:33) */
#define S23PMIC_ADDR_FG          0x36u /* 0x6C >> 1 fuel gauge          (ARCH.md:33) */
#define S23PMIC_ADDR_DEBUG       0x62u /* 0xC4 >> 1 debug               (ARCH.md:33) */
#define S23PMIC_ADDR_PCA9481     0x57u /* NXP direct charger, Phase 5   (ARCH.md:18,92) */
#define S23PMIC_ADDR_UNVERIFIED  0x3Du /* brief only; no repo source -> always denied */

/* ---- 0x66 registers ---- */
#define S23PMIC_REG_PMICID1      0x00u /* SENSORS_MAP.md:142 */
#define S23PMIC_REG_PMICREV      0x01u /* SENSORS_MAP.md:142 */
#define S23PMIC_REG_INTSRC       0x22u /* ARCH.md:36, SENSORS_MAP.md:142 */
#define S23PMIC_REG_INTSRC_MASK  0x23u /* ARCH.md:36 (polarity not documented) */

/* ---- INTSRC bits (ARCH.md:23,36) ---- */
#define S23PMIC_INTSRC_CHG       (1u << 0)
#define S23PMIC_INTSRC_FG        (1u << 2)
#define S23PMIC_INTSRC_MUIC      (1u << 3)
#define S23PMIC_INTSRC_KNOWN     (S23PMIC_INTSRC_CHG | S23PMIC_INTSRC_FG | S23PMIC_INTSRC_MUIC)

/* ---- 0x25 USBC follow-up after INTSRC.MUIC (ARCH.md:36) ---- */
#define S23PMIC_USBC_REG_UIC_INT     0x02u /* REG_UIC_INT..REG_VDM_INT = 0x02..0x05 */
#define S23PMIC_USBC_INT_LEN         4u
#define S23PMIC_USBC_REG_STATUS_FIRST 0x06u
#define S23PMIC_USBC_REG_STATUS_LAST  0x0Du
#define S23PMIC_USBC_MBOX_WR_FIRST   0x21u /* ARCH.md:52 */
#define S23PMIC_USBC_MBOX_WR_LAST    0x41u /* ARCH.md:52 (OPCODE_WRITE_END) */
#define S23PMIC_USBC_MBOX_RD_FIRST   0x51u /* ARCH.md:53 */
#define S23PMIC_USBC_MBOX_RD_LAST    0x71u /* ARCH.md:53 */

/* ---- 0x69 charger write window: CHG_CNFG_00 (0xB7) .. CHG_CNFG_12 (0xC3). This is exactly the span
 *      drivers/s23batt/s23batt.c:679-722 writes live today (0xB7/B8/B9/BB/BD/C0/C1/C3); ARCH.md:90 names
 *      CNFG_00 0xB7 .. CNFG_10 0xC1. test_s23pmic_intsrc.c extracts every s23batt write and checks it. ---- */
#define S23PMIC_CHG_WR_FIRST         0xB7u
#define S23PMIC_CHG_WR_LAST          0xC3u

/* ---- Sub-device ids used by the arbiter's client table ---- */
#define S23PMIC_SUB_CHG          (1u << 0)
#define S23PMIC_SUB_FG           (1u << 1)
#define S23PMIC_SUB_USBC         (1u << 2)
#define S23PMIC_SUB_ALL          (S23PMIC_SUB_CHG | S23PMIC_SUB_FG | S23PMIC_SUB_USBC)

typedef struct {
    uint32_t dispatch;        /* S23PMIC_SUB_* clients to notify (asserted AND subscribed) */
    uint32_t unclaimed;       /* S23PMIC_SUB_* asserted but nobody subscribed (log + drop) */
    uint8_t  unknown_bits;    /* INTSRC bits with no documented meaning (never dispatched) */
    uint8_t  usbc_followup;   /* 1: read S23PMIC_USBC_INT_LEN bytes from 0x02 on 0x25 next */
} s23pmic_demux_result;

/* Demux one INTSRC (0x22) snapshot. `subscribed` = S23PMIC_SUB_* mask of registered clients.
 * Pure function: reading 0x22 clears it in hardware (ARCH.md:18 "clear-on-read"), so the caller
 * must call this exactly once per read value. */
static inline void s23pmic_intsrc_demux(uint8_t intsrc, uint32_t subscribed, s23pmic_demux_result *r)
{
    if (r == NULL) return;
    uint32_t asserted = 0u;
    if (intsrc & S23PMIC_INTSRC_CHG)  asserted |= S23PMIC_SUB_CHG;
    if (intsrc & S23PMIC_INTSRC_FG)   asserted |= S23PMIC_SUB_FG;
    if (intsrc & S23PMIC_INTSRC_MUIC) asserted |= S23PMIC_SUB_USBC;
    subscribed &= S23PMIC_SUB_ALL;
    r->dispatch = asserted & subscribed;
    r->unclaimed = asserted & ~subscribed;
    r->unknown_bits = (uint8_t)(intsrc & (uint8_t)~S23PMIC_INTSRC_KNOWN);
    r->usbc_followup = (r->dispatch & S23PMIC_SUB_USBC) ? 1u : 0u;
}

/* ---- Address map / policy ---- */
#define S23PMIC_SLAVE_DOCUMENTED  (1u << 0) /* named in a repo doc */
#define S23PMIC_SLAVE_READ        (1u << 1) /* register reads allowed */
#define S23PMIC_SLAVE_WRITE_WIN   (1u << 2) /* writes allowed inside [write_first, write_last] only */
#define S23PMIC_SLAVE_DENY        (1u << 3) /* every transfer denied */

typedef struct {
    uint8_t     addr7;
    uint8_t     addr8;        /* 8-bit write address as written in Linux max77705.c (ARCH.md:33), 0 if n/a */
    uint8_t     flags;
    uint8_t     write_first;
    uint8_t     write_last;
    const char *name;
} s23pmic_slave_info;

/* I2C-bus specification (NXP UM10204, table "reserved addresses"): 0000xxx and 1111xxx are reserved. */
static inline int s23pmic_addr7_is_valid(uint32_t addr7)
{
    return addr7 >= 0x08u && addr7 <= 0x77u;
}

/* 8-bit (R/W bit = 0) to 7-bit; returns -1 if the low bit is set or the result is reserved. */
static inline int s23pmic_addr8_to_addr7(uint32_t addr8)
{
    if (addr8 > 0xFFu || (addr8 & 1u)) return -1;
    uint32_t a = addr8 >> 1;
    return s23pmic_addr7_is_valid(a) ? (int)a : -1;
}

/* Fill *out for a known SE8 slave. Returns 0 if known, -1 for an unknown / reserved / >7-bit address
 * (then *out describes a denied unknown slave). */
static inline int s23pmic_slave_lookup(uint32_t addr7, s23pmic_slave_info *out)
{
    s23pmic_slave_info s;
    s.addr7 = (uint8_t)(addr7 & 0x7Fu);
    s.addr8 = 0u;
    s.flags = S23PMIC_SLAVE_DENY;
    s.write_first = 0u;
    s.write_last = 0u;
    s.name = "unknown";
    int known = 1;
    if (addr7 > 0x7Fu || !s23pmic_addr7_is_valid(addr7)) {
        known = 0;
    } else {
        switch (addr7) {
        case S23PMIC_ADDR_PMIC:
            s.addr8 = 0xCCu; s.flags = S23PMIC_SLAVE_DOCUMENTED | S23PMIC_SLAVE_READ;
            s.name = "MAX77705 PMIC top"; break;
        case S23PMIC_ADDR_USBC:
            s.addr8 = 0x4Au; s.flags = S23PMIC_SLAVE_DOCUMENTED | S23PMIC_SLAVE_READ | S23PMIC_SLAVE_WRITE_WIN;
            s.write_first = S23PMIC_USBC_MBOX_WR_FIRST; s.write_last = S23PMIC_USBC_MBOX_WR_LAST;
            s.name = "MAX77705 USBC/PDIC/MUIC"; break;
        case S23PMIC_ADDR_CHG:
            s.addr8 = 0xD2u; s.flags = S23PMIC_SLAVE_DOCUMENTED | S23PMIC_SLAVE_READ | S23PMIC_SLAVE_WRITE_WIN;
            s.write_first = S23PMIC_CHG_WR_FIRST; s.write_last = S23PMIC_CHG_WR_LAST;
            s.name = "MAX77705 charger"; break;
        case S23PMIC_ADDR_FG:
            s.addr8 = 0x6Cu; s.flags = S23PMIC_SLAVE_DOCUMENTED | S23PMIC_SLAVE_READ;
            s.name = "MAX77705 fuel gauge"; break;
        case S23PMIC_ADDR_DEBUG:
            s.addr8 = 0xC4u; s.flags = S23PMIC_SLAVE_DOCUMENTED | S23PMIC_SLAVE_DENY;
            s.name = "MAX77705 debug"; break;
        case S23PMIC_ADDR_PCA9481:
            s.flags = S23PMIC_SLAVE_DOCUMENTED | S23PMIC_SLAVE_DENY;
            s.name = "PCA9481 direct charger (Phase 5)"; break;
        case S23PMIC_ADDR_UNVERIFIED:
            s.flags = S23PMIC_SLAVE_DENY;
            s.name = "UNVERIFIED 0x3D (no repo source)"; break;
        default:
            known = 0; break;
        }
    }
    if (out) *out = s;
    return known ? 0 : -1;
}

/* Transfer policy verdicts */
#define S23PMIC_XFER_OK            0
#define S23PMIC_XFER_BAD_ADDR      1 /* reserved, > 7 bit or not an SE8 slave */
#define S23PMIC_XFER_DENIED_SLAVE  2 /* slave on the deny list */
#define S23PMIC_XFER_BAD_LEN       3 /* len == 0 or > max_len */
#define S23PMIC_XFER_WRAP          4 /* reg + len crosses 0xFF (8-bit register space) */
#define S23PMIC_XFER_READ_DENIED   5
#define S23PMIC_XFER_WRITE_DENIED  6 /* outside the documented write window */

/* Is [reg, reg+len) on addr7 allowed? max_len caps the transfer (e.g. S23_GENI_I2C_MAX_XFER). */
static inline int s23pmic_xfer_check(uint32_t addr7, uint32_t reg, uint32_t len, int is_write, uint32_t max_len)
{
    s23pmic_slave_info s;
    if (s23pmic_slave_lookup(addr7, &s) != 0) return S23PMIC_XFER_BAD_ADDR;
    if (s.flags & S23PMIC_SLAVE_DENY) return S23PMIC_XFER_DENIED_SLAVE;
    if (len == 0u || len > max_len) return S23PMIC_XFER_BAD_LEN;
    if (reg > 0xFFu || len > 0x100u - reg) return S23PMIC_XFER_WRAP;
    if (!is_write) {
        return (s.flags & S23PMIC_SLAVE_READ) ? S23PMIC_XFER_OK : S23PMIC_XFER_READ_DENIED;
    }
    if (!(s.flags & S23PMIC_SLAVE_WRITE_WIN)) return S23PMIC_XFER_WRITE_DENIED;
    if (reg < s.write_first || reg + len - 1u > s.write_last) return S23PMIC_XFER_WRITE_DENIED;
    return S23PMIC_XFER_OK;
}

static inline const char *s23pmic_xfer_verdict_name(int v)
{
    switch (v) {
    case S23PMIC_XFER_OK:           return "OK";
    case S23PMIC_XFER_BAD_ADDR:     return "BAD_ADDR";
    case S23PMIC_XFER_DENIED_SLAVE: return "DENIED_SLAVE";
    case S23PMIC_XFER_BAD_LEN:      return "BAD_LEN";
    case S23PMIC_XFER_WRAP:         return "WRAP";
    case S23PMIC_XFER_READ_DENIED:  return "READ_DENIED";
    case S23PMIC_XFER_WRITE_DENIED: return "WRITE_DENIED";
    default:                        return "?";
    }
}

#endif /* S23PMIC_INTSRC_H */
