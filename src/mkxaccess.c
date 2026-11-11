/* MKX Access - screen reader support for Mortal Kombat X (Steam, PC).
   Hooks the game's UI text and focus functions and speaks through NVDA.

   Hotkeys (only while the game window is in front):
     F5  repeat last spoken text
     F6  read everything currently known on screen
     F7  toggle auto-reading of new text that is not tied to focus
     F8  mute / unmute MKX Access
     F9  say which version of MKX Access is running
     F10 turn damage numbers on or off (hit levels and combo counts stay)
     F11 next Steam friend to invite, Shift+F11 previous, Ctrl+F11 send the invite
         (on the invite screen also D-pad down / up and A on the controller)
         When a friend invites you: Ctrl+F11 or the controller's Back button accepts
   Everything is also written to MKXAccess.log next to the game exe. */
#include "win.h"

#define VERSION L"0.9.15"
#define FOCUS_DELAY 70   /* ms to wait after focus moves, for descriptions to arrive */
#define IMAGE_BASE 0x140000000ULL

/* ------------------------------------------------------------------ */
/* game addresses (from MK10.pdb, current Steam build)                 */
typedef struct {
    const char* name; u64 addr; int steal; BYTE expect[8]; void* hookfn; void** orig;
} HookDef;

typedef void   (*setText_t)(void*, const wchar*);
typedef DWORD  (*setTextHTML_t)(void*, const wchar*);
typedef BYTE   (*valSetText_t)(void*, const wchar*);
typedef BYTE   (*valSetTextA_t)(void*, const char*);
typedef void   (*gainFocus_t)(void*);
typedef void   (*setFocus_t)(void*, void*, int);
typedef void   (*drawCell_t)(void*, int, int, int);

static setText_t     o_setText;
static setTextHTML_t o_setTextHTML;
static valSetText_t  o_valSetText, o_valSetTextHTML;
static valSetTextA_t o_valSetTextA;
static gainFocus_t   o_gainFocus;
static setFocus_t    o_setFocus;
static drawCell_t    o_drawCell;
typedef DWORD  (*gotoLabel_t)(void*, const char*);
static gotoLabel_t   o_gotoAndPlay, o_gotoAndStop;

/* ------------------------------------------------------------------ */
/* event queue: game thread -> worker thread                           */
enum { EV_TEXT = 1, EV_HTML, EV_VTEXT, EV_VHTML, EV_FOCUS, EV_SETFOCUS, EV_GRID, EV_ANIM, EV_LABEL, EV_COMBOEND, EV_INVITE, EV_PAD, EV_LOBBYINVITE, EV_MENUPAD, EV_METERFULL, EV_FDIAG };
#define PATHMAX 192
#define TEXTMAX 512
typedef struct { int type; u64 id; DWORD t; int x, y; wchar path[PATHMAX]; wchar text[TEXTMAX]; } Event;
#define QSIZE 1024
static Event* g_q;
static volatile int g_qhead, g_qtail;
static CRITICAL_SECTION g_qlock;
static HANDLE g_qevent;
static volatile int g_ready;

static wchar g_dir[MAX_PATH];
static u64 g_base;

static u64 g_imageEnd;   /* base + SizeOfImage: the game's own code and constant data */
static volatile LONG g_statCalls, g_statQueued, g_statDeduped;

static int readable(const void* p, SIZE_T n)
{
    MEMORY_BASIC_INFORMATION mbi;
    if ((u64)p < 0x10000 || (u64)p > 0x7FFFFFFFFFFFULL) return 0;
    if ((u64)p >= g_base && (u64)p + n <= g_imageEnd) return 1;   /* inside MK10.exe: always mapped */
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    if (mbi.Protect & 0x101) return 0; /* NOACCESS or GUARD */
    if (!(mbi.Protect & 0xEE)) return 0; /* any readable protection */
    return (u64)p + n <= (u64)mbi.BaseAddress + mbi.RegionSize;
}

static void path_of(void* elem, wchar* out)
{
    out[0] = 0;
    if (!elem || !readable((BYTE*)elem + 0x18, 16)) return;
    const char* p = *(const char**)((BYTE*)elem + 0x18);
    int n = *(int*)((BYTE*)elem + 0x20);
    if (!p || n <= 0 || n > 4096 || !readable(p, n)) return;
    int i = 0;
    for (; i < PATHMAX - 1 && p[i]; i++) out[i] = (BYTE)p[i];
    out[i] = 0;
}

static Event* q_begin(void)
{
    if (!g_ready) return 0;
    EnterCriticalSection(&g_qlock);
    int next = (g_qhead + 1) % QSIZE;
    if (next == g_qtail) { LeaveCriticalSection(&g_qlock); return 0; } /* full: drop */
    Event* e = &g_q[g_qhead];
    e->t = GetTickCount(); e->path[0] = 0; e->text[0] = 0; e->id = 0; e->x = e->y = 0;
    g_statQueued++;
    return e;
}
static void q_end(void)
{
    g_qhead = (g_qhead + 1) % QSIZE;
    LeaveCriticalSection(&g_qlock);
    SetEvent(g_qevent);
}

/* ------------------------------------------------------------------ */
/* hooks (run on the game thread: keep them fast)                      */
/* last text per label, so re-setting the same text costs almost nothing.
   Keyed by the label's path, NOT by the UITextItem pointer: the game reuses one
   temporary UITextItem for many labels in a row, so "OFF" then "OFF" on two
   different settings came from the same pointer and the second was lost. */
typedef struct { DWORD key, hash, t; } LastSet;
static LastSet g_lastSet[2048];
static int same_as_last(void* self, const wchar* s, int salt, wchar* pathOut)
{
    path_of(self, pathOut);                      /* needed for the event anyway */
    DWORD k = 2166136261u;
    for (int i = 0; pathOut[i]; i++) { k ^= pathOut[i]; k *= 16777619u; }
    if (!pathOut[0]) k ^= (DWORD)((u64)self >> 4);
    DWORD h = 2166136261u ^ (DWORD)salt;
    if (s) for (int i = 0; s[i] && i < TEXTMAX; i++) { h ^= s[i]; h *= 16777619u; }
    LastSet* c = &g_lastSet[(k ^ (k >> 11)) & 2047];
    DWORD now = GetTickCount();
    g_statCalls++;
    /* only drop rapid repeats: a page that re-sets the same text a second later
       (being rebuilt) still has to be seen, so the mod knows the text is current */
    if (c->key == k && c->hash == h && now - c->t < 1000) { g_statDeduped++; return 1; }
    c->key = k; c->hash = h; c->t = now;
    return 0;
}

/* Practice mode's input history (ButtonLogs) re-sets its cells all the time and is
   never read: drop it here, before it costs a queue slot and a log line. It was about
   three quarters of all text the mod handled in a practice session. */
static int hot_ignored(const wchar* p)
{
    for (; *p; p++)
        if (p[0] == 'B' && p[1] == 'u' && p[2] == 't' && p[3] == 't' && p[4] == 'o' && p[5] == 'n' &&
            p[6] == 'L' && p[7] == 'o' && p[8] == 'g' && p[9] == 's') return 1;
    return 0;
}
static void hk_setText(void* self, const wchar* s)
{
    wchar path[PATHMAX];
    if (same_as_last(self, s, 1, path) || hot_ignored(path)) { o_setText(self, s); return; }
    Event* e = q_begin();
    if (e) { e->type = EV_TEXT; e->id = (u64)self; wcpy(e->path, path, PATHMAX); if (s) wcpy(e->text, s, TEXTMAX); q_end(); }
    o_setText(self, s);
}
static DWORD hk_setTextHTML(void* self, const wchar* s)
{
    wchar path[PATHMAX];
    if (same_as_last(self, s, 2, path) || hot_ignored(path)) return o_setTextHTML(self, s);
    Event* e = q_begin();
    if (e) { e->type = EV_HTML; e->id = (u64)self; wcpy(e->path, path, PATHMAX); if (s) wcpy(e->text, s, TEXTMAX); q_end(); }
    return o_setTextHTML(self, s);
}
static BYTE hk_valSetText(void* v, const wchar* s)
{
    Event* e = q_begin();
    if (e) { e->type = EV_VTEXT; e->id = readable((BYTE*)v + 0x20, 8) ? *(u64*)((BYTE*)v + 0x20) : 0; if (s) wcpy(e->text, s, TEXTMAX); q_end(); }
    return o_valSetText(v, s);
}
static BYTE hk_valSetTextHTML(void* v, const wchar* s)
{
    Event* e = q_begin();
    if (e) { e->type = EV_VHTML; e->id = readable((BYTE*)v + 0x20, 8) ? *(u64*)((BYTE*)v + 0x20) : 0; if (s) wcpy(e->text, s, TEXTMAX); q_end(); }
    return o_valSetTextHTML(v, s);
}
static BYTE hk_valSetTextA(void* v, const char* s)
{
    Event* e = q_begin();
    if (e) {
        e->type = EV_VTEXT; e->id = readable((BYTE*)v + 0x20, 8) ? *(u64*)((BYTE*)v + 0x20) : 0;
        if (s) { int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, e->text, TEXTMAX - 1); e->text[n > 0 ? n : 0] = 0; e->text[TEXTMAX-1] = 0; }
        q_end();
    }
    return o_valSetTextA(v, s);
}
static void hk_gainFocus(void* self)
{
    Event* e = q_begin();
    if (e) { e->type = EV_FOCUS; e->id = (u64)self; path_of(self, e->path); q_end(); }
    o_gainFocus(self);
}
static void hk_setFocus(void* self, void* target, int a)
{
    Event* e = q_begin();
    if (e) { e->type = EV_SETFOCUS; e->id = (u64)target; e->x = a; path_of(target, e->path); path_of(self, e->text); q_end(); }
    o_setFocus(self, target, a);
}
typedef void* (*getCell_t)(void*, int, int);
static getCell_t g_getCell;   /* UIGridSelectWidget::GetCellElement<UIFocusElementWidget>(x, y) */
static void hk_drawCell(void* self, int cursor, int x, int y)
{
    o_drawCell(self, cursor, x, y);
    void* cell = g_getCell ? g_getCell(self, x, y) : 0;
    Event* e = q_begin();
    if (e) { e->type = EV_GRID; e->id = (u64)cell; e->x = x; e->y = y; path_of(cell, e->path); q_end(); }
}

/* every menu type plays a "GainFocus" animation on the item it highlights */
static int is_focus_label(const char* l)
{
    /* GainFocus, SnapToGainFocus, gainFocus... but not LoseFocus */
    for (const char* p = l; *p; p++) {
        if ((p[0] == 'G' || p[0] == 'g') && p[1] == 'a' && p[2] == 'i' && p[3] == 'n' &&
            (p[4] == 'F' || p[4] == 'f') && p[5] == 'o' && p[6] == 'c' && p[7] == 'u' && p[8] == 's') return 1;
    }
    return 0;
}
static const char* g_seenLabels[512];
static void anim_event(void* self, const char* label)
{
    if (!label || !readable(label, 1)) return;
    /* the combo counter plays "Finish" when a combo is really over */
    if (label[0] == 'F' && label[1] == 'i' && label[2] == 'n' && label[3] == 'i' && label[4] == 's' && label[5] == 'h' && !label[6]) {
        Event* e = q_begin();
        if (e) { e->type = EV_COMBOEND; e->id = (u64)self; path_of(self, e->path); q_end(); }
    }
    if (is_focus_label(label)) {
        Event* e = q_begin();
        if (e) { e->type = EV_ANIM; e->id = (u64)self; path_of(self, e->path); q_end(); }
        return;
    }
    /* log each distinct label once, to learn what other animations exist */
    unsigned h = (unsigned)(((u64)label >> 3) * 2654435761u) & 511;
    for (int k = 0; k < 8; k++) {
        int i = (h + k) & 511;
        if (g_seenLabels[i] == label) return;
        if (!g_seenLabels[i]) {
            g_seenLabels[i] = label;
            Event* e = q_begin();
            if (e) { e->type = EV_LABEL; path_of(self, e->path); for (int j = 0; j < 60 && label[j]; j++) { e->text[j] = (BYTE)label[j]; e->text[j+1] = 0; } q_end(); }
            return;
        }
    }
}
static void steam_register_callbacks(void);
static DWORD hk_gotoAndPlay(void* self, const char* label) { steam_register_callbacks(); anim_event(self, label); return o_gotoAndPlay(self, label); }
static DWORD hk_gotoAndStop(void* self, const char* label) { anim_event(self, label); return o_gotoAndStop(self, label); }

/* controller: while the invite screen is up, D-pad down/up and A choose a friend
   and send the invite. Those buttons are hidden from the game then, so A can't
   also press something in the game; B still reaches the game (cancel). */
typedef struct { WORD wButtons; BYTE lt, rt; short lx, ly, rx, ry; } XPad;
typedef struct { DWORD packet; XPad pad; } XState;
typedef DWORD (WINAPI *XInputGetState_t)(DWORD, XState*);
static XInputGetState_t o_XInputGetState;
static volatile int g_padMode;        /* set by the worker while the invite screen is up */
static volatile int g_acceptMode;     /* set by the worker while a received invite waits */
static void steam_register_callbacks(void);
static WORD g_padPrev[4];
#define PAD_UP 0x0001
#define PAD_DOWN 0x0002
#define PAD_A 0x1000
#define PAD_BACK 0x0020
#define PAD_LEFT 0x0004
#define PAD_RIGHT 0x0008
#define PAD_START 0x0010
#define PAD_Y 0x8000
#define PAD_B 0x2000
/* accessibility menu (F3, or hold Back and press Y): while it is open the game
   gets no controller input and no key presses, so nothing moves underneath it */
static volatile int g_menuOpen;
static WORD g_padHide[4];   /* buttons still held from the menu: hidden until released */
static int g_padWasMenu[4];
static DWORD WINAPI hk_XInputGetState(DWORD idx, XState* st)
{
    DWORD r = o_XInputGetState(idx, st);
    if (r != 0 || !st || idx >= 4) return r;
    steam_register_callbacks();   /* game thread: safe place to register with Steam */
    WORD b = st->pad.wButtons, fresh = b & ~g_padPrev[idx];
    g_padPrev[idx] = b;
    /* Back + Y opens the menu (Back + Start was also caught by Steam for some players) */
    if (!g_menuOpen && (b & PAD_BACK) && (fresh & PAD_Y)) {
        g_menuOpen = 1;
        Event* e = q_begin(); if (e) { e->type = EV_MENUPAD; e->x = PAD_BACK | PAD_Y; q_end(); }
    }
    if (g_menuOpen) {
        WORD mine = fresh & (PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT | PAD_A | PAD_B | PAD_START | PAD_Y);
        if (mine && g_padWasMenu[idx]) { Event* e = q_begin(); if (e) { e->type = EV_MENUPAD; e->x = mine | (b & PAD_BACK); q_end(); } }
        g_padWasMenu[idx] = 1;
        memset(&st->pad, 0, sizeof(st->pad));
        return r;
    }
    if (g_padWasMenu[idx]) { g_padWasMenu[idx] = 0; g_padHide[idx] = b; }   /* menu just closed */
    if (g_padHide[idx]) { g_padHide[idx] &= b; st->pad.wButtons &= (WORD)~g_padHide[idx]; b = st->pad.wButtons; }
    if (g_acceptMode && !g_padMode) {
        if (fresh & PAD_BACK) { Event* e = q_begin(); if (e) { e->type = EV_PAD; e->x = PAD_BACK; q_end(); } }
        st->pad.wButtons &= (WORD)~PAD_BACK;
    }
    if (g_padMode) {
        WORD mine = fresh & (PAD_UP | PAD_DOWN | PAD_A);
        if (mine) { Event* e = q_begin(); if (e) { e->type = EV_PAD; e->x = mine; q_end(); } }
        st->pad.wButtons &= (WORD)~(PAD_UP | PAD_DOWN | PAD_A);
    }
    return r;
}

/* point the game's import of XInputGetState at our function */
static int patch_import(const char* dll, const char* fn, void* hook, void** orig)
{
    BYTE* base = (BYTE*)g_base;
    DWORD lfanew = *(DWORD*)(base + 0x3C);
    DWORD impRva = *(DWORD*)(base + lfanew + 0x90);   /* data directory 1: imports (PE32+) */
    if (!impRva) return 0;
    for (DWORD* d = (DWORD*)(base + impRva); d[3]; d += 5) {   /* IMAGE_IMPORT_DESCRIPTOR */
        const char* name = (const char*)(base + d[3]);
        int k = 0; while (dll[k]) { char a = name[k], c = dll[k]; if (a >= 'a' && a <= 'z') a -= 32; if (c >= 'a' && c <= 'z') c -= 32; if (a != c) break; k++; }
        if (dll[k] || name[k]) continue;
        u64* names = (u64*)(base + (d[0] ? d[0] : d[4]));
        u64* iat = (u64*)(base + d[4]);
        for (int i = 0; names[i]; i++) {
            if (names[i] >> 63) continue;   /* by ordinal */
            const char* nm = (const char*)(base + (DWORD)names[i] + 2);
            int j = 0; while (fn[j] && nm[j] == fn[j]) j++;
            if (fn[j] || nm[j]) continue;
            DWORD old;
            if (!VirtualProtect(&iat[i], 8, 0x04, &old)) return 0;
            *orig = (void*)iat[i];
            iat[i] = (u64)hook;
            VirtualProtect(&iat[i], 8, old, &old);
            return 1;
        }
    }
    return 0;
}
static int g_padHooked;

/* keyboard: the game gets its keys from raw input through PCInput::HandleKeyboardInput
   (this, device, key, down). While the menu is open key presses are kept from the game;
   key releases still go through so no key is left stuck down. */
typedef void (*kbd_t)(void*, void*, WORD, BYTE);
static kbd_t o_kbd;
static void hk_kbd(void* self, void* dev, WORD key, BYTE down)
{
    if (g_menuOpen && down) return;
    o_kbd(self, dev, key, down);
}

/* PLAYER_INFO::SetSuperMeter(this, float): every meter gain goes through it. The computer
   can fire its X-ray in the same moment its meter fills, too fast for the 50 ms check,
   so the moment the meter reaches full is caught here and told to the worker. */
typedef void (*setMeter_t)(void*, float);
static setMeter_t o_setMeter;
static void hk_setMeter(void* self, float v)
{
    if (!self || !readable((BYTE*)self + 0x28C, 4)) { o_setMeter(self, v); return; }
    float before = *(float*)((BYTE*)self + 0x28C);
    o_setMeter(self, v);
    float after = *(float*)((BYTE*)self + 0x28C);
    if (before < 0.999f && after >= 0.999f) { Event* e = q_begin(); if (e) { e->type = EV_METERFULL; e->id = (u64)self; q_end(); } }
}

/* frame data research (test builds): PLAYER_INFO::Tick runs once per game tick for each
   player (from FG_GAME_INFO::Tick). After it, the fighter's attack and reaction state is
   compared with the last tick and every change is logged with the game tick number
   (GAME_INFO +0xD0, GetGameTickCounter). FGCharacterObj: +0xC78 GetAttackState, +0x1010
   GetLastStartAttackVMTick, +0x1054 GetFirstHitDetected, +0xA2C bit 0x400 / +0xF0C
   IsAlreadyReacting, +0xA38 life. */
typedef void (*playerTick_t)(void*);
static playerTick_t o_playerTick;
typedef struct { int atk, start, hit, react, f0c, a2c, a28, f20, bo, bos, blk, c790; float life; int valid; } FdState;
static FdState g_fd[2];
static void fd_num(wchar* d, int* n, long long v)
{
    if (v < 0) { d[(*n)++] = '-'; v = -v; }
    wchar t[24]; int k = 0; do { t[k++] = (wchar)('0' + v % 10); v /= 10; } while (v && k < 23);
    while (k) d[(*n)++] = t[--k];
}
static void fd_put(wchar* d, int* n, const char* s) { while (*s) d[(*n)++] = (wchar)*s++; }
static void hk_playerTick(void* info)
{
    o_playerTick(info);
    BYTE* pi = (BYTE*)info;
    u64 gg = *(u64*)(g_base + (0x143316848ULL - IMAGE_BASE));
    u64 fg = *(u64*)(g_base + (0x1430DBA00ULL - IMAGE_BASE));
    if (!pi || !gg || !fg || !readable(pi + 0x20, 16)) return;
    int p = *(u64*)(fg + 0x1968) == (u64)pi ? 0 : *(u64*)(fg + 0x1970) == (u64)pi ? 1 : -1;
    if (p < 0) return;
    BYTE* obj = *(BYTE**)(pi + 0x20);
    int h = *(int*)(pi + 0x28);
    if (!obj || h == -1 || !readable(obj, 0x1060) || *(DWORD*)(obj + 8) != (DWORD)h) { g_fd[p].valid = 0; return; }
    FdState n;
    n.atk = *(int*)(obj + 0xC78); n.start = *(int*)(obj + 0x1010); n.hit = *(int*)(obj + 0x1054);
    n.a2c = *(int*)(obj + 0xA2C); n.f0c = *(int*)(obj + 0xF0C);
    n.react = (n.a2c & 0x400) || n.f0c; n.life = *(float*)(obj + 0xA38); n.valid = 1;
    if (!readable(obj, 0x1100)) return;
    n.a28 = *(int*)(obj + 0xA28); n.c790 = *(int*)(obj + 0x790);
    n.f20 = 0; for (int i = 0; i < 4; i++) n.f20 |= *(int*)(obj + 0xF20 + 8 * i);
    n.bo = *(int*)(obj + 0x1048); n.bos = *(int*)(obj + 0x104C);
    /* FGCharacterObj::CanIBeInABlockingState without calling it (it can touch the
       online desync tracker): no "can't block" flags, not in a few states, alive */
    n.blk = !(n.f20 & ((1 << 0x13) | 0x40)) && n.a2c != 0x1201 && n.a2c != 0x2013 && !(n.a28 & 8) && n.a2c != 0x4605 && !n.c790 &&
            !*(int*)(obj + 0x1098) && !*(int*)(obj + 0x109C) && !*(int*)(obj + 0x10C8) && !*(int*)(obj + 0x10F8) && n.life > 0;
    FdState* o = &g_fd[p];
    if (o->valid && n.atk == o->atk && n.start == o->start && n.hit == o->hit && n.react == o->react && n.f0c == o->f0c && n.life == o->life &&
        n.a2c == o->a2c && n.f20 == o->f20 && n.bo == o->bo && n.bos == o->bos && n.blk == o->blk && n.a28 == o->a28 && n.c790 == o->c790) return;
    *o = n;
    Event* e = q_begin(); if (!e) return;
    e->type = EV_FDIAG;
    int k = 0; wchar* d = e->text;
    fd_put(d, &k, p ? "P2 tick " : "P1 tick "); fd_num(d, &k, *(int*)(gg + 0xD0));
    fd_put(d, &k, " atk "); fd_num(d, &k, n.atk);
    fd_put(d, &k, " start "); fd_num(d, &k, n.start);
    fd_put(d, &k, " hit "); fd_num(d, &k, n.hit);
    fd_put(d, &k, " react "); fd_num(d, &k, n.react);
    fd_put(d, &k, " f0c "); fd_num(d, &k, n.f0c);
    fd_put(d, &k, " a2c "); fd_num(d, &k, n.a2c);
    fd_put(d, &k, " life "); fd_num(d, &k, (long long)(n.life * 1000));
    fd_put(d, &k, " blk "); fd_num(d, &k, n.blk);
    fd_put(d, &k, " f20 "); fd_num(d, &k, (unsigned)n.f20);
    fd_put(d, &k, " a28 "); fd_num(d, &k, n.a28);
    fd_put(d, &k, " bo "); fd_num(d, &k, n.bo);
    fd_put(d, &k, " bos "); fd_num(d, &k, n.bos);
    fd_put(d, &k, " c790 "); fd_num(d, &k, n.c790);
    d[k] = 0;
    q_end();
}

static HookDef g_hooks[] = {
    { "UIElement::gotoAndPlay",        0x140C36B20, 7, {0x48,0x83,0xEC,0x28,0x4C,0x8B,0xC2}, hk_gotoAndPlay,    (void**)&o_gotoAndPlay },
    { "UIElement::gotoAndStop",        0x140C36BA0, 7, {0x48,0x83,0xEC,0x28,0x4C,0x8B,0xC2}, hk_gotoAndStop,    (void**)&o_gotoAndStop },
    { "UITextItem::setText(w)",        0x140C382D0, 5, {0x48,0x8B,0xC4,0x41,0x56},           hk_setText,        (void**)&o_setText },
    { "UITextItem::setTextHTML(w)",    0x140C385E0, 6, {0x41,0x56,0x48,0x83,0xEC,0x40},      hk_setTextHTML,    (void**)&o_setTextHTML },
    { "GFx::Value::SetText(w)",        0x14051F300, 7, {0x4C,0x8B,0xD1,0x48,0x8B,0x49,0x10}, hk_valSetText,     (void**)&o_valSetText },
    { "GFx::Value::SetText(a)",        0x14081E7F0, 7, {0x4C,0x8B,0xD1,0x48,0x8B,0x49,0x10}, hk_valSetTextA,    (void**)&o_valSetTextA },
    { "GFx::Value::SetTextHTML(w)",    0x140C31DC0, 7, {0x4C,0x8B,0xD1,0x48,0x8B,0x49,0x10}, hk_valSetTextHTML, (void**)&o_valSetTextHTML },
    { "UIFocusElementWidget::GainFocus",0x1405B9590,6, {0x40,0x53,0x48,0x83,0xEC,0x20},      hk_gainFocus,      (void**)&o_gainFocus },
    { "UIElement::setFocus",           0x140C37B30, 5, {0x48,0x89,0x5C,0x24,0x08},           hk_setFocus,       (void**)&o_setFocus },
    { "UIGridSelectWidget::DrawCellGainFocus",0x1405B7550,6,{0x40,0x53,0x48,0x83,0xEC,0x20}, hk_drawCell,       (void**)&o_drawCell },
    { "PCInput::HandleKeyboardInput",  0x140813550, 5, {0x48,0x89,0x5C,0x24,0x08},           hk_kbd,            (void**)&o_kbd },
    { "PLAYER_INFO::SetSuperMeter",    0x14055EC40, 6, {0x40,0x53,0x48,0x83,0xEC,0x40},      hk_setMeter,       (void**)&o_setMeter },
    { "PLAYER_INFO::Tick",             0x140563D70, 5, {0x48,0x89,0x5C,0x24,0x08},           hk_playerTick,     (void**)&o_playerTick },
};
#define NHOOKS (int)(sizeof(g_hooks)/sizeof(g_hooks[0]))
static int g_hookOk[NHOOKS];

/* ------------------------------------------------------------------ */
/* hook installer: 5-byte jmp to a nearby relay, trampoline for orig   */
static BYTE* alloc_near(u64 target, SIZE_T size)
{
    u64 gran = 0x10000;
    u64 start = (target & ~(gran - 1));
    for (u64 a = start - gran; a > start - 0x70000000ULL; a -= gran) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void*)a, &mbi, sizeof(mbi))) continue;
        if (mbi.State != MEM_FREE) continue;
        void* p = VirtualAlloc((void*)a, size, MEM_COMMIT | MEM_RESERVE, 0x04 /* PAGE_READWRITE */);
        if (p) return (BYTE*)p;
    }
    return 0;
}

static void put_absjmp(BYTE* p, u64 dest)
{
    p[0] = 0xFF; p[1] = 0x25; p[2] = p[3] = p[4] = p[5] = 0;
    memcpy(p + 6, &dest, 8);
}

static void install_hooks(void)
{
    BYTE* mem = alloc_near(g_base, 0x1000);
    if (!mem) return;
    for (int i = 0; i < NHOOKS; i++) {
        HookDef* h = &g_hooks[i];
        BYTE* target = (BYTE*)(g_base + (h->addr - IMAGE_BASE));
        int ok = 1;
        for (int b = 0; b < h->steal; b++) if (target[b] != h->expect[b]) ok = 0;
        if (!ok) { g_hookOk[i] = -1; continue; }
        BYTE* tramp = mem + i * 64;          /* stolen bytes + jmp back */
        BYTE* relay = tramp + 40;            /* jmp to our hook */
        memcpy(tramp, target, h->steal);
        put_absjmp(tramp + h->steal, (u64)(target + h->steal));
        put_absjmp(relay, (u64)h->hookfn);
        *h->orig = tramp;
        DWORD old;
        if (!VirtualProtect(target, 16, PAGE_EXECUTE_READWRITE, &old)) { g_hookOk[i] = -2; continue; }
        int rel = (int)((i64)relay - (i64)(target + 5));
        target[0] = 0xE9; memcpy(target + 1, &rel, 4);
        for (int b = 5; b < h->steal; b++) target[b] = 0x90;
        VirtualProtect(target, 16, old, &old);
        FlushInstructionCache(GetCurrentProcess(), target, 16);
        g_hookOk[i] = 1;
    }
    /* trampolines are written: make them run-only, never writable and runnable at once */
    DWORD old;
    VirtualProtect(mem, 0x1000, 0x20 /* PAGE_EXECUTE_READ */, &old);
    FlushInstructionCache(GetCurrentProcess(), mem, 0x1000);
}

/* ------------------------------------------------------------------ */
/* logging (worker thread only)                                        */
static HANDLE g_log = INVALID_HANDLE_VALUE;
static DWORD g_logBytes, g_t0;
static void log_w(const wchar* s)
{
    if (g_log == INVALID_HANDLE_VALUE || g_logBytes > 20u * 1024 * 1024) return;
    char buf[4096];
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, buf, sizeof(buf) - 2, 0, 0);
    if (n <= 0) return;
    n--; buf[n++] = '\r'; buf[n++] = '\n';
    DWORD w; WriteFile(g_log, buf, n, &w, 0); g_logBytes += w;
}
static void utoa_w(u64 v, wchar* out, int base, int minw)
{
    wchar tmp[24]; int n = 0;
    do { int d = (int)(v % base); tmp[n++] = (wchar)(d < 10 ? '0' + d : 'a' + d - 10); v /= base; } while (v && n < 22);
    while (n < minw) tmp[n++] = '0';
    int i = 0; while (n) out[i++] = tmp[--n]; out[i] = 0;
}
static void logf3(const wchar* tag, const wchar* a, const wchar* b)
{
    wchar line[1024], num[24];
    utoa_w(GetTickCount() - g_t0, num, 10, 7);
    wcpy(line, num, 1024); wcat(line, (const wchar*)L" ", 1024); wcat(line, tag, 1024);
    if (a) { wcat(line, (const wchar*)L" [", 1024); wcat(line, a, 1024); wcat(line, (const wchar*)L"]", 1024); }
    if (b) { wcat(line, (const wchar*)L" ", 1024); wcat(line, b, 1024); }
    log_w(line);
}

/* ------------------------------------------------------------------ */
/* speech                                                              */
typedef DWORD (WINAPI *nvSpeak_t)(const wchar*);
typedef DWORD (WINAPI *nvVoid_t)(void);
static nvSpeak_t nv_speak; static nvVoid_t nv_cancel, nv_test;
static int g_muted, g_autoRead = 1;
static wchar g_last[2048];
static DWORD g_lastTime;

static void speak(const wchar* s, int interrupt)
{
    if (!s || !s[0]) return;
    logf3((const wchar*)L"SPEAK", 0, s);
    wcpy(g_last, s, 2048); g_lastTime = GetTickCount();
    if (g_muted || !nv_speak) return;
    if (interrupt && nv_cancel) nv_cancel();
    nv_speak(s);
}
static void speak_force(const wchar* s)   /* ignores mute (for status messages) */
{
    logf3((const wchar*)L"SPEAK", 0, s);
    if (!nv_speak) return;
    if (nv_cancel) nv_cancel();
    nv_speak(s);
}

/* ------------------------------------------------------------------ */
/* text cleanup                                                        */
static int is_space(wchar c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == 0xA0; }

static int starts_ci(const wchar* s, const char* p)
{
    for (; *p; p++, s++) { wchar a = *s, b = (wchar)*p; if (a >= 'A' && a <= 'Z') a += 32; if (b >= 'A' && b <= 'Z') b += 32; if (a != b) return 0; }
    return 1;
}

/* strips <html tags>, decodes common entities, collapses whitespace */
/* some callers hand over 8-bit text where 16-bit is expected; every 16-bit
   unit then holds two printable ASCII bytes (U+4544 = "DE") */
static void fix_packed_ansi(const wchar* in, wchar* out, int max)
{
    int n = wlen(in), packed = n > 0;
    for (int i = 0; i < n && packed; i++) {
        wchar c = in[i]; BYTE lo = (BYTE)(c & 0xFF), hi = (BYTE)(c >> 8);
        if (!(lo >= 0x20 && lo < 0x7F && ((hi >= 0x20 && hi < 0x7F) || (hi == 0 && i == n - 1)))) packed = 0;
    }
    if (!packed || n == 1) { wcpy(out, in, max); return; }
    int o = 0;
    for (int i = 0; i < n && o < max - 2; i++) {
        out[o++] = in[i] & 0xFF;
        if (in[i] >> 8) out[o++] = in[i] >> 8;
    }
    out[o] = 0;
}

static void clean_text(const wchar* in0, wchar* out, int max)
{
    static wchar fixed[TEXTMAX];
    fix_packed_ansi(in0, fixed, TEXTMAX);
    const wchar* in = fixed;
    int o = 0, space = 1;
    for (int i = 0; in[i] && o < max - 1; i++) {
        wchar c = in[i];
        if (c == 0xB7 || c == 0x2022) c = ' ';   /* a bullet in front of move notes */
        if (c == '<') {
            /* <br> and </p> count as a space */
            while (in[i] && in[i] != '>') i++;
            if (!in[i]) break;
            c = ' ';
        } else if (c == '&') {
            if (starts_ci(in + i, "&amp;")) { c = '&'; i += 4; }
            else if (starts_ci(in + i, "&lt;")) { c = '<'; i += 3; }
            else if (starts_ci(in + i, "&gt;")) { c = '>'; i += 3; }
            else if (starts_ci(in + i, "&quot;")) { c = '"'; i += 5; }
            else if (starts_ci(in + i, "&apos;")) { c = '\''; i += 5; }
            else if (starts_ci(in + i, "&nbsp;")) { c = ' '; i += 5; }
        }
        if (is_space(c)) { if (!space) { out[o++] = ' '; space = 1; } continue; }
        out[o++] = c; space = 0;
    }
    while (o > 0 && out[o - 1] == ' ') o--;
    out[o] = 0;
}

/* true if text has no letters (timers, counters, scores) */
static int no_letters(const wchar* s)
{
    for (; *s; s++) { wchar c = *s; if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c >= 0xC0) return 0; }
    return 1;
}

/* turns an internal element path like "_root.mc_MainMenu.btn_StoryMode" into "Story Mode" */
static void friendly_name(const wchar* path, wchar* out, int max)
{
    const wchar* last = path;
    for (const wchar* p = path; *p; p++) if (*p == '.' || *p == '/') last = p + 1;
    static const char* prefixes[] = { "mc_", "btn_", "txt_", "m_", "mc", "btn", "item_", "ui_" };
    int changed = 1;
    while (changed) {
        changed = 0;
        for (int k = 0; k < 8; k++) {
            int L = 0; while (prefixes[k][L]) L++;
            if (starts_ci(last, prefixes[k]) && last[L] && (k < 4 || k >= 6 || (last[L] >= 'A' && last[L] <= 'Z')))
            { last += L; changed = 1; }
        }
    }
    int o = 0;
    for (int i = 0; last[i] && o < max - 2; i++) {
        wchar c = last[i];
        if (c == '_') c = ' ';
        if (i > 0 && c >= 'A' && c <= 'Z' && last[i - 1] >= 'a' && last[i - 1] <= 'z') out[o++] = ' ';
        out[o++] = c;
    }
    out[o] = 0;
}

/* ------------------------------------------------------------------ */
/* button/direction codes -> words, e.g. "[Down][Away][FK]" -> "down back front kick" */
static const char* g_tok[][2] = {
    {"A","A"},{"B","B"},{"X","X"},{"Y","Y"},
    {"LSH","left bumper"},{"RSH","right bumper"},{"LB","left bumper"},{"RB","right bumper"},
    {"LT","left trigger"},{"RT","right trigger"},{"LTR","left trigger"},{"RTR","right trigger"},
    {"LTRIG","left trigger"},{"RTRIG","right trigger"},{"LTRIGGER","left trigger"},{"RTRIGGER","right trigger"},
    {"LS","left stick"},{"RS","right stick"},{"L3","left stick click"},{"R3","right stick click"},
    {"START","start"},{"BACK","back button"},{"SELECT","select"},{"MENU","menu"},{"VIEW","view"},
    {"U","up"},{"D","down"},{"L","left"},{"R","right"},
    {"Up","up"},{"Down","down"},{"Left","left"},{"Right","right"},
    {"Away","back"},{"Towards","forward"},{"Toward","forward"},
    {"AwayUp","up back"},{"TowardsUp","up forward"},{"AwayDown","down back"},{"TowardsDown","down forward"},
    {"FP","front punch"},{"BP","back punch"},{"FK","front kick"},{"BK","back kick"},
    {"BLK","block"},{"BL","block"},{"INT","interact"},{"SS","stance switch"},{"FLIP","flip stance"},
    {"Key_Ent","Enter"},{"Key_Enter","Enter"},{"Key_Esc","Escape"},{"Key_Escape","Escape"},{"Key_NM_5","numpad 5"},
    {"Key_Bksp","backspace"},{"Key_Back","backspace"},{"Key_Space","space"},{"Key_Tab","tab"},
    {"Force_A","A"},{"Force_B","B"},{"Force_X","X"},{"Force_Y","Y"},
    {"DPAD","d-pad"},{"DPADUP","d-pad up"},{"DPADDOWN","d-pad down"},{"DPADLEFT","d-pad left"},{"DPADRIGHT","d-pad right"},
};
static int tok_eq(const wchar* s, int n, const char* t)
{
    int i = 0;
    for (; i < n && t[i]; i++) { wchar a = s[i], b = (wchar)t[i]; if (a >= 'a' && a <= 'z') a -= 32; if (b >= 'a' && b <= 'z') b -= 32; if (a != b) return 0; }
    return i == n && !t[i];
}
static void speakify(const wchar* in, wchar* out, int max)
{
    int o = 0;
    for (int i = 0; in[i] && o < max - 40; ) {
        if (in[i] == '?' && in[i + 1] == '?' && in[i + 2] == '?') {   /* "????????": not unlocked yet */
            while (in[i] == '?') i++;
            for (const char* w = "hidden"; *w; w++) out[o++] = (wchar)*w;
            continue;
        }
        if (in[i] == '[') {
            int j = i + 1; while (in[j] && in[j] != ']' && j - i < 24) j++;
            if (in[j] == ']') {
                const wchar* t = in + i + 1; int n = j - i - 1;
                if (o > 0 && out[o - 1] != ' ') out[o++] = ' ';
                int found = 0;
                for (int k = 0; k < (int)(sizeof(g_tok) / sizeof(g_tok[0])); k++)
                    if (tok_eq(t, n, g_tok[k][0])) { for (const char* w = g_tok[k][1]; *w; w++) out[o++] = (wchar)*w; found = 1; break; }
                if (!found) for (int k = 0; k < n; k++) out[o++] = (t[k] == '_') ? ' ' : t[k];
                i = j + 1;
                if (in[i] && in[i] != ' ' && in[i] != ',' && in[i] != '.') out[o++] = ' ';
                continue;
            }
        }
        out[o++] = in[i++];
    }
    out[o] = 0;
}

/* screen parts that are never auto-read (news panel, input history, debug text) */
static int contains_ci(const wchar* s, const char* sub)
{
    for (; *s; s++) if (starts_ci(s, sub)) return 1;
    return 0;
}
static int contains_w(const wchar* hay, const wchar* needle)
{
    static wchar tmp[4096];
    speakify(needle, tmp, 4096);
    int n = wlen(tmp); if (!n) return 0;
    for (; *hay; hay++) { int i = 0; while (i < n && hay[i] == tmp[i]) i++; if (i == n) return 1; }
    return 0;
}
/* a screen/section title, not an item label like option3.ToggleElement.Title */
static int is_titleish(const wchar* p)
{
    if (contains_ci(p, "option") || contains_ci(p, "Cell_") || contains_ci(p, "lineItem") || contains_ci(p, "Toggle")) return 0;
    return contains_ci(p, "title") || contains_ci(p, "header") || contains_ci(p, "CharText") ||
           contains_ci(p, "message") || contains_ci(p, "msgBox") || contains_ci(p, "popup") || contains_ci(p, "dialog");
}
/* "[X] RETURN TO KRYPT ENTRANCE [Y] QUIT TO MAIN MENU" and similar button hints */
static int is_prompt(const wchar* p)
{
    return contains_ci(p, "navText") || contains_ci(p, "helpText") || contains_ci(p, "buttonPrompt") ||
           contains_ci(p, "ButtonText") || contains_ci(p, "hintText") || contains_ci(p, ".help.");
}
static int is_popup_message(const wchar* p)
{
    return (contains_ci(p, "msgBox") || contains_ci(p, "popup") || contains_ci(p, "dialog")) &&
           (contains_ci(p, "mainText") || contains_ci(p, "messaging") || contains_ci(p, "message"));
}
static int is_price(const wchar* p) { return contains_ci(p, "cost") || contains_ci(p, "price"); }
static int ends_with_colon(const wchar* t) { int n = wlen(t); return n > 0 && t[n - 1] == ':'; }
static int ignored_path(const wchar* p)
{
    return contains_ci(p, "ButtonLogs") || contains_ci(p, "wbPlayWindow") || contains_ci(p, "BenchmarkStats") ||
           contains_ci(p, "versionNumber") || starts_ci(p, "_root.legal.") || contains_ci(p, "panel.news") ||
           contains_ci(p, "MinimapContainer.timer") || contains_ci(p, "MapSelection") ||
           contains_ci(p, "FrameDataLayer");   /* read with the move it belongs to (movelist_frames) */
}

/* ------------------------------------------------------------------ */
/* text table (worker thread only), keyed by element path              */
typedef struct { u64 key; wchar path[PATHMAX]; wchar text[TEXTMAX]; DWORD changed, touched, winStart; int winCount, used, gen; } Entry;
#define NENT 4096
#define PROBE 64
static Entry* g_ent;
static int g_gen;
#define NRECENT 64
static int g_recent[NRECENT], g_recentGen[NRECENT], g_recentSpoken[NRECENT]; static DWORD g_recentT[NRECENT]; static int g_nrecent;

static u64 hash_path(const wchar* p)
{
    u64 h = 1469598103934665603ULL;
    for (; *p; p++) { h ^= *p; h *= 1099511628211ULL; }
    return h | 1;
}
static Entry* find_entry(u64 key)
{
    int idx = (int)((key * 0x9E3779B97F4A7C15ULL) >> 52) & (NENT - 1);
    Entry* oldest = 0;
    for (int k = 0; k < PROBE; k++) {
        Entry* e = &g_ent[(idx + k) & (NENT - 1)];
        if (e->used && e->key == key) return e;
        if (!e->used) { oldest = e; break; }
        if (!oldest || (int)(e->changed - oldest->changed) < 0) oldest = e;
    }
    /* new entry (or recycle the oldest one nearby) */
    Entry* e = oldest;
    e->used = 1; e->key = key; e->path[0] = 0; e->text[0] = 0; e->winCount = 0; e->winStart = 0; e->changed = e->touched = GetTickCount();
    e->gen = ++g_gen;
    return e;
}
/* a ticker/timer: keeps changing for seconds. A status line that changes a few
   times quickly (LOOKING... CREATING... WAITING FOR OPPONENT) is not busy. */
/* find an entry without creating one */
static Entry* lookup_entry(u64 key)
{
    int idx = (int)((key * 0x9E3779B97F4A7C15ULL) >> 52) & (NENT - 1);
    for (int k = 0; k < PROBE; k++) {
        Entry* e = &g_ent[(idx + k) & (NENT - 1)];
        if (e->used && e->key == key) return e;
        if (!e->used) return 0;
    }
    return 0;
}
typedef struct { u64 focus; u64 key[2]; } PromptMemo;
static PromptMemo g_promptMemo[64];

static int is_busy(Entry* e, DWORD now) { return now - e->winStart <= 4000 && e->winCount > 8; }

/* focus state */
static wchar g_focusPath[PATHMAX];
static DWORD g_focusTime; static int g_focusPending;
static int g_gridX = -1, g_gridY = -1;
static wchar g_lastFocus[PATHMAX]; static DWORD g_lastFocusT;
static wchar g_focusItem[PATHMAX];   /* parent holding the focused item's labels */
static void* g_valEntry; static int g_valGen; static DWORD g_valT;   /* value change waiting to be said */
static DWORD g_burstT; static int g_burstN;                           /* texts set within 50 ms */

static int is_child_path(const wchar* child, const wchar* parent);
static void say(const wchar* raw, int interrupt);
static void speak(const wchar* s, int interrupt);

/* ------------------------------------------------------------------ */
/* fight readout: damage per hit and combo totals                      */
/* Practice mode's damage panel sets, for each hit,
     _root.BaseScreen.Elements.DamageBase.Damage.Cell<n>.DamageText.text  "7.00%"
     _root.BaseScreen.Elements.DamageBase.Damage.Cell<n>.HitText.text     "MID"
   and every fight's combo counter sets
     _root.comboNotifierLeft|Right.container.hitsAmount.text  "3"
     _root.comboNotifierLeft|Right.container.damageAmount.text "12%"
   A single hit is read after a short wait ("mid, 7 percent"). Hits that turn
   into a combo are not read one by one; the combo is read once it ends
   ("player 1, 3 hit combo, 12 percent"). */
static int g_damageNumbers = 1;   /* F10: say the percentages (hit levels and combo counts always) */
static int g_comboReadout = 1;    /* Ctrl+F10: read hits and combos at all */
static wchar g_hitDmg[32], g_hitLevel[32]; static int g_hitCell = -1; static DWORD g_hitT; static int g_hitPending;
typedef struct { wchar hits[16], dmg[32]; DWORD t, start; int dirty, ended, gotHits; } Combo;
static DWORD g_lifeDropT; static int g_lifeKnown;   /* last time a fighter lost health (health_tick) */
/* the wall hum is kept quiet while the fight isn't being played: 1 = a game menu is up
   (pause, results; menus only appear then), 2 = a round or the match is won. Ends when the
   round timer ticks again, a new fight starts, or (1 only, for fights without a timer)
   when a fighter moves. */
static int g_wallQuiet, g_quietPosSet;
static Combo g_combo[2];
static wchar g_seq[256]; static int g_seqN; static DWORD g_seqT;   /* "high, mid, mid" */
#define HIT_WAIT 900     /* ms: a second hit within this turns the first into a combo (launchers take ~850) */
#define COMBO_END 700    /* ms without a new hit: hit levels belong to a new string */
#define COMBO_GIVE_UP 3000  /* ms: read the combo anyway if its Finish animation was missed */
#define THROW_WAIT 2500  /* ms: a throw's grab lands first, its slam ~1.7 s later */
static DWORD g_throwT; static int g_throwOn;   /* a throw's grab landed; its damage is still to come */

static int weq_ci(const wchar* a, const char* b)
{
    for (; *a && *b; a++, b++) { wchar x = *a, y = (wchar)*b; if (x >= 'a' && x <= 'z') x -= 32; if (y >= 'a' && y <= 'z') y -= 32; if (x != y) return 0; }
    return !*a && !*b;
}
static int cell_index(const wchar* p)
{
    const wchar* q = p; int n = -1;
    for (; *q; q++) if (q[0] == 'C' && q[1] == 'e' && q[2] == 'l' && q[3] == 'l' && q[4] >= '0' && q[4] <= '9') {
        n = 0; for (q += 4; *q >= '0' && *q <= '9'; q++) n = n * 10 + (*q - '0');
        break;
    }
    return n;
}
/* "7.00%" -> "7 percent", "4.75%" -> "4.75 percent", "12%" -> "12 percent" */
static void say_percent(const wchar* in, wchar* out, int max)
{
    int o = 0, i = 0, dot = -1;
    for (; in[i] && in[i] != '%' && o < max - 12; i++) { if (in[i] == '.') dot = o; out[o++] = in[i]; }
    if (dot >= 0) { while (o > dot + 1 && out[o - 1] == '0') o--; if (o == dot + 1) o = dot; }
    out[o] = 0;
    if (in[i] == '%') wcat(out, (const wchar*)L" percent", max);
}
static void lower_word(const wchar* in, wchar* out, int max)
{
    int i = 0; for (; in[i] && i < max - 1; i++) { wchar c = in[i]; out[i] = (c >= 'A' && c <= 'Z') ? c + 32 : c; } out[i] = 0;
}
/* returns 1 if the text belongs to the fight readout (and has been handled) */
static int fight_text(Entry* e, const wchar* clean, DWORD t)
{
    const wchar* p = e->path;
    if (contains_ci(p, "DamageBase.Damage.Cell")) {
        int cell = cell_index(p);
        if (cell != g_hitCell || t - g_hitT > 100) { g_hitCell = cell; g_hitDmg[0] = g_hitLevel[0] = 0; }
        if (contains_ci(p, "DamageText")) wcpy(g_hitDmg, clean, 32);
        else if (contains_ci(p, "HitText")) {
            wcpy(g_hitLevel, clean, 32);
            /* a throw that opens the action (not the last hit of a running combo) */
            if (weq_ci(clean, "THROW") && !(g_combo[0].dirty && !g_combo[0].ended)) { g_throwOn = 1; g_throwT = t; }
            /* remember the levels of the hits in a row, for the combo summary */
            /* a new string starts after a quiet moment, unless a combo is still going */
            if (t - g_seqT > COMBO_END && !(g_combo[0].dirty && !g_combo[0].ended)) { g_seq[0] = 0; g_seqN = 0; }
            if (clean[0]) {
                wchar lw[32]; lower_word(clean, lw, 32);
                if (g_seq[0]) wcat(g_seq, (const wchar*)L", ", 256);
                wcat(g_seq, lw, 256); g_seqN++;
            }
            g_seqT = t;
        }
        g_hitT = t; g_hitPending = 1;
        return 1;
    }
    if (contains_ci(p, "comboNotifier")) {
        int side = contains_ci(p, "comboNotifierRight") ? 1 : 0;
        Combo* c = &g_combo[side];
        if (contains_ci(p, "hitsAmount")) { if (!c->dirty) c->start = t; wcpy(c->hits, clean, 16); c->t = t; c->dirty = 1; c->ended = 0; c->gotHits = 1; }
        else if (contains_ci(p, "damageAmount")) { if (!c->dirty) { c->start = t; c->gotHits = 0; } wcpy(c->dmg, clean, 32); c->t = t; c->dirty = 1; c->ended = 0; }
        return 1;   /* "Hits" / "Damage" labels are not read either */
    }
    return 0;
}
static void combo_finished(Event* ev)
{
    if (!contains_ci(ev->path, "comboNotifier")) return;
    logf3((const wchar*)L"COMBO-END", ev->path, 0);
    Combo* c = &g_combo[contains_ci(ev->path, "comboNotifierRight") ? 1 : 0];
    if (c->dirty) c->ended = 1;
}

static void fight_tick(DWORD now)
{
    /* a combo that just grew swallows the hit that started it */
    for (int s = 0; s < 2; s++)
        if (g_hitPending && g_combo[s].dirty && (int)(g_combo[s].t - g_hitT) >= -50) g_hitPending = 0;
    int wait = weq_ci(g_hitLevel, "THROW") ? THROW_WAIT : HIT_WAIT;
    if (g_hitPending && now - g_hitT >= (DWORD)wait) {
        g_hitPending = 0;
        g_throwOn = 0;   /* a throw with nothing after it (broken, or no slam): read the grab */
        wchar line[96]; line[0] = 0;
        if (g_hitLevel[0]) lower_word(g_hitLevel, line, 96);
        if (g_hitDmg[0] && (g_damageNumbers || !line[0])) {
            wchar pct[48]; say_percent(g_hitDmg, pct, 48);
            if (line[0]) wcat(line, (const wchar*)L", ", 96);
            wcat(line, pct, 96);
        }
        if (line[0] && g_comboReadout) speak(line, 1);
    }
    for (int s = 0; s < 2; s++) {
        Combo* c = &g_combo[s];
        /* wait for the counter's Finish animation: combos can pause for a second
           mid-string (launchers, specials) and a timer alone cut them in half */
        if (!c->dirty || !(c->ended || now - c->t >= COMBO_GIVE_UP)) continue;
        c->dirty = 0; c->ended = 0;
        /* the same hit count as last time isn't sent again, so a combo can be just a new
           damage value. But the counter also re-sets its old damage when a fight starts:
           without a new count, only read it if somebody actually lost health just now */
        if (!c->gotHits && g_lifeKnown && now - g_lifeDropT > 3000) { c->gotHits = 0; continue; }
        c->gotHits = 0;
        int hits = 0; for (int i = 0; c->hits[i] >= '0' && c->hits[i] <= '9'; i++) hits = hits * 10 + (c->hits[i] - '0');
        if (hits < 2) continue;
        wchar line[128];
        /* the throw came first and the counted hits started within THROW_WAIT of it */
        int isThrow = s == 0 && g_throwOn && (int)(c->start - g_throwT) >= 0 && (int)(c->start - g_throwT) < THROW_WAIT;
        g_throwOn = 0;
        if (isThrow) {   /* grab + slam: "throw, 12 percent" */
            wcpy(line, (const wchar*)L"throw", 128);
            if (c->dmg[0] && g_damageNumbers) { wchar pct[48]; say_percent(c->dmg, pct, 48); wcat(line, (const wchar*)L", ", 128); wcat(line, pct, 128); }
            g_seq[0] = 0; g_seqN = 0;
            if (g_comboReadout) speak(line, 1);
            continue;
        }
        wcpy(line, c->hits, 128); wcat(line, (const wchar*)L" hit combo", 128);
        if (c->dmg[0] && g_damageNumbers) { wchar pct[48]; say_percent(c->dmg, pct, 48); wcat(line, (const wchar*)L", ", 128); wcat(line, pct, 128); }
        /* the hit levels, only when they clearly belong to this combo */
        static wchar full[400];
        wcpy(full, line, 400);
        if (s == 0 && g_seqN == hits && (int)(c->t - g_seqT) >= -100 && (int)(c->t - g_seqT) <= 100) { wcat(full, (const wchar*)L", ", 400); wcat(full, g_seq, 400); }
        if (s == 0) { g_seq[0] = 0; g_seqN = 0; }   /* this string is done */
        if (g_comboReadout) speak(full, 1);
    }
}

/* incoming fight challenges: the 30 second timer is only read at 10 and 5 seconds
   left; a challenger who isn't the focused one is announced on their own; the
   detail texts (record, odds, ping, buttons) are part of the focus sentence */
static int challenge_text(Entry* e, const wchar* clean);

/* ------------------------------------------------------------------ */
/* typing a name or message (Create Room, room chat)                    */
/* On PC the game's text box (PCSystemChatPad + TextBox, screen ui_c_text_entry_box)
   shows each typed key by re-setting
     _root.msgBoxAnim.dynamic_text.messaging   the text typed so far
   next to staticText.message (the title), descriptionText.messaging and
   center_msging.text (the keys). Keys come from the window's WM_CHAR messages, so
   the keyboard types; Enter accepts, Escape cancels. The title and keys are read
   like any popup; the typed text is echoed one character at a time here instead. */
#define GSYSTEMCHATPAD 0x14331D568ULL   /* SystemChatPad* (a PCSystemChatPad on PC) */
static Entry* entry_at(const char* path);
static wchar g_entryRaw[TEXTMAX]; static int g_entryOpen; static DWORD g_entryOpenT, g_entryKeyT; static int g_entryHint;
static int is_entry_text(const wchar* p) { return contains_ci(p, "msgBoxAnim.dynamic_text.messag"); }
/* the text box is still up: PCSystemChatPad+0xA8 -> TextBox, whose first byte is
   "enabled" (set by TextBox::Initialize, cleared by TextBox::Terminate) */
static int entry_box_up(void)
{
    if (!g_base) return 1;   /* test harness */
    u64* gp = (u64*)(g_base + (GSYSTEMCHATPAD - IMAGE_BASE));
    if (!readable(gp, 8) || !*gp || !readable((BYTE*)*gp + 0xA8, 8)) return 0;
    BYTE* tb = *(BYTE**)((BYTE*)*gp + 0xA8);
    return tb && readable(tb, 1) && tb[0];
}
static void char_name(wchar c, wchar* out, int max)
{
    static const struct { wchar c; const char* w; } names[] = {
        {' ',"space"},{'.',"dot"},{',',"comma"},{'-',"dash"},{'_',"underscore"},{'\'',"apostrophe"},{'"',"quote"},
        {'!',"exclamation"},{'?',"question"},{':',"colon"},{';',"semicolon"},{'/',"slash"},{'\\',"backslash"},
        {'(',"left paren"},{')',"right paren"},{'[',"left bracket"},{']',"right bracket"},{'&',"and"},{'@',"at"},
        {'#',"number"},{'$',"dollar"},{'%',"percent"},{'*',"star"},{'+',"plus"},{'=',"equals"},{'<',"less"},{'>',"greater"},
        {'~',"tilde"},{'^',"caret"},{'|',"bar"},{'`',"grave"},{'{',"left brace"},{'}',"right brace"},
    };
    out[0] = 0;
    for (int i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++)
        if (names[i].c == c) { int k = 0; for (; names[i].w[k] && k < max - 1; k++) out[k] = (wchar)names[i].w[k]; out[k] = 0; return; }
    if (c >= 'A' && c <= 'Z') { wcpy(out, (const wchar*)L"cap ", max); int n = wlen(out); out[n] = c; out[n + 1] = 0; return; }
    out[0] = c; out[1] = 0;
}
static void entry_say_all(int interrupt)
{
    static wchar line[TEXTMAX + 64];
    wcpy(line, g_entryRaw[0] ? g_entryRaw : (const wchar*)L"blank", TEXTMAX + 64);
    speak(line, interrupt);
}
static void entry_text(const wchar* raw, DWORD t)
{
    static wchar nw[TEXTMAX]; wcpy(nw, raw ? raw : (const wchar*)L"", TEXTMAX);
    int on = wlen(g_entryRaw), nn = wlen(nw);
    logf3((const wchar*)L"TYPED", 0, nw);
    if (!g_entryOpen) {               /* the box just opened (it starts with an empty text) */
        g_entryOpen = 1; g_entryOpenT = t; g_entryHint = 1; wcpy(g_entryRaw, nw, TEXTMAX);
        log_w((const wchar*)L"ENTRY open");
        if (!nn) return;
        on = 0;
    }
    g_entryKeyT = t; g_entryHint = 0;   /* typing started: no hint needed */
    int same = 1; for (int i = 0; i < on && i < nn; i++) if (g_entryRaw[i] != nw[i]) { same = 0; break; }
    wchar w[32];
    if (same && nn == on + 1) { char_name(nw[on], w, 32); speak(w, 1); }        /* a key typed */
    else if (same && nn == on - 1) {                                           /* backspace */
        char_name(g_entryRaw[nn], w, 32);
        static wchar line[64]; wcpy(line, w, 64);
        if (!nn) wcat(line, (const wchar*)L", blank", 64);
        speak(line, 1);
    }
    else if (nn != on || !same) { wcpy(g_entryRaw, nw, TEXTMAX); entry_say_all(1); return; }
    wcpy(g_entryRaw, nw, TEXTMAX);
}
static void entry_tick(DWORD now)
{
    if (!g_entryOpen) return;
    /* how to type, once the title and keys have been read */
    if (g_entryHint && now - g_entryOpenT >= 900) {
        g_entryHint = 0;
        speak((const wchar*)L"Type with the keyboard. Enter to accept, Escape to cancel, F6 reads what you typed.", 0);
    }
    static DWORD lastCheck;
    if (now - lastCheck >= 200) {
        lastCheck = now;
        if (now - g_entryOpenT > 500 && !entry_box_up()) { g_entryOpen = 0; g_entryHint = 0; g_entryRaw[0] = 0; log_w((const wchar*)L"ENTRY closed"); }
    }
}
/* F6 while typing: the title and the text so far */
static int entry_read(void)
{
    if (!g_entryOpen) return 0;
    static wchar line[TEXTMAX * 2 + 64]; line[0] = 0;
    Entry* title = entry_at("_root.msgBoxAnim.staticText.message");
    if (title) { wcpy(line, title->text, TEXTMAX * 2 + 64); wcat(line, (const wchar*)L", ", TEXTMAX * 2 + 64); }
    wcat(line, (const wchar*)L"typed: ", TEXTMAX * 2 + 64);
    wcat(line, g_entryRaw[0] ? g_entryRaw : (const wchar*)L"blank", TEXTMAX * 2 + 64);
    say(line, 1);
    return 1;
}

static void handle_text(Event* ev)
{
    wchar clean[TEXTMAX];
    clean_text(ev->text, clean, TEXTMAX);
    if (contains_ci(ev->path, "roundTimer.timer")) g_wallQuiet = 0;   /* the fight is on */
    else if ((contains_ci(ev->path, "endUI.wins") || contains_ci(ev->path, "centerText")) && contains_ci(clean, " WINS")) g_wallQuiet = 2;
    const wchar* tag = ev->type == EV_TEXT ? (const wchar*)L"TEXT" : ev->type == EV_HTML ? (const wchar*)L"HTML" :
                       ev->type == EV_VTEXT ? (const wchar*)L"VTEXT" : (const wchar*)L"VHTML";
    u64 key = ev->path[0] ? hash_path(ev->path) : (ev->id & ~1ULL);
    if (!key) return;
    Entry* e = find_entry(key);
    if (ev->path[0]) wcpy(e->path, ev->path, PATHMAX);
    DWORD prevTouched = e->touched;
    e->touched = ev->t;                        /* set again, even if unchanged: still current */
    if (fight_text(e, clean, ev->t)) {         /* damage and combos have their own readout */
        if (!weq(e->text, clean)) { logf3(tag, e->path, clean); wcpy(e->text, clean, TEXTMAX); }
        return;
    }
    if (ev->path[0] && is_entry_text(ev->path)) {   /* typing: echoed key by key, never auto-read */
        wcpy(e->text, clean, TEXTMAX);
        entry_text(ev->text, ev->t);
        return;
    }
    int again = 0;
    if (weq(e->text, clean)) {
        /* a popup asking the same question again ("Do you want to save your changes?")
           is new to the player even though the words are the same. A popup that is
           simply re-set while it stays up (every frame, let through once a second)
           has no quiet gap before it and is not repeated. */
        if (!(clean[0] && is_popup_message(e->path) && ev->t - prevTouched > 1500)) return;
        again = 1;
    }
    logf3(again ? (const wchar*)L"TEXT-AGAIN" : tag, e->path[0] ? e->path : (const wchar*)L"?", clean);
    wcpy(e->text, clean, TEXTMAX);
    DWORD now = ev->t;
    if (now - e->winStart > 4000) { e->winStart = now; e->winCount = 0; }
    e->winCount++;
    e->changed = now;
    if (g_padMode && contains_ci(e->path, "looking.mainText") &&
        (contains_ci(clean, "CANCEL") || contains_ci(clean, "FOUND") || contains_ci(clean, "ACCEPTED") ||
         contains_ci(clean, "JOINING") || contains_ci(clean, "EXITING") || contains_ci(clean, "LEAVING"))) {
        g_padMode = 0;   /* invite screen is over: the controller goes back to the game */
        log_w((const wchar*)L"INVITE controller mode off");
    }
    if (!clean[0] || ignored_path(e->path)) return;
    if (challenge_text(e, clean)) return;
    /* a value of the item you are on changed (volume 45 -> 44, ON -> OFF): say it right
       away, unless a whole page is being filled in at this moment (checked in the loop) */
    if (ev->t - g_burstT > 50) { g_burstT = ev->t; g_burstN = 0; }
    g_burstN++;
    if (!g_focusPending && g_focusItem[0] && is_child_path(e->path, g_focusItem) && !ends_with_colon(clean)) {
        g_valEntry = e; g_valGen = e->gen; g_valT = ev->t;
        return;
    }
    int slot = g_nrecent % NRECENT;
    g_recent[slot] = (int)(e - g_ent); g_recentGen[slot] = e->gen; g_recentT[slot] = now; g_recentSpoken[slot] = 0;
    g_nrecent++;
}

static int is_child_path(const wchar* child, const wchar* parent)
{
    int n = wlen(parent);
    if (!n) return 0;
    for (int i = 0; i < n; i++) if (child[i] != parent[i]) return 0;
    return child[n] == '.' || child[n] == '/' || child[n] == 0;
}

static int add_part(wchar* out, int max, const wchar* part, int* count)
{
    if (!part[0]) return 0;
    /* skip a phrase that is already one of the parts of the utterance */
    int pl = wlen(part), ol = wlen(out);
    for (int i = 0; i + pl <= ol; i++) {
        if (i > 0 && !(i >= 2 && out[i - 2] == ',' && out[i - 1] == ' ')) continue;   /* must start a part */
        int m = 1; for (int j = 0; j < pl; j++) if (out[i + j] != part[j]) { m = 0; break; }
        if (m && (out[i + pl] == 0 || (out[i + pl] == ',' && out[i + pl + 1] == ' '))) return 0;
    }
    if (out[0]) wcat(out, (const wchar*)L", ", max);
    wcat(out, part, max);
    (*count)++;
    return 1;
}

/* "M O V E D A T A" -> "MOVEDATA": join runs of 3+ single-letter words */
static int single_at(const wchar* t, int i)   /* t[i] is a 1-char word? */
{
    int L = (i == 0 || t[i - 1] == ' ');
    int R = (t[i + 1] == 0 || t[i + 1] == ' ' || t[i + 1] == ',');
    return t[i] && t[i] != ' ' && t[i] != ',' && L && R;
}
static void collapse_spaced(wchar* t)
{
    static wchar src[4096];
    wcpy(src, t, 4096);
    int o = 0, i = 0;
    while (src[i]) {
        if (single_at(src, i)) {
            int j = i, n = 0;   /* count the run: X Y Z */
            while (single_at(src, j)) { n++; if (src[j + 1] == ' ' && single_at(src, j + 2)) j += 2; else break; }
            if (n >= 3) {
                for (int k = 0, p = i; k < n; k++, p += 2) t[o++] = src[p];
                i = j + 1;
                continue;
            }
        }
        t[o++] = src[i++];
    }
    t[o] = 0;
}

static void say(const wchar* raw, int interrupt)
{
    static wchar buf[4096];
    speakify(raw, buf, 4096);
    collapse_spaced(buf);
    speak(buf, interrupt);
}

/* path order, comparing numbers by value so Cell_2 comes before Cell_10 */
static int path_less(const wchar* a, const wchar* b)
{
    while (*a && *b) {
        if (*a >= '0' && *a <= '9' && *b >= '0' && *b <= '9') {
            u64 x = 0, y = 0;
            while (*a >= '0' && *a <= '9') x = x * 10 + (*a++ - '0');
            while (*b >= '0' && *b <= '9') y = y * 10 + (*b++ - '0');
            if (x != y) return x < y;
            continue;
        }
        if (*a != *b) return *a < *b;
        a++; b++;
    }
    return *a == 0 && *b != 0;
}

/* labels first, values last, otherwise screen order: "EFFECTS, 90" not "90, EFFECTS" */
static int item_rank(const wchar* p)
{
    const wchar* last = p; int dots = 0;
    for (const wchar* q = p; *q; q++) if (*q == '.') dots++;
    (void)last; (void)dots;
    if (contains_ci(p, "Label") || contains_ci(p, "Title") || contains_ci(p, "Name") || contains_ci(p, "Header")) return 0;
    if (contains_ci(p, "Value") || contains_ci(p, "Cursor") || contains_ci(p, ".Txt") || contains_ci(p, "Amount")) return 2;
    return 1;
}
static int item_kind(const wchar* p) { return item_rank(p) == 2 ? 1 : 0; }
static int item_less(const wchar* a, const wchar* b)
{
    int ra = item_rank(a), rb = item_rank(b);
    if (ra != rb) return ra < rb;
    return path_less(a, b);
}

/* when was this list last filled in? = the newest moment when 3 or more of its
   texts were set within 300 ms of each other (a page being built), 0 if never */
static DWORD last_rebuild(const wchar* list)
{
    static DWORD t[512]; int n = 0;
    for (int i = 0; i < NENT && n < 512; i++) { Entry* e = &g_ent[i]; if (e->used && is_child_path(e->path, list)) t[n++] = e->touched; }
    for (int a = 1; a < n; a++) for (int b = a; b > 0 && (int)(t[b] - t[b-1]) > 0; b--) { DWORD x = t[b]; t[b] = t[b-1]; t[b-1] = x; }   /* newest first */
    for (int i = 0; i + 2 < n; i++) {
        if (t[i] - t[i + 2] <= 300) {          /* a cluster starts here; find where it ends */
            int j = i + 2;
            while (j + 1 < n && t[i] - t[j + 1] <= 300) j++;
            return t[j];
        }
    }
    return 0;
}

static int count_children(const wchar* parent)
{
    int n = 0;
    for (int i = 0; i < NENT; i++) { Entry* e = &g_ent[i]; if (e->used && e->text[0] && is_child_path(e->path, parent)) n++; }
    return n;
}

/* ------------------------------------------------------------------ */
/* online rooms: room list entries and players in a room have their own wording */
static int challenge_line(const wchar* cell, wchar* out, int max, int brief);
static Entry* child_ending(const wchar* parent, const char* suffix)
{
    int sl = 0; while (suffix[sl]) sl++;
    Entry* best = 0;
    for (int i = 0; i < NENT; i++) {
        Entry* e = &g_ent[i];
        if (!e->used || !e->text[0] || !is_child_path(e->path, parent)) continue;
        int pl = wlen(e->path); if (pl < sl) continue;
        if (!starts_ci(e->path + pl - sl, suffix)) continue;
        if (!best || (int)(e->touched - best->touched) > 0) best = e;
    }
    return best;
}
static int is_na(const wchar* t) { return weq_ci(t, "N/A") || weq_ci(t, "...") || !t[0]; }
/* "4/12" -> "4 wins 12 losses" */
static void say_record(const wchar* t, wchar* out, int max)
{
    int i = 0; wchar a[16], b[16]; int na = 0, nb = 0;
    while (t[i] && t[i] != '/' && na < 15) a[na++] = t[i++]; a[na] = 0;
    if (t[i] == '/') i++;
    while (t[i] && nb < 15) b[nb++] = t[i++]; b[nb] = 0;
    wcpy(out, a, max); wcat(out, weq(a, (const wchar*)L"1") ? (const wchar*)L" win " : (const wchar*)L" wins ", max);
    wcat(out, b, max); wcat(out, weq(b, (const wchar*)L"1") ? (const wchar*)L" loss" : (const wchar*)L" losses", max);
}
/* "85ms" -> "ping 85 milliseconds" */
static void say_ping(const wchar* t, wchar* out, int max)
{
    int i = 0, o = 0; wchar n[16];
    while (t[i] >= '0' && t[i] <= '9' && o < 15) n[o++] = t[i++]; n[o] = 0;
    wcpy(out, (const wchar*)L"ping ", max); wcat(out, n[0] ? n : t, max);
    if (n[0]) wcat(out, weq(n, (const wchar*)L"1") ? (const wchar*)L" millisecond" : (const wchar*)L" milliseconds", max);
}
/* a single value of a room player that changed while you're on them */
static int room_value_line(Entry* e, wchar* out, int max)
{
    if (!contains_ci(e->path, "PlayerListGridSelectElement.Cell_") || is_na(e->text)) return 0;
    if (contains_ci(e->path, "pingText")) { say_ping(e->text, out, max); return 1; }
    if (contains_ci(e->path, "winLossValue")) { say_record(e->text, out, max); return 1; }
    if (contains_ci(e->path, "matchUpValue")) { say_percent(e->text, out, max); wcat(out, (const wchar*)L" win odds", max); return 1; }
    return 0;
}
static Entry* entry_at(const char* path)   /* the current text of one exact label path */
{
    wchar w[PATHMAX]; int i = 0; for (; path[i] && i < PATHMAX - 1; i++) w[i] = (BYTE)path[i]; w[i] = 0;
    Entry* e = lookup_entry(hash_path(w));
    return (e && e->text[0]) ? e : 0;
}
/* "sixpaths099 challenged you to Versus. Level 73, ..., 30 seconds to answer. A accept, ..." */
static int challenge_line(const wchar* cell, wchar* out, int max, int brief)
{
    Entry* name = child_ending(cell, "gamertagText.text");
    if (!name) return 0;
    wchar part[96]; Entry* e;
    wcpy(out, name->text, max);
    wcat(out, brief ? (const wchar*)L" also challenged you" : (const wchar*)L" challenged you", max);
    if (brief) return 1;
    if ((e = entry_at("_root.looking.matchTextBig.text")) && !is_na(e->text)) { wcat(out, (const wchar*)L" to ", max); wcat(out, e->text, max); }
    wcat(out, (const wchar*)L".", max);
    if ((e = child_ending(cell, "playerLevel.text")) && !is_na(e->text)) { wcat(out, (const wchar*)L" Level ", max); wcat(out, e->text, max); }
    if ((e = entry_at("_root.looking.detailData1.Text")) && !is_na(e->text)) { say_record(e->text, part, 96); wcat(out, (const wchar*)L", ", max); wcat(out, part, max); }
    if ((e = entry_at("_root.looking.detailData2.Text")) && !is_na(e->text)) { say_percent(e->text, part, 96); wcat(out, (const wchar*)L", ", max); wcat(out, part, max); wcat(out, (const wchar*)L" win odds", max); }
    if ((e = entry_at("_root.looking.detailData3.ping.pingTextWrapper.pingText")) && !is_na(e->text)) { say_ping(e->text, part, 96); wcat(out, (const wchar*)L", ", max); wcat(out, part, max); }
    if ((e = child_ending(cell, "timer.text")) && !is_na(e->text)) { wcat(out, (const wchar*)L". ", max); wcat(out, e->text, max); wcat(out, (const wchar*)L" seconds to answer", max); }
    if ((e = entry_at("_root.looking.buttonText.text"))) { wcat(out, (const wchar*)L". ", max); wcat(out, e->text, max); }
    return 1;
}

static int room_focus_line(wchar* out, int max)
{
    out[0] = 0;
    if (contains_ci(g_focusPath, "onlineMainMenu.panel.challenges.cell")) return challenge_line(g_focusPath, out, max, 0);
    if (contains_ci(g_focusPath, "RoomListGridSelectElement.Cell_")) {
        Entry* name = child_ending(g_focusPath, "LabelText");
        Entry* count = child_ending(g_focusPath, "ValueText");
        if (!name) return 0;
        wcpy(out, name->text, max);
        if (count) { wcat(out, (const wchar*)L", ", max); wcat(out, count->text, max); wcat(out, weq(count->text, (const wchar*)L"1") ? (const wchar*)L" player" : (const wchar*)L" players", max); }
        return 1;
    }
    if (!contains_ci(g_focusPath, "PlayerListGridSelectElement.Cell_")) return 0;
    Entry* name = child_ending(g_focusPath, "gamertagText.text");
    if (!name) return 0;
    wchar part[96];
    wcpy(out, name->text, max);
    Entry* e;
    if ((e = child_ending(g_focusPath, "playerLevel")) && !is_na(e->text)) { wcat(out, (const wchar*)L", level ", max); wcat(out, e->text, max); }
    Entry* st = child_ending(g_focusPath, "statusTitle.text");
    Entry* mode = child_ending(g_focusPath, "info.help.text");
    if (st) {
        wchar low[48]; lower_word(st->text, low, 48);
        wcat(out, (const wchar*)L", ", max); wcat(out, low, max);
        if (mode && !is_na(mode->text)) { wcat(out, (const wchar*)L" ", max); wcat(out, mode->text, max); }
        else if (weq_ci(st->text, "LOOKING FOR")) wcat(out, (const wchar*)L" any match", max);
    }
    if ((e = child_ending(g_focusPath, "winLossValue.text")) && !is_na(e->text)) { say_record(e->text, part, 96); wcat(out, (const wchar*)L", ", max); wcat(out, part, max); }
    if ((e = child_ending(g_focusPath, "matchUpValue.color.text")) && !is_na(e->text)) { say_percent(e->text, part, 96); wcat(out, (const wchar*)L", ", max); wcat(out, part, max); wcat(out, (const wchar*)L" win odds", max); }
    if ((e = child_ending(g_focusPath, "pingText")) && !is_na(e->text)) { say_ping(e->text, part, 96); wcat(out, (const wchar*)L", ", max); wcat(out, part, max); }
    return 1;
}

static wchar g_newChal[PATHMAX]; static DWORD g_newChalT, g_chalCountT, g_lastChallenger;
static wchar g_pingPend[64]; static DWORD g_pingT;
static int challenge_text(Entry* e, const wchar* clean)
{
    const wchar* p = e->path;
    if (starts_ci(p, "_root.looking.detail") || starts_ci(p, "_root.looking.module") || starts_ci(p, "_root.looking.matchText")) {
        if (!contains_ci(g_focusPath, "challenges.cell")) return 0;
        /* the ping often arrives after the sentence: read it once it does */
        if (contains_ci(p, "pingText") && !is_na(clean) && e->touched - g_focusTime > FOCUS_DELAY) {
            /* held briefly: when a second challenger arrives, the game shows their ping first */
            say_ping(clean, g_pingPend, 64); g_pingT = e->touched;
        }
        return 1;   /* part of the challenge sentence */
    }
    if (contains_ci(p, "panel.option0.titleState.title") && e->touched - g_chalCountT < 600) return 1;   /* count was just said */
    if (!contains_ci(p, "onlineMainMenu.panel.challenges.cell")) return 0;
    /* the challenge cell this text belongs to: path up to ".focus" */
    static wchar cell[PATHMAX]; wcpy(cell, p, PATHMAX);
    for (int i = 0; cell[i]; i++) if (starts_ci(cell + i, ".focus")) { cell[i] = 0; break; }
    if (contains_ci(p, "timer.text")) {
        int n = 0; for (int i = 0; clean[i] >= '0' && clean[i] <= '9'; i++) n = n * 10 + (clean[i] - '0');
        if (n == 10 || n == 5) {
            Entry* name = child_ending(cell, "gamertagText.text");
            wchar line[128]; wcpy(line, clean, 128); wcat(line, (const wchar*)L" seconds left to answer", 128);
            if (name) { wcat(line, (const wchar*)L" ", 128); wcat(line, name->text, 128); }
            speak(line, 0);
        }
        return 1;
    }
    if (contains_ci(p, "gamertagText.text")) {
        /* a challenger arrived: decide shortly, once the game has moved focus (or not) */
        wcpy(g_newChal, cell, PATHMAX); g_newChalT = e->touched; g_lastChallenger = e->touched;
        return 1;
    }
    return 1;   /* level, faction rank: read with the challenge */
}
/* run from speech_tick: a challenger the focus didn't go to is announced on their own */
static void challenge_tick(DWORD now)
{
    if (g_pingPend[0] && now - g_pingT >= 250) {
        if ((int)(g_lastChallenger - g_pingT) < -100 || (int)(g_lastChallenger - g_pingT) > 250) speak(g_pingPend, 0);
        g_pingPend[0] = 0;
    }
    if (!g_newChal[0] || now - g_newChalT < 200) return;
    static wchar cell[PATHMAX]; wcpy(cell, g_newChal, PATHMAX); g_newChal[0] = 0;
    if (weq(g_focusPath, cell) || is_child_path(g_focusPath, cell)) return;   /* the focus sentence covers it */
    int another = contains_ci(g_focusPath, "challenges.cell");
    static wchar line[600];
    if (!challenge_line(cell, line, 600, another)) return;
    Entry* count = entry_at("_root.onlineMainMenu.panel.option0.titleState.title.text");
    if (another && count) { wchar low[64]; lower_word(count->text, low, 64); wcat(line, (const wchar*)L", ", 600); wcat(line, low, 600); }
    g_chalCountT = now;
    /* the pending count is in this sentence: don't read it again */
    int start = g_nrecent > NRECENT ? g_nrecent - NRECENT : 0;
    for (int k = start; k < g_nrecent; k++)
        if (contains_ci(g_ent[g_recent[k % NRECENT]].path, "panel.option0.titleState.title")) g_recentSpoken[k % NRECENT] = 1;
    say(line, 0);
}

/* the practice move list shows the selected move's frame data in a panel:
   _root.MoveListBaseLayer.Elements.FrameDataLayer.Cell_0..8, Text0 the label (MOVE TYPE:,
   START-UP:, ACTIVE:, RECOVER:, DAMAGE:, BLOCK DAMAGE:, BLOCK ADV:, HIT ADV:, CANCEL:) and
   Text1 the value. It is read after the move's name and buttons, advantages right after
   the frame counts: "High, start-up 11, active 3, recovery 13, plus 2 on block, plus 24 on
   hit, cancel 20, damage 2 percent, block damage 0.4 percent". Only for a move (a cell with
   buttons); values the game marks N/A are left out. */
static void wcat_a(wchar* d, const char* s, int max) { int n = wlen(d); for (int i = 0; s[i] && n < max - 1; i++) d[n++] = (wchar)(BYTE)s[i]; d[n] = 0; }
static void movelist_frames(wchar* out, int max)
{
    char focusInput[PATHMAX]; int n = 0;
    for (; g_focusPath[n] && n < PATHMAX - 40; n++) focusInput[n] = (char)g_focusPath[n];
    focusInput[n] = 0;
    const char* tail = ".FocusElement.FocusText1.Text";
    for (int k = 0; tail[k]; k++) focusInput[n + k] = tail[k];
    focusInput[n + 29] = 0;   /* strlen(tail) */
    Entry* in = entry_at(focusInput);
    if (!in || !in->text[0]) return;   /* a heading, not a move */
    static const int order[9] = { 0, 1, 2, 3, 6, 7, 8, 4, 5 };
    static const char* names[9] = { "", "start-up", "active", "recovery", "damage", "block damage", "on block", "on hit", "cancel" };
    static wchar line[512]; line[0] = 0;
    for (int j = 0; j < 9; j++) {
        int c = order[j];
        char path[96] = "_root.MoveListBaseLayer.Elements.FrameDataLayer.Cell_0.Text1.Text";
        path[53] = (char)('0' + c);
        Entry* v = entry_at(path);
        if (!v || !v->text[0]) continue;   /* (the game sets every cell for each move) */
        const wchar* t = v->text;
        if (t[0] == '-' && (!t[1] || t[1] == '-')) continue;   /* no value */
        if ((t[0] == 'N' || t[0] == 'n') && t[1] == '/' && (t[2] == 'A' || t[2] == 'a') && !t[3]) continue;   /* N/A */
        wchar val[64]; int m = 0;
        for (int i = 0; t[i] && m < 60; i++) {
            if (t[i] == '%') { const char* w = " percent"; while (*w && m < 60) val[m++] = (wchar)*w++; }
            else val[m++] = t[i];
        }
        val[m] = 0;
        /* "2.00 percent" -> "2 percent", "0.40 percent" -> "0.4 percent" */
        for (int i = 0; val[i]; i++) if (val[i] == '.') {
            int e = i + 1; while (val[e] >= '0' && val[e] <= '9') e++;
            int z = e; while (z > i + 1 && val[z - 1] == '0') z--;
            int from = z == i + 1 ? i : z;
            int q = from; for (int r = e; ; r++) { val[q++] = val[r]; if (!val[r]) break; }
            break;
        }
        if (line[0]) wcat(line, (const wchar*)L", ", 512);
        if (c == 6 || c == 7) {   /* advantage: plus or minus, then on block / on hit */
            int neg = val[0] == '-', num = (val[0] >= '0' && val[0] <= '9') || (neg && val[1] >= '0' && val[1] <= '9');
            if (num && !(val[neg] == '0' && !val[neg + 1])) wcat(line, neg ? (const wchar*)L"minus " : (const wchar*)L"plus ", 512);
            wcat(line, val + (num && neg), 512); wcat(line, (const wchar*)L" ", 512); wcat_a(line, names[c], 512);
        } else if (c == 0) wcat(line, val, 512);
        else { wcat_a(line, names[c], 512); wcat(line, (const wchar*)L" ", 512); wcat(line, val, 512); }
    }
    if (line[0]) { if (out[0]) wcat(out, (const wchar*)L", ", max); wcat(out, line, max); }
}

static void announce_focus(void)
{
    static wchar out[2048]; out[0] = 0; int count = 0;
    /* a room player highlighted before the game filled in the name: try again shortly */
    static int roomRetries;
    if ((contains_ci(g_focusPath, "PlayerListGridSelectElement.Cell_") || contains_ci(g_focusPath, "RoomListGridSelectElement.Cell_") ||
         contains_ci(g_focusPath, "onlineMainMenu.panel.challenges.cell")) &&
        !room_focus_line(out, 2048)) {
        if (roomRetries++ < 10) { g_focusPending = 1; g_focusTime = GetTickCount() - FOCUS_DELAY + 100; return; }
    }
    roomRetries = 0;
    if (room_focus_line(out, 2048)) {   /* room list entry or a player in a room */
        wcpy(g_focusItem, g_focusPath, PATHMAX);
        int start = g_nrecent > NRECENT ? g_nrecent - NRECENT : 0;
        for (int k = start; k < g_nrecent; k++) if ((int)(g_recentT[k % NRECENT] - g_focusTime) >= -30) g_recentSpoken[k % NRECENT] = 1;
        say(out, 1);
        return;
    }
    /* 1. the focused item's own label: the focus marker is often a sibling of the
          label (lineItem2.menuItem.leftFocus.posAlign vs lineItem2.menuItem.centerJust.text),
          so climb up until we reach a parent that holds 1-4 text items */
    static wchar anc[PATHMAX];
    wcpy(anc, g_focusPath, PATHMAX);
    g_focusItem[0] = 0;
    int found = 0;
    for (int level = 0; level < 4 && anc[0]; level++) {
        int n = count_children(anc);
        if (n >= 1 && n <= 4) { found = 1; break; }
        if (n > 4) break;   /* reached the whole list: stop */
        int L = wlen(anc); while (L > 0 && anc[L - 1] != '.') L--;
        if (L <= 1) break;
        anc[L - 1] = 0;
    }
    if (!found) anc[0] = 0;
    /* the list that holds the focused item: its other items are not read */
    static wchar list[PATHMAX]; list[0] = 0;
    if (anc[0]) {
        wcpy(list, anc, PATHMAX);
        for (int level = 0; level < 3; level++) {
            int L = wlen(list); while (L > 0 && list[L - 1] != '.') L--;
            if (L <= 1) { list[0] = 0; break; }
            list[L - 1] = 0;
            if (count_children(list) > 4) break;
            if (level == 2) list[0] = 0;
        }
    }
    if (anc[0]) {
        /* pages that share slots (Audio and Gameplay options both use Cell_0...) leave the
           old page's text behind, hidden. Only keep text set since the list was last filled. */
        DWORD rebuilt = list[0] ? last_rebuild(list) : 0;
        Entry* all[8]; int na = 0;
        for (int i = 0; i < NENT && na < 8; i++) {
            Entry* e = &g_ent[i];
            if (e->used && e->text[0] && is_child_path(e->path, anc)) all[na++] = e;
        }
        /* an old text is a leftover only if a newer text of the same kind (name or value)
           replaced it. After "reset to default" only values are new, so names still count. */
        /* kinds: 0 = a name (MeterLabel, ToggleText, Title...), 1 = a value */
        int freshRank[3] = {0, 0, 0};
        for (int a = 0; a < na; a++) if (!rebuilt || (int)(all[a]->touched - rebuilt) >= -150) freshRank[item_kind(all[a]->path)] = 1;
        Entry* items[4]; int ni = 0;
        for (int a = 0; a < na && ni < 4; a++) {
            int old = rebuilt && (int)(all[a]->touched - rebuilt) < -150;
            if (old && freshRank[item_kind(all[a]->path)]) continue;   /* left over from another page */
            items[ni++] = all[a];
        }
        /* label first, value last, otherwise screen order */
        for (int a = 1; a < ni; a++) for (int b = a; b > 0 && item_less(items[b]->path, items[b-1]->path); b--) { Entry* t = items[b]; items[b] = items[b-1]; items[b-1] = t; }
        wcpy(g_focusItem, anc, PATHMAX);
        for (int a = 0; a < ni; a++) add_part(out, 2048, items[a]->text, &count);
    }
    /* 2. text that changed right after the focus moved (descriptions, character names) */
    int start = g_nrecent > NRECENT ? g_nrecent - NRECENT : 0;
    int burst = 0;
    for (int k = start; k < g_nrecent; k++) {
        int s = k % NRECENT;
        if ((int)(g_recentT[s] - g_focusTime) >= -30 && !no_letters(g_ent[g_recent[s]].text)) burst++;
    }
    for (int k = start; k < g_nrecent && count < 6; k++) {
        int s = k % NRECENT;
        if ((int)(g_recentT[s] - g_focusTime) < -30) continue;
        if (burst > 8) {   /* a whole screen just loaded: besides the item, read only titles */
            Entry* t = &g_ent[g_recent[s]];
            g_recentSpoken[s] = 1;
            if (!is_titleish(t->path) && !contains_ci(t->path, "description") && !is_prompt(t->path)) continue;
        }
        g_recentSpoken[s] = 1;
        Entry* e = &g_ent[g_recent[s]];
        if (e->gen != g_recentGen[s] || !e->text[0]) continue;
        if (list[0] && is_child_path(e->path, list) && !(anc[0] && is_child_path(e->path, anc)) && !is_titleish(e->path) && !is_prompt(e->path)) continue;
        if (no_letters(e->text) && !(anc[0] && is_child_path(e->path, anc))) continue;
        add_part(out, 2048, e->text, &count);
    }
    /* 3. button prompts the screen showed again (same text as last time, so not "changed") */
    int nprompt = 0; u64 promptKeys[2] = {0, 0};
    for (int i = 0; i < NENT && count < 7; i++) {
        Entry* e = &g_ent[i];
        if (!e->used || !e->text[0] || !is_prompt(e->path) || ignored_path(e->path)) continue;
        if ((int)(e->touched - g_focusTime) < -30 || (int)(e->touched - g_focusTime) > 150) continue;
        add_part(out, 2048, e->text, &count);
    }
    /* 4. remember which prompts came with this item, so returning to it (pausing the
          Krypt again) reads them too, even if the game doesn't set them again */
    for (int i = 0; i < NENT && nprompt < 2; i++) {
        Entry* e = &g_ent[i];
        if (!e->used || !e->text[0] || !is_prompt(e->path)) continue;
        /* must be one whole part of what we are about to say, and a real prompt, not a lone "[Y]" */
        int w = wlen(e->text), ol = wlen(out), found = 0;
        if (w < 6) continue;
        for (int k = 0; k + w <= ol && !found; k++) {
            if (k > 0 && !(k >= 2 && out[k - 2] == ',' && out[k - 1] == ' ')) continue;
            int m = 1; for (int j = 0; j < w && m; j++) if (out[k + j] != e->text[j]) m = 0;
            if (m && (out[k + w] == 0 || out[k + w] == ',')) found = 1;
        }
        if (found) promptKeys[nprompt++] = e->key;
    }
    u64 fk = hash_path(g_focusPath);
    PromptMemo* pm = &g_promptMemo[(fk >> 3) & 63];
    if (nprompt) { pm->focus = fk; pm->key[0] = promptKeys[0]; pm->key[1] = promptKeys[1]; }
    else if (pm->focus == fk) {
        for (int k = 0; k < 2; k++) {
            if (!pm->key[k]) continue;
            Entry* e = lookup_entry(pm->key[k]);
            if (e && e->text[0]) add_part(out, 2048, e->text, &count);
        }
    }
    if (contains_ci(g_focusPath, "MoveListBaseLayer.Elements.GridSelectInterface.Cell_")) movelist_frames(out, 2048);
    if (out[0]) say(out, 1);
}

static void auto_read(DWORD now)
{
    if (!g_autoRead || g_focusPending) return;
    static wchar out[2048]; out[0] = 0; int count = 0;
    static Entry* cand[64]; int nc = 0;
    int start = g_nrecent > NRECENT ? g_nrecent - NRECENT : 0;
    for (int k = start; k < g_nrecent; k++) {   /* wait until text settles */
        int s = k % NRECENT;
        if (g_recentSpoken[s]) continue;
        Entry* e = &g_ent[g_recent[s]];
        /* a ticking clock or counter must not hold everything else back */
        if (e->gen != g_recentGen[s] || is_busy(e, now) || (no_letters(e->text) && !is_price(e->path))) { g_recentSpoken[s] = 1; continue; }
        if (now - g_recentT[s] < 250) return;
    }
    for (int k = start; k < g_nrecent; k++) {
        int s = k % NRECENT;
        if (g_recentSpoken[s]) continue;
        g_recentSpoken[s] = 1;
        Entry* e = &g_ent[g_recent[s]];
        if (e->gen != g_recentGen[s] || is_busy(e, now) || !e->text[0] || (no_letters(e->text) && !is_price(e->path))) continue;
        if (ends_with_colon(e->text)) continue;   /* a label waiting for its value, e.g. "WINS:" */
        if (contains_ci(e->path, "PlayerListGridSelectElement.Cell_") || contains_ci(e->path, "RoomListGridSelectElement.Cell_")) continue;   /* read on focus */
        if (now - g_lastTime < 3000 && wlen(e->text) >= 8 && contains_w(g_last, e->text)) continue;   /* just said it */
        int dup = 0; for (int i = 0; i < nc; i++) if (cand[i] == e) dup = 1;   /* only its latest value */
        if (!dup && nc < 64) cand[nc++] = e;
    }
    /* many texts at once = a whole screen loaded: read only its title / message */
    for (int i = 0; i < nc; i++) {
        Entry* e = cand[i];
        int titleish = is_titleish(e->path) || is_prompt(e->path) || is_price(e->path);
        if (nc > 6 && !titleish) continue;
        if (count < 8) add_part(out, 2048, e->text, &count);
        /* a popup question: also read its choices, even if they are the same as last time */
        if (contains_ci(e->path, "msgBox") || contains_ci(e->path, "popup")) {
            static wchar root[PATHMAX]; int d = 0, k = 0;
            for (; e->path[k] && k < PATHMAX - 1; k++) { if (e->path[k] == '.' && ++d == 2) break; root[k] = e->path[k]; }
            root[k] = 0;
            Entry* opt[8]; int no = 0;
            int question = wlen(e->text) > 0 && e->text[wlen(e->text) - 1] == '?';
            for (int pass = 0; pass < (question ? 2 : 1) && no == 0; pass++)   /* pass 1: a question whose choices weren't re-sent */
                for (int j = 0; j < NENT && no < 8; j++) {
                    Entry* o = &g_ent[j];
                    if (o == e || !o->used || !o->text[0] || !is_child_path(o->path, root)) continue;
                    if (pass == 0 && ((int)(o->touched - e->touched) < -400 || (int)(o->touched - e->touched) > 400)) continue;
                    if (pass == 1 && !contains_ci(o->path, "option")) continue;
                    if (ends_with_colon(o->text)) continue;
                    opt[no++] = o;
                }
            for (int a = 1; a < no; a++) for (int b = a; b > 0 && path_less(opt[b]->path, opt[b-1]->path); b--) { Entry* t = opt[b]; opt[b] = opt[b-1]; opt[b-1] = t; }
            for (int a = 0; a < no; a++) add_part(out, 2048, opt[a]->text, &count);
        }
    }
    if (out[0]) say(out, 0);
}

static void read_screen(void)
{
    static wchar out[4096]; out[0] = 0; int count = 0;
    /* screen root = first two parts of the focus path */
    wchar root[PATHMAX]; root[0] = 0;
    int dots = 0, i = 0;
    for (; g_focusPath[i] && i < PATHMAX - 1; i++) { if (g_focusPath[i] == '.' && ++dots == 2) break; root[i] = g_focusPath[i]; }
    root[i] = 0;
    DWORD now = GetTickCount();
    for (int k = 0; k < NENT && count < 25; k++) {
        Entry* e = &g_ent[k];
        if (!e->used || !e->text[0] || contains_ci(e->path, "ButtonLogs") || contains_ci(e->path, "versionNumber")) continue;
        if (now - e->changed > 120000 && !(root[0] && is_child_path(e->path, root))) continue;
        add_part(out, 4096, e->text, &count);
    }
    say(out[0] ? out : (const wchar*)L"No text found", 1);
}

static void handle_focus(Event* ev)
{
    if (g_wallQuiet != 2) { g_wallQuiet = 1; g_quietPosSet = 0; }
    if (g_padMode) { g_padMode = 0; log_w((const wchar*)L"INVITE controller mode off"); }   /* back in a menu */
    const wchar* tag = ev->type == EV_FOCUS ? (const wchar*)L"FOCUS" : ev->type == EV_SETFOCUS ? (const wchar*)L"SETFOCUS" :
                      ev->type == EV_ANIM ? (const wchar*)L"ANIMFOCUS" : (const wchar*)L"GRID";
    wchar extra[PATHMAX + 40]; extra[0] = 0;
    if (ev->type == EV_GRID) {
        wchar n[16]; wcpy(extra, (const wchar*)L"x=", 40); utoa_w(ev->x, n, 10, 1); wcat(extra, n, 40);
        wcat(extra, (const wchar*)L" y=", 40); utoa_w(ev->y, n, 10, 1); wcat(extra, n, 40);
    } else if (ev->type == EV_SETFOCUS) {
        wcpy(extra, (const wchar*)L"from=", PATHMAX + 40); wcat(extra, ev->text, PATHMAX + 40);
    }
    logf3(tag, ev->path[0] ? ev->path : (const wchar*)L"?", extra);
    if (!ev->path[0]) return;
    if (ev->type == EV_GRID) { g_gridX = ev->x; g_gridY = ev->y; }
    else g_gridX = g_gridY = -1;
    if (g_focusPending && weq(g_focusPath, ev->path)) return;
    if (!g_focusPending && weq(g_lastFocus, ev->path) && ev->t - g_lastFocusT < 1000) return;   /* same item re-highlighted */
    wcpy(g_lastFocus, ev->path, PATHMAX); g_lastFocusT = ev->t;
    wcpy(g_focusPath, ev->path, PATHMAX);
    g_focusTime = ev->t;
    g_focusPending = 1;
    g_valEntry = 0;   /* the focus announcement will include it */
}

/* ------------------------------------------------------------------ */
/* accessible Steam invites                                            */
/* Private matches and private King of the Hill open Steam's overlay invite
   window through ISteamFriends::ActivateGameOverlayInviteDialog(lobby). That
   window is drawn by Steam's overlay and has no text the mod or NVDA can read.
   We patch that one slot of the ISteamFriends vtable: the lobby ID is kept, the
   overlay is not opened (unless NVDA isn't running), and F11 / Shift+F11 /
   Ctrl+F11 pick a friend and send the invite with
   ISteamMatchmaking::InviteUserToLobby. The friend gets a normal Steam invite.

   Interface versions in this game's steam_api64.dll: SteamFriends014 and
   SteamMatchMaking009. vtable slots, confirmed against the game's own calls:
     Friends:     3 GetFriendCount(flags)  4 GetFriendByIndex(ret*, i, flags)
                  6 GetFriendPersonaState(id)  7 GetFriendPersonaName(id)
                  8 GetFriendGamePlayed(id, FriendGameInfo_t*)
                  27 ActivateGameOverlayInviteDialog(lobby)
     Matchmaking: 16 InviteUserToLobby(lobby, invitee)  17 GetNumLobbyMembers(lobby) */
#define MKX_APPID 307780
#define FRIEND_FLAG_IMMEDIATE 4
typedef int   (*sfGetFriendCount_t)(void*, int);
typedef u64*  (*sfGetFriendByIndex_t)(void*, u64*, int, int);
typedef int   (*sfGetPersonaState_t)(void*, u64);
typedef const char* (*sfGetPersonaName_t)(void*, u64);
typedef BYTE  (*sfGetGamePlayed_t)(void*, u64, void*);
typedef void  (*sfInviteDialog_t)(void*, u64);
typedef BYTE  (*smInviteUser_t)(void*, u64, u64);
typedef int   (*smNumMembers_t)(void*, u64);
typedef void* (*SteamIface_t)(void);

static void* g_sFriends; static void* g_sMatch;
static sfInviteDialog_t o_inviteDialog;
static volatile u64 g_inviteLobby;
static int g_inviteHooked, g_nvOk;
typedef struct { u64 id; wchar name[64]; int playing, state; } Friend;
static Friend g_friends[200]; static int g_nfriends, g_friendIdx = -1;

static void* vfn(void* iface, int slot) { return (*(void***)iface)[slot]; }

static void hk_inviteDialog(void* self, u64 lobby)
{
    g_inviteLobby = lobby;
    Event* e = q_begin();
    if (e) { e->type = EV_INVITE; e->id = lobby; q_end(); }
    if (!g_nvOk && nv_test) g_nvOk = nv_test() == 0;                 /* NVDA may have started after the game */
    if (!g_nvOk && o_inviteDialog) o_inviteDialog(self, lobby);   /* no NVDA: keep Steam's own window */
}

typedef BYTE (*suGamepadText_t)(void*, int, int, const char*, DWORD, const char*);
static suGamepadText_t o_gamepadText;
static BYTE hk_gamepadText(void* self, int mode, int lines, const char* desc, DWORD maxChars, const char* existing)
{
    if (!g_nvOk && nv_test) g_nvOk = nv_test() == 0;
    if (!g_nvOk && o_gamepadText) return o_gamepadText(self, mode, lines, desc, maxChars, existing);
    return 0;   /* "not shown": the game opens its own readable text box */
}

/* patch the vtable slot once Steam is up (the game starts Steam after we load) */
static void steam_try_hook(void)
{
    if (g_inviteHooked) return;
    HMODULE sa = GetModuleHandleW((const wchar*)L"steam_api64.dll");
    if (!sa) return;
    SteamIface_t friendsFn = (SteamIface_t)GetProcAddress(sa, "SteamFriends");
    SteamIface_t matchFn = (SteamIface_t)GetProcAddress(sa, "SteamMatchmaking");
    if (!friendsFn || !matchFn) { g_inviteHooked = -1; return; }
    void* f = friendsFn(); void* m = matchFn();
    if (!f || !m || !readable(f, 8) || !readable(*(void**)f, 32 * 8)) return;   /* Steam not started yet */
    g_sFriends = f; g_sMatch = m;
    void** slot = &(*(void***)f)[27];
    DWORD old;
    if (!VirtualProtect(slot, 8, 0x04, &old)) { g_inviteHooked = -1; return; }
    o_inviteDialog = (sfInviteDialog_t)*slot;
    *slot = (void*)hk_inviteDialog;
    VirtualProtect(slot, 8, old, &old);
    g_inviteHooked = 1;
    log_w((const wchar*)L"STEAM invite window replaced (SteamFriends slot 27)");
    /* Steam's on-screen keyboard (Big Picture) can't be read either: TextBox::Initialize
       asks ISteamUtils::ShowGamepadTextInput (slot 20) first and only uses the game's
       own text box when Steam says no. With NVDA running, Steam says no. */
    SteamIface_t utilsFn = (SteamIface_t)GetProcAddress(sa, "SteamUtils");
    void* u = utilsFn ? utilsFn() : 0;
    if (u && readable(u, 8) && readable(*(void**)u, 21 * 8)) {
        void** us = &(*(void***)u)[20];
        if (VirtualProtect(us, 8, 0x04, &old)) {
            o_gamepadText = (suGamepadText_t)*us;
            *us = (void*)hk_gamepadText;
            VirtualProtect(us, 8, old, &old);
            log_w((const wchar*)L"STEAM on-screen keyboard replaced by the game's text box (SteamUtils slot 20)");
        }
    }
}

static const wchar* state_word(int st)
{
    switch (st) {
    case 1: return (const wchar*)L"online";  case 2: return (const wchar*)L"busy";
    case 3: return (const wchar*)L"away";    case 4: return (const wchar*)L"snooze";
    case 5: return (const wchar*)L"looking to trade"; case 6: return (const wchar*)L"looking to play";
    default: return (const wchar*)L"offline";
    }
}

/* online friends, those playing Mortal Kombat X first */
static void steam_load_friends(void)
{
    g_nfriends = 0; g_friendIdx = -1;
    if (!g_sFriends) return;
    int n = ((sfGetFriendCount_t)vfn(g_sFriends, 3))(g_sFriends, FRIEND_FLAG_IMMEDIATE);
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < n && g_nfriends < 200; i++) {
            u64 id = 0;
            ((sfGetFriendByIndex_t)vfn(g_sFriends, 4))(g_sFriends, &id, i, FRIEND_FLAG_IMMEDIATE);
            if (!id) continue;
            int st = ((sfGetPersonaState_t)vfn(g_sFriends, 6))(g_sFriends, id);
            if (st == 0) continue;   /* offline friends can't join */
            struct { u64 game; DWORD ip; WORD port, qport; u64 lobby; } gi;
            memset(&gi, 0, sizeof(gi));
            int playing = ((sfGetGamePlayed_t)vfn(g_sFriends, 8))(g_sFriends, id, &gi) && (gi.game & 0xFFFFFF) == MKX_APPID;
            if (playing != (pass == 0)) continue;
            Friend* fr = &g_friends[g_nfriends++];
            fr->id = id; fr->playing = playing; fr->state = st;
            const char* nm = ((sfGetPersonaName_t)vfn(g_sFriends, 7))(g_sFriends, id);
            int k = nm ? MultiByteToWideChar(CP_UTF8, 0, nm, -1, fr->name, 63) : 0;
            fr->name[k > 0 ? k - 1 : 0] = 0; fr->name[63] = 0;
            if (!fr->name[0]) wcpy(fr->name, (const wchar*)L"unknown name", 64);
        }
}

static void steam_say_friend(void)
{
    if (!g_nfriends) { speak_force((const wchar*)L"None of your Steam friends are online"); return; }
    Friend* fr = &g_friends[g_friendIdx];
    wchar line[160], num[12];
    wcpy(line, fr->name, 160);
    wcat(line, fr->playing ? (const wchar*)L", playing Mortal Kombat X" : (const wchar*)L", ", 160);
    if (!fr->playing) wcat(line, state_word(fr->state), 160);
    wcat(line, (const wchar*)L", ", 160); utoa_w(g_friendIdx + 1, num, 10, 1); wcat(line, num, 160);
    wcat(line, (const wchar*)L" of ", 160); utoa_w(g_nfriends, num, 10, 1); wcat(line, num, 160);
    speak_force(line);
}

static void steam_invite_requested(Event* ev)
{
    wchar num[24]; utoa_w(ev->id, num, 16, 1);
    logf3((const wchar*)L"INVITE-WINDOW", num, 0);
    steam_load_friends();
    logf3((const wchar*)L"STEAM friends online", 0, 0);
    /* queued, so the game's own "INVITE PLAYERS" / "WAITING FOR OPPONENT" isn't cut off */
    g_padMode = g_padHooked;
    speak(g_nfriends ? (const wchar*)L"Invite a friend. D-pad down or F11 for the next friend, D-pad up or Shift F11 for the previous one, A or Control F11 to send the invite."
                     : (const wchar*)L"Invite a friend. None of your Steam friends are online right now. Press D-pad down or F11 to check again.", 0);
}

static void steam_accept_invite(void);
static u64 g_rxLobby;
static void steam_invite_key(int shift, int ctrl)
{
    if (g_inviteHooked != 1) { speak_force((const wchar*)L"Steam invites aren't available"); return; }
    if (ctrl && g_rxLobby && !g_padMode) { steam_accept_invite(); return; }   /* someone invited us */
    if (ctrl) {
        if (!g_inviteLobby) { speak_force((const wchar*)L"No match to invite to yet. Create a private match first."); return; }
        if (g_friendIdx < 0 || g_friendIdx >= g_nfriends) { speak_force((const wchar*)L"Choose a friend first with F11"); return; }
        if (g_sMatch && ((smNumMembers_t)vfn(g_sMatch, 17))(g_sMatch, g_inviteLobby) <= 0) {
            speak_force((const wchar*)L"That match is no longer open. Create a new private match, then try again.");
            return;
        }
        Friend* fr = &g_friends[g_friendIdx];
        BYTE ok = g_sMatch ? ((smInviteUser_t)vfn(g_sMatch, 16))(g_sMatch, g_inviteLobby, fr->id) : 0;
        wchar line[128];
        wcpy(line, ok ? (const wchar*)L"Invite sent to " : (const wchar*)L"Could not send the invite to ", 128);
        wcat(line, fr->name, 128);
        logf3((const wchar*)L"INVITE", fr->name, ok ? (const wchar*)L"sent" : (const wchar*)L"failed");
        speak_force(line);
        return;
    }
    if (!g_nfriends || g_friendIdx < 0) steam_load_friends();   /* first press: fresh list */
    if (!g_nfriends) { steam_say_friend(); return; }
    if (g_friendIdx < 0) g_friendIdx = shift ? g_nfriends - 1 : 0;
    else g_friendIdx = (g_friendIdx + (shift ? g_nfriends - 1 : 1)) % g_nfriends;
    steam_say_friend();
}

/* receiving invites: Steam calls back with LobbyInvite_t (503) when a friend
   invites us. Accepting does what Steam's own accept does: the game's
   GameLobbyJoinRequested handler only stores the lobby in GLobbyToJoin, and
   OnlineWin32Platform::DoWork joins it once online. CCallbackBase layout on
   Windows (matches the game's own CCallback vtables): vtbl[0] Run(param, ioFailure,
   apiCall), vtbl[1] Run(param), vtbl[2] GetCallbackSizeBytes; flags byte at +8,
   callback id at +12. Registered from the game thread. */
#define CB_LOBBY_INVITE 503
#define GLOBBY_TO_JOIN 0x14331FF90ULL
typedef struct { u64 user, lobby, game; } LobbyInvite;
typedef void (*SteamRegCb_t)(void*, int);
static void cb_got_invite(LobbyInvite* li)
{
    if (!li) return;
    Event* e = q_begin();
    if (e) { e->type = EV_LOBBYINVITE; e->id = li->lobby; e->x = (int)(DWORD)li->user; e->y = (int)(DWORD)(li->user >> 32);
             e->text[0] = (wchar)((li->game & 0xFFFFFF) == MKX_APPID); q_end(); }
}
static void cb_run3(void* self, void* param, BYTE io, u64 call) { cb_got_invite((LobbyInvite*)param); }
static void cb_run1(void* self, void* param) { cb_got_invite((LobbyInvite*)param); }
static int  cb_size(void* self) { return (int)sizeof(LobbyInvite); }
static void* g_cbVtbl[3] = { (void*)cb_run3, (void*)cb_run1, (void*)cb_size };
static struct { void** vtbl; BYTE flags; BYTE pad[3]; int iCallback; } g_cbInvite = { g_cbVtbl, 0, {0,0,0}, 0 };
static volatile int g_cbRegistered;

static void steam_register_callbacks(void)
{
    if (g_cbRegistered || g_inviteHooked != 1) return;   /* Steam must be up first */
    g_cbRegistered = 1;
    HMODULE sa = GetModuleHandleW((const wchar*)L"steam_api64.dll");
    SteamRegCb_t reg = sa ? (SteamRegCb_t)GetProcAddress(sa, "SteamAPI_RegisterCallback") : 0;
    if (reg) { reg(&g_cbInvite, CB_LOBBY_INVITE); g_cbRegistered = 2; }
}

static u64 g_rxFrom; static DWORD g_rxT; static wchar g_rxName[64];
#define INVITE_EXPIRES 120000

static void steam_invite_received(Event* ev)
{
    u64 from = (u64)(DWORD)ev->x | ((u64)(DWORD)ev->y << 32);
    wchar num[24]; utoa_w(ev->id, num, 16, 1);
    logf3((const wchar*)L"INVITE-RECEIVED", num, ev->text[0] ? (const wchar*)L"Mortal Kombat X" : (const wchar*)L"other game");
    if (!ev->text[0] || !ev->id) return;   /* an invite for another game */
    g_rxLobby = ev->id; g_rxFrom = from; g_rxT = ev->t;
    g_rxName[0] = 0;
    if (g_sFriends) {
        const char* nm = ((sfGetPersonaName_t)vfn(g_sFriends, 7))(g_sFriends, from);
        int k = nm ? MultiByteToWideChar(CP_UTF8, 0, nm, -1, g_rxName, 63) : 0;
        g_rxName[k > 0 ? k - 1 : 0] = 0; g_rxName[63] = 0;
    }
    if (!g_rxName[0]) wcpy(g_rxName, (const wchar*)L"A friend", 64);
    g_acceptMode = g_padHooked;
    wchar line[200];
    wcpy(line, g_rxName, 200);
    wcat(line, (const wchar*)L" invited you to a match. Press Back on the controller, or Control F11, to accept.", 200);
    speak_force(line);
}

static void steam_accept_invite(void)
{
    if (!g_rxLobby) { speak_force((const wchar*)L"No invite to accept"); return; }
    *(volatile u64*)(g_base + (GLOBBY_TO_JOIN - IMAGE_BASE)) = g_rxLobby;   /* the game joins it */
    wchar line[160];
    wcpy(line, (const wchar*)L"Joining ", 160); wcat(line, g_rxName, 160); wcat(line, (const wchar*)L"'s match", 160);
    logf3((const wchar*)L"INVITE-ACCEPTED", g_rxName, 0);
    speak_force(line);
    g_rxLobby = 0; g_acceptMode = 0;
}

static void steam_invite_tick(DWORD now)
{
    if (g_rxLobby && now - g_rxT > INVITE_EXPIRES) { g_rxLobby = 0; g_acceptMode = 0; log_w((const wchar*)L"INVITE-RECEIVED expired"); }
}

/* menu speech: heard even while the mod is muted, and F5 repeats it */
static void speak_force_q(const wchar* s, int interrupt)
{
    logf3((const wchar*)L"SPEAK", 0, s);
    wcpy(g_last, s, 2048); g_lastTime = GetTickCount();
    if (!nv_speak) return;
    if (interrupt && nv_cancel) nv_cancel();
    nv_speak(s);
}

/* ------------------------------------------------------------------ */
/* sounds: .wav files in the "MKXAccess Sounds" folder next to the mod  */
/* Each cue is one file that anyone can replace with their own (16-bit PCM .wav plays
   at the volume set in the menu; other kinds play at their own volume). A missing
   file just stays silent. Played with Windows' PlaySound from memory, one at a time. */
typedef int (WINAPI *PlaySoundW_t)(const void*, HMODULE, DWORD);
static PlaySoundW_t p_PlaySound;
typedef struct { const char* name; int tried; BYTE* raw; DWORD len; DWORD dataOff, dataLen; int pcm16, chans, rate; } Sound;
static Sound g_snd[] = {
    {"you75"}, {"you50"}, {"you25"}, {"opponent75"}, {"opponent50"}, {"opponent25"},
    {"menu_open"}, {"menu_close"}, {"menu_move"}, {"menu_edge"}, {"xray_you"}, {"xray_opponent"},
    {"wall_loop"},
};
#define NSND (int)(sizeof(g_snd) / sizeof(g_snd[0]))
static int g_volume = 80;   /* percent: everything the mod plays */
/* each kind of sound has its own volume (0 = off), set in the menu */
enum { C_HEALTH_YOU, C_HEALTH_OPP, C_XRAY_YOU, C_XRAY_OPP, C_HUM, C_MENU, NCAT };
static int g_cv[NCAT] = { -1, -1, -1, -1, -1, -1 };   /* -1 until loaded: then 100 or the old on/off */
static int g_stereo = 1;    /* health and X-ray sounds come from the side of that fighter's health bar (always) */
static void wcat_u(wchar* d, u64 v, int max) { wchar num[24]; utoa_w(v, num, 10, 1); wcat(d, num, max); }
static unsigned rd32(const BYTE* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24); }
static void wr32(BYTE* p, unsigned v) { p[0] = (BYTE)v; p[1] = (BYTE)(v >> 8); p[2] = (BYTE)(v >> 16); p[3] = (BYTE)(v >> 24); }
static void wr16(BYTE* p, unsigned v) { p[0] = (BYTE)v; p[1] = (BYTE)(v >> 8); }
static void snd_load(Sound* s)
{
    s->tried = 1;
    wchar path[MAX_PATH]; wcpy(path, g_dir, MAX_PATH);
    wcat(path, (const wchar*)L"MKXAccess Sounds\\", MAX_PATH); wcat_a(path, s->name, MAX_PATH); wcat(path, (const wchar*)L".wav", MAX_PATH);
    HANDLE f = CreateFileW(path, 0x80000000 /* GENERIC_READ */, 1 /* share read */, 0, 3 /* OPEN_EXISTING */, 0x80, 0);
    if (f == INVALID_HANDLE_VALUE || !f) return;
    DWORD len = GetFileSize(f, 0), got = 0;
    if (len >= 44 && len <= 8u * 1024 * 1024) {
        s->raw = (BYTE*)VirtualAlloc(0, len, MEM_COMMIT | MEM_RESERVE, 0x04);
        if (s->raw && ReadFile(f, s->raw, len, &got, 0) && got == len) s->len = len;
    }
    CloseHandle(f);
    if (!s->len || rd32(s->raw) != 0x46464952 /* RIFF */ || rd32(s->raw + 8) != 0x45564157 /* WAVE */) { s->len = 0; return; }
    for (DWORD o = 12; o + 8 <= s->len; ) {
        unsigned id = rd32(s->raw + o), sz = rd32(s->raw + o + 4);
        if (id == 0x20746D66 /* fmt  */ && sz >= 16 && o + 24 <= s->len) {
            s->chans = s->raw[o + 10] | (s->raw[o + 11] << 8);
            s->rate = (int)rd32(s->raw + o + 12);
            s->pcm16 = (s->raw[o + 8] | (s->raw[o + 9] << 8)) == 1 && (s->raw[o + 22] | (s->raw[o + 23] << 8)) == 16 && (s->chans == 1 || s->chans == 2) && s->rate >= 4000 && s->rate <= 192000;
        }
        if (id == 0x61746164 /* data */) { s->dataOff = o + 8; s->dataLen = sz; if (s->dataOff + s->dataLen > s->len) s->dataLen = s->len - s->dataOff; }
        o += 8 + sz + (sz & 1);
    }
}
/* the mixer: one stereo 44.1 kHz stream (waveOut) on its own thread, so sounds can
   overlap and a steady sound can change its volume while it plays. 16-bit .wav files of
   any rate, mono or stereo, go through it; anything else falls back to PlaySound. */
typedef struct { char* data; DWORD len, rec; u64 user; DWORD flags, loops; void* next; u64 res; } WaveHdr;
typedef UINT (WINAPI *wOpen_t)(void**, UINT, const void*, u64, u64, DWORD);
typedef UINT (WINAPI *wHdr_t)(void*, WaveHdr*, UINT);
static wHdr_t p_wPrepare, p_wWrite;
static void* g_wave; static HANDLE g_waveEvent; static CRITICAL_SECTION g_mixLock; static int g_mixOk;
#define MIX_RATE 44100
#define MIX_FRAMES 1024          /* 23 ms a buffer, 4 buffers */
typedef struct { Sound* s; u64 pos, step; int gl, gr; } Voice;   /* gains in 1/256 */
static Voice g_voice[12];
typedef struct { Sound* s; u64 pos, step; float vol, target; int pan; } Loop;   /* steady sounds */
static Loop g_loop[2];
static short g_mixBuf[4][MIX_FRAMES * 2]; static WaveHdr g_hdr[4];
static int sample_at(Sound* s, u64 pos, int ch)   /* pos: 32.32 frames */
{
    DWORD frames = s->dataLen / (2 * s->chans), i = (DWORD)(pos >> 32);
    if (i >= frames) return 0;
    const short* d = (const short*)(s->raw + s->dataOff);
    return d[i * s->chans + (s->chans == 2 ? ch : 0)];
}
static void mix_fill(short* out)
{
    static int acc[MIX_FRAMES * 2];
    memset(acc, 0, sizeof(acc));
    EnterCriticalSection(&g_mixLock);
    int master = g_muted ? 0 : g_volume * 256 / 100;
    for (int v = 0; v < 12; v++) {
        Voice* x = &g_voice[v]; if (!x->s) continue;
        DWORD frames = x->s->dataLen / (2 * x->s->chans);
        for (int i = 0; i < MIX_FRAMES; i++) {
            if ((x->pos >> 32) >= frames) { x->s = 0; break; }
            int l = sample_at(x->s, x->pos, 0), r = x->s->chans == 2 ? sample_at(x->s, x->pos, 1) : l;
            if (x->s->chans == 2 && x->gl != x->gr) l = r = (l + r) / 2;   /* placed: one voice */
            acc[i * 2] += l * x->gl >> 8; acc[i * 2 + 1] += r * x->gr >> 8;
            x->pos += x->step;
        }
    }
    for (int k = 0; k < 2; k++) {
        Loop* x = &g_loop[k]; if (!x->s) continue;
        DWORD frames = x->s->dataLen / (2 * x->s->chans);
        if (!frames) continue;
        float step = (x->target - x->vol) / MIX_FRAMES;   /* glide to the new volume: no clicks */
        for (int i = 0; i < MIX_FRAMES; i++) {
            x->vol += step;
            if ((x->pos >> 32) >= frames) x->pos -= (u64)frames << 32;
            int m = sample_at(x->s, x->pos, 0);
            if (x->s->chans == 2) m = (m + sample_at(x->s, x->pos, 1)) / 2;
            float g = x->vol, gl = x->pan < 0 ? g : x->pan > 0 ? g * 0.18f : g * 0.85f, gr = x->pan > 0 ? g : x->pan < 0 ? g * 0.18f : g * 0.85f;
            acc[i * 2] += (int)(m * gl); acc[i * 2 + 1] += (int)(m * gr);
            x->pos += x->step;
        }
        x->vol = x->target;
    }
    LeaveCriticalSection(&g_mixLock);
    for (int i = 0; i < MIX_FRAMES * 2; i++) {
        int v = acc[i] * master >> 8;
        out[i] = (short)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
}
static DWORD WINAPI mix_thread(void* arg)
{
    for (int b = 0; b < 4; b++) {
        g_hdr[b].data = (char*)g_mixBuf[b]; g_hdr[b].len = sizeof(g_mixBuf[b]);
        mix_fill(g_mixBuf[b]);
        p_wPrepare(g_wave, &g_hdr[b], sizeof(WaveHdr)); p_wWrite(g_wave, &g_hdr[b], sizeof(WaveHdr));
    }
    for (;;) {
        WaitForSingleObject(g_waveEvent, 100);
        for (int b = 0; b < 4; b++)
            if (g_hdr[b].flags & 1 /* WHDR_DONE */) { g_hdr[b].flags &= ~1u; mix_fill(g_mixBuf[b]); p_wWrite(g_wave, &g_hdr[b], sizeof(WaveHdr)); }
    }
    return 0;
}
static void mix_start(void)
{
    static int tried; if (tried) return; tried = 1;
    HMODULE w = LoadLibraryW((const wchar*)L"winmm.dll");
    if (!w) return;
    p_PlaySound = (PlaySoundW_t)GetProcAddress(w, "PlaySoundW");
    wOpen_t open = (wOpen_t)GetProcAddress(w, "waveOutOpen");
    p_wPrepare = (wHdr_t)GetProcAddress(w, "waveOutPrepareHeader"); p_wWrite = (wHdr_t)GetProcAddress(w, "waveOutWrite");
    if (!open || !p_wPrepare || !p_wWrite) return;
    InitializeCriticalSection(&g_mixLock);
    g_waveEvent = CreateEventW(0, 0, 0, 0);
    BYTE fmt[18] = {0};
    wr16(fmt, 1); wr16(fmt + 2, 2); wr32(fmt + 4, MIX_RATE); wr32(fmt + 8, MIX_RATE * 4); wr16(fmt + 12, 4); wr16(fmt + 14, 16);
    if (open(&g_wave, 0xFFFFFFFF /* WAVE_MAPPER */, fmt, (u64)g_waveEvent, 0, 0x50000 /* CALLBACK_EVENT */) != 0) { log_w((const wchar*)L"SOUND waveOut failed - one sound at a time"); return; }
    g_mixOk = 1;
    CreateThread(0, 0, mix_thread, 0, 0, 0);
    log_w((const wchar*)L"SOUND mixer started");
}
static Sound* snd_get(const char* name)
{
    for (int i = 0; i < NSND; i++) {
        const char* a = g_snd[i].name; int k = 0; while (a[k] && a[k] == name[k]) k++;
        if (!a[k] && !name[k]) { if (!g_snd[i].tried) snd_load(&g_snd[i]); return g_snd[i].len ? &g_snd[i] : 0; }
    }
    return 0;
}
/* pan: -1 left, 0 middle, 1 right: loud on its side and soft on the other, so it still
   sounds natural */
static void snd_play_pan(const char* name, int pan, int cat)
{
    if (g_muted || !g_base) return;
    int cv = cat >= 0 && cat < NCAT ? g_cv[cat] : 100;
    if (cv <= 0) return;
    mix_start();
    Sound* s = snd_get(name);
    if (!s || g_volume <= 0) return;
    if (!g_mixOk || !s->pcm16) { if (p_PlaySound) p_PlaySound(s->raw, 0, 0x0001 | 0x0002 | 0x0004); return; }
    int gl = pan < 0 ? 256 : pan > 0 ? 46 : 218, gr = pan > 0 ? 256 : pan < 0 ? 46 : 218;
    gl = gl * cv / 100; gr = gr * cv / 100;
    EnterCriticalSection(&g_mixLock);
    int slot = 0; for (int v = 0; v < 12; v++) if (!g_voice[v].s) { slot = v; break; } else if (g_voice[v].pos > g_voice[slot].pos) slot = v;   /* else replace the oldest */
    g_voice[slot].s = s; g_voice[slot].pos = 0; g_voice[slot].step = ((u64)s->rate << 32) / MIX_RATE; g_voice[slot].gl = gl; g_voice[slot].gr = gr;
    LeaveCriticalSection(&g_mixLock);
}
/* a steady sound for slot k (0/1): volume 0..1 (0 stops it after a short fade), pan */
static void snd_loop(int k, const char* name, float vol, int pan)
{
    if (!g_base) return;
    mix_start();
    if (!g_mixOk) return;
    Sound* s = vol > 0 ? snd_get(name) : g_loop[k].s;
    if (vol > 0 && (!s || !s->pcm16)) return;
    EnterCriticalSection(&g_mixLock);
    Loop* x = &g_loop[k];
    if (vol > 0 && x->s != s) { x->s = s; x->pos = 0; x->vol = 0; x->step = ((u64)s->rate << 32) / MIX_RATE; }
    x->target = vol; x->pan = pan;
    if (vol <= 0 && x->vol <= 0.001f) x->s = 0;
    LeaveCriticalSection(&g_mixLock);
}
static void snd_play(const char* name) { snd_play_pan(name, 0, C_MENU); }

/* ------------------------------------------------------------------ */
/* settings, kept in MKXAccess.ini next to the mod                      */
static int g_side = 0;   /* always 0: who you are is worked out each fight */
/* the on/off switches of 0.9.0-0.9.7, only read to carry them over to the volumes */
static int g_oldHealthYou = -1, g_oldHealthOpp = -1, g_oldXrayYou = -1, g_oldXrayOpp = -1, g_oldWallYou = -1, g_oldWallOpp = -1;
static void settings_path(wchar* p) { wcpy(p, g_dir, MAX_PATH); wcat(p, (const wchar*)L"MKXAccess.ini", MAX_PATH); }
static struct { const char* key; int* v; int lo, hi; int save; } g_setDefs[] = {
    {"volume", &g_volume, 0, 100, 1},
    {"sound_health_you", &g_cv[C_HEALTH_YOU], 0, 100, 1}, {"sound_health_opponent", &g_cv[C_HEALTH_OPP], 0, 100, 1},
    {"sound_xray_you", &g_cv[C_XRAY_YOU], 0, 100, 1}, {"sound_xray_opponent", &g_cv[C_XRAY_OPP], 0, 100, 1},
    {"sound_wall_hum", &g_cv[C_HUM], 0, 100, 1}, {"sound_menu", &g_cv[C_MENU], 0, 100, 1},
    {"combo_readout", &g_comboReadout, 0, 1, 1}, {"damage_numbers", &g_damageNumbers, 0, 1, 1}, {"auto_read", &g_autoRead, 0, 1, 1},
    {"health_you", &g_oldHealthYou, 0, 1, 0}, {"health_opponent", &g_oldHealthOpp, 0, 1, 0}, {"xray_you", &g_oldXrayYou, 0, 1, 0},
    {"xray_opponent", &g_oldXrayOpp, 0, 1, 0}, {"wall_you", &g_oldWallYou, 0, 1, 0}, {"wall_opponent", &g_oldWallOpp, 0, 1, 0},
};
/* sounds without a saved volume: 100, or off if they were switched off before */
static void settings_defaults(void)
{
    int* old[NCAT] = { &g_oldHealthYou, &g_oldHealthOpp, &g_oldXrayYou, &g_oldXrayOpp, 0, 0 };
    for (int c = 0; c < NCAT; c++) if (g_cv[c] < 0) g_cv[c] = old[c] && *old[c] == 0 ? 0 : 100;
    if (g_cv[C_HUM] < 0) g_cv[C_HUM] = 100;
    if (g_oldWallYou == 0 && g_oldWallOpp == 0 && g_cv[C_HUM] == 100) g_cv[C_HUM] = 0;
}
#define NSET (int)(sizeof(g_setDefs) / sizeof(g_setDefs[0]))
static void settings_load(void)
{
    wchar p[MAX_PATH]; settings_path(p);
    HANDLE f = CreateFileW(p, 0x80000000, 1, 0, 3, 0x80, 0);
    if (f == INVALID_HANDLE_VALUE || !f) { settings_defaults(); return; }
    static char buf[4096]; DWORD got = 0;
    ReadFile(f, buf, sizeof(buf) - 1, &got, 0); CloseHandle(f);
    buf[got] = 0;
    for (char* line = buf; *line; ) {
        char* end = line; while (*end && *end != '\n') end++;
        for (int i = 0; i < NSET; i++) {
            const char* k = g_setDefs[i].key; int n = 0; while (k[n] && line[n] == k[n]) n++;
            if (k[n] || line[n] != '=') continue;
            int v = 0, any = 0; for (char* c = line + n + 1; c < end && *c >= '0' && *c <= '9'; c++) { v = v * 10 + (*c - '0'); any = 1; }
            if (any && v >= g_setDefs[i].lo && v <= g_setDefs[i].hi) *g_setDefs[i].v = v;
        }
        line = *end ? end + 1 : end;
    }
    settings_defaults();
    log_w((const wchar*)L"SETTINGS loaded from MKXAccess.ini");
}
static void settings_save(void)
{
    if (!g_base) return;
    static char buf[1024]; int n = 0;
    const char* head = "; MKX Access settings - changed from the accessibility menu (F3, or hold Back and press Y)\r\n";
    for (int i = 0; head[i]; i++) buf[n++] = head[i];
    for (int i = 0; i < NSET; i++) {
        if (!g_setDefs[i].save) continue;
        for (const char* k = g_setDefs[i].key; *k; k++) buf[n++] = *k;
        buf[n++] = '=';
        char num[8]; int m = 0, v = *g_setDefs[i].v; do { num[m++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (m) buf[n++] = num[--m];
        buf[n++] = '\r'; buf[n++] = '\n';
    }
    wchar p[MAX_PATH]; settings_path(p);
    HANDLE f = CreateFileW(p, 0x40000000 /* GENERIC_WRITE */, 1, 0, 2 /* CREATE_ALWAYS */, 0x80, 0);
    if (f == INVALID_HANDLE_VALUE || !f) return;
    DWORD w; WriteFile(f, buf, n, &w, 0); CloseHandle(f);
}

/* ------------------------------------------------------------------ */
/* health cues: a sound when a fighter drops to 75, 50 and 25 percent   */
/* GFGGameInfo (FG_GAME_INFO*) +0x1968: PLAYER_INFO* for player 1 and 2.
   PLAYER_INFO +0x20 FGCharacterObj*, +0x28 its handle (valid while obj+8 == handle,
   bit 30 clear: what PLAYER_INFO::GetActiveFGCharacterObj checks).
   FGCharacterObj +0xA38 life, +0xA3C max life (floats; FGCharacterObj::GetLife/GetMaxLife).
   Which fighter is you: PLAYER_INFO +0x50 GAMEPAD* (GAMEPAD+0 index; above 7 is a
   network pad, Input / GAMEPAD::IsNetworkGamepad), +0x2D8/+0x2E0 the AI drone and its
   handle (set when the computer plays that side). */
#define GFGGAMEINFO 0x1430DBA00ULL
static u64 game_ptr(u64 addr) { u64* p = (u64*)(g_base + (addr - IMAGE_BASE)); return readable(p, 8) ? *p : 0; }
static u64 rdp(u64 a) { return readable((void*)a, 8) ? *(u64*)a : 0; }
static int obj_live(u64 ptr, u64 handleAt)
{
    if (!ptr || !readable((void*)handleAt, 4) || !readable((void*)(ptr + 8), 4)) return 0;
    int h = *(int*)handleAt; DWORD id = *(DWORD*)(ptr + 8);
    return h != -1 && !(id & 0x40000000) && id == (DWORD)h;
}
typedef struct { u64 info, obj; float life, max, meter, x; int ok, pad, ai, hasX; } Fighter;
static void read_fighter(u64 fg, int p, Fighter* f)
{
    memset(f, 0, sizeof(*f)); f->pad = -1;
    f->info = rdp(fg + 0x1968 + 8 * p);
    if (!f->info) return;
    u64 obj = rdp(f->info + 0x20);
    if (!obj_live(obj, f->info + 0x28) || !readable((void*)(obj + 0xA38), 8)) return;
    f->obj = obj; f->life = *(float*)(obj + 0xA38); f->max = *(float*)(obj + 0xA3C);
    f->ok = f->max > 0 && f->life >= 0 && f->life <= f->max * 1.01f;
    u64 gp = rdp(f->info + 0x50);
    if (gp && readable((void*)gp, 4)) f->pad = *(int*)gp;
    f->ai = obj_live(rdp(f->info + 0x2D8), f->info + 0x2E0);
    /* PLAYER_INFO +0x28C super meter, 0 to 1 (PLAYER_INFO::GetSuperMeter; AdjustSuperMeter
       keeps it there). Full, all three bars, is what an X-ray costs. */
    f->meter = readable((void*)(f->info + 0x28C), 4) ? *(float*)(f->info + 0x28C) : -1;
    /* where the fighter stands between the stage walls. MKObj::GetWorldLocation: obj+0x20
       -> +0xE8 x,y,z. The fight line (FGConstrain): left wall point 0x1430CFD28, right wall
       point 0x1430CFD34, unit direction 0x1430CFD50, wall to wall length 0x1430CFD5C
       (GetTightropeCoordinate = dot(location - left wall, direction); AdjustConstrainWall
       moves the walls). */
    u64 ao = rdp(obj + 0x20);
    if (ao && readable((void*)(ao + 0xE8), 12)) {
        float* loc = (float*)(ao + 0xE8);
        float* a = (float*)(g_base + (0x1430CFD28ULL - IMAGE_BASE));
        float* d = (float*)(g_base + (0x1430CFD50ULL - IMAGE_BASE));
        f->x = (loc[0] - a[0]) * d[0] + (loc[1] - a[1]) * d[1] + (loc[2] - a[2]) * d[2];
        f->hasX = 1;
    }
}
static int g_hpLevel[2], g_you, g_xrayReady[2]; static float g_hpLife[2];
static u64 g_hpObj[2];
static int hp_level(float frac) { return (frac <= 0.75f) + (frac <= 0.50f) + (frac <= 0.25f); }
static DWORD g_xrayT[2];
static void xray_ready(int p, const wchar* how)
{
    g_xrayReady[p] = 1; g_xrayT[p] = GetTickCount();
    int mine = p == g_you;
    snd_play_pan(mine ? "xray_you" : "xray_opponent", g_stereo ? (p ? 1 : -1) : 0, mine ? C_XRAY_YOU : C_XRAY_OPP);
    static wchar line[64]; wcpy(line, mine ? (const wchar*)L"you ready" : (const wchar*)L"opponent ready", 64); wcat(line, how, 64);
    logf3((const wchar*)L"XRAY", 0, line);
}
/* from hk_setMeter: a fighter's meter reached full */
static void meter_full(u64 info)
{
    if (!g_hpObj[0]) return;   /* not in a fight we know yet */
    u64 fg = game_ptr(GFGGAMEINFO);
    for (int p = 0; p < 2; p++)
        if (fg && rdp(fg + 0x1968 + 8 * p) == info && !g_xrayReady[p]) xray_ready(p, (const wchar*)L" (meter hook)");
}
/* walls: a steady hum from that wall's side while a fighter is close to a wall, getting
   louder the closer they get (full in the corner). Reaching the corner is only logged. */
static int g_wallZone[2], g_wallSide[2];
#define WALL_NEAR 0.15f     /* of the stage length: the steady sound starts */
#define WALL_AT 0.05f       /* in the corner (checked against the log: sounds right) */
#define WALL_BACK 0.03f     /* has to move this much further out before it is logged again */
static float g_humVol; static int g_humPan;
static void walls_quiet(void) { snd_loop(0, "", 0, 0); }
static void wall_tick(Fighter* f, int p, float len, DWORD now)
{
    if (!f->hasX || len < 100) return;
    float left = f->x, right = len - f->x;
    int side = left <= right ? 0 : 1;            /* 0 left wall, 1 right wall */
    float dist = (side ? right : left) / len;     /* 0 at the wall, 0.5 mid stage */
    if (dist < 0) dist = 0;
    int mine = p == g_you;
    /* the steady sound: quiet as it starts, full volume in the corner. One hum for both
       fighters: whoever is closer to a wall decides how loud and from which side */
    float vol = dist < WALL_NEAR ? 0.12f + 0.88f * (1 - dist / WALL_NEAR) : 0;
    if (vol > g_humVol) { g_humVol = vol; g_humPan = side ? 1 : -1; }
    int zone = dist <= WALL_AT ? 2 : dist <= WALL_NEAR ? 1 : 0;
    int cur = g_wallZone[p];
    if (zone > cur) {
        g_wallZone[p] = zone; g_wallSide[p] = side;
        static wchar line[96]; line[0] = 0;
        wcat(line, mine ? (const wchar*)L"you " : (const wchar*)L"opponent ", 96);
        wcat(line, zone == 2 ? (const wchar*)L"in the corner " : (const wchar*)L"near the wall ", 96);
        wcat(line, side ? (const wchar*)L"right, " : (const wchar*)L"left, ", 96);
        wcat_u(line, (u64)(dist * 1000), 96); wcat(line, (const wchar*)L" per mille", 96);
        logf3((const wchar*)L"WALL", 0, line);
    } else if (zone < cur) {   /* moved out: allow again once clearly away */
        float lim = cur == 2 ? WALL_AT + WALL_BACK : WALL_NEAR + WALL_BACK;
        if (dist > lim) g_wallZone[p] = zone;
    }
}

static void health_tick(DWORD now)
{
    static DWORD last;
    if (!g_base || now - last < 50) return;
    last = now;
    u64 fg = game_ptr(GFGGAMEINFO);
    Fighter f[2];
    if (!fg) { g_hpObj[0] = g_hpObj[1] = 0; walls_quiet(); return; }
    read_fighter(fg, 0, &f[0]); read_fighter(fg, 1, &f[1]);
    if (!f[0].ok || !f[1].ok) { g_hpObj[0] = g_hpObj[1] = 0; walls_quiet(); return; }
    if (f[0].obj != g_hpObj[0] || f[1].obj != g_hpObj[1]) {   /* a new fight: work out who is who, no cue */
        int local0 = f[0].pad >= 0 && f[0].pad <= 7 && !f[0].ai, local1 = f[1].pad >= 0 && f[1].pad <= 7 && !f[1].ai;
        g_you = g_side ? g_side - 1 : (local1 && !local0) ? 1 : 0;
        static wchar line[256]; line[0] = 0;
        for (int p = 0; p < 2; p++) {
            wcat(line, p ? (const wchar*)L" | P2 life " : (const wchar*)L"P1 life ", 256); wcat_u(line, (u64)f[p].life, 256);
            wcat(line, (const wchar*)L"/", 256); wcat_u(line, (u64)f[p].max, 256);
            wcat(line, (const wchar*)L" pad ", 256); if (f[p].pad < 0) wcat(line, (const wchar*)L"none", 256); else wcat_u(line, (u64)f[p].pad, 256);
            wcat(line, f[p].ai ? (const wchar*)L" computer" : (const wchar*)L" human", 256);
        }
        wcat(line, g_you ? (const wchar*)L" -> you are P2" : (const wchar*)L" -> you are P1", 256);
        { int mode = readable((void*)(fg + 0x198C), 4) ? *(int*)(fg + 0x198C) : -1;   /* FG_GAME_INFO::GetGameMode */
          wcat(line, (const wchar*)L" mode ", 256); wcat_u(line, (u64)(mode < 0 ? 0 : mode), 256); }
        logf3((const wchar*)L"FIGHTERS", 0, line);
        for (int p = 0; p < 2; p++) { g_hpObj[p] = f[p].obj; g_hpLevel[p] = hp_level(f[p].life / f[p].max); g_hpLife[p] = f[p].life; }
        g_combo[0].dirty = g_combo[1].dirty = 0;   /* nothing from the last fight carries over */
        for (int p = 0; p < 2; p++) g_xrayReady[p] = f[p].meter >= 0.999f;   /* known from the start: no sound */
        g_wallZone[0] = g_wallZone[1] = 0;
        g_lifeKnown = 1; g_wallQuiet = 0;
        return;
    }
    {   /* walls; and while testing, where everybody stands once a second */
        float len = *(float*)(g_base + (0x1430CFD5CULL - IMAGE_BASE));
        g_humVol = 0;
        for (int p = 0; p < 2; p++) wall_tick(&f[p], p, len, now);
        if (f[0].life <= 0 || f[1].life <= 0) g_humVol = 0;   /* knocked out: the round is over */
        if (g_wallQuiet == 1) {   /* a menu: quiet until somebody moves (fights without a timer) */
            static float qx[2];
            if (!g_quietPosSet) { qx[0] = f[0].x; qx[1] = f[1].x; g_quietPosSet = 1; }
            else if (qx[0] - f[0].x > 10 || f[0].x - qx[0] > 10 || qx[1] - f[1].x > 10 || f[1].x - qx[1] > 10) g_wallQuiet = 0;
        }
        if (g_wallQuiet) g_humVol = 0;
        if (!g_menuOpen) snd_loop(0, "wall_loop", g_cv[C_HUM] > 0 ? g_humVol * g_cv[C_HUM] / 100 : 0, g_humPan);   /* the menu uses it for its preview */
        static DWORD lastPos;
        if (now - lastPos >= 1000 && f[0].hasX && f[1].hasX) {
            lastPos = now;
            static wchar line[96]; line[0] = 0;
            wcat(line, (const wchar*)L"length ", 96); wcat_u(line, (u64)(len > 0 ? len : 0), 96);
            wcat(line, (const wchar*)L" P1 ", 96); wcat_u(line, (u64)(f[0].x > 0 ? f[0].x : 0), 96);
            wcat(line, (const wchar*)L" P2 ", 96); wcat_u(line, (u64)(f[1].x > 0 ? f[1].x : 0), 96);
            logf3((const wchar*)L"POS", 0, line);
        }
    }
    for (int p = 0; p < 2; p++) {   /* X-ray ready: the meter just filled */
        float m = f[p].meter;
        if (m < 0 || m > 1.01f) continue;
        if (!g_xrayReady[p] && m >= 0.999f) xray_ready(p, (const wchar*)L"");
        else if (g_xrayReady[p] && m < 0.98f && now - g_xrayT[p] > 300) g_xrayReady[p] = 0;   /* used it (or lost meter): ready to tell again */
    }
    for (int p = 0; p < 2; p++) {
        if (f[p].life < g_hpLife[p]) g_lifeDropT = now;
        g_hpLife[p] = f[p].life;
        int lv = hp_level(f[p].life / f[p].max);
        if (lv > g_hpLevel[p] && f[p].life > 0) {
            int mine = p == g_you;
            {
                static const char* names[2][3] = { {"you75", "you50", "you25"}, {"opponent75", "opponent50", "opponent25"} };
                snd_play_pan(names[mine ? 0 : 1][lv - 1], g_stereo ? (p ? 1 : -1) : 0, mine ? C_HEALTH_YOU : C_HEALTH_OPP);
            }
            static wchar line[64]; line[0] = 0;
            wcat(line, mine ? (const wchar*)L"you " : (const wchar*)L"opponent ", 64); wcat_u(line, (u64)(75 - 25 * (lv - 1)), 64);
            logf3((const wchar*)L"HEALTH", 0, line);
        }
        g_hpLevel[p] = lv;   /* goes back up for a new round */
    }
}

/* ------------------------------------------------------------------ */
/* the accessibility menu                                               */
/* Up/down choose, left/right change. On a sound, left/right set its volume in steps of
   10 (0 is off) and play the sound at the new volume, from the side it comes from in a
   fight. A/Enter on a sound just plays it. */
enum { MI_AUTOREAD, MI_COMBO, MI_DAMAGE, MI_S_MENU, MI_VOLUME,
       MI_S_HEALTH_YOU, MI_S_HEALTH_OPP, MI_S_XRAY_YOU, MI_S_XRAY_OPP, MI_S_HUM,
       MI_CLOSE, MI_COUNT };
static int g_menuShown, g_menuItem, g_menuHinted;
static DWORD g_humPreviewT; static int g_humPreviewPan;
static int item_cat(int i) { return i == MI_S_MENU ? C_MENU : i >= MI_S_HEALTH_YOU && i <= MI_S_HUM ? C_HEALTH_YOU + (i - MI_S_HEALTH_YOU) : -1; }
static void menu_line(int i, wchar* out, int max, int withPos)
{
    static const wchar* labels[MI_COUNT] = {
        (const wchar*)L"Auto read", (const wchar*)L"Hit and combo readout", (const wchar*)L"Combo damage numbers",
        (const wchar*)L"Menu sounds", (const wchar*)L"Overall sound volume",
        (const wchar*)L"Your health sounds", (const wchar*)L"Opponent health sounds",
        (const wchar*)L"Your X-ray ready sound", (const wchar*)L"Opponent X-ray ready sound",
        (const wchar*)L"Wall hum",
        (const wchar*)L"Close menu" };
    wcpy(out, labels[i], max);
    int* b = i == MI_COMBO ? &g_comboReadout : i == MI_DAMAGE ? &g_damageNumbers : i == MI_AUTOREAD ? &g_autoRead : 0;
    if (b) wcat(out, *b ? (const wchar*)L", on" : (const wchar*)L", off", max);
    int c = item_cat(i), v = i == MI_VOLUME ? g_volume : c >= 0 ? g_cv[c] : -1;
    if (v == 0) wcat(out, (const wchar*)L", off", max);
    else if (v > 0) { wcat(out, (const wchar*)L", ", max); wcat_u(out, (u64)v, max); wcat(out, (const wchar*)L" percent", max); }
    if (withPos) { wcat(out, (const wchar*)L", ", max); wcat_u(out, (u64)i + 1, max); wcat(out, (const wchar*)L" of ", max); wcat_u(out, MI_COUNT, max); }
}
static void menu_say(int interrupt) { static wchar line[160]; menu_line(g_menuItem, line, 160, 1); speak_force_q(line, interrupt); }
/* play a sound the way it comes in a fight: yours from your health bar's side */
static void menu_preview(int i)
{
    int you = g_you ? 1 : -1, opp = -you;
    int pyou = g_stereo ? you : 0, popp = g_stereo ? opp : 0;
    switch (i) {
    case MI_VOLUME:       snd_play_pan("you75", 0, -1); break;
    case MI_S_HEALTH_YOU: snd_play_pan("you75", pyou, C_HEALTH_YOU); break;
    case MI_S_HEALTH_OPP: snd_play_pan("opponent75", popp, C_HEALTH_OPP); break;
    case MI_S_XRAY_YOU:   snd_play_pan("xray_you", pyou, C_XRAY_YOU); break;
    case MI_S_XRAY_OPP:   snd_play_pan("xray_opponent", popp, C_XRAY_OPP); break;
    case MI_S_HUM:        g_humPreviewT = GetTickCount(); g_humPreviewPan = you; break;   /* menu_tick plays it */
    case MI_S_MENU:       snd_play("menu_move"); break;
    }
}
static void menu_show(void)
{
    g_menuOpen = 1; g_menuShown = 1;
    snd_loop(0, "", 0, 0);   /* no wall hum under the menu */
    snd_play("menu_open");
    static wchar line[400]; line[0] = 0;
    wcpy(line, (const wchar*)L"Accessibility menu. ", 400);
    if (!g_menuHinted) { wcat(line, (const wchar*)L"Up and down to choose, left and right to change, B or Escape to close. ", 400); g_menuHinted = 1; }
    wchar item[160]; menu_line(g_menuItem, item, 160, 1); wcat(line, item, 400);
    speak_force_q(line, 1);
    log_w((const wchar*)L"MENU open");
}
static void menu_close(void)
{
    g_menuShown = 0; g_humPreviewT = 0;
    snd_loop(0, "", 0, 0);
    settings_save();
    g_menuOpen = 0;
    snd_play("menu_close");
    speak_force_q((const wchar*)L"Menu closed", 1);
    log_w((const wchar*)L"MENU closed");
}
static void menu_move(int d)
{
    int n = g_menuItem + d;
    if (n < 0 || n >= MI_COUNT) { snd_play("menu_edge"); n = (n + MI_COUNT) % MI_COUNT; }   /* wraps around */
    else snd_play("menu_move");
    g_menuItem = n; menu_say(1);
}
static void menu_change(int d, int activate)
{
    int i = g_menuItem, c = item_cat(i);
    int* b = i == MI_COMBO ? &g_comboReadout : i == MI_DAMAGE ? &g_damageNumbers : i == MI_AUTOREAD ? &g_autoRead : 0;
    if (i == MI_VOLUME || c >= 0) {
        if (activate) { menu_preview(i); return; }   /* A/Enter: just listen */
        int* v = i == MI_VOLUME ? &g_volume : &g_cv[c];
        int nv = *v + 10 * d; nv = nv < 0 ? 0 : nv > 100 ? 100 : nv;
        if (nv == *v) { snd_play("menu_edge"); menu_say(1); return; }   /* already at 0 or 100 */
        *v = nv;
        settings_save();
        static wchar line[64]; line[0] = 0;
        if (nv) { wcat_u(line, (u64)nv, 64); wcat(line, (const wchar*)L" percent", 64); } else wcpy(line, (const wchar*)L"off", 64);
        speak_force_q(line, 1);
        menu_preview(i);
        return;
    }
    if (b) *b = !*b;
    else if (i == MI_CLOSE) { if (activate) menu_close(); return; }
    else return;
    settings_save();
    menu_say(1);
}
static void menu_tick(DWORD now)
{
    if (g_menuOpen && !g_menuShown) menu_show();          /* opened from the controller */
    if (g_humPreviewT) {   /* the wall hum for a second, at its volume */
        if (g_menuShown && now - g_humPreviewT < 1200) snd_loop(0, "wall_loop", g_cv[C_HUM] / 100.0f, g_humPreviewPan);
        else { snd_loop(0, "", 0, 0); g_humPreviewT = 0; }
    }
}
static void menu_pad(int x)
{
    if (!g_menuShown) return;   /* the opening press itself */
    if ((x & PAD_Y) && (x & PAD_BACK)) { menu_close(); return; }   /* Back + Y again */
    if (x & PAD_B) { menu_close(); return; }
    if (x & PAD_UP) menu_move(-1);
    if (x & PAD_DOWN) menu_move(1);
    if (x & PAD_LEFT) menu_change(-1, 0);
    if (x & PAD_RIGHT) menu_change(1, 0);
    if (x & (PAD_A | PAD_START)) menu_change(1, 1);
}

/* ------------------------------------------------------------------ */
/* hotkeys                                                             */
static int game_in_front(void)
{
    HWND w = GetForegroundWindow(); DWORD pid = 0;
    if (w) GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}
static int g_keyDown[256];
static int pressed(int vk)
{
    int down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    int edge = down && !g_keyDown[vk];
    g_keyDown[vk] = down;
    return edge;
}
static void poll_keys(void)
{
    int front = game_in_front();
    int f5 = pressed(0x74), f6 = pressed(0x75), f7 = pressed(0x76), f8 = pressed(0x77), f9 = pressed(0x78), f10 = pressed(0x79), f11 = pressed(0x7A);
    int f3 = pressed(0x72), up = pressed(0x26), dn = pressed(0x28), lf = pressed(0x25), rt = pressed(0x27), ent = pressed(0x0D), esc = pressed(0x1B), sp = pressed(0x20);
    if (!front) return;
    if (g_menuShown) {   /* the menu has the keyboard (the game gets no key presses meanwhile) */
        if (f3 || esc) { menu_close(); return; }
        if (up) menu_move(-1);
        if (dn) menu_move(1);
        if (lf) menu_change(-1, 0);
        if (rt) menu_change(1, 0);
        if (ent || sp) menu_change(1, 1);
        if (f5) { static wchar again[2048]; wcpy(again, g_last, 2048); speak_force_q(again, 1); }
        return;
    }
    if (f3) { g_menuOpen = 1; menu_show(); return; }
    if (f11) steam_invite_key((GetAsyncKeyState(0x10) & 0x8000) != 0, (GetAsyncKeyState(0x11) & 0x8000) != 0);
    if (f10 && (GetAsyncKeyState(0x11) & 0x8000)) {   /* Ctrl+F10: hit and combo readout */
        g_comboReadout = !g_comboReadout; settings_save();
        speak_force(g_comboReadout ? (const wchar*)L"Hit and combo readout on" : (const wchar*)L"Hit and combo readout off");
    }
    else if (f10) { g_damageNumbers = !g_damageNumbers; settings_save(); speak_force(g_damageNumbers ? (const wchar*)L"Damage numbers on" : (const wchar*)L"Damage numbers off"); }
    if (f9) speak_force((const wchar*)(L"MKX Access version " VERSION));
    if (f5) { static wchar again[2048]; wcpy(again, g_last[0] ? g_last : (const wchar*)L"Nothing yet", 2048); speak(again, 1); }
    if (f6 && !entry_read()) read_screen();
    if (f7) { g_autoRead = !g_autoRead; settings_save(); speak_force(g_autoRead ? (const wchar*)L"Auto read on" : (const wchar*)L"Auto read off"); }
    if (f8) { g_muted = !g_muted; speak_force(g_muted ? (const wchar*)L"MKX Access muted" : (const wchar*)L"MKX Access on"); }
}

/* ------------------------------------------------------------------ */
/* the timing side of the speech logic, run every 25 ms (also used by test/harness.c) */
static void challenge_tick(DWORD now);
static void speech_tick(DWORD now)
{
    fight_tick(now);
    challenge_tick(now);
    entry_tick(now);
    if (g_valEntry && now - g_valT >= 40) {
        Entry* e = (Entry*)g_valEntry; g_valEntry = 0;
        int pageLoad = (g_burstT - g_valT < 60) && g_burstN > 3;
        if (!g_focusPending && !pageLoad && e->gen == g_valGen && e->text[0]) {
            static wchar lbl[160];
            if (room_value_line(e, lbl, 160)) say(lbl, 1);
            else if (!(contains_ci(e->path, "PlayerListGridSelectElement.Cell_") && is_na(e->text))) say(e->text, 1);
        }
    }
    if (g_focusPending && now - g_focusTime >= FOCUS_DELAY) { g_focusPending = 0; announce_focus(); }
    auto_read(now);
}

static DWORD WINAPI worker(void* arg)
{
    wchar p[MAX_PATH];
    wcpy(p, g_dir, MAX_PATH); wcat(p, (const wchar*)L"MKXAccess.log", MAX_PATH);
    g_log = CreateFileW(p, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    log_w((const wchar*)L"MKX Access " VERSION L" started");
    for (int i = 0; i < NHOOKS; i++) {
        wchar name[96]; int k = 0; for (; g_hooks[i].name[k] && k < 95; k++) name[k] = (BYTE)g_hooks[i].name[k]; name[k] = 0;
        logf3(g_hookOk[i] == 1 ? (const wchar*)L"HOOK OK" : g_hookOk[i] == -1 ? (const wchar*)L"HOOK SKIPPED (bytes differ)" : (const wchar*)L"HOOK FAILED", name, 0);
    }

    log_w(g_padHooked ? (const wchar*)L"HOOK OK [XInputGetState import]" : (const wchar*)L"HOOK FAILED [XInputGetState import]");
    wcpy(p, g_dir, MAX_PATH); wcat(p, (const wchar*)L"nvdaControllerClient64.dll", MAX_PATH);
    HMODULE nv = LoadLibraryW(p);
    if (nv) {
        nv_speak = (nvSpeak_t)GetProcAddress(nv, "nvdaController_speakText");
        nv_cancel = (nvVoid_t)GetProcAddress(nv, "nvdaController_cancelSpeech");
        nv_test = (nvVoid_t)GetProcAddress(nv, "nvdaController_testIfRunning");
        g_nvOk = nv_test && nv_test() == 0;
        log_w(g_nvOk ? (const wchar*)L"NVDA is running" : (const wchar*)L"NVDA not detected yet");
    } else log_w((const wchar*)L"nvdaControllerClient64.dll not found - speech disabled, logging only");
    settings_load();

    int okCount = 0; for (int i = 0; i < NHOOKS; i++) if (g_hookOk[i] == 1) okCount++;
    const wchar* hello = okCount == NHOOKS ? (const wchar*)(L"MKX Access " VERSION L" loaded") :
                okCount ? (const wchar*)(L"MKX Access " VERSION L" loaded with some hooks missing") :
                          (const wchar*)(L"MKX Access " VERSION L" could not hook this game version");
    int announced = 0, frontSince = 0; DWORD frontT = 0;

    DWORD lastStats = GetTickCount(), lastSteamTry = 0;
    for (;;) {
        WaitForSingleObject(g_qevent, 25);
        for (;;) {
            Event ev; int have = 0;
            EnterCriticalSection(&g_qlock);
            if (g_qtail != g_qhead) { memcpy(&ev, &g_q[g_qtail], sizeof(Event)); g_qtail = (g_qtail + 1) % QSIZE; have = 1; }
            LeaveCriticalSection(&g_qlock);
            if (!have) break;
            if (ev.type <= EV_VHTML) handle_text(&ev);
            else if (ev.type == EV_LABEL) logf3((const wchar*)L"ANIM", ev.path[0] ? ev.path : (const wchar*)L"?", ev.text);
            else if (ev.type == EV_COMBOEND) combo_finished(&ev);
            else if (ev.type == EV_INVITE) steam_invite_requested(&ev);
            else if (ev.type == EV_LOBBYINVITE) steam_invite_received(&ev);
            else if (ev.type == EV_MENUPAD) menu_pad(ev.x);
            else if (ev.type == EV_METERFULL) meter_full(ev.id);
            else if (ev.type == EV_FDIAG) logf3((const wchar*)L"FD", 0, ev.text);
            else if (ev.type == EV_PAD && (ev.x & PAD_BACK)) steam_accept_invite();
            else if (ev.type == EV_PAD) {
                if (ev.x & PAD_DOWN) steam_invite_key(0, 0);
                if (ev.x & PAD_UP) steam_invite_key(1, 0);
                if (ev.x & PAD_A) steam_invite_key(0, 1);
            }
            else handle_focus(&ev);
        }
        DWORD now = GetTickCount();
        /* say hello once the game window has been in front for 2 seconds (NVDA is done
           announcing the window by then), or after 30 seconds at the latest */
        if (!announced) {
            if (game_in_front()) { if (!frontSince) { frontSince = 1; frontT = now; } }
            else frontSince = 0;
            if ((frontSince && now - frontT >= 2000) || now - g_t0 >= 30000) {
                announced = 1;
                logf3((const wchar*)L"SPEAK", 0, hello);
                if (nv_speak) nv_speak(hello);   /* queued after whatever NVDA is saying */
            }
        }
        speech_tick(now);
        if (!g_inviteHooked && now - lastSteamTry > 1000) { steam_try_hook(); lastSteamTry = now; }
        steam_invite_tick(now);
        if (g_cbRegistered == 2) { log_w((const wchar*)L"STEAM listening for invites (LobbyInvite_t)"); g_cbRegistered = 3; }
        poll_keys();
        menu_tick(now);
        health_tick(now);
        /* no forced disk flush: written lines are in Windows' file cache right away
           (readable by anyone, kept if the game crashes); forcing them onto the disk
           every 2 seconds only made extra disk work */
        if (now - lastStats > 60000) {
            wchar a[24], b[24], c[24], line[128];
            utoa_w((u64)g_statCalls, a, 10, 1); utoa_w((u64)g_statDeduped, b, 10, 1); utoa_w((u64)g_statQueued, c, 10, 1);
            wcpy(line, (const wchar*)L"text calls=", 128); wcat(line, a, 128); wcat(line, (const wchar*)L" skipped-unchanged=", 128);
            wcat(line, b, 128); wcat(line, (const wchar*)L" events=", 128); wcat(line, c, 128);
            logf3((const wchar*)L"STATS", 0, line);
            lastStats = now;
        }
    }
    return 0;
}

BOOL WINAPI DllMain(HMODULE h, DWORD reason, void* r)
{
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(h);
    /* only run inside MK10.exe */
    wchar exe[MAX_PATH]; DWORD n = GetModuleFileNameW(0, exe, MAX_PATH);
    int j = (int)n; while (j > 0 && exe[j - 1] != '\\') j--;
    const wchar* base = exe + j;
    if (!(starts_ci(base, "MK10.exe") || starts_ci(base, "MK10Game.exe"))) return TRUE;
    if (starts_ci(base, "MK10Game.exe")) return TRUE; /* addresses are for MK10.exe */

    n = GetModuleFileNameW(h, g_dir, MAX_PATH);
    while (n && g_dir[n - 1] != '\\') n--;
    g_dir[n] = 0;
    g_t0 = GetTickCount();
    g_base = (u64)GetModuleHandleW(0);
    {   /* SizeOfImage from the PE header */
        DWORD lfanew = *(DWORD*)(g_base + 0x3C);
        g_imageEnd = g_base + *(DWORD*)(g_base + lfanew + 0x50);
    }
    g_q = (Event*)VirtualAlloc(0, sizeof(Event) * QSIZE, MEM_COMMIT | MEM_RESERVE, 0x04);
    g_ent = (Entry*)VirtualAlloc(0, sizeof(Entry) * NENT, MEM_COMMIT | MEM_RESERVE, 0x04);
    if (!g_q || !g_ent) return TRUE;
    InitializeCriticalSection(&g_qlock);
    g_qevent = CreateEventW(0, FALSE, FALSE, 0);
    {   /* verify GetCellElement prologue before trusting it: mov [rsp+20h],rsi / push rdi / sub rsp,40h */
        BYTE* gc = (BYTE*)(g_base + (0x1403A3CD0ULL - IMAGE_BASE));
        static const BYTE want[] = {0x48,0x89,0x74,0x24,0x20,0x57,0x48,0x83,0xEC,0x40};
        int ok = 1; for (int i = 0; i < 10; i++) if (gc[i] != want[i]) ok = 0;
        if (ok) g_getCell = (getCell_t)gc;
    }
    install_hooks();
    g_padHooked = patch_import("XINPUT9_1_0.dll", "XInputGetState", (void*)hk_XInputGetState, (void**)&o_XInputGetState);
    g_ready = 1;
    HANDLE t = CreateThread(0, 1 << 20, worker, 0, 0, 0); /* 1 MB committed stack */
    if (t) CloseHandle(t);
    return TRUE;
}
