# winrec User Manual

**Version 1.2 — March 2026**

---

## Table of Contents

1. [Overview](#1-overview)
2. [System Requirements](#2-system-requirements)
3. [Installation](#3-installation)
4. [First-Run Setup](#4-first-run-setup)
5. [The Tray Icon](#5-the-tray-icon)
6. [Recording a Session](#6-recording-a-session)
7. [Teams Auto-Detection](#7-teams-auto-detection)
8. [Post-Recording Pipeline](#8-post-recording-pipeline)
9. [File Locations and Naming](#9-file-locations-and-naming)
10. [Audio Capture Behaviour](#10-audio-capture-behaviour)
11. [Upload to Google Drive](#11-upload-to-google-drive)
12. [Transcript Delivery to OneDrive](#12-transcript-delivery-to-onedrive)
13. [Error Notifications](#13-error-notifications)
14. [Exiting the Application](#14-exiting-the-application)
15. [Building from Source](#15-building-from-source)
16. [Known Limitations](#16-known-limitations)
17. [Security Considerations](#17-security-considerations)

---

## 1. Overview

**winrec** is a portable, no-install Windows tray application that records the audio from a Microsoft Teams call — or any system audio — and saves it as a transcription-ready WAV file, then uploads it automatically to Google Drive.

It captures two audio streams simultaneously:

- **System output (loopback):** everything you hear through your speakers — remote participants, shared media, system sounds.
- **Microphone:** your own voice.

The two streams are mixed together in real time into a single mono track, then — after you stop the recording — normalized and resampled to **16 kHz mono 16-bit PCM WAV**. This format is compact and directly accepted by speech-to-text services (Whisper, Google STT, Azure STT, etc.).

After normalization, winrec calls `rclone.exe` to move the finished file to `gdrive:teams-audio/` and then returns to idle.

**Teams auto-detection:** winrec can connect to the Microsoft Teams Third-Party App API and automatically start recording when you join a call and stop when you leave. No manual clicks required.

---

## 2. System Requirements

| Component | Requirement |
|---|---|
| OS | Windows 10 or Windows 11, 64-bit |
| Audio output device | A device set as the Windows default playback device that exposes a **Stereo Mix** recording input (laptop speakers recommended; see §10 for details) |
| Audio input device | Any microphone set as the Windows default recording device |
| Disk space | Enough for temporary raw PCM files (≈ 5.5 MB per minute at 48 kHz) plus the final WAV (≈ 1.9 MB per minute at 16 kHz) |
| Network | Internet access for the rclone upload step |
| rclone | `rclone.exe` version 1.60 or later, placed in the same folder as `winrec.exe` |
| Microsoft Teams | Optional — required only for auto-detection of call start/end |

No Visual C++ redistributable or .NET runtime is required. winrec is statically linked.

---

## 3. Installation

winrec has no installer. To deploy it:

1. Create a folder, for example `C:\winrec\`.
2. Copy `winrec.exe` into that folder.
3. Copy a pre-built `rclone.exe` into the same folder.
4. (Optional) Create a shortcut to `winrec.exe` in your Startup folder so it launches automatically at logon:
   - Press `Win + R`, type `shell:startup`, press Enter.
   - Right-click inside that folder → New → Shortcut → browse to `winrec.exe`.

On first launch, winrec automatically creates:

```
C:\winrec\
    winrec.exe
    rclone.exe
    tmp\          ← created automatically (raw in-progress recordings)
    out\          ← created automatically (normalized WAVs awaiting upload)
```

---

## 4. First-Run Setup

### 4.1 Configure rclone

winrec uses `rclone.exe` to upload recordings to Google Drive. Before running `rclone config`, you must have a Google Cloud project with OAuth credentials:

1. Go to [Google Cloud Console](https://console.cloud.google.com/) → APIs & Services → Credentials.
2. Create an OAuth 2.0 Client ID (type: Desktop App). Copy the **Client ID** and **Client Secret**.
3. Open a **Command Prompt** and run the interactive configuration wizard:
   ```
   rclone config
   ```
4. Choose **n** (New remote), and when asked for a name enter exactly:
   ```
   gdrive
   ```
5. Select **Google Drive** as the storage type.
6. Enter your **Client ID** and **Client Secret** when prompted.
7. Follow the prompts to authorize rclone with your Google account. A browser window will open for OAuth consent.
8. After configuration, verify access:
   ```
   rclone ls gdrive:
   ```
   A listing of your Drive root confirms success.

> **Important:** Using your own OAuth credentials prevents Google from revoking access to a shared test app. The Client ID and Client Secret are stored in `rclone.conf` — see §16 for security guidance.

### 4.2 Enable Stereo Mix

winrec captures system audio via the **Stereo Mix** recording device. Enable it in Windows:

1. Run `mmsys.cpl` (Win + R, type `mmsys.cpl`, Enter).
2. Go to the **Recording** tab.
3. Right-click in the device list → **Show Disabled Devices**.
4. Right-click **Stereo Mix** → **Enable**.

> **Audio output requirement:** Stereo Mix captures audio from the device currently set as the Windows default output. For Teams calls, set your audio output to **laptop speakers** in Teams Settings → Devices. Bluetooth and most USB headsets do not expose a Stereo Mix device and their audio will not be captured. See §10 for full details.

### 4.3 Enable Teams auto-detection (optional)

winrec can automatically start and stop recording when you join or leave a Teams call.

1. Open Microsoft Teams → your profile picture → **Settings → Privacy**.
2. Find **Third-party app API** and toggle it **on**.
3. Start winrec — it will connect to Teams automatically within a few seconds.
4. The first time winrec connects, Teams will show a pairing prompt — click **Allow**.

After pairing, winrec appears in Teams' "Allowed apps and devices" list permanently. You only need to approve once per machine.

---

## 5. The Tray Icon

winrec runs entirely in the notification area (system tray). No main window ever opens.

### 5.1 Finding the icon

The icon appears in the notification area at the right end of the taskbar. On Windows 10/11 some icons are hidden behind the `^` overflow arrow. To keep it always visible:

Windows Settings → Personalization → Taskbar → Other system tray icons → enable **winrec**.

### 5.2 Icon appearance by state

| State | Icon color | Tooltip |
|---|---|---|
| Idle | Sky blue | winrec – Idle (click to start) |
| Recording | Mint green | winrec – Recording… |
| Normalizing | Mint green | winrec – Normalizing… |
| Uploading | Mint green | winrec – Uploading… |

### 5.3 Left-click behaviour

| Current state | Result |
|---|---|
| Idle | Starts recording |
| Recording | Stops recording and begins normalization |
| Normalizing / Uploading | No action (click is ignored) |

### 5.4 Right-click context menu

Right-clicking the tray icon shows a context menu with **Exit**. Selecting Exit stops any active recording and quits winrec.

---

## 6. Recording a Session

### 6.1 Manual recording

**Start:** Left-click the tray icon while idle. The icon turns mint green and a balloon shows *Recording started at HH:MM*.

**Stop:** Left-click again. winrec notes the stop time and begins the post-recording pipeline (§8). A balloon shows *Recording stopped; normalizing…*

### 6.2 Automatic recording (Teams)

When Teams auto-detection is enabled (§4.3), winrec starts recording automatically when you join a meeting or call, and stops automatically when you leave. See §7 for full details.

### 6.3 During recording

The icon stays mint green throughout. Audio data is written continuously to a temporary `.pcm` file in `tmp\`. No CPU-heavy processing happens during capture — disk writes are the only ongoing activity.

You can continue working normally. winrec has no visible window and no audible impact on playback.

---

## 7. Teams Auto-Detection

### 7.1 How it works

winrec connects to the Microsoft Teams Third-Party App API — a local WebSocket server that Teams runs on port 8124 when the Third-party app API setting is enabled.

On connection, Teams begins sending real-time meeting state updates. winrec watches for the `canLeave` permission flag:

- `canLeave: true` → you are in an active call or meeting → winrec posts an internal start signal.
- `canLeave: false` → you have left the call → winrec posts an internal stop signal.

### 7.2 Pairing

The first time winrec connects to Teams, Teams shows a **pairing prompt** asking you to allow or deny access. Click **Allow**. After pairing:

- winrec's token is saved to `winrec_teams_token.txt` in the exe folder.
- Teams adds winrec to its "Allowed apps and devices" list permanently.
- Future connections are automatic — no prompts.

### 7.3 Automatic start behaviour

When a call is detected and winrec is **Idle**, it:
1. Shows a balloon: *Call detected – recording started automatically.*
2. Starts recording immediately.
3. The tray icon turns mint green.

If winrec is already recording (manually started), the auto-detection event is ignored — the current recording continues.

### 7.4 Automatic stop behaviour

When call end is detected and winrec is **Recording**, it:
1. Shows a balloon: *Call ended – stopping recording.*
2. Stops recording and begins the pipeline.

This applies whether the recording was started manually or automatically.

### 7.5 Teams not running

If Teams is not running (or the Third-party app API is disabled), winrec simply retries the WebSocket connection every 5 seconds in the background. Manual recording works normally in the meantime.

### 7.6 Debug log

winrec writes connection events to `winrec_teams_log.txt` in the exe folder. Check this file if auto-detection is not working.

---

## 8. Post-Recording Pipeline

After stopping, winrec runs three automatic steps without any user action required.

### 8.1 Normalization and resampling (Normalizing state)

The raw PCM file (16-bit mono at the device's native sample rate, typically 48 kHz) is processed in two passes:

**Pass 1 — Resample and scan for peak:**
- Each output sample at 16 kHz is computed from surrounding input samples using linear interpolation.
- The maximum absolute sample value (peak) across the entire file is tracked.
- Resampled float data is written to a temporary `.tmp` file.

**Pass 2 — Apply gain and write WAV:**
- Normalization gain: `gain = 0.95 / peak` (5% headroom to avoid hard clipping).
- If the recording is near silence (peak < 0.000001), gain defaults to 1.0.
- Each resampled sample is multiplied by gain, clamped to [-1, 1], and converted to 16-bit signed integer.
- A standard 44-byte PCM WAV header is written at the start of the output file.

After Pass 2 succeeds:
- The raw `.pcm` file is deleted.
- The intermediate `.tmp` file is deleted.
- The final `.wav` file remains in `out\`.

### 8.2 Upload (Uploading state)

winrec calls `rclone.exe move` with the path to the final WAV and the destination `gdrive:teams-audio/`. The `move` command uploads the file and deletes it locally on success.

**OAuth auto-reconnect:** If rclone reports an authentication error (empty or expired token), winrec automatically runs `rclone config reconnect gdrive:` in the background, feeding `y` to the "Use web browser?" prompt. A browser window opens for you to re-authorize. After you complete the authorization, winrec retries the upload automatically — no manual CLI steps required.

winrec waits for rclone to exit and checks its exit code:
- **Exit code 0:** Upload succeeded. Balloon: *Upload complete: filename.wav*. Returns to Idle.
- **Non-zero exit code:** Upload failed. Balloon: *Upload failed. WAV kept in out\ folder.* The WAV file is kept for manual retry. Returns to Idle.

### 8.3 State flow summary

```
Idle
 │  (left-click, or Teams call detected)
 ▼
Recording  ──────────────────────────── raw_YYMMDD_HHMM.pcm written to tmp\
 │  (left-click, or Teams call ended)
 ▼
Normalizing ─────────────── resample + normalize → out\YYMMDD_HHMM-YYMMDD_HHMM.wav
 │  (auto)
 ▼
Uploading  ─────────────── rclone move → gdrive:teams-audio/  (reconnect if needed)
 │  (auto)
 ▼
Idle
```

---

## 9. File Locations and Naming

### 9.1 Directory layout

All paths are relative to the folder containing `winrec.exe`.

```
<exe dir>\
    winrec.exe
    rclone.exe
    winrec_teams_token.txt      ← Teams pairing token (auto-created)
    winrec_teams_log.txt        ← Teams connection log
    tmp\
        raw_YYMMDD_HHMM.pcm     ← active recording (deleted after normalization)
        raw_YYMMDD_HHMM.tmp     ← resampled floats (deleted after WAV write)
    out\
        YYMMDD_HHMM-YYMMDD_HHMM.wav  ← final WAV (deleted after upload)
```

Under normal operation both `tmp\` and `out\` are empty when winrec is idle.

### 9.2 Filename format

Timestamps use local 24-hour time, format `YYMMDD_HHMM`:

| Component | Meaning |
|---|---|
| `YY` | Two-digit year (e.g. `26` for 2026) |
| `MM` | Month, zero-padded |
| `DD` | Day, zero-padded |
| `_` | Literal underscore |
| `HH` | Hour in 24-hour format, zero-padded |
| `MM` | Minute, zero-padded |

**Example:** A recording started at 14:30 and stopped at 15:12 on 8 March 2026 produces:

```
260308_1430-260308_1512.wav
```

### 9.3 Manual recovery

If a recording was interrupted (crash, power loss) or an upload failed, files may remain in `tmp\` or `out\`. These can be:

- Opened directly as PCM or WAV audio in any audio editor (Audacity, etc.).
- Uploaded manually: `rclone move "C:\winrec\out\name.wav" gdrive:teams-audio/`
- Deleted safely if the recording is no longer needed.

---

## 10. Audio Capture Behaviour

### 10.1 Primary capture mode: WASAPI

winrec first attempts to capture audio using WASAPI (Windows Audio Session API):

- **Loopback:** Opens the default Windows playback device in loopback mode. Captures everything routed through that device (call audio, browser tabs, media players).
- **Microphone:** Opens the default Windows communications microphone.

WASAPI loopback works with any default playback device and does not require Stereo Mix or any special configuration.

> **Corporate DLP software:** Some corporate endpoint protection products block WASAPI loopback capture. If this occurs, winrec automatically falls back to WaveIn mode (see §10.2) and shows a balloon notification.

### 10.2 Fallback mode: WaveIn + Stereo Mix

If WASAPI loopback is blocked, winrec falls back to the legacy WaveIn API and scans all recording devices for Stereo Mix (loopback) devices. Device names containing "stereo mix", "what u hear", "loopback", "wave out", or "mix" (case-insensitive) are treated as loopback sources.

winrec opens **all** matching loopback devices simultaneously and sums their outputs. This ensures that if multiple Stereo Mix devices exist (e.g. one for speakers, one for a USB adapter), all are captured.

A balloon notification at recording start indicates which mode is active:
- *WaveIn mode – mic + N loopback source(s)* — fallback with loopback audio.
- *WaveIn mode – mic only (no loopback device found)* — fallback, no call audio captured.

### 10.3 Audio device requirements for call audio capture

| Output device | Stereo Mix available? | Call audio captured? |
|---|---|---|
| Laptop built-in speakers | Usually yes | Yes |
| USB audio adapter (cheap) | Sometimes | Check with `mmsys.cpl` |
| USB headset | Rarely | Usually no |
| Bluetooth headset/speaker | No | No |

**Recommendation:** Set Teams audio output to laptop speakers for best capture compatibility. Your mic can remain on any device (USB or Bluetooth headsets work fine for mic input).

### 10.4 Mixing

During capture, both streams are mixed with equal 50/50 weight:

```
mixed = 0.5 × loopback_mono + 0.5 × mic_mono
```

Multi-channel loopback is downmixed to mono before mixing: `(L + R) / 2`.

The 0.5 weight on each source provides headroom to prevent clipping during the live mix. Actual loudness balance is corrected in the normalization step.

### 10.5 Raw PCM format

The file in `tmp\` is raw 16-bit signed integer, mono, little-endian, at the native sample rate of the playback device (typically 48 000 Hz). There is no header.

---

## 11. Upload to Google Drive

### 11.1 rclone command used

winrec invokes:

```
rclone.exe move "<out\name.wav>" gdrive:teams-audio/
```

The `move` subcommand uploads the file and removes it locally on success.

### 11.2 Destination folder

Files are uploaded to `gdrive:teams-audio/`. The `teams-audio` folder is created automatically by rclone if it does not exist.

### 11.3 OAuth auto-reconnect

If rclone exits with a non-zero code and the rclone log contains an authentication error keyword ("empty token", "oauth", "token"), winrec:

1. Shows a balloon: *Drive auth expired — browser opening to re-authorize.*
2. Runs `rclone config reconnect gdrive:` in the background.
3. Automatically answers rclone's "Use web browser?" prompt.
4. Waits up to 5 minutes for you to complete the browser authorization.
5. Retries the upload once authorization is complete.

If the reconnect itself fails or times out, winrec shows an error balloon and leaves the WAV in `out\` for manual retry.

### 11.4 rclone configuration location

rclone stores its configuration (including OAuth tokens) in the user profile, typically:

```
C:\Users\<username>\AppData\Roaming\rclone\rclone.conf
```

This file contains access tokens for your Google account. See §16 for security guidance.

### 11.5 Network failures

If rclone fails to upload (no internet, quota exceeded, etc.), the WAV file is kept in `out\`. You can retry the upload manually:

```
rclone move "C:\winrec\out\<filename>.wav" gdrive:teams-audio/
```

---

## 12. Transcript Delivery to OneDrive

After a recording is uploaded and the Linux transcription app processes it, the resulting `.txt` transcript is uploaded back to `gdrive:teams-audio/`. winrec picks it up automatically and delivers it to your OneDrive.

### 12.1 How it works

A background thread checks `gdrive:teams-audio/` for `.txt` files every 60 seconds. The first check runs immediately at startup. When transcript files are found, winrec copies them to:

```
C:\Users\jmitchiner\OneDrive - Pomeroy\Brian@Home\winrec\transcripts
```

This folder is created automatically if it does not exist. OneDrive then syncs the files to the cloud within seconds, making them available to Microsoft Copilot.

### 12.2 Balloon notification

When transcripts are successfully copied, winrec shows:

> **winrec** — *Transcript(s) moved to OneDrive.*

If the rclone copy fails, no balloon is shown — check `rclone_log.txt` for details.

### 12.3 Files remain on Google Drive

The current implementation uses `rclone copy` (not `rclone move`), so originals remain on `gdrive:teams-audio/` after delivery. This is intentional while the pipeline is being verified. Once confirmed working, the source will be switched to `rclone move` to clean up Drive automatically.

### 12.4 End-of-day workflow with Copilot

Once transcripts are in OneDrive, Microsoft Copilot can reference them alongside your Teams messages, emails, and calendar to generate an end-of-day summary — what was accomplished, what is outstanding, and where to pick up tomorrow.

---

## 13. Error Notifications

winrec communicates all errors and status changes via Windows balloon notifications.

| Notification | Meaning |
|---|---|
| *Recording started at HH:MM* | Capture began successfully |
| *WaveIn mode – mic + N loopback source(s)* | WASAPI blocked; WaveIn fallback active with loopback |
| *WaveIn mode – mic only (no loopback device found)* | WASAPI blocked; only mic is captured |
| *Mic unavailable; recording system audio only* | Microphone could not be opened |
| *Call detected – recording started automatically* | Teams auto-detection triggered recording |
| *Call ended – stopping recording* | Teams auto-detection stopped recording |
| *Recording stopped; normalizing…* | Stop accepted; pipeline running |
| *Upload complete: filename.wav* | Full pipeline succeeded |
| *Transcript(s) moved to OneDrive.* | .txt files copied from Drive to OneDrive sync folder |
| *Drive auth expired — browser opening to re-authorize* | rclone OAuth token empty; reconnect starting |
| *Re-authorized. Retrying upload…* | Reconnect succeeded; upload retrying |
| *Upload failed. WAV kept in out\ folder* | rclone failed; file kept for manual retry |
| *Failed to initialize WASAPI capture* | No audio devices found, or device in exclusive use |
| *rclone.exe not found next to winrec.exe* | rclone.exe missing from the exe folder |
| *Normalization failed* | Could not read/write temp or output files |
| *Capture thread encountered an error* | Disk write failure during recording |

### Balloon visibility

Windows may suppress balloon notifications if you are in full-screen mode or Do Not Disturb is active. The tray icon tooltip always reflects the current state regardless of balloon visibility.

---

## 14. Exiting the Application

### Normal exit

Right-click the tray icon → **Exit**.

- If the state is **Idle**: winrec exits immediately.
- If the state is **Recording**: the capture thread is stopped, the raw PCM file is closed and left in `tmp\` (no normalization or upload is triggered on exit).
- If the state is **Normalizing** or **Uploading**: winrec exits; the background thread terminates.

### Startup with Windows

To have winrec start automatically:

1. Press `Win + R`, type `shell:startup`, press Enter.
2. Right-click in the folder → New → Shortcut.
3. Browse to `winrec.exe` and create the shortcut.

winrec will then launch silently at logon and sit idle in the tray until a recording is started.

---

## 15. Building from Source

winrec is written in C++17 and cross-compiled on Linux using MinGW-w64.

### 14.1 Prerequisites

Install the MinGW-w64 cross toolchain on Ubuntu/Debian/Mint:

```bash
sudo apt install mingw-w64
```

### 14.2 Build

```bash
cd /path/to/winrec
make
```

The output is `winrec.exe` in the project root.

### 14.3 Clean

```bash
make clean
```

### 14.4 Source layout

```
winrec/
    Makefile
    src/
        winrec.h        — shared types, enums, message IDs, function declarations
        main.cpp        — WinMain, hidden window, state machine, message loop
        tray.cpp        — Shell_NotifyIcon management and balloon notifications
        capture.cpp     — WASAPI loopback + mic capture; WaveIn fallback with multi-loopback
        normalizer.cpp  — two-pass resample + normalize, WAV header writer
        uploader.cpp    — rclone.exe invocation with OAuth auto-reconnect retry loop
        teams.cpp       — Teams Third-Party App API WebSocket monitor
        transcript_fetcher.cpp — polls gdrive for .txt files, copies to OneDrive
    documentation/
        winrec_design_doc.md
        quickstart.md
        user_manual.md
```

### 14.5 Compiler flags summary

| Flag | Purpose |
|---|---|
| `-std=c++17` | C++17 features |
| `-O2` | Optimization |
| `-mwindows` | Suppress console window; use WinMain entry point |
| `-DUNICODE -D_UNICODE` | Wide-character Windows API throughout |
| `-D_WIN32_WINNT=0x0A00` | Target Windows 10 API surface |
| `-static-libgcc -static-libstdc++` | No MinGW DLL dependencies at runtime |

Linked libraries: `ole32`, `oleaut32`, `uuid`, `shell32`, `user32`, `comctl32`, `mmdevapi`, `winmm`, `winhttp`.

---

## 16. Known Limitations

**Stereo Mix / loopback device required for call audio.** Without a Stereo Mix recording device enabled in Windows for the active playback endpoint, call participants' audio cannot be captured. Laptop speakers typically expose Stereo Mix; USB and Bluetooth headsets typically do not.

**Corporate DLP software.** Products that block WASAPI loopback cause winrec to fall back to WaveIn mode. If no Stereo Mix device is available, only the microphone is captured (no call audio).

**Device changes during a recording.** If the default audio device changes mid-recording (e.g. a USB headset is plugged in), the capture streams may stop delivering data or return errors. Stop and restart the recording after changing devices.

**No gain control during recording.** The live mix uses fixed 0.5/0.5 weights. Balance between loopback and mic cannot be adjusted without rebuilding.

**Linear interpolation resampling.** For 3:1 downsample (48 kHz → 16 kHz) the aliasing from linear interpolation is inaudible for speech, but may affect music recordings.

**Single session at a time.** Starting a new recording while one is in progress is not possible. The tray icon click is ignored until the full pipeline (including upload) completes.

**No rclone progress feedback.** winrec waits for rclone to finish and only checks the final exit code. Large files over slow connections leave winrec in the Uploading state for as long as rclone takes.

**Teams auto-detection requires Third-party app API.** If your organization's Teams policy disables this setting, auto-detection is unavailable. Manual recording still works normally.

---

## 17. Security Considerations

**rclone configuration file.** The file `%APPDATA%\Roaming\rclone\rclone.conf` contains OAuth refresh tokens that grant write access to your Google Drive. Protect it:

- Ensure only your Windows user account has read access to `rclone.conf`.
- Do not place `rclone.conf` in a shared or network-accessible folder.
- If a token is compromised, revoke it at your Google account's Connected Apps page and re-run `rclone config reconnect gdrive:`.

**Google OAuth credentials.** The Client ID and Client Secret in `rclone.conf` identify your Google Cloud project. Treat the Client Secret as a password.

**winrec.exe access.** Anyone who can run `winrec.exe` on this machine can start a recording that captures all system audio, including calls and media. Restrict access to the exe folder to authorised users.

**Teams pairing token.** The UUID in `winrec_teams_token.txt` identifies winrec to Teams. It grants the ability to receive meeting state events. It does not grant access to Teams messages, files, or contacts.

**Recorded audio.** Recordings contain the audio of all parties on the call. Ensure you comply with applicable laws regarding consent to record before using this tool. In many jurisdictions, all parties must be informed that a call is being recorded.

**Temporary files.** Raw PCM files in `tmp\` are not encrypted. On a shared machine, other users with filesystem access could read in-progress recordings. Use Windows folder permissions to restrict access to `tmp\` and `out\` if necessary.
