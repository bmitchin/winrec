#include "winrec.h"
#include <combaseapi.h>
#include <cstring>

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

AppState     g_app  = {};
std::wstring g_exeDir;

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
    g_app.outPath = L"";  // filled on stop

    std::wstring captureErr = CaptureStart(g_app.rawPath, g_app.startTime);
    if (!captureErr.empty()) {
        TrayBalloon(L"winrec – Error", captureErr.c_str());
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

static void OnRecordingDone(bool ok)
{
    if (!ok) {
        TrayBalloon(L"winrec – Error", L"Capture thread encountered an error.");
        EnterIdle();
        return;
    }

    g_app.state = RecorderState::Normalizing;
    TrayUpdate(RecorderState::Normalizing);

    // CaptureStop() fills g_inputRate – we pass via NormParams
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
        TrayBalloon(L"winrec – Error", L"Failed to start normalizer thread.");
        EnterIdle();
    }
}

// ---------------------------------------------------------------------------
// Message: normalizer finished → launch uploader thread
// ---------------------------------------------------------------------------

static void OnNormDone(bool ok)
{
    if (!ok) {
        TrayBalloon(L"winrec – Error", L"Normalization failed.");
        EnterIdle();
        return;
    }

    g_app.state = RecorderState::Uploading;
    TrayUpdate(RecorderState::Uploading);

    std::wstring rclonePath = g_exeDir + L"\\rclone.exe";
    auto* p = new UploadParams{
        g_app.outPath,
        rclonePath,
        L"gdrive:teams-audio/",
        g_app.hwnd
    };
    HANDLE h = CreateThread(nullptr, 0, UploaderThread, p, 0, nullptr);
    if (h) CloseHandle(h);
    else {
        delete p;
        TrayBalloon(L"winrec – Error", L"Failed to start uploader thread.");
        EnterIdle();
    }
}

// ---------------------------------------------------------------------------
// Message: upload finished
// ---------------------------------------------------------------------------

static void OnUploadDone(bool ok)
{
    if (ok) {
        // Extract filename from outPath for balloon
        auto pos = g_app.outPath.find_last_of(L"\\/");
        std::wstring name = (pos != std::wstring::npos)
                            ? g_app.outPath.substr(pos + 1)
                            : g_app.outPath;
        std::wstring msg = L"Upload complete: " + name;
        TrayBalloon(L"winrec", msg.c_str());
    } else {
        TrayBalloon(L"winrec – Error", L"Upload failed. WAV kept in out\\ folder.");
    }
    EnterIdle();
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
            if (g_app.state == RecorderState::Idle)      StartRecording();
            else if (g_app.state == RecorderState::Recording) StopRecording();
            break;
        case WM_RBUTTONUP:
            ShowContextMenu(hwnd);
            break;
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

    case WM_APP_TEAMS_CALL_START:
        if (g_app.state == RecorderState::Idle) {
            TrayBalloon(L"winrec – Teams", L"Call detected – recording started automatically.");
            StartRecording();
        }
        return 0;

    case WM_APP_TEAMS_CALL_END:
        if (g_app.state == RecorderState::Recording) {
            TrayBalloon(L"winrec – Teams", L"Call ended – stopping recording.");
            StopRecording();
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

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    TeamsMonitorStop();
    CoUninitialize();
    return (int)msg.wParam;
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    return WinMainImpl();
}
