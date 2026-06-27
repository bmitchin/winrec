## In Progress

## Future Development

- [ ] **Meeting name sanitization cleanup** (due: end of Wednesday)
  - Collect edge cases from winrec_teams_log.txt over the next few days
  - Remove phone number artifacts (e.g. `+1`, country codes)
  - Remove or collapse dashes that appear in contact names
  - Switch timestamp↔title separator from `_` to a unique delimiter (e.g. `--`)
    so filenames are easier to parse: `YYMMDD_HHMM-YYMMDD_HHMM--MeetingName.wav`
  - Review all observed raw titles from logs before finalising rules

## Completed

- [x] Teams meeting-name detection — scrape Teams window title via EnumWindows on call start, sanitize, append to chunk filenames (`..._MeetingName.wav`)
- [x] Single-pass normalizer — dropped two-pass 0.95 peak normalization; resample-only, int16 written directly to WAV (faster, no `.tmp`)
- [x] Unified chunked pipeline — manual + Teams recordings both split every ~35 min; per-chunk normalize→upload
- [x] Tray icon states — added amber (normalizing/uploading) and red (error)
- [x] Removed transcript fetcher — `transcript_fetcher.cpp` deleted in v1.3 (gdrive→OneDrive .txt delivery no longer part of winrec)
- [x] Updated all docs to v1.3 (design doc, user manual, quickstart, transcription integration)
- [x] ~~Transcript fetcher — polls gdrive:teams-audio/ every 60 s, copies .txt files to OneDrive sync folder~~ (shipped v1.2, later removed in v1.3)
