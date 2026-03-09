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
