/*
 * s23perf_gov.h - Pure-C11 EPSS DVFS & TSENS thermal governor logic
 * Shared between drivers/s23perf/s23perf.c and tests/c/test_perf_gov.c.
 */
#ifndef S23PERF_GOV_H
#define S23PERF_GOV_H

#include <stdint.h>
#include "s23perf_ioctl.h"

#define S23PERF_HYST_DC 30 /* 3.0 C hysteresis when stepping down thermal state */

static inline void S23PerfDecodeDomainState(
    uint32_t domain_state_reg,
    uint32_t xo_khz,
    uint32_t *hw_khz_out,
    uint32_t *granted_idx_out,
    uint32_t *granted_mv_out)
{
    uint32_t lval = domain_state_reg & 0xFFU;
    uint32_t idx  = (domain_state_reg >> 8) & 0xFFU;
    uint32_t mv   = (domain_state_reg >> 20) & 0xFFFU;
    if (hw_khz_out)      *hw_khz_out      = lval * xo_khz;
    if (granted_idx_out) *granted_idx_out = idx;
    if (granted_mv_out)  *granted_mv_out  = mv;
}

static inline uint32_t S23PerfComputeLoadPct(
    uint64_t prev_idle,
    uint64_t prev_total,
    uint64_t cur_idle,
    uint64_t cur_total)
{
    if (cur_total <= prev_total) {
        return 0U;
    }
    uint64_t dt = cur_total - prev_total;
    uint64_t di = (cur_idle >= prev_idle) ? (cur_idle - prev_idle) : 0ULL;
    if (di >= dt) {
        return 0U;
    }
    uint64_t busy = dt - di;
    uint64_t pct = (busy * 100ULL + (dt / 2ULL)) / dt;
    if (pct > 100ULL) pct = 100ULL;
    return (uint32_t)pct;
}

static inline uint32_t S23PerfComputeThermalState(
    int32_t max_dC,
    uint32_t valid_cnt,
    int32_t soft_dC,
    int32_t hard_dC,
    int32_t crit_dC,
    uint32_t prev_state)
{
    if (valid_cnt == 0U) {
        return S23PERF_THERMAL_FAILSAFE;
    }
    if (max_dC >= crit_dC) {
        return S23PERF_THERMAL_CRITICAL;
    }
    if (max_dC >= hard_dC) {
        if (prev_state == S23PERF_THERMAL_CRITICAL && max_dC > (crit_dC - S23PERF_HYST_DC)) {
            return S23PERF_THERMAL_CRITICAL;
        }
        return S23PERF_THERMAL_HARD;
    }
    if (max_dC >= soft_dC) {
        if (prev_state == S23PERF_THERMAL_CRITICAL && max_dC > (crit_dC - S23PERF_HYST_DC)) {
            return S23PERF_THERMAL_CRITICAL;
        }
        if (prev_state >= S23PERF_THERMAL_HARD && max_dC > (hard_dC - S23PERF_HYST_DC)) {
            return S23PERF_THERMAL_HARD;
        }
        return S23PERF_THERMAL_SOFT;
    }
    /* Below soft_dC: check hysteresis down from CRITICAL / HARD / SOFT */
    if (prev_state == S23PERF_THERMAL_CRITICAL && max_dC > (crit_dC - S23PERF_HYST_DC)) {
        return S23PERF_THERMAL_CRITICAL;
    }
    if (prev_state >= S23PERF_THERMAL_HARD && max_dC > (hard_dC - S23PERF_HYST_DC)) {
        return S23PERF_THERMAL_HARD;
    }
    if (prev_state >= S23PERF_THERMAL_SOFT && max_dC > (soft_dC - S23PERF_HYST_DC)) {
        return S23PERF_THERMAL_SOFT;
    }
    return S23PERF_THERMAL_NORMAL;
}

static inline uint32_t S23PerfSelectTargetIdx(
    uint32_t lut_count,
    uint32_t last_non_boost_idx,
    uint32_t load_pct,
    uint32_t profile,
    uint32_t manual_idx,
    int32_t max_dC,
    uint32_t thermal_state,
    int32_t soft_dC,
    int32_t hard_dC,
    uint32_t prev_idx)
{
    if (lut_count == 0U) {
        return 0U;
    }
    uint32_t max_idx = lut_count - 1U;
    uint32_t nb_max  = (last_non_boost_idx <= max_idx) ? last_non_boost_idx : max_idx;
    if (load_pct > 100U) load_pct = 100U;

    /* 1. Compute thermal ceiling (applies unconditionally to all profiles, including FIXED) */
    if (thermal_state == S23PERF_THERMAL_CRITICAL || thermal_state == S23PERF_THERMAL_FAILSAFE) {
        return 0U;
    }

    uint32_t hard_cap = (nb_max * 40U) / 100U;
    uint32_t therm_cap = max_idx;
    if (thermal_state == S23PERF_THERMAL_HARD) {
        therm_cap = hard_cap;
    } else if (thermal_state == S23PERF_THERMAL_SOFT) {
        if (hard_dC <= soft_dC || max_dC >= hard_dC) {
            therm_cap = hard_cap;
        } else if (max_dC <= soft_dC) {
            therm_cap = nb_max;
        } else {
            uint32_t span_dC = (uint32_t)(hard_dC - soft_dC);
            uint32_t over_dC = (uint32_t)(max_dC - soft_dC);
            uint32_t drop = ((nb_max - hard_cap) * over_dC) / span_dC;
            therm_cap = (nb_max > drop) ? (nb_max - drop) : hard_cap;
        }
    }

    /* 2. Desired LUT index by profile */
    uint32_t want = 0U;
    if (profile == S23PERF_PROFILE_FIXED) {
        want = (manual_idx <= max_idx) ? manual_idx : max_idx;
    } else if (profile == S23PERF_PROFILE_POWERSAVE) {
        uint32_t ps_cap = nb_max / 2U;
        if (load_pct <= 20U || ps_cap == 0U) {
            want = 0U;
        } else {
            want = ((load_pct - 20U) * ps_cap + 40U) / 80U;
            if (want > ps_cap) want = ps_cap;
        }
    } else if (profile == S23PERF_PROFILE_PERFORMANCE) {
        uint32_t perf_floor = nb_max / 2U;
        if (load_pct >= 60U && thermal_state == S23PERF_THERMAL_NORMAL) {
            want = max_idx;
        } else {
            uint32_t span = (nb_max > perf_floor) ? (nb_max - perf_floor) : 0U;
            want = perf_floor + (load_pct * span + 30U) / 60U;
            if (want > nb_max) want = nb_max;
        }
    } else {
        /* BALANCED (default) */
        if (load_pct <= 12U) {
            want = 0U;
        } else if (load_pct > 85U && thermal_state == S23PERF_THERMAL_NORMAL) {
            want = max_idx;
        } else {
            uint32_t clamped_load = (load_pct > 85U) ? 85U : load_pct;
            want = ((clamped_load - 12U) * nb_max + 36U) / 73U;
            if (want > nb_max) want = nb_max;
        }
    }

    /* 3. Smooth step-down hysteresis for dynamic profiles (ramp up immediately; step down <= 2) */
    if (profile != S23PERF_PROFILE_FIXED) {
        uint32_t p = (prev_idx <= max_idx) ? prev_idx : max_idx;
        if (want < p && (p - want) > 2U) {
            want = p - 2U;
        }
    }

    /* 4. Apply thermal cap after rate-limiting so thermal throttling is always immediate */
    if (want > therm_cap) {
        want = therm_cap;
    }
    if (want > max_idx) {
        want = max_idx;
    }
    return want;
}

#endif /* S23PERF_GOV_H */
