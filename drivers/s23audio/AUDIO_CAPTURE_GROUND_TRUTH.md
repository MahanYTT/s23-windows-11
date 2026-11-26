# Audio capture (DMIC) — ground truth and status

**Status:** `s23aud_lpass_proto.h` and `../s23haptics/cirrus_fw.h` are now wired into `s23audio.sys`
(`drivers/s23audio/s23aud_hw.h` and `drivers/s23audio/s23aud_hw.c`) behind opt-in registry flags under
`HKLM\SYSTEM\CurrentControlSet\Services\s23audio\Parameters` (`CaptureProbeMode` default `0`,
`InspectSpkProtFw` default `0`). `waveInGetNumDevs` is still `0`, and default playback behaviour,
INF, raw ASP bypass (`0x08`) and the `-6.0 dB` hardware volume ceiling remain unchanged.

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

## Wired Registry Hooks in `s23aud_hw.c` (`HKLM\SYSTEM\CurrentControlSet\Services\s23audio\Parameters`)

1. **`CaptureProbeMode` (`REG_DWORD`, default `0`)**:
   - When `CaptureProbeMode == 1`, inside `S23AudHwStartStream()` — immediately after `EnsureAdspReadyLocked()`
     and `PrmHwCoreReq(PARAM_ID_RSC_LPASS_CORE, 1, TRUE)` succeed so `LPASS_CORE` clocks are voted on —
     `ProbeLpassVaTxMacrosLocked(hw)` maps `S23AUD_TX_MACRO_PA` (`0x06AE0000`, size `0x1000`) and
     `S23AUD_VA_MACRO_PA` (`0x06D44000`, size `0x2000`) as `MmNonCached` (`MapIo`) and reads **only**
     4-byte aligned offsets approved by `s23aud_lpass_probe_read_allowed(pa, 4)`:
     - `VA_MACRO` `TOP_CSR` (`+0x80..+0x98`, including `+0x80` `CORE_ID` and `+0x84..+0x90` `DMIC0..3_CTL`),
       `INP_MUX` (`+0x100..+0x120`), and `CDC_TX0..3` (`+0x400`, `+0x480`, `+0x500`, `+0x580`);
     - `TX_MACRO` `TOP_CSR` (`+0x80..+0x98`), `INP_MUX` (`+0x100..+0x120`), and `CDC_TX0..7`
       (`+0x400 + n*0x80`);
   - Logs all read register values, the computed VA DMIC divider/CTL word (`9.6 MHz -> 2.4 MHz`, `div_sel=2`,
     `factor=4`), and the AudioReach capture module IDs (`CODEC_DMA_SOURCE = 0x07001024`,
     `PCM_ENC = 0x07001004`, `RD_SHARED_MEM_EP = 0x07001001`, `s23aud_codec_dma_ch_mask(2) = 0x3`) to
     `C:\Users\Public\s23audio.log`, then unmaps cleanly with zero writes.

2. **`InspectSpkProtFw` (`REG_DWORD`, default `0`)**:
   - When `InspectSpkProtFw == 1`, during CS35L43 initialization in `InitCs35l43PairLocked(hw)`, the driver
     attempts to load `\SystemRoot\System32\drivers\s23fw\cs35l43-bot-dsp1-spk-prot.wmfw` (`@0x40`) and
     `\SystemRoot\System32\drivers\s23fw\cs35l43-rcv-dsp1-spk-prot.wmfw` (`@0x41`) into a temporary
     non-paged pool buffer, validates them with `cirrus_wmfw_validate()`, and iterates their regions with
     `cirrus_wmfw_iter_init()` / `cirrus_wmfw_iter_next()` while converting word offsets to Halo DSP byte
     register addresses via `cirrus_halo_dsp_addr()`.
   - Logs the region counts, memory/total payload byte counts, and Halo I2C target addresses to
     `C:\Users\Public\s23audio.log`, while keeping the safe raw ASP bypass (`CS35L43_DACPCM1_INPUT = 0x08`)
     and `-6.0 dB` ceiling active.

## Not done / unverified (TODO)

1. Which DMIC lines (0..5) are populated on dm1q, and their LPI TLMM pins. ACDB/topology blobs not decoded.
2. The S23 VA-macro MCLK rate (the header uses Linux's 9.6 MHz) and the DMIC clock.
3. Capture channel count (`S23AUD_CAP_MAX_CH = 4` comes from a resourcemanager.xml note, unverified).
4. The capture graph itself, the PortCls capture pin and the INF change that would make `waveInGetNumDevs > 0`.
5. **CS35L43 Halo DSP speaker protection / calibration active I2C upload:** `InspectSpkProtFw=1` validates
   and logs the Halo WMFW regions read-only; streaming the firmware into the CS35L43 amplifiers and switching
   `DACPCM1_INPUT` from `0x08` to `0x32` still requires on-device thermal/excursion limit verification.
