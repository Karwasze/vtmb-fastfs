/*
 * vtmb-fastfs: fast number parsing in the game DLL ("Fast Parse = 1").
 *
 * vampire.dll is statically linked with the old MSVC runtime, whose atof()
 * calls strlen() on its argument before parsing it. The game parses entity
 * and save data with atof() pointing into one large text buffer, so every
 * number parsed scans the whole rest of the buffer: quadratic work that took
 * about a sixth of each level build (~330 ms of ~2 s).
 *
 * The length is only used as an upper bound for the number parser (_fltin),
 * which stops at the first character that cannot be part of a number anyway.
 * This module redirects that one strlen call inside atof to num_len(), which
 * returns the length of the run of characters that can occur in a number.
 * The parser therefore sees exactly the same number and returns bit-identical
 * results, without the scan.
 *
 * The patch is a single call-target change and is only applied when the
 * surrounding code matches the expected bytes exactly.
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
__declspec(dllimport) BOOL   WINAPI VirtualProtect(void *, DWORD, DWORD, DWORD *);
__declspec(dllimport) BOOL   WINAPI FlushInstructionCache(HANDLE, const void *, DWORD);
__declspec(dllimport) HANDLE WINAPI GetCurrentProcess(void);

void sl_mark(const char *text);

volatile DWORD fp_patched;    /* 1 = applied, 2 = code did not match */

/* atof's helper call: push 0; push 0; push esi; call strlen; pop ecx; push eax; push esi; call _fltin */
#define ATOF_SITE_RVA   0x431944u
#define STRLEN_RVA      0x434370u
static const BYTE expect[] = { 0x6a, 0x00, 0x6a, 0x00, 0x56, 0xe8, 0x22, 0x2a, 0x00, 0x00, 0x59, 0x50, 0x56, 0xe8 };

static unsigned __cdecl num_len(const char *s)
{
    unsigned n = 0;
    for (;;) {
        char c = s[n];
        if ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.' ||
            c == 'e' || c == 'E' || c == 'd' || c == 'D') n++;
        else return n;
    }
}

static DWORD WINAPI fastparse_thread(void *unused)
{
    BYTE *m, *site;
    DWORD old, i, tries = 0;
    (void)unused;
    /* vampire.dll is loaded when the game starts its first level; poll for it */
    while (!(m = (BYTE *)GetModuleHandleA("vampire.dll"))) {
        Sleep(tries++ < 2000 ? 5 : 100);
    }
    site = m + ATOF_SITE_RVA;
    for (i = 0; i < sizeof expect; i++)
        if (site[i] != expect[i]) { fp_patched = 2; sl_mark("fast parse: unexpected vampire.dll build, off"); return 0; }
    if (!VirtualProtect(site + 6, 4, 0x40 /*PAGE_EXECUTE_READWRITE*/, &old)) { fp_patched = 2; return 0; }
    /* one aligned-within-cache-line 32-bit store: the call target switches atomically */
    *(volatile DWORD *)(site + 6) = (DWORD)((BYTE *)num_len - (site + 10));
    VirtualProtect(site + 6, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 16);
    fp_patched = 1;
    sl_mark("fast parse: atof length scan patched");
    return 0;
}

void fastparse_start(void)
{
    DWORD tid;
    HANDLE t = CreateThread(0, 0, fastparse_thread, 0, 0, &tid);
    if (t) CloseHandle(t);
}
