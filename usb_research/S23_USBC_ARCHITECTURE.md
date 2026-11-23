# Native USB-C Architecture and Bring-Up Specification for Samsung Galaxy S23 on Windows 11 ARM64
Bringing up full native USB-C functionality—dynamic USB 2.0 and USB 3.2 Gen 1/2 Dual-Role switching, USB-PD 3.0 Sink and Source charging, OTG 5 V VBUS and VCONN sourcing, simultaneous PD charging with USB-C Hub Host Mode (`DR_SWAP`/`PR_SWAP`), and 4K60 DisplayPort Alt-Mode—on Windows 11 Pro 24H2 ARM64 (10.0.26100.4349) for the Samsung Galaxy S23 (`dm1q` / `SM-S911B`, Snapdragon 8 Gen 2 for Galaxy / `SM8550-AC`) requires replacing static peripheral forcing (`RoleSwitchMode=2`) with a coordinated four-driver Windows kernel architecture. Because the Samsung `MAX77705` sub-PMIC runs an internal USB-PD 3.0 state machine over a proprietary I2C OPCODE mailbox (`0x25`) rather than UCSI or raw TCPCI registers, Microsoft's USB Type-C driver model dictates implementing a custom **KMDF `UcmCx` client driver (`s23usbc.sys`)** paired with a custom **KMDF `UrsCx` client driver (`s23urs.sys`)** that orchestrates Qualcomm-specific `QSCRATCH`, `eUSB2`, `PTN3222`, and `QMP` PHY transitions while delegating host and peripheral traffic to inbox `usbxhci.sys` and `ufxsynopsys.sys`. Both `UcmCx` and `UrsCx` are WDF/KMDF class extensions (`UrsCx` requires minimum KMDF 1.15 and links `Urscxstub.lib`) that cannot bind to standalone WDM binaries, so both core USB-C drivers must be compiled with the existing MSVC + WDK `10.0.26100.6584` toolchain alongside `s23gpu.sys`, reserving the WSL `clang-18` WDM pipeline for Phase 0 read-only probes.
Two hardware realities govern the bring-up order and prevent destructive races or dead-end debugging. First, unifying bus ownership on `QUPv3_I2C_HUB SE8` (`0x009A0000`) is a prerequisite before enabling any `MAX77705` USB-C control: leaving `s23batt.sys` bit-banging `GPIO206`/`GPIO207` every 2 seconds while a USB-C driver services `GPIO133` interrupts on the same bus will corrupt OPCODE transactions (`0x21–0x41`) and race on clear-on-read top-level `INTSRC` (`0x22`) and `UIC`/`CC`/`PD`/`VDM` interrupt decoding. Second, while 4K60 DisplayPort Alt-Mode video integrates cleanly into `s23gpu.sys` via SM8550's independent DPU 9.0 `INTF_0` (`0x0AE35000`) and DisplayPort controller (`0x0AE90000`), native DisplayPort Audio PCM samples do not enter through a CPU-writable FIFO inside the DP controller; on SM8550 they are supplied exclusively by LPASS (`DISPLAY_PORT_RX_0`), whose LPAIF register space is XPU-locked to the Hexagon ADSP (`VMID_LPASS`) on retail Gunyah builds. Immediate external audio should therefore use inbox `usbaudio2.sys` over USB-C Host Mode or the already operational QCA6490 Bluetooth A2DP stack (`BthA2dp.sys`), deferring native DP monitor audio until ADSP PIL and AudioReach are brought up independently.
Three hardware register areas lack complete public documentation and must be audited directly on the phone or extracted from Samsung's official `SM-S911B` open-source kernel drop before writing production driver code: (1) the NDA-restricted NXP `PCA9481` (`0x57`) switched-capacitor direct charger register map for 25 W PPS Super Fast Charging (`MAX77705`'s internal switching buck charger safely covers 15 W–18 W Fixed PDO charging without enabling `PCA9481`), (2) the exact 4-bit mode enumeration in `MAX77705` charger `CHG_CNFG_00` (`0xB7`, beyond mode `0x04` verified during firmware updates) plus exact byte-level payload offsets inside `MAX77705` VDM (`0x48`/`0x4B`/`0x55`) and PPS (`0x3A`/`0x3C`) OPCODE frames for the S23's specific MCU firmware revision, and (3) individual `DISP_CC` CBCR gate offsets and full `dp_reg.h` / `qmp_v6_dp_serdes_tbl_*` register arrays beyond the verified base apertures and RCG offsets detailed below.

## 1. S23 (dm1q / SM8550) USB-C Hardware Topology Verification
Mapping every active and passive IC between the USB-C receptacle and the SM8550 SoC prevents writing unnecessary UEFI or Windows redriver code while ensuring that polarity-sensitive sideband paths (`SBU1`/`SBU2` to DP AUX) are not overlooked. The Galaxy S23 USB-C port splits into four distinct hardware planes: a power/CC/MUIC control plane anchored by `MAX77705` and `PCA9481` on `QUPv3_I2C_HUB SE8`, a USB 2.0 signaling plane translated by the NXP `PTN3222` eUSB2 repeater on `QUP1 SE6`, a four-lane SuperSpeed and DisplayPort plane routed directly into SM8550's QMP Combo PHY, and an SBU/AUX sideband plane switched by CC polarity.

### External SuperSpeed Redriver and SBU/AUX Switch Verification
Available hardware inventories and kernel module analyses indicate that the Galaxy S23 (`dm1q` / `SM-S911B`) does not populate an external SuperSpeed or DisplayPort redriver (`PS5169`, `TUSB546`, `PTN36502`, or `NB7VPQ904M`), eliminating any need for a UEFI `RedriverDxe` or Windows redriver driver. Community device-probe dumps on `deviceinfohw.ru` for `SM-S911B` (running stock `5.15.74` Android 13) enumerate `pca9481-charger`, `max77705`, and `qca6490`, with no `ps5169`, `ptn36502`, `tusb546`, or `nb7vpq904m` driver bound, whereas competing SM8550 handsets in the same database explicitly report `ps5169` and load dedicated `nb7vpq904m.ko` and `ps5169.ko` vendor modules. Although `/sys/module/redriver` is visible on stock S23 Android and `redriver.ko` appears in generic `dm1q` TWRP `BoardConfig.mk`, that list reflects Qualcomm's generic Kalama GKI vendor set (which also lists unpopulated `wcd938x_dlkm.ko`, `wsa884x_dlkm.ko`, `fsa4480-i2c.ko`, and Sony `pinctrl-somc.ko` modules); `redriver.ko` itself is Qualcomm's generic `drivers/usb/redriver/` framework helper, not a populated external redriver chip. On `dm1q`, the four SuperSpeed TX/RX pairs route directly into the SM8550 QMP USB3+DP Combo PHY (`0x088E8000`), which handles internal 4-lane muxing and polarity flipping via `USB3_DP_COM_PHY_MODE_CTRL` and `USB3_DP_COM_TYPEC_CTRL`. The NXP `PTN3222` (`0x4F` on QUP1 SE6) is strictly an **eUSB2-to-USB2.0** (LS/FS/HS) redriver/repeater and never touches SuperSpeed or DisplayPort lanes. Because `deviceinfohw.ru` reports four unidentified I2C entries (`unk18`, `unk20`, `unk23`, `unk25`, where `0x25` is `MAX77705` USBC) and the live `s23_live.dts` USB/DP graph was not present in the extracted repository files, Phase 0 should still confirm from `s23_live.dts` that no undocumented redriver or `fsa4480` mux node exists.
For the sideband channel, `MAX77705` does **not** serve as a full high-speed DP AUX cross-point switch by itself. Instead, Samsung's `MAX77705`-based hardware designs tap `SBU1`/`SBU2` into `MAX77705` for moisture detection (`OPCODE_READ_SBU 0x25`) and VBUS-SBU / SBU-GND short protection (`OPCODE 0x56`), while routing `SBU1`/`SBU2` through a discrete **Vishay `DG2730` 2-port DPDT analog switch** to flip polarity into the SoC's `DP_AUX_P`/`DP_AUX_N` differential pins. Both the Galaxy S20 FE (`SM-G780F`, `MAX77705`) service manual block diagram and third-party Galaxy S23 Ultra (`MAX77705C` + `PCA9481UK`) board teardown identifications (`sahii.in`) confirm the Vishay `DG2730` DPDT switch alongside `MAX77705`. On `dm1q`, the repository's verified baseline identifies `TLMM GPIO11` (`samsung,cc_dir`) as the CC polarity GPIO—consistent with driving the `DG2730` select pin (or equivalent passive SBU switch). Because no official `dm1q` schematic was available, Phase 0 must inspect `s23_live.dts` for any secondary AUX-enable or orientation GPIO paired with `GPIO11` and verify live pin state before debugging DisplayPort AUX timeouts.

### SMMU and DMA Coherency (`_CCA`) Constraints for DWC3 Host and Function
In SM8550 Linux Device Trees, the Synopsys DWC3 controller at `0x0A600000` (core aperture `0xCD00` bytes) sits behind `apps_smmu` (Stream ID `0x740` / `1856` in SM8550 DWC3 schema checks; `0x820` on related QTI SoCs) and uses `dma-coherent: true` when an OS-managed SMMU context bank enforces cacheable transactions. On Qualcomm Windows-class ACPI systems, `URS0` (`QCOM0C8B-0`, `0x0A600000–0x0A6FFFFE`) enumerates xHCI 1.10 with 64-bit DMA.
On the S23 running Project Mu UEFI without Qualcomm's proprietary `qcsmmu` driver (`MMU0` / `QCOM0C09`), the USB Stream ID executes either in APPS SMMU bypass or under UEFI-inherited translation. Two constraints directly affect stability for both `usbxhci.sys` (Host) and `ufxsynopsys.sys` (Function). First, advertising ACPI `_CCA = 1` (hardware cache coherent) when the SMMU stream is in raw bypass without hardware snooping attributes causes Windows' HAL DMA layer to omit explicit ARM64 cache clean/invalidate operations around xHCI TRB rings, ERST entries, and bulk buffers, leading to silent descriptor corruption or intermittent endpoint stalls. Setting **`_CCA = 0`** on `URS0`, `USB0`, and `UFN0` forces explicit OS cache maintenance on every DMA transfer and is the safe baseline unless Phase 0 verifies that APPS SMMU `S2CR` for Stream ID `0x740` forces coherent Inner-Shareable Write-Back attributes. Second, independent SM8550 bare-metal bring-up work (`unixnu.com`) warns that SMMU bypass still imposes address-width constraints and requires validating every DMA address range. Phase 0 should confirm xHCI 64-bit addressing (`HCCPARAMS1.AC64`) and test DMA above 4 GiB physical RAM on the 8 GB S23; if bypass truncates upper physical address bits, adding an ACPI `_DMA` resource window to `URS0`/`USB0`/`UFN0` cleanly constrains Windows DMA allocations below the safe ceiling.

### Race-Free I2C and PMIC Unification Architecture
`QUPv3_I2C_HUB SE8` (`0x009A0000`, `GPIO206`/`GPIO207` eGPIO, `IRQ GPIO133`) is shared by five I2C slave addresses across two critical power chips: `MAX77705` PMIC (`0x66`), USBC/PDIC/MUIC (`0x25`), Switching Charger (`0x69`), Fuel Gauge (`0x36`), Debug (`0x62`), and NXP `PCA9481` Direct Charger (`0x57`). Meanwhile, `s23batt.sys` currently polls `0x36` and `0x69` every 2 seconds using raw TLMM bit-banging, and the 168-entry `s23_qmp_seq.h` USB3 PHY init table lives inside `drivers/s23touch/`. Leaving `s23batt.sys` bit-banging SE8 concurrently with a USB-C driver servicing `GPIO133` interrupts guarantees bus corruption during multi-byte OPCODE transfers (`0x21–0x41`) and lost interrupts on clear-on-read `INTSRC` (`0x22`) and `REG_UIC_INT..REG_VDM_INT` (`0x02–0x05`). Similarly, leaving any QMP PHY programming inside `s23touch.sys` will collide with dynamic USB-C hot-plug resets and DisplayPort mode switches on `0x088E8000`.

| Architectural Component | Current Baseline (`s23-windows-11`) | Target Race-Free Architecture | Serialization & Ownership Mechanism |
| --- | --- | --- | --- |
| **`QUPv3_I2C_HUB SE8` (`0x009A0000`)** | TLMM bit-bang inside `s23batt.sys` (2 s poll) | Single owner **`s23pmic.sys`** (GENI FIFO I2C preferred, serialized bit-bang fallback) | Single `FAST_MUTEX` / `KMUTEX` mirroring Linux `max77705->i2c_lock` |
| **`MAX77705` Shared IRQ (`GPIO133`)** | Polled / unused | Owned exclusively by **`s23pmic.sys`** | Reads `0x66` `INTSRC` (`0x22`), fans out `CHG` (`BIT 0`), `FG` (`BIT 2`), `MUIC/USBC` (`BIT 3`) callbacks |
| **`s23batt.sys` (Battery Class)** | Direct TLMM bit-bang to `0x36` / `0x69` | Thin client of `s23pmic.sys` via `GUID_S23_PMIC_BUS_INTERFACE` | Zero direct SE8 GPIO/MMIO access; queries FG (`0x05–0x13`) and charger status via arbiter |
| **`s23usbc.sys` (`UcmCx` Client)** | Does not exist | Primary client of `s23pmic.sys` for `0x25`, `0x69` OTG/ILIM, `0x57` (Phase 5) | Registers `MUIC/USBC` IRQ callback; serializes OPCODE mailbox (`0x21–0x41` / `0x51–0x71`) under its own command mutex |
| **`s23touch.sys` & `QMP PHY` (`0x088E8000`)** | `s23_qmp_seq.h` located in `drivers/s23touch/` | **Decoupled:** `s23touch.sys` handles touch only; **`s23urs.sys`** owns QMP PHY (`0x088E8000`) | Single PHY state machine coordinating USB3 resets and DP lane grants with `s23gpu.sys` |
| **`QUP1 SE6` (`0x00A98000`, `PTN3222` `0x4F`)** | UEFI-initialized only | Owned by **`s23urs.sys`** | Dedicated I2C bus (`GPIO48`/`GPIO49`); zero contention with SE8 |
| **`S2MIW04` (`0x5B` on `SE5` `0x00994000`)** | Unused on Windows | Separate bus (`SE5`), coordinated with `s23pmic.sys` on `0x69` `CHG_CNFG_00` | Prevents wireless charging (`WCIN`) from conflicting with USB-C OTG boost (`wpc_en_changed` pattern) |

Implementing **`s23pmic.sys`** as a dedicated KMDF MFD bus arbiter is superior to attempting inbox `SpbCx` sharing on `dm1q`. Signed Qualcomm Kailua `qci2c`, `qcpep`, and `qcgpio` drivers are unavailable for SM8550—the exact constraint that made direct GENI FIFO hardware ownership succeed in `s23bthx.sys`—and even with `SpbCx`, a parent MFD dispatcher is still required to read `0x66` `INTSRC` (`0x22`) on `GPIO133` before routing interrupts to `s23batt.sys` and `s23usbc.sys`. Because `QUPv3_I2C_HUB` GENI FIFO operation is already validated live on `SE3` (`0x0098C000`) and `SE6` (`0x00998000`) via `s23sensprobe.sys`, `s23pmic.sys` can run hardware 400 kHz GENI FIFO I2C on `0x009A0000` (with a transparent fallback to serialized TLMM bit-bang inside the same mutex if Phase 0 finds `SE8` locked to GPI/GSI DMA) and expose a single `GUID_S23_PMIC_BUS_INTERFACE` to `s23batt.sys` and `s23usbc.sys`.

## 2. MAX77705 and PCA9481 Protocol Specification
Samsung's `MAX77705` sub-PMIC exposes five 7-bit I2C slave addresses (derived via `>> 1` from 8-bit addresses in `drivers/mfd/max77705.c`): PMIC top-level (`0x66` = `0xCC >> 1`), USBC/PDIC/MUIC (`0x25` = `0x4A >> 1`), Switching Charger (`0x69` = `0xD2 >> 1`), Fuel Gauge (`0x36` = `0x6C >> 1`), and Debug (`0x62` = `0xC4 >> 1`).

### Top-Level Interrupt Dispatch (`0x66`) and USBC Register Map (`0x25`)
On every active-low `GPIO133` interrupt, `s23pmic.sys` reads `MAX77705_PMIC_REG_INTSRC` (`0x22` on slave `0x66`; masked by `INTSRC_MASK` at `0x23`). `BIT(0)` (`MAX77705_IRQSRC_CHG`) notifies charger state changes, `BIT(2)` (`MAX77705_IRQSRC_FG`) notifies fuel-gauge alerts, and `BIT(3)` (`MAX77705_IRQSRC_MUIC`) notifies all USBC/PDIC/MUIC/VDM events. When `BIT(3)` asserts, `s23usbc.sys` performs a 4-byte bulk read starting at `0x02` (`REG_UIC_INT` through `REG_VDM_INT`) on slave `0x25` to latch and clear all pending interrupt bits, then reads status registers `0x06–0x0D`.

| Slave `0x25` Offset | Register Name | Bitfields & Verified Functional Meaning |
| --- | --- | --- |
| `0x00` / `0x01` / `0x09` | `UIC_HW_REV` / `UIC_FW_REV` / `UIC_FW_MINOR` | Hardware revision and internal MCU firmware major/minor version |
| `0x02` | `REG_UIC_INT` (Mask: `0x0E`) | `BIT(7) APCmdResI` (OPCODE response ready), `BIT(6) SYSMsgI`, `BIT(5) VBUSDetI`, `BIT(4) VbADCI`, `BIT(3) DCDTmoI`, `BIT(1) CHGTypI`, `BIT(0) UIDADCI` |
| `0x03` | `REG_CC_INT` (Mask: `0x0F`) | `BIT(7) VCONNOCPI`, `BIT(6) VSAFE0VI`, `BIT(5) AttachSrcErrI`, `BIT(4) VCONNSCI`, `BIT(3) CCPinStatI`, `BIT(2) CCIStatI`, `BIT(1) CCVcnStatI`, `BIT(0) CCStatI` |
| `0x04` | `REG_PD_INT` (Mask: `0x10`) | `BIT(7) PDMsgI`, `BIT(5) DataRole`, `BIT(1) SSAccI`, `BIT(0) FCTIDI` |
| `0x05` | `REG_VDM_INT` (Mask: `0x11`) | Structured VDM / DisplayPort Alt-Mode interrupt flags |
| `0x06` | `REG_USBC_STATUS1` | `BITS[7:4] VBADC` (VBUS voltage bucket: `<3.8 V`, `3.8–4.5 V`, `7.5–8.5 V`, etc.), `BITS[2:0] UIDADC` |
| `0x07` | `REG_USBC_STATUS2` | `BITS[7:0] SYSMsg` (MCU boot/system/fault message code) |
| `0x08` | `REG_BC_STATUS` | `BIT(7) VBUSDet`, `BITS[5:3] PrChgTyp`, `BIT(2) DCDTmo`, `BITS[1:0] ChgTyp` (BC1.2 SDP/CDP/DCP) |
| `0x0A` | `REG_CC_STATUS0` (*commented `CC_STATUS1`*) | `BITS[7:6] CCPinStat` (active CC1/CC2 orientation), `BITS[5:4] CCIStat` (Type-C Rp current: Default/1.5 A/3.0 A), `BIT(3) CCVcnStat` (VCONN active), `BITS[2:0] CCStat` (UFP/DFP/Detach) |
| `0x0B` | `REG_CC_STATUS1` (*commented `CC_STATUS2`*) | `BITS[7:6] CCSBUSHORT`, `BIT(5) VCONNOCP`, `BIT(4) VCONNSC`, `BIT(3) VSAFE0V`, `BIT(2) AttachSrcErr`, `BIT(1) ConnStat`, `BIT(0) Altmode` |
| `0x0C` | `REG_PD_STATUS0` | `BITS[7:0] PDMsg` (latest USB-PD protocol message/state ID) |
| `0x0D` | `REG_PD_STATUS1` | `BIT(7) PD_DataRole` (UFP vs DFP), `BIT(4) PD_PSRDY`, `BITS[3:0] FCT_ID` |
| `0x21` / `0x22–0x40` / `0x41` | `REG_OPCODE` / `REG_OPCODE_DATA` / `REG_END_DATA` | AP-to-MCU write mailbox (`OPCODE_WRITE` at `0x21`, payload at `0x22+`, `OPCODE_WRITE_END` at `0x41`) |
| `0x51–0x71` | `REG_OPCDE_RES` (`OPCODE_READ`) | MCU-to-AP response buffer (`0x51` echoes completed opcode, followed by response payload) |

(Register-naming caveat: Samsung's `max77705.h` defines the register addresses as `REG_CC_STATUS0 = 0x0A` and `REG_CC_STATUS1 = 0x0B`, while labeling the corresponding bitfield comment blocks `REG_CC_STATUS1` and `REG_CC_STATUS2`. Driver code must index strictly by hex addresses `0x0A` and `0x0B`.)

### AP-to-MCU OPCODE Mailbox Protocol and Verified Command Set
Control transactions to `MAX77705` (`0x25`) execute sequentially through a single-slot FIFO queue synchronized by `APCmdResI` (`REG_UIC_INT.BIT(7)`):

1. Acquire `s23usbc`'s OPCODE queue mutex so only one AP-to-MCU transaction is active at any instant.
2. Write `[opcode, write_data[0..N-1]]` via `s23pmic.sys` starting at `0x21` (`REG_OPCODE` / `REG_OPCODE_DATA`), spanning up to the `0x21–0x41` mailbox aperture.
3. Trigger MCU execution by writing the end-of-message register `0x41` (`OPCODE_WRITE_END`) when the write does not already reach `0x41` (confirm exact short-write padding/commit byte `0x00` against Samsung's `max77705_usbc_opcode_write()` implementation, whose body was truncated in public views).
4. Wait on a kernel `KEVENT` (200–500 ms timeout) signaled when `GPIO133` fires with `REG_UIC_INT.BIT(7) == 1` (`BIT_APCmdResI`).
5. Bulk-read `1 + read_length` bytes starting at `0x51` (`OPCODE_READ`), verify byte `0` echoes the submitted `opcode`, parse the response payload, and release the queue for the next command.

| Functional Group | Opcode (Hex) | Samsung `max77705.h` Identifier | Verified Role & Payload Notes |
| --- | --- | --- | --- |
| **BC1.2 & MUIC Switch** | `0x01` / `0x02` | `OPCODE_BCCTRL1_R` / `W` | Enable/trigger BC1.2 charger detection (`CHGDetEn`, `CHGDetMan`) |
| **BC1.2 & MUIC Switch** | `0x03` / `0x04` | `OPCODE_BCCTRL2_R` / `W` | Manual D+/D- control and D+ reset (`DPDNMan \| DPDrv`, value `0x10`) |
| **BC1.2 & MUIC Switch** | `0x05` / `0x06` | `OPCODE_CTRL1_R` / `W` | MUIC analog path routing: `COM_OPEN` (TA/AFC/QC), `COM_USB` (AP/CP), `COM_UART` (AP/CP) |
| **BC1.2 & MUIC Switch** | `0x07–0x0A`, `0x1A–0x1D` | `OPCODE_CTRL2/3`, `CTRLREG_INIT/3` | Secondary MUIC/USBC control and initialization registers |
| **CC & VCONN Control** | `0x0B–0x10` | `OPCODE_CCCTRL1/2/3_R` / `W` | CC state machine configuration (DRP/UFP/DFP, Try.SNK, recovery) |
| **CC & VCONN Control** | `0x13` / `0x14` | `OPCODE_OPCODE_VCONN_ILIM_R` / `W` | VCONN current limit and sourcing control |
| **SBU & Safety** | `0x25` | `OPCODE_READ_SBU` | Write `[0x01]` (len 1), read 2 bytes (`sbu1`, `sbu2`) for moisture/water check |
| **SBU & Safety** | `0x56` | Short-Circuit Detection Enable | Write `[0x07]` (len 1): `BIT(0)` VBUS-CC, `BIT(1)` SBU-GND, `BIT(2)` VBUS-SBU short detect |
| **AFC / QC / Input ILIM** | `0x11/0x12`, `0x15–0x18`, `0x57` | `HVCTRL`, `CHGIN_ILIM/2`, `FW_AUTOIBUS` | HV charging control, buck input current limits, firmware auto-IBUS control |
| **AFC / QC / Input ILIM** | `0x20` / `0x21` / `0x22` | `AFC_HV_W`, `AFC_RESULT_R`, `QC2P0_SET` | Legacy Samsung AFC 9 V and Qualcomm QC 2.0 negotiation |
| **USB-PD 3.0 Fixed PDO** | `0x30–0x33` | `CURRENT_SRCCAP`, `GET_SRCCAP`, `SRCCAP_REQUEST`, `SET_SRCCAP` | Query partner PDO table, request Fixed PDO (e.g., 5 V/3 A or 9 V/2 A), advertise local Source caps |
| **USB-PD 3.0 Get/Resp** | `0x34–0x36` | `SEND_GET_REQUEST`, `READ_RESPONSE_FOR_GET_REQUEST`, `SEND_GET_RESPONSE` | Extended USB-PD 3.0 Get/Response control messages |
| **USB-PD 3.0 PPS APDO** | `0x3A` / `0x3B` / `0x3C` | `APDO_SRCCAP_REQUEST`, `GET_PPS_STATUS`, `SET_PPS` | Request PPS APDO, query PPS status, enable MCU periodic PPS keep-alive |
| **Role Swaps** | `0x37` / `0x38` | `OPCODE_SWAP_REQUEST` / `_RESPONSE` | Payload `0x01` = `DR_SWAP`, `0x02` = `PR_SWAP`, `0x04` = `MANUAL_ROLE_SWAP` |
| **DP Alt-Mode / VDM** | `0x40`, `0x48`, `0x4B`, `0x55` | `VDM_DISCOVER_*`, `SET_ALTERNATEMODE` | Arm Alt-Mode (`0x55`), transmit Structured VDM (`0x48`), fetch VDM/Attention/HPD response (`0x4B`) |
| **Diagnostics / Reset** | `0x4D`, `0x54`, `0x59`, `0x70`, `0xD1` | `GET_PD_MSG`, `FACTORY_TEST`, `SELFTEST`, `FW_OPCODE_CLEAR`, `RAM_TEST` | PD message query, factory/water control, self-test, stuck-opcode clear, RAM test |

### CC Attach, BC1.2/MUIC Switching, and Role Swaps (`PR_SWAP` / `DR_SWAP`)
On cable attach, `REG_CC_INT.CCStatI` (`BIT 0`) and `CCPinStatI` (`BIT 3`) fire. Reading `REG_CC_STATUS0` (`0x0A`) resolves three hardware states: partner type (UFP peripheral, DFP host/charger, or detached) in `CCStat` (`BITS[2:0]`), active CC orientation (`CC1` normal vs `CC2` flipped) in `CCPinStat` (`BITS[7:6]`), and Type-C Rp current advertisement (Default, 1.5 A, or 3.0 A) in `CCIStat` (`BITS[5:4]`). For non-PD legacy sources, `REG_BC_STATUS` (`0x08`) reports `VBUSDet` (`BIT 7`) and BC1.2 `ChgTyp` (`BITS[1:0]`): SDP and CDP connections route MUIC to `COM_USB` (AP PDA path) so `s23link.py` enumerates cleanly, whereas Dedicated Chargers (TA), AFC 9 V, and QC chargers set `COM_OPEN` via `OPCODE_CTRL1_W` (`0x06`) to isolate D+/D- during HV charging.
Simultaneous **PD Charging + USB-C Hub Host Mode** (enabling desktop docks with external storage, Ethernet, keyboard, and 4K60 display while charging the phone) relies on decoupling power and data roles via `OPCODE_SWAP_REQUEST` (`0x37`). A powered USB-C PD hub initially attaches as **Power Source + DFP**, leaving the S23 as Power Sink + UFP. Once the PD power contract settles (`REG_PD_STATUS1.PD_PSRDY == 1`), `s23usbc.sys` submits `OPCODE_SWAP_REQUEST` (`0x37`) with payload **`0x01` (`DR_SWAP`)**. When `REG_PD_INT.DataRole` (`BIT 5`) fires and `REG_PD_STATUS1.PD_DataRole` (`BIT 7`) confirms DFP, `s23usbc.sys` keeps the S23 in **Power Sink** (charging continuously at 9 V without ever touching OTG VBUS boost) and calls `UcmConnectorDataDirectionChanged(UcmTypeCPortStateDfp)`, instructing `UrsCx` to switch DWC3 into `usbxhci.sys` Host Mode. When a power-direction reversal is needed (such as powering a peer device or accepting power from an initially-sink dock), payload **`0x02` (`PR_SWAP`)** triggers a USB-PD power role swap followed by contract renegotiation.

### OTG 5 V VBUS Boost, VCONN Sourcing, and Charging (`0x69` vs `PCA9481` `0x57`)
On the `MAX77705` switching charger (`0x69`), upstream Linux patches verify the core register offsets: `MAX77705_CHG_REG_CNFG_00` (`0xB7`), `CNFG_01` (`0xB8`), `CNFG_02` (`0xB9`), `CNFG_09` (`0xC0`), and `CNFG_10` (`0xC1`). In `CHG_CNFG_00` (`0xB7`), bits `[3:0]` (`mask 0x0F`) select the converter mode—mode `0x04` is verified in `max77705.c` to force Buck-only / disable charging during USBC firmware updates—while `CHG_CNFG_00_WDTEN_MASK` enables the hardware charger watchdog timer. (Documentation gap: the complete 4-bit `CHG_CNFG_00` mode table distinguishing Buck+Charger, OTG 5 V Boost, and UNO wireless reverse-boost, plus exact milliamp step sizes in `CNFG_02`/`CNFG_09`, were inaccessible in the retrieved `max77705-private.h` header; extract those exact constants from Samsung's `SM-S911B` `max77705_charger.h` or verify against existing `s23batt.c` reads before enabling OTG boost.)
When a passive UFP peripheral (flash drive, keyboard, UAC2 DAC) attaches and `CCStat` resolves to DFP (Power Source), `s23usbc.sys` must execute a strict safety sequence: confirm `REG_BC_STATUS.VBUSDet == 0` and `REG_CC_STATUS1.VSAFE0V == 1`, program `0x69` `CHG_CNFG_00` (`0xB7`) to its verified OTG 5 V boost mode, and configure VCONN current limiting via `0x25` `OPCODE_OPCODE_VCONN_ILIM_W` (`0x14`) whenever an e-marked cable or active Alt-Mode adapter requires VCONN (monitoring `VCONNOCPI` and `VCONNSCI` in `REG_CC_INT`). On detach, OTG boost must be cleared back to Buck/Charger mode before tearing down `usbxhci.sys`.
For charging power tiers, separating Phases 2–4 from Phase 5 avoids unnecessary battery-safety risk. During Phases 2–4, `s23usbc.sys` handles **15 W–18 W Fixed PDO charging** exclusively through `MAX77705`'s internal switching buck (`0x69`): it queries partner capabilities via `OPCODE_GET_SRCCAP` (`0x31`), requests the highest safe Fixed PDO (5 V/3 A or 9 V/2 A) via `OPCODE_SRCCAP_REQUEST` (`0x32`), programs `0x69` input/fast-charge current limits, and pets `CHG_CNFG_00` WDT periodically. For **25 W Super Fast Charging**, hardware probes and teardowns confirm `dm1q` pairs `MAX77705` with an **NXP `PCA9481`** 2:1 switched-capacitor direct charger at `0x57` (successor to NXP `PCA9468` used in the Galaxy S20 and Pixel 6). Direct charging drives VBUS via `OPCODE_APDO_SRCCAP_REQUEST` (`0x3A`) and `OPCODE_SET_PPS` (`0x3C`) to `~2 × V_BAT + I_CHG × R_path` (8.0–9.8 V) while `PCA9481` divides voltage by 2 and doubles current into the cell. Because NXP's `PCA9481` datasheet is NDA-restricted and public sources document only partial `PCA9468` controls (`START_CTRL`, `STANDBY_FORCED`, `iin_cfg`, `v_float`, watchdog output), keep `PCA9481` in standby during Phases 0–4 and implement Phase 5 PPS direct charging only after extracting the verbatim `pca9481_charger.c` register map and protection limits from Samsung's `SM-S911B` kernel drop.

### DisplayPort Alt-Mode VDM Negotiation (`SVID 0xFF01`)
DisplayPort Alt-Mode begins by arming `MAX77705`'s Alt-Mode state machine via `OPCODE_SET_ALTERNATEMODE` (`0x55`) during driver init. When a DP-capable monitor or USB-C dock attaches, `MAX77705` processes Structured VDMs (`OPCODE_VDM_DISCOVER_SET_VDM_REQ` `0x48` and `OPCODE_VDM_DISCOVER_GET_VDM_RESP` `0x4B`), asserting `REG_VDM_INT` (`0x05`) and `REG_CC_STATUS1.Altmode` (`BIT 0`). The negotiation follows four standard VESA DP Alt-Mode stages:

1. **Discover Identity and Discover SVIDs:** Confirm the port partner advertises DisplayPort SVID **`0xFF01`**.
2. **Discover Modes and Pin Assignment Selection:** Read the partner's DP Capabilities VDO (`Pin Assignments Supported`). Select **Pin Assignment C (4-lane DP, USB 2.0 via `PTN3222`)** when connecting directly to a 4K60 USB-C monitor or when full 4K60 CTA-861 RGB bandwidth is required; select **Pin Assignment D (2-lane DP + 2-lane USB 3.2 SuperSpeed)** when docking to a multi-function hub where concurrent SuperSpeed storage or Gigabit Ethernet is needed alongside 1440p60/4K30 (or 4K60 CVT-RBv2 HBR3) output.
3. **Enter Mode and DP Configure:** Issue `Enter Mode (0xFF01)`, exchange `DP Status Update`, and send `DP Configure` locking the selected Pin Assignment (C or D) and active signaling mode.
4. **HPD and `IRQ_HPD` Forwarding:** Partner `Attention` VDMs carry `HPD_State` (High/Low) and `IRQ_HPD` pulses; `s23usbc.sys` decodes these from `REG_VDM_INT` (analogous to Samsung's `ID_DP_HPD` notifier) and dispatches them into `s23gpu.sys`. *(Verify exact payload byte indices inside `0x48`/`0x4B`/`0x55` frames against Samsung's `max77705_alternate.c` prior to Phase 4 coding.)*

## 3. Dynamic Dual-Role USB 2.0 and USB 3.2 SuperSpeed on Windows 11 ARM64
Seamless hot-plug switching—keeping `s23link.py` WinUSB and MTP working whenever connected to a PC while automatically activating `usbxhci.sys` Host Mode for hubs, SSDs, keyboards, USB Ethernet, and UAC2 DACs without rebooting—relies on separating Type-C connector policy (`UcmCx`), SoC dual-role hardware sequencing (`UrsCx`), and Microsoft's inbox USB protocol stacks (`usbxhci.sys` and `ufxsynopsys.sys`).

### Windows UCM and URS Driver Selection
Microsoft defines three USB Type-C class extensions—`UcmUcsiCx`, `UcmTcpciCx`, and `UcmCx`—matched strictly to controller firmware capabilities:

| Microsoft Class Extension | Hardware Prerequisite | Fit for Samsung `MAX77705` (`dm1q`) |
| --- | --- | --- |
| **`UcmUcsiCx.sys` + `UcmUcsiAcpiClient`** | Controller implements UCSI specification over ACPI or custom transport | **Incompatible** — `MAX77705` uses Samsung's proprietary I2C OPCODE mailbox, not UCSI |
| **`UcmTcpciCx.sys`** | Port controller without internal PD state machine (Windows runs PD FSM) | **Incompatible** — `MAX77705` executes USB-PD 3.0 inside its onboard MCU firmware |
| **`UcmCx.sys` (Required)** | Controller implements PD state machine in hardware/firmware but is not UCSI-compliant | **Exact Match** — `s23usbc.sys` maps `MAX77705` events directly to `UcmConnector*` DDIs |

On the dual-role controller side, `UrsCx.sys` binds to `URS0` (`ACPI\QCOM0C8B`) and guarantees mutually exclusive PnP lifetime between the Host PDO (`URS\QCOM0C8B&HOST` -> `usbxhci.sys`) and Function PDO (`URS\QCOM0C8B&FUNCTION` -> `ufxsynopsys.sys`). Microsoft's inbox `UrsSynopsys.sys` client driver cannot run dynamic SM8550 switching alone because it only toggles generic Synopsys `GCTL.PRTCAPDIR` registers without touching Qualcomm's `QSCRATCH` VBUS overrides (`+0xF8800`), SM8550's `eUSB2` PHY (`0x088E3000`), NXP's `PTN3222` repeater (`0x4F`), or QMP Combo PHY polarity registers (`0x088E8000`). Writing a custom KMDF `UrsCx` client (**`s23urs.sys`**) encapsulates all SM8550 glue logic cleanly:

1. **URS Initialization (`EVT_WDF_DRIVER_DEVICE_ADD`):** Call `UrsDeviceInitInitialize(DeviceInit)`, create the WDF device, populate `URS_CONFIG` with `UrsHostInterfaceTypeXhci`, `EvtUrsFilterRemoveResourceRequirements`, and `EvtUrsSetRole`, and invoke `UrsDeviceInitialize(Device, &UrsConfig)`.
2. **Hand Off Role Events to `UcmCx`:** Call `UrsSetHardwareEventSupport(Device, FALSE)`. On USB Type-C systems, Microsoft instructs the URS client *not* to report raw ID-pin hardware events; instead, `UcmCx` pairs `s23usbc.sys`'s connector to `URS0` via identical ACPI **`_PLD`** buffers and drives `EvtUrsSetRole` directly.
3. **Resource Forwarding (`EVT_URS_DEVICE_FILTER_RESOURCE_REQUIREMENTS`):** Append the `0x0A600000` MMIO window and shared DWC3 GIC interrupt into both the Host and Function `URSIORESLIST` lists via `UrsIoResourceListAppendDescriptor` (Microsoft explicitly permits sharing the same exclusive interrupt across both child stacks because only one runs at a time), while `s23urs.sys` maps `QSCRATCH` (`0x0A6F8800`), `eUSB2` (`0x088E3000`), `QMP` (`0x088E8000`), `GCC` (`0x00100000`), and `TCSR` (`0x01FD5108`/`0x01FD5118`) directly.
4. **Strict Mux-Before-Attach Ordering:** Microsoft's `UcmConnectorTypeCAttach` contract warns that **mux and SuperSpeed polarity configuration must complete *before* calling `UcmConnectorTypeCAttach`**, or Function mode drops to High-Speed instead of SuperSpeed. On attach, `s23usbc.sys` first invokes `s23urs.sys`'s internal callback to set `GPIO11` (`cc_dir`) and `QMP` polarity, then calls `UcmConnectorTypeCAttach`—triggering `UrsCx` -> `EvtUrsSetRole` -> `usbxhci.sys` or `ufxsynopsys.sys`.

### Exact Register-Level Transitions: DWC3, QSCRATCH, eUSB2, PTN3222, and QMP PHY
When `UrsCx` calls `EvtUrsSetRole`, PnP has already torn down the outgoing child stack (`usbxhci.sys` or `ufxsynopsys.sys`). Before programming DWC3 and PHY registers, `s23urs.sys` must verify that the GCC USB clocks/GDSCs (`0x00100000 + 0x39000..0x3908C`, `+0x50000..0x50018`, `+0x12000`) and TCSR clkrefs (`0x01FD5108`/`0x01FD5118`) remain ungated and that the EUD debug hub (`0x0C276000`) is disabled so it does not hijack the HS PHY.

#### 1. QSCRATCH (`0x0A6F8800`, `+0xF8800` from DWC3, size `0x400`) and DWC3 Core (`0x0A600000`, size `0xCD00`)

| Register | Offset | Host Mode (`usbxhci.sys`) | Device / Function Mode (`ufxsynopsys.sys`) | Engineering Purpose |
| --- | --- | --- | --- | --- |
| **`QSCRATCH_HS_PHY_CTRL`** | `0x0A6F8810` (`+0x10`) | **CLEAR** `BIT(20)` (`UTMI_OTG_VBUS_VALID`) and `BIT(28)` (`SW_SESSVLD_SEL`) | **SET** `BIT(20) \| BIT(28)` (`0x10100000`) | Triggers `ConnectDone` session-valid event in Device mode; must be cleared in Host mode |
| **`QSCRATCH_SS_PHY_CTRL`** | `0x0A6F8830` (`+0x30`) | **CLEAR** `BIT(24)` (`LANE0_PWR_PRESENT`) | **SET** `BIT(24)` (`0x01000000`) | Signals SuperSpeed VBUS presence to DWC3 link layer in Device mode |
| **`QSCRATCH_GENERAL_CFG`** | `0x0A6F8808` (`+0x08`) | Keep `BIT(0)` (`PIPE_UTMI_CLK_SEL`), `BIT(3)` (`PIPE3_PHYSTATUS_SW`), `BIT(8)` (`PIPE_UTMI_CLK_DIS`) **CLEARED** for USB 3.2 | Same (**CLEARED** for USB 3.2; set only if intentionally forcing HS-only fallback without QMP PIPE clock) | Selects QMP SuperSpeed PIPE clock vs UTMI HS fallback clock |
| **`DWC3_GCTL`** | Core `GCTL` | `PRTCAPDIR = HOST` (`2'b01`); clear `GBLHIBERNATIONEN` | `PRTCAPDIR = DEVICE` (`2'b10`); clear `GBLHIBERNATIONEN` | Switches Synopsys DWC3 core between xHCI Host and UFX Peripheral engines |
| **`DWC3_OCFG`** | Core `OCFG` | **SET** `SFTRSTMASK`; clear `SRPCAP`, `HNPCAP`, `DISPWRCUTTOFF` | **SET** `SFTRSTMASK`; clear `SRPCAP`, `HNPCAP` | Prevents `USBCMD.HCRST` (xHCI reset) from resetting PHY signals or OTG filters |
| **`DWC3_OCTL`** | Core `OCTL` | Clear `PERIMODE`, `TERMSELIDPULSE`, `DEVSETHNPEN`, `HSTSETHNPEN`; set `PRTPWRCTL = 1` | Set `PERIMODE = 1`; clear `TERMSELIDPULSE`, `HNPREQ`, `DEVSETHNPEN`, `HSTSETHNPEN`, `PRTPWRCTL` | Aligns internal OTG port power control with active role |
| **`DWC3_GUSB2PHYCFG(0)`** | Core `GUSB2PHYCFG0` | Clear `SUSPHY` (SM8550 enables `snps,dis_u2_susphy_quirk`) | Clear `SUSPHY` (`snps,dis_u2_susphy_quirk`) | Prevents eUSB2 UTMI clock gating hangs during mode transitions |

#### 2. SNPS eUSB2 HS PHY (`0x088E3000`) and NXP `PTN3222` Repeater (`0x4F` on QUP1 SE6)
In mainline `drivers/phy/phy-snps-eusb2.c`, SM8550's eUSB2 PHY (`qcom,sm8550-snps-eusb2-phy`) has no internal Host-vs-Device register divergence—`snps_eusb2_hsphy_set_mode` forwards mode transitions directly to the external repeater. When performing a full cold reset or recovering the HS PHY during role switching (after asserting and de-asserting the GCC eUSB2 PHY reset with a 100–150 µs delay), execute the verified 16-step sequence at `0x088E3000`:

1. Set `QCOM_USB_PHY_CFG0` (`+0x94`) `CMN_CTRL_OVERRIDE_EN` (`BIT 1`).
2. Set `QCOM_USB_PHY_UTMI_CTRL5` (`+0x50`) `POR` (`BIT 1`) = `1`.
3. Set `QCOM_USB_PHY_HS_PHY_CTRL_COMMON0` (`+0x54`) `PHY_ENABLE` (`BIT 0`) | `RETENABLEN` (`BIT 3`).
4. Set `QCOM_USB_PHY_APB_ACCESS_CMD` (`+0x130`) `APB_LOGIC_RESET` (`BIT 2`) = `1`.
5. Clear `QCOM_UTMI_PHY_CMN_CTRL0` (`+0x98`) `TESTBURNIN` (`BIT 6`) = `0`.
6. Set `QCOM_USB_PHY_FSEL_SEL` (`+0xB8`) `FSEL_SEL` (`BIT 0`) = `1`.
7. Program the **19.2 MHz reference clock**: `+0x54` `FSEL[6:4] = 0x0`; `QCOM_USB_PHY_CFG_CTRL_2` (`+0x5C`) `FB_DIV_7_0 = 0x90`; `QCOM_USB_PHY_CFG_CTRL_3` (`+0x60`) `FB_DIV_11_8[3:0] = 0x1` and `REF_DIV[7:4] = 0x0`.
8. Configure PLL loop registers: `CFG_CTRL_1` (`+0x58`) `CPBIAS[7:1] = 0x0`; `CFG_CTRL_4` (`+0x68`) `INT[7:2] = 0x8`, `GMP[1:0] = 0x1`; `CFG_CTRL_5` (`+0x6C`) `PROP[4:0] = 0x10`, `VREF_TUNE[7:6] = 0x1`; `CFG_CTRL_6` (`+0x70`) `VCO[2:0] = 0x0`.
9. Enable external VBUS detect select: `QCOM_USB_PHY_HS_PHY_CTRL2` (`+0x64`) `VBUS_DET_EXT_SEL` (`BIT 4`) = `1`.
10. Apply default Qualcomm TX tuning: `CFG_CTRL_9` (`+0x7C`) `PREEMP[2:0] = 0x0`, `RES[4:3] = 0x1`, `RISE[6:5] = 0x2`; `CFG_CTRL_8` (`+0x78`) `HS_VREF[5:3] = 0x3`, `HS_XV[7:6] = 0x0`.
11. Assert suspend release: `HS_PHY_CTRL2` (`+0x64`) set `USB2_SUSPEND_N_SEL` (`BIT 3`) | `USB2_SUSPEND_N` (`BIT 2`).
12. Exit sleep mode: `QCOM_USB_PHY_UTMI_CTRL0` (`+0x3C`) set `SLEEPM` (`BIT 0`).
13. Exit SIDDQ power-down: `HS_PHY_CTRL_COMMON0` (`+0x54`) set `SIDDQ_SEL` (`BIT 1`), then clear `SIDDQ` (`BIT 2`) = `0`.
14. Release POR: `UTMI_CTRL5` (`+0x50`) clear `POR` (`BIT 1`) = `0`.
15. Clear suspend override: `HS_PHY_CTRL2` (`+0x64`) clear `USB2_SUSPEND_N_SEL` (`BIT 3`) = `0`.
16. Clear common control override: `CFG0` (`+0x94`) clear `CMN_CTRL_OVERRIDE_EN` (`BIT 1`) = `0`.
For the paired **NXP `PTN3222`** repeater (`ADDR` pin tied directly to GND -> 7-bit I2C address `0x4F` on QUP1 SE6 `0x00A98000`), both NXP's product datasheet and mainline `drivers/phy/phy-nxp-ptn3222.c` confirm a critical simplification: once `vdd3v3` (`ldob5` 3.1 V, 11 mA load) and `vdd1v8` (`ldob15` 1.8 V, 55 mA load) are powered and active-low `RST_N` (`pm8550vs@3 gpio4`) is de-asserted (`0` in Linux active-high reset semantics, driving the physical pin high), **`PTN3222` exits POR in default Host repeater mode and transitions dynamically between Host and Peripheral (Device) repeater roles automatically via eUSB2 Extended SE1 (`ESE1`) signaling**, requiring zero I2C role writes. Note that while `RST_N` is asserted low, `PTN3222`'s `SCL`/`SDA` pins float Hi-Z and will NACK I2C probes. Optional signal-integrity tuning registers verified from NXP's datasheet are `0x05` (`eUSB2 TX CONTROL`, `BITS[1:0]` TX de-emphasis), `0x06` (`USB2 RX CONTROL`, `BITS[6:4]` HS RX squelch threshold default `101` = 125 mV; `BITS[2:0]` HS RX EQ default `000` = 0 dB), and `0x07` (`USB2 TX CONTROL 1`, `BITS[5:4]` TX de-emphasis duration default `00` = 0 UI; `BITS[2:0]` TX de-emphasis default `000` = 0 dB). (Register indices `0x00–0x04`, registers above `0x07`, and exact `RST_N` pulse durations were omitted in the browsed datasheet excerpt, though mainline `phy-nxp-ptn3222.c` leaves all I2C registers at OTP defaults.)

#### 3. QMP USB3+DP Combo SS PHY (`0x088E8000`) and Hot-Plug CC Polarity Flipping
SM8550's QMP Combo PHY (`sm8550_usb3dpphy_cfg`) uses the **`qmp_combo_offsets_v3`** layout: `.com = 0x0000`, `.usb3_serdes = 0x1000`, `.txa = 0x1200`, `.rxa = 0x1400`, `.txb = 0x1600`, `.rxb = 0x1800`, and `.usb3_pcs_misc = 0x1A00`. Dynamic hot-plugging requires `s23urs.sys` to reset and re-orient the COM mux whenever cable orientation flips between CC1 and CC2 or DisplayPort Alt-Mode engages:

| QMP COM Offset (`0x088E8000 +`) | Register Name | Bitfields & Values | Usage During Hot-Plug / Role Switch |
| --- | --- | --- | --- |
| `+0x0000` | `USB3_DP_COM_PHY_MODE_CTRL` | `BIT(0)` = `USB3_MODE` (`0x01`); `BIT(1)` = `DP_MODE` (`0x02`); `0x03` = `USB3_DP_COMBO_MODE` | Write `0x01` (USB3 only), `0x02` (4-lane DP Pin C), or `0x03` (2-lane DP + USB3 Pin D); flush with dummy read |
| `+0x0004` | `USB3_DP_COM_SW_RESET` | Software reset bit | Assert before modifying `PHY_MODE_CTRL` or flipping `TYPEC_CTRL`, then re-run `s23_qmp_seq.h` USB3 SERDES/TX/RX/PCS init |
| `+0x0010` | `USB3_DP_COM_TYPEC_CTRL` | `BIT(0)` = `SW_PORTSELECT` (`0` = Lane A / CC1, `1` = Lane B / CC2); `BIT(1)` = `SW_PORTSELECT_MX` (`1` = SW control) | Write **`0x02`** for CC1 normal orientation, **`0x03`** for CC2 flipped orientation |
| `+0x0014` | `USB3_DP_COM_TYPEC_PWRDN_CTRL` | Type-C power-down control | Assert during disconnect / USB SAFE transition between modes |
| `SWI_CTRL` | `USB3_DP_COM_SWI_CTRL` | `BIT(0)` = `USB3_SWI_ACT_ACCESS_EN`; `BIT(1)` = `DP_SWI_ACT_ACCESS_EN` | Arbitrates lane-register access between USB3 and DP blocks |

Between Host and Device roles, the QMP USB3 PHY runs the exact same SERDES/TX/RX/PCS calibration tables (`s23_qmp_seq.h`), differing primarily in power gating (`msm_ssphy_power_enable` shuts down LDOs when disconnected in Device mode) and autonomous suspend detection (`ALFPS_DTCT_EN` when already in SuperSpeed versus `ARCVR_DTCT_EVENT_SEL` for Rx-detect). Always cycle the QMP PHY through reset/SAFE state when changing `TYPEC_CTRL` polarity on a reversed cable plug-in so SuperSpeed Gen 1/2 link training succeeds in both orientations.

## 4. USB-C DisplayPort Alt-Mode (4K60 and DP Audio) and WDDM 2.0 Integration
Driving an external 4K60 display alongside the S23's internal `1080×2340 @ 120 Hz` S6E3FAC AMOLED panel requires coordinating four SM8550 blocks—MDSS DPU 9.0 (`0x0AE00000`), `DISP_CC` (`0x0AF00000`), QMP Combo PHY (`0x088E8000`), and the DisplayPort Controller (`0x0AE90000`)—while upgrading `s23gpu.sys` into a dual-source, dual-target WDDM 2.0 miniport.

### DisplayPort Bandwidth Budget: Pin Assignment C (4-Lane) vs. Pin Assignment D (2-Lane)
SM8550's QMP v6 DP SERDES supports all four DisplayPort 1.4 link rates: RBR (`1.62 Gbps/lane`), HBR (`2.70 Gbps/lane`), HBR2 (`5.40 Gbps/lane`), and HBR3 (`8.10 Gbps/lane`). Applying 8b/10b channel coding efficiency (`0.80`) yields usable payload capacities of `2.592 / 4.320 / 8.640 / 12.960 Gbps` on 2 lanes (Pin Assignment D) and `5.184 / 8.640 / 17.280 / 25.920 Gbps` on 4 lanes (Pin Assignment C). Comparing those ceilings against uncompressed 24 bpp (8 bpc RGB) video modes—`1080p60` (`148.5 MHz × 24 = 3.564 Gbps`), `1440p60` (`241.5 MHz × 24 = 5.796 Gbps`), `4K30` (`297.0 MHz × 24 = 7.128 Gbps`), `4K60 CVT-RBv2` (`533.25 MHz × 24 = 12.798 Gbps`), and standard `4K60 CTA-861` (`594.0 MHz × 24 = 14.256 Gbps`)—establishes why Pin Assignment C is required for universal 4K60 RGB compatibility.

### SM8550 MDSS DPU 9.0 Pipeline Routing Alongside DSI INTF_1
In mainline SM8550 DPU 9.0 catalog definitions (`dpu_9_0_sm8550.h`), **`INTF_0`** (`INTF_DP`, bound to `MSM_DP_CONTROLLER_0`, interrupt line `24`, `INTF_SC7280_MASK`) sits at offset **`0x34000`** from MDP base (`0x0AE01000`), mapping to physical **`0x0AE35000`** (size `0x280`)—one `0x1000` stride below the repository's verified DSI **`INTF_1`** at **`0x0AE36000`** (`0x0AE01000 + 0x35000`).
To drive both displays simultaneously without stalling or tearing the internal 120 Hz S6E3FAC panel, `s23gpu.sys` assigns disjoint DPU blocks to each active VidPN target. Target 0 (Internal DSI, `1080×2340 @ 120 Hz`) keeps its existing `CTL`, `SSPP`, `LM`, `PingPong`, and `INTF_1` (`0x0AE36000`). Target 1 (External USB-C DP) allocates a separate `CTL` (such as `CTL_1` or `CTL_2`), separate `SSPP` pipe(s) (SM8550 exposes `DMA0–DMA5` plus `VIG` pipes), separate Layer Mixer(s) (`LM_0` at MDP `+0x44000` = `0x0AE45000` and paired `LM`s via `.lm_pair`), and separate `PingPong`(s) feeding **`INTF_0`** (`0x0AE35000`) in active Video Mode. Because single Qualcomm Layer Mixers have a hardware horizontal scanout width limit below 3840 pixels, Phase 4 bring-up should first validate `1920×1080 @ 60 Hz` on a single `SSPP` + single `LM`, and then enable **dual-LM split** (splitting left 1920 px and right 1920 px across two paired SSPPs/LMs into merged `INTF_0`) paired with **DP wide-bus** (2 pixels per clock across DPU->DP, halving the internal `INTF_0` pixel-clock domain from 594 MHz to 297 MHz for 4K60).

### DisplayPort Controller (0x0AE90000), DISP_CC (0x0AF00000), and QMP DP PHY (0x088E8000)
The SM8550 DisplayPort controller (`qcom,sm8550-dp`, compatible with `qcom,sm8350-dp`) exposes four register apertures clocked by three `DISP_CC` (`0x0AF00000`) Root Clock Generators sourced from the QMP Combo PHY (`0x088E8000`):

| Hardware Block | Physical Address / Offset | Verified Registers, Sub-Blocks & Clock Parents |
| --- | --- | --- |
| **DP AHB** | `0x0AE90000` (size `0x200`) | Top-level DP controller AHB control, SW reset, global IRQ status/mask |
| **DP AUX** | `0x0AE90200` (size `0x200`) | `REG_DP_AUX_CTRL` (`+0x30`, `DP_AUX_CTRL_ENABLE = BIT(0)`), `REG_DP_AUX_DATA`, `REG_DP_AUX_TRANS_CTRL` |
| **DP LINK** | `0x0AE90400` (size `0x600–0xC00`) | `REG_DP_MAINLINK_CTRL`, `REG_DP_STATE_CTRL` (`DP_STATE_CTRL_PUSH_IDLE`), `REG_DP_CONFIGURATION_CTRL`, `REG_DP_MAINLINK_LEVELS`; Audio/SDP (`MMSS_DP_AUDIO_CFG` `+0x600`, `MMSS_DP_AUDIO_ACR_CTRL` `+0x610`, `AUDIO_CTRL_RESET` `+0x614`, `SDP_CFG`, `SDP_CFG2`) |
| **DP P0 (Stream 0)** | `0x0AE91000` (size `0x400`) | Video timing generator (H/V active, blanking, sync), MSA, Transfer Unit (TU), MVID/NVID |
| **`DISP_CC` AUX RCG** | `0x0AF081BC` (`+0x81BC`) | `disp_cc_mdss_dptx0_aux_clk_src` (ID `13`, `mnd_width = 0`), parent = 19.2 MHz XO |
| **`DISP_CC` LINK RCG** | `0x0AF08170` (`+0x8170`) | `disp_cc_mdss_dptx0_link_clk_src` (ID `16`), parent = QMP `QMP_USB43DP_DP_LINK_CLK` (162/270/540/810 MHz) |
| **`DISP_CC` PIXEL0 RCG** | `0x0AF0818C` (`+0x818C`) | `disp_cc_mdss_dptx0_pixel0_clk_src` (`mnd_width = 16`), parent = QMP DP VCO divider clock |
| **QMP v6 DP PHY** | `0x088E8000` (`qmp_combo_offsets_v3`) | `qmp_v6_dp_serdes_tbl` + rate tables (`_rbr`, `_hbr`, `_hbr2`, `_hbr3`), `qmp_v6_dp_tx_tbl`, swing/pre-emphasis (`v5`/`v6` tables), `qmp_v4_dp_aux_init`, `qmp_v4_configure_dp_tx`, `qmp_v3_configure_dp_clocks`, `qmp_v4_configure_dp_phy` (`QSERDES_DP_PHY_MODE` = **`0x5C`** normal / **`0x4C`** reverse) |

Two implementation caveats apply directly to SM8550 DP bring-up. First, upstream Linux 6.8.12 identified an SM8550 DisplayPort link-clock OPP bug caused by inheriting outdated frequency tables (`_opp_config_clk_single: failed to set` on `ae90000.displayport-controller`); program `dptx0_link_clk_src` directly from the QMP DP symbol clock rate instead of copying legacy OPP tables. Second, exact `DISP_CC` CBCR branch gate offsets and complete `dp_reg.h` / `qmp_v6_dp_serdes_tbl_*` register arrays were unavailable in the browsed extracts and must be imported directly from mainline Linux `drivers/clk/qcom/dispcc-sm8550.c`, `drivers/phy/qualcomm/phy-qcom-qmp-combo.c`, and `drivers/gpu/drm/msm/dp/dp_reg.h`.

### DP AUX, DPCD, EDID, and Link Training Sequence
When `s23usbc.sys` completes DP Alt-Mode (`SVID 0xFF01`, Pin C or D) and signals HPD-High to `s23gpu.sys`, `s23gpu.sys` executes the following bring-up sequence:

1. **AUX Path and Clock Initialization:** Confirm `GPIO11` (`samsung,cc_dir` / `DG2730`) matches cable orientation; ensure `MDSS` GDSC and `DISP_CC` `dptx0_aux_clk` (`0x81BC`) are active; run `qmp_v4_dp_aux_init` and enable the DP AUX controller (`REG_DP_AUX_CTRL = DP_AUX_CTRL_ENABLE` at `0x0AE90230`).
2. **DPCD Capability Read:** Perform native DP AUX reads (`0x00000–0x0000F`) for `DPCD_REV`, `MAX_LINK_RATE` (`0x06` RBR, `0x0A` HBR, `0x14` HBR2, `0x1E` HBR3), `MAX_LANE_COUNT` (clamped to `2` for Pin Assignment D or `4` for Pin Assignment C), and `SINK_COUNT` (`0x00200`).
3. **EDID Acquisition via I2C-over-AUX:** Read 128-byte EDID blocks from I2C address `0x50` in 16-byte MOT-flagged AUX transactions, inspect extension count at byte `126`, read CEA-861 extension blocks (using E-DDC segment pointer `0x30` beyond 256 bytes), validate checksums, and cache the raw blocks in `s23gpu.sys`.
4. **QMP DP PLL and Mainlink Clocks:** Select the target link rate and lane count; program `QSERDES_DP_PHY_MODE` (`0x5C` normal / `0x4C` reverse); write `qmp_v6_dp_serdes_tbl` + rate table (`_rbr`/`_hbr`/`_hbr2`/`_hbr3`) and `qmp_v6_dp_tx_tbl`; lock the DP PLL; and enable `DISP_CC` `dptx0_link_clk_src` (`0x8170`) and `dptx0_pixel0_clk_src` (`0x818C`).
5. **Link Training (Clock Recovery and Channel Equalization):** Program DPCD `0x00100` (`LINK_BW_SET`) and `0x00101` (`LANE_COUNT_SET` + Enhanced Framing); emit TPS1 via `REG_DP_STATE_CTRL` and DPCD `0x00102`, stepping `qmp_dp_v5/v6` voltage-swing and pre-emphasis tables until all lanes pass CR (`0x00202`/`0x00203`); advance to TPS2/TPS3/TPS4 until Channel Equalization, Symbol Lock, and Interlane Alignment pass; clear training patterns, program `P0` timing/MSA/TU registers, enable `REG_DP_MAINLINK_CTRL` and `REG_DP_CONFIGURATION_CTRL`, and start `INTF_0` scanout.

### WDDM 2.0 Multi-Target VidPN Integration in s23gpu.sys
Upgrading `s23gpu.sys` to expose native Windows 11 multi-monitor controls (Extend, Duplicate/Clone, Second Screen Only, refresh-rate selection) follows Microsoft's WDDM 2.0 child-device and hot-plug contract. In `DxgkDdiStartDevice`, bump `NumberOfVideoPresentSources` from `1` to **`2`** (`Source 0` = internal DSI, `Source 1` = external DP) and `NumberOfChildren` from `1` to **`2`**. In `DxgkDdiQueryChildRelations`, always report **both** onboard children at boot regardless of whether a cable is attached: Child 0 (`ChildUid = 0`, `TypeVideoOutput`, `HpdAwarenessAlwaysConnected`) and Child 1 (`ChildUid = 1`, `TypeVideoOutput`, `D3DKMDT_VOT_DISPLAYPORT_EXTERNAL`, **`HpdAwarenessInterruptible`**).
Because HPD events originate from `MAX77705` VDM interrupts rather than a display hardware IRQ, `s23gpu.sys` should expose a kernel callback interface (`GUID_S23_DP_HPD_BRIDGE`) to `s23usbc.sys`. On HPD-High, `s23gpu.sys` executes DPCD/EDID reads on a passive worker thread and invokes **`DxgkCbIndicateChildStatus`** with `DXGK_CHILD_STATUS { ChildUid = 1, Type = StatusConnection, HotPlug.Connected = TRUE }`. On HPD-Low or cable detach, `s23gpu.sys` calls `DxgkCbIndicateChildStatus(Connected = FALSE)`, halts `INTF_0` and DP clocks, and releases QMP DP lanes back to `s23urs.sys`. On short `IRQ_HPD` pulses, `s23gpu.sys` inspects DPCD `0x200–0x205` and silently retrains the link without tearing down the monitor PDO unless link recovery fails.
When `dxgkrnl` and `monitor.sys` invoke `DxgkDdiQueryDeviceDescriptor` for `ChildUid = 1`, `s23gpu.sys` serves cached 128-byte EDID blocks starting at `DeviceDescriptor->DescriptorOffset` without exceeding `DescriptorLength`, returning `STATUS_MONITOR_NO_MORE_DESCRIPTOR_DATA` past the last cached extension block (Windows intentionally calls `DxgkDdiQueryDeviceDescriptor` twice for block 0—once at PnP PDO enumeration and again from `monitor.sys`). In `DxgkDdiEnumVidPnCofuncModality` and `DxgkDdiCommitVidPn`, support four functional topologies: Internal Only (`S0 -> T0`), Extend (`S0 -> T0` + `S1 -> T1` using two independent primary allocations and DPU chains), Clone (`S0 -> T0` + `S0 -> T1` scanning out the same primary allocation from both SSPPs), and External Only (`S0 -> T1` or `S1 -> T1`, blanking DSI for desktop-mode 4K60). Report independent VSync interrupts (`DxgkCbNotifyInterrupt`) tagged with `VidPnTargetId` (`120 Hz` for `T0`, `60 Hz` for `T1`).

### DisplayPort Audio Delivery Options and the SM8550 LPASS Constraint
Programming the DisplayPort controller's audio framing engine inside `s23gpu.sys` is fully documented in mainline `drivers/gpu/drm/msm/dp/dp_audio.c`. Specifically, `msm_dp_audio_prepare()` sets SDP enable flags in `MMSS_DP_SDP_CFG` (`BIT(1)` Timestamp, `BIT(2)` Stream, `BIT(5)` Copy Management, `BIT(6)` ISRC, `BIT(20)` InfoFrame), clears register-source override bits in `MMSS_DP_SDP_CFG2` (`BIT(0..1)`), writes the five SDP header pairs (`STREAM` with `HB1=0x02, HB3=channels-1`; `TIMESTAMP` with `HB1=0x01, HB2=0x17, HB3=0x44`; `INFOFRAME` with `HB1=0x84, HB2=0x1B, HB3=0x44`; `COPYMANAGEMENT` with `HB1=0x05, HB2=0x0F`; and `ISRC` with `HB1=0x06, HB2=0x0F`), programs Audio Clock Recovery in `MMSS_DP_AUDIO_ACR_CTRL` (`+0x610`: `(bw_select << 4) | BIT(31) | BIT(14) | BIT(8)` for `0`=1.62G, `1`=2.7G, `2`=5.4G, `3`=8.1G), sets `safe_to_exit_level` in `REG_DP_MAINLINK_LEVELS[4:0]` (`14` for 1 lane, `8` for 2 lanes, `5` for 4 lanes), and asserts `BIT(0)` in `MMSS_DP_AUDIO_CFG` (`+0x600`) followed by a write barrier.
However, SM8550's DP block contains **no CPU-writable PCM DMA FIFO**—audio samples arrive over an internal hardware bus from **LPASS (`LPASS_DP_RX` / AudioReach `DISPLAY_PORT_RX_0`)**. On retail Qualcomm Gunyah/TrustZone builds, direct CPU access to LPASS LPAIF DMA registers (`0x06000000–0x06FFFFFF`) is XPU-locked to `VMID_LPASS` and triggers an immediate bus hang or kernel panic. External audio delivery therefore splits into three clear engineering options:

| Audio Delivery Path | Engineering Prerequisites | Readiness & Trade-Off |
| --- | --- | --- |
| **1. USB-C Audio (`usbaudio2.sys`) & Bluetooth A2DP (`BthA2dp.sys`)** | Phase 3 xHCI Host Mode (for USB-C DACs/hub audio) or existing `s23bthx.sys` (already operational) | **Immediate / Recommended First:** Zero ADSP dependency; works simultaneously with 4K60 Pin Assignment C (over USB 2.0 `PTN3222`) or Pin Assignment D docks |
| **2. Native DP Monitor Audio (ADSP PIL + AudioReach)** | Boot ADSP via PAS (`pas_id = 1`, `dtb_pas_id = 0x24`), bring up GLink `adsp_apps` + GPR + AudioReach (`MODULE_ID_WR_SHARED_MEM_EP 0x07001000` -> `MODULE_ID_DISPLAY_PORT_SINK 0x07001069`, `PARAM_ID_DISPLAY_PORT_INTF_CFG 0x08001154`), add `dp_audio` setup to `s23gpu.sys`, and expose a PortCls `IMiniportWaveRT` miniport | **Long-Term Native Path:** High bring-up cost; note SM8550 Linux reports (`debian-piano`) warn that starting ADSP without coordinated display/PMIC handoffs can cause an unrecoverable DSI black screen |
| **3. Direct APPS LPASS LPAIF DMA** | TrustZone XPU unlock (unavailable on retail Samsung firmware) | **Blocked on Retail Hardware:** Do not touch `0x06000000` LPAIF space without explicit XPU-open confirmation |

## 5. ACPI Updates, Driver Architecture, Phased Roadmap, and Safety Rules

### Exact ACPI DSDT / SSDT Updates (`_UPC`, `_PLD`, `URS0`, `UCM0`, `PMIC`)
Windows links the USB Type-C connector (`UcmCx`) to the dual-role controller (`UrsCx`) exclusively through **byte-identical `_PLD` (Physical Location Description)** buffers on the connector node and the xHCI/UFX root hub ports. Because `MAX77705` is neither UCSI nor TCPCI, ACPI defines a custom `UcmCx` device (`S23U7705`) rather than a UCSI `USBC000` or TCPCI port node, sets `_UPC` Element 0 (`PortIsConnectable`) to `0xFF`, Element 1 (`PortConnectorType`) to **`0x09`** (*Type-C connector — USB2 and SS with Switch*), and `_PLD` Bit 64 (`UserVisible`) to `1` with Panel bits `[69:67]` populated. Remember to convert GIC SPI interrupt numbers from `s23_live.dts` to ACPI GSIs via `GIC_SPI + 32`:

```code snippet
Scope (\_SB)
{
    // 1. Unified MAX77705 + SE8 I2C Arbiter (s23pmic.sys)
    //    Replaces raw bit-bang in s23batt.sys; owns QUPv3_I2C_HUB SE8 + GPIO133
    Device (PMIC)
    {
        Name (_HID, "S23P7705")
        Name (_UID, 0)
        Method (_CRS, 0, NotSerialized)
        {
            Name (RBUF, ResourceTemplate ()
            {
                // QUPv3_I2C_HUB SE8 (0x009A0000, 16 KiB)
                Memory32Fixed (ReadWrite, 0x009A0000, 0x00004000)
                // Add SE8 GIC SPI (GIC_SPI + 32) and GPIO133 PDC/TLMM IRQ
                // verified from s23_live.dts in Phase 0
            })
            Return (RBUF)
        }
    }

    // 2. USB Type-C Connector Manager (s23usbc.sys -> UcmCx.sys)
    Device (UCM0)
    {
        Name (_HID, "S23U7705")
        Name (_UID, 0)
        Name (_DEP, Package () { \_SB.PMIC })

        Device (CON0)
        {
            Name (_ADR, 0x00000000)
            // Type-C USB2 + SuperSpeed with Switch (0x09)
            Name (_UPC, Package (4) { 0xFF, 0x09, 0x00000000, 0x00000000 })
            // Shared _PLD: Revision=2, UserVisible=1 (Bit 64), Panel=Bottom,
            // GroupToken=1, GroupPosition=1 — MUST match URS0.USB0.RHUB & UFN0!
            Name (_PLD, Package (1)
            {
                ToPLD (
                    PLD_Revision           = 0x2,
                    PLD_IgnoreColor        = 0x1,
                    PLD_Panel              = "BOTTOM",
                    PLD_VerticalPosition   = "CENTER",
                    PLD_HorizontalPosition = "CENTER",
                    PLD_Shape              = "HORIZONTALOVAL",
                    PLD_UserVisible        = 0x1,
                    PLD_GroupToken         = 0x1,
                    PLD_GroupPosition      = 0x1
                )
            })
        }
    }

    // 3. Dual-Role DWC3 Controller (s23urs.sys -> UrsCx.sys)
    //    Remove static RoleSwitchMode=2 registry override when entering Phase 3
    Device (URS0)
    {
        Name (_HID, "QCOM0C8B")
        Name (_UID, 0)
        Name (_CCA, 0) // Safe default under SMMU bypass; promote to 1 only if verified coherent
        Name (_DEP, Package () { \_SB.UCM0 })

        Method (_CRS, 0, NotSerialized)
        {
            Name (RBUF, ResourceTemplate ()
            {
                // Full DWC3 + QSCRATCH (+0xF8800) aperture
                Memory32Fixed (ReadWrite, 0x0A600000, 0x00100000)
                // Keep existing DWC3 core GIC SPI interrupt (GIC_SPI + 32) from DSDT;
                // s23urs.sys maps eUSB2 (0x088E3000), QMP (0x088E8000), and QUP1 SE6 (0x00A98000)
            })
            Return (RBUF)
        }

        // Child 0: Host Stack -> HWID URS\QCOM0C8B&HOST -> usbxhci.sys
        Device (USB0)
        {
            Name (_ADR, 0x00000000)
            Name (_CCA, 0)
            Device (RHUB)
            {
                Name (_ADR, 0x00000000)
                Device (HS01)
                {
                    Name (_ADR, 1)
                    Name (_UPC, Package (4) { 0xFF, 0x09, 0x00000000, 0x00000000 })
                    Alias (\_SB.UCM0.CON0._PLD, _PLD)
                }
                Device (SS01)
                {
                    Name (_ADR, 2)
                    Name (_UPC, Package (4) { 0xFF, 0x09, 0x00000000, 0x00000000 })
                    Alias (\_SB.UCM0.CON0._PLD, _PLD)
                }
            }
        }

        // Child 1: Function Stack -> HWID URS\QCOM0C8B&FUNCTION -> ufxsynopsys.sys
        Device (UFN0)
        {
            Name (_ADR, 0x00000001)
            Name (_CCA, 0)
            Name (_UPC, Package (4) { 0xFF, 0x09, 0x00000000, 0x00000000 })
            Alias (\_SB.UCM0.CON0._PLD, _PLD)
        }
    }
}

```
Ship two companion INFs alongside `s23urs.sys` (`ACPI\QCOM0C8B`) and `s23usbc.sys` (`ACPI\S23U7705`): a Host INF matching `URS\QCOM0C8B&HOST` via `Include=usbxhci.inf` / `Needs=Generic.Install.NT`, and a Function INF matching `URS\QCOM0C8B&FUNCTION` via `Include=ufxsynopsys.inf` / `Needs=UfxSynopsys.NT`. On the first Host Mode boot, verify the exact xHCI root-hub port indices (`HS01` vs `SS01` `_ADR`) with `USBView` and swap `1`/`2` if SM8550's xHCI maps SuperSpeed to Port 1.

### Windows Driver Architecture and Toolchain Allocation

| Driver Binary | Framework & Class Extension | Mandatory Toolchain | Rationale |
| --- | --- | --- | --- |
| **`s23usbprobe.sys`** | Minimal WDM probe | WSL `clang-18` | Rapid Phase 0 read-only audit (SE8 GENI check, `MAX77705` `0x25` dump, SMMU SID check, `GPIO11` state) |
| **`s23pmic.sys`** | KMDF MFD Bus/Arbiter | MSVC + WDK `10.0.26100.6584` | Native `WdfDeviceAddQueryInterface`, `WdfInterrupt`, serialized `FAST_MUTEX` I2C arbiter |
| **`s23batt.sys`** | Battery Miniclass (updated) | WSL `clang-18` or MSVC+WDK | Remove TLMM bit-bang; consume `GUID_S23_PMIC_BUS_INTERFACE` from `s23pmic.sys` |
| **`s23usbc.sys`** | **KMDF `UcmCx` Client** | **MSVC + WDK `10.0.26100.6584`** | WDF class extension (`ucmmanager.h` / `UcmCx.h`) |
| **`s23urs.sys`** | **KMDF `UrsCx` Client** | **MSVC + WDK `10.0.26100.6584`** | Links `Urscxstub.lib` (`ursdevice.h` / `Urscx.h`), minimum KMDF 1.15 |
| **`s23gpu.sys`** | WDDM 2.0 KMD (updated) | MSVC + WDK `10.0.26100.6584` | Already built on MSVC+WDK; add `INTF_0`, `0x0AE90000` DP engine, `NumSources=2`/`NumChildren=2` |

### Phased Implementation Roadmap
Executing bring-up in six gated phases isolates bus ownership, USB-PD charging, dual-role switching, and DisplayPort video while enforcing a mandatory `s23link.py` recovery check at every transition.

1. **Phase 0 — Read-Only Hardware & Bus Audit (`s23usbprobe.sys`):** While `s23link.py` remains untouched, probe (a) `QUPv3_I2C_HUB SE8` (`0x009A0000`) `GENI_FW_REVISION_RO` and `GENI_IF_DISABLE_RO` to confirm FIFO mode availability (mirroring the verified UART14 probe), (b) `MAX77705` `0x66` `PMICID1`/`PMICREV`/`INTSRC_MASK` and `0x25` `0x00–0x11` in both USB-C plug orientations, (c) `GPIO11` (`samsung,cc_dir`) plus `s23_live.dts` for any secondary AUX/SBU switch pins or undocumented redriver nodes, and (d) UEFI-inherited APPS SMMU `S2CR` and `QSCRATCH` (`0x0A6F8808`/`10`/`30`) values.
2. **Phase 1 — Race-Free Bus & PHY Decoupling (`s23pmic.sys` + `s23batt.sys` + `s23touch.sys`):** Deploy `s23pmic.sys` owning `SE8` (`0x009A0000`) and `GPIO133`, migrate `s23batt.sys` onto `GUID_S23_PMIC_BUS_INTERFACE`, and remove QMP PHY init from `s23touch.sys`. *Exit Gate:* Battery telemetry updates cleanly with zero I2C NACKs under concurrent `s23link.py` transfers and touch input.
3. **Phase 2 — `MAX77705` OPCODE Mailbox, Fixed PDO Sink, and `UcmCx` Skeleton (`s23usbc.sys`):** Implement the serialized OPCODE queue (`0x21–0x41` / `0x51–0x71`), interrupt-driven CC/BC1.2/MUIC state machine, and Fixed PDO negotiation (`0x31`/`0x32` for 5 V/3 A and 9 V/2 A into `MAX77705` buck `0x69`) while keeping `URS0` locked to Peripheral mode (`RoleSwitchMode=2`). *Exit Gate:* USB-C PD chargers negotiate 9 V Fixed PDO reliably while PC attach preserves `s23link.py` and logs CC1 vs CC2 polarity accurately.
4. **Phase 3 — Dynamic Dual-Role USB 2.0 + USB 3.2 SuperSpeed (`s23urs.sys` + ACPI `_UPC`/`_PLD`):** Remove `RoleSwitchMode=2`, deploy `s23urs.sys` (`UrsCx` client) with `QSCRATCH`, `eUSB2`, `PTN3222`, and `QMP` hot-plug CC lane flipping, enable OTG 5 V VBUS boost (`0x69` `CHG_CNFG_00`) for passive UFP devices, and implement `DR_SWAP` (`OPCODE 0x37 = 0x01`) for powered PD hubs. *Exit Gate:* Ten consecutive hot-swap cycles pass cleanly between PC (`s23link.py` WinUSB at USB 3.2 SS in both plug orientations), passive flash drive/keyboard/UAC2 DAC (`usbxhci.sys` Host + 5 V OTG), and powered PD hub (simultaneous 9 V charging + Host Mode) without rebooting.
5. **Phase 4 — DisplayPort Alt-Mode 1080p60 to 4K60 in `s23gpu.sys`:** Enable `MAX77705` Alt-Mode (`OPCODE 0x55`) and VDM `0xFF01` (`0x48`/`0x4B`), connect the `s23usbc.sys` -> `s23gpu.sys` HPD bridge, bring up `DISP_CC` (`0x8170`/`0x818C`/`0x81BC`), QMP DP PLL (`qmp_v6_dp_serdes_tbl_*`), `0x0AE90000` AUX/DPCD/EDID/Link Training, and DPU `INTF_0` (`0x0AE35000`). Validate **1080p60 single-LM** first (confirming WDDM 2.0 `NumSources=2`, `NumChildren=2`, Clone/Extend/External-Only), then enable **4K60 dual-LM + wide-bus** on Pin Assignment C (4-lane HBR2) and Pin Assignment D (2-lane DP + USB 3.2).
6. **Phase 5 — 25 W PPS Direct Charging (`PCA9481`) & Native DP Audio:** Extract the verbatim `PCA9481` register map from Samsung's `SM-S911B` kernel drop, implement a 250 ms PPS regulation loop (`OPCODE 0x3A`/`0x3C`) with hardware watchdog and thermal foldback, and—only once ADSP PIL and AudioReach are operational—wire `MODULE_ID_DISPLAY_PORT_SINK` (`0x07001069`) to `s23gpu.sys`'s `dp_audio` SDP/ACR path.

### Electrical and Recovery Safety Rules
Protecting the S23's USB-C power stage and preserving `s23link.py` access require four non-negotiable hardware and software interlocks. First, to prevent **VBUS back-feeding or dual-source collisions**, `s23usbc.sys` must never write OTG 5 V boost into `MAX77705` (`0x69` `CHG_CNFG_00`) unless `REG_CC_STATUS0.CCStat` confirms a UFP sink is attached, `REG_BC_STATUS.VBUSDet == 0`, AND `REG_CC_STATUS1.VSAFE0V == 1`. Before executing any `PR_SWAP` to Sink, disable OTG boost immediately; when a powered PD hub attaches, stay in Power Sink and issue `DR_SWAP` (`0x37 = 0x01`) only, never OTG boost. Second, to enforce **VCONN, short-circuit, and moisture protection**, arm `MAX77705` hardware short-circuit detection during init via `OPCODE 0x56` payload `0x07` (VBUS-CC, SBU-GND, VBUS-SBU) and monitor `REG_CC_INT` `VCONNOCPI` (`BIT 7`) and `VCONNSCI` (`BIT 4`); on any `OPCODE_READ_SBU` (`0x25`) water alert, immediately force MUIC `COM_OPEN`, drop VBUS boost and VCONN, and refuse HV PD contracts until dry.
Third, enforce **hardware watchdogs on every charging path**: keep `CHG_CNFG_00_WDTEN_MASK` enabled on `MAX77705` (`0x69`) so a Windows bugcheck safely resets charger state instead of overcharging the cell, and never enable `PCA9481` (`0x57`) without its hardware watchdog enabled, `V_FLOAT` clamped to cell specification, and automatic fallback to `MAX77705` buck charging on any I2C timeout or cell temperature exceeding 45 °C. Fourth, build a **two-layer `s23link.py` recovery kill-switch** into `s23urs.sys` before removing `RoleSwitchMode=2`: a persistent registry override (`HKLM\SYSTEM\CurrentControlSet\Services\s23urs\Parameters\ForcePeripheral = 1`) plus a boot-time physical key check (holding **Volume Down** during Windows driver load forces `s23urs.sys` to bypass `UcmCx`, assert `QSCRATCH` VBUS overrides, and lock `UFN0` into Peripheral mode). Always validate ACPI SSDT/DSDT updates via a secondary UEFI boot image before updating the primary slot, and gate every `QMP`, `DP`, and `MDSS` MMIO access behind explicit clock and GDSC-active checks to avoid unclocked ARM64 SError panics.

---

Source: https://gemini.google.com/app/ea6c52275d2599c4
Exported at: 2026-11-23T13:36:27.009Z