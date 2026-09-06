# aoe3rate-mod

In-game resource-rate overlay for **Age of Empires III: The Asian Dynasties** (`age3y.exe`, TAD 1.0.8).

Drops in as `d3d9.dll` next to the game executable. Reads the human player's **decrypted stock**
(food / wood / coin / export — the verified memory chain), computes **real-time gather/spend rates**
from per-slot ring sampling with EMA smoothing, and draws a small native-looking panel in-game.
Settings live in `ResourceRateMod.ini` next to the DLL; F9 toggles the panel, and the overlay's
settings hints let you flip toggles live (written back to the INI immediately).

## Verified facts (live-tested vs in-game HUD)

Memory chain (confirmed 2026-09-06):

```
game    = *(base + 0x866234)
ctx     = *(game + 0x13c)
n       = *(ctx + 0x5c)          // player count
players = *(ctx + 0x58)          // player array
human   = players[1]             // human player = player array index 1
                                  //  (fallback: scan all players, largest food)
stock   = *(human + 0x230)       // XOR-encrypted stock container
income  = *(human + 0x80)        // game's own rate bookkeeping (read-only)
```

Decrypt: `value[slot] = float32( key[slot] ^ *(stock + slot*4) )`.

```
key table  VA 0xC6DF14    // 8 dwords
count      VA 0xC6DF38 = 8
XOR2       VA 0xC6DF10 = 0xFBCC4F02   (anti-cheat checksum seed, unused)
```

Slot map:

```
food   = slot 2
coin   = slot 0
wood   = slot 1
export = slot 7
slots 3..6 uncovered  // shown if nonzero, generic labels
```

Log format (d3d9mod.log):

```
RES t=.. food=.. wood=.. coin=.. export=.. player=.. res=..
```

## Rate engine (how the numbers are computed)

- Ring samples per slot at `SampleMs` cadence (default 500 ms, range 100-1000).
- Rate = `(value[cur] - value[prev]) / elapsed`, elapsed in game time
  (1 tick = 1 ms — the game's own convention, `RVA_TIME_STEP = 0.001f`).
- **EMA smoothing**: `low` 0.1 / `med` 0.2 / `high` 0.4.
- Unit: `+x.x/s` or `+x.x/min` (×60).
- **Spending spikes**: when the instant rate is negative and magnitude > EMA ×
  `DiscontinuityRatio` (default 3.0), the sample is skipped and the rate freezes
  until income resumes. **Positive jumps (instant gains) are counted**.
- **Pause**: if the game tick does not advance (ESC / menu / loading), rates freeze —
  no division by zero, no drift.
- **Menus**: `n == 0` (main menu) → overlay hidden entirely.

## Settings (`ResourceRateMod.ini`)

| Section | Key | Default | Meaning |
|---|---|---|---|
| `[General]` | `Enabled` | 1 | master switch |
| | `Hotkey` | 0x78 (F9) | virtual-key code |
| | `StartHidden` | 0 | start with panel hidden |
| `[Display]` | `FontName` | *(empty ⇢ Georgia)* | font face |
| | `FontSize` | 14 | row height / text size |
| | `Opacity` | 0.85 | panel + text alpha |
| | `PosX` / `PosY` | 12 / 12 | panel position |
| | `ShowHeader` | 1 | "RESOURCES" header |
| | `ShowSlots567` | 0 | show unknown slots 3..6 (nonzero only) |
| `[Rate]` | `SampleMs` | 500 | ring interval (100-1000) |
| | `Smoothing` | med | EMA alpha low/med/high |
| | `Unit` | min | per-minute display |
| | `UseGameTime` | 1 | 1 = game tick, 0 = QPC realtime |
| | `DiscontinuityRatio` | 3.0 | spend-spike skip threshold |
| | `ShowGains` | 1 | count positive jumps |
| `[Debug]` | `Enabled` | 0 | extended log |

The file is parsed robustly (missing / partial / garbage → defaults), rewritten
atomically (temp + replace), and the active `Users\DefaultProfile*.xml` is polled
(mtime + parse) for any `ResourceRateMod.*` `<Setting>` keys that override the INI.

### F9 overlay + live toggles

While the panel is shown:

| Key | Effect (writes INI live) |
|---|---|
| `F9` | toggle the panel |
| `1` | ShowHeader on/off |
| `2` | ShowSlots567 on/off |
| `3` | ShowGains on/off |
| `4` | Unit sec/min |
| `5` | SampleMs 100 → 250 → 500 → 1000 |
| `6` | Smoothing low → med → high |

## Version gate + fail-safe

The proxy verifies `age3y.exe`: size 11,598,648 B, PE `6.0108.0321.0137`, image
base `0x400000`, i386. On mismatch the overlay is disabled with a one-line log
reason and the proxy stays a pure passthrough — the game is never touched.

## Build

You need a **32-bit (i686)** MinGW toolchain. Recommended: **w64devkit-x86** (portable).

```
build\build.bat
```

or by hand (with the w64devkit `bin` + `libexec\gcc\i686-w64-mingw32\16.2.0` on PATH):

```
i686-w64-mingw32-gcc.exe -shared -static-libgcc -O2 -o d3d9.dll d3d9.c d3d9.def -lwinmm -luser32
```

`build.bat` compiles, verifies `pei-i386`, the import set ⊆ {KERNEL32, msvcrt, USER32},
exports, writes the SHA256, deploys to the game dir + `tests\`, copies the INI
example, ships `d3dx9_25.dll` if absent, and deletes the old `d3d9mod.log`.

## Deploy / run

1. Copy `d3d9.dll` (+ `ResourceRateMod.ini`) into the AoE3 TAD install folder, next to `age3y.exe`.
   `d3dx9_25.dll` must be present there (DX9 redist; the proxy loads it at runtime).
2. Launch **`age3y.exe`** and play. F9 toggles the panel.
3. Do **NOT** launch `age3.exe` / `age3x.exe` — the proxy targets `age3y.exe`.

## Layout

```
d3d9.c                 build entry (DllMain, D3D9 wrapper + hooks, version gate)
src/state.h            shared declarations
src/logger.c           d3d9mod.log + debug logging
src/tracker.c          ring sampling, EMA, spend-spike skip, pause freeze
src/rate.c             /s vs /min display rates
src/gameif.c           memory chain, decrypt, observer, rate_for
src/settings.c         INI parse/write + DefaultProfile XML poll
src/ui.c               D3DX9 overlay (font, panel, F9, live toggles)
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

## Versions

| Tag | State | Artifact |
|-----|-------|----------|
| R14 | **Current — full rate mod + overlay** | `d3d9.dll` (see `.swarm/BUILD.md` for size + SHA256) |
| R13 | Observer-only proxy — verified vs HUD 2026-09-06 | 83,272 B `E6F79C46EF26FD430D3366B06418752B5E3E59870116295DF3BF75DA5D7DF6D0` |

Full round-by-round detail: `.swarm/BUILD.md`.