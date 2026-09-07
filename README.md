# aoe3rate-mod

In-game resource-rate overlay for **Age of Empires III: The Asian Dynasties** (`age3y.exe`, TAD 1.0.8).

Drops in as `d3d9.dll` next to the game executable. Reads the human player's **decrypted stock**
(food / wood / coin / export — the verified memory chain), computes **real-time gather/spend rates**
from per-slot ring sampling with EMA smoothing, and draws a small native-looking panel in-game.
Settings live in `ResourceRateMod.ini` next to the DLL; F9 toggles the panel, and the overlay's
two settings-hint lines let you flip toggles live (written back to the INI immediately).

## Definitive Edition is NOT supported

**Age of Empires III: Definitive Edition is NOT supported and will never load this overlay.**
The proxy targets the legacy `age3y.exe` (The Asian Dynasties 1.0.8) only; any other executable is
refused by the version gate and left completely untouched.

## Installation

1. Copy `d3d9.dll` (+ `ResourceRateMod.ini`) into the AoE3 TAD install folder, next to `age3y.exe`.
   `d3dx9_25.dll` must be present there (DX9 redist; the proxy loads it at runtime).
2. Launch **`age3y.exe`** and play. F9 toggles the panel, F9+Alt flips format/visibility toggles.
3. Do **NOT** launch `age3.exe` / `age3x.exe` — the proxy targets `age3y.exe` only (see below).

## Supported Versions

| Build | Executable | Verified |
|---|---|---|
| **TAD 1.0.8** (The Asian Dynasties) | `age3y.exe` — 11,598,648 B, PE version 6.0108.0321.0137, image base `0x400000`, i386 | **Yes** — this is the release target |
| Definitive Edition (`AoE3DE.exe` / any Steam/MS Store binary) | — | **Never** — refused by the version gate and left untouched |
| Vanilla 1.0 (`age3.exe`), WarChiefs (`age3x.exe`), other TAD builds | — | **Not yet verified** — placeholder rows in the gate table; the gate refuses them with a logged reason |

The gate table (`g_versions[]` in `d3d9.c`) is data-driven: each verified build is one row pinned
by exe size + PE identity, and the gate runs the first *populated* row. Adding a supported build is
a one-row edit (size + version + base) once it has been measured off the real exe.

## Features

- Real-time food / wood / coin / export rates (EMA-smoothed ring samples, 100–1000 ms cadence).
- Native-looking panel: resource rows highlight the current value + rate with color-matched text.
- Per-resource visibility (Food/Wood/Coin/Export), decimal places, plus-sign, zero-rate hiding.
- Panel position: `PosX/PosY`, or pinned `TopLeft` / `TopRight` corner modes.
- F9 overlay with live toggles (both a plain layer and an F9+Alt layer) — written back to the INI.
- Spending-spike freeze + pause freeze; global freeze on menu (`n == 0`).
- **ARMED/DISABLED diagnostic line** in `d3d9mod.log` at load, plus a visible red GDI line when the
  overlay is disabled — "invisible failure" is impossible.

## Rate engine

- Ring samples per slot at `SampleMs` cadence (default 500 ms, range 100–1000).
- Rate = `(value[cur] - value[prev]) / elapsed`.
- **EMA smoothing**: `low` 0.1 / `med` 0.2 / `high` 0.4.
- Unit: `+x.x/min` or `+x.x/s` (×60 when per-minute).
- **Spending spikes**: when the instant rate is negative and magnitude > EMA × `DiscontinuityRatio`
  (default 3.0), the sample is skipped and the rate freezes until income resumes. **Positive jumps
  (instant gains) are counted** (ShowGains can disable that).
- **Pause**: when the sample source does not advance (ESC / menu / loading), rates freeze — no
  division by zero, no drift.
- **Menus**: `n == 0` (main menu) → overlay hidden entirely.

## Configuration

All keys below live in `ResourceRateMod.ini` next to the DLL (see `ResourceRateMod.ini.example` and
`config/schema.md`). Missing / partial / garbage values fall back to the defaults. The file is
rewritten atomically (temp + replace) and the active `Users\DefaultProfile*.xml` is polled
(mtime + parse) for any `ResourceRateMod.*` `<Setting>` keys that override the INI.

| Section | Key | Default | Meaning |
|---|---|---|---|
| `[General]` | `Enabled` | 1 | master switch (0 → pure passthrough proxy) |
| | `Hotkey` | 0x78 (F9) | virtual-key code |
| | `StartHidden` | 0 | start with panel hidden |
| `[Display]` | `FontName` | *(empty ⇢ Georgia)* | font face |
| | `FontSize` | 14 | row height / text size |
| | `Opacity` | 0.85 | panel + text alpha |
| | `PosX` / `PosY` | 12 / 12 | panel position (only in `Default` mode) |
| | `ShowHeader` | 1 | "RESOURCES" header |
| | `ShowSlots567` | 0 | show unknown slots 3..6 (nonzero only) |
| | `DecimalPlaces` | 1 | rate decimals 0..3 (`+2` / `+2.4` / `+2.37` / `+2.375`) |
| | `ShowPlusSign` | 1 | `+` prefix on positive rates |
| | `ShowResourceNames` | 1 | resource labels (Food/Wood/Coin/Export) |
| | `ShowZeroRates` | 1 | hide rows whose rate is ~0 |
| | `ShowFood` | 1 | Food row visible |
| | `ShowWood` | 1 | Wood row visible |
| | `ShowCoin` | 1 | Coin row visible |
| | `ShowExport` | 1 | Export row visible |
| | `PositionMode` | `Default` | corner anchor: `Default` (PosX/PosY), `TopLeft`, `TopRight` |
| `[Rate]` | `SampleMs` | 500 | ring interval (100–1000) |
| | `Smoothing` | med | EMA alpha low/med/high |
| | `Unit` | min | per-minute vs per-second display |
| | `UseGameTime` | 1 | **remnant switch** — both modes wall-clock QPC (see Known Limitations) |
| | `DiscontinuityRatio` | 3.0 | spend-spike skip threshold |
| | `ShowGains` | 1 | count positive jumps |
| `[Debug]` | `Enabled` | 0 | extended diagnostics into `d3d9mod.log` (incl. the one-shot `ovl diag` line) |

### Hotkeys

While the overlay panel is shown and the **game window is foreground** (typing in chat or being
alt-tabbed never flips anything):

**F9 held** (plain layer — settings hints line 1):

| Key | Effect (writes INI live) |
|---|---|
| `F9` | toggle the panel (works even when hidden) |
| `1` | ShowHeader on/off |
| `2` | ShowSlots567 on/off |
| `3` | ShowGains on/off |
| `4` | Unit sec/min |
| `5` | SampleMs 100 → 250 → 500 → 1000 |
| `6` | Smoothing low → med → high |

**F9 + Alt held** (format/visibility layer — settings hints line 2):

| Key | Effect (writes INI live) |
|---|---|
| `Alt+1` | DecimalPlaces 0 → 1 → 2 → 3 → 0 |
| `Alt+2` | ShowPlusSign on/off |
| `Alt+3` | ShowResourceNames on/off |
| `Alt+4` | ShowZeroRates on/off |
| `Alt+5` | ShowFood on/off |
| `Alt+6` | ShowWood on/off |
| `Alt+7` | ShowCoin on/off |
| `Alt+8` | ShowExport on/off |
| `Alt+9` | PositionMode Default → TopLeft → TopRight → Default |

Two settings-hint lines at the bottom of the panel keep the complete key reference on screen; they
clip at a fixed 78-character cap.

## Troubleshooting

If the overlay does not appear, read `d3d9mod.log` and walk the expected life line. The reference
healthy startup is (these are the literal log strings; the `→` just chains them):

**P0 checklist (the exact run to reproduce a clean state):**

```
OVERLAY ARMED → UI ready: d3dx9_25.dll loaded → CreateDevice … patched=yes → chain … [human-p1] → UI: font created size=14 face=Georgia → ovl first draw ok frame=… → ovl heartbeat n=…
```

- `OVERLAY ARMED` — version gate passed, the overlay is armed. With `Debug=1` the very first draw
  also emits `ovl diag rt=… rect=… alpha=…`, printing the current render-target address plus the
  exact panel rectangle/alpha that frame (one-shot, debug-gated):
  `ovl diag rt=%p rect=%ld,%ld:%ldx%ld alpha=0x%lX`.
- In `Default` mode the `rect=` comes straight from `PosX/PosY`/width/height; in `TopRight` mode it
  pins to the right edge using the backbuffer size the DLL captured at CreateDevice/Reset.

**A/B isolation runs** (each produces a *visible* red line at the top-left if the overlay is off):

- **(a) rename `age3y.exe`** → the gate fails on size: log starts
  `OVERLAY DISABLED: exe size=… expected=…`, **plus** the red GDI line in-game.
  Proves the gate + fallback path end-to-end.
- **(b) move `d3dx9_25.dll` away** → `OVERLAY DISABLED: d3dx9_25.dll not loadable`, **plus** the
  red GDI line. Isolates the D3DX9 dependency from the hook.
  Restore the DLL after the run.

### It doesn't load at all
Check the executable is `age3y.exe` (TAD 1.0.8). The proxy refuses everything else by design.

### It crashes
The overlay is fail-safe by construction: read-only memory reads, no writes, no game-state
mutations, fault net + version gate + re-entrancy tripwire. Any fault is dumped to `d3d9mod.log`
with the exact dying call (breadcrumb). No overlay change ships without all 4 harnesses green.

### No rates are shown
`chain` line must end in `[human-p1]` (n=3 match-start) — in menu/loading (`n==0`) the overlay is
intentionally hidden. `ovl heartbeat n=…` repeating means the draw path is completing.

### Wrong version / overlay refuses to arm
`OVERLAY DISABLED: <reason>` names it exactly (`exe size=…`, `base=…`, `pe ver=…`).

### Panel invisible while the game runs fine
F9 toggles it; check `ShowHeader/ShowSlots567` and per-resource toggles — with e.g. `ShowFood=0`
that row is skipped *and* its row-height, and every format toggle has a visible effect via the two
hint lines.

## Multiplayer Safety

Client-side and read-only. The proxy **reads** a few verified game-memory pointers (stock, income),
computes rates locally, and draws with the D3D9/D3DX9 APIs. It **never writes game memory, sends
anything over the network, or alters game behavior**. The only files it touches are
`ResourceRateMod.ini`, the active `Users\DefaultProfile*.xml` (read), and `d3d9mod.log`.

## Uninstallation

Delete `d3d9.dll` and `ResourceRateMod.ini` from the TAD folder, and remove any
`ResourceRateMod.*` `<Setting>` entries from the active `Users\DefaultProfile*.xml`. `d3d9mod.log`
(if present) can be deleted too. Nothing is written outside the TAD folder.

## Known Limitations

1. **Corner-anchored HUD — not a native resource bar.** The panel is drawn with the verified
   D3D9/D3DX9 draw path at a screen position derived from settings; replacing the game's own
   resource-bar UI would require reverse-engineering the render/HUD pipeline that is out of the
   proven slot set — deliberately not attempted.
2. **F9 / F9+Alt hotkeys are overlay-side, not the game's Options tab.** They work only while the
   game window is foreground and (for toggles) the panel is shown.
3. **`UseGameTime` is a remnant switch.** Both modes today use wall-clock QPC — `s_tick` is a
   per-`Present` frame counter, not milliseconds of sim time. True sim-time requires a verified
   tick read; `RVA_TIME_STEP` in `rate_for` is only exercised by the SWARM_TEST harness
   (`FUN_0086da39` is not live-called). Open work, stated honestly.
4. **No fabricated coordinates/offsets.** The version gate pins `age3y.exe` 1.0.8 by size, PE
   version and image base; every memory address used is a verified constant for that build. The
   panel rectangle comes from settings, never from invented game data.

## Build / layout / versions

You need a **32-bit (i686)** MinGW toolchain. Recommended: **w64devkit-x86** (portable).

```
build\build.bat
```

or by hand (with the w64devkit `bin` + `libexec\gcc\i686-w64-mingw32\16.2.0` on PATH):

```
i686-w64-mingw32-gcc.exe -shared -static-libgcc -O2 -o d3d9.dll d3d9.c d3d9.def -lwinmm -luser32
```

`build.bat` compiles, verifies `pei-i386`, the import set ⊆ {KERNEL32, msvcrt, USER32}, exports,
writes the SHA256, deploys to the game dir + `tests\`, copies the INI example, ships
`d3dx9_25.dll` if absent, and deletes the old `d3d9mod.log`.

```
d3d9.c                 build entry (DllMain, D3D9 wrapper + hooks, version table)
src/state.h            shared declarations
src/logger.c           d3d9mod.log + debug logging
src/tracker.c          ring sampling, EMA, spend-spike skip, pause freeze
src/rate.c             /s vs /min display rates (rate_display / rate_unit_label)
src/gameif.c           memory chain, decrypt, observer, rate_for
src/settings.c         INI parse/write + DefaultProfile XML poll (incl. format/position keys)
src/ui.c               D3DX9 overlay (font, panel geometry, format_rate_line, F9/F9+Alt)
d3d9.def               exported-symbol list
build/build.bat        build + verify + deploy
build/verify_pe.py     PE checks (machine, imports, exports, SHA256)
tools/extract_bar.py   ESPN-v2 .bar list/extract (RE / experimental XML path)
config/schema.md       INI schema doc
ResourceRateMod.ini.example
tests/                 harness sources (SWARM_TEST builds use d3d9.dll)
.swarm/                round-by-round build history
VERIFIED_ADDRESSES.md  address notes
```

| Tag | State | Artifact |
|-----|-------|----------|
| R11 (this round) | **Current — settings gap (decimal/plus/visibility/position) + two-line hints + version table + P0 `ovl diag`** | `d3d9.dll` (see `.swarm/BUILD.md` for size + SHA256) |
| R14 | Overlay + rate engine + observer (previous) | `d3d9.dll` |
| R13 | Observer-only proxy — verified vs HUD 2026-09-06 | 83,272 B `E6F79C46EF26FD430D3366B06418752B5E3E59870116295DF3BF75DA5D7DF6D0` |

Full round-by-round detail: `.swarm/BUILD.md`.