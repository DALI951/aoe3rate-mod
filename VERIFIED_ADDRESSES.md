# VERIFIED_ADDRESSES.md — AoE3 TAD (age3y.exe) resource HUD address map

Source of truth: fresh Ghidra 12.1.3 decompile (JDK 25) of the vector:
`C:\Users\Dali\Documents\gaames\Age of Empires III - Complete Collection\age3y.exe`
Debase RVA = VA − 0x400000. Results recorded on 2026-09-05.

## Summary table

| # | Known fact (prior session) | Verdict | Fresh value / notes |
|---|---|---|---|
| 1 | Function `0x44EFFF` decrypt: `*(float*)(arr + 4*slot)` XOR `key[slot]`, key via `*(DWORD*)(0xC6DF14 + 4*slot)` | **CONFIRMED (round 2)** | `FUN_0044efff` decompiles exactly: `*(uint*)(*(this) + slot*4) ^ *(uint*)(0xC6DF14 + slot*4)` cast to float; guard `0 <= slot < *(DWORD*)0xC6DF38`. The container cell `this` (`*param_1`) is **NOT 0xDCB808** — see rounds 2/3. Call sites: eco-score `FUN_005009c0` (`lea 0x230(%esi),%edi; mov %edi,%ecx`) → **`this` = `[player + 0x230]`**, the CURRENT PLAYER's live encrypted resource container. |
| 2 | `0xC6DF14` bytes == `28 48 AC 4F 94 F8 3A 35 8B D8 4C 3F AB 12 FB AF 20 B3 5B CA F9 AB C4 2A B1 A1 CF DA F2 E4 82 10` | **CONFIRMED** | Memory dump of 0xC6DF10..0xC6DF37 matches exactly (0x30-byte region). Key is 8 dwords (slots 0..7): `0x4FAC4828, 0x94F83A35, 0x8BD84C3F, 0xAB12FBAF, 0xCA5BB320, 0xF9ABC42A, 0xB1A1CFDA, 0xF2E48210`. |
| 3 | Count `0xC6DF38` == 8; secondary key `0xC6DF10` | **CONFIRMED / count runtime-initialized** | `0xC6DF38` is 4 zero bytes in the file image (BSS, written to 8 at startup); every decoder guards `0 < 0xC6DF38` and loops while slot `< 0xC6DF38`. Secondary key **0xC6DF10 = 0xFBCC4F02** (bytes `02 4F CC FB`). |
| 4 | Container via `mov eax,[0xDCB808]` — DIRECT deref is correct | **SUPERSEDED (round 2 — was WRONG)** | 0xDCB808 is the game's **income-projection scratch** (7 refs inside `FUN_0086da39`, init by `FUN_004470db`, zeroed by `FUN_004e2617`) — NOT the current player's live resources. The HUD must resolve the current player and read `[player + 0x230]` (round-2 contract). |
| 4b | Live container: `game=*(base+0x866234)`, `ctx=*(game+0x13c)`, `idx=*(base+0x866664)` (current player id, -1 none), `players=*(ctx+0x58)`, `n=*(ctx+0x5c)`, `player=*(players+idx*4)` guarded `0<=idx<n` (FUN_0044cb21), `g_res=*(player+0x230)` | **CONFIRMED (round 2)** | Call site 0x88352e-0x88356a: `context=[game+0x13c]`, `FUN_0044cb21(context, [0xc66664])` (= wrapper: `-1 < idx < *(p+0x5c)` → `*(*(p+0x58) + idx*4)`), result's `+0x80` passed as `this` to `FUN_0086da39`. Eco-score reads `[player+0x230]` containers for slots 2/0/1/7. |
| 5 | Rate fields at `0xDCB808+0x160/0x164/0x168` (floats) — REPLACED by FUN_0086da39 math | **REWORKED (round 2)** | `FUN_0086da39` ("X Minute Income"; this = income obj `[player+0x80]`) reads **signed ints** `[this+0x160]` (cur) / `[this+0x164]` (prev), `[this+0x16c]` (sample count), history **array** at `[this+0x168]` (per-entry container pointers). Algo: window<`0xBA12E8`→0; latest sample `v=(snap−[this+0x164])+[this+0x160]` with `snap=[ctx+0x108]`, older samples `v=[this+0x160]`; negative→`+` **2^32 (0xBA12FC)**; `acc+=v*0.001f (0xB9A5C8)`; `sum[slot]+=decrypt(hist[j],slot)`; result `= sum*(0xB856BC(1.0f)/acc)`. HUD reproduces this exactly (window 60.0f). |
| 6 | Helpers `0x49A859`, `0x4470DB`, `0x7E8171` → decrypted buffer `0xDCB360` | **CONFIRMED** | `0x49A859` = add/encrypt (`new = (float(key^old)+amount) ^ key`). `0x4470DB` = init: alloc block, fill with key table, store `[+4] = 0xC6DF10`. `0x7E8171` = decrypt-copy to **0xDCB360** + checksum (`FUN_00a40016`) + secondary-key verify. `0xDCB360` is zero in image (runtime BSS buffer). |
| 7 | Slots → resources: 0/1/2=food/wood/coin, 7=export | **CONFIRMED** | `FUN_005009c0` reads slots 2,0,1,7 together for the economic score (`FUN_0044efff(2/0/1/7)`) — consistent with coin/food/wood/export. Note `0x58EF83` (set slot) and `0x58EF02` (re-checksum: `[+4] = (uint)float-sum ^ secondary_key`) confirm container layout: **`[0xDCB808]+0 = slot array ptr`, `[0xDCB808]+4 = checksum`**. |

## Facts the d3d9.c implementation relies on (round 2 — all CONFIRMED above)

- Current-player chain (every deref VirtualQuery-guarded):
  `game=*(base+0x866234)` → `ctx=*(game+0x13c)` → `idx=*(base+0x866664)` (bound `0<=idx<*(ctx+0x5c)`) → `player=*(*(ctx+0x58)+idx*4)`.
- `g_res = *(player + 0x230)` → the CURRENT PLAYER's live encrypted 8-slot container (NOT 0xDCB808 scratch).
- `value[slot] = float( *(uint*)(BASE + 0x86DF14 + slot*4) ^ *(uint*)(g_res + slot*4) )`, slot `< count(0x86DF38)`.
- `g_inc = *(player + 0x80)` → income object; `rate_for(slot)` reproduces FUN_0086da39: window 60.0f, `acc += (snap − prev + cur | cur) * 0.001f` (+2^32 if negative), `sum += decrypt(hist[j], slot)`, `rate = sum * (1.0f / acc)`.

## Runtime notes

- Everything in 0xDCBxxx region (scratch, 0xDCB360 buffer) and 0xC6DF38/0xC66664 is **BSS — zero in the file image**; only key tables / constants (0x79A5C8, 0x7A12E8, 0x7A12FC, 0x7856BC) live in image. Reading through `safe_r32`/`safe_rf` (VirtualQuery guard) is mandatory.
- Secondary key 0xFBCC4F02 participates only in the game's own anti-cheat checksum; the HUD does not need to reproduce it (pure read).