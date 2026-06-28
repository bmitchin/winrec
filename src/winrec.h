#pragma once
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <shellapi.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <string>
#include <atomic>
#include <cstdint>
#include <vector>

// ---------------------------------------------------------------------------
// Application state
// ---------------------------------------------------------------------------

enum class RecorderState { Idle, Recording, Normalizing, Uploading, Error };

struct AppState {
    HWND            hwnd;
    RecorderState   state;
    SYSTEMTIME      startTime;
    SYSTEMTIME      endTime;
    std::wstring    rawPath;   // temp .pcm
    std::wstring    tmpPath;   // temp resampled float .tmp
    std::wstring    outPath;   // final .wav
};

// ---------------------------------------------------------------------------
// Split recording support
// ---------------------------------------------------------------------------

constexpr int SPLIT_INTERVAL_SECONDS = 2100;  // ~35 min per chunk

// ---------------------------------------------------------------------------
// Upload target — single knob for the rclone remote folder.
// v2 builds override this to a quarantined folder so test uploads never land
// in the live folder the transcription project consumes.
// ---------------------------------------------------------------------------

constexpr const wchar_t* REMOTE_FOLDER = L"gdrive:teams-audio/";

struct SplitChunk {
    std::wstring rawPath;
    std::wstring tmpPath;
    std::wstring outPath;
    SYSTEMTIME   startTime;
    SYSTEMTIME   endTime;
};

extern AppState g_app;
extern std::wstring g_exeDir;

// ---------------------------------------------------------------------------
// Custom window messages
// ---------------------------------------------------------------------------

#define WM_TRAYICON          (WM_USER + 1)
#define WM_APP_RECORDING_DONE (WM_USER + 2)   // posted by capture thread when stopped
#define WM_APP_NORM_DONE      (WM_USER + 3)   // posted by normalizer thread when done
#define WM_APP_UPLOAD_DONE       (WM_USER + 4)   // posted by uploader thread when done
// wParam for WM_APP_*_DONE: 0 = success, 1 = error

#define WM_APP_TEAMS_CALL_START   (WM_USER + 5)   // Teams call detected
#define WM_APP_TEAMS_CALL_END     (WM_USER + 6)   // Teams call ended

// Menu IDs
#define IDM_START  1001
#define IDM_STOP   1002
#define IDM_EXIT   1003

// ---------------------------------------------------------------------------
// Tray
// ---------------------------------------------------------------------------

void TrayAdd(HWND hwnd);
void TrayUpdate(RecorderState state);
void TrayRemove();
void TrayBalloon(const wchar_t* title, const wchar_t* msg);

// ---------------------------------------------------------------------------
// Capture engine
// ---------------------------------------------------------------------------

// Returns empty string on success, or a human-readable error description.
std::wstring CaptureStart(const std::wstring& rawPath, const SYSTEMTIME& startTime);
void         CaptureStop();  // sets stop flag; thread posts WM_APP_RECORDING_DONE

// Set to true by CaptureStart if mic was unavailable but loopback succeeded.
extern bool g_micFallback;
// Set to true when WaveIn fallback is active instead of WASAPI.
extern bool g_usingWaveIn;
// Number of WaveIn loopback devices successfully opened (0 = mic only).
extern int  g_numLoops;

// ---------------------------------------------------------------------------
// Normalizer  (runs in a thread; posts WM_APP_NORM_DONE)
// ---------------------------------------------------------------------------

struct NormParams {
    std::wstring rawPath;
    std::wstring outPath;
    UINT32       inputRate;   // sample rate of raw PCM
    HWND         hwnd;
};
DWORD WINAPI NormalizerThread(LPVOID param);

// ---------------------------------------------------------------------------
// Uploader  (runs in a thread; posts WM_APP_UPLOAD_DONE)
// ---------------------------------------------------------------------------

struct UploadParams {
    std::wstring outPath;
    std::wstring rclonePath;
    std::wstring remote;
    HWND         hwnd;
};
DWORD WINAPI UploaderThread(LPVOID param);

// ---------------------------------------------------------------------------
// File manager helpers
// ---------------------------------------------------------------------------

std::wstring GetExeDir();
std::wstring FormatTimestamp(const SYSTEMTIME& st);
void EnsureDirectory(const std::wstring& path);

// ---------------------------------------------------------------------------
// Teams monitor
// ---------------------------------------------------------------------------

void            TeamsMonitorStart(HWND hwnd);
void            TeamsMonitorStop();
const wchar_t*  TeamsGetMeetingName();

