/*
 * s23aud_lpass_proto.h - pure C11 helpers for S23 (dm1q / SM8550) DMIC capture groundwork.
 *
 * NOT included by s23audio.sys yet (no behaviour change). Dependency-free: <stdint.h>/<stddef.h> only,
 * every helper validates its inputs and never shifts/multiplies past 32 bits.
 *
 * Sources of truth (see drivers/s23audio/AUDIO_CAPTURE_GROUND_TRUTH.md for the full evidence):
 *  [FW]   stock ADSP image 03_audio_adsp_haptics/adsp_pil/adsp.mdt (sha256 2b0b805d23cbeaea..) + adsp.bNN:
 *         HWIO block table in segment b09 (parent entry "LPASS" pa 0x06000000 size 0x02000000) and the
 *         static AudioReach module table in segment b33.
 *  [HW]   drivers/s23audio/s23aud_hw.c (#defines used by the working playback path; cross-checked by
 *         tests/c/extract_s23aud_lpass.py + tests/c/test_s23aud_lpass.c).
 *  [LNX]  public Linux sources: sound/soc/codecs/lpass-va-macro.c (DMIC clock divider / DMICn_CTL fields,
 *         VA_MACRO_MCLK_FREQ, va_dmic_clk_enable pair refcount), sound/soc/qcom/qdsp6/audioreach.{c,h}
 *         (module / param IDs, codec-DMA active_channels_mask).
 * Anything not traceable to one of these is deliberately absent (see the TODO list in the .md file).
 */
#ifndef S23AUD_LPASS_PROTO_H
#define S23AUD_LPASS_PROTO_H

#include <stddef.h>
#ifndef _STDINT
#include <stdint.h>
#endif

#if defined(_MSC_VER) && !defined(__cplusplus)
#define S23AUD_INLINE static __inline
#else
#define S23AUD_INLINE static inline
#endif

/* ---------------------------------------------------------------------------------------------
 * 1. LPASS physical block map  [FW] HWIO table, offsets relative to the "LPASS" parent (0x06000000)
 * ------------------------------------------------------------------------------------------- */
#define S23AUD_LPASS_PA_BASE            0x06000000u
#define S23AUD_LPASS_PA_SIZE            0x02000000u

#define S23AUD_TX_MACRO_PA              0x06AE0000u  /* LPASS_TX_TX_MACRO           size 0x1000  */
#define S23AUD_TX_MACRO_SIZE            0x00001000u
#define S23AUD_VA_MACRO_PA              0x06D44000u  /* LPASS_VA_MACRO              size 0x2000  */
#define S23AUD_VA_MACRO_SIZE            0x00002000u
#define S23AUD_LPAIF_RXTX_PA            0x06B40000u  /* LPASS_RXTX_LPASS_LPAIF_RXTX size 0x2B000 */
#define S23AUD_LPAIF_VA_PA              0x06D00000u  /* LPASS_VA_LPAIF_VA           size 0x2B000 */
#define S23AUD_LPAIF_SIZE               0x0002B000u
#define S23AUD_LPI_TLMM_PA              0x06E80000u  /* LPASS_LPASS_TLMM_CSR        size 0x20000 */
#define S23AUD_LPI_TLMM_SIZE            0x00020000u

/* Sub-blocks inside each macro (offsets from the macro base, [FW]) */
#define S23AUD_MACRO_CLK_RST_CTRL_OFF   0x0000u      /* size 0x20 (clock gating: never touch from a probe) */
#define S23AUD_MACRO_CLK_RST_CTRL_SIZE  0x0020u
#define S23AUD_MACRO_TOP_CSR_OFF        0x0080u      /* size 0x80 */
#define S23AUD_MACRO_TOP_CSR_SIZE       0x0080u
#define S23AUD_MACRO_INP_MUX_OFF        0x0100u      /* size 0x60 */
#define S23AUD_MACRO_INP_MUX_SIZE       0x0060u
#define S23AUD_MACRO_DEC_OFF            0x0400u      /* CDC_TXn decimator n at 0x400 + n*0x80, size 0x40 */
#define S23AUD_MACRO_DEC_STRIDE         0x0080u
#define S23AUD_MACRO_DEC_SIZE           0x0040u
#define S23AUD_TX_MACRO_NUM_DEC         8u           /* LPASS_TX_CDC_TX0..TX7 */
#define S23AUD_VA_MACRO_NUM_DEC         4u           /* LPASS_CDC_TX0..TX3 under LPASS_VA_MACRO */

enum s23aud_lpass_kind {
    S23AUD_BLK_CAPTURE_REG = 1, /* macro TOP_CSR / INP_MUX / decimator registers (future Mode-1 reads) */
    S23AUD_BLK_CLOCK       = 2, /* CLK_RST_CTRL, AUDIO_CC, AON_CC: clocks are PRM/ADSP owned        */
    S23AUD_BLK_DMA         = 3, /* LPAIF codec-DMA engines: driven by the ADSP CODEC_DMA modules      */
    S23AUD_BLK_ACCESS_CTRL = 4, /* XPU / VMIDMT protection units: never touch                        */
    S23AUD_BLK_PINCTRL     = 5, /* LPI TLMM: pin muxing, not touched by any S23 driver today          */
    S23AUD_BLK_CONTAINER   = 6  /* whole-macro window (only used for containment checks)            */
};

typedef struct s23aud_lpass_block {
    const char *name;   /* exact HWIO name from the ADSP image */
    uint32_t    pa;
    uint32_t    size;
    uint8_t     kind;   /* enum s23aud_lpass_kind */
} s23aud_lpass_block;

/* Capture-relevant subset of the [FW] HWIO table, physical addresses = 0x06000000 + HWIO offset. */
S23AUD_INLINE const s23aud_lpass_block *s23aud_lpass_blocks(size_t *count)
{
    static const s23aud_lpass_block k[] = {
        { "LPASS_TX_TX_MACRO",                                    0x06AE0000u, 0x1000u,  S23AUD_BLK_CONTAINER },
        { "LPASS_TX_TX_CLK_RST_CTRL_TX_CLK_RST_CTRL_TX_CLK_RST_CTRL", 0x06AE0000u, 0x20u, S23AUD_BLK_CLOCK },
        { "LPASS_TX_TX_TOP_CSR",                                  0x06AE0080u, 0x80u,    S23AUD_BLK_CAPTURE_REG },
        { "LPASS_TX_TX_INP_MUX_TX_INP_MUX_TX_INP_MUX",            0x06AE0100u, 0x60u,    S23AUD_BLK_CAPTURE_REG },
        { "LPASS_TX_CDC_TX0",                                     0x06AE0400u, 0x40u,    S23AUD_BLK_CAPTURE_REG },
        { "LPASS_TX_CDC_TX1",                                     0x06AE0480u, 0x40u,    S23AUD_BLK_CAPTURE_REG },
        { "LPASS_TX_CDC_TX2",                                     0x06AE0500u, 0x40u,    S23AUD_BLK_CAPTURE_REG },
        { "LPASS_TX_CDC_TX3",                                     0x06AE0580u, 0x40u,    S23AUD_BLK_CAPTURE_REG },
        { "LPASS_TX_CDC_TX4",                                     0x06AE0600u, 0x40u,    S23AUD_BLK_CAPTURE_REG },
        { "LPASS_TX_CDC_TX5",                                     0x06AE0680u, 0x40u,    S23AUD_BLK_CAPTURE_REG },
        { "LPASS_TX_CDC_TX6",                                     0x06AE0700u, 0x40u,    S23AUD_BLK_CAPTURE_REG },
        { "LPASS_TX_CDC_TX7",                                     0x06AE0780u, 0x40u,    S23AUD_BLK_CAPTURE_REG },
        { "LPASS_VA_MACRO",                                       0x06D44000u, 0x2000u,  S23AUD_BLK_CONTAINER },
        { "LPASS_VA_CLK_RST_CTRL_VA_CLK_RST_CTRL_VA_CLK_RST_CTRL", 0x06D44000u, 0x20u,   S23AUD_BLK_CLOCK },
        { "LPASS_VA_TOP_CSR",                                     0x06D44080u, 0x80u,    S23AUD_BLK_CAPTURE_REG },
        { "LPASS_VA_INP_MUX",                                     0x06D44100u, 0x60u,    S23AUD_BLK_CAPTURE_REG },
        { "LPASS_CDC_TX0",                                        0x06D44400u, 0x40u,    S23AUD_BLK_CAPTURE_REG },
        { "LPASS_CDC_TX1",                                        0x06D44480u, 0x40u,    S23AUD_BLK_CAPTURE_REG },
        { "LPASS_CDC_TX2",                                        0x06D44500u, 0x40u,    S23AUD_BLK_CAPTURE_REG },
        { "LPASS_CDC_TX3",                                        0x06D44580u, 0x40u,    S23AUD_BLK_CAPTURE_REG },
        { "LPASS_RXTX_LPASS_VMIDMT_RXTX",                         0x06B3D000u, 0x1000u,  S23AUD_BLK_ACCESS_CTRL },
        { "LPASS_RXTX_LPASS_XPU_RXTX",                            0x06B3E000u, 0x1140u,  S23AUD_BLK_ACCESS_CTRL },
        { "LPASS_RXTX_LPASS_LPAIF_RXTX",                          0x06B40000u, 0x2B000u, S23AUD_BLK_DMA },
        { "LPASS_VA_LPAIF_VA",                                    0x06D00000u, 0x2B000u, S23AUD_BLK_DMA },
        { "LPASS_VA_LPASS_VMIDMT_VA",                             0x06D46000u, 0x1000u,  S23AUD_BLK_ACCESS_CTRL },
        { "LPASS_VA_LPASS_XPU_VA",                                0x06D4A000u, 0x1140u,  S23AUD_BLK_ACCESS_CTRL },
        { "AUDIO_CC",                                             0x06BC0000u, 0x38000u, S23AUD_BLK_CLOCK },
        { "MCC_LPASS_AON_CC",                                     0x06E00000u, 0x38000u, S23AUD_BLK_CLOCK },
        { "LPASS_LPASS_TLMM_CSR",                                 0x06E80000u, 0x20000u, S23AUD_BLK_PINCTRL },
    };
    if (count) *count = sizeof(k) / sizeof(k[0]);
    return k;
}

/* 1 if [pa, pa+len) lies completely inside [base, base+size) without 32-bit wraparound. */
S23AUD_INLINE int s23aud_range_inside(uint32_t pa, uint32_t len, uint32_t base, uint32_t size)
{
    if (len == 0u || size == 0u) return 0;
    if (pa < base) return 0;
    if ((uint64_t)pa + (uint64_t)len > (uint64_t)base + (uint64_t)size) return 0;
    return 1;
}

/*
 * Mode-1 read whitelist for a FUTURE capture probe: 1 only for 4-byte aligned dword reads that lie
 * completely inside one S23AUD_BLK_CAPTURE_REG block. Clock / DMA / XPU / VMIDMT / LPI-TLMM blocks are
 * always rejected. NOTE: whitelisting is necessary, not sufficient - the macro registers are only
 * readable while their LPASS clocks are voted (see AUDIO_CAPTURE_GROUND_TRUTH.md "Safety").
 */
S23AUD_INLINE int s23aud_lpass_probe_read_allowed(uint32_t pa, uint32_t len)
{
    size_t n = 0, i;
    const s23aud_lpass_block *b = s23aud_lpass_blocks(&n);
    if (len == 0u || (pa & 3u) != 0u || (len & 3u) != 0u) return 0;
    for (i = 0; i < n; i++) {
        if (b[i].kind != S23AUD_BLK_CAPTURE_REG) continue;
        if (s23aud_range_inside(pa, len, b[i].pa, b[i].size)) return 1;
    }
    return 0;
}

/* Physical address of decimator n (CDC_TXn) of the TX (is_va=0, n<8) or VA (is_va=1, n<4) macro; 0 = invalid */
S23AUD_INLINE uint32_t s23aud_dec_block_pa(int is_va, uint32_t n)
{
    if (is_va) {
        if (n >= S23AUD_VA_MACRO_NUM_DEC) return 0u;
        return S23AUD_VA_MACRO_PA + S23AUD_MACRO_DEC_OFF + n * S23AUD_MACRO_DEC_STRIDE;
    }
    if (n >= S23AUD_TX_MACRO_NUM_DEC) return 0u;
    return S23AUD_TX_MACRO_PA + S23AUD_MACRO_DEC_OFF + n * S23AUD_MACRO_DEC_STRIDE;
}

/* ---------------------------------------------------------------------------------------------
 * 2. VA-macro DMIC clock  [LNX] lpass-va-macro.c; register offsets cross-checked against [FW] TOP_CSR
 * ------------------------------------------------------------------------------------------- */
#define S23AUD_VA_MACRO_MCLK_HZ         9600000u /* VA_MACRO_MCLK_FREQ (Linux); S23 rate itself UNVERIFIED */
#define S23AUD_VA_DMIC0_CTL_OFF         0x0084u  /* CDC_VA_TOP_CSR_DMIC0_CTL; DMIC1/2/3_CTL at +4/+8/+0xC */
#define S23AUD_VA_DMIC_CTL_COUNT        4u       /* one CTL per DMIC pair: DMIC0/1, 2/3, 4/5, 6/7 */
#define S23AUD_VA_NUM_DMIC              8u
#define S23AUD_DMIC_EN_MASK             0x1u     /* CDC_VA_DMIC_EN_MASK     BIT(0)       */
#define S23AUD_DMIC_CLK_SEL_MASK        0xEu     /* CDC_VA_DMIC_CLK_SEL_MASK GENMASK(3,1) */
#define S23AUD_DMIC_CLK_SEL_SHIFT       1u

/* VA_MACRO_CLK_DIV_* enumerators (index written into CLK_SEL) */
enum s23aud_dmic_div_sel {
    S23AUD_DMIC_DIV_2 = 0, S23AUD_DMIC_DIV_3 = 1, S23AUD_DMIC_DIV_4 = 2,
    S23AUD_DMIC_DIV_6 = 3, S23AUD_DMIC_DIV_8 = 4, S23AUD_DMIC_DIV_16 = 5,
    S23AUD_DMIC_DIV_COUNT = 6
};

/* Divider factor for a CLK_SEL index, 0 if the index is invalid. */
S23AUD_INLINE uint32_t s23aud_dmic_div_factor(uint32_t sel)
{
    static const uint32_t f[S23AUD_DMIC_DIV_COUNT] = { 2u, 3u, 4u, 6u, 8u, 16u };
    return sel < (uint32_t)S23AUD_DMIC_DIV_COUNT ? f[sel] : 0u;
}

/* va_macro_validate_dmic_sample_rate(): mclk must be an exact multiple of the DMIC clock and the factor
 * must be one of 2/3/4/6/8/16. Returns 0 and stores the CLK_SEL index, or -1 (out untouched). */
S23AUD_INLINE int s23aud_dmic_clk_div_sel(uint32_t mclk_hz, uint32_t dmic_hz, uint32_t *out_sel)
{
    uint32_t div, sel;
    if (!out_sel || dmic_hz == 0u || mclk_hz == 0u || (mclk_hz % dmic_hz) != 0u) return -1;
    div = mclk_hz / dmic_hz;
    for (sel = 0; sel < (uint32_t)S23AUD_DMIC_DIV_COUNT; sel++) {
        if (s23aud_dmic_div_factor(sel) == div) { *out_sel = sel; return 0; }
    }
    return -1;
}

/* Offset (from the VA macro base) of the DMICn_CTL register serving dmic 0..7, 0 if invalid. */
S23AUD_INLINE uint32_t s23aud_va_dmic_ctl_off(uint32_t dmic)
{
    if (dmic >= S23AUD_VA_NUM_DMIC) return 0u;
    return S23AUD_VA_DMIC0_CTL_OFF + (dmic / 2u) * 4u;
}

/* Read-modify-write value for a DMICn_CTL register: CLK_SEL[3:1] = sel, EN[0] = enable; other bits kept.
 * Returns -1 for an invalid sel (out untouched). */
S23AUD_INLINE int s23aud_va_dmic_ctl_value(uint32_t old, uint32_t sel, int enable, uint32_t *out)
{
    uint32_t v;
    if (!out || sel >= (uint32_t)S23AUD_DMIC_DIV_COUNT) return -1;
    v = old & ~(S23AUD_DMIC_CLK_SEL_MASK | S23AUD_DMIC_EN_MASK);
    v |= (sel << S23AUD_DMIC_CLK_SEL_SHIFT) & S23AUD_DMIC_CLK_SEL_MASK;
    v |= enable ? S23AUD_DMIC_EN_MASK : 0u;
    *out = v;
    return 0;
}

/* va_dmic_clk_enable(): the two DMICs of a pair share one clock; refcount per pair. */
typedef struct s23aud_dmic_clk_state {
    uint8_t pair_cnt[S23AUD_VA_DMIC_CTL_COUNT];
} s23aud_dmic_clk_state;

enum s23aud_dmic_clk_action {
    S23AUD_DMIC_CLK_ERROR   = -1, /* bad dmic index, refcount underflow or saturation: state unchanged */
    S23AUD_DMIC_CLK_NONE    = 0,  /* pair clock already in the requested state                         */
    S23AUD_DMIC_CLK_PROGRAM = 1,  /* first user: program CLK_SEL then set EN                              */
    S23AUD_DMIC_CLK_DISABLE = 2   /* last user gone: clear EN                                             */
};

S23AUD_INLINE int s23aud_dmic_clk_ref(s23aud_dmic_clk_state *st, uint32_t dmic, int enable)
{
    uint8_t *c;
    if (!st || dmic >= S23AUD_VA_NUM_DMIC) return S23AUD_DMIC_CLK_ERROR;
    c = &st->pair_cnt[dmic / 2u];
    if (enable) {
        if (*c == UINT8_MAX) return S23AUD_DMIC_CLK_ERROR;
        (*c)++;
        return *c == 1u ? S23AUD_DMIC_CLK_PROGRAM : S23AUD_DMIC_CLK_NONE;
    }
    if (*c == 0u) return S23AUD_DMIC_CLK_ERROR;
    (*c)--;
    return *c == 0u ? S23AUD_DMIC_CLK_DISABLE : S23AUD_DMIC_CLK_NONE;
}

/* ---------------------------------------------------------------------------------------------
 * 3. AudioReach IDs for a capture graph
 *    [HW] = already used by the playback path in s23aud_hw.c (checked byte-for-byte by the test)
 *    [FW] = present in the ADSP static module table (segment b33)   [LNX] = public Linux audioreach.h
 * ------------------------------------------------------------------------------------------- */
#define S23AUD_AR_MODULE_ID_WR_SHARED_MEM_EP   0x07001000u /* [HW][FW][LNX] APPS -> DSP (playback) */
#define S23AUD_AR_MODULE_ID_RD_SHARED_MEM_EP   0x07001001u /* [FW][LNX]     DSP -> APPS (capture)  */
#define S23AUD_AR_MODULE_ID_PCM_CNV            0x07001003u /* [HW][LNX]                            */
#define S23AUD_AR_MODULE_ID_PCM_ENC            0x07001004u /* [FW][LNX]                            */
#define S23AUD_AR_MODULE_ID_PCM_DEC            0x07001005u /* [HW][FW][LNX]                        */
#define S23AUD_AR_MODULE_ID_I2S_SINK           0x0700100Au /* [HW][FW][LNX]                        */
#define S23AUD_AR_MODULE_ID_I2S_SOURCE         0x0700100Bu /* [FW][LNX]                            */
#define S23AUD_AR_MODULE_ID_CODEC_DMA_SINK     0x07001023u /* [FW][LNX]                            */
#define S23AUD_AR_MODULE_ID_CODEC_DMA_SOURCE   0x07001024u /* [FW][LNX] LPAIF RXTX/VA TX -> graph  */
#define S23AUD_AR_PARAM_ID_CODEC_DMA_INTF_CFG  0x08001063u /* [LNX] only (param IDs are code immediates) */
#define S23AUD_AR_PARAM_ID_RSC_LPASS_CORE      0x0800102Bu /* [HW] */
#define S23AUD_AR_PARAM_ID_RSC_AUDIO_HW_CLK    0x0800102Cu /* [HW] */
#define S23AUD_AR_PRM_CMD_REQUEST_HW_RSC       0x0100100Fu /* [HW] */
#define S23AUD_AR_PRM_CMD_RELEASE_HW_RSC       0x01001010u /* [HW] */
#define S23AUD_AR_APM_CMD_GRAPH_OPEN           0x01001000u /* [HW] */
#define S23AUD_AR_APM_CMD_GRAPH_PREPARE        0x01001001u /* [HW] */
#define S23AUD_AR_APM_CMD_GRAPH_START          0x01001002u /* [HW] */
#define S23AUD_AR_APM_CMD_GRAPH_STOP           0x01001003u /* [HW] */
#define S23AUD_AR_APM_CMD_GRAPH_CLOSE          0x01001004u /* [HW] */

/* Codec-DMA active_channels_mask = (1 << num_channels) - 1 (Linux audioreach_codec_dma_set_media_format).
 * Valid for 1..8 channels; 0 = invalid request. */
#define S23AUD_CODEC_DMA_MAX_CH 8u
S23AUD_INLINE uint32_t s23aud_codec_dma_ch_mask(uint32_t num_channels)
{
    if (num_channels == 0u || num_channels > S23AUD_CODEC_DMA_MAX_CH) return 0u;
    return (1u << num_channels) - 1u;
}

/* ---------------------------------------------------------------------------------------------
 * 4. PCM capture format arithmetic for the future PortCls capture pin (WAVEFORMATEX rules)
 * ------------------------------------------------------------------------------------------- */
#define S23AUD_CAP_RATE_HZ   48000u /* = S23_SAMPLE_RATE of the playback path; capture rate UNVERIFIED on device */
#define S23AUD_CAP_BITS      16u
#define S23AUD_CAP_MAX_CH    4u     /* repo doc: resourcemanager.xml capture backend "1-4 ch" (UNVERIFIED) */

/* nBlockAlign = channels * bits/8. Returns 0 for unsupported input (bits not 8/16/24/32, ch 0 or > 8). */
S23AUD_INLINE uint32_t s23aud_pcm_block_align(uint32_t channels, uint32_t bits)
{
    if (channels == 0u || channels > S23AUD_CODEC_DMA_MAX_CH) return 0u;
    if (bits != 8u && bits != 16u && bits != 24u && bits != 32u) return 0u;
    return channels * (bits / 8u);
}

/* Bytes for `ms` milliseconds of PCM; requires rate*ms to be a whole number of frames (rate*ms % 1000 == 0).
 * Returns 0 on success, -1 on invalid input or 32-bit overflow (out untouched). */
S23AUD_INLINE int s23aud_pcm_bytes_for_ms(uint32_t rate, uint32_t channels, uint32_t bits, uint32_t ms,
                                          uint32_t *out)
{
    uint64_t frames_x1000, bytes;
    uint32_t ba = s23aud_pcm_block_align(channels, bits);
    if (!out || ba == 0u || rate == 0u || ms == 0u) return -1;
    frames_x1000 = (uint64_t)rate * (uint64_t)ms;
    if (frames_x1000 % 1000u) return -1;
    bytes = (frames_x1000 / 1000u) * (uint64_t)ba;
    if (bytes > 0xFFFFFFFFu) return -1;
    *out = (uint32_t)bytes;
    return 0;
}

/* The only capture format this groundwork plans for: 48 kHz, 16-bit, 1..4 channels. 0 = accepted. */
S23AUD_INLINE int s23aud_capture_fmt_check(uint32_t rate, uint32_t channels, uint32_t bits)
{
    if (rate != S23AUD_CAP_RATE_HZ || bits != S23AUD_CAP_BITS) return -1;
    if (channels == 0u || channels > S23AUD_CAP_MAX_CH) return -1;
    return 0;
}

#endif /* S23AUD_LPASS_PROTO_H */
