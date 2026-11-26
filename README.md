# Windows 11 ARM64 on the Samsung Galaxy S23 (`dm1q` / `SM-S911B`)

[![lint](https://github.com/MahanYTT/s23-windows-11/actions/workflows/lint.yml/badge.svg)](https://github.com/MahanYTT/s23-windows-11/actions/workflows/lint.yml)
[![GitHub Stars](https://img.shields.io/github/stars/MahanYTT/s23-windows-11?style=flat)](https://github.com/MahanYTT/s23-windows-11/stargazers)

Native **Windows 11 Pro 24H2 ARM64 (`10.0.26100`)** on the **Samsung Galaxy S23** (Snapdragon 8 Gen 2 for Galaxy, `SM8550-AC`, "Kailua").
This repository contains the ARM64 kernel drivers, UEFI DXE patches, ACPI SSDTs, kernel research probes and PC-side tooling used to bring the phone up.

> [!CAUTION]
> **Hardware & OS safety rules — read before touching a device**
> 1. **Never write to `sdb` or `sde`** — they are power-on write-protected (`ro=1`). Windows lives on `sda43` (`/dev/block/by-name/win`), the UEFI boot image on `sda25`.
> 2. **Keep `SanPolicy = 4`** (`HKLM\SYSTEM\ControlSet001\Services\partmgr\Parameters`) on the phone so the Android UFS LUNs stay offline in Windows.
> 3. **Keep `testsigning on`** (`bcdedit`). All `s23*.sys` drivers are test-signed (`CN=S23TouchTest2020`); the *Test Mode* watermark is expected.
> 4. **Never call `Set-Date`** in live scripts on the phone. The UEFI RTC starts at `2024-04-01`; jumping the clock forward triggers the display idle-off timer (black screen).
> 5. **Avoid WHEA `0x124` SError aborts** — never touch TZ-reserved QUP/TLMM pins (`GPIO36..39`, `50..51`, `56..59`, `64..65`; `QUP1_SE2`, `QUP2_SE0`, `QUP2_SE2`), non-APPS SMEM partitions, KGSL `0x03D00000` while `GPUCC_GX_GDSCR` bit 31 is 0, GMU `0x03D6A000` while `GPUCC_CX_GDSCR` is collapsed, or PIL DDR carve-outs before checking `IMEM ImageLoadInfo (0x146AA94C)`.

---

## Status

**Demonstrated:** native Windows 11 Pro 24H2 ARM64 (`10.0.26100`) on UFS, 120 Hz AMOLED display with native brightness & Night Light, experimental Adreno 740 WDDM 2.0 acceleration (Mesa Freedreno D3D11 + OpenGL 4.6), multitouch, volume keys, Wi-Fi 6E with internet, 5G/LTE cellular data, native SMS & eSIM provisioning (`Snapdragon X70` + `IPA v5.5`), Bluetooth 5.3 + BLE, stereo ADSP AudioReach + dual CS35L43 audio, MAX77705 battery reporting and wired fast charging, and WinUSB PC link.  
**Still unfinished:** cellular voice/IMS, IPv6 dual-stack & 5G NR tuning, native USB-C dual-role Host/OTG + PD 3.0 + DP Alt-Mode, power-button sleep/wake and system suspend/resume (S0ix), haptics (`CS40L26A`), motion/light/proximity sensors, cameras, GNSS, NFC, TPM/Secure Boot, and a portable public installer/UEFI workflow.

| Area | Status | Notes |
|---|---|---|
| UEFI / ACPI | **WORKS ✅** | Custom UEFI boot image on `sda25` with `S23DisplayFixDxe` ([`display_research/`](display_research/)), PCIe0 bring-up and ACPI SSDTs; boots WinPE and full Windows 11 Pro 24H2. |
| CPU | **WORKS ✅** | Native ARM64 execution across all 8 cores on Snapdragon 8 Gen 2 for Galaxy (`SM8550-AC`); resident dynamic EPSS DVFS & 41-sensor TSENS thermal governor + `s23perfctl.ps1` CLI ([`drivers/s23perf/`](drivers/s23perf/)). |
| RAM | **WORKS ✅** | Usable Windows memory map with reserved subsystem carve-outs for daily desktop and GPU workloads; full stress and suspend-retention qualification still pending. |
| UFS / installation | **WORKS ✅** | Installed Windows 11 boots directly from 4 KiB-sector UFS partition `sda43` (`/dev/block/by-name/win`) with Android LUNs isolated (`SanPolicy = 4`); public reproducible installer still pending. |
| Display | **WORKS ✅** | Native `1080×2340 @ 120 Hz` AMOLED presentation with high-resolution VSync, native brightness slider and Night Light ([`drivers/s23gpu/`](drivers/s23gpu/), [`display_research/`](display_research/)). Display idle-off/sleep-wake hardening still pending. |
| GPU / WDDM acceleration | **WORKS ✅** | Experimental Adreno 740 WDDM 2.0 display + 3D KMD ([`drivers/s23gpu/`](drivers/s23gpu/)): GMU + SQE + ZAP + KGSL-SMMU, Mesa Freedreno D3D11 (`d3d10umd`) and **OpenGL 4.6** WGL ICD (ARM64 + x64), live Task Manager GPU utilization & TSENS temperature. Broad stability/Vulkan hardening still pending. |
| Touchscreen | **WORKS ✅** | STM `FTS2BA61Y` HID multitouch minidriver on QUP1 SE4 GENI SPI (`0x00A90000`, IRQ `gpio25`, [`drivers/s23touch/`](drivers/s23touch/)). Everyday multitouch works; suspend/resume qualification pending. |
| Physical buttons | **PARTIAL 🟡** | Read-only SPMI PMIC arbiter HID minidriver ([`drivers/s23keys/`](drivers/s23keys/)): Volume Up / Volume Down work natively; Power button display-off/wake behavior still needs investigation. |
| Wi-Fi 6E | **WORKS ✅** | Live with internet on `QCA6490` PCIe0 (`PCI\VEN_17CB&DEV_1103`): UEFI RC bring-up + Qualcomm `qcwlan8280.sys` + S23 `bdwlan.elf` ([`wifi_research/`](wifi_research/), [`drivers/s23pcieprobe/`](drivers/s23pcieprobe/)). |
| Bluetooth 5.3 + BLE | **WORKS ✅** | `BthMini` extensible-transport lower filter for `QCA6490` on `UART14` (`3.2 Mbaud`, IBS, [`drivers/s23bthx/`](drivers/s23bthx/)): Classic + BLE, A2DP-sized ACL buffers, plus [`fix_bt_settings_ui.ps1`](drivers/s23bthx/fix_bt_settings_ui.ps1) for Windows Settings pairing visibility. Suspend/resume & HFP/SCO qualification pending. |
| Audio | **WORKS ✅** | Native ARM64 PortCls WaveRT + Topology driver (`ROOT\S23AUDIO` / `ACPI\S23AUD01`, [`drivers/s23audio/`](drivers/s23audio/)): lazy ADSP PIL (`PAS 36` + `PAS 1`), GLink/GPRv2/PRM, Gunyah-safe APPS-SMMU bypass, dual Cirrus Logic `CS35L43` stereo playback verified via WASAPI. Mic capture & Halo DSP calibration pending. |
| Battery reporting | **WORKS ✅** | Maxim `MAX77705` fuel gauge (`@0x36`) Windows battery miniclass driver (`battc.sys`, [`drivers/s23batt/`](drivers/s23batt/)): validated percentage, voltage, current rate and calibrated battery temperature. |
| Charging | **WORKS ✅** | Maxim `MAX77705` charger (`@0x69`, [`drivers/s23batt/`](drivers/s23batt/)): 3.0 A input limit, 2.5 A CC wired fast charge, 4.4 V float, watchdog handling and abnormal-off recovery. Dynamic USB-PD/PPS (`PCA9481`) & wireless charging (`S2MIW04`) not yet wired. |
| USB-C / PC link | **PARTIAL 🟡** | WinUSB remote shell & file transfer works ([`s23link.py`](s23link.py)); native USB-C architecture & host-tested MAX77705 OPCODE / PD 3.0 codec ready ([`usb_research/`](usb_research/)). Shared `SE8` PMIC arbiter, Host/OTG (`s23urs.sys`), USB-PD (`UcmCx`) & DP Alt-Mode not yet implemented. |
| 5G cellular / SIM/ eSIM / SMS | **WORKS ✅ (5G might not connect and fallback to 4G)** | Live on-air cellular internet, native Windows 11 SMS & eSIM provisioning (`Snapdragon X70` MPSS split-ELF PAS boot, RAM-sandboxed `rmtfs`/`pd-mapper`/`tqftpserv`, GLink `"IPCRTR"`, NXP SN220 eUICC `eSIM mode(2,3)` + SGP.22 ES10c `lpa:` onboarding, `s23mbb.sys` `MBBCx 1.0 + NetAdapterCx 2.0 + MBIMEx 3.0` with `GW_PRIMARY` provisioning, QMI `DPM`/`WDA`/`WDS`, MBIM 1.0 `UUID_SMS` ↔ `QMI_WMS 0x05`, native `s23sms.ps1` `WwanSvc`/`mbnapi.dll` client, `s23modemd.exe --sms-*` CLI, and SM8550 IPA v5.5 + GSI v5.5 `2048 B` QMAP datapath, [`drivers/s23modem/`](drivers/s23modem/), [`drivers/s23mbb/`](drivers/s23mbb/)). Cold-boot auto-registration, IPv4/DNS, ping, HTTPS, SMSC `+32475161616`, and live over-the-air MT SMS delivery verified; voice/IMS & IPv6 dual-stack pending. |
| Power management | **PARTIAL 🟡** | Working awake profile for daily development and testing; system suspend/resume (Modern Standby / S0ix), display sleep-wake, and idle RPMh/thermal policies are not finished. |
| Haptics | **IN PROGRESS 🛠️** | Cirrus Logic `CS40L26A` verified on live I2C (`QUPv3_I2C_HUB SE3 @0x40`, Device ID `0x0040A260`, [`drivers/s23sensprobe/`](drivers/s23sensprobe/), [`audio_sensors_research/`](audio_sensors_research/)). Firmware load and Windows waveform driver not yet built. |
| Motion / light / proximity sensors | **NOT WORKING ❌** | SSC bus / Windows sensor class integration not implemented yet. |
| Cameras | **NOT WORKING ❌** | Camera sensor / ISP bring-up and Windows AVStream integration not started. |
| GNSS (GPS) | **NOT WORKING ❌** | Windows location / GNSS integration not started. |
| NFC | **NOT WORKING ❌** | Windows NFC class driver integration not started. |
| TPM / production boot security | **NOT WORKING ❌** | Runs with `testsigning on`; Windows TPM and production Secure Boot are not implemented. |

Driver versions are tracked in each driver's `.inf` (`DriverVer`) and in the commit history (`git log --oneline`).

<details>
<summary><strong>Detailed development checkpoints</strong></summary>

### UEFI / ACPI — 4/6 complete

- [x] OEM boot-chain integration (`sda25` UEFI boot image, Project Mu / `dm1q`).
- [x] Custom DXE (`S23DisplayFixDxe`, PCIe0 bring-up) and ACPI SSDT resources.
- [x] Windows boot-manager handoff (`SanPolicy = 4`, `testsigning on`).
- [x] WinPE and full Windows 11 Pro 24H2 (`10.0.26100`) boot.
- [ ] Portable public UEFI build/reconstruction workflow.
- [ ] Cold-start and recovery hardening.

### CPU — 3/4 complete

- [x] Native ARM64 execution on Snapdragon 8 Gen 2 for Galaxy (`SM8550-AC`).
- [x] All 8 logical processors enumerated; EPSS CPUfreq & TSENS telemetry probed ([`drivers/s23perf/`](drivers/s23perf/)).
- [x] Resident dynamic EPSS DVFS & 41-sensor TSENS thermal governor (`Mode=2`, Balanced/Powersave/Performance/Fixed profiles, `s23perfctl.ps1`).
- [ ] Idle and full ACPI/PEP thermal-policy qualification.

### RAM — 2/4 complete

- [x] Usable Windows memory map with reserved subsystem carve-outs.
- [x] Native allocations used by the working desktop & GPU.
- [ ] Full-capacity / stress qualification.
- [ ] Memory retention across suspend.

### UFS / installation — 3/5 complete

- [x] 4 KiB-sector UFS filesystem integration (`sda43` `/dev/block/by-name/win`).
- [x] Windows-compatible GPT partition layout (Android LUNs isolated via `SanPolicy = 4`, `sdb`/`sde` untouched).
- [x] Installed Windows 11 boots directly from internal storage.
- [ ] Reproducible public installation/servicing workflow.
- [ ] Power-loss and storage-performance qualification.

### Display — 4/5 complete

- [x] Native `1080×2340 @ 120 Hz` presentation with high-resolution VSync ([`drivers/s23gpu/`](drivers/s23gpu/) + `S23DisplayFixDxe`).
- [x] Usable Windows desktop scaling.
- [x] Accelerated WDDM 2.0 presentation & dirty-rect / direct-flip scanout.
- [x] Native Windows brightness slider & Night Light support.
- [ ] Full display sleep/wake lifecycle (RTC/idle-off black-screen hardening).

### GPU / WDDM acceleration — 8/9 complete

- [x] Kernel resource/ownership foundations ([`drivers/s23gpu/`](drivers/s23gpu/)).
- [x] GMU CM3 firmware and HFI acknowledgements (`gmu_gen70200.bin`).
- [x] KGSL-SMMU 3-level 4 KiB page tables & GPU VA allocator (host-tested with ASan/UBSan).
- [x] ZAP (`PAS 13`) auth & reset + SQE firmware boot (`a740_sqe.fw`).
- [x] CP Ring0 IB1 command submission, interrupts & fence completion.
- [x] WDDM 2.0 display + 3D KMD integration with live Task Manager GPU utilization & TSENS temperature.
- [x] Mesa Freedreno D3D11 (`d3d10umd`) & **OpenGL 4.6** WGL ICD (ARM64 + x64).
- [x] Hardware-accelerated application rendering & desktop presentation.
- [ ] Broad game/app stability, Vulkan ICD & full production hardening.

### Touchscreen — 3/4 complete

- [x] STM `FTS2BA61Y` GENI SPI transport (`0x00A90000`, IRQ `gpio25`) & report decoding ([`drivers/s23touch/`](drivers/s23touch/)).
- [x] Native Windows HID multitouch minidriver.
- [x] Sustained everyday multitouch demonstrated.
- [ ] General suspend/resume & palm-rejection qualification.

### Physical buttons — 2/3 complete

- [x] Read-only SPMI PMIC arbiter HID minidriver ([`drivers/s23keys/`](drivers/s23keys/)).
- [x] Volume Up / Volume Down working natively in Windows.
- [ ] Power-button display-off / wake behavior (needs investigation).

### Wi-Fi 6E — 6/6 complete

- [x] PCIe0 RC enumeration (`0x01C00000` -> `PCI\VEN_17CB&DEV_1103`) & UEFI bring-up ([`wifi_research/`](wifi_research/)).
- [x] Qualcomm `qcwlan8280.sys` WHQL driver & `wlanfw20.mbn` + S23 `bdwlan.elf` loading.
- [x] Sustained MHI/firmware responses & credits.
- [x] Network scanning, authentication & WPA2/WPA3 association.
- [x] DHCP, DNS and live internet traffic.
- [x] Everyday Wi-Fi 6E networking verified on device.

### Bluetooth 5.3 + BLE — 3/4 complete

- [x] `BthMini` extensible-transport filter ([`drivers/s23bthx/`](drivers/s23bthx/)) on `UART14` (`3.2 Mbaud`, IBS).
- [x] Classic Bluetooth + BLE scanning & device pairing.
- [x] A2DP-sized ACL buffers & peripheral traffic.
- [ ] Full suspend/resume & HFP/SCO qualification.

### Audio — 3/4 complete

- [x] ADSP split-ELF PIL (`PAS 36` + `PAS 1`), GLink/GPRv2/PRM & Gunyah-safe APPS-SMMU DMA ([`drivers/s23audio/`](drivers/s23audio/)).
- [x] Native ARM64 PortCls WaveRT + Topology audio endpoint (`Speakers (Samsung Galaxy S23 Stereo)`).
- [x] Dual Cirrus Logic `CS35L43` stereo speaker playback verified via WASAPI.
- [ ] Microphone capture & Halo DSP speaker-protection calibration.

### Battery reporting — 4/4 complete

- [x] Maxim `MAX77705` fuel gauge (`@0x36`) acquisition ([`drivers/s23batt/`](drivers/s23batt/)).
- [x] Installed Windows battery miniclass driver (`battc.sys`).
- [x] Validated percentage, voltage, current rate & calibrated battery temperature.
- [x] Sustained normal-Windows lifecycle.

### Charging — 3/4 complete

- [x] Maxim `MAX77705` charger (`@0x69`) write-unlock & watchdog handling ([`drivers/s23batt/`](drivers/s23batt/)).
- [x] Sustained normal-Windows wired fast charging (3.0 A input / 2.5 A CC / 4.4 V float) & abnormal-off recovery.
- [x] Positive net charging current verified under Windows load.
- [ ] Dynamic USB-PD 3.0 / PPS (`PCA9481`) negotiation & wireless charging (`S2MIW04`).

### USB-C / PC link — 3/6 complete

- [x] USB peripheral enumeration & WinUSB remote shell / file transfer ([`s23link.py`](s23link.py)).
- [x] Guarded remote-control & file-deploy workflow with WinUSB retry/backoff.
- [x] Native USB-C architecture spec & host-tested MAX77705 OPCODE / PD 3.0 codec ([`usb_research/`](usb_research/)).
- [ ] Race-free `SE8` PMIC bus arbiter (`s23pmic.sys`) shared with `s23batt.sys`.
- [ ] Dynamic Dual-Role Host/OTG (`s23urs.sys`, external USB peripherals/hubs).
- [ ] Native USB-PD 3.0 (`s23usbc.sys` `UcmCx`) & 4K60 DisplayPort Alt-Mode.

### 5G cellular / eSIM / SMS — 7/7 complete

- [x] Stock MPSS / `modem_dtb` / IPA v5.5 firmware ground-truth verified ([`modem_5g_research/FIRMWARE_GROUND_TRUTH.md`](modem_5g_research/FIRMWARE_GROUND_TRUTH.md)).
- [x] Architecture blueprint, inert SSDTs & host-tested QMI/QMUX TLV codec ([`modem_5g_research/qmi_codec.py`](modem_5g_research/qmi_codec.py)).
- [x] MPSS split-ELF PAS boot (`PAS 0x26` + `PAS 4`) & QRTR service discovery (`s23modem_xprt.sys` + `s23modemd.exe`).
- [x] RAM-sandboxed `rmtfs` + physical SIM1 & embedded NXP SN220 eUICC (`eSIM mode(2,3)` + SGP.22 ES10c) via QMI `UIM`.
- [x] Native Windows 11 Cellular & eSIM UI (`s23mbb.sys`: `MBBCx 1.0 + NetAdapterCx 2.0 + MBIMEx 3.0` + `WwanSvc` + `wlpasvc`) & SM8550 IPA v5.5 / GSI v5.5 (`PAS 15` + `apps-smmu` SID `0x4A0`/`0x4A2` + QMI `0x31`/`0x2F` handshake).
- [x] Live on-air eSIM download, cold-boot `GW_PRIMARY` registration, QMI `DPM`/`WDA`/`WDS` data call, full-MTU ping, DNS, and HTTPS verified over cellular.
- [x] Native Windows 11 SMS (`MBIM 1.0 UUID_SMS` ↔ `QMI_WMS 0x05` in `s23mbb.sys`, `WwanSvc`/`mbnapi.dll` client [`s23sms.ps1`](drivers/s23mbb/s23sms.ps1), and `s23modemd.exe --sms-*` CLI) verified live over the air.

### Power management — 1/5 complete

- [x] Working awake profile for daily development & testing.
- [ ] System suspend/resume (Modern Standby / S0ix).
- [ ] Display / power-button sleep-wake lifecycle.
- [ ] Idle clock and RPMh rail policies.
- [ ] Platform thermal / EPSS performance policies.

### Haptics — 1/3 complete

- [x] Live I2C probe verified (`QUPv3_I2C_HUB SE3 @0x40`, Device ID `0x0040A260`, [`drivers/s23sensprobe/`](drivers/s23sensprobe/)).
- [ ] Firmware loading & waveform playback driver.
- [ ] Windows haptic feedback integration.

### Motion / light / proximity sensors — 0/3 complete

- [ ] SSC / sensor bus transport & Windows sensor class integration.
- [ ] Validated accelerometer / gyro / ALS / proximity readings.
- [ ] Auto-rotation & adaptive brightness policy integration.

### Cameras — 0/3 complete

- [ ] Camera sensor / ISP initialization.
- [ ] Windows camera / AVStream integration.
- [ ] Validated photo & video capture.

### GNSS (GPS) — 0/2 complete

- [ ] Windows location / GNSS integration.
- [ ] Validated positioning.

### NFC — 0/2 complete

- [ ] Windows NFC class driver integration.
- [ ] Functional tag / transaction reads.

### TPM / production boot security — 0/3 complete

- [ ] Working Windows TPM integration.
- [ ] Production Secure Boot configuration.
- [ ] Qualification without `testsigning on`.

</details>

---

## Repository layout

```text
.
├── s23link.py                 PC ⇄ phone USB-C control link (WinUSB)
├── drivers/
│   ├── s23gpu/                Adreno 740 WDDM 2.0 KMD + UMD/test tools (MSVC build.ps1)
│   ├── s23batt/               MAX77705 battery & charger miniclass driver
│   ├── s23touch/              FTS2BA61Y HID multitouch
│   ├── s23keys/               Power / volume keys HID
│   ├── s23bthx/               QCA6490 UART Bluetooth transport
│   ├── s23audio/              Native ADSP AudioReach + Dual CS35L43 PortCls driver + WASAPI verifier
│   ├── s23modem/              X70 MPSS PIL/GLink/QRTR/RAM-rmtfs/IPA v5.5 + GSI v5.5 transport (`s23modem_xprt.sys`) + CLI (`s23modemd.exe`)
│   ├── s23mbb/                Native Windows 11 MBBCx 1.0 + NetAdapterCx 2.0 + MBIMEx 3.0 cellular, eSIM & SMS driver (`s23mbb.sys`) + native SMS app (`s23sms.ps1`)
│   ├── s23audprobe/           ADSP PIL + GLink + APPS-SMMU + CS35L43 research probe
│   ├── s23gpuprobe/           GPU research probe + verify_pe.py (PE/import verifier)
│   └── s23*probe/, s23perf/, s23disp_diag/, s23acl*/   research probes & recovery helpers
├── display_research/          UEFI display DXE + display black-screen fix write-up
├── gpu_research/              Adreno 740 architecture notes, GPU SSDT, firmware tooling
├── bt_research/               QCA6490 UART14 bring-up notes + SSDT
├── wifi_research/             QCA6490 PCIe Wi-Fi 6E bring-up & install (working)
├── audio_sensors_research/    audio / haptics / sensors bus map
├── modem_5g_research/         5G modem blueprint, PIL/subsystem notes, SSDTs (index: README.md)
├── usb_research/              native USB-C spec + MAX77705 OPCODE mailbox reference (host-tested)
├── docs/archive/              historical bring-up reports & handoff notes
└── scripts/legacy/            one-off bring-up scripts kept for reference
```

---

## Building the drivers

All drivers target **Windows ARM64 kernel mode** and only import standard kernel exports (`ntoskrnl.exe`, plus class drivers such as `hidclass.sys`, `battc.sys`, `dxgkrnl.sys` where needed).

### Prerequisites

- **WSL** (for `build_wsl.py` drivers): `clang-18`, `lld` (`lld-link`), `llvm-dlltool`, `binutils-aarch64-linux-gnu` (`objdump`), Python 3; `acpica-tools` (`iasl`) for SSDTs.
- **Windows** (for `s23gpu` and signing): Visual Studio 2022 with the ARM64 MSVC toolset, Windows SDK/WDK `10.0.26100`, PowerShell, Python 3, and the test-signing certificate `CN=S23TouchTest2020` in `Cert:\CurrentUser\My`.

### WSL / Clang drivers (`s23batt`, `s23touch`, `s23keys`, `s23bthx`, probes)

```powershell
# 1. compile + link the ARM64 .sys inside WSL (writes drivers/<name>/out/)
wsl python3 ./drivers/s23bthx/build_wsl.py

# 2. sign the driver and create the catalog on Windows (writes drivers/<name>/pkg/)
powershell -ExecutionPolicy Bypass -File .\drivers\s23bthx\sign_and_cat.ps1

# 3. verify the signed binary (ARM64, PE32+, .reloc, checksum, Authenticode, imports)
python .\drivers\s23gpuprobe\verify_pe.py .\drivers\s23bthx\pkg\s23bthx.sys <path-to-phone-ntoskrnl.exe>
```

### MSVC driver (`s23gpu`)

```powershell
powershell -ExecutionPolicy Bypass -File .\drivers\s23gpu\build.ps1
```

`build.ps1` compiles the KMD with `Hostx64\arm64` `cl.exe`/`link.exe`, builds the user-mode test tools, pulls the Mesa UMD/WGL DLLs from the WSL Mesa build, signs everything and runs `verify_pe.py`.

> [!NOTE]
> Always verify and deploy the **signed** `pkg\*.sys` (never the unsigned `out\*.sys`). When compiling by hand, use `Hostx64\arm64`, never `Hostx64\x64`.

### ACPI SSDTs

```bash
iasl -tc gpu_research/s23_gpu_bringup/s23_gpu_acpi.asl
```

---

## Using `s23link.py`

With the phone running Windows and connected over USB-C:

```powershell
python s23link.py ping                       # round-trip test
python s23link.py exec "whoami && hostname"  # run a command as SYSTEM on the phone
python s23link.py exec -t 300 "pnputil /enum-drivers"
python s23link.py put .\drivers\s23batt\pkg\s23batt.sys C:\drv\s23batt.sys
python s23link.py get C:\Users\Public\s23keys.log .\s23keys.log
python s23link.py reboot
python s23link.py wait                       # block until the phone answers again
```

Set `RM9_WINUSB=1` to force the inbox WinUSB transport.

---

## Firmware (not included)

Proprietary Samsung/Qualcomm firmware, partition images, logs and all compiled binaries are excluded via [`.gitignore`](.gitignore). Extract firmware from **your own** device in Lineage Recovery using strictly read-only mounts:

- GPU: [`gpu_research/s23_gpu_bringup/fw/pull_s23_gpu_fw.py`](gpu_research/s23_gpu_bringup/fw/pull_s23_gpu_fw.py) (+ [`extract_and_verify_s23_gpu_fw.py`](gpu_research/s23_gpu_bringup/fw/extract_and_verify_s23_gpu_fw.py))
- Wi-Fi / Bluetooth: [`wifi_research/pull_boot24_and_wifi_fw.py`](wifi_research/pull_boot24_and_wifi_fw.py), [`QCA6490_DRIVER_AND_FW_GUIDE.md`](wifi_research/qca6490_bringup/QCA6490_DRIVER_AND_FW_GUIDE.md)
- Modem, DSPs, audio/haptics: [`modem_5g_research/pull_s23_5g_and_subsys_fw.py`](modem_5g_research/pull_s23_5g_and_subsys_fw.py)

---

## Contributing

- Open pull requests against `master`; keep each PR small and focused.
- Never commit binaries or firmware (`.sys`, `.exe`, `.dll`, `.obj`, `.pdb`, `.bin`, `.mbn`, `.img`, …).
- Kernel code: keep stack frames small (the ARM64 kernel stack is 24 KB — no `__chkstk` in any `.sys`), no CRT/user-mode dependencies, and do not change register offsets, clock/GDSC sequences or power votes without hardware validation.
- Historical reports live in [`docs/archive/`](docs/archive/), one-off bring-up scripts in [`scripts/legacy/`](scripts/legacy/).

---

## Star History

<a href="https://www.star-history.com/?repos=MahanYTT%2Fs23-windows-11&type=date&legend=top-left">
 <picture>
   <source media="(prefers-color-scheme: dark)" srcset="https://api.star-history.com/chart?repos=MahanYTT/s23-windows-11&type=date&theme=dark&legend=top-left" />
   <source media="(prefers-color-scheme: light)" srcset="https://api.star-history.com/chart?repos=MahanYTT/s23-windows-11&type=date&legend=top-left" />
   <img alt="Star History Chart" src="https://api.star-history.com/chart?repos=MahanYTT/s23-windows-11&type=date&legend=top-left" />
 </picture>
</a>
    <img alt="Star History Chart" src="https://api.star-history.com/svg?repos=MahanYTT/s23-windows-11&type=Date" />
  </picture>
</a>
