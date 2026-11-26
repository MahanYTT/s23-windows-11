/*
 * s23aud_hw.h - SM8550 ADSP PIL + GLink + APPS-SMMU + AudioReach + Dual CS35L43 Hardware Engine
 */
#pragma once

#include <wdm.h>

#ifndef _VCRUNTIME_H
#define _VCRUNTIME_H
#endif
#ifndef _VCRT_COMPILER_PREPROCESSOR
#define _VCRT_COMPILER_PREPROCESSOR 1
#endif
#ifndef _VCRUNTIME_DISABLED_WARNINGS
#define _VCRUNTIME_DISABLED_WARNINGS 4514
#endif

#ifndef _STDINT
#define _STDINT
typedef INT8    int8_t;
typedef UINT8   uint8_t;
typedef INT16   int16_t;
typedef UINT16  uint16_t;
typedef INT32   int32_t;
typedef UINT32  uint32_t;
typedef INT64   int64_t;
typedef UINT64  uint64_t;
#ifndef UINT8_MAX
#define UINT8_MAX  0xFFU
#endif
#ifndef UINT16_MAX
#define UINT16_MAX 0xFFFFU
#endif
#ifndef UINT32_MAX
#define UINT32_MAX 0xFFFFFFFFU
#endif
#ifndef UINT64_MAX
#define UINT64_MAX 0xFFFFFFFFFFFFFFFFULL
#endif
#endif

#include "s23aud_lpass_proto.h"
#include "../s23haptics/cirrus_fw.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CIRRUS_HALO_DSP_ADDR_DEFINED
#define CIRRUS_HALO_DSP_ADDR_DEFINED
#define CIRRUS_HALO_XM_PACKED_BASE   0x02000000U
#define CIRRUS_HALO_XM_UNPACKED_BASE 0x02800000U
#define CIRRUS_HALO_YM_PACKED_BASE   0x02C00000U
#define CIRRUS_HALO_YM_UNPACKED_BASE 0x03400000U
#define CIRRUS_HALO_PM_PACKED_BASE   0x03800000U

S23AUD_INLINE uint32_t cirrus_halo_dsp_addr(uint16_t mem_type, uint32_t word_offset)
{
    if (word_offset >= 0x00200000U) {
        return 0U;
    }
    switch (mem_type) {
    case CIRRUS_WMFW_HALO_XM_PACKED:
        return CIRRUS_HALO_XM_PACKED_BASE + (word_offset * 4U);
    case CIRRUS_WMFW_XM:
        return CIRRUS_HALO_XM_UNPACKED_BASE + (word_offset * 4U);
    case CIRRUS_WMFW_HALO_YM_PACKED:
        return CIRRUS_HALO_YM_PACKED_BASE + (word_offset * 4U);
    case CIRRUS_WMFW_YM:
        return CIRRUS_HALO_YM_UNPACKED_BASE + (word_offset * 4U);
    case CIRRUS_WMFW_HALO_PM_PACKED:
    case CIRRUS_WMFW_PM:
        return CIRRUS_HALO_PM_PACKED_BASE + (word_offset * 4U);
    case CIRRUS_WMFW_ABSOLUTE:
    case CIRRUS_WMDR_ABSOLUTE:
        return word_offset;
    default:
        return 0U;
    }
}
#endif

#define S23_AUDIO_TAG             'A32S'
#define S23_SAMPLE_RATE           48000U
#define S23_CHANNELS              2U
#define S23_BITS_PER_SAMPLE       16U
#define S23_BLOCK_ALIGN           4U
#define S23_AVG_BYTES_PER_SEC     (S23_SAMPLE_RATE * S23_BLOCK_ALIGN) /* 192000 */
#define S23_10MS_BYTES            1920U
#define S23_5MS_BYTES             960U
#define S23_HW_FIFO_BYTES         128U
#define S23_MAX_NOTIFY_EVENTS     8U

/* Volume range in 1/65536 dB: -96 dB to 0 dB (hardware clamped <= -6 dB in CS35L43) */
#define S23_VOL_MIN_DB_FIXED      (-96 * 65536)
#define S23_VOL_MAX_DB_FIXED      (0)
#define S23_VOL_STEP_DB_FIXED     (32768)        /* 0.5 dB */
#define S23_VOL_DEFAULT_DB_FIXED  (-18 * 65536)  /* -18 dB initial safe default */

typedef struct _S23_AUD_HW S23_AUD_HW;

/* Logging helpers (C:\Users\Public\s23audio.log) */
VOID S23AudLogFmt(const char *fmt, ...);
VOID S23AudLogFlush(VOID);

/* Lifecycle */
NTSTATUS S23AudHwCreate(S23_AUD_HW **OutHw);
VOID     S23AudHwDestroy(S23_AUD_HW *Hw);

/* Stream management (PASSIVE_LEVEL) */
NTSTATUS S23AudHwAttachRing(
    S23_AUD_HW *Hw,
    PVOID       RingVa,
    ULONGLONG   RingPa,
    ULONG       RingSize,
    ULONG       AllocSize,
    ULONG       NotificationCount,
    ULONG      *OutFifoBytes
);
VOID S23AudHwDetachRing(S23_AUD_HW *Hw);

NTSTATUS S23AudHwStartStream(S23_AUD_HW *Hw);
VOID     S23AudHwStopStream(S23_AUD_HW *Hw, BOOLEAN ResetPosition);

/* Position & Notification Events (may be called at <= DISPATCH_LEVEL) */
VOID S23AudHwGetPosition(
    S23_AUD_HW *Hw,
    ULONGLONG  *OutPlayOffset,
    ULONGLONG  *OutWriteOffset
);
NTSTATUS S23AudHwRegisterEvent(S23_AUD_HW *Hw, PKEVENT Event);
NTSTATUS S23AudHwUnregisterEvent(S23_AUD_HW *Hw, PKEVENT Event);

/* Topology Volume & Mute Controls (PASSIVE_LEVEL, safe hardware ceiling enforced) */
LONG    S23AudHwGetVolume(S23_AUD_HW *Hw, LONG Channel);
VOID    S23AudHwSetVolume(S23_AUD_HW *Hw, LONG Channel, LONG LevelFixedDb);
BOOLEAN S23AudHwGetMute(S23_AUD_HW *Hw, LONG Channel);
VOID    S23AudHwSetMute(S23_AUD_HW *Hw, LONG Channel, BOOLEAN Muted);

#ifdef __cplusplus
}
#endif
