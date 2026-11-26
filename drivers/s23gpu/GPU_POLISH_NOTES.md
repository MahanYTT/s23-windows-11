# s23gpu polish notes (track 1: adapter IDs, 64 Hz, WoW64/x64, standby, ACPI\SSDE)

These notes cover five open s23gpu symptoms. Each section separates what the repo proves
(cited `path:line`) from what is only a hypothesis. **No change in this track alters how
`s23gpu.sys` behaves.** The only code added is a host test (`tests/c/test_gpu_modes.c`), its
marker extractor (`tests/c/extract_gpu_modes.py`), and a comment in `build.ps1`.

Line numbers refer to commit `eaaf2f9` (branch `ci/auto-discovery`).

No local WDK, kernel tree or DTS was available for this work. Every WDK name below that is not
already used in this repo comes from memory and is labelled **unverified**.

---

## a) DXGI reports `VendorId=0x0000 DeviceId=0x0000`

**What the code does**

- `QueryAdapterInfoInner` (`drivers/s23gpu/ddi.c:242-467`) handles DRIVERCAPS, QUERYSEGMENT3/4,
  PHYSICALADAPTERCAPS, GPUMMUCAPS, PAGETABLELEVELDESC and DISPLAY_DRIVERCAPS_EXTENSION. It also
  handles the WDDM 2.4 perf/version types 24-27, types 6/7 and HISTORYBUFFERPRECISION.
  None of these types carries a PCI vendor or device ID. Every other type returns
  `STATUS_NOT_SUPPORTED`.
- The driver registers as WDDM 2.0: `drivers/s23gpu/s23gpu.h:6` and
  `d.Version = DXGKDDI_INTERFACE_VERSION_WDDM2_0` (`drivers/s23gpu/ddi.c:1369`).
- The adapter is a root-enumerated device, `ROOT\DISPLAY\0000`
  (`drivers/s23gpu/deploy_s23gpu.ps1:145`). The installer already sets these hardware IDs
  (`drivers/s23gpu/s23gpu_install.c:12-15`):
  `ROOT\S23GPU`, `PCI\VEN_5143&DEV_0740&SUBSYS_07405143&REV_01`, `PCI\VEN_5143&DEV_0740`.
  `s23gpu.inf` also matches `PCI\VEN_5143&DEV_0740`.

**Conclusion (backed by the repo)**

DXGI still reports `0x0000/0x0000` even though those `VEN_5143&DEV_0740` hardware-ID strings
are present. So dxgkrnl does not read vendor/device IDs from a root device's hardware-ID
strings. Adding or changing hardware IDs or INF match strings will not fix this.

For PCI adapters, dxgkrnl takes the IDs from the bus (PCI configuration space). A
root-enumerated device has no bus interface, so the IDs come out as zero.

**Why nothing was implemented**

I could not cite, from a header I can see, a kernel-mode `DXGKQAITYPE_*` value plus output
structure that lets a WDDM 2.0 KMD supply vendor/device IDs.

- The user-mode query side exists: `KMTQAITYPE_PHYSICALADAPTERDEVICEIDS` returns
  `D3DKMT_QUERY_DEVICE_IDS { PhysicalAdapterIndex; D3DKMT_DEVICE_IDS DeviceIds }`, where
  `D3DKMT_DEVICE_IDS` has `VendorID, DeviceID, SubVendorID, SubSystemID, RevisionID, BusType`.
  That is where DXGI reads the IDs, but it is dxgkrnl's output, not a DDI the KMD answers.
  (From memory; **unverified** against a local `d3dkmthk.h`.)
- I am not aware of a matching `DXGKQAITYPE_PHYSICALADAPTERDEVICEIDS` on the KMD side.
- Any newer QAI type would not be sent to a driver that registers as WDDM 2.0 anyway.

The track rule is: implement only if the exact WDK type and field names can be cited. That bar
is not met, so `ddi.c` is unchanged.

**Notes for whoever continues this**

- `0x5143` is Qualcomm's **Khronos/Vulkan** vendor ID (ASCII "QC"). It is not a PCI-SIG vendor
  ID; Qualcomm's PCI-SIG ID is `0x17CB`.
- On commercial Snapdragon Windows devices the Adreno adapter is ACPI-enumerated
  (`QCOM0C36`-style `_HID`, see `gpu_research/S23_ADRENO740_ARCHITECTURE.md:181`). I believe
  DXGI there reports `VendorId = 0x4D4F4351` ("QCOM" as little-endian ASCII). **Unverified.**
  If that is right, apps that detect Qualcomm GPUs on Windows check `0x4D4F4351`, not `0x5143`.
- Approaches that might work (all untested, none attempted here):
  1. Enumerate the GPU through ACPI with a Qualcomm-style `_HID`. See the `GPU0` node in
     `gpu_research/s23_gpu_bringup/s23_gpu_acpi.asl`, and whether dxgkrnl derives IDs from
     ACPI hardware IDs.
  2. A bus driver or filter that answers `GUID_BUS_INTERFACE_STANDARD` config-space reads with
     a synthetic header.

  Either needs on-device experiments. To check the result, run `s23gpu_d3d11_test.exe`; it
  prints `ven=/dev=` (`drivers/s23gpu/s23gpu_d3d11_test.c:99`).

---

## b) Legacy `EnumDisplaySettings` shows 64 Hz while CCD/DWM run at 120 Hz

**Every mode-related number the driver reports**

| Source | Where | Values |
|---|---|---|
| VidPN target modes | `AddTargetModes` (`disp.c:1265`), `kHz[] = {120, 60}` (`disp.c:1272`) | `FillVideoSignal` (`disp.c:1189-1208`) |
| Monitor source modes | `DispRecommendMonitorModes` (`disp.c:1400`), same `kHz[]` (`disp.c:1403`) | `FillVideoSignal` |
| `FillVideoSignal` | `disp.c:1189` | Active 1080x2340, Total 1200x2400, `VSyncFreq = Hz/1`, `HSyncFreq = 2400*Hz`, `PixelRate = 1200*2400*Hz` |
| EDID DTD 1 / DTD 2 | `DispQueryDeviceDescriptor` (`disp.c:1477`, DTDs at `disp.c:1510-1527`) | 345.60 MHz and 172.80 MHz, 1080+120 x 2340+60 |
| Commit path | `QueryVidPnHz` (`disp.c:1120`) / `DispCommitVidPn` (`disp.c:1159`) | Reads the pinned target mode `VSyncFreq` and accepts only 60 or 120 |
| Scan line | `DispGetScanLine` (`disp.c:1541`) | 2400 lines per frame, VBlank from line 2340 |
| VSync timer | `DispVsyncEnable` (`disp.c:779`) | Period 7.95 ms at 120 Hz, 16.45 ms at 60 Hz |

**What the new test proves**

`tests/c/test_gpu_modes.c` uses `FillVideoSignal`, `DispQueryDeviceDescriptor`, both `kHz[]`
tables and `S23_FB_WIDTH/HEIGHT`, extracted verbatim from the driver by
`tests/c/extract_gpu_modes.py`. It decodes the EDID with an independent E-EDID 1.3 decoder
(VESA E-EDID: detailed-timing, standard-timing and display-descriptor layouts) and checks:

- `PixelRate / (TotalSize.cx * TotalSize.cy) == VSyncFreq` and
  `HSyncFreq / TotalSize.cy == VSyncFreq`, exactly, for 120 and 60 Hz. This holds with the
  `S23_FB_*` fallback and with the live POST geometry.
- Both DTDs decode to exactly 120.000 Hz and 60.000 Hz. Their active and total sizes and pixel
  clock match `FillVideoSignal`, and the preferred DTD (the first one) is the preferred
  `kHz[0]` = 120.
- There are no established timings and no range-limits (`0xFD`) descriptor. None of the eight
  standard-timing slots can decode to 64 Hz.
- `DispQueryDeviceDescriptor` error paths write nothing, and it never writes past 128 bytes.

The test passes on unmodified code (274 checks). **No arithmetic in the driver produces or
implies 64 Hz**, so there is no provable code bug to fix, and the driver is unchanged.

**Side finding (not changed)**

The eight standard-timing slots (EDID bytes 38..53) are `00 00`. E-EDID 1.3 says unused slots
must be filled with `01 01`. A lenient parser would read `00 00` as 248xN at 60 Hz, never
64 Hz. I left this alone for two reasons:

- It is not the 64 Hz cause.
- Changing EDID bytes changes the EDID checksum, which Windows may use to key the persisted
  display configuration. That is a behaviour change on a driver that works today.

It is a one-line, spec-backed fix if wanted later; the test prints a `NOTE:` line for it.

**Hypothesis to check on the device (unverified)**

1/64 s = 15.625 ms, which is exactly the default Windows clock interval
(`gpu_research/S23_ADRENO740_ARCHITECTURE.md:432`). My recollection is that a timer-driven
display path such as the Microsoft Basic Display Adapter reports 64 Hz.

`s23disp_only.c:1-3` exists to move the active path away from the Microsoft Basic Display
Driver. That strongly suggests that adapter is still present. So the `\\.\DISPLAYn` queried
through the legacy API may belong to BasicDisplay rather than s23gpu.

Check on the phone:

```powershell
Get-CimInstance Win32_VideoController | Format-Table Name, PNPDeviceID, CurrentRefreshRate, Status
C:\s23gpu\s23disp_only.exe     # logs PATH[...] name/mon/drv per adapter path
```

- If 64 Hz is reported only for the BasicDisplay controller, the fix is to keep that adapter
  inactive or disabled, which is policy, not driver code.
- If s23gpu's own controller shows 64, capture `C:\Users\Public\s23gpu.log` (CommitVidPn
  `hz=` lines) and the `EnumDisplaySettingsEx` index list for that device name before
  changing anything.

---

## c) WoW64 / x64-emulated UMD and WGL packaging

**What `build.ps1` actually produces**

| Artifact | Arch | Source | Packaged into `pkg\` / INF? |
|---|---|---|---|
| `s23gpu_umd.dll` (Mesa `libgallium_d3d10.dll`) | ARM64 | `build_d3d10` (`build.ps1:95-98`, `145-146`) | yes (`UserModeDriverName`) |
| `libgallium_wgl.dll` | ARM64 | `build_d3d10` (`build.ps1:148-154`) | yes (`OpenGLDriverName`) |
| `opengl32.dll` (Mesa `libgl-gdi`) | ARM64 | `build_d3d10` (`build.ps1:148-154`) | no (copied to `C:\s23gpu` by `deploy_s23gpu.ps1`) |
| `x64\libgallium_wgl.dll`, `x64\opengl32.dll` | **x64** | `build_x64` (`build.ps1:156-164`) | no; deployed **app-locally** next to FurMark by `deploy_s23gpu.ps1` |

No x86 (32-bit) artifact and no x64 D3D UMD are built.

**Why `UserModeDriverNameWoW` / `OpenGLDriverNameWoW` were NOT added**

The `...WoW` registry values name the UMD/ICD that **32-bit** processes load from
`%windir%\SysWOW64`. On ARM64 Windows that means x86 processes. x64 apps under emulation are
64-bit processes and do not use the WoW values.

`build.ps1` produces no x86 DLL, so any `...WoW` entry would point at a file that is never
built, or at an x64 DLL that a 32-bit process cannot load. Per the track rule, `s23gpu.inf` is
unchanged. A comment in `build.ps1` now records that `out\x64` is for app-local use only.

Never package Mesa's `opengl32.dll` (`libgl-gdi`) through the INF. It is a full replacement
for the system `opengl32.dll`, not an ICD. Copying it to System32 would break OpenGL for
every process.

**Why x64 apps fall back to WARP (`DXGI_ERROR_UNSUPPORTED`)**

The following is from memory and **unverified on this device**:

- An x64 process on Windows 11 ARM64 is an ARM64EC/x64 process. The system `d3d11.dll` and
  `opengl32.dll` it uses are ARM64X.
- They read the same `UserModeDriverName` / `OpenGLDriverName` and `LoadLibrary` our DLLs
  from System32.
- Those DLLs are classic ARM64, which an x64/ARM64EC process cannot load. The runtime then
  falls back to WARP (D3D) or GDI generic (OpenGL 1.1).

**Still missing for x64 D3D/GL (none of it built or tested here)**

1. An x64 (or ARM64EC) build of the Mesa D3D10 UMD: `build_x64` with the `d3d10umd` target and
   the topaz/msm WDDM winsys from `mesa_s23_a740.patch`. Today `build_x64` only provides `wgl`
   and `libgl-gdi`.
2. One System32 file name that serves both architectures. Microsoft documents an **ARM64X
   "pure forwarder" DLL** (MSVC `link /MACHINE:ARM64X` with separate native and EC `.def`
   files, see Microsoft Learn "Build Arm64X binaries"). It forwards native ARM64 loads to
   `*_arm64.dll` and x64 loads to `*_x64.dll`.
   - D3D: forward `OpenAdapter10` / `OpenAdapter10_2` (`s23gpu_umd.def`).
   - OpenGL: forward the WGL ICD `Drv*` exports. This could reuse the already-built
     `x64\libgallium_wgl.dll`.
   - In both cases the INF would copy the forwarder plus both payload DLLs.
3. Escape ABI: `topazgpu_escape.h` and the msm structs use fixed-width fields, so x64 matches
   ARM64. 32-bit (WoW) callers would additionally need user-VA checks in the
   `TOPAZGPU_NR_MMAP` path.
4. Signing: every new DLL must be added to the `signtool` lines in `build.ps1` and to
   `[SourceDisksFiles]` and `CopyFiles` in the INF.

---

## d) S1-S3 standby "D-state blocker"

**What the code does**

- `TgSetPowerState` (`drivers/s23gpu/ddi.c:217-224`) returns `STATUS_SUCCESS` for every
  `DeviceUid`, `DEVICE_POWER_STATE` and `POWER_ACTION`, and does nothing.
  - It **cannot veto** a D-state transition, so it is not a blocker in itself.
  - On D3 it also does not blank the panel, save state, or stop the VSync timer and scan
    thread.
  - On D0 it restores nothing.

  That is harmless only because nothing powers MDSS down today.
- `DXGK_DRIVERCAPS` (`ddi.c:245-281`) does not set `SupportRuntimePowerManagement`. As far as I
  know (**unverified**), dxgkrnl only queries `DXGKQAITYPE_NUMPOWERCOMPONENTS` (6) and
  `DXGKQAITYPE_POWERCOMPONENTINFO` (7) from drivers that opt into runtime PM. So the
  single-`OTHER`-component reply (`ddi.c:431-449`, "GPU_TSENS", one F-state) is most likely
  dead code.
  - Check on the device: `Select-String 'QueryAdapterInfo type=(6|7) ' C:\Users\Public\s23gpu.log`.
  - If runtime PM is ever enabled, this reply is not sufficient: it needs at least an engine
    component for node 0 and a monitor component for the panel.

**Conclusion**

Nothing in this repo proves that `s23gpu` is what disables S1-S3, so nothing was changed.

S1-S3 availability is decided first by the firmware's ACPI tables: `\_S1` / `\_S3` objects
and the FADT `LOW_POWER_S0_IDLE_CAPABLE` flag. Those tables come from the Project Silicium
UEFI and are not in this repo. To my knowledge, Snapdragon Windows platforms support only
Modern Standby (S0 low-power idle), never S3 (**unverified for this firmware**). The README
already lists Modern Standby / S0ix as unfinished (`README.md:41`, `README.md:176`).

**Next step on the device**

Run `powercfg /a` and quote the exact reason lines. If a "Graphics" or display-driver reason
appears, also check whether the Microsoft Basic Display Adapter is still bound (section b).
Changing `TgSetPowerState` without that evidence could break the working display, so it was
not attempted.

---

## e) Dangling `ACPI\SSDE` (Device Manager problem code 28)

**Search result**

Searching every tracked `.asl` (case-insensitive) for `SSDE` found no `Device (SSDE)` and no
`_HID`/`_CID` that evaluates to `SSDE`:

- `audio_sensors_research/s23_audio.asl`
- `bt_research/s23_bt_uart14.asl`
- `gpu_research/s23_gpu_bringup/s23_gpu_acpi.asl`
- `modem_5g_research/s23_modem_subsystems.asl`
- `modem_5g_research/s23_subsys_pil.asl`
- `wifi_research/qca6490_bringup/s23_pcie0_wlan.asl`

The string `SSDE` appears nowhere else in the repo either.

**Conclusion**

`ACPI\SSDE` is enumerated from the firmware's own DSDT/SSDTs (the Silicium UEFI tables for
`dm1q`), not from an SSDT in this repo. No ASL was written for it.

Problem code 28 only means "no driver installed for this device", so the node is cosmetic
unless something depends on it.

**What would be needed**

1. Identify it on the phone: dump the tables (`acpidump -b` from the ACPICA Windows tools),
   `iasl -d` them, and find the node whose `_HID` is `"SSDE"`, along with its resources and
   `_STA`.
2. Then choose one:
   - **Remove it or return `_STA = 0`** in the firmware's ACPI source and reflash the UEFI.
     An SSDT loaded by the OS cannot delete or redefine an object that already exists in the
     DSDT; that fails with `AE_ALREADY_EXISTS`.
   - **Bind a null driver**: an INF with an `AddService = ,2` (`SPSVCINST_ASSOCSERVICE`) entry
     for `ACPI\SSDE`. This is the standard Windows null-driver pattern and clears code 28
     without touching ACPI. It is not added here because the device's identity and purpose
     are unknown.

---

## Verification

```text
python scripts/ci/check_repo_hygiene.py
python tests/c/run_c_tests.py            # includes test_gpu_modes.c (gcc, and gcc/clang + ASan/UBSan in CI)
python -m unittest discover -s tests
```

`s23gpu` is built with MSVC/WDK (`build.ps1`), and CI does not compile it. This track does not
change its compiled sources, so no on-device step is needed.
