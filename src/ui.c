/* ui.c — D3DX9 overlay: resource-rate panel + F9 toggle + settings panel (R14)
 *
 * Resurrects the R3-R7 draw architecture, hardened:
 *  - d3dx9_25.dll loaded at runtime; font via D3DXCreateFontA.
 *  - ID3DXFont vtable (canonical, d3dx9core.h): 14 DrawTextA, 15 DrawTextW,
 *    16 OnLostDevice, 17 OnResetDevice. NO Begin and NO End slots exist on
 *    ID3DXFont (Begin/End are ID3DXSprite) — R9: slot 13 is PreloadTextW, so
 *    the old "Begin(12)/DrawTextA(13)/End(15)" calls were shipping garbage.
 *  - Draw BEFORE original Present (hooked in d3d9.c). Explicit backbuffer RT via
 *    GetBackBuffer + SetRenderTarget; render states saved/restored.
 *  - TestCooperativeLevel guard; Reset hook (slot 16) -> font invalidate only.
 *  - Font is created at device-create and re-created LAZILY on the first
 *    present frame where TestCooperativeLevel == OK after a Reset (creating
 *    inside Reset while the device is NOTRESET returns a broken/dangling
 *    font — the round-7 eip=0xD8 ovl-panel crash). ui_draw skips cleanly
 *    when no font is present.
 *  - Crash-proof: every call null-guarded; overlay disabled gracefully if
 *    d3dx9_25.dll or the font fails.
 *  - Panel hidden in menus (n==0 => g_res==NULL) and frozen while paused
 *    (tracker freezes). F9 toggles; keys 1-6 flip settings and write INI live.
 */
#include "state.h"

#define UI_D3DCOLOR_ARGB(a,r,g,b) \
    ((DWORD)((((a)&0xff)<<24)|(((r)&0xff)<<16)|(((g)&0xff)<<8)|((b)&0xff)))

/* ---------- D3D9 device vtable slots ---------- */
#define D9_TESTCOOPLEVEL  3
#define D9_RESET          16
#define D9_BEGINSCENE     41
#define D9_ENDSCENE       42
#define D9_GETRENDERTARGET 38
#define D9_SETRENDERSTATE 57
#define D9_GETRENDERSTATE 58
#define D9_DRAWPRIMITIVEUP 83
#define D9_SETFVF         89   /* R16 B6: canonical DX9 vtable */
#define D9_GETFVF         90

/* ---------- D3D render states / values ---------- */
#define D3DRS_ZENABLE         7
#define D3DRS_ZWRITEENABLE    14
#define D3DRS_STENCILENABLE   52
#define D3DRS_ALPHABLENDENABLE 27
#define D3DRS_SRCBLEND        19
#define D3DRS_DESTBLEND       20
#define D3DRS_LIGHTING        137
#define D3DBLEND_SRCALPHA     5
#define D3DBLEND_INVSRCALPHA  6
#define D3DBLEND_ONE          2
#define D3DBLEND_ZERO         1

/* ---------- D3DX font vtable slots ----------
 * Canonical ID3DXFont (verified against the real Microsoft d3dx9core.h, SDK 43,
 * and mingw-w64/ReactOS/Wine copies): after IUnknown(0-2) ...
 *   3 GetDevice 4 GetDescA 5 GetDescW 6 GetTextMetricsA 7 GetTextMetricsW
 *   8 GetDC 9 GetGlyphData 10 PreloadCharacters 11 PreloadGlyphs
 *   12 PreloadTextA 13 PreloadTextW 14 DrawTextA 15 DrawTextW
 *   16 OnLostDevice 17 OnResetDevice
 * There is NO Begin and NO End on ID3DXFont. DrawTextA is the only call needed
 * (the real signature is (pSprite, pString, Count, pRect, Format, Color)). */
#define FONT_DRAWTEXTA    14
#define FONT_ONLOSTDEVICE 16
#define FONT_ONRESETDEVICE 17

#define DT_LEFT   0x00000000
#define DT_TOP    0x00000000
#define DT_NOCLIP 0x00000100
#define DT_CENTER 0x00000001
#define DT_SINGLELINE 0x00000020   /* R10: GDI fallback uses it (USER32) */

#define D3DPT_TRIANGLESTRIP 5
#define D3DFVF_XYZRHW_DIFFUSE 0x0044

typedef int (STDMETHODCALLTYPE *VF_HR)(void *self);
typedef int (STDMETHODCALLTYPE *VF_HRRESET)(void *self, const void *pp);
typedef int (STDMETHODCALLTYPE *VF_GETSTATE)(void *self, DWORD state, DWORD *v);
typedef int (STDMETHODCALLTYPE *VF_SETSTATE)(void *self, DWORD state, DWORD v);
typedef int (STDMETHODCALLTYPE *VF_GETRT)(void *self, DWORD idx, void **out);
typedef int (STDMETHODCALLTYPE *VF_DRAWUP)(void *self, DWORD prim, DWORD count,
        const void *data, DWORD stride);
typedef int (STDMETHODCALLTYPE *VF_FVF)(void *self, DWORD fvf);      /* SetFVF */
typedef int (STDMETHODCALLTYPE *VF_GETFVF)(void *self, DWORD *fvf);  /* GetFVF */
typedef int (STDMETHODCALLTYPE *VF_DRAWTEXT)(void *self, void *sprite,
        const char *text, int count, void *rect, DWORD fmt, DWORD color);

/* font vtable: DrawTextA(14) draws; slot 15 is DrawTextW (never called);
 * OnLost/OnReset (16/17) go unused because the LOST path destroys the font
 * (ui_on_reset) and the present path re-creates it lazily after TCL==S_OK. */

static HMODULE g_d3dx = NULL;
static void   *g_font = NULL;       /* g_font_dev extern lives in d3d9.c */
static int     g_ui_ready = 0;
static int     g_font_guard_warned = 0;
static int     s_font_fail_n = 0;   /* consecutive font-create fails (R16 B4) */
static DWORD   s_ovl_draws = 0;     /* R10: completed overlay draws (600-frame heartbeat) */
static unsigned int s_ovl_abort_n = 0;  /* R13/R14: consecutive frame aborts at the 8 pre-RT gates + 4 RT stages */
static unsigned short g_key_prev[17];  /* 6 plain toggles + indices 6/7 (F9/scratch) + 9 Alt toggles (R11) */

static void set_trace(void *dev, void **vt, int slot, const char *step);

/* Returns 1 only when the font object and its vtable are safely dereferenceable:
 * g_font non-NULL, reading *g_font won't fault, and the vtable pointed at is a
 * committed, readable (non-guard/noaccess) region. Checking both regions makes a
 * dangling/corrupted font fail BEFORE a virtual call instead of executing at
 * garbage (eip=0xD8). */
static int font_safe(void) {
    if (g_font == NULL) return 0;
    void **vt = *(void ***)g_font;
    if (vt == NULL) return 0;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(g_font, &mbi, sizeof(mbi)) ||
        mbi.State != MEM_COMMIT ||
        (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return 0;
    if (!VirtualQuery(vt, &mbi, sizeof(mbi)) ||
        mbi.State != MEM_COMMIT ||
        (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return 0;
    return 1;
}

typedef int (STDMETHODCALLTYPE *PFN_CREATEFONT)(void *dev, int h, UINT w, UINT wt,
        UINT mip, int italic, DWORD charset, DWORD outprec, DWORD qual, DWORD pitch,
        const char *face, void **out);

/* The ONLY way the font object gets freed. Strict NULL discipline: g_font is
 * cleared immediately after Release so no code can ever call through a dangling
 * font vtable (round-7 crash was exactly that: font created inside Reset ->
 * broken object -> eip=000000D8 at ovl-panel).
 * R16 B3: the Release is only attempted through a font_safe() pointer — a
 * dangling/corrupted font is dropped (both pointers NULLed) without calling
 * into garbage memory. */
static void font_destroy(void) {
    if (g_font != NULL) {
        if (font_safe()) {
            void **vt = *(void ***)g_font;
            VF_HR rel = (VF_HR)vt[2];
            if (rel) rel(g_font);
        }
        g_font = NULL;
        g_font_dev = NULL;
    }
}

void ui_init(void) {
    for (size_t i = 0; i < sizeof(g_key_prev) / sizeof(g_key_prev[0]); i++)
        g_key_prev[i] = 0;
    g_d3dx = LoadLibraryA("d3dx9_25.dll");
    g_ui_ready = (g_d3dx != NULL);
    if (!g_ui_ready) {
        dlog("UI disabled: d3dx9_25.dll not loadable");
        return;
    }
    dlog("UI ready: d3dx9_25.dll loaded");
}

/* R10: d3dx9_25.dll-loadability accessor for the ARMED/DISABLED status line and
 * the GDI fallback gate (g_ui_ready stays file-static — no renames). */
int ui_ready(void) {
    return g_ui_ready;
}

#ifdef SWARM_TEST
/* R10 harness seam: force the D3DX9-ready flag so the armed/disables branch of
 * ui_gdi_fallback_draw can be driven deterministically (never compiled into the
 * shipped d3d9.dll — SWARM_TEST only). */
void ui_test_set_ready(int v) {
    g_ui_ready = v ? 1 : 0;
}
#endif

void ui_on_reset(void *dev) {
    font_destroy();
    dlog("reset dev=%p -> font invalidated", dev);
}

static int ui_key_edge(int vk, int idx) {
    unsigned short now = (unsigned short)((GetAsyncKeyState(vk) & 0x8000) ? 1 : 0);
    int n = (int)(sizeof(g_key_prev) / sizeof(g_key_prev[0]));
    int edge = (idx >= 0 && idx < n) ? (now && g_key_prev[idx] == 0) : now;
    if (idx >= 0 && idx < n) g_key_prev[idx] = now;
    return edge;
}

void ui_toggle_panel(void) {
    g_panel_visible = !g_panel_visible;
    dlog("panel %s", g_panel_visible ? "shown" : "hidden");
}

/* R16 B2: the 1-6 settings toggles only respond while the game window is the
 * foreground app — typing "3" in in-game chat or being alt-tabbed must not
 * flip the overlay settings. */
static int ui_is_foreground(void) {
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg == NULL) return 0;
    GetWindowThreadProcessId(fg, &pid);
    return (pid == GetCurrentProcessId());
}

void ui_check_hotkey(void) {
    if (!g_settings.enabled) return;
    /* F9 toggles the panel; edge-triggered so a held F9 toggles once */
    if (ui_key_edge(g_settings.hotkey, 6)) {
        ui_toggle_panel();
    }
    if (!g_panel_visible) return;
    /* in-game settings toggles — written back to the INI live. They need the
     * game window foreground AND F9 held simultaneously (R16 B2): no phantom
     * toggles when typing in chat or driving the window from elsewhere. */
    int f9_down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    int alt_down = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;  /* R11 panel-position layer */
    int fg = ui_is_foreground();
    int changed = 0;
    if (f9_down && fg && !alt_down) {
        static const int keys[6] = { '1','2','3','4','5','6' };
        for (int i = 0; i < 6; i++) {
            if (!ui_key_edge(keys[i], i)) continue;
            switch (i) {
            case 0: g_settings.show_header = !g_settings.show_header; break;
            case 1: g_settings.show_slots_567 = !g_settings.show_slots_567; break;
            case 2: g_settings.show_gains = !g_settings.show_gains; break;
            case 3: g_settings.use_unit_min = !g_settings.use_unit_min; break;
            case 4:
                if (g_settings.sample_ms <= 100) g_settings.sample_ms = 250;
                else if (g_settings.sample_ms < 500) g_settings.sample_ms = 500;
                else if (g_settings.sample_ms < 1000) g_settings.sample_ms = 1000;
                else g_settings.sample_ms = 100;
                break;
            case 5:
                if (g_settings.smoothing[0] == 'l')
                    lstrcpyA(g_settings.smoothing, "med");
                else if (g_settings.smoothing[0] == 'm' || g_settings.smoothing[0] == '\0')
                    lstrcpyA(g_settings.smoothing, "high");
                else
                    lstrcpyA(g_settings.smoothing, "low");
                break;
            }
            changed = 1;
        }
    }
    /* R11: F9+Alt layer — format/visibility/position toggles. Same
     * foreground+F9-held edge rules as the plain layer (no phantom presses
     * while chat-typing); arbitrates on the shared edge array indices 8..16. */
    if (f9_down && fg && alt_down) {
        static const int keys2[9] = { '1','2','3','4','5','6','7','8','9' };
        for (int i = 0; i < 9; i++) {
            if (!ui_key_edge(keys2[i], 8 + i)) continue;
            switch (i) {
            case 0: g_settings.decimal_places++;
                    if (g_settings.decimal_places > 3) g_settings.decimal_places = 0;
                    break;
            case 1: g_settings.show_plus_sign = !g_settings.show_plus_sign; break;
            case 2: g_settings.show_resource_names = !g_settings.show_resource_names; break;
            case 3: g_settings.show_zero_rates = !g_settings.show_zero_rates; break;
            case 4: g_settings.show_food = !g_settings.show_food; break;
            case 5: g_settings.show_wood = !g_settings.show_wood; break;
            case 6: g_settings.show_coin = !g_settings.show_coin; break;
            case 7: g_settings.show_export = !g_settings.show_export; break;
            case 8: g_settings.position_mode++;
                    if (g_settings.position_mode > 2) g_settings.position_mode = 0;
                    break;
            }
            changed = 1;
        }
    }
    if (changed) {
        settings_save();
        dlog("settings updated via hotkeys (saved)");
    }
}

static void ui_draw_backdrop(void *dev, int x, int y, int w, int h, DWORD color) {
    if (dev == NULL || w <= 0 || h <= 0) return;
    void **vt = *(void ***)dev;
    if (vt == NULL) return;
    VF_DRAWUP du = (VF_DRAWUP)vt[D9_DRAWPRIMITIVEUP];
    if (du == NULL) return;
    struct { float x, y, z, rhw; DWORD c; } v[4];
    float fx = (float)x, fy = (float)y;
    float fw = (float)w, fh = (float)h;
    v[0].x = fx;      v[0].y = fy;      v[0].z = 0.0f; v[0].rhw = 1.0f; v[0].c = color;
    v[1].x = fx + fw; v[1].y = fy;      v[1].z = 0.0f; v[1].rhw = 1.0f; v[1].c = color;
    v[2].x = fx;      v[2].y = fy + fh; v[2].z = 0.0f; v[2].rhw = 1.0f; v[2].c = color;
    v[3].x = fx + fw; v[3].y = fy + fh; v[3].z = 0.0f; v[3].rhw = 1.0f; v[3].c = color;
    /* R16 B6: DrawPrimitiveUp respects the device's current FVF; the game may
     * have any FVF set (fog pass, minimap, xform). Save it, set the mixed
     * XYZRHW|DIFFUSE flavor this vertex layout needs, and restore afterwards so
     * the game's own primitives keep working. Breadcrumbed in case of a fault. */
    VF_GETFVF gf = (VF_GETFVF)vt[D9_GETFVF];
    VF_FVF sf = (VF_FVF)vt[D9_SETFVF];
    DWORD saved_fvf = 0;
    if (gf != NULL) { set_trace(dev, vt, D9_GETFVF, "ovl-fvf"); gf(dev, &saved_fvf); }
    if (sf != NULL) { set_trace(dev, vt, D9_SETFVF, "ovl-fvf"); sf(dev, D3DFVF_XYZRHW_DIFFUSE); }
    set_trace(dev, vt, D9_DRAWPRIMITIVEUP, "ovl-panel");
    du(dev, D3DPT_TRIANGLESTRIP, 2, v, (DWORD)(5 * sizeof(float)));
    if (sf != NULL) { set_trace(dev, vt, D9_SETFVF, "ovl-fvf"); sf(dev, saved_fvf); }
}

static void ui_font_draw(void *font, const char *s, int x, int y, DWORD color) {
    if (font == NULL || s == NULL) return;
    void **vt = *(void ***)font;
    if (vt == NULL) return;
    VF_DRAWTEXT dt = (VF_DRAWTEXT)vt[FONT_DRAWTEXTA];
    if (dt == NULL) return;
    RECT rc;
    rc.left = x; rc.top = y; rc.right = x + 400; rc.bottom = y + 200;
    /* real ID3DXFont::DrawTextA(this, pSprite, pString, Count, pRect, Format,
     * Color) — pSprite=NULL lets D3DX use its own sprite object (R9) */
    dt(font, NULL, s, -1, &rc, DT_LEFT | DT_TOP | DT_NOCLIP, color);
}

static void ui_format_rate(char *out, size_t n, float rate) {
    if (g_settings.use_unit_min) rate *= 60.0f;
    if (rate >= 0.0f)
        _snprintf(out, n, "+%.1f", rate);
    else
        _snprintf(out, n, "%+.1f", rate);
}

/* R11: truncate a line to PANEL_LINE_MAX_CHARS visible chars + "..". No font
 * width measurement is used here on purpose: GetTextMetricsA / GetDC sit at
 * font-vtable slots 6 and 8, which are NOT in the proven slot set {14,16,17},
 * so the 78-char cap is a fixed-text estimate for the hint rows (unused-byte
 * safe: only the buffer contents change, never the DrawTextA call shape). */
static void clip_line(char *s, size_t n) {
    if (s == NULL || n == 0) return;
    size_t len = strlen(s);
    if (len <= (size_t)PANEL_LINE_MAX_CHARS) return;
    if (n <= (size_t)PANEL_LINE_MAX_CHARS) return;
    s[PANEL_LINE_MAX_CHARS - 2] = '.';
    s[PANEL_LINE_MAX_CHARS - 1] = '.';
    s[PANEL_LINE_MAX_CHARS] = '\0';
}

/* R11: single owner of the panel rect so the P0 diagnostic logs the EXACT
 * rectangle ui_draw_backdrop paints. Position modes are corner anchors only:
 * Default honours pos_x/pos_y, TopLeft pins 12,12, TopRight pins to the right
 * edge using g_bb_w/g_bb_h captured by d3d9.c (0 -> falls back to TopLeft).
 * This is a corner-anchored HUD; it is NOT a native resource-bar integration
 * (that would need unverified render-pipeline hooks — README Known
 * Limitations). Height covers the worst case (all rows visible), rows that are
 * skipped (hidden resource / zero rate) just leave slack in the backdrop. */
static void ui_panel_geometry(int *px, int *py, int *pw, int *ph) {
    int fs = (g_settings.font_size >= 8 && g_settings.font_size <= 40)
           ? g_settings.font_size : 14;
    int row = fs + 5;
    int lines = 4 + (g_settings.show_header ? 1 : 0)
              + (g_settings.show_slots_567 ? 4 : 0) + 2;   /* 4 rows + hint x2 */
    int w = 250;
    int h = lines * row + 8;
    int x, y;
    switch (g_settings.position_mode) {
    case 2: x = (g_bb_w > (DWORD)w) ? (int)((int)g_bb_w - w - 12) : 12;
            y = 12;
            break;
    case 1: x = 12; y = 12;
            break;
    default: x = g_settings.pos_x; y = g_settings.pos_y;
            break;
    }
    *px = x; *py = y; *pw = w; *ph = h;
}

/* R11: the seam the RATE rows (and the harness) use. Returns 1 when the line
 * was written, 0 when the row must be SKIPPED entirely (resource hidden via
 * its show_* switch, or zero rate with ShowZeroRates off). Slot mapping:
 * 2->Food, 1->Wood, 0->Coin, 7->Export; any other slot_id is always eligible
 * (pairs with the untouched slots 3..6 loop below). The name argument is the
 * label already known at the call site; when NULL/empty and names are off the
 * slot-number fallback "Slot%d" is used so unnamed rows stay identifiable. */
int format_rate_line(char *out, size_t n, int slot_id, const char *name,
                     float value, float rate, const ModSettings *s) {
    if (s == NULL || out == NULL || n == 0) return 0;
    int eligible = 1;
    switch (slot_id) {
    case 2: eligible = s->show_food;      break;
    case 1: eligible = s->show_wood;      break;
    case 0: eligible = s->show_coin;      break;
    case 7: eligible = s->show_export;    break;
    default: break;
    }
    if (!eligible) return 0;
    float disp = rate * (s->use_unit_min ? 60.0f : 1.0f);
    if (!s->show_zero_rates && disp > -0.0005f && disp < 0.0005f) return 0;
    char name_str[32];
    if (name != NULL && name[0] != '\0') {
        if (s->show_resource_names)
            lstrcpynA(name_str, name, sizeof(name_str));
        else
            name_str[0] = '\0';
    } else {
        _snprintf(name_str, sizeof(name_str), "Slot%d", slot_id);
    }
    int dec = s->decimal_places;
    if (dec < 0) dec = 0; else if (dec > 3) dec = 3;
    const char *plus = (s->show_plus_sign && disp >= 0.0f) ? "+" : "";
    const char *unit = s->use_unit_min ? "/min" : "/s";
    _snprintf(out, n, "%s %8.0f  %s%.*f%s", name_str, value, plus, dec, disp, unit);
    return 1;
}

static void ui_draw_panel(void *dev) {
    /* double-guard (round-7): never call through the font unless the object AND
     * its vtable are actually mapped readable. A broken/dangling font faults
     * with eip = small offset (live: eip=000000D8 at ovl-panel). */
    if (!font_safe()) {
        if (!g_font_guard_warned) {
            g_font_guard_warned = 1;
            logger_debug("UI: font guard failed - panel skipped");
        }
        return;
    }
    if (g_font == NULL) return;
    void **vt = *(void ***)g_font;
    if (vt == NULL) return;

    /* R9: ID3DXFont has NO Begin/End — the old slot-12 "Begin" was actually
     * PreloadTextA and slot-15 "End" was DrawTextW, so those dispatches were
     * calling garbage. The text below (DrawTextA, slot 14) is the whole draw. */

    int px, py, pw, ph;
    ui_panel_geometry(&px, &py, &pw, &ph);
    int fs = (g_settings.font_size >= 8 && g_settings.font_size <= 40)
           ? g_settings.font_size : 14;
    int row = fs + 5;

    DWORD alpha = (DWORD)((g_settings.opacity * 255.0f) + 0.5f);
    if (alpha > 255) alpha = 255;
    DWORD panel_col = UI_D3DCOLOR_ARGB(alpha, 8, 10, 12);
    DWORD header_col = UI_D3DCOLOR_ARGB(alpha, 0xE0, 0xC0, 0x80);
    DWORD text_col = UI_D3DCOLOR_ARGB(alpha, 0xFF, 0xF3, 0xD6);
    DWORD dim_col = UI_D3DCOLOR_ARGB(alpha, 0xA8, 0x90, 0x60);

    const char *un = rate_unit_label();   /* slots 3..6 loop still uses ui_format_rate */
    ui_draw_backdrop(dev, px, py, pw, ph, panel_col);

    /* breadcrumb the font call: a fault inside DrawTextA is then nameable
     * (g_fault_dev=font, g_fault_slot=14, step=ovl-panel-text) */
    set_trace(g_font, vt, FONT_DRAWTEXTA, "ovl-panel-text");

    int yy = py + 4;
    if (g_settings.show_header) {
        ui_font_draw(g_font, "RESOURCES", px + 8, yy, header_col);
        yy += row;
    }
    char tmp[160];
    char line[160];
    /* R11: the four main resource rows route through format_rate_line so the
     * per-resource visibility switches, decimal places and plus-sign settings
     * apply uniformly (hidden or zero-rate rows skip the line AND its row). */
    /* R12: ShowResourceNames=0 must yield "Slot%d", not a blank label — the
     * helper only falls back to "Slot%d" when the name arg is NULL/empty, and
     * this is the only caller that always had the real label, so names-off
     * used to print a blank label. Pass NULL whenever names are hidden and the
     * helper's existing NULL fallback does the right thing. Default (names on)
     * unchanged. Rows ordered by the slot map 2/1/0/7 (Food/Wood/Coin/Export). */
    if (format_rate_line(line, sizeof(line), 2,
                         g_settings.show_resource_names ? "Food" : NULL,
                         g_last_values[2], rate_get_ema(2), &g_settings)) {
        ui_font_draw(g_font, line, px + 8, yy, text_col);
        yy += row;
    }
    if (format_rate_line(line, sizeof(line), 1,
                         g_settings.show_resource_names ? "Wood" : NULL,
                         g_last_values[1], rate_get_ema(1), &g_settings)) {
        ui_font_draw(g_font, line, px + 8, yy, text_col);
        yy += row;
    }
    if (format_rate_line(line, sizeof(line), 0,
                         g_settings.show_resource_names ? "Coin" : NULL,
                         g_last_values[0], rate_get_ema(0), &g_settings)) {
        ui_font_draw(g_font, line, px + 8, yy, text_col);
        yy += row;
    }
    if (format_rate_line(line, sizeof(line), 7,
                         g_settings.show_resource_names ? "Export" : NULL,
                         g_last_values[7], rate_get_ema(7), &g_settings)) {
        ui_font_draw(g_font, line, px + 8, yy, text_col);
        yy += row;
    }

    if (g_settings.show_slots_567) {
        for (int s = 3; s <= 6; s++) {
            float v = g_last_values[s];
            if (v < 0.5f && v > -0.5f) continue; /* show-if-nonzero */
            float r = rate_get_ema(s);
            char vs[40], rs[40];
            _snprintf(vs, sizeof(vs), "Slot%d %8.0f", s, v);
            ui_format_rate(rs, sizeof(rs), r);
            _snprintf(tmp, sizeof(tmp), "%s  %s%s", vs, rs, un);
            ui_font_draw(g_font, tmp, px + 8, yy, dim_col);
            yy += row;
        }
    }

    /* settings hint lines (R11: two rows; both clip at PANEL_LINE_MAX_CHARS) */
    _snprintf(tmp, sizeof(tmp), "[%s] 1:header 2:slots 3:gains 4:%s 5:%dms 6:%s",
              g_panel_visible ? "ON" : "OFF",
              g_settings.use_unit_min ? "sec" : "min",
              g_settings.sample_ms, g_settings.smoothing);
    clip_line(tmp, sizeof(tmp));
    ui_font_draw(g_font, tmp, px + 8, yy, dim_col);
    yy += row;
    _snprintf(tmp, sizeof(tmp),
              "Alt+1:dps 2:plus 3:names 4:zeror 5:food 6:wood 7:coin 8:exp 9:pos");
    clip_line(tmp, sizeof(tmp));
    ui_font_draw(g_font, tmp, px + 8, yy, dim_col);

    set_step("ovl-panel"); /* font draws done — back on the device view */
}

void ui_create_font(void *dev) {
    /* R16 B1: a font is bound to ONE device. If one already exists but for a
     * different device (e.g. a Reset/Create on a new object), drop it first so
     * the HUD never draws through a font that belongs to a gone device. */
    if (g_font != NULL) {
        if (g_font_dev == dev) return;
        font_destroy();
    }
    if (dev == NULL) return;
    if (!g_ui_ready) return;
    void **vt = *(void ***)dev;
    if (vt == NULL) return;
    PFN_CREATEFONT cf = (PFN_CREATEFONT)(DWORD_PTR)
        GetProcAddress(g_d3dx, "D3DXCreateFontA");
    if (cf == NULL) {
        dlog("UI: D3DXCreateFontA missing");
        return;
    }
    const char *face = (g_settings.font_name[0] != '\0')
                     ? g_settings.font_name : "Georgia";
    int size = (g_settings.font_size >= 8 && g_settings.font_size <= 40)
             ? g_settings.font_size : 14;
    void *font = NULL;
    int hr = cf(dev, -size, 0, 400, 1, 0, 0, 1, 0, 0, face, &font);
    if (hr == 0 && font != NULL) {
        g_font = font;
        g_font_dev = dev;
        s_font_fail_n = 0;
        dlog("UI: font created size=%d face=%s font=%p", size, face, font);
    } else {
        /* never leave a dangling/non-NULL font on failure; log at most once,
         * then a heartbeat every 300 failures (R16 B4) so a persistent
         * create-failure cannot flood the live log ~60 lines/sec */
        g_font = NULL;
        g_font_dev = NULL;
        s_font_fail_n++;
        unsigned int h = (unsigned)hr;
        if (s_font_fail_n == 1)
            dlog("UI: font create FAILED hr=0x%08X", h);
        else if ((s_font_fail_n % 300) == 0)
            dlog("UI: font create still failing (%d)", s_font_fail_n);
    }
}

/* per-call trace: records the device + vtable + slot being called so a FAULT
 * dump (which sees only globals) names the exact dying call */
static void set_trace(void *dev, void **vt, int slot, const char *step) {
    g_fault_dev = dev;
    g_fault_vt  = vt;
    g_fault_slot = slot;
    set_step(step);
}

/* R14: silent-stop detector (generalized) — one counter + helper covering
 * BOTH the six/eight PRE-RT gates below (enabled/version/panel/values/res/
 * device/ui_ready/cooldown) AND the four RT stages (grt-missing / grt-fail /
 * surface-bad / guard-fail). Live fact: the R13 build detected zero RT-stage
 * aborts while the panel vanished AFTER 'ovl first draw ok' + a clean
 * 'ovl diag rt=.. rect=12,12:250x141 alpha=0xD9' — yet no heartbeat either.
 * That can only be a GATE killing the draw every frame (those were silent in
 * R13 — the detector's blind spot). After 60 CONSECUTIVE aborts (~1s at
 * 60fps) it logs ONCE per burst (not per frame, NOT debug-gated), naming the
 * exact stage, so the killer gate/stage is never ambiguous again. The counter
 * resets on the first draw that completes (reaches the heartbeat bump below). */
static void ovl_abort_stop(const char *stage) {
    /* R15: menu false-alarm suppression — BEFORE a match starts there is no
     * chain, so g_values_valid==0 and g_res==NULL are BY DESIGN and their
     * per-frame bursts only spam `ovl stop` on menus. Until g_match_active
     * (set by gameif.c on `--- match start ---`, cleared on chain-break) the
     * values/res stages do NOT count; the other six gates
     * (enabled/version/panel/device/ui_ready/cooldown) always do. */
    if (!g_match_active && (!strcmp(stage, "values") || !strcmp(stage, "res")))
        return;
    s_ovl_abort_n++;
    if (s_ovl_abort_n == 60) {
        dlog("ovl stop: %u consecutive frame aborts at stage=%s - overlay draw halted",
             s_ovl_abort_n, stage);
    }
}

void ui_draw(void) {
    set_step("ovl-start");
    if (!g_settings.enabled) { ovl_abort_stop("enabled"); return; }
    if (!g_version_ok) { ovl_abort_stop("version"); return; }
    if (!g_panel_visible) { ovl_abort_stop("panel"); return; }
    if (!g_values_valid) { ovl_abort_stop("values"); return; }
    if (g_res == NULL) { ovl_abort_stop("res"); return; }
    if (g_device == NULL) { ovl_abort_stop("device"); return; }
    if (!g_ui_ready) { ovl_abort_stop("ui_ready"); return; }
    if (g_frames_since_reset < RESET_COOLDOWN_FRAMES) { ovl_abort_stop("cooldown"); return; }

    void *dev = g_device;
    void **vt = *(void ***)dev;
    if (vt == NULL) return;

    /* cooperative level guard */
    set_trace(dev, vt, D9_TESTCOOPLEVEL, "ovl-tcl");
    VF_HR tcl = (VF_HR)vt[D9_TESTCOOPLEVEL];
    if (tcl != NULL) {
        int hr = tcl(dev);
        DWORD uhr = (DWORD)(unsigned long)hr;
        if (uhr == 0x88760868u) return;    /* D3DERR_DEVICELOST: no draw */
        if (uhr == 0x88760869u) return;    /* D3DERR_DEVICENOTRESET: no draw
                                              (font re-created lazily below) */
        if (uhr != 0) return;              /* R16 B5: only S_OK may proceed */
    }

    /* LAZY font creation (round-7 fix): the font is NEVER created inside the
     * Reset hook — the device is still NOTRESET there and D3DXCreateFontA
     * fails/returns a broken object. Now TCL==OK just proved the device is
     * truly usable, so (re)create if needed. Fails -> skip the frame. */
    if (g_font == NULL) {
        ui_create_font(dev);
    }
    if (g_font == NULL) return;          /* font-first guard: none -> no overlay */

    /* Draw ON THE GAME'S CURRENT render target — no SetRenderTarget /
     * GetBackBuffer anywhere in the draw path (round-5 crash sites ovl-srt /
     * ovl-gbb eliminated). At Present time the device's current RT IS the
     * backbuffer, so the HUD stays visible (same visibility logic as drawing
     * before Present). GetRenderTarget is hardened: hr==0, surface != NULL,
     * and a VirtualQuery guard on the surface before any use; any check fails
     * -> skip the whole frame. */
    void *cur = NULL;
    VF_GETRT grt = (VF_GETRT)vt[D9_GETRENDERTARGET];
    if (grt == NULL) { ovl_abort_stop("grt-missing"); return; }
    set_trace(dev, vt, D9_GETRENDERTARGET, "ovl-grt");
    if (grt(dev, 0, &cur) != 0 || cur == NULL) { ovl_abort_stop("grt-fail"); return; }
    void **cvt = *(void ***)cur;
    if (cvt == NULL) { ovl_abort_stop("surface-bad"); return; }
    VF_HR crel = (VF_HR)cvt[2];                    /* IDirect3DSurface9::Release (slot 2) */
    {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(cur, &mbi, sizeof(mbi)) ||
            mbi.State != MEM_COMMIT ||
            (mbi.Protect & PAGE_NOACCESS) ||
            (mbi.Protect & PAGE_GUARD)) {
            if (crel != NULL) crel(cur);
            ovl_abort_stop("guard-fail");
            return;
        }
    }

    /* render-state save / set (drawn on the current RT; restored at the end) */
    set_trace(dev, vt, D9_GETRENDERSTATE, "ovl-states");
    DWORD st_z = 1, st_zw = 1, st_st = 0, st_ab = 0, st_sb = 0, st_db = 0, st_li = 1;
    VF_GETSTATE grs = (VF_GETSTATE)vt[D9_GETRENDERSTATE];
    VF_SETSTATE srs = (VF_SETSTATE)vt[D9_SETRENDERSTATE];
    if (grs != NULL) {
        grs(dev, D3DRS_ZENABLE, &st_z);
        grs(dev, D3DRS_ZWRITEENABLE, &st_zw);
        grs(dev, D3DRS_STENCILENABLE, &st_st);
        grs(dev, D3DRS_ALPHABLENDENABLE, &st_ab);
        grs(dev, D3DRS_SRCBLEND, &st_sb);
        grs(dev, D3DRS_DESTBLEND, &st_db);
        grs(dev, D3DRS_LIGHTING, &st_li);
    }
    if (srs != NULL) {
        srs(dev, D3DRS_ZENABLE, 0);
        srs(dev, D3DRS_ZWRITEENABLE, 0);
        srs(dev, D3DRS_STENCILENABLE, 0);
        srs(dev, D3DRS_ALPHABLENDENABLE, 1);
        srs(dev, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        srs(dev, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        srs(dev, D3DRS_LIGHTING, 0);
    }

    /* scene + panel on the current RT: EndScene is ALWAYS called once
     * BeginScene succeeded */
    set_trace(dev, vt, D9_BEGINSCENE, "ovl-begin");
    VF_HR beg = (VF_HR)vt[D9_BEGINSCENE];
    VF_HR end = (VF_HR)vt[D9_ENDSCENE];
    if (beg != NULL && end != NULL && beg(dev) >= 0) {
        set_trace(dev, vt, D9_DRAWPRIMITIVEUP, "ovl-panel");
        ui_draw_panel(dev);
        set_trace(dev, vt, D9_ENDSCENE, "ovl-end");
        end(dev);
    }

    /* restore render states (reverse) — unreachable failure left none open,
     * since every early-return above happens BEFORE any state was changed */
    set_trace(dev, vt, D9_GETRENDERTARGET, "ovl-restore");
    if (srs != NULL) {
        srs(dev, D3DRS_LIGHTING, st_li);
        srs(dev, D3DRS_DESTBLEND, st_db);
        srs(dev, D3DRS_SRCBLEND, st_sb);
        srs(dev, D3DRS_ALPHABLENDENABLE, st_ab);
        srs(dev, D3DRS_STENCILENABLE, st_st);
        srs(dev, D3DRS_ZWRITEENABLE, st_zw);
        srs(dev, D3DRS_ZENABLE, st_z);
    }
    if (crel != NULL) crel(cur);   /* release our GetRenderTarget reference */
    if (!g_ovl_first_done) {
        dlog("ovl first draw ok frame=%d", g_frames_since_reset);
        /* R13: the render diagnostic is UNCONDITIONAL by design — exactly one
         * line per process load, even on a shipped DebugEnabled=0 run, because
         * it is the PRIMARY render-invisibility diagnostic: a missing line on
         * an otherwise-working load means the deployed DLL is stale or the
         * draw path died before the first successful draw. Content identical
         * to R11 P0: the CURRENT render target (released pointer value only,
         * never dereferenced) plus the exact panel rect/alpha that
         * ui_draw_panel used this same frame. g_ovl_first_done semantics kept:
         * the flag is set AFTER this block. */
        int dpx, dpy, dpw, dph;
        ui_panel_geometry(&dpx, &dpy, &dpw, &dph);
        DWORD dal = (DWORD)((g_settings.opacity * 255.0f) + 0.5f);
        if (dal > 255) dal = 255;
        dlog("ovl diag rt=%p rect=%ld,%ld:%ldx%ld alpha=0x%lX",
             cur, (long)dpx, (long)dpy, (long)dpw, (long)dph,
             (unsigned long)dal);
        g_ovl_first_done = 1;
    }
    /* ROUND 10 heartbeat: every 600 completed draws (~10s at 60fps) prove the
     * draw path is STILL completing long after first-frame — not debug-gated
     * (max ~6 lines/min), value/res reads reuse what ui_draw already has. */
    s_ovl_abort_n = 0;   /* R13: any completed draw resets the stop detector */
    s_ovl_draws++;
    if ((s_ovl_draws % OVL_HEARTBEAT_FRAMES) == 0) {
        dlog("ovl heartbeat n=%u frame=%d res=%p food=%0.0f wood=%0.0f coin=%0.0f export=%0.0f",
             s_ovl_draws, g_frames_since_reset, g_res,
             g_last_values[2], g_last_values[1], g_last_values[0], g_last_values[7]);
    }
    set_step("ovl-done");
}

/* ========================= ROUND 10: GDI VISIBLE FALLBACK =========================
 * When the overlay is DISABLED (version gate failed -> `ver:<reason>`, or
 * d3dx9_25.dll not loadable) a USER32/GDI red text line is drawn on the top-left
 * of the game window every frame, so "invisible failure" is impossible. Fully
 * dependency-free: only USER32/KERNEL32 import-table symbols are used —
 * SetBkMode/SetBkColor/SetTextColor come from gdi32.dll loaded DYNAMICALLY
 * (cached), keeping the DLL import table exactly {KERNEL32, USER32, msvcrt}
 * (verify_pe.py / test_pe_structure.py stay green; no -lgdi32 in build.bat).
 * The best HWND is found ONCE per process (largest visible, non-minimized
 * top-level window of this process); returns 0 quietly when unavailable.
 * No new breadcrumbs, no dlog per frame — the drawn line IS the visibility. */

typedef int (WINAPI *PFN_GDI_TEXTCOLOR)(HDC, DWORD);   /* SetTextColor(COLORREF) */
typedef int (WINAPI *PFN_GDI_BKCOLOR)(HDC, DWORD);     /* SetBkColor(COLORREF) */
typedef int (WINAPI *PFN_GDI_BKMODE)(HDC, int);        /* SetBkMode */
static PFN_GDI_TEXTCOLOR s_gdi_settext = NULL;
static PFN_GDI_BKCOLOR   s_gdi_setbk   = NULL;
static PFN_GDI_BKMODE    s_gdi_bkmode  = NULL;
static HWND s_gdi_hwnd = NULL;    /* cached after the first successful scan */
static HWND s_gdi_best_hwnd;      /* per-scan best (EnumWindows accumulator) */
static long s_gdi_best_area;

static BOOL CALLBACK ui_gdi_find_hwnd(HWND top, LPARAM l) {
    (void)l;
    DWORD pid = 0;
    GetWindowThreadProcessId(top, &pid);
    if (pid != GetCurrentProcessId()) return TRUE;
    if (!IsWindowVisible(top) || IsIconic(top)) return TRUE;
    RECT rc;
    if (!GetWindowRect(top, &rc)) return TRUE;
    long area = (long)(rc.right - rc.left) * (long)(rc.bottom - rc.top);
    if (s_gdi_best_hwnd == NULL || area > s_gdi_best_area) {
        s_gdi_best_hwnd = top;
        s_gdi_best_area = area;
    }
    return TRUE;
}

int ui_gdi_fallback_draw(void) {
    if (overlay_disabled_reason()[0] == '\0') return 0;   /* ARMED -> draw nothing */
    if (s_gdi_hwnd == NULL) {
        s_gdi_best_hwnd = NULL;
        s_gdi_best_area = 0;
        if (!EnumWindows(ui_gdi_find_hwnd, 0)) return 0;
        s_gdi_hwnd = s_gdi_best_hwnd;
        if (s_gdi_hwnd == NULL) return 0;   /* no visible window -> safe no-op */
    }
    HDC dc = GetDC(s_gdi_hwnd);
    if (dc == NULL) return 0;
    RECT rc;
    GetClientRect(s_gdi_hwnd, &rc);
    rc.left = 8;
    rc.top = 8;
    if (s_gdi_settext == NULL) {
        HMODULE g = LoadLibraryA("gdi32.dll");
        if (g != NULL) {
            s_gdi_settext = (PFN_GDI_TEXTCOLOR)(DWORD_PTR)GetProcAddress(g, "SetTextColor");
            s_gdi_setbk   = (PFN_GDI_BKCOLOR)(DWORD_PTR)GetProcAddress(g, "SetBkColor");
            s_gdi_bkmode  = (PFN_GDI_BKMODE)(DWORD_PTR)GetProcAddress(g, "SetBkMode");
        }
    }
    if (s_gdi_bkmode != NULL)  s_gdi_bkmode(dc, OPAQUE);
    if (s_gdi_setbk != NULL)   s_gdi_setbk(dc, 0x000000u);        /* black background */
    if (s_gdi_settext != NULL) s_gdi_settext(dc, 0x002828E0u);    /* RGB(0xE0,0x28,0x28) */
    static char s_gdi_msg[160];
    _snprintf(s_gdi_msg, sizeof(s_gdi_msg), "Resource Rate Mod: disabled (%s)",
              overlay_disabled_reason());
    DrawTextA(dc, s_gdi_msg, -1, &rc, DT_LEFT | DT_TOP | DT_NOCLIP | DT_SINGLELINE);
    ReleaseDC(s_gdi_hwnd, dc);
    return 1;
}