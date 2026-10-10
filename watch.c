/*
 * vtmb-fastfs connection-state watcher (diagnostic, off by default;
 * "Watch Loads = 1", needs "Log Console = 1").
 *
 * After a map file is opened, polls a few engine.dll globals every ~1 ms for
 * 20 seconds and writes every change into vtmb-console.log (via sl_mark), so
 * engine messages and connection-state changes share one timeline. Only reads
 * memory; only runs on the exact engine.dll build it was written for.
 */

typedef unsigned int   DWORD;
typedef int            BOOL;
typedef void          *HANDLE;
typedef void          *HMODULE;
typedef unsigned char  BYTE;
typedef const char    *LPCSTR;
#define WINAPI __stdcall

__declspec(dllimport) HMODULE WINAPI GetModuleHandleA(LPCSTR);
__declspec(dllimport) HANDLE WINAPI CreateThread(void *, DWORD, DWORD (WINAPI *)(void *), void *, DWORD, DWORD *);
__declspec(dllimport) BOOL   WINAPI CloseHandle(HANDLE);
__declspec(dllimport) void   WINAPI Sleep(DWORD);
__declspec(dllimport) DWORD  WINAPI timeGetTime(void);
__declspec(dllimport) BOOL   WINAPI IsBadReadPtr(const void *, unsigned);
__declspec(dllimport) unsigned WINAPI timeBeginPeriod(unsigned);
__declspec(dllimport) unsigned WINAPI timeEndPeriod(unsigned);

void sl_mark(const char *text);

#define ENGINE_PREF 0x20000000u
typedef struct { const char *name; DWORD va; int size; } Var;
static const Var vars[] = {
    { "cls.state",        0x201dbe50, 4 },
    { "cl.signon",        0x2020b658, 4 },
    { "connect.retries",  0x2033d86c, 4 },
    { "g_201dbe5c",       0x201dbe5c, 4 },
    { "g_20315bdc",       0x20315bdc, 4 },
    { "g_2034629c(b)",    0x2034629c, 1 },
};
#define NV (sizeof vars / sizeof vars[0])
#define REALTIME_VA 0x20b42e58u
#define CLS_NETCHAN_VA 0x201dbe58u
#define HOST_CLIENT_VA 0x20b42b74u

static volatile DWORD w_seq;
static BYTE *w_eng;

void watch_note_map_open(void) { w_seq++; }

static unsigned put_str(char *b, unsigned n, const char *s) { while (*s) b[n++] = *s++; return n; }
static unsigned put_int(char *b, unsigned n, int v)
{
    char t[12]; int k = 0; unsigned u = v < 0 ? (unsigned)-v : (unsigned)v;
    if (v < 0) b[n++] = '-';
    do { t[k++] = (char)('0' + u % 10); u /= 10; } while (u);
    while (k) b[n++] = t[--k];
    return n;
}

static int rd(const Var *v)
{
    BYTE *p = w_eng + (v->va - ENGINE_PREF);
    return v->size == 1 ? (int)*p : *(int *)p;
}

static int engine_ok(BYTE *b)
{
    DWORD pe;
    if (!b || *(unsigned short *)b != 0x5A4D) return 0;
    pe = *(DWORD *)(b + 0x3C);
    return *(DWORD *)(b + pe + 8) == 0x41afaf48u && *(DWORD *)(b + pe + 24 + 56) == 0x13bb000u;
}

static DWORD WINAPI watcher(void *unused)
{
    DWORD seen = 0;
    (void)unused;
    for (;;) {
        int last[NV];
        unsigned i;
        DWORD start, frames = 0;
        unsigned long long last_rt;
        while (w_seq == seen) Sleep(20);
        seen = w_seq;
        if (!w_eng) w_eng = (BYTE *)GetModuleHandleA("engine.dll");
        if (!engine_ok(w_eng)) { sl_mark("watch: unexpected engine.dll build, not watching"); w_eng = 0; continue; }
        for (i = 0; i < NV; i++) last[i] = rd(&vars[i]);
        last_rt = *(volatile unsigned long long *)(w_eng + (REALTIME_VA - ENGINE_PREF));
        {
            char b[300]; unsigned n = put_str(b, 0, "watch start:");
            for (i = 0; i < NV; i++) { n = put_str(b, n, " "); n = put_str(b, n, vars[i].name); n = put_str(b, n, "="); n = put_int(b, n, last[i]); }
            b[n] = 0; sl_mark(b);
        }
        timeBeginPeriod(1);
        start = timeGetTime();
        {
        DWORD lastsum = 0, nc_prev[8] = {0}, sv_prev[8] = {0};
        int rel_prev[4] = {-1, -1, -1, -1};
        while (timeGetTime() - start < 20000) {
            /* --- packet flow, both ends of the loopback connection --- */
            {
                BYTE *cl = w_eng + (CLS_NETCHAN_VA - ENGINE_PREF);
                BYTE *hc = *(BYTE **)(w_eng + (0x212af1d8u - ENGINE_PREF));   /* svs.clients[0] */
                if (hc && IsBadReadPtr(hc, 0x2f860)) hc = 0;   /* never read through a stale pointer */
                BYTE *sv = hc ? hc + 0x20 : 0;
                int rel[4];
                rel[0] = *(int *)(cl + 0xfc60);                 /* client reliable bytes in flight */
                rel[1] = sv ? *(int *)(sv + 0xfc60) : -1;       /* server reliable bytes in flight */
                rel[2] = hc ? *(int *)(hc + 0x2f850) : -1;      /* server send_message flag */
                rel[3] = hc ? (*(int *)(hc + 0xc) | (*(int *)(hc + 0x10) << 1)) : -1;  /* active|spawned<<1 */
                for (i = 0; i < 4; i++) if (rel[i] != rel_prev[i] && (i != 2 || rel[i] == 0 || rel_prev[i] == 0) ) {
                    static const char *nm[4] = { "client reliable in flight", "server reliable in flight", "server send_message", "server client active|spawned<<1" };
                    if (i == 2) { rel_prev[i] = rel[i]; continue; }   /* flag toggles every frame; summarized below */
                    {
                        char b[160]; unsigned n = put_str(b, 0, "watch: ");
                        n = put_str(b, n, nm[i]); n = put_str(b, n, " "); n = put_int(b, n, rel_prev[i]);
                        n = put_str(b, n, " -> "); n = put_int(b, n, rel[i]);
                        b[n] = 0; sl_mark(b);
                    }
                    rel_prev[i] = rel[i];
                }
                /* while connecting (state 3): one line per packet event, both ends */
                if (*(int *)(w_eng + (0x201dbe50u - ENGINE_PREF)) == 3) {
                    static DWORD pk[8];
                    DWORD now[8];
                    now[0] = *(DWORD *)(cl + 0x40); now[1] = *(DWORD *)(cl + 0x30);
                    now[2] = *(DWORD *)(cl + 0x44); now[3] = *(DWORD *)(cl + 0x38);
                    now[4] = sv ? *(DWORD *)(sv + 0x40) : 0; now[5] = sv ? *(DWORD *)(sv + 0x30) : 0;
                    now[6] = sv ? *(DWORD *)(sv + 0x44) : 0; now[7] = sv ? *(DWORD *)(sv + 0x38) : 0;
                    if (now[0] != pk[0] || now[1] != pk[1] || now[4] != pk[4] || now[5] != pk[5]) {
                        char b[260]; unsigned n = put_str(b, 0, "pkt: cl out ");
                        n = put_int(b, n, (int)now[0]); n = put_str(b, n, " in "); n = put_int(b, n, (int)now[1]);
                        n = put_str(b, n, " relseq "); n = put_int(b, n, (int)now[2]); n = put_str(b, n, " relack "); n = put_int(b, n, (int)now[3]);
                        n = put_str(b, n, " relbytes "); n = put_int(b, n, rel[0]);
                        n = put_str(b, n, " | sv out "); n = put_int(b, n, (int)now[4]); n = put_str(b, n, " in "); n = put_int(b, n, (int)now[5]);
                        n = put_str(b, n, " relseq "); n = put_int(b, n, (int)now[6]); n = put_str(b, n, " relack "); n = put_int(b, n, (int)now[7]);
                        n = put_str(b, n, " relbytes "); n = put_int(b, n, rel[1]);
                        n = put_str(b, n, " signon "); n = put_int(b, n, *(int *)(w_eng + (0x2020b658u - ENGINE_PREF)));
                        if (hc) {
                            double rt = *(volatile double *)(w_eng + (REALTIME_VA - ENGINE_PREF));
                            double nm = *(volatile double *)(hc + 0x2f840), iv = *(volatile double *)(hc + 0x2f848);
                            n = put_str(b, n, " | sv nextmsg-in "); n = put_int(b, n, (int)((nm - rt) * 1000.0));
                            n = put_str(b, n, "ms interval "); n = put_int(b, n, (int)(iv * 1000.0));
                            n = put_str(b, n, "ms sendflag "); n = put_int(b, n, *(int *)(hc + 0x2f850));
                            n = put_str(b, n, " frags "); n = put_int(b, n, *(int *)(sv + 0x1f858) ? 1 : 0);
                        }
                        b[n] = 0; sl_mark(b);
                        for (i = 0; i < 8; i++) pk[i] = now[i];
                    }
                }
                if (timeGetTime() - lastsum >= 50) {
                    DWORD nc[4], svv[4];
                    nc[0] = *(DWORD *)(cl + 0x40); nc[1] = *(DWORD *)(cl + 0x30); nc[2] = *(DWORD *)(cl + 0x44); nc[3] = *(DWORD *)(cl + 0x38);
                    svv[0] = sv ? *(DWORD *)(sv + 0x40) : 0; svv[1] = sv ? *(DWORD *)(sv + 0x30) : 0;
                    svv[2] = sv ? *(DWORD *)(sv + 0x44) : 0; svv[3] = sv ? *(DWORD *)(sv + 0x38) : 0;
                    if (lastsum) {
                        int d0 = (int)(nc[0] - nc_prev[0]), d1 = (int)(nc[1] - nc_prev[1]);
                        int d2 = (int)(svv[0] - sv_prev[0]), d3 = (int)(svv[1] - sv_prev[1]);
                        char b[200]; unsigned n = put_str(b, 0, "flow 50ms: client sent ");
                        n = put_int(b, n, d0); n = put_str(b, n, " recv "); n = put_int(b, n, d1);
                        n = put_str(b, n, " | server sent "); n = put_int(b, n, d2); n = put_str(b, n, " recv "); n = put_int(b, n, d3);
                        n = put_str(b, n, " | cl relseq "); n = put_int(b, n, (int)nc[2]); n = put_str(b, n, " relack "); n = put_int(b, n, (int)nc[3]);
                        n = put_str(b, n, " | sv relseq "); n = put_int(b, n, (int)svv[2]); n = put_str(b, n, " relack "); n = put_int(b, n, (int)svv[3]);
                        b[n] = 0; sl_mark(b);
                    }
                    for (i = 0; i < 4; i++) { nc_prev[i] = nc[i]; sv_prev[i] = svv[i]; }
                    lastsum = timeGetTime();
                }
            }
            unsigned long long rt = *(volatile unsigned long long *)(w_eng + (REALTIME_VA - ENGINE_PREF));
            if (rt != last_rt) { frames++; last_rt = rt; }
            for (i = 0; i < NV; i++) {
                int v = rd(&vars[i]);
                if (v != last[i]) {
                    char b[160]; unsigned n = put_str(b, 0, "watch: ");
                    n = put_str(b, n, vars[i].name); n = put_str(b, n, " ");
                    n = put_int(b, n, last[i]); n = put_str(b, n, " -> "); n = put_int(b, n, v);
                    n = put_str(b, n, "   (engine frames since map open: "); n = put_int(b, n, (int)frames); n = put_str(b, n, ")");
                    b[n] = 0; sl_mark(b);
                    last[i] = v;
                }
            }
            Sleep(1);
        }
        }
        timeEndPeriod(1);
        {
            char b[80]; unsigned n = put_str(b, 0, "watch end, engine frames: ");
            n = put_int(b, n, (int)frames); b[n] = 0; sl_mark(b);
        }
        seen = w_seq;
    }
}

void watch_start(void)
{
    DWORD tid;
    HANDLE t = CreateThread(0, 0, watcher, 0, 0, &tid);
    if (t) CloseHandle(t);
}
