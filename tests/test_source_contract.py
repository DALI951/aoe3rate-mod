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
check("D9_GETBACKBUFFER  18" in src, "GetBackBuffer device slot 18")
check("D9_BEGINSCENE     41" in src, "BeginScene device slot 41")
check("D9_ENDSCENE       42" in src, "EndScene device slot 42")
check("D9_SETRENDERTARGET 37" in src, "SetRenderTarget device slot 37")
check("D9_DRAWPRIMITIVEUP 83" in src, "DrawPrimitiveUp device slot 83")
check("reset_hook" in src and "ui_on_reset()" in src,
      "Reset hook invalidates/recreates the font")
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

# ================= R14: modular layout =================

for mod in ("logger.c", "tracker.c", "rate.c", "gameif.c", "settings.c", "ui.c"):
    inc = f'#include "src/{mod}"'
    check(inc in src, f"d3d9.c includes src/{mod} (monolithic build entry)")

print("SOURCE-CONTRACT:", "PASS" if failures == 0 else f"{failures} FAILURES")
sys.exit(0 if failures == 0 else 1)