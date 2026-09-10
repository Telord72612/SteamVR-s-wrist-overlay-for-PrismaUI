#!/usr/bin/env python3
"""
gen_smf_offsets.py — SKSE Menu Framework offset generator for the wrist overlay's
native SMF source (no ImGui VR Helper). Doc 08 discipline: offline, call sites
not prologues, gate on SKSEPlugin_Version + sha256.

    python gen_smf_offsets.py <SKSEMenuFramework.dll> <SKSEMenuFramework.pdb> [--out DIR]
"""
import hashlib, json, os, struct, sys
from smfpdb import parse_pe, secva_of, decode_skse_version, symbol_table, lookup, containing, text_section

FUNCS = {
    "RenderDrawData":        b"?ImGui_ImplDX11_RenderDrawData@@",
    "DX11_NewFrame":         b"?ImGui_ImplDX11_NewFrame@@",
    "Win32_NewFrame":        b"?ImGui_ImplWin32_NewFrame@@",
    "ImGui_NewFrame":        b"?NewFrame@ImGui@@YAXXZ",
    "ImGui_Render":          b"?Render@ImGui@@YAXXZ",
    "ImGui_GetDrawData":     b"?GetDrawData@ImGui@@YA",
    "SMF_Render":            b"?Render@@YAXXZ",
    "InputHook_thunk":       b"?thunk@ProcessInputQueueHook@Hooks@@",
    "ShouldTheGameBePaused": b"?ShouldTheGameBePaused@WindowManager@@SA_NXZ",
    "Config_FreezeTime":     b"?FreezeTimeOnMenu@Config@@",
    "Client_BlitDrawData":   b"?BlitDrawData@Client@ImGuiVRHelperPluginAPI@@",
    "Client_RenderToPanel":  b"?RenderToPanel@Client@ImGuiVRHelperPluginAPI@@",
    "Client_RenderFrame":    b"?RenderFrame@Client@ImGuiVRHelperPluginAPI@@",
    "Client_PumpInput":      b"?PumpInput@Client@ImGuiVRHelperPluginAPI@@",
    "Client_IsConnected":    b"?IsConnected@Client@ImGuiVRHelperPluginAPI@@",
    "Client_HasFocus":       b"?HasFocus@Client@ImGuiVRHelperPluginAPI@@",
    "Client_PumpKeyboard":   b"?PumpKeyboard@Client@ImGuiVRHelperPluginAPI@@",
    "WM_IsAnyWindowOpen":    b"?IsAnyWindowOpen@WindowManager@@SA_NXZ",
    "WM_Open":               b"?Open@WindowManager@@SAXXZ",
    "WM_Close":              b"?Close@WindowManager@@SAXXZ",
    "EnableImGuiInput":      b"?EnableImGuiInput@@YAXXZ",
    "DisableImGuiInput":     b"?DisableImGuiInput@@YAXXZ",
    "GameLock_SetState":     b"?SetState@GameLock@@YAXW4State@1@@Z",
    "UI_RenderWindows":      b"?RenderWindows@Renderer@UI@@SAXXZ",
}
CALLSITE_TARGETS = ["RenderDrawData", "DX11_NewFrame", "Win32_NewFrame", "ImGui_NewFrame", "ImGui_Render",
                    "ShouldTheGameBePaused", "Client_BlitDrawData", "Client_RenderToPanel",
                    "Client_RenderFrame", "Client_PumpInput", "Client_PumpKeyboard", "WM_IsAnyWindowOpen",
                    "EnableImGuiInput", "DisableImGuiInput", "GameLock_SetState", "UI_RenderWindows"]


def scan_callsites(data, pe, targets):
    _n, va, vsz, rp, rs = text_section(pe)
    code = data[rp:rp + rs]
    want = {v: k for k, v in targets.items() if v}
    sites = []
    for i in range(len(code) - 5):
        op = code[i]
        if op != 0xE8 and op != 0xE9:
            continue
        tgt = va + i + 5 + struct.unpack_from("<i", code, i + 1)[0]
        if tgt in want:
            sites.append((va + i, "call" if op == 0xE8 else "jmp", want[tgt]))
    return sites


def main():
    if len(sys.argv) < 3:
        print(__doc__); sys.exit(1)
    dll_path, pdb_path = sys.argv[1], sys.argv[2]
    out_dir = sys.argv[sys.argv.index("--out") + 1] if "--out" in sys.argv else None
    data = open(dll_path, "rb").read()
    pdb = open(pdb_path, "rb").read()
    pe = parse_pe(data)
    secva = secva_of(pe)
    sha = hashlib.sha256(data).hexdigest().upper()
    ver_str, ver_raw = "UNKNOWN", None
    if "SKSEPlugin_Version" in pe["exports"]:
        o = pe["rva_to_off"](pe["exports"]["SKSEPlugin_Version"])
        ver_raw = struct.unpack_from("<I", data, o + 4)[0]
        ver_str = decode_skse_version(ver_raw)

    syms, rvas = symbol_table(pdb, secva)
    funcs = lookup(FUNCS, syms)
    sites = scan_callsites(data, pe, {k: funcs.get(k) for k in CALLSITE_TARGETS})

    print("=" * 72)
    print("  SKSE Menu Framework build fingerprint")
    print("=" * 72)
    print("  file      : %s   size %d   version %s (raw 0x%08X)" % (os.path.basename(dll_path), len(data), ver_str, ver_raw or 0))
    print("  sha256    : %s" % sha)
    print("  symbols   : %d (PUB32 + GPROC32 + LPROC32)" % len(syms))
    print("\n  FUNCTION RVAs")
    for k in FUNCS:
        v = funcs.get(k)
        print("    %-24s %s" % (k, ("0x%07X" % v) if v else "*** NOT FOUND ***"))
    print("\n  CALL SITES  (site RVA -> target)   [containing function +offset]")
    bytarget = {}
    for s_rva, kind, tname in sorted(sites, key=lambda s: (s[2], s[0])):
        bytarget.setdefault(tname, []).append(s_rva)
        cname, cstart = containing(syms, rvas, s_rva)
        print("    0x%07X  %-4s -> %-22s [%s +0x%X]" % (s_rva, kind, tname, cname[:70], s_rva - cstart))
    print("\n  CALL-SITE COUNT PER TARGET  (1 = hook it; >1 = hook all or reconsider; 0 = inlined)")
    for t in CALLSITE_TARGETS:
        print("    %-24s %d" % (t, len(bytarget.get(t, []))))

    rec = {"file": os.path.basename(dll_path), "size": len(data), "sha256": sha, "version": ver_str,
           "version_raw": ver_raw, "imagebase": pe["imagebase"], "functions": funcs,
           "callsites": [{"site": s, "kind": k, "target": t, "in": containing(syms, rvas, s)[0]} for s, k, t in sites]}
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
        p = os.path.join(out_dir, "smf_offsets.json")
        json.dump(rec, open(p, "w", encoding="utf-8"), indent=2)
        print("\n  wrote %s" % p)


if __name__ == "__main__":
    main()
