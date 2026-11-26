# Sensors ground truth — S23 (`dm1q`)

## Where the sensors are

From `audio_sensors_research/S23_AUDIO_HAPTICS_SENSORS_MAP.md` (live DTS): the IMU, magnetometer,
barometer and front ALS/proximity sit on the **LPASS SSC (Qualcomm Sensing Hub / SEE) inside the
ADSP** and have no AP device-tree nodes. Windows can only reach them through ADSP PIL + GLink/QRTR
+ the SEE client protocol. Only rear ALS, grip/SAR and the Hall GPIO107 are AP-visible.

## What the stock ADSP image says (read-only string scan)

Printable-string scan of `03_audio_adsp_haptics/adsp_pil/adsp.b*` in the firmware dump (outside
the repo; nothing committed). The numbers below are distinct strings that contain the name:

| Name | Distinct strings | Notes |
|---|---:|---|
| `lsm6dso` | 504 | IMU driver; registry configs `lsm6dso_0.{accel,gyro,md,temp}.config` |
| `ak0991x` | 346 | AKM magnetometer driver family (`ak09918` and `ak09917` named once each) |
| `lps22hx` / `lps22hh` | 133 / 15 | ST barometer driver family. **The image names LPS22HH, not the LPS22DF the map doc lists.** Treat the exact part as unverified. |
| `stk3a9x` / `stk33911` | 83 / 5 | Sensortek ALS/proximity driver family |

The SEE client API is protobuf. The image contains 51 distinct `sns_*.proto` names (e.g. `sns_std_request`,
`sns_suid`, `sns_timer.proto`) and nanopb `pb_decode`/`pb_encode` symbols. `04_.../dsp_image/adsp/`
also ships the FastRPC skeletons `libsns_device_mode_skel.so`, `libsns_direct_channel_skel.so`,
`libsns_dynamic_loader_skel.so`, `libsns_remote_proc_state_skel.so` and `sns_tppe.so`.

## What `s23sns_pb.h` is (and is not)

`s23sns_pb.h` implements only the **public protobuf wire format** (varint, ZigZag, I32/I64, LEN,
nested messages), bounds-checked and transactional, tested under ASan + UBSan by
`tests/c/test_s23sns_pb.c`. It contains **no** SEE message layouts, field numbers or SUIDs. Those
need the `.proto` definitions, which are not in the dump as text. Deriving them is future work and
must be checked against a live ADSP before anything depends on them.

## `s23sensors.c` — Freestanding ARM64 WDM Sensors Driver (`s23sensors.sys`)

`drivers/s23sensors/s23sensors.c` (built via `drivers/s23sensors/build_wsl.py` and signed via
`drivers/s23sensors/sign.ps1`) pairs the zero-allocation `s23sns_pb.h` wire codec with the
AP-direct sensors on S23 (`dm1q`). It reads `Mode` (`REG_DWORD`, default `0`) from
`HKLM\SYSTEM\CurrentControlSet\Services\s23sensors\Parameters` and logs to
`\??\C:\Users\Public\s23sensors.log`:

- **`Mode = 0` (Default — Safe One-Shot Self-Test & Hall GPIO107 Read):**
  1. Encodes Qualcomm SEE `sns_client_request_msg` frames targeting the well-known SUID Lookup
     sensor (`suid_low = 0xABABABABABABABAB`, `suid_high = 0xABABABABABABABAB`,
     `msg_id = 512` / `SNS_SUID_MSGID_SNS_SUID_REQ`) for all six QSH sensor data types
     (`"accel"`, `"gyro"`, `"mag"`, `"pressure"`, `"ambient_light"`, `"proximity"`) using
     `s23sns_pb.h` (`s23pb_writer_init`, `s23pb_submsg_begin`/`s23pb_submsg_end`,
     `s23pb_write_fixed64`, `s23pb_write_varint`, `s23pb_write_str`).
  2. Runs a self-test of the QSH `sns_std_sensor_event` decoder (`s23pb_reader_init`,
     `s23pb_read_tag`, `s23pb_submsg_enter`, `s23pb_packed_fixed32_count`, `s23pb_f32_from_bits`)
     on a 3-axis float sample (`x`, `y`, `z`) + `status`.
  3. Maps `TLMM` (`0x0F100000`) and reads the AP-direct magnetic Hall flip-cover sensor on
     `GPIO107` (`+0x6B000`: `CFG +0x00`, `IN_OUT +0x04`), logging the protobuf wire hex and
     Hall state to `C:\Users\Public\s23sensors.log` before unloading cleanly.
- **`Mode = 1` (Opt-In AP I2C Sensor Probe + `\Device\S23Sensors` IOCTL Interface):**
  1. Performs all `Mode = 0` steps, then probes the two AP-visible I2C sensors:
     - **Rear ALS / Flicker (`ams TSL2585` @ `0x39`)** on `QUPv3_I2C_HUB SE0` (`0x00980000`,
       `GPIO16` SDA / `GPIO17` SCL, IRQ `GPIO62`), reading `AUXID` (`0x90`), `REVID` (`0x91`),
       `ID` (`0x92`), and `STATUS` (`0x93`).
     - **Grip / SAR Proximity (`Imagis ISG6320` @ `0x28`)** on bit-bang `GPIO169` (SDA) /
       `GPIO170` (SCL) (IRQ `GPIO153`), reading `WHOAMI` (`0x00`) and `STATUS` (`0x01`).
  2. Exposes `\Device\S23Sensors` (`\DosDevices\S23Sensors`) supporting
     `IOCTL_S23SENSORS_GET_TELEMETRY` and `IOCTL_S23SENSORS_POLL_SENSORS` for querying live
     Hall/ALS/SAR/QSH telemetry.
