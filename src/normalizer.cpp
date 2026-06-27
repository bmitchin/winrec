// normalizer.cpp — single-pass resampler (no peak normalization)
// Reads raw 16-bit mono PCM, resamples to 16 kHz, writes directly to .wav
#include "winrec.h"

#include <cstdio>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <algorithm>
#include <vector>

static const UINT32 TARGET_RATE = 16000;

// ---------------------------------------------------------------------------
// WAV header
// ---------------------------------------------------------------------------

#pragma pack(push, 1)
struct WavHeader {
    char     riff[4];       // "RIFF"
    uint32_t riffSize;      // file size - 8
    char     wave[4];       // "WAVE"
    char     fmt[4];        // "fmt "
    uint32_t fmtSize;       // 16
    uint16_t audioFmt;      // 1 = PCM
    uint16_t channels;      // 1
    uint32_t sampleRate;    // 16000
    uint32_t byteRate;      // sampleRate * channels * bitsPerSample / 8
    uint16_t blockAlign;    // channels * bitsPerSample / 8
    uint16_t bitsPerSample; // 16
    char     data[4];       // "data"
    uint32_t dataSize;      // num bytes of PCM
};
#pragma pack(pop)

static void FillWavHeader(WavHeader& h, uint32_t numSamples)
{
    uint32_t dataBytes = numSamples * sizeof(int16_t);
    memcpy(h.riff, "RIFF", 4);
    h.riffSize      = 36 + dataBytes;
    memcpy(h.wave, "WAVE", 4);
    memcpy(h.fmt,  "fmt ", 4);
    h.fmtSize       = 16;
    h.audioFmt      = 1;
    h.channels      = 1;
    h.sampleRate    = TARGET_RATE;
    h.byteRate      = TARGET_RATE * 2;
    h.blockAlign    = 2;
    h.bitsPerSample = 16;
    memcpy(h.data, "data", 4);
    h.dataSize      = dataBytes;
}

// ---------------------------------------------------------------------------
// Normalizer thread
// ---------------------------------------------------------------------------

DWORD WINAPI NormalizerThread(LPVOID param)
{
    auto* p = reinterpret_cast<NormParams*>(param);
    NormParams np = *p;
    delete p;

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    bool ok = false;

    // Open raw PCM file (16-bit mono at np.inputRate)
    FILE* fRaw = _wfopen(np.rawPath.c_str(), L"rb");
    if (!fRaw) {
        PostMessageW(np.hwnd, WM_APP_NORM_DONE, 1, 0);
        CoUninitialize();
        return 1;
    }

    FILE* fOut = _wfopen(np.outPath.c_str(), L"wb");
    if (!fOut) {
        fclose(fRaw);
        PostMessageW(np.hwnd, WM_APP_NORM_DONE, 1, 0);
        CoUninitialize();
        return 1;
    }

    // ------------------------------------------------------------------
    // Single pass: resample to TARGET_RATE with linear interpolation,
    // convert float → int16_t, write directly to WAV
    // ------------------------------------------------------------------

    const double ratio = (double)np.inputRate / TARGET_RATE;  // e.g. 48000/16000 = 3.0

    fseek(fRaw, 0, SEEK_END);
    long rawFileBytes = ftell(fRaw);
    fseek(fRaw, 0, SEEK_SET);
    uint64_t totalInputFrames  = rawFileBytes / sizeof(int16_t);
    uint64_t totalOutputFrames = (uint64_t)((totalInputFrames - 1) / ratio);

    // Write placeholder WAV header (patched with actual count at end)
    WavHeader hdr = {};
    FillWavHeader(hdr, (uint32_t)totalOutputFrames + 1);
    fwrite(&hdr, sizeof(hdr), 1, fOut);

    uint64_t    inputLoaded = 0;
    std::vector<int16_t> inWindow;
    inWindow.reserve(16384);

    auto getInputSample = [&](uint64_t pos) -> float {
        if (pos >= totalInputFrames) return 0.0f;
        while (inputLoaded <= pos + 1 && inputLoaded < totalInputFrames) {
            int16_t s = 0;
            if (fread(&s, sizeof(int16_t), 1, fRaw) == 1) {
                inWindow.push_back(s);
                ++inputLoaded;
            } else {
                break;
            }
        }
        uint64_t base = inputLoaded - inWindow.size();
        if (pos < base) return 0.0f;
        return inWindow[(size_t)(pos - base)] / 32768.0f;
    };

    auto trimWindow = [&](uint64_t minNeeded) {
        uint64_t base = inputLoaded - inWindow.size();
        if (minNeeded > base) {
            size_t drop = (size_t)(minNeeded - base);
            if (drop > inWindow.size()) drop = inWindow.size();
            inWindow.erase(inWindow.begin(), inWindow.begin() + drop);
        }
    };

    const size_t WRITE_BUF = 8192;
    std::vector<int16_t> outBuf;
    outBuf.reserve(WRITE_BUF);
    uint64_t outCount = 0;

    for (uint64_t oi = 0; oi <= totalOutputFrames; ++oi) {
        double   pos  = oi * ratio;
        uint64_t idx0 = (uint64_t)pos;
        double   frac = pos - (double)idx0;

        float s0 = getInputSample(idx0);
        float s1 = getInputSample(idx0 + 1);
        float s  = (float)(s0 * (1.0 - frac) + s1 * frac);

        if (s >  1.0f) s =  1.0f;
        if (s < -1.0f) s = -1.0f;
        outBuf.push_back((int16_t)roundf(s * 32767.0f));

        if (outBuf.size() >= WRITE_BUF) {
            fwrite(outBuf.data(), sizeof(int16_t), outBuf.size(), fOut);
            outCount += outBuf.size();
            outBuf.clear();
        }

        if (idx0 > 1) trimWindow(idx0 - 1);
    }
    if (!outBuf.empty()) {
        fwrite(outBuf.data(), sizeof(int16_t), outBuf.size(), fOut);
        outCount += outBuf.size();
        outBuf.clear();
    }

    // Patch WAV header with actual sample count
    rewind(fOut);
    FillWavHeader(hdr, (uint32_t)outCount);
    fwrite(&hdr, sizeof(hdr), 1, fOut);

    fclose(fOut);
    fclose(fRaw);
    ok = true;

    _wremove(np.rawPath.c_str());

    PostMessageW(np.hwnd, WM_APP_NORM_DONE, ok ? 0 : 1, 0);
    CoUninitialize();
    return ok ? 0 : 1;
}
