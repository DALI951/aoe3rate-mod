# verify_pe.py — verify d3d9.dll: i386/PE32, import set, exports, SHA256 (R14)
import struct, hashlib, sys

path = sys.argv[1] if len(sys.argv) > 1 else "d3d9.dll"
data = open(path, "rb").read()
if data[:2] != b"MZ":
    print("FAIL: not a PE (no MZ)"); sys.exit(1)

pe = struct.unpack_from("<I", data, 0x3C)[0]
num_sections = struct.unpack_from("<H", data, pe + 6)[0]
opt = pe + 24
opt_magic = struct.unpack_from("<H", data, opt)[0]
imgbase = struct.unpack_from("<I", data, opt + 28)[0]
sec_tbl = opt + struct.unpack_from("<H", data, pe + 20)[0]
N = len(data)

def rva2off(rva):
    for i in range(num_sections):
        off = sec_tbl + i * 40
        va = struct.unpack_from("<I", data, off + 12)[0]
        vsz = struct.unpack_from("<I", data, off + 8)[0]
        rawsz = struct.unpack_from("<I", data, off + 16)[0]
        rawptr = struct.unpack_from("<I", data, off + 20)[0]
        if va <= rva < va + max(vsz, rawsz):
            return rva - va + rawptr
    return None

status = 0
def chk(cond, msg):
    global status
    print(("PASS" if cond else "FAIL") + ": " + msg)
    if not cond: status = 1

machine = struct.unpack_from("<H", data, pe + 4)[0]
chk(machine == 0x14C, "machine=0x%04X i386" % machine)
chk(opt_magic == 0x10B, "optional header PE32 (magic 0x%04X)" % opt_magic)
chk(imgbase >= 0x10000 and imgbase < 0x80000000, "image base 0x%08X sane" % imgbase)

# imports
imports = []
imp_rva = struct.unpack_from("<I", data, opt + 104)[0]
if imp_rva:
    poff = rva2off(imp_rva)
    if poff is not None:
        for i in range(64):
            rec = poff + i * 20
            if rec + 20 > N:
                break
            orig = struct.unpack_from("<I", data, rec)[0]
            name_rva = struct.unpack_from("<I", data, rec + 12)[0]
            if orig == 0 and name_rva == 0:
                break
            if name_rva:
                noff = rva2off(name_rva)
                if noff is not None and noff < N:
                    end = data.find(b"\0", noff)
                    if end != -1:
                        try:
                            imports.append(data[noff:end].decode("ascii", "replace"))
                        except Exception:
                            pass

imp_set = set(imports)
allowed = {"KERNEL32.dll", "msvcrt.dll", "USER32.dll"}
chk(imp_set <= allowed, "imports subset {KERNEL32,msvcrt,USER32}: " + ",".join(sorted(imports)))

# exports
nnames = 0
exp_rva = struct.unpack_from("<I", data, opt + 96)[0]
if exp_rva:
    exp_off = rva2off(exp_rva)
    if exp_off is not None and exp_off + 40 <= N:
        nnames = struct.unpack_from("<I", data, exp_off + 24)[0]
chk(nnames >= 11, "exports >= 11 (got %d)" % nnames)

sha = hashlib.sha256(data).hexdigest()
print("sha256=" + sha)
print("size=%d" % len(data))
print("RESULT: " + ("VERIFY PASS" if status == 0 else "VERIFY FAIL"))
sys.exit(status)