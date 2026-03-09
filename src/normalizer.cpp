// normalizer.cpp — two-pass offline resampler + normalizer
// Pass 1: read raw 16-bit mono PCM, resample to 16 kHz, track peak, write floats to .tmp
// Pass 2: read .tmp, apply gain, write final WAV
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

    // Open .tmp file for resampled float data
    FILE* fTmp = _wfopen(np.tmpPath.c_str(), L"wb");
    if (!fTmp) {
        fclose(fRaw);
        PostMessageW(np.hwnd, WM_APP_NORM_DONE, 1, 0);
        CoUninitialize();
        return 1;
    }

    // ------------------------------------------------------------------
    // Pass 1: resample to TARGET_RATE with linear interpolation, track peak
    // ------------------------------------------------------------------

    const double ratio = (double)np.inputRate / TARGET_RATE;  // e.g. 48000/16000 = 3.0

    // We read raw PCM into a rolling window to support interpolation
    // Total input frames (for progress, not strictly needed)
    fseek(fRaw, 0, SEEK_END);
    long rawFileBytes = ftell(fRaw);
    fseek(fRaw, 0, SEEK_SET);
    uint64_t totalInputFrames  = rawFileBytes / sizeof(int16_t);
    uint64_t totalOutputFrames = (uint64_t)((totalInputFrames - 1) / ratio);

    float    maxPeak    = 0.0f;
    uint64_t outCount   = 0;

    // We'll stream: maintain a "loaded" window of input samples.
    // For each output frame index i, input position = i * ratio.
    // We read ahead as needed.

    // Use a simple approach: pre-load all input into memory if < 500MB,
    // otherwise stream with buffered reads.
    // 1 hour at 48kHz 16-bit mono = 345 MB — load in chunks.

    // Streaming approach with small look-back buffer:
    // Keep a circular buffer of input samples and track absolute read position.

    uint64_t    inputLoaded = 0;    // absolute index of next input sample to load
    std::vector<int16_t> inWindow; // ring of loaded samples, indexed by absolute pos
    inWindow.reserve(16384);

    // Helper: get input sample at absolute position pos (as float)
    auto getInputSample = [&](uint64_t pos) -> float {
        if (pos >= totalInputFrames) return 0.0f;
        // Load up to pos+1 if needed
        while (inputLoaded <= pos + 1 && inputLoaded < totalInputFrames) {
            int16_t s = 0;
            if (fread(&s, sizeof(int16_t), 1, fRaw) == 1) {
                inWindow.push_back(s);
                ++inputLoaded;
            } else {
                break;
            }
            // Trim window: keep only what we still need (pos and one ahead)
            // Since we access only increasing positions, trim old entries
        }
        // inWindow[0] corresponds to absolute position (inputLoaded - inWindow.size())
        uint64_t base = inputLoaded - inWindow.size();
        if (pos < base) return 0.0f;  // already discarded (shouldn't happen)
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
    std::vector<float> outBuf;
    outBuf.reserve(WRITE_BUF);

    for (uint64_t oi = 0; oi <= totalOutputFrames; ++oi) {
        double  pos  = oi * ratio;
        uint64_t idx0 = (uint64_t)pos;
        double  frac = pos - (double)idx0;

        float s0 = getInputSample(idx0);
        float s1 = getInputSample(idx0 + 1);
        float s  = (float)(s0 * (1.0 - frac) + s1 * frac);

        if (s >  maxPeak) maxPeak = s;
        if (-s > maxPeak) maxPeak = -s;

        outBuf.push_back(s);
        if (outBuf.size() >= WRITE_BUF) {
            fwrite(outBuf.data(), sizeof(float), outBuf.size(), fTmp);
            outBuf.clear();
        }

        // Trim no-longer-needed input
        if (idx0 > 1) trimWindow(idx0 - 1);
        ++outCount;
    }
    if (!outBuf.empty()) {
        fwrite(outBuf.data(), sizeof(float), outBuf.size(), fTmp);
        outBuf.clear();
    }

    fclose(fRaw);
    fclose(fTmp);

    // ------------------------------------------------------------------
    // Pass 2: read .tmp, apply gain, write WAV
    // ------------------------------------------------------------------

    float gain = (maxPeak < 1e-6f) ? 1.0f : (0.95f / maxPeak);

    FILE* fTmp2 = _wfopen(np.tmpPath.c_str(), L"rb");
    if (!fTmp2) goto cleanup;

    {
        FILE* fOut = _wfopen(np.outPath.c_str(), L"wb");
        if (!fOut) { fclose(fTmp2); goto cleanup; }

        // Write placeholder WAV header
        WavHeader hdr = {};
        FillWavHeader(hdr, (uint32_t)outCount);
        fwrite(&hdr, sizeof(hdr), 1, fOut);

        // Stream floats → int16
        const size_t RBUF = 4096;
        std::vector<float>   fbuf(RBUF);
        std::vector<int16_t> ibuf(RBUF);
        uint64_t total = 0;

        while (true) {
            size_t n = fread(fbuf.data(), sizeof(float), RBUF, fTmp2);
            if (n == 0) break;
            for (size_t i = 0; i < n; ++i) {
                float s = fbuf[i] * gain;
                if (s >  1.0f) s =  1.0f;
                if (s < -1.0f) s = -1.0f;
                ibuf[i] = (int16_t)roundf(s * 32767.0f);
            }
            fwrite(ibuf.data(), sizeof(int16_t), n, fOut);
            total += n;
        }

        // Patch WAV header with actual sample count
        rewind(fOut);
        FillWavHeader(hdr, (uint32_t)total);
        fwrite(&hdr, sizeof(hdr), 1, fOut);

        fclose(fOut);
        fclose(fTmp2);
        ok = true;
    }

cleanup:
    // Delete temp files
    _wremove(np.rawPath.c_str());
    _wremove(np.tmpPath.c_str());

    PostMessageW(np.hwnd, WM_APP_NORM_DONE, ok ? 0 : 1, 0);
    CoUninitialize();
    return ok ? 0 : 1;
}
