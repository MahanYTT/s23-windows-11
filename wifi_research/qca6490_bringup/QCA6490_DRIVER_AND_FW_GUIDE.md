# Samsung Galaxy S23 (`dm1q` / `SM8550-AC`) — QCA6490 Windows 11 ARM64 Driver & Firmware Bring-Up Guide

**PCI Hardware ID:** `PCI\VEN_17CB&DEV_1103` (Qualcomm QCA6490 / WCN685x / FastConnect 6900)  
**Target OS:** Windows 11 Pro 24H2 ARM64 (`10.0.26100.4349`)  
**Signing Certificate (for patched INF/test builds):** `CN=S23TouchTest2020` (Thumbprint `05DBD92557F4FC52EF2A5E5955789D8EED637C68`, valid `2020–2040`, TestSigning enabled on S23)  

---

## 1. Inventory of Driver & Firmware Files Already on Disk

### 1.1 Qualcomm Windows 11 ARM64 Driver Package (`downloaded_qcwlan8280\extracted\`)
Already downloaded and ready — **`qcwlan8280.inf` (`DriverVer = 07/16/2025,1.0.4369.8700`) ALREADY natively matches `PCI\VEN_17CB&DEV_1103` (`%QcWlan.DeviceDesc.6490% = QcWlan_LITHIUM.ndi`) and is WHQL-signed (`qcwlan8280.cat`)!**

| File | Size | MD5 | Role in `qcwlan8280.inf` |
|---|---|---|---|
| `qcwlan8280.sys` | `3,287,440 B` | `2cb9e430d24077987130b87db6aabd82` | WDI / KMDF 1.33 ARM64 Wi-Fi 6E miniport (`%12%\qcwlan8280.sys`) |
| `qcwlan8280.inf` | `34,248 B` | `7dcae3fe476e3ca78b9c328640ad7d86` | INF matching `PCI\VEN_17CB&DEV_1103` & `SUBSYS_010817CB` |
| `qcwlan8280.cat` | `24,362 B` | `306d22cdc54c7e2a2d9e3c960eed8789` | Official Qualcomm/Microsoft WHQL catalog signature |
| `wlanfw.mbn` | `4,847,392 B` | `be8aab4e4532873e36fcc03db3d21f4c` | QCA6490 Rev 1.x AMSS firmware (BHIe/MHI) |
| `wlanfw20.mbn` | `4,878,336 B` | `7228a13ecc119e5a91c312ab130b0733` | QCA6490 Rev 2.x AMSS firmware (BHIe/MHI) — S23 uses 2.x! |
| `m3.bin` / `m320.bin` | `266,684 B` | `51478cd801a44bce7d4cd2295cc59701` | M3 subsystem firmware |
| `regdb.bin` | `24,278 B` | `2b0213641c932c6d99249b0acfaff434` | Regulatory database |
| `Data.msc` / `Data20.msc` | `723,097 B` / `738,720 B` | `346abea9...` / `7b549585...` | MHI firmware metadata / ELF metadata |
| `bdwlan.elf`, `bdwlan.e*`, `bdwlang.*` | `59,868–59,968 B` (22 files) | various | Generic/laptop board calibration data (BDF) |

### 1.2 S23 Stock Android Vendor Firmware (`wifi_research\s23_vendor_wifi_fw\`)
Extracted directly from Samsung Galaxy S23 (`dm1q`) `/vendor/firmware_mnt/image` & `/vendor/firmware/wlan/qca_cld/qca6490`:

| S23 Stock File | Size | MD5 | Corresponding Windows `%12%` Target |
|---|---|---|---|
| `qca6490/amss20.bin` | `5,496,832 B` | `fd0795350c2a9a217df2a3c9e0dbea0f` | `wlanfw20.mbn` (note: **try Qualcomm's `wlanfw20.mbn` first** because Windows WDI expects Windows-ABI WMI/QMI host interface; S23's `bdwlan.elf` is the key RF calibration file!) |
| `qca6490/m3.bin` | `266,684 B` | `8b104cd8a1b43ec44655b376dcf72672` | `m320.bin` / `m3.bin` (identical 266,684 B container format) |
| `qca6490/bdwlan.elf` | `59,900 B` | `916a72b57f9fc2c6140ea982fb60d9a2` | **`bdwlan.elf` (S23-exact RF front-end/antenna BDF — critical for TX/RX RF calibration!)** |
| `qca6490/bdwlan.elf{1,2,10}` | `59,900 B` each | `34cb6585...` / `f6fd694a...` / `dfe86962...` | SKU/revision BDF variants |
| `qca6490/bdwlang.elf{,1,2,10}` | `59,900 B` each | `b4550c79...` / `2cf07c22...` / `9eb3a94b...` | Alternative BDFs |
| `qca6490/regdb.bin` | `24,278 B` | `540ea32af226221811c702112f747fbb` | `regdb.bin` (exact same 24,278 B size) |
| `Data.msc` | `967,519 B` | `c0a96923567db27425639c2a08c59c01` | `Data20.msc` (keep Qualcomm Windows `Data20.msc` paired with `wlanfw20.mbn`) |

> **Important Firmware Pairing Rule:**
> Always pair **`wlanfw20.mbn` + `m320.bin` + `Data20.msc`** from the **same** build (start with Qualcomm's Windows `downloaded_qcwlan8280\extracted\` set so WDI/WMI version matches `qcwlan8280.sys`!), and swap in S23's **`bdwlan.elf`** (`59,900 B`, `md5=916a72b57f9fc2c6140ea982fb60d9a2`) if RF calibration / antenna gain / BDF board-ID matching fails with the generic laptop `bdwlan.elf`.

---

## 2. Critical Gunyah EL2 Hypervisor & INF Notes (Before First Driver Load!)

Lessons directly proven on SM8550/SM8650 Gunyah phones (`oneplus-open-windows` + `rm9-windows-arm64`):

1. **D3Cold & ASPM MUST Be Disabled Initially (Awake-Pinned Profile):**
   - Stock `qcwlan8280.inf` line 71 includes `Needs = PciASPMOptIn.HW, PciD3ColdSupported.HW` and `S0IdleEnable = 1`.
   - Because S23 Windows has no PEP/CNSS ACPI power-gating handler to re-run QMP/RPMh/PHY bring-up after D3Cold, allowing Windows to put `VEN_17CB&DEV_1103` into D3Cold kills the PCIe link permanently until reboot!
   - **Fix:** Our `s23_pcie0_wlan.asl` sets `_S0W = 0` on `WLAN`, AND in the patched S23 INF we remove `PciD3ColdSupported.HW` and set `S0IdleEnable = 0`, `D0PCEnable = 0`, `wowEnable = 0`.
2. **SMMU / DMA Remapping (`DmaRemappingCompatible`):**
   - Stock `qcwlan8280.inf` line 304 sets `HKR,Parameters,DmaRemappingCompatible,0x00010001,1`.
   - When `s23_uefi_pcie0_dxe.c` programs SMMU-500 (`0x15000000`) into coherent identity pass-through (`M=0`) for SID `0x1401`, Windows HAL does not need to build stage-1 IOVA tables if IORT omits SMMU or uses identity mapping; if Windows attempts SMMU programming and Gunyah traps writes to `0x15000000`, set `DmaRemappingCompatible = 0` in `HKLM\SYSTEM\CurrentControlSet\Services\qcwlan\Parameters`.
3. **GIC ITS MSI vs Gunyah EL2 Stall (`CLOCK_WATCHDOG_TIMEOUT` Watchpoint!):**
   - Stock `qcwlan8280.inf` lines 120–121 enable `MSISupported = 1`, `MessageNumberLimit = 16`.
   - On Qualcomm Gunyah EL2 phones (`rm9-windows-arm64/docs/wifi-bringup.md` §§4–7):
     - **Step 1 (Verify Enumeration First):** Boot with `s23_uefi_pcie0_dxe` + `s23_pcie0_wlan.aml` **BEFORE** installing `qcwlan8280.inf`. Confirm `PCI\VEN_17CB&DEV_1103` appears in Device Manager (Code 28: no driver) with BAR0 assigned inside `0x60300000..0x63FFFFFF` and **zero hangs**.
     - **Step 2 (Test MSI / Legacy INTx):** Install `qcwlan8280.inf` with `MSISupported = 1` first (to check whether S23's Gunyah EL2 permits `DeviceID 0x1401` ITS commands — remember Root Port `0x1400` is ALREADY hidden via `_BBN = 1` + `Interrupt Pin = 1`!).
     - **Step 3 (If `CLOCK_WATCHDOG_TIMEOUT` occurs on ITS `MAPD`/`MAPTI`):**
       - First test `MSISupported = 0` in the patched INF (forces legacy INTA via `_PRT` -> GIC SPI 149 / GSI 181, completely bypassing GIC ITS!).
       - If `qcwlan8280.sys` requires multi-vector MSI (MHI CE + DP ext groups), enable the `\_SB.S23I` (`S23I0001`) GIC MBI SPI `800..811` (`0x17110040`) interrupt-redirect architecture already stubbed in `s23_pcie0_wlan.asl`!

---

## 3. Exact INF Patching, Test-Signing (`CN=S23TouchTest2020`) & Installation Steps

### 3.1 Option A — Stock Unmodified WHQL Install (Zero Resigning Needed!)
Because `downloaded_qcwlan8280\extracted\qcwlan8280.inf` already matches `PCI\VEN_17CB&DEV_1103` and has a valid WHQL `qcwlan8280.cat`:
```powershell
# On S23 Windows (via s23link / PowerShell as Administrator):
pnputil /add-driver C:\Users\Public\qcwlan8280\qcwlan8280.inf /install

# Disable S0Idle / D3Cold immediately in the device registry before reboot:
reg add "HKLM\SYSTEM\CurrentControlSet\Services\qcwlan\Parameters" /v DmaRemappingCompatible /t REG_DWORD /d 0 /f
```

### 3.2 Option B — Patched S23 INF (S23 `bdwlan.elf` + No D3Cold + Optional `MSISupported=0`)
1. Copy `downloaded_qcwlan8280\extracted\*` to a working staging folder `wifi_research\qca6490_bringup\pkg_s23_qcwlan\`.
2. Overwrite `bdwlan.elf` with S23's stock `wifi_research\s23_vendor_wifi_fw\qca6490\bdwlan.elf` (`59,900 B`, `md5=916a72b57f9fc2c6140ea982fb60d9a2`).
3. Edit `qcwlan8280.inf`:
   - Under `[QcWlan_LITHIUM.ndi.NTARM64.HW]`: change `Needs = PciASPMOptIn.HW, PciD3ColdSupported.HW` -> remove `PciD3ColdSupported.HW` (or comment out both `Include`/`Needs` lines).
   - Under `[S0Idle.reg]`: set `HKR, , S0IdleEnable, 0x00010001, 0`.
   - Under `[wowEnable.reg]`: set `HKR, , wowEnable, 0x00010001, 0`.
   - Under `[MSI.reg]` (ONLY if GIC ITS stalls on S23 Gunyah): set `MSISupported, 0x00010001, 0`.
   - Under `[QcWlan.Service.AddReg]`: set `DmaRemappingCompatible, 0x00010001, 0` (when using UEFI SMMU identity pass-through).
4. Regenerate catalog & test-sign with `CN=S23TouchTest2020` (on host Windows PC):
```powershell
$pkg  = "C:\Users\Mahan\Documents\windows-on-android\s23_dm1q\wifi_research\qca6490_bringup\pkg_s23_qcwlan"
$cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq 'CN=S23TouchTest2020' -and $_.HasPrivateKey } | Select-Object -First 1

# Rebuild qcwlan8280.cat (requires WDK inf2cat.exe)
& "C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x86\inf2cat.exe" /driver:"$pkg" /os:10_VB_ARM64 /verbose

# Sign catalog (and leave qcwlan8280.sys untouched or re-sign if patched)
Set-AuthenticodeSignature -FilePath (Join-Path $pkg "qcwlan8280.cat") -Certificate $cert -HashAlgorithm SHA256
```
5. Install on S23 Windows (with `bcdedit /set testsigning on` and `s23test.cer` already in `Root` & `TrustedPublisher`):
```powershell
pnputil /add-driver C:\Users\Public\pkg_s23_qcwlan\qcwlan8280.inf /install
```

### 3.3 Post-Install Verification Commands (Live on S23 Windows)
```powershell
# 1. Confirm PCI enumeration, BAR0 allocation, and driver binding (Status = Started, Problem = 0)
pnputil /enum-devices /instanceid "PCI\VEN_17CB*" /properties /resources

# 2. Confirm Wi-Fi adapter & WDI driver initialization
netsh wlan show drivers
netsh wlan show interfaces
netsh wlan show networks mode=bssid

# 3. Capture WlanLogger ETW trace if FW download (BHIe/MHI) or QMI fails:
logman start WlanTrace -p {bb6f5b93-635c-47be-816f-e895e77064a8} 0xFFFFFFFF 5 -o C:\Users\Public\wlan.etl -ets
# ... reproduce ...
logman stop WlanTrace -ets
```

---

## 4. Integrating `s23_pcie0_wlan.asl` & `s23_uefi_pcie0_dxe.c` into `Mu-dm1q`

Once `s23pcieprobe.sys v1.0.0.10` confirms `LINK UP` + `0x110317CB` tomorrow morning:
1. Compile `s23_pcie0_wlan.asl` into a C header array in WSL:
   ```bash
   iasl -tc -p /tmp/s23_pcie0_wlan s23_pcie0_wlan.asl
   ```
2. Copy `s23_uefi_pcie0_dxe.c`, `s23_uefi_pcie0_dxe.inf`, and the generated `s23_pcie0_wlan.hex` into `Mu-Silicium/Platforms/Samsung/dm1qPkg/Drivers/S23Pcie0Dxe/`.
3. Enable `#define S23_INCLUDE_SSDT_AML 1` and add `dm1qPkg/Drivers/S23Pcie0Dxe/S23Pcie0Dxe.inf` to `dm1q.dsc`, `Include/DXE.inc`, and `Silicium-ACPI/Platforms/Samsung/dm1q/AcpiTables.inf` (adding `ASL|Kailua/MCFG.aml` and `ASL|Kailua/IORT.aml`!).
4. Build a separate test UEFI image (`Mu-dm1q-wifi.img`) without touching `Mu-dm1q-cca0.img` or `backups\`.
