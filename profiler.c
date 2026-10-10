/*
 * vtmb-fastfs load profiler (diagnostic, off by default; "Profile Loads = 1").
 *
 * When the filesystem opens a map (.bsp), every thread of the game is
 * sampled for the next 20 seconds:
 *   - the loading thread every ~2 ms, other threads every ~8 ms if they used
 *     CPU since their last sample (per their cycle counter): instruction
 *     pointer and up to 20 code-looking stack values;
 *   - every ~50 ms, each thread's cycle counter is recorded, so busy and
 *     waiting time can be told apart.
 * The module list and the export tables of Windows DLLs are saved with the
 * samples so addresses can be named afterwards. Output, one per map load:
 *   <game folder>\vtmb-profile-<n>.bin
 *
 * The sampler never allocates or takes locks while a thread is suspended;
 * all buffers are preallocated.
 */

typedef unsigned long  DWORD;
typedef int            BOOL;
typedef void          *HANDLE;
typedef unsigned char  BYTE;
typedef unsigned short WORD;
typedef const char    *LPCSTR;
typedef unsigned long long U64;
#define WINAPI __stdcall
#define INVALID_HANDLE_VALUE ((HANDLE)(long)-1)

__declspec(dllimport) DWORD  WINAPI GetCurrentThreadId(void);
__declspec(dllimport) DWORD  WINAPI GetCurrentProcessId(void);
__declspec(dllimport) HANDLE WINAPI OpenThread(DWORD, BOOL, DWORD);
__declspec(dllimport) DWORD  WINAPI SuspendThread(HANDLE);
__declspec(dllimport) DWORD  WINAPI ResumeThread(HANDLE);
__declspec(dllimport) BOOL   WINAPI GetThreadContext(HANDLE, void *);
__declspec(dllimport) BOOL   WINAPI QueryThreadCycleTime(HANDLE, U64 *);
__declspec(dllimport) BOOL   WINAPI ReadProcessMemory(HANDLE, const void *, void *, DWORD, DWORD *);
__declspec(dllimport) HANDLE WINAPI GetCurrentProcess(void);
__declspec(dllimport) HANDLE WINAPI CreateToolhelp32Snapshot(DWORD, DWORD);
__declspec(dllimport) BOOL   WINAPI Module32First(HANDLE, void *);
__declspec(dllimport) BOOL   WINAPI Module32Next(HANDLE, void *);
__declspec(dllimport) BOOL   WINAPI Thread32First(HANDLE, void *);
__declspec(dllimport) BOOL   WINAPI Thread32Next(HANDLE, void *);
__declspec(dllimport) BOOL   WINAPI CloseHandle(HANDLE);
__declspec(dllimport) void   WINAPI Sleep(DWORD);
__declspec(dllimport) DWORD  WINAPI GetTickCount(void);
__declspec(dllimport) HANDLE WINAPI CreateFileA(LPCSTR, DWORD, DWORD, void *, DWORD, DWORD, HANDLE);
__declspec(dllimport) BOOL   WINAPI WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
__declspec(dllimport) HANDLE WINAPI CreateThread(void *, DWORD, DWORD (WINAPI *)(void *), void *, DWORD, DWORD *);
__declspec(dllimport) void * WINAPI VirtualAlloc(void *, DWORD, DWORD, DWORD);
__declspec(dllimport) unsigned WINAPI timeBeginPeriod(unsigned);
__declspec(dllimport) unsigned WINAPI timeEndPeriod(unsigned);

#define MAXMODS    160
#define MAXTHR     96
#define NSTACK     20
#define MAXSAMP    90000            /* ~8.3 MB */
#define MAXCYC     40000            /* 20 s / 50 ms * up to 100 threads */
#define MAXEXP     60000            /* exported names of Windows DLLs */
#define EXPNAMES   (1u << 20)       /* bytes for export names */
#define CAPTURE_MS 20000

typedef struct { DWORD base, size; char name[200]; } Mod;
typedef struct { DWORD tick, tid, eip, n, stack[NSTACK]; } Sample;
typedef struct { DWORD tick, tid; U64 cycles; } Cyc;
typedef struct { DWORD mod, rva, nameoff; } Exp;

static Mod    *g_mods;   static DWORD g_nmods;
static Sample *g_samp;
static Cyc    *g_cyc;
static Exp    *g_exp;    static DWORD g_nexp;
static char   *g_expnames; static DWORD g_expnlen;
static volatile DWORD g_arm_tid, g_arm_seq;
static char     g_outbase[520];
static unsigned g_outlen;
static BYTE     g_ctx[1024];        /* CONTEXT (716 bytes on x86), 16-aligned below */
static DWORD    g_self_tid;

static HANDLE   g_th[MAXTHR];
static DWORD    g_tid[MAXTHR];
static U64      g_last[MAXTHR];
static DWORD    g_nth;

static void pcpy(void *d, const void *s, unsigned n) { BYTE *p = d; const BYTE *q = s; while (n--) *p++ = *q++; }

void prof_note_map_open(void)
{
    if (!g_samp) return;
    g_arm_tid = GetCurrentThreadId();
    g_arm_seq++;
}

static int is_windows_path(const char *p)
{
    for (; *p; p++) {
        const char *a = p, *b = "\\windows\\";
        while (*b) {
            char c = *a;
            if (c >= 'A' && c <= 'Z') c += 32;
            if (c != *b) break;
            a++, b++;
        }
        if (!*b) return 1;
    }
    return 0;
}

/* Read the export table of a module already mapped in this process. */
static void collect_exports(DWORD modidx)
{
    BYTE *base = (BYTE *)g_mods[modidx].base;
    DWORD pe, rva, i, n, *names, *funcs;
    WORD *ords;
    if (*(WORD *)base != 0x5A4D) return;
    pe = *(DWORD *)(base + 0x3C);
    if (*(DWORD *)(base + pe) != 0x4550 || *(WORD *)(base + pe + 24) != 0x10B) return;
    rva = *(DWORD *)(base + pe + 24 + 96);
    if (!rva) return;
    n = *(DWORD *)(base + rva + 24);
    funcs = (DWORD *)(base + *(DWORD *)(base + rva + 28));
    names = (DWORD *)(base + *(DWORD *)(base + rva + 32));
    ords  = (WORD *)(base + *(DWORD *)(base + rva + 36));
    for (i = 0; i < n && g_nexp < MAXEXP; i++) {
        const char *nm = (const char *)(base + names[i]);
        unsigned len = 0;
        while (nm[len] && len < 120) len++;
        if (g_expnlen + len + 1 > EXPNAMES) return;
        g_exp[g_nexp].mod = modidx;
        g_exp[g_nexp].rva = funcs[ords[i]];
        g_exp[g_nexp].nameoff = g_expnlen;
        pcpy(g_expnames + g_expnlen, nm, len);
        g_expnames[g_expnlen + len] = 0;
        g_expnlen += len + 1;
        g_nexp++;
    }
}

static void snapshot_modules(void)
{
    BYTE me[600];                   /* MODULEENTRY32 (ANSI) is 548 bytes */
    HANDLE s = CreateToolhelp32Snapshot(0x8 /*TH32CS_SNAPMODULE*/, 0);
    DWORD i;
    g_nmods = 0; g_nexp = 0; g_expnlen = 0;
    if (s == INVALID_HANDLE_VALUE) return;
    *(DWORD *)me = 548;
    if (Module32First(s, me)) {
        do {
            Mod *m = &g_mods[g_nmods];
            const char *nm = (const char *)(me + 288);   /* szExePath */
            unsigned k = 0;
            m->base = *(DWORD *)(me + 20);
            m->size = *(DWORD *)(me + 24);
            while (nm[k] && k < sizeof m->name - 1) { m->name[k] = nm[k]; k++; }
            m->name[k] = 0;
            g_nmods++;
        } while (g_nmods < MAXMODS && Module32Next(s, me));
    }
    CloseHandle(s);
    for (i = 0; i < g_nmods; i++)
        if (is_windows_path(g_mods[i].name)) collect_exports(i);
}

static void refresh_threads(void)
{
    BYTE te[28];                    /* THREADENTRY32 */
    DWORD pid = GetCurrentProcessId(), i;
    HANDLE s = CreateToolhelp32Snapshot(0x4 /*TH32CS_SNAPTHREAD*/, 0);
    if (s == INVALID_HANDLE_VALUE) return;
    *(DWORD *)te = 28;
    if (Thread32First(s, te)) {
        do {
            DWORD tid = *(DWORD *)(te + 8), owner = *(DWORD *)(te + 12);
            HANDLE h;
            if (owner != pid || tid == g_self_tid) continue;
            for (i = 0; i < g_nth; i++) if (g_tid[i] == tid) break;
            if (i < g_nth || g_nth >= MAXTHR) continue;
            h = OpenThread(0x0002 | 0x0008 | 0x0040 /*SUSPEND|GET_CONTEXT|QUERY_INFO*/, 0, tid);
            if (!h) continue;
            g_th[g_nth] = h; g_tid[g_nth] = tid; g_last[g_nth] = 0;
            QueryThreadCycleTime(h, &g_last[g_nth]);
            g_nth++;
        } while (Thread32Next(s, te));
    }
    CloseHandle(s);
}

static int in_module(DWORD a)
{
    DWORD i;
    for (i = 0; i < g_nmods; i++)
        if (a - g_mods[i].base < g_mods[i].size) return 1;
    return 0;
}

static void wr(HANDLE f, const void *p, DWORD n)
{
    DWORD w;
    const BYTE *b = p;
    while (n) {
        DWORD c = n > (1u << 20) ? (1u << 20) : n;
        if (!WriteFile(f, b, c, &w, 0) || !w) return;
        b += w; n -= w;
    }
}

static void write_profile(unsigned seq, DWORD nsamp, DWORD ncyc, DWORD main_tid)
{
    char path[540];
    unsigned n = g_outlen;
    DWORD hdr[10];
    HANDLE f;
    char num[12];
    int k = 0;
    pcpy(path, g_outbase, n);
    do { num[k++] = (char)('0' + seq % 10); seq /= 10; } while (seq);
    while (k) path[n++] = num[--k];
    pcpy(path + n, ".bin", 5);
    f = CreateFileA(path, 0x40000000UL, 1, 0, 2 /*CREATE_ALWAYS*/, 0x80, 0);
    if (f == INVALID_HANDLE_VALUE) return;
    hdr[0] = 0x32465056;            /* "VPF2" */
    hdr[1] = g_nmods; hdr[2] = nsamp; hdr[3] = NSTACK; hdr[4] = ncyc;
    hdr[5] = g_nexp;  hdr[6] = g_expnlen; hdr[7] = main_tid; hdr[8] = 0; hdr[9] = 0;
    wr(f, hdr, sizeof hdr);
    wr(f, g_mods, g_nmods * sizeof(Mod));
    wr(f, g_exp, g_nexp * sizeof(Exp));
    wr(f, g_expnames, g_expnlen);
    wr(f, g_cyc, ncyc * sizeof(Cyc));
    wr(f, g_samp, nsamp * sizeof(Sample));
    CloseHandle(f);
}

static volatile DWORD g_stop_req, g_writes, g_capturing;

static DWORD WINAPI sampler(void *unused)
{
    DWORD seen = 0;
    BYTE *ctx = (BYTE *)(((DWORD)g_ctx + 15) & ~15UL);
    (void)unused;
    g_self_tid = GetCurrentThreadId();
    for (;;) {
        DWORD seq, main_tid, start, nsamp = 0, ncyc = 0, last_cyc = 0, last_refresh = 0, i, tickno = 0;
        while ((seq = g_arm_seq) == seen) Sleep(50);
        main_tid = g_arm_tid;
        snapshot_modules();         /* may allocate: done before any suspend */
        for (i = 0; i < g_nth; i++) CloseHandle(g_th[i]);
        g_nth = 0;
        refresh_threads();
        timeBeginPeriod(1);
        g_capturing = 1;
        start = GetTickCount();
        for (;;) {
            DWORD now = GetTickCount() - start;
            if (now >= CAPTURE_MS || g_stop_req) break;
            if (now - last_refresh >= 1000) { refresh_threads(); last_refresh = now; }
            if (now - last_cyc >= 50 || ncyc == 0) {
                for (i = 0; i < g_nth && ncyc < MAXCYC; i++) {
                    U64 c = 0;
                    if (QueryThreadCycleTime(g_th[i], &c)) {
                        g_cyc[ncyc].tick = now; g_cyc[ncyc].tid = g_tid[i]; g_cyc[ncyc].cycles = c; ncyc++;
                    }
                }
                last_cyc = now;
            }
            tickno++;
            for (i = 0; i < g_nth && nsamp < MAXSAMP; i++) {
                U64 c = 0;
                /* loading thread every tick (~500/s), others every 4th (~125/s) */
                if (g_tid[i] != main_tid && (tickno & 3)) continue;
                Sample *s = &g_samp[nsamp];
                DWORD stackbuf[160], got = 0, j;
                if (!QueryThreadCycleTime(g_th[i], &c)) continue;
                if (c == g_last[i] && g_tid[i] != main_tid) continue;   /* didn't run: skip */
                g_last[i] = c;
                *(DWORD *)ctx = 0x10001 /*CONTEXT_CONTROL*/;
                if (SuspendThread(g_th[i]) == (DWORD)-1) continue;
                if (!GetThreadContext(g_th[i], ctx)) { ResumeThread(g_th[i]); continue; }
                s->tick = now;
                s->tid = g_tid[i];
                s->eip = *(DWORD *)(ctx + 0xB8);
                s->n = 0;
                if (!ReadProcessMemory(GetCurrentProcess(), (void *)*(DWORD *)(ctx + 0xC4), stackbuf, sizeof stackbuf, &got))
                    ReadProcessMemory(GetCurrentProcess(), (void *)*(DWORD *)(ctx + 0xC4), stackbuf, 128, &got);
                ResumeThread(g_th[i]);
                for (j = 0; j < got / 4 && s->n < NSTACK; j++)
                    if (in_module(stackbuf[j])) s->stack[s->n++] = stackbuf[j];
                nsamp++;
            }
            Sleep(1);
        }
        timeEndPeriod(1);
        write_profile(seq, nsamp, ncyc, main_tid);
        g_capturing = 0;
        g_writes++;
        seen = g_arm_seq;           /* map opens during this capture belong to it */
    }
}

void prof_start(const char *exedir)
{
    unsigned n = 0;
    DWORD tid;
    HANDLE t;
    while (exedir[n] && n < 480) { g_outbase[n] = exedir[n]; n++; }
    pcpy(g_outbase + n, "vtmb-profile-", 13);
    g_outlen = n + 13;
    g_mods = (Mod *)VirtualAlloc(0, MAXMODS * sizeof(Mod), 0x3000, 0x04);
    g_samp = (Sample *)VirtualAlloc(0, MAXSAMP * sizeof(Sample), 0x3000, 0x04);
    g_cyc  = (Cyc *)VirtualAlloc(0, MAXCYC * sizeof(Cyc), 0x3000, 0x04);
    g_exp  = (Exp *)VirtualAlloc(0, MAXEXP * sizeof(Exp), 0x3000, 0x04);
    g_expnames = (char *)VirtualAlloc(0, EXPNAMES, 0x3000, 0x04);
    if (!g_mods || !g_samp || !g_cyc || !g_exp || !g_expnames) { g_samp = 0; return; }
    t = CreateThread(0, 0, sampler, 0, 0, &tid);
    if (t) CloseHandle(t); else g_samp = 0;
}

/* Ends a capture in progress early and waits (up to 5 s) until it is written. */
void prof_flush(void)
{
    DWORD w = g_writes, t0 = GetTickCount();
    if (!g_samp || !g_capturing) return;
    g_stop_req = 1;
    while (g_writes == w && GetTickCount() - t0 < 5000) Sleep(10);
}
