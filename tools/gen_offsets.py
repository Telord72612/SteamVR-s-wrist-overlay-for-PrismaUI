#!/usr/bin/env python3
"""
gen_offsets.py — PrismaUI addon offset generator.

Takes a stock PrismaUI.dll + its matching PrismaUI.pdb and emits the
fingerprint + RVAs/offsets the addon needs, as JSON and as a ready-to-paste
C++ table row.

    python gen_offsets.py <PrismaUI.dll> <PrismaUI.pdb> [--out DIR]

Pure Python: parses the PE and the PDB directly. No dbghelp, no VS tools,
nothing shipped to end users. Run this OFFLINE for each new PrismaUI release,
paste the row into the addon's known-builds table, rebuild.

Everything it emits is derived from the binary the author shipped plus the
public source at github.com/PrismaUI-SKSE/framework (license 2.2 grants the
right to study the source). Ultralight SDK binaries are never touched.
"""
import hashlib
import json
import re
import struct
import sys
import os

# Symbols the addon needs. Mangled names as emitted by MSVC x64.
FUNCS = {
    "CopyPixelsToTexture":          b"?CopyPixelsToTexture@ViewRenderer@PrismaUI@@YA",
    "DrawSingleTexture":            b"?DrawSingleTexture@ViewRenderer@PrismaUI@@YA",
    # not hooked today, but useful for diagnostics / future phases
    "CopyBitmapToBuffer":           b"?CopyBitmapToBuffer@ViewRenderer@PrismaUI@@YA",
    "UpdateSingleTextureFromBuffer": b"?UpdateSingleTextureFromBuffer@ViewRenderer@PrismaUI@@YA",
    "RenderSingleView":             b"?RenderSingleView@ViewRenderer@PrismaUI@@YA",
    "IsValid":                      b"?IsValid@ViewManager@PrismaUI@@YA",
}
GLOBALS = {
    "views":      b"?views@Core@PrismaUI@@3V",
    "viewsMutex": b"?viewsMutex@Core@PrismaUI@@3V",
}
# PrismaView members the addon reads (id is offset 0 by the object model,
# originalUrl is read once per view to classify it).
MEMBERS = ["id", "originalUrl", "isHidden", "pixelBuffer",
           "bufferWidth", "bufferHeight", "bufferStride",
           "bufferMutex", "newFrameReady"]


def parse_pe(data):
    """Return dict with sections, exports, imagebase, codeview info."""
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    assert data[e_lfanew:e_lfanew + 4] == b"PE\0\0", "not a PE"
    coff = e_lfanew + 4
    nsec = struct.unpack_from("<H", data, coff + 2)[0]
    opt_size = struct.unpack_from("<H", data, coff + 16)[0]
    opt = coff + 20
    magic = struct.unpack_from("<H", data, opt)[0]
    pe32plus = (magic == 0x20B)
    imagebase = struct.unpack_from("<Q", data, opt + 24)[0] if pe32plus else \
        struct.unpack_from("<I", data, opt + 28)[0]
    ddir = opt + (112 if pe32plus else 96)
    nddir = struct.unpack_from("<I", data, opt + (108 if pe32plus else 92))[0]
    dirs = []
    for i in range(nddir):
        rva, sz = struct.unpack_from("<II", data, ddir + i * 8)
        dirs.append((rva, sz))

    sect = opt + opt_size
    sections = []          # 1-based index -> (name, VA, vsize, raw_ptr, raw_size)
    for i in range(nsec):
        off = sect + i * 40
        name = data[off:off + 8].rstrip(b"\0").decode("ascii", "replace")
        vsize, va, rawsize, rawptr = struct.unpack_from("<IIII", data, off + 8)
        sections.append((name, va, vsize, rawptr, rawsize))

    def rva_to_off(rva):
        for _n, va, vsz, rp, rs in sections:
            if va <= rva < va + max(vsz, rs):
                d = rva - va
                if d < rs:
                    return rp + d
        return None

    # ---- exports ----
    exports = {}
    if dirs and dirs[0][0]:
        eo = rva_to_off(dirs[0][0])
        if eo is not None:
            # IMAGE_EXPORT_DIRECTORY: NumberOfNames +24, AddressOfFunctions +28,
            # AddressOfNames +32, AddressOfNameOrdinals +36
            nnames = struct.unpack_from("<I", data, eo + 24)[0]
            a_funcs, a_names, a_ords = struct.unpack_from("<III", data, eo + 28)
            fo, no, oo = (rva_to_off(a_funcs), rva_to_off(a_names),
                          rva_to_off(a_ords))
            if None not in (fo, no, oo):
                for i in range(nnames):
                    nrva = struct.unpack_from("<I", data, no + i * 4)[0]
                    s = rva_to_off(nrva)
                    if s is None:
                        continue
                    nm = data[s:data.index(b"\0", s)].decode("ascii", "replace")
                    ordi = struct.unpack_from("<H", data, oo + i * 2)[0]
                    frva = struct.unpack_from("<I", data, fo + ordi * 4)[0]
                    exports[nm] = frva

    # ---- CodeView (debug dir index 6) ----
    cv = None
    if len(dirs) > 6 and dirs[6][0]:
        do = rva_to_off(dirs[6][0])
        for i in range(dirs[6][1] // 28):
            b = do + i * 28
            dtype = struct.unpack_from("<I", data, b + 12)[0]
            praw = struct.unpack_from("<I", data, b + 24)[0]
            if dtype == 2 and data[praw:praw + 4] == b"RSDS":
                g = data[praw + 4:praw + 20]
                age = struct.unpack_from("<I", data, praw + 20)[0]
                guid = "%08X-%04X-%04X-%s-%s" % (
                    struct.unpack_from("<I", g, 0)[0],
                    struct.unpack_from("<H", g, 4)[0],
                    struct.unpack_from("<H", g, 6)[0],
                    g[8:10].hex().upper(), g[10:16].hex().upper())
                end = data.index(b"\0", praw + 24)
                cv = {"guid": guid, "age": age,
                      "pdb": data[praw + 24:end].decode("ascii", "replace")}
    return {"sections": sections, "exports": exports, "imagebase": imagebase,
            "codeview": cv, "rva_to_off": rva_to_off}


def decode_skse_version(v):
    """SKSE MakeVersion: major<<24 | minor<<16 | patch<<4 | beta"""
    return "%d.%d.%d.%d" % ((v >> 24) & 0xFF, (v >> 16) & 0xFF,
                            (v >> 4) & 0xFFF, v & 0xF)


def pdb_public_rvas(pdb, secva, patterns):
    """Scan S_PUB32 (0x110e) records for mangled names -> RVA."""
    out = {}
    for key, pat in patterns.items():
        best = None
        for m in re.finditer(re.escape(pat), pdb):
            i = m.start()
            rs = i - 14
            if rs < 0:
                continue
            reclen, rectyp = struct.unpack_from("<HH", pdb, rs)
            off = struct.unpack_from("<I", pdb, rs + 8)[0]
            seg = struct.unpack_from("<H", pdb, rs + 12)[0]
            if rectyp != 0x110E or seg not in secva:
                continue
            end = pdb.index(b"\0", i)
            if rs + 2 + reclen < end:          # record must cover the name
                continue
            rva = secva[seg] + off
            if best is None or rva < best:
                best = rva
        out[key] = best
    return out


def pdb_struct(pdb, struct_name, members):
    """LF_STRUCTURE sizeof + LF_MEMBER offsets."""
    size = None
    for m in re.finditer(re.escape(struct_name.encode() + b"\0"), pdb):
        rec = m.start() - 20
        if rec < 0:
            continue
        if struct.unpack_from("<H", pdb, rec)[0] == 0x1505:      # LF_STRUCTURE
            sz = struct.unpack_from("<H", pdb, rec + 18)[0]
            if sz and (size is None or sz > size):
                size = sz
    offs = {}
    for n in members:
        cands = set()
        for m in re.finditer(re.escape(n.encode() + b"\0"), pdb):
            rec = m.start() - 10
            if rec < 0:
                continue
            leaf = struct.unpack_from("<H", pdb, rec)[0]
            off = struct.unpack_from("<H", pdb, rec + 8)[0]
            if leaf == 0x150D and off < 0x8000:                  # LF_MEMBER
                cands.add(off)
        offs[n] = sorted(cands)
    return size, offs


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    dll_path, pdb_path = sys.argv[1], sys.argv[2]
    out_dir = None
    if "--out" in sys.argv:
        out_dir = sys.argv[sys.argv.index("--out") + 1]

    data = open(dll_path, "rb").read()
    pdb = open(pdb_path, "rb").read()
    pe = parse_pe(data)
    secva = {i + 1: s[1] for i, s in enumerate(pe["sections"])}

    sha = hashlib.sha256(data).hexdigest().upper()
    ver_str, ver_raw = "UNKNOWN", None
    if "SKSEPlugin_Version" in pe["exports"]:
        o = pe["rva_to_off"](pe["exports"]["SKSEPlugin_Version"])
        if o:
            ver_raw = struct.unpack_from("<I", data, o + 4)[0]
            ver_str = decode_skse_version(ver_raw)

    funcs = pdb_public_rvas(pdb, secva, FUNCS)
    globs = pdb_public_rvas(pdb, secva, GLOBALS)
    sizeof, members = pdb_struct(pdb, "PrismaUI::Core::PrismaView", MEMBERS)

    print("=" * 68)
    print("  PrismaUI build fingerprint")
    print("=" * 68)
    print("  file        : %s" % os.path.basename(dll_path))
    print("  size        : %d bytes" % len(data))
    print("  sha256      : %s" % sha)
    print("  imagebase   : 0x%X" % pe["imagebase"])
    print("  version     : %s (raw 0x%08X)" % (ver_str, ver_raw or 0))
    if pe["codeview"]:
        print("  pdb guid    : {%s}  age %d" % (pe["codeview"]["guid"],
                                                pe["codeview"]["age"]))
    print("  exports     : %s" % ", ".join(sorted(pe["exports"])))
    print()
    print("  FUNCTION RVAs")
    for k in FUNCS:
        v = funcs.get(k)
        print("    %-32s %s" % (k, ("0x%07X" % v) if v else "*** NOT FOUND ***"))
    print("  GLOBAL RVAs")
    for k in GLOBALS:
        v = globs.get(k)
        print("    %-32s %s" % (k, ("0x%07X" % v) if v else "*** NOT FOUND ***"))
    print("  PrismaView sizeof = %s" % (("%d (0x%X)" % (sizeof, sizeof)) if sizeof else "?"))
    for n in MEMBERS:
        c = members.get(n) or []
        print("    %-32s %s" % (n, ", ".join("+0x%03X" % x for x in c) or "?"))

    required = ["CopyPixelsToTexture", "DrawSingleTexture"]
    missing = [r for r in required if not funcs.get(r)]
    print()
    if missing:
        print("  !! MISSING REQUIRED SYMBOLS: %s" % ", ".join(missing))
        print("  !! Do NOT ship a table row for this build.")
    else:
        print("  C++ TABLE ROW (paste into the addon's known-builds table):")
        print()
        url = (members.get("originalUrl") or [None])[0]
        print('    { 0x%08X, "%s",  // %s' % (ver_raw or 0, sha[:16], ver_str))
        print('      /* CopyPixelsToTexture */ 0x%07X,' % funcs["CopyPixelsToTexture"])
        print('      /* DrawSingleTexture   */ 0x%07X,' % funcs["DrawSingleTexture"])
        print('      /* PrismaView::originalUrl */ %s,' %
              (("0x%03X" % url) if url is not None else "0 /* ?? */"))
        print('      /* PrismaView sizeof   */ %s },' % (sizeof or 0))

    rec = {"file": os.path.basename(dll_path), "size": len(data), "sha256": sha,
           "imagebase": pe["imagebase"], "version": ver_str, "version_raw": ver_raw,
           "codeview": pe["codeview"], "exports": sorted(pe["exports"]),
           "functions": {k: v for k, v in funcs.items()},
           "globals": {k: v for k, v in globs.items()},
           "prismaview_sizeof": sizeof, "prismaview_members": members}
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
        p = os.path.join(out_dir, "offsets.json")
        with open(p, "w", encoding="utf-8") as f:
            json.dump(rec, f, indent=2)
        print("\n  wrote %s" % p)


if __name__ == "__main__":
    main()
