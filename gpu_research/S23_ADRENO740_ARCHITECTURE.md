# Samsung Galaxy S23 (`dm1q` / `SM-S911B`, `SM8550-AC`) — Adreno 740 Native Windows 11 ARM64 Architecture & Bring-Up Analysis

**Target Device:** Samsung Galaxy S23 (`dm1q` / `SM-S911B`, `S911BXXS8DYI3`, Qualcomm `SM8550-AC` "Snapdragon 8 Gen 2 for Galaxy")  
**Target OS:** Windows 11 Pro 24H2 ARM64 (`10.0.26100.4349`)  
**GPU Silicon:** Qualcomm Adreno 740 (`GEN7_2_1`, `ADRENO_REV_GEN7_2_1 = 0x070201`, GMU rev `0x07020100`, 3 MiB GMEM, UBWC 4.0)  
**Primary Sources Verified On Disk:**
- Live S23 device tree: `/home/mahan/mu_s23/s23_live.dts`
- S23 Linux kernel sources: `/home/mahan/s23/src/kernel_platform/msm-kernel/drivers/clk/qcom/{gpucc-kalama.c,gcc-kalama.c}`, `/home/mahan/s23/src/vendor/qcom/opensource/graphics-kernel/{adreno-gpulist.h,adreno.c,adreno_gen7.c,adreno_gen7_gmu.c,adreno_gen7_rpmh.c,gen7_reg.h}`
- S23 live `cmd-db` capture: `wifi_research/s23pcieprobe_v8.log`
- OnePlus Open (`SM8550`) Windows GPU research: `repos/oneplus-open-windows/` (`G109`, `G142`, `G143`, `G721`–`G724`, `G736r2`, `G741`, `G778`, `G779`, `G991`, `G1287`, `G1311`, `G1313`, `G1315`, `G1348`, `userspace/mesa-G739`)
- Qualcomm Kailua Windows BSP: `repos/windows_silicon_qcom_kailua/` & `repos/Silicium-ACPI/Silicon/Qualcomm/Kailua/`

---

## 1. Exact S23 (`SM8550-AC`) Adreno 740 Hardware Map

### 1.1 Physical MMIO Regions (`s23_live.dts` + `gpucc-kalama.c`)

All addresses below are verified directly against `/home/mahan/mu_s23/s23_live.dts` (`clock-controller@3d90000`, `qcom,kgsl-3d0@3d00000`, `kgsl-smmu@3da0000`, `qcom,gmu@3d69000`, `apps_rsc@17a00000`, `aop_cmd_db_region@81c60000`) and `gcc-kalama.c`.

| Block | Physical Base | Length | Power / Clock Gate Required | Purpose on S23 (`dm1q`) |
|---|---:|---:|---|---|
| **GCC (GPU clocks)** | `0x00100000` | `0x1F5000` | Always-on APSS AHB | `GPLL0` (`0x00000`), `APCS_VOTE` (`0x52000`/`0x52018`), `GCC_GPU_*` (`0x71000..0x71158`) |
| **KGSL-3D0 (Adreno 740 Core)** | `0x03D00000` | `0x40000` | **CX GDSC + GX GDSC + AHB + GX clocks** | RBBM, CP, UCHE, GBIF, RB/SP/TP/GRAS, `SECVID/TSB` (`0xF400..0xFC01` DWORDs) |
| **GMU AO Block Dec0** | `0x03D40000` | `0x10000` | CX GDSC + AHB | GMU Always-On aperture (`gmu_ao_blk_dec0`) |
| **GPU RSCC** | `0x03D50000` | `0x10000` | CX GDSC + AHB | GPU Resource State Coordinator (`RSCC` DRV0 sequencer & TCS0–2) |
| **CX DBGC** | `0x03D61000` | `0x00800` | CX GDSC + AHB | GPU CX Debug Bus Controller |
| **GMU (`qcom,gmu@3d69000`)** | `0x03D68000` | `0x37000` | CX GDSC + AHB (+ GMU clocks for CM3) | DT window; GMU Core namespace (`G482`) starts at **`0x03D6A000`** (`KGSL + 0x1A800*4`) |
| **GMU Core / CX / HFI** | `0x03D7E000` | `0x01000` | CX GDSC + AHB + CX_GMU | `CM3_SYSRESET (0x03D7E000)`, `GFX_PWR_CLK_STATUS (0x03D7E340)`, `HFI (0x03D7E600)`, `GENERAL_0..10 (0x03D7E714..3C)` |
| **ISENSE CNTL** | `0x03D8B000` | `0x02000` | CX GDSC + AHB | Current/voltage sense control (`isense_cntl`) |
| **GMU AO Control** | `0x03D8EC00` | `0x00100` | CX GDSC + AHB | `AO_HOST_INTR (0x03D8EC14)`, `CX_BUSY_STATUS (0x03D8EC30)`, `AHB_FENCE_CTRL (0x03D8EC40)`, `AO_SPARE_CNTL (0x03D8EC58)` |
| **GPUCC (`kalama-gpucc`)** | `0x03D90000` | `0x0A000` | **`GCC_GPU_CFG_AHB` + `GCC_GPU_BCR == 0`** | `PLL0 (0x0000)`, `PLL1 (0x1000)`, `HLOS1_VOTE_SMMU (0x7000)`, GDSCs & CBCRs (`0x9000..0x9988`) |
| **CX MISC** | `0x03D9E000` | `0x01000` | CX GDSC + AHB | GPU CX miscellaneous / retention controls (`cx_misc`) |
| **KGSL-SMMU (`qsmmu-v500`)** | `0x03DA0000` | `0x40000` | CX GDSC + `HLOS1_VOTE_SMMU` + MEMNOC/DVM/AHB | Dedicated GPU SMMU-500 (`0x03DA0000`), TCU (`0x03DE6000+0x40`), QTB (`0x03DE8000+0x1000`) |
| **GMU PDC** | `0x0B280000` | `0x10000` | AOSS awake | Power Domain Controller for GPU RPMh sleep/wake sequences |
| **APPS RSC DRV2** | `0x17A20000` | `0x02000` | Always-on | HLOS RPMh AMC TCS0 (`0xD00`) for direct ARC rail votes |
| **CMD_DB (`aop_cmd_db`)** | `0x81C60000` | `0x20000` | Reserved RAM | Live RPMh resource address & VLVL lookup table |
| **ChipInfo / SMEM** | `0x81CF4000` / `0x81D00000` | `0x1000` / `0x200000` | Reserved RAM | SoCInfo item 137 (`soc_id=519`, GPU part info) |
| **ZAP Microcode Carveout** | `0x9B09A000` | `0x02000` | `no-map` TZ-protected | `gpu_micro_code_region@9b09a000` (`zap-shader` PAS memory region) |

> [!CAUTION]
> **Strict AXI Bus Error (SError) Gate Rule:**
> 1. Reading **GPUCC (`0x03D90000`)** while `GCC_GPU_BCR (0x00171000)[0] == 1` or `GCC_GPU_CFG_AHB_CBCR (0x00171004)` is halted triggers an unrecoverable synchronous external abort / SError.
> 2. Reading **GMU / RSCC / KGSL-SMMU (`0x03D40000..0x03DEFFFF`)** while `GPUCC_CX_GDSCR (0x03D99108)` `PWR_ON` (bit 31) == `0` or `GPUCC_AHB_CLK (0x03D9911C)` is off triggers an AXI SError.
> 3. Reading **KGSL GX-domain core registers (`0x03D00000..0x03D3FFFF`, e.g. `RBBM_STATUS`, `GBIF_HALT`, `CP_*`)** while `GPUCC_GX_GDSCR (0x03D9905C)` `PWR_ON` (bit 31) == `0` or `GPUCC_GX_CFG_GDSCR (0x03D99060)` bit 15 (`PWR_DOWN_COMPLETE`) == `1` triggers an AXI abort (the exact OnePlus Open `phase-8 0x483` blocker documented in `G723`/`G724`).

---

### 1.2 S23 `SM8550-AC` Binning & Frequency / Voltage Table (719 MHz vs Standard 680 MHz)

Unlike standard `SM8550` devices (`sku-codes = <0x00>`, `qcom,gpu-pwrlevels-1`, peak **680 MHz** at corner `0xC0`), the Samsung Galaxy S23 (`SM8550-AC` "for Galaxy") selects `qcom,gpu-pwrlevels-0` (`qcom,sku-codes = <0x03 0x06 0x300f1 0x300f3 0x500f1 0x500f3 0x700f1 0x700f2 0x700fe 0x700ff>`), adding a **719 MHz Turbo L1** top bin:

| Level (`reg`) | S23 `SM8550-AC` (`pwrlevels-0`) Freq | Hex (`qcom,gpu-freq`) | Corner (`qcom,level`) | ACD Word (`qcom,acd-level`) | DDR Bus (`min/freq/max`) | Standard `SM8550` (`pwrlevels-1`) |
|---:|---:|---:|---:|---:|---|---|
| **0** | **719.0 MHz** | `0x2ADB11C0` | `0xE0` (224) | `0x882E5FFD` | `9 / 9 / 9` | *(absent — caps at 680 MHz)* |
| **1** | **680.0 MHz** | `0x2887FA00` | `0xC0` (192) | `0x882E5FFD` | `5 / 7 / 9` | Level 0 (`680 MHz`, `0xC0`) |
| **2** | **615.0 MHz** | `0x24A827C0` | `0x90` (144) | `0xA82F5FFD` | `3 / 6 / 7` | Level 1 (`615 MHz`, `0x90`) |
| **3** | **550.0 MHz** | `0x20C85580` | `0x80` (128) | `0xE0285FFD` | `2 / 3 / 7` | Level 2 (`550 MHz`, `0x80`) |
| **4** | **475.0 MHz** | `0x1C4FECC0` | `0x50` (80) | `0xE0285FFD` | `1 / 3 / 5` | Level 3 (`475 MHz`, `0x50`) |
| **5** | **401.0 MHz** | `0x17E6C640` | `0x40` (64) | `0xC02A5FFD` | `1 / 3 / 4` | Level 4 (`401 MHz`, `0x40`) |
| **6** | **348.0 MHz** | `0x14BE0F00` | `0x3C` (60) | `0xE02B5FFD` | `1 / 3 / 3` | Level 5 (`348 MHz`, `0x3C`) |
| **7** | **295.0 MHz** | `0x119557C0` | `0x38` (56) | `0xE02D5FFD` | `1 / 3 / 3` | Level 6 (`295 MHz`, `0x38`) |
| **8** *(init)* | **220.0 MHz** | `0x0D1CEF00` | `0x34` (52) | `0xC02F5FFD` | `1 / 1 / 3` | Level 7 (`220 MHz`, `0x34`) |
| **9** *(floor)* | **124.8 MHz** | `0x07704C00` | `0x34` (52) | *(none)* | `1 / 1 / 1` | Level 8 (`124.8 MHz`, `0x34`) |

- **GMU Frequency Table (`qcom,gmu-freq-table`):** `220.0 MHz` (`0x0D1CEF00`, corner `0x40`) and `550.0 MHz` (`0x20C85580`, corner `0x80`).
- **GMU Hub Clock (`gmu_hub_clk_freq`):** `200.0 MHz` (`GPLL0 / 3`, `src=5, hid=5`).
- **GPU Silicon Features (`adreno_gpu_core_gen7_2_1`):** `ADRENO_APRIV | ADRENO_IOCOHERENT | ADRENO_IFPC | ADRENO_CONTENT_PROTECTION | ADRENO_LPAC | ADRENO_BCL | ADRENO_L3_VOTE | ADRENO_ACD`, `gmem_size = 3 MiB`, `uche_gmem_alignment = 16 MiB`, `highest_bank_bit = 16`, `ubwc-mode = 4`, `min-access-length = 32`, `hang_detect_cycles = 0xCFFFFF`.

---

### 1.3 Live S23 `cmd-db` RPMh ARC Rails & `GPUCC` / `GCC` Clock Register Map

Verified against S23's live `cmd-db` dump (`wifi_research/s23pcieprobe_v8.log`) and `s23_live.dts` lines `1093..1301`:

| Rail Purpose | Live S23 `cmd-db` Name | Live `cmd-db` Address | DTS Regulator / Supply Role |
|---|---|---:|---|
| **CX Logic Rail** | `cx.lvl` | **`0x30000`** | `vdd_cx` (`gpucc`), `parent-supply` of `gpu_cc_cx_gdsc`, `vddcx` |
| **MX Memory Rail** | `mx.lvl` | **`0x30010`** | `vdd_mx` (`gpucc`, `gpu_cc_pll1`, `gpu_cc_gmu_clk_src`) |
| **GFX Core Rail** | **`gfx.lvl`** *(note: NOT `gx.lvl`!)* | **`0x30050`** | `pm_v6d_s5_level` -> parent of `pm_v8_s5_gfx_voter_level` -> `gpu_cc_gx_gdsc` |
| **MXC Rail** | **`mxc.lvl`** *(note: NOT `gmxc.lvl`!)* | **`0x300A0`** | `vdd_mxc` (`gpucc`, `gpu_cc_pll0`, `pm_v8_s5_gfx_voter_level` co-rail) |
| **XO Clock Rail** | `xo.lvl` | **`0x300C0`** | `bi_tcxo` (`19.2 MHz` reference for `PLL0`/`PLL1` and `gpu_cc_cxo`) |

> [!IMPORTANT]
> **Critical S23 Naming Fact:** On `SM8550` (`Kalama`/`Kailua`), `cmd-db` exposes **`gfx.lvl` (`0x30050`)** and **`mxc.lvl` (`0x300A0`)** (NOT `gx.lvl` or `gmxc.lvl`). Both `s23gpuprobe.c` and `s23_uefi_gpu_dxe.c` query all candidate names dynamically from `cmd-db` (`0x81C60000`) and decode the exact 16-bit VLVL AUX array before issuing any `APPS_RSC` DRV2 TCS0 vote.

**Verified `GCC` & `GPUCC` Register Offsets (`gcc-kalama.c` & `gpucc-kalama.c`):**

| Register | Physical Address | Type / Control Bits |
|---|---:|---|
| `GCC_GPLL0_MODE` | `0x00100000` | Lucid EVO PLL; bit 31 = `LOCK_DET` (600 MHz main) |
| `GCC_APCS_CLOCK_BRANCH_ENA_VOTE` | `0x00152000` | Bit 15 = `gcc_gpu_gpll0_clk_src`, bit 16 = `gcc_gpu_gpll0_div_clk_src` |
| `GCC_APCS_GPLL_ENA_VOTE` | `0x00152018` | Bit 0 = `GPLL0` AP vote |
| `GCC_GPU_BCR` | `0x00171000` | Bit 0 = GPU subsystem block reset (must be `0`) |
| `GCC_GPU_CFG_AHB_CBCR` | `0x00171004` | Bit 0 = enable, bit 1 = HWCG, bit 31 = `CLK_OFF` |
| `GCC_QMIP_GPU_AHB_CBCR` | `0x00171008` | Bit 0 = enable, bit 1 = HWCG, bit 31 = `CLK_OFF` |
| `GCC_GPU_MEMNOC_GFX_CBCR` | `0x00171010` | Bit 0 = enable, bit 1 = HWCG (`BRANCH_HALT_VOTED`), bit 31 = `CLK_OFF` |
| `GCC_GPU_SNOC_DVM_GFX_CBCR` | `0x00171018` | Bit 0 = enable (`BRANCH_HALT_DELAY`), bit 31 = `CLK_OFF` |
| `GCC_DDRSS_GPU_AXI_CBCR` | `0x00171154` | Bit 0 = enable (`BRANCH_HALT_SKIP` / AON) |
| `GPU_CC_PLL0` (`LUCID_OLE`) | `0x03D90000` | `L=0x1E (cal=0x4444)`, `ALPHA=0xBAAA` (~590 MHz), `CONFIG_CTL=0x20485699/0x00182261/0x82AA299C` |
| `GPU_CC_PLL1` (`LUCID_OLE`) | `0x03D91000` | `L=0x16 (cal=0x4444)`, `ALPHA=0xEAAA` (440.0 MHz for GMU 220 MHz), same `CONFIG_CTL`/`TEST_CTL` |
| `GPU_CC_HLOS1_VOTE_GPU_SMMU_CLK` | `0x03D97000` | Bit 0 = enable, bit 31 = `CLK_OFF` (`BRANCH_HALT_VOTED`) |
| `GPU_CC_CXO_AON_CBCR` | `0x03D99004` | Bit 0 = enable, bit 31 = `CLK_OFF` |
| `GPU_CC_GX_BCR` / `GX_GDSCR` / `GX_CFG` | `0x03D99058` / `905C` / `9060` | `GX_GDSCR[0]=SW_COLLAPSE`, `[31]=PWR_ON`; `GX_CFG[15]=PWR_DOWN_COMPLETE` (`support-cfg-gdscr`) |
| `GPU_CC_GX_GFX3D_CBCR` / `GX_GMU` / `GX_FF` | `0x03D990A8` / `90BC` / `90C0` | GX-domain branches (require GX GDSC on) |
| `GPU_CC_CX_BCR` / `CX_GDSCR` / `CX_HW_CTRL` | `0x03D99104` / `9108` / `953C` | `CX_GDSCR[0]=SW_COLLAPSE`, `[1]=HW_CONTROL`, `[31]=PWR_ON`; `HW_CTRL[31]=PWR_ON` |
| `GPU_CC_AHB_CBCR` / `CRC_AHB` | `0x03D9911C` / `9120` | `AHB` (`BRANCH_HALT_DELAY`), `CRC_AHB` (`BRANCH_HALT_VOTED`); parent `hub_clk_src` |
| `GPU_CC_CX_GMU` / `CXO` / `HUB_CX_INT` / `CX_FF` | `0x03D9913C` / `9144` / `9148` / `914C` | `CX_GMU` (`aon_ops`), `CXO`, `HUB_CX_INT`, `CX_FF` |
| `GPU_CC_MEMNOC_GFX_CBCR` | `0x03D99150` | `BRANCH_HALT_VOTED` |
| `GPU_CC_GMU_CLK_SRC` CMD / CFG | `0x03D99318` / `931C` | `220 MHz`: `PLL1 (src=3)` / `div=2 (hid=3)` + `HW_CLK_CTRL (BIT 20)` = `0x00100303` |
| `GPU_CC_HUB_AON_CBCR` | `0x03D993E8` | `BRANCH_HALT`, parent `hub_clk_src` |
| `GPU_CC_HUB_CLK_SRC` CMD / CFG | `0x03D993EC` / `93F0` | `200 MHz`: `GPLL0 (src=5)` / `div=3 (hid=5)` + `HW_CLK_CTRL (BIT 20)` = `0x00100505` |
| `GPU_CC_FF_CLK_SRC` CMD / CFG | `0x03D99474` / `9478` | `200 MHz`: `GPLL0 (src=5)` / `div=3 (hid=5)` + `HW_CLK_CTRL (BIT 20)` = `0x00100505` |
| `GPU_CC_GX_DOMAIN_MISC3` | `0x03D99504` | GX domain clamp/retention misc control |

---

### 1.4 `KGSL-SMMU` (`0x03DA0000`) & GIC Interrupt Map

Verified from `s23_live.dts` (`qcom,kgsl-3d0@3d00000`, `qcom,gmu@3d69000`, `kgsl-smmu@3da0000`, `qcom,kgsl-iommu@3da0000`) and `Kailua/Decompiled/DSDT_MTP.dsl` + `IORT.dsl`:

| Device / Interrupt | DTS SPI (Hex / Dec) | GIC GSI (`SPI + 32`) | Stream ID / Context Role |
|---|---:|---:|---|
| `kgsl_3d0_irq` (Adreno CP/RBBM) | `0x12C` (`300`) | **`332`** (`0x14C`) | GPU core interrupt |
| `gmu_hfi_irq` (GMU HFI) | `0x130` (`304`) | **`336`** (`0x150`) | HFI message queue IRQ |
| `gmu_irq` (GMU Watchdog/Fault) | `0x131` (`305`) | **`337`** (`0x151`) | GMU core/AO interrupt |
| `kgsl-smmu` Global Fault | `0x2A1` (`673`) | **`705`** (`0x2C1`) | Dedicated GPU SMMU-500 NS Global Fault (`#global-interrupts = 1`) |
| `kgsl-smmu` CB0 (`gfx3d_user`) | `0x2A5` (`677`) | **`709`** (`0x2C5`) | `SID 0` (`gfx3d_user` non-secure GPU VA / SQE / CP) |
| `kgsl-smmu` CB1 (`gfx3d_lpac`) | `0x2A6` (`678`) | **`710`** (`0x2C6`) | `SID 1` (`gfx3d_lpac`) |
| `kgsl-smmu` CB2 (`gfx3d_secure`) | `0x2A7` (`679`) | **`711`** (`0x2C7`) | `SID 2` (allocator bootstrap / secure CB) |
| `kgsl-smmu` GMU Stream | *(uses SID 5)* | — | `SID 5` (`qcom,gmu@3d69000` `iommus = <&kgsl_smmu 5 0>`) |
| **QTB (`gpu_qtb@3de8000`)** | — | — | `stream-id-range = <0x00 0x400>`, `iova-width = 49` (`0x31`), `ACTLR = 0x32B`, `dma-coherent` |

---

## 2. Hard-Evidence Comparison of the Two GPU Paths on S23 Windows 11 ARM64

### 2.1 Path 1: Open-Source SM8550 Native WDDM + Mesa Freedreno/Turnip (`oneplus-open-windows` Stack)

**Components in `repos/oneplus-open-windows/`:**
1. **UEFI Clock/Power Bootstrap (`firmware/G736r2/G290Pkg/G290GpuPower.c` + `G721`–`G723` + `firmware/G991/OpenOwnerPkg/`):**
   - Establishes `CX`/`MX`/`MXC` voltage corners, enables `GCC_GPU_*` and `GPUCC` `CX_GDSC`, programs `HUB` (200 MHz), `FF` (200 MHz), `AHB`/`CRC_AHB`/`HUB_AON`/`HUB_CX_INT`, locks `GPU_CC_PLL1` at `440.0 MHz`, and leaves `CX_GMU`/`CXO`/`GX_GMU` clean for the Windows clock broker FSM.
   - Publishes split ACPI tables (`Gpu0.asl` `OPEN0515` + `Mmu1.asl` `OPEN0D01`) and level-triggered `IORT` (`IortLevel.h`).
2. **Windows Kernel SMMU/Clock Broker & Native GPU Owner (`experiments/G778-cold-reset-startup/`, `experiments/G1287-cold-aperture/`, `experiments/G1311-gpu-nonsecure-va/`, `experiments/G1348/`):**
   - `OpenSmmuD0` (`OPEN0D01`): owns dedicated `KGSL-SMMU (0x03DA0000)` + `GPUCC`/`GCC` clock pages (`gmu_clock.c` 220 MHz GMU clock FSM).
   - `OpenGpuStart` (`OPEN0515`): loads GMU firmware (`g190_backend.c`), executes Gunyah/TZ **NORMAL64 SCM `0x42000C1B`** (`scm_set_cp0.S`, `x0=0x42000C1B, x1=4, x2=0xFFFF0000, x3=x4=x5=0xFFFFFFFF`) to open the non-secure CP0 aperture, runs GMU/HFI startup (`startup_acked = 0xF`), acquires GX via OOB (`g440_gpu_power.c`), initializes Adreno 740 (`g456_hwinit.c`), and constructs 39-bit Stage-1 page tables for `SID0/CB0`.
3. **WDDM KMD Bridge (`drivers/gpu/gpu-a740-kmd-G741/` -> `drivers/gpu/gpu-wddm-bridge-G779/` + `experiments/G1315-wddm-integration/`):**
   - Ported from TopazGpu (`A610` WDDM KMD) with `a740_hal.c`, `g190_backend.c`, `g433_cp.c`, `g440_gpu_power.c`, `g456_hwinit.c`, `g482_board.c`, and `msm.c` implementing Linux `drm/msm` ioctls via `DxgkDdiEscape` (`topazgpu_escape.h`).
4. **Userspace Mesa Freedreno / Turnip ICD (`userspace/mesa-G739/`):**
   - Overlay against Mesa commit `62f1e49a1eb3a408f748725326fe53447887f97d` (`fd_wddm.c` translates `libdrm` `drmIoctl`/`drmCommand*` into `TopazGpu` `DxgkDdiEscape` calls for Freedreno Gallium / D3D10 UMD).

**What Actually Works Today on OnePlus Open (`SM8550`) vs What Remains Unfinished (per `docs/AGENT_HANDOFF.md`, `G1315`, `G1348`):**
- **Demonstrated & Working:**
  - Full UEFI + Windows `GPUCC`/`GCC` clock bring-up (`PLL1=440 MHz`, `HUB=200 MHz`, `GMU=220 MHz`).
  - Live `SCM` query (`0xC2000601`, `x2=0x02000C1B` -> `x0=0, x1=1`) and live `NORMAL64` CP0 secure-aperture setter (`0x42000C1B` -> `x0=0, x1=0`).
  - Live Adreno 740 GMU firmware boot, PDC/RSC load, and HFI startup acknowledgements (`mask = 15`).
  - **G1348 (Boot177):** Hardware `KGSL-SMMU` `SID0` privileged instruction-translation page-walk matched the exact physical page of the pinned `a740_sqe.fw` MDL (`result=8 MATCH`, `status=1`, zero faults, Windows stayed responsive).
- **Still Unfinished (Honest Status):**
  - **First CP SQE Start Boundary (`CP_SQE_CNTL = 1`):** Writing `1` to `CP_SQE_CNTL (0x03D02020)` still triggers a Qualcomm `900E` crash before fence1 completion (`G1311`, `G1313`, `G1318`, `G1334`, `G1335`, `G1341`; whereas `G1343` zero-write `CP_SQE_CNTL = 0` returns cleanly). Resolving the exact SQE fetch/release security or retention precondition is the single active upstream blocker.
  - **WDDM Integration (`G1315`):** `G779` `TgStartDevice` still intentionally returns `STATUS_DEVICE_NOT_READY`; it is not yet wired to the `G1287`/`G1348` native CB0 owner, dynamic user BO residency, GPU IRQ/DPC fence completion, or authenticated ZAP (`a740_zap.mbn`) fence2 transition.

**What Differs on Samsung Galaxy S23 (`dm1q`, `SM8550-AC`) vs OnePlus Open:**
1. **Silicon Identity Matches!** Both OnePlus Open and S23 `dm1q` run `qcom,adreno-gpu-gen7-2-1` (`0x07020100`, `soc_id = 519`, `rev = 2`). Almost the entire `GPUCC`/`GMU`/`RBBM`/`KGSL-SMMU` register map is **bit-for-bit identical**.
2. **Higher Bin (`719 MHz` vs `680 MHz`):** S23 `SM8550-AC` adds a 10th OPP bin (`719 MHz` @ `0xE0`). Initial bring-up can safely run at `124.8 MHz` / `220 MHz` (matching `g440_gpu_power.c`'s pinned `124.8 MHz` boot vote) before enabling higher DCVS corners.
3. **No OEM `EFI_NPA_PROTOCOL` / `EFI_CLOCK_PROTOCOL` in `Mu-dm1q`:** OnePlus Open's `G290GpuPower.c` calls OEM XBL/ABL `EFI_NPA_PROTOCOL` (`79d6c870-...`) and `EFI_CLOCK_PROTOCOL` because it injects DXEs into the OEM FV. Our `Mu-dm1q` boots as a standalone Project Mu image on Samsung ABL without OEM Clock/NPA DXEs — so S23 MUST use **direct `APPS_RSC` DRV2 TCS0 (`0x17A20000`) + `cmd-db` (`0x81C60000`)** rail voting and **direct `GCC`/`GPUCC` MMIO** (which `s23pcieprobe` already proved works live on S23!).
4. **Gunyah EL2 Hypervisor Constraints on Samsung `dm1q`:**
   - Samsung `dm1q` boots Windows in EL1 underneath Qualcomm Gunyah EL2 + Samsung TZ (`QTEE`).
   - Direct EL1 writes to `apps-smmu@15000000` global/secure registers or `gpu_micro_code_region@9b09a000` are mediated by EL2/TZ; dedicated `kgsl-smmu@03da0000` CB0 non-secure context programming and `SCM 0x42000C1B` (non-secure CP0 aperture) must be validated live on Samsung's TZ build (`S911BXXS8DYI3`).
   - Disable D3Cold/S0Idle (`_S0W = 0`) and keep `GPUCC`/`KGSL-SMMU` clocks pinned across `ExitBootServices`.

---

### 2.2 Path 2: Official Qualcomm `qcdx` / `windows_silicon_qcom_kailua` WDDM Driver

**Hard Evidence Audit from `repos/windows_silicon_qcom_kailua/` and `repos/oneplus-open-windows/experiments/{G109,G142,G143}`:**

| Criterion | Verified Evidence on Disk | Impact on S23 `dm1q` |
|---|---|---|
| **1. Driver Availability in Kailua BSP** | Full recursive audit of `repos/windows_silicon_qcom_kailua/Drivers/` (19 drivers + 2 extensions): **NO `qcdx`, NO `qcpep`, NO `qcglink`, NO graphics/display driver is present.** Only base SOC plumbing (`qcabd`, `qcgpio`, `qci2c`, `qcspi`, `qcpil`, `qciommu`, `qcscm`, `qcpmic*`, `qcadc`, `qcsyscache`, `qcusbcucsi`, `qctftp`, `qcsocpartition`, `qccdi`, `qcrpen`, `qcsecapp`). | Cannot obtain a Qualcomm-signed SM8550 `qcdx` from the Kailua BSP repo. |
| **2. `qcdx` Gen7 Family Selector Hard-Reject (`G109` / `G142` / `G143`)** | Unicorn ARM64 emulation of Microsoft/Qualcomm's shipping Windows Gen7 `qcdx` (`sha256=cb127c49...`) in `G142` & `G143`: `qcdx` queries `ACPI`/`DSDT` for the `SOSI` QWord pointer, maps SMEM Item 137 (`SoCInfo`), reads `family` at offset `+108` (`127 = 0x7F`, `soc_id = 519`, `rev = 0x20000` on SM8550), and passes it to the Gen7 profile selector (`0x14049C288`). **The selector ONLY accepts `family = 0x88` (Snapdragon X Elite / Hamoa, profile `0x50C000`) and `family = 0x9A` (Snapdragon X Plus / Purwa, profile `0x30C000`), and returns `0` (failure, log tag `0x1437`) for `family = 127 (0x7F)`!** | Even if you supply a valid `SOSI` pointer in DSDT, shipping Windows ARM64 Gen7 `qcdx` **explicitly rejects SM8550** and embeds only Hamoa/Purwa microcode (`0x4E3C`/`0x4DEC` words), NOT Adreno 740 (`gen7_2_1`) microcode. |
| **3. Mandatory `QCOM0C36` ACPI & 10-Driver `_DEP` Chain** | `Kailua/Decompiled/DSDT_MTP.dsl` lines `9870..9990`: `GPU0` (`QCOM0C36`) bundles **MDSS Display (`0x0AE00000`) + DP PHY (`0x00FD0000`) + Adreno GPU (`0x03D00000`) + GMU PDC (`0x0B280000`) + Video Iris3 (`0x0AA00000`)** into a single monolithic device with `_DEP = { MMU0, MMU1, IMM0, IMM1, PEP0, PMIC, PILC, RPEN, TREE, SCM0 }` and custom `RESI`/`_DSM` methods. | Requires full Qualcomm `qcpep` (not public for Kailua!) + Qualcomm `qcglink` + Qualcomm `PMIC` + `SCM0`/`TREE` + Qualcomm panel XML/DSI takeover (which would kill SimpleFB and conflict with Samsung `DM1_S6E3FAC_AMB606AW01`!). |
| **4. TrustZone / Gunyah / Signing Barriers** | Qualcomm `qcdx` expects Qualcomm Windows-signed `a740_zap` / `qcpep` power-rail handshakes and `QTEE` Windows TA (`TREE` / `QCOM04DE`) that do not exist in Samsung's Android `S911BXXS8DYI3` TZ image. Spoofing `family = 0x88` fails later because Hamoa/Purwa register maps, GMEM size, clock tables, and embedded microcode do not match SM8550 silicon (`G142` negative fixture). | **Hard Blocked.** |

**Verdict:**
- **Path 2 (`qcdx`) is definitively non-viable on Samsung Galaxy S23 (`dm1q`)** — blocked simultaneously by missing public Kailua `qcdx`/`qcpep` binaries, hard-coded Hamoa/Purwa-only (`0x88`/`0x9A`) family checks in shipping Windows Gen7 `qcdx`, monolithic MDSS+GPU+Video `QCOM0C36` dependencies, and Samsung Android TZ incompatibilities.
- **Path 1 (Open-Source Direct MMIO + RPMh + `G1287`/`G1348`/`G779` Native A740 KMD/WDDM + Mesa Freedreno/Turnip)** is the **ONLY viable path** for native Adreno 740 acceleration on S23 `dm1q`, and S23 shares the exact `gen7-2-1` (`0x07020100`) GPU IP with OnePlus Open.

---

## 3. Adreno 740 Firmware Inventory On Disk (Read-Only Audit)

An exhaustive read-only scan of `s23_dm1q\`, `s23_dm1q\backups\`, `s23_dm1q\repos\`, `..\device_firmware\`, `..\modem_fix\`, and WSL `/home/mahan/` established the following exact status:

| Location Audited | What Was Found | Applicable to S23 `dm1q` (`SM8550-AC`)? |
|---|---|---|
| `..\device_firmware\firmware\` | `a612_rgmu.bin`, `a618_gmu.bin`, `a630_gmu.bin`, `a630_sqe.fw`, `a640_gmu.bin` | **NO** — Confirmed via `..\modem_fix\MODEM_FORENSIC_REPORT.md` to be extracted from **Samsung Galaxy A52 4G (`SM-A525F` / `a52q` / `SM7125`, Adreno 618)**. Do NOT use on S23! |
| `s23_dm1q\wifi_research\s23_vendor_wifi_fw\` | `qca6490/{amss20.bin,bdwlan.elf,m3.bin,regdb.bin}`, `hpbtfw21.tlv`, `hpnv21.bin`, `hpnv21g.bin`, `Data.msc` | Wi-Fi & Bluetooth firmware only (no GPU firmware). |
| `s23_dm1q\backups\` | `abl.img`, `boot.img`, `dtbo.img`, `efs.img`, `fsc.img`, `fsg.img`, `init_boot.img`, `live_fdt.dtb`, `misc.img`, `modemst1.img`, `modemst2.img`, `param.img`, `persist.img`, `sec_efs.img`, `uefi.img`, `vbmeta.img`, `vendor_boot.img`, `xbl.img`, `xbl_config.img` | Boot/NV backups only (`vendor` / `vendor_firmware_mnt` / `modem` partition images were not backed up to PC). |
| `repos/oneplus-open-windows/` | Source-only snapshot (proprietary firmware excluded by `README.md` publication policy). | Source reference only. |
| **Required S23 Adreno 740 Firmware Files (NOT yet extracted to PC)** | 1. **`a740_sqe.fw`** (CP SQE microcode)<br>2. **`gmu_gen70200.bin`** (GMU CM3 firmware — Linux upstream alias `a740_gmu.bin`, GMU v4.1.0)<br>3. **`a740_zap.mbn`** (or split `a740_zap.mdt` + `a740_zap.b00..b02`, signed by Samsung/QCOM for S23's TZ!) | **Action Required Once Live Device Access Resumes:** Pull from S23 Android/Recovery `/vendor/firmware/a740_sqe.fw`, `/vendor/firmware/gmu_gen70200.bin` (or `a740_gmu.bin`), and `/vendor/firmware_mnt/image/a740_zap.*` into `gpu_research\s23_gpu_fw\`. |

---

## 4. Recommended S23 Adreno 740 Bring-Up Sequence

1. **Phase 1 — Live Telemetry & Clock/GX Proof (`drivers\s23gpuprobe\pkg\s23gpuprobe.sys`):**
   - Run `StageMask = 0x01` (safe read-only default) to dump S23's boot-time `GCC_GPU_*`, `GPUCC`, `cmd-db` (`cx.lvl`/`mx.lvl`/`mxc.lvl`/`gfx.lvl`), and `ChipInfo`.
   - Run `StageMask = 0x03` (`0x01 | 0x02`) to vote RPMh ARC rails via `APPS_RSC` DRV2 TCS0, enable `CX_GDSC` + `GPUCC` clocks + `PLL0`/`PLL1` + `GX_GDSC`, and read live Adreno 740 `GMU_AO`, `GMU_CX`, `RBBM_STATUS`, and `GBIF` registers from Windows.
   - Run `StageMask = 0x07` (`0x01 | 0x02 | 0x04`) to verify non-destructive scratch register R/W (`GMU_GENERAL_0` in CX, `RBBM_SP_HYST_CNT` in GX) and GX power-collapse/recovery retention.
2. **Phase 2 — Firmware Extraction & UEFI Pre-EBS Integration (`gpu_research\s23_gpu_bringup\`):**
   - Pull S23's stock `a740_sqe.fw`, `gmu_gen70200.bin`, and `a740_zap.*` from `/vendor/firmware*`.
   - Integrate `s23_uefi_gpu_dxe.c` + `s23_gpu_acpi.asl` (`MMU1` `OPEN0D01` + `GPU0` `OPEN0515`) + level-triggered `IORT` entry for `0x03DA0000` into a separate test UEFI build (`Mu-dm1q-gpu.img`).
3. **Phase 3 — SCM Aperture (`0x42000C1B`), GMU/HFI Boot, & `KGSL-SMMU` `SID0` Walk:**
   - Test `SCM` `0xC2000601` / `0x42000C1B` under Samsung `S911BXXS8DYI3` TZ/Gunyah, port `OpenSmmuD0` + `OpenGpuStart` (`G1287`/`G1348`), and validate GMU/HFI ack mask 15 + `SID0` page walk.
4. **Phase 4 — First CP Fence & WDDM/Mesa (`G779` + `G739`):**
   - Track/adopt the upstream OnePlus Open fix for the `CP_SQE_CNTL=1` boundary, authenticate S23's `a740_zap.mbn` via PAS, connect the native CB0 owner to `G779` WDDM KMD (render-only adapter preserving SimpleFB), and deploy ARM64 Mesa `G739` (`fd_wddm.c` / Freedreno/Turnip).

---

## 5. Live Stage 5A Results, ACPI Validation & Stage 5B Runbook

### 5.1 Live Stage 5A Execution (`s23gpuprobe.sys v1.0.0.5`)

Tested live on the S23 (`StageMask = 0x1B` = `0x01 | 0x02 | 0x08 | 0x10`, captured in `drivers/s23gpuprobe/s23gpuprobe_v5_s5a_live.log`, `StageMask` restored to `0x01`):

- **RPMh & Clocks (`Stage 0x02`):**
  - `cx.lvl`, `mx.lvl`, `mxc.lvl` voted to `idx=5` (`vlvl=0x100`) and `gfx.lvl` voted to `idx=1` (`vlvl=0x034`) in `1 us` (`idle=1`).
  - `GPUCC_CX_GDSCR = 0xF822F800` (ON), `GPUCC_PLL0_MODE = 0xE0000005` (locked), `GPUCC_PLL1_MODE = 0xE0000005` (locked), `GPUCC_CX_GMU_CBCR = 0x00002001` (ON), `GPUCC_HLOS1_SMMU = 0x00000001` (ON).
  - `GPUCC_GX_GDSCR` remained `0x6822F800` (`PWR_ON=0`, `0x03D00000` MMIO reads safely blocked) as expected prior to GMU CM3 boot.
- **GMU / RSCC / PDC Read-Only State (`Stage 0x08`):**
  - `ITCM (0x03D6D000)` and `DTCM (0x03D71000)` 4096-word windows readable without fault (`nz=4096 ff=0`).
  - `RSCC (0x03D50000)`: `RSCC_TCS0..3_DRV0_STATUS = 0x00000001`; `PDC_GPU_ENABLE_PDC (0x0B284500) = 0x00000101`.
- **ARM64 SCM (`smc #0`) Availability on Samsung `S911BXXS8DYI3` TZ (`Stage 0x10`):**
  - `SCM IS_CALL_AVAIL` (`0xC2000601`) returned `r0=0, avail=1` for:
    - `SET_CP_POOL_SIZE` (`0x02000C1B`) -> **Available (`avail=1`)**
    - `PAS_INIT_IMAGE` (`0x02000201`) -> `avail=1`
    - `PAS_MEM_SETUP` (`0x02000202`) -> `avail=1`
    - `PAS_AUTH_AND_RST` (`0x02000205`) -> `avail=1`
    - `PAS_SHUTDOWN` (`0x02000206`) -> `avail=1`
    - `PAS_IS_SUPPORTED` (`0x02000207`) -> `avail=1`
  - Direct `SCM PAS_IS_SUPPORTED(PAS_ID=13)` (`0x42000207`) completed cleanly returning `r0=0, r1=0, r2=0`.
- **AXI-Gated `KGSL-SMMU` (`0x03DA0000`, `QSMMU-v500`, `Stage 0x10`):**
  - With `GPUCC_HLOS1_SMMU = 0x00000001` verified live first, read-only registers completed with zero faults:
    - `SMMU_sCR0 (0x03DA0000) = 0x002D0406`
    - `SMMU_sACR (0x03DA0010) = 0x0000001C`
    - `SMMU_sIDR0 (0x03DA0020) = 0x4C017E18`
    - `SMMU_sIDR1 (0x03DA0024) = 0x40000016`
    - `SMMU_sIDR2 (0x03DA0028) = 0x00005511`
    - `SMMU_sIDR7 (0x03DA003C) = 0x00000024`
    - `SMMU_sGFSR (0x03DA0048) = 0x00000000`

### 5.2 ACPI SSDT Validation (`gpu_research/s23_gpu_bringup/s23_gpu_acpi.asl`)

- Split broker/owner contract adapted to S23 `dm1q` hardware resources:
  - `\_SB.MMU1` (`_HID "OPEN0D01"`): `0x03DA0000` (`256 KiB` SMMU-500) + `GPUCC`/`GCC` clock pages (`0x03D97000`, `0x03D99000`, `0x00171000`, RO `0x00100000`/`0x03D91000`) + Level ActiveHigh GSIs `705, 711, 709`.
  - `\_SB.GPU0` (`_HID "OPEN0515"`): `_DEP { \_SB.MMU1 }`, 5 MMIO banks (`0x03D00000`, `0x03D50000`, `0x03D68000`, `0x03D9A000`, `0x0B280000`) + Level ActiveHigh GSIs `332, 336, 337`.
- Validated via `iasl -tc`: **0 Errors, 0 Warnings, 0 Remarks, 1 Optimizations** (`744 bytes` AML).

### 5.3 Stage 5B & WDDM Bring-Up Runbook

1. **Pull Stock S23 GPU Firmware (Lineage Recovery only, strictly read-only):**
   Boot the phone into **Lineage Recovery** (`Power + VolUp` over USB) and run:
   ```powershell
   python gpu_research/s23_gpu_bringup/fw/pull_s23_gpu_fw.py
   ```
   This maps `/dev/block/mapper/vendor{_a,_b}` via `lptools_new` read-only (`-o ro`), extracts `gmu_gen70200.bin` (and/or `a740_gmu.bin`), `a740_sqe.fw`, and `a740_zap.*` into `gpu_research/s23_gpu_bringup/fw/s23_vendor_gpu_fw/`, and runs `extract_and_verify_s23_gpu_fw.py`.
2. **Stage `gmu_gen70200.bin` on Windows & Test Stage 5B (`StageMask = 0x3B`):**
   Upload `gmu_gen70200.bin` to `C:\Users\Public\gmu_gen70200.bin`, set `StageMask = 0x3B` (`0x01 | 0x02 | 0x08 | 0x10 | 0x20`) on `HKLM\SYSTEM\CurrentControlSet\Services\s23gpuprobe\Parameters`, run `sc.exe start s23gpuprobe`, inspect `C:\Users\Public\s23gpuprobe.log`, and restore `StageMask = 0x01`.
3. **ACPI + Native WDDM (`G1287`/`G1348`/`G779`) + Mesa (`G739`):**
   Once Stage 5B confirms GMU CM3 boot + `GPUCC_GX_GDSCR` (`PWR_ON` bit 31) + `SQE` / `SET_CP_POOL_SIZE` / `a740_zap` bring-up, integrate `s23_gpu_acpi.aml` into a dedicated UEFI build and connect the open-source WDDM + ARM64 Mesa Freedreno/Turnip stack.

