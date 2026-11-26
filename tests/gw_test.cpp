// gw_test.cpp — drives a real GhostWin.exe from another process, the way a
// user would: test windows live here, GhostWin changes them from outside.
//   gw_test.exe <path to GhostWin.exe>
// This thread keeps pumping messages while it waits, because GhostWin's
// style changes are delivered to these windows synchronously.
#include <windows.h>
#include <stdio.h>
#include <string>
#include <vector>

static int g_pass, g_fail;
#define CHECK(c, ...)                              \
    do {                                           \
        if (c) {                                   \
            ++g_pass;                              \
        } else {                                   \
            ++g_fail;                              \
            wprintf(L"FAIL line %d: ", __LINE__);  \
            wprintf(__VA_ARGS__);                  \
            wprintf(L"\n");                        \
        }                                          \
    } while (0)

static std::wstring g_exe, g_stateDir;

static void Pump(int ms) {
    DWORD end = GetTickCount() + ms;
    while ((int)(end - GetTickCount()) > 0) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
    }
}

static bool WaitProcess(HANDLE p, int ms) {
    DWORD end = GetTickCount() + ms;
    while ((int)(end - GetTickCount()) > 0) {
        DWORD r = MsgWaitForMultipleObjects(1, &p, FALSE, 20, QS_ALLINPUT);
        if (r == WAIT_OBJECT_0) return true;
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    return false;
}

static LRESULT CALLBACK TestProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ERASEBKGND) {
        RECT rc;
        GetClientRect(h, &rc);
        HBRUSH b = CreateSolidBrush((COLORREF)GetWindowLongPtrW(h, GWLP_USERDATA));
        FillRect((HDC)wp, &rc, b);
        DeleteObject(b);
        return 1;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static HWND MakeWin(const wchar_t* title, int x, int y, int w, int hgt, DWORD ex = 0, COLORREF c = RGB(30, 90, 160)) {
    HWND h = CreateWindowExW(ex, L"GwTestWin", title, WS_OVERLAPPEDWINDOW | WS_VISIBLE, x, y, w, hgt, nullptr, nullptr,
                             GetModuleHandleW(nullptr), nullptr);
    SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)c);
    InvalidateRect(h, nullptr, TRUE);
    Pump(150);
    return h;
}

static PROCESS_INFORMATION Launch(const std::wstring& args) {
    std::wstring cmd = L"\"" + g_exe + L"\" " + args;
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    // Give GhostWin a stdout (NUL) so usage text never becomes a message box here.
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = nul;
    si.hStdInput = nullptr;
    BOOL ok = CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi);
    CloseHandle(nul);
    if (!ok)
        wprintf(L"can't start %ls (%lu)\n", cmd.c_str(), GetLastError());
    return pi;
}

static int Cli(const std::wstring& args) {
    PROCESS_INFORMATION pi = Launch(args);
    if (!pi.hProcess) return -1;
    DWORD code = 99;
    if (WaitProcess(pi.hProcess, 15000)) GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    Pump(250);
    return (int)code;
}

static std::wstring Hex(HWND h) {
    wchar_t b[32];
    swprintf(b, 32, L"0x%llx", (unsigned long long)(ULONG_PTR)h);
    return b;
}

static PROCESS_INFORMATION StartGhostWin() {
    PROCESS_INFORMATION pi = Launch(L"");
    for (int i = 0; i < 100 && !FindWindowW(L"GhostWin.Main", nullptr); ++i) Pump(50);
    Pump(700);
    return pi;
}

struct Btn {
    HWND h;
    RECT r;
    bool visible;
};
static BOOL CALLBACK CollectButtons(HWND h, LPARAM lp) {
    wchar_t c[64];
    GetClassNameW(h, c, 64);
    if (!wcscmp(c, L"GhostWin.Button")) {
        Btn b{h, {}, IsWindowVisible(h) != 0};
        GetWindowRect(h, &b.r);
        reinterpret_cast<std::vector<Btn>*>(lp)->push_back(b);
    }
    return TRUE;
}
static std::vector<Btn> Buttons() {
    std::vector<Btn> v;
    EnumWindows(CollectButtons, (LPARAM)&v);
    return v;
}
// The visible button sitting on `target`'s title bar, if any.
static HWND ButtonOn(HWND target, RECT* out = nullptr) {
    RECT w;
    GetWindowRect(target, &w);
    for (auto& b : Buttons())
        if (b.visible && b.r.left >= w.left && b.r.right <= w.right && b.r.top >= w.top && b.r.bottom <= w.top + 64) {
            if (out) *out = b.r;
            return b.h;
        }
    return nullptr;
}

static LONG_PTR Ex(HWND h) { return GetWindowLongPtrW(h, GWL_EXSTYLE); }
static bool Ghosted(HWND h) {
    LONG_PTR e = Ex(h);
    return (e & WS_EX_LAYERED) && (e & WS_EX_TRANSPARENT) && (e & WS_EX_TOPMOST);
}
static bool Clean(HWND h) { return !(Ex(h) & (WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST)); }
static int Alpha(HWND h) {
    BYTE a = 0;
    COLORREF k;
    DWORD f = 0;
    if (!GetLayeredWindowAttributes(h, &k, &a, &f) || !(f & LWA_ALPHA)) return -1;
    return a;
}

static void Click(HWND b) {
    PostMessageW(b, WM_MOUSEMOVE, 0, MAKELPARAM(4, 4));
    PostMessageW(b, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(4, 4));
    PostMessageW(b, WM_LBUTTONUP, 0, MAKELPARAM(4, 4));
    Pump(500);
}

static bool StateMentions(HWND h) {
    FILE* f = _wfopen((g_stateDir + L"\\ghosts.txt").c_str(), L"r");
    if (!f) return false;
    wchar_t line[512];
    wchar_t want[32];
    swprintf(want, 32, L"%llx ", (unsigned long long)(ULONG_PTR)h);
    bool found = false;
    while (fgetws(line, 512, f))
        if (!wcsncmp(line, want, wcslen(want))) found = true;
    fclose(f);
    return found;
}
static bool StateExists() { return GetFileAttributesW((g_stateDir + L"\\ghosts.txt").c_str()) != INVALID_FILE_ATTRIBUTES; }

// Is `upper` above `lower` in the z-order?
static bool Above(HWND upper, HWND lower) {
    for (HWND w = GetWindow(upper, GW_HWNDNEXT); w; w = GetWindow(w, GW_HWNDNEXT))
        if (w == lower) return true;
    return false;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        wprintf(L"usage: gw_test.exe <GhostWin.exe>\n");
        return 2;
    }
    g_exe = argv[1];
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    g_stateDir = std::wstring(tmp) + L"gw_test_state";
    CreateDirectoryW(g_stateDir.c_str(), nullptr);
    DeleteFileW((g_stateDir + L"\\ghosts.txt").c_str());
    SetEnvironmentVariableW(L"GHOSTWIN_STATE_DIR", g_stateDir.c_str());  // inherited by GhostWin
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\GhostWin");             // default settings
    RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\GhostWin", L"WelcomeShown", REG_DWORD, "\1\0\0\0", 4);

    WNDCLASSW wc{};
    wc.lpfnWndProc = TestProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"GwTestWin";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    HWND A = MakeWin(L"Test A", 60, 60, 640, 420, 0, RGB(30, 90, 160));
    HWND B = MakeWin(L"Test B", 760, 120, 560, 420, 0, RGB(160, 70, 40));
    SetForegroundWindow(A);
    Pump(200);

    PROCESS_INFORMATION gw = StartGhostWin();
    { wchar_t c[64] = L""; GetClassNameW(GetForegroundWindow(), c, 64); wprintf(L"   fg after start: %ls\n", c); }
    SetForegroundWindow(A);  // as a user would: activate the window to work with
    Pump(700);
    CHECK(FindWindowW(L"GhostWin.Main", nullptr) != nullptr, L"GhostWin didn't start");

    // 1. The button sits on the active window's title bar, right of centre.
    wprintf(L"-- button on the active window\n");
    RECT br{}, ar;
    GetWindowRect(A, &ar);
    HWND bA = ButtonOn(A, &br);
    wprintf(L"   fg=%p A=%p buttons=%zu\n", GetForegroundWindow(), A, Buttons().size());
    for (auto& b : Buttons()) wprintf(L"   btn vis=%d %ld,%ld,%ld,%ld\n", b.visible, b.r.left, b.r.top, b.r.right, b.r.bottom);
    wprintf(L"   A rect %ld,%ld,%ld,%ld\n", ar.left, ar.top, ar.right, ar.bottom);
    GetWindowRect(A, &ar);
    CHECK(bA != nullptr, L"no button on the active window's title bar");
    if (bA) {
        CHECK(br.left > (ar.left + ar.right) / 2 && br.right < ar.right - 40, L"button not next to the caption buttons: %ld..%ld in %ld..%ld",
              br.left, br.right, ar.left, ar.right);
        CHECK(br.bottom - br.top >= 14 && br.right - br.left >= 20, L"button too small");
        CHECK(Ex(bA) & WS_EX_NOACTIVATE, L"button can take focus");
    }

    // 2. Click it: A becomes a ghost at the default 40%.
    wprintf(L"-- click: make see-through\n");
    if (bA) Click(bA);
    CHECK(Ghosted(A), L"A not ghosted (ex=0x%llx)", (unsigned long long)Ex(A));
    CHECK(Alpha(A) == 102, L"A alpha %d, want 102 (40%%)", Alpha(A));
    CHECK(GetForegroundWindow() != A, L"ghost kept the keyboard focus");
    CHECK(StateMentions(A), L"state file doesn't list A");
    HWND gA = ButtonOn(A);
    CHECK(gA != nullptr, L"ghost has no button to bring it back");
    CHECK(gA && !(Ex(gA) & WS_EX_TRANSPARENT), L"ghost's button is click-through");
    CHECK(gA && Above(gA, A), L"ghost's button is under the ghost");

    if (GetEnvironmentVariableW(L"GW_PAUSE", nullptr, 0)) {  // screenshot hook for the README
        SetForegroundWindow(B);
        Pump(800);
        CloseHandle(CreateFileW(L"Z:\\tmp\\gw_shot_now", GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr));
        Pump(3000);
    }

    // 3. Mouse wheel on its button: +5%.
    wprintf(L"-- wheel: opacity\n");
    if (gA) {
        PostMessageW(gA, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), 0);
        Pump(300);
        CHECK(Alpha(A) == 114, L"after wheel up alpha %d, want 114 (45%%)", Alpha(A));
        PostMessageW(gA, WM_MOUSEWHEEL, MAKEWPARAM(0, (WORD)(short)-WHEEL_DELTA), 0);
        PostMessageW(gA, WM_MOUSEWHEEL, MAKEWPARAM(0, (WORD)(short)-WHEEL_DELTA), 0);
        Pump(300);
        CHECK(Alpha(A) == 89, L"after 2x wheel down alpha %d, want 89 (35%%)", Alpha(A));
    }

    // 4. Ghosts stay above other always-on-top windows when those activate.
    wprintf(L"-- stays above other topmost windows\n");
    HWND T = MakeWin(L"Topmost app", 120, 120, 300, 200, WS_EX_TOPMOST, RGB(40, 140, 60));
    SetForegroundWindow(T);
    Pump(1200);
    CHECK(Above(A, T), L"ghost fell below a newly activated always-on-top window");
    DestroyWindow(T);
    Pump(200);

    // 5. Click the ghost's button: back to normal and in front.
    wprintf(L"-- click: bring back\n");
    gA = ButtonOn(A);
    if (gA) Click(gA);
    CHECK(Clean(A), L"A not restored (ex=0x%llx)", (unsigned long long)Ex(A));
    CHECK(!StateExists(), L"state file left behind with no ghosts");
    Pump(300);

    // 6. Command line.
    wprintf(L"-- command line\n");
    CHECK(Cli(L"--ghost " + Hex(A) + L" 30") == 0, L"--ghost failed");
    CHECK(Ghosted(A) && Alpha(A) == 76, L"--ghost 30: alpha %d", Alpha(A));
    CHECK(Cli(L"--ghost " + Hex(A) + L" 60") == 0 && Alpha(A) == 153, L"--ghost on a ghost should change opacity: %d", Alpha(A));
    CHECK(Cli(L"--toggle " + Hex(A)) == 0 && Clean(A), L"--toggle didn't restore");
    CHECK(Cli(L"--restore " + Hex(A)) == 1, L"--restore on a normal window should fail");
    { int rc = Cli(L"--ghost"); wprintf(L"   --ghost alone -> %d\n", rc); }
    CHECK(Cli(L"--ghost") == 2, L"missing argument should be exit 2");
    CHECK(Cli(L"--ghost 0x1") == 1, L"bogus window should be exit 1");
    HWND tool = CreateWindowExW(WS_EX_TOOLWINDOW, L"GwTestWin", L"tool", WS_POPUP | WS_VISIBLE, 10, 600, 200, 100, nullptr,
                                nullptr, GetModuleHandleW(nullptr), nullptr);
    Pump(100);
    CHECK(Cli(L"--ghost " + Hex(tool)) == 1, L"tool windows must be refused");
    DestroyWindow(tool);

    // 7. Windows that were already layered / always on top get their own state back.
    wprintf(L"-- pre-existing layered and topmost windows\n");
    HWND C = MakeWin(L"Layered", 200, 500, 400, 300, WS_EX_LAYERED, RGB(120, 40, 140));
    SetLayeredWindowAttributes(C, 0, 200, LWA_ALPHA);
    HWND D = MakeWin(L"Topmost", 700, 560, 400, 300, WS_EX_TOPMOST, RGB(40, 120, 120));
    CHECK(Cli(L"--ghost " + Hex(C) + L" 50") == 0 && Alpha(C) == 127, L"layered window: alpha %d", Alpha(C));
    CHECK(Cli(L"--ghost " + Hex(D)) == 0 && Ghosted(D), L"topmost window not ghosted");
    CHECK(Cli(L"--restore-all") == 0, L"--restore-all failed");
    CHECK((Ex(C) & WS_EX_LAYERED) && !(Ex(C) & WS_EX_TRANSPARENT) && Alpha(C) == 200, L"layered window not restored: ex=0x%llx alpha=%d",
          (unsigned long long)Ex(C), Alpha(C));
    CHECK((Ex(D) & WS_EX_TOPMOST) && !(Ex(D) & (WS_EX_LAYERED | WS_EX_TRANSPARENT)), L"topmost window lost topmost: ex=0x%llx",
          (unsigned long long)Ex(D));

    // 8. Bring back the most recent ghost.
    wprintf(L"-- bring back latest\n");
    Cli(L"--ghost " + Hex(A));
    Cli(L"--ghost " + Hex(B));
    CHECK(Cli(L"--bring-back") == 0 && Clean(B) && Ghosted(A), L"--bring-back should restore B (latest) only");
    Cli(L"--restore-all");

    // 9. A ghost window that closes takes its button with it.
    wprintf(L"-- ghost window closed\n");
    HWND E = MakeWin(L"Closes", 300, 300, 400, 300);
    Cli(L"--ghost " + Hex(E));
    size_t before = Buttons().size();
    DestroyWindow(E);
    Pump(1200);
    CHECK(Buttons().size() == before - 1, L"button of a closed ghost wasn't removed (%zu -> %zu)", before, Buttons().size());
    CHECK(!StateExists(), L"closed ghost still in the state file");

    // 10. GhostWin killed while a window is a ghost: the next start brings it back.
    wprintf(L"-- crash recovery\n");
    Cli(L"--ghost " + Hex(B));
    CHECK(Ghosted(B) && StateMentions(B), L"B not ghosted before the crash test");
    TerminateProcess(gw.hProcess, 9);
    WaitProcess(gw.hProcess, 5000);
    CloseHandle(gw.hProcess);
    CloseHandle(gw.hThread);
    Pump(300);
    CHECK(Ghosted(B), L"(setup) B should still be a ghost after the kill");
    gw = StartGhostWin();
    CHECK(Clean(B), L"B not recovered on restart (ex=0x%llx)", (unsigned long long)Ex(B));
    CHECK(!StateExists(), L"state file not cleared after recovery");

    // 11. --exit brings everything back.
    wprintf(L"-- exit\n");
    Cli(L"--ghost " + Hex(A));
    Cli(L"--ghost " + Hex(B));
    CHECK(Cli(L"--exit") == 0, L"--exit failed");
    CHECK(WaitProcess(gw.hProcess, 5000), L"GhostWin didn't quit");
    CHECK(Clean(A) && Clean(B), L"windows not restored on exit");
    CHECK(Buttons().empty(), L"buttons left after exit");

    wprintf(L"%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
