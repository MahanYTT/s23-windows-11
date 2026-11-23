# Samsung Galaxy S23 (`dm1q` / `SM-S911B`, `SM8550-AC`) — Offline Windows 11 ARM64 Bring-Up Pass 3 Report

**Date:** 2026-11-21  
**Mode:** 100% Offline (zero `adb`, zero `fastboot`, zero `s23link.py`, zero changes to baseline/active phone images or `backups\`)  
**Target OS:** Windows 11 Pro 24H2 ARM64 (`10.0.26100.4349`)  
**Toolchain:** WSL `clang-18 --target=aarch64-pc-windows-msvc` + `lld-link /DRIVER:WDM /TSAWARE:NO` + forced `.reloc` + PE CheckSum fixup + `iasl 20230628` + Authenticode SHA-256 (`CN=S23TouchTest2020`, thumbprint `05DBD92557F4FC52EF2A5E5955789D8EED637C68`) + `verify_pe.py` against phone `ntoskrnl_phone.exe` (3,246 exports) & ARM64 `hidclass.sys`.

---

## Executive Summary

All **4 offline engineering workstreams** are complete, compiled with zero warnings (`-Wall -Wextra -Werror` / `iasl -tc` 0 warnings), signed, and PE-verified:

| Task | Scope | Build & Verification Result |
|---|---|---|
| **Task 1A** | **QCA6490 Wi-Fi UEFI DXE & ACPI** (`s23_uefi_pcie0_dxe.c`, `s23_pcie0_wlan.asl/.aml/.hex`) — 6 audited bugs fixed | `iasl -tc`: **0 Errors, 0 Warnings, 0 Remarks** (`618 B` AML); `clang-18 -Wall -Wextra -Werror`: **4/4 PASS** (Standalone + Mu_Basecore, with & without `-DS23_INCLUDE_SSDT_AML`) |
| **Task 1B** | **Adreno 740 GPU Probe `v1.0.0.2` & UEFI DXE** (`s23gpuprobe.c/.sys`, `s23_uefi_gpu_dxe.c`, `verify_pe.py`) — 7 audited bugs fixed | `s23gpuprobe.sys` (`58,248 B`): **10/10 `ntoskrnl_phone.exe` OK, `missing=[]`, `PE VERIFICATION: PASS`**; `s23_uefi_gpu_dxe.c`: **3/3 PASS** (`-Wall -Wextra -Werror`) |
| **Task 1C** | **AMOLED Display Auto-Refresh Fix DXE v2** (`S23DisplayFixDxe_v2.c`, `s23_display_autorefresh_fix_v2.patch`) | `clang-18 -Wall -Wextra -Werror` vs `Mu_Basecore`: **2/2 PASS** (`WAKE=1` & `WAKE=0`); `patch --dry-run -p1`: **PASS** |
| **Task 2** | **EPSS CPUfreq + 41-Sensor TSENS v2 Driver** (`drivers\s23perf\pkg\s23perf.sys` + `install_s23perf.ps1`) | `s23perf.sys` (`37,256 B`): **10/10 `ntoskrnl_phone.exe` OK, `missing=[]`, `PE VERIFICATION: PASS`** (`Mode=0` read-only default; `Mode=1` thermal-guarded <=75 C vote) |
| **Task 3** | **Physical Power & Volume Buttons HID Minidriver** (`drivers\s23keys\pkg\s23keys.sys/.inf/.cat` + `install_s23keys.ps1`) | `s23keys.sys` (`14,216 B`): **24/24 `ntoskrnl_phone.exe` + 1/1 ARM64 `hidclass.sys` OK, `missing=[]`, `PE VERIFICATION: PASS`**; `Test-FileCatalog`: **`Valid` (SHA-256)** |
| **Task 4** | **QCA6490 Bluetooth 5.3 Blueprint, ACPI & Safe UART14 Probe** (`S23_QCA6490_BT_BRINGUP.md`, `s23_bt_uart14.asl/.aml/.hex`, `drivers\s23btprobe\pkg\s23btprobe.sys` + `install_s23btprobe.ps1`) | `iasl -tc`: **0 Errors, 0 Warnings, 0 Remarks** (`705 B` AML); `s23btprobe.sys` (`45,448 B`): **10/10 `ntoskrnl_phone.exe` OK, `missing=[]`, `PE VERIFICATION: PASS`** |

---

## Critical Hardware Discoveries Verified Against Live S23 DTS & Kernel Source

1. **SM8550 CPUfreq uses `qcom,cpufreq-epss` (`epss_soc_data`), NOT legacy OSM (`qcom-cpufreq-hw.c` L515–543, `s23_live.dts` L10101):**
   - Domains: Silver `0x17D91000` (CPUs 0..2), Gold `0x17D92000` (CPUs 3..6), Prime `0x17D93000` (CPU 7, SM8550-AC up to 3.36 GHz).
   - Verified EPSS offsets: `REG_ENABLE = 0x000`, `REG_DOMAIN_STATE = 0x020`, `REG_FREQ_LUT = 0x100` (4 B stride), `REG_VOLT_LUT = 0x200` (4 B stride), `REG_INTR_CLEAR = 0x308`, `REG_INTR_STATUS = 0x30C`, **`REG_PERF_STATE = 0x320`**, **`REG_CYCLE_CNTR = 0x3C4`** (accumulative per-core counters at `0x3C4 + (core + 1) * 4`). Using legacy OSM offsets (`0x110`/`0x920`) would read wrong registers and write outside EPSS `PERF_STATE`.
2. **TSENS v2 Split TM / SROT Bases (`s23_live.dts` L23779–23805, `tsens.c` L868–897):**
   - `tsens0` (16 sensors): TM `0x0C271000`, SROT `0x0C222000`; `tsens1` (16 sensors): TM `0x0C272000`, SROT `0x0C223000`; `tsens2` (9 sensors): TM `0x0C273000`, SROT `0x0C224000` -> **41 thermal sensors total**.
   - `Sn_STATUS` at `TM + 0xA0 + 4*n`: bits `[11:0]` = signed 12-bit two's complement deciCelsius (`sign_extend(raw & 0xFFF, 11)`), bit `21` = `VALID`.
3. **Physical Buttons Exact SPMI `RT_STS` Offset (`+0x10`, NOT `+0x08`):**
   - Verified in `qpnp-power-on.c`, `pm8941-pwrkey.c`, and `pinctrl-spmi-gpio.c`: both `PMK8550 PON_HLOS` (`PPID 0x013`, subtype `0x09`) and `PM8550 GPIO6` (`PPID 0x18D`, type `0x10`) expose real-time pin state at **`+0x10`** (`PON`: bit 7 = Power active-high, bit 6 = VolDown active-high; `GPIO6`: bit 0 = VolUp active-low). `s23keys.sys` maps ONLY the read-only SPMI Observer (`0x0C440000`) and never maps `SPMI_CHNLS` (`0x0C500000`).
4. **QCA6490 Bluetooth UART14 (`0x00898000`) Uses GENI FIFO Mode (Zero DMA/GSI Dependency):**
   - `s23_live.dts` L22676–22693 omits `dmas` on `qcom,qup_uart@898000` (GIC SPI 461 -> ACPI GSI 493, wakeup GPIO 79).
   - Exact `7,372,800 Hz` clock (`115200` baud @ 0.000% error with `16x` oversampling on QUP >= v2.5, `GENI_SER_M/S_CLK_CFG = 0x41`): `GCC + 0x52010` bits `0/1/2/3/10`, RCG `0x1E760..0x1E770` with `CFG=0x00102601`, `M=0x0180`, `N=0xC476`, `D=0xC2F6`.
   - Exact 4x8 LSB-first GENI packing verified against `qcom-geni-se.c`: `PACKING_CFG0 = 0x0004380E`, `PACKING_CFG1 = 0x000C3E0E`, `SE_GENI_BYTE_GRAN = 0`, `SE_GENI_RX_RFR_WATERMARK_REG = rx_depth - 2`.
   - S23 stock BT firmware is already on disk under `wifi_research\s23_vendor_wifi_fw\`: `hpbtfw21.tlv` (`171,680 B`), `hpnv21.bin` (`6,612 B`), `hpnv21g.bin` (`6,452 B`).

---

## Detailed Task Breakdown & Bug-Fix Audit

### Task 1A — QCA6490 Wi-Fi UEFI DXE & ACPI (`wifi_research\qca6490_bringup\`)
All 6 audited bugs fixed in `s23_uefi_pcie0_dxe.c` and `s23_pcie0_wlan.asl`:
1. **SPMI 32 kHz Sleep Clock:** Mapped SPMI Core (`0x0C400000`), Cnfg (`0x0C42D000`), Observer (`0x0C440000`), and Channels (`0x0C500000`) in `MapAllS23Pcie0Regions()` and programmed `PMK8550 GPIO3 (PPID 0x0BA, 0x40..0x46 = 01/00/05/02/03/80)` inside `OnReadyToBoot()`.
2. **Cold Rail Discharge, BCM/VRM Votes & Pass 2 LTSSM Fallback:** Added cold rail discharge (`smpg4`/`smpg6`/`smpe4`/`smpg2` off + 150 ms), `MC0`/`SH0`/`CN0`/`SN7`/`vrm.wlan` votes, and Pass 2 `x2/NL=1 + CLKREQ_OVR=0x02 + PERST#` recovery matching `s23pcieprobe.c v1.0.0.11`.
3. **Root Port Bus Programming Order:** Programmed Root Port primary/secondary/subordinate bus `0/1/1` (`DBI + 0x18 = 0x00010100`) **before** the initial `0x60100000` (`0x110317CB`) probe.
4. **Synthetic `MCFG` ACPI Table Generation:** Synthesizes a valid 60-byte `MCFG` (`Base=0x60000000, Seg=0, StartBus=1, EndBus=1`) in `PublishMcfgAndSsdt()` whenever no existing `MCFG` is installed (`dm1q` ships no stock `MCFG.aml`).
5. **Hardened `RpmhWriteTcs0()`:** Polls `TCS0_STATUS (+0x28 bit 0 == 1)` idle prior to programming, saves/enables/restores `IRQ_EN (+0x00)`, and restores `MSGID`/`ADDR`/`DATA` **before** restoring `CMD_ENABLE (+0x2C)`.
6. **Hardened SMMU-500 & ECAM Transport + `_OSC` Fix:** `ConfigureSmmu500Identity()` verifies `CR0.CLIENTPD==0`, `GFSR==0`, SMR overlap vs `0x1400`/`0x1401`, `ACTLR==3`, and `FSR==0`; `ConfigureBus1EcamTransport()` polls iATU `REGION_EN` and checks absent functions `Df=1..255 == 0xFFFFFFFF`; `s23_pcie0_wlan.asl` L154 masks `_OSC` `CTRL &= 0x0000001C` (PME/AER/PCIe Cap, HotPlug masked) with corrected L203 `_S0W=0` comment.

### Task 1B — Adreno 740 GPU Probe `v1.0.0.2` & UEFI DXE (`drivers\s23gpuprobe\`, `gpu_research\s23_gpu_bringup\`)
All 7 audited bugs fixed:
1. **Self-Contained Stage 2 & Stage 3 (`s23gpuprobe.c`):** Added `EnsureArcRailsLoaded()` and `EnsureGmuAndKgslMapped()` so `StageMask = 0x02` or `0x04` can be invoked standalone without Stage 1.
2. **Hardened `RpmhWriteTcs0()` (both `.sys` and DXE):** Polls `TCS0_STATUS` idle first, hybrid fast-poll + `SleepMs(1)` at `PASSIVE_LEVEL`, saves/restores `IRQ_EN`, and restores `MSGID`/`ADDR`/`DATA` before `V3_CMDEN`.
3. **Corrected `GpuCxAxiGateOpen()`:** Requires BOTH `(ahb & 1) == 1` (branch enabled) AND `(ahb & 0x80000000) == 0` (not halted).
4. **Lucid OLE PLL Safe Reconfiguration (both `.sys` and DXE):** Clears `OUTCTRL` and enters `OPMODE = STANDBY` **before** programming `L_VAL`/`ALPHA`/`CFG`/`TEST`.
5. **Post-GX-Collapse Recovery:** Re-verifies/re-enables GX CBCRs and inspects `CX_HW_CTRL` bit 31 and `GX_DOMAIN_MISC3 (0x9504)` after GX GDSC collapse/enable.
6. **Page-by-Page GCD Mapping (`s23_uefi_gpu_dxe.c`):** `MapMmioRegion()` walks `0x1000` pages safely without clobbering existing descriptors.
7. **Clean RCG `ROOT_EN` Clearing (`s23_uefi_gpu_dxe.c`):** `ProgramRcg()` clears `ROOT_EN` (bit 1) after `CMD_UPDATE` completes.

### Task 1C — AMOLED Display Auto-Refresh Fix DXE v2 (`display_research\`)
Created `S23DisplayFixDxe_v2.c` and `s23_display_autorefresh_fix_v2.patch` (dry-run verified against `~/mu_s23/Mu-Silicium` without modifying the repo):
- Backports the full 5-register `G116Gate` check from `s23disp_diag.c v1.0.0.2`.
- Replaces noisy `FLC_EN == 0` re-arming with clean `AR_STATUS`/`TE_INT_CNT`/`DSI0_CTRL`/`SYNC_VSYNC` stall detection.
- Uses a non-colliding `ExitBootServices` event handler.
- Includes optional QCOM DSI v2.7 TPG-FIFO `S6E3FAC` panel wake fallback (`S23_DISP_ENABLE_DSI_WAKE=0/1`, both compile-tested cleanly with `-Wall -Wextra -Werror`).

### Task 2 — EPSS CPUfreq + TSENS v2 41-Sensor Driver (`drivers\s23perf\`)
- Pure-C ARM64 WDM Demand-Start (`Start = 3`) driver writing telemetry to pre-allocated 64 KB write-through `C:\Users\Public\s23perf.log`.
- **Default `Mode = 0` (100% read-only):** Dumps all 3 EPSS domains (LUT frequency/voltage rows, active `DOMAIN_STATE`, SW `PERF_STATE`, `INTR_STATUS`, per-core `CYCLE_CNTR` deltas over 50 ms) and all 41 TSENS v2 sensors (`tsens0..2` HW_VER, SROT CTRL, TRDY, min/max/valid counts, per-sensor deciCelsius).
- **Opt-in `Mode = 1`:** Reads all 41 sensors before & after voting; clamps target indices strictly to `[0, lut_count - 1]` (default 75% safe non-boost index); **immediately throttles all 3 domains to LUT index 0** if ANY valid sensor >= `ThrottleTempC` (default 75 C, hard-capped at 75.0 C).

### Task 3 — Physical Power & Volume Buttons HID Minidriver (`drivers\s23keys\`)
- Modeled directly on `drivers\s23touch\s23touch.c` (`ROOT\S23KEYS`, `HidRegisterMinidriver`, 24 `ntoskrnl.exe` + 1 `hidclass.sys` import).
- **Strictly read-only SPMI Observer (`0x0C440000`)** — never maps `SPMI_CHNLS` (`0x0C500000`). Dynamically resolves APIDs for `PPID 0x013` (`PON_HLOS`, Power bit 7 + VolDown bit 6) and `PPID 0x18D` (`PM8550 GPIO6`, VolUp bit 0 active-low) from `SPMI_CORE + 0x2000`.
- 20 ms software debounce; emits HID Report ID 1 (Consumer Control: Volume Increment `0xE9`, Volume Decrement `0xEA`) and Report ID 2 (Generic Desktop System Control: System Power Down `0x81`). Logs events to `C:\Users\Public\s23keys.log`.
- Packaged with `s23keys.inf`, SHA-256 signed `s23keys.cat` (`OS=10_ARM64,10_X64`, `HWID1=root\s23keys`, `Test-FileCatalog Status: Valid`), `s23test.cer`, and `install_s23keys.ps1`.

### Task 4 — QCA6490 Bluetooth 5.3 Bring-Up Blueprint, ACPI & Safe UART14 Probe (`bt_research\`, `drivers\s23btprobe\`)
- `bt_research\S23_QCA6490_BT_BRINGUP.md`: Complete S23 hardware map, GCC 7.3728 MHz & 3.2/4.0 Mbaud RCG math, TLMM GPIOs (`76..82`, `204`), shared QCA6490 power rails + `wlan_pdc`, parsed headers of on-disk `hpbtfw21.tlv` / `hpnv21.bin` / `hpnv21g.bin`, QCA EDL TLV download sequence, and recommended custom WDM `IOCTL_BTHX_*` (`BthMini`/`BthPort`) bus driver architecture.
- `bt_research\s23_bt_uart14.asl` -> `.aml` (`705 B`) & `.hex` (`7,099 B`): Inert `_STA = 0` default SSDT declaring `\_SB.UR15` (`0x00898000`, GSI 493, Wake GPIO 79) and child `BTH0` (`QCOM0C6B`, `UARTSerialBusV2` 115200 8N1 HW flow control + `BT_EN` GPIO 81).
- `drivers\s23btprobe\pkg\s23btprobe.sys` (`v1.0.0.1`): Demand-Start (`Start = 3`) pure-C ARM64 WDM probe logging to 64 KB write-through `C:\Users\Public\s23btprobe.log`:
  - `StageMask = 0x01` (**default, 100% read-only**): Dumps GCC QUP2/SE6 vote/CBCRs/RCG, TLMM `GPIO76..82` & `GPIO204`, SPMI Observer `PMK8550 GPIO3`, CMD_DB QCA6490 resources, and performs an **AXI-clock-gated** snapshot of `0x008C0000`/`0x00898000` (zero SError risk when clocks are halted).
  - `StageMask = 0x03` (opt-in active probe): Enables QUP2/SE6 clocks at `7,372,800 Hz`, verifies `proto == 2` (`GENI_SE_UART`) and `GENI_IF_DISABLE_RO bit 0 == 0` (FIFO confirmed), configures 115200 8N1 GENI FIFO UART (`PACKING_CFG0=0x0004380E`, `CFG1=0x000C3E0E`, `SER_CLK=0x41`, `RFR_WM=rxDepth-2`, polling mode with `SE_IRQ_EN=0`), runs `btpower.c` `GPIO204 XO_CLK` + `GPIO81 BT_EN` bootstrap pulse, and sends `HCI_Reset`, `EDL_PATCH_VER_REQ_CMD`, and `EDL_GET_BUILD_INFO_REQ` with full RX hex/ASCII dump & automatic QCA EDL version decoding (`hpnv21g.bin` vs `hpnv21.bin`).
  - `StageMask = 0x07` (opt-in standalone rails + active probe): Also enables `PMK8550 GPIO3` 32 kHz clock, RPMh ARC/XO/VRM rails (`ldob15`, `smpg2`, `smpe4`, `smpg6`, `smpg4`), and AOSS QMP `wlan_pdc` so BT can be tested even before Wi-Fi is active.

---

## Complete File Inventory, Sizes, MD5 Checksums & Verification Matrix

| Deliverable Path (relative to `s23_dm1q\`) | Size | MD5 | Verification |
|---|---:|---|---|
| `wifi_research\qca6490_bringup\s23_pcie0_wlan.asl` | 11,607 B | `9e5948274ece2e1ff25a6546c9bf00fc` | `iasl -tc` 0 Err / 0 Warn |
| `wifi_research\qca6490_bringup\s23_pcie0_wlan.aml` | 618 B | `87e1a8e305e1ad746a640f7cecf223d3` | Generated by `iasl -tc` |
| `wifi_research\qca6490_bringup\s23_pcie0_wlan.hex` | 6,278 B | `4e37b925ed15bd122d04c67437f2cb58` | Compiled into DXE (`-DS23_INCLUDE_SSDT_AML`) |
| `wifi_research\qca6490_bringup\s23_uefi_pcie0_dxe.c` | 45,342 B | `8839291d109e89123cf757a387ba63b7` | `clang-18 -Wall -Wextra -Werror` 4/4 PASS |
| `drivers\s23gpuprobe\s23gpuprobe.c` | 61,341 B | `383e408989ba8e12cfad283c659b09ce` | `v1.0.0.2`, `-Wall -Wextra -Werror` PASS |
| `drivers\s23gpuprobe\verify_pe.py` | 6,575 B | `7c49b4f520664b744033997a5f5e03ab` | Extended for optional extra ref DLLs |
| `drivers\s23gpuprobe\pkg\s23gpuprobe.sys` | 58,248 B | `df47353dcba60967a64a7ff87a409b62` | Signed SHA-256; **10/10 OK, PE PASS** |
| `gpu_research\s23_gpu_bringup\s23_uefi_gpu_dxe.c` | 32,758 B | `ff3992c38f5989309310d77a9f032158` | `clang-18 -Wall -Wextra -Werror` 3/3 PASS |
| `display_research\S23DisplayFixDxe_v2.c` | 15,557 B | `c026e53707ff9147b6652507f4e9073b` | `Mu_Basecore` `-Wall -Wextra -Werror` 2/2 PASS |
| `display_research\s23_display_autorefresh_fix_v2.patch` | 18,266 B | `a92c479607f7b54e7c96d432f9d81e84` | `patch --dry-run -p1` PASS |
| `drivers\s23perf\s23perf.c` | 28,147 B | `10b18e4d35c302f00d779553ce7a9bcd` | `-Wall -Wextra -Werror` PASS |
| `drivers\s23perf\build_wsl.py` | 2,386 B | `3ba86d23b0740819ef743fbd3469f0a4` | ARM64 WDM build + CheckSum fixup |
| `drivers\s23perf\sign.ps1` | 1,337 B | `ab8b5d46cbb6bd616289136a8cdce87e` | `CN=S23TouchTest2020` SHA-256 |
| `drivers\s23perf\pkg\s23perf.sys` | 37,256 B | `c428451b0ecd69ccc147754838b5dd7e` | Signed SHA-256; **10/10 OK, PE PASS** |
| `drivers\s23perf\pkg\s23test.cer` | 778 B | `60c63680ee9fa82b580a25b225d4c996` | Public test-signing certificate |
| `drivers\s23perf\pkg\install_s23perf.ps1` | 1,790 B | `13406a578181dba14236beff5bfea499` | Demand-start installer (`Mode=0`) |
| `drivers\s23keys\s23keys.c` | 44,701 B | `f7ed86c32970bfef07ee59918a7d807c` | `-Wall -Wextra -Werror` PASS |
| `drivers\s23keys\s23keys.inf` | 1,097 B | `38a951419870a09a0d705fb8927f375e` | `ROOT\S23KEYS` ARM64 HID minidriver |
| `drivers\s23keys\build_wsl.py` | 3,035 B | `bfa62e973311bea8bf4f579193f35c75` | ARM64 WDM + `hidclass.lib` build |
| `drivers\s23keys\sign_and_cat.ps1` | 5,360 B | `e35f435cdebe72ef2dbea4e9e314f212` | Catalog + Authenticode signing |
| `drivers\s23keys\pkg\s23keys.sys` | 14,216 B | `6d5d057af458936c530bcce9055427f8` | Signed SHA-256; **24+1/25 OK, PE PASS** |
| `drivers\s23keys\pkg\s23keys.inf` | 1,097 B | `38a951419870a09a0d705fb8927f375e` | Matches catalog hash |
| `drivers\s23keys\pkg\s23keys.cat` | 2,413 B | `415809241556c70d54c65631326b326b` | `Test-FileCatalog`: **Valid (SHA-256)** |
| `drivers\s23keys\pkg\s23test.cer` | 778 B | `60c63680ee9fa82b580a25b225d4c996` | Public test-signing certificate |
| `drivers\s23keys\pkg\install_s23keys.ps1` | 4,004 B | `d5feb57c35076b727da836b21092930c` | `pnputil` + `ROOT\S23KEYS` installer |
| `bt_research\S23_QCA6490_BT_BRINGUP.md` | 12,079 B | `06f7f4c9b646d15e7a4c2b72b3e2d396` | Complete QCA6490 BT 5.3 blueprint |
| `bt_research\s23_bt_uart14.asl` | 5,064 B | `1eb6e31331e8808a5bdcabe689ccaa5f` | `iasl -tc` 0 Err / 0 Warn (`_STA=0`) |
| `bt_research\s23_bt_uart14.aml` | 705 B | `9a21da16482ef8c001192849f61f982d` | Generated by `iasl -tc` |
| `bt_research\s23_bt_uart14.hex` | 7,099 B | `eb04964a12a44b14bda10a8e3a066ebe` | C header array for UEFI DXE |
| `drivers\s23btprobe\s23btprobe.c` | 55,351 B | `37573551e6a92b04fdeb2fbb143afd8a` | `-Wall -Wextra -Werror` PASS |
| `drivers\s23btprobe\build_wsl.py` | 2,404 B | `40f391a92e86c74770aea5afd99b9fb1` | ARM64 WDM build + CheckSum fixup |
| `drivers\s23btprobe\sign.ps1` | 1,352 B | `5113dda1f644bffbf61eb185b75815cd` | `CN=S23TouchTest2020` SHA-256 |
| `drivers\s23btprobe\pkg\s23btprobe.sys` | 45,448 B | `24d14c822a260538995e8e851b586de3` | Signed SHA-256; **10/10 OK, PE PASS** |
| `drivers\s23btprobe\pkg\s23test.cer` | 778 B | `60c63680ee9fa82b580a25b225d4c996` | Public test-signing certificate |
| `drivers\s23btprobe\pkg\install_s23btprobe.ps1` | 2,157 B | `a49bd984f2ae13b8f26a27741852ae93` | Demand-start installer (`StageMask=0x01`) |

---

## PE Verification Summary (All 4 Signed ARM64 `.sys` Drivers)

| Driver | Machine / Magic | `TSAWARE` | `.reloc` | `AuthSig` | PE CheckSum | Import Check | Result |
|---|---|---|---:|---:|---|---|---|
| `s23gpuprobe.sys` (`v1.0.0.2`) | `0xAA64` / `0x020B` | `False` | `0x11000+32` | `0xDE00+1416` | `0x0001E1E5` (match) | `10/10 ntoskrnl_phone.exe`, `missing=[]` | **PASS** |
| `s23perf.sys` (`v1.0.0.1`) | `0xAA64` / `0x020B` | `False` | `0xC000+28` | `0x8C00+1416` | `0x00013AA4` (match) | `10/10 ntoskrnl_phone.exe`, `missing=[]` | **PASS** |
| `s23keys.sys` (`v1.0.0.1`) | `0xAA64` / `0x020B` | `False` | `0x6000+12` | `0x3200+1416` | `0x000116E2` (match) | `24/24 ntoskrnl_phone.exe` + `1/1 hidclass.sys`, `missing=[]` | **PASS** |
| `s23btprobe.sys` (`v1.0.0.1`) | `0xAA64` / `0x020B` | `False` | `0x1D000+44` | `0xAC00+1416` | `0x00017597` (match) | `10/10 ntoskrnl_phone.exe`, `missing=[]` | **PASS** |

---

## Ready-to-Run Live Validation Commands (When Ready to Deploy on S23 Windows)

Copy each `pkg\` folder to S23 Windows and run elevated PowerShell:

1. **Adreno 740 GPU Probe (`v1.0.0.2`):**
   ```powershell
   # Install updated signed pkg\s23gpuprobe.sys, run read-only Stage 1 first, then Stage 2/3
   Copy-Item .\s23gpuprobe.sys C:\Windows\System32\drivers\s23gpuprobe.sys -Force
   sc.exe start s23gpuprobe ; sc.exe stop s23gpuprobe ; Get-Content C:\Users\Public\s23gpuprobe.log
   ```
2. **EPSS CPUfreq + 41-Sensor TSENS v2 (`s23perf`):**
   ```powershell
   powershell -ExecutionPolicy Bypass -File .\install_s23perf.ps1
   sc.exe start s23perf ; sc.exe stop s23perf ; Get-Content C:\Users\Public\s23perf.log
   # Opt-in thermal-guarded vote (<75 C):
   reg add HKLM\SYSTEM\CurrentControlSet\Services\s23perf\Parameters /v Mode /t REG_DWORD /d 1 /f
   sc.exe start s23perf ; sc.exe stop s23perf ; Get-Content C:\Users\Public\s23perf.log
   ```
3. **Physical Power & Volume Buttons HID Minidriver (`s23keys`):**
   ```powershell
   powershell -ExecutionPolicy Bypass -File .\install_s23keys.ps1
   # Press VolUp / VolDown (OS volume OSD) or Power, then inspect log:
   Get-Content C:\Users\Public\s23keys.log
   ```
4. **QCA6490 Bluetooth UART14 Probe (`s23btprobe`):**
   ```powershell
   powershell -ExecutionPolicy Bypass -File .\install_s23btprobe.ps1
   # Step 1 (100% read-only):
   sc.exe start s23btprobe ; sc.exe stop s23btprobe ; Get-Content C:\Users\Public\s23btprobe.log
   # Step 2 (UART14 115200 8N1 FIFO + HCI_Reset + QCA EDL probe; use /d 7 if Wi-Fi rails are not yet up):
   reg add HKLM\SYSTEM\CurrentControlSet\Services\s23btprobe\Parameters /v StageMask /t REG_DWORD /d 7 /f
   sc.exe start s23btprobe ; sc.exe stop s23btprobe ; Get-Content C:\Users\Public\s23btprobe.log
   ```
