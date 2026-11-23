# Archived bring-up reports

Historical reports and handoff notes from the early S23 bring-up. They are kept for reference only; paths and versions inside them reflect the state at the time they were written and may be outdated. The current status is in the top-level [`README.md`](../../README.md).

| File | Content |
|---|---|
| [`HANDOFF.md`](HANDOFF.md) | Agent handoff notes from the initial bring-up (USB link, partition layout, next steps). |
| [`NEON_OVERNIGHT_REPORT.md`](NEON_OVERNIGHT_REPORT.md) | Overnight pass: display auto-refresh patch, `s23pcieprobe` audit, deploy script fixes. |
| [`NEON_OFFLINE_PASS3_REPORT.md`](NEON_OFFLINE_PASS3_REPORT.md) | Offline pass 3 report. |
| [`NEON_OFFLINE_PASS4_REPORT.md`](NEON_OFFLINE_PASS4_REPORT.md) | Offline pass 4 report. |
| [`NEON_CHAT2_5G_AUDIO_REPORT.md`](NEON_CHAT2_5G_AUDIO_REPORT.md) | Parallel worker report on 5G modem / PIL and audio probing. |
| [`PCIE0_PROBE_AUDIT.md`](PCIE0_PROBE_AUDIT.md) | Line-by-line audit of `s23pcieprobe.c` v1.0.0.9 → v1.0.0.10 (the deploy script now lives in [`scripts/legacy/`](../../scripts/legacy/)). |

Related write-ups that belong to a specific subsystem were moved next to their sources instead:

- [`display_research/DISPLAY_BLACK_SCREEN_FIX.md`](../../display_research/DISPLAY_BLACK_SCREEN_FIX.md) + `s23_display_autorefresh_fix.patch`
- [`wifi_research/WIFI_BRINGUP_FIXES.md`](../../wifi_research/WIFI_BRINGUP_FIXES.md) + `s23_wifi_pcie_fix.c`
