# Audio capture (DMIC) — ground truth and status

**Status:** groundwork only. `s23aud_lpass_proto.h` is **not included** by `s23audio.sys`, and
`waveInGetNumDevs` is still 0. This change alters no playback behaviour, INF or registry value.

## Evidence used

| Tag | Source | What it gave |
|---|---|---|
| [FW] | stock `03_audio_adsp_haptics/adsp_pil/adsp.mdt` (sha256 prefix `2b0b805d23cbeaea`) + `adsp.bNN`, outside the repo | HWIO block table in segment `b09`. Every one of the 29 `{name, offset, size}` entries in `s23aud_lpass_blocks()` was re-matched against it (parent `LPASS` = `0x06000000`, size `0x02000000`). The static AudioReach module table in `b33` contains `0x07001000/01/03/04/05/0A/0B/23/24` once each. |
| [HW] | `drivers/s23audio/s23aud_hw.c` / `.h` (working playback path) | APM/PRM command IDs, `PARAM_ID_RSC_*`, playback module IDs, `S23_SAMPLE_RATE` = 48000. `tests/c/extract_s23aud_lpass.py` copies them and `tests/c/test_s23aud_lpass.c` requires equality. |
| [LNX] | Linux `sound/soc/codecs/lpass-va-macro.c`, `sound/soc/qcom/qdsp6/audioreach.{c,h}` | DMIC CTL field layout (EN bit0, CLK_SEL bits 3:1), divider set 2/3/4/6/8/16, pair refcount, `PARAM_ID_CODEC_DMA_INTF_CFG` = `0x08001063`, codec-DMA channel mask. Param IDs are code immediates and do not appear in `b33`. |

## Safety

- Clock control (`*_CLK_RST_CTRL`, `AUDIO_CC`, `MCC_LPASS_AON_CC`), LPAIF DMA, XPU/VMIDMT and the LPI
  TLMM are **never** whitelisted (`s23aud_lpass_probe_read_allowed` returns 0, and the test sweeps both macros
  byte by byte).
- The whitelist is necessary, not sufficient. The macro registers only read back while their LPASS
  clocks are voted through PRM (`PRM_CMD_REQUEST_HW_RSC`), so an unclocked read can hang the bus. A Mode=1
  probe must request the LPASS core and HW clock resources first, exactly as the playback path does.
- The LPAIF space is XPU-locked to the ADSP on retail Gunyah builds
  (`usb_research/S23_USBC_ARCHITECTURE.md`). Capture must therefore go through an AudioReach graph
  (`CODEC_DMA_SOURCE` → `PCM_ENC`/`PCM_CNV` → `RD_SHARED_MEM_EP`), not through AP-side DMA.

## Not done / unverified (TODO)

1. Which DMIC lines (0..5) are populated on dm1q, and their LPI TLMM pins. ACDB/topology blobs not decoded.
2. The S23 VA-macro MCLK rate (the header uses Linux's 9.6 MHz) and the DMIC clock.
3. Capture channel count (`S23AUD_CAP_MAX_CH = 4` comes from a resourcemanager.xml note, unverified).
4. The capture graph itself, the PortCls capture pin and the INF change that would make `waveInGetNumDevs > 0`.
5. **CS35L43 Halo DSP speaker protection / calibration loader:** not started. The stock
   `cs35l43-{bot,rcv}-dsp1-spk-prot.wmfw/.bin` parse cleanly with `drivers/s23haptics/cirrus_fw.h`
   (see `drivers/s23haptics/CIRRUS_FW_GROUND_TRUTH.md`). Loading them needs thermal/excursion limits
   validated on the device before any write to the amplifiers.

## Next on-device step (future PR, opt-in)

Mode=1 read-only probe: after the existing PRM resource request, read VA/TX `TOP_CSR`,
`INP_MUX` and the `CDC_TXn` decimators through the whitelist while Android-equivalent capture is
*not* running, and log them to `C:\Users\Public\s23audio.log`. No writes. Rollback: Mode=0 (default).
