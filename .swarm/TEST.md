# TEST.md — AoE3 TAD resource-rate HUD mod — tester report, ROUND 2 (re-test after rework)

Date: 2026-09-05. Tester: swarm `tester`.
Round 2 re-verification of the data-layer rework described in `.swarm/BUILD.md`
(round 2), driven by `.swarm/REVIEW.md` (round 1) MUST-FIX 1 + MUST-FIX 2
(wrong container `*(0xDCB808)` scratch, wrong rate field types) and SHOULD-FIX 3
(loadsmoke_path loaded system32).
No game launch anywhere in this report — live checks are Dali's step.

## 1. Test files used / created

Rebuilt from CURRENT source by the tester (fresh binaries, prebuilt exes not trusted):

- `C:\Users\Dali\Documents\gaames\aoe3rate-mod\decrypt_unit.c` — standalone decrypt/encrypt unit (worker)
- `C:\Users\Dali\Documents\gaames\aoe3rate-mod\test_decrypt.c` → `tests\TESTER_decrypt.exe` (tester-rebuilt)
- `C:\Users\Dali\Documents\gaames\aoe3rate-mod\test_decrypt.py` — Python equivalent (run as-is)
- `C:\Users\Dali\Documents\gaames\aoe3rate-mod\test_nullsafe.c` → `tests\TESTER_nullsafe_O0/O1/O2/O3.exe` (tester-rebuilt, -O0..-O3)
- `C:\Users\Dali\Documents\gaames\aoe3rate-mod\tests\test_d3d9_actual.c` → `tests\TESTER_d3d9_actual.exe` (tester-rebuilt; compiles the REAL d3d9.c in TU via SWARM_TEST)
- `C:\Users\Dali\Documents\gaames\aoe3rate-mod\loadsmoke.c` → `tests\TESTER_loadsmoke.exe` (tester-rebuilt)
- `C:\Users\Dali\Documents\gaames\aoe3rate-mod\tests\loadsmoke_path.c` → `tests\TESTER_loadsmoke_path.exe` (tester-rebuilt)
- `C:\Users\Dali\Documents\gaames\aoe3rate-mod\tests\test_pe_structure.py` — PE parser (run as-is)

Ground truth used for the chain cross-check (independent of the mod):

- `C:\Users\Dali\Documents\gaames\ghidra_proj\aoe3y_decompiled\0086da39_FUN_0086da39.c`, `005009c0_FUN_005009c0.c`, `0044cb21_FUN_0044cb21.c`, `0044efff_FUN_0044efff.c`
- Fresh `objdump -d` of the deployed `age3y.exe` (call sites 0x88352e/0x883548/0x88356a, 0x71936c, income body 0x86da39-0x86dbd3, eco-score 0x5009c0-0x500a2c, helpers 0x44cb21/0x44efff)
- Byte dumps of the game image constants at 0xB856BC, 0xB9A5C8, 0xBA12E8, 0xBA12FC, 0xB84BE4, 0xC6DF10, 0xC6DF14, 0xC6DF38, 0xC66234, 0xC66664, 0xDCB808, 0xDCB360 (RVA-mapped via section table)

## 2. Runner + commands + output tails

Toolchain: `C:\Users\Dali\Documents\gaames\w64devkit-x86\w64devkit\bin\i686-w64-mingw32-gcc.exe` (GCC 16.2.0, i686; `w64devkit\bin` on PATH). Python: `C:\Python314\python.exe` (3.14.6). All from `C:\Users\Dali\Documents\gaames\aoe3rate-mod`.

```
gcc decrypt_unit.c test_decrypt.c -o tests\TESTER_decrypt.exe                rc=0
gcc test_nullsafe.c -o tests\TESTER_nullsafe_O0.exe                           rc=0
gcc -O1 test_nullsafe.c -o tests\TESTER_nullsafe_O1.exe                       rc=0
gcc -O2 test_nullsafe.c -o tests\TESTER_nullsafe_O2.exe                       rc=0
gcc -O3 test_nullsafe.c -o tests\TESTER_nullsafe_O3.exe                       rc=0
gcc tests\test_d3d9_actual.c -o tests\TESTER_d3d9_actual.exe                  rc=0
gcc loadsmoke.c -o tests\TESTER_loadsmoke.exe                                 rc=0
gcc tests\loadsmoke_path.c -o tests\TESTER_loadsmoke_path.exe                rc=0
```

Runs (output tails):

```
tests\TESTER_decrypt.exe
  PASS: verified key table matches game bytes
  PASS: decrypt(encrypt(x)) == x for all slots (0..1e6), no NaN/INF          rc=0 (2/2)

C:\Python314\python.exe test_decrypt.py
  PASS: decrypt(encrypt(x)) == x for slots 0..7 (0..1e6), no NaN/INF          rc=0

tests\TESTER_nullsafe_O0.exe  (identical O1/O2/O3)
  PASS: zero game-ptr cell -> g_res/g_inc NULL
  PASS: draw_hud with NULL resources returned (no crash)
  PASS: unmapped base -> g_res/g_inc NULL (guard)
  PASS: draw_hud with unmapped resources returned (no crash)
  PASS: out-of-range player id -> NULLs (bound check)
  PASS: decrypt_slot reads LIVE container (12345/54321)
  PASS: rate_for(0) == 500.0 (FUN_0086da39 math reproduced)
  PASS: g_res/g_inc set, no device, draw returned (no crash)
  ALL NULL-SAFETY CHECKS PASSED                                              rc=0 (8/8) x 4 levels

tests\TESTER_d3d9_actual.exe
  PASS: actual d3d9.c decrypt_slot round-trip 0..1e6, no NaN/INF
  PASS: rate_for(0) == 562.5 (FUN_0086da39 math)
  PASS: rate_for returns 0.0f without a live income object
  PASS: zero game-ptr cell -> NULLs, draw no-op (no crash)
  PASS: rate_for returns 0.0f when g_res NULL (main menu)
  PASS: unmapped page guard -> NULLs, draw no-op (no crash)
  ALL D3D9 ACTUAL CHECKS PASSED                                              rc=0 (6/6)

tests\TESTER_loadsmoke.exe (mod root)
  proxy loaded, Direct3DCreate9@71c85e50, Ex=present
  IDirect3D9 ok @71db6014; Release() -> 0                                    rc=0

tests\TESTER_loadsmoke_path.exe
  loaded module: C:\Users\Dali\Documents\gaames\aoe3rate-mod\tests\d3d9.dll
  PASS: loaded OUR proxy copy, not system32's d3d9.dll
  Direct3DCreate9 @ 71c85e50; wrapper vtable 71db4020, Release -> 0          rc=0
  -> SHOULD-FIX 3 (review round 1) GENUINELY FIXED: the exe derives its own dir
     and loads tests\d3d9.dll by explicit path; resolved path asserted == target
     and !system32.

C:\Python314\python.exe tests\test_pe_structure.py
  PASS x7: d3d9.dll machine==0x14C; exports Direct3DCreate9/Ex;
  imports NO d3dx9* (KERNEL32/msvcrt/USER32 only); age3y.exe 0x14C,
  ImageBase 0x400000; age3y.exe imports d3d9.dll                           rc=0 (7/7)
```

Objective counts: 0 failed, 0 errored across all runs.

## 3. Chain-vs-decompile cross-check (tester's own disassembly, not the mod's claims)

| Element (round-2 contract) | Decompile/disasm evidence | Verdict |
|---|---|---|
| `game = *(base+0x866234)` | `mov 0xc66234,%eax` (0x5009c0, 0x86dac2, 0x883570); file cell 0 (runtime) | **CONFIRMED** |
| `ctx = *(game+0x13c)` | `mov 0x13c(%eax),%ecx` (0x88352e, 0x86dac8; decompile `*(int*)(DAT_00c66234+0x13c)`) | **CONFIRMED** |
| `idx = *(base+0x866664)` | `mov 0xc66664,%edx` (0x883538/0x883582/0x8835e2); image byte at 0xC66664 = `0xFFFFFFFF` (**-1**, i.e. no current player in main menu — matches the `idx<0` guard) | **CONFIRMED** |
| bound `0<=idx<*(ctx+0x5c)` | FUN_0044cb21 disasm 0x44cb21: `jl`/`cmp 0x5c(%ecx),%eax; jge` return 0 | **CONFIRMED** |
| `player = *(*(ctx+0x58)+idx*4)` | FUN_0044cb21: `mov 0x58(%ecx),%ecx; mov (%ecx,%eax,4),%eax` | **CONFIRMED** |
| `g_res = *(player+0x230)` | eco-score 0x5009f1 `lea 0x230(%esi),%edi` then `mov %edi,%ecx; call 0x44efff` for slots 2/0/1/7; FUN_0044efff does `mov (%ecx),%ecx` then `xor key[slot] ^ container[slot]` — decrypt target IS the pointer stored at `player+0x230` | **CONFIRMED** |
| `g_inc = *(player+0x80)` | 0x883548 `mov 0x80(%eax),%eax` → `mov %eax,%ecx; call 0x86da39` (this) | **CONFIRMED** |
| income `+0x160/+0x164` signed ints (`fild`) | `fildl 0x160(%ebx)` 0x86daf2, `sub 0x164(%ebx),%edx` 0x86dad4, `add 0x160(%ebx),%edx` 0x86dada | **CONFIRMED** |
| income `+0x168` history array, `+0x16C` count | `mov 0x168(%ebx),%eax; mov (%eax,%edi,4),%ecx` (0x86db16-1c — ecx becomes FUN_0044efff's `this` = hist[j] container), `cmpl $0x0,0x16c(%ebx)` 0x86da54, `mov 0x16c(%ebx),%edi` 0x86da9e | **CONFIRMED** |
| snap `[ctx+0x108]` | `mov 0x13c(%edx),%eax; mov 0x108(%eax),%edx` 0x86dac8-ce | **CONFIRMED** |
| `v=(snap-prev)+cur` latest, else `cur` | `sub 0x164(%ebx),%edx; add 0x160(%ebx),%edx` latest branch vs `mov 0x160(%ebx),%eax` older branch, `cmp %eax,%edi/jne` 0x86dabe | **CONFIRMED** |
| negative → `+2^32` | `test %edx,%edx / test %eax,%eax; jge skip; fadds 0xba12fc` 0x86dae8-fc | **CONFIRMED** |
| `acc += v*0.001f` | `fmuls 0xb9a5c8; fadds (acc)` 0x86db02-0c; image bytes 0xB9A5C8 = `6F 12 83 3A` = 0x3A83126F = 0.001f | **CONFIRMED** |
| 60s window | call site pushes `0x42700000` = 60.0f (0x71936c/0x71938a/0x7193a8) | **CONFIRMED** |
| window < thr → 0 (`0xBA12E8`) | `fcomps 0xba12e8; jnp 0x86dbc9` (bail); image 0xBA12E8 = `BD 37 86 35` ≈ 1e-6f | **CONFIRMED** |
| `rate = sum*(1.0f/acc)` | inner per-slot sum: `call 0x44efff` (this=hist[j]) → `push; mov $0xdcb808,%ecx; call 0x49a859` (accumulate); final `flds 0xb856bc; fdivs acc` 0x86db5f-65; image 0xB856BC = `00 00 80 3F` = 1.0f | **CONFIRMED** |
| `sum += decrypt(hist[j], slot)` per sample | 0x86db16-2f (hist[j] container as this; slot in esi; accumulator 0xdcb808) — the mod's per-slot-only accumulation is exactly equivalent (slots are independent) | **CONFIRMED** |
| decrypt `float(key[slot] ^ res[slot])` + key table bytes | FUN_0044efff disasm 0x44f02e-3c; image 0xC6DF14 = `28 48 AC 4F ... 10 82 E4 F2` == `KEY_BYTES` in tests byte-for-byte | **CONFIRMED** |
| `0xDCB808` is scratch, NOT the live container | scratch referenced 8×, all inside FUN_0086da39 (0x86da79/94/2a/6e/7d/9b/b2 + 0xdcb810 flag) + init/zero via 0x44717f/0x4e2617; it is the ACCUMULATOR the income eval writes, reset per call (`FUN_004e2617(0)`, `movl $0,0x4(%esp)` passes 0) | **CONFIRMED** — old data layer was indeed wrong, round-2 rework is right |
| slot→resource order (0/1/2=food/wood/coin, 7=export) | eco-score decrypts slots 2,0,1,7 in one sum (coin/food/wood/export) | **CONFIRMED** (display names in draw_hud_text follow this order) |

Contradictions found: **none**. One doc nit: VERIFIED_ADDRESSES.md "Runtime notes" says the 0xDCBxxx region is "BSS — zero in the file image"; actually 0xDCB808 and 0xDCB360 have non-zero `.data` initial bytes. Irrelevant to round-2 code (it never reads the scratch) — doc-only.

## 4. PE + deploy hash results

- Machine: `pei-i386` (objdump) / 0x14C (parser) for d3d9.dll AND age3y.exe. ✓
- Exports: 11 (Direct3DCreate9, Direct3DCreate9Ex, 9 passthroughs) — match d3d9.def. ✓
- Imports: KERNEL32.dll, msvcrt.dll, USER32.dll only — **no d3dx9 hard link** (loaded at runtime). ✓
- `d3dx9_25.dll` (2,337,488 B) present in the game dir for the HUD font. ✓
- SHA256: workspace `d3d9.dll` == deployed `Age of Empires III - Complete Collection\d3d9.dll` == `tests\d3d9.dll` = `9C23C7936E9646A5425000C48CBF9CB75B4F0665ECBDEDA1E9DFB1666EB75134`, size 45,748 B — matches BUILD.md claim exactly. ✓
- Stale binaries: `test_nullsafe_o1.exe` (mod root) is gone; only current-source builds remain. ✓

## 5. Failures

**None.** All rebuilt-from-source tests pass at all optimization levels; the
decompile/disassembly cross-check CONFIRMS every element of the round-2 chain
and rate math; the Present-slot claim was independently re-verified against the
canonical DXSDK d3d9.h (order: GetSwapChain, GetNumberOfSwapChains, Reset,
**Present = vtable index 17**) — the hook targets the correct slot.

## 6. Not testable in this environment (UNVERIFIED-live)

1. Real HUD rendering on a live match screen (requires Dali launching the game).
2. The vtable slot-17 Present hook end-to-end (patch + forward + draw) — needs a
   real `CreateDevice` with adapter/window; load-smokes deliberately don't create one.
3. Whether displayed rates match the in-game ledger numbers for the same 60s window.
4. If the game ever creates devices with more than one distinct device vtable,
   `g_devvt`/`s_orig_present` are single-slot globals (second vtable's patch would
   overwrite the saved original) — watch in the live test.

## Verdict

**TESTS PASS**

---

# TEST.md — ROUND 5 — authoritative active-player rework (chain `idx = *(game+0x14c)` + probe-lite fallback)

Date: 2026-09-05. Tester re-verification of the round-5 rework described in `.swarm/BUILD.md`
round 5: the live log proved `idx=*(base+0x866664)` is the UI selection marker (-1 in a real
skirmish), so the engine's own in-game active player `*(game+0x14c)` is now the authoritative
index, with a probe-lite fallback scan over `*(ctx+0x58)`/`*(ctx+0x5c)` and the quiet chain log.
No game launch anywhere in this report — live checks are Dali's step.

## 1. Test files updated (this round)

- `tests\test_d3d9_actual.c` — REWRITTEN for the round-5 chain. New tests:
  - authoritative happy path: `game+0x14c==1` (n=3) resolves player#1 while the dead cell
    `*(base+0x866664)` holds -1; and `game+0x14c==0` resolves player#0 while the dead cell
    holds 1 — both directions prove the old source is ignored (a regression to
    `0x866664` FAILS these asserts).
  - live-value test: lays game=**0x05BF2000** / ctx=**0x14500000** (the round-4 live log
    addresses, n=3, idx=0 and 1) — player resolved, **NOT NULL**. Literal addresses ARE used:
    fixed VirtualAlloc of the 64KB-aligned superpage covering 0x05BF2000 + ctx at 0x14500000.
  - menu-like edge: `game+0x14c = -1/0xFFFFFFFF` with 3 players (p0 sane; p1 income+container
    but food==0 → probe-fail; p2 income but no container → probe-fail) → fallback picks
    player#0 AND the chain log `d3d9mod.log` (deleted first) carries `[src-fallback] [fallback: player#0]`.
  - OOB edge: `game+0x14c = 99`, n=3 → fallback picks player#0.
  - no-sane-player edge: idx in range but probe fails everywhere → NULLs + `[chain-break]`, no crash.
  - kept: decrypt round-trip via REAL `decrypt_slot` (0..1e6, no NaN/INF), `rate_for(0)==562.5`
    (FUN_0086da39 math), menu n==0 → NULLs + rate 0, zero-game-cell and unmapped-page guards.
  - Result: **14/14 PASS**.
- `test_nullsafe.c` — removed the stale `BASE_OFF_CURIDX 0x866664` define (dead-cell residue);
  ADDED case 3c: `game+0x14c` = -1/0xFFFFFFFF (the exact live value) → probe-lite fallback picks
  the sane player. Result: **10/10 PASS at -O0, -O1, -O2, -O3** (9 old cases + new 3c).
- `tests\test_source_contract.py` — ADDED round-5 contracts (22 checks total, all PASS):
  `#define OFF_GAME_ACTIVE_PLAYER 0x14c` present; **no `RVA_CUR_PLAYER_ID` reference remains**;
  `locate_resources_impl` reads `idx` from `game+OFF_GAME_ACTIVE_PLAYER`; chain defines present;
  `[src-authoritative]` + `[src-fallback]` + `[fallback: player#%d]` tags; fallback iterates
  `0..n`; probe rules `+OFF_PLAYER_RES`, `+OFF_PLAYER_INCOME`, `decrypt_slot_at(res,0) > 0.0f`;
  quiet-log format `chain src=game+0x14C ...` and the `g_chain_calls > 5` gate.
- `tests\d3d9.dll` — **fixture REFRESHED** (see finding below).

## 2. Runner + exact commands

Toolchain: `C:\Users\Dali\Documents\gaames\w64devkit-x86\w64devkit\bin\i686-w64-mingw32-gcc.exe`
(GCC 16.2.0 i686; its `bin` MUST be on PATH or the driver cannot find `cc1` under `libexec`).
Python: `C:\Python314\python.exe`. Working dir: `C:\Users\Dali\Documents\gaames\aoe3rate-mod`.

```
$env:PATH = "C:\Users\Dali\Documents\gaames\w64devkit-x86\w64devkit\bin;" + $env:PATH
gcc  decrypt_unit.c test_decrypt.c -o tests\TESTER_decrypt.exe                   rc=0
gcc  test_nullsafe.c -o tests\TESTER_nullsafe_O0.exe                             rc=0
gcc  -O1 test_nullsafe.c -o tests\TESTER_nullsafe_O1.exe                         rc=0
gcc  -O2 test_nullsafe.c -o tests\TESTER_nullsafe_O2.exe                         rc=0
gcc  -O3 test_nullsafe.c -o tests\TESTER_nullsafe_O3.exe                         rc=0
gcc  tests\test_d3d9_actual.c -o tests\TESTER_d3d9_actual.exe                    rc=0
gcc  loadsmoke.c -o tests\TESTER_loadsmoke.exe                                   rc=0
gcc  tests\loadsmoke_path.c -o tests\TESTER_loadsmoke_path.exe                  rc=0
```

Fixture refresh (was STALE): `Copy-Item d3d9.dll tests\d3d9.dll -Force`.

## 3. Run results (per test file / case)

| Test | Pass | Fail | Notes |
|---|---|---|---|
| tests\TESTER_decrypt.exe | 2 | 0 | key bytes + round-trip |
| test_decrypt.py | 1 | 0 | python equivalent |
| tests\TESTER_nullsafe_O0..O3.exe (4 runs) | 10 x4 | 0 | incl. new case 3c (idx=-1 live value → fallback) |
| tests\TESTER_d3d9_actual.exe | 14 | 0 | round-5 chain + fallbacks + log tag + decrypt/rate/menu |
| tests\TESTER_loadsmoke.exe | 1 | 0 | proxy loads, Create9 callable |
| tests\TESTER_loadsmoke_path.exe | 2 | 0 | loads OUR tests\d3d9.dll, NOT system32 |
| tests\test_source_contract.py | 22 | 0 | incl. 9 new round-5 contracts |
| tests\test_pe_structure.py | 7 | 0 | PE i386, exports, no d3dx9 import |

**Total: 95 PASS, 0 FAIL, 0 ERROR.** (`TESTER_d3d9_actual.exe` key output:
`PASS: live game=05BF2000 ctx=14500000 n=3 idx=0..1 -> player resolved, NOT NULL`;
`PASS: chain log carries [src-fallback] [fallback: player#0]`.)

Deploy-consistency (all three copies byte-identical): workspace `d3d9.dll` == `tests\d3d9.dll`
== `Age of Empires III - Complete Collection\d3d9.dll` = **86,805 B, SHA256
B979FC78FF7C39B9B5BC5A2A925D91477844837A4474A4D5FD267D282D8350CE** — matches the BUILD.md
round-5 claim EXACTLY.

## 4. Findings

1. **tests\d3d9.dll was STALE** — hash `4313E98E...` (86,403 B) ≠ the round-5 build
   (`B979FC78...`, 86,805 B). The worker's round-5 build notes did not mention re-copying the
   fixture; `loadsmoke_path` would have smoke-tested the OLD binary. Fixed by the tester
   (fixture refresh, listed in §2) — now byte-identical. Builder note for future rounds:
   refresh `tests\d3d9.dll` whenever `d3d9.dll` is rebuilt.
2. **First compile attempt failed** (`cannot execute 'cc1'`) until the w64devkit `bin` dir was
   prepended to PATH — this machine needs that for every gcc invocation (cc1 lives under
   `libexec\gcc\i686-w64-mingw32\16.2.0`, found via PATH).
3. `tests\d3d9mod.log` is generated by `TESTER_d3d9_actual.exe` runs (the quiet chain log under
   test) — it is a test artifact, not production output.

## 5. Not testable in this environment (UNVERIFIED-live, unchanged from prior rounds)

1. Real HUD rendering + authoritative/fallback tag on a live match screen (Dali's step;
   the expected healthy log tail is specified in BUILD.md §6).
2. Whether in-game the fallback never triggers because `*(game+0x14c)` is always in range
   (the live log will show which tag fires; both paths are now covered by tests).
3. End-to-end Present-hook vtable patching with a real device (load-smokes deliberately
   don't CreateDevice).

## Verdict

**TESTS PASS**