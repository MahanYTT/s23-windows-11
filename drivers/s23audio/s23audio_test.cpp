/*
 * s23audio_test.cpp - WASAPI Shared-Mode Render & IAudioClock Verification for S23 Speakers
 */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propkey.h>
#include <avrt.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>

static constexpr uint32_t kSampleRate     = 48000;
static constexpr uint16_t kChannels       = 2;
static constexpr uint16_t kBitsPerSample  = 16;
static constexpr double   kToneHz         = 480.0;
static constexpr double   kDurationSec    = 2.0;
static constexpr uint64_t kTargetFrames   = static_cast<uint64_t>(kSampleRate * kDurationSec);
static constexpr uint64_t kMinClockFrames = 80000;
static constexpr double   kAmpMinus20dBFS = 0.1 * 32767.0;
static constexpr double   kTwoPi          = 6.283185307179586476925286766559;

static bool ContainsInsensitive(const wchar_t *haystack, const wchar_t *needle)
{
    if (!haystack || !needle || !*needle) return false;
    size_t nlen = wcslen(needle);
    for (const wchar_t *p = haystack; *p; ++p) {
        if (_wcsnicmp(p, needle, nlen) == 0) return true;
    }
    return false;
}

static void PrintWaveFormat(const char *label, const WAVEFORMATEX *wfx)
{
    if (!wfx) return;
    printf("%s: tag=0x%04x ch=%u rate=%lu bits=%u blockAlign=%u avgBytes=%lu cbSize=%u",
           label, wfx->wFormatTag, wfx->nChannels, wfx->nSamplesPerSec,
           wfx->wBitsPerSample, wfx->nBlockAlign, wfx->nAvgBytesPerSec, wfx->cbSize);
    if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wfx->cbSize >= 22) {
        const auto *ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(wfx);
        const char *sub = "OTHER";
        if (IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_PCM))             sub = "PCM";
        else if (IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) sub = "IEEE_FLOAT";
        printf(" [EXT validBits=%u mask=0x%lx sub=%s]",
               ext->Samples.wValidBitsPerSample, ext->dwChannelMask, sub);
    }
    printf("\n");
}

static HRESULT FillToneFrames(IAudioRenderClient *renderClient, UINT32 toWrite, uint64_t &framesWritten)
{
    BYTE *data = nullptr;
    HRESULT r = renderClient->GetBuffer(toWrite, &data);
    if (FAILED(r)) return r;
    auto *pcm = reinterpret_cast<int16_t *>(data);
    for (UINT32 f = 0; f < toWrite; ++f) {
        double phase = kTwoPi * kToneHz * static_cast<double>(framesWritten + f) / static_cast<double>(kSampleRate);
        int16_t s = static_cast<int16_t>(std::lround(kAmpMinus20dBFS * std::sin(phase)));
        pcm[2 * f]     = s;
        pcm[2 * f + 1] = s;
    }
    r = renderClient->ReleaseBuffer(toWrite, 0);
    if (SUCCEEDED(r)) framesWritten += toWrite;
    return r;
}

int wmain(int argc, wchar_t **argv)
{
    const wchar_t *matchSubstr = L"S23";
    int forceIndex = -1;
    bool listOnly = false;
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"--list") == 0) {
            listOnly = true;
        } else if (_wcsicmp(argv[i], L"--match") == 0 && i + 1 < argc) {
            matchSubstr = argv[++i];
        } else if (_wcsicmp(argv[i], L"--index") == 0 && i + 1 < argc) {
            forceIndex = _wtoi(argv[++i]);
        }
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        printf("[FAIL] CoInitializeEx hr=0x%08lx\n", (unsigned long)hr);
        return 1;
    }

    int rc = 1;
    IMMDeviceEnumerator *enumerator   = nullptr;
    IMMDeviceCollection *collection   = nullptr;
    IMMDevice           *selectedDev  = nullptr;
    IAudioClient        *audioClient  = nullptr;
    IAudioRenderClient  *renderClient = nullptr;
    IAudioClock         *audioClock   = nullptr;
    WAVEFORMATEX        *mixFmt       = nullptr;
    WAVEFORMATEX        *closestFmt   = nullptr;
    HANDLE               mmcssHandle  = nullptr;
    WAVEFORMATEX         reqFmt       = {};
    DWORD                streamFlags  = 0;
    UINT32               bufferFrames = 0;
    UINT64               clockFreq    = 0;
    UINT64               posBefore    = 0;
    UINT64               qpcBefore    = 0;
    UINT64               posAfter     = 0;
    UINT64               qpcAfter     = 0;
    uint64_t             framesWritten = 0;
    UINT32               initialFill  = 0;
    ULONGLONG            tStartMs     = 0;
    ULONGLONG            nextLogMs    = 0;
    UINT                 count        = 0;

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator), reinterpret_cast<void **>(&enumerator));
    if (FAILED(hr)) {
        printf("[FAIL] CoCreateInstance(MMDeviceEnumerator) hr=0x%08lx\n", (unsigned long)hr);
        goto cleanup;
    }

    hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr)) {
        printf("[FAIL] EnumAudioEndpoints hr=0x%08lx\n", (unsigned long)hr);
        goto cleanup;
    }

    hr = collection->GetCount(&count);
    if (FAILED(hr)) {
        printf("[FAIL] IMMDeviceCollection::GetCount hr=0x%08lx\n", (unsigned long)hr);
        goto cleanup;
    }
    printf("Active eRender endpoints: %u\n", count);

    for (UINT i = 0; i < count; ++i) {
        IMMDevice *dev = nullptr;
        if (FAILED(collection->Item(i, &dev)) || !dev) continue;

        LPWSTR devId = nullptr;
        dev->GetId(&devId);

        IPropertyStore *props = nullptr;
        PROPVARIANT pv;
        PropVariantInit(&pv);
        const wchar_t *friendly = L"<unknown>";
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props)) && props) {
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) {
                friendly = pv.pwszVal;
            }
        }
        wprintf(L"  [%u] Name=\"%ls\" ID=%ls\n", i, friendly, devId ? devId : L"<null>");

        bool pick = (forceIndex >= 0) ? (static_cast<int>(i) == forceIndex)
                                      : (!selectedDev && ContainsInsensitive(friendly, matchSubstr));
        if (pick) {
            dev->AddRef();
            selectedDev = dev;
        }

        PropVariantClear(&pv);
        if (props) props->Release();
        if (devId) CoTaskMemFree(devId);
        dev->Release();
    }

    if (listOnly) {
        rc = (count > 0) ? 0 : 1;
        goto cleanup;
    }

    if (!selectedDev && forceIndex < 0) {
        wprintf(L"No endpoint matched \"%ls\"; falling back to default eRender/eConsole endpoint\n", matchSubstr);
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &selectedDev);
        if (FAILED(hr)) {
            printf("[FAIL] GetDefaultAudioEndpoint hr=0x%08lx\n", (unsigned long)hr);
            goto cleanup;
        }
    } else if (!selectedDev) {
        printf("[FAIL] Requested endpoint index %d not found\n", forceIndex);
        goto cleanup;
    }

    {
        LPWSTR selId = nullptr;
        selectedDev->GetId(&selId);
        wprintf(L"Selected endpoint ID: %ls\n", selId ? selId : L"<null>");
        if (selId) CoTaskMemFree(selId);
    }

    hr = selectedDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                               reinterpret_cast<void **>(&audioClient));
    if (FAILED(hr)) {
        printf("[FAIL] IMMDevice::Activate(IAudioClient) hr=0x%08lx\n", (unsigned long)hr);
        goto cleanup;
    }

    hr = audioClient->GetMixFormat(&mixFmt);
    if (FAILED(hr)) {
        printf("[FAIL] GetMixFormat hr=0x%08lx\n", (unsigned long)hr);
        goto cleanup;
    }
    PrintWaveFormat("MixFormat", mixFmt);

    reqFmt.wFormatTag      = WAVE_FORMAT_PCM;
    reqFmt.nChannels       = kChannels;
    reqFmt.nSamplesPerSec  = kSampleRate;
    reqFmt.wBitsPerSample  = kBitsPerSample;
    reqFmt.nBlockAlign     = static_cast<WORD>((kChannels * kBitsPerSample) / 8);
    reqFmt.nAvgBytesPerSec = kSampleRate * reqFmt.nBlockAlign;
    reqFmt.cbSize          = 0;
    PrintWaveFormat("ReqFormat", &reqFmt);

    streamFlags = 0;
    hr = audioClient->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED, &reqFmt, &closestFmt);
    if (hr == S_OK) {
        printf("IsFormatSupported: S_OK (direct PCM, streamFlags=0)\n");
    } else {
        printf("IsFormatSupported: hr=0x%08lx -> enabling AUTOCONVERTPCM | SRC_DEFAULT_QUALITY\n",
               (unsigned long)hr);
        if (closestFmt) PrintWaveFormat("ClosestMatch", closestFmt);
        streamFlags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    }

    static constexpr REFERENCE_TIME kBufferDuration100ns = 1000000; /* 100 ms */
    hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, streamFlags,
                                 kBufferDuration100ns, 0, &reqFmt, nullptr);
    if (FAILED(hr)) {
        printf("[FAIL] IAudioClient::Initialize hr=0x%08lx\n", (unsigned long)hr);
        goto cleanup;
    }

    hr = audioClient->GetBufferSize(&bufferFrames);
    if (FAILED(hr) || bufferFrames == 0) {
        printf("[FAIL] GetBufferSize hr=0x%08lx frames=%u\n", (unsigned long)hr, bufferFrames);
        goto cleanup;
    }
    printf("BufferSize: %u frames (%.1f ms)\n", bufferFrames, 1000.0 * bufferFrames / kSampleRate);

    hr = audioClient->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void **>(&renderClient));
    if (FAILED(hr)) {
        printf("[FAIL] GetService(IAudioRenderClient) hr=0x%08lx\n", (unsigned long)hr);
        goto cleanup;
    }
    hr = audioClient->GetService(__uuidof(IAudioClock), reinterpret_cast<void **>(&audioClock));
    if (FAILED(hr)) {
        printf("[FAIL] GetService(IAudioClock) hr=0x%08lx\n", (unsigned long)hr);
        goto cleanup;
    }

    hr = audioClock->GetFrequency(&clockFreq);
    if (FAILED(hr) || clockFreq == 0) {
        printf("[FAIL] IAudioClock::GetFrequency hr=0x%08lx freq=%llu\n", (unsigned long)hr, clockFreq);
        goto cleanup;
    }
    printf("AudioClock frequency: %llu ticks/sec\n", clockFreq);

    {
        DWORD taskIdx = 0;
        mmcssHandle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIdx);
    }

    hr = audioClock->GetPosition(&posBefore, &qpcBefore);
    if (FAILED(hr)) {
        printf("[FAIL] GetPosition(before) hr=0x%08lx\n", (unsigned long)hr);
        goto cleanup;
    }
    printf("Clock BEFORE: pos=%llu qpc=%llu\n", posBefore, qpcBefore);

    initialFill = (bufferFrames < kTargetFrames) ? bufferFrames : static_cast<UINT32>(kTargetFrames);
    hr = FillToneFrames(renderClient, initialFill, framesWritten);
    if (FAILED(hr)) {
        printf("[FAIL] Initial fill hr=0x%08lx\n", (unsigned long)hr);
        goto cleanup;
    }

    hr = audioClient->Start();
    if (FAILED(hr)) {
        printf("[FAIL] IAudioClient::Start hr=0x%08lx\n", (unsigned long)hr);
        goto cleanup;
    }

    tStartMs = GetTickCount64();
    nextLogMs = tStartMs + 500;
    static constexpr ULONGLONG kRenderTimeoutMs = 5000;

    while (framesWritten < kTargetFrames) {
        Sleep(10);
        UINT32 padding = 0;
        hr = audioClient->GetCurrentPadding(&padding);
        if (FAILED(hr)) {
            printf("[FAIL] GetCurrentPadding hr=0x%08lx\n", (unsigned long)hr);
            goto cleanup;
        }
        UINT32 avail = (bufferFrames > padding) ? (bufferFrames - padding) : 0;
        uint64_t rem = kTargetFrames - framesWritten;
        UINT32 chunk = (avail < rem) ? avail : static_cast<UINT32>(rem);
        if (chunk > 0) {
            hr = FillToneFrames(renderClient, chunk, framesWritten);
            if (FAILED(hr)) {
                printf("[FAIL] Render chunk hr=0x%08lx\n", (unsigned long)hr);
                goto cleanup;
            }
        }
        ULONGLONG nowMs = GetTickCount64();
        if (nowMs >= nextLogMs) {
            UINT64 posMid = 0, qpcMid = 0;
            if (SUCCEEDED(audioClock->GetPosition(&posMid, &qpcMid))) {
                uint64_t midFrames = static_cast<uint64_t>(
                    (static_cast<double>(posMid - posBefore) * kSampleRate) / static_cast<double>(clockFreq));
                printf("Clock DURING (%llums): written=%llu padding=%u pos=%llu (~%llu frames)\n",
                       nowMs - tStartMs, framesWritten, padding, posMid, midFrames);
            }
            nextLogMs = nowMs + 500;
        }
        if (nowMs - tStartMs > kRenderTimeoutMs) {
            printf("[FAIL] Timed out rendering (stalled buffer? written=%llu/%llu padding=%u)\n",
                   framesWritten, kTargetFrames, padding);
            goto cleanup;
        }
    }

    for (int i = 0; i < 50; ++i) {
        UINT32 padding = 0;
        if (FAILED(audioClient->GetCurrentPadding(&padding)) || padding == 0) break;
        Sleep(10);
    }

    hr = audioClock->GetPosition(&posAfter, &qpcAfter);
    if (FAILED(hr)) {
        printf("[FAIL] GetPosition(after) hr=0x%08lx\n", (unsigned long)hr);
        goto cleanup;
    }

    hr = audioClient->Stop();
    if (FAILED(hr)) {
        printf("[FAIL] IAudioClient::Stop hr=0x%08lx\n", (unsigned long)hr);
        goto cleanup;
    }

    {
        UINT64 deltaTicks = (posAfter >= posBefore) ? (posAfter - posBefore) : 0;
        double elapsedSec = static_cast<double>(deltaTicks) / static_cast<double>(clockFreq);
        uint64_t framesAdvanced = static_cast<uint64_t>(elapsedSec * kSampleRate);
        printf("Clock AFTER:  pos=%llu qpc=%llu deltaTicks=%llu (%.3f s, %llu frames)\n",
               posAfter, qpcAfter, deltaTicks, elapsedSec, framesAdvanced);
        printf("Summary: framesWritten=%llu/%llu clockFramesAdvanced=%llu (threshold>%llu)\n",
               framesWritten, kTargetFrames, framesAdvanced, kMinClockFrames);

        if (framesWritten == kTargetFrames && framesAdvanced > kMinClockFrames) {
            printf("WASAPI VERIFICATION: PASS\n");
            rc = 0;
        } else {
            printf("WASAPI VERIFICATION: FAIL\n");
            rc = 1;
        }
    }

cleanup:
    if (mmcssHandle)  AvRevertMmThreadCharacteristics(mmcssHandle);
    if (closestFmt)   CoTaskMemFree(closestFmt);
    if (mixFmt)       CoTaskMemFree(mixFmt);
    if (audioClock)   audioClock->Release();
    if (renderClient) renderClient->Release();
    if (audioClient)  audioClient->Release();
    if (selectedDev)  selectedDev->Release();
    if (collection)   collection->Release();
    if (enumerator)   enumerator->Release();
    CoUninitialize();
    return rc;
}
