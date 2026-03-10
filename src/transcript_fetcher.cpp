// transcript_fetcher.cpp — polls gdrive:teams-audio/ for .txt files every 60 s
// and copies them to the local OneDrive sync folder.
//
// NOTE: currently uses rclone copy (not move) so originals remain on Drive
// until the pipeline is verified end-to-end. Change to "move" when ready.
#include "winrec.h"
#include <string>

static HANDLE g_fetchStopEvent = nullptr;
static HANDLE g_fetchThread    = nullptr;

static const wchar_t* TRANSCRIPT_DEST =
    L"C:\\Users\\jmitchiner\\OneDrive - Pomeroy\\Brian@Home\\winrec\\transcripts";
static const wchar_t* GDRIVE_SOURCE    = L"gdrive:teams-audio/";
static const DWORD    FETCH_INTERVAL_MS = 60 * 1000;

// Returns true if gdrive:teams-audio/ contains at least one .txt file.
static bool HasTranscripts(const std::wstring& rclonePath)
{
    wchar_t cmd[1024];
    _snwprintf(cmd, _countof(cmd),
               L"\"%ls\" ls --include \"*.txt\" %ls",
               rclonePath.c_str(), GDRIVE_SOURCE);

    HANDLE hPipeRead = nullptr, hPipeWrite = nullptr;
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength        = sizeof(sa);
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&hPipeRead, &hPipeWrite, &sa, 0)) return false;
    SetHandleInformation(hPipeRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW        si = {};
    PROCESS_INFORMATION pi = {};
    si.cb          = sizeof(si);
    si.dwFlags     = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput  = hPipeWrite;
    si.hStdError   = nullptr;

    bool hasFiles = false;
    if (CreateProcessW(nullptr, cmd, nullptr, nullptr, TRUE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(hPipeWrite);
        hPipeWrite = nullptr;

        char buf[4096] = {};
        DWORD nRead = 0;
        ReadFile(hPipeRead, buf, sizeof(buf) - 1, &nRead, nullptr);
        WaitForSingleObject(pi.hProcess, 30 * 1000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);

        hasFiles = (nRead > 0 && buf[0] != '\0');
    }
    if (hPipeWrite) CloseHandle(hPipeWrite);
    CloseHandle(hPipeRead);
    return hasFiles;
}

// Copies all .txt files from gdrive:teams-audio/ to TRANSCRIPT_DEST.
// Returns true on rclone exit code 0.
static bool CopyTranscripts(const std::wstring& rclonePath)
{
    wchar_t cmd[1024];
    _snwprintf(cmd, _countof(cmd),
               L"\"%ls\" copy --include \"*.txt\" %ls \"%ls\"",
               rclonePath.c_str(), GDRIVE_SOURCE, TRANSCRIPT_DEST);

    STARTUPINFOW        si = {};
    PROCESS_INFORMATION pi = {};
    si.cb          = sizeof(si);
    si.dwFlags     = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    if (!CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return false;

    WaitForSingleObject(pi.hProcess, 5 * 60 * 1000);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return exitCode == 0;
}

static DWORD WINAPI TranscriptFetcherThread(LPVOID param)
{
    HWND hwnd = reinterpret_cast<HWND>(param);
    extern std::wstring g_exeDir;
    std::wstring rclonePath = g_exeDir + L"\\rclone.exe";

    // Ensure destination folder exists
    CreateDirectoryW(TRANSCRIPT_DEST, nullptr);

    while (true) {
        if (HasTranscripts(rclonePath)) {
            bool ok = CopyTranscripts(rclonePath);
            PostMessageW(hwnd, WM_APP_TRANSCRIPT_FETCHED, ok ? 1 : 0, 0);
        }

        // Wait 60 s before next check, or wake immediately on shutdown
        if (WaitForSingleObject(g_fetchStopEvent, FETCH_INTERVAL_MS) == WAIT_OBJECT_0)
            break;
    }
    return 0;
}

void TranscriptFetcherStart(HWND hwnd)
{
    g_fetchStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_fetchThread = CreateThread(nullptr, 0, TranscriptFetcherThread,
                                 reinterpret_cast<LPVOID>(hwnd), 0, nullptr);
}

void TranscriptFetcherStop()
{
    if (g_fetchStopEvent) SetEvent(g_fetchStopEvent);
    if (g_fetchThread) {
        WaitForSingleObject(g_fetchThread, 10 * 1000);
        CloseHandle(g_fetchThread);
        g_fetchThread = nullptr;
    }
    if (g_fetchStopEvent) {
        CloseHandle(g_fetchStopEvent);
        g_fetchStopEvent = nullptr;
    }
}
