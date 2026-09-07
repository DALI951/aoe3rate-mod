# AoE3 Legacy UI Modding — Community Research (2026-09-07)
Research question from Dali: how does the modding community change the AoE3 UI, and does the "live resource-gathering-rate HUD" mod we are building already exist?

## 1. How the community mods the UI (the real way)
The community does NOT inject DLLs to change the UI. The legacy UI is **XML/XMB gadgets** inside the game's `.bar` archives, and UI mods are file swaps:
- The in-game UI (main HUD, minimap panel, objective screens, etc.) is defined by `ui*.xml.xmb` files living inside the game's `data*.bar` archives (e.g. `Data.bar`, `Data2.bar`, `Data3.bar`; art in `Art*.bar`).
- Canonical files modders touch: `uimainnew.xml.xmb` (the main in-game HUD), `uiminimappanelnew.xml`, `uiobjectives.xml.xmb`, menu files — replacing the maximised UI in your install.
- **Workflow (per HeavenGames tutorial "Modding – Basis"):**
  1. Extract `ui*.xml.xmb` from the `.bar` with **AoE3Ed** (Ykkrosh's tool: Archive Viewer + File Converter — converts `.bar`→files, `.xmb`↔`.xml`, `.ddt`↔`.tga`). I2D=**TempiresG/Resource-Manager** (GitHub) — modern replacement, also opens DE bars.
  2. Edit the XML (layout, positions, sizes, re-using gadget ids; any text editor).
  3. Convert back to `.xmb`, drop the file into the game's `data/` folder (loose files override the `.bar` archives).
- **Install = copy XML/XMB into `<game>/data/`**; uninstall = delete the file. ESO-compatible (works on the official multiplayer lobby, no CD-key/checksum issues for UI-only changes).

## 2. Known UI mods (all file-swaps, all ESO-safe)
- **Ekanta TAD UI** (aoe3.heavengames.com showfile 1656) — transparent UI via maximised-mode; hidden buttons (pop/popcap, queue-click toggles); installer; ESO-compatible.
- **QazUI** (Qazitory) — larger pop/shipment numbers, market buy/sell shortcuts, deck menu move, transparent variants, TAD + TWC versions.
- **Xentelian's UI** — another popular fork base.
- **Jams UI Mod** (jammainen) — combined Ekanta/Xentelian + civ-specific toggles; use minimised UI.
- **Minimalistic custom AOE3 UI** (neuron, ESOCommunity) — removes clutter, moves the production queue; resolution-fixed, data/ folder install.
- **Aizamk TAD Observer UI** — IMPORTANT: uses XML gadgets to display **dynamic live game data** (per-player resource totals, "Improvement Total Cost", "Units Lost Cost", death counter, population) — but only for observer mode and only with custom maps that expose the data; requires `uimainnew.xml` in `data/` + custom RM3 maps.

## 3. Does the exact mod Dali wants already exist? — NO
- The **resource gather-rate display** is a known, **unfulfilled** feature request: `forums.ageofempires.com/t/show-the-resource-gather-rate-per-second/120838` (2021, DE forum) — players asked for res/min + res/sec toggles; nothing was shipped by the dev toggles, and no community mod fulfils it for DE or legacy.
- The **ESOC wiki "Gathering Rates" page** documents *static* theory rates (res/sec per eco source per unit type: vill 0.67 berries, 0.5 farm, 0.84 hunt, 0.6 mine, etc.) — this is exactly the data shape our rate engine computes, but no one has shipped a LIVE in-game HUD showing actual per-game rates.
- **Conclusion: the live resource-rate HUD is genuinely novel for legacy TAD** — the community's XML approach can show static elements and some dynamic totals (observer maps), but has **no scripting/binding to compute time-derivatives** of your own stock in real time. Our DLL computes what the XML layer inherently cannot.

## 4. Engine-level community changes: exe patching, not proxying
- The one "engine modding" project is **AoE3 UnHardcode Patch** (GitHub danielpereira/AoE3UnHardcoded; v1.10, by kangcliff + danielpereira) — **patches `age3y.exe` in place** (drop your exe on the patcher) to remove hardcoded limits (AI/civ limit, Fame resource UI, etc.) and installs `UHC.dll` with a plugin system in `Startup\uhc.cfg`.
- Meaning: the community's accepted engine-level technique = direct exe binary patching with per-version offsets, NOT a d3d9 proxy DLL. Two consequences for us:
  1. Our `d3d9.dll` proxy + verified-memory-chain approach is unusual for this game (no precedent found) — which is why it's hard, but also why it's new.
  2. If the Present-path hunt stalls, the community-proven fallback is an **exe patch** that hooks the same stock reads inside age3y.exe (same verified addresses we already hold: `res=+0x230`, keys table, etc.) — or the XML route for a static HUD.

## 5. Multiplayer fairness norm (validates our design)
Neuron's minimalist UI explicitly blocked adding market/training buttons as an "unfair advantage" — the community treats read-only presentational HUD changes as fair. Our overlay is client-side, read-only, no inputs injected (F9 toggles the panel only, no game commands) — consistent with community norms.

## 6. Strategic recommendation (for Dali)
- **Keep the DLL path** — it is the only way to show *live rates*: XML UI cannot compute them; exe-patch (UnHardcode style) would need re-discovering offsets per version and risks breaking the file; we already own verified addresses + a working rate engine + a once-drawing overlay.
- **The remaining bug is purely "where does the in-match Present live"** — round-17 tracer (`present-path dev=/sw=/scene=`) exists to answer exactly that with one more log. Community research did not find a shortcut around it.
- **Fallback A (quick visible win):** ship a *static/observer-style* XML HUD mod too (uimainnew.xml with res totals, data/-folder install, uninstall=delete) — gives the community-style thing while the DLL render path gets finished. Needs AoE3Ed to extract the base uimainnew.xml.xmb from Data3.bar (we never touch game files on our side — Dali would run extraction locally or we document the steps).
- **Fallback B:** exe-patch route per section 4 — higher risk, breaks on other versions (our version gate philosophy argues against it).

## TODO / references
- HeavenGames modding-tools page: `aoe3.heavengames.com/downloads/showcategory.php/category/526`
- AoE3Ed (Ykkrosh's archive viewer + file converter): `aoe3.heavengames.com/community/showthread.php?t=417085`
- TempiresG/Resource-Manager (modern .bar/.xmb replacement): `github.com/TempiresG/Resource-Manager`
- QazUI (Qazitory): `aoe3.heavengames.com/cgi-bin/forums/display.cgi?action=ct&f=9,16932,0,all`
- Ekanta TAD UI (showfile 1656): `aoe3.heavengames.com/showfile.html?f=1656`
- ESOCommunity minimalist UI (neuron): `eso-community.net/viewtopic.php?t=10902`
- Aizamk TAD Observer UI: `aoe3.heavengames.com/showfile.html?f=1665`
- AoE3 UnHardcode Patch GitHub: `github.com/danielpereira/AoE3UnHardcoded`
- ESOC Gathering Rates wiki: `eso-community.net/wiki/Gathering_Rates`
- Forum feature request (res gather rate per second): `forums.ageofempires.com/t/show-the-resource-gather-rate-per-second/120838`