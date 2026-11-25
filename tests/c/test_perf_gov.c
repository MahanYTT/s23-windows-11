/*
 * Host unit tests for drivers/s23perf/s23perf_gov.h (compiled with ASan+UBSan).
 * Covers EPSS DOMAIN_STATE decoding, CPU load calculation, multi-stage TSENS thermal
 * state machine with 3.0 C hysteresis, and all 4 DVFS profiles + thermal caps.
 */
#include <stdint.h>
#include "host_test.h"
#include "s23perf_gov.h"

static void test_decode_domain_state(void)
{
    uint32_t hw = 0, idx = 0, mv = 0;

    /* Real S23 Silver snapshot: 0x2E800C57 -> L=0x57(87)=1670.4MHz, idx=12, 0x2E8=744mV */
    S23PerfDecodeDomainState(0x2E800C57U, 19200U, &hw, &idx, &mv);
    CHECK_EQ_U64(hw, 1670400U);
    CHECK_EQ_U64(idx, 12U);
    CHECK_EQ_U64(mv, 744U);

    /* Real S23 Gold snapshot: 0x3B80128D -> L=0x8D(141)=2707.2MHz, idx=18, 0x3B8=952mV */
    S23PerfDecodeDomainState(0x3B80128DU, 19200U, &hw, &idx, &mv);
    CHECK_EQ_U64(hw, 2707200U);
    CHECK_EQ_U64(idx, 18U);
    CHECK_EQ_U64(mv, 952U);

    /* Real S23 Prime snapshot: 0x3A40139A -> L=0x9A(154)=2956.8MHz, idx=19, 0x3A4=932mV */
    S23PerfDecodeDomainState(0x3A40139AU, 19200U, &hw, &idx, &mv);
    CHECK_EQ_U64(hw, 2956800U);
    CHECK_EQ_U64(idx, 19U);
    CHECK_EQ_U64(mv, 932U);
}

static void test_compute_load_pct(void)
{
    /* Zero/negative total delta -> 0% */
    CHECK_EQ_U64(S23PerfComputeLoadPct(100, 1000, 100, 1000), 0U);
    CHECK_EQ_U64(S23PerfComputeLoadPct(100, 1000, 90, 900), 0U);

    /* 100% idle -> 0% */
    CHECK_EQ_U64(S23PerfComputeLoadPct(1000, 2000, 2000, 3000), 0U);

    /* Idle delta > total delta (clock skew guard) -> 0% */
    CHECK_EQ_U64(S23PerfComputeLoadPct(1000, 2000, 2500, 3000), 0U);

    /* 0% idle delta -> 100% */
    CHECK_EQ_U64(S23PerfComputeLoadPct(1000, 2000, 1000, 3000), 100U);

    /* 50% busy */
    CHECK_EQ_U64(S23PerfComputeLoadPct(1000, 2000, 1500, 3000), 50U);

    /* 25% busy */
    CHECK_EQ_U64(S23PerfComputeLoadPct(1000, 2000, 1750, 3000), 25U);
}

static void test_thermal_state_machine(void)
{
    const int32_t soft = 680, hard = 750, crit = 800;

    /* 0 valid sensors -> FAILSAFE regardless of temp */
    CHECK_EQ_U64(S23PerfComputeThermalState(400, 0U, soft, hard, crit, S23PERF_THERMAL_NORMAL),
                 S23PERF_THERMAL_FAILSAFE);

    /* Ramp up across thresholds */
    CHECK_EQ_U64(S23PerfComputeThermalState(679, 41U, soft, hard, crit, S23PERF_THERMAL_NORMAL),
                 S23PERF_THERMAL_NORMAL);
    CHECK_EQ_U64(S23PerfComputeThermalState(680, 41U, soft, hard, crit, S23PERF_THERMAL_NORMAL),
                 S23PERF_THERMAL_SOFT);
    CHECK_EQ_U64(S23PerfComputeThermalState(750, 41U, soft, hard, crit, S23PERF_THERMAL_SOFT),
                 S23PERF_THERMAL_HARD);
    CHECK_EQ_U64(S23PerfComputeThermalState(800, 41U, soft, hard, crit, S23PERF_THERMAL_HARD),
                 S23PERF_THERMAL_CRITICAL);

    /* 3.0 C (30 dC) hysteresis on step-down */
    CHECK_EQ_U64(S23PerfComputeThermalState(771, 41U, soft, hard, crit, S23PERF_THERMAL_CRITICAL),
                 S23PERF_THERMAL_CRITICAL);
    CHECK_EQ_U64(S23PerfComputeThermalState(770, 41U, soft, hard, crit, S23PERF_THERMAL_CRITICAL),
                 S23PERF_THERMAL_HARD);
    CHECK_EQ_U64(S23PerfComputeThermalState(721, 41U, soft, hard, crit, S23PERF_THERMAL_HARD),
                 S23PERF_THERMAL_HARD);
    CHECK_EQ_U64(S23PerfComputeThermalState(720, 41U, soft, hard, crit, S23PERF_THERMAL_HARD),
                 S23PERF_THERMAL_SOFT);
    CHECK_EQ_U64(S23PerfComputeThermalState(651, 41U, soft, hard, crit, S23PERF_THERMAL_SOFT),
                 S23PERF_THERMAL_SOFT);
    CHECK_EQ_U64(S23PerfComputeThermalState(650, 41U, soft, hard, crit, S23PERF_THERMAL_SOFT),
                 S23PERF_THERMAL_NORMAL);
}

static void test_select_target_idx(void)
{
    /* Prime domain shape: 22 LUT entries (0..21), last_non_boost_idx = 19 */
    const uint32_t lut = 22U, nb = 19U;
    const int32_t soft = 680, hard = 750;

    /* Balanced: idle -> 0, medium -> intermediate, >85% -> boost (21) when normal */
    CHECK_EQ_U64(S23PerfSelectTargetIdx(lut, nb, 5U, S23PERF_PROFILE_BALANCED, 0U, 450, S23PERF_THERMAL_NORMAL, soft, hard, 0U), 0U);
    uint32_t mid = S23PerfSelectTargetIdx(lut, nb, 50U, S23PERF_PROFILE_BALANCED, 0U, 450, S23PERF_THERMAL_NORMAL, soft, hard, 0U);
    CHECK(mid > 0U && mid < nb);
    CHECK_EQ_U64(S23PerfSelectTargetIdx(lut, nb, 95U, S23PERF_PROFILE_BALANCED, 0U, 450, S23PERF_THERMAL_NORMAL, soft, hard, 0U), 21U);

    /* Step-down hysteresis: dropping from 21 to idle steps down by at most 2 per tick */
    CHECK_EQ_U64(S23PerfSelectTargetIdx(lut, nb, 0U, S23PERF_PROFILE_BALANCED, 0U, 450, S23PERF_THERMAL_NORMAL, soft, hard, 21U), 19U);
    CHECK_EQ_U64(S23PerfSelectTargetIdx(lut, nb, 0U, S23PERF_PROFILE_BALANCED, 0U, 450, S23PERF_THERMAL_NORMAL, soft, hard, 1U), 0U);

    /* Powersave: capped at nb/2 = 9 even at 100% load */
    CHECK_EQ_U64(S23PerfSelectTargetIdx(lut, nb, 100U, S23PERF_PROFILE_POWERSAVE, 0U, 450, S23PERF_THERMAL_NORMAL, soft, hard, 0U), 9U);

    /* Performance: floor at nb/2 = 9 at 0% load, boost at >= 60% load */
    CHECK_EQ_U64(S23PerfSelectTargetIdx(lut, nb, 0U, S23PERF_PROFILE_PERFORMANCE, 0U, 450, S23PERF_THERMAL_NORMAL, soft, hard, 9U), 9U);
    CHECK_EQ_U64(S23PerfSelectTargetIdx(lut, nb, 60U, S23PERF_PROFILE_PERFORMANCE, 0U, 450, S23PERF_THERMAL_NORMAL, soft, hard, 9U), 21U);

    /* Soft thermal state blocks boost (caps at <= nb) */
    uint32_t soft_idx = S23PerfSelectTargetIdx(lut, nb, 100U, S23PERF_PROFILE_PERFORMANCE, 0U, 715, S23PERF_THERMAL_SOFT, soft, hard, 21U);
    CHECK(soft_idx <= nb && soft_idx >= (nb * 40U) / 100U);

    /* Hard thermal state immediately caps to 40% of nb (7), overriding step-down limit */
    CHECK_EQ_U64(S23PerfSelectTargetIdx(lut, nb, 100U, S23PERF_PROFILE_PERFORMANCE, 0U, 760, S23PERF_THERMAL_HARD, soft, hard, 21U), (nb * 40U) / 100U);

    /* Critical & Failsafe force LUT[0] even in FIXED mode */
    CHECK_EQ_U64(S23PerfSelectTargetIdx(lut, nb, 100U, S23PERF_PROFILE_FIXED, 21U, 800, S23PERF_THERMAL_CRITICAL, soft, hard, 21U), 0U);
    CHECK_EQ_U64(S23PerfSelectTargetIdx(lut, nb, 100U, S23PERF_PROFILE_FIXED, 21U, 400, S23PERF_THERMAL_FAILSAFE, soft, hard, 21U), 0U);

    /* Fixed clamps out-of-range manual_idx */
    CHECK_EQ_U64(S23PerfSelectTargetIdx(lut, nb, 0U, S23PERF_PROFILE_FIXED, 999U, 450, S23PERF_THERMAL_NORMAL, soft, hard, 0U), 21U);

    /* Zero LUT safety */
    CHECK_EQ_U64(S23PerfSelectTargetIdx(0U, 0U, 100U, S23PERF_PROFILE_BALANCED, 0U, 450, S23PERF_THERMAL_NORMAL, soft, hard, 0U), 0U);
}

int main(void)
{
    test_decode_domain_state();
    test_compute_load_pct();
    test_thermal_state_machine();
    test_select_target_idx();
    return test_report("test_perf_gov");
}
