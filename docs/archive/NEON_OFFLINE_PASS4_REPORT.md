# Samsung Galaxy S23 (`dm1q` / `SM-S911B`, `SM8550-AC`) — Offline Windows 11 ARM64 Pass 4 Report

**Date:** 2026-11-21  
**Mode:** 100% Offline (zero `adb`, zero `fastboot`, zero `s23link.py`, zero edits to `backups\*`, `Mu-dm1q-*.img`, `s23link.py`, or `drivers\s23pcieprobe\*`)  
**Signing Certificate:** `CN=S23TouchTest2020` (Thumbprint `05DBD92557F4FC52EF2A5E5955789D8EED637C68`, SHA-256)

---

## Executive Summary

| Task | Deliverable | Verification Status |
|---|---|---|
| **Task 1** | Patched & catalog-signed S23 `qcwlan8280` Wi-Fi 6E driver packages (`pkg_s23_qcwlan_intx\` & `pkg_s23_qcwlan_msi\`) + repeatable builder `build_s23_qcwlan_pkgs.ps1` | Both catalogs (64 members, `OS=10_ARM64,10_X64`, `HWID1/HWID2` for `17CB:1103` + `SUBSYS_010817CB`): **`Test-FileCatalog: Valid` (SHA-256)**; Qualcomm EV signature on `qcwlan8280.sys` preserved untouched |
| **Task 2** | `s23btprobe.sys v1.0.0.2` — Stage 3 (`StageMask & 0x08`) QCA6490 Rampatch TLV (`hpbtfw21.tlv`) & NVM (`hpnv21g.bin`) downloader + BT 5.3 HCI verification suite + staged `pkg\` firmware & updated `install_s23btprobe.ps1` | `clang-18 -Wall -Wextra -Werror`: **0 warnings**; Signed SHA-256; **`verify_pe.py`: 11/11 `ntoskrnl_phone.exe` OK, `missing=[]`, `PE VERIFICATION: PASS`** |
| **Task 3** | Adreno 740 (`GEN7_2_1`) GMU firmware analysis + RSCC ucode extraction (`fw\s23_rsc_seq_ucode.bin`, `extract_and_verify_s23_gpu_fw.py`, `GMU_FW_ANALYSIS.md`) AND `s23gpuprobe.sys v1.0.0.4` read-only Stage 4 (`StageMask & 0x08`) | `s23_rsc_seq_ucode.bin`: **EXACT SHA-256 MATCH** with `G1287` `G482RscSha`; `s23gpuprobe.sys v1.0.0.4`: `clang-18 -Wall -Wextra -Werror` **0 warnings**, Signed SHA-256, **`verify_pe.py`: 10/10 `ntoskrnl_phone.exe` OK, `missing=[]`, `PE VERIFICATION: PASS`** |

---

## Task 1 — S23 `qcwlan8280` Wi-Fi 6E Driver Packages (`wifi_research\qca6490_bringup\`)

- **Builder:** `wifi_research\qca6490_bringup\build_s23_qcwlan_pkgs.ps1`
- **Firmware Injection (both packages):**
  - Replaced `bdwlan.elf` with S23 stock `wifi_research\s23_vendor_wifi_fw\qca6490\bdwlan.elf` (`59,900 B`, MD5 `916a72b57f9fc2c6140ea982fb60d9a2`).
  - Replaced `bdwlang.elf` **and** `bdwlan.elf.g` (confirmed byte-identical alias in stock Qualcomm package; critical for S23's GF silicon `SocID=0x400C1211`) with S23 stock `bdwlang.elf` (`59,900 B`, MD5 `b4550c79e9b462dd5706d3679d622658`).
  - Preserved Qualcomm's Windows-ABI `wlanfw20.mbn` (`4,878,336 B`), `m320.bin` (`266,684 B`), `Data20.msc` (`738,720 B`), and EV-signed `qcwlan8280.sys` (`3,287,440 B`).
- **Exact `qcwlan8280.inf` Patches (UTF-16LE BOM + CRLF preserved):**
  - `[QcWlan_LITHIUM.ndi.NTARM64.HW]`: `Needs = PciASPMOptIn.HW` (`PciD3ColdSupported.HW` removed).
  - `[D0PC.reg]`: explicit `HKR, , D0PCEnable, 0, "0"` added alongside `Default,,"0"`.
  - `[wowEnable.reg]`: `wowEnable = 0`.
  - `[S0Idle.reg]`: `S0IdleEnable = 0`.
  - `[QcWlan.Service.AddReg]`: `DmaRemappingCompatible = 0` (SMMU-500 Stage-1 identity pass-through already active on `SMR[6]=0x80001401`).
  - `[MSI.reg]`: `MSISupported = 0` in **`pkg_s23_qcwlan_intx\`** (Legacy INTA -> GSI 181); `MSISupported = 1` in **`pkg_s23_qcwlan_msi\`** (for testing GIC ITS on endpoint `0x1401`).
- **Catalogs:** Rebuilt via WinTrust v2 `New-FileCatalog`, augmented with `OS=10_ARM64,10_X64`, `HWID1=pci\ven_17cb&dev_1103`, `HWID2=pci\ven_17cb&dev_1103&subsys_010817cb`, and per-member `OSAttr=2:10.0`, signed SHA-256 with `CN=S23TouchTest2020`, and validated via `Test-FileCatalog` (`Status = Valid`, 64 members).

---

## Task 2 — `s23btprobe.sys v1.0.0.2` QCA6490 TLV/NVM Downloader (`drivers\s23btprobe\`)

Preserves all live-verified fixes (`GPIO204` `EGPIO_ENABLE=BIT12` `0x13C3`/`0x1201`, `gcc_qupv3_wrap_2_s_ahb_clk` `.hwcg_bit=1` handling, and one-shot `0xC0000001` return for `sc start`) and adds **Stage 3 (`StageMask & 0x08`)**:

- **Critical Hardware & Protocol Fixes Verified Against S23 `btqca.c`, `hci_qca.c`, `hpbtfw21.tlv`, `hpnv21g.bin`, and `s23btprobe_s3_live.log`:**
  1. **TX FIFO Back-Pressure (`GeniWaitTxFifoSpace`):** Live S23 UART14 reports `txDepth = 48` words (`192 B` total FIFO!). `GeniTxPacket()` now polls `SE_GENI_TX_FIFO_STATUS < txDepth - 2` before every 32-bit word so 246-byte `0xFC00/0x1E` TLV chunks never overflow the 48-word FIFO.
  2. **QCA6490 `download_mode == 3` (`QCA_SKIP_EVT_VSE_CC`):** Parsed live from `hpbtfw21.tlv` (`tlv_type=1, total_size=171644, data_len=171608, prod_id=0x0013, rom_build=0x0201, patch_ver=0x7A6B, download_mode=3`). Intermediate `hpbtfw21.tlv` segments skip ACK wait (only final segment waits for `04 0E 05 01 00 FC 00 1E`), whereas `hpnv21g.bin` (`TLV_TYPE_NVM`, `dnldMode=0`) verifies Command Complete on every segment.
  3. **Exact 12-Byte `struct tlv_type_nvm` In-Memory Patching (`hpnv21g.bin`, 65 tags):**
     - **Tag 2 (`BD_ADDR`):** Replaces QCA placeholder `AD 5A 00 00 00 00` with clean unicast locally-administered MAC `02:23:53:23:64:90`.
     - **Tag 17 (`EDL_TAG_ID_HCI`, stock `15 11 22 00 2E 00`):** Clears IBS (`data[0] &= ~0x80`) and patches **`data[1]`** (`QCA_WCN6855` baud byte, per `btqca.c` L384) to match active baud (`0x00` for `115,200` when `UseHighBaud=0`, `0x11` for `3,200,000` when `UseHighBaud=1`).
     - **Tag 27 (`EDL_TAG_ID_DEEP_SLEEP`, stock `01 01 ...`):** Clears bit 0 (`data[0] &= ~0x01`) so controller stays awake on UART without IBS wakeup bytes.
  4. **Selectable Baud (`UseHighBaud=0/1`):** Default `0` streams safely at `115,200` baud; `1` executes `hci_qca.c`'s exact `EDL_SET_BAUDRATE_CMD (0xFC48, 0x11)` + RTS deassert + `51.2 MHz` RCG (`CFG=0x00102601, M=0x0040, N=0xFEC8, D=0xFE88`, `SER_CLK=0x11` = `3,200,000` baud exact).
  5. **Zero Extra Heap Imports:** Uses static 180 KB `.bss` buffer (`g_FwBuf`) + `ZwReadFile` (`11/11` `ntoskrnl_phone.exe` imports resolved).
  6. **Post-Download Activation & BT 5.3 Verification Suite:** `QCA_DISABLE_LOGGING (0xFC17)` -> `HCI_Reset (0x0C03)` -> `EDL_GET_BUILD_INFO (0xFC00/0x20)` -> `HCI_Reset` -> `HCI_Read_Local_Version_Information (0x1001)` -> `HCI_Read_BD_ADDR (0x1009)` -> `HCI_Read_Local_Name (0x0C14)` -> `HCI_LE_Read_Local_Supported_Features (0x2003)`, with full decoded output.
- **Staged Package (`drivers\s23btprobe\pkg\`):** Includes signed `s23btprobe.sys`, `hpbtfw21.tlv`, `hpnv21g.bin`, `s23test.cer`, and updated `install_s23btprobe.ps1` (auto-copies both firmware files to `C:\Users\Public\`).

---

## Task 3 — Adreno 740 GMU Analysis & `s23gpuprobe.sys v1.0.0.4` Stage 4 (`drivers\s23gpuprobe\`, `gpu_research\s23_gpu_bringup\fw\`)

1. **Firmware Extraction & `G1287-cold-aperture` Comparison (`gpu_research\s23_gpu_bringup\fw\`):**
   - Verified S23 stock filenames from `/home/mahan/s23/src/vendor/qcom/opensource/graphics-kernel/adreno-gpulist.h` L2088–2091 (`GEN7_2_1`): **`gmu_gen70200.bin`**, **`a740_sqe.fw`**, **`a740_zap.mbn`**.
   - **Offline Status:** Neither `super.img`/`vendor.img` nor `gmu_gen70200.bin`/`a740_sqe.fw` exist offline on the host PC yet (`device_firmware\` is Galaxy A52 SM7125, ignored). Created `extract_and_verify_s23_gpu_fw.py` + `GMU_FW_ANALYSIS.md` with exact recovery pull commands and full block/SHA-256 validator.
   - **Verified Exact Match:** Extracted S23's 20-byte `GEN7_2_1` RSCC sequencer ucode (`0xEAAAE5A0, 0xE1A1EBAB, 0xA2E0A581, 0xECAC82E2, 0x0020EDAD`) into `fw\s23_rsc_seq_ucode.bin` — SHA-256 `20175df4f9aafab7f958205d71bf931a30a484bd6e94db348da5497a4e13ce90` matches `G1287` `G482RscSha` **bit-for-bit**.
   - **Explained Live `s23gpuprobe_s7_live.log`:** `GPUCC_GX_GDSCR=0x6822F800` stays collapsed until GMU CM3 boots + loads PDC/RSCC + receives HFI DCVS + `OOB_REQUEST (BIT 30)` on `GMU_H2G_SET`.
2. **`s23gpuprobe.sys v1.0.0.4` — Read-Only Stage 4 (`StageMask & 0x08`):**
   - Preserves live-proven `StockAhbRawReady` (`GPUCC_AHB_CBCR` non-sticky bit 0) CX gate and Stages `0x01/0x02/0x04`.
   - Strictly gated on `GpuCxAxiGateOpen()`, Stage 4 dumps:
     - **GMU ITCM (`0x03D6D000`, 16 KB) & DTCM (`0x03D71000`, 16 KB):** First 4 vector/data words, 4096-word non-zero/`0xFFFFFFFF` scan, and `GMU_DTCM_VERSION (0x03D74FE0)`.
     - **`CX_MISC` (`0x03D9E000`):** `CX_MISC_TCM_RET_CNTL (0x03D9E0E4)`.
     - **GMU Cache / HFI / AO RSCC Bridge (`0x03D6A000`):** `ICACHE/DCACHE/SYS_BUS_CONFIG`, `RPMH_HYST/CTRL`, `HFI_QTBL_INFO/ADDR`, `G2H_INTR_MASK`, `GENERAL_8/9`, `AO_HOST_INTR_MASK`, `GMU_RSCC_CONTROL_REQ/ACK (0x03D8EC1C/20)`, `AO_AHB_FENCE_RANGE_0`.
     - **Verified `G482_RSCC` (`0x03D50000`, NOT `0x03D8C000` which is `isense_cntl`):** `STATUS0 (0x10)`, `PDC_SEQ/MATCH/SLAVE_ID (0x20..0x2C)`, `OVERRIDE_START (0x400)`, `SEQ_BUSY (0x404)`, `SEQ_MEM_0..4 (0x550..0x560)`, `TCS0..3_STATUS (0xD28/0xFC8/0x1268/0x1508)`.
     - **Verified `GMU_PDC` (`0x0B280000`):** `STATUS_1024 (0x1024)`, `ENABLE_PDC (0x4500)`, `MATCH_LO/HI (0x4510/14)`, `SEQ_START_ADDR (0x4520)`.
     - **Verified `GPUCC` `GX` / `ACD` / `MEMNOC` (`0x03D90000`):** `GX_BCR/GDSCR/CFG_GDSCR/MISC3`, `GX_GFX3D/VSENSE/GMU/FF/RDVM`, `ACD_BCR (0x9358)`, `GFX3D_AON_BCR (0x9198)`, `MND1X_0/1`, `SLEEP`, `FREQ_MEAS`, `DEMET`, `MEMNOC_GFX`, and GCC `MEMNOC`/`SNOC_DVM`/`DDRSS_AXI`.

---

## Complete Pass 4 Artifact Inventory & Checksums

| File (relative to `s23_dm1q\`) | Size | MD5 | Verification |
|---|---:|---|---|
| `wifi_research\qca6490_bringup\build_s23_qcwlan_pkgs.ps1` | 10,287 B | `fd98edcb2f1f4b90f60b54a7296903f3` | Executed cleanly (2 packages built) |
| `wifi_research\qca6490_bringup\pkg_s23_qcwlan_intx\qcwlan8280.inf` | 34,256 B | `4e4374edcf220086741260eef2558335` | UTF-16LE BOM, `MSISupported=0` |
| `wifi_research\qca6490_bringup\pkg_s23_qcwlan_intx\qcwlan8280.cat` | 15,530 B | `dadab0e2f7db250560793ac645c47e45` | Signed SHA-256; `Test-FileCatalog: Valid` |
| `wifi_research\qca6490_bringup\pkg_s23_qcwlan_msi\qcwlan8280.inf` | 34,256 B | `411af5ada19a991db6a517a3e23c7f5a` | UTF-16LE BOM, `MSISupported=1` |
| `wifi_research\qca6490_bringup\pkg_s23_qcwlan_msi\qcwlan8280.cat` | 15,530 B | `42aeb11e7a5b15917d9ebebc2e22da53` | Signed SHA-256; `Test-FileCatalog: Valid` |
| `wifi_research\qca6490_bringup\pkg_s23_qcwlan_*\bdwlan.elf` | 59,900 B | `916a72b57f9fc2c6140ea982fb60d9a2` | S23 stock board data |
| `wifi_research\qca6490_bringup\pkg_s23_qcwlan_*\bdwlang.elf` (+ `.elf.g`) | 59,900 B | `b4550c79e9b462dd5706d3679d622658` | S23 stock GF board data |
| `drivers\s23btprobe\s23btprobe.c` | 79,828 B | `1458d11841237e35659f70fc076a64b8` | `v1.0.0.2`, `-Wall -Wextra -Werror` |
| `drivers\s23btprobe\build_wsl.py` | 2,417 B | `bea1f2b4c6bbe48114f217b439757d38` | Added `ZwReadFile` export |
| `drivers\s23btprobe\pkg\s23btprobe.sys` | 76,168 B | `78975e76c3d7c1934ae82c04093d4833` | Signed SHA-256; **11/11 OK, PE PASS** |
| `drivers\s23btprobe\pkg\hpbtfw21.tlv` | 171,680 B | `e4d5244b5257720b2c4d2a4267e6414a` | S23 stock BT rampatch TLV |
| `drivers\s23btprobe\pkg\hpnv21g.bin` | 6,452 B | `36a5b0c16769f4ac7db8f9906949a6aa` | S23 stock GF NVM (65 tags) |
| `drivers\s23btprobe\pkg\install_s23btprobe.ps1` | 2,816 B | `bc2e9817b8fb0598f4ca700a784ad1b2` | Stages `.sys` + `.tlv`/`.bin` |
| `gpu_research\s23_gpu_bringup\fw\s23_rsc_seq_ucode.bin` | 20 B | `82877e8a42509a2962eea9485c16ba68` | SHA-256 exact match `G482RscSha` |
| `gpu_research\s23_gpu_bringup\fw\extract_and_verify_s23_gpu_fw.py` | 5,866 B | `10f1ffce039e599134c5a2ed87552fa3` | `py_compile` + run PASS |
| `gpu_research\s23_gpu_bringup\fw\GMU_FW_ANALYSIS.md` | 8,720 B | `f9d0f3dc0f9b6c6b6a1a952fb0c42722` | Complete `GEN7_2_1` vs `G1287` report |
| `drivers\s23gpuprobe\s23gpuprobe.c` | 75,290 B | `97b6ea80e1dd07ba328df9663b711b97` | `v1.0.0.4`, `-Wall -Wextra -Werror` |
| `drivers\s23gpuprobe\pkg\s23gpuprobe.sys` | 73,096 B | `a70381ac652ab508d9f22796973665c3` | Signed SHA-256; **10/10 OK, PE PASS** |

---

## Ready-to-Run Live Commands for the Active Operator

1. **Install S23 QCA6490 Wi-Fi 6E Driver (INTx first, MSI optional test):**
   ```powershell
   pnputil /add-driver .\pkg_s23_qcwlan_intx\qcwlan8280.inf /install
   ```
2. **Test Live QCA6490 Bluetooth 5.3 Rampatch + NVM Download (`s23btprobe v1.0.0.2`):**
   ```powershell
   powershell -ExecutionPolicy Bypass -File .\install_s23btprobe.ps1
   # Safe 115200 baud full download (Wi-Fi rails already up -> 11; standalone rails -> 15):
   reg add HKLM\SYSTEM\CurrentControlSet\Services\s23btprobe\Parameters /v StageMask /t REG_DWORD /d 11 /f
   sc.exe start s23btprobe
   Get-Content C:\Users\Public\s23btprobe.log
   # Optional high-speed 3.2 Mbps mode:
   reg add HKLM\SYSTEM\CurrentControlSet\Services\s23btprobe\Parameters /v UseHighBaud /t REG_DWORD /d 1 /f
   ```
3. **Run Read-Only Adreno 740 Stage 4 Cold-Preflight Dump (`s23gpuprobe v1.0.0.4`):**
   ```powershell
   Copy-Item .\s23gpuprobe.sys C:\Windows\System32\drivers\s23gpuprobe.sys -Force
   reg add HKLM\SYSTEM\CurrentControlSet\Services\s23gpuprobe\Parameters /v StageMask /t REG_DWORD /d 11 /f
   sc.exe start s23gpuprobe
   Get-Content C:\Users\Public\s23gpuprobe.log
   ```
