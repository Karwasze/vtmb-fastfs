/*
 * vtmb-fastfs search cache.
 *
 * FileSystem_Stdio.dll answers wildcard searches (FindFirst/FindNext, e.g.
 * "sound/Surfaces/Wood/ *.wav") by walking every entry of every VPK archive
 * from the start, comparing names one by one. vampire.dll runs ~40 such
 * searches on every level transition, which costs ~2.5 s of CPU.
 *
 * This module hooks the filesystem object's FindFirstEx / FindNext /
 * FindIsDirectory / FindClose vtable slots. The first time a pattern is
 * searched, the real search runs and its results are recorded as the caller
 * iterates; when it completes, the result list is cached and later searches
 * for the same pattern are replayed from the cache. Results are saved to
 * Bin\loader\vtmb-fastfs-find.cache together with a signature of the game
 * folder's contents (from the startup index) and the command line, so they
 * are reused on the next launch only if nothing changed.
 *
 * Never cached: patterns touching save\, cfg\, python\, maps\graphs\,
 * screenshots\ or absolute paths. Any file write, delete, move or folder
 * creation through the filesystem drops the whole cache for this session.
 *
 * Only activates on the exact FileSystem_Stdio.dll build it was written for
 * (vtable contents are checked first).
 */

typedef unsigned int   DWORD;     /* 32-bit, as on Windows */
typedef int            BOOL;
typedef void          *HANDLE;
typedef unsigned char  BYTE;
typedef const char    *LPCSTR;
typedef unsigned long long U64;

#ifndef FASTFS_HOST_TEST
#define WINAPI __stdcall
#define FASTCALL __fastcall
__declspec(dllimport) HANDLE WINAPI HeapCreate(DWORD, DWORD, DWORD);
__declspec(dllimport) void * WINAPI HeapAlloc(HANDLE, DWORD, DWORD);
__declspec(dllimport) BOOL   WINAPI VirtualProtect(void *, DWORD, DWORD, DWORD *);
__declspec(dllimport) HANDLE WINAPI CreateFileA(LPCSTR, DWORD, DWORD, void *, DWORD, DWORD, HANDLE);
__declspec(dllimport) BOOL   WINAPI WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
__declspec(dllimport) BOOL   WINAPI ReadFile(HANDLE, void *, DWORD, DWORD *, void *);
__declspec(dllimport) DWORD  WINAPI GetFileSize(HANDLE, DWORD *);
__declspec(dllimport) BOOL   WINAPI CloseHandle(HANDLE);
__declspec(dllimport) BOOL   WINAPI MoveFileExA(LPCSTR, LPCSTR, DWORD);
typedef struct { BYTE opaque[24]; } CRITICAL_SECTION;
__declspec(dllimport) void   WINAPI InitializeCriticalSection(CRITICAL_SECTION *);
__declspec(dllimport) void   WINAPI EnterCriticalSection(CRITICAL_SECTION *);
__declspec(dllimport) void   WINAPI LeaveCriticalSection(CRITICAL_SECTION *);
#define INVALID_HANDLE_VALUE ((HANDLE)(long)-1)
#else
#include "findcache_mock.h"
#endif

/* thiscall methods are hooked as fastcall functions with an unused EDX
 * argument: same stack layout, callee pops the stack arguments. */
typedef const char *(FASTCALL *PFN_FindFirstEx)(void *self, void *edx, const char *wild, int *handle, int flag);
typedef const char *(FASTCALL *PFN_FindNext)(void *self, void *edx, int handle);
typedef int         (FASTCALL *PFN_FindIsDir)(void *self, void *edx, int handle);
typedef void        (FASTCALL *PFN_FindClose)(void *self, void *edx, int handle);

static PFN_FindFirstEx real_FindFirstEx;
static PFN_FindNext    real_FindNext;
static PFN_FindIsDir   real_FindIsDir;
static PFN_FindClose   real_FindClose;

typedef struct { char *name; BYTE isdir; } Item;
typedef struct CEntry { struct CEntry *next; unsigned h; char *key; DWORD n; Item *items; } CEntry;

#define NBUCKET 1024
#define NREC 32
#define NFAKE 64
#define FAKE_BASE 0x7F000000

static CRITICAL_SECTION fc_cs;
static HANDLE   fc_heap;
static CEntry  *fc_bucket[NBUCKET];
static DWORD    fc_gen;            /* bumped on every invalidation */
static int      fc_on;
static int      fc_tainted;        /* something changed on disk this session */
static volatile DWORD fc_dirty;    /* entries added since the last save */
static U64      fc_sig;
static char     fc_path[520];

struct FcStats { DWORD replayed, recorded, passthrough, loaded, saved, mismatch; } fc_st;

typedef struct { int used, handle, gen, n, cap, done; char *key; Item *items; } Rec;
static Rec fc_rec[NREC];
typedef struct { int used; CEntry *e; DWORD pos, cur; } Fake;
static Fake fc_fake[NFAKE];

static unsigned fc_len(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static void fc_cpy(void *d, const void *s, unsigned n) { BYTE *p = d; const BYTE *q = s; while (n--) *p++ = *q++; }
static void *fc_alloc(unsigned n) { return HeapAlloc(fc_heap, 8, n); }
static char *fc_strdup(const char *s) { unsigned n = fc_len(s) + 1; char *d = fc_alloc(n); if (d) fc_cpy(d, s, n); return d; }
static int fc_eq(const char *a, const char *b) { while (*a && *a == *b) a++, b++; return *a == *b; }
static unsigned fc_hash(const char *k) { unsigned h = 2166136261u; while (*k) { h ^= (BYTE)*k++; h *= 16777619u; } return h; }
static int fc_has(const char *h, const char *n)
{
    unsigned ln = fc_len(n);
    for (; *h; h++) { unsigned i = 0; while (i < ln && h[i] == n[i]) i++; if (i == ln) return 1; }
    return 0;
}

/* key = normalized pattern + '|' + flag digit; returns 0 if not cacheable */
static int fc_key(const char *wild, int flag, char *out)
{
    unsigned n = 0;
    char prev = 0;
    for (; *wild; wild++) {
        char c = *wild;
        if ((BYTE)c >= 0x80) return 0;
        if (c == '\\') c = '/';
        if (c >= 'A' && c <= 'Z') c += 32;
        if (c == '/' && prev == '/') continue;
        if (n >= 500) return 0;
        out[n++] = c; prev = c;
    }
    out[n] = 0;
    if (!n || fc_has(out, ":") || out[0] == '/' || fc_has(out, "..")) return 0;
    {
        char tmp[520];
        tmp[0] = '/';
        fc_cpy(tmp + 1, out, n + 1);
        if (fc_has(tmp, "/save") || fc_has(tmp, "/cfg") || fc_has(tmp, "/python") ||
            fc_has(tmp, "/screenshots") || fc_has(tmp, "/graphs") || fc_has(tmp, "/logs") ||
            fc_has(tmp, "/materialsrc") || fc_has(tmp, "/hackterminals") ||
            fc_has(tmp, ".sav") || fc_has(tmp, ".hl")) return 0;
    }
    out[n++] = '|';
    out[n++] = (char)('0' + (flag & 7));
    out[n] = 0;
    return 1;
}

static CEntry *fc_find(const char *key)
{
    unsigned h = fc_hash(key);
    CEntry *e;
    for (e = fc_bucket[h & (NBUCKET - 1)]; e; e = e->next)
        if (e->h == h && fc_eq(e->key, key)) return e;
    return 0;
}

static CEntry *fc_insert(const char *key, DWORD n, Item *items)   /* takes ownership of items */
{
    unsigned h = fc_hash(key);
    CEntry *e = fc_find(key);
    if (e) return e;
    e = fc_alloc(sizeof *e);
    if (!e) return 0;
    e->key = fc_strdup(key);
    if (!e->key) return 0;
    e->h = h; e->n = n; e->items = items;
    e->next = fc_bucket[h & (NBUCKET - 1)];
    fc_bucket[h & (NBUCKET - 1)] = e;
    return e;
}

/* Called by the rest of the plugin when the filesystem changes the disk. */
void fc_invalidate(void)
{
    unsigned i;
    if (!fc_on) return;
    EnterCriticalSection(&fc_cs);
    for (i = 0; i < NBUCKET; i++) fc_bucket[i] = 0;   /* strings stay valid for open iterations */
    fc_gen++;
    fc_tainted = 1;
    LeaveCriticalSection(&fc_cs);
}

static Rec *rec_for(int handle)
{
    int i;
    for (i = 0; i < NREC; i++) if (fc_rec[i].used && fc_rec[i].handle == handle) return &fc_rec[i];
    return 0;
}

static void rec_add(Rec *r, const char *name, int isdir)
{
    if (!name) return;
    if (r->n == r->cap) {
        int nc = r->cap ? r->cap * 2 : 16;
        Item *ni = fc_alloc(nc * sizeof(Item));
        if (!ni) { r->used = 0; return; }
        if (r->n) fc_cpy(ni, r->items, r->n * sizeof(Item));
        r->items = ni; r->cap = nc;
    }
    r->items[r->n].name = fc_strdup(name);
    if (!r->items[r->n].name) { r->used = 0; return; }
    r->items[r->n].isdir = (BYTE)(isdir ? 1 : 0);
    r->n++;
}

static void rec_commit(Rec *r)
{
    if (r->gen == (int)fc_gen && !fc_find(r->key)) {
        if (fc_insert(r->key, r->n, r->items)) { fc_st.recorded++; fc_dirty++; }
    }
    r->used = 0;
}

static const char *FASTCALL hk_FindFirstEx(void *self, void *edx, const char *wild, int *handle, int flag)
{
    char key[520];
    const char *res;
    CEntry *e;
    int i;
    (void)edx;
    if (!fc_on || !wild || !handle || !fc_key(wild, flag, key)) {
        fc_st.passthrough++;
        return real_FindFirstEx(self, 0, wild, handle, flag);
    }
    EnterCriticalSection(&fc_cs);
    e = fc_find(key);
    if (e) {
        for (i = 0; i < NFAKE; i++) if (!fc_fake[i].used) break;
        if (i < NFAKE) {
            fc_fake[i].used = 1; fc_fake[i].e = e; fc_fake[i].cur = 0; fc_fake[i].pos = 1;
            *handle = FAKE_BASE + i;
            fc_st.replayed++;
            res = e->n ? e->items[0].name : 0;
            LeaveCriticalSection(&fc_cs);
            return res;
        }
    }
    LeaveCriticalSection(&fc_cs);

    res = real_FindFirstEx(self, 0, wild, handle, flag);

    EnterCriticalSection(&fc_cs);
    {
        Rec *stale = rec_for(*handle);                 /* handle reused without FindClose */
        if (stale) stale->used = 0;
    }
    {
        for (i = 0; i < NREC; i++) if (!fc_rec[i].used) break;
        if (i < NREC) {
            Rec *r = &fc_rec[i];
            r->used = 1; r->handle = *handle; r->gen = (int)fc_gen; r->n = 0; r->cap = 0; r->items = 0; r->done = 0;
            r->key = fc_strdup(key);
            if (!r->key) r->used = 0;
            else if (res) rec_add(r, res, real_FindIsDir(self, 0, *handle));
            else rec_commit(r);                        /* empty result is a result too */
        }
    }
    LeaveCriticalSection(&fc_cs);
    return res;
}

static const char *FASTCALL hk_FindNext(void *self, void *edx, int handle)
{
    const char *res;
    Rec *r;
    (void)edx;
    if ((unsigned)(handle - FAKE_BASE) < NFAKE) {
        Fake *f = &fc_fake[handle - FAKE_BASE];
        EnterCriticalSection(&fc_cs);
        res = 0;
        if (f->used && f->pos < f->e->n) { f->cur = f->pos; res = f->e->items[f->pos].name; f->pos++; }
        else if (f->used) f->pos = f->e->n + 1;
        LeaveCriticalSection(&fc_cs);
        return res;
    }
    res = real_FindNext(self, 0, handle);
    EnterCriticalSection(&fc_cs);
    r = rec_for(handle);
    if (r) {
        if (res) rec_add(r, res, real_FindIsDir(self, 0, handle));
        else rec_commit(r);
    }
    LeaveCriticalSection(&fc_cs);
    return res;
}

static int FASTCALL hk_FindIsDir(void *self, void *edx, int handle)
{
    (void)edx;
    if ((unsigned)(handle - FAKE_BASE) < NFAKE) {
        Fake *f = &fc_fake[handle - FAKE_BASE];
        int r = 0;
        EnterCriticalSection(&fc_cs);
        if (f->used && f->cur < f->e->n) r = f->e->items[f->cur].isdir;
        LeaveCriticalSection(&fc_cs);
        return r;
    }
    return real_FindIsDir(self, 0, handle);
}

static void FASTCALL hk_FindClose(void *self, void *edx, int handle)
{
    Rec *r;
    (void)edx;
    if ((unsigned)(handle - FAKE_BASE) < NFAKE) {
        EnterCriticalSection(&fc_cs);
        fc_fake[handle - FAKE_BASE].used = 0;
        LeaveCriticalSection(&fc_cs);
        return;
    }
    EnterCriticalSection(&fc_cs);
    r = rec_for(handle);
    if (r) r->used = 0;                                /* closed before the end: not complete */
    LeaveCriticalSection(&fc_cs);
    real_FindClose(self, 0, handle);
}

/* ---- persistence ---- */
#define FC_MAGIC 0x31434656u   /* "VFC1" */

static void fc_load(void)
{
    HANDLE f = CreateFileA(fc_path, 0x80000000UL, 1, 0, 3, 0x80, 0);
    DWORD size, got, pos = 0, nent, i;
    BYTE *buf;
    if (f == INVALID_HANDLE_VALUE) return;
    size = GetFileSize(f, 0);
    if (size < 16 || size > (64u << 20)) { CloseHandle(f); return; }
    buf = fc_alloc(size);
    if (!buf || !ReadFile(f, buf, size, &got, 0) || got != size) { CloseHandle(f); return; }
    CloseHandle(f);
#define RD32(v) do { if (pos + 4 > size) return; fc_cpy(&(v), buf + pos, 4); pos += 4; } while (0)
    {
        DWORD magic, slo, shi;
        RD32(magic); RD32(slo); RD32(shi); RD32(nent);
        if (magic != FC_MAGIC) return;
        if (slo != (DWORD)fc_sig || shi != (DWORD)(fc_sig >> 32)) { fc_st.mismatch = 1; return; }
    }
    for (i = 0; i < nent; i++) {
        DWORD kl, n, j;
        char *key;
        Item *items = 0;
        RD32(kl);
        if (kl == 0 || kl > 512 || pos + kl > size) return;
        key = (char *)buf + pos;                       /* stored with terminator */
        if (key[kl - 1]) return;
        pos += kl;
        RD32(n);
        if (n > 100000) return;
        if (n) { items = fc_alloc(n * sizeof(Item)); if (!items) return; }
        for (j = 0; j < n; j++) {
            DWORD nl;
            RD32(nl);
            if (nl < 2 || nl > 300 || pos + nl > size) return;
            items[j].isdir = buf[pos];
            items[j].name = (char *)buf + pos + 1;
            if (items[j].name[nl - 2]) return;
            pos += nl;
        }
        if (fc_insert(key, n, items)) fc_st.loaded++;
    }
#undef RD32
}

static int fc_wr(HANDLE f, const void *p, DWORD n) { DWORD w; return WriteFile(f, p, n, &w, 0) && w == n; }

/* Called periodically from the plugin's worker thread. */
void fc_maybe_save(void)
{
    char tmp[530];
    HANDLE f;
    DWORD nent = 0, i, ok = 1, hdr[4], dirty;
    unsigned n;
    if (!fc_on || !fc_sig || fc_tainted || !fc_dirty) return;
    EnterCriticalSection(&fc_cs);
    dirty = fc_dirty;
    n = fc_len(fc_path);
    fc_cpy(tmp, fc_path, n);
    fc_cpy(tmp + n, ".tmp", 5);
    f = CreateFileA(tmp, 0x40000000UL, 0, 0, 2 /*CREATE_ALWAYS*/, 0x80, 0);
    if (f == INVALID_HANDLE_VALUE) { LeaveCriticalSection(&fc_cs); return; }
    for (i = 0; i < NBUCKET; i++) { CEntry *e; for (e = fc_bucket[i]; e; e = e->next) nent++; }
    hdr[0] = FC_MAGIC; hdr[1] = (DWORD)fc_sig; hdr[2] = (DWORD)(fc_sig >> 32); hdr[3] = nent;
    ok &= fc_wr(f, hdr, sizeof hdr);
    for (i = 0; i < NBUCKET && ok; i++) {
        CEntry *e;
        for (e = fc_bucket[i]; e && ok; e = e->next) {
            DWORD kl = fc_len(e->key) + 1, j;
            ok &= fc_wr(f, &kl, 4) && fc_wr(f, e->key, kl) && fc_wr(f, &e->n, 4);
            for (j = 0; j < e->n && ok; j++) {
                DWORD nl = fc_len(e->items[j].name) + 2;
                ok &= fc_wr(f, &nl, 4) && fc_wr(f, &e->items[j].isdir, 1) && fc_wr(f, e->items[j].name, nl - 1);
            }
        }
    }
    CloseHandle(f);
    if (ok && MoveFileExA(tmp, fc_path, 1 /*REPLACE_EXISTING*/)) {
        fc_st.saved = nent;
        if (fc_dirty == dirty) fc_dirty = 0;
    }
    LeaveCriticalSection(&fc_cs);
}

#ifndef FASTFS_HOST_TEST
/* ---- installation ---- */
static int vt_patch(void **slot, void *hook, void **real_out)
{
    DWORD old;
    if (!VirtualProtect(slot, sizeof(void *), 0x04, &old)) return 0;
    *real_out = *slot;
    *slot = hook;
    VirtualProtect(slot, sizeof(void *), old, &old);
    return 1;
}

/* fsbase: FileSystem_Stdio.dll base; sig: content signature (0 = don't persist).
 * Returns 1 if hooks were installed. */
int fc_install(BYTE *fsbase, U64 sig, const char *loaderdir)
{
    DWORD b = (DWORD)fsbase;
    void **vt = (void **)(fsbase + 0x1d240);           /* FindFirst slot */
    unsigned n = 0;
    /* Exact build check: slot contents must point at the expected functions. */
    if ((DWORD)vt[0] != b + 0x3bf0 || (DWORD)vt[1] != b + 0x4420 || (DWORD)vt[2] != b + 0x4500 ||
        (DWORD)vt[3] != b + 0x4520 || (DWORD)vt[24] != b + 0x3c10)
        return 0;
    InitializeCriticalSection(&fc_cs);
    fc_heap = HeapCreate(0, 1 << 20, 0);
    if (!fc_heap) return 0;
    fc_sig = sig;
    while (loaderdir[n] && n < 480) { fc_path[n] = loaderdir[n]; n++; }
    fc_cpy(fc_path + n, "vtmb-fastfs-find.cache", 23);
    if (fc_sig) fc_load();
    fc_on = 1;
    /* FindNext/IsDir/Close first, so fake handles can never reach the originals. */
    if (!vt_patch(&vt[1], (void *)hk_FindNext, (void **)&real_FindNext)) return fc_on = 0, 0;
    if (!vt_patch(&vt[2], (void *)hk_FindIsDir, (void **)&real_FindIsDir)) return fc_on = 0, 0;
    if (!vt_patch(&vt[3], (void *)hk_FindClose, (void **)&real_FindClose)) return fc_on = 0, 0;
    if (!vt_patch(&vt[24], (void *)hk_FindFirstEx, (void **)&real_FindFirstEx)) return fc_on = 0, 0;
    return 1;
}
#endif
