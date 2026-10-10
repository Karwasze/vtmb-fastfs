/*
 * vtmb-fastfs console logger (diagnostic, off by default; "Log Console = 1").
 *
 * Installs a tier0 spew handler that records every engine/game message with
 * a millisecond timestamp (and passes it on to the original handler, so the
 * in-game console is unaffected), and enables developer-level messages.
 * Messages are buffered in memory and written by the plugin's worker thread
 * to <game folder>\vtmb-console.log.
 */

typedef unsigned int   DWORD;
typedef int            BOOL;
typedef void          *HANDLE;
typedef void          *HMODULE;
typedef unsigned char  BYTE;
typedef const char    *LPCSTR;
#define WINAPI __stdcall
#define INVALID_HANDLE_VALUE ((HANDLE)(long)-1)
typedef struct { BYTE opaque[24]; } CRITICAL_SECTION;

__declspec(dllimport) HMODULE WINAPI GetModuleHandleA(LPCSTR);
__declspec(dllimport) void * WINAPI GetProcAddress(HMODULE, LPCSTR);
__declspec(dllimport) HANDLE WINAPI CreateFileA(LPCSTR, DWORD, DWORD, void *, DWORD, DWORD, HANDLE);
__declspec(dllimport) BOOL   WINAPI WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
__declspec(dllimport) void * WINAPI VirtualAlloc(void *, DWORD, DWORD, DWORD);
__declspec(dllimport) void   WINAPI InitializeCriticalSection(CRITICAL_SECTION *);
__declspec(dllimport) void   WINAPI EnterCriticalSection(CRITICAL_SECTION *);
__declspec(dllimport) void   WINAPI LeaveCriticalSection(CRITICAL_SECTION *);
__declspec(dllimport) DWORD  WINAPI timeGetTime(void);
__declspec(dllimport) DWORD  WINAPI GetCurrentThreadId(void);

typedef int  (__cdecl *SpewFn)(int type, const char *msg);
typedef void (__cdecl *PFN_SpewOutputFunc)(SpewFn);
typedef SpewFn (__cdecl *PFN_GetSpewOutputFunc)(void);
typedef void (__cdecl *PFN_SpewActivate)(const char *group, int level);

#define BUFSZ (8u << 20)
static char  *sl_buf;
static DWORD  sl_len, sl_flushed;
static HANDLE sl_file = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION sl_cs;
static SpewFn sl_prev;
static PFN_SpewOutputFunc    p_SpewOutputFunc;
static PFN_GetSpewOutputFunc p_GetSpewOutputFunc;
static PFN_SpewActivate      p_SpewActivate;
static DWORD  sl_t0, sl_main_tid;
static int    sl_on;

static void put(const char *s, DWORD n)
{
    DWORD i;
    if (sl_len + n > BUFSZ) n = BUFSZ - sl_len;
    for (i = 0; i < n; i++) sl_buf[sl_len + i] = s[i];
    sl_len += n;
}

static void put_stamp(void)
{
    char tmp[24];
    DWORD t = timeGetTime() - sl_t0, n = 0, k;
    char num[12];
    int j = 0;
    tmp[n++] = '[';
    do { num[j++] = (char)('0' + t % 10); t /= 10; } while (t);
    for (k = j; k < 7; k++) tmp[n++] = ' ';
    while (j) tmp[n++] = num[--j];
    tmp[n++] = ']'; tmp[n++] = ' ';
    if (GetCurrentThreadId() != sl_main_tid) { tmp[n++] = '{'; tmp[n++] = 'T'; tmp[n++] = '}'; tmp[n++] = ' '; }
    put(tmp, n);
}

static int at_line_start = 1;

static void append(const char *msg)
{
    const char *p = msg;
    while (*p) {
        const char *q = p;
        if (at_line_start) { put_stamp(); at_line_start = 0; }
        while (*q && *q != '\n') q++;
        put(p, (DWORD)(q - p));
        if (*q == '\n') { put("\r\n", 2); at_line_start = 1; q++; }
        p = q;
    }
}

static int __cdecl sl_spew(int type, const char *msg)
{
    if (msg && sl_buf) {
        EnterCriticalSection(&sl_cs);
        append(msg);
        LeaveCriticalSection(&sl_cs);
    }
    return sl_prev ? sl_prev(type, msg) : 1 /* SPEW_CONTINUE */;
}

/* A marker line from the plugin itself. */
void sl_mark(const char *text)
{
    if (!sl_on) return;
    EnterCriticalSection(&sl_cs);
    if (!at_line_start) { put("\r\n", 2); at_line_start = 1; }
    append("### ");
    append(text);
    append("\n");
    LeaveCriticalSection(&sl_cs);
}

/* Called from the worker thread every couple of seconds. */
void sl_tick(void)
{
    SpewFn cur;
    DWORD w;
    if (!sl_on) return;
    if (!p_SpewOutputFunc) {
        HMODULE t0 = GetModuleHandleA("tier0.dll");
        if (!t0) return;
        p_SpewOutputFunc = (PFN_SpewOutputFunc)GetProcAddress(t0, "SpewOutputFunc");
        p_GetSpewOutputFunc = (PFN_GetSpewOutputFunc)GetProcAddress(t0, "GetSpewOutputFunc");
        p_SpewActivate = (PFN_SpewActivate)GetProcAddress(t0, "SpewActivate");
        if (!p_SpewOutputFunc || !p_GetSpewOutputFunc) { p_SpewOutputFunc = 0; return; }
    }
    /* (Re)install on top of whatever handler the engine set last. */
    cur = p_GetSpewOutputFunc();
    if (cur != sl_spew) {
        sl_prev = cur;
        p_SpewOutputFunc(sl_spew);
    }
    if (p_SpewActivate) p_SpewActivate("developer", 2);
    EnterCriticalSection(&sl_cs);
    if (sl_len > sl_flushed && sl_file != INVALID_HANDLE_VALUE) {
        WriteFile(sl_file, sl_buf + sl_flushed, sl_len - sl_flushed, &w, 0);
        sl_flushed = sl_len;
    }
    LeaveCriticalSection(&sl_cs);
}

void sl_start(const char *exedir, DWORD main_tid)
{
    char path[540];
    unsigned n = 0;
    while (exedir[n] && n < 500) { path[n] = exedir[n]; n++; }
    {
        const char *nm = "vtmb-console.log";
        unsigned k = 0;
        while (nm[k]) path[n++] = nm[k++];
        path[n] = 0;
    }
    sl_buf = (char *)VirtualAlloc(0, BUFSZ, 0x3000, 0x04);
    if (!sl_buf) return;
    InitializeCriticalSection(&sl_cs);
    sl_t0 = timeGetTime();
    sl_main_tid = main_tid;
    sl_file = CreateFileA(path, 0x40000000UL, 1, 0, 2 /*CREATE_ALWAYS*/, 0x80, 0);
    sl_on = 1;
    sl_mark("vtmb-fastfs console log started");
}
