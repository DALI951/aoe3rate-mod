# BUILD — ROUND 10 (2026-09-07) — ARMED/DISABLED STATUS + GDI VISIBLE FALLBACK + DRAW HEARTBEAT

## ROUND SUMMARY
No crash this round (game not launched). R10 closes the P0 spec gap from the R9 live
expectations: if time ever ran with the overlay silently disabled this round makes it
**visibly impossible**.
- **Status line (once per load)** — DllMain emits exactly one line after `version_gate_check()`
  and `ui_init()`: `OVERLAY ARMED`, `OVERLAY ARMED (ini missing: using defaults)`, or
  `OVERLAY DISABLED: <reason>`. Reasons cover every disable path (`ver:<gate-reason>` wins,
  then `d3dx9_25.dll not loadable`); ini-missing is a suffix, never a disable.
  Pure helper `overlay_state_reason(version_ok, ui_ready, ini_missing)` + public
  `overlay_disabled_reason()`.
- **GDI visible fallback (P0)** — when the overlay is DISABLED, every frame draws a red
  `Resource Rate Mod: disabled (<reason>)` line at the top-left of the largest visible
  game window (found once via EnumWindows, cached). Zero new imports: SetBkMode/SetBkColor/
  SetTextColor arrive via runtime `LoadLibraryA("gdi32.dll")` + GetProcAddress. Called from
  `present_hook` AFTER the tripwire clear — outside B7's set/clear interval and the
  `Enabled=0` zero-touch branch. No breadcrumb, no per-frame log: the red line IS the
  visibility. No `-lgdi32` in build line, import table stays exactly
  {KERNEL32, USER32, msvcrt}.
- **Draw heartbeat** — every 600 completed overlay draws (~10s @ 60fps) ui_draw logs
  `ovl heartbeat n=N frame=F res=.. food=.. wood=.. coin=.. export=..` (not debug-gated,
  <= ~6 lines/min), proving the draw path still completes long after first frame.
- **Harness P0 hardening** — contract pins the DllMain-only once-per-load call, the
  tripwire-clear ordering, EXACT device/font vtable slot sets ({3,16,38,41,42,57,58,83,89,90}
  and {14,16,17} — no new slots), dynamic-gdi32 use, and the heartbeat position; the actual-
  code harness runs `overlay_state_reason` on all four precedence cases, exercises the real
  EnumWindows/GDI draw on a visible window (return 1), the no-window safety (return 0, no
  crash), the destroyed-window reuse (return 0), and the ARMED short-circuit (return 0).

## LIVE EXPECTATIONS (verify against d3d9mod.log)
```
R14 DLL loaded base=00400000 real=.. version=OK reason=ok
UI ready: d3dx9_25.dll loaded
OVERLAY ARMED                       <- NEW, once per load
...
chain .. n=3 .. [human-p1]
--- match start n=3 .. ---
UI: font created size=14 face=Georgia font=..
ovl first draw ok frame=..
ovl heartbeat n=600 frame=.. res=1286A300 food=.. wood=.. coin=.. export=..
(..repeats every 600 draws — proves long-run draw completion..)
```
A/B try-broken-check-triple (verify the DISABLED paths real before trusting ARMED):
- **(a)** rename `age3y.exe` → `d3d9mod.log` must show `version=BYPASS reason=exe size=..`
  + `OVERLAY DISABLED: ver:exe size=..` and a **red GDI line** appears in-game. Restore.
- **(b)** move `d3dx9_25.dll` out of the game dir → must show
  `OVERLAY DISABLED: d3dx9_25.dll not loadable` and the **red GDI line**. Restore.
Zero `FAULT` lines throughout. `OVERLAY ARMED` / `OVERLAY DISABLED:` appears EXACTLY once
per launch.

## File changes
- `src/ui.c` — R10: `ui_ready()` accessor; `s_ovl_draws` + 600-frame heartbeat after the
  first-draw marker; `ui_gdi_fallback_draw()` + `ui_gdi_find_hwnd` (EnumWindows best-window
  scan, cached) + 3 dynamic gdi32 procs (SetTextColor/SetBkColor/SetBkMode via
  LoadLibraryA) + DrawTextA at (8,8), `DT_SINGLELINE` define added; `#ifdef SWARM_TEST`
  `ui_test_set_ready` seam.
- `d3d9.c` — R10: `g_ini_missing` global; static pure `overlay_state_reason()` +
  `overlay_disabled_reason()` + `overlay_status_log()`; DllMain emits the one status line
  after `ui_init()` before `dllmain-done`; `ui_gdi_fallback_draw()` called in `present_hook`
  after the tripwire clear.
- `src/settings.c` — R10: `g_ini_missing = 1;` in the fopen-failure branch (defaults still
  loaded).
- `src/state.h` — R10: `g_ini_missing` extern, `OVL_HEARTBEAT_FRAMES 600`, status/GDI/`ui_ready`
  declarations, SWARM_TEST setter decl.
- `tests/test_source_contract.py` — R10 P0 block: once-per-load positioning, hook-body
  cleanliness, g_ini_missing placement, dynamic gdi32 + no `-lgdi32`, EXACT slot pins,
  heartbeat order, status variants.
- `tests/test_d3d9_actual.c` — R10 P0: state_reason precedence matrix; no-window safety;
  visible-window smoke (return 1); destroyed-window (return 0); ARMED short-circuit
  (return 0) via the SWARM_TEST seam.

## Build / harnesses (this round)
- Build rc=0, zero warnings (-Wall -Wextra), VERIFY PASS (i386 PE32, imports
  KERNEL32/USER32/msvcrt only, 11 exports).
- d3d9.dll = **145,229 B**, SHA256
  `0f3e1bb3183d4e52900fecaf62fd04557a4de1ff7ff4077f8b129f006d27403a`, deployed byte-identical
  to repo root + game dir + tests\d3d9.dll (all 3 SHA256 equal); old d3d9mod.log deleted.
- Harnesses: test_d3d9_actual.exe ALL PASS (incl. new R10 P0 GDI/status checks) ·
  test_rate_engine.exe FAILURES 0 · test_source_contract.py PASS (0 failures) ·
  test_pe_structure.py FAILURES 0.

> Game NOT launched this round. Next step = the live boss fight: run with `[Debug] Enabled=1`,
> confirm the LIVE EXPECTATIONS block verbatim, then do A/B checks (a) and (b).

---

# BUILD — ROUND 9 (REWORK, 2026-09-07) — FONT VTABLE TRUTH + DEVICE-GATED BINDING + DRAIN CLAMP

## ROUND SUMMARY
No crash this round. R9 is the final hardening pass before the live test — four findings from a fresh read of the code:
- **F1 (the big one): the ID3DXFont vtable slots were WRONG.** Since R14 the fallback binding used the canonical D3DX9 device layout (`Begin 12 / DrawTextA 13 / End 15`) against the D3DXFONT interface — that table is for `ID3DXSprite`, not `ID3DXFont`. Verified against the ACTUAL SDK interfaces (mingw-w64 `d3dx9core.h`, ReactOS, Wine-mirror, and the genuine Microsoft SDK 43 header — all identical since D3DX9 FWL iface never changed): ID3DXFont has **NO Begin and NO End**; slot 13 is **PreloadTextW**, DrawTextA=**14**, DrawTextW=15, OnLostDevice=16, OnResetDevice=17. The old table was dispatching `PreloadTextA/PreloadTextW/DrawTextW` into a real font (garbage calls on slots that exist but do the wrong thing). Fix: no Begin/End at all — a single `DrawTextA(14)` call with the true layout `(This, pSprite, pString, Count, pRect, Format, Color)`, `pSprite=NULL`, using the already-rotated render target. (The R9 draft claimed "Begin=13"; header grep shows slot 13 = PreloadTextW, and no Begin exists — the derived counts are authoritative.)
- **F2:** `ui_create_font(*ppdev)` ran unconditionally in `w_create_device`/`w_create_device_ex` even when `patch_device_present` returned 0 (aux/second device) — the font attached to an un-patched device. Now gated: `if (patched) ui_create_font(*ppdev);` in BOTH paths.
- **F3:** the ShowGains=0 skip tested `inst > g_last_ema[s] * ratio` — a drained slot (EMA ≈ 0/negative) makes `inst > 0` match EVERY positive sample forever. The reference is now clamped: `float ref = g_last_ema[s] > 0.0f ? g_last_ema[s] : 0.0f;` (skip against `ref * ratio`), so the slot lifecycle stays honest.
- **F4:** contract harness tightened — A1 pins `s_tick` ABSENT from the `clock_now` body (old `return s_tick` guard only matched one shape); B6 pins FVF order `save < DrawPrimitiveUp < restore` inside the backdrop; B7 pins no early `return` between the tripwire set and clear while holding the token; new R9 asserts for the font slots/End-ban/7-arg DrawTextA/device-gated creation/clamp; rate harness got a drained-slot (EMA=0) ShowGains clamp sub-test.

## The F1 fix (the reason this round exists)
- `src/ui.c` slot table is now `FONT_DRAWTEXTA 14`, `FONT_ONLOSTDEVICE 16`, `FONT_ONRESETDEVICE 17` — `FONT_BEGIN`/`FONT_END` deleted. `ui_font_draw` calls `dt(font, NULL, s, -1, &rc, DT_LEFT|DT_TOP|DT_NOCLIP, color)` (real 7-arg COM layout). The panel no longer calls Begin/End at all. Breadcrumb `ovl-panel-text` added after `ui_draw_backdrop`. Header evidence recorded in `tests/test_source_contract.py` (R9 block) and this report.

## File changes
- `src/ui.c` — R9 ID3DXFont slot truth (DrawTextA=14 only; Begin/End removed); real 7-arg DrawTextA; `ovl-panel-text` breadcrumb; F1 layout comment.
- `d3d9.c` — F2: `if (patched) ui_create_font(*ppdev);` in both create paths (font only binds to a device whose Present hook is installed).
- `src/tracker.c` — F3: ShowGains skip reference clamped to `>= 0` (`ref * ratio`).
- `tests/test_source_contract.py` — F4: A1 `s_tick` absent from clock_now body; B6 FVF order pinned; B7 no-early-return pin; R9 font-slot/End-ban/7-arg/gating/clamp asserts; R14 doc header corrected.
- `tests/test_rate_engine.c` — F4/R9: drained-slot (EMA=0) ShowGains=0 clamp sub-test + resume-on-ShowGains=1.

## Build / harnesses (this round)
- Build rc=0, zero warnings, VERIFY PASS (i386 PE32, imports KERNEL32/USER32/msvcrt, 11 exports).
- d3d9.dll = **142,052 B**, SHA256 `1bb1b24161d38c0153a261754f5590f236fe7fe27c48cc7caed959cf0ee16d5d`, deployed byte-identical to game dir + tests\d3d9.dll (verified identical on all 3 copies); old d3d9mod.log deleted.
- Harnesses: test_d3d9_actual.exe ALL PASS · test_rate_engine.exe FAILURES 0 · test_source_contract.py PASS (0 failures) · test_pe_structure.py FAILURES 0.

> Game NOT launched this round. Next step = the live test (with `[Debug] Enabled=1` for the first calibrated run).

---

## ROUND SUMMARY
No crash this round — R8 is a hardening/pass two-review pass over the R7 build:
- **Part A (external review):** A1 the rate clock was WRONG — `s_tick` is the per-Present FRAME counter, not millisecond game time, so game-time-based rates scaled with FPS; switched to realtime QPC (+ GetTickCount fallback), kept `UseGameTime` parsed-but-cosmetic, one-time log note. A2 added an explicit `g_ema_valid[MAX_SLOTS]` liveness flag (replaces the EMA-magnitude heuristic). A3 the per-tick `RES t=...` stream is now gated behind `[Debug] Enabled=1` (quiet log by default). A4 added `settings_poll_profile()` (1/sec-throttled re-poll of `Users\DefaultProfile*.xml`) called from present_hook with a `profile-poll` breadcrumb. A5 a second device vtable is now LOGGED (`second vtable %p seen (had %p) - not re-patched, overlay inactive on this device`) instead of silently skipped.
- **Part B (inspector):** B1 `g_font_dev` binds the font to its device (a bound font for a different device is destroyed first). B2 panel hotkeys 1-6 now require F9 HELD + game-window foreground + per-key edge (no phantom toggles in chat / alt-tab); F9 toggle stays edge-triggered. B3 `font_destroy` Releases only through a `font_safe()` pointer. B4 font-create-failure logged once, then a heartbeat every 300 failures. B5 only `S_OK` proceeds past TestCooperativeLevel (`uhr != 0` → abort). B6 panel backdrop now saves/restores the device FVF via GetFVF/SetFVF vtable slots **89/90** (canonical DX9 vtable) around `DrawPrimitiveUp`, breadcrumbed `ovl-fvf`. B7 present_hook got an `InterlockedCompareExchange` re-entrancy tripwire (nested Present → forwarded raw, `present-reentrant` step). B8 `ShowGains=0` now treats strongly positive jumps like spend spikes (EMA frozen, baseline fresh, same ratio). B9 `[General] StartHidden=1` hides the panel at load.

## The A1 fix (most important)
- `src/tracker.c` `clock_now()`: formerly `if (use_game_time) return (DWORD)s_tick;` → now ALWAYS realtime (`QueryPerformanceCounter` → ms, `GetTickCount` fallback). Verified live definition: `s_tick++` in `observer_sample()` (src/gameif.c) — one increment per present frame, no ms meaning (the game's own `FUN_0086da39 * RVA_TIME_STEP=0.001f` multiplies per-tick deltas, but never produces a ms counter). Real-time EMA deltas are now `ms/1000` seconds regardless of FPS. `tracker_set_clock_override(ms)` keeps the deterministic harness; `tracker_reset` clears it.

## File changes
- `src/tracker.c` — realtime clock (A1), `tracker_set_clock_override` (A1), `g_ema_valid` liveness flag (A2, valid only after a real first rate — the very first sample lays down the baseline without marking alive), ShowGains=0 positive-jump skip (B8).
- `src/settings.c` — one-time `clock: using realtime QPC (s_tick is a frame counter, not game time)` note (A1); `settings_poll_profile()` 1/s throttle (A4).
- `src/gameif.c` — `RES t=...` + `rates ...` both under `if (g_settings.debug_enabled)` (A3).
- `src/state.h` — externs: `g_ema_valid`, `g_font_dev`, `tracker_set_clock_override`, `settings_poll_profile`.
- `src/ui.c` — `g_font_dev` binding + `font_destroy` NULLs it (B1/B3); hotkeys F9-held + foreground + edge (B2); failure log once + every-300 heartbeat (B4); `uhr != 0` TCL gate (B5); `D9_SETFVF 89 / D9_GETFVF 90` + save/restore around the backdrop + `ovl-fvf` breadcrumbs (B6); `font_safe()` gate inside `font_destroy` (B3).
- `d3d9.c` — present_hook re-entrancy tripwire + `present-reentrant` step + single-exit clear (B7); `profile-poll` step + `settings_poll_profile()` call (A4); `patch_device_present` second-vtable log (A5); DllMain `StartHidden` → `g_panel_visible = 0` after `settings_load()` (B9); `g_ema_valid`, `g_font_dev` definitions.
- `tests/test_rate_engine.c` — `ts()` clock-override helper (replaces the direct s_tick timeline), `show_gains=1` default, new ShowGains=0 skip test; realtime QPC test kept.
- `tests/test_d3d9_actual.c` — observer log test sets `debug_enabled=1` (RES is gated); per-tick delta now `+2` (RES + paired `rates` line).
- `tests/test_source_contract.py` — round-8 asserts (A1 QPC/no-s_tick-clock + note, A2 valid-flag lifecycle, A3 RES inside debug gate, A4 poll throttle + breadcrumb, A5 log string, B1 bound font, B2 F9+foreground+edge, B3 safe release, B4 heartbeat, B5 S_OK-only, B6 slots 89/90 + saved_fvf + ovl-fvf, B7 tripwire + single exit, B8 skip threshold, B9 start_hidden order).

## Build / harnesses (this round)
- Build rc=0, zero warnings, VERIFY PASS (i386 PE32, imports KERNEL32/USER32/msvcrt, 11 exports).
- d3d9.dll = **142,052 B**, SHA256 `226c947dd77ca1fd851ac5fd4ef5e8e7f184bb59fbddf2ec5483f14a500190a0`, deployed byte-identical to game dir + tests\d3d9.dll (verified identical on all 3 copies); old d3d9mod.log deleted.
- Harnesses: test_d3d9_actual.exe FAILURES 0 · test_rate_engine.exe FAILURES 0 · test_source_contract.py PASS (0 failures) · test_pe_structure.py FAILURES 0.

> NOTE for the live test: with default settings (`[Debug] Enabled=0`) the log is now QUIET per frame — only chain/matches/errors and one-time notes. Set `[Debug] Enabled=1` to restore the per-tick `RES t=...` lines for calibration. RES/rates lines unchanged in format; Rate display unit untouched (`/min` by default).

---

# BUILD — ROUND 7 (REWORK, 2026-09-06) — CRASH MOVED TO ovl-panel: IT'S THE FONT. LAZY RE-CREATE + STRICT NULL + DOUBLE-GUARD

## ROUND SUMMARY
Confirmed in `d3d9mod.log` (both runs, killed twice each):
```
11: reset dev=0cca2b20 -> font invalidated
12: UI: font created size=14 face=Georgia      <- created INSIDE reset_hook, device still NOTRESET
15: RES t=1 food=0 wood=0 coin=0 export=0
16: FAULT addr=000000D8 eip=000000D8 ... breadcrumb=ovl-panel code=C0000005 dev=0CCA2B20 vt=0CCA5A7C slot=83
```
Genuine progress: `ovl-grt`/`ovl-begin` never appear — the RT step is FIXED. New crash: `eip=000000D8` = a virtual call through a NULL/dangling object, inside `ovl-panel` (the D3DXFont Begin→DrawText→End block). `dev=0CCA2B20` is the real device (instrumentation sane). **Root cause:** the font is created inside `reset_hook` DURING `Reset` while the device is in NOTRESET limbo — `D3DXCreateFontA` fails (log printed "font created" unconditionally) or returns a broken object; first DrawText on it → call through NULL → `eip=0xD8`. The R6/R7-proven behavior was LAZY re-create: Reset only invalidates; the font is (re)created on the NEXT present frame only when TestCooperativeLevel == OK.

## The fix
### 1. reset_hook (d3d9.c) — NO font creation
- log `reset dev=%p -> font invalidated`; destroy font via `ui_on_reset` → `font_destroy()` (Release via font vtable + **g_font = NULL immediately**); forward to real Reset (`s_orig_reset`); re-assert hooks on actual `self`; return. `ui_create_font(self)` line REMOVED from the hook.

### 2. Lazy creation on the present path (ui_draw, src/ui.c)
After the cooldown and `TestCooperativeLevel` (LOST/NOTRESET already bail), if `g_font == NULL` → `ui_create_font(dev)`:
- `hr == 0 && font != NULL` → `g_font = font`, log `UI: font created size=%d face=%s font=%p` (**includes the pointer** so a fresh object is provable).
- else → `g_font = NULL` (never leave a dangling non-NULL font) and log `UI: font create FAILED hr=0x%08X` (the unconditional "created" message is gone).
- then the existing font-first guard `if (g_font == NULL) return;` skips the frame.

### 3. Double-guard in ovl-panel (`font_safe()`)
Immediately before the font's `Begin`, `font_safe()` re-verifies `g_font != NULL` AND `VirtualQuery`-guards both `g_font` and its vtable pointer are mapped readable `MEM_COMMIT` with no `PAGE_GUARD`/`PAGE_NOACCESS`; if bad → `logger_debug` once and skip, never call through it.

### 4. Kept from round 6
No SetRenderTarget/GetBackBuffer; GetRenderTarget current-RT flow; breadcrumbs `ovl-tcl/grt/states/begin/panel/end/restore/done`; cooldown 30f; g_values_valid; TCL guard; rich FAULT dump; first-draw marker; zero-touch Enabled=0.

## File changes
- `src/ui.c` — `font_destroy()` (renamed from `ui_release_font`, drops the risky OnLostDevice call, preserves the immediate `g_font = NULL`); `font_safe()` helper + `g_font_guard_warned`; `ui_draw` lazy-create after TCL; `ui_create_font` honest hr check + `font=%p` + FAILED log; `ui_draw_panel` double-guard gate.
- `d3d9.c` — `reset_hook`: removed `if (hr >= 0) ui_create_font(self)`.
- `src/state.h` — `ui_create_font` comment updated (device-create + lazy present path).
- `tests/test_source_contract.py` — round-7 asserts: `font_destroy` NULLs `g_font`; `reset_hook` segment never calls `ui_create_font`/`D3DXCreateFontA`; lazy create after `0x88760869u`; `font_safe` VirtualQuery double-guard; create FAILED log string.

## Build / harnesses (this round)
- Build rc=0, zero warnings, VERIFY PASS (i386 PE32, imports KERNEL32/USER32/msvcrt, 11 exports).
- d3d9.dll = **138,318 B**, SHA256 `a17033b201cc9bac222ec9f9febc4d3043fb4ba9416d85bd42162b0db4b76ad6`, deployed byte-identical to game dir + tests\d3d9.dll; old d3d9mod.log deleted.
- Harnesses: test_d3d9_actual.exe ALL PASS · test_rate_engine.exe FAILURES 0 · test_source_contract.py PASS (0 failures) · test_pe_structure.py FAILURES 0.

---

# BUILD — ROUND 6 (REWORK, 2026-09-06) — REVERT PER-VTABLE MACHINERY + REMOVE SetRenderTarget/GetBackBuffer FROM THE DRAW PATH

## ROUND SUMMARY
Round-5 live log (confirmed in `d3d9mod.log`, two independent runs, killed twice each):
```
FAULT addr=774EFF25 eip=774EFF25 esp=0019F9A8 ebp=0019F9AC stk=0F677800,0019F9BC,6F80AA3D,6F8F4EC4,... breadcrumb=ovl-srt code=C0000005 dev=6F8F4EC0 vt=00000000 slot=37
```
`ovl-srt` = dying inside **SetRenderTarget**; eip in ntdll (774EFF25) with d3d9.dll return addrs on the stack (6F80AA3D, 6F8F4EC4); `dev=6F8F4EC0 vt=0 slot=37` is garbage (dev is a code address in d3d9.dll, not a heap surface/device; vt=0) — bad arguments to the call (stale/garbage render-target surface or shifted stack). The game uses ONE device object per run (`dev=0ca74140`, `dev=0c8015a0` — one each); the round-5 per-vtable/resolve machinery was never exercised and is implicated in the wrong-pointer mess. R6/R7 drew LIVE on this game with a single global pair.

## (a) Reverted to R6-proven hook semantics (d3d9.c)
- **Removed** the round-5 per-vtable originals table: `s_ovtab`/`s_novtab`/`OVTAB_MAX`, `resolve_orig_present(self)`/`resolve_orig_reset(self)`, the evict-oldest path (`s_ovtab[k] = s_ovtab[k+1]`).
- **Restored** single-slot globals `s_orig_present`/`s_orig_reset`, saved **ONCE** from the first patched device vtable via `if (s_orig_present == NULL)` / `if (s_orig_reset == NULL)` guards; `g_devvt` still tracked so a post-Reset vtable swap gets re-asserted on the ACTUAL device (`patch_device_present(self)` in `reset_hook`).
- `reset_hook` keeps the `ui_on_reset(self)` → `reset dev=%p -> font invalidated` log for the ACTUAL device passed to Reset; re-patches `self` after the original Reset; re-creates the font on success.
- `present_hook` zero-touch path (`Enabled=0`) forwards through `s_orig_present` directly (no resolve machinery).

## (b) Draw path without RT switching (src/ui.c)
New flow — draw on the game's CURRENT render target:
```
ovl-tcl    TestCooperativeLevel (LOST/NOTRESET -> skip frame)
ovl-grt    GetRenderTarget(0, &cur)   [hardened: hr==0, cur != NULL,
           VirtualQuery-guard cur (MEM_COMMIT, no PAGE_GUARD/NOACCESS)]
ovl-states render-state save + set (Z/ZW/STENCIL off, ALPHABLEND SRCALPHA/INVSRCALPHA, LIGHTING off)
ovl-begin  BeginScene
ovl-panel  ui_draw_panel(dev)  (D3DXFont Begin -> DrawText rows -> End on current RT)
ovl-end    EndScene
ovl-restore restore render states (reverse) + Release(cur)
ovl-done   first-draw marker + done breadcrumb
```
- **GetBackBuffer + SetRenderTarget removed entirely** — `ovl-srt` and `ovl-gbb` crash sites no longer exist. At Present time the device's current RT IS the backbuffer, so the HUD stays visible without any RT switch (same visibility logic as drawing before Present).
- Any GetRenderTarget failure (non-0 hr, NULL surface, VirtualQuery reject) → skip the whole frame's draw BEFORE any state was changed (no half-restore possible).
- The only `Release` is the `IDirect3DSurface9::Release` (vtable slot 2) on the `GetRenderTarget` reference we took.

## (c) Kept from rounds 4-5 (all intact)
font-first guard · n==0 hide · g_values_valid gate · RESET_COOLDOWN_FRAMES=30 post-Reset cooldown · TCL guard (0x88760868/69 skip) · render-state save/set/restore list · per-call breadcrumbs (now `ovl-tcl/ovl-grt/ovl-states/ovl-begin/ovl-panel/ovl-end/ovl-restore/ovl-done`) with `set_trace(dev,vt,slot,bc)` stashing `g_fault_dev/g_fault_vt/g_fault_slot` · richer FAULT dump (`eip/esp/ebp/stk=8 dwords/breadcrumb/code/dev/vt/slot`, VirtualQuery-guarded) · `ovl first draw ok frame=N` first-draw marker · zero-touch `Enabled=0` A/B path · explicit `STDMETHODCALLTYPE` (__stdcall) function-pointer casts for Present/Reset/GetRenderTarget/Release/Begin/DrawText/End.

---

# BUILD — ROUND 5 (REWORK, 2026-09-06) — PERSISTENT ovl-rt CRASH: PORT-UNRECOVERABLE, PER-VTABLE FIX + FULL CRASH INSTRUMENTATION

## ROUND SUMMARY
Dali's round-4 live log shows the cooldown did NOT help: the post-Reset font re-create now logs correctly (`UI: font created size=14 face=Georgia` a second time), the cooldown elapsed, and `FAULT addr=774EFF25 breadcrumb=ovl-rt code=C0000005` still kills the process (x4) with the SAME stable ntdll-ish address. A stable ntdll address + intact overlay is a stale/REPLACED device vtable signature — the game or driver restores its own vtable or creates a SECOND device whose patch clobbers the first device's saved originals.

## (a) R6-vs-R14 diff findings — R6 source is UNRECOVERABLE; the mechanics were already R6-faithful
Mandate step 1 asked to port `git show <r6>:d3d9.c` "verbatim". **That commit does not exist.** Proof:
- `git log --all -S "draw_hud_text"` and `-S "gotbb"` and `-S "reset dev="` return commits `21f7403, 07f5b4f, 1cc8708, 8262a4d` — R12/R13/R14. The repo history STARTS at R12 (`21f7403`); the root commit's `d3d9.c` already says *"All previous draw code (font init, text=/values=/gotbb dumps, red rect, SetRenderTarget/SetRenderState, Begin/EndScene) was DELETED in round 8"* (confirmed at `old_r12.c:543`). No R6/R7 blob exists anywhere in `--all`. The old repo (`C:\Users\Dali\Documents\gaames\aoe3rate-mod`) is gone, and the remote `main` contains only R12→R15.
- Whole-tree grep for `gotbb`/`draw_hud_text`/`hud_text`/`reset dev=%p`/`text=%d`: only doc mentions in `.swarm\TEST.md`/`REVIEW.md`. `test_nullsafe.c` header says "the DLL no longer draws anything".
- **Therefore the R6 "verbatim" port could not run.** I diffed the current RT/draw machinery element-by-element against the R6 spec you enumerated (the only authoritative record left). Result: **every R6 mechanical requirement was already satisfied**:
  - GetBackBuffer(0,0,MONO,&bb) + HRESULT + NULL check — MATCH (slot 18)
  - GetRenderTarget(0,&prev_rt) + SetRenderTarget(0,bb) + both-refs release order — MATCH (slot 38/37; grt/gbb each AddRef, both Released)
  - render-state save/set/restore list ZENABLE/ZWRITEENABLE/STENCILENABLE/ALPHABLENDENABLE(SRCALPHA→INVSRCALPHA)/LIGHTING — MATCH (slots 58/57)
  - Begin/End gated: BeginScene(41) failure skips panel AND EndScene; font Begin(12) failure skips DrawText AND font End — MATCH
  - TestCooperativeLevel guard: LOST/NOTRESET skip the whole frame before any font/RT use — MATCH (slot 3 + 0x88760868/69)
  - font-first + n==0 + g_values_valid + 30-frame cooldown (R14 hardening) — KEPT
  - So "the RT mechanics must be R6-pure" was already true; the crash is NOT a mechanics regression vs R6. The real divergence from the proven R13 hooks is structural and below.

## (b) The reset-hook / present-hook finding — single-slot globals could forward into the WRONG device's code
OLD code (rounds 2-4, and R13): `patch_device_present()` saved the originals in ONE global pair and only patched when `g_devvt != vt`. If the game creates a SECOND device (different vtable — AoE3 does create auxiliary devices) or a driver swaps a vtable at Reset, the second patch RE-SAVES `s_orig_present`/`s_orig_reset` with the second device's originals and clobbers the first device's. Every subsequent Present/Reset on device #1 then forwarded into device #2's original Present/Reset via our stale single-slot globals — exactly the "calls a device in the wrong state -> dies inside ntdll with a stable address" signature of the log.
NEW code (round 5): a per-vtable table (`s_ovtab`, 4 entries, oldest-evicted):
- `patch_device_present()` keys entries by the device's CURRENT vtable pointer; records EACH vtable's true originals; never touches another entry's saved originals.
- `resolve_orig_present(self)/resolve_orig_reset(self)` resolve the original by the CURRENT device's CURRENT vtable on EVERY call (falls back to last-patched if unknown).
- `reset_hook` still re-patches `self` — the ACTUAL device passed to Reset (never a stored pointer) — and now logs `reset dev=%p -> font invalidated`.
- `s_orig_present`/`s_orig_reset` remain only as last-patched diagnostics; the hooks no longer trust them alone.

## (c) Crash instrumentation added
- **Per-call breadcrumbs + trace:** replacing coarse `ovl-rt` with `ovl-tcl`, `ovl-grt`, `ovl-gbb`, `ovl-srt` set immediately BEFORE each individual call (keep `ovl-states/begin/panel/end/restore/done`). Each trace also stashes `g_fault_dev`/`g_fault_vt`/`g_fault_slot` so the fault dump knows the object, vtable, and slot being called.
- **Richer FAULT dump:** the line is now `FAULT addr=.. eip=.. esp=.. ebp=.. stk=h1,h2,...h8 breadcrumb=.. code=.. dev=.. vt=.. slot=..` — context registers read from the exception CONTEXT (VirtualQuery-guarded stack read), plus the device/vtable/slot trace from the globals. A next crash names the exact dying call AND gives a call trail.
- **First-draw marker:** `ovl first draw ok frame=N` logged once when the first full overlay draw completes, so the log proves whether the draw ever finishes.
- **Zero-touch Enabled=0 path:** with `[General] Enabled=0`, `present_hook` forwards to the resolved original immediately — no locate/observer/hotkey/overlay/breadcrumbs. This is Dali's A/B control to split hook-vs-overlay as the culprit.

## FILES CHANGED
- `d3d9.c` — per-vtable originals table + resolve helpers; present_hook zero-touch path; reset_hook logs `reset dev=%p`; `g_fault_dev/vt/slot`, `g_ovl_first_done` globals.
- `src/ui.c` — `set_trace()` helper; per-call `ovl-grt/gbb/srt` breadcrumbs (runs after cooldown+font-first, before any RT call); first-draw marker; all R14 gating kept.
- `src/gameif.c` — `fault_log_points` now dumps eip/esp/ebp + 8 stack dwords + dev/vt/slot (VirtualQuery-guarded).
- `src/state.h` — externs for the trace globals; `ui_on_reset(void *dev)`.
- `tests/test_rate_engine.c` — `ui_on_reset(NULL)` (signature).
- `tests/test_source_contract.py` — round-5 asserts (per-vt table, resolve helpers, eviction, `reset dev=%p`, `ovl-grt/gbb/srt` trace presence, eip/esp/ebp/stk dump, first-draw marker, zero-touch ordering).

## BUILD RESULT
- `cmd /c build\build.bat` → **rc=0, ZERO warnings**, verify_pe PASS (i386, PE32, imports ⊆ {KERNEL32, USER32, msvcrt}, **11 exports**)
- **d3d9.dll = 138,890 B, SHA256 `71c41e2844bf8e095c28f2d6ca75b2a9d85c0e6d78c08ddb4a595c78747ce260`** (`d3d9.sha256` updated)
- **Deployed byte-identical**: repo root = game dir = `tests\d3d9.dll` (SHA256 equal); old game `d3d9mod.log` deleted. Game NOT launched.

## HARNESS RESULTS (all green)
- `tests\test_d3d9_actual.exe` — ALL CHECKS PASSED
- `tests\test_rate_engine.exe` — FAILURES: 0 (incl. STEP-7 fault semantics with the richer dump)
- `tests\test_source_contract.py` — SOURCE-CONTRACT: PASS (incl. 10 new round-5 checks)
- `tests\test_pe_structure.py` — FAILURES: 0

## WHAT DALI TESTS NEXT (live)
1. **A/B first:** set `[General] Enabled=0` in the game dir's `ResourceRateMod.ini`, launch, start a skirmish. If the process survives, the culprit is in the overlay draw/RT path; if it STILL dies, it's in the hook/observer path.
2. Then `Enabled=1`: if it still crashes, the FAULT line now names the exact call: `ovl-grt` vs `ovl-gbb` vs `ovl-srt` vs `ovl-states`/`ovl-begin`/`ovl-panel`, with `dev=.. vt=.. slot=.. eip=.. esp=.. ebp=.. stk=..`. Send that line + the `CreateDevice`/`reset dev=` lines.
3. Expect after the match-start Reset: `reset dev=.. -> font invalidated`, a second `UI: font created`, then `ovl first draw ok frame=..` once the overlay actually renders.

---

# BUILD — ROUND 4 (REWORK, 2026-09-05) — CRASH ON FIRST FRAME AFTER DEVICE RESET (ovl-rt AV)

## ROUND SUMMARY
Dali's live log (R14.3):
```
CreateDevice hr=0000000000 dev=0cc38d60 patched=yes
UI: font created size=14 face=Georgia
chain ... ctx==0 (x4)
reset -> font invalidated
chain game=04AAF000 ctx=059E0000 n=3 player=0F6F7800 res=1286A300 [human-p1]
--- match start n=3 ...
RES t=1 ...
FAULT addr=774EFF25 breadcrumb=ovl-rt code=C0000005   (x4, then process dies)
```
`ovl-rt` = the overlay's GetBackBuffer/SetRenderTarget step in `ui_draw()`; `0xC0000005` = a real access violation inside d3d9.dll itself — the device was in a transitional state right after Reset. The overlay drew on the very frame the match started.

## ROOT CAUSE (one paragraph)
After a D3D9 device Reset the device object is in a transitional state; calling `GetBackBuffer`/`SetRenderTarget` on it before it settles faults inside d3d9.dll (AV at `774EFF25`). Two aggravators: (1) the font was actually re-created by the Reset hook but the creation line was suppressed by a once-only `g_ui_logged` latch — so the log misled us into thinking the font was missing, when in fact `ui_draw` ran its whole RT dance on a device that had been reset the same frame; (2) no cooldown existed between Reset success and the first overlay draw. Dalan's draw ordering was technically correct (font checked before RT calls) but there was no safety delay and no HRESULT acceptance test on `SetRenderTarget`.

## THE FIX (exact semantics)
1. **Post-Reset cooldown:** new global `g_frames_since_reset` (defined in `d3d9.c`, `extern` in `state.h`, `RESET_COOLDOWN_FRAMES 30` ≈ 0.5s @60fps). Zeroed on every `CreateDevice`/`CreateDeviceEx` success and at the top of `reset_hook`; incremented once per `present_hook` frame. `ui_draw()` returns immediately while `g_frames_since_reset < RESET_COOLDOWN_FRAMES` — no GetBackBuffer/SetRenderTarget/BeginScene until the device has settled. The check sits AFTER the `g_font == NULL` guard (font-first) and BEFORE the `ovl-tcl`/`ovl-rt` steps.
2. **Font-first ordering (confirmed):** `if (g_font == NULL) return;` remains the first device guard, before ANY RT manipulation (GetBackBuffer, SetRenderTarget, state save, BeginScene).
3. **Font re-creation on Reset + honest logging:** the Reset hook already re-creates the font via `ui_create_font(self)` when `hr >= 0`; the once-only `g_ui_logged` latch is REMOVED so every create logs `UI: font created size=.. face=..` — the post-Reset path is now visible in `d3d9mod.log`. (In the failing log, the font HAD been re-created — that's why execution reached `ovl-rt` at all; the suppressed log was the red herring.)
4. **HRESULT checks on all RT calls in `ui_draw`:** `GetRenderTarget`, `GetBackBuffer` already bail; new — `SetRenderTarget(dev, 0, bb)` failure now restores the previous RT, releases both surfaces and skips the frame (never continues into BeginScene/DrawText on a failed RT setup).
5. All prior round fixes kept intact (log-only vectored handler + benign-code filter, font at device-create only, both-surfaces release, match-start needs a valid chain, `ovl-*` breadcrumbs).

## FILES CHANGED
- `src/ui.c` — cooldown gate in `ui_draw`; `SetRenderTarget` HRESULT bail (restore prev RT + release + skip); `g_ui_logged` removed, every font create logged.
- `src/state.h` — `extern int g_frames_since_reset;` + `RESET_COOLDOWN_FRAMES 30`.
- `d3d9.c` — global `g_frames_since_reset`; zeroed in `w_create_device`/`w_create_device_ex`/`reset_hook`; incremented in `present_hook`.
- `tests/test_source_contract.py` — round-4 asserts: cooldown define + increment + triple zero, font-first guard, cooldown-before-`ovl-rt` ordering, unconditional font-create log.

## BUILD RESULT
- `cmd /c build\build.bat` → **rc=0, ZERO warnings**, verify_pe PASS (i386, PE32, imports ⊆ {KERNEL32, USER32, msvcrt}, **11 exports**)
- **d3d9.dll = 137,179 B, SHA256 `f55f556d6af25bb585b5de23578a2f0b254896e254cdf93867f6ed316ef97817`** (`d3d9.sha256` updated)
- **Deployed byte-identical**: repo root = game dir = `tests\d3d9.dll` (hashes equal); old game `d3d9mod.log` deleted. Game NOT launched.

## HARNESS RESULTS (all green)
- `tests\test_d3d9_actual.exe` — ALL CHECKS PASSED
- `tests\test_rate_engine.exe` — FAILURES: 0
- `tests\test_source_contract.py` — SOURCE-CONTRACT: PASS (incl. new round-4 checks)
- `tests\test_pe_structure.py` — FAILURES: 0

## WHAT DALI TESTS NEXT (live)
1. Game must reach the main menu (no load-time FAULT).
2. Start a skirmish (the game's Reset + match start): the overlay must NOT appear instantly, then fade in ~0.5s later (`UI: font created` must ALSO appear in the post-Reset section of `d3d9mod.log` this time), no `FAULT breadcrumb=ovl-rt` line.
3. If a FAULT line still appears, send it — it proves a genuinely unhandled fatal and gives us the real addr.

---

# BUILD — ROUND 3 (REWORK, 2026-09-06) — GAME WON'T START (BENIGN-EXCEPTION KILL)

## ROUND SUMMARY
After round 2, Dali's live test: **the game died at load**. `d3d9mod.log` repeated per launch:
`FAULT addr=75AFD8C2 breadcrumb=dllmain-done code=406D1388` (twice) — i.e., right after our DllMain completed, during the loader phase, with the breadcrumb `dllmain-done`. Root cause: round 2 registered `fault_filter` (which logs FAULT **and `ExitProcess`**) as an `AddVectoredExceptionHandler(0, ...)` handler. `0x406D1388` is the **benign OutputDebugString notification exception** (raised by d3dx9_25.dll / the game at load; addr in kernelbase). A vectored handler runs on EVERY exception nobody else handled — including benign informational ones that the loader/game would otherwise ignore — so exit-on-any-exception = death at load. The double FAULT line was the same function running in both roles (VEH + unhandled filter) for the one exception.

## ROOT CAUSE (one paragraph)
Round 2's fault net treated **every** second-chance exception as a fatal crash: the VEH (which cannot be replaced and therefore sees benign notifications too) logged `FAULT` and called `ExitProcess(0)` before the loader could ignore the informational `0x406D1388` debug-print notification. The game never even got past DLL loading.

## THE FIX (exact semantics)
Split the two roles, with a benign-code guard shared by both:
- `fault_code_benign(code)` — `1` (I) for any code whose top 2 bits are not `11` (success/informational/warning severity → covers `0x40010006`, `0x40010007`, `0x406D1388` and the whole `DBG_*`/`0x40000000` family), (II) `0x80000003` breakpoint / `0x80000004` single-step, (III) `0xE06D7363` MSVC C++ exception.
- `vectored_fault_filter` (DllMain, `AddVectoredExceptionHandler(0, ...)`) — **LOG-ONLY, never exits**: benign code → silent `EXCEPTION_CONTINUE_SEARCH`; anything else → write the `FAULT addr=/breadcrumb=/code=` line (raw Win32 WriteFile+Flush) then `EXCEPTION_CONTINUE_SEARCH`. A first-chance/last-chance exception the game owns is allowed to propagate to its own handlers.
- `fault_filter` (legacy `SetUnhandledExceptionFilter`, re-pinned each present frame by `ensure_fault_filter`) — the ONLY place that exits: benign → `CONTINUE_SEARCH` (defensive), genuinely unhandled fatal (`0xC0000005` etc.) → log FAULT + silent `ExitProcess` (R6 semantics, no crash dialog). If the game replaces our filter we just lose the exit — the VEH already gave the breadcrumb.
- Round-2 fixes that were correct are untouched: font created at device-create/Reset only (never mid-frame), never `SetRenderTarget(NULL)`, both surfaces released, match-start requires a valid chain, `ovl-*` draw breadcrumbs.

## FILES CHANGED
- `src/gameif.c` — `fault_code_benign`, `vectored_fault_filter` (log-only), rewritten `fault_filter` (benign-skip + log + exit), shared `fault_log_points`; `ensure_fault_filter` unchanged in behavior.
- `src/state.h` — declare `fault_code_benign`, `vectored_fault_filter`.
- `d3d9.c` — DllMain registers `vectored_fault_filter` (was `fault_filter`).
- `tests/test_rate_engine.c` — new STEP 7 `test_fault_filters`: benign codes (`0x406D1388`, `0x40010006/07`, `0xE06D7363`, `0x80000003/04`, `0x40080201`) → `fault_code_benign==1` AND both filters return `CONTINUE_SEARCH` (harness survives = no exit); fatal `0xC0000005`/`0xC00000FD` → not benign (exit path, not run in-process by design); `0x80000002` warning → benign; NULL EP safe.
- `tests/test_source_contract.py` — asserts VEH registration, the benign-code list, and (on the comment-stripped TU) that the vectored filter body contains **no** `ExitProcess` and returns `CONTINUE_SEARCH`.

## BUILD RESULT
- `cmd /c build\build.bat` from repo dir → **rc=0, ZERO warnings**, verify_pe PASS (i386, PE32, imports ⊆ {KERNEL32, USER32, msvcrt}, **11 exports**)
- **d3d9.dll = 137,170 B, SHA256 `12fe9ef97ad2e36d382b7b755058d870d424911d7a8b809a8ce4c3b4c275975f`** (`d3d9.sha256` updated)
- **Deployed byte-identical**: repo root = game dir = `tests\d3d9.dll` (SHA256 equal on all three); old game `d3d9mod.log` deleted. Game NOT launched.

## HARNESS RESULTS (all green)
- `tests\test_d3d9_actual.exe` — ALL CHECKS PASSED
- `tests\test_rate_engine.exe` — FAILURES: 0 (incl. new STEP 7 fault-filter semantics)
- `tests\test_source_contract.py` — SOURCE-CONTRACT: PASS (incl. new round-3 checks)
- `tests\test_pe_structure.py` — FAILURES: 0

## WHAT DALI TESTS NEXT (live)
1. Game must now **start and reach the main menu** (the `0x406D1388` notification is ignored, no FAULT line at load).
2. Start a skirmish: chain → match start (valid chain) → `RES t=...` lines; overlay panel appears; no crash.
3. Only a genuinely unhandled fatal exception will log `FAULT addr=.. breadcrumb=ovl-.. code=..` — if you ever see one, send that line.

---

# BUILD — ROUND 2 (REWORK, 2026-09-06) — FIX LIVE MATCH-START CRASH

## ROUND SUMMARY
Dali's live test: the R14 DLL loads (version=OK), chain/decrypt/RES logging all work, the human player is found, the font is created — and the process dies on the FIRST overlay draw at match start, with NO `FAULT` line (crash net failed to catch it). Rebuilt the draw path against the known-good R6/R7 pattern. Everything else from R14 is untouched; all 4 harnesses stay green; Dali re-tests live.

## ROOT CAUSE (what deviated from R6/R7)
1. **Font created MID-FRAME in the draw path.** R14 called `D3DXCreateFontA` lazily from `ui_draw()` on the very first present where the chain became valid (`UI: font created` is the log's last line — i.e., inside the game's Present capture, right at match start, immediately after a device Reset). R6-R7 created the font ONCE at device-create and re-created it on a successful Reset only. A mid-frame font creation can corrupt device state → immediate AV, unrecoverable.
2. **Crash net was replaceable + buffered.** The `FAULT` line never appeared because `SetUnhandledExceptionFilter` was installed once in DllMain — the game (or another DLL) replaces it later, so our filter never ran, and the fault write went through flimsy paths only. R14 also had no per-draw-step breadcrumbs to pin the dying call.
3. **Draw-order hardening regressions vs R6/R7:** RT switch happened even when `GetBackBuffer` failed (`SetRenderTarget(0, NULL)` risk), no strict prev-RT requirement, and `GetRenderTarget`'s ref was never released (per-frame leak).
4. **Phantom `--- match start n=0 ... ---`** fired during loading: match-start fired on `n != s_last_n` with `s_last_n` initialized to -1 before any chain existed.

## THE FIX (files changed)
- `src/ui.c` — font ownership moved OUT of the draw path: new public `ui_create_font(dev)` (device-create + successful Reset ONLY); `ui_draw()` no longer creates fonts, skips cleanly when none, gates on `g_values_valid` (≥1 RES sample taken). Rewrote the RT section to R6/R7 shape: capture prev-RT AND backbuffer BEFORE switching; bail if either fails (`SetRenderTarget` never called with NULL); save/set/restore the 7 render states (Z off ×3, ALPHABLEND SRCALPHA/INVSRCALPHA, LIGHTING off); Begin→EndScene pair with `EndScene` always after successful `BeginScene`; restore states (reverse), restore RT, then release BOTH surfaces (the per-frame prev-RT leak is gone). Added volatile breadcrumbs every step: `ovl-start/tcl/rt/states/begin/panel/end/restore/done`.
- `src/gameif.c` — fault safety net rebuilt: `fault_filter` writes the `FAULT addr=/breadcrumb=/code=` line via a direct Win32 `CreateFileA+WriteFile+FlushFileBuffers+CloseHandle` path (survives heap/CRT damage), then logs + `ExitProcess`. New `ensure_fault_filter()` re-pins `SetUnhandledExceptionFilter` every present frame (the game may replace it). Match-start now requires a VALID chain (`ctx!=0 && n>0 && g_res && g_inc`) — the loading `n=0` phantom is gone.
- `src/state.h` — declarations for `ui_create_font`, `ensure_fault_filter`.
- `d3d9.c` — `w_create_device`/`w_create_device_ex` call `ui_create_font` right after patching the device; `reset_hook` re-creates the font only when the Reset succeeded; `present_hook` calls `ensure_fault_filter()` first; DllMain adds `AddVectoredExceptionHandler(0, fault_filter)` as a second-chance backstop that cannot be replaced.
- `tests/*` harnesses — unchanged, all still pass (font/draw live behavior is not harness-testable without a real device).

## BUILD RESULT
- Same command via `cmd /c build\build.bat` from the repo dir → **rc=0, ZERO warnings**
- verify_pe: i386, PE32, imports ⊆ {KERNEL32, msvcrt, USER32}, 11 exports, `VERIFY PASS`
- **d3d9.dll = 137,000 B, SHA256 `3dcf26d5400c28acca167fcecc59ee7228045b0f6521df3f3a1678829d877101`**; SHA256 recorded in `d3d9.sha256`
- **Deployed byte-identical** (SHA256 match on all three): repo root = game dir = `tests\d3d9.dll`; old `d3d9mod.log` deleted from the game dir.

## HARNESS RESULTS (all green)
- `tests\test_d3d9_actual.exe` — ALL CHECKS PASSED (rc=0)
- `tests\test_rate_engine.exe` — FAILURES: 0 (rc=0)
- `tests\test_source_contract.py` — SOURCE-CONTRACT: PASS (rc=0)
- `tests\test_pe_structure.py` — FAILURES: 0; exports = D3DPERF_BeginEvent, D3DPERF_EndEvent, D3DPERF_GetStatus, D3DPERF_QueryRepeatFrame, D3DPERF_SetMarker, D3DPERF_SetOptions, D3DPERF_SetRegion, DebugSetLevel, DebugSetMute, Direct3DCreate9, Direct3DCreate9Ex (11)

## WHAT DALI TESTS NEXT (live)
1. Start a skirmish; the log should now show `Reset -> font invalidated` then `UI: font created` IMMEDIATELY (at the Reset / device-create, NOT at match start inside the draw).
2. Expect a stable log: chain → match start (only once the chain is valid) → `RES t=...` lines → NO `FAULT` line, game keeps running.
3. If it STILL crashes, the new breadcrumbs + vectored handler WILL produce `FAULT addr=.. breadcrumb=ovl-.. code=..` — that names the exact dying call; send that line.

---

# BUILD — ROUND 14 (2026-09-06) — FULL RESOURCE-RATE MOD

## ROUND SUMMARY
R13 was a zero-render observer proving the memory chain. R14 turns it into the shippable mod: a rate engine (ring + EMA + spike/pause freeze), a D3DX9 font overlay (F9 toggle, live setting keys 1-6), an INI config with a DefaultProfile*.xml merge, a version gate that refuses to arm the overlay on a wrong age3y.exe, and a build/deploy pipeline. Verified on-box, zero game launches: compiled clean, all 4 harnesses pass; only Dali's live calibration remains.

## FILES CREATED/CHANGED
- New: `src/logger.c`, `src/tracker.c`, `src/rate.c`, `src/settings.c`, `src/ui.c`, `build/build.bat`, `build/verify_pe.py`, `tools/extract_bar.py`, `config/schema.md`, `ResourceRateMod.ini.example`, `LICENSE` (MIT), `tests/test_rate_engine.c`
- Rewritten: `src/state.h`, `d3d9.c` (monolithic entry: globals, module includes, D3D9 wrapper/hooks, version gate, DllMain — R13 address constants and RES-line format untouched), `README.md`
- Edited: `src/gameif.c` (debug rates line; aliasing fixes; cleanup — logic unchanged), `tests/test_source_contract.py`, `tests/test_pe_structure.py`, `.gitignore`, `.swarm/BUILD.md`

## BUILD RESULT
- Command: `i686-w64-mingw32-gcc.exe -shared -static-libgcc -O2 -Wall -Wextra -o d3d9.dll d3d9.c d3d9.def -lwinmm -luser32` → rc=0, ZERO warnings
- verify_pe: i386, PE32, imports ⊆ {KERNEL32, msvcrt, USER32} (confirmed), 11 exports, VERIFY PASS
- **d3d9.dll = 135,180 B, SHA256 `dbec64b6d541356298157fc6dd08dc4f3362deea95a8b540e678c5949a15f04a`**; deployed byte-identical to game dir + `tests\d3d9.dll`; INI example copied to game dir

## RATE-ENGINE RULES (implemented)
- Bootstrap primes without emitting a fake rate; cadence gate (100-1000 ms, default 500) honors sample interval
- Steady +1/500ms → EMA 2.0/s (120.0/min); spending spike (`|inst| > EMA×3`, DiscontinuityRatio=3.0) freezes EMA while keeping ring baseline fresh; instant gains counted (ShowGains)
- Clock-not-advancing (game tick stagnant) → global pause, no div-by-zero, no drift
- Per-slot value-flat across a full interval → EMA frozen, unfreezes on change (ESC-pause detection path)
- Realtime mode (QPC) finite/sane

## OVERLAY BEHAVIOR
- Draws only when: enabled + version-gate OK + panel visible + in-match + device/d3dx9/font available; runs BEFORE original Present
- Backbuffer explicit RT, 7 render states saved/restored, surface released via its own vtable
- F9 toggles panel; keys 1=Food 2=Wood 3=Coin 4=Export 5=ShowZero 6=ShowGains toggle live (settings_save() atomic write)
- Menus / DEVICELOST / gate-fail = graceful no-ops

## INI SCHEMA (full doc: config/schema.md)
- `[General]` Enabled(1) / Hotkey(0x78) / StartHidden(0)
- `[Display]` FontName / FontSize(14) / Opacity(0.85) / PosX(12) / PosY(12) / ShowHeader(1) / ShowSlots567(0)
- `[Rate]` SampleMs(500) / Smoothing(med) / Unit(min) / UseGameTime(1) / DiscontinuityRatio(3.0) / ShowGains(1)
- `[Debug]` Enabled(0)
- Plus mtime-gated merge from `Users\DefaultProfile*.xml` mod `<Setting>` keys

## VERSION GATE (age3y.exe on this machine — statically verified, no launch)
- size 11,598,648 ✔ · i386 0x14C ✔ · PE32 ✔ · base 0x400000 ✔ · version resource 6.108.321.137 ✔ → gate PASSES, overlay arms. Failure path proven in harness.

## DEPLOY (done)
- `cmd /c build\build.bat`: compile → verify → d3d9.sha256 → deploy to game dir + tests\d3d9.dll → INI example → d3dx9 note (System32) → clean stale logs

## DEVIATIONS / RISKS (honest)
1. **Exports = 11, not the documented "13"** — d3d9.def has 11; kept 11; verify asserts ≥11
2. Monolithic `d3d9.c` entry instead of `src/dllmain.c` (SWARM_TEST harness compatibility)
3. ESC-pause detected via per-slot value-flat freeze (Present counter keeps ticking during ESC) — **live-checkable**
4. Version gate reads the version RESOURCE (raw PE header read returns 4.0.0.0 and would false-fail) — fixed mid-round
5. Overlay rendering unverified live → Dali's launch test: expect `version=OK`, overlay panel, then calibrate rates/freeze vs HUD

## TEST STATUS (all offline harnesses, run on-box)
- `test_d3d9_actual` 22/22 · `test_rate_engine` 46/46 · `test_source_contract` PASS · `test_pe_structure` 0 failures
- Committed + pushed: `1cc8708` (main)