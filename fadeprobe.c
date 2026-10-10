/*
 * vtmb-fastfs: diagnostic probe for full-screen fades ("Probe Fades = 1", needs Log Console).
 *
 * Logs every call of the engine's IVRenderView::ViewDrawFade (how the client
 * draws a full-screen colour fade): colour and alpha, time since the player got
 * control, and the caller's return address, so the source of a fade can be
 * found. Calls are passed on unchanged.
 */

typedef unsigned int   DWORD;
typedef int            BOOL;
typedef void          *HANDLE;
typedef void          *HMODULE;
typedef unsigned char  BYTE;
typedef unsigned short WORD;
typedef const char    *LPCSTR;
#define WINAPI __stdcall

__declspec(dllimport) HMODULE WINAPI GetModuleHandleA(LPCSTR);
__declspec(dllimport) HANDLE WINAPI CreateThread(void *, DWORD, DWORD (WINAPI *)(void *), void *, DWORD, DWORD *);
__declspec(dllimport) BOOL   WINAPI CloseHandle(HANDLE);
__declspec(dllimport) void   WINAPI Sleep(DWORD);
__declspec(dllimport) BOOL   WINAPI VirtualProtect(void *, DWORD, DWORD, DWORD *);
__declspec(dllimport) DWORD  WINAPI GetTickCount(void);

void sl_mark(const char *text);

#define SLOT_VA   0x20187eacu   /* CVRenderView vtable: ViewDrawFade */
#define FUNC_VA   0x2010f230u
#define CLS_STATE 0x201dbe50u

typedef void (__fastcall *DrawFadeFn)(void *self, void *edx, BYTE *color, void *mat);
static DrawFadeFn real_fade;
static BYTE *g_eng, *g_client;
static volatile DWORD in_game_since, logged;
static int last_a = -1;
static DWORD last_ret;

static unsigned put_s(char *b, unsigned n, const char *s) { while (*s) b[n++] = *s++; return n; }
static unsigned put_i(char *b, unsigned n, int v)
{
    char t[12]; int k = 0; unsigned u = v < 0 ? (unsigned)-v : (unsigned)v;
    if (v < 0) b[n++] = '-';
    do { t[k++] = (char)('0' + u % 10); u /= 10; } while (u);
    while (k) b[n++] = t[--k];
    return n;
}
static unsigned put_x(char *b, unsigned n, DWORD v)
{
    int i; for (i = 28; i >= 0; i -= 4) b[n++] = "0123456789abcdef"[(v >> i) & 15];
    return n;
}

static void __fastcall probe(void *self, void *edx, BYTE *color, void *mat)
{
    DWORD ret = (DWORD)__builtin_return_address(0);
    int a = color ? color[3] : -1;
    (void)edx;
    if (logged < 600 && (a != last_a || ret != last_ret)) {
        char b[220]; unsigned n = put_s(b, 0, "fade probe: rgba ");
        if (color) { n = put_i(b, n, color[0]); b[n++] = ','; n = put_i(b, n, color[1]); b[n++] = ',';
                     n = put_i(b, n, color[2]); b[n++] = ','; n = put_i(b, n, color[3]); }
        n = put_s(b, n, " | ms after control ");
        n = put_i(b, n, in_game_since ? (int)(GetTickCount() - in_game_since) : -1);
        n = put_s(b, n, " | caller ");
        if (!g_client) g_client = (BYTE *)GetModuleHandleA("client.dll");
        if (g_client && ret > (DWORD)g_client && ret < (DWORD)g_client + 0x800000) { n = put_s(b, n, "client.dll+"); n = put_x(b, n, ret - (DWORD)g_client); }
        else { n = put_s(b, n, "abs "); n = put_x(b, n, ret); }
        b[n] = 0; sl_mark(b);
        last_a = a; last_ret = ret; logged++;
    }
    real_fade(self, 0, color, mat);
}

static DWORD WINAPI probe_thread(void *unused)
{
    DWORD *slot, old;
    int last = -1;
    (void)unused;
    while (!(g_eng = (BYTE *)GetModuleHandleA("engine.dll"))) Sleep(10);
    slot = (DWORD *)(g_eng + (SLOT_VA - 0x20000000u));
    if (*slot != (DWORD)(g_eng + (FUNC_VA - 0x20000000u))) { sl_mark("fade probe: unexpected engine build, off"); return 0; }
    real_fade = (DrawFadeFn)*slot;
    if (!VirtualProtect(slot, 4, 0x04, &old)) return 0;
    *(volatile DWORD *)slot = (DWORD)probe;
    VirtualProtect(slot, 4, old, &old);
    sl_mark("fade probe: installed");
    for (;;) {
        int st = *(volatile int *)(g_eng + (CLS_STATE - 0x20000000u));
        if (st != last) { in_game_since = st == 4 ? (GetTickCount() | 1) : 0; last = st; }
        Sleep(2);
    }
}

void fadeprobe_start(void)
{
    DWORD tid;
    HANDLE t = CreateThread(0, 0, probe_thread, 0, 0, &tid);
    if (t) CloseHandle(t);
}
