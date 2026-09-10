#!/usr/bin/env python3
"""
disasm_smf.py — disassemble one SMF function (by mangled-name prefix) with every
call/jmp target resolved to a symbol, so the frame's branch structure is readable.

    python disasm_smf.py <dll> <pdb> "<name-prefix>" [max_bytes]
"""
import struct, sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from smfpdb import parse_pe, secva_of, symbol_table, containing, text_section, lookup


def main():
    dll, pdbp, prefix = sys.argv[1], sys.argv[2], sys.argv[3]
    maxb = int(sys.argv[4], 0) if len(sys.argv) > 4 else 0x400
    data = open(dll, "rb").read(); pdb = open(pdbp, "rb").read()
    pe = parse_pe(data); secva = secva_of(pe)
    syms, rvas = symbol_table(pdb, secva)
    start = lookup({"f": prefix.encode()}, syms)["f"]
    if start is None:
        print("symbol not found:", prefix); return
    # function end = next symbol start (good enough for a leaf-ish frame function)
    import bisect
    i = bisect.bisect_right(rvas, start)
    end = rvas[i] if i < len(rvas) else start + maxb
    end = min(end, start + maxb)
    off = pe["rva_to_off"](start)
    code = data[off:off + (end - start)]
    md = Cs(CS_ARCH_X86, CS_MODE_64); md.detail = False
    print("=== %s  @ 0x%07X .. 0x%07X (%d bytes) ===" % (prefix, start, end, end - start))
    for ins in md.disasm(code, start):
        note = ""
        if ins.mnemonic in ("call", "jmp") and ins.op_str.startswith("0x"):
            tgt = int(ins.op_str, 16)
            name, cs = containing(syms, rvas, tgt)
            note = "   ; -> %s%s" % (name[:72], "" if tgt == cs else " +0x%X" % (tgt - cs))
        elif ins.mnemonic.startswith("j") and ins.op_str.startswith("0x"):
            note = "   ; -> +0x%X" % (int(ins.op_str, 16) - start)
        print("  0x%07X  %-6s %-40s%s" % (ins.address, ins.mnemonic, ins.op_str, note))


if __name__ == "__main__":
    main()
