// teams.cpp — Microsoft Teams Third-Party App API monitor
// Connects to Teams' local WebSocket, detects call start/end,
// and posts WM_APP_TEAMS_CALL_START / WM_APP_TEAMS_CALL_END to the main window.
#include "winrec.h"
#include <winhttp.h>
#include <cstdio>
#include <cstring>

static HANDLE            g_teamsThread  = nullptr;
static std::atomic<bool> g_teamsRunning { false };
static HWND              g_teamsHwnd    = nullptr;
static wchar_t           g_meetingName[128] = {};

// ---------------------------------------------------------------------------
// Debug log — appends timestamped entries to winrec_teams_log.txt
// ---------------------------------------------------------------------------

static void TeamsLog(const char* fmt, ...)
{
    std::wstring logPath = g_exeDir + L"\\winrec_teams_log.txt";
    FILE* f = _wfopen(logPath.c_str(), L"a");
    if (!f) return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "[%02d:%02d:%02d] ", st.wHour, st.wMinute, st.wSecond);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);

    fprintf(f, "\n");
    fclose(f);
}

// ---------------------------------------------------------------------------
// Persistent token — generated once, saved to winrec_teams_token.txt
// ---------------------------------------------------------------------------

static std::wstring GetOrCreateToken()
{
    std::wstring path = g_exeDir + L"\\winrec_teams_token.txt";

    FILE* f = _wfopen(path.c_str(), L"r");
    if (f) {
        wchar_t buf[128] = {};
        fgetws(buf, 127, f);
        fclose(f);
        size_t len = wcslen(buf);
        while (len > 0 && (buf[len-1] == L'\n' || buf[len-1] == L'\r' || buf[len-1] == L' '))
            buf[--len] = L'\0';
        if (len > 0) return std::wstring(buf);
    }

    GUID guid = {};
    CoCreateGuid(&guid);
    wchar_t token[64];
    _snwprintf(token, _countof(token),
               L"%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
               guid.Data1, guid.Data2, guid.Data3,
               guid.Data4[0], guid.Data4[1],
               guid.Data4[2], guid.Data4[3], guid.Data4[4],
               guid.Data4[5], guid.Data4[6], guid.Data4[7]);

    FILE* fw = _wfopen(path.c_str(), L"w");
    if (fw) { fwprintf(fw, L"%ls\n", token); fclose(fw); }

    return std::wstring(token);
}

// ---------------------------------------------------------------------------
// Meeting name — read from Teams window title via EnumWindows
// ---------------------------------------------------------------------------

// Returns true if the window belongs to the Teams process (ms-teams.exe or Teams.exe).
// Used as fallback for 1:1 calls where the window title is just the contact name.
static bool IsTeamsProcess(HWND hwnd, char* outExeName, int outLen)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) {
        if (outExeName) _snprintf(outExeName, outLen, "(no pid)");
        return false;
    }

    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) {
        if (outExeName) _snprintf(outExeName, outLen, "(OpenProcess failed, pid=%lu)", pid);
        return false;
    }

    wchar_t exePath[MAX_PATH] = {};
    DWORD   size              = MAX_PATH;
    BOOL    ok = QueryFullProcessImageNameW(hProc, 0, exePath, &size);
    CloseHandle(hProc);
    if (!ok) {
        if (outExeName) _snprintf(outExeName, outLen, "(QueryProcessName failed)");
        return false;
    }

    const wchar_t* fname = wcsrchr(exePath, L'\\');
    fname = fname ? fname + 1 : exePath;

    // Copy narrow version for logging
    if (outExeName) {
        int i = 0;
        while (fname[i] && i < outLen - 1) { outExeName[i] = (char)fname[i]; i++; }
        outExeName[i] = '\0';
    }

    return _wcsicmp(fname, L"ms-teams.exe") == 0 ||
           _wcsicmp(fname, L"Teams.exe")    == 0;
}

struct FindTeamsCtx {
    wchar_t result[128];
};

static BOOL CALLBACK FindTeamsWindowCb(HWND hwnd, LPARAM lParam)
{
    if (!IsWindowVisible(hwnd)) return TRUE;
    wchar_t title[512] = {};
    if (!GetWindowTextW(hwnd, title, _countof(title)) || !title[0]) return TRUE;

    auto* ctx = reinterpret_cast<FindTeamsCtx*>(lParam);

    // Log every visible window with a non-empty title
    TeamsLog("  [enum] hwnd=%p title=\"%ls\"", (void*)hwnd, title);

    // Format 1: "Meeting Name | [Org | email | ] Microsoft Teams"
    // The title can contain extra segments: take only the first one.
    const wchar_t* sep = wcsstr(title, L" | Microsoft Teams");
    if (sep && sep != title) {
        size_t len = (size_t)(sep - title);
        if (len > 60) len = 60;
        wcsncpy(ctx->result, title, len);
        ctx->result[len] = L'\0';
        // Strip any additional " | org | email" segments from what we extracted
        wchar_t* extraPipe = wcsstr(ctx->result, L" | ");
        if (extraPipe) *extraPipe = L'\0';
        TeamsLog("  [enum] -> matched Format1 (| Microsoft Teams), raw=\"%ls\"", ctx->result);
        return FALSE;
    }

    // Format 2: "Microsoft Teams – Meeting Name" or "Microsoft Teams - Meeting Name"
    const wchar_t* prefixes[] = { L"Microsoft Teams \u2013 ", L"Microsoft Teams - ", nullptr };
    for (int i = 0; prefixes[i]; ++i) {
        size_t plen = wcslen(prefixes[i]);
        if (wcsncmp(title, prefixes[i], plen) == 0) {
            wcsncpy(ctx->result, title + plen, 60);
            ctx->result[60] = L'\0';
            TeamsLog("  [enum] -> matched Format2 (prefix), raw=\"%ls\"", ctx->result);
            return FALSE;
        }
    }

    // Skip the generic home-screen title
    if (wcscmp(title, L"Microsoft Teams") == 0) {
        TeamsLog("  [enum] -> skipped (generic 'Microsoft Teams' title)");
        return TRUE;
    }

    // Fallback: check if it's a Teams process window
    char exeName[64] = {};
    bool isTeams = IsTeamsProcess(hwnd, exeName, sizeof(exeName));
    TeamsLog("  [enum] -> process=%s isTeams=%s", exeName, isTeams ? "YES" : "no");

    if (isTeams) {
        wcsncpy(ctx->result, title, 60);
        ctx->result[60] = L'\0';
        TeamsLog("  [enum] -> matched Fallback (Teams process), raw=\"%ls\"", ctx->result);
        return FALSE;
    }

    return TRUE;  // keep looking
}

static void CaptureMeetingName()
{
    TeamsLog("CaptureMeetingName: starting EnumWindows");

    FindTeamsCtx ctx = {};
    EnumWindows(FindTeamsWindowCb, reinterpret_cast<LPARAM>(&ctx));

    TeamsLog("CaptureMeetingName: EnumWindows done, raw=\"%ls\"", ctx.result);

    // Sanitize: limit length, replace illegal/space chars with _, collapse runs
    std::wstring s(ctx.result);
    if (s.size() > 60) s.resize(60);

    std::wstring result;
    bool prevUnderscore = false;
    for (wchar_t c : s) {
        wchar_t out = c;
        if (c == L' ' || c == L'<' || c == L'>' || c == L':' ||
            c == L'"' || c == L'/' || c == L'\\' || c == L'|' ||
            c == L'?' || c == L'*') {
            out = L'_';
        }
        if (out == L'_') {
            if (!prevUnderscore) result += out;
            prevUnderscore = true;
        } else {
            result += out;
            prevUnderscore = false;
        }
    }
    while (!result.empty() && result.front() == L'_') result.erase(result.begin());
    while (!result.empty() && result.back()  == L'_') result.pop_back();

    wcsncpy(g_meetingName, result.c_str(), 127);
    g_meetingName[127] = L'\0';
    TeamsLog("CaptureMeetingName: final=\"%ls\" (%d chars)", g_meetingName, (int)wcslen(g_meetingName));
}

// ---------------------------------------------------------------------------
// Simple JSON field search
// ---------------------------------------------------------------------------

static bool IsInMeeting(const char* msg)
{
    // Teams sends meetingPermissions; canLeave:true means we're in a call/meeting
    return strstr(msg, "\"canLeave\":true")  != nullptr ||
           strstr(msg, "\"canLeave\": true") != nullptr;
}

static bool IsNotInMeeting(const char* msg)
{
    return strstr(msg, "\"canLeave\":false")  != nullptr ||
           strstr(msg, "\"canLeave\": false") != nullptr;
}

// ---------------------------------------------------------------------------
// Monitor thread
// ---------------------------------------------------------------------------

static DWORD WINAPI TeamsMonitorThread(LPVOID)
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    std::wstring token = GetOrCreateToken();

    wchar_t path[512];
    _snwprintf(path, _countof(path),
               L"/?token=%ls&protocol-version=2.0.0"
               L"&manufacturer=WinRec&device=WinRec"
               L"&app=WinRec&app-version=1.0.0",
               token.c_str());

    TeamsLog("Teams monitor started. Connecting to localhost:8124");

    bool wasInMeeting = false;

    while (g_teamsRunning.load()) {

        HINTERNET hSession = WinHttpOpen(L"WinRec/1.0",
                                         WINHTTP_ACCESS_TYPE_NO_PROXY,
                                         nullptr, nullptr, 0);
        if (!hSession) {
            TeamsLog("WinHttpOpen failed (err=%lu) — retrying", GetLastError());
            Sleep(5000); continue;
        }

        DWORD timeout = 5000;
        WinHttpSetOption(hSession, WINHTTP_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));

        HINTERNET hConnect = WinHttpConnect(hSession, L"localhost", 8124, 0);
        if (!hConnect) {
            TeamsLog("WinHttpConnect failed (err=%lu) — retrying", GetLastError());
            WinHttpCloseHandle(hSession);
            Sleep(5000); continue;
        }

        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path,
                                                nullptr, nullptr, nullptr, 0);
        if (!hRequest) {
            TeamsLog("WinHttpOpenRequest failed (err=%lu) — retrying", GetLastError());
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            Sleep(5000); continue;
        }

        WinHttpSetOption(hRequest, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0);

        BOOL sendOk = WinHttpSendRequest(hRequest, nullptr, 0, nullptr, 0, 0, 0);
        if (!sendOk) {
            TeamsLog("WinHttpSendRequest failed (err=%lu) — retrying", GetLastError());
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            Sleep(5000); continue;
        }

        BOOL recvOk = WinHttpReceiveResponse(hRequest, nullptr);
        if (!recvOk) {
            TeamsLog("WinHttpReceiveResponse failed (err=%lu) — retrying", GetLastError());
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            Sleep(5000); continue;
        }

        // Log the HTTP status code
        DWORD statusCode = 0;
        DWORD statusSize = sizeof(statusCode);
        WinHttpQueryHeaders(hRequest,
                            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            nullptr, &statusCode, &statusSize, nullptr);
        TeamsLog("HTTP response status: %lu", statusCode);

        HINTERNET hWs = WinHttpWebSocketCompleteUpgrade(hRequest, 0);
        WinHttpCloseHandle(hRequest);

        if (!hWs) {
            TeamsLog("WebSocketCompleteUpgrade failed (err=%lu) — retrying", GetLastError());
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            Sleep(5000); continue;
        }

        TeamsLog("WebSocket connected successfully");

        // Connected — receive meeting state messages
        char buf[8192];
        while (g_teamsRunning.load()) {
            DWORD bytesRead = 0;
            WINHTTP_WEB_SOCKET_BUFFER_TYPE bufType;
            DWORD ret = WinHttpWebSocketReceive(hWs, buf, sizeof(buf) - 1,
                                                &bytesRead, &bufType);
            if (ret != ERROR_SUCCESS) {
                TeamsLog("Receive failed (err=%lu) — reconnecting", ret);
                break;
            }
            if (bufType == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
                TeamsLog("WebSocket closed by Teams — reconnecting");
                break;
            }

            buf[bytesRead] = '\0';

            if (IsInMeeting(buf) && !wasInMeeting) {
                wasInMeeting = true;
                CaptureMeetingName();
                TeamsLog(">>> isInMeeting=true — posting CALL_START");
                PostMessageW(g_teamsHwnd, WM_APP_TEAMS_CALL_START, 0, 0);
            } else if (IsNotInMeeting(buf) && wasInMeeting) {
                wasInMeeting = false;
                TeamsLog(">>> isInMeeting=false — posting CALL_END");
                PostMessageW(g_teamsHwnd, WM_APP_TEAMS_CALL_END, 0, 0);
            }
        }

        WinHttpWebSocketClose(hWs, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
        WinHttpCloseHandle(hWs);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);

        if (g_teamsRunning.load()) {
            TeamsLog("Disconnected — retrying in 5s");
            Sleep(5000);
        }
    }

    TeamsLog("Teams monitor stopped");
    CoUninitialize();
    return 0;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void TeamsMonitorStart(HWND hwnd)
{
    g_teamsHwnd = hwnd;
    g_teamsRunning.store(true);
    g_teamsThread = CreateThread(nullptr, 0, TeamsMonitorThread, nullptr, 0, nullptr);
}

const wchar_t* TeamsGetMeetingName()
{
    return g_meetingName;
}

void TeamsMonitorStop()
{
    g_teamsRunning.store(false);
    if (g_teamsThread) {
        if (WaitForSingleObject(g_teamsThread, 3000) == WAIT_TIMEOUT)
            TerminateThread(g_teamsThread, 0);
        CloseHandle(g_teamsThread);
        g_teamsThread = nullptr;
    }
}
