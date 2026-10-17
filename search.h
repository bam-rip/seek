/* search.h - Seek's search engine: the in-memory index, the folder watcher, matching and the
   calculator. Seek includes it, and so can any other launcher (Silver Styler's Start menu)
   without Seek itself growing: each program compiles in only what it uses.

   The includer sets g_indexWnd to the window that receives WM_INDEX (lParam: a new Index*,
   which it swaps into g_ix and frees the old one), and calls rescan() when it wants a walk. */
#ifndef SEEK_SEARCH_H
#define SEEK_SEARCH_H

#define MAXN      1500000   /* index entries */
#define MAXPATH   32768     /* longest Win32 path, in characters */
#define MAXDEPTH  48        /* folder nesting followed below a root */
#define MAXQUERY  256
#define NONE      0xFFFFFFFFu
#define WM_INDEX  (WM_APP + 1)

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

static Index *g_ix;
static HWND g_indexWnd;
static volatile LONG g_scanning, g_dirty = 1, g_watching;
static DWORD g_lastScan;

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
    if (!ix || !PostMessageW(g_indexWnd, WM_INDEX, 0, (LPARAM)ix)) { freeIndex(ix); InterlockedExchange(&g_scanning, 0); }
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

/* ---------- query ---------- */

/* The best rows for `query` (already trimmed), at most maxRows: a maths answer first (its text
   in calcOut), then apps, settings, folders and files by score, then a web search. */
static int seekQuery(const wchar_t *query, int maxRows, Row *rows, wchar_t *calcOut, int calcCap) {
    wchar_t q[MAXQUERY];
    int m = 0, n = 0;
    for (const wchar_t *s = query; *s && m < MAXQUERY - 1; s++) q[m++] = towlower(*s);
    q[m] = 0;
    if (!m) return 0;
    if (calc(query, calcOut, calcCap)) rows[n++] = (Row){ K_CALC, NONE, 0 };
    Row top[16];
    int nt = 0, lim = maxRows - n - 1;
    if (lim > 16) lim = 16;
    unsigned qmask = charMask(q, m);
    const Index *ix = g_ix;
    for (unsigned i = 0; ix && lim > 0 && i < ix->n; i++) {
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
    for (int i = 0; i < nt; i++) rows[n++] = top[i];
    rows[n++] = (Row){ K_WEB, NONE, 0 };
    return n;
}

#endif
