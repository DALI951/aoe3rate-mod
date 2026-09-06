# PLAN: Rebuild AoE3 TAD resource-rate HUD mod (d3d9 proxy DLL) — full Ghidra decompile first

## Environment facts (VERIFIED)

| Item | Status |
|---|---|
| `C:\Users\Dali\Documents\gaames\Age of Empires III - Complete Collection\age3y.exe` | EXISTS, **32-bit i386** (machine=0x14C) |
| `redist\DirectX\Apr2005_d3dx9_25_x86.cab` | EXISTS (no `d3dx9_25.dll` in game dir yet) |
| `expand.exe` | EXISTS (`C:\Windows\system32\expand.exe`) |
| Ghidra 12.1.3 | `C:\Users\Dali\Documents\gaames\ghidra_12.1.3_PUBLIC\` with `support\analyzeHeadless.bat`, `Ghidra\Features\Decompiler\lib\Decompiler.jar` |
| Python | **FOUND: `C:\Python314\python.exe` (3.14.6) + `C:\Windows\py.exe`** |
| JDK 25 | `C:\Users\Dali\Tools\jdk-25.0.4+7\bin\java.exe` (Temurin 25.0.4 LTS) — verify launch works, fallback to JDK 17 on PATH if not |
| x86 GCC | NONE installed → download `w64devkit-x86` (GitHub release asset, self-extracting 7z, needs `7zr.exe` for scripted extract) |

**Chosen decompile route:** Java post-script via `analyzeHeadless -postScript` (no Python dependency). **Fallback:** PyGhidra headless (`support\pyghidraRun.bat`) using `C:\Python314\python.exe`.

**Dir layout:**
- Workspace: `C:\Users\Dali\Documents\gaames\aoe3rate-mod\`
- Ghidra project: `C:\Users\Dali\Documents\gaames\ghidra_proj\`
- Decompiled output: `C:\Users\Dali\Documents\gaames\ghidra_proj\aoe3y_decompiled\<addr>_<name>.c` + `dump_all_functions.txt`

## Step 1 — Ghidra headless import + full analysis + decompile dump

Set JAVA_HOME per command. Prefer JDK 25; if analyzeHeadless.bat fails on launch, retry with JDK 17.

**1a. Import + full analysis** (working dir: `C:\Users\Dali\Documents\gaames`):
```
set "JAVA_HOME=C:\Users\Dali\Tools\jdk-25.0.4+7" && "C:\Users\Dali\Documents\gaames\ghidra_12.1.3_PUBLIC\support\analyzeHeadless.bat" "C:\Users\Dali\Documents\gaames\ghidra_proj" AoE3Y -import "C:\Users\Dali\Documents\gaames\Age of Empires III - Complete Collection\age3y.exe" -overwrite
```
Wait — the -import may take minutes; run with a generous timeout. If launch fails, retry with JDK 17.

**1b. Write the Java decompile-dump post-script** at `C:\Users\Dali\Documents\gaames\aoe3rate-mod\ghidra_scripts\DumpAllFunctions.java`:
```java
// DumpAllFunctions.java — no package; run with analyzeHeadless -postScript
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import java.io.File;
import java.io.PrintWriter;

public class DumpAllFunctions extends GhidraScript {
    @Override public void run() throws Exception {
        File outDir = new File("C:/Users/Dali/Documents/gaames/ghidra_proj/aoe3y_decompiled");
        outDir.mkdirs();
        DecompInterface ifc = new DecompInterface();
        ifc.openProgram(currentProgram);
        PrintWriter big = new PrintWriter(
            new File("C:/Users/Dali/Documents/gaames/ghidra_proj/aoe3y_decompiled/dump_all_functions.txt"));
        int i = 0;
        for (FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
                it.hasNext(); ) {
            Function f = it.next();
            DecompileResults r = ifc.decompileFunction(f, 60, monitor);
            String c = (r == null || !r.decompileCompleted())
                ? "// DECOMPILE FAILED: " + f.getName()
                : r.getDecompiledFunction().getC();
            String fn = String.format("%08x_%s.c", f.getEntryPoint().getOffset(), sanitize(f.getName()));
            PrintWriter w = new PrintWriter(new File(outDir, fn));
            w.println("// Function: " + f.getName() + " @ 0x" + f.getEntryPoint().toString());
            w.println(c); w.close();
            big.println("=================== " + fn + " ===================");
            big.println(c);
            if (monitor.isCancelled()) break;
            if ((++i % 100) == 0) println("decompiled " + i + " functions");
        }
        big.close(); ifc.disposeProgram(); ifc.dispose();
        println("DONE " + i + " functions -> " + outDir);
    }
    static String sanitize(String n) { return n.replaceAll("[^A-Za-z0-9_.]", "_"); }
}
```
If the harness rejects the static helper, inline the replaceAll. IMPORTANT: if the full dump takes too long (very large binaries), you MAY write a bounded variant that dumps only a targeted slice FIRST (see Step 2 addresses) so the build isn't blocked, and run the full dump in the background. Prioritize the targeted functions over completeness.

**1c. Run the dump** (appends to the existing project, -noanalysis):
```
set "JAVA_HOME=C:\Users\Dali\Tools\jdk-25.0.4+7" && "C:\Users\Dali\Documents\gaames\ghidra_12.1.3_PUBLIC\support\analyzeHeadless.bat" "C:\Users\Dali\Documents\gaames\ghidra_proj" AoE3Y -process -noanalysis -postScript "C:\Users\Dali\Documents\gaames\aoe3rate-mod\ghidra_scripts\DumpAllFunctions.java"
```

**1d. FALLBACK — PyGhidra headless (only if 1c's script compile fails):**
```
set "JAVA_HOME=C:\Users\Dali\Tools\jdk-25.0.4+7" && C:\Users\Dali\Documents\gaames\ghidra_12.1.3_PUBLIC\support\pyghidraRun.bat "C:\Users\Dali\Documents\gaames\aoe3rate-mod\ghidra_scripts\dump_all_py.py"
```

## Step 2 — Address-map verification checklist (fresh decompile WINS)
Grep the dump for each known fact from prior session; if any address differs, the FRESH value wins and d3d9.c must use it. Record findings in `C:\Users\Dali\Documents\gaames\aoe3rate-mod\VERIFIED_ADDRESSES.md`:
1. Function `0x44EFFF` (decrypt): confirm `*(float *)(arr + 4*slot)` XOR `key[slot]`, key read via `*(DWORD *)(0xC6DF14 + 4*slot)`.
2. `0xC6DF14` contents == `28 48 AC 4F 94 F8 3A 35 8B D8 4C 3F AB 12 FB AF 20 B3 5B CA F9 AB C4 2A B1 A1 CF DA F2 E4 82 10`.
3. Count `0xC6DF38` == 8; secondary key `0xC6DF10` value.
4. Container: any function doing `mov eax, [0xDCB808]` then `[eax + 4*slot]`, and `0xDCB808` used in add/encrypt `0x49A859`? Confirm PLAYER_ARRAY-chain NOT needed. RVA = 0xDCB808 − 0x400000 = 0x9CB808.
5. Rate fields at `0xDCB808+0x160/0x164/0x168` (floats), updated by `0x86DA39`, divided via `0xB856BC`.
6. Helpers `0x49A859`, `0x4470DB`, `0x7E8171` → decrypted buffer `0xDCB360`.
7. Slots → resource mapping (0/1/2=food/wood/coin, 7=export — confirm or mark UNCONFIRMED).

## Step 3 — Write `d3d9.c` + `d3d9.def` (in `C:\Users\Dali\Documents\gaames\aoe3rate-mod\`)
- `d3d9.c`: global `g_real = LoadLibraryA("d3d9.dll")` in DllMain; **no hard link deps** on d3dx9 — load `d3dx9_25.dll` at runtime via LoadLibraryA + GetProcAddress(`D3DXCreateFontA`); if missing, HUD draws nothing but all device calls still forward.
- Hook: intercept `Direct3DCreate9` → call real → take the returned `IDirect3D9*` → get device via `CreateDevice` (call real vtable method) → patch the DEVICE vtable slot 17 (Present, offset 17*4) to our hook (page-protect + restore). Store original.
- Present hook: call original Present first (return its HRESULT), then draw HUD if `g_font && g_res`.
- **`locate_resources()`**: `void *base = GetModuleHandleA("age3y.exe"); g_res = base ? (void*)*(DWORD*)((BYTE*)base + 0x9CB808) : 0;` — guarded (SEH `__try`/`__except`) reads via `safe_r32`. DIRECT deref, NO PLAYER_ARRAY chain.
- Decrypt: replicate the game's own decrypt (from `0x44EFFF`) exactly — value[slot] = float( key_dword[slot] ^ res_dword[slot] ), where key table at `base + (0xC6DF14 - 0x400000)`.
- Rate fields: `rate = safe_r32(g_res + 0x160/0x164/0x168)` then `rate / (g_div ? g_div : 1.0f)` with `g_div = safe_r32(base + (0xB856BC - 0x400000))`.
- HUD: `D3DXCreateFontA(device, 16, 8, FW_BOLD, 0, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH|FF_DONTCARE, "Arial", &g_font)`, draw 4 lines (+N/s food/wood/coin + export). Skip drawing when `g_res==0` (main menu).
- Build must compile with **purely MSVC-compatible win32 types** if mingw headers lack d3d9 structs — prefer declaring the needed COM vtables manually (they're well-known) to avoid header dependency problems on mingw.
- `d3d9.def`: forward ALL standard d3d9 exports (`Direct3DCreate9`, `Direct3DCreate9Ex`, `D3DPERF_*`, `DebugSetLevel`, `DebugSetMute`, `D3Dbg` if present) via `Name = d3d9.Name` forwarding, OR implement thin wrappers calling GetProcAddress(g_real, name). The game only strictly needs `Direct3DCreate9`. Choose whichever is simplest and robust — thin wrappers are safest.

## Step 4 — Compile (32-bit) with w64devkit-x86
Prereq: download & extract w64devkit-x86 (needs internet — verify; if `curl` unavailable use PowerShell `Invoke-WebRequest`):
```
# from C:\Users\Dali\Documents\gaames\
curl -LO https://github.com/skeeto/w64devkit/releases/latest/download/w64devkit-x86.exe
7zr x w64devkit-x86.exe -oC:\Users\Dali\Documents\gaames\w64devkit-x86
```
If 7zr.exe is missing, run the self-extracting exe non-interactively if it supports it, otherwise extract via `C:\Python314\python.exe` + py7zr if installable, or ask Dali to double-click it. Bin at `C:\Users\Dali\Documents\gaames\w64devkit-x86\bin\i686-w64-mingw32-gcc.exe`.

Build (working dir `C:\Users\Dali\Documents\gaames\aoe3rate-mod`):
```
C:\Users\Dali\Documents\gaames\w64devkit-x86\bin\i686-w64-mingw32-gcc.exe -shared -static-libgcc -O2 d3d9.c -o d3d9.dll d3d9.def -lwinmm 2>build_log.txt
```
(Drop `-ld3d9` if you hand-declare vtables — no import lib dependency.) Verify PE machine:
```
C:\Users\Dali\Documents\gaames\w64devkit-x86\bin\i686-w64-mingw32-objdump.exe -p d3d9.dll | findstr /c:"architecture"
```
Expect `i386` (32-bit).

## Step 5 — Deploy
```
expand "C:\Users\Dali\Documents\gaames\Age of Empires III - Complete Collection\redist\DirectX\Apr2005_d3dx9_25_x86.cab" -F:d3dx9_25.dll "C:\Users\Dali\Documents\gaames\Age of Empires III - Complete Collection\"
copy d3d9.dll "C:\Users\Dali\Documents\gaames\Age of Empires III - Complete Collection\"
```
Also copy build artifacts (d3d9.c, d3d9.def, VERIFIED_ADDRESSES.md, dump files) stay in workspace. The LIVE game test is Dali's step — do not launch the game.

## Step 6 — Test assertions the tester will defend (MANDATORY requirements — build must be structured so these are testable)
1. HAPPY PATH — decrypt math: given the real key array `28 48 AC 4F 94 F8 3A 35 8B D8 4C 3F AB 12 FB AF 20 B3 5B CA F9 AB C4 2A B1 A1 CF DA F2 E4 82 10` and count 8: for each slot, `float D[s] = bits_as_float(bits(E[s] XOR K[s]))`. Provide a small self-contained unit (e.g. `decrypt_unit.c` with `decrypt_slot()` copied from d3d9.c's exact logic, plus a `test_decrypt.c` main, or a python equivalent in `test_decrypt.py`) so the tester can run a round-trip assertion `decrypt(encrypt(x)) == x` within finite range (0…1e6, no NaN/INF).
2. EDGE CASE — `locate_resources()` NULL-safe: when `*(DWORD*)(base + 0x9CB808) == 0` or the page is unmapped, locate must set `g_res=NULL` and HUD-draw called right after must return immediately drawing nothing, no crash. Provide a `test_nullsafe.c` stub harness that sets g_res=NULL (and a fake base with a zero at the offset) and calls the HUD path.
