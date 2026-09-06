/*
 * test_nullsafe.c — SWARM test 6.2 (round 2), updated for the ROUND-8 pure
 * observer: the DLL no longer draws anything, so the "draw no-crash" cases
 * target observer_sample() (the read+log hook) instead.
 *
 * Harness: includes d3d9.c in the SAME translation unit (its statics are
 * callable) and lays down a fake "world" inside one 10 MB buffer at the real
 * RVAs, then exercises:
 *   1) game ptr cell == 0            -> g_res/g_inc stay NULL, observer no-ops,
 *   2) unmapped base                 -> guarded reads -> NULLs, no crash,
 *   3) active player id out of range -> fallback picks sane player,
 *   3b) main menu (n == 0)           -> NULLs, no crash,
 *   3c) active idx == -1 (0xFFFFFFFF, the EXACT live round-4 value, read from
 *       game+0x14c)                  -> fallback picks the sane player,
 *   4) FULL lived chain              -> g_res == container,
 *      decrypt_slot(slot) == known float, rate_for(slot) == hand-computed
 *      FUN_0086da39 math, observer with no device -> early return, no crash.
 *
 * Build: i686-w64-mingw32-gcc.exe test_nullsafe.c -o test_nullsafe.exe
 */
#ifndef SWARM_TEST
#define SWARM_TEST
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "d3d9.c"

/* fake world must cover RVA 0x866664 + our objects: 0xA00000 is enough */
#define BUF_SIZE 0xA00000u

static const unsigned char KEY_BYTES[32] = {
    0x28,0x48,0xAC,0x4F,0x94,0xF8,0x3A,0x35,
    0x8B,0xD8,0x4C,0x3F,0xAB,0x12,0xFB,0xAF,
    0x20,0xB3,0x5B,0xCA,0xF9,0xAB,0xC4,0x2A,
    0xB1,0xA1,0xCF,0xDA,0xF2,0xE4,0x82,0x10,
};

static LPVOID big_zero_buf(void) {
    SYSTEM_INFO si; GetSystemInfo(&si);
    DWORD page = si.dwAllocationGranularity;
    SIZE_T size = ((BUF_SIZE + page - 1) / page) * page;
    LPVOID p = VirtualAlloc(NULL, size, MEM_COMMIT, PAGE_READWRITE);
    if (p) memset(p, 0, (size_t)size);
    return p;
}

static void putf(DWORD addr, float f) { DWORD b; memcpy(&b, &f, 4); *(volatile DWORD *)addr = b; }
static void putu(DWORD addr, DWORD v) { *(volatile DWORD *)addr = v; }

/* fake world layout (offsets are RVAs / object offsets, exactly as the DLL) */
#define BASE_OFF_GAME       0x866234u      /* *(base+0x866234) = game  */
#define BASE_OFF_KEY        0x86DF14u
#define BASE_OFF_COUNT      0x86DF38u
#define BASE_OFF_TSTEP      0x79A5C8u
#define BASE_OFF_THRESH     0x7A12E8u
#define BASE_OFF_FUDGE      0x7A12FCu
#define BASE_OFF_DIV        0x7856BCu

static void install_world(LPVOID base, DWORD idx, int with_income) {
    DWORD b = (DWORD)(DWORD_PTR)base;
    DWORD game    = b + 0x1000;
    DWORD ctx     = b + 0x2000;
    DWORD players = b + 0x3000;
    DWORD player  = b + 0x4000;
    DWORD inc     = b + 0x5000;
    DWORD hist    = b + 0x6000;
    DWORD cont0   = b + 0x7000;
    DWORD cont1   = b + 0x8000;
    DWORD cont2   = b + 0x9000;
    DWORD resarr  = b + 0x100;              /* encrypted slot array   */

    /* constants the eval reads from the image */
    putu(b + BASE_OFF_TSTEP, *(DWORD *)&(float){0.001f});
    putu(b + BASE_OFF_THRESH, *(DWORD *)&(float){1e-6f});
    putu(b + BASE_OFF_FUDGE, *(DWORD *)&(float){4294967296.0f});
    putu(b + BASE_OFF_DIV, *(DWORD *)&(float){1.0f});
    memcpy((BYTE *)base + BASE_OFF_KEY, KEY_BYTES, 32);
    putu(b + BASE_OFF_COUNT, 8);

    /* player/context chain */
    putu(b + BASE_OFF_GAME,   game);
    putu(game + OFF_GAME_CTX, ctx);
    putu(ctx + OFF_CTX_SNAP,  3000);
    putu(ctx + OFF_CTX_PLAYERCNT, 1);
    putu(ctx + OFF_CTX_PLAYERS, players);
    putu(players + 0 * 4, player);
    putu(game + OFF_GAME_ACTIVE_PLAYER, idx);   /* round-5: game+0x14C */
    putu(player + OFF_PLAYER_RES, resarr);
    if (with_income) {
        putu(player + OFF_PLAYER_INCOME, inc);
        putu(inc + OFF_INC_CUR, 2000);
        putu(inc + OFF_INC_PREV, 1000);
        putu(inc + OFF_INC_COUNT, 3);
        putu(inc + OFF_INC_HIST, hist);
        putu(hist + 0 * 4, cont0);
        putu(hist + 1 * 4, cont1);
        putu(hist + 2 * 4, cont2);
        /* sample containers slot 0 values: cont0=1000, cont1=1000, cont2=2000 */
        float sv[3] = { 1000.0f, 1000.0f, 2000.0f };
        for (int i = 0; i < 3; i++) {
            DWORD c = (i == 0) ? cont0 : (i == 1) ? cont1 : cont2;
            DWORD key = KEY_BYTES[0] | (KEY_BYTES[1] << 8)
                      | (KEY_BYTES[2] << 16) | (KEY_BYTES[3] << 24);
            DWORD bits; memcpy(&bits, &sv[i], 4);
            putu(c + 0, key ^ bits);
        }
    }
    /* main container: slot 0 <-> 12345.0, slot 1 <-> 54321.0 */
    {
        float mainv[2] = { 12345.0f, 54321.0f };
        for (int s = 0; s < 2; s++) {
            DWORD key = KEY_BYTES[s*4+0] | (KEY_BYTES[s*4+1] << 8)
                      | (KEY_BYTES[s*4+2] << 16) | (KEY_BYTES[s*4+3] << 24);
            DWORD bits; memcpy(&bits, &mainv[s], 4);
            putu(resarr + s * 4, key ^ bits);
        }
    }
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int fail = 0;

    /* ---- case 1: game ptr cell == 0 ---- */
    {
        LPVOID p = big_zero_buf();
        if (!p) { printf("FAIL: alloc\n"); return 1; }
        g_res = (void *)0xDEADBEEF; g_inc = (void *)0xDEADBEEF;
        locate_resources_at(p);                /* *(p+0x866234) == 0   */
        if (g_res != NULL || g_inc != NULL) { printf("FAIL: game==0 left non-NULL\n"); fail++; }
        else printf("PASS: zero game-ptr cell -> g_res/g_inc NULL\n");
        observer_sample();
        printf("PASS: observer with NULL resources returned (no crash)\n");
        VirtualFree(p, 0, MEM_RELEASE);
    }

    /* ---- case 2: UNMAPPED base ---- */
    g_res = (void *)0xDEADBEEF; g_inc = (void *)0xDEADBEEF;
    locate_resources_at((void *)0x7F000000);
    if (g_res != NULL || g_inc != NULL) { printf("FAIL: unmapped -> non-NULL\n"); fail++; }
    else printf("PASS: unmapped base -> g_res/g_inc NULL (guard)\n");
    observer_sample();
    printf("PASS: observer with unmapped resources returned (no crash)\n");

    /* ---- case 3: active player id OUT OF RANGE -> probe-lite fallback ---- */
    {
        LPVOID p = big_zero_buf();
        if (!p) { printf("FAIL: alloc2\n"); return 1; }
        install_world(p, 7, 1);                /* idx 7 >= nplayers(1)   */
        locate_resources_at(p);
        if (g_res == NULL || g_inc == NULL) { printf("FAIL: oob idx + sane fallback\n"); fail++; }
        else printf("PASS: out-of-range active id -> fallback picked sane player\n");
        VirtualFree(p, 0, MEM_RELEASE);
    }

    /* ---- case 3b: main menu (n == 0) -> NULLs, no fallback candidates ---- */
    {
        LPVOID p = big_zero_buf();
        if (!p) { printf("FAIL: alloc2b\n"); return 1; }
        install_world(p, 0, 1);
        DWORD game_ = *(volatile DWORD *)((DWORD)(DWORD_PTR)p + BASE_OFF_GAME);
        DWORD ctx_  = *(volatile DWORD *)(game_ + OFF_GAME_CTX);
        putu(ctx_ + OFF_CTX_PLAYERCNT, 0);     /* menu: player count 0   */
        g_res = (void *)0xDEADBEEF; g_inc = (void *)0xDEADBEEF;
        locate_resources_at(p);
        if (g_res != NULL || g_inc != NULL) { printf("FAIL: menu n==0 -> non-NULL\n"); fail++; }
        else printf("PASS: main menu (n==0) -> g_res/g_inc NULL, no crash\n");
        VirtualFree(p, 0, MEM_RELEASE);
    }

    /* ---- case 3c: active idx == -1 (0xFFFFFFFF from game+0x14c, the EXACT
     * live round-4 value) -> probe-lite fallback picks the sane player ---- */
    {
        LPVOID p = big_zero_buf();
        if (!p) { printf("FAIL: alloc2c\n"); return 1; }
        install_world(p, 0xFFFFFFFF, 1);       /* game+0x14c = -1        */
        g_res = (void *)0xDEADBEEF; g_inc = (void *)0xDEADBEEF;
        locate_resources_at(p);
        if (g_res == NULL || g_inc == NULL) { printf("FAIL: idx=-1 + sane world\n"); fail++; }
        else printf("PASS: active idx -1 (live value) -> fallback picked sane player\n");
        VirtualFree(p, 0, MEM_RELEASE);
    }

    /* ---- case 4: FULL live chain ---- */
    {
        LPVOID p = big_zero_buf();
        if (!p) { printf("FAIL: alloc3\n"); return 1; }
        install_world(p, 0, 1);

        g_device = NULL;
        locate_resources_at(p);
        if (g_res == NULL || g_inc == NULL) { printf("FAIL: valid chain\n"); fail++; }
        else {
            if (g_res != (void *)((DWORD)(DWORD_PTR)p + 0x100)) { printf("FAIL: res addr\n"); fail++; }
            /* decrypt_slot(0) must equal 12345.0, slot 1 -> 54321.0 */
            float d0 = decrypt_slot(0), d1 = decrypt_slot(1);
            if (d0 != 12345.0f || d1 != 54321.0f) {
                printf("FAIL: decrypt live container %g/%g\n", (double)d0, (double)d1);
                fail++;
            } else printf("PASS: decrypt_slot reads LIVE container (12345/54321)\n");

            /* hand-computed rate slot0 (FUN_0086da39 math):
             * j=2(latest): (snap3000-prev1000)+cur2000 = 4000 -> acc += 4.0, sum+=cont2[0]=2000
             * j=1:          cur2000                  -> acc += 2.0, sum+=cont1[0]=1000
             * j=0:          cur2000                  -> acc += 2.0, sum+=cont0[0]=1000
             * acc = 8.0 < 60 window;  rate = sum(4000) * (1.0/8.0) = 500.0  */
            float r0 = rate_for(0);
            if (r0 != 500.0f) {
                printf("FAIL: rate_for(0) expected 500.0, got %g\n", (double)r0);
                fail++;
            } else printf("PASS: rate_for(0) == 500.0 (FUN_0086da39 math reproduced)\n");

            observer_sample();                 /* read+log only -> safe */
            printf("PASS: g_res/g_inc set, observer sample returned (no crash)\n");
        }
        VirtualFree(p, 0, MEM_RELEASE);
    }

    printf(fail ? "FAILURES: %d\n" : "ALL NULL-SAFETY CHECKS PASSED\n", fail);
    return fail ? 1 : 0;
}