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
#include <stdlib.h>
#include <wchar.h>
#include <wctype.h>
#pragma comment(lib, "user32")
#pragma comment(lib, "gdi32")
#pragma comment(lib, "shell32")
#pragma comment(lib, "ole32")
#pragma comment(lib, "comctl32")
#pragma comment(lib, "advapi32")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#define SEEK_VERSION L"1.1.0"
#define MAXN      1500000   /* index entries */
#define MAXPATH   32768     /* longest Win32 path, in characters */
#define MAXDEPTH  48        /* folder nesting followed below a root */
#define MAXQUERY  256
#define MAXROWS   8
#define NONE      0xFFFFFFFFu
#define WM_INDEX  (WM_APP + 1)
#define WM_TRAY   (WM_APP + 2)
#define WM_SHOWME (WM_APP + 3)

/* entry kinds up to K_FILE are searchable; K_NODE is a folder kept only so paths can be rebuilt */
enum { K_APP, K_SETTINGS, K_DIR, K_FILE, K_NODE, K_CALC, K_WEB };

typedef struct {
    unsigned parent;                 /* entry index of the containing folder, NONE for a root */
    unsigned name;                   /* offset of the on-disk name; a root holds its full path */
    unsigned lname;                  /* offset of the lowercased shown name (== name when equal) */
    unsigned mask;                   /* charBit() of every character in lname */
    unsigned short nameLen, dispLen; /* dispLen < nameLen for apps, which hide ".lnk" */
    unsigned char kind, uses;
} Entry;
typedef struct { wchar_t *s; unsigned sn, scap; Entry *e; unsigned n, cap; } Index;
typedef struct { int kind; unsigned idx; int score; } Row;

static HWND g_wnd, g_edit;
static WNDPROC g_editProc;
static Index *g_ix;
static Row g_rows[MAXROWS + 2];
static int g_nrows, g_sel;
static wchar_t g_query[MAXQUERY], g_calc[64];
static HFONT g_fText, g_fSub, g_fGlyph, g_fIcon;
static HIMAGELIST g_sysil;
static int g_iconPx;
static float S = 1;
static COLORREF C_BG, C_FIELD, C_SEL, C_TEXT, C_SUB, C_BORDER;
static const COLORREF C_ACCENT = RGB(0, 120, 215);
static HBRUSH g_fieldBrush;
static volatile LONG g_scanning, g_dirty = 1, g_watching;
static DWORD g_lastScan;
static UINT g_taskbarCreated;
static NOTIFYICONDATAW g_nid;

#define PX(v) ((int)((v) * S + 0.5f))
/* sizes match Otto's search box and chat cards (HistoryView.cs) */
#define W_WIN PX(640)
#define H_FIELD PX(44)
#define H_TOP (H_FIELD + PX(4))
#define H_ROW PX(54)
#define GAP PX(4)

/* ---------- small helpers ---------- */

#define FNV_SEED 2166136261u
static unsigned fnv(unsigned h, const wchar_t *s, int n) {
    for (int i = 0; i < n; i++) h = (h ^ towlower(s[i])) * 16777619u;
    return h;
}
static unsigned fnvDone(unsigned h) { return h ? h : 1; } /* 0 marks an empty hash slot */
static unsigned hashPath(const wchar_t *p) { return fnvDone(fnv(FNV_SEED, p, (int)wcslen(p))); }

/* one bit per letter, five for digit pairs, one for anything else: a cheap superset test */
static unsigned charBit(wchar_t c) {
    if (c >= 'a' && c <= 'z') return 1u << (c - 'a');
    if (c >= '0' && c <= '9') return 1u << (26 + (c - '0') % 5);
    return c == ' ' ? 0 : 1u << 31;
}
static unsigned charMask(const wchar_t *s, int n) {
    unsigned m = 0;
    for (int i = 0; i < n; i++) m |= charBit(s[i]);
    return m;
}

static int endsWith(const wchar_t *s, int n, const wchar_t *suf) {
    int m = (int)wcslen(suf);
    return n >= m && !_wcsicmp(s + n - m, suf);
}

static int skipDir(const wchar_t *n) {
    static const wchar_t *skip[] = { L"AppData", L"node_modules", L"$Recycle.Bin", L"__pycache__",
        L"site-packages", L"obj", L"Temp", L"cache", L"Cache", 0 };
    if (n[0] == '.') return 1;
    for (int i = 0; skip[i]; i++) if (!wcscmp(n, skip[i])) return 1;
    return 0;
}

/* ---------- usage: things you open often rank higher ---------- */

#define MAXUSE 512
#define USESLOTS (MAXUSE * 2)
typedef struct { unsigned h; int count; } Use;
static Use g_use[MAXUSE];
static int g_nuse;
static CRITICAL_SECTION g_useLock;

static int usageFile(wchar_t *out, const wchar_t *suffix) {
    if (SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, out) != S_OK) return 0;
    wcscat(out, L"\\Seek");
    CreateDirectoryW(out, NULL);
    wcscat(out, suffix);
    return 1;
}

static void loadUsage(void) {
    wchar_t f[MAX_PATH + 32];
    FILE *fp;
    if (!usageFile(f, L"\\usage.txt") || !(fp = _wfopen(f, L"r"))) return;
    unsigned h; int c;
    while (g_nuse < MAXUSE && fscanf(fp, "%u %d", &h, &c) == 2)
        if (h && c > 0) g_use[g_nuse++] = (Use){ h, c };
    fclose(fp);
}

/* written to a temp file then swapped in, so a crash mid-write never loses the history */
static void saveUsage(void) {
    wchar_t f[MAX_PATH + 32], tmp[MAX_PATH + 32];
    if (!usageFile(f, L"\\usage.txt") || !usageFile(tmp, L"\\usage.tmp")) return;
    FILE *fp = _wfopen(tmp, L"w");
    if (!fp) return;
    for (int i = 0; i < g_nuse; i++) fprintf(fp, "%u %d\n", g_use[i].h, g_use[i].count);
    if (fclose(fp) == 0) MoveFileExW(tmp, f, MOVEFILE_REPLACE_EXISTING);
    else DeleteFileW(tmp);
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
    saveUsage();
    LeaveCriticalSection(&g_useLock);
}

/* ---------- index ---------- */

typedef struct {
    Index *ix;
    Use uses[USESLOTS];   /* open counts by path hash, open addressing */
    wchar_t path[MAXPATH]; /* the folder being walked */
} Build;

static void loadUseTable(Build *b) {
    EnterCriticalSection(&g_useLock);
    for (int i = 0; i < g_nuse; i++) {
        unsigned k = g_use[i].h % USESLOTS;
        while (b->uses[k].h) k = (k + 1) % USESLOTS;
        b->uses[k] = g_use[i];
    }
    LeaveCriticalSection(&g_useLock);
}

static unsigned char usesOf(const Build *b, unsigned h) {
    unsigned k = h % USESLOTS;
    while (b->uses[k].h && b->uses[k].h != h) k = (k + 1) % USESLOTS;
    int c = b->uses[k].h ? b->uses[k].count : 0;
    return (unsigned char)(c > 255 ? 255 : c);
}

static int grow(void **p, unsigned *cap, unsigned need, size_t elem) {
    if (need <= *cap) return 1;
    unsigned c = *cap ? *cap : 4096;
    while (c < need) c *= 2;
    void *q = realloc(*p, (size_t)c * elem);
    if (!q) return 0;
    *p = q; *cap = c;
    return 1;
}

static unsigned putStr(Index *ix, const wchar_t *s, int n, int lower) {
    unsigned at = ix->sn;
    wchar_t *d = ix->s + at;
    for (int i = 0; i < n; i++) d[i] = lower ? towlower(s[i]) : s[i];
    d[n] = 0;
    ix->sn += n + 1;
    return at;
}

/* returns the new entry's index, or NONE when the index is full or memory ran out */
static unsigned add(Build *b, int kind, unsigned parent, const wchar_t *name, int nameLen, int dispLen, unsigned hash) {
    Index *ix = b->ix;
    if (ix->n >= MAXN || !grow((void **)&ix->e, &ix->cap, ix->n + 1, sizeof(Entry)) ||
        !grow((void **)&ix->s, &ix->scap, ix->sn + nameLen + dispLen + 2, sizeof(wchar_t))) return NONE;
    Entry *e = &ix->e[ix->n];
    e->parent = parent;
    e->kind = (unsigned char)kind;
    e->nameLen = (unsigned short)nameLen;
    e->dispLen = (unsigned short)dispLen;
    e->name = putStr(ix, name, nameLen, 0);
    int same = kind == K_NODE || dispLen == nameLen;
    for (int i = 0; same && i < dispLen; i++) same = towlower(name[i]) == name[i];
    e->lname = same ? e->name : putStr(ix, name, dispLen, 1);
    e->mask = kind == K_NODE ? 0 : charMask(ix->s + e->lname, dispLen);
    e->uses = usesOf(b, fnvDone(hash));
    return ix->n++;
}

static unsigned childHash(unsigned h, const wchar_t *name, int n) { return fnv(fnv(h, L"\\", 1), name, n); }

/* the shown length of a Start menu shortcut name, or 0 if it is not a shortcut */
static int shortcutStem(const wchar_t *n, int nl) {
    if (endsWith(n, nl, L".lnk") || endsWith(n, nl, L".url")) return nl - 4;
    if (endsWith(n, nl, L".appref-ms")) return nl - 10;
    return 0;
}

static int haveApp(const Index *ix, const wchar_t *lower, int n) {
    for (unsigned j = 0; j < ix->n; j++)
        if (ix->e[j].kind == K_APP && ix->e[j].dispLen == n && !wcscmp(ix->s + ix->e[j].lname, lower)) return 1;
    return 0;
}

/* apps: walk the Start menu, keeping shortcuts; sub-folders are path-only nodes */
static void scanApps(Build *b, int len, unsigned parent, unsigned hash, int depth) {
    wchar_t *p = b->path;
    if (depth > MAXDEPTH || len + 2 + MAX_PATH >= MAXPATH) return;
    wcscpy(p + len, L"\\*");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(p, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const wchar_t *n = fd.cFileName;
        if (n[0] == '.') continue;
        int nl = (int)wcslen(n);
        unsigned nh = childHash(hash, n, nl);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            unsigned d = add(b, K_NODE, parent, n, nl, nl, nh);
            if (d == NONE) break;
            p[len] = '\\';
            memcpy(p + len + 1, n, (nl + 1) * sizeof(wchar_t));
            scanApps(b, len + 1 + nl, d, nh, depth + 1);
            continue;
        }
        int sl = shortcutStem(n, nl);
        if (!sl) continue;
        wchar_t low[MAX_PATH];
        for (int i = 0; i < sl; i++) low[i] = towlower(n[i]);
        low[sl] = 0;
        if (wcsstr(low, L"uninstall") || haveApp(b->ix, low, sl)) continue;
        if (add(b, K_APP, parent, n, nl, sl, nh) == NONE) break;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void scanFiles(Build *b, int len, unsigned parent, unsigned hash, int depth) {
    wchar_t *p = b->path;
    if (depth > MAXDEPTH || len + 2 + MAX_PATH >= MAXPATH) return;
    wcscpy(p + len, L"\\*");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(p, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const wchar_t *n = fd.cFileName;
        DWORD a = fd.dwFileAttributes;
        if (n[0] == '.' && (!n[1] || (n[1] == '.' && !n[2]))) continue;
        if (a & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
        int isDir = (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
        /* symlinks and junctions can loop; OneDrive folders are reparse points too but are real */
        if (isDir && (skipDir(n) || ((a & FILE_ATTRIBUTE_REPARSE_POINT) &&
            (fd.dwReserved0 == IO_REPARSE_TAG_SYMLINK || fd.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT)))) continue;
        int nl = (int)wcslen(n);
        unsigned nh = childHash(hash, n, nl);
        unsigned d = add(b, isDir ? K_DIR : K_FILE, parent, n, nl, nl, nh);
        if (d == NONE) break;
        if (isDir) {
            p[len] = '\\';
            memcpy(p + len + 1, n, (nl + 1) * sizeof(wchar_t));
            scanFiles(b, len + 1 + nl, d, nh, depth + 1);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

typedef void (*ScanFn)(Build *, int, unsigned, unsigned, int);

static void scanRoot(Build *b, int csidl, ScanFn scan) {
    if (SHGetFolderPathW(NULL, csidl, NULL, 0, b->path) != S_OK) return;
    int len = (int)wcslen(b->path);
    unsigned h = fnv(FNV_SEED, b->path, len);
    unsigned r = add(b, K_NODE, NONE, b->path, len, len, h);
    if (r != NONE) scan(b, len, r, h, 0);
}

static void freeIndex(Index *ix) { if (ix) { free(ix->s); free(ix->e); free(ix); } }

static Index *buildIndex(void) {
    Index *ix = calloc(1, sizeof *ix);
    Build *b = calloc(1, sizeof *b);
    if (!ix || !b) { free(ix); free(b); return NULL; }
    b->ix = ix;
    loadUseTable(b);
    scanRoot(b, CSIDL_PROGRAMS, scanApps);
    scanRoot(b, CSIDL_COMMON_PROGRAMS, scanApps);
    add(b, K_SETTINGS, NONE, L"Settings", 8, 8, fnv(FNV_SEED, L"ms-settings:", 12));
    scanRoot(b, CSIDL_PROFILE, scanFiles);
    free(b);
    /* give back the slack from doubling; shrinking cannot fail in practice, but keep the old block if it does */
    void *e = realloc(ix->e, (ix->n ? ix->n : 1) * sizeof(Entry)), *s = realloc(ix->s, (ix->sn ? ix->sn : 1) * sizeof(wchar_t));
    if (e) { ix->e = e; ix->cap = ix->n; }
    if (s) { ix->s = s; ix->scap = ix->sn; }
    return ix;
}

/* writes entry i's full path into out; returns its length, or -1 if it does not fit */
static int pathOf(const Index *ix, unsigned i, wchar_t *out, int cap) {
    if (ix->e[i].kind == K_SETTINGS) { wcscpy(out, L"ms-settings:"); return 12; }
    unsigned chain[MAXDEPTH + 8];
    int n = 0, len = 0;
    for (unsigned j = i; j != NONE; j = ix->e[j].parent) {
        if (n == MAXDEPTH + 8) return -1;
        chain[n++] = j;
    }
    while (n--) {
        const Entry *e = &ix->e[chain[n]];
        if (len + 1 + e->nameLen >= cap) return -1;
        if (len) out[len++] = '\\';
        memcpy(out + len, ix->s + e->name, e->nameLen * sizeof(wchar_t));
        len += e->nameLen;
    }
    out[len] = 0;
    return len;
}

static DWORD WINAPI indexThread(void *arg) {
    (void)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    Index *ix = buildIndex();
    if (!ix || !PostMessageW(g_wnd, WM_INDEX, 0, (LPARAM)ix)) { freeIndex(ix); InterlockedExchange(&g_scanning, 0); }
    return 0;
}

static void rescan(void) {
    if (InterlockedCompareExchange(&g_scanning, 1, 0)) return;
    InterlockedExchange(&g_dirty, 0); /* changes made during the walk mark it dirty again */
    g_lastScan = GetTickCount();
    HANDLE t = CreateThread(NULL, 0, indexThread, NULL, 0, NULL);
    if (t) CloseHandle(t); else InterlockedExchange(&g_scanning, 0);
}

/* ---------- watcher: notes when the profile or Start menu changes ---------- */

/* a change matters unless it sits inside a folder the index skips */
static int relevant(const wchar_t *rel, int n) {
    wchar_t part[MAX_PATH];
    for (int i = 0, j; i < n; i = j + 1) {
        for (j = i; j < n && rel[j] != '\\'; j++) {}
        if (j == n) return 1; /* the changed item itself */
        int k = j - i < MAX_PATH - 1 ? j - i : MAX_PATH - 1;
        memcpy(part, rel + i, k * sizeof(wchar_t));
        part[k] = 0;
        if (skipDir(part)) return 0;
    }
    return 1;
}

static DWORD WINAPI watchThread(void *arg) {
    (void)arg;
    static DWORD buf[16384]; /* 64 KB, DWORD-aligned as ReadDirectoryChangesW requires */
    wchar_t p[MAX_PATH];
    HANDLE ev[3]; int nev = 0;
    OVERLAPPED ov = { 0 };
    if (SHGetFolderPathW(NULL, CSIDL_PROFILE, NULL, 0, p) != S_OK) return 0;
    HANDLE dir = CreateFileW(p, FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                             OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, NULL);
    if (dir == INVALID_HANDLE_VALUE) return 0;
    if (!(ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL))) { CloseHandle(dir); return 0; }
    ev[nev++] = ov.hEvent;
    /* the user Start menu lives under AppData, which the profile watch ignores */
    static const int menus[] = { CSIDL_PROGRAMS, CSIDL_COMMON_PROGRAMS };
    for (int i = 0; i < 2; i++) {
        if (SHGetFolderPathW(NULL, menus[i], NULL, 0, p) != S_OK) continue;
        HANDLE c = FindFirstChangeNotificationW(p, TRUE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME);
        if (c != INVALID_HANDLE_VALUE) ev[nev++] = c;
    }
    InterlockedExchange(&g_watching, 1);
    const DWORD what = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME;
    while (ReadDirectoryChangesW(dir, buf, sizeof buf, TRUE, what, NULL, &ov, NULL)) {
        DWORD w;
        while ((w = WaitForMultipleObjects(nev, ev, FALSE, INFINITE) - WAIT_OBJECT_0) > 0 && w < (DWORD)nev) {
            InterlockedExchange(&g_dirty, 1);
            FindNextChangeNotification(ev[w]);
        }
        DWORD got;
        if (w != 0 || !GetOverlappedResult(dir, &ov, &got, FALSE)) break;
        ResetEvent(ov.hEvent);
        if (!got) { InterlockedExchange(&g_dirty, 1); continue; } /* overflowed: assume it mattered */
        for (FILE_NOTIFY_INFORMATION *f = (FILE_NOTIFY_INFORMATION *)buf;; f = (FILE_NOTIFY_INFORMATION *)((BYTE *)f + f->NextEntryOffset)) {
            if (relevant(f->FileName, f->FileNameLength / sizeof(wchar_t))) { InterlockedExchange(&g_dirty, 1); break; }
            if (!f->NextEntryOffset) break;
        }
    }
    InterlockedExchange(&g_watching, 0); /* show() falls back to a timer */
    return 0;
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

/* recursive descent: expr = term {+|- term}; term = unary {*|/|%|x|implicit unary};
   unary = {+|-} power; power = postfix [^|** unary]; postfix = primary {!} */
typedef struct { const wchar_t *p; int err, ops, depth, trig; } Calc;
typedef struct { const wchar_t *name; double (*f1)(double); double (*f2)(double, double); int trig; } Func;

static double c_fact(double v) { return tgamma(v + 1); }
static double c_logBase(double x, double base) { return log(x) / log(base); }
static double c_sqrt(double v) { return sqrt(v); }
static double c_abs(double v) { return fabs(v); }
static double c_exp(double v) { return exp(v); }
static double c_ln(double v) { return log(v); }
static double c_log10(double v) { return log10(v); }
static double c_sin(double v) { return sin(v); }
static double c_cos(double v) { return cos(v); }
static double c_tan(double v) { return tan(v); }
static double c_asin(double v) { return asin(v); }
static double c_acos(double v) { return acos(v); }
static double c_atan(double v) { return atan(v); }
static double c_sinh(double v) { return sinh(v); }
static double c_cosh(double v) { return cosh(v); }
static double c_tanh(double v) { return tanh(v); }
static double c_floor(double v) { return floor(v); }
static double c_ceil(double v) { return ceil(v); }
static double c_pow(double a, double b) { return pow(a, b); }
static double c_mod(double a, double b) { return fmod(a, b); }
static double c_atan2(double a, double b) { return atan2(a, b); }

static const Func FUNCS[] = {
    { L"sqrt", c_sqrt }, { L"cbrt", cbrt }, { L"abs", c_abs }, { L"exp", c_exp },
    { L"ln", c_ln }, { L"log", c_log10, c_logBase }, { L"log10", c_log10 }, { L"log2", log2 },
    { L"sin", c_sin, 0, 1 }, { L"cos", c_cos, 0, 1 }, { L"tan", c_tan, 0, 1 },
    { L"asin", c_asin }, { L"acos", c_acos }, { L"atan", c_atan, c_atan2 },
    { L"sinh", c_sinh }, { L"cosh", c_cosh }, { L"tanh", c_tanh },
    { L"floor", c_floor }, { L"ceil", c_ceil }, { L"round", round }, { L"trunc", trunc },
    { L"fact", c_fact }, { L"gamma", tgamma },
    { L"pow", 0, c_pow }, { L"mod", 0, c_mod }, { L"min", 0, fmin }, { L"max", 0, fmax }, { L"hypot", 0, hypot },
};
static const struct { const wchar_t *name; double v; } CONSTS[] = {
    { L"pi", 3.14159265358979323846 }, { L"tau", 6.28318530717958647692 },
    { L"e", 2.71828182845904523536 }, { L"phi", 1.61803398874989484820 },
};

static double cExpr(Calc *c);
static double cUnary(Calc *c);
static int cAt(Calc *c, wchar_t ch) { while (*c->p == ' ') c->p++; return *c->p == ch; }
static int cEat(Calc *c, wchar_t ch) { if (!cAt(c, ch)) return 0; c->p++; return 1; }
static double cFail(Calc *c) { c->err = 1; return 0; }
static void cClose(Calc *c) { if (!cEat(c, ')') && *c->p) c->err = 1; } /* a missing ")" at the end is fine */

static double cName(Calc *c) {
    const wchar_t *s = c->p;
    int n = 0;
    while (iswalnum(s[n])) n++;
    c->p += n;
    for (int i = 0; i < (int)(sizeof CONSTS / sizeof *CONSTS); i++)
        if ((int)wcslen(CONSTS[i].name) == n && !wcsncmp(s, CONSTS[i].name, n)) return CONSTS[i].v;
    for (int i = 0; i < (int)(sizeof FUNCS / sizeof *FUNCS); i++) {
        const Func *f = &FUNCS[i];
        if ((int)wcslen(f->name) != n || wcsncmp(s, f->name, n)) continue;
        if (!cEat(c, '(')) return cFail(c);
        double a = cExpr(c), r;
        if (cEat(c, ',')) { double b = cExpr(c); if (!f->f2) return cFail(c); r = f->f2(a, b); }
        else { if (!f->f1) return cFail(c); r = f->f1(a); }
        cClose(c);
        c->ops++;
        c->trig |= f->trig;
        return r;
    }
    return cFail(c);
}

static double cPrimary(Calc *c) {
    if (cEat(c, '(')) { double v = cExpr(c); cClose(c); return v; }
    if (*c->p == 0x3C0) { c->p++; return CONSTS[0].v; } /* pi */
    if (iswalpha(*c->p)) return cName(c);
    if (!iswdigit(*c->p) && *c->p != '.') return cFail(c);
    wchar_t *end;
    double v = wcstod(c->p, &end);
    if (end == c->p) return cFail(c);
    c->p = end;
    return v;
}

static double cPostfix(Calc *c) {
    double v = cPrimary(c);
    while (!c->err && cEat(c, '!')) { v = c_fact(v); c->ops++; }
    return v;
}

static double cPower(Calc *c) {
    double b = cPostfix(c);
    if (cAt(c, '^')) c->p++;
    else if (cAt(c, '*') && c->p[1] == '*') c->p += 2;
    else return b;
    c->ops++;
    return pow(b, cUnary(c)); /* right-associative: 2^3^2 = 2^9 */
}

static double cUnary(Calc *c) {
    if (c->err || ++c->depth > 64) return cFail(c);
    double v = cEat(c, '-') || cEat(c, 0x2212) ? -cUnary(c) : cEat(c, '+') ? cUnary(c) : cPower(c);
    c->depth--;
    return v;
}

static double cTerm(Calc *c) {
    double v = cUnary(c);
    while (!c->err) {
        cAt(c, 0);
        wchar_t o = *c->p;
        if (o == '*' || o == '/' || o == '%' || o == 0xD7 || o == 0xF7 || (o == 'x' && !iswalpha(c->p[1]))) c->p++;
        else if (o == '(' || o == 0x3C0 || iswalpha(o)) o = '*'; /* implicit: 2pi, 3(4+1), 2sqrt(2) */
        else break;
        double r = cUnary(c);
        c->ops++;
        v = o == '/' || o == 0xF7 ? v / r : o == '%' ? fmod(v, r) : v * r;
    }
    return v;
}

static double cExpr(Calc *c) {
    double v = cTerm(c);
    while (!c->err) {
        if (cEat(c, '+')) v += cTerm(c);
        else if (cEat(c, '-') || cEat(c, 0x2212)) v -= cTerm(c);
        else break;
        c->ops++;
    }
    return v;
}

/* fills out with the answer when q is a maths expression; plain numbers and words are not */
static int calc(const wchar_t *q, wchar_t *out, int cap) {
    int hint = 0;
    if (*q == '=') q++;
    for (const wchar_t *s = q; *s; s++) {
        if (iswdigit(*s) || *s == '(') hint = 1;
        else if (!iswalpha(*s) && !wcschr(L" .+-*/%^!(),\xD7\xF7\x3C0\x2212", *s)) return 0;
    }
    if (!hint) return 0;
    Calc c = { q };
    double v = cExpr(&c);
    if (c.err || !c.ops || !cAt(&c, 0) || !isfinite(v)) return 0;
    if (c.trig && fabs(v) < 1e-12) v = 0; /* sin(pi) is 1e-16, show 0 */
    if (v == 0) v = 0;                     /* no "-0" */
    if (fabs(v) < 1e15 && v == floor(v)) swprintf(out, cap, L"%.0f", v);
    else swprintf(out, cap, L"%.12g", v);
    return 1;
}

/* ---------- search ---------- */

static void layout(void);

static void search(void) {
    wchar_t raw[MAXQUERY], q[MAXQUERY];
    int n = GetWindowTextW(g_edit, raw, MAXQUERY), s0 = 0, m = 0;
    while (n && raw[n - 1] == ' ') raw[--n] = 0;
    while (raw[s0] == ' ') s0++;
    wcscpy(g_query, raw + s0);
    for (const wchar_t *s = g_query; *s; s++) q[m++] = towlower(*s);
    q[m] = 0;
    g_nrows = g_sel = 0;
    if (m) {
        if (calc(g_query, g_calc, 64)) g_rows[g_nrows++] = (Row){ K_CALC, NONE, 0 };
        Row top[MAXROWS];
        int nt = 0, lim = MAXROWS - g_nrows - 1;
        unsigned qmask = charMask(q, m);
        const Index *ix = g_ix;
        for (unsigned i = 0; ix && i < ix->n; i++) {
            const Entry *e = &ix->e[i];
            if (e->kind > K_FILE || (e->mask & qmask) != qmask) continue;
            int sc = score(ix->s + e->lname, e->dispLen, q, m, e->kind <= K_SETTINGS);
            if (sc < 0) continue;
            sc += (e->kind <= K_SETTINGS ? 600 : e->kind == K_DIR ? 30 : 0) + (e->uses > 20 ? 20 : e->uses) * 40;
            if (nt == lim && sc <= top[nt - 1].score) continue;
            int j = nt < lim ? nt++ : nt - 1;
            while (j > 0 && top[j - 1].score < sc) { top[j] = top[j - 1]; j--; }
            top[j] = (Row){ e->kind, i, sc };
        }
        for (int i = 0; i < nt; i++) g_rows[g_nrows++] = top[i];
        g_rows[g_nrows++] = (Row){ K_WEB, NONE, 0 };
    }
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

static void paint(HDC dc, const RECT *cr) {
    fill(dc, 0, 0, cr->right, cr->bottom, C_BORDER);
    fill(dc, 1, 1, cr->right - 1, cr->bottom - 1, C_BG);
    /* search field: Otto's FieldBox with the focused accent outline */
    fill(dc, 0, 0, cr->right, H_FIELD, C_ACCENT);
    fill(dc, 1, 1, cr->right - 1, H_FIELD - 1, C_FIELD);
    SetBkMode(dc, TRANSPARENT);
    RECT gr = { PX(4), 0, PX(34), H_FIELD };
    glyph(dc, g_fGlyph, &gr, L"\xE721", C_SUB);
    static wchar_t buf[MAXPATH];
    for (int i = 0; i < g_nrows; i++) {
        const Row *row = &g_rows[i];
        int y = H_TOP + i * (H_ROW + GAP), l = PX(4), r = cr->right - PX(4);
        if (i == g_sel) {
            fill(dc, l, y, r, y + H_ROW, C_SEL);
            fill(dc, l, y, l + PX(3), y + H_ROW, C_ACCENT);
        }
        RECT ir = { l + PX(8), y, l + PX(8) + g_iconPx, y + H_ROW };
        int iy = y + (H_ROW - g_iconPx) / 2, nameLen = -1;
        const wchar_t *name, *sub;
        if (row->kind == K_CALC) {
            glyph(dc, g_fIcon, &ir, L"\xE8EF", C_TEXT);
            name = g_calc; sub = L"Calculator \x2014 Enter to copy";
        } else if (row->kind == K_WEB) {
            ImageList_Draw(g_sysil, webIcon(), dc, ir.left, iy, ILD_TRANSPARENT);
            swprintf(buf, MAXPATH, L"Search the web for \x201C%s\x201D", g_query);
            name = buf; sub = L"Opens in your browser";
        } else {
            const Entry *e = &g_ix->e[row->idx];
            if (e->kind == K_SETTINGS) glyph(dc, g_fIcon, &ir, L"\xE713", C_TEXT);
            else ImageList_Draw(g_sysil, iconOf(row->idx), dc, ir.left, iy, ILD_TRANSPARENT);
            name = g_ix->s + e->name; nameLen = e->dispLen;
            sub = e->kind <= K_SETTINGS ? L"App" : pathOf(g_ix, e->parent, buf, MAXPATH) >= 0 ? buf : L"";
        }
        int tx = ir.right + PX(12);
        RECT tr = { tx, y + PX(8), r - PX(12), y + PX(28) };
        SelectObject(dc, g_fText); SetTextColor(dc, C_TEXT);
        DrawTextW(dc, name, nameLen, &tr, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        RECT sr = { tx, y + PX(30), r - PX(12), y + PX(48) };
        SelectObject(dc, g_fSub); SetTextColor(dc, C_SUB);
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

/* follows the Windows app light/dark setting; called at startup and on WM_SETTINGCHANGE */
static void theme(void) {
    DWORD light = 0, sz = sizeof light;
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &light, &sz);
    int blur = acrylic(g_wnd, !light) && !light;
    /* Otto's Theme.Back / Theme.Field, and Theme.Over(44) for the hovered card */
    if (light) { C_BG = RGB(243, 243, 243); C_FIELD = RGB(255, 255, 255); C_SEL = RGB(211, 211, 211); C_TEXT = RGB(0, 0, 0); C_SUB = RGB(96, 96, 96); C_BORDER = RGB(200, 200, 200); }
    else       { C_BG = RGB(26, 26, 26);    C_FIELD = RGB(46, 46, 46);    C_SEL = RGB(66, 66, 66); C_TEXT = RGB(255, 255, 255); C_SUB = RGB(160, 160, 160); C_BORDER = RGB(60, 60, 60); }
    if (blur) C_BG = C_FIELD = RGB(0, 0, 0); /* black = see-through on acrylic */
    if (g_fieldBrush) DeleteObject(g_fieldBrush);
    g_fieldBrush = CreateSolidBrush(C_FIELD);
}

/* ---------- window ---------- */

static int rowAt(LPARAM lp) {
    int y = GET_Y_LPARAM(lp);
    int r = y > H_TOP ? (y - H_TOP) / (H_ROW + GAP) : -1;
    return r < g_nrows ? r : -1;
}

static LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
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
        if (lp && !wcscmp((const wchar_t *)lp, L"ImmersiveColorSet")) {
            theme();
            InvalidateRect(h, NULL, TRUE);
            InvalidateRect(g_edit, NULL, TRUE);
        }
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
    g_fText = font(L"Segoe UI", 13.33f);            /* 10pt */
    g_fSub = font(L"Segoe UI", 11.33f);             /* 8.5pt */
    g_fGlyph = font(L"Segoe MDL2 Assets", 12);      /* 9pt */
    g_fIcon = font(L"Segoe MDL2 Assets", 20);
    SHFILEINFOW sfi;
    g_sysil = (HIMAGELIST)SHGetFileInfoW(L"C:\\", 0, &sfi, sizeof sfi, SHGFI_SYSICONINDEX | SHGFI_LARGEICON);
    int ih; ImageList_GetIconSize(g_sysil, &g_iconPx, &ih);

    WNDCLASSW wc = { 0, wndProc, 0, 0, inst, NULL, LoadCursorW(NULL, IDC_ARROW), NULL, NULL, L"SeekWnd" };
    RegisterClassW(&wc);
    g_wnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"SeekWnd", L"Seek", WS_POPUP | WS_CLIPCHILDREN,
                            0, 0, W_WIN, H_FIELD, NULL, NULL, inst, NULL);
    theme();
    int eh = PX(20);
    g_edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                             PX(36), (H_FIELD - eh) / 2, W_WIN - PX(44), eh, g_wnd, NULL, inst, NULL);
    SendMessageW(g_edit, WM_SETFONT, (WPARAM)g_fText, 0);
    SendMessageW(g_edit, EM_SETCUEBANNER, TRUE, (LPARAM)L"Search apps and files");
    SendMessageW(g_edit, EM_SETLIMITTEXT, MAXQUERY - 1, 0);
    g_editProc = (WNDPROC)SetWindowLongPtrW(g_edit, GWLP_WNDPROC, (LONG_PTR)editProc);

    if (!wcscmp(flag, L"--bench")) { bench(); return 0; }
    if (argc == 4 && !wcscmp(flag, L"--shot")) { shot(argv[2], argv[3]); return 0; }
    LocalFree(argv);

    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    addTray();
    if (!RegisterHotKey(g_wnd, 1, MOD_ALT | MOD_NOREPEAT, VK_SPACE))
        MessageBoxW(NULL, L"Alt+Space is already taken by another program (PowerToys Run?).\nUse the tray icon to open Seek.", L"Seek", MB_ICONWARNING);
    HANDLE t = CreateThread(NULL, 64 * 1024, watchThread, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (t) CloseHandle(t);
    rescan();

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
}
