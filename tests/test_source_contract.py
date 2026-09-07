#!/usr/bin/env python3
"""test_source_contract.py — round-14 source-level contract checks for the
R13->R14 upgrade: the observer chain/decrypt/logging contracts survive, and the
new R14 rate/overlay/settings layer is present and correctly wired.

R13 observer contracts (must NOT regress):
  - observer_sample() present, called from present_hook BEFORE the original
    Present, logs one `RES t=%d food=%d wood=%d coin=%d export=%d
    player=%08X res=%08X` line per tick. Slot map food=slot2/wood=slot1/
    coin=slot0/export=slot7. Match-start separator `--- match start ... ---`.
  - g_player_idx tracked (1 for [human-p1], pick_i fallback, -1 entry).
  - fallback scan picks largest decrypted food (slot 2); [chain-break] tags.
  - quiet chain log gate (first 5 calls, then on change).
  - NO per-player dumps / resraw / altCap/altAdd/altPanel / cell lines.

R14 contracts (new):
  - draw layer lives in src/ui.c: D3DXCreateFontA loaded via GetProcAddress at
    runtime (no import-table dep on d3dx9*); ID3DXFont vtable DrawTextA 14 /
    OnLostDevice 16 / OnResetDevice 17 (R9: the old "Begin 12 / DrawTextA 13 /
    End 15" table was wrong — ID3DXFont has NO Begin/End; slot 13 is
    PreloadTextW).
  - device vtable hooks: Reset slot 16, Present slot 17; ui_draw runs BEFORE
    the original Present. TestCooperativeLevel / GetBackBuffer /
    SetRenderTarget / DrawPrimitiveUp present.
  - rate engine in src/tracker.c: EMA alphas 0.1/0.2/0.4, spending-spike skip
    (ratio-based), pause freeze, time-step/EMA ring.
  - settings in src/settings.c: INI sections General/Display/Rate/Debug,
    atomic write-back; DefaultProfile*.xml merge with mtime gate.
  - version gate in d3d9.c refuses to arm the overlay on mismatch.

Usage: C:\\Python312\\python.exe tests\\test_source_contract.py
"""
import os
import sys
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "d3d9.c")

failures = 0


def check(cond, msg):
    global failures
    if cond:
        print(f"PASS: {msg}")
    else:
        print(f"FAIL: {msg}")
        failures += 1


def load_sources():
    """d3d9.c plus every src/*.c module = the whole compiled TU."""
    parts = [SRC]
    src_dir = os.path.join(ROOT, "src")
    for name in sorted(os.listdir(src_dir)):
        if name.endswith(".c"):
            parts.append(os.path.join(src_dir, name))
    text = []
    for p in parts:
        with open(p, "r", encoding="utf-8") as fh:
            text.append(f"\n/* ==== {os.path.basename(p)} ==== */\n" + fh.read())
    return "\n".join(text)


src = load_sources()
code = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
code = re.sub(r"//[^\r\n]*", "", code)


def code_contains(sym):
    return sym in code


# ================= R13 observer contracts (regression) =================

check("observer_sample" in src, "observer_sample() function present")
m = re.search(r'"RES t=%d food=%d wood=%d coin=%d export=%d player=%08X res=%08X"', src)
check(m is not None,
      "R13 RES line `RES t=%d food=%d wood=%d coin=%d export=%d player=%08X res=%08X` present")
m = re.search(r'"--- match start n=%d player=%08X res=%08X ---"', src)
check(m is not None, "R13 match-start separator format present")
check("rnd_i(v[2]), rnd_i(v[1]), rnd_i(v[0]), rnd_i(v[7])" in src,
      "named RES line maps food=slot2, wood=slot1, coin=slot0, export=slot7")
check("decrypt_slot_at(rr, s)" in src,
      "observer reads the 8 slots via decrypt_slot_at (real decrypt)")

# present_hook must run the SHARED overlay body (observer -> hotkey -> overlay)
# then forward to the original device Present (R15: the body lives in the
# common overlay_present_common, so the hook is a thin forward — one code path
# for both Device::Present and SwapChain::Present).
i_obs = src.find("observer_sample();")
i_ui = src.find("ui_draw();")
i_orig = src.find("fl(self, a, b, hwnd, dirty)")
check(i_obs != -1, "present_hook path calls observer_sample()")
check(i_ui != -1, "present_hook path calls ui_draw()")
check(i_orig != -1, "present_hook still forwards the original Present")
check(i_obs != -1 and i_ui != -1 and i_orig != -1 and
      i_obs < i_ui < i_orig,
      "present_hook order: observer -> hotkey -> overlay -> original Present")
check(src.find("set_step(\"observer-sample\")") < src.find("set_step(\"draw-overlay\")") < i_orig,
      "observer (read) runs strictly before the overlay flip")
check("ui_check_hotkey()" in src, "present_hook path checks the F9/settings hotkeys")

check("g_player_idx = 1;" in src, "primary path: g_player_idx = 1 (human-p1)")
check("g_player_idx = pick_i;" in src, "fallback path: g_player_idx = pick_i")
check("g_player_idx = -1;" in src, "reset/entry: g_player_idx = -1")
check("[human-p1]" in src, "chain tag [human-p1] present")
check("n >= 2" in src or "n>=2" in src, "primary path checks n >= 2")
check("arr + 1 * 4" in src or "arr + 4" in src, "primary path reads player index 1")
check("[fallback:" in src, "chain tag [fallback: ...] present")
m = re.search(r"for\s*\(\s*int\s+i\s*=\s*0\s*;\s*i\s*<\s*n\s*;\s*i\+\+\)", src)
check(m is not None, "fallback iterates the ctx players table (0..n)")
check("decrypt_slot_at(r, 2)" in src or "decrypt_slot_at(r2, 2)" in src,
      "fallback scan uses largest food (slot 2)")
check("[chain-break: no sane player]" in src, "chain-break tag retained")
check("chain game=" in src, "compact chain log format `chain game= ...` present")
check("g_chain_calls > 5" in src, "chain log quiet-gate (first 5, then on change) present")

# no per-player scanning / dumps / raw trail (R13 mandated deletions)
check("P%d res" not in src, "no per-player P%d res lines")
check("P%d raw" not in src, "no per-player P%d raw trail lines")
check("dump_all_players" not in src, "no dump_all_players")
check("dump_window_cells" not in src, "no dump_window_cells")
check("log_resraw" not in src, "no log_resraw")
check("resraw" not in src, "no resraw references")
check("altCap" not in src and "altAdd" not in src and "altPanel" not in src,
      "no altCap/altAdd/altPanel probes")
check("cell P" not in src, "no cell dump lines")
check("s_tick++" in src, "tick counter increments each frame")

# ================= R14: draw layer present, runtime-loaded =================

check("D3DXCreateFontA" in src, "D3DXCreateFontA referenced (runtime font)")
check("GetProcAddress(g_d3dx, \"D3DXCreateFontA\")" in src,
      "D3DXCreateFontA resolved via GetProcAddress (no import dep)")
check("LoadLibraryA(\"d3dx9_25.dll\")" in src,
      "d3dx9_25.dll loaded at runtime via LoadLibraryA")
# R9: header-derived ID3DXFont slots (real Microsoft d3dx9core.h SDK43 +
# mingw-w64/ReactOS/Wine all agree): DrawTextA=14, DrawTextW=15, OnLost=16,
# OnReset=17 — and there is NO Begin / NO End on ID3DXFont (slot 13 is
# PreloadTextW), so the old Begin=12/DrawTextA=13/End=15 table is banned.
check("FONT_DRAWTEXTA    14" in src, "R9: ID3DXFont DrawTextA=14 vtable slot")
check("FONT_ONLOSTDEVICE 16" in src, "ID3DXFont OnLostDevice=16 vtable slot")
check("FONT_ONRESETDEVICE 17" in src, "ID3DXFont OnResetDevice=17 vtable slot")
check("FONT_BEGIN" not in src and "FONT_END" not in src,
      "R9: no Begin/End slots — ID3DXFont has neither (Begin/End are ID3DXSprite)")
check("vt[FONT_DRAWTEXTA]" in src, "R9: text drawn ONLY through the DrawTextA(14) slot")
check("dt(font, NULL, s, -1, &rc" in src,
      "R9: DrawTextA called with the real (pSprite=NULL, pString, Count, rect, fmt, color) layout")
check("slot 13 is PreloadTextW" in src, "R9: PreloadTextW-at-13 hazard documented in ui.c")
check("D9_TESTCOOPLEVEL  3" in src, "TestCooperativeLevel device slot 3")
check("D9_RESET          16" in src, "Reset device hook slot 16")
check("D9_BEGINSCENE     41" in src, "BeginScene device slot 41")
check("D9_ENDSCENE       42" in src, "EndScene device slot 42")
check("D9_GETRENDERTARGET 38" in src, "GetRenderTarget device slot 38")
check("D9_DRAWPRIMITIVEUP 83" in src, "DrawPrimitiveUp device slot 83")
check("reset_hook" in src and "ui_on_reset(" in src,
      "Reset hook invalidates the font (lazy re-create on present path)")
check("reset dev=%p" in src, "Reset hook logs the ACTUAL device (reset dev=%p)")
check("!g_version_ok" in src and 'ovl_abort_stop("version")' in src,
      "ui_draw is disabled when the version gate fails")

# ================= R14: rate engine =================

check("ema_alpha" in src and "0.4f" in src and "0.1f" in src,
      "EMA alphas low/med/high (0.1/0.2/0.4) present")
check("discontinuity_ratio" in src, "spending-spike ratio present")
check("inst < 0.0f" in src, "spending-spike skip on negative rate")
check("g_slot_frozen" in src, "slot-level pause freeze present")
check("s_last_time" in src and "s_have_time" in src,
      "sampling cadence (interval gate) present")
check("now == s_last_time" in src, "global pause detected via tick not advancing")
check("SAMPLE_RING_SIZE" in src, "ring buffer defined")
check("QueryPerformanceCounter" in src, "realtime clock fallback present")

# ================= R14: settings =================

for sec in ("[General]", "[Display]", "[Rate]", "[Debug]"):
    check(sec in src, f"INI section {sec} present")
check('"[Rate]"' not in src or 'else if (!strcmp(section, "Rate"))' in src,
      "INI sections parsed in settings.c")
check("MoveFileExA(tmp, g_ini_path, MOVEFILE_REPLACE_EXISTING)" in src,
      "settings written back atomically (tmp + rename)")
check("DefaultProfile" in src, "DefaultProfile*.xml merge present")
check("ftLastWriteTime" in src, "profile mtime gate present")

# ================= R14: version gate =================

check("EXPECTED_EXE_SIZE" in src, "version gate checks exe size")
check("FindResourceA(me, MAKEINTRESOURCE(1), RT_VERSION)" in src,
      "version gate reads the PE version resource")
check("0xFEEF04BD" in src, "version gate scans VS_FIXEDFILEINFO")

# ================= Round 3: fault-safety semantics =================

check("AddVectoredExceptionHandler(0, vectored_fault_filter)" in src,
      "vectored handler registered (log-only, cannot be replaced)")
check("fault_code_benign" in src and "0x406D1388" in src,
      "benign notification 0x406D1388 (OutputDebugString) in the ignore list")
check("0xE06D7363" in src, "MSVC C++ exception 0xE06D7363 ignored at VEH level")
check("0x80000003" in src, "breakpoint 0x80000003 ignored at VEH level")
n_veh = src.count("vectored_fault_filter")
check(n_veh >= 2, "vectored_fault_filter defined AND registered")
check("vectored_fault_filter" in src, "vectored fault filter present")
# log-only VEH: CONTINUE_SEARCH and NO ExitProcess within vectored_fault_filter
# (operate on `code`, the comment-stripped TU, so doc text can't false-positive)
i_veh = code.find("long WINAPI vectored_fault_filter")
i_veh_end = code.find("long WINAPI fault_filter")
seg = code[i_veh:i_veh_end] if (i_veh != -1 and i_veh_end != -1 and i_veh < i_veh_end) else code
check(seg.count("ExitProcess") == 0,
      "vectored filter is LOG-ONLY (no ExitProcess inside)")
check("EXCEPTION_CONTINUE_SEARCH" in seg,
      "vectored filter returns EXCEPTION_CONTINUE_SEARCH")
check("fault_filter" in src and "ExitProcess" in src,
      "fatal exit lives on the legacy unhandled-filter path only")

# ================= Round 4: post-Reset cooldown + draw ordering =================

check("RESET_COOLDOWN_FRAMES" in src, "RESET_COOLDOWN_FRAMES defined (state.h)")
check("g_frames_since_reset" in src and "g_frames_since_reset++" in src,
      "present path increments g_frames_since_reset every frame (common body ends)")
check("g_frames_since_reset = 0;" in src and src.count("g_frames_since_reset = 0;") >= 3,
      "counter zeroed on CreateDevice / CreateDeviceEx / Reset")
check("g_font == NULL) return;" in src, "ui_draw guards font-first before RT work")
check("g_frames_since_reset < RESET_COOLDOWN_FRAMES" in src,
      "ui_draw waits out the post-Reset cooldown before RT setup")
# set-step ordering inside ui_draw: the RT breadcrumbs must come AFTER the
# cooldown check (ovl-grt = first RT call, replaces the coarse ovl-rt)
i_rt = src.find("ovl-grt")
i_cd = src.find("g_frames_since_reset < RESET_COOLDOWN_FRAMES")
check(i_rt != -1 and i_cd != -1 and i_cd < i_rt,
      "cooldown check precedes the ovl-grt (GetRenderTarget) step")
check('dlog("UI: font created size=%d face=%s font=%p"' in src,
      "font creation logged on EVERY create with the font pointer (post-Reset path visible)")

# ================= Round 7: font lifecycle — never create inside Reset =================
# Crash forensics: round-6 build drew overlay again but died at ovl-panel with
# eip=000000D8 (a virtual call through a NULL/dangling object) on BOTH runs. The
# font was created inside reset_hook while the device is still NOTRESET ->
# D3DXCreateFontA returned a broken object / D3DXFont died on the first real
# DrawText. Fix: font_destroy (strict NULL), NO creation in reset_hook, LAZY
# re-create on the present path guarded by TestCooperativeLevel == OK, and a
# VirtualQuery double-guard right before the font's Begin.

# (1) font_destroy is the only free path and clears g_font immediately
check("font_destroy" in src and "font_destroy(void)" in src,
      "font_destroy helper exists (the only way the font is freed)")
i_d = src.find("static void font_destroy(void)")
i_e = src.find("void ui_init(void)", i_d)
seg = src[i_d:i_e] if (i_d != -1 and i_e != -1 and i_d < i_e) else ""
check('g_font = NULL;' in seg and 'rel(g_font)' in seg,
      "font_destroy Releases via the font vtable AND NULLs g_font immediately")
# (2) reset_hook must NOT create the font (device is NOTRESET during Reset)
i_r = code.find("reset_hook(void *self, const void *pp) {")
i_rb = code.find("set_step(\"reset-done\")", i_r)
rseg = code[i_r:i_rb] if (i_r != -1 and i_rb != -1 and i_r < i_rb) else code
check("ui_create_font" not in rseg and "D3DXCreateFontA" not in rseg,
      "reset_hook never creates the font (lazy re-create only)")
check("ui_on_reset(self)" in rseg, "reset_hook still calls ui_on_reset (invalidate + log)")
# (3) lazy create in the present path AFTER TestCooperativeLevel == OK
i_create = code.find("ui_create_font(dev);")
i_tcl = code.find("0x88760869u")
check(i_create != -1 and i_tcl != -1 and i_tcl < i_create,
      "lazy font creation happens AFTER the TCL==OK check in ui_draw")
check('if (g_font == NULL) {' in code and "ui_create_font(dev);" in code,
      "font created only when g_font == NULL on the present path")
check("if (g_font == NULL) return;" in code,
      "font-first guard kept: create failure skips the frame")
# (4) honest create log (never claim success on failure)
check('UI: font create FAILED hr=0x%08X' in src, "failed create logged as FAILED")
# (5) VirtualQuery double-guard right before the font Begin
check("font_safe" in src and "VirtualQuery(g_font" in src and "VirtualQuery(vt" in src,
      "panel re-verifies font object + vtable are mapped readable before Begin")

# ================= Round 6: revert per-vtable originals; drop RT-switch =================
# Crash forensics: FAULT breadcrumb=ovl-srt (SetRenderTarget) with garbage
# dev=6F8F4EC0 vt=0 slot=37 pointed at the round-5 per-vtable machinery + the
# GetBackBuffer/SetRenderTarget switch. Round 6 reverts to the proven
# single-global-original pair and removes srt/gbb from the draw path.

# d3d9.c (code): single-slot originals saved once, no per-vtable table anywhere
check("s_ovtab" not in code and "OVTAB_MAX" not in code and "resolve_orig_present" not in code
      and "resolve_orig_reset" not in code,
      "round-5 per-vtable originals table + resolve_orig_* REMOVED from d3d9.c")
check("static PRESENT_FN s_orig_present = NULL;" in code.replace("  "," ") or
      "s_orig_present = NULL;" in code,
      "single global s_orig_present restored (saved once)")
check('if (s_orig_present == NULL)' in code,
      "original Present saved ONCE from the first patched vtable (R6 guard)")
check('if (s_orig_reset == NULL)' in code and 'ui_on_reset(self)' in code,
      "original Reset saved once; reset re-assert logs the ACTUAL device")
# ui.c (src): no srt/gbb anywhere in the draw path (check the comment-stripped
# TU so my own explanatory doc text can't false-positive)
check("ovl-srt" not in code and "ovl-gbb" not in code,
      "SetRenderTarget/GetBackBuffer breadcrumbs (ovl-srt/ovl-gbb) REMOVED")
check("D9_SETRENDERTARGET" not in src and "D9_GETBACKBUFFER" not in src,
      "no SetRenderTarget/GetBackBuffer vtable use remains in ui.c")
check('grt(dev, 0, &cur) != 0' in src or 'grt(dev, 0, &cur) != 0)' in src,
      "GetRenderTarget hard-checked (hr==0)")
check("cur == NULL" in src, "GetRenderTarget surface NULL-checked")
check("VirtualQuery(cur" in src, "GetRenderTarget surface VirtualQuery-guarded before use")
check("ovl-begin" in src and "ovl-end" in src and "ovl-restore" in src,
      "draw breadcrumbs chain kept (begin/end/restore; no srt)")
check("ovl-tcl" in src and "ovl-grt" in src,
      "TCL + GetRenderTarget breadcrumbs retained")
# zero-touch Enabled path: the SHARED overlay body (R15) forwards before work
# (both present_hook and sw_present_hook defer to overlay_present_common, whose
# FIRST branch is the Enabled=0 zero-touch forward, before observer/overlay)
i_oc = code.find("static int overlay_present_common(void *self, int is_sw)")
i_e = code.find("!g_settings.enabled", i_oc)
i_l = code.find("locate_resources()", i_e)
check(i_oc != -1 and i_e != -1 and i_l != -1 and i_e < i_l,
      "Enabled=0 zero-touch: common overlay body forwards before observer/overlay work")
check("ContextRecord->Eip" in src and "ContextRecord->Esp" in src and "ContextRecord->Ebp" in src,
      "fault dump reads eip/esp/ebp from the exception CONTEXT")
check('"ovl first draw ok frame=%d"' in src, "first-draw marker logged once")

# ================= ROUND 8: inspector + external-review fixes =================
# Part A (external review): A1 realtime clock (s_tick is a FRAME counter, not
# game-time ms — game-time-based rates scaled with FPS), A2 explicit EMA
# liveness flag, A3 RES lines gated behind DebugEnabled, A4 profile re-poll
# once per second from present_hook, A5 log (never silently skip) a second
# device vtable.
# Part B (inspector): B1 font bound to a device, B2 hotkeys need F9 held +
# foreground window + per-key edge, B3 font_destroy only Releases a font_safe()
# pointer, B4 font-failure log throttled to 1 + heartbeat/300, B5 only S_OK
# proceeds past TestCooperativeLevel, B6 SetFVF/GetFVF (slots 89/90) saved +
# restored around the panel backdrop, B7 noreentrancy tripwire in present_hook,
# B8 ShowGains=0 skips positive jumps like spend spikes, B9 StartHidden.

# ---- Part A ----
check("g_ema_valid" in src, "A2/A8: g_ema_valid liveness flags present")
check("g_ema_valid[s] = 1;" in code, "A2: EMA marked valid on bootstrap")
check("g_ema_valid[s] = 0;" in code, "A2: liveness flags cleared on tracker_reset")
i_rates = code.find('logger_debug("rates food=')
i_gate = code.rfind("if (g_settings.debug_enabled) {", 0, i_rates)
i_res = code.find('dlog("RES t=%d food=%d wood=%d coin=%d export=%d player=%08X res=%08X"')
check(i_gate != -1 and i_res != -1 and i_gate < i_res and (i_res - i_gate) < 800,
      "A3: RES line sits INSIDE the DebugEnabled-gated block (quiet log by default)")
check("settings_poll_profile" in src, "A4: settings_poll_profile declared + defined")
check("profile-poll" in src, "A4: present_hook breadcrumbs the profile poll step")
check("settings_poll_profile();" in code, "A4: present_hook calls settings_poll_profile()")
check("now - s_poll_last) < 1000) return;" in code,
      "A4: profile poll throttled to once per second (GetTickCount)")
check("second vtable %p seen" in src,
      "A5: second-device-vtable case is LOGGED (not silently skipped)")
check("not re-patched" in src, "A5: single patched vtable semantics preserved + explained")
# A1: clock_now() must NEVER hand back the s_tick frame counter — tighten the
# old `return (DWORD)s_tick not in code` (a `return s_tick;` regression slips
# through) by requiring the clock_now BODY itself to be free of the identifier.
i_clock = code.find("static DWORD clock_now(void)")
i_cnext = code.find("static void tracker_store_sample", i_clock)
cseg = code[i_clock:i_cnext] if (i_clock != -1 and i_cnext != -1 and i_clock < i_cnext) else ""
check("QueryPerformanceCounter" in cseg and "s_tick" not in cseg,
      "A1: clock_now body uses realtime QPC and contains NO s_tick identifier")
check("clock: using realtime QPC (s_tick is a frame counter, not game time)" in src,
      "A1: one-time realtime-clock note logged in settings_load")

# ---- Part B ----
check("g_font_dev" in src, "B1: font binds to its device (g_font_dev)")
check("g_font_dev == dev) return;" in code, "B1: bound font reused only for the SAME device")
check("(GetAsyncKeyState(VK_F9) & 0x8000)" in code,
      "B2: panel keys require F9 held (modifier)")
check("ui_is_foreground" in code and "GetForegroundWindow" in code and
      "GetWindowThreadProcessId" in code,
      "B2: keys gated on the game window being foreground")
check("ui_key_edge(keys[i], i)" in code, "B2: per-key edge-trigger kept for 1-6")
check("if (font_safe())" in code, "B3: font_destroy Releases only a font_safe() pointer")
check("still failing" in code, "B4: font-failure heartbeat (once/300) present")
check("s_font_fail_n == 1" in code, "B4: FAILED line logged only on the FIRST failure")
check("uhr != 0) return;" in code, "B5: only S_OK proceeds past TestCooperativeLevel")
check("D9_SETFVF         89" in src and "D9_GETFVF         90" in src,
      "B6: SetFVF=89 / GetFVF=90 canonical vtable slots")
check("saved_fvf" in code and "gf(dev, &saved_fvf)" in code and "sf(dev, saved_fvf)" in code,
      "B6: backdrop saves + restores the device FVF around DrawPrimitiveUp")
check("ovl-fvf" in src, "B6: FVF calls breadcrumbed (ovl-fvf)")
# R9: pin the FVF ORDER inside the backdrop helper — save < draw < restore
i_back = code.find("static void ui_draw_backdrop(void *dev")
i_backend = code.find("static void ui_font_draw", i_back)
bseg = code[i_back:i_backend] if (i_back != -1 and i_backend != -1 and i_back < i_backend) else ""
i_save = bseg.find("gf(dev, &saved_fvf)")
i_draw = bseg.find("du(dev, D3DPT_TRIANGLESTRIP")
i_rest = bseg.find("sf(dev, saved_fvf)")
check(i_save != -1 and i_draw != -1 and i_rest != -1 and i_save < i_draw < i_rest,
      "B6: FVF order pinned — save(GetFVF) < DrawPrimitiveUp < restore(SetFVF)")
check("InterlockedCompareExchange" in code and "present-reentrant" in src,
      "B7: re-entrancy tripwire guards the overlay body (nested Present forwarded raw)")
check("InterlockedExchange(&g_in_present, 0)" in code,
      "B7: tripwire cleared on the single exit path")
# R9: pin the overlay-body no-early-return invariant — between the tripwire SET
# and the CLEAR, the ONLY `return` is inside the reentrant branch (which never
# holds the token); the overlay body cannot bail without clearing the flag.
i_set = code.find("InterlockedCompareExchange(&g_in_present, 1, 0)")
i_clr = code.find("InterlockedExchange(&g_in_present, 0)")
pseg = code[i_set:i_clr] if (i_set != -1 and i_clr != -1 and i_set < i_clr) else ""
ib = pseg.find("{")
ie = pseg.find("}", ib)
pmain = (pseg[:ib] + pseg[ie+1:]) if (ib != -1 and ie != -1) else pseg
check(i_set != -1 and i_clr != -1 and i_set < i_clr and "return" not in pmain,
      "B7: no early return between tripwire-set and clear while holding the token")
check("!g_settings.show_gains" in code, "B8: ShowGains=0 skips positive jumps (EMA frozen)")
check("inst > ref * ratio" in code,
      "B8: positive-jump skip uses the ratio threshold (R9: against the clamped ref)")
check("if (g_settings.start_hidden) g_panel_visible = 0;" in code,
      "B9: StartHidden hides the panel at load in DllMain")
check("g_panel_visible = 0;" in code.split("settings_init")[-1] or
      "start_hidden) g_panel_visible = 0;" in code,
      "B9: start_hidden applied AFTER settings_load (init order preserved)")

# ================= ROUND 9: font vtable truth + drained-slot clamp =================
# Finding 1: the R14 "font slots" were wrong. The REAL ID3DXFont (Microsoft
# d3dx9core.h, SDK 43 — verified + reported in the round-9 message; mingw-w64/
# ReactOS/Wine agree) is: ... 12 PreloadTextA 13 PreloadTextW 14 DrawTextA 15
# DrawTextW 16 OnLostDevice 17 OnResetDevice. There is NO Begin/No End on
# ID3DXFont, so "Begin=12 / DrawTextA=13 / End=15" was shipping garbage
# (PreloadTextA / PreloadTextW / DrawTextW) into a live font — crash-or-no-text.
# Fix: DrawTextA(14) only, with the real 7-arg layout (pSprite=NULL).
check("FONT_DRAWTEXTA    14" in src, "R9: DrawTextA slot firmly 14")
check("FONT_BEGIN" not in src and "FONT_END" not in src,
      "R9: Begin/End slot macros FORBIDDEN (they do not exist on ID3DXFont)")
check("vt[FONT_END]" not in src and "vt[15]" not in src and "vt[FONT_BEGIN]" not in src,
      "R9: no slot-15 'End' / slot-12 'Begin' dispatch anywhere in ui.c")
# Finding 2: font is bound ONLY to a device whose Present hook was actually
# installed (CreateDevice/CreateDeviceEx gate on the patch result).
check("if (patched) ui_create_font(*ppdev);" in code,
      "R9: font created only when patch_device_present returned 1 (both create paths)")
check(code.count("if (patched) ui_create_font(*ppdev);") == 2,
      "R9: patch-gated font creation in BOTH CreateDevice and CreateDeviceEx")
# Finding 3: ShowGains=0 drain-clamp — EMA reference clamped to >= 0 so a
# drained slot cannot match every positive sample forever.
check("g_last_ema[s] > 0.0f ? g_last_ema[s] : 0.0f" in code,
      "R9: ShowGains skip clamps the EMA reference to >= 0 (drained slots recover)")
check("g_ema_valid" in code, "R9 re-check: explicit EMA liveness remains (A2)")
check("ovl-panel-text" in src, "R9: font DrawTextA calls breadcrumbed (ovl-panel-text)")

# ================= ROUND 10: ARMED/DISABLED status + GDI fallback + heartbeat =================
# P0 of the spec-gap closure: status line (once/load), red GDI fallback when the
# overlay is DISABLED, and a 600-frame heartbeat proving the draw path completes
# long after first frame.

# 1) status line: emitted EXACTLY once, from DllMain's DLL_PROCESS_ATTACH,
#    between the version gate and the dllmain-done breadcrumb.
check("OVERLAY ARMED" in src and "OVERLAY DISABLED:" in src,
      "R10: ARMED / DISABLED status strings present in d3d9.c")
i_vg = src.find("version_gate_check();")
i_dd = src.find('set_step("dllmain-done")')
i_st = src.find("overlay_status_log();")
check(src.count("overlay_status_log();") == 1 and i_vg != -1 and i_st != -1 and i_dd != -1
      and i_vg < i_st < i_dd,
      "R10: overlay_status_log() called EXACTLY once, inside DLL_PROCESS_ATTACH (gate < call < done)")
i_ph = code.find("static int STDMETHODCALLTYPE present_hook(void *self")
i_ph_end = code.find("static int s_second_vt_logged", i_ph)
pbody = code[i_ph:i_ph_end] if (i_ph != -1 and i_ph_end != -1 and i_ph < i_ph_end) else ""
i_res = code.find("static int STDMETHODCALLTYPE reset_hook(void *self, const void *pp)")
i_res_end = code.find("static int STDMETHODCALLTYPE present_hook(void *self", i_res + 1)
rbody = code[i_res:i_res_end] if (i_res != -1 and i_res_end != -1 and i_res < i_res_end) else ""
i_cre = code.find("static int STDMETHODCALLTYPE w_create_device(void *self, UINT adapter, UINT type,")
i_cre_end = code.find("static int STDMETHODCALLTYPE w_query_interface", i_cre)
cbody = code[i_cre:i_cre_end] if (i_cre != -1 and i_cre_end != -1 and i_cre < i_cre_end) else ""
check(all(("OVERLAY" not in b and "overlay_status_log" not in b and "overlay_disabled_reason" not in b)
          for b in (pbody, rbody, cbody)),
      "R10: no OVERLAY/status references inside present_hook/reset_hook/w_create_device bodies")
check('"OVERLAY ARMED%s"' in src and "(ini missing: using defaults)" in src,
      "R10: ARMED-with-ini-note variant present (ini missing is a suffix, never a disable)")
check("g_ini_missing = 1;" in code, "R10: g_ini_missing set in settings_load's fopen-failure branch")

# 2) pure helper + public wrapper; precedence version-first then d3dx9.
check("overlay_state_reason(" in code, "R10: pure overlay_state_reason helper present")
check('"d3dx9_25.dll not loadable"' in src,
      "R10: d3dx9-disable reason string present")

# 3) GDI fallback: defined in ui.c, dependency-free (dynamic gdi32), drawn AFTER
#    the tripwire clear, body contains NO vtable-slot dispatch. The tripwire +
#    fallback live in the SHARED overlay body (overlay_present_common, R15) —
#    one code path for both Device::Present and SwapChain::Present.
check("int ui_gdi_fallback_draw(void)" in code, "R10: ui_gdi_fallback_draw defined in ui.c")
i_oc = code.find("static int overlay_present_common(void *self, int is_sw)")
i_oc_end = code.find("static int STDMETHODCALLTYPE sw_present_hook", i_oc)
pcbody = code[i_oc:i_oc_end] if (i_oc != -1 and i_oc_end != -1 and i_oc < i_oc_end) else ""
i_gdi_call = pcbody.find("ui_gdi_fallback_draw();")
i_clrp = pcbody.find("InterlockedExchange(&g_in_present, 0)")
check(i_gdi_call != -1 and i_clrp != -1 and i_clrp < i_gdi_call,
      "R10: ui_gdi_fallback_draw() called AFTER the tripwire clear in the common body")
i_gd = code.find("int ui_gdi_fallback_draw(void)")
gbody = code[i_gd:] if i_gd != -1 else ""
check('LoadLibraryA("gdi32.dll")' in src and "GetProcAddress(" in gbody,
      "R10: gdi32 loaded DYNAMICALLY (SetTextColor/SetBkColor/SetBkMode via GetProcAddress)")
with open(os.path.join(ROOT, "build", "build.bat"), "r", encoding="utf-8", errors="replace") as bat:
    bb = bat.read()
check("-lgdi32" not in bb, "R10: build.bat does NOT link gdi32 statically (import table stays clean)")

# 4) vtable-slot constants: device D9_* and font FONT_* pinned EXACTLY (R15/R16
# canonical slots — 14 GetSwapChain + 3 SwapChain::Present hooked the game's
# swapchain; R16 adds 13 CreateAdditionalSwapChain, the OTHER acquisition path
# — the R15 live log showed zero GetSwapChain calls, so in-match swapchains are
# either created here or live on a second device (R16 device-count tracer)).
d9mac = set(int(v) for v in re.findall(r"#define D9_\w+\s+(\d+)", src))
ftmac = set(int(v) for v in re.findall(r"#define FONT_\w+\s+(\d+)", src))
check(d9mac == {3, 13, 14, 16, 38, 41, 42, 57, 58, 83, 89, 90},
      "R16: slot constants pinned EXACTLY {3(SW Present),13(CreateAdditionalSwapChain),14(GetSwapChain),16,38,41,42,57,58,83,89,90}")
check(ftmac == {14, 16, 17},
      "R10: font vtable slots pinned EXACTLY {14,16,17} (no additions)")
check("vt[" not in gbody, "R10: GDI fallback body contains NO device-vtable dispatch (vt[)")

# 5) heartbeat: after the first-draw marker, before the ovl-done breadcrumb.
check("OVL_HEARTBEAT_FRAMES" in src and "#define OVL_HEARTBEAT_FRAMES" in open(
        os.path.join(ROOT, "src", "state.h"), "r", encoding="utf-8", errors="replace").read(),
      "R10: OVL_HEARTBEAT_FRAMES defined (state.h) and used in ui.c")
i_fd = src.find('"ovl first draw ok frame=%d"')
i_hb = src.find("ovl heartbeat n=%u")
i_od = src.find('set_step("ovl-done")')
check(i_fd != -1 and i_hb != -1 and i_od != -1 and i_fd < i_hb < i_od,
      "R10: heartbeat block sits between the first-draw marker and the ovl-done step")

# ================= ROUND 11: settings gap + panel layout + version table =================
# P0/P1 spec-gap closure: per-resource visibility, decimal places, plus sign,
# position modes + hotkeys, a two-line hint bar, and a multi-version gate table.

# 1) version table in d3d9.c, initialized from the EXPECTED_* macros; TODO rows.
check("const struct ver_entry g_versions[]" in src,
      "R11: g_versions[] present with struct ver_entry")
check("TAD 1.0.8" in src, "R11: TAD 1.0.8 row label present")
check(src.count("TODO: capture") >= 2, "R11: >=2 not-yet-captured TODO rows in the table")
i_rowref = src.find("struct ver_entry *row = NULL;")
check(i_rowref != -1 and src.find("row->base", i_rowref) != -1,
      "R11: version_gate_check selects a row by size then reads row->base")
check('"exe size=%lu expected=%lu"' in src and '"base=%08lX expected=%08lX"' in src and
      '"pe ver=%u.%u.%u.%u expected=%u.%u.%u.%u"' in src,
      "R11: the three version-gate reason strings retained verbatim")
with open(os.path.join(ROOT, "src", "state.h"), "r", encoding="utf-8", errors="replace") as fsh:
    stateh = fsh.read()
for _m in ("EXPECTED_EXE_SIZE", "EXPECTED_IMAGE_BASE", "EXPECTED_PE_VER_HI",
           "EXPECTED_PE_VER_LO", "EXPECTED_PE_VER_R", "EXPECTED_PE_VER_B"):
    check(_m in stateh, f"R11: {_m} still defined in state.h")
check("extern DWORD g_bb_w" in stateh and "extern DWORD g_bb_h" in stateh,
      "R11: g_bb_w/g_bb_h extern'd in state.h")
check("DWORD g_bb_w = 0;" in src and "DWORD g_bb_h = 0;" in src,
      "R11: g_bb_w/g_bb_h defined in d3d9.c")


def _fn_body(sig):
    """Return the brace-balanced body of the DEFINITION of `sig` (the part of
    the signature AFTER 'static int STDMETHODCALLTYPE '), or None. Requires the
    '{' right at the end of the signature so a forward DECLARATION (which ends
    in ';') is never mistaken for the definition."""
    m = re.search(r"static int STDMETHODCALLTYPE " + re.escape(sig) + r"\s*\{", src)
    if not m:
        return None
    i = m.end() - 1                      # points at the '{'
    depth = 0
    for k in range(i, len(src)):
        if src[k] == "{":
            depth += 1
        elif src[k] == "}":
            depth -= 1
            if depth == 0:
                return src[i:k + 1]
    return None


# R12: g_bb_w capture must appear inside EACH specific body (the weak
# count>=3 only proved "some 3 places exist"). reset_hook reads `pp`, the
# create paths read `pparams` — pin each function individually.
for _sig, _fn in (("w_create_device(void *self, UINT adapter, UINT type,\n"
                   "        HWND focus, DWORD flags, void *pparams, void **ppdev)",
                   "w_create_device"),
                  ("w_create_device_ex(void *self, UINT adapter, UINT type,\n"
                   "        HWND focus, DWORD flags, void *pparams, void *pfs, void **ppdev)",
                   "w_create_device_ex"),
                  ("reset_hook(void *self, const void *pp)", "reset_hook")):
    _body = _fn_body(_sig)
    check(_body is not None and "g_bb_w = " in _body and "g_bb_h = " in _body,
          f"R12: g_bb_w/g_bb_h captured inside {_fn}")
check("PANEL_LINE_MAX_CHARS" in stateh and "PANEL_LINE_MAX_CHARS" in src,
      "R11: PANEL_LINE_MAX_CHARS defined (state.h) and used in ui.c")

# 2) settings: the 9 new keys — parsed (both paths) AND saved + documented.
newkeys = ["DecimalPlaces", "ShowPlusSign", "ShowResourceNames", "ShowZeroRates",
           "ShowFood", "ShowWood", "ShowCoin", "ShowExport", "PositionMode"]
for _k in newkeys:
    check(src.count(f'!strcmp(key, "{_k}")') >= 2 and f'"{_k}=' in src,
          f"R11: {_k} parsed (load + profile) AND saved in settings.c")
check('PositionMode=%s' in src, "R11: PositionMode saved as a string name")
check(re.search(r"s->decimal_places\s*=\s*1;", src) is not None,
      "R11: default decimal_places = 1")
check(re.search(r"s->position_mode\s*=\s*0;", src) is not None,
      "R11: default position_mode = 0 (Default)")
check(re.search(r"s->show_plus_sign\s*=\s*1;", src) is not None and
      re.search(r"s->show_resource_names\s*=\s*1;", src) is not None and
      re.search(r"s->show_zero_rates\s*=\s*1;", src) is not None,
      "R11: new show_* defaults on (1)")
check('"ShowHeader"' in src and '"ShowSlots567"' in src and '"DiscontinuityRatio"' in src,
      "R11: legacy keys ShowHeader/ShowSlots567/DiscontinuityRatio still parsed")
with open(os.path.join(ROOT, "ResourceRateMod.ini.example"), "r", encoding="utf-8",
          errors="replace") as fine:
    ini_ex = fine.read()
with open(os.path.join(ROOT, "config", "schema.md"), "r", encoding="utf-8",
          errors="replace") as fsc:
    schema = fsc.read()
for _k in newkeys:
    check(_k in ini_ex and _k in schema, f"R11: {_k} documented in ini.example + schema.md")

# 3) panel layout + hotkeys: Alt layer, edge array sized by sizeof, geometry.
check("VK_MENU" in src, "R11: Alt detected via VK_MENU")
check("!alt_down" in code, "R11: plain toggles blocked while Alt is held")
check("f9_down && fg && alt_down" in code, "R11: F9+Alt layer gated on foreground + Alt")
check("Alt+1:dps 2:plus 3:names 4:zeror 5:food 6:wood 7:coin 8:exp 9:pos" in src,
      "R11: two-line hint string present (F9+Alt reference)")
check("g_key_prev[17]" in src, "R11: edge-tracking array grown to 17")
check("sizeof(g_key_prev) / sizeof(g_key_prev[0])" in code,
      "R11: key-edge bounds driven by sizeof (no stale literal 8)")
check("ui_panel_geometry" in code and "position_mode" in stateh,
      "R11: panel geometry helper + position_mode setting present")
check('"Default"' in code and '"TopLeft"' in code and '"TopRight"' in code,
      "R11: position-mode names Default/TopLeft/TopRight rendered in settings.c")
check("format_rate_line" in stateh, "R11: format_rate_line declared in state.h")
check("int format_rate_line(char *out, size_t n, int slot_id" in code,
      "R11: format_rate_line defined non-static in ui.c")
# R12: the four main rows feed rate_get_ema(slot) through format_rate_line and
# pass NULL when names are off (FIX 1) so the helper's "Slot%d" fallback wins.
for _sl, _nm in ((2, "Food"), (1, "Wood"), (0, "Coin"), (7, "Export")):
    _patt = f'g_settings.show_resource_names ? "{_nm}" : NULL'
    check(_patt in code and
          f"format_rate_line(line, sizeof(line), {_sl}," in code and
          f"rate_get_ema({_sl})" in code,
          f"R12: row {_sl} ({_nm}) names-off->NULL via format_rate_line+rate_get_ema")

# R13: the one-shot render diagnostic is UNCONDITIONAL (primary invisibility
# diagnostic — one line per load even on DebugEnabled=0 deployments), so the
# marker..diag region must no longer reference the debug flag at all.
i_fd = src.find('"ovl first draw ok frame=%d"')
i_diag = src.find('ovl diag rt=%p rect=%ld,%ld:%ldx%ld alpha=0x%lX', i_fd)
check(i_fd != -1 and i_diag != -1, "R13: ovl diag line present after the first-draw marker")
check(i_fd != -1 and i_diag != -1 and "debug_enabled" not in src[i_fd:i_diag],
      "R13: ovl diag is NOT debug-gated (marker..diag region free of the gate)")
check("!g_ovl_first_done && g_settings.debug_enabled" not in code,
      "R13: old debug-gated diag guard literal removed")

# R13: silent-stop detectors — one draw-path abort logger (4 named stages,
# fires once at 60 consecutive aborts) and one reentrant-present logger, both
# non-debug-gated; both counters reset on a completed/normal frame.
check(code.count('ovl stop: %u consecutive frame aborts at stage=') == 1,
      "R13: draw-path stop detector message exactly once")
for stage in ("grt-missing", "grt-fail", "surface-bad", "guard-fail"):
    check(code.count('ovl_abort_stop("%s")' % stage) == 1,
          "R13: abort stage %s wired exactly once" % stage)
# R14: the silent-stop detector was extended to the six/eight PRE-RT gates
# (the R14 live blind spot — panel vanished after a clean diag with zero
# heartbeats AND zero RT-stage ovl stop, so a GATE was killing the draw every
# frame). Each gate stage must be wired exactly once; stage strings distinct
# from the four RT stages above.
for stage in ("enabled", "version", "panel", "values", "res", "device",
              "ui_ready", "cooldown"):
    check(code.count('ovl_abort_stop("%s")' % stage) == 1,
          "R14: gate stage %s wired exactly once" % stage)
check(code.count("ovl stop: reentrant-present for %u consecutive frames") == 1,
      "R13: reentrant-present stop detector exactly once")
check("s_ovl_abort_n = 0;" in code and "s_re_present_n = 0;" in code,
      "R13: both stop detectors reset on a completed/normal frame")

# R14 (FINDING A): the sample-cadence bug — `s_last_time = now` on the NOT-due
# tracker path reset the interval accumulator every frame, so `due` could only
# fire on the very first sample (live proof: exactly one RES t=1 line per 60s
# session). Pin: between `int due` and the DUE-path store (the first
# `s_last_time = now` after it), there must be NO further `s_last_time = now`.
_t_i = src.find("int due")
_t_store = src.find("s_last_time = now", _t_i)
check(_t_i != -1 and _t_store > _t_i and "s_last_time = now" not in src[_t_i:_t_store],
      "R14: no s_last_time reset between the cadence gate and the due-path store")

# ================= R14: modular layout =================

for mod in ("logger.c", "tracker.c", "rate.c", "gameif.c", "settings.c", "ui.c"):
    inc = f'#include "src/{mod}"'
    check(inc in src, f"d3d9.c includes src/{mod} (monolithic build entry)")

# ================= ROUND 15: swap-chain Present root-cause =================
# Slot truth (verified from the REAL mingw-w64 d3d9.h of this build toolchain):
#   IDirect3DDevice9 (header lines 1773-1781): 13 CreateAdditionalSwapChain,
#   14 GetSwapChain, 15 GetNumberOfSwapChains, 16 Reset, 17 Present,
#   18 GetBackBuffer.
#   IDirect3DSwapChain9 (header lines 307-323): 0 QueryInterface, 1 AddRef,
#   2 Release, 3 Present(const RECT*, const RECT*, HWND, const RGNDATA*,
#   DWORD flags) — NOTE the trailing DWORD flags Device::Present does not have,
#   4 GetFrontBufferData, 5 GetBackBuffer, 6 GetRasterStatus, 7 GetDisplayMode,
#   8 GetDevice, 9 GetPresentParameters.
check("D9_GETSWAPCHAIN 14" in src, "R15: GetSwapChain device slot 14 constant")
check("D9_SW_PRESENT    3" in src or "D9_SW_PRESENT 3" in src,
      "R15: SwapChain::Present slot 3 constant")
check("static int STDMETHODCALLTYPE w_get_swapchain" in code,
      "R15: device GetSwapChain wrapped (captures every swapchain)")
check('dlog("patch_swapchain: sw=%p vt=%p present=%p"' in src,
      "R15: one install log per swapchain (patch_swapchain: sw=.. vt=.. present=..)")
check(code.count('dlog("patch_swapchain: sw=%p vt=%p present=%p"') == 1,
      "R15: patch_swapchain install log defined exactly once")
check("vt[D9_SW_PRESENT] = (void *)sw_present_hook;" in code,
      "R15: swapchain vtable slot 3 patched to sw_present_hook")
check("if (s_orig_sw_present == NULL) s_orig_sw_present = o;" in code,
      "R15: original swapchain Present saved ONCE (R6 single-original semantics)")
check("s_orig_sw_present != NULL" in code or "s_orig_sw_present" in code,
      "R15: fallback forward uses the saved original swapchain Present")
check(code.count("overlay_present_common(") >= 2,
      "R15: BOTH present hooks call the SHARED common body (no code duplication)")
check("g_ovl_last_draw_frame" in code,
      "R15: same-frame double-call marker present (draw once per frame even when both Present kinds run)")
check("DWORD flags" in code and "fl(sw, a, b, hwnd, dirty, flags)" in code,
      "R15: swapchain hook forwards the trailing DWORD flags (stdcall frame intact)")

# R15 FINDING-analog: menu false-alarm suppression — values/res gate bursts are
# legal pre-match and must not trip the stop detector until a match started.
check("g_match_active" in code, "R15: g_match_active exists (match latch)")
check("g_match_active = 1;" in code and "g_match_active = 0;" in code,
      "R15: latch set on match_start, cleared when the chain breaks")
check('!strcmp(stage, "values")' in code and '!strcmp(stage, "res")' in code,
      "R15: values/res stages suppressed pre-match in ovl_abort_stop")
i_mg = src.find("g_match_active")
check(i_mg != -1 and src.find("match start n=%d", i_mg) != -1,
      "R15: match-start separator (where the latch is set) present")

# ================= ROUND 16/17: merged present-path + scene tracer =================
# R15 live log (build 61d8317d): ZERO `patch_swapchain:` lines while the match
# rendered on a dead device-Present hook => the game never called our wrapped
# IDirect3DDevice9::GetSwapChain -> in-match present is an UNBOUND path. R16
# instrumented both present kinds; R17 MERGED the line to
# `present-path dev=%u sw=%u scene=%u frames=%u` (one mod-300 site per hook,
# one shared tracer_tick), wrapped EndScene (slot 42) as the scene-alive probe,
# and EAGERLY captures the runtime's IMPLICIT swapchain at device create.
# Slot truth (R17 re-verified against the REAL mingw-w64 d3d9.h of this
# toolchain, IDirect3DDevice9 interface): BeginScene=41 (line 1244), EndScene
# =42 (line 1245) — ARGLESS stdcall (STDMETHOD(EndScene)(THIS)), device slots
# 13 CreateAdditionalSwapChain / 14 GetSwapChain / 16 Reset / 17 Present all
# unchanged -> the pinned d9mac set is unchanged: 42 was already pinned.
check("static unsigned int s_dev_presents" in code,
      "R16: device-present counter static at file scope")
check("static unsigned int s_sw_presents" in code,
      "R16: swapchain-present counter static at file scope")
check("static unsigned int s_scene_ends" in code,
      "R17: EndScene counter static at file scope")
check(code.count("s_dev_presents++;") == 1 and code.count("s_sw_presents++;") == 1
      and code.count("s_scene_ends++;") == 1,
      "R16/R17: counters incremented on exactly one path each")
i_dev_inc = code.find("s_dev_presents++;")
i_dev_cmn = code.find("overlay_present_common(self, 0);")
i_sw_inc = code.find("s_sw_presents++;")
i_sw_cmn = code.find("overlay_present_common(sw, 1);")
check(i_dev_inc != -1 and i_dev_cmn != -1 and i_dev_inc < i_dev_cmn,
      "R16: device counter increments BEFORE the enabled check (present_hook top)")
check(i_sw_inc != -1 and i_sw_cmn != -1 and i_sw_inc < i_sw_cmn,
      "R16: swapchain counter increments BEFORE the enabled check (sw_present_hook top)")
check(code.count('dlog("present-path dev=%u sw=%u scene=%u frames=%u"') == 1,
      "R17: merged tracer format defined EXACTLY once (shared tracer_tick)")
check(code.count("tracer_tick();") == 3,
      "R17: all three hooks call the single tracer tick (dev + sw + scene)")
check("(s_dev_presents + s_sw_presents + s_scene_ends) % 300" in code,
      "R17: tracer logs once per 300 COMBINED Dev/Sw/Scene calls (mod-300)")
check("s_dev_presents, s_sw_presents, s_scene_ends," in code
      and "(unsigned)g_frames_since_reset" in code,
      "R16/R17: tracer fields carry dev/sw/scene counters + the post-Reset frame counter")
check("D9_CASC         13" in src or "D9_CASC 13" in src,
      "R16: Device::CreateAdditionalSwapChain slot 13 constant")
check("static int STDMETHODCALLTYPE w_casc" in code,
      "R16: CreateAdditionalSwapChain wrapped (captures the OTHER acquisition)")
check("patch_swapchain(*sc)" in code,
      "R16: w_casc installs the swapchain slot-3 hook on the created object")
check("s_orig_casc = (DEV_CASC)vt[13];" in code and "vt[13] = (void *)w_casc;" in code,
      "R16: slot 13 saved once + patched inside patch_device_present")
check("s_n_dev_seen" in code,
      "R16: distinct-device-vtable counter present")
check('dlog("device-count: n_devices=%u"' in code,
      "R16: device-count line logged on the second-vtable path")
check("s_second_vt_logged = 0;" in code,
      "R16: second-vtable latch reset on process attach")
check("s_orig_casc = NULL;" in code and "s_dev_presents = 0;" in code
      and "s_sw_presents = 0;" in code and "s_scene_ends = 0;" in code
      and "s_orig_endscene = NULL;" in code,
      "R16/R17: tracer + EndScene statics reset on process attach")

# ================= ROUND 17: eager implicit-swapchain capture + EndScene probe =================
# The runtime creates an implicit swapchain AT DEVICE CREATION (no game call).
# Dali's R16 log (build ecca3c5f) froze the device Present hook exactly at
# match start (dev=7500, then silence) — in fullscreen the driver hands Present
# off to that implicit swapchain. R17 captures+patches it EAGERLY, on the
# game's behalf, right after patch_device_present — and wraps EndScene so the
# merged tracer proves whether the device is still alive when Present is not.
check("static int STDMETHODCALLTYPE w_endscene" in code,
      "R17: EndScene wrapped (the scene-alive probe)")
check("DEV_ENDSCENE o = s_orig_endscene;" in code and "o(self)" in code,
      "R17: w_endscene forwards to the saved original EndScene")
check("s_orig_endscene = (DEV_ENDSCENE)vt[D9_ENDSCENE];" in code
      and "vt[D9_ENDSCENE] = (void *)w_endscene;" in code,
      "R17: device slot 42 saved once + patched inside patch_device_present")
check("static void eager_implicit_swapchain" in code,
      "R17: eager implicit-swapchain capture helper defined")
check(code.count("eager_implicit_swapchain(*ppdev);") == 2,
      "R17: eager capture called from BOTH CreateDevice and CreateDeviceEx")
i_pd = code.find("int patched = patch_device_present(*ppdev);")
i_ea = code.find("if (patched) eager_implicit_swapchain(*ppdev);")
i_font = code.find("if (patched) ui_create_font(*ppdev);")
check(i_pd != -1 and i_ea != -1 and i_font != -1 and i_pd < i_ea < i_font,
      "R17: eager capture runs AFTER patch, BEFORE the R9 font bind (both paths)")
check("int shr = w_get_swapchain(dev, 0, &sw);" in code,
      "R17: eager capture calls the WRAPPED slot 14 (flows through R15 patch logic)")
check('dlog("swapchain-impl: sw=%p patched=%d"' in code
      and 'dlog("swapchain-impl: n/a hr=0x%08X"' in code,
      "R17: swapchain-impl line (patched / n/a) logged once per device")

print("SOURCE-CONTRACT:", "PASS" if failures == 0 else f"{failures} FAILURES")
sys.exit(0 if failures == 0 else 1)