/*
 * s23perf_ioctl.h - Shared kernel / user-mode ABI for s23perf.sys v2.0.0.0
 */
#ifndef S23PERF_IOCTL_H
#define S23PERF_IOCTL_H

#include <stdint.h>

#define S23PERF_ABI_VERSION          0x00020000U

#define S23PERF_DEVICE_NAME_W        L"\\Device\\S23Perf"
#define S23PERF_SYMLINK_NAME_W       L"\\DosDevices\\S23Perf"

/* CTL_CODE(0x8000, func, METHOD_BUFFERED=0, access) */
#define IOCTL_S23PERF_GET_TELEMETRY  0x80006000U  /* 0x8000, 0x800, BUFFERED, FILE_READ_ACCESS  */
#define IOCTL_S23PERF_SET_POLICY     0x8000A004U  /* 0x8000, 0x801, BUFFERED, FILE_WRITE_ACCESS */
#define IOCTL_S23PERF_GET_LUT        0x80006008U  /* 0x8000, 0x802, BUFFERED, FILE_READ_ACCESS  */

#define S23PERF_NUM_DOMAINS          3U
#define S23PERF_MAX_LUT              40U

#define S23PERF_PROFILE_BALANCED     0U
#define S23PERF_PROFILE_POWERSAVE    1U
#define S23PERF_PROFILE_PERFORMANCE  2U
#define S23PERF_PROFILE_FIXED        3U

#define S23PERF_THERMAL_NORMAL       0U
#define S23PERF_THERMAL_SOFT         1U
#define S23PERF_THERMAL_HARD         2U
#define S23PERF_THERMAL_CRITICAL     3U
#define S23PERF_THERMAL_FAILSAFE     4U

typedef struct _S23PERF_DOMAIN_TELEMETRY {
    uint32_t lut_count;
    uint32_t last_non_boost_idx;
    uint32_t cur_vote_idx;
    uint32_t granted_idx;
    uint32_t hw_freq_khz;
    uint32_t target_freq_khz;
    uint32_t granted_mv;
    uint32_t load_pct;
    uint32_t min_khz;
    uint32_t max_khz;
    uint32_t non_boost_max_khz;
    uint32_t lmh_events;
} S23PERF_DOMAIN_TELEMETRY;
_Static_assert(sizeof(S23PERF_DOMAIN_TELEMETRY) == 48, "S23PERF_DOMAIN_TELEMETRY size");

typedef struct _S23PERF_TELEMETRY {
    uint32_t version;
    uint32_t mode;
    uint32_t profile;
    uint32_t poll_ms;
    uint64_t sample_count;
    S23PERF_DOMAIN_TELEMETRY dom[S23PERF_NUM_DOMAINS];
    uint32_t tsens_valid;
    int32_t  tsens_min_dC;
    int32_t  tsens_max_dC;
    uint32_t tsens_hot_bank;
    uint32_t tsens_hot_sensor;
    uint32_t thermal_state;
    int32_t  soft_dC;
    int32_t  hard_dC;
    int32_t  crit_dC;
    uint32_t manual_idx[S23PERF_NUM_DOMAINS];
} S23PERF_TELEMETRY;
_Static_assert(sizeof(S23PERF_TELEMETRY) == 216, "S23PERF_TELEMETRY size");

typedef struct _S23PERF_POLICY_REQ {
    uint32_t profile;        /* S23PERF_PROFILE_* or 0xFFFFFFFF to keep */
    uint32_t manual_idx[S23PERF_NUM_DOMAINS]; /* 0xFFFFFFFF to keep */
    uint32_t soft_temp_c;    /* 0 to keep */
    uint32_t hard_temp_c;    /* 0 to keep */
    uint32_t crit_temp_c;    /* 0 to keep */
    uint32_t poll_ms;        /* 0 to keep */
} S23PERF_POLICY_REQ;
_Static_assert(sizeof(S23PERF_POLICY_REQ) == 32, "S23PERF_POLICY_REQ size");

typedef struct _S23PERF_LUT_ENTRY_ABI {
    uint32_t freq_khz;
    uint32_t volt_mv;
    uint8_t  src;
    uint8_t  lval;
    uint8_t  cores;
    uint8_t  _pad;
} S23PERF_LUT_ENTRY_ABI;
_Static_assert(sizeof(S23PERF_LUT_ENTRY_ABI) == 12, "S23PERF_LUT_ENTRY_ABI size");

typedef struct _S23PERF_LUT_RESPONSE {
    uint32_t domain_idx;
    uint32_t lut_count;
    uint32_t last_non_boost_idx;
    uint32_t _pad;
    S23PERF_LUT_ENTRY_ABI entries[S23PERF_MAX_LUT];
} S23PERF_LUT_RESPONSE;
_Static_assert(sizeof(S23PERF_LUT_RESPONSE) == 496, "S23PERF_LUT_RESPONSE size");

#endif /* S23PERF_IOCTL_H */
