/*
 * test_rate_engine.c ??? R14 rate-engine + settings + version-gate harness.
 * Compiles the REAL d3d9.c (via SWARM_TEST include) and drives the actual
 * production functions: tracker_sample(), rate_get_ema/raw/display(),
 * settings_init/load/save(), ui_* no-crash, observer path, version gate.
 *
 * Rate engine rules under test (deterministic override clock, R16 A1):
 *   - first sample bootstraps (no fake rate), cadence honored (500 ms default)
 *   - steady gain:  +1 / 500 ms  => EMA converges to 2.0/s (120.0/min)
 *   - spending spike: inst < 0 and |inst| > EMA*3 => EMA frozen, baseline fresh
 *   - instant gains counted (positive jumps move the EMA strongly)
 *   - ShowGains=0: a positive jump is treated like a spend spike (EMA frozen)
 *   - global pause: clock not advancing => g_paused, EMA untouched
 *   - slot freeze: value frozen across a full interval => EMA frozen
 *   - realtime mode (QPC): finite, sane values, no div-by-zero
 *   - settings INI round-trip + garbage-file resilience
 *   - version gate failure path (host exe is the test, not age3y)
 *
 * The R16 A1 clock change makes the shipped tracker use realtime QPC. The
 * deterministic timeline below therefore pins the clock via
 * tracker_set_clock_override(); the QPC path itself is exercised end-to-end
 * in test_rate_realtime().
 *
 * Build (from tests dir):
 *   i686-w64-mingw32-gcc.exe test_rate_engine.c -o test_rate_engine.exe -luser32 -lwinmm
 */
#define SWARM_TEST
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "..\d3d9.c"

static int failures = 0;

static void putu(DWORD a, DWORD v) { *(volatile DWORD *)a = v; }
static void putf(DWORD a, float f) { DWORD b; memcpy(&b, &f, 4); putu(a, b); }

static void delete_log(void) {
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    char *s = strrchr(path, '\\'); if (s) s[1] = '\0';
    lstrcatA(path, "d3d9mod.log");
    DeleteFileA(path);
}

#define CHECK(cond, msg) do { \
    if (cond) printf("PASS: %s\n", msg); \
    else { printf("FAIL: %s\n", msg); failures++; } \
} while (0)

#define FEQ(a, b) (fabsf((a) - (b)) < 1e-3f)

static void set_vals(float a[8], float v0) {
    for (int i = 0; i < 8; i++) a[i] = (i == 0) ? v0 : 0.0f;
}

/* deterministic clock pin (R16 A1): sample with the s_tick timeline as the
 * clock value, exactly like the old game-time mode did */
static void ts(DWORD tick, float *a) {
    tracker_set_clock_override(tick);
    tracker_sample(tick, a, 8);
}

/* ---- STEP 1: rate engine, game-time ---- */
static void test_rate_game_time(void) {
    g_settings.use_game_time = 1;
    g_settings.use_unit_min = 1;
    g_settings.sample_ms = (int)SAMPLE_MS_DEFAULT;
    lstrcpyA(g_settings.smoothing, "med");
    g_settings.discontinuity_ratio = 3.0f;
    g_settings.show_gains = 1;   /* production default */
    tracker_reset();

    float a[8];
    /* 1st sample: bootstrap, no rate yet */
    s_tick = 500;   set_vals(a, 100.0f);   ts((DWORD)s_tick, a);
    CHECK(rate_get_ema(0) == 0.0f, "bootstrap sample sets no EMA");
    CHECK(g_values_valid == 1, "values valid after first sample");

    /* steady +1 per 500ms => 2.0/s */
    s_tick = 1000;  set_vals(a, 101.0f);   ts((DWORD)s_tick, a);
    CHECK(FEQ(rate_get_ema(0), 2.0f), "steady +1/500ms => EMA 2.0/s");
    CHECK(FEQ(rate_get_raw(0), 2.0f), "raw rate 2.0/s from ring");
    CHECK(FEQ(rate_display(0), 120.0f), "display x60 => 120.0/min");

    s_tick = 1500;  set_vals(a, 102.0f);   ts((DWORD)s_tick, a);
    CHECK(FEQ(rate_get_ema(0), 2.0f), "steady rate keeps EMA at 2.0/s");

    /* cadence is honored: sub-interval frame must not sample */
    s_tick = 1600;  set_vals(a, 102.0f);   ts((DWORD)s_tick, a);
    CHECK(FEQ(rate_get_ema(0), 2.0f), "sub-interval frame does not re-sample");

    /* spending spike: -82 over 500ms => -164/s, |..| > 2*3 => EMA frozen */
    s_tick = 2100;  set_vals(a, 20.0f);    ts((DWORD)s_tick, a);
    CHECK(FEQ(rate_get_ema(0), 2.0f), "spending spike freezes EMA (skip sample)");
    /* ring baseline advances to the post-spike value; the next normal sample
     * measures against it (this is why the resume raw comes out as 10.0/s) */

    /* resume: +5 over 500ms => 10/s, EMA = 2 + 0.2*(10-2) = 3.6 */
    s_tick = 2600;  set_vals(a, 25.0f);    ts((DWORD)s_tick, a);
    CHECK(FEQ(g_last_values[0], 25.0f), "post-spike values continue feeding overlay");
    CHECK(FEQ(rate_get_ema(0), 3.6f), "resume: EMA = 2 + 0.2*(10-2) = 3.6");
    CHECK(FEQ(rate_get_raw(0), 10.0f), "raw rate 10.0/s after resume");

    /* instant gain: +975/500ms => 1950/s positive, counted (ShowGains) */
    s_tick = 3100;  set_vals(a, 1000.0f);  ts((DWORD)s_tick, a);
    CHECK(rate_get_ema(0) > 100.0f, "instant gain counted (EMA jumps up)");

    /* global pause: same tick again */
    float e = rate_get_ema(0);
    s_tick = 3100;  set_vals(a, 1001.0f);  ts((DWORD)s_tick, a);
    CHECK(g_paused == 1, "advancing nothing => paused");
    CHECK(rate_get_ema(0) == e, "paused: EMA untouched");
    s_tick = 3600;  set_vals(a, 1002.0f);  ts((DWORD)s_tick, a);
    CHECK(g_paused == 0, "unpause when clock advances again");

    /* slot freeze: value identical across a full interval */
    e = rate_get_ema(0);
    s_tick = 4100;  set_vals(a, 1002.0f);  ts((DWORD)s_tick, a);
    CHECK(g_slot_frozen[0] == 1, "slot frozen when value flat across interval");
    CHECK(rate_get_ema(0) == e, "flat value freezes EMA (no drift to zero)");
    s_tick = 4600;  set_vals(a, 1003.0f);  ts((DWORD)s_tick, a);
    CHECK(g_slot_frozen[0] == 0, "slot unfreezes on next value change");

    /* ShowGains=0 (R16 B8): a strongly positive jump is skipped like a spend
     * spike — the EMA must NOT jump up. +497 over 500ms => ~994/s, > 3*EMA. */
    float sg0 = rate_get_ema(0);
    g_settings.show_gains = 0;
    s_tick = 5200;  set_vals(a, 1500.0f);  ts((DWORD)s_tick, a);
    CHECK(rate_get_ema(0) == sg0, "ShowGains=0: positive jump does not move EMA");

    /* R9 clamp: a DRAINED slot (EMA exactly 0) with ShowGains=0. The skip
     * reference is clamped to >= 0, so any positive sample is a candidate and
     * the baseline stays fresh instead of the EMA wedging forever at 0; the
     * slot is NOT stuck — with ShowGains=1 the next normal sample resumes. */
    g_last_ema[0] = 0.0f;
    g_ema_valid[0] = 1;
    g_slot_frozen[0] = 0;
    s_tick = 5700;  set_vals(a, 1550.0f);  ts((DWORD)s_tick, a); /* +100/s => skip */
    CHECK(rate_get_ema(0) == 0.0f, "R9: drained slot positive sample skipped (clamped ref), EMA stays 0");
    g_settings.show_gains = 1; /* drained slot must NOT stay wedged once gains count */
    s_tick = 6200;  set_vals(a, 1555.0f);  ts((DWORD)s_tick, a); /* +10/s */
    CHECK(FEQ(rate_get_ema(0), 2.0f), "R9: drained slot resumes normally once gains are counted (0 + 0.2*10 = 2)");
    g_last_ema[0] = sg0;
    g_settings.show_gains = 1;
}

/* ---- STEP 2: realtime mode ---- */
static void test_rate_realtime(void) {
    g_settings.use_game_time = 0;
    g_settings.sample_ms = 100; /* cadence min; sleeps below guarantee due */
    tracker_reset();
    float a[8];
    float e0, e1, e2;
    set_vals(a, 100.0f);  tracker_sample(1, a, 8);
    Sleep(120);
    set_vals(a, 101.0f);  tracker_sample(2, a, 8);
    e0 = rate_get_ema(0);
    Sleep(120);
    set_vals(a, 102.0f);  tracker_sample(3, a, 8);
    e1 = rate_get_ema(0);
    CHECK(e0 > 0.0f && e0 < 1e5f && e1 == e1 && e1 < 1e5f,
          "realtime mode: finite, sane rates (no div-by-zero)");
    CHECK(g_values_valid == 1, "realtime mode: values valid");
    Sleep(120);
    e2 = e1;
    set_vals(a, 102.0f);  tracker_sample(4, a, 8); /* flat => slot freeze */
    CHECK(g_slot_frozen[0] == 1, "realtime mode: flat value freezes slot EMA");
    CHECK(rate_get_ema(0) == e2, "realtime mode: frozen EMA untouched");
    Sleep(120);
    set_vals(a, 103.0f);  tracker_sample(5, a, 8);
    CHECK(g_slot_frozen[0] == 0, "realtime mode: unfreeze on change");
    g_settings.use_game_time = 1;
    g_settings.sample_ms = (int)SAMPLE_MS_DEFAULT;
}

/* ---- STEP 3: settings INI round-trip ---- */
static void test_settings_ini(void) {
    char ini[MAX_PATH];
    GetModuleFileNameA(NULL, ini, MAX_PATH);
    char *s = strrchr(ini, '\\'); if (s) s[1] = '\0';
    lstrcatA(ini, "ResourceRateMod.ini");
    char tmp[MAX_PATH];
    _snprintf(tmp, sizeof(tmp), "%s.tmp", ini);
    DeleteFileA(ini);
    DeleteFileA(tmp);

    settings_init();
    CHECK(g_settings.use_game_time == 1, "default: game-time clock");
    CHECK(g_settings.sample_ms == (int)SAMPLE_MS_DEFAULT, "default: 500ms sample");
    CHECK(g_settings.discontinuity_ratio == 3.0f, "default: discontinuity ratio 3.0");

    lstrcpyA(g_settings.font_name, "Arial");
    g_settings.font_size = 18;
    g_settings.sample_ms = 900;
    g_settings.opacity = 0.37f;
    g_settings.pos_x = 3;
    g_settings.pos_y = 9;
    lstrcpyA(g_settings.smoothing, "high");
    g_settings.use_unit_min = 0;
    g_settings.show_slots_567 = 1;
    g_settings.debug_enabled = 1;
    settings_save();

    /* clobber, then reload */
    memset(&g_settings, 0, sizeof(g_settings));
    lstrcpyA(g_settings.smoothing, "");
    settings_load();
    CHECK(strcmp(g_settings.font_name, "Arial") == 0, "INI round-trip: font_name");
    CHECK(g_settings.font_size == 18, "INI round-trip: font_size");
    CHECK(g_settings.sample_ms == 900, "INI round-trip: sample_ms");
    CHECK(FEQ(g_settings.opacity, 0.37f), "INI round-trip: opacity");
    CHECK(g_settings.pos_x == 3 && g_settings.pos_y == 9, "INI round-trip: pos");
    CHECK(strcmp(g_settings.smoothing, "high") == 0, "INI round-trip: smoothing");
    CHECK(g_settings.use_unit_min == 0, "INI round-trip: unit=sec");
    CHECK(g_settings.use_game_time == 1, "INI round-trip: UseGameTime default kept");
    CHECK(g_settings.show_slots_567 == 1 && g_settings.debug_enabled == 1,
          "INI round-trip: slots567 + debug");
    CHECK(g_settings.discontinuity_ratio == 3.0f, "INI round-trip: ratio default kept");

    /* garbage file must not crash or corrupt */
    FILE *f = fopen(ini, "wb");
    fputs("garbage line\nno=[\nUn=broken", f);
    fclose(f);
    settings_load();
    CHECK(g_settings.sample_ms == 900, "garbage contents ignored (no crash)");

    DeleteFileA(ini);
    DeleteFileA(tmp);
}

/* ---- STEP 4: version gate failure path (host is the test exe) ---- */
static void test_version_gate(void) {
    g_version_ok = 1;
    version_gate_check();
    CHECK(g_version_ok == 0, "version gate fails against the test exe (not age3y)");
    CHECK(strstr(g_version_reason, "size") != NULL, "reason mentions exe size mismatch");

    /* overlay fully disabled when the gate is closed: no crash even with
     * garbage device pointers, because ui_draw returns before touching vt. */
    g_settings.enabled = 1;
    g_panel_visible = 1;
    g_res = (void *)0x1;
    g_device = (void *)0x1;
    ui_draw();
    CHECK(1, "ui_draw no-op on failed gate (graceful)");
    g_device = NULL;
    g_res = NULL;
    g_version_ok = 1;
}

/* ---- STEP 5: observer + UI no-crash on menu (n==0) ---- */
static const unsigned char KEY_BYTES[32] = {
    0x28, 0x48, 0xAC, 0x4F, 0x94, 0xF8, 0x3A, 0x35,
    0x8B, 0xD8, 0x4C, 0x3F, 0xAB, 0x12, 0xFB, 0xAF,
    0x20, 0xB3, 0x5B, 0xCA, 0xF9, 0xAB, 0xC4, 0x2A,
    0xB1, 0xA1, 0xCF, 0xDA, 0xF2, 0xE4, 0x82, 0x10,
};
#define MAX_RVA 0xA00000u

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

static void test_observer_menu(void) {
    LPVOID img = make_fake_image();
    if (!img) { printf("FAIL: alloc (menu)\n"); failures++; return; }
    DWORD b = (DWORD)(DWORD_PTR)img;
    DWORD game = b + 0x1000, ctx = b + 0x2000, players = b + 0x3000;
    putu(b + RVA_GAME_PTR, game);
    putu(game + OFF_GAME_CTX, ctx);
    putu(ctx + OFF_CTX_PLAYERCNT, 0); /* n==0 => menu */
    putu(ctx + OFF_CTX_PLAYERS, players);

    delete_log();
    g_res = g_inc = NULL;
    locate_resources_at(img);
    observer_sample();
    CHECK(g_res == NULL, "menu (n==0): resources stay NULL");
    CHECK(1, "observer no-crash in menu");
    VirtualFree(img, 0, MEM_RELEASE);
}

static void test_observer_rates_line(void) {
    LPVOID img = make_fake_image();
    if (!img) { printf("FAIL: alloc (rates)\n"); failures++; return; }
    DWORD b = (DWORD)(DWORD_PTR)img;
    DWORD game = b + 0x1000, ctx = b + 0x2000, players = b + 0x3000;
    putu(b + RVA_GAME_PTR, game);
    putu(game + OFF_GAME_CTX, ctx);
    putu(ctx + OFF_CTX_SNAP, 3000);
    putu(ctx + OFF_CTX_PLAYERCNT, 2);
    putu(ctx + OFF_CTX_PLAYERS, players);
    for (int i = 0; i < 2; i++) {
        DWORD p = b + 0x4000 + (DWORD)i * 0x1000;
        putu(players + (DWORD)i * 4, p);
        DWORD cont = p + 0x300, inc = p + 0x600;
        set_encrypted_slot(cont, 2, (float)(100 + i * 200), KEY_BYTES); /* food */
        set_encrypted_slot(cont, 0, (float)(50 + i * 50), KEY_BYTES);   /* coin  */
        putu(p + OFF_PLAYER_RES, cont);
        putu(p + OFF_PLAYER_INCOME, inc);
        putu(inc + OFF_INC_CUR, 1); putu(inc + OFF_INC_PREV, 1);
        putu(inc + OFF_INC_COUNT, 1);
    }

    delete_log();
    g_res = g_inc = NULL;
    locate_resources_at(img);
    CHECK(g_res != NULL, "observer: full chain resolves player in game (n=2)");
    g_settings.debug_enabled = 1;
    observer_sample();
    s_tick += 500; /* advance game time so the tracker samples */
    observer_sample();
    CHECK(1, "observer no-crash in game");
    g_settings.debug_enabled = 0;
    VirtualFree(img, 0, MEM_RELEASE);
}

/* ---- STEP 6: UI graceful-degrade guards ---- */
static void test_ui_guards(void) {
    g_settings.enabled = 1;
    g_version_ok = 1;
    g_panel_visible = 1;
    g_device = NULL;
    ui_draw();
    CHECK(1, "ui_draw no-op with no device (no crash)");
    ui_init(); /* d3dx9_25.dll may or may not load; must not crash */
    ui_draw();
    CHECK(1, "ui_draw after init no-op/graceful (no crash)");
    ui_on_reset(NULL); /* font NULL => no-op */
    CHECK(1, "ui_on_reset with no font (no crash)");
    g_settings.hotkey = 0x78; /* F9 ??? GetAsyncKeyState returns 0 headless */
    ui_check_hotkey();
    CHECK(g_panel_visible == 1, "hotkey check headless: no phantom toggle");
}

/* ---- STEP 7: fault-filter semantics (round 3) ----
 * The vectored handler is LOG-ONLY and must return EXCEPTION_CONTINUE_SEARCH
 * for every code the game owns (benign notifications, breakpoints, MSVC++ EH),
 * never ExitProcess. The legacy unhandled filter also refuses benign codes.
 * ExitProcess on a truly-unhandled fatal code is intentionally NOT tested
 * in-process (it would kill this harness); fault_code_benign() covers the
 * decision, and the fatal path is the R6-proven behavior. */
static void test_fault_filters(void) {
    static EXCEPTION_RECORD rec;
    static CONTEXT       ctx;
    EXCEPTION_POINTERS   ep = { &rec, &ctx };
    static const DWORD benign_codes[] = {
        0x406D1388u, /* OutputDebugString notification (d3dx9/game at load) */
        0x40010006u, /* DbgPrint / RIP */
        0x40010007u, /* DbgPrintEx */
        0xE06D7363u, /* MSVC C++ exception */
        0x80000003u, /* breakpoint */
        0x80000004u, /* single-step */
        0x40080201u, /* VCPP EH */
    };
    for (size_t i = 0; i < sizeof(benign_codes) / sizeof(benign_codes[0]); i++) {
        DWORD c = benign_codes[i];
        memset(&rec, 0, sizeof(rec));
        rec.ExceptionCode      = c;
        rec.ExceptionAddress   = (PVOID)(DWORD_PTR)0x1234;
        CHECK(fault_code_benign(c) == 1, "fault_code_benign recognizes a benign code");
        CHECK(vectored_fault_filter(&ep) == EXCEPTION_CONTINUE_SEARCH,
              "vectored filter: benign code -> CONTINUE_SEARCH (no exit)");
        CHECK(fault_filter(&ep) == EXCEPTION_CONTINUE_SEARCH,
              "unhandled filter: benign code -> CONTINUE_SEARCH (no exit)");
    }
    CHECK(fault_code_benign(0xC0000005u) == 0,
          "fault_code_benign: fatal AV 0xC0000005 is NOT benign (fatal path exits)");
    CHECK(fault_code_benign(0xC00000FDu) == 0,
          "fault_code_benign: stack overflow 0xC00000FD is NOT benign");
    CHECK(fault_code_benign(0x80000002u) == 1,
          "fault_code_benign: 0x80000002 (warning severity) is benign");
    CHECK(vectored_fault_filter(NULL) == EXCEPTION_CONTINUE_SEARCH,
          "vectored filter: NULL pointers -> CONTINUE_SEARCH safe");
}

/* ---- R11 STEP: format_rate_line + version table ---- */
static void test_format_line(void) {
    ModSettings def;
    memset(&def, 0, sizeof(def));
    def.use_unit_min = 1;
    def.decimal_places = 1;
    def.show_plus_sign = 1;
    def.show_resource_names = 1;
    def.show_zero_rates = 1;
    def.show_food = 1; def.show_wood = 1; def.show_coin = 1; def.show_export = 1;

    char b[96];

    /* defaults: Food, value 100, +2.0, /min */
    int r = format_rate_line(b, sizeof(b), 2, "Food", 100.0f, 2.0f/60.0f, &def);
    CHECK(r == 1 && strstr(b, "Food") != NULL && strstr(b, "100") != NULL &&
          strstr(b, "+2.0") != NULL && strstr(b, "/min") != NULL,
          "R11: format_rate_line defaults (Food 100 +2.0/min)");

    /* decimal places 0..3, and out-of-range clamp to 3 — R12 tightens to
     * EXACT match (the stock value is `%8.0f`, separate from the RATE's `%.*f`),
     * and asserts the finer precision is NOT present (so "+2.0" can't satisfy a
     * `decimal_places=0` expectation and "+2.00" can't satisfy `decimal_places=2`
     * via a prefix match on "+2"). */
    for (int d = 0; d <= 3; d++) {
        def.decimal_places = d;
        format_rate_line(b, sizeof(b), 1, "Wood", 60.0f, 2.0f/60.0f, &def);
        char exp[24];
        if (d == 0) lstrcpyA(exp, "+2/min");       /* whole number, no '.' */
        else _snprintf(exp, sizeof(exp), "+2.%.*d/min", d, 0);
        char msg[96];
        _snprintf(msg, sizeof(msg), "R12: decimal_places=%d EXACT rate match", d);
        char *hr = strstr(b, exp);
        CHECK(hr != NULL, msg);
        if (d == 0) {
            CHECK(strstr(b, "+2.") == NULL, "R12: decimal_places=0 -> no '.' in the rate");
        } else {
            /* one finer decimal must NOT appear right after the "+2.": build
             * "+2." + (d+1) zeros and require it absent (only when buffer kept it) */
            char finer[24];
            _snprintf(finer, sizeof(finer), "+2.%.*d", d + 1, 0);
            if (strlen(b) + strlen(finer) < sizeof(b)) {  /* room to attempt the match */
                char msg2[96];
                _snprintf(msg2, sizeof(msg2),
                          "R12: decimal_places=%d -> finer precision absent", d);
                CHECK(strstr(b, finer) == NULL, msg2);
            }
        }
    }
    def.decimal_places = 5;
    format_rate_line(b, sizeof(b), 1, "Wood", 60.0f, 2.0f/60.0f, &def);
    CHECK(strstr(b, "+2.000/min") != NULL && strstr(b, "2.0000") == NULL,
          "R12: decimal_places clamped to 3 (no 4th decimal, exact bound)");

    /* plus sign off: positive rate strips the '+' (negative keeps '-') */
    def.decimal_places = 1;
    def.show_plus_sign = 0;
    format_rate_line(b, sizeof(b), 1, "Wood", 60.0f, 2.0f/60.0f, &def);
    CHECK(strstr(b, "2.0/min") != NULL && strstr(b, "+2") == NULL,
          "R11: show_plus_sign=0 strips '+' on positive rates");
    format_rate_line(b, sizeof(b), 1, "Wood", 60.0f, -2.0f/60.0f, &def);
    CHECK(strstr(b, "-2.0") != NULL, "R11: negative rate always keeps '-'");
    def.show_plus_sign = 1;

    /* NULL name + names off -> Slot%d fallback (still readable) */
    def.show_resource_names = 0;
    format_rate_line(b, sizeof(b), 2, NULL, 100.0f, 2.0f/60.0f, &def);
    CHECK(strstr(b, "Slot2") != NULL, "R11: NULL name falls back to Slot2");
    def.show_resource_names = 1;

    /* R12: pin the TWO helper paths under show_resource_names=0. (a) NULL/empty
     * name -> "Slot%d" fallback (this is what FIX 1 makes the panel call). (b) a
     * REAL (non-NULL) label with names off -> blank label — that was the R11 bug
     * the panel hit, which FIX 1 removes by passing NULL instead. */
    def.show_resource_names = 0;
    format_rate_line(b, sizeof(b), 1, "Wood", 60.0f, 2.0f/60.0f, &def);
    CHECK(strstr(b, "Wood") == NULL && strstr(b, "Slot1") == NULL &&
          strstr(b, "2.0/min") != NULL,
          "R12: names off + real label -> blank label (panel never does this now)");
    format_rate_line(b, sizeof(b), 1, NULL, 60.0f, 2.0f/60.0f, &def);
    CHECK(strstr(b, "Slot1") != NULL && strstr(b, "2.0/min") != NULL,
          "R12: names off + NULL name -> Slot1 (FIX 1 path)");
    def.show_resource_names = 1;

    /* zero-rate suppression */
    def.show_zero_rates = 0;
    CHECK(format_rate_line(b, sizeof(b), 2, "Food", 100.0f, 0.0f, &def) == 0,
          "R11: show_zero_rates=0 hides a zero-rate row (returns 0)");
    CHECK(format_rate_line(b, sizeof(b), 2, "Food", 100.0f, 3.0f, &def) == 1,
          "R11: show_zero_rates=0 keeps a nonzero row");
    def.show_zero_rates = 1;

    /* per-resource visibility */
    def.show_food = 0;
    CHECK(format_rate_line(b, sizeof(b), 2, "Food", 100.0f, 2.0f/60.0f, &def) == 0,
          "R11: show_food=0 hides Food (slot 2)");
    def.show_food = 1;
    CHECK(format_rate_line(b, sizeof(b), 2, "Food", 100.0f, 2.0f/60.0f, &def) == 1,
          "R11: show_food=1 keeps Food (slot 2)");

    /* sec unit: no *60, rate as-is, /s suffix */
    def.use_unit_min = 0;
    format_rate_line(b, sizeof(b), 2, "Food", 100.0f, 3.0f, &def);
    CHECK(strstr(b, "+3.0") != NULL && strstr(b, "/s") != NULL && strstr(b, "/min") == NULL,
          "R11: use_unit_min=0 -> per-second, no *60");
    def.use_unit_min = 1;

    /* tiny buffer: FORCE truncation and pin the ACTUAL bound the helper
     * guarantees. The helper formats via _snprintf(out, n, ...). The
     * mingw/msvcrt _snprintf, when the formatted length would exceed n, writes
     * exactly n bytes and does NOT NUL-terminate (probed: r==-1, buffer holds n
     * chars, no terminator). So the one hard guarantee is "never writes past n"
     * and the content equals the first n chars of the untruncated line. Pin
     * that with a guard byte AND an exact-prefix compare. */
    {
        static const float huge = 987654321.0f;
        static const char *long_name = "WoodiestWoodOfTheWesternWood"; /* 28 chars */
        char full[256];
        format_rate_line(full, sizeof(full), 2, long_name, huge, huge * 60.0f, &def);
        CHECK(strlen(full) > 48, "R12: overflow fixture is >48 chars");
        char tight[49];
        memset(tight, 0x55, sizeof(tight));          /* guard at tight[48] */
        format_rate_line(tight, 48, 2, long_name, huge, huge * 60.0f, &def);
        CHECK(memcmp(tight, full, 48) == 0 && tight[48] == 0x55,
              "R12: 48-byte buffer = exact 48-char prefix, no write past n");
        char tiny8[9];
        memset(tiny8, 0x55, sizeof(tiny8));          /* guard at tiny8[8] */
        format_rate_line(tiny8, 8, 2, long_name, huge, huge * 60.0f, &def);
        CHECK(memcmp(tiny8, full, 8) == 0 && tiny8[8] == 0x55,
              "R12: 8-byte buffer = exact 8-char prefix, no write past n");
        def.decimal_places = 1;
    }

    /* per-resource visibility — R12 adds the Wood/Coin/Export pairs mirroring
     * the show_food pair, pinning that mutating EACH field gates its row (the
     * R11 test only covered Food). */
    def.show_wood = 0;
    CHECK(format_rate_line(b, sizeof(b), 1, "Wood", 60.0f, 2.0f/60.0f, &def) == 0,
          "R12: show_wood=0 hides Wood (slot 1)");
    def.show_wood = 1;
    CHECK(format_rate_line(b, sizeof(b), 1, "Wood", 60.0f, 2.0f/60.0f, &def) == 1,
          "R12: show_wood=1 keeps Wood (slot 1)");
    def.show_coin = 0;
    CHECK(format_rate_line(b, sizeof(b), 0, "Coin", 60.0f, 2.0f/60.0f, &def) == 0,
          "R12: show_coin=0 hides Coin (slot 0)");
    def.show_coin = 1;
    CHECK(format_rate_line(b, sizeof(b), 0, "Coin", 60.0f, 2.0f/60.0f, &def) == 1,
          "R12: show_coin=1 keeps Coin (slot 0)");
    def.show_export = 0;
    CHECK(format_rate_line(b, sizeof(b), 7, "Export", 60.0f, 2.0f/60.0f, &def) == 0,
          "R12: show_export=0 hides Export (slot 7)");
    def.show_export = 1;
    CHECK(format_rate_line(b, sizeof(b), 7, "Export", 60.0f, 2.0f/60.0f, &def) == 1,
          "R12: show_export=1 keeps Export (slot 7)");

    /* R12: slot-map / name order probe — the 4 main rows and their labels. */
    {
        static const int slot_map[4] = { 2, 1, 0, 7 };
        static const char *slot_names[4] = { "Food", "Wood", "Coin", "Export" };
        for (int r = 0; r < 4; r++) {
            def.show_food = 1; def.show_wood = 1; def.show_coin = 1; def.show_export = 1;
            char nm[32];
            _snprintf(nm, sizeof(nm), "Slot%d", slot_map[r]);
            char msg[96];
            def.show_resource_names = 1;
            format_rate_line(b, sizeof(b), slot_map[r], slot_names[r],
                             100.0f, 2.0f/60.0f, &def);
            _snprintf(msg, sizeof(msg), "R12: row slot-map[%d]=slot %d renders %s",
                      r, slot_map[r], slot_names[r]);
            CHECK(strstr(b, slot_names[r]) != NULL, msg);
            def.show_resource_names = 0;
            format_rate_line(b, sizeof(b), slot_map[r], NULL, 100.0f,
                             2.0f/60.0f, &def);
            _snprintf(msg, sizeof(msg), "R12: names-off renders %s (NULL fallback)", nm);
            CHECK(strstr(b, nm) != NULL, msg);
        }
    }

    /* R12: clip_line 78-char cap — an >78-char line must truncate to EXACTLY
     * PANEL_LINE_MAX_CHARS chars total with the trailing ".." at 76/77, and the
     * short path (< cap) must be left untouched. (clip_line is static in ui.c,
     * reachable because this harness #includes d3d9.c.) */
    {
        char cl[160];
        memset(cl, 'A', sizeof(cl) > 120 ? 120 : sizeof(cl));   /* 120 'A's */
        cl[120] = '\0';
        size_t len0 = strlen(cl);
        CHECK(len0 > (size_t)PANEL_LINE_MAX_CHARS, "R12: clip_line overflow fixture");
        clip_line(cl, sizeof(cl));
        CHECK(strlen(cl) == (size_t)PANEL_LINE_MAX_CHARS &&
              cl[PANEL_LINE_MAX_CHARS - 2] == '.' &&
              cl[PANEL_LINE_MAX_CHARS - 1] == '.' &&
              cl[PANEL_LINE_MAX_CHARS] == '\0',
              "R12: clip_line truncates >78-char line to exactly 78 + trailing '..'");
        lstrcpyA(cl, "short line");
        clip_line(cl, sizeof(cl));
        CHECK(strcmp(cl, "short line") == 0, "R12: clip_line leaves short lines untouched");
    }

    /* version table: R12 pins ALL SIX row-0 fields equal the EXPECTED_* macros
     * (R11 only checked size + base), and rows 1..3 EACH have label==NULL and
     * all-zero fields. */
    CHECK(g_versions[0].label != NULL && strcmp(g_versions[0].label, "TAD 1.0.8") == 0,
          "R12: g_versions[0] labeled TAD 1.0.8");
    CHECK(g_versions[0].size   == EXPECTED_EXE_SIZE  &&
          g_versions[0].base   == EXPECTED_IMAGE_BASE &&
          g_versions[0].pe_hi  == EXPECTED_PE_VER_HI &&
          g_versions[0].pe_lo  == EXPECTED_PE_VER_LO &&
          g_versions[0].pe_r   == EXPECTED_PE_VER_R  &&
          g_versions[0].pe_b   == EXPECTED_PE_VER_B,
          "R12: g_versions[0] all six fields == EXPECTED_* macros");
    for (int i = 1; i < 4; i++) {
        char msg[96];
        _snprintf(msg, sizeof(msg), "R12: g_versions[%d] all-zero TODO row", i);
        CHECK(g_versions[i].label == NULL &&
              g_versions[i].size == 0 &&
              g_versions[i].base == 0 &&
              g_versions[i].pe_hi == 0 &&
              g_versions[i].pe_lo == 0 &&
              g_versions[i].pe_r  == 0 &&
              g_versions[i].pe_b  == 0,
              msg);
    }
}

int main(void) {
    tracker_init();
    test_rate_game_time();
    printf("---\n");
    test_rate_realtime();
    printf("---\n");
    test_settings_ini();
    printf("---\n");
    test_version_gate();
    printf("---\n");
    test_observer_menu();
    test_observer_rates_line();
    printf("---\n");
    test_ui_guards();
    printf("---\n");
    test_format_line();
    printf("---\n");
    test_fault_filters();
    printf("\nFAILURES: %d\n", failures);
    return failures ? 1 : 0;
}
