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