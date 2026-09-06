# aoe3rate-mod

Observer-only AoE3 TAD (The Asian Dynasties) resource logger proxy DLL.

**State:** R12 — all-players scan, per-player stock dumps, human player/stock offsets still pending.

## What it does

Drop-in `d3d9.dll` proxy next to `age3y.exe`. Reads the human player's food/wood/coin/export resources every frame and logs them to `d3d9mod.log`. Zero rendering, zero visual corruption — pure observer.

## Build

Requires 32-bit MinGW cross-compiler (w64devkit-x86):

```
i686-w64-mingw32-gcc -shared -static-libgcc -O2 d3d9.c -o d3d9.dll
```

## Deploy

1. Copy `d3d9.dll` + `d3dx9_25.dll` to the game dir next to `age3y.exe`
2. Delete any old `d3d9mod.log`
3. Launch **age3y.exe** (NOT age3.exe or age3x.exe)
4. Play a skirmish for 30-60s, then check `d3d9mod.log`

## State

- R12: scans all players P0..Pn-1, logs per-player res/raw + 5s cell-window dumps + plain-int stock hunt
- Fallback player#2 = AI (starts at 0 resources); human player index + stock offsets unresolved
- HUD calibration anchors: food=600/wood=100/coin=230, food=600/wood=200/coin=30, food=600/wood=100/coin=50

## History

See `.swarm/BUILD.md` for full round-by-round build log. Swarm state history in `.swarm/`.
