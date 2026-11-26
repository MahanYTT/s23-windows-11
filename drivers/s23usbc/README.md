# s23usbc — pure-C11 protocol headers (no driver yet)

These headers are the hardware-free half of the future `s23usbc.sys` UcmCx client described in
`usb_research/S23_USBC_ARCHITECTURE.md`. They contain **no** register access, no I2C, no KMDF
calls and no allocation; every function is `static inline`, takes caller-owned buffers and is
covered by ASan/UBSan host tests under `tests/c/`.

| Header | Scope | Host test |
|---|---|---|
| `s23usbc_opcode.h` | MAX77705 OPCODE mailbox framing (`0x21..0x41` write, `0x51..0x71` read, echo check), interrupt block `0x02..0x05`, status-register field decode | `test_s23usbc_opcode.c` (parity with `usb_research/max77705_mailbox.py` via generated `s23usbc_vectors.h`) |
| `s23usbc_pd.h` | USB PD 3.0 message header, Fixed/Battery/Variable/PPS PDO and RDO codec, 15–18 W Fixed-PDO sink policy, PPS request clamping, OTG boost guard, DR/PR swap request write plans and readiness | `test_s23usbc_pd.c` |
| `s23usbc_dp.h` | VESA DP Alt Mode structured VDMs: header codec, object-position rules, Discover Identity/SVIDs/Modes, C/D/E pin policy, DP Status/Attention/HPD edges, DP Configure | `test_s23usbc_dp.c` |

Regenerate the parity vectors after changing the Python reference:

```
python usb_research/gen_s23usbc_c_vectors.py          # rewrite tests/c/s23usbc_vectors.h
python usb_research/gen_s23usbc_c_vectors.py --check  # CI / tests/test_s23usbc_vectors.py
```

## What is deliberately not here

* The exact byte offsets inside the MAX77705 VDM (`0x48`/`0x4B`/`0x55`) and PPS (`0x3A`/`0x3C`)
  OPCODE frames for the dm1q MCU firmware revision are **unverified** (architecture doc, open item 2).
  The PD/DP headers therefore model the *USB-IF / VESA wire objects* (PDO, RDO, VDM header, DP
  Status/Configure VDOs), not the MAX77705-specific wrapping around them.
* No PCA9481 direct-charger logic (NDA register map; 25 W PPS stays out of scope). The sink policy
  refuses anything above 18 W / 20 V / 5 A.
* No driver, INF or registry change. Nothing here changes current runtime behaviour.
