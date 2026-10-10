/*
 * vtmb-fastfs: fast console ("Fast Console = 1").
 *
 * Every line printed to the in-game console (also while the console is
 * closed) makes the console's text control update its scroll bar. That code
 * calls a helper once per line of the console history, and the helper scans
 * the whole line-break table each time - quadratic in the console length
 * per printed line. During a level change the engine and game print a few
 * hundred lines, so the cost grows with everything printed before; we
 * measured level builds of 4.2-5.7 s instead of 1.8 s from this alone.
 *
 * The helper counts the line breaks at or before the current position. The
 * line-break table is ascending, so this module answers the same question by
 * binary search. Whenever the table changes, it is checked once that it is
 * really sorted; if not, the original linear count is used, so the result is
 * always identical. The 10 calls to the helper in gameui.dll are redirected
 * (one 4-byte call-target change each), only if all of them and the helper
 * itself match the expected code exactly.
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

volatile DWORD fu_patched;     /* 1 = applied, 2 = gameui.dll build not recognized */

#define HELPER_RVA 0x29250u
static const DWORD call_sites[] = { 0x284cf, 0x284da, 0x284e6, 0x284f1, 0x284fc, 0x28508,
                                    0x292a7, 0x292dd, 0x29337, 0x2a799 };
#define NSITES (sizeof call_sites / sizeof call_sites[0])
/* push esi; mov esi,[ecx+64h]; xor eax,eax; push edi; test esi,esi; jle; mov edi,[ecx+70h] */
static const BYTE helper_code[] = { 0x56, 0x8b, 0x71, 0x64, 0x33, 0xc0, 0x57, 0x85, 0xf6, 0x7e, 0x1f,
                                    0x8b, 0x79, 0x70 };

static const int *v_arr;
static int v_n, v_sorted;

/* thiscall(this) emulated as fastcall(ecx, edx): no stack arguments */
static int __fastcall lines_upto(BYTE *t, void *edx)
{
    int n = *(int *)(t + 0x64), pos = *(int *)(t + 0x70), i = 0;
    const int *arr = *(const int **)(t + 0x6c);
    (void)edx;
    if (n > 0) {
        if (arr != v_arr || n != v_n) {
            int k;
            v_sorted = 1;
            for (k = 1; k < n; k++) if (arr[k] < arr[k - 1]) { v_sorted = 0; break; }
            v_arr = arr; v_n = n;
        }
        if (v_sorted) {                       /* first index with arr[i] > pos */
            int lo = 0, hi = n;
            while (lo < hi) { int mid = (lo + hi) >> 1; if (arr[mid] <= pos) lo = mid + 1; else hi = mid; }
            i = lo;
        } else {
            while (i < n && pos >= arr[i]) i++;   /* the original loop */
        }
    }
    if (t[0x75] && *(int *)(t + 0x70) != *(int *)(t + 0x58)) i--;
    return i;
}

static DWORD WINAPI fastui_thread(void *unused)
{
    BYTE *m;
    DWORD i, old, tries = 0;
    (void)unused;
    while (!(m = (BYTE *)GetModuleHandleA("gameui.dll"))) Sleep(tries++ < 2000 ? 5 : 100);
    for (i = 0; i < sizeof helper_code; i++)
        if (m[HELPER_RVA + i] != helper_code[i]) goto unknown;
    for (i = 0; i < NSITES; i++) {
        BYTE *c = m + call_sites[i];
        if (c[0] != 0xe8 || (DWORD)(c + 5 + *(int *)(c + 1)) != (DWORD)(m + HELPER_RVA)) goto unknown;
    }
    if (!VirtualProtect(m + 0x28000, 0x3000, 0x40, &old)) goto unknown;
    for (i = 0; i < NSITES; i++) {
        BYTE *c = m + call_sites[i];
        *(volatile int *)(c + 1) = (int)((BYTE *)lines_upto - (c + 5));
    }
    VirtualProtect(m + 0x28000, 0x3000, old, &old);
    FlushInstructionCache(GetCurrentProcess(), m + 0x28000, 0x3000);
    fu_patched = 1;
    sl_mark("fast console: console scroll-bar line count patched");
    return 0;
unknown:
    fu_patched = 2;
    sl_mark("fast console: unexpected gameui.dll build, off");
    return 0;
}

void fastui_start(void)
{
    DWORD tid;
    HANDLE t = CreateThread(0, 0, fastui_thread, 0, 0, &tid);
    if (t) CloseHandle(t);
}
