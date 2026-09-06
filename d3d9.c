/*
 * d3d9.c — AoE3 TAD resource-rate HUD mod (d3d9 proxy DLL), 32-bit i386.
 *
 * ROUND 13: single-player observer. Human = player index 1 when n>=2 (fallback
 * to largest-food scan). Output: one RES line per Present tick for the chosen
 * player. Slot map: food=slot2, wood=slot1, coin=slot0, export=slot7 (empirical
 * proof from R12 live: HUD 530/100/130 vs logged food=530 wood=130 coin=100
 * proved slots 0 and 1 were SWAPPED in R12 labels). Zero rendering, zero device
 * state changes — pure read-only observer. KERNEL32+msvcrt only.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <malloc.h>

typedef DWORD D3DCOLOR;

/* ------------------------- addressing ------------------------------------ */
#define RVA_GAME_PTR           0x00866234u
#define OFF_GAME_CTX           0x13c
#define OFF_GAME_ACTIVE_PLAYER 0x14c
#define OFF_CTX_PLAYERS        0x58
#define OFF_CTX_PLAYERCNT      0x5c
#define OFF_PLAYER_RES         0x230
#define OFF_PLAYER_RES_CAP     0x238
#define OFF_PLAYER_RES_ADD     0x240
#define OFF_PLAYER_INCOME      0x80
#define RVA_KEY_TABLE     0x0086DF14u
#define RVA_SLOT_COUNT    0x0086DF38u
#define MAX_SLOTS         8u

/* income object fields (SWARM_TEST: rate_for) */
#define OFF_INC_CUR       0x160
#define OFF_INC_PREV      0x164
#define OFF_INC_HIST      0x168
#define OFF_INC_COUNT     0x16c
#define OFF_CTX_SNAP      0x108
#define RVA_TIME_STEP     0x0079A5C8u
#define RVA_NEG_FUDGE     0x007A12FCu
#define RVA_WIN_THRESH    0x007A12E8u
#define RVA_DIV_CONST     0x007856BCu
#define RATE_WINDOW       60.0f
#define MAX_HIST          1024u

/* ------------------------------ globals ---------------------------------- */
static HMODULE g_real   = NULL;
static void   *g_base   = NULL;
static void   *g_ctx    = NULL;
static void   *g_res    = NULL;
static void   *g_inc    = NULL;
static void   *g_device = NULL;
static void  **g_devvt  = NULL;

static int   g_player_idx = -1;
static int   s_snap_have = 0;

static DWORD g_obs_player = 0;
static DWORD s_last_ctx = 0;
static DWORD s_last_player = 0;
static int   s_last_n = -1;
static int   s_tick = 0;

/* --------------------------- diagnostic log ------------------------------- */
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

/* ------------------------- crash catch ----------------------------------- */
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
    ExitProcess(0);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* ------------------------- guarded reads --------------------------------- */
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
    if (!page_readable(addr)) return FALSE;
    *out = *(volatile DWORD *)(DWORD_PTR)addr;
    return TRUE;
}
static DWORD safe_r32(DWORD addr) { DWORD v = 0; read32(addr, &v); return v; }
#ifdef SWARM_TEST
static float safe_rf(DWORD addr)  { DWORD v = 0; read32(addr, &v); return *(float *)&v; }
static void *safe_rptr(DWORD addr){ DWORD v = 0; if (!read32(addr, &v)) return NULL;
                                    return (void *)v; }
#endif

/* --------------------------- decrypt ------------------------------------- */
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
static float decrypt_slot(int slot) {
    return decrypt_slot_at(g_res ? (DWORD)(DWORD_PTR)g_res : 0, slot);
}
#endif

/* ------------------------- chain log (compact) --------------------------- */
static unsigned long g_chain_calls = 0;
static DWORD g_last_c[3];
static char  g_last_tag[128];

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
    if (g_chain_calls > 5 && !changed) return;
    memcpy(g_last_c, cur, sizeof(cur));
    g_last_tag[0] = '\0';
    if (tag != NULL) strncpy(g_last_tag, tag, sizeof(g_last_tag) - 1);
    dlog("chain game=%08X ctx=%08X n=%d player=%08X res=%08X inc=%08X%s%s",
         (unsigned)game, (unsigned)ctx, n,
         (unsigned)player, (unsigned)res, (unsigned)inc,
         tag ? " " : "", tag ? tag : "");
}

/* ------------------------- resource location ------------------------------ */
/* R13: human = player index 1 when n>=2 (confirmed by R12 live analysis:
 * index 1 gathered food 0->530 across two matches while index 2 = AI
 * trickled 0->55). Fallback: scan all players, pick the one with the
 * largest decrypted food (slot 2). If no valid player found, log zeros. */
static void locate_resources_impl(void) {
    g_res = NULL;
    g_inc = NULL;
    g_ctx = NULL;
    g_player_idx = -1;
    g_obs_player = 0;
    if (g_base == NULL) return;
    DWORD base     = (DWORD)(DWORD_PTR)g_base;
    DWORD game     = safe_r32(base + RVA_GAME_PTR);
    DWORD ctx = 0, arr = 0, player = 0, res = 0, inc = 0;
    int   n = 0;

    if (game == 0) { dlog_chain(0, 0, 0, 0, 0, 0, "[chain-break: game==0]"); return; }
    ctx = safe_r32(game + OFF_GAME_CTX);
    if (ctx == 0) { dlog_chain(game, 0, 0, 0, 0, 0, "[chain-break: ctx==0]"); return; }
    g_ctx = (void *)ctx;
    n   = (int)safe_r32(ctx + OFF_CTX_PLAYERCNT);
    arr = safe_r32(ctx + OFF_CTX_PLAYERS);
    if (n <= 0) { dlog_chain(game, ctx, n, 0, 0, 0, "[chain-break: n<=0]"); return; }
    if (arr == 0) { dlog_chain(game, ctx, n, 0, 0, 0, "[chain-break: arr==0]"); return; }

    /* primary: human = index 1 when n >= 2 */
    if (n >= 2) {
        DWORD p1 = safe_r32(arr + 1 * 4);
        if (p1 != 0) {
            DWORD r1 = safe_r32(p1 + OFF_PLAYER_RES);
            DWORD i1 = safe_r32(p1 + OFF_PLAYER_INCOME);
            if (r1 != 0 && i1 != 0) {
                g_res = (void *)r1;
                g_inc = (void *)i1;
                g_player_idx = 1;
                g_obs_player = p1;
                dlog_chain(game, ctx, n, p1, r1, i1, "[human-p1]");
                return;
            }
        }
    }

    /* fallback: scan all players, pick largest decrypted food (slot 2) */
    {
        int pick_i = -1;
        DWORD pick = 0, pick_r = 0, pick_i2 = 0;
        float best_food = -1.0f;
        for (int i = 0; i < n; i++) {
            DWORD p = safe_r32(arr + (DWORD)i * 4);
            if (p == 0) continue;
            DWORD r = safe_r32(p + OFF_PLAYER_RES);
            DWORD ic = safe_r32(p + OFF_PLAYER_INCOME);
            if (r == 0 || ic == 0) continue;
            float food = decrypt_slot_at(r, 2);
            if (food > best_food) {
                best_food = food;
                pick = p; pick_i = i;
                pick_r = r; pick_i2 = ic;
            }
        }
        if (pick != 0) {
            g_res = (void *)pick_r;
            g_inc = (void *)pick_i2;
            g_player_idx = pick_i;
            g_obs_player = pick;
            char tag[80];
            _snprintf(tag, sizeof(tag), "[fallback: player#%d food=%.0f]", pick_i, best_food);
            dlog_chain(game, ctx, n, pick, pick_r, pick_i2, tag);
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
static void locate_resources_at(void *base) {
    g_base = base;
    locate_resources_impl();
}
#endif

/* ----------------------- rate fields (SWARM_TEST only) ------------------- */
#ifdef SWARM_TEST
static float rate_for(int slot) {
    DWORD base = g_base ? (DWORD)(DWORD_PTR)g_base : 0;
    if (base == 0 || slot < 0 || (DWORD)slot >= MAX_SLOTS) return 0.0f;
    DWORD inc = g_inc ? (DWORD)(DWORD_PTR)g_inc : 0;
    if (inc == 0) return 0.0f;

    int count = (int)safe_r32(inc + OFF_INC_COUNT);
    if (count < 1) return 0.0f;

    float window = RATE_WINDOW;
    float thr = safe_rf(base + RVA_WIN_THRESH);
    if (!(window >= thr)) return 0.0f;

    float tstep = safe_rf(base + RVA_TIME_STEP);
    if (!(tstep >= 0.0f) || !(tstep <= 1e3f)) tstep = 0.001f;
    float fudge = safe_rf(base + RVA_NEG_FUDGE);
    float divc  = safe_rf(base + RVA_DIV_CONST);
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
            DWORD cont = (DWORD)(DWORD_PTR)safe_rptr(hist + (DWORD)j * 4);
            if (cont != 0) {
                DWORD key = safe_r32(base + RVA_KEY_TABLE + (DWORD)slot * 4);
                DWORD enc = safe_r32(cont + (DWORD)slot * 4);
                DWORD dec = enc ^ key;
                sum += *(float *)&dec;
            }
        }
        j--;
    }
    if (!(acc > 0.0f)) return 0.0f;
    float rate = sum * (divc / acc);
    if (!(rate <= 1e7f) || !(rate >= -1e7f)) rate = 0.0f;
    return rate;
}
#endif

/* ----------------------------- observer ---------------------------------- */
static int rnd_i(float x) {
    return (x >= 0.0f) ? (int)(x + 0.5f) : (int)(x - 0.5f);
}

static void observer_sample(void) {
    DWORD base  = g_base ? (DWORD)(DWORD_PTR)g_base : 0;
    DWORD game  = base ? safe_r32(base + RVA_GAME_PTR) : 0;
    DWORD ctx   = g_ctx ? (DWORD)(DWORD_PTR)g_ctx : 0;
    int   n     = ctx ? (int)safe_r32(ctx + OFF_CTX_PLAYERCNT) : 0;

    /* match-start detection: ctx 0->non-0, player change, or n change */
    int match_start = ((s_last_ctx == 0) && (ctx != 0)) ||
                      (g_obs_player != 0 && g_obs_player != s_last_player) ||
                      (n != s_last_n);
    if (match_start) {
        dlog("--- match start n=%d player=%08X res=%08X ---",
             n,
             g_obs_player ? (unsigned)g_obs_player : 0u,
             g_res ? (unsigned)(DWORD_PTR)g_res : 0u);
        s_last_ctx = ctx;
        s_last_player = g_obs_player;
        s_last_n = n;
        s_snap_have = 0;
        s_tick = 0;
    }

    s_tick++;

    if (g_res == NULL || g_inc == NULL) return;

    DWORD rr = (DWORD)(DWORD_PTR)g_res;
    float v[8];
    for (int s = 0; s < 8; s++)
        v[s] = decrypt_slot_at(rr, s);

    /* R13 output: one RES line per tick for the chosen human player.
     * Slot map: food=slot2, wood=slot1, coin=slot0, export=slot7 */
    dlog("RES t=%d food=%d wood=%d coin=%d export=%d player=%08X res=%08X",
         s_tick,
         rnd_i(v[2]), rnd_i(v[1]), rnd_i(v[0]), rnd_i(v[7]),
         g_obs_player ? (unsigned)g_obs_player : 0u,
         (unsigned)rr);
}

/* -------------------------- device vtable hook ---------------------------- */
typedef int  (STDMETHODCALLTYPE *PRESENT_FN)(void *self, const RECT *a,
        const RECT *b, HWND hwnd, const void *dirty);
typedef int  (STDMETHODCALLTYPE *DEV_RESET)(void *, const void *);
static PRESENT_FN s_orig_present = NULL;
static DEV_RESET   s_orig_reset   = NULL;
static int patch_device_present(void *dev);

static int STDMETHODCALLTYPE reset_hook(void *self, const void *pp) {
    g_device = self;
    int hr = (s_orig_reset != NULL) ? s_orig_reset(self, pp) : (int)0x8876086c;
    patch_device_present(self);
    return hr;
}

static int STDMETHODCALLTYPE present_hook(void *self, const RECT *a, const RECT *b,
        HWND hwnd, const void *dirty) {
    g_device = self;
    locate_resources();
    set_step("observer-sample");
    observer_sample();
    set_step("present-before");
    int hr = (s_orig_present != NULL)
        ? s_orig_present(self, a, b, hwnd, dirty)
        : (int)0x8876086c;
    set_step("present-after");
    return hr;
}

static int patch_device_present(void *dev) {
    if (dev == NULL) return 0;
    void **vt = *(void ***)dev;
    if (vt == NULL) return 0;
    if (g_devvt != vt) {
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
    return (g_devvt == vt && s_orig_present != NULL);
}

/* ------------------------ IDirect3D9 COM wrapper --------------------------- */
struct d3d9w {
    void **vt;
    void  *real;
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
        if (s_prev_filter == NULL)
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
