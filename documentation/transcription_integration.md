# winrec → Transcription App Integration Guide

This document covers everything the transcription app needs to know about the audio files winrec produces and how to access them from Google Drive using the same rclone setup.

---

## 1. Audio File Specification

### Format

| Property | Value |
|---|---|
| Container | WAV (RIFF PCM) |
| Encoding | 16-bit signed integer PCM |
| Sample rate | 16 000 Hz |
| Channels | 1 (mono) |
| Bit depth | 16 bit |
| Byte order | Little-endian |
| Header | Standard 44-byte RIFF/WAV header |

This format is directly accepted by all major speech-to-text services without conversion:
- OpenAI Whisper (`whisper` CLI or API)
- Google Cloud Speech-to-Text
- Azure Cognitive Services Speech
- AWS Transcribe

### File size

Approximately **1.9 MB per minute** of recording.

A 1-hour Teams call produces a file of roughly 115 MB.

### Amplitude

**As of winrec v1.3, no peak normalization is applied.** Files are written at their captured level (samples are clamped to full scale but not rescaled). Levels may therefore vary between recordings depending on system and mic volume. This is fine for the major speech-to-text engines, which are robust to moderate level variation; if a downstream tool needs a consistent level, normalize on the transcription side. (Earlier winrec versions normalized to 0.95 peak — do not assume that any longer.)

### Content

The file contains a **mono mix** of:
- **System audio (loopback):** everything audible through the laptop speakers during the call — remote participants, shared video, hold music.
- **Microphone:** the local speaker's voice.

Both are mixed at equal weight (50/50). All participants are on a single track — there is no speaker separation or channel splitting.

### Filename format

```
YYMMDD_HHMM-YYMMDD_HHMM[_MeetingName].wav
│      │         │            └─ Teams meeting name (Teams calls only; omitted for manual recordings)
│      │         └─ stop  time (local, 24-hour, YYMMDD_HHMM)
│      └─────────── start time
└────────────────── two-digit year
```

Examples:
- `260308_1430-260308_1512.wav` — a manual recording, 8 March 2026, 14:30 to 15:12.
- `260308_1430-260308_1512_Weekly_Team_Sync.wav` — a Teams call tagged with the meeting name.

The leading `YYMMDD_HHMM-YYMMDD_HHMM` portion is always present and gives exact start and end times. An optional `_<MeetingName>` suffix follows for Teams-detected calls: it is the Teams window title, sanitized (illegal/space characters → `_`, collapsed, truncated to 60 chars), so it may contain underscores and is not guaranteed unique.

**Parsing tip:** match the timestamps with a prefix pattern such as `^(\d{6}_\d{4})-(\d{6}_\d{4})(?:_(.*))?\.wav$` rather than splitting on `_`, since the meeting name itself contains underscores.

**Chunking:** recordings longer than ~35 minutes are split by winrec into multiple files, each with its own start/end timestamps (and, for Teams calls, the same meeting name). A single long meeting may therefore arrive as several sequential WAVs.

### Google Drive location

winrec uploads all files to:

```
gdrive:teams-audio/
```

This maps to a folder named `teams-audio` at the root of the configured Google Drive account. Files are moved (not copied) — once uploaded, they no longer exist on the Windows machine.

---

## 2. Accessing Files with rclone

The transcription app should use `rclone` to list and download files from `gdrive:teams-audio/`. This reuses the same Google Drive authorization that winrec already established.

### 2.1 Shared rclone configuration

rclone stores its configuration — including the remote definition, Client ID, Client Secret, and OAuth refresh token — in a single file:

```
%APPDATA%\Roaming\rclone\rclone.conf
```

Full path example:
```
C:\Users\<username>\AppData\Roaming\rclone\rclone.conf
```

The transcription app does not need its own rclone configuration. rclone always reads `rclone.conf` from the user's AppData folder automatically. As long as the transcription app runs as the same Windows user as winrec, it will find the existing config with no extra setup — same remote, same token, same Drive account. No additional Google authorization is required.

### 2.2 Useful rclone commands

**List all WAV files in the upload folder:**
```
rclone ls gdrive:teams-audio/
```

**List with full metadata (size, date, name):**
```
rclone lsl gdrive:teams-audio/
```

**List filenames only (one per line), newest first:**
```
rclone lsf gdrive:teams-audio/ --format "tp" | sort -r
```

**Download a specific file:**
```
rclone copy gdrive:teams-audio/260308_1430-260308_1512.wav C:\transcriptions\
```

**Download all new files (skip already downloaded):**
```
rclone copy gdrive:teams-audio/ C:\transcriptions\
```

**Move a file (download and remove from Drive):**
```
rclone move gdrive:teams-audio/260308_1430-260308_1512.wav C:\transcriptions\
```

**Watch for new files (poll every 60 seconds):**
rclone does not have a native watch command. Poll with `rclone lsf` on a timer and compare to a known list.

### 2.3 Calling rclone from code

Use `subprocess` (Python) to invoke `rclone`. rclone finds `rclone.conf` automatically from its default location.

**Python example (Linux Mint):**
```python
import subprocess

RCLONE = "rclone"          # installed via apt or placed in the transcription folder
REMOTE = "gdrive:teams-audio/"

def list_files():
    result = subprocess.run(
        [RCLONE, "lsf", REMOTE, "--format", "ps"],
        capture_output=True, text=True, check=True
    )
    # Each line: "size;filename"
    return result.stdout.strip().splitlines()

def download(filename, dest_dir):
    subprocess.run(
        [RCLONE, "copy", REMOTE + filename, dest_dir],
        check=True
    )
```

### 2.4 Detecting new uploads

winrec uploads files as soon as recording stops. To detect them:

1. Call `rclone lsf gdrive:teams-audio/` on a polling interval (e.g. every 30–60 seconds).
2. Compare the returned list against a set of already-processed filenames.
3. Any filename not in the processed set is a new file ready for transcription.
4. After transcribing, either delete from Drive (`rclone deletefile`) or move to an archive folder (`rclone move`).

The filename's embedded timestamps let you record when the call happened without querying Drive metadata.

---

## 3. rclone Configuration Reference

### rclone.conf structure

The relevant section of `rclone.conf` for the `gdrive` remote looks like this:

```ini
[gdrive]
type = drive
client_id = <YOUR_CLIENT_ID>.apps.googleusercontent.com
client_secret = <YOUR_CLIENT_SECRET>
scope = drive
token = {"access_token":"...","token_type":"Bearer","refresh_token":"...","expiry":"..."}
```

| Field | Description |
|---|---|
| `type` | Always `drive` for Google Drive |
| `client_id` | OAuth 2.0 Client ID from Google Cloud Console |
| `client_secret` | OAuth 2.0 Client Secret from Google Cloud Console |
| `scope` | `drive` = full Drive access |
| `token` | JSON blob containing access token and refresh token; auto-updated by rclone |

The refresh token does not expire unless revoked. rclone refreshes the access token automatically when it expires (typically after 1 hour).

### If the token expires or is revoked

Run from the machine where the config lives:

```
rclone config reconnect gdrive:
```

A browser window opens for re-authorization. After completing it, `rclone.conf` is updated with a new token. No other changes are needed — winrec and the transcription app both immediately benefit from the refreshed token.

### Running on Linux Mint alongside the Windows machine

The transcription app runs on Linux Mint while winrec runs on Windows. Both can use the same Google Drive remote simultaneously — OAuth refresh tokens work across multiple machines at once. Each machine refreshes its own access token independently without affecting the other.

**Files to copy from Windows to the Mint machine:**

| Windows source | Linux destination | Notes |
|---|---|---|
| `%APPDATA%\Roaming\rclone\rclone.conf` | `~/.config/rclone/rclone.conf` | The remote definition and OAuth token |

That's it. No other files needed from the winrec folder.

**Steps:**

1. On the Windows machine, find `rclone.conf`:
   ```
   C:\Users\<username>\AppData\Roaming\rclone\rclone.conf
   ```

2. Copy it to the Mint machine:
   ```bash
   mkdir -p ~/.config/rclone
   # paste or scp the file to:
   ~/.config/rclone/rclone.conf
   ```

3. Install rclone on Mint — unlike Windows (where the portable `rclone.exe` had to be bundled
   with winrec because software installation isn't allowed), on Mint you can install it properly:
   ```bash
   sudo apt install rclone
   # or the latest version via the official installer:
   curl https://rclone.org/install.sh | sudo bash
   ```
   Once installed, `rclone` is available system-wide. The transcription app just calls `"rclone"` —
   no need to bundle or copy the binary into the project folder.

4. Test:
   ```bash
   rclone ls gdrive:teams-audio/
   ```

rclone on Linux reads `~/.config/rclone/rclone.conf` automatically — no `--config` flag needed.

**If the token is refreshed on Windows** (e.g. winrec triggers a reconnect), the Linux copy of `rclone.conf` will have a stale access token but a valid refresh token. rclone on Linux will silently refresh it on the next command. No manual action needed.

**If you ever need to re-authorize** (token revoked), run this on either machine:
```bash
rclone config reconnect gdrive:
```
Then copy the updated `rclone.conf` to the other machine, or re-run the reconnect there too.

---

## 4. Summary Checklist for the Transcription App

- [ ] WAV files are **16 kHz, mono, 16-bit PCM** — pass directly to STT API with no conversion.
- [ ] Levels are **not normalized** (v1.3+) — normalize on the transcription side if a tool needs it.
- [ ] Files appear in `gdrive:teams-audio/` after each recording; long calls arrive as several ~35-min chunks.
- [ ] Use `rclone lsf gdrive:teams-audio/` to list available files.
- [ ] Use `rclone copy` or `rclone move` to download files.
- [ ] Filename encodes start and end time, with an optional meeting name: `YYMMDD_HHMM-YYMMDD_HHMM[_MeetingName].wav`. Parse timestamps by prefix, not by splitting on `_`.
- [ ] No separate Google authorization needed — reuse the existing `rclone.conf`.
- [ ] After transcription, remove or archive the file from Drive to avoid reprocessing.
