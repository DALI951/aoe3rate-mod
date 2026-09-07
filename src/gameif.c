/* gameif.c — game-context walk, slot decrypt, chain log, observer, rate_for
 *
 * Verbatim behavior from R13 (proven live), with rate_for production-ready.
 * All derefs guarded by VirtualQuery via safe_r32/read32.
 */
#include "state.h"

/* ---- diagnostic log (open/flush/close for crash safety) ---- */
void dlog(const char *fmt, ...) {
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

/* ---- crash / fault ---- */
void set_step(const char *s) {
    size_t n = (s != NULL) ? strlen(s) : 0;
    if (n >= sizeof(g_step)) n = sizeof(g_step) - 1;
    memcpy((void *)g_step, (s != NULL) ? s : "", n);
    g_step[n] = '\0';
}

/* Direct file append + flush via the Win32 API: usable even if the CRT heap
 * is corrupted by the crash. Same path/format as dlog ("d3d9mod.log"). */
static void fault_write_raw(const char *line) {
    char path[MAX_PATH];
    if (GetModuleFileNameA(NULL, path, MAX_PATH) == 0) return;
    char *slash = strrchr(path, '\\');
    if (slash != NULL) { slash[1] = '\0'; lstrcatA(path, "d3d9mod.log"); }
    else lstrcpyA(path, "d3d9mod.log");
    HANDLE h = CreateFileA(path, FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(h, line, (DWORD)strlen(line), &w, NULL);
    FlushFileBuffers(h);
    CloseHandle(h);
}

/* Exception codes the game OWNS and must be allowed to survive:
 *  - any success/informational/warning severity (top 2 bits != 11): covers the
 *    OutputDebugString notification 0x406D1388, DbgPrint 0x40010006/0x40010007,
 *    and the whole DBG_* / 0x40000000 family,
 *  - 0x80000003 breakpoint and 0x80000004 single-step (deliberate, debuggers),
 *  - 0xE06D7363 MSVC C++ exception thrown across module boundaries (the game's
 *    normal throw/catch flow).
 * Fatal severity codes (0xC0000005 AV etc.) are NOT benign. */
int fault_code_benign(DWORD code) {
    if ((code & 0xC0000000u) != 0xC0000000u) return 1;
    if (code == 0x80000003u || code == 0x80000004u) return 1;
    if (code == 0xE06D7363u) return 1;
    return 0;
}

static void fault_log_points(struct _EXCEPTION_POINTERS *ep) {
    DWORD addr = 0, code = 0;
    DWORD eip = 0, esp = 0, ebp = 0;
    DWORD stk[8];
    int nstk = 0;
    if (ep != NULL && ep->ExceptionRecord != NULL) {
        addr = (DWORD)(DWORD_PTR)ep->ExceptionRecord->ExceptionAddress;
        code = ep->ExceptionRecord->ExceptionCode;
    }
    if (ep != NULL && ep->ContextRecord != NULL) {
        eip = ep->ContextRecord->Eip;
        esp = ep->ContextRecord->Esp;
        ebp = ep->ContextRecord->Ebp;
        if (esp != 0) {
            MEMORY_BASIC_INFORMATION mbi;
            if (VirtualQuery((const void *)(DWORD_PTR)esp, &mbi, sizeof(mbi))
                    && mbi.State == MEM_COMMIT
                    && (mbi.Protect & (PAGE_READWRITE | PAGE_READONLY |
                        PAGE_EXECUTE | PAGE_EXECUTE_READ |
                        PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY |
                        PAGE_EXECUTE_WRITECOPY))
                    && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
                const DWORD *p = (const DWORD *)(DWORD_PTR)esp;
                for (nstk = 0; nstk < 8; nstk++) stk[nstk] = p[nstk];
            }
        }
    }
    char line[320];
    int ln = _snprintf(line, sizeof(line),
        "FAULT addr=%08X eip=%08X esp=%08X ebp=%08X stk=", 
        (unsigned)addr, (unsigned)eip, (unsigned)esp, (unsigned)ebp);
    if (nstk > 0) {
        for (int i = 0; i < nstk && ln > 0 && ln < (int)sizeof(line); i++)
            ln += _snprintf(line + ln, sizeof(line) - ln, "%s%08X", i ? "," : "", (unsigned)stk[i]);
    } else {
        ln += _snprintf(line + ln, sizeof(line) - ln, "-");
    }
    _snprintf(line + ln, sizeof(line) - ln,
        " breadcrumb=%s code=%08X dev=%08X vt=%08X slot=%d\r\n",
        (const char *)g_step, (unsigned)code,
        (unsigned)(DWORD_PTR)g_fault_dev, (unsigned)(DWORD_PTR)g_fault_vt,
        g_fault_slot);
    fault_write_raw(line);   /* Win32 direct: survives heap/CRT damage */
    dlog("FAULT addr=%08X eip=%08X esp=%08X ebp=%08X stk=%d breadcrumb=%s code=%08X dev=%08X vt=%08X slot=%d",
         (unsigned)addr, (unsigned)eip, (unsigned)esp, (unsigned)ebp, nstk,
         (const char *)g_step, (unsigned)code,
         (unsigned)(DWORD_PTR)g_fault_dev, (unsigned)(DWORD_PTR)g_fault_vt,
         g_fault_slot);
}

/* Vectored handler (registered AddVectoredExceptionHandler(0, ...)): cannot be
 * replaced by the game, so it ALWAYS gives us the breadcrumb of the dying call.
 * LOG-ONLY — it must never ExitProcess: a first-chance/benign exception may be
 * owned by the game's own __try/__except and must be allowed to propagate. */
long WINAPI vectored_fault_filter(struct _EXCEPTION_POINTERS *ep) {
    if (ep != NULL && ep->ExceptionRecord != NULL &&
        !fault_code_benign(ep->ExceptionRecord->ExceptionCode)) {
        fault_log_points(ep);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* Legacy unhandled filter (SetUnhandledExceptionFilter, re-pinned every present
 * frame): reached only for a genuinely UNHANDLED exception. Fatal -> log the
 * FAULT line and silent ExitProcess (no Windows crash dialog), R6 semantics.
 * Benign code defensively ignored here too. */
long WINAPI fault_filter(struct _EXCEPTION_POINTERS *ep) {
    if (ep == NULL || ep->ExceptionRecord == NULL) {
        ExitProcess(1);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (fault_code_benign(ep->ExceptionRecord->ExceptionCode))
        return EXCEPTION_CONTINUE_SEARCH;
    fault_log_points(ep);
    ExitProcess(ep->ExceptionRecord->ExceptionCode ?
                ep->ExceptionRecord->ExceptionCode : 1);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* The game (or another DLL) may replace SetUnhandledExceptionFilter after we
 * install it at DllMain. Re-pin it every present frame so a genuinely fatal
 * unhandled crash ALWAYS gets our FAULT line + silent exit. */
void ensure_fault_filter(void) {
    if (s_prev_filter == NULL)
        s_prev_filter = SetUnhandledExceptionFilter(fault_filter);
    else
        SetUnhandledExceptionFilter(fault_filter);
}

/* ---- guarded reads ---- */
BOOL page_readable(DWORD addr) {
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

BOOL read32(DWORD addr, DWORD *out) {
    if (addr == 0) return FALSE;
    if (!page_readable(addr)) return FALSE;
    *out = *(volatile DWORD *)(DWORD_PTR)addr;
    return TRUE;
}

DWORD safe_r32(DWORD addr) { DWORD v = 0; read32(addr, &v); return v; }

#ifdef SWARM_TEST
float safe_rf(DWORD addr)  { DWORD v = 0; read32(addr, &v); float f = 0.0f; memcpy(&f, &v, sizeof(f)); return f; }
void *safe_rptr(DWORD addr){ DWORD v = 0; if (!read32(addr, &v)) return NULL;
                              return (void *)v; }
#endif

/* ---- decrypt ---- */
float decrypt_slot_at(DWORD rbase, int slot) {
    DWORD base  = g_base ? (DWORD)(DWORD_PTR)g_base : 0;
    DWORD count = base ? safe_r32(base + RVA_SLOT_COUNT) : 0;
    if (count == 0 || count > MAX_SLOTS) count = MAX_SLOTS;
    if (base == 0 || rbase == 0 || slot < 0 || (DWORD)slot >= count) return 0.0f;
    DWORD key = safe_r32(base + RVA_KEY_TABLE + (DWORD)slot * 4);
    DWORD enc = safe_r32(rbase + (DWORD)slot * 4);
    DWORD dec = enc ^ key;
    float f = 0.0f;
    memcpy(&f, &dec, sizeof(f));
    return f;
}

#ifdef SWARM_TEST
float decrypt_slot(int slot) {
    return decrypt_slot_at(g_res ? (DWORD)(DWORD_PTR)g_res : 0, slot);
}
#endif

/* ---- chain log (compact, first-5 + on-change quiet gate) ---- */
static unsigned long g_chain_calls = 0;
static DWORD g_last_c[3];
static char  g_last_tag[128];

void dlog_chain(DWORD game, DWORD ctx, int n,
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
    if (g_settings.debug_enabled)
        dlog("chain game=%08X ctx=%08X n=%d player=%08X res=%08X inc=%08X%s%s",
             (unsigned)game, (unsigned)ctx, n,
             (unsigned)player, (unsigned)res, (unsigned)inc,
             tag ? " " : "", tag ? tag : "");
}

/* ---- resource location ---- */
void locate_resources_impl(void) {
    g_res = NULL;
    g_inc = NULL;
    g_ctx = NULL;
    g_player_idx = -1;
    g_obs_player = 0;
    if (g_base == NULL) return;
    DWORD base = (DWORD)(DWORD_PTR)g_base;
    DWORD game = safe_r32(base + RVA_GAME_PTR);
    DWORD ctx = 0, arr = 0;
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

void locate_resources(void) {
    g_base = (void *)GetModuleHandleA("age3y.exe");
    locate_resources_impl();
}

#ifdef SWARM_TEST
void locate_resources_at(void *base) {
    g_base = base;
    locate_resources_impl();
}
#endif

/* ---- FUN_0086da39 reproduction (rate_for) ---- */
#ifdef SWARM_TEST
float rate_for(int slot) {
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
                float fv = 0.0f;
                memcpy(&fv, &dec, sizeof(fv));
                sum += fv;
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

/* ---- helper ---- */
static int rnd_i(float x) {
    return (x >= 0.0f) ? (int)(x + 0.5f) : (int)(x - 0.5f);
}

/* ---- R20 export line formatter ----
 * Writes the exact recurring format
 *   "t=%lu,food=%d,wood=%d,coin=%d,export=%d"
 * t_ms is the verified realtime millisecond clock (clock_now(): QPC->ms,
 * GetTickCount fallback); values rounded with (int)(x+0.5f). Returns chars
 * written, 0 on NULL/0-size. Never writes past len. */
int format_export_line(unsigned long t_ms, float food, float wood, float coin,
                       float export, char *buf, size_t len) {
    if (buf == NULL || len == 0) return 0;
    buf[0] = '\0';
    int n = _snprintf(buf, len, "t=%lu,food=%d,wood=%d,coin=%d,export=%d",
                      t_ms,
                      (int)(food + 0.5f), (int)(wood + 0.5f),
                      (int)(coin + 0.5f), (int)(export + 0.5f));
    if (n < 0 || (size_t)n >= len) {
        buf[len - 1] = '\0';
        if (n < 0) n = 0;
        else if ((size_t)n >= len) n = (int)(len - 1);
    }
    return n;
}

/* ---- observer ---- */
void observer_sample(void) {
    DWORD ctx   = g_ctx ? (DWORD)(DWORD_PTR)g_ctx : 0;
    int   n     = ctx ? (int)safe_r32(ctx + OFF_CTX_PLAYERCNT) : 0;
    int   chain_ok = (ctx != 0) && (n > 0) &&
                     (g_res != NULL) && (g_inc != NULL);

    /* match-start: only on a VALID chain — loading/menu (n==0 or broken
     * chain) never emits the separator or resets the engine. */
    if (chain_ok) {
        int match_start = (s_last_ctx == 0) ||
                          (g_obs_player != 0 && g_obs_player != s_last_player) ||
                          (n != s_last_n);
        if (match_start) {
            g_match_active = 1;   /* R15: from the first valid frame on, values/res
                                     gate failures are REAL (menu suppression lifts) */
            if (g_settings.debug_enabled)
                dlog("--- match start n=%d player=%08X res=%08X ---",
                     n,
                     (unsigned)g_obs_player,
                     (unsigned)(DWORD_PTR)g_res);
            s_snap_have = 0;
            s_tick = 0;
            tracker_reset();
        }
        s_last_ctx = ctx;
        s_last_player = g_obs_player;
        s_last_n = n;
    } else {
        g_match_active = 0;   /* R15: chain gone (menu/loading) -> values/res gate
                                 bursts are legal again and stop being counted */
    }

    s_tick++;

    if (g_res == NULL || g_inc == NULL) return;

    DWORD rr = (DWORD)(DWORD_PTR)g_res;
    float v[8];
    for (int s = 0; s < 8; s++)
        v[s] = decrypt_slot_at(rr, s);

    /* feed rate engine */
    tracker_sample((DWORD)s_tick, v, 8);

    if (g_settings.debug_enabled) {
        logger_debug("rates food=%.1f wood=%.1f coin=%.1f export=%.1f paused=%d",
                     rate_get_ema(2), rate_get_ema(1), rate_get_ema(0),
                     rate_get_ema(7), g_paused);
        /* R13 output: one RES line per tick, gated by DebugEnabled (R16 A3):
         * the per-frame RES stream was writing ~1 line per frame into live
         * games. Debug=0 keeps the log quiet (chain matches + errors only);
         * Debug=1 restores it for calibration. */
        dlog("RES t=%d food=%d wood=%d coin=%d export=%d player=%08X res=%08X",
             s_tick,
             rnd_i(v[2]), rnd_i(v[1]), rnd_i(v[0]), rnd_i(v[7]),
             g_obs_player ? (unsigned)g_obs_player : 0u,
             (unsigned)rr);
    }
}

/* ---- R23: thread-side resource resolver (NO observer dependency) ----
 * The export thread MUST NOT depend on the globals the observer resolved at
 * match start (g_res/g_inc/g_ctx). Those are only refreshed from the present
 * hook, which is DEAD in-match, so they hold a stale/one-shot/reallocated
 * pointer whose decrypt reads come back 0 forever. This resolver re-walks
 * the full verified chain FRESH on every call (game=*(base+RVA_GAME_PTR),
 * ctx=*(game+OFF_GAME_CTX), n=*(ctx+OFF_CTX_PLAYERCNT), arr=*(ctx+
 * OFF_CTX_PLAYERS)) and never writes any shared global. Guarded like
 * locate_resources_impl (safe_r32 chain, NULL-safe throughout).
 *
 * Player pick for MP+SP: iterate i in 0..n-1, take p=*(arr+i*4),
 * r=*(p+OFF_PLAYER_RES), ic=*(p+OFF_PLAYER_INCOME), skip any 0; total =
 * decrypt_slot_at(r,2)+decrypt_slot_at(r,1)+decrypt_slot_at(r,0)+
 * decrypt_slot_at(r,7) (decrypt_slot_at itself unchanged); pick the
 * candidate with the LARGEST total (> previous best). The candidate list
 * (every (i,total)) and n are exposed through g_export_cands/g_export_n so
 * the caller can log once on change. Returns 1 + *out_res/*out_idx on a sane
 * pick, 0 (no write) when no candidate is usable (all zero/garbage). */
typedef struct {
    int   idx;
    float total;
} ExportCand;
#define EXPORT_MAX_CANDS 32
static ExportCand g_export_cands[EXPORT_MAX_CANDS];
static int        g_export_ncand = 0;   /* candidates filled this call */
static int        g_export_n = 0;       /* player count seen this call */

int resolve_export_player(DWORD base, int limit, DWORD *out_res, int *out_idx) {
    g_export_ncand = 0;
    g_export_n = 0;
    if (out_res) *out_res = 0;
    if (out_idx) *out_idx = -1;
    if (base == 0) return 0;

    DWORD game = safe_r32(base + RVA_GAME_PTR);
    if (game == 0) return 0;
    DWORD ctx = safe_r32(game + OFF_GAME_CTX);
    if (ctx == 0) return 0;

    int n = (int)safe_r32(ctx + OFF_CTX_PLAYERCNT);
    g_export_n = n;
    if (n <= 0) return 0;
    if (limit > 0 && n > limit) n = limit;

    DWORD arr = safe_r32(ctx + OFF_CTX_PLAYERS);
    if (arr == 0) return 0;

    int   pick_i = -1;
    DWORD pick_r = 0;
    float best = -1.0f;
    for (int i = 0; i < n; i++) {
        DWORD p = safe_r32(arr + (DWORD)i * 4);
        if (p == 0) continue;
        DWORD r = safe_r32(p + OFF_PLAYER_RES);
        DWORD ic = safe_r32(p + OFF_PLAYER_INCOME);
        if (r == 0 || ic == 0) continue;
        float total = decrypt_slot_at(r, 2) + decrypt_slot_at(r, 1) +
                      decrypt_slot_at(r, 0) + decrypt_slot_at(r, 7);
        if (g_export_ncand < EXPORT_MAX_CANDS) {
            g_export_cands[g_export_ncand].idx = i;
            g_export_cands[g_export_ncand].total = total;
            g_export_ncand++;
        }
        if (total > best) {
            best = total;
            pick_i = i;
            pick_r = r;
        }
    }
    if (pick_i < 0 || pick_r == 0) return 0;
    if (out_res) *out_res = pick_r;
    if (out_idx) *out_idx = pick_i;
    return 1;
}

/* ---- R21: export thread ----
 * Background thread that tails the verified resource chain and writes the
 * live exported values to rates.log (R21: the DLL's own pipe, read by
 * Dali's config/log_tailer/parser pipeline) in the exact recurring format
 * `t=%lu,food=%d,wood=%d,coin=%d,export=%d` (one line ~every 500ms).
 * t comes from clock_now() — the verified realtime QPC->ms clock (GetTickCount
 * fallback) already used across the rate engine. Launched lazily from the
 * FIRST Direct3DCreate9 call (never DllMain — avoids loader lock), only when
 * [Debug] Enabled=0 (export mode). Shutdown via s_export_running=0 from
 * DLL_PROCESS_DETACH + WaitForSingleObject. d3d9mod.log stays the DEBUG
 * diagnostics log (Debug=1 only) — the export pipe is now rates.log. */
static int s_exp_last_idx = -1;   /* R23: previous pick index for one-shot diag */
static int s_exp_last_n   = -1;   /* R23: previous player count for one-shot diag */

static DWORD WINAPI export_thread(LPVOID param) {
    (void)param;
    char logpath[MAX_PATH];
    if (GetModuleFileNameA(NULL, logpath, MAX_PATH) == 0)
        return 0;
    char *slash = strrchr(logpath, '\\');
    if (slash != NULL) { slash[1] = '\0'; lstrcatA(logpath, "rates.log"); }
    else lstrcpyA(logpath, "rates.log");

    /* truncate once at thread start (fresh session) */
    {
        FILE *f = fopen(logpath, "w");
        if (f != NULL) fclose(f);
    }

    while (s_export_running) {
        Sleep(500);
        unsigned long t = (unsigned long)clock_now();
        /* R23: resolve the player FRESH each iteration — never trust the
         * observer-resolved g_res (stale in-match, zeros forever). */
        DWORD base = g_base ? (DWORD)(DWORD_PTR)g_base : 0;
        DWORD res = 0;
        int   idx = -1;
        if (base == 0) continue;
        if (!resolve_export_player(base, EXPORT_MAX_CANDS, &res, &idx))
            continue;   /* no sane player -> no zero line */
        if (res == 0) continue;

        /* R23 who-is-who diagnostic: ONE line ONLY when the pick (index or
         * player count) changes — rare, tells us which index Dali IS. */
        if (idx != s_exp_last_idx || g_export_n != s_exp_last_n) {
            char dbg[256];
            int ln = _snprintf(dbg, sizeof(dbg), "R23 pick: n=%d idx=%d cand=[",
                               g_export_n, idx);
            for (int k = 0; k < g_export_ncand && ln > 0 && ln < (int)sizeof(dbg); k++)
                ln += _snprintf(dbg + ln, sizeof(dbg) - ln, "%s%d:%.0f",
                                k ? " " : "", g_export_cands[k].idx,
                                g_export_cands[k].total);
            if (ln > 0 && ln < (int)sizeof(dbg))
                ln += _snprintf(dbg + ln, sizeof(dbg) - ln, "]");
            dlog("%s", dbg);
            s_exp_last_idx = idx;
            s_exp_last_n = g_export_n;
        }

        float food   = decrypt_slot_at(res, 2);
        float wood   = decrypt_slot_at(res, 1);
        float coin   = decrypt_slot_at(res, 0);
        float export = decrypt_slot_at(res, 7);
        char buf[64];
        int n = format_export_line(t, food, wood, coin, export, buf, sizeof(buf));
        if (n <= 0) continue;
        FILE *f = fopen(logpath, "a");
        if (f == NULL) continue;
        fwrite(buf, 1, (size_t)n, f);
        fputc('\n', f);
        fflush(f);
        fclose(f);
    }
    return 0;
}

int export_start(void) {
    if (s_export_running) return 0;   /* already running (once-only) */
    if (g_settings.debug_enabled) return 0;   /* export mode ONLY when debug off */
    s_export_running = 1;
    s_export_thread = CreateThread(NULL, 0, export_thread, NULL, 0, NULL);
    if (s_export_thread == NULL) {
        s_export_running = 0;
        return 0;
    }
    return 1;
}

void export_shutdown(void) {
    s_export_running = 0;
    if (s_export_thread != NULL) {
        HANDLE h = s_export_thread;
        s_export_thread = NULL;
        WaitForSingleObject(h, 200);
        CloseHandle(h);
    }
}
