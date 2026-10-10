/*
 * Claude Dictate - global hotkey for the dictation button in the Claude
 * desktop app (Windows). Uses Claude's own dictation via UI Automation.
 *
 * Shortcut (default Win+Y, changeable from the tray menu):
 *   - idle (Chat tab)       -> invokes "Dictate"
 *   - while dictating       -> invokes "Finish dictation"
 *   - Code tab              -> toggles "Press and hold to record" / "Stop dictation"
 *
 * Runs in the background with a tray icon: click = same as the shortcut,
 * right-click -> Change shortcut / Exit.
 * The shortcut is saved in HKCU\Software\ClaudeDictate.
 * Log file: claude_dictate.log next to the exe.
 *
 * Build: build.cmd   (MSVC or clang; see README.md)
 */
#define COBJMACROS
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <shellapi.h>
#include <uiautomation.h>
#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>
#include <dwmapi.h>

/* The few GDI+ flat-API functions used for anti-aliased rounded shapes
   (gdiplus.h is C++-only). */
typedef struct { UINT32 version; void *debug_cb; BOOL no_bg_thread, no_codecs; } GdiplusStartupInput;
int WINAPI GdiplusStartup(ULONG_PTR *token, const GdiplusStartupInput *in, void *out);
int WINAPI GdipCreateFromHDC(HDC dc, void **g);
int WINAPI GdipDeleteGraphics(void *g);
int WINAPI GdipSetSmoothingMode(void *g, int mode);
int WINAPI GdipCreatePath(int fill_mode, void **path);
int WINAPI GdipAddPathArc(void *path, float x, float y, float w, float h, float start, float sweep);
int WINAPI GdipClosePathFigure(void *path);
int WINAPI GdipDeletePath(void *path);
int WINAPI GdipCreatePen1(DWORD argb, float width, int unit, void **pen);
int WINAPI GdipDeletePen(void *pen);
int WINAPI GdipDrawPath(void *g, void *pen, void *path);
int WINAPI GdipCreateSolidFill(DWORD argb, void **brush);
int WINAPI GdipDeleteBrush(void *brush);
int WINAPI GdipFillPath(void *g, void *brush, void *path);
#define GP_SMOOTH_ANTIALIAS 4
#define GP_UNIT_PIXEL       2

/* ---- Names as exposed by the Claude app. An app update may rename these. ---- */
#define PROCESS_EXE        L"claude.exe"
#define BTN_DICTATE        L"Dictate"
#define BTN_FINISH         L"Finish dictation"
#define BTN_CANCEL         L"Cancel dictation"
#define BTN_CODE_RECORD    L"Press and hold to record"
#define BTN_CODE_STOP      L"Stop dictation"

/* ---- Behaviour ---- */
#define DEFAULT_MODS       MOD_WIN   /* default shortcut: Win+Y */
#define DEFAULT_VK         'Y'
#define REG_KEY            L"Software\\ClaudeDictate"
#define FOCUS_WINDOW       1     /* bring Claude to front before invoking */
#define FIND_RETRIES       12    /* accessibility tree is built lazily */
#define FIND_RETRY_MS      250
#define POLL_MS            500   /* recording-state check while red */
#define POLL_MISSES        4     /* misses in a row before going back to idle */

#define MASK_VK            0xE8  /* unassigned key, injected so a lone Win/Alt
                                    release doesn't open Start or a menu bar */

#define POLL_TIMER_ID      1
#define WM_TRAY            (WM_APP + 1)
#define WM_SHORTCUT_HIT    (WM_APP + 2)  /* hook thread -> main window */
#define WM_DLG_MODS        (WM_APP + 3)  /* hook thread -> shortcut window */
#define WM_DLG_KEY         (WM_APP + 4)
#define IDM_EXIT           100
#define IDM_SHORTCUT       101
#define IDC_SAVE           201
#define IDC_CANCEL         202
#define IDC_RESET          203

/* GUIDs defined locally so we don't need uuid.lib */
static const CLSID my_CLSID_CUIAutomation =
    {0xff48dba4, 0x60ef, 0x4201, {0xaa, 0x87, 0x54, 0x10, 0x3e, 0xef, 0x59, 0x4e}};
static const IID my_IID_IUIAutomation =
    {0x30cbe57d, 0xd9d0, 0x452a, {0xab, 0x13, 0x7a, 0xc5, 0xac, 0x48, 0x25, 0xee}};
static const IID my_IID_IUIAutomationInvokePattern =
    {0xfb377fbe, 0x8ea6, 0x46d5, {0x9c, 0x73, 0x64, 0x99, 0x64, 0x2d, 0x30, 0x59}};
static const IID my_IID_IUIAutomationTogglePattern =
    {0x94cf8058, 0x9b8d, 0x4ab9, {0x8b, 0xfd, 0x4c, 0xd0, 0xa3, 0x3c, 0x8c, 0x70}};

static IUIAutomation *g_uia;
static NOTIFYICONDATAW g_nid;
static FILE *g_log;
static HWND g_main;                       /* hidden window that receives the shortcut */
static volatile UINT g_hk_mods = DEFAULT_MODS;   /* current shortcut (read by hook thread) */
static volatile UINT g_hk_vk = DEFAULT_VK;
static HWND volatile g_dlg;               /* "Change shortcut" window, if open */

static void logf_(const char *fmt, ...)
{
    if (!g_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

static void open_log(void)
{
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
    wchar_t *slash = wcsrchr(path, L'\\');
    if (!n || !slash) return;
    wcscpy(slash + 1, L"claude_dictate.log");
    g_log = _wfopen(path, L"w");
}

/* ---------------- Finding the Claude window ---------------- */

static BOOL is_claude_process(DWORD pid)
{
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return FALSE;
    wchar_t path[MAX_PATH];
    DWORD len = MAX_PATH;
    BOOL ok = QueryFullProcessImageNameW(h, 0, path, &len);
    CloseHandle(h);
    if (!ok) return FALSE;
    const wchar_t *base = wcsrchr(path, L'\\');
    return _wcsicmp(base ? base + 1 : path, PROCESS_EXE) == 0;
}

#define MAX_WINDOWS 16
typedef struct { HWND list[MAX_WINDOWS]; int count; } WindowList;

static BOOL CALLBACK enum_proc(HWND hwnd, LPARAM lp)
{
    WindowList *wl = (WindowList *)lp;
    if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (is_claude_process(pid) && wl->count < MAX_WINDOWS)
        wl->list[wl->count++] = hwnd;
    return TRUE;
}

/* ---------------- UI Automation helpers ---------------- */

static IUIAutomationElement *find_button(IUIAutomationElement *root, const wchar_t *name)
{
    IUIAutomationCondition *c_type = NULL, *c_name = NULL, *c_and = NULL;
    IUIAutomationElement *found = NULL;
    VARIANT v;

    VariantInit(&v);
    v.vt = VT_I4;
    v.lVal = UIA_ButtonControlTypeId;
    IUIAutomation_CreatePropertyCondition(g_uia, UIA_ControlTypePropertyId, v, &c_type);

    VariantInit(&v);
    v.vt = VT_BSTR;
    v.bstrVal = SysAllocString(name);
    IUIAutomation_CreatePropertyCondition(g_uia, UIA_NamePropertyId, v, &c_name);
    VariantClear(&v);

    if (c_type && c_name &&
        SUCCEEDED(IUIAutomation_CreateAndCondition(g_uia, c_type, c_name, &c_and)))
        IUIAutomationElement_FindFirst(root, TreeScope_Descendants, c_and, &found);

    if (c_and) IUIAutomationCondition_Release(c_and);
    if (c_name) IUIAutomationCondition_Release(c_name);
    if (c_type) IUIAutomationCondition_Release(c_type);
    return found;
}

static BOOL invoke(IUIAutomationElement *el)
{
    IUIAutomationInvokePattern *p = NULL;
    if (FAILED(IUIAutomationElement_GetCurrentPatternAs(el, UIA_InvokePatternId,
            &my_IID_IUIAutomationInvokePattern, (void **)&p)) || !p)
        return FALSE;
    HRESULT hr = IUIAutomationInvokePattern_Invoke(p);
    IUIAutomationInvokePattern_Release(p);
    return SUCCEEDED(hr);
}

static BOOL toggle(IUIAutomationElement *el)
{
    IUIAutomationTogglePattern *p = NULL;
    if (FAILED(IUIAutomationElement_GetCurrentPatternAs(el, UIA_TogglePatternId,
            &my_IID_IUIAutomationTogglePattern, (void **)&p)) || !p)
        return FALSE;
    HRESULT hr = IUIAutomationTogglePattern_Toggle(p);
    IUIAutomationTogglePattern_Release(p);
    return SUCCEEDED(hr);
}

static void focus_window(HWND hwnd)
{
    if (!FOCUS_WINDOW) return;
    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    SetForegroundWindow(hwnd);
}

/* Stop buttons first, so the hotkey acts as a toggle. */
static const wchar_t *const g_order[] = {
    BTN_FINISH, BTN_CODE_STOP, BTN_DICTATE, BTN_CODE_RECORD
};
#define N_STOP_BUTTONS 2   /* the first entries of g_order stop recording */

/* One attempt over all Claude windows. Returns the g_order index of the
   button that was pressed, or -1. */
static int try_once(BOOL *any_window)
{
    WindowList wl = {0};
    EnumWindows(enum_proc, (LPARAM)&wl);
    *any_window = wl.count > 0;

    for (int i = 0; i < wl.count; i++) {
        IUIAutomationElement *root = NULL, *btn;
        if (FAILED(IUIAutomation_ElementFromHandle(g_uia, wl.list[i], &root)) || !root)
            continue;

        /* The Chat tab's buttons support Invoke, the Code tab's support
           Toggle; try both. */
        int pressed = -1;
        for (int k = 0; k < (int)(sizeof g_order / sizeof *g_order); k++) {
            if (!(btn = find_button(root, g_order[k]))) continue;
            focus_window(wl.list[i]);
            const char *how = "invoked";
            BOOL done = invoke(btn);
            if (!done) { how = "toggled"; done = toggle(btn); }
            logf_("'%ls' -> %s", g_order[k], done ? how : "FAILED (no Invoke/Toggle)");
            IUIAutomationElement_Release(btn);
            if (done) pressed = k;
            break;
        }
        IUIAutomationElement_Release(root);
        if (pressed >= 0) return pressed;
    }
    return -1;
}

static int on_hotkey(void)
{
    BOOL any_window = FALSE;
    logf_("hotkey pressed");
    for (int i = 0; i < FIND_RETRIES; i++) {
        int pressed = try_once(&any_window);
        if (pressed >= 0) return pressed;
        if (!any_window) break;
        Sleep(FIND_RETRY_MS);
    }
    logf_(any_window ? "no dictation button found" : "Claude window not found");
    MessageBeep(MB_ICONWARNING);
    return -1;
}

/* TRUE if any Claude window currently shows a stop-dictation button. */
static BOOL is_recording(void)
{
    WindowList wl = {0};
    EnumWindows(enum_proc, (LPARAM)&wl);
    for (int i = 0; i < wl.count; i++) {
        IUIAutomationElement *root = NULL, *btn = NULL;
        if (FAILED(IUIAutomation_ElementFromHandle(g_uia, wl.list[i], &root)) || !root)
            continue;
        for (int k = 0; k < N_STOP_BUTTONS && !btn; k++)
            btn = find_button(root, g_order[k]);
        IUIAutomationElement_Release(root);
        if (btn) { IUIAutomationElement_Release(btn); return TRUE; }
    }
    return FALSE;
}

/* ---------------- Shortcut: names, storage, registration ---------------- */

static UINT mod_of_vk(UINT vk)
{
    switch (vk) {
    case VK_LWIN: case VK_RWIN: return MOD_WIN;
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: return MOD_CONTROL;
    case VK_MENU: case VK_LMENU: case VK_RMENU: return MOD_ALT;
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: return MOD_SHIFT;
    }
    return 0;
}

static void key_name(UINT vk, wchar_t *out, int n)
{
    UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    BOOL ext = FALSE;
    switch (vk) {   /* keys that share scan codes with the numpad */
    case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END: case VK_PRIOR:
    case VK_NEXT: case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
    case VK_DIVIDE: case VK_NUMLOCK: case VK_SNAPSHOT:
        ext = TRUE;
    }
    if (!scan || !GetKeyNameTextW((LONG)((scan << 16) | (ext << 24)), out, n))
        swprintf(out, n, L"Key %u", vk);
}

/* Splits a shortcut into display parts ("Win", "Ctrl", ..., key). vk may be 0
   (modifiers only). Returns the number of parts. */
static int combo_parts(UINT mods, UINT vk, wchar_t parts[5][32])
{
    int n = 0;
    if (mods & MOD_WIN)     wcscpy(parts[n++], L"Win");
    if (mods & MOD_CONTROL) wcscpy(parts[n++], L"Ctrl");
    if (mods & MOD_ALT)     wcscpy(parts[n++], L"Alt");
    if (mods & MOD_SHIFT)   wcscpy(parts[n++], L"Shift");
    if (vk) key_name(vk, parts[n++], 32);
    return n;
}

static void format_combo(UINT mods, UINT vk, wchar_t *out, size_t n)
{
    wchar_t parts[5][32];
    int count = combo_parts(mods, vk, parts);
    out[0] = 0;
    for (int i = 0; i < count; i++) {
        if (i) wcsncat(out, L" + ", n - wcslen(out) - 1);
        wcsncat(out, parts[i], n - wcslen(out) - 1);
    }
}

/* A usable shortcut needs a modifier, except for F1-F24 on their own. */
static BOOL combo_valid(UINT mods, UINT vk)
{
    return vk && (mods || (vk >= VK_F1 && vk <= VK_F24));
}

static void load_shortcut(void)
{
    DWORD mods, vk, size = sizeof(DWORD);
    if (RegGetValueW(HKEY_CURRENT_USER, REG_KEY, L"HotkeyMods", RRF_RT_REG_DWORD, NULL, &mods, &size) == ERROR_SUCCESS &&
        (size = sizeof(DWORD), RegGetValueW(HKEY_CURRENT_USER, REG_KEY, L"HotkeyVk", RRF_RT_REG_DWORD, NULL, &vk, &size)) == ERROR_SUCCESS &&
        combo_valid(mods, vk)) {
        g_hk_mods = mods;
        g_hk_vk = vk;
    }
}

static void save_shortcut(void)
{
    DWORD mods = g_hk_mods, vk = g_hk_vk;
    RegSetKeyValueW(HKEY_CURRENT_USER, REG_KEY, L"HotkeyMods", REG_DWORD, &mods, sizeof mods);
    RegSetKeyValueW(HKEY_CURRENT_USER, REG_KEY, L"HotkeyVk", REG_DWORD, &vk, sizeof vk);
}

/* ---------------- Keyboard hook (own thread) ----------------
 * A low-level hook instead of RegisterHotKey, so the shortcut can take over
 * combinations Windows reserves (Win+J etc.). It runs on its own thread so the
 * keyboard never waits on the main thread's UI Automation work. */

static HHOOK g_hook;

static UINT held_mods(void)
{
    UINT m = 0;
    if ((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000) m |= MOD_WIN;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) m |= MOD_CONTROL;
    if (GetAsyncKeyState(VK_MENU) & 0x8000)    m |= MOD_ALT;
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000)   m |= MOD_SHIFT;
    return m;
}

static void send_mask_key(void)
{
    INPUT in[2] = {0};
    in[0].type = in[1].type = INPUT_KEYBOARD;
    in[0].ki.wVk = in[1].ki.wVk = MASK_VK;
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}

/* While the shortcut window is in front, every key goes to it (modifiers are
   tracked here, since swallowed keys don't update the system key state). */
static HWND g_hook_dlg;   /* window the hook's held-key state belongs to */

static BOOL hook_dialog(HWND dlg, UINT vk, BOOL down)
{
    static UINT s_held, s_last;
    if (g_hook_dlg != dlg) { g_hook_dlg = dlg; s_held = 0; s_last = 0; }

    UINT m = mod_of_vk(vk);
    if (m) {
        UINT before = s_held;
        if (down) s_held |= m; else s_held &= ~m;
        if (s_held != before) PostMessageW(dlg, WM_DLG_MODS, s_held, 0);
        return TRUE;
    }
    if (!down) { if (vk == s_last) s_last = 0; return TRUE; }
    if (vk == s_last) return TRUE;               /* auto-repeat */
    s_last = vk;
    if (!s_held && vk == VK_RETURN)      PostMessageW(dlg, WM_COMMAND, IDC_SAVE, 0);
    else if (!s_held && vk == VK_ESCAPE) PostMessageW(dlg, WM_COMMAND, IDC_CANCEL, 0);
    else                                 PostMessageW(dlg, WM_DLG_KEY, vk, s_held);
    return TRUE;
}

static LRESULT CALLBACK kb_hook(int code, WPARAM wp, LPARAM lp)
{
    static UINT s_down;   /* shortcut key currently held down (swallowed) */
    if (code == HC_ACTION) {
        KBDLLHOOKSTRUCT *k = (KBDLLHOOKSTRUCT *)lp;
        UINT vk = k->vkCode;
        BOOL down = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN;
        HWND dlg = g_dlg;
        BOOL in_dlg = dlg && GetForegroundWindow() == dlg;
        if (!in_dlg) g_hook_dlg = NULL;   /* start fresh when it's in front again */

        if (vk == MASK_VK) {
            /* our own injected key: let it through */
        } else if (in_dlg) {
            if (hook_dialog(dlg, vk, down)) return 1;
        } else if (down) {
            if (vk == s_down) return 1;               /* auto-repeat */
            if (vk == g_hk_vk && !mod_of_vk(vk) && held_mods() == g_hk_mods) {
                s_down = vk;
                if (g_hk_mods & (MOD_WIN | MOD_ALT)) send_mask_key();
                PostMessageW(g_main, WM_SHORTCUT_HIT, 0, 0);
                return 1;
            }
        } else if (vk == s_down) {
            s_down = 0;
            return 1;
        }
    }
    return CallNextHookEx(g_hook, code, wp, lp);
}

static DWORD WINAPI hook_thread(LPVOID arg)
{
    (void)arg;
    g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, kb_hook, GetModuleHandleW(NULL), 0);
    if (!g_hook) logf_("SetWindowsHookEx failed: %lu", GetLastError());
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {}
    if (g_hook) UnhookWindowsHookEx(g_hook);
    return 0;
}

/* ---------------- Tray icon state ---------------- */

static HICON g_icon_idle, g_icon_rec;
static BOOL g_recording;
static int g_misses;

static void update_tray_tip(void)
{
    if (g_recording) {
        wcscpy(g_nid.szTip, L"Claude Dictate - recording");
    } else {
        wchar_t combo[64];
        format_combo(g_hk_mods, g_hk_vk, combo, 64);
        swprintf(g_nid.szTip, 128, L"Claude Dictate (%ls)", combo);
    }
}

static void set_recording(HWND hwnd, BOOL on)
{
    g_misses = 0;
    if (on == g_recording) return;
    g_recording = on;
    g_nid.hIcon = on ? g_icon_rec : g_icon_idle;
    update_tray_tip();
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    /* While recording, watch for it ending by other means (Enter, a click). */
    if (on) SetTimer(hwnd, POLL_TIMER_ID, POLL_MS, NULL);
    else    KillTimer(hwnd, POLL_TIMER_ID);
    logf_("tray: %s", on ? "recording" : "idle");
}

/* Touch the accessibility tree once so Chromium builds it before first use. */
static void warm_up(void)
{
    WindowList wl = {0};
    EnumWindows(enum_proc, (LPARAM)&wl);
    for (int i = 0; i < wl.count; i++) {
        IUIAutomationElement *root = NULL, *btn;
        if (FAILED(IUIAutomation_ElementFromHandle(g_uia, wl.list[i], &root)) || !root)
            continue;
        if ((btn = find_button(root, BTN_DICTATE))) IUIAutomationElement_Release(btn);
        IUIAutomationElement_Release(root);
    }
    logf_("warm-up: %d Claude window(s)", wl.count);
}

/* ---------------- "Change shortcut" window ---------------- */

static UINT g_dpi = 96;
static HFONT g_font, g_font_key;
static UINT g_new_mods, g_new_vk;     /* shortcut shown in the window */
static UINT g_held;                   /* modifiers currently held */
static BOOL g_building;               /* modifiers held, main key not yet pressed */
static const wchar_t *g_status;
static BOOL g_status_error;

#define S(x) MulDiv((x), (int)g_dpi, 96)

/* Dark palette */
#define C_BG           RGB(32, 32, 32)
#define C_TEXT         RGB(232, 232, 232)
#define C_MUTED        RGB(150, 150, 150)
#define C_ERROR        RGB(255, 120, 110)
#define C_ACCENT       RGB(217, 119, 87)
#define C_ACCENT_DOWN  RGB(186, 98, 70)
#define C_KEY_BORDER   RGB(110, 110, 110)
#define C_BTN          RGB(58, 58, 58)
#define C_BTN_DOWN     RGB(78, 78, 78)

static DWORD argb(COLORREF c)
{
    return 0xFF000000u | ((DWORD)GetRValue(c) << 16) | ((DWORD)GetGValue(c) << 8) | GetBValue(c);
}

/* Anti-aliased rounded rectangle; border is drawn inside r. border_w 0 = fill only. */
static void rounded_rect(HDC dc, RECT r, int radius, COLORREF fill, COLORREF border, float border_w)
{
    void *g = NULL, *path = NULL, *brush = NULL, *pen = NULL;
    if (GdipCreateFromHDC(dc, &g) != 0) return;
    GdipSetSmoothingMode(g, GP_SMOOTH_ANTIALIAS);

    float h = border_w / 2, d = 2.0f * radius;
    float x0 = r.left + h, y0 = r.top + h, x1 = r.right - 1 - h, y1 = r.bottom - 1 - h;
    GdipCreatePath(0, &path);
    GdipAddPathArc(path, x0, y0, d, d, 180, 90);
    GdipAddPathArc(path, x1 - d, y0, d, d, 270, 90);
    GdipAddPathArc(path, x1 - d, y1 - d, d, d, 0, 90);
    GdipAddPathArc(path, x0, y1 - d, d, d, 90, 90);
    GdipClosePathFigure(path);

    GdipCreateSolidFill(argb(fill), &brush);
    GdipFillPath(g, brush, path);
    if (border_w > 0) {
        GdipCreatePen1(argb(border), border_w, GP_UNIT_PIXEL, &pen);
        GdipDrawPath(g, pen, path);
        GdipDeletePen(pen);
    }
    GdipDeleteBrush(brush);
    GdipDeletePath(path);
    GdipDeleteGraphics(g);
}

static void set_status(const wchar_t *text, BOOL error)
{
    g_status = text;
    g_status_error = error;
    InvalidateRect(g_dlg, NULL, FALSE);
}

static void capture_key(UINT mods, UINT vk)
{
    g_new_mods = mods;
    g_new_vk = vk;
    g_building = FALSE;
    if (!combo_valid(g_new_mods, g_new_vk))
        set_status(L"Add Ctrl, Alt, Shift or Win to the key.", TRUE);
    else if (g_new_mods == g_hk_mods && g_new_vk == g_hk_vk)
        set_status(L"That's the current shortcut.", FALSE);
    else
        set_status(L"Press Enter or click Save to use this shortcut.", FALSE);
}

/* Modifier state from the hook thread (WM_DLG_MODS). */
static void on_held_mods(UINT held)
{
    if (held & ~g_held) g_building = TRUE;     /* a modifier was added */
    if (!held) g_building = FALSE;
    g_held = held;
    InvalidateRect(g_dlg, NULL, FALSE);
}

static void paint_keycaps(HDC dc, RECT area, UINT mods, UINT vk, BOOL placeholder, BOOL highlight)
{
    wchar_t parts[6][32];
    int n = combo_parts(mods, vk, parts);
    if (placeholder) wcscpy(parts[n++], L"...");
    if (!n) return;

    SelectObject(dc, g_font_key);
    int pad = S(20), gap = S(12), cap_h = S(49);
    SIZE plus;
    GetTextExtentPoint32W(dc, L"+", 1, &plus);
    int widths[6], total = 0;
    for (int i = 0; i < n; i++) {
        SIZE sz;
        GetTextExtentPoint32W(dc, parts[i], (int)wcslen(parts[i]), &sz);
        widths[i] = max(sz.cx + 2 * pad, cap_h);
        total += widths[i] + (i ? 2 * gap + plus.cx : 0);
    }

    int x = (area.left + area.right - total) / 2;
    int y = (area.top + area.bottom - cap_h) / 2;
    SetBkMode(dc, TRANSPARENT);
    for (int i = 0; i < n; i++) {
        if (i) {
            SetTextColor(dc, C_MUTED);
            RECT pr = {x, y, x + 2 * gap + plus.cx, y + cap_h};
            DrawTextW(dc, L"+", 1, &pr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            x = pr.right;
        }
        RECT cap = {x, y, x + widths[i], y + cap_h};
        rounded_rect(dc, cap, S(10), C_BG, highlight ? C_ACCENT : C_KEY_BORDER, (float)S(4));
        SetTextColor(dc, C_TEXT);
        DrawTextW(dc, parts[i], -1, &cap, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        x += widths[i];
    }
}

/* Owner-drawn dark buttons; Save uses the accent colour. */
static void draw_button(const DRAWITEMSTRUCT *di)
{
    BOOL primary = di->CtlID == IDC_SAVE;
    BOOL pressed = (di->itemState & ODS_SELECTED) != 0;
    COLORREF fill = primary ? (pressed ? C_ACCENT_DOWN : C_ACCENT)
                            : (pressed ? C_BTN_DOWN : C_BTN);
    HBRUSH bg = CreateSolidBrush(C_BG);
    FillRect(di->hDC, &di->rcItem, bg);
    DeleteObject(bg);
    rounded_rect(di->hDC, di->rcItem, S(6), fill, fill, 0);

    wchar_t text[64];
    GetWindowTextW(di->hwndItem, text, 64);
    SelectObject(di->hDC, g_font);
    SetBkMode(di->hDC, TRANSPARENT);
    SetTextColor(di->hDC, primary ? RGB(255, 255, 255) : C_TEXT);
    RECT r = di->rcItem;
    DrawTextW(di->hDC, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static void paint_dialog(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);

    /* double-buffered to avoid flicker while keys are pressed */
    HDC dc = CreateCompatibleDC(wdc);
    HBITMAP bmp = CreateCompatibleBitmap(wdc, rc.right, rc.bottom);
    HGDIOBJ old_bmp = SelectObject(dc, bmp);
    HBRUSH bg = CreateSolidBrush(C_BG);
    FillRect(dc, &rc, bg);
    DeleteObject(bg);
    SetBkMode(dc, TRANSPARENT);

    SelectObject(dc, g_font);
    SetTextColor(dc, C_TEXT);
    RECT t = {S(20), S(16), rc.right - S(20), S(40)};
    DrawTextW(dc, L"Press the shortcut that starts and stops dictation.", -1, &t,
              DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

    RECT keys = {0, S(48), rc.right, S(120)};
    BOOL changed = g_new_mods != g_hk_mods || g_new_vk != g_hk_vk;
    if (g_building)
        paint_keycaps(dc, keys, g_held, 0, TRUE, FALSE);
    else
        paint_keycaps(dc, keys, g_new_mods, g_new_vk, FALSE,
                      changed && combo_valid(g_new_mods, g_new_vk));

    SelectObject(dc, g_font);
    SetTextColor(dc, g_status_error ? C_ERROR : C_MUTED);
    RECT st = {S(20), S(126), rc.right - S(20), S(150)};
    DrawTextW(dc, g_status ? g_status : L"", -1, &st, DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    BitBlt(wdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old_bmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    EndPaint(hwnd, &ps);
}

static void on_save(void)
{
    if (!combo_valid(g_new_mods, g_new_vk)) {
        set_status(L"Add Ctrl, Alt, Shift or Win to the key.", TRUE);
        return;
    }
    g_hk_mods = g_new_mods;
    g_hk_vk = g_new_vk;
    save_shortcut();
    update_tray_tip();
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    wchar_t combo[64];
    format_combo(g_hk_mods, g_hk_vk, combo, 64);
    logf_("shortcut changed to %ls", combo);
    DestroyWindow(g_dlg);
}

static LRESULT CALLBACK dlg_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT:
        paint_dialog(hwnd);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_DRAWITEM:
        draw_button((const DRAWITEMSTRUCT *)lp);
        return TRUE;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) on_held_mods(0);
        return 0;
    case WM_DLG_MODS:
        on_held_mods((UINT)wp);
        return 0;
    case WM_DLG_KEY:
        capture_key((UINT)lp, (UINT)wp);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_SAVE:   on_save(); break;
        case IDC_CANCEL: DestroyWindow(hwnd); break;
        case IDC_RESET:  capture_key(DEFAULT_MODS, DEFAULT_VK); break;
        }
        return 0;
    case WM_DESTROY:
        g_dlg = NULL;
        DeleteObject(g_font);
        DeleteObject(g_font_key);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void open_shortcut_dialog(HINSTANCE inst)
{
    logf_("shortcut window: open");
    if (g_dlg) { SetForegroundWindow(g_dlg); return; }

    static BOOL registered;
    if (!registered) {
        WNDCLASSW wc = {0};
        wc.lpfnWndProc = dlg_proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.hIcon = g_icon_idle;
        wc.lpszClassName = L"ClaudeDictateShortcut";
        registered = RegisterClassW(&wc) != 0;
    }

    g_dpi = GetDpiForSystem();
    g_font = CreateFontW(-S(14), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                         CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_font_key = CreateFontW(-S(17), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                             CLEARTYPE_QUALITY, 0, L"Segoe UI");

    /* WS_CLIPCHILDREN: repainting the keycaps must not draw over the buttons. */
    DWORD style = WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN, exstyle = WS_EX_TOPMOST;
    RECT r = {0, 0, S(460), S(210)};
    AdjustWindowRectExForDpi(&r, style, FALSE, exstyle, g_dpi);
    int w = r.right - r.left, h = r.bottom - r.top;
    g_dlg = CreateWindowExW(exstyle, L"ClaudeDictateShortcut", L"Claude Dictate - Shortcut", style,
                            (GetSystemMetrics(SM_CXSCREEN) - w) / 2,
                            (GetSystemMetrics(SM_CYSCREEN) - h) / 2,
                            w, h, NULL, NULL, inst, NULL);
    if (!g_dlg) {
        logf_("shortcut window: CreateWindow failed: %lu", GetLastError());
        DeleteObject(g_font);
        DeleteObject(g_font_key);
        return;
    }

    int bw = S(96), bh = S(30), by = S(164);
    HWND b;
    b = CreateWindowW(L"BUTTON", L"Reset to Win+Y", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                      S(20), by, S(120), bh, g_dlg, (HMENU)IDC_RESET, inst, NULL);
    SendMessageW(b, WM_SETFONT, (WPARAM)g_font, TRUE);
    b = CreateWindowW(L"BUTTON", L"Save", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                      S(460) - S(20) - 2 * bw - S(8), by, bw, bh, g_dlg, (HMENU)IDC_SAVE, inst, NULL);
    SendMessageW(b, WM_SETFONT, (WPARAM)g_font, TRUE);
    b = CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                      S(460) - S(20) - bw, by, bw, bh, g_dlg, (HMENU)IDC_CANCEL, inst, NULL);
    SendMessageW(b, WM_SETFONT, (WPARAM)g_font, TRUE);

    /* Start from the current shortcut. While this window is in front, the hook
       sends every key here instead of triggering dictation. */
    g_new_mods = g_hk_mods;
    g_new_vk = g_hk_vk;
    g_held = 0;
    g_building = FALSE;
    g_status = L"Current shortcut. Press a new one to change it.";
    g_status_error = FALSE;

    BOOL dark = TRUE;   /* dark title bar (DWMWA_USE_IMMERSIVE_DARK_MODE) */
    DwmSetWindowAttribute(g_dlg, 20, &dark, sizeof dark);
    ShowWindow(g_dlg, SW_SHOW);
    SetForegroundWindow(g_dlg);
}

/* ---------------- Window / tray ---------------- */

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SHORTCUT_HIT: {
        int pressed = on_hotkey();
        if (pressed >= 0) set_recording(hwnd, pressed >= N_STOP_BUTTONS);
        /* Drop presses that queued up while we were busy. */
        MSG m;
        while (PeekMessageW(&m, hwnd, WM_SHORTCUT_HIT, WM_SHORTCUT_HIT, PM_REMOVE)) {}
        return 0;
    }
    case WM_TIMER:
        /* A few consecutive misses, since the stop button can take a moment
           to appear after starting. */
        if (wp == POLL_TIMER_ID) {
            if (is_recording()) g_misses = 0;
            else if (++g_misses >= POLL_MISSES) set_recording(hwnd, FALSE);
        }
        return 0;
    case WM_TRAY:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) {
            POINT pt;
            GetCursorPos(&pt);
            wchar_t combo[64], label[96];
            format_combo(g_hk_mods, g_hk_vk, combo, 64);
            swprintf(label, 96, L"Shortcut: %ls", combo);
            HMENU m = CreatePopupMenu();
            AppendMenuW(m, MF_STRING | MF_GRAYED, 0, label);
            AppendMenuW(m, MF_STRING, IDM_SHORTCUT, L"Change shortcut...");
            AppendMenuW(m, MF_SEPARATOR, 0, NULL);
            AppendMenuW(m, MF_STRING, IDM_EXIT, L"Exit");
            SetForegroundWindow(hwnd);
            int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
            DestroyMenu(m);
            if (cmd) SendMessageW(hwnd, WM_COMMAND, cmd, 0);
        } else if (LOWORD(lp) == WM_LBUTTONUP) {
            /* A click on the icon works like the shortcut. */
            PostMessageW(hwnd, WM_SHORTCUT_HIT, 0, 0);
        }
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == IDM_SHORTCUT) open_shortcut_dialog(GetModuleHandleW(NULL));
        if (LOWORD(wp) == IDM_EXIT) DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (g_dlg) DestroyWindow(g_dlg);
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    (void)prev; (void)cmd; (void)show;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    HANDLE mutex = CreateMutexW(NULL, TRUE, L"ClaudeDictateSingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(NULL, L"Claude Dictate is already running.", L"Claude Dictate", MB_ICONINFORMATION);
        return 0;
    }

    open_log();
    ULONG_PTR gdip_token;
    GdiplusStartupInput gdip_in = {1, NULL, FALSE, FALSE};
    GdiplusStartup(&gdip_token, &gdip_in, NULL);
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(CoCreateInstance(&my_CLSID_CUIAutomation, NULL, CLSCTX_INPROC_SERVER,
                                &my_IID_IUIAutomation, (void **)&g_uia))) {
        MessageBoxW(NULL, L"Could not create UI Automation.", L"Claude Dictate", MB_ICONERROR);
        return 1;
    }

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.lpszClassName = L"ClaudeDictateWnd";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"Claude Dictate", 0, 0, 0, 0, 0,
                              HWND_MESSAGE, NULL, inst, NULL);
    g_main = hwnd;
    load_shortcut();
    wchar_t combo[64];
    format_combo(g_hk_mods, g_hk_vk, combo, 64);

    g_nid.cbSize = sizeof g_nid;
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    g_nid.uCallbackMessage = WM_TRAY;
    /* Icon resources 1 and 2 (claude_dictate.rc), at the tray's size for this DPI. */
    int cx = GetSystemMetrics(SM_CXSMICON), cy = GetSystemMetrics(SM_CYSMICON);
    g_icon_idle = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, cx, cy, 0);
    g_icon_rec  = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(2), IMAGE_ICON, cx, cy, 0);
    if (!g_icon_idle) g_icon_idle = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    if (!g_icon_rec) g_icon_rec = g_icon_idle;
    g_nid.hIcon = g_icon_idle;
    update_tray_tip();
    Shell_NotifyIconW(NIM_ADD, &g_nid);

    DWORD hook_tid;
    HANDLE hook = CreateThread(NULL, 0, hook_thread, NULL, 0, &hook_tid);
    logf_("started, shortcut %ls", combo);
    warm_up();

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    PostThreadMessageW(hook_tid, WM_QUIT, 0, 0);
    WaitForSingleObject(hook, 2000);
    IUIAutomation_Release(g_uia);
    CoUninitialize();
    if (g_log) fclose(g_log);
    ReleaseMutex(mutex);
    return 0;
}
