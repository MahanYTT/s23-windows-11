# NEON — Samsung Galaxy S23 (`dm1q` / `SM-S911B`, `SM8550-AC`) Adreno 740 GPU & X70 5G Modem Bring-Up Master Report

**Date:** 2026-11-21  
**Target:** Samsung Galaxy S23 (`dm1q` / `SM-S911B`, `SM8550-AC`), Windows 11 Pro 24H2 ARM64 (`10.0.26100.4349`)  
**Execution Mode:** 100% Offline & Strictly Isolated (`gpu_research\`, `drivers\s23gpuprobe\`, `modem_research\` ONLY; ZERO `adb`/`fastboot`/`s23link.py` calls; ZERO writes outside the 3 allowed directories; PC not shut down).

---

## 1. Executive Summary

### Workstream A — Adreno 740 (`SM8550-AC`, `GEN7_2_1`, 719 MHz) Native GPU Bring-Up Package
- **Silicon & Path Verdict (`gpu_research\S23_ADRENO740_ARCHITECTURE.md`):**
  - S23 (`SM8550-AC`) runs Adreno 740 `qcom,adreno-gpu-gen7-2-1` (`0x07020100`, 3 MiB GMEM, UBWC 4.0, `highest_bank_bit = 16`, `pwrlevels-0` top bin **719 MHz @ `0xE0`** vs standard SM8550 **680 MHz @ `0xC0`**) with live S23 `cmd-db` ARC rails `cx.lvl (0x30000)`, `mx.lvl (0x30010)`, **`gfx.lvl (0x30050)`**, and **`mxc.lvl (0x300A0)`**.
  - **Path 2 (Qualcomm `qcdx` / `windows_silicon_qcom_kailua`) is definitively blocked:** `windows_silicon_qcom_kailua` ships zero `qcdx`/`qcpep` binaries, and shipping Windows Gen7 `qcdx` (`G142`/`G143`) hard-rejects `SM8550` (`family = 127 / 0x7F`, accepting only `0x88` Hamoa / `0x9A` Purwa) and requires monolithic `QCOM0C36` MDSS+GPU+Video + 10-driver `_DEP` + Windows QTEE TAs.
  - **Path 1 (Open-Source Direct MMIO + RPMh + `G1287`/`G1348`/`G779` Native A740 KMD/WDDM + Mesa `G739`) is the ONLY viable path**, sharing bit-for-bit `gen7-2-1` silicon with OnePlus Open (`SM8550`). Note: `..\device_firmware\` belongs to **Galaxy A52 4G (`SM7125`)**, not S23 — S23's `a740_sqe.fw`, `gmu_gen70200.bin`, and `a740_zap.*` must be pulled from `/vendor/firmware*` once live device access resumes.
- **Compiled, Signed & Verified Safe Probe Driver (`drivers\s23gpuprobe\pkg\s23gpuprobe.sys`):**
  - Demand-Start (`Start = 3`) pure-C ARM64 WDM driver with pre-allocated 64 KB write-through log `C:\Users\Public\s23gpuprobe.log` and safe default `StageMask = 0x01` (read-only telemetry).
  - Enforces a **3-tier AXI SError safety gate** (`GCC_GPU_BCR/CFG_AHB` -> `GPUCC_CX_GDSCR + AHB` -> `GPUCC_GX_GDSCR + GX_CFG`) before touching `GPUCC`, `GMU`, or `KGSL`.
  - Stage `0x02` votes `cx`/`mx`/`mxc`/`gfx.lvl` via `APPS_RSC DRV2 TCS0`, brings up `GCC_GPU`, `CX_GDSC`, `HUB/FF/GMU` RCGs, `PLL0`/`PLL1`, `GX_GDSC`, and reads live `GMU_CX`/`GMU_AO`/`RBBM_STATUS`/`GBIF`/`CP_SQE_CNTL`/`SECVID`. Stage `0x04` runs non-destructive CX (`GMU_GENERAL_0`) & GX (`RBBM_SP_HYST_CNT`/`RAC_THRESH_CNT`) scratch R/W plus a GX power-collapse/recovery retention test.
- **S23 UEFI DXE & ACPI SSDT (`gpu_research\s23_gpu_bringup\`):**
  - `s23_uefi_gpu_dxe.c` + `.inf`: S23-exact port of `G736r2/G290GpuPower.c` (`G721`/`G722`/`G723`) with **dual-mode rail voting** (`EFI_NPA_PROTOCOL` with automatic `APPS_RSC DRV2 TCS0` + `cmd-db` fallback for standalone `Mu-dm1q`), `PLL1` 440 MHz lock, clean broker GMU leaf handoff, `S23G` diagnostic ACPI table, and optional `s23_gpu_acpi.aml` publication.
  - `s23_gpu_acpi.asl`: S23-exact `MMU1` (`OPEN0D01`, `0x03DA0000`, GSIs `705`/`711`/`709`) + `GPU0` (`OPEN0515`, 5 split MMIO banks, GSIs `332`/`336`/`337`, `_CCA=1`, `_S0W=0`, `_DSD` 719 MHz metadata).

### Workstream B — Snapdragon X70 5G Modem & Kailua Core Subsystems
- **Kailua BSP Audit (`modem_research\KAILUA_SUBSYSTEMS_INVENTORY.md`):** Audited all **21 INF packages** (19 drivers + 2 extensions) in `windows_silicon_qcom_kailua`. Discovered both the **critical `_HID` shift** (`QCOM5Exx`/`QCOM05DD` in Kailua BSP INFs vs `QCOM0Cxx`/`QCOM06xx` in `DSDT_MTP.dsl`) and the **0/21 presence** in current `dm1q/DSDT.aml`. Confirmed `qcpep`, `qcglink`, `qcipc`, `qcamss`, `qcipa`, `qcqmi`, `qcrmnet`, `qcmbb`, `qcdx`, `qcaud`, `qcbt`, `qcsensor`, and `qccam` are absent from the public Kailua repo.
- **X70 5G Feasibility & Quick Wins (`modem_research\S23_5G_MODEM_BRINGUP.md` + `s23_modem_subsystems.asl`):**
  - Mapped S23 `remoteproc-mss@04080000` (`PAS ID 4`, `GSI 296`, `mpss_region 0x8A800000/264 MiB`, `mpss_dsm_region 0xD4D00000/51 MiB`), **IPA v5.0 at `0x03E00000`** (NOT `0x01E00000`!, `PAS ID 0x0F`, GSIs `686`/`464`, `apps_smmu` SIDs `0x4A0..0x4A4`), and UIM0/UIM1 pins (`GPIO130-132/27`, `GPIO134-136/26`).
  - Documented all 5 hard blockers (missing Kailua modem/PEP/GLINK drivers, Gunyah/TZ PAS + `hyp_assign`, Samsung `sec_efs`/NV safety, `SanPolicy=4` UFS LUN isolation, carrier MCFG/IMS) and ranked top quick wins (**#1 EPSS CPUfreq `0x17D91000` + TSENS `0x0C271000`**, **#2 QCA6490 Bluetooth over `qupv3_se14_4uart @ 0x00898000` with S23 `hpbtfw21.tlv` ALREADY on disk!**, **#3 signed Kailua `qcscm`/`qcgpio`/`qci2c`/`qcspi`**, **#4 USB-C xHCI host mode**, **#5 LPASS + Cirrus `CS35L43` speakers**).
  - Compiled inert-by-default (`_STA = 0`) S23 ACPI stubs (`s23_modem_subsystems.asl`) with `iasl -tc` (0 Errors, 0 Warnings).

---

## 2. Complete File Index, Sizes, MD5 Checksums & Verification Results

| File (relative to `s23_dm1q\`) | Size | MD5 | Verification Status |
|---|---:|---|---|
| `gpu_research\S23_ADRENO740_ARCHITECTURE.md` | `25,607 B` | `8a49ae5cfbff0fd447e5a14dc4853373` | Complete S23 `SM8550-AC` hardware, Path 1 vs Path 2 proof, FW audit |
| `gpu_research\s23_gpu_bringup\s23_gpu_acpi.asl` | `5,931 B` | `db3aba183691399381409a3407582a48` | `iasl -tc`: **0 Errors, 0 Warnings, 0 Remarks** (1 Optimization) |
| `gpu_research\s23_gpu_bringup\s23_gpu_acpi.aml` | `744 B` | `71282e72063913102098ef9a7776d7ac` | Compiled ACPI SSDT (`S23GPU74`: `MMU1` + `GPU0`) |
| `gpu_research\s23_gpu_bringup\s23_gpu_acpi.hex` | `7,395 B` | `37c38f0d11229f9bbfeb957827359389` | C byte-array header (`s23_gpu_acpi_aml_code[]`) |
| `gpu_research\s23_gpu_bringup\s23_uefi_gpu_dxe.c` | `30,880 B` | `1d119d9a4c73a832747d86bd2dec6861` | `clang-18 -Wall -Wextra -Werror`: **3/3 PASS** (standalone, `Mu_Basecore` EDK2, EDK2 + `-DS23_INCLUDE_GPU_SSDT_AML`) |
| `gpu_research\s23_gpu_bringup\s23_uefi_gpu_dxe.inf` | `1,164 B` | `f2f060a8e88ce257cbeb1865d5576967` | EDK2 `DXE_DRIVER` INF (`S23GpuDxeEntry`) |
| `drivers\s23gpuprobe\s23gpuprobe.c` | `57,169 B` | `5a3501a4ffc7f4c9c174d648fbb0c4d9` | `clang-18 -Wall -Wextra -Werror`: **0 warnings, 0 errors** |
| `drivers\s23gpuprobe\build_wsl.py` | `2,410 B` | `26d2830d802d19b7ccf3630e7c042536` | `python -m py_compile`: **PASS**; executed cleanly (`RC=0`) |
| `drivers\s23gpuprobe\sign.ps1` | `1,357 B` | `2df4e482b281b83602e651c389991ced` | Signed with `CN=S23TouchTest2020` (`05DBD92557F4FC52EF2A5E5955789D8EED637C68`) |
| `drivers\s23gpuprobe\verify_pe.py` | `6,022 B` | `c0402ff9286d570552b5ee448910bf67` | `python -m py_compile`: **PASS**; verified `pkg\s23gpuprobe.sys` |
| `drivers\s23gpuprobe\out\s23gpuprobe.sys` | `53,760 B` | `79bbc8c671205a7ac1de9e5f2469851e` | Unsigned linker output (`Machine=0xAA64`, `Reloc=0x11000+32`) |
| **`drivers\s23gpuprobe\pkg\s23gpuprobe.sys`** | **`55,176 B`** | **`6fd5c9e0bbcaf468eecb5f20f2de8888`** | **PE32+ ARM64 `0xAA64`, `TSAWARE=False`, `Reloc=0x11000+32`, `AuthSig=0xD200+1416`, CheckSum `0x000179AD` match, 10/10 phone `ntoskrnl.exe` (26100.4349, 3,246 exports) imports OK** |
| `drivers\s23gpuprobe\pkg\s23test.cer` | `778 B` | `60c63680ee9fa82b580a25b225d4c996` | Exported `CN=S23TouchTest2020` cert |
| `modem_research\KAILUA_SUBSYSTEMS_INVENTORY.md` | `12,067 B` | `2c33c0aa43dbfed1db9a166740a09fdc` | Complete 21-package Kailua BSP + `DSDT_MTP` vs `dm1q` gap audit |
| `modem_research\S23_5G_MODEM_BRINGUP.md` | `17,460 B` | `3ee81b4917611ee868682e9e65909382` | X70 MPSS/IPA v5.0/SIM map, 5 blockers, phased plan, ranked quick wins |
| `modem_research\s23_modem_subsystems.asl` | `5,769 B` | `1f4fab0c9bb4803d303500813a7521b8` | `iasl -tc`: **0 Errors, 0 Warnings, 0 Remarks** (2 Optimizations) |
| `modem_research\s23_modem_subsystems.aml` | `826 B` | `f7812964a594fedebef853a2ebd09dad` | Compiled inert-by-default (`_STA=0`) Kailua/Modem ACPI SSDT |
| `modem_research\s23_modem_subsystems.hex` | `8,252 B` | `6a8340949ec6481b3dd696a2194e0a30` | C byte-array header (`s23_modem_subsystems_aml_code[]`) |

---

## 3. Live Testing Runbook (Once Display + Wi-Fi Session Completes)

1. **Pre-allocate log & install `s23gpuprobe.sys` (Demand Start, `Start = 3`):**
   - Push signed `drivers\s23gpuprobe\pkg\s23gpuprobe.sys` to `C:\Windows\System32\drivers\s23gpuprobe.sys` (verify `MD5 = 6fd5c9e0bbcaf468eecb5f20f2de8888`).
   - Pre-allocate `C:\Users\Public\s23gpuprobe.log` (`fsutil file createnew C:\Users\Public\s23gpuprobe.log 65536`).
   - Register Demand-Start service:
     ```cmd
     sc create s23gpuprobe type= kernel start= demand binPath= C:\Windows\System32\drivers\s23gpuprobe.sys
     ```
2. **Pass 1 — Safe Read-Only Telemetry (`StageMask = 0x01`, default):**
   ```cmd
   reg add HKLM\SYSTEM\CurrentControlSet\Services\s23gpuprobe\Parameters /v StageMask /t REG_DWORD /d 1 /f
   sc start s23gpuprobe
   type C:\Users\Public\s23gpuprobe.log
   ```
   - Verify `CMD_DB[cx.lvl]=0x30000`, `mx.lvl=0x30010`, `gfx.lvl=0x30050`, `mxc.lvl=0x300A0` + AUX VLVL tables, `GCC_GPU_BCR == 0`, and safe AXI gate status.
3. **Pass 2 — Opt-In Clock/Power Bring-Up & Live Adreno 740 Hardware Read (`StageMask = 0x03`):**
   ```cmd
   reg add HKLM\SYSTEM\CurrentControlSet\Services\s23gpuprobe\Parameters /v StageMask /t REG_DWORD /d 3 /f
   sc start s23gpuprobe
   type C:\Users\Public\s23gpuprobe.log
   ```
   - Confirm `GPUCC_CX_GDSCR` bit 31 == `1`, `PLL0`/`PLL1` LOCKED, `GPUCC_GX_GDSCR` bit 31 == `1` (`GX_CFG` bit 15 == `0`), and non-`0xFFFFFFFF` live reads of `GMU_CM3_SYSRESET`, `GMU_GFX_PWR_CLK_ST`, `GMU_AO_CX_BUSY_ST`, `RBBM_STATUS`, `GBIF_HALT`, and `CP_SQE_CNTL`.
4. **Pass 3 — Opt-In Scratch R/W & GX Collapse/Recovery Self-Test (`StageMask = 0x07`):**
   ```cmd
   reg add HKLM\SYSTEM\CurrentControlSet\Services\s23gpuprobe\Parameters /v StageMask /t REG_DWORD /d 7 /f
   sc start s23gpuprobe
   type C:\Users\Public\s23gpuprobe.log
   ```
   - Confirm `[CX SCRATCH] ... -> PASS`, `[GX SCRATCH]` exact pattern echo, and `[GX COLLAPSE] down=OK` -> `[GX RECOVER] up=OK` -> `[GX POST-RECOVER]`.
5. **Phase 2 Firmware Extraction & UEFI Integration (Only After Passes 1–3 Succeed):**
   - In Android/Recovery, pull S23's stock `/vendor/firmware/a740_sqe.fw`, `/vendor/firmware/gmu_gen70200.bin`, and `/vendor/firmware_mnt/image/a740_zap.*` into `gpu_research\s23_gpu_fw\`.
   - Integrate `gpu_research\s23_gpu_bringup\s23_uefi_gpu_dxe.{c,inf}` + `s23_gpu_acpi.hex` (`-DS23_INCLUDE_GPU_SSDT_AML`) + level-triggered GPU SMMU `IORT` into a **new separate UEFI build** (`Mu-dm1q-gpu.img`, never overwriting working baseline images).
6. **Parallel Quick Wins (Recommended Next After GPU Probe):**
   - Build a lightweight **EPSS CPUfreq (`0x17D91000..0x17D93000`) + TSENS (`0x0C271000..0x0C273000`)** telemetry/DVFS driver, and test **QCA6490 Bluetooth over `qupv3_se14_4uart (0x00898000)`** using the S23 `hpbtfw21.tlv` / `hpnv21.bin` firmware already on disk.
