#include "winrec.h"
#include <combaseapi.h>
#include <cstring>

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

AppState     g_app  = {};
std::wstring g_exeDir;

// Teams auto-record and split state
static std::wstring          g_callMeetingName;           // captured at call start, used for all chunk filenames
static bool                  g_isTeamsCall    = false;
static bool                  g_pendingSplit   = false;   // split timer fired, waiting for capture stop
static bool                  g_teamsCallEnding = false;  // call ended, waiting for capture stop
static std::vector<SplitChunk> g_chunks;
static int                   g_normChunkIdx   = 0;
static int                   g_uploadChunkIdx = 0;

#define SPLIT_TIMER_ID  2001

// ---------------------------------------------------------------------------
// File-manager helpers
// ---------------------------------------------------------------------------

std::wstring GetExeDir()
{
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring path(buf);
    auto pos = path.find_last_of(L"\\/");
    if (pos != std::wstring::npos) path.resize(pos);
    return path;
}

std::wstring FormatTimestamp(const SYSTEMTIME& st)
{
    wchar_t buf[32];
    int yy = st.wYear % 100;
    _snwprintf(buf, _countof(buf), L"%02d%02d%02d_%02d%02d",
               yy, st.wMonth, st.wDay, st.wHour, st.wMinute);
    return std::wstring(buf);
}

void EnsureDirectory(const std::wstring& path)
{
    CreateDirectoryW(path.c_str(), nullptr);
}

// ---------------------------------------------------------------------------
// State transitions (called from main thread only)
// ---------------------------------------------------------------------------

static void EnterIdle()
{
    g_app.state = RecorderState::Idle;
    TrayUpdate(RecorderState::Idle);
}

static void EnterError(const wchar_t* msg)
{
    g_app.state = RecorderState::Error;
    TrayUpdate(RecorderState::Error);
    TrayBalloon(L"winrec – Error", msg);
}

// Build the output .wav path for a chunk, appending the saved meeting name if set.
static std::wstring ComputeChunkOutPath(const SYSTEMTIME& start, const SYSTEMTIME& end)
{
    std::wstring outDir = g_exeDir + L"\\out";
    std::wstring name   = FormatTimestamp(start) + L"-" + FormatTimestamp(end);
    if (!g_callMeetingName.empty()) { name += L"_"; name += g_callMeetingName; }
    return outDir + L"\\" + name + L".wav";
}

static void StartRecording()
{
    if (g_app.state != RecorderState::Idle) return;

    GetLocalTime(&g_app.startTime);
    std::wstring ts      = FormatTimestamp(g_app.startTime);
    std::wstring tmpDir  = g_exeDir + L"\\tmp";
    std::wstring outDir  = g_exeDir + L"\\out";
    EnsureDirectory(tmpDir);
    EnsureDirectory(outDir);

    g_app.rawPath = tmpDir + L"\\raw_" + ts + L".pcm";
    g_app.tmpPath = tmpDir + L"\\raw_" + ts + L".tmp";
    g_app.outPath = L"";  // filled on stop (manual) or per-chunk (Teams)

    std::wstring captureErr = CaptureStart(g_app.rawPath, g_app.startTime);
    if (!captureErr.empty()) {
        EnterError(captureErr.c_str());
        return;
    }

    g_app.state = RecorderState::Recording;
    TrayUpdate(RecorderState::Recording);

    // Show mode warning if we fell back from the primary capture path
    if (g_usingWaveIn) {
        if (g_numLoops > 0) {
            wchar_t buf[128];
            _snwprintf(buf, _countof(buf),
                       L"WASAPI blocked; WaveIn mic + %d loopback source(s).", g_numLoops);
            TrayBalloon(L"winrec – WaveIn mode", buf);
        } else {
            TrayBalloon(L"winrec – WaveIn mode",
                        L"WASAPI blocked; mic only (no loopback device found). "
                        L"See winrec_devices.txt.");
        }
    } else if (g_micFallback) {
        TrayBalloon(L"winrec – Warning",
                    L"Mic unavailable; recording system audio only.");
    }

    wchar_t msg[64];
    _snwprintf(msg, _countof(msg), L"Recording started at %02d:%02d",
               g_app.startTime.wHour, g_app.startTime.wMinute);
    TrayBalloon(L"winrec", msg);
}

static void StopRecording()
{
    if (g_app.state != RecorderState::Recording) return;
    GetLocalTime(&g_app.endTime);

    std::wstring startStr = FormatTimestamp(g_app.startTime);
    std::wstring endStr   = FormatTimestamp(g_app.endTime);
    std::wstring outDir   = g_exeDir + L"\\out";
    g_app.outPath = outDir + L"\\" + startStr + L"-" + endStr + L".wav";

    CaptureStop();  // async; WM_APP_RECORDING_DONE will be posted when thread exits
    TrayBalloon(L"winrec", L"Recording stopped; normalizing…");
}

// ---------------------------------------------------------------------------
// Message: recording thread finished → launch normalizer thread
// ---------------------------------------------------------------------------

// Forward declaration for batch processing
static void StartBatchNormalize();

static void OnRecordingDone(bool ok)
{
    if (!ok) {
        EnterError(L"Capture thread encountered an error.");
        return;
    }

    // --- Split timer fired: restart capture for next chunk ---
    if (g_pendingSplit) {
        g_pendingSplit = false;

        if (g_teamsCallEnding) {
            // Call ended while split was in flight; process what we have
            g_teamsCallEnding = false;
            StartBatchNormalize();
            return;
        }

        // Start new capture chunk
        GetLocalTime(&g_app.startTime);
        std::wstring ts     = FormatTimestamp(g_app.startTime);
        std::wstring tmpDir = g_exeDir + L"\\tmp";
        g_app.rawPath = tmpDir + L"\\raw_" + ts + L".pcm";
        g_app.tmpPath = tmpDir + L"\\raw_" + ts + L".tmp";
        g_app.outPath = L"";

        std::wstring captureErr = CaptureStart(g_app.rawPath, g_app.startTime);
        if (!captureErr.empty()) {
            EnterError(captureErr.c_str());
            return;
        }
        g_app.state = RecorderState::Recording;
        TrayUpdate(RecorderState::Recording);
        return;
    }

    // --- Teams call ended: all chunks collected, begin batch normalise ---
    if (g_teamsCallEnding) {
        g_teamsCallEnding = false;
        StartBatchNormalize();
        return;
    }

    // --- Manual recording: normalise immediately ---
    g_app.state = RecorderState::Normalizing;
    TrayUpdate(RecorderState::Normalizing);

    extern UINT32 g_captureInputRate;

    auto* p = new NormParams{
        g_app.rawPath,
        g_app.tmpPath,
        g_app.outPath,
        g_captureInputRate,
        g_app.hwnd
    };
    HANDLE h = CreateThread(nullptr, 0, NormalizerThread, p, 0, nullptr);
    if (h) CloseHandle(h);
    else {
        delete p;
        EnterError(L"Failed to start normalizer thread.");
    }
}

// ---------------------------------------------------------------------------
// Batch normalise → upload for Teams multi-chunk recordings
// ---------------------------------------------------------------------------

static void StartBatchNormalize()
{
    if (g_chunks.empty()) { EnterIdle(); return; }

    g_app.state = RecorderState::Normalizing;
    TrayUpdate(RecorderState::Normalizing);
    g_normChunkIdx = 0;

    extern UINT32 g_captureInputRate;
    auto& chunk = g_chunks[0];
    auto* p = new NormParams{
        chunk.rawPath, chunk.tmpPath, chunk.outPath,
        g_captureInputRate, g_app.hwnd
    };
    HANDLE h = CreateThread(nullptr, 0, NormalizerThread, p, 0, nullptr);
    if (h) CloseHandle(h);
    else { delete p; EnterError(L"Failed to start normalizer thread."); }
}

// ---------------------------------------------------------------------------
// Message: normalizer finished → launch uploader thread
// ---------------------------------------------------------------------------

static void OnNormDone(bool ok)
{
    if (!ok) {
        EnterError(L"Normalization failed.");
        return;
    }

    // --- Batch mode: normalise next chunk, or start upload batch ---
    if (g_isTeamsCall && !g_chunks.empty()) {
        g_normChunkIdx++;
        if (g_normChunkIdx < (int)g_chunks.size()) {
            // Normalise next chunk
            extern UINT32 g_captureInputRate;
            auto& chunk = g_chunks[g_normChunkIdx];
            auto* p = new NormParams{
                chunk.rawPath, chunk.tmpPath, chunk.outPath,
                g_captureInputRate, g_app.hwnd
            };
            HANDLE h = CreateThread(nullptr, 0, NormalizerThread, p, 0, nullptr);
            if (h) CloseHandle(h);
            else { delete p; EnterError(L"Failed to start normalizer thread."); }
            return;
        }

        // All chunks normalised — begin uploading
        g_app.state = RecorderState::Uploading;
        TrayUpdate(RecorderState::Uploading);
        g_uploadChunkIdx = 0;
        std::wstring rclonePath = g_exeDir + L"\\rclone.exe";
        auto* p = new UploadParams{
            g_chunks[0].outPath, rclonePath, L"gdrive:teams-audio/", g_app.hwnd
        };
        HANDLE h = CreateThread(nullptr, 0, UploaderThread, p, 0, nullptr);
        if (h) CloseHandle(h);
        else { delete p; EnterError(L"Failed to start uploader thread."); }
        return;
    }

    // --- Manual recording: upload immediately ---
    g_app.state = RecorderState::Uploading;
    TrayUpdate(RecorderState::Uploading);

    std::wstring rclonePath = g_exeDir + L"\\rclone.exe";
    auto* p = new UploadParams{
        g_app.outPath, rclonePath, L"gdrive:teams-audio/", g_app.hwnd
    };
    HANDLE h = CreateThread(nullptr, 0, UploaderThread, p, 0, nullptr);
    if (h) CloseHandle(h);
    else {
        delete p;
        EnterError(L"Failed to start uploader thread.");
    }
}

// ---------------------------------------------------------------------------
// Message: upload finished
// ---------------------------------------------------------------------------

static void OnUploadDone(bool ok)
{
    // --- Batch mode ---
    if (g_isTeamsCall && !g_chunks.empty()) {
        if (!ok) {
            EnterError(L"Upload failed. WAV kept in out\\ folder.");
            return;
        }
        const std::wstring& uploaded = g_chunks[g_uploadChunkIdx].outPath;
        auto pos = uploaded.find_last_of(L"\\/");
        std::wstring name = (pos != std::wstring::npos) ? uploaded.substr(pos + 1) : uploaded;
        TrayBalloon(L"winrec", (L"Upload complete: " + name).c_str());

        g_uploadChunkIdx++;
        if (g_uploadChunkIdx < (int)g_chunks.size()) {
            std::wstring rclonePath = g_exeDir + L"\\rclone.exe";
            auto* p = new UploadParams{
                g_chunks[g_uploadChunkIdx].outPath, rclonePath,
                L"gdrive:teams-audio/", g_app.hwnd
            };
            HANDLE h = CreateThread(nullptr, 0, UploaderThread, p, 0, nullptr);
            if (h) CloseHandle(h);
            else { delete p; EnterError(L"Failed to start uploader thread."); }
            return;
        }

        // All chunks uploaded
        g_isTeamsCall     = false;
        g_callMeetingName.clear();
        g_chunks.clear();
        EnterIdle();
        return;
    }

    // --- Manual recording ---
    if (ok) {
        auto pos = g_app.outPath.find_last_of(L"\\/");
        std::wstring name = (pos != std::wstring::npos)
                            ? g_app.outPath.substr(pos + 1)
                            : g_app.outPath;
        TrayBalloon(L"winrec", (L"Upload complete: " + name).c_str());
        EnterIdle();
    } else {
        EnterError(L"Upload failed. WAV kept in out\\ folder.");
    }
}

// ---------------------------------------------------------------------------
// Context menu
// ---------------------------------------------------------------------------

static void ShowContextMenu(HWND hwnd)
{
    HMENU hMenu = CreatePopupMenu();
    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_STRING, IDM_EXIT, L"Exit");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN,
                   pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(hMenu);
}

// ---------------------------------------------------------------------------
// Hidden window procedure
// ---------------------------------------------------------------------------

static LRESULT CALLBACK HiddenWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_TRAYICON:
        switch ((UINT)lParam) {
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
            if (g_app.state == RecorderState::Idle)           StartRecording();
            else if (g_app.state == RecorderState::Recording)  StopRecording();
            else if (g_app.state == RecorderState::Error)      EnterIdle();  // dismiss
            break;
        case WM_RBUTTONUP:
            ShowContextMenu(hwnd);
            break;
        }
        return 0;

    case WM_TIMER:
        if ((UINT_PTR)wParam == SPLIT_TIMER_ID &&
            g_isTeamsCall && g_app.state == RecorderState::Recording) {
            // Save current chunk, then restart capture
            SYSTEMTIME endTime;
            GetLocalTime(&endTime);

            SplitChunk chunk;
            chunk.rawPath   = g_app.rawPath;
            chunk.tmpPath   = g_app.tmpPath;
            chunk.startTime = g_app.startTime;
            chunk.endTime   = endTime;
            chunk.outPath   = ComputeChunkOutPath(chunk.startTime, chunk.endTime);
            g_chunks.push_back(chunk);

            g_pendingSplit = true;
            CaptureStop();  // async; WM_APP_RECORDING_DONE will restart capture
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDM_START: StartRecording();  break;
        case IDM_STOP:  StopRecording();   break;
        case IDM_EXIT:
            if (g_app.state == RecorderState::Recording) CaptureStop();
            TrayRemove();
            PostQuitMessage(0);
            break;
        }
        return 0;

    case WM_APP_RECORDING_DONE:
        OnRecordingDone(wParam == 0);
        return 0;

    case WM_APP_NORM_DONE:
        OnNormDone(wParam == 0);
        return 0;

    case WM_APP_UPLOAD_DONE:
        OnUploadDone(wParam == 0);
        return 0;

    case WM_APP_TRANSCRIPT_FETCHED:
        if (wParam > 0)
            TrayBalloon(L"winrec", L"Transcript(s) moved to OneDrive.");
        return 0;

    case WM_APP_TEAMS_CALL_START:
        if (g_app.state == RecorderState::Idle) {
            g_callMeetingName = TeamsGetMeetingName();  // snapshot before thread clears it
            g_isTeamsCall     = true;
            g_pendingSplit    = false;
            g_teamsCallEnding = false;
            g_chunks.clear();
            TrayBalloon(L"winrec – Teams", L"Call detected – recording started automatically.");
            StartRecording();
            SetTimer(hwnd, SPLIT_TIMER_ID, SPLIT_INTERVAL_SECONDS * 1000, nullptr);
        }
        return 0;

    case WM_APP_TEAMS_CALL_END:
        if (g_isTeamsCall) {
            KillTimer(hwnd, SPLIT_TIMER_ID);
            TrayBalloon(L"winrec – Teams", L"Call ended – stopping recording.");

            if (g_app.state == RecorderState::Recording && !g_pendingSplit) {
                // Normal path: stop capture and save final chunk
                SYSTEMTIME endTime;
                GetLocalTime(&endTime);

                SplitChunk chunk;
                chunk.rawPath   = g_app.rawPath;
                chunk.tmpPath   = g_app.tmpPath;
                chunk.startTime = g_app.startTime;
                chunk.endTime   = endTime;
                chunk.outPath   = ComputeChunkOutPath(chunk.startTime, chunk.endTime);
                g_chunks.push_back(chunk);

                g_teamsCallEnding = true;
                CaptureStop();
            } else if (g_pendingSplit) {
                // Split already in flight; OnRecordingDone will detect g_teamsCallEnding
                g_teamsCallEnding = true;
            }
        }
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// WinMain
// ---------------------------------------------------------------------------

int WINAPI WinMainImpl()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    g_exeDir = GetExeDir();

    // Register window class
    WNDCLASSEXW wc   = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = HiddenWndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"WinRecHidden";
    RegisterClassExW(&wc);

    // Create hidden window
    g_app.hwnd = CreateWindowExW(0, L"WinRecHidden", L"winrec",
                                 0, 0, 0, 0, 0,
                                 HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    if (!g_app.hwnd) return 1;

    g_app.state = RecorderState::Idle;
    TrayAdd(g_app.hwnd);
    TeamsMonitorStart(g_app.hwnd);
    TranscriptFetcherStart(g_app.hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    TeamsMonitorStop();
    TranscriptFetcherStop();
    CoUninitialize();
    return (int)msg.wParam;
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    return WinMainImpl();
}
