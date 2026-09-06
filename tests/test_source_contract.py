#!/usr/bin/env python3
"""test_source_contract.py — round-12 pure-observer source-level contract checks
on d3d9.c.

Round-12 observer contracts (Dali's mission: NO rendering at all):
  1. ALL draw-layer symbols must be ABSENT from the source: D3DXCreateFont,
     ID3DXFont vtable Begin/DrawText/End indices, draw_hud_text, init_font,
     draw_set_rt/states, draw_redrect, GetBackBuffer/SetRenderTarget,
     BeginScene/EndScene wrappers, D3DFVF use.
  2. The observer survives: observer_sample() present, called from present_hook
     BEFORE s_orig_present, logging PER-PLAYER NAMED `P%d res food=%d wood=%d
     coin=%d export=%d` lines (map food=slot2, wood=slot0, coin=slot1,
     export=slot7) plus the RAW `P%d raw 0..7 incA incB incN` trail, reading
     slots via decrypt_slot_at and income fields via OFF_INC_CUR/PREV/COUNT.
     A `--- match start n=.. idx=.. ---` separator + snapshot reset fires on
     ctx 0->non-0 / player change / n change.
  3. g_player_idx must be tracked (=-1, =idx authoritative, =pick_i fallback);
     g_obs_player tracked the same way for match-start detection.
  3b. Every chain log line carries `idx=N` (raw game+0x14c, `-` when ctx==0).

Round-12 dump contracts (the decisive experiment — scan ALL players):
  dump_all_players() loops every players-table slot 0..n-1, dumping per-player
  windows around res (tag r) / player (tag p) / inc (tag i) with the ACTUAL
  address in `addr=`, the stock-hunt `P%d resraw`/`P%d resraw2` plain-int lines,
  a `--- dump t=%d mode=all ---` separator per burst, and `cell P0-2 none` when
  nothing changed. t = seconds since match start (s_dump_tick anchored at match
  start, NEVER re-anchored after a dump). Per-player on-change snapshots exist.

Round-5/7 chain contracts (regression cannot re-introduce): authoritative idx
from game+0x14c trusted unconditionally when 0 <= idx < n (no probe veto),
sticky fallback + [sticky], [src-authoritative]/[src-fallback]/[chain-break]
tags, quiet chain log gate, no RVA_CUR_PLAYER_ID, probe uses
decrypt_slot_at(<container>, 0) > 0.0f.

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

# code-only copy: strip /* ... */ block comments and // line comments so that
# negative checks never false-positive on ROUND-8 prose mentioning the deleted
# draw APIs (e.g. "SetRenderTarget was DELETED in round 8").
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
m = re.search(r'"P%d res food=%d wood=%d coin=%d export=%d"', src)
check(m is not None,
      "ROUND-12 named line `P%d res food=%d wood=%d coin=%d export=%d` present")
m = re.search(r'"P%d raw 0=%d 1=%d 2=%d 3=%d 4=%d 5=%d 6=%d 7=%d '
              r'incA=%d incB=%d incN=%d"', src)
check(m is not None,
      "ROUND-12 raw trail line `P%d raw 0..7 incA incB incN` present")
m = re.search(r'"--- match start n=%d idx=%s ---"', src)
check(m is not None,
      "ROUND-9 match-start separator format present")
check("rnd_i(v[2]), rnd_i(v[0]), rnd_i(v[1]), rnd_i(v[7])" in src,
      "named line maps food=slot2, wood=slot0, coin=slot1, export=slot7 (R9)")
check("decrypt_slot_at(rr, s)" in src,
      "observer reads the 8 slots via decrypt_slot_at (real decrypt)")
for off in ("OFF_INC_CUR", "OFF_INC_PREV", "OFF_INC_COUNT"):
    check(f"ii + {off}" in src, f"observer reads income field {off}")
check("s_snap" in src and "observer_sample()" in src,
      "on-change snapshot state present")

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
loidx = src.find("g_player_idx = idx;")
check(loidx != -1, "authoritative path: g_player_idx = idx")
lpick = src.find("g_player_idx = pick_i;")
check(lpick != -1, "fallback path: g_player_idx = pick_i")
lnull = src.find("g_player_idx = -1;")
check(lnull != -1, "reset/entry: g_player_idx = -1")

# --- contract 3b (round 9): g_obs_player tracked for match-start -------------
check("g_obs_player = 0;" in src, "match-start: g_obs_player reset at entry")
check("g_obs_player = player;" in src,
      "match-start: g_obs_player set from authoritative player")
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

# --- contract 4 (round 5): authoritative active-player source ---------------
m = re.search(r"#define\s+OFF_GAME_ACTIVE_PLAYER\s+0x14c\b", src)
check(m is not None, "#define OFF_GAME_ACTIVE_PLAYER 0x14c present")
check("RVA_CUR_PLAYER_ID" not in src,
      "no RVA_CUR_PLAYER_ID reference remains (old idx source 0x866664 removed)")
m = re.search(r"idx\s*=\s*\(int\)safe_r32\(game\s*\+\s*OFF_GAME_ACTIVE_PLAYER\)", src)
check(m is not None,
      "locate_resources_impl reads idx from game+OFF_GAME_ACTIVE_PLAYER")
check("OFF_GAME_CTX" in src and "OFF_CTX_PLAYERCNT" in src and
      "OFF_CTX_PLAYERS" in src and "OFF_PLAYER_RES" in src and "OFF_PLAYER_INCOME" in src,
      "chain defines present (ctx/players/res/income)")

# --- contract 5 (round 5): probes + fallback -------------------------------
check("[src-authoritative]" in src, "chain tag [src-authoritative] present")
check("[src-fallback]" in src, "chain tag [src-fallback] present")
check("[fallback: player#%d]" in src, "fallback logs the picked player index")
m = re.search(r"for\s*\(\s*int\s+i\s*=\s*0\s*;\s*i\s*<\s*n\s*;\s*i\+\+\)", src)
check(m is not None, "fallback iterates the ctx players table (0..n)")
for probe in ("+ OFF_PLAYER_RES", "+ OFF_PLAYER_INCOME"):
    check(probe in src, f"probe-lite sanity rule uses {probe.strip()}")
check(re.search(r"decrypt_slot_at\(\w+, 0\) > 0.0f", src) is not None,
      "probe-lite sanity rule uses decrypt_slot_at(<container>, 0) > 0.0f")

# --- contract 6 (round 5): quiet chain log ------------------------
check("chain src=game+0x14C" in src,
      "quiet chain log format `chain src=game+0x14C ...` present")
check("g_chain_calls > 5" in src,
      "chain log quiet-gate (first 5 calls, then on change) present")

# --- contract 6b (round 9): chain lines carry raw idx + probe-bypass proof ---
check("idx=%s" in src and "idxbuf" in src,
      "ROUND-9 chain lines append `idx=N` (raw game+0x14c, or `-` when ctx==0)")

# --- contract 7 (round 7): unconditional authoritative + sticky fallback ----
lo = src.find("static void locate_resources_impl")
lr = src.find("#ifdef SWARM_TEST", lo)
check(lo != -1 and lr != -1 and lr > lo, "locate_resources_impl body located")
lbody = src[lo:lr]
auth = re.search(r"idx\s*>=\s*0\s*&&\s*idx\s*<\s*n", lbody)
check(auth is not None, "authoritative gate is 0 <= idx < n (in-range only)")
if auth is not None:
    seg = lbody[auth.start():lbody.find("dlog_chain", auth.start())]
    check("player_sane" not in seg,
          "NO sanity probe vetoes an in-range idx (unconditional trust)")
check("s_fb_last" in src,
      "sticky fallback remembers the last-picked player")
check("[sticky]" in src, "[sticky] tag present")
check("[chain-break: no sane player]" in src, "chain-break tag retained")

# --- contract 8 (round 12): decisive-experiment dump, ALL players scanned -----
m = re.search(r"static void dump_all_players\(DWORD arr, int n, DWORD now_ms\)", src)
check(m is not None, "ROUND-12 dump_all_players() present (scans all players)")
m = re.search(r"static int dump_window_cells\(int pdx, const char \*tag, DWORD center,"
              r"\s*int lo, int nwords, DWORD \*snap\)", src)
check(m is not None, "ROUND-12 dump_window_cells() present (per-window, per-player)")
check("s_pA_snap[MAX_PLAYERS][256]" in src and "s_pB_snap[MAX_PLAYERS][512]" in src
      and "s_pC_snap[MAX_PLAYERS][128]" in src,
      "ROUND-12 per-player window snapshots (256/512/128) present")
check("s_p_have[MAX_PLAYERS]" in src and "s_p_vals[MAX_PLAYERS][8]" in src,
      "ROUND-12 per-player on-change observer snapshots present")
m = re.search(r"for \(int i = 0; i < lim; i\+\+\)", src)
check(m is not None, "ROUND-12 per-player loop over players table (0..n-1)")
m = re.search(r"P%d resraw 0=%d 1=%d 2=%d 3=%d 4=%d 5=%d 6=%d 7=%d", src)
check(m is not None, "ROUND-12 stock-hunt resraw (first 8 dwords, no decrypt)")
m = re.search(r"P%d resraw2 0=%d 1=%d 2=%d 3=%d 4=%d 5=%d 6=%d 7=%d", src)
check(m is not None, "ROUND-12 stock-hunt resraw2 (dwords at container+0x80)")
m = re.search(r"addr=%08X int=%d float=%.1f", src)
check(m is not None, "ROUND-12 cell line prints the ACTUAL address (addr=%08X)")
m = re.search(r"--- dump t=%d mode=all ---", src)
check(m is not None, "ROUND-12 dump burst separator present")
m = re.search(r"cell P0-%d none", src)
check(m is not None, "ROUND-12 silent-burst marker `cell P0-%d none` present")
check('dump_window_cells(i, "r"' in src, "ROUND-12 res window cells use tag r")
check('dump_window_cells(i, "p"' in src, "ROUND-12 player window cells use tag p")
check('dump_window_cells(i, "i"' in src, "ROUND-12 inc window cells use tag i")
check("(now_ms - s_dump_tick) / 1000u" in src,
      "ROUND-12 t= is seconds since match start (integer slice of delta)")
check("s_dump_tick = now_ms;" in src,
      "ROUND-12 s_dump_tick anchored in match-start block (t= start)")
check("now_ms - s_dump_tick) >= 5000u" in src,
      "ROUND-12 cadence gate uses 5000 ms window from match-start anchor")
check("match_end" in src and "match_start" in src,
      "ROUND-12 match-start/match-end detection present")
check("dump_all_players(arr, n, now_ms)" in src,
      "ROUND-12 dump reachable (dumps the players table + resraw)")
check("int do_dump = match_start" in src,
      "ROUND-12 dump fires on match start (t=0 first burst), cadence AND match-end")
for off in ("-0x200", "-0x100"):
    check(off in src, f"ROUND-12 dump window offsets include {off}")
check("center + lo" in src, "ROUND-12 windows computed as center + lo (real address)")
check("si >= 0 && si <= 20000" in src,
      "ROUND-12 int filter keeps [0, 20000] only")
check("sf <= 20000.0f" in src, "ROUND-12 float filter keeps [0, 20000] only")
check("0x7F800000u" in src, "ROUND-12 Inf/NaN rejected via exponent bits (no libm)")
check("GetTickCount()" in src, "ROUND-12 timing source GetTickCount (KERNEL32 only)")

print("SOURCE-CONTRACT:", "PASS" if failures == 0 else f"{failures} FAILURES")
sys.exit(0 if failures == 0 else 1)