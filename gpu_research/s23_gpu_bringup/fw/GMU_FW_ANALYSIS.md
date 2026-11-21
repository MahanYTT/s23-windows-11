# Samsung Galaxy S23 (`dm1q` / `SM8550-AC`) Adreno 740 (`GEN7_2_1`) — GMU & SQE Firmware Analysis

**Date:** 2026-11-21  
**Target GPU:** Adreno 740 (`ADRENO_REV_GEN7_2_1`, ChipID `0x07020100`, `SM8550-AC` "Snapdragon 8 Gen 2 for Galaxy", 719 MHz top bin)  
**Comparison Baseline:** `repos\oneplus-open-windows\experiments\G1287-cold-aperture\` (OnePlus Open SM8550 `0x07020100` cold-aperture native GPU bring-up)

---

## 1. Offline Extraction Status & Exact S23 Firmware Filenames

Verified directly from S23's stock graphics kernel driver (`/home/mahan/s23/src/vendor/qcom/opensource/graphics-kernel/adreno-gpulist.h`, L2073–2105):

| Role | S23 Stock Filename (`adreno-gpulist.h`) | Upstream Linux / Freedreno Name | Verified On-Disk & Live Status (Lineage Recovery Pulled) |
|---|---|---|---|
| **GMU Cortex-M3 Firmware** | **`gmu_gen70200.bin`** (`GMU_VERSION(4, 1, 0)`) | `a740_gmu.bin` | **PULLED & EXACT MATCH** (`66,844 B`, `15` blocks, `DTCM_VER=0x40010009`, SHA-256 `1a2a419c39046d3141fc5fed5aa7f971de2db40cc7a1d89693c3e26fad64dd98` == `G191_STOCK_GMU_SHA256`) |
| **CP SQE Microcode** | **`a740_sqe.fw`** | `a740_sqe.fw` / `gen70200_sqe.fw` | **PULLED & EXACT MATCH** (`76,724 B`, SHA-256 `96fee336424b139100fc60b5b45a907360e4b3936d7e1d00406b9bd80ca48473` == `G433_SQE_SHA256`) |
| **TrustZone PAS ZAP Shader** | **`a740_zap.mbn`** / `.elf` / `.mdt`+`.b00..b02` | `a740_zap.mbn` | **PULLED** (`a740_zap.mbn` `12,088 B`, SHA-256 `362882eca2c5112b5aa1b8b56c2606947b7b73314070c9c514cf108bbffadfa8`; `a740_zap.elf` `5,168 B`, `3cbdf8d0ca6744c368170e8d248c409c3b93963e56fa1ef0d3951eae0ef5148b`; `.mdt` `4,044 B` + `.b00..b02`) |
| **RSCC Sequencer Ucode** | Embedded in `adreno_gen7_gmu.c` L191–195 (`20 B`) | `GEN7_2_0_RSCC_SEQ_MEM_0..4_DRV0` | **EXTRACTED & EXACT MATCH** -> `fw\s23_rsc_seq_ucode.bin` (`20 B`, SHA-256 `20175df4f9aafab7f958205d71bf931a30a484bd6e94db348da5497a4e13ce90` == `G482RscSha`) |

> **Important Note:** `C:\Users\Mahan\Documents\windows-on-android\device_firmware\firmware\` contains Galaxy A52 (`SM7125` / Adreno 618: `a618_gmu.bin`, `a630_sqe.fw`, etc.) and **MUST NOT** be used on S23.

### Read-Only Lineage Recovery Extraction & Stage 5B Live Hardware Verification
Pulled strictly read-only from `super/vendor` (`/dev/block/sda30`, `erofs`) via [`pull_s23_gpu_fw.py`](pull_s23_gpu_fw.py) into `gpu_research\s23_gpu_bringup\fw\s23_vendor_gpu_fw\` and verified with [`extract_and_verify_s23_gpu_fw.py`](extract_and_verify_s23_gpu_fw.py):
```powershell
python gpu_research/s23_gpu_bringup/fw/pull_s23_gpu_fw.py
```
Live execution on Windows (`s23gpuprobe.sys v1.0.0.5`, `StageMask = 0x3B`, captured in `drivers\s23gpuprobe\s23gpuprobe_v5_s5b_live.log`):
- Loaded `C:\Users\Public\gmu_gen70200.bin` (`66,844 B`), parsed all `15` blocks (`ITCM=1` [`476 B`], `ICACHE=1` [`66,064 B` @ `0x00004000`], `DTCM=2` [`40 B` + `24 B`], `META=11`).
- Wrote and verified all `ITCM` and `DTCM` words against live GMU TCM hardware with **0 mismatches** (`ITCM/DTCM verified cleanly`), and safely gated CM3 reset release because Region 2 (`ICACHE` IOVA `0x00004000`) requires active `KGSL-SMMU` CB5 page tables (`OpenSmmuD0` + `OpenGpuStart`).

---

## 2. S23 (`GEN7_2_1` / `SM8550-AC`) vs `G1287-cold-aperture` Contract Comparison

### 2.1 RSCC Sequencer Microcode — **100% BIT-EXACT MATCH**
From S23 `/home/mahan/s23/src/vendor/qcom/opensource/graphics-kernel/adreno_gen7_gmu.c` L191–195:
- 5 little-endian 32-bit words written to `GEN7_2_0_RSCC_SEQ_MEM_0_DRV0` (`0x03D50550..0x03D50560`):
  `0xEAAAE5A0, 0xE1A1EBAB, 0xA2E0A581, 0xECAC82E2, 0x0020EDAD` (`20 bytes`)
- **S23 SHA-256:** `20175df4f9aafab7f958205d71bf931a30a484bd6e94db348da5497a4e13ce90`
- **G1287 `G482RscSha`:** `20175df4f9aafab7f958205d71bf931a30a484bd6e94db348da5497a4e13ce90` (**IDENTICAL!**)
- **PDC Setup (`gen7_load_pdc_ucode`, `0x0B280000`):** `0x0B284520 = 0x00000000`, `0x0B284500 = 0x80000001` — **IDENTICAL** to `g482_board.c` L97!
- **RSCC Handshake (`gen7_load_rsc_ucode`, `0x03D50000`):** `0x010=BIT(24)`, `0x02C=1`, `0x038/0x034/0x040/0x03C=0`, `0x048=0x80000021`, `0x044=0`, `0x400=0`, `0x020=0x4520`, `0x024=0x4510`, `0x028=0x4514` — **IDENTICAL** to `g482_board.c` L98–100!

### 2.2 GMU & SQE Firmware Format & `G1287` Pinned Profiles
- **GMU Firmware (`gmu_gen70200.bin`):** Sequence of 16-byte `struct gmu_block_header { u32 addr; u32 size; u32 type; u32 value; }` entries followed by `size` payload bytes (`adreno_gen7_gmu.c` L448–485 & `AdrenoFirmware.h`):
  - `size == 0`: Metadata / pre-allocation requests (`GMU_BLK_TYPE_PREALLOC_REQ`, `GMU_BLK_TYPE_PREALLOC_PERSIST_REQ`) guaranteed to precede loadable blocks.
  - `ITCM`: GMU VA `[0x00000000, 0x00004000)` -> MMIO `0x03D6D000..0x03D70FFC` (`Core + 0x3000`, `GEN7_GMU_CM3_ITCM_START = 0x1B400`).
  - `ICACHE`: GMU VA `[0x00004000, 0x01000000)` -> GPU SMMU CB5 mapped DDR backing.
  - `DTCM`: GMU VA `[0x10004000, 0x10008000)` -> MMIO `0x03D71000..0x03D74FFC` (`Core + 0x7000`, `GEN7_GMU_CM3_DTCM_START = 0x1C400`); version word at `0x10007FE0` -> MMIO `0x03D74FE0` (`G190_DTCM_VERSION`).
  - **OnePlus Open `G1287` Pins (`g191_stock_gmu_profile.h` / `g433_sqe_pin.h`):**
    - `G191_STOCK_GMU_BYTES = 66,844 B`, `G191_STOCK_GMU_BLOCKS = 15`, `SHA256 = 1a2a419c39046d3141fc5fed5aa7f971de2db40cc7a1d89693c3e26fad64dd98`.
    - `G433_SQE_FILE_BYTES = 76,724 B`, `SHA256 = 96fee336424b139100fc60b5b45a907360e4b3936d7e1d00406b9bd80ca48473`.
  - Note: S23 Android 14/15/16 firmware may carry a newer Qualcomm patch revision of `gmu_gen70200.bin` / `a740_sqe.fw` with identical block architecture; `extract_and_verify_s23_gpu_fw.py` validates structure independently of OnePlus's exact hash pin.

### 2.3 Verified Physical MMIO Aperture Map (S23 `s23_live.dts` == `G482_BINDING`)
| Bank | `G482_BANK` Name | Verified S23 Physical Base | Size | Key Registers |
|---|---|---|---:|---|
| KGSL GX Core | `G482_GPU` | `0x03D00000` | `0x40000` | RBBM (`+0x840`), CP (`+0x2018`), GBIF (`+0xF114`), SECVID (`+0x3D000`) |
| **GPU RSCC** | **`G482_RSCC`** | **`0x03D50000`** *(NOT `0x03D8C000`!)* | `0x10000` | `STATUS0 (+0x10)`, `PDC_SEQ (+0x20)`, `SEQ_BUSY (+0x404)`, `SEQ_MEM_0..4 (+0x550)`, `TCS0..3 (+0xD28/0xFC8/0x1268/0x1508)` |
| GMU Low (TCM + CX + AO) | `G482_GMU_LOW` | `0x03D68000` (`Core = 0x03D6A000`) | `0x28000` | ITCM `0x03D6D000`, DTCM `0x03D71000` (`VER 0x03D74FE0`), CM3/HFI `0x03D7E000`, AO `0x03D8EC00` (`RSCC_REQ/ACK 0x03D8EC1C/20`) |
| GPUCC | *(MMU1 owned)* | `0x03D90000` | `0x0A000` | `PLL0/1`, `GX_GDSCR (0x905C)`, `CX_GDSCR (0x9108)`, `ACD_BCR (0x9358)`, `MEMNOC_GFX (0x9150)` |
| GMU High / `CX_MISC` | `G482_GMU_HIGH` | `0x03D9A000` (`CX_MISC = 0x03D9E000`) | `0x05000` | `GEN7_GPU_CX_MISC_TCM_RET_CNTL (0x03D9E0E4)` |
| **GPU PDC** | **`G482_PDC`** | **`0x0B280000`** | `0x10000` | `PDC_GPU_ENABLE_PDC (0x0B284500)`, `MATCH_LO/HI (0x0B284510/14)`, `SEQ_START_ADDR (0x0B284520)` |

*(Note: `0x03D8B000..0x03D8CFFF` in `s23_live.dts` is `isense_cntl`, NOT `RSCC`! `s23gpuprobe v1.0.0.4` Stage 4 reads the verified `0x03D50000` `G482_RSCC` block and `0x03D8EC1C/20` `GMU_RSCC_CONTROL_REQ/ACK` safely.)*

### 2.4 OPP Table Delta (`SM8550-AC` 719 MHz vs `G196Policy` 680 MHz)
From `s23_live.dts` L16128 (`qcom,gpu-pwrlevels-0`):
- **Shared 9 active levels (`124.8` -> `680 MHz`):** `124.8, 220, 295, 348, 401, 475, 550, 615, 680 MHz` — **100% identical frequencies** to `G196Policy`!
- **S23 `SM8550-AC` Top Bin (level 0):** **`719,000,000 Hz`** (`0x2ADB11C0`) at ARC `vlvl = 0xE0`, `acd-level = 0x882E5FFD`, `bus-freq = 9`.
- **Safe Bring-Up Rule:** Initial HFI DCVS boot on S23 can safely use the exact `G196Policy` `124.8..680 MHz` 10-entry table (with `acd_enable_mask = 0`, `enable_ifpc = 0`) before optionally adding the `719 MHz` AC top bin!

---

## 3. Root-Cause Interpretation of `s23gpuprobe_s7_live.log` (Why GX Stayed `0x6822F800`)

In `drivers\s23gpuprobe\s23gpuprobe_s7_live.log`:
- **CX + GMU Aperture is ALREADY LIVE AND PROVEN:** `GPUCC_CX_GDSCR = 0xF822F800`, `GPUCC_PLL0/1 = 0xE0000005`, `GMU_CM3_CFG = 0x00004052`, `GMU_AO_CNT` ticking at 19.2 MHz, and `GMU_GENERAL_0` scratch R/W **PASSED**.
- **Why `GPUCC_GX_GDSCR` stayed `0x6822F800` (bit 31 `PWR_ON = 0`) even after `gfx.lvl` vote:** On Adreno 740 (`GEN7_2_1`), `GX_GDSC` is **GMU/RSCC-controlled**. Direct APPS CPU clearing of `SW_COLLAPSE` cannot open GX while `GMU_CM3_SYSRESET == 1` (`0x03D7E000 = 1`) and `GMU_GFX_PWR_CLK_STATUS == 0x00000002` (GX collapsed).
- **Exact Next Path to Open `0x03D00000` (KGSL GX):**
  1. Run **`s23gpuprobe v1.0.0.4` Stage 4 (`StageMask = 0x0B`)** to confirm clean cold preflight on `ITCM/DTCM`, `CX_MISC_TCM_RET_CNTL (0x03D9E0E4 == 0)`, `RSCC_SEQ_BUSY_DRV0 (0x03D50404 == 0)`, `RSCC_TCS0..3`, and `GMU_PDC (0x0B284500)`.
  2. Pull S23's `gmu_gen70200.bin` & `a740_sqe.fw` from `/vendor/firmware/` and validate with `extract_and_verify_s23_gpu_fw.py`.
  3. Follow the exact `adreno_gen7_gmu.c` / `G482` + `G440` sequence: load `gmu_gen70200.bin` into ITCM/DTCM/ICACHE -> set `TCM_RET_CNTL=1` -> release `GMU_CM3_SYSRESET=0` -> wait `FW_INIT_RESULT=0x100` -> load `PDC` + 20-byte `RSCC` ucode -> start HFI + `HFI30` DCVS vote + `OOB_REQUEST (BIT 30)` on `GMU_H2G_SET` -> `GX_GDSC` powers ON (`GMU_GFX_PWR_CLK_STATUS[1:0] == 0`)!
