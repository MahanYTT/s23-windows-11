# s23urs — migration plan for USB PHY programming (not started; groundwork only)

`usb_research/S23_USBC_ARCHITECTURE.md` (lines 18, 25) says USB PHY programming must leave
`s23touch.sys` and move to a dedicated UrsCx client (`s23urs.sys`). Otherwise dynamic USB-C
hot-plug resets and DP mode switches on the QMP combo PHY (`0x088E8000`) collide with the touch
driver. This change only adds the hardware-free validator; **no driver code moves yet**.

## Address sources used by `s23urs_seq.h`

| Block | Physical | Size | Source |
|---|---|---|---|
| QMP combo PHY | `0x088E8000` | `0x3000` mapped | ARCH.md:10,26,158; `s23touch.c` maps it |
| ↳ `.usb3_serdes` … PCS | `+0x1000` | `0x1000` | ARCH.md:158 `qmp_combo_offsets_v3`; `s23touch.c` PCS writes at `+0x1C00/+0x1C40/+0x1C44` |
| eUSB2 PHY | `0x088E3000` | `0x1000` | ARCH.md:114,136; `s23touch.c` |
| DWC3 (URS0) | `0x0A600000` | `0x100000` | ARCH.md:14,124,292 |
| QSCRATCH | `0x0A6F8800` | — | ARCH.md:118,124 |
| TLMM | `0x0F100000` | stride `0x1000` | `s23batt.c` |
| TZ QUPs (never touched) | QUP1_SE2 `0x00A88000`, QUP2_SE0 `0x00880000`, QUP2_SE2 `0x00888000` | `0x4000` each | `s23sensprobe.c` TZ guard |

## What the validator proves today

`tests/c/test_s23urs_seq.c` runs `s23urs_seq_validate_u16` over the **real** `gS23QmpSeq` table
that `s23touch.c` programs today (`drivers/s23touch/s23_qmp_seq.h`, unmodified). Every entry is
4-byte aligned, holds an 8-bit value, lies in `.usb3_serdes`..PCS (`+0x1000..+0x1FFF`), overlaps
no TZ / eUSB2 / TLMM range and never rewrites an offset with a different value.

## Migration steps (each its own PR, each reversible)

1. **Mode=0** `s23urs.sys`: loads and logs to `C:\Users\Public\s23urs.log` only, with no MMIO. It validates
   `gS23QmpSeq` with this header at load and logs the report.
2. **Mode=1**: read-only snapshot of the QMP PCS status and the DWC3 `GCTL`/`GSTS`, compared with what
   `s23touch.c` leaves behind. Still no writes.
3. **Mode=2** (opt-in registry value, default stays 0): `s23urs.sys` programs the table, and `s23touch.sys`
   gets a registry switch to skip its own QMP init. The default remains the current behaviour until a
   cold-boot, hot-plug and suspend matrix is logged on the device.
4. Only then remove the QMP code from `s23touch.c`.

Rollback at every step: set Mode=0 (or remove the s23urs service). `s23touch.sys` keeps its QMP
init until step 4.
