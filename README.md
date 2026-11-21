# Native Windows 11 Pro 24H2 ARM64 on Samsung Galaxy S23 (`dm1q` / `SM-S911B`)

Hardware bring-up drivers, UEFI DXE patches, ACPI SSDTs, kernel telemetry probes, and tooling for running native **Windows 11 Pro 24H2 ARM64 (`10.0.26100.4349`)** on the **Samsung Galaxy S23 (`dm1q` / `SM-S911B`, Snapdragon 8 Gen 2 for Galaxy / `SM8550-AC`)**.

> [!CAUTION]
> **Hardware & OS Safety Rules (Read First):**
> 1. **NEVER write to `sdb` or `sde`** — hardware Power-On Write-Protected (`ro=1`). Windows goes on `sda43` (`/dev/block/by-name/win`), UEFI boot image on `sda25`.
> 2. **Keep `SanPolicy = 4`** in `HKLM\SYSTEM\ControlSet001\Services\partmgr\Parameters` on the phone so Android UFS LUNs stay offline in Windows.
> 3. **Keep `testsigning on`** (`bcdedit`) — required to load test-signed `s23*.sys` drivers (`CN=S23TouchTest2020`). The desktop **Test Mode** watermark is expected and must not be turned off.
> 4. **Never call `Set-Date`** in live Windows scripts on the phone — UEFI RTC initializes at `2024-04-01`; jumping the clock forward immediately triggers Windows Power Manager's display idle-off timer (black screen).
> 5. **Avoid WHEA `0x124` SError Data Aborts** — never touch TZ-reserved QUP/TLMM pins (`GPIO36..39`, `50..51`, `56..59`, `64..65`; `QUP1_SE2`, `QUP2_SE0`, `QUP2_SE2`), non-APPS SMEM partitions, un-gated KGSL `0x03D00000` while `GPUCC_GX_GDSCR` bit 31 is 0, un-gated GMU `0x03D6A000` while `GPUCC_CX_GDSCR` is collapsed, or live PIL DDR carveouts before checking `IMEM ImageLoadInfo (0x146AA94C)`.

---

## Hardware & Driver Bring-Up Matrix

| Subsystem | Component / Path | Status | Verified Details |
|---|---|---|---|
| **UEFI & Display** | [`display_research/S23DisplayFixDxe_v3.c`](display_research/S23DisplayFixDxe_v3.c) | **Working** | `Mu-dm1q-pcie0-dispfix.img` flashed to `sda25`: 5-reg `DISP_CC` G116Gate (`0x0AF00000`), live DSI0 BTA readback (`0x0A` / `0x52`), S6E3FAC TPG-FIFO DCS wake, `MDP_INTF_1_TEAR_INT_COUNT (0x36298)` heal. |
| **Windows 11 Shell** | System resource ACLs (`C:\Windows\rescache`, etc.) | **Working** | Fixed `0x80070005` (`E_ACCESSDENIED`) in `MrmCoreR.dll` by restoring `Users`, `Authenticated Users`, `ALL APPLICATION PACKAGES`, and `ALL RESTRICTED APPLICATION PACKAGES` RX permissions; Settings (`ms-settings:`), Start Menu, and ShellExperienceHost run cleanly. |
| **Bluetooth 5.3 + BLE** | [`drivers/s23bthx/s23bthx.c`](drivers/s23bthx/s23bthx.c), [`s23bthx.inf`](drivers/s23bthx/s23bthx.inf) | **Working (Live)** | `v1.0.0.4` ARM64 WDM `BthMini.NT` LowerFilter over `QCA6490` `QUP2_SE6` (`0x00898000` / `UART14`) @ `3,200,000` baud with IBS filtering, `ScoSupportHCIBypass=2`, `MaxScoChannels=1`, and `0x0C6D` `Simultaneous_LE_Host` quirk. Classic + BLE scanning verified live (`BD_ADDR = 02:23:53:23:64:90`). |
| **Touchscreen** | [`drivers/s23touch/`](drivers/s23touch/) | **Deployed** | ARM64 WDM HID Multitouch minidriver (`QUP1 SE4` GENI SPI @ `0x00A90000`, `gpio44..47`, `gpio25` IRQ). |
| **Physical Keys** | [`drivers/s23keys/`](drivers/s23keys/) | **Deployed** | ARM64 WDM HID minidriver (`ROOT\S23KEYS`). |
| **Battery** | [`drivers/s23batt/`](drivers/s23batt/) | **Deployed** | ARM64 WDM Battery Miniclass driver (`MAX77705` on `QUP2 SE6`, `gpio206`/`gpio207`). |
| **USB-C Link Control** | [`s23link.py`](s23link.py) | **Working** | PC-to-phone WinUSB control link (`RM9_WINUSB=1 python s23link.py ping\|exec\|put\|get`). |
| **Wi-Fi 6E (`QCA6490`)** | [`wifi_research/qca6490_bringup/`](wifi_research/qca6490_bringup/), [`drivers/s23pcieprobe/`](drivers/s23pcieprobe/) | **Bring-Up / Staged** | `PCI\VEN_17CB&DEV_1103` INTx/MSI packaging (`build_s23_qcwlan_pkgs.ps1`), UEFI DXE (`s23_uefi_pcie0_dxe.c`), ACPI SSDT (`s23_pcie0_wlan.asl`), and kernel probe (`s23pcieprobe`). |
| **Audio & Haptics** | [`drivers/s23sensprobe/`](drivers/s23sensprobe/), [`audio_sensors_research/`](audio_sensors_research/) | **Probed Live** | Dual `CS35L43` speaker amps on `I2C_HUB_SE6` (`0x00998000`, `GPIO8/9`, rst `GPIO42`/`GPIO55`, `@0x40`/`@0x41` `DEVID=0x00035A42 REVID=0xA1`) + `CS40L26A` haptics on `I2C_HUB_SE3` (`0x0098C000`, `@0x40` `DEVID=0x0040A260 REVID=0xA1`). |
| **Adreno 740 GPU** | [`drivers/s23gpuprobe/`](drivers/s23gpuprobe/), [`gpu_research/`](gpu_research/) | **Stage 5B Verified (ITCM/DTCM)** | `s23gpuprobe.sys v1.0.0.5` (`StageMask=0x1B` & `0x3B`): RPMh ARC votes (`1–48 us`), `GPUCC_CX_GDSCR=0xF822F800`, locked `PLL0`/`PLL1` (`0xE0000005`), Samsung TZ `SCM` (`SET_CP_POOL_SIZE` `0x02000C1B` + `PAS` `0x02000201..207` available), fault-free `KGSL-SMMU` (`0x03DA0000`, `sGFSR=0`), pulled S23 stock `gmu_gen70200.bin` (`66,844 B`, `1a2a419c...`) & `a740_sqe.fw` (`76,724 B`, `96fee336...`) matching `G191_STOCK_GMU_SHA256` / `G433_SQE_SHA256` bit-for-bit, and verified live `ITCM`/`DTCM` load with `0` mismatches before `ICACHE` gates on `KGSL-SMMU` CB5 page tables. [`s23_gpu_acpi.asl`](gpu_research/s23_gpu_bringup/s23_gpu_acpi.asl) compiles cleanly (`0 Errors, 0 Warnings`, `744 B` AML). |
| **5G Modem / PIL / Perf** | [`drivers/s23pilprobe/`](drivers/s23pilprobe/), [`drivers/s23perf/`](drivers/s23perf/), [`modem_5g_research/`](modem_5g_research/) | **Probed / ACPI** | XPU-hardened SMEM/IMEM/PIL telemetry (`s23pilprobe`), `s23_subsys_pil.asl` ACPI SSDT, and EPSS CPUfreq / TSENS v2 41-sensor probe (`s23perf`). |
| **ACL Recovery** | [`drivers/s23aclfix/`](drivers/s23aclfix/), [`drivers/s23aclsvc/`](drivers/s23aclsvc/) | **Utility** | Early-boot WDM & LocalSystem one-shot NTFS ACL repair helpers. |

---

## Building ARM64 Kernel Drivers

All `drivers/s23*` kernel drivers are built via standalone WSL `build_wsl.py` scripts using Clang + `lld-link` targeting Windows ARM64, then verified and Authenticode-signed on Windows:

1. **Prerequisites:**
   - **WSL:** `clang-18`, `llvm-dlltool`, `lld-link` (`lld`), `acpica-tools` (`iasl`), Python 3.
   - **Windows Host:** PowerShell signing helper with test-signing certificate `CN=S23TouchTest2020`, `signtool` / `inf2cat` (WDK `10.0.26100.6584` or SDK), and Python 3.
2. **Build, Sign & Verify (Example: `s23bthx` or `s23gpuprobe`):**
   ```powershell
   # Compile ARM64 .sys in WSL (freestanding + -Wall -Wextra -Werror + .reloc + CheckSum fixup)
   wsl python3 /mnt/c/<repo-path>/drivers/s23bthx/build_wsl.py

   # Sign driver & generate catalog on Windows
   powershell -ExecutionPolicy Bypass -File .\drivers\s23bthx\sign_and_cat.ps1

   # Verify PE machine type (0xAA64 ARM64) and resolve all imports against phone's ntoskrnl exports
   python .\drivers\s23gpuprobe\verify_pe.py .\drivers\s23bthx\pkg\s23bthx.sys <path-to-ntoskrnl_phone.exe>
   ```
   > [!NOTE]
   > Always verify and deploy the **signed** `pkg\*.sys` binary (never unsigned `out\*.sys`), because `verify_pe.py` also checks the embedded Authenticode signature. If compiling manually with Visual Studio, use **`Hostx64\arm64`** (`cl.exe` / `link.exe`), never `Hostx64\x64`.

3. **Validate ACPI SSDTs:**
   ```bash
   cp gpu_research/s23_gpu_bringup/s23_gpu_acpi.asl /tmp/
   iasl -tc /tmp/s23_gpu_acpi.asl
   ```

---

## Firmware Extraction via Lineage Recovery (No Blobs Included)

Proprietary Samsung / Qualcomm firmware binaries, partition backups, logs, and compiled binaries are excluded from this repository by `.gitignore`. Extract them directly from your own Galaxy S23 over **Lineage Recovery** (strictly read-only `-o ro` mounts):

1. **Adreno 740 GPU Firmware (for `s23gpuprobe` Stage 5B & WDDM bring-up):**
   Boot the S23 into **Lineage Recovery** (`Power + VolUp` over USB) and run:
   ```powershell
   python gpu_research/s23_gpu_bringup/fw/pull_s23_gpu_fw.py
   ```
   This parses the `0PLA` `LpMetadataHeader` from `/dev/block/by-name/super`, mounts `vendor` read-only (`losetup -r` + `erofs`, with `lptools_new` fallback), pulls all 8 files (`gmu_gen70200.bin`, `a740_sqe.fw`, `a740_zap.*`) into `gpu_research/s23_gpu_bringup/fw/s23_vendor_gpu_fw/`, and validates them with [`extract_and_verify_s23_gpu_fw.py`](gpu_research/s23_gpu_bringup/fw/extract_and_verify_s23_gpu_fw.py) (`EXACT MATCH` against `G191_STOCK_GMU_SHA256` & `G433_SQE_SHA256`).
2. **QCA6490 Wi-Fi 6E & Bluetooth 5.3 Firmware:**
   With the phone in **Lineage Recovery**, see [`wifi_research/pull_boot24_and_wifi_fw.py`](wifi_research/pull_boot24_and_wifi_fw.py) and [`wifi_research/qca6490_bringup/QCA6490_DRIVER_AND_FW_GUIDE.md`](wifi_research/qca6490_bringup/QCA6490_DRIVER_AND_FW_GUIDE.md).
3. **5G Modem, ADSP/CDSP/SPSS, `core_nhlos_a.img`, and Audio/Haptics Firmware:**
   With the phone in **Lineage Recovery**, run:
   ```powershell
   python modem_5g_research/pull_s23_5g_and_subsys_fw.py
   ```
   This mounts `modem`, `apnhlos`, `dsp`, and `super/vendor` read-only (`-o ro`) and extracts `modem_image/` (`modem.mdt`, `modem_dtb.mdt`, `ipa_fws.mdt`, `fw_ipa_gsi_5.5_w.elf`, `mcfg_hw/sw.mbn`), `apnhlos_image/` (`adsp`/`cdsp`/`spss1p`), `dsp_image/`, raw `core_nhlos_a.img` (`_FVH` UEFI FV), and `CS35L43`/`CS40L26A` audio/haptics firmware into gitignored directories.

---

## Repository Layout

- `drivers/s23*` — Native ARM64 WDM drivers & diagnostic kernel probes (`s23bthx`, `s23touch`, `s23keys`, `s23batt`, `s23gpuprobe`, `s23btprobe`, `s23sensprobe`, `s23pcieprobe`, `s23pilprobe`, `s23perf`, `s23disp_diag`, `s23aclfix`, `s23aclsvc`).
- `display_research/` — `S23DisplayFixDxe_v2.c` / `v3.c` and MDSS/DSI0 auto-refresh patches.
- `gpu_research/` — Adreno 740 (`GEN7_2_1`) hardware architecture report, ACPI SSDT (`s23_gpu_acpi.asl`), UEFI DXE, and Lineage Recovery firmware tooling.
- `bt_research/` — QCA6490 UART14 (`0x00898000`) bring-up documentation and ACPI SSDT (`s23_bt_uart14.asl`).
- `wifi_research/` — QCA6490 PCIe0 (`17CB:1103`) bring-up guide, UEFI DXE, ACPI SSDT, and INF/package builder.
- `audio_sensors_research/` — `CS35L43` / `CS40L26A` / QUPv3 GENI bus map.
- `modem_5g_research/` & `modem_research/` — SM8550 PIL/SMEM/IPCC documentation and ACPI SSDTs.
- `s23link.py` — USB-C WinUSB remote shell & file transfer client.
