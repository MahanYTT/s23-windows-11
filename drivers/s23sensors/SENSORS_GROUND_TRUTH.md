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

Nothing here talks to hardware. ADSP PIL is not up on Windows, so there is no on-device test yet.
