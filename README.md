# Windows 11 on Samsung Galaxy S23 (dm1q)

This repository contains the hardware bring-up drivers and tools necessary to run native Windows 11 Pro 24H2 ARM64 on the Samsung Galaxy S23 (SM-S911B / SM8550-AC).

## Features & Hardware Support

- **UEFI Bootloader**: Extracted, compiled, and integrated custom Project Mu UEFI payload for the `dm1q` platform (`Mu-dm1q-pcie0-dispfix.img`).
- **Display**: Native S6E3FAC panel display rendering via custom UEFI DXE driver (`S23DisplayFixDxe`). Uses MIPI DSI0 TPG-FIFO DCS wake commands (Sleep Out, Display On) over DSI0 BTA.
- **CPU & Memory**: Full access to Snapdragon 8 Gen 2 (SM8550) APPS processors. 
- **Storage**: UFS integration natively handled by the UEFI.
- **Audio (SmartPA)**: Hardware communication established via `s23sensprobe` to Cirrus Logic `CS35L43` and `CS40L26A` on the GENI I2C engine using MSB-first packing (`0x0007F8FE`). Uses dynamic GPIO reset sequencing (TLMM `GPIO42`, `GPIO55`).
- **Bluetooth 5.3**: Native QCA6490 Bluetooth via UART transport driver (`s23bthx.sys`). Implements custom HCI In-Band Sleep (IBS) parser and ACPI `QCOM0831` resource routing. 

## Drivers & Probes

| Driver Name | Description | Status |
|-------------|-------------|--------|
| `s23pilprobe` | SMEM region scanner and PIL loader. Hardened against XPU/Gunyah Data Aborts. | Active |
| `s23sensprobe` | Hardware diagnostic tool for I2C audio amplifiers and GPIO pin states. | Active |
| `s23btprobe`  | QCA6490 GENI UART Bluetooth diagnostic probe. Parses HCI event boundaries and IBS. | Active |
| `s23bthx`     | Native Windows Extensibility Transport (BTHX) kernel mode driver for Bluetooth. | Active (Compiled) |
| `s23disp_diag`| Framebuffer and MIPI DSI0 physical diagnostic driver. | Active |

## Installation & Setup

> **⚠️ WARNING:** This process modifies the device's boot partitions. **NEVER write to `sdb` or `sde`** (Power-On Write-Protected). Modifying these will hard-brick the device. Keep `SanPolicy = 4` to prevent Windows from overwriting original Android partition tables.

1. Ensure the device is unlocked and you have TWRP recovery access.
2. Dump the original boot and UFS partitions (Use the provided python tools).
3. Flash the custom `Mu-dm1q-pcie0-dispfix.img` UEFI to `sda25` (boot) using sector alignment.
4. Push the compiled drivers to the Windows partition.

## Development Environment

To compile the `s23bthx` kernel driver, you need:
- Windows 11 SDK (e.g., `10.0.26100.0`)
- Windows Driver Kit (WDK) matching the SDK version.
- Visual Studio 2022 (MSVC ARM64 Build Tools).

Run `build.bat` in `drivers/s23bthx/` to compile `s23bthx.sys`.

## Credits & Thanks
- Hardware engineering and bringing this to life: @MahanYTT
- The Renegade Project & Project Mu communities for the bootloader foundation.