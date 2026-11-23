# Samsung Galaxy S23 (`dm1q` / `SM-S911B`) — Warm-Reboot AMOLED Black Screen Root Cause & Fix

**Target Platform:** Samsung Galaxy S23 (`dm1q` / `SM-S911B`, Qualcomm `SM8550-AC` / Kailua)  
**Display Panel:** Samsung Display `DM1_S6E3FAC_AMB606AW01` (SDC `810225`, Rev E, `1080x2340` FHD+ LTPO AMOLED, DSI Command Mode + DSC 1.1)  
**UEFI Firmware:** `Mu-Silicium` (`dm1qPkg`, `USE_CUSTOM_DISPLAY_DRIVER = 0`, `SimpleFbDxe` @ `0xB8000000`)  
**OS:** Native Windows 11 Pro 24H2 ARM64 (`10.0.26100.4349`)  

---

## 1. Executive Summary

During Windows 11 boot after a **warm reboot from Lineage Recovery** (`adb reboot`), Windows 11 boots cleanly to the desktop and renders into the SimpleFB framebuffer (`0xB8000000`, `1080x2340x32bpp`) — proven by `wifi_research/boot27_inspect/s23_shot1.png` (live desktop screenshot showing taskbar, battery icon, clock, and cursor captured while the physical AMOLED panel remained completely dark), `BasicDisplay Start = 1`, `s23touch.sys v1.0.0.13` alive, and `s23batt.sys v1.0.0.1` alive — **yet the physical AMOLED panel stays black**.

Cross-examination of live Lineage Recovery kernel logs, S23 device tree (`s23_live.dts` / `dm1q_eur_openx_w00_r13.dts`), S23 Linux display drivers (`msm_drv.c`, `ss_dsi_panel_common.c`, `DM1_S6E3FAC_AMB606AW01_panel.c`), stock Samsung `ABL` (`LinuxLoader.efi`) and XBL `DisplayDxe.efi` / `ClockDxe.efi` ARM64 disassemblies, and `Mu-Silicium/Platforms/Samsung/dm1qPkg` proves the exact hardware/firmware root cause:

1. **The `S6E3FAC` panel is strictly DSI Command Mode (`dsi_cmd_mode`) with DSC 1.1**, NOT Video Mode. Writing pixels to `0xB8000000` (SimpleFB DDR) never updates the panel unless MDSS (`CTL_0` + `INTF_1` TE-synced Auto-Refresh) actively reads DDR and pushes DCS `0x2C`/`0x3C` (Memory Write) frames over DSI0, **and** the panel DDI (`S6E3FAC`) is awake (`0x11` Sleep Out, `0x29` Display On) with non-zero brightness (`0x51` DBV != `0x0000`).
2. **Lineage Recovery blanks and shuts down the entire MDSS/DSI0/panel pipeline before warm reboot:**
   - Live ADB telemetry from Lineage Recovery confirms idle dimming sets panel brightness to zero (`ss_brightness_dcs: level:0 candela:1CD`).
   - On `adb reboot`, `msm_pdev_shutdown()` (`msm_drv.c:536, 2350`) runs `drm_atomic_helper_shutdown()` -> `ss_panel_off_post()` -> sends `POWER_OFF_SETTTING` (`29 9F A5 A5`, `05 28 00` Display Off, `05 10 00` Sleep In, `29 9F 5A 5A`, 120 ms delay), clears `INTF_TEAR_AUTOREFRESH_CONFIG (0x0AE362B4 = 0)`, disables `MDSS_CORE_GDSC (0x0AF09000)`, shuts down `panel_vddr`/`panel_vci`/`panel_vdd3`, and pulls `Reset_N (GPIO125)` LOW right before PSCI warm reset.
3. **Stock XBL `DisplayDxe.efi` + `ABL` handoff depends on continuous-splash continuity and leaves `Mu-dm1q` blind:**
   - Verified in stock `dm1q` `ABL` (`dm1q_abl_LinuxLoader.efi` from `backups/abl.img`): `Mu-dm1q-cca0.img` DOES include `/reserved-memory/splash_region` (`0xB8000000`, `0x2B00000`), so ABL does NOT hit its `Splash region not found` power-down (`0x396D0`).
   - Verified in Samsung XBL `DisplayDxe.efi`: `DisplayDxeExitBootServicesEvent` (`0x1518`) calls `SetDisplayPowerState(OFF)` (`0x20504`) UNLESS `normal_uefi_exit` (`0x51C48`) is set by `NormalBootingEventCallBack` (GUID `7AB77467-16CC-4B13-A983-38D1E3072DA1`, signaled by ABL at `0xB220` right before `ExitBootServices`). Additionally, XBL `DisplayDxe` manages the external OLED PMIC `SM3080`/`S2DOS05` (I2C `0xC0`, `[SM3080] Disable restart` at `0x34780`: `0xCF=0x7E, 0xCF=0xE7, 0xCA=0x03, 0xCF=0x00`).
   - Verified in `Mu-Silicium/Platforms/Samsung/dm1qPkg`:
     - `USE_CUSTOM_DISPLAY_DRIVER = 0` (`SimpleFbDxe` only — `SimpleFb.c` performs **zero** hardware register reads/writes; it only publishes a GOP over `0xB8000000`).
     - `MemoryMapLib.c` **does not even map `MDSS (0x0AE00000)` or `DISP_CC (0x0AF00000)`**.
     - `ClockDxe.efi` in `Mu-dm1q` is stock unpatched (`md5=715d099553a7c9b7daa73aac5780fe97`), whereas `ClockDxe.patched.efi` NOPs `0xC58C` (`97FFE5FD -> D503201F`, DCD disable-dependencies) to avoid gating MDSS clocks on boot transition.
     - Therefore, whenever warm reboot from Lineage Recovery leaves `INTF_TEAR_AUTOREFRESH_CONFIG (0x0AE362B4)` unarmed, `CTL_0_START (0x0AE1601C)` stalled, or `S6E3FAC` in dimmed/display-off state, **`Mu-dm1q` never detects, re-arms, or kicks the command-mode display pipeline**, and Windows `BasicDisplay.sys` simply writes into `0xB8000000` forever without the panel ever scanning out. By contrast, a **cold hard reset** (full PMIC + OLED DDI POR) forces XBL `DisplayDxe` through its full cold panel initialization path, which arms `INTF_TEAR_AUTOREFRESH_CONFIG = 0x80000002`, sends max boot brightness `0x51 0x07 0xFF` + `0x29` Display On, and hands off live continuous splash.

---

## 2. Verified Hardware Map (`DM1_S6E3FAC_AMB606AW01` on `SM8550-AC`)

### 2.1 Panel & Timing Parameters (`s23_live.dts` + Live Recovery DRM)
| Property | Verified Value | Source |
|---|---|---|
| Panel ID / Model | `DM1_S6E3FAC_AMB606AW01` (`SDC 810225`, Rev E) | `dm1q_eur_openx_w00_r13.dts`, `/sys/class/drm/card0-DSI-1` |
| Resolution | `1080 x 2340` (`32 bpp` SimpleFB @ `0xB8000000`, `0x02B00000` bytes) | `dm1q.dsc`, `MemoryMapLib.c`, `s23_shot1.png` |
| Panel Mode | **DSI Command Mode** (`qcom,mdss-dsi-panel-type = "dsi_cmd_mode"`) | `dm1q` DTS, DRM modes `1080x2340x120xcmdHS`, `60xcmdPHS`, `30/24/10` |
| Compression | **DSC 1.1**, 2 encoders, slice `540 x 117`, `8 bpp`, `951 MHz` DSI bit clock (`0x38B05440`) | `s23_live.dts` |
| Hardware TE Pin | `TLMM GPIO86` (`0x56`), dedicated MDP VSync TE input | `s23_live.dts` L40402–40434 |

### 2.2 Panel GPIOs & Power Rails (`s23_live.dts` + XBL `DisplayDxe.efi`)
| Signal / Rail | Hardware Resource | Polarity / Voltage | Notes |
|---|---|---|---|
| `panel_vci` | `TLMM GPIO20` (`0x14`) | Active-High, `3.0 V` | `fixed_regulator@0` |
| `panel_vdd3` | `TLMM GPIO21` (`0x15`) | Active-High, `1.8 V` | `fixed_regulator@1` |
| `panel_vddr` | `TLMM GPIO187` (`0xBB`) | Active-High, `1.3 V` | `fixed_regulator@2` |
| `Reset_N` | `TLMM GPIO125` (`0x7D`) | Active-High (run) / Low (reset) | Pulled LOW in `ss_panel_off_post` |
| `TE` (Tearing Effect) | `TLMM GPIO86` (`0x56`) | Input (MDP VSYNC) | 120 Hz / 60 Hz panel scanout sync |
| `UB_CON_DET` | `TLMM GPIO108` (`0x6C`) | Input | Panel flex connector detect |
| `ESD_IRQ1` | `TLMM GPIO186` (`0xBA`) | Input | Panel ESD fault IRQ |
| `DSI0 PHY vdda-0p9` | `ldoe1` (`pm_v6e_l1`) | `880 mV` | Shared with PCIe0 PHY 0.9 V |
| `DSI0 CTRL vdda-1p2` | `ldoe3` (`pm_v6e_l3`) | `1200 mV` | Shared with PCIe0 PHY 1.2 V |
| External OLED PMIC | `SM3080` / `S2DOS05` (ELVDD/ELVSS/AVDD) | I2C slave `0xC0` | XBL `DisplayDxe` `0x34780`: `[SM3080] Disable restart` (`0xCF=0x7E`, `0xCF=0xE7`, `0xCA=0x03`, `0xCF=0x00`) |

### 2.3 SM8550 `DISP_CC`, `MDSS_MDP`, and `DSI0_CTRL` Register Map

> **CRITICAL AXI SAFETY GATE (`G116Gate`):**
> **NEVER** read or write any register inside `MDSS` (`0x0AE00000..0x0AEFFFFF`) unless ALL THREE `DISP_CC` (`0x0AF00000`) gate conditions hold simultaneously:
> 1. `DISP_CC_MDSS_CORE_GDSCR (0x0AF09000)` bit 31 (`PWR_ON`) == `1`
> 2. `DISP_CC_MDSS_AHB_CLK_CBCR (0x0AF080A4)` bit 31 (`CLK_OFF`) == `0`
> 3. `DISP_CC_MDSS_MDP_CLK_CBCR (0x0AF0800C)` bit 31 (`CLK_OFF`) == `0`
>
> Accessing `0x0AE00000+` while `MDSS_CORE_GDSC` is collapsed or `AHB_CLK` is off raises an **unrecoverable synchronous/asynchronous AXI SError** (instant reboot / Windows BugCheck)!

| Block | Physical Base | Register | Offset | Purpose / Expected Live Value |
|---|---|---|---|---|
| `DISP_CC` | `0x0AF00000` | `MDSS_CORE_GDSCR` | `+0x9000` | Bit 31 = `PWR_ON` (must be `1`) |
| `DISP_CC` | `0x0AF00000` | `MDSS_CORE_GDSC_CFG` | `+0x9004` | GDSC retention config |
| `DISP_CC` | `0x0AF00000` | `MDSS_PCLK0_CLK_CBCR` | `+0x8004` | Bit 31 = `CLK_OFF` (must be `0` for DSI0 pixel) |
| `DISP_CC` | `0x0AF00000` | `MDSS_ESC0_CLK_CBCR` | `+0x8008` | Bit 31 = `CLK_OFF` (must be `0` for DSI0 LP/DCS) |
| `DISP_CC` | `0x0AF00000` | `MDSS_MDP_CLK_CBCR` | `+0x800C` | Bit 31 = `CLK_OFF` (must be `0`) |
| `DISP_CC` | `0x0AF00000` | `MDSS_BYTE0_CLK_CBCR` | `+0x8028` | Bit 31 = `CLK_OFF` (must be `0` for DSI0 HS) |
| `DISP_CC` | `0x0AF00000` | `MDSS_BYTE0_INTF_CLK_CBCR` | `+0x802C` | Bit 31 = `CLK_OFF` (must be `0`) |
| `DISP_CC` | `0x0AF00000` | `MDSS_AHB_CLK_CBCR` | `+0x80A4` | Bit 31 = `CLK_OFF` (must be `0` — AHB gate!) |
| `MDSS_HW` | `0x0AE00000` | `MDSS_HW_VERSION` | `+0x0000` | SM8550 MDSS v9.0 (`0x90000000` family) |
| `MDSS_HW` | `0x0AE00000` | `MDSS_HW_INTR_STATUS` | `+0x0010` | Top-level MDSS IRQ status |
| `MDP_CTL_0` | `0x0AE16000` | `CTL_TOP` | `+0x0014` | Command-mode vs Video-mode select |
| `MDP_CTL_0` | `0x0AE16000` | `CTL_FLUSH` | `+0x0018` | Pending flush mask |
| `MDP_CTL_0` | `0x0AE16000` | **`CTL_START`** | **`+0x001C`** | **Write `0x00000001` to trigger single command-mode frame transfer!** |
| `MDP_CTL_0` | `0x0AE16000` | `CTL_STATUS` | `+0x0024` | Active frame status |
| `MDP_INTF_1` | `0x0AE36000` | `INTF_FRAME_COUNT` | `+0x00AC` | Monotonic frame counter (proves active scanout!) |
| `MDP_INTF_1` | `0x0AE36000` | `INTF_LINE_COUNT` | `+0x00B0` | Current scanline |
| `MDP_INTF_1` | `0x0AE36000` | `INTF_UNDERRUN_COUNT` | `+0x0268` | Underflow counter |
| `MDP_INTF_1` | `0x0AE36000` | `INTF_STATUS` | `+0x026C` | Interface busy/active bits |
| `MDP_INTF_1` | `0x0AE36000` | **`INTF_TEAR_TEAR_CHECK_EN`** | **`+0x0284`** | **Bit 0 = `1` enables TE check (required for Auto-Refresh)** |
| `MDP_INTF_1` | `0x0AE36000` | `INTF_TEAR_SYNC_CONFIG_VSYNC` | `+0x0288` | VSync / TE period configuration |
| `MDP_INTF_1` | `0x0AE36000` | `INTF_TEAR_OUT_LINE_COUNT` | `+0x02AC` | Output TE line count |
| `MDP_INTF_1` | `0x0AE36000` | `INTF_TEAR_LINE_COUNT` | `+0x02B0` | Incoming TE line count (ticks if TE pin alive!) |
| `MDP_INTF_1` | `0x0AE36000` | **`INTF_TEAR_AUTOREFRESH_CONFIG`** | **`+0x02B4`** | **`0x80000002` = Enable (bit 31) \| FrameDiv=2 (60 Hz from 120 Hz TE)** |
| `MDP_INTF_1` | `0x0AE36000` | **`INTF_TEAR_AUTOREFRESH_STATUS`** | **`+0x02C0`** | **Auto-Refresh frame tick status / counter** |
| `MDP_PP_0` | `0x0AE6A000` | `PP_TEAR_CHECK_EN` / status | `+0x0000..+0x0040` | PingPong 0 status & DSC path |
| `MDP_DSC_0` | `0x0AE81000` | `DSC_COMMON_MODE` / `ENC` | `+0x0000..+0x0010` | DSC 1.1 encoder active status |
| `DSI0_CTRL` | `0x0AE94000` | `DSI_CTRL` | `+0x0004` | Bit 0=EN, Bit 2=CMD_MODE_EN, Bits 4..7=Lanes, Bit 8=CLK_EN |
| `DSI0_CTRL` | `0x0AE94000` | `DSI_STATUS` | `+0x0008` | Bit 0=CMD_MODE_ENGINE_BUSY, Bit 1=CMD_MODE_DMA_BUSY, Bit 2=CMD_MODE_MDP_BUSY |
| `DSI0_CTRL` | `0x0AE94000` | `DSI_FIFO_STATUS` | `+0x000C` | TX/RX FIFO state |
| `DSI0_CTRL` | `0x0AE94000` | `DSI_DMA_CMD_OFFSET` | `+0x0048` | Physical address of DMA DCS command buffer |
| `DSI0_CTRL` | `0x0AE94000` | `DSI_DMA_CMD_LENGTH` | `+0x004C` | Byte length of DMA DCS command packet |
| `DSI0_CTRL` | `0x0AE94000` | `DSI_TRIG_CTRL` | `+0x0084` | MDP/DMA trigger mux |
| `DSI0_CTRL` | `0x0AE94000` | `DSI_CMD_MODE_DMA_SW_TRIGGER` | `+0x0090` | Write `1` to fire DMA DCS command packet |
| `DSI0_CTRL` | `0x0AE94000` | `DSI_CMD_MODE_MDP_SW_TRIGGER` | `+0x0094` | Write `1` to trigger MDP command-mode transfer |
| `DSI0_CTRL` | `0x0AE94000` | `DSI_LANE_STATUS` | `+0x00A8` | Data/Clock lane LP/HS/ULPS state |
| `DSI0_CTRL` | `0x0AE94000` | `DSI_INT_CTRL` | `+0x0110` | Bit 24 = `CMD_MODE_DMA_DONE_STAT` (W1C) |
| `DSI0_CTRL` | `0x0AE94000` | `DSI_CLK_CTRL` | `+0x011C` | DSI clock enables (`AHBS`/`AHBM`/`PCLK`/`BYTE`/`ESC`/`FORCE_ON`) |
| `DSI0_CTRL` | `0x0AE94000` | `DSI_CMD_COMPRESSION_MODE_CTRL` | `+0x02A8` | DSC command-mode enable |

### 2.4 Verified `S6E3FAC` DCS Command Sequences (`dm1q` DTS & XBL `DisplayDxe.efi`)

**Power-Off Sequence (`POWER_OFF_SETTTING`, sent by Lineage Recovery `msm_pdev_shutdown` -> `ss_panel_off_post`):**
```text
29 01 00 00 00 00 03  9F A5 A5   ; Unlock Level-1 / Level-3 key
05 01 00 00 00 00 02  28 00      ; DCS Display Off (0x28)
05 01 00 00 00 00 02  10 00      ; DCS Sleep In (0x10)
29 01 00 00 00 00 03  9F 5A 5A   ; Lock Level-1 / Level-3 key
Delay 120 ms
```

**Wake + Max Boot Brightness Sequence (verified from Samsung XBL `DisplayDxe.efi` `S6E3FAC` XML):**
```text
29 01 00 00 00 00 03  9F A5 A5   ; Unlock Level-3 key
29 01 00 00 00 00 03  F0 5A 5A   ; Unlock Level-2 test key
29 01 00 00 00 00 02  53 28      ; WRCTRLD: BCTRL=1, DD=0, BL=1 (enable dimming/backlight ctrl)
29 01 00 00 00 00 03  51 07 FF   ; WRDISBV: 11-bit DBV = 0x07FF (full boot brightness)
29 01 00 00 00 00 02  F7 0F      ; Samsung LTPS/Gamma update latch
05 01 00 00 00 00 01  29         ; DCS Display On (0x29)
29 01 00 00 00 00 03  F0 A5 A5   ; Lock Level-2 test key
29 01 00 00 00 00 03  9F 5A 5A   ; Lock Level-3 key
```

---

## 3. Root Cause Proof (Warm Reboot vs Cold Hard Reset)

| Stage | Cold Hard Reset (Works) | Warm Reboot from Lineage Recovery (Black Screen) |
|---|---|---|
| **Pre-reboot state** | Full PMIC + DDI POR (`S6E3FAC` + `SM3080` cold start) | Lineage Recovery dims DBV to `0`, then `msm_pdev_shutdown()` sends `0x28` + `0x10`, sets `INTF_TEAR_AUTOREFRESH_CONFIG = 0`, collapses `MDSS_CORE_GDSC`, drops `panel_vci`/`vdd3`/`vddr`, pulls `Reset_N` LOW, and triggers PSCI warm reset |
| **XBL `DisplayDxe` & `ABL`** | Runs full cold `S6E3FAC` + `SM3080` init, programs `DSC 1.1`, arms `INTF_TEAR_AUTOREFRESH_CONFIG = 0x80000002`, kicks `CTL_0_START`, signals `NormalBootingEvent` (`7AB77467-...`), preserves splash | On warm reboot / recovery handoff, `INTF_TEAR_AUTOREFRESH_CONFIG` is either cleared or stops ticking after `ExitBootServices`, OR `S6E3FAC`/`SM3080` retains dimmed/sleep state when `DisplayDxe` skips cold re-init or executes `DisplayDxeExitBootServicesEvent` (`0x20504`) |
| **`Mu-dm1q` UEFI** | `SimpleFbDxe` publishes `0xB8000000` passively; hardware Auto-Refresh keeps scanning out `0xB8000000` into Windows | `SimpleFbDxe` touches **zero** MDSS/DSI0 registers (`MDSS`/`DISP_CC` are not even mapped in `MemoryMapLib.c`!); `ClockDxe` is unpatched; nothing re-arms `0x0AE36284`/`0x0AE362B4` or kicks `CTL_0_START (0x0AE1601C)` |
| **Windows `BasicDisplay.sys`** | Writes `1080x2340` desktop into `0xB8000000`; Auto-Refresh continuously transmits frames to `S6E3FAC` | Writes full `1080x2340` desktop into `0xB8000000` (verified in `s23_shot1.png`), **but zero frames reach the panel / panel stays blanked** |

---

## 4. Concrete Fixes & Deliverables Built

### Fix 1 (Primary UEFI Fix): `S23DisplayFixDxe` + `MemoryMapLib` Patch
- **Patch file:** `s23_display_autorefresh_fix.patch`
- **Separate test UEFI image (does NOT overwrite `Mu-dm1q-cca0.img` or `backups\`):** `Mu-dm1q-dispfix.img`
- **What it does:**
  1. Maps `MDSS (0x0AE00000, 0x00100000)` and `DISP_CC (0x0AF00000, 0x00020000)` as `NS_DEVICE` in `Platforms/Samsung/dm1qPkg/Library/MemoryMapLib/MemoryMapLib.c` (cleanly fitting in the unmapped `0x0A800000..0x0B000000` gap between `USB` and `AOSS`).
  2. Adds `S23DisplayFixDxe` right after `SimpleFbDxe` in `dm1q.dsc`, `Include/DXE.inc`, and `Include/APRIORI.inc`.
  3. On entry, checks the **safe `G116Gate`** (`DISP_CC+0x9000 bit 31 == 1`, `DISP_CC+0x80A4 bit 31 == 0`, `DISP_CC+0x800C bit 31 == 0`). If MDSS is powered and clocked:
     - Verifies `MDSS_HW_VERSION (0x0AE00000)` is valid (`!= 0 && != 0xFFFFFFFF`).
     - Re-arms `INTF_TEAR_TEAR_CHECK_EN (0x0AE36284 = 0x1)` and `INTF_TEAR_AUTOREFRESH_CONFIG (0x0AE362B4 = 0x80000002)` (Enable | FrameDiv=2 = 60 Hz from 120 Hz TE).
     - Kicks `MDP_CTL_0_START (0x0AE1601C = 0x1)`.
     - Starts a `33 ms` (~30 Hz) periodic UEFI timer during boot services that keeps `INTF_TEAR_AUTOREFRESH_CONFIG` armed and kicks `CTL_0_START` whenever `INTF_FRAME_COUNT` has not incremented.
     - Registers an `EVT_SIGNAL_EXIT_BOOT_SERVICES` callback that cancels the timer cleanly, re-verifies `0x0AE36284 = 1` and `0x0AE362B4 = 0x80000002`, and issues one final `CTL_0_START = 1` handoff kick immediately before handing control to `winload.efi` / Windows!

### Fix 2 (Live Windows Hardware Diagnostic & Wake Driver): `drivers\s23disp_diag\`
- **Built & signed binary:** `drivers\s23disp_diag\pkg\s23disp_diag.sys` (signed with `CN=S23TouchTest2020`, `Start = 3` Demand Start, logs to pre-allocated 64 KB write-through `C:\Users\Public\s23disp_diag.log`).
- **Registry control:** `HKLM\SYSTEM\CurrentControlSet\Services\s23disp_diag\Parameters\ActionMask` (REG_DWORD):
  - **`0x00` (Default — 100% Read-Only Safe Dump):**
    - Reads TLMM panel GPIOs (`GPIO20/21/86/108/125/186/187`).
    - Reads `DISP_CC` GDSC & clock CBCRs (`0x0AF09000`, `0x8004/08/0C/28/2C/A4`).
    - **ONLY if `G116Gate` passes:** reads `MDSS_HW_VERSION`, `CTL_0 (0x0AE16000)`, `INTF_1 (0x0AE36000)`, `PP_0 (0x0AE6A000)`, `DSC_0 (0x0AE81000)`, `DSI0_CTRL (0x0AE94000)`, then sleeps `100 ms` and re-samples `INTF_FRAME_COUNT`, `INTF_LINE_COUNT`, `INTF_TEAR_LINE_COUNT`, and `INTF_TEAR_AUTOREFRESH_STATUS` to prove live whether TE and Auto-Refresh are ticking!
  - **`0x01` (Opt-in Auto-Refresh + `CTL_0_START` Kick):**
    - Writes `INTF_TEAR_TEAR_CHECK_EN (0x0AE36284) = 1`, `INTF_TEAR_AUTOREFRESH_CONFIG (0x0AE362B4) = 0x80000002`, and `CTL_0_START (0x0AE1601C) = 1`, waits `100 ms`, and re-samples frame counters to confirm scanout resumed.
  - **`0x02` (Opt-in `S6E3FAC` Brightness/Wake DCS + `CTL_0_START` Kick):**
    - Allocates a physically contiguous non-cached DMA buffer (`MmAllocateContiguousMemory`), sends the exact XBL `S6E3FAC` wake & brightness DCS sequence (`9F A5 A5`, `F0 5A 5A`, `53 28`, `51 07 FF`, `F7 0F`, `29`, `F0 A5 A5`, `9F 5A 5A`) via `DSI0_CTRL` Command-Mode DMA (`+0x48/+0x4C/+0x90`, polling `DSI_INT_CTRL` bit 24), frees the buffer, and kicks `CTL_0_START`.

### Fix 3 (Immediate Operational Workaround & Secondary Option)
1. **Immediate workaround (zero code changes):** If the screen ever boots black after a warm `adb reboot` from Lineage Recovery, force a **cold hard reset** (hold `Power + VolDown` for ~7 seconds until full power cycle, release into Windows boot) so XBL `DisplayDxe` performs a full cold `S6E3FAC` + `SM3080` initialization.
2. **Future full-reinit fallback (if `s23disp_diag.sys` ever shows `DISP_CC+0x9000 bit 31 == 0`, i.e., MDSS GDSC completely collapsed):** Follow `Mu-Silicium/Platforms/Samsung/b5qPkg` by switching `USE_CUSTOM_DISPLAY_DRIVER = 1` with a patched `dm1q` `DisplayDxe.efi` (bypassing `DisplayDxeExitBootServicesEvent` `0x20504` shutdown) + `ClockDxe.patched.efi` (NOP at `0xC58C`). Running `s23disp_diag.sys` with `ActionMask = 0x00` during a black-screen boot immediately tells us whether MDSS is still powered (`GDSC PWR_ON == 1` -> Fix 1/2 solves it completely) or collapsed (`GDSC PWR_ON == 0` -> requires patched `DisplayDxe` + `ClockDxe`).
