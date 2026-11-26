# s23keys — what the host test locks, and the rules for a future sleep/wake change

`tests/c/test_keys_report.c` extracts the live code of `s23keys.c` (via `tests/c/extract_keys.py`,
without modifying the driver) and locks the current, working behaviour:

| Locked behaviour | Source in `s23keys.c` |
|---|---|
| HID descriptor bytes = the v1.0.0.1 golden copy: Report ID 1 Consumer Control (bit0 Volume Increment `0xE9`, bit1 Volume Decrement `0xEA`), Report ID 2 System Control (bit0 System Power Down `0x81`) | `gHidReportDesc` |
| PON `RT_STS` (`0x10`) bit7 = Power (active-high), bit6 = RESIN = VolDown (active-high); PM8550 GPIO6 `RT_STS` bit0 = VolUp (active-low); a failed SPMI read keeps the last stable state | `S23KeysReadRawButtons` |
| `DEBOUNCE_TICKS` = 2 polls (2 × 10 ms); volume edges only emit Report ID 1, power edges only Report ID 2; shorter glitches emit nothing | worker-thread debounce block |

PMIC access is read-only: register reads are issued as commands on the SPMI arbiter **Observer** channel (`0x0C440000`). That
stays unchanged.

## Rules for a sleep/wake (Modern Standby) change

1. Keep `tests/c/test_keys_report.c` green without editing its golden descriptor. A new report or
   usage needs a new Report ID, not a change to IDs 1/2.
2. Do not switch to PON interrupts or write any PON/PMIC register in the same change. The poller stays
   the fallback, and any interrupt path ships behind a registry opt-in, default off.
3. A long-press / power-off policy belongs to Windows (System Power Down → shell), not to the
   driver.
