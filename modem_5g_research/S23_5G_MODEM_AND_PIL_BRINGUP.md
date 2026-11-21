# Samsung Galaxy S23 (`dm1q` / `SM-S911B`, `SM8550-AC`) — Snapdragon X70 5G Modem (`MSS`) & Remoteproc PIL (`ADSP`, `CDSP`, `SPSS`) Architecture Blueprint

**Target Device:** Samsung Galaxy S23 (`dm1q` / `SM-S911B`, Bootloader `S911BXXS8DYI3`, SoC `SM8550-AC` / Qualcomm `Kalama` / `Kailua` + integrated Snapdragon X70 5G Modem)  
**Target OS:** Windows 11 Pro 24H2 ARM64 (`10.0.26100.4349`)  
**Verification Mode:** 100% Offline, Silicon-Verified Directly Against Authoritative S23 Sources:
- Live S23 Flattened Device Tree: `/home/mahan/mu_s23/s23_live.dts`
- Samsung S23 Kernel Source (`msm-kernel`):
  - `drivers/remoteproc/{qcom_q6v5_pas.c,qcom_q6v5_mss.c,qcom_spss.c,qcom_pil_info.c}`
  - `drivers/firmware/{qcom_scm.c,qcom_scm.h,qcom_scm-smc.c}`
  - `drivers/soc/qcom/{smem.c,socinfo.c,smp2p.c,cmd-db.c}`
  - `drivers/rpmsg/{qcom_glink_smem.c,qcom_glink_native.c}`
  - `drivers/mailbox/qcom-ipcc.c` & `include/dt-bindings/mailbox/qcom-ipcc.h`
  - `drivers/clk/qcom/{gcc-kalama.c,clk-rpmh.c,debugcc-kalama.c}`
- Active `Mu-Silicium` UEFI Platform Config (`dm1qPkg`):
  - `Platforms/Samsung/dm1qPkg/Library/MemoryMapLib/MemoryMapLib.c`
  - `Platforms/Samsung/dm1qPkg/Include/{DXE.inc,APRIORI.inc}`
  - `Binaries/dm1q/RawFiles/uefipil.cfg` & `Binaries/dm1q/QcomPkg/Drivers/{PILDxe,PILProxyDxe,SPSSDxe}/`

---

## 1. Remoteproc Subsystems Master Hardware Table (`s23_live.dts` + `qcom_q6v5_pas.c` + `uefipil.cfg`)

All four Hexagon / Secure co-processor subsystems on S23 (`dm1q`) use Qualcomm Peripheral Authentication Service (PAS) ELF64 split-image loading (`*.mdt` + `*.bNN`) with separate DTB authentication on `MSS`, `ADSP`, and `CDSP`:

| Subsystem | DTS Node & Compatible | MMIO Base (Size) | PAS ID (Main / DTB) | Firmware & `uefipil.cfg` Target | Watchdog / IRQ (`SPI` -> `ACPI GSI = SPI+32`) | RPMh Clocks & Rails (`s23_live.dts`) | Interconnects & AOSS QMP | SMEM Crash Reason / Minidump / SSCTL |
|---|---|---|---|---|---|---|---|---|
| **Snapdragon X70 5G Modem (`MSS` / `MPSS`)** | `remoteproc-mss@04080000`<br>`qcom,kalama-modem-pas` | `0x04080000` (`0x10000`) | **`4`** / **`0x26` (38)** | `modem.mdt` + `modem_dtb.mdt`<br>(`FULL_MODEM` / `FULL_MODEM_DTB`) | WDOG: GIC `SPI 0x108` (264) -> **`GSI 296` (`0x128`)** (Edge Rising)<br>SMP2P: `fatal`, `handover`, `ready`, `stop-ack`, `shutdown-ack` | `xo` (`xo.lvl`), `cx` (`pm_v6e_s6_level`, `0x180`), `mx`/`mss` (`pm_v8_s7_level`, `0x140`) | `rproc_ddr`, `crypto_ddr`; `qcom,signal-aop` (`qmp_name="modem"`) | SMEM `421`, Minidump `3`, SSCTL `0x12`, `needs_dsm_mem_setup=true`, `dma_phys_below_32b=true` |
| **Audio & Sensors DSP (`ADSP` / `LPASS+SLPI`)** | `remoteproc-adsp@03000000`<br>`qcom,kalama-adsp-pas` | `0x03000000` (`0x10000`) | **`1`** / **`0x24` (36)** | `adsp.mdt` + `adsp_dtb.mdt`<br>(`FULL_ADSP` / `FULL_ADSP_DTB`) | WDOG: PDC pin `6` (`0x47 0x06`, `qcom,pdc-ranges` base `0x1E0 + 6 = SPI 486` -> **`GSI 518` (`0x206`)**)<br>SMP2P: `fatal..shutdown-ack` | `xo` (`xo.lvl`), `cx` (`pm_v6d_s4_level`, `0x180`), `mx` (`pm_v6d_l2_level`, `0x180`), `sensor_vdd` (`pm_humu_l1`, `1.8 V`) | `rproc_ddr`, `crypto_ddr`; `qcom,signal-aop` (`qmp_name="adsp"`) | SMEM `423`, Minidump `5`, SSCTL `0x14` |
| **Hexagon NPU / Compute DSP (`CDSP` / `NSP`)** | `remoteproc-cdsp@32300000`<br>`qcom,kalama-cdsp-pas` | `0x32300000` (`0x10000`) | **`18` (`0x12`)** / **`0x25` (37)** | `cdsp.mdt` + `cdsp_dtb.mdt`<br>(`FULL_CDSP` / `FULL_CDSP_DTB`) | WDOG: GIC `SPI 0x242` (578) -> **`GSI 610` (`0x262`)** (Edge Rising)<br>SMP2P: `fatal..shutdown-ack` | `xo` (`xo.lvl`), `cx` (`pm_v6e_s6_level`, `0x180`), `mx` (`pm_v8_s5_level`, `0x180`), `nsp` (`pm_v8_s1_level`, `0x180`) | `rproc_ddr`, `crypto_ddr`; `qcom,signal-aop` (`qmp_name="cdsp"`) | SMEM `601`, Minidump `7`, SSCTL `0x17` |
| **Secure Processor (`SPSS` / `SPU`)** | `remoteproc-spss@1880000`<br>`qcom,kalama-spss-pas` | `0x0188101C..0x01882014` (`sp2soc_irq_*`, `rmb_*`), GLink `0x01885008..0x01885013`, SCSR debug `0x01886020..0x0188C020` | **`14` (`0x0E`)** | `\image\spss1t` (`SPSS`) | Generic IRQ: GIC `SPI 0x160` (352) -> **`GSI 384` (`0x180`)** (Edge Rising) | `xo` (`xo.lvl`), `cx` (`pm_v6e_s6_level`, `0x180`) | `crypto_ddr`; `qcom,signal-aop`; SCSR bits `24, 25` | Loaded pre-boot by UEFI `SPSSDxe` -> `PILDxe` (`uefipil.cfg [SPSS]`) |

> [!IMPORTANT]
> **SM8550 (`Kalama`) Clock Architecture Finding (Verified in `gcc-kalama.c`, `clk-rpmh.c`, and `debugcc-kalama.c`):**
> Unlike older Snapdragon platforms (`SM8150`/`SM8250`), SM8550 **does NOT expose APPS-controlled GCC branch clocks (`GCC_MSS_CFG_AHB_CBCR`, `GCC_IPA_CBCR`, `GCC_LPASS_*`) in `gcc-kalama.c`!**
> - `MSS`, `ADSP`, `CDSP`, and `SPSS` only require RPMh **`xo.lvl`** (`RPMH_CXO_CLK`) plus their ARC voltage levels (`cx.lvl`, `mx.lvl`, `mss.lvl`, `nsp.lvl`, `lcx.lvl`, `lmx.lvl`) and BCM interconnect votes (`MC0`, `SH0`, `ACV`, `CE0`).
> - **`IPA` core clock** is an RPMh BCM resource (**`IP0`** via `RPMH_IPA_CLK = &sdm845_ipa.hw` in `clk-rpmh.c`), NOT a GCC CBCR (`measure_only_ipa_2x_clk` in `debugcc-kalama.c` is a debug-mux dummy!).
> - The APPS-visible GCC registers (`0x00100000`, `0x1F4200`) control `GPLL0/4/7/9`, `GCC_BOOT_ROM_AHB_CLK` (`0x38004`), `GCC_AGGRE_*` (`PCIe`/`UFS`/`USB3`), `GCC_CFG_NOC_*`, `GCC_CNOC_*`, `GCC_GPU_*`, `GCC_DISP_*`, `GCC_CAMERA_*`, `GCC_VIDEO_*`, and `GCC_QUPV3_*`.

---

## 2. Reserved DDR Carveouts & `PIL Reserved` Sub-Range Map (`s23_live.dts` vs `MemoryMapLib.c` vs `uefipil.cfg`)

Cross-referencing `s23_live.dts` lines 32..279 against `dm1qPkg/Library/MemoryMapLib/MemoryMapLib.c` and `Binaries/dm1q/RawFiles/uefipil.cfg` reveals the exact physical layout—plus **two critical discrepancies** that must be fixed before ever booting `MPSS` or `ADSP` under Windows:

| Region / DTS Node (`s23_live.dts`) | Physical Base | Size | End (Exclusive) | Parent `MemoryMapLib.c` Entry | Notes & Safety Status |
|---|---:|---:|---:|---|---|
| `gunyah_hyp_region@80000000` | `0x80000000` | `0x00A00000` (10 MiB) | `0x80A00000` | `HYP` (`MEM_RES`) | Gunyah EL2 hypervisor (**NEVER map/touch**) |
| `cpusys_vm_region@80a00000` | `0x80A00000` | `0x00400000` (4 MiB) | `0x80E00000` | `CPUSYS VM` (`MEM_RES`) | Gunyah VM (**NEVER map/touch**) |
| `aop_cmd_db_region@81c60000` | `0x81C60000` | `0x00020000` (128 KiB) | `0x81C80000` | Inside `AOP` (`0x81C00000`, `0xF4000`) | **CMD_DB** (`qcom,cmd-db`, read-only safe) |
| `chipinfo_region@81cf4000` | `0x81CF4000` | `0x00001000` (4 KiB) | `0x81CF5000` | `Chip Info` (`SYS_MEM`, `Reserv`) | XBL chip info structure (read-only safe) |
| **`smem_region@81d00000`** | **`0x81D00000`** | **`0x00200000` (2 MiB)** | **`0x81F00000`** | **`SMEM` (`MEM_RES`, `UNCACHEABLE`)** | **Qualcomm SMEM v12** (`$TOC` at `0x81EFF000`) |
| **`adsp_mhi_region@81f00000`** | **`0x81F00000`** | **`0x00020000` (128 KiB)** | **`0x81F20000`** | **`ADSP MHI` (`SYS_MEM`, `Reserv`)** | ADSP MHI shared region |
| `tz_stat_region@82700000` | `0x82700000` | `0x00100000` (1 MiB) | `0x82800000` | `TZ STATS` (`MEM_RES`) | TZ diagnostics (read-only) |
| *(PIL Pre-Modem Headroom)* | `0x89800000` | `0x01000000` (16 MiB) | `0x8A800000` | **`PIL Reserved` (`0x89800000`, `0x19280000` -> ends `0xA2A80000`)** | Note: DTS node name is `mpss_region@89800000`, but its actual `reg` starts at `0x8A800000`! (Legacy WLAN FW slot `wlan_fw_mem` is absent on S23 because QCA6490 is PCIe discrete with host-DMA firmware, NOT integrated WCSS!) |
| **`mpss_region@89800000` (`modem_mem`)** | **`0x8A800000`** | **`0x10800000` (264 MiB)** | **`0x9B000000`** | Inside `PIL Reserved` | X70 MPSS Hexagon Q6 ELF load (`PAS ID 4`) |
| **`q6_mpss_dtb_region@9b000000`** | **`0x9B000000`** | **`0x00080000` (512 KiB)** | **`0x9B080000`** | Inside `PIL Reserved` | MPSS DTB (`modem_dtb`, `PAS ID 0x26`) |
| **`ipa_fw_region@9b080000`** | **`0x9B080000`** | **`0x00010000` (64 KiB)** | **`0x9B090000`** | Inside `PIL Reserved` | IPA uC firmware (`ipa_fws`, `PAS ID 0x0F`) |
| **`ipa_gsi_region@9b090000`** | **`0x9B090000`** | **`0x0000A000` (40 KiB)** | **`0x9B09A000`** | Inside `PIL Reserved` | IPA GSI firmware (`ipa_gsi_fw`) |
| `gpu_micro_code_region@9b09a000` | `0x9B09A000` | `0x00002000` (8 KiB) | `0x9B09C000` | Inside `PIL Reserved` | Adreno 740 GPU microcode / ZAP |
| **`spss_region_region@9b100000`** | **`0x9B100000`** | **`0x00180000` (1.5 MiB)** | **`0x9B280000`** | Inside `PIL Reserved` | **SPSS (`spss1t`, `PAS ID 14`) — WARNING: XPU/Gunyah-locked if loaded by UEFI `SPSSDxe`!** |
| `spu_tz_shared_mem@9b280000` | `0x9B280000` | `0x00060000` (384 KiB) | `0x9B2E0000` | Inside `PIL Reserved` | SPU <-> TZ secure shared memory (**DO NOT probe**) |
| `spu_modem_shared_mem@9b2e0000` | `0x9B2E0000` | `0x00020000` (128 KiB) | `0x9B300000` | Inside `PIL Reserved` | SPU <-> Modem shared memory |
| `camera_region@9b300000` | `0x9B300000` | `0x00800000` (8 MiB) | `0x9BB00000` | Inside `PIL Reserved` | Camera ICP firmware |
| `video_region@9bb00000` | `0x9BB00000` | `0x00700000` (7 MiB) | `0x9C200000` | Inside `PIL Reserved` | Iris/Venus video firmware |
| `cvp_region@9c200000` | `0x9C200000` | `0x00700000` (7 MiB) | `0x9C900000` | Inside `PIL Reserved` | Computer Vision Processor firmware |
| **`cdsp_region@9c900000` (`cdsp_mem`)** | **`0x9C900000`** | **`0x02000000` (32 MiB)** | **`0x9E900000`** | Inside `PIL Reserved` | Hexagon NPU/CDSP ELF load (`PAS ID 18`) |
| **`q6_cdsp_dtb_region@9e900000`** | **`0x9E900000`** | **`0x00080000` (512 KiB)** | **`0x9E980000`** | Inside `PIL Reserved` | CDSP DTB (`cdsp_dtb`, `PAS ID 0x25`) |
| **`q6_adsp_dtb_region@9e980000`** | **`0x9E980000`** | **`0x00080000` (512 KiB)** | **`0x9EA00000`** | Inside `PIL Reserved` | ADSP DTB (`adsp_dtb`, `PAS ID 0x24`) |
| **`adspslpi_region@9ea00000` (`adsp_mem`)** | **`0x9EA00000`** | **`0x059B4000` (~89.7 MiB)** | **`0xA43B4000`** | **OVERFLOWS `PIL Reserved` (`0xA2A80000`)!** | **CRITICAL FINDING #1:** `adspslpi_region` (`0x9EA00000..0xA43B4000`) extends `0x1934000` (~25.2 MiB) PAST `PIL Reserved` (`0xA2A80000`) into `Display Demura` (`0xA2A80000..0xA5580000`) in `MemoryMapLib.c`! (Note `CORE_ADSP` in `uefipil.cfg` is smaller at `0x4080000` -> ends `0xA2A80000`!) Before booting `FULL_ADSP`, `Display Demura` overlap must be resolved! |
| **`mpss_dsm_region@d4d00000`** | **`0xD4D00000`** | **`0x03300000` (51 MiB)** | **`0xD8000000`** | **NOT YET CARVED OUT in `MemoryMapLib.c`!** | **CRITICAL FINDING #2:** `kalama_mpss_resource.needs_dsm_mem_setup = true` requires `0xD4D00000..0xD8000000` reserved `MEM_RES` before calling `PAS_MEM_SETUP` for MPSS! |
| `IMEM PIL ImageLoadInfo` | `0x146AA94C` | `200 B` (`10 x 20 B`) | `0x146AAA14` | `IMEM Cookie Base` (`0x146AA000`, `0x16000`) | Array of `{ char name[8]; le32 base_lo; le32 base_hi; le32 size; }` written by UEFI `PILDxe` / Linux `qcom_pil_info.c` |

---

## 3. Qualcomm PAS TZ SMC Calling Convention, Gunyah (`QHEE`), and UEFI `PILDxe` Pre-Boot Loading

### 3.1 Exact ARM64 SMCCC Calling Convention (`drivers/firmware/qcom_scm.h`, `qcom_scm.c`, `qcom_scm-smc.c`)

All Qualcomm PAS calls use **ARM SMCCC 64-bit Standard Calling Convention** with `Owner = ARM_SMCCC_OWNER_SIP (2)` and `Service = QCOM_SCM_SVC_PIL (0x02)`:
- Function ID in `X0`: `ARM_SMCCC_CALL_VAL(STD_CALL=0, SMC_64=1, SIP=2, (svc << 8) | cmd)` = **`0xC2000200 | cmd`** (or `0x82000200 | cmd` for 32-bit compatibility; Linux `qcom_scm-smc.c` uses `SMC_64` with `ARM_SMCCC_QUIRK_QCOM_A6` retry loop on `QCOM_SCM_INTERRUPTED` and wait-queue handling via `QCOM_SCM_SVC_WAITQ`).
- Argument descriptor in `X1`: `QCOM_SCM_ARGS(nargs, type0, type1, ...)` (`nargs` in bits `[3:0]`, 2-bit arg types in bits `[4..]`: `VAL=0`, `RO=1`, `RW=2`, `BUFVAL=3`).

| PAS Function (`qcom_scm.c`) | Service / Cmd | SMCCC64 `X0` (`FID`) | `X1` (`arginfo`) | Register Arguments (`X2`, `X3`, `X4`) | Purpose |
|---|---|---|---|---|---|
| `qcom_scm_pas_init_image` | `0x02` / `0x01` (`0x0201`) | `0xC2000201` | `QCOM_SCM_ARGS(2, VAL, RW)` = `0x00000022` | `X2 = pas_id`, `X3 = metadata_pa` (coherent DMA PA of `.mdt` ELF+hash header) | Authenticates RSA/ECDSA certificate chain & hash segment against Samsung OEM root-of-trust fuses in QTEE |
| `qcom_scm_pas_mem_setup` | `0x02` / `0x02` (`0x0202`) | `0xC2000202` | `QCOM_SCM_ARGS(3)` = `0x00000003` | `X2 = pas_id`, `X3 = start_pa`, `X4 = size` | Configures XPU / Gunyah stage-2 memory protection for subsystem carveout before segment loading |
| `qcom_scm_pas_auth_and_reset` | `0x02` / `0x05` (`0x0205`) | `0xC2000205` | `QCOM_SCM_ARGS(1)` = `0x00000001` | `X2 = pas_id` | Verifies SHA-256 hashes of all loaded ELF program segments in DDR, locks carveout to remote VMID, and releases Q6/SPU from reset |
| `qcom_scm_pas_shutdown` | `0x02` / `0x06` (`0x0206`) | `0xC2000206` | `QCOM_SCM_ARGS(1)` = `0x00000001` | `X2 = pas_id` | Halts remote processor, tears down XPU protection, and returns carveout to HLOS |
| `qcom_scm_pas_supported` | `0x02` / `0x07` (`0x0207`) | `0xC2000207` | `QCOM_SCM_ARGS(1)` = `0x00000001` | `X2 = pas_id` | Queries whether `pas_id` is supported by TZ PAS |
| `qcom_scm_pas_mss_reset` | `0x02` / `0x0A` (`0x020A`) | `0xC200020A` | `QCOM_SCM_ARGS(2)` = `0x00000002` | `X2 = reset (1=assert, 0=deassert)`, `X3 = 0` | Asserts/deasserts MSS reset controller |

### 3.2 Gunyah (`QHEE`) EL2 Policy & UEFI `PILDxe` Pre-Boot Reality on Samsung `dm1q`

1. **Does Samsung's Gunyah (`QHEE`, `qcom,gunyah-hypervisor-1.0`, HLOS `vmid = 3`) allow PAS SMC calls from EL1?**
   - **YES — with strict OEM signature & SHMBridge enforcement.** Android Linux runs in the exact same HLOS EL1 VM (`vmid = 3`) under Gunyah EL2 and boots `ADSP`, `CDSP`, and `MPSS` via `qcom_scm_pas_*` `SMC #0` calls.
   - **CRITICAL CAVEAT FOR WINDOWS EL1:** On SM8550 (`Kalama`), QTEE enforces **SHMBridge** (`ShmBridgeDxeLA` / `qtee_shmbridge`) for `QCOM_SCM_RW` buffers passed to `pas_init_image` (metadata PA must reside in a registered SHMBridge pool, or `ShmBridge` must be disabled/registered via `SCM_SVC_MP`), AND `pas_auth_and_reset` invokes Gunyah stage-2 ownership transfer (`hyp_assign` from `VMID_HLOS=3` to `VMID_MSS_MSA` / `VMID_LPASS` / `VMID_CDSP`). Any memory overlap with Windows kernel allocations causes an instant Gunyah EL2 panic (`900E` ramdump).
   - Furthermore, Samsung's `S911BXXS8DYI3` fuses require **Samsung's OEM-signed stock firmware images** (`modem.mdt`, `modem_dtb.mdt`, `adsp.mdt`, `adsp_dtb.mdt`, `cdsp.mdt`, `cdsp_dtb.mdt`, `ipa_fws.mdt`, `spss1t.mdt`); unsigned or cross-device (e.g. A52/OnePlus) ELF images are immediately rejected by `pas_init_image`.
2. **UEFI `PILDxe` / `PILProxyDxe` / `SPSSDxe` Pre-Boot Loading (`DXE.inc` + `uefipil.cfg`):**
   - `dm1qPkg/Include/DXE.inc` ALREADY includes and executes `SmemDxe`, `IPCCDxe`, `CmdDbDxe`, `ShmBridgeDxeLA`, `ScmDxeCompat`, `TzDxeLA`, `PILDxe`, `PILProxyDxe`, and `SPSSDxe` (`GLinkDxe` is currently commented out in `DXE.inc`!).
   - Inspecting `Binaries/dm1q/RawFiles/uefipil.cfg` and `PILDxe.efi` / `SPSSDxe.efi` confirms:
     * `[AUTO]` only auto-loads **`ABL`** (`SubsysID = 21`).
     * `SPSSDxe.efi` explicitly calls `PILDxe` (`ProcessPilImageExt`) to load **`[SPSS]`** (`\image\spss1t`, `SubsysID = 14`, `0x9B100000`, `0x180000`) when fuses allow SPU enablement!
     * `[RETAIL]` permits `PILDxe` to pre-boot **`FULL_ADSP`**, **`CORE_ADSP`**, **`CORE_CDSP`**, and **`CORE_MODEM`** (plus their `_DTB` pre-images) from `EBD0A0A2-...` (FAT `modem` partition) or `core_nhlos_a`!
     * **Key Strategic Insight:** Pre-booting `CORE_ADSP` or `FULL_ADSP` via UEFI `PILDxe` (after ensuring `PIL Reserved` covers `0x9EA00000..0xA43B4000`) could potentially bring up ADSP **before `ExitBootServices`** using Qualcomm's own signed `TzDxeLA`/`ShmBridgeDxeLA`/`PILDxe`/`PILProxyDxe` — bypassing the missing Windows `qcpep`/`qcpil` stack! However, `FULL_MODEM` requires runtime **RFS/TFTP** (to serve `modemst1`/`modemst2`/`fsg`) and **proxy unvote** after OS handoff, so `MPSS` cannot stay alive across Windows boot without an active Windows GLINK+QMI+RFS+PIL client!

---

## 4. Inter-Processor Communication Fabric (`IPCC`, `SMEM v12`, `SMP2P`, `GLink SMEM`)

### 4.1 `IPCC` (`qcom,ipcc@00408000`, `0x1000`, GIC `SPI 0xE5` = 229 -> `ACPI GSI 261`)

Register offsets (`drivers/mailbox/qcom-ipcc.c`):
- `0x00C` `IPCC_REG_SEND_ID`: write `(client_id << 16) | signal_id` to ring remote doorbell
- `0x010` `IPCC_REG_RECV_ID`: read pending `(client_id << 16) | signal_id` (`0xFFFFFFFF` = none)
- `0x014` `IPCC_REG_RECV_SIGNAL_ENABLE`, `0x018` `DISABLE`, `0x01C` `CLEAR`, `0x038` `CLIENT_CLEAR`

MPROC Protocol Signal IDs (`include/dt-bindings/mailbox/qcom-ipcc.h`):
- Signal `0` = `IPCC_MPROC_SIGNAL_GLINK_QMP`
- Signal `2` = `IPCC_MPROC_SIGNAL_SMP2P`
- Signal `3` = `IPCC_MPROC_SIGNAL_PING`

MPROC Client IDs (`qcom-ipcc.h`):

| Client ID | Macro | Subsystem | S23 Usage (`s23_live.dts`) |
|---:|---|---|---|
| `0` | `IPCC_CLIENT_AOP` | Always-On Processor | QMP (`0x0C300000`, signal `0`) |
| `1` | `IPCC_CLIENT_TZ` | TrustZone (`QTEE`) | TZ IPC |
| `2` | `IPCC_CLIENT_MPSS` | **Snapdragon X70 Modem** | GLink `mpss_smem` (sig `0`), SMP2P `smp2p-modem` (sig `2`) |
| `3` | `IPCC_CLIENT_LPASS` | **ADSP / LPASS** | GLink `adsp_smem` (sig `0`), SMP2P `smp2p-adsp` (sig `2`) |
| `4` | `IPCC_CLIENT_SLPI` | Sensors Low-Power Island | Merged into ADSP on SM8550 |
| `5` | `IPCC_CLIENT_SDC` | Sensors Data Controller | LPASS SSC internal |
| `6` | `IPCC_CLIENT_CDSP` | **Compute DSP / NPU** | GLink `cdsp_smem` (sig `0`), SMP2P `smp2p-cdsp` (sig `2`) |
| `7` | `IPCC_CLIENT_NPU` | Legacy NPU | Merged into CDSP on SM8550 |
| `8` | `IPCC_CLIENT_APSS` | Application Processor (HLOS) | Local host (`0x00408000` is APSS MPROC frame) |
| `16` | `IPCC_CLIENT_SPSS` | **Secure Processor** | GLink `spss_spss` (sig `0`, `0x1885008`) |

### 4.2 `SMEM v12` (`0x81D00000`, `0x200000`, `tcsr-mutex` hwlock #3)

Verified directly in `drivers/soc/qcom/smem.c`:
- **Primary Header (`0x81D00000`)**: `smem_proc_comm[4]` (`0x00..0x3F`) + `version[32]` (`0x40..0xBF`, where `version[7] >> 16 == 12` = `SMEM_GLOBAL_PART_VERSION`) + `initialized` (`0xC0`) + `free_offset` (`0xC4`) + `available` (`0xC8`) + `reserved` (`0xCC`) + `toc[512]` (`0xD0..0x20CF`, 16 bytes each: `allocated`, `offset`, `size`, `aux_base`).
- **Partition Table (`$TOC` at `SMEM_BASE + SMEM_SIZE - 4096 = 0x81EFF000`)**: `magic="$TOC"`, `version=1`, `num_entries`, `reserved[5]`, `entry[num_entries]` (48 bytes each: `offset`, `size`, `flags`, `host0`, `host1`, `cacheline`, `reserved[7]`), followed immediately by `struct smem_info` (`magic="SIII"`, `size`, `base_addr`, `reserved`, `num_items`).
- **Each Partition (`$PRT` at `SMEM_BASE + entry.offset`)**:
  - `host0`/`host1`: `0xFFFE:0xFFFE` = Global Partition (SMEM v12); `0:1` = APPS<->MPSS; `0:2` = APPS<->ADSP; `0:5` = APPS<->CDSP; `0:6` = APPS<->TZ; `0:8` = APPS<->SPSS; `0:12` = APPS<->AOP.
  - **Uncached heap** walks forward from `phdr + 32` to `phdr + offset_free_uncached`; **Cached heap** walks backward from `phdr + size - ALIGN(16, cacheline)` down to `phdr + offset_free_cached`. Every entry starts with `canary == 0xA5A5`, `item`, `size`, `padding_data`, `padding_hdr`.
- **Key SMEM Items Decoded by `s23pilprobe.sys`**:
  - **Item `137` (`SMEM_HW_SW_BUILD_ID` / `SMEM_SOCINFO`)**: SoC ID, version, `build_id[32]`, `raw_id/ver`, `hw_plat`, `hw_plat_subtype`, PMIC models/revs, `serial_num`, `chip_id[32]`, `nmodem_supported`, `feature_code` (`SM8550-AC` = `SOCINFO_FC_AC` = `3`).
  - **Item `469` (`SMEM_IMAGE_VERSION_TABLE`)**: `4096` bytes = `32` slots × `128` bytes (`name[75]`, `variant[20]`, `pad[1]`, `oem[32]`) covering `0=boot`, `1=tz`, `3=rpm/aop`, `10=apps`, `11=mpss`, `12=adsp`, `13=cnss`, `14=video`, etc.

### 4.3 `SMP2P` & `GLink SMEM` Item Mapping (`s23_live.dts` + `smp2p.c` + `qcom_glink_smem.c`)

- **`SMP2P` (`struct smp2p_smem_item`, `magic = 0x504D5324` `"$SMP"`, 16 named 32-bit state entries):**

| Edge (`s23_live.dts`) | Local PID -> Remote PID | Inbound SMEM Item (`remote -> APPS`) | Outbound SMEM Item (`APPS -> remote`) | IPCC Client / Signal | Named Entries |
|---|---:|---:|---:|---|---|
| `qcom,smp2p-modem` | `0` -> `1` (`MPSS`) | **`435` (`0x1B3`)** | **`428` (`0x1AC`)** | Client `2`, Sig `2` | `master-kernel`, `slave-kernel`, `ipa` (out & in) |
| `qcom,smp2p-adsp` | `0` -> `2` (`ADSP`) | **`443` (`0x1BB`)** | **`429` (`0x1AD`)** | Client `3`, Sig `2` | `master-kernel`, `slave-kernel`, `sleepstate`, `sleepstate_see`, `rdbg` |
| `qcom,smp2p-cdsp` | `0` -> `5` (`CDSP`) | **`94` (`0x5E`)** | **`432` (`0x1B0`)** | Client `6`, Sig `2` | `master-kernel`, `slave-kernel`, `rdbg` |

*(Note: `94` (`0x5E`) for CDSP inbound and `443` (`0x1BB`) for ADSP inbound lie outside the legacy `427..438` range! `s23pilprobe.sys` therefore scans ALL items `0..512` across all partitions and explicitly decodes `94` and `427..445`.)*

- **`GLink SMEM` Native Transport (`drivers/rpmsg/qcom_glink_smem.c`):**
  - Allocated in each edge's private SMEM partition (`0:1` MPSS, `0:2` ADSP, `0:5` CDSP):
    * **Item `478` (`SMEM_GLINK_NATIVE_XPRT_DESCRIPTOR`, `32` bytes)**: `le32 tx_tail`, `le32 tx_head`, `le32 rx_tail`, `le32 rx_head` (plus 16 bytes reserved/remote).
    * **Item `479` (`SMEM_GLINK_NATIVE_XPRT_FIFO_0`, `16 KiB`)**: APPS -> Remote TX ring buffer.
    * **Item `480` (`SMEM_GLINK_NATIVE_XPRT_FIFO_1`, `16 KiB`)**: Remote -> APPS RX ring buffer.
  - MPSS GLink Channels (`s23_live.dts`): `IPCRTR` (QRTR/QMI control plane), `DS` (`at_mdm0`), `DATA40_CNTL` (`smdcntl8`), `DATA1`/`4`/`11`, `QMC_DMA_LINE`, `QMC_CMA_LINE`, `glink_bridge`.

---

## 5. IPA v5.0 (`0x03E00000`) + GSI + QMI/QRTR Cellular WWAN Data Path

> [!WARNING]
> **Critical SM8550 IPA Address & Version Correction:** On `SM8550` (`dm1q`), IPA is **IPA v5.0 (`qcom,ipa-hw-ver = 0x18`) at `0x03E00000`** (`s23_live.dts` L24701), **NOT** legacy `0x01E00000` (SM8150–SM8450) and NOT v5.5 (SM8650)!

- **MMIO Regions:** `ipa-base = 0x03E00000` (`0x84000`), `gsi-base = 0x03E04000` (`0xFC000`), `qcom,entire-ipa-block-size = 0x200000`, `qcom,ipa-cfg-offset = 0x140000`.
- **Interrupts:** `ipa-irq` = GIC `SPI 0x28E` (654) -> **`GSI 686` (`0x2AE`)**; `gsi-irq` = GIC `SPI 0x1B0` (432) -> **`GSI 464` (`0x1D0`)** (Level ActiveHigh).
- **Firmware & PAS:** `ipa_fws` (`PAS ID 0x0F`, `0x9B080000`, 64 KiB) + `ipa_gsi_fw` (`0x9B090000`, 40 KiB).
- **SMMU (`apps_smmu@15000000`, `dma-coherent`):** SID `0x4A0` (`ipa_smmu_ap`, extra mapping `0x146A8000` `0x2000`), `0x4A1` (`wlan`), `0x4A2` (`uc`), `0x4A4` (`11ad`).
- **Clock & Interconnect:** RPMh BCM `IP0` (`RPMH_IPA_CLK`), interconnects `ipa_to_llcc`, `llcc_to_ebi1`, `appss_to_ipa`; SMP2P `ipa` out/in on `smp2p-modem`.
- **End-to-End 5G WWAN Stack for Windows (`MBBCx` / `WwanSvc`):**
  1. **Control Plane:** `SMEM (0x81D00000)` + `IPCC (0x00408000)` -> `GLink (mpss)` -> `IPCRTR` (`QRTR`) -> **QMI** services (`DMS`, `NAS`, `WDS`, `UIM`, `WDA`, `DFS`) + **RFS/TFTP** (serving **read-only RAM copies** of `modemst1`/`modemst2`/`fsg` to protect S23 IMEI/NV while `SanPolicy=4` keeps UFS LUNs offline!).
  2. **Data Plane:** QMI `WDA_SET_DATA_FORMAT` (`QMAPv5` aggregation + checksum offload) over **IPA v5.0 GSI TX/RX rings** (`0x03E04000`, SID `0x4A0`) -> Windows `NetAdapterCx` / **`MBBCx`** (`MBIM`-to-`QMI` translation miniport).
  3. **SIM (`UIM0`/`UIM1`):** Owned directly by MPSS (`GPIO130..132`/`GPIO27` + `ldob9` for SIM1; `GPIO134..136`/`GPIO26` + `ldob10` for SIM2/eSIM).
  4. **Feasibility Summary:** Because `windows_silicon_qcom_kailua` lacks `qcpep`, `qcglink`, `qcipc`, `qcamss`, `qcipa`, `qcqmi`, `qcrmnet`, and `qcmbb`, native 5G WWAN requires an open-source KMDF/MBBCx stack (or first using `s23pilprobe.sys` to verify SMEM/CMD_DB/IMEM PIL state and `PILDxe` pre-boot behavior safely!).

---

## 6. Safe Bring-Up & Verification Strategy (`s23_subsys_pil.asl` + `s23pilprobe.sys`)

1. **Inert ACPI SSDT (`modem_5g_research\s23_subsys_pil.asl` -> `.aml` & `.hex`):** Declares `SMEM`, `IPCC`, `AMSS` (`0x04080000`, `GSI 296`), `ADSP` (`0x03000000`, `GSI 518`), `CDSP` (`0x32300000`, `GSI 610`), `SPSS` (`0x0188101C`, `GSI 384`), and `IPA0` (`0x03E00000`/`0x03E04000`, `GSI 686`/`464`) — all gated behind master switch `S23P = 0x00` (`_STA = 0`) so it is 100% inert until explicitly enabled.
2. **Read-Only Live Probe (`drivers\s23pilprobe\pkg\s23pilprobe.sys`, `v1.0.0.1`, `StageMask = 0x01` default):** Zero MMIO writes, zero unclocked subsystem accesses, and skips `spss_region@9B100000` by default to prevent Gunyah/XPU faults while capturing SMEM v12 TOC/items/SocInfo/ImageVersions/SMP2P/GLink, CMD_DB, IMEM `ImageLoadInfo`, GCC clocks, and HLOS-readable PIL carveout headers into `C:\Users\Public\s23pilprobe.log`.
