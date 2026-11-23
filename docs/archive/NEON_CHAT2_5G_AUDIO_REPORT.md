# Samsung Galaxy S23 (`dm1q` / `SM-S911B`, `SM8550-AC`) — Chat #2 Offline Bring-Up Report: X70 5G Modem / PIL & Audio / Haptics / Sensors / All-26-SE QUPv3

**Date:** 2026-11-21  
**Mode:** 100% Offline Parallel Worker (Chat #2) — zero `adb`, zero `fastboot`, zero `s23link.py`, zero writes outside the 5 exclusive Chat #2 paths (`modem_5g_research\`, `drivers\s23pilprobe\`, `audio_sensors_research\`, `drivers\s23sensprobe\`, `NEON_CHAT2_5G_AUDIO_REPORT.md`).  
**Target OS:** Windows 11 Pro 24H2 ARM64 (`10.0.26100.4349`)  
**Toolchain:** WSL `clang-18 --target=aarch64-pc-windows-msvc -Wall -Wextra -Werror` + `lld-link /DRIVER:WDM /TSAWARE:NO` + forced `.reloc` + PE CheckSum fixup + `iasl 20230628` + Authenticode SHA-256 (`CN=S23TouchTest2020`, Thumbprint `05DBD92557F4FC52EF2A5E5955789D8EED637C68`) + `verify_pe.py` against phone `ntoskrnl_phone.exe` (`3,246` ARM64 exports).

---

## 1. Executive Summary

| Deliverable | Scope | Build & Verification Result |
|---|---|---|
| **Task 1A — 5G Modem & PIL Blueprint** | `modem_5g_research\S23_5G_MODEM_AND_PIL_BRINGUP.md` (`24,569 B`) | Verified against `s23_live.dts`, `qcom_q6v5_pas.c`, `qcom_spss.c`, `qcom_scm.c`, `smem.c`, `smp2p.c`, `qcom_glink_smem.c`, `qcom-ipcc.h`, `uefipil.cfg`, `MemoryMapLib.c`, and `PILDxe`/`SPSSDxe` binaries |
| **Task 1B — Inert Subsystems ACPI SSDT** | `modem_5g_research\s23_subsys_pil.asl` -> `.aml` (`2,085 B`) & `.hex` (`20,003 B`) | `iasl -tc`: **0 Errors, 0 Warnings, 0 Remarks, 19 Optimizations** (Master gate `S23P = 0x00` -> 100% inert `_STA = 0` default) |
| **Task 1C — Safe SMEM/CMD_DB/PIL Probe Driver** | `drivers\s23pilprobe\pkg\s23pilprobe.sys` (`v1.0.0.1`, `76,168 B`) + `install_s23pilprobe.ps1` | `clang-18 -Wall -Wextra -Werror`: **PASS**; Signed SHA-256 (`05DBD925...`); `verify_pe.py`: **10/10 `ntoskrnl_phone.exe` OK, `missing=[]`, `PE VERIFICATION: PASS`** |
| **Task 2A — Audio/Haptics/Sensors & QUPv3 Map** | `audio_sensors_research\S23_AUDIO_HAPTICS_SENSORS_MAP.md` (`21,448 B`) | Complete 26-SE QUPv3 master table (`QUPv3_I2C_HUB` 10 SEs + `QUPv3_1` 8 SEs + `QUPv3_2` 8 SEs) + `CS35L43`, `CS40L26A`, `MAX77705`, `TSL2585`, `ISG6320`, `hall_ic` & ADSP SSC breakdown |
| **Task 2B — Safe All-26-SE QUPv3 & I2C Probe Driver** | `drivers\s23sensprobe\pkg\s23sensprobe.sys` (`v1.0.0.1`, `49,032 B`) + `install_s23sensprobe.ps1` | `clang-18 -Wall -Wextra -Werror`: **PASS**; Signed SHA-256 (`05DBD925...`); `verify_pe.py`: **10/10 `ntoskrnl_phone.exe` OK, `missing=[]`, `PE VERIFICATION: PASS`** |

---

## 2. Critical Silicon Truths Verified Against Live S23 DTS & S23 Kernel Source

Direct inspection of `/home/mahan/mu_s23/s23_live.dts`, `/home/mahan/s23/src/kernel_platform/msm-kernel/`, and `/home/mahan/mu_s23/Mu-Silicium/` resolved **eight major hardware discrepancies** before writing a single line of driver code:

1. **IPA on S23 (`SM8550`) Is IPA v5.0 at `0x03E00000` (NOT `0x01E00000` / v5.5):**
   - `s23_live.dts` L24701: `qcom,ipa@3e00000`, `ipa-base = 0x03E00000` (`0x84000`), `gsi-base = 0x03E04000` (`0xFC000`), `qcom,ipa-hw-ver = <0x18>` (IPA 5.0), `PAS ID = 0x0F`, `ipa-irq GSI 686`, `gsi-irq GSI 464`, SMMU SID `0x4A0`.
2. **SM8550 `MSS`/`LPASS`/`CDSP`/`IPA` Clocks Are RPMh ARC/BCM-Controlled (NOT Legacy GCC CBCRs):**
   - Verified in `gcc-kalama.c`, `clk-rpmh.c`, and `debugcc-kalama.c`: `RPMH_IPA_CLK` maps to BCM **`IP0`** (`measure_only_ipa_2x_clk` is dummy), and `MSS`/`ADSP`/`CDSP`/`SPSS` use RPMh `xo.lvl` + ARC rails (`cx.lvl`, `mx.lvl`, `mss.lvl`, `nsp.lvl`) + BCMs (`SH0`, `MC0`, `ACV`, `CE0`).
3. **Two `MemoryMapLib.c` Carveout Gaps + Pre-Boot `SPSSDxe` Gunyah/XPU Safety Catch:**
   - `adspslpi_region@9ea00000` (`0x059B4000`) ends at `0xA43B4000`, overlapping `Display Demura` (`0xA2A80000`) past `PIL Reserved` (`0xA2A80000`), and `mpss_dsm_region@d4d00000` (`51 MiB`) is not yet carved out in `MemoryMapLib.c`.
   - `SPSSDxe.efi` + `PILDxe.efi` (`uefipil.cfg`) load `[SPSS]` (`\image\spss1t`, `SubsysID=14`, `0x9B100000`) during UEFI DXE! `s23pilprobe.sys` therefore reads **IMEM `ImageLoadInfo` (`0x146AA94C`, 10 × 20 B)** first and skips `0x9B100000`/`0x9B280000` (and any IMEM-active PIL region) by default to guarantee zero risk of a Gunyah Stage-2 Data Abort!
4. **SM8550 `SMP2P` Uses Non-Contiguous Item IDs (`94`, `428`, `429`, `432`, `435`, `443`):**
   - `smp2p-cdsp` inbound is SMEM item **`94` (`0x5E`)** and `smp2p-adsp` inbound is **`443` (`0x1BB`)** (outside `427..438`). `s23pilprobe.sys` scans ALL global and partitioned items (`0..512`, both uncached forward and cached backward with strict `cpos > free_cac` from `smem.c` L625) and decodes `94` + `427..445` + `478..485`.
5. **SM8550 Exposes 26 QUPv3 Serial Engines Across 3 Wrappers (NOT Just 16 Across `QUP0`+`QUP2`):**
   - `QUPv3_I2C_HUB` (`0x009C0000`, **10 FIFO-only I2C SEs** `0x00980000..0x009A4000`, `GCC_QUPV3_I2C_S_AHB_CLK` + `CORE_CLK` + `S0..S9`), **`QUPv3_1`** (`0x00AC0000`, **8 multi-protocol SEs** `0x00A80000..0x00A9C000`), and **`QUPv3_2`** (`0x008C0000`, **8 multi-protocol SEs** `0x00880000..0x0089C000`).
   - Exact live-driver bus locations verified: `s23touch.sys` owns `QUPv3_1 SE4 (0x00A90000)`, `s23batt.sys` bit-bangs `QUPv3_I2C_HUB SE8` pins `GPIO206/207 (0x009A0000)`, `s23btprobe` owns `QUPv3_2 SE6 (0x00898000)`, and `QUPv3_1 SE2 (0x00A88000)` has TZ-reserved `GPIO36..39`. All are strictly protected in `s23sensprobe.sys`!
6. **S23 Smart Speaker Amps Are Dual Cirrus Logic `CS35L43` (NOT `CS35L41`), and `WCD9385` / `WSA884x` / `SWR0..3` Are Disabled:**
   - `cs35l43-right@40` (`bot`, `rst=GPIO42`) and `cs35l43-left@41` (`rcv`, `rst=GPIO55`, shared `irq=GPIO14`) sit on **`QUPv3_I2C_HUB SE6` (`0x00998000`, `GPIO8/9`)** (`DEVID=0x00035A43`, 32-bit BE I2C) fed by **LPASS `TDM0` (`GPIO126..129`)**. `wcd938x-codec`, `wsa-macro`, `wsa2-macro`, and `swr0..3` are all `status = "disabled"` (`qcom,wcd-disabled = <1>`).
7. **S23 Tactile Haptics Is Driven by Cirrus Logic `CS40L26A` (NOT `MAX77705` Haptic):**
   - `cs40l26a@40` sits on **`QUPv3_I2C_HUB SE3` (`0x0098C000`, `GPIO22/23`, `rst=GPIO145`, `irq=GPIO88`)** with `DEVID = 0x0040A260` and built-in ROM waveform mailbox (`0x00013020`) — enabling direct Windows tactile clicks over I2C with zero ADSP! `qcom,hv-haptics@f000` is `status = "disabled"`, and `MAX77705 @ 0x66` (`I2C_HUB SE8`) serves PMIC/FG(`0x36`)/Charger(`0x69`)/MUIC(`0x25`).
8. **Sensors Split Cleanly Between AP-Visible (`TSL2585`, `ISG6320`, `hall_ic`) and ADSP SSC (`LSM6DSO`, `AK09918C`, `LPS22DF`, Front Prox):**
   - Rear ALS/flicker (`ams,tsl2585@39` on `I2C_HUB SE0`), grip/SAR (`isg6320@28` on `i2c-gpio @ 53` `GPIO169/170`), and Hall (`GPIO107`) are AP-accessible; IMU/mag/baro/front-prox sit inside LPASS SSC (`adspslpi_region@9ea00000`, `sensor_vdd=pm_humu_l1`).

---

## 3. Summary Hardware Tables

### 3.1 Remoteproc PIL Subsystems & Reserved Carveouts (`s23_live.dts` + `uefipil.cfg`)

| Subsystem | MMIO Base | PAS ID (Main/DTB) | DDR Carveout (Base / Size) | WDOG / IRQ (`ACPI GSI`) | IPCC Client | SMP2P In/Out SMEM |
|---|---|---:|---|---:|---:|---:|
| **MSS (X70 5G)** | `0x04080000` | `4` / `38 (0x26)` | `0x8A800000` (`264 MiB`) + DTB `0x9B000000` (`512 KiB`) + DSM `0xD4D00000` (`51 MiB`) | `GSI 296` | `2` | `435` / `428` |
| **ADSP (LPASS/SSC)** | `0x03000000` | `1` / `36 (0x24)` | `0x9EA00000` (`~89.7 MiB`) + DTB `0x9E980000` (`512 KiB`) | `GSI 518` | `3` | `443` / `429` |
| **CDSP (NPU)** | `0x32300000` | `18` / `37 (0x25)` | `0x9C900000` (`32 MiB`) + DTB `0x9E900000` (`512 KiB`) | `GSI 610` | `6` | `94` / `432` |
| **SPSS (SPU)** | `0x01881000` | `14` (`0x0E`) | `0x9B100000` (`1.5 MiB`) *(UEFI `SPSSDxe` target!)* | `GSI 384` | `16` | SCSR IRQs |
| **IPA v5.0 + GSI** | `0x03E00000` / `0x03E04000` | `15` (`0x0F`) | `0x9B080000` (`64 KiB`) + GSI `0x9B090000` (`40 KiB`) | `GSI 686` / `464` | — | `ipa` on `smp2p-modem` |

### 3.2 Active S23 QUPv3 Serial Engines (`13` Enabled of `26` Total)

| Wrapper / SE | Phys Base | GCC Vote (`Reg:Bit`) | GSI | GPIOs (`FUNC_SEL`) | Protocol | Connected S23 Peripheral(s) | `s23sensprobe` Stage 2 Mode |
|---|---|---|---:|---|---|---|---|
| `I2C_HUB SE0` | `0x00980000` | `0x52008:BIT10` | `496` | `16, 17` (fn1) | I2C | NXP `pn547@28` NFC + `ams,tsl2585@39` Rear ALS | **PROTECTED** (read-only if on) |
| `I2C_HUB SE3` | `0x0098C000` | `0x52008:BIT13` | `499` | `22, 23` (fn1) | I2C | **Cirrus `cs40l26a@40` LRA Haptics** (`rst=145`, `irq=88`) | Active FW + **DEVID/REVID probe** |
| `I2C_HUB SE5` | `0x00994000` | `0x52008:BIT15` | `501` | `6, 7` (fn1) | I2C | Samsung `s2miw04@5b` Wireless Charger | Active FW + **ID probe** |
| `I2C_HUB SE6` | `0x00998000` | `0x52008:BIT16` | `502` | `8, 9` (fn1) | I2C | **Dual Cirrus `cs35l43@40/41` Speaker Amps** (`rst=42/55`) | Active FW + **DEVID/REVID probe** |
| `I2C_HUB SE8` | `0x009A0000` | `0x52010:BIT14` | `504` | `206, 207` (fn1) | I2C | **`max77705@66/36/69/25` + `pca9481@57`** (`s23batt` live!) | **PROTECTED** (opt-in `0x07` only) |
| `I2C_HUB SE9` | `0x009A4000` | `0x52010:BIT15` | `505` | `84, 85` (fn1) | I2C | `qcom,cam-i2c-actuator@18` | Active FW scan only |
| `QUP1 SE4` | `0x00A90000` | `0x52008:BIT26` | `389` | `44..47` (fn1) | SPI | **STM `stm_ts_spi` Touchscreen** (`s23touch` live!) | **PROTECTED** (read-only if on) |
| `QUP1 SE6` | `0x00A98000` | `0x52008:BIT28` | `395` | `48` (fn2), `49` (fn1) | I2C | NXP `eusb2-repeater@4f` | Active FW scan only |
| `QUP1 SE7` | `0x00A9C000` | `0x52010:BIT16` | `611` | `26, 27` (fn1) | UART | 2-wire `hsuart5` (`sec_atd`) | Active FW scan only |
| `QUP2 SE0` | `0x00880000` | `0x52010:BIT4` | `405` | `56..59` (fn1) | SPI | NXP `p61` eSE Secure Element | Active FW scan only |
| `QUP2 SE1` | `0x00884000` | `0x52010:BIT5` | `615` | `60, 61` (fn1) | I2C | `qcom,cam-i2c-sensor@20` | Active FW scan only |
| `QUP2 SE2` | `0x00888000` | `0x52010:BIT6` | `616` | `64, 65` (fn1) | I2C | Samsung `sec_k250a@23` StrongBox | Active FW scan only |
| `QUP2 SE6` | `0x00898000` | `0x52010:BIT10` | `493` | `76..79` (fn1) | UART | **QCA6490 BT 5.3 UART14** (`s23btprobe`!) | **PROTECTED** (read-only if on) |
| `QUP2 SE7` | `0x0089C000` | `0x52010:BIT17` | `494` | `72, 106` (fn2) | I2C | `qcom,cam-i2c-ois@62` | Active FW scan only |

*(Full 26-row table including all 12 disabled/TZ-reserved SEs is documented in `audio_sensors_research\S23_AUDIO_HAPTICS_SENSORS_MAP.md`.)*

---

## 4. Complete File Inventory, Byte Sizes, MD5 Checksums & Verification Matrix

| Deliverable Path (relative to `s23_dm1q\`) | Size | MD5 | Verification |
|---|---:|---|---|
| `modem_5g_research\S23_5G_MODEM_AND_PIL_BRINGUP.md` | 24,569 B | `4316fc768a0cd9c2b10bd689481e02ac` | Silicon-verified against `s23_live.dts` + S23 kernel + `uefipil.cfg` |
| `modem_5g_research\s23_subsys_pil.asl` | 11,862 B | `fac4aac22a9e0259a34e4e418694f8c5` | `iasl -tc`: **0 Errors, 0 Warnings, 0 Remarks** (`_STA = 0` default) |
| `modem_5g_research\s23_subsys_pil.aml` | 2,085 B | `4599af17daa0c0d18f06497be1a4abee` | Compiled by `iasl 20230628` |
| `modem_5g_research\s23_subsys_pil.hex` | 20,003 B | `2257203c59f5cd229f917a84133ea68f` | C byte array for UEFI DXE embedding |
| `drivers\s23pilprobe\s23pilprobe.c` | 47,490 B | `9514bd845e6cea24e4b36700a8d7b98a` | `v1.0.0.1`, `clang-18 -Wall -Wextra -Werror` PASS |
| `drivers\s23pilprobe\build_wsl.py` | 2,410 B | `f5cacc2f6a225bc86dce029d50807757` | ARM64 WDM build + forced `.reloc` + PE CheckSum fixup |
| `drivers\s23pilprobe\sign.ps1` | 1,357 B | `1ed5e35381705b6a10656f4da5721940` | Authenticode SHA-256 (`CN=S23TouchTest2020`) |
| `drivers\s23pilprobe\pkg\s23pilprobe.sys` | 76,168 B | `c16cd8335c7073ee29a5bf703f2e5047` | Signed SHA-256; **10/10 OK, `PE VERIFICATION: PASS`** |
| `drivers\s23pilprobe\pkg\s23test.cer` | 778 B | `60c63680ee9fa82b580a25b225d4c996` | Public test-signing certificate |
| `drivers\s23pilprobe\pkg\install_s23pilprobe.ps1` | 1,730 B | `0a0b34a88aa82f2e359f05acad39e54e` | Demand-start (`Start=3`) installer, `StageMask=0x01` read-only |
| `audio_sensors_research\S23_AUDIO_HAPTICS_SENSORS_MAP.md` | 21,448 B | `a2b62305a66822bd932e874dcddf890e` | Complete 26-SE QUPv3 + Audio/Haptics/Sensors blueprint |
| `drivers\s23sensprobe\s23sensprobe.c` | 48,129 B | `2120e11f5df7f4f1972882235f174959` | `v1.0.0.1`, `clang-18 -Wall -Wextra -Werror` PASS |
| `drivers\s23sensprobe\build_wsl.py` | 2,416 B | `ff12a8abbcb70476aa626c38f374a5f4` | ARM64 WDM build + forced `.reloc` + PE CheckSum fixup |
| `drivers\s23sensprobe\sign.ps1` | 1,362 B | `63a707194e4300a773dc0a120903c282` | Authenticode SHA-256 (`CN=S23TouchTest2020`) |
| `drivers\s23sensprobe\pkg\s23sensprobe.sys` | 49,032 B | `8115f40a54d436180617b63963a5f14a` | Signed SHA-256; **10/10 OK, `PE VERIFICATION: PASS`** |
| `drivers\s23sensprobe\pkg\s23test.cer` | 778 B | `60c63680ee9fa82b580a25b225d4c996` | Public test-signing certificate |
| `drivers\s23sensprobe\pkg\install_s23sensprobe.ps1` | 2,071 B | `024f6ab6663a88d90728e10860af0256` | Demand-start (`Start=3`) installer, `StageMask=0x01` read-only |

---

## 5. Exact `verify_pe.py` & `iasl -tc` Verification Outputs

### 5.1 `iasl -tc modem_5g_research\s23_subsys_pil.asl`
```text
Intel ACPI Component Architecture
ASL+ Optimizing Compiler/Disassembler version 20230628
ASL Input:     s23_subsys_pil.asl -   11862 bytes     58 keywords      0 source lines
AML Output:    s23_subsys_pil.aml -    2085 bytes      7 opcodes      51 named objects
Hex Dump:      s23_subsys_pil.hex -   20003 bytes
Compilation successful. 0 Errors, 0 Warnings, 0 Remarks, 19 Optimizations
```

### 5.2 `verify_pe.py` — `drivers\s23pilprobe\pkg\s23pilprobe.sys`
```text
FILE: c:\Users\Mahan\Documents\windows-on-android\s23_dm1q\drivers\s23pilprobe\pkg\s23pilprobe.sys (76168 bytes, MD5=c16cd8335c7073ee29a5bf703f2e5047)
  Machine=0xAA64 (ARM64=0xAA64), Magic=0x020B (PE32+=0x020B)
  Subsystem=1 (NATIVE=1), DllChars=0x2160, TSAWARE=False
  Reloc=0x16000+224, AuthSig=0x12400+1416
  CheckSum stored=0x00021333 calc=0x00021333 match=True
  Imports from ntoskrnl.exe (10): KeDelayExecutionThread, KeStallExecutionProcessor, MmMapIoSpace, MmUnmapIoSpace, RtlInitUnicodeString, ZwClose, ZwCreateFile, ZwOpenKey, ZwQueryValueKey, ZwWriteFile
  Ref ntoskrnl.exe check (Machine=0xAA64, 3246 exports): 10/10 OK, missing=[]
PE VERIFICATION: PASS
```

### 5.3 `verify_pe.py` — `drivers\s23sensprobe\pkg\s23sensprobe.sys`
```text
FILE: c:\Users\Mahan\Documents\windows-on-android\s23_dm1q\drivers\s23sensprobe\pkg\s23sensprobe.sys (49032 bytes, MD5=8115f40a54d436180617b63963a5f14a)
  Machine=0xAA64 (ARM64=0xAA64), Magic=0x020B (PE32+=0x020B)
  Subsystem=1 (NATIVE=1), DllChars=0x2160, TSAWARE=False
  Reloc=0xF000+132, AuthSig=0xBA00+1416
  CheckSum stored=0x0000E554 calc=0x0000E554 match=True
  Imports from ntoskrnl.exe (10): KeDelayExecutionThread, KeStallExecutionProcessor, MmMapIoSpace, MmUnmapIoSpace, RtlInitUnicodeString, ZwClose, ZwCreateFile, ZwOpenKey, ZwQueryValueKey, ZwWriteFile
  Ref ntoskrnl.exe check (Machine=0xAA64, 3246 exports): 10/10 OK, missing=[]
PE VERIFICATION: PASS
```

---

## 6. Ready-to-Run Live Validation Commands (When Ready on S23 Windows)

1. **SMEM v12 / CMD_DB / IMEM PIL / GCC / PIL DDR Probe (`s23pilprobe`, 100% read-only):**
   ```powershell
   powershell -ExecutionPolicy Bypass -File .\install_s23pilprobe.ps1
   sc.exe start s23pilprobe ; Get-Content C:\Users\Public\s23pilprobe.log
   ```
2. **All-26-SE QUPv3 & Audio (`CS35L43`) / Haptics (`CS40L26A`) Probe (`s23sensprobe`):**
   ```powershell
   powershell -ExecutionPolicy Bypass -File .\install_s23sensprobe.ps1
   # Step 1 (100% read-only default):
   sc.exe start s23sensprobe ; Get-Content C:\Users\Public\s23sensprobe.log
   # Step 2 (active 26-SE GENI FW table + read-only CS35L43 @40/41 & CS40L26A @40 DEVID probe):
   reg add HKLM\SYSTEM\CurrentControlSet\Services\s23sensprobe\Parameters /v StageMask /t REG_DWORD /d 3 /f
   sc.exe start s23sensprobe ; Get-Content C:\Users\Public\s23sensprobe.log
   ```
