# Legacy bring-up scripts

One-off scripts from the early bring-up phase, moved here from the repository root. They are **not** part of the current build or deploy flow and are kept for reference only.

Most of them contain hard-coded paths from the original development machine and some of them write to the phone's partitions. **Read a script completely before running it**, and follow the safety rules in the top-level [`README.md`](../../README.md).

| Script | Purpose |
|---|---|
| `init_mu.sh`, `build_dm1q.sh`, `compile_dm1q*.sh` | Early Project Mu / Mu-Silicium UEFI (`dm1q`) setup and build variants (memory-map and UFS DSDT fixes). |
| `build_bcd_hivex.sh` | Offline BCD store generation with `hivex` (WSL). |
| `check_gpt_gaps.py`, `test_gdisk.py`, `test_gdisk_4k.sh` | GPT analysis of the UFS LUN backups (entry-array gaps, last partition, 4K-sector `gdisk` test on a loop image). |
| `probe_s23.sh` | Read-only hardware probe for a bare-metal Alpine Linux boot. |
| `dump_s23_mem.py` | Prints `memory` / `reserved-memory` from the live device tree (`dtc`). |
| `repair_and_deploy_s23.py` | Early recovery-side repair + deploy helper (`adb`, `ntfsfix`, `wimlib`). |
| `deploy_aclfix.py`, `deploy_aclfix_drv.py`, `deploy_aclsvc.py` | Recovery-side (`adb`) deploy helpers for the `s23aclfix` / `s23aclsvc` ACL recovery tools. |
| `deploy_s23pcieprobe.py` | Recovery-side deploy helper for `drivers/s23pcieprobe` (see [`docs/archive/PCIE0_PROBE_AUDIT.md`](../../docs/archive/PCIE0_PROBE_AUDIT.md)). |
| `fix_pc_winusb.ps1` | PC-side fix: rebinds the phone's WinUSB interface GUID for the USB link (run elevated). |
| `check_os.bat` | Prints the Windows OS name/version. |
