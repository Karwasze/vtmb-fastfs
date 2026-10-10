/*
 * vtmb-fastfs load-time test harness.
 *
 * Activated only by command-line switches (normal play is unaffected):
 *   -fastfs_record        record the player's keyboard and mouse-button input
 *   -fastfs_replay        replay the recording and measure the transition
 *   -fastfs_label <name>  label written to the results file
 *
 * Both modes expect the game to be started straight into a save
 * (+load fastfs_test) made while standing at a door. Timeline (all modes):
 *   load start  = first map (.bsp) opened
 *   anchor      = client becomes active in that map (cls.state == 4)
 *   door        = a different map is opened (the transition starts)
 *   server done = client drops back to "connected" (cls.state 4 -> 3)
 *   client map  = client opens the new map
 *   signon 1    = first signon message from the server
 *   in game     = cls.state == 4 again
 *
 * Record: keyboard and mouse buttons are captured with low-level hooks (only
 * while the game window has focus) from the anchor until 0.5 s after "in
 * game", then saved to <game>\vtmb-fastfs-input.rec. Mouse movement is not
 * recorded: the save should already face the door.
 *
 * Replay: the same events are sent with SendInput at the same offsets from the
 * anchor - only while the game window is in the foreground; otherwise the run
 * is aborted and every key that was pressed is released. Results are appended
 * to <game>\vtmb-fastfs-results.csv and the game is closed.
 *
 * In both modes the startup videos (Activision, Troika, White Wolf, NVIDIA logos)
 * are skipped, so a run goes straight to loading the save.
 *
 * Reads engine.dll globals only on the exact build it was written for.
 */

typedef unsigned int   DWORD;
typedef int            BOOL;
typedef void          *HANDLE;
typedef void          *HMODULE;
typedef void          *HWND;
typedef unsigned char  BYTE;
typedef unsigned short WORD;
typedef const char    *LPCSTR;
typedef long           LONG;
typedef unsigned long  ULONG_PTR;
typedef long           LRESULT;
typedef unsigned int   WPARAM;
typedef long           LPARAM;
#define WINAPI __stdcall
#define CALLBACK __stdcall
#define INVALID_HANDLE_VALUE ((HANDLE)(long)-1)

typedef struct { WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds; } SYSTEMTIME;
typedef struct { LONG x, y; } POINT;
typedef struct { HWND hwnd; DWORD message; WPARAM wParam; LPARAM lParam; DWORD time; POINT pt; } MSG;
typedef struct { DWORD vkCode, scanCode, flags, time; ULONG_PTR dwExtraInfo; } KBDLLHOOKSTRUCT;
typedef struct { POINT pt; DWORD mouseData, flags, time; ULONG_PTR dwExtraInfo; } MSLLHOOKSTRUCT;
typedef struct { LONG dx, dy; DWORD mouseData, dwFlags, time; ULONG_PTR dwExtraInfo; } MOUSEINPUT;
typedef struct { WORD wVk, wScan; DWORD dwFlags, time; ULONG_PTR dwExtraInfo; } KEYBDINPUT;
typedef struct { DWORD type; union { MOUSEINPUT mi; KEYBDINPUT ki; } u; } INPUT;
typedef LRESULT (CALLBACK *HOOKPROC)(int, WPARAM, LPARAM);

__declspec(dllimport) HMODULE WINAPI GetModuleHandleA(LPCSTR);
__declspec(dllimport) HANDLE WINAPI CreateThread(void *, DWORD, DWORD (WINAPI *)(void *), void *, DWORD, DWORD *);
__declspec(dllimport) BOOL   WINAPI CloseHandle(HANDLE);
__declspec(dllimport) void   WINAPI Sleep(DWORD);
__declspec(dllimport) DWORD  WINAPI timeGetTime(void);
__declspec(dllimport) unsigned WINAPI timeBeginPeriod(unsigned);
__declspec(dllimport) HANDLE WINAPI CreateFileA(LPCSTR, DWORD, DWORD, void *, DWORD, DWORD, HANDLE);
__declspec(dllimport) BOOL   WINAPI WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
__declspec(dllimport) BOOL   WINAPI ReadFile(HANDLE, void *, DWORD, DWORD *, void *);
__declspec(dllimport) DWORD  WINAPI SetFilePointer(HANDLE, LONG, LONG *, DWORD);
__declspec(dllimport) void * WINAPI VirtualAlloc(void *, DWORD, DWORD, DWORD);
__declspec(dllimport) DWORD  WINAPI GetCurrentProcessId(void);
__declspec(dllimport) DWORD  WINAPI GetCurrentThreadId(void);
__declspec(dllimport) HANDLE WINAPI GetCurrentProcess(void);
__declspec(dllimport) BOOL   WINAPI TerminateProcess(HANDLE, DWORD);
__declspec(dllimport) void   WINAPI GetLocalTime(SYSTEMTIME *);
__declspec(dllimport) const char * WINAPI GetCommandLineA(void);
__declspec(dllimport) HWND   WINAPI GetForegroundWindow(void);
__declspec(dllimport) DWORD  WINAPI GetWindowThreadProcessId(HWND, DWORD *);
__declspec(dllimport) DWORD  WINAPI SendInput(DWORD, INPUT *, int);
__declspec(dllimport) HANDLE WINAPI SetWindowsHookExA(int, HOOKPROC, HMODULE, DWORD);
__declspec(dllimport) BOOL   WINAPI UnhookWindowsHookEx(HANDLE);
__declspec(dllimport) LRESULT WINAPI CallNextHookEx(HANDLE, int, WPARAM, LPARAM);
__declspec(dllimport) BOOL   WINAPI GetMessageA(MSG *, HWND, DWORD, DWORD);
__declspec(dllimport) BOOL   WINAPI PostThreadMessageA(DWORD, DWORD, WPARAM, LPARAM);
__declspec(dllimport) BOOL   WINAPI MessageBeep(DWORD);

void sl_mark(const char *text);
void sl_tick(void);

/* ---- engine state (engine.dll build 0x41afaf48 only) ---- */
#define ENGINE_PREF 0x20000000u
#define CLS_STATE_VA 0x201dbe50u
#define CL_SIGNON_VA 0x2020b658u
static BYTE *t_eng;

static int engine_ok(BYTE *b)
{
    DWORD pe;
    if (!b || *(WORD *)b != 0x5A4D) return 0;
    pe = *(DWORD *)(b + 0x3C);
    return *(DWORD *)(b + pe + 8) == 0x41afaf48u && *(DWORD *)(b + pe + 24 + 56) == 0x13bb000u;
}
static int cls_state(void) { return *(volatile int *)(t_eng + (CLS_STATE_VA - ENGINE_PREF)); }
static int cl_signon(void) { return *(volatile int *)(t_eng + (CL_SIGNON_VA - ENGINE_PREF)); }

/* ---- small helpers ---- */
static unsigned slen_(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static unsigned put_s(char *b, unsigned n, const char *s) { while (*s) b[n++] = *s++; return n; }
static unsigned put_i(char *b, unsigned n, int v)
{
    char t[12]; int k = 0; unsigned u = v < 0 ? (unsigned)-v : (unsigned)v;
    if (v < 0) b[n++] = '-';
    do { t[k++] = (char)('0' + u % 10); u /= 10; } while (u);
    while (k) b[n++] = t[--k];
    return n;
}
static unsigned put_2(char *b, unsigned n, int v) { b[n++] = (char)('0' + v / 10 % 10); b[n++] = (char)('0' + v % 10); return n; }

/* ---- state shared with the file hooks ---- */
enum { TH_OFF = 0, TH_RECORD = 1, TH_REPLAY = 2 };
static int  th_mode;
static char th_label[64] = "default";
static char th_dir[520];
static volatile DWORD th_bsp_t;        /* time of the latest .bsp open */
static volatile DWORD th_bsp_seq;      /* number of .bsp opens */
static char th_bsp_name[64];           /* map file name of the latest open */
static char th_first_map[64];

/* Called from the CreateFileA hook for every .bsp opened under maps\. */
void th_note_bsp(const char *key)
{
    const char *b = key, *p;
    unsigned n = 0;
    if (!th_mode) return;
    for (p = key; *p; p++) if (*p == '\\') b = p + 1;
    while (b[n] && n < sizeof th_bsp_name - 1) { th_bsp_name[n] = b[n]; n++; }
    th_bsp_name[n] = 0;
    th_bsp_t = timeGetTime();
    th_bsp_seq++;
}

/* ---- input events ---- */
typedef struct { DWORD t; BYTE kind, flags; WORD code; DWORD data; } Ev;   /* 12 bytes */
enum { EV_KEY = 1, EV_MBTN = 2, EV_WHEEL = 3 };
enum { F_UP = 1, F_EXT = 2 };
#define MAXEV 100000
static Ev   *ev;
static DWORD nev;
static volatile DWORD rec_on, rec_t0, rec_moves;
static DWORD rec_tid;

static int game_has_focus(void)
{
    DWORD pid = 0;
    HWND w = GetForegroundWindow();
    if (!w) return 0;
    GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}

static void rec_add(BYTE kind, BYTE flags, WORD code, DWORD data)
{
    if (!rec_on || nev >= MAXEV || !game_has_focus()) return;
    ev[nev].t = timeGetTime() - rec_t0;
    ev[nev].kind = kind; ev[nev].flags = flags; ev[nev].code = code; ev[nev].data = data;
    nev++;
}

static HANDLE hk_kb, hk_ms;
static LRESULT CALLBACK ll_kb(int code, WPARAM wp, LPARAM lp)
{
    if (code >= 0) {
        KBDLLHOOKSTRUCT *k = (KBDLLHOOKSTRUCT *)lp;
        if (!(k->flags & 0x10 /*INJECTED*/))
            rec_add(EV_KEY, (BYTE)(((k->flags & 0x80) ? F_UP : 0) | ((k->flags & 0x01) ? F_EXT : 0)),
                    (WORD)k->scanCode, k->vkCode);
    }
    (void)wp;
    return CallNextHookEx(0, code, wp, lp);
}
static LRESULT CALLBACK ll_ms(int code, WPARAM wp, LPARAM lp)
{
    if (code >= 0) {
        MSLLHOOKSTRUCT *m = (MSLLHOOKSTRUCT *)lp;
        if (!(m->flags & 0x01 /*INJECTED*/)) {
            switch (wp) {
            case 0x201: rec_add(EV_MBTN, 0, 1, 0); break;  case 0x202: rec_add(EV_MBTN, F_UP, 1, 0); break;
            case 0x204: rec_add(EV_MBTN, 0, 2, 0); break;  case 0x205: rec_add(EV_MBTN, F_UP, 2, 0); break;
            case 0x207: rec_add(EV_MBTN, 0, 3, 0); break;  case 0x208: rec_add(EV_MBTN, F_UP, 3, 0); break;
            case 0x20B: rec_add(EV_MBTN, 0, (WORD)(3 + (m->mouseData >> 16)), 0); break;
            case 0x20C: rec_add(EV_MBTN, F_UP, (WORD)(3 + (m->mouseData >> 16)), 0); break;
            case 0x20A: rec_add(EV_WHEEL, 0, 0, m->mouseData >> 16); break;
            case 0x200: if (rec_on && game_has_focus()) rec_moves++; break;
            }
        }
    }
    return CallNextHookEx(0, code, wp, lp);
}

static DWORD WINAPI hook_thread(void *unused)
{
    MSG m;
    (void)unused;
    hk_kb = SetWindowsHookExA(13 /*WH_KEYBOARD_LL*/, ll_kb, GetModuleHandleA(0), 0);
    hk_ms = SetWindowsHookExA(14 /*WH_MOUSE_LL*/, ll_ms, GetModuleHandleA(0), 0);
    while (GetMessageA(&m, 0, 0, 0) > 0) {}
    if (hk_kb) UnhookWindowsHookEx(hk_kb);
    if (hk_ms) UnhookWindowsHookEx(hk_ms);
    return 0;
}

static void path_of(char *out, const char *name)
{
    unsigned n = 0, k = 0;
    while (th_dir[n]) { out[n] = th_dir[n]; n++; }
    while (name[k]) out[n++] = name[k++];
    out[n] = 0;
}

static int save_recording(void)
{
    char p[560];
    DWORD w, hdr[3];
    HANDLE f;
    path_of(p, "vtmb-fastfs-input.rec");
    f = CreateFileA(p, 0x40000000UL, 1, 0, 2, 0x80, 0);
    if (f == INVALID_HANDLE_VALUE) return 0;
    hdr[0] = 0x31434552; /* "REC1" */ hdr[1] = nev; hdr[2] = sizeof(Ev);
    WriteFile(f, hdr, sizeof hdr, &w, 0);
    WriteFile(f, ev, nev * sizeof(Ev), &w, 0);
    CloseHandle(f);
    return 1;
}

static int load_recording(void)
{
    char p[560];
    DWORD got, hdr[3];
    HANDLE f;
    path_of(p, "vtmb-fastfs-input.rec");
    f = CreateFileA(p, 0x80000000UL, 1, 0, 3, 0x80, 0);
    if (f == INVALID_HANDLE_VALUE) return 0;
    if (!ReadFile(f, hdr, sizeof hdr, &got, 0) || got != sizeof hdr || hdr[0] != 0x31434552 ||
        hdr[2] != sizeof(Ev) || hdr[1] > MAXEV) { CloseHandle(f); return 0; }
    nev = hdr[1];
    if (!ReadFile(f, ev, nev * sizeof(Ev), &got, 0) || got != nev * sizeof(Ev)) { CloseHandle(f); nev = 0; return 0; }
    CloseHandle(f);
    return 1;
}

/* ---- replay ---- */
static BYTE key_down[512];   /* scan | ext<<8 */
static BYTE btn_down[8];

/* Without focus, input goes to the game window as window messages (Source reads the
   keyboard from WM_KEYDOWN/WM_KEYUP), so replay does not depend on the foreground. */
__declspec(dllimport) BOOL WINAPI PostMessageA(HWND, unsigned, WPARAM, LPARAM);
__declspec(dllimport) unsigned WINAPI MapVirtualKeyA(unsigned, unsigned);
static HWND game_wnd(void);
static int  game_has_focus(void);
static DWORD th_posted;

static void send_key(WORD scan, int ext, int up)
{
    INPUT in;
    if (!game_has_focus()) {
        HWND w = game_wnd();
        if (w) {
            unsigned vk = MapVirtualKeyA(scan | (ext ? 0xE000 : 0), 3 /*MAPVK_VSC_TO_VK_EX*/);
            LPARAM lp = 1 | ((LPARAM)(scan & 0xff) << 16) | (ext ? (1L << 24) : 0) | (up ? (3L << 30) : 0);
            PostMessageA(w, up ? 0x101 : 0x100, vk, lp);
            th_posted++;
        }
        return;
    }
    BYTE *z = (BYTE *)&in; unsigned i;
    for (i = 0; i < sizeof in; i++) z[i] = 0;
    in.type = 1;
    in.u.ki.wScan = scan;
    in.u.ki.dwFlags = 0x8 /*SCANCODE*/ | (ext ? 0x1 : 0) | (up ? 0x2 : 0);
    SendInput(1, &in, sizeof in);
}
static void send_btn(int b, int up, DWORD wheel)
{
    INPUT in;
    BYTE *z = (BYTE *)&in; unsigned i;
    static const DWORD dn[4] = { 0x2, 0x8, 0x20, 0x80 }, upf[4] = { 0x4, 0x10, 0x40, 0x100 };
    if (!game_has_focus()) {
        static const unsigned wdn[4] = { 0x201, 0x204, 0x207, 0x20B }, wup[4] = { 0x202, 0x205, 0x208, 0x20C };
        HWND w = game_wnd();
        if (w && b >= 1) {
            unsigned k = b <= 3 ? (unsigned)b - 1 : 3;
            PostMessageA(w, up ? wup[k] : wdn[k], b > 3 ? ((WPARAM)(b - 3) << 16) : 0, 0);
            th_posted++;
        } else if (w) PostMessageA(w, 0x20A, (WPARAM)(wheel << 16), 0);
        return;
    }
    for (i = 0; i < sizeof in; i++) z[i] = 0;
    in.type = 0;
    if (b == 0) { in.u.mi.dwFlags = 0x800; in.u.mi.mouseData = (DWORD)(short)wheel; }
    else if (b <= 3) in.u.mi.dwFlags = up ? upf[b - 1] : dn[b - 1];
    else { in.u.mi.dwFlags = up ? upf[3] : dn[3]; in.u.mi.mouseData = (DWORD)(b - 3); }
    SendInput(1, &in, sizeof in);
}
/* ---- bring the game window to the front (replay needs it there) ---- */
__declspec(dllimport) BOOL WINAPI EnumWindows(BOOL (CALLBACK *)(HWND, LPARAM), LPARAM);
__declspec(dllimport) BOOL WINAPI IsWindowVisible(HWND);
__declspec(dllimport) HWND WINAPI GetWindow(HWND, unsigned);
__declspec(dllimport) BOOL WINAPI SetForegroundWindow(HWND);
__declspec(dllimport) BOOL WINAPI BringWindowToTop(HWND);
__declspec(dllimport) BOOL WINAPI AttachThreadInput(DWORD, DWORD, BOOL);

static BOOL CALLBACK find_game_wnd(HWND w, LPARAM out)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(w) && !GetWindow(w, 4 /*GW_OWNER*/)) {
        *(HWND *)out = w;
        return 0;
    }
    return 1;
}

__declspec(dllimport) int  WINAPI GetClassNameA(HWND, char *, int);
__declspec(dllimport) int  WINAPI GetWindowTextA(HWND, char *, int);
__declspec(dllimport) HANDLE WINAPI OpenProcess(DWORD, BOOL, DWORD);
__declspec(dllimport) BOOL WINAPI QueryFullProcessImageNameA(HANDLE, DWORD, char *, DWORD *);

/* Logs which window holds the foreground (for "lost focus" failures). */
static void log_foreground(const char *what, HWND me, DWORD tries, int sfw)
{
    char m[700], tmp[260];
    unsigned k = put_s(m, 0, what);
    HWND fg = GetForegroundWindow();
    k = put_s(m, k, " | game window found: "); k = put_s(m, k, me ? "yes" : "NO");
    k = put_s(m, k, " | attempts: "); k = put_i(m, k, (int)tries);
    k = put_s(m, k, " | last SetForegroundWindow: "); k = put_i(m, k, sfw);
    if (!fg) k = put_s(m, k, " | foreground: none");
    else {
        DWORD pid = 0, n = sizeof tmp; HANDLE p; int c;
        GetWindowThreadProcessId(fg, &pid);
        k = put_s(m, k, " | foreground pid "); k = put_i(m, k, (int)pid);
        p = OpenProcess(0x1000 /*QUERY_LIMITED_INFORMATION*/, 0, pid);
        if (p) {
            if (QueryFullProcessImageNameA(p, 0, tmp, &n)) { tmp[n] = 0; k = put_s(m, k, " exe "); k = put_s(m, k, tmp); }
            CloseHandle(p);
        }
        c = GetClassNameA(fg, tmp, 100); if (c > 0) { tmp[c] = 0; k = put_s(m, k, " class "); k = put_s(m, k, tmp); }
        c = GetWindowTextA(fg, tmp, 100); if (c > 0) { tmp[c] = 0; k = put_s(m, k, " title \""); k = put_s(m, k, tmp); k = put_s(m, k, "\""); }
    }
    m[k] = 0; sl_mark(m);
}

static HWND game_wnd(void)
{
    HWND me = 0;
    EnumWindows(find_game_wnd, (LPARAM)&me);
    return me;
}

/* Tries for up to `ms` to get the game window into the foreground. */
static int grab_focus(DWORD ms)
{
    DWORD t0 = timeGetTime(), tries = 0;
    HWND me = 0;
    int sfw = -1, logged = 0;
    while (!game_has_focus()) {
        HWND fg;
        DWORD fg_tid, my_tid;
        if (timeGetTime() - t0 >= ms) { log_foreground("test: could not bring the game to the front", me, tries, sfw); return 0; }
        if (!logged) { log_foreground("test: game is not in front", me, tries, sfw); logged = 1; }
        me = 0;
        EnumWindows(find_game_wnd, (LPARAM)&me);
        if (me) {
            fg = GetForegroundWindow();
            fg_tid = fg ? GetWindowThreadProcessId(fg, 0) : 0;
            my_tid = GetWindowThreadProcessId(me, 0);
            if (tries & 1) {                       /* every other try: the Alt-tap trick */
                INPUT in[2];
                BYTE *z = (BYTE *)in; unsigned i;
                for (i = 0; i < sizeof in; i++) z[i] = 0;
                in[0].type = in[1].type = 1;
                in[0].u.ki.wVk = in[1].u.ki.wVk = 0x12; /* VK_MENU */
                in[1].u.ki.dwFlags = 0x2;
                SendInput(2, in, sizeof in[0]);
            }
            if (fg_tid && fg_tid != my_tid) AttachThreadInput(fg_tid, my_tid, 1);
            BringWindowToTop(me);
            sfw = SetForegroundWindow(me);
            if (fg_tid && fg_tid != my_tid) AttachThreadInput(fg_tid, my_tid, 0);
            tries++;
        }
        Sleep(100);
    }
    if (tries) {
        char m[80]; unsigned k = put_s(m, 0, "test: brought the game window to the front (attempts: ");
        k = put_i(m, k, (int)tries); k = put_s(m, k, ")"); m[k] = 0; sl_mark(m);
    }
    return 1;
}

static void release_all(void)
{
    unsigned i;
    for (i = 0; i < 512; i++) if (key_down[i]) { send_key((WORD)(i & 0xff), i >> 8, 1); key_down[i] = 0; }
    for (i = 1; i < 8; i++) if (btn_down[i]) { send_btn((int)i, 1, 0); btn_down[i] = 0; }
}

/* ---- results ---- */
static void write_result(const char *status, int load_ms, int door_ms, int server_ms,
                         int climap_ms, int signon_wait_ms, int final_ms)
{
    char p[560], line[400];
    unsigned n = 0;
    DWORD w, sz;
    HANDLE f;
    SYSTEMTIME st;
    path_of(p, "vtmb-fastfs-results.csv");
    f = CreateFileA(p, 0x40000000UL, 1, 0, 4 /*OPEN_ALWAYS*/, 0x80, 0);
    if (f == INVALID_HANDLE_VALUE) return;
    sz = SetFilePointer(f, 0, 0, 2 /*FILE_END*/);
    if (sz == 0) {
        const char *h = "time,label,mode,result,save_load_ms,door_to_control_ms,server_build_ms,client_reconnect_ms,"
                        "signon_wait_ms,signon_to_control_ms\r\n";
        WriteFile(f, h, slen_(h), &w, 0);
    }
    GetLocalTime(&st);
    n = put_i(line, n, st.wYear); line[n++] = '-'; n = put_2(line, n, st.wMonth); line[n++] = '-'; n = put_2(line, n, st.wDay);
    line[n++] = ' '; n = put_2(line, n, st.wHour); line[n++] = ':'; n = put_2(line, n, st.wMinute); line[n++] = ':'; n = put_2(line, n, st.wSecond);
    line[n++] = ','; n = put_s(line, n, th_label);
    line[n++] = ','; n = put_s(line, n, th_mode == TH_RECORD ? "record" : "replay");
    line[n++] = ','; n = put_s(line, n, status);
    line[n++] = ','; n = put_i(line, n, load_ms);
    line[n++] = ','; n = put_i(line, n, door_ms);
    line[n++] = ','; n = put_i(line, n, server_ms);
    line[n++] = ','; n = put_i(line, n, climap_ms);
    line[n++] = ','; n = put_i(line, n, signon_wait_ms);
    line[n++] = ','; n = put_i(line, n, final_ms);
    line[n++] = '\r'; line[n++] = '\n';
    WriteFile(f, line, n, &w, 0);
    CloseHandle(f);
    {
        char m[480]; unsigned k = put_s(m, 0, "test result: ");
        line[n - 2] = 0; k = put_s(m, k, line); m[k] = 0; sl_mark(m);
    }
}

void prof_flush(void);
void tp_flush(void);
static void finish(void)
{
    if (th_mode == TH_REPLAY) {
        release_all();
        sl_tick();
        Sleep(3000);                       /* let the autosave finish */
        prof_flush();                      /* write a profile capture in progress */
        tp_flush();                        /* save the texture list being learned */
        sl_tick();
        TerminateProcess(GetCurrentProcess(), 0);
    }
}

/* ---- skip the startup videos in test runs ----
 * The engine plays vampire\media\{activision,troika,whitewolf,NvidiaBloodLogo}.bik
 * through binkw32!BinkOpen. Reporting "not found" for exactly those files is the
 * same as the Unofficial Patch readme's tip of renaming the media folder; the
 * engine just moves on. Every other video (cutscenes) is passed through. */
__declspec(dllimport) BOOL WINAPI VirtualProtect(void *, DWORD, DWORD, DWORD *);
typedef void *(WINAPI *PFN_BinkOpen)(const char *, DWORD);
static PFN_BinkOpen real_BinkOpen;
static DWORD *bink_slot;
static volatile DWORD th_skipped;

static int ends_with_ci(const char *s, const char *suf)
{
    unsigned a = slen_(s), b = slen_(suf), i;
    if (b > a) return 0;
    for (i = 0; i < b; i++) {
        char x = s[a - b + i], y = suf[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (x == '/') x = '\\';
        if (x != y) return 0;
    }
    return 1;
}

static void *WINAPI hk_BinkOpen(const char *name, DWORD flags)
{
    static const char *intro[] = { "media\\activision.bik", "media\\troika.bik",
                                   "media\\whitewolf.bik", "media\\nvidiabloodlogo.bik" };
    unsigned i;
    if (name)
        for (i = 0; i < 4; i++)
            if (ends_with_ci(name, intro[i])) { th_skipped++; return 0; }
    return real_BinkOpen(name, flags);
}

static int ieq_(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return *a == *b;
}

static int patch_bink(BYTE *base)
{
    DWORD pe = *(DWORD *)(base + 0x3C), imp, old;
    BYTE *opt = base + pe + 24;
    DWORD *d;
    if (*(WORD *)opt != 0x10B) return 0;
    imp = *(DWORD *)(opt + 96 + 8);
    if (!imp) return 0;
    for (d = (DWORD *)(base + imp); d[3]; d += 5) {          /* IMAGE_IMPORT_DESCRIPTOR */
        DWORD *names, *slots;
        if (!ieq_((const char *)(base + d[3]), "binkw32.dll")) continue;
        names = (DWORD *)(base + (d[0] ? d[0] : d[4]));
        slots = (DWORD *)(base + d[4]);
        for (; *names; names++, slots++) {
            if (*names & 0x80000000u) continue;
            if (ieq_((const char *)(base + *names + 2), "_BinkOpen@8")) {
                if (!VirtualProtect(slots, 4, 0x04, &old)) return 0;
                if (*slots != (DWORD)hk_BinkOpen) real_BinkOpen = (PFN_BinkOpen)*slots;
                *slots = (DWORD)hk_BinkOpen;
                bink_slot = slots;
                VirtualProtect(slots, 4, old, &old);
                return 1;
            }
        }
    }
    return 0;
}

static DWORD WINAPI bink_guard(void *unused)
{
    DWORD s = timeGetTime();
    (void)unused;
    while (timeGetTime() - s < 30000) {
        if (bink_slot && *(volatile DWORD *)bink_slot != (DWORD)hk_BinkOpen) patch_bink(t_eng);
        Sleep(2);
    }
    return 0;
}

#define WAIT_UNTIL(cond, limit_ms) do { DWORD _s = timeGetTime(); \
    while (!(cond)) { if (timeGetTime() - _s > (limit_ms)) goto timeout; Sleep(1); } } while (0)

/* The engine sleeps 20-50 ms per frame while its window is not the active one
   (CGame::SleepUntilInput, called from CEngine::Frame). Test runs may have the
   game in the background, so in test mode that call does nothing. */
#define GAME_VT_SLEEP_VA 0x20186d08u   /* CGame vtable slot 4 */
#define GAME_SLEEP_FN_VA 0x200fe5d0u
static void __fastcall no_sleep(void *self, void *edx, int ms) { (void)self; (void)edx; (void)ms; }
static int patch_bg_sleep(BYTE *eng)
{
    DWORD *slot = (DWORD *)(eng + (GAME_VT_SLEEP_VA - ENGINE_PREF)), old;
    if (*slot != (DWORD)(eng + (GAME_SLEEP_FN_VA - ENGINE_PREF))) return 0;
    if (!VirtualProtect(slot, 4, 0x04, &old)) return 0;
    *slot = (DWORD)no_sleep;
    VirtualProtect(slot, 4, old, &old);
    return 1;
}

static DWORD WINAPI harness(void *unused)
{
    DWORD t_load0, t_anchor, t_door, t_srv, t_climap = 0, t_sig1 = 0, t_in2, seq0, i = 0;
    const char *why = "timeout";
    (void)unused;
    timeBeginPeriod(1);
    while (!(t_eng = (BYTE *)GetModuleHandleA("engine.dll"))) Sleep(1);
    if (!engine_ok(t_eng)) { sl_mark("test: unexpected engine.dll build, harness disabled"); return 0; }
    if (!patch_bink(t_eng)) sl_mark("test: could not hook BinkOpen, intro videos will play");
    if (!patch_bg_sleep(t_eng)) sl_mark("test: could not disable the background-window throttle");
    else { DWORD tid; HANDLE h = CreateThread(0, 0, bink_guard, 0, 0, &tid); if (h) CloseHandle(h); }
    if (th_mode == TH_REPLAY && !load_recording()) {
        write_result("FAIL no recording (run record-load-test.bat first)", -1, -1, -1, -1, -1, -1);
        finish(); return 0;
    }
    if (th_mode == TH_RECORD) {
        DWORD tid;
        HANDLE h = CreateThread(0, 0, hook_thread, 0, 0, &tid);
        if (h) CloseHandle(h);
        rec_tid = tid;
    }

    /* 1. save load: first map opened, then client active */
    why = "save did not load (is there a save named fastfs_test?)";
    WAIT_UNTIL(th_bsp_seq > 0, 180000);
    t_load0 = th_bsp_t;
    { unsigned k = 0; while (th_bsp_name[k] && k < 63) { th_first_map[k] = th_bsp_name[k]; k++; } th_first_map[k] = 0; }
    WAIT_UNTIL(cls_state() == 4, 120000);
    t_anchor = timeGetTime();
    seq0 = th_bsp_seq;
    sl_mark(th_mode == TH_RECORD ? "test: in game - recording your input now" : "test: in game - replaying input");
    {
        char m[80]; unsigned k = put_s(m, 0, "test: intro videos skipped: ");
        k = put_i(m, k, (int)th_skipped); m[k] = 0; sl_mark(m);
    }

    /* 2. input: record or replay until the door is used */
    if (th_mode == TH_RECORD) {
        rec_t0 = t_anchor;
        rec_on = 1;
        why = "door not used within 10 minutes";
    } else {
        DWORD last = nev ? ev[nev - 1].t : 0;
        why = "game window not found";
        grab_focus(1000);   /* nice to have; without focus, input is posted to the window */
        while (i < nev && th_bsp_seq == seq0) {
            DWORD now = timeGetTime() - t_anchor;
            if (now >= ev[i].t) {
                if (!game_has_focus() && !game_wnd()) goto timeout;
                if (ev[i].kind == EV_KEY) {
                    unsigned idx = ev[i].code | ((ev[i].flags & F_EXT) ? 0x100 : 0);
                    send_key(ev[i].code, (ev[i].flags & F_EXT) != 0, (ev[i].flags & F_UP) != 0);
                    if (idx < 512) key_down[idx] = (ev[i].flags & F_UP) ? 0 : 1;
                } else if (ev[i].kind == EV_MBTN) {
                    send_btn(ev[i].code, (ev[i].flags & F_UP) != 0, 0);
                    if (ev[i].code < 8) btn_down[ev[i].code] = (ev[i].flags & F_UP) ? 0 : 1;
                } else if (ev[i].kind == EV_WHEEL) {
                    send_btn(0, 0, ev[i].data);
                }
                i++;
            } else Sleep(1);
        }
        if (th_posted) {
            char m[96]; unsigned k = put_s(m, 0, "test: game was not in front - input sent as window messages: ");
            k = put_i(m, k, (int)th_posted); m[k] = 0; sl_mark(m);
        }
        why = "door not reached with the recorded input";
        (void)last;
    }

    /* 3. transition: a different map is opened */
    {
        DWORD s = timeGetTime();
        for (;;) {
            if (th_bsp_seq != seq0) {
                unsigned k = 0; int same = 1;
                while (th_bsp_name[k] || th_first_map[k]) { if (th_bsp_name[k] != th_first_map[k]) { same = 0; break; } k++; }
                if (!same) break;
                seq0 = th_bsp_seq;                 /* same map re-opened: keep waiting */
            }
            if (timeGetTime() - s > (th_mode == TH_RECORD ? 600000u : 20000u + (nev ? ev[nev - 1].t : 0))) goto timeout;
            Sleep(1);
        }
    }
    t_door = th_bsp_t;
    if (th_mode == TH_REPLAY) release_all();
    sl_mark("test: door used, transition started");

    why = "transition did not finish";
    WAIT_UNTIL(cls_state() == 3, 120000);              /* server built the level, client reconnects */
    t_srv = timeGetTime();
    {
        DWORD seq1 = th_bsp_seq;
        DWORD s = timeGetTime();
        while (cls_state() != 4) {
            if (!t_climap && th_bsp_seq != seq1) t_climap = th_bsp_t;
            if (!t_sig1 && cl_signon() >= 1) t_sig1 = timeGetTime();
            if (timeGetTime() - s > 120000) goto timeout;
            Sleep(1);
        }
        if (!t_climap) t_climap = t_srv;
        if (!t_sig1) t_sig1 = timeGetTime();
    }
    t_in2 = timeGetTime();

    if (th_mode == TH_RECORD) {
        Sleep(500);
        rec_on = 0;
        if (rec_tid) PostThreadMessageA(rec_tid, 0x0012 /*WM_QUIT*/, 0, 0);
        if (save_recording()) {
            char m[160]; unsigned k = put_s(m, 0, "test: recording saved, events: ");
            k = put_i(m, k, (int)nev);
            if (rec_moves) { k = put_s(m, k, " (mouse movement not recorded: "); k = put_i(m, k, (int)rec_moves); k = put_s(m, k, " moves)"); }
            m[k] = 0; sl_mark(m);
            MessageBeep(0x40 /*MB_ICONINFORMATION*/);
        }
    }
    write_result("OK", (int)(t_anchor - t_load0), (int)(t_in2 - t_door), (int)(t_srv - t_door),
                 (int)(t_climap - t_srv), (int)(t_sig1 - t_climap), (int)(t_in2 - t_sig1));
    finish();
    return 0;

timeout:
    {
        char st[160]; unsigned k = put_s(st, 0, "FAIL ");
        k = put_s(st, k, why); st[k] = 0;
        write_result(st, -1, -1, -1, -1, -1, -1);
    }
    if (th_mode == TH_RECORD) { rec_on = 0; if (rec_tid) PostThreadMessageA(rec_tid, 0x0012, 0, 0); }
    finish();
    return 0;
}

/* Parse the command line; start the harness if a test switch is present. */
int th_start(const char *exedir)
{
    const char *cl = GetCommandLineA(), *p;
    unsigned n = 0;
    if (!cl) return 0;
    for (p = cl; *p; p++) {
        const char *a = "-fastfs_record", *b = "-fastfs_replay", *c = "-fastfs_label ";
        unsigned k;
        for (k = 0; a[k] && p[k] == a[k]; k++) {} if (!a[k]) th_mode = TH_RECORD;
        for (k = 0; b[k] && p[k] == b[k]; k++) {} if (!b[k]) th_mode = TH_REPLAY;
        for (k = 0; c[k] && p[k] == c[k]; k++) {}
        if (!c[k]) {
            const char *q = p + k; unsigned m = 0;
            while (*q && *q != ' ' && *q != '"' && m < sizeof th_label - 1) {
                char ch = *q++;
                th_label[m++] = (ch == ',') ? '_' : ch;
            }
            th_label[m] = 0;
        }
    }
    if (!th_mode) return 0;
    while (exedir[n] && n < 500) { th_dir[n] = exedir[n]; n++; }
    th_dir[n] = 0;
    ev = (Ev *)VirtualAlloc(0, MAXEV * sizeof(Ev), 0x3000, 0x04);
    if (!ev) { th_mode = 0; return 0; }
    {
        DWORD tid;
        HANDLE h = CreateThread(0, 0, harness, 0, 0, &tid);
        if (h) CloseHandle(h);
    }
    (void)GetCurrentThreadId;
    return 1;
}
