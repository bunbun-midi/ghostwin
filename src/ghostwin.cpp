// GhostWin — turn any window into a "ghost": see-through, click-through and
// floating above every other window. A small button on the window's title bar
// turns it into a ghost; the same button (which stays clickable) brings it back.
//
// GhostWin is a normal tray app. It only uses public window APIs on other
// apps' windows (extended styles, layered-window opacity, z-order) and
// out-of-context accessibility events to follow them. Nothing is injected
// into other processes.

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

#include "glyph.h"

#ifndef EVENT_OBJECT_CLOAKED
#define EVENT_OBJECT_CLOAKED 0x8017
#define EVENT_OBJECT_UNCLOAKED 0x8018
#endif

namespace {

// --------------------------------------------------------------- constants
constexpr wchar_t kApp[] = L"GhostWin";
constexpr wchar_t kVersion[] = L"0.1.1";
constexpr wchar_t kMainClass[] = L"GhostWin.Main";
constexpr wchar_t kButtonClass[] = L"GhostWin.Button";
constexpr wchar_t kMutexName[] = L"Local\\GhostWin.SingleInstance";
constexpr wchar_t kRegKey[] = L"Software\\GhostWin";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT WM_GW_CLICK = WM_APP + 2;  // wParam = target, lParam = 1 if it's a ghost
constexpr UINT WM_GW_MENU = WM_APP + 3;   // wParam = target, lParam = 1 if it's a ghost
constexpr UINT_PTR kSyncTimer = 1;
constexpr UINT kSyncMs = 500;
constexpr int kHotkeyGhost = 1, kHotkeyBringBack = 2;
constexpr ULONG_PTR kCopyDataCommand = 0x47485354;  // 'GHST'
constexpr int kMinOpacity = 5, kMaxOpacity = 95;

enum Placement { kPlaceRight = 0, kPlaceCenter = 1, kPlaceOff = 2 };

enum MenuId : UINT {
    ID_RESTORE_ALL = 100,
    ID_BRING_BACK,
    ID_PLACE_RIGHT,
    ID_PLACE_CENTER,
    ID_PLACE_OFF,
    ID_STARTUP,
    ID_HOTKEYS,
    ID_HELP,
    ID_EXIT,
    ID_GHOST_THIS,
    ID_RESTORE_THIS,
    ID_GHOST_LIST = 1000,   // + index into g_ghosts
    ID_DEFAULT_OPACITY = 2000,  // + percent
    ID_THIS_OPACITY = 3000,     // + percent
};

// ----------------------------------------------------------------- state
struct Settings {
    int opacity = 40;          // percent, for new ghosts
    int placement = kPlaceRight;
    bool hotkeys = true;
};

struct Button {
    HWND hwnd = nullptr;
    HWND target = nullptr;
    bool ghost = false;
    bool hover = false;
    bool down = false;
    int w = 0, h = 0;
};

struct Ghost {
    HWND hwnd = nullptr;
    DWORD pid = 0;
    LONG_PTR exStyle = 0;    // extended style before we touched it
    bool wasLayered = false;
    bool hadAttrs = false;   // GetLayeredWindowAttributes worked (not per-pixel alpha)
    COLORREF key = 0;
    BYTE alpha = 255;
    DWORD flags = 0;
    bool wasTopmost = false;
    bool alphaControl = true;  // false for per-pixel-alpha windows: click-through + on top only
    int opacity = 40;
    Button* button = nullptr;
};

HINSTANCE g_inst;
HWND g_main;
HWND g_tip;
Settings g_set;
std::vector<Ghost> g_ghosts;
Button* g_active;          // the button on the active (normal) window
HWND g_dragging;           // window being moved/resized by the user
bool g_elevated;
UINT g_taskbarCreated;
NOTIFYICONDATAW g_nid;
HICON g_iconSmall, g_iconBig;
std::vector<HWINEVENTHOOK> g_hooks;
bool g_hotkeysOk[3];

// ------------------------------------------------------------- utilities
DWORD RegGet(const wchar_t* name, DWORD def) {
    DWORD v = 0, n = sizeof v;
    return RegGetValueW(HKEY_CURRENT_USER, kRegKey, name, RRF_RT_REG_DWORD, nullptr, &v, &n) == ERROR_SUCCESS ? v : def;
}
void RegPut(const wchar_t* name, DWORD v) {
    RegSetKeyValueW(HKEY_CURRENT_USER, kRegKey, name, REG_DWORD, &v, sizeof v);
}
void LoadSettings() {
    g_set.opacity = std::clamp((int)RegGet(L"Opacity", 40), kMinOpacity, kMaxOpacity);
    g_set.placement = std::clamp((int)RegGet(L"ButtonPlacement", kPlaceRight), 0, 2);
    g_set.hotkeys = RegGet(L"Hotkeys", 1) != 0;
}

BYTE AlphaOf(int percent) { return (BYTE)std::clamp(percent * 255 / 100, 1, 255); }

std::wstring StateFolder() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(L"GHOSTWIN_STATE_DIR", buf, ARRAYSIZE(buf));
    if (n && n < ARRAYSIZE(buf)) return buf;
    n = ExpandEnvironmentStringsW(L"%LOCALAPPDATA%\\GhostWin", buf, ARRAYSIZE(buf));
    return (n && n <= ARRAYSIZE(buf)) ? std::wstring(buf) : std::wstring(L".");
}
std::wstring StateFile() { return StateFolder() + L"\\ghosts.txt"; }

std::wstring ExePath() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, ARRAYSIZE(buf));
    return std::wstring(buf, n);
}

UINT DpiOf(HWND h) {
    using GetDpiForMonitor_t = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    static auto getDpiForMonitor = [] {
        HMODULE m = LoadLibraryW(L"shcore.dll");
        return m ? (GetDpiForMonitor_t)(void*)GetProcAddress(m, "GetDpiForMonitor") : nullptr;
    }();
    if (getDpiForMonitor) {
        UINT x = 96, y = 96;
        if (SUCCEEDED(getDpiForMonitor(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), 0, &x, &y)) && x) return x;
    }
    HDC dc = GetDC(nullptr);
    int d = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(nullptr, dc);
    return d > 0 ? (UINT)d : 96;
}

bool IsOurs(HWND h) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    return pid == GetCurrentProcessId();
}

bool IsCloaked(HWND h) {
    DWORD c = 0;
    return SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &c, sizeof c)) && c;
}

std::wstring ClassOf(HWND h) {
    wchar_t c[128] = L"";
    GetClassNameW(h, c, ARRAYSIZE(c));
    return c;
}

std::wstring TitleOf(HWND h) {
    wchar_t t[256] = L"";
    // Bounded wait: a hung app must not hang GhostWin.
    DWORD_PTR n = 0;
    if (!SendMessageTimeoutW(h, WM_GETTEXT, ARRAYSIZE(t), (LPARAM)t, SMTO_ABORTIFHUNG | SMTO_BLOCK, 200, &n)) t[0] = 0;
    std::wstring s = t;
    if (s.empty()) s = L"(" + ClassOf(h) + L")";
    if (s.size() > 60) s = s.substr(0, 57) + L"...";
    return s;
}

// Desktop, taskbar, Start, task view and similar shell surfaces.
bool IsShellSurface(HWND h) {
    static const wchar_t* kShell[] = {
        L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", L"NotifyIconOverflowWindow",
        L"Windows.UI.Core.CoreWindow", L"XamlExplorerHostIslandWindow", L"TopLevelWindowForOverflowXamlIsland",
        L"ForegroundStaging", L"MultitaskingViewFrame", L"TaskListThumbnailWnd", L"Shell_InputSwitchTopLevelWindow",
        L"#32768" /* menus */, L"tooltips_class32",
    };
    std::wstring c = ClassOf(h);
    for (auto* s : kShell)
        if (c == s) return true;
    return h == GetShellWindow() || h == GetDesktopWindow();
}

bool ProcessIsElevated(DWORD pid, bool* unknownIsElevated) {
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return *unknownIsElevated;
    HANDLE tok = nullptr;
    bool elevated = *unknownIsElevated;
    if (OpenProcessToken(p, TOKEN_QUERY, &tok)) {
        TOKEN_ELEVATION e{};
        DWORD n = 0;
        if (GetTokenInformation(tok, TokenElevation, &e, sizeof e, &n)) elevated = e.TokenIsElevated != 0;
        CloseHandle(tok);
    }
    CloseHandle(p);
    return elevated;
}

// Windows refuses style changes to windows of apps running as administrator
// unless GhostWin runs as administrator too.
bool NeedsAdmin(HWND h) {
    if (g_elevated) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    bool assume = true;  // token access denied: it's almost always an elevated process
    return ProcessIsElevated(pid, &assume);
}

Ghost* FindGhost(HWND h) {
    for (auto& g : g_ghosts)
        if (g.hwnd == h) return &g;
    return nullptr;
}

// A window GhostWin may turn into a ghost. `why` explains a refusal.
bool CanGhost(HWND h, std::wstring* why) {
    auto no = [&](const wchar_t* w) {
        if (why) *why = w;
        return false;
    };
    if (!h || !IsWindow(h)) return no(L"That window no longer exists.");
    if (GetAncestor(h, GA_ROOT) != h) return no(L"Only top-level windows can be made see-through.");
    if (IsOurs(h)) return no(L"GhostWin's own windows can't be made see-through.");
    if (IsShellSurface(h)) return no(L"The desktop, taskbar and Start can't be made see-through.");
    if (!IsWindowVisible(h)) return no(L"That window isn't visible.");
    if (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return no(L"Tool windows and popups can't be made see-through.");
    if (IsHungAppWindow(h)) return no(L"That window isn't responding.");
    if (NeedsAdmin(h)) return no(L"That app runs as administrator. Run GhostWin as administrator to make its windows see-through.");
    return true;
}

// Does the active window get the "make see-through" button?
bool WantsButton(HWND h) {
    if (!CanGhost(h, nullptr) || IsIconic(h) || IsCloaked(h)) return false;
    LONG_PTR st = GetWindowLongPtrW(h, GWL_STYLE);
    return (st & WS_CAPTION) == WS_CAPTION || (st & WS_THICKFRAME);
}

HICON MakeIcon(int size) {
    auto cov = GhostCoverage(size, size, IconGlyphSize(size));
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), size, -size, 1, 32, BI_RGB};
    void* bits = nullptr;
    HBITMAP color = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!color) return nullptr;
    auto* p = static_cast<DWORD*>(bits);
    for (int i = 0; i < size * size; ++i) {
        DWORD a = cov[i];
        // lavender ghost; the eyes are holes, so the taskbar shows through them
        p[i] = (a << 24) | (kIconR << 16) | (kIconG << 8) | kIconB;
    }
    std::vector<BYTE> zeros((size_t)((size + 15) / 16) * 2 * size, 0);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, zeros.data());
    ICONINFO ii{TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return icon;
}

// ------------------------------------------------------------- tray / UI
void Notify(const std::wstring& text, DWORD icon = NIIF_INFO) {
    NOTIFYICONDATAW n = g_nid;
    n.uFlags = NIF_INFO;
    lstrcpynW(n.szInfo, text.c_str(), ARRAYSIZE(n.szInfo));
    lstrcpynW(n.szInfoTitle, kApp, ARRAYSIZE(n.szInfoTitle));
    n.dwInfoFlags = icon | NIIF_NOSOUND;
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

void UpdateTrayTip() {
    g_nid.uFlags = NIF_TIP | NIF_SHOWTIP;
    std::wstring tip = std::wstring(kApp) + L": ";
    tip += g_ghosts.empty() ? L"no see-through windows" : std::to_wstring(g_ghosts.size()) + L" see-through";
    lstrcpynW(g_nid.szTip, tip.c_str(), ARRAYSIZE(g_nid.szTip));
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

void AddTrayIcon() {
    g_nid = {};
    g_nid.cbSize = sizeof g_nid;
    g_nid.hWnd = g_main;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = g_iconSmall;
    lstrcpynW(g_nid.szTip, kApp, ARRAYSIZE(g_nid.szTip));
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    g_nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
    UpdateTrayTip();
}

// ---------------------------------------------------------------- buttons
// Where the button goes, in screen pixels: just left of the window's
// minimize/maximize/close buttons (or centred), vertically centred in the
// title bar.
bool ButtonRect(HWND t, int placement, RECT* out) {
    RECT wr;
    if (!GetWindowRect(t, &wr) || IsRectEmpty(&wr)) return false;
    RECT fr = wr;  // visible frame (excludes the invisible resize borders)
    if (FAILED(DwmGetWindowAttribute(t, DWMWA_EXTENDED_FRAME_BOUNDS, &fr, sizeof fr)) || IsRectEmpty(&fr)) fr = wr;
    const UINT dpi = DpiOf(t);
    auto S = [&](int v) { return MulDiv(v, (int)dpi, 96); };

    int capTop = fr.top, capBottom = fr.top + S(31), buttonsLeft = 0;
    RECT cb{};
    bool haveCb = SUCCEEDED(DwmGetWindowAttribute(t, DWMWA_CAPTION_BUTTON_BOUNDS, &cb, sizeof cb)) &&
                  cb.right > cb.left && cb.bottom > cb.top;
    if (haveCb) {  // window-relative -> screen; reject nonsense from custom-drawn frames
        OffsetRect(&cb, wr.left, wr.top);
        haveCb = cb.left >= fr.left && cb.right <= fr.right + S(16) && cb.top >= wr.top - S(4) &&
                 cb.bottom <= fr.top + S(90) && cb.bottom - cb.top >= S(14);
    }
    if (haveCb) {
        capTop = std::max(cb.top, fr.top);
        capBottom = cb.bottom;
        buttonsLeft = cb.left;
    } else {
        LONG_PTR st = GetWindowLongPtrW(t, GWL_STYLE);
        int n = (st & WS_SYSMENU) ? 1 + ((st & (WS_MINIMIZEBOX | WS_MAXIMIZEBOX)) ? 2 : 0) : 0;
        TITLEBARINFO tbi{sizeof tbi};
        if (GetTitleBarInfo(t, &tbi) && !(tbi.rgstate[0] & (STATE_SYSTEM_INVISIBLE | STATE_SYSTEM_OFFSCREEN)) &&
            tbi.rcTitleBar.bottom - tbi.rcTitleBar.top >= S(14)) {
            capTop = std::max(tbi.rcTitleBar.top, fr.top);
            capBottom = tbi.rcTitleBar.bottom;
        }
        buttonsLeft = fr.right - n * S(46);
    }
    const int capH = std::max(capBottom - capTop, S(18));
    const int bw = S(30);
    const int bh = std::clamp(capH - S(6), S(16), S(24));
    const int y = capTop + (capH - bh) / 2;
    int x = placement == kPlaceCenter ? (fr.left + fr.right - bw) / 2 : buttonsLeft - S(6) - bw;
    if (x < fr.left + S(8) || fr.right - fr.left < S(160)) return false;  // window too narrow
    *out = {x, y, x + bw, y + bh};
    return true;
}

void SetButtonTip(Button* b) {
    if (!g_tip) return;
    TOOLINFOW ti{sizeof ti};
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = g_main;
    ti.uId = (UINT_PTR)b->hwnd;
    ti.lpszText = LPSTR_TEXTCALLBACKW;
    SendMessageW(g_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
}

Button* CreateButton(HWND target, bool ghost) {
    auto* b = new Button;
    b->target = target;
    b->ghost = ghost;
    b->hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, kButtonClass, L"", WS_POPUP, 0, 0,
                              1, 1, nullptr, nullptr, g_inst, b);
    if (!b->hwnd) {
        delete b;
        return nullptr;
    }
    SetButtonTip(b);
    return b;
}

void DestroyButton(Button* b) {
    if (!b) return;
    if (g_tip) {
        TOOLINFOW ti{sizeof ti};
        ti.hwnd = g_main;
        ti.uId = (UINT_PTR)b->hwnd;
        SendMessageW(g_tip, TTM_DELTOOLW, 0, (LPARAM)&ti);
    }
    DestroyWindow(b->hwnd);  // WM_NCDESTROY deletes b
}

void HideButton(Button* b) {
    if (b && IsWindowVisible(b->hwnd)) ShowWindow(b->hwnd, SW_HIDE);
}

void PlaceButton(Button* b) {
    if (!b) return;
    HWND t = b->target;
    int placement = g_set.placement;
    if (b->ghost && placement == kPlaceOff) placement = kPlaceRight;  // a ghost always keeps its way back
    RECT r;
    bool show = IsWindow(t) && IsWindowVisible(t) && !IsIconic(t) && !IsCloaked(t) && t != g_dragging &&
                (b->ghost || placement != kPlaceOff) && ButtonRect(t, placement, &r);
    if (!show) return HideButton(b);
    int w = r.right - r.left, h = r.bottom - r.top;
    if (w != b->w || h != b->h) {
        b->w = w;
        b->h = h;
        int c = std::max(4, h / 3);
        SetWindowRgn(b->hwnd, CreateRoundRectRgn(0, 0, w + 1, h + 1, c, c), FALSE);
        InvalidateRect(b->hwnd, nullptr, FALSE);
    }
    SetWindowPos(b->hwnd, HWND_TOPMOST, r.left, r.top, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
}

void PaintButton(Button* b, HDC hdc) {
    RECT rc;
    GetClientRect(b->hwnd, &rc);
    const int w = rc.right, h = rc.bottom;
    if (w <= 0 || h <= 0) return;
    COLORREF bg, fg;
    if (b->ghost) {
        bg = b->down ? RGB(88, 70, 188) : b->hover ? RGB(132, 114, 242) : RGB(108, 88, 220);
        fg = RGB(255, 255, 255);
    } else {
        bg = b->down ? RGB(34, 34, 38) : b->hover ? RGB(74, 74, 82) : RGB(48, 48, 54);
        fg = b->hover ? RGB(255, 255, 255) : RGB(214, 214, 222);
    }
    auto cov = GhostCoverage(w, h, h * 3 / 4);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
    std::vector<DWORD> px((size_t)w * h);
    for (size_t i = 0; i < px.size(); ++i) {
        int a = cov[i];
        auto mix = [&](int f, int g) { return (f * a + g * (255 - a)) / 255; };
        int r = mix(GetRValue(fg), GetRValue(bg)), g = mix(GetGValue(fg), GetGValue(bg)),
            bl = mix(GetBValue(fg), GetBValue(bg));
        px[i] = (DWORD)((r << 16) | (g << 8) | bl);
    }
    SetDIBitsToDevice(hdc, 0, 0, w, h, 0, 0, 0, h, px.data(), &bi, DIB_RGB_COLORS);
}

bool SetGhostOpacity(HWND h, int pct);

Button* ButtonFromHwnd(HWND h) {
    if (g_active && g_active->hwnd == h) return g_active;
    for (auto& g : g_ghosts)
        if (g.button && g.button->hwnd == h) return g.button;
    return nullptr;
}

LRESULT CALLBACK ButtonProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* b = reinterpret_cast<Button*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE:
            b = static_cast<Button*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            b->hwnd = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)b);
            break;
        case WM_NCDESTROY:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            delete b;
            return DefWindowProcW(hwnd, msg, wp, lp);
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;  // never take focus from the window it sits on
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            if (b) PaintButton(b, dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_MOUSEMOVE:
            if (b && !b->hover) {
                b->hover = true;
                TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&t);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_MOUSELEAVE:
            if (b) {
                b->hover = false;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONDOWN:
            if (b) {
                b->down = true;
                SetCapture(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONUP:
            if (b && b->down) {
                b->down = false;
                if (GetCapture() == hwnd) ReleaseCapture();
                InvalidateRect(hwnd, nullptr, FALSE);
                RECT rc;
                GetClientRect(hwnd, &rc);
                POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                // Handled by the main window: the click may destroy this button.
                if (PtInRect(&rc, pt)) PostMessageW(g_main, WM_GW_CLICK, (WPARAM)b->target, b->ghost);
            }
            return 0;
        case WM_CAPTURECHANGED:
            if (b && b->down) {
                b->down = false;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_RBUTTONUP:
            if (b) PostMessageW(g_main, WM_GW_MENU, (WPARAM)b->target, b->ghost);
            return 0;
        case WM_MOUSEWHEEL:
            if (b && b->ghost)
                if (Ghost* g = FindGhost(b->target)) {
                    int delta = GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 5 : -5;
                    if (SetGhostOpacity(g->hwnd, g->opacity + delta)) SendMessageW(g_tip, TTM_UPDATE, 0, 0);
                }
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ------------------------------------------------------------- ghosting
void SaveState() {
    std::wstring path = StateFile();
    if (g_ghosts.empty()) {
        DeleteFileW(path.c_str());
        return;
    }
    CreateDirectoryW(StateFolder().c_str(), nullptr);
    std::wstring tmp = path + L".tmp";
    FILE* f = _wfopen(tmp.c_str(), L"w");
    if (!f) return;
    fwprintf(f, L"GhostWin-state 1\n");
    for (auto& g : g_ghosts)
        fwprintf(f, L"%llx %lu %llx %d %d %lx %u %lx %d %d\n", (unsigned long long)(ULONG_PTR)g.hwnd, g.pid,
                 (unsigned long long)g.exStyle, g.wasLayered, g.hadAttrs, (unsigned long)g.key, (unsigned)g.alpha,
                 g.flags, g.wasTopmost, g.opacity);
    fclose(f);
    MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

// Put a ghost's window back the way it was. Only the bits GhostWin changed
// are reverted, so style changes the app made meanwhile survive.
void RevertWindow(const Ghost& g) {
    HWND h = g.hwnd;
    if (!IsWindow(h)) return;
    LONG_PTR cur = GetWindowLongPtrW(h, GWL_EXSTYLE);
    LONG_PTR ours = WS_EX_LAYERED | WS_EX_TRANSPARENT;
    LONG_PTR next = (cur & ~ours) | (g.exStyle & ours);
    if (g.wasLayered && g.hadAttrs) SetLayeredWindowAttributes(h, g.key, g.alpha, g.flags);
    if (next != cur) SetWindowLongPtrW(h, GWL_EXSTYLE, next);
    if (g.wasLayered && g.hadAttrs) SetLayeredWindowAttributes(h, g.key, g.alpha, g.flags);
    SetWindowPos(h, g.wasTopmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS);
    if (!g.wasLayered) RedrawWindow(h, nullptr, nullptr, RDW_ERASE | RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
}

void UpdateActive();

// After a window becomes a ghost it shouldn't keep the keyboard: give focus
// to the next real window below it.
void ActivateNextBelow(HWND ghost) {
    for (HWND w = GetTopWindow(nullptr); w; w = GetWindow(w, GW_HWNDNEXT)) {
        if (w == ghost || FindGhost(w) || IsOurs(w) || !IsWindowVisible(w) || IsIconic(w) || IsCloaked(w)) continue;
        LONG_PTR ex = GetWindowLongPtrW(w, GWL_EXSTYLE);
        if ((ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT)) || IsShellSurface(w)) continue;
        RECT r;
        if (!GetWindowRect(w, &r) || IsRectEmpty(&r)) continue;
        SetForegroundWindow(w);
        return;
    }
    if (HWND shell = GetShellWindow()) SetForegroundWindow(shell);
}

bool MakeGhost(HWND h, int opacity, bool tell) {
    if (FindGhost(h)) return true;
    std::wstring why;
    if (!CanGhost(h, &why)) {
        if (tell) Notify(why, NIIF_WARNING);
        return false;
    }
    Ghost g;
    g.hwnd = h;
    GetWindowThreadProcessId(h, &g.pid);
    g.exStyle = GetWindowLongPtrW(h, GWL_EXSTYLE);
    g.wasTopmost = (g.exStyle & WS_EX_TOPMOST) != 0;
    g.wasLayered = (g.exStyle & WS_EX_LAYERED) != 0;
    if (g.wasLayered) {
        g.hadAttrs = GetLayeredWindowAttributes(h, &g.key, &g.alpha, &g.flags) != 0;
        // A window that draws its own per-pixel alpha (UpdateLayeredWindow)
        // would break if we switched it to constant alpha.
        g.alphaControl = g.hadAttrs;
    }
    g.opacity = std::clamp(opacity, kMinOpacity, kMaxOpacity);

    SetLastError(0);
    SetWindowLongPtrW(h, GWL_EXSTYLE, g.exStyle | WS_EX_LAYERED | WS_EX_TRANSPARENT);
    LONG_PTR now = GetWindowLongPtrW(h, GWL_EXSTYLE);
    if ((now & (WS_EX_LAYERED | WS_EX_TRANSPARENT)) != (WS_EX_LAYERED | WS_EX_TRANSPARENT)) {
        RevertWindow(g);
        if (tell) Notify(L"Windows didn't allow changing that window (it may belong to an app running as administrator).",
                         NIIF_WARNING);
        return false;
    }
    if (g.alphaControl)
        SetLayeredWindowAttributes(h, g.hadAttrs ? g.key : 0, AlphaOf(g.opacity),
                                   LWA_ALPHA | (g.hadAttrs ? (g.flags & LWA_COLORKEY) : 0));
    SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    g.button = CreateButton(h, true);
    g_ghosts.push_back(g);
    SaveState();
    UpdateTrayTip();
    if (GetForegroundWindow() == h) ActivateNextBelow(h);
    PlaceButton(g_ghosts.back().button);
    UpdateActive();
    if (tell && !g.alphaControl)
        Notify(L"This window draws its own transparency, so only click-through and on-top were applied.");
    return true;
}

bool RestoreGhost(HWND h, bool activate) {
    for (size_t i = 0; i < g_ghosts.size(); ++i) {
        if (g_ghosts[i].hwnd != h) continue;
        Ghost g = g_ghosts[i];
        g_ghosts.erase(g_ghosts.begin() + i);
        DestroyButton(g.button);
        RevertWindow(g);
        if (activate && IsWindow(h)) {
            SetWindowPos(h, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_ASYNCWINDOWPOS);
            SetForegroundWindow(h);
        }
        SaveState();
        UpdateTrayTip();
        UpdateActive();
        return true;
    }
    return false;
}

// The window was closed while it was a ghost.
void ForgetGhost(HWND h) {
    for (size_t i = 0; i < g_ghosts.size(); ++i)
        if (g_ghosts[i].hwnd == h) {
            DestroyButton(g_ghosts[i].button);
            g_ghosts.erase(g_ghosts.begin() + i);
            SaveState();
            UpdateTrayTip();
            return;
        }
}

void RestoreAll() {
    while (!g_ghosts.empty()) RestoreGhost(g_ghosts.back().hwnd, false);
}

bool BringBackLatest() {
    return !g_ghosts.empty() && RestoreGhost(g_ghosts.back().hwnd, true);
}

bool SetGhostOpacity(HWND h, int pct) {
    Ghost* g = FindGhost(h);
    if (!g || !g->alphaControl) return false;
    g->opacity = std::clamp(pct, kMinOpacity, kMaxOpacity);
    SetLayeredWindowAttributes(h, g->hadAttrs ? g->key : 0, AlphaOf(g->opacity),
                               LWA_ALPHA | (g->hadAttrs ? (g->flags & LWA_COLORKEY) : 0));
    SaveState();
    return true;
}

// Keep ghosts above everything, including other always-on-top windows.
void RaiseGhosts() {
    for (auto& g : g_ghosts) {
        if (!IsWindow(g.hwnd)) continue;
        SetWindowPos(g.hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS);
        PlaceButton(g.button);
    }
}

// Has another app's always-on-top window been raised above a ghost?
bool GhostCovered(const Ghost& g) {
    for (HWND w = GetWindow(g.hwnd, GW_HWNDPREV); w; w = GetWindow(w, GW_HWNDPREV)) {
        if (IsOurs(w) || FindGhost(w) || !IsWindowVisible(w) || IsCloaked(w)) continue;
        LONG_PTR ex = GetWindowLongPtrW(w, GWL_EXSTYLE);
        if ((ex & WS_EX_TOPMOST) && !(ex & (WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT)) && !IsShellSurface(w)) return true;
    }
    return false;
}

// If GhostWin was killed while windows were see-through, they'd stay that
// way. On start, put back anything an earlier session left behind.
int RecoverState() {
    std::wstring path = StateFile();
    FILE* f = _wfopen(path.c_str(), L"r");
    if (!f) return 0;
    int restored = 0;
    wchar_t line[512];
    if (fgetws(line, ARRAYSIZE(line), f) && wcsncmp(line, L"GhostWin-state 1", 16) == 0) {
        while (fgetws(line, ARRAYSIZE(line), f)) {
            unsigned long long hw = 0, ex = 0;
            unsigned long pid = 0, key = 0, flags = 0;
            unsigned alpha = 255;
            int wasLayered = 0, hadAttrs = 0, wasTopmost = 0, opacity = 0;
            if (swscanf(line, L"%llx %lu %llx %d %d %lx %u %lx %d %d", &hw, &pid, &ex, &wasLayered, &hadAttrs, &key,
                        &alpha, &flags, &wasTopmost, &opacity) != 10)
                continue;
            Ghost g;
            g.hwnd = (HWND)(ULONG_PTR)hw;
            DWORD curPid = 0;
            if (!IsWindow(g.hwnd) || !GetWindowThreadProcessId(g.hwnd, &curPid) || curPid != pid) continue;
            LONG_PTR cur = GetWindowLongPtrW(g.hwnd, GWL_EXSTYLE);
            if ((cur & (WS_EX_LAYERED | WS_EX_TRANSPARENT)) != (WS_EX_LAYERED | WS_EX_TRANSPARENT)) continue;
            g.exStyle = (LONG_PTR)ex;
            g.wasLayered = wasLayered != 0;
            g.hadAttrs = hadAttrs != 0;
            g.key = key;
            g.alpha = (BYTE)alpha;
            g.flags = flags;
            g.wasTopmost = wasTopmost != 0;
            RevertWindow(g);
            ++restored;
        }
    }
    fclose(f);
    DeleteFileW(path.c_str());
    return restored;
}

// Last resort: an unexpected crash restores every window before dying.
LONG WINAPI CrashFilter(EXCEPTION_POINTERS*) {
    for (auto& g : g_ghosts) RevertWindow(g);
    DeleteFileW(StateFile().c_str());
    return EXCEPTION_CONTINUE_SEARCH;
}

// ------------------------------------------------------- active window
void UpdateActive() {
    HWND fg = GetForegroundWindow();
    if (fg && IsOurs(fg)) return;  // our own menu is open; leave things as they are
    bool want = g_set.placement != kPlaceOff && fg && !FindGhost(fg) && WantsButton(fg);
    if (!want) {
        if (g_active) {
            HideButton(g_active);
            g_active->target = nullptr;
        }
        return;
    }
    if (!g_active) g_active = CreateButton(fg, false);
    if (!g_active) return;
    g_active->target = fg;
    PlaceButton(g_active);
}

void Sync(HWND h) {
    if (g_active && g_active->target == h) PlaceButton(g_active);
    if (Ghost* g = FindGhost(h)) PlaceButton(g->button);
}

void SyncAll() {
    bool covered = false;
    for (size_t i = 0; i < g_ghosts.size();) {
        if (!IsWindow(g_ghosts[i].hwnd)) {
            ForgetGhost(g_ghosts[i].hwnd);
            continue;
        }
        covered = covered || GhostCovered(g_ghosts[i]);
        PlaceButton(g_ghosts[i].button);
        ++i;
    }
    if (covered) RaiseGhosts();
    UpdateActive();
}

void CALLBACK OnWinEvent(HWINEVENTHOOK, DWORD ev, HWND h, LONG obj, LONG child, DWORD, DWORD) {
    if (!h || obj != OBJID_WINDOW || child != CHILDID_SELF) return;
    switch (ev) {
        case EVENT_SYSTEM_FOREGROUND:
            RaiseGhosts();
            UpdateActive();
            break;
        case EVENT_SYSTEM_MOVESIZESTART:
            g_dragging = h;
            Sync(h);
            break;
        case EVENT_SYSTEM_MOVESIZEEND:
            g_dragging = nullptr;
            Sync(h);
            break;
        case EVENT_OBJECT_DESTROY:
            if (FindGhost(h)) ForgetGhost(h);
            if (g_active && g_active->target == h) UpdateActive();
            break;
        case EVENT_SYSTEM_MINIMIZESTART:
        case EVENT_SYSTEM_MINIMIZEEND:
        case EVENT_OBJECT_SHOW:
        case EVENT_OBJECT_HIDE:
        case EVENT_OBJECT_CLOAKED:
        case EVENT_OBJECT_UNCLOAKED:
        case EVENT_OBJECT_LOCATIONCHANGE:
            Sync(h);
            if (ev == EVENT_SYSTEM_MINIMIZEEND || ev == EVENT_OBJECT_SHOW || ev == EVENT_OBJECT_UNCLOAKED) UpdateActive();
            break;
    }
}

void InstallEventHooks() {
    const DWORD f = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;
    const DWORD ranges[][2] = {
        {EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND},
        {EVENT_SYSTEM_MOVESIZESTART, EVENT_SYSTEM_MINIMIZEEND},
        {EVENT_OBJECT_DESTROY, EVENT_OBJECT_HIDE},
        {EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE},
        {EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED},
    };
    for (auto& r : ranges)
        if (HWINEVENTHOOK hk = SetWinEventHook(r[0], r[1], nullptr, OnWinEvent, 0, 0, f)) g_hooks.push_back(hk);
}

// ------------------------------------------------------------------ menus
bool StartupEnabled() {
    wchar_t v[MAX_PATH * 2];
    DWORD n = sizeof v;
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, kApp, RRF_RT_REG_SZ, nullptr, v, &n) == ERROR_SUCCESS;
}
void SetStartup(bool on) {
    if (on) {
        std::wstring cmd = L"\"" + ExePath() + L"\"";
        RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, kApp, REG_SZ, cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        HKEY k;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
            RegDeleteValueW(k, kApp);
            RegCloseKey(k);
        }
    }
}

const int kOpacities[] = {10, 20, 30, 40, 50, 60, 70, 80, 90};

HMENU OpacityMenu(UINT base, int checked) {
    HMENU m = CreatePopupMenu();
    for (int p : kOpacities) {
        std::wstring s = std::to_wstring(p) + L"% visible";
        AppendMenuW(m, MF_STRING | (p == checked ? MF_CHECKED : 0), base + p, s.c_str());
    }
    return m;
}

UINT Track(HMENU m) {
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_main);
    UINT cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, g_main, nullptr);
    PostMessageW(g_main, WM_NULL, 0, 0);
    DestroyMenu(m);
    return cmd;
}

void ShowHelp() {
    std::wstring t =
        L"GhostWin " + std::wstring(kVersion) +
        L"\n\n"
        L"Click the ghost button on a window's title bar to make that window see-through. "
        L"It stays above every other window, and clicks and typing go to whatever is underneath.\n\n"
        L"Click the purple ghost button on a see-through window to bring it back to normal.\n\n"
        L"On the purple button:\n"
        L"   Mouse wheel: more or less visible\n"
        L"   Right-click: opacity and more\n\n"
        L"Keys:\n"
        L"   Ctrl+Alt+G: make the active window see-through\n"
        L"   Ctrl+Alt+Shift+G: bring back the most recent see-through window\n\n"
        L"Exiting GhostWin brings every window back to normal.";
    if (!g_set.hotkeys)
        t += L"\n\n(Keyboard shortcuts are turned off in the tray menu.)";
    else if (!g_hotkeysOk[kHotkeyGhost] || !g_hotkeysOk[kHotkeyBringBack])
        t += L"\n\n(One or both keys are in use by another app, so they're unavailable.)";
    MessageBoxW(nullptr, t.c_str(), kApp, MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
}

void RegisterHotkeys() {
    UnregisterHotKey(g_main, kHotkeyGhost);
    UnregisterHotKey(g_main, kHotkeyBringBack);
    g_hotkeysOk[kHotkeyGhost] = g_hotkeysOk[kHotkeyBringBack] = true;
    if (!g_set.hotkeys) return;
    g_hotkeysOk[kHotkeyGhost] = RegisterHotKey(g_main, kHotkeyGhost, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'G') != 0;
    g_hotkeysOk[kHotkeyBringBack] =
        RegisterHotKey(g_main, kHotkeyBringBack, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, 'G') != 0;
}

void ApplyPlacement(int p) {
    g_set.placement = p;
    RegPut(L"ButtonPlacement", (DWORD)p);
    SyncAll();
}

void TrayMenu() {
    HMENU m = CreatePopupMenu();
    if (g_ghosts.empty()) {
        AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"No see-through windows");
    } else {
        AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"Bring back:");
        for (size_t i = 0; i < g_ghosts.size() && i < 900; ++i) {
            std::wstring s = L"    " + TitleOf(g_ghosts[i].hwnd) + L"  (" + std::to_wstring(g_ghosts[i].opacity) + L"%)";
            AppendMenuW(m, MF_STRING, ID_GHOST_LIST + i, s.c_str());
        }
        AppendMenuW(m, MF_STRING, ID_RESTORE_ALL, L"Bring back &all");
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)OpacityMenu(ID_DEFAULT_OPACITY, g_set.opacity), L"&Opacity for new see-through windows");
    HMENU place = CreatePopupMenu();
    AppendMenuW(place, MF_STRING | (g_set.placement == kPlaceRight ? MF_CHECKED : 0), ID_PLACE_RIGHT, L"Next to the window's buttons");
    AppendMenuW(place, MF_STRING | (g_set.placement == kPlaceCenter ? MF_CHECKED : 0), ID_PLACE_CENTER, L"Middle of the title bar");
    AppendMenuW(place, MF_STRING | (g_set.placement == kPlaceOff ? MF_CHECKED : 0), ID_PLACE_OFF, L"Hidden (use Ctrl+Alt+G)");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)place, L"&Button on the active window");
    AppendMenuW(m, MF_STRING | (g_set.hotkeys ? MF_CHECKED : 0), ID_HOTKEYS, L"&Keyboard shortcuts (Ctrl+Alt+G)");
    AppendMenuW(m, MF_STRING | (StartupEnabled() ? MF_CHECKED : 0), ID_STARTUP, L"Start with &Windows");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_HELP, L"&How to use");
    AppendMenuW(m, MF_STRING, ID_EXIT, L"E&xit (brings everything back)");
    UINT cmd = Track(m);
    if (cmd >= ID_GHOST_LIST && cmd < ID_GHOST_LIST + g_ghosts.size()) {
        RestoreGhost(g_ghosts[cmd - ID_GHOST_LIST].hwnd, true);
    } else if (cmd > ID_DEFAULT_OPACITY && cmd <= ID_DEFAULT_OPACITY + 100) {
        g_set.opacity = (int)(cmd - ID_DEFAULT_OPACITY);
        RegPut(L"Opacity", (DWORD)g_set.opacity);
    } else {
        switch (cmd) {
            case ID_RESTORE_ALL: RestoreAll(); break;
            case ID_PLACE_RIGHT: ApplyPlacement(kPlaceRight); break;
            case ID_PLACE_CENTER: ApplyPlacement(kPlaceCenter); break;
            case ID_PLACE_OFF: ApplyPlacement(kPlaceOff); break;
            case ID_STARTUP: SetStartup(!StartupEnabled()); break;
            case ID_HOTKEYS:
                g_set.hotkeys = !g_set.hotkeys;
                RegPut(L"Hotkeys", g_set.hotkeys);
                RegisterHotkeys();
                break;
            case ID_HELP: ShowHelp(); break;
            case ID_EXIT: DestroyWindow(g_main); break;
        }
    }
}

void ButtonMenu(HWND target, bool ghost) {
    HMENU m = CreatePopupMenu();
    Ghost* g = ghost ? FindGhost(target) : nullptr;
    if (ghost && !g) {
        DestroyMenu(m);
        return;
    }
    if (ghost) {
        AppendMenuW(m, MF_STRING, ID_RESTORE_THIS, L"&Bring back to normal");
        AppendMenuW(m, MF_POPUP | (g->alphaControl ? 0 : MF_GRAYED), (UINT_PTR)OpacityMenu(ID_THIS_OPACITY, g->opacity), L"&Opacity");
        if (g_ghosts.size() > 1) AppendMenuW(m, MF_STRING, ID_RESTORE_ALL, L"Bring back &all");
    } else {
        AppendMenuW(m, MF_STRING, ID_GHOST_THIS, L"Make &see-through");
        AppendMenuW(m, MF_POPUP, (UINT_PTR)OpacityMenu(ID_THIS_OPACITY, -1), L"Make see-through &at");
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_HELP, L"&How to use");
    UINT cmd = Track(m);
    if (cmd > ID_THIS_OPACITY && cmd <= ID_THIS_OPACITY + 100) {
        int pct = (int)(cmd - ID_THIS_OPACITY);
        if (ghost) SetGhostOpacity(target, pct);
        else MakeGhost(target, pct, true);
    } else if (cmd == ID_GHOST_THIS) {
        MakeGhost(target, g_set.opacity, true);
    } else if (cmd == ID_RESTORE_THIS) {
        RestoreGhost(target, true);
    } else if (cmd == ID_RESTORE_ALL) {
        RestoreAll();
    } else if (cmd == ID_HELP) {
        ShowHelp();
    } else if (!ghost && IsWindow(target)) {
        SetForegroundWindow(target);  // menu dismissed: give the window its focus back
    }
    if (!cmd || cmd == ID_HELP) UpdateActive();
}

// ------------------------------------------------------- commands (CLI)
HWND ParseWindow(const std::wstring& s) {
    if (s == L"fg" || s == L"foreground" || s == L"active") return GetForegroundWindow();
    return (HWND)(ULONG_PTR)wcstoull(s.c_str(), nullptr, 0);
}

std::vector<std::wstring> Split(const std::wstring& s, wchar_t sep) {
    std::vector<std::wstring> out;
    size_t a = 0;
    while (a <= s.size()) {
        size_t b = s.find(sep, a);
        if (b == std::wstring::npos) b = s.size();
        if (b > a) out.push_back(s.substr(a, b - a));
        a = b + 1;
    }
    return out;
}

// Exit codes: 0 done, 1 refused / not found, 2 bad arguments.
int ExecCommand(const std::vector<std::wstring>& a) {
    if (a.empty()) return 2;
    const std::wstring& c = a[0];
    if (c == L"ghost" && a.size() >= 2) {
        int pct = a.size() >= 3 ? _wtoi(a[2].c_str()) : g_set.opacity;
        if (pct <= 0) return 2;
        HWND h = ParseWindow(a[1]);
        if (FindGhost(h) && a.size() >= 3) return SetGhostOpacity(h, pct) ? 0 : 1;
        return MakeGhost(h, pct, false) ? 0 : 1;
    }
    if (c == L"restore" && a.size() >= 2) return RestoreGhost(ParseWindow(a[1]), true) ? 0 : 1;
    if (c == L"toggle" && a.size() >= 2) {
        HWND h = ParseWindow(a[1]);
        if (FindGhost(h)) return RestoreGhost(h, true) ? 0 : 1;
        return MakeGhost(h, g_set.opacity, false) ? 0 : 1;
    }
    if (c == L"restore-all") return RestoreAll(), 0;
    if (c == L"bring-back") return BringBackLatest() ? 0 : 1;
    if (c == L"exit") return DestroyWindow(g_main), 0;
    return 2;
}

// ------------------------------------------------------------ main window
LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_taskbarCreated && g_taskbarCreated) {  // Explorer restarted
        AddTrayIcon();
        return 0;
    }
    switch (msg) {
        case WM_GW_CLICK: {
            HWND t = (HWND)wp;
            if (lp) RestoreGhost(t, true);
            else MakeGhost(t, g_set.opacity, true);
            return 0;
        }
        case WM_GW_MENU:
            ButtonMenu((HWND)wp, lp != 0);
            return 0;
        case WM_HOTKEY:
            if (wp == kHotkeyGhost) {
                HWND fg = GetForegroundWindow();
                if (fg && FindGhost(fg)) RestoreGhost(fg, true);
                else MakeGhost(fg, g_set.opacity, true);
            } else if (wp == kHotkeyBringBack) {
                if (!BringBackLatest()) Notify(L"No see-through windows to bring back.");
            }
            return 0;
        case WM_TRAY:
            switch (LOWORD(lp)) {
                case WM_CONTEXTMENU:
                case NIN_SELECT:
                case NIN_KEYSELECT:
                case WM_LBUTTONUP:
                case WM_RBUTTONUP:
                    TrayMenu();
                    break;
            }
            return 0;
        case WM_TIMER:
            if (wp == kSyncTimer) SyncAll();
            return 0;
        case WM_NOTIFY: {
            auto* hdr = reinterpret_cast<NMHDR*>(lp);
            if (hdr->code == TTN_GETDISPINFOW) {
                auto* di = reinterpret_cast<NMTTDISPINFOW*>(lp);
                static std::wstring text;
                Button* b = ButtonFromHwnd((HWND)hdr->idFrom);
                if (!b) text.clear();
                else if (!b->ghost) text = L"Make see-through (" + std::to_wstring(g_set.opacity) +
                                           L"%), click-through and always on top\nRight-click: pick the opacity";
                else {
                    Ghost* g = FindGhost(b->target);
                    text = L"Bring back to normal";
                    if (g && g->alphaControl) text += L"\nWheel: more or less visible (now " + std::to_wstring(g->opacity) + L"%)";
                    text += L"\nRight-click: more";
                }
                di->lpszText = const_cast<LPWSTR>(text.c_str());
                return 0;
            }
            break;
        }
        case WM_COPYDATA: {
            auto* cds = reinterpret_cast<COPYDATASTRUCT*>(lp);
            if (!cds || cds->dwData != kCopyDataCommand || !cds->lpData || cds->cbData < sizeof(wchar_t)) return 2;
            std::wstring s((const wchar_t*)cds->lpData, cds->cbData / sizeof(wchar_t));
            while (!s.empty() && s.back() == L'\0') s.pop_back();
            return ExecCommand(Split(s, L'\n'));
        }
        case WM_QUERYENDSESSION:
            return TRUE;
        case WM_ENDSESSION:
            if (wp) RestoreAll();
            return 0;
        case WM_DESTROY:
            RestoreAll();
            KillTimer(hwnd, kSyncTimer);
            for (auto hk : g_hooks) UnhookWinEvent(hk);
            g_hooks.clear();
            UnregisterHotKey(hwnd, kHotkeyGhost);
            UnregisterHotKey(hwnd, kHotkeyBringBack);
            if (g_active) DestroyButton(g_active), g_active = nullptr;
            Shell_NotifyIconW(NIM_DELETE, &g_nid);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void EnableDpiAwareness() {
    using SetCtx_t = BOOL(WINAPI*)(HANDLE);
    if (auto f = (SetCtx_t)(void*)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"))
        if (f((HANDLE)-4 /* PER_MONITOR_AWARE_V2 */)) return;
    SetProcessDPIAware();
}

bool SelfIsElevated() {
    bool assume = false;
    return ProcessIsElevated(GetCurrentProcessId(), &assume);
}

std::vector<std::wstring> CommandFromArgs(int argc, wchar_t** argv, bool* bad) {
    std::vector<std::wstring> c;
    *bad = false;
    if (argc < 2) return c;
    std::wstring a = argv[1];
    auto need = [&](int n) {
        if (argc < 2 + n) *bad = true;
    };
    if (a == L"--ghost") {
        need(1);
        c = {L"ghost"};
    } else if (a == L"--restore") {
        need(1);
        c = {L"restore"};
    } else if (a == L"--toggle") {
        need(1);
        c = {L"toggle"};
    } else if (a == L"--restore-all") {
        c = {L"restore-all"};
    } else if (a == L"--bring-back") {
        c = {L"bring-back"};
    } else if (a == L"--exit") {
        c = {L"exit"};
    } else {
        *bad = true;
        return c;
    }
    for (int i = 2; i < argc; ++i) c.push_back(argv[i]);
    return c;
}

void Usage() {
    const wchar_t* text =
                L"GhostWin.exe                       start (tray icon)\n"
                L"GhostWin.exe --ghost <window> [percent]\n"
                L"GhostWin.exe --restore <window>\n"
                L"GhostWin.exe --toggle <window>\n"
                L"GhostWin.exe --bring-back          most recent see-through window\n"
                L"GhostWin.exe --restore-all\n"
                L"GhostWin.exe --exit                brings everything back, then quits\n\n"
                L"<window> is a window handle (decimal or 0x hex) or \"active\".\n"
                L"Exit code: 0 done, 1 refused or not found, 2 bad arguments, 3 GhostWin not responding.\n";
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out && out != INVALID_HANDLE_VALUE && GetFileType(out) != FILE_TYPE_UNKNOWN) {  // redirected / piped
        std::string u(WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr), '\0');
        WideCharToMultiByte(CP_UTF8, 0, text, -1, &u[0], (int)u.size(), nullptr, nullptr);
        DWORD n = 0;
        WriteFile(out, u.data(), (DWORD)u.size() - 1, &n, nullptr);
        return;
    }
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {  // started from a terminal: print there
        DWORD n = 0;
        WriteConsoleW(GetStdHandle(STD_OUTPUT_HANDLE), L"\n", 1, &n, nullptr);
        WriteConsoleW(GetStdHandle(STD_OUTPUT_HANDLE), text, (DWORD)wcslen(text), &n, nullptr);
        FreeConsole();
        return;
    }
    MessageBoxW(nullptr, text, kApp, MB_OK | MB_ICONINFORMATION);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    g_inst = inst;
    EnableDpiAwareness();
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool bad = false;
    std::vector<std::wstring> cmd = CommandFromArgs(argc, argv, &bad);
    if (argv) LocalFree(argv);
    if (bad) {
        Usage();
        return 2;
    }

    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = nullptr;
        for (int i = 0; i < 50 && !(other = FindWindowW(kMainClass, nullptr)); ++i) Sleep(100);
        if (!other) return 3;
        if (cmd.empty()) {
            MessageBoxW(nullptr, L"GhostWin is already running. Look for the ghost in the notification area.", kApp,
                        MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        DWORD pid = 0;
        GetWindowThreadProcessId(other, &pid);
        AllowSetForegroundWindow(pid);  // so "--restore" can bring the window to the front
        std::wstring s;
        for (auto& p : cmd) s += p + L'\n';
        COPYDATASTRUCT cds{kCopyDataCommand, (DWORD)((s.size() + 1) * sizeof(wchar_t)), (PVOID)s.c_str()};
        DWORD_PTR r = 3;
        if (!SendMessageTimeoutW(other, WM_COPYDATA, 0, (LPARAM)&cds, SMTO_ABORTIFHUNG, 10000, &r)) return 3;
        return (int)r;
    }
    if (!cmd.empty() && (cmd[0] == L"exit" || cmd[0] == L"restore-all")) {
        // Not running: nothing to do, but clean up after a crashed session.
        RecoverState();
        return 0;
    }

    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_WIN95_CLASSES};
    InitCommonControlsEx(&icc);
    LoadSettings();
    g_elevated = SelfIsElevated();
    SetUnhandledExceptionFilter(CrashFilter);

    g_iconSmall = MakeIcon(GetSystemMetrics(SM_CXSMICON));
    g_iconBig = MakeIcon(GetSystemMetrics(SM_CXICON));

    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = MainProc;
    wc.hInstance = inst;
    wc.lpszClassName = kMainClass;
    wc.hIcon = g_iconBig;
    wc.hIconSm = g_iconSmall;
    RegisterClassExW(&wc);
    WNDCLASSEXW bc{sizeof bc};
    bc.lpfnWndProc = ButtonProc;
    bc.hInstance = inst;
    bc.lpszClassName = kButtonClass;
    bc.hCursor = LoadCursorW(nullptr, IDC_HAND);
    RegisterClassExW(&bc);

    g_main = CreateWindowExW(WS_EX_TOOLWINDOW, kMainClass, kApp, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    if (!g_main) return 1;
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    ChangeWindowMessageFilterEx(g_main, g_taskbarCreated, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(g_main, WM_COPYDATA, MSGFLT_ALLOW, nullptr);  // CLI from a non-elevated prompt

    g_tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, 0, 0, 0,
                            0, nullptr, nullptr, inst, nullptr);
    if (g_tip) SendMessageW(g_tip, TTM_SETMAXTIPWIDTH, 0, 420);

    AddTrayIcon();
    int recovered = RecoverState();
    if (recovered)
        Notify(std::to_wstring(recovered) + L" window(s) left see-through by an earlier GhostWin session were brought back.");

    RegisterHotkeys();
    InstallEventHooks();
    SetTimer(g_main, kSyncTimer, kSyncMs, nullptr);
    UpdateActive();

    int code = 0;
    if (!cmd.empty()) code = ExecCommand(cmd);  // first launch with a command: run it, keep running
    if (!RegGet(L"WelcomeShown", 0)) {
        RegPut(L"WelcomeShown", 1);
        Notify(L"Click the ghost button on any window's title bar to make it see-through. Right-click the tray icon for options.");
    }

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (mutex) CloseHandle(mutex);
    return code;
}
