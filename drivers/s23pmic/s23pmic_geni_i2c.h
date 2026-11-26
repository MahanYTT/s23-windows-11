/*
 * s23pmic_geni_i2c.h - Pure C11 Qualcomm GENI (QUPv3) I2C transfer encoding for s23pmic.
 *
 * Dependency-free (<stdint.h>/<stddef.h> only), no MMIO: every function only computes register
 * values / decodes register snapshots, so it can be shared by a future drivers/s23pmic/s23pmic.c (kernel, clang
 * freestanding) and tests/c/test_s23pmic_geni.c (host, ASan+UBSan).
 *
 * Sources of truth (nothing here is new hardware knowledge):
 *   - Register offsets, M_CMD opcodes, I2C params, M_IRQ bits, RX FIFO word-count mask, 100 kHz clock
 *     values: drivers/s23sensprobe/s23sensprobe.c:156-229 and drivers/s23audio/s23aud_hw.c:85-126
 *     (both already drive GENI I2C FIFO transfers live on QUPv3_I2C_HUB SE3/SE6).
 *   - Packing-vector algorithm, I2C_STOP_ON_BUS opcode, RX_LAST / RX_LAST_BYTE_VALID fields, clock
 *     field widths, error priority: public Linux drivers/soc/qcom/qcom-geni-se.c
 *     (geni_se_config_packing), include/linux/soc/qcom/geni-se.h and drivers/i2c/busses/i2c-qcom-geni.c.
 *
 * Note: s23sensprobe.c:225 uses PACKING_4X8_MSB_CFG1 = 0x000FFAFE (no PACKING_STOP_BIT in vector 3),
 * s23aud_hw.c:126 uses 0x000FFEFE. The Linux algorithm below yields 0x000FFEFE; the test pins this.
 */
#ifndef S23PMIC_GENI_I2C_H
#define S23PMIC_GENI_I2C_H

#include <stdint.h>
#include <stddef.h>

/* ---- GENI SE register offsets (s23sensprobe.c:156-193) ---- */
#define S23_GENI_FW_REVISION_RO      0x068u
#define S23_GENI_IF_DISABLE_RO       0x064u
#define S23_GENI_STATUS              0x040u
#define S23_GENI_SER_M_CLK_CFG       0x048u
#define S23_GENI_TX_PACKING_CFG0     0x260u
#define S23_GENI_TX_PACKING_CFG1     0x264u
#define S23_GENI_I2C_TX_TRANS_LEN    0x26Cu
#define S23_GENI_I2C_RX_TRANS_LEN    0x270u
#define S23_GENI_I2C_SCL_COUNTERS    0x278u
#define S23_GENI_RX_PACKING_CFG0     0x284u
#define S23_GENI_RX_PACKING_CFG1     0x288u
#define S23_GENI_M_CMD0              0x600u
#define S23_GENI_M_IRQ_STATUS        0x610u
#define S23_GENI_M_IRQ_CLEAR         0x618u
#define S23_GENI_TX_FIFOn            0x700u
#define S23_GENI_RX_FIFOn            0x780u
#define S23_GENI_RX_FIFO_STATUS      0x804u

/* ---- M_CMD0 layout: opcode [31:27], params [26:0] (s23sensprobe.c:679,768: op << 27) ---- */
#define S23_GENI_M_OPCODE_SHFT       27u
#define S23_GENI_M_PARAMS_MASK       0x07FFFFFFu

/* I2C opcodes (s23sensprobe.c:218-220; i2c-qcom-geni.c) */
#define S23_GENI_I2C_OP_WRITE        1u
#define S23_GENI_I2C_OP_READ         2u
#define S23_GENI_I2C_OP_STOP_ON_BUS  7u

/* I2C params (s23sensprobe.c:221-222; i2c-qcom-geni.c SLV_ADDR_MSK GENMASK(15,9)) */
#define S23_GENI_I2C_STOP_STRETCH    (1u << 2)
#define S23_GENI_I2C_SLV_ADDR_SHFT   9u
#define S23_GENI_I2C_SLV_ADDR_MASK   (0x7Fu << S23_GENI_I2C_SLV_ADDR_SHFT)

/* ---- M_IRQ_STATUS bits (s23sensprobe.c:204-216) ---- */
#define S23_GENI_M_CMD_DONE          (1u << 0)
#define S23_GENI_M_CMD_OVERRUN       (1u << 1)
#define S23_GENI_M_ILLEGAL_CMD       (1u << 2)
#define S23_GENI_M_CMD_FAILURE       (1u << 3)
#define S23_GENI_M_CMD_CANCEL        (1u << 4)
#define S23_GENI_M_CMD_ABORT         (1u << 5)
#define S23_GENI_M_GP_IRQ_0          (1u << 9)
#define S23_GENI_M_GP_IRQ_1_NACK     (1u << 10)
#define S23_GENI_M_GP_IRQ_3_BUSPROTO (1u << 12)
#define S23_GENI_M_GP_IRQ_4_ARBLOST  (1u << 13)
#define S23_GENI_M_I2C_ERR_MASK      (S23_GENI_M_CMD_OVERRUN | S23_GENI_M_ILLEGAL_CMD | \
                                      S23_GENI_M_CMD_FAILURE | S23_GENI_M_CMD_ABORT |   \
                                      S23_GENI_M_GP_IRQ_0 | S23_GENI_M_GP_IRQ_1_NACK |  \
                                      S23_GENI_M_GP_IRQ_3_BUSPROTO | S23_GENI_M_GP_IRQ_4_ARBLOST)

/* ---- RX FIFO status (s23sensprobe.c:745 word count mask; geni-se.h RX_LAST*) ---- */
#define S23_GENI_RX_FIFO_WC_MASK     0x01FFFFFFu
#define S23_GENI_RX_LAST             (1u << 31)
#define S23_GENI_RX_LAST_BYTE_SHFT   28u
#define S23_GENI_RX_LAST_BYTE_MASK   0x7u

/* ---- FW revision / interface (s23sensprobe.c:195-198,552,558) ---- */
#define S23_GENI_PROTO_I2C           3u
#define S23_GENI_IF_DISABLE_FIFO     (1u << 0) /* 1 = FIFO interface disabled (GSI/DMA only) */

/* ---- Packing vectors (geni-se.c geni_se_config_packing) ---- */
#define S23_GENI_PACKING_START_SHFT  5u
#define S23_GENI_PACKING_DIR_SHFT    4u
#define S23_GENI_PACKING_LEN_SHFT    1u
#define S23_GENI_PACKING_STOP_BIT    1u
#define S23_GENI_PACKING_VEC_SHFT    10u
#define S23_GENI_PACKING_NUM_VEC     4u

/* ---- Clock fields (geni-se.h: SER_CLK_EN BIT(0), CLK_DIV GENMASK(15,4);
 *      i2c-qcom-geni.c: HIGH GENMASK(29,20), LOW GENMASK(19,10), CYCLE GENMASK(9,0)) ---- */
#define S23_GENI_SER_CLK_EN          1u
#define S23_GENI_CLK_DIV_SHFT        4u
#define S23_GENI_CLK_DIV_MAX         0xFFFu
#define S23_GENI_SCL_HIGH_SHFT       20u
#define S23_GENI_SCL_LOW_SHFT        10u
#define S23_GENI_SCL_FIELD_MAX       0x3FFu

/* 100 kHz on 19.2 MHz XO, exactly the values s23sensprobe.c:227-229 uses live (div 7, 10/11/26).
 * TODO(unverified): no 400 kHz table entry is encoded on purpose - nothing in this repo has used
 * one on SE8 yet; take it from the Linux geni_i2c_clk_map when Phase 0 proves SE8 FIFO mode. */
#define S23_GENI_I2C_100K_DIV        7u
#define S23_GENI_I2C_100K_HIGH       10u
#define S23_GENI_I2C_100K_LOW        11u
#define S23_GENI_I2C_100K_CYCLE      26u

/* Policy (not a hardware limit): largest single transfer the arbiter will plan. The MAX77705 OPCODE
 * mailbox is 0x21..0x41 / 0x51..0x71 = 33 bytes (usb_research/S23_USBC_ARCHITECTURE.md:52-53). */
#define S23_GENI_I2C_MAX_XFER        64u

/* Return codes */
#define S23_GENI_OK                  0
#define S23_GENI_EINVAL              (-1)
#define S23_GENI_ERANGE              (-2)
#define S23_GENI_EUNKNOWN_OP         (-3)
#define S23_GENI_ERESERVED           (-4)

static inline int s23_geni_i2c_opcode_known(uint32_t op)
{
    return op == S23_GENI_I2C_OP_WRITE || op == S23_GENI_I2C_OP_READ ||
           op == S23_GENI_I2C_OP_STOP_ON_BUS;
}

/* Encode M_CMD0 for an I2C command. addr7 must be a 7-bit address; stop_stretch keeps the bus
 * (repeated START follows) and is only meaningful for WRITE. */
static inline int s23_geni_i2c_m_cmd(uint32_t op, uint32_t addr7, int stop_stretch, uint32_t *out)
{
    if (out == NULL) return S23_GENI_EINVAL;
    *out = 0u;
    if (!s23_geni_i2c_opcode_known(op)) return S23_GENI_EUNKNOWN_OP;
    if (addr7 > 0x7Fu) return S23_GENI_ERANGE;
    if (stop_stretch && op != S23_GENI_I2C_OP_WRITE) return S23_GENI_EINVAL;
    uint32_t params = (addr7 << S23_GENI_I2C_SLV_ADDR_SHFT) | (stop_stretch ? S23_GENI_I2C_STOP_STRETCH : 0u);
    *out = (op << S23_GENI_M_OPCODE_SHFT) | (params & S23_GENI_M_PARAMS_MASK);
    return S23_GENI_OK;
}

/* Strict decode: rejects unknown opcodes and any param bit this driver never sets. */
static inline int s23_geni_i2c_m_cmd_decode(uint32_t word, uint32_t *op, uint32_t *addr7, int *stop_stretch)
{
    uint32_t o = word >> S23_GENI_M_OPCODE_SHFT;
    uint32_t p = word & S23_GENI_M_PARAMS_MASK;
    if (op) *op = o;
    if (addr7) *addr7 = (p & S23_GENI_I2C_SLV_ADDR_MASK) >> S23_GENI_I2C_SLV_ADDR_SHFT;
    if (stop_stretch) *stop_stretch = (p & S23_GENI_I2C_STOP_STRETCH) ? 1 : 0;
    if (!s23_geni_i2c_opcode_known(o)) return S23_GENI_EUNKNOWN_OP;
    if (p & ~(S23_GENI_I2C_SLV_ADDR_MASK | S23_GENI_I2C_STOP_STRETCH)) return S23_GENI_ERESERVED;
    return S23_GENI_OK;
}

/* Port of Linux geni_se_config_packing(): bpw bits per word, pack_words FIFO words per transfer
 * word. Valid when 1 <= bpw <= 32 and ceil(bpw/8)*pack_words <= 4. */
static inline int s23_geni_packing_cfg(uint32_t bpw, uint32_t pack_words, int msb_to_lsb,
                                       uint32_t *cfg0, uint32_t *cfg1)
{
    if (cfg0 == NULL || cfg1 == NULL) return S23_GENI_EINVAL;
    *cfg0 = 0u;
    *cfg1 = 0u;
    if (bpw == 0u || bpw > 32u || pack_words == 0u || pack_words > 4u) return S23_GENI_ERANGE;
    uint32_t ceil_bpw = (bpw + 7u) & ~7u;
    uint32_t iter = (ceil_bpw * pack_words) / 8u;
    if (iter == 0u || iter > S23_GENI_PACKING_NUM_VEC) return S23_GENI_ERANGE;

    uint32_t cfg[S23_GENI_PACKING_NUM_VEC] = { 0u, 0u, 0u, 0u };
    int32_t idx_start = msb_to_lsb ? (int32_t)bpw - 1 : 0;
    int32_t idx = idx_start;
    int32_t idx_delta = msb_to_lsb ? -8 : 8;
    uint32_t temp_bpw = bpw;
    for (uint32_t i = 0u; i < iter; i++) {
        uint32_t len = (temp_bpw < 8u ? temp_bpw : 8u) - 1u;
        if (idx < 0 || idx > 31) return S23_GENI_ERANGE; /* cannot happen for valid inputs */
        cfg[i] = ((uint32_t)idx << S23_GENI_PACKING_START_SHFT) |
                 ((msb_to_lsb ? 1u : 0u) << S23_GENI_PACKING_DIR_SHFT) |
                 (len << S23_GENI_PACKING_LEN_SHFT);
        if (temp_bpw <= 8u) {
            idx = (int32_t)((i + 1u) * 8u) + idx_start;
            temp_bpw = bpw;
        } else {
            idx += idx_delta;
            temp_bpw -= 8u;
        }
    }
    cfg[iter - 1u] |= S23_GENI_PACKING_STOP_BIT;
    *cfg0 = cfg[0] | (cfg[1] << S23_GENI_PACKING_VEC_SHFT);
    *cfg1 = cfg[2] | (cfg[3] << S23_GENI_PACKING_VEC_SHFT);
    return S23_GENI_OK;
}

/* Number of 32-bit FIFO words for len bytes (no overflow for any size_t). */
static inline size_t s23_geni_fifo_words(size_t len)
{
    return len / 4u + ((len % 4u) ? 1u : 0u);
}

/* Pack up to 4 bytes of buf[pos..len) into one TX FIFO word, byte k at bits [8k+7:8k]
 * (s23sensprobe.c:763-765, i2c-qcom-geni.c geni_i2c_tx_fifo_fill). Returns bytes consumed. */
static inline size_t s23_geni_tx_pack(const uint8_t *buf, size_t len, size_t pos, uint32_t *word)
{
    if (word == NULL) return 0u;
    *word = 0u;
    if (buf == NULL || pos >= len) return 0u;
    size_t n = len - pos;
    if (n > 4u) n = 4u;
    uint32_t w = 0u;
    for (size_t k = 0u; k < n; k++) {
        w |= (uint32_t)buf[pos + k] << (8u * (uint32_t)k);
    }
    *word = w;
    return n;
}

/* Unpack one RX FIFO word into dst[pos..dst_len), at most 4 bytes (s23sensprobe.c:810-812).
 * Returns bytes written; never writes past dst_len. */
static inline size_t s23_geni_rx_unpack(uint32_t word, uint8_t *dst, size_t dst_len, size_t pos)
{
    if (dst == NULL || pos >= dst_len) return 0u;
    size_t n = dst_len - pos;
    if (n > 4u) n = 4u;
    for (size_t k = 0u; k < n; k++) {
        dst[pos + k] = (uint8_t)((word >> (8u * (uint32_t)k)) & 0xFFu);
    }
    return n;
}

/* Valid bytes currently in the RX FIFO according to SE_GENI_RX_FIFO_STATUS. */
static inline uint32_t s23_geni_rx_fifo_bytes(uint32_t st)
{
    uint32_t wc = st & S23_GENI_RX_FIFO_WC_MASK;
    if (wc == 0u) return 0u;
    uint32_t lbv = (st >> S23_GENI_RX_LAST_BYTE_SHFT) & S23_GENI_RX_LAST_BYTE_MASK;
    if ((st & S23_GENI_RX_LAST) && lbv >= 1u && lbv <= 3u) {
        return (wc - 1u) * 4u + lbv; /* wc <= 0x01FFFFFF so no 32-bit overflow */
    }
    return wc * 4u;
}

/* M_IRQ_STATUS classification, errors first in the order Linux geni_i2c_irq() reports them. */
typedef enum {
    S23_GENI_XFER_PENDING = 0,
    S23_GENI_XFER_DONE,
    S23_GENI_XFER_NACK,
    S23_GENI_XFER_BUS_PROTO,
    S23_GENI_XFER_ARB_LOST,
    S23_GENI_XFER_OVERRUN,
    S23_GENI_XFER_ILLEGAL_CMD,
    S23_GENI_XFER_ABORTED,
    S23_GENI_XFER_GP_IRQ0,
    S23_GENI_XFER_FAILURE,
    S23_GENI_XFER_CANCELLED
} s23_geni_xfer_result;

static inline s23_geni_xfer_result s23_geni_classify_m_irq(uint32_t st)
{
    if (st & S23_GENI_M_GP_IRQ_1_NACK)     return S23_GENI_XFER_NACK;
    if (st & S23_GENI_M_GP_IRQ_3_BUSPROTO) return S23_GENI_XFER_BUS_PROTO;
    if (st & S23_GENI_M_GP_IRQ_4_ARBLOST)  return S23_GENI_XFER_ARB_LOST;
    if (st & S23_GENI_M_CMD_OVERRUN)       return S23_GENI_XFER_OVERRUN;
    if (st & S23_GENI_M_ILLEGAL_CMD)       return S23_GENI_XFER_ILLEGAL_CMD;
    if (st & S23_GENI_M_CMD_ABORT)         return S23_GENI_XFER_ABORTED;
    if (st & S23_GENI_M_GP_IRQ_0)          return S23_GENI_XFER_GP_IRQ0;
    if (st & S23_GENI_M_CMD_FAILURE)       return S23_GENI_XFER_FAILURE;
    if (st & S23_GENI_M_CMD_CANCEL)        return S23_GENI_XFER_CANCELLED;
    if (st & S23_GENI_M_CMD_DONE)          return S23_GENI_XFER_DONE;
    return S23_GENI_XFER_PENDING;
}

static inline const char *s23_geni_xfer_result_name(s23_geni_xfer_result r)
{
    switch (r) {
    case S23_GENI_XFER_PENDING:     return "PENDING";
    case S23_GENI_XFER_DONE:        return "DONE";
    case S23_GENI_XFER_NACK:        return "NACK";
    case S23_GENI_XFER_BUS_PROTO:   return "BUS_PROTO";
    case S23_GENI_XFER_ARB_LOST:    return "ARB_LOST";
    case S23_GENI_XFER_OVERRUN:     return "OVERRUN";
    case S23_GENI_XFER_ILLEGAL_CMD: return "ILLEGAL_CMD";
    case S23_GENI_XFER_ABORTED:     return "ABORTED";
    case S23_GENI_XFER_GP_IRQ0:     return "GP_IRQ0";
    case S23_GENI_XFER_FAILURE:     return "FAILURE";
    case S23_GENI_XFER_CANCELLED:   return "CANCELLED";
    }
    return "?";
}

/* SER_M_CLK_CFG + SCL_COUNTERS from (div, high, low, cycle). */
static inline int s23_geni_i2c_clk_encode(uint32_t div, uint32_t high, uint32_t low, uint32_t cycle,
                                          uint32_t *m_clk_cfg, uint32_t *scl_counters)
{
    if (m_clk_cfg == NULL || scl_counters == NULL) return S23_GENI_EINVAL;
    *m_clk_cfg = 0u;
    *scl_counters = 0u;
    if (div == 0u || div > S23_GENI_CLK_DIV_MAX) return S23_GENI_ERANGE;
    if (high == 0u || low == 0u || cycle == 0u) return S23_GENI_ERANGE;
    if (high > S23_GENI_SCL_FIELD_MAX || low > S23_GENI_SCL_FIELD_MAX || cycle > S23_GENI_SCL_FIELD_MAX)
        return S23_GENI_ERANGE;
    *m_clk_cfg = (div << S23_GENI_CLK_DIV_SHFT) | S23_GENI_SER_CLK_EN;
    *scl_counters = (high << S23_GENI_SCL_HIGH_SHFT) | (low << S23_GENI_SCL_LOW_SHFT) | cycle;
    return S23_GENI_OK;
}

/* SE is usable for FIFO-mode I2C: FW protocol is I2C and the FIFO interface is not disabled. */
static inline int s23_geni_se_fifo_i2c_usable(uint32_t fw_revision, uint32_t if_disable)
{
    if (((fw_revision >> 8) & 0xFFu) != S23_GENI_PROTO_I2C) return 0;
    return (if_disable & S23_GENI_IF_DISABLE_FIFO) ? 0 : 1;
}

/* ---- Transfer plans: the exact command sequence an arbiter would issue ---- */
typedef struct {
    uint32_t m_cmd;     /* value for SE_GENI_M_CMD0 */
    uint32_t trans_len; /* value for SE_I2C_TX_TRANS_LEN (write) or SE_I2C_RX_TRANS_LEN (read) */
    uint32_t is_read;
} s23_geni_i2c_step;

typedef struct {
    uint32_t          nsteps;
    s23_geni_i2c_step step[2];
} s23_geni_i2c_plan;

static inline void s23_geni_i2c_plan_clear(s23_geni_i2c_plan *p)
{
    p->nsteps = 0u;
    for (uint32_t i = 0u; i < 2u; i++) {
        p->step[i].m_cmd = 0u;
        p->step[i].trans_len = 0u;
        p->step[i].is_read = 0u;
    }
}

/* Register read: WRITE reg (STOP_STRETCH) then repeated-START READ rx_len (s23sensprobe.c:761-790). */
static inline int s23_geni_i2c_plan_reg_read(uint32_t addr7, uint32_t rx_len, s23_geni_i2c_plan *p)
{
    if (p == NULL) return S23_GENI_EINVAL;
    s23_geni_i2c_plan_clear(p);
    if (rx_len == 0u || rx_len > S23_GENI_I2C_MAX_XFER) return S23_GENI_ERANGE;
    int rc = s23_geni_i2c_m_cmd(S23_GENI_I2C_OP_WRITE, addr7, 1, &p->step[0].m_cmd);
    if (rc != S23_GENI_OK) return rc;
    rc = s23_geni_i2c_m_cmd(S23_GENI_I2C_OP_READ, addr7, 0, &p->step[1].m_cmd);
    if (rc != S23_GENI_OK) { s23_geni_i2c_plan_clear(p); return rc; }
    p->step[0].trans_len = 1u;
    p->step[1].trans_len = rx_len;
    p->step[1].is_read = 1u;
    p->nsteps = 2u;
    return S23_GENI_OK;
}

/* Register write: one WRITE of [reg, data...] = 1 + data_len bytes, STOP at the end. */
static inline int s23_geni_i2c_plan_reg_write(uint32_t addr7, uint32_t data_len, s23_geni_i2c_plan *p)
{
    if (p == NULL) return S23_GENI_EINVAL;
    s23_geni_i2c_plan_clear(p);
    if (data_len == 0u || data_len > S23_GENI_I2C_MAX_XFER - 1u) return S23_GENI_ERANGE;
    int rc = s23_geni_i2c_m_cmd(S23_GENI_I2C_OP_WRITE, addr7, 0, &p->step[0].m_cmd);
    if (rc != S23_GENI_OK) return rc;
    p->step[0].trans_len = 1u + data_len;
    p->nsteps = 1u;
    return S23_GENI_OK;
}

#endif /* S23PMIC_GENI_I2C_H */
