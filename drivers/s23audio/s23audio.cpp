/*
 * s23audio.cpp - Samsung Galaxy S23 (SM8550-AC / dm1q) Native Windows 11 ARM64
 * PortCls WaveRT + Topology Audio Miniport Driver (Zero stdunk / Zero __chkstk)
 */
#define PUT_GUIDS_HERE
#include <portcls.h>
#include "s23aud_hw.h"

/* ============================================================================
 * Standalone CUnknownBase (No stdunk.h, no stdunk.lib, no operator delete)
 * ========================================================================== */
inline void *__cdecl operator new(size_t, void *p) noexcept { return p; }
void __cdecl operator delete(void *p) noexcept { if (p) ExFreePoolWithTag(p, S23_AUDIO_TAG); }

template <typename TDerived, typename TInterface>
class DECLSPEC_NOVTABLE CUnknownBase : public TInterface {
protected:
    volatile LONG m_RefCount;
    CUnknownBase() : m_RefCount(1) {}
    ~CUnknownBase() {}

public:
    template <typename... TArgs>
    static TDerived *Create(TArgs &&...args) {
        void *mem = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(TDerived), S23_AUDIO_TAG);
        if (!mem) return NULL;
        return new (mem) TDerived(static_cast<TArgs &&>(args)...);
    }

    STDMETHODIMP_(ULONG) AddRef() override {
        return (ULONG)InterlockedIncrement(&m_RefCount);
    }

    STDMETHODIMP_(ULONG) Release() override {
        ULONG rc = (ULONG)InterlockedDecrement(&m_RefCount);
        if (rc == 0) {
            TDerived *self = static_cast<TDerived *>(this);
            self->~TDerived();
            ExFreePoolWithTag(self, S23_AUDIO_TAG);
        }
        return rc;
    }

    STDMETHODIMP_(NTSTATUS) QueryInterface(
        _In_         REFIID InterfaceId,
        _COM_Outptr_ PVOID *Interface
    ) override {
        if (!Interface) return STATUS_INVALID_PARAMETER;
        *Interface = NULL;
        if (IsEqualGUIDAligned(InterfaceId, IID_IUnknown) ||
            static_cast<TDerived *>(this)->MatchesIID(InterfaceId)) {
            *Interface = static_cast<TInterface *>(this);
            AddRef();
            return STATUS_SUCCESS;
        }
        return STATUS_INVALID_PARAMETER;
    }
};

/* ============================================================================
 * Static GUIDs in .rdata (zero dynamic initializers / zero .CRT$XCU)
 * ========================================================================== */
static const GUID g_GuidCatAudio     = { STATIC_KSCATEGORY_AUDIO };
static const GUID g_GuidCatRender    = { STATIC_KSCATEGORY_RENDER };
static const GUID g_GuidCatRealtime  = { STATIC_KSCATEGORY_REALTIME };
static const GUID g_GuidCatTopology  = { STATIC_KSCATEGORY_TOPOLOGY };
static const GUID g_GuidNodeDac      = { STATIC_KSNODETYPE_DAC };
static const GUID g_GuidNodeVolume   = { STATIC_KSNODETYPE_VOLUME };
static const GUID g_GuidNodeMute     = { STATIC_KSNODETYPE_MUTE };
static const GUID g_GuidNodeSpeaker  = { STATIC_KSNODETYPE_SPEAKER };
static const GUID g_GuidPropAudio    = { STATIC_KSPROPSETID_Audio };
static const GUID g_GuidPropJack     = { STATIC_KSPROPSETID_Jack };
static const GUID g_GuidPropTypeGen  = { STATIC_KSPROPTYPESETID_General };
static const GUID g_GuidSubPcm       = { STATIC_KSDATAFORMAT_SUBTYPE_PCM };
static const GUID g_GuidFmtAudio     = { STATIC_KSDATAFORMAT_TYPE_AUDIO };
static const GUID g_GuidSpecWfx      = { STATIC_KSDATAFORMAT_SPECIFIER_WAVEFORMATEX };
static const GUID g_GuidWildcard     = { 0x00000000, 0x0000, 0x0000, { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 } };

/* Global hardware engine & chained PortCls PnP/Unload handlers */
static S23_AUD_HW      *g_Hw           = NULL;
static PDRIVER_DISPATCH g_PcPnpDispatch = NULL;
static PDRIVER_UNLOAD   g_PcUnload      = NULL;
static LONG             g_RingLogCnt    = 0;
static LONG             g_MiscLogCnt    = 0;

/* ============================================================================
 * WaveRT Stream (IMiniportWaveRTStreamNotification)
 * ========================================================================== */
class CS23WaveRTStream : public CUnknownBase<CS23WaveRTStream, IMiniportWaveRTStreamNotification> {
private:
    S23_AUD_HW        *m_Hw;
    PPORTWAVERTSTREAM  m_PortStream;
    PMDL               m_BufferMdl;
    PVOID              m_BufferVa;
    ULONGLONG          m_BufferPa;
    ULONG              m_BufferSize;
    ULONG              m_AllocSize;
    ULONG              m_FifoBytes;
    KSSTATE            m_State;

public:
    CS23WaveRTStream(S23_AUD_HW *hw, PPORTWAVERTSTREAM portStream)
        : m_Hw(hw), m_PortStream(portStream), m_BufferMdl(NULL), m_BufferVa(NULL),
          m_BufferPa(0), m_BufferSize(0), m_AllocSize(0), m_FifoBytes(S23_HW_FIFO_BYTES),
          m_State(KSSTATE_STOP) {
        if (m_PortStream) m_PortStream->AddRef();
    }

    ~CS23WaveRTStream() {
        if (InterlockedIncrement(&g_MiscLogCnt) <= 40) {
            S23AudLogFmt("[RT] ~Stream this=%p\r\n", this);
            S23AudLogFlush();
        }
        CleanupBuffer();
        if (m_PortStream) {
            m_PortStream->Release();
            m_PortStream = NULL;
        }
    }

    bool MatchesIID(REFIID iid) const {
        return IsEqualGUIDAligned(iid, IID_IMiniportWaveRTStream) ||
               IsEqualGUIDAligned(iid, IID_IMiniportWaveRTStreamNotification);
    }

    VOID CleanupBuffer() {
        if (m_Hw) {
            S23AudHwDetachRing(m_Hw);
        }
        if (m_BufferVa && m_BufferMdl && m_PortStream) {
            m_PortStream->UnmapAllocatedPages(m_BufferVa, m_BufferMdl);
            m_BufferVa = NULL;
        }
        if (m_BufferMdl && m_PortStream) {
            m_PortStream->FreePagesFromMdl(m_BufferMdl);
            m_BufferMdl = NULL;
        }
        m_BufferPa   = 0;
        m_BufferSize = 0;
        m_AllocSize  = 0;
    }

    NTSTATUS AllocateRingCommon(
        ULONG                NotificationCount,
        ULONG                RequestedSize,
        PMDL                *AudioBufferMdl,
        ULONG               *ActualSize,
        ULONG               *OffsetFromFirstPage,
        MEMORY_CACHING_TYPE *CacheType
    ) {
        if (!AudioBufferMdl || !ActualSize || !OffsetFromFirstPage || !CacheType || !m_PortStream || !m_Hw) {
            return STATUS_INVALID_PARAMETER;
        }
        *AudioBufferMdl      = NULL;
        *ActualSize          = 0;
        *OffsetFromFirstPage = 0;
        *CacheType           = MmNonCached;

        CleanupBuffer();

        ULONG nCount = (NotificationCount >= 1U) ? NotificationCount : 2U;
        if (nCount > 8U) nCount = 8U;

        /* Align ActualSize to a multiple of (nCount * 5ms = nCount * 960B), minimum 1920B (10ms), max 61440B */
        ULONG gran = nCount * S23_5MS_BYTES;
        ULONG sz   = RequestedSize;
        if (sz < 2U * S23_5MS_BYTES) sz = 2U * S23_5MS_BYTES;
        sz = ((sz + gran - 1U) / gran) * gran;
        if (sz > 32U * S23_10MS_BYTES) sz = (32U * S23_10MS_BYTES / gran) * gran;
        if (sz < gran) sz = gran;

        ULONG allocSz = (sz + 0xFFFU) & ~0xFFFU;
        PHYSICAL_ADDRESS lo, hi;
        lo.QuadPart = 0;
        hi.QuadPart = 0xFFFFFFFFULL;

        /* CPortPinWaveRT::AllocateContiguousPagesForMdl allocates MmNonCached pages in PFN database */
        PMDL mdl = m_PortStream->AllocateContiguousPagesForMdl(lo, hi, allocSz);
        if (!mdl) {
            S23AudLogFmt("[RT] AllocateContiguousPagesForMdl(%u) failed\r\n", allocSz);
            S23AudLogFlush();
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        PVOID va = m_PortStream->MapAllocatedPages(mdl, MmNonCached);
        if (!va) {
            S23AudLogFmt("[RT] MapAllocatedPages(MmNonCached) failed\r\n");
            S23AudLogFlush();
            m_PortStream->FreePagesFromMdl(mdl);
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        RtlZeroMemory(va, allocSz);
        ULONGLONG basePa = (ULONGLONG)m_PortStream->GetPhysicalPageAddress(mdl, 0).QuadPart;
        if (basePa == 0ULL || (basePa + allocSz) > 0x100000000ULL) {
            S23AudLogFmt("[RT] Contiguous PA out of 32-bit range: 0x%llx\r\n", basePa);
            S23AudLogFlush();
            m_PortStream->UnmapAllocatedPages(va, mdl);
            m_PortStream->FreePagesFromMdl(mdl);
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        m_BufferMdl  = mdl;
        m_BufferVa   = va;
        m_BufferPa   = basePa;
        m_BufferSize = sz;
        m_AllocSize  = allocSz;

        NTSTATUS st = S23AudHwAttachRing(m_Hw, va, basePa, sz, allocSz, nCount, &m_FifoBytes);
        if (!NT_SUCCESS(st)) {
            CleanupBuffer();
            return st;
        }

        *AudioBufferMdl      = mdl;
        *ActualSize          = sz;
        *OffsetFromFirstPage = 0U;
        *CacheType           = MmNonCached;
        if (InterlockedIncrement(&g_RingLogCnt) <= 12) {
            S23AudLogFmt("[RT] AllocRing ok n=%u req=%u sz=%u alloc=%u mdl=%p pa=0x%llx cache=%d\r\n",
                         nCount, RequestedSize, sz, allocSz, mdl, basePa, (int)*CacheType);
            S23AudLogFlush();
        }
        return STATUS_SUCCESS;
    }

    IMP_IMiniportWaveRTStream;
    IMP_IMiniportWaveRTStreamNotification;
};

STDMETHODIMP_(NTSTATUS) CS23WaveRTStream::SetFormat(_In_ PKSDATAFORMAT DataFormat) {
    if (!DataFormat || DataFormat->FormatSize < sizeof(KSDATAFORMAT_WAVEFORMATEX)) {
        return STATUS_INVALID_PARAMETER;
    }
    const WAVEFORMATEX *wfx = &((const KSDATAFORMAT_WAVEFORMATEX *)DataFormat)->WaveFormatEx;
    if (wfx->nChannels != S23_CHANNELS ||
        wfx->nSamplesPerSec != S23_SAMPLE_RATE ||
        wfx->wBitsPerSample != S23_BITS_PER_SAMPLE) {
        return STATUS_NO_MATCH;
    }
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CS23WaveRTStream::SetState(_In_ KSSTATE State) {
    S23AudLogFmt("[RT] SetState %d -> %d\r\n", (int)m_State, (int)State);
    NTSTATUS st = STATUS_SUCCESS;
    if (State == KSSTATE_RUN) {
        if (m_State != KSSTATE_RUN) {
            st = S23AudHwStartStream(m_Hw);
        }
    } else if (State == KSSTATE_PAUSE) {
        if (m_State == KSSTATE_RUN) {
            S23AudHwStopStream(m_Hw, FALSE);
        }
    } else if (State == KSSTATE_STOP || State == KSSTATE_ACQUIRE) {
        if (m_State == KSSTATE_RUN || m_State == KSSTATE_PAUSE) {
            S23AudHwStopStream(m_Hw, (State == KSSTATE_STOP) ? TRUE : FALSE);
        } else if (State == KSSTATE_STOP) {
            S23AudHwStopStream(m_Hw, TRUE);
        }
    }
    if (NT_SUCCESS(st)) {
        m_State = State;
    }
    return st;
}

STDMETHODIMP_(NTSTATUS) CS23WaveRTStream::GetPosition(_Out_ PKSAUDIO_POSITION Position) {
    if (!Position || !m_Hw) return STATUS_INVALID_PARAMETER;
    S23AudHwGetPosition(m_Hw, &Position->PlayOffset, &Position->WriteOffset);
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CS23WaveRTStream::AllocateAudioBuffer(
    _In_  ULONG                RequestedSize,
    _Out_ PMDL                *AudioBufferMdl,
    _Out_ ULONG               *ActualSize,
    _Out_ ULONG               *OffsetFromFirstPage,
    _Out_ MEMORY_CACHING_TYPE *CacheType
) {
    return AllocateRingCommon(2U, RequestedSize, AudioBufferMdl, ActualSize, OffsetFromFirstPage, CacheType);
}

STDMETHODIMP_(VOID) CS23WaveRTStream::FreeAudioBuffer(
    _In_opt_ PMDL  AudioBufferMdl,
    _In_     ULONG BufferSize
) {
    UNREFERENCED_PARAMETER(AudioBufferMdl);
    UNREFERENCED_PARAMETER(BufferSize);
    CleanupBuffer();
}

STDMETHODIMP_(VOID) CS23WaveRTStream::GetHWLatency(_Out_ KSRTAUDIO_HWLATENCY *hwLatency) {
    if (InterlockedIncrement(&g_MiscLogCnt) <= 40) {
        S23AudLogFmt("[RT] GetHWLatency fifo=%u\r\n", m_FifoBytes);
        S23AudLogFlush();
    }
    if (!hwLatency) return;
    hwLatency->FifoSize     = m_FifoBytes;
    hwLatency->ChipsetDelay = 0U;
    hwLatency->CodecDelay   = 0U;
}

STDMETHODIMP_(NTSTATUS) CS23WaveRTStream::GetPositionRegister(_Out_ KSRTAUDIO_HWREGISTER *Register) {
    if (InterlockedIncrement(&g_MiscLogCnt) <= 40) {
        S23AudLogFmt("[RT] GetPositionRegister -> NOT_SUPPORTED\r\n");
        S23AudLogFlush();
    }
    UNREFERENCED_PARAMETER(Register);
    return STATUS_NOT_SUPPORTED;
}

STDMETHODIMP_(NTSTATUS) CS23WaveRTStream::GetClockRegister(_Out_ KSRTAUDIO_HWREGISTER *Register) {
    if (InterlockedIncrement(&g_MiscLogCnt) <= 40) {
        S23AudLogFmt("[RT] GetClockRegister -> NOT_SUPPORTED\r\n");
        S23AudLogFlush();
    }
    UNREFERENCED_PARAMETER(Register);
    return STATUS_NOT_SUPPORTED;
}

STDMETHODIMP_(NTSTATUS) CS23WaveRTStream::AllocateBufferWithNotification(
    _In_  ULONG                NotificationCount,
    _In_  ULONG                RequestedSize,
    _Out_ PMDL                *AudioBufferMdl,
    _Out_ ULONG               *ActualSize,
    _Out_ ULONG               *OffsetFromFirstPage,
    _Out_ MEMORY_CACHING_TYPE *CacheType
) {
    return AllocateRingCommon(NotificationCount, RequestedSize, AudioBufferMdl, ActualSize, OffsetFromFirstPage, CacheType);
}

STDMETHODIMP_(VOID) CS23WaveRTStream::FreeBufferWithNotification(
    _In_ PMDL  AudioBufferMdl,
    _In_ ULONG BufferSize
) {
    if (InterlockedIncrement(&g_RingLogCnt) <= 12) {
        S23AudLogFmt("[RT] FreeBufferWithNotification mdl=%p sz=%u\r\n", AudioBufferMdl, BufferSize);
        S23AudLogFlush();
    }
    UNREFERENCED_PARAMETER(AudioBufferMdl);
    UNREFERENCED_PARAMETER(BufferSize);
    CleanupBuffer();
}

STDMETHODIMP_(NTSTATUS) CS23WaveRTStream::RegisterNotificationEvent(_In_ PKEVENT NotificationEvent) {
    S23AudLogFmt("[RT] RegisterNotificationEvent(%p)\r\n", NotificationEvent);
    S23AudLogFlush();
    return S23AudHwRegisterEvent(m_Hw, NotificationEvent);
}

STDMETHODIMP_(NTSTATUS) CS23WaveRTStream::UnregisterNotificationEvent(_In_ PKEVENT NotificationEvent) {
    S23AudLogFmt("[RT] UnregisterNotificationEvent(%p)\r\n", NotificationEvent);
    S23AudLogFlush();
    return S23AudHwUnregisterEvent(m_Hw, NotificationEvent);
}

/* ============================================================================
 * WaveRT Miniport Descriptor & Implementation (IMiniportWaveRT)
 * ========================================================================== */
static KSDATARANGE_AUDIO g_WavePinDataRangePcm = {
    {
        sizeof(KSDATARANGE_AUDIO),
        0,
        0,
        0,
        { STATIC_KSDATAFORMAT_TYPE_AUDIO },
        { STATIC_KSDATAFORMAT_SUBTYPE_PCM },
        { STATIC_KSDATAFORMAT_SPECIFIER_WAVEFORMATEX }
    },
    S23_CHANNELS,         /* MaximumChannels */
    S23_BITS_PER_SAMPLE,  /* MinimumBitsPerSample */
    S23_BITS_PER_SAMPLE,  /* MaximumBitsPerSample */
    S23_SAMPLE_RATE,      /* MinimumSampleFrequency */
    S23_SAMPLE_RATE       /* MaximumSampleFrequency */
};

static PKSDATARANGE g_WaveHostPinRanges[] = {
    (PKSDATARANGE)&g_WavePinDataRangePcm
};

static KSDATARANGE g_AnalogBridgeRange = {
    sizeof(KSDATARANGE),
    0,
    0,
    0,
    { STATIC_KSDATAFORMAT_TYPE_AUDIO },
    { STATIC_KSDATAFORMAT_SUBTYPE_ANALOG },
    { STATIC_KSDATAFORMAT_SPECIFIER_NONE }
};

static PKSDATARANGE g_AnalogBridgeRanges[] = {
    &g_AnalogBridgeRange
};

static const PCPIN_DESCRIPTOR g_WavePins[] = {
    /* Pin 0: WaveRT Host Render Sink */
    {
        1, 1, 0,
        NULL,
        {
            0, NULL,
            0, NULL,
            RTL_NUMBER_OF(g_WaveHostPinRanges), g_WaveHostPinRanges,
            KSPIN_DATAFLOW_IN,
            KSPIN_COMMUNICATION_SINK,
            &g_GuidCatAudio,
            NULL,
            0
        }
    },
    /* Pin 1: WaveRT Bridge Out -> Topology Bridge In */
    {
        0, 0, 0,
        NULL,
        {
            0, NULL,
            0, NULL,
            RTL_NUMBER_OF(g_AnalogBridgeRanges), g_AnalogBridgeRanges,
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_NONE,
            &g_GuidCatAudio,
            NULL,
            0
        }
    }
};

static const PCNODE_DESCRIPTOR g_WaveNodes[] = {
    { 0, NULL, &g_GuidNodeDac, NULL }
};

static const PCCONNECTION_DESCRIPTOR g_WaveConnections[] = {
    { PCFILTER_NODE, 0, 0, KSNODEPIN_STANDARD_IN },
    { 0, KSNODEPIN_STANDARD_OUT, PCFILTER_NODE, 1 }
};

static const GUID g_WaveCategories[] = {
    { STATIC_KSCATEGORY_AUDIO },
    { STATIC_KSCATEGORY_RENDER },
    { STATIC_KSCATEGORY_REALTIME }
};

static PCFILTER_DESCRIPTOR g_WaveFilterDesc = {
    0,
    NULL,
    sizeof(PCPIN_DESCRIPTOR),
    RTL_NUMBER_OF(g_WavePins),
    g_WavePins,
    sizeof(PCNODE_DESCRIPTOR),
    RTL_NUMBER_OF(g_WaveNodes),
    g_WaveNodes,
    RTL_NUMBER_OF(g_WaveConnections),
    g_WaveConnections,
    RTL_NUMBER_OF(g_WaveCategories),
    g_WaveCategories
};

class CS23WaveRTMiniport : public CUnknownBase<CS23WaveRTMiniport, IMiniportWaveRT> {
private:
    S23_AUD_HW *m_Hw;
    PPORTWAVERT m_Port;

public:
    explicit CS23WaveRTMiniport(S23_AUD_HW *hw) : m_Hw(hw), m_Port(NULL) {}
    ~CS23WaveRTMiniport() {
        if (m_Port) { m_Port->Release(); m_Port = NULL; }
    }

    bool MatchesIID(REFIID iid) const {
        return IsEqualGUIDAligned(iid, IID_IMiniport) ||
               IsEqualGUIDAligned(iid, IID_IMiniportWaveRT);
    }

    IMP_IMiniportWaveRT;
};

STDMETHODIMP_(NTSTATUS) CS23WaveRTMiniport::Init(
    _In_ PUNKNOWN      UnknownAdapter,
    _In_ PRESOURCELIST ResourceList,
    _In_ PPORTWAVERT   Port
) {
    UNREFERENCED_PARAMETER(UnknownAdapter);
    UNREFERENCED_PARAMETER(ResourceList);
    if (!Port) return STATUS_INVALID_PARAMETER;
    m_Port = Port;
    m_Port->AddRef();
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CS23WaveRTMiniport::GetDescription(_Out_ PPCFILTER_DESCRIPTOR *Description) {
    if (!Description) return STATUS_INVALID_PARAMETER;
    *Description = &g_WaveFilterDesc;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CS23WaveRTMiniport::DataRangeIntersection(
    _In_  ULONG        PinId,
    _In_  PKSDATARANGE DataRange,
    _In_  PKSDATARANGE MatchingDataRange,
    _In_  ULONG        OutputBufferLength,
    _Out_writes_bytes_to_opt_(OutputBufferLength, *ResultantFormatLength) PVOID ResultantFormat,
    _Out_ PULONG       ResultantFormatLength
) {
    UNREFERENCED_PARAMETER(MatchingDataRange);
    if (!DataRange || !ResultantFormatLength || PinId != 0U) {
        return STATUS_NOT_IMPLEMENTED;
    }
    if ((!IsEqualGUIDAligned(DataRange->MajorFormat, g_GuidFmtAudio) &&
         !IsEqualGUIDAligned(DataRange->MajorFormat, g_GuidWildcard)) ||
        (!IsEqualGUIDAligned(DataRange->SubFormat, g_GuidSubPcm) &&
         !IsEqualGUIDAligned(DataRange->SubFormat, g_GuidWildcard)) ||
        DataRange->FormatSize < sizeof(KSDATARANGE_AUDIO)) {
        return STATUS_NO_MATCH;
    }

    const KSDATARANGE_AUDIO *ar = (const KSDATARANGE_AUDIO *)DataRange;
    if (ar->MaximumChannels < S23_CHANNELS ||
        ar->MinimumBitsPerSample > S23_BITS_PER_SAMPLE || ar->MaximumBitsPerSample < S23_BITS_PER_SAMPLE ||
        ar->MinimumSampleFrequency > S23_SAMPLE_RATE   || ar->MaximumSampleFrequency < S23_SAMPLE_RATE) {
        return STATUS_NO_MATCH;
    }

    const ULONG needSize = sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE);
    *ResultantFormatLength = needSize;
    if (InterlockedIncrement(&g_MiscLogCnt) <= 40) {
        S23AudLogFmt("[RT] DataRangeIntersection outLen=%u ch=%u hz=%u..%u bits=%u..%u\r\n",
                     OutputBufferLength, ar->MaximumChannels,
                     ar->MinimumSampleFrequency, ar->MaximumSampleFrequency,
                     ar->MinimumBitsPerSample, ar->MaximumBitsPerSample);
        S23AudLogFlush();
    }
    if (OutputBufferLength == 0U) {
        return STATUS_BUFFER_OVERFLOW;
    }
    if (OutputBufferLength < needSize || !ResultantFormat) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    KSDATAFORMAT_WAVEFORMATEXTENSIBLE *out = (KSDATAFORMAT_WAVEFORMATEXTENSIBLE *)ResultantFormat;
    RtlZeroMemory(out, needSize);
    out->DataFormat.FormatSize                  = needSize;
    out->DataFormat.MajorFormat                 = g_GuidFmtAudio;
    out->DataFormat.SubFormat                   = g_GuidSubPcm;
    out->DataFormat.Specifier                   = g_GuidSpecWfx;
    out->DataFormat.SampleSize                  = S23_BLOCK_ALIGN;
    out->WaveFormatExt.Format.wFormatTag        = WAVE_FORMAT_EXTENSIBLE;
    out->WaveFormatExt.Format.nChannels         = S23_CHANNELS;
    out->WaveFormatExt.Format.nSamplesPerSec    = S23_SAMPLE_RATE;
    out->WaveFormatExt.Format.wBitsPerSample    = S23_BITS_PER_SAMPLE;
    out->WaveFormatExt.Format.nBlockAlign       = S23_BLOCK_ALIGN;
    out->WaveFormatExt.Format.nAvgBytesPerSec   = S23_AVG_BYTES_PER_SEC;
    out->WaveFormatExt.Format.cbSize            = 22U;
    out->WaveFormatExt.Samples.wValidBitsPerSample = S23_BITS_PER_SAMPLE;
    out->WaveFormatExt.dwChannelMask            = KSAUDIO_SPEAKER_STEREO;
    out->WaveFormatExt.SubFormat                = g_GuidSubPcm;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CS23WaveRTMiniport::GetDeviceDescription(_Out_ PDEVICE_DESCRIPTION DeviceDescription) {
    UNREFERENCED_PARAMETER(DeviceDescription);
    return STATUS_NOT_SUPPORTED;
}

STDMETHODIMP_(NTSTATUS) CS23WaveRTMiniport::NewStream(
    _Out_ PMINIPORTWAVERTSTREAM *Stream,
    _In_  PPORTWAVERTSTREAM      PortStream,
    _In_  ULONG                  Pin,
    _In_  BOOLEAN                Capture,
    _In_  PKSDATAFORMAT          DataFormat
) {
    if (!Stream || !PortStream || Pin != 0U || Capture) {
        return STATUS_INVALID_PARAMETER;
    }
    *Stream = NULL;

    CS23WaveRTStream *s = CS23WaveRTStream::Create(m_Hw, PortStream);
    if (!s) return STATUS_INSUFFICIENT_RESOURCES;

    NTSTATUS st = s->SetFormat(DataFormat);
    if (InterlockedIncrement(&g_MiscLogCnt) <= 40) {
        const WAVEFORMATEX *w = (DataFormat && DataFormat->FormatSize >= sizeof(KSDATAFORMAT_WAVEFORMATEX))
            ? &((const KSDATAFORMAT_WAVEFORMATEX *)DataFormat)->WaveFormatEx : NULL;
        S23AudLogFmt("[RT] NewStream s=%p fmtSz=%u tag=0x%x ch=%u hz=%u bits=%u st=0x%x\r\n",
                     s, DataFormat ? DataFormat->FormatSize : 0U,
                     w ? w->wFormatTag : 0U, w ? w->nChannels : 0U,
                     w ? w->nSamplesPerSec : 0U, w ? w->wBitsPerSample : 0U, st);
        S23AudLogFlush();
    }
    if (!NT_SUCCESS(st)) {
        s->Release();
        return st;
    }
    *Stream = s;
    return STATUS_SUCCESS;
}

/* ============================================================================
 * Topology Miniport Property Handlers & Descriptor (IMiniportTopology)
 * ========================================================================== */
static NTSTATUS TopoPropCpuResources(_In_ PPCPROPERTY_REQUEST Req) {
    if (Req->Verb & KSPROPERTY_TYPE_BASICSUPPORT) {
        if (Req->ValueSize >= sizeof(ULONG) && Req->Value) {
            *(PULONG)Req->Value = KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT;
            Req->ValueSize = sizeof(ULONG);
            return STATUS_SUCCESS;
        }
        Req->ValueSize = sizeof(ULONG);
        return STATUS_BUFFER_OVERFLOW;
    }
    if (Req->Verb & KSPROPERTY_TYPE_GET) {
        if (Req->ValueSize >= sizeof(ULONG) && Req->Value) {
            *(PULONG)Req->Value = KSAUDIO_CPU_RESOURCES_NOT_HOST_CPU;
            Req->ValueSize = sizeof(ULONG);
            return STATUS_SUCCESS;
        }
        Req->ValueSize = sizeof(ULONG);
        return STATUS_BUFFER_TOO_SMALL;
    }
    return STATUS_INVALID_DEVICE_REQUEST;
}

static NTSTATUS TopoPropVolume(_In_ PPCPROPERTY_REQUEST Req) {
    if (Req->Verb & KSPROPERTY_TYPE_BASICSUPPORT) {
        const ULONG fullSize = sizeof(KSPROPERTY_DESCRIPTION) +
                               sizeof(KSPROPERTY_MEMBERSHEADER) +
                               sizeof(KSPROPERTY_STEPPING_LONG);
        if (Req->ValueSize == 0U) {
            Req->ValueSize = fullSize;
            return STATUS_BUFFER_OVERFLOW;
        }
        if (Req->ValueSize == sizeof(ULONG) && Req->Value) {
            *(PULONG)Req->Value = KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT;
            Req->ValueSize = sizeof(ULONG);
            return STATUS_SUCCESS;
        }
        if (Req->ValueSize >= sizeof(KSPROPERTY_DESCRIPTION) && Req->Value) {
            PKSPROPERTY_DESCRIPTION desc = (PKSPROPERTY_DESCRIPTION)Req->Value;
            desc->AccessFlags        = KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT;
            desc->DescriptionSize    = fullSize;
            desc->PropTypeSet.Set    = g_GuidPropTypeGen;
            desc->PropTypeSet.Id     = VT_I4;
            desc->PropTypeSet.Flags  = 0U;
            desc->MembersListCount   = 1U;
            desc->Reserved           = 0U;
            if (Req->ValueSize >= fullSize) {
                PKSPROPERTY_MEMBERSHEADER mh = (PKSPROPERTY_MEMBERSHEADER)(desc + 1);
                mh->MembersFlags = KSPROPERTY_MEMBER_STEPPEDRANGES;
                mh->MembersSize  = sizeof(KSPROPERTY_STEPPING_LONG);
                mh->MembersCount = 1U;
                mh->Flags        = KSPROPERTY_MEMBER_FLAG_BASICSUPPORT_MULTICHANNEL;
                PKSPROPERTY_STEPPING_LONG rng = (PKSPROPERTY_STEPPING_LONG)(mh + 1);
                rng->SteppingDelta        = (ULONG)S23_VOL_STEP_DB_FIXED;
                rng->Reserved             = 0U;
                rng->Bounds.SignedMinimum = S23_VOL_MIN_DB_FIXED;
                rng->Bounds.SignedMaximum = S23_VOL_MAX_DB_FIXED;
                Req->ValueSize = fullSize;
            } else {
                Req->ValueSize = sizeof(KSPROPERTY_DESCRIPTION);
            }
            return STATUS_SUCCESS;
        }
        Req->ValueSize = fullSize;
        return STATUS_BUFFER_TOO_SMALL;
    }

    LONG ch = (Req->InstanceSize >= sizeof(LONG) && Req->Instance) ? *(PLONG)Req->Instance : -1;
    if (Req->ValueSize < sizeof(LONG) || !Req->Value) {
        Req->ValueSize = sizeof(LONG);
        return STATUS_BUFFER_TOO_SMALL;
    }
    if (Req->Verb & KSPROPERTY_TYPE_GET) {
        *(PLONG)Req->Value = S23AudHwGetVolume(g_Hw, ch);
        Req->ValueSize = sizeof(LONG);
        return STATUS_SUCCESS;
    }
    if (Req->Verb & KSPROPERTY_TYPE_SET) {
        S23AudHwSetVolume(g_Hw, ch, *(PLONG)Req->Value);
        return STATUS_SUCCESS;
    }
    return STATUS_INVALID_DEVICE_REQUEST;
}

static NTSTATUS TopoPropMute(_In_ PPCPROPERTY_REQUEST Req) {
    if (Req->Verb & KSPROPERTY_TYPE_BASICSUPPORT) {
        if (Req->ValueSize == 0U) {
            Req->ValueSize = sizeof(KSPROPERTY_DESCRIPTION);
            return STATUS_BUFFER_OVERFLOW;
        }
        if (Req->ValueSize == sizeof(ULONG) && Req->Value) {
            *(PULONG)Req->Value = KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT;
            Req->ValueSize = sizeof(ULONG);
            return STATUS_SUCCESS;
        }
        if (Req->ValueSize >= sizeof(KSPROPERTY_DESCRIPTION) && Req->Value) {
            PKSPROPERTY_DESCRIPTION desc = (PKSPROPERTY_DESCRIPTION)Req->Value;
            desc->AccessFlags        = KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT;
            desc->DescriptionSize    = sizeof(KSPROPERTY_DESCRIPTION);
            desc->PropTypeSet.Set    = g_GuidPropTypeGen;
            desc->PropTypeSet.Id     = VT_BOOL;
            desc->PropTypeSet.Flags  = 0U;
            desc->MembersListCount   = 0U;
            desc->Reserved           = 0U;
            Req->ValueSize = sizeof(KSPROPERTY_DESCRIPTION);
            return STATUS_SUCCESS;
        }
        Req->ValueSize = sizeof(KSPROPERTY_DESCRIPTION);
        return STATUS_BUFFER_TOO_SMALL;
    }

    LONG ch = (Req->InstanceSize >= sizeof(LONG) && Req->Instance) ? *(PLONG)Req->Instance : -1;
    if (Req->ValueSize < sizeof(BOOL) || !Req->Value) {
        Req->ValueSize = sizeof(BOOL);
        return STATUS_BUFFER_TOO_SMALL;
    }
    if (Req->Verb & KSPROPERTY_TYPE_GET) {
        *(PBOOL)Req->Value = S23AudHwGetMute(g_Hw, ch) ? TRUE : FALSE;
        Req->ValueSize = sizeof(BOOL);
        return STATUS_SUCCESS;
    }
    if (Req->Verb & KSPROPERTY_TYPE_SET) {
        S23AudHwSetMute(g_Hw, ch, (*(PBOOL)Req->Value != 0) ? TRUE : FALSE);
        return STATUS_SUCCESS;
    }
    return STATUS_INVALID_DEVICE_REQUEST;
}

static NTSTATUS TopoPropJackDescription(_In_ PPCPROPERTY_REQUEST Req) {
    ULONG pinId = (Req->InstanceSize >= sizeof(ULONG) && Req->Instance) ? *(PULONG)Req->Instance : (ULONG)-1;
    if (pinId != 1U) {
        return STATUS_INVALID_PARAMETER;
    }
    const ULONG needSize = sizeof(KSMULTIPLE_ITEM) + sizeof(KSJACK_DESCRIPTION);
    if (Req->Verb & KSPROPERTY_TYPE_BASICSUPPORT) {
        if (Req->ValueSize >= sizeof(ULONG) && Req->Value) {
            *(PULONG)Req->Value = KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT;
            Req->ValueSize = sizeof(ULONG);
            return STATUS_SUCCESS;
        }
        Req->ValueSize = sizeof(ULONG);
        return STATUS_BUFFER_OVERFLOW;
    }
    if (Req->Verb & KSPROPERTY_TYPE_GET) {
        if (Req->ValueSize == 0U) {
            Req->ValueSize = needSize;
            return STATUS_BUFFER_OVERFLOW;
        }
        if (Req->ValueSize < needSize || !Req->Value) {
            Req->ValueSize = needSize;
            return STATUS_BUFFER_TOO_SMALL;
        }
        PKSMULTIPLE_ITEM mi = (PKSMULTIPLE_ITEM)Req->Value;
        mi->Size  = needSize;
        mi->Count = 1U;
        PKSJACK_DESCRIPTION jd = (PKSJACK_DESCRIPTION)(mi + 1);
        jd->ChannelMapping = KSAUDIO_SPEAKER_STEREO;
        jd->Color          = 0U;
        jd->ConnectionType = eConnTypeUnknown;
        jd->GeoLocation    = eGeoLocBottom;
        jd->GenLocation    = eGenLocInternal;
        jd->PortConnection = ePortConnIntegratedDevice;
        jd->IsConnected    = TRUE;
        Req->ValueSize     = needSize;
        return STATUS_SUCCESS;
    }
    return STATUS_INVALID_DEVICE_REQUEST;
}

static const PCPROPERTY_ITEM g_TopoVolProps[] = {
    {
        &g_GuidPropAudio,
        KSPROPERTY_AUDIO_VOLUMELEVEL,
        PCPROPERTY_ITEM_FLAG_GET | PCPROPERTY_ITEM_FLAG_SET | PCPROPERTY_ITEM_FLAG_BASICSUPPORT,
        TopoPropVolume
    },
    {
        &g_GuidPropAudio,
        KSPROPERTY_AUDIO_CPU_RESOURCES,
        PCPROPERTY_ITEM_FLAG_GET | PCPROPERTY_ITEM_FLAG_BASICSUPPORT,
        TopoPropCpuResources
    }
};

static const PCAUTOMATION_TABLE g_TopoVolAuto = {
    sizeof(PCPROPERTY_ITEM), RTL_NUMBER_OF(g_TopoVolProps), g_TopoVolProps,
    0, 0, NULL, 0, 0, NULL, 0
};

static const PCPROPERTY_ITEM g_TopoMuteProps[] = {
    {
        &g_GuidPropAudio,
        KSPROPERTY_AUDIO_MUTE,
        PCPROPERTY_ITEM_FLAG_GET | PCPROPERTY_ITEM_FLAG_SET | PCPROPERTY_ITEM_FLAG_BASICSUPPORT,
        TopoPropMute
    },
    {
        &g_GuidPropAudio,
        KSPROPERTY_AUDIO_CPU_RESOURCES,
        PCPROPERTY_ITEM_FLAG_GET | PCPROPERTY_ITEM_FLAG_BASICSUPPORT,
        TopoPropCpuResources
    }
};

static const PCAUTOMATION_TABLE g_TopoMuteAuto = {
    sizeof(PCPROPERTY_ITEM), RTL_NUMBER_OF(g_TopoMuteProps), g_TopoMuteProps,
    0, 0, NULL, 0, 0, NULL, 0
};

static const PCPROPERTY_ITEM g_TopoFilterProps[] = {
    {
        &g_GuidPropJack,
        KSPROPERTY_JACK_DESCRIPTION,
        PCPROPERTY_ITEM_FLAG_GET | PCPROPERTY_ITEM_FLAG_BASICSUPPORT,
        TopoPropJackDescription
    }
};

static const PCAUTOMATION_TABLE g_TopoFilterAuto = {
    sizeof(PCPROPERTY_ITEM), RTL_NUMBER_OF(g_TopoFilterProps), g_TopoFilterProps,
    0, 0, NULL, 0, 0, NULL, 0
};

static const PCPIN_DESCRIPTOR g_TopoPins[] = {
    /* Pin 0: Bridge In from WaveRT Pin 1 */
    {
        0, 0, 0,
        NULL,
        {
            0, NULL, 0, NULL,
            RTL_NUMBER_OF(g_AnalogBridgeRanges), g_AnalogBridgeRanges,
            KSPIN_DATAFLOW_IN,
            KSPIN_COMMUNICATION_NONE,
            &g_GuidCatAudio,
            NULL,
            0
        }
    },
    /* Pin 1: Physical Stereo Speaker Out */
    {
        0, 0, 0,
        NULL,
        {
            0, NULL, 0, NULL,
            RTL_NUMBER_OF(g_AnalogBridgeRanges), g_AnalogBridgeRanges,
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_NONE,
            &g_GuidNodeSpeaker,
            NULL,
            0
        }
    }
};

static const PCNODE_DESCRIPTOR g_TopoNodes[] = {
    { 0, &g_TopoVolAuto,  &g_GuidNodeVolume, NULL },
    { 0, &g_TopoMuteAuto, &g_GuidNodeMute,   NULL }
};

static const PCCONNECTION_DESCRIPTOR g_TopoConnections[] = {
    { PCFILTER_NODE, 0, 0, KSNODEPIN_STANDARD_IN },
    { 0, KSNODEPIN_STANDARD_OUT, 1, KSNODEPIN_STANDARD_IN },
    { 1, KSNODEPIN_STANDARD_OUT, PCFILTER_NODE, 1 }
};

static const GUID g_TopoCategories[] = {
    { STATIC_KSCATEGORY_AUDIO },
    { STATIC_KSCATEGORY_TOPOLOGY }
};

static PCFILTER_DESCRIPTOR g_TopoFilterDesc = {
    0,
    &g_TopoFilterAuto,
    sizeof(PCPIN_DESCRIPTOR),
    RTL_NUMBER_OF(g_TopoPins),
    g_TopoPins,
    sizeof(PCNODE_DESCRIPTOR),
    RTL_NUMBER_OF(g_TopoNodes),
    g_TopoNodes,
    RTL_NUMBER_OF(g_TopoConnections),
    g_TopoConnections,
    RTL_NUMBER_OF(g_TopoCategories),
    g_TopoCategories
};

class CS23TopologyMiniport : public CUnknownBase<CS23TopologyMiniport, IMiniportTopology> {
private:
    PPORTTOPOLOGY m_Port;

public:
    CS23TopologyMiniport() : m_Port(NULL) {}
    ~CS23TopologyMiniport() {
        if (m_Port) { m_Port->Release(); m_Port = NULL; }
    }

    bool MatchesIID(REFIID iid) const {
        return IsEqualGUIDAligned(iid, IID_IMiniport) ||
               IsEqualGUIDAligned(iid, IID_IMiniportTopology);
    }

    IMP_IMiniportTopology;
};

STDMETHODIMP_(NTSTATUS) CS23TopologyMiniport::Init(
    _In_ PUNKNOWN      UnknownAdapter,
    _In_ PRESOURCELIST ResourceList,
    _In_ PPORTTOPOLOGY Port
) {
    UNREFERENCED_PARAMETER(UnknownAdapter);
    UNREFERENCED_PARAMETER(ResourceList);
    if (!Port) return STATUS_INVALID_PARAMETER;
    m_Port = Port;
    m_Port->AddRef();
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CS23TopologyMiniport::GetDescription(_Out_ PPCFILTER_DESCRIPTOR *Description) {
    if (!Description) return STATUS_INVALID_PARAMETER;
    *Description = &g_TopoFilterDesc;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CS23TopologyMiniport::DataRangeIntersection(
    _In_  ULONG        PinId,
    _In_  PKSDATARANGE DataRange,
    _In_  PKSDATARANGE MatchingDataRange,
    _In_  ULONG        OutputBufferLength,
    _Out_writes_bytes_to_opt_(OutputBufferLength, *ResultantFormatLength) PVOID ResultantFormat,
    _Out_ PULONG       ResultantFormatLength
) {
    UNREFERENCED_PARAMETER(PinId);
    UNREFERENCED_PARAMETER(DataRange);
    UNREFERENCED_PARAMETER(MatchingDataRange);
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(ResultantFormat);
    UNREFERENCED_PARAMETER(ResultantFormatLength);
    return STATUS_NOT_IMPLEMENTED;
}

/* ============================================================================
 * Adapter StartDevice, AddDevice, PnP Cleanup, and DriverEntry
 * ========================================================================== */
static NTSTATUS S23AudioStartDevice(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP           Irp,
    _In_ PRESOURCELIST  ResourceList
) {
    UNREFERENCED_PARAMETER(Irp);
    NTSTATUS st = STATUS_SUCCESS;

    if (!g_Hw) {
        st = S23AudHwCreate(&g_Hw);
        if (!NT_SUCCESS(st)) {
            return st;
        }
    }

    PPORT pWavePort = NULL;
    PPORT pTopoPort = NULL;
    CS23WaveRTMiniport   *pWaveMini = NULL;
    CS23TopologyMiniport *pTopoMini = NULL;

    /* 1. WaveRT subdevice ("WaveSpeaker") */
    st = PcNewPort(&pWavePort, CLSID_PortWaveRT);
    if (!NT_SUCCESS(st)) goto cleanup;
    pWaveMini = CS23WaveRTMiniport::Create(g_Hw);
    if (!pWaveMini) { st = STATUS_INSUFFICIENT_RESOURCES; goto cleanup; }
    st = pWavePort->Init(DeviceObject, Irp, pWaveMini, NULL, ResourceList);
    if (!NT_SUCCESS(st)) goto cleanup;
    st = PcRegisterSubdevice(DeviceObject, L"WaveSpeaker", pWavePort);
    if (!NT_SUCCESS(st)) goto cleanup;

    /* 2. Topology subdevice ("TopoSpeaker") */
    st = PcNewPort(&pTopoPort, CLSID_PortTopology);
    if (!NT_SUCCESS(st)) goto cleanup;
    pTopoMini = CS23TopologyMiniport::Create();
    if (!pTopoMini) { st = STATUS_INSUFFICIENT_RESOURCES; goto cleanup; }
    st = pTopoPort->Init(DeviceObject, Irp, pTopoMini, NULL, ResourceList);
    if (!NT_SUCCESS(st)) goto cleanup;
    st = PcRegisterSubdevice(DeviceObject, L"TopoSpeaker", pTopoPort);
    if (!NT_SUCCESS(st)) goto cleanup;

    /* 3. Physical connection: WaveSpeaker Pin 1 (bridge out) -> TopoSpeaker Pin 0 (bridge in) */
    st = PcRegisterPhysicalConnection(DeviceObject, pWavePort, 1U, pTopoPort, 0U);
    if (!NT_SUCCESS(st)) goto cleanup;

    S23AudLogFmt("[DRV] StartDevice registered WaveSpeaker + TopoSpeaker + PhysicalConnection OK!\r\n");
    S23AudLogFlush();

cleanup:
    if (pTopoMini) pTopoMini->Release();
    if (pWaveMini) pWaveMini->Release();
    if (pTopoPort) pTopoPort->Release();
    if (pWavePort) pWavePort->Release();
    if (!NT_SUCCESS(st)) {
        S23AudLogFmt("[DRV] StartDevice failed st=0x%08x\r\n", st);
        S23AudLogFlush();
    }
    return st;
}

static NTSTATUS S23AudioAddDevice(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PDEVICE_OBJECT PhysicalDeviceObject
) {
    return PcAddAdapterDevice(DriverObject, PhysicalDeviceObject, S23AudioStartDevice, 8U, 0U);
}

static NTSTATUS S23AudioPnpDispatch(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP           Irp
) {
    PIO_STACK_LOCATION isl = IoGetCurrentIrpStackLocation(Irp);
    UCHAR minor = isl ? isl->MinorFunction : 0U;
    NTSTATUS st = g_PcPnpDispatch ? g_PcPnpDispatch(DeviceObject, Irp) : PcDispatchIrp(DeviceObject, Irp);
    if (minor == IRP_MN_REMOVE_DEVICE && g_Hw) {
        S23AudHwDestroy(g_Hw);
        g_Hw = NULL;
    }
    return st;
}

static VOID S23AudioUnload(_In_ PDRIVER_OBJECT DriverObject) {
    if (g_Hw) {
        S23AudHwDestroy(g_Hw);
        g_Hw = NULL;
    }
    if (g_PcUnload) {
        g_PcUnload(DriverObject);
    }
}

extern "C" NTSTATUS DriverEntry(
    _In_ PDRIVER_OBJECT  DriverObject,
    _In_ PUNICODE_STRING RegistryPath
) {
    NTSTATUS st = PcInitializeAdapterDriver(DriverObject, RegistryPath, S23AudioAddDevice);
    if (NT_SUCCESS(st)) {
        g_PcPnpDispatch = DriverObject->MajorFunction[IRP_MJ_PNP];
        DriverObject->MajorFunction[IRP_MJ_PNP] = S23AudioPnpDispatch;
        g_PcUnload = DriverObject->DriverUnload;
        DriverObject->DriverUnload = S23AudioUnload;
    }
    return st;
}
