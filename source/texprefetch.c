/*
 * vtmb-fastfs: texture prefetch ("Prefetch Textures = 1", needs Fast Inflate).
 *
 * Even with libdeflate, decompressing a level's textures (~1,000 .ttz textures,
 * ~130 MB of output for the hub) costs the loading thread ~200 ms, while the
 * other CPU cores sit idle.
 *
 * Learning: while a level loads, every texture the game decompresses is noted
 * with where its compressed bytes came from - the file (a VPK archive or a loose
 * .ttz) and the offset in it, found from the file reads of FileSystem_Stdio.dll
 * that filled the buffer - plus its sizes and a content fingerprint. The list
 * for each map is saved to Bin\loader\vtmb-fastfs-tex.cache.
 *
 * Prefetching: when a map that has a list is opened again, worker threads (one
 * per spare CPU core, at most 4) read those compressed textures themselves and
 * decompress them ahead of the game, in the order the game will want them.
 * When the game then asks to decompress a texture, the prefetched result is
 * used only if the game's compressed input is byte-for-byte identical to what
 * the worker decompressed; it is then copied out. Anything else - textures not
 * on the list, changed files, a worker that hasn't got there yet - is
 * decompressed as before. Output is the same either way.
 *
 * At most PF_BUDGET bytes of prefetched output are held at once, and anything
 * not used within PF_TIMEOUT ms of the map opening is dropped.
 */

#include "libdeflate/libdeflate.h"

typedef unsigned int   DWORD;
typedef int            BOOL;
typedef void          *HANDLE;
typedef unsigned char  BYTE;
typedef unsigned short WORD;
typedef const char    *LPCSTR;
#define WINAPI __stdcall
#define INVALID_HANDLE_VALUE ((HANDLE)(long)-1)

typedef struct { WORD arch; WORD r; DWORD page; void *mn, *mx; DWORD mask; DWORD ncpu; DWORD t; DWORD g; WORD l, rv; } SYSTEM_INFO_;

__declspec(dllimport) HANDLE WINAPI CreateThread(void *, DWORD, DWORD (WINAPI *)(void *), void *, DWORD, DWORD *);
__declspec(dllimport) BOOL   WINAPI CloseHandle(HANDLE);
__declspec(dllimport) void   WINAPI Sleep(DWORD);
__declspec(dllimport) DWORD  WINAPI GetTickCount(void);
__declspec(dllimport) HANDLE WINAPI GetProcessHeap(void);
__declspec(dllimport) void * WINAPI HeapAlloc(HANDLE, DWORD, DWORD);
__declspec(dllimport) BOOL   WINAPI HeapFree(HANDLE, DWORD, void *);
__declspec(dllimport) long   WINAPI InterlockedExchange(volatile long *, long);
__declspec(dllimport) long   WINAPI InterlockedCompareExchange(volatile long *, long, long);
__declspec(dllimport) long   WINAPI InterlockedIncrement(volatile long *);
__declspec(dllimport) long   WINAPI InterlockedDecrement(volatile long *);
__declspec(dllimport) long   WINAPI InterlockedExchangeAdd(volatile long *, long);
__declspec(dllimport) HANDLE WINAPI CreateFileA(LPCSTR, DWORD, DWORD, void *, DWORD, DWORD, HANDLE);
__declspec(dllimport) BOOL   WINAPI ReadFile(HANDLE, void *, DWORD, DWORD *, void *);
__declspec(dllimport) BOOL   WINAPI WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
__declspec(dllimport) DWORD  WINAPI SetFilePointer(HANDLE, long, long *, DWORD);
__declspec(dllimport) DWORD  WINAPI GetFileSize(HANDLE, DWORD *);
__declspec(dllimport) BOOL   WINAPI MoveFileExA(LPCSTR, LPCSTR, DWORD);
__declspec(dllimport) void   WINAPI GetSystemInfo(SYSTEM_INFO_ *);
__declspec(dllimport) DWORD  WINAPI GetLastError(void);
__declspec(dllimport) void   WINAPI SetLastError(DWORD);
__declspec(dllimport) BOOL   WINAPI SetThreadPriority(HANDLE, int);

void sl_mark(const char *text);

volatile DWORD tp_on, tp_served, tp_selfdone, tp_mismatch, tp_waited_ms, tp_learned, tp_maps_known, tp_prefetched;

#define PF_BUDGET   (192u << 20)
#define PF_TIMEOUT  30000u
#define MAXPATHS    256
#define MAXMAPS     256
#define MAXREC      8192
#define MAXPATHLEN  260

static void *xalloc(DWORD n) { return HeapAlloc(GetProcessHeap(), 0, n); }
static void xfree(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }
static unsigned slen(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static void mcpy(void *d, const void *s, DWORD n) { BYTE *a = d; const BYTE *b = s; while (n--) *a++ = *b++; }
static int streq(const char *a, const char *b) { while (*a && *a == *b) a++, b++; return *a == *b; }
static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static int ends_ci(const char *s, const char *suf)
{
    unsigned n = slen(s), m = slen(suf), i;
    if (n < m) return 0;
    for (i = 0; i < m; i++) if (lc(s[n - m + i]) != suf[i]) return 0;
    return 1;
}
void *memcpy(void *d, const void *s, unsigned n);
/* byte-for-byte equality, a word at a time (the compiler vectorises the inner loop) */
static int same_bytes(const BYTE *a, const BYTE *b, DWORD n)
{
    DWORD i = 0;
    while (n - i >= 64) {
        const DWORD *x = (const DWORD *)(a + i), *y = (const DWORD *)(b + i);
        DWORD d = 0, k;
        for (k = 0; k < 16; k++) d |= x[k] ^ y[k];
        if (d) return 0;
        i += 64;
    }
    for (; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}
static char *sdup(const char *s) { unsigned n = slen(s) + 1; char *d = xalloc(n); if (d) mcpy(d, s, n); return d; }

DWORD tp_key(const BYTE *p, DWORD n)
{
    DWORD h = 2166136261u ^ n, i, step = n / 61 + 1;
    for (i = 0; i < n; i += step) { h ^= p[i]; h *= 16777619u; }
    for (i = n > 32 ? n - 32 : 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h ? h : 1;
}

/* ---------------------------------------------------------------- learned data */
typedef struct { WORD path; WORD pad; DWORD off, alen, olen, key; } Rec;
typedef struct { char *map; DWORD n; Rec *r; } MapList;

static char *g_paths[MAXPATHS];
static DWORD g_npaths;
static MapList g_maps[MAXMAPS];
static DWORD g_nmaps;
static volatile long g_data_lock;        /* guards g_paths/g_maps and recording */
static char g_cachefile[MAXPATHLEN], g_cachetmp[MAXPATHLEN];

static void lock(volatile long *l) { while (InterlockedExchange(l, 1)) Sleep(0); }
static void unlock(volatile long *l) { InterlockedExchange(l, 0); }

static int path_id(const char *p)        /* caller holds g_data_lock */
{
    DWORD i;
    for (i = 0; i < g_npaths; i++) if (streq(g_paths[i], p)) return (int)i;
    if (g_npaths >= MAXPATHS || slen(p) >= MAXPATHLEN) return -1;
    if (!(g_paths[g_npaths] = sdup(p))) return -1;
    return (int)g_npaths++;
}
static MapList *find_map(const char *m)
{
    DWORD i;
    for (i = 0; i < g_nmaps; i++) if (streq(g_maps[i].map, m)) return &g_maps[i];
    return 0;
}

/* file format: "VTPF0001", u32 npaths, {u16 len, bytes}, u32 nmaps, {u16 len, bytes, u32 n, Rec[n]} */
typedef struct { BYTE *p; DWORD n, cap; } Buf;
static void put(Buf *b, const void *d, DWORD n)
{
    if (b->n + n > b->cap) {
        DWORD c = b->cap ? b->cap * 2 : 65536; BYTE *q;
        while (c < b->n + n) c *= 2;
        q = xalloc(c); if (!q) { b->cap = 0; return; }
        if (b->p) { mcpy(q, b->p, b->n); xfree(b->p); }
        b->p = q; b->cap = c;
    }
    if (b->p) { mcpy(b->p + b->n, d, n); b->n += n; }
}
static void put_str(Buf *b, const char *s) { WORD n = (WORD)slen(s); put(b, &n, 2); put(b, s, n); }

static void save_cache(void)                 /* caller holds g_data_lock */
{
    Buf b = { 0, 0, 0 };
    DWORD i, w;
    HANDLE f;
    put(&b, "VTPF0001", 8);
    put(&b, &g_npaths, 4);
    for (i = 0; i < g_npaths; i++) put_str(&b, g_paths[i]);
    put(&b, &g_nmaps, 4);
    for (i = 0; i < g_nmaps; i++) { put_str(&b, g_maps[i].map); put(&b, &g_maps[i].n, 4); put(&b, g_maps[i].r, g_maps[i].n * sizeof(Rec)); }
    if (!b.p || !b.cap) { xfree(b.p); return; }
    f = CreateFileA(g_cachetmp, 0x40000000, 0, 0, 2, 0x80, 0);
    if (f != INVALID_HANDLE_VALUE) {
        BOOL ok = WriteFile(f, b.p, b.n, &w, 0) && w == b.n;
        CloseHandle(f);
        if (ok) MoveFileExA(g_cachetmp, g_cachefile, 1 /*REPLACE_EXISTING*/);
    }
    xfree(b.p);
}

static void load_cache(void)
{
    HANDLE f = CreateFileA(g_cachefile, 0x80000000, 1, 0, 3, 0x80, 0);
    DWORD size, got, pos = 8, i, n;
    BYTE *d;
    if (f == INVALID_HANDLE_VALUE) return;
    size = GetFileSize(f, 0);
    d = (size > 16 && size < (64u << 20)) ? xalloc(size + 1) : 0;
    if (!d || !ReadFile(f, d, size, &got, 0) || got != size) { CloseHandle(f); xfree(d); return; }
    CloseHandle(f);
#define NEED(k) if (pos + (k) > size) goto bad
    for (i = 0; i < 8; i++) if (d[i] != (BYTE)"VTPF0001"[i]) goto bad;
    NEED(4); n = *(DWORD *)(d + pos); pos += 4;
    if (n > MAXPATHS) goto bad;
    for (i = 0; i < n; i++) {
        WORD l; char *s;
        NEED(2); l = *(WORD *)(d + pos); pos += 2;
        NEED(l); if (l >= MAXPATHLEN) goto bad;
        s = xalloc(l + 1u); if (!s) goto bad;
        mcpy(s, d + pos, l); s[l] = 0; pos += l;
        g_paths[g_npaths++] = s;
    }
    NEED(4); n = *(DWORD *)(d + pos); pos += 4;
    if (n > MAXMAPS) goto bad;
    for (i = 0; i < n; i++) {
        WORD l; DWORD k, j; MapList *m = &g_maps[g_nmaps];
        NEED(2); l = *(WORD *)(d + pos); pos += 2;
        NEED(l); if (l >= MAXPATHLEN) goto bad;
        m->map = xalloc(l + 1u); if (!m->map) goto bad;
        mcpy(m->map, d + pos, l); m->map[l] = 0; pos += l;
        NEED(4); k = *(DWORD *)(d + pos); pos += 4;
        if (k > MAXREC) { xfree(m->map); goto bad; }
        NEED(k * sizeof(Rec));
        m->r = xalloc(k * sizeof(Rec) + 1); if (!m->r) { xfree(m->map); goto bad; }
        mcpy(m->r, d + pos, k * sizeof(Rec)); pos += k * sizeof(Rec);
        for (j = 0; j < k; j++) if (m->r[j].path >= g_npaths) { k = j; break; }
        m->n = k;
        g_nmaps++;
    }
#undef NEED
bad:
    xfree(d);
    tp_maps_known = g_nmaps;
}

/* ---------------------------------------------------------------- file read tracking */
typedef BOOL (WINAPI *PFN_ReadFile)(HANDLE, void *, DWORD, DWORD *, void *);
PFN_ReadFile tp_real_ReadFile;

#define NH 1024
static HANDLE h_handle[NH];
static short  h_path[NH];
static volatile long g_h_lock;

void tp_note_open(HANDLE h, const char *name)
{
    DWORD i, k, slot = 0xffffffffu;
    int id = -1;
    if (!tp_on || h == INVALID_HANDLE_VALUE || !h || !name) return;
    if (ends_ci(name, ".vpk") || ends_ci(name, ".ttz")) {
        lock(&g_data_lock); id = path_id(name); unlock(&g_data_lock);
    }
    lock(&g_h_lock);
    for (i = ((DWORD)h >> 2) & (NH - 1), k = 0; k < NH && h_handle[i]; k++, i = (i + 1) & (NH - 1)) {
        if (h_handle[i] == h) { slot = i; break; }
        if (h_handle[i] == INVALID_HANDLE_VALUE && slot == 0xffffffffu) slot = i;
    }
    if (slot == 0xffffffffu && k < NH && !h_handle[i]) slot = i;
    if (slot != 0xffffffffu) {
        if (id >= 0) { h_handle[slot] = h; h_path[slot] = (short)id; }
        else if (h_handle[slot] == h) h_handle[slot] = INVALID_HANDLE_VALUE;   /* reused handle: forget */
    }
    unlock(&g_h_lock);
}
static int handle_path(HANDLE h)
{
    DWORD i, k; int r = -1;
    lock(&g_h_lock);
    for (i = ((DWORD)h >> 2) & (NH - 1), k = 0; k < NH && h_handle[i]; k++, i = (i + 1) & (NH - 1))
        if (h_handle[i] == h) { r = h_path[i]; break; }
    unlock(&g_h_lock);
    return r;
}

typedef struct { const BYTE *buf; DWORD len, off; int path; } ReadRec;
#define NRING 64
static ReadRec ring[NRING];
static DWORD ring_pos;
static volatile long g_ring_lock;

BOOL WINAPI tp_hk_ReadFile(HANDLE h, void *buf, DWORD len, DWORD *got, void *ov)
{
    int path;
    DWORD off = 0xffffffffu, err;
    BOOL r;
    if (!tp_on || ov || len < 2048 || (path = handle_path(h)) < 0)
        return tp_real_ReadFile(h, buf, len, got, ov);
    {
        long hi = 0;
        off = SetFilePointer(h, 0, &hi, 1 /*FILE_CURRENT*/);
        if (hi) off = 0xffffffffu;
    }
    r = tp_real_ReadFile(h, buf, len, got, ov);
    err = GetLastError();
    if (r && got && *got && off != 0xffffffffu) {
        lock(&g_ring_lock);
        ring[ring_pos].buf = (const BYTE *)buf; ring[ring_pos].len = *got; ring[ring_pos].off = off; ring[ring_pos].path = path;
        ring_pos = (ring_pos + 1) % NRING;
        unlock(&g_ring_lock);
    }
    SetLastError(err);
    return r;
}

/* ---------------------------------------------------------------- recording */
static char  *rec_map;
static Rec   *rec_r;
static DWORD  rec_n;
static DWORD  rec_start;

static DWORD rec_saved_n, rec_last_add;
/* store the list recorded so far for the current map (recording goes on) */
static void rec_snapshot(void)                /* caller holds g_data_lock */
{
    MapList *m;
    Rec *copy;
    DWORD i;
    if (!rec_map || !rec_r || rec_n < 16 || rec_n == rec_saved_n) return;
    m = find_map(rec_map);
    if (!m && g_nmaps < MAXMAPS) {
        m = &g_maps[g_nmaps];
        if (!(m->map = sdup(rec_map))) return;
        m->r = 0; m->n = 0; g_nmaps++;
    }
    if (!m || !(copy = xalloc(rec_n * sizeof(Rec)))) return;
    for (i = 0; i < rec_n; i++) copy[i] = rec_r[i];
    xfree(m->r); m->r = copy; m->n = rec_n;
    rec_saved_n = rec_n;
    tp_maps_known = g_nmaps;
    save_cache();
}

static void rec_finish(void)                 /* caller holds g_data_lock */
{
    rec_snapshot();
    xfree(rec_map); xfree(rec_r);
    rec_map = 0; rec_r = 0; rec_n = 0; rec_saved_n = 0;
}

void tp_flush(void)
{
    if (!tp_on) return;
    lock(&g_data_lock); rec_snapshot(); unlock(&g_data_lock);
}

/* called by the inflate front end for every texture it decompressed or was served */
void tp_record(const BYTE *in, DWORD alen, DWORD olen, DWORD key)
{
    DWORD i, off = 0xffffffffu; int path = -1;
    if (!tp_on || !rec_map) return;
    lock(&g_ring_lock);
    for (i = 0; i < NRING; i++) {
        ReadRec *r = &ring[(ring_pos + NRING - 1 - i) % NRING];   /* newest first */
        if (!r->buf || !r->len) continue;
        if (r->buf < in + alen && r->buf + r->len > in) {      /* this read filled part of the input */
            if (r->buf <= in) { off = r->off + (DWORD)(in - r->buf); }
            else if (r->off >= (DWORD)(r->buf - in)) { off = r->off - (DWORD)(r->buf - in); }
            path = r->path;
            break;
        }
    }
    unlock(&g_ring_lock);
    if (path < 0 || off == 0xffffffffu) return;
    lock(&g_data_lock);
    if (rec_map && rec_r && rec_n < MAXREC) {
        for (i = 0; i < rec_n; i++) if (rec_r[i].key == key && rec_r[i].alen == alen) break;   /* repeat */
        if (i == rec_n) {
            Rec *r = &rec_r[rec_n++];
            r->path = (WORD)path; r->pad = 0; r->off = off; r->alen = alen; r->olen = olen; r->key = key;
            tp_learned++;
            rec_last_add = GetTickCount();
        }
    }
    unlock(&g_data_lock);
}

/* ---------------------------------------------------------------- prefetching */
enum { E_QUEUED, E_WORKING, E_READY, E_TAKEN, E_FAILED };
typedef struct {
    volatile long state;
    Rec r;
    BYTE *comp, *out;
    DWORD in_used;
} Entry;
typedef struct {
    DWORD n, mask;
    Entry *e;
    int *tab;
    volatile long next, stop, workers;
    volatile long budget;
    char *paths[MAXPATHS];
    DWORD npaths, start;
} Session;

static Session *volatile g_sess;
static volatile long g_sess_lock;           /* held by the consumer while it uses g_sess */

static void entry_free(Session *s, Entry *e)
{
    if (e->out) InterlockedExchangeAdd(&s->budget, -(long)e->r.olen);
    xfree(e->comp); xfree(e->out); e->comp = 0; e->out = 0;
}

static void *__cdecl pmalloc(size_t n) { return xalloc((DWORD)n); }
static void __cdecl pfree(void *p) { xfree(p); }

static DWORD WINAPI worker(void *arg)
{
    Session *s = (Session *)arg;
    struct libdeflate_decompressor *dec = libdeflate_alloc_decompressor();
    HANDLE f = INVALID_HANDLE_VALUE;
    int fpath = -1;
    long i;
    SetThreadPriority((HANDLE)(long)-2 /*current thread*/, -1 /*below normal*/);
    while (dec && !s->stop && (i = InterlockedIncrement(&s->next) - 1) < (long)s->n) {
        Entry *e = &s->e[i];
        DWORD got, lo; size_t used = 0, outn = 0;
        /* wait for room before claiming the entry, so the game never waits on a worker that waits for memory */
        while (!s->stop && e->state == E_QUEUED && s->budget + (long)e->r.olen > (long)PF_BUDGET) Sleep(1);
        if (s->stop) break;
        if (InterlockedCompareExchange(&e->state, E_WORKING, E_QUEUED) != E_QUEUED) continue;
        if (fpath != e->r.path) {
            if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
            f = CreateFileA(s->paths[e->r.path], 0x80000000, 7, 0, 3, 0x80, 0);
            fpath = e->r.path;
        }
        e->comp = xalloc(e->r.alen);
        e->out = xalloc(e->r.olen ? e->r.olen : 1);
        if (e->out) InterlockedExchangeAdd(&s->budget, (long)e->r.olen);
        lo = f != INVALID_HANDLE_VALUE ? SetFilePointer(f, (long)e->r.off, 0, 0) : 0xffffffffu;
        if (!e->comp || !e->out || lo != e->r.off || !ReadFile(f, e->comp, e->r.alen, &got, 0) || got != e->r.alen ||
            libdeflate_zlib_decompress_ex(dec, e->comp, e->r.alen, e->out, e->r.olen, &used, &outn) != LIBDEFLATE_SUCCESS ||
            outn != e->r.olen) {
            entry_free(s, e);
            e->state = E_FAILED;
            continue;
        }
        e->in_used = (DWORD)used;
        InterlockedIncrement((volatile long *)&tp_prefetched);
        e->state = E_READY;
    }
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    if (dec) libdeflate_free_decompressor(dec);
    InterlockedDecrement(&s->workers);
    return 0;
}

static void session_end(void)
{
    Session *s;
    DWORD i;
    lock(&g_sess_lock);
    s = g_sess; g_sess = 0;
    unlock(&g_sess_lock);
    if (!s) return;
    s->stop = 1;
    while (s->workers) Sleep(1);
    for (i = 0; i < s->n; i++) entry_free(s, &s->e[i]);
    for (i = 0; i < s->npaths; i++) xfree(s->paths[i]);
    xfree(s->e); xfree(s->tab); xfree(s);
}

static DWORD g_nworkers = 1;

static void session_start(const MapList *m)
{
    Session *s = xalloc(sizeof(Session));
    DWORD i, size, t;
    if (!s) return;
    { BYTE *z = (BYTE *)s; for (i = 0; i < sizeof(Session); i++) z[i] = 0; }
    for (size = 16; size < m->n * 2; size <<= 1) {}
    s->n = m->n; s->mask = size - 1;
    s->e = xalloc(m->n * sizeof(Entry) + 1);
    s->tab = xalloc(size * sizeof(int));
    if (!s->e || !s->tab) { xfree(s->e); xfree(s->tab); xfree(s); return; }
    for (i = 0; i < size; i++) s->tab[i] = -1;
    lock(&g_data_lock);
    for (i = 0; i < g_npaths; i++) s->paths[i] = sdup(g_paths[i]);
    s->npaths = g_npaths;
    unlock(&g_data_lock);
    for (i = 0; i < m->n; i++) {
        Entry *e = &s->e[i];
        DWORD j;
        e->state = (m->r[i].path < s->npaths && s->paths[m->r[i].path]) ? E_QUEUED : E_FAILED;
        e->r = m->r[i]; e->comp = e->out = 0; e->in_used = 0;
        for (j = m->r[i].key & s->mask; s->tab[j] >= 0; j = (j + 1) & s->mask) {}
        s->tab[j] = (int)i;
    }
    s->start = GetTickCount();
    for (t = 0; t < g_nworkers; t++) {
        DWORD tid; HANDLE th;
        InterlockedIncrement(&s->workers);
        th = CreateThread(0, 0, worker, s, 0, &tid);
        if (th) CloseHandle(th); else InterlockedDecrement(&s->workers);
    }
    g_sess = s;
}

/*
 * Called by the inflate front end before decompressing. Returns 1 if out[] now
 * holds the decompressed texture (*in_used, *out_used set), 0 to decompress
 * as usual.
 */
int tp_consume(const BYTE *in, DWORD alen, DWORD key, BYTE *out, DWORD avail_out, DWORD *in_used, DWORD *out_used)
{
    Session *s;
    DWORD j;
    int served = 0;
    if (!g_sess) return 0;
    lock(&g_sess_lock);
    s = g_sess;
    if (s) {
        for (j = key & s->mask; s->tab[j] >= 0; j = (j + 1) & s->mask) {
            Entry *e = &s->e[s->tab[j]];
            long st;
            DWORD i;
            if (e->r.key != key || e->r.alen != alen) continue;
            st = InterlockedCompareExchange(&e->state, E_TAKEN, E_QUEUED);
            if (st == E_QUEUED) { tp_selfdone++; break; }         /* not started: decompress here */
            if (st == E_WORKING) {
                DWORD t0 = GetTickCount();
                while (e->state == E_WORKING && GetTickCount() - t0 < 1000) Sleep(0);
                tp_waited_ms += GetTickCount() - t0;
                st = e->state;
            }
            if (st != E_READY) break;                               /* failed or too slow: decompress here */
            e->state = E_TAKEN;
            if (e->r.olen <= avail_out) {
                if (same_bytes(e->comp, in, alen)) {
                    memcpy(out, e->out, e->r.olen);
                    *in_used = e->in_used; *out_used = e->r.olen;
                    served = 1; tp_served++;
                } else tp_mismatch++;
            }
            entry_free(s, e);
            break;
        }
    }
    unlock(&g_sess_lock);
    return served;
}

/* a map is being opened (main thread) */
void tp_map_open(const char *key)
{
    MapList *m;
    if (!tp_on) return;
    lock(&g_data_lock);
    if (rec_map && streq(rec_map, key) && GetTickCount() - rec_start < 15000) { unlock(&g_data_lock); return; }  /* same load */
    unlock(&g_data_lock);
    session_end();
    lock(&g_data_lock);
    rec_finish();
    rec_map = sdup(key);
    rec_r = xalloc(MAXREC * sizeof(Rec));
    rec_n = 0; rec_saved_n = 0;
    rec_start = rec_last_add = GetTickCount();
    m = find_map(key);
    unlock(&g_data_lock);
    if (m && m->n) session_start(m);
}

/* housekeeping: end the recording and the prefetch session PF_TIMEOUT ms after the map opened */
static DWORD WINAPI manager(void *unused)
{
    (void)unused;
    for (;;) {
        Sleep(250);
        if (rec_map && rec_n != rec_saved_n && GetTickCount() - rec_last_add > 2000) {
            lock(&g_data_lock);
            if (rec_map && GetTickCount() - rec_last_add > 2000) rec_snapshot();
            unlock(&g_data_lock);
        }
        if (rec_map && GetTickCount() - rec_start > PF_TIMEOUT) {
            session_end();
            lock(&g_data_lock);
            if (rec_map && GetTickCount() - rec_start > PF_TIMEOUT) rec_finish();
            unlock(&g_data_lock);
        }
    }
}

void tp_start(const char *exedir)
{
    SYSTEM_INFO_ si;
    unsigned n = slen(exedir);
    DWORD tid; HANDLE t;
    if (n + 40 >= MAXPATHLEN) return;
    mcpy(g_cachefile, exedir, n); mcpy(g_cachefile + n, "Bin\\loader\\vtmb-fastfs-tex.cache", 33);
    mcpy(g_cachetmp, exedir, n);  mcpy(g_cachetmp + n, "Bin\\loader\\vtmb-fastfs-tex.tmp", 31);
    libdeflate_set_memory_allocator(pmalloc, pfree);
    load_cache();
    GetSystemInfo(&si);
    g_nworkers = si.ncpu > 1 ? si.ncpu - 1 : 1;
    if (g_nworkers > 4) g_nworkers = 4;
    t = CreateThread(0, 0, manager, 0, 0, &tid);
    if (t) CloseHandle(t);
    tp_on = 1;
}
