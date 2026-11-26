/*
 * Host tests for drivers/s23audio/s23aud_lpass_proto.h (DMIC capture groundwork; not included by
 * s23audio.sys). Checks:
 *   - the LPASS block table is self-consistent (inside the 0x06000000 LPASS window, capture registers
 *     inside their macro, never overlapping clock / DMA / XPU / VMIDMT / LPI TLMM blocks);
 *   - the Mode-1 read whitelist over the whole TX/VA macro windows, byte address by byte address;
 *   - DMIC clock divider / CTL register helpers and the per-pair clock refcount;
 *   - every [HW] AudioReach ID equals what s23aud_hw.c uses today (tests/c/extract_s23aud_lpass.py);
 *   - codec-DMA channel mask and PCM byte arithmetic.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "host_test.h"
#include "s23audio/s23aud_lpass_proto.h"
#include "s23aud_lpass_hw.inc"

static int overlaps(uint32_t a, uint32_t al, uint32_t b, uint32_t bl)
{
    return (uint64_t)a < (uint64_t)b + bl && (uint64_t)b < (uint64_t)a + al;
}

static void test_block_table(void)
{
    size_t n = 0, i, j;
    const s23aud_lpass_block *b = s23aud_lpass_blocks(&n);
    size_t caps = 0;
    CHECK(n == 29u);
    CHECK(s23aud_lpass_blocks(NULL) == b);
    for (i = 0; i < n; i++) {
        CHECK(b[i].name != NULL && b[i].name[0] != 0);
        CHECK(b[i].size != 0u);
        CHECK(s23aud_range_inside(b[i].pa, b[i].size, S23AUD_LPASS_PA_BASE, S23AUD_LPASS_PA_SIZE));
        CHECK(b[i].kind >= S23AUD_BLK_CAPTURE_REG && b[i].kind <= S23AUD_BLK_CONTAINER);
        if (b[i].kind != S23AUD_BLK_CAPTURE_REG) continue;
        caps++;
        CHECK(s23aud_range_inside(b[i].pa, b[i].size, S23AUD_TX_MACRO_PA, S23AUD_TX_MACRO_SIZE) ||
              s23aud_range_inside(b[i].pa, b[i].size, S23AUD_VA_MACRO_PA, S23AUD_VA_MACRO_SIZE));
        /* a capture block overlaps nothing but its container macro */
        for (j = 0; j < n; j++) {
            if (j == i || b[j].kind == S23AUD_BLK_CONTAINER) continue;
            CHECK(!overlaps(b[i].pa, b[i].size, b[j].pa, b[j].size));
        }
    }
    CHECK_EQ_U64(caps, 2u + 8u + 2u + 4u);   /* TX TOP/INP_MUX + 8 DEC, VA TOP/INP_MUX + 4 DEC */

    /* decimator helper agrees with the named table entries */
    for (i = 0; i < 12u; i++) {
        int va = i >= 8u;
        uint32_t pa = s23aud_dec_block_pa(va, (uint32_t)(va ? i - 8u : i));
        int found = 0;
        for (j = 0; j < n; j++) found |= (b[j].pa == pa && b[j].size == S23AUD_MACRO_DEC_SIZE);
        CHECK(found);
    }
    CHECK_EQ_U64(s23aud_dec_block_pa(0, 8), 0);
    CHECK_EQ_U64(s23aud_dec_block_pa(1, 4), 0);
    CHECK_EQ_U64(s23aud_dec_block_pa(1, 0xFFFFFFFFu), 0);

    CHECK(!s23aud_range_inside(0, 0, 0, 0x10));
    CHECK(!s23aud_range_inside(0xFFFFFFF0u, 0x20u, 0xFFFFFFF0u, 0x10u));
    CHECK(s23aud_range_inside(0xFFFFFFF0u, 0x10u, 0xFFFFFFF0u, 0x10u));
}

static int in_capture_block(uint32_t pa, uint32_t len)
{
    size_t n = 0, i;
    const s23aud_lpass_block *b = s23aud_lpass_blocks(&n);
    for (i = 0; i < n; i++)
        if (b[i].kind == S23AUD_BLK_CAPTURE_REG && s23aud_range_inside(pa, len, b[i].pa, b[i].size)) return 1;
    return 0;
}

static void test_probe_whitelist(void)
{
    uint32_t pa;
    size_t allowed = 0;
    for (pa = S23AUD_TX_MACRO_PA; pa < S23AUD_TX_MACRO_PA + S23AUD_TX_MACRO_SIZE; pa++) {
        int a4 = s23aud_lpass_probe_read_allowed(pa, 4);
        CHECK_EQ_U64(a4, (pa & 3u) == 0u && in_capture_block(pa, 4));
        allowed += (size_t)a4;
        CHECK_EQ_U64(s23aud_lpass_probe_read_allowed(pa, 8), (pa & 3u) == 0u && in_capture_block(pa, 8));
    }
    for (pa = S23AUD_VA_MACRO_PA; pa < S23AUD_VA_MACRO_PA + S23AUD_VA_MACRO_SIZE; pa++) {
        int a4 = s23aud_lpass_probe_read_allowed(pa, 4);
        CHECK_EQ_U64(a4, (pa & 3u) == 0u && in_capture_block(pa, 4));
        allowed += (size_t)a4;
    }
    /* (0x80 + 0x60 + 8*0x40)/4 TX dwords + (0x80 + 0x60 + 4*0x40)/4 VA dwords */
    CHECK_EQ_U64(allowed, (0x80u + 0x60u + 8u * 0x40u) / 4u + (0x80u + 0x60u + 4u * 0x40u) / 4u);

    /* clock gating, DMA, XPU, VMIDMT, LPI TLMM, AUDIO_CC are never readable */
    CHECK(!s23aud_lpass_probe_read_allowed(S23AUD_TX_MACRO_PA, 4));
    CHECK(!s23aud_lpass_probe_read_allowed(S23AUD_VA_MACRO_PA + 0x1Cu, 4));
    CHECK(!s23aud_lpass_probe_read_allowed(S23AUD_LPAIF_RXTX_PA, 4));
    CHECK(!s23aud_lpass_probe_read_allowed(S23AUD_LPAIF_VA_PA, 4));
    CHECK(!s23aud_lpass_probe_read_allowed(0x06B3E000u, 4));
    CHECK(!s23aud_lpass_probe_read_allowed(0x06D4A000u, 4));
    CHECK(!s23aud_lpass_probe_read_allowed(S23AUD_LPI_TLMM_PA, 4));
    CHECK(!s23aud_lpass_probe_read_allowed(0x06BC0000u, 4));
    /* reads running past the end of a block, zero / odd length, wrap */
    CHECK(s23aud_lpass_probe_read_allowed(S23AUD_VA_MACRO_PA + 0x80u, 0x80u));
    CHECK(!s23aud_lpass_probe_read_allowed(S23AUD_VA_MACRO_PA + 0x80u, 0x84u));
    CHECK(!s23aud_lpass_probe_read_allowed(S23AUD_VA_MACRO_PA + 0x80u, 0));
    CHECK(!s23aud_lpass_probe_read_allowed(S23AUD_VA_MACRO_PA + 0x80u, 2));
    CHECK(!s23aud_lpass_probe_read_allowed(0xFFFFFFFCu, 8));
}

static void test_dmic_clock(void)
{
    uint32_t sel = 99, v = 0, dmic, mclk, d;
    static const uint32_t factors[] = { 2u, 3u, 4u, 6u, 8u, 16u };

    for (sel = 0; sel < 6u; sel++) CHECK_EQ_U64(s23aud_dmic_div_factor(sel), factors[sel]);
    CHECK_EQ_U64(s23aud_dmic_div_factor(6), 0);
    CHECK_EQ_U64(s23aud_dmic_div_factor(0xFFFFFFFFu), 0);

    CHECK(s23aud_dmic_clk_div_sel(S23AUD_VA_MACRO_MCLK_HZ, 2400000u, &sel) == 0);
    CHECK_EQ_U64(sel, S23AUD_DMIC_DIV_4);
    CHECK(s23aud_dmic_clk_div_sel(S23AUD_VA_MACRO_MCLK_HZ, 600000u, &sel) == 0);
    CHECK_EQ_U64(sel, S23AUD_DMIC_DIV_16);
    sel = 77;
    CHECK(s23aud_dmic_clk_div_sel(S23AUD_VA_MACRO_MCLK_HZ, 1920000u, &sel) == -1);   /* /5 */
    CHECK(s23aud_dmic_clk_div_sel(S23AUD_VA_MACRO_MCLK_HZ, 2500000u, &sel) == -1);   /* not exact */
    CHECK(s23aud_dmic_clk_div_sel(S23AUD_VA_MACRO_MCLK_HZ, 0, &sel) == -1);
    CHECK(s23aud_dmic_clk_div_sel(0, 1u, &sel) == -1);
    CHECK(s23aud_dmic_clk_div_sel(S23AUD_VA_MACRO_MCLK_HZ, 2400000u, NULL) == -1);
    CHECK_EQ_U64(sel, 77);
    /* exhaustive: accepted iff mclk is an exact 2/3/4/6/8/16 multiple of the DMIC clock */
    for (mclk = 1u; mclk <= 96u; mclk++) {
        for (d = 1u; d <= 96u; d++) {
            int want = 0;
            size_t k;
            for (k = 0; k < 6u; k++) want |= (mclk == d * factors[k]);
            CHECK_EQ_U64(s23aud_dmic_clk_div_sel(mclk, d, &sel) == 0, want);
            if (want) CHECK_EQ_U64(s23aud_dmic_div_factor(sel) * d, mclk);
        }
    }

    for (dmic = 0; dmic < 8u; dmic++) CHECK_EQ_U64(s23aud_va_dmic_ctl_off(dmic), 0x84u + (dmic / 2u) * 4u);
    CHECK_EQ_U64(s23aud_va_dmic_ctl_off(8), 0);
    /* every DMIC CTL register lies in VA TOP_CSR, i.e. inside the Mode-1 whitelist */
    for (dmic = 0; dmic < 8u; dmic++)
        CHECK(s23aud_lpass_probe_read_allowed(S23AUD_VA_MACRO_PA + s23aud_va_dmic_ctl_off(dmic), 4));

    CHECK(s23aud_va_dmic_ctl_value(0xFFFFFFF0u, S23AUD_DMIC_DIV_4, 1, &v) == 0);
    CHECK_EQ_U64(v, 0xFFFFFFF0u | (2u << 1) | 1u);
    CHECK(s23aud_va_dmic_ctl_value(0xFFFFFFFFu, S23AUD_DMIC_DIV_2, 0, &v) == 0);
    CHECK_EQ_U64(v, 0xFFFFFFF0u);
    v = 5;
    CHECK(s23aud_va_dmic_ctl_value(0, 6, 1, &v) == -1);
    CHECK_EQ_U64(v, 5);
    CHECK(s23aud_va_dmic_ctl_value(0, 0, 1, NULL) == -1);
}

static void test_dmic_refcount(void)
{
    s23aud_dmic_clk_state st;
    unsigned k;
    memset(&st, 0, sizeof(st));
    CHECK(s23aud_dmic_clk_ref(&st, 0, 1) == S23AUD_DMIC_CLK_PROGRAM);
    CHECK(s23aud_dmic_clk_ref(&st, 1, 1) == S23AUD_DMIC_CLK_NONE);      /* same pair */
    CHECK(s23aud_dmic_clk_ref(&st, 2, 1) == S23AUD_DMIC_CLK_PROGRAM);   /* next pair */
    CHECK(s23aud_dmic_clk_ref(&st, 0, 0) == S23AUD_DMIC_CLK_NONE);
    CHECK(s23aud_dmic_clk_ref(&st, 1, 0) == S23AUD_DMIC_CLK_DISABLE);
    CHECK(s23aud_dmic_clk_ref(&st, 1, 0) == S23AUD_DMIC_CLK_ERROR);     /* underflow */
    CHECK_EQ_U64(st.pair_cnt[0], 0);
    CHECK_EQ_U64(st.pair_cnt[1], 1);
    CHECK(s23aud_dmic_clk_ref(&st, 8, 1) == S23AUD_DMIC_CLK_ERROR);
    CHECK(s23aud_dmic_clk_ref(NULL, 0, 1) == S23AUD_DMIC_CLK_ERROR);
    for (k = 1; k < 255u; k++) (void)s23aud_dmic_clk_ref(&st, 6, 1);
    CHECK_EQ_U64(st.pair_cnt[3], 254);
    CHECK(s23aud_dmic_clk_ref(&st, 7, 1) == S23AUD_DMIC_CLK_NONE);
    CHECK(s23aud_dmic_clk_ref(&st, 7, 1) == S23AUD_DMIC_CLK_ERROR);     /* saturation */
    CHECK_EQ_U64(st.pair_cnt[3], 255);
}

static void test_ids_match_playback_path(void)
{
    CHECK_EQ_U64(S23AUD_AR_MODULE_ID_WR_SHARED_MEM_EP, X_MODULE_ID_WR_SHARED_MEM_EP);
    CHECK_EQ_U64(S23AUD_AR_MODULE_ID_PCM_CNV, X_MODULE_ID_PCM_CNV);
    CHECK_EQ_U64(S23AUD_AR_MODULE_ID_PCM_DEC, X_MODULE_ID_PCM_DEC);
    CHECK_EQ_U64(S23AUD_AR_MODULE_ID_I2S_SINK, X_MODULE_ID_I2S_SINK);
    CHECK_EQ_U64(S23AUD_AR_APM_CMD_GRAPH_OPEN, X_APM_CMD_GRAPH_OPEN);
    CHECK_EQ_U64(S23AUD_AR_APM_CMD_GRAPH_PREPARE, X_APM_CMD_GRAPH_PREPARE);
    CHECK_EQ_U64(S23AUD_AR_APM_CMD_GRAPH_START, X_APM_CMD_GRAPH_START);
    CHECK_EQ_U64(S23AUD_AR_APM_CMD_GRAPH_STOP, X_APM_CMD_GRAPH_STOP);
    CHECK_EQ_U64(S23AUD_AR_APM_CMD_GRAPH_CLOSE, X_APM_CMD_GRAPH_CLOSE);
    CHECK_EQ_U64(S23AUD_AR_PRM_CMD_REQUEST_HW_RSC, X_PRM_CMD_REQUEST_HW_RSC);
    CHECK_EQ_U64(S23AUD_AR_PRM_CMD_RELEASE_HW_RSC, X_PRM_CMD_RELEASE_HW_RSC);
    CHECK_EQ_U64(S23AUD_AR_PARAM_ID_RSC_LPASS_CORE, X_PARAM_ID_RSC_LPASS_CORE);
    CHECK_EQ_U64(S23AUD_AR_PARAM_ID_RSC_AUDIO_HW_CLK, X_PARAM_ID_RSC_AUDIO_HW_CLK);
    CHECK_EQ_U64(S23AUD_CAP_RATE_HZ, X_S23_SAMPLE_RATE);
    /* capture IDs are the read-side twins of the playback IDs */
    CHECK_EQ_U64(S23AUD_AR_MODULE_ID_RD_SHARED_MEM_EP, S23AUD_AR_MODULE_ID_WR_SHARED_MEM_EP + 1u);
    CHECK_EQ_U64(S23AUD_AR_MODULE_ID_I2S_SOURCE, S23AUD_AR_MODULE_ID_I2S_SINK + 1u);
    CHECK_EQ_U64(S23AUD_AR_MODULE_ID_CODEC_DMA_SOURCE, S23AUD_AR_MODULE_ID_CODEC_DMA_SINK + 1u);
}

static void test_pcm_math(void)
{
    uint32_t ch, out = 0;
    CHECK_EQ_U64(s23aud_codec_dma_ch_mask(0), 0);
    for (ch = 1; ch <= 8u; ch++) CHECK_EQ_U64(s23aud_codec_dma_ch_mask(ch), (1u << ch) - 1u);
    CHECK_EQ_U64(s23aud_codec_dma_ch_mask(9), 0);
    CHECK_EQ_U64(s23aud_codec_dma_ch_mask(0xFFFFFFFFu), 0);

    CHECK_EQ_U64(s23aud_pcm_block_align(2, 16), 4);
    CHECK_EQ_U64(s23aud_pcm_block_align(8, 32), 32);
    CHECK_EQ_U64(s23aud_pcm_block_align(1, 12), 0);
    CHECK_EQ_U64(s23aud_pcm_block_align(9, 16), 0);
    CHECK(s23aud_pcm_bytes_for_ms(48000, 2, 16, 10, &out) == 0);
    CHECK_EQ_U64(out, 1920);
    CHECK(s23aud_pcm_bytes_for_ms(44100, 2, 16, 10, &out) == 0);
    CHECK_EQ_U64(out, 1764);
    out = 7;
    CHECK(s23aud_pcm_bytes_for_ms(44100, 2, 16, 1, &out) == -1);                  /* 44.1 frames */
    CHECK(s23aud_pcm_bytes_for_ms(0xFFFFFFFFu, 8, 32, 0xFFFFFFFFu, &out) == -1);  /* > 32 bit */
    CHECK(s23aud_pcm_bytes_for_ms(48000, 2, 16, 0, &out) == -1);
    CHECK(s23aud_pcm_bytes_for_ms(48000, 2, 16, 10, NULL) == -1);
    CHECK_EQ_U64(out, 7);

    CHECK(s23aud_capture_fmt_check(48000, 1, 16) == 0);
    CHECK(s23aud_capture_fmt_check(48000, 4, 16) == 0);
    CHECK(s23aud_capture_fmt_check(48000, 5, 16) == -1);
    CHECK(s23aud_capture_fmt_check(48000, 0, 16) == -1);
    CHECK(s23aud_capture_fmt_check(44100, 2, 16) == -1);
    CHECK(s23aud_capture_fmt_check(48000, 2, 24) == -1);
}

int main(void)
{
    test_block_table();
    test_probe_whitelist();
    test_dmic_clock();
    test_dmic_refcount();
    test_ids_match_playback_path();
    test_pcm_math();
    return test_report("test_s23aud_lpass");
}
