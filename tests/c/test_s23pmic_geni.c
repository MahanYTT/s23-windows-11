/*
 * Host unit tests for drivers/s23pmic/s23pmic_geni_i2c.h (compiled with ASan+UBSan in CI).
 *
 * Cross-checks the pure GENI I2C encoders against the constants the live drivers s23sensprobe and
 * s23audio use on the phone today (extracted by tests/c/extract_s23pmic.py, nothing hand-copied),
 * and exercises every bounds check with truncated / malformed input.
 */
#include <stdint.h>
#include <stddef.h>
#include "host_test.h"
#include "s23pmic/s23pmic_geni_i2c.h"
#include "s23pmic_live_consts.inc"

static void test_register_offsets_match_live_drivers(void)
{
    CHECK_EQ_U64(S23_GENI_M_CMD0, X_SENSPROBE_SE_GENI_M_CMD0);
    CHECK_EQ_U64(S23_GENI_M_IRQ_STATUS, X_SENSPROBE_SE_GENI_M_IRQ_STATUS);
    CHECK_EQ_U64(S23_GENI_RX_FIFO_STATUS, X_SENSPROBE_SE_GENI_RX_FIFO_STATUS);
    CHECK_EQ_U64(S23_GENI_TX_FIFOn, X_SENSPROBE_SE_GENI_TX_FIFOn);
    CHECK_EQ_U64(S23_GENI_RX_FIFOn, X_SENSPROBE_SE_GENI_RX_FIFOn);
    CHECK_EQ_U64(S23_GENI_I2C_TX_TRANS_LEN, X_SENSPROBE_SE_I2C_TX_TRANS_LEN);
    CHECK_EQ_U64(S23_GENI_I2C_RX_TRANS_LEN, X_SENSPROBE_SE_I2C_RX_TRANS_LEN);
    CHECK_EQ_U64(S23_GENI_I2C_SCL_COUNTERS, X_SENSPROBE_SE_I2C_SCL_COUNTERS);
    CHECK_EQ_U64(S23_GENI_SER_M_CLK_CFG, X_SENSPROBE_GENI_SER_M_CLK_CFG);
    CHECK_EQ_U64(S23_GENI_FW_REVISION_RO, X_SENSPROBE_GENI_FW_REVISION_RO);
    CHECK_EQ_U64(S23_GENI_IF_DISABLE_RO, X_SENSPROBE_GENI_IF_DISABLE_RO);

    CHECK_EQ_U64(S23_GENI_I2C_OP_WRITE, X_SENSPROBE_I2C_OP_WRITE);
    CHECK_EQ_U64(S23_GENI_I2C_OP_READ, X_SENSPROBE_I2C_OP_READ);
    CHECK_EQ_U64(S23_GENI_I2C_OP_STOP_ON_BUS, X_SENSPROBE_I2C_OP_STOP_ON_BUS);
    CHECK_EQ_U64(S23_GENI_I2C_STOP_STRETCH, X_SENSPROBE_I2C_STOP_STRETCH);
    CHECK_EQ_U64(S23_GENI_I2C_SLV_ADDR_SHFT, X_SENSPROBE_I2C_SLV_ADDR_SHFT);
    CHECK_EQ_U64(S23_GENI_I2C_OP_WRITE, X_S23AUDIO_I2C_OP_WRITE);
    CHECK_EQ_U64(S23_GENI_I2C_OP_READ, X_S23AUDIO_I2C_OP_READ);
    CHECK_EQ_U64(S23_GENI_I2C_SLV_ADDR_SHFT, X_S23AUDIO_I2C_SLV_ADDR_SHFT);
    CHECK_EQ_U64(S23_GENI_I2C_STOP_STRETCH, X_S23AUDIO_I2C_STOP_STRETCH);

    CHECK_EQ_U64(S23_GENI_M_CMD_DONE, X_SENSPROBE_M_CMD_DONE_EN);
    CHECK_EQ_U64(S23_GENI_M_CMD_OVERRUN, X_SENSPROBE_M_CMD_OVERRUN_EN);
    CHECK_EQ_U64(S23_GENI_M_ILLEGAL_CMD, X_SENSPROBE_M_ILLEGAL_CMD_EN);
    CHECK_EQ_U64(S23_GENI_M_CMD_FAILURE, X_SENSPROBE_M_CMD_FAILURE_EN);
    CHECK_EQ_U64(S23_GENI_M_CMD_CANCEL, X_SENSPROBE_M_CMD_CANCEL_EN);
    CHECK_EQ_U64(S23_GENI_M_CMD_ABORT, X_SENSPROBE_M_CMD_ABORT_EN);
    CHECK_EQ_U64(S23_GENI_M_GP_IRQ_0, X_SENSPROBE_M_GP_IRQ_0_EN);
    CHECK_EQ_U64(S23_GENI_M_GP_IRQ_1_NACK, X_SENSPROBE_M_GP_IRQ_1_EN);
    CHECK_EQ_U64(S23_GENI_M_GP_IRQ_3_BUSPROTO, X_SENSPROBE_M_GP_IRQ_3_EN);
    CHECK_EQ_U64(S23_GENI_M_GP_IRQ_4_ARBLOST, X_SENSPROBE_M_GP_IRQ_4_EN);
}

static void test_m_cmd_encode_decode(void)
{
    uint32_t w = 0xDEADBEEFu, op = 0, a = 0;
    int ss = -1;

    /* Exactly what s23sensprobe.c:766-768 / s23aud_hw.c:1832-1834 write for a reg-address write */
    CHECK(s23_geni_i2c_m_cmd(S23_GENI_I2C_OP_WRITE, 0x66u, 1, &w) == S23_GENI_OK);
    CHECK_EQ_U64(w, (X_SENSPROBE_I2C_OP_WRITE << 27) | (0x66ull << X_SENSPROBE_I2C_SLV_ADDR_SHFT) |
                        X_SENSPROBE_I2C_STOP_STRETCH);
    CHECK_EQ_U64(w, 0x0800CC04u);
    CHECK(s23_geni_i2c_m_cmd_decode(w, &op, &a, &ss) == S23_GENI_OK);
    CHECK_EQ_U64(op, 1u);
    CHECK_EQ_U64(a, 0x66u);
    CHECK(ss == 1);

    /* repeated-START read (s23sensprobe.c:788-790) */
    CHECK(s23_geni_i2c_m_cmd(S23_GENI_I2C_OP_READ, 0x25u, 0, &w) == S23_GENI_OK);
    CHECK_EQ_U64(w, 0x10004A00u);
    CHECK(s23_geni_i2c_m_cmd_decode(w, &op, &a, &ss) == S23_GENI_OK);
    CHECK_EQ_U64(op, 2u);
    CHECK_EQ_U64(a, 0x25u);
    CHECK(ss == 0);

    /* STOP_ON_BUS recovery (s23sensprobe.c:677-679) */
    CHECK(s23_geni_i2c_m_cmd(S23_GENI_I2C_OP_STOP_ON_BUS, 0x7Fu, 0, &w) == S23_GENI_OK);
    CHECK_EQ_U64(w, (7u << 27) | (0x7Fu << 9));

    /* every 7-bit address round-trips, both stretch settings for WRITE */
    for (uint32_t addr = 0; addr <= 0x7Fu; addr++) {
        for (int s = 0; s <= 1; s++) {
            CHECK(s23_geni_i2c_m_cmd(S23_GENI_I2C_OP_WRITE, addr, s, &w) == S23_GENI_OK);
            CHECK(s23_geni_i2c_m_cmd_decode(w, &op, &a, &ss) == S23_GENI_OK);
            CHECK(op == 1u && a == addr && ss == s);
        }
    }

    /* invalid inputs */
    CHECK(s23_geni_i2c_m_cmd(S23_GENI_I2C_OP_WRITE, 0x80u, 0, &w) == S23_GENI_ERANGE);
    CHECK_EQ_U64(w, 0u);
    CHECK(s23_geni_i2c_m_cmd(S23_GENI_I2C_OP_WRITE, 0xFFFFFFFFu, 0, &w) == S23_GENI_ERANGE);
    CHECK(s23_geni_i2c_m_cmd(0u, 0x10u, 0, &w) == S23_GENI_EUNKNOWN_OP);
    CHECK(s23_geni_i2c_m_cmd(3u, 0x10u, 0, &w) == S23_GENI_EUNKNOWN_OP);
    CHECK(s23_geni_i2c_m_cmd(0xFFFFFFFFu, 0x10u, 0, &w) == S23_GENI_EUNKNOWN_OP);
    CHECK(s23_geni_i2c_m_cmd(S23_GENI_I2C_OP_READ, 0x10u, 1, &w) == S23_GENI_EINVAL);
    CHECK(s23_geni_i2c_m_cmd(S23_GENI_I2C_OP_WRITE, 0x10u, 0, NULL) == S23_GENI_EINVAL);

    /* strict decode rejects unknown opcodes and stray param bits */
    CHECK(s23_geni_i2c_m_cmd_decode(0u, NULL, NULL, NULL) == S23_GENI_EUNKNOWN_OP);
    CHECK(s23_geni_i2c_m_cmd_decode((1u << 27) | 1u, NULL, NULL, NULL) == S23_GENI_ERESERVED);
    CHECK(s23_geni_i2c_m_cmd_decode((1u << 27) | (1u << 16), NULL, NULL, NULL) == S23_GENI_ERESERVED);
    CHECK(s23_geni_i2c_m_cmd_decode(0xFFFFFFFFu, &op, &a, &ss) == S23_GENI_EUNKNOWN_OP);
    CHECK_EQ_U64(op, 31u);
}

static void test_packing(void)
{
    uint32_t c0 = 1, c1 = 1;
    /* i2c-qcom-geni.c: geni_se_config_packing(se, 8, 4, true, true, true) */
    CHECK(s23_geni_packing_cfg(8u, 4u, 1, &c0, &c1) == S23_GENI_OK);
    CHECK_EQ_U64(c0, 0x0007F8FEu);
    CHECK_EQ_U64(c1, 0x000FFEFEu);
    /* s23audio (live) uses exactly these values */
    CHECK_EQ_U64(c0, X_S23AUDIO_PACKING_4X8_MSB_CFG0);
    CHECK_EQ_U64(c1, X_S23AUDIO_PACKING_4X8_MSB_CFG1);
    /* s23sensprobe: CFG0 identical; CFG1 lacks PACKING_STOP_BIT in vector 3 (bit 10). Pin the known
     * difference so it is visible (s23sensprobe is not touched by this change). */
    CHECK_EQ_U64(c0, X_SENSPROBE_PACKING_4X8_MSB_CFG0);
    CHECK_EQ_U64(c1 ^ X_SENSPROBE_PACKING_4X8_MSB_CFG1, (unsigned long long)S23_GENI_PACKING_STOP_BIT << 10);

    /* LSB-first 4x8 (UART-style): start idx 0, 8, 16, 24 */
    CHECK(s23_geni_packing_cfg(8u, 4u, 0, &c0, &c1) == S23_GENI_OK);
    CHECK_EQ_U64(c0, 0x0E | ((0x100u | 0x0E) << 10));
    CHECK_EQ_U64(c1, (0x200u | 0x0E) | ((0x300u | 0x0E | 1u) << 10));

    /* 1 x 32-bit word msb-first: one transfer word spread over 4 byte vectors, stop on the last */
    CHECK(s23_geni_packing_cfg(32u, 1u, 1, &c0, &c1) == S23_GENI_OK);
    CHECK_EQ_U64(c0, ((31u << 5) | 0x10u | 0x0Eu) | (((23u << 5) | 0x10u | 0x0Eu) << 10));
    CHECK_EQ_U64(c1, ((15u << 5) | 0x10u | 0x0Eu) | (((7u << 5) | 0x10u | 0x0Eu | 1u) << 10));

    /* single 8-bit word: only vector 0, with stop bit */
    CHECK(s23_geni_packing_cfg(8u, 1u, 1, &c0, &c1) == S23_GENI_OK);
    CHECK_EQ_U64(c0, 0xFFu);
    CHECK_EQ_U64(c1, 0u);

    /* odd widths stay inside 0..31 bit indices */
    for (uint32_t bpw = 1; bpw <= 32u; bpw++) {
        for (uint32_t pw = 1; pw <= 4u; pw++) {
            for (int msb = 0; msb <= 1; msb++) {
                int rc = s23_geni_packing_cfg(bpw, pw, msb, &c0, &c1);
                uint32_t iter = ((bpw + 7u) / 8u) * pw;
                CHECK((rc == S23_GENI_OK) == (iter <= 4u));
                if (rc != S23_GENI_OK) CHECK(c0 == 0u && c1 == 0u);
            }
        }
    }

    /* invalid */
    CHECK(s23_geni_packing_cfg(0u, 4u, 1, &c0, &c1) == S23_GENI_ERANGE);
    CHECK(s23_geni_packing_cfg(33u, 1u, 1, &c0, &c1) == S23_GENI_ERANGE);
    CHECK(s23_geni_packing_cfg(8u, 0u, 1, &c0, &c1) == S23_GENI_ERANGE);
    CHECK(s23_geni_packing_cfg(8u, 5u, 1, &c0, &c1) == S23_GENI_ERANGE);
    CHECK(s23_geni_packing_cfg(16u, 3u, 1, &c0, &c1) == S23_GENI_ERANGE);
    CHECK(s23_geni_packing_cfg(0xFFFFFFFFu, 0xFFFFFFFFu, 1, &c0, &c1) == S23_GENI_ERANGE);
    CHECK(s23_geni_packing_cfg(8u, 4u, 1, NULL, &c1) == S23_GENI_EINVAL);
    CHECK(s23_geni_packing_cfg(8u, 4u, 1, &c0, NULL) == S23_GENI_EINVAL);
}

static void test_fifo_pack_unpack(void)
{
    const uint8_t buf[7] = { 0x22, 0x11, 0x33, 0x44, 0x55, 0x66, 0x77 };
    uint32_t w = 0xFFFFFFFFu;

    CHECK_EQ_U64(s23_geni_fifo_words(0), 0u);
    CHECK_EQ_U64(s23_geni_fifo_words(1), 1u);
    CHECK_EQ_U64(s23_geni_fifo_words(4), 1u);
    CHECK_EQ_U64(s23_geni_fifo_words(5), 2u);
    CHECK_EQ_U64(s23_geni_fifo_words(SIZE_MAX), SIZE_MAX / 4u + 1u);

    CHECK_EQ_U64(s23_geni_tx_pack(buf, 7, 0, &w), 4u);
    CHECK_EQ_U64(w, 0x44331122u);
    CHECK_EQ_U64(s23_geni_tx_pack(buf, 7, 4, &w), 3u);
    CHECK_EQ_U64(w, 0x00776655u);
    CHECK_EQ_U64(s23_geni_tx_pack(buf, 7, 6, &w), 1u);
    CHECK_EQ_U64(w, 0x77u);
    /* single register byte exactly like s23sensprobe.c:762-765 (tx_word |= b << i*8) */
    CHECK_EQ_U64(s23_geni_tx_pack(buf, 1, 0, &w), 1u);
    CHECK_EQ_U64(w, 0x22u);
    /* truncated / out of range */
    CHECK_EQ_U64(s23_geni_tx_pack(buf, 7, 7, &w), 0u);
    CHECK_EQ_U64(w, 0u);
    CHECK_EQ_U64(s23_geni_tx_pack(buf, 7, SIZE_MAX, &w), 0u);
    CHECK_EQ_U64(s23_geni_tx_pack(NULL, 7, 0, &w), 0u);
    CHECK_EQ_U64(s23_geni_tx_pack(buf, 7, 0, NULL), 0u);
    CHECK_EQ_U64(s23_geni_tx_pack(buf, 0, 0, &w), 0u);

    uint8_t out[6] = { 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA };
    CHECK_EQ_U64(s23_geni_rx_unpack(0x44332211u, out, 6, 0), 4u);
    CHECK(out[0] == 0x11 && out[1] == 0x22 && out[2] == 0x33 && out[3] == 0x44 && out[4] == 0xAA);
    CHECK_EQ_U64(s23_geni_rx_unpack(0x88776655u, out, 6, 4), 2u); /* clipped at dst_len */
    CHECK(out[4] == 0x55 && out[5] == 0x66);
    CHECK_EQ_U64(s23_geni_rx_unpack(0x12345678u, out, 6, 6), 0u);
    CHECK_EQ_U64(s23_geni_rx_unpack(0x12345678u, out, 6, SIZE_MAX), 0u);
    CHECK_EQ_U64(s23_geni_rx_unpack(0x12345678u, NULL, 6, 0), 0u);
    CHECK(out[5] == 0x66);

    /* round trip of a 33-byte OPCODE mailbox payload (ARCH.md:52) */
    uint8_t mb[33], back[33];
    for (size_t i = 0; i < sizeof(mb); i++) { mb[i] = (uint8_t)(i * 7u + 1u); back[i] = 0; }
    size_t pos = 0, words = 0;
    while (pos < sizeof(mb)) {
        size_t n = s23_geni_tx_pack(mb, sizeof(mb), pos, &w);
        CHECK(n >= 1u && n <= 4u);
        CHECK_EQ_U64(s23_geni_rx_unpack(w, back, sizeof(back), pos), n);
        pos += n;
        words++;
    }
    CHECK_EQ_U64(words, s23_geni_fifo_words(sizeof(mb)));
    for (size_t i = 0; i < sizeof(mb); i++) CHECK(back[i] == mb[i]);
}

static void test_rx_fifo_status(void)
{
    CHECK_EQ_U64(s23_geni_rx_fifo_bytes(0u), 0u);
    CHECK_EQ_U64(s23_geni_rx_fifo_bytes(1u), 4u);
    CHECK_EQ_U64(s23_geni_rx_fifo_bytes(3u), 12u);
    CHECK_EQ_U64(s23_geni_rx_fifo_bytes(0x80000000u | (2u << 28) | 3u), 10u);
    CHECK_EQ_U64(s23_geni_rx_fifo_bytes(0x80000000u | (0u << 28) | 3u), 12u); /* lbv 0 = full word */
    CHECK_EQ_U64(s23_geni_rx_fifo_bytes(0x80000000u | (5u << 28) | 2u), 8u);  /* lbv >= 4 ignored */
    CHECK_EQ_U64(s23_geni_rx_fifo_bytes(0x80000000u | (1u << 28)), 0u);       /* last but wc 0 */
    CHECK_EQ_U64(s23_geni_rx_fifo_bytes(0x01FFFFFFu), 0x01FFFFFFull * 4u);
    /* all bits set: lbv 7 is not a valid partial count -> full words; result fits in 32 bits */
    CHECK_EQ_U64(s23_geni_rx_fifo_bytes(0xFFFFFFFFu), 0x07FFFFFCu);
    CHECK_EQ_U64(s23_geni_rx_fifo_bytes(0xB1FFFFFFu), (0x01FFFFFFull - 1u) * 4u + 3u);
}

static void test_classify(void)
{
    CHECK(s23_geni_classify_m_irq(0u) == S23_GENI_XFER_PENDING);
    CHECK(s23_geni_classify_m_irq(S23_GENI_M_CMD_DONE) == S23_GENI_XFER_DONE);
    CHECK(s23_geni_classify_m_irq(S23_GENI_M_CMD_DONE | S23_GENI_M_CMD_FAILURE | S23_GENI_M_GP_IRQ_1_NACK) ==
          S23_GENI_XFER_NACK);
    CHECK(s23_geni_classify_m_irq(S23_GENI_M_CMD_FAILURE | S23_GENI_M_GP_IRQ_3_BUSPROTO) == S23_GENI_XFER_BUS_PROTO);
    CHECK(s23_geni_classify_m_irq(S23_GENI_M_GP_IRQ_4_ARBLOST) == S23_GENI_XFER_ARB_LOST);
    CHECK(s23_geni_classify_m_irq(S23_GENI_M_CMD_OVERRUN) == S23_GENI_XFER_OVERRUN);
    CHECK(s23_geni_classify_m_irq(S23_GENI_M_ILLEGAL_CMD) == S23_GENI_XFER_ILLEGAL_CMD);
    CHECK(s23_geni_classify_m_irq(S23_GENI_M_CMD_ABORT) == S23_GENI_XFER_ABORTED);
    CHECK(s23_geni_classify_m_irq(S23_GENI_M_GP_IRQ_0) == S23_GENI_XFER_GP_IRQ0);
    CHECK(s23_geni_classify_m_irq(S23_GENI_M_CMD_FAILURE) == S23_GENI_XFER_FAILURE);
    CHECK(s23_geni_classify_m_irq(S23_GENI_M_CMD_CANCEL | S23_GENI_M_CMD_DONE) == S23_GENI_XFER_CANCELLED);
    CHECK(s23_geni_classify_m_irq(0xFFFFFFFFu) == S23_GENI_XFER_NACK);
    /* every error bit of the live M_I2C_ERR_MASK classifies as an error */
    for (uint32_t b = 0; b < 32u; b++) {
        uint32_t bit = 1u << b;
        s23_geni_xfer_result r = s23_geni_classify_m_irq(bit | S23_GENI_M_CMD_DONE);
        if (bit & S23_GENI_M_I2C_ERR_MASK) CHECK(r != S23_GENI_XFER_DONE && r != S23_GENI_XFER_PENDING);
        CHECK(s23_geni_xfer_result_name(r)[0] != '?');
    }
    CHECK(s23_geni_xfer_result_name((s23_geni_xfer_result)99)[0] == '?');
}

static void test_clock(void)
{
    uint32_t m = 1, s = 1;
    CHECK(s23_geni_i2c_clk_encode(S23_GENI_I2C_100K_DIV, S23_GENI_I2C_100K_HIGH, S23_GENI_I2C_100K_LOW,
                                  S23_GENI_I2C_100K_CYCLE, &m, &s) == S23_GENI_OK);
    CHECK_EQ_U64(m, X_SENSPROBE_I2C_100K_SER_M_CLK_CFG);
    CHECK_EQ_U64(s, X_SENSPROBE_I2C_100K_SCL_COUNTERS);
    CHECK(s23_geni_i2c_clk_encode(0u, 1u, 1u, 1u, &m, &s) == S23_GENI_ERANGE);
    CHECK(m == 0u && s == 0u);
    CHECK(s23_geni_i2c_clk_encode(0x1000u, 1u, 1u, 1u, &m, &s) == S23_GENI_ERANGE);
    CHECK(s23_geni_i2c_clk_encode(1u, 0x400u, 1u, 1u, &m, &s) == S23_GENI_ERANGE);
    CHECK(s23_geni_i2c_clk_encode(1u, 1u, 0x400u, 1u, &m, &s) == S23_GENI_ERANGE);
    CHECK(s23_geni_i2c_clk_encode(1u, 1u, 1u, 0x400u, &m, &s) == S23_GENI_ERANGE);
    CHECK(s23_geni_i2c_clk_encode(1u, 0u, 1u, 1u, &m, &s) == S23_GENI_ERANGE);
    CHECK(s23_geni_i2c_clk_encode(0xFFFu, 0x3FFu, 0x3FFu, 0x3FFu, &m, &s) == S23_GENI_OK);
    CHECK_EQ_U64(m, 0xFFF1u);
    CHECK_EQ_U64(s, 0x3FFFFFFFu);
    CHECK(s23_geni_i2c_clk_encode(1u, 1u, 1u, 1u, NULL, &s) == S23_GENI_EINVAL);
}

static void test_se_usable(void)
{
    CHECK(s23_geni_se_fifo_i2c_usable(0x00000300u, 0u) == 1);
    CHECK(s23_geni_se_fifo_i2c_usable(0x00000300u, 1u) == 0); /* GSI only */
    CHECK(s23_geni_se_fifo_i2c_usable(0x00000200u, 0u) == 0); /* UART */
    CHECK(s23_geni_se_fifo_i2c_usable(0xFFFF03FFu, 0xFFFFFFFEu) == 1);
    CHECK(s23_geni_se_fifo_i2c_usable(0xFFFFFFFFu, 0u) == 0);
}

static void test_plans(void)
{
    s23_geni_i2c_plan p;
    /* PMICID1 read on 0x66: WRITE reg (stretch) + READ 1 */
    CHECK(s23_geni_i2c_plan_reg_read(0x66u, 1u, &p) == S23_GENI_OK);
    CHECK_EQ_U64(p.nsteps, 2u);
    CHECK_EQ_U64(p.step[0].m_cmd, 0x0800CC04u);
    CHECK_EQ_U64(p.step[0].trans_len, 1u);
    CHECK_EQ_U64(p.step[0].is_read, 0u);
    CHECK_EQ_U64(p.step[1].m_cmd, 0x1000CC00u);
    CHECK_EQ_U64(p.step[1].trans_len, 1u);
    CHECK_EQ_U64(p.step[1].is_read, 1u);
    /* UIC_INT..VDM_INT 4-byte bulk read on 0x25 (ARCH.md:36) */
    CHECK(s23_geni_i2c_plan_reg_read(0x25u, 4u, &p) == S23_GENI_OK);
    CHECK_EQ_U64(p.step[1].trans_len, 4u);
    /* full 33-byte mailbox write fits */
    CHECK(s23_geni_i2c_plan_reg_write(0x25u, 32u, &p) == S23_GENI_OK);
    CHECK_EQ_U64(p.nsteps, 1u);
    CHECK_EQ_U64(p.step[0].trans_len, 33u);
    CHECK_EQ_U64(p.step[0].m_cmd, 0x08004A00u);

    CHECK(s23_geni_i2c_plan_reg_read(0x66u, 0u, &p) == S23_GENI_ERANGE);
    CHECK_EQ_U64(p.nsteps, 0u);
    CHECK(s23_geni_i2c_plan_reg_read(0x66u, S23_GENI_I2C_MAX_XFER + 1u, &p) == S23_GENI_ERANGE);
    CHECK(s23_geni_i2c_plan_reg_read(0x66u, 0xFFFFFFFFu, &p) == S23_GENI_ERANGE);
    CHECK(s23_geni_i2c_plan_reg_read(0x80u, 1u, &p) == S23_GENI_ERANGE);
    CHECK_EQ_U64(p.nsteps, 0u);
    CHECK(s23_geni_i2c_plan_reg_read(0x66u, 1u, NULL) == S23_GENI_EINVAL);
    CHECK(s23_geni_i2c_plan_reg_write(0x25u, 0u, &p) == S23_GENI_ERANGE);
    CHECK(s23_geni_i2c_plan_reg_write(0x25u, S23_GENI_I2C_MAX_XFER, &p) == S23_GENI_ERANGE);
    CHECK(s23_geni_i2c_plan_reg_write(0x25u, 0xFFFFFFFFu, &p) == S23_GENI_ERANGE);
    CHECK(s23_geni_i2c_plan_reg_write(0x100u, 1u, &p) == S23_GENI_ERANGE);
    CHECK(s23_geni_i2c_plan_reg_write(0x25u, 1u, NULL) == S23_GENI_EINVAL);
}

int main(void)
{
    test_register_offsets_match_live_drivers();
    test_m_cmd_encode_decode();
    test_packing();
    test_fifo_pack_unpack();
    test_rx_fifo_status();
    test_classify();
    test_clock();
    test_se_usable();
    test_plans();
    return test_report("test_s23pmic_geni");
}
