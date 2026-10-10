/*
 * vtmb-fastfs: fast texture decompression ("Fast Inflate = 1").
 *
 * Bloodlines stores textures zlib-compressed (.ttz). materialsystem.dll
 * decompresses them with zlib1.dll (zlib 1.2.1 from 2004), which was the
 * largest single cost of a level change (~460 ms of ~2 s).
 *
 * materialsystem hands inflate() the complete compressed texture and a buffer
 * for the complete result. This module replaces materialsystem's import of
 * inflate() with a front end that, for exactly that case - a fresh stream,
 * whole input, enough output space - decompresses with libdeflate (Eric
 * Biggers, MIT licence, built into this plugin), which is several times
 * faster. libdeflate verifies the zlib checksum, and the stream fields are
 * updated as zlib would leave them after finishing. Any other call, and any
 * stream libdeflate does not accept in one piece, goes to the original zlib
 * unchanged, so behaviour on unusual data stays zlib's.
 */

#include "libdeflate/libdeflate.h"

typedef unsigned int   DWORD;
typedef int            BOOL;
typedef void          *HANDLE;
typedef void          *HMODULE;
typedef unsigned char  BYTE;
typedef const char    *LPCSTR;
#define WINAPI __stdcall

__declspec(dllimport) HMODULE WINAPI GetModuleHandleA(LPCSTR);
__declspec(dllimport) void * WINAPI GetProcAddress(HMODULE, LPCSTR);
__declspec(dllimport) HANDLE WINAPI CreateThread(void *, DWORD, DWORD (WINAPI *)(void *), void *, DWORD, DWORD *);
__declspec(dllimport) BOOL   WINAPI CloseHandle(HANDLE);
__declspec(dllimport) void   WINAPI Sleep(DWORD);
__declspec(dllimport) BOOL   WINAPI VirtualProtect(void *, DWORD, DWORD, DWORD *);
__declspec(dllimport) HANDLE WINAPI GetProcessHeap(void);
__declspec(dllimport) void * WINAPI HeapAlloc(HANDLE, DWORD, DWORD);
__declspec(dllimport) BOOL   WINAPI HeapFree(HANDLE, DWORD, void *);
__declspec(dllimport) long   WINAPI InterlockedExchange(volatile long *, long);

void sl_mark(const char *text);
DWORD tp_key(const BYTE *p, DWORD n);
int tp_consume(const BYTE *in, DWORD alen, DWORD key, BYTE *out, DWORD avail_out, DWORD *in_used, DWORD *out_used);
void tp_record(const BYTE *in, DWORD alen, DWORD olen, DWORD key);
extern volatile DWORD tp_on;

volatile DWORD fi_patched;            /* 1 = applied, 2 = not applied */
volatile DWORD fi_fast, fi_passed;    /* calls handled by libdeflate / passed to zlib */
volatile DWORD fi_dups, fi_kb_in, fi_kb_out, fi_dup_kb_out;   /* statistics */

#define DSIZE 8192
static DWORD seen_key[DSIZE];
static DWORD sample_key(const BYTE *p, DWORD n)
{
    DWORD h = 2166136261u ^ n, i, step = n / 61 + 1;
    for (i = 0; i < n; i += step) { h ^= p[i]; h *= 16777619u; }
    for (i = n > 32 ? n - 32 : 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h ? h : 1;
}

/* zlib 1.2.x z_stream (32-bit) */
typedef struct {
    const BYTE *next_in; DWORD avail_in; DWORD total_in;
    BYTE *next_out; DWORD avail_out; DWORD total_out;
    char *msg; void *state; void *zalloc; void *zfree; void *opaque;
    int data_type; DWORD adler; DWORD reserved;
} ZStream;

#define Z_STREAM_END 1
#define MS_INFLATE_IAT_RVA 0x49138u     /* materialsystem.dll import slot of zlib1!inflate */

typedef int (__cdecl *InflateFn)(ZStream *, int);
static InflateFn real_inflate;
static struct libdeflate_decompressor *g_dec;
static volatile long g_busy;

static void *__cdecl my_malloc(size_t n) { return HeapAlloc(GetProcessHeap(), 0, (DWORD)n); }
static void __cdecl my_free(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }

static int __cdecl fast_inflate(ZStream *s, int flush)
{
    size_t in_used = 0, out_used = 0;
    enum libdeflate_result r;
    /* only a fresh stream with input to decompress; everything else -> zlib */
    if (!s || s->total_in || s->total_out || !s->next_in || s->avail_in < 6 || !s->next_out ||
        (flush != 0 && flush != 2 && flush != 4) || InterlockedExchange(&g_busy, 1)) {
        fi_passed++;
        return real_inflate(s, flush);
    }
    {
        DWORD key = tp_on ? tp_key(s->next_in, s->avail_in) : 0, iu = 0, ou = 0;
        if (tp_on && tp_consume(s->next_in, s->avail_in, key, s->next_out, s->avail_out, &iu, &ou)) {
            in_used = iu; out_used = ou; r = LIBDEFLATE_SUCCESS;
        } else {
            r = libdeflate_zlib_decompress_ex(g_dec, s->next_in, s->avail_in, s->next_out, s->avail_out,
                                              &in_used, &out_used);
        }
        g_busy = 0;
        if (r != LIBDEFLATE_SUCCESS) { fi_passed++; return real_inflate(s, flush); }  /* untouched stream */
        if (tp_on) tp_record(s->next_in, s->avail_in, (DWORD)out_used, key);
    }
    {
        const BYTE *t = s->next_in + in_used - 4;   /* zlib trailer: Adler-32 of the output (verified) */
        s->adler = ((DWORD)t[0] << 24) | ((DWORD)t[1] << 16) | ((DWORD)t[2] << 8) | t[3];
    }
    s->next_in += in_used;  s->avail_in -= (DWORD)in_used;  s->total_in += (DWORD)in_used;
    s->next_out += out_used; s->avail_out -= (DWORD)out_used; s->total_out += (DWORD)out_used;
    s->msg = 0;
    fi_fast++;
    fi_kb_in += (DWORD)(in_used >> 10); fi_kb_out += (DWORD)(out_used >> 10);
    {
        DWORD k = sample_key(s->next_in - in_used, (DWORD)in_used), i = k & (DSIZE - 1), probes;
        for (probes = 0; probes < 32; probes++, i = (i + 1) & (DSIZE - 1)) {
            if (seen_key[i] == k) { fi_dups++; fi_dup_kb_out += (DWORD)(out_used >> 10); break; }
            if (!seen_key[i]) { seen_key[i] = k; break; }
        }
    }
    return Z_STREAM_END;
}

static DWORD WINAPI fastinflate_thread(void *unused)
{
    BYTE *ms;
    HMODULE z;
    DWORD *slot, old, tries = 0;
    (void)unused;
    while (!(ms = (BYTE *)GetModuleHandleA("materialsystem.dll"))) Sleep(tries++ < 4000 ? 2 : 100);
    z = GetModuleHandleA("zlib1.dll");
    real_inflate = z ? (InflateFn)GetProcAddress(z, "inflate") : 0;
    slot = (DWORD *)(ms + MS_INFLATE_IAT_RVA);
    /* the import may still be unbound for a moment after the module appears */
    for (tries = 0; real_inflate && *slot != (DWORD)real_inflate && tries < 500; tries++) Sleep(2);
    if (!real_inflate || *slot != (DWORD)real_inflate) { fi_patched = 2; sl_mark("fast inflate: materialsystem.dll not recognized, off"); return 0; }
    libdeflate_set_memory_allocator(my_malloc, my_free);
    g_dec = libdeflate_alloc_decompressor();
    if (!g_dec) { fi_patched = 2; return 0; }
    if (!VirtualProtect(slot, 4, 0x04, &old)) { fi_patched = 2; return 0; }
    *(volatile DWORD *)slot = (DWORD)fast_inflate;
    VirtualProtect(slot, 4, old, &old);
    fi_patched = 1;
    sl_mark("fast inflate: texture decompression uses libdeflate");
    return 0;
}

void fastinflate_start(void)
{
    DWORD tid;
    HANDLE t = CreateThread(0, 0, fastinflate_thread, 0, 0, &tid);
    if (t) CloseHandle(t);
}

/* logged at every map open, to tell the loads apart */
static unsigned put_u(char *b, unsigned n, DWORD v)
{
    char t[12]; int k = 0;
    do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (k) b[n++] = t[--k];
    return n;
}
void fi_mark(void)
{
    char b[200]; unsigned n = 0; const char *p;
    if (fi_patched != 1) return;
    for (p = "fast inflate so far: textures "; *p; ) b[n++] = *p++;
    n = put_u(b, n, fi_fast);
    for (p = ", KB in "; *p; ) b[n++] = *p++;  n = put_u(b, n, fi_kb_in);
    for (p = ", KB out "; *p; ) b[n++] = *p++; n = put_u(b, n, fi_kb_out);
    for (p = ", repeats "; *p; ) b[n++] = *p++; n = put_u(b, n, fi_dups);
    for (p = " ("; *p; ) b[n++] = *p++; n = put_u(b, n, fi_dup_kb_out);
    for (p = " KB out)"; *p; ) b[n++] = *p++;
    b[n] = 0;
    sl_mark(b);
}
