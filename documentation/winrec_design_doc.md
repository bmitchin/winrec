# winrec — Design Document

**Version 1.3 — June 2026**

This document describes the architecture, data flows, and key design decisions for winrec as actually implemented. It supersedes the original brief-form spec.

---

## Table of Contents

1. [Goals and Constraints](#1-goals-and-constraints)
2. [Architecture Overview](#2-architecture-overview)
3. [State Machine](#3-state-machine)
4. [Component: Tray Controller (tray.cpp)](#4-component-tray-controller-traycpp)
5. [Component: Capture Engine (capture.cpp)](#5-component-capture-engine-capturecpp)
6. [Component: Normalizer (normalizer.cpp)](#6-component-normalizer-normalizercpp)
7. [Component: Uploader (uploader.cpp)](#7-component-uploader-uploadercpp)
8. [Component: Teams Monitor (teams.cpp)](#8-component-teams-monitor-teamscpp)
9. [Component: Main / State Coordinator (main.cpp)](#9-component-main--state-coordinator-maincpp)
10. [Message Protocol](#10-message-protocol)
11. [File Layout and Naming](#11-file-layout-and-naming)
12. [Audio Capture Details](#12-audio-capture-details)
13. [Build System](#13-build-system)
14. [Key Design Decisions](#14-key-design-decisions)
15. [Known Issues and Limitations](#15-known-issues-and-limitations)

---

## 1. Goals and Constraints

**Goal:** A portable, no-install Win32 C++ tray application that records Microsoft Teams call audio (and any system audio) and uploads a 16 kHz mono WAV to Google Drive automatically. Long recordings are split into ~35-minute chunks, and Teams calls are tagged with the meeting name in the output filename.

**Hard constraints:**
- No installer — single `winrec.exe` plus `rclone.exe` in the same folder.
- No Visual C++ redistributables — statically linked with MinGW-w64.
- No external libraries beyond the Windows API and C/C++ standard library.
- Must work on Windows 10 and 11, 64-bit.
- Cross-compiled from Linux Mint using `x86_64-w64-mingw32-g++`.

**Soft constraints discovered during development:**
- Corporate DLP software may block WASAPI loopback — must have a WaveIn fallback.
- Only playback devices that expose a Stereo Mix WaveIn entry can be captured in fallback mode. Bluetooth and most USB headsets do not.
- rclone OAuth tokens can expire or be missing — auto-reconnect must be transparent.
- Microsoft Teams Third-Party App API uses `canLeave` permission (not `isInMeeting`) to signal active call state.

---

## 2. Architecture Overview

winrec is a single-process, message-driven application. All state transitions happen on the main thread via Win32 messages. All blocking work (audio capture, resampling, upload) happens on dedicated worker threads that post messages back to the main window on completion.

```
┌─────────────────────────────────────────────────────────────────┐
│  Main Thread (Win32 Message Loop)                               │
│                                                                 │
│  ┌─────────────┐    ┌──────────────┐    ┌──────────────────┐  │
│  │  Tray Icon  │    │  State       │    │  Hidden Window   │  │
│  │  (tray.cpp) │    │  Machine     │    │  (WndProc)       │  │
│  └─────────────┘    │  (main.cpp)  │    └──────────────────┘  │
│                     └──────────────┘                           │
└────────────────────────────┬────────────────────────────────────┘
                             │  PostMessageW
         ┌───────────────────┼───────────────────────┐
         │                   │                       │
┌────────▼──────┐  ┌─────────▼──────┐  ┌────────────▼──────────┐
│ Capture       │  │ Normalizer      │  │ Uploader              │
│ Thread        │  │ Thread          │  │ Thread                │
│ (capture.cpp) │  │ (normalizer.cpp)│  │ (uploader.cpp)        │
│               │  │                 │  │                       │
│ WM_APP_       │  │ WM_APP_         │  │ WM_APP_UPLOAD_DONE    │
│ RECORDING_DONE│  │ NORM_DONE       │  │                       │
└───────────────┘  └─────────────────┘  └───────────────────────┘

┌───────────────────────────────────────────────────────────────┐
│ Teams Monitor Thread (teams.cpp)                              │
│                                                               │
│ WebSocket → localhost:8124  →  WM_APP_TEAMS_CALL_START/END   │
│ On CALL_START: EnumWindows scrape of Teams window title      │
│                → meeting name for output filenames           │
└───────────────────────────────────────────────────────────────┘
```

---

## 3. State Machine

The application is always in exactly one of five states:

```
enum class RecorderState { Idle, Recording, Normalizing, Uploading, Error };
```

```
        ┌──────────────────────────────────────────────────────────┐
        │                                                          │
        ▼                                                          │
     [Idle] ──── left-click / Teams CALL_START ────► [Recording]  │
                                                         │        │
                                         left-click /   │        │
                                   Teams CALL_END        │        │
                                                         ▼        │
                                                  [Normalizing]   │  (per chunk,
                                                         │        │   repeated)
                                              (auto, thread done) │
                                                         ▼        │
                                                  [Uploading] ────┘
                                             (auto, rclone done)

     any state ── unrecoverable failure ──► [Error] ── click ──► [Idle]
```

**Rules:**
- Only `Idle → Recording` is user-triggered (or Teams-triggered).
- All subsequent transitions are automatic (thread completion messages).
- Normalize and upload run once **per chunk**: the state cycles
  `Normalizing → Uploading` for each chunk in the batch before returning to Idle
  (see §9 for the chunk pipeline).
- Clicking in Normalizing or Uploading states is a no-op.
- Teams call-end only stops recording if state is `Recording` (ignores if already Idle or in pipeline).
- `Error` is a terminal state surfaced by the red tray icon; a left-click dismisses it back to Idle.

---

## 4. Component: Tray Controller (tray.cpp)

### Responsibility

Manages the system tray icon using `Shell_NotifyIcon`. Provides:
- `TrayAdd(hwnd)` — adds icon on startup
- `TrayUpdate(state)` — changes icon and tooltip to reflect current state
- `TrayRemove()` — removes icon on exit
- `TrayBalloon(title, msg)` — shows a balloon notification

### Icon colors

| State | Color | RGB |
|---|---|---|
| Idle | Sky blue | `0x87CEEB` |
| Recording | Mint green | `0x3CD482` |
| Normalizing | Amber | `0xFFC300` |
| Uploading | Amber | `0xFFC300` |
| Error | Red | `0xD45A5A` |

Icons are drawn at runtime by `MakeBubbleIcon(COLORREF)` — a filled circle with a small gloss highlight — and cached, then selected by state. The tray icon also serves as the sole visual indicator that winrec is running.

### Tray icon interaction

The hidden window receives `WM_TRAYICON` (defined as `WM_USER + 1`) with the mouse message in `lParam`. The WndProc handles:
- `WM_LBUTTONUP` / `WM_LBUTTONDBLCLK`: toggle Start/Stop.
- `WM_RBUTTONUP`: show context menu (Exit only).

---

## 5. Component: Capture Engine (capture.cpp)

### Responsibility

Captures audio from the Windows default playback device (loopback) and the default microphone simultaneously, mixes them to a mono stream, and writes raw 16-bit PCM to a temp file.

### Public API

```cpp
std::wstring CaptureStart(const std::wstring& rawPath, const SYSTEMTIME& startTime);
void         CaptureStop();

extern bool g_micFallback;   // true if mic open failed but loopback succeeded
extern bool g_usingWaveIn;   // true if WaveIn fallback is active
extern int  g_numLoops;      // number of WaveIn loopback devices opened
extern UINT32 g_captureInputRate;  // filled by CaptureStop, used by normalizer
```

### Primary path: WASAPI

1. `IMMDeviceEnumerator` → `GetDefaultAudioEndpoint(eRender, eConsole)` → loopback client.
2. `IMMDeviceEnumerator` → `GetDefaultAudioEndpoint(eCapture, eCommunications)` → mic client.
3. Each client initialized with `AUDCLNT_SHAREMODE_SHARED`. Loopback uses `AUDCLNT_STREAMFLAGS_LOOPBACK`.
4. Worker thread polls both `IAudioCaptureClient` interfaces every 10 ms.
5. Loopback frames (may be multi-channel float) are downmixed to mono float.
6. Mic frames (mono or stereo) are converted to mono float.
7. Ring buffers synchronize the two streams.
8. Mixed output: `0.5 × loopback_mono + 0.5 × mic_mono`, clamped, cast to `int16_t`, written to the `.pcm` file.

### Fallback path: WaveIn

If WASAPI loopback initialization fails (common with corporate DLP software), winrec falls back to the legacy WaveIn API:

1. `waveInGetNumDevs()` enumerates all recording devices.
2. `waveInGetDevCapsW()` reads each device name.
3. Devices whose name contains "stereo mix", "stereomix", "what u hear", "loopback", "wave out", or "mix" (case-insensitive) are classified as loopback sources.
4. **All** matching loopback devices are opened simultaneously (up to `MAX_LOOPS = 4`).
5. The first non-loopback device is opened as the microphone.
6. Each device uses double-buffering with `WAVEHDR` blocks of ~20 ms.
7. The worker thread sums all loopback sources into a single float buffer, then mixes with mic at 50/50.

Flags set on WaveIn activation:
- `g_usingWaveIn = true`
- `g_numLoops` = count of successfully opened loopback devices

The balloon at recording start reflects the fallback mode and loopback count.

### Thread lifecycle

`CaptureStart` spawns a single worker thread and returns immediately. `CaptureStop` sets `g_running = false` and returns immediately. The worker thread detects the flag, flushes and closes the file, then posts `WM_APP_RECORDING_DONE` (wParam=0 on success, 1 on error).

---

## 6. Component: Normalizer (normalizer.cpp)

### Responsibility

Runs in a dedicated thread spawned by `main.cpp` (once per chunk). Reads a raw `.pcm` file, resamples it to 16 kHz, and writes a standard PCM WAV.

> **Note:** Despite the file name, this stage no longer applies peak normalization. As of v1.3 it is a single-pass resampler only — see the rationale below. The name is retained to avoid churn in the message protocol and call sites.

### Parameters

```cpp
struct NormParams {
    std::wstring rawPath;    // input: tmp\raw_YYMMDD_HHMM.pcm
    std::wstring outPath;    // output: out\YYMMDD_HHMM-YYMMDD_HHMM[_Meeting].wav
    UINT32       inputRate;  // sample rate of rawPath (from g_captureInputRate)
    HWND         hwnd;
};
```

### Single-pass algorithm

- Open `rawPath` for read and `outPath` for write; emit a placeholder 44-byte RIFF/PCM WAV header (patched at the end with the true sample count).
- Step ratio: `inputRate / 16000.0`.
- For each output sample at index `i`, compute input position `i × step`, take the two nearest input samples (served from a small rolling input window so memory stays bounded for hour-long files), and linearly interpolate.
- Clamp the interpolated value to [-1, 1], convert to `int16_t`, and write directly to `outPath` in 8192-sample buffers.
- After the loop, `rewind` and rewrite the header with the actual output sample count.

**No gain stage.** Samples are written at their captured amplitude. The intermediate `.tmp` float file used by the old two-pass design is gone.

**Why drop normalization (v1.3):** the prior two-pass design scanned for the peak, then applied `0.95 / peak` gain. In practice this amplified background noise during quiet calls and added a full second read/write pass over multi-hundred-MB files. Speech-to-text engines (Whisper, Google/Azure STT) are robust to moderate level variation, so the gain stage was removed in favor of a faster single pass and a smaller memory/disk footprint.

**Cleanup:**
- Delete `rawPath` on success.
- Post `WM_APP_NORM_DONE` (wParam=0 success, 1 error).

---

## 7. Component: Uploader (uploader.cpp)

### Responsibility

Runs in a dedicated thread spawned by `main.cpp` after `WM_APP_NORM_DONE`. Invokes `rclone.exe move` to upload the WAV to Google Drive. Handles OAuth token expiry by triggering `rclone config reconnect` automatically.

### Parameters

```cpp
struct UploadParams {
    std::wstring outPath;     // e.g. C:\winrec\out\260308_1430-260308_1512.wav
    std::wstring rclonePath;  // C:\winrec\rclone.exe
    std::wstring remote;      // gdrive:teams-audio/
    HWND         hwnd;
};
```

### Upload loop (2 attempts)

```
For attempt = 0, 1:
    Run: rclone.exe move "<outPath>" "<remote>" --log-file "<rclone_log.txt>"
    Wait for exit.
    If exit code == 0: success, break.
    Read last ~300 chars of rclone_log.txt.
    If attempt == 0 and log contains "empty token" / "oauth" / "token":
        Run: rclone.exe config reconnect <remoteName>: --log-level DEBUG --log-file "<rcLogPath>"
             (pipe "y\n" to stdin to answer "Use web browser?" prompt)
        Wait up to 5 minutes.
        If reconnect exit code == 0: log "Re-authorized. Retrying upload…" and continue.
        Else: log reconnect error detail, break.
    Else: log upload error, break.
```

### rclone process creation

The upload rclone process is created with `CREATE_NO_WINDOW` and stdout/stderr redirected to a log file via `STARTF_USESTDHANDLES`. The log file is created with `CREATE_ALWAYS` each iteration so it is clean.

The reconnect rclone process is created differently:
- `CREATE_NO_WINDOW` (no console shown — rclone handles the browser launch directly).
- A pipe provides `"y\n"` to `hStdInput` to answer the "Use web browser?" prompt non-interactively.
- `hStdOutput` and `hStdError` are `nullptr` — rclone writes its reconnect log via `--log-file`.
- rclone owns the reconnect log file exclusively; winrec does not open it during reconnect.

### Remote name extraction

```cpp
// From "gdrive:teams-audio/" extract "gdrive"
size_t colon = up.remote.find(L':');
std::wstring remoteName = (colon != std::wstring::npos)
                          ? up.remote.substr(0, colon)
                          : up.remote;
```

### Cleanup

On success: delete `rclone_log.txt`. On failure: leave it for diagnosis.

Post `WM_APP_UPLOAD_DONE` (wParam=0 success, 1 error).

---

## 8. Component: Teams Monitor (teams.cpp)

### Responsibility

Runs in a persistent background thread for the entire lifetime of the application. Connects to the Microsoft Teams Third-Party App API via WebSocket and posts `WM_APP_TEAMS_CALL_START` or `WM_APP_TEAMS_CALL_END` to the main window when call state changes.

### Teams Third-Party App API

Teams runs a local WebSocket server on `localhost:8124` when the "Third-party app API" Privacy setting is enabled. The API:
- Sends meeting/call state updates in JSON as WebSocket text frames.
- Uses a token (UUID) for app identification and pairing.
- Shows a one-time pairing prompt in Teams UI on first connection.
- Persists paired apps in Teams' "Allowed apps and devices" list.

### Token management

On startup, `GetOrCreateToken()`:
1. Reads `winrec_teams_token.txt` from the exe directory if it exists.
2. If not found, generates a new UUID via `CoCreateGuid`, saves it, and returns it.

The same token is used for every reconnection, ensuring Teams recognises winrec after the initial pairing.

### WebSocket URL

```
ws://localhost:8124/?token=<UUID>&protocol-version=2.0.0
    &manufacturer=WinRec&device=WinRec&app=WinRec&app-version=1.0.0
```

### WinHTTP WebSocket implementation

winrec uses the WinHTTP WebSocket API (Windows 8+):

```
WinHttpOpen → WinHttpConnect(localhost, 8124)
           → WinHttpOpenRequest(GET, path)
           → WinHttpSetOption(UPGRADE_TO_WEB_SOCKET)
           → WinHttpSendRequest
           → WinHttpReceiveResponse
           → WinHttpWebSocketCompleteUpgrade → hWs
           → WinHttpWebSocketReceive loop
```

Each received frame (up to 8191 bytes) is null-terminated and inspected with `strstr`.

### Meeting state detection

Teams sends `meetingPermissions` updates containing a `canLeave` field:

```json
{ "meetingPermissions": { "canLeave": true, ... }, ... }
```

```cpp
static bool IsInMeeting(const char* msg)
{
    return strstr(msg, "\"canLeave\":true")  != nullptr ||
           strstr(msg, "\"canLeave\": true") != nullptr;
}
static bool IsNotInMeeting(const char* msg)
{
    return strstr(msg, "\"canLeave\":false")  != nullptr ||
           strstr(msg, "\"canLeave\": false") != nullptr;
}
```

`canLeave: true` = in an active call or meeting.
`canLeave: false` = not in a call.

**Note:** An earlier implementation looked for `"isInMeeting":true`, which Teams does not send. The correct field is `canLeave`.

### Meeting name capture (v1.3)

On each `canLeave: false → true` transition (call start), `CaptureMeetingName()` runs before `WM_APP_TEAMS_CALL_START` is posted. The Third-Party App API does not expose the meeting subject, so the name is scraped from the Teams window title via `EnumWindows`:

1. **Format 1** — `"<Meeting Name> | Microsoft Teams"`: take the text before ` | Microsoft Teams`, dropping any extra ` | org | email` segments.
2. **Format 2** — `"Microsoft Teams – <Meeting Name>"` (en-dash or hyphen): take the text after the prefix.
3. **Fallback** — for 1:1 calls the title is just the contact name with no Teams marker, so any visible window owned by `ms-teams.exe` / `Teams.exe` (checked via `QueryFullProcessImageNameW`) supplies its title.

The captured string is sanitized for use in a filename: truncated to 60 chars, illegal/space characters (`<>:"/\|?*` and space) replaced with `_`, runs of `_` collapsed, and leading/trailing `_` trimmed. The result is exposed via `TeamsGetMeetingName()`; `main.cpp` snapshots it into `g_callMeetingName` at `CALL_START` and uses it for every chunk filename of that call. Manual recordings leave it empty.

All enumeration steps are written to `winrec_teams_log.txt` for diagnosis.

### Reconnection loop

If Teams is not running, or the connection drops, the thread sleeps 5 seconds and retries indefinitely. When Teams starts, the connection is established automatically within ~5 seconds.

### Thread lifecycle

`TeamsMonitorStart(hwnd)` spawns the thread and sets `g_teamsRunning = true`.
`TeamsMonitorStop()` sets `g_teamsRunning = false` and waits up to 3 seconds for the thread; terminates if it times out.

### Debug log

All connection events are written to `winrec_teams_log.txt` in the exe directory via `TeamsLog()` with timestamps.

---

## 9. Component: Main / State Coordinator (main.cpp)

### Responsibility

- Registers the hidden window class and creates the message-only window (`HWND_MESSAGE`).
- Runs `WinMain` and the Win32 message loop.
- Orchestrates all state transitions in `HiddenWndProc`.
- Spawns `NormalizerThread` and `UploaderThread` (capture.cpp handles its own thread).
- Splits recordings into chunks and drives the per-chunk normalize/upload batch.

### Key globals

```cpp
AppState                g_app  = {};     // current state, hwnd, file paths, timestamps
std::wstring            g_exeDir;        // set once at startup from GetModuleFileNameW
std::wstring            g_callMeetingName;  // snapshot of TeamsGetMeetingName() at CALL_START
bool                    g_isTeamsCall;   // distinguishes Teams call from manual recording
std::vector<SplitChunk> g_chunks;        // pending chunks awaiting normalize+upload
int                     g_normChunkIdx;  // index of chunk currently normalizing
int                     g_uploadChunkIdx;// index of chunk currently uploading
```

### Chunked recording pipeline (v1.3)

Both manual and Teams recordings now share one pipeline. A `WM_TIMER` (`SPLIT_TIMER_ID`) fires every `SPLIT_INTERVAL_SECONDS` (2100 s ≈ 35 min) while `Recording`. On each fire — and on stop — the current capture is closed into a `SplitChunk` (raw path + start/end times + computed `outPath`), `g_chunks` grows, and capture restarts for the next chunk. `ComputeChunkOutPath()` builds `out\<start>-<end>[_<MeetingName>].wav`, appending `g_callMeetingName` when non-empty.

When recording stops, the batch runs sequentially: normalize chunk 0 → upload chunk 0 → normalize chunk 1 → … Each `OnNormDone` advances `g_normChunkIdx`; once all chunks are normalized, uploads begin and `OnUploadDone` advances `g_uploadChunkIdx`, firing an "Upload complete: \<name\>" balloon per chunk. After the last upload, `g_chunks`, `g_isTeamsCall`, and `g_callMeetingName` reset and the app returns to Idle. Splitting bounds per-file size and memory, and keeps a single long meeting recoverable as discrete uploads.

### State transition handlers

| Function | Transition | Trigger |
|---|---|---|
| `StartRecording()` | Idle → Recording | Left-click or `WM_APP_TEAMS_CALL_START`; arms split timer |
| `StopRecording()` | Recording → (async) | Left-click or `WM_APP_TEAMS_CALL_END`; closes final chunk |
| `OnRecordingDone(ok)` | → batch normalize | `WM_APP_RECORDING_DONE` |
| `OnNormDone(ok)` | → next chunk / Uploading | `WM_APP_NORM_DONE` |
| `OnUploadDone(ok)` | → next chunk / Idle | `WM_APP_UPLOAD_DONE` |
| `EnterIdle()` / `EnterError()` | → Idle / Error | Error paths |

### Teams auto-detection handlers

```cpp
case WM_APP_TEAMS_CALL_START:
    if (g_app.state == RecorderState::Idle) {
        g_callMeetingName = TeamsGetMeetingName();  // snapshot before next call clears it
        g_isTeamsCall     = true;
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
```

The CALL_END handler uses `RecorderState::Recording` guard to ensure it cannot interrupt the Normalizing or Uploading pipeline. If the user manually starts a recording with no Teams call, Teams CALL_END will stop it — this is the intended behaviour.

### Startup sequence

```cpp
CoInitializeEx(nullptr, COINIT_MULTITHREADED);
g_exeDir = GetExeDir();
// Register WndClass, create HWND_MESSAGE window
g_app.state = RecorderState::Idle;
TrayAdd(g_app.hwnd);
TeamsMonitorStart(g_app.hwnd);
// Enter message loop
```

### Shutdown sequence

```cpp
// On WM_COMMAND IDM_EXIT:
if (g_app.state == RecorderState::Recording) CaptureStop();
TrayRemove();
PostQuitMessage(0);
// After message loop:
TeamsMonitorStop();
CoUninitialize();
```

---

## 10. Message Protocol

All inter-thread communication goes through `PostMessageW` to the main hidden window.

| Message constant | Value | Sender | wParam | Meaning |
|---|---|---|---|---|
| `WM_TRAYICON` | `WM_USER+1` | Windows (Shell) | — | Mouse event on tray icon |
| `WM_APP_RECORDING_DONE` | `WM_USER+2` | capture thread | 0=ok, 1=err | Capture thread exited |
| `WM_APP_NORM_DONE` | `WM_USER+3` | normalizer thread | 0=ok, 1=err | Normalization complete |
| `WM_APP_UPLOAD_DONE` | `WM_USER+4` | uploader thread | 0=ok, 1=err | Upload complete |
| `WM_APP_TEAMS_CALL_START` | `WM_USER+5` | Teams monitor thread | — | Call/meeting joined |
| `WM_APP_TEAMS_CALL_END` | `WM_USER+6` | Teams monitor thread | — | Call/meeting left |

Using `PostMessageW` (not `SendMessageW`) ensures background threads never block on the main thread.

---

## 11. File Layout and Naming

### Runtime files

```
<exe dir>\
    winrec.exe
    rclone.exe
    winrec_teams_token.txt      UUID for Teams pairing (auto-created on first run)
    winrec_teams_log.txt        Teams connection debug log (appended each run)
    rclone_log.txt              Upload rclone log (deleted on success)
    tmp\
        raw_YYMMDD_HHMM.pcm    Raw 16-bit mono PCM at device sample rate (deleted after resample)
    out\
        YYMMDD_HHMM-YYMMDD_HHMM[_Meeting].wav   16 kHz mono WAV (deleted after upload)
```

The intermediate `.tmp` float buffer no longer exists — the normalizer is single-pass (see §6).

### Timestamp format

`FormatTimestamp(SYSTEMTIME)` → `YYMMDD_HHMM` using local time.

Example: 14:30 on 8 March 2026 → `260308_1430`.

### Output filename

`ComputeChunkOutPath(start, end)` builds:

```
<start>-<end>[_<MeetingName>].wav
```

- Manual recording: `260308_1430-260308_1512.wav`.
- Teams call: `260308_1430-260308_1512_Weekly_Sync.wav` — the sanitized meeting name (see §8) is appended after an underscore.
- A long recording produces several files, one per ~35-min chunk, each with its own start/end stamp and (for Teams) the same meeting name.

Colons are excluded from filenames because Windows does not allow them in paths.

---

## 12. Audio Capture Details

### WASAPI path — format handling

The loopback mix format is queried with `IAudioClient::GetMixFormat`. Common results:
- 48 000 Hz, 2 channels, 32-bit float (`WAVE_FORMAT_IEEE_FLOAT`).
- 44 100 Hz, 2 channels, 32-bit float.

The worker thread reads raw float frames from `IAudioCaptureClient::GetBuffer`, downmixes stereo to mono, and scales by 0.5.

The microphone is typically 16 000 Hz or 48 000 Hz, 1 channel, 16-bit integer or 32-bit float. If 16-bit integer, samples are divided by 32768.0f before mixing.

The raw `.pcm` file stores the mixed output as `int16_t` at the loopback device's native rate. The actual rate is stored in `g_captureInputRate` for the normalizer.

### WaveIn path — device enumeration

```cpp
static bool IsLoopbackDevice(const wchar_t* name)
{
    // case-insensitive search in device name
    const wchar_t* keywords[] = {
        L"stereo mix", L"stereomix", L"what u hear",
        L"loopback", L"wave out", L"mix"
    };
    // returns true if any keyword found
}
```

WaveIn devices use `waveInOpen` with double-buffering. Each `WAVEHDR` covers ~20 ms of audio. The callback (`waveInProc`) copies audio to a ring buffer. The worker thread drains all loopback ring buffers, sums them, mixes with the mic ring buffer, and writes to the PCM file.

### Ring buffer

A simple circular buffer of `int16_t` samples, protected by a `CRITICAL_SECTION`, is used to decouple WaveIn callbacks from the mixer thread. When a ring buffer has more data than needed for the current mix iteration, excess stays in the buffer for the next iteration.

---

## 13. Build System

### Makefile

```makefile
CXX     := x86_64-w64-mingw32-g++
WINDRES := x86_64-w64-mingw32-windres

CXXFLAGS := -std=c++17 -O2 -Wall \
            -DUNICODE -D_UNICODE \
            -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00

LDFLAGS := -mwindows -static-libgcc -static-libstdc++

LIBS := -lole32 -loleaut32 -luuid \
        -lshell32 -luser32 -lcomctl32 \
        -lmmdevapi -lwinmm -lwinhttp

SRCS := src/main.cpp src/tray.cpp src/capture.cpp \
        src/normalizer.cpp src/uploader.cpp src/teams.cpp
```

### Library requirements

| Library | Used for |
|---|---|
| `ole32`, `oleaut32`, `uuid` | COM initialization, `CoCreateGuid` |
| `shell32` | `Shell_NotifyIcon` (tray) |
| `user32` | Window management, menus, `PostMessageW` |
| `comctl32` | Common controls (balloon notifications) |
| `mmdevapi` | WASAPI device enumeration and capture |
| `winmm` | WaveIn API (`waveInOpen`, `waveInAddBuffer`, etc.) |
| `winhttp` | Teams WebSocket connection |

### Build prerequisites

```bash
sudo apt install mingw-w64   # Ubuntu/Debian/Mint
make
```

Output: `winrec.exe` in the project root.

---

## 14. Key Design Decisions

### No main window

winrec has no visible window. The `HWND` created with `HWND_MESSAGE` is a message-only window (no taskbar button, no Alt+Tab entry). All UI is through the tray icon and balloon notifications. This keeps the app unobtrusive and avoids focus conflicts during calls.

### Async pipeline via PostMessage

Each pipeline stage (capture, normalize, upload) runs on its own thread and signals completion via `PostMessageW`. This keeps the Win32 message loop responsive for tray events even during long uploads. No shared mutable state is accessed without synchronization.

### WaveIn fallback for corporate environments

WASAPI loopback is blocked by some corporate DLP products (e.g. Symantec, Trellix). The WaveIn fallback allows winrec to function in those environments by using the Stereo Mix legacy recording device. Multiple loopback devices are opened simultaneously to handle configurations where more than one Stereo Mix entry exists.

The practical consequence: **call audio can only be captured if the default Windows playback endpoint exposes a Stereo Mix WaveIn device**. Laptop speakers typically do; Bluetooth and USB headsets typically do not.

### rclone for Google Drive upload

rclone handles the entire OAuth2 flow, chunked upload, and retry logic for Google Drive. winrec only needs to invoke `rclone.exe move` and check the exit code. This avoids implementing Drive API client code.

Auto-reconnect was added after discovering that rclone's OAuth tokens stored during `rclone config` (interactive browser flow) are not the same as tokens generated by `rclone config reconnect`. The reconnect sub-command performs the browser OAuth flow and writes a valid token. Automating the "y\n" stdin response eliminates the interactive prompt.

### Teams canLeave vs isInMeeting

The Teams Third-Party App API documentation suggests looking for `isInMeeting`, but the actual payloads Teams sends contain `meetingPermissions.canLeave`. The `canLeave` field is `true` when you are in an active call and `false` when you have left. This was discovered by examining the raw WebSocket messages in `winrec_teams_log.txt`.

### Teams pairing via persistent UUID token

The token is generated once with `CoCreateGuid` and saved to `winrec_teams_token.txt`. Using the same token on every startup means Teams only shows the pairing prompt once per machine, and winrec appears permanently in Teams' "Allowed apps and devices" list.

---

## 15. Known Issues and Limitations

| Issue | Detail |
|---|---|
| No Stereo Mix on Bluetooth/USB headsets | Hardware limitation; workaround is to use laptop speakers as Teams audio output |
| WASAPI loopback blocked by corporate DLP | WaveIn fallback partially mitigates this |
| No per-device volume control | Fixed 50/50 mix; no gain correction is applied (normalization was removed in v1.3) |
| Linear interpolation aliasing | Inaudible for speech; would require a polyphase FIR filter to fix |
| rclone reconnect requires browser | Cannot work in headless/locked sessions; user must be present |
| Teams Third-Party App API policy | Some organizations disable this setting; auto-detection unavailable in those cases |
| Single recording session | Cannot start a new recording during Normalizing or Uploading |
| Exit during Normalizing/Uploading | Background threads are terminated; partial files may remain |
| Teams log grows unbounded | `winrec_teams_log.txt` is appended on every run; manually delete if it grows large |
