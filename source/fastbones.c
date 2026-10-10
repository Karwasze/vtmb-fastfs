/*
 * vtmb-fastfs: fast bone linking ("Fast Bone Link = 1").
 *
 * When the engine loads a model that includes other models (shared animation
 * and skeleton files), it links the bones by name: for every bone it walks the
 * included model's bone list with stricmp() until the names match. That is
 * O(bones^2) string compares per link, and a level links hundreds of models:
 * ~10% of a level change.
 *
 * This module replaces that inner search (engine.dll 0x2000d001-0x2000d092)
 * with a hash lookup. Each included model gets its own table of bone names,
 * kept for the session and rebuilt if a different model appears at that address
 * (checked by bone count, bone table offset and the model's checksum). It maps every
 * name (case-insensitive, like stricmp in the "C" locale) to its first bone,
 * so the result is the bone the original loop would have found. If a non-"C"
 * locale were active, the original linear stricmp search is used instead.
 * The effects of a match are the same as the original code's: the bone index
 * is stored and the bone's flags are merged.
 *
 * Applied only if the engine code matches exactly (engine.dll 2004 build).
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
void sl_mark(const char *text);

volatile DWORD fb_patched;      /* 1 = applied, 2 = engine code not recognized */
volatile DWORD fb_lookups, fb_builds, fb_linear;

#define ENGINE_PREF   0x20000000u
#define SITE_VA       0x2000d001u     /* start of the inner search */
#define CONT_VA       0x2000d092u     /* code after the inner search */
#define STRICMP_VA    0x20172c30u     /* engine's _strcmpi */
#define LOCALE_VA     0x2138eea4u     /* CRT locale info; +8 != 0 => non-"C" locale */
#define BONE_SIZE     0xa0

static const BYTE site_code[] = { 0x8b, 0x87, 0xf0, 0x00, 0x00, 0x00, 0x85, 0xc0, 0xc7, 0x44, 0x24, 0x20 };

static BYTE *g_eng;
typedef int (__cdecl *StricmpFn)(const char *, const char *);
static StricmpFn eng_stricmp;
__attribute__((used)) DWORD g_bone_cont;   /* absolute address of CONT_VA, for the stub */

/* one hash table per included model, kept across link passes (a level links the
   same few skeletons thousands of times) */
typedef struct { BYTE *hdr; int n, boneindex; DWORD checksum; short *tab; DWORD mask; } BTable;
#define NBT 256
static BTable btabs[NBT];
static BTable *b_last;
static DWORD b_victim;
static unsigned char lc(unsigned char c) { return (c >= 'A' && c <= 'Z') ? (unsigned char)(c + 32) : c; }
static DWORD hash_ci(const char *s)
{
    DWORD h = 2166136261u;
    while (*s) { h ^= lc((unsigned char)*s++); h *= 16777619u; }
    return h;
}
static int eq_ci(const char *a, const char *b)   /* stricmp(a,b)==0 in the "C" locale */
{
    for (;; a++, b++) {
        unsigned char x = lc((unsigned char)*a), y = lc((unsigned char)*b);
        if (x != y) return 0;
        if (!x) return 1;
    }
}
static const char *bone_name(BYTE *hdr, int j)
{
    BYTE *bone = hdr + *(int *)(hdr + 0xf4) + j * BONE_SIZE;
    return (const char *)(bone + *(int *)bone);
}

/* returns the first bone of hdr whose name equals a (case-insensitive), or -1 */
__attribute__((used)) int __cdecl bone_find(BYTE *hdr, const char *a, int outer)
{
    int n = *(int *)(hdr + 0xf0), j;
    DWORD h, i;
    if (n <= 0) return -1;
    fb_lookups++;
    if (*(int *)(g_eng + (LOCALE_VA - ENGINE_PREF) + 8) != 0 || n >= 32767) {
        fb_linear++;
        for (j = 0; j < n; j++) if (eng_stricmp(a, bone_name(hdr, j)) == 0) return j;
        return -1;
    }
    {
        int bi = *(int *)(hdr + 0xf4);
        DWORD ck = *(DWORD *)(hdr + 8), k, size;
        BTable *t = b_last;
        (void)outer;
        if (!(t && t->hdr == hdr && t->n == n && t->boneindex == bi && t->checksum == ck)) {
            DWORD hh = ((DWORD)hdr >> 4) * 2654435761u >> 24;            /* 8 bits */
            for (k = 0; k < 8; k++) {
                t = &btabs[(hh + k) & (NBT - 1)];
                if (t->hdr == hdr || !t->hdr) break;
            }
            if (k == 8) t = &btabs[(hh + (b_victim++ & 7)) & (NBT - 1)];
            if (!(t->hdr == hdr && t->n == n && t->boneindex == bi && t->checksum == ck)) {
                for (size = 16; size < (DWORD)n * 2; size <<= 1) {}
                if (t->tab) HeapFree(GetProcessHeap(), 0, t->tab);
                t->tab = (short *)HeapAlloc(GetProcessHeap(), 8 /*zeroed*/, size * sizeof(short));
                if (!t->tab) { t->hdr = 0; b_last = 0; fb_linear++;
                    for (j = 0; j < n; j++) if (eng_stricmp(a, bone_name(hdr, j)) == 0) return j;
                    return -1; }
                t->mask = size - 1;
                for (j = 0; j < n; j++) {
                    const char *nm = bone_name(hdr, j);
                    for (i = hash_ci(nm) & t->mask; t->tab[i]; i = (i + 1) & t->mask)
                        if (eq_ci(nm, bone_name(hdr, t->tab[i] - 1))) break;   /* keep the first */
                    if (!t->tab[i]) t->tab[i] = (short)(j + 1);
                }
                t->hdr = hdr; t->n = n; t->boneindex = bi; t->checksum = ck; fb_builds++;
            }
            b_last = t;
        }
        for (i = hash_ci(a) & t->mask; t->tab[i]; i = (i + 1) & t->mask)
            if (eq_ci(a, bone_name(hdr, t->tab[i] - 1))) return t->tab[i] - 1;
        return -1;
    }
}

/*
 * Jumped to from 0x2000d001 with the engine's frame intact:
 *   edi = included model, [esp+40h] = this bone's name, [esp+14h] = this bone's index,
 *   [esp+18h] = this bone, [esp+24h] = where the matching index goes,
 *   [esp+20h] = the search loop's counter (left as the original leaves it).
 * esi, edi, ebp are preserved (bone_find is a normal C function); eax, ecx, edx and
 * ebx are not live at 0x2000d092.
 */
__attribute__((naked)) static void bone_stub(void)
{
    __asm__ volatile(
        "pushl 0x14(%esp)\n\t"          /* outer index */
        "pushl 0x44(%esp)\n\t"          /* name ([esp+40h] before the first push) */
        "pushl %edi\n\t"                /* included model */
        "call _bone_find\n\t"
        "addl $12, %esp\n\t"
        "testl %eax, %eax\n\t"
        "js 1f\n\t"
        "movl %eax, 0x20(%esp)\n\t"
        "movl 0x24(%esp), %ecx\n\t"
        "movw %ax, (%ecx)\n\t"
        "imull $0xa0, %eax, %eax\n\t"
        "addl 0xf4(%edi), %eax\n\t"
        "addl %edi, %eax\n\t"           /* matching bone */
        "movl 0x88(%eax), %edx\n\t"
        "andl $0xfffc, %edx\n\t"
        "movl 0x18(%esp), %eax\n\t"
        "orl %edx, 0x88(%eax)\n\t"
        "jmp *_g_bone_cont\n"
        "1:\n\t"
        "movl 0xf0(%edi), %eax\n\t"     /* no match: counter ends at numbones (0 if none) */
        "testl %eax, %eax\n\t"
        "jg 2f\n\t"
        "xorl %eax, %eax\n"
        "2:\n\t"
        "movl %eax, 0x20(%esp)\n\t"
        "jmp *_g_bone_cont\n\t");
}

static int engine_ok(BYTE *b)
{
    DWORD pe;
    if (!b || *(unsigned short *)b != 0x5A4D) return 0;
    pe = *(DWORD *)(b + 0x3C);
    return *(DWORD *)(b + pe + 8) == 0x41afaf48u && *(DWORD *)(b + pe + 24 + 56) == 0x13bb000u;
}

static DWORD WINAPI fastbones_thread(void *unused)
{
    BYTE *site, *call;
    DWORD i, old;
    (void)unused;
    while (!(g_eng = (BYTE *)GetModuleHandleA("engine.dll"))) Sleep(2);
    if (!engine_ok(g_eng)) goto unknown;
    site = g_eng + (SITE_VA - ENGINE_PREF);
    for (i = 0; i < sizeof site_code; i++) if (site[i] != site_code[i]) goto unknown;
    call = g_eng + 0xd039;                              /* the stricmp call inside the loop */
    if (call[0] != 0xe8 || (DWORD)(call + 5 + *(int *)(call + 1)) != (DWORD)(g_eng + (STRICMP_VA - ENGINE_PREF))) goto unknown;
    eng_stricmp = (StricmpFn)(g_eng + (STRICMP_VA - ENGINE_PREF));
    g_bone_cont = (DWORD)(g_eng + (CONT_VA - ENGINE_PREF));
    if (!VirtualProtect(site, 6, 0x40, &old)) goto unknown;
    /* jmp bone_stub; nop  (replaces "mov eax,[edi+0F0h]") */
    site[5] = 0x90;
    *(volatile int *)(site + 1) = (int)((BYTE *)bone_stub - (site + 5));
    site[0] = 0xe9;
    VirtualProtect(site, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 6);
    fb_patched = 1;
    sl_mark("fast bone link: bone-name hash lookup installed");
    return 0;
unknown:
    fb_patched = 2;
    sl_mark("fast bone link: unexpected engine.dll build, off");
    return 0;
}

void fastbones_start(void)
{
    DWORD tid;
    HANDLE t = CreateThread(0, 0, fastbones_thread, 0, 0, &tid);
    if (t) CloseHandle(t);
}
