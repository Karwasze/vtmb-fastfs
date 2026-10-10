/*
 * vtmb-fastfs: fast model lookup ("Fast Model Lookup = 1").
 *
 * The Unofficial Patch's modelloader-fix plugin (by JillCrungus) replaces the
 * engine's Mod_FindName with a version that supports more models. To find a
 * model it walks all 1024 model slots and, for every used slot, lowercases and
 * normalises the stored name before comparing it - hundreds of thousands of
 * string normalisations per level load (~15% of a level build).
 *
 * This module puts a cache in front of it: a hash table from the normalised
 * name to the slot the plugin returned last time. A cached slot is only used
 * after checking that it still holds that exact (normalised) name; otherwise,
 * and for names the cache hasn't seen, the plugin's own function is called and
 * its answer is cached. Names are unique per slot (the plugin only adds a name
 * it did not find), so the answer is always the one the plugin would give.
 *
 * Installed by redirecting the engine's jump into the plugin (one 4-byte jump
 * target change), and only if the jump and the plugin's code are exactly as
 * expected; without that plugin this module does nothing.
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
__declspec(dllimport) long   WINAPI InterlockedExchange(volatile long *, long);

void sl_mark(const char *text);

volatile DWORD fm_patched;   /* 1 = applied, 2 = plugin/engine code not recognized */
volatile DWORD fm_hits, fm_misses;

#define ENGINE_FINDNAME_RVA 0xb87a0u     /* Mod_FindName in engine.dll (0x200b87a0) */
#define MLF_FIND_RVA        0x1360u      /* the plugin's replacement */
#define SLOT0      0x4
#define SLOTSIZE   0x184
#define NSLOTS     0x400
#define NAMEMAX    0x80

static const BYTE mlf_code[] = { 0x55, 0x8b, 0xec, 0x81, 0xec, 0x08, 0x01, 0x00, 0x00 };

typedef void *(__fastcall *FindFn)(void *self, void *edx, const char *name);
static FindFn mlf_find;

/* identical to the plugin's normaliser: '\' -> '/', A-Z -> a-z, no "//", < 128 chars */
static unsigned norm(const char *s, char *d)
{
    unsigned n = 0;
    char prev = 0;
    for (; *s && n + 1 < NAMEMAX; s++) {
        char c = *s;
        if (c == '\\') c = '/';
        else if (c >= 'A' && c <= 'Z') c += 32;
        if (c == '/' && prev == '/') continue;
        d[n++] = c; prev = c;
    }
    d[n] = 0;
    return n;
}

#define HBITS 12
#define HSIZE (1u << HBITS)
typedef struct { DWORD hash; void *self; int slot; } Ent;   /* slot + 1, 0 = empty */
static Ent tab[HSIZE];
static volatile long busy;

static DWORD fnv(const char *s, unsigned n)
{
    DWORD h = 2166136261u; unsigned i;
    for (i = 0; i < n; i++) { h ^= (BYTE)s[i]; h *= 16777619u; }
    return h ? h : 1;
}

static int same(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

static void *__fastcall fast_find(BYTE *self, void *edx, const char *name)
{
    char key[NAMEMAX], tmp[NAMEMAX];
    unsigned n, i, pos;
    DWORD h;
    BYTE *r;
    (void)edx;
    if (!name || !*name || *name == '*') return mlf_find(self, 0, name);
    if (InterlockedExchange(&busy, 1)) return mlf_find(self, 0, name);   /* reentrant / other thread */
    n = norm(name, key);
    h = fnv(key, n);
    pos = h & (HSIZE - 1);
    for (i = 0; i < 16; i++) {
        Ent *e = &tab[(pos + i) & (HSIZE - 1)];
        if (!e->slot) break;
        if (e->hash == h && e->self == self) {
            BYTE *slot = self + SLOT0 + (e->slot - 1) * SLOTSIZE;
            if (slot[0]) { norm((const char *)slot, tmp); if (same(tmp, key)) { fm_hits++; busy = 0; return slot; } }
        }
    }
    fm_misses++;
    r = (BYTE *)mlf_find(self, 0, name);
    if (r >= self + SLOT0 && r < self + SLOT0 + NSLOTS * SLOTSIZE && (DWORD)(r - (self + SLOT0)) % SLOTSIZE == 0) {
        int idx = (int)((r - (self + SLOT0)) / SLOTSIZE) + 1;
        Ent *free_e = 0;
        for (i = 0; i < 16; i++) {
            Ent *e = &tab[(pos + i) & (HSIZE - 1)];
            if (!e->slot || (e->hash == h && e->self == self)) { free_e = e; break; }
        }
        if (!free_e) free_e = &tab[pos];                  /* full run: overwrite (still verified on use) */
        free_e->hash = h; free_e->self = self; free_e->slot = idx;
    }
    busy = 0;
    return r;
}

static DWORD WINAPI fastmodels_thread(void *unused)
{
    BYTE *eng, *mlf, *jmp;
    DWORD i, old, waited = 0;
    (void)unused;
    while (!(eng = (BYTE *)GetModuleHandleA("engine.dll"))) Sleep(10);
    /* the plugin installs its jump when the engine is loaded; give it up to 60 s */
    for (;;) {
        mlf = (BYTE *)GetModuleHandleA("modelloader-fix.vtm");
        jmp = eng + ENGINE_FINDNAME_RVA;
        if (mlf && jmp[0] == 0xe9 && (DWORD)(jmp + 5 + *(int *)(jmp + 1)) == (DWORD)(mlf + MLF_FIND_RVA)) break;
        if (waited > 60000) { fm_patched = 2; return 0; }   /* plugin absent or disabled: nothing to do */
        Sleep(20); waited += 20;
    }
    for (i = 0; i < sizeof mlf_code; i++)
        if (mlf[MLF_FIND_RVA + i] != mlf_code[i]) { fm_patched = 2; sl_mark("fast model lookup: unexpected modelloader-fix build, off"); return 0; }
    mlf_find = (FindFn)(mlf + MLF_FIND_RVA);
    if (!VirtualProtect(jmp, 5, 0x40, &old)) { fm_patched = 2; return 0; }
    *(volatile int *)(jmp + 1) = (int)((BYTE *)fast_find - (jmp + 5));
    VirtualProtect(jmp, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), jmp, 5);
    fm_patched = 1;
    sl_mark("fast model lookup: cache installed in front of modelloader-fix");
    return 0;
}

void fastmodels_start(void)
{
    DWORD tid;
    HANDLE t = CreateThread(0, 0, fastmodels_thread, 0, 0, &tid);
    if (t) CloseHandle(t);
}
