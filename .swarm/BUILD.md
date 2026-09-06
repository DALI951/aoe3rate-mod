# BUILD.md — AoE3 TAD resource-rate HUD mod (d3d9 proxy DLL) — ROUND 13

## ROUND 13 — single-human-observer, index-1 selection, corrected slot map (still ZERO rendering)
R12's LIVE analysis settled both open questions: (1) the **human is player-array index 1** (index 0 = nature/gaia, 2+ = AI) — in the live n=3 session P1 (base 0x17FD6000) gathered food 0→530 across two matches while P2 (AI) trickled 0→55; (2) the stock container `*(player+0x230)` IS the real resource object and the existing XOR decrypt is CORRECT (decrypts to food=530 etc.). The only remaining error was the **slot label swap**: the HUD showed 530 food / 100 wood / 130 coin while R12's logger printed food=530 wood=130 coin=100 — proving slots 0/1 were swapped in the R12 labels. ROUND-13 applies the corrected map, hard-picks the human, deletes all probe/dump/scaffolding output, and emits ONE clean RES line per Present tick.

### The R13 code changes (d3d9.c)
1. **Player selection** — primary = **index 1 when n >= 2** (`[human-p1]` tag). Robust fallback when index 1 is out of range (n<2) or its base/res is 0: scan all players 0..n-1 and pick the one with the **largest decrypted food (slot 2)** (`[fallback: player#N food=..]`). If nothing sane, `[chain-break: no sane player]` and NULLs. All match-start machinery kept: `--- match start n=%d player=%08X res=%08X ---` header on ctx/player/n change, `s_tick` reset there.
2. **Slot map (corrected)** — output now maps **food=slot2, wood=slot1, coin=slot0, export=slot7**. Decrypt math unchanged (still `key[slot] ^ enc[slot]`); only the name→slot mapping changed.
3. **Output format** — one line per tick, chosen human only:
   `RES t=%d food=%d wood=%d coin=%d export=%d player=%08X res=%08X`
   Removed: per-player P0/P1/P2 blocks, `raw 0..7 incA/incB/incN` trail, `resraw`/`resraw2` stock-hunt, `cell P%d` dumps, `altCap/altAdd/altPanel` probes, `--- dump mode=all ---`, `MAX_PLAYERS`/`dump_all_players`/`dump_window_cells`/`log_resraw` infrastructure, and all window/probe machinery. Kept: compact match-start header per match, tick counter `s_tick++` per frame (reset at match start).
4. **Observer-only invariant** preserved — no drawing, no fonts, no D3DX, no device state touches (KERNEL32+msvcrt imports only). fault filter + breadcrumbs + VirtualQuery-guarded reads all retained.

### Build command
```
C:\Users\Dali\Documents\gaames\w64devkit-x86\w64devkit\bin\i686-w64-mingw32-gcc.exe -shared -static-libgcc -O2 -o d3d9.dll d3d9.c d3d9.def
```
(with the w64devkit bin + libexec\gcc\i686-w64-mingw32\16.2.0 on PATH so cc1 is found) → **rc=0, zero warnings**.

### Build output / deploy
- **Size 83,272 B**; SHA256 **`E6F79C46EF26FD430D3366B06418752B5E3E59870116295DF3BF75DA5D7DF6D0`**
- objdump format: **`file format pei-i386`** (architecture i386)
- Deployed byte-identical (`fc /b`) to BOTH the game dir (`Age of Empires III - Complete Collection\d3d9.dll`) and `tests\d3d9.dll` — both `no differences encountered`.
- Import scan (pe_structure): **KERNEL32.dll + msvcrt.dll only**; exports unchanged (Direct3DCreate9 + Direct3DCreate9Ex + D3DPERF_*/DebugSet*).

### Test results (per harness)
- **test_source_contract.py (R13)**: **PASS** (72/72 checks — draw-layer absence, RES line format, slot map `food=v[2], wood=v[1], coin=v[0], export=v[7]`, match-start header, [human-p1]/[fallback:]/chain-break tags, tick `s_tick++`, and the removal of all P%d/raw/resraw/cell/dump/altCap/MAX_PLAYERS).
- **test_d3d9_actual.c (R13, compiled + run)**: **ALL PASS** (22 checks — index-1 primary path, fallback to largest-food, single-player fallback, n=0 menu NULLs, all-zero-food still resolves p1, decrypt round-trip 0..1e6 no NaN/INF, rate_for 562.5, RES-line every tick, correct slot map on change, removal of raw/resraw/altCap/cell lines, nullsafe + unmapped-page guards).
- **TESTER_nullsafe_O2**: **10/10 PASS** (safety properties unchanged).
- **TESTER_decrypt**: **PASS** (key table matches game bytes, round-trip no NaN/INF).
- **TESTER_loadsmoke_path**: **PASS** (loads OUR tests\d3d9.dll copy, Create9 callable).
- **test_pe_structure.py**: **0 FAILURES** (0x14C, exports, KERNEL32/msvcrt only, age3y 0x400000 base + imports d3d9.dll).
- Note: the nullsafe/decrypt/loadsmoke TESTER_* .exe harnesses were NOT recompiled — they test invariants (page guards, decrypt math, proxy load) that R13 does not change; they still pass against the deployed DLL.

### What Dali does next
Launch `age3y.exe` (game-dir d3d9.dll = R13 build), start a 1v1 skirmish, gather normally. The log should show:
```
DLL loaded .. / Create9 .. / CreateDevice ..
chain game=.. ctx=.. n=3 player=.. res=.. inc=.. [human-p1]
--- match start n=3 player=17FD6000 res=... ---
RES t=1 food=530 wood=100 coin=130 export=.. player=17FD6000 res=...
RES t=2 food=531 wood=100 coin=130 export=.. player=17FD6000 res=...
...one RES line per Present frame for the human only...
```
HUD numbers (food/wood/coin/export) should match the HUD exactly now (slots 0/1 corrected). If the numbers still don't match the HUD, send me the log.

## ROUND 12 — SCAN ALL PLAYERS: per-player P%d lines + per-player memory dumps + plain-int stock-hunt (still ZERO rendering)
R11's live log answered the wrong player question: the resolver's pick (player#2) was the **AI** (its stock climbed 0→~95/43/96 while Dali's HUD sat at food=600, wood=200, coin=30 — the human's stock NEVER appeared in any R11-scanned cell). ROUND-12 stops trusting one resolved player and does the decisive experiment the hard way: **scan every player 0..n-1** every frame for the per-player `P%d` resource lines, and every 5s dump three memory windows around EACH player's res/player/inc objects plus a **plain-int stock-hunt** (`resraw` = first 8 dwords of the res container, `resraw2` = the 8 dwords at container+0x80, NO decrypt) so Dali's frozen 600/200/30 is caught verbatim wherever the game stores it. No drawing, no D3DX, KERNEL32+msvcrt only.

### R11 bugs fixed
1. **`t` now increments** — `s_dump_tick` is anchored at match start (`--- match start ---` sets it) and is **NEVER re-anchored after a dump**. `t = (now_ms - s_dump_tick)/1000` → 0, 5, 10, ... instead of the old always-`t=5` (the old code re-anchored `s_dump_tick` after every burst so the delta never grew).
2. **`addr=` is now the ACTUAL address** — the R11 `va=%08X` bug printed the raw *dword value*, not its address. `dump_window_cells` now prints `addr=<window center + offset>` (plus the signed-hex tag like `r+0D0`), so every cell is a direct, debuggable memory location.
3. **The dump no longer assumes a single "human" player** — all three redesign items below.

### What CHANGED (d3d9.c)
- **`observer_sample` loops ALL players 0..n-1** (guarded reads for every player pointer; a NULL/invalid player slot is skipped). Each player keeps its OWN quiet/on-change snapshot (`s_p_have/s_p_res/s_p_vals/s_p_A/B/N[MAX_PLAYERS]`): first-valid logs, then only on `>=0.5` float delta or an inc-field change. Logs per player:
  `P%d res food=%d wood=%d coin=%d export=%d` (R9 map food=slot2, wood=slot0, coin=slot1, export=slot7)
  `P%d raw 0..7 incA incB incN` + `P%d altCap/altAdd/altPanel` (R10 probes, now per player).
- **Per-player cell dumps every 5000 ms** (`dump_all_players`): for each player, three windows — A: 256 dwords centered on res_container (`-0x200..+0x200`), B: 512 dwords on the player object (`-0x200..+0x600`), C: 128 dwords on the inc object (`-0x100..+0x100`). Only CHANGED cells are logged (per-player per-window snapshots `s_pA[256]/s_pB[512]/s_pC[128]`), value filter `[0, 20000]` as int OR float:
  `cell P%d <tag+offset> addr=%08X int=%d float=%.1f` (tags `r`/`p`/`i`).
  Header per burst: `--- dump t=%d mode=all ---`. If nothing changed: header + `cell P0-2 none`.
  First burst fires at match start (`t=0`) — snapshots are zeroed there, so it's the full in-range baseline; after that only deltas.
- **Stock-hunt bonus** (every dump tick AND match end, per player): `P%d resraw 0..7` (first 8 dwords of the res container as PLAIN signed ints, no decrypt) and `P%d resraw2 0..7` (the dwords at container+0x80..+0xA0). If 600/200/30 lives anywhere in a res slice as a plain int, it shows up verbatim.
- **Match-end** still fires an immediate burst; the `s_snap_have` latch is no longer reset by a cadence dump (that would force a full P-line re-log every 5s — the P-lines stay quiet/on-change; only a fresh `--- match start ---` resets them).
- UNCHANGED: fault filter + breadcrumbs, VirtualQuery guards on every read, imports KERNEL32+msvcrt only, no drawing/D3DX, `--- match start n=%d idx=%s ---` separator + snapshot reset on ctx/player/n change, chain log (quiet first-5 + on-change) with `idx/hall/uisel`, encrypted-slot decrypt via `decrypt_slot_at`, raw float slots 4/5 ints printed exactly as before.

### Code delta / files touched
- `d3d9.c` — globals: per-player observer snapshots + per-player window snapshots (`MAX_PLAYERS 16`, `s_pA_snap[.][256]`, `s_pB_snap[.][512]`, `s_pC_snap[.][128]`); old single-player `s_snap_res/s_snap/s_snapA/B/N` deleted. `fmt_cell` widened to `%04X`. New: `dump_window_cells` (one window, one player, real `addr=`), `log_resraw` (plain-int stock hunt), `dump_all_players` (per-player burst + `cell P0-%d none`). `observer_sample` rewritten: all-player loop + t-anchor fix (`s_dump_tick` set only at match start).
- `tests\test_source_contract.py` — **contract 8 rewritten for ROUND-12**: dump_all_players/dump_window_cells signatures, per-player 256/512/128 snapshots, `P%d`-prefixed named+raw lines, `P%d resraw/resraw2`, `addr=%08X`, `--- dump t=%d mode=all ---`, `cell P0-%d none`, t= seconds-since-match-start (`(now_ms - s_dump_tick)/1000u`), no post-dump re-anchor, match-start/cadence/match-end triggers. ALL PASS. (The literal count of source checks changed 8→12-ish because the contract moved from single-window to per-player scan — that's the intended output change; everything R11 asserted that still describes the design is kept, renamed.)
- Throwaway validation harness (not shipped): `Temp\opencode\r12dump\r12dump.c` proved the whole dump design **14/14**: 3-player world → `--- match start n=3 idx=0 ---`, all three `P0/P1/P2 res food=..` lines, P0 named = Dali anchor `600/200/30`, `resraw2 0=600 1=200 2=30` verbatim, res-window `cell P0 r+0090 addr=<real> float=6400.0` + player `p+0280 int=600` + inc `i+0024 int=60` with the ACTUAL addresses, `t=0` first burst then `t=6` after a simulated 6000 ms (R11 increment bug dead), unchanged burst → `cell P0-2 none`, P-lines quiet/on-change (P0 logged exactly once), wood 200→203 re-logs only P0, match-end burst fires.

### Rebuild results (round 12)
- `gcc -shared -static-libgcc -O2 d3d9.c -o d3d9.dll d3d9.def -lwinmm` with w64devkit-x86 i686 (`i686-w64-mingw32-gcc.exe`, PATH prepended `bin` + `libexec\gcc\i686-w64-mingw32\16.2.0`) → **rc=0, zero warnings**
- Size **93,256 B**; SHA256 **`B162D494B2E742283418D6033BD8359724963EA94EE9B5611DEB374F7E2ECADA`** — deployed byte-identical (Copy-Item) to game dir + `tests\d3d9.dll`, all three hashes equal; old game-dir/tests-dir/root `d3d9mod.log` DELETED (no log exists anywhere → next run starts clean); `pei-i386` (0x14C) confirmed by pe_structure; import scan = **KERNEL32.dll + msvcrt.dll only**; binary scan CLEAN (no D3DXCreateFont/DrawText/SetRenderTarget/GetBackBuffer/BeginScene/EndScene/D3DFVF/d3dx9/ID3DXFont/Arial/text=/values=/gotbb/redrect).
- Harnesses ALL PASS against the R12 build: **nullsafe O0/O1/O2/O3 10/10 each** (observer no-op on NULL/unmapped/menu still holds with the all-player loop), **d3d9_actual ALL PASS** (all previous chain/decrypt/rate 562.5/fallback/authoritative + the observer-log contract kept intact under the `P%d`-prefixed format — the 5-line-per-change count is unchanged for a 1-player world; the cadence dump never elapses inside the harness), **source_contract PASS** (rewritten contract 8), **pe_structure 0 FAILURES**, **TESTER_decrypt PASS** (keys == game bytes, round-trip no NaN/INF), **TESTER_loadsmoke + TESTER_loadsmoke_path PASS** (loads OUR tests copy, Create9 callable), **R12 dump harness 14/14 PASS**.

### Human player identification — what Dali does EXACTLY
The R11 finding is now built into the design: we scan ALL players. With your anchor **(food=600, wood=200, coin=30)** we expect **EXACTLY ONE** player's block in the pause-time dump to contain cells / `resraw*` values equal to **600 (food), 200 (wood), 30 (coin)** — that `P%d` IS you, and the offsets (`r+????` / `p+????` / `i+????` / `resraw2 slot?`) of those cells ARE your stock. (n=3 in a 1v1 skirmish, so you'll see `P0`, `P1`, `P2` — one of them is the AI whose res-values climb from 0; another is the second AI; the human's block will stay at/near the HUD numbers you read.)

1. Launch `age3y.exe` (game-dir `d3d9.dll` = R12 build) — log starts clean.
2. Start a **1v1 skirmish vs 1 AI** (n=3: you + 2AI? a 1v1 has you + 1AI = n=2, or 1v2 = n=3 — any n works, expect n=2 or n=3).
3. Play **~30-60 s** (gather normally so wood/coin RISE, food FALLS).
4. Press **ESC to pause**, read and REMEMBER your frozen HUD numbers (top-bar food/wood/coin, export if visible).
5. Quit to menu / exit, then send me **`d3d9mod.log`** (next to age3y.exe) **+ the exact paused numbers you read** (e.g. food=410 wood=320 coin=95).

I will then find the single `P%d` whose dump cells match your numbers, state your player index, and the exact stock offsets — that becomes the hard-coded `P%d` the HUD reads.

## ROUND 11 — THE DECISIVE EXPERIMENT: memory-window dumps to find Dali's exact HUD stock (still ZERO rendering)
Rounds 9-10 proved the decrypted `+0x230` container, `+0x238` cap, `+0x240` add-accumulator, and the `+0xb4`→`+0x94` panel chain do NOT hold the numbers on Dali's HUD top bar (food=600, wood=100, coin=230 when he quit). R11 stops guessing containers and BRUTE-FORCES memory: every 5s it dumps every CHANGED 32-bit cell in three windows that bracket the whole Player/container/income objects, printing each cell BOTH as int and as float. Dali reads his frozen HUD stock, quits, and we match his numbers to `p+0x??`/`res+0x??`/`i+0x??` offsets directly. No drawing, no D3DX, imports KERNEL32+msvcrt only.

### The data flow (unchanged from round 10, still workings)
`game=*(base+0x866234)` → `ctx=*(game+0x13c)` → `n=*(ctx+0x5c)` → `players=*(ctx+0x58)` → `player=*(*(ctx+0x58)+idx*4)` with idx from `*(game+0x14c)` trusted when `0<=idx<n`, else the sticky fallback (`income+0x80` + `res+0x230` + decrypted food>0 probe). In a skirmish idx is -1 (the `[0xC66664]` UI cell is dead in-match), so the fallback wins and data flows — FINE.

### What CHANGED (`d3d9.c`, adds, nothing removed)
1. **`dump_windows(rr, pp, ii, now_ms)`** (d3d9.c, after `is_dumpable`): reads three windows, each cell logged as BOTH views:
   - `res+0x230` container window: 256 dwords `res-0x200 .. res+0x1FC` → `cell res+0XX va=%08X int=%d float=%.1f`
   - player window: 512 dwords `p-0x200 .. p+0x5FC` → `cell p+0XX ...`
   - income `+0x80` window: 128 dwords `i-0x100 .. i+0xFC` → `cell i+0XX ...`
   Every cell prints `int=`, and `float=` only when finite (bit-level exponent all-ones = Inf/NaN rejected — **no libm**, `0x7F800000` mask) and `|f| < 1e6`.
2. **Filters** (so the log stays useful): a cell is logged ONLY if it (a) differs from the previous dump's snapshot of that window (per-window `s_res_snap[256]/s_plr_snap[512]/s_inc_snap[128]` kept), AND (b) its int ((int)cell) or its rounded float ((int)(f+0.5f)==f) is in **[0, 20000]** — garbage/skew pointers skipped. First dump logs everything in range.
3. **Cadence** (`observer_sample`): `now_ms = GetTickCount()`; a dump fires when `(now_ms - s_dump_tick) >= 5000` — and CRITICALLY the unchanged-frame path now `goto dump_check` **instead of `return`**, so the dump fires every 5s even while ESC-frozen (values not moving would have skipped it — caught in the throwaway harness and fixed). The burst header is `--- dump t=%d player=%08X res=%08X inc=%08X ---`, `t` = seconds INTO the match (`s_dump_tick` is re-anchored at the `--- match start ---` separator).
4. **Match-end dump**: `match_end = (s_snap_have && (g_res==NULL || g_inc==NULL))` fires an immediate burst on res/inc going NULL (single-shot: `s_snap_have` cleared first), even sub-5s. In practice the game keeps the world at the score screen, so the last cadence dump already captured the frozen stock; the match-end burst is the end marker.
5. **Signed-hex tags**: `%+03X` is ignored for hex by the CRT, so `fmt_cell()` builds `p+0D0` / `res-040` / `i-018` explicitly. NEW `now_ms` cached once per frame and passed to dump_windows (no 3× GetTickCount skew).
6. UNCHANGED: named `res food=.. wood=.. coin=.. export=..`, `raw 0..7 incA/incB/incN`, `altCap/altAdd/altPanel`, match-start separator, quiet chain log, VirtualQuery-guarded reads, fault filter + breadcrumbs, no device state touches.

### urls / files touched
- `d3d9.c` — dump_windows + fmt_cell + is_dumpable inserted; observer_sample gains now_ms/match_end/dump_check routing (`goto dump_check` from both the NULL path AND the unchanged path).
- `tests\test_source_contract.py` — **contract 8 (ROUND-11)** added: dump_windows signature, three snapshot arrays, `cell %s va=%08X int=%d float=%.1f` format, `--- dump t=.. ---` separator, 5000 ms gate, `0x7F800000` Inf/NaN mask (no libm), `GetTickCount`, match-start `t=` anchoring, dump_check reachable from NULL-resource path. ALL PASS.
- Throwaway validation harness (not shipped): `Temp\opencode\r11dump\r11dump.c` proved: match-start does NOT dump, cadence dump fires at t=6, planted int cells (`p+0D0 int=600 float=0.0`), float cells (`p+0D4 va=42C80000 float=100.0`), scaled float (`i+024 float=60.0`), delta-only re-log (p+0D0 600→601 re-logs exactly once, unchanged p+0D4 stays once), 99999 and +Inf cells filtered, sub-5s match-end dump fires. ALL PASS.

### Rebuild results (round 11)
- `gcc -shared -static-libgcc -O2 d3d9.c -o d3d9.dll d3d9.def -lwinmm` → **rc=0, zero warnings** (PATH prepended `w64devkit-x86\w64devkit\bin` + `libexec\gcc\i686-w64-mingw32\16.2.0` for cc1)
- Size **90,492 B**; SHA256 **`E4E3821E2BF02F6C0B577E328C85898EBEACDF699774BF82B91759E9309CD79C`** — deployed byte-identical (Copy-Item) to game dir + `tests\d3d9.dll`, all three hashes equal; old game-dir/tests-dir/root `d3d9mod.log` DELETED; `pei-i386` confirmed; import scan = **KERNEL32.dll + msvcrt.dll only**; binary scan CLEAN (no D3DX/DrawText/RT/BeginScene/D3DFVF/d3dx9/ID3DXFont/Arial/text=/values=/gotbb/redrect).
- Harnesses ALL PASS against the R11 build: **nullsafe O0/O1/O2/O3 10/10 each**, **d3d9_actual 23/23** (all chain/decrypt/rate/observer contracts intact with the new dump code — no extra lines leak into the existing `n0+5` counts because the 5 s cadence never elapses inside the harness), **source_contract PASS** (all rounds incl. new contract 8), **pe_structure 0 FAILURES**, **TESTER_loadsmoke_path + loadsmoke_path PASS** (loads OUR tests copy, Create9 callable), **TESTER_decrypt PASS** (keys == game bytes, no NaN/INF), **TESTER_d3d9_actual + TESTER_nullsafe_O2 PASS**.

### What Dali does EXACTLY (the decisive experiment)
1. Launch `age3y.exe` (the deployed `d3d9.dll` in the game dir is the R11 build).
2. Start a **1v1 skirmish vs AI** with the same civ you rate.
3. Play **~40-60 s** (gather normally so wood/coin RISE, food FALLS).
4. Press **ESC to pause**, read your **frozen HUD numbers** (top bar food / wood / coin, plus export if you can) and **remember them** — e.g. food=600, wood=100, coin=230.
5. Quit to menu / exit the game, then send me `d3d9mod.log` (sits next to age3y.exe).

In the log, the **LAST `--- dump t=.. player=.. res=.. inc=.. ---` burst before the end** should contain cells with `int=` or `float=` equal to your paused numbers. Example shape:
```
--- dump t=42 player=13D70F00 res=13D7C8E0 inc=1522E6C0 ---
cell p+0D0 va=00000258 int=600 float=0.0     <- stock food (int slot)
cell p+0D4 va=42C80000 int=1120403456 float=100.0   <- stock wood (float slot)
cell p+0D8 va=000000E6 int=230 float=0.0     <- stock coin (int slot)
```
The `+0D0`/`+0D4`/`+0D8` (and any `res+0x??` / `i+0x??`) offsets that match his numbers ARE the stock. Notes: values print as BOTH int and float; a float-stored number shows `float=600.0` (with a garbage `int=` as in the 100.0 row above — ignore the int column there), an int-stored number shows `int=600` (with `float=0.0`; ignored). If his number is stored scaled (e.g. `float=60.0` for 600), that scaled form still counts. He just sends his paused numbers + the log.

## ROUND 10 — PROBE round: real stock containers + both human-player authorities (still ZERO rendering)
ROUND-9's live log (food 0→141, wood 3, coin 0→29, export 0, slot4 ~10005 draining to 9830) is NOT the HUD stock (Dali eyeballed it against a stock that starts ~200/100/100 and falls while gathering). This round went back to the disassembly to find the containers the game ACTUALLY reads for player resources, then shipped a probe that logs ALL candidate containers + BOTH alternative human-player indices so ONE in-game run decides each question against the HUD.

### Findings (disassembly evidence, `objdump -d age3y.exe` + Ghidra dump)
1. **`[player+0x230]`, `[player+0x238]`, `[player+0x240]` are THREE SEPARATE container-POINTER fields on the Player**, not one inline array. The write path `FUN_0049a8c5` (= rmSetPlayerResource target, FUN_00906829 → `FUN_0049a8c5(id, value, 0)`) disasm at **0x49a8e8 / 0x49a906 / 0x49a918** does `lea 0x238(%esi),%ecx` (cap READ), `lea 0x230(%esi),%ebx` (current READ + SET via `FUN_0058ef83`), `lea 0x240(%esi),%ecx` (running ADD via `FUN_0049a859`). `FUN_0058ef83` derefs `*this` (`mov (%esi),%eax` at 0x58ef83+) so `*(player+0x230)` is the pointer to the 8-slot encrypted array d3d9.c already uses — **the structure is CORRECT**; the question is whether its values are the HUD stock.
   - `player+0x238` = cap/limit container (read-only in the write path — rejected when `current(+0x238) < value`-style guard fires).
   - `player+0x240` = numeric ADD accumulator (every delta summed = lifetime total).
2. **Slot map food=2, wood=0, coin=1, export=7 is CONFIRMED** by the eco-score fn `FUN_005009c0` disasm **0x5009f1-0x500a2c**: `lea 0x230(%esi),%edi` then 4× `FUN_0044efff` with slots **2, 0, 1, 7** summed into the match-score accumulator — the game's OWN count order for the four HUD resources. Matches Dali's live numbers + the R9 map.
3. **The top bar finds the human player two ways you can observe live:**
   - `FUN_0086da39` (the "X Minute Income" eval) is called from **0x883529 / 0x883570** with `player = FUN_0044cb21(ctx, [0xC66664])` then `income = [player+0x80]` — so the classic `[0xC66664]` (the old dead-in-R5 cell) IS the top-bar's index. We now log it as `uisel=` instead of trusting R5's menu-time verdict.
   - The resource panel resolver `FUN_0044cc6d` / `FUN_006f6216` (0x6f6234-0x6f6244) uses `idx = *(*(0xC67310)+0x920)+0x13c` masked `&0x3ffff`, resolved through the `ctx+0xc` player table, and reads the panel's own stock container at `*(*(player+0xb4)+0x94)+4`. We now log that idx as `hall=` and that container as `altPanel`.
4. R9 primary line (`+0x230`, map 2/0/1/7) is UNCHANGED so the comparison against the HUD stays apples-to-apples.

### What CHANGED (d3d9.c)
- **Three alt-container probe lines** (`observer_sample`, after the R9 `raw` line): `altCap 0..7` (`*(player+0x238)`), `altAdd 0..7` (`*(player+0x240)`), `altPanel 0..7` (`*(*(player+0xb4)+0x94)+4` inline base). A NULL/unreadable container logs `<name> skip`.
- **Human-authority probe on every quiet chain line** (`dlog_chain`): format now `... idx=%s hall=%d uisel=%d ...` where `hall` = `*(*(0xC67310)+0x920)+0x13c & 0x3ffff` and `uisel` = `[0xC66664]` (either may read -1 in menu; -1 also when the chain link is 0).
- New defines: `OFF_PLAYER_RES_CAP 0x238`, `OFF_PLAYER_RES_ADD 0x240`, `OFF_PLAYER_PANELOBJ 0xB4`, `OFF_PANEL_CONT 0x94`, `OFF_PANEL_BASE 0x04`, `RVA_HALL_PTR 0x00867310`, `OFF_HALL_SEL 0x920`, `OFF_HALL_LOCAL 0x13c`, `HALL_EMASK 0x3ffff`, `RVA_UI_SEL 0x00866664`.
- NO new hooks, NO render calls, NO state changes — still read-only observer (guarded reads everywhere; ROUND-10 alt reads go through `safe_r32`/`decrypt_slot_at`).

### What Dali's log should look like now
```
chain src=game+0x14C game=.. ctx=.. n=3 idx=0 hall=0 uisel=0 player=.. res=.. inc=.. [src-authoritative]
--- match start n=3 idx=0 ---
res food=500 wood=200 coin=30 export=80     <- primary: +0x230 map 2/0/1/7
raw 0=200 1=30 2=500 3=0 4=0 5=0 6=0 7=80 incA=.. incB=.. incN=..
altCap 0=.. 1=.. ..                          <- +0x238 cap/limit container
altAdd 0=.. 1=.. ..                          <- +0x240 lifetime-add container
altPanel 0=.. 1=.. ..                        <- the panel chain *(*(player+0xb4)+0x94)+4
```
Report to Dali: eyeball `res`/`altCap`/`altAdd`/`altPanel` food/wood/coin columns against the HUD top bar (food should FALL from its start value when villagers gather, wood/coin RISE); note `hall=` and `uisel=` values in-match. The container + the slot order that move with the HUD win; then we hard-code them and drop the probe round.

### Rebuild results (round 10)
- `gcc -shared -static-libgcc -O2 d3d9.c -o d3d9.dll d3d9.def -lwinmm` (PATH prepended with `w64devkit-x86\w64devkit\bin` + `libexec\gcc\i686-w64-mingw32\16.2.0` for cc1) → **rc=0, zero warnings**
- Size **87,561 B**; SHA256 **`0F40A79EBB90EC46C19BCB6A605CD4139546C43B19558BF4C3FFF594570998EF`** — deployed byte-identical (`Copy-Item`) to game dir + `tests\d3d9.dll` (rebuilt: tests fixture is the PRODUCTION build again), all three hashes equal; old game-dir `d3d9mod.log` + stale `tests\d3d9mod.log` DELETED; `pei-i386` confirmed.
- Harnesses rebuilt to the R10 contract: **nullsafe O0/O1/O2/O3 10/10 PASS** each; **d3d9_actual ALL PASS** (23/23 — observer now expects the +3.0 change to re-log named+raw+3 probe `skip` lines = `n0+5`, `altCap/altAdd/altPanel skip` asserted for the zeroed fake world; chain/decrypt/rate 562.5/fallback/authoritative/match-start all green); **source_contract PASS** (all R9 draw-absence + chain contracts, untouched); **pe_structure 0 FAILURES** (d3d9.dll 0x14C, exports Direct3DCreate9/9Ex, imports KERNEL32/msvcrt only; age3y 0x14C, base 0x400000, statically imports d3d9.dll); **TESTER_loadsmoke_path PASS** (loads `tests\d3d9.dll`, not system32); **TESTER_decrypt PASS** (keys == game bytes, decrypt round-trip no NaN/INF).

## ROUND 9 — named resources via the decompile map + match-start separator + raw trail (still ZERO rendering)
Dali's round-8 live log said slot 0 is NOT food (certain). The round-2 decompile of FUN_005009c0 (eco-score / resource iteration) resolves the map definitively: **food=slot2, wood=slot0, coin=slot1, export=slot7**. R9 keeps the pure-observer DLL exactly as round 8 (no drawing!) but re-labels the log so Dali can verify each number against his in-game HUD, and adds a fresh-start marker per match.

### What CHANGED (d3d9.c)
- **Named log line** — on each change (`observer_sample`, d3d9.c ~492, R9 snippet): `res food=%d wood=%d coin=%d export=%d` with `food=rnd_i(v[2])`, `wood=rnd_i(v[0])`, `coin=rnd_i(v[1])`, `export=rnd_i(v[7])` — the decompile map, rounded via a tiny `rnd_i()` helper (`x>=0 ? (int)(x+0.5f) : (int)(x-0.5f)`, no math.h).
- **Raw trail line** — same change, second line: `raw 0=%d 1=%d 2=%d 3=%d 4=%d 5=%d 6=%d 7=%d incA=%d incB=%d incN=%d` (all 8 slots + the round-8 income fields `inc+0x160/+0x164/+0x16C`, signed) so a wrong label can be fixed with ZERO rebuilds.
- **Match-start separator + reset** (~d3d9.c 481-497): when ctx transitions 0→non-0, OR the resolved player pointer (new `g_obs_player`, set by `locate_resources_impl` entry/authoritative/fallback) changes, OR the player count `n` changes → log `--- match start n=%d idx=%d ---` (idx = raw `*(game+0x14c)`, `-` when ctx==0) and reset `s_snap_have` so the next frame logs regardless of the 0.5 threshold. Detection runs BEFORE the `g_res/g_inc` NULL guard so a menu→game transition always prints its separator.
- **`idx` on every chain line** (`dlog_chain`, ~276-287): format now `chain src=game+0x14C game=%08X ctx=%08X n=%d idx=%s player=... res=... inc=...<tag>` — raw `*(game+0x14c)` when ctx!=0, else `idx=-`. This diagnoses R8's persistent `[chain-break: no sane player]`: we now SEE whether the idx is out of 0..n or still being vetoed.
- **Probe-bypass CHECK = PASS**: the round-7 authoritative branch (`idx >= 0 && idx < n` at ~327) still reads `player/res/inc` directly and logs `[src-authoritative]` with NO `player_sane()` call in-between (contract 7 re-verified in `test_source_contract.py`, plus the `seg` slice check). An in-range idx ALREADY wins unconditionally — no fix needed; the new `idx=` makes it observable.

### What Dali's log should look like now
```
chain src=game+0x14C game=.. ctx=.. n=3 idx=0 player=.. res=.. inc=.. [src-authoritative]
--- match start n=3 idx=0 ---
res food=500 wood=200 coin=30 export=80      <- matches HUD at game start?
raw 0=200 1=30 2=500 3=0 4=0 5=0 6=0 7=80 incA=.. incB=.. incN=..
<lines repeat on change as he gathers: food falls, wood/coin/export rise>
```
He eyeballs food/wood/coin/export against his HUD and reports which labels are right; if any slot is wrong we fix the label in the NAMED line only (the raw trail already prints every slot).

### Rebuild results (round 9)
- `gcc -shared -static-libgcc -O2 d3d9.c -o d3d9.dll d3d9.def -lwinmm` → **rc=0**
- Size **84,501 B**; SHA256 **`50AA0D83EB2F57401664B76AD651A309DFF8EDB9E0AABD2C602FC6558E7ED0C2`** — deployed byte-identical (Copy-Item) to game dir + `tests\d3d9.dll`, all three hashes equal; old game-dir `d3d9mod.log` deleted; binary scan CLEAN (no `D3DXCreateFont`/`DrawText`/`SetRenderTarget`/`GetBackBuffer`/`BeginScene`/`EndScene`/`D3DFVF`/`d3dx9_25`/`ID3DXFont`/`Arial`/`gotbb`/`redrect`).
- Harnesses (rebuilt to the R9 contract): **nullsafe -O2 10/10 PASS**; **d3d9_actual ALL PASS** (rebuilt `test_observer_log`: `--- match start n=1 idx=0 ---` separator + `res food=500 wood=200 coin=30 export=80` from seeds slot2=500/slot0=200/slot1=30/slot7=80, raw trail carries slot0/1/2/7, unchanged frame logs NOTHING, +3.0 wood bump re-logs named+raw exactly once, raw `incA=2000 incB=1000 incN=3`); **source_contract PASS** (added R9 checks: both new format strings, match-start regex, map order `rnd_i(v[2]), rnd_i(v[0]), rnd_i(v[1]), rnd_i(v[7])`, `g_obs_player` tracking, `idx=%s`+`idxbuf` chain lines, probe-bypass slice re-verified); **pe_structure 0 FAILURES**.

## ROUND 8 — pure observer, zero rendering (Dali's mission: DELETE the draw layer)
Round-7 live run PROVED the draw path corrupts the game: units "shatter / appear-and-disappear", grass shader breaks when using the selection box, and `step=gotbb desc.w=0 desc.h=0 fmt=00000000` showed GetBackBuffer returning garbage → we `SetRenderTarget`'d an invalid surface, released it, never restored the game's RT/states → the game's next frames rendered with OUR leftover state (also why every DrawText was invisible). Dali explicitly changed the goal: NO drawing, NO HUD — just READ the local player's resources every Present and LOG them when they change. The game must run 100% pristine (no shatter/flicker/grass artifacts) — that is the acceptance check.

### What was DELETED (d3d9.c, no dead weight left)
- `init_font` + `D3DXCreateFontA`/`s_create_font`/`g_font` + `g_d3dx` + `d3dx9_25.dll` load attempts
- `ID3DXFont` vtable Begin/DrawText/End wrappers (`D3DXFONT_BEGIN_IDX/DRAWTEXT_IDX/END_IDX`)
- `draw_set_rt` (GetBackBuffer/GetDesc/SetRenderTarget), `draw_set_states` (all D3DRS_*), `draw_redrect` (D3DFVF 0x044 DrawPrimitiveUP), `draw_hud_text` (scene wrapper + text=/values= dumps)
- `log_step`, `dev_not_ready`, `g_bb_dumped/g_diag_done/g_redrect_frames/g_frame_draws/s_font_logged`, present frame-cadence logging
- `rate_for()` survives ONLY behind `#ifdef SWARM_TEST` (harness-proves FUN_0086da39 math 562.5; the production observer logs the RAW inc fields instead)

### What REMAINS in the Present hook (only read + log; nothing drawn)
```
present_hook: g_device=self;
  locate_resources()            // menu -> quiet  chain  lines only
  set_step("observer-sample")
  observer_sample()             // read+log on change, ZERO rendering
  set_step("present-before") -> forward original Present -> set_step("present-after")
```
`observer_sample()` (d3d9.c ~462): returns early when `g_res||g_inc == NULL` (main menu → no new log lines). Otherwise reads all 8 decrypted slots `v[s] = decrypt_slot_at(rr, s)` (container `res+0x230`, keys VA 0xC6DF14, XOR2 0xC6DF10) + income raw fields `A=safe_r32(inc+0x160)`, `B=inc+0x164`, `N=inc+0x16C` (signed). LOGS ONLY ON CHANGE (first snapshot per resolve/player, then when any of the 8 floats differs by >= 0.5 abs or any of A/B/N changed):
```
res P%d f=%d w=%d c=%d e=%d s3=%d s4=%d s5=%d s6=%d s7=%d incA=%d incB=%d incN=%d
```
(f/w/c/e = decoded slot[0][1][2][7] — mapping still suspect → ALL 8 slots print; s3..s7 = slots 3..7 raw; P = resolved player index or -1, tracked in `g_player_idx` by `locate_resources_impl` from `idx`/`pick_i`.)

### KEPT AS-IS (unchanged from round 7)
- Chain: `game=*(base+0x866234)` → `ctx=*(game+0x13c)` → `n=*(ctx+0x5c)` → `players=*(ctx+0x58)`; authoritative idx `*(game+0x14c)` trusted UNCONDITIONALLY when `0 <= idx < n` (no probe veto); sticky fallback (`s_fb_last`, income+0x80 / res+0x230 / decrypted food>0 probe) for OOR only
- Decrypt (`decrypt_slot_at`), VirtualQuery-guarded reads, fault filter + `g_step` breadcrumbs, Reset hook (pointer hygiene + hook re-assert only — no font to invalidate), CreateDevice forwarding/patching, Create9 wrap
- Log tags `[src-authoritative]` / `[src-fallback] [fallback: player#N]` / `[sticky]` / `[chain-break: ...]`, first-5-then-on-change quiet gate

### Evidencing the strip is real
- `grep` of `d3d9.c` (comment-stripped): NO `D3DXCreateFontA`, `ID3DXFont`, `D3DXFONT_*`, `draw_hud_text`, `init_font`, `draw_set_rt/states`, `draw_redrect`, `GetBackBuffer`, `SetRenderTarget`, `BeginScene`, `EndScene`, `D3DFVF`, `g_font`, `log_step`, `d3dx9_25.dll`.
- Binary scan of the built DLL (all-False): `D3DXCreateFont`, `DrawText`, `SetRenderTarget`, `GetBackBuffer`, `BeginScene`, `EndScene`, `D3DFVF`, `d3dx9_25`, `ID3DXFont`, `Arial`, `text=`, `values=`, `gotbb`, `redrect`.
- PE: machine 0x14C (i386/32-bit), imports now **KERNEL32.dll + msvcrt.dll only** (USER32 dropped with the last `SetRect`/`DrawText*`), 11 exports unchanged (Direct3DCreate9(+Ex) + D3DPERF_*/DebugSet*).

### Rebuild results
- `gcc -shared -static-libgcc -O2 d3d9.c -o d3d9.dll d3d9.def -lwinmm` → **rc=0, zero warnings** (one pre-existing strict-aliasing note in `decrypt_slot_at` does not reproduce at this TU split)
- Size **83,880 B**; SHA256 **`5BD44B3F14449BB18EC32D5D5FF49510F3C29B0059DE7FD7BB77B8E4C1D1910A`** — deployed byte-identical (Copy-Item) to game dir + `tests\d3d9.dll`, all three hashes equal; old game-dir `d3d9mod.log` deleted
- Harnesses (rebuilt to the observer-only contract): **nullsafe -O2 10/10 PASS** (observer no-op on NULL/unmapped/no-device); **d3d9_actual ALL PASS** (incl. new `test_observer_log`: first snapshot `res P0 f=200` logged, unchanged frame logs NOTHING, +3.0-slot change re-logs exactly once `res P0 f=203`, inc fields present; plus chain/decrypt/rate 562.5/fallback/authoritative checks); **source_contract PASS** (all round-8 negative draw checks + round-5/7 chain/observer contracts); **pe_structure 0 FAILURES**.

### What Dali's next log should show
- On load: `DLL loaded` → `Create9` → `CreateDevice` lines (once each).
- Entering a game: `chain src=game+0x14C game=.. ctx=.. n=.. player=.. res=.. inc=.. [src-authoritative]` (or `[src-fallback] ... [sticky]` if the idx was OOR) — the FIRST resolve, then quiet.
- Then `res P# f=.. w=.. c=.. e=.. s3=.. s4=.. s5=.. s6=.. s7=.. incA=.. incB=.. incN=..` lines: first snapshot logged immediately, afterward ONLY when a decoded slot moves by >= 0.5 (abs) or an inc int changes — as Dali gathers, they'll step up/down while f (food) falls, w/c (wood/coin) rise, inc counters tick. FAIL-safe: ANY change in the ledger should produce a new line.
- **Acceptance:** game renders PERFECTLY — no unit shatter, no flicker, no grass-shader artifacts, HUD/selection box normal. The DLL touches nothing but reads; any visual corruption now = game-side, not ours.

## ROUND 7 — player-flip fix + phase-split draw diagnostics (live: R6 crash fix WORKED, but HUD invisible AND HUD showed the AI player; SUPERSEDED by round 8 — draw layer deleted)
Round-6 live log (366 lines) PROVED two separate bugs: (1) **player flip** — line 26 `[fallback: player#2]` (res=13CCC9A0) → line 294 `[fallback: player#1]` (res=13CCCB40) while line 23 `[chain-break: no sane player]` fired every frame: `*(game+0x14c)` was in range but FAILED the sanity probe, so the fallback re-scanned every frame and flickered between AI players; (2) **invisible HUD** — 20 frames of `step=begin/draw×8/end/drawdone` ALL completing with `font init ok dev=... font=06a28460`, no crash, game ran to menu, but NOTHING ever rendered.
- **Fix A — unconditional authoritative idx (flip):** `locate_resources_impl` now trusts `*(game+0x14c)` whenever `0 <= idx < n` with **NO sanity probe veto** (`d3d9.c:294`; the probe `player_sane()` `:262` ONLY gates the fallback). The AI flip came from the probe rejecting the human (e.g. coin-slot 0) and re-running the scan each frame. Log now emits `[src-authoritative]` and stays stuck on one player.
- **Fix A2 — sticky fallback (d3d9.c:261, 306-341):** if the idx IS OOR/broken, the fallback prefers `s_fb_last` (last-kept player) when still sane, else first sane, and re-adopting it logs `[src-fallback] [fallback: player#%d] [sticky]` — a player pointer is stable, so the human keeps its identity instead of "first-sane" flapping between AI players. Logs only on change; `[chain-break: no sane player]` retained.
- **Fix B — phase-split draw diagnostics (ONE run decides):** `draw_hud_text` (`d3d9.c:606`) now:
  1. **Scene wrapper** (`:650`): our hook runs at Present, AFTER the game's EndScene — D3D9 rasterizes NOTHING outside a scene, so every DrawText silently no-ops ("returns OK, renders nothing"). We open our own `BeginScene`/`EndScene` (vtable slots 41/42) around the draw — EndScene only if our BeginScene succeeded.
  2. **Red rect** (`draw_redrect` `:584`, gate `:666`): solid 160x40 `0xFFFF4040` quad at (8,8) via `DrawPrimitiveUP` (D3DFVF 0x044) for the first 30 frames after a player resolves, logged `step=redrect frame=N` — separates "text params broken" from "nothing rasterizes" in one look.
  3. **One-shot param dumps**: `step=gotbb desc.w=%d desc.h=%d fmt=%08X ms=%d` from `GetBackBuffer` → `GetDesc` (`:559`), then per line `text=%s x=%d y=%d w=%d h=%d color=%08X font_h=%d` (`:673`) + `values f=%.0f w=%.0f c=%.0f e=%.0f rate_f=%.1f ...` (`:633`) on the first resolve — proves text strings/slots/math against the in-game ledger.
  4. **Force-legible text**: opaque white `0xFFFFFFFF`, `DT_LEFT|DT_TOP|DT_NOCLIP`, `step=states` logged after `draw_set_states` (`:568`) so the log proves the render-state setup ran.
- **UNTOUCHED (objective C):** chain offsets, decrypt, rate math (FUN_0086da39), log tag formats, fault filter, Reset hook.
- **Diagnostic interpretation (Dali's run):** `[src-authoritative]` fires with NO mid-game flip; `redrect` ~30 frames; one `text=`/`values` block. Red rect visible + text invisible = text-params; nothing visible at all = draw-buffer/present problem; both visible = **HUD SHIPPED**.

## ROUND 6 — crash-catch + hardened draw path (live: chain fix WORKS, game exits silently mid-run once the HUD draws)
Round-5 live log: `chain src=game+0x14C game=05BF2000 ctx=14530000 n=3 player=1EC12800 res=13D5C9A0 inc=1521E000 [src-fallback] [fallback: player#2]` → `font init ok` → log ends, game closed itself with no dialog = a hard AV inside the DLL in the FIRST frames that actually draw (res was always NULL before, so the draw path only now runs). Chain/locate/decrypt/fallback/log-tag UNTOUCHED.
- **Crash catch (installed FIRST in DllMain, before all else):** `SetUnhandledExceptionFilter(fault_filter)` → `FAULT addr=%08X breadcrumb=%s code=%08X` then `ExitProcess(0)` (silent exit, no dialog). Global `volatile g_step[64]` updated around every risky call. Next run identifies the exact dying call in ONE shot if it faults.
- **Per-frame breadcrumbs:** the first 20 frames that actually draw log `step=draw-rt / draw-states / begin / draw / end / drawdone`; then quiet per the round-5 rule (g_step still updates, only the FAULT line prints it). `present-before`/`present-after` bracket the original Present call.
- **Hardened draw (all independent, all plausibly-the-AV):**
  - Explicit render target: `GetBackBuffer(0,0,MONO)` → `SetRenderTarget(0,bb)` before Begin (a stale/lost RT left set by the game during a transition/loading frame = AV on DrawText); GetBackBuffer ref released after.
  - Render states set defensively: ZENABLE/ZWRITEENABLE/STENCILENABLE off, ALPHABLENDENABLE on (SRCALPHA/INVSRCALPHA), LIGHTING off.
  - Device-loss guard: `TestCooperativeLevel` via vtable slot 3; `D3DERR_DEVICELOST/DEVICENOTRESET` → skip the frame, never touch the font. **Reset hook (vtable slot 16):** releases + invalidates the font (lazy re-create on next non-NULL res), logs `reset dev=%p -> font invalidated`, re-asserts Present/Reset hooks.
  - Airtight Begin/End pairing: a failed Begin now skips BOTH DrawText AND End.
  - Draw only when font && res && device intact (main menu still draws nothing).
- **test_source_contract.py fixed/updated:** the present_hook body slice now anchors end on the `patch_device_present(void *dev) {` DEFINITION (round 6 added a forward declaration which the old `.find("static int patch_device_present")` matched first, slicing an empty body → 3 spurious FAILs).

## ROUND 5 — authoritative local-player source (live log: `game=05BF2000 ctx=14500000 n=3 idx=-1 arr=00000000 [chain-break: idx OOR n]`)
Round 4's instrumentation PROVED the dead link: inside a real skirmish `idx=*(base+0x866664)=0xFFFFFFFF`, i.e. **DAT_00c66664 is a UI *selection* marker, not the local player**. Round 5 found and shipped the engine's own authoritative index:
- **Authoritative source (evidence in the decompile dump):** `*(game + 0x14c)`.
  - `FUN_00405574` (per-frame UI player resolution): `idx = *(int*)(DAT_00c66234+0x14c)` when `ctx && ctx[0x5c] >= 1`, else -1, then pushes it into the UI current-player cache `*(*(hall+0xc1a0)+0x28)`.
  - `FUN_0045084c` (player-select UI handler): `FUN_0044cb21(ctx, *(int*)(DAT_00c66234+0x14c))` guarded `-1 < idx` — the game resolving a player from this exact index.
  - Dead sources checked: `[0xC66664]` (UI-only writes FUN_00720ca7/FUN_007212bf/FUN_004286f1/FUN_00429026, reset -1), `DAT_00dc3cd8`, `*(game+0x140)` (resource manager; `+0x80` is a slot/bounds count, not the income obj), `*(game+0x148)` (UI wrapper), hall `+0xc19c/+0xc1a0`, `DAT_00c66240` (hall ptr), "X Minute Income" dev-command `FUN_00718f85` (console, not a HUD), `FUN_007e8171` callers (archive/schema evaluators).
- **probe-lite fallback (d3d9.c `locate_resources_impl`):** if the authoritative index is OOB or its player fails the sanity probe (income `+0x80` non-NULL AND container `+0x230` non-NULL AND decrypted slot-0 food > 0), scan the ctx players (`[ctx+0x58]`, n=`[ctx+0x5c]`) and take the FIRST sane one → logged `[src-fallback] [fallback: player#%d]`. Main menu (`n==0`) resolves to NULL/0 and draws nothing.
- **Quiet chain log (round-5 spec):** one line `chain src=game+0x14C game=%08X ctx=%08X n=%d player=%08X res=%08X inc=%08X <tag>` with tag `[src-authoritative]`, `[src-fallback] [fallback: player#%d]` or `[chain-break: <link>]`; emitted for the first 5 calls, then only when `player/res/inc` (or the tag) CHANGE. `scratch=` and `idx=` dropped.
- **Tests updated (production chain semantics changed):** `tests\test_d3d9_actual.c` now lays `idx` at `game+0x14C`, seeds container slot-0 food (probe), menu = `n==0`; NEW fallback test (idx OOB → sane player found). `test_nullsafe.c`: world sets `game+0x14C`; case 3 reworked to "OOB idx → fallback picks sane player", new case 3b "n==0 menu → NULLs". All green: actual-harness 7/7, NULL-safety 9/9, decrypt, loadsmokes, source-contract, pe-structure.
- Hash/byte-identical deploy: **86,805 B**, SHA256 `B979FC78FF7C39B9B5BC5A2A925D91477844837A4474A4D5FD267D282D8350CE`, `pei-i386`, imports KERNEL32/msvcrt/USER32 only. Old game-dir `d3d9mod.log` deleted.

## ROUND 4 — deep chain instrumentation (live test: hook chain perfect, res=0/inc=0 every frame, count 0→8)
Live d3d9mod.log showed the hook chain is perfection (DLL loaded → Create9 wrapped → CreateDevice patched → present firing continuously) but `res=00000000 inc=00000000` every frame, while `count` went 0→8 (resource system alive). The player-chain hit a NULL link at runtime. Per instructions: DO NOT guess — instrumented the chain and logged every step:
- **Chain line (one per call):** `chain game=%08X ctx=%08X n=%d idx=%d arr=%08X player=%08X res=%08X inc=%08X scratch=%08X [chain-break: <link>]`
- Break tags: `game==0`, `ctx==0`, `n<=0`, `idx OOR n`, `arr==0`, `player==0`, `res==0`.
- `scratch=%08X` = guarded deref of the OLD contested global `*(base+0x9CB808)` (VA 0xDCB808, laptop mod used it; inspector said scratch) — comparison data on the same line.
- Cadence: FIRST call after DllMain + first 15 calls + every 300th (~5 s) + whenever ANY of the 9 observed values CHANGES (a NULL→non-NULL link transition always fires). One line per call, no spam.
- **TASK 2 honest re-audit (decompile ground truth re-verified, NO production change):** disasm of the income call site 0x88530e..0x885334 confirms our chain EXACTLY: `game=[0xC66234] → ctx=[game+0x13c] → FUN_0044cb21(ctx, [0xC66664]) → player`, and FUN_0044efff call sites in FUN_005009c0 confirm container = `[player+0x230]` (`lea edi,[esi+0x230]; mov ecx,edi; [ecx]`). VERIFIED strike: **DAT_00c66664 is written ONLY by UI/input-layer handlers (FUN_00720ca7 / FUN_007212bf `=param_1[1]`, FUN_004286f1 / FUN_00429026) and reset to 0xFFFFFFFF (FUN_00765dfa etc.)** — a prime suspect for the dead link (idx=-1 during a skirmish when no UI event set it). The container is valid on ANY player object (FUN_005009c0 iterates players), so a future fix would use a stable local-player pointer — but production code is UNCHANGED this round; the log will prove which link dies live.
- Whole suite still green (decrypt, NULL-safety -O0..-O3 6/6, actual-harness 6/6, loadsmokes, source-contract).

## ROUND 3 — draw-path fixes (live game: "no HUD at all", game runs fine)
Live test from round 2 = proxy transparent, nothing draws. Root cause = 2 draw bugs + no visibility, all fixed:
- **BUG 1 (draw timing):** `present_hook` drew AFTER the original Present → HUD landed in the NEXT frame's back buffer and that frame's Clear erased it. Fixed: `draw_hud_text()` now runs BEFORE `s_orig_present` (d3d9.c `present_hook`, ~line 347).
- **BUG 2 (font vtable):** `D3DXFONT_DRAWTEXT_IDX` was 5 = **GetLogFontW** (a silent no-op with our args), so DrawText never drew. Canonical ID3DXFont layout: 12 Begin, **13 DrawTextA**, 15 End. Fixed: `draw_hud_text` calls Begin(12) → N×DrawText(13) → End(15), End ALWAYS called even on Begin/Draw error; Begin failure skips draws without crashing (d3d9.c HUD text section).
- **BUG 3 (diagnostics):** always-on append log `d3d9mod.log` next to the exe: DLL-ATTACH (base + real d3d9 addrs), Create9 (v + wrapped ptr), CreateDevice hr/dev/patched, present (dev/res/inc/count, first 10 calls then every 300th frame), and one-time font-init failures (stage + HRESULT/err). Writes are open/flush/close so a crash loses nothing. The log converts "nothing" into an exact stage trace.
- **New test:** `tests\test_source_contract.py` — asserts DRAWTEXT_IDX==13 (Begin 12 / End 15) and draw-before-present ordering in source. **PASS** along with the whole suite (decrypt, NULL-safety -O0..-O3, actual-harness, both load-smokes).

## 1. Files created
- `.swarm/PLAN.md` — build plan (persisted verbatim)
- `.swarm/REVIEW.md` — reviewer findings that drive round 2
- `.swarm/import.bat`, `dump_targeted.bat`, `dump_all.bat`, `xrefs.bat`, `memdump.bat` + logs
- `ghidra_scripts/DumpAllFunctions.java`, `DumpTargeted.java`, `Xrefs.java`, `MemDump.java`
- `ghidra_proj/aoe3y_decompiled/` — 30,581 `.c` files, `dump_all_functions.txt` (41.4 MB), `targeted_functions.txt`, `xrefs_dcb808.txt`, `memdump_constants.txt`
- `VERIFIED_ADDRESSES.md` — address verdicts, updated for round 2 (container `[player+0x230]`, FUN_0086da39 income math)
- `d3d9.c`, `d3d9.def`, `d3d9.dll`, `build_log.txt` — the proxy DLL (round-2 data layer)
- `decrypt_unit.c`, `test_decrypt.c`, `test_decrypt.py`, `.exe` — test 6.1
- `test_nullsafe.c`, `.exe`, `tests\nullsafe_O1/O2/O3.exe` — NULL-safety harness (round-2: fake world at real RVAs, chain tests)
- `tests\test_d3d9_actual.c`, `.exe` — independent SWARM harness compiling the real d3d9.c
- `tests\d3d9.dll` — proxy copy the path-load smoke test must load
- `loadsmoke.c`, `loadsmoke.exe`, `tests\loadsmoke_path.c/.exe` — COM-wrapper smoke tests
- Stale `test_nullsafe_o1.exe` (SEH-era binary) **DELETED** per tester

## 2. Decompile status
- Ghidra 12.1.3 + JDK 25: import + full analysis OK, exit 0
- Full decompile dump completed: **30,581 functions**, 5 decompile-failures (non-code stubs)
- Round-2: `FUN_0086da39` (0x86da39–0x86dbe0) disassembled with objdump → full "X Minute Income" algorithm decoded; current-player chain verified via call site 0x88352e + `FUN_0044cb21` (get-by-index), `FUN_0044efff` decrypt re-verified against eco-score `FUN_005009c0` (`[player+0x230]`)

## 3. VERIFIED_ADDRESSES.md — round-2 corrections
1. **0xDCB808 is NOT the live container** — it is the income-projection scratch (7 refs in FUN_0086da39 + alloc/init). Replaced by the current-player chain:
   `game=*(base+0x866234)` → `ctx=*(game+0x13c)` → `idx=*(base+0x866664)` (bound `0<=idx<*(ctx+0x5c)`) → `player=*(*(ctx+0x58)+idx*4)` → `g_res=*(player+0x230)` (live encrypted container), `g_inc=*(player+0x80)` (income obj).
2. Rate fields +0x160/+0x164 are **signed ints** (fild) on the income object; +0x168 is the history-container **array** (count +0x16c), not a float rate. Reproduction of FUN_0086da39: latest `v=(snap−prev)+cur` (snap=`[ctx+0x108]`), else `v=cur`; negative→`+2^32`; `acc+=v*0.001f`; `sum+=decrypt(hist[j],slot)`; rate `= sum*(1.0f/acc)`, window 60.0f.
3. Constants confirmed from image bytes: 0x7856BC=1.0f, 0x79A5C8=0.001f, 0x7A12E8≈1e-6f, 0x7A12FC=4294967296.0f.
4. Retained from round 1: 0x44EFFF decrypt math, 0x86DF14 key table bytes, 0x86DF38 count=8, slots 0/1/2/7 = food/wood/coin/export.

## 4. Build status (round 9)
- Toolchain: w64devkit-x86 (i686 GCC 16.2.0)
- `d3d9.dll` compiles (`-shared -static-libgcc -O2 d3d9.c -o d3d9.dll d3d9.def -lwinmm`, **exit 0**)
- **84,501 B**, verified `pei-i386` (machine 0x14C); imports **KERNEL32/msvcrt only**; 11 exports
- Round-9 change: observer now logs TWO lines per change — NAMED `res food=%d wood=%d coin=%d export=%d` (map food=slot2, wood=slot0, coin=slot1, export=slot7) + RAW `raw 0..7 incA incB incN` trail; match-start separator `--- match start n=%d idx=%d ---` + snapshot reset on ctx/player/n change; every chain line carries `idx=%s`. Drawing layer still ABSENT, chain/decrypt/log tags/fault filter/authoritative+sticky logic UNCHANGED

## 4b. Test results (round 9)
- `tests\TESTER_nullsafe_O2.exe` (rebuilt -O2, real d3d9.c in TU): **10/10 PASS** (observer-sample no-op on NULL/menu/unmapped/no-device)
- `tests\TESTER_d3d9_actual.exe` (rebuilt): **ALL PASS** — prior chain/decrypt/rate(562.5)/fallback/authoritative checks kept; rebuilt `test_observer_log` proves the R9 contract: match-start separator `--- match start n=1 idx=0 ---`, named line `res food=500 wood=200 coin=30 export=80` (seeds slot2=500/slot0=200/slot1=30/slot7=80 — proves the map), raw trail `raw 0=200 1=30 2=500 ... 7=80` + `incA=2000 incB=1000 incN=3`, unchanged frame logs NOTHING, +3.0 wood bump re-logs exactly once (named+raw)
- `tests\test_pe_structure.py`: **0 FAILURES** (0x14C, 0x400000 base, statically imports d3d9.dll, no d3dx9 imports)
- `tests\test_source_contract.py`: **PASS** — round-8/9 negative draw checks kept; new R9 checks: `res food=%d wood=%d coin=%d export=%d` + `raw 0=... incA=.. incB=.. incN=..` format strings present, `rnd_i(v[2]), rnd_i(v[0]), rnd_i(v[1]), rnd_i(v[7])` map order, `--- match start n=%d idx=%s ---` separator, `g_obs_player` tracked (reset/authoritative/fallback) + `s_last_ctx/s_last_player/s_last_n` + match-start three-trigger regex + snapshot reset, chain `idx=%s`+`idxbuf`; probe-bypass slice still asserts NO `player_sane` vetoes an in-range idx

## 5. Deploy status (round 9)
- New `d3d9.dll` (84,501 B) copied byte-identical to `Age of Empires III - Complete Collection\`
- **SHA256 built == deployed == `tests\d3d9.dll` copy** (`50AA0D83EB2F57401664B76AD651A309DFF8EDB9E0AABD2C602FC6558E7ED0C2`)
- Binary scan CLEAN: `D3DXCreateFont`, `DrawText`, `SetRenderTarget`, `GetBackBuffer`, `BeginScene`, `EndScene`, `D3DFVF`, `d3dx9_25`, `ID3DXFont`, `Arial`, `gotbb`, `redrect` — nothing render-y survives
- Old `d3d9mod.log` **deleted** — the next run starts clean
- `d3dx9_25.dll` (2,337,488 B) still in game dir from round 1 — now UNUSED by the DLL
- Do NOT launch the game in build sessions — that's Dali's step

## 6. Anything that needs Dali (round-9 re-test)
- Launch `age3y.exe`, start a skirmish, gather for ~60 s. Watch each new game begin with a fresh `--- match start n=%d idx=%d ---` line.
- Expected healthy log: `DLL loaded`/`Create9`/`CreateDevice` once each → `chain src=game+0x14C ... idx=N [src-authoritative]` (or `[src-fallback] ... [sticky]`) → `--- match start n=.. idx=.. ---` → then `res food=%d wood=%d coin=%d export=%d` + `raw 0=.. 1=.. 2=.. 3=.. 4=.. 5=.. 6=.. 7=.. incA=.. incB=.. incN=..` pairs, first pair immediately then ONLY when a decoded slot moves by >= 0.5 (abs) or an inc int changes.
- **He checks food/wood/coin/export against his HUD at game start and reports which labels are right** (expected: food=slot2, wood=slot0, coin=slot1, export=slot7). If a label is wrong, fix the NAMED line only — the raw trail already prints all eight slots.
- **Acceptance check: the game must render PERFECTLY — no unit shatter, no flicker, no grass-shader artifacts, no HUD change, selection box normal.** The DLL reads but never writes device state; any corruption now is game-side, not ours.
- If it faults: log ends with `FAULT addr=%08X breadcrumb=<exact step> code=%08X` — that pinpoints the killer.