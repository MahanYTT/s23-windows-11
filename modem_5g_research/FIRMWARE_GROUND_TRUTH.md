# Firmware ground truth — S23 (`dm1q`) modem, IPA and DSP images

Facts measured from the stock firmware dump (`s23_dm1q_firmware_dump/04_modem_5g_dsp_apnhlos/`, 827 files, 446,663,657 bytes, pulled read-only from the device). **No firmware is committed to this repository**; the dump stays outside the repo. Every number below can be reproduced with:

```powershell
python scripts/verify_firmware_dump.py <path-to>\s23_dm1q_firmware_dump --images-only
```

Method: ELF32 program headers of the split PIL images (`*.mdt` + `*.bNN`) and plain ELF/MBN files; Qualcomm segment type = `p_flags` bits 24..26 (`2` = hash segment, `7` = ELF header); "load span" = lowest `p_paddr` to highest `p_paddr + p_memsz` over the non-empty `PT_LOAD` segments. Integrity: the SHA-384 digest of every non-empty segment was found in the image's own hash segment (internal consistency, not a signature check).

## 1. Images

| Image (dump path under `04_modem_5g_dsp_apnhlos/`) | ELF | `e_machine` | Segments (empty) | Bytes in segments | Load span (`p_paddr`) | Segment hashes |
|---|---|---|---|---|---|---|
| `modem_image/modem.mdt` + `modem.b00..b31` | 32 | 164 (QDSP6) | 32 (3 `p_filesz = 0`) | 141,938,230 | **`0x8A800000`..`0x9B000000` = `0x10800000` (264 MiB)**, entry `0x8A800000` | 28/28 SHA-384 |
| `modem_image/modem_dtb.mdt` + `.b00..b02` | 32 | 1 | 3 | 4,556 | **`0x9B000000`..`0x9B000200`** (512 B) | 2/2 SHA-384 |
| `modem_image/ipa_fws.mdt` + `.b00..b04` | 32 | 164 | 5 | 23,788 | `0x0`..`0x8568` (relocatable, 34,152 B) | 4/4 SHA-384 |
| `modem_image/fw_ipa_gsi_5.5_w.elf` | 32 | 164 | 5 | 23,788 | `0x0`..`0x8568` | 4/4 SHA-384 |
| `apnhlos_image/ipa_fws.mdt` + `.b00..b04` | 32 | 164 | 5 | 23,788 | `0x0`..`0x8568` | 4/4 SHA-384 |
| `apnhlos_image/fw_ipa_gsi_5.5_w.elf` | 32 | 164 | 5 | 23,788 | `0x0`..`0x8568` | 4/4 SHA-384 |
| `apnhlos_image/cdsp.mdt` + `.b00..b15` | 32 | 164 | 16 (1 empty) | 7,015,718 | **`0x9C900000`..`0x9E900000` = `0x02000000` (32 MiB)**, entry `0x9C900000` | 14/14 SHA-384 |
| `apnhlos_image/cdsp_dtb.mdt` | 32 | 1 | 3 | 4,556 | **`0x9E900000`..`0x9E900200`** | 2/2 SHA-384 |
| `apnhlos_image/adsp_dtb.mdt` | 32 | 1 | 3 | 12,420 | **`0x9E980000`..`0x9E9820B8`** | 2/2 SHA-384 |
| `apnhlos_image/adsp.mdt` (header only, listed for the carve-out check) | 32 | 164 | 51 | — | **`0x9EA00000`..`0xA43B4000` = `0x059B4000`** | 46/46 SHA-384 |

Whole dump: 100 ELF/MBN images consistent (all segment hashes found); `apnhlos_image/prov.mdt` + `prov.b00..b08` are not ELF (every part starts with `10 e3 5b 1e 20 13 07 24` + length, i.e. an encrypted container) and are only covered by file hashes.

## 2. IPA / GSI firmware

- `ipa_fws.mdt` + `ipa_fws.b00..b04` and `fw_ipa_gsi_5.5_w.elf` in the same folder are **the same image**: all five segments are byte-identical (split vs. single-file packaging). So `ipa_fws` *is* the GSI firmware, not a separate "IPA uC" image.
- The copies in `modem_image/` and `apnhlos_image/` have **identical code segments (1..3)**; only the hash/signature segment (`b04`, file offset `0x8000`) differs → same firmware, two signed packages.
- Version evidence:
  - file name `fw_ipa_gsi_5.5_w.elf` (both partitions);
  - the MPSS image contains the IPA component tag **`ipa.dataipa.mpss.5.5.1.v01.16`** (found as a string in `modem.bNN`);
  - the firmware ELF itself contains no version string.
- Size: the loadable span is `0x8568` bytes (33.4 KiB) at `p_paddr 0` (relocated by PAS), which fits both the 64 KiB `ipa_fw_region@9b080000` and the 40 KiB `ipa_gsi_region@9b090000` from the DTS. Which of the two regions the loader uses is not recorded in the image.

## 3. `modem_dtb` content

`modem_dtb.b01` (the 512-byte segment loaded at `0x9B000000`) is a `CFGL` container (version 2) with one entry named `platform-mtp.dtb` holding a complete FDT (`0xd00dfeed`, totalsize 450, version 17):

```
/ { model = "qcom,kailua-mtp"; compatible = "qcom,kailua-mtp", "qcom,kailua", "qcom,mtp";
    #address-cells = <2>; #size-cells = <2>;
    soc { #address-cells = <1>; #size-cells = <1>; };
    sw { core { boot { config = <...>; }; }; };
    __symbols__ { soc = "/soc"; sw = "/sw"; }; };
```

It is the modem's own (Q6-side) platform tree. It has **no memory, carve-out, IPA or interrupt information**, so the AP-side questions below (IPA register version, interrupt triggers) cannot be answered from it.

## 4. Carve-outs: documents vs. images

| Region (DTS name in the docs) | Documented base / size | Image says | Result |
|---|---|---|---|
| `mpss_region@89800000` (`modem_mem`) | `0x8A800000` / `0x10800000` (264 MiB), end `0x9B000000` | `modem.mdt` loads exactly `0x8A800000..0x9B000000` | **Confirmed** — the image fills the region completely (no headroom) |
| `q6_mpss_dtb_region@9b000000` | `0x9B000000` / 512 KiB | `modem_dtb` loads 512 B at `0x9B000000` | **Confirmed** |
| `ipa_fw_region@9b080000` / `ipa_gsi_region@9b090000` | 64 KiB / 40 KiB | relocatable, 0x8568 B | Fits either; load target not determinable from the image |
| `cdsp_region@9c900000` | `0x9C900000` / 32 MiB | `cdsp.mdt` loads exactly `0x9C900000..0x9E900000` | **Confirmed** |
| `q6_cdsp_dtb_region@9e900000` | `0x9E900000` / 512 KiB | `cdsp_dtb` at `0x9E900000` | **Confirmed** |
| `q6_adsp_dtb_region@9e980000` | `0x9E980000` / 512 KiB | `adsp_dtb` at `0x9E980000` | **Confirmed** |
| `adspslpi_region@9ea00000` | `0x9EA00000` / `0x059B4000`, overflows `PIL Reserved` (ends `0xA2A80000`) | `adsp.mdt` loads exactly `0x9EA00000..0xA43B4000` | **Confirmed** — "critical finding #1" in `S23_5G_MODEM_AND_PIL_BRINGUP.md` is real: the ADSP image itself extends `0x1934000` bytes past `PIL Reserved` |
| `mpss_dsm_region@d4d00000` | `0xD4D00000` / 51 MiB | not part of any image (runtime DSM pool) | Not checkable from firmware |

The ASL drafts agree with this: `s23_subsys_pil.asl` uses `qcom,fw-base 0x8A800000` / `qcom,dtb-base 0x9B000000` (MPSS), `0x9C900000` / `0x9E900000` (CDSP), `0x9E980000` (ADSP DTB), `ipa-fw-base 0x9B080000` / `ipa-gsi-base 0x9B090000`; `s23_modem_subsystems.asl` documents MPSS `0x8A800000` (264 MiB) and DTB `0x9B000000`.

## 5. What the dump settles and what it does not

| Question (from `README.md`) | Answer |
|---|---|
| IPA / GSI version v5.0 or v5.5? | **v5.5** for the shipped firmware: GSI firmware `fw_ipa_gsi_5.5_w.elf` (= `ipa_fws`) and the MPSS IPA component `ipa.dataipa.mpss.5.5.1`. The docs' "`qcom,ipa-hw-ver = 0x18` = IPA 5.0" decoding is not supported by the dump; `0x18` = 24 decimal must be decoded against the `enum ipa_hw_type` of the exact downstream kernel that produced the live DTS before it is quoted again. |
| MPSS / `modem_dtb` / CDSP / ADSP carve-outs | Confirmed exactly (section 4). |
| Two SSDTs defining the same devices | Not a firmware question — still: load only one of them. |
| IPA interrupt trigger (Level vs Edge) | Not in the firmware; needs the AP-side live DTS. |
