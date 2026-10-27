/* MKX Access - screen reader support for Mortal Kombat X (Steam, PC).
   Hooks the game's UI text and focus functions and speaks through NVDA.

   Hotkeys (only while the game window is in front):
     F5  repeat last spoken text
     F6  read everything currently known on screen
     F7  toggle auto-reading of new text that is not tied to focus
     F8  mute / unmute MKX Access
   Everything is also written to MKXAccess.log next to the game exe. */
#include "win.h"

#define VERSION L"0.3"
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
enum { EV_TEXT = 1, EV_HTML, EV_VTEXT, EV_VHTML, EV_FOCUS, EV_SETFOCUS, EV_GRID, EV_ANIM, EV_LABEL };
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

static int readable(const void* p, SIZE_T n)
{
    MEMORY_BASIC_INFORMATION mbi;
    if ((u64)p < 0x10000 || (u64)p > 0x7FFFFFFFFFFFULL) return 0;
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
static void hk_setText(void* self, const wchar* s)
{
    Event* e = q_begin();
    if (e) { e->type = EV_TEXT; e->id = (u64)self; path_of(self, e->path); if (s) wcpy(e->text, s, TEXTMAX); q_end(); }
    o_setText(self, s);
}
static DWORD hk_setTextHTML(void* self, const wchar* s)
{
    Event* e = q_begin();
    if (e) { e->type = EV_HTML; e->id = (u64)self; path_of(self, e->path); if (s) wcpy(e->text, s, TEXTMAX); q_end(); }
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
static DWORD hk_gotoAndPlay(void* self, const char* label) { anim_event(self, label); return o_gotoAndPlay(self, label); }
static DWORD hk_gotoAndStop(void* self, const char* label) { anim_event(self, label); return o_gotoAndStop(self, label); }

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
        void* p = VirtualAlloc((void*)a, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
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
static void clean_text(const wchar* in, wchar* out, int max)
{
    int o = 0, space = 1;
    for (int i = 0; in[i] && o < max - 1; i++) {
        wchar c = in[i];
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
    {"LS","left stick"},{"RS","right stick"},{"L3","left stick click"},{"R3","right stick click"},
    {"START","start"},{"BACK","back button"},{"SELECT","select"},{"MENU","menu"},{"VIEW","view"},
    {"U","up"},{"D","down"},{"L","left"},{"R","right"},
    {"Up","up"},{"Down","down"},{"Left","left"},{"Right","right"},
    {"Away","back"},{"Towards","forward"},{"Toward","forward"},
    {"AwayUp","up back"},{"TowardsUp","up forward"},{"AwayDown","down back"},{"TowardsDown","down forward"},
    {"FP","front punch"},{"BP","back punch"},{"FK","front kick"},{"BK","back kick"},
    {"BLK","block"},{"BL","block"},{"INT","interact"},{"SS","stance switch"},{"FLIP","flip stance"},
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
static int ignored_path(const wchar* p)
{
    return contains_ci(p, "ButtonLogs") || contains_ci(p, "wbPlayWindow") || contains_ci(p, "BenchmarkStats") ||
           contains_ci(p, "versionNumber") || starts_ci(p, "_root.legal.");
}

/* ------------------------------------------------------------------ */
/* text table (worker thread only), keyed by element path              */
typedef struct { u64 key; wchar path[PATHMAX]; wchar text[TEXTMAX]; DWORD changed, winStart; int winCount, used, gen; } Entry;
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
    e->used = 1; e->key = key; e->path[0] = 0; e->text[0] = 0; e->winCount = 0; e->winStart = 0; e->changed = GetTickCount();
    e->gen = ++g_gen;
    return e;
}
static int is_busy(Entry* e, DWORD now) { return now - e->winStart <= 2000 && e->winCount > 3; }

/* focus state */
static wchar g_focusPath[PATHMAX];
static DWORD g_focusTime; static int g_focusPending;
static int g_gridX = -1, g_gridY = -1;
static wchar g_lastFocus[PATHMAX]; static DWORD g_lastFocusT;

static void handle_text(Event* ev)
{
    wchar clean[TEXTMAX];
    clean_text(ev->text, clean, TEXTMAX);
    const wchar* tag = ev->type == EV_TEXT ? (const wchar*)L"TEXT" : ev->type == EV_HTML ? (const wchar*)L"HTML" :
                       ev->type == EV_VTEXT ? (const wchar*)L"VTEXT" : (const wchar*)L"VHTML";
    u64 key = ev->path[0] ? hash_path(ev->path) : (ev->id & ~1ULL);
    if (!key) return;
    Entry* e = find_entry(key);
    if (ev->path[0]) wcpy(e->path, ev->path, PATHMAX);
    if (weq(e->text, clean)) return;           /* unchanged: ignore */
    logf3(tag, e->path[0] ? e->path : (const wchar*)L"?", clean);
    wcpy(e->text, clean, TEXTMAX);
    DWORD now = ev->t;
    if (now - e->winStart > 2000) { e->winStart = now; e->winCount = 0; }
    e->winCount++;
    e->changed = now;
    if (!clean[0] || ignored_path(e->path)) return;
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

static int count_children(const wchar* parent)
{
    int n = 0;
    for (int i = 0; i < NENT; i++) { Entry* e = &g_ent[i]; if (e->used && e->text[0] && is_child_path(e->path, parent)) n++; }
    return n;
}

static void announce_focus(void)
{
    static wchar out[2048]; out[0] = 0; int count = 0;
    /* 1. the focused item's own label: the focus marker is often a sibling of the
          label (lineItem2.menuItem.leftFocus.posAlign vs lineItem2.menuItem.centerJust.text),
          so climb up until we reach a parent that holds 1-4 text items */
    static wchar anc[PATHMAX];
    wcpy(anc, g_focusPath, PATHMAX);
    for (int level = 0; level < 4 && anc[0]; level++) {
        int n = count_children(anc);
        if (n >= 1 && n <= 4) {
            /* read them in screen order (sorted by path: Title before Value, Text0 before Text1) */
            Entry* items[4]; int ni = 0;
            for (int i = 0; i < NENT && ni < 4; i++) {
                Entry* e = &g_ent[i];
                if (e->used && e->text[0] && is_child_path(e->path, anc)) items[ni++] = e;
            }
            for (int a = 1; a < ni; a++) for (int b = a; b > 0 && path_less(items[b]->path, items[b-1]->path); b--) { Entry* t = items[b]; items[b] = items[b-1]; items[b-1] = t; }
            for (int a = 0; a < ni; a++) add_part(out, 2048, items[a]->text, &count);
            break;
        }
        if (n > 4) { anc[0] = 0; break; }   /* reached the whole list: stop */
        int L = wlen(anc); while (L > 0 && anc[L - 1] != '.') L--;
        if (L <= 1) { anc[0] = 0; break; }
        anc[L - 1] = 0;
    }
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
            if (!is_titleish(t->path) && !contains_ci(t->path, "description") && !contains_ci(t->path, "HelpText")) continue;
        }
        g_recentSpoken[s] = 1;
        Entry* e = &g_ent[g_recent[s]];
        if (e->gen != g_recentGen[s] || !e->text[0]) continue;
        if (list[0] && is_child_path(e->path, list) && !(anc[0] && is_child_path(e->path, anc)) && !contains_ci(e->path, "title")) continue;
        if (no_letters(e->text) && !(anc[0] && is_child_path(e->path, anc))) continue;
        add_part(out, 2048, e->text, &count);
    }
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
        if (!g_recentSpoken[s] && now - g_recentT[s] < 250) return;
    }
    for (int k = start; k < g_nrecent; k++) {
        int s = k % NRECENT;
        if (g_recentSpoken[s]) continue;
        g_recentSpoken[s] = 1;
        Entry* e = &g_ent[g_recent[s]];
        if (e->gen != g_recentGen[s] || is_busy(e, now) || !e->text[0] || no_letters(e->text)) continue;
        if (now - g_lastTime < 3000 && wlen(e->text) >= 8 && contains_w(g_last, e->text)) continue;   /* just said it */
        if (nc < 64) cand[nc++] = e;
    }
    /* many texts at once = a whole screen loaded: read only its title / message */
    for (int i = 0; i < nc; i++) {
        Entry* e = cand[i];
        int titleish = is_titleish(e->path);
        if (nc > 6 && !titleish) continue;
        if (count < 8) add_part(out, 2048, e->text, &count);
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
    int f5 = pressed(0x74), f6 = pressed(0x75), f7 = pressed(0x76), f8 = pressed(0x77);
    if (!front) return;
    if (f5) { static wchar again[2048]; wcpy(again, g_last[0] ? g_last : (const wchar*)L"Nothing yet", 2048); speak(again, 1); }
    if (f6) read_screen();
    if (f7) { g_autoRead = !g_autoRead; speak_force(g_autoRead ? (const wchar*)L"Auto read on" : (const wchar*)L"Auto read off"); }
    if (f8) { g_muted = !g_muted; speak_force(g_muted ? (const wchar*)L"MKX Access muted" : (const wchar*)L"MKX Access on"); }
}

/* ------------------------------------------------------------------ */
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

    wcpy(p, g_dir, MAX_PATH); wcat(p, (const wchar*)L"nvdaControllerClient64.dll", MAX_PATH);
    HMODULE nv = LoadLibraryW(p);
    if (nv) {
        nv_speak = (nvSpeak_t)GetProcAddress(nv, "nvdaController_speakText");
        nv_cancel = (nvVoid_t)GetProcAddress(nv, "nvdaController_cancelSpeech");
        nv_test = (nvVoid_t)GetProcAddress(nv, "nvdaController_testIfRunning");
        log_w(nv_test && nv_test() == 0 ? (const wchar*)L"NVDA is running" : (const wchar*)L"NVDA not detected yet");
    } else log_w((const wchar*)L"nvdaControllerClient64.dll not found - speech disabled, logging only");

    int okCount = 0; for (int i = 0; i < NHOOKS; i++) if (g_hookOk[i] == 1) okCount++;
    speak_force(okCount == NHOOKS ? (const wchar*)L"MKX Access loaded" :
                okCount ? (const wchar*)L"MKX Access loaded with some hooks missing" : (const wchar*)L"MKX Access could not hook this game version");

    DWORD lastFlush = GetTickCount();
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
            else handle_focus(&ev);
        }
        DWORD now = GetTickCount();
        if (g_focusPending && now - g_focusTime >= 120) { g_focusPending = 0; announce_focus(); }
        auto_read(now);
        poll_keys();
        if (now - lastFlush > 2000) { if (g_log != INVALID_HANDLE_VALUE) FlushFileBuffers(g_log); lastFlush = now; }
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
    g_ready = 1;
    HANDLE t = CreateThread(0, 1 << 20, worker, 0, 0, 0); /* 1 MB committed stack */
    if (t) CloseHandle(t);
    return TRUE;
}
