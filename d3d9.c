/*
 * d3d9.c — AoE3 TAD resource-rate HUD mod (d3d9 proxy DLL), 32-bit i386.
 *
 * R14: Full resource-rate mod. Single-file build entry that #includes all
 * module .c files. Monolithic compilation for SWARM_TEST compatibility.
 * Single artifact: d3d9.dll. Exports: Direct3DCreate9, Direct3DCreate9Ex,
 * D3DPERF_*, DebugSet*.
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <malloc.h>

typedef DWORD D3DCOLOR;

/* ---- include shared state ---- */
#include "src/state.h"

/* ---- global variable definitions ---- */
HMODULE g_real   = NULL;
void   *g_base   = NULL;
void   *g_ctx    = NULL;
void   *g_res    = NULL;
void   *g_inc    = NULL;
void   *g_device = NULL;
void  **g_devvt  = NULL;

int   g_player_idx = -1;
int   s_snap_have  = 0;

DWORD g_obs_player = 0;
DWORD s_last_ctx   = 0;
DWORD s_last_player = 0;
int   s_last_n     = -1;
int   s_tick       = 0;

volatile char g_step[64];
FILE         *g_log       = NULL;
LPTOP_LEVEL_EXCEPTION_FILTER s_prev_filter = NULL;

/* tracker state */
SlotTracker g_tracker[MAX_SLOTS];
DWORD g_last_sample_tick = 0;
int   g_paused           = 0;
float g_last_values[MAX_SLOTS];
int   g_values_valid     = 0;
float g_last_ema[MAX_SLOTS];
int   g_slot_frozen[MAX_SLOTS];

/* settings state */
ModSettings g_settings;
int g_panel_visible = 1;

/* version gate state */
int  g_version_ok = 0;
char g_version_reason[128] = "";

/* ---- include modules ---- */
#include "src/logger.c"
#include "src/tracker.c"
#include "src/rate.c"
#include "src/gameif.c"
#include "src/settings.c"
#include "src/ui.c"

/* ========================= D3D9 wrapper ========================= */
static int patch_device_present(void *dev);

struct d3d9w { void **vt; void *real; };
typedef struct d3d9w D3D9W;
static D3D9W g_wrapper_obj;
static D3D9W *g_wrapper = NULL;

typedef int  (STDMETHODCALLTYPE *PRESENT_FN)(void *self, const RECT *a,
        const RECT *b, HWND hwnd, const void *dirty);
typedef int  (STDMETHODCALLTYPE *DEV_RESET)(void *, const void *);
static PRESENT_FN s_orig_present = NULL;
static DEV_RESET   s_orig_reset   = NULL;

/* (patch_device_present forward-declared above) */

static int STDMETHODCALLTYPE reset_hook(void *self, const void *pp);
static int STDMETHODCALLTYPE present_hook(void *self, const RECT *a, const RECT *b,
        HWND hwnd, const void *dirty);

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
        ui_create_font(*ppdev);   /* font at device-create, never mid-frame */
    }
    return hr;
}
static int STDMETHODCALLTYPE w_create_device_ex(void *self, UINT adapter, UINT type,
        HWND focus, DWORD flags, void *pparams, void *pfs, void **ppdev) {
    D3D9W *w = (D3D9W *)self;
    int hr = ((int (STDMETHODCALLTYPE *)(void *, UINT, UINT, HWND, DWORD, void *, void *, void **))
        ((void ***)w->real)[0][17])(w->real, adapter, type, focus, flags, pparams, pfs, ppdev);
    if (hr >= 0 && ppdev != NULL && *ppdev != NULL) {
        int patched = patch_device_present(*ppdev);
        dlog("CreateDeviceEx hr=%#010x dev=%p patched=%s",
             (unsigned)hr, *ppdev, patched ? "yes" : "no");
        ui_create_font(*ppdev);   /* font at device-create, never mid-frame */
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

/* ========================= device hook ========================= */

static int STDMETHODCALLTYPE reset_hook(void *self, const void *pp) {
    g_device = self;
    set_step("reset-hook");
    ui_on_reset();
    int hr = (s_orig_reset != NULL) ? s_orig_reset(self, pp) : (int)0x8876086c;
    patch_device_present(self);
    if (hr >= 0) ui_create_font(self);   /* re-create font on successful Reset */
    set_step("reset-done");
    return hr;
}

static int STDMETHODCALLTYPE present_hook(void *self, const RECT *a, const RECT *b,
        HWND hwnd, const void *dirty) {
    g_device = self;
    ensure_fault_filter();   /* the game may have replaced our unhandled filter */
    set_step("locate");
    locate_resources();

    set_step("observer-sample");
    observer_sample();

    set_step("hotkey");
    ui_check_hotkey();

    set_step("draw-overlay");
    ui_draw();

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

/* ========================= version gate ========================= */

void version_gate_check(void) {
    g_version_ok = 0;
    g_version_reason[0] = '\0';

    /* 1) exe size */
    char path[MAX_PATH];
    if (!GetModuleFileNameA(NULL, path, MAX_PATH)) {
        lstrcpyA(g_version_reason, "GetModuleFileNameA failed"); return;
    }
    HANDLE hf = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                            OPEN_EXISTING, 0, NULL);
    if (hf == INVALID_HANDLE_VALUE) { lstrcpyA(g_version_reason, "cannot open exe"); return; }
    DWORD fsize = GetFileSize(hf, NULL);
    CloseHandle(hf);
    if (fsize != EXPECTED_EXE_SIZE) {
        _snprintf(g_version_reason, sizeof(g_version_reason),
                  "exe size=%lu expected=%lu",
                  (unsigned long)fsize, (unsigned long)EXPECTED_EXE_SIZE);
        return;
    }

    /* 2) PE identity via loaded image */
    HMODULE me = GetModuleHandleA(NULL);
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)me;
    if (dos == NULL || dos->e_magic != IMAGE_DOS_SIGNATURE) {
        lstrcpyA(g_version_reason, "not a PE image"); return;
    }
    const IMAGE_NT_HEADERS *nth = (const IMAGE_NT_HEADERS *)
        ((const BYTE *)me + dos->e_lfanew);
    if (nth->Signature != IMAGE_NT_SIGNATURE || nth->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        nth->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        lstrcpyA(g_version_reason, "not i386 PE32"); return;
    }
    if (nth->OptionalHeader.ImageBase != EXPECTED_IMAGE_BASE) {
        _snprintf(g_version_reason, sizeof(g_version_reason),
                  "base=%08lX expected=%08lX",
                  (unsigned long)nth->OptionalHeader.ImageBase,
                  (unsigned long)EXPECTED_IMAGE_BASE);
        return;
    }

    /* 3) file version from the version resource */
    HRSRC hr = FindResourceA(me, MAKEINTRESOURCE(1), RT_VERSION);
    if (hr == NULL) { lstrcpyA(g_version_reason, "no version resource"); return; }
    HGLOBAL hg = LoadResource(me, hr);
    const BYTE *pv = (const BYTE *)LockResource(hg);
    DWORD sz = SizeofResource(me, hr);
    if (pv == NULL || sz < 60) { lstrcpyA(g_version_reason, "version resource unreadable"); return; }
    {
        const BYTE *p = pv;
        DWORD left = sz;
        DWORD sig_off = 0;
        while (left >= 4) {
            if (*(const DWORD *)(const void *)p == 0xFEEF04BD) { sig_off = (DWORD)(p - pv); break; }
            p++; left--;
        }
        if (sig_off == 0 || sig_off + 16 > sz) {
            lstrcpyA(g_version_reason, "VS_FIXEDFILEINFO not found"); return;
        }
        DWORD ms = *(const DWORD *)(const void *)(pv + sig_off + 8);
        DWORD ls = *(const DWORD *)(const void *)(pv + sig_off + 12);
        DWORD ems = (EXPECTED_PE_VER_HI << 16) | EXPECTED_PE_VER_LO;
        DWORD els = (EXPECTED_PE_VER_R << 16) | EXPECTED_PE_VER_B;
        if (ms != ems || ls != els) {
            _snprintf(g_version_reason, sizeof(g_version_reason),
                      "pe ver=%u.%u.%u.%u expected=%u.%u.%u.%u",
                      (unsigned)(ms >> 16), (unsigned)(ms & 0xFFFF),
                      (unsigned)(ls >> 16), (unsigned)(ls & 0xFFFF),
                      EXPECTED_PE_VER_HI, EXPECTED_PE_VER_LO,
                      EXPECTED_PE_VER_R, EXPECTED_PE_VER_B);
            return;
        }
    }

    lstrcpyA(g_version_reason, "ok");
    g_version_ok = 1;
}

/* ========================= IDirect3D9 COM ========================= */

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

/* ========================= thin passthroughs ========================= */

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

/* ========================= DllMain ========================= */

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        char sys[MAX_PATH];
        g_step[0] = '\0';
        if (s_prev_filter == NULL)
            s_prev_filter = SetUnhandledExceptionFilter(fault_filter);
        /* second-chance vectored handler: cannot be replaced by the game's own
         * SetUnhandledExceptionFilter calls, so an unhandled fault ALWAYS logs
         * FAULT. LOG-ONLY (never exits) — the game may own/expect certain
         * exceptions (benign 0x406D1388 debug-print etc.). */
        AddVectoredExceptionHandler(0, vectored_fault_filter);
        set_step("dllmain");

        /* load real d3d9.dll */
        GetSystemDirectoryA(sys, MAX_PATH);
        lstrcatA(sys, "\\d3d9.dll");
        g_real = LoadLibraryA(sys);
        DisableThreadLibraryCalls(hInst);
        s_orig_present = NULL;
        s_orig_reset = NULL;
        g_devvt = NULL;
        g_base = (void *)GetModuleHandleA("age3y.exe");

        /* version gate */
        version_gate_check();

        /* init subsystems */
        logger_init();
        settings_init();
        settings_load();
        tracker_init();

        dlog("R14 DLL loaded base=%p real=%p version=%s reason=%s",
             g_base, (void *)g_real,
             g_version_ok ? "OK" : "BYPASS", g_version_reason);

        if (g_settings.debug_enabled) {
            logger_debug("debug: settings enabled=%d hotkey=0x%02X hidden=%d",
                         g_settings.enabled, g_settings.hotkey, g_settings.start_hidden);
            logger_debug("debug: font=%s size=%d opacity=%.2f pos=(%d,%d)",
                         g_settings.font_name, g_settings.font_size,
                         g_settings.opacity, g_settings.pos_x, g_settings.pos_y);
            logger_debug("debug: sample_ms=%d smoothing=%s unit=%s discontinuity=%.1f",
                         g_settings.sample_ms, g_settings.smoothing,
                         g_settings.use_unit_min ? "min" : "sec",
                         g_settings.discontinuity_ratio);
            logger_debug("debug: show_slots567=%d show_gains=%d show_header=%d",
                         g_settings.show_slots_567, g_settings.show_gains,
                         g_settings.show_header);
        }

        ui_init();
        set_step("dllmain-done");
    }
    return TRUE;
}
