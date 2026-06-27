# winrec Quick Start Guide

Get recording in under five minutes.

---

## Requirements

- Windows 10 or 11, 64-bit
- A microphone (built-in, USB, or Bluetooth)
- System audio output that supports Stereo Mix (laptop speakers recommended; see Audio Notes below)
- `rclone.exe` configured with a `gdrive:` remote (for upload)
- Microsoft Teams (optional — enables auto-start/stop recording on calls)

---

## Step 1 — Copy the files

Place both files in the same folder, for example `C:\winrec\`:

```
C:\winrec\
    winrec.exe
    rclone.exe
```

`winrec.exe` creates `tmp\` and `out\` subfolders automatically on first run.

---

## Step 2 — Configure rclone (first time only)

Open a command prompt and run:

```
rclone config
```

Follow the prompts to add a remote named exactly **`gdrive`** pointing to your Google Drive account.

> **Important:** Use your own OAuth credentials (Client ID and Client Secret) from Google Cloud Console. See §4.1 of the User Manual for full instructions.

After setup, verify access:

```
rclone ls gdrive:
```

---

## Step 3 — Enable Stereo Mix (first time only)

winrec captures system audio via Stereo Mix. Enable it in Windows:

1. Run `mmsys.cpl`
2. Go to the **Recording** tab
3. Right-click in the device list → **Show Disabled Devices**
4. Right-click **Stereo Mix** → **Enable**

> **Audio note:** Stereo Mix captures audio from the device currently set as the Windows default output. For Teams calls, set your audio output to **laptop speakers** in Teams Settings → Devices. Bluetooth and most USB headsets do not expose a Stereo Mix device and their audio will not be captured.

---

## Step 4 — Enable Teams auto-detection (recommended)

winrec can automatically start and stop recording when you join or leave a Teams call.

1. Open Microsoft Teams → your profile picture → **Settings → Privacy**
2. Find **Third-party app API** and toggle it **on**
3. Start winrec — it will connect to Teams automatically within a few seconds
4. The first time winrec connects, Teams will show a pairing prompt — click **Allow**

After pairing, winrec appears in Teams' "Allowed apps and devices" list permanently.

---

## Step 5 — Start winrec

Double-click `winrec.exe`. A **sky blue bubble** icon appears in the system tray (bottom-right, near the clock). You may need to click the `^` overflow arrow to find it.

> **Tip:** To keep the icon always visible — Windows Settings → Personalization → Taskbar → Other system tray icons → enable **winrec**.

---

## Step 6 — Recording

### Automatic (Teams calls)

When you join a Teams meeting or call, winrec detects it and starts recording automatically. The icon turns **mint green**. When you leave the call, recording stops, the icon turns **amber** while the audio is resampled and uploaded, then returns to sky blue. The output filename includes the Teams meeting name.

### Manual

**Start:** Left-click the tray icon. The icon turns mint green.

**Stop:** Left-click again. winrec then automatically (icon amber during processing):
1. Resamples the audio to 16 kHz mono WAV
2. Uploads the file to `gdrive:teams-audio/` via rclone
3. Returns to Idle (sky blue)

Recordings longer than ~35 minutes are split into multiple files, each uploaded separately.

---

## Step 7 — Exit

Right-click the tray icon → **Exit**.

---

## File naming

```
260308_1430-260308_1512_Weekly_Sync.wav
│      │         │             └─ Teams meeting name (omitted for manual recordings)
│      │         └─ stop  time (YYMMDD_HHMM)
│      └─────────── start time
└────────────────── year 2026
```

---

## Troubleshooting

| Symptom | Fix |
|---|---|
| No tray icon | Check tray overflow (`^` arrow); pin via Taskbar settings |
| No audio from call participants | Enable Stereo Mix; use laptop speakers as Teams audio output |
| Teams auto-detection not working | Enable Third-party app API in Teams Privacy settings; approve pairing prompt |
| "rclone.exe not found" | Put `rclone.exe` in the same folder as `winrec.exe` |
| "rclone reconnect failed" | Check Client ID and Client Secret in `rclone.conf` match Google Cloud Console |
| "Upload failed" | Check internet; WAV is kept in `out\` for manual retry |
| Icon stuck in Uploading | Check internet connection; rclone is still running |
| WaveIn mode balloon on startup | WASAPI blocked by corporate software — Stereo Mix fallback is active |
