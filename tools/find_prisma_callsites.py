#!/usr/bin/env python3
"""
find_prisma_callsites.py -- derive the four PrismaBuild fields gen_offsets.py does
NOT emit, so a new PrismaUI release is fully automatic:

    rvaCopyPixelsCall   the ONE E8 whose rel32 resolves to CopyPixelsToTexture
    rvaDrawSingleCall   the ONE E8 whose rel32 resolves to DrawSingleTexture
    rvaScreenSize       the GetScreenSize thunk PrismaUI calls to size new views
    rvaScreenSizeCall   its call site -- identified as the E8 immediately followed
                        by `mov [rip+disp], rax` whose target is Core::screenSize

It also prints the PE identity (TimeDateStamp, SizeOfImage) that keys each row:
the SKSEPlugin_Version word alone does not identify a build.

Doc 08 rules: call sites, never prologues; exactly one caller or refuse.

    python find_prisma_callsites.py <PrismaUI.dll> <PrismaUI.pdb> [--expect RC-row-values]

Validate on a build whose row is already known before trusting it on a new one.
"""
import struct
import sys

from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from smfpdb import parse_pe, secva_of, symbol_table, lookup, text_section, decode_skse_version

SYMS = {
    "CopyPixels": b"?CopyPixelsToTexture@ViewRenderer@PrismaUI@@YA",
    "DrawSingle": b"?DrawSingleTexture@ViewRenderer@PrismaUI@@YA",
    "screenSize": b"?screenSize@Core@PrismaUI@@3",
}


def e8_callers(code, text_va, target):
    out = []
    for i in range(len(code) - 5):
        if code[i] == 0xE8:
            if text_va + i + 5 + struct.unpack_from("<i", code, i + 1)[0] == target:
                out.append(text_va + i)
    return out


def main():
    dll, pdbp = sys.argv[1], sys.argv[2]
    data = open(dll, "rb").read()
    pdb = open(pdbp, "rb").read()
    pe = parse_pe(data)
    secva = secva_of(pe)
    syms, _ = symbol_table(pdb, secva)
    # data globals are only in the public stream; reuse gen_offsets' PUB32 scan
    from gen_offsets import pdb_public_rvas
    rv = lookup({"CopyPixels": SYMS["CopyPixels"], "DrawSingle": SYMS["DrawSingle"]}, syms)
    rv["screenSize"] = pdb_public_rvas(pdb, secva, {"s": SYMS["screenSize"]})["s"]

    ver = None
    if "SKSEPlugin_Version" in pe["exports"]:
        o = pe["rva_to_off"](pe["exports"]["SKSEPlugin_Version"])
        ver = struct.unpack_from("<I", data, o + 4)[0]

    _n, tva, _vs, trp, trs = text_section(pe)
    code = data[trp:trp + trs]

    print("version        : %s (0x%08X)" % (decode_skse_version(ver) if ver else "?", ver or 0))
    # PE identity -- the row key. The version word is NOT unique per build
    # (1.5.0 RC and 1.5.0 final share 0x01050000).
    e = struct.unpack_from("<I", data, 0x3C)[0]
    print("TimeDateStamp  : 0x%08X" % struct.unpack_from("<I", data, e + 8)[0])
    print("SizeOfImage    : 0x%X" % struct.unpack_from("<I", data, e + 24 + 56)[0])
    for k in ("CopyPixels", "DrawSingle", "screenSize"):
        print("%-15s: %s" % (k, ("0x%07X" % rv[k]) if rv[k] else "*** NOT FOUND ***"))

    ok = True
    result = {}
    for key, name in (("CopyPixels", "rvaCopyPixelsCall"), ("DrawSingle", "rvaDrawSingleCall")):
        callers = e8_callers(code, tva, rv[key]) if rv[key] else []
        print("%-15s: %d E8 caller(s) %s" % (name, len(callers), ["0x%X" % c for c in callers]))
        if len(callers) != 1:
            ok = False
        else:
            result[name] = callers[0]

    # screen-size: find `mov [rip+d], rax` targeting Core::screenSize, preceded by an E8
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    found = []
    if rv["screenSize"]:
        # 48 89 05 <disp32> = mov [rip+disp], rax  writing Core::screenSize.
        # MSVC may spill the call's return through a stack slot first:
        #     call X ; mov [rsp+N],rax ; mov rax,[rsp+N] ; mov [rip+d],rax
        # so look BACK up to 32 bytes for an E8 and require that rax reaches the
        # write through NOTHING but a spill/reload of one and the same slot.
        for i in range(len(code) - 7):
            if not (code[i] == 0x48 and code[i + 1] == 0x89 and code[i + 2] == 0x05):
                continue
            if tva + i + 7 + struct.unpack_from("<i", code, i + 3)[0] != rv["screenSize"]:
                continue
            for back in range(5, 33):
                j = i - back
                if j < 0 or code[j] != 0xE8:
                    continue
                insns = list(md.disasm(code[j:i + 7], tva + j))
                if not insns or insns[0].mnemonic != "call" or insns[-1].address != tva + i:
                    continue
                mid = insns[1:-1]
                slot = None
                clean = True
                for ins in mid:
                    op = ins.op_str.replace(" ", "")
                    if ins.mnemonic == "mov" and op.startswith("qwordptr[rsp+") and op.endswith("],rax"):
                        slot = op[len("qwordptr[rsp+"):-len("],rax")]
                    elif ins.mnemonic == "mov" and op.startswith("rax,qwordptr[rsp+") and slot is not None                             and op[len("rax,qwordptr[rsp+"):-1] == slot:
                        pass
                    else:
                        clean = False
                        break
                if clean:
                    site = tva + j
                    callee = site + 5 + struct.unpack_from("<i", code, j + 1)[0]
                    found.append((site, callee))
                    break
    print("screenSize write sites preceded by a call: %s" %
          ["site 0x%X -> callee 0x%X" % f for f in found])
    if len(found) != 1:
        ok = False
    else:
        result["rvaScreenSizeCall"], result["rvaScreenSize"] = found[0]
        site = found[0][0]
        off = trp + (site - tva)
        print("  disasm around the call site:")
        for ins in md.disasm(data[off:off + 16], site):
            print("    0x%07X  %-6s %s" % (ins.address, ins.mnemonic, ins.op_str))

    print()
    if not ok:
        print("!! NOT UNIQUE -- do NOT ship a row for this build; inspect by hand.")
        sys.exit(1)
    print("four fields:")
    for k in ("rvaCopyPixelsCall", "rvaDrawSingleCall", "rvaScreenSize", "rvaScreenSizeCall"):
        print("  %-18s 0x%X" % (k, result[k]))

    if "--expect" in sys.argv:
        exp = [int(x, 16) for x in sys.argv[sys.argv.index("--expect") + 1].split(",")]
        got = [result["rvaCopyPixelsCall"], result["rvaDrawSingleCall"],
               result["rvaScreenSize"], result["rvaScreenSizeCall"]]
        print("\nVALIDATION against the known row: %s" %
              ("MATCH -- finder is trustworthy" if exp == got else "MISMATCH %s vs %s" %
               (["0x%X" % x for x in exp], ["0x%X" % x for x in got])))


if __name__ == "__main__":
    main()
