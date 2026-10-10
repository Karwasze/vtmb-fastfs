/*
 * vtmb-fastfs: no fade-in from black after loading a level ("Skip Load Fade-In = 1").
 *
 * Maps whose worldspawn has "startdark" set make the game DLL (vampire.dll,
 * world precache at 0x1023c322) set the client cvar v_dark = 1 on every load.
 * The client's view effects then start the level with a 5-second fade from
 * black and reset v_dark. This module makes the game DLL always set v_dark = 0
 * instead, so the level is visible as soon as the player gets control. Other
 * fades (env_fade, cutscenes, deaths) are not affected.
 *
 * The patch turns "setne cl" (cl = startdark != 0) into nops; ecx is zeroed just
 * before it. Applied only if the code matches exactly.
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
__declspec(dllimport) DWORD  WINAPI GetTickCount(void);

void sl_mark(const char *text);

volatile DWORD ff_patched;

#define SITE_RVA 0x23c322u
/* mov ecx,[cvar]; push "v_dark"; mov edx,[ecx]; call [edx+8]; test eax,eax; je +13h;
   mov edx,[eax]; xor ecx,ecx; cmp [esi+478h],cl; setne cl; push ecx; mov ecx,eax; call [edx+10h] */
static const BYTE site_code[] = {
    0x8b, 0x0d, 0x34, 0xb2, 0x70, 0x10, 0x68, 0xcc, 0x44, 0x5c, 0x10, 0x8b, 0x11, 0xff, 0x52, 0x08,
    0x85, 0xc0, 0x74, 0x13, 0x8b, 0x10, 0x33, 0xc9, 0x38, 0x8e, 0x78, 0x04, 0x00, 0x00, 0x0f, 0x95,
    0xc1, 0x51, 0x8b, 0xc8, 0xff, 0x52, 0x10 };
#define SETNE_OFF 30

static int site_ok(BYTE *m, BYTE *p)
{
    DWORD i;
    for (i = 0; i < sizeof site_code; i++) {
        if ((i >= 2 && i < 6) || (i >= 7 && i < 11)) continue;
        if (p[i] != site_code[i]) return 0;
    }
    if (*(DWORD *)(p + 2) - (DWORD)m != *(const DWORD *)(site_code + 2) - 0x10000000u) return 0;
    if (*(DWORD *)(p + 7) - (DWORD)m != *(const DWORD *)(site_code + 7) - 0x10000000u) return 0;
    { const char *v = (const char *)*(DWORD *)(p + 7);
      if (v[0] != 'v' || v[1] != '_' || v[2] != 'd' || v[3] != 'a' || v[4] != 'r' || v[5] != 'k' || v[6]) return 0; }
    return 1;
}

static DWORD WINAPI fadefix_thread(void *unused)
{
    BYTE *m, *p;
    DWORD i, old, tries = 0;
    (void)unused;
    while (!(m = (BYTE *)GetModuleHandleA("vampire.dll"))) Sleep(tries++ < 4000 ? 2 : 100);
    p = m + SITE_RVA;
    /* the two absolute addresses in the code (cvar interface, "v_dark") are relocated with
       the DLL; allow the loader a moment to finish relocating */
    for (i = 0; !site_ok(m, p); i++) { if (i > 300) goto unknown; Sleep(10); }
    if (!VirtualProtect(p + SETNE_OFF, 3, 0x40, &old)) goto unknown;
    p[SETNE_OFF] = 0x90; p[SETNE_OFF + 1] = 0x90; p[SETNE_OFF + 2] = 0x90;
    VirtualProtect(p + SETNE_OFF, 3, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p + SETNE_OFF, 3);
    ff_patched = 1;
    sl_mark("load fade-in: level start-dark fade disabled");
    return 0;
unknown:
    ff_patched = 2;
    sl_mark("load fade-in: unexpected vampire.dll build, off");
    return 0;
}

/*
 * Part 2: the loading screen's fade-out (client.dll).
 *
 * When a level has loaded, the client hides its loading screen (CLoadingDisc,
 * the tips screen) not at once but by fading it out over 3 seconds: it is drawn
 * fully opaque for one more second and then faded over two, on top of the new
 * level - this is the slow "fade from black" after every load. The hide code
 * sets the end of the fade to curtime + 3.0; here the "+ 3.0" (fadd qword
 * [3.0], 6 bytes at 0x101679e8) is replaced by a 6-byte nop, so the fade ends
 * immediately and the screen is hidden in the same frame. The change is swapped
 * in with one atomic 8-byte write (the game is running).
 */
volatile DWORD fx_patched, fx_seen;

#define CL_HIDE_RVA  0x1679e5u          /* fld [eax+0Ch] (curtime); fadd qword [3.0]; fstp [fadeEnd] */
#define CL_THREE_RVA 0x227ee0u          /* the 3.0 */
static const BYTE hide_code[] = { 0xd9, 0x40, 0x0c, 0xdc, 0x05, 0, 0, 0, 0, 0xd9, 0x1d };

/* replace n bytes at p (all inside one aligned 8-byte block) in one atomic write */
static int swap_bytes(BYTE *p, const BYTE *with, DWORD n)
{
    BYTE *blk = (BYTE *)((DWORD)p & ~7u);
    DWORD off = (DWORD)(p - blk), i, old;
    unsigned long long was, now;
    if (off + n > 8) return 0;
    if (!VirtualProtect(blk, 8, 0x40, &old)) return 0;
    was = *(volatile unsigned long long *)blk; now = was;
    for (i = 0; i < n; i++) ((BYTE *)&now)[off + i] = with[i];
    i = __sync_val_compare_and_swap((volatile unsigned long long *)blk, was, now) == was;
    VirtualProtect(blk, 8, old, &old);
    FlushInstructionCache(GetCurrentProcess(), blk, 8);
    return (int)i;
}

static int hide_ok(BYTE *cl)
{
    BYTE *p = cl + CL_HIDE_RVA;
    DWORD i;
    for (i = 0; i < sizeof hide_code; i++) {
        if (i >= 5 && i < 9) continue;                    /* address of the 3.0, relocated */
        if (p[i] != hide_code[i]) return 0;
    }
    if (*(DWORD *)(p + 5) != (DWORD)(cl + CL_THREE_RVA)) return 0;
    return *(double *)(cl + CL_THREE_RVA) == 3.0;
}

static DWORD WINAPI loadfade_thread(void *unused)
{
    BYTE *cl;
    DWORD tries = 0;
    static const BYTE nop6[] = { 0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00 };
    (void)unused;
    while (!(cl = (BYTE *)GetModuleHandleA("client.dll"))) Sleep(tries++ < 4000 ? 2 : 100);
    for (tries = 0; !hide_ok(cl); tries++) {
        if (tries > 300) { fx_patched = 2; sl_mark("loading screen fade-out: unexpected client.dll build, off"); return 0; }
        Sleep(10);
    }
    if (!swap_bytes(cl + CL_HIDE_RVA + 3, nop6, sizeof nop6)) {
        fx_patched = 2; sl_mark("loading screen fade-out: patch failed, off"); return 0;
    }
    fx_patched = 1;
    sl_mark("loading screen fade-out: removed (loading screen is hidden at once)");
    return 0;
}

void fadefix_start(void)
{
    DWORD tid;
    HANDLE t = CreateThread(0, 0, fadefix_thread, 0, 0, &tid);
    if (t) CloseHandle(t);
    t = CreateThread(0, 0, loadfade_thread, 0, 0, &tid);
    if (t) CloseHandle(t);
}
