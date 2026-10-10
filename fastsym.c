/*
 * vtmb-fastfs: fast symbol table lookups in FileSystem_Stdio.dll ("Fast Symbols = 1").
 *
 * The filesystem turns strings (search-path IDs, path names) into symbols with
 * CUtlSymbolTable::AddString, which searches a red-black tree of all strings
 * seen so far, comparing strings on every step. It does this for every file
 * lookup and costs ~70 ms of each level change.
 *
 * This module puts a hash cache in front of AddString: a string seen before is
 * answered with its symbol directly, after checking that the symbol still
 * stands for exactly that string in that table (so a cleared or rebuilt table
 * can never give a wrong answer - it just goes to the original again). New
 * strings go to the original AddString unchanged.
 *
 * Installed with a 5-byte jump at the start of AddString, only if the code of
 * AddString and the two string-compare functions matches exactly.
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
__declspec(dllimport) void * WINAPI VirtualAlloc(void *, DWORD, DWORD, DWORD);
__declspec(dllimport) BOOL   WINAPI FlushInstructionCache(HANDLE, const void *, DWORD);
__declspec(dllimport) HANDLE WINAPI GetCurrentProcess(void);
__declspec(dllimport) long   WINAPI InterlockedExchange(volatile long *, long);

void sl_mark(const char *text);

volatile DWORD fy_patched, fy_hits, fy_misses;

#define ADD_RVA      0xcf30u   /* CUtlSymbolTable::AddString(int *out, const char *) (thiscall, ret 8) */
#define LESS_CS_RVA  0xcc00u   /* tree compare, case-sensitive */
#define LESS_CI_RVA  0xcc80u   /* tree compare, case-insensitive */

static const BYTE add_code[] = { 0x83, 0xec, 0x08, 0x55, 0x56, 0x8b, 0x74, 0x24, 0x18, 0x8b, 0xe9, 0x85, 0xf6, 0x75, 0x12 };
static const BYTE cs_code[]  = { 0x8b, 0x44, 0x24, 0x04, 0x8b, 0x15 };
static const BYTE ci_code[]  = { 0x8b, 0x44, 0x24, 0x04, 0x56, 0x8b, 0x35 };

typedef int *(__attribute__((thiscall)) *AddFn)(void *table, int *out, const char *s);
static AddFn real_add;                /* trampoline: the 5 moved bytes + jump back */
static DWORD less_cs, less_ci;

#define NC 16384
typedef struct { void *table; DWORD hash; int id; } CEnt;
static CEnt cache[NC];
static volatile long g_lock;

static BYTE fold(BYTE c) { return (c >= 'A' && c <= 'Z') ? (BYTE)(c + 32) : c; }

/* the string a symbol stands for, or 0 if the id is not a valid entry of the table */
static const char *sym_string(const BYTE *t, int id, DWORD *room)
{
    const BYTE *nodes = *(BYTE *const *)(t + 4);
    DWORD nalloc = *(const DWORD *)(t + 8), off, size;
    const char *pool = *(const char *const *)(t + 0x20);
    if (id < 0 || (DWORD)id >= nalloc || !nodes || !pool) return 0;
    off = *(const DWORD *)(nodes + (DWORD)id * 20 + 0x10);
    size = *(const DWORD *)(t + 0x2c);
    if (off >= size) return 0;
    *room = size - off;
    return pool + off;
}

__attribute__((thiscall)) static int *my_add(void *table, int *out, const char *s)
{
    const BYTE *t = (const BYTE *)table;
    DWORD less, h = 2166136261u, i, k, room;
    int ci;
    const BYTE *p;
    if (!s || !t) return real_add(table, out, s);
    less = *(const DWORD *)t;
    if (less != less_cs && less != less_ci) return real_add(table, out, s);
    ci = less == less_ci;
    for (p = (const BYTE *)s; *p; p++) { h ^= ci ? fold(*p) : *p; h *= 16777619u; }
    if (InterlockedExchange(&g_lock, 1)) return real_add(table, out, s);   /* another thread: just go the old way */
    for (i = h & (NC - 1), k = 0; k < 8; k++, i = (i + 1) & (NC - 1)) {
        CEnt *e = &cache[i];
        if (!e->table) break;
        if (e->table == table && e->hash == h) {
            const char *str = sym_string(t, e->id, &room);
            if (str) {
                DWORD j;
                for (j = 0; j < room; j++) {
                    BYTE a = (BYTE)str[j], b = (BYTE)s[j];
                    if (ci ? fold(a) != fold(b) : a != b) break;
                    if (!a) { *out = e->id; fy_hits++; g_lock = 0; return out; }
                }
            }
        }
    }
    g_lock = 0;
    real_add(table, out, s);
    fy_misses++;
    if (*out != -1 && !InterlockedExchange(&g_lock, 1)) {
        CEnt *e = 0;
        for (i = h & (NC - 1), k = 0; k < 8; k++, i = (i + 1) & (NC - 1)) {
            if (!cache[i].table || (cache[i].table == table && cache[i].hash == h)) { e = &cache[i]; break; }
        }
        if (!e) e = &cache[h & (NC - 1)];                  /* all 8 taken: replace the first */
        e->table = 0;
        e->hash = h; e->id = *out; e->table = table;
        g_lock = 0;
    }
    return out;
}

static DWORD WINAPI fastsym_thread(void *unused)
{
    BYTE *fs, *site, *tramp;
    DWORD i, old, tries = 0;
    (void)unused;
    while (!(fs = (BYTE *)GetModuleHandleA("filesystem_stdio.dll"))) { if (tries++ > 60000) return 0; Sleep(1); }
    site = fs + ADD_RVA;
    for (i = 0; i < sizeof add_code; i++) if (site[i] != add_code[i]) goto unknown;
    for (i = 0; i < sizeof cs_code; i++) if (fs[LESS_CS_RVA + i] != cs_code[i]) goto unknown;
    for (i = 0; i < sizeof ci_code; i++) if (fs[LESS_CI_RVA + i] != ci_code[i]) goto unknown;
    less_cs = (DWORD)(fs + LESS_CS_RVA); less_ci = (DWORD)(fs + LESS_CI_RVA);
    tramp = (BYTE *)VirtualAlloc(0, 4096, 0x3000, 0x40);
    if (!tramp) goto unknown;
    for (i = 0; i < 5; i++) tramp[i] = site[i];                  /* sub esp,8; push ebp; push esi */
    tramp[5] = 0xe9; *(int *)(tramp + 6) = (int)((site + 5) - (tramp + 10));
    real_add = (AddFn)tramp;
    /* the filesystem is already in use by the game: swap the 5 bytes in with one
       atomic 8-byte write (the site is 16-byte aligned) */
    if ((DWORD)site & 7) goto unknown;
    if (!VirtualProtect(site, 8, 0x40, &old)) goto unknown;
    {
        volatile unsigned long long *q = (volatile unsigned long long *)site;
        unsigned long long was = *q, now = was;
        BYTE *n = (BYTE *)&now;
        int rel = (int)((BYTE *)my_add - (site + 5));
        n[0] = 0xe9; n[1] = (BYTE)rel; n[2] = (BYTE)(rel >> 8); n[3] = (BYTE)(rel >> 16); n[4] = (BYTE)(rel >> 24);
        if (__sync_val_compare_and_swap(q, was, now) != was) { VirtualProtect(site, 8, old, &old); goto unknown; }
    }
    VirtualProtect(site, 8, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    fy_patched = 1;
    sl_mark("fast symbols: filesystem symbol-table cache installed");
    return 0;
unknown:
    fy_patched = 2;
    sl_mark("fast symbols: unexpected FileSystem_Stdio.dll build, off");
    return 0;
}

void fastsym_start(void)
{
    DWORD tid;
    HANDLE t = CreateThread(0, 0, fastsym_thread, 0, 0, &tid);
    if (t) CloseHandle(t);
}
