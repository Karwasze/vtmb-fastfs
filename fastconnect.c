/*
 * vtmb-fastfs: fast connect ("Fast Connect = 1").
 *
 * Every level change in Bloodlines is a local client/server reconnect. After
 * the server has built the level it sends the client the level info and the
 * string tables (tens of KB) through the netchan, which splits them into
 * 1 KB fragments and sends one fragment per packet exchange. The client
 * exchanges one packet per engine frame, and the frame rate is capped by
 * fps_max (65 with the Unofficial Patch), so this alone took a constant ~1 s
 * per transition (and per save load).
 *
 * While the client is connecting (cls.state 1..3) this module sets the
 * runtime value of fps_max to 0 ("no cap"; the engine still runs at most one
 * frame per millisecond). As soon as the player is in the game (state 4), or
 * back at the menu (state 0), the previous value is restored. Only the
 * in-memory float the frame limiter reads is changed; the cvar's string and
 * the config files are untouched, so nothing is saved.
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
__declspec(dllimport) BOOL   WINAPI IsBadReadPtr(const void *, unsigned);
__declspec(dllimport) DWORD  WINAPI GetTickCount(void);

void sl_mark(const char *text);

#define ENGINE_PREF     0x20000000u
#define CLS_STATE_VA    0x201dbe50u
#define FPSMAX_PARENT_VA 0x20b42884u   /* ConVar fps_max (0x20b42880) -> m_pParent */
#define CVAR_FVALUE     0x28           /* ConVar::m_fValue, read by Host_FilterTime */
#define CL_SIGNON_VA    0x2020b658u
#define SCR_SKIPUPDATE_VA 0x20b42490u  /* SCR_UpdateScreen returns at once while set */

static BYTE *fc_eng;
volatile DWORD fcn_lifted;             /* number of loads the cap was lifted for (for the log) */
volatile DWORD fcn_skips;              /* number of loads with skipped loading-screen redraws */
volatile DWORD fcn_unstuck;            /* watchdog resets (should stay 0) */

static int engine_ok(BYTE *b)
{
    DWORD pe;
    if (!b || *(WORD *)b != 0x5A4D) return 0;
    pe = *(DWORD *)(b + 0x3C);
    return *(DWORD *)(b + pe + 8) == 0x41afaf48u && *(DWORD *)(b + pe + 24 + 56) == 0x13bb000u;
}

static volatile float *fps_value(void)
{
    BYTE *parent = *(BYTE **)(fc_eng + (FPSMAX_PARENT_VA - ENGINE_PREF));
    if (!parent || IsBadReadPtr(parent, CVAR_FVALUE + 4)) return 0;
    return (volatile float *)(parent + CVAR_FVALUE);
}

static DWORD WINAPI fastconnect_thread(void *unused)
{
    int lifted = 0, skipping = 0;
    float saved = 0;
    DWORD skip_since = 0, now, stuck_since = 0;
    volatile int *skipflag;
    (void)unused;
    while (!(fc_eng = (BYTE *)GetModuleHandleA("engine.dll"))) Sleep(10);
    if (!engine_ok(fc_eng)) { sl_mark("fast connect: unexpected engine.dll build, off"); return 0; }
    skipflag = (volatile int *)(fc_eng + (SCR_SKIPUPDATE_VA - ENGINE_PREF));
    for (;;) {
        int st = *(volatile int *)(fc_eng + (CLS_STATE_VA - ENGINE_PREF));
        int signon = *(volatile int *)(fc_eng + (CL_SIGNON_VA - ENGINE_PREF));
        int connecting = st >= 1 && st <= 3;
        volatile float *fv = fps_value();
        now = GetTickCount();
        /* 1. no frame cap while connecting (re-applied if a config sets fps_max mid-load) */
        if (fv) {
            if (connecting) {
                float v = *fv;
                if (v != 0.0f) { saved = v; *fv = 0.0f; if (!lifted) fcn_lifted++; lifted = 1; }
            } else if (lifted) {
                if (*fv == 0.0f) *fv = saved;   /* unless the user changed fps_max meanwhile */
                lifted = 0;
            }
        }
        /* 2. while the level data streams in (connected, signon < 2), don't redraw the
              loading screen: each redraw waits for the monitor's refresh (V-Sync), which
              limits the transfer to one packet exchange per refresh. At most 2 s. */
        {
            int want = st == 3 && signon < 2 && (!skipping || now - skip_since < 2000);
            if (want && !skipping) {
                if (*skipflag == 0) { *skipflag = 1; skipping = 1; skip_since = now; fcn_skips++; }
            } else if (!want && skipping == 1) {
                if (*skipflag == 1) *skipflag = 0;
                skipping = (st == 3 && signon < 2) ? 2 : 0;   /* 2 = timed out: wait for this load to end */
            } else if (skipping == 2 && !(st == 3 && signon < 2)) skipping = 0;
        }
        /* 3. watchdog: the engine itself sets the flag around short synchronous calls and
              restores the previous value afterwards; should that ever leave it stuck while
              in the game, clear it after 5 s */
        if (st == 4 && !skipping && *skipflag) {
            if (!stuck_since) stuck_since = now ? now : 1;
            else if (now - stuck_since > 5000) { *skipflag = 0; stuck_since = 0; fcn_unstuck++; }
        } else stuck_since = 0;
        Sleep(1);
    }
}

void fastconnect_start(void)
{
    DWORD tid;
    HANDLE t = CreateThread(0, 0, fastconnect_thread, 0, 0, &tid);
    if (t) CloseHandle(t);
}
int _fltused = 0;
