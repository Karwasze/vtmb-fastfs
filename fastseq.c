/*
 * vtmb-fastfs: fast animation lookups ("Fast Anim Lookup = 1").
 *
 * The game DLL looks up a model's animation sequences by name (sequence label)
 * and by activity name. Both functions (vampire.dll 0x10428360 and 0x104284c0,
 * the game's copies of LookupActivity/LookupSequence) walk every sequence of
 * the model comparing names with _strcmpi, then repeat that for every included
 * animation model. Spawning a level's characters does this tens of thousands of
 * times: ~90 ms of every level build.
 *
 * This module replaces the walk over one model's own sequences with a hash
 * lookup. A table per model maps each name (case-insensitive, as _strcmpi in
 * the "C" locale) to the first sequence that has it, so the answer is the one
 * the original loop gives; the search of included models is left to the game.
 * Tables are keyed by the model's address and checked against its sequence
 * count, sequence-table offset and checksum before use, so a different model
 * loaded at the same address gets a new table. If a non-"C" locale were active, the
 * original comparison is used.
 *
 * Applied only if the game code at both places matches exactly.
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
__declspec(dllimport) HANDLE WINAPI GetProcessHeap(void);
__declspec(dllimport) void * WINAPI HeapAlloc(HANDLE, DWORD, DWORD);
__declspec(dllimport) BOOL   WINAPI HeapFree(HANDLE, DWORD, void *);
__declspec(dllimport) long   WINAPI InterlockedExchange(volatile long *, long);

void sl_mark(const char *text);

volatile DWORD fs_patched, fs_lookups, fs_builds, fs_linear;

#define ACT_SITE_RVA   0x428380u   /* LookupActivity: after IndexActivities(), before its loop */
#define ACT_FOUND_RVA  0x428412u   /* edi = index, ebp = sequence table -> returns activity */
#define ACT_NEXT_RVA   0x4283c1u   /* search included models */
#define SEQ_SITE_RVA   0x4284d0u   /* LookupSequence: before its loop */
#define SEQ_FOUND_RVA  0x428569u   /* edi = index -> returns it */
#define SEQ_NEXT_RVA   0x428519u   /* search included models */
#define STRCMPI_RVA    0x43e780u
#define LOCALE_RVA     0x9f052cu   /* CRT locale info; +8 != 0 => non-"C" locale */
#define SEQ_SIZE       0x2fc

static const BYTE act_code[] = { 0x8b, 0xae, 0x14, 0x01, 0x00, 0x00, 0x8b, 0x86, 0x10, 0x01, 0x00, 0x00 };
static const BYTE seq_code[] = { 0x8b, 0x86, 0x10, 0x01, 0x00, 0x00, 0x33, 0xff, 0x85, 0xc0, 0x7e, 0x3d };

static BYTE *g_vamp;
typedef int (__cdecl *StricmpFn)(const char *, const char *);
static StricmpFn game_stricmp;
__attribute__((used)) DWORD g_act_found, g_act_next, g_seq_found, g_seq_next;

static unsigned char lc(unsigned char c) { return (c >= 'A' && c <= 'Z') ? (unsigned char)(c + 32) : c; }
static DWORD hash_ci(const char *s)
{
    DWORD h = 2166136261u;
    while (*s) { h ^= lc((unsigned char)*s++); h *= 16777619u; }
    return h;
}
static int eq_ci(const char *a, const char *b)
{
    for (;; a++, b++) {
        unsigned char x = lc((unsigned char)*a), y = lc((unsigned char)*b);
        if (x != y) return 0;
        if (!x) return 1;
    }
}
static const char *seq_name(const BYTE *hdr, int j, int off)
{
    const BYTE *d = hdr + *(const int *)(hdr + 0x114) + j * SEQ_SIZE;
    return (const char *)(d + *(const int *)(d + off));
}

typedef struct {
    const BYTE *hdr; int off, n, seqindex; DWORD namehash;
    int *tab; DWORD mask;
} Table;
#define NTABLES 512
static Table tables[NTABLES];
static DWORD next_victim;
static volatile long busy;

static Table *g_last;                                     /* table of the previous lookup */

static Table *table_for(const BYTE *hdr, int off, int n)
{
    DWORD i, h, nh, size, ck = *(const DWORD *)(hdr + 8);   /* studiohdr checksum */
    int si = *(const int *)(hdr + 0x114), j;
    Table *t = g_last;
    /* identity: address, name offset, sequence count/table and the model's checksum */
    if (t && t->hdr == hdr && t->off == off && t->n == n && t->seqindex == si && t->namehash == ck) return t;
    h = ((DWORD)hdr >> 4) ^ (DWORD)off;
    for (i = 0; i < 8; i++) {
        t = &tables[(h + i) & (NTABLES - 1)];
        if (t->hdr == hdr && t->off == off) {
            if (t->n == n && t->seqindex == si && t->namehash == ck) return g_last = t;
            break;                                         /* stale: rebuild in place */
        }
        if (!t->hdr) break;
    }
    if (i == 8) t = &tables[(h + (next_victim++ & 7)) & (NTABLES - 1)];
    if (t == g_last) g_last = 0;
    if (t->tab) { HeapFree(GetProcessHeap(), 0, t->tab); t->tab = 0; }
    for (size = 16; size < (DWORD)n * 2; size <<= 1) {}
    t->tab = (int *)HeapAlloc(GetProcessHeap(), 8 /*HEAP_ZERO_MEMORY*/, size * sizeof(int));
    if (!t->tab) { t->hdr = 0; return 0; }
    (void)nh;
    t->hdr = hdr; t->off = off; t->n = n; t->seqindex = si; t->namehash = ck; t->mask = size - 1;
    for (j = 0; j < n; j++) {
        const char *nm = seq_name(hdr, j, off);
        for (i = hash_ci(nm) & t->mask; t->tab[i]; i = (i + 1) & t->mask)
            if (eq_ci(nm, seq_name(hdr, t->tab[i] - 1, off))) break;      /* keep the first */
        if (!t->tab[i]) t->tab[i] = j + 1;
    }
    fs_builds++;
    return g_last = t;
}

/* first sequence of hdr (own sequences only) whose name at `off` equals name, or -1 */
__attribute__((used)) int __cdecl seq_find(const BYTE *hdr, int off, const char *name)
{
    int n = *(const int *)(hdr + 0x110), j;
    Table *t;
    DWORD i;
    if (n <= 0 || !name) return -1;
    fs_lookups++;
    if (*(const int *)(g_vamp + LOCALE_RVA + 8) != 0 || InterlockedExchange(&busy, 1)) {
        fs_linear++;
        for (j = 0; j < n; j++) if (game_stricmp(seq_name(hdr, j, off), name) == 0) return j;
        return -1;
    }
    t = table_for(hdr, off, n);
    if (!t) {
        busy = 0;
        for (j = 0; j < n; j++) if (game_stricmp(seq_name(hdr, j, off), name) == 0) return j;
        return -1;
    }
    for (i = hash_ci(name) & t->mask; t->tab[i]; i = (i + 1) & t->mask)
        if (eq_ci(name, seq_name(hdr, t->tab[i] - 1, off))) { j = t->tab[i] - 1; busy = 0; return j; }
    busy = 0;
    return -1;
}

/*
 * Activity lookup, entered at 0x10428380 (in place of "mov ebp,[esi+114h]"), right
 * after "push esi; call IndexActivities" whose argument the original pops later.
 * esi = model, name at [esp+18h] once that argument is popped.
 */
__attribute__((naked)) static void act_stub(void)
{
    __asm__ volatile(
        "addl $4, %esp\n\t"
        "pushl 0x18(%esp)\n\t"
        "pushl $4\n\t"
        "pushl %esi\n\t"
        "call _seq_find\n\t"
        "addl $12, %esp\n\t"
        "testl %eax, %eax\n\t"
        "js 1f\n\t"
        "movl %eax, %edi\n\t"
        "movl 0x114(%esi), %ebp\n\t"
        "addl %esi, %ebp\n\t"
        "jmp *_g_act_found\n"
        "1:\n\t"
        "jmp *_g_act_next\n\t");
}

/* Sequence lookup, entered at 0x104284d0. esi = model, name at [esp+18h]. */
__attribute__((naked)) static void seq_stub(void)
{
    __asm__ volatile(
        "pushl 0x18(%esp)\n\t"
        "pushl $0\n\t"
        "pushl %esi\n\t"
        "call _seq_find\n\t"
        "addl $12, %esp\n\t"
        "testl %eax, %eax\n\t"
        "js 1f\n\t"
        "movl %eax, %edi\n\t"
        "jmp *_g_seq_found\n"
        "1:\n\t"
        "jmp *_g_seq_next\n\t");
}

static int patch_jmp(BYTE *site, void *to)
{
    DWORD old;
    if (!VirtualProtect(site, 6, 0x40, &old)) return 0;
    site[5] = 0x90;
    *(volatile int *)(site + 1) = (int)((BYTE *)to - (site + 5));
    site[0] = 0xe9;
    VirtualProtect(site, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 6);
    return 1;
}

static DWORD WINAPI fastseq_thread(void *unused)
{
    BYTE *m;
    DWORD i, tries = 0;
    (void)unused;
    while (!(m = (BYTE *)GetModuleHandleA("vampire.dll"))) Sleep(tries++ < 4000 ? 2 : 100);
    for (i = 0; i < sizeof act_code; i++) if (m[ACT_SITE_RVA + i] != act_code[i]) goto unknown;
    for (i = 0; i < sizeof seq_code; i++) if (m[SEQ_SITE_RVA + i] != seq_code[i]) goto unknown;
    /* the _strcmpi calls inside both loops */
    if (m[0x4283a4] != 0xe8 || (DWORD)(m + 0x4283a9 + *(int *)(m + 0x4283a5)) != (DWORD)(m + STRCMPI_RVA)) goto unknown;
    if (m[0x428502] != 0xe8 || (DWORD)(m + 0x428507 + *(int *)(m + 0x428503)) != (DWORD)(m + STRCMPI_RVA)) goto unknown;
    g_vamp = m;
    game_stricmp = (StricmpFn)(m + STRCMPI_RVA);
    g_act_found = (DWORD)(m + ACT_FOUND_RVA); g_act_next = (DWORD)(m + ACT_NEXT_RVA);
    g_seq_found = (DWORD)(m + SEQ_FOUND_RVA); g_seq_next = (DWORD)(m + SEQ_NEXT_RVA);
    if (!patch_jmp(m + ACT_SITE_RVA, (void *)act_stub) || !patch_jmp(m + SEQ_SITE_RVA, (void *)seq_stub)) goto unknown;
    fs_patched = 1;
    sl_mark("fast anim lookup: sequence/activity hash lookups installed");
    return 0;
unknown:
    fs_patched = 2;
    sl_mark("fast anim lookup: unexpected vampire.dll build, off");
    return 0;
}

void fastseq_start(void)
{
    DWORD tid;
    HANDLE t = CreateThread(0, 0, fastseq_thread, 0, 0, &tid);
    if (t) CloseHandle(t);
}
