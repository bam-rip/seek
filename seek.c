/* Seek - a Spotlight-style launcher for Windows 10. Alt+Space to open.
   One C file, no dependencies beyond Win32. Everything is indexed into RAM
   at startup, so each keystroke is a linear scan of memory, not a disk query. */
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <wctype.h>
#pragma comment(lib, "user32")
#pragma comment(lib, "gdi32")
#pragma comment(lib, "shell32")
#pragma comment(lib, "ole32")
#pragma comment(lib, "comctl32")
#pragma comment(lib, "dwmapi")
#pragma comment(lib, "advapi32")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#define MAXN      1500000
#define MAXROWS   8
#define WM_INDEX  (WM_APP + 1)
#define WM_TRAY   (WM_APP + 2)
#define WM_SHOWME (WM_APP + 3)

enum { K_APP, K_DIR, K_FILE, K_CALC, K_WEB };

typedef struct { unsigned path, disp, lname; unsigned short dispLen, lnameLen, dirLen; unsigned char kind, uses; int icon; } Entry;
typedef struct { wchar_t *s; size_t sn, scap; Entry *e; int n, cap; } Index;
typedef struct { int kind, idx, score; } Row;

static HWND g_wnd, g_edit;
static WNDPROC g_editProc;
static Index *g_ix;
static Row g_rows[MAXROWS + 2];
static int g_nrows, g_sel;
static wchar_t g_calc[64];
static HFONT g_fEdit, g_fName, g_fSub, g_fGlyph, g_fIcon;
static HIMAGELIST g_sysil;
static int g_iconPx;
static float S = 1;
static COLORREF C_BG, C_FIELD, C_SEL, C_TEXT, C_SUB, C_BORDER;
static const COLORREF C_ACCENT = RGB(0, 120, 215);
static HBRUSH g_fieldBrush;
static HBRUSH g_bgBrush;
static volatile LONG g_scanning;
static DWORD g_lastScan;
static UINT g_taskbarCreated;
static NOTIFYICONDATAW g_nid;

#define PX(v) ((int)((v) * S + 0.5f))
/* sizes match Otto's search box and chat cards (HistoryView.cs) */
#define W_WIN PX(640)
#define MARGIN PX(0)
#define H_FIELD PX(36)
#define H_TOP (H_FIELD + PX(4))
#define H_ROW PX(54)
#define GAP PX(4)

/* ---------- usage: things you open often rank higher ---------- */

#define MAXUSE 512
typedef struct { unsigned h; int count; } Use;
static Use g_use[MAXUSE];
static int g_nuse;
static CRITICAL_SECTION g_useLock;

static unsigned hashPath(const wchar_t *p) {
    unsigned h = 2166136261u;
    for (; *p; p++) h = (h ^ towlower(*p)) * 16777619u;
    return h ? h : 1;
}

static void usageFile(wchar_t *out) {
    SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, out);
    wcscat(out, L"\\Seek");
    CreateDirectoryW(out, NULL);
    wcscat(out, L"\\usage.txt");
}

static void loadUsage(void) {
    wchar_t f[MAX_PATH + 32]; usageFile(f);
    FILE *fp = _wfopen(f, L"r");
    if (!fp) return;
    unsigned h; int c;
    while (g_nuse < MAXUSE && fscanf(fp, "%u %d", &h, &c) == 2) g_use[g_nuse++] = (Use){ h, c };
    fclose(fp);
}

static void bumpUsage(const wchar_t *path) {
    unsigned h = hashPath(path);
    EnterCriticalSection(&g_useLock);
    int i = 0;
    while (i < g_nuse && g_use[i].h != h) i++;
    if (i == g_nuse) {
        if (g_nuse < MAXUSE) g_nuse++;
        else { i = 0; for (int j = 1; j < g_nuse; j++) if (g_use[j].count < g_use[i].count) i = j; }
        g_use[i] = (Use){ h, 0 };
    }
    g_use[i].count++;
    wchar_t f[MAX_PATH + 32]; usageFile(f);
    FILE *fp = _wfopen(f, L"w");
    if (fp) { for (int j = 0; j < g_nuse; j++) fprintf(fp, "%u %d\n", g_use[j].h, g_use[j].count); fclose(fp); }
    LeaveCriticalSection(&g_useLock);
}

/* tag each entry with its open count; a small hash table keeps this one probe per entry */
static void applyUsage(Index *ix) {
    static Use t[MAXUSE * 2];
    memset(t, 0, sizeof t);
    EnterCriticalSection(&g_useLock);
    for (int i = 0; i < g_nuse; i++) {
        unsigned k = g_use[i].h % (MAXUSE * 2);
        while (t[k].h) k = (k + 1) % (MAXUSE * 2);
        t[k] = g_use[i];
    }
    LeaveCriticalSection(&g_useLock);
    for (int i = 0; i < ix->n; i++) {
        unsigned h = hashPath(ix->s + ix->e[i].path), k = h % (MAXUSE * 2);
        while (t[k].h && t[k].h != h) k = (k + 1) % (MAXUSE * 2);
        ix->e[i].uses = (unsigned char)(t[k].h ? (t[k].count > 255 ? 255 : t[k].count) : 0);
    }
}

/* ---------- index ---------- */

static unsigned push(Index *ix, const wchar_t *s, int n, int lower) {
    if (ix->sn + n + 1 > ix->scap) {
        ix->scap = (ix->scap + n + 1) * 2;
        ix->s = realloc(ix->s, ix->scap * sizeof(wchar_t));
    }
    unsigned at = (unsigned)ix->sn;
    wchar_t *d = ix->s + at;
    if (lower) for (int i = 0; i < n; i++) d[i] = towlower(s[i]);
    else memcpy(d, s, n * sizeof(wchar_t));
    d[n] = 0;
    ix->sn += n + 1;
    return at;
}

static void add(Index *ix, int kind, const wchar_t *path, int plen, const wchar_t *name, int nlen, int dirLen) {
    if (ix->n == ix->cap) { ix->cap = ix->cap ? ix->cap * 2 : 4096; ix->e = realloc(ix->e, ix->cap * sizeof(Entry)); }
    Entry *e = &ix->e[ix->n++];
    e->kind = (unsigned char)kind;
    e->path = push(ix, path, plen, 0);
    e->disp = kind == K_APP ? push(ix, name, nlen, 0) : e->path + (unsigned)(plen - nlen);
    e->lname = push(ix, name, nlen, 1);
    e->dispLen = e->lnameLen = (unsigned short)nlen;
    e->dirLen = (unsigned short)dirLen;
    e->icon = -1;
}

static void freeIndex(Index *ix) { if (ix) { free(ix->s); free(ix->e); free(ix); } }

static int skipDir(const wchar_t *n) {
    static const wchar_t *skip[] = { L"AppData", L"node_modules", L"$Recycle.Bin", L"__pycache__",
        L"site-packages", L"obj", L".git", L"Temp", L"cache", L"Cache", 0 };
    if (n[0] == '.') return 1;
    for (int i = 0; skip[i]; i++) if (!wcscmp(n, skip[i])) return 1;
    return 0;
}

static int endsWith(const wchar_t *s, int n, const wchar_t *suf) {
    int m = (int)wcslen(suf);
    return n >= m && !_wcsicmp(s + n - m, suf);
}

/* apps: walk Start menu folders, keep shortcuts, show them without extension */
static void scanApps(Index *ix, wchar_t *p, int len) {
    wcscpy(p + len, L"\\*");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(p, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        wchar_t *n = fd.cFileName;
        if (n[0] == '.') continue;
        int nl = (int)wcslen(n);
        p[len] = '\\';
        memcpy(p + len + 1, n, (nl + 1) * sizeof(wchar_t));
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { scanApps(ix, p, len + 1 + nl); continue; }
        int bl = nl;
        if (endsWith(n, nl, L".lnk") || endsWith(n, nl, L".url")) bl = nl - 4;
        else if (endsWith(n, nl, L".appref-ms")) bl = nl - 10;
        else continue;
        wchar_t low[MAX_PATH]; int i;
        for (i = 0; i < bl && i < MAX_PATH - 1; i++) low[i] = towlower(n[i]);
        low[i] = 0;
        if (wcsstr(low, L"uninstall")) continue;
        int dup = 0;
        for (int j = 0; j < ix->n && !dup; j++)
            if (ix->e[j].kind == K_APP && ix->e[j].lnameLen == bl && !wcscmp(ix->s + ix->e[j].lname, low)) dup = 1;
        if (!dup) add(ix, K_APP, p, len + 1 + nl, n, bl, len);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void scanFiles(Index *ix, wchar_t *p, int len, int depth) {
    if (len > 32000 || depth > 48 || ix->n >= MAXN) return;
    wcscpy(p + len, L"\\*");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(p, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        wchar_t *n = fd.cFileName;
        DWORD a = fd.dwFileAttributes;
        if (n[0] == '.' && (!n[1] || (n[1] == '.' && !n[2]))) continue;
        if (a & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
        int nl = (int)wcslen(n);
        if (len + 1 + nl > 32000) continue;
        p[len] = '\\';
        memcpy(p + len + 1, n, (nl + 1) * sizeof(wchar_t));
        if (a & FILE_ATTRIBUTE_DIRECTORY) {
            if ((a & FILE_ATTRIBUTE_REPARSE_POINT) &&
                (fd.dwReserved0 == IO_REPARSE_TAG_SYMLINK || fd.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT)) continue;
            if (skipDir(n)) continue;
            add(ix, K_DIR, p, len + 1 + nl, n, nl, len);
            scanFiles(ix, p, len + 1 + nl, depth + 1);
        } else {
            add(ix, K_FILE, p, len + 1 + nl, n, nl, len);
        }
    } while (ix->n < MAXN && FindNextFileW(h, &fd));
    FindClose(h);
}

static Index *buildIndex(void) {
    Index *ix = calloc(1, sizeof(Index));
    static wchar_t p[32768];
    static const int apps[] = { CSIDL_PROGRAMS, CSIDL_COMMON_PROGRAMS };
    for (int i = 0; i < 2; i++)
        if (SHGetFolderPathW(NULL, apps[i], NULL, 0, p) == S_OK) scanApps(ix, p, (int)wcslen(p));
    add(ix, K_APP, L"ms-settings:", 12, L"Settings", 8, 0);
    ix->e[ix->n - 1].icon = -2;
    if (SHGetFolderPathW(NULL, CSIDL_PROFILE, NULL, 0, p) == S_OK) scanFiles(ix, p, (int)wcslen(p), 0);
    applyUsage(ix);
    return ix;
}

static DWORD WINAPI indexThread(void *arg) {
    (void)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    Index *ix = buildIndex();
    if (!PostMessageW(g_wnd, WM_INDEX, 0, (LPARAM)ix)) freeIndex(ix);
    return 0;
}

static void rescan(void) {
    if (InterlockedCompareExchange(&g_scanning, 1, 0)) return;
    g_lastScan = GetTickCount();
    CloseHandle(CreateThread(NULL, 0, indexThread, NULL, 0, NULL));
}

/* ---------- matching ---------- */

static int isWordStart(const wchar_t *s, const wchar_t *at) { return at == s || !iswalnum(at[-1]); }

static int score(const wchar_t *ln, int len, const wchar_t *q, int m, int fuzzy) {
    const wchar_t *f = wcsstr(ln, q);
    int lp = len > 60 ? 60 : len;
    if (f) {
        if (f == ln) return (len == m ? 1000 : 900) - lp;
        for (const wchar_t *g = f; g; g = wcsstr(g + 1, q))
            if (isWordStart(ln, g)) return 700 - lp;
        return 500 - (int)(f - ln) - lp;
    }
    if (m < 2 || !fuzzy) return -1;
    int i = 0, starts = 0;
    for (int j = 0; j < len && i < m; j++)
        if (ln[j] == q[i]) { if (isWordStart(ln, ln + j)) starts++; i++; }
    if (i < m || starts == 0) return -1;
    return 100 + starts * 40 - lp;
}

/* ---------- calculator ---------- */

static const wchar_t *cp;
static int cerr;
static double cexpr(void);
static void cws(void) { while (*cp == ' ') cp++; }
static double cprim(void) {
    cws();
    if (*cp == '(') { cp++; double v = cexpr(); cws(); if (*cp == ')') cp++; return v; }
    wchar_t *e; double v = wcstod(cp, &e);
    if (e == cp) { cerr = 1; return 0; }
    cp = e; return v;
}
static double cunary(void);
static double cpow(void) { double b = cprim(); cws(); if (*cp == '^') { cp++; return pow(b, cunary()); } return b; }
static double cunary(void) { cws(); if (*cp == '-') { cp++; return -cunary(); } if (*cp == '+') { cp++; return cunary(); } return cpow(); }
static double cterm(void) {
    double v = cunary();
    for (;;) {
        cws(); wchar_t o = *cp;
        if (o != '*' && o != '/' && o != '%' && o != 'x') return v;
        cp++; double r = cunary();
        v = o == '/' ? v / r : o == '%' ? fmod(v, r) : v * r;
    }
}
static double cexpr(void) {
    double v = cterm();
    for (;;) { cws(); if (*cp == '+') { cp++; v += cterm(); } else if (*cp == '-') { cp++; v -= cterm(); } else return v; }
}

static int calc(const wchar_t *q, wchar_t *out) {
    int digit = 0, op = 0;
    if (*q == '=') q++;
    for (const wchar_t *s = q; *s; s++) {
        if (iswdigit(*s)) digit = 1;
        else if (wcschr(L"+-*/%^x", *s)) op = op || s != q;
        else if (!wcschr(L" .()", *s)) return 0;
    }
    if (!digit || !op) return 0;
    cp = q; cerr = 0;
    double v = cexpr(); cws();
    if (cerr || *cp || !isfinite(v)) return 0;
    if (v == floor(v) && fabs(v) < 1e15) swprintf(out, 64, L"%.0f", v);
    else swprintf(out, 64, L"%.10g", v);
    return 1;
}

/* ---------- search ---------- */

static void layout(void);

static void search(void) {
    wchar_t raw[512], q[512];
    int m = GetWindowTextW(g_edit, raw, 512);
    while (m && raw[m - 1] == ' ') raw[--m] = 0;
    int s0 = 0; while (raw[s0] == ' ') s0++;
    m = 0;
    for (const wchar_t *s = raw + s0; *s; s++) q[m++] = towlower(*s);
    q[m] = 0;
    g_nrows = 0; g_sel = 0;
    if (m) {
        if (calc(raw + s0, g_calc)) g_rows[g_nrows++] = (Row){ K_CALC, -1, 0 };
        Row top[MAXROWS]; int nt = 0;
        int lim = MAXROWS - g_nrows - 1;
        Index *ix = g_ix;
        if (ix) for (int i = 0; i < ix->n; i++) {
            Entry *e = &ix->e[i];
            int sc = score(ix->s + e->lname, e->lnameLen, q, m, e->kind == K_APP);
            if (sc < 0) continue;
            sc += (e->kind == K_APP ? 600 : e->kind == K_DIR ? 30 : 0) + (e->uses > 20 ? 20 : e->uses) * 40;
            if (nt == lim && sc <= top[nt - 1].score) continue;
            int j = nt < lim ? nt++ : nt - 1;
            while (j > 0 && top[j - 1].score < sc) { top[j] = top[j - 1]; j--; }
            top[j] = (Row){ e->kind, i, sc };
        }
        for (int i = 0; i < nt; i++) g_rows[g_nrows++] = top[i];
        g_rows[g_nrows++] = (Row){ K_WEB, -1, 0 };
    }
    layout();
}

/* ---------- actions ---------- */

static void hide(void) { ShowWindow(g_wnd, SW_HIDE); }

static void copyText(const wchar_t *t) {
    size_t n = (wcslen(t) + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, n);
    if (!h) return;
    memcpy(GlobalLock(h), t, n); GlobalUnlock(h);
    if (OpenClipboard(g_wnd)) { EmptyClipboard(); SetClipboardData(CF_UNICODETEXT, h); CloseClipboard(); }
    else GlobalFree(h);
}

static void activate(int r, int reveal) {
    if (r < 0 || r >= g_nrows) return;
    Row *row = &g_rows[r];
    hide();
    if (row->kind == K_CALC) { copyText(g_calc); return; }
    if (row->kind == K_WEB) {
        wchar_t q[512], url[2048] = L"https://www.google.com/search?q=";
        char u8[1536];
        GetWindowTextW(g_edit, q, 512);
        int n = WideCharToMultiByte(CP_UTF8, 0, q, -1, u8, sizeof u8, NULL, NULL);
        wchar_t *o = url + wcslen(url);
        for (int i = 0; i < n - 1 && o < url + 2040; i++) {
            unsigned char c = (unsigned char)u8[i];
            if (isalnum(c) || strchr("-_.~", c)) *o++ = c;
            else o += swprintf(o, 4, L"%%%02X", c);
        }
        *o = 0;
        ShellExecuteW(NULL, NULL, url, NULL, NULL, SW_SHOWNORMAL);
        return;
    }
    Entry *e = &g_ix->e[row->idx];
    const wchar_t *path = g_ix->s + e->path;
    if (e->uses < 255) e->uses++;
    bumpUsage(path);
    if (reveal && path[1] == ':') {
        wchar_t arg[33000];
        swprintf(arg, 33000, L"/select,\"%s\"", path);
        ShellExecuteW(NULL, NULL, L"explorer.exe", arg, NULL, SW_SHOWNORMAL);
    } else {
        ShellExecuteW(NULL, NULL, path, NULL, NULL, SW_SHOWNORMAL);
    }
}

/* ---------- drawing ---------- */

static int iconOf(Entry *e) {
    if (e->icon != -1) return e->icon;
    const wchar_t *p = g_ix->s + e->path;
    int len = (int)wcslen(p);
    UINT f = SHGFI_SYSICONINDEX | SHGFI_LARGEICON;
    DWORD attr = 0;
    static const wchar_t *own[] = { L".exe", L".lnk", L".ico", L".url", L".appref-ms", L".msc", L".cpl", 0 };
    int real = 0;
    for (int i = 0; own[i]; i++) if (endsWith(p, len, own[i])) real = 1;
    if (e->kind == K_DIR) { f |= SHGFI_USEFILEATTRIBUTES; attr = FILE_ATTRIBUTE_DIRECTORY; }
    else if (!real) { f |= SHGFI_USEFILEATTRIBUTES; attr = FILE_ATTRIBUTE_NORMAL; }
    SHFILEINFOW sfi = { 0 };
    SHGetFileInfoW(p, attr, &sfi, sizeof sfi, f);
    return e->icon = sfi.iIcon;
}

static int webIcon(void) {
    static int i = -1;
    if (i < 0) { SHFILEINFOW sfi = { 0 }; SHGetFileInfoW(L".html", FILE_ATTRIBUTE_NORMAL, &sfi, sizeof sfi, SHGFI_SYSICONINDEX | SHGFI_LARGEICON | SHGFI_USEFILEATTRIBUTES); i = sfi.iIcon; }
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

static void paint(HDC dc, RECT *cr) {
    fill(dc, 0, 0, cr->right, cr->bottom, C_BG);
    HBRUSH b = CreateSolidBrush(C_BORDER);
    FrameRect(dc, cr, b);
    DeleteObject(b);
    /* search field: Otto's FieldBox with the focused accent outline */
    int fl = MARGIN, ft = MARGIN, fr = cr->right - MARGIN, fb = MARGIN + H_FIELD;
    fill(dc, fl, ft, fr, fb, C_ACCENT);
    fill(dc, fl + 1, ft + 1, fr - 1, fb - 1, C_FIELD);
    SetBkMode(dc, TRANSPARENT);
    RECT gr = { fl + PX(4), ft, fl + PX(34), fb };
    glyph(dc, g_fGlyph, &gr, L"\xE721", C_SUB);
    for (int i = 0; i < g_nrows; i++) {
        Row *row = &g_rows[i];
        int y = H_TOP + i * (H_ROW + GAP);
        int l = PX(4), r = cr->right - PX(4);
        if (i == g_sel) {
            fill(dc, l, y, r, y + H_ROW, C_SEL);
            fill(dc, l, y, l + PX(3), y + H_ROW, C_ACCENT);
        }
        RECT ir = { l + PX(8), y, l + PX(8) + g_iconPx, y + H_ROW };
        int iy0 = y + (H_ROW - g_iconPx) / 2;
        const wchar_t *name, *sub; wchar_t buf[600]; int nameLen = -1, subLen = -1;
        if (row->kind == K_CALC) {
            glyph(dc, g_fIcon, &ir, L"\xE8EF", C_TEXT);
            name = g_calc; sub = L"Calculator \x2014 Enter to copy";
        } else if (row->kind == K_WEB) {
            ImageList_Draw(g_sysil, webIcon(), dc, ir.left, iy0, ILD_TRANSPARENT);
            wchar_t q[512]; GetWindowTextW(g_edit, q, 512);
            swprintf(buf, 600, L"Search the web for \x201C%s\x201D", q);
            name = buf; sub = L"Opens in your browser";
        } else {
            Entry *e = &g_ix->e[row->idx];
            int ic = iconOf(e);
            if (ic == -2) glyph(dc, g_fIcon, &ir, L"\xE713", C_TEXT);
            else ImageList_Draw(g_sysil, ic, dc, ir.left, iy0, ILD_TRANSPARENT);
            name = g_ix->s + e->disp; nameLen = e->dispLen;
            if (e->kind == K_APP) sub = L"App";
            else { sub = g_ix->s + e->path; subLen = e->dirLen; }
        }
        int tx = ir.right + PX(12);
        RECT tr = { tx, y + PX(8), r - PX(12), y + PX(28) };
        SelectObject(dc, g_fName); SetTextColor(dc, C_TEXT);
        DrawTextW(dc, name, nameLen, &tr, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        RECT sr = { tx, y + PX(30), r - PX(12), y + PX(48) };
        SelectObject(dc, g_fSub); SetTextColor(dc, C_SUB);
        wchar_t sb2[1024];
        if (subLen >= 0) { int n = subLen < 1023 ? subLen : 1023; memcpy(sb2, sub, n * sizeof(wchar_t)); sb2[n] = 0; sub = sb2; }
        DrawTextW(dc, sub, -1, &sr, DT_SINGLELINE | DT_NOPREFIX | DT_PATH_ELLIPSIS);
    }
}

static void layout(void) {
    int h = g_nrows ? H_TOP + g_nrows * (H_ROW + GAP) : H_FIELD;
    RECT wr; GetWindowRect(g_wnd, &wr);
    if (wr.bottom - wr.top != h) SetWindowPos(g_wnd, NULL, 0, 0, W_WIN, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(g_wnd, NULL, FALSE);
}

static void show(void) {
    POINT pt; GetCursorPos(&pt);
    MONITORINFO mi = { sizeof mi };
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
    RECT w = mi.rcWork;
    int x = w.left + (w.right - w.left - W_WIN) / 2, y = w.top + (w.bottom - w.top) / 4;
    SetWindowPos(g_wnd, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(g_wnd);
    SetFocus(g_edit);
    SendMessageW(g_edit, EM_SETSEL, 0, -1);
    if (GetTickCount() - g_lastScan > 60 * 1000) rescan();
}

/* ---------- edit box ---------- */

static LRESULT CALLBACK editProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_KEYDOWN) {
        int ctrl = GetKeyState(VK_CONTROL) < 0;
        switch (wp) {
        case VK_DOWN: case VK_TAB: if (g_nrows) { g_sel = (g_sel + 1) % g_nrows; InvalidateRect(g_wnd, NULL, FALSE); } return 0;
        case VK_UP: if (g_nrows) { g_sel = (g_sel + g_nrows - 1) % g_nrows; InvalidateRect(g_wnd, NULL, FALSE); } return 0;
        case VK_RETURN: activate(g_sel, ctrl); return 0;
        case VK_ESCAPE: if (GetWindowTextLengthW(h)) SetWindowTextW(h, L""); else hide(); return 0;
        }
    }
    if (msg == WM_CHAR) {
        if (wp == '\r' || wp == 27 || wp == '\t') return 0;
        if (wp == 0x7F) { /* Ctrl+Backspace: delete previous word */
            DWORD end; SendMessageW(h, EM_GETSEL, 0, (LPARAM)&end);
            wchar_t t[512]; GetWindowTextW(h, t, 512);
            int i = (int)end;
            while (i > 0 && t[i - 1] == ' ') i--;
            while (i > 0 && t[i - 1] != ' ' && t[i - 1] != '\\' && t[i - 1] != '.') i--;
            SendMessageW(h, EM_SETSEL, i, end);
            SendMessageW(h, EM_REPLACESEL, TRUE, (LPARAM)L"");
            return 0;
        }
    }
    return CallWindowProcW(g_editProc, h, msg, wp, lp);
}

/* ---------- tray / autostart ---------- */

static const wchar_t *RUNKEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

static int autostart(int set) {
    HKEY k; int on = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUNKEY, 0, KEY_READ | KEY_WRITE, &k)) return 0;
    on = RegQueryValueExW(k, L"Seek", NULL, NULL, NULL, NULL) == ERROR_SUCCESS;
    if (set) {
        if (on) RegDeleteValueW(k, L"Seek");
        else {
            wchar_t exe[MAX_PATH + 2] = L"\"";
            GetModuleFileNameW(NULL, exe + 1, MAX_PATH);
            wcscat(exe, L"\"");
            RegSetValueExW(k, L"Seek", 0, REG_SZ, (BYTE *)exe, (DWORD)(wcslen(exe) + 1) * sizeof(wchar_t));
        }
        on = !on;
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
    wcscpy(g_nid.szTip, L"Seek \x2014 Alt+Space");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void trayMenu(void) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1, L"Open\tAlt+Space");
    AppendMenuW(m, MF_STRING | (autostart(0) ? MF_CHECKED : 0), 2, L"Start with Windows");
    AppendMenuW(m, MF_STRING | (g_scanning ? MF_GRAYED : 0), 3, g_scanning ? L"Indexing\x2026" : L"Re-index now");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, 4, L"Exit");
    POINT pt; GetCursorPos(&pt);
    SetForegroundWindow(g_wnd);
    int c = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_wnd, NULL);
    DestroyMenu(m);
    if (c == 1) show();
    else if (c == 2) autostart(1);
    else if (c == 3) rescan();
    else if (c == 4) DestroyWindow(g_wnd);
}

/* ---------- window ---------- */

static LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_HOTKEY: case WM_SHOWME:
        if (IsWindowVisible(h) && GetForegroundWindow() == h && msg == WM_HOTKEY) hide(); else show();
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
        HeapCompact(GetProcessHeap(), 0);
        SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
        return 0;
    case WM_MOUSEMOVE: {
        int r = (GET_Y_LPARAM(lp) - H_TOP) / (H_ROW + GAP);
        if (GET_Y_LPARAM(lp) > H_TOP && r >= 0 && r < g_nrows && r != g_sel) { g_sel = r; InvalidateRect(h, NULL, FALSE); }
        return 0;
    }
    case WM_LBUTTONUP:
        if (GET_Y_LPARAM(lp) > H_TOP) {
            int r = (GET_Y_LPARAM(lp) - H_TOP) / (H_ROW + GAP);
            if (r < g_nrows) activate(r, GetKeyState(VK_CONTROL) < 0);
        }
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
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
    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    if (msg == g_taskbarCreated && msg) { addTray(); return 0; }
    return DefWindowProcW(h, msg, wp, lp);
}

/* live blur behind the window, same call and tint as Otto's panel (Win32.cs Acrylic) */
typedef struct { int state, flags; DWORD color; int anim; } AccentPolicy;
typedef struct { int attr; void *data; SIZE_T size; } WinCompAttrData;
static int g_acrylic;

static int acrylic(HWND h) {
    typedef BOOL(WINAPI *SWCA)(HWND, WinCompAttrData *);
    SWCA f = (SWCA)GetProcAddress(GetModuleHandleW(L"user32"), "SetWindowCompositionAttribute");
    AccentPolicy a = { 4 /* ACRYLICBLURBEHIND */, 2, 0xEB1A1A1A, 0 };
    WinCompAttrData d = { 19 /* WCA_ACCENT_POLICY */, &a, sizeof a };
    return f && f(h, &d);
}

static void theme(void) {
    DWORD light = 0, sz = sizeof light;
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &light, &sz);
    /* Otto's Theme.Back / Theme.Field, and Theme.Over(44) for the hovered card */
    if (light) { C_BG = RGB(243, 243, 243); C_FIELD = RGB(255, 255, 255); C_SEL = RGB(211, 211, 211); C_TEXT = RGB(0, 0, 0); C_SUB = RGB(96, 96, 96); C_BORDER = RGB(200, 200, 200); }
    else       { C_BG = RGB(26, 26, 26);    C_FIELD = RGB(46, 46, 46);    C_SEL = RGB(66, 66, 66); C_TEXT = RGB(255, 255, 255); C_SUB = RGB(160, 160, 160); C_BORDER = RGB(60, 60, 60); }
    if (!light && g_acrylic) C_BG = C_FIELD = RGB(0, 0, 0); /* black = see-through on acrylic */
    g_bgBrush = CreateSolidBrush(C_BG);
    g_fieldBrush = CreateSolidBrush(C_FIELD);
}

static HFONT font(const wchar_t *face, float px, int weight) {
    return CreateFontW(-PX(px), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, face);
}

/* --bench: build the index and time searches, for measuring without the UI */
static void bench(void) {
    LARGE_INTEGER f, a, b; QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    g_ix = buildIndex();
    QueryPerformanceCounter(&b);
    char out[4096]; int n = 0;
    n += sprintf(out + n, "index: %d entries, %.0f ms, %.1f MB\n", g_ix->n, (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart,
                 (g_ix->sn * 2 + g_ix->cap * sizeof(Entry)) / 1048576.0);
    static const wchar_t *qs[] = { L"c", L"chrome", L"note", L"xyzq", L"pdf", L"rdme", 0 };
    for (int i = 0; qs[i]; i++) {
        SetWindowTextW(g_edit, qs[i]);
        QueryPerformanceCounter(&a);
        for (int k = 0; k < 20; k++) search();
        QueryPerformanceCounter(&b);
        n += sprintf(out + n, "%-8ls %.2f ms  top: %ls\n", qs[i], (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart / 20,
                     g_nrows > 1 && g_rows[0].idx >= 0 ? g_ix->s + g_ix->e[g_rows[0].idx].disp : L"-");
    }
    DWORD w; WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), out, n, &w, NULL);
}

/* --shot "query" out.bmp: render the window offscreen to a bitmap (dev check) */
static void shot(const wchar_t *q, const wchar_t *file) {
    g_ix = buildIndex();
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
    DWORD wr; WriteFile(f, &fh, sizeof fh, &wr, NULL); WriteFile(f, &bi.bmiHeader, sizeof bi.bmiHeader, &wr, NULL);
    WriteFile(f, bits, w * h * 4, &wr, NULL); CloseHandle(f);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmd, int show_) {
    (void)prev; (void)show_;
    int benchMode = wcsstr(cmd, L"--bench") != NULL;
    HANDLE mtx = CreateMutexW(NULL, TRUE, L"SeekLauncherSingleInstance");
    if (!benchMode && !wcsstr(cmd, L"--shot") && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND w = FindWindowW(L"SeekWnd", NULL);
        if (w) { AllowSetForegroundWindow(ASFW_ANY); PostMessageW(w, WM_SHOWME, 0, 0); }
        return 0;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    S = GetDpiForSystem() / 96.0f;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    InitializeCriticalSection(&g_useLock);
    loadUsage();
    g_fEdit = font(L"Segoe UI", 13.33f, FW_NORMAL);   /* 10pt */
    g_fName = font(L"Segoe UI", 13.33f, FW_NORMAL);   /* 10pt */
    g_fSub = font(L"Segoe UI", 11.33f, FW_NORMAL);    /* 8.5pt */
    g_fGlyph = font(L"Segoe MDL2 Assets", 12, FW_NORMAL); /* 9pt */
    g_fIcon = font(L"Segoe MDL2 Assets", 20, FW_NORMAL);
    SHFILEINFOW sfi;
    g_sysil = (HIMAGELIST)SHGetFileInfoW(L"C:\\", 0, &sfi, sizeof sfi, SHGFI_SYSICONINDEX | SHGFI_LARGEICON);
    int ih; ImageList_GetIconSize(g_sysil, &g_iconPx, &ih);

    WNDCLASSW wc = { CS_DROPSHADOW, wndProc, 0, 0, inst, NULL, LoadCursorW(NULL, IDC_ARROW), NULL, NULL, L"SeekWnd" };
    RegisterClassW(&wc);
    g_wnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"SeekWnd", L"Seek", WS_POPUP | WS_CLIPCHILDREN,
                            0, 0, W_WIN, H_FIELD, NULL, NULL, inst, NULL);
    DWORD light = 0, lsz = sizeof light;
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &light, &lsz);
    g_acrylic = !light && acrylic(g_wnd);
    theme();
    int eh = PX(20);
    g_edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                             PX(36), (H_FIELD - eh) / 2, W_WIN - PX(44), eh, g_wnd, NULL, inst, NULL);
    SendMessageW(g_edit, WM_SETFONT, (WPARAM)g_fEdit, 0);
    SendMessageW(g_edit, EM_SETCUEBANNER, TRUE, (LPARAM)L"Search apps and files");
    g_editProc = (WNDPROC)SetWindowLongPtrW(g_edit, GWLP_WNDPROC, (LONG_PTR)editProc);

    if (benchMode) { bench(); return 0; }
    int argc; wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc == 4 && !wcscmp(argv[1], L"--shot")) { shot(argv[2], argv[3]); return 0; }

    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    addTray();
    if (!RegisterHotKey(g_wnd, 1, MOD_ALT | MOD_NOREPEAT, VK_SPACE))
        MessageBoxW(NULL, L"Alt+Space is already taken by another program (PowerToys Run?).\nUse the tray icon to open Seek.", L"Seek", MB_ICONWARNING);
    rescan();

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    (void)mtx;
    return 0;
}
