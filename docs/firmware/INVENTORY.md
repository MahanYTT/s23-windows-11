# Firmware dump inventory — S23 (`dm1q`)

Measured metadata for the stock firmware pulled read-only from the device into
`s23_dm1q_firmware_dump/`. That dump **stays outside this repository**: no firmware, blob or log
is committed here, only numbers you can reproduce with the commands below.

```powershell
python scripts/verify_firmware_dump.py <path-to>\s23_dm1q_firmware_dump --images-only --include-audio
```

Measured 2026-11-26: 105 lines, `FIRMWARE DUMP: OK`. That is 103 ELF/MBN images whose segment hashes all match their own hash segment, 0 failures, and 1 non-ELF container skipped.

## 1. Folders

| Folder | Files | Bytes | ELF/MBN images checked |
|---|---:|---:|---:|
| `01_gpu` | 9 | 170,004 | 3 |
| `02_wifi_bt_qca6490` | 48 | 23,887,686 | 6 |
| `03_audio_adsp_haptics` | 82 | 36,880,141 | 3 |
| `04_modem_5g_dsp_apnhlos` | 688 | 385,725,826 | 91 |

`modem_5g_research/FIRMWARE_GROUND_TRUTH.md` reports 827 files and 446,663,657 bytes for
`04_modem_5g_dsp_apnhlos/`. The dump on disk today has fewer, so the two snapshots differ. Re-measure
before you rely on either total. For every image listed in both documents, the load spans below match exactly.

## 2. Images with a fixed physical load address

These are the images whose `PT_LOAD` segments carry absolute `p_paddr` values. They are the ones
whose carve-outs a Windows driver or ACPI table must never overlap. Everything else in the dump
links at `0x0`/`0x1000` (relocatable TAs, GPU zap shader, WLAN board data).

| Image | Load span (`p_paddr`) | Size | Segment hashes |
|---|---|---:|---|
| `04_.../modem_image/modem.mdt` | `0x8A800000..0x9B000000` | 264 MiB | 28/28 SHA-384 |
| `04_.../modem_image/modem_dtb.mdt` | `0x9B000000..0x9B000200` | 512 B | 2/2 SHA-384 |
| `04_.../apnhlos_image/cdsp.mdt` | `0x9C900000..0x9E900000` | 32 MiB | 14/14 SHA-384 |
| `04_.../apnhlos_image/cdsp_dtb.mdt` | `0x9E900000..0x9E900200` | 512 B | 2/2 SHA-384 |
| `04_.../apnhlos_image/adsp_dtb.mdt` (= `03_.../adsp_pil/adsp_dtb.mdt`) | `0x9E980000..0x9E9820B8` | 8,376 B | 2/2 SHA-384 |
| `04_.../apnhlos_image/adsp.mdt` (= `03_.../adsp_pil/adsp.mdt`) | `0x9EA00000..0xA43B4000` | `0x059B4000` | 46/46 SHA-384 |
| `04_.../apnhlos_image/cpusys_vm.mdt` | `0x80A00000..0x80C0120D` | `0x20120D` | 3/3 SHA-384 |
| `03_.../cs35l43_cs40l26_vendor/vpu30_4v.mbn` | `0x0F500000..0x0FBFF020` | `0x6FF020` | 11/11 SHA-384 |
| `02_.../qcwlan8280_win11_driver/wlanfw.mbn` | `0x01400140..0xF001F000` | (sparse) | 16/16 SHA-256 |
| `02_.../qcwlan8280_win11_driver/wlanfw20.mbn` | `0x01400140..0xF001F000` | (sparse) | 15/15 SHA-256 |

The WLAN span is the min/max over sparse segments in the QCA6490's own address space, not an AP
carve-out.

## 3. Not covered

- `04_.../apnhlos_image/prov.mdt` + `prov.b00..b08`: not ELF (magic `10 e3 5b 1e`). It looks like an encrypted container and is covered only by file hashes.
- Segment-hash consistency is **not** a signature check. Nothing here proves the images are what Qualcomm or Samsung signed.
- Register maps, OPCODE payloads and the Cirrus `.wmfw`/`.bin` contents are documented per subsystem (below), not here.

## 4. Per-subsystem ground truth

| Subsystem | Document |
|---|---|
| Modem / IPA / CDSP / ADSP PIL images | `modem_5g_research/FIRMWARE_GROUND_TRUTH.md` |
| USB-C / MAX77705 | `usb_research/S23_USBC_ARCHITECTURE.md` |
| Firmware dump verifier and its tests | `scripts/verify_firmware_dump.py`, `tests/test_verify_firmware_dump.py` |

## 5. Core driver host tests that pin current behaviour

These tests extract code verbatim from the production drivers (the drivers are not modified) and
run it under ASan + UBSan on the host (`python tests/c/run_c_tests.py`):

| Test | Driver code under test |
|---|---|
| `tests/c/test_batt_fg.c` (`extract_batt.py`) | `s23batt.c`: `S23BattReadHardware` MAX77705 FG/charger decode → SOC, mV, mA, 0.1 K, mWh, Windows power state; log formatting bounds |
| `tests/c/test_bthx_h4.c` (`extract_bthx.py`) | `s23bthx.c`: H4 RX state machine `ProcessRxByte`, `DeliverRxPacket`, IRP queue and packet ring |
| `tests/c/test_touch_report.c` (`extract_touch.py`) | `s23touch.c`: HID report descriptor vs. `S23EmitTouchFrame` field offsets, 10-finger frames, event decode loop |

Scale factors and bit layouts those tests lock are **as implemented**. They are regression guards,
not datasheet verification (for example, the battery Rsense of 2 mΩ is inferred from the driver's LSBs and
has not been checked against a schematic).
