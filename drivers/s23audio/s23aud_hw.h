/*
 * s23aud_hw.h - SM8550 ADSP PIL + GLink + APPS-SMMU + AudioReach + Dual CS35L43 Hardware Engine
 */
#pragma once

#include <wdm.h>

#ifdef __cplusplus
extern "C" {
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
