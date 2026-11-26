# s23usbc — USB-C PD 3.0 & DisplayPort Alt Mode driver (`s23usbc.sys`) and pure-C11 protocol headers

`drivers/s23usbc/` contains both the hardware-free pure-C11 protocol headers and the freestanding
ARM64 WDM kernel driver `s23usbc.c` (`s23usbc.sys`) described in
`usb_research/S23_USBC_ARCHITECTURE.md`.

## Protocol headers (hardware-free, host-tested with ASan/UBSan)

The headers contain **no** register access, no I2C, no KMDF calls and no allocation; every function
is `static inline`, takes caller-owned buffers and is covered by ASan/UBSan host tests under
`tests/c/`.

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

## Kernel driver (`s23usbc.c` -> `s23usbc.sys`)

`s23usbc.c` is a freestanding ARM64 WDM driver built via `python drivers/s23usbc/build_wsl.py` and
test-signed via `drivers/s23usbc/sign.ps1`. It logs to `\??\C:\Users\Public\s23usbc.log` and reads
`HKLM\SYSTEM\CurrentControlSet\Services\s23usbc\Parameters\Mode` (default `0`):

* **Mode 0 (default safe self-test, zero MMIO):** Runs an in-kernel verification of
  `S23UsbcOpcodeEncodeWrite` / `S23UsbcOpcodeDecodeResponse`, `S23PdSelectFixed` (18 W `9 V / 2 A`
  selection), `S23UsbcOtgEnableBlockers` / `S23UsbcOtgDisableReasons`, and
  `S23DpSelectPinAssignment` (`S23DpChoosePin`: Pin D 2-lane DP+USB3 vs Pin C 4-lane DP) +
  `S23DpClassifyHpd` (`S23DpHpdEvent`), logging `PASS` results to `s23usbc.log` with zero MMIO.
* **Mode 1 (read-only MAX77705 `0x25` status probe):** Maps TLMM (`0x0F100000`), reads MAX77705
  slave `0x25` registers `0x00..0x0D` over serialized SE8 open-drain eGPIO (`GPIO206` SDA /
  `GPIO207` SCL, preserving `EGPIO_ENABLE` `0x1000` when `EGPIO_PRESENT` `0x0800` is set), and
  decodes them with `S23UsbcDecodeIntBlock`, `S23UsbcDecodeUsbcStatus1`, `S23UsbcDecodeBcStatus`,
  `S23UsbcDecodeCcStatus0`, `S23UsbcDecodeCcStatus1`, and `S23UsbcDecodePdStatus1` into
  `s23usbc.log`.
* **Mode 2 (opt-in active policy & IOCTL device `\Device\S23Usbc` / `\DosDevices\S23Usbc`):**
  Exposes buffered IOCTLs (`IOCTL_S23USBC_GET_STATUS`, `IOCTL_S23USBC_SELECT_PDO`,
  `IOCTL_S23USBC_CHECK_OTG`, `IOCTL_S23USBC_EVAL_DP`) to query decoded CC/PD/BC/DP state, evaluate
  15–18 W Fixed PDO sink requests, and check OTG boost safety interlocks.

## Scope & safety boundaries

* The exact byte offsets inside the MAX77705 VDM (`0x48`/`0x4B`/`0x55`) and PPS (`0x3A`/`0x3C`)
  OPCODE frames for the dm1q MCU firmware revision remain **unverified** (architecture doc, open
  item 2). The PD/DP helpers therefore model the *USB-IF / VESA wire objects* (PDO, RDO, VDM header,
  DP Status/Configure VDOs), and Mode 1/2 only perform policy-checked status reads on `0x25`.
* No PCA9481 direct-charger logic (NDA register map; 25 W PPS stays out of scope). The sink policy
  refuses anything above 18 W / 20 V / 5 A.
