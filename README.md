# aoe3rate-mod

Observer-only resource logger proxy DLL for **Age of Empires III: The Asian Dynasties** (`age3y.exe`).

It drops in as `d3d9.dll` next to the game executable and logs the **human player's** stock
(food / wood / coin / export) to `d3d9mod.log` in the game folder — one line per frame tick.
It does **not** draw anything, **does not** modify the game, and **does not** render anything.
Pure observer. GameSpy-era-free: no online services involved at all.

## Verified facts (live-tested vs in-game HUD)

Memory chain (Age of Empires III: The Asian Dynasties `age3y.exe`, confirmed 2026-09-06):

```
game    = *(base + 0x866234)
ctx     = *(game + 0x13c)
n       = *(ctx + 0x5c)          // player count
players = *(ctx + 0x58)          // player array
human   = players[1]             // human player = player array index 1
stock   = *(human + 0x230)       // stock container
```

The stock container holds XOR-encrypted resource cells decrypted with keys located at:

```
VA 0xC6DF14              // key table
count 0xC6DF38 = 8       // key count
XOR2 0xC6DF10 = 0xFBCC4F02
```

Slot map:

```
food   = slot 2
coin   = slot 0
wood   = slot 1
export = slot 7
```

Live proof: in-game HUD showing **500 / 121 / 100** produced the log line
`RES t=14630 food=500 wood=121 coin=100`. Log format (per frame tick):

```
RES t=.. food=.. wood=.. coin=.. export=.. player=.. res=..
```

## Build

You need a **32-bit (i686)** MinGW toolchain. Recommended: **w64devkit-x86 v3.1.1** (portable).

Desktop command (exact):

```
C:\Users\Dali\Documents\gaames\w64devkit-x86\w64devkit\bin\i686-w64-mingw32-gcc.exe -shared -static-libgcc -O2 -o d3d9.dll d3d9.c d3d9.def
```

On the Celeron N4020 laptop: same toolchain (portable w64devkit-x86) + a local copy of the
game folder. As long as it is the **same `age3y.exe` version**, the addresses are identical —
no Ghidra session needed to rebuild for the laptop.

## Deploy / run

1. Copy `d3d9.dll` into the AoE3 TAD install folder, next to `age3y.exe`.
   (The original `d3dx9_25.dll` must be present in that folder.)
2. Delete any old `d3d9mod.log` before a test run.
3. Launch **`age3y.exe`** and play. Watch `d3d9mod.log` grow one `RES` line per frame tick.
4. Do **NOT** launch `age3.exe` / `age3x.exe` — the proxy DLL will not load with them.

## Layout

```
d3d9.c                 source (proxy init, hooks, stock decrypt + logging)
d3d9.def               exported-symbol list for the proxy
tests/                 probe/harness sources + build/test scripts
.swarm/                round-by-round build history (BUILD.md, PLAN.md, REVIEW.md, ...)
VERIFIED_ADDRESSES.md  address notes
ghidra_scripts/        Ghidra scripts used during RE (not needed to rebuild)
d3d9.dll               R13 production binary (committed)
```

## Versions

| Tag | State | Artifact |
|-----|-------|----------|
| R13 | **Production — verified vs HUD 2026-09-06** | `d3d9.dll` SHA256 `E6F79C46EF26FD430D3366B06418752B5E3E59870116295DF3BF75DA5D7DF6D0`, 83,272 B |
| R12 | Probe — all-player scan, stock offsets unresolved | source in history |

Full round-by-round detail: `.swarm/BUILD.md`.