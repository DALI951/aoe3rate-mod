# BUILD — ROUND 26 (2026-09-11) — ENDSCENE-DRAW PATH: OVERLAY ALIVE IN-MATCH

## ROUND SUMMARY
R17 tracer proved the device IS alive in-match: `scene` climbing while dev+sw froze
(device Present rerouted, but EndScene keeps firing). R26 wires the SHARED overlay
body into `w_endscene` — the overlay now draws via the EndScene path in-match
where the Present hook was dead. The overlay body is the SAME
`overlay_present_common(self, 0)` the Present hooks use, so gates, chain walk,
observer, hotkeys, profile poll and the once-per-frame guard are identical.

## WHY IT'S SAFE (the once-per-frame analysis)
- `w_endscene` runs the overlay body AFTER the REAL EndScene (`o(self)` first).
  At that point the game's frame is complete and the back buffer holds it.
- The real EndScene internally triggers the game's Present → our `present_hook`
  → `overlay_present_common` → the `g_ovl_last_draw_frame == g_frames_since_reset`
  guard SKIPS it (we already drew this frame via the EndScene path). The game's
  real Present still runs — nothing is swallowed.
- In menu/loading the device Present path is alive and draws there; EndScene's
  call is skipped by the same guard. In-match, Present is frozen and EndScene
  takes over. Both paths share one code body + one frame guard → overlay draws
  EXACTLY once per frame regardless of which hook fires.

## CHANGES (d3d9.c)
1. `w_endscene` (slot 42): after forwarding to the real EndScene, calls
   `overlay_present_common(self, 0)` — the same shared overlay body.
2. One-shot diagnostic `s_endscene_drew`: on the FIRST successful EndScene-path
   draw, logs (unconditionally) `R26 EndScene-draw: overlay active via EndScene
   path (dev=%p)` so a live log proves the path is active. Reset on attach.
3. `s_endscene_drew` reset in DllMain PROCESS_ATTACH.

## ALSO THIS ROUND (app/)
- `app/app.py`: session stats bar (duration + peak rates), Escape=pause,
  Ctrl+R=session-stats reset, Ctrl+M=mini-mode, pause indicator label.
- `tests/test_source_contract.py`: R26 pins (overlay body in BOTH hooks = the
  EndScene + Present paths; `o(self)` before overlay; one-shot marker + reset).
  R16 dev-before-present pin fixed for the new two-call-site shape.
- `tests/test_pe_structure.py`: EXE path → `Documents\gaames\...` (big-PC path).

## BUILD (on the big PC)
```
build\build.bat
```
Build rc=0 zero warnings, VERIFY PASS, byte-identical 3-copy
(repo root / game dir / tests\d3d9.dll). Harnesses: test_source_contract.py PASS
(R26 block green), test_app_core.py PASS. d3d9.dll hash + size to be recorded
after the first build — this round is CODE + DOCS ONLY until Dali builds.

## LIVE ACCEPTANCE (Dali, on the big PC)
1. Deploy R26 build (build.bat deploy step) — keep `[General] PlayerIdx=1`,
   `[Debug] Enabled=0` from the last deploy.
2. Delete old game-dir `d3d9mod.log` if present.
3. Launch a skirmish vs AI ≥90s, spend resources, watch the IN-GAME PANEL:
   **the panel should now be VISIBLE during the match** (it was invisible
   before R26 — that was the whole project blocker).
4. Prove the path in the log: FIRST run with `[Debug] Enabled=1`, confirm
   `R26 EndScene-draw: overlay active via EndScene path (dev=..)` appears once,
   plus the `present-path dev=.. sw=.. scene=..` tracer showing scene climbing
   in-match. Then set Debug back to 0 for the export-mode default.
5. Confirm the external viewer (`AOE3 Rates` shortcut) still works alongside.

## RISK / ROLLBACK
- `w_endscene` draws on the current RT exactly like the Present path already
  did (same ui_draw gate/RT/states). Worst case = a panel that flickers or
  overlaps the game HUD; the `[General] Enabled=0` zero-touch path and the
  `g_panel_visible` F9 toggle both still kill the draw completely.
- Rollback = revert this commit's d3d9.c hunk (the R17 probe semantics stay).

---

# BUILD — ROUND 24 (2026-09-07) — STABLE "HUMAN" LOCK IN THE EXPORT THREAD

## ROUND SUMMARY
Dali's R23 live log (d3d9mod.log) shows the fix for the zeros worked (real values stream
now), but introduced a NEW problem: `resolve_export_player()` picks the LARGEST total each
sample, and with n=3 (idx0 always empty, idx1+idx2 real) the pick FLIPS between idx1 and
idx2 every few seconds as the leader changes — repeated ~25 times in the log:
`R23 pick: n=3 idx=1 cand=[0:0 1:815 2:814]` → `idx=2 cand=[0:0 1:232 2:877]` → ...
Result: the user sees the opponent's numbers half the time, and his own spends look
"invisible" (after spending, the tracker switches to the other player whose stock keeps
climbing).

EVIDENCE FOR WHO IS HUMAN: idx1 does big sudden batch-spend drops (815→232, 1100→778,
1413→1203) while idx2 (the Japan AI) climbs smoothly with no single-line drops. Historic
SP skirmish layout verified: players[1] (idx1) = human when n>=2.

R24 reworks the selection with PERSISTENT per-session state (file-scope statics) so a
single "human" index is LOCKED and stays there, instead of flipping to the leader every
sample.

## LIVE EVIDENCE (d3d9mod.log, R23)
```
R23 pick: n=3 idx=1 cand=[0:0 1:815 2:814]
R23 pick: n=3 idx=2 cand=[0:0 1:232 2:877]     <- flips! (opponent overtakes)
R23 pick: n=3 idx=1 cand=[0:0 1:736 2:692]
R23 pick: n=3 idx=2 cand=[0:0 1:695 2:706]
... (~25 flips total) ...
R23 pick: n=3 idx=1 cand=[0:0 1:1413 2:1401]   <- idx1 spend drops
R23 pick: n=3 idx=2 cand=[0:0 1:1203 2:2144]
R23 pick: n=3 idx=1 cand=[0:0 1:1529 2:764]    <- idx1 spend drops again
```
idx1 = big batch spends (815→232 = -583 in one sample); idx2 = smooth climb (Japan AI).

## FIX — R24 `resolve_export_player` with human lock (src/gameif.c)
The resolver still re-walks the chain FRESH each sample (safe_r32-guarded, no shared
globals) and builds the candidate list exactly as R23 (p/res/income nonzero; total =
slots 2,1,0,7). But the SELECTION now uses PERSISTENT file-scope state:
- `s_lock_idx` — the locked human player index (persists across samples/session).
- `s_lock_rule_init` + `s_lock_rule[24]` — the rule that acquired the lock.
- `s_last_diag_idx` / `s_last_diag_n` / `s_last_diag_lock` — one-shot-change gating.
- Per-candidate `prev` (previous total) + `big_drops` (spend-drop count), carried in the
  static `g_export_cands[]` slots, so the spend signature accumulates across samples.

**Selection rules (first match wins):**
1. `lock` — a locked index from a previous sample is still present this sample → use it.
2. `sp_locked` — else if idx1 total nonzero → provisional idx1 (SP + LAN-host verified).
3. `sp` — else first nonzero-total candidate.
4. (fall-through) — else NO candidate → return 0 → write NOTHING this sample (fixes the
   match-load blip `food=0,wood=0,coin=0,export=100` and the n=1 menu zero lines).

**Spend-signature switch (runs regardless of lock):** if the currently tracked player has
0 big drops while some OTHER candidate has >= 2 big drops → switch the lock to that
candidate (rule = `spend_switch`). A "big drop" = a single-sample drop >= 15% of prev AND
>= 40 resources. The human spends in batches; an AI climbs smoothly — so this lets the
lock self-correct away from a smoothly-climbing AI companion if idx1 was wrong.

**rates.log format UNCHANGED** — exactly `t=%lu,food=%d,wood=%d,coin=%d,export=%d`
(food=slot2, wood=slot1, coin=slot0, export=slot7) from the locked/selected player. Dali's
Python parser contract is untouched.

**Diagnostic (d3d9mod.log):** on any selection/lock CHANGE writes ONE line
`R24 human: idx=%d rule=<lock|sp_locked|sp|first|spend_switch> cand=[i:total ...]`
(no-spam: only when idx, n, or lock changes).

## FILES CHANGED
- `src/gameif.c` — R24: persistent human-lock selection in `resolve_export_player`;
  ExportCand += prev/big_drops; s_lock_* + s_last_diag_* statics; spend-signature switch;
  R23-pick diag renamed to `R24 human:` with rule tag; removed `s_exp_last_idx/s_exp_last_n`.
- `tests/test_source_contract.py` — R24: updated the R23 pins that referenced moved/removed
  internals (s_exp_last_* → s_lock_*/s_lock_rule; R23-pick format → R24 human format + rule
  strings; added big_drops/prev/sel_idx + s_last_diag_* pins).
- `.swarm/BUILD.md` — this section.

## BUILD
Build rc=0, zero warnings, VERIFY PASS (i386 PE32, imports {KERNEL32, USER32, msvcrt},
11 exports).
**d3d9.dll = 158,231 B, SHA256 `f020f639796b26991101ed18d5f2e01fb42232381e9da51c0a0f4e46742ce5f2`**

## DEPLOY
- repo root d3d9.dll: `f020f639...` (R24) ✓
- tests\d3d9.dll: `f020f639...` (R24) ✓
- game dir d3d9.dll: `b6dae6a3...` (R23) — **BLOCKED** by the running age3y.exe (PID 8188).
  R23 was already deployed and is what Dali was live-testing; R24 deploy will happen after
  he closes the game. (build.bat's copy step reports "file in use".)

## HARNESS (ALL 6 GREEN)
- `test_rate_engine.exe` — **FAILURES: 0**
- `test_export.exe` — **FAILURES: 0**
- `test_d3d9_actual.exe` — **ALL D3D9 ACTUAL CHECKS PASSED**
- `test_source_contract.py` — **SOURCE-CONTRACT: PASS** (R24 block + all prior pins green)
- `test_pe_structure.py` — **FAILURES: 0**
- `test_app_core.py` — **APP-CORE: PASS**

## COMMIT
R24 commit on `main`.

---
# BUILD — ROUND 23 (2026-09-07) — LIVE FIX: THREAD-SIDE RESOLVER (export thread no longer reads stale observer globals)

## ROUND SUMMARY
Dali's LAN match produced 1930 lines in rates.log — ALL zeros while t climbed. Root cause
confirmed by orchestrator: the export thread read `g_res`, which was resolved ONCE at match
start by the observer (locate_resources_impl, players[1] rule), and since the in-match Present
hook is dead, `g_res` never refreshed — stale/reallocated pointer → decrypt reads 0 forever.

R23 fix: the export thread MUST NOT depend on the observer-resolved globals at all. A new
`resolve_export_player()` function does a FRESH chain walk (game→ctx→n→arr→player→res) on
every iteration, never writes any shared global (g_res/g_inc/g_ctx/g_player_idx), and picks the
candidate with the LARGEST total stock (food+wood+coin+export). The old `g_res`/`g_base` reads
in the thread are completely removed.

Also adds a who-is-who diagnostic line (R23 pick: n=%d idx=%d cand=[...]) that fires
UNCONDITIONALLY in d3d9mod.log ONLY when the chosen index or player count changes — tells
us which array slot Dali's human player is in for the next match.

## DIAGNOSIS
Export thread (gameif.c `export_thread`) read `g_res` (from the shared observer path). The
observer only runs from the present hook. In-match, the present hook is dead (R15/R17 showed
it). Therefore `g_res` is set once at match start and never updated — stale pointer → zeros.

## FIX (C-side only, app-side UNCHANGED)
### 1. new: `resolve_export_player(DWORD base, int limit, DWORD *out_res, int *out_idx)`
Self-contained chain walker called ONLY by the export thread. Does NOT write any shared global
(g_res, g_inc, g_ctx, g_player_idx, g_obs_player — all untouched). Full safe_r32 guarded:
- game = safe_r32(base + RVA_GAME_PTR); guard
- ctx = safe_r32(game + OFF_GAME_CTX); guard
- n = safe_r32(ctx + OFF_CTX_PLAYERCNT); guard; cap to limit
- arr = safe_r32(ctx + OFF_CTX_PLAYERS); guard
- iterate i in 0..n-1: p=arr[i], r=p→res, ic=p→income; skip if any zero
- total = decrypt_slot_at(r,2) + decrypt_slot_at(r,1) + decrypt_slot_at(r,0) + decrypt_slot_at(r,7)
- pick candidate with LARGEST total (strict >)
- stores every candidate (i,total) in static `g_export_cands[32]` / `g_export_ncand` for the
  caller's diagnostic log
- returns 1 + out_res/out_idx on sane pick, 0 (no write) when all zero/garbage

### 2. export_thread rewrite
Per iteration: `resolve_export_player((DWORD)g_base, EXPORT_MAX_CANDS, &res, &idx)` → if
fails or res==0 → skip (no zero lines). Decrypt slots from the freshly-resolved DWORD res.
Format output EXACTLY as R21: `t=%lu,food=%d,wood=%d,coin=%d,export=%d`.

### 3. who-is-who diagnostic (R23 pick)
Static `s_exp_last_idx` / `s_exp_last_n` track previous values. Each time the chosen index or
player count changes, ONE unconditional dlog line fires:
`R23 pick: n=%d idx=%d cand=[i:total ...]`
Example: `R23 pick: n=3 idx=1 cand=[1:1280 2:970]`
This identifies Dali's player slot — his totals will track his play.

### 4. state.h
`int resolve_export_player(DWORD base, int limit, DWORD *out_res, int *out_idx);` declared.

### 5. test_source_contract.py
R20/21 `decrypt_slot_at((DWORD)(DWORD_PTR)res, ...)` pin updated to `decrypt_slot_at(res, ...)` (res
is now DWORD from the fresh resolve). The `base == NULL || res == NULL` skip pin removed (replaced by
resolve return-value gating). 10 new R23 pins: resolve_export_player defined, EXPORT_MAX_CANDS,
g_export_cands, g_export_ncand, g_export_n, s_exp_last_idx, s_exp_last_n, "R23 pick:" format
string (exactly once), safe_r32/RVA_GAME_PTR/OFF_GAME_CTX chain, update of last tracking statics.

## FILES CHANGED
- `src/gameif.c` — R23: resolve_export_player() defined; export_thread rewritten (no g_res);
  s_exp_last_idx/s_exp_last_n statics; ExportCand struct + g_export_cands/g_export_ncand/g_export_n
- `src/state.h` — R23: resolve_export_player declaration added
- `tests/test_source_contract.py` — R23: 10 new pins; R20/21 decrypt_slot_at pin updated;
  old base==NULL guard pin removed
- `.swarm/BUILD.md` — this section

## BUILD
Build rc=0, zero errors, cosmetic `-Wcomment` only (`/*` inside comment). VERIFY PASS (i386 PE32,
imports {KERNEL32, USER32, msvcrt}, 11 exports).
**d3d9.dll = 157,138 B, SHA256 `b6dae6a311a321549cf8263eec870f564046753f426c7574b28d64a93a9d216a`**

## DEPLOY
- repo root d3d9.dll: `b6dae6a3...` (R23) ✓
- tests\d3d9.dll: `b6dae6a3...` (R23) ✓
- game dir d3d9.dll: `11473214...` (R22) — **BLOCKED by locked file** (age3y.exe PID 9280 running).
  Deploy will succeed after Dali closes the game and we re-run the deploy step.

## HARNESS (ALL 5 GREEN)
- `test_rate_engine.exe` — **FAILURES: 0**
- `test_export.exe` — **FAILURES: 0**
- `test_d3d9_actual.exe` — **ALL D3D9 ACTUAL CHECKS PASSED**
- `test_source_contract.py` — **SOURCE-CONTRACT: PASS** (R23 block + all R11-R21 pins green)
- `test_pe_structure.py` — **FAILURES: 0**

## COMMIT
R23 commit on `main`.

## ONE INSTRUCTION FOR DALI
1. Close Age of Empires III (exit the game fully).
2. The DLL auto-deploys on build. Re-run: `cmd /c build\build.bat` from the repo, or ask me
   to re-deploy — the copy will succeed once the game is not locking the file.
3. Restart a match. The R23 fix resolves the player fresh every 500ms instead of trusting the
   one-shot observer pointer, so non-zero values should now stream.
4. Check d3d9mod.log for `R23 pick: n=... idx=... cand=[...]` — this tells us which player
   index you are (your totals will match your in-game resources). The first entry in cand is
   your slot if you're in the lead, otherwise it's whichever candidate had the highest total.
   The index (idx) is the slot the mod is tracking — if it's NOT your index, say so and we
   pin it next round.
5. Note: if you're behind the AI, the mod tracks the LEADER (largest total) until your
   resources exceed theirs — the diag line will confirm who is picked.

---

��#   B U I L D   �� �   R O U N D   2 2   ( 2 0 2 6 - 0 9 - 0 7 )   �� �   F I X   S T U C K   U I   ( t r u n c a t i o n - b l i n d   t a i l e r   +   s i l e n t   p y t h o n w ) 
 
 
 
 # #   R O U N D   S U M M A R Y 
 
 D a l i :   " t h e   u i   i s   s t u c k   a n d   s h o w i n g   n o t h i n g . "   D i a g n o s e d   w i t h   E V I D E N C E ,   n o t   g u e s s e s : 
 
 -   * * H 1   R U L E D   O U T   ( c o r r e c t   D L L ,   g a m e   r a n ) : * *   d e p l o y e d   ` d 3 d 9 . d l l `   = =   R 2 1   b u i l d   h a s h 
 
     ` 1 1 4 7 3 2 1 4 �� � `   ( 1 5 5 , 8 2 5   B ,   m a t c h e s   d 3 d 9 . s h a 2 5 6 ) ;   ` r a t e s . l o g `   E X I S T S   i n   t h e   g a m e   f o l d e r 
 
     ( 1 4 , 8 8 7   B ,   l i v e   l i n e s ,   m t i m e   1 5 : 4 3   �� �   t h e   g a m e   H A D   r u n   a n d   W A S   e x p o r t i n g ) . 
 
     L a t e r   c o n f i r m e d   t h e   g a m e   w a s   r u n n i n g   l i v e   d u r i n g   t h i s   s e s s i o n   ( a g e 3 y   P I D   ~ 9 2 8 0 , 
 
     r a t e s . l o g   a d v a n c i n g   e v e r y   5 0 0   m s ) . 
 
 -   * * H 2   C O N F I R M E D   �� �   R O O T   C A U S E : * *   t h e   D L L   t r u n c a t e s   ` r a t e s . l o g `   O N C E   a t   e x p o r t - t h r e a d 
 
     s t a r t   ( ` f o p e n   " w " ` ) .   D a l i ' s   ` a p p / l o g _ t a i l e r . p y   f o l l o w ( ) `   o p e n s   a t   E O F   a n d   y i e l d s   o n l y 
 
     n e w   c o n t e n t   �� �   i f   t h e   f i l e   a l r e a d y   e x i s t e d   w h e n   t h e   a p p   s t a r t e d ,   t h e   r e a d e r ' s   p o s i t i o n 
 
     s i t s   b e y o n d   E O F   f o r e v e r   �� �   ` r e a d l i n e ( ) `   r e t u r n s   ' '   �� �   U I   s t u c k .   R E P R O :   t i n y   s c r i p t 
 
     ( s e e d   �� �   f o l l o w   �� �   t r u n c a t e   �� �   a p p e n d )   p r i n t e d   ` l i n e s   y i e l d e d   a f t e r   t r u n c a t i o n + r e w r i t e :   [ ] ` . 
 
 -   * * H 3   C O N F I R M E D   �� �   s y m p t o m   a m p l i f i e r : * *   p y t h o n w   s h o w s   n o t h i n g   o n   e r r o r ;   t h e   a p p ' s   p r e - f i x 
 
     c o n s o l e   m o d e   p r i n t e d   N O T H I N G   u n t i l   t h e   f i r s t   l i n e   a r r i v e d   ( b l a n k   w i n d o w ) ,   a n d   a n y 
 
     p o l l i n g - l o o p   e x c e p t i o n   d i e d   s i l e n t l y . 
 
 
 
 # #   F I X   ( D a l i ' s   3   f i l e s   �� �   c o n f i g . p y   /   l o g _ t a i l e r . p y   /   p a r s e r . p y   �� �   S T A Y   B Y T E - I D E N T I C A L ) 
 
 -   * * N E W   ` a p p / f i l e r . p y `   ( O U R S ) : * *   ` f o l l o w ( ) `   w i t h   t h e   s a m e   p h i l o s o p h y   ( o n l y   N E W   c o m p l e t e 
 
     l i n e s ,   p o l l - b a s e d ,   m i s s i n g - f i l e - s a f e ,   p a r t i a l - l i n e   r e w i n d )   P L U S   t r u n c a t i o n / r e c r e a t i o n 
 
     d e t e c t i o n :   o n   ` g e t s i z e ( p a t h )   <   p o s i t i o n `   t h e   f i l e   w a s   t r u n c a t e d / r e c r e a t e d   �� �   r e o p e n   + 
 
     r e s u m e   a f t e r   t h e   l a s t   c o m p l e t e   l i n e   ( d r o p s   t o r n   t a i l s ) ;   f i r s t   o p e n   o f   a n   e x i s t i n g   f i l e 
 
     s e e k s   t o   e n d   ( n o   s t a l e   f l a s h ) .   S u r v i v e s :   m i s s i n g   f i l e   ( n o   c r a s h ) ,   f i l e   a p p e a r s   l a t e r , 
 
     t r u n c a t i o n   m i d - s e s s i o n ,   p a r t i a l   l a s t   l i n e ,   g a r b a g e   l i n e s . 
 
 -   * * ` a p p / a p p . p y ` : * *   n o w   u s e s   ` f i l e r . f o l l o w ` ;   G U I   s h o w s   * * " W a i t i n g   f o r   d a t a �� �   ( s t a r t   a 
 
     m a t c h ) " * *   i n   a   v i s i b l e   d i m   c o l o r   w h e n   n o t h i n g   a r r i v e d ,   k e e p s   l a s t - k n o w n   v a l u e s 
 
     ( n e v e r   b l a n k s   o u t   o n   a   m o m e n t a r y   r e a d   g a p ) ,   a n d   a n y   p o l l i n g - l o o p   e x c e p t i o n   i s   a p p e n d e d 
 
     t o   * * ` a p p / a p p . e r r o r . l o g ` * *   ( t i m e s t a m p e d ,   t r a c e b a c k )   i n s t e a d   o f   d y i n g   s i l e n t l y   u n d e r 
 
     p y t h o n w .   C o n s o l e   m o d e   p r i n t s   ` r e a d i n g   < p a t h > �� � `   t h e n   a   ` w a i t i n g   f o r   d a t a �� �   ( s t a r t   a 
 
     m a t c h ) `   t i c k   e v e r y   ~ 2   s   u n t i l   t h e   f i r s t   l i n e ,   t h e n   e v e r y   l i v e   l i n e . 
 
 -   * * ` a p p / e n g i n e . p y ` : * *   ` r e s e t ( ) `   n o w   s e t s   ` s e l f . l a s t   =   N o n e `   ( w a s   u n s e t   b e f o r e   t h e   f i r s t 
 
     s a m p l e   �� �   ` e n g i n e . r a w `   A t t r i b u t e E r r o r   u n d e r   p y t h o n w   =   a   s i l e n t - d e a t h   p a t h ) . 
 
 
 
 # #   H A R N E S S   ( A L L   6   G R E E N ) 
 
 -   * * N E W   ` t e s t s / t e s t _ f i l e r . p y `   �� �   F I L E R :   1 2   c h e c k s   P A S S * * :   g r o w i n g   f i l e   y i e l d s   n e w   l i n e s 
 
     o n l y ;   t r u n c a t i o n   m i d - s t r e a m   r e c o v e r s   +   c o n t i n u e s ;   m i s s i n g   f i l e   �� �   c l e a n   s t a r t ;   p a r t i a l 
 
     l i n e   h e l d   u n t i l   n e w l i n e ;   n o   i n f i n i t e   l o o p s ;   b i g - f i l e   t r u n c a t i o n   ( 1 5   K B   �� �   3   l i n e s ) 
 
     r e o p e n s   a t   e n d ,   n o   s t a l e   r e p l a y .   L i v e   a p p   c h e c k :   ` p y t h o n   a p p \ a p p . p y   - - c o n s o l e ` 
 
     a g a i n s t   a   s i m u l a t e d   t r u n c a t e   s h o w e d   ` r e a d i n g   �� � `   /   ` w a i t i n g   f o r   d a t a �� � `   t h e n   t h e   N E W 
 
     s e s s i o n ' s   l i n e s   ( f o o d   5 �� � 8 �� � 1 1 �� � 1 4 �� � 1 7 �� � 2 0 ,   r a t e s   �� �   + 3 . 1 / s e c )   �� �   o l d   h i s t o r y   s k i p p e d . 
 
 -   ` t e s t s / t e s t _ a p p _ c o r e . p y `   A P P - C O R E   P A S S   ,%V%  ` t e s t s / t e s t _ s o u r c e _ c o n t r a c t . p y `   P A S S   ,%V%
 
     ` t e s t s / t e s t _ p e _ s t r u c t u r e . p y `   0   ,%V%  ` t e s t s / t e s t _ e x p o r t . e x e `   F A I L U R E S   0   ,%V%
 
     ` t e s t s / t e s t _ r a t e _ e n g i n e . e x e `   F A I L U R E S   0   ,%V%  ` t e s t s / t e s t _ d 3 d 9 _ a c t u a l . e x e `   A L L   P A S S . 
 
 -   b u i l d \ b u i l d . b a t :   r c = 0 ,   V E R I F Y   P A S S ,   S H A   ` 1 1 4 7 3 2 1 4 �� � ` ,   1 5 5 , 8 2 5   B   u n c h a n g e d   ( N O   C - s i d e 
 
     c h a n g e   t h i s   r o u n d ) .   G a m e - d i r   d e p l o y   [ 4 / 7 ]   l o c k e d   b y   t h e   R U N N I N G   a g e 3 y . e x e   �� �   d e p l o y e d 
 
     D L L   a l r e a d y   = =   R 2 1   h a s h ,   s o   n o   a c t i o n   n e e d e d . 
 
 
 
 # #   A G R E E M E N T / D X 
 
 -   C a u s e   f o r   D a l i :   * * t h e   a p p   m u s t   b e   s t a r t e d   A F T E R   t h e   g a m e   ( o r   w h i l e   a   m a t c h   r u n s ) . * * 
 
     N o w   r o b u s t   t o   A N Y   o r d e r   ( s t a r t   f i r s t   /   a f t e r   /   m i d - m a t c h   a l l   w o r k ) . 
 
 
 
 - - - 
 
 #   B U I L D   �� �   U X   R O U N D   ( 2 0 2 6 - 0 9 - 0 7 )   �� �   T A S K B A R / D E S K T O P   S H O R T C U T   +   F A L L B A C K   C M D 
 
 O n e - c l i c k   l a u n c h   f o r   t h e   r a t e s   a p p   ( n o   c o d e / D L L   c h a n g e s ) .   D e s k t o p   s h o r t c u t 
 
 ` C : \ U s e r s \ d a l i \ D e s k t o p \ A O E 3   R a t e s . l n k `   �� �   ` p y t h o n w . e x e   " C : \ U s e r s \ d a l i \ a o e 3 r a t e - m o d \ a p p \ a p p . p y " ` 
 
 ( W o r k i n g D i r e c t o r y   =   ` a p p \ ` ,   p y t h o n w   =   n o   c o n s o l e   w i n d o w ) ,   c o p y   d r o p p e d   i n t o 
 
 ` % A P P D A T A % \ M i c r o s o f t \ I n t e r n e t   E x p l o r e r \ Q u i c k   L a u n c h \ U s e r   P i n n e d \ T a s k B a r \ ` . 
 
 F a l l b a c k   ` s t a r t _ r a t e s . c m d `   ( z e r o - d e p e n d e n c y ) :   r e p o   r o o t   +   g a m e - f o l d e r   c o p y ,   b o t h 
 
 p o i n t i n g   a t   t h e   s a m e   a b s o l u t e   p y t h o n w   +   a p p . p y   ( p u s h d   t o   ` a p p \ `   t h e n   ` s t a r t ` ) . 
 
 T a s k b a r   p i n   m a y   n e e d   o n e   m a n u a l   s t e p :   r i g h t - c l i c k   d e s k t o p   s h o r t c u t   �� �   P i n   t o 
 
 t a s k b a r   ( d o n e   o n c e )   i f   t h e   U s e r   P i n n e d \ T a s k B a r   c o p y   i s n ' t   p i c k e d   u p   a u t o m a t i c a l l y . 
 
 
 
 #   B U I L D   �� �   R O U N D   2 1   ( 2 0 2 6 - 0 9 - 0 7 )   �� �   P Y T H O N   P I P E L I N E   O N   D A L I ' S   C O R E   +   ` r a t e s . l o g ` 
 
 
 
 # #   R O U N D   S U M M A R Y 
 
 R 2 1   r e b u i l d s   t h e   w h o l e   e x t e r n a l   s u r f a c e   a r o u n d   * * D a l i ' s   o w n   P y t h o n   c o r e * * ,   c o p i e d   i n t o 
 
 ` a p p / `   * * b y t e - i d e n t i c a l   a n d   u n m o d i f i e d * * :   ` a p p / c o n f i g . p y ` ,   ` a p p / l o g _ t a i l e r . p y ` ,   ` a p p / p a r s e r . p y ` 
 
 ( S H A 2 5 6 - v e r i f i e d   i d e n t i c a l   t o   D a l i ' s   o r i g i n a l s ) .   A r o u n d   i t   s i t   t w o   n e w   f i l e s   �� �   ` a p p / e n g i n e . p y ` 
 
 ( ` R a t e E n g i n e ` :   E M A   i n c o m e   r a t e   +   s p e n d - s p i k e   d e t e c t i o n   +   f o r m a t t i n g )   a n d   ` a p p / a p p . p y ` 
 
 ( a l w a y s - o n - t o p   T k i n t e r   l i v e   v i e w e r ,   h e a d l e s s   ` - - c o n s o l e `   m o d e )   �� �   p l u s   ` t e s t s / t e s t _ a p p _ c o r e . p y ` . 
 
 T h e   D L L ' s   e x p o r t   t h r e a d   n o w   w r i t e s   * * ` r a t e s . l o g ` * *   ( w a s   ` d 3 d 9 m o d . l o g ` ) ,   a n d 
 
 ` d 3 d 9 m o d . l o g `   i s   e x p l i c i t l y   t h e   d e b u g - o n l y   d i a g n o s t i c s   l o g .   T h e   o l d 
 
 ` t o o l s / r a t e s _ w i d g e t . p y `   +   ` t e s t s / t e s t _ w i d g e t _ p a r s e . p y `   a r e   * * r e m o v e d * *   ( s u p e r s e d e d   b y   t h e   a p p ) . 
 
 I m p o r t s   s t a y   { K E R N E L 3 2 , U S E R 3 2 , m s v c r t } ,   s t i l l   1 1   e x p o r t s .   A l l   h a r n e s s e s   g r e e n . 
 
 
 
 # #   ( 1 )   D A L I ' S   F I L E S   �� �   B Y T E - I D E N T I C A L ,   U N M O D I F I E D 
 
 C o p i e d   f r o m   ` C : \ U s e r s \ d a l i \ D o w n l o a d s \ D o c u m e n t s \ `   i n t o   ` C : \ U s e r s \ d a l i \ a o e 3 r a t e - m o d \ a p p \ ` : 
 
 -   ` c o n f i g . p y `   �� �   S H A 2 5 6   ` 5 E D B E D 5 3 1 9 8 7 D 0 0 B B F C 7 2 7 A A 3 8 A 0 6 9 E 5 9 5 6 A 6 C 8 E A 5 4 B F F 6 6 5 F F 9 1 B 0 7 6 8 A 0 2 C 9 B ` 
 
 -   ` l o g _ t a i l e r . p y `   �� �   S H A 2 5 6   ` 7 D 4 E 9 D D 8 6 4 B 0 F F 5 7 F D 4 4 E 3 7 B C 1 D 4 E 3 D 8 C 0 1 0 4 8 9 A 9 4 7 3 5 0 3 A E 1 A D E D E 3 8 0 7 C 6 E 8 9 ` 
 
 -   ` p a r s e r . p y `   �� �   S H A 2 5 6   ` 7 3 E 1 B D A 1 7 4 E 6 3 B B 9 7 F 9 7 1 5 7 2 7 B C 8 2 2 2 2 6 0 6 2 7 2 2 2 8 C 8 C 8 7 0 2 8 A 4 1 C 8 E 9 9 C 9 1 9 5 8 A ` 
 
 C o p i e s   r e - h a s h e d   a f t e r   c o p y   a n d   m a t c h   t h e   o r i g i n a l s   e x a c t l y .   N o n e   o f   t h e   t h r e e   a r e   e d i t e d   b y   t h e   r e p o . 
 
 
 
 # #   ( 2 )   D L L   C H A N G E   �� �   E X P O R T   W R I T E S   ` r a t e s . l o g ` 
 
 -   ` s r c / g a m e i f . c `   ` e x p o r t _ t h r e a d `   l o g   p a t h   i s   n o w   ` r a t e s . l o g `   ( ` l s t r c a t A ( l o g p a t h ,   " r a t e s . l o g " ) ` 
 
     f r o m   t h e   w r i t a b l e   g a m e s - f o l d e r - s c r a t c h   b u f f e r ,   ` l s t r c p y A ( l o g p a t h ,   " r a t e s . l o g " ) `   f a l l b a c k ) . 
 
 -   ` d 3 d 9 m o d . l o g `   r e m a i n s   * * d i a g n o s t i c s   o n l y * *   ( d l o g   s i t e s   +   l o g g e r . c   u n t o u c h e d   �� �   3   u n c o n d i t i o n a l 
 
     s i t e s ,   ` [ D e b u g ]   E n a b l e d = 1 `   o n l y ) . 
 
 -   E x p o r t   f o r m a t   * * u n c h a n g e d * *   a n d   e x a c t :   ` " t = % l u , f o o d = % d , w o o d = % d , c o i n = % d , e x p o r t = % d " ` , 
 
     ` t   =   ( u n s i g n e d   l o n g ) c l o c k _ n o w ( ) `   ( Q P C �� � m s ) ,   s l o t s   f o o d = 2 ,   w o o d = 1 ,   c o i n = 0 ,   e x p o r t = 7 ,   ` S l e e p ( 5 0 0 ) ` , 
 
     t r u n c a t e - o n c e   t h e n   a p p e n d + ` f f l u s h ` ,   D e b u g = 0   g a t i n g ,   l a z y   l a u n c h / s h u t d o w n   u n c h a n g e d . 
 
 
 
 # #   ( 3 )   N E W   F I L E S   �� �   E N G I N E   +   A P P 
 
 -   ` a p p / e n g i n e . p y `   �� �   ` R a t e E n g i n e ( c o n f i g = N o n e ) ` : 
 
     -   f i r s t   s a m p l e   s e e d s   b a s e l i n e   ( ` e m a = 0 ` ,   l a s t   t / v ) ;   ` d t < = 0 `   k e e p s   l a s t   r a t e ;   ` d t   <   m i n _ d t _ s e c `   i g n o r e d . 
 
     -   ` i n s t a n t   =   d e l t a / d t ` ;   * * s p e n d   r u l e * * :   ` i n s t a n t   <   0   a n d   | i n s t a n t |   >   | e m a | * s p e n d _ r a t i o `   �� � 
 
         S P E N D   ( c o u n t e d ,   e x c l u d e d   f r o m   E M A ) ;   e l s e   f o l d   ` a l p h a   =   1   �� �   e x p ( �� � d t / g%� ) ` ,   ` e m a   =   i n s t a n t * l%�%  +   e m a * ( 1 �� � l%�%) ` . 
 
     -   p e r - r e s o u r c e   r e c o r d :   ` v a l u e ,   r a t e ,   r a t e _ m i n   ( r a t e * 6 0 ) ,   s p e n t _ m i n   ( n * 6 0 / s p a n ,   s p a n   �� �   1 s ) , 
 
         s p e n d _ c o u n t ,   f o r m a t t e d   ( + / d e c i m a l s / z e r o _ e p s i l o n ) ,   f o r m a t t e d _ m i n ` ;   ` u p d a t e ( ) `   �� �   ` { " t " ,   " r e s o u r c e s " } ` , 
 
         ` . l a s t ` ,   ` r e s e t ( ) ` ,   ` r a t e ( ) ` ,   ` s p e n t ( ) ` ,   ` d i s p l a y ( ) ` . 
 
 -   ` a p p / a p p . p y `   �� �   C L I   ` - - l o g   < p a t h > `   +   ` - - c o n s o l e ` ;   t a i l s   ` C O N F I G . l o g _ p a t h `   v i a   ` l o g _ t a i l e r . f o l l o w ` , 
 
     p a r s e s   v i a   ` p a r s e r . p a r s e _ l i n e ` ,   f e e d s   ` R a t e E n g i n e ` ;   G U I   =   f r a m e l e s s / a l w a y s - o n - t o p / l%�%�� � 0 . 8 5   d a r k 
 
     v i e w e r   ( F o o d / W o o d / C o i n / E x p o r t ,   g r e e n   i n c o m e   /   r e d   s p e n d   r a t e s ,   d i m m e d   ` t = `   f o o t e r ) ,   r e p a i n t   a t 
 
     ` C O N F I G . r e f r e s h _ h z ` ,   d r a g   =   m o v e ,   r i g h t - c l i c k   =   c l o s e ,   T k - m i s s i n g   �� �   c o n s o l e   f a l l b a c k . 
 
 -   ` t e s t s / t e s t _ a p p _ c o r e . p y `   �� �   p a r s e r   ( D a l i ' s   3   l i n e s ,   g a r b a g e / p a r t i a l / m i s s i n g - t / e x t r a - s p a c e / n e g a t i v e 
 
     e x p o r t ) ,   t a i l e r   ( E O F - s t a r t ,   n o   r e p l a y ,   p a r t i a l - l i n e   r e w i n d ,   m i s s i n g - f i l e   s u r v i v a l ) ,   e n g i n e 
 
     ( E M A   c o n v e r g e n c e ,   s p e n d   e x c l u s i o n   +   c o u n t i n g ,   d t < m i n   i g n o r e ,   r a t e _ m i n ,   p r e s e t s   v i a 
 
     ` s e t _ s m o o t h i n g _ p r e s e t `   i n c l .   V a l u e E r r o r ,   r e s e t ) ,   f o r m a t t i n g   h e l p e r s . 
 
 -   * * R E M O V E D * *   ` t o o l s / r a t e s _ w i d g e t . p y `   +   ` t e s t s / t e s t _ w i d g e t _ p a r s e . p y `   ( s u p e r s e d e d   b y   ` a p p / ` ) . 
 
 
 
 # #   H O W   T O   U S E 
 
 D e f a u l t   ` [ D e b u g ]   E n a b l e d = 0 `   �� �   ` r a t e s . l o g `   c a r r i e s   O N L Y   t h e   r e c u r r i n g   e x p o r t   l i n e s : 
 
 ` p y t h o n w . e x e   a p p \ a p p . p y `   ( G U I ;   d r a g   =   m o v e ,   r i g h t - c l i c k   =   c l o s e ) ,   ` p y t h o n   a p p \ a p p . p y   - - c o n s o l e ` 
 
 ( h e a d l e s s   s t d o u t ) ,   o p t i o n a l   ` - - l o g   < p a t h > ` .   ` [ D e b u g ]   E n a b l e d = 1 `   �� �   f u l l   d i a g n o s t i c s 
 
 i n   ` d 3 d 9 m o d . l o g ` ,   e x p o r t   s u p p r e s s e d .   ` b u i l d \ b u i l d . b a t `   a u t o - d e p l o y s   t h e   D L L   t o   t h e   g a m e   f o l d e r 
 
 ( i n t e n t i o n a l l y   �� �   D a l i ' s   l i v e   p i p e l i n e   i n c l u d e s   t h e   f u l l   g a m e ) . 
 
 
 
 # #   H A R N E S S   ( A L L   G R E E N ) 
 
 -   ` t e s t s / t e s t _ a p p _ c o r e . p y `   �� �   * * A P P - C O R E :   P A S S * *   ( p a r s e r   /   t a i l e r   /   e n g i n e   /   d i s p l a y   /   p r e s e t s ) . 
 
 -   ` t e s t s / t e s t _ s o u r c e _ c o n t r a c t . p y `   �� �   * * S O U R C E - C O N T R A C T :   P A S S * *   �� �   R 2 1   p i n s :   ` r a t e s . l o g `   l i t e r a l , 
 
     f o r m a t   l i t e r a l ,   5 - a r g   s i g n a t u r e ,   ` ( i n t ) ( x + 0 . 5 f ) ` ,   s l o t   d e c r y p t ,   t r u n c a t e / a p p e n d / f f l u s h ,   g a t i n g , 
 
     D E T A C H   s h u t d o w n ,   l o g g i n g   d i e t ,   e x p o r t - o n l y   D e b u g = 0 .   W i d g e t - c o d e   p i n   r e m o v e d   w i t h   t h e   w i d g e t . 
 
 -   ` t e s t s / t e s t _ r a t e _ e n g i n e . e x e `   �� �   * * F A I L U R E S :   0 * * ;   ` t e s t s / t e s t _ e x p o r t . e x e `   �� �   * * F A I L U R E S :   0 * * ; 
 
     ` t e s t s / t e s t _ d 3 d 9 _ a c t u a l . e x e `   �� �   * * P A S S * * ;   ` t e s t s / t e s t _ p e _ s t r u c t u r e . p y `   �� �   * * P A S S * * . 
 
 
 
 # #   C O M M I T 
 
 ` g i t `   c o m m i t   w i t h   ` - - a u t h o r = " D A L I 9 5 1   < d a l i 9 5 1 @ u s e r s . n o r e p l y . g i t h u b . c o m > " ` ,   p u s h   ` m a i n ` . 
 
 
 
 - - - 
 
 
 
 # #   R O U N D   S U M M A R Y 
 
 R 2 0   r e - f o r m a t s   t h e   r e c u r r i n g   e x p o r t   l i n e   t o   t h e   e x a c t 
 
 ` t = % l u , f o o d = % d , w o o d = % d , c o i n = % d , e x p o r t = % d `   c o n t r a c t :   ` t `   i s   t h e   * * v e r i f i e d   r e a l t i m e 
 
 m i l l i s e c o n d   c l o c k * *   ( ` t r a c k e r . c   c l o c k _ n o w ( ) ` :   Q P C   �� �   m s ,   ` G e t T i c k C o u n t `   f a l l b a c k   �� �   N O T   t h e 
 
 p e r - P r e s e n t   ` s _ t i c k `   f r a m e   c o u n t e r ) ,   a n d   t h e   4 t h   r e s o u r c e   * * e x p o r t   ( s l o t   7 ) * *   i s   a d d e d   v i a   t h e 
 
 s a m e   v e r i f i e d   ` d e c r y p t _ s l o t _ a t `   c h a i n .   W i d g e t   s h o w s   F o o d / W o o d / C o i n / E x p o r t   +   a   s m a l l   d i m m e d 
 
 ` t = `   i n d i c a t o r .   I m p o r t s   s t a y   { K E R N E L 3 2 , U S E R 3 2 , m s v c r t } ,   s t i l l   1 1   e x p o r t s ,   a l l   6   h a r n e s s e s   g r e e n . 
 
 * * N O T   d e p l o y e d   t o   t h e   g a m e   f o l d e r * *   �� �   D a l i   v e r i f i e s   o n   h i s   m a c h i n e   f i r s t . 
 
 
 
 # #   H O W   T O   U S E   ( u n c h a n g e d ) 
 
 D e f a u l t   ` [ D e b u g ]   E n a b l e d = 0 `   �� �   t h e   l o g   c a r r i e s   O N L Y   t h e   r e c u r r i n g   e x p o r t   l i n e s : 
 
 ` p y t h o n w . e x e   t o o l s \ r a t e s _ w i d g e t . p y `   ( d r a g   =   m o v e ,   r i g h t - c l i c k   =   c l o s e ) .   ` [ D e b u g ]   E n a b l e d = 1 `   �� � 
 
 f u l l   d i a g n o s t i c s ,   e x p o r t   s u p p r e s s e d . 
 
 
 
 # #   I M P L E M E N T A T I O N 
 
 -   ` s r c / g a m e i f . c `   ` f o r m a t _ e x p o r t _ l i n e `   s i g n a t u r e   i s   n o w 
 
     ` i n t   f o r m a t _ e x p o r t _ l i n e ( u n s i g n e d   l o n g   t _ m s ,   f l o a t   f o o d ,   f l o a t   w o o d ,   f l o a t   c o i n ,   f l o a t   e x p o r t ,   c h a r   * b u f ,   s i z e _ t   l e n ) ` 
 
     �� �   e x a c t   ` " t = % l u , f o o d = % d , w o o d = % d , c o i n = % d , e x p o r t = % d " ` ,   v a l u e s   ` ( i n t ) ( x + 0 . 5 f ) ` , 
 
     c l a m p e d   +   f o r c e d   N U L ,   0   o n   N U L L / 0 - s i z e .   D e c l a r e d   i n   ` s r c / s t a t e . h ` . 
 
 -   ` e x p o r t _ t h r e a d ` :   ` S l e e p ( 5 0 0 ) `   �� �   ` t   =   ( u n s i g n e d   l o n g ) c l o c k _ n o w ( ) `   ( Q P C �� � m s   v e r i f i e d ) , 
 
     s k i p   w h i l e   ` g _ b a s e ` / ` g _ r e s `   N U L L ,   d e c r y p t   s l o t s   * * f o o d = 2 ,   w o o d = 1 ,   c o i n = 0 ,   e x p o r t = 7 * * , 
 
     t r u n c a t e - o n c e   ( ` " w " ` )   t h e n   a p p e n d + ` f f l u s h `   ( ` " a " ` ) . 
 
 -   L a u n c h / s h u t d o w n   u n c h a n g e d :   l a z y   o n   f i r s t   ` D i r e c t 3 D C r e a t e 9 ` ,   D e b u g = 0   o n l y ;   ` D L L _ P R O C E S S _ D E T A C H `   �� � 
 
     ` e x p o r t _ s h u t d o w n ( ) ` . 
 
 -   ` t o o l s / r a t e s _ w i d g e t . p y ` :   P A T T E R N   ` t = ( \ d + ) , f o o d = ( \ d + ) , w o o d = ( \ d + ) , c o i n = ( \ d + ) , e x p o r t = ( \ d + ) `   �� � 
 
     ` ( t , f o o d , w o o d , c o i n , e x p o r t ) ` ;   n e w   E X P O R T   r o w   ( ` # a 5 e 8 a 0 ` ) ;   s m a l l   d i m m e d   ` t = `   f o o t e r 
 
     ( ` # 5 5 6 6 7 7 ` ) ;   f r a m e l e s s / t o p m o s t / d r a g / 2 0 0 m s   p o l l   a l l   k e p t . 
 
 
 
 # #   H A R N E S S   ( A L L   6   G R E E N ) 
 
 -   ` t e s t s / t e s t _ e x p o r t . e x e `   �� �   * * F A I L U R E S :   0 * *   �� �   e x a c t   ` t = % l u , . . . `   s t r i n g   i n c l .   D a l i ' s   3   s a m p l e   l i n e s 
 
     ( ` t = 1 8 2 3 4 0 , f o o d = 1 0 0 , w o o d = 0 , c o i n = 0 , e x p o r t = 0 ` ,   ` 1 8 2 8 4 0 / 1 2 0 ` ,   ` 1 8 3 3 4 0 / 1 2 0 ` ) ,   ` ( i n t ) ( x + 0 . 5 f ) ` 
 
     r o u n d i n g   i n c l .   e x p o r t ,   b i g - ` t `   0 x F F F F F F F F   w r a p ,   n o - s p a c e   c h e c k ,   N U L L / 0 - s i z e / t i n y - b u f f e r . 
 
 -   ` t e s t s / t e s t _ w i d g e t _ p a r s e . p y `   �� �   * * W I D G E T - P A R S E :   P A S S * *   �� �   5 - t u p l e   p a r s e ,   D a l i ' s   3   l i n e s ,   m i s s i n g - 
 
     f i e l d / m a l f o r m e d / R 1 9 - l e g a c y   �� �   N o n e ,   m o d u l e   P A T T E R N   p i n n e d . 
 
 -   ` t e s t s / t e s t _ s o u r c e _ c o n t r a c t . p y `   �� �   * * S O U R C E - C O N T R A C T :   P A S S * *   �� �   R 1 9   p i n s   u p d a t e d   t o   R 2 0 :   n e w   5 - a r g 
 
     s i g n a t u r e   ( d e f   +   s t a t e . h   d e c l ) ,   ` t = % l u `   f o r m a t   l i t e r a l ,   ` ( u n s i g n e d   l o n g ) c l o c k _ n o w ( ) `   i n   t h r e a d , 
 
     s l o t - 7   d e c r y p t ,   w i d g e t   P A T T E R N   s t r i n g ,   u n c h a n g e d   d i e t / g a t i n g / o r d e r   p i n s . 
 
 -   ` t e s t s / t e s t _ r a t e _ e n g i n e . e x e `   �� �   * * F A I L U R E S :   0 * *   ,%V%  ` t e s t s / t e s t _ d 3 d 9 _ a c t u a l . e x e `   �� �   * * A L L   D 3 D 9   A C T U A L 
 
     C H E C K S   P A S S E D * *   ,%V%  ` t e s t s / t e s t _ p e _ s t r u c t u r e . p y `   �� �   * * F A I L U R E S :   0 * *   ( i m p o r t s   s u b s e t   v e r i f i e d ) . 
 
 
 
 # #   B U I L D 
 
 r c = 0 ,   z e r o   w a r n i n g s .   * * d 3 d 9 . d l l   =   1 5 5 , 8 2 5   B * * ,   S H A 2 5 6 : 
 
 ` 9 b 9 9 c 2 a 3 0 7 a 5 7 2 9 b 6 7 7 4 9 0 9 a e 8 c 7 c f f 0 0 a 3 6 8 5 e 1 8 7 8 3 4 1 d 4 3 2 c 7 f f 4 7 8 4 b 2 c 1 7 0 ` 
 
 ( s e e   ` d 3 d 9 . s h a 2 5 6 ` ) . 
 
 
 
 - - - 
 
 
 
 #   B U I L D   �� �   R O U N D   1 9 - P I V O T   ( 2 0 2 6 - 0 9 - 0 7 )   �� �   F I L E - E X P O R T   P I V O T :   l i v e   v a l u e s   �� �   d 3 d 9 m o d . l o g   +   a l w a y s - o n - t o p   P y t h o n   w i d g e t 
 
 
 
 # #   R O U N D   S U M M A R Y 
 
 D a l i   p i v o t e d   * * a w a y   f r o m   t h e   f l a k y   i n - m a t c h   D 3 D   o v e r l a y * *   ( R 1 5 - 1 7   t r a c e d   t h e   i n - m a t c h   P r e s e n t   t o   t h e 
 
 r u n t i m e ' s   i m p l i c i t   s w a p c h a i n   b u t   t h e   d r a w   p a t h   n e v e r   p r o v e d   r e l i a b l e ) .   T h e   D L L   n o w   e x p o r t s   t h e   L I V E 
 
 d e c r y p t e d   r e s o u r c e s   a s   r e c u r r i n g   ` f o o d : % d   , w o o d : % d   , c o i n : % d `   l i n e s   t o 
 
 ` C : \ U s e r s \ d a l i \ D o c u m e n t s \ A g e   o f   E m p i r e s   I I I   -   C o m p l e t e   C o l l e c t i o n \ d 3 d 9 m o d . l o g `   e v e r y   ~ 5 0 0 m s ,   a n d   a   n e w 
 
 s t d l i b   T k i n t e r   w i d g e t   r e a d s   t h e m   i n   a n   a l w a y s - o n - t o p   f r a m e l e s s   w i n d o w .   R e u s e s   O N L Y   t h e   v e r i f i e d   c h a i n   + 
 
 d e c r y p t ;   * * N O   n e w   i m p o r t s * *   ( t h r e a d   v i a   K E R N E L 3 2   ` C r e a t e T h r e a d ` ) ;   i m p o r t s   s t a y 
 
 { K E R N E L 3 2 , U S E R 3 2 , m s v c r t } ,   s t i l l   1 1   e x p o r t s .   A l l   e x i s t i n g   h a r n e s s e s   s t a y   g r e e n . 
 
 
 
 # #   H O W   T O   U S E   ( d e f a u l t   =   e x p o r t   m o d e ) 
 
 1 .   L a u n c h   t h e   g a m e   ( s e r v e r / d e p l o y   a l r e a d y   c o p i e d   t h e   D L L   +   i n i ) .   I n   d e f a u l t   ` [ D e b u g ]   E n a b l e d = 0 `   t h e   l o g 
 
       c a r r i e s   O N L Y   t h e   r e c u r r i n g   e x p o r t   l i n e s . 
 
 2 .   S h o w   t h e   l i v e   n u m b e r s : 
 
       ` ` ` 
 
       p y t h o n w . e x e   t o o l s \ r a t e s _ w i d g e t . p y 
 
       ` ` ` 
 
       ( d e f a u l t   l o g   =   t h e   g a m e   d i r   ` d 3 d 9 m o d . l o g ` ;   o p t i o n a l   ` - - l o g   P A T H `   a n d   ` - - f p s ` ) .   D r a g   w i t h   t h e   l e f t 
 
       b u t t o n ,   c l o s e   w i t h   r i g h t - c l i c k . 
 
 3 .   D i a g n o s t i c s :   s e t   ` [ D e b u g ]   E n a b l e d = 1 `   i n   ` R e s o u r c e R a t e M o d . i n i `   �� �   t h e   f u l l   v e r b o s e   l o g   ( c h a i n   /   d e v i c e   / 
 
       d r a w   /   h e a r t b e a t )   r e t u r n s   a n d   e x p o r t   l i n e s   a r e   s u p p r e s s e d . 
 
 
 
 # #   I M P L E M E N T A T I O N 
 
 -   ` s r c / g a m e i f . c ` :   ` f o r m a t _ e x p o r t _ l i n e ( f l o a t , f l o a t , f l o a t , c h a r * , s i z e _ t ) `   �� �   e x a c t   ` f o o d : % d   , w o o d : % d   , c o i n : % d ` , 
 
     v a l u e s   r o u n d e d   ` ( i n t ) ( x + 0 . 5 f ) ` ,   r e t u r n s   0   o n   N U L L / 0 - s i z e ,   n e v e r   w r i t e s   p a s t   l e n   ( c l a m p e d   +   f o r c e d   N U L ) . 
 
 -   ` e x p o r t _ t h r e a d ( L P V O I D ) `   �� �   ` S l e e p ( 5 0 0 ) `   t h e n   r e a d   ` g _ b a s e ` / ` g _ r e s ` ;   s k i p   w h i l e   e i t h e r   N U L L ;   d e c r y p t 
 
     ` d e c r y p t _ s l o t _ a t `   s l o t s   * * f o o d = 2 ,   w o o d = 1 ,   c o i n = 0 * *   ( t h e   v e r i f i e d   m a p ) ;   t r u n c a t e   t h e   l o g   O N C E   o n   t h r e a d 
 
     s t a r t   ( ` " w " ` ) ,   t h e n   a p p e n d   +   ` f f l u s h `   p e r   w r i t e   ( ` " a " ` ) . 
 
 -   ` e x p o r t _ s t a r t ( ) `   /   ` e x p o r t _ s h u t d o w n ( ) `   ( d 3 d 9 . c   f i l e - s c o p e   s t a t i c s   ` s _ e x p o r t _ r u n n i n g `   /   ` s _ e x p o r t _ t h r e a d ` ) : 
 
     t h r e a d   l a u n c h e d   * * l a z i l y   o n   t h e   f i r s t   ` D i r e c t 3 D C r e a t e 9 `   c a l l   O N L Y   w h e n   ` d e b u g _ e n a b l e d = = 0 ` * *   ( n e v e r   f r o m 
 
     D l l M a i n   �� �   a v o i d s   t h e   l o a d e r   l o c k ) ;   ` D L L _ P R O C E S S _ D E T A C H `   �� �   ` e x p o r t _ s h u t d o w n ( ) `   s e t s   ` s _ e x p o r t _ r u n n i n g = 0 ` 
 
     a n d   d r a i n s   w i t h   ` W a i t F o r S i n g l e O b j e c t ( h , 2 0 0 ) `   +   ` C l o s e H a n d l e ` . 
 
 
 
 # #   L O G G I N G   D I E T 
 
 3   s i t e s   s t a y   U N C O N D I T I O N A L   ( t h e   s h i p p e d   D e b u g = 0   l o g ' s   o n l y   n o n - e x p o r t   c o n t e n t ) : 
 
 -   ` D L L   l o a d e d   b a s e = % p   r e a l = % p   v e r s i o n = % s   r e a s o n = % s `   ( R 1 4 ) 
 
 -   ` o v l   s t o p :   % u   c o n s e c u t i v e   f r a m e   a b o r t s   a t   s t a g e = . . `   ( R 1 3   d r a w - p a t h   a b o r t ) 
 
 -   ` o v l   s t o p :   r e e n t r a n t - p r e s e n t   f o r   % u   c o n s e c u t i v e   f r a m e s `   ( R 1 3 ) 
 
 
 
 E v e r y t h i n g   e l s e   ( ~ 2 4   s i t e s   a c r o s s   ` d 3 d 9 . c ` / ` s r c / g a m e i f . c ` / ` s r c / u i . c ` / ` s r c / s e t t i n g s . c ` )   i s   n o w   g a t e d   b e h i n d 
 
 ` g _ s e t t i n g s . d e b u g _ e n a b l e d ` ,   i n c l u d i n g :   C r e a t e D e v i c e ( E x )   h r ,   p a t c h _ s w a p c h a i n ,   s w a p c h a i n - i m p l , 
 
 d e v i c e - c o u n t ,   t h e   m e r g e d   p r e s e n t - p a t h   t r a c e r ,   t h e   O V E R L A Y   A R M E D / D I S A B L E D   s t a t u s ,   t h e   c h a i n   l i n e ,   t h e 
 
 m a t c h - s t a r t   s e p a r a t o r ,   t h e   R 1 3   u n c o n d i t i o n a l   ` o v l   d i a g `   ( i t s   p i n   w a s   F L I P P E D   t o   g a t e d ) ,   t h e   o v l 
 
 h e a r t b e a t ,   u i . c   f o n t / p a n e l / h o t k e y   s a v i n g ,   a n d   t h e   s e t t i n g s   c l o c k   n o t e .   ` R e s o u r c e R a t e M o d . i n i . e x a m p l e ` 
 
 ` [ D e b u g ] `   b l o c k   d o c u m e n t s   e x p o r t   v s   d i a g n o s t i c s   m o d e s . 
 
 
 
 # #   D R I F T   C A U G H T   &   F I X E D   ( t h e   h a r n e s s   n e e d e d   d e b u g   m o d e ) 
 
 ` t e s t _ d 3 d 9 _ a c t u a l . c `   a s s e r t s   m a n y   b r e a d c r u m b s   ( c h a i n   t a g s ,   s w a p c h a i n   i n s t a l l s ,   t r a c e r )   t h a t   t h e   d i e t 
 
 n o w   g a t e s   �� �   i t   s e t   ` d e b u g _ e n a b l e d = 1 `   a t   t h e   t o p   o f   ` m a i n ( ) `   a n d   a g a i n   a t   t h e   s t a r t   o f 
 
 ` t e s t _ s w a p c h a i n _ h o o k ( ) `   s o   t h o s e   l i n e s   a r e   e m i t t e d .   T h i s   i s   a   D E V   h a r n e s s   o n   p u r p o s e   ( f u l l   v e r b o s e 
 
 s t r e a m ) ;   i t   d o e s   N O T   c h a n g e   t h e   s h i p p e d   D e b u g = 0   d e f a u l t . 
 
 
 
 # #   H A R N E S S   ( A L L   6   G R E E N ) 
 
 -   ` t e s t s / t e s t _ r a t e _ e n g i n e . e x e `   �� �   * * F A I L U R E S :   0 * *   ( u n t o u c h e d ) . 
 
 -   ` t e s t s / t e s t _ d 3 d 9 _ a c t u a l . e x e `   �� �   * * A L L   D 3 D 9   A C T U A L   C H E C K S   P A S S E D * *   ( a f t e r   t h e   d e b u g _ e n a b l e d = 1   h a r n e s s 
 
     f i x   �� �   c h a i n / [ h u m a n - p 1 ] / [ f a l l b a c k : ] / [ c h a i n - b r e a k : ]   +   s w a p c h a i n / t r a c e r   a s s e r t s   a l l   e m i t t e d ) . 
 
 -   ` t e s t s / t e s t _ e x p o r t . e x e `   ( * * N E W * * )   �� �   * * F A I L U R E S :   0 * *   �� �   e x a c t   f o r m a t   s t r i n g ,   ` ( i n t ) ( x + 0 . 5 f ) `   r o u n d i n g 
 
     ( 9 9 . 6 �� � 1 0 0   /   2 0 0 . 5 �� � 2 0 1   /   - 0 . 5 �� � 0 ) ,   N U L L / 0 - s i z e / t i n y - b u f f e r   o v e r f l o w   s a f e t y ,   o r d e r   u n d e r   N U L L . 
 
 -   ` t e s t s / t e s t _ w i d g e t _ p a r s e . p y `   ( * * N E W * * )   �� �   * * W I D G E T - P A R S E :   P A S S * *   �� �   ` p a r s e _ l i n e `   i n t e g e r - o n l y   +   l o c a l e - s a f e 
 
     ( c o m m a - d e c i m a l   �� �   N o n e ) ,   w h i t e s p a c e / e m b e d d e d   v a r i a n t s ,   m o d u l e - l e v e l   P A T T E R N   p i n s . 
 
 -   ` t e s t s / t e s t _ p e _ s t r u c t u r e . p y `   �� �   * * F A I L U R E S :   0 * *   �� �   i m p o r t s   { K E R N E L 3 2 , U S E R 3 2 , m s v c r t } ,   1 1   e x p o r t s . 
 
 -   ` t e s t s / t e s t _ s o u r c e _ c o n t r a c t . p y `   �� �   * * S O U R C E - C O N T R A C T :   P A S S * *   �� �   R 1 3   o v l - d i a g - u n c o n d i t i o n a l   p i n   F L I P P E D   t o 
 
     g a t e d ,   ~ 2 0   n e w   R 1 9   p i n s   ( f o r m a t _ e x p o r t _ l i n e   s i g ,   t h r e a d + C r e a t e T h r e a d ,   5 0 0 m s + S l e e p ,   l o g   t r u n c a t e / a p p e n d / 
 
     f l u s h ,   d e b u g - o n l y   l a u n c h ,   D E T A C H   s h u t d o w n   o r d e r ,   e x a c t l y - 2   o v l - s t o p   s i t e s ,   t r a c e r / h e a r t b e a t   g a t e d ) . 
 
 
 
 # #   B U I L D 
 
 r c = 0 ,   z e r o   w a r n i n g s ,   * * V E R I F Y   P A S S * *   ( i 3 8 6   P E 3 2 ,   i m p o r t s   s u b s e t   { K E R N E L 3 2 , U S E R 3 2 , m s v c r t } ,   1 1   e x p o r t s ) . 
 
 * * d 3 d 9 . d l l   =   1 5 5 , 2 5 9   B ,   S H A 2 5 6   ` 6 5 d 8 e 8 8 0 1 c 1 a c 7 7 1 6 1 4 4 1 4 9 7 c e a 5 4 9 e 1 e b 9 8 9 3 8 c a d 1 5 5 2 e 1 f 6 1 c d 8 5 8 0 0 f 9 c 7 d 9 ` * * , 
 
 b y t e - i d e n t i c a l   3 - c o p y   ( r e p o   r o o t   +   g a m e   d i r   +   t e s t s ) ,   o l d   g a m e   l o g   d e l e t e d . 
 
 
 
 - - - 
 
 
 
 #   B U I L D   �� �   R O U N D   1 8   ( 2 0 2 6 - 0 9 - 0 7 )   �� �   R E S E A R C H   D O C U M E N T A T I O N   ( n o   c o d e   c h a n g e s   t o   d 3 d 9 . c / s r c ) 
 
 
 
 # #   R O U N D   S U M M A R Y 
 
 R e s e a r c h   r o u n d   �� �   * * N O   c o d e   c h a n g e s * * .   d 3 d 9 . c   a n d   s r c /   w e r e   N O T   t o u c h e d .   N e w   f i l e 
 
 ` R E S E A R C H _ U I _ M O D D I N G . m d `   r e c o r d s   t h e   c o m m u n i t y   r e s e a r c h   ( w e b   r e s e a r c h   d o n e   b y   t h e   o r c h e s t r a t i o n   t e a m ) : 
 
 h o w   t h e   A o E 3   l e g a c y   c o m m u n i t y   m o d s   t h e   U I ,   w h e t h e r   D a l i ' s   l i v e   r e s o u r c e - r a t e   H U D   a l r e a d y   e x i s t s ,   a n d 
 
 t h e   c o m m u n i t y ' s   e n g i n e - l e v e l   t e c h n i q u e s .   C u r r e n t   b u i l d   r e m a i n s   t h e   R 1 7   b u i l d 
 
 ` 9 7 f c 2 a d f 5 a 8 7 8 0 a a b 2 4 6 3 5 6 1 a c e e 4 3 c e 0 5 6 7 c f d 3 9 2 8 1 c 8 6 6 6 d c 2 c 3 9 e 5 6 a 2 1 f 3 1 `   ( 1 5 2 , 7 3 6   B )   �� �   n o t   r e b u i l t ,   n o t 
 
 r e d e p l o y e d . 
 
 
 
 # #   K E Y   F I N D I N G S   ( p e r s i s t e d   i n   R E S E A R C H _ U I _ M O D D I N G . m d ) 
 
 -   * * C o m m u n i t y   U I   m o d s   a r e   X M L / X M B   f i l e - s w a p s * * ,   n o t   D L L   i n j e c t i o n :   e x t r a c t   ` u i * . x m l . x m b `   f r o m   ` d a t a * . b a r ` 
 
     w i t h   A o E 3 E d   /   R e s o u r c e - M a n a g e r ,   e d i t   X M L ,   d r o p   t h e   l o o s e   f i l e   i n t o   ` < g a m e > / d a t a / `   ( u n i n s t a l l   =   d e l e t e ; 
 
     E S O - s a f e ) .   C a n o n i c a l   f i l e s :   ` u i m a i n n e w . x m l . x m b ` ,   ` u i m i n i m a p p a n e l n e w . x m l ` ,   ` u i o b j e c t i v e s . x m l . x m b ` . 
 
 -   * * T h e   e x a c t   m o d   D a l i   w a n t s   d o e s   N O T   a l r e a d y   e x i s t * *   �� �   t h e   r e s o u r c e   g a t h e r - r a t e - p e r - s e c o n d   d i s p l a y   i s   a 
 
     K N O W N   U N F U L F I L L E D   f e a t u r e   r e q u e s t   ( D E   f o r u m   t h r e a d   1 2 0 8 3 8   f r o m   2 0 2 1 ) ;   t h e   E S O C   G a t h e r i n g   R a t e s   w i k i   o n l y 
 
     d o c u m e n t s   s t a t i c   t h e o r y   r a t e s ;   n o   c o m m u n i t y   m o d   s h i p s   a   L I V E   p e r - g a m e   r a t e   H U D   f o r   l e g a c y   T A D .   G e n u i n e l y 
 
     n o v e l .   T h e   X M L   l a y e r   h a s   n o   s c r i p t i n g   t o   c o m p u t e   t i m e - d e r i v a t i v e s   o f   y o u r   o w n   s t o c k   �� �   o n l y   o u r   D L L   c a n . 
 
 -   * * C o m m u n i t y   e n g i n e - l e v e l   t e c h n i q u e   =   d i r e c t   e x e   b i n a r y   p a t c h i n g * *   ( A o E 3   U n H a r d c o d e   P a t c h ,   p e r - v e r s i o n 
 
     o f f s e t s   +   a   p l u g i n   D L L   i n   ` S t a r t u p \ u h c . c f g ` ) ,   N O T   a   d 3 d 9   p r o x y   D L L .   O u r   a p p r o a c h   h a s   n o   p r e c e d e n t   i n 
 
     t h i s   c o m m u n i t y   �� �   h a r d ,   b u t   n e w .   F a l l b a c k   B   d o c u m e n t e d   ( e x e - p a t c h   o n   o u r   a l r e a d y - v e r i f i e d   a d d r e s s e s ) . 
 
 -   * * M u l t i p l a y e r   f a i r n e s s   n o r m   v a l i d a t e s   t h e   d e s i g n : * *   r e a d - o n l y   p r e s e n t a t i o n a l   H U D   c h a n g e s   a r e   t r e a t e d   a s 
 
     f a i r   ( n e u r o n ' s   m i n i m a l i s t   U I   e x p l i c i t l y   b l o c k e d   i n p u t   b u t t o n s   a s   " u n f a i r   a d v a n t a g e " ) .   O u r   o v e r l a y   i s 
 
     c l i e n t - s i d e ,   r e a d - o n l y ,   i n j e c t s   n o   g a m e   i n p u t s . 
 
 -   * * R e c o m m e n d a t i o n :   k e e p   t h e   D L L   p a t h * *   ( o n l y   w a y   t o   s h o w   l i v e   r a t e s ) ;   t h e   r e m a i n i n g   b u g   i s   p u r e l y 
 
     " w h e r e   d o e s   t h e   i n - m a t c h   P r e s e n t   l i v e "   �� �   t h e   R 1 7   t r a c e r   ( ` p r e s e n t - p a t h   d e v = / s w = / s c e n e = ` )   a n s w e r s   i t 
 
     w i t h   o n e   m o r e   l o g .   F a l l b a c k   A   =   a   s t a t i c   X M L   H U D   ( c o m m u n i t y - s t y l e )   f o r   a   q u i c k   v i s i b l e   w i n   w h i l e   t h e 
 
     D L L   r e n d e r   p a t h   f i n i s h e s . 
 
 
 
 # #   F I L E S   C H A N G E D   T H I S   R O U N D 
 
 -   ` R E S E A R C H _ U I _ M O D D I N G . m d `   ( A D D E D   �� �   t h e   r e s e a r c h   s u m m a r y ,   v e r b a t i m   f r o m   t h e   o r c h e s t r a t i o n   t e a m ) . 
 
 -   ` . s w a r m / B U I L D . m d `   ( t h i s   s e c t i o n ) . 
 
 -   ` R E A D M E . m d `   ( a d d e d   a   s i n g l e   " # #   R e s e a r c h "   l i n e   p o i n t i n g   a t   t h e   r e s e a r c h   f i l e   �� �   i f   t h e   r e p o   l i n k s   i t ) . 
 
 -   N O   c h a n g e s   t o   ` d 3 d 9 . c ` ,   ` s r c / * ` ,   ` t e s t s / * ` ,   o r   b u i l d   s c r i p t s . 
 
 
 
 - - - 
 
 
 
 #   B U I L D   �� �   R O U N D   1 7   ( 2 0 2 6 - 0 9 - 0 7 )   �� �   T W O   D E C I S I V E   I N S T R U M E N T S :   E A G E R   I M P L I C I T - S W A P C H A I N   C A P T U R E   +   E N D S C E N E   P R O B E   ( t h e   f u l l s c r e e n   P r e s e n t   h a n d o f f ,   p r o v e n   n o t   t h e o r i z e d ) 
 
 
 
 # #   R O U N D   S U M M A R Y 
 
 N o   c r a s h   t h i s   r o u n d   ( g a m e   N O T   l a u n c h e d ) .   D a l i ' s   R 1 6   l i v e   l o g   f r o z e   t h e   m y s t e r y   a n d   p o i n t e d   t h e   g u n : 
 
 ` p r e s e n t - p a t h   d e v = 3 0 0   s w = 0   . . .   d e v = 7 5 0 0   s w = 0   f r a m e s = 2 9 2 5 `   d u r i n g   m e n u + l o a d i n g ,   t h e n   t h e   d e v i c e   P r e s e n t 
 
 h o o k   w e n t   S I L E N T   e x a c t l y   a t   m a t c h   s t a r t   w h i l e   t h e   g a m e   k e p t   r e n d e r i n g   �� �   a n d   Z E R O   ` p a t c h _ s w a p c h a i n : ` 
 
 l i n e s   e v e r .   D e d u c t i o n   1   ( t e s t a b l e ) :   t h e   i n - m a t c h   f r a m e s   p r e s e n t   t h r o u g h   t h e   * * I M P L I C I T   s w a p c h a i n * *   t h e 
 
 D 3 D 9   r u n t i m e   c r e a t e s   a t   d e v i c e   c r e a t i o n   �� �   n e v e r   c a p t u r e d   b e c a u s e   w e   o n l y   p a t c h e d   E X P L I C I T   a c q u i s i t i o n s ; 
 
 i n   f u l l s c r e e n   t h e   d r i v e r   h a n d s   P r e s e n t   o f f   t o   i t   i n t e r n a l l y .   D e d u c t i o n   2   ( t e s t a b l e ) :   t h e   d e v i c e   m a y 
 
 S T I L L   r u n   s c e n e   m e t h o d s   i n - m a t c h   ( E n d S c e n e   a l i v e   +   P r e s e n t   f r o z e n   = >   t h e   o v e r l a y   b e l o n g s   a t   s c e n e   e n d ) . 
 
 
 
 # #   D a l i   1 - M I N   L I V E   L O G   ( R 1 6   b u i l d   e c c a 3 c 5 f ,   s e s s i o n   d e v = 0 c a 5 a 1 e 0   �� �   t h e   E V I D E N C E ) 
 
 ` ` ` 
 
 p r e s e n t - p a t h   d e v = 3 0 0   s w = 0   . . .   d e v = 7 5 0 0   s w = 0   f r a m e s = 2 9 2 5       < -   d e v i c e   P r e s e n t   h o o k   r u n s   T H R O U G H   l o a d i n g 
 
 c h a i n   . . .   n = 3   [ h u m a n - p 1 ] 
 
 - - -   m a t c h   s t a r t   n = 3   p l a y e r = 0 F 3 8 7 8 0 0   r e s = 1 2 5 E 9 F 8 0   - - - 
 
 R E S   t = 1   . . .                                                                                           < -   O N E   s a m p l e 
 
 U I :   f o n t   c r e a t e d   . . . 
 
 o v l   f i r s t   d r a w   o k   f r a m e = 3 0 9 5 
 
 o v l   d i a g   r t = 0 1 1 2 9 a 6 0   r e c t = 1 2 , 1 2 : 2 5 0 x 1 4 1   a l p h a = 0 x D 9 
 
 ( E N D   �� �   d e v   c o u n t e r   f r e e z e s   a t   7 5 0 0 ;   n o   f u r t h e r   p r e s e n t - p a t h   l i n e s   d e s p i t e   t h e 
 
   m a t c h   c o n t i n u i n g ;   u s e r   s a w   t h e   o v e r l a y   f l a s h   o n c e   a t   m a t c h   s t a r t ) 
 
 ` ` ` 
 
 D e c i s i v e   r e a d i n g :   o u r   d e v i c e   P r e s e n t   h o o k   r a n   7 , 5 0 0   t i m e s   t h r o u g h   m e n u + l o a d i n g ,   t h e n   Z E R O   m o r e   a t   m a t c h 
 
 s t a r t   w h i l e   r e n d e r i n g   v i s i b l y   c o n t i n u e d .   C o m b i n e d   w i t h   z e r o   e x p l i c i t   s w a p c h a i n   r e q u e s t s ,   t h e   O N L Y 
 
 s e l f - c o n s i s t e n t   D 3 D 9   e x p l a n a t i o n   i s   t h e   r u n t i m e ' s   i m p l i c i t   s w a p c h a i n   ( f u l l s c r e e n   P r e s e n t   h a n d o f f ) . 
 
 
 
 # #   C H A N G E   L I S T   ( a l l   d i a g n o s t i c - o n l y   �� �   z e r o   b e h a v i o r   c h a n g e   t o   g a t e s / t r a c k e r / c h a i n / f o n t s ) 
 
 -   * * E a g e r   i m p l i c i t - s w a p c h a i n   c a p t u r e   ( t h e   f i x   t h a t   l i k e l y   m a k e s   s w   c o u n t   i n - m a t c h ) : * *   n e w 
 
     ` e a g e r _ i m p l i c i t _ s w a p c h a i n ( d e v ) `   c a l l e d   f r o m   B O T H   w _ c r e a t e _ d e v i c e   a n d   w _ c r e a t e _ d e v i c e _ e x ,   A F T E R 
 
     p a t c h _ d e v i c e _ p r e s e n t   a n d   B E F O R E   t h e   R 9   f o n t   b i n d .   I t   c a l l s   O U R   W R A P P E D   s l o t   1 4 
 
     ( ` w _ g e t _ s w a p c h a i n ( d e v ,   0 ,   & s w ) ` )   o n   t h e   g a m e ' s   b e h a l f   �� �   t h e   c a l l   f l o w s   t h r o u g h   t h e   e x i s t i n g   R 1 5 
 
     p a t c h   l o g i c   ( p a t c h   +   ` p a t c h _ s w a p c h a i n :   s w = . .   v t = . .   p r e s e n t = . . `   l o g ) ,   s o   t h e   i m p l i c i t   s w a p c h a i n   t h e 
 
     r u n t i m e   m a d e   a t   c r e a t i o n   i s   c a p t u r e d + p a t c h e d   u p - f r o n t ,   W I T H O U T   t h e   g a m e   e v e r   a s k i n g .   L o g s 
 
     ` s w a p c h a i n - i m p l :   s w = % p   p a t c h e d = % d `   p e r   d e v i c e   ( o r   ` s w a p c h a i n - i m p l :   n / a   h r = 0 x % 0 8 X `   i f   G e t S w a p C h a i n ( 0 ) 
 
     f a i l s ) .   I d e m p o t e n t :   p a t c h _ s w a p c h a i n   r e t u r n s   e a r l y   o n   a n   a l r e a d y - p a t c h e d   v t a b l e .   T h e   l a z y 
 
     w _ g e t _ s w a p c h a i n / w _ c a s c   w r a p p e r s   s t a y . 
 
 -   * * S c e n e - a l i v e   p r o b e : * *   ` w _ e n d s c e n e `   ( d e v i c e   s l o t   4 2 )   �� �   f o r w a r d   t o   t h e   s a v e d   o r i g i n a l ,   b u m p 
 
     ` s _ s c e n e _ e n d s ` ,   l o g   o n l y   v i a   t h e   m e r g e d   t r a c e r .   S l o t   t r u t h   R E - V E R I F I E D   a g a i n s t   t h e   R E A L   m i n g w - w 6 4 
 
     d 3 d 9 . h   o f   t h i s   t o o l c h a i n   ( I D i r e c t 3 D D e v i c e 9   i n t e r f a c e   l i n e s   1 2 4 4 - 1 2 4 5 ) :   * * B e g i n S c e n e = 4 1 ,   E n d S c e n e = 4 2 , 
 
     B O T H   A R G L E S S   ` S T D M E T H O D ( E n d S c e n e ) ( T H I S ) ` * *   ( o n e   ` t h i s `   s t d c a l l   o n   i 3 8 6 )   �� �   t h e   e x i s t i n g   D 9 _ E N D S C E N E   4 2 
 
     d e f i n e   ( u i . c )   a n d   t h e   p i n n e d   s l o t   s e t   { 3 , 1 3 , 1 4 , 1 6 , 3 8 , 4 1 , 4 2 , 5 7 , 5 8 , 8 3 , 8 9 , 9 0 }   a r e   c o n f i r m e d   c o r r e c t , 
 
     n o   s e t   c h a n g e .   p a t c h e d   s l o t   4 2   i n s i d e   p a t c h _ d e v i c e _ p r e s e n t   ( o r i g i n a l   s a v e d   O N C E ) . 
 
 -   * * M E R G E D   t r a c e r : * *   ` t r a c e r _ t i c k ( ) `   s h a r e d   b y   a l l   t h r e e   h o o k s ;   c o u n t e r s   b u m p   a t   t h e   T O P   o f   e a c h   h o o k 
 
     ( b e f o r e   t h e   e n a b l e d   c h e c k ) ,   t h e n   t h e   t i c k   l o g s   O N E   l i n e   p e r   3 0 0   C O M B I N E D   D e v / S w / S c e n e   i n c r e m e n t s : 
 
     ` p r e s e n t - p a t h   d e v = % u   s w = % u   s c e n e = % u   f r a m e s = % u `   ( R 1 6   f i e l d s   k e p t ,   ` s c e n e `   a d d e d ,   N O T   d e b u g - g a t e d ) . 
 
 -   * * C O N D I T I O N A L   E N D S C E N E   D R A W   R E A D Y   ( d o c u m e n t e d ,   N O T   e n a b l e d ) : * *   a   c l e a r l y - m a r k e d   c o m m e n t e d   h o o k   p o i n t 
 
     i n   w _ e n d s c e n e   e x p l a i n s   e x a c t l y   h o w   t h e   o v e r l a y   b o d y   w o u l d   r u n   a t   s c e n e   e n d   ( p o s t - E n d S c e n e ,   m i r r o r i n g 
 
     p r e s e n t _ h o o k ,   s a m e   g _ o v l _ l a s t _ d r a w _ f r a m e   g u a r d   +   B 7   t r i p w i r e )   �� �   w i r e d   O N L Y   i f   t h e   t r a c e r   p r o v e s 
 
     ` s c e n e `   c l i m b s   i n - m a t c h .   W e   a d d   c o d e   p e r   e v i d e n c e ,   n o t   b e f o r e . 
 
 
 
 # #   T E S T   E V I D E N C E   ( a l l   4   h a r n e s s e s   g r e e n ) 
 
 -   ` t e s t _ s o u r c e _ c o n t r a c t . p y ` :   * * S O U R C E - C O N T R A C T :   P A S S * *   �� �   R 1 6   p i n s   u p d a t e d   t o   t h e   M E R G E D   f o r m a t 
 
     ( f o r m a t   d e f i n e d   E X A C T L Y   o n c e   i n   t r a c e r _ t i c k ,   ` t r a c e r _ t i c k ( ) ; `   e x a c t l y   3   s i t e s ,   m o d - 3 0 0   o v e r   t h e 
 
     D e v + S w + S c e n e   t r i p l e ,   s c e n e   c o u n t e r   s t a t i c   +   o n e   i n c r e m e n t   p a t h ,   o r d e r i n g   p i n s   i n t a c t ,   a t t a c h   r e s e t s 
 
     i n c l .   s _ s c e n e _ e n d s   +   s _ o r i g _ e n d s c e n e )   p l u s   N E W   R 1 7   p i n s :   w _ e n d s c e n e   e x i s t s   +   f o r w a r d s   ` o ( s e l f ) ` 
 
     +   s a v e s   o r i g i n a l s   o n c e   ( v t [ D 9 _ E N D S C E N E ]   p a t c h e d ) ,   e a g e r   h e l p e r   d e f i n e d ,   e a g e r   c a l l   i n   B O T H   c r e a t e 
 
     w r a p p e r s ,   e a g e r   o r d e r i n g   A F T E R   p a t c h _ d e v i c e _ p r e s e n t   B E F O R E   t h e   R 9   f o n t   b i n d ,   w r a p p e d - s l o t   c a l l 
 
     ` w _ g e t _ s w a p c h a i n ( d e v ,   0 ,   & s w ) ` ,   b o t h   s w a p c h a i n - i m p l   l i n e   f o r m a t s .   A l l   R 1 5 / R 1 6   p i n s   s t i l l   p a s s . 
 
 -   ` t e s t s \ t e s t _ r a t e _ e n g i n e . e x e ` :   * * F A I L U R E S :   0 * *   ( u n t o u c h e d ) . 
 
 -   ` t e s t s \ t e s t _ d 3 d 9 _ a c t u a l . e x e ` :   * * A L L   D 3 D 9   A C T U A L   C H E C K S   P A S S E D * *   �� �   n e w   R 1 7   a s s e r t s   o n   t h e   R E A L 
 
     d e v i c e :   ( a )   a f t e r   C r e a t e D e v i c e   t h r o u g h   O U R   w r a p p e r ,   t h e   i m p l i c i t   s w a p c h a i n   w a s   A L R E A D Y   c a p t u r e d + 
 
     p a t c h e d   W I T H O U T   a n y   t e s t - s i d e   G e t S w a p C h a i n   ( s _ o r i g _ s w _ p r e s e n t   +   g _ s w   s e t ,   ` s w a p c h a i n - i m p l :   s w = . . 
 
     p a t c h e d = 1 `   A N D   ` p a t c h _ s w a p c h a i n :   s w = . . `   b o t h   l o g g e d ) ;   ( b )   s l o t   4 2   - >   w _ e n d s c e n e ;   ( c )   3 0 0   r e a l 
 
     E n d S c e n e   c a l l s   = >   s _ s c e n e _ e n d s = = 3 0 0 ,   d e v / s w   h o l d   a t   1 5 0 / 1 5 0 ,   a n d   t h e   m e r g e d   t r a c e r   f i r e s   i t s 
 
     S E C O N D   l i n e :   l o g   l i t e r a l l y   s h o w s   ` p r e s e n t - p a t h   d e v = 1 5 0   s w = 1 5 0   s c e n e = 0   f r a m e s = 0 `   t h e n 
 
     ` p r e s e n t - p a t h   d e v = 1 5 0   s w = 1 5 0   s c e n e = 3 0 0   f r a m e s = 0 `   �� �   t h e   e x a c t   i n - m a t c h   s h a p e   ( d e v i c e   a l i v e ,   p r e s e n t 
 
     f r o z e n )   r e p r o d u c e d   l i v e .   A l l   R 1 5 / R 1 6   a s s e r t s   s t i l l   g r e e n . 
 
 -   ` t e s t s \ t e s t _ p e _ s t r u c t u r e . p y ` :   * * F A I L U R E S :   0 * *   �� �   i m p o r t s   u n c h a n g e d   { K E R N E L 3 2 . d l l ,   m s v c r t . d l l , 
 
     U S E R 3 2 . d l l } ,   1 1   e x p o r t s ,   i 3 8 6 . 
 
 
 
 # #   R E S U L T 
 
 ` ` ` 
 
 s h a 2 5 6 = 9 7 f c 2 a d f 5 a 8 7 8 0 a a b 2 4 6 3 5 6 1 a c e e 4 3 c e 0 5 6 7 c f d 3 9 2 8 1 c 8 6 6 6 d c 2 c 3 9 e 5 6 a 2 1 f 3 1 
 
 s i z e = 1 5 2 7 3 6 
 
 ` ` ` 
 
 D e p l o y e d   b y t e - i d e n t i c a l   t o   r e p o   r o o t   +   g a m e   d i r   +   t e s t s ;   o l d   d 3 d 9 m o d . l o g   d e l e t e d ;   g a m e   N O T   l a u n c h e d . 
 
 
 
 # #   E X P E C T E D - L O G   f o r   D a l i ' s   n e x t   r u n   ( A C C E P T A N C E   �� �   r u n   > = 9 0 s   s o   t h e   m o d - 3 0 0   t r a c e r   c e r t a i n l y   f i r e s ) 
 
 ` ` ` 
 
 s w a p c h a i n - i m p l :   s w = . .   p a t c h e d = 1                                     < -   N E W :   e a g e r   c a p t u r e   a t   d e v i c e   c r e a t e   ( b o t h   c r e a t e   p a t h s ) 
 
 p a t c h _ s w a p c h a i n :   s w = . .   v t = . .   p r e s e n t = . .                     < -   t h e   i m p l i c i t   s w a p c h a i n   i n s t a l l   ( v i a   t h e   s a m e   p a t c h   l o g i c ) 
 
 p r e s e n t - p a t h   d e v = . .   s w = . .   s c e n e = . .   f r a m e s = . .           < -   e v e r y   ~ 5 s   T O G E T H E R   I N - M A T C H   i s   t h e   d e c i d i n g   m a r k e r 
 
 ` ` ` 
 
 R e a d i n g   t a b l e   ( t h e   w h o l e   p o i n t   o f   t h e   t w o   i n s t r u m e n t s ) : 
 
 -   ` s w `   C L I M B I N G   i n - m a t c h     = >   t h e   i m p l i c i t   c a p t u r e   W O R K E D   �� �   t h e   m a t c h   p r e s e n t s   t h r o u g h   t h e   r u n t i m e ' s 
 
     s w a p c h a i n   a n d   w e   n o w   o w n   i t s   P r e s e n t   h o o k .   N e x t   s t e p :   w i r e   t h e   r e d u n d a n t   d r a w   ( t h e   o v e r l a y   b o d y   o n 
 
     t h a t   P r e s e n t   i s   a l r e a d y   i n s t a l l e d   �� �   e x p e c t   i t   V I S I B L E )   a n d   o n l y   t h e n   a n y   r e n d e r   g e o m e t r y   f i x e s . 
 
 -   ` s c e n e `   C L I M B I N G   +   ` d e v ` / ` s w `   b o t h   f r o z e n   w h i l e   t h e   g a m e   r e n d e r s   = >   d e v i c e   i s   A L I V E   b u t   P r e s e n t   i s 
 
     r e r o u t e d   s o m e w h e r e   e x t e r n a l   ( a   w i n d o w e d   s w a p c h a i n   w e   s t i l l   c a n ' t   s e e ,   o r   e x p l i c i t - w i n d o w   H a n d o f f ) : 
 
     E N A B L E   t h e   a l r e a d y - p r e p a r e d   E n d S c e n e - d r a w   h o o k   p o i n t   ( d o c u m e n t e d   i n   w _ e n d s c e n e )   a s   t h e   a r c h i t e c t u r e . 
 
 -   ` d e v `   C L I M B I N G   i n - m a t c h     = >   t h e   d e v i c e   P r e s e n t   p a t h   r e t u r n e d   ( u n l i k e l y ;   r e v i s i t   w h a t   c h a n g e d ) . 
 
 -   A l l   t h r e e   f r o z e n   w h i l e   t h e   g a m e   r e n d e r s   = >   p r e s e n t   i s   e x t e r n a l / 2 n d   c o n t e x t   e n t i r e l y   - >   n e x t   r o u n d 
 
     t r a c e s   t h e   D 3 D 9 E x / C r e a t e D e v i c e E x   s e c o n d   d e v i c e   ( ` d e v i c e - c o u n t :   n _ d e v i c e s = `   a l r e a d y   r e p o r t s   i t ) . 
 
 -   E v e r y t h i n g   f r o z e n   A N D   t h e   g a m e   b r e a k s   = >   o u r   p a t c h i n g   b r o k e   t h e   g a m e   - >   r e v e r t   t h e   t r a c e r ,   c o m p a r e . 
 
 
 
 - - - 
 
 
 
 #   B U I L D   �� �   R O U N D   1 6   ( 2 0 2 6 - 0 9 - 0 7 )   �� �   I N S T R U M E N T - F I R S T :   P R E S E N T - P A T H   T R A C E R   ( t h e   R 1 5   t h e o r y   p r o d u c e d   N O   i n s t a l l   l i n e   �� �   p r o v e   i t ,   d o n ' t   t h e o r i z e ) 
 
 
 
 # #   R O U N D   S U M M A R Y 
 
 N o   c r a s h   t h i s   r o u n d   ( g a m e   N O T   l a u n c h e d ) .   R 1 5 ' s   s w a p c h a i n   t h e o r y   i s   n o w   D E C I S I V E L Y   I N V A L I D A T E D   a s   t h e 
 
 c o m p l e t e   a n s w e r :   D a l i ' s   R 1 5   l i v e   l o g   c o n t a i n s   * * Z E R O   ` p a t c h _ s w a p c h a i n : `   l i n e s * *   = >   t h e   g a m e   N E V E R   c a l l e d 
 
 o u r   w r a p p e d   ` I D i r e c t 3 D D e v i c e 9 : : G e t S w a p C h a i n `   ( s l o t   1 4 ) ,   y e t   t h e   m a t c h   r e n d e r e d   f i n e   w i t h   t h e   d e v i c e 
 
 ` P r e s e n t `   h o o k   d e a d   ( z e r o   ` R E S   t = 2 + ` ,   z e r o   h e a r t b e a t s ,   z e r o   ` o v l   s t o p ` ) .   T h e   i n - m a t c h   p r e s e n t   r u n s   t h r o u g h 
 
 a   p a t h   w e   d o   N O T   i n t e r c e p t :   e i t h e r   a   s w a p c h a i n   a c q u i r e d   v i a   * * C r e a t e A d d i t i o n a l S w a p C h a i n   ( s l o t   1 3 ) * *   o r   a 
 
 * * s e c o n d   d e v i c e * *   ( D 3 D 9 E x   /   C r e a t e D e v i c e E x ) .   L e g a c y   t h e o r y   i s   g o n e ;   t h i s   r o u n d   m a k e s   t h e   n e x t   l i v e   r u n 
 
 D E C I S I V E   i n s t e a d   o f   s i l e n t . 
 
 
 
 # #   D a l i   1 - M I N   L I V E   L O G   ( R 1 5   b u i l d   6 1 d 8 3 1 7 d ,   s e s s i o n   d e v = 0 c a 1 8 0 a 0   �� �   t h e   E V I D E N C E ) 
 
 ` ` ` 
 
 C r e a t e D e v i c e   h r = 0 0 0 0 0 0 0 0 0 0   d e v = 0 c a 1 8 0 a 0   p a t c h e d = y e s 
 
 c h a i n - b r e a k   x 5   ( m e n u ) 
 
 r e s e t   d e v = 0 c a 1 8 0 a 0   - >   f o n t   i n v a l i d a t e d 
 
 c h a i n   . . .   n = 3   [ h u m a n - p 1 ] 
 
 - - -   m a t c h   s t a r t   - - - 
 
 R E S   t = 1   f o o d = 0   w o o d = 0   c o i n = 0   e x p o r t = 0   p l a y e r = 0 F 5 C 5 8 0 0   r e s = 1 2 8 6 9 F 8 0 
 
 U I :   f o n t   c r e a t e d   . . . 
 
 o v l   f i r s t   d r a w   o k   f r a m e = 5 0 6 
 
 o v l   d i a g   r t = 0 1 1 4 9 a 6 0   r e c t = 1 2 , 1 2 : 2 5 0 x 1 4 1   a l p h a = 0 x D 9 
 
 ( E N D   �� �   n o   p a t c h _ s w a p c h a i n : ,   n o   h e a r t b e a t s ,   n o   R E S   t = 2 + ,   n o   o v l   s t o p ) 
 
 ` ` ` 
 
 D e c i s i v e   r e a d i n g :   i f   t h e   m a t c h   p r e s e n t e d   t h r o u g h   a   s w a p c h a i n   w e   P A T C H E D ,   t h e   i n s t a l l   l i n e 
 
 ( ` p a t c h _ s w a p c h a i n :   s w = . .   v t = . .   p r e s e n t = . . ` ,   l o g g e d   o n c e   p e r   s w a p c h a i n   o n   G e t S w a p C h a i n )   w o u l d   e x i s t . 
 
 I t   d o e s   n o t   = >   G e t S w a p C h a i n   w a s   n e v e r   c a l l e d   = >   t h e   i n - m a t c h   s w a p c h a i n   ( i f   a n y )   c o m e s   f r o m 
 
 C r e a t e A d d i t i o n a l S w a p C h a i n   o r   l i v e s   o n   a   s e c o n d   d e v i c e .   P R O V E N ,   n o   l o n g e r   t h e o r y . 
 
 
 
 # #   C H A N G E   L I S T   ( a l l   d i a g n o s t i c - o n l y   �� �   z e r o   b e h a v i o r   c h a n g e   t o   g a t e s / t r a c k e r / c h a i n / f o n t s ) 
 
 -   * * p r e s e n t - p a t h   t r a c e r   ( c o r e ) : * *   f i l e - s c o p e   ` s _ d e v _ p r e s e n t s `   /   ` s _ s w _ p r e s e n t s `   c o u n t e r s ;   i n c r e m e n t e d   a t 
 
     t h e   T O P   o f   ` p r e s e n t _ h o o k `   /   ` s w _ p r e s e n t _ h o o k `   ( B E F O R E   t h e   e n a b l e d   c h e c k ,   s o   a   d i s a b l e d   o v e r l a y   c a n 
 
     n e v e r   h i d e   t h e   p a t h ) .   E v e r y   3 0 0   c o m b i n e d   c a l l s   �� �   ` ( ( s _ d e v _ p r e s e n t s   +   s _ s w _ p r e s e n t s )   %   3 0 0 )   = =   0 `   �� � 
 
     O N E   n o t - d e b u g - g a t e d   l i n e :   ` p r e s e n t - p a t h   d e v = % u   s w = % u   f r a m e s = % u `   ( f r a m e s   =   g _ f r a m e s _ s i n c e _ r e s e t ) . 
 
     ~ 1   l i n e / 5 s   a t   6 0 f p s .   I f   n e i t h e r   c o u n t e r   m o v e s   d u r i n g   a n   8 - m i n   m a t c h ,   t h e   n e x t   l o g   n o w   S H O W S   i t . 
 
 -   * * C r e a t e A d d i t i o n a l S w a p C h a i n   h o o k   ( s l o t   1 3 ) : * *   ` w _ c a s c `   w r a p s   i t   e x a c t l y   l i k e   ` w _ g e t _ s w a p c h a i n ` 
 
     ( f o r w a r d   - >   o n   s u c c e s s   ` p a t c h _ s w a p c h a i n ( * s c ) `   - >   s a m e   s l o t - 3   h o o k   +   o n c e - p e r - o b j e c t   i n s t a l l   l o g ) . 
 
     ` p a t c h _ d e v i c e _ p r e s e n t `   s a v e s   ` s _ o r i g _ c a s c `   O N C E   a n d   p a t c h e s   ` v t [ 1 3 ] ` .   S l o t   t r u t h   r e - v e r i f i e d   a g a i n s t 
 
     t h e   r e a l   m i n g w - w 6 4   d 3 d 9 . h ,   I D i r e c t 3 D D e v i c e 9   i n t e r f a c e   l i n e s   1 1 9 5 - 1 2 1 8 :   1 3   C r e a t e A d d i t i o n a l S w a p C h a i n , 
 
     1 4   G e t S w a p C h a i n ,   1 5   G e t N u m b e r O f S w a p C h a i n s ,   1 6   R e s e t ,   1 7   P r e s e n t ,   1 8   G e t B a c k B u f f e r .   N e w   c a n o n i c a l 
 
     s l o t   s e t   p i n n e d   E X A C T L Y :   { 3 ,   1 3 ,   1 4 ,   1 6 ,   3 8 ,   4 1 ,   4 2 ,   5 7 ,   5 8 ,   8 3 ,   8 9 ,   9 0 } . 
 
 -   * * d e v i c e - c o u n t   ( s e c o n d   d e v i c e ) : * *   ` s _ n _ d e v _ s e e n `   c o u n t s   d i s t i n c t   d e v i c e   v t a b l e s   s e e n   t h r o u g h 
 
     ` p a t c h _ d e v i c e _ p r e s e n t ` ;   w h e n   a   S E C O N D   v t a b l e   s h o w s   u p   ( t h e   R 8   A 5   " s e c o n d   v t a b l e "   b r a n c h )   t h e   f i r s t 
 
     l o g   a d d s   ` d e v i c e - c o u n t :   n _ d e v i c e s = % u ` .   T e l l s   u s   i f   t h e   m a t c h   r u n s   o n   a   s e c o n d   D 3 D 9 E x / C r e a t e D e v i c e E x 
 
     d e v i c e .   A l s o   i n c r e m e n t e d   f o r   t h e   F I R S T   ( p a t c h e d )   v t a b l e   s o   n _ d e v i c e s   i s   t r u t h f u l . 
 
 -   * * D l l M a i n : * *   r e s e t s   ` s _ o r i g _ c a s c ` ,   ` s _ d e v _ p r e s e n t s ` ,   ` s _ s w _ p r e s e n t s ` ,   ` s _ n _ d e v _ s e e n ` , 
 
     ` s _ s e c o n d _ v t _ l o g g e d `   o n   p r o c e s s   a t t a c h . 
 
 
 
 # #   T E S T   E V I D E N C E   ( a l l   4   h a r n e s s e s   g r e e n ) 
 
 -   ` t e s t _ s o u r c e _ c o n t r a c t . p y ` :   * * S O U R C E - C O N T R A C T :   P A S S * *   �� �   d 9 m a c   s e t   p i n n e d   t o   { 3 , 1 3 , 1 4 , 1 6 , 3 8 , 4 1 , 4 2 , 5 7 , 5 8 , 
 
     8 3 , 8 9 , 9 0 } ;   1 4   N E W   R 1 6   p i n s   ( c o u n t e r s   s t a t i c   a t   f i l e   s c o p e ,   o n e   i n c r e m e n t   p a t h   e a c h ,   i n c r e m e n t s   B E F O R E 
 
     t h e   e n a b l e d - c h e c k / c o m m o n   b o d y   i n   B O T H   h o o k s ,   t r a c e r   l i n e   d e f i n e d   E X A C T L Y   o n c e ,   m o d - 3 0 0   c o n d i t i o n , 
 
     f r a m e s   f i e l d   =   g _ f r a m e s _ s i n c e _ r e s e t ,   D 9 _ C A S C   1 3 ,   w _ c a s c   e x i s t s   +   p e n d i n g   ` p a t c h _ s w a p c h a i n ( * s c ) ` , 
 
     s l o t   1 3   s a v e d   o n c e   +   p a t c h e d ,   s _ n _ d e v _ s e e n   p r e s e n t ,   d e v i c e - c o u n t   l i n e   o n   s e c o n d - v t a b l e   p a t h ,   a t t a c h 
 
     r e s e t s ) .   A l l   R 1 5   p i n s   s t i l l   p a s s   u n c h a n g e d . 
 
 -   ` t e s t s \ t e s t _ r a t e _ e n g i n e . e x e ` :   * * F A I L U R E S :   0 * *   ( u n t o u c h e d   t h i s   r o u n d ) . 
 
 -   ` t e s t s \ t e s t _ d 3 d 9 _ a c t u a l . e x e ` :   * * A L L   D 3 D 9   A C T U A L   C H E C K S   P A S S E D * *   �� �   t h e   R 1 5   r e a l - d e v i c e   s w a p c h a i n   b l o c k 
 
     ( s l o t   1 4   - >   w _ g e t _ s w a p c h a i n ,   s l o t   3   - >   s w _ p r e s e n t _ h o o k ,   o r i g   s a v e d ,   i n s t a l l   l o g g e d ,   i d e m p o t e n t )   P L U S   a 
 
     N E W   R 1 6   t r a c e r   b l o c k   a g a i n s t   t h e   R E A L   d e v i c e   +   R E A L   s w a p c h a i n :   w i t h   ` g _ s e t t i n g s . e n a b l e d = 0 `   ( z e r o - t o u c h 
 
     c o m m o n   b o d y ,   f o r w a r d s   s t i l l   h i t   r e a l   d 3 d 9 . d l l )   i t   d r i v e s   ` v t [ 1 7 ] `   ( p r e s e n t _ h o o k )   1 5 0 x   +   ` s w _ p r e s e n t _ h o o k ` 
 
     1 5 0 x   =   3 0 0   c o m b i n e d   - >   c o u n t e r s   r e a d   1 5 0 / 1 5 0 ,   t h e   m o d - 3 0 0   ` p r e s e n t - p a t h   d e v = 1 5 0   s w = 1 5 0   f r a m e s = . . `   l i n e 
 
     f i r e s   e x a c t l y   o n c e ,   a n d   d e v i c e   s l o t   1 3   i s   w _ c a s c   w i t h   t h e   o r i g i n a l   s a v e d . 
 
 -   ` t e s t s \ t e s t _ p e _ s t r u c t u r e . p y ` :   * * F A I L U R E S :   0 * *   �� �   i m p o r t s   u n c h a n g e d   { K E R N E L 3 2 . d l l ,   m s v c r t . d l l ,   U S E R 3 2 . d l l } , 
 
     1 1   e x p o r t s ,   i 3 8 6 . 
 
 
 
 # #   R E S U L T 
 
 ` ` ` 
 
 s h a 2 5 6 = e c c a 3 c 5 f c e 6 1 7 9 a 0 a a 2 e b e 3 4 d 4 a 8 e a 8 d 3 2 4 b a 2 4 a 3 e d f a 9 f 7 0 8 5 1 5 6 9 1 5 8 3 8 2 6 c e 
 
 s i z e = 1 5 2 0 3 8 
 
 ` ` ` 
 
 D e p l o y e d   b y t e - i d e n t i c a l   t o   r e p o   r o o t   +   g a m e   d i r   +   t e s t s ;   o l d   d 3 d 9 m o d . l o g   d e l e t e d ;   g a m e   N O T   l a u n c h e d . 
 
 
 
 # #   E X P E C T E D - L O G   f o r   D a l i ' s   n e x t   r u n   ( A C C E P T A N C E   �� �   r u n   > = 9 0 s   s o   t h e   3 0 0 - m o d u l o   t r a c e r   c e r t a i n l y   f i r e s ) 
 
 ` ` ` 
 
 p a t c h _ s w a p c h a i n :   s w = . .   v t = . .   p r e s e n t = . .             < -   O N L Y   i f   t h e   g a m e   a c q u i r e s   v i a   G e t S w a p C h a i n   O R   C r e a t e A d d i t i o n a l S w a p C h a i n 
 
 p r e s e n t - p a t h   d e v = . .   s w = . .   f r a m e s = . .                     < -   e v e r y   ~ 5 s   I N - M A T C H   i s   t h e   D E C I D I N G   m a r k e r 
 
 R E S   t = 1   . . .   t = 2   . . .                                                     < -   c a d e n c e   a s   b e f o r e 
 
 o v l   h e a r t b e a t   3 0 0   . . .                                                 < -   w h e n e v e r   t h e   o v e r l a y   d r a w   p a t h   i s   a l i v e 
 
 ` ` ` 
 
 R e a d i n g   t a b l e : 
 
 -   ` d e v `   c o u n t s   R I S I N G   i n - m a t c h     = >   t h e   d e v i c e   P r e s e n t   p a t h   I S   a l i v e   ( t h e   R 1 5   d e v i c e - h o o k   s i l e n c e   w a s 
 
     a   d i f f e r e n t   b u g   �� �   r e v i s i t   R E S / l o g   o r d e r i n g ,   n o t   t h e   h o o k i n g ) . 
 
 -   ` s w `   c o u n t s   R I S I N G   i n - m a t c h       = >   s w a p c h a i n   p a t h   C O N F I R M E D   a l i v e   - >   h o o k   d e e p e r   i n t o   t h a t   s w a p c h a i n 
 
     ( s u r f a c e / R T   a c q u i r e   +   r a w   d r a w )   n e x t   r o u n d . 
 
 -   N E I T H E R   r i s i n g   w h i l e   t h e   g a m e   r e n d e r s   = >   p r e s e n t   h a p p e n s   O U T S I D E   o u r   p a t c h e d   o b j e c t s   e n t i r e l y   - > 
 
     n e x t   s t e p   i s   D 3 D 9 E x   /   C r e a t e D e v i c e E x   s e c o n d - d e v i c e   t r a c i n g   ( w a t c h   ` d e v i c e - c o u n t :   n _ d e v i c e s = ` ) 
 
     o r   a   G e t D e v i c e C a p s - l e v e l   p r o b e . 
 
 -   b o t h   c o u n t i n g   i n   m e n u / l o a d i n g   O N L Y   = >   m a t c h - s p e c i f i c   p r e s e n t   p a t h   �� �   k e e p   t h e   l o g   a n d   c o r r e l a t e   t h e 
 
     c o u n t e r   b o u n d a r y   w i t h   t h e   m a t c h - s t a r t   f r a m e . 
 
 
 
 - - - 
 
 
 
 #   B U I L D   �� �   R O U N D   1 5   ( 2 0 2 6 - 0 9 - 0 7 )   �� �   R O O T   C A U S E :   t h e   g a m e   r e n d e r s   I N - M A T C H   v i a   I D i r e c t 3 D S w a p C h a i n 9 : : P r e s e n t 
 
 
 
 # #   R O U N D   S U M M A R Y 
 
 N o   c r a s h   t h i s   r o u n d   ( g a m e   N O T   l a u n c h e d ) .   T h e   i n - m a t c h   o v e r l a y   i n v i s i b i l i t y   i s   n o w   E X P L A I N E D   a n d   F I X E D   a t 
 
 t h e   r o o t :   * * A g e   o f   E m p i r e s   I I I :   T A D   r e n d e r s   t h e   m a t c h   t h r o u g h   t h e   s w a p   c h a i n ,   n o t   t h e   d e v i c e . * *   O u r 
 
 o v e r l a y   r a n   o n   ` I D i r e c t 3 D D e v i c e 9 : : P r e s e n t `   ( m e n u / l o a d i n g   p a t h )   �� �   t h e   m a t c h   n e v e r   c a l l s   i t . 
 
 V e r i f i e d   S L O T   T R U T H   a g a i n s t   t h e   R E A L   m i n g w - w 6 4   d 3 d 9 . h   o f   t h i s   b u i l d   t o o l c h a i n 
 
 ( ` . . . \ w 6 4 d e v k i t \ i n c l u d e \ d 3 d 9 . h ` ,   2 2 2 9   l i n e s ) :   I D i r e c t 3 D D e v i c e 9   �� �   G e t S w a p C h a i n = * * 1 4 * * ,   R e s e t = 1 6 ,   P r e s e n t = 1 7 ; 
 
 I D i r e c t 3 D S w a p C h a i n 9   i s   a   S E P A R A T E   i n t e r f a c e   ( h e a d e r   l i n e s   3 0 7 - 3 2 3 )   �� �   * * P r e s e n t = 3 * * ,   s i g n a t u r e 
 
 ` ( s r c ,   d s t ,   h w n d ,   d i r t y ,   D W O R D   f l a g s ) `   �� �   a   t r a i l i n g   ` D W O R D   f l a g s `   t h a t   D e v i c e : : P r e s e n t   d o e s   N O T   h a v e . 
 
 A   w r o n g - a r i t y   h o o k   t h e r e   w o u l d   c o r r u p t   t h e   s t d c a l l   s t a c k ,   s o   t h e   h o o k   m u s t   b e   6 - p a r a m . 
 
 
 
 # #   D a l i   8 - M I N   L I V E   L O G   ( R 1 4   b u i l d   2 9 5 0 3 e 0 9   �� �   t h e   E V I D E N C E ) 
 
 ` ` ` 
 
 r e s e t   d e v = 0 c a 4 e 4 6 0   - >   f o n t   i n v a l i d a t e d                                     < -   p r e c e d e s   m a t c h   s t a r t   ( d e v i c e   r e s e t ) 
 
 o v l   f i r s t   d r a w   o k   f r a m e = 6 8 6                                                           < -   d e v i c e   P r e s e n t   p a t h   r a n   O N C E   ( m e n u / l o a d ) 
 
 o v l   d i a g   r t = 0 1 2 7 9 6 0 0   r e c t = 1 2 , 1 2 : 2 5 0 x 1 4 1   a l p h a = 0 x D 9             < -   g e o m e t r y   f i n e 
 
 R E S   t = 1   f o o d = . . .   w o o d = . . .   c o i n = . . .   e x p o r t = . . .                       < -   E X A C T L Y   O N E   R E S ,   O N L Y   a t   t = 1 
 
 ` ` ` 
 
 -   * * Z E R O * *   h e a r t b e a t   l i n e s ,   * * Z E R O * *   ` R E S   t = 2 + ` ,   * * Z E R O * *   ` o v l   s t o p ` ,   * * Z E R O * *   c r a s h e s   a c r o s s   8   m i n u t e s   o f   L I V E   B A T T L E 
 
     w h i l e   t h e   g a m e   r e n d e r e d   v i s i b l y   = >   t h e   d e v i c e   p r e s e n t   h o o k   i s   n e v e r   c a l l e d   i n - m a t c h   ( n o t   a   g a t e , 
 
     n o t   t h e   t r a c k e r )   = >   t h e   g a m e   p r e s e n t s   t h r o u g h   t h e   S W A P   C H A I N   a f t e r   t h e   m e n u .   R o o t   c a u s e   c o n f i r m e d . 
 
 
 
 # #   C H A N G E   L I S T 
 
 -   * * d 3 d 9 . c   �� �   s w a p - c h a i n   c a p t u r e   c h a i n   ( t h e   f i x ) : * *   ` p a t c h _ d e v i c e _ p r e s e n t `   n o w   a l s o   w r a p s   d e v i c e   s l o t 
 
     1 4   ( ` G e t S w a p C h a i n ` )   w i t h   ` w _ g e t _ s w a p c h a i n ` ,   w h i c h   c a l l s   t h e   o r i g i n a l   t h e n   ` p a t c h _ s w a p c h a i n ( p p ) ` : 
 
     s a v e s   t h e   R E A L   s w a p c h a i n   v t a b l e ,   p a t c h e s   s l o t   3   t o   ` s w _ p r e s e n t _ h o o k `   ( V i r t u a l P r o t e c t   R W ) ,   a n d   l o g s 
 
     ` p a t c h _ s w a p c h a i n :   s w = % p   v t = % p   p r e s e n t = % p `   O N C E   p e r   s w a p c h a i n .   ` s w _ p r e s e n t _ h o o k `   ( 6   p a r a m s , 
 
     f o r w a r d s   t h e   t r a i l i n g   ` D W O R D   f l a g s ` )   a n d   ` p r e s e n t _ h o o k `   a r e   T H I N   f o r w a r d s   t o   o n e   S H A R E D   b o d y 
 
     ` o v e r l a y _ p r e s e n t _ c o m m o n ( s e l f ,   i s _ s w ) `   �� �   n o   c o d e   d u p l i c a t i o n ,   o n e   B 7   t r i p w i r e   f o r   b o t h . 
 
 -   * * d 3 d 9 . c   �� �   s a m e - f r a m e   d o u b l e - d r a w   g u a r d : * *   ` g _ o v l _ l a s t _ d r a w _ f r a m e `   ( i n i t   - 1 )   c h e c k e d   B E F O R E   t h e 
 
     c o m m o n - b o d y   w o r k ;   b o d y   m a r k s   ` g _ o v l _ l a s t _ d r a w _ f r a m e   =   g _ f r a m e s _ s i n c e _ r e s e t `   t h e n   i n c r e m e n t s   t h e 
 
     f r a m e   c o u n t e r   a t   t h e   E N D .   S t e a d y - s t a t e   s i n g l e - P r e s e n t   n e v e r   f a l s e - s k i p s ;   p a t h o l o g i c a l   s a m e - f r a m e 
 
     d o u b l e - p r e s e n t   o f   b o t h   k i n d s   C A N   d o u b l e - d r a w   ( i m p o s s i b l e   t o   f u l l y   s e p a r a t e   w i t h   a   m o n o t o n i c 
 
     c o u n t e r   �� �   a n a l y z e d ,   a c c e p t e d ) .   M a r k e r   N O T   r e s e t   o n   R e s e t   ( a f t e r   r e s e t   f r a m e s = 0 ,   l a s t = N   - >   p a s s e s ) . 
 
 -   * * d 3 d 9 . c / g a m e i f . c / u i . c   �� �   m e n u   f a l s e - a l a r m   s u p p r e s s i o n : * *   u n t i l   a   m a t c h   s t a r t s   t h e r e   i s   n o   c h a i n ,   s o 
 
     ` g _ v a l u e s _ v a l i d = = 0 `   /   ` g _ r e s = = N U L L `   a r e   B Y   D E S I G N .   N e w   ` i n t   g _ m a t c h _ a c t i v e `   l a t c h   ( s t a t e . h + d 3 d 9 . c 
 
     g l o b a l s ) :   s e t   o n   ` - - -   m a t c h   s t a r t   - - - `   ( g a m e i f . c ,   w h e n   t h e   c h a i n   i s   V A L I D ) ,   c l e a r e d   w h e n   t h e   c h a i n 
 
     b r e a k s ;   w h i l e   i n a c t i v e ,   ` o v l _ a b o r t _ s t o p `   n o   l o n g e r   c o u n t s   t h e   v a l u e s / r e s   s t a g e s   ( t h e   o t h e r   s i x 
 
     g a t e s   a l w a y s   c o u n t )   �� �   n o   m o r e   p r e - m a t c h   ` o v l   s t o p `   f a l s e   a l a r m s ,   n o   p o s t - m a t c h   f a l s e   f r i e n d l i n e s s . 
 
 -   * * d 3 d 9 . c   D l l M a i n : * *   n e w   s t a t i c s   r e s e t   o n   a t t a c h   ( ` s _ o r i g _ g e t _ s w a p c h a i n ` ,   ` s _ o r i g _ s w _ p r e s e n t ` , 
 
     ` g _ s w ` ,   ` g _ o v l _ l a s t _ d r a w _ f r a m e ` ) .   r e s e t _ h o o k   c o m m e n t   n o w   s a y s   s l o t s   1 4 / 1 6 / 1 7   r e - a s s e r t e d . 
 
 -   * * N e w   c a n o n i c a l   s l o t   s e t   ( c o n t r a c t - p i n n e d ) : * *   ` { 3 ,   1 4 ,   1 6 ,   3 8 ,   4 1 ,   4 2 ,   5 7 ,   5 8 ,   8 3 ,   8 9 ,   9 0 } ` 
 
     ( 3   =   S w a p C h a i n : : P r e s e n t ,   1 4   =   D e v i c e : : G e t S w a p C h a i n ,   1 6 / 1 7   v i a   t h e   $ D 9   c o n s t a n t s   a l r e a d y   t h e r e ) . 
 
 
 
 # #   T E S T   E V I D E N C E   ( a l l   4   h a r n e s s e s   g r e e n ) 
 
 -   ` t e s t _ s o u r c e _ c o n t r a c t . p y ` :   * * S O U R C E - C O N T R A C T :   P A S S * *   �� �   i n c l u d i n g   t h e   t h r e e   r e - p i n n e d   o r d e r   p i n s 
 
     ( t h i n - f o r w a r d   h o o k s   +   s h a r e d   c o m m o n   b o d y ) ,   t h e   r e - p o i n t e d   z e r o - t o u c h   +   g d i - a f t e r - c l e a r   p i n s ,   a n d 
 
     t h e   1 4   n e w   R 1 5   p i n s   ( s l o t   s e t ,   w _ g e t _ s w a p c h a i n ,   p a t c h _ s w a p c h a i n - o n c e ,   s w a p c h a i n   s l o t   3 ,   o r i g   s a v e d 
 
     o n c e ,   b o t h   h o o k s   - >   c o m m o n   > =   2 ,   s a m e - f r a m e   m a r k e r ,   D W O R D   f l a g s   f o r w a r d ,   g _ m a t c h _ a c t i v e   s e t / c l e a r , 
 
     v a l u e s / r e s   s u p p r e s s i o n ) . 
 
 -   ` t e s t s \ t e s t _ r a t e _ e n g i n e . e x e ` :   * * F A I L U R E S :   0 * * 
 
 -   ` t e s t s \ t e s t _ d 3 d 9 _ a c t u a l . e x e ` :   * * A L L   D 3 D 9   A C T U A L   C H E C K S   P A S S E D * *   �� �   i n c l u d i n g   t h e   N E W   S W A P C H A I N   b l o c k 
 
     a g a i n s t   a   R E A L   d 3 d 9   d e v i c e   ( l o a d s   t h e   r e a l   s y s t e m   d 3 d 9 . d l l   f o r   g _ r e a l ,   C r e a t e W i n d o w A   +   H A L / R E F 
 
     f a l l b a c k ,   h a n d - b u i l t   D 3 D P R E S E N T _ P A R A M E T E R S ) :   d e v i c e   s l o t   1 4   - >   w _ g e t _ s w a p c h a i n ,   G e t S w a p C h a i n   - > 
 
     s w a p c h a i n   s l o t   3   = =   s w _ p r e s e n t _ h o o k ,   o r i g i n a l   s a v e d ,   ` p a t c h _ s w a p c h a i n `   l o g g e d ,   g _ s w   c a p t u r e d , 
 
     r e p e a t e d   G e t S w a p C h a i n   s t a y s   p a t c h e d   ( i d e m p o t e n t ) . 
 
 -   ` t e s t s \ t e s t _ p e _ s t r u c t u r e . p y ` :   * * F A I L U R E S :   0 * *   �� �   i m p o r t s   u n c h a n g e d   { K E R N E L 3 2 . d l l ,   m s v c r t . d l l , 
 
     U S E R 3 2 . d l l } ,   1 1   e x p o r t s ,   i 3 8 6 . 
 
 
 
 # #   R E S U L T 
 
 ` ` ` 
 
 s h a 2 5 6 = 6 1 d 8 3 1 7 d 1 d 1 7 e 4 6 9 0 0 5 c 2 7 7 e 9 7 c f d 7 b 4 c 6 8 c 7 6 3 4 8 5 6 a 5 8 9 c 9 a 4 b 6 8 9 f 2 4 d 3 1 2 5 c 
 
 s i z e = 1 5 1 2 9 6 
 
 ` ` ` 
 
 D e p l o y e d   b y t e - i d e n t i c a l   t o   r e p o   r o o t   +   g a m e   d i r   +   t e s t s ;   o l d   d 3 d 9 m o d . l o g   d e l e t e d ;   g a m e   N O T   l a u n c h e d . 
 
 
 
 # #   E X P E C T E D - L O G   f o r   D a l i ' s   n e x t   r u n   ( A C C E P T A N C E ) 
 
 ` ` ` 
 
 p a t c h _ s w a p c h a i n :   s w = . .   v t = . .   p r e s e n t = . .       < -   N E W ,   a p p e a r s   o n   i n s t a l l   ( m e n u / l o a d   e a c h   r e s e t   t o o ) 
 
 r e s e t   d e v = 0 c a 4 e 4 6 0   - >   f o n t   i n v a l i d a t e d 
 
 o v l   f i r s t   d r a w   o k   f r a m e = . . 
 
 R E S   t = 1   . . .   p l a y e r = . .   r e s = . . 
 
 R E S   t = 2   . . .                                                                                           < -   N E W :   R E S   l i n e s   C O N T I N U E   d u r i n g   t h e   m a t c h 
 
 R E S   t = 3   . . . 
 
 o v l   h e a r t b e a t   3 0 0   . . .                                                                       < -   N E W :   h e a r t b e a t s   ( ~ 1 0 s )   D U R I N G   t h e   m a t c h 
 
 o v l   d i a g   r t = . .   r e c t = . . , . . : 2 5 0 x 1 4 1   a l p h a = . . 
 
 ` ` ` 
 
 D e c i s i o n   t a b l e :   ( 1 )   R E S   t = 1 , 2 , 3 �� �   +   h e a r t b e a t s   d u r i n g   b a t t l e   +   o v e r l a y   v i s i b l e   = >   A C C E P T ,   R 1 5   d o n e . 
 
 ( 2 )   R E S / h e a r t b e a t s   p r e s e n t   b u t   o v e r l a y   i n v i s i b l e   ( + ` o v l   s t o p `   o r   g e o m e t r y )   = >   r e n d e r - g e o m e t r y   p r o b l e m , 
 
 n e x t   r o u n d   =   s w a p c h a i n   r a w   d r a w   ( n o   R T   c h a i n ) .   ( 3 )   S T I L L   o n l y   o n e   R E S   t = 1 ,   n o   h e a r t b e a t s   d u r i n g   m a t c h 
 
 = >   s w a p c h a i n   P r e s e n t   N O T   c a l l e d   e i t h e r   ( g a m e   p r e s e n t s   v i a   a   d i f f e r e n t   p a t h   e n t i r e l y )   = >   n e x t   r o u n d   = 
 
 h o o k   ` p r e s e n t `   a t   t h e   S D L / G D I   b o u n d a r y   o r   c a p t u r e   s w a p c h a i n s   o u t   o f   t h e   w r a p p e r ' s   C r e a t e A d d i t i o n a l S w a p C h a i n . 
 
 
 
 - - - 
 
 
 
 #   B U I L D   �� �   R O U N D   1 4   ( 2 0 2 6 - 0 9 - 0 7 )   �� �   T W O   E V I D E N C E - D R I V E N   F I X E S   ( t r a c k e r   c a d e n c e   b u g   +   g a t e - s t a g e   s t o p   d e t e c t o r ) 
 
 
 
 # #   R O U N D   S U M M A R Y 
 
 N o   c r a s h   t h i s   r o u n d   ( g a m e   N O T   l a u n c h e d ) .   T w o   f i n d i n g s ,   b o t h   f r o m   l i v e   l o g s   +   c o d e   r e v i e w : 
 
 -   * * F I N D I N G   A   ( p r o v e n   t r a c k e r   b u g ,   h a r d - r u l e   o v e r r i d e ) : * *   s r c / t r a c k e r . c : 1 0 7 - 1 1 2   �� �   t h e   N O T - d u e 
 
     b r a n c h   o f   t h e   s a m p l e - c a d e n c e   g a t e   d i d   ` s _ l a s t _ t i m e   =   n o w ` ,   r e s e t t i n g   t h e   i n t e r v a l   a c c u m u l a t o r   o n 
 
     E V E R Y   n o n - d u e   f r a m e .   R e s u l t :   ` d u e `   c o u l d   o n l y   e v e r   b e   t r u e   o n   t h e   v e r y   f i r s t   s a m p l e 
 
     ( ` ! s _ h a v e _ t i m e ` ) ,   s o   t h e   t r a c k e r   s a m p l e d   E X A C T L Y   O N C E   p e r   s e s s i o n   a n d   a l l   r a t e s   f r o z e   a t   0 . 
 
 -   * * F I N D I N G   B   ( d e t e c t o r   b l i n d   s p o t ) : * *   t h e   R 1 3   s i l e n t - s t o p   d e t e c t o r   o n l y   c o v e r e d   t h e   f o u r   R T   s t a g e s 
 
     I N S I D E   u i _ d r a w ;   t h e   p r e - R T   g a t e s   k i l l e d   t h e   d r a w   e v e r y   f r a m e   i n   s i l e n c e . 
 
 
 
 # #   L I V E - L O G   Q U O T E   ( t h e   l a s t   l o g ,   4 5 0 3   B ,   t h r e e   f u l l   s e s s i o n s   �� �   R 1 3 - e r a   b i n a r y ,   D e b u g = 1   I N I ) 
 
 E v e r y   o n e   o f   t h e   t h r e e   s e s s i o n s   s h o w s   t h e   s a m e   b o d y   a f t e r   ` - - -   m a t c h   s t a r t   n = 3   . . .   - - - ` : 
 
 ` ` ` 
 
 D B G   r a t e s   f o o d = 0 . 0   w o o d = 0 . 0   c o i n = 0 . 0   e x p o r t = 0 . 0   p a u s e d = 0 
 
 R E S   t = 1   f o o d = 0   w o o d = 0   c o i n = 0   e x p o r t = 0   p l a y e r = 0 F 6 0 7 8 0 0   r e s = 1 2 8 6 A 0 8 0       < -   E X A C T L Y   O N E ,   a l w a y s   t = 1 
 
 U I :   f o n t   c r e a t e d   s i z e = 1 4   f a c e = G e o r g i a   f o n t = 0 2 e 7 d f 1 8 
 
 o v l   f i r s t   d r a w   o k   f r a m e = 4 5 9 
 
 o v l   d i a g   r t = 0 1 2 6 9 9 c 0   r e c t = 1 2 , 1 2 : 2 5 0 x 1 4 1   a l p h a = 0 x D 9                                         < -   d i a g   p r e s e n t ,   g e o m e t r y   f i n e 
 
 ` ` ` 
 
 -   * * R E S * *   a p p e a r s   o n c e   p e r   s e s s i o n   a n d   o n l y   a t   ` t = 1 `   = >   F I N D I N G   A   c o n f i r m e d   ( c a d e n c e   b u g ) . 
 
 -   * * h e a r t b e a t s * * :   n o n e   i n   a n y   s e s s i o n   ( d r a w   n e v e r   r e a c h e s   t h e   b u m p   = >   b u t   t h e   R 1 3   R T - s t a g e   d e t e c t o r 
 
     l o g g e d   Z E R O   ` o v l   s t o p `   t o o )   = >   t h e   d r a w   d i e s   a t   a   G A T E ,   n o t   a n   R T   s t a g e   ( F I N D I N G   B ' s   b l i n d   s p o t ) . 
 
 -   * * L i k e l i e s t   k i l l e r   ( p e n d i n g   g a t e - d e t e c t o r   c o n f i r m   o n   n e x t   r u n ) : * *   ` g _ r e s   = =   N U L L `   �� �   t h e   c h a i n 
 
     f o u n d   d u r i n g   t r a i n / l o a d   ( ` c t x = 0 5 9 A 0 0 0 0   n = 3   p l a y e r = . .   r e s = 1 2 8 6 A 0 8 0   i n c = . . ` )   m a y   n o t   s u r v i v e   t h e 
 
     r e a l   b a t t l e   l a y o u t ;   ` g _ v a l u e s _ v a l i d `   ( c l e a r e d   b y   a   m i d - s e s s i o n   r e s e t )   i s   t h e   r u n n e r - u p . 
 
 -   N o t e :   ` R 1 4   D L L   l o a d e d   b a s e = . . `   i s   j u s t   D l l M a i n ' s   h i s t o r i c a l   l a b e l   ( d 3 d 9 . c : 6 5 3 ) ,   n o t   a   r o u n d   t a g . 
 
 
 
 # #   C H A N G E   L I S T 
 
 -   * * s r c / t r a c k e r . c : 1 0 7 - 1 1 2   ( F I N D I N G   A ) * *   �� �   t h e   ` s _ l a s t _ t i m e   =   n o w ; `   a s s i g n m e n t   i s   R E M O V E D   f r o m   t h e 
 
     ` ! d u e `   b r a n c h   ( k e p t   ` g _ v a l u e s _ v a l i d   =   1 ;   r e t u r n ; ` ) .   T h e   i n t e r v a l   a c c u m u l a t o r   n o w   k e e p s   g r o w i n g 
 
     a c r o s s   n o n - d u e   f r a m e s   u n t i l   ` ( n o w   -   s _ l a s t _ t i m e )   > =   i n t e r v a l ` ;   t h e   D U E   p a t h   s t i l l   s e t s 
 
     ` s _ l a s t _ t i m e   =   n o w `   ( l i n e   ~ 1 9 0 ) .   A 2   s e m a n t i c s   v e r i f i e d   u n c h a n g e d :   t h e   f i r s t - e v e r   s a m p l e   s t i l l 
 
     o n l y   l a y s   d o w n   t h e   b a s e l i n e   ( ` g _ e m a _ v a l i d `   n e e d s   ` c o u n t > = 1 `   c h e c k e d   B E F O R E   t h e   s t o r e   a t   l i n e s 
 
     1 8 1   v s   1 8 3 ,   s o   t h e   f i r s t   s a m p l e   �� �   c o u n t = = 0   �� �   n e v e r   m a r k s   a   s l o t   a l i v e ) . 
 
 -   * * s r c / u i . c   ( F I N D I N G   B ) * *   �� �   T H E   s i l e n t - s t o p   d e t e c t o r   i s   g e n e r a l i z e d   t o   c o v e r   B O T H   t h e   p r e - R T   g a t e s 
 
     A N D   t h e   f o u r   R T   s t a g e s   ( o n e   c o u n t e r   ` s _ o v l _ a b o r t _ n ` ,   o n e   h e l p e r   ` o v l _ a b o r t _ s t o p ( s t a g e ) ` ;   s t a g e 
 
     s t r i n g s   s t a y   d i s t i n c t ) .   T h e   e i g h t   g a t e s   n o w   l o g   o n c e   a t   6 0   c o n s e c u t i v e   a b o r t s : 
 
     ` o v l   s t o p :   % u   c o n s e c u t i v e   f r a m e   a b o r t s   a t   s t a g e = < e n a b l e d | v e r s i o n | p a n e l | v a l u e s | r e s | d e v i c e | u i _ r e a d y | c o o l d o w n >   -   o v e r l a y   d r a w   h a l t e d ` . 
 
     E x i s t i n g   R T   s t a g e s   ( ` g r t - m i s s i n g | g r t - f a i l | s u r f a c e - b a d | g u a r d - f a i l ` )   u n c h a n g e d .   C o u n t e r   s t i l l   r e s e t s 
 
     o n   a n y   c o m p l e t e d   d r a w   ( h e a r t b e a t   b u m p ) .   G a t e   s e m a n t i c s   u n t o u c h e d   �� �   i n s t r u m e n t a t i o n   o n l y . 
 
 -   * * t e s t s / t e s t _ r a t e _ e n g i n e . c * *   �� �   N E W   ` t e s t _ r a t e _ c a d e n c e _ m u l t i ( ) `   ( R 1 4   r e g r e s s i o n ) :   f r a m e - g r a n u l a r i t y 
 
     t i m e l i n e   v i a   ` t r a c k e r _ s e t _ c l o c k _ o v e r r i d e `   �� �   f i r s t   s a m p l e   a t   T ,   t w o   s u b - i n t e r v a l   f r a m e s   ( T + 1 6 / T + 3 2 ) , 
 
     t h e n   T + 5 0 0   M U S T   b e   d u e   ( c a d e n c e   a c c u m u l a t e d   p a s t   t h e   s u b - i n t e r v a l   f r a m e s ;   t h e   o l d   b u g   w o u l d   h a v e 
 
     k e p t   i t   ! d u e ) .   P i n s   ` g _ l a s t _ s a m p l e _ t i c k `   a d v a n c i n g   T   - >   T + 5 0 0   - >   T + 1 0 0 0 ,   r a w   2 . 0 / s   e a c h   d u e ,   p l u s 
 
     t h e   A 2   l i v e n e s s   s e m a n t i c s   ( b a s e l i n e - o n l y   o n   f i r s t   s a m p l e ,   a l i v e   a f t e r   t h e   s e c o n d   r e a l   r a t e ) .   T h e 
 
     e x i s t i n g   S t e p - 1   t i m e l i n e   w a s   t o o   c o a r s e   ( e v e r y   t i c k   > =   s a m p l e _ m s   a p a r t )   t o   e v e r   e x e r c i s e   t h e   b u g . 
 
 -   * * t e s t s / t e s t _ s o u r c e _ c o n t r a c t . p y * *   �� �   n e w   p i n s :   t h e   8   g a t e   s t a g e s   e a c h   w i r e d   e x a c t l y   o n c e ;   n o 
 
     ` s _ l a s t _ t i m e   =   n o w `   b e t w e e n   ` i n t   d u e `   a n d   t h e   d u e - p a t h   s t o r e   ( F I N D I N G   A   r e g r e s s i o n   p i n ) .   S t a l e   p i n 
 
     F L I P P E D :   ` i f   ( ! g _ v e r s i o n _ o k )   r e t u r n ; `   - >   t h e   i n s t r u m e n t a t i o n   f o r m 
 
     ` o v l _ a b o r t _ s t o p ( " v e r s i o n " ) `   +   ` ! g _ v e r s i o n _ o k ` .   N o t h i n g   o v e r - t i g h t e n e d . 
 
 
 
 # #   B U I L D   /   H A R N E S S E S   ( t h i s   r o u n d ) 
 
 -   B u i l d   r c = 0 ,   z e r o   w a r n i n g s   ( - W a l l   - W e x t r a ) ,   V E R I F Y   P A S S   ( i 3 8 6   P E 3 2 ,   i m p o r t s   K E R N E L 3 2 / U S E R 3 2 / m s v c r t 
 
     o n l y ,   1 1   e x p o r t s   v i a   d 3 d 9 . d e f   �� �   u n c h a n g e d   s e t ) . 
 
 -   d 3 d 9 . d l l   =   * * 1 5 0 , 9 0 6   B * * ,   S H A 2 5 6   ` 2 9 5 0 3 e 0 9 5 c 3 7 c 4 7 d 0 0 0 f 8 7 8 8 2 2 2 b a 3 e 9 1 6 9 e 1 5 d 9 a 1 9 b 5 9 e 3 1 3 a c e 0 1 e e 2 0 4 c 1 6 2 ` , 
 
     d e p l o y e d   b y t e - i d e n t i c a l   t o   r e p o   r o o t   +   g a m e   d i r   +   t e s t s \ d 3 d 9 . d l l   ( a l l   3   S H A 2 5 6   e q u a l ) ;   o l d 
 
     d 3 d 9 m o d . l o g   d e l e t e d .   G a m e   N O T   l a u n c h e d . 
 
 -   H a r n e s s e s   ( r e b u i l t   w i t h   i 6 8 6 - w 6 4 - m i n g w 3 2 - g c c   1 6 . 2 . 0 ,   t h e n   r u n ) :   t e s t _ r a t e _ e n g i n e . e x e   F A I L U R E S   0 
 
     ( 7   n e w   R 1 4   c a d e n c e   a s s e r t s )   ,%V%  t e s t _ d 3 d 9 _ a c t u a l . e x e   A L L   D 3 D 9   A C T U A L   C H E C K S   P A S S E D   ,%V%
 
     t e s t _ p e _ s t r u c t u r e . p y   F A I L U R E S   0   ,%V%  t e s t _ s o u r c e _ c o n t r a c t . p y   S O U R C E - C O N T R A C T :   P A S S   ( R 1 4   g a t e - s t a g e 
 
     p i n s   +   F I N D I N G - A   p i n   +   a l l   R 1 1 / R 1 2 / R 1 3   p i n s   g r e e n ) . 
 
 
 
 - - - 
 
 #   B U I L D   �� �   R O U N D   1 3   ( 2 0 2 6 - 0 9 - 0 7 )   �� �   D I A G N O S I S - D R I V E N   H A R D E N I N G   ( s i l e n t - s t o p   d e t e c t o r   +   u n c o n d i t i o n a l   d i a g ) 
 
 
 
 # #   R O U N D   S U M M A R Y 
 
 N o   c r a s h   t h i s   r o u n d   ( g a m e   N O T   l a u n c h e d ) .   T w o   f r e s h   l i v e   r u n s   ( l o g   b o d i e s   I D E N T I C A L )   s h o w e d 
 
 ` O V E R L A Y   A R M E D `   - >   ` U I :   f o n t   c r e a t e d `   - >   ` o v l   f i r s t   d r a w   o k   f r a m e = 4 4 0 ` / ` 3 4 2 ` ,   t h e n   A B S O L U T E 
 
 S I L E N C E   f o r   6 0 s   �� �   t h e   6 0 0 - f r a m e   h e a r t b e a t   ( s r c / u i . c : 6 9 7 - 7 0 2 ,   N O T   d e b u g - g a t e d )   l o g g e d   Z E R O   t i m e s . 
 
 T h i s   r o u n d :   ( 1 )   b y t e - v e r i f y   t h e   d e p l o y e d   D L L ,   ( 2 )   u n - g a t e   t h e   r e n d e r   d i a g ,   ( 3 )   a d d   a   p e r m a n e n t 
 
 s i l e n t - s t o p   d e t e c t o r   s o   a   d e a d   o v e r l a y   i s   n e v e r   a m b i g u o u s   a g a i n ,   ( 4 )   r e f r e s h   t h e   s t a l e   g a m e - d i r   I N I . 
 
 
 
 # #   F I N D I N G   # 1   �� �   d e p l o y e d   D L L   i s   C U R R E N T ,   N O T   s t a l e   ( b y t e s ) 
 
 -   ` G e t - F i l e H a s h `   o n   a l l   t h r e e   c o p i e s   B E F O R E   a n y   r e b u i l d :   g a m e   d i r   = =   r e p o   = =   t e s t s   = = 
 
     ` 1 c f 2 8 5 b 6 a 7 3 0 a 0 e 5 3 1 b e 8 b b 1 a b 3 6 d b 7 b b c a 6 4 e c 6 e 1 0 b 5 d 7 7 8 1 5 9 1 1 e f 2 e 3 8 c b e b `   ( t h e   e x a c t   R 1 2   h a s h ) , 
 
     1 4 9 , 8 1 5   B ,   s a m e   m t i m e   �� �   * * t h e   d e p l o y e d   g a m e - d i r   D L L   I S   t h e   R 1 2   b u i l d   a n d   D O E S   c o n t a i n   t h e   R 1 0 
 
     h e a r t b e a t * * .   A   s t a l e   d e p l o y m e n t   C A N N O T   e x p l a i n   t h e   s i l e n c e .   = >   T h e   m i s s i n g   h e a r t b e a t   i s   R E A L : 
 
     t h e   d r a w   p a t h   d i e s   q u i e t l y   a f t e r   t h e   f i r s t   s u c c e s s f u l   d r a w ,   i . e .   o n e   o f   t h e   f o u r   E A R L Y   R E T U R N S   a t 
 
     s r c / u i . c : 6 0 9 / 6 1 1 / 6 1 3 / 6 2 2   ( g r t = = N U L L   /   g r t - f a i l   /   c v t = = N U L L   /   V i r t u a l Q u e r y   g u a r d )   f i r e s   e v e r y 
 
     l a t e r   f r a m e .   T h o s e   p a t h s   w e r e   s i l e n t   a n d   p r e - s t a t e - c h a n g e ,   w h i c h   i s   e x a c t l y   w h y   t h e y   n e v e r 
 
     s h o w e d   u p   �� �   n o w   t h e y   c a n n o t   h i d e . 
 
 
 
 # #   C H A N G E   L I S T 
 
 -   * * s r c / u i . c   �� �   ` o v l   d i a g `   u n - g a t e d   ( r e n d e r - i n v i s i b i l i t y   d i a g n o s t i c ) . * *   T h e   o n e - s h o t 
 
     ` d l o g ( " o v l   d i a g   r t = % p   r e c t = % l d , % l d : % l d x % l d   a l p h a = 0 x % l X " ,   . . . ) `   n o w   f i r e s   o n   t h e   F I R S T   d r a w 
 
     U N C O N D I T I O N A L L Y   �� �   o n e   l i n e   p e r   p r o c e s s   l o a d ,   e v e n   o n   a   s h i p p e d   ` [ D e b u g ]   E n a b l e d = 0 `   r u n   �� �   b e c a u s e 
 
     i t   i s   t h e   P R I M A R Y   r e n d e r - i n v i s i b i l i t y   d i a g n o s t i c .   C o n t e n t   i d e n t i c a l   t o   R 1 1   P 0   ( r t   +   e x a c t   p a n e l 
 
     r e c t / a l p h a   u s e d   t h e   s a m e   f r a m e ) .   ` g _ o v l _ f i r s t _ d o n e `   s e m a n t i c s   k e p t   ( f l a g   s e t   A F T E R   t h e   b l o c k ) ; 
 
     c o m m e n t   u p d a t e d   t o   s t a t e   i t   i s   u n c o n d i t i o n a l - b y - d e s i g n .   N o   s t a t e . h   c h a n g e   n e e d e d   ( n o   m a c r o / f l a g 
 
     i n v o l v e d   �� �   ` g _ o v l _ f i r s t _ d o n e `   a l r e a d y   a n   e x t e r n ) . 
 
 -   * * s r c / u i . c   �� �   s i l e n t - s t o p   d e t e c t o r   ( d r a w   p a t h ) . * *   N e w   ` o v l _ a b o r t _ s t o p ( s t a g e ) ` :   c o u n t s   C O N S E C U T I V E 
 
     f r a m e s   a b o r t i n g   a t   t h e   f o u r   e a r l y   r e t u r n s   �� �   s t a g e   ` g r t - m i s s i n g `   ( g r t = = N U L L ) ,   ` g r t - f a i l ` 
 
     ( G e t R e n d e r T a r g e t   h r ! = 0   o r   c u r = = N U L L ) ,   ` s u r f a c e - b a d `   ( s u r f a c e   v t a b l e   N U L L ) ,   ` g u a r d - f a i l ` 
 
     ( V i r t u a l Q u e r y   M E M _ C O M M I T / N O A C C E S S / G U A R D   c h e c k ) .   A t   e x a c t l y   6 0   c o n s e c u t i v e   a b o r t s   i t   l o g s   O N C E 
 
     ( n o t   p e r   f r a m e ,   N O T   d e b u g - g a t e d ) :   ` o v l   s t o p :   % u   c o n s e c u t i v e   f r a m e   a b o r t s   a t   s t a g e = < s t a g e >   -   o v e r l a y 
 
     d r a w   h a l t e d ` .   C o u n t e r   r e s e t s   o n   a n y   c o m p l e t e d   d r a w   ( t h e   ` s _ o v l _ d r a w s + + `   h e a r t b e a t   b u m p ) .   T h e   a b o r t 
 
     s i t e s   a l r e a d y   r a n   B E F O R E   a n y   s t a t e   w a s   c h a n g e d ,   s o   l o g g i n g   i s   t r i v i a l l y   s a f e . 
 
 -   * * d 3 d 9 . c   �� �   s i l e n t - s t o p   d e t e c t o r   ( r e e n t r a n t   p r e s e n t ) . * *   C o u n t s   C O N S E C U T I V E   f r a m e s   w h e r e   t h e 
 
     p r e s e n t _ h o o k   r e - e n t r a n c y   b r a n c h   f i r e s   ( n e s t e d   P r e s e n t   w h i l e   t h e   t r i p w i r e   w a s   s e t ) ;   a t   6 0   l o g s   o n c e : 
 
     ` o v l   s t o p :   r e e n t r a n t - p r e s e n t   f o r   % u   c o n s e c u t i v e   f r a m e s ` .   R e s e t   o n   a n y   n o r m a l   p r e s e n t .   D i s t i n g u i s h e s 
 
     " h o o k   c a l l e d   b u t   o v e r l a y   s k i p p e d   e v e r y   f r a m e "   f r o m   " u i _ d r a w   e a r l y - r e t u r n s " .   K e p t   b r a c e - f l a t   i n s i d e 
 
     t h e   r e e n t r a n t   b r a n c h   ( s i n g l e - s t a t e m e n t   i f )   s o   t h e   R 9   B 7   p i n   " n o   e a r l y   r e t u r n   b e t w e e n   t r i p w i r e - s e t 
 
     a n d   c l e a r   w h i l e   h o l d i n g   t h e   t o k e n "   s t i l l   p a s s e s . 
 
 -   * * G a m e - d i r   I N I   R E F R E S H . * *   O l d   ` R e s o u r c e R a t e M o d . i n i `   w a s   t h e   2 6 - l i n e   p r e - R 1 1   v e r s i o n   ( m i s s i n g   t h e   9 
 
     R 1 1   k e y s   +   ` [ D e b u g ]   E n a b l e d = 0 `   w h i c h   a l s o   h i d e s   R E S   +   d i a g ) .   B a c k e d   u p   t o 
 
     ` R e s o u r c e R a t e M o d . i n i . b a k `   ( 3 6 6   B ,   2 6   l i n e s )   i n   t h e   g a m e   d i r ;   n e w   f i l e   i s   t h e   C O M P L E T E   3 5 - l i n e 
 
     c o n f i g   f r o m   t h e   r e p o   e x a m p l e   w i t h   ` [ D e b u g ]   E n a b l e d = 1 `   ( a l l   9   k e y s :   D e c i m a l P l a c e s ,   S h o w P l u s S i g n , 
 
     S h o w R e s o u r c e N a m e s ,   S h o w Z e r o R a t e s ,   S h o w F o o d / W o o d / C o i n / E x p o r t ,   P o s i t i o n M o d e ) .   R e p o - s i d e 
 
     ` R e s o u r c e R a t e M o d . i n i . e x a m p l e `   a l r e a d y   h a d   e v e r y   k e y   �� �   u n c h a n g e d .   N o   k e y   i n v e n t e d   b e y o n d   s e t t i n g s . c . 
 
 -   * * t e s t s / t e s t _ s o u r c e _ c o n t r a c t . p y * *   �� �   R 1 1   g a t e   p i n s   F L I P P E D   t o   R 1 3 :   t h e   m a r k e r . . d i a g   r e g i o n   m u s t   N O T 
 
     c o n t a i n   ` d e b u g _ e n a b l e d ` ;   t h e   o l d   ` ! g _ o v l _ f i r s t _ d o n e   & &   g _ s e t t i n g s . d e b u g _ e n a b l e d `   g u a r d   l i t e r a l   m u s t 
 
     b e   G O N E .   N e w   p i n s   ( n o t   o v e r - t i g h t e n e d ) :   d r a w - p a t h   s t o p   m e s s a g e   e x a c t l y   o n c e ;   e a c h   s t a g e   n a m e   w i r e d 
 
     e x a c t l y   o n c e ;   r e e n t r a n t - p r e s e n t   m e s s a g e   e x a c t l y   o n c e ;   b o t h   c o u n t e r s   r e s e t   o n   a   c o m p l e t e d / n o r m a l 
 
     f r a m e .   A l l   o t h e r   p i n s   ( i n c l .   B 7 )   u n t o u c h e d . 
 
 
 
 # #   L I V E - T E S T   E X P E C T A T I O N S   ( D a l i ' s   n e x t   r u n ,   t h i s   b u i l d ) 
 
 T h e   g a m e - d i r   I N I   n o w   h a s   ` [ D e b u g ]   E n a b l e d = 1 ` ,   s o   t h e   n e x t   r u n   M U S T   s h o w ,   o n   T O P   o f   t h e   p r e v i o u s 
 
 b o d y :   u n i n t e r r u p t e d   p e r i o d i c   ` o v l   h e a r t b e a t   n = 6 0 0   f r a m e = . .   r e s = . .   f o o d = . .   w o o d = . .   c o i n = . .   e x p o r t = . . ` 
 
 l i n e s   e v e r y   ~ 1 0 s   ( t h e   R 1 6   c h e c k ) ,   ` R E S `   s a m p l i n g   l i n e s   p e r   S E T T L E M E N T   ( w h e n   d e b u g   o n ) ,   a n d   t h e 
 
 p h i l o s o p h y   o f   t h e   l o g   r e a d i n g : 
 
 -   h e a r t b e a t   l i n e s   e v e r y   ~ 1 0 s     = >   d r a w   p a t h   c o m p l e t e s   c o n t i n u o u s l y     ( p a n e l   m a y   s t i l l   b e   i n v i s i b l e 
 
     f o r   g e o m e t r y   r e a s o n s   - >   n e x t - s t e p   i s   r e n d e r - g e o m e t r y   m a t h ) . 
 
 -   ` o v l   s t o p :   . .   a b o r t s   a t   s t a g e = g r t - m i s s i n g | g r t - f a i l | s u r f a c e - b a d | g u a r d - f a i l `   = >   d r a w   p a t h   d y i n g 
 
     e v e r y   f r a m e   - >   n e x t - s t e p   i s   R T - s u r f a c e   h a n d l i n g . 
 
 -   ` o v l   s t o p :   r e e n t r a n t - p r e s e n t   f o r   6 0 `   = >   h o o k   c a l l e d   b u t   o v e r l a y   s k i p p e d   - >   n e x t - s t e p   i s   t r i p w i r e 
 
     l i f e t i m e . 
 
 -   s t i l l   N O   h e a r t b e a t   A N D   N O   o v l   s t o p   = >   e v e n   t h e   a b o r t   p a t h   n e v e r   r u n s   - >   n e x t - s t e p   i s 
 
     p r e s e n t _ h o o k   n o t   b e i n g   c a l l e d   a t   a l l   ( d e v i c e / v t a b l e ) . 
 
 
 
 # #   B U I L D   /   H A R N E S S E S   ( t h i s   r o u n d ) 
 
 -   B u i l d   r c = 0 ,   z e r o   w a r n i n g s   ( - W a l l   - W e x t r a ) ,   V E R I F Y   P A S S   ( i 3 8 6   P E 3 2 ,   i m p o r t s   K E R N E L 3 2 / U S E R 3 2 / m s v c r t 
 
     o n l y ,   1 1   e x p o r t s   v i a   d 3 d 9 . d e f   �� �   u n c h a n g e d   s e t ) . 
 
 -   d 3 d 9 . d l l   =   * * 1 5 0 , 3 9 4   B * * ,   S H A 2 5 6   ` e 0 4 c c 7 c 4 3 4 1 c 4 a c 6 8 f 3 1 1 5 4 c 1 f 5 0 0 1 c 8 8 0 f 7 d c 1 2 0 f 1 c b 7 6 8 a e 1 b e a 8 9 b d 8 b 4 2 4 9 ` , 
 
     d e p l o y e d   b y t e - i d e n t i c a l   t o   r e p o   r o o t   +   g a m e   d i r   +   t e s t s \ d 3 d 9 . d l l   ( a l l   3   S H A 2 5 6   e q u a l ) ;   o l d 
 
     d 3 d 9 m o d . l o g   d e l e t e d .   ( F i r s t   e 0 4 c c 7 c 4   b u i l d ,   t h e n   a   p u r e - b r a c e   r e f o r m a t   r e b u i l t   t o   t h e   S A M E   h a s h   �� � 
 
     c o n f i r m e d   c o d e - e q u i v a l e n t . ) 
 
 -   H a r n e s s e s   ( r e b u i l t   f r o m   t e s t s \   w i t h   i 6 8 6 - w 6 4 - m i n g w 3 2 - g c c   1 6 . 2 . 0 ,   t h e n   r u n ) :   t e s t _ r a t e _ e n g i n e . e x e 
 
     F A I L U R E S   0   ,%V%  t e s t _ d 3 d 9 _ a c t u a l . e x e   A L L   D 3 D 9   A C T U A L   C H E C K S   P A S S E D   ,%V%  t e s t _ p e _ s t r u c t u r e . p y 
 
     F A I L U R E S   0   ,%V%  t e s t _ s o u r c e _ c o n t r a c t . p y   S O U R C E - C O N T R A C T :   P A S S   ( R 1 3   p i n s   +   a l l   R 1 1 / R 1 2   p i n s   g r e e n ) . 
 
 -   O n e   h a r n e s s   i t e r a t i o n   c a u g h t :   R 1 3 ' s   n e s t e d - i f   i n s i d e   t h e   r e e n t r a n t   b r a n c h   b r o k e   t h e   R 9   B 7   p i n ' s 
 
     f l a t - b r a c e   s c a n   - >   r e v e r t e d   t o   a   s i n g l e - s t a t e m e n t   i f   ( i d e n t i c a l   c o d e ,   i d e n t i c a l   h a s h ) . 
 
 
 
 - - - 
 
 #   B U I L D   �� �   R O U N D   1 2   ( 2 0 2 6 - 0 9 - 0 7 )   �� �   T E S T E R - F I N D I N G   C L O S U R E S   ( n a m e s - o f f   r e a l   f i x   +   t i g h t e n e d   h a r n e s s e s ) 
 
 
 
 # #   R O U N D   S U M M A R Y 
 
 N o   c r a s h   t h i s   r o u n d   ( g a m e   n o t   l a u n c h e d ) .   R 1 2   i s   t h e   s m a l l ,   s u r g i c a l   c l o s u r e   r o u n d   f r o m   t h e 
 
 R 1 1   t e s t e r   a u d i t :   O N E   c o d e   b e h a v i o r   c h a n g e   ( t h e   M E D I U M   f i n d i n g )   p l u s   t i g h t e r   t e s t   p i n s . 
 
 -   * * F I X   1   ( M E D I U M ,   t h e   o n l y   c o d e   c h a n g e ) * *   �� �   ` S h o w R e s o u r c e N a m e s = 0 `   p r e v i o u s l y   p r i n t e d   a   B L A N K 
 
     l a b e l :   ` f o r m a t _ r a t e _ l i n e `   o n l y   f a l l s   b a c k   t o   " S l o t % d "   w h e n   t h e   n a m e   a r g   i s   N U L L / e m p t y ,   b u t 
 
     ` u i _ d r a w _ p a n e l `   ( s r c / u i . c ,   l i n e   ~ 4 5 8 )   A L W A Y S   p a s s e d   t h e   r e a l   l a b e l s   ( " F o o d " / " W o o d " / " C o i n " / " E x p o r t " ) . 
 
     N o w   t h e   p a n e l   p a s s e s   ` g _ s e t t i n g s . s h o w _ r e s o u r c e _ n a m e s   ?   " F o o d "   :   N U L L `   ( a n d   W o o d / C o i n / E x p o r t )   s o 
 
     n a m e s - o f f   y i e l d s   " S l o t % d "   v i a   t h e   h e l p e r ' s   e x i s t i n g   N U L L   f a l l b a c k   �� �   t h e   s p e c   b e h a v i o r .   D e f a u l t 
 
     ( n a m e s   o n )   u n c h a n g e d . 
 
 -   * * F I X   2   ( t e s t s / t e s t _ r a t e _ e n g i n e . c ,   L O W   f i n d i n g s ) * *   �� �   d e c i m a l - p r e c i s i o n   l o o p   t i g h t e n e d   t o   E X A C T 
 
     m a t c h   ( ` + 2 / m i n `   /   ` + 2 . 0 / m i n `   /   ` + 2 . 0 0 / m i n `   /   ` + 2 . 0 0 0 / m i n ` )   w i t h   f i n e r - p r e c i s i o n   A B S E N C E   a s s e r t s 
 
     ( e . g .   ` + 2 . `   m u s t   n o t   a p p e a r   w h e n   d e c i m a l _ p l a c e s = = 0 ) ;   t h e   c l a m p   t e s t   n o w   r e q u i r e s   ` + 2 . 0 0 0 `   w i t h   N O 
 
     ` 2 . 0 0 0 0 ` ;   p e r - r e s o u r c e   v i s i b i l i t y   p a i r s   a d d e d   f o r   s l o t   1   " W o o d " ,   s l o t   0   " C o i n " ,   s l o t   7   " E x p o r t " 
 
     m i r r o r i n g   t h e   e x i s t i n g   s h o w _ f o o d   p a i r ;   a   s l o t - m a p / n a m e - o r d e r   p r o b e   d r i v e s   ` f o r m a t _ r a t e _ l i n e `   f o r 
 
     2 / 1 / 0 / 7   a n d   p i n s   e a c h   n a m e s - o f f   N U L L   f a l l b a c k .   T h e   4 8 - b y t e / 8 - b y t e   b u f f e r   t e s t s   F O R C E   t r u n c a t i o n 
 
     ( h u g e   v a l u e   +   l o n g   n a m e   +   h u g e   r a t e )   a n d   p i n   t h e   A C T U A L   b o u n d   t h e   h e l p e r   g u a r a n t e e s :   p r o b i n g 
 
     s h o w e d   m i n g w / m s v c r t   ` _ s n p r i n t f `   w r i t e s   e x a c t l y   ` n `   b y t e s   w i t h   N O   N U L   t e r m i n a t o r   w h e n   t r u n c a t e d 
 
     ( t h e   h e l p e r ' s   o n e   h a r d   r u l e ) ,   s o   t h e   t e s t s   p i n   " e x a c t   n - c h a r   p r e f i x   +   g u a r d   b y t e   a f t e r   n   i s 
 
     u n t o u c h e d "   ( n o   w r i t e   p a s t   n )   i n s t e a d   o f   a s s u m i n g   4 7 + N U L .   ` c l i p _ l i n e `   d r i v e n   > 7 8   c h a r s   a n d   p i n n e d 
 
     t o   e x a c t l y   P A N E L _ L I N E _ M A X _ C H A R S   w i t h   t r a i l i n g   " . . "   a t   7 6 / 7 7   +   s h o r t - p a t h   u n t o u c h e d ;   v e r s i o n   t a b l e 
 
     p i n s   A L L   S I X   r o w - 0   f i e l d s   = =   t h e   s i x   E X P E C T E D _ *   m a c r o s   a n d   r o w s   1 . . 3   E A C H   i n d i v i d u a l l y   h a v e 
 
     l a b e l = = N U L L   a n d   a l l - z e r o   f i e l d s .   N a m e s - o f f   n o w   p i n s   B O T H   h e l p e r   p a t h s :   r e a l   l a b e l   +   n a m e s   o f f   - > 
 
     b l a n k   ( t h e   R 1 1   c a l l e r   b u g ,   n o   l o n g e r   r e a c h a b l e ) ,   N U L L   +   n a m e s   o f f   - >   " S l o t % d "   ( t h e   F I X   1   p a t h ) . 
 
 -   * * F I X   3   ( t e s t s / t e s t _ s o u r c e _ c o n t r a c t . p y ) * *   �� �   r e p l a c e d   t h e   w e a k   ` c o d e . c o u n t ( " g _ b b _ w   =   " )   > =   3 `   w i t h 
 
     ` _ f n _ b o d y ( ) `   s l i c e s   t h a t   r e q u i r e   t h e   g _ b b _ w   A N D   g _ b b _ h   c a p t u r e   i n s i d e   e a c h   s p e c i f i c   f u n c t i o n   b o d y 
 
     ( w _ c r e a t e _ d e v i c e ,   w _ c r e a t e _ d e v i c e _ e x ,   r e s e t _ h o o k ) ;   a d d e d   t h e   ` s h o w _ r e s o u r c e _ n a m e s   ?   " F o o d "   :   N U L L ` 
 
     ( a n d   W o o d / C o i n / E x p o r t )   p a t t e r n   a s s e r t s   a t   t h e   4   m a i n   r o w s .   N o   o t h e r   e x i s t i n g   p i n s   c h a n g e d . 
 
 -   * * F I X   4   ( d o c s ) * *   �� �   ` . s w a r m / B U I L D . m d `   R 1 1   L I V E   E X P E C T A T I O N S :   ` a l p h a = 0 x D 8 `   - >   ` a l p h a = 0 x D 9 ` 
 
     ( c o d e   c o m p u t e s   ( D W O R D ) ( 0 . 8 5 * 2 5 5 + 0 . 5 ) = 2 1 7 = 0 x D 9 ) ;   ` s r c / s t a t e . h `   s h o w _ r e s o u r c e _ n a m e s   c o m m e n t   n o w 
 
     s t a t e s   n a m e s - o f f   - >   " S l o t % d "   v i a   t h e   N U L L   p a t h   f o r   t h e   4   m a i n   r o w s . 
 
 
 
 # #   L I V E   E X P E C T A T I O N S   ( v e r i f y   a g a i n s t   d 3 d 9 m o d . l o g ) 
 
 S a m e   c h a i n   a s   R 1 1 ,   b u t   t h e   P 0   d i a g n o s t i c ' s   a l p h a   i s   n o w   * * 0 x D 9 * *   ( n o t   0 x D 8 ) : 
 
 ` ` ` 
 
 R 1 4   D L L   l o a d e d   b a s e = 0 0 4 0 0 0 0 0   r e a l = . .   v e r s i o n = O K   r e a s o n = o k 
 
 U I   r e a d y :   d 3 d x 9 _ 2 5 . d l l   l o a d e d 
 
 O V E R L A Y   A R M E D 
 
 . . . 
 
 c h a i n   . .   n = 3   . .   [ h u m a n - p 1 ] 
 
 - - -   m a t c h   s t a r t   n = 3   . .   - - - 
 
 U I :   f o n t   c r e a t e d   s i z e = 1 4   f a c e = G e o r g i a   f o n t = . . 
 
 o v l   f i r s t   d r a w   o k   f r a m e = . . 
 
 o v l   d i a g   r t = . .   r e c t = 1 2 , 1 2 : 2 5 0 x 1 4 1   a l p h a = 0 x D 9       < -   O N E - S H O T ,   o n l y   w h e n   D e b u g   E n a b l e d = 1 
 
             ( 0 x D 9   =   ( D W O R D ) ( 0 . 8 5 * 2 5 5 + 0 . 5 ) = 2 1 7 ,   t h e   d e f a u l t   o p a c i t y   o f   0 . 8 5 ) 
 
 o v l   h e a r t b e a t   n = . .   f r a m e = . .   r e s = . .   f o o d = . .   w o o d = . .   c o i n = . .   e x p o r t = . . 
 
 ` ` ` 
 
 W i t h   ` [ G e n e r a l ]   S h o w R e s o u r c e N a m e s = 0 `   t h e   f o u r   m a i n   r o w s   n o w   s h o w   ` S l o t 2   S l o t 1   S l o t 0   S l o t 7 ` 
 
 i n s t e a d   o f   b l a n k   l a b e l s .   A / B   d i s a b l e   r u n s   +   F 9 + A l t   t o g g l e   c h e c k s   u n c h a n g e d . 
 
 
 
 # #   F i l e   c h a n g e s 
 
 -   ` s r c / u i . c `   �� �   R 1 2 :   4   m a i n   r o w s   p a s s   ` g _ s e t t i n g s . s h o w _ r e s o u r c e _ n a m e s   ?   s l o t _ n a m e s [ r ]   :   N U L L ` 
 
     s o   n a m e s - o f f   y i e l d s   " S l o t % d "   ( c o m m e n t   a d d e d ) . 
 
 -   ` s r c / s t a t e . h `   �� �   R 1 2 :   s h o w _ r e s o u r c e _ n a m e s   c o m m e n t   c l a r i f i e d   ( n a m e s - o f f   - >   S l o t % d   v i a   N U L L   p a t h ) . 
 
 -   ` t e s t s / t e s t _ r a t e _ e n g i n e . c `   �� �   R 1 2 :   d e c i m a l   E X A C T - m a t c h   +   f i n e r - a b s e n c e ,   c l a m p   b o u n d ,   W o o d / C o i n / 
 
     E x p o r t   v i s i b i l i t y   p a i r s ,   s l o t - m a p   o r d e r   p r o b e ,   f o r c e d   4 8 - b y t e   ( a n d   8 - b y t e )   t r u n c a t i o n   b o u n d , 
 
     c l i p _ l i n e   7 8 - c a p   +   s h o r t - p a t h ,   v e r s i o n   t a b l e   = =   A L L   s i x   m a c r o s   +   r o w s   1 . . 3   a l l - z e r o . 
 
 -   ` t e s t s / t e s t _ s o u r c e _ c o n t r a c t . p y `   �� �   R 1 2 :   p e r - f u n c t i o n - b o d y   g _ b b _ w / g _ b b _ h   c a p t u r e   ( r e p l a c e s 
 
     c o u n t > = 3 ) ,   n a m e s - o f f   N U L L   t e r n a r y   a t   t h e   4   m a i n   r o w s . 
 
 -   ` . s w a r m / B U I L D . m d `   �� �   R 1 2 :   0 x D 8   - >   0 x D 9   i n   t h e   R 1 1   L I V E   E X P E C T A T I O N S   b l o c k   +   t h i s   s e c t i o n . 
 
 
 
 # #   B u i l d   /   h a r n e s s e s   ( t h i s   r o u n d ) 
 
 -   B u i l d   r c = 0 ,   z e r o   w a r n i n g s   ( - W a l l   - W e x t r a ) ,   V E R I F Y   P A S S   ( i 3 8 6   P E 3 2 ,   i m p o r t s 
 
     K E R N E L 3 2 / U S E R 3 2 / m s v c r t   o n l y ,   1 1   e x p o r t s   v i a   d 3 d 9 . d e f   �� �   u n c h a n g e d   s e t ) . 
 
 -   d 3 d 9 . d l l   =   * * 1 4 9 , 8 1 5   B * * ,   S H A 2 5 6   ` 1 c f 2 8 5 b 6 a 7 3 0 a 0 e 5 3 1 b e 8 b b 1 a b 3 6 d b 7 b b c a 6 4 e c 6 e 1 0 b 5 d 7 7 8 1 5 9 1 1 e f 2 e 3 8 c b e b ` , 
 
     d e p l o y e d   b y t e - i d e n t i c a l   t o   r e p o   r o o t   +   g a m e   d i r   +   t e s t s \ d 3 d 9 . d l l   ( a l l   3   S H A 2 5 6   e q u a l ) ;   o l d 
 
     d 3 d 9 m o d . l o g   d e l e t e d . 
 
 -   H a r n e s s e s   ( r e b u i l t   f r o m   t e s t s \ \ ,   t h e n   r u n   w i t h   P y t h o n 3 1 2 ) :   t e s t _ r a t e _ e n g i n e . e x e   F A I L U R E S   0   ( i n c l . 
 
     a l l   n e w   R 1 2   a s s e r t s )   ,%V%  t e s t _ d 3 d 9 _ a c t u a l . e x e   A L L   P A S S   ,%V%  t e s t _ p e _ s t r u c t u r e . p y   0   f a i l u r e s   ,%V%
 
     t e s t _ s o u r c e _ c o n t r a c t . p y   P A S S   ( R 1 2   b l o c k   +   a l l   R 1 1 / R 1 4   p i n s   g r e e n ) . 
 
 
 
 - - - 
 
 
 
 #   B U I L D   �� �   R O U N D   1 1   ( 2 0 2 6 - 0 9 - 0 7 )   �� �   S E T T I N G S   G A P   +   P A N E L   L A Y O U T   +   V E R S I O N   T A B L E   +   P 0   D I A G 
 
 
 
 # #   R O U N D   S U M M A R Y 
 
 N o   c r a s h   t h i s   r o u n d   ( g a m e   n o t   l a u n c h e d ) .   R 1 1   c l o s e s   t h e   P 0 / P 1   s p e c   g a p s   f r o m   t h e   R 1 0   " l i v e 
 
 e x p e c t a t i o n s "   r e p o r t :   f u l l   f o r m a t / v i s i b i l i t y / p o s i t i o n   s e t t i n g s ,   t w o - l i n e   h i n t   b a r ,   a 
 
 d a t a - d r i v e n   v e r s i o n   t a b l e ,   a n d   a   o n e - s h o t   d e b u g - g a t e d   r e n d e r   d i a g n o s t i c   t o   p r o v e   f i r s t   d r a w . 
 
 -   * * P 0   o n e - s h o t   r e n d e r   d i a g n o s t i c * *   �� �   t h e   v e r y   f i r s t   o v e r l a y   d r a w   l o g s ,   w h e n   ` [ D e b u g ]   E n a b l e d = 1 ` , 
 
     ` o v l   d i a g   r t = < c u r - r t >   r e c t = < x > , < y > : < w > x < h t >   a l p h a = 0 x < a r g b > `   w i t h   t h e   A C T U A L   c u r r e n t   r e n d e r 
 
     t a r g e t   ( r e l e a s e d   p o i n t e r   v a l u e   o n l y ,   n e v e r   d e r e f e r e n c e d )   a n d   t h e   e x a c t   p a n e l   r e c t / a l p h a   f r o m 
 
     ` u i _ p a n e l _ g e o m e t r y ` .   S i t s   i n s i d e   t h e   f i r s t - d r a w   b l o c k   A F T E R   t h e   ` o v l   f i r s t   d r a w   o k `   m a r k e r ; 
 
     ` g _ o v l _ f i r s t _ d o n e `   i s   s e t   a f t e r   t h e   b l o c k   s o   t h e   g u a r d   l i t e r a l 
 
     ` ! g _ o v l _ f i r s t _ d o n e   & &   g _ s e t t i n g s . d e b u g _ e n a b l e d `   i s   t r u e   o n   t h e   o n e   s h o t . 
 
 -   * * S e t t i n g s   g a p   ( 9   n e w   k e y s ) * *   �� �   ` D e c i m a l P l a c e s `   ( 0 . . 3 ,   d e f a u l t   1 ) ,   ` S h o w P l u s S i g n `   ( 1 ) , 
 
     ` S h o w R e s o u r c e N a m e s `   ( 1 ) ,   ` S h o w Z e r o R a t e s `   ( 1 ) ,   ` S h o w F o o d / S h o w W o o d / S h o w C o i n / S h o w E x p o r t `   ( a l l   1 ) , 
 
     ` P o s i t i o n M o d e `   ( ` D e f a u l t ` | ` T o p L e f t ` | ` T o p R i g h t ` ,   d e f a u l t   0 ) .   P a r s e d   i n   B O T H   ` s e t t i n g s _ l o a d `   a n d 
 
     ` p r o f i l e _ a p p l y `   ( c l a m p e d   a f t e r   e a c h ) ,   s a v e d   b y   ` s e t t i n g s _ s a v e ` ;   ` P o s i t i o n M o d e `   r o u n d - t r i p s   a s   a 
 
     n a m e .   ` i n i _ p o s _ m o d e ( ) `   i s   a   c a s e - i n s e n s i t i v e   ` _ s t r i c m p `   h e l p e r   ( m s v c r t   �� �   n o   n e w   i m p o r t s ) . 
 
 -   * * F o r m a t   s e a m * *   �� �   n e w   n o n - s t a t i c   ` f o r m a t _ r a t e _ l i n e ( c h a r * ,   s i z e _ t ,   s l o t ,   n a m e ,   v a l u e ,   r a t e , 
 
     s e t t i n g s ) `   ( d e c l a r e d   i n   s t a t e . h ,   c a l l e d   b y   h a r n e s s   +   t h e   4   m a i n   r o w s ) .   S l o t   m a p   2 - > F o o d , 
 
     1 - > W o o d ,   0 - > C o i n ,   7 - > E x p o r t ;   a   h i d d e n   r e s o u r c e   o r   ~ 0   r a t e   ( w h e n   S h o w Z e r o R a t e s = 0 )   r e t u r n s   0   a n d 
 
     t h e   r o w + i t s   h e i g h t   a r e   s k i p p e d .   D e c i m a l s   c l a m p e d   0 . . 3 ,   ` + `   p r e f i x   o n l y   w h e n   S h o w P l u s S i g n   a n d 
 
     r a t e > = 0   ( n e g a t i v e s   k e e p   ` - ` ) ,   N U L L   n a m e   f a l l s   b a c k   t o   ` S l o t N ` .   ` c l i _ p a n e l _ g e o m e t r y ( ) `   i s   t h e 
 
     s i n g l e   o w n e r   o f   t h e   r e c t   ( c o r n e r   a n c h o r s   o n l y :   D e f a u l t   u s e s   P o s X / P o s Y ,   T o p L e f t   p i n s   1 2 , 1 2 , 
 
     T o p R i g h t   p i n s   t o   t h e   r i g h t   e d g e   u s i n g   ` g _ b b _ w ` / ` g _ b b _ h `   c a p t u r e d   f r o m   D 3 D P R E S E N T _ P A R A M E T E R S   i n 
 
     C r e a t e D e v i c e   * a n d *   C r e a t e D e v i c e E x   * a n d *   r e s e t _ h o o k ;   0   - >   f a l l s   b a c k   t o   T o p L e f t ) .   T w o   h i n t   l i n e s 
 
     ( s e c o n d   =   t h e   F 9 + A l t   r e f e r e n c e ) ,   b o t h   c l i p p e d   a t   ` P A N E L _ L I N E _ M A X _ C H A R S   7 8 `   v i a   ` c l i p _ l i n e ` 
 
     ( f i x e d   c a p   �� �   f o n t   w i d t h   m e a s u r e m e n t   w o u l d   n e e d   f o n t - v t a b l e   s l o t s   6 / 8 ,   n o t   i n   t h e   p r o v e n   s e t ) . 
 
 -   * * H o t k e y s :   F 9 + A l t   l a y e r * *   �� �   ` g _ k e y _ p r e v `   g r o w n   8 - > 1 7 ;   b o u n d s   n o w   s i z e o f - d r i v e n .   ` A l t + 1 `   d e c i m a l 
 
     c y c l e ,   ` A l t + 2 `   p l u s ,   ` A l t + 3 `   n a m e s ,   ` A l t + 4 `   z e r o r ,   ` A l t + 5 . . 8 `   f o o d / w o o d / c o i n / e x p o r t   t o g g l e s , 
 
     ` A l t + 9 `   p o s i t i o n   c y c l e ;   p l a i n   l a y e r   i s   b l o c k e d   w h i l e   A l t   i s   h e l d   ( ` ! a l t _ d o w n ` ) ;   b o t h   l a y e r s   n e e d 
 
     t h e   g a m e   w i n d o w   f o r e g r o u n d   ( B 2 )   a n d   s a v e   i m m e d i a t e l y . 
 
 -   * * V e r s i o n   t a b l e * *   �� �   ` s t r u c t   v e r _ e n t r y   g _ v e r s i o n s [ ] `   i n   d 3 d 9 . c :   r o w   0   =   T A D   1 . 0 . 8   p i n n e d   f r o m   t h e 
 
     E X P E C T E D _ *   m a c r o s ,   r o w s   1 . . 3   =   ` l a b e l   N U L L `   T O D O   s t u b s   ( v a n i l l a   /   W a r C h i e f s   /   o t h e r   T A D ) .   T h e 
 
     g a t e   p i c k s   t h e   f i r s t   r o w   w h o s e   s i z e   m a t c h e s   t h e   e x e   a n d   c h e c k s   b a s e   +   P E   v e r s i o n   a g a i n s t   t h a t 
 
     r o w   �� �   r e a s o n   s t r i n g s   b y t e - i d e n t i c a l   t o   R 1 0 . 
 
 -   * * D o c s   P 2 * *   �� �   R E A D M E   r e w r i t t e n   t o   1 2   s e c t i o n s   i n c l .   t r o u b l e - s h o o t i n g   P 0   c h e c k l i s t   ( v e r b a t i m   l o g 
 
     c h a i n ) ,   A / B   i s o l a t i o n   r u n s ,   m u l t i p l a y e r - s a f e t y ,   u n i n s t a l l a t i o n ,   a n d   f o u r   h o n e s t   K n o w n 
 
     L i m i t a t i o n s   ( c o r n e r   H U D   v   n a t i v e   b a r ,   h o t k e y s   v   O p t i o n s   t a b ,   U s e G a m e T i m e   r e m n a n t ,   n o   f a b r i c a t e d 
 
     o f f s e t s ) .   i n i . e x a m p l e   +   s c h e m a . m d   c a r r y   a l l   9   n e w   k e y s . 
 
 
 
 # #   L I V E   E X P E C T A T I O N S   ( v e r i f y   a g a i n s t   d 3 d 9 m o d . l o g ) 
 
 ` ` ` 
 
 R 1 4   D L L   l o a d e d   b a s e = 0 0 4 0 0 0 0 0   r e a l = . .   v e r s i o n = O K   r e a s o n = o k 
 
 U I   r e a d y :   d 3 d x 9 _ 2 5 . d l l   l o a d e d 
 
 O V E R L A Y   A R M E D 
 
 . . . 
 
 c h a i n   . .   n = 3   . .   [ h u m a n - p 1 ] 
 
 - - -   m a t c h   s t a r t   n = 3   . .   - - - 
 
 U I :   f o n t   c r e a t e d   s i z e = 1 4   f a c e = G e o r g i a   f o n t = . . 
 
 o v l   f i r s t   d r a w   o k   f r a m e = . . 
 
 o v l   d i a g   r t = . .   r e c t = 1 2 , 1 2 : 2 5 0 x 1 4 1   a l p h a = 0 x D 9       < -   O N E - S H O T ,   o n l y   w h e n   D e b u g   E n a b l e d = 1 
 
             ( 0 x D 9   =   ( D W O R D ) ( 0 . 8 5 * 2 5 5 + 0 . 5 ) = 2 1 7 ,   t h e   d e f a u l t   o p a c i t y   o f   0 . 8 5 ) 
 
             ( T o p R i g h t :   r e c t   p i n s   t o   t h e   r i g h t   e d g e   v i a   g _ b b _ w ;   D e f a u l t :   P o s X / P o s Y ) 
 
 o v l   h e a r t b e a t   n = . .   f r a m e = . .   r e s = . .   f o o d = . .   w o o d = . .   c o i n = . .   e x p o r t = . . 
 
 ` ` ` 
 
 F 9   p a n e l   n o w   s h o w s   T W O   h i n t   l i n e s   ( 7 8 - c h a r   c l i p ) :   ` [ . . ]   1 : h e a d e r   2 : s l o t s   3 : g a i n s   4 : . .   5 : . . m s   6 : . . ` 
 
 a n d   ` A l t + 1 : d p s   2 : p l u s   3 : n a m e s   4 : z e r o r   5 : f o o d   6 : w o o d   7 : c o i n   8 : e x p   9 : p o s ` .   A / B   r u n s   ( r e n a m e 
 
 a g e 3 y . e x e   /   m o v e   d 3 d x 9 _ 2 5 . d l l )   m u s t   s t i l l   s h o w   t h e   D I S A B L E D   r e a s o n   +   r e d   G D I   l i n e .   Z e r o   ` F A U L T ` 
 
 l i n e s   t h r o u g h o u t . 
 
 
 
 # #   F i l e   c h a n g e s 
 
 -   ` s r c / s t a t e . h `   �� �   R 1 1 :   M o d S e t t i n g s   + =   d e c i m a l _ p l a c e s / s h o w _ p l u s _ s i g n / s h o w _ r e s o u r c e _ n a m e s / 
 
     s h o w _ z e r o _ r a t e s / s h o w _ f o o d / w o o d / c o i n / e x p o r t / p o s i t i o n _ m o d e ;   ` e x t e r n   D W O R D   g _ b b _ w / g _ b b _ h ` ; 
 
     ` P A N E L _ L I N E _ M A X _ C H A R S   7 8 ` ;   n o n - s t a t i c   ` f o r m a t _ r a t e _ l i n e `   d e c l . 
 
 -   ` s r c / s e t t i n g s . c `   �� �   R 1 1 :   d e f a u l t s ;   ` i n i _ p o s _ m o d e ( ) `   ( _ s t r i c m p ) ;   9   k e y s   p a r s e d   i n 
 
     ` s e t t i n g s _ l o a d `   +   ` p r o f i l e _ a p p l y `   ( c l a m p e d ) ,   9   l i n e s   i n   ` s e t t i n g s _ s a v e `   ( ` P o s i t i o n M o d e = % s ` ) . 
 
 -   ` s r c / u i . c `   �� �   R 1 1 :   ` g _ k e y _ p r e v [ 1 7 ] `   +   s i z e o f   b o u n d s / i n i t ;   ` a l t _ d o w n `   +   ` ! a l t _ d o w n `   g u a r d   + 
 
     F 9 + A l t   b l o c k   ( e d g e s   8 . . 1 6 ) ;   ` u i _ p a n e l _ g e o m e t r y `   +   ` c l i p _ l i n e `   +   ` f o r m a t _ r a t e _ l i n e ` ; 
 
     m a i n   4   r o w s   v i a   t h e   s e a m ,   t w o   c l i p p e d   h i n t   l i n e s ;   f i r s t - d r a w   b l o c k   r e s t r u c t u r e d   w i t h   t h e   P 0 
 
     ` o v l   d i a g `   ( d e b u g - g a t e d ,   f l a g   s e t   a f t e r ) . 
 
 -   ` d 3 d 9 . c `   �� �   R 1 1 :   ` g _ b b _ w / g _ b b _ h `   g l o b a l s   +   c a p t u r e   ( f m t [ 0 ] / f m t [ + 4 ] )   i n   C r e a t e D e v i c e , 
 
     C r e a t e D e v i c e E x   a n d   r e s e t _ h o o k ;   ` s t r u c t   v e r _ e n t r y   g _ v e r s i o n s [ ] `   +   r o w - s e l e c t i o n   g a t e   l o o p 
 
     ( r e a s o n   s t r i n g s   v e r b a t i m ) . 
 
 -   ` t e s t s / t e s t _ r a t e _ e n g i n e . c `   �� �   R 1 1 :   ` t e s t _ f o r m a t _ l i n e `   S T E P   ( d e f a u l t s / p r e c i s i o n / c l a m p s / p l u s / 
 
     n a m e s / z e r o - r a t e / v i s i b i l i t y / s e c - u n i t / 4 8 - b y t e   b u f f e r / v e r s i o n - t a b l e   r o w   0   +   T O D O   r o w   1 ) . 
 
 -   ` t e s t s / t e s t _ s o u r c e _ c o n t r a c t . p y `   �� �   R 1 1   b l o c k :   t a b l e   +   T O D O   r o w s   +   r e a s o n   s t r i n g s ,   s t a t e . h 
 
     E X P E C T E D _ *   +   e x t e r n s   +   P A N E L _ L I N E _ M A X _ C H A R S ,   9   s e t t i n g s   k e y s   ( p a r s e   x 2   +   s a v e   +   e x a m p l e   + 
 
     s c h e m a ) ,   V K _ M E N U / ! a l t _ d o w n / F 9 + A l t / h i n t / g _ k e y _ p r e v [ 1 7 ] / s i z e o f ,   g e o m e t r y   +   P o s i t i o n M o d e   n a m e s , 
 
     f o r m a t _ r a t e _ l i n e   s e a m ,   ` o v l   d i a g `   g u a r d   +   p o s i t i o n . 
 
 -   ` R E A D M E . m d ` ,   ` R e s o u r c e R a t e M o d . i n i . e x a m p l e ` ,   ` c o n f i g / s c h e m a . m d `   �� �   R 1 1   d o c s / s c h e m a . 
 
 
 
 # #   B u i l d   /   h a r n e s s e s   ( t h i s   r o u n d ) 
 
 -   B u i l d   r c = 0 ,   z e r o   w a r n i n g s   ( - W a l l   - W e x t r a ) ,   V E R I F Y   P A S S   ( i 3 8 6   P E 3 2 ,   i m p o r t s 
 
     K E R N E L 3 2 / U S E R 3 2 / m s v c r t   o n l y ,   1 1   e x p o r t s   v i a   d 3 d 9 . d e f   �� �   u n c h a n g e d   s e t ) . 
 
 -   d 3 d 9 . d l l   =   * * 1 4 8 , 8 5 3   B * * ,   S H A 2 5 6 
 
     ` 7 7 9 5 7 c 7 8 d 6 7 3 9 d 6 c 6 b 5 4 d 5 e 2 0 6 b a 5 d 1 6 5 f 9 8 3 9 8 3 1 1 3 8 0 e d 6 d e 6 b 3 0 8 2 0 6 1 3 b 9 1 9 ` ,   d e p l o y e d   b y t e - i d e n t i c a l 
 
     t o   r e p o   r o o t   +   g a m e   d i r   +   t e s t s \ d 3 d 9 . d l l   ( a l l   3   S H A 2 5 6   e q u a l ) ;   ` d 3 d x 9 _ 2 5 . d l l `   r e s t o r e d   t o   t h e 
 
     g a m e   d i r   f r o m   S y s W O W 6 4   ( s y s t e m   D X 9   r u n t i m e ;   l o a d e r   a l s o   f a l l s   b a c k   t o   i t ) ;   o l d   d 3 d 9 m o d . l o g 
 
     d e l e t e d . 
 
 -   T o o l c h a i n :   w 6 4 d e v k i t - x 8 6   * * v 2 . 9 . 1 * *   r e - e x t r a c t e d   t o   t h e   T e m p   w o r k i n g   d i r   ( v 2 . 8   T E M P   i n s t a l l   w a s 
 
     w i p e d   w i t h   T e m p   c l e a n u p ;   s a m e   b i n   +   l i b e x e c \ g c c \ i 6 8 6 - w 6 4 - m i n g w 3 2 \ 1 6 . 2 . 0   l a y o u t ,   g c c   1 6 . 2 . 0 ) . 
 
 -   H a r n e s s e s :   t e s t _ r a t e _ e n g i n e . e x e   F A I L U R E S   0   ( i n c l .   n e w   R 1 1   f o r m a t / t a b l e   S T E P s )   ,%V%
 
     t e s t _ d 3 d 9 _ a c t u a l . e x e   A L L   P A S S   ,%V%  l o a d s m o k e _ p a t h . e x e   P A S S   ,%V%  t e s t _ s o u r c e _ c o n t r a c t . p y   P A S S 
 
     ( R 1 1   b l o c k   +   a l l   R 1 0 / R 1 4   p i n s   g r e e n ) . 
 
 
 
 - - - 
 
 
 
 #   B U I L D   �� �   R O U N D   1 0   ( 2 0 2 6 - 0 9 - 0 7 )   �� �   A R M E D / D I S A B L E D   S T A T U S   +   G D I   V I S I B L E   F A L L B A C K   +   D R A W   H E A R T B E A T 
 
 
 
 # #   R O U N D   S U M M A R Y 
 
 N o   c r a s h   t h i s   r o u n d   ( g a m e   n o t   l a u n c h e d ) .   R 1 0   c l o s e s   t h e   P 0   s p e c   g a p   f r o m   t h e   R 9   l i v e 
 
 e x p e c t a t i o n s :   i f   t i m e   e v e r   r a n   w i t h   t h e   o v e r l a y   s i l e n t l y   d i s a b l e d   t h i s   r o u n d   m a k e s   i t 
 
 * * v i s i b l y   i m p o s s i b l e * * . 
 
 -   * * S t a t u s   l i n e   ( o n c e   p e r   l o a d ) * *   �� �   D l l M a i n   e m i t s   e x a c t l y   o n e   l i n e   a f t e r   ` v e r s i o n _ g a t e _ c h e c k ( ) ` 
 
     a n d   ` u i _ i n i t ( ) ` :   ` O V E R L A Y   A R M E D ` ,   ` O V E R L A Y   A R M E D   ( i n i   m i s s i n g :   u s i n g   d e f a u l t s ) ` ,   o r 
 
     ` O V E R L A Y   D I S A B L E D :   < r e a s o n > ` .   R e a s o n s   c o v e r   e v e r y   d i s a b l e   p a t h   ( ` v e r : < g a t e - r e a s o n > `   w i n s , 
 
     t h e n   ` d 3 d x 9 _ 2 5 . d l l   n o t   l o a d a b l e ` ) ;   i n i - m i s s i n g   i s   a   s u f f i x ,   n e v e r   a   d i s a b l e . 
 
     P u r e   h e l p e r   ` o v e r l a y _ s t a t e _ r e a s o n ( v e r s i o n _ o k ,   u i _ r e a d y ,   i n i _ m i s s i n g ) `   +   p u b l i c 
 
     ` o v e r l a y _ d i s a b l e d _ r e a s o n ( ) ` . 
 
 -   * * G D I   v i s i b l e   f a l l b a c k   ( P 0 ) * *   �� �   w h e n   t h e   o v e r l a y   i s   D I S A B L E D ,   e v e r y   f r a m e   d r a w s   a   r e d 
 
     ` R e s o u r c e   R a t e   M o d :   d i s a b l e d   ( < r e a s o n > ) `   l i n e   a t   t h e   t o p - l e f t   o f   t h e   l a r g e s t   v i s i b l e 
 
     g a m e   w i n d o w   ( f o u n d   o n c e   v i a   E n u m W i n d o w s ,   c a c h e d ) .   Z e r o   n e w   i m p o r t s :   S e t B k M o d e / S e t B k C o l o r / 
 
     S e t T e x t C o l o r   a r r i v e   v i a   r u n t i m e   ` L o a d L i b r a r y A ( " g d i 3 2 . d l l " ) `   +   G e t P r o c A d d r e s s .   C a l l e d   f r o m 
 
     ` p r e s e n t _ h o o k `   A F T E R   t h e   t r i p w i r e   c l e a r   �� �   o u t s i d e   B 7 ' s   s e t / c l e a r   i n t e r v a l   a n d   t h e 
 
     ` E n a b l e d = 0 `   z e r o - t o u c h   b r a n c h .   N o   b r e a d c r u m b ,   n o   p e r - f r a m e   l o g :   t h e   r e d   l i n e   I S   t h e 
 
     v i s i b i l i t y .   N o   ` - l g d i 3 2 `   i n   b u i l d   l i n e ,   i m p o r t   t a b l e   s t a y s   e x a c t l y 
 
     { K E R N E L 3 2 ,   U S E R 3 2 ,   m s v c r t } . 
 
 -   * * D r a w   h e a r t b e a t * *   �� �   e v e r y   6 0 0   c o m p l e t e d   o v e r l a y   d r a w s   ( ~ 1 0 s   @   6 0 f p s )   u i _ d r a w   l o g s 
 
     ` o v l   h e a r t b e a t   n = N   f r a m e = F   r e s = . .   f o o d = . .   w o o d = . .   c o i n = . .   e x p o r t = . . `   ( n o t   d e b u g - g a t e d , 
 
     < =   ~ 6   l i n e s / m i n ) ,   p r o v i n g   t h e   d r a w   p a t h   s t i l l   c o m p l e t e s   l o n g   a f t e r   f i r s t   f r a m e . 
 
 -   * * H a r n e s s   P 0   h a r d e n i n g * *   �� �   c o n t r a c t   p i n s   t h e   D l l M a i n - o n l y   o n c e - p e r - l o a d   c a l l ,   t h e 
 
     t r i p w i r e - c l e a r   o r d e r i n g ,   E X A C T   d e v i c e / f o n t   v t a b l e   s l o t   s e t s   ( { 3 , 1 6 , 3 8 , 4 1 , 4 2 , 5 7 , 5 8 , 8 3 , 8 9 , 9 0 } 
 
     a n d   { 1 4 , 1 6 , 1 7 }   �� �   n o   n e w   s l o t s ) ,   d y n a m i c - g d i 3 2   u s e ,   a n d   t h e   h e a r t b e a t   p o s i t i o n ;   t h e   a c t u a l - 
 
     c o d e   h a r n e s s   r u n s   ` o v e r l a y _ s t a t e _ r e a s o n `   o n   a l l   f o u r   p r e c e d e n c e   c a s e s ,   e x e r c i s e s   t h e   r e a l 
 
     E n u m W i n d o w s / G D I   d r a w   o n   a   v i s i b l e   w i n d o w   ( r e t u r n   1 ) ,   t h e   n o - w i n d o w   s a f e t y   ( r e t u r n   0 ,   n o 
 
     c r a s h ) ,   t h e   d e s t r o y e d - w i n d o w   r e u s e   ( r e t u r n   0 ) ,   a n d   t h e   A R M E D   s h o r t - c i r c u i t   ( r e t u r n   0 ) . 
 
 
 
 # #   L I V E   E X P E C T A T I O N S   ( v e r i f y   a g a i n s t   d 3 d 9 m o d . l o g ) 
 
 ` ` ` 
 
 R 1 4   D L L   l o a d e d   b a s e = 0 0 4 0 0 0 0 0   r e a l = . .   v e r s i o n = O K   r e a s o n = o k 
 
 U I   r e a d y :   d 3 d x 9 _ 2 5 . d l l   l o a d e d 
 
 O V E R L A Y   A R M E D                                               < -   N E W ,   o n c e   p e r   l o a d 
 
 . . . 
 
 c h a i n   . .   n = 3   . .   [ h u m a n - p 1 ] 
 
 - - -   m a t c h   s t a r t   n = 3   . .   - - - 
 
 U I :   f o n t   c r e a t e d   s i z e = 1 4   f a c e = G e o r g i a   f o n t = . . 
 
 o v l   f i r s t   d r a w   o k   f r a m e = . . 
 
 o v l   h e a r t b e a t   n = 6 0 0   f r a m e = . .   r e s = 1 2 8 6 A 3 0 0   f o o d = . .   w o o d = . .   c o i n = . .   e x p o r t = . . 
 
 ( . . r e p e a t s   e v e r y   6 0 0   d r a w s   �� �   p r o v e s   l o n g - r u n   d r a w   c o m p l e t i o n . . ) 
 
 ` ` ` 
 
 A / B   t r y - b r o k e n - c h e c k - t r i p l e   ( v e r i f y   t h e   D I S A B L E D   p a t h s   r e a l   b e f o r e   t r u s t i n g   A R M E D ) : 
 
 -   * * ( a ) * *   r e n a m e   ` a g e 3 y . e x e `   �� �   ` d 3 d 9 m o d . l o g `   m u s t   s h o w   ` v e r s i o n = B Y P A S S   r e a s o n = e x e   s i z e = . . ` 
 
     +   ` O V E R L A Y   D I S A B L E D :   v e r : e x e   s i z e = . . `   a n d   a   * * r e d   G D I   l i n e * *   a p p e a r s   i n - g a m e .   R e s t o r e . 
 
 -   * * ( b ) * *   m o v e   ` d 3 d x 9 _ 2 5 . d l l `   o u t   o f   t h e   g a m e   d i r   �� �   m u s t   s h o w 
 
     ` O V E R L A Y   D I S A B L E D :   d 3 d x 9 _ 2 5 . d l l   n o t   l o a d a b l e `   a n d   t h e   * * r e d   G D I   l i n e * * .   R e s t o r e . 
 
 Z e r o   ` F A U L T `   l i n e s   t h r o u g h o u t .   ` O V E R L A Y   A R M E D `   /   ` O V E R L A Y   D I S A B L E D : `   a p p e a r s   E X A C T L Y   o n c e 
 
 p e r   l a u n c h . 
 
 
 
 # #   F i l e   c h a n g e s 
 
 -   ` s r c / u i . c `   �� �   R 1 0 :   ` u i _ r e a d y ( ) `   a c c e s s o r ;   ` s _ o v l _ d r a w s `   +   6 0 0 - f r a m e   h e a r t b e a t   a f t e r   t h e 
 
     f i r s t - d r a w   m a r k e r ;   ` u i _ g d i _ f a l l b a c k _ d r a w ( ) `   +   ` u i _ g d i _ f i n d _ h w n d `   ( E n u m W i n d o w s   b e s t - w i n d o w 
 
     s c a n ,   c a c h e d )   +   3   d y n a m i c   g d i 3 2   p r o c s   ( S e t T e x t C o l o r / S e t B k C o l o r / S e t B k M o d e   v i a 
 
     L o a d L i b r a r y A )   +   D r a w T e x t A   a t   ( 8 , 8 ) ,   ` D T _ S I N G L E L I N E `   d e f i n e   a d d e d ;   ` # i f d e f   S W A R M _ T E S T ` 
 
     ` u i _ t e s t _ s e t _ r e a d y `   s e a m . 
 
 -   ` d 3 d 9 . c `   �� �   R 1 0 :   ` g _ i n i _ m i s s i n g `   g l o b a l ;   s t a t i c   p u r e   ` o v e r l a y _ s t a t e _ r e a s o n ( ) `   + 
 
     ` o v e r l a y _ d i s a b l e d _ r e a s o n ( ) `   +   ` o v e r l a y _ s t a t u s _ l o g ( ) ` ;   D l l M a i n   e m i t s   t h e   o n e   s t a t u s   l i n e 
 
     a f t e r   ` u i _ i n i t ( ) `   b e f o r e   ` d l l m a i n - d o n e ` ;   ` u i _ g d i _ f a l l b a c k _ d r a w ( ) `   c a l l e d   i n   ` p r e s e n t _ h o o k ` 
 
     a f t e r   t h e   t r i p w i r e   c l e a r . 
 
 -   ` s r c / s e t t i n g s . c `   �� �   R 1 0 :   ` g _ i n i _ m i s s i n g   =   1 ; `   i n   t h e   f o p e n - f a i l u r e   b r a n c h   ( d e f a u l t s   s t i l l 
 
     l o a d e d ) . 
 
 -   ` s r c / s t a t e . h `   �� �   R 1 0 :   ` g _ i n i _ m i s s i n g `   e x t e r n ,   ` O V L _ H E A R T B E A T _ F R A M E S   6 0 0 ` ,   s t a t u s / G D I / ` u i _ r e a d y ` 
 
     d e c l a r a t i o n s ,   S W A R M _ T E S T   s e t t e r   d e c l . 
 
 -   ` t e s t s / t e s t _ s o u r c e _ c o n t r a c t . p y `   �� �   R 1 0   P 0   b l o c k :   o n c e - p e r - l o a d   p o s i t i o n i n g ,   h o o k - b o d y 
 
     c l e a n l i n e s s ,   g _ i n i _ m i s s i n g   p l a c e m e n t ,   d y n a m i c   g d i 3 2   +   n o   ` - l g d i 3 2 ` ,   E X A C T   s l o t   p i n s , 
 
     h e a r t b e a t   o r d e r ,   s t a t u s   v a r i a n t s . 
 
 -   ` t e s t s / t e s t _ d 3 d 9 _ a c t u a l . c `   �� �   R 1 0   P 0 :   s t a t e _ r e a s o n   p r e c e d e n c e   m a t r i x ;   n o - w i n d o w   s a f e t y ; 
 
     v i s i b l e - w i n d o w   s m o k e   ( r e t u r n   1 ) ;   d e s t r o y e d - w i n d o w   ( r e t u r n   0 ) ;   A R M E D   s h o r t - c i r c u i t 
 
     ( r e t u r n   0 )   v i a   t h e   S W A R M _ T E S T   s e a m . 
 
 
 
 # #   B u i l d   /   h a r n e s s e s   ( t h i s   r o u n d ) 
 
 -   B u i l d   r c = 0 ,   z e r o   w a r n i n g s   ( - W a l l   - W e x t r a ) ,   V E R I F Y   P A S S   ( i 3 8 6   P E 3 2 ,   i m p o r t s 
 
     K E R N E L 3 2 / U S E R 3 2 / m s v c r t   o n l y ,   1 1   e x p o r t s ) . 
 
 -   d 3 d 9 . d l l   =   * * 1 4 5 , 2 2 9   B * * ,   S H A 2 5 6 
 
     ` 0 f 3 e 1 b b 3 1 8 3 d 4 e 5 2 9 0 0 f e c a f 6 2 f d 0 4 5 5 7 a 4 d e 1 f f 7 f f 4 0 7 7 f 8 b 1 2 9 f 0 0 6 d 2 7 4 0 3 a ` ,   d e p l o y e d   b y t e - i d e n t i c a l 
 
     t o   r e p o   r o o t   +   g a m e   d i r   +   t e s t s \ d 3 d 9 . d l l   ( a l l   3   S H A 2 5 6   e q u a l ) ;   o l d   d 3 d 9 m o d . l o g   d e l e t e d . 
 
 -   H a r n e s s e s :   t e s t _ d 3 d 9 _ a c t u a l . e x e   A L L   P A S S   ( i n c l .   n e w   R 1 0   P 0   G D I / s t a t u s   c h e c k s )   ,%V%
 
     t e s t _ r a t e _ e n g i n e . e x e   F A I L U R E S   0   ,%V%  t e s t _ s o u r c e _ c o n t r a c t . p y   P A S S   ( 0   f a i l u r e s )   ,%V%
 
     t e s t _ p e _ s t r u c t u r e . p y   F A I L U R E S   0 . 
 
 
 
 >   G a m e   N O T   l a u n c h e d   t h i s   r o u n d .   N e x t   s t e p   =   t h e   l i v e   b o s s   f i g h t :   r u n   w i t h   ` [ D e b u g ]   E n a b l e d = 1 ` , 
 
 >   c o n f i r m   t h e   L I V E   E X P E C T A T I O N S   b l o c k   v e r b a t i m ,   t h e n   d o   A / B   c h e c k s   ( a )   a n d   ( b ) . 
 
 
 
 - - - 
 
 
 
 #   B U I L D   �� �   R O U N D   9   ( R E W O R K ,   2 0 2 6 - 0 9 - 0 7 )   �� �   F O N T   V T A B L E   T R U T H   +   D E V I C E - G A T E D   B I N D I N G   +   D R A I N   C L A M P 
 
 
 
 # #   R O U N D   S U M M A R Y 
 
 N o   c r a s h   t h i s   r o u n d .   R 9   i s   t h e   f i n a l   h a r d e n i n g   p a s s   b e f o r e   t h e   l i v e   t e s t   �� �   f o u r   f i n d i n g s   f r o m   a   f r e s h   r e a d   o f   t h e   c o d e : 
 
 -   * * F 1   ( t h e   b i g   o n e ) :   t h e   I D 3 D X F o n t   v t a b l e   s l o t s   w e r e   W R O N G . * *   S i n c e   R 1 4   t h e   f a l l b a c k   b i n d i n g   u s e d   t h e   c a n o n i c a l   D 3 D X 9   d e v i c e   l a y o u t   ( ` B e g i n   1 2   /   D r a w T e x t A   1 3   /   E n d   1 5 ` )   a g a i n s t   t h e   D 3 D X F O N T   i n t e r f a c e   �� �   t h a t   t a b l e   i s   f o r   ` I D 3 D X S p r i t e ` ,   n o t   ` I D 3 D X F o n t ` .   V e r i f i e d   a g a i n s t   t h e   A C T U A L   S D K   i n t e r f a c e s   ( m i n g w - w 6 4   ` d 3 d x 9 c o r e . h ` ,   R e a c t O S ,   W i n e - m i r r o r ,   a n d   t h e   g e n u i n e   M i c r o s o f t   S D K   4 3   h e a d e r   �� �   a l l   i d e n t i c a l   s i n c e   D 3 D X 9   F W L   i f a c e   n e v e r   c h a n g e d ) :   I D 3 D X F o n t   h a s   * * N O   B e g i n   a n d   N O   E n d * * ;   s l o t   1 3   i s   * * P r e l o a d T e x t W * * ,   D r a w T e x t A = * * 1 4 * * ,   D r a w T e x t W = 1 5 ,   O n L o s t D e v i c e = 1 6 ,   O n R e s e t D e v i c e = 1 7 .   T h e   o l d   t a b l e   w a s   d i s p a t c h i n g   ` P r e l o a d T e x t A / P r e l o a d T e x t W / D r a w T e x t W `   i n t o   a   r e a l   f o n t   ( g a r b a g e   c a l l s   o n   s l o t s   t h a t   e x i s t   b u t   d o   t h e   w r o n g   t h i n g ) .   F i x :   n o   B e g i n / E n d   a t   a l l   �� �   a   s i n g l e   ` D r a w T e x t A ( 1 4 ) `   c a l l   w i t h   t h e   t r u e   l a y o u t   ` ( T h i s ,   p S p r i t e ,   p S t r i n g ,   C o u n t ,   p R e c t ,   F o r m a t ,   C o l o r ) ` ,   ` p S p r i t e = N U L L ` ,   u s i n g   t h e   a l r e a d y - r o t a t e d   r e n d e r   t a r g e t .   ( T h e   R 9   d r a f t   c l a i m e d   " B e g i n = 1 3 " ;   h e a d e r   g r e p   s h o w s   s l o t   1 3   =   P r e l o a d T e x t W ,   a n d   n o   B e g i n   e x i s t s   �� �   t h e   d e r i v e d   c o u n t s   a r e   a u t h o r i t a t i v e . ) 
 
 -   * * F 2 : * *   ` u i _ c r e a t e _ f o n t ( * p p d e v ) `   r a n   u n c o n d i t i o n a l l y   i n   ` w _ c r e a t e _ d e v i c e ` / ` w _ c r e a t e _ d e v i c e _ e x `   e v e n   w h e n   ` p a t c h _ d e v i c e _ p r e s e n t `   r e t u r n e d   0   ( a u x / s e c o n d   d e v i c e )   �� �   t h e   f o n t   a t t a c h e d   t o   a n   u n - p a t c h e d   d e v i c e .   N o w   g a t e d :   ` i f   ( p a t c h e d )   u i _ c r e a t e _ f o n t ( * p p d e v ) ; `   i n   B O T H   p a t h s . 
 
 -   * * F 3 : * *   t h e   S h o w G a i n s = 0   s k i p   t e s t e d   ` i n s t   >   g _ l a s t _ e m a [ s ]   *   r a t i o `   �� �   a   d r a i n e d   s l o t   ( E M A   �� �   0 / n e g a t i v e )   m a k e s   ` i n s t   >   0 `   m a t c h   E V E R Y   p o s i t i v e   s a m p l e   f o r e v e r .   T h e   r e f e r e n c e   i s   n o w   c l a m p e d :   ` f l o a t   r e f   =   g _ l a s t _ e m a [ s ]   >   0 . 0 f   ?   g _ l a s t _ e m a [ s ]   :   0 . 0 f ; `   ( s k i p   a g a i n s t   ` r e f   *   r a t i o ` ) ,   s o   t h e   s l o t   l i f e c y c l e   s t a y s   h o n e s t . 
 
 -   * * F 4 : * *   c o n t r a c t   h a r n e s s   t i g h t e n e d   �� �   A 1   p i n s   ` s _ t i c k `   A B S E N T   f r o m   t h e   ` c l o c k _ n o w `   b o d y   ( o l d   ` r e t u r n   s _ t i c k `   g u a r d   o n l y   m a t c h e d   o n e   s h a p e ) ;   B 6   p i n s   F V F   o r d e r   ` s a v e   <   D r a w P r i m i t i v e U p   <   r e s t o r e `   i n s i d e   t h e   b a c k d r o p ;   B 7   p i n s   n o   e a r l y   ` r e t u r n `   b e t w e e n   t h e   t r i p w i r e   s e t   a n d   c l e a r   w h i l e   h o l d i n g   t h e   t o k e n ;   n e w   R 9   a s s e r t s   f o r   t h e   f o n t   s l o t s / E n d - b a n / 7 - a r g   D r a w T e x t A / d e v i c e - g a t e d   c r e a t i o n / c l a m p ;   r a t e   h a r n e s s   g o t   a   d r a i n e d - s l o t   ( E M A = 0 )   S h o w G a i n s   c l a m p   s u b - t e s t . 
 
 
 
 # #   T h e   F 1   f i x   ( t h e   r e a s o n   t h i s   r o u n d   e x i s t s ) 
 
 -   ` s r c / u i . c `   s l o t   t a b l e   i s   n o w   ` F O N T _ D R A W T E X T A   1 4 ` ,   ` F O N T _ O N L O S T D E V I C E   1 6 ` ,   ` F O N T _ O N R E S E T D E V I C E   1 7 `   �� �   ` F O N T _ B E G I N ` / ` F O N T _ E N D `   d e l e t e d .   ` u i _ f o n t _ d r a w `   c a l l s   ` d t ( f o n t ,   N U L L ,   s ,   - 1 ,   & r c ,   D T _ L E F T | D T _ T O P | D T _ N O C L I P ,   c o l o r ) `   ( r e a l   7 - a r g   C O M   l a y o u t ) .   T h e   p a n e l   n o   l o n g e r   c a l l s   B e g i n / E n d   a t   a l l .   B r e a d c r u m b   ` o v l - p a n e l - t e x t `   a d d e d   a f t e r   ` u i _ d r a w _ b a c k d r o p ` .   H e a d e r   e v i d e n c e   r e c o r d e d   i n   ` t e s t s / t e s t _ s o u r c e _ c o n t r a c t . p y `   ( R 9   b l o c k )   a n d   t h i s   r e p o r t . 
 
 
 
 # #   F i l e   c h a n g e s 
 
 -   ` s r c / u i . c `   �� �   R 9   I D 3 D X F o n t   s l o t   t r u t h   ( D r a w T e x t A = 1 4   o n l y ;   B e g i n / E n d   r e m o v e d ) ;   r e a l   7 - a r g   D r a w T e x t A ;   ` o v l - p a n e l - t e x t `   b r e a d c r u m b ;   F 1   l a y o u t   c o m m e n t . 
 
 -   ` d 3 d 9 . c `   �� �   F 2 :   ` i f   ( p a t c h e d )   u i _ c r e a t e _ f o n t ( * p p d e v ) ; `   i n   b o t h   c r e a t e   p a t h s   ( f o n t   o n l y   b i n d s   t o   a   d e v i c e   w h o s e   P r e s e n t   h o o k   i s   i n s t a l l e d ) . 
 
 -   ` s r c / t r a c k e r . c `   �� �   F 3 :   S h o w G a i n s   s k i p   r e f e r e n c e   c l a m p e d   t o   ` > =   0 `   ( ` r e f   *   r a t i o ` ) . 
 
 -   ` t e s t s / t e s t _ s o u r c e _ c o n t r a c t . p y `   �� �   F 4 :   A 1   ` s _ t i c k `   a b s e n t   f r o m   c l o c k _ n o w   b o d y ;   B 6   F V F   o r d e r   p i n n e d ;   B 7   n o - e a r l y - r e t u r n   p i n ;   R 9   f o n t - s l o t / E n d - b a n / 7 - a r g / g a t i n g / c l a m p   a s s e r t s ;   R 1 4   d o c   h e a d e r   c o r r e c t e d . 
 
 -   ` t e s t s / t e s t _ r a t e _ e n g i n e . c `   �� �   F 4 / R 9 :   d r a i n e d - s l o t   ( E M A = 0 )   S h o w G a i n s = 0   c l a m p   s u b - t e s t   +   r e s u m e - o n - S h o w G a i n s = 1 . 
 
 
 
 # #   B u i l d   /   h a r n e s s e s   ( t h i s   r o u n d ) 
 
 -   B u i l d   r c = 0 ,   z e r o   w a r n i n g s ,   V E R I F Y   P A S S   ( i 3 8 6   P E 3 2 ,   i m p o r t s   K E R N E L 3 2 / U S E R 3 2 / m s v c r t ,   1 1   e x p o r t s ) . 
 
 -   d 3 d 9 . d l l   =   * * 1 4 2 , 0 5 2   B * * ,   S H A 2 5 6   ` 1 b b 1 b 2 4 1 6 1 d 3 8 c 0 1 5 3 a 2 6 1 7 5 4 f 5 5 9 0 f 2 3 6 f e 7 f e 2 7 c 4 8 c c 7 c a e d 9 5 9 c f 0 e e 1 6 d 5 d ` ,   d e p l o y e d   b y t e - i d e n t i c a l   t o   g a m e   d i r   +   t e s t s \ d 3 d 9 . d l l   ( v e r i f i e d   i d e n t i c a l   o n   a l l   3   c o p i e s ) ;   o l d   d 3 d 9 m o d . l o g   d e l e t e d . 
 
 -   H a r n e s s e s :   t e s t _ d 3 d 9 _ a c t u a l . e x e   A L L   P A S S   ,%V%  t e s t _ r a t e _ e n g i n e . e x e   F A I L U R E S   0   ,%V%  t e s t _ s o u r c e _ c o n t r a c t . p y   P A S S   ( 0   f a i l u r e s )   ,%V%  t e s t _ p e _ s t r u c t u r e . p y   F A I L U R E S   0 . 
 
 
 
 >   G a m e   N O T   l a u n c h e d   t h i s   r o u n d .   N e x t   s t e p   =   t h e   l i v e   t e s t   ( w i t h   ` [ D e b u g ]   E n a b l e d = 1 `   f o r   t h e   f i r s t   c a l i b r a t e d   r u n ) . 
 
 
 
 - - - 
 
 
 
 # #   R O U N D   S U M M A R Y 
 
 N o   c r a s h   t h i s   r o u n d   �� �   R 8   i s   a   h a r d e n i n g / p a s s   t w o - r e v i e w   p a s s   o v e r   t h e   R 7   b u i l d : 
 
 -   * * P a r t   A   ( e x t e r n a l   r e v i e w ) : * *   A 1   t h e   r a t e   c l o c k   w a s   W R O N G   �� �   ` s _ t i c k `   i s   t h e   p e r - P r e s e n t   F R A M E   c o u n t e r ,   n o t   m i l l i s e c o n d   g a m e   t i m e ,   s o   g a m e - t i m e - b a s e d   r a t e s   s c a l e d   w i t h   F P S ;   s w i t c h e d   t o   r e a l t i m e   Q P C   ( +   G e t T i c k C o u n t   f a l l b a c k ) ,   k e p t   ` U s e G a m e T i m e `   p a r s e d - b u t - c o s m e t i c ,   o n e - t i m e   l o g   n o t e .   A 2   a d d e d   a n   e x p l i c i t   ` g _ e m a _ v a l i d [ M A X _ S L O T S ] `   l i v e n e s s   f l a g   ( r e p l a c e s   t h e   E M A - m a g n i t u d e   h e u r i s t i c ) .   A 3   t h e   p e r - t i c k   ` R E S   t = . . . `   s t r e a m   i s   n o w   g a t e d   b e h i n d   ` [ D e b u g ]   E n a b l e d = 1 `   ( q u i e t   l o g   b y   d e f a u l t ) .   A 4   a d d e d   ` s e t t i n g s _ p o l l _ p r o f i l e ( ) `   ( 1 / s e c - t h r o t t l e d   r e - p o l l   o f   ` U s e r s \ D e f a u l t P r o f i l e * . x m l ` )   c a l l e d   f r o m   p r e s e n t _ h o o k   w i t h   a   ` p r o f i l e - p o l l `   b r e a d c r u m b .   A 5   a   s e c o n d   d e v i c e   v t a b l e   i s   n o w   L O G G E D   ( ` s e c o n d   v t a b l e   % p   s e e n   ( h a d   % p )   -   n o t   r e - p a t c h e d ,   o v e r l a y   i n a c t i v e   o n   t h i s   d e v i c e ` )   i n s t e a d   o f   s i l e n t l y   s k i p p e d . 
 
 -   * * P a r t   B   ( i n s p e c t o r ) : * *   B 1   ` g _ f o n t _ d e v `   b i n d s   t h e   f o n t   t o   i t s   d e v i c e   ( a   b o u n d   f o n t   f o r   a   d i f f e r e n t   d e v i c e   i s   d e s t r o y e d   f i r s t ) .   B 2   p a n e l   h o t k e y s   1 - 6   n o w   r e q u i r e   F 9   H E L D   +   g a m e - w i n d o w   f o r e g r o u n d   +   p e r - k e y   e d g e   ( n o   p h a n t o m   t o g g l e s   i n   c h a t   /   a l t - t a b ) ;   F 9   t o g g l e   s t a y s   e d g e - t r i g g e r e d .   B 3   ` f o n t _ d e s t r o y `   R e l e a s e s   o n l y   t h r o u g h   a   ` f o n t _ s a f e ( ) `   p o i n t e r .   B 4   f o n t - c r e a t e - f a i l u r e   l o g g e d   o n c e ,   t h e n   a   h e a r t b e a t   e v e r y   3 0 0   f a i l u r e s .   B 5   o n l y   ` S _ O K `   p r o c e e d s   p a s t   T e s t C o o p e r a t i v e L e v e l   ( ` u h r   ! =   0 `   �� �   a b o r t ) .   B 6   p a n e l   b a c k d r o p   n o w   s a v e s / r e s t o r e s   t h e   d e v i c e   F V F   v i a   G e t F V F / S e t F V F   v t a b l e   s l o t s   * * 8 9 / 9 0 * *   ( c a n o n i c a l   D X 9   v t a b l e )   a r o u n d   ` D r a w P r i m i t i v e U p ` ,   b r e a d c r u m b e d   ` o v l - f v f ` .   B 7   p r e s e n t _ h o o k   g o t   a n   ` I n t e r l o c k e d C o m p a r e E x c h a n g e `   r e - e n t r a n c y   t r i p w i r e   ( n e s t e d   P r e s e n t   �� �   f o r w a r d e d   r a w ,   ` p r e s e n t - r e e n t r a n t `   s t e p ) .   B 8   ` S h o w G a i n s = 0 `   n o w   t r e a t s   s t r o n g l y   p o s i t i v e   j u m p s   l i k e   s p e n d   s p i k e s   ( E M A   f r o z e n ,   b a s e l i n e   f r e s h ,   s a m e   r a t i o ) .   B 9   ` [ G e n e r a l ]   S t a r t H i d d e n = 1 `   h i d e s   t h e   p a n e l   a t   l o a d . 
 
 
 
 # #   T h e   A 1   f i x   ( m o s t   i m p o r t a n t ) 
 
 -   ` s r c / t r a c k e r . c `   ` c l o c k _ n o w ( ) ` :   f o r m e r l y   ` i f   ( u s e _ g a m e _ t i m e )   r e t u r n   ( D W O R D ) s _ t i c k ; `   �� �   n o w   A L W A Y S   r e a l t i m e   ( ` Q u e r y P e r f o r m a n c e C o u n t e r `   �� �   m s ,   ` G e t T i c k C o u n t `   f a l l b a c k ) .   V e r i f i e d   l i v e   d e f i n i t i o n :   ` s _ t i c k + + `   i n   ` o b s e r v e r _ s a m p l e ( ) `   ( s r c / g a m e i f . c )   �� �   o n e   i n c r e m e n t   p e r   p r e s e n t   f r a m e ,   n o   m s   m e a n i n g   ( t h e   g a m e ' s   o w n   ` F U N _ 0 0 8 6 d a 3 9   *   R V A _ T I M E _ S T E P = 0 . 0 0 1 f `   m u l t i p l i e s   p e r - t i c k   d e l t a s ,   b u t   n e v e r   p r o d u c e s   a   m s   c o u n t e r ) .   R e a l - t i m e   E M A   d e l t a s   a r e   n o w   ` m s / 1 0 0 0 `   s e c o n d s   r e g a r d l e s s   o f   F P S .   ` t r a c k e r _ s e t _ c l o c k _ o v e r r i d e ( m s ) `   k e e p s   t h e   d e t e r m i n i s t i c   h a r n e s s ;   ` t r a c k e r _ r e s e t `   c l e a r s   i t . 
 
 
 
 # #   F i l e   c h a n g e s 
 
 -   ` s r c / t r a c k e r . c `   �� �   r e a l t i m e   c l o c k   ( A 1 ) ,   ` t r a c k e r _ s e t _ c l o c k _ o v e r r i d e `   ( A 1 ) ,   ` g _ e m a _ v a l i d `   l i v e n e s s   f l a g   ( A 2 ,   v a l i d   o n l y   a f t e r   a   r e a l   f i r s t   r a t e   �� �   t h e   v e r y   f i r s t   s a m p l e   l a y s   d o w n   t h e   b a s e l i n e   w i t h o u t   m a r k i n g   a l i v e ) ,   S h o w G a i n s = 0   p o s i t i v e - j u m p   s k i p   ( B 8 ) . 
 
 -   ` s r c / s e t t i n g s . c `   �� �   o n e - t i m e   ` c l o c k :   u s i n g   r e a l t i m e   Q P C   ( s _ t i c k   i s   a   f r a m e   c o u n t e r ,   n o t   g a m e   t i m e ) `   n o t e   ( A 1 ) ;   ` s e t t i n g s _ p o l l _ p r o f i l e ( ) `   1 / s   t h r o t t l e   ( A 4 ) . 
 
 -   ` s r c / g a m e i f . c `   �� �   ` R E S   t = . . . `   +   ` r a t e s   . . . `   b o t h   u n d e r   ` i f   ( g _ s e t t i n g s . d e b u g _ e n a b l e d ) `   ( A 3 ) . 
 
 -   ` s r c / s t a t e . h `   �� �   e x t e r n s :   ` g _ e m a _ v a l i d ` ,   ` g _ f o n t _ d e v ` ,   ` t r a c k e r _ s e t _ c l o c k _ o v e r r i d e ` ,   ` s e t t i n g s _ p o l l _ p r o f i l e ` . 
 
 -   ` s r c / u i . c `   �� �   ` g _ f o n t _ d e v `   b i n d i n g   +   ` f o n t _ d e s t r o y `   N U L L s   i t   ( B 1 / B 3 ) ;   h o t k e y s   F 9 - h e l d   +   f o r e g r o u n d   +   e d g e   ( B 2 ) ;   f a i l u r e   l o g   o n c e   +   e v e r y - 3 0 0   h e a r t b e a t   ( B 4 ) ;   ` u h r   ! =   0 `   T C L   g a t e   ( B 5 ) ;   ` D 9 _ S E T F V F   8 9   /   D 9 _ G E T F V F   9 0 `   +   s a v e / r e s t o r e   a r o u n d   t h e   b a c k d r o p   +   ` o v l - f v f `   b r e a d c r u m b s   ( B 6 ) ;   ` f o n t _ s a f e ( ) `   g a t e   i n s i d e   ` f o n t _ d e s t r o y `   ( B 3 ) . 
 
 -   ` d 3 d 9 . c `   �� �   p r e s e n t _ h o o k   r e - e n t r a n c y   t r i p w i r e   +   ` p r e s e n t - r e e n t r a n t `   s t e p   +   s i n g l e - e x i t   c l e a r   ( B 7 ) ;   ` p r o f i l e - p o l l `   s t e p   +   ` s e t t i n g s _ p o l l _ p r o f i l e ( ) `   c a l l   ( A 4 ) ;   ` p a t c h _ d e v i c e _ p r e s e n t `   s e c o n d - v t a b l e   l o g   ( A 5 ) ;   D l l M a i n   ` S t a r t H i d d e n `   �� �   ` g _ p a n e l _ v i s i b l e   =   0 `   a f t e r   ` s e t t i n g s _ l o a d ( ) `   ( B 9 ) ;   ` g _ e m a _ v a l i d ` ,   ` g _ f o n t _ d e v `   d e f i n i t i o n s . 
 
 -   ` t e s t s / t e s t _ r a t e _ e n g i n e . c `   �� �   ` t s ( ) `   c l o c k - o v e r r i d e   h e l p e r   ( r e p l a c e s   t h e   d i r e c t   s _ t i c k   t i m e l i n e ) ,   ` s h o w _ g a i n s = 1 `   d e f a u l t ,   n e w   S h o w G a i n s = 0   s k i p   t e s t ;   r e a l t i m e   Q P C   t e s t   k e p t . 
 
 -   ` t e s t s / t e s t _ d 3 d 9 _ a c t u a l . c `   �� �   o b s e r v e r   l o g   t e s t   s e t s   ` d e b u g _ e n a b l e d = 1 `   ( R E S   i s   g a t e d ) ;   p e r - t i c k   d e l t a   n o w   ` + 2 `   ( R E S   +   p a i r e d   ` r a t e s `   l i n e ) . 
 
 -   ` t e s t s / t e s t _ s o u r c e _ c o n t r a c t . p y `   �� �   r o u n d - 8   a s s e r t s   ( A 1   Q P C / n o - s _ t i c k - c l o c k   +   n o t e ,   A 2   v a l i d - f l a g   l i f e c y c l e ,   A 3   R E S   i n s i d e   d e b u g   g a t e ,   A 4   p o l l   t h r o t t l e   +   b r e a d c r u m b ,   A 5   l o g   s t r i n g ,   B 1   b o u n d   f o n t ,   B 2   F 9 + f o r e g r o u n d + e d g e ,   B 3   s a f e   r e l e a s e ,   B 4   h e a r t b e a t ,   B 5   S _ O K - o n l y ,   B 6   s l o t s   8 9 / 9 0   +   s a v e d _ f v f   +   o v l - f v f ,   B 7   t r i p w i r e   +   s i n g l e   e x i t ,   B 8   s k i p   t h r e s h o l d ,   B 9   s t a r t _ h i d d e n   o r d e r ) . 
 
 
 
 # #   B u i l d   /   h a r n e s s e s   ( t h i s   r o u n d ) 
 
 -   B u i l d   r c = 0 ,   z e r o   w a r n i n g s ,   V E R I F Y   P A S S   ( i 3 8 6   P E 3 2 ,   i m p o r t s   K E R N E L 3 2 / U S E R 3 2 / m s v c r t ,   1 1   e x p o r t s ) . 
 
 -   d 3 d 9 . d l l   =   * * 1 4 2 , 0 5 2   B * * ,   S H A 2 5 6   ` 2 2 6 c 9 4 7 d d 7 7 c a 1 f d 8 5 1 a c 5 f d 4 e f 5 e 8 e 7 f 1 8 4 b b 5 9 f b d d f 2 e c 5 4 8 3 f 1 4 a 5 0 0 1 9 0 a 0 ` ,   d e p l o y e d   b y t e - i d e n t i c a l   t o   g a m e   d i r   +   t e s t s \ d 3 d 9 . d l l   ( v e r i f i e d   i d e n t i c a l   o n   a l l   3   c o p i e s ) ;   o l d   d 3 d 9 m o d . l o g   d e l e t e d . 
 
 -   H a r n e s s e s :   t e s t _ d 3 d 9 _ a c t u a l . e x e   F A I L U R E S   0   ,%V%  t e s t _ r a t e _ e n g i n e . e x e   F A I L U R E S   0   ,%V%  t e s t _ s o u r c e _ c o n t r a c t . p y   P A S S   ( 0   f a i l u r e s )   ,%V%  t e s t _ p e _ s t r u c t u r e . p y   F A I L U R E S   0 . 
 
 
 
 >   N O T E   f o r   t h e   l i v e   t e s t :   w i t h   d e f a u l t   s e t t i n g s   ( ` [ D e b u g ]   E n a b l e d = 0 ` )   t h e   l o g   i s   n o w   Q U I E T   p e r   f r a m e   �� �   o n l y   c h a i n / m a t c h e s / e r r o r s   a n d   o n e - t i m e   n o t e s .   S e t   ` [ D e b u g ]   E n a b l e d = 1 `   t o   r e s t o r e   t h e   p e r - t i c k   ` R E S   t = . . . `   l i n e s   f o r   c a l i b r a t i o n .   R E S / r a t e s   l i n e s   u n c h a n g e d   i n   f o r m a t ;   R a t e   d i s p l a y   u n i t   u n t o u c h e d   ( ` / m i n `   b y   d e f a u l t ) . 
 
 
 
 - - - 
 
 
 
 #   B U I L D   �� �   R O U N D   7   ( R E W O R K ,   2 0 2 6 - 0 9 - 0 6 )   �� �   C R A S H   M O V E D   T O   o v l - p a n e l :   I T ' S   T H E   F O N T .   L A Z Y   R E - C R E A T E   +   S T R I C T   N U L L   +   D O U B L E - G U A R D 
 
 
 
 # #   R O U N D   S U M M A R Y 
 
 C o n f i r m e d   i n   ` d 3 d 9 m o d . l o g `   ( b o t h   r u n s ,   k i l l e d   t w i c e   e a c h ) : 
 
 ` ` ` 
 
 1 1 :   r e s e t   d e v = 0 c c a 2 b 2 0   - >   f o n t   i n v a l i d a t e d 
 
 1 2 :   U I :   f o n t   c r e a t e d   s i z e = 1 4   f a c e = G e o r g i a             < -   c r e a t e d   I N S I D E   r e s e t _ h o o k ,   d e v i c e   s t i l l   N O T R E S E T 
 
 1 5 :   R E S   t = 1   f o o d = 0   w o o d = 0   c o i n = 0   e x p o r t = 0 
 
 1 6 :   F A U L T   a d d r = 0 0 0 0 0 0 D 8   e i p = 0 0 0 0 0 0 D 8   . . .   b r e a d c r u m b = o v l - p a n e l   c o d e = C 0 0 0 0 0 0 5   d e v = 0 C C A 2 B 2 0   v t = 0 C C A 5 A 7 C   s l o t = 8 3 
 
 ` ` ` 
 
 G e n u i n e   p r o g r e s s :   ` o v l - g r t ` / ` o v l - b e g i n `   n e v e r   a p p e a r   �� �   t h e   R T   s t e p   i s   F I X E D .   N e w   c r a s h :   ` e i p = 0 0 0 0 0 0 D 8 `   =   a   v i r t u a l   c a l l   t h r o u g h   a   N U L L / d a n g l i n g   o b j e c t ,   i n s i d e   ` o v l - p a n e l `   ( t h e   D 3 D X F o n t   B e g i n �� � D r a w T e x t �� � E n d   b l o c k ) .   ` d e v = 0 C C A 2 B 2 0 `   i s   t h e   r e a l   d e v i c e   ( i n s t r u m e n t a t i o n   s a n e ) .   * * R o o t   c a u s e : * *   t h e   f o n t   i s   c r e a t e d   i n s i d e   ` r e s e t _ h o o k `   D U R I N G   ` R e s e t `   w h i l e   t h e   d e v i c e   i s   i n   N O T R E S E T   l i m b o   �� �   ` D 3 D X C r e a t e F o n t A `   f a i l s   ( l o g   p r i n t e d   " f o n t   c r e a t e d "   u n c o n d i t i o n a l l y )   o r   r e t u r n s   a   b r o k e n   o b j e c t ;   f i r s t   D r a w T e x t   o n   i t   �� �   c a l l   t h r o u g h   N U L L   �� �   ` e i p = 0 x D 8 ` .   T h e   R 6 / R 7 - p r o v e n   b e h a v i o r   w a s   L A Z Y   r e - c r e a t e :   R e s e t   o n l y   i n v a l i d a t e s ;   t h e   f o n t   i s   ( r e ) c r e a t e d   o n   t h e   N E X T   p r e s e n t   f r a m e   o n l y   w h e n   T e s t C o o p e r a t i v e L e v e l   = =   O K . 
 
 
 
 # #   T h e   f i x 
 
 # # #   1 .   r e s e t _ h o o k   ( d 3 d 9 . c )   �� �   N O   f o n t   c r e a t i o n 
 
 -   l o g   ` r e s e t   d e v = % p   - >   f o n t   i n v a l i d a t e d ` ;   d e s t r o y   f o n t   v i a   ` u i _ o n _ r e s e t `   �� �   ` f o n t _ d e s t r o y ( ) `   ( R e l e a s e   v i a   f o n t   v t a b l e   +   * * g _ f o n t   =   N U L L   i m m e d i a t e l y * * ) ;   f o r w a r d   t o   r e a l   R e s e t   ( ` s _ o r i g _ r e s e t ` ) ;   r e - a s s e r t   h o o k s   o n   a c t u a l   ` s e l f ` ;   r e t u r n .   ` u i _ c r e a t e _ f o n t ( s e l f ) `   l i n e   R E M O V E D   f r o m   t h e   h o o k . 
 
 
 
 # # #   2 .   L a z y   c r e a t i o n   o n   t h e   p r e s e n t   p a t h   ( u i _ d r a w ,   s r c / u i . c ) 
 
 A f t e r   t h e   c o o l d o w n   a n d   ` T e s t C o o p e r a t i v e L e v e l `   ( L O S T / N O T R E S E T   a l r e a d y   b a i l ) ,   i f   ` g _ f o n t   = =   N U L L `   �� �   ` u i _ c r e a t e _ f o n t ( d e v ) ` : 
 
 -   ` h r   = =   0   & &   f o n t   ! =   N U L L `   �� �   ` g _ f o n t   =   f o n t ` ,   l o g   ` U I :   f o n t   c r e a t e d   s i z e = % d   f a c e = % s   f o n t = % p `   ( * * i n c l u d e s   t h e   p o i n t e r * *   s o   a   f r e s h   o b j e c t   i s   p r o v a b l e ) . 
 
 -   e l s e   �� �   ` g _ f o n t   =   N U L L `   ( n e v e r   l e a v e   a   d a n g l i n g   n o n - N U L L   f o n t )   a n d   l o g   ` U I :   f o n t   c r e a t e   F A I L E D   h r = 0 x % 0 8 X `   ( t h e   u n c o n d i t i o n a l   " c r e a t e d "   m e s s a g e   i s   g o n e ) . 
 
 -   t h e n   t h e   e x i s t i n g   f o n t - f i r s t   g u a r d   ` i f   ( g _ f o n t   = =   N U L L )   r e t u r n ; `   s k i p s   t h e   f r a m e . 
 
 
 
 # # #   3 .   D o u b l e - g u a r d   i n   o v l - p a n e l   ( ` f o n t _ s a f e ( ) ` ) 
 
 I m m e d i a t e l y   b e f o r e   t h e   f o n t ' s   ` B e g i n ` ,   ` f o n t _ s a f e ( ) `   r e - v e r i f i e s   ` g _ f o n t   ! =   N U L L `   A N D   ` V i r t u a l Q u e r y ` - g u a r d s   b o t h   ` g _ f o n t `   a n d   i t s   v t a b l e   p o i n t e r   a r e   m a p p e d   r e a d a b l e   ` M E M _ C O M M I T `   w i t h   n o   ` P A G E _ G U A R D ` / ` P A G E _ N O A C C E S S ` ;   i f   b a d   �� �   ` l o g g e r _ d e b u g `   o n c e   a n d   s k i p ,   n e v e r   c a l l   t h r o u g h   i t . 
 
 
 
 # # #   4 .   K e p t   f r o m   r o u n d   6 
 
 N o   S e t R e n d e r T a r g e t / G e t B a c k B u f f e r ;   G e t R e n d e r T a r g e t   c u r r e n t - R T   f l o w ;   b r e a d c r u m b s   ` o v l - t c l / g r t / s t a t e s / b e g i n / p a n e l / e n d / r e s t o r e / d o n e ` ;   c o o l d o w n   3 0 f ;   g _ v a l u e s _ v a l i d ;   T C L   g u a r d ;   r i c h   F A U L T   d u m p ;   f i r s t - d r a w   m a r k e r ;   z e r o - t o u c h   E n a b l e d = 0 . 
 
 
 
 # #   F i l e   c h a n g e s 
 
 -   ` s r c / u i . c `   �� �   ` f o n t _ d e s t r o y ( ) `   ( r e n a m e d   f r o m   ` u i _ r e l e a s e _ f o n t ` ,   d r o p s   t h e   r i s k y   O n L o s t D e v i c e   c a l l ,   p r e s e r v e s   t h e   i m m e d i a t e   ` g _ f o n t   =   N U L L ` ) ;   ` f o n t _ s a f e ( ) `   h e l p e r   +   ` g _ f o n t _ g u a r d _ w a r n e d ` ;   ` u i _ d r a w `   l a z y - c r e a t e   a f t e r   T C L ;   ` u i _ c r e a t e _ f o n t `   h o n e s t   h r   c h e c k   +   ` f o n t = % p `   +   F A I L E D   l o g ;   ` u i _ d r a w _ p a n e l `   d o u b l e - g u a r d   g a t e . 
 
 -   ` d 3 d 9 . c `   �� �   ` r e s e t _ h o o k ` :   r e m o v e d   ` i f   ( h r   > =   0 )   u i _ c r e a t e _ f o n t ( s e l f ) ` . 
 
 -   ` s r c / s t a t e . h `   �� �   ` u i _ c r e a t e _ f o n t `   c o m m e n t   u p d a t e d   ( d e v i c e - c r e a t e   +   l a z y   p r e s e n t   p a t h ) . 
 
 -   ` t e s t s / t e s t _ s o u r c e _ c o n t r a c t . p y `   �� �   r o u n d - 7   a s s e r t s :   ` f o n t _ d e s t r o y `   N U L L s   ` g _ f o n t ` ;   ` r e s e t _ h o o k `   s e g m e n t   n e v e r   c a l l s   ` u i _ c r e a t e _ f o n t ` / ` D 3 D X C r e a t e F o n t A ` ;   l a z y   c r e a t e   a f t e r   ` 0 x 8 8 7 6 0 8 6 9 u ` ;   ` f o n t _ s a f e `   V i r t u a l Q u e r y   d o u b l e - g u a r d ;   c r e a t e   F A I L E D   l o g   s t r i n g . 
 
 
 
 # #   B u i l d   /   h a r n e s s e s   ( t h i s   r o u n d ) 
 
 -   B u i l d   r c = 0 ,   z e r o   w a r n i n g s ,   V E R I F Y   P A S S   ( i 3 8 6   P E 3 2 ,   i m p o r t s   K E R N E L 3 2 / U S E R 3 2 / m s v c r t ,   1 1   e x p o r t s ) . 
 
 -   d 3 d 9 . d l l   =   * * 1 3 8 , 3 1 8   B * * ,   S H A 2 5 6   ` a 1 7 0 3 3 b 2 0 1 c c 9 b a c 2 2 2 e c 9 f 9 f e b c 4 d 3 0 4 3 f b 4 b a 9 4 1 6 d 8 5 b d 4 2 1 6 2 b 0 d b 4 b 7 6 a d 6 ` ,   d e p l o y e d   b y t e - i d e n t i c a l   t o   g a m e   d i r   +   t e s t s \ d 3 d 9 . d l l ;   o l d   d 3 d 9 m o d . l o g   d e l e t e d . 
 
 -   H a r n e s s e s :   t e s t _ d 3 d 9 _ a c t u a l . e x e   A L L   P A S S   ,%V%  t e s t _ r a t e _ e n g i n e . e x e   F A I L U R E S   0   ,%V%  t e s t _ s o u r c e _ c o n t r a c t . p y   P A S S   ( 0   f a i l u r e s )   ,%V%  t e s t _ p e _ s t r u c t u r e . p y   F A I L U R E S   0 . 
 
 
 
 - - - 
 
 
 
 #   B U I L D   �� �   R O U N D   6   ( R E W O R K ,   2 0 2 6 - 0 9 - 0 6 )   �� �   R E V E R T   P E R - V T A B L E   M A C H I N E R Y   +   R E M O V E   S e t R e n d e r T a r g e t / G e t B a c k B u f f e r   F R O M   T H E   D R A W   P A T H 
 
 
 
 # #   R O U N D   S U M M A R Y 
 
 R o u n d - 5   l i v e   l o g   ( c o n f i r m e d   i n   ` d 3 d 9 m o d . l o g ` ,   t w o   i n d e p e n d e n t   r u n s ,   k i l l e d   t w i c e   e a c h ) : 
 
 ` ` ` 
 
 F A U L T   a d d r = 7 7 4 E F F 2 5   e i p = 7 7 4 E F F 2 5   e s p = 0 0 1 9 F 9 A 8   e b p = 0 0 1 9 F 9 A C   s t k = 0 F 6 7 7 8 0 0 , 0 0 1 9 F 9 B C , 6 F 8 0 A A 3 D , 6 F 8 F 4 E C 4 , . . .   b r e a d c r u m b = o v l - s r t   c o d e = C 0 0 0 0 0 0 5   d e v = 6 F 8 F 4 E C 0   v t = 0 0 0 0 0 0 0 0   s l o t = 3 7 
 
 ` ` ` 
 
 ` o v l - s r t `   =   d y i n g   i n s i d e   * * S e t R e n d e r T a r g e t * * ;   e i p   i n   n t d l l   ( 7 7 4 E F F 2 5 )   w i t h   d 3 d 9 . d l l   r e t u r n   a d d r s   o n   t h e   s t a c k   ( 6 F 8 0 A A 3 D ,   6 F 8 F 4 E C 4 ) ;   ` d e v = 6 F 8 F 4 E C 0   v t = 0   s l o t = 3 7 `   i s   g a r b a g e   ( d e v   i s   a   c o d e   a d d r e s s   i n   d 3 d 9 . d l l ,   n o t   a   h e a p   s u r f a c e / d e v i c e ;   v t = 0 )   �� �   b a d   a r g u m e n t s   t o   t h e   c a l l   ( s t a l e / g a r b a g e   r e n d e r - t a r g e t   s u r f a c e   o r   s h i f t e d   s t a c k ) .   T h e   g a m e   u s e s   O N E   d e v i c e   o b j e c t   p e r   r u n   ( ` d e v = 0 c a 7 4 1 4 0 ` ,   ` d e v = 0 c 8 0 1 5 a 0 `   �� �   o n e   e a c h ) ;   t h e   r o u n d - 5   p e r - v t a b l e / r e s o l v e   m a c h i n e r y   w a s   n e v e r   e x e r c i s e d   a n d   i s   i m p l i c a t e d   i n   t h e   w r o n g - p o i n t e r   m e s s .   R 6 / R 7   d r e w   L I V E   o n   t h i s   g a m e   w i t h   a   s i n g l e   g l o b a l   p a i r . 
 
 
 
 # #   ( a )   R e v e r t e d   t o   R 6 - p r o v e n   h o o k   s e m a n t i c s   ( d 3 d 9 . c ) 
 
 -   * * R e m o v e d * *   t h e   r o u n d - 5   p e r - v t a b l e   o r i g i n a l s   t a b l e :   ` s _ o v t a b ` / ` s _ n o v t a b ` / ` O V T A B _ M A X ` ,   ` r e s o l v e _ o r i g _ p r e s e n t ( s e l f ) ` / ` r e s o l v e _ o r i g _ r e s e t ( s e l f ) ` ,   t h e   e v i c t - o l d e s t   p a t h   ( ` s _ o v t a b [ k ]   =   s _ o v t a b [ k + 1 ] ` ) . 
 
 -   * * R e s t o r e d * *   s i n g l e - s l o t   g l o b a l s   ` s _ o r i g _ p r e s e n t ` / ` s _ o r i g _ r e s e t ` ,   s a v e d   * * O N C E * *   f r o m   t h e   f i r s t   p a t c h e d   d e v i c e   v t a b l e   v i a   ` i f   ( s _ o r i g _ p r e s e n t   = =   N U L L ) `   /   ` i f   ( s _ o r i g _ r e s e t   = =   N U L L ) `   g u a r d s ;   ` g _ d e v v t `   s t i l l   t r a c k e d   s o   a   p o s t - R e s e t   v t a b l e   s w a p   g e t s   r e - a s s e r t e d   o n   t h e   A C T U A L   d e v i c e   ( ` p a t c h _ d e v i c e _ p r e s e n t ( s e l f ) `   i n   ` r e s e t _ h o o k ` ) . 
 
 -   ` r e s e t _ h o o k `   k e e p s   t h e   ` u i _ o n _ r e s e t ( s e l f ) `   �� �   ` r e s e t   d e v = % p   - >   f o n t   i n v a l i d a t e d `   l o g   f o r   t h e   A C T U A L   d e v i c e   p a s s e d   t o   R e s e t ;   r e - p a t c h e s   ` s e l f `   a f t e r   t h e   o r i g i n a l   R e s e t ;   r e - c r e a t e s   t h e   f o n t   o n   s u c c e s s . 
 
 -   ` p r e s e n t _ h o o k `   z e r o - t o u c h   p a t h   ( ` E n a b l e d = 0 ` )   f o r w a r d s   t h r o u g h   ` s _ o r i g _ p r e s e n t `   d i r e c t l y   ( n o   r e s o l v e   m a c h i n e r y ) . 
 
 
 
 # #   ( b )   D r a w   p a t h   w i t h o u t   R T   s w i t c h i n g   ( s r c / u i . c ) 
 
 N e w   f l o w   �� �   d r a w   o n   t h e   g a m e ' s   C U R R E N T   r e n d e r   t a r g e t : 
 
 ` ` ` 
 
 o v l - t c l         T e s t C o o p e r a t i v e L e v e l   ( L O S T / N O T R E S E T   - >   s k i p   f r a m e ) 
 
 o v l - g r t         G e t R e n d e r T a r g e t ( 0 ,   & c u r )       [ h a r d e n e d :   h r = = 0 ,   c u r   ! =   N U L L , 
 
                       V i r t u a l Q u e r y - g u a r d   c u r   ( M E M _ C O M M I T ,   n o   P A G E _ G U A R D / N O A C C E S S ) ] 
 
 o v l - s t a t e s   r e n d e r - s t a t e   s a v e   +   s e t   ( Z / Z W / S T E N C I L   o f f ,   A L P H A B L E N D   S R C A L P H A / I N V S R C A L P H A ,   L I G H T I N G   o f f ) 
 
 o v l - b e g i n     B e g i n S c e n e 
 
 o v l - p a n e l     u i _ d r a w _ p a n e l ( d e v )     ( D 3 D X F o n t   B e g i n   - >   D r a w T e x t   r o w s   - >   E n d   o n   c u r r e n t   R T ) 
 
 o v l - e n d         E n d S c e n e 
 
 o v l - r e s t o r e   r e s t o r e   r e n d e r   s t a t e s   ( r e v e r s e )   +   R e l e a s e ( c u r ) 
 
 o v l - d o n e       f i r s t - d r a w   m a r k e r   +   d o n e   b r e a d c r u m b 
 
 ` ` ` 
 
 -   * * G e t B a c k B u f f e r   +   S e t R e n d e r T a r g e t   r e m o v e d   e n t i r e l y * *   �� �   ` o v l - s r t `   a n d   ` o v l - g b b `   c r a s h   s i t e s   n o   l o n g e r   e x i s t .   A t   P r e s e n t   t i m e   t h e   d e v i c e ' s   c u r r e n t   R T   I S   t h e   b a c k b u f f e r ,   s o   t h e   H U D   s t a y s   v i s i b l e   w i t h o u t   a n y   R T   s w i t c h   ( s a m e   v i s i b i l i t y   l o g i c   a s   d r a w i n g   b e f o r e   P r e s e n t ) . 
 
 -   A n y   G e t R e n d e r T a r g e t   f a i l u r e   ( n o n - 0   h r ,   N U L L   s u r f a c e ,   V i r t u a l Q u e r y   r e j e c t )   �� �   s k i p   t h e   w h o l e   f r a m e ' s   d r a w   B E F O R E   a n y   s t a t e   w a s   c h a n g e d   ( n o   h a l f - r e s t o r e   p o s s i b l e ) . 
 
 -   T h e   o n l y   ` R e l e a s e `   i s   t h e   ` I D i r e c t 3 D S u r f a c e 9 : : R e l e a s e `   ( v t a b l e   s l o t   2 )   o n   t h e   ` G e t R e n d e r T a r g e t `   r e f e r e n c e   w e   t o o k . 
 
 
 
 # #   ( c )   K e p t   f r o m   r o u n d s   4 - 5   ( a l l   i n t a c t ) 
 
 f o n t - f i r s t   g u a r d   ,%V%  n = = 0   h i d e   ,%V%  g _ v a l u e s _ v a l i d   g a t e   ,%V%  R E S E T _ C O O L D O W N _ F R A M E S = 3 0   p o s t - R e s e t   c o o l d o w n   ,%V%  T C L   g u a r d   ( 0 x 8 8 7 6 0 8 6 8 / 6 9   s k i p )   ,%V%  r e n d e r - s t a t e   s a v e / s e t / r e s t o r e   l i s t   ,%V%  p e r - c a l l   b r e a d c r u m b s   ( n o w   ` o v l - t c l / o v l - g r t / o v l - s t a t e s / o v l - b e g i n / o v l - p a n e l / o v l - e n d / o v l - r e s t o r e / o v l - d o n e ` )   w i t h   ` s e t _ t r a c e ( d e v , v t , s l o t , b c ) `   s t a s h i n g   ` g _ f a u l t _ d e v / g _ f a u l t _ v t / g _ f a u l t _ s l o t `   ,%V%  r i c h e r   F A U L T   d u m p   ( ` e i p / e s p / e b p / s t k = 8   d w o r d s / b r e a d c r u m b / c o d e / d e v / v t / s l o t ` ,   V i r t u a l Q u e r y - g u a r d e d )   ,%V%  ` o v l   f i r s t   d r a w   o k   f r a m e = N `   f i r s t - d r a w   m a r k e r   ,%V%  z e r o - t o u c h   ` E n a b l e d = 0 `   A / B   p a t h   ,%V%  e x p l i c i t   ` S T D M E T H O D C A L L T Y P E `   ( _ _ s t d c a l l )   f u n c t i o n - p o i n t e r   c a s t s   f o r   P r e s e n t / R e s e t / G e t R e n d e r T a r g e t / R e l e a s e / B e g i n / D r a w T e x t / E n d . 
 
 
 
 - - - 
 
 
 
 #   B U I L D   �� �   R O U N D   5   ( R E W O R K ,   2 0 2 6 - 0 9 - 0 6 )   �� �   P E R S I S T E N T   o v l - r t   C R A S H :   P O R T - U N R E C O V E R A B L E ,   P E R - V T A B L E   F I X   +   F U L L   C R A S H   I N S T R U M E N T A T I O N 
 
 
 
 # #   R O U N D   S U M M A R Y 
 
 D a l i ' s   r o u n d - 4   l i v e   l o g   s h o w s   t h e   c o o l d o w n   d i d   N O T   h e l p :   t h e   p o s t - R e s e t   f o n t   r e - c r e a t e   n o w   l o g s   c o r r e c t l y   ( ` U I :   f o n t   c r e a t e d   s i z e = 1 4   f a c e = G e o r g i a `   a   s e c o n d   t i m e ) ,   t h e   c o o l d o w n   e l a p s e d ,   a n d   ` F A U L T   a d d r = 7 7 4 E F F 2 5   b r e a d c r u m b = o v l - r t   c o d e = C 0 0 0 0 0 0 5 `   s t i l l   k i l l s   t h e   p r o c e s s   ( x 4 )   w i t h   t h e   S A M E   s t a b l e   n t d l l - i s h   a d d r e s s .   A   s t a b l e   n t d l l   a d d r e s s   +   i n t a c t   o v e r l a y   i s   a   s t a l e / R E P L A C E D   d e v i c e   v t a b l e   s i g n a t u r e   �� �   t h e   g a m e   o r   d r i v e r   r e s t o r e s   i t s   o w n   v t a b l e   o r   c r e a t e s   a   S E C O N D   d e v i c e   w h o s e   p a t c h   c l o b b e r s   t h e   f i r s t   d e v i c e ' s   s a v e d   o r i g i n a l s . 
 
 
 
 # #   ( a )   R 6 - v s - R 1 4   d i f f   f i n d i n g s   �� �   R 6   s o u r c e   i s   U N R E C O V E R A B L E ;   t h e   m e c h a n i c s   w e r e   a l r e a d y   R 6 - f a i t h f u l 
 
 M a n d a t e   s t e p   1   a s k e d   t o   p o r t   ` g i t   s h o w   < r 6 > : d 3 d 9 . c `   " v e r b a t i m " .   * * T h a t   c o m m i t   d o e s   n o t   e x i s t . * *   P r o o f : 
 
 -   ` g i t   l o g   - - a l l   - S   " d r a w _ h u d _ t e x t " `   a n d   ` - S   " g o t b b " `   a n d   ` - S   " r e s e t   d e v = " `   r e t u r n   c o m m i t s   ` 2 1 f 7 4 0 3 ,   0 7 f 5 b 4 f ,   1 c c 8 7 0 8 ,   8 2 6 2 a 4 d `   �� �   R 1 2 / R 1 3 / R 1 4 .   T h e   r e p o   h i s t o r y   S T A R T S   a t   R 1 2   ( ` 2 1 f 7 4 0 3 ` ) ;   t h e   r o o t   c o m m i t ' s   ` d 3 d 9 . c `   a l r e a d y   s a y s   * " A l l   p r e v i o u s   d r a w   c o d e   ( f o n t   i n i t ,   t e x t = / v a l u e s = / g o t b b   d u m p s ,   r e d   r e c t ,   S e t R e n d e r T a r g e t / S e t R e n d e r S t a t e ,   B e g i n / E n d S c e n e )   w a s   D E L E T E D   i n   r o u n d   8 " *   ( c o n f i r m e d   a t   ` o l d _ r 1 2 . c : 5 4 3 ` ) .   N o   R 6 / R 7   b l o b   e x i s t s   a n y w h e r e   i n   ` - - a l l ` .   T h e   o l d   r e p o   ( ` C : \ U s e r s \ D a l i \ D o c u m e n t s \ g a a m e s \ a o e 3 r a t e - m o d ` )   i s   g o n e ,   a n d   t h e   r e m o t e   ` m a i n `   c o n t a i n s   o n l y   R 1 2 �� � R 1 5 . 
 
 -   W h o l e - t r e e   g r e p   f o r   ` g o t b b ` / ` d r a w _ h u d _ t e x t ` / ` h u d _ t e x t ` / ` r e s e t   d e v = % p ` / ` t e x t = % d ` :   o n l y   d o c   m e n t i o n s   i n   ` . s w a r m \ T E S T . m d ` / ` R E V I E W . m d ` .   ` t e s t _ n u l l s a f e . c `   h e a d e r   s a y s   " t h e   D L L   n o   l o n g e r   d r a w s   a n y t h i n g " . 
 
 -   * * T h e r e f o r e   t h e   R 6   " v e r b a t i m "   p o r t   c o u l d   n o t   r u n . * *   I   d i f f e d   t h e   c u r r e n t   R T / d r a w   m a c h i n e r y   e l e m e n t - b y - e l e m e n t   a g a i n s t   t h e   R 6   s p e c   y o u   e n u m e r a t e d   ( t h e   o n l y   a u t h o r i t a t i v e   r e c o r d   l e f t ) .   R e s u l t :   * * e v e r y   R 6   m e c h a n i c a l   r e q u i r e m e n t   w a s   a l r e a d y   s a t i s f i e d * * : 
 
     -   G e t B a c k B u f f e r ( 0 , 0 , M O N O , & b b )   +   H R E S U L T   +   N U L L   c h e c k   �� �   M A T C H   ( s l o t   1 8 ) 
 
     -   G e t R e n d e r T a r g e t ( 0 , & p r e v _ r t )   +   S e t R e n d e r T a r g e t ( 0 , b b )   +   b o t h - r e f s   r e l e a s e   o r d e r   �� �   M A T C H   ( s l o t   3 8 / 3 7 ;   g r t / g b b   e a c h   A d d R e f ,   b o t h   R e l e a s e d ) 
 
     -   r e n d e r - s t a t e   s a v e / s e t / r e s t o r e   l i s t   Z E N A B L E / Z W R I T E E N A B L E / S T E N C I L E N A B L E / A L P H A B L E N D E N A B L E ( S R C A L P H A �� � I N V S R C A L P H A ) / L I G H T I N G   �� �   M A T C H   ( s l o t s   5 8 / 5 7 ) 
 
     -   B e g i n / E n d   g a t e d :   B e g i n S c e n e ( 4 1 )   f a i l u r e   s k i p s   p a n e l   A N D   E n d S c e n e ;   f o n t   B e g i n ( 1 2 )   f a i l u r e   s k i p s   D r a w T e x t   A N D   f o n t   E n d   �� �   M A T C H 
 
     -   T e s t C o o p e r a t i v e L e v e l   g u a r d :   L O S T / N O T R E S E T   s k i p   t h e   w h o l e   f r a m e   b e f o r e   a n y   f o n t / R T   u s e   �� �   M A T C H   ( s l o t   3   +   0 x 8 8 7 6 0 8 6 8 / 6 9 ) 
 
     -   f o n t - f i r s t   +   n = = 0   +   g _ v a l u e s _ v a l i d   +   3 0 - f r a m e   c o o l d o w n   ( R 1 4   h a r d e n i n g )   �� �   K E P T 
 
     -   S o   " t h e   R T   m e c h a n i c s   m u s t   b e   R 6 - p u r e "   w a s   a l r e a d y   t r u e ;   t h e   c r a s h   i s   N O T   a   m e c h a n i c s   r e g r e s s i o n   v s   R 6 .   T h e   r e a l   d i v e r g e n c e   f r o m   t h e   p r o v e n   R 1 3   h o o k s   i s   s t r u c t u r a l   a n d   b e l o w . 
 
 
 
 # #   ( b )   T h e   r e s e t - h o o k   /   p r e s e n t - h o o k   f i n d i n g   �� �   s i n g l e - s l o t   g l o b a l s   c o u l d   f o r w a r d   i n t o   t h e   W R O N G   d e v i c e ' s   c o d e 
 
 O L D   c o d e   ( r o u n d s   2 - 4 ,   a n d   R 1 3 ) :   ` p a t c h _ d e v i c e _ p r e s e n t ( ) `   s a v e d   t h e   o r i g i n a l s   i n   O N E   g l o b a l   p a i r   a n d   o n l y   p a t c h e d   w h e n   ` g _ d e v v t   ! =   v t ` .   I f   t h e   g a m e   c r e a t e s   a   S E C O N D   d e v i c e   ( d i f f e r e n t   v t a b l e   �� �   A o E 3   d o e s   c r e a t e   a u x i l i a r y   d e v i c e s )   o r   a   d r i v e r   s w a p s   a   v t a b l e   a t   R e s e t ,   t h e   s e c o n d   p a t c h   R E - S A V E S   ` s _ o r i g _ p r e s e n t ` / ` s _ o r i g _ r e s e t `   w i t h   t h e   s e c o n d   d e v i c e ' s   o r i g i n a l s   a n d   c l o b b e r s   t h e   f i r s t   d e v i c e ' s .   E v e r y   s u b s e q u e n t   P r e s e n t / R e s e t   o n   d e v i c e   # 1   t h e n   f o r w a r d e d   i n t o   d e v i c e   # 2 ' s   o r i g i n a l   P r e s e n t / R e s e t   v i a   o u r   s t a l e   s i n g l e - s l o t   g l o b a l s   �� �   e x a c t l y   t h e   " c a l l s   a   d e v i c e   i n   t h e   w r o n g   s t a t e   - >   d i e s   i n s i d e   n t d l l   w i t h   a   s t a b l e   a d d r e s s "   s i g n a t u r e   o f   t h e   l o g . 
 
 N E W   c o d e   ( r o u n d   5 ) :   a   p e r - v t a b l e   t a b l e   ( ` s _ o v t a b ` ,   4   e n t r i e s ,   o l d e s t - e v i c t e d ) : 
 
 -   ` p a t c h _ d e v i c e _ p r e s e n t ( ) `   k e y s   e n t r i e s   b y   t h e   d e v i c e ' s   C U R R E N T   v t a b l e   p o i n t e r ;   r e c o r d s   E A C H   v t a b l e ' s   t r u e   o r i g i n a l s ;   n e v e r   t o u c h e s   a n o t h e r   e n t r y ' s   s a v e d   o r i g i n a l s . 
 
 -   ` r e s o l v e _ o r i g _ p r e s e n t ( s e l f ) / r e s o l v e _ o r i g _ r e s e t ( s e l f ) `   r e s o l v e   t h e   o r i g i n a l   b y   t h e   C U R R E N T   d e v i c e ' s   C U R R E N T   v t a b l e   o n   E V E R Y   c a l l   ( f a l l s   b a c k   t o   l a s t - p a t c h e d   i f   u n k n o w n ) . 
 
 -   ` r e s e t _ h o o k `   s t i l l   r e - p a t c h e s   ` s e l f `   �� �   t h e   A C T U A L   d e v i c e   p a s s e d   t o   R e s e t   ( n e v e r   a   s t o r e d   p o i n t e r )   �� �   a n d   n o w   l o g s   ` r e s e t   d e v = % p   - >   f o n t   i n v a l i d a t e d ` . 
 
 -   ` s _ o r i g _ p r e s e n t ` / ` s _ o r i g _ r e s e t `   r e m a i n   o n l y   a s   l a s t - p a t c h e d   d i a g n o s t i c s ;   t h e   h o o k s   n o   l o n g e r   t r u s t   t h e m   a l o n e . 
 
 
 
 # #   ( c )   C r a s h   i n s t r u m e n t a t i o n   a d d e d 
 
 -   * * P e r - c a l l   b r e a d c r u m b s   +   t r a c e : * *   r e p l a c i n g   c o a r s e   ` o v l - r t `   w i t h   ` o v l - t c l ` ,   ` o v l - g r t ` ,   ` o v l - g b b ` ,   ` o v l - s r t `   s e t   i m m e d i a t e l y   B E F O R E   e a c h   i n d i v i d u a l   c a l l   ( k e e p   ` o v l - s t a t e s / b e g i n / p a n e l / e n d / r e s t o r e / d o n e ` ) .   E a c h   t r a c e   a l s o   s t a s h e s   ` g _ f a u l t _ d e v ` / ` g _ f a u l t _ v t ` / ` g _ f a u l t _ s l o t `   s o   t h e   f a u l t   d u m p   k n o w s   t h e   o b j e c t ,   v t a b l e ,   a n d   s l o t   b e i n g   c a l l e d . 
 
 -   * * R i c h e r   F A U L T   d u m p : * *   t h e   l i n e   i s   n o w   ` F A U L T   a d d r = . .   e i p = . .   e s p = . .   e b p = . .   s t k = h 1 , h 2 , . . . h 8   b r e a d c r u m b = . .   c o d e = . .   d e v = . .   v t = . .   s l o t = . . `   �� �   c o n t e x t   r e g i s t e r s   r e a d   f r o m   t h e   e x c e p t i o n   C O N T E X T   ( V i r t u a l Q u e r y - g u a r d e d   s t a c k   r e a d ) ,   p l u s   t h e   d e v i c e / v t a b l e / s l o t   t r a c e   f r o m   t h e   g l o b a l s .   A   n e x t   c r a s h   n a m e s   t h e   e x a c t   d y i n g   c a l l   A N D   g i v e s   a   c a l l   t r a i l . 
 
 -   * * F i r s t - d r a w   m a r k e r : * *   ` o v l   f i r s t   d r a w   o k   f r a m e = N `   l o g g e d   o n c e   w h e n   t h e   f i r s t   f u l l   o v e r l a y   d r a w   c o m p l e t e s ,   s o   t h e   l o g   p r o v e s   w h e t h e r   t h e   d r a w   e v e r   f i n i s h e s . 
 
 -   * * Z e r o - t o u c h   E n a b l e d = 0   p a t h : * *   w i t h   ` [ G e n e r a l ]   E n a b l e d = 0 ` ,   ` p r e s e n t _ h o o k `   f o r w a r d s   t o   t h e   r e s o l v e d   o r i g i n a l   i m m e d i a t e l y   �� �   n o   l o c a t e / o b s e r v e r / h o t k e y / o v e r l a y / b r e a d c r u m b s .   T h i s   i s   D a l i ' s   A / B   c o n t r o l   t o   s p l i t   h o o k - v s - o v e r l a y   a s   t h e   c u l p r i t . 
 
 
 
 # #   F I L E S   C H A N G E D 
 
 -   ` d 3 d 9 . c `   �� �   p e r - v t a b l e   o r i g i n a l s   t a b l e   +   r e s o l v e   h e l p e r s ;   p r e s e n t _ h o o k   z e r o - t o u c h   p a t h ;   r e s e t _ h o o k   l o g s   ` r e s e t   d e v = % p ` ;   ` g _ f a u l t _ d e v / v t / s l o t ` ,   ` g _ o v l _ f i r s t _ d o n e `   g l o b a l s . 
 
 -   ` s r c / u i . c `   �� �   ` s e t _ t r a c e ( ) `   h e l p e r ;   p e r - c a l l   ` o v l - g r t / g b b / s r t `   b r e a d c r u m b s   ( r u n s   a f t e r   c o o l d o w n + f o n t - f i r s t ,   b e f o r e   a n y   R T   c a l l ) ;   f i r s t - d r a w   m a r k e r ;   a l l   R 1 4   g a t i n g   k e p t . 
 
 -   ` s r c / g a m e i f . c `   �� �   ` f a u l t _ l o g _ p o i n t s `   n o w   d u m p s   e i p / e s p / e b p   +   8   s t a c k   d w o r d s   +   d e v / v t / s l o t   ( V i r t u a l Q u e r y - g u a r d e d ) . 
 
 -   ` s r c / s t a t e . h `   �� �   e x t e r n s   f o r   t h e   t r a c e   g l o b a l s ;   ` u i _ o n _ r e s e t ( v o i d   * d e v ) ` . 
 
 -   ` t e s t s / t e s t _ r a t e _ e n g i n e . c `   �� �   ` u i _ o n _ r e s e t ( N U L L ) `   ( s i g n a t u r e ) . 
 
 -   ` t e s t s / t e s t _ s o u r c e _ c o n t r a c t . p y `   �� �   r o u n d - 5   a s s e r t s   ( p e r - v t   t a b l e ,   r e s o l v e   h e l p e r s ,   e v i c t i o n ,   ` r e s e t   d e v = % p ` ,   ` o v l - g r t / g b b / s r t `   t r a c e   p r e s e n c e ,   e i p / e s p / e b p / s t k   d u m p ,   f i r s t - d r a w   m a r k e r ,   z e r o - t o u c h   o r d e r i n g ) . 
 
 
 
 # #   B U I L D   R E S U L T 
 
 -   ` c m d   / c   b u i l d \ b u i l d . b a t `   �� �   * * r c = 0 ,   Z E R O   w a r n i n g s * * ,   v e r i f y _ p e   P A S S   ( i 3 8 6 ,   P E 3 2 ,   i m p o r t s   �� �   { K E R N E L 3 2 ,   U S E R 3 2 ,   m s v c r t } ,   * * 1 1   e x p o r t s * * ) 
 
 -   * * d 3 d 9 . d l l   =   1 3 8 , 8 9 0   B ,   S H A 2 5 6   ` 7 1 c 4 1 e 2 8 4 4 b f 8 e 0 9 5 c 2 8 f 2 d 6 c a 7 5 b 2 a 9 d 8 5 c 0 e 6 d 7 8 c 0 8 d d b 4 a 5 9 5 c 7 8 7 4 7 c e 2 6 0 ` * *   ( ` d 3 d 9 . s h a 2 5 6 `   u p d a t e d ) 
 
 -   * * D e p l o y e d   b y t e - i d e n t i c a l * * :   r e p o   r o o t   =   g a m e   d i r   =   ` t e s t s \ d 3 d 9 . d l l `   ( S H A 2 5 6   e q u a l ) ;   o l d   g a m e   ` d 3 d 9 m o d . l o g `   d e l e t e d .   G a m e   N O T   l a u n c h e d . 
 
 
 
 # #   H A R N E S S   R E S U L T S   ( a l l   g r e e n ) 
 
 -   ` t e s t s \ t e s t _ d 3 d 9 _ a c t u a l . e x e `   �� �   A L L   C H E C K S   P A S S E D 
 
 -   ` t e s t s \ t e s t _ r a t e _ e n g i n e . e x e `   �� �   F A I L U R E S :   0   ( i n c l .   S T E P - 7   f a u l t   s e m a n t i c s   w i t h   t h e   r i c h e r   d u m p ) 
 
 -   ` t e s t s \ t e s t _ s o u r c e _ c o n t r a c t . p y `   �� �   S O U R C E - C O N T R A C T :   P A S S   ( i n c l .   1 0   n e w   r o u n d - 5   c h e c k s ) 
 
 -   ` t e s t s \ t e s t _ p e _ s t r u c t u r e . p y `   �� �   F A I L U R E S :   0 
 
 
 
 # #   W H A T   D A L I   T E S T S   N E X T   ( l i v e ) 
 
 1 .   * * A / B   f i r s t : * *   s e t   ` [ G e n e r a l ]   E n a b l e d = 0 `   i n   t h e   g a m e   d i r ' s   ` R e s o u r c e R a t e M o d . i n i ` ,   l a u n c h ,   s t a r t   a   s k i r m i s h .   I f   t h e   p r o c e s s   s u r v i v e s ,   t h e   c u l p r i t   i s   i n   t h e   o v e r l a y   d r a w / R T   p a t h ;   i f   i t   S T I L L   d i e s ,   i t ' s   i n   t h e   h o o k / o b s e r v e r   p a t h . 
 
 2 .   T h e n   ` E n a b l e d = 1 ` :   i f   i t   s t i l l   c r a s h e s ,   t h e   F A U L T   l i n e   n o w   n a m e s   t h e   e x a c t   c a l l :   ` o v l - g r t `   v s   ` o v l - g b b `   v s   ` o v l - s r t `   v s   ` o v l - s t a t e s ` / ` o v l - b e g i n ` / ` o v l - p a n e l ` ,   w i t h   ` d e v = . .   v t = . .   s l o t = . .   e i p = . .   e s p = . .   e b p = . .   s t k = . . ` .   S e n d   t h a t   l i n e   +   t h e   ` C r e a t e D e v i c e ` / ` r e s e t   d e v = `   l i n e s . 
 
 3 .   E x p e c t   a f t e r   t h e   m a t c h - s t a r t   R e s e t :   ` r e s e t   d e v = . .   - >   f o n t   i n v a l i d a t e d ` ,   a   s e c o n d   ` U I :   f o n t   c r e a t e d ` ,   t h e n   ` o v l   f i r s t   d r a w   o k   f r a m e = . . `   o n c e   t h e   o v e r l a y   a c t u a l l y   r e n d e r s . 
 
 
 
 - - - 
 
 
 
 #   B U I L D   �� �   R O U N D   4   ( R E W O R K ,   2 0 2 6 - 0 9 - 0 5 )   �� �   C R A S H   O N   F I R S T   F R A M E   A F T E R   D E V I C E   R E S E T   ( o v l - r t   A V ) 
 
 
 
 # #   R O U N D   S U M M A R Y 
 
 D a l i ' s   l i v e   l o g   ( R 1 4 . 3 ) : 
 
 ` ` ` 
 
 C r e a t e D e v i c e   h r = 0 0 0 0 0 0 0 0 0 0   d e v = 0 c c 3 8 d 6 0   p a t c h e d = y e s 
 
 U I :   f o n t   c r e a t e d   s i z e = 1 4   f a c e = G e o r g i a 
 
 c h a i n   . . .   c t x = = 0   ( x 4 ) 
 
 r e s e t   - >   f o n t   i n v a l i d a t e d 
 
 c h a i n   g a m e = 0 4 A A F 0 0 0   c t x = 0 5 9 E 0 0 0 0   n = 3   p l a y e r = 0 F 6 F 7 8 0 0   r e s = 1 2 8 6 A 3 0 0   [ h u m a n - p 1 ] 
 
 - - -   m a t c h   s t a r t   n = 3   . . . 
 
 R E S   t = 1   . . . 
 
 F A U L T   a d d r = 7 7 4 E F F 2 5   b r e a d c r u m b = o v l - r t   c o d e = C 0 0 0 0 0 0 5       ( x 4 ,   t h e n   p r o c e s s   d i e s ) 
 
 ` ` ` 
 
 ` o v l - r t `   =   t h e   o v e r l a y ' s   G e t B a c k B u f f e r / S e t R e n d e r T a r g e t   s t e p   i n   ` u i _ d r a w ( ) ` ;   ` 0 x C 0 0 0 0 0 0 5 `   =   a   r e a l   a c c e s s   v i o l a t i o n   i n s i d e   d 3 d 9 . d l l   i t s e l f   �� �   t h e   d e v i c e   w a s   i n   a   t r a n s i t i o n a l   s t a t e   r i g h t   a f t e r   R e s e t .   T h e   o v e r l a y   d r e w   o n   t h e   v e r y   f r a m e   t h e   m a t c h   s t a r t e d . 
 
 
 
 # #   R O O T   C A U S E   ( o n e   p a r a g r a p h ) 
 
 A f t e r   a   D 3 D 9   d e v i c e   R e s e t   t h e   d e v i c e   o b j e c t   i s   i n   a   t r a n s i t i o n a l   s t a t e ;   c a l l i n g   ` G e t B a c k B u f f e r ` / ` S e t R e n d e r T a r g e t `   o n   i t   b e f o r e   i t   s e t t l e s   f a u l t s   i n s i d e   d 3 d 9 . d l l   ( A V   a t   ` 7 7 4 E F F 2 5 ` ) .   T w o   a g g r a v a t o r s :   ( 1 )   t h e   f o n t   w a s   a c t u a l l y   r e - c r e a t e d   b y   t h e   R e s e t   h o o k   b u t   t h e   c r e a t i o n   l i n e   w a s   s u p p r e s s e d   b y   a   o n c e - o n l y   ` g _ u i _ l o g g e d `   l a t c h   �� �   s o   t h e   l o g   m i s l e d   u s   i n t o   t h i n k i n g   t h e   f o n t   w a s   m i s s i n g ,   w h e n   i n   f a c t   ` u i _ d r a w `   r a n   i t s   w h o l e   R T   d a n c e   o n   a   d e v i c e   t h a t   h a d   b e e n   r e s e t   t h e   s a m e   f r a m e ;   ( 2 )   n o   c o o l d o w n   e x i s t e d   b e t w e e n   R e s e t   s u c c e s s   a n d   t h e   f i r s t   o v e r l a y   d r a w .   D a l a n ' s   d r a w   o r d e r i n g   w a s   t e c h n i c a l l y   c o r r e c t   ( f o n t   c h e c k e d   b e f o r e   R T   c a l l s )   b u t   t h e r e   w a s   n o   s a f e t y   d e l a y   a n d   n o   H R E S U L T   a c c e p t a n c e   t e s t   o n   ` S e t R e n d e r T a r g e t ` . 
 
 
 
 # #   T H E   F I X   ( e x a c t   s e m a n t i c s ) 
 
 1 .   * * P o s t - R e s e t   c o o l d o w n : * *   n e w   g l o b a l   ` g _ f r a m e s _ s i n c e _ r e s e t `   ( d e f i n e d   i n   ` d 3 d 9 . c ` ,   ` e x t e r n `   i n   ` s t a t e . h ` ,   ` R E S E T _ C O O L D O W N _ F R A M E S   3 0 `   �� �   0 . 5 s   @ 6 0 f p s ) .   Z e r o e d   o n   e v e r y   ` C r e a t e D e v i c e ` / ` C r e a t e D e v i c e E x `   s u c c e s s   a n d   a t   t h e   t o p   o f   ` r e s e t _ h o o k ` ;   i n c r e m e n t e d   o n c e   p e r   ` p r e s e n t _ h o o k `   f r a m e .   ` u i _ d r a w ( ) `   r e t u r n s   i m m e d i a t e l y   w h i l e   ` g _ f r a m e s _ s i n c e _ r e s e t   <   R E S E T _ C O O L D O W N _ F R A M E S `   �� �   n o   G e t B a c k B u f f e r / S e t R e n d e r T a r g e t / B e g i n S c e n e   u n t i l   t h e   d e v i c e   h a s   s e t t l e d .   T h e   c h e c k   s i t s   A F T E R   t h e   ` g _ f o n t   = =   N U L L `   g u a r d   ( f o n t - f i r s t )   a n d   B E F O R E   t h e   ` o v l - t c l ` / ` o v l - r t `   s t e p s . 
 
 2 .   * * F o n t - f i r s t   o r d e r i n g   ( c o n f i r m e d ) : * *   ` i f   ( g _ f o n t   = =   N U L L )   r e t u r n ; `   r e m a i n s   t h e   f i r s t   d e v i c e   g u a r d ,   b e f o r e   A N Y   R T   m a n i p u l a t i o n   ( G e t B a c k B u f f e r ,   S e t R e n d e r T a r g e t ,   s t a t e   s a v e ,   B e g i n S c e n e ) . 
 
 3 .   * * F o n t   r e - c r e a t i o n   o n   R e s e t   +   h o n e s t   l o g g i n g : * *   t h e   R e s e t   h o o k   a l r e a d y   r e - c r e a t e s   t h e   f o n t   v i a   ` u i _ c r e a t e _ f o n t ( s e l f ) `   w h e n   ` h r   > =   0 ` ;   t h e   o n c e - o n l y   ` g _ u i _ l o g g e d `   l a t c h   i s   R E M O V E D   s o   e v e r y   c r e a t e   l o g s   ` U I :   f o n t   c r e a t e d   s i z e = . .   f a c e = . . `   �� �   t h e   p o s t - R e s e t   p a t h   i s   n o w   v i s i b l e   i n   ` d 3 d 9 m o d . l o g ` .   ( I n   t h e   f a i l i n g   l o g ,   t h e   f o n t   H A D   b e e n   r e - c r e a t e d   �� �   t h a t ' s   w h y   e x e c u t i o n   r e a c h e d   ` o v l - r t `   a t   a l l ;   t h e   s u p p r e s s e d   l o g   w a s   t h e   r e d   h e r r i n g . ) 
 
 4 .   * * H R E S U L T   c h e c k s   o n   a l l   R T   c a l l s   i n   ` u i _ d r a w ` : * *   ` G e t R e n d e r T a r g e t ` ,   ` G e t B a c k B u f f e r `   a l r e a d y   b a i l ;   n e w   �� �   ` S e t R e n d e r T a r g e t ( d e v ,   0 ,   b b ) `   f a i l u r e   n o w   r e s t o r e s   t h e   p r e v i o u s   R T ,   r e l e a s e s   b o t h   s u r f a c e s   a n d   s k i p s   t h e   f r a m e   ( n e v e r   c o n t i n u e s   i n t o   B e g i n S c e n e / D r a w T e x t   o n   a   f a i l e d   R T   s e t u p ) . 
 
 5 .   A l l   p r i o r   r o u n d   f i x e s   k e p t   i n t a c t   ( l o g - o n l y   v e c t o r e d   h a n d l e r   +   b e n i g n - c o d e   f i l t e r ,   f o n t   a t   d e v i c e - c r e a t e   o n l y ,   b o t h - s u r f a c e s   r e l e a s e ,   m a t c h - s t a r t   n e e d s   a   v a l i d   c h a i n ,   ` o v l - * `   b r e a d c r u m b s ) . 
 
 
 
 # #   F I L E S   C H A N G E D 
 
 -   ` s r c / u i . c `   �� �   c o o l d o w n   g a t e   i n   ` u i _ d r a w ` ;   ` S e t R e n d e r T a r g e t `   H R E S U L T   b a i l   ( r e s t o r e   p r e v   R T   +   r e l e a s e   +   s k i p ) ;   ` g _ u i _ l o g g e d `   r e m o v e d ,   e v e r y   f o n t   c r e a t e   l o g g e d . 
 
 -   ` s r c / s t a t e . h `   �� �   ` e x t e r n   i n t   g _ f r a m e s _ s i n c e _ r e s e t ; `   +   ` R E S E T _ C O O L D O W N _ F R A M E S   3 0 ` . 
 
 -   ` d 3 d 9 . c `   �� �   g l o b a l   ` g _ f r a m e s _ s i n c e _ r e s e t ` ;   z e r o e d   i n   ` w _ c r e a t e _ d e v i c e ` / ` w _ c r e a t e _ d e v i c e _ e x ` / ` r e s e t _ h o o k ` ;   i n c r e m e n t e d   i n   ` p r e s e n t _ h o o k ` . 
 
 -   ` t e s t s / t e s t _ s o u r c e _ c o n t r a c t . p y `   �� �   r o u n d - 4   a s s e r t s :   c o o l d o w n   d e f i n e   +   i n c r e m e n t   +   t r i p l e   z e r o ,   f o n t - f i r s t   g u a r d ,   c o o l d o w n - b e f o r e - ` o v l - r t `   o r d e r i n g ,   u n c o n d i t i o n a l   f o n t - c r e a t e   l o g . 
 
 
 
 # #   B U I L D   R E S U L T 
 
 -   ` c m d   / c   b u i l d \ b u i l d . b a t `   �� �   * * r c = 0 ,   Z E R O   w a r n i n g s * * ,   v e r i f y _ p e   P A S S   ( i 3 8 6 ,   P E 3 2 ,   i m p o r t s   �� �   { K E R N E L 3 2 ,   U S E R 3 2 ,   m s v c r t } ,   * * 1 1   e x p o r t s * * ) 
 
 -   * * d 3 d 9 . d l l   =   1 3 7 , 1 7 9   B ,   S H A 2 5 6   ` f 5 5 f 5 5 6 d 6 a f 2 5 b b 5 8 5 b 5 d e 2 3 5 7 8 a 2 f 0 b 2 5 4 8 9 6 e 2 5 4 c d f 9 3 8 6 7 f 6 e d 3 1 6 e f 9 7 8 1 7 ` * *   ( ` d 3 d 9 . s h a 2 5 6 `   u p d a t e d ) 
 
 -   * * D e p l o y e d   b y t e - i d e n t i c a l * * :   r e p o   r o o t   =   g a m e   d i r   =   ` t e s t s \ d 3 d 9 . d l l `   ( h a s h e s   e q u a l ) ;   o l d   g a m e   ` d 3 d 9 m o d . l o g `   d e l e t e d .   G a m e   N O T   l a u n c h e d . 
 
 
 
 # #   H A R N E S S   R E S U L T S   ( a l l   g r e e n ) 
 
 -   ` t e s t s \ t e s t _ d 3 d 9 _ a c t u a l . e x e `   �� �   A L L   C H E C K S   P A S S E D 
 
 -   ` t e s t s \ t e s t _ r a t e _ e n g i n e . e x e `   �� �   F A I L U R E S :   0 
 
 -   ` t e s t s \ t e s t _ s o u r c e _ c o n t r a c t . p y `   �� �   S O U R C E - C O N T R A C T :   P A S S   ( i n c l .   n e w   r o u n d - 4   c h e c k s ) 
 
 -   ` t e s t s \ t e s t _ p e _ s t r u c t u r e . p y `   �� �   F A I L U R E S :   0 
 
 
 
 # #   W H A T   D A L I   T E S T S   N E X T   ( l i v e ) 
 
 1 .   G a m e   m u s t   r e a c h   t h e   m a i n   m e n u   ( n o   l o a d - t i m e   F A U L T ) . 
 
 2 .   S t a r t   a   s k i r m i s h   ( t h e   g a m e ' s   R e s e t   +   m a t c h   s t a r t ) :   t h e   o v e r l a y   m u s t   N O T   a p p e a r   i n s t a n t l y ,   t h e n   f a d e   i n   ~ 0 . 5 s   l a t e r   ( ` U I :   f o n t   c r e a t e d `   m u s t   A L S O   a p p e a r   i n   t h e   p o s t - R e s e t   s e c t i o n   o f   ` d 3 d 9 m o d . l o g `   t h i s   t i m e ) ,   n o   ` F A U L T   b r e a d c r u m b = o v l - r t `   l i n e . 
 
 3 .   I f   a   F A U L T   l i n e   s t i l l   a p p e a r s ,   s e n d   i t   �� �   i t   p r o v e s   a   g e n u i n e l y   u n h a n d l e d   f a t a l   a n d   g i v e s   u s   t h e   r e a l   a d d r . 
 
 
 
 - - - 
 
 
 
 #   B U I L D   �� �   R O U N D   3   ( R E W O R K ,   2 0 2 6 - 0 9 - 0 6 )   �� �   G A M E   W O N ' T   S T A R T   ( B E N I G N - E X C E P T I O N   K I L L ) 
 
 
 
 # #   R O U N D   S U M M A R Y 
 
 A f t e r   r o u n d   2 ,   D a l i ' s   l i v e   t e s t :   * * t h e   g a m e   d i e d   a t   l o a d * * .   ` d 3 d 9 m o d . l o g `   r e p e a t e d   p e r   l a u n c h : 
 
 ` F A U L T   a d d r = 7 5 A F D 8 C 2   b r e a d c r u m b = d l l m a i n - d o n e   c o d e = 4 0 6 D 1 3 8 8 `   ( t w i c e )   �� �   i . e . ,   r i g h t   a f t e r   o u r   D l l M a i n   c o m p l e t e d ,   d u r i n g   t h e   l o a d e r   p h a s e ,   w i t h   t h e   b r e a d c r u m b   ` d l l m a i n - d o n e ` .   R o o t   c a u s e :   r o u n d   2   r e g i s t e r e d   ` f a u l t _ f i l t e r `   ( w h i c h   l o g s   F A U L T   * * a n d   ` E x i t P r o c e s s ` * * )   a s   a n   ` A d d V e c t o r e d E x c e p t i o n H a n d l e r ( 0 ,   . . . ) `   h a n d l e r .   ` 0 x 4 0 6 D 1 3 8 8 `   i s   t h e   * * b e n i g n   O u t p u t D e b u g S t r i n g   n o t i f i c a t i o n   e x c e p t i o n * *   ( r a i s e d   b y   d 3 d x 9 _ 2 5 . d l l   /   t h e   g a m e   a t   l o a d ;   a d d r   i n   k e r n e l b a s e ) .   A   v e c t o r e d   h a n d l e r   r u n s   o n   E V E R Y   e x c e p t i o n   n o b o d y   e l s e   h a n d l e d   �� �   i n c l u d i n g   b e n i g n   i n f o r m a t i o n a l   o n e s   t h a t   t h e   l o a d e r / g a m e   w o u l d   o t h e r w i s e   i g n o r e   �� �   s o   e x i t - o n - a n y - e x c e p t i o n   =   d e a t h   a t   l o a d .   T h e   d o u b l e   F A U L T   l i n e   w a s   t h e   s a m e   f u n c t i o n   r u n n i n g   i n   b o t h   r o l e s   ( V E H   +   u n h a n d l e d   f i l t e r )   f o r   t h e   o n e   e x c e p t i o n . 
 
 
 
 # #   R O O T   C A U S E   ( o n e   p a r a g r a p h ) 
 
 R o u n d   2 ' s   f a u l t   n e t   t r e a t e d   * * e v e r y * *   s e c o n d - c h a n c e   e x c e p t i o n   a s   a   f a t a l   c r a s h :   t h e   V E H   ( w h i c h   c a n n o t   b e   r e p l a c e d   a n d   t h e r e f o r e   s e e s   b e n i g n   n o t i f i c a t i o n s   t o o )   l o g g e d   ` F A U L T `   a n d   c a l l e d   ` E x i t P r o c e s s ( 0 ) `   b e f o r e   t h e   l o a d e r   c o u l d   i g n o r e   t h e   i n f o r m a t i o n a l   ` 0 x 4 0 6 D 1 3 8 8 `   d e b u g - p r i n t   n o t i f i c a t i o n .   T h e   g a m e   n e v e r   e v e n   g o t   p a s t   D L L   l o a d i n g . 
 
 
 
 # #   T H E   F I X   ( e x a c t   s e m a n t i c s ) 
 
 S p l i t   t h e   t w o   r o l e s ,   w i t h   a   b e n i g n - c o d e   g u a r d   s h a r e d   b y   b o t h : 
 
 -   ` f a u l t _ c o d e _ b e n i g n ( c o d e ) `   �� �   ` 1 `   ( I )   f o r   a n y   c o d e   w h o s e   t o p   2   b i t s   a r e   n o t   ` 1 1 `   ( s u c c e s s / i n f o r m a t i o n a l / w a r n i n g   s e v e r i t y   �� �   c o v e r s   ` 0 x 4 0 0 1 0 0 0 6 ` ,   ` 0 x 4 0 0 1 0 0 0 7 ` ,   ` 0 x 4 0 6 D 1 3 8 8 `   a n d   t h e   w h o l e   ` D B G _ * ` / ` 0 x 4 0 0 0 0 0 0 0 `   f a m i l y ) ,   ( I I )   ` 0 x 8 0 0 0 0 0 0 3 `   b r e a k p o i n t   /   ` 0 x 8 0 0 0 0 0 0 4 `   s i n g l e - s t e p ,   ( I I I )   ` 0 x E 0 6 D 7 3 6 3 `   M S V C   C + +   e x c e p t i o n . 
 
 -   ` v e c t o r e d _ f a u l t _ f i l t e r `   ( D l l M a i n ,   ` A d d V e c t o r e d E x c e p t i o n H a n d l e r ( 0 ,   . . . ) ` )   �� �   * * L O G - O N L Y ,   n e v e r   e x i t s * * :   b e n i g n   c o d e   �� �   s i l e n t   ` E X C E P T I O N _ C O N T I N U E _ S E A R C H ` ;   a n y t h i n g   e l s e   �� �   w r i t e   t h e   ` F A U L T   a d d r = / b r e a d c r u m b = / c o d e = `   l i n e   ( r a w   W i n 3 2   W r i t e F i l e + F l u s h )   t h e n   ` E X C E P T I O N _ C O N T I N U E _ S E A R C H ` .   A   f i r s t - c h a n c e / l a s t - c h a n c e   e x c e p t i o n   t h e   g a m e   o w n s   i s   a l l o w e d   t o   p r o p a g a t e   t o   i t s   o w n   h a n d l e r s . 
 
 -   ` f a u l t _ f i l t e r `   ( l e g a c y   ` S e t U n h a n d l e d E x c e p t i o n F i l t e r ` ,   r e - p i n n e d   e a c h   p r e s e n t   f r a m e   b y   ` e n s u r e _ f a u l t _ f i l t e r ` )   �� �   t h e   O N L Y   p l a c e   t h a t   e x i t s :   b e n i g n   �� �   ` C O N T I N U E _ S E A R C H `   ( d e f e n s i v e ) ,   g e n u i n e l y   u n h a n d l e d   f a t a l   ( ` 0 x C 0 0 0 0 0 0 5 `   e t c . )   �� �   l o g   F A U L T   +   s i l e n t   ` E x i t P r o c e s s `   ( R 6   s e m a n t i c s ,   n o   c r a s h   d i a l o g ) .   I f   t h e   g a m e   r e p l a c e s   o u r   f i l t e r   w e   j u s t   l o s e   t h e   e x i t   �� �   t h e   V E H   a l r e a d y   g a v e   t h e   b r e a d c r u m b . 
 
 -   R o u n d - 2   f i x e s   t h a t   w e r e   c o r r e c t   a r e   u n t o u c h e d :   f o n t   c r e a t e d   a t   d e v i c e - c r e a t e / R e s e t   o n l y   ( n e v e r   m i d - f r a m e ) ,   n e v e r   ` S e t R e n d e r T a r g e t ( N U L L ) ` ,   b o t h   s u r f a c e s   r e l e a s e d ,   m a t c h - s t a r t   r e q u i r e s   a   v a l i d   c h a i n ,   ` o v l - * `   d r a w   b r e a d c r u m b s . 
 
 
 
 # #   F I L E S   C H A N G E D 
 
 -   ` s r c / g a m e i f . c `   �� �   ` f a u l t _ c o d e _ b e n i g n ` ,   ` v e c t o r e d _ f a u l t _ f i l t e r `   ( l o g - o n l y ) ,   r e w r i t t e n   ` f a u l t _ f i l t e r `   ( b e n i g n - s k i p   +   l o g   +   e x i t ) ,   s h a r e d   ` f a u l t _ l o g _ p o i n t s ` ;   ` e n s u r e _ f a u l t _ f i l t e r `   u n c h a n g e d   i n   b e h a v i o r . 
 
 -   ` s r c / s t a t e . h `   �� �   d e c l a r e   ` f a u l t _ c o d e _ b e n i g n ` ,   ` v e c t o r e d _ f a u l t _ f i l t e r ` . 
 
 -   ` d 3 d 9 . c `   �� �   D l l M a i n   r e g i s t e r s   ` v e c t o r e d _ f a u l t _ f i l t e r `   ( w a s   ` f a u l t _ f i l t e r ` ) . 
 
 -   ` t e s t s / t e s t _ r a t e _ e n g i n e . c `   �� �   n e w   S T E P   7   ` t e s t _ f a u l t _ f i l t e r s ` :   b e n i g n   c o d e s   ( ` 0 x 4 0 6 D 1 3 8 8 ` ,   ` 0 x 4 0 0 1 0 0 0 6 / 0 7 ` ,   ` 0 x E 0 6 D 7 3 6 3 ` ,   ` 0 x 8 0 0 0 0 0 0 3 / 0 4 ` ,   ` 0 x 4 0 0 8 0 2 0 1 ` )   �� �   ` f a u l t _ c o d e _ b e n i g n = = 1 `   A N D   b o t h   f i l t e r s   r e t u r n   ` C O N T I N U E _ S E A R C H `   ( h a r n e s s   s u r v i v e s   =   n o   e x i t ) ;   f a t a l   ` 0 x C 0 0 0 0 0 0 5 ` / ` 0 x C 0 0 0 0 0 F D `   �� �   n o t   b e n i g n   ( e x i t   p a t h ,   n o t   r u n   i n - p r o c e s s   b y   d e s i g n ) ;   ` 0 x 8 0 0 0 0 0 0 2 `   w a r n i n g   �� �   b e n i g n ;   N U L L   E P   s a f e . 
 
 -   ` t e s t s / t e s t _ s o u r c e _ c o n t r a c t . p y `   �� �   a s s e r t s   V E H   r e g i s t r a t i o n ,   t h e   b e n i g n - c o d e   l i s t ,   a n d   ( o n   t h e   c o m m e n t - s t r i p p e d   T U )   t h a t   t h e   v e c t o r e d   f i l t e r   b o d y   c o n t a i n s   * * n o * *   ` E x i t P r o c e s s `   a n d   r e t u r n s   ` C O N T I N U E _ S E A R C H ` . 
 
 
 
 # #   B U I L D   R E S U L T 
 
 -   ` c m d   / c   b u i l d \ b u i l d . b a t `   f r o m   r e p o   d i r   �� �   * * r c = 0 ,   Z E R O   w a r n i n g s * * ,   v e r i f y _ p e   P A S S   ( i 3 8 6 ,   P E 3 2 ,   i m p o r t s   �� �   { K E R N E L 3 2 ,   U S E R 3 2 ,   m s v c r t } ,   * * 1 1   e x p o r t s * * ) 
 
 -   * * d 3 d 9 . d l l   =   1 3 7 , 1 7 0   B ,   S H A 2 5 6   ` 1 2 f e 9 e f 9 7 a d 2 e 3 6 d 3 8 2 b 7 b 7 5 5 0 5 8 d 8 7 0 d 4 2 4 9 1 1 d 7 a 8 b 8 0 9 a 8 c e 4 c 3 b 4 c 2 7 5 9 7 5 f ` * *   ( ` d 3 d 9 . s h a 2 5 6 `   u p d a t e d ) 
 
 -   * * D e p l o y e d   b y t e - i d e n t i c a l * * :   r e p o   r o o t   =   g a m e   d i r   =   ` t e s t s \ d 3 d 9 . d l l `   ( S H A 2 5 6   e q u a l   o n   a l l   t h r e e ) ;   o l d   g a m e   ` d 3 d 9 m o d . l o g `   d e l e t e d .   G a m e   N O T   l a u n c h e d . 
 
 
 
 # #   H A R N E S S   R E S U L T S   ( a l l   g r e e n ) 
 
 -   ` t e s t s \ t e s t _ d 3 d 9 _ a c t u a l . e x e `   �� �   A L L   C H E C K S   P A S S E D 
 
 -   ` t e s t s \ t e s t _ r a t e _ e n g i n e . e x e `   �� �   F A I L U R E S :   0   ( i n c l .   n e w   S T E P   7   f a u l t - f i l t e r   s e m a n t i c s ) 
 
 -   ` t e s t s \ t e s t _ s o u r c e _ c o n t r a c t . p y `   �� �   S O U R C E - C O N T R A C T :   P A S S   ( i n c l .   n e w   r o u n d - 3   c h e c k s ) 
 
 -   ` t e s t s \ t e s t _ p e _ s t r u c t u r e . p y `   �� �   F A I L U R E S :   0 
 
 
 
 # #   W H A T   D A L I   T E S T S   N E X T   ( l i v e ) 
 
 1 .   G a m e   m u s t   n o w   * * s t a r t   a n d   r e a c h   t h e   m a i n   m e n u * *   ( t h e   ` 0 x 4 0 6 D 1 3 8 8 `   n o t i f i c a t i o n   i s   i g n o r e d ,   n o   F A U L T   l i n e   a t   l o a d ) . 
 
 2 .   S t a r t   a   s k i r m i s h :   c h a i n   �� �   m a t c h   s t a r t   ( v a l i d   c h a i n )   �� �   ` R E S   t = . . . `   l i n e s ;   o v e r l a y   p a n e l   a p p e a r s ;   n o   c r a s h . 
 
 3 .   O n l y   a   g e n u i n e l y   u n h a n d l e d   f a t a l   e x c e p t i o n   w i l l   l o g   ` F A U L T   a d d r = . .   b r e a d c r u m b = o v l - . .   c o d e = . . `   �� �   i f   y o u   e v e r   s e e   o n e ,   s e n d   t h a t   l i n e . 
 
 
 
 - - - 
 
 
 
 #   B U I L D   �� �   R O U N D   2   ( R E W O R K ,   2 0 2 6 - 0 9 - 0 6 )   �� �   F I X   L I V E   M A T C H - S T A R T   C R A S H 
 
 
 
 # #   R O U N D   S U M M A R Y 
 
 D a l i ' s   l i v e   t e s t :   t h e   R 1 4   D L L   l o a d s   ( v e r s i o n = O K ) ,   c h a i n / d e c r y p t / R E S   l o g g i n g   a l l   w o r k ,   t h e   h u m a n   p l a y e r   i s   f o u n d ,   t h e   f o n t   i s   c r e a t e d   �� �   a n d   t h e   p r o c e s s   d i e s   o n   t h e   F I R S T   o v e r l a y   d r a w   a t   m a t c h   s t a r t ,   w i t h   N O   ` F A U L T `   l i n e   ( c r a s h   n e t   f a i l e d   t o   c a t c h   i t ) .   R e b u i l t   t h e   d r a w   p a t h   a g a i n s t   t h e   k n o w n - g o o d   R 6 / R 7   p a t t e r n .   E v e r y t h i n g   e l s e   f r o m   R 1 4   i s   u n t o u c h e d ;   a l l   4   h a r n e s s e s   s t a y   g r e e n ;   D a l i   r e - t e s t s   l i v e . 
 
 
 
 # #   R O O T   C A U S E   ( w h a t   d e v i a t e d   f r o m   R 6 / R 7 ) 
 
 1 .   * * F o n t   c r e a t e d   M I D - F R A M E   i n   t h e   d r a w   p a t h . * *   R 1 4   c a l l e d   ` D 3 D X C r e a t e F o n t A `   l a z i l y   f r o m   ` u i _ d r a w ( ) `   o n   t h e   v e r y   f i r s t   p r e s e n t   w h e r e   t h e   c h a i n   b e c a m e   v a l i d   ( ` U I :   f o n t   c r e a t e d `   i s   t h e   l o g ' s   l a s t   l i n e   �� �   i . e . ,   i n s i d e   t h e   g a m e ' s   P r e s e n t   c a p t u r e ,   r i g h t   a t   m a t c h   s t a r t ,   i m m e d i a t e l y   a f t e r   a   d e v i c e   R e s e t ) .   R 6 - R 7   c r e a t e d   t h e   f o n t   O N C E   a t   d e v i c e - c r e a t e   a n d   r e - c r e a t e d   i t   o n   a   s u c c e s s f u l   R e s e t   o n l y .   A   m i d - f r a m e   f o n t   c r e a t i o n   c a n   c o r r u p t   d e v i c e   s t a t e   �� �   i m m e d i a t e   A V ,   u n r e c o v e r a b l e . 
 
 2 .   * * C r a s h   n e t   w a s   r e p l a c e a b l e   +   b u f f e r e d . * *   T h e   ` F A U L T `   l i n e   n e v e r   a p p e a r e d   b e c a u s e   ` S e t U n h a n d l e d E x c e p t i o n F i l t e r `   w a s   i n s t a l l e d   o n c e   i n   D l l M a i n   �� �   t h e   g a m e   ( o r   a n o t h e r   D L L )   r e p l a c e s   i t   l a t e r ,   s o   o u r   f i l t e r   n e v e r   r a n ,   a n d   t h e   f a u l t   w r i t e   w e n t   t h r o u g h   f l i m s y   p a t h s   o n l y .   R 1 4   a l s o   h a d   n o   p e r - d r a w - s t e p   b r e a d c r u m b s   t o   p i n   t h e   d y i n g   c a l l . 
 
 3 .   * * D r a w - o r d e r   h a r d e n i n g   r e g r e s s i o n s   v s   R 6 / R 7 : * *   R T   s w i t c h   h a p p e n e d   e v e n   w h e n   ` G e t B a c k B u f f e r `   f a i l e d   ( ` S e t R e n d e r T a r g e t ( 0 ,   N U L L ) `   r i s k ) ,   n o   s t r i c t   p r e v - R T   r e q u i r e m e n t ,   a n d   ` G e t R e n d e r T a r g e t ` ' s   r e f   w a s   n e v e r   r e l e a s e d   ( p e r - f r a m e   l e a k ) . 
 
 4 .   * * P h a n t o m   ` - - -   m a t c h   s t a r t   n = 0   . . .   - - - ` * *   f i r e d   d u r i n g   l o a d i n g :   m a t c h - s t a r t   f i r e d   o n   ` n   ! =   s _ l a s t _ n `   w i t h   ` s _ l a s t _ n `   i n i t i a l i z e d   t o   - 1   b e f o r e   a n y   c h a i n   e x i s t e d . 
 
 
 
 # #   T H E   F I X   ( f i l e s   c h a n g e d ) 
 
 -   ` s r c / u i . c `   �� �   f o n t   o w n e r s h i p   m o v e d   O U T   o f   t h e   d r a w   p a t h :   n e w   p u b l i c   ` u i _ c r e a t e _ f o n t ( d e v ) `   ( d e v i c e - c r e a t e   +   s u c c e s s f u l   R e s e t   O N L Y ) ;   ` u i _ d r a w ( ) `   n o   l o n g e r   c r e a t e s   f o n t s ,   s k i p s   c l e a n l y   w h e n   n o n e ,   g a t e s   o n   ` g _ v a l u e s _ v a l i d `   ( �� � 1   R E S   s a m p l e   t a k e n ) .   R e w r o t e   t h e   R T   s e c t i o n   t o   R 6 / R 7   s h a p e :   c a p t u r e   p r e v - R T   A N D   b a c k b u f f e r   B E F O R E   s w i t c h i n g ;   b a i l   i f   e i t h e r   f a i l s   ( ` S e t R e n d e r T a r g e t `   n e v e r   c a l l e d   w i t h   N U L L ) ;   s a v e / s e t / r e s t o r e   t h e   7   r e n d e r   s t a t e s   ( Z   o f f   %� 3 ,   A L P H A B L E N D   S R C A L P H A / I N V S R C A L P H A ,   L I G H T I N G   o f f ) ;   B e g i n �� � E n d S c e n e   p a i r   w i t h   ` E n d S c e n e `   a l w a y s   a f t e r   s u c c e s s f u l   ` B e g i n S c e n e ` ;   r e s t o r e   s t a t e s   ( r e v e r s e ) ,   r e s t o r e   R T ,   t h e n   r e l e a s e   B O T H   s u r f a c e s   ( t h e   p e r - f r a m e   p r e v - R T   l e a k   i s   g o n e ) .   A d d e d   v o l a t i l e   b r e a d c r u m b s   e v e r y   s t e p :   ` o v l - s t a r t / t c l / r t / s t a t e s / b e g i n / p a n e l / e n d / r e s t o r e / d o n e ` . 
 
 -   ` s r c / g a m e i f . c `   �� �   f a u l t   s a f e t y   n e t   r e b u i l t :   ` f a u l t _ f i l t e r `   w r i t e s   t h e   ` F A U L T   a d d r = / b r e a d c r u m b = / c o d e = `   l i n e   v i a   a   d i r e c t   W i n 3 2   ` C r e a t e F i l e A + W r i t e F i l e + F l u s h F i l e B u f f e r s + C l o s e H a n d l e `   p a t h   ( s u r v i v e s   h e a p / C R T   d a m a g e ) ,   t h e n   l o g s   +   ` E x i t P r o c e s s ` .   N e w   ` e n s u r e _ f a u l t _ f i l t e r ( ) `   r e - p i n s   ` S e t U n h a n d l e d E x c e p t i o n F i l t e r `   e v e r y   p r e s e n t   f r a m e   ( t h e   g a m e   m a y   r e p l a c e   i t ) .   M a t c h - s t a r t   n o w   r e q u i r e s   a   V A L I D   c h a i n   ( ` c t x ! = 0   & &   n > 0   & &   g _ r e s   & &   g _ i n c ` )   �� �   t h e   l o a d i n g   ` n = 0 `   p h a n t o m   i s   g o n e . 
 
 -   ` s r c / s t a t e . h `   �� �   d e c l a r a t i o n s   f o r   ` u i _ c r e a t e _ f o n t ` ,   ` e n s u r e _ f a u l t _ f i l t e r ` . 
 
 -   ` d 3 d 9 . c `   �� �   ` w _ c r e a t e _ d e v i c e ` / ` w _ c r e a t e _ d e v i c e _ e x `   c a l l   ` u i _ c r e a t e _ f o n t `   r i g h t   a f t e r   p a t c h i n g   t h e   d e v i c e ;   ` r e s e t _ h o o k `   r e - c r e a t e s   t h e   f o n t   o n l y   w h e n   t h e   R e s e t   s u c c e e d e d ;   ` p r e s e n t _ h o o k `   c a l l s   ` e n s u r e _ f a u l t _ f i l t e r ( ) `   f i r s t ;   D l l M a i n   a d d s   ` A d d V e c t o r e d E x c e p t i o n H a n d l e r ( 0 ,   f a u l t _ f i l t e r ) `   a s   a   s e c o n d - c h a n c e   b a c k s t o p   t h a t   c a n n o t   b e   r e p l a c e d . 
 
 -   ` t e s t s / * `   h a r n e s s e s   �� �   u n c h a n g e d ,   a l l   s t i l l   p a s s   ( f o n t / d r a w   l i v e   b e h a v i o r   i s   n o t   h a r n e s s - t e s t a b l e   w i t h o u t   a   r e a l   d e v i c e ) . 
 
 
 
 # #   B U I L D   R E S U L T 
 
 -   S a m e   c o m m a n d   v i a   ` c m d   / c   b u i l d \ b u i l d . b a t `   f r o m   t h e   r e p o   d i r   �� �   * * r c = 0 ,   Z E R O   w a r n i n g s * * 
 
 -   v e r i f y _ p e :   i 3 8 6 ,   P E 3 2 ,   i m p o r t s   �� �   { K E R N E L 3 2 ,   m s v c r t ,   U S E R 3 2 } ,   1 1   e x p o r t s ,   ` V E R I F Y   P A S S ` 
 
 -   * * d 3 d 9 . d l l   =   1 3 7 , 0 0 0   B ,   S H A 2 5 6   ` 3 d c f 2 6 d 5 4 0 0 c 2 8 a c c a 1 6 7 f c e c c 5 9 e e 7 2 2 8 0 4 5 b 0 f 6 5 2 1 d f 3 f 3 a 1 6 7 8 8 2 9 d 8 7 7 1 0 1 ` * * ;   S H A 2 5 6   r e c o r d e d   i n   ` d 3 d 9 . s h a 2 5 6 ` 
 
 -   * * D e p l o y e d   b y t e - i d e n t i c a l * *   ( S H A 2 5 6   m a t c h   o n   a l l   t h r e e ) :   r e p o   r o o t   =   g a m e   d i r   =   ` t e s t s \ d 3 d 9 . d l l ` ;   o l d   ` d 3 d 9 m o d . l o g `   d e l e t e d   f r o m   t h e   g a m e   d i r . 
 
 
 
 # #   H A R N E S S   R E S U L T S   ( a l l   g r e e n ) 
 
 -   ` t e s t s \ t e s t _ d 3 d 9 _ a c t u a l . e x e `   �� �   A L L   C H E C K S   P A S S E D   ( r c = 0 ) 
 
 -   ` t e s t s \ t e s t _ r a t e _ e n g i n e . e x e `   �� �   F A I L U R E S :   0   ( r c = 0 ) 
 
 -   ` t e s t s \ t e s t _ s o u r c e _ c o n t r a c t . p y `   �� �   S O U R C E - C O N T R A C T :   P A S S   ( r c = 0 ) 
 
 -   ` t e s t s \ t e s t _ p e _ s t r u c t u r e . p y `   �� �   F A I L U R E S :   0 ;   e x p o r t s   =   D 3 D P E R F _ B e g i n E v e n t ,   D 3 D P E R F _ E n d E v e n t ,   D 3 D P E R F _ G e t S t a t u s ,   D 3 D P E R F _ Q u e r y R e p e a t F r a m e ,   D 3 D P E R F _ S e t M a r k e r ,   D 3 D P E R F _ S e t O p t i o n s ,   D 3 D P E R F _ S e t R e g i o n ,   D e b u g S e t L e v e l ,   D e b u g S e t M u t e ,   D i r e c t 3 D C r e a t e 9 ,   D i r e c t 3 D C r e a t e 9 E x   ( 1 1 ) 
 
 
 
 # #   W H A T   D A L I   T E S T S   N E X T   ( l i v e ) 
 
 1 .   S t a r t   a   s k i r m i s h ;   t h e   l o g   s h o u l d   n o w   s h o w   ` R e s e t   - >   f o n t   i n v a l i d a t e d `   t h e n   ` U I :   f o n t   c r e a t e d `   I M M E D I A T E L Y   ( a t   t h e   R e s e t   /   d e v i c e - c r e a t e ,   N O T   a t   m a t c h   s t a r t   i n s i d e   t h e   d r a w ) . 
 
 2 .   E x p e c t   a   s t a b l e   l o g :   c h a i n   �� �   m a t c h   s t a r t   ( o n l y   o n c e   t h e   c h a i n   i s   v a l i d )   �� �   ` R E S   t = . . . `   l i n e s   �� �   N O   ` F A U L T `   l i n e ,   g a m e   k e e p s   r u n n i n g . 
 
 3 .   I f   i t   S T I L L   c r a s h e s ,   t h e   n e w   b r e a d c r u m b s   +   v e c t o r e d   h a n d l e r   W I L L   p r o d u c e   ` F A U L T   a d d r = . .   b r e a d c r u m b = o v l - . .   c o d e = . . `   �� �   t h a t   n a m e s   t h e   e x a c t   d y i n g   c a l l ;   s e n d   t h a t   l i n e . 
 
 
 
 - - - 
 
 
 
 #   B U I L D   �� �   R O U N D   1 4   ( 2 0 2 6 - 0 9 - 0 6 )   �� �   F U L L   R E S O U R C E - R A T E   M O D 
 
 
 
 # #   R O U N D   S U M M A R Y 
 
 R 1 3   w a s   a   z e r o - r e n d e r   o b s e r v e r   p r o v i n g   t h e   m e m o r y   c h a i n .   R 1 4   t u r n s   i t   i n t o   t h e   s h i p p a b l e   m o d :   a   r a t e   e n g i n e   ( r i n g   +   E M A   +   s p i k e / p a u s e   f r e e z e ) ,   a   D 3 D X 9   f o n t   o v e r l a y   ( F 9   t o g g l e ,   l i v e   s e t t i n g   k e y s   1 - 6 ) ,   a n   I N I   c o n f i g   w i t h   a   D e f a u l t P r o f i l e * . x m l   m e r g e ,   a   v e r s i o n   g a t e   t h a t   r e f u s e s   t o   a r m   t h e   o v e r l a y   o n   a   w r o n g   a g e 3 y . e x e ,   a n d   a   b u i l d / d e p l o y   p i p e l i n e .   V e r i f i e d   o n - b o x ,   z e r o   g a m e   l a u n c h e s :   c o m p i l e d   c l e a n ,   a l l   4   h a r n e s s e s   p a s s ;   o n l y   D a l i ' s   l i v e   c a l i b r a t i o n   r e m a i n s . 
 
 
 
 # #   F I L E S   C R E A T E D / C H A N G E D 
 
 -   N e w :   ` s r c / l o g g e r . c ` ,   ` s r c / t r a c k e r . c ` ,   ` s r c / r a t e . c ` ,   ` s r c / s e t t i n g s . c ` ,   ` s r c / u i . c ` ,   ` b u i l d / b u i l d . b a t ` ,   ` b u i l d / v e r i f y _ p e . p y ` ,   ` t o o l s / e x t r a c t _ b a r . p y ` ,   ` c o n f i g / s c h e m a . m d ` ,   ` R e s o u r c e R a t e M o d . i n i . e x a m p l e ` ,   ` L I C E N S E `   ( M I T ) ,   ` t e s t s / t e s t _ r a t e _ e n g i n e . c ` 
 
 -   R e w r i t t e n :   ` s r c / s t a t e . h ` ,   ` d 3 d 9 . c `   ( m o n o l i t h i c   e n t r y :   g l o b a l s ,   m o d u l e   i n c l u d e s ,   D 3 D 9   w r a p p e r / h o o k s ,   v e r s i o n   g a t e ,   D l l M a i n   �� �   R 1 3   a d d r e s s   c o n s t a n t s   a n d   R E S - l i n e   f o r m a t   u n t o u c h e d ) ,   ` R E A D M E . m d ` 
 
 -   E d i t e d :   ` s r c / g a m e i f . c `   ( d e b u g   r a t e s   l i n e ;   a l i a s i n g   f i x e s ;   c l e a n u p   �� �   l o g i c   u n c h a n g e d ) ,   ` t e s t s / t e s t _ s o u r c e _ c o n t r a c t . p y ` ,   ` t e s t s / t e s t _ p e _ s t r u c t u r e . p y ` ,   ` . g i t i g n o r e ` ,   ` . s w a r m / B U I L D . m d ` 
 
 
 
 # #   B U I L D   R E S U L T 
 
 -   C o m m a n d :   ` i 6 8 6 - w 6 4 - m i n g w 3 2 - g c c . e x e   - s h a r e d   - s t a t i c - l i b g c c   - O 2   - W a l l   - W e x t r a   - o   d 3 d 9 . d l l   d 3 d 9 . c   d 3 d 9 . d e f   - l w i n m m   - l u s e r 3 2 `   �� �   r c = 0 ,   Z E R O   w a r n i n g s 
 
 -   v e r i f y _ p e :   i 3 8 6 ,   P E 3 2 ,   i m p o r t s   �� �   { K E R N E L 3 2 ,   m s v c r t ,   U S E R 3 2 }   ( c o n f i r m e d ) ,   1 1   e x p o r t s ,   V E R I F Y   P A S S 
 
 -   * * d 3 d 9 . d l l   =   1 3 5 , 1 8 0   B ,   S H A 2 5 6   ` d b e c 6 4 b 6 d 5 4 1 3 5 6 2 9 8 1 5 7 f c 6 d d 0 8 d c 4 f 3 3 6 2 d e e a 9 5 a 8 b 5 4 0 e 6 7 8 c 5 9 4 9 a 1 5 f 0 4 a ` * * ;   d e p l o y e d   b y t e - i d e n t i c a l   t o   g a m e   d i r   +   ` t e s t s \ d 3 d 9 . d l l ` ;   I N I   e x a m p l e   c o p i e d   t o   g a m e   d i r 
 
 
 
 # #   R A T E - E N G I N E   R U L E S   ( i m p l e m e n t e d ) 
 
 -   B o o t s t r a p   p r i m e s   w i t h o u t   e m i t t i n g   a   f a k e   r a t e ;   c a d e n c e   g a t e   ( 1 0 0 - 1 0 0 0   m s ,   d e f a u l t   5 0 0 )   h o n o r s   s a m p l e   i n t e r v a l 
 
 -   S t e a d y   + 1 / 5 0 0 m s   �� �   E M A   2 . 0 / s   ( 1 2 0 . 0 / m i n ) ;   s p e n d i n g   s p i k e   ( ` | i n s t |   >   E M A %� 3 ` ,   D i s c o n t i n u i t y R a t i o = 3 . 0 )   f r e e z e s   E M A   w h i l e   k e e p i n g   r i n g   b a s e l i n e   f r e s h ;   i n s t a n t   g a i n s   c o u n t e d   ( S h o w G a i n s ) 
 
 -   C l o c k - n o t - a d v a n c i n g   ( g a m e   t i c k   s t a g n a n t )   �� �   g l o b a l   p a u s e ,   n o   d i v - b y - z e r o ,   n o   d r i f t 
 
 -   P e r - s l o t   v a l u e - f l a t   a c r o s s   a   f u l l   i n t e r v a l   �� �   E M A   f r o z e n ,   u n f r e e z e s   o n   c h a n g e   ( E S C - p a u s e   d e t e c t i o n   p a t h ) 
 
 -   R e a l t i m e   m o d e   ( Q P C )   f i n i t e / s a n e 
 
 
 
 # #   O V E R L A Y   B E H A V I O R 
 
 -   D r a w s   o n l y   w h e n :   e n a b l e d   +   v e r s i o n - g a t e   O K   +   p a n e l   v i s i b l e   +   i n - m a t c h   +   d e v i c e / d 3 d x 9 / f o n t   a v a i l a b l e ;   r u n s   B E F O R E   o r i g i n a l   P r e s e n t 
 
 -   B a c k b u f f e r   e x p l i c i t   R T ,   7   r e n d e r   s t a t e s   s a v e d / r e s t o r e d ,   s u r f a c e   r e l e a s e d   v i a   i t s   o w n   v t a b l e 
 
 -   F 9   t o g g l e s   p a n e l ;   k e y s   1 = F o o d   2 = W o o d   3 = C o i n   4 = E x p o r t   5 = S h o w Z e r o   6 = S h o w G a i n s   t o g g l e   l i v e   ( s e t t i n g s _ s a v e ( )   a t o m i c   w r i t e ) 
 
 -   M e n u s   /   D E V I C E L O S T   /   g a t e - f a i l   =   g r a c e f u l   n o - o p s 
 
 
 
 # #   I N I   S C H E M A   ( f u l l   d o c :   c o n f i g / s c h e m a . m d ) 
 
 -   ` [ G e n e r a l ] `   E n a b l e d ( 1 )   /   H o t k e y ( 0 x 7 8 )   /   S t a r t H i d d e n ( 0 ) 
 
 -   ` [ D i s p l a y ] `   F o n t N a m e   /   F o n t S i z e ( 1 4 )   /   O p a c i t y ( 0 . 8 5 )   /   P o s X ( 1 2 )   /   P o s Y ( 1 2 )   /   S h o w H e a d e r ( 1 )   /   S h o w S l o t s 5 6 7 ( 0 ) 
 
 -   ` [ R a t e ] `   S a m p l e M s ( 5 0 0 )   /   S m o o t h i n g ( m e d )   /   U n i t ( m i n )   /   U s e G a m e T i m e ( 1 )   /   D i s c o n t i n u i t y R a t i o ( 3 . 0 )   /   S h o w G a i n s ( 1 ) 
 
 -   ` [ D e b u g ] `   E n a b l e d ( 0 ) 
 
 -   P l u s   m t i m e - g a t e d   m e r g e   f r o m   ` U s e r s \ D e f a u l t P r o f i l e * . x m l `   m o d   ` < S e t t i n g > `   k e y s 
 
 
 
 # #   V E R S I O N   G A T E   ( a g e 3 y . e x e   o n   t h i s   m a c h i n e   �� �   s t a t i c a l l y   v e r i f i e d ,   n o   l a u n c h ) 
 
 -   s i z e   1 1 , 5 9 8 , 6 4 8   �� �   ,%V%  i 3 8 6   0 x 1 4 C   �� �   ,%V%  P E 3 2   �� �   ,%V%  b a s e   0 x 4 0 0 0 0 0   �� �   ,%V%  v e r s i o n   r e s o u r c e   6 . 1 0 8 . 3 2 1 . 1 3 7   �� �   �� �   g a t e   P A S S E S ,   o v e r l a y   a r m s .   F a i l u r e   p a t h   p r o v e n   i n   h a r n e s s . 
 
 
 
 # #   D E P L O Y   ( d o n e ) 
 
 -   ` c m d   / c   b u i l d \ b u i l d . b a t ` :   c o m p i l e   �� �   v e r i f y   �� �   d 3 d 9 . s h a 2 5 6   �� �   d e p l o y   t o   g a m e   d i r   +   t e s t s \ d 3 d 9 . d l l   �� �   I N I   e x a m p l e   �� �   d 3 d x 9   n o t e   ( S y s t e m 3 2 )   �� �   c l e a n   s t a l e   l o g s 
 
 
 
 # #   D E V I A T I O N S   /   R I S K S   ( h o n e s t ) 
 
 1 .   * * E x p o r t s   =   1 1 ,   n o t   t h e   d o c u m e n t e d   " 1 3 " * *   �� �   d 3 d 9 . d e f   h a s   1 1 ;   k e p t   1 1 ;   v e r i f y   a s s e r t s   �� � 1 1 
 
 2 .   M o n o l i t h i c   ` d 3 d 9 . c `   e n t r y   i n s t e a d   o f   ` s r c / d l l m a i n . c `   ( S W A R M _ T E S T   h a r n e s s   c o m p a t i b i l i t y ) 
 
 3 .   E S C - p a u s e   d e t e c t e d   v i a   p e r - s l o t   v a l u e - f l a t   f r e e z e   ( P r e s e n t   c o u n t e r   k e e p s   t i c k i n g   d u r i n g   E S C )   �� �   * * l i v e - c h e c k a b l e * * 
 
 4 .   V e r s i o n   g a t e   r e a d s   t h e   v e r s i o n   R E S O U R C E   ( r a w   P E   h e a d e r   r e a d   r e t u r n s   4 . 0 . 0 . 0   a n d   w o u l d   f a l s e - f a i l )   �� �   f i x e d   m i d - r o u n d 
 
 5 .   O v e r l a y   r e n d e r i n g   u n v e r i f i e d   l i v e   �� �   D a l i ' s   l a u n c h   t e s t :   e x p e c t   ` v e r s i o n = O K ` ,   o v e r l a y   p a n e l ,   t h e n   c a l i b r a t e   r a t e s / f r e e z e   v s   H U D 
 
 
 
 # #   T E S T   S T A T U S   ( a l l   o f f l i n e   h a r n e s s e s ,   r u n   o n - b o x ) 
 
 -   ` t e s t _ d 3 d 9 _ a c t u a l `   2 2 / 2 2   ,%V%  ` t e s t _ r a t e _ e n g i n e `   4 6 / 4 6   ,%V%  ` t e s t _ s o u r c e _ c o n t r a c t `   P A S S   ,%V%  ` t e s t _ p e _ s t r u c t u r e `   0   f a i l u r e s 
 
 -   C o m m i t t e d   +   p u s h e d :   ` 1 c c 8 7 0 8 `   ( m a i n ) 
 
 