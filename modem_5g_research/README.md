# 5G modem (Snapdragon X70 / MPSS) — index and master checklist

Single entry point for everything modem-related. The former `modem_research/` folder was merged into this folder (files moved unchanged with `git mv`, only path references updated), so there is one place for modem notes and ACPI drafts.

> [!CAUTION]
> **Status: research only — nothing here is enabled on the device.** Both SSDTs are inert (`_STA = 0`). Read the EFS/IMEI safety protocol in the blueprint (section *Non-Negotiable Hardware Safety Protocol*) before any on-device step: back up `modemst1`, `modemst2`, `fsg`, `fsc`, `efs`, `sec_efs`, `persist` first.

## Files

| File | Origin | Content |
|---|---|---|
| [`FIRMWARE_GROUND_TRUTH.md`](FIRMWARE_GROUND_TRUTH.md) | from the firmware dump | Measured ELF load spans, segment-hash integrity, IPA/GSI v5.5 evidence, `modem_dtb` content, carve-out confirmation |
| [`S23_5G_MODEM_AUTONOMOUS_BLUEPRINT.md`](S23_5G_MODEM_AUTONOMOUS_BLUEPRINT.md) | newest | Scope tiers (registration / packet data / voice), target architecture, SM8550 hardware spec, EFS safety protocol, agent harness, Day-0 checklist |
| [`S23_5G_MODEM_AND_PIL_BRINGUP.md`](S23_5G_MODEM_AND_PIL_BRINGUP.md) | `modem_5g_research/` | Remoteproc table (MPSS/ADSP/CDSP/SPSS), DDR carve-outs vs `MemoryMapLib.c`, PAS SMC calling convention, IPCC/SMEM/SMP2P/GLINK, IPA + QMI data path, extracted firmware inventory |
| [`S23_5G_MODEM_BRINGUP.md`](S23_5G_MODEM_BRINGUP.md) | moved from `modem_research/` | X70 hardware map, MBB dependency chain, 5 hard blockers, phased bring-up order, quick wins |
| [`KAILUA_SUBSYSTEMS_INVENTORY.md`](KAILUA_SUBSYSTEMS_INVENTORY.md) | moved from `modem_research/` | Audit of the 21 Kailua (SM8550) Windows BSP packages, `_HID` namespace shift, subsystem gap matrix |
| [`s23_subsys_pil.asl`](s23_subsys_pil.asl) | `modem_5g_research/` | SSDT `S23PIL00`: `SMEM`, `IPCC`, `MSS0`, `ADSP`, `CDSP`, `SPSS`, `IPA0` (master gate `S23P = 0`) |
| [`s23_modem_subsystems.asl`](s23_modem_subsystems.asl) | moved from `modem_research/` | SSDT `S23MDMS0`: `ABD`, `SCM0`, `RPEN`, `IPCC`, `GLNK`, `IPC0`, `PILC`, `TFTP`, `SOCP`, `AMSS`, `IPA` (all `_STA = 0`) |
| [`pull_s23_5g_and_subsys_fw.py`](pull_s23_5g_and_subsys_fw.py) | `modem_5g_research/` | Read-only firmware extraction from Lineage Recovery into gitignored folders |

Related elsewhere: [`drivers/s23pilprobe/`](../drivers/s23pilprobe/) (read-only SMEM/IMEM/PIL/CMD_DB telemetry), [`drivers/s23perf/`](../drivers/s23perf/), historical reports in [`docs/archive/`](../docs/archive/) and `gpu_research/NEON_GPU_MODEM_REPORT.md` (that report still names the old `modem_research\` paths; it is kept unchanged as a historical record).

## Facts both bring-up documents agree on

| Item | Value | Sources |
|---|---|---|
| MPSS remoteproc | `0x04080000` (`0x10000`), `qcom,kalama-modem-pas`, PAS ID `4`, WDOG GIC SPI `0x108` → ACPI GSI `296` | both bring-up docs |
| MPSS DDR load region | `mpss_region@89800000`, actual `reg` start `0x8A800000`, size `0x10800000` (264 MiB) | both bring-up docs |
| SMEM | `0x81D00000`, 2 MiB | both bring-up docs, `s23pilprobe` |
| IPCC | `0x00408000` (`0x1000`), `_HID QCOM06C2` | both docs, both SSDTs |
| IPA / GSI MMIO | IPA `0x03E00000` (`0x84000`), GSI `0x03E04000` (`0xFC000`); IRQs GSI `686` (ipa) and `464` (gsi) | both docs, both SSDTs |
| IPA firmware | `ipa_fws`, PAS ID `0x0F`; `apps_smmu` SID `0x4A0` | both bring-up docs |

## Contradictions — status after the firmware dump

Measured against the stock images in [`FIRMWARE_GROUND_TRUTH.md`](FIRMWARE_GROUND_TRUTH.md) (dump kept outside the repo, reproducible with `scripts/verify_firmware_dump.py`).

| # | Topic | Status | Result |
|---|---|---|---|
| 1 | **IPA / GSI version** (docs: v5.0 from `qcom,ipa-hw-ver = 0x18`; blueprint: v5.5) | ✅ **Resolved: v5.5** | The shipped GSI firmware is `fw_ipa_gsi_5.5_w.elf` (byte-identical segments to `ipa_fws.mdt/.bNN`) and the MPSS image carries the IPA component tag `ipa.dataipa.mpss.5.5.1`. The "0x18 = 5.0" decoding is not supported; decode `0x18` (= 24) against the matching downstream `enum ipa_hw_type` before quoting it again. `S23_5G_MODEM_BRINGUP.md` / `S23_5G_MODEM_AND_PIL_BRINGUP.md` still say v5.0 in their text — read them with this correction. |
| 2 | **Two SSDTs describe the same devices** (`\_SB.IPCC` in both, `MSS0`/`AMSS` and `IPA0`/`IPA` with the same MMIO) | ⚠️ **Open (by design)** | Not a firmware question. **Never load both**; pick one SSDT per experiment. |
| 3 | **IPA/GSI interrupt trigger** (Level in `s23_subsys_pil.asl`, Edge in `s23_modem_subsystems.asl`) | ⚠️ **Open** | The modem DTB in the dump is the Q6-side `qcom,kailua-mtp` tree without interrupt data; needs the AP-side live DTS. |
| 4 | **Stale paths in the blueprint** | ✅ **Resolved** | All root paths (`HANDOFF.md`, `compile_dm1q_*.sh`, `build_bcd_hivex.sh`, `WIFI_BRINGUP_FIXES.md`, …) now point to `docs/archive/`, `scripts/legacy/`, `wifi_research/`. |
| 5 | **Carve-outs** (MPSS `0x8A800000`/264 MiB, MPSS DTB `0x9B000000`, CDSP `0x9C900000`/32 MiB, CDSP/ADSP DTB `0x9E900000`/`0x9E980000`) | ✅ **Confirmed** | The image load spans match exactly. The ADSP image really spans `0x9EA00000..0xA43B4000`, i.e. past `PIL Reserved` — the critical finding in `S23_5G_MODEM_AND_PIL_BRINGUP.md` is real. |

## Master checklist

Combined from the phase plan in `S23_5G_MODEM_BRINGUP.md` (section 3) and the blueprint (tiers, Day-0 checklist). The details live in those sections; nothing here is new.

- [ ] **Gate 0 — backups (human, one time):** full partition backup incl. `modemst1/2`, `fsg`, `fsc`, `efs`, `sec_efs`, `persist` (blueprint, Day-0 step 1).
- [x] **Firmware ground truth:** stock images pulled and analysed ([`FIRMWARE_GROUND_TRUTH.md`](FIRMWARE_GROUND_TRUTH.md)); check a local dump with `scripts/verify_firmware_dump.py`.
- [ ] **Live DTS:** re-dump the AP-side live DTB (IPA interrupt trigger, `qcom,ipa-hw-ver` decoding).
- [ ] **Settle the remaining open points** (SSDT choice, IPA IRQ trigger).
- [ ] **Phase 0:** reserve `mpss_dsm_region` (`0xD4D00000`, 51 MiB) in `MemoryMapLib.c`; verify Windows still boots.
- [ ] **Phase 1:** `qcscm` (`QCOM05DD`) + `qcabd` (`QCOM0427`), read-only SCM query first.
- [ ] **Phase 2:** RPMh/clock votes (`cx.lvl`, `mss.lvl`, `xo`) via the verified `APPS_RSC DRV2 TCS0` path.
- [ ] **Phase 3:** IPCC + GLINK-SMEM + QRTR, mapped read-only first (`s23pilprobe` already reads SMEM).
- [ ] **Phase 4 / Tier 1:** PIL + MPSS (PAS ID 4) with RAM-only RFS (never write `modemst*`/`fsg`); SIM detection and 5G/LTE registration over QMI.
- [ ] **Phase 5 / Tier 2:** IPA + GSI + QMI data path, then `NetAdapterCx`, later `MBBCx`.
- [ ] **Tier 3 (voice, IMS, emergency):** out of scope for unattended work (blueprint).
