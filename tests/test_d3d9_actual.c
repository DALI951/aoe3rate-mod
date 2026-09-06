/*
 * test_d3d9_actual.c — independent SWARM test that compiles the REAL d3d9.c
 * into the test binary (via SWARM_TEST include) and drives its actual
 * production functions: locate_resources_at(), decrypt_slot(), rate_for().
 *
 * This is stronger than the build worker's decrypt_unit.c copy because it
 * exercises the exact production code, not a re-typed version.
 *
 * ROUND 5 (authoritative active-player rework): the harness now lays the game
 * object so the active-player index lives at game+0x14c (OFF_GAME_ACTIVE_PLAYER)
 * and PROVES the old cell *(base+0x866664) is dead & ignored (it holds -1 or a
 * contradicting value while game+0x14c decides the player). It also exercises
 * the probe-lite fallback for BOTH hostile cases — active idx == -1/0xFFFFFFFF
 * (the exact live menu-like value) and idx == 99 (out of range) — asserts the
 * fallback picks the FIRST sane player (income+0x80, container+0x230, decrypted
 * slot-0 food>0) and that the quiet chain log really contains the
 * [src-fallback] [fallback: player#N] tag. A dedicated case reproduces the
 * round-4 LIVE chain log addresses (game=0x05BF2000, ctx=0x14500000, n=3,
 * idx=0/1) and asserts non-NULL resolution when those addresses are mappable,
 * degrading to the identical relative layout if the address space is busy.
 *
 * Build (from tests dir):
 *   i686-w64-mingw32-gcc.exe test_d3d9_actual.c -o test_d3d9_actual.exe
 *
 * ROUND 8 (pure observer): all draw code was deleted from d3d9.c; the harness
 * draw-path cases now target observer_sample() (read+log only). test_observer_log
 * boots a real world, forces the first snapshot, asserts the exact
 * `res P0 f=.. .. incA=.. incB=.. incN=..` line, then proves unchanged frames
 * log NOTHING and a >= 0.5 dellected-slot change re-logs exactly once.
 */
#define SWARM_TEST
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "..\d3d9.c"

/* real key array bytes from the Ghidra memory dump (0xC6DF14) */
static const unsigned char KEY_BYTES[32] = {
    0x28, 0x48, 0xAC, 0x4F, 0x94, 0xF8, 0x3A, 0x35,
    0x8B, 0xD8, 0x4C, 0x3F, 0xAB, 0x12, 0xFB, 0xAF,
    0x20, 0xB3, 0x5B, 0xCA, 0xF9, 0xAB, 0xC4, 0x2A,
    0xB1, 0xA1, 0xCF, 0xDA, 0xF2, 0xE4, 0x82, 0x10,
};
#define MAX_RVA 0xA00000u   /* fake age3y image covers the game-ptr cell RVA 0x866234.. */

/* round-4 live chain log addresses (seen in a real skirmish) */
#define LIVE_GAME 0x05BF2000u
#define LIVE_CTX  0x14500000u
/* the DEAD cell from round 4: *(base+0x866664), a UI selection marker, was -1
 * live. No production code may read it any more.                            */
#define OLD_DEAD_CELL_RVA 0x866664u

static int failures = 0;

static void putu(DWORD a, DWORD v) { *(volatile DWORD *)a = v; }
static void putf(DWORD a, float f) { DWORD b; memcpy(&b, &f, 4); putu(a, b); }

/* Encrypt: replicate game FUN_0049a859 (add/encrypt): new = (key^old)^valbits */
static void set_encrypted_slot(DWORD res_base, int slot, float value,
                               const unsigned char *keybytes) {
    DWORD key = keybytes[slot*4+0] | (keybytes[slot*4+1]<<8)
              | (keybytes[slot*4+2]<<16) | (keybytes[slot*4+3]<<24);
    DWORD bits; memcpy(&bits, &value, 4);
    *(volatile DWORD *)(res_base + slot*4) = key ^ bits;
}

/* allocate a zeroed fake age3y image: key table + slot count + eval constants */
static LPVOID make_fake_image(void) {
    SYSTEM_INFO si; GetSystemInfo(&si);
    DWORD page = si.dwAllocationGranularity;
    SIZE_T size = ((MAX_RVA + page - 1) / page) * page;
    LPVOID p = VirtualAlloc(NULL, size, MEM_COMMIT, PAGE_READWRITE);
    if (!p) return NULL;
    memset(p, 0, (size_t)size);
    DWORD b = (DWORD)(DWORD_PTR)p;
    memcpy((BYTE*)p + (0xC6DF14 - 0x400000), KEY_BYTES, 32);
    putu(b + (0xC6DF38 - 0x400000), 8);
    putf(b + 0x79A5C8, 0.001f);
    putf(b + 0x7A12E8, 1e-6f);
    putf(b + 0x7A12FC, 4294967296.0f);
    putf(b + 0x7856BC, 1.0f);
    return p;
}

/* one-player world + income history (for the decrypt/rate/menu tests) */
static LPVOID make_fake_age3y(DWORD *out_container) {
    LPVOID p = make_fake_image();
    if (!p) return NULL;
    DWORD b = (DWORD)(DWORD_PTR)p;

    DWORD game    = b + 0x1000;
    DWORD ctx     = b + 0x2000;
    DWORD players = b + 0x3000;
    DWORD player  = b + 0x4000;
    DWORD inc     = b + 0x5000;
    DWORD hist    = b + 0x6000;
    DWORD container = b + 0x7000;      /* = out_container */

    putu(b + RVA_GAME_PTR, game);
    putu(game + OFF_GAME_CTX, ctx);
    putu(ctx + OFF_CTX_SNAP, 3000);
    putu(ctx + OFF_CTX_PLAYERCNT, 1);
    putu(ctx + OFF_CTX_PLAYERS, players);
    putu(players + 0, player);
    putu(game + OFF_GAME_ACTIVE_PLAYER, 0);
    set_encrypted_slot(container, 0, 200.0f, KEY_BYTES);  /* food>0: probe sanity */
    putu(player + OFF_PLAYER_RES, container);
    putu(player + OFF_PLAYER_INCOME, inc);
    putu(inc + OFF_INC_CUR, 2000);
    putu(inc + OFF_INC_PREV, 1000);
    putu(inc + OFF_INC_COUNT, 3);
    putu(inc + OFF_INC_HIST, hist);
    for (int i = 0; i < 3; i++) {      /* sample containers */
        DWORD c = b + 0x8000 + i * 0x80;
        putu(hist + i * 4, c);
        set_encrypted_slot(c, 0, (float)(1000 + i * 500), KEY_BYTES); /* 1000/1500/2000 */
    }

    if (out_container) *out_container = container;
    return p;
}

/* round-5 authoritative happy path: n>=2, old dead cell set to a CONTRADICTING
 * value, and game+0x14c must decide the player. Runs first with old cell=-1
 * and idx=1 (regression would resolve player 0 via fallback -> FAIL), then
 * with old cell=1 and idx=0 (regression would resolve player 1 -> FAIL).    */
static void test_authoritative_happy_path(void) {
    LPVOID img = make_fake_image();
    if (!img) { printf("FAIL: alloc (happy)\n"); failures++; return; }
    DWORD b = (DWORD)(DWORD_PTR)img;

    DWORD game = b + 0x1000, ctx = b + 0x2000, players = b + 0x3000;
    putu(b + RVA_GAME_PTR, game);
    putu(game + OFF_GAME_CTX, ctx);
    putu(ctx + OFF_CTX_SNAP, 3000);
    putu(ctx + OFF_CTX_PLAYERCNT, 3);
    putu(ctx + OFF_CTX_PLAYERS, players);
    /* three SANE players, distinct containers/income */
    for (int i = 0; i < 3; i++) {
        DWORD p = b + 0x4000 + (DWORD)i * 0x1000;
        putu(players + (DWORD)i * 4, p);
        DWORD cont = p + 0x300, inc = p + 0x600;
        set_encrypted_slot(cont, 0, (float)(150 + i * 100), KEY_BYTES);
        putu(p + OFF_PLAYER_RES, cont);
        putu(p + OFF_PLAYER_INCOME, inc);
    }
    DWORD p0 = b + 0x4000, p1 = b + 0x5000;
    DWORD c0 = p0 + 0x300,  c1 = p1 + 0x300;
    DWORD i0 = p0 + 0x600,  i1 = p1 + 0x600;

    /* part A: dead cell = -1 (the LIVE value from round 4), game+0x14c = 1.
     * A regression to the old source would yield idx=-1 -> fallback -> p0.   */
    putu(b + OLD_DEAD_CELL_RVA, 0xFFFFFFFF);
    putu(game + OFF_GAME_ACTIVE_PLAYER, 1);
    g_res = g_inc = NULL;
    locate_resources_at(img);
    if (g_res != (void *)c1 || g_inc != (void *)i1) {
        printf("FAIL: idx=1 must resolve player#1 (old cell -1 ignored), res=%p\n", g_res);
        failures++;
    } else {
        printf("PASS: game+0x14c==1 resolves player#1 while dead cell 0x866664=-1 (ignored)\n");
    }

    /* part B: dead cell = 1 (contradicts), game+0x14c = 0 -> player 0 wins.  */
    putu(b + OLD_DEAD_CELL_RVA, 1);
    putu(game + OFF_GAME_ACTIVE_PLAYER, 0);
    g_res = g_inc = NULL;
    locate_resources_at(img);
    if (g_res != (void *)c0 || g_inc != (void *)i0) {
        printf("FAIL: idx=0 must resolve player#0 (dead cell 1 ignored), res=%p\n", g_res);
        failures++;
    } else {
        printf("PASS: game+0x14c==0 resolves player#0 while dead cell says 1 (ignored)\n");
    }
    VirtualFree(img, 0, MEM_RELEASE);
}

/* round-4 LIVE addresses: game=0x05BF2000 ctx=0x14500000 n=3 idx=0/1 must
 * resolve a player and NOT NULL. If those addresses are already mapped by the
 * OS the identical chain shape runs in the relative layout (documented).     */
static void test_live_values(void) {
    LPVOID img = make_fake_image();
    if (!img) { printf("FAIL: alloc (live)\n"); failures++; return; }
    DWORD b = (DWORD)(DWORD_PTR)img;

    /* try to map the exact live-log addresses. 0x05BF2000 is not 64KB-aligned,
     * so allocate the aligned superpage (0x05BF0000, 64KB) and place the game
     * object at LIVE_GAME inside it; 0x14500000 is 64KB-aligned already.     */
    LPVOID g = NULL, c = NULL;
#define GAME_MAP_BASE (LIVE_GAME & ~0xFFFFu)
    if (VirtualAlloc((LPVOID)GAME_MAP_BASE, 0x10000, MEM_RESERVE | MEM_COMMIT,
                     PAGE_READWRITE) == (LPVOID)GAME_MAP_BASE) {
        g = (LPVOID)GAME_MAP_BASE;
        if (VirtualAlloc((LPVOID)LIVE_CTX, 0x20000, MEM_RESERVE | MEM_COMMIT,
                         PAGE_READWRITE) != (LPVOID)LIVE_CTX) {
            VirtualFree(g, 0, MEM_RELEASE); g = NULL;
        } else c = (LPVOID)LIVE_CTX;
    }
    int literal = (g != NULL && c != NULL);

    DWORD game, ctx, players, pbase;
    if (literal) {
        game = LIVE_GAME; ctx = LIVE_CTX; players = ctx + 0x1000; pbase = ctx + 0x2000;
    } else {
        game = b + 0x1000; ctx = b + 0x2000; players = b + 0x3000; pbase = b + 0x4000;
    }
    putu(b + RVA_GAME_PTR, game);
    putu(game + OFF_GAME_CTX, ctx);
    putu(ctx + OFF_CTX_SNAP, 3000);
    putu(ctx + OFF_CTX_PLAYERCNT, 3);
    putu(ctx + OFF_CTX_PLAYERS, players);
    /* p0/p1 sane, p2 insane (no income, no container) */
    for (int i = 0; i < 3; i++) {
        DWORD p = pbase + (DWORD)i * 0x2000;
        putu(players + (DWORD)i * 4, p);
        if (i < 2) {
            DWORD cont = p + 0x300, inc = p + 0x600;
            set_encrypted_slot(cont, 0, (float)(150 + i * 100), KEY_BYTES);
            putu(p + OFF_PLAYER_RES, cont);
            putu(p + OFF_PLAYER_INCOME, inc);
        }
    }
    putu(b + OLD_DEAD_CELL_RVA, 0xFFFFFFFF);   /* the live -1 of the dead cell */

    for (int idx = 0; idx <= 1; idx++) {
        DWORD p  = pbase + (DWORD)idx * 0x2000;
        DWORD ec = p + 0x300, ei = p + 0x600;
        putu(game + OFF_GAME_ACTIVE_PLAYER, (DWORD)idx);
        g_res = g_inc = NULL;
        locate_resources_at(img);
        if (g_res != (void *)ec || g_inc != (void *)ei || g_res == NULL || g_inc == NULL) {
            printf("FAIL: live chain idx=%d res=%p inc=%p want res=%08X\n",
                   idx, g_res, g_inc, ec);
            failures++;
        }
    }
    if (literal)
        printf("PASS: live game=%08X ctx=%08X n=3 idx=0..1 -> player resolved, NOT NULL\n",
               LIVE_GAME, LIVE_CTX);
    else
        printf("PASS: live-value shape (literal %08X/%08X busy -> relative): idx 0..1 resolve, NOT NULL\n",
               LIVE_GAME, LIVE_CTX);
    if (c) VirtualFree(c, 0, MEM_RELEASE);
    if (g) VirtualFree(g, 0, MEM_RELEASE);
    VirtualFree(img, 0, MEM_RELEASE);
}

/* 3-player world for the fallback tests:
 *   p0 SANE   income+container+food>0
 *   p1 has income+container but food==0   -> probe FAILS (food rule)
 *   p2 has income but NO container        -> probe FAILS (container rule)
 */
static DWORD build_fallback_world(DWORD b) {
    DWORD game = b + 0x1000, ctx = b + 0x2000, players = b + 0x3000;
    putu(b + RVA_GAME_PTR, game);
    putu(game + OFF_GAME_CTX, ctx);
    putu(ctx + OFF_CTX_SNAP, 3000);
    putu(ctx + OFF_CTX_PLAYERCNT, 3);
    putu(ctx + OFF_CTX_PLAYERS, players);
    for (int i = 0; i < 3; i++)
        putu(players + (DWORD)i * 4, b + 0x4000 + (DWORD)i * 0x1000);
    {   /* p0 */
        DWORD p = b + 0x4000, cont = p + 0x300, inc = p + 0x600;
        set_encrypted_slot(cont, 0, 150.0f, KEY_BYTES);
        putu(p + OFF_PLAYER_RES, cont);
        putu(p + OFF_PLAYER_INCOME, inc);
    }
    {   /* p1: food == 0 -> must NOT win */
        DWORD p = b + 0x5000, cont = p + 0x300, inc = p + 0x600;
        set_encrypted_slot(cont, 0, 0.0f, KEY_BYTES);
        putu(p + OFF_PLAYER_RES, cont);
        putu(p + OFF_PLAYER_INCOME, inc);
    }
    {   /* p2: no container -> must NOT win */
        DWORD p = b + 0x6000;
        putu(p + OFF_PLAYER_INCOME, p + 0x600);
    }
    return game;
}

static void delete_chain_log(void) {
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    if (!n) return;
    char *sl = strrchr(path, '\\');
    if (!sl) return;
    *sl = '\0';
    _snprintf(path + strlen(path), MAX_PATH - strlen(path), "\\d3d9mod.log");
    DeleteFileA(path);
}
static int log_contains(const char *needle) {
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    if (!n) return 0;
    char *sl = strrchr(path, '\\');
    if (!sl) return 0;
    *sl = '\0';
    _snprintf(path + strlen(path), MAX_PATH - strlen(path), "\\d3d9mod.log");
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char buf[16384];
    size_t got = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[got] = '\0';
    return strstr(buf, needle) != NULL;
}
static int log_count(void) {
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    if (!n) return 0;
    char *sl = strrchr(path, '\\');
    if (!sl) return 0;
    *sl = '\0';
    _snprintf(path + strlen(path), MAX_PATH - strlen(path), "\\d3d9mod.log");
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char buf[16384];
    size_t got = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[got] = '\0';
    int cnt = 0;
    for (size_t i = 0; i < got; i++)
        if (buf[i] == '\n') cnt++;
    return cnt;
}

/* menu-like: game+0x14c == -1 (0xFFFFFFFF, the exact LIVE value), 3 players,
 * first sane = p0 -> fallback must pick p0 AND log the [src-fallback] tag.  */
static void test_fallback_menu_like(void) {
    LPVOID img = make_fake_image();
    if (!img) { printf("FAIL: alloc (fb -1)\n"); failures++; return; }
    DWORD b = (DWORD)(DWORD_PTR)img;
    DWORD game = build_fallback_world(b);
    putu(b + OLD_DEAD_CELL_RVA, 0xFFFFFFFF);

    putu(game + OFF_GAME_ACTIVE_PLAYER, 0xFFFFFFFF);   /* idx = -1 */
    delete_chain_log();
    g_res = g_inc = NULL;
    locate_resources_at(img);
    if (g_res != (void *)(b + 0x4300) || g_inc != (void *)(b + 0x4600)) {
        printf("FAIL: idx=-1 fallback must pick player#0, got res=%p inc=%p\n", g_res, g_inc);
        failures++;
    } else {
        printf("PASS: idx=-1 (0xFFFFFFFF, menu-like) -> fallback picks player#0\n");
    }
    if (!log_contains("[src-fallback] [fallback: player#0]")) {
        printf("FAIL: d3d9mod.log missing [src-fallback] [fallback: player#0]\n");
        failures++;
    } else {
        printf("PASS: chain log carries [src-fallback] [fallback: player#0]\n");
    }
    VirtualFree(img, 0, MEM_RELEASE);
}

/* out-of-range: game+0x14c == 99 with n == 3 -> fallback scan -> p0.         */
static void test_fallback_oob(void) {
    LPVOID img = make_fake_image();
    if (!img) { printf("FAIL: alloc (fb 99)\n"); failures++; return; }
    DWORD b = (DWORD)(DWORD_PTR)img;
    DWORD game = build_fallback_world(b);

    putu(game + OFF_GAME_ACTIVE_PLAYER, 99);           /* idx 99 >= n(3)    */
    g_res = g_inc = NULL;
    locate_resources_at(img);
    if (g_res != (void *)(b + 0x4300) || g_inc != (void *)(b + 0x4600)) {
        printf("FAIL: idx=99 OOB must fall back to player#0, got res=%p\n", g_res);
        failures++;
    } else {
        printf("PASS: idx=99 out of range (n=3) -> fallback picks player#0\n");
    }
    VirtualFree(img, 0, MEM_RELEASE);
}

/* ROUND-7 (unconditional authoritative): an in-range idx is trusted with NO
 * sanity probe — the probe only gates the FALLBACK, never the authoritative
 * pick (round-4's live flip bug: a coin-slot-0 read made the human "insane"
 * and the fallback re-aimed at an AI player every frame). Part A regression =
 * any probe vetoing an in-range idx. Part B proves [chain-break] still fires
 * only for OOR idx with no sane player -> NULLs, no crash.                */
static void test_fallback_none(void) {
    LPVOID img = make_fake_image();
    if (!img) { printf("FAIL: alloc (fb none)\n"); failures++; return; }
    DWORD b = (DWORD)(DWORD_PTR)img;
    DWORD game = build_fallback_world(b);
    /* p0 loses its container -> p0 would FAIL the probe (res reads 0) */
    putu(b + 0x4000 + OFF_PLAYER_RES, 0);

    /* part A: idx==0 IN RANGE + probe-insane p0 -> still taken authoritatively.
     * res=0 -> g_res NULL (inc survives); NEVER handed to the fallback.     */
    putu(game + OFF_GAME_ACTIVE_PLAYER, 0);
    delete_chain_log();
    g_res = g_inc = (void *)0xDEADBEEF;
    locate_resources_at(img);
    if (g_res != NULL) {
        printf("FAIL: in-range idx must be trusted (res=0 -> NULL), got %p\n", g_res);
        failures++;
    } else {
        printf("PASS: in-range idx==0 wins unconditionally (no probe veto)\n");
    }
    if (!log_contains("[src-authoritative]")) {
        printf("FAIL: authoritative tag missing for in-range idx\n");
        failures++;
    } else {
        printf("PASS: authoritative tag logged for in-range idx\n");
    }

    /* part B: idx OOR (-1) AND nothing sane -> NULLs + chain-break, no crash */
    putu(game + OFF_GAME_ACTIVE_PLAYER, 0xFFFFFFFF);
    g_res = g_inc = (void *)0xDEADBEEF;
    locate_resources_at(img);
    if (g_res != NULL || g_inc != NULL) {
        printf("FAIL: no sane player -> expected NULLs, got res=%p\n", g_res);
        failures++;
    } else {
        printf("PASS: OOR idx + no sane player -> NULLs (chain-break), no crash\n");
    }
    if (!log_contains("[chain-break: no sane player]")) {
        printf("FAIL: chain-break tag missing for OOR + no sane\n");
        failures++;
    } else {
        printf("PASS: chain-break tag logged for OOR + no sane player\n");
    }
    VirtualFree(img, 0, MEM_RELEASE);
}

static void test_roundtrip_actual(LPVOID base, DWORD container) {
    locate_resources_at(base);
    if (g_res == NULL || g_res != (void *)container) {
        printf("FAIL: locate valid chain\n");
        failures++;
        return;
    }

    double vals[] = { 0.0, 1.0, 0.5, 123.456, 3.14159,
                      1000.0, 98765.4321, 500000.0, 999999.0, 1e6 };
    for (int s = 0; s < 8; s++) {
        for (int i = 0; i < (int)(sizeof(vals)/sizeof(vals[0])); i++) {
            set_encrypted_slot((DWORD)(DWORD_PTR)g_res, s, (float)vals[i], KEY_BYTES);
            float dec = decrypt_slot(s);
            if (!(dec == (float)vals[i]) || isnan(dec) || isinf(dec)) {
                printf("FAIL: actual decrypt slot %d val %g -> %g\n",
                       s, vals[i], (double)dec);
                failures++;
            }
        }
    }
    printf("PASS: actual d3d9.c decrypt_slot round-trip 0..1e6, no NaN/INF\n");

    /* rate via the income object: (cont2=2000)+(cont1=1500)+(cont0=1000) =
     * 4500; snapshot3000-prev1000+cur2000=4000; others cur=2000 -> acc =
     * (4+2+2)=8.0 -> rate = 4500/8 = 562.5  */
    float r0 = rate_for(0);
    if (r0 != 562.5f) {
        printf("FAIL: rate_for(0) expected 562.5, got %g\n", (double)r0);
        failures++;
    } else printf("PASS: rate_for(0) == 562.5 (FUN_0086da39 math)\n");

    /* main-menu: rate_for returns 0.0f when the ctx has no players (n==0),
     * exactly the case FUN_00405574 routes to -1. */
    {
        DWORD game_ = *(volatile DWORD *)((DWORD)(DWORD_PTR)base + RVA_GAME_PTR);
        DWORD ctx_  = *(volatile DWORD *)(game_ + OFF_GAME_CTX);
        putu(ctx_ + OFF_CTX_PLAYERCNT, 0);            /* menu: no players  */
        g_res = g_inc = NULL;
        locate_resources_at(base);
        if (g_res != NULL || g_inc != NULL) {
            printf("FAIL: menu n==0 must give NULLs, got res=%p\n", g_res);
            failures++;
        }
        for (int s = 0; s < 8; s++) {
            float r = rate_for(s);
            if (r != 0.0f || isnan(r) || isinf(r)) {
                printf("FAIL: rate_for(%d) expected 0.0f, got %g\n", s, (double)r);
                failures++;
            }
        }
        putu(ctx_ + OFF_CTX_PLAYERCNT, 1);            /* restore           */
        printf("PASS: menu (n==0) -> NULLs and rate_for 0.0f\n");
    }

    /* probe-lite fallback with the one-player world: idx out of range -> the
     * scan picks the sane player. */
    {
        DWORD game_ = *(volatile DWORD *)((DWORD)(DWORD_PTR)base + RVA_GAME_PTR);
        putu(game_ + OFF_GAME_ACTIVE_PLAYER, 7);      /* idx 7 >= n(1)     */
        g_res = g_inc = NULL;
        locate_resources_at(base);
        if (g_res != (void *)container) {
            printf("FAIL: fallback did not resolve the sane player\n");
            failures++;
        } else printf("PASS: probe-lite fallback resolved the sane player\n");
        putu(game_ + OFF_GAME_ACTIVE_PLAYER, 0);      /* restore           */
    }
}

static void test_observer_log(void) {
    /* round-9: present hook logs TWO lines per change sample — the NAMED
     * `res food=%d wood=%d coin=%d export=%d` (map food=slot2, wood=slot0,
     * coin=slot1, export=slot7) plus the RAW `raw 0..7 incA incB incN` trail.
     * ROUND-10: three more PROBE lines (altCap/altAdd/altPanel) follow on the
     * same sample so a single in-game run can be compared slot-by-slot against
     * the HUD; in the zeroed fake world they are all "skip".
     * A `--- match start n=%d idx=%d ---` separator + snapshot reset fires on
     * the first sample of a resolved world (ctx 0->non-0) so the next frame
     * always logs. Seeds all four mapped slots to prove the label->slot map. */
    DWORD container = 0;
    LPVOID base = make_fake_age3y(&container);
    if (!base) { printf("FAIL: alloc (observer)\n"); failures++; return; }
    delete_chain_log();
    g_res = g_inc = NULL;
    s_last_ctx = 0; s_last_player = 0; s_last_n = -1;    /* force match-start */
    s_snap_have = 0;                                     /* fresh snapshot    */
    locate_resources_at(base);                       /* authoritative idx=0 */
    if (g_res != (void *)(DWORD_PTR)container) {
        printf("FAIL: observer world did not resolve\n");
        failures++; VirtualFree(base, 0, MEM_RELEASE); return;
    }
    /* R9 map: food=slot2, wood=slot0, coin=slot1, export=slot7 */
    set_encrypted_slot(container, 0, 200.0f, KEY_BYTES);
    set_encrypted_slot(container, 1, 30.0f,  KEY_BYTES);
    set_encrypted_slot(container, 2, 500.0f, KEY_BYTES);
    set_encrypted_slot(container, 7, 80.0f,  KEY_BYTES);
    observer_sample();
    if (!log_contains("--- match start n=1 idx=0 ---"))
        { printf("FAIL: match-start separator not logged\n"); failures++; }
    else printf("PASS: match-start separator logged (n=1 idx=0)\n");
    if (!log_contains("res food=500 wood=200 coin=30 export=80"))
        { printf("FAIL: first named snapshot not logged (food/wood/coin/export)\n"); failures++; }
    else printf("PASS: first named snapshot logged (food=500 wood=200 coin=30 export=80)\n");
    if (!log_contains("raw 0=200 1=30 2=500") || !log_contains("7=80"))
        { printf("FAIL: raw trail line missing map slots\n"); failures++; }
    else printf("PASS: raw trail line carries all map slots\n");

    int n0 = log_count();
    observer_sample();                               /* unchanged -> silent */
    if (log_count() != n0)
        { printf("FAIL: unchanged snapshot re-logged\n"); failures++; }
    else printf("PASS: unchanged snapshot logs nothing (on-change only)\n");

    set_encrypted_slot(container, 0, 203.0f, KEY_BYTES);   /* wood +3.0 */
    observer_sample();   /* +3.0 >= 0.5 -> named+raw, then the ROUND-10 probe
                          * lines altCap/altAdd/altPanel — in THIS zeroed fake
                          * world all three containers are NULL, so exactly
                          * n0+5 lines (named + raw + 3 "skip" probe lines). */
    if (!log_contains("res food=500 wood=203") || log_count() != n0 + 5)
        { printf("FAIL: >=0.5 change did not re-log named+raw+probe lines once\n"); failures++; }
    else if (!log_contains("altCap skip") || !log_contains("altAdd skip") ||
             !log_contains("altPanel skip"))
        { printf("FAIL: ROUND-10 probe skip lines missing (NULL alt containers)\n"); failures++; }
    else printf("PASS: >=0.5 change re-logs once (wood 200->203, named+raw+probes)\n");

    s_snap_have = 0;
    observer_sample();
    if (!log_contains("raw 0=203") || !log_contains("incA=2000") ||
        !log_contains("incB=1000") || !log_contains("incN=3"))
        { printf("FAIL: raw line missing inc fields\n"); failures++; }
    else printf("PASS: raw line includes incA/incB/incN on the income object\n");
    VirtualFree(base, 0, MEM_RELEASE);
}

static void test_nullsafe_actual(void) {
    SYSTEM_INFO si; GetSystemInfo(&si);
    SIZE_T size = ((MAX_RVA + si.dwAllocationGranularity - 1)
                   / si.dwAllocationGranularity) * si.dwAllocationGranularity;
    LPVOID base = VirtualAlloc(NULL, size, MEM_COMMIT, PAGE_READWRITE);
    if (!base) { printf("FAIL: alloc(nullsafe)\n"); failures++; return; }
    memset(base, 0, (size_t)size);
    /* game ptr cell = 0 (from memset) */
    g_res = g_inc = (void*)0xDEADBEEF;
    locate_resources_at(base);
    if (g_res != NULL || g_inc != NULL) { printf("FAIL: zero game cell\n"); failures++; }
    observer_sample();
    VirtualFree(base, 0, MEM_RELEASE);
    printf("PASS: zero game-ptr cell -> NULLs, observer no-op (no crash)\n");

    /* main-menu rate reads all 0.0f when g_res NULL */
    g_res = g_inc = NULL;
    for (int s = 0; s < 8; s++)
        if (rate_for(s) != 0.0f) { printf("FAIL: rate_for NULL g_res\n"); failures++; }
    printf("PASS: rate_for returns 0.0f when g_res NULL (main menu)\n");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    test_authoritative_happy_path();
    test_live_values();
    test_fallback_menu_like();
    test_fallback_oob();
    test_fallback_none();

    DWORD container = 0;
    LPVOID base = make_fake_age3y(&container);
    if (!base) { printf("FAIL: big alloc\n"); return 1; }
    test_roundtrip_actual(base, container);
    VirtualFree(base, 0, MEM_RELEASE);

    test_nullsafe_actual();
    test_observer_log();

    /* unmapped-page guard: locate against an address not mapped */
    g_res = g_inc = (void*)0xDEADBEEF;
    locate_resources_at((void*)0x7F000000);
    if (g_res != NULL || g_inc != NULL) { printf("FAIL: unmapped -> non-NULL\n"); failures++; }
    observer_sample();
    printf("PASS: unmapped page guard -> NULLs, observer no-op (no crash)\n");

    printf(failures ? "D3D9-ACTUAL FAILURES: %d\n" : "ALL D3D9 ACTUAL CHECKS PASSED\n",
           failures);
    return failures ? 1 : 0;
}