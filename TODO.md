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

- [x] Transcript fetcher — polls gdrive:teams-audio/ every 60 s, copies .txt files to OneDrive sync folder; balloon notification on success
