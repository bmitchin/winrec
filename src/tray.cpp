#include "winrec.h"
#include <cstdio>

static NOTIFYICONDATAW g_nid         = {};
static bool            g_added       = false;
static HICON           g_icoIdle     = nullptr;  // sky blue  – not recording
static HICON           g_icoRecording= nullptr;  // mint green – recording

// ---------------------------------------------------------------------------
// Draw a filled circle "bubble" icon with a small gloss highlight.
// ---------------------------------------------------------------------------

static HICON MakeBubbleIcon(COLORREF color)
{
    const int sz = 32;

    HDC hdcScreen = GetDC(nullptr);
    HDC hdcColor  = CreateCompatibleDC(hdcScreen);
    HDC hdcMask   = CreateCompatibleDC(hdcScreen);

    HBITMAP hbmColor = CreateCompatibleBitmap(hdcScreen, sz, sz);
    HBITMAP hbmMask  = CreateBitmap(sz, sz, 1, 1, nullptr);

    HGDIOBJ hOldC = SelectObject(hdcColor, hbmColor);
    HGDIOBJ hOldM = SelectObject(hdcMask,  hbmMask);

    RECT rc = {0, 0, sz, sz};

    // --- Color bitmap ---
    // Black background (will be made transparent via the mask)
    FillRect(hdcColor, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));

    // Main circle
    const int m = 3;
    HBRUSH hBrush = CreateSolidBrush(color);
    SelectObject(hdcColor, hBrush);
    SelectObject(hdcColor, GetStockObject(NULL_PEN));
    Ellipse(hdcColor, m, m, sz - m, sz - m);
    DeleteObject(hBrush);

    // Gloss highlight: small white ellipse in the upper-left
    HBRUSH hGloss = CreateSolidBrush(RGB(255, 255, 255));
    SelectObject(hdcColor, hGloss);
    Ellipse(hdcColor, m + 4, m + 3, m + 13, m + 9);
    DeleteObject(hGloss);

    // --- Mask bitmap (1 = transparent, 0 = opaque) ---
    FillRect(hdcMask, &rc, (HBRUSH)GetStockObject(WHITE_BRUSH));
    SelectObject(hdcMask, GetStockObject(BLACK_BRUSH));
    SelectObject(hdcMask, GetStockObject(NULL_PEN));
    Ellipse(hdcMask, m, m, sz - m, sz - m);

    SelectObject(hdcColor, hOldC);
    SelectObject(hdcMask,  hOldM);

    ICONINFO ii  = {};
    ii.fIcon     = TRUE;
    ii.hbmColor  = hbmColor;
    ii.hbmMask   = hbmMask;
    HICON hIcon  = CreateIconIndirect(&ii);

    DeleteDC(hdcColor);
    DeleteDC(hdcMask);
    DeleteObject(hbmColor);
    DeleteObject(hbmMask);
    ReleaseDC(nullptr, hdcScreen);
    return hIcon;
}

// ---------------------------------------------------------------------------
// State helpers
// ---------------------------------------------------------------------------

static HICON GetStateIcon(RecorderState state)
{
    return (state == RecorderState::Recording) ? g_icoRecording : g_icoIdle;
}

static const wchar_t* GetStateTip(RecorderState state)
{
    switch (state) {
    case RecorderState::Idle:        return L"winrec – Idle (click to start)";
    case RecorderState::Recording:   return L"winrec – Recording…";
    case RecorderState::Normalizing: return L"winrec – Normalizing…";
    case RecorderState::Uploading:   return L"winrec – Uploading…";
    }
    return L"winrec";
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void TrayAdd(HWND hwnd)
{
    if (!g_icoIdle)
        g_icoIdle      = MakeBubbleIcon(RGB(0x87, 0xCE, 0xEB));  // sky blue
    if (!g_icoRecording)
        g_icoRecording = MakeBubbleIcon(RGB(0x3C, 0xD4, 0x82));  // mint green

    g_nid.cbSize           = sizeof(g_nid);
    g_nid.hWnd             = hwnd;
    g_nid.uID              = 1;
    g_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon            = GetStateIcon(RecorderState::Idle);
    wcsncpy(g_nid.szTip, GetStateTip(RecorderState::Idle), 127);

    Shell_NotifyIconW(NIM_ADD, &g_nid);
    g_added = true;
}

void TrayUpdate(RecorderState state)
{
    if (!g_added) return;
    g_nid.uFlags = NIF_ICON | NIF_TIP;
    g_nid.hIcon  = GetStateIcon(state);
    wcsncpy(g_nid.szTip, GetStateTip(state), 127);
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

void TrayRemove()
{
    if (!g_added) return;
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    g_added = false;
}

// No pop-up notifications — append timestamped entry to winrec_log.txt instead.
void TrayBalloon(const wchar_t* title, const wchar_t* msg)
{
    std::wstring logPath = g_exeDir + L"\\winrec_log.txt";
    FILE* f = _wfopen(logPath.c_str(), L"a, ccs=UTF-8");
    if (!f) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fwprintf(f, L"[%04d-%02d-%02d %02d:%02d:%02d] %ls: %ls\n",
             st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond,
             title, msg);
    fclose(f);
}
