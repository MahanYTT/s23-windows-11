# Cirrus Logic DSP firmware — ground truth from the stock S23 (`dm1q`) files

Every stock Cirrus container in `s23_dm1q_firmware_dump/03_audio_adsp_haptics/cs35l43_cs40l26_vendor/`
was run through `cirrus_fw.h` (`cirrus_wmfw_validate` / `cirrus_wmdr_validate`) with a throwaway
host program kept outside the repo. **No firmware bytes are committed.** `tests/c/test_cirrus_fw.c`
uses synthetic fixtures only. The copies under `adsp_pil/` are byte-identical (checked with `cmp`).

## WMFW (DSP images)

All four are Halo core (`core = 4`), file format `ver = 3`, `rev = 0x0300`, header_len 40, and all of
them parse with **no error under the strict policy** (no trailing bytes, aligned regions, exact
coefficient counts, no unknown region types).

| File | Bytes | Regions | Memory | Algorithm (0xF2) | Text | Coefficients | Memory payload bytes |
|---|---:|---:|---:|---:|---:|---:|---:|
| `cs35l43-bot-dsp1-spk-prot.wmfw` | 52,456 | 85 | 56 | 28 | 1 | 368 | 22,768 |
| `cs35l43-rcv-dsp1-spk-prot.wmfw` | 52,456 | 85 | 56 | 28 | 1 | 368 | 22,768 |
| `cs40l26.wmfw` | 56,712 | 42 | 26 | 15 | 1 | 532 | 20,932 |
| `cs40l26-calib.wmfw` | 44,732 | 40 | 26 | 13 | 1 | 399 | 18,628 |

## WMDR (coefficient / tuning `.bin`)

| File | Bytes | `ver` | `core_ver` | Blocks (data / text) | Data bytes |
|---|---:|---|---|---|---:|
| `cs35l43-bot-dsp1-spk-prot.bin` | 14,104 | `0x01071102` | `0x04030000` | 336 (333 / 3) | 5,324 |
| `cs35l43-rcv-dsp1-spk-prot.bin` | 14,108 | `0x01071102` | `0x04030000` | 336 (333 / 3) | 5,324 |
| `cs40l26.bin` | 12,380 | `0x0207021F` | `0x02000500` | 7 (4 / 3) | 12,128 |
| `cs40l26-wt1.bin` / `-wt2` / `-wt3` | 12,388 / 12,380 / 12,380 | `0x0207021F` | `0x02000500` | 7 (4 / 3) | 12,128 |
| `cs40l26-svc.bin` / `-svc1` / `-svc2` / `-svc3` | 1,312 / 1,308 / 1,312 / 1,312 | `0x0107021F` | `0x04030000` | 34 (33 / 1) | 532 |
| `cs40l26-a2h.bin` | 712 | `0x0107021F` | `0x04030000` | 10 (9 / 1) | 380 |
| `cs40l26-dvl.bin` | 184 | `0x0107021F` | `0x04030000` | 2 (1 / 1) | 48 |
| `cs40l26-calib.bin` | 188 | `0x01010117` | `0x04030000` | 3 (2 / 1) | 16 |

`cs40l26.bin` and the `-wt*` files carry the haptics wavetable (largest data blocks). They use a
different `core_ver` (`0x02000500`) from the tuning files.

## Not Cirrus containers

`SoundBoosterParam.bin` (21,000 B, starts `00 00 00 00`) and `dax_param.bin` (33,820 B, starts
`09 00 00 00`) are Samsung/Dolby parameter blobs, not WMFW/WMDR. The parser correctly rejects them
with *bad magic*.

## What this means for a driver

- The parser accepts every stock file, so the stricter-than-`cs_dsp` policy costs nothing on this device.
- Parsing is **all** this header does. Loading the image into the CS40L26/CS35L43 DSP needs the
  regmap/I2C path, the boot handshake and the `HALO_STATE` checks. None of that is implemented, and none
  of it may run before a Mode=0 driver has logged the amplifier's ID registers read-only.
