// capture.cpp — WASAPI loopback + mic capture with single worker thread + inline mixer
// Falls back to WaveIn API if WASAPI is blocked (e.g. by corporate DLP software).
#define INITGUID   // define COM GUIDs in this TU
#include "winrec.h"

#include <mmdeviceapi.h>
#include <audioclient.h>
#include <initguid.h>
#include <cguid.h>
#include <ks.h>
#include <ksmedia.h>
#include <mmsystem.h>
#include <propsys.h>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <algorithm>
#include <vector>

// ---------------------------------------------------------------------------
// GUIDs (define them here so INITGUID picks them up)
// ---------------------------------------------------------------------------

DEFINE_GUID(IID_IClassFactory_,
    0x00000001, 0x0000, 0x0000, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46);
DEFINE_GUID(CLSID_MMDeviceEnumerator_,
    0xBCDE0395, 0xE52F, 0x467C, 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E);
DEFINE_GUID(IID_IMMDeviceEnumerator_,
    0xA95664D2, 0x9614, 0x4F35, 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x38, 0x17, 0xE6);
DEFINE_GUID(IID_IAudioClient_,
    0x1CB9AD4C, 0xDBFA, 0x4C32, 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2);
DEFINE_GUID(IID_IAudioCaptureClient_,
    0xC8ADBD64, 0xE71E, 0x48A0, 0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17);

static const GUID KSDATAFORMAT_SUBTYPE_PCM_ =
    {0x00000001,0x0000,0x0010,{0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71}};
static const GUID KSDATAFORMAT_SUBTYPE_IEEE_FLOAT_ =
    {0x00000003,0x0000,0x0010,{0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71}};

// PKEY_Device_FriendlyName: {a45c254e-df1c-4efd-8020-67d146a850e0} pid=14
static const PROPERTYKEY PKEY_Device_FriendlyName_ = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

static inline bool GuidEq(const GUID& a, const GUID& b) {
    return memcmp(&a, &b, sizeof(GUID)) == 0;
}

// ---------------------------------------------------------------------------
// Shared state (capture thread ↔ main thread)
// ---------------------------------------------------------------------------

UINT32 g_captureInputRate = 48000;   // exported for NormParams

static std::atomic<bool> g_doCapture{false};
static HANDLE            g_captureThread = nullptr;

// WASAPI objects (created before thread, released after)
static IMMDeviceEnumerator* g_pEnum     = nullptr;
static IMMDevice*           g_pRender   = nullptr;
static IMMDevice*           g_pCapture  = nullptr;
static IAudioClient*        g_pLbClient = nullptr;
static IAudioClient*        g_pMicClient= nullptr;
static IAudioCaptureClient* g_pLbCap    = nullptr;
static IAudioCaptureClient* g_pMicCap   = nullptr;

static WAVEFORMATEX*   g_pLbFmt  = nullptr;
static WAVEFORMATEX*   g_pMicFmt = nullptr;

static HWND  g_captureHwnd = nullptr;
static FILE* g_rawFile      = nullptr;


// ---------------------------------------------------------------------------
// Format helpers
// ---------------------------------------------------------------------------

struct FmtInfo {
    UINT32 sampleRate;
    UINT16 channels;
    UINT16 bitsPerSample;
    bool   isFloat;
};

static FmtInfo ParseFormat(const WAVEFORMATEX* wfx)
{
    FmtInfo fi = {};
    fi.sampleRate    = wfx->nSamplesPerSec;
    fi.channels      = wfx->nChannels;
    fi.bitsPerSample = wfx->wBitsPerSample;

    if (wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        fi.isFloat = true;
    } else if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const auto* wex = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wfx);
        fi.isFloat = GuidEq(wex->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT_);
        fi.bitsPerSample = wex->Samples.wValidBitsPerSample;
    }
    return fi;
}

// Convert a raw audio buffer to a float mono sample array
// Returns number of mono frames written into |out|.
static size_t ToFloatMono(const BYTE* data, UINT32 frames,
                           const FmtInfo& fi, std::vector<float>& out)
{
    out.resize(frames);
    for (UINT32 i = 0; i < frames; ++i) {
        float mono = 0.0f;
        for (UINT16 ch = 0; ch < fi.channels; ++ch) {
            float s = 0.0f;
            size_t byteOffset = ((size_t)i * fi.channels + ch) *
                                 (fi.bitsPerSample / 8u);
            if (fi.isFloat && fi.bitsPerSample == 32) {
                float tmp;
                memcpy(&tmp, data + byteOffset, 4);
                s = tmp;
            } else if (!fi.isFloat && fi.bitsPerSample == 16) {
                int16_t tmp;
                memcpy(&tmp, data + byteOffset, 2);
                s = tmp / 32768.0f;
            } else if (!fi.isFloat && fi.bitsPerSample == 32) {
                int32_t tmp;
                memcpy(&tmp, data + byteOffset, 4);
                s = tmp / 2147483648.0f;
            }
            mono += s;
        }
        mono /= fi.channels;
        out[i] = mono;
    }
    return frames;
}

// ---------------------------------------------------------------------------
// Ring buffer (simple FIFO using std::vector)
// ---------------------------------------------------------------------------

struct RingBuf {
    std::vector<float> data;
    size_t             head = 0;  // read  pos
    size_t             tail = 0;  // write pos
    size_t             count = 0;

    void push(const std::vector<float>& v, size_t n) {
        if (data.size() == 0) data.resize(65536, 0.0f);
        for (size_t i = 0; i < n; ++i) {
            data[tail % data.size()] = v[i];
            ++tail; ++count;
        }
        if (count > data.size()) {
            // overrun: drop oldest
            head  = tail - data.size();
            count = data.size();
        }
    }

    float pop() {
        if (count == 0) return 0.0f;
        float v = data[head % data.size()];
        ++head; --count;
        return v;
    }

    bool empty() const { return count == 0; }
    size_t size() const { return count; }
};

// ---------------------------------------------------------------------------
// WaveIn fallback state  (placed after RingBuf so WvDev can embed one)
// ---------------------------------------------------------------------------

bool g_usingWaveIn = false;  // exported

static const int WV_BUF_COUNT = 4;
static const int WV_BUF_MS    = 50;   // ms per WaveIn buffer

struct WvDev {
    HWAVEIN            hWave  = nullptr;
    WAVEFORMATEX       fmt    = {};
    bool               stereo = false;
    WAVEHDR            hdrs[WV_BUF_COUNT];
    std::vector<BYTE>  raw[WV_BUF_COUNT];
    CRITICAL_SECTION   cs;
    RingBuf            ring;
};

static const int MAX_LOOPS = 4;
static WvDev  g_wvMic;
static WvDev  g_wvLoops[MAX_LOOPS];
int           g_numLoops = 0;  // exported

// ---------------------------------------------------------------------------
// Worker thread
// ---------------------------------------------------------------------------

static DWORD WINAPI CaptureWorker(LPVOID)
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    FmtInfo lbFi  = ParseFormat(g_pLbFmt);
    FmtInfo micFi = g_pMicFmt ? ParseFormat(g_pMicFmt) : FmtInfo{};

    g_captureInputRate = lbFi.sampleRate;

    // ---- local refs so worker owns its lifetime ----
    IAudioClient*        pLbClient  = g_pLbClient;
    IAudioClient*        pMicClient = g_pMicClient;
    IAudioCaptureClient* pLbCap     = g_pLbCap;
    IAudioCaptureClient* pMicCap    = g_pMicCap;

    pLbClient->Start();
    if (pMicClient) pMicClient->Start();

    RingBuf lbBuf, micBuf;
    std::vector<float> tmp;

    bool ok = true;
    while (g_doCapture.load()) {
        // --- Drain loopback ---
        {
            UINT32 pkSz = 0;
            while (SUCCEEDED(pLbCap->GetNextPacketSize(&pkSz)) && pkSz > 0) {
                BYTE*  pData  = nullptr;
                UINT32 frames = 0;
                DWORD  flags  = 0;
                HRESULT hr = pLbCap->GetBuffer(&pData, &frames, &flags, nullptr, nullptr);
                if (FAILED(hr)) break;
                if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && frames > 0) {
                    ToFloatMono(pData, frames, lbFi, tmp);
                    lbBuf.push(tmp, frames);
                } else {
                    tmp.assign(frames, 0.0f);
                    lbBuf.push(tmp, frames);
                }
                pLbCap->ReleaseBuffer(frames);
                if (FAILED(pLbCap->GetNextPacketSize(&pkSz))) break;
            }
        }

        // --- Drain mic (or push matching silence if no mic) ---
        if (pMicCap) {
            UINT32 pkSz = 0;
            while (SUCCEEDED(pMicCap->GetNextPacketSize(&pkSz)) && pkSz > 0) {
                BYTE*  pData  = nullptr;
                UINT32 frames = 0;
                DWORD  flags  = 0;
                HRESULT hr = pMicCap->GetBuffer(&pData, &frames, &flags, nullptr, nullptr);
                if (FAILED(hr)) break;
                if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && frames > 0) {
                    ToFloatMono(pData, frames, micFi, tmp);
                    micBuf.push(tmp, frames);
                } else {
                    tmp.assign(frames, 0.0f);
                    micBuf.push(tmp, frames);
                }
                pMicCap->ReleaseBuffer(frames);
                if (FAILED(pMicCap->GetNextPacketSize(&pkSz))) break;
            }
        } else {
            // No mic: push silence to match however much loopback data arrived
            if (lbBuf.size() > micBuf.size()) {
                size_t needed = lbBuf.size() - micBuf.size();
                tmp.assign(needed, 0.0f);
                micBuf.push(tmp, needed);
            }
        }

        // --- Mix and write ---
        size_t avail = std::min(lbBuf.size(), micBuf.size());
        if (avail > 0) {
            std::vector<int16_t> out(avail);
            for (size_t i = 0; i < avail; ++i) {
                float lb  = lbBuf.pop();
                float mic = micBuf.pop();
                float mixed = 0.5f * lb + 0.5f * mic;
                // Clamp
                if (mixed >  1.0f) mixed =  1.0f;
                if (mixed < -1.0f) mixed = -1.0f;
                out[i] = (int16_t)roundf(mixed * 32767.0f);
            }
            if (g_rawFile) {
                if (fwrite(out.data(), sizeof(int16_t), avail, g_rawFile) != avail)
                    ok = false;
            }
        }

        Sleep(10);
    }

    pLbClient->Stop();
    if (pMicClient) pMicClient->Stop();

    if (g_rawFile) { fclose(g_rawFile); g_rawFile = nullptr; }

    // Release WASAPI COM objects owned by this thread
    if (pMicCap)    pMicCap->Release();
    pLbCap->Release();
    if (pMicClient) pMicClient->Release();
    pLbClient->Release();
    // Render/Capture device and enumerator released by CaptureStop() on main thread
    // (safe: CaptureStop waits for thread via the handle being closed before releasing those)

    CoUninitialize();
    PostMessageW(g_captureHwnd, WM_APP_RECORDING_DONE, ok ? 0 : 1, 0);
    return 0;
}

// ---------------------------------------------------------------------------
// WaveIn fallback implementation
// ---------------------------------------------------------------------------

static void CALLBACK WvCallback(HWAVEIN /*hwi*/, UINT msg, DWORD_PTR instance,
                                 DWORD_PTR param1, DWORD_PTR /*param2*/)
{
    if (msg != WIM_DATA) return;
    WvDev*   dev = reinterpret_cast<WvDev*>(instance);
    WAVEHDR* hdr = reinterpret_cast<WAVEHDR*>(param1);

    if (hdr->dwBytesRecorded > 0) {
        int    nCh    = dev->fmt.nChannels;
        size_t frames = hdr->dwBytesRecorded / (2u * (size_t)nCh);
        const int16_t* pcm = reinterpret_cast<const int16_t*>(hdr->lpData);

        std::vector<float> tmp(frames);
        for (size_t i = 0; i < frames; ++i) {
            float s = 0.0f;
            for (int ch = 0; ch < nCh; ++ch)
                s += pcm[i * nCh + ch] / 32768.0f;
            tmp[i] = s / nCh;
        }

        EnterCriticalSection(&dev->cs);
        dev->ring.push(tmp, frames);
        LeaveCriticalSection(&dev->cs);
    }

    hdr->dwBytesRecorded = 0;
    if (g_doCapture.load())
        waveInAddBuffer(dev->hWave, hdr, sizeof(WAVEHDR));
}

static bool OpenWvDev(WvDev& dev, UINT id, UINT rate)
{
    InitializeCriticalSection(&dev.cs);

    // Try stereo first (needed for Stereo Mix), fall back to mono
    for (WORD ch : {(WORD)2, (WORD)1}) {
        dev.fmt.wFormatTag      = WAVE_FORMAT_PCM;
        dev.fmt.nChannels       = ch;
        dev.fmt.nSamplesPerSec  = rate;
        dev.fmt.wBitsPerSample  = 16;
        dev.fmt.nBlockAlign     = (WORD)(ch * 2);
        dev.fmt.nAvgBytesPerSec = rate * ch * 2;
        dev.fmt.cbSize          = 0;

        if (waveInOpen(&dev.hWave, id, &dev.fmt,
                       (DWORD_PTR)WvCallback, (DWORD_PTR)&dev,
                       CALLBACK_FUNCTION) == MMSYSERR_NOERROR) {
            dev.stereo = (ch == 2);
            break;
        }
    }
    if (!dev.hWave) { DeleteCriticalSection(&dev.cs); return false; }

    DWORD bufBytes = dev.fmt.nAvgBytesPerSec * WV_BUF_MS / 1000;
    bufBytes       = (bufBytes + 3) & ~3u;

    for (int i = 0; i < WV_BUF_COUNT; ++i) {
        dev.raw[i].assign(bufBytes, 0);
        dev.hdrs[i]               = {};
        dev.hdrs[i].lpData        = reinterpret_cast<LPSTR>(dev.raw[i].data());
        dev.hdrs[i].dwBufferLength = bufBytes;
        waveInPrepareHeader(dev.hWave, &dev.hdrs[i], sizeof(WAVEHDR));
        waveInAddBuffer    (dev.hWave, &dev.hdrs[i], sizeof(WAVEHDR));
    }
    return true;
}

static void CloseWvDev(WvDev& dev)
{
    if (!dev.hWave) return;
    waveInReset(dev.hWave);
    Sleep(80);  // let in-flight callbacks complete
    for (int i = 0; i < WV_BUF_COUNT; ++i)
        waveInUnprepareHeader(dev.hWave, &dev.hdrs[i], sizeof(WAVEHDR));
    waveInClose(dev.hWave);
    dev.hWave = nullptr;
    DeleteCriticalSection(&dev.cs);
}

// Score a mic device name — higher score = more preferred.
// Returns 0 for likely built-in mics, >0 for external/preferred.
static int ScoreMicName(const wchar_t* name)
{
    wchar_t low[256] = {};
    wcsncpy(low, name, 255);
    for (wchar_t* p = low; *p; ++p) *p = (wchar_t)towlower(*p);

    // Score 40: Bluetooth / premium wireless headsets
    if (wcsstr(low, L"bluetooth") || wcsstr(low, L"bt headset") ||
        wcsstr(low, L"airpod")    || wcsstr(low, L"bose")       ||
        wcsstr(low, L"jabra")     || wcsstr(low, L"sennheiser") ||
        wcsstr(low, L"poly"))
        return 40;

    // Score 0: built-in / integrated mics (check before generic USB/headset)
    if (wcsstr(low, L"array")      || wcsstr(low, L"internal")   ||
        wcsstr(low, L"built-in")   || wcsstr(low, L"integrated") ||
        wcsstr(low, L"realtek")    || wcsstr(low, L"conexant")   ||
        wcsstr(low, L"idt"))
        return 0;

    // Score 30: USB mics
    if (wcsstr(low, L"usb"))
        return 30;

    // Score 20: headset / headphone
    if (wcsstr(low, L"headset") || wcsstr(low, L"headphone"))
        return 20;

    // Score 10: anything else (unknown external)
    return 10;
}

// Enumerate WaveIn capture devices, score each, log to winrec_devices.txt,
// and return the device ID with the highest score (WAVE_MAPPER if all score 0).
static UINT LogAndFindBestMicDevice()
{
    std::wstring logPath = g_exeDir + L"\\winrec_devices.txt";
    FILE* f = _wfopen(logPath.c_str(), L"w, ccs=UTF-8");

    UINT n = waveInGetNumDevs();
    if (f) fwprintf(f, L"WaveIn capture devices (%u):\n", n);

    int  bestScore = -1;
    UINT bestId    = WAVE_MAPPER;
    wchar_t bestName[MAXPNAMELEN] = L"(WAVE_MAPPER)";

    for (UINT i = 0; i < n; ++i) {
        WAVEINCAPSW caps = {};
        if (waveInGetDevCapsW(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR) continue;
        int score = ScoreMicName(caps.szPname);
        if (f) fwprintf(f, L"  [%u] score=%2d  %ls\n", i, score, caps.szPname);
        if (score > bestScore) {
            bestScore = score;
            bestId    = i;
            wcsncpy(bestName, caps.szPname, MAXPNAMELEN - 1);
            bestName[MAXPNAMELEN - 1] = L'\0';
        }
    }

    if (bestScore <= 0) {
        bestId = WAVE_MAPPER;
        if (f) fwprintf(f, L"\nChosen: WAVE_MAPPER (no preferred external mic found)\n");
    } else {
        if (f) fwprintf(f, L"\nChosen: [%u] score=%d  %ls\n", bestId, bestScore, bestName);
    }

    if (f) fclose(f);
    return bestId;
}

// Returns true if a WaveIn device name looks like a loopback/mix source.
static bool IsLoopbackDevice(const wchar_t* szPname)
{
    wchar_t name[MAXPNAMELEN];
    wcsncpy(name, szPname, MAXPNAMELEN - 1);
    name[MAXPNAMELEN - 1] = L'\0';
    for (wchar_t* p = name; *p; ++p) *p = (wchar_t)towlower(*p);

    return wcsstr(name, L"stereo mix")  ||
           wcsstr(name, L"stereomix")   ||
           wcsstr(name, L"what u hear") ||
           wcsstr(name, L"loopback")    ||
           wcsstr(name, L"wave out")    ||
           wcsstr(name, L"mix") != nullptr;
}

static DWORD WINAPI WaveInWorker(LPVOID)
{
    waveInStart(g_wvMic.hWave);
    for (int k = 0; k < g_numLoops; ++k) waveInStart(g_wvLoops[k].hWave);

    bool ok = true;
    while (g_doCapture.load()) {
        EnterCriticalSection(&g_wvMic.cs);
        size_t micN = g_wvMic.ring.size();
        LeaveCriticalSection(&g_wvMic.cs);

        // Collect per-device sample counts
        size_t loopN[MAX_LOOPS] = {};
        for (int k = 0; k < g_numLoops; ++k) {
            EnterCriticalSection(&g_wvLoops[k].cs);
            loopN[k] = g_wvLoops[k].ring.size();
            LeaveCriticalSection(&g_wvLoops[k].cs);
        }

        // avail = minimum across mic and all loopback devices
        size_t avail = micN;
        for (int k = 0; k < g_numLoops; ++k)
            avail = std::min(avail, loopN[k]);

        if (avail > 0) {
            // Sum all loopback sources (typically only one has audio at a time)
            std::vector<float> loopSum(avail, 0.0f);
            for (int k = 0; k < g_numLoops; ++k) {
                EnterCriticalSection(&g_wvLoops[k].cs);
                for (size_t i = 0; i < avail; ++i)
                    loopSum[i] += g_wvLoops[k].ring.pop();
                LeaveCriticalSection(&g_wvLoops[k].cs);
            }

            std::vector<int16_t> out(avail);
            EnterCriticalSection(&g_wvMic.cs);
            for (size_t i = 0; i < avail; ++i) {
                float mic = g_wvMic.ring.pop();
                float sample = (g_numLoops > 0) ? (0.5f * loopSum[i] + 0.5f * mic) : mic;
                if (sample >  1.0f) sample =  1.0f;
                if (sample < -1.0f) sample = -1.0f;
                out[i] = (int16_t)roundf(sample * 32767.0f);
            }
            LeaveCriticalSection(&g_wvMic.cs);

            if (g_rawFile)
                if (fwrite(out.data(), sizeof(int16_t), avail, g_rawFile) != avail)
                    ok = false;
        } else if (g_numLoops > 0 && micN > 0) {
            // Pad any dry loopback devices with silence so mic isn't blocked
            for (int k = 0; k < g_numLoops; ++k) {
                if (loopN[k] == 0) {
                    std::vector<float> silence(micN, 0.0f);
                    EnterCriticalSection(&g_wvLoops[k].cs);
                    g_wvLoops[k].ring.push(silence, micN);
                    LeaveCriticalSection(&g_wvLoops[k].cs);
                }
            }
        }

        Sleep(20);
    }

    CloseWvDev(g_wvMic);
    for (int k = 0; k < g_numLoops; ++k) CloseWvDev(g_wvLoops[k]);
    g_numLoops = 0;

    if (g_rawFile) { fclose(g_rawFile); g_rawFile = nullptr; }

    PostMessageW(g_captureHwnd, WM_APP_RECORDING_DONE, ok ? 0 : 1, 0);
    return 0;
}

static std::wstring WaveInCaptureStart(const std::wstring& rawPath)
{
    UINT micId = LogAndFindBestMicDevice();  // enumerate, score, log, pick best

    UINT rate = 48000;
    if (!OpenWvDev(g_wvMic, micId, rate)) {
        if (micId != WAVE_MAPPER)
            OpenWvDev(g_wvMic, WAVE_MAPPER, rate);  // retry with system default
        if (!g_wvMic.hWave) {
            rate = 44100;
            if (!OpenWvDev(g_wvMic, WAVE_MAPPER, rate))
                return L"WaveIn: failed to open default mic (WAVE_MAPPER). "
                       L"Check winrec_devices.txt for available devices.";
        }
    }

    // Open ALL loopback/mix WaveIn devices (one per audio endpoint that supports it)
    g_numLoops = 0;
    UINT nDevs = waveInGetNumDevs();
    for (UINT i = 0; i < nDevs && g_numLoops < MAX_LOOPS; ++i) {
        WAVEINCAPSW caps = {};
        if (waveInGetDevCapsW(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR) continue;
        if (IsLoopbackDevice(caps.szPname))
            if (OpenWvDev(g_wvLoops[g_numLoops], i, rate))
                ++g_numLoops;
    }

    g_captureInputRate = rate;

    g_rawFile = _wfopen(rawPath.c_str(), L"wb");
    if (!g_rawFile) {
        CloseWvDev(g_wvMic);
        for (int k = 0; k < g_numLoops; ++k) CloseWvDev(g_wvLoops[k]);
        g_numLoops = 0;
        return L"Failed to open temp PCM file for writing.";
    }

    g_captureHwnd = g_app.hwnd;
    g_doCapture.store(true);

    g_captureThread = CreateThread(nullptr, 0, WaveInWorker, nullptr, 0, nullptr);
    if (!g_captureThread) {
        fclose(g_rawFile); g_rawFile = nullptr;
        CloseWvDev(g_wvMic);
        for (int k = 0; k < g_numLoops; ++k) CloseWvDev(g_wvLoops[k]);
        g_numLoops = 0;
        return L"WaveIn: CreateThread failed.";
    }
    return L"";  // success
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool g_micFallback = false;  // exported: true if mic was skipped

// Helper: format "step description (HRESULT 0x...)"
static std::wstring HrMsg(const wchar_t* step, HRESULT hr)
{
    wchar_t buf[128];
    _snwprintf(buf, 128, L"%ls (hr=0x%08X)", step, (unsigned)hr);
    return std::wstring(buf);
}

// Release all capture-side WASAPI objects on failure path
static void ReleaseAll()
{
    if (g_pMicCap)    { g_pMicCap->Release();    g_pMicCap    = nullptr; }
    if (g_pLbCap)     { g_pLbCap->Release();     g_pLbCap     = nullptr; }
    if (g_pMicClient) { g_pMicClient->Release();  g_pMicClient = nullptr; }
    if (g_pLbClient)  { g_pLbClient->Release();   g_pLbClient  = nullptr; }
    if (g_pLbFmt)     { CoTaskMemFree(g_pLbFmt);  g_pLbFmt     = nullptr; }
    if (g_pMicFmt)    { CoTaskMemFree(g_pMicFmt); g_pMicFmt    = nullptr; }
    if (g_pCapture)   { g_pCapture->Release();    g_pCapture   = nullptr; }
    if (g_pRender)    { g_pRender->Release();     g_pRender    = nullptr; }
    if (g_pEnum)      { g_pEnum->Release();        g_pEnum      = nullptr; }
}

// Attempt to create IMMDeviceEnumerator by loading mmdevapi.dll directly,
// bypassing CoCreateInstance (and any hooks on it).
static HRESULT CreateEnumeratorDirect(IMMDeviceEnumerator** ppEnum)
{
    // Try the direct DLL route first
    HMODULE hMod = LoadLibraryW(L"mmdevapi.dll");
    if (hMod) {
        typedef HRESULT (WINAPI *DllGetClassObjectFn)(REFCLSID, REFIID, LPVOID*);
        auto pDGCO = reinterpret_cast<DllGetClassObjectFn>(
                         GetProcAddress(hMod, "DllGetClassObject"));
        if (pDGCO) {
            IClassFactory* pCF = nullptr;
            HRESULT hr = pDGCO(CLSID_MMDeviceEnumerator_, IID_IClassFactory_,
                               (void**)&pCF);
            if (SUCCEEDED(hr) && pCF) {
                hr = pCF->CreateInstance(nullptr, IID_IMMDeviceEnumerator_,
                                         (void**)ppEnum);
                pCF->Release();
                if (SUCCEEDED(hr)) return hr;
            }
        }
        FreeLibrary(hMod);
    }
    // Fall back to standard CoCreateInstance
    return CoCreateInstance(CLSID_MMDeviceEnumerator_, nullptr, CLSCTX_INPROC_SERVER,
                            IID_IMMDeviceEnumerator_, (void**)ppEnum);
}

// Enumerate WASAPI capture endpoints, score by friendly name, return the best
// IMMDevice* (caller must Release). Returns nullptr → caller should fall back to
// GetDefaultAudioEndpoint. Also appends results to winrec_devices.txt.
static IMMDevice* FindBestWasapiMicDevice(IMMDeviceEnumerator* pEnum)
{
    IMMDeviceCollection* pColl = nullptr;
    HRESULT hr = pEnum->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &pColl);
    if (FAILED(hr) || !pColl) return nullptr;

    UINT count = 0;
    pColl->GetCount(&count);

    std::wstring logPath = g_exeDir + L"\\winrec_devices.txt";
    FILE* f = _wfopen(logPath.c_str(), L"w, ccs=UTF-8");
    if (f) fwprintf(f, L"WASAPI capture endpoints (%u):\n", count);

    int        bestScore = -1;
    IMMDevice* bestDev   = nullptr;
    wchar_t    bestName[256] = L"(default)";

    for (UINT i = 0; i < count; ++i) {
        IMMDevice* pDev = nullptr;
        if (FAILED(pColl->Item(i, &pDev)) || !pDev) continue;

        wchar_t friendlyName[256] = L"(unknown)";
        IPropertyStore* pStore = nullptr;
        if (SUCCEEDED(pDev->OpenPropertyStore(STGM_READ, &pStore)) && pStore) {
            PROPVARIANT pv;
            PropVariantInit(&pv);
            if (SUCCEEDED(pStore->GetValue(PKEY_Device_FriendlyName_, &pv)) &&
                pv.vt == VT_LPWSTR && pv.pwszVal) {
                wcsncpy(friendlyName, pv.pwszVal, 255);
                friendlyName[255] = L'\0';
            }
            PropVariantClear(&pv);
            pStore->Release();
        }

        int score = ScoreMicName(friendlyName);
        if (f) fwprintf(f, L"  [%u] score=%2d  %ls\n", i, score, friendlyName);

        if (score > bestScore) {
            bestScore = score;
            if (bestDev) bestDev->Release();
            bestDev = pDev;
            pDev = nullptr;  // ownership transferred
            wcsncpy(bestName, friendlyName, 255);
            bestName[255] = L'\0';
        }
        if (pDev) pDev->Release();
    }

    pColl->Release();

    if (bestScore <= 0) {
        if (f) fwprintf(f, L"\nWASAPI chosen: system default (no preferred external mic)\n");
        if (f) fclose(f);
        if (bestDev) { bestDev->Release(); bestDev = nullptr; }
        return nullptr;  // signal: use GetDefaultAudioEndpoint fallback
    }

    if (f) fwprintf(f, L"\nWASAPI chosen: score=%d  %ls\n", bestScore, bestName);
    if (f) fclose(f);
    return bestDev;
}

std::wstring CaptureStart(const std::wstring& rawPath, const SYSTEMTIME& /*startTime*/)
{
    HRESULT hr;
    g_micFallback  = false;
    g_usingWaveIn  = false;

    // --- COM device enumerator ---
    hr = CreateEnumeratorDirect(&g_pEnum);
    if (FAILED(hr)) {
        // WASAPI completely unavailable — try WaveIn fallback
        g_usingWaveIn = true;
        return WaveInCaptureStart(rawPath);
    }

    // --- Render device (loopback source) ---
    hr = g_pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &g_pRender);
    if (FAILED(hr)) { ReleaseAll(); return HrMsg(L"GetDefaultAudioEndpoint(render)", hr); }

    // --- Loopback AudioClient ---
    hr = g_pRender->Activate(IID_IAudioClient_, CLSCTX_ALL, nullptr, (void**)&g_pLbClient);
    if (FAILED(hr)) { ReleaseAll(); return HrMsg(L"Activate(loopback AudioClient)", hr); }

    hr = g_pLbClient->GetMixFormat(&g_pLbFmt);
    if (FAILED(hr)) { ReleaseAll(); return HrMsg(L"GetMixFormat(loopback)", hr); }

    hr = g_pLbClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                  AUDCLNT_STREAMFLAGS_LOOPBACK,
                                  2000000, 0, g_pLbFmt, nullptr);
    if (FAILED(hr)) { ReleaseAll(); return HrMsg(L"Initialize(loopback)", hr); }

    hr = g_pLbClient->GetService(IID_IAudioCaptureClient_, (void**)&g_pLbCap);
    if (FAILED(hr)) { ReleaseAll(); return HrMsg(L"GetService(loopback CaptureClient)", hr); }

    // --- Mic capture device (optional — fall back to loopback-only if unavailable) ---
    // Try to find the best external mic; fall back to system default if enumeration
    // returns no preferred device.
    g_pCapture = FindBestWasapiMicDevice(g_pEnum);
    if (!g_pCapture) {
        hr = g_pEnum->GetDefaultAudioEndpoint(eCapture, eCommunications, &g_pCapture);
        if (FAILED(hr))
            hr = g_pEnum->GetDefaultAudioEndpoint(eCapture, eConsole, &g_pCapture);
    } else {
        hr = S_OK;
    }

    if (SUCCEEDED(hr)) {
        hr = g_pCapture->Activate(IID_IAudioClient_, CLSCTX_ALL, nullptr, (void**)&g_pMicClient);
        if (SUCCEEDED(hr)) {
            hr = g_pMicClient->GetMixFormat(&g_pMicFmt);
            if (SUCCEEDED(hr)) {
                hr = g_pMicClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0,
                                               2000000, 0, g_pMicFmt, nullptr);
                if (SUCCEEDED(hr)) {
                    hr = g_pMicClient->GetService(IID_IAudioCaptureClient_,
                                                   (void**)&g_pMicCap);
                }
            }
        }
        if (FAILED(hr)) {
            // Mic setup failed — release mic objects, continue loopback-only
            if (g_pMicCap)    { g_pMicCap->Release();    g_pMicCap    = nullptr; }
            if (g_pMicClient) { g_pMicClient->Release();  g_pMicClient = nullptr; }
            if (g_pMicFmt)    { CoTaskMemFree(g_pMicFmt); g_pMicFmt    = nullptr; }
            if (g_pCapture)   { g_pCapture->Release();    g_pCapture   = nullptr; }
            g_micFallback = true;
        }
    } else {
        g_micFallback = true;
    }

    // --- Open raw PCM output file ---
    g_rawFile = _wfopen(rawPath.c_str(), L"wb");
    if (!g_rawFile) { ReleaseAll(); return L"Failed to open temp file for writing. Check disk space and permissions."; }

    g_captureHwnd = g_app.hwnd;
    g_doCapture.store(true);

    g_captureThread = CreateThread(nullptr, 0, CaptureWorker, nullptr, 0, nullptr);
    if (!g_captureThread) {
        fclose(g_rawFile); g_rawFile = nullptr;
        ReleaseAll();
        return L"CreateThread failed for capture worker.";
    }

    return L"";  // success
}

void CaptureStop()
{
    // Signal worker thread to stop, then wait for it to exit cleanly.
    g_doCapture.store(false);

    if (g_captureThread) {
        WaitForSingleObject(g_captureThread, INFINITE);
        CloseHandle(g_captureThread);
        g_captureThread = nullptr;
    }

    if (g_usingWaveIn) {
        // WaveInWorker already called CloseWvDev and closed g_rawFile.
        // Nothing further to release here.
        return;
    }

    // WASAPI cleanup: free format buffers and device COM objects.
    if (g_pLbFmt)  { CoTaskMemFree(g_pLbFmt);  g_pLbFmt  = nullptr; }
    if (g_pMicFmt) { CoTaskMemFree(g_pMicFmt); g_pMicFmt = nullptr; }

    if (g_pCapture) { g_pCapture->Release(); g_pCapture = nullptr; }
    if (g_pRender)  { g_pRender->Release();  g_pRender  = nullptr; }
    if (g_pEnum)    { g_pEnum->Release();     g_pEnum    = nullptr; }

    g_pMicCap = g_pLbCap = nullptr;
    g_pMicClient = g_pLbClient = nullptr;
}
