# Samsung Galaxy S23 (`dm1q` / `SM-S911B`) — Native Windows 11 Pro 24H2 ARM64 Bring-Up Handoff

**Last updated:** 2026-11-20 (after Boot #20 deployed & tested, phone back in Lineage Recovery waiting for ADB authorization)  
**Target device:** Samsung Galaxy S23 (`dm1q` / `SM-S911B`, Snapdragon 8 Gen 2 for Galaxy / `SM8550-AC`), ADB serial `RFCW90N0N7X`  
**OS on phone:** Windows 11 Pro 24H2 ARM64 (`10.0.26100.4349`, Test Mode enabled, auto-login user `s23`), installed on `/dev/block/by-name/win` (`sda43`).  
**Recovery environment:** **Lineage Recovery** (NOT TWRP — do not call it TWRP).  
**User preferences:**
- Always run `adb reboot` automatically after finishing a recovery deploy. (Note: Lineage warm-reboot into `Mu-dm1q` UEFI sometimes hits a black screen, which the user bypasses with a quick hard reset.)
- Keep responses concise, technical, and direct.
- **Priority order requested by user:**
  1. **USB-C PC-Link** (live automated Windows control from PC over USB-C via `GenericUSBFn` + `WinUSB` so the AI can execute commands, transfer files, and work directly inside live Windows without recovery round-trips) + USB-C Host if desired.
  2. **Battery** (`max77705-fuelgauge` / `max77705-charger`).
  3. **Wi-Fi**.

---

## 1. Ironclad Safety Rules (NEVER Violate)

1. **NEVER write to `sdb` or `sde`** — hardware Power-On Write-Protected (`ro=1`).
2. **NEVER modify original backups** in `C:\Users\Mahan\Documents\windows-on-android\s23_dm1q\backups\`.
3. **Keep `SanPolicy = 4`** in `HKLM\SYSTEM\ControlSet001\Services\partmgr\Parameters` so Android UFS LUNs stay offline and untouched by Windows.
4. **NTFS write protocol from Lineage Recovery** (already automated in `deploy_boot16.py`):
   - Lineage Recovery has no built-in `ntfs-3g`; push Alpine musl `ntfs-3g` (`twrp_wimlib\bin\ntfs-3g`), `ntfsfix` (`twrp_wimlib\usr\bin\ntfsfix`), and musl libs (`twrp_wimlib\lib\ld-musl-aarch64.so.1` + `.so*`) to `/tmp/wimlib/{bin,lib}`.
   - Always unmount `/tmp/win_ro` and `/tmp/win_rw`, run `ntfsfix -d /dev/block/by-name/win`, mount RW (`ntfs-3g ... -o rw,remove_hiberfile`), verify ` rw,` in `/proc/mounts`.
   - Verify **every** written file with read-back `md5sum` before `sync && umount /tmp/win_rw && sync`.
5. **Do NOT blindly read `apps-smmu@15000000` from Windows EL1** without verifying Gunyah EL2 allows it (could trigger a sync abort/BSOD).
6. **Do NOT call `Set-Date` in Windows scripts** — jumping the clock +2.6 years from UEFI RTC (`2024-03-31`) immediately trips Windows Power Manager's idle display-off timer (black screen!). Our signing cert `CN=S23TouchTest2020` is valid `2020..2040` and `DriverVer` is `01/xx/2024`, so both are valid at `2024-03-31`.
7. **PowerShell 5.1 host gotcha:** No `&&` (use `;`), and avoid `|` or nested quotes inside inline `python -c "..."` or `adb shell "..."` — write small `.py` scripts in the scratch folder instead.

---

## 2. Current Status Summary

| Subsystem | Status | Details |
|---|---|---|
| **Windows 11 ARM64 Boot & Display** | **WORKING** | Boots cleanly via `Mu-dm1q-cca0.img` (`5a46cd39d18d9bd3b46b3b8565d5d16f`). Display stays awake (`powercfg VIDEOIDLE 0` + `SetThreadExecutionState(0x80000003)`). Boot disk checking permanently disabled (`chkntfs /x C:` + `BootExecute = autocheck autochk /k:C *` verified in Boot #18/#19). |
| **Native Touchscreen (`fts2ba61y`)** | **100% WORKING & CALIBRATED** | Hand-built pure-C ARM64 WDM HID 10-finger multitouch minidriver (`s23touch.sys`). Loads cleanly (`STATE: 4 RUNNING`), enumerates native `HID-compliant touch screen` (`CM_PROB_NONE`), perfect calibration/axes, smooth multi-finger touch confirmed by user. |
| **USB-C PC-Link (`GenericUSBFn` + `WinUSB`)** | **95% — Hardware Live, Final Enumeration Debug** | Windows USB Function stack (`UrsSynopsys` -> `ufxsynopsys` -> `GenericUSBFn` -> `ACTIVATE_USB_BUS ok=True`) is `CM_PROB_NONE`. SM8550 USB PHYs (`eUSB2` + 168-entry `QMP` + 5 PMIC VRM rails + `TCSR` + `GCC` clocks) all come up cleanly (`pipeClk=00000001`, `QMP PCS_STATUS` ready in 1 poll, `eUSB2=4B4B4B4B`, `DCTL=80F00000`). **Boot #17 electrically connected & chimed on the PC at High-Speed (`BUS_EVENT 01 Attach -> 04 Suspend -> 02 02 HS Reset`, PC saw `USB\VID_0000&PID_0002 Device Descriptor Request Failed`)**, then Boots #18–#19 didn't trigger HS chirp; Boot #20 (`v1.0.0.10`, PTN3222 `ldob15` power-cycle + full `eUSB2` re-init) ran and its logs are **unpulled on the phone waiting for inspection**. |
| **Battery** | **HW Identified, Not Yet Built** | Samsung uses Maxim **MAX77705** (`max77705-fuelgauge`, `max77705-charger`, `max77705-usbc`) on I2C `0x009A0000` (`i2c-62`, addr `0x66`). |
| **Wi-Fi** | **Not Started** | Check `s23_live.dts` for PCIe/WLAN chip (`WCN7850` or Broadcom) + reference `rm9-windows-arm64/docs/wifi-bringup.md` and `oneplus-open-windows`. |

---

## 3. Verified S23 (`dm1q` / `SM8550`) Hardware Map

All addresses/sequences below were extracted directly from this phone's live `/sys/firmware/fdt` (`s23_live.dts`) and pulled kernel modules in `scratch\s23_hw\`:

### A. RPMh Power Rails (100% Proven Working in Windows Kernel)
- `cmd-db`: physical `0x81C60000`, size `0x20000`, magic `0x0C0330DB`, 8 `rsc_hdr`s at `+0x08` (stride 16), entries at `+0x90 + hdr_off` (stride 24), VRM addr at `+16`.
- `apps_rsc` DRV2: physical `0x17A20000`, TCS0 at `+0x0D00` (`IRQ_EN +0xD00`, `IRQ_ST +0xD04`, `IRQ_CLR +0xD08`, `CTRL +0xD24`, `STATUS +0xD28`, `CMD_EN +0xD2C`, `MSGID +0xD34`, `ADDR +0xD38`, `DATA +0xD3C`, `CMD0_ST +0xD40`, `RESP +0xD44`).
- VRM offsets: `+0` voltage (mV), `+4` enable (`1`/`0`), `+8` mode (`7` = HPM).
- **Verified rails:**
  - `ldob12` (`0x00040B00`, `tsp_io_ldo`): `1800 mV` (Touch IOVDD — OFF at Windows boot, turned ON by `s23touch.sys`).
  - `ldob16` (`0x00041B00`, `tsp_avdd_ldo`): `3000 mV` (Touch AVDD — OFF at Windows boot, turned ON by `s23touch.sys`).
  - `ldoe1` (`pm_v6e_l1`): `880 mV` (`hsphy vdd-supply`).
  - `ldoe3` (`pm_v6e_l3`): `1200 mV` (`hsphy vdda12` & `ssphy core`).
  - `ldof3` (`pm_v8_l3`): `912 mV` (`ssphy vdd-supply`).
  - `ldob15` (`pm_humu_l15`): `1800 mV` (`eusb2_repeater@4f vdd18` — **was OFF at Windows boot!**).
  - `ldob5` (`pm_humu_l5`): `3104 mV` (`eusb2_repeater@4f vdd3`).

### B. Touchscreen (`fts2ba61y` / `stm_ts_spi`) — 100% Working
- QUP1 SE4 (`0x00A90000`) has `GENI_IF_DISABLE_RO (0x64) = 1` (FIFO mode hardware-fused off on S23).
- Bypassed cleanly with **direct TLMM (`0x0F100000`) GPIO Bit-Bang SPI Mode 0** (~400 kHz init, ~2.5 MHz 120 Hz poll):
  - `gpio44` (`0x2C000`) = MISO (in), `gpio45` (`0x2D000`) = MOSI (out), `gpio46` (`0x2E000`) = CLK (out), `gpio47` (`0x2F000`) = CS (out), `gpio25` (`0x19000`) = IRQ (active-low in).
- Confirmed ChipID `53 54 40 36 01`, HWID `36 40 01 00 33 01 90 63`, 12-bit coords `0..4095`.

### C. USB-C Subsystem (`SM8550`, Verified from S23 `s23_live.dts` + `.ko`s)
- **UEFI/ACPI:** `Mu-dm1q-cca0.img` uses `UsbConfigDxe.patched.efi` (IOMMU detach removed from EBS) and `DSDT.aml` with `Device (URS0)` (`_HID = "QCOM0C8B"`, `_CCA = Zero`, `_CRS = 0x0A600000..0x0A6FFFFF`) containing both `Device (USB0)` (`_ADR = 0`, IRQs `0xA5, 0xA3, 0x211, 0x20F, 0x20E`) and `Device (UFN0)` (`_ADR = 1`, IRQs `0xA5, 0xA3`, Microsoft UFX `_DSM`s).
- **DWC3 + QSCRATCH:** `0x0A600000` (size `0x100000`), `QSCRATCH` at `+0xF8800`:
  - `QSCRATCH + 0x08` (`GENERAL_CFG`): set bit 1 (`0x2`).
  - `QSCRATCH + 0x10` (`HS_PHY_CTRL`): set bit 20 (`0x00100000` `UTMI_OTG_VBUS_VALID`) + bit 28 (`0x10000000` `SW_SESSVLD_SEL`).
  - `QSCRATCH + 0x30` (`SS_PHY_CTRL`): set bit 24 (`0x01000000` `LANE0_PWR_PRESENT`).
  - Verified against S23's own `dwc3-msm.ko` (`dwc3_otg_start_peripheral`).
- **GCC (`0x00100000`, verified from `gcc-kalama.ko`):**
  - GDSCs: `0x39004` (`usb30_prim_gdsc`, already ON `F822F800`), `0x50018` (`usb3_phy_gdsc`, already ON `F8222800`).
  - Branch clocks: `0x3908C` (`aggre_usb3_axi`), `0x39088` (`cfg_noc_usb3_axi`), `0x39018` (`usb30_master`), `0x39024` (`sleep`), `0x39028` (`mock_utmi`), `0x39060` (`phy_aux`), `0x39064` (`phy_com_aux`), `0x39068` (`phy_pipe`), `0x3906C` (`pipe_clk_mux` -> `0` = external QMP).
  - Resets (BCRs): `0x12000` (`eUSB2` `reset[0x14]`), `0x39000` (`USB30` `reset[0x19]`), `0x50008` (`USB3_DP_PHY` `reset[0x1a]`), `0x50000` (`USB3_PHY` `reset[0x1c]`).
- **TCSR Clock Controller (`0x01FC0000`, verified from `tcsrcc-kalama.ko`):**
  - `tcsr_usb3_clkref_en`: **`0x01FD5108`** (`= 1`) — *NOTE: SM8550 is `0x01FD5108/118`, NOT SM8650's `0x01FF1108/118`!*
  - `tcsr_usb2_clkref_en`: **`0x01FD5118`** (`= 1`).
- **SNPS `eUSB2` HS PHY (`0x088E3000`, verified from `phy-msm-snps-eusb2.ko`, 38.4 MHz `bi_tcxo`):**
  - `eud_enable_reg = 0x088E2000`, `eud_detect_reg = 0x0C276000` (`= 1`, *NOT SM8650's `0x0C278000`!*).
  - Exact SM8550 `msm_eusb2_phy_init` + `msm_eusb2_phy_notify_connect` already implemented in `s23touch.c`.
- **QMP USB3/DP Combo SS PHY (`0x088E8000`, verified from `phy-msm-ssusb-qmp.ko` + `s23_live.dts`):**
  - 168 exact `(offset, val)` pairs extracted into `s23_qmp_seq.h`.
  - `DP_COM_POWER_DOWN_CTRL=0x08`, `DP_COM_SW_RESET=0x04`, `DP_COM_RESET_OVRD_CTRL=0x1C`, `DP_COM_TYPEC_CTRL=0x10`, `DP_COM_PHY_MODE_CTRL=0x00`, `USB3_PCS_POWER_DOWN_CONTROL=0x1C40`, `USB3_PCS_SW_RESET=0x1C00`, `USB3_PCS_START_CONTROL=0x1C44`, `USB3_PCS_STATUS=0x1C14` (**poll bit 6 `0x40 == 0`** -> confirmed ready in 1 poll!).
- **NXP PTN3222 `eUSB2` Repeater (`eusb2_repeater@4f`):**
  - On QUP1 I2C `0x00A98000`, I2C addr `0x4F`.
  - Supplies: `ldob15` (1.8 V), `ldob5` (3.104 V).
  - Reset GPIO: **`pm8550vs@3` (`spmi@c42d000` SID `3`) `pinctrl@8800` `gpio4`**.
  - DT `qcom,param-override-seq = <0x40 0x06 0x20 0x07 0x62 0x08 0x03 0x09 0x00 0x0a>` (value, reg pairs: `reg 0x06=0x40, 0x07=0x20, 0x08=0x62, 0x09=0x03, 0x0A=0x00`).
- **Type-C PD / MUIC / Charger / Fuel Gauge:**
  - Maxim **MAX77705** on `9a0000.i2c` (`i2c-62`, addr `0x66`), `max77705-usbc`, `max77705-charger`, `max77705-fuelgauge`.

---

## 4. Critical USB-C Debugging History (Boots #16–#20) & Exact Next Steps

### What Happened Across Boots #16–#20
- **Boot #16 (`v1.0.0.7`):** Set `RoleSwitchMode = 2` + `Control\USBFN` WinUSB registry + started `C:\s23_usb_agent.ps1`. Entire Windows stack came up `CM_PROB_NONE`, `ACTIVATE_USB_BUS ok=True err=0`, but `DWC3 DCTL` hung at `0x40F00000` (`CSFTRST` stuck) because UEFI left `pipeClk = 80000001` (`CLK_OFF`) and `eUSB2 = 09090909/07070707` uninitialized.
- **Boot #17 (`v1.0.0.8`):** Added full SM8550 `eUSB2` + `QMP` + 5 VRM rails + `TCSR` + `GCC` init into `s23touch.sys`.
  - **BREAKTHROUGH:** `pipeClk` became `00000001`, `QMP` ready in 1 poll, `eUSB2 = 4B4B4B4B`, `ldob15: 0 -> 1`, `DCTL` became `80F00000`, `DSTS` became `000DF940` (**High-Speed!**), `s23_agent.log` received **`BUS_EVENT 01` (Attach) -> `04` (Suspend) -> `02 02` (High-Speed Reset)**, and **THE PC CHIMED** with `Unknown USB Device (Device Descriptor Request Failed) | USB\VID_0000&PID_0002`!
  - **Why Boot #17 failed `GET_DESCRIPTOR`:** 3.2 seconds after `UsbfnEventReset`, our old `s23_sys_install.ps1` killed `s23_usb_agent.ps1` (`pid=2720`) and called `pnputil /restart-device ACPI\QCOM0C8B\0` (which hung mid-enumeration and dropped `GenericUSBFn`).
- **Boot #18 (`v1.0.0.8` already installed at boot, fixed `s23_sys_install.ps1`):** Removed the agent kill & `pnputil /restart-device`, disabled `chkdsk` permanently (`chkntfs /x C:`). However, because `S23InitUsbPhys()` ran *before* `S23UsbAgent` called `ACTIVATE_USB_BUS`, `ufxsynopsys.sys` started with `DCFG = 000C0804` (SuperSpeed), transitioned to `DSTS = 000E0001` (Full-Speed, no HS chirp), and timed out its 30s charger timer (`DCTL: 80F00000 -> 00700000`, `OUT err=22`).
- **Boot #19 (`v1.0.0.9`):** Added `HS-Kick` (`DCFG.DEVSPD = 0`, `GUSB2PHYCFG = 00102400`, `QSCRATCH` VBUS, `eUSB2 notify_connect`, 300ms `DCTL.RunStop` toggle). Logged critical telemetry:
  - `USB pre-HS (DCFG, DEVTEN, DALEPENA, GUSB2): 000C0804 0000025F 00000003 00102400`
  - `USB EvBuf (lo, hi, siz, eusb54): FFDCE000 00000009 00001000 4B4B4B4B` (**Notice Event Buffer is at `0x9_FFDCE000`, >4 GB!**)
  - `DSTS` stayed `00020001` (Full-Speed, no PC chime) because `ldob15` was already ON and `eUSB2` wasn't BCR-reset after `RunStop`.
- **Boot #20 (`v1.0.0.10` — JUST RAN, LOGS NOT PULLED YET!):** Added `S23CycleRepeaterLdob15()` (`ldob15 0 -> 20ms -> 1`) + full `S23Eusb2FullInit()` (`GCC_QUSB2PHY_PRIM_BCR` + full `eUSB2` init + `notify_connect`) inside `HS-Kick`. No PC USB device detected within 180s watcher; phone returned to Lineage Recovery (`unauthorized` when last checked).

### Exact First Steps for the Next AI
1. **Pull Boot #20 logs immediately** once ADB is authorized:
   ```powershell
   python "C:\Users\Mahan\.gemini\antigravity\brain\9c658663-7a3d-4ac0-af0f-1d608772d9d0\scratch\pull_logs.py" boot20
   ```
   Check `USB HS-Kick+600ms (DCFG, DCTL, DSTS, EVCNT)` and `s23_agent.log` (`BUS_EVENT`s).
2. **Diagnose why Boot #17 chimed High-Speed (`BUS_EVENT 01 -> 04 -> 02 02`) while Boots #18–#20 didn't (or why EP0 `GET_DESCRIPTOR` failed on Boot #17):**
   - **Check #1 — Compare Boot #17 vs Boot #20 exact register sequence!** On Boot #17, `v1.0.0.8` ALSO re-ran **QMP SS PHY BCR (`0x50008`/`0x50000`) + full 168-entry `QMP` init** right after `eUSB2` init, AND `DCTL` was transitioning from `40F00000 -> 80F00000` (NOT manually toggled `0 -> 1`). Check if `DCTL.RunStop` being manually cleared (`dctl & ~0x80000000`) caused `ufxsynopsys.sys` or DWC3 to drop its `DALEPENA` / EP0 state, OR if `QMP` BCR + init is needed alongside `eUSB2`!
   - **Check #2 — NXP PTN3222 `eusb2_repeater@4f` reset pin (`pm8550vs@3 gpio4`) & I2C override (`0x00A98000`, addr `0x4F`)!** Remember `ldob15` was OFF at boot; in `s23_live.dts`, PTN3222 has `reset-gpio = <pm8550vs@3 gpio4>` and `qcom,param-override-seq = <0x40 0x06 0x20 0x07 0x62 0x08 0x03 0x09 0x00 0x0a>`. Check whether toggling/driving `pm8550vs@3 gpio4` HIGH via SPMI (`0x0C42D000` or `PmicDxe`) and/or writing PTN3222 regs `0x06..0x0A` over I2C (or bit-bang I2C on QUP1 SE6 GPIOs from `s23_live.dts`) is needed for clean signal integrity on `GET_DESCRIPTOR`!
   - **Check #3 — UEFI-stage PHY init (like `rm9`'s `MuChain.c` `UsbPhyPrep` + `UsbVbusOverride`)!** Remember on `rm9-windows-arm64`, the **100% reliable winning formula** was initializing `eUSB2` + `QMP` + `TCSR` + `GCC` **in UEFI BEFORE Windows boots** (so `ufxsynopsys.sys` NEVER sees dead PHYs or hung `CSFTRST` during boot!), and then applying ONLY the 3 `QSCRATCH` VBUS overrides in Windows ~15–20 s after boot! We have the full `~/mu_s23/Mu-Silicium` UEFI source tree ready to compile in WSL!
   - **Check #4 — Capture Windows `ufx01000` / `ufxsynopsys` ETW trace (`fn.etl`)** using the exact two GUIDs from `rm9`'s `esp_run_usblive.cmd`:
     - `{E98EBDBF-3058-4784-8521-47860B1D2B8E}` (ufx01000)
     - `{49B12C7C-4BD5-4F93-BB75-30FCE739600B}` (ufxsynopsys)
     That will tell you in one boot the exact NTSTATUS inside `ufxsynopsys.sys` during `Reset` & `GET_DESCRIPTOR`!
   - **Check #5 — Check `USB Host` mode (`RoleSwitchMode = 1`) as a quick sanity test or fallback:** Now that `eUSB2` + `QMP` + `pipe_clk` are live, `USBXHCI` (which previously failed `0xC0000001` due to dead PHYs!) should also start cleanly — useful for USB-C peripherals (and a USB-C Ethernet adapter if ever desired).

---

## 5. Complete File & Toolchain Map

### Driver Source & Build/Sign Pipeline
- Driver C source (`v1.0.0.10`): `C:\Users\Mahan\Documents\windows-on-android\s23_dm1q\drivers\s23touch\s23touch.c`
- Extracted S23 QMP SS PHY table (168 entries): `C:\Users\Mahan\Documents\windows-on-android\s23_dm1q\drivers\s23touch\s23_qmp_seq.h`
- Driver INF: `C:\Users\Mahan\Documents\windows-on-android\s23_dm1q\drivers\s23touch\s23touch.inf` *(always bump `DriverVer` and matching `'v1.0.0.X'` string in `s23_sys_install.ps1`)*
- Build in WSL (`clang-18` + `lld-link` ARM64 WDM):
  ```powershell
  wsl -e python3 /mnt/c/Users/Mahan/Documents/windows-on-android/s23_dm1q/drivers/s23touch/build_wsl.py
  ```
  - **CRITICAL PE requirements (already in `build_wsl.py` & `s23touch.c`):** `/TSAWARE:NO` (`DllChars = 0x2160` — `ntoskrnl.exe` rejects `TSAWARE` on `NATIVE` drivers with `0xC0000018`) AND keep non-const `gRelocAnchors[2]` so `lld-link` emits real `.reloc` (`DD[5:BASERELOC]`).
- Sign & build catalog (`CN=S23TouchTest2020`, thumbprint `05DBD92557F4FC52EF2A5E5955789D8EED637C68`, valid `2020..2040`, already trusted in phone's `Root` & `TrustedPublisher`):
  ```powershell
  powershell -NoProfile -ExecutionPolicy Bypass -File "C:\Users\Mahan\Documents\windows-on-android\s23_dm1q\drivers\s23touch\sign_and_cat.ps1"
  ```
- Verify PE header, `.reloc`, checksum, and imports against phone's `ntoskrnl.exe` / `hidclass.sys`:
  ```powershell
  python "C:\Users\Mahan\.gemini\antigravity\brain\9c658663-7a3d-4ac0-af0f-1d608772d9d0\scratch\check_pe.py"
  ```

### Guest Scripts, PC Link Client, Deploy & Pull Scripts
All located in `C:\Users\Mahan\.gemini\antigravity\brain\9c658663-7a3d-4ac0-af0f-1d608772d9d0\scratch\`:
- `s23_diag_v4.ps1` -> deployed to `C:\s23_diag.ps1` (runs at user logon, keeps display awake, launches `S23SysInst` SYSTEM task).
- `s23_sys_install.ps1` -> deployed to `C:\s23_sys_install.ps1` (runs as `NT AUTHORITY\SYSTEM`, disables `chkdsk` on `C:`, upgrades `s23touch.sys` bypassing DriverStore ACL lockout, ensures `RoleSwitchMode = 2` & `S23UsbAgent`).
- `s23_usb_agent.ps1` -> deployed to `C:\s23_usb_agent.ps1` (resident SYSTEM daemon, opens `GenericUSBFn`, sends `ACTIVATE_USB_BUS`, serves `0x5239` `PING`/`EXEC`/`PUT`/`GET`/`REBOOT`/`VER` over bulk OUT `0x01` / IN `0x11`, logs to `C:\Users\Public\s23_agent.log`).
- `s23link.py` -> PC-side WinUSB CLI client (`python s23link.py wait | ping | ver | exec "cmd" | put | get | reboot`, uses native inbox `winusb.dll`, `VID_1209&PID_0004`, `GUID {DEE824EF-729B-4A0E-9C14-B7117D33A817}`).
- `watch_usb_boot16.py` -> PC-side 180 s live watcher for `VID_1209&PID_0004` or any new/error USB PnP device.
- `deploy_boot16.py` -> runs `ntfsfix -d`, mounts `/dev/block/by-name/win` RW, writes all 7 files, resets 64 KB pre-allocated logs (`s23_diag.txt`, `s23_sys.txt`, `s23touch.log`, `s23_agent.log`), verifies 7/7 read-back MD5s, unmounts cleanly, syncs, and runs `adb reboot`.
- `pull_logs.py <tag>` -> pushes musl `ntfs-3g` if needed, mounts `/dev/block/by-name/win` RO, pulls all 4 logs into `scratch\<tag>\`.

### Extracted S23 Hardware Dumps & Reference Repos
- Pulled S23 live FDT + decompiled DTS + `.ko` modules: `C:\Users\Mahan\.gemini\antigravity\brain\9c658663-7a3d-4ac0-af0f-1d608772d9d0\scratch\s23_hw\`
  - `s23_live.dtb`, `s23_live.dts` (full 1.3 MB live S23 device tree!)
  - `dwc3-msm.ko`, `phy-msm-snps-eusb2.ko`, `phy-msm-ssusb-qmp.ko`, `repeater-i2c-eusb2.ko`, `repeater-qti-pmic-eusb2.ko`, `gcc-kalama.ko`, `tcsrcc-kalama.ko`, `cmd-db.ko`, `ntoskrnl.exe`, `hidi2c.sys`
- Boot logs archive: `scratch\boot14\`, `boot15\` (first working touch!), `boot16\` (first USB Function stack), `boot17\` (PC CHIMED High-Speed!), `boot18\`, `boot19\`.
- UEFI source (WSL): `~/mu_s23/Mu-Silicium` (`Platforms/Samsung/dm1qPkg/`, `Binaries/dm1q/`, `Silicium-ACPI/Platforms/Samsung/dm1q/DSDT.aml`).
- Reference repos on disk:
  - `C:\Users\Mahan\Documents\windows-on-android\s23_dm1q\repos\rm9-windows-arm64\` (working USB PC-link `usbfn_agent.ps1` + `rm9link.py` + `MuChain.c` `UsbPhyPrep`/`UsbVbusOverride`, Battery `tools\acpi\add_battery.py`, Wi-Fi `docs\wifi-bringup.md`).
  - `C:\Users\Mahan\Documents\windows-on-android\s23_dm1q\repos\oneplus-open-windows\` (SM8550 RPMh RSC `G379\source\g98_rsc.c`, Battery `G162-battery-cache-callbacks`, Wi-Fi `G289Wlan`).
  - `C:\Users\Mahan\Documents\windows-on-android\s23_dm1q\repos\windows_silicon_qcom_kailua\` (SM8550 Qualcomm SOC drivers).
  - `C:\Users\Mahan\Documents\windows-on-android\s23_dm1q\repos\Device-Binaries\dm2q\` (sister device S23+ patches).

---

## 6. Roadmap After USB-C PC-Link Connects

1. **Verify `s23link.py exec "whoami && hostname"`** — once working, all further driver builds, registry edits, Device Manager queries, log pulls, and reboots happen live over USB-C in seconds!
2. **Battery (`max77705-fuelgauge`):** Read live SOC%, voltage, current, charging state from MAX77705 on I2C `0x009A0000` (addr `0x66`, check `s23_live.dts` for exact GPIOs & `max77705` fuelgauge/charger register map in `/lib/modules/`) and expose a Windows battery miniclass driver or ACPI `_BIF`/`_BST` device (`oneplus-open-windows/experiments/G162-battery-cache-callbacks` is a ready SM8550 Windows battery miniclass template!).
3. **Wi-Fi:** Check `s23_live.dts` for the S23's exact WLAN chip + PCIe root complex node, then follow `rm9-windows-arm64/docs/wifi-bringup.md` / `oneplus-open-windows` (`G289Wlan`).
4. **Polish:** Clean up temporary `s23_diag.ps1` / old `s23touch` DriverStore copies and fix the temporary-profile `Users\s23\NTUSER.DAT` warning if Windows still shows it.
