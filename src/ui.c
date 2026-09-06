/* ui.c — D3DX9 overlay: resource-rate panel + F9 toggle + settings panel (R14)
 *
 * Resurrects the R3-R7 draw architecture, hardened:
 *  - d3dx9_25.dll loaded at runtime; font via D3DXCreateFontA.
 *  - ID3DXFont vtable: 12 Begin, 13 DrawTextA, 15 End, 16 OnLostDevice, 17 OnResetDevice.
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

/* ---------- D3DX font vtable slots ---------- */
#define FONT_BEGIN        12
#define FONT_DRAWTEXTA    13
#define FONT_END          15
#define FONT_ONLOSTDEVICE 16
#define FONT_ONRESETDEVICE 17

#define DT_LEFT   0x00000000
#define DT_TOP    0x00000000
#define DT_NOCLIP 0x00000100
#define DT_CENTER 0x00000001

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
typedef int (STDMETHODCALLTYPE *VF_DRAWTEXT)(void *self, const char *text,
        int count, void *rect, DWORD fmt, DWORD color);

/* font vtable: Begin(12), DrawTextA(13), End(15), OnLost(16), OnReset(17) */

static HMODULE g_d3dx = NULL;
static void   *g_font = NULL;       /* g_font_dev extern lives in d3d9.c */
static int     g_ui_ready = 0;
static int     g_font_guard_warned = 0;
static int     s_font_fail_n = 0;   /* consecutive font-create fails (R16 B4) */
static unsigned short g_key_prev[8];

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
    g_key_prev[0] = g_key_prev[1] = g_key_prev[2] = g_key_prev[3] = 0;
    g_key_prev[4] = g_key_prev[5] = g_key_prev[6] = g_key_prev[7] = 0;
    g_d3dx = LoadLibraryA("d3dx9_25.dll");
    g_ui_ready = (g_d3dx != NULL);
    if (!g_ui_ready) {
        dlog("UI disabled: d3dx9_25.dll not loadable");
        return;
    }
    dlog("UI ready: d3dx9_25.dll loaded");
}

void ui_on_reset(void *dev) {
    font_destroy();
    dlog("reset dev=%p -> font invalidated", dev);
}

static int ui_key_edge(int vk, int idx) {
    unsigned short now = (unsigned short)((GetAsyncKeyState(vk) & 0x8000) ? 1 : 0);
    int edge = (idx >= 0 && idx < 8) ? (now && !g_key_prev[idx]) : now;
    if (idx >= 0 && idx < 8) g_key_prev[idx] = now;
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
    int fg = ui_is_foreground();
    int changed = 0;
    if (f9_down && fg) {
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
    dt(font, s, -1, &rc, DT_LEFT | DT_TOP | DT_NOCLIP, color);
}

static void ui_format_rate(char *out, size_t n, float rate) {
    if (g_settings.use_unit_min) rate *= 60.0f;
    if (rate >= 0.0f)
        _snprintf(out, n, "+%.1f", rate);
    else
        _snprintf(out, n, "%+.1f", rate);
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
    VF_HR begin = (VF_HR)vt[FONT_BEGIN];
    VF_HR end = (VF_HR)vt[FONT_END];
    if (!begin || !end) return;

    if (begin(g_font) < 0) return; /* Begin failed: skip entirely */

    int fs = (g_settings.font_size >= 8 && g_settings.font_size <= 40)
           ? g_settings.font_size : 14;
    int row = fs + 5;
    int px = g_settings.pos_x;
    int py = g_settings.pos_y;

    DWORD alpha = (DWORD)((g_settings.opacity * 255.0f) + 0.5f);
    if (alpha > 255) alpha = 255;
    DWORD panel_col = UI_D3DCOLOR_ARGB(alpha, 8, 10, 12);
    DWORD header_col = UI_D3DCOLOR_ARGB(alpha, 0xE0, 0xC0, 0x80);
    DWORD text_col = UI_D3DCOLOR_ARGB(alpha, 0xFF, 0xF3, 0xD6);
    DWORD dim_col = UI_D3DCOLOR_ARGB(alpha, 0xA8, 0x90, 0x60);

    char food[80], wood[80], coin[80], expo[80];
    float rf = rate_get_ema(2), rw = rate_get_ema(1), rc = rate_get_ema(0), rx = rate_get_ema(7);
    _snprintf(food, sizeof(food), "Food %8.0f", g_last_values[2]);
    _snprintf(wood, sizeof(wood), "Wood %8.0f", g_last_values[1]);
    _snprintf(coin, sizeof(coin), "Coin %8.0f", g_last_values[0]);
    _snprintf(expo, sizeof(expo), "Export %8.0f", g_last_values[7]);

    char rf_s[40], rw_s[40], rc_s[40], rx_s[40];
    ui_format_rate(rf_s, sizeof(rf_s), rf);
    ui_format_rate(rw_s, sizeof(rw_s), rw);
    ui_format_rate(rc_s, sizeof(rc_s), rc);
    ui_format_rate(rx_s, sizeof(rx_s), rx);
    const char *un = rate_unit_label();

    int lines = 4 + (g_settings.show_header ? 1 : 0) + (g_settings.show_slots_567 ? 4 : 0) + 1;
    int pw = 250;
    int ph = lines * row + 8;
    ui_draw_backdrop(dev, px, py, pw, ph, panel_col);

    int yy = py + 4;
    if (g_settings.show_header) {
        ui_font_draw(g_font, "RESOURCES", px + 8, yy, header_col);
        yy += row;
    }
    char tmp[160];
    _snprintf(tmp, sizeof(tmp), "%s  %s%s", food, rf_s, un);
    ui_font_draw(g_font, tmp, px + 8, yy, text_col); yy += row;
    _snprintf(tmp, sizeof(tmp), "%s  %s%s", wood, rw_s, un);
    ui_font_draw(g_font, tmp, px + 8, yy, text_col); yy += row;
    _snprintf(tmp, sizeof(tmp), "%s  %s%s", coin, rc_s, un);
    ui_font_draw(g_font, tmp, px + 8, yy, text_col); yy += row;
    _snprintf(tmp, sizeof(tmp), "%s  %s%s", expo, rx_s, un);
    ui_font_draw(g_font, tmp, px + 8, yy, text_col); yy += row;

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

    /* settings hint line */
    _snprintf(tmp, sizeof(tmp), "[%s] 1:header 2:slots 3:gains 4:%s 5:%dms 6:%s",
              g_panel_visible ? "ON" : "OFF",
              g_settings.use_unit_min ? "sec" : "min",
              g_settings.sample_ms, g_settings.smoothing);
    ui_font_draw(g_font, tmp, px + 8, yy, dim_col);

    end(g_font);
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

void ui_draw(void) {
    set_step("ovl-start");
    if (!g_settings.enabled) return;
    if (!g_version_ok) return;           /* version gate failed -> no overlay */
    if (!g_panel_visible) return;
    if (!g_values_valid) return;         /* no sampled RES yet -> no overlay */
    if (g_res == NULL) return;           /* menu/loading (n==0) -> no overlay */
    if (g_device == NULL) return;
    if (!g_ui_ready) return;             /* d3dx9_25.dll missing -> graceful */
    if (g_frames_since_reset < RESET_COOLDOWN_FRAMES) return; /* device settling
                                          after Create/Reset before RT calls */

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
    if (grt == NULL) return;
    set_trace(dev, vt, D9_GETRENDERTARGET, "ovl-grt");
    if (grt(dev, 0, &cur) != 0 || cur == NULL) return;
    void **cvt = *(void ***)cur;
    if (cvt == NULL) return;
    VF_HR crel = (VF_HR)cvt[2];                    /* IDirect3DSurface9::Release (slot 2) */
    {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(cur, &mbi, sizeof(mbi)) ||
            mbi.State != MEM_COMMIT ||
            (mbi.Protect & PAGE_NOACCESS) ||
            (mbi.Protect & PAGE_GUARD)) {
            if (crel != NULL) crel(cur);
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
    if (!g_ovl_first_done) { g_ovl_first_done = 1; dlog("ovl first draw ok frame=%d", g_frames_since_reset); }
    set_step("ovl-done");
}