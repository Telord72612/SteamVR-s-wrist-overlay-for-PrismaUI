"""Shared PE + PDB helpers for the SMF tooling (pure Python, offline)."""
import bisect, re, struct


def parse_pe(data):
    e = struct.unpack_from("<I", data, 0x3C)[0]
    assert data[e:e + 4] == b"PE\0\0"
    coff = e + 4
    nsec = struct.unpack_from("<H", data, coff + 2)[0]
    opt_size = struct.unpack_from("<H", data, coff + 16)[0]
    opt = coff + 20
    pe32plus = struct.unpack_from("<H", data, opt)[0] == 0x20B
    imagebase = struct.unpack_from("<Q" if pe32plus else "<I", data, opt + (24 if pe32plus else 28))[0]
    ddir = opt + (112 if pe32plus else 96)
    nddir = struct.unpack_from("<I", data, opt + (108 if pe32plus else 92))[0]
    dirs = [struct.unpack_from("<II", data, ddir + i * 8) for i in range(nddir)]
    sect = opt + opt_size
    sections = []
    for i in range(nsec):
        off = sect + i * 40
        name = data[off:off + 8].rstrip(b"\0").decode("ascii", "replace")
        vsize, va, rawsize, rawptr = struct.unpack_from("<IIII", data, off + 8)
        sections.append((name, va, vsize, rawptr, rawsize))

    def rva_to_off(rva):
        for _n, va, vsz, rp, rs in sections:
            if va <= rva < va + max(vsz, rs):
                d = rva - va
                return rp + d if d < rs else None
        return None

    exports = {}
    if dirs and dirs[0][0]:
        eo = rva_to_off(dirs[0][0])
        nnames = struct.unpack_from("<I", data, eo + 24)[0]
        a_funcs, a_names, a_ords = struct.unpack_from("<III", data, eo + 28)
        fo, no, oo = rva_to_off(a_funcs), rva_to_off(a_names), rva_to_off(a_ords)
        for i in range(nnames):
            nrva = struct.unpack_from("<I", data, no + i * 4)[0]
            s = rva_to_off(nrva)
            nm = data[s:data.index(b"\0", s)].decode("ascii", "replace")
            ordi = struct.unpack_from("<H", data, oo + i * 2)[0]
            exports[nm] = struct.unpack_from("<I", data, fo + ordi * 4)[0]
    return {"sections": sections, "exports": exports, "imagebase": imagebase, "rva_to_off": rva_to_off}


def secva_of(pe):
    return {i + 1: s[1] for i, s in enumerate(pe["sections"])}


def decode_skse_version(v):
    return "%d.%d.%d.%d" % ((v >> 24) & 0xFF, (v >> 16) & 0xFF, (v >> 4) & 0xFFF, v & 0xF)


def _rec_rva(pdb, rs, rectyp, secva):
    """RVA for a symbol record at rs, or None. S_PUB32 name@+14; S_GPROC32/S_LPROC32 name@+35."""
    if rectyp == 0x110E:
        off = struct.unpack_from("<I", pdb, rs + 8)[0]
        seg = struct.unpack_from("<H", pdb, rs + 12)[0]
    elif rectyp in (0x1110, 0x110F):
        off = struct.unpack_from("<I", pdb, rs + 28)[0]
        seg = struct.unpack_from("<H", pdb, rs + 32)[0]
    else:
        return None
    return secva[seg] + off if seg in secva else None


def symbol_table(pdb, secva):
    """Every S_PUB32 / S_GPROC32 / S_LPROC32 record -> sorted [(rva, name)]. Dedup by rva (first name wins)."""
    seen = {}
    for rectyp, hdr in ((0x110E, 14), (0x1110, 35), (0x110F, 35)):
        tag = struct.pack("<H", rectyp)
        for m in re.finditer(re.escape(tag), pdb):
            rs = m.start() - 2
            if rs < 0:
                continue
            reclen = struct.unpack_from("<H", pdb, rs)[0]
            if reclen < hdr or reclen > 4096:
                continue
            rva = _rec_rva(pdb, rs, rectyp, secva)
            if rva is None:
                continue
            ns = rs + hdr
            ne = pdb.find(b"\0", ns, rs + 2 + reclen)
            if ne <= ns:
                continue
            name = pdb[ns:ne].decode("ascii", "replace")
            if rva not in seen or (seen[rva].startswith("?") is False and name.startswith("?")):
                seen.setdefault(rva, name)
    out = sorted(seen.items())
    return out, [r for r, _ in out]


def lookup(patterns, syms):
    """name-prefix (bytes) -> lowest RVA among symbols whose name starts with it."""
    out = {}
    for key, pat in patterns.items():
        p = pat.decode("ascii")
        best = None
        for rva, name in syms:
            if name.startswith(p) and (best is None or rva < best):
                best = rva
        out[key] = best
    return out


def containing(syms, rvas, rva):
    i = bisect.bisect_right(rvas, rva) - 1
    return (syms[i][1], syms[i][0]) if i >= 0 else ("?", 0)


def text_section(pe):
    return next(s for s in pe["sections"] if s[0] == ".text")
