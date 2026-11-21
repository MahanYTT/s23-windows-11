# Neon Overnight Engineering Report — Samsung Galaxy S23 (`dm1q` / `SM-S911B`, `SM8550-AC`) Native Windows 11 ARM64 Bring-Up

**Generated:** Overnight autonomous run (Workstreams 1–4 complete & verified)  
**Working Directory:** `C:\Users\Mahan\Documents\windows-on-android\s23_dm1q`  
**Target OS:** Native Windows 11 Pro 24H2 ARM64 (`10.0.26100.4349`)  
**Signing Certificate:** `CN=S23TouchTest2020` (`05DBD92557F4FC52EF2A5E5955789D8EED637C68`)  

---

## 1. Executive Summary

All four overnight workstreams have been completed, compiled, signed, and verified with zero broken artifacts:

1. **Workstream 1 — AMOLED Black Screen on Warm Reboot (Root Cause + UEFI Fix + Live Windows Diag/Wake Driver):**
   - Proved from live Lineage Recovery logs, S23 DTS/DRM drivers, stock `dm1q` `ABL`/`DisplayDxe`/`ClockDxe` ARM64 disassemblies, and `Mu-Silicium/Platforms/Samsung/dm1qPkg` that `DM1_S6E3FAC_AMB606AW01` is strictly **DSI Command Mode + DSC 1.1** (`1080x2340`, TE on `GPIO86`). On warm `adb reboot` from Lineage Recovery, Linux dims DBV to `0` and runs `msm_pdev_shutdown()` -> `ss_panel_off_post()` (DCS `0x28`/`0x10`, `INTF_TEAR_AUTOREFRESH_CONFIG = 0`, GDSC collapse, rails off, `Reset_N` low), while `Mu-dm1q` (`USE_CUSTOM_DISPLAY_DRIVER = 0`, `SimpleFbDxe`) neither maps `MDSS`/`DISP_CC` nor re-arms Auto-Refresh (`0x0AE362B4 = 0x80000002`) / `CTL_0_START (0x0AE1601C = 1)`.
   - Built a clean UEFI patch (`s23_display_autorefresh_fix.patch`) adding `MDSS`/`DISP_CC` mappings to `MemoryMapLib.c` + `S23DisplayFixDxe` (strictly `G116Gate`-protected Auto-Refresh keeper + `ExitBootServices` kick) and compiled a separate test image **`Mu-dm1q-dispfix.img`** (`2,443,264 B`, `Build RC = 0`) without touching `Mu-dm1q-cca0.img` or `backups\`.
   - Built, signed, and verified **`drivers\s23disp_diag\pkg\s23disp_diag.sys`** (`v1.0.0.1`, read-only safe default + opt-in Auto-Refresh kick & `S6E3FAC` max-brightness/Display-On DCS wake via `DSI0_CTRL` DMA).
   - Full technical report saved to **`DISPLAY_BLACK_SCREEN_FIX.md`**.

2. **Workstream 2 — Exhaustive Line-by-Line Audit of `s23pcieprobe.c` (`v1.0.0.9 -> v1.0.0.10`) & `deploy_s23pcieprobe.py` Fix:**
   - Audited every GPIO, VRM rail, PMK8550 clock, AOSS QMP frame, GCC GDSC/RCG/CBCR/BCR, 115-triplet QMP PHY table (`115/115 EXACT_MATCH` vs `s23_live.dts`), PARF, DBI, iATU, and Windows kernel safety invariant against `pci-msm.c`, `gcc-kalama.c`, `cnss2/power.c`, `s23_live.dts`, `g289_rc0.c`, and `s23pcieprobe_v8_stagemask1f.log`.
   - Found and fixed **7 concrete bugs**:
     1. `deploy_s23pcieprobe.py` was deploying **unsigned** `out\s23pcieprobe.sys` -> fixed to deploy signed `pkg\s23pcieprobe.sys` with pre-push PE32+ ARM64 + `.reloc` + Authenticode Security Data Directory (`DD[4]`) enforcement (negative-tested: refuses unsigned binaries).
     2. `PARF_LTSSM (0x1B0)` `SW_CLR_FLUSH_MODE (BIT(10))` was unconditionally asserted AND left permanently set alongside `LTSSM_EN (BIT(8))` -> fixed to check `FLUSH_MODE (BIT(11))` in `0x1B0` first, pulse `BIT(10)` only if needed, and clear `BIT(10)` cleanly before asserting `BIT(8)`.
     3. Added automatic Pass 2 fallback testing canonical `pci-msm.c` / `g289_rc0.c` `x2 PORT_LINK_MODE (0x030000) + NUM_OF_LANES=1 (0x0100)` + `PERST#` re-pulse + `CLKREQ_OVR=0x02` (plus `PHY` debug dump if still down) if Pass 1 strict `x1` fails.
     4. Added Root Port Bus `0/1/1` setup (`DBI+0x18`) + `1000 x 1 ms` poll loop on `ECAM 0x60100000` (preventing false `0xFFFFFFFF` CRS misses after `L0`).
     5. Removed unused `smpg5` (`vdd-wlan-supply;` is empty in `s23_live.dts`, zero consumers on `dm1q`, `st=0x47 VREG_OK=0`).
     6. Explicitly held `GPIO81 (BT_EN)` and `GPIO80 (WLAN_EN)` LOW with `0x0201` (matching `bt_en_sleep` / `cnss_wlan_en_sleep`), and switched Stage 2C.1 BCR resets to S23's canonical pair (`0x6B000`, `0x6C01C`).
     7. Replaced 150 ms `KeStallExecutionProcessor` busy-spins with `KeDelayExecutionThread` (`SleepMs(1)`) at `PASSIVE_LEVEL` and cleared global MMIO pointers in `cleanup:`.
   - Rebuilt, signed, and verified **`drivers\s23pcieprobe\pkg\s23pcieprobe.sys`** (`v1.0.0.10`), and saved the full audit to **`PCIE0_PROBE_AUDIT.md`**.

3. **Workstream 3 — Full QCA6490 Windows Wi-Fi Bring-Up Package (`wifi_research\qca6490_bringup\`):**
   - **`s23_pcie0_wlan.asl`** (+ compiled **`s23_pcie0_wlan.aml`** `618 B` & **`s23_pcie0_wlan.hex`** `6,365 B`): S23-exact SSDT (`PCI0` Bus 1 hidden-RP, `_CRS` `0x60300000..0x63FFFFFF` 61 MiB, `_PRT` GSIs `181..184`, `_CCA=1`, `_OSC`, `_DSM`, `R401` reserved ECAM, `WLAN` `_S0W=0`, plus disabled-by-default `S23I0001` GIC MBI SPI `800..811` helper) — compiled with `iasl` (**0 Errors, 0 Warnings, 0 Remarks**).
   - **`s23_uefi_pcie0_dxe.c`** + **`s23_uefi_pcie0_dxe.inf`**: Complete S23-exact UEFI DXE driver (PCIe0 + QCA6490 `0x110317CB` bring-up, `G401` 3-slot iATU ECAM, hidden RP `Interrupt Pin=1`, `PARF_BDF_TO_SID` CRC8, `G735` SMMU-500 coherent identity stage-1 pass-through for SID `0x1401`/`0x1400`, dynamic `MCFG` + SSDT publication) — verified with `clang-18 -Wall -Wextra -Werror` across standalone, real `Mu_Basecore` EDK2 headers, AND with `-DS23_INCLUDE_SSDT_AML` (**0 Errors, 0 Warnings**).
   - **`QCA6490_DRIVER_AND_FW_GUIDE.md`**: Full file-by-file inventory (sizes + MD5s) of `downloaded_qcwlan8280\extracted\` (`qcwlan8280.inf` already matches `PCI\VEN_17CB&DEV_1103` and is WHQL-signed!) and `wifi_research\s23_vendor_wifi_fw\qca6490\` (`bdwlan.elf` `59,900 B`), Gunyah EL2 D3Cold/SMMU/ITS watchpoints, INF patching + `CN=S23TouchTest2020` signing, and `pnputil`/`netsh`/ETW commands.

---

## 2. Complete Inventory of Created & Modified Files

| File (under `C:\Users\Mahan\Documents\windows-on-android\s23_dm1q\`) | Status | Size | MD5 | Verification |
|---|---|---:|---|---|
| `DISPLAY_BLACK_SCREEN_FIX.md` | Created | `18,724 B` | `65e7dd11298c87b2757194f72fb3d6c9` | Complete report |
| `s23_display_autorefresh_fix.patch` | Created | `25,270 B` | `a0b3e8e833da1d547f55944fb8d40cdb` | Surgical 6-file diff |
| `Mu-dm1q-dispfix.img` | Created | `2,443,264 B` | `067994ddba831ebbe27c5705358e7710` | `build_uefi.py -d dm1q -r RELEASE` RC=0 |
| `drivers\s23disp_diag\s23disp_diag.c` | Created | `21,291 B` | `39ba3cb6fb71ef34304f6c1c29cb55a4` | `clang-18 -Wall -Wextra` 0 warnings |
| `drivers\s23disp_diag\build_wsl.py` | Created | `2,332 B` | `0996d981133cca79260f8e72f139698d` | Built `out\s23disp_diag.sys` RC=0 |
| `drivers\s23disp_diag\sign.ps1` | Created | `1,153 B` | `05e83d333eeefdbd79332cf94fd4150d` | Signed with `CN=S23TouchTest2020` |
| **`drivers\s23disp_diag\pkg\s23disp_diag.sys`** | **Created/Signed** | **`34,696 B`** | **`19a4d0e1e9649e97a2afc259f8e9ab1f`** | **PE ARM64, TSAWARE=0, `.reloc` `0xB000+24`, AuthSig `0x8200+1416`, 13/13 `ntoskrnl` imports OK** |
| `drivers\s23pcieprobe\s23pcieprobe.c` | Modified (`v1.0.0.10`) | `62,176 B` | `9764b94ae5843263d17807ad6a9655c1` | `clang-18 -Wall -Wextra` 0 warnings |
| **`drivers\s23pcieprobe\pkg\s23pcieprobe.sys`** | **Rebuilt/Signed** | **`77,192 B`** | **`41bb22874d05ef36e57f1ac10f3d5267`** | **PE ARM64, TSAWARE=0, `.reloc` `0x19000+32`, AuthSig `0x12800+1416`, 10/10 `ntoskrnl` imports OK** |
| `deploy_s23pcieprobe.py` | Modified | `6,392 B` | `f9b10ff855b844fc29857c32dbfb3af4` | `py_compile` RC=0; rejects unsigned `out\`, accepts signed `pkg\` |
| `PCIE0_PROBE_AUDIT.md` | Created | `11,203 B` | `f0142f6b623e14cf25939940e2c9b28f` | Complete line-by-line audit |
| `wifi_research\qca6490_bringup\s23_pcie0_wlan.asl` | Created | `11,402 B` | `8b507fbabe62d73f5dcbd491f15644e3` | `iasl -tc` 0 Errors, 0 Warnings, 0 Remarks |
| `wifi_research\qca6490_bringup\s23_pcie0_wlan.aml` | Compiled | `618 B` | `c8a66d10968cb844103abf8e56b9af48` | Compiled SSDT AML |
| `wifi_research\qca6490_bringup\s23_pcie0_wlan.hex` | Compiled | `6,365 B` | `e0573f55824c3696c279928a94a91546` | C array `s23_pcie0_wlan_aml_code[]` |
| `wifi_research\qca6490_bringup\s23_uefi_pcie0_dxe.c` | Created | `34,402 B` | `d9ea6db74342ca4e7fc5efa82435bfac` | `clang-18 -Wall -Wextra -Werror` RC=0 (standalone + real EDK2 + SSDT hex) |
| `wifi_research\qca6490_bringup\s23_uefi_pcie0_dxe.inf` | Created | `737 B` | `272bfafa2c779b52b59c615263750ae0` | EDK2 DXE INF |
| `wifi_research\qca6490_bringup\QCA6490_DRIVER_AND_FW_GUIDE.md` | Created | `10,364 B` | `4d17a3066c4232e6384bdca37f587f57` | Complete Windows driver & FW guide |

---

## 3. Safety Rules Verification

- **`backups\` Strictly Read-Only:** Verified every file in `C:\Users\Mahan\Documents\windows-on-android\s23_dm1q\backups\` retains its original `Thu Nov 19 2026` mtime (zero modifications).
- **Working UEFI Untouched:** `Mu-dm1q-cca0.img` (`2,443,264 B`, `md5=5a46cd39d18d9bd3b46b3b8565d5d16f`, `Nov 20 00:11`) was NOT modified; the new display-fix build was saved strictly to `Mu-dm1q-dispfix.img`.
- **Phone Safely in Recovery:** Verified via read-only `adb devices`: `RFCW90N0N7X	recovery`. Phone was NEVER rebooted, flashed, shut down, or taken out of Lineage Recovery.
- **System Time Untouched:** Never called `Set-Date` or changed any OS time/system settings.

---

## 4. Prioritized Tomorrow-Morning Runbook

### Step 1 — Test `s23pcieprobe.sys v1.0.0.10` (Confirm PCIe0 L0 + `0x110317CB`)
1. From Lineage Recovery (phone is already ready!), run the fixed deployment script:
   ```powershell
   python C:\Users\Mahan\Documents\windows-on-android\s23_dm1q\deploy_s23pcieprobe.py
   ```
   *(Verify it prints `PE+Authenticode OK: s23pcieprobe.sys (77192 B ...)` before pushing and rebooting.)*
2. Wait for Windows 11 to boot and connect via `s23link.py`. *(If the AMOLED screen stays black on warm reboot from recovery, Windows is still alive over USB-C PC-Link — proceed directly to Step 2 or force a 7 s `Power+VolDown` hard reset!)*
3. Run the probe on S23 Windows (`StageMask = 0x3F` for full bring-up + ECAM `0x60100000` readback):
   ```powershell
   # Via s23link / admin shell on S23:
   reg add "HKLM\SYSTEM\CurrentControlSet\Services\s23pcieprobe\Parameters" /v StageMask /t REG_DWORD /d 0x3F /f
   sc start s23pcieprobe
   type C:\Users\Public\s23pcieprobe.log
   ```
4. **Target Result:** Look for `LINK UP (pass 1 x1)` or `LINK UP (pass 2 x2/NL=1 + CLKREQ_OVR)`, `Negotiated Link: Gen1 x1` (or `x2`), and **`[0x00] Vendor/Device ID = 0x110317CB`**!

### Step 2 — Fix & Verify AMOLED Warm-Reboot Display
1. If Windows booted with a black screen after warm reboot from Lineage Recovery, copy `drivers\s23disp_diag\pkg\s23disp_diag.sys` to `C:\Users\Public\s23disp_diag.sys`, create a Demand Start service (`sc create s23disp_diag type= kernel start= demand binPath= C:\Users\Public\s23disp_diag.sys`), and run:
   - First with `ActionMask = 0x00` (read-only) -> inspect `C:\Users\Public\s23disp_diag.log` to check `G116Gate`, `INTF_1 AR CFG`, and `FRAME_CNT` delta.
   - If `G116Gate = OPEN` and `FRAME_CNT` is stalled: set `ActionMask = 0x01` (re-arm Auto-Refresh `0x80000002` + `CTL_0_START`), then if still dark set `ActionMask = 0x02` (`S6E3FAC` max brightness `0x51 0x07 0xFF` + `0x29` Display On)!
2. Flash/boot test **`Mu-dm1q-dispfix.img`** to verify warm-reboot display recovery automatically at the UEFI stage (keep `Mu-dm1q-cca0.img` as your known-good fallback!).

### Step 3 — Move Straight to Native QCA6490 Windows Wi-Fi (Once `0x110317CB` Confirmed!)
1. Drop `wifi_research\qca6490_bringup\s23_uefi_pcie0_dxe.{c,inf}` + `s23_pcie0_wlan.hex` into `Mu-Silicium/Platforms/Samsung/dm1qPkg/Drivers/S23Pcie0Dxe/`, enable `#define S23_INCLUDE_SSDT_AML 1`, add `Kailua/MCFG.aml` + `Kailua/IORT.aml` to `dm1q/AcpiTables.inf`, and build `Mu-dm1q-wifi.img`.
2. Boot `Mu-dm1q-wifi.img` **before** installing the Wi-Fi driver and confirm `PCI\VEN_17CB&DEV_1103` appears cleanly in `pnputil /enum-devices /instanceid "PCI\VEN_17CB*"` with a BAR0 window in `0x60300000..0x63FFFFFF`.
3. Follow `wifi_research\qca6490_bringup\QCA6490_DRIVER_AND_FW_GUIDE.md` to install `downloaded_qcwlan8280\extracted\qcwlan8280.inf` (with S23 `bdwlan.elf` + `S0IdleEnable = 0` + D3Cold disabled) and test `netsh wlan show networks`!
