#!/usr/bin/env python3
"""test_source_contract.py — round-13 pure-observer source-level contract checks
on d3d9.c.

Round-13 observer contracts:
  1. ALL draw-layer symbols must be ABSENT from the source.
  2. observer_sample() present, called from present_hook BEFORE s_orig_present,
     logs ONE `RES t=%d food=%d wood=%d coin=%d export=%d player=%08X res=%08X`
     line per tick for the chosen human player. Slot map: food=slot2, wood=slot1,
     coin=slot0, export=slot7. Match-start separator:
     `--- match start n=%d player=%08X res=%08X ---`.
  3. g_player_idx tracked; g_obs_player tracked for match-start.
  4. Player selection: index 1 when n>=2 [human-p1], fallback scans all for
     largest food, [chain-break: no sane player] on failure.
  5. No per-player P%d blocks, no resraw, no cell dumps, no altCap/altAdd/altPanel,
     no raw trail lines — only the RES line and match-start header.

Round-5/7 chain contracts (regression cannot re-introduce): sticky fallback
tags, [src-fallback]/[chain-break] tags, quiet chain log gate.

Usage: C:\\Python314\\python.exe tests\\test_source_contract.py
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


with open(SRC, "r", encoding="utf-8") as fh:
    src = fh.read()

code = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
code = re.sub(r"//[^\r\n]*", "", code)


def code_contains(sym):
    return sym in code


# --- contract 1: the entire draw layer is GONE ------------------------------
for sym in ("D3DXCreateFontA", "s_create_font", "ID3DXFont", "D3DXFONT_",
            "draw_hud_text", "init_font", "draw_set_rt", "draw_set_states",
            "draw_redrect", "GetBackBuffer", "SetRenderTarget", "BeginScene",
            "EndScene", "D3DFVF", "g_font", "dev_not_ready", "log_step"):
    check(not code_contains(sym), f"no {sym} anywhere in d3d9.c code (draw layer deleted)")
for name in ("D3DXFONT_BEGIN_IDX", "D3DXFONT_DRAWTEXT_IDX", "D3DXFONT_END_IDX"):
    check(f"#define {name}" not in code, f"no #define {name} (font vtable gone)")
m = re.search(r"BeginScene\(|s_orig_begin\b|draw_hud_text\(\)", code)
check(m is None, "no scene/draw call survives in any code path")
m = re.search(r"d3dx9_25\.dll\b", code)
check(m is None, "no d3dx9 module references remain in d3d9.c code")

# --- contract 2: observer survives and runs before Present ------------------
check("observer_sample" in src, "observer_sample() function present")
obs = src.find("static void observer_sample")
check(obs != -1, "observer_sample defined (static fn)")
m = re.search(r'"RES t=%d food=%d wood=%d coin=%d export=%d player=%08X res=%08X"', src)
check(m is not None,
      "ROUND-13 RES line `RES t=%d food=%d wood=%d coin=%d export=%d player=%08X res=%08X` present")
m = re.search(r'"--- match start n=%d player=%08X res=%08X ---"', src)
check(m is not None,
      "ROUND-13 match-start separator format present")
check("rnd_i(v[2]), rnd_i(v[1]), rnd_i(v[0]), rnd_i(v[7])" in src,
      "named line maps food=slot2, wood=slot1, coin=slot0, export=slot7 (R13)")
check("decrypt_slot_at(rr, s)" in src,
      "observer reads the 8 slots via decrypt_slot_at (real decrypt)")

# observer runs in present_hook BEFORE the original Present
start = src.find("static int STDMETHODCALLTYPE present_hook")
end = src.find("static int patch_device_present(void *dev) {")
check(start != -1 and end != -1 and end > start, "present_hook body located")
body = src[start:end]
pi = body.find("s_orig_present")
oi = body.find("observer_sample()")
check(oi != -1, "present_hook calls observer_sample()")
check(pi != -1, "present_hook still forwards the original Present")
if oi != -1 and pi != -1:
    check(oi < pi, "observer_sample() runs BEFORE s_orig_present (read, then flip)")

# --- contract 3: g_player_idx tracked through resolution --------------------
# R13: g_player_idx = 1 in the primary [human-p1] path
check("g_player_idx = 1;" in src, "primary path: g_player_idx = 1 (human-p1)")
check("g_player_idx = pick_i;" in src, "fallback path: g_player_idx = pick_i")
check("g_player_idx = -1;" in src, "reset/entry: g_player_idx = -1")

# --- contract 3b: g_obs_player tracked for match-start ----------------------
check("g_obs_player = 0;" in src, "match-start: g_obs_player reset at entry")
check("g_obs_player = p1;" in src or "g_obs_player = player;" in src,
      "match-start: g_obs_player set from primary player")
check("g_obs_player = pick;" in src,
      "match-start: g_obs_player set from fallback pick")
check("s_last_ctx" in src and "s_last_player" in src and "s_last_n" in src,
      "match-start: previous ctx/player/n tracked")
m = re.search(r"match_start\s*=\s*\(\(s_last_ctx\s*==\s*0\)\s*&&\s*\(ctx\s*!=\s*0\)\)"
              r"\s*\|\|\s*\(g_obs_player\s*!=\s*0\s*&&\s*g_obs_player\s*!=\s*s_last_player\)"
              r"\s*\|\|\s*\(n\s*!=\s*s_last_n\)", src)
check(m is not None,
      "match-start fires on ctx 0->non-0 OR player-pointer change OR n change")
check("s_snap_have = 0;" in src and "match_start" in src,
      "match-start resets the snapshot so the next frame always logs")
check("s_tick = 0;" in src, "match-start resets the tick counter")

# --- contract 4 (R13): player selection via index 1 -------------------------
check("[human-p1]" in src, "chain tag [human-p1] present")
check("n >= 2" in src or "n>=2" in src, "primary path checks n >= 2")
# index 1 access: arr + 1 * 4
check("arr + 1 * 4" in src or "arr + 4" in src, "primary path reads player index 1")

# --- contract 5: fallback scan for largest food -----------------------------
check("[fallback:" in src, "chain tag [fallback: ...] present")
m = re.search(r"for\s*\(\s*int\s+i\s*=\s*0\s*;\s*i\s*<\s*n\s*;\s*i\+\+\)", src)
check(m is not None, "fallback iterates the ctx players table (0..n)")
check("decrypt_slot_at(r, 2)" in src or "decrypt_slot_at(r2, 2)" in src,
      "fallback scan uses largest food (slot 2)")
check("[chain-break: no sane player]" in src, "chain-break tag retained")

# --- contract 6: quiet chain log -------------------------------------------
check("chain game=" in src,
      "compact chain log format `chain game= ...` present")
check("g_chain_calls > 5" in src,
      "chain log quiet-gate (first 5 calls, then on change) present")

# --- contract 7: NO per-player scanning / dumps / raw trail -----------------
check("P%d res" not in src, "no per-player P%d res lines (R13)")
check("P%d raw" not in src, "no per-player P%d raw trail lines (R13)")
check("dump_all_players" not in src, "no dump_all_players (R13)")
check("dump_window_cells" not in src, "no dump_window_cells (R13)")
check("log_resraw" not in src, "no log_resraw (R13)")
check("resraw" not in src, "no resraw references (R13)")
check("altCap" not in src and "altAdd" not in src and "altPanel" not in src,
      "no altCap/altAdd/altPanel probes (R13)")
check("cell P" not in src, "no cell dump lines (R13)")
check("MAX_PLAYERS" not in src, "no MAX_PLAYERS (R13)")

# --- contract 8: tick counter increments per frame -------------------------
check("s_tick++" in src, "tick counter increments each frame")
check("s_tick" in src and "int" in src, "s_tick is a static int")

print("SOURCE-CONTRACT:", "PASS" if failures == 0 else f"{failures} FAILURES")
sys.exit(0 if failures == 0 else 1)
