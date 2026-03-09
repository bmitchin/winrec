// uploader.cpp — rclone.exe invocation via CreateProcess
#include "winrec.h"
#include <cstdio>

DWORD WINAPI UploaderThread(LPVOID param)
{
    auto* p = reinterpret_cast<UploadParams*>(param);
    UploadParams up = *p;
    delete p;

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    bool ok = false;

    // Check rclone exists
    DWORD attr = GetFileAttributesW(up.rclonePath.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        TrayBalloon(L"winrec – Error", L"rclone.exe not found next to winrec.exe");
        PostMessageW(up.hwnd, WM_APP_UPLOAD_DONE, 1, 0);
        CoUninitialize();
        return 1;
    }

    // Build command line:
    // "C:\path\rclone.exe" move "C:\path\out\file.wav" gdrive:teams-audio/
    wchar_t cmd[2048];
    _snwprintf(cmd, _countof(cmd),
               L"\"%ls\" move \"%ls\" %ls",
               up.rclonePath.c_str(),
               up.outPath.c_str(),
               up.remote.c_str());

    // Create a log file in the winrec directory to capture rclone stdout+stderr
    extern std::wstring g_exeDir;
    std::wstring logPath = g_exeDir + L"\\rclone_log.txt";

    // Extract remote name (everything before ':'), e.g. "gdrive:teams-audio/" -> "gdrive"
    std::wstring remoteName;
    {
        size_t colon = up.remote.find(L':');
        remoteName = (colon != std::wstring::npos) ? up.remote.substr(0, colon) : up.remote;
    }

    for (int attempt = 0; attempt < 2; ++attempt) {
        // (Re-)create log file, truncating any previous content
        SECURITY_ATTRIBUTES sa = {};
        sa.nLength        = sizeof(sa);
        sa.bInheritHandle = TRUE;

        HANDLE hLog = CreateFileW(logPath.c_str(),
                                  GENERIC_WRITE,
                                  FILE_SHARE_READ,
                                  &sa,
                                  CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL,
                                  nullptr);

        STARTUPINFOW        si = {};
        PROCESS_INFORMATION pi = {};
        si.cb          = sizeof(si);
        si.dwFlags     = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
        si.wShowWindow = SW_HIDE;
        si.hStdOutput  = (hLog != INVALID_HANDLE_VALUE) ? hLog : nullptr;
        si.hStdError   = (hLog != INVALID_HANDLE_VALUE) ? hLog : nullptr;

        if (!CreateProcessW(nullptr, cmd, nullptr, nullptr,
                            /*bInheritHandles=*/TRUE,
                            CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            if (hLog != INVALID_HANDLE_VALUE) CloseHandle(hLog);
            TrayBalloon(L"winrec – Error", L"Failed to launch rclone.exe");
            break;
        }

        WaitForSingleObject(pi.hProcess, INFINITE);

        DWORD exitCode = 0;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);

        // Close write handle so we can read the file
        if (hLog != INVALID_HANDLE_VALUE) CloseHandle(hLog);

        if (exitCode == 0) {
            ok = true;
            break;
        }

        // Read last ~300 chars of rclone_log.txt for error detail
        char   buf[301]     = {};
        wchar_t detail[320] = {};
        HANDLE hRead = CreateFileW(logPath.c_str(), GENERIC_READ,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   nullptr, OPEN_EXISTING, 0, nullptr);
        if (hRead != INVALID_HANDLE_VALUE) {
            DWORD fileSize   = GetFileSize(hRead, nullptr);
            DWORD readOffset = (fileSize > 300) ? fileSize - 300 : 0;
            SetFilePointer(hRead, (LONG)readOffset, nullptr, FILE_BEGIN);
            DWORD nRead = 0;
            ReadFile(hRead, buf, 300, &nRead, nullptr);
            CloseHandle(hRead);
            MultiByteToWideChar(CP_UTF8, 0, buf, (int)nRead, detail, 319);
        }

        // On first attempt, check for OAuth/token error and try to reconnect
        if (attempt == 0) {
            bool isAuthError = (strstr(buf, "empty token") != nullptr ||
                                strstr(buf, "oauth")       != nullptr ||
                                strstr(buf, "token")       != nullptr);

            if (isAuthError) {
                wchar_t note[256];
                _snwprintf(note, _countof(note),
                           L"Drive auth expired – browser opening to re-authorize %ls:",
                           remoteName.c_str());
                TrayBalloon(L"winrec – Auth", note);

                // Run: "<rclonePath>" config reconnect <remoteName>:
                // Capture reconnect output to a separate log for diagnosis.
                // rclone opens the default browser on its own regardless of console.
                std::wstring rcLogPath = g_exeDir + L"\\rclone_reconnect_log.txt";

                wchar_t rcCmd[1024];
                _snwprintf(rcCmd, _countof(rcCmd),
                           L"\"%ls\" config reconnect %ls: --log-level DEBUG --log-file \"%ls\"",
                           up.rclonePath.c_str(),
                           remoteName.c_str(),
                           rcLogPath.c_str());

                // Create a pipe to send "y\n" to rclone's stdin so it auto-answers
                // "Use web browser? y/n" and proceeds to open the browser.
                HANDLE hPipeRead = nullptr, hPipeWrite = nullptr;
                SECURITY_ATTRIBUTES pipeSa = {};
                pipeSa.nLength        = sizeof(pipeSa);
                pipeSa.bInheritHandle = TRUE;
                CreatePipe(&hPipeRead, &hPipeWrite, &pipeSa, 0);
                // Write the answer before launching so it's ready immediately
                DWORD written = 0;
                WriteFile(hPipeWrite, "y\n", 2, &written, nullptr);
                CloseHandle(hPipeWrite);

                // rclone owns rclone_reconnect_log.txt via --log-file; we must NOT
                // open it ourselves or rclone will fail with a sharing violation.
                STARTUPINFOW        rcSi = {};
                PROCESS_INFORMATION rcPi = {};
                rcSi.cb          = sizeof(rcSi);
                rcSi.dwFlags     = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
                rcSi.wShowWindow = SW_HIDE;
                rcSi.hStdInput   = hPipeRead;
                rcSi.hStdOutput  = nullptr;
                rcSi.hStdError   = nullptr;

                if (CreateProcessW(nullptr, rcCmd, nullptr, nullptr,
                                   TRUE, CREATE_NO_WINDOW,
                                   nullptr, nullptr, &rcSi, &rcPi)) {
                    if (hPipeRead != nullptr) CloseHandle(hPipeRead);

                    // Wait up to 5 minutes for the user to complete browser auth
                    WaitForSingleObject(rcPi.hProcess, 5 * 60 * 1000);

                    DWORD rcExit = STILL_ACTIVE;
                    GetExitCodeProcess(rcPi.hProcess, &rcExit);

                    if (rcExit == STILL_ACTIVE) {
                        TerminateProcess(rcPi.hProcess, 1);
                        CloseHandle(rcPi.hProcess);
                        CloseHandle(rcPi.hThread);
                        TrayBalloon(L"winrec – Error",
                                    L"Re-authorization timed out after 5 minutes");
                        break;
                    }

                    CloseHandle(rcPi.hProcess);
                    CloseHandle(rcPi.hThread);

                    if (rcExit != 0) {
                        // Read reconnect log to show why it failed
                        wchar_t rcDetail[320] = {};
                        HANDLE hRcRead = CreateFileW(rcLogPath.c_str(), GENERIC_READ,
                                                     FILE_SHARE_READ, nullptr,
                                                     OPEN_EXISTING, 0, nullptr);
                        if (hRcRead != INVALID_HANDLE_VALUE) {
                            DWORD rcSize   = GetFileSize(hRcRead, nullptr);
                            DWORD rcOffset = (rcSize > 300) ? rcSize - 300 : 0;
                            SetFilePointer(hRcRead, (LONG)rcOffset, nullptr, FILE_BEGIN);
                            char rcBuf[301] = {};
                            DWORD rcRead = 0;
                            ReadFile(hRcRead, rcBuf, 300, &rcRead, nullptr);
                            CloseHandle(hRcRead);
                            MultiByteToWideChar(CP_UTF8, 0, rcBuf, (int)rcRead, rcDetail, 319);
                        }
                        wchar_t rcMsg[512];
                        if (rcDetail[0])
                            _snwprintf(rcMsg, _countof(rcMsg),
                                       L"rclone reconnect failed (code %lu):\n%ls",
                                       rcExit, rcDetail);
                        else
                            _snwprintf(rcMsg, _countof(rcMsg),
                                       L"rclone reconnect failed (code %lu)", rcExit);
                        TrayBalloon(L"winrec – Error", rcMsg);
                        break;
                    }

                    DeleteFileW(rcLogPath.c_str());
                    TrayBalloon(L"winrec – Auth", L"Re-authorized. Retrying upload\u2026");
                    continue; // attempt 1
                }
                if (hPipeRead != nullptr) CloseHandle(hPipeRead);
                // If CreateProcess for reconnect failed, fall through to error reporting
            }
        }

        // Report the error (attempt 1, non-auth error on attempt 0, or reconnect launch failure)
        wchar_t msg[512];
        if (detail[0])
            _snwprintf(msg, _countof(msg), L"rclone error (code %lu):\n%ls", exitCode, detail);
        else
            _snwprintf(msg, _countof(msg), L"rclone exited with code %lu", exitCode);
        TrayBalloon(L"winrec – Error", msg);
        break;
    }

    if (ok) {
        DeleteFileW(logPath.c_str());
    }
    // On failure, leave rclone_log.txt on disk for diagnosis

    PostMessageW(up.hwnd, WM_APP_UPLOAD_DONE, ok ? 0 : 1, 0);
    CoUninitialize();
    return ok ? 0 : 1;
}
