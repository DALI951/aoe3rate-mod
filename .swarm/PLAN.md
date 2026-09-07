# PLAN — AoE3 TAD Resource-Rate Mod (R14+)

## 1. SUMMARY

Upgrade the R13 observer-only proxy (`d3d9.dll`) into the full resource-rate mod: a real rate engine computed from the verified decrypted stock slots, a native-looking in-game overlay, and reliable settings. **Key empirical finding from this investigation changes the settings architecture:** the classic engine's UI gadget events are dispatched by compiled C++ handlers (gadslider.cpp, gadcore.cpp in the exe string table) — a proxy DLL cannot receive XML click events. UI XML also lives **inside `.bar` archives** (ESPN v2), not as loose files. Therefore the **F9 DLL-drawn overlay panel + INI is the primary settings surface**; the "native options tab via XML override" is an **experimental, gate-verified stretch** (approach b from the task, clearly documented, droppable).

**Deliveries:** `d3d9.dll` (single artifact) + `ResourceRateMod.ini` (+ optional experimental loose `data\ui\options.xml`). Two-file delivery stays intact.

**ROUND 19-PIVOT (2026-09-07):** primary surface changed from the in-game panel to an **external
always-on-top viewer** — the DLL exports live decrypted stock as `food:X ,wood:X ,coin:X` to
`d3d9mod.log` every ~500ms, read by `tools/rates_widget.py` (stdlib Tkinter). The overlay path stays
intact/zero-touch but is no longer relied upon for in-match visibility. House rules held: reuse ONLY
the verified chain + decrypt, NO new imports (CreateThread via KERNEL32; imports
{KERNEL32,USER32,msvcrt}, 11 exports), additive, all harnesses green. Full detail: `.swarm/BUILD.md`
ROUND 19-PIVOT. Delivery = `d3d9.dll` + `ResourceRateMod.ini` + `tools/rates_widget.py`.

## 2. CURRENT-STATE AUDIT (verified this session)

**Repo** `C:\Users\dali\aoe3rate-mod` @ `07f5b4f` R13, clean. `d3d9.c` = 616 lines, observer-only, logs `RES t=... food/wood/coin/export player= res=`. Exports 11, imports KERNEL32/msvcrt only, 83,272 B, SHA256 `E6F79C46...DF6D0`.

**Game install** (MOVED from `Documents\gaames\`): `C:\Users\dali\Documents\Age of Empires III - Complete Collection\`. `age3y.exe` = 11,598,648 B, PE ver `6.0108.0321.0137` (TAD 1.0.8), image base `0x400000`, i386. Bars: `DataP/DataPX/DataPY.bar` + `data\Data/Data2/Data3.bar`, `FONTS\Fonts.bar`. Loose `data\*.xml` + `*.xml.XMB` pairs exist (stringtable etc.) — engine loads loose virtual files (standard). `ui\eso\game3.ddt` only loose UI file.

**UI XML system:** all UI XML lives **inside `.bar` archives** (magic `ESPN`, ver 2, `11 22 33 44`, UTF-16 name table). `Data.bar` holds `options.xml.xmb`, `uioptions.xml.xmb`, `uioptionsdlg.xml.xmb`, `uilayout.xml.xmb`, `uimain.xml.xmb`, ~300 `ui*.xml.xmb` screens, plus `uilayout.dtd`. `Data3.bar` holds UI content (no options). `DataPY.bar` holds **no** UI strings. So the TAD options screen ships from `Data.bar`. The engine expects compiled `.xmb` (binary XML) — editing requires recompile (engine regens XMB from XML if XML newer; this is the documented AoE3 modding workflow).

**Settings persistence:** `Users\DefaultProfile3.xml` etc. contain `<GameSettings Name="GameOptions" Def="cDefGameOptions">` / `<Settings Version="53">` / `<Setting Name="...">value</Setting>`. Settings are a generic name→value map — the DLL can poll the active profile for mod keys, but **driving those keys from XML clicks is NOT possible** (handlers compiled in exe).

**Exe evidence of compiled UI dispatch:** strings `OptionsScreen`, `GameUI`, `HotKeySetupScreen`, `.\gadslider.cpp`, `.\gadcore.cpp`, `.\gadlist.cpp`, `.\gadtextsimple.cpp`, `.\screen.cpp`, `.\gameui.cpp`, `Executing console file %s`, `developer.con`/`gamey.con`.

**Verified memory chain (reuse, do NOT re-derive):**
`game=*(base+0x866234)` → `ctx=*(game+0x13c)` → `n=*(ctx+0x5c)` → `players=*(ctx+0x58)` → `human=players[1]` (n≥2; else largest-food scan) → `stock=*(human+0x230)`. Decrypt `value[slot]=float(key_dword[slot]^res_dword[slot])`, key table VA `0xC6DF14` (8 keys), count `0xC6DF38`, XOR2 `0xC6DF10=0xFBCC4F02`. **Slots: food=2, wood=1, coin=0, export=7; slots 3–6 unknown.** Income object `[human+0x80]`: `+0x160/+0x164` signed ints = game's own rate bookkeeping (do NOT trust for rate; we compute our own from deltas). Tick source: game clock read at `RVA_TIME_STEP 0x0079A5C8` region; `t` already available in Present. Menu/loading state = `n==0`.

**Decrypted assets for RE:** `C:\Users\dali\Documents\age of empires 3 decompiled\age3y_full_disasm.txt` (75 MB) + `resources-analysis.txt`.

**Toolchain (MOVED):** `C:\Users\dali\AppData\Local\Temp\opencode\w64devkit-x86\w64devkit\bin\i686-w64-mingw32-gcc.exe` (i686 GCC 16.2.0; re-extractor `w64devkit-x86-2.9.1.7z.exe` in same temp dir — re-extract if temp is cleaned). Python: `C:\Users\dali\AppData\Local\Programs\Python\Python312\python.exe`. All old `Documents\gaames\...` and `C:\Python314\...` references in README/BUILD.md/TEST.md/test_source_contract.py/test_pe_structure.py are **stale — update them**.

**Build cmd (proven):** prepend `bin` + `libexec\gcc\i686-w64-mingw32\16.2.0` to PATH (cc1), then:
`i686-w64-mingw32-gcc.exe -shared -static-libgcc -O2 -o d3d9.dll d3d9.c d3d9.def -lwinmm`

**Imports will change from 2 → 3 DLLs:** add USER32 (`GetAsyncKeyState` for F9). Update all "imports = KERNEL32/msvcrt only" assertions.

## 3. FILE-BY-FILE BUILD PLAN

New modular layout (keeps single artifact `d3d9.dll`):

- `src/dllmain.c` — DllMain, `Direct3DCreate9`/`Ex` wrappers, device + chain init, Present hook, Reset hook, version-gate, module path detection (existing pattern at d3d9.c:73).
- `src/hooks.c/.h` — vtable hook manager: install, resurrect, recovery on lost device.
- `src/gameif.c/.h` — game-context walk, human selection, slot decrypt, idle/menu state (`n==0`), signature/version check.
- `src/tracker.c/.h` — per-slot ring of `(tick, value)` samples; delta; spend-spike skip; EMA; 60s window, depth 1024, zero alloc/frame.
- `src/rate.c/.h` — rates from tracker; sec/min ×60; game-time vs real-time.
- `src/ui.c/.h` — overlay: font mgmt, panel layout, line drawing, F9 toggle, menu hide.
- `src/settings.c/.h` — INI parse/write with defaults + atomic write; `ResourceRateMod.ini` next to DLL.
- `src/logger.c/.h` — existing chain-log moved verbatim.
- `build/build.bat` — PATH prepend, gcc, hash, Copy-Item deploy to game dir + `tests\d3d9.dll`, delete old `d3d9mod.log`.
- `tools/extract_bar.py` — ESPN-v2 bar listing/extract (for the experimental XML path + future RE).
- `config/` — INI schema doc.
- `ResourceRateMod.ini.example`, updated `README.md`, `LICENSE` (add — public domain/MIT).
- Keep `d3d9.def` 13 exports unchanged.

## 4. DRAW LAYER PLAN (resurrect R3–R7 architecture)

- Load **`d3dx9_25.dll`** at runtime (`LoadLibraryA`), `D3DXCreateFontA` for a serif/sans font (game style: ivory/gold on translucent dark, red accent only for hotkey/alert — per taste: no clutter). **Ship `d3dx9_25.dll` beside `d3d9.dll`** (deploy step); if it fails to load → overlay disabled gracefully, proxy keeps working.
- ID3DXFont vtable: `12 Begin, 13 DrawTextA, 14 DrawTextW, 15 End, 16 OnLostDevice, 17 OnResetDevice`.
- Draw **before** original `Present`. Explicit RT via `GetBackBuffer`+`SetRenderTarget`; save/restore render states (Z off, stencil off, ALPHABLEND `SRCALPHA`). `TestCooperativeLevel` guard. Hook `Reset` (slot 16) → invalidate font, recreate next frame. Keep `VirtualQuery` guards + `SetUnhandledExceptionFilter` (MinGW SEH breakage).
- Panel: header "RESOURCES"/rate title, rows `Food / Wood / Coin / Export` + `+x.x/s` (or `/min`), optional unknown slots 3–6 shown-if-nonzero labelled `Slot5` etc.

## 5. RATE ENGINE SPEC

- Per slot ring of samples at configurable `sample_ms` (default 500, range 100–1000). Rate = `(value[cur]−value[prev])/elapsed`; integrate over window; report smoothed.
- **EMA smoothing:** Low/Med/High → alpha ≈ 0.1/0.2/0.4 (configurable). Unit sec → min ×60.
- **Gametime vs realtime:** default game-time (tick delta, immune to FPS jitter); realtime fallback via `QueryPerformanceCounter`; a tick that does not advance = pause → freeze rates (do not divide by zero, do not show drift).
- **Spending/spike detection:** large negative delta vs current EMA by `[Rate] DiscontinuityRatio` (default ~3.0) → skip sample (freeze), then resume; **positive jumps (instant gains) counted** (documented behavior).
- Slots 3–6: auto-detect at runtime; display only when nonzero, labelled generically; configurable to hide.
- Zero per-frame allocations; all state static.

## 6. SETTINGS / INI SPEC

`ResourceRateMod.ini` next to DLL, UTF-8, robust parser ignoring unknown keys, defaults applied, atomic rewrite (write temp + replace). Sections:
- `[General]` `Enabled=1`, `Hotkey=0x78` (F9), `StartHidden=0`.
- `[Display]` `FontName=`, `FontSize=14`, `Opacity=0.85`, `PosX=12`, `PosY=12`, `ShowHeader=1`, `ShowSlots567=0`.
- `[Rate]` `SampleMs=500`, `Smoothing=med`, `Unit=min`, `UseGameTime=1`, `DiscontinuityRatio=3.0`, `ShowGains=1`.
- `[Debug]` `Enabled=0` → extended `d3d9mod.log`.

**Settings architecture decision (the honest mechanism):**
1. **Primary — F9 overlay panel (DLL-ownable, reliable).** Drawn in game style; toggles; edits write INI live. This is the real settings surface, matching the task's allowed fallback.
2. **Experimental — native options tab (approach b).** Requires unpacking `options.xml` from `Data.bar`, adding a "Resource Rate Mod" section bound to `<Setting>` keys, shipping loose at the matching virtual path, deleting stale `.XMB`. **Gate:** Dali live-tests one change (see §8). If the engine rejects foreign controls/keys → **drop XML, ship F9 + INI only**, document why. DLL also polls the active `Users\DefaultProfile*.xml` for any mod `<Setting>` keys it can find (cheap mtime+parse), merging over INI defaults.
3. DLL always writes its own INI regardless of which surface changed values.

## 7. VERSION DETECTION + FAIL-SAFE

- Locate module dir via `GetModuleFileNameA` (already used for log path). Target exe must be `age3y.exe` (or fall back to `age3.exe`/`age3x.exe` at same ABI).
- Signature gate: file size = 11,598,648 B, PE ver `6.0108.0321.0137`, image base `0x400000`, i386, + verify game-context chain bytes on first tick.
- On mismatch: overlay disabled, one-line log reason, proxy passthrough only — never crash the game. All derefs behind `VirtualQuery` guards (existing nullsafe pattern).

## 8. TEST PLAN

**Offline harness (auto, in `tests/`, reuse `test_d3d9_actual.c` fixture: SWARM_TEST include, `KEY_BYTES`, `set_encrypted_slot`, `putu`/`putf`):**
- Steady production → EMA converges to delta; unit sec→min correct.
- Spending spike (large negative) → skipped, no discontinuity jump; resume after.
- Instant gain (positive jump) → counted per `ShowGains`.
- Pause (tick repeats) → rate frozen, no div-by-zero.
- Game-time vs realtime divergence test.
- `n==0` menu state → overlay hidden, no crash.
- Lost-device/Reset → hook+font recovery (nullsafe).
- PE checks: i386, 13 exports, imports ⊆ {KERNEL32, msvcrt, USER32}; SHA256 recorded.
- Version gate: correct sig passes; tampered size/ver → disabled + log reason.
- INI: missing/partial/garbage file falls back to defaults; round-trip write.
- Fix stale paths in `test_pe_structure.py` / `test_source_contract.py` / `TEST.md` (new game path, new toolchain, Python312).
- Runner: `test_rate_engine.py` driving the C harness + `test_rate_engine.c`; all via Python312.

**Dali live checklist (manual):** deploy → delete old log → launch; verify panel shows values matching HUD (food/wood/coin); F9 toggles; watch during a spending burst (rates freeze then resume); ESC/menu hides panel; alt-tab/lost device no crash; quit clean; optionally test the experimental options-tab XML gate: if the tab renders AND a changed `<Setting>` value appears in `DefaultProfile3.xml` → keep approach b; else drop it.

## 9. BUILD/DEPLOY

1. `build\build.bat` (PATH prepend w64devkit bin + `libexec\gcc\i686-w64-mingw32\16.2.0`; gcc command from §2; capture rc/warnings).
2. Verify `pei-i386`, exports, import set, SHA256.
3. `Copy-Item d3d9.dll` → game dir supersedes existing; copy `ResourceRateMod.ini.example` → `ResourceRateMod.ini`; copy `d3dx9_25.dll` (from DX Redist 9.0c) → game dir if absent; delete old `d3d9mod.log`.
4. **Dali launches the game — NEVER launch the game from swarm tasks.**

## 10. RISKS + HONEST LIMITS

- **XML clicks cannot reach the DLL** (compiled handlers) → F9+INI is the real settings surface; native options tab is an experiment and may ship disabled/dropped. Do not promise "settings in the in-game menu" as guaranteed.
- UI XML is bar-packed XMB; loose-override + XMB recompile is unverified → gate it live.
- `d3dx9_25.dll` may be missing → ship it or graceful disable.
- Unknown slots 3–6: shown-if-nonzero with honest labels, never asserted as specific resources.
- New import (USER32) + draw code violate R13's "clean binary" guarantee — tests must be updated accordingly, and the min-size increase documented.
- Tick/time source behavior during pause/ESC is engine-dependent → freeze logic + live check.
- Tunables (DiscontinuityRatio, alphas) are heuristic defaults; calibrate from real logs.
