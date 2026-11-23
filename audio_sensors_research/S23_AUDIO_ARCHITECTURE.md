# Samsung Galaxy S23 (`SM-S911B` / `dm1q`) — Native Windows 11 ARM64 Audio Architecture

## 1. Hardware Topology Overview

On the Samsung Galaxy S23 (`SM8550-AC` / `Kalama`), speaker playback and digital/analog microphone capture are split across:
1. **Qualcomm ADSP (`remoteproc-adsp@03000000`) + AudioReach (SPF)**:
   - Manages Low Power Audio Subsystem (`LPASS`), hardware endpoint DMA/I2S/TDM clocking via **PRM**, and PCM graph routing/mixing via **APM** over **GPRv2** (`"adsp_apps"` GLink channel).
2. **Dual Cirrus Logic CS35L43 Smart Speaker Amplifiers (with Halo DSP)**:
   - Control bus: `QUPv3_I2C_HUB SE6` (`0x00998000`, `GPIO8` SDA `fn=1`, `GPIO9` SCL `fn=1`, GSI `502`).
   - Devices (verified live on S23 via `s23sensprobe`, both returning `DEVID = 0x00035A42`, `REVID = 0x000000A1`):
     - **`0x40` (`cs35l43-bot`)**: Bottom / Right speaker amplifier, `RST = GPIO42`, `ASPRX1` TDM slot `1`.
     - **`0x41` (`cs35l43-rcv`)**: Top Earpiece / Left stereo speaker amplifier, `RST = GPIO55`, `ASPRX1` TDM slot `0`.
   - Shared interrupt line: `GPIO14` (open-drain, active-low).
   - Digital Audio Interface: **Primary MI2S / PRI_TDM0 (`LPAIF` Primary)** on `GPIO126` (`i2s0_sck`), `GPIO127` (`i2s0_data0`, din), `GPIO128` (`i2s0_data1`, dout), `GPIO129` (`i2s0_ws`), TLMM `FUNC_SEL = 1`.
3. **Cirrus Logic CS40L26A Haptic Actuator Driver**:
   - Control bus: `QUPv3_I2C_HUB SE3` (`0x0098C000`, `GPIO22/23`, `@0x40`, live `DEVID = 0x0040A260`, `REVID = 0x000000A1`).
   - `RST = GPIO145` (active-low released HIGH), `IRQ = GPIO88` (`s23_live.dts` verified).
4. **Bluetooth Audio (`QCA6490`)**:
   - Hosted over `UART14` (`0x00898000`) by `s23bthx.sys v1.0.0.5` (`RING_SLOTS = 64`, `MAX_PENDING_IRP = 32`, `MaxAclTransferInSize = 1025`, burst TX/RX pacing), pairing with inbox `BthA2dp.sys` / `Microsoft.Bluetooth.Audio`.

---

## 2. ACPI & Memory Map Design (`s23_audio.asl` & UEFI Note)

### ACPI SSDT (`audio_sensors_research/s23_audio.asl`)
Compiles cleanly with `iasl -tc` (`0 Errors, 0 Warnings`), declaring two isolated hardware-descriptive devices under `\_SB`:
- `\_SB.AUD0` (`_HID = "ACPI\S23A8550"`): aggregates `I2C_HUB SE6` (`0x00998000`), `QUPv3_HUB` (`0x00980000`), `IPCC` (`0x00408000`), `AOSS QMP` (`0x0C300000`), `RPMh TCS` (`0x17A00000`), and `TLMM` (`0x0F100000`), plus `GPIO8/9/14/42/55/126/127/128/129` `_DSD` metadata.
- `\_SB.HAP0` (`_HID = "ACPI\S23H4026"`): describes `CS40L26A` on `I2C_HUB SE3` (`0x0098C000`, `@0x40`, `RST = GPIO145`, `IRQ = GPIO88`).

Also supports software-enumerated fallback `ROOT\S23AUDIO` so Windows Update / live deployment works immediately without requiring UEFI DSDT reflashing.

### Stock ADSP Memory Regions & Mu-Silicium `MemoryMapLib.c` Note
From `s23_live.dts` and stock `apnhlos_image`:
- `q6_adsp_dtb_region@9e980000`: base `0x9E980000`, size `0x00080000` (`512 KiB`, `PAS_ID = 36`, `adsp_dtb.mdt`).
- `adspslpi_region@9ea00000`: base `0x9EA00000`, size `0x059B4000` (`89.70 MiB`, ends `0xA43B4000`, `PAS_ID = 1`, `adsp.mdt`).

> [!IMPORTANT]
> In `Mu-Silicium` (`Binaries/dm1q/RawFiles/uefiplat.cfg` & `MemoryMapLib.c`), `"PIL Reserved"` (`UNCACHED_UNBUFFERED_XN`) stops at `0xA2A80000` (`0x8A800000 + 0x18280000`), and `0xA2A80000..0xA5580000` is labeled `"Display Demura"` (`MEM_RES`, `Reserv`, `WRITE_THROUGH_XN`). On S23 `dm1q`, live display scanout uses `0xB8000000`, so `0xA2A80000..0xA43B4000` is unused reserved memory (not allocated as Windows RAM), and `s23audprobe`/`s23audio` maps both PIL carveouts as `MmNonCached` during ELF segment copy and unmaps cleanly via `MmUnmapIoSpace` BEFORE `PAS_AUTH_AND_RESET`. For clean upstream Mu-Silicium builds, extend `"PIL Reserved"` to cover `0x9EA00000..0xA4400000` (`UNCACHED_UNBUFFERED_XN`).

---

## 3. ADSP Boot, GLink, and GPRv2 Wire Protocol

### SCM PAS Sequence (`qcom_q6v5_pas.c` + `s23_live.dts`)
1. Copy `adsp_dtb.mdt` (ELF header + PHDRs + hash) to 4 KB-aligned contiguous non-cached DMA buffer `< 4 GiB`, invoke `PAS_INIT_IMAGE(36, metaPa)` (`0x42000201`), `PAS_MEM_SETUP(36, 0x9E980000, 0x80000)` (`0x42000202`), load `adsp_dtb.b*` segments into `0x9E980000`, `dsb sy`, `MmUnmapIoSpace`.
2. Enable PMIC/clock votes via RPMh TCS0 (`xo.lvl=0x300C0`, `lcx.lvl=0x30030`, `lmx.lvl=0x30040`, `lnoc.lvl=0x300B0`, `LP0=0x50054`, `ldob1=0x40800 @ 1800 mV`) and signal AOP QMP `"{class: image, res: load_state, name: adsp, val: on}"`.
3. `PAS_AUTH_AND_RESET(36)` (`0x42000205`) — **DTB must be authenticated before ADSP**.
4. Repeat ELF load for `adsp.mdt` (`PAS_ID = 1`, `0x9EA00000`, `0x059B4000`), zero-fill `memsz - filesz`, `dsb sy`, `MmUnmapIoSpace`, then `PAS_AUTH_AND_RESET(1)`.
5. Poll `SMP2P` SMEM item `443` (`0x1BB`, magic `0x504D5324`) entry `"slave-kernel"` for bit `1` (`ready`) and SMEM item `480` (`RX FIFO_1`) creation.

### GLink SMEM Native (`APPS:ADSP` `0x19F000`, size `0x60000`)
- Descriptor item `478` (`32 B`): `[0]=tx_tail, [1]=tx_head, [2]=rx_tail, [3]=rx_head`.
- FIFOs: item `479` (`16 KiB` TX `APPS->ADSP`), item `480` (RX `ADSP->APPS`). All pointer advances are 8-byte aligned; IPCC (`0x00408000`) doorbell `client=3, sig=0` (`0x00030000` -> `IPCC_SEND_ID`) notifies ADSP.
- Channel: `"adsp_apps"` (`GLINK_FEATURE_INTENT_REUSE = 1`, 20 reusable 512..2048 B RX intents matching `s23_live.dts` `qcom,gpr`).

### GPRv2 Packet Header (`audio-kernel/include/ipc/gpr-lite.h` & `dsp/audio_prm.c`)
- **24-byte `gpr_hdr`**:
  - `header`: `(ver=0) | (hdr_words=6 << 4) | (pkt_bytes << 8)` — verified directly against Samsung `audio_prm.c` (`GPR_PKT_PACKET_SIZE_SHFT = 8`, NOT mainline `msg_type<<8`).
  - `dst_domain_id = 2` (ADSP), `src_domain_id = 3` (APPS), `src_port`, `dst_port` (`0x1` APM, `0x2` PRM), `token`, `opcode`.

---

## 4. AudioReach Graph, PRM Clocks, & CS35L43 Safety Design

### Verified `adsp.b*` & `kalama.c` Endpoint IDs
- Binary inspection of S23 `adsp.b00..b31` (`34,038,572 B`) confirms `MODULE_ID_WR_SHARED_MEM_EP (0x07001000)`, `MODULE_ID_RD_SHARED_MEM_EP (0x07001001)`, `MODULE_ID_I2S_SINK (0x0700100A)`, and `MODULE_ID_TDM_SINK (0x0700102E)` (`MODULE_ID_AUDIO_IF_SINK 0x0700117C` is absent).
- PRM (`dst_port = 0x2`): `PRM_CMD_REQUEST_HW_RSC (0x0100100F)` enables `PARAM_ID_RSC_LPASS_CORE (0x0800102B)` (`hw_core_id = 1`) and `PARAM_ID_RSC_AUDIO_HW_CLK (0x0800102C)` for `CLOCK_ID_PRI_MI2S_IBIT (0x100)` (`1.536 MHz`) or `CLOCK_ID_PRI_TDM_IBIT (0x200)` (`3.072 MHz`).
- Capture backend (`dm1q/resourcemanager.xml`): `CODEC_DMA-LPAIF_RXTX-TX-3` (`48 kHz`, `16-bit`, `1-4 ch`).

### Dual CS35L43 Register Highlights & Speaker Safety
- Init sequence (`cs35l43.c`): verify `DEVID=0x00035A42` / `REVID=0x000000A1`, write `TST_DAC_MSM_CONFIG (0x7404) = 0x11330000` (`errata_patch`), enable `ALIVE_DCIN_WD (0x7424 bit 0)`, enable VPBR (`0x6404` + `0x201C bit 12`), set `ASP_FRAME_CONTROL5 (0x4820)` (`@0x41` Slot 0, `@0x40` Slot 1), apply `pup_patch` (`0x40<-0x55/0xAA`, `0x2084<-0x000F1AA0`, `0x40<-0xCC/0x33`) before `GLOBAL_ENABLES (0x2014) = 1`.
- Shutdown sequence: mute `AMP_CTRL (0x6000)`, clear `AMP_EN` (`0x2018`), apply `pdn_patch` (`0x2084<-0x000F1AA3`), wait `>=1 ms`, clear `GLOBAL_ENABLES (0x2014) = 0`.
- **Thermal/Excursion Guardrail**: initial volume locked at `-24 dB` (`AMP_VOL_PCM = 0x740`), capped at `-12 dB` (`0x7A0`) and conservative `VBST` / `BST_IPK` while in raw ASP bypass (`DACPCM1_INPUT = 0x08`), relaxing only once Halo DSP `.wmfw`/`.bin` speaker protection (`DACPCM1_INPUT = 0x32`) is loaded and verified healthy.

---

## 5. Windows 11 PortCls WaveRT Driver Architecture (`drivers/s23audio/`)

- **Subdevices**: `IMiniportWaveRT` (`CLSID_PortWaveRT`, 48 kHz 16-bit stereo cyclic zero-copy DMA ring mapped via `APM_CMD_SHARED_MEM_MAP_REGIONS` with `10 ms` / `1920 B` period chunks) + `IMiniportTopology` (`CLSID_PortTopology`, `KSNODETYPE_SPEAKER` volume & mute backed by CS35L43 I2C registers).
- **Streaming Lifecycle**: `KSSTATE_RUN` -> PRM clocks -> AudioReach graph start -> CS35L43 `pup_patch` + unmute -> keep 3 periods queued in flight via `DATA_CMD_WR_SH_MEM_EP_DATA_BUFFER_V2` and advance WaveRT position / fire notification events on each `DATA_CMD_RSP_WR_SH_MEM_EP_DATA_BUFFER_DONE_V2`. `KSSTATE_PAUSE`/`STOP` -> mute -> `pdn_patch` -> graph stop/close -> PRM clock release.
