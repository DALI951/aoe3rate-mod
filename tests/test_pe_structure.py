#!/usr/bin/env python
"""test_pe_structure.py — SWARM additional verification (R14).

Checks the d3d9 proxy DLL and the game binary for the robustness items the
plan demands (plan section "ADDITIONAL VERIFICATION"):
  1. d3d9.dll is a 32-bit PE (machine 0x14C / i386) — a 64-bit DLL would not
     load into the 32-bit game process.
  2. age3y.exe is also i386 and its ImageBase == 0x400000 (all the RVAs in
     d3d9.c/VERIFIED_ADDRESSES.md assume VA = base + 0x400000).
  3. d3d9.dll exports Direct3DCreate9 + Direct3DCreate9Ex (the game's strict
     requirements) and the DebugSet*/D3DPERF_* family.
  4. d3d9.dll has NO import entry for d3dx9* (font must be loaded at runtime
     via LoadLibraryA/GetProcAddress — plan: "no hard link deps on d3dx9").
  5. d3d9.dll's own import set is a small allow-list subset (no accidental
     heavy imports that could fail on the LTSC image).
  6. age3y.exe statically imports d3d9.dll (proxy injection via import table).
  7. Version-gate primitives on age3y.exe: size 11,598,648 bytes.
"""
import struct
import sys

DLL = r"C:\Users\dali\aoe3rate-mod\d3d9.dll"
EXE = r"C:\Users\dali\Documents\gaames\Age of Empires III - Complete Collection\age3y.exe"

failures = 0


def pe_info(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:2] != b"MZ":
        return None
    pe_off = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe_off:pe_off + 4] != b"PE\0\0":
        return None
    machine = struct.unpack_from("<H", data, pe_off + 4)[0]
    nsec = struct.unpack_from("<H", data, pe_off + 6)[0]
    opt_off = pe_off + 24
    magic = struct.unpack_from("<H", data, opt_off)[0]
    image_base = struct.unpack_from("<I", data, opt_off + 28)[0]
    dd_off = opt_off + 96
    exp_rva, exp_size = struct.unpack_from("<II", data, dd_off)
    imp_rva, imp_size = struct.unpack_from("<II", data, dd_off + 8)
    sec_tbl = opt_off + struct.unpack_from("<H", data, pe_off + 20)[0]
    sections = []
    for i in range(nsec):
        off = sec_tbl + i * 40
        name = data[off:off + 8].rstrip(b"\0").decode("ascii", "replace")
        vsize, vaddr, rsize, raddr = struct.unpack_from("<IIII", data, off + 8)
        sections.append((name, vaddr, vsize, raddr, rsize))

    def rva_to_off(rva):
        for name, vaddr, vsize, raddr, rsize in sections:
            if vaddr <= rva < vaddr + max(vsize, rsize):
                return raddr + (rva - vaddr)
        return None

    exports = []
    if exp_rva:
        eo = rva_to_off(exp_rva)
        if eo is not None:
            nnames = struct.unpack_from("<I", data, eo + 24)[0]
            names_rva = struct.unpack_from("<I", data, eo + 32)[0]
            no = rva_to_off(names_rva)
            for i in range(nnames):
                nrva = struct.unpack_from("<I", data, no + i * 4)[0]
                xo = rva_to_off(nrva)
                if xo is not None:
                    end_ = data.index(b"\0", xo)
                    exports.append(data[xo:end_].decode("ascii", "replace"))
    imports = []
    if imp_rva:
        io = rva_to_off(imp_rva)
        while io is not None and io + 20 <= len(data):
            ilt = struct.unpack_from("<I", data, io)[0]
            name_rva = struct.unpack_from("<I", data, io + 12)[0]
            if ilt == 0 and name_rva == 0:
                break
            no = rva_to_off(name_rva)
            if no is not None:
                end_ = data.index(b"\0", no)
                imports.append(data[no:end_].decode("ascii", "replace"))
            io += 20
    return {"machine": machine, "image_base": image_base, "exports": exports,
            "imports": imports, "filesize": len(data)}


def check(cond, label, detail=""):
    global failures
    if cond:
        print(f"PASS: {label}")
    else:
        print(f"FAIL: {label} {detail}")
        failures += 1


dll = pe_info(DLL)
if dll is None:
    print("FAIL: d3d9.dll is not a valid PE")
    sys.exit(1)
check(dll["machine"] == 0x14C, "d3d9.dll machine==0x14C (i386 32-bit)",
      f"got 0x{dll['machine']:X}")
check("Direct3DCreate9" in dll["exports"], "d3d9.dll exports Direct3DCreate9")
check("Direct3DCreate9Ex" in dll["exports"], "d3d9.dll exports Direct3DCreate9Ex")
check("DebugSetLevel" in dll["exports"], "d3d9.dll exports DebugSetLevel")
check("DebugSetMute" in dll["exports"], "d3d9.dll exports DebugSetMute")
check(len([e for e in dll["exports"] if e.startswith("D3DPERF_")]) >= 5,
      "d3d9.dll exports the D3DPERF_* family",
      f"got {dll['exports']}")
bad = [i for i in dll["imports"] if "d3dx9" in i.lower()]
check(len(bad) == 0, "d3d9.dll imports NO d3dx9* (font loaded at runtime)",
      f"found {bad}")
allowed = {"kernel32.dll", "msvcrt.dll", "user32.dll", "winmm.dll"}
extra = [i.lower() for i in dll["imports"] if i.lower() not in allowed]
check(len(extra) == 0, "d3d9.dll import set is allow-listed (no heavy deps)",
      f"unexpected: {extra}")
print("  d3d9.dll imports:", ", ".join(dll["imports"]) or "(none)")
print("  d3d9.dll exports:", ", ".join(sorted(dll["exports"])))

exe = pe_info(EXE)
if exe is None:
    print("FAIL: age3y.exe is not a valid PE")
    sys.exit(1)
check(exe["machine"] == 0x14C, "age3y.exe machine==0x14C (i386 32-bit)",
      f"got 0x{exe['machine']:X}")
check(exe["image_base"] == 0x400000,
      "age3y.exe ImageBase==0x400000 (RVAs in d3d9.c are correct)",
      f"got 0x{exe['image_base']:X}")
check(exe["filesize"] == 11598648,
      "age3y.exe file size == 11,598,648 (version-gate expected size)",
      f"got {exe['filesize']}")
check(any(i.lower() == "d3d9.dll" for i in exe["imports"]),
      "age3y.exe statically imports d3d9.dll (proxy injection via import table)",
      f"imports: {exe['imports']}")

print("FAILURES:", failures)
sys.exit(1 if failures else 0)