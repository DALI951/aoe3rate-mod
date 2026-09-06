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
    runtime (no import-table dep on d3dx9*); ID3DXFont vtable Begin 12 /
    DrawTextA 13 / End 15 / OnLostDevice 16 / OnResetDevice 17.
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

# present_hook must run observer -> hotkey -> overlay -> original Present
i_obs = src.find("observer_sample();")
i_ui = src.find("ui_draw();")
i_orig = src.find("s_orig_present(self, a, b, hwnd, dirty)")
check(i_obs != -1, "present_hook calls observer_sample()")
check(i_ui != -1, "present_hook calls ui_draw()")
check(i_orig != -1, "present_hook still forwards the original Present")
check(i_obs != -1 and i_ui != -1 and i_orig != -1 and
      i_obs < i_ui < i_orig,
      "present_hook order: observer -> hotkey -> overlay -> original Present")
check(src.find("set_step(\"observer-sample\")") < src.find("set_step(\"draw-overlay\")") < i_orig,
      "observer (read) runs strictly before the overlay flip")
check("ui_check_hotkey()" in src, "present_hook checks the F9/settings hotkeys")

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
check("FONT_BEGIN        12" in src, "ID3DXFont Begin=12 vtable slot")
check("FONT_DRAWTEXTA    13" in src, "ID3DXFont DrawTextA=13 vtable slot")
check("FONT_ONLOSTDEVICE 16" in src, "ID3DXFont OnLostDevice=16 vtable slot")
check("FONT_ONRESETDEVICE 17" in src, "ID3DXFont OnResetDevice=17 vtable slot")
check("D9_TESTCOOPLEVEL  3" in src, "TestCooperativeLevel device slot 3")
check("D9_RESET          16" in src, "Reset device hook slot 16")
check("D9_BEGINSCENE     41" in src, "BeginScene device slot 41")
check("D9_ENDSCENE       42" in src, "EndScene device slot 42")
check("D9_GETRENDERTARGET 38" in src, "GetRenderTarget device slot 38")
check("D9_DRAWPRIMITIVEUP 83" in src, "DrawPrimitiveUp device slot 83")
check("reset_hook" in src and "ui_on_reset(" in src,
      "Reset hook invalidates the font (lazy re-create on present path)")
check("reset dev=%p" in src, "Reset hook logs the ACTUAL device (reset dev=%p)")
check("g_version_ok) return;" in src or "if (!g_version_ok) return;" in src,
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
      "present_hook increments g_frames_since_reset every frame")
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
# zero-touch Enabled path: present_hook forwards before ANY other work
i_p = code.find("int STDMETHODCALLTYPE present_hook")
i_e = code.find("!g_settings.enabled", i_p)
i_l = code.find("locate_resources()", i_p)
check(i_p != -1 and i_e != -1 and i_l != -1 and i_e < i_l,
      "Enabled=0 zero-touch: present_hook forwards before observer/overlay work")
check("ContextRecord->Eip" in src and "ContextRecord->Esp" in src and "ContextRecord->Ebp" in src,
      "fault dump reads eip/esp/ebp from the exception CONTEXT")
check('"ovl first draw ok frame=%d"' in src, "first-draw marker logged once")

# ================= R14: modular layout =================

for mod in ("logger.c", "tracker.c", "rate.c", "gameif.c", "settings.c", "ui.c"):
    inc = f'#include "src/{mod}"'
    check(inc in src, f"d3d9.c includes src/{mod} (monolithic build entry)")

print("SOURCE-CONTRACT:", "PASS" if failures == 0 else f"{failures} FAILURES")
sys.exit(0 if failures == 0 else 1)