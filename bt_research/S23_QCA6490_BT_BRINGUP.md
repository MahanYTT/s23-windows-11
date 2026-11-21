# Samsung Galaxy S23 (`dm1q` / `SM-S911B`, `SM8550-AC`) — QCA6490 Bluetooth 5.3 Bring-Up Blueprint

**Date:** 2026-11-21  
**Target:** Qualcomm QCA6490 (WCN6855 / Hastings Prime / FastConnect 6900) Bluetooth 5.3 over `qupv3_se14_4uart`  
**Why Bluetooth First:** Unlike native S23 speakers (blocked behind ADSP/LPASS + SoundWire + dual Cirrus `CS35L43` DSPs), QCA6490 Bluetooth connects directly to APPS over a **FIFO-capable 4-wire GENI HSUART** (`0x00898000`) with **zero ADSP dependency**, **zero GPI/GSI DMA requirement**, and **S23's exact stock Bluetooth firmware already on disk**. Bringing up BT 5.3 immediately unlocks **Bluetooth Audio (A2DP/HFP)** and **Bluetooth Keyboard/Mouse/Gamepad (HID)** on Windows 11 ARM64.

---

## 1. Verified S23 Hardware Map (`s23_live.dts` + `gcc-kalama.c` + `pinctrl-kalama.c` + `DSDT_MTP.dsl`)

### 1.1 GENI HSUART Controller (`qupv3_se14_4uart` / `\_SB.UR15`)
Verified from `/home/mahan/mu_s23/s23_live.dts` (L543, L22676–22693) and `Silicium-ACPI/Kailua/Decompiled/DSDT_MTP.dsl` (L9041–9078):

| Parameter | Verified S23 Value | Source |
|---|---|---|
| QUP Wrapper Base | `0x008C0000` (`qcom,qupv3_2_geni_se`, `QUP_HW_VER_REG` @ `+0x04`) | `s23_live.dts` L543 |
| Serial Engine Base | **`0x00898000`** (size `0x4000`, QUP2 SE6 / `hsuart0`) | `s23_live.dts` L22678, `DSDT_MTP` L9057 |
| GIC Interrupt | SPI `0x1CD` (`461`) -> **ACPI GSI `493` (`0x1ED`)**, Level ActiveHigh | `s23_live.dts` L22680, `DSDT_MTP` L9062 |
| Wakeup Interrupt | TLMM **GPIO 79 (`0x4F`)** (UART RX line), Edge ActiveLow | `s23_live.dts` L22680, `DSDT_MTP` L9068 |
| Transfer Mode | **GENI FIFO Mode (NO DMA required!)** — `dmas` property is absent on `qcom,qup_uart@898000` (unlike neighboring `i2c@89c000`). Confirm `GENI_IF_DISABLE_RO (+0x64) bit 0 == 0` and `GENI_FW_REVISION_RO (+0x68) [15:8] == 2 (GENI_SE_UART)` live. | `s23_live.dts` L22676–22693, `qcom_geni_serial.c` L955 |
| Oversampling | `16x` (QUP HW version >= `2.5` halves `UART_OVERSAMPLING` from `32` to `16`) | `qcom_geni_serial.c` L1031 |
| Wakeup Byte | `0xFD` (IBS in-band sleep wakeup) | `s23_live.dts` L22690 |

### 1.2 GCC QUP2 Wrapper & SE6 Clocks (`gcc-kalama.c`, base `0x00100000`)
All 5 required clocks share a single APCS vote register (`GCC_APCS_CLK_BR_ENA_VOTE_2` at **`GCC + 0x52010`**):

| Clock Name | Vote Register & Bit | CBCR / Halt Reg | Notes |
|---|---|---|---|
| `gcc_qupv3_wrap2_core_clk` | `0x52010` `BIT(0)` | `0x233D4` | Wrapper core clock |
| `gcc_qupv3_wrap_2_s_ahb_clk` | `0x52010` `BIT(1)` | `0x233D0` | Slave AHB (HWCG bit 1) — **required for `0x00898000` MMIO!** |
| `gcc_qupv3_wrap_2_m_ahb_clk` | `0x52010` `BIT(2)` | `0x233CC` | Master AHB (HWCG bit 1) — **required for `0x008C0000` MMIO!** |
| `gcc_qupv3_wrap2_core_2x_clk` | `0x52010` `BIT(3)` | `0x23004` | Wrapper 2x core clock |
| `gcc_qupv3_wrap2_s6_clk` | `0x52010` `BIT(10)` | **`0x1E754`** | SE6 serial engine branch clock |
| `gcc_qupv3_wrap2_s6_clk_src` | RCG @ **`0x1E760`** | `CFG=0x1E764, M=0x1E768, N=0x1E76C, D=0x1E770` | 16-bit M/N/D, `gcc_parent_map_8`: `0=BI_TCXO (19.2M)`, `1=GPLL0_MAIN (600M)`, `2=GPLL7`, `6=GPLL0_EVEN (300M)` |

**Exact Baud Rate Clock Table (`ftbl_gcc_qupv3_wrap2_s6_clk_src`, `gcc-kalama.c` L1305):**
- **115,200 baud (initial ROM boot / EDL handshake):** `7,372,800 Hz` = `GPLL0_OUT_EVEN (300 MHz, src=6)` × `M=384 / N=15625`, `hid=1`, `MODE=DUAL_EDGE (2)` -> `CFG = 0x00102601`, `M = 0x0180`, `N = 0xC476` (`~(15625-384)&0xFFFF`), `D = 0xC2F6` (`~15625&0xFFFF`). With `16x` oversampling, `GENI_SER_M/S_CLK_CFG = (4 << 4) | 1 = 0x41` (`7,372,800 / (16 * 4) = 115,200.000` baud, **0.000% error**).
- **3,000,000 / 3,200,000 baud (high-speed TLV download & A2DP operation):** `48,000,000 Hz` (`M=4, N=25`, div=1 -> `3.0 Mbaud`) or `51,200,000 Hz` (`M=64, N=375`, div=1 -> `3.2 Mbaud`) or `64,000,000 Hz` (`M=16, N=75`, div=1 -> `4.0 Mbaud`!).

### 1.3 TLMM GPIOs (`0x0F100000`, `pin * 0x1000`)
Verified from `s23_live.dts` (L3232–3285, L38002–38068) and `pinctrl-kalama.c` (L2171–2185, L2383):

| GPIO | Function | Active TLMM CFG | Direction / Notes |
|---|---|---|---|
| **GPIO 76** | `qup2_se6_l0` (**CTS**) | `FUNC_SEL=1`, 2 mA, no-pull (`0x0004`) | Input from QCA6490 RTS |
| **GPIO 77** | `qup2_se6_l1` (**RTS / RFR**) | `FUNC_SEL=1`, 2 mA, pull-down (`0x0005`) | Output to QCA6490 CTS |
| **GPIO 78** | `qup2_se6_l2` (**TX**) | `FUNC_SEL=1`, 2 mA, pull-up (`0x0007`) | APPS -> QCA6490 RX |
| **GPIO 79** | `qup2_se6_l3` (**RX**) | `FUNC_SEL=1`, 2 mA, pull-up (`0x0007`) | QCA6490 TX -> APPS (+ wakeup IRQ) |
| **GPIO 80** | `WLAN_EN` (`0x50`) | GPIO out (`0x03C3` high / `0x0201` low) | Shared QCA6490 WLAN enable |
| **GPIO 81** | **`BT_EN`** (`0x51`) | GPIO out (`0x0201`, OE=1) | **QCA6490 BT reset/enable** (`0` = reset, `1` = run) |
| **GPIO 82** | **`SW_CTRL`** (`0x52`) | GPIO in (`0x0001`/`0x0000`, PDC `INTR_CFG=0x7C2`) | QCA6490 power/bootstrap status monitor |
| **GPIO 204** | **`XO_CLK_GPIO`** (`0xCC`) | GPIO out (`0x0201`, OE=1) | Pulsed HIGH 200 us before `BT_EN` rising edge, held 4–6 ms, then LOW (`btpower.c` L536–544) |

### 1.4 Shared QCA6490 Power Rails, PDC & 32 kHz Sleep Clock
Verified from `s23_live.dts` L38002–38027 (`bt_qca6490`) & `qcom,cnss-qca6490`:
- **Shared PMIC VRM Rails (RPMh `APPS_RSC DRV2 TCS0`):**
  - `ldob15` (`qcom,bt-vdd-io-supply` = **1800 mV**, NPM mode 7)
  - `smpe4` (`qcom,bt-vdd-aon/dig/rfa0p8-supply` = **966 mV**, NPM mode 7)
  - `smpg2` (shared `vdd-wlan-aon-supply` on `cnss-qca6490` + `s2g` in `bt_qca6490` `pdc_init_table` = **1012 mV**, NPM mode 7)
  - `smpg6` (`qcom,bt-vdd-rfa1-supply` = **1900 mV**, NPM mode 7)
  - `smpg4` (`qcom,bt-vdd-rfa2-supply` = **1350 mV**, NPM mode 7)
  - *(Note: `qcom,bt-vdd-pmuCx-supply` / `smpg5` is empty/unconnected on S23 `dm1q`, identical to WLAN!)*
- **32 kHz Sleep Clock:** `PMK8550` (`SID 0`) **`GPIO3 @ 0xBA00`** (`0xBA40..0xBA46 = 01/00/05/02/03/80`).
- **AOSS QMP `wlan_pdc` table (`0x0C300000` + IPCC `0x00408000`):** Same 10/11-frame S23 `wlan_pdc` table shared with QCA6490 Wi-Fi (keeps `s4e/s4g/s6g/s2g` active levels coordinated with AOP).
- **Hardware Power-On Sequence (`btpower.c` L511–650):**
  1. Ensure shared QCA6490 rails (`ldob15`, `smpg2`, `smpe4`, `smpg6`, `smpg4`), XO buffers (`clka1..8`/`rclkha1`), `PMK8550 GPIO3` 32 kHz clock, and AOSS QMP `wlan_pdc` are active (either already enabled by Wi-Fi DXE/driver, or voted via RPMh).
  2. Drive `GPIO81 (BT_EN) = 0` for `50 ms` (clean BT core reset without disturbing `WLAN_EN`).
  3. Assert `GPIO204 (XO_CLK_GPIO) = 1`, wait `200 us`.
  4. Assert `GPIO81 (BT_EN) = 1`, wait `5 ms`.
  5. De-assert `GPIO204 (XO_CLK_GPIO) = 0`, wait `150 ms` for QCA6490 BT ROM bootloader to initialize UART at `115200 8N1` with HW CTS/RTS flow control.

---

## 2. On-Disk S23 Stock QCA6490 BT Firmware Inventory & TLV Download Protocol

S23's exact stock Bluetooth firmware files are **already present on disk** in `wifi_research\s23_vendor_wifi_fw\`:

| File | Size | MD5 | Parsed Header (`btqca.h`) |
|---|---:|---|---|
| `hpbtfw21.tlv` | `171,680 B` | `e4d5244b5257720b2c4d2a4267e6414a` | `TLV_TYPE_PATCH (0x01)`, payload `171,676 B` (`0x029E9C`), Product ID `0x0013` (QCA6490 / Hastings Prime), ROM Build `0x0201` (v2.1), Patch `0x7A6B` |
| `hpnv21.bin` | `6,612 B` | `d58163f065b6f19a9785b4f680108ada` | `TLV_TYPE_NVM (0x02)`, payload `6,608 B` (`0x0019D0`), tag `"BTFW.HSP_C.2.1.1..."` (Samsung `dm1q` primary NVM) |
| `hpnv21g.bin` | `6,452 B` | `36a5b0c16769f4ac7db8f9906949a6aa` | `TLV_TYPE_NVM (0x02)`, payload `6,448 B` (`0x001930`), tag `"BTFW.HSP_C.2.1.1..."` (GF variant NVM selected when `(soc_id & 0xFF00) == 0x1200`) |

### QCA6490 UART HCI / EDL Bring-Up Protocol (`btqca.c` + `hci_qca.c`)
All packets use standard **HCI H4** framing (`0x01` = Command, `0x04` = Event):
1. **ROM Wake / Ping (`115200 8N1`, HW RTS/CTS):**
   - Send `HCI_Reset`: `01 03 0C 00` -> expect Command Complete `04 0E 04 01 03 0C 00`.
   - Send `EDL_PATCH_VER_REQ_CMD` (opcode `0xFC00`, sub-cmd `0x19`): `01 00 FC 01 19` -> returns `product_id (4B)`, `patch_ver (2B)`, `rom_ver (2B)`, `soc_id (4B)` (verify Hastings Prime v2.1; check `(soc_id & 0xFF00) == 0x1200` to pick `hpnv21g.bin` vs `hpnv21.bin`).
   - Send `EDL_GET_BUILD_INFO_CMD` (opcode `0xFC00`, sub-cmd `0x20`): `01 00 FC 01 20` -> returns ROM/FW build label string.
2. **Baud Rate Switch to 3.2 Mbps (optional before TLV download):**
   - Send `QCA_SET_BAUDRATE` (opcode `0xFC48`, param `0x11` = `3,200,000` or `0x0E` = `3,000,000`), expect vendor event `0x92` (`EDL_SET_BAUDRATE_RSP_EVT`), then switch GENI `gcc_qupv3_wrap2_s6_clk_src` to `51.2 MHz` / `48.0 MHz` with `CLK_DIV = 1`.
3. **Rampatch (`hpbtfw21.tlv`) & NVM (`hpnv21.bin`) Download:**
   - Parse 4-byte TLV header (`type` + 24-bit `len`). Split payload into chunks of at most **`243 bytes`** (`MAX_SIZE_PER_TLV_SEGMENT`).
   - Wrap each chunk in `EDL_PATCH_TLV_REQ_CMD`: `01 00 FC <len+2> 1E <chunk_len> <chunk_bytes...>`.
   - Respect `download_mode` (`tlv_type_patch.download_mode` bits 0/1 = skip VSE / skip CC events) so download completes in ~600 ms at 3.2 Mbps.
   - After `hpbtfw21.tlv` completes, stream `hpnv21.bin` (or `hpnv21g.bin`) using the exact same `0xFC00 / 0x1E` TLV command (with `EDL_TAG_ID_HCI = 17` baud-rate tag & `EDL_TAG_ID_DEEP_SLEEP = 27` updated if keeping awake-pinned UART without IBS).
4. **Final Operational Reset & BD_ADDR:**
   - Issue `HCI_Reset` (`01 03 0C 00`) -> QCA6490 transitions from ROM bootloader into full Bluetooth 5.3 controller firmware!
   - Optional: program valid public BD_ADDR via `EDL_WRITE_BD_ADDR_OPCODE` (`0xFC14`, 6 bytes LSB-first) before exposing controller to Windows `BthPort`.

---

## 3. Windows 11 ARM64 Bluetooth Driver Architecture (`SerialHci` / `BthMini` Path)

Windows 11 ARM64 ships inbox **`BthPort.sys` + `BthEnum.sys` + `BthA2dp.sys` + `BthHFEnum.sys` + `HidBth.sys` + `SerialHci` (HCI-over-UART transport)**! Two integration paths exist:

| Path | Components Required | Pros / Cons |
|---|---|---|
| **Path A (Recommended — Custom Minimal WDM `s23bt_hci.sys` Bus Driver)** | Single self-contained ARM64 WDM driver (`ROOT\S23BTH` or ACPI `BTH0`) owning `0x00898000` GENI FIFO UART directly, downloading `hpbtfw21.tlv` + `hpnv21.bin` from `C:\Windows\System32\drivers\s23bt\`, and implementing the standard Microsoft **Bluetooth Transport Bus Driver** interface (`IOCTL_BTHX_*` to inbox `BthMini.sys` -> `BthPort.sys`!) | **Zero dependency on missing Kailua `qcuart`/`qcpep`/`qcbt` binaries!** Works immediately with our proven standalone `clang-18` WDM pipeline (`s23touch`/`s23keys` pattern), giving full native Windows 11 Settings -> Bluetooth, A2DP stereo audio, HFP mic, and BT keyboard/mouse! |
| **Path B (ACPI `UR15` + `BTH0` + Qualcomm `qcuart`/`qcbtuart`)** | Requires Kailua `qcuart` (`QCOM0C16`/`QCOM5E16`), `qcpep` (`\_SB.PEP0`), `qcgpio` (`\_SB.GIO0`), and Qualcomm `qcbtuart` (`QCOM0C6B`) — **none** of `qcpep`, `qcuart`, or `qcbtuart` exist in public `windows_silicon_qcom_kailua` | Documented & stubbed in `bt_research\s23_bt_uart14.asl` (`_STA = 0` inert by default) in case signed Qualcomm Kailua/Hamoa UART/BT drivers are obtained later. |

### Recommended Bring-Up Order
1. **Step 1 (Ready Now):** Run `drivers\s23btprobe\pkg\s23btprobe.sys` with `StageMask = 0x01` (read-only) to confirm GCC QUP2 SE6 clock state, `GPIO76..82`/`204`, and shared rail state.
2. **Step 2 (Ready Now):** Run `s23btprobe.sys` with `StageMask = 0x03` (or `0x07` if Wi-Fi hasn't already powered on shared QCA6490 rails) to verify live `GENI_FW_REVISION_RO == UART`, `GENI_IF_DISABLE_RO == 0` (FIFO confirmed!), and receive live `HCI_Reset` + `EDL_PATCH_VER_REQ` + `EDL_GET_BUILD_INFO` bytes from QCA6490!
3. **Step 3 (Next):** Once Step 2 prints `04 0E 04 01 03 0C 00` + Hastings Prime v2.1 version bytes, build `s23bth.sys` implementing `IOCTL_BTHX_GET_VERSION`, `IOCTL_BTHX_SET_VERSION`, `IOCTL_BTHX_QUERY_CAPABILITIES`, `IOCTL_BTHX_WRITE_HCI`, and `IOCTL_BTHX_READ_HCI` over our verified GENI FIFO UART + `hpbtfw21.tlv`/`hpnv21.bin` loader to light up full Windows 11 Bluetooth 5.3!
