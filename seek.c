/* Seek - a Spotlight-style launcher for Windows 10. Alt+Space to open.
   One C file, no dependencies beyond Win32. Everything is indexed into RAM, so each
   keystroke is a scan of memory, not a disk query. The index stores a tree (each entry
   holds its name and its parent folder) rather than full paths, and a watcher thread
   notes when files change, so the disk is only walked again when something did. */
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commctrl.h>
#include <math.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <wchar.h>
#include <wctype.h>
#pragma comment(lib, "user32")
#pragma comment(lib, "gdi32")
#pragma comment(lib, "shell32")
#pragma comment(lib, "ole32")
#pragma comment(lib, "comctl32")
#pragma comment(lib, "advapi32")
#include "search.h"
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#define SEEK_VERSION L"1.1.0"
#define MAXROWS   8
#define WM_TRAY   (WM_APP + 2)
#define WM_SHOWME (WM_APP + 3)


static HWND g_wnd, g_edit;
static WNDPROC g_editProc;
static Row g_rows[MAXROWS + 2];
static int g_nrows, g_sel;
static wchar_t g_query[MAXQUERY], g_calc[64];
static HFONT g_fText, g_fSub, g_fGlyph, g_fIcon;
static HIMAGELIST g_sysil;
static int g_iconPx;
static float S = 1;
static COLORREF C_BG, C_FIELD, C_SEL, C_TEXT, C_SUB, C_BORDER;
static COLORREF C_ACCENT = RGB(0, 120, 215);
static HBRUSH g_fieldBrush;
static UINT g_taskbarCreated;
static NOTIFYICONDATAW g_nid;

#define PX(v) ((int)((v) * S + 0.5f))

/* Every size, colour, font and behaviour is a setting (seek.cfg, normally written by Silver
   Styler's `silver seek set ...`). The defaults are Seek's own look: Otto's search box and cards. */
typedef struct {
    int width, fieldH, rowH, gap, maxRows, radius, border, outline;
    int posX, posY, results;                /* window centre %, field top %; results 0 below, 1 above, 2 auto */
    int icons, subtitles, searchIcon, selBar, blur, opacity;
    float fontSize, subSize;
    long bg, field, text, sub, sel, borderC, accent; /* 0xRRGGBB, or -1 = from the light/dark theme */
    int launcher, lX, lY, lW, lH, lRadius, openAtLauncher; /* launcher 0 none, 1 bar, 2 dot */
    wchar_t font[64], placeholder[96], hotkey[32], lText[96];
} Style;
static Style st;
static int g_above, g_anchorY; /* results that open upwards keep the field's bottom edge fixed */
static HWND g_launch;
static COLORREF C_LFIELD;
#define W_WIN PX(st.width)
#define H_FIELD PX(st.fieldH)
#define H_TOP (H_FIELD + PX(4))
#define H_ROW PX(st.rowH)
#define GAP PX(st.gap)

/* ---------- search ---------- */

static void layout(void);

static void search(void) {
    wchar_t raw[MAXQUERY];
    int n = GetWindowTextW(g_edit, raw, MAXQUERY), s0 = 0;
    while (n && raw[n - 1] == ' ') raw[--n] = 0;
    while (raw[s0] == ' ') s0++;
    wcscpy(g_query, raw + s0);
    g_nrows = seekQuery(g_query, st.maxRows, g_rows, g_calc, 64);
    g_sel = 0;
    layout();
}

/* ---------- actions ---------- */

static void hide(void) { ShowWindow(g_wnd, SW_HIDE); }

static void copyText(const wchar_t *t) {
    size_t n = (wcslen(t) + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, n);
    void *p = h ? GlobalLock(h) : NULL;
    if (!p) { if (h) GlobalFree(h); return; }
    memcpy(p, t, n);
    GlobalUnlock(h);
    if (OpenClipboard(g_wnd)) {
        EmptyClipboard();
        if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
        CloseClipboard();
    } else GlobalFree(h);
}

static void webSearch(void) {
    static const wchar_t base[] = L"https://www.google.com/search?q=";
    char u8[MAXQUERY * 3 + 1];
    wchar_t url[sizeof base / sizeof *base + MAXQUERY * 9];
    int n = WideCharToMultiByte(CP_UTF8, 0, g_query, -1, u8, sizeof u8, NULL, NULL);
    wchar_t *o = url + swprintf(url, 64, L"%s", base);
    for (int i = 0; i < n - 1; i++) {
        unsigned char c = (unsigned char)u8[i];
        if (isalnum(c) || strchr("-_.~", c)) *o++ = c;
        else o += swprintf(o, 4, L"%%%02X", c);
    }
    *o = 0;
    ShellExecuteW(NULL, NULL, url, NULL, NULL, SW_SHOWNORMAL);
}

/* selects the item in Explorer without building a command line */
static void reveal(const wchar_t *path) {
    PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(path);
    if (pidl) { SHOpenFolderAndSelectItems(pidl, 0, NULL, 0); ILFree(pidl); }
}

static void activate(int r, int showInFolder) {
    if (r < 0 || r >= g_nrows) return;
    Row row = g_rows[r];
    hide();
    if (row.kind == K_CALC) { copyText(g_calc); return; }
    if (row.kind == K_WEB) { webSearch(); return; }
    static wchar_t path[MAXPATH];
    Entry *e = &g_ix->e[row.idx];
    if (pathOf(g_ix, row.idx, path, MAXPATH) < 0) return;
    if (e->uses < 255) e->uses++;
    bumpUsage(path);
    if (showInFolder && e->kind != K_SETTINGS) reveal(path);
    else ShellExecuteW(NULL, NULL, path, NULL, NULL, SW_SHOWNORMAL);
}

/* ---------- drawing ---------- */

/* the few rows on screen keep their icon here instead of a slot in every entry */
static struct { const Index *ix; unsigned idx; int icon; } g_icons[32];

static int iconOf(unsigned i) {
    int slot = i % 32;
    if (g_icons[slot].ix == g_ix && g_icons[slot].idx == i) return g_icons[slot].icon;
    const Entry *e = &g_ix->e[i];
    static wchar_t p[MAXPATH];
    int len = pathOf(g_ix, i, p, MAXPATH), icon = 0;
    if (len >= 0) {
        static const wchar_t *own[] = { L".exe", L".lnk", L".ico", L".url", L".appref-ms", L".msc", L".cpl", 0 };
        int real = 0;
        for (int k = 0; own[k]; k++) if (endsWith(p, len, own[k])) real = 1;
        UINT f = SHGFI_SYSICONINDEX | SHGFI_LARGEICON;
        DWORD attr = 0;
        if (e->kind == K_DIR) { f |= SHGFI_USEFILEATTRIBUTES; attr = FILE_ATTRIBUTE_DIRECTORY; }
        else if (!real) { f |= SHGFI_USEFILEATTRIBUTES; attr = FILE_ATTRIBUTE_NORMAL; }
        SHFILEINFOW sfi = { 0 };
        SHGetFileInfoW(p, attr, &sfi, sizeof sfi, f);
        icon = sfi.iIcon;
    }
    g_icons[slot].ix = g_ix; g_icons[slot].idx = i; g_icons[slot].icon = icon;
    return icon;
}

static int webIcon(void) {
    static int i = -1;
    if (i < 0) {
        SHFILEINFOW sfi = { 0 };
        SHGetFileInfoW(L".html", FILE_ATTRIBUTE_NORMAL, &sfi, sizeof sfi, SHGFI_SYSICONINDEX | SHGFI_LARGEICON | SHGFI_USEFILEATTRIBUTES);
        i = sfi.iIcon;
    }
    return i;
}

static void glyph(HDC dc, HFONT f, RECT *r, const wchar_t *g, COLORREF c) {
    SelectObject(dc, f); SetTextColor(dc, c);
    DrawTextW(dc, g, 1, r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

static void fill(HDC dc, int l, int t, int r, int b, COLORREF c) {
    RECT rc = { l, t, r, b };
    SetBkColor(dc, c);
    ExtTextOutW(dc, 0, 0, ETO_OPAQUE, &rc, NULL, 0, NULL);
}

/* Row 0 sits next to the field: below it normally, above it when results open upwards. */
static int rowY(int i, int h) {
    return g_above ? h - H_TOP - (i + 1) * (H_ROW + GAP) + GAP : H_TOP + i * (H_ROW + GAP);
}

static int fieldY(int h) { return g_above ? h - H_FIELD : 0; }

static void paint(HDC dc, const RECT *cr) {
    int b = PX(st.border), o = PX(st.outline), fy = fieldY(cr->bottom);
    fill(dc, 0, 0, cr->right, cr->bottom, b ? C_BORDER : C_BG);
    fill(dc, b, b, cr->right - b, cr->bottom - b, C_BG);
    /* the search field, with the focused outline in the accent colour */
    fill(dc, 0, fy, cr->right, fy + H_FIELD, o ? C_ACCENT : C_FIELD);
    fill(dc, o, fy + o, cr->right - o, fy + H_FIELD - o, C_FIELD);
    SetBkMode(dc, TRANSPARENT);
    if (st.searchIcon) {
        RECT gr = { PX(4), fy, PX(34), fy + H_FIELD };
        glyph(dc, g_fGlyph, &gr, L"\xE721", C_SUB);
    }
    static wchar_t buf[MAXPATH];
    for (int i = 0; i < g_nrows; i++) {
        const Row *row = &g_rows[i];
        int y = rowY(i, cr->bottom), l = PX(4), r = cr->right - PX(4);
        if (i == g_sel) {
            fill(dc, l, y, r, y + H_ROW, C_SEL);
            if (st.selBar) fill(dc, l, y, l + PX(3), y + H_ROW, C_ACCENT);
        }
        RECT ir = { l + PX(8), y, l + PX(8) + (st.icons ? g_iconPx : 0), y + H_ROW };
        int iy = y + (H_ROW - g_iconPx) / 2, nameLen = -1;
        const wchar_t *name, *sub;
        if (row->kind == K_CALC) {
            if (st.icons) glyph(dc, g_fIcon, &ir, L"\xE8EF", C_TEXT);
            name = g_calc; sub = L"Calculator \x2014 Enter to copy";
        } else if (row->kind == K_WEB) {
            if (st.icons) ImageList_Draw(g_sysil, webIcon(), dc, ir.left, iy, ILD_TRANSPARENT);
            swprintf(buf, MAXPATH, L"Search the web for \x201C%s\x201D", g_query);
            name = buf; sub = L"Opens in your browser";
        } else {
            const Entry *e = &g_ix->e[row->idx];
            if (!st.icons) {}
            else if (e->kind == K_SETTINGS) glyph(dc, g_fIcon, &ir, L"\xE713", C_TEXT);
            else ImageList_Draw(g_sysil, iconOf(row->idx), dc, ir.left, iy, ILD_TRANSPARENT);
            name = g_ix->s + e->name; nameLen = e->dispLen;
            sub = e->kind <= K_SETTINGS ? L"App" : pathOf(g_ix, e->parent, buf, MAXPATH) >= 0 ? buf : L"";
        }
        int tx = ir.right + PX(12), th = PX(st.fontSize * 1.5f), sh = PX(st.subSize * 1.5f);
        int ty = st.subtitles ? y + (H_ROW - th - sh) / 2 : y + (H_ROW - th) / 2;
        RECT tr = { tx, ty, r - PX(12), ty + th };
        SelectObject(dc, g_fText); SetTextColor(dc, C_TEXT);
        DrawTextW(dc, name, nameLen, &tr, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS | DT_VCENTER);
        if (!st.subtitles) continue;
        RECT sr = { tx, ty + th, r - PX(12), ty + th + sh };
        SelectObject(dc, g_fSub); SetTextColor(dc, C_SUB);
        DrawTextW(dc, sub, -1, &sr, DT_SINGLELINE | DT_NOPREFIX | DT_PATH_ELLIPSIS | DT_VCENTER);
    }
}

static void roundWindow(HWND h, int w, int ht, int r) {
    SetWindowRgn(h, r > 0 ? CreateRoundRectRgn(0, 0, w + 1, ht + 1, 2 * r, 2 * r) : NULL, TRUE);
}

static void layout(void) {
    int h = g_nrows ? H_TOP + g_nrows * (H_ROW + GAP) : H_FIELD;
    RECT wr; GetWindowRect(g_wnd, &wr);
    int y = g_above ? g_anchorY - h : wr.top;
    if (wr.bottom - wr.top != h || wr.right - wr.left != W_WIN || wr.top != y) {
        SetWindowPos(g_wnd, NULL, wr.left, y, W_WIN, h, SWP_NOZORDER | SWP_NOACTIVATE);
        roundWindow(g_wnd, W_WIN, h, PX(st.radius));
    }
    /* the edit box lives in the field, which moves when results open upwards */
    int eh = PX(st.fontSize * 1.5f), ex = st.searchIcon ? PX(36) : PX(12);
    SetWindowPos(g_edit, NULL, ex, fieldY(h) + (H_FIELD - eh) / 2, W_WIN - ex - PX(8), eh, SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(g_wnd, NULL, FALSE);
}

static void show(void) {
    POINT pt; GetCursorPos(&pt);
    MONITORINFO mi = { sizeof mi };
    int fromLauncher = g_launch && st.openAtLauncher && IsWindowVisible(g_launch);
    RECT lr = { 0 };
    if (fromLauncher) { GetWindowRect(g_launch, &lr); pt.x = lr.left; pt.y = lr.top; }
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
    RECT w = mi.rcWork;
    int x, y;
    if (fromLauncher) { /* open next to the launcher, towards the middle of the screen */
        x = (lr.left + lr.right - W_WIN) / 2;
        g_above = lr.top > (w.top + w.bottom) / 2;
        y = g_above ? lr.top - PX(8) - H_FIELD : lr.bottom + PX(8);
    } else {
        x = w.left + (w.right - w.left) * st.posX / 100 - W_WIN / 2;
        y = w.top + (w.bottom - w.top) * st.posY / 100;
        g_above = st.results == 1 || (st.results == 2 && st.posY > 50);
    }
    x = max(w.left, min(x, w.right - W_WIN));
    g_anchorY = y + H_FIELD;
    g_nrows = 0; /* layout() sizes the window from the field alone until the next search */
    SetWindowPos(g_wnd, HWND_TOPMOST, x, y, W_WIN, H_FIELD, SWP_SHOWWINDOW);
    layout();
    search();
    SetForegroundWindow(g_wnd);
    SetFocus(g_edit);
    SendMessageW(g_edit, EM_SETSEL, 0, -1);
    /* re-walk the disk only when the watcher saw a change; with no watcher, once a minute */
    DWORD age = GetTickCount() - g_lastScan;
    if (g_watching ? g_dirty && age > 2000 : age > 60 * 1000) rescan();
}

/* ---------- edit box ---------- */

static void moveSel(int d) {
    if (!g_nrows) return;
    g_sel = (g_sel + d + g_nrows) % g_nrows;
    InvalidateRect(g_wnd, NULL, FALSE);
}

static LRESULT CALLBACK editProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_KEYDOWN) {
        switch (wp) {
        case VK_DOWN: case VK_TAB: moveSel(GetKeyState(VK_SHIFT) < 0 && wp == VK_TAB ? -1 : 1); return 0;
        case VK_UP: moveSel(-1); return 0;
        case VK_RETURN: activate(g_sel, GetKeyState(VK_CONTROL) < 0); return 0;
        case VK_ESCAPE: if (GetWindowTextLengthW(h)) SetWindowTextW(h, L""); else hide(); return 0;
        }
    }
    if (msg == WM_CHAR) {
        if (wp == '\r' || wp == 27 || wp == '\t') return 0;
        if (wp == 0x7F) { /* Ctrl+Backspace: delete the selection, or the previous word */
            DWORD start, end;
            SendMessageW(h, EM_GETSEL, (WPARAM)&start, (LPARAM)&end);
            if (start == end) {
                wchar_t t[MAXQUERY];
                GetWindowTextW(h, t, MAXQUERY);
                while (start > 0 && t[start - 1] == ' ') start--;
                while (start > 0 && !wcschr(L" \\.", t[start - 1])) start--;
                SendMessageW(h, EM_SETSEL, start, end);
            }
            SendMessageW(h, EM_REPLACESEL, TRUE, (LPARAM)L"");
            return 0;
        }
    }
    return CallWindowProcW(g_editProc, h, msg, wp, lp);
}

/* ---------- tray / autostart ---------- */

static const wchar_t RUNKEY[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

static int autostart(int toggle) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUNKEY, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &k)) return 0;
    int on = RegQueryValueExW(k, L"Seek", NULL, NULL, NULL, NULL) == ERROR_SUCCESS;
    if (toggle) {
        wchar_t exe[MAX_PATH + 2] = L"\"";
        DWORD n = GetModuleFileNameW(NULL, exe + 1, MAX_PATH);
        if (on) on = RegDeleteValueW(k, L"Seek") != ERROR_SUCCESS;
        else if (n && n < MAX_PATH) {
            wcscat(exe, L"\"");
            on = RegSetValueExW(k, L"Seek", 0, REG_SZ, (const BYTE *)exe, (DWORD)(wcslen(exe) + 1) * sizeof(wchar_t)) == ERROR_SUCCESS;
        }
    }
    RegCloseKey(k);
    return on;
}

static void addTray(void) {
    g_nid.cbSize = sizeof g_nid;
    g_nid.hWnd = g_wnd;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    if (!g_nid.hIcon) ExtractIconExW(L"shell32.dll", 22, NULL, &g_nid.hIcon, 1);
    wcscpy(g_nid.szTip, L"Seek " SEEK_VERSION L" \x2014 Alt+Space");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void trayMenu(void) {
    enum { CMD_OPEN = 1, CMD_AUTOSTART, CMD_REINDEX, CMD_EXIT };
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"Seek " SEEK_VERSION);
    AppendMenuW(m, MF_STRING, CMD_OPEN, L"Open\tAlt+Space");
    AppendMenuW(m, MF_STRING | (autostart(0) ? MF_CHECKED : 0), CMD_AUTOSTART, L"Start with Windows");
    AppendMenuW(m, MF_STRING | (g_scanning ? MF_GRAYED : 0), CMD_REINDEX, g_scanning ? L"Indexing\x2026" : L"Re-index now");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, CMD_EXIT, L"Exit");
    POINT pt; GetCursorPos(&pt);
    SetForegroundWindow(g_wnd);
    int c = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_wnd, NULL);
    DestroyMenu(m);
    if (c == CMD_OPEN) show();
    else if (c == CMD_AUTOSTART) autostart(1);
    else if (c == CMD_REINDEX) rescan();
    else if (c == CMD_EXIT) DestroyWindow(g_wnd);
}

/* ---------- theme ---------- */

/* live blur behind the window, same call and tint as Otto's panel (Win32.cs Acrylic) */
typedef struct { int state, flags; DWORD color; int anim; } AccentPolicy;
typedef struct { int attr; void *data; SIZE_T size; } WinCompAttrData;

static int acrylic(HWND h, int on) {
    typedef BOOL(WINAPI *SWCA)(HWND, WinCompAttrData *);
    SWCA f = (SWCA)(void *)GetProcAddress(GetModuleHandleW(L"user32"), "SetWindowCompositionAttribute");
    AccentPolicy a = { on ? 4 /* ACRYLICBLURBEHIND */ : 0 /* DISABLED */, 2, 0xEB1A1A1A, 0 };
    WinCompAttrData d = { 19 /* WCA_ACCENT_POLICY */, &a, sizeof a };
    return f && f(h, &d);
}

/* Settings. Seek works on its own; if %APPDATA%\SilverStyler exists, theme.cfg and seek.cfg
   there set any of the Style fields, and the "SilverStylerReload" broadcast applies them live. */
static int g_cfgTheme; /* 0 = follow Windows, 1 = light, 2 = dark */
static UINT g_reloadMsg;

static void styleDefaults(void) {
    Style d = { 640, 44, 54, 4, MAXROWS, 0, 1, 1, 50, 25, 0, 1, 1, 1, 1, 1, 100, 13.33f, 11.33f,
                -1, -1, -1, -1, -1, -1, -1, 0, 50, 96, 300, 40, 20, 1 };
    st = d;
    wcscpy(st.font, L"Segoe UI");
    wcscpy(st.placeholder, L"Search apps and files");
    wcscpy(st.hotkey, L"alt+space");
    wcscpy(st.lText, L"Type here to search");
}

static long hexOrAuto(const wchar_t *v) {
    if (!*v || !_wcsicmp(v, L"auto")) return -1;
    return (long)(wcstoul(v[0] == L'#' ? v + 1 : v, NULL, 16) & 0xFFFFFF);
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void readStylerCfg(const wchar_t *name) {
    wchar_t path[MAX_PATH], line[512];
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, path))) return;
    wcscat_s(path, MAX_PATH, L"\\SilverStyler\\");
    wcscat_s(path, MAX_PATH, name);
    FILE *f = _wfopen(path, L"r, ccs=UTF-8");
    if (!f) return;
    while (fgetws(line, 512, f)) {
        wchar_t *eq = wcschr(line, L'='), *k = line, *v;
        if (!eq || line[0] == L'#') continue;
        *eq = 0; v = eq + 1;
        while (*v == L' ') v++;
        v[wcscspn(v, L"\r\n")] = 0;
        int n = _wtoi(v), on = n != 0 || !_wcsicmp(v, L"on") || !_wcsicmp(v, L"true");
        /* numbers */
        static const struct { const wchar_t *k; size_t off; int lo, hi; } nums[] = {
            { L"width", offsetof(Style, width), 160, 4000 }, { L"field_height", offsetof(Style, fieldH), 20, 200 },
            { L"row_height", offsetof(Style, rowH), 20, 200 }, { L"row_gap", offsetof(Style, gap), 0, 40 },
            { L"max_rows", offsetof(Style, maxRows), 2, MAXROWS }, { L"radius", offsetof(Style, radius), 0, 100 },
            { L"border", offsetof(Style, border), 0, 20 }, { L"outline", offsetof(Style, outline), 0, 10 },
            { L"pos_x", offsetof(Style, posX), 0, 100 }, { L"pos_y", offsetof(Style, posY), 0, 100 },
            { L"opacity", offsetof(Style, opacity), 20, 100 }, { L"launcher_x", offsetof(Style, lX), 0, 100 },
            { L"launcher_y", offsetof(Style, lY), 0, 100 }, { L"launcher_width", offsetof(Style, lW), 40, 4000 },
            { L"launcher_height", offsetof(Style, lH), 12, 400 }, { L"launcher_radius", offsetof(Style, lRadius), 0, 200 } };
        static const struct { const wchar_t *k; size_t off; } bools[] = {
            { L"icons", offsetof(Style, icons) }, { L"subtitles", offsetof(Style, subtitles) },
            { L"search_icon", offsetof(Style, searchIcon) }, { L"selection_bar", offsetof(Style, selBar) },
            { L"blur", offsetof(Style, blur) }, { L"open_at_launcher", offsetof(Style, openAtLauncher) } };
        static const struct { const wchar_t *k; size_t off; } colors[] = {
            { L"bg", offsetof(Style, bg) }, { L"field", offsetof(Style, field) }, { L"text", offsetof(Style, text) },
            { L"subtext", offsetof(Style, sub) }, { L"selection", offsetof(Style, sel) },
            { L"border_color", offsetof(Style, borderC) }, { L"accent", offsetof(Style, accent) } };
        static const struct { const wchar_t *k; size_t off; size_t cap; } strs[] = {
            { L"font", offsetof(Style, font), 64 }, { L"placeholder", offsetof(Style, placeholder), 96 },
            { L"hotkey", offsetof(Style, hotkey), 32 }, { L"launcher_text", offsetof(Style, lText), 96 } };
        char *base = (char *)&st;
        int done = 0;
        for (int i = 0; !done && i < (int)(sizeof nums / sizeof *nums); i++)
            if (!_wcsicmp(k, nums[i].k)) { *(int *)(base + nums[i].off) = clampi(n, nums[i].lo, nums[i].hi); done = 1; }
        for (int i = 0; !done && i < (int)(sizeof bools / sizeof *bools); i++)
            if (!_wcsicmp(k, bools[i].k)) { *(int *)(base + bools[i].off) = on; done = 1; }
        for (int i = 0; !done && i < (int)(sizeof colors / sizeof *colors); i++)
            if (!_wcsicmp(k, colors[i].k)) { *(long *)(base + colors[i].off) = hexOrAuto(v); done = 1; }
        for (int i = 0; !done && i < (int)(sizeof strs / sizeof *strs); i++)
            if (!_wcsicmp(k, strs[i].k)) { lstrcpynW((wchar_t *)(base + strs[i].off), v, (int)strs[i].cap); done = 1; }
        if (done) continue;
        if (!_wcsicmp(k, L"theme")) g_cfgTheme = !_wcsicmp(v, L"light") ? 1 : !_wcsicmp(v, L"dark") ? 2 : g_cfgTheme;
        else if (!_wcsicmp(k, L"font_size")) st.fontSize = (float)clampi(n, 6, 72);
        else if (!_wcsicmp(k, L"subtitle_size")) st.subSize = (float)clampi(n, 6, 72);
        else if (!_wcsicmp(k, L"results")) st.results = !_wcsicmp(v, L"above") ? 1 : !_wcsicmp(v, L"auto") ? 2 : 0;
        else if (!_wcsicmp(k, L"launcher")) st.launcher = !_wcsicmp(v, L"bar") ? 1 : !_wcsicmp(v, L"dot") ? 2 : 0;
    }
    fclose(f);
}

static void loadStylerCfg(void) {
    styleDefaults();
    g_cfgTheme = 0;
    readStylerCfg(L"theme.cfg");
    readStylerCfg(L"seek.cfg");
}

/* follows the Windows app light/dark setting (unless Silver Styler overrides it); called at startup and on WM_SETTINGCHANGE */
static void theme(void) {
    DWORD light = 0, sz = sizeof light;
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &light, &sz);
    if (g_cfgTheme) light = g_cfgTheme == 1;
    /* acrylic needs black to show through, so a custom background turns it off */
    int blur = acrylic(g_wnd, !light && st.blur && st.bg < 0 && st.field < 0) && !light;
    /* Otto's Theme.Back / Theme.Field, and Theme.Over(44) for the hovered card */
    if (light) { C_BG = RGB(243, 243, 243); C_FIELD = RGB(255, 255, 255); C_SEL = RGB(211, 211, 211); C_TEXT = RGB(0, 0, 0); C_SUB = RGB(96, 96, 96); C_BORDER = RGB(200, 200, 200); }
    else       { C_BG = RGB(26, 26, 26);    C_FIELD = RGB(46, 46, 46);    C_SEL = RGB(66, 66, 66); C_TEXT = RGB(255, 255, 255); C_SUB = RGB(160, 160, 160); C_BORDER = RGB(60, 60, 60); }
    C_ACCENT = RGB(0, 120, 215);
#define OVERRIDE(c, v) if ((v) >= 0) c = RGB(((v) >> 16) & 255, ((v) >> 8) & 255, (v) & 255)
    OVERRIDE(C_BG, st.bg); OVERRIDE(C_FIELD, st.field); OVERRIDE(C_TEXT, st.text); OVERRIDE(C_SUB, st.sub);
    OVERRIDE(C_SEL, st.sel); OVERRIDE(C_BORDER, st.borderC); OVERRIDE(C_ACCENT, st.accent);
#undef OVERRIDE
    C_LFIELD = C_FIELD; /* the launcher has no acrylic, so it keeps the real colour */
    if (blur) C_BG = C_FIELD = RGB(0, 0, 0); /* black = see-through on acrylic */
    if (g_fieldBrush) DeleteObject(g_fieldBrush);
    g_fieldBrush = CreateSolidBrush(C_FIELD);
}

/* ---------- launcher: an always-visible search bar or dot that opens Seek ---------- */

static void show(void);
static void hide(void);

static void paintLauncher(HDC dc, int w, int h) {
    RECT r = { 0, 0, w, h };
    SetBkMode(dc, TRANSPARENT);
    if (st.launcher == 2) { /* dot: accent disc with the search glyph */
        fill(dc, 0, 0, w, h, C_ACCENT);
        glyph(dc, g_fGlyph, &r, L"\xE721", RGB(255, 255, 255));
        return;
    }
    fill(dc, 0, 0, w, h, C_BORDER);
    int b = PX(st.border);
    fill(dc, b, b, w - b, h - b, C_LFIELD);
    RECT gr = { PX(4), 0, PX(34), h };
    glyph(dc, g_fGlyph, &gr, L"\xE721", C_SUB);
    RECT tr = { PX(36), 0, w - PX(8), h };
    SelectObject(dc, g_fText); SetTextColor(dc, C_SUB);
    DrawTextW(dc, st.lText, -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
}

static LRESULT CALLBACK launcherProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_SETCURSOR: SetCursor(LoadCursorW(NULL, IDC_HAND)); return TRUE;
    case WM_LBUTTONUP: if (IsWindowVisible(g_wnd)) hide(); else show(); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT cr; GetClientRect(h, &cr);
        paintLauncher(dc, cr.right, cr.bottom);
        EndPaint(h, &ps);
        return 0;
    }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void updateLauncher(void) {
    if (!st.launcher) { if (g_launch) ShowWindow(g_launch, SW_HIDE); return; }
    if (!g_launch) {
        WNDCLASSW wc = { 0, launcherProc, 0, 0, GetModuleHandleW(NULL), NULL, NULL, NULL, NULL, L"SeekLauncher" };
        RegisterClassW(&wc);
        g_launch = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, L"SeekLauncher", L"Seek",
                                   WS_POPUP, 0, 0, 1, 1, NULL, NULL, GetModuleHandleW(NULL), NULL);
    }
    MONITORINFO mi = { sizeof mi };
    GetMonitorInfoW(MonitorFromPoint((POINT){ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &mi);
    RECT w = mi.rcWork;
    int lw = st.launcher == 2 ? PX(st.lH) : PX(st.lW), lh = PX(st.lH);
    int x = w.left + (w.right - w.left - lw) * st.lX / 100, y = w.top + (w.bottom - w.top - lh) * st.lY / 100;
    SetWindowPos(g_launch, HWND_TOPMOST, x, y, lw, lh, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    roundWindow(g_launch, lw, lh, st.launcher == 2 ? lh / 2 : PX(st.lRadius));
    InvalidateRect(g_launch, NULL, TRUE);
}

/* "ctrl+shift+space", "alt+f1", "win+alt+k" ... */
static int parseHotkey(const wchar_t *s, UINT *mod, UINT *vk) {
    wchar_t t[32];
    lstrcpynW(t, s, 32);
    *mod = MOD_NOREPEAT; *vk = 0;
    wchar_t *ctx = NULL;
    for (wchar_t *p = wcstok(t, L"+ ", &ctx); p; p = wcstok(NULL, L"+ ", &ctx)) {
        if (!_wcsicmp(p, L"ctrl") || !_wcsicmp(p, L"control")) *mod |= MOD_CONTROL;
        else if (!_wcsicmp(p, L"alt")) *mod |= MOD_ALT;
        else if (!_wcsicmp(p, L"shift")) *mod |= MOD_SHIFT;
        else if (!_wcsicmp(p, L"win")) *mod |= MOD_WIN;
        else if (!_wcsicmp(p, L"space")) *vk = VK_SPACE;
        else if (!_wcsicmp(p, L"tab")) *vk = VK_TAB;
        else if (!_wcsicmp(p, L"enter")) *vk = VK_RETURN;
        else if ((p[0] == L'f' || p[0] == L'F') && p[1] >= L'1' && p[1] <= L'9') *vk = VK_F1 + _wtoi(p + 1) - 1;
        else if (!p[1] && iswalnum(p[0])) *vk = towupper(p[0]);
    }
    return *vk != 0;
}

static int registerHotkey(void) {
    UINT mod, vk;
    UnregisterHotKey(g_wnd, 1);
    return parseHotkey(st.hotkey, &mod, &vk) && RegisterHotKey(g_wnd, 1, mod, vk);
}

/* Fonts, edit box, opacity, launcher and hotkey: everything that depends on the Style. */
static HFONT font(const wchar_t *face, float px);
static void applyStyle(void) {
    theme();
    if (g_fText) { DeleteObject(g_fText); DeleteObject(g_fSub); }
    g_fText = font(st.font, st.fontSize);
    g_fSub = font(st.font, st.subSize);
    SendMessageW(g_edit, WM_SETFONT, (WPARAM)g_fText, 0);
    SendMessageW(g_edit, EM_SETCUEBANNER, TRUE, (LPARAM)st.placeholder);
    LONG ex = GetWindowLongW(g_wnd, GWL_EXSTYLE);
    if (st.opacity < 100) {
        SetWindowLongW(g_wnd, GWL_EXSTYLE, ex | WS_EX_LAYERED);
        SetLayeredWindowAttributes(g_wnd, 0, (BYTE)(st.opacity * 255 / 100), LWA_ALPHA);
    } else if (ex & WS_EX_LAYERED) SetWindowLongW(g_wnd, GWL_EXSTYLE, ex & ~WS_EX_LAYERED);
    updateLauncher();
    layout();
    InvalidateRect(g_wnd, NULL, TRUE);
    InvalidateRect(g_edit, NULL, TRUE);
}

/* ---------- window ---------- */

static int rowAt(LPARAM lp) {
    RECT cr; GetClientRect(g_wnd, &cr);
    int y = GET_Y_LPARAM(lp);
    for (int i = 0; i < g_nrows; i++) {
        int ry = rowY(i, cr.bottom);
        if (y >= ry && y < ry + H_ROW) return i;
    }
    return -1;
}

static LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_reloadMsg && g_reloadMsg) {
        loadStylerCfg();
        applyStyle();
        registerHotkey();
        return 0;
    }
    switch (msg) {
    case WM_HOTKEY: case WM_SHOWME:
        if (msg == WM_HOTKEY && IsWindowVisible(h) && GetForegroundWindow() == h) hide(); else show();
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) hide();
        return 0;
    case WM_COMMAND:
        if (HIWORD(wp) == EN_CHANGE) search();
        return 0;
    case WM_CTLCOLOREDIT:
        SetTextColor((HDC)wp, C_TEXT); SetBkColor((HDC)wp, C_FIELD);
        return (LRESULT)g_fieldBrush;
    case WM_INDEX:
        freeIndex(g_ix);
        g_ix = (Index *)lp;
        InterlockedExchange(&g_scanning, 0);
        search();
        return 0;
    case WM_MOUSEMOVE: {
        int r = rowAt(lp);
        if (r >= 0 && r != g_sel) { g_sel = r; InvalidateRect(h, NULL, FALSE); }
        return 0;
    }
    case WM_LBUTTONUP: {
        int r = rowAt(lp);
        if (r >= 0) activate(r, GetKeyState(VK_CONTROL) < 0);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { /* double-buffered so typing never flickers */
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT cr; GetClientRect(h, &cr);
        HDC mdc = CreateCompatibleDC(dc);
        HBITMAP bm = CreateCompatibleBitmap(dc, cr.right, cr.bottom);
        HGDIOBJ old = SelectObject(mdc, bm);
        paint(mdc, &cr);
        BitBlt(dc, 0, 0, cr.right, cr.bottom, mdc, 0, 0, SRCCOPY);
        SelectObject(mdc, old); DeleteObject(bm); DeleteDC(mdc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_TRAY:
        if (lp == WM_LBUTTONUP) show();
        else if (lp == WM_RBUTTONUP) trayMenu();
        return 0;
    case WM_SETTINGCHANGE: /* Windows light/dark switch: recolour live */
        if (lp && !wcscmp((const wchar_t *)lp, L"ImmersiveColorSet")) applyStyle();
        break;
    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    if (msg == g_taskbarCreated && msg) { addTray(); return 0; } /* Explorer restarted */
    return DefWindowProcW(h, msg, wp, lp);
}

/* ---------- dev flags ---------- */

/* --bench: build the index and time searches, for measuring without the UI */
static void bench(void) {
    LARGE_INTEGER f, a, b; QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    g_ix = buildIndex();
    QueryPerformanceCounter(&b);
    if (!g_ix) return;
    char out[4096]; int n = 0;
    n += sprintf(out + n, "index: %u entries, %.0f ms, %.1f MB\n", g_ix->n, (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart,
                 (g_ix->sn * sizeof(wchar_t) + g_ix->n * sizeof(Entry)) / 1048576.0);
    static const wchar_t *qs[] = { L"c", L"chrome", L"note", L"xyzq", L"pdf", L"rdme", L"2pi*sqrt(16)+5!", 0 };
    for (int i = 0; qs[i]; i++) {
        SetWindowTextW(g_edit, qs[i]);
        QueryPerformanceCounter(&a);
        for (int k = 0; k < 50; k++) search();
        QueryPerformanceCounter(&b);
        const Row *r = &g_rows[0];
        const wchar_t *top = L"-";
        int topLen = 1;
        if (r->kind == K_CALC) { top = g_calc; topLen = (int)wcslen(g_calc); }
        else if (g_nrows > 1) { top = g_ix->s + g_ix->e[r->idx].name; topLen = g_ix->e[r->idx].dispLen; }
        n += sprintf(out + n, "%-16ls %.2f ms  top: %.*ls\n", qs[i], (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart / 50, topLen, top);
    }
    DWORD w; WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), out, n, &w, NULL);
}

/* --calc "expr": print what the calculator row would show */
static void calcCli(const wchar_t *q) {
    wchar_t r[64];
    char out[256];
    int n = calc(q, r, 64) ? sprintf(out, "%ls\n", r) : sprintf(out, "(no result)\n");
    DWORD w; WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), out, n, &w, NULL);
}

/* --shot "query" out.bmp: render the window offscreen to a bitmap (dev check) */
static void shot(const wchar_t *q, const wchar_t *file) {
    g_ix = buildIndex();
    g_above = st.results == 1;
    SetWindowTextW(g_edit, q);
    search();
    RECT cr; GetClientRect(g_wnd, &cr);
    int w = cr.right, h = cr.bottom;
    HDC sdc = GetDC(NULL), mdc = CreateCompatibleDC(sdc);
    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), w, h, 1, 32, BI_RGB } };
    void *bits; HBITMAP bm = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    SelectObject(mdc, bm);
    paint(mdc, &cr);
    RECT er; GetWindowRect(g_edit, &er); MapWindowPoints(NULL, g_wnd, (POINT *)&er, 2);
    SetViewportOrgEx(mdc, er.left, er.top, NULL);
    SendMessageW(g_edit, WM_PRINT, (WPARAM)mdc, PRF_CLIENT | PRF_ERASEBKGND);
    GdiFlush();
    BITMAPFILEHEADER fh = { 0x4D42, (DWORD)(sizeof fh + sizeof bi.bmiHeader + w * h * 4), 0, 0, sizeof fh + sizeof bi.bmiHeader };
    HANDLE f = CreateFileW(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD wr; WriteFile(f, &fh, sizeof fh, &wr, NULL); WriteFile(f, &bi.bmiHeader, sizeof bi.bmiHeader, &wr, NULL);
    WriteFile(f, bits, w * h * 4, &wr, NULL); CloseHandle(f);
    if (g_launch && st.launcher) { /* and the launcher, next to it: <file>.launcher.bmp */
        RECT lr; GetClientRect(g_launch, &lr);
        BITMAPINFO li = { { sizeof(BITMAPINFOHEADER), lr.right, lr.bottom, 1, 32, BI_RGB } };
        void *lb; HBITMAP lbm = CreateDIBSection(sdc, &li, DIB_RGB_COLORS, &lb, NULL, 0);
        SelectObject(mdc, lbm); SetViewportOrgEx(mdc, 0, 0, NULL);
        paintLauncher(mdc, lr.right, lr.bottom); GdiFlush();
        wchar_t lf[MAX_PATH]; swprintf(lf, MAX_PATH, L"%s.launcher.bmp", file);
        BITMAPFILEHEADER lh = { 0x4D42, (DWORD)(sizeof lh + sizeof li.bmiHeader + lr.right * lr.bottom * 4), 0, 0, sizeof lh + sizeof li.bmiHeader };
        f = CreateFileW(lf, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        if (f == INVALID_HANDLE_VALUE) return;
        WriteFile(f, &lh, sizeof lh, &wr, NULL); WriteFile(f, &li.bmiHeader, sizeof li.bmiHeader, &wr, NULL);
        WriteFile(f, lb, lr.right * lr.bottom * 4, &wr, NULL); CloseHandle(f);
    }
}

/* ---------- startup ---------- */

static HFONT font(const wchar_t *face, float px) {
    return CreateFontW(-PX(px), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, face);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmd, int show_) {
    (void)prev; (void)show_; (void)cmd;
    /* load system DLLs only from System32, never from the folder seek.exe sits in */
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    int argc;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const wchar_t *flag = argv && argc > 1 ? argv[1] : L"";
    if (argc == 3 && !wcscmp(flag, L"--calc")) { calcCli(argv[2]); return 0; }
    int dev = !wcscmp(flag, L"--bench") || !wcscmp(flag, L"--shot");
    if (!dev) {
        CreateMutexW(NULL, TRUE, L"SeekLauncherSingleInstance"); /* held until exit */
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            HWND w = FindWindowW(L"SeekWnd", NULL);
            if (w) { AllowSetForegroundWindow(ASFW_ANY); PostMessageW(w, WM_SHOWME, 0, 0); }
            return 0;
        }
    }
    /* system-aware: one scale for every monitor, and Windows rescales on the others */
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE);
    S = GetDpiForSystem() / 96.0f;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    InitializeCriticalSection(&g_useLock);
    loadUsage();
    g_fGlyph = font(L"Segoe MDL2 Assets", 12);      /* 9pt */
    g_fIcon = font(L"Segoe MDL2 Assets", 20);
    SHFILEINFOW sfi;
    g_sysil = (HIMAGELIST)SHGetFileInfoW(L"C:\\", 0, &sfi, sizeof sfi, SHGFI_SYSICONINDEX | SHGFI_LARGEICON);
    int ih; ImageList_GetIconSize(g_sysil, &g_iconPx, &ih);

    WNDCLASSW wc = { 0, wndProc, 0, 0, inst, NULL, LoadCursorW(NULL, IDC_ARROW), NULL, NULL, L"SeekWnd" };
    RegisterClassW(&wc);
    g_wnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"SeekWnd", L"Seek", WS_POPUP | WS_CLIPCHILDREN,
                            0, 0, W_WIN, H_FIELD, NULL, NULL, inst, NULL);
    g_indexWnd = g_wnd;
    g_reloadMsg = RegisterWindowMessageW(L"SilverStylerReload");
    loadStylerCfg();
    g_edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 1, 1, g_wnd, NULL, inst, NULL);
    SendMessageW(g_edit, EM_SETLIMITTEXT, MAXQUERY - 1, 0);
    g_editProc = (WNDPROC)SetWindowLongPtrW(g_edit, GWLP_WNDPROC, (LONG_PTR)editProc);
    applyStyle();

    if (!wcscmp(flag, L"--bench")) { bench(); return 0; }
    if (argc == 4 && !wcscmp(flag, L"--shot")) { shot(argv[2], argv[3]); return 0; }
    LocalFree(argv);

    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    addTray();
    if (!registerHotkey()) {
        wchar_t m[160];
        swprintf(m, 160, L"%s is already taken by another program (PowerToys Run?).\nUse the tray icon to open Seek.", st.hotkey);
        MessageBoxW(NULL, m, L"Seek", MB_ICONWARNING);
    }

    HANDLE t = CreateThread(NULL, 64 * 1024, watchThread, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (t) CloseHandle(t);
    rescan();

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
}
