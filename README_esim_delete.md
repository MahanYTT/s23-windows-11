# eSIM Profile Deletion Status

## The Problem
When trying to disable or delete an eSIM profile using the `s23modemd.exe` tool via raw ISO 7816-4 ES10c APDUs, the eUICC chip (NXP SN220) responds with APDU status word `SW=6985`. 
`6985` stands for **Conditions of use not satisfied**. 

## What We Discovered
1. **The profile is actually DISABLED.** 
   When we ran `--disable`, it initially returned `SW=9000` (Success). Later calls to `--disable` returned `6985` because the profile is *already* disabled. The output of `s23modemd.exe --diag-live` explicitly shows `State=0(DISABLED)` for the target profile (ICCID `8932002200013419981`).
2. **Deleting the profile still fails with `6985`.**
   Even after stopping all packet data (`WDS_STOP_NETWORK_INTERFACE`), switching the modem to `LOW_POWER` (airplane mode), unbinding the logical channel `GW_PRI`, and stopping Windows `WwanSvc`, the `DeleteProfile` and `EUICCMemoryReset` ES10c APDUs return `6985`.
3. **Reason for `6985` on Delete:**
   According to the GSMA SGP.22 specification, `SW=6985` on `DeleteProfile` happens when Profile Policy Rules (PPR) explicitly forbid deletion, or if the profile is mandated as a Fallback Profile, or if the eUICC requires explicit user intent via a signed SM-DP+ / SM-DS token that we are bypassing by using raw APDUs.

## Resolution
To correctly delete this eSIM profile so it can be provisioned on another device, we need to use the native Windows 11 LPA (Local Profile Assistant) UI (Settings -> Network & internet -> Cellular -> eSIM profiles).
Windows LPA handles the complex cryptographic handshakes with the SM-DP+ server if the profile policy requires it, and is fully supported now that we verified the `s23mbb` driver correctly surfaces the eSIM chip over MBIMEx 3.0 to `WwanSvc`.
