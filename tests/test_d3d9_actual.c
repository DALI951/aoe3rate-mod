/*
 * test_d3d9_actual.c — independent SWARM test that compiles the REAL d3d9.c
 * into the test binary (via SWARM_TEST include) and drives its actual
 * production functions: locate_resources_at(), decrypt_slot(), rate_for().
 *
 * ROUND 13: tests rewritten for new player selection (human = index 1 when
 * n>=2, fallback to largest-food scan) and new output format:
 *   RES t=%d food=%d wood=%d coin=%d export=%d player=%08X res=%08X
 * Slot map: food=slot2, wood=slot1, coin=slot0, export=slot7.
 *
 * Build (from tests dir):
 *   i686-w64-mingw32-gcc.exe test_d3d9_actual.c -o test_d3d9_actual.exe
 */
#define SWARM_TEST
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "..\d3d9.c"

static const unsigned char KEY_BYTES[32] = {
    0x28, 0x48, 0xAC, 0x4F, 0x94, 0xF8, 0x3A, 0x35,
    0x8B, 0xD8, 0x4C, 0x3F, 0xAB, 0x12, 0xFB, 0xAF,
    0x20, 0xB3, 0x5B, 0xCA, 0xF9, 0xAB, 0xC4, 0x2A,
    0xB1, 0xA1, 0xCF, 0xDA, 0xF2, 0xE4, 0x82, 0x10,
};
#define MAX_RVA 0xA00000u

static int failures = 0;

static void delete_chain_log(void);
static int log_contains(const char *needle);
static int log_count(void);

static void putu(DWORD a, DWORD v) { *(volatile DWORD *)a = v; }
static void putf(DWORD a, float f) { DWORD b; memcpy(&b, &f, 4); putu(a, b); }

static void set_encrypted_slot(DWORD res_base, int slot, float value,
                               const unsigned char *keybytes) {
    DWORD key = keybytes[slot*4+0] | (keybytes[slot*4+1]<<8)
              | (keybytes[slot*4+2]<<16) | (keybytes[slot*4+3]<<24);
    DWORD bits; memcpy(&bits, &value, 4);
    *(volatile DWORD *)(res_base + slot*4) = key ^ bits;
}

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
    DWORD container = b + 0x7000;

    putu(b + RVA_GAME_PTR, game);
    putu(game + OFF_GAME_CTX, ctx);
    putu(ctx + OFF_CTX_SNAP, 3000);
    putu(ctx + OFF_CTX_PLAYERCNT, 1);
    putu(ctx + OFF_CTX_PLAYERS, players);
    putu(players + 0, player);
    putu(game + OFF_GAME_ACTIVE_PLAYER, 0);
    set_encrypted_slot(container, 0, 200.0f, KEY_BYTES);
    putu(player + OFF_PLAYER_RES, container);
    putu(player + OFF_PLAYER_INCOME, inc);
    putu(inc + OFF_INC_CUR, 2000);
    putu(inc + OFF_INC_PREV, 1000);
    putu(inc + OFF_INC_COUNT, 3);
    putu(inc + OFF_INC_HIST, hist);
    for (int i = 0; i < 3; i++) {
        DWORD c = b + 0x8000 + i * 0x80;
        putu(hist + i * 4, c);
        set_encrypted_slot(c, 0, (float)(1000 + i * 500), KEY_BYTES);
    }

    if (out_container) *out_container = container;
    return p;
}

/* R13 test: n=3, player[1] valid -> must resolve to player[1] */
static void test_primary_p1(void) {
    LPVOID img = make_fake_image();
    if (!img) { printf("FAIL: alloc (p1)\n"); failures++; return; }
    DWORD b = (DWORD)(DWORD_PTR)img;

    DWORD game = b + 0x1000, ctx = b + 0x2000, players = b + 0x3000;
    putu(b + RVA_GAME_PTR, game);
    putu(game + OFF_GAME_CTX, ctx);
    putu(ctx + OFF_CTX_SNAP, 3000);
    putu(ctx + OFF_CTX_PLAYERCNT, 3);
    putu(ctx + OFF_CTX_PLAYERS, players);

    for (int i = 0; i < 3; i++) {
        DWORD p = b + 0x4000 + (DWORD)i * 0x1000;
        putu(players + (DWORD)i * 4, p);
        DWORD cont = p + 0x300, inc = p + 0x600;
        set_encrypted_slot(cont, 0, (float)(150 + i * 100), KEY_BYTES);
        putu(p + OFF_PLAYER_RES, cont);
        putu(p + OFF_PLAYER_INCOME, inc);
    }
    DWORD p1 = b + 0x5000;
    DWORD c1 = p1 + 0x300, i1 = p1 + 0x600;

    delete_chain_log();
    g_res = g_inc = NULL;
    locate_resources_at(img);
    if (g_res != (void *)c1 || g_inc != (void *)i1) {
        printf("FAIL: n=3 must resolve player#1 (human-p1), got res=%p inc=%p\n", g_res, g_inc);
        failures++;
    } else {
        printf("PASS: n=3 -> resolves player#1 [human-p1]\n");
    }
    if (!log_contains("[human-p1]")) {
        printf("FAIL: chain log missing [human-p1] tag\n");
        failures++;
    } else {
        printf("PASS: chain log carries [human-p1] tag\n");
    }
    VirtualFree(img, 0, MEM_RELEASE);
}

/* R13 test: n=3, player[1] has no res -> fallback scans all, picks largest food */
static void test_fallback_p1_broken(void) {
    LPVOID img = make_fake_image();
    if (!img) { printf("FAIL: alloc (fb-p1-broken)\n"); failures++; return; }
    DWORD b = (DWORD)(DWORD_PTR)img;

    DWORD game = b + 0x1000, ctx = b + 0x2000, players = b + 0x3000;
    putu(b + RVA_GAME_PTR, game);
    putu(game + OFF_GAME_CTX, ctx);
    putu(ctx + OFF_CTX_SNAP, 3000);
    putu(ctx + OFF_CTX_PLAYERCNT, 3);
    putu(ctx + OFF_CTX_PLAYERS, players);

    /* p0: food=500, p1: no res, p2: food=200 */
    for (int i = 0; i < 3; i++) {
        DWORD p = b + 0x4000 + (DWORD)i * 0x1000;
        putu(players + (DWORD)i * 4, p);
    }
    {   /* p0: food=500 (slot 2) */
        DWORD p = b + 0x4000, cont = p + 0x300, inc = p + 0x600;
        set_encrypted_slot(cont, 2, 500.0f, KEY_BYTES);
        putu(p + OFF_PLAYER_RES, cont);
        putu(p + OFF_PLAYER_INCOME, inc);
    }
    {   /* p1: no res -> broken */
        DWORD p = b + 0x5000;
        putu(p + OFF_PLAYER_RES, 0);
        putu(p + OFF_PLAYER_INCOME, p + 0x600);
    }
    {   /* p2: food=200 */
        DWORD p = b + 0x6000, cont = p + 0x300, inc = p + 0x600;
        set_encrypted_slot(cont, 2, 200.0f, KEY_BYTES);
        putu(p + OFF_PLAYER_RES, cont);
        putu(p + OFF_PLAYER_INCOME, inc);
    }

    delete_chain_log();
    g_res = g_inc = NULL;
    locate_resources_at(img);
    DWORD expected_res = b + 0x4300;  /* p0's container */
    if (g_res != (void *)expected_res) {
        printf("FAIL: p1 broken -> fallback should pick p0 (food=500), got res=%p\n", g_res);
        failures++;
    } else {
        printf("PASS: p1 broken -> fallback picks p0 (largest food=500)\n");
    }
    if (!log_contains("[fallback:")) {
        printf("FAIL: chain log missing [fallback: tag\n");
        failures++;
    } else {
        printf("PASS: chain log carries [fallback: tag\n");
    }
    VirtualFree(img, 0, MEM_RELEASE);
}

/* R13 test: n=1 -> fallback scans all (only p0 available) */
static void test_single_player(void) {
    LPVOID img = make_fake_image();
    if (!img) { printf("FAIL: alloc (single)\n"); failures++; return; }
    DWORD b = (DWORD)(DWORD_PTR)img;

    DWORD game = b + 0x1000, ctx = b + 0x2000, players = b + 0x3000;
    putu(b + RVA_GAME_PTR, game);
    putu(game + OFF_GAME_CTX, ctx);
    putu(ctx + OFF_CTX_SNAP, 3000);
    putu(ctx + OFF_CTX_PLAYERCNT, 1);
    putu(ctx + OFF_CTX_PLAYERS, players);
    DWORD p = b + 0x4000;
    putu(players + 0, p);
    DWORD cont = p + 0x300, inc = p + 0x600;
    set_encrypted_slot(cont, 2, 100.0f, KEY_BYTES);
    putu(p + OFF_PLAYER_RES, cont);
    putu(p + OFF_PLAYER_INCOME, inc);

    delete_chain_log();
    g_res = g_inc = NULL;
    locate_resources_at(img);
    if (g_res != (void *)cont || g_inc != (void *)inc) {
        printf("FAIL: n=1 fallback must resolve p0, got res=%p inc=%p\n", g_res, g_inc);
        failures++;
    } else {
        printf("PASS: n=1 -> fallback resolves p0\n");
    }
    VirtualFree(img, 0, MEM_RELEASE);
}

/* R13 test: n=0 (menu) -> chain-break, NULLs */
static void test_menu(void) {
    LPVOID img = make_fake_image();
    if (!img) { printf("FAIL: alloc (menu)\n"); failures++; return; }
    DWORD b = (DWORD)(DWORD_PTR)img;

    DWORD game = b + 0x1000, ctx = b + 0x2000, players = b + 0x3000;
    putu(b + RVA_GAME_PTR, game);
    putu(game + OFF_GAME_CTX, ctx);
    putu(ctx + OFF_CTX_SNAP, 3000);
    putu(ctx + OFF_CTX_PLAYERCNT, 0);
    putu(ctx + OFF_CTX_PLAYERS, players);

    delete_chain_log();
    g_res = g_inc = (void*)0xDEADBEEF;
    locate_resources_at(img);
    if (g_res != NULL || g_inc != NULL) {
        printf("FAIL: n=0 menu -> expected NULLs, got res=%p\n", g_res);
        failures++;
    } else {
        printf("PASS: n=0 menu -> NULLs (chain-break)\n");
    }
    if (!log_contains("[chain-break: n<=0]")) {
        printf("FAIL: chain-break tag missing for n=0\n");
        failures++;
    } else {
        printf("PASS: chain-break tag logged for n=0\n");
    }
    VirtualFree(img, 0, MEM_RELEASE);
}

/* R13 test: n=3, all players have food=0 -> fallback picks first valid */
static void test_all_zero_food(void) {
    LPVOID img = make_fake_image();
    if (!img) { printf("FAIL: alloc (allzero)\n"); failures++; return; }
    DWORD b = (DWORD)(DWORD_PTR)img;

    DWORD game = b + 0x1000, ctx = b + 0x2000, players = b + 0x3000;
    putu(b + RVA_GAME_PTR, game);
    putu(game + OFF_GAME_CTX, ctx);
    putu(ctx + OFF_CTX_SNAP, 3000);
    putu(ctx + OFF_CTX_PLAYERCNT, 3);
    putu(ctx + OFF_CTX_PLAYERS, players);

    for (int i = 0; i < 3; i++) {
        DWORD p = b + 0x4000 + (DWORD)i * 0x1000;
        putu(players + (DWORD)i * 4, p);
        DWORD cont = p + 0x300, inc = p + 0x600;
        set_encrypted_slot(cont, 2, 0.0f, KEY_BYTES);  /* food=0 */
        putu(p + OFF_PLAYER_RES, cont);
        putu(p + OFF_PLAYER_INCOME, inc);
    }

    delete_chain_log();
    g_res = g_inc = NULL;
    locate_resources_at(img);
    /* p1 is checked first (n>=2), food=0 but valid container+inc -> p1 wins */
    DWORD p1 = b + 0x5000, c1 = p1 + 0x300;
    if (g_res != (void *)c1) {
        printf("FAIL: all-zero food n>=2 -> should resolve p1 (primary path), got res=%p\n", g_res);
        failures++;
    } else {
        printf("PASS: all-zero food n>=2 -> resolves p1 (primary path still works)\n");
    }
    VirtualFree(img, 0, MEM_RELEASE);
}

/* roundtrip + decrypt + rate: unchanged from R12 (just uses the one-player world) */
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

    float r0 = rate_for(0);
    if (r0 != 562.5f) {
        printf("FAIL: rate_for(0) expected 562.5, got %g\n", (double)r0);
        failures++;
    } else printf("PASS: rate_for(0) == 562.5 (FUN_0086da39 math)\n");

    /* menu: rate_for returns 0.0f when n==0 */
    {
        DWORD game_ = *(volatile DWORD *)((DWORD)(DWORD_PTR)base + RVA_GAME_PTR);
        DWORD ctx_  = *(volatile DWORD *)(game_ + OFF_GAME_CTX);
        putu(ctx_ + OFF_CTX_PLAYERCNT, 0);
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
        putu(ctx_ + OFF_CTX_PLAYERCNT, 1);
        printf("PASS: menu (n==0) -> NULLs and rate_for 0.0f\n");
    }
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

/* R13 observer_log test: every tick produces a RES line, new format + slot map */
static void test_observer_log(void) {
    DWORD container = 0;
    LPVOID base = make_fake_age3y(&container);
    if (!base) { printf("FAIL: alloc (observer)\n"); failures++; return; }
    delete_chain_log();
    g_res = g_inc = NULL;
    s_last_ctx = 0; s_last_player = 0; s_last_n = -1;
    s_snap_have = 0;
    s_tick = 0;
    g_settings.debug_enabled = 1;   /* R16 A3: RES lines are DebugEnabled-gated */
    locate_resources_at(base);
    if (g_res != (void *)(DWORD_PTR)container) {
        printf("FAIL: observer world did not resolve\n");
        failures++; VirtualFree(base, 0, MEM_RELEASE); return;
    }
    /* R13 map: food=slot2, wood=slot1, coin=slot0, export=slot7 */
    set_encrypted_slot(container, 0, 200.0f, KEY_BYTES);  /* coin */
    set_encrypted_slot(container, 1, 30.0f, KEY_BYTES);   /* wood */
    set_encrypted_slot(container, 2, 500.0f, KEY_BYTES);  /* food */
    set_encrypted_slot(container, 7, 80.0f, KEY_BYTES);   /* export */

    observer_sample();  /* tick 1 */
    if (!log_contains("--- match start n=1 player="))
        { printf("FAIL: match-start separator not logged\n"); failures++; }
    else printf("PASS: match-start separator logged\n");

    if (!log_contains("RES t=1 food=500 wood=30 coin=200 export=80"))
        { printf("FAIL: first RES line not logged (food=500 wood=30 coin=200 export=80)\n"); failures++; }
    else printf("PASS: first RES line logged (food=500 wood=30 coin=200 export=80)\n");

    int n0 = log_count();

    /* with DebugEnabled=1, each tick logs TWO lines: the `rates` debug line
     * + the RES line (R16 A3 put both per-tick lines in the same gate) */
    observer_sample();  /* tick 2, same values -> STILL logs (every tick) */
    if (log_count() != n0 + 2)
        { printf("FAIL: tick 2 must log (every tick)\n"); failures++; }
    else printf("PASS: tick 2 logs (every tick)\n");

    observer_sample();  /* tick 3 */
    if (log_count() != n0 + 4)
        { printf("FAIL: tick 3 must log\n"); failures++; }
    else printf("PASS: tick 3 logs\n");

    /* verify slot map: change coin (slot 0) from 200 -> 250 */
    set_encrypted_slot(container, 0, 250.0f, KEY_BYTES);
    observer_sample();  /* tick 4 */
    if (!log_contains("coin=250"))
        { printf("FAIL: slot 0 change should show coin=250\n"); failures++; }
    else printf("PASS: slot 0 change shows coin=250 (correct slot map)\n");

    /* verify NO raw trail lines */
    if (log_contains("raw 0="))
        { printf("FAIL: raw trail line should be removed\n"); failures++; }
    else printf("PASS: no raw trail line (removed)\n");

    /* verify NO per-player prefix */
    if (log_contains("P0 res") || log_contains("P1 res"))
        { printf("FAIL: per-player P%%d prefix should be removed\n"); failures++; }
    else printf("PASS: no per-player P%%d prefix (removed)\n");

    /* verify NO dump/cell/resraw/altCap lines */
    if (log_contains("cell P") || log_contains("dump") || log_contains("resraw") ||
        log_contains("altCap") || log_contains("altAdd") || log_contains("altPanel"))
        { printf("FAIL: dump/cell/resraw/probe lines should be removed\n"); failures++; }
    else printf("PASS: no dump/cell/resraw/probe lines (removed)\n");

    g_settings.debug_enabled = 0;
    VirtualFree(base, 0, MEM_RELEASE);
}

static void test_nullsafe_actual(void) {
    SYSTEM_INFO si; GetSystemInfo(&si);
    SIZE_T size = ((MAX_RVA + si.dwAllocationGranularity - 1)
                   / si.dwAllocationGranularity) * si.dwAllocationGranularity;
    LPVOID base = VirtualAlloc(NULL, size, MEM_COMMIT, PAGE_READWRITE);
    if (!base) { printf("FAIL: alloc(nullsafe)\n"); failures++; return; }
    memset(base, 0, (size_t)size);
    g_res = g_inc = (void*)0xDEADBEEF;
    locate_resources_at(base);
    if (g_res != NULL || g_inc != NULL) { printf("FAIL: zero game cell\n"); failures++; }
    observer_sample();
    VirtualFree(base, 0, MEM_RELEASE);
    printf("PASS: zero game-ptr cell -> NULLs, observer no-op (no crash)\n");

    g_res = g_inc = NULL;
    for (int s = 0; s < 8; s++)
        if (rate_for(s) != 0.0f) { printf("FAIL: rate_for NULL g_res\n"); failures++; }
    printf("PASS: rate_for returns 0.0f when g_res NULL (main menu)\n");
}

/* ==== ROUND 10 (P0): ARMED/DISABLED status + GDI visible fallback ==== */
/* R15 SWAPCHAIN block: a REAL device through the production wrapper —
 * GetSwapChain (device slot 14) must resolve to OUR w_get_swapchain, and the
 * returned swapchain's Present (slot 3) must be patched to OUR sw_present_hook
 * with the original saved once and the install line logged. Runs against the
 * real system d3d9.dll (loaded for g_real) so the vtable layout is real. */
static void test_swapchain_hook(void) {
    if (g_real == NULL) {
        char sys[MAX_PATH];
        if (!GetSystemDirectoryA(sys, MAX_PATH)) return;
        lstrcatA(sys, "\\d3d9.dll");
        g_real = LoadLibraryA(sys);
    }
    if (g_real == NULL) { printf("FAIL: real d3d9.dll not loadable (sw)\n"); failures++; return; }
    logger_init();
    delete_chain_log();

    HWND h = CreateWindowA("STATIC", "rrmod-sw", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           0, 0, 320, 240, NULL, NULL, GetModuleHandleA(NULL), NULL);
    if (h == NULL) { printf("FAIL: CreateWindowA (sw)\n"); failures++; return; }

    void *d3d = Direct3DCreate9(32);
    if (d3d == NULL) { printf("FAIL: Direct3DCreate9 (sw)\n"); DestroyWindow(h); failures++; return; }

    /* D3DPRESENT_PARAMETERS (windowed): zeroed 64 bytes; poke the real offsets
     * (i386 COM struct, d3d9types.h): [3]=BackBufferCount, [6]=SwapEffect
     * (DISCARD), [7]=hDeviceWindow, [8]=Windowed. */
    DWORD pp[16];
    memset(pp, 0, sizeof(pp));
    pp[3] = 1;
    pp[6] = 1;
    pp[7] = (DWORD)(DWORD_PTR)h;
    pp[8] = 1;

    void *dev = NULL;
    int hr = w_create_device(d3d, 0, 1 /*D3DDEVTYPE_HAL*/, h,
                             0x20 /*D3DCREATE_SOFTWARE_VERTEXPROCESSING*/, pp, &dev);
    if (hr < 0 || dev == NULL)
        hr = w_create_device(d3d, 0, 2 /*D3DDEVTYPE_REF*/, h,
                             0x20 /*D3DCREATE_SOFTWARE_VERTEXPROCESSING*/, pp, &dev);
    if (hr < 0 || dev == NULL) {
        printf("FAIL: CreateDevice (sw) hr=%#010x\n", (unsigned)hr);
        DestroyWindow(h);
        failures++;
        return;
    }

    void **vt = *(void ***)dev;
    if (vt == NULL || vt[14] != (void *)w_get_swapchain) {
        printf("FAIL: device GetSwapChain (slot 14) is not our wrapper\n");
        failures++;
    } else {
        printf("PASS: device slot 14 -> w_get_swapchain (capture in place)\n");
    }

    /* R17 EAGER implicit-swapchain capture: w_create_device ran
     * eager_implicit_swapchain() on the game's behalf INSIDE the create call —
     * the runtime's implicit swapchain was captured+patched before THIS test
     * ever called GetSwapChain. (Slot-3 hook + saved original + g_sw + BOTH
     * the patch_swapchain AND swapchain-impl lines must already be present.) */
    if (s_orig_sw_present == NULL || g_sw == NULL) {
        printf("FAIL: w_create_device did not eagerly capture the implicit swapchain\n");
        failures++;
    } else {
        printf("PASS: implicit swapchain eagerly captured+patched at device create (no game GetSwapChain call)\n");
    }
    if (!log_contains("swapchain-impl: sw=")) {
        printf("FAIL: swapchain-impl capture line not logged\n");
        failures++;
    } else {
        printf("PASS: `swapchain-impl: sw=.. patched=1` logged at create\n");
    }
    if (!log_contains("patch_swapchain: sw=")) {
        printf("FAIL: patch_swapchain install line not logged for the implicit swapchain\n");
        failures++;
    } else {
        printf("PASS: implicit swapchain went through the R15 patch logic (patch_swapchain: sw=..)\n");
    }

    void *sw = NULL;
    int h2 = ((DEV_GETSC)vt[14])(dev, 0, &sw);
    if (h2 < 0 || sw == NULL) {
        printf("FAIL: GetSwapChain(0) returned nothing (hr=%#010x)\n", (unsigned)h2);
        failures++;
    } else {
        void **svt = *(void ***)sw;
        int ok = (svt != NULL) && (svt[3] == (void *)sw_present_hook);
        if (!ok) {
            printf("FAIL: swapchain Present (slot 3) not patched to sw_present_hook\n");
            failures++;
        } else {
            printf("PASS: swapchain slot 3 -> sw_present_hook (in-match Present hooked)\n");
        }
        if (s_orig_sw_present == NULL) {
            printf("FAIL: original swapchain Present not saved\n");
            failures++;
        } else {
            printf("PASS: original swapchain Present saved (s_orig_sw_present)\n");
        }
        if (g_sw != sw) {
            printf("FAIL: g_sw not the captured swapchain\n");
            failures++;
        } else {
            printf("PASS: g_sw == captured swapchain\n");
        }
        if (!log_contains("patch_swapchain: sw=")) {
            printf("FAIL: patch_swapchain install line not logged\n");
            failures++;
        } else {
            printf("PASS: `patch_swapchain: sw=.. vt=.. present=..` logged on install\n");
        }
        /* second GetSwapChain: same vtable already patched; orig unchanged */
        void *sw2 = NULL;
        ((DEV_GETSC)vt[14])(dev, 0, &sw2);
        if (*(void ***)dev == NULL) { /* never */ }
        if (sw2 == NULL) {
            printf("FAIL: second GetSwapChain(0) returned nothing\n");
            failures++;
        } else {
            if ((*(void ***)sw2)[3] != (void *)sw_present_hook) {
                printf("FAIL: re-patched swapchain lost our hook\n");
                failures++;
            } else {
                printf("PASS: repeated GetSwapChain stays patched (idempotent)\n");
            }
        }
    }

    /* R16 present-path tracer: drive BOTH production hooks against the REAL
     * device + REAL swapchain. g_settings.enabled=0 (zero-touch common body:
     * only the counters move, no chain/font/RT work) while every call still
     * FORWARDS through the true d3d9.dll Present — 150 device + 150 swapchain
     * = 300 combined, so the mod-300 tracer line MUST fire exactly once and
     * the readbacks must be 150/150. Proves the counters are live on the
     * actual vtable call path (the R16 primary diagnostic). */
    {
        int saved_en = g_settings.enabled;
        g_settings.enabled = 0;
        int k;
        for (k = 0; k < 150; k++) {
            ((PRESENT_FN)vt[17])(dev, NULL, NULL, NULL, NULL);
            sw_present_hook(sw, NULL, NULL, NULL, NULL, 0);
        }
        g_settings.enabled = saved_en;
        if (s_dev_presents != 150 || s_sw_presents != 150) {
            printf("FAIL: present-path counters wrong (%u device / %u swapchain)\n",
                   s_dev_presents, s_sw_presents);
            failures++;
        } else {
            printf("PASS: tracer counted 150 device + 150 swapchain presents\n");
        }
        if (!log_contains("present-path dev=")) {
            printf("FAIL: present-path tracer line not logged at the 300-boundary\n");
            failures++;
        } else {
            printf("PASS: `present-path dev=150 sw=150 scene=0 frames=..` logged at the 300-boundary\n");
        }
        if (s_orig_casc != NULL && ((DEV_CASC)vt[13]) == (DEV_CASC)w_casc) {
            printf("PASS: device slot 13 -> w_casc (CreateAdditionalSwapChain captured)\n");
        } else {
            printf("FAIL: device slot 13 (CreateAdditionalSwapChain) not wrapped\n");
            failures++;
        }

        /* R17 scene-alive probe: drive REAL EndScene through the patched slot
         * 42 (w_endscene -> forwards to real d3d9 EndScene). +300 combined
         * calls take the merged counter 300 -> 600, so the mod-300 tracer MUST
         * fire a SECOND line and it MUST show the scene field climbing while
         * dev/sw hold at 150 (the exact R17 in-match scenario shape). */
        if (vt[42] != (void *)w_endscene) {
            printf("FAIL: device EndScene (slot 42) is not w_endscene (scene probe absent)\n");
            failures++;
        } else {
            printf("PASS: device slot 42 -> w_endscene (EndScene probe in place)\n");
        }
        {
            int k2;
            for (k2 = 0; k2 < 300; k2++) ((DEV_ENDSCENE)vt[42])(dev);
        }
        if (s_scene_ends != 300) {
            printf("FAIL: scene counter wrong (%u)\n", s_scene_ends);
            failures++;
        } else {
            printf("PASS: scene counter rose to 300 via real EndScene calls\n");
        }
        if (s_dev_presents != 150 || s_sw_presents != 150) {
            printf("FAIL: dev/sw counters moved during the scene probe\n");
            failures++;
        } else {
            printf("PASS: dev/sw hold at 150/150 while scene climbs (device alive, present frozen)\n");
        }
        if (!log_contains("scene=300")) {
            printf("FAIL: merged tracer line missing the scene=300 boundary\n");
            failures++;
        } else {
            printf("PASS: `present-path dev=150 sw=150 scene=300 frames=..` at the 600-boundary\n");
        }
    }

    /* release the created objects via the vtable (COM) */
    void **vd = *(void ***)dev;
    if (vd != NULL && vd[2] != NULL) ((void (STDMETHODCALLTYPE *)(void *))vd[2])(dev);
    DestroyWindow(h);
}

static void test_overlay_status_p0(void) {
    /* pure helper: version-first precedence, then d3dx9 loadability */
    if (strstr(overlay_state_reason(0, 1, 0), "ver:") == NULL)
        { printf("FAIL: overlay_state_reason(0,1,0) must contain \"ver:\"\n"); failures++; }
    else printf("PASS: state_reason(0,1,0) -> ver: reason (version precedence)\n");
    if (strstr(overlay_state_reason(1, 0, 0), "d3dx9_25.dll") == NULL)
        { printf("FAIL: overlay_state_reason(1,0,0) must mention d3dx9_25.dll\n"); failures++; }
    else printf("PASS: state_reason(1,0,0) -> d3dx9_25.dll not loadable\n");
    if (overlay_state_reason(1, 1, 0)[0] != '\0')
        { printf("FAIL: overlay_state_reason(1,1,0) must be empty (armed)\n"); failures++; }
    else printf("PASS: state_reason(1,1,0) -> empty (overlay ARMED)\n");
    if (overlay_state_reason(1, 1, 1)[0] != '\0')
        { printf("FAIL: overlay_state_reason(1,1,1) must be empty (ini is a suffix, not a disable)\n"); failures++; }
    else printf("PASS: state_reason(1,1,1) -> empty (ini-missing never disables)\n");

    /* no-window safety: scan finds no visible same-process window -> 0, no crash */
    g_version_ok = 0;            /* force DISABLED so the gate lets the scan run */
    g_version_reason[0] = '\0';
    if (ui_gdi_fallback_draw() != 0)
        { printf("FAIL: no matching HWND must return 0\n"); failures++; }
    else printf("PASS: GDI fallback with no window -> 0 (safe no-op)\n");

    /* hidden-window smoke: a real visible top-level window is found and drawn */
    HWND h = CreateWindowA("STATIC", "rrmod-p0", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           0, 0, 200, 100, NULL, NULL, GetModuleHandleA(NULL), NULL);
    if (h == NULL) { printf("FAIL: CreateWindowA failed\n"); failures++; return; }
    ShowWindow(h, SW_SHOW);
    UpdateWindow(h);
    if (ui_gdi_fallback_draw() != 1)
        { printf("FAIL: disabled + visible window must draw (return 1)\n"); failures++; }
    else printf("PASS: GDI fallback draws on the visible window (return 1)\n");
    DestroyWindow(h);
    if (ui_gdi_fallback_draw() != 0)
        { printf("FAIL: destroyed window must return 0\n"); failures++; }
    else printf("PASS: destroyed window -> 0 (stale HWND safe)\n");

    /* armed -> the gate short-circuits before any GDI work */
    ui_test_set_ready(1);
    g_version_ok = 1;
    g_version_reason[0] = '\0';
    if (ui_gdi_fallback_draw() != 0)
        { printf("FAIL: ARMED (version ok + ui ready) must draw nothing\n"); failures++; }
    else printf("PASS: ARMED -> ui_gdi_fallback_draw() returns 0 (no stray GDI line)\n");
    ui_test_set_ready(0);
    g_version_ok = 0;            /* leave the harness globals as found */
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    test_primary_p1();
    test_fallback_p1_broken();
    test_single_player();
    test_menu();
    test_all_zero_food();

    DWORD container = 0;
    LPVOID base = make_fake_age3y(&container);
    if (!base) { printf("FAIL: big alloc\n"); return 1; }
    test_roundtrip_actual(base, container);
    VirtualFree(base, 0, MEM_RELEASE);

    test_nullsafe_actual();
    test_observer_log();
    test_swapchain_hook();

    /* unmapped-page guard */
    g_res = g_inc = (void*)0xDEADBEEF;
    locate_resources_at((void*)0x7F000000);
    if (g_res != NULL || g_inc != NULL) { printf("FAIL: unmapped -> non-NULL\n"); failures++; }
    observer_sample();
    printf("PASS: unmapped page guard -> NULLs, observer no-op (no crash)\n");

    test_overlay_status_p0();

    printf(failures ? "D3D9-ACTUAL FAILURES: %d\n" : "ALL D3D9 ACTUAL CHECKS PASSED\n",
           failures);
    return failures ? 1 : 0;
}
