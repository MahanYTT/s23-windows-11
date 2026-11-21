# Samsung Galaxy S23 (`dm1q` / `SM-S911B`, `SM8550-AC`) — Audio (`CS35L43`), Haptics (`CS40L26A` / `MAX77705`), Sensors & Complete 26-SE QUPv3 Hardware Blueprint

**Target Device:** Samsung Galaxy S23 (`dm1q` / `SM-S911B`, Bootloader `S911BXXS8DYI3`, SoC `SM8550-AC` / Qualcomm `Kalama` / `Kailua`)  
**Target OS:** Windows 11 Pro 24H2 ARM64 (`10.0.26100.4349`)  
**Verification Mode:** 100% Offline, Silicon-Verified Directly Against Authoritative S23 Sources:
- Live S23 Flattened Device Tree: `/home/mahan/mu_s23/s23_live.dts`
- Samsung S23 Kernel Source (`msm-kernel`):
  - `drivers/clk/qcom/gcc-kalama.c` & `include/dt-bindings/clock/qcom,gcc-kalama.h`
  - `drivers/pinctrl/qcom/pinctrl-kalama.c`
  - `include/linux/qcom-geni-se.h` & `drivers/i2c/busses/i2c-qcom-geni.c`
  - `sound/soc/codecs/{cs35l43.c,cs35l43.h,cs35l43-i2c.c,cs35l43-tables.c}`
  - `drivers/vibrator/cs/cs40l26/{cs40l26.c,cs40l26-i2c.c,cs40l26-tables.c}` & `include/linux/vibrator/cs40l26.h`
  - `drivers/mfd/maxim/max77705.c` & `include/linux/mfd/max77705-private.h`
- Existing Live S23 Windows Drivers Audited (Read-Only): `drivers\s23touch\s23touch.c`, `drivers\s23batt\s23batt.c`, `drivers\s23btprobe\s23btprobe.c`

---

## 1. Executive Hardware Corrections Discovered in Live S23 DTS (`s23_live.dts`)

Parsing `/home/mahan/mu_s23/s23_live.dts`, `gcc-kalama.c`, and `pinctrl-kalama.c` directly uncovered **five major SM8550 / Galaxy S23 (`dm1q`) architectural realities** that differ from older Snapdragon generations (`SM8350`/`SM8450`) and reference MTP boards:

1. **SM8550 Has 26 QUPv3 Serial Engines Across 3 Wrappers (NOT 16 Across `QUP0` + `QUP2`):**
   - Qualcomm replaced legacy `QUPv3_0` (`0x009C0000`) with a dedicated **10-engine `QUPv3_I2C_HUB` (`i2chub0`, `qcom,qupv3_i2c_geni_se@9c0000`, SE0..SE9 at `0x00980000..0x009A4000`)** controlled by `GCC_QUPV3_I2C_S_AHB_CLK` + `GCC_QUPV3_I2C_CORE_CLK` + `GCC_QUPV3_I2C_S0..S9_CLK` (MND=0 simple divider, NO `M_AHB` or `CORE_2X`!).
   - The **16 full multi-protocol GENI Serial Engines** (I2C / SPI / UART / I3C) sit in **`QUPv3_1` (`0x00AC0000`, SE0..SE7 at `0x00A80000..0x00A9C000`)** and **`QUPv3_2` (`0x008C0000`, SE0..SE7 / `se8..se15` at `0x00880000..0x0089C000`)**!
   - All live S23 I2C audio/haptic/power/NFC peripherals (`CS35L43`, `CS40L26A`, `MAX77705`, `TSL2585`, `PN547`, `S2MIW04`) live on **`QUPv3_I2C_HUB` (`0x009C0000`)**, while `s23touch` lives on **`QUPv3_1 SE4` (`0x00A90000`)**, `s23batt` bit-bangs **`QUPv3_I2C_HUB SE8` pins `gpio206/207` (`0x009A0000`)**, and QCA6490 BT UART14 lives on **`QUPv3_2 SE6` (`0x00898000`)**!
2. **Smart Speaker Amplifiers Are Dual Cirrus Logic `CS35L43` (NOT `CS35L41`):**
   - `s23_live.dts` L23083–23148 confirms **`cirrus,cs35l43`** at I2C **`0x40`** (`cs35l43-right` / `cs35l43-bot`) and **`0x41`** (`cs35l43-left` / `cs35l43-rcv`) on **`QUPv3_I2C_HUB SE6` (`0x00998000`, `gpio8/9`)**. Both use 32-bit big-endian I2C register addressing (`DEVID @ 0x00000000` = `0x00035A43`; CS35L41 is `0x00035A40`).
3. **`WCD9385` Codec, `WSA884x` Amps, and ALL 4 SoundWire Masters (`swr0..3`) Are DISABLED/UNPOPULATED on S23 (`dm1q`):**
   - `s23_live.dts` L28159 (`wcd938x-codec { status = "disabled"; }`), L28210 (`qcom,wcd-disabled = <0x01>;`), L28217 (`qcom,wsa-max-devs = <0x00>;`), and `status = "disabled"` on `swr0`, `swr1`, `swr2`, `swr3`! S23 speakers use **LPASS TDM0 (`PRI_MI2S`: `GPIO126..129`)** -> dual `CS35L43`, and microphones use **DMIC0..5** directly into LPASS `TX-macro` / `VA-macro` via `LPI TLMM (0x06E80000)`.
4. **Haptics / Vibrator Is Cirrus Logic `CS40L26A` (NOT `MAX77705` Haptic, and NOT PMIC `qcom,hv-haptics`):**
   - `s23_live.dts` L22894 confirms **`cirrus,cs40l26a @ 0x40`** on **`QUPv3_I2C_HUB SE3` (`0x0098C000`, `gpio22/23`, `reset=GPIO145`, `irq=GPIO88`)** with `DEVID @ 0x00000000 = 0x0040A260` and direct ROM waveform mailbox (`0x00013020`). Meanwhile, `qcom,hv-haptics@f000` has `status = "disabled"`, and `MAX77705 @ 0x66` (`QUPv3_I2C_HUB SE8 @ 0x009A0000`) is the Samsung Sub-PMIC MFD for Fuel Gauge (`0x36`), Charger (`0x69`), and USB-C PDIC/MUIC (`0x25`) — its legacy `MCONFIG/MCONFIG2` haptic driver is unpopulated on `dm1q`.
5. **IMU / Magnetometer / Barometer / Front Prox Are ADSP/SSC-Only; Only Rear ALS (`TSL2585`), Grip/SAR (`ISG6320`), and Hall (`GPIO107`) Are AP-Visible:**
   - `LSM6DSO`, `AK09918C`, `LPS22DF`, and front ALS/proximity have **zero AP DTS nodes** because they sit on the **LPASS SSC Qualcomm Sensing Hub (`QSH`/`SEE`)** powered by `sensor_vdd = pm_humu_l1 (1.8 V)` inside `remoteproc-adsp@03000000` (`adspslpi_region@9ea00000`).

---

## 2. Complete Master Table: All 26 QUPv3 Serial Engines on S23 (`dm1q` / `SM8550-AC`)

### 2.1 Wrapper Summary (`GCC Base = 0x00100000`, `.hwcg_bit = 1` Aware)

| Wrapper | Physical Base | SE Count | GCC Wrapper Clocks (`CBCR`, `VoteReg:Bit`, `hwcg_bit`) | SMMU SID (`apps_smmu`) |
|---|---|---:|---|---|
| **`QUPv3_I2C_HUB`** (`qcom,qupv3_i2c_geni_se@9c0000`) | `0x009C0000` (`0x2000`) | **10** (`I2C_S0..S9`) | `S_AHB`: `0x23140` (`0x52008:BIT7`, **`hwcg=1`**)<br>`CORE`: `0x23144` (`0x52008:BIT8`) | None (FIFO-only I2C Hub!) |
| **`QUPv3_1`** (`qcom,qupv3_1_geni_se@ac0000`) | `0x00AC0000` (`0x2000`) | **8** (`WRAP1_S0..S7`) | `M_AHB`: `0x2327C` (`0x52008:BIT20`, **`hwcg=1`**)<br>`S_AHB`: `0x23280` (`0x52008:BIT21`, **`hwcg=1`**)<br>`CORE`: `0x23284` (`0x52008:BIT19`)<br>`CORE_2X`: `0x23294` (`0x52008:BIT18`) | `0xA3` (GPI DMA `0x00A00000`, SID `0xB6`) |
| **`QUPv3_2`** (`qcom,qupv3_2_geni_se@8c0000`) | `0x008C0000` (`0x2000`) | **8** (`WRAP2_S0..S7`, DTS `se8..se15`) | `M_AHB`: `0x233CC` (`0x52010:BIT2`, **`hwcg=1`**)<br>`S_AHB`: `0x233D0` (`0x52010:BIT1`, **`hwcg=1`**)<br>`CORE`: `0x233D4` (`0x52010:BIT0`)<br>`CORE_2X`: `0x23004` (`0x52010:BIT3`) | `0x423` (GPI DMA `0x00800000`) |

> [!TIP]
> **Critical `.hwcg_bit = 1` Clock Check Rule:** On SM8550, all wrapper `M_AHB` and `S_AHB` CBCRs have `hwcg_bit = 1` (`HW_CTL` dynamic clock gating). When enabled and idle, reading the CBCR returns `0x80000002` (`CLK_OFF=1` AND `HW_CTL=1`) while its bit in `VOTE_1`/`VOTE_2` is `1`! A bus clock is **ON** whenever `((cbcr & 0x80000000) == 0)` **OR** `(hwcg && (cbcr & 2) && (vote & vote_bit))`.

### 2.2 Group A — `QUPv3_I2C_HUB` (`0x009C0000`, 10 Dedicated I2C-Only Hub Engines, FIFO Mode)

| # | SE Name | Phys Base (`0x4000`) | GCC Vote (`Reg:Bit`) | CBCR / RCG `CMD` (`MND=0`) | GIC SPI -> ACPI GSI | TLMM GPIOs (`FUNC_SEL`) | DTS Status & Protocol | Connected S23 (`dm1q`) Peripheral(s) & Live Safety |
|---:|---|---|---|---|---|---|---|---|
| 1 | `I2C_HUB SE0` | `0x00980000` | `0x52008:BIT10` | `0x17004` / `0x17008` | `464` -> **`496`** | `GPIO16` (fn1), `GPIO17` (fn1) | **`okay`** — I2C (FIFO) | **NXP `pn547@28`** (NFC) + **`ams,tsl2585@39`** (Rear ALS / Flicker, `irq=GPIO62`, `cal=GPIO87`) *(Protected in `s23sensprobe`)* |
| 2 | `I2C_HUB SE1` | `0x00984000` | `0x52008:BIT11` | `0x17020` / `0x17024` | `465` -> **`497`** | `GPIO18` (fn1), `GPIO19` (fn1) | `disabled` — I2C | Unused on S23 `dm1q` |
| 3 | `I2C_HUB SE2` | `0x00988000` | `0x52008:BIT12` | `0x1703C` / `0x17040` | `466` -> **`498`** | `GPIO20` (fn1), `GPIO21` (fn1) | `disabled` — I2C | `fsa4480@42` disabled (USB-C analog audio mux handled by MAX77705) |
| 4 | `I2C_HUB SE3` | `0x0098C000` | `0x52008:BIT13` | `0x17058` / `0x1705C` | `467` -> **`499`** | `GPIO22` (fn1), `GPIO23` (fn1) | **`okay`** — I2C (FIFO) | **Cirrus Logic `cs40l26a@40`** (S23 LRA Haptic Driver! `reset=GPIO145`, `irq=GPIO88`) |
| 5 | `I2C_HUB SE4` | `0x00990000` | `0x52008:BIT14` | `0x17074` / `0x17078` | `468` -> **`500`** | `GPIO4` (fn1), `GPIO5` (fn1) | `disabled` — I2C | Unused on S23 `dm1q` *(Protected in `s23sensprobe`)* |
| 6 | `I2C_HUB SE5` | `0x00994000` | `0x52008:BIT15` | `0x17090` / `0x17094` | `469` -> **`501`** | `GPIO6` (fn1), `GPIO7` (fn1) | **`okay`** — I2C (FIFO) | **Samsung `s2miw04-charger@5b`** (Wireless Charging IC; `@3b` disabled) |
| 7 | `I2C_HUB SE6` | `0x00998000` | `0x52008:BIT16` | `0x170AC` / `0x170B0` | `470` -> **`502`** | `GPIO8` (fn1), `GPIO9` (fn1) | **`ok`** — I2C (`i2c18`, FIFO) | **Dual Cirrus Logic `CS35L43` Smart Speaker Amps!** `cs35l43-right@40` (`bot`, `rst=GPIO42`) + `cs35l43-left@41` (`rcv`, `rst=GPIO55`, shared `irq=GPIO14`) |
| 8 | `I2C_HUB SE7` | `0x0099C000` | `0x52008:BIT17` | `0x170C8` / `0x170CC` | `471` -> **`503`** | `GPIO10` (fn1), `GPIO11` (fn1) | `disabled` — I2C | Unused on S23 `dm1q` |
| 9 | `I2C_HUB SE8` | `0x009A0000` | `0x52010:BIT14` | `0x170E4` / `0x170E8` | `472` -> **`504`** | `GPIO206` (fn1, eGPIO), `GPIO207` (fn1, eGPIO) | **`okay`** — I2C (FIFO) | **Maxim `max77705@66`** (MFD: PMIC `0x66`, MUIC/USBC `0x25`, Charger `0x69`, Fuel Gauge `0x36`) + **NXP `pca9481@57`** (Direct Charger) — **LIVE `s23batt.sys` bit-bangs `GPIO206/207`! Protected!** |
| 10 | `I2C_HUB SE9` | `0x009A4000` | `0x52010:BIT15` | `0x17100` / `0x17104` | `473` -> **`505`** | `GPIO84` (fn1), `GPIO85` (fn1) | **`ok`** — I2C (FIFO, 1 MHz) | `qcom,cam-i2c-actuator@18` (Camera VCM actuator 1) |

### 2.3 Group B — `QUPv3_1` (`0x00AC0000`, Full Multi-Protocol SE0..SE7)

| # | SE Name | Phys Base (`0x4000`) | GCC Vote (`Reg:Bit`) | CBCR / RCG `CMD` (`MND=16`) | GIC SPI -> ACPI GSI | TLMM GPIOs (`FUNC_SEL`) | DTS Status & Protocol | Connected S23 (`dm1q`) Peripheral(s) & Live Safety |
|---:|---|---|---|---|---|---|---|---|
| 11 | `QUP1 SE0` | `0x00A80000` | `0x52008:BIT22` | `0x18004` / `0x18010` | `353` -> **`385`** | `GPIO28..31` (fn1; `28/29` fn2 I3C) | `disabled` | Unused (`nq@28` disabled) |
| 12 | `QUP1 SE1` | `0x00A84000` | `0x52008:BIT23` | `0x1813C` / `0x18148` | `354` -> **`386`** | `GPIO32..35` (fn1; `32/33` fn2 I3C) | `disabled` | Unused |
| 13 | `QUP1 SE2` | `0x00A88000` | `0x52008:BIT24` | `0x18274` / `0x18280` | `355` -> **`387`** | `GPIO36..39` (fn1; **TZ reserved `gpios-reserved`!**) | `disabled` | Reserved by QTEE/Secure Peripheral (`qcom,gpios-reserved = 36..39`) |
| 14 | `QUP1 SE3` | `0x00A8C000` | `0x52008:BIT25` | `0x183AC` / `0x183B8` | `356` -> **`388`** | `GPIO40..43` (fn1) | `disabled` | Unused (`GPIO42` used as `CS35L43-right` Reset GPIO!) |
| 15 | `QUP1 SE4` | `0x00A90000` | `0x52008:BIT26` | `0x184E4` / `0x184F0` | `357` -> **`389`** | `GPIO44` MISO (fn1), `GPIO45` MOSI (fn1), `GPIO46` CLK (fn1), `GPIO47` SW CS | **`ok`** — **SPI** | **STMicro `stm,stm_ts_spi`** (FTS2BA61Y Touchscreen, `irq=GPIO25`) — **OWNED BY LIVE `s23touch.sys`! STRICTLY PROTECTED!** |
| 16 | `QUP1 SE5` | `0x00A94000` | `0x52008:BIT27` | `0x1861C` / `0x18628` | `358` -> **`390`** | `GPIO52..54` (fn2), `GPIO55` (fn1) | `disabled` | Unused (`GPIO55` used as `CS35L43-left` Reset GPIO!) |
| 17 | `QUP1 SE6` | `0x00A98000` | `0x52008:BIT28` | `0x18754` / `0x18760` | `363` -> **`395`** | `GPIO48` SDA (fn2), `GPIO49` SCL (fn1) | **`ok`** — **I2C** | **NXP `nxp,eusb2-repeater@4f`** (USB2 eUSB2 signal repeater) |
| 18 | `QUP1 SE7` | `0x00A9C000` | `0x52010:BIT16` | `0x1888C` / `0x18898` | `579` -> **`611`** | `GPIO26` TX (fn1), `GPIO27` RX (fn1) | **`ok`** — **2-Wire HSUART** (`hsuart5`) | Samsung ATD / Console UART (`androidboot.sec_atd.tty=/dev/ttyHS5`; FIFO mode, no DMA) |

### 2.4 Group C — `QUPv3_2` (`0x008C0000`, Full Multi-Protocol SE0..SE7 / DTS `se8..se15`)

| # | SE Name (DTS) | Phys Base (`0x4000`) | GCC Vote (`Reg:Bit`) | CBCR / RCG `CMD` (`MND=16`) | GIC SPI -> ACPI GSI | TLMM GPIOs (`FUNC_SEL`) | DTS Status & Protocol | Connected S23 (`dm1q`) Peripheral(s) & Live Safety |
|---:|---|---|---|---|---|---|---|---|
| 19 | `QUP2 SE0` (`se8`) | `0x00880000` | `0x52010:BIT4` | `0x1E004` / `0x1E010` | `373` -> **`405`** | `GPIO56..59` (`mira` fn1) | **`ok`** — **SPI** | **NXP `p61`** (`ese_spi@0`, NFC Embedded Secure Element) |
| 20 | `QUP2 SE1` (`se9`) | `0x00884000` | `0x52010:BIT5` | `0x1E13C` / `0x1E148` | `583` -> **`615`** | `GPIO60` SDA (fn1), `GPIO61` SCL (fn1) | **`ok`** — **I2C** | `qcom,cam-i2c-sensor@20` (Camera sensor 0 I2C) |
| 21 | `QUP2 SE2` (`se10`) | `0x00888000` | `0x52010:BIT6` | `0x1E274` / `0x1E280` | `584` -> **`616`** | `GPIO64` SDA (fn1), `GPIO65` SCL (fn1) | **`okay`** — **I2C** | **Samsung `sec_k250a@23`** (StrongBox Keymaster Secure Element) |
| 22 | `QUP2 SE3` (`se11`) | `0x0088C000` | `0x52010:BIT7` | `0x1E3AC` / `0x1E3B8` | `585` -> **`617`** | `GPIO68..70` (fn1), `GPIO71` (fn2) | `disabled` | Unused |
| 23 | `QUP2 SE4` (`se12`) | `0x00890000` | `0x52010:BIT8` | `0x1E4E4` / `0x1E4F0` | `586` -> **`618`** | `GPIO2/3/118/119` (fn1) | `disabled` | Unused |
| 24 | `QUP2 SE5` (`se13`) | `0x00894000` | `0x52010:BIT9` | `0x1E61C` / `0x1E628` | `587` -> **`619`** | `GPIO80..83` (fn1) | `disabled` | Pins repurposed as GPIO (`GPIO80=WLAN_EN`, `GPIO81=BT_EN`, `GPIO82=SW_CTRL`) |
| 25 | `QUP2 SE6` (`se14`) | `0x00898000` | `0x52010:BIT10` | `0x1E754` / `0x1E760` | `461` -> **`493`** | `GPIO76` CTS, `GPIO77` RTS, `GPIO78` TX, `GPIO79` RX (fn1) | **`ok`** — **4-Wire HSUART** (`hsuart0`, FIFO) | **QCA6490 Bluetooth 5.3 UART14** — **OWNED BY `s23btprobe`! STRICTLY PROTECTED!** |
| 26 | `QUP2 SE7` (`se15`) | `0x0089C000` | `0x52010:BIT17` | `0x1E88C` / `0x1E898` | `462` -> **`494`** | `GPIO72` SDA (fn2), `GPIO106` SCL (fn2) | **`ok`** — **I2C** | `qcom,cam-i2c-ois@62` (Camera Optical Image Stabilization) |

*(Additional non-QUP bit-bang I2C buses in `s23_live.dts`: `i2c@51` (`i2c51`, `s2mpb02@59` camera PMIC/flash LEDs) and `i2c@53` (`i2c53`, `gpio169 SDA` / `gpio170 SCL`, `isg6320@28` grip/SAR sensor).)*

---

## 3. S23 Audio Subsystem Architecture (`CS35L43` Dual Smart Amps, `WCD9385` Disabled, LPASS TDM0)

### 3.1 Dual Cirrus Logic `CS35L43` Smart Speaker Amplifiers (`QUPv3_I2C_HUB SE6 @ 0x00998000`)

| Property | Right / Bottom Speaker (`cs35l43-right@40`) | Left / Earpiece Receiver (`cs35l43-left@41`) |
|---|---|---|
| **Compatible / Part** | `cirrus,cs35l43` (`cs35l43-bot`) | `cirrus,cs35l43` (`cs35l43-rcv`) |
| **I2C Bus & 7-bit Addr** | `QUPv3_I2C_HUB SE6` (`0x00998000`, `GPIO8/9`, `i2c18`) @ **`0x40`** | `QUPv3_I2C_HUB SE6` (`0x00998000`, `GPIO8/9`, `i2c18`) @ **`0x41`** |
| **Regmap Wire Format** | 32-bit Big-Endian Reg Addr -> 32-bit Big-Endian Value (`stride=4`) | 32-bit Big-Endian Reg Addr -> 32-bit Big-Endian Value (`stride=4`) |
| **Identification Regs** | `DEVID (0x00000000)` = `0x00035A43`, `REVID (0x00000004)` | `DEVID (0x00000000)` = `0x00035A43`, `REVID (0x00000004)` |
| **Reset GPIO (`TLMM`)** | **`GPIO42` (`0x2A`)** (active-high release; min 2 ms pulse) | **`GPIO55` (`0x37`)** (active-high release; min 2 ms pulse) |
| **IRQ GPIO (`TLMM`)** | **`GPIO14` (`0x0E`)** (shared open-drain interrupt) | **`GPIO14` (`0x0E`)** (shared open-drain interrupt) |
| **Power Supply (`VP`/`VA`)** | `dummy_vreg` (direct VBAT / always-on board rail; `bst-ipk-ma = 4000 mA`) | `dummy_vreg` (direct VBAT / always-on board rail; `bst-ipk-ma = 4100 mA`) |
| **Audio PCM Bus** | **LPASS `PRI_MI2S` / `TDM0`** (`GPIO126` `i2s0_sck`, `GPIO127` `i2s0_data0` DIN, `GPIO128` `i2s0_data1` DOUT, `GPIO129` `i2s0_ws`) | Shared **LPASS `PRI_MI2S` / `TDM0`** (stereo TDM slots, `tdm-max-slots = 4`) |
| **Halo DSP Firmware** | `cs35l43-bot-dsp1-spk-prot.wmfw` + `.bin` (present in `/home/mahan/s23/src/vendor/qcom/opensource/audio-hal/`) | `cs35l43-rcv-dsp1-spk-prot.wmfw` + `.bin` |

### 3.2 `WCD9385` Codec, SoundWire Controllers, and Microphones (`s23_live.dts` L27863–28233)

- **`WCD938x` (`wcd938x-codec`):** Explicitly **`status = "disabled"`** and `sound` sets **`qcom,wcd-disabled = <0x01>`**. (`GPIO108` reset pin exists only in base QTI Kalama DTSI.) S23 has no 3.5 mm jack and no `WCD9385` populated.
- **SoundWire Masters (`swr0..3`):** All four (`swr0` WSA `0x06B10000`, `swr1` RX `0x06AD0000`, `swr2` VA `0x06D30000`, `swr3` WSA2 `0x06AB0000`) and all `wsa884x` / `dmic_swr` slaves have **`status = "disabled"`** (`qcom,wsa-max-devs = <0x00>`).
- **Microphones (`DMIC0..5`):** Connected directly to LPASS `tx-macro@06AE0000` and `va-macro@06D44000` via LPI TLMM (`0x06E80000`, `lpi_gpio6+`).

### 3.3 Can APPS Drive `CS35L43` Directly Over I2S/TDM Without ADSP GLink?

- **I2C Control Plane — YES (Direct from APPS/Windows):** `CS35L43` sits on AP-owned `QUPv3_I2C_HUB SE6` (`0x00998000`). Windows can release `GPIO42`/`GPIO55`, read `DEVID`/`REVID`, apply `cs35l43_errata_patch`, and configure ASP/TDM bypass mode (without Halo DSP speaker protection, or by loading `.wmfw`) directly over I2C!
- **Audio PCM Data Plane (`TDM0` / `PRI_MI2S`) — REQUIRES LPASS Hardware Clocks/LPAIF:** On SM8550, the `PRI_MI2S`/`TDM0` bit-clock (`i2s0_sck`), frame-sync (`i2s0_ws`), and LPAIF DMA engine live inside the **LPASS** power domain (`0x06000000..0x06FFFFFF`). Notice `sound` uses `vote_lpass_core_hw` and `vote_lpass_audio_hw` (`qcom,audio-ref-clk` via `q6prm` over ADSP GPR/GLink!).
  - **Path A (Standard Qualcomm AudioReach):** Boot `ADSP` via PIL -> GLink `adsp_apps` -> GPR `spf_core` + `q6prm` -> AudioReach graph -> `PRI_MI2S`/`TDM0` -> `CS35L43` (very high effort; requires ADSP PIL + GLink + GPR).
  - **Path B (Bare-Metal APPS LPAIF Direct Drive — Experimental Research Path):** On some Qualcomm SoCs, if `LPASS_CORE_HM` GDSC (`lcx.lvl`/`lmx.lvl`) + LPASS `LPAIF` (`I2S0` RDMA + `LPASS_LPI_CC` MCLK/BCLK) registers are NOT XPU-locked to `VMID_LPASS`, APPS could drive `LPAIF` DMA + `PRI_MI2S` clocks directly without booting ADSP. However, on retail SM8550 Gunyah builds, LPASS clock/LPAIF regions are frequently firewalled to ADSP. **Therefore, Bluetooth A2DP (via QCA6490 UART14) or USB-C Audio (via xHCI) remains the fastest path to audio on Windows, while `CS35L43` I2C bring-up is immediately testable via `s23sensprobe.sys`!**

---

## 4. Haptics / Vibrator Architecture (`CS40L26A` LRA vs `MAX77705` MFD)

### 4.1 Primary S23 Tactile Vibrator: Cirrus Logic `CS40L26A` (`QUPv3_I2C_HUB SE3 @ 0x0098C000`)

- **Bus & Address:** `QUPv3_I2C_HUB SE3` (`0x0098C000`, `GPIO22 SDA` / `GPIO23 SCL`, `FUNC_SEL=1`), 7-bit I2C **`0x40`**, 32-bit Big-Endian register address & value.
- **Control GPIOs (`TLMM 0x0F100000`):** **`GPIO145` (`0x91`)** = `RESET#` (output, pull high to wake IC), **`GPIO88` (`0x58`)** = `IRQ#` (input, pull-up).
- **Identification Registers (`include/linux/vibrator/cs40l26.h`):**
  - `CS40L26_DEVID (0x00000000)` & `0xFFFFFF` = **`0x40A260`** (`CS40L26A`) / `0x40A26B` (`CS40L26B`)
  - `CS40L26_REVID (0x00000004)` & `0xFF` = `0xA1` / `0xB0`
- **Direct Windows Tactile Pulse Capability (ZERO ADSP Needed!):**
  - Unlike `CS35L43` speakers (which require LPASS TDM0 audio clocks), `CS40L26A` contains an **internal Halo Core DSP + built-in ROM haptic waveform bank** (`CS40L26_ROM_INDEX_START = 0x01800000` .. `0x01800026`, plus Buzzgen `0x01800081`)!
  - Pulsing a short tactile click from Windows requires **ONLY** releasing `GPIO145` (`RESET# = 1`), waiting ~3 ms for boot-done, and writing a single 32-bit command (`0x01800000 + idx`) to **`CS40L26_DSP_VIRTUAL1_MBOX_1 (0x00013020)`** over `QUPv3_I2C_HUB SE3`! That makes **native S23 Windows haptics a realistic Quick Win** once `s23sensprobe.sys` confirms `DEVID = 0x40A260`!

### 4.2 `MAX77705` Sub-PMIC MFD (`QUPv3_I2C_HUB SE8 @ 0x009A0000`, `GPIO206/207`)

- **7-bit I2C Sibling Addresses (`drivers/mfd/maxim/max77705.c`):**
  - **`0x66`** (`0xCC >> 1`): PMIC Top (`PMICID1=0x00`, `PMICREV=0x01`, `MAINCTRL1=0x02`, `INTSRC=0x22`, `BOOSTCONTROL1=0x4C`, unused legacy haptic `MCONFIG=0x10`, `MCONFIG2=0x11`).
  - **`0x25`** (`0x4A >> 1`): USB-C PDIC / CCIC / MUIC (Type-C role swap, PD contracts, analog audio accessory switch).
  - **`0x69`** (`0xD2 >> 1`): Main Battery Charger (already read by `s23batt.sys`).
  - **`0x36`** (`0x6C >> 1`): Fuel Gauge (already read by `s23batt.sys`).
  - **`0x62`** (`0xC4 >> 1`): Debug.
- **IRQ GPIO:** `GPIO133` (`0x85`). Note that on S23 `dm1q`, `MAX77705` does NOT drive a haptic motor (Samsung uses `CS40L26A` instead), and Qualcomm PMIC `qcom,hv-haptics@f000` is also `status = "disabled"`.

---

## 5. Sensors Architecture: AP-Visible vs. LPASS/ADSP SSC (`QSH`/`SEE`)

| Sensor / Function | Part / Compatible | Bus / Physical Interface | AP-Visible in Windows Without ADSP? | Details (`s23_live.dts`) |
|---|---|---|---|---|
| **Rear ALS & Flicker** | **ams `TSL2585`** (`ams,tsl2585`) | `QUPv3_I2C_HUB SE0` (`0x00980000`, `GPIO16/17`) @ `0x39` | **YES** (direct I2C) | `irq=GPIO62`, `cal=GPIO87`, shares bus with NXP `pn547@28` NFC |
| **Grip / SAR Proximity** | **Imagis `ISG6320`** (`isg6320`) | Bit-bang `i2c-gpio` `i2c@53` (`GPIO169 SDA` / `GPIO170 SCL`) @ `0x28` | **YES** (TLMM bit-bang like `s23batt`) | `irq=GPIO153`, rail `pm_humu_l2` |
| **Magnetic Hall Sensor** | `hall_ic` | Direct TLMM **`GPIO107`** (`0x6B`) | **YES** (1-register TLMM read) | Flip-cover / magnet detection |
| **6-Axis IMU (Accel/Gyro)** | `LSM6DSO` (QSH/SEE) | **LPASS SSC (`SLPI` inside `ADSP`)** | **NO** (requires ADSP PIL + GLink/QMI SEE) | Powered by `sensor_vdd` (`pm_humu_l1`, `1.8 V`); `qcom,fastrpc-adsp-sensors-pdr` |
| **Magnetometer** | `AK09918C` (QSH/SEE) | **LPASS SSC (`SLPI` inside `ADSP`)** | **NO** (requires ADSP PIL + GLink/QMI SEE) | Not exposed on AP QUPv3 |
| **Barometer** | `LPS22DF` (QSH/SEE) | **LPASS SSC (`SLPI` inside `ADSP`)** | **NO** (requires ADSP PIL + GLink/QMI SEE) | Not exposed on AP QUPv3 |
| **Front ALS / Proximity** | Under-display optical (`STK33911` / ams, QSH/SEE) | **LPASS SSC (`SLPI` inside `ADSP`)** | **NO** (requires ADSP PIL + GLink/QMI SEE) | Coordinated with display TE/panel brightness inside ADSP |
