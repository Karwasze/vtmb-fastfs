/*
 * vtmb-fastfs: file-lookup index for Vampire: The Masquerade - Bloodlines.
 *
 * The Source-era filesystem in FileSystem_Stdio.dll checks for a loose copy
 * of every asset in each search path (Unofficial_Patch\, Vampire\) before
 * falling back to the VPK archives. A single hub load makes ~29,000 failed
 * CreateFileA calls (mostly repeats, or inside folders that don't exist)
 * plus a FindFirstFileA "stat" for every successful open.
 *
 * At game start this plugin indexes every file and folder under the game
 * folder (in a background thread, before it activates). It then patches
 * FileSystem_Stdio.dll's import table so that read-only lookups are answered
 * from the index:
 *   - a file that isn't in the index fails instantly (FILE/PATH_NOT_FOUND);
 *   - a file that is in the index is opened by Windows as usual;
 *   - a "stat" (FindFirstFileA) of an indexed file returns the indexed info.
 * The index is kept up to date for files the filesystem itself writes,
 * creates, moves or deletes. Writable or unusual locations (save\, cfg\,
 * python\, maps\graphs\, screenshots\, folders whose names can't be
 * represented in the ANSI code page, ...) are never answered from the index.
 *
 * If indexing fails or the folder is unexpectedly large, it falls back to
 * learning misses as they happen (still correct, only less effective on the
 * first visit to an area).
 *
 * Built freestanding (no CRT) for i686 Windows; loaded by the Unofficial
 * Patch loader from Bin\loader\*.vtm. Settings: Bin\loader\vtmb-fastfs.ini.
 */

typedef unsigned long  DWORD;
typedef int            BOOL;
typedef void          *HANDLE;
typedef void          *HMODULE;
typedef const char    *LPCSTR;
typedef unsigned short WORD;
typedef unsigned char  BYTE;
#ifndef WINAPI
#define WINAPI __stdcall
#endif
#define INVALID_HANDLE_VALUE ((HANDLE)(long)-1)
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#define FILE_ATTRIBUTE_DIRECTORY 0x10

typedef struct { DWORD lo, hi; } FILETIME;
typedef struct {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow, dwReserved0, dwReserved1;
    char cFileName[260];
    char cAlternateFileName[14];
} WIN32_FIND_DATAA;
typedef struct { BYTE opaque[24]; } CRITICAL_SECTION;

#ifndef FASTFS_HOST_TEST
__declspec(dllimport) HANDLE WINAPI CreateFileA(LPCSTR, DWORD, DWORD, void *, DWORD, DWORD, HANDLE);
__declspec(dllimport) HANDLE WINAPI FindFirstFileA(LPCSTR, WIN32_FIND_DATAA *);
__declspec(dllimport) BOOL   WINAPI FindNextFileA(HANDLE, WIN32_FIND_DATAA *);
__declspec(dllimport) BOOL   WINAPI FindClose(HANDLE);
__declspec(dllimport) DWORD  WINAPI GetFileAttributesA(LPCSTR);
__declspec(dllimport) DWORD  WINAPI GetLastError(void);
__declspec(dllimport) void   WINAPI SetLastError(DWORD);
__declspec(dllimport) HANDLE WINAPI HeapCreate(DWORD, DWORD, DWORD);
__declspec(dllimport) BOOL   WINAPI HeapDestroy(HANDLE);
__declspec(dllimport) void * WINAPI HeapAlloc(HANDLE, DWORD, DWORD);
__declspec(dllimport) HMODULE WINAPI GetModuleHandleA(LPCSTR);
__declspec(dllimport) DWORD  WINAPI GetModuleFileNameA(HMODULE, char *, DWORD);
__declspec(dllimport) BOOL   WINAPI VirtualProtect(void *, DWORD, DWORD, DWORD *);
__declspec(dllimport) HANDLE WINAPI CreateThread(void *, DWORD, DWORD (WINAPI *)(void *), void *, DWORD, DWORD *);
__declspec(dllimport) BOOL   WINAPI CloseHandle(HANDLE);
__declspec(dllimport) void   WINAPI Sleep(DWORD);
__declspec(dllimport) void   WINAPI InitializeCriticalSection(CRITICAL_SECTION *);
__declspec(dllimport) void   WINAPI EnterCriticalSection(CRITICAL_SECTION *);
__declspec(dllimport) void   WINAPI LeaveCriticalSection(CRITICAL_SECTION *);
__declspec(dllimport) unsigned WINAPI GetPrivateProfileIntA(LPCSTR, LPCSTR, int, LPCSTR);
__declspec(dllimport) BOOL   WINAPI WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
__declspec(dllimport) DWORD  WINAPI GetTickCount(void);
__declspec(dllimport) const char * WINAPI GetCommandLineA(void);
__declspec(dllimport) DWORD  WINAPI GetCurrentThreadId(void);
__declspec(dllimport) void * WINAPI VirtualAlloc(void *, DWORD, DWORD, DWORD);
__declspec(dllimport) BOOL   WINAPI MoveFileExA(LPCSTR, LPCSTR, DWORD);
#else
#include "host_mock.h"
#endif

/* ---- freestanding helpers ---- */
#ifndef FASTFS_HOST_TEST
/* rep stosb / rep movsb: fast on every CPU since 2012 (ERMSB), and never turned back into a call */
void *memset(void *d, int c, unsigned n) { void *r = d; __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory"); return r; }
void *memcpy(void *d, const void *s, unsigned n) { void *r = d; __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory"); return r; }
#endif
static void cpy(void *d, const void *s, unsigned n) { BYTE *p = d; const BYTE *q = s; while (n--) *p++ = *q++; }
static unsigned slen(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static int contains(const char *h, const char *n)
{
    unsigned ln = slen(n);
    for (; *h; h++) {
        unsigned i = 0;
        while (i < ln && h[i] == n[i]) i++;
        if (i == ln) return 1;
    }
    return 0;
}
static int ends_with(const char *s, const char *suf)
{
    unsigned a = slen(s), b = slen(suf), i;
    if (b > a) return 0;
    for (i = 0; i < b; i++) if (s[a - b + i] != suf[i]) return 0;
    return 1;
}

/* ---- state ---- */
#define MAXP 520
#define MAX_INDEX_ENTRIES 400000

enum { MODE_OFF = 0, MODE_LEARN = 1, MODE_INDEX = 2 };
static volatile int g_mode;
static char  g_root[MAXP];     /* lowercased game folder, with trailing '\' */
static unsigned g_rootlen;
static CRITICAL_SECTION g_cs;
static HANDLE g_heap;

static struct {
    DWORD miss_hits, find_hits, real_opens, real_finds, passthrough,
          dirs_probed, flushes, idx_files, idx_dirs, idx_untrusted, scan_ms, scan_aborted;
} st;

/* entry types */
enum { T_FILE = 1, T_DIR = 2, T_MISSING = 3, T_PROBE = 4 };
/* T_FILE val */
enum { F_PRESENT = 0, F_GONE = 1 };
/* T_DIR val (index mode) */
enum { D_TRUSTED = 1, D_UNTRUSTED = 2, D_GONE = 3 };

typedef struct {
    DWORD attrs;
    FILETIME c, a, w;
    DWORD szhi, szlo;
    char *name;          /* original-case file name */
} FileInfo;

typedef struct {
    unsigned h;
    char *key;
    BYTE type;
    DWORD val;
    FileInfo *fi;        /* T_FILE: info for stat, NULL = ask Windows */
} Entry;

static unsigned long long g_sig = 1469598103934665603ULL;
static void sig_add(const void *p, unsigned n) { const BYTE *b = p; while (n--) { g_sig ^= *b++; g_sig *= 1099511628211ULL; } }
/* Diagnostic list of what went into the signature (vtmb-fastfs-sig.txt). */
static char *g_man; static unsigned g_manlen, g_mancap;
static void man_put(const char *s, unsigned n)
{
    if (!g_man || g_manlen + n > g_mancap) return;
    while (n--) g_man[g_manlen++] = *s++;
}
static void man_hex(DWORD v)
{
    char t[8]; int i;
    for (i = 7; i >= 0; i--) { t[i] = "0123456789abcdef"[v & 15]; v >>= 4; }
    man_put(t, 8);
}

static Entry *g_tab;
static unsigned g_cap, g_count;

static void *halloc(unsigned n) { return HeapAlloc(g_heap, 8 /*HEAP_ZERO_MEMORY*/, n); }

static void table_reset(void)
{
    if (g_heap) HeapDestroy(g_heap);
    g_heap = HeapCreate(0, 1 << 20, 0);
    g_cap = 1 << 14;
    g_count = 0;
    g_tab = g_heap ? (Entry *)halloc(g_cap * sizeof(Entry)) : 0;
}

static unsigned hash(const char *k, BYTE type)
{
    unsigned h = 2166136261u ^ type;
    while (*k) { h ^= (BYTE)*k++; h *= 16777619u; }
    return h ? h : 1;
}

static int keyeq(const char *a, const char *b) { while (*a && *a == *b) a++, b++; return !*a && !*b; }

static Entry *table_find(const char *key, BYTE type)
{
    unsigned h = hash(key, type), i;
    if (!g_tab) return 0;
    for (i = h & (g_cap - 1);; i = (i + 1) & (g_cap - 1)) {
        Entry *e = &g_tab[i];
        if (!e->h) return 0;
        if (e->h == h && e->type == type && keyeq(e->key, key)) return e;
    }
}

static int table_grow(void)
{
    Entry *old = g_tab, *nt;
    unsigned oldcap = g_cap, i;
    nt = (Entry *)halloc(oldcap * 2 * sizeof(Entry));
    if (!nt) return 0;
    g_tab = nt;
    g_cap = oldcap * 2;
    for (i = 0; i < oldcap; i++) {
        unsigned j;
        if (!old[i].h) continue;
        for (j = old[i].h & (g_cap - 1); g_tab[j].h; j = (j + 1) & (g_cap - 1)) {}
        g_tab[j] = old[i];
    }
    return 1;   /* the old array stays in the private heap until the next reset */
}

/* Returns the entry for key (creating it zeroed if new), or NULL on OOM. */
static Entry *table_put(const char *key, BYTE type)
{
    unsigned h, i, n;
    Entry *e;
    if (!g_tab) return 0;
    if ((g_count + 1) * 10 > g_cap * 7 && !table_grow()) return 0;
    h = hash(key, type);
    for (i = h & (g_cap - 1);; i = (i + 1) & (g_cap - 1)) {
        e = &g_tab[i];
        if (!e->h) break;
        if (e->h == h && e->type == type && keyeq(e->key, key)) return e;
    }
    n = slen(key) + 1;
    e->key = (char *)halloc(n);
    if (!e->key) return 0;
    cpy(e->key, key, n);
    e->h = h;
    e->type = type;
    e->val = 0;
    e->fi = 0;
    g_count++;
    return e;
}

/* Lowercase, '/' -> '\', squash repeated separators (not a leading "\\").
 * Returns 0 if too long or contains non-ASCII (never cached). */
static int normalize(const char *in, char *out)
{
    unsigned n = 0;
    char prev = 0;
    for (; *in; in++) {
        char c = *in;
        if ((BYTE)c >= 0x80) return 0;
        if (c == '/') c = '\\';
        if (c >= 'A' && c <= 'Z') c += 32;
        if (c == '\\' && prev == '\\' && n > 1) continue;
        if (n >= MAXP - 2) return 0;
        out[n++] = c;
        prev = c;
    }
    out[n] = 0;
    return 1;
}

/* Paths we answer from the cache/index: plain read lookups under the game
 * folder, outside every location the game (or Python) writes to. */
/* Windows device names (nul, con, aux, prn, com1-9, lpt1-9), with or without
 * an extension: writes to them don't touch the disk. */
static int is_device_name(const char *k)
{
    const char *b = k, *p;
    unsigned n;
    for (p = k; *p; p++) if (*p == '\\') b = p + 1;
    for (n = 0; b[n] && b[n] != '.'; n++) {}
    if (n == 3 && ((b[0]=='n'&&b[1]=='u'&&b[2]=='l') || (b[0]=='c'&&b[1]=='o'&&b[2]=='n') ||
                   (b[0]=='a'&&b[1]=='u'&&b[2]=='x') || (b[0]=='p'&&b[1]=='r'&&b[2]=='n'))) return 1;
    if (n == 4 && b[3] >= '1' && b[3] <= '9' &&
        ((b[0]=='c'&&b[1]=='o'&&b[2]=='m') || (b[0]=='l'&&b[1]=='p'&&b[2]=='t'))) return 1;
    return 0;
}

static int comp_is(const char *c, unsigned n, const char *w)
{
    unsigned i;
    for (i = 0; i < n; i++) if (!w[i] || c[i] != w[i]) return 0;
    return !w[n];
}
/* One pass over the path after the game root; same rules as the original
   list of substring checks (kept in cacheable_slow for the tests). */
static int cacheable(const char *k)
{
    unsigned i, n;
    const char *p, *c, *prev = 0;
    unsigned prevn = 0;
    if (!g_rootlen) return 0;
    for (i = 0; i < g_rootlen; i++) if (k[i] != g_root[i]) return 0;
    if (!k[g_rootlen]) return 0;                       /* the root itself */
    if (is_device_name(k)) return 0;
    p = k + g_rootlen;
    c = p;
    for (;; p++) {
        char ch = *p;
        if (ch == '*' || ch == '?' || ch == '~' || ch == ':') return 0;
        if (ch == '\\' || !ch) {
            n = (unsigned)(p - c);
            if ((n == 1 && c[0] == '.') || (n >= 2 && c[0] == '.' && c[1] == '.')) return 0;
            if (comp_is(c, n, "save") || comp_is(c, n, "cfg") || comp_is(c, n, "python") ||
                comp_is(c, n, "screenshots") || comp_is(c, n, "materialsrc") || comp_is(c, n, "procmon") ||
                comp_is(c, n, "logs")) return 0;
            if (prev && comp_is(prev, prevn, "maps") && comp_is(c, n, "graphs")) return 0;
            if (prev && comp_is(prev, prevn, "vdata") && comp_is(c, n, "hackterminals")) return 0;
            if (!ch) break;
            prev = c; prevn = n; c = p + 1;
        }
    }
    n = (unsigned)(p - k);                              /* length; p is at the terminator */
    if (k[n - 1] == '\\' || k[n - 1] == '.' || k[n - 1] == ' ') return 0;
    if (n >= 4 && k[n - 4] == '.') {
        const char *e = k + n - 3;
        static const char ext[][4] = { "log", "tmp", "dmp", "cfg", "ini", "dat", "sav", "bak", "pyc" };
        for (i = 0; i < sizeof ext / sizeof ext[0]; i++)
            if (e[0] == ext[i][0] && e[1] == ext[i][1] && e[2] == ext[i][2]) return 0;
    }
    return 1;
}

static int cacheable_slow(const char *k)
{
    unsigned i;
    if (!g_rootlen) return 0;
    for (i = 0; i < g_rootlen; i++) if (k[i] != g_root[i]) return 0;
    if (!k[g_rootlen]) return 0;                       /* the root itself */
    if (is_device_name(k)) return 0;
    if (contains(k, "*") || contains(k, "?") || contains(k, "~")) return 0;
    if (contains(k + 2, ":")) return 0;                /* alternate data streams */
    if (contains(k, "\\.\\") || contains(k, "\\..")) return 0;
    if (ends_with(k, "\\") || ends_with(k, ".") || ends_with(k, " ")) return 0;
    if (contains(k, "\\save\\") || ends_with(k, "\\save")) return 0;
    if (contains(k, "\\cfg\\") || ends_with(k, "\\cfg")) return 0;
    if (contains(k, "\\python\\") || ends_with(k, "\\python")) return 0;
    if (contains(k, "\\screenshots\\") || ends_with(k, "\\screenshots")) return 0;
    if (contains(k, "\\maps\\graphs\\") || ends_with(k, "\\maps\\graphs")) return 0;
    if (contains(k, "\\materialsrc\\") || ends_with(k, "\\materialsrc")) return 0;
    if (contains(k, "\\procmon\\") || ends_with(k, "\\procmon")) return 0;
    if (contains(k, "\\logs\\") || ends_with(k, "\\logs")) return 0;
    if (contains(k, "\\vdata\\hackterminals\\") || ends_with(k, "\\vdata\\hackterminals")) return 0;  /* rewritten every session */
    if (ends_with(k, ".log") || ends_with(k, ".tmp") || ends_with(k, ".dmp") ||
        ends_with(k, ".cfg") || ends_with(k, ".ini") || ends_with(k, ".dat") ||
        ends_with(k, ".sav") || ends_with(k, ".bak") || ends_with(k, ".pyc")) return 0;
    return 1;
}

static int is_missing_err(DWORD e) { return e == 2 /*FILE_NOT_FOUND*/ || e == 3 /*PATH_NOT_FOUND*/; }

/* Writes the parent folder of key into dir. Returns 0 if the parent is the root. */
static int parent_of(const char *key, char *dir)
{
    unsigned n = slen(key);
    while (n > 0 && key[n - 1] != '\\') n--;
    if (n <= g_rootlen) return 0;
    cpy(dir, key, n - 1);
    dir[n - 1] = 0;
    return 1;
}

/* ---- index mode decisions (must hold g_cs) ----
 * Returns 0 = ask Windows, 2/3 = report that error, 1 = indexed file (fi set). */
enum { ASK = 0, HAVE_FILE = 1 };
static int index_lookup(const char *key, FileInfo **fi_out)
{
    char dir[MAXP];
    Entry *e;
    *fi_out = 0;
    if (parent_of(key, dir)) {
        e = table_find(dir, T_DIR);
        if (!e || e->val == D_GONE) return 3;          /* folder doesn't exist */
        if (e->val != D_TRUSTED) return ASK;
    }
    e = table_find(key, T_FILE);
    if (e && e->val == F_PRESENT) { *fi_out = e->fi; return HAVE_FILE; }
    if (e) return 2;                                   /* deleted at runtime */
    e = table_find(key, T_DIR);
    if (e) return e->val == D_GONE ? 2 : ASK;          /* a folder: let Windows answer */
    return 2;                                          /* not in index */
}

/* ---- learn mode decisions (must hold g_cs) ---- */
static int learn_lookup(const char *key)
{
    char dir[MAXP];
    Entry *e = table_find(key, T_MISSING);
    if (e) return (int)e->val;
    if (parent_of(key, dir)) {
        e = table_find(dir, T_PROBE);
        if (!e) {
            DWORD a = GetFileAttributesA(dir);
            st.dirs_probed++;
            e = table_put(dir, T_PROBE);
            if (!e) return ASK;
            e->val = (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) ? 1 : 0;
        }
        if (!e->val) return 3;
    }
    return ASK;
}

static void learn_record_missing(const char *key, DWORD err)
{
    Entry *e = table_put(key, T_MISSING);
    if (e) e->val = err;
}

/* Something on disk changed through the filesystem. */
void fc_invalidate(void);
#ifdef FASTFS_HOST_TEST
void fc_invalidate(void) {}
#endif
static int g_logconsole;
void sl_mark(const char *text);
static unsigned g_change_notes;
static void log_change(const char *what, const char *key)
{
#ifndef FASTFS_HOST_TEST
    if (g_logconsole && g_change_notes < 8) {
        char m[MAXP + 48]; unsigned n = 0, k = slen(what);
        cpy(m, what, k); n = k;
        k = slen(key); if (k > MAXP - 1) k = MAXP - 1;
        cpy(m + n, key, k); m[n + k] = 0;
        g_change_notes++;
        sl_mark(m);
    }
#else
    (void)what; (void)key;
#endif
}
static void note_created_file(const char *key)        /* must hold g_cs */
{
    log_change("search cache: dropped, file written: ", key);
#ifndef FASTFS_HOST_TEST
    fc_invalidate();
#endif
    if (g_mode == MODE_INDEX) {
        char dir[MAXP];
        Entry *e = table_put(key, T_FILE);
        if (e) { e->val = F_PRESENT; e->fi = 0; }      /* stat: ask Windows from now on */
        if (parent_of(key, dir)) {
            Entry *d = table_find(dir, T_DIR);
            if (!d) { d = table_put(dir, T_DIR); if (d) d->val = D_UNTRUSTED; }
            else if (d->val == D_GONE) d->val = D_UNTRUSTED;
        }
    }
}

static void note_unsure(const char *key)              /* must hold g_cs */
{
    /* Mark the parent folder untrusted, and the path itself if it's a folder:
     * lookups there go to Windows from now on. */
    char dir[MAXP];
    Entry *e;
    if (g_mode != MODE_INDEX) return;
    e = table_find(key, T_DIR);
    if (e) e->val = D_UNTRUSTED;
    e = table_find(key, T_FILE);
    if (e) e->fi = 0;
    if (parent_of(key, dir)) {
        e = table_put(dir, T_DIR);
        if (e) e->val = D_UNTRUSTED;
    }
}

static void flush_learned(void)
{
    if (g_mode == MODE_LEARN) {
        EnterCriticalSection(&g_cs);
        if (g_count) { table_reset(); st.flushes++; }
        LeaveCriticalSection(&g_cs);
    }
}

/* ---- real functions (whatever was in the IAT before us) ---- */
typedef HANDLE (WINAPI *PFN_CreateFileA)(LPCSTR, DWORD, DWORD, void *, DWORD, DWORD, HANDLE);
typedef HANDLE (WINAPI *PFN_FindFirstFileA)(LPCSTR, WIN32_FIND_DATAA *);
typedef BOOL   (WINAPI *PFN_FindNextFileA)(HANDLE, WIN32_FIND_DATAA *);
typedef BOOL   (WINAPI *PFN_FindClose)(HANDLE);
typedef BOOL   (WINAPI *PFN_Path1)(LPCSTR);
typedef BOOL   (WINAPI *PFN_Path2)(LPCSTR, LPCSTR);
typedef BOOL   (WINAPI *PFN_CreateDirectoryA)(LPCSTR, void *);

void prof_note_map_open(void);
void sl_mark(const char *text);
void watch_note_map_open(void);
void th_note_bsp(const char *key);
int th_start(const char *exedir);
void watch_start(void);
void fastconnect_start(void);
extern volatile unsigned int fcn_lifted, fcn_skips, fcn_unstuck, fp_patched;
void fastparse_start(void);
static int g_fastparse;
void fastui_start(void);
void fastmodels_start(void);
extern volatile unsigned int fu_patched, fm_patched, fm_hits, fm_misses;
static int g_fastui, g_fastmodels;
void fastinflate_start(void);
void fi_mark(void);
extern volatile unsigned int fi_patched, fi_fast, fi_passed, fi_dups, fi_kb_in, fi_kb_out, fi_dup_kb_out;
static int g_fastinflate;
void fastbones_start(void);
extern volatile unsigned int fb_patched, fb_lookups, fb_builds, fb_linear;
static int g_fastbones;
void fastseq_start(void);
extern volatile unsigned int fs_patched, fs_lookups, fs_builds, fs_linear;
static int g_fastseq;
void fadeprobe_start(void);
void fadefix_start(void);
void fastsym_start(void);
extern volatile unsigned int fy_patched, fy_hits, fy_misses;
static int g_fastsym;
extern volatile unsigned int ff_patched, fx_patched, fx_seen;
static int g_fadefix;
void tp_start(const char *exedir);
void tp_note_open(HANDLE h, const char *name);
void tp_map_open(const char *key);
BOOL WINAPI tp_hk_ReadFile(HANDLE, void *, DWORD, DWORD *, void *);
extern void *tp_real_ReadFile;
extern volatile unsigned int tp_on, tp_served, tp_selfdone, tp_mismatch, tp_waited_ms, tp_learned, tp_maps_known, tp_prefetched;
static int g_texprefetch;
static int g_probefades;
static int g_fastconnect;
static int g_watch;
static int g_baseline;   /* -fastfs_baseline: keep hooks for measuring, but no speed-ups */
void sl_tick(void);
void sl_start(const char *exedir, DWORD main_tid);
static int g_logconsole;
void prof_start(const char *exedir);
static int g_profile;

static PFN_CreateFileA      real_CreateFileA;
static PFN_FindFirstFileA   real_FindFirstFileA;
static PFN_FindNextFileA    real_FindNextFileA;
static PFN_FindClose        real_FindClose;
static PFN_Path1            real_DeleteFileA;
static PFN_Path2            real_MoveFileA;
static PFN_CreateDirectoryA real_CreateDirectoryA;

/* Fake handles for stat results answered from the index: addresses inside
 * this static array, so foreign handles are never dereferenced. */
#define NFAKE 64
static BYTE g_fake_used[NFAKE];
static int fake_index(HANDLE h)
{
    BYTE *p = (BYTE *)h;
    if (p < g_fake_used || p >= g_fake_used + NFAKE) return -1;
    return (int)(p - g_fake_used);
}

static void fill_find(WIN32_FIND_DATAA *fd, const FileInfo *fi)
{
    unsigned n = slen(fi->name);
    memset(fd, 0, sizeof *fd);
    fd->dwFileAttributes = fi->attrs;
    fd->ftCreationTime = fi->c;
    fd->ftLastAccessTime = fi->a;
    fd->ftLastWriteTime = fi->w;
    fd->nFileSizeHigh = fi->szhi;
    fd->nFileSizeLow = fi->szlo;
    if (n > 259) n = 259;
    cpy(fd->cFileName, fi->name, n);
}

/* ---- hooks ---- */
#define GENERIC_WRITE_ 0x40000000UL
#define GENERIC_ALL_   0x10000000UL
#define WRITE_BITS     (GENERIC_WRITE_ | GENERIC_ALL_ | 0x2 /*WRITE_DATA*/ | 0x4 /*APPEND*/ | 0x10000 /*DELETE*/)
#define OPEN_EXISTING_ 3UL

static HANDLE WINAPI hk_CreateFileA(LPCSTR name, DWORD acc, DWORD share, void *sa,
                                    DWORD disp, DWORD flags, HANDLE tmpl)
{
    char key[MAXP];
    HANDLE h;
    DWORD err;
    int ok_key;
    key[0] = 0;
    ok_key = name && normalize(name, key) && cacheable(key);

#ifndef FASTFS_HOST_TEST
    if (g_logconsole && name) {
        char mk[MAXP + 16];
        unsigned ln = slen(name), i = 0;
        int wr = (acc & WRITE_BITS) || disp != OPEN_EXISTING_;
        const char *tag = wr ? "write " : "open ";
        if (ln < MAXP && (wr || ends_with(key[0] ? key : "", ".bsp"))) {
            while (*tag) mk[i++] = *tag++;
            cpy(mk + i, name, ln + 1);
            sl_mark(mk);
        }
    }
#endif
    if (!name || (acc & WRITE_BITS) || disp != OPEN_EXISTING_) {
        h = real_CreateFileA(name, acc, share, sa, disp, flags, tmpl);
        err = GetLastError();
        if (g_mode == MODE_LEARN) { flush_learned(); if (ok_key && h != INVALID_HANDLE_VALUE) fc_invalidate(); }
        else if (ok_key && h != INVALID_HANDLE_VALUE) {
            EnterCriticalSection(&g_cs); note_created_file(key); LeaveCriticalSection(&g_cs);
        }
        SetLastError(err);
        return h;
    }
#ifndef FASTFS_HOST_TEST
    if (g_profile && ok_key && ends_with(key, ".bsp") && contains(key, "\\maps\\")) prof_note_map_open();
    if (g_watch && ok_key && ends_with(key, ".bsp") && contains(key, "\\maps\\")) watch_note_map_open();
    if (ok_key && ends_with(key, ".bsp") && contains(key, "\\maps\\")) { th_note_bsp(key); fi_mark(); if (g_texprefetch) tp_map_open(key); }
#endif
    if (!ok_key || g_mode == MODE_OFF) {
        st.passthrough++;
        h = real_CreateFileA(name, acc, share, sa, disp, flags, tmpl);
        if (g_texprefetch && h != INVALID_HANDLE_VALUE) { err = GetLastError(); tp_note_open(h, name); SetLastError(err); }
        return h;
    }

    EnterCriticalSection(&g_cs);
    {
        int r;
        if (g_mode == MODE_INDEX) { FileInfo *fi; r = index_lookup(key, &fi); if (r == HAVE_FILE) r = ASK; }
        else r = learn_lookup(key);
        if (r) { st.miss_hits++; LeaveCriticalSection(&g_cs); SetLastError((DWORD)r); return INVALID_HANDLE_VALUE; }
    }
    LeaveCriticalSection(&g_cs);

    st.real_opens++;
    h = real_CreateFileA(name, acc, share, sa, disp, flags, tmpl);
    if (g_texprefetch && h != INVALID_HANDLE_VALUE) { err = GetLastError(); tp_note_open(h, name); SetLastError(err); }
    if (h == INVALID_HANDLE_VALUE) {
        err = GetLastError();
        if (g_mode == MODE_LEARN && is_missing_err(err)) {
            EnterCriticalSection(&g_cs); learn_record_missing(key, err); LeaveCriticalSection(&g_cs);
        }
        SetLastError(err);
    }
    return h;
}

static HANDLE WINAPI hk_FindFirstFileA(LPCSTR name, WIN32_FIND_DATAA *fd)
{
    char key[MAXP];
    HANDLE h;
    DWORD err;
    if (!name || !fd || g_mode == MODE_OFF || !normalize(name, key) || !cacheable(key)) {
        st.passthrough++;
        return real_FindFirstFileA(name, fd);
    }

    EnterCriticalSection(&g_cs);
    if (g_mode == MODE_INDEX) {
        FileInfo *fi;
        int r = index_lookup(key, &fi);
        if (r == 2 || r == 3) { st.miss_hits++; LeaveCriticalSection(&g_cs); SetLastError((DWORD)r); return INVALID_HANDLE_VALUE; }
        if (r == HAVE_FILE && fi) {
            int i;
            for (i = 0; i < NFAKE; i++) if (!g_fake_used[i]) break;
            if (i < NFAKE) {
                g_fake_used[i] = 1;
                fill_find(fd, fi);
                st.find_hits++;
                LeaveCriticalSection(&g_cs);
                SetLastError(0);
                return (HANDLE)&g_fake_used[i];
            }
        }
    } else {
        int r = learn_lookup(key);
        if (r) { st.miss_hits++; LeaveCriticalSection(&g_cs); SetLastError((DWORD)r); return INVALID_HANDLE_VALUE; }
    }
    LeaveCriticalSection(&g_cs);

    st.real_finds++;
    h = real_FindFirstFileA(name, fd);
    err = GetLastError();
    if (h == INVALID_HANDLE_VALUE && g_mode == MODE_LEARN && is_missing_err(err)) {
        EnterCriticalSection(&g_cs); learn_record_missing(key, err); LeaveCriticalSection(&g_cs);
    }
    SetLastError(err);
    return h;
}

static BOOL WINAPI hk_FindNextFileA(HANDLE h, WIN32_FIND_DATAA *fd)
{
    if (fake_index(h) >= 0) { SetLastError(18 /*NO_MORE_FILES*/); return 0; }
    return real_FindNextFileA(h, fd);
}

static BOOL WINAPI hk_FindClose(HANDLE h)
{
    int i = fake_index(h);
    if (i >= 0) {
        EnterCriticalSection(&g_cs);
        g_fake_used[i] = 0;
        LeaveCriticalSection(&g_cs);
        return 1;
    }
    return real_FindClose(h);
}

static void note_path_changed(LPCSTR p, int created_dir)
{
    char key[MAXP];
    if (g_mode == MODE_LEARN) { flush_learned(); return; }
    if (!p || !normalize(p, key) || !cacheable(key)) return;
    log_change(created_dir ? "search cache: dropped, folder created: " : "search cache: dropped, path changed: ", key);
#ifndef FASTFS_HOST_TEST
    fc_invalidate();
#endif
    EnterCriticalSection(&g_cs);
    if (created_dir) {
        Entry *e = table_put(key, T_DIR);
        if (e) e->val = D_UNTRUSTED;   /* new folder: contents unknown to the index */
    }
    note_unsure(key);
    LeaveCriticalSection(&g_cs);
}

static BOOL WINAPI hk_DeleteFileA(LPCSTR a)
{
    BOOL r = real_DeleteFileA(a);
    DWORD err = GetLastError();
    if (r) note_path_changed(a, 0);                    /* a failed call changed nothing */
    SetLastError(err);
    return r;
}
static BOOL WINAPI hk_MoveFileA(LPCSTR a, LPCSTR b)
{
    BOOL r = real_MoveFileA(a, b);
    DWORD err = GetLastError();
    if (r) { note_path_changed(a, 0); note_path_changed(b, 0); }
    SetLastError(err);
    return r;
}
static BOOL WINAPI hk_CreateDirectoryA(LPCSTR a, void *s)
{
    BOOL r = real_CreateDirectoryA(a, s);
    DWORD err = GetLastError();
    /* the engine "creates" existing folders (maps\, ...) before writing; that fails and changes nothing */
    if (r) note_path_changed(a, 1);
    SetLastError(err);
    return r;
}

/* ---- index building ---- */
static int dir_excluded(const char *dirkey)
{
    char probe[MAXP];
    unsigned n = slen(dirkey);
    if (n + 3 >= MAXP) return 1;
    cpy(probe, dirkey, n);
    probe[n] = '\\'; probe[n + 1] = 'a'; probe[n + 2] = 0;
    return !cacheable(probe);
}

static int name_ok(const char *s)
{
    for (; *s; s++) if ((BYTE)*s >= 0x80 || *s == '?') return 0;
    return 1;
}

/* Record one directory entry found by the scan. dirkey is the normalized
 * folder; is_dir/attrs/... describe the entry. Returns 0 on OOM. */
static int index_add(const char *dirkey, const WIN32_FIND_DATAA *fd, char *childkey_out)
{
    char key[MAXP];
    unsigned dn = slen(dirkey), nn = slen(fd->cFileName);
    Entry *e;
    if (dn + 1 + nn + 1 >= MAXP) return 1;
    cpy(key, dirkey, dn);
    key[dn] = '\\';
    cpy(key + dn + 1, fd->cFileName, nn + 1);
    if (!normalize(key, key)) return 1;
    if (childkey_out) cpy(childkey_out, key, slen(key) + 1);
    /* Content signature for the search cache: game data only. Files the game,
     * the loader or this plugin rewrite (logs, configs, profiles, caches) are
     * left out, or the signature would change every session. */
    if ((!(fd->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !dir_excluded(key)) &&
        cacheable(key) && !contains(key, "\\vtmb-") && !ends_with(key, ".cache") &&
        !ends_with(key, ".bin") && !ends_with(key, ".pml") && !ends_with(key, ".csv") &&
        !contains(key, "\\bin\\loader\\")) {
        sig_add(key, slen(key) + 1);
        man_put(key, slen(key));
        if (!(fd->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            sig_add(&fd->nFileSizeLow, 4); sig_add(&fd->nFileSizeHigh, 4); sig_add(&fd->ftLastWriteTime, 8);
            man_put(" ", 1); man_hex(fd->nFileSizeLow); man_put(" ", 1);
            man_hex(fd->ftLastWriteTime.hi); man_hex(fd->ftLastWriteTime.lo);
        }
        man_put("\r\n", 2);
    }
    if (fd->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        if (dir_excluded(key)) return 1;
        e = table_put(key, T_DIR);
        if (!e) return 0;
        e->val = D_TRUSTED;
        st.idx_dirs++;
    } else {
        FileInfo *fi;
        e = table_put(key, T_FILE);
        if (!e) return 0;
        fi = (FileInfo *)halloc(sizeof *fi);
        if (!fi) return 0;
        fi->attrs = fd->dwFileAttributes;
        fi->c = fd->ftCreationTime; fi->a = fd->ftLastAccessTime; fi->w = fd->ftLastWriteTime;
        fi->szhi = fd->nFileSizeHigh; fi->szlo = fd->nFileSizeLow;
        fi->name = (char *)halloc(nn + 1);
        if (!fi->name) return 0;
        cpy(fi->name, fd->cFileName, nn + 1);
        e->val = F_PRESENT;
        e->fi = fi;
        st.idx_files++;
    }
    return 1;
}

static void index_mark_untrusted(const char *dirkey)
{
    Entry *e = table_put(dirkey, T_DIR);
    if (e) e->val = D_UNTRUSTED;
    st.idx_untrusted++;
}

#ifndef FASTFS_HOST_TEST
/* Breadth-first scan of the game folder using the real (unhooked) API.
 * Directory paths for the scan keep their original case. */
typedef struct DirNode { struct DirNode *next; char path[1]; } DirNode;

static int build_index(const char *rootpath /* original case, trailing '\' */)
{
    DirNode *head, *tail, *n;
    unsigned rl = slen(rootpath);
    DWORD t0 = GetTickCount();
    head = tail = (DirNode *)halloc(sizeof(DirNode) + rl);
    if (!head) return 0;
    cpy(head->path, rootpath, rl - 1);                 /* without trailing '\' */
    head->path[rl - 1] = 0;
    head->next = 0;

    while ((n = head) != 0) {
        char pat[MAXP], dirkey[MAXP];
        WIN32_FIND_DATAA fd;
        HANDLE fh;
        unsigned pl = slen(n->path);
        head = n->next;
        if (!head) tail = 0;
        if (pl + 3 >= MAXP) continue;
        if (!normalize(n->path, dirkey)) continue;
        cpy(pat, n->path, pl);
        pat[pl] = '\\'; pat[pl + 1] = '*'; pat[pl + 2] = 0;
        fh = FindFirstFileA(pat, &fd);
        if (fh == INVALID_HANDLE_VALUE) {
            if (pl > g_rootlen) index_mark_untrusted(dirkey);
            continue;
        }
        do {
            char ck[MAXP];
            const char *nm = fd.cFileName;
            if (nm[0] == '.' && (!nm[1] || (nm[1] == '.' && !nm[2]))) continue;
            if (!name_ok(nm)) {
                /* can't be represented exactly: let Windows answer in this folder */
                if (pl > g_rootlen) index_mark_untrusted(dirkey);
                else { FindClose(fh); return 0; }      /* odd name at the top: give up */
                continue;
            }
            if (st.idx_files + st.idx_dirs > MAX_INDEX_ENTRIES) { FindClose(fh); st.scan_aborted = 1; return 0; }
            if (!index_add(dirkey, &fd, ck)) { FindClose(fh); return 0; }
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                !(fd.dwFileAttributes & 0x400 /*REPARSE_POINT*/) && !dir_excluded(ck)) {
                unsigned nl = slen(nm);
                DirNode *c;
                if (pl + 1 + nl + 1 >= MAXP) { index_mark_untrusted(ck); continue; }
                c = (DirNode *)halloc(sizeof(DirNode) + pl + 1 + nl);
                if (!c) { FindClose(fh); return 0; }
                cpy(c->path, n->path, pl);
                c->path[pl] = '\\';
                cpy(c->path + pl + 1, nm, nl + 1);
                c->next = 0;
                if (tail) tail->next = c; else head = c;
                tail = c;
            } else if (fd.dwFileAttributes & 0x400) {
                Entry *e = table_find(ck, T_DIR);
                if (e) e->val = D_UNTRUSTED;           /* junction: don't vouch for its contents */
            }
        } while (FindNextFileA(fh, &fd));
        FindClose(fh);
    }
    st.scan_ms = GetTickCount() - t0;
    return 1;
}

/* ---- import table patching ---- */
typedef struct { DWORD OriginalFirstThunk, TimeDateStamp, ForwarderChain, Name, FirstThunk; } IMPDESC;

static int ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return *a == *b;
}

static void **find_iat_slot(BYTE *base, const char *dll, const char *fn)
{
    DWORD pe = *(DWORD *)(base + 0x3C);
    BYTE *opt = base + pe + 24;
    DWORD imp_rva;
    IMPDESC *d;
    if (*(WORD *)opt != 0x10B) return 0;               /* PE32 only */
    imp_rva = *(DWORD *)(opt + 96 + 8);                /* DataDirectory[IMPORT] */
    if (!imp_rva) return 0;
    for (d = (IMPDESC *)(base + imp_rva); d->Name; d++) {
        DWORD *names, *slots;
        if (!ieq((char *)(base + d->Name), dll)) continue;
        names = (DWORD *)(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        slots = (DWORD *)(base + d->FirstThunk);
        for (; *names; names++, slots++) {
            if (*names & 0x80000000UL) continue;
            if (keyeq((char *)(base + *names + 2), fn)) return (void **)slots;
        }
    }
    return 0;
}

static int patch(BYTE *base, const char *fn, void *hook, void **real_out)
{
    void **slot = find_iat_slot(base, "kernel32.dll", fn);
    DWORD old;
    if (!slot) return 0;
    if (!VirtualProtect(slot, sizeof(void *), 0x04 /*PAGE_READWRITE*/, &old)) return 0;
    *real_out = *slot;
    *slot = hook;
    VirtualProtect(slot, sizeof(void *), old, &old);
    return 1;
}

static char g_exedir[MAXP];
static char g_logpath[MAXP];
static int g_logging, g_use_index, g_cache_finds, g_finds_on;
struct FcStats { DWORD replayed, recorded, passthrough, loaded, saved, mismatch; };
extern struct FcStats fc_st;
int fc_install(BYTE *fsbase, unsigned long long sig, const char *loaderdir);
void fc_maybe_save(void);
static int g_patched;

static void append_num(char *buf, unsigned *n, const char *label, DWORD v)
{
    char tmp[12];
    int i = 0;
    while (*label) buf[(*n)++] = *label++;
    do { tmp[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (i) buf[(*n)++] = tmp[--i];
    buf[(*n)++] = '\r'; buf[(*n)++] = '\n';
}

static void write_log(void)
{
    char buf[1800];
    unsigned n = 0;
    DWORD w;
    HANDLE f;
    const char *hdr = "vtmb-fastfs statistics (since game start)\r\n";
    const char *mode = g_mode == MODE_INDEX ? "mode: index\r\n" : g_mode == MODE_LEARN ? "mode: learn\r\n" : "mode: off\r\n";
    while (*hdr) buf[n++] = *hdr++;
    while (*mode) buf[n++] = *mode++;
    append_num(buf, &n, "hooks installed (of 7): ", (DWORD)g_patched);
    append_num(buf, &n, "indexed files: ", st.idx_files);
    append_num(buf, &n, "indexed folders: ", st.idx_dirs);
    append_num(buf, &n, "folders left to Windows: ", st.idx_untrusted);
    append_num(buf, &n, "index build time (ms): ", st.scan_ms);
    append_num(buf, &n, "index aborted (too large): ", st.scan_aborted);
    append_num(buf, &n, "missing-file lookups answered instantly: ", st.miss_hits);
    append_num(buf, &n, "file stats answered from index: ", st.find_hits);
    append_num(buf, &n, "opens passed to Windows: ", st.real_opens);
    append_num(buf, &n, "stats passed to Windows: ", st.real_finds);
    append_num(buf, &n, "lookups outside cached locations: ", st.passthrough);
    append_num(buf, &n, "folders probed (learn mode): ", st.dirs_probed);
    append_num(buf, &n, "cache resets (learn mode): ", st.flushes);
    append_num(buf, &n, "search cache active: ", (DWORD)g_finds_on);
    append_num(buf, &n, "searches loaded from cache file: ", fc_st.loaded);
    append_num(buf, &n, "searches answered from cache: ", fc_st.replayed);
    append_num(buf, &n, "searches recorded this session: ", fc_st.recorded);
    append_num(buf, &n, "searches not cacheable: ", fc_st.passthrough);
    append_num(buf, &n, "searches in cache file: ", fc_st.saved);
    append_num(buf, &n, "cache file ignored (game files changed): ", fc_st.mismatch);
    append_num(buf, &n, "fast connect: frame cap lifted during loads: ", g_fastconnect ? fcn_lifted : 0);
    append_num(buf, &n, "fast connect: loads with loading-screen redraws skipped: ", g_fastconnect ? fcn_skips : 0);
    append_num(buf, &n, "fast connect: watchdog resets (should be 0): ", g_fastconnect ? fcn_unstuck : 0);
    append_num(buf, &n, "fast parse: atof patched (1 = yes, 2 = unknown game build): ", g_fastparse ? fp_patched : 0);
    append_num(buf, &n, "fast console: patched (1 = yes, 2 = unknown gameui build): ", g_fastui ? fu_patched : 0);
    append_num(buf, &n, "fast model lookup: installed (1 = yes, 2 = modelloader-fix absent/unknown): ", g_fastmodels ? fm_patched : 0);
    append_num(buf, &n, "fast model lookup: answered from cache: ", fm_hits);
    append_num(buf, &n, "fast model lookup: passed to modelloader-fix: ", fm_misses);
    append_num(buf, &n, "fast inflate: installed (1 = yes, 2 = no): ", g_fastinflate ? fi_patched : 0);
    append_num(buf, &n, "fast inflate: textures decompressed by libdeflate: ", fi_fast);
    append_num(buf, &n, "fast inflate: passed to zlib: ", fi_passed);
    append_num(buf, &n, "fast inflate: KB compressed in: ", fi_kb_in);
    append_num(buf, &n, "fast inflate: KB decompressed out: ", fi_kb_out);
    append_num(buf, &n, "fast inflate: repeats of an earlier texture: ", fi_dups);
    append_num(buf, &n, "fast inflate: KB decompressed again for repeats: ", fi_dup_kb_out);
    append_num(buf, &n, "fast bone link: installed (1 = yes, 2 = unknown engine build): ", g_fastbones ? fb_patched : 0);
    append_num(buf, &n, "fast bone link: lookups: ", fb_lookups);
    append_num(buf, &n, "fast bone link: tables built: ", fb_builds);
    append_num(buf, &n, "fast anim lookup: installed (1 = yes, 2 = unknown game build): ", g_fastseq ? fs_patched : 0);
    append_num(buf, &n, "fast anim lookup: lookups: ", fs_lookups);
    append_num(buf, &n, "fast anim lookup: tables built: ", fs_builds);
    append_num(buf, &n, "fast anim lookup: linear fallbacks: ", fs_linear);
    append_num(buf, &n, "load fade-in: disabled (1 = yes, 2 = unknown game build): ", g_fadefix ? ff_patched : 0);
    append_num(buf, &n, "loading screen fade-out: removed (1 = yes, 2 = unknown client build): ", g_fadefix ? fx_patched : 0);
    append_num(buf, &n, "fast symbols: installed (1 = yes, 2 = unknown filesystem build): ", g_fastsym ? fy_patched : 0);
    append_num(buf, &n, "fast symbols: answered from cache: ", fy_hits);
    append_num(buf, &n, "fast symbols: passed to the filesystem: ", fy_misses);
    append_num(buf, &n, "texture prefetch: active: ", tp_on);
    append_num(buf, &n, "texture prefetch: maps with a texture list: ", tp_maps_known);
    append_num(buf, &n, "texture prefetch: textures learned this session: ", tp_learned);
    append_num(buf, &n, "texture prefetch: decompressed ahead by worker threads: ", tp_prefetched);
    append_num(buf, &n, "texture prefetch: served to the game from prefetch: ", tp_served);
    append_num(buf, &n, "texture prefetch: on the list but done by the game (worker not there yet): ", tp_selfdone);
    append_num(buf, &n, "texture prefetch: content mismatches (should be 0): ", tp_mismatch);
    append_num(buf, &n, "texture prefetch: ms the game waited for a worker: ", tp_waited_ms);
    f = CreateFileA(g_logpath, GENERIC_WRITE_, 1, 0, 2 /*CREATE_ALWAYS*/, 0x80, 0);
    if (f == INVALID_HANDLE_VALUE) return;
    WriteFile(f, buf, n, &w, 0);
    CloseHandle(f);
}

static DWORD WINAPI worker(void *unused)
{
    BYTE *fs = 0;
    DWORD waited = 0, last = 0xFFFFFFFF;
    int mode = MODE_LEARN;
    (void)unused;

    if (g_use_index) {
        if (g_logging) { g_mancap = 6u << 20; g_man = (char *)VirtualAlloc(0, g_mancap, 0x3000, 0x04); }
        EnterCriticalSection(&g_cs);
        if (build_index(g_exedir)) mode = MODE_INDEX;
        else table_reset();                            /* fall back to learning */
        LeaveCriticalSection(&g_cs);
    }

    if (g_texprefetch) tp_start(g_exedir);
    while (!(fs = (BYTE *)GetModuleHandleA("filesystem_stdio.dll")) && waited < 120000) { Sleep(1); waited++; }
    if (fs) {
        /* Hooks that only pass through or update state go in first; the ones
         * that answer lookups go in last, once FindNext/FindClose are ready. */
        g_patched += patch(fs, "FindNextFileA", (void *)hk_FindNextFileA, (void **)&real_FindNextFileA);
        g_patched += patch(fs, "FindClose", (void *)hk_FindClose, (void **)&real_FindClose);
        g_patched += patch(fs, "DeleteFileA", (void *)hk_DeleteFileA, (void **)&real_DeleteFileA);
        g_patched += patch(fs, "MoveFileA", (void *)hk_MoveFileA, (void **)&real_MoveFileA);
        g_patched += patch(fs, "CreateDirectoryA", (void *)hk_CreateDirectoryA, (void **)&real_CreateDirectoryA);
        /* Only answer lookups if every change can be seen. */
        if (g_patched == 5) {
            g_mode = MODE_OFF;                         /* hooks below pass through until switched on */
            g_patched += patch(fs, "FindFirstFileA", (void *)hk_FindFirstFileA, (void **)&real_FindFirstFileA);
            g_patched += patch(fs, "CreateFileA", (void *)hk_CreateFileA, (void **)&real_CreateFileA);
            if (g_patched == 7 && !g_baseline) g_mode = mode;   /* baseline: hooks only observe */
            if (g_patched == 7 && g_texprefetch && !patch(fs, "ReadFile", (void *)tp_hk_ReadFile, (void **)&tp_real_ReadFile))
                g_texprefetch = 0;
        }
        if (g_cache_finds) {
            char ldir[MAXP];
            unsigned n = slen(g_exedir);
            const char *cl = GetCommandLineA();
            cpy(ldir, g_exedir, n);
            cpy(ldir + n, "Bin\\loader\\", 12);
            if (cl) sig_add(cl, slen(cl));
            if (g_man) {
                char p1[MAXP + 32], p2[MAXP + 32];
                HANDLE mf;
                DWORD w;
                unsigned k = slen(g_exedir);
                man_put("cmdline: ", 9);
                if (cl) man_put(cl, slen(cl));
                man_put("\r\nsignature: ", 13);
                man_hex((DWORD)(g_sig >> 32)); man_hex((DWORD)g_sig);
                man_put("\r\n", 2);
                cpy(p1, g_exedir, k); cpy(p1 + k, "vtmb-fastfs-sig.txt", 20);
                cpy(p2, g_exedir, k); cpy(p2 + k, "vtmb-fastfs-sig.old.txt", 24);
                MoveFileExA(p1, p2, 1 /*REPLACE_EXISTING*/);
                mf = CreateFileA(p1, GENERIC_WRITE_, 1, 0, 2 /*CREATE_ALWAYS*/, 0x80, 0);
                if (mf != INVALID_HANDLE_VALUE) { WriteFile(mf, g_man, g_manlen, &w, 0); CloseHandle(mf); }
            }
            g_finds_on = fc_install(fs, mode == MODE_INDEX ? g_sig : 0, ldir);
        }
    }
    for (;;) {
        DWORD sum;
        Sleep(g_logconsole ? 500 : 2000);
        sl_tick();
        fc_maybe_save();
        if (!g_logging) continue;
        sum = st.miss_hits + st.find_hits + st.real_opens + st.real_finds + st.flushes + (DWORD)g_patched + (DWORD)g_mode +
              fc_st.replayed + fc_st.recorded + fc_st.saved;
        if (sum != last) { write_log(); last = sum; }
    }
}

BOOL WINAPI DllMain(HMODULE self, DWORD reason, void *reserved)
{
    (void)reserved; (void)self;
    if (reason == 1 /*DLL_PROCESS_ATTACH*/) {
        char ini[MAXP];
        unsigned n, i;
        DWORD th;
        HANDLE t;
        n = GetModuleFileNameA(0, g_exedir, MAXP);
        if (!n || n >= MAXP - 40) return 1;
        while (n > 0 && g_exedir[n - 1] != '\\') n--;  /* game folder incl. '\' */
        g_exedir[n] = 0;
        cpy(ini, g_exedir, n);
        cpy(ini + n, "Bin\\loader\\vtmb-fastfs.ini", 27);
        if (!GetPrivateProfileIntA("Main", "Enable", 1, ini)) return 1;
        g_use_index = (int)GetPrivateProfileIntA("Main", "Index At Startup", 1, ini);
        g_logging = (int)GetPrivateProfileIntA("Main", "Logging", 0, ini);
        g_profile = (int)GetPrivateProfileIntA("Main", "Profile Loads", 0, ini);
        g_cache_finds = (int)GetPrivateProfileIntA("Main", "Cache Searches", 1, ini);
        g_logconsole = (int)GetPrivateProfileIntA("Main", "Log Console", 0, ini);
        g_watch = g_logconsole && (int)GetPrivateProfileIntA("Main", "Watch Loads", 0, ini);
        g_fastconnect = (int)GetPrivateProfileIntA("Main", "Fast Connect", 1, ini);
        g_fastparse = (int)GetPrivateProfileIntA("Main", "Fast Parse", 1, ini);
        g_fastui = (int)GetPrivateProfileIntA("Main", "Fast Console", 1, ini);
        g_fastmodels = (int)GetPrivateProfileIntA("Main", "Fast Model Lookup", 1, ini);
        g_fastinflate = (int)GetPrivateProfileIntA("Main", "Fast Inflate", 1, ini);
        g_fastbones = (int)GetPrivateProfileIntA("Main", "Fast Bone Link", 1, ini);
        g_fastseq = (int)GetPrivateProfileIntA("Main", "Fast Anim Lookup", 1, ini);
        g_probefades = (int)GetPrivateProfileIntA("Main", "Probe Fades", 0, ini);
        g_fadefix = (int)GetPrivateProfileIntA("Main", "Skip Load Fade-In", 1, ini);
        g_fastsym = (int)GetPrivateProfileIntA("Main", "Fast Symbols", 1, ini);
        g_texprefetch = g_fastinflate && (int)GetPrivateProfileIntA("Main", "Prefetch Textures", 1, ini);
        cpy(g_logpath, g_exedir, n);
        cpy(g_logpath + n, "vtmb-fastfs.log", 16);
        if (!normalize(g_exedir, g_root)) return 1;    /* non-ASCII game path: stay off */
        g_rootlen = slen(g_root);
        for (i = 0; i < NFAKE; i++) g_fake_used[i] = 0;
        g_mode = MODE_OFF;
        InitializeCriticalSection(&g_cs);
        table_reset();
        if (g_profile) prof_start(g_exedir);
        if (g_logconsole) sl_start(g_exedir, GetCurrentThreadId());
        if (g_watch) watch_start();
        {
            const char *cl = GetCommandLineA(), *p;
            for (p = cl ? cl : ""; *p; p++) {
                const char *b = "-fastfs_baseline"; unsigned k;
                for (k = 0; b[k] && p[k] == b[k]; k++) {}
                if (!b[k]) g_baseline = 1;
            }
            if (g_baseline) { g_use_index = 0; g_cache_finds = 0; g_fastconnect = 0; g_fastparse = 0; g_fastui = 0; g_fastmodels = 0; g_fastinflate = 0; g_fastbones = 0; g_fastseq = 0; g_fadefix = 0; g_texprefetch = 0; g_fastsym = 0; }
        }
        if (g_fastconnect) fastconnect_start();
        if (g_fastparse) fastparse_start();
        if (g_fastui) fastui_start();
        if (g_fastmodels) fastmodels_start();
        if (g_fastinflate) fastinflate_start();
        if (g_fastbones) fastbones_start();
        if (g_fastseq) fastseq_start();
        if (g_probefades && g_logconsole) fadeprobe_start();
        if (g_fadefix) fadefix_start();
        if (g_fastsym) fastsym_start();
        th_start(g_exedir);                            /* only active with -fastfs_record / -fastfs_replay */
        t = CreateThread(0, 0, worker, 0, 0, &th);
        if (t) CloseHandle(t);
    }
    return 1;
}
#endif
