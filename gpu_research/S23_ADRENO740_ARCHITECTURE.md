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
| **Pulled S23 Adreno 740 Firmware Files (Gitignored local folder `gpu_research\s23_gpu_bringup\fw\s23_vendor_gpu_fw\`)** | 1. **`a740_sqe.fw`** (`76,724 B`, SHA-256 `96fee336424b139100fc60b5b45a907360e4b3936d7e1d00406b9bd80ca48473`)<br>2. **`gmu_gen70200.bin`** (`66,844 B`, `15` blocks, `DTCM_VER=0x40010009`, SHA-256 `1a2a419c39046d3141fc5fed5aa7f971de2db40cc7a1d89693c3e26fad64dd98`)<br>3. **`a740_zap.mbn`** (`12,088 B`, `362882ec...`), **`a740_zap.elf`** (`5,168 B`), **`a740_zap.mdt` + `.b00..b02`** | **PULLED & VERIFIED VIA LINEAGE RECOVERY (`pull_s23_gpu_fw.py`):** Both `gmu_gen70200.bin` and `a740_sqe.fw` match OnePlus Open `G191_STOCK_GMU_SHA256` and `G433_SQE_SHA256` **100% bit-for-bit**. |

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

### 5.3 Live Stage 5B Execution & Verified S23 Stock Firmware (`s23gpuprobe.sys v1.0.0.5`)

1. **Extracted & Verified Stock S23 GPU Firmware (Lineage Recovery, read-only via `pull_s23_gpu_fw.py`):**
   - Parsed `0PLA` `LpMetadataHeader` from `/dev/block/by-name/super` (`/dev/block/sda30`, `vendor` offset `8559525888`, mounted read-only via `losetup -r` + `erofs`) and extracted all `8` stock files into `gpu_research/s23_gpu_bringup/fw/s23_vendor_gpu_fw/`.
   - **100% Bit-Exact SHA-256 Matches to `G1287-cold-aperture` Reference Pins (`extract_and_verify_s23_gpu_fw.py`):**
     - `gmu_gen70200.bin`: `66,844 B`, `15` blocks (`ITCM 476 B`, `ICACHE 66,064 B`, `DTCM 40 B + 24 B`), `GMU DTCM Version Word (0x10007FE0) = 0x40010009`, SHA-256 `1a2a419c39046d3141fc5fed5aa7f971de2db40cc7a1d89693c3e26fad64dd98` (**EXACT MATCH** to `G191_STOCK_GMU_SHA256`).
     - `a740_sqe.fw`: `76,724 B`, SHA-256 `96fee336424b139100fc60b5b45a907360e4b3936d7e1d00406b9bd80ca48473` (**EXACT MATCH** to `G433_SQE_SHA256`).
     - `s23_rsc_seq_ucode.bin`: `20 B`, SHA-256 `20175df4f9aafab7f958205d71bf931a30a484bd6e94db348da5497a4e13ce90` (**EXACT MATCH** to `G482RscSha`).
     - `a740_zap.mbn` (`12,088 B`, ELF magic, `362882eca2c5112b5aa1b8b56c2606947b7b73314070c9c514cf108bbffadfa8`), `a740_zap.elf` (`5,168 B`, `3cbdf8d0ca6744c368170e8d248c409c3b93963e56fa1ef0d3951eae0ef5148b`), `a740_zap.mdt` (`4,044 B`) + `a740_zap.b00..b02`.
2. **Live Stage 5B ITCM/DTCM Verification (`s23gpuprobe.sys v1.0.0.5`, `StageMask = 0x3B` -> `0x01`, `drivers/s23gpuprobe/s23gpuprobe_v5_s5b_live.log`):**
   - Staged `gmu_gen70200.bin`, `a740_sqe.fw`, and `a740_zap.mbn` to `C:\Users\Public\` on the phone and executed `s23gpuprobe.sys v1.0.0.5` with `StageMask = 0x3B`.
   - Stage `0x20` (`Stage 5B`) loaded `C:\Users\Public\gmu_gen70200.bin` (`66,844 B`), parsed all `15` blocks (`ITCM=1`, `ICACHE=1`, `DTCM=2`), wrote `ITCM` (`476 B`) and `DTCM` (`40 B + 24 B`) to live GMU TCM hardware and read back every 32-bit word with **0 mismatches**, and safely skipped CM3 reset release prior to `KGSL-SMMU` CB2 page table setup.

### 5.4 Live Stage 5B Full `KGSL-SMMU` CB2 + GMU CM3 Boot + HFI Startup + `DCVS_VOTE` + `OOB_REQUEST` GX Wake (`s23gpuprobe.sys v1.0.0.6`)

Tested live on the Galaxy S23 (`StageMask = 0x3B` -> restored to `0x01`, captured in `drivers/s23gpuprobe/s23gpuprobe_v6_s5b_live.log`):

1. **Cold `KGSL-SMMU` (`0x03DA0000`) Stream Map & 89-Page Stage-1 Page Table (`CB2`, `SID = 5`, `ASID = 2`):**
   - Verified cold SMR/S2CR slots `0..23`: `SMMU_ROUTES: valid_smr=0 cold_fault_default=1`.
   - Allocated an 89-page (`364,544 B`) `MmNonCached` contiguous physical pool at `PA = 0x0009FFD69000` (`< 64 GB` 36-bit OAS, `BoundaryAddressMultiple = 0`):
     - **Pages `0..5`:** 3-level AArch64 4K Stage-1 page tables (`L1` @ `+0x0000`, `L2_low` @ `+0x1000`, `L2_high` @ `+0x2000`, `L3_low0` @ `+0x3000`, `L3_low1` @ `+0x4000`, `L3_600` @ `+0x5000`).
     - **Pages `6..88`:** 83 GMU data pages mapped with `leaf_attrs = 0xE03` (`nG=1, AF=1, SH=2 Outer Shareable, AttrIdx=0 Non-Cacheable, RWX Priv`):
       - `0x00004000` (`17` pages, `68 KB` `ICACHE` Region 2 backing)
       - `0x00100000` (`9` pages), `0x00200000` (`19` pages), `0x00300000` (`28` pages)
       - `0x60000000` (`1` page), `0x60001000` (`1` page)
       - `0x60002000` (`4` pages, `16 KB` `GMU_LOG` ring)
       - `0x60006000` (`4` pages, `16 KB` `HFI` queue table + `CMDQ`/`MSGQ`/`DBGQ`)
   - Armed `KGSL-SMMU` Context Bank 2 (`0x03DC2000`):
     - `SMMU_sCR0(armed) (0x03DA0000) = 0x00ED0406`
     - `CB2_SCTLR(armed) (0x03DC2000) = 0x00C00067`
     - `CB2_TTBR0 = 0x00020009FFD69000` (`ASID = 2`)
     - `SMR[0](armed) (0x03DA0800) = 0x80000005` (`VALID | SID=5`)
     - `S2CR[0](armed) (0x03DA0C00) = 0x00000302` (`TRANS | PRIVCFG=3 | CBNDX=2`)
     - `CB2_FSR(armed) (0x03DC2058) = 0x00000400`

2. **Critical GMU Ownership Requirement for `GPUCC_PLL0 (0x03D90000)` & `GPUCC_GX_GDSCR (0x03D9905C)`:**
   - The host CPU must **only** lock `gpu_cc_pll1` (`0x03D91000`, `440 MHz` -> `220 MHz` GMU CX clock) and must leave `gpu_cc_pll0` (`0x03D90000`) pre-configured in **standby** (`MODE = 0x00000100 / 0`, `OPMODE = 0`, `USER_CTL = 0`, `L = 0x4444001E`, `ALPHA = 0xBAAA`) and `GPUCC_GX_GDSCR (0x03D9905C)` with **`SW_COLLAPSE = 1` (`0x0022F801`)**.
   - If the host CPU force-locks `GPUCC_PLL0` or clears `SW_COLLAPSE` on `GPUCC_GX_GDSCR` prior to GMU boot, `HFI[DCVS_VOTE]` (`id=30`) times out because GMU CM3 owns `GPUCC_PLL0` and the GX headswitch.

3. **Live GMU CM3 Boot, PDC/RSCC Sequencer Load & HFI Startup + `DCVS_VOTE`:**
   - Loaded all `15` blocks of `gmu_gen70200.bin` (`ITCM=1`, `ICACHE=1`, `DTCM=2`, `GMU_DTCM_VERSION = 0x40010009`) and released `GMU_CM3_SYSRESET (0x03D7E000) = 0`:
     - **`GMU_CM3_FW_INIT_RES(post) (0x03D7E070) = 0x00000100`** (`GMU_CM3_FW_BUSY = 0`, `SMMU_sGFSR = 0`, `CB2_FSR = 0x00000400` — zero translation faults).
   - Loaded `GMU_PDC` (`0x0B284520 = 0`, `0x0B284500 = 0x80000001`), `RSCC` (`0x03D50000`) PDC handshake registers, and S23's 20-byte `GEN7_2_1` RSCC sequencer ucode at `0x03D50550..0x03D50560`.
   - Started HFI (`GMU_HFI_INIT = 1` -> **`GMU_HFI_CTRL_STATUS(post) (0x03D7E600) = 0x00000001`**) and completed all 5 HFI messages with `err = 0x00000000`:
     - **`HFI[PERF_TBL id=4 seq=1] reply hdr=0x00010304 err=0x00000000`**
     - **`HFI[BW_TBL id=3 seq=2] reply hdr=0x00110303 err=0x00000000`**
     - **`HFI[CORE_START id=14 seq=3] reply hdr=0x0021030E err=0x00000000`**
     - **`HFI[START id=10 seq=4] reply hdr=0x0031030A err=0x00000000`**
     - **`HFI[DCVS_VOTE id=30 seq=5] reply hdr=0x0041031E err=0x00000000`**
   - Post-`DCVS_VOTE` hardware verification:
     - `HFI CMDQ r=232 w=232 | MSGQ r=20 w=20 | DBGQ r=0 w=0`
     - GMU CM3 autonomously locked `GPUCC_PLL0` for the 124.8 MHz vote (`GPUCC_PLL0_MODE = 0xE0000005`, `OPMODE = 1`, `GPUCC_PLL0_L_VAL = 0x0044000D`), completed all 4 RSCC TCS votes (`RSCC_TCS0..3_DRV0_STATUS = 0x00000001`, `RSCC_SEQ_BUSY_DRV0 = 0x00000000`), and wrote `225` nonzero dwords (`GMU_GENERAL_9 = 0x0000003B`) into `GMU_LOG (0x60002000)`.

4. **Live `OOB_REQUEST` (`oob_gpu`) -> GX Headswitch Power-On & Live Adreno 740 `KGSL` (`0x03D00000`) Telemetry:**
   - Asserted `OOB_REQUEST (BIT 30)` on `GMU_H2G_SET (0x03D7E650)`:
     - **`GMU_G2H_INTR_INFO(oob) (0x03D7E648) = 0x80000001`** (`OOB_ACK` bit 31 set, then W1C cleared)
     - **`GMU_GFX_PWR_CLK_ST(oob) (0x03D7E340) = 0x00000000`** (transitioned from `0x00000003` -> `0x00000000`: GX GDSC power & GX clocks ON!)
     - **`GPUCC_GX_GDSCR(5B) (0x03D9905C) = 0xF8222800`** (bit 31 `PWR_ON = 1`!)
     - **`GPUCC_GX_CFG_GDSCR(5B) (0x03D99060) = 0x04070000`** (`POWER_UP_COMPLETE` bit 16 = `1`, `POWER_DOWN_COMPLETE` bit 15 = `0`)
   - With `GpuGxAxiGateOpen()` and `(GMU_GFX_PWR_CLK_STATUS & 3) == 0` satisfied, live `0x03D00000` (`KGSL`) registers read cleanly without fault:
     - `KGSL_RBBM_STATUS(5B) (0x03D00840) = 0x00000001`
     - `KGSL_RBBM_STATUS3(5B) (0x03D0084C) = 0x00000000`
     - `KGSL_GBIF_HALT(5B) (0x03D0F114) = 0x00000008`
     - `KGSL_GBIF_HALT_ACK(5B) (0x03D0F118) = 0x00000003`
     - `KGSL_CP_SQE_CNTL(5B) (0x03D02020) = 0x00000000`
   - Clean teardown verified: released `OOB_ACK` (`BIT 31` on `GMU_H2G_SET`), asserted `GMU_CM3_SYSRESET(stopped) = 1`, fault-pinned and disabled `KGSL-SMMU` CB2 (`tlb_done=1 CM3_RST=1 CB2_SCTLR=00000000 TLBSTATUS=00000000`), and freed the 89-page pool.

5. **Persistent WDDM 1.3 Render-Only KMD (`drivers/s23gpu/` — Done) & Next Step — ARM64 Mesa (`Turnip` / `Freedreno`)**:
   - Stage 6A/6B/6C and the persistent ARM64 WDDM 1.3 Render-Only KMD (`drivers/s23gpu/s23gpu.sys`, `s23gpu_umd.dll`, `s23gpu.inf`) are now verified live (see Sections 5.5 and 5.6 below). Next step is building and deploying ARM64 Mesa (`Turnip` Vulkan + `Freedreno` / `d3d10umd` / `Zink`) against the `topazgpu_escape.h` / Linux `msm` DRM ABI exposed by `s23gpu.sys`.

### 5.5 Live Stage 6A/6B/6C Full Adreno 740 Execution: `KGSL-SMMU` CB0 + `QTB` ECATS + `a740_sqe.fw` + PM4 Fences + TZ `a740_zap.mbn` (`s23gpuprobe.sys v1.0.0.7`)

Tested incrementally live on the Galaxy S23 (`RFCW90N0N7X`, `StageMask = 0x7B` -> `0xFB` -> `0x1FB`, unconditionally restored to `0x01`):

1. **Stage 6A (`StageMask = 0x7B`, `drivers/s23gpuprobe/s23gpuprobe_v7_s6a_live.log`) — CB0 + `gpu_qtb` ECATS + A740 `gpuHwInit()`:**
   - Moved GMU CB2 from `SMR[0]` to `SMR[2]` (`0x80000005` / `S2CR[2] = 0x00000302`) so slots `0` & `1` are dedicated to the GPU (`SID=0` `gfx3d_user`, `SID=1` `gfx3d_lpac`).
   - Allocated 34-page (`139,264 B`, `< 64 GB` 36-bit OAS) `MmNonCached` CB0 pool @ `PA = 0x0009FFC30000`, copied `a740_sqe.fw` (`76,724 B`, minus 4-byte header = `76,720 B` payload) to IOVA `0x00100000` (`R+X`, NC), Ring 0 to `0x00200000` (`32 KiB`), Memptrs/Fence to `0x00300000`, `pwrup_reglist` (`16` preemption registers) to `0x00301000`, and Scratch to `0x00302000`.
   - Armed `KGSL-SMMU` CB0 (`ASID = 1`, `CB0_TTBR0 = 0x00010009FFC30000`):
     - `CB0_SCTLR(armed) = 0x00000167`, `CB0_ACTLR = 0x00000301` (`CMTLB | PREFETCH_DEEP`, `CPRE=0`), `CB0_TCR = 0x80803519`, `CB0_TCR2 = 0x00038071`, `CB0_FSR = 0x00000400`, `SMR[0] = 0x80000000`, `SMR[1] = 0x80000001`, `S2CR[0..1] = 0`, `SMMU_sGFSR = 0`.
   - **`gpu_qtb@0x03DE8000` Hardware ECATS Walk (`SID=0`, `IOVA = 0x00100000`):**
     - **`QTB ECATS: MATCH status=0x0000000000000001 out=0x00009FFC34000000 extracted=0x0009FFC34000 expected=0x0009FFC34000 fsr=0x00000000`**
   - Cleared `GBIF_HALT (0x03D0F114) = 0` & `RBBM_GBIF_HALT (0x03D00058) = 0` (note: `GBIF_HALT_ACK (0x03D0F118) = 0x00000003` when idle with no traffic in flight, matching Linux `a6xx_gpu.c` / S23 `adreno_gen7_gmu.c`).
   - Completed full A740 `gpuHwInit()` (`AO_HWCG`, 54 `HWCG` entries, `RBBM_CLOCK_CNTL = 0x8AA8AA82`, 5 `GBIF` QoS/QSB regs, `UCHE` GMEM `0x01000000..0x012FFFFF`, 45 `CP_PROTECT` entries, `APRIV`, UBWC 4.0 `HBB=16` on `RB`/`TPL1`/`SP`/`GRAS` via BR+BV apertures, `RBBM_INT_0_MASK = 0x33D283C2`, Ring 0 + SQE bases):
     - `GMU_AO_AHB_FENCE_ST(6A) = 0`, `RBBM_STATUS(6A) = 0x1`, `CP_SQE_INSTR_BASE_LO = 0x00100000`, `CP_RB_RPTR = 0`, `CP_SQE_CNTL = 0`, `CB0_FSR = 0x400`, `SMMU_sGFSR = 0`.

2. **Stage 6B (`StageMask = 0xFB`, `drivers/s23gpuprobe/s23gpuprobe_v7_s6b_live.log`) — Live SQE (`CP_SQE_CNTL=1`) + `CP_ME_INIT` + `CP_MEM_WRITE`:**
   - Wrote `CP_SQE_CNTL (0x03D02020) = 1`: returned cleanly (`AHB = 0`, `RBBM_STATUS = 0x00C00015`, `CB0_FSR = 0x400`).
   - Submitted `CP_THREAD_CONTROL + CP_ME_INIT` + `CACHE_CLEAN` (`WPTR = 17`):
     - **`Fence1: val=1 polls=1 rptr_shadow=0x00000011 RPTR=0x00000011 WPTR=0x00000011`**
     - `RBBM_STATUS(f1) = 0x1` (idle), `RBBM_INT_0_STATUS(f1) = 0x00100000`, `CP_HW_FAULT = 0`, `CB0_FSR = 0x400`, `SMMU_sGFSR = 0`.
   - Submitted `CP_MEM_WRITE(0x05E9A740)` -> Scratch IOVA `0x00302000` + `CACHE_CLEAN` (`WPTR = 28`):
     - **`Fence2: val=2 polls=2 scratch=0x05E9A740 RPTR=0x0000001C RBBM_STATUS=0x00000001 CB0_FSR=0x00000400`**

3. **Stage 6C (`StageMask = 0x1FB`, `drivers/s23gpuprobe/s23gpuprobe_v7_s6c_live.log`) — TZ Aperture + `PAS_ID=13` ZAP + Non-Secure Mode:**
   - **`SCM CP_SMMU_APERTURE_ID(CB0)` (`0x42000C1B`): `r0=0, r1=0`**
   - Loaded `C:\Users\Public\a740_zap.mbn` (`12,088 B`) ELF32 segments into `gpu_micro_code_region@0x9B09A000` (`0x2000 B`):
     - **`PAS_INIT_IMAGE(13)` (`0x42000201`): `r0=0, r1=0`**
     - **`PAS_MEM_SETUP(13)` (`0x42000202`): `r0=0, r1=0`**
     - **`PAS_AUTH_RESET(13)` (`0x42000205`): `r0=0, r1=0`**
   - Submitted `CP_SET_SECURE_MODE(0)` + `CACHE_CLEAN` (`WPTR = 37`):
     - **`Fence3(non-sec): val=3 polls=3`**
     - **`SECVID_TRUST_CNTL(6C) (0x03D3D000) = 0x00000000`** (switched from `1` -> `0`: full non-secure GPU execution active!), `RBBM_STATUS(6C) = 0x1`, `CB0_FSR = 0x400`.
   - Clean teardown verified (`CP_SQE_CNTL=3` -> `GBIF_HALT` drain -> `RBBM_SW_RESET_CMD` -> `CP_SQE_CNTL=0` -> `CB0` & `CB2` SMMU teardown `tlb_done=1` -> `GMU_CM3_SYSRESET=1`).

### 5.6 Persistent WDDM 1.3 Render-Only KMD (`drivers/s23gpu/s23gpu.sys` — Verified Live)

- **Driver Package (`drivers/s23gpu/`)**:
  - Built natively on host PC with MSVC `14.44.35207` (`Hostx64\arm64`, `/W3 /WX`) + WDK `10.0.26100.6584`, verified against phone `ntoskrnl.exe` (`45/45`) & `hal.dll` (`3/3`) via `verify_pe.py`, cataloged (`inf2cat`), and Authenticode-signed with `CN=S23TouchTest2020`.
  - Contains `s23gpu.sys` (WDDM 1.3 Render-Only KMD, `NumSources=0, NumChildren=0, SupportNonVGA=TRUE`), `s23gpu_umd.dll` (minimal ARM64 UMD stub exporting `OpenAdapter10` / `OpenAdapter10_2`), `s23gpu.inf` (`ROOT\S23GPU`, `FeatureScore=F8`, `InstalledDisplayDrivers` + `UserModeDriverName` `REG_MULTI_SZ`), `s23gpu_install.exe`, and `s23gpu_test.exe`.
  - **24H2 WDDM Bring-Up Finding**: `dxgkrnl.sys` rejects a Display-class adapter post-start (`Event ID 549 StartAdapter_AddAdapterFailed`, `STATUS_OBJECT_NAME_NOT_FOUND 0xC0000034`) unless `HKR,,UserModeDriverName` (`REG_MULTI_SZ`) + `HKR,,InstalledDisplayDrivers` point to a valid ARM64 UMD DLL. Adding `s23gpu_umd.dll` resolved the start failure immediately while keeping `ROOT\BasicDisplay\0000` UEFI SimpleFB scanout active (`Started`).
- **Escape ABI & MMU / Ring Bring-Up**:
  - Exposes `topazgpu_escape.h` / Linux `msm` DRM ABI (`VERSION 1.9.0 "msm"`, `CHIP_ID=0x43050a01`, `GPU_ID=740`, `GMEM=0x01000000+0x00300000`, `HBB=16`, `VA=0x02000000+0xEE000000`, `<64 GB` scatter-gather MDLs, 3-level 4 KiB SMMU CB0 page tables, A7xx Ring0 BR/BV submit with separated `memptrs` slots, `GMU_ALWAYS_ON_CNT` timestamps, and `PAS_SHUTDOWN(13)` teardown).
- **Live Verification (`drivers/s23gpu/s23gpu_v1_live.log`)**:
  - Device installed as `ROOT\DISPLAY\0000` (`oem21.inf`, `Status: Started`) alongside `ROOT\BasicDisplay\0000` (`Status: Started`); kernel log confirmed `QTB ECATS MATCH`, `CP_SMMU_APERTURE(CB0) r0=0 r1=0`, and `Adreno 740 READY (CHIP_ID=43050a01 CB0+CB2+GMU+SQE+ZAP non-sec live)`.
  - `s23gpu_test.exe` Run 1 (`LUID 0:014e7796`, `TS=154466028704->154466389966`, `d=361262`):
    - **Test 1** (GEM alloc + CPU write + GPU `CP_MEM_WRITE`): `PASS` (`p0=cafebabe,5230a740 p1=11223344,55667788`).
    - **Test 2** (100 sequential `GEM_SUBMIT` + `WAIT_FENCE` loop): `PASS` (`last val=0xa7400064 fence=103`).
    - **Test 3** (`SET_IOVA=0x10000000` softpin): `PASS` (`ret=0 val=0x50f70001`).
    - **Test 4** (BO close + re-alloc): `PASS`.
  - Clean `StopDevice` + cold-restart cycle (`pnputil /disable-device` -> `/enable-device` on `ROOT\DISPLAY\0000`) -> Run 2 (`LUID 0:0151d263`, `TS=154908831397->154909044075`, `d=212678`): all 4 tests passed identically with `s23link.py ping` (`pong 2.0 ms`).

### 5.7 Live ARM64 Mesa 24.3 (`Freedreno` + `d3d10umd` + `WGL`) Hardware-Accelerated Direct3D 11, DXGI SwapChain & OpenGL 4.6 Rendering (`s23gpu.sys v1.0.0.5`)

- **WSL2 ARM64 Mesa 24.3 (`62f1e49a1eb3a408f748725326fe53447887f97d`) Cross-Build (`/home/mahan/mesa_s23/`)**:
  - Cross-compiled `libgallium_d3d10.dll` -> `s23gpu_umd.dll` (`8,237,952 B`), `libgallium_wgl.dll` (`13,479,296 B`), and `opengl32.dll` (`137,088 B`, all signed with `CN=S23TouchTest2020`, `[1050/1050]` targets) using `clang-18 --driver-mode=cl --target=aarch64-pc-windows-msvc` + `lld-link`, MSVC `14.44.35207` ARM64 `/MT` static CRT, Windows SDK `10.0.26100.0` ARM64 libraries, and static `zlib` (`-Dgallium-drivers=freedreno -Dgallium-d3d10umd=true -Dopengl=true -Dfreedreno-kmds=msm -Db_vscrt=mt -Dzlib=enabled -Dzlib:default_library=static`).
  - Applied `mesa-G739` (`src/freedreno/drm/wddm/fd_wddm.c` + `src/gallium/frontends/d3d10umd/topaz_wddm.cpp`) plus S23-specific patch [`drivers/s23gpu/mesa_s23_a740.patch`](../drivers/s23gpu/mesa_s23_a740.patch) (`402 lines`):
    1. `meson.build`: `(cc.get_argument_syntax() == 'msvc' ? '/FI' : '-include')` for `fd_wddm_compat.h`.
    2. `fd_wddm.c` / `xf86drm.h`: translate KMD `-62 -> -ETIMEDOUT (138)` and `-38 -> -ENOSYS (40)` for Windows UCRT `errno.h`, and add `fd_wddm_open_kmt()` (`D3DKMTEnumAdapters3`/`D3DKMTQueryAdapterInfo` `TOPAZGPU_ESC_MAGIC` + `D3DKMTCreateDevice` + `D3DKMTEscape`) so OpenGL WGL (`libgallium_wgl.dll`) opens `s23gpu.sys` directly.
    3. `topaz_wddm.cpp`: default `nowait = 0` (`TOPAZ_NOPRESENTWAIT` opt-in) so Render-Only presents wait for the GPU BO fence before `pfnPresentCb`, and exclude `dwm.exe` by default in `TopazWddmEnabled()` (unless `C:\topaz\umd.allowdwm` or `C:\topaz\umd.dwmonly` exists) so `C:\topaz\umd.enable` can be enabled globally for all applications while `dwm.exe` stays safely on `ROOT\BasicDisplay\0000`.
    4. `src/gallium/targets/wgl/{meson.build, wgl.c}`: wire `GALLIUM_FREEDRENO` (`driver_freedreno`) into `wgl_screen_create_by_name`, `wgl_screen_create`, and `wgl_present` (`pipe_texture_map` + `StretchDIBits`).
- **KMD `s23gpu.sys v1.0.0.5` Enhancements (`70,016 B`, `PE VERIFICATION: PASS` 48/48 `ntoskrnl` + 3/3 `hal`)**:
  - `msm_validate.h`: check `c->nr_relocs != 0u` only (upstream Mesa `msm_ringbuffer_sp.c` allocates `drm_msm_gem_submit_cmd` on the stack with `nr_relocs = 0` and uninitialized `relocs`).
  - `msm.c` / `hw.c`: QPC-based `KeQueryPerformanceCounter` timeout loop in `DRM_MSM_WAIT_FENCE` with live A740 fault telemetry (`RPTR`, `WPTR`, `RBBM_STATUS`, `CP_HW_FAULT`, `CB0_FSR`, `CB0_FAR`) and multi-context `g_LastSubmitOwner` tracking (`CP_SET_AMBLE(1)` `CTX_RESTORE_BUF` preamble emitted on context switch).
  - `ddi.c` / `eng.c`: WDDM allocation private data (`TOPAZGPU_ALLOC_MAGIC = 0x31415054`), Render-Only `DxgkDdiPresent` (`TgPresent`) & `DxgkDdiBuildPagingBuffer`, and 128-slot `EngSubmit` fence ring so `DxgkCbNotifyInterrupt` + `DxgkCbNotifyDpc` complete every DXGI `PresentCb` cleanly.
- **Live On-Device Verification (`drivers/s23gpu/s23gpu_d3d11_test.c` & `drivers/s23gpu/s23gpu_opengl_test.c`, captured in `drivers/s23gpu/s23gpu_v3_d3d11_live.log`)**:
  - **`FD_MESA_DEBUG=sysmem` (System Memory Bypass Mode)**:
    - `D3D11CreateDevice` on `Qualcomm Adreno 740 Render-Only GPU (S23)` succeeded with `FeatureLevel = 0xa000`.
    - **Test 1 (Clear RT `256x256`)**: `@128,128 RGBA = (51,102,204,255)` -> **`PASS`**.
    - **Test 2 (HLSL VS+PS 3D Triangle)**: `corner(4,4)=(13,13,38,255)`, `center(128,140)=(108,74,73,255)`, `top(128,70)=(216,20,19,255)`, `right(175,190)=(32,184,39,255)`, `left(81,190)=(32,40,183,255)` -> **`PASS`**.
    - **Test 3 (120-Frame Rotating 3D Triangle Benchmark)**: `120 frames in 43.83 ms` (**`2,738.1 FPS`**) -> **`PASS`**.
  - **Default 3 MiB On-Chip GMEM Tiled Binning Mode (`0x01000000 + 0x00300000`, `HBB = 16`, Global `C:\topaz\umd.enable`)**:
    - Tests 1–3 **`ALL PASS`**, achieving **`2,674.0 FPS` cold (`44.88 ms`)** and **`4,710.4 FPS` warm (`25.48 ms`)**.
    - Pixel-for-pixel comparison between `sysmem` and `gmem` `256x256` readback BMPs: **`max_abs_diff = 0` across all `196,608` color channels**.
  - **Session 1 Interactive Desktop `IDXGISwapChain` (`s23gpu_d3d11_test.exe --window`)**:
    - `IDXGIFactory::CreateSwapChain` on Adreno 740 (`DXGI[1]` alongside `ROOT\BasicDisplay\0000` on `DXGI[0]`): **`hr = 0x00000000 (S_OK)`**.
    - `IDXGISwapChain::Present(0, 0)` across 60 consecutive interactive frames: **`DXGI Present #1..#60 -> 0x00000000 (S_OK)`**.
  - **Live Hardware OpenGL 4.6 + WGL (`s23gpu_opengl_test.exe` with `opengl32.dll` + `libgallium_wgl.dll`)**:
    - **`GL_VENDOR: freedreno`**, **`GL_RENDERER: FD740`**, **`GL_VERSION: 4.6 (Compatibility Profile) Mesa 24.3.0-devel (git-62f1e49a1e)`**, **`GL_SHADING_LANGUAGE_VERSION: 4.60`**.
    - **Test 1 (`glClear 256x256`)**: `@128,128 RGBA = (51,102,204,255)` -> **`PASS`**.
    - **Test 2 (`GLSL VS+FS 3D Triangle + SwapBuffers`)**: `corner=(13,13,38,255)`, `center=(108,74,73,255)`, `top=(216,20,19,255)`, `right=(32,184,39,255)`, `left=(32,40,183,255)` (**`max_abs_diff = 0` vs D3D11 GMEM BMP!**) -> **`PASS`**.
    - **Test 3 (120-Frame Rotating 3D Triangle OpenGL Benchmark)**: `120 frames in 12.93 ms` (**`9,280.0 FPS`**) -> **`PASS`**.

### 5.8 WDDM 2.0 Full Display + 120 Hz Coherent Scanout + Native Brightness & Night Light (`s23gpu.sys v1.0.1.7`)

- **WDDM 2.0 Full Display-3D Transition (`drivers/s23gpu/disp.c`, `ddi.c`, `s23gpu.inf`)**:
  - Promoted `s23gpu.sys` from WDDM 1.3 Render-Only (`NumSources=0, NumChildren=0`) to **WDDM 2.0 Full Display + 3D** (`NumSources=1, NumChildren=1`, `ROOT\DISPLAY\0000`, `Qualcomm Adreno 740 GPU (S23)`, `92,032 B`, `PE VERIFICATION: PASS` `58/58` `ntoskrnl` + `3/3` `hal`, MD5 `06B77DEE793F52D060CAF2963E37A14E`, `DriverVer=04/01/2024,1.0.1.7`).
  - Added single-display EDID + monitor mode recommendation (`SAM0911`, `"S23 AMOLED 2X"`, `1080x2340 @ 120 Hz`), coherent WBWA scanout worker (`scanout #300 last=2520 us avg=2214 us`, `pitch=4352->4320`, `bo=1`), 120 Hz `MDP_INTF_1_AUTOREFRESH_CONFIG` activation (`AR 0x80000002->0x80000001`, `fc_delta 6->14 per 100ms`), `D3DKMDT_GAMMA_RAMP` (`RGB256x3x16`) software LUT composition for Windows Night Light, and `DXGK_BRIGHTNESS_INTERFACE_2` DSI0 TPG-FIFO DBV brightness control (`S23_DBV_MIN..S23_DBV_MAX`).
  - **24H2 WDDM 2.0 Display Bring-Up Findings**:
    1. `DispEnumCofuncModality` requires `p.ContentTransformation.RotationSupport.Offset0 = 1` (preventing `0x113` `VIDEO_DXGKRNL_FATAL_ERROR` BSOD fixed in `v1.0.1.5`).
    2. `dxgkrnl!DpiFdoEnumChildDevices` fails with `STATUS_NOT_SUPPORTED (0xC00000BB)` if a non-POST root-enumerated adapter (`ROOT\S23GPU`) reports `D3DKMDT_VOT_INTERNAL`; reporting `D3DKMDT_VOT_OTHER` allows `ROOT\DISPLAY\0000` to start cleanly (`CM_PROB_NONE`) while `dxgkrnl!DpiBrightnessStartDevice` still binds `GUID_DEVINTERFACE_BRIGHTNESS_2`.
    3. Setting `REG_DWORD BrightnessControl = 1` on monitor driver key `HKLM\SYSTEM\CurrentControlSet\Control\Class\{4d36e96e-e325-11ce-bfc1-08002be10318}\0001` (`DISPLAY\SAM0911\1&28A6823A&0&UID0`) enables `monitor.sys` WMI/IOCTL brightness (`WmiMonitorBrightness`: `Active=True, Levels=101`) across cold reboots.
    4. [`drivers/s23gpu/s23disp_only.c`](../drivers/s23gpu/s23disp_only.c) (`s23disp_only.exe`) calls `SetDisplayConfig` to deactivate the legacy `BasicDisplay` path (`PATH[1]: active=0`) and lock `1080x2340 @ 120/1 Hz` on `PATH[0]` (`"S23 AMOLED 2X"`), which persisted across cold reboot.
- **Post-Cold-Reboot Live Verification (`s23gpu.sys v1.0.1.7`, captured in `drivers/s23gpu/s23gpu_v4_desktop120hz_live.log`)**:
  - **PnP & Display Mode**: `ROOT\DISPLAY\0000` `Status=OK`, `Problem=CM_PROB_NONE`, `VER=1.0.1.7`; `s23disp_only.exe` confirmed pre-apply `QDC_ALL_PATHS` had single active path `"S23 AMOLED 2X"` (`1080x2340 @ 120/1 Hz`, `BasicDisplay` `active=0`). On `v1.0.1.6`, Session-1 `dxdiag` reported `Full Device`, `Driver Model: WDDM 2.0`, `1080 x 2340 (32 bit) (120Hz)`.
  - **Task Manager / WDDM 2.0 GPU Performance Counters**: `\GPU Engine(*)\Utilization Percentage` exposed Adreno 740 LUID `0x00000000_0x00007666` (`phys_0_eng_0_engtype_3d`, 39 total instances); `\GPU Adapter Memory(luid_0x00000000_0x00007666_phys_0)` reported `shared usage = 32,133,120 B`, `total committed = 212,844,544 B`.
  - **Native Brightness**: `WmiMonitorBrightness` persisted (`Active=True, Levels=101`); boot applied `pct=100 -> dbv=0x7ff ok=1` and `pct=60 -> dbv=0x2ed ok=1`; `WmiSetBrightness` verified `25% -> dbv=0x092 ok=1` and `60% -> dbv=0x2ed ok=1`.
  - **Native Night Light & GammaRamp**: Windows Night Light (`bluelightreductionstate` `B18=0x15 ACTIVE=True`) applied `disp: GammaRamp type=2 active=1 r255=0xfc g255=0xd0 b255=0xa3` alongside boot identity ramps (`type=1`/`type=2 active=0`), and Session 1 GDI `SetDeviceGammaRamp` warm/identity returned `True(0)`.
  - **Hardware 3D Verification**: `s23gpu_d3d11_test.exe` (`DXGI[0]` `"Qualcomm Adreno 740 GPU (S23)"`, `luid=0:00007666`, `FeatureLevel=0xa000`) **`ALL PASS`** offscreen (**`4,441.1 FPS`**) and Session 1 windowed (**`3,825.9 FPS`**, `CreateSwapChain hr=0x00000000`); `s23gpu_opengl_test.exe` (`GL_VENDOR=freedreno`, `GL_RENDERER=FD740`, `OpenGL 4.6`, `Mesa 24.3.0-devel`) **`ALL PASS`** (**`5,813.3 FPS`**).
- **Smooth 120 Hz VSync + Async S6E3FAC Smooth Dimming (`s23gpu.sys v1.0.1.8`, `94,080 B`, MD5 `5C0A7EBF5CF059DAE474082A50D3F28F`)**:
  - **High-Resolution 120 Hz VSync (`ExAllocateTimer(..., EX_TIMER_HIGH_RESOLUTION)` + `ExSetTimer(-83333, 83333, ...)` + QPC-phase-locked `DispGetScanLine`)**: Overcame Windows' default `15.625 ms` clock tick (`NtQueryTimerResolution cur=156250`), lifting Session-1 `DwmFlush()` from **`62.10 FPS`** (`avgMs=16.10, maxMs=29.65` on `v1.0.1.7`) to **`115.87 FPS`** (`avgMs=8.63, minMs=7.97, maxMs=9.38` across 180 consecutive frames on `v1.0.1.8`). (`PE VERIFICATION: PASS` `59/59` `ntoskrnl` + `3/3` `hal`.)
  - **Non-Blocking Smooth Brightness (`DispBrightThread` + CIE L\* LUT `0x008..0x7FF` + `S6E3FAC` `0x94 0x70 0x0F` / `0x53 0x28`)**: Replaced synchronous `DispSleepMs(20u)` and `0x53 0x20` (`SMOOTH_DIM OFF`) with fast DSI0 idle polling, an async coalescing worker thread, 15-frame hardware smooth dimming (`0x94 0x70 0x0F`, `0x53 0x28`), and a 101-step CIE L\* DBV table (`8..2047`). Verified `SetDbv ok=1` in **`1,026–5,950 us`** across `10..80%` (`49%->0x1a8`, `59%->0x26e`, `60%->0x285`), with L1-buffered `CopyRowWithLut` WC bursts (`2,677–3,037 us` steady-state Night Light scanout) and `v1.0.1.8` 3D (`s23gpu_d3d11_test` **`4,921.9 FPS`**, `s23gpu_opengl_test` **`5,516.5 FPS`**, `ALL PASS`).
- **True 120 Hz Desktop Animations, 0.5 ms Timer Quantum & Row-Delta Scanout (`s23gpu.sys v1.0.1.9`, `96,128 B`, MD5 `FE859797E77C361C676EF6E6FE27D2C7`)**:
  - **Root Causes of Low-Hz Desktop Animations Identified via `TOPAZ_TIMING=1` / `TOPAZ_ESCLOG=1` in `dwm.exe`**:
    1. **Global Windows Clock Quantum (`curRes = 156250` = `15.625 ms` = `64 Hz`)**: `EX_TIMER_HIGH_RESOLUTION` uses tickless one-shot interrupts without lowering the system clock tick, leaving `s23touch.sys`'s `SleepMs(4)` touch polling quantized to **`63.1 Hz`** and any `SleepMs(1u)` in `HwWaitFenceEx` sleeping **`8.5–15.6 ms`**. Calling `ExSetTimerResolution(5000u, TRUE)` (`0.5 ms`) in `DispStart` (and `ExSetTimerResolution(0u, FALSE)` in `DispStop`) lowered system-wide `NtQueryTimerResolution` `curRes` to **`5,000` (`0.5 ms`)** and boosted `Sleep(4)` touch polling to **`221.6 Hz`**.
    2. **`HwWaitFenceEx` Spin Ceiling (`250 us` -> `3,000 us` @ `2 us` step)**: DWM full-screen composition on Adreno 740 takes `~300–600 us`; spinning only `250 us` caused `HwWaitFenceEx` to fall into `SleepMs(1u)` (`gpu wait = 8.52–11.06 ms` per frame in `dwm.exe`). Extending the RAM fence poll (`g_MemptrsVa[MEMPTRS_OFF_FENCE_BR]`) to `3,000 us` at `2 us` resolution detects completion within `2 us`, boosting `s23gpu_d3d11_test` from `4,921.9 FPS` to **`7,587.4 FPS`** and `s23gpu_opengl_test` from `5,516.5 FPS` to **`19,796.4 FPS`**.
    3. **UMD `TOPAZ_SCFIX=0` & `TOPAZ_NOPRESENTWAIT=1` (`C:\ProgramData\topaz\umd.env` + Machine Env)**: `TOPAZ_SCFIX` (default ON) performed up to 8 `1080x2340` strip copies (`resource_copy_region` to/from `stripTex`) around every scissored draw, splitting each DWM frame into **38–65 `DRM_MSM_GEM_SUBMIT` calls (`3,307 submits/s`)** and causing 137–452 BO cache misses/s. Setting `TOPAZ_SCFIX=0` and `TOPAZ_NOPRESENTWAIT=1` reduced DWM submits to `~6–10/frame`, eliminated BO cache misses (`hits 1732, miss 0, busy-head 0`), and dropped internal DWM 60-present frame time from **`19.9–21.3 ms` (`46.9–50.2 FPS`)** to **`8.6 ms` (`116.3 FPS`)**.
    4. **Dedicated `DispScanThread` (`LOW_REALTIME_PRIORITY`) + `10.11 MB` `g_ShadowVa` Row-Delta Scanout**: Decoupled framebuffer scanout from `EngThread` WDDM DMA fence completion and added per-row `RtlCompareMemory` delta skipping against a cached `g_ShadowVa` buffer (`shadow=FFFFD783CA000000`). Even with Night Light active (`GammaRamp type=2 active=1 r255=0xfc g255=0xd0 b255=0xa3`), scanout copy time during a live Session-1 moving/repainting window animation averaged **`1,475–2,226 us` (`rows=352..733/2340`)**, and end-to-end Session-1 animated window throughput jumped from **`45.4 FPS` (`avg=22.04 ms`)** to **`110.48 FPS` (`277 frames in 2.507 s, avg=9.05 ms`)** (`DwmFlush()` **`114.15 FPS`**). (`PE VERIFICATION: PASS` `60/60` `ntoskrnl` + `3/3` `hal`.)
- **Touch Keyboard & AppContainer Shell Isolation, Freedreno BO-Renaming & 2x2 Scissor Fixes, WDDM 2.0 Preemption (`DXGK_INTERRUPT_DMA_PREEMPTED`), Live Task Manager 3D GPU Utilization & ARM64/x64 OpenGL 4.6 ICD (`s23gpu.sys v1.0.2.3`, `97,664 B`, MD5 `B2AFC674FD33050816C955BEA1DDC14D`)**:
  - **Touch Keyboard (`TextInputHost.exe`) & AppContainer XAML Shell Isolation (`TopazWddmEnabled` default `dwm`-only + `*S-1-15-2-1`/`*S-1-15-2-2` ACLs)**: Made `dwm`-only mode the compiled-in default in `TopazWddmEnabled()` (`topaz_wddm.cpp`, with `C:\topaz\umd.allprocs` / `TOPAZGPU_FORCE=1` opt-in), granted `ALL APPLICATION PACKAGES` (`*S-1-15-2-1`) and `ALL RESTRICTED APPLICATION PACKAGES` (`*S-1-15-2-2`) read permissions on `C:\topaz` and `C:\ProgramData\topaz`, and refreshed stale shell processes in `deploy_s23gpu.ps1`. Previously, AppContainer processes (`TextInputHost.exe`, `StartMenuExperienceHost.exe`, `SearchHost.exe`, `ShellHost.exe`, `SystemSettings.exe`) received `ERROR_ACCESS_DENIED` on `GetFileAttributesA("C:\\topaz\\umd.dwmonly")` and kept loading `s23gpu_umd.dll`; they now cleanly decline `s23gpu_umd.dll` and render via Windows' native ARM64 NEON D3D11.1 path into shared Adreno 740 GEM BO redirection textures (`D3DKMDT_GDISURFACE_TEXTURE`) composited by `dwm.exe` at 120 Hz.
  - **Moving-Window Dirty-Rect & BO-Renaming Fixes (`drivers/s23gpu/mesa_s23_a740.patch`, `drivers/s23gpu/msm.c`)**:
    1. Fixed Freedreno `invalidate_resource()` (`freedreno_resource.c:760`) to call `realloc_bo()` before `rebind_resource()`, added post-shadow `rebind_resource(rsc)` in `fd_try_shadow_resource()`, and fixed `rebind_resource_in_ctx()` (`freedreno_resource.c:74,86,105,119,132,147`) to check `ctx->dirty_resource` for VBOs/streamout and `ctx->dirty_shader_resource[stage]` (`FD_DIRTY_SHADER_CONST`, `FD_DIRTY_SHADER_TEX`, `FD_DIRTY_SHADER_IMAGE`, `FD_DIRTY_SHADER_SSBO`) across UBO slots `0..num_ubos-1`.
    2. Enabled zero-flush constant-buffer renaming in `UpdateSubresourceUP` (`Resource.cpp:1085`, `TOPAZ_UPDFLUSH=0` default, `PIPE_MAP_DISCARD_WHOLE_RESOURCE` when `!pDstBox || (box.x == 0 && box.width >= dst_resource->width0)`), eliminating mid-frame batch flushes (`NO_OVERWRITE flushes/frame = 0.0`, `BO cache: hits 910 miss 0`).
    3. Gated strip-copy batch splits on `TOPAZ_STRIPS=1` (default `0` in `Rasterizer.cpp:133`) while keeping `2x2` pixel-quad hardware scissor alignment (`TOPAZ_SCFIX=1`) and synchronous present fence completion (`TOPAZ_NOPRESENTWAIT=0`), eliminating moving-window edge derivative artifacts while reducing `DRM_MSM_GEM_SUBMIT` rate from `3,000+/s` to `11–38/s`.
    4. Removed the `waitMs == 0u` bypass in `DRM_MSM_WAIT_FENCE` (`msm.c:581`) and first-poll `DXGI_DDI_ERR_WASSTILLDRAWING` delay for `D3D10DDI_QUERY_EVENT` (`Query.cpp:255`) so event queries complete within a single `2 us` RAM fence poll.
  - **System-Wide WDDM OpenGL 4.6 ICD (ARM64 + x64) & FurMark 2 x64 Live on Adreno 740 (`s23gpu.inf`, `build.ps1`, `deploy_s23gpu.ps1`, `/home/mahan/mesa_s23/build_x64`)**:
    1. Registered `OpenGLDriverName = "libgallium_wgl.dll"`, `OpenGLVersion = 0x1000`, `OpenGLFlags = 3` in `s23gpu.inf` (`[S23Gpu_AddReg]`) and packaged `libgallium_wgl.dll` into `%11%` (`C:\Windows\System32\libgallium_wgl.dll`).
    2. Guarded `fd_wddm_compat.h` with `#ifndef __ASSEMBLER__` and cross-compiled `x86_64-pc-windows-msvc` (`Machine = 0x8664`) `libgallium_wgl.dll` (`14,353,792 B`) and `opengl32.dll` (`139,648 B`) in `/home/mahan/mesa_s23/build_x64` (`[1102/1102]` targets), talking directly to `s23gpu.sys` via `fd_wddm_open_kmt()` (`D3DKMTEscape`).
    3. Verified live in **Geeks3D FurMark 2 x64 (`FurMark_GUI.exe` / `furmark.exe`)**: detected `GPU1: Qualcomm Adreno 740 GPU (S23), VBIOS: A740.4001.0018, driver: 1.0.2.2 GL:libgallium_wgl.dll`, initialized an **OpenGL 4.6 context (`GL_RENDERER: FD740`, `GL_VENDOR: freedreno`, `GL_VERSION: 4.6 (Compatibility Profile) Mesa 24.3.0-devel`, `315 extensions`)**, and compiled scenes with `Scene compilation: SUCCESS`.
  - **WDDM 2.0 Preemption Notification (`DXGK_INTERRUPT_DMA_PREEMPTED`), VidMM Paging Bypass & Live Task Manager 3D Utilization (`ddi.c`, `eng.c`, `hw.c`, `disp.c`, `topaz_wddm.cpp`)**:
    1. Added a dedicated Node 0 WDDM telemetry context (`topazRenderCtx`) in `TopazWddmNotifyRender` (`topaz_wddm.cpp`, removed from `Flush()` in `Device.cpp`) that rotates `D3DDDICB_RENDER` command/allocation/patch buffers properly and submits a 4-byte Node 0 render notification overlapping `screen->fence_finish()`.
    2. Implemented allocation-free static preemption slots (`g_PreemptSlots[8]`) in `EngPreempt` (`eng.c`) and `DXGK_INTERRUPT_DMA_PREEMPTED` notification (`DmaPreempted.PreemptionFenceId`, `LastCompletedFenceId = Adapter->LastCompletedFence`) in `NotifyRoutine` / `TgPreemptCommand` (`ddi.c`), plus immediate preemption/paging wakeup via `g_AbortStall`, `g_QueueDepth`, and interruptible `KeWaitForMultipleObjects` (`0%` CPU spin), eliminating 2-second WDDM Preemption Timeout TDRs during heavy window dragging.
    3. Distinguished VidMM paging/control submissions (`A->Flags.Paging`, `ContextSwitch`, `NullRendering`, `Flip`) from 3D `TgRender` submissions in `TgSubmitCommand` / `EngThread` so `System (pid_4)` stays at **`0.00%`** GPU usage while `dwm.exe` reports **`5.3%–16.4%`** live 3D GPU utilization in Task Manager (`\GPU Engine(pid_<dwm>_..._engtype_3d)\Utilization Percentage`).
    4. Tuned `DispVsyncEnable` periodic 120 Hz high-resolution timer to `79,500` (`16 x 0.5 ms` ticks), achieving **`122.75–123.22 FPS` (`8.12–8.15 ms`)** `DwmFlush(180)`, **`223.2 Hz`** `Sleep(4)` touch polling, **`114.19 FPS` (`286 frames in 2.505 s, avgMs = 8.76 ms`)** live moving-window animation with constant `dwm.exe` PID (zero TDR / zero device resets), `60%` brightness (`dbv=0x285`), active Night Light (`GammaRamp type=2 active=1`), **`7,062.5–7,390.7 FPS`** D3D11, and **`20,792.7–21,131.0 FPS`** OpenGL 4.6 (`PE VERIFICATION: PASS` `60/60` `ntoskrnl` + `3/3` `hal`).




