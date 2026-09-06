#!/usr/bin/env python3
"""extract_bar.py — list/extract ESPN-v2 .bar archives (AoE3 TAD, Data/Data2/...).

Use for the experimental loose-UI-XML path and future RE:
    python extract_bar.py list   "Data.bar"
    python extract_bar.py one    "Data.bar" "options.xml"     out.bin
    python extract_bar.py all    "Data.bar"                   outdir\
    python extract_bar.py names  "Data.bar"                   utf16-name-table

ESPN v2 layout (empirical from TAD bars):
  magic "ESPN" (4) | version u32 (2) | entry area |
  then UTF-16 name table + raw payload; offsets computed from the name table.
"""
import sys
import struct
import os

MAGIC = b"ESPN"


def read_entries(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != MAGIC:
        raise SystemExit("not an ESPN bar: %s" % path)
    ver = struct.unpack_from("<I", data, 4)[0]
    if ver != 2:
        raise SystemExit("unsupported ESPN version %d (expected 2)" % ver)

    # Entry directory: after magic+version. Layout (observed):
    #   count u32, then per entry: crc/hash u32, offset u32, size u32, name_ptr u32
    base = 8
    count = struct.unpack_from("<I", data, base)[0]
    if not 0 < count < 1_000_000:
        raise SystemExit("implausible entry count %d" % count)
    entries = []
    p = base + 4
    for i in range(count):
        a, b, c, d = struct.unpack_from("<4I", data, p)
        p += 16
        entries.append({"crc": a, "offset": b, "size": c, "name": d})
    return data, entries


def name_of(data, ptr):
    # name table holds UTF-16LE C strings; name_ptr is byte offset into the
    # file relative to the end of the entry directory (i.e. absolute).
    if ptr >= len(data):
        return "?"
    end = ptr
    while end + 1 < len(data) and not (data[end] == 0 and data[end + 1] == 0):
        end += 2
    try:
        return data[ptr:end].decode("utf-16-le")
    except Exception:
        return "?"


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    cmd = sys.argv[1]
    path = sys.argv[2]
    data, entries = read_entries(path)

    if cmd == "names":
        for i, e in enumerate(entries):
            print("%4d  off=%08X  size=%8d  %s" % (i, e["offset"], e["size"], name_of(data, e["name"])))
        return
    if cmd == "list":
        for e in sorted(entries, key=lambda x: name_of(data, x["name"])):
            print("%-40s %8d  off=%08X" % (name_of(data, e["name"]), e["size"], e["offset"]))
        return
    if cmd == "one":
        if len(sys.argv) < 5:
            raise SystemExit("usage: extract_bar.py one <bar> <name> <out>")
        want = sys.argv[3]
        out = sys.argv[4]
        for e in entries:
            if name_of(data, e["name"]) == want:
                blob = data[e["offset"]:e["offset"] + e["size"]]
                with open(out, "wb") as f:
                    f.write(blob)
                print("extracted %s (%d bytes) -> %s" % (want, len(blob), out))
                return
        raise SystemExit("entry not found: %s" % want)
    if cmd == "all":
        if len(sys.argv) < 4:
            raise SystemExit("usage: extract_bar.py all <bar> <outdir>")
        outdir = sys.argv[3]
        os.makedirs(outdir, exist_ok=True)
        for e in entries:
            nm = name_of(data, e["name"])
            if not nm or "/" in nm or "\\" in nm or nm in (".", ".."):
                nm = "entry_%08X" % e["offset"]
            blob = data[e["offset"]:e["offset"] + e["size"]]
            with open(os.path.join(outdir, nm), "wb") as f:
                f.write(blob)
        print("extracted %d entries -> %s" % (len(entries), outdir))
        return
    raise SystemExit("unknown command: %s" % cmd)


if __name__ == "__main__":
    main()