/*
 * d3d9.c — AoE3 TAD resource-rate HUD mod (d3d9 proxy DLL), 32-bit i386.
 *
 * Swarm build worker: implementation of .swarm/PLAN.md (round 2 rework).
 * Address map from fresh Ghidra decompile + objdump — see VERIFIED_ADDRESSES.md.
 * All runtime memory reads are VirtualQuery-guarded (MinGW __try1/__except1
 * were verified empirically NOT to work on i686-w64-mingw32 16.2.0, so the
 * guard is the authoritative protection) — a stale pointer can never crash.
 *
 * Data layer (round 2): counts come from the CURRENT PLAYER's live encrypted
 * resource container `[player + 0x230]` (proven by the economic-score fn
 * FUN_005009c0), and the displayed rate reproduces the game's own
 * "X Minute Income" math from FUN_0086da39 on the per-player income object
 * `[player + 0x80]` (int samples +0x160/+0x164, history array +0x168,
 * count +0x16c). The old `[0xDCB808]` scratch is NOT used (scratch buffer).
 *
 * Design:
 *   - directs CreateDevice/CreateDeviceEx to patch DEVICE vtable slot 17
 *     (Present) to our hook and slot 16 (Reset) for pointer hygiene,
 *   - Present hook (ROUND 8): PURE OBSERVER — resolve the local player's
 *     chain, read all 8 decrypted resource slots plus the income object's
 *     raw ints, and log ONE `res P%d f=.. w=.. ..` line when they move by
 *     >= 0.5. NOTHING is drawn: no font, no render states, no render target,
 *     no scene, no device call beyond the forwarded Present/Reset. Dali's
 *     round-7 corruption (unit shatter / appear-disappear / grass-shader
 *     artifacts from OUR un-restored draw state) is impossible by
 *     construction — the game's rendering state is never touched,
 *   - an always-on `d3d9mod.log` next to the exe traces every stage so the
 *     observed numbers are provable, never blind.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <malloc.h>

typedef DWORD D3DCOLOR;

/* ------------------------- addressing (round-2 verified) ------------------ */
/* current-player chain (ground truth: FUN_005009c0 eco-score disasm at
 * 0x5009f1-0x500a2c for [player+0x230]; FUN_0086da39 call sites at 0x719376
* / 0x883548 for the income object at [player+0x80]; FUN_0044cb21 wrapper
 * for the player lookup):
 *
 *   game   = *(base + 0x866234)          [0xC66234] game object
 *   ctx    = *(game + 0x13c)
 *   idx    = *(game + 0x14c)             [OFF_GAME_ACTIVE_PLAYER] the engine's
 *                                         authority for "the player this
 *                                         client controls" in-game
 *   player = *(*(ctx + 0x58) + idx*4)    guard 0 <= idx < *(ctx + 0x5c)
 *   g_res  = *(player + 0x230)           live encrypted resource container
 *   g_inc  = *(player + 0x80)            income object (this of FUN_0086da39)
 *
 * ROUND-10 (probe round): FUN_0049a8c5 disasm (0x49a8e8/0x49a906/0x49a918)
 * shows [player+0x230]/[+0x238]/[+0x240] are THREE SEPARATE container POINTER
 * fields on the Player: +0x230 is the SET target (rmSetPlayerResource writes
 * the requested stock through FUN_0058ef83(this=+0x230), which derefs *this),
 * +0x238 is the cap/limit read, +0x240 is the running ADD accumulator
 * (FUN_0049a859). slot map (food=2, wood=0, coin=1, export=7) is proven by
 * the eco-score fn FUN_005009c0 (0x5009f1-0x500a2c sums slots 2,0,1,7).
 * Plus two independent human-player authorities to compare live:
 *   hall = *(ui_hall+0x920)+0x13c (FUN_0044cc6d mask &0x3ffff) — how the
 *          game's own resource panel (FUN_006f6216, 0x6f6234-0x6f6244)
 *          resolves the local player, reading stock from
 *          *(*(player+0xb4)+0x94)+4;
 *   uisel = [0xC66664] — the index the TOP-BAR income eval uses directly
 *          (0x883538/0x883582: FUN_0044cb21(ctx, [0xC66664]) -> player+0x80
 *          -> FUN_0086da39).
 * The observer logs all three containers (+0x230 primary, +0x238/+0x240 +
 * the +0xb4->+0x94->+4 panel chain as alt lines) and both alt indices, so a
 * single in-game run decides which of the candidates actually equals the HUD.
 *
 * ROUND-5 evidence for `*(game+0x14c)` (replacing the OLD [0xC66664]):
 *  - FUN_00405574 (per-frame UI player resolution) sets the UI current-player
 *    cache `*(*(hall+0xc1a0)+0x28)` to `*(int*)(DAT_00c66234+0x14c)` when
 *    `ctx && ctx[0x5c] >= 1`, else -1 (menu).
 *  - FUN_0045084c (player-select UI handler) calls
 *    `FUN_0044cb21(ctx, *(int*)(DAT_00c66234+0x14c))` guarded `-1 < idx`.
 *  - [0xC66664] is written ONLY by UI/input handlers (FUN_00720ca7,
 *    FUN_007212bf, FUN_004286f1/FUN_00429026) and reset to -1 — proven dead
 *    in-skirmish by the live log: `idx=-1 [chain-break: idx OOR n]`.
 */
#define RVA_GAME_PTR           0x00866234u   /* [0xC66234] game object      */
#define OFF_GAME_CTX           0x13c         /* game  -> context            */
#define OFF_GAME_ACTIVE_PLAYER 0x14c         /* game  -> active player index*/
#define OFF_CTX_PLAYERS        0x58          /* context -> players table    */
#define OFF_CTX_PLAYERCNT      0x5c          /* context -> player count     */
#define OFF_PLAYER_RES         0x230         /* player -> enc res container */
#define OFF_PLAYER_RES_CAP     0x238         /* player -> cap/limit cont ptr*/
#define OFF_PLAYER_RES_ADD     0x240         /* player -> running-total ptr */
#define OFF_PLAYER_INCOME      0x80          /* player -> income object     */
#define OFF_PLAYER_PANELOBJ    0xB4          /* player -> panel obj (0x6f62e2) */
#define OFF_PANEL_CONT         0x94          /* panelobj -> container holder */
#define OFF_PANEL_BASE         0x04          /* holder -> inline enc array    */
#define RVA_KEY_TABLE     0x0086DF14u   /* [0xC6DF14] decrypt key table       */
#define RVA_SLOT_COUNT    0x0086DF38u   /* slot count (8 at runtime)          */
#define RVA_HALL_PTR      0x00867310u   /* [0xC67310] UI hall object          */
#define OFF_HALL_SEL      0x920         /* hall -> current-selection object   */
#define OFF_HALL_LOCAL    0x13c         /* select -> hall-local player id     */
#define HALL_EMASK        0x3ffff       /* FUN_0044cc6d mask of that id       */
#define RVA_UI_SEL        0x00866664u   /* [0xC66664] UI current-player id    */
#define MAX_SLOTS         8u

/* income object fields + eval constants (FUN_0086da39 disasm 0x86da39...) */
#define OFF_INC_CUR       0x160         /* int, current sample value          */
#define OFF_INC_PREV      0x164         /* int, previous sample               */
#define OFF_INC_HIST      0x168         /* ptr, per-sample container array    */
#define OFF_INC_COUNT     0x16c         /* int, sample count                  */
#define OFF_CTX_SNAP      0x108         /* context -> current tick snapshot   */
#define RVA_TIME_STEP     0x0079A5C8u   /* 0.001f  samples<->game-time        */
#define RVA_NEG_FUDGE     0x007A12FCu   /* 2^32     unsigned re-map           */
#define RVA_WIN_THRESH    0x007A12E8u   /* ~1e-6f   window lower bound        */
#define RVA_DIV_CONST     0x007856BCu   /* 1.0f     rate dividend            */
#define RATE_WINDOW       60.0f         /* "1 Minute Income" window           */
#define MAX_HIST          1024u         /* history cap for guarded loop       */

/* ------------------------------ globals ---------------------------------- */
static HMODULE g_real   = NULL;      /* real d3d9.dll (system)        */
static void   *g_base   = NULL;      /* age3y.exe module base         */
static void   *g_ctx    = NULL;      /* context ([game+0x13c])        */
static void   *g_res    = NULL;      /* current player res container  */
static void   *g_inc    = NULL;      /* current player income object  */
static void   *g_device = NULL;      /* current IDirect3DDevice9*     */
static void  **g_devvt  = NULL;      /* patched device vtable         */

/* ROUND-8 observer snapshot state (shared by locate_resources_impl + hook).
 * ROUND-12: the old single resolved-player snapshot fields were superseded by
 * the per-player s_p_* arrays (below); s_snap_have survives purely as the
 * "fresh world, force-log this frame" latch for the per-player loop.         */
static int   g_player_idx = -1;         /* last resolved player index         */
static int   s_snap_have = 0;           /* fresh-world "force log" latch    */

/* ROUND-9 match-start tracking: the resolved player pointer + last ctx/n so
 * observer_sample can log a `--- match start n=%d idx=%s ---` separator and
 * reset the snapshot on ctx 0->non-0 / player-pointer change / n change.    */
static DWORD g_obs_player = 0;          /* last resolved player ptr (locate) */
static DWORD s_last_ctx = 0;            /* last observed ctx ptr             */
static DWORD s_last_player = 0;         /* last observed player ptr          */
static int   s_last_n = -1;             /* last observed player count        */

/* ROUND-11/12: memory-window dump infrastructure. Every 5s (and on match
 * start / match end) dump CHANGED cells in three windows around each player's
 * res/player/inc so Dali can correlate on-screen HUD stock to a raw offset.
 * ROUND-12: t is anchored at match start and NEVER re-anchored, so `t=` is
 * integer seconds elapsed since match start (the R11 `t=5 always` bug).
 * Per-player per-window snapshots indexed by the players-table slot 0..n-1. */
#define MAX_PLAYERS 16
static DWORD s_dump_tick = 0;           /* GetTickCount AT MATCH START (R12 t=) */
static int   s_last_snap_had = 0;       /* did last dump have valid data?    */
static DWORD s_pA_snap[MAX_PLAYERS][256]; /* res-window snapshot, per player  */
static DWORD s_pB_snap[MAX_PLAYERS][512]; /* player-window snapshot, per player */
static DWORD s_pC_snap[MAX_PLAYERS][128]; /* inc-window snapshot, per player   */

/* ROUND-12 per-player observer on-change snapshots (one per players-table
 * slot 0..n-1), so each player keeps its OWN first-logged + >=0.5-change
 * quiet state instead of the round-11 single resolved-player snapshot.     */
static int   s_p_have[MAX_PLAYERS];     /* player has a valid res snapshot   */
static DWORD s_p_res[MAX_PLAYERS];      /* container the snapshot is for     */
static float s_p_vals[MAX_PLAYERS][8];  /* last-logged decrypted slot floats */
static long  s_p_A[MAX_PLAYERS];        /* last inc cur (signed)             */
static long  s_p_B[MAX_PLAYERS];        /* last inc prev (signed)            */
static long  s_p_N[MAX_PLAYERS];        /* last inc count (signed)           */

/* --------------------------- diagnostic log ------------------------------- */
/* Always-on append log `d3d9mod.log` in the directory of whatever loads us
 * (== the game dir when running as age3y's proxy). Every write is
 * open/format/flush/close so a game crash can never lose the last stage.
 * ROUND 8: written only for chain transitions + the observer `res` samples
 * (on change >= 0.5), so steady state is silent. ASCII only — the log is
 * Dali's eyes.                                                              */
static FILE *g_log = NULL;

static void dlog(const char *fmt, ...) {
    if (g_log == NULL) {
        char path[MAX_PATH];
        if (GetModuleFileNameA(NULL, path, MAX_PATH) != 0) {
            char *slash = strrchr(path, '\\');
            if (slash != NULL) { slash[1] = '\0'; lstrcatA(path, "d3d9mod.log"); }
            else lstrcpyA(path, "d3d9mod.log");
            g_log = fopen(path, "a");
        }
    }
    if (g_log == NULL) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
    fclose(g_log);
    g_log = NULL;
}

/* ------------------ ROUND-6 crash catch + read breadcrumbs ---------------- */
/* age3y can exit silently mid-run with no dialog = an AV inside our DLL
 * (round-5 shipped the first live res and the game died; rounds 6-8 proved it
 * was OUR draw layer, now deleted). SetUnhandledExceptionFilter runs from
 * DllMain BEFORE anything else and pins the exact dying call via the g_step
 * breadcrumb before ExitProcess(0). All round-8 reads are VirtualQuery-guarded
 * anyway, so a stale pointer can never fault.                              */
static volatile char g_step[64];
static LPTOP_LEVEL_EXCEPTION_FILTER s_prev_filter = NULL;

static void set_step(const char *s) {
    size_t n = (s != NULL) ? strlen(s) : 0;
    if (n >= sizeof(g_step)) n = sizeof(g_step) - 1;
    memcpy((void *)g_step, (s != NULL) ? s : "", n);
    g_step[n] = '\0';
}

static long WINAPI fault_filter(struct _EXCEPTION_POINTERS *ep) {
    DWORD addr = 0, code = 0;
    if (ep != NULL && ep->ExceptionRecord != NULL) {
        addr = (DWORD)(DWORD_PTR)ep->ExceptionRecord->ExceptionAddress;
        code = ep->ExceptionRecord->ExceptionCode;
    }
    dlog("FAULT addr=%08X breadcrumb=%s code=%08X",
         (unsigned)addr, (const char *)g_step, (unsigned)code);
    ExitProcess(0);                          /* silent exit, no dialog */
    return EXCEPTION_CONTINUE_SEARCH;        /* never reached */
}

/* ------------------------- guarded reads -------------------------------
 * Deterministic guard (VirtualQuery). NOTE: MinGW's __try1/__except1 were
 * verified empirically NOT to work on i686-w64-mingw32 16.2.0 (uncaught
 * access violations / phantom catches once inlined) so the guard is the
 * authoritative protection: committed + readable pages only.             */
static BOOL page_readable(DWORD addr) {
    MEMORY_BASIC_INFORMATION mbi;
    if (addr == 0) return FALSE;
    if (VirtualQuery((LPCVOID)(DWORD_PTR)addr, &mbi, sizeof(mbi)) != sizeof(mbi))
        return FALSE;
    if (mbi.State != MEM_COMMIT) return FALSE;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return FALSE;
    switch (mbi.Protect & 0xFF) {
        case PAGE_READONLY: case PAGE_READWRITE: case PAGE_WRITECOPY:
        case PAGE_EXECUTE: case PAGE_EXECUTE_READ: case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            return TRUE;
        default:
            return FALSE;
    }
}
static BOOL read32(DWORD addr, DWORD *out) {
    if (addr == 0) return FALSE;
    if (!page_readable(addr)) return FALSE;   /* never touch bad pages */
    *out = *(volatile DWORD *)(DWORD_PTR)addr;
    return TRUE;
}
static DWORD safe_r32(DWORD addr) { DWORD v = 0; read32(addr, &v); return v; }
#ifdef SWARM_TEST
static float safe_rf(DWORD addr)  { DWORD v = 0; read32(addr, &v); return *(float *)&v; }
static void *safe_rptr(DWORD addr){ DWORD v = 0; if (!read32(addr, &v)) return NULL;
                                    return (void *)v; }
#endif

/* --------------------------- decrypt (game-exact) -------------------------- */
/* value[slot] = float( key[slot] ^ res[slot] ) — matches FUN_0044efff. The
 * slot-0 probe is the "food > 0" sanity check used to find the live player;
 * decrypt_slot_at() takes an arbitrary container so probe/fallback can scan
 * the whole players table without touching g_res.                        */
static float decrypt_slot_at(DWORD rbase, int slot) {
    DWORD base  = g_base ? (DWORD)(DWORD_PTR)g_base : 0;
    DWORD count = base ? safe_r32(base + RVA_SLOT_COUNT) : 0;
    if (count == 0 || count > MAX_SLOTS) count = MAX_SLOTS;
    if (base == 0 || rbase == 0 || slot < 0 || (DWORD)slot >= count) return 0.0f;
    DWORD key = safe_r32(base + RVA_KEY_TABLE + (DWORD)slot * 4);
    DWORD enc = safe_r32(rbase + (DWORD)slot * 4);
    DWORD dec = enc ^ key;
    return *(float *)&dec;
}
#ifdef SWARM_TEST
/* production observer uses decrypt_slot_at() directly; these convenience
 * wrappers exist for the test harnesses only */
static float decrypt_slot(int slot) {
    return decrypt_slot_at(g_res ? (DWORD)(DWORD_PTR)g_res : 0, slot);
}
#endif

/* ------------------------- resource location ------------------------------ */
/* Resolution of the CURRENT PLAYER's live data (see addressing block above).
 * Every deref is a guarded read; any broken/null link yields NULL/0 cleanly.
 *
 * ROUND-5 rework: the live log proved `idx=*(base+0x866664)` is -1 inside a
 * skirmish (`chain ... idx=-1 arr=00000000 [chain-break: idx OOR n]`):
 * [0xC66664] is the UI's *selection* marker, reset to -1 outside input-side
 * moments. The engine's own per-frame player resolution (FUN_00405574) keys
 * the in-game active player off `*(game + 0x14c)` instead — that is now the
 * authoritative index HERE. probe-lite fallback: if the resolved active
 * player fails the sanity probe (income +0x80 non-NULL, container +0x230
 * non-NULL, live decrypted slot-0 food > 0), iterate the ctx players and
 * take the FIRST sane one (`[src-fallback] [fallback: player#%d]`). The
 * main menu (no players, n == 0) still resolves to NULL/0 — the observer
 * then logs nothing new (round-8), only quiet chain transitions.
 *
 * ROUND-7: an in-range authoritative idx is now used UNCONDITIONALLY — the
 * old food>0 sanity probe wrongly REJECTED the live human (a slot may legally
 * be 0, e.g. coin at game start), which flipped the HUD between players via
 * the fallback. The probe now only gates the fallback for idx-OOR/broken.
 *
 * ROUND-8: resolution still drives the observer's P%d number. no drawing.
 */

/* chain-log state (ROUND-5 QUIET spec): log the first 5 chain calls, then
 * ONLY when player/res/inc (or the src tag) change. No steady-state spam. */
static unsigned long g_chain_calls = 0;
static DWORD g_last_c[3];            /* player res inc */
static char  g_last_tag[128];        /* last src tag, copied (stack-safe) */

static void dlog_chain(DWORD game, DWORD ctx, int n,
                       DWORD player, DWORD res, DWORD inc,
                       const char *tag) {
    DWORD cur[3] = { player, res, inc };
    int first = (g_chain_calls == 0);
    g_chain_calls++;
    int changed = first;
    if (!changed) {
        for (int i = 0; i < 3; i++)
            if (cur[i] != g_last_c[i]) { changed = 1; break; }
        const char *curtag = tag ? tag : "";
        if ((tag != NULL) != (g_last_tag[0] != '\0')) changed = 1;
        else if (strcmp(curtag, g_last_tag) != 0) changed = 1;
    }
    if (g_chain_calls > 5 && !changed) return;   /* quiet in steady state */
    memcpy(g_last_c, cur, sizeof(cur));
    g_last_tag[0] = '\0';
    if (tag != NULL) strncpy(g_last_tag, tag, sizeof(g_last_tag) - 1);
    /* ROUND-9: every chain line carries the raw *(game+0x14c) as `idx=N`
     * (or `idx=-` when ctx==0) so we can see WHY an authoritative idx isn't
     * winning: is it out of 0..n, or is a probe still vetoing in-range?
     * ROUND-10: also log the two other human-player authorities the game
     * uses in-match — `hall=` = *(*(0xC67310)+0x920)+0x13c masked (FUN_
     * 0044cc6d / FUN_006f6216 panel resolution) and `uisel=` = [0xC66664]
     * (the top-bar income eval, 0x883538). -1 means the chain link is 0. */
    char idxbuf[64];
    if (ctx != 0 && game != 0)
        _snprintf(idxbuf, sizeof(idxbuf), "%d",
                  (int)safe_r32(game + OFF_GAME_ACTIVE_PLAYER));
    else
        strncpy(idxbuf, "-", sizeof(idxbuf));
    {
        DWORD bb  = g_base ? (DWORD)(DWORD_PTR)g_base : 0;
        long  hall = -1, uisel = -1;
        if (bb) {
            DWORD uih = safe_r32(bb + RVA_HALL_PTR);
            DWORD sel = (uih != 0) ? safe_r32(uih + OFF_HALL_SEL) : 0;
            if (sel != 0) hall = (long)(safe_r32(sel + OFF_HALL_LOCAL) & HALL_EMASK);
            uisel = (long)safe_r32(bb + RVA_UI_SEL);
        }
        _snprintf(idxbuf + strlen(idxbuf),
                  sizeof(idxbuf) - strlen(idxbuf),
                  " hall=%d uisel=%d", (int)hall, (int)uisel);
    }
    dlog("chain src=game+0x14C game=%08X ctx=%08X n=%d idx=%s player=%08X res=%08X inc=%08X%s%s",
         (unsigned)game, (unsigned)ctx, n, idxbuf, (unsigned)player,
         (unsigned)res, (unsigned)inc,
         tag ? " " : "", tag ? tag : "");
}

/* ROUND-7 sticky fallback state + probe. The probe (live income +0x80, live
 * container +0x230, decrypted slot-0 food > 0) ONLY gates the fallback scan
 * now — it is NOT allowed to reject an in-range authoritative idx.         */
static DWORD s_fb_last = 0;                 /* last player the fallback kept */
static int player_sane(DWORD p) {
    if (p == 0) return 0;
    DWORD r = safe_r32(p + OFF_PLAYER_RES);
    DWORD c = safe_r32(p + OFF_PLAYER_INCOME);
    return (r != 0 && c != 0 && decrypt_slot_at(r, 0) > 0.0f) ? 1 : 0;
}

static void locate_resources_impl(void) {
    g_res = NULL;
    g_inc = NULL;
    g_ctx = NULL;
    g_player_idx = -1;               /* ROUND-8: resolved player for the P%d tag */
    g_obs_player = 0;                /* ROUND-9: resolved player ptr for match-start */
    if (g_base == NULL) return;
    DWORD base     = (DWORD)(DWORD_PTR)g_base;
    DWORD game     = safe_r32(base + RVA_GAME_PTR);
    DWORD ctx = 0, arr = 0, player = 0, res = 0, inc = 0;
    int   n = 0, idx = -1;

    if (game == 0) { dlog_chain(0, 0, 0, 0, 0, 0, "[chain-break: game==0]"); return; }

    ctx = safe_r32(game + OFF_GAME_CTX);
    if (ctx == 0) { dlog_chain(game, 0, 0, 0, 0, 0, "[chain-break: ctx==0]"); return; }
    g_ctx = (void *)ctx;

    n   = (int)safe_r32(ctx + OFF_CTX_PLAYERCNT);
    arr = safe_r32(ctx + OFF_CTX_PLAYERS);
    if (n <= 0) { dlog_chain(game, ctx, n, 0, 0, 0, "[chain-break: n<=0]"); return; }
    if (arr == 0) { dlog_chain(game, ctx, n, 0, 0, 0, "[chain-break: arr==0]"); return; }

    /* ROUND-7 AUTHORITATIVE: an in-range idx IS the human (FUN_00405574 /
     * FUN_0045084c) — use it unconditionally. Only the guarded derefs protect
     * us; an OOB/unreadable idx drops to the sticky fallback below.        */
    idx = (int)safe_r32(game + OFF_GAME_ACTIVE_PLAYER);
    if (idx >= 0 && idx < n) {
        player = safe_r32(arr + (DWORD)idx * 4);
        res = (player != 0) ? safe_r32(player + OFF_PLAYER_RES)    : 0;
        inc = (player != 0) ? safe_r32(player + OFF_PLAYER_INCOME) : 0;
        if (player != 0) {
            g_res = (void *)res;                 /* may be NULL if unreadable */
            g_inc = (void *)inc;
            g_player_idx = idx;                  /* ROUND-8: authoritative P */
            g_obs_player = player;               /* ROUND-9: match-start track */
            dlog_chain(game, ctx, n, player, res, inc, "[src-authoritative]");
            return;
        }
    }

    /* ROUND-7 STICKY fallback (idx OOR/broken only): scan for a sane player
     * but PREFER the last-picked one — player pointers are stable, so the
     * human keeps its identity instead of "first sane" flickering between
     * players. Logs [sticky] when the previous pick is re-adopted.          */
    {
        int pick_i = -1;
        DWORD pick = 0;
        if (s_fb_last != 0) {
            for (int i = 0; i < n; i++) {
                player = safe_r32(arr + (DWORD)i * 4);
                if (player == s_fb_last && player_sane(player)) {
                    pick = player; pick_i = i; break;
                }
            }
        }
        if (pick == 0) {
            for (int i = 0; i < n; i++) {
                player = safe_r32(arr + (DWORD)i * 4);
                if (player_sane(player)) { pick = player; pick_i = i; break; }
            }
        }
        if (pick != 0 && pick_i >= 0) {
            int sticky = (pick == s_fb_last);
            s_fb_last = pick;
            res = safe_r32(pick + OFF_PLAYER_RES);
            inc = safe_r32(pick + OFF_PLAYER_INCOME);
            g_res = (void *)res;
            g_inc = (void *)inc;
            g_player_idx = pick_i;               /* ROUND-8: fallback P */
            g_obs_player = pick;                 /* ROUND-9: match-start track */
            char tag[80];
            _snprintf(tag, sizeof(tag), "[src-fallback] [fallback: player#%d]%s",
                      pick_i, sticky ? " [sticky]" : "");
            dlog_chain(game, ctx, n, pick, res, inc, tag);
            return;
        }
    }
    dlog_chain(game, ctx, n, 0, 0, 0, "[chain-break: no sane player]");
}

static void locate_resources(void) {
    g_base = (void *)GetModuleHandleA("age3y.exe");
    locate_resources_impl();
}

#ifdef SWARM_TEST
/* test-only: run the EXACT production chain against a caller-supplied base
 * (the test lays a fake world at the real RVAs), no GetModuleHandleA.        */
static void locate_resources_at(void *base) {
    g_base = base;
    locate_resources_impl();
}
#endif

#ifdef SWARM_TEST
/* ----------------------- rate fields (round-2), test-only ------------------ */
/* Reproduces FUN_0086da39 ("X Minute Income" eval, disasm 0x86da39-0x86dbd3)
 * for a single resource slot, window = 60s:
 *   acc = 0; slotsum[slot] = 0;
 *   for j = count-1 .. 0 while (acc < window):
 *       v  = (j == count-1) ? (snap - prev) + cur : cur;   // int samples
 *       f  = (float)v;  if (v < 0) f += 2^32;             // unsigned re-map
 *       acc += f * 0.001f;
 *       slotsum[slot] += decrypt(hist[j], slot);          // per-sample container
 *   return slotsum[slot] * (1.0f / acc);                  // avg per game-time
 * All int fields are read as signed ints (fild), guarded reads everywhere.
 * ROUND-8: the observer logs the RAW inc fields directly (incA/incB/incN); the
 * harvested-rate function survives ONLY as this SWARM_TEST harness proof that
 * FUN_0086da39's math was reproduced correctly in round 2.                  */
static float rate_for(int slot) {
    DWORD base = g_base ? (DWORD)(DWORD_PTR)g_base : 0;
    if (base == 0 || slot < 0 || (DWORD)slot >= MAX_SLOTS) return 0.0f;
    DWORD inc = g_inc ? (DWORD)(DWORD_PTR)g_inc : 0;
    if (inc == 0) return 0.0f;

    int count = (int)safe_r32(inc + OFF_INC_COUNT);        /* [inc+0x16c]  */
    if (count < 1) return 0.0f;

    float window = RATE_WINDOW;
    float thr = safe_rf(base + RVA_WIN_THRESH);            /* 0xBA12E8     */
    if (!(window >= thr)) return 0.0f;                      /* fcomp guard  */

    float tstep = safe_rf(base + RVA_TIME_STEP);            /* 0xB9A5C8     */
    if (!(tstep >= 0.0f) || !(tstep <= 1e3f)) tstep = 0.001f;
    float fudge = safe_rf(base + RVA_NEG_FUDGE);            /* 0xBA12FC     */
    float divc  = safe_rf(base + RVA_DIV_CONST);            /* 0xB856BC     */
    if (!(divc > 0.0f) || !(divc <= 1e10f)) divc = 1.0f;

    DWORD ctx  = g_ctx ? (DWORD)(DWORD_PTR)g_ctx : 0;
    int   snap = ctx ? (int)safe_r32(ctx + OFF_CTX_SNAP) : 0;
    int   cur  = (int)safe_r32(inc + OFF_INC_CUR);
    int   prev = (int)safe_r32(inc + OFF_INC_PREV);
    DWORD hist = (DWORD)(DWORD_PTR)safe_rptr(inc + OFF_INC_HIST);

    DWORD ns = safe_r32(base + RVA_SLOT_COUNT);
    if (ns == 0 || ns > MAX_SLOTS) ns = MAX_SLOTS;
    if ((DWORD)slot >= ns) return 0.0f;

    float acc = 0.0f;
    float sum = 0.0f;
    int   j   = count - 1;
    int   guard = MAX_HIST;
    while (j >= 0 && guard-- > 0 && acc < window) {
        int v = (j == count - 1) ? (snap - prev) + cur : cur;
        float f = (float)v;
        if (v < 0) f += fudge;
        acc += f * tstep;
        if (hist != 0) {
            DWORD cont = (DWORD)(DWORD_PTR)safe_rptr(hist + (DWORD)j * 4);   /* sample j */
            if (cont != 0) {
                DWORD key = safe_r32(base + RVA_KEY_TABLE + (DWORD)slot * 4);
                DWORD enc = safe_r32(cont + (DWORD)slot * 4);
                DWORD dec = enc ^ key;
                sum += *(float *)&dec;
            }
        }
        j--;
    }
    if (!(acc > 0.0f)) return 0.0f;                          /* div-0 guard */
    float rate = sum * (divc / acc);
    if (!(rate <= 1e7f) || !(rate >= -1e7f)) rate = 0.0f;
    return rate;
}
#endif /* SWARM_TEST (rate_for is harness-only since round-8 observer) */

/* ----------------------------- ROUND-8 observer ---------------------------- */
/* Dali's mission (round 8): NO rendering at all. This DLL only READS the
 * local player's encrypted container and income object every Present and
 * LOGS a line when the numbers move by >= 0.5. The game's device state is
 * NEVER touched — no font, no render states, no render target, no scene.
 * All previous draw code (font init, text=/values=/gotbb dumps, red rect,
 * SetRenderTarget/SetRenderState, Begin/EndScene) was DELETED in round 8;
 * nothing render-y may reappear.                                            */
/* Read all 8 decrypted slots of the current player's container + the raw
 * income-object ints; on ANY change (>= 0.5 abs on a slot, or an int moved)
 * log TWO lines per sample:
 *   `res food=%d wood=%d coin=%d export=%d`
 *       named via the round-2 decompile map: food=slot2, wood=slot0,
 *       coin=slot1, export=slot7 (R9: slot0 is NOT food — Dali's log).
 *   `raw 0=%d 1=%d 2=%d 3=%d 4=%d 5=%d 6=%d 7=%d incA=%d incB=%d incN=%d`
 *       the untagged trail so a wrong label can be fixed with ZERO rebuilds.
 * Match-start: on ctx 0->non-0, resolved player change, or n change, log
 *   `--- match start n=%d idx=%d ---` (idx = raw *(game+0x14c), "-" if ctx 0)
 *   and reset the snapshot so the NEXT frame logs regardless of threshold.
 * Pure guarded reads, zero writes. Menu (g_res/g_inc NULL) logs the separator
 * only (match-start is detected before the container guard runs).           */
static int rnd_i(float x) {           /* R9: round decrypted floats to int */
    return (x >= 0.0f) ? (int)(x + 0.5f) : (int)(x - 0.5f);
}

/* no libm: bit-level Inf/NaN check (exponent all-ones) + plain range clamp */
static int is_dumpable(DWORD cu) {
    if ((cu & 0x7F800000u) == 0x7F800000u) return 0;   /* Inf or NaN */
    float x = *(float *)&cu;
    return (x > -1e6f && x < 1e6f);
}

/* signed-hex cell tag: `%+03X` is ignored for hex conversions by the CRT,
 * so build `res+130` / `res-018` explicitly. */
static void fmt_cell(char *dst, size_t n, const char *tag, int off) {
    if (off < 0) _snprintf(dst, n, "%s-%04X", tag, (unsigned)(-off));
    else         _snprintf(dst, n, "%s+%04X", tag, (unsigned)off);
}

/* ROUND-12: dump ONE window (center +/- span) for ONE player, logging only
 * CHANGED cells whose int or rounded-float value falls in [0, 20000]. The
 * tag is `r` (res container), `p` (player object) or `i` (income object).
 * `addr=` is the ACTUAL computed address (window center + offset), fixing
 * the R11 bug where `va=` printed the raw dword instead of the address.
 * Returns 1 if any cell was logged. Every read is VirtualQuery-guarded. */
static int dump_window_cells(int pdx, const char *tag, DWORD center,
                             int lo, int nwords, DWORD *snap) {
    if (center == 0) return 0;
    int hi = lo + (nwords - 1) * 4;
    if (!page_readable(center + lo) || !page_readable(center + hi)) return 0;
    DWORD *cur = (DWORD *)_alloca((size_t)nwords * sizeof(DWORD));
    for (int i = 0; i < nwords; i++)
        read32(center + lo + (DWORD)i * 4, &cur[i]);
    int any = 0;
    for (int i = 0; i < nwords; i++) {
        char ctag[20];
        fmt_cell(ctag, sizeof(ctag), tag, lo + i * 4);
        DWORD cv = cur[i];
        if (cv == snap[i]) continue;
        int   si = (int)cv;
        float sf = *(float *)&cv;
        int iv = (si >= 0 && si <= 20000) ? 1 : 0;
        int fv = (is_dumpable(cv) && sf >= 0.0f && sf <= 20000.0f &&
                  (int)(sf + 0.5f) == sf) ? 1 : 0;
        if (!iv && !fv) continue;
        dlog("cell P%d %s addr=%08X int=%d float=%.1f",
             pdx, ctag,
             (unsigned)(center + lo + (DWORD)i * 4),
             si, (double)sf);
        any = 1;
    }
    memcpy(snap, cur, (size_t)nwords * sizeof(DWORD));
    return any;
}

/* ROUND-12 stock-hunt bonus: the FIRST 8 dwords of the res container + the 8
 * dwords at container+0x80, logged as PLAIN signed ints (NO decrypt) so a
 * 600/200/30 stock stored as a plain int slice is caught verbatim. Always
 * logged at every dump tick / match end for every player.                 */
static void log_resraw(int pdx, DWORD rr) {
    if (rr == 0) return;
    dlog("P%d resraw 0=%d 1=%d 2=%d 3=%d 4=%d 5=%d 6=%d 7=%d",
         pdx, (int)safe_r32(rr + 0x00), (int)safe_r32(rr + 0x04),
         (int)safe_r32(rr + 0x08), (int)safe_r32(rr + 0x0C),
         (int)safe_r32(rr + 0x10), (int)safe_r32(rr + 0x14),
         (int)safe_r32(rr + 0x18), (int)safe_r32(rr + 0x1C));
    dlog("P%d resraw2 0=%d 1=%d 2=%d 3=%d 4=%d 5=%d 6=%d 7=%d",
         pdx, (int)safe_r32(rr + 0x80), (int)safe_r32(rr + 0x84),
         (int)safe_r32(rr + 0x88), (int)safe_r32(rr + 0x8C),
         (int)safe_r32(rr + 0x90), (int)safe_r32(rr + 0x94),
         (int)safe_r32(rr + 0x98), (int)safe_r32(rr + 0x9C));
}

/* ROUND-12: dump every player 0..n-1's three windows (res/player/inc) plus
 * the resraw stock-hunt lines. Header once; `cell P0-2 none` when nothing
 * changed anywhere so the burst can never flood while paused.            */
static void dump_all_players(DWORD arr, int n, DWORD now_ms) {
    int lim = (n < MAX_PLAYERS) ? n : MAX_PLAYERS;
    if (lim <= 0) return;
    dlog("--- dump t=%d mode=all ---", (int)((now_ms - s_dump_tick) / 1000u));
    int any = 0;
    for (int i = 0; i < lim; i++) {
        DWORD p = safe_r32(arr + (DWORD)i * 4);
        if (p == 0) continue;
        DWORD rr = safe_r32(p + OFF_PLAYER_RES);
        DWORD ii = safe_r32(p + OFF_PLAYER_INCOME);
        log_resraw(i, rr);                                   /* stock hunt     */
        /* window A: 256 dwords centered on res_container (tag r) */
        if (dump_window_cells(i, "r", rr, -0x200, 256, s_pA_snap[i])) any = 1;
        /* window B: 512 dwords centered on player object (tag p) */
        if (dump_window_cells(i, "p", p,  -0x200, 512, s_pB_snap[i])) any = 1;
        /* window C: 128 dwords centered on inc object (tag i) */
        if (dump_window_cells(i, "i", ii, -0x100, 128, s_pC_snap[i])) any = 1;
    }
    if (!any) dlog("cell P0-%d none", lim - 1);
}

static void observer_sample(void) {
    DWORD base  = g_base ? (DWORD)(DWORD_PTR)g_base : 0;
    DWORD game  = base ? safe_r32(base + RVA_GAME_PTR) : 0;
    DWORD ctx   = g_ctx ? (DWORD)(DWORD_PTR)g_ctx : 0;
    int   n     = ctx ? (int)safe_r32(ctx + OFF_CTX_PLAYERCNT) : 0;
    DWORD now_ms = GetTickCount();

    /* ROUND-9 match-start: detect FIRST (before any container guard) so a
     * menu->game transition / player switch / player-count change always
     * prints the separator and forces the next frame to log afresh.         */
    int match_start = ((s_last_ctx == 0) && (ctx != 0)) ||
                      (g_obs_player != 0 && g_obs_player != s_last_player) ||
                      (n != s_last_n);
    int match_end = (s_snap_have && (g_res == NULL || g_inc == NULL));
    if (match_start) {
        char idxbuf[16];
        if (ctx != 0 && game != 0)
            _snprintf(idxbuf, sizeof(idxbuf), "%d",
                      (int)safe_r32(game + OFF_GAME_ACTIVE_PLAYER));
        else
            strcpy(idxbuf, "-");
        dlog("--- match start n=%d idx=%s ---", n, idxbuf);
        s_last_ctx = ctx;
        s_last_player = g_obs_player;
        s_last_n = n;
        s_snap_have = 0;                 /* next frame logs regardless */
        s_dump_tick = now_ms;            /* R12: t= anchor = seconds INTO match
                                         * (ANCHOR ONLY — never re-anchored, so
                                         * `t=` increments: 0,5,10,...). */
        for (int i = 0; i < MAX_PLAYERS; i++) {   /* fresh per-player state    */
            s_p_have[i] = 0; s_p_res[i] = 0;
            memset(s_pA_snap[i], 0, sizeof(s_pA_snap[i]));
            memset(s_pB_snap[i], 0, sizeof(s_pB_snap[i]));
            memset(s_pC_snap[i], 0, sizeof(s_pC_snap[i]));
        }
    }

    DWORD arr = (ctx != 0) ? safe_r32(ctx + OFF_CTX_PLAYERS) : 0;
    DWORD count = base ? safe_r32(base + RVA_SLOT_COUNT) : 0;
    if (count == 0 || count > MAX_SLOTS) count = MAX_SLOTS;
    int lim = (n < MAX_PLAYERS) ? n : MAX_PLAYERS;

    /* ROUND-12: no single-player authority any more — the R11 live log proved
     * the resolver's pick (player#2) was the AI, whose stock (0..~95) never
     * matched Dali's HUD (600/200/30). So loop ALL players 0..n-1 and log the
     * P%d lines for each, keeping an INDEPENDENT quiet/on-change snapshot per
     * player (first-valid always logs, then only >=0.5 / inc-field changes). */
    if (ctx != 0 && arr != 0 && n > 0 && base != 0 && count != 0) {
        int force = (s_snap_have == 0);
        for (int i = 0; i < lim; i++) {
            DWORD p = safe_r32(arr + (DWORD)i * 4);
            if (p == 0) { s_p_have[i] = 0; continue; }
            DWORD rr = safe_r32(p + OFF_PLAYER_RES);
            DWORD ii = safe_r32(p + OFF_PLAYER_INCOME);
            if (rr == 0 || ii == 0) { s_p_have[i] = 0; continue; }  /* need a
                                       * readable container + income to log   */

            float v[8];
            for (int s = 0; s < 8; s++)
                v[s] = decrypt_slot_at(rr, s);
            long A = (long)safe_r32(ii + OFF_INC_CUR);
            long B = (long)safe_r32(ii + OFF_INC_PREV);
            long N = (long)safe_r32(ii + OFF_INC_COUNT);

            int changed = force || !s_p_have[i] || s_p_res[i] != rr;
            if (!changed)
                for (int s = 0; s < 8; s++) {
                    float d = v[s] - s_p_vals[i][s];
                    if (d < 0.0f) d = -d;
                    if (d >= 0.5f) { changed = 1; break; }
                }
            if (!changed && (A != s_p_A[i] || B != s_p_B[i] || N != s_p_N[i]))
                changed = 1;
            if (!changed)
                continue;                       /* this player quiet this frame */

            s_p_have[i] = 1; s_p_res[i] = rr;
            for (int s = 0; s < 8; s++) s_p_vals[i][s] = v[s];
            s_p_A[i] = A; s_p_B[i] = B; s_p_N[i] = N;

            /* R9 named line (decompile map: food=2, wood=0, coin=1, export=7),
             * now P%d-prefixed and per player (ROUND-12).                    */
            dlog("P%d res food=%d wood=%d coin=%d export=%d",
                 i, rnd_i(v[2]), rnd_i(v[0]), rnd_i(v[1]), rnd_i(v[7]));
            /* R9 raw trail line — all 8 slots + income fields, per player     */
            dlog("P%d raw 0=%d 1=%d 2=%d 3=%d 4=%d 5=%d 6=%d 7=%d incA=%d incB=%d incN=%d",
                 i, (int)v[0], (int)v[1], (int)v[2], (int)v[3],
                 (int)v[4], (int)v[5], (int)v[6], (int)v[7],
                 (int)A, (int)B, (int)N);

            /* ROUND-10 probe: altCap (+0x238) / altAdd (+0x240) / panel chain
             * (+0xb4->+0x94->+4) — now with the player's own index as P%d.    */
            DWORD cap = safe_r32(p + OFF_PLAYER_RES_CAP);
            DWORD add = safe_r32(p + OFF_PLAYER_RES_ADD);
            if (cap != 0)
                dlog("P%d altCap 0=%d 1=%d 2=%d 3=%d 4=%d 5=%d 6=%d 7=%d",
                     i, (int)decrypt_slot_at(cap,0), (int)decrypt_slot_at(cap,1),
                     (int)decrypt_slot_at(cap,2), (int)decrypt_slot_at(cap,3),
                     (int)decrypt_slot_at(cap,4), (int)decrypt_slot_at(cap,5),
                     (int)decrypt_slot_at(cap,6), (int)decrypt_slot_at(cap,7));
            else
                dlog("P%d altCap skip", i);
            if (add != 0)
                dlog("P%d altAdd 0=%d 1=%d 2=%d 3=%d 4=%d 5=%d 6=%d 7=%d",
                     i, (int)decrypt_slot_at(add,0), (int)decrypt_slot_at(add,1),
                     (int)decrypt_slot_at(add,2), (int)decrypt_slot_at(add,3),
                     (int)decrypt_slot_at(add,4), (int)decrypt_slot_at(add,5),
                     (int)decrypt_slot_at(add,6), (int)decrypt_slot_at(add,7));
            else
                dlog("P%d altAdd skip", i);
            /* FUN_006f6216 panel chain: this = *(*(player+0xb4)+0x94) + 4   */
            {
                DWORD po = safe_r32(p + OFF_PLAYER_PANELOBJ);
                DWORD hd = (po != 0) ? safe_r32(po + OFF_PANEL_CONT) : 0;
                DWORD pb = (hd != 0) ? hd + OFF_PANEL_BASE : 0;
                if (pb != 0 && page_readable(pb) &&
                    page_readable(pb + (MAX_SLOTS - 1) * 4))
                    dlog("P%d altPanel 0=%d 1=%d 2=%d 3=%d 4=%d 5=%d 6=%d 7=%d",
                         i, (int)decrypt_slot_at(pb,0), (int)decrypt_slot_at(pb,1),
                         (int)decrypt_slot_at(pb,2), (int)decrypt_slot_at(pb,3),
                         (int)decrypt_slot_at(pb,4), (int)decrypt_slot_at(pb,5),
                         (int)decrypt_slot_at(pb,6), (int)decrypt_slot_at(pb,7));
                else
                    dlog("P%d altPanel skip", i);
            }
        }
        s_snap_have = 1;
    } else {
        s_snap_have = 0;                 /* menu / no players: nothing new     */
    }

dump_check:
    /* ROUND-12: dump at (a) match start (t=0), (b) every ~5000 ms, (c) on
     * match-end. For (a)+(b)+(c) we dump ALL players 0..n-1 (not just the
     * resolved one — R11 proved that player was the AI). s_dump_tick is the
     * match-start anchor and is NEVER re-anchored here, so the header `t=`
     * increments through the match (0,5,10,...). The burst is silent (cells
     * only on change; `cell P0-2 none` when nothing moved while paused).   */
    int do_dump = match_start || ((now_ms - s_dump_tick) >= 5000u) || match_end;
    if (do_dump && ctx != 0 && arr != 0 && n > 0) {
        dump_all_players(arr, n, now_ms);
        s_last_snap_had = 1;
        /* NOTE: s_snap_have intentionally LEFT at its current value here —
         * the cadence dump must NOT force a fresh P-line re-log (R12 keeps the
         * P-lines quiet/on-change per player; only a new match_start resets
         * them). The dump's own changed-cell quiet is handled inside
         * dump_all_players via per-player window snapshots.                */
    } else if (match_end) {
        /* world torn down (menu): res/inc NULL — nothing to scan, just end   */
        s_last_snap_had = 0;
    }
    /* NOTE: s_dump_tick is NOT re-anchored after a dump (R12 bug fix #1).   */
    return;
}

/* -------------------------- device vtable hook ---------------------------- */
/* IDirect3DDevice9 method 17 = Present, method 16 = Reset. These are the ONLY
 * device slots we touch. Reset exists purely for pointer hygiene (g_device and
 * the hook assertions stay valid across alt-tab / fullscreen toggles).       */
typedef int  (STDMETHODCALLTYPE *PRESENT_FN)(void *self, const RECT *a,
        const RECT *b, HWND hwnd, const void *dirty);
typedef int  (STDMETHODCALLTYPE *DEV_RESET)(void *, const void *);
static PRESENT_FN s_orig_present = NULL;
static DEV_RESET   s_orig_reset   = NULL;
static int patch_device_present(void *dev);   /* used by reset_hook below */

/* Device Reset (alt-tab / fullscreen toggle / resolution change): keep
 * g_device current and re-assert the vtable hooks (Reset may swap vt).
 * ROUND 8: no font exists to invalidate — the game's device is untouched.   */
static int STDMETHODCALLTYPE reset_hook(void *self, const void *pp) {
    g_device = self;
    int hr = (s_orig_reset != NULL) ? s_orig_reset(self, pp) : (int)0x8876086c;
    patch_device_present(self);              /* re-assert Present/Reset hooks */
    return hr;
}

/* ROUND 8 pure-observer Present: resolve the local player's chain, read the
 * 8 decrypted resource slots + income ints, LOG on change (>= 0.5 abs).
 * Nothing is drawn and no device state is ever changed — the game must run
 * 100% pristine (Dali's acceptance: no shatter / flicker / grass artifacts).*/
static int STDMETHODCALLTYPE present_hook(void *self, const RECT *a, const RECT *b,
        HWND hwnd, const void *dirty) {
    g_device = self;
    locate_resources();                      /* menu -> quiet chain lines only */
    set_step("observer-sample");
    observer_sample();                       /* read+log only, no drawing      */
    set_step("present-before");
    int hr = (s_orig_present != NULL)
        ? s_orig_present(self, a, b, hwnd, dirty)
        : (int)0x8876086c;    /* D3DERR_INVALIDCALL */
    set_step("present-after");
    return hr;
}

static int patch_device_present(void *dev) {
    if (dev == NULL) return 0;
    void **vt = *(void ***)dev;
    if (vt == NULL) return 0;
    if (g_devvt != vt) {                    /* patch each distinct vtable */
        DWORD old = 0;
        if (s_orig_present == NULL) {
            DWORD slot = (DWORD)(DWORD_PTR)&vt[17];
            VirtualProtect((LPVOID)slot, sizeof(void *), PAGE_READWRITE, &old);
            s_orig_present = (PRESENT_FN)vt[17];
            vt[17] = (void *)present_hook;
            VirtualProtect((LPVOID)slot, sizeof(void *), old, &old);
        }
        if (s_orig_reset == NULL) {
            DWORD slot = (DWORD)(DWORD_PTR)&vt[16];
            VirtualProtect((LPVOID)slot, sizeof(void *), PAGE_READWRITE, &old);
            s_orig_reset = (DEV_RESET)vt[16];
            vt[16] = (void *)reset_hook;
            VirtualProtect((LPVOID)slot, sizeof(void *), old, &old);
        }
        g_devvt = vt;
    }
    return (g_devvt == vt && s_orig_present != NULL);   /* patched now */
}

/* ------------------------ IDirect3D9 COM wrapper --------------------------- */
/* All 18 vtable slots are forwarded properly (ABI-safe: enum-typed params
 * pass as 4-byte ints, D3DADAPTER_IDENTIFIER9/DISPLAYMODE/CAPS pass by ptr).
 * CreateDevice (16) / CreateDeviceEx (17) install the Present hook.        */
struct d3d9w {
    void **vt;           /* our wrapper vtable  */
    void  *real;         /* real IDirect3D9/Ex  */
};
typedef struct d3d9w D3D9W;
static D3D9W g_wrapper_obj;
static D3D9W *g_wrapper = NULL;

static int  STDMETHODCALLTYPE w_query_interface(void *self, const void *iid, void **p);
static unsigned long STDMETHODCALLTYPE w_add_ref(void *self);
static unsigned long STDMETHODCALLTYPE w_release(void *self);

static int  STDMETHODCALLTYPE w_reg_swdev(void *self, void *fn) {
    return ((int (STDMETHODCALLTYPE *)(void *, void *))
        ((void ***)((D3D9W *)self)->real)[0][3])(((D3D9W *)self)->real, fn);
}
static UINT STDMETHODCALLTYPE w_get_adapter_count(void *self) {
    return ((UINT (STDMETHODCALLTYPE *)(void *))
        ((void ***)((D3D9W *)self)->real)[0][4])(((D3D9W *)self)->real);
}
static int STDMETHODCALLTYPE w_get_adapter_id(void *self, UINT a, void *id) {
    return ((int (STDMETHODCALLTYPE *)(void *, UINT, void *))
        ((void ***)((D3D9W *)self)->real)[0][5])(((D3D9W *)self)->real, a, id);
}
static UINT STDMETHODCALLTYPE w_get_adapter_mode_count(void *self, UINT a, UINT fmt) {
    return ((UINT (STDMETHODCALLTYPE *)(void *, UINT, UINT))
        ((void ***)((D3D9W *)self)->real)[0][6])(((D3D9W *)self)->real, a, fmt);
}
static int STDMETHODCALLTYPE w_enum_adapter_modes(void *self, UINT a, UINT fmt, UINT m, void *mode) {
    return ((int (STDMETHODCALLTYPE *)(void *, UINT, UINT, UINT, void *))
        ((void ***)((D3D9W *)self)->real)[0][7])(((D3D9W *)self)->real, a, fmt, m, mode);
}
static int STDMETHODCALLTYPE w_get_adapter_display_mode(void *self, UINT a, void *mode) {
    return ((int (STDMETHODCALLTYPE *)(void *, UINT, void *))
        ((void ***)((D3D9W *)self)->real)[0][8])(((D3D9W *)self)->real, a, mode);
}
static int STDMETHODCALLTYPE w_check_device_type(void *self, UINT a, UINT t, UINT f1, UINT f2, BOOL win) {
    return ((int (STDMETHODCALLTYPE *)(void *, UINT, UINT, UINT, UINT, BOOL))
        ((void ***)((D3D9W *)self)->real)[0][9])(((D3D9W *)self)->real, a, t, f1, f2, win);
}
static int STDMETHODCALLTYPE w_check_device_format(void *self, UINT a, UINT t, UINT adapt_disp, DWORD usage, UINT rtype, UINT fmt) {
    return ((int (STDMETHODCALLTYPE *)(void *, UINT, UINT, UINT, DWORD, UINT, UINT))
        ((void ***)((D3D9W *)self)->real)[0][10])(((D3D9W *)self)->real, a, t, adapt_disp, usage, rtype, fmt);
}
static int STDMETHODCALLTYPE w_check_multi_sample(void *self, UINT a, UINT t, UINT f, BOOL win, UINT ms, DWORD *n) {
    return ((int (STDMETHODCALLTYPE *)(void *, UINT, UINT, UINT, BOOL, UINT, DWORD *))
        ((void ***)((D3D9W *)self)->real)[0][11])(((D3D9W *)self)->real, a, t, f, win, ms, n);
}
static int STDMETHODCALLTYPE w_check_depth(void *self, UINT a, UINT t, UINT f1, UINT f2, UINT f3) {
    return ((int (STDMETHODCALLTYPE *)(void *, UINT, UINT, UINT, UINT, UINT))
        ((void ***)((D3D9W *)self)->real)[0][12])(((D3D9W *)self)->real, a, t, f1, f2, f3);
}
static int STDMETHODCALLTYPE w_check_format_conv(void *self, UINT a, UINT t, UINT f1, UINT f2) {
    return ((int (STDMETHODCALLTYPE *)(void *, UINT, UINT, UINT, UINT))
        ((void ***)((D3D9W *)self)->real)[0][13])(((D3D9W *)self)->real, a, t, f1, f2);
}
static int STDMETHODCALLTYPE w_get_device_caps(void *self, UINT a, UINT t, void *caps) {
    return ((int (STDMETHODCALLTYPE *)(void *, UINT, UINT, void *))
        ((void ***)((D3D9W *)self)->real)[0][14])(((D3D9W *)self)->real, a, t, caps);
}
static void *STDMETHODCALLTYPE w_get_adapter_monitor(void *self, UINT a) {
    return ((void * (STDMETHODCALLTYPE *)(void *, UINT))
        ((void ***)((D3D9W *)self)->real)[0][15])(((D3D9W *)self)->real, a);
}
static int STDMETHODCALLTYPE w_create_device(void *self, UINT adapter, UINT type,
        HWND focus, DWORD flags, void *pparams, void **ppdev) {
    D3D9W *w = (D3D9W *)self;
    int hr = ((int (STDMETHODCALLTYPE *)(void *, UINT, UINT, HWND, DWORD, void *, void **))
        ((void ***)w->real)[0][16])(w->real, adapter, type, focus, flags, pparams, ppdev);
    if (hr >= 0 && ppdev != NULL && *ppdev != NULL) {
        int patched = patch_device_present(*ppdev);
        dlog("CreateDevice hr=%#010x dev=%p patched=%s",
             (unsigned)hr, *ppdev, patched ? "yes" : "no");
    } else if (hr >= 0) {
        dlog("CreateDevice hr=%#010x dev=<NULL>", (unsigned)hr);
    }
    return hr;
}
static int STDMETHODCALLTYPE w_create_device_ex(void *self, UINT adapter, UINT type,
        HWND focus, DWORD flags, void *pparams, void *pfs, void **ppdev) {
    D3D9W *w = (D3D9W *)self;
    int hr = ((int (STDMETHODCALLTYPE *)(void *, UINT, UINT, HWND, DWORD, void *, void *, void **))
        ((void ***)w->real)[0][17])    (w->real, adapter, type, focus, flags, pparams, pfs, ppdev);
    if (hr >= 0 && ppdev != NULL && *ppdev != NULL) {
        int patched = patch_device_present(*ppdev);
        dlog("CreateDeviceEx hr=%#010x dev=%p patched=%s",
             (unsigned)hr, *ppdev, patched ? "yes" : "no");
    }
    return hr;
}

static void *g_d3d9w_vtable[18] = {
    (void *)w_query_interface, (void *)w_add_ref,     (void *)w_release,
    (void *)w_reg_swdev,       (void *)w_get_adapter_count,
    (void *)w_get_adapter_id,  (void *)w_get_adapter_mode_count,
    (void *)w_enum_adapter_modes, (void *)w_get_adapter_display_mode,
    (void *)w_check_device_type,(void *)w_check_device_format,
    (void *)w_check_multi_sample,(void *)w_check_depth,
    (void *)w_check_format_conv,(void *)w_get_device_caps,
    (void *)w_get_adapter_monitor, (void *)w_create_device,
    (void *)w_create_device_ex,
};

static int STDMETHODCALLTYPE w_query_interface(void *self, const void *iid, void **p) {
    D3D9W *w = (D3D9W *)self;
    return ((int (STDMETHODCALLTYPE *)(void *, const void *, void **))
        ((void ***)w->real)[0][0])(w->real, iid, p);
}
static unsigned long STDMETHODCALLTYPE w_add_ref(void *self) {
    return ((unsigned long (STDMETHODCALLTYPE *)(void *))
        ((void ***)((D3D9W *)self)->real)[0][1])(((D3D9W *)self)->real);
}
static unsigned long STDMETHODCALLTYPE w_release(void *self) {
    return ((unsigned long (STDMETHODCALLTYPE *)(void *))
        ((void ***)((D3D9W *)self)->real)[0][2])(((D3D9W *)self)->real);
}

static D3D9W *wrap_d3d9(void *real) {
    if (real == NULL) return NULL;
    if (g_wrapper != NULL) { g_wrapper->real = real; return g_wrapper; }
    g_wrapper_obj.vt   = g_d3d9w_vtable;
    g_wrapper_obj.real = real;
    g_wrapper          = &g_wrapper_obj;
    return g_wrapper;
}

/* ------------------------ Direct3DCreate9 exports -------------------------- */
void *WINAPI Direct3DCreate9(UINT SDKVersion) {
    if (g_real == NULL) return NULL;
    typedef void * (WINAPI *FN)(UINT);
    FN f = (FN)(DWORD_PTR)GetProcAddress(g_real, "Direct3DCreate9");
    if (f == NULL) return NULL;
    void *wrapped = (void *)wrap_d3d9(f(SDKVersion));
    dlog("Create9 v=%u -> wrapped=%p", (unsigned)SDKVersion, wrapped);
    return wrapped;
}

int WINAPI Direct3DCreate9Ex(UINT SDKVersion, void **ppOut) {
    if (g_real == NULL) { if (ppOut) *ppOut = NULL; return 0x8876086c; }
    typedef int (WINAPI *FN)(UINT, void **);
    FN f = (FN)(DWORD_PTR)GetProcAddress(g_real, "Direct3DCreate9Ex");
    if (f == NULL) { if (ppOut) *ppOut = NULL; return 0x8876086c; }
    void *out = NULL;
    int hr = f(SDKVersion, &out);
    if (ppOut) *ppOut = (hr >= 0) ? (void *)wrap_d3d9(out) : NULL;
    return hr;
}

/* ----------------------- thin-wrapper passthroughs ------------------------- */
int  WINAPI D3DPERF_BeginEvent(D3DCOLOR color, LPCWSTR name);
int  WINAPI D3DPERF_EndEvent(void);
void WINAPI D3DPERF_SetMarker(D3DCOLOR color, LPCWSTR name);
void WINAPI D3DPERF_SetRegion(D3DCOLOR color, LPCWSTR name);
BOOL WINAPI D3DPERF_QueryRepeatFrame(void);
void WINAPI D3DPERF_SetOptions(DWORD flags);
DWORD WINAPI D3DPERF_GetStatus(void);
int  WINAPI DebugSetLevel(DWORD flags);
void WINAPI DebugSetMute(void);

int  WINAPI D3DPERF_BeginEvent(D3DCOLOR color, LPCWSTR name) {
    if (g_real == NULL) return 0;
    typedef int (WINAPI *FN)(D3DCOLOR, LPCWSTR);
    FN f = (FN)(DWORD_PTR)GetProcAddress(g_real, "D3DPERF_BeginEvent");
    return f ? f(color, name) : 0;
}
int WINAPI D3DPERF_EndEvent(void) {
    if (g_real == NULL) return 0;
    typedef int (WINAPI *FN)(void);
    FN f = (FN)(DWORD_PTR)GetProcAddress(g_real, "D3DPERF_EndEvent");
    return f ? f() : 0;
}
void WINAPI D3DPERF_SetMarker(D3DCOLOR color, LPCWSTR name) {
    if (g_real == NULL) return;
    typedef void (WINAPI *FN)(D3DCOLOR, LPCWSTR);
    FN f = (FN)(DWORD_PTR)GetProcAddress(g_real, "D3DPERF_SetMarker");
    if (f) f(color, name);
}
void WINAPI D3DPERF_SetRegion(D3DCOLOR color, LPCWSTR name) {
    if (g_real == NULL) return;
    typedef void (WINAPI *FN)(D3DCOLOR, LPCWSTR);
    FN f = (FN)(DWORD_PTR)GetProcAddress(g_real, "D3DPERF_SetRegion");
    if (f) f(color, name);
}
BOOL WINAPI D3DPERF_QueryRepeatFrame(void) {
    if (g_real == NULL) return FALSE;
    typedef BOOL (WINAPI *FN)(void);
    FN f = (FN)(DWORD_PTR)GetProcAddress(g_real, "D3DPERF_QueryRepeatFrame");
    return f ? f() : FALSE;
}
void WINAPI D3DPERF_SetOptions(DWORD flags) {
    if (g_real == NULL) return;
    typedef void (WINAPI *FN)(DWORD);
    FN f = (FN)(DWORD_PTR)GetProcAddress(g_real, "D3DPERF_SetOptions");
    if (f) f(flags);
}
DWORD WINAPI D3DPERF_GetStatus(void) {
    if (g_real == NULL) return 0;
    typedef DWORD (WINAPI *FN)(void);
    FN f = (FN)(DWORD_PTR)GetProcAddress(g_real, "D3DPERF_GetStatus");
    return f ? f() : 0;
}
int WINAPI DebugSetLevel(DWORD flags) {
    if (g_real == NULL) return 0;
    typedef int (WINAPI *FN)(DWORD);
    FN f = (FN)(DWORD_PTR)GetProcAddress(g_real, "DebugSetLevel");
    return f ? f(flags) : 0;
}
void WINAPI DebugSetMute(void) {
    if (g_real == NULL) return;
    typedef void (WINAPI *FN)(void);
    FN f = (FN)(DWORD_PTR)GetProcAddress(g_real, "DebugSetMute");
    if (f) f();
}

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        char sys[MAX_PATH];
        g_step[0] = '\0';
        if (s_prev_filter == NULL)          /* crash catch FIRST, before all else */
            s_prev_filter = SetUnhandledExceptionFilter(fault_filter);
        set_step("dllmain");
        GetSystemDirectoryA(sys, MAX_PATH);
        lstrcatA(sys, "\\d3d9.dll");
        g_real = LoadLibraryA(sys);
        DisableThreadLibraryCalls(hInst);
        s_orig_present = NULL;
        s_orig_reset = NULL;
        g_devvt = NULL;
        g_base = (void *)GetModuleHandleA("age3y.exe");
        dlog("DLL loaded, base=%p real_d3d9=%p", g_base, (void *)g_real);
    }
    return TRUE;
}