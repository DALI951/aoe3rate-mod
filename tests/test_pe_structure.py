#!/usr/bin/env python
"""test_pe_structure.py — SWARM additional verification, run with C:\\Python314\\python.exe

Checks the d3d9 proxy DLL and the game binary for the robustness items the
plan demands (plan section "ADDITIONAL VERIFICATION"):
  1. d3d9.dll is a 32-bit PE (machine 0x14C / i386) — a 64-bit DLL would not
     load into the 32-bit game process.
  2. age3y.exe is also i386 and its ImageBase == 0x400000 (all the RVAs in
     d3d9.c/VERIFIED_ADDRESSES.md assume VA = base + 0x400000).
  3. d3d9.dll exports Direct3DCreate9 (the game's only strict requirement).
  4. d3d9.dll has NO import entry for d3dx9* (font DLL must be loaded at
     runtime via LoadLibrary/GetProcAddress — plan line: "no hard link deps
     on d3dx9").
"""
import struct
import sys

DLL = r"C:\Users\Dali\Documents\gaames\aoe3rate-mod\d3d9.dll"
EXE = r"C:\Users\Dali\Documents\gaames\Age of Empires III - Complete Collection\age3y.exe"

failures = 0

def pe_info(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:2] != b"MZ":
        return None
    pe_off = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe_off:pe_off+4] != b"PE\0\0":
        return None
    machine = struct.unpack_from("<H", data, pe_off + 4)[0]
    nsec = struct.unpack_from("<H", data, pe_off + 6)[0]
    opt_off = pe_off + 24
    magic = struct.unpack_from("<H", data, opt_off)[0]
    opt_size = 112 if magic == 0x10B else 240
    image_base = struct.unpack_from("<I", data, opt_off + 28)[0]
    # data directories (first dir entry starts at opt_off + 96 in PE32)
    dd_off = opt_off + 96
    exp_rva, exp_size = struct.unpack_from("<II", data, dd_off)
    imp_rva, imp_size = struct.unpack_from("<II", data, dd_off + 8)
    # section table
    sections = []
    sec_off = dd_off + 0x80
    for i in range(nsec):
        off = sec_off + i * 40
        name = data[off:off+8].rstrip(b"\0").decode("ascii", "replace")
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
                    end = data.index(b"\0", xo)
                    exports.append(data[xo:end].decode("ascii", "replace"))
    imports = []
    if imp_rva:
        io = rva_to_off(imp_rva)
        while io is not None and io < len(data):
            ilt = struct.unpack_from("<I", data, io)[0]
            name_rva = struct.unpack_from("<I", data, io + 12)[0]
            if ilt == 0 and name_rva == 0:
                break
            no = rva_to_off(name_rva)
            if no is not None:
                end = data.index(b"\0", no)
                imports.append(data[no:end].decode("ascii", "replace"))
            io += 20
    return {"machine": machine, "image_base": image_base, "exports": exports,
            "imports": imports}

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
bad = [i for i in dll["imports"] if i.lower().startswith("d3dx9")]
check(len(bad) == 0, "d3d9.dll imports NO d3dx9* (font loaded at runtime)",
      f"found {bad}")
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
check(any(i.lower() == "d3d9.dll" for i in exe["imports"]),
      "age3y.exe statically imports d3d9.dll (proxy injection via import table)",
      f"imports: {exe['imports']}")
print("  age3y.exe imports d3d9.dll:", "d3d9.dll" in exe["imports"])

print("FAILURES:", failures)
sys.exit(1 if failures else 0)