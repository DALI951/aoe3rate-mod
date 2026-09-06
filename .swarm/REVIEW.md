# REVIEW.md — Inspector report: AoE3 TAD resource-rate HUD mod (d3d9 proxy DLL)

Reviewer: Inspector (read-only). Date: 2026-09-05.
Reviewed against: `.swarm/PLAN.md`, `.swarm/TEST.md`, `.swarm/BUILD.md`,
`VERIFIED_ADDRESSES.md`, `d3d9.c`, `d3d9.def`, `tests/*`, fresh Ghidra
decompile of `age3y.exe`, and direct disassembly of the deployed binary.

---

## Executive verdict

**CHANGES REQUIRED**

The build/deploy/test pipeline is mechanically sound and every shipped test
passes — but the **data layer is wrong**. Fresh disassembly proves that:

1. `*(0xDCB808)` (the mod's `g_res`) is the game's **income-projection
   scratch accumulator**, not the current player's live resource container.
   The HUD's resource **counts** will show zeros or leftover projection
   values, never the actual food/wood/coin/export.
2. The planned rate fields `+0x160/+0x164/+0x168` exist on a **different
   object** than `g_res`, are **signed INTs (not floats)**, and `+0x168` is a
   **history-container-array pointer** (with the sample *count* at `+0x16C`),
   not a "coin rate". `rate_for()` will reliably return garbage (guarded, so
   no crash — but wrong numbers every frame).

The existing tests cannot catch this: they synthesize memory and verify the
cryptographic math + the guard behavior only, never the live data path.

---

## Findings

### MUST-FIX 1 — HUD reads the wrong container: `g_res = *(0xDCB808)` is a scratch buffer

- **WHAT:** `locate_resources()` / `decrypt_slot()` / `draw_hud_text()` read
  resource counts from `*(base + 0x9CB808)` = `*(0xDCB808)`.
- **WHERE:** `d3d9.c:25` (`RVA_RES_SLOT_PTR`), `d3d9.c:96-101`,
  `d3d9.c:81-91`, `d3d9.c:158-188`.
- **WHY it's wrong (proven from disassembly, not speculation):**

  | Evidence | Source |
  |---|---|
  | `0xDCB808` is referenced in the whole `.text` exactly **8 times**: 7× inside `FUN_0086da39` (0x86da79/94/86db11…/86dbb2) + 1× at the code label `0xB82430` passed to `FUN_00469971`. Nothing else in the game ever reads or writes the cell. | full `objdump -d` scan of `age3y.exe` |
  | `FUN_0086da39` = the game's "X Minute Income" eval: lazily inits the container (`FUN_0044717f` → `FUN_004470db`, which **allocates a fresh block** via `FUN_0040113c` and fills it with the key table → all slots decrypt to **0.0**), then resets all slots to 0 (`FUN_004e2617(v=0)`), accumulates per-sample sums, scales, and re-encrypts. | `004470db_FUN_004470db.c`, `004e2617_FUN_004e2617.c`, disasm 0x86da39-0x86dbe0 |
  | The cell is written **only once** (lazy init, guarded by `*param_1 == 0`); there is **no** `mov [0xDCB808],…` anywhere to re-point it at a per-player container. | FUN_004470db:11, full scan |
  | The economic score (`FUN_005009c0`) reads slots 2/0/1/7 from **`param_1 + 0x230`** (`lea 0x230(%esi),%edi` → `mov %edi,%ecx` → `call 0x44efff`), **not** from `0xDCB808`. So row 7 of VERIFIED_ADDRESSES confirms the *slot order* (coin/food/wood/export), **not** the container identity. | disasm 0x5009f1-0x500a2c, FUN_005009c0:22-25 |
  | `FUN_0086da39`'s real `this` = per-player object: at 0x71937a `mov ecx,[esi+8]`; at 0x88356a `mov ecx,eax` where `eax=[player+0x80]`. | disasm of both call sites |

- **RISK:** the HUD's food/wood/coin/export counters show the last income
  projection's leftover accumulator values (0.0 until the game first runs the
  income query) — i.e., *wrong data on screen every frame*. No crash (guards),
  but the feature does not work.
- **FIX:** resolve the real per-player resource container and read slots from
  it — e.g. the container at `[player + 0x230]` (exact layout verified for the
  eco-score path) or the object passed as `this` to `FUN_0086da39`
  (`[esi+8]` / `[player+0x80]`). Re-verify each slot against a known game
  state (start a match, compare with the in-game UI).
- **TRUST:** FACT (disassembly evidence above).

### MUST-FIX 2 — Rate fields: wrong base object + wrong types + `+0x168` is a pointer, not a rate

- **WHAT:** `rate_for()` reads `g_res+0x160/0x164/0x168` as floats.
- **WHERE:** `d3d9.c:29-31` (`OFFSET_RATE_FOOD/WOOD/COIN`), `d3d9.c:116-132`;
  same assumption in `PLAN.md:92,102` and `VERIFIED_ADDRESSES.md` row 5.
- **WHY it's wrong:**
  - The game reads these offsets on **`FUN_0086da39`'s `this`** (per-player
    object), **not** on the slot array pointed to by `*(0xDCB808)` — and
    `VERIFIED_ADDRESSES.md` row 5 itself admits: "`this` … **not** the g_res
    slot array".
  - `+0x160` / `+0x164` are read as **signed ints** in the game
    (`fildl 0x160(%ebx)` at 0x86daf2; `iVar3 = *(int*)(param_1+0x160)` in the
    decompile) — not floats.
  - `+0x168` is a **pointer to an array of history containers**
    (`mov 0x168(%ebx),%eax; mov (%eax,%edi,4),%ecx` at 0x86db16-0x86db1c),
    and `+0x16c` is the **sample count** (`cmpl $0x0,0x16c(%ebx)` at
    0x86da54). There is no "coin rate" at `+0x168`; on the scratch `g_res`
    (a freshly key-filled 8-DWORD block) `+0x168` is well past the
    allocation and shifts as the block pointer itself does.
  - VERIFIED_ADDRESSES row 5 says "+0x168 NOT referenced" — **false**: the
    disassembly reads it at 0x86db16 (as the history array base).
- **RISK:** rates display garbage (clamped, so bounded noise) — the feature's
  headline numbers are meaningless even after fixing MUST-FIX 1's base, unless
  the field semantics are re-derived.
- **FIX:** decode the real rate semantics on the per-player object: `+0x160`
  (int amount sample), `+0x164` (prev sample), `+0x16c` (count), `+0x168`
  (history array). The game computes `local_8 = 0xB856BC / local_8` then
  multiplies slot values — reproduce the game's own math rather than
  floating-reinterpreting ints.
- **TRUST:** FACT for the int-vs-float, pointer-at-0x168 and wrong-object
  points (disassembly). UNSURE on the *exact* formula to display as
  "rate/sec" — needs live-game values to pin down.

### SHOULD-FIX 3 — TEST.md claims `loadsmoke_path.exe` proves the proxy loads; it actually loads SYSTEM32's d3d9.dll

- **WHAT:** `.swarm/TEST.md:45` "(from mod dir) -> rc=0, **loaded OUR mod
  d3d9.dll**" and `TEST.md:19,67` "proves the proxy actually loads
  in-process **from its own directory**".
- **WHERE:** `tests\loadsmoke_path.exe` (source `tests\loadsmoke_path.c`).
- **WHY:** executed from the mod root it printed
  `loaded module: C:\Windows\SYSTEM32\d3d9.dll` — the exe lives in `tests\`
  (app dir contains no `d3d9.dll`), and system32 precedes the CWD in the
  SafeDllSearchOrder, so the *system* D3D9 wins. The test as built cannot
  load the proxy.
- **The proxy loading genuinely works** — proven by `loadsmoke.exe` (in the
  mod root, app dir has `d3d9.dll`): it bound the **proxy's** exported
  `Direct3DCreate9` (module at different base, wrapper object, `Release()→0`).
- **RISK:** none to the shipped DLL (the real game loads the proxy via
  app-directory-first next to `age3y.exe`), but the test claims are false and
  mislead future debugging.
- **FIX:** update TEST.md wording, or place a copy of `d3d9.dll` next to
  `loadsmoke_path.exe` and re-run.
- **TRUST:** FACT (executed run output).

### NIT 4 — Stale "SEH" comment in d3d9.c

- **WHERE:** `d3d9.c:6` ("All runtime memory reads are SEH-guarded (MinGW
  __try1/__except1)") vs `d3d9.c:47-51` (SEH empirically broken on
  i686-w64-mingw32 16.2.0; the authoritative guard is `VirtualQuery`).
- **Why it exists:** wording from an earlier design revision; the code is
  correct (VirtualQuery guard). **Risk:** doc-only confusion.
- **TRUST:** FACT.

### NIT 5 — Stale `test_nullsafe_o1.exe` in the mod root

- **WHERE:** `C:\Users\Dali\Documents\gaames\aoe3rate-mod\test_nullsafe_o1.exe`.
- **WHY:** leftover SEH-era build (contains "DBG: case3 start" strings and
  fails its own case 3: "FAIL: valid cell should set g_res", exit 1). Not in
  current source. Current `test_nullsafe.exe` and `tests\nullsafe_O1/O3.exe`
  pass 5/5.
- **RISK:** someone runs the wrong binary and believes the guard is broken.
  Recommend deleting it (I am read-only).
- **TRUST:** FACT (executed run + string scan).

### UNVERIFIED-live 6 — Present-hook itself

- **WHERE:** `d3d9.c:206-219` (patches device vtable `&vt[17]` = `vt+0x44`;
  slot 17 = `IDirect3DDevice9::Present`).
- **WHY not verified:** patching/forwarding requires a real D3D device, which
  needs an adapter/window — impossible in this environment (and the game must
  not be launched here). Code inspection is sound; `TEST.md:82` correctly
  defers this to the live step.
- **TRUST:** UNSURE (code-inspected only).

---

## What is confirmed GOOD (all independently checked)

- **Decrypt math matches the game exactly:** `FUN_0044efff` =
  `float(uint(key[slot] ^ *(arr+slot*4)))`; key table `0xC6DF14` (8 dwords,
  bytes match the file), count `0xC6DF38`, secondary key `0xC6DF10`. Verified
  against the decompile *and* the live bytes. ✓
- **Direct deref without player chain** is right *for that container*
  (VERIFIED_ADDRESSES row 4); the failure is only that the container chosen is
  the scratch, not the live one (MUST-FIX 1).
- **Divisor `0xB856BC`** (RVA `0x7856BC`): bytes `00 00 80 3F` = 1.0f;
  d3d9.c guards `!(div > 0.0f)` → 1.0f. ✓ (Note: the game uses it as a
  *dividend* in its own projection math — cosmetic difference at init value
  1.0.)
- **NULL-safety:** every read goes through `VirtualQuery`-guarded
  `safe_r32/safe_rf/safe_rptr`; `draw_hud_text` no-ops when `g_res==NULL`
  (main menu). Tests drain the guard paths (5/5). ✓
- **Proxy transparency:** `DllMain` loads real d3d9 by full path from
  system32; no hard d3dx9 link (LoadLibrary/GetProcAddress at runtime);
  import table = KERNEL32/msvcrt/USER32 only; 11 exports match `d3d9.def`;
  `age3y.exe` imports **only** `Direct3DCreate9` from d3d9.dll → the proxy
  fully satisfies the game. PE/structure tests 7/7. ✓
- **Deploy sanity:** `d3d9.dll` in the game dir = 43,664 B, MD5
  `BEBCDF36…` = workspace build; `d3dx9_25.dll` (2,337,488 B) present; game
  NOT launched. ✓
- **Tests assert the plan's mechanics:** `test_d3d9_actual.c` compiles the
  REAL `d3d9.c` (SWARM_TEST) and passes 5/5; decrypt round-trip (0..1e6,
  no NaN/INF) passes; PE structure passes. What the tests cannot assert is
  live data correctness — that is exactly where the two MUST-FIXes live.

---

## Rule-7 completeness check — "Is this project done?"

- Stubs/TODOs in `d3d9.c` / `d3d9.def`: **none**. `build_log.txt` empty
  (clean build). Tests all green.
- Missing pieces that block completion:
  1. **Correct live-data source** (MUST-FIX 1): the HUD reads a scratch
     buffer; counts and rates are not the player's actual resources.
  2. **Correct rate semantics** (MUST-FIX 2): int fields on the per-player
     object, `+0x168` as history-array pointer, sample count at `+0x16C`.
  3. **Live in-game verification** (Dali's step per TEST.md:80-82) — but note
     this is not a "maybe it works" punt: the current addresses are
     *objectively wrong per disassembly*, so the in-game check would fail.
     "Done" = HUD numbers match the game's own UI for counts and rate-of-
     change in a live match.
  4. Test-claim cleanup (SHOULD-FIX 3) and NITs 4-5.
- Verdict: **NOT DONE** — mechanically built and tested, functionally wrong
  at the data layer.

---

# VERDICT: CHANGES REQUIRED

1. `d3d9.c:25,96-101,81-91` — `g_res=*(0xDCB808)` is the game's income-
   projection scratch (proven: only 8 refs in all of `.text`, all in
   `FUN_0086da39` chain; cell never re-pointed; eco-score reads `[player+0x230]`
   instead). Counts will be zeros/leftovers. Fix: read the per-player resource
   container (e.g. `[player+0x230]` or FUN_0086da39's `this` =
   `[esi+8]`/`[player+0x80]`), re-verify against live UI.
2. `d3d9.c:29-31,116-132` + `PLAN.md:92,102` — rate fields are on the wrong
   object, are signed **ints** (`fildl`), and `+0x168` is a **pointer** to the
   history-container array (count at `+0x16C`); no float "coin rate" exists
   anywhere near these offsets. As built, `rate_for()` shows garbage. Fix:
   derive rate from the per-player sample ints (reproduce `0xB856BC / accum`
   math) and re-pin every slot in a live match.
3. `TEST.md:19,45,67` — `loadsmoke_path.exe` does **not** load the proxy; my
   run printed `C:\Windows\SYSTEM32\d3d9.dll` (exe in `tests\` → system32
   precedes CWD). Proxy loading is real (proven by `loadsmoke.exe` in mod
   root). Fix: correct the claim or place `d3d9.dll` beside the test exe.
4. `d3d9.c:6` — "SEH-guarded" comment stale vs VirtualQuery guard (lines
   47-51). Doc-only.
5. `test_nullsafe_o1.exe` (mod root) — stale SEH-era binary, fails its own
   case 3, exit 1. Recommend deletion.