# SteamVR's PrismaUI Wrist Overlays

Puts [PrismaUI](https://www.nexusmods.com/skyrimspecialedition/mods/148718) web overlays **and [SKSE Menu Framework](https://www.nexusmods.com/skyrimspecialedition/mods/120099) menus** on your **left wrist** in Skyrim VR, running native SteamVR.

Turn your left wrist palm-down (like checking a watch) and three numbered icons appear above your forearm. Click one with the right controller and that overlay opens as a floating panel — clickable, hoverable, scrollable, typeable (SteamVR keyboard), and resizable — while the game keeps running and you keep full movement.

Works on the **stock, unmodified PrismaUI from Nexus**, and on **stock SKSE Menu Framework**. This mod ships only its own plugin — nothing of either framework is bundled or modified on disk, which is what makes it distributable under PrismaUI's license.

## Features

- **Three wrist icons**, each bindable by **name** via MCM to any PrismaUI overlay in your load order *or* to SKSE Menu Framework — the dropdown rebuilds itself as sources are discovered, so installing or removing other mods never shuffles your bindings
- **SKSE Menu Framework on the wrist** — every SMF-based mod's menu (MCM-style config panels, debug tools, whatever registers with SMF) becomes a wrist panel, **without pausing the game**. Edit settings live while you walk around
- **Full interaction**: trigger = left click, A button = right click, right stick = scroll, hover works, and the SteamVR keyboard auto-summons when you click into a PrismaUI text field
- **Right-stick guard**: while your pointer is on the panel, stick-up/stick-down stop triggering jump/sneak — and come back the instant the pointer leaves
- **Resolution presets** (1080p / 1440p / 4K) as pure supersampling — identical layout, more sharpness
- **Zoom**: drag the panel's corner handle to change its physical size, independent of resolution
- **Coexists with [ImGui VR Helper](https://www.nexusmods.com/skyrimspecialedition/mods/183466)** — if you have it, `F1` still opens SMF on its own floating panel exactly as stock, while the wrist icon opens it on your wrist. Neither path interferes with the other, and the helper is not required
- **Fail-closed by design**: on an unknown framework version it installs nothing and leaves that framework byte-identical (see *Version support*)

## Requirements

- Skyrim VR with SKSE VR
- SkyUI VR + **MCM Helper (the VR build)** — load MCM Helper *after* SkyUI VR
- **SkyrimVR ESL Support** (`skyrimvresl`) — the plugin is ESL-flagged
- A **SteamVR-native** runtime. Under OpenComposite this mod stays dormant on purpose — PrismaUI 1.5+ has its own OCU path there.
- **At least one source to display**, either:
  - **PrismaUI 1.4.1, 1.5.0 RC or 1.5.0** (stock, from Nexus) plus a PrismaUI-based overlay mod, and/or
  - **SKSE Menu Framework 3.13.0** plus any mod that registers a menu with it

Both frameworks are optional and independent — install either, or both.

## Installation

Grab the zip from [Releases](../../releases) and install it with your mod manager like any other mod. Enable `PrismaUIWristOverlays.esp`. Configure in MCM under **"SteamVR's PrismaUI Wrist Overlays"**.

Leave the mod enabled permanently once a save has seen it — it carries the ESP, and pulling an ESP out of a save's load order is what actually breaks saves. The plugin itself is safe on any setup because it fails closed.

## How it works

### PrismaUI source

Stock PrismaUI renders each overlay page (via Ultralight) into a CPU-side BGRA bitmap, but exposes no API for the pixels or the view list. This plugin recovers both without touching PrismaUI on disk:

| Need | Mechanism |
|---|---|
| Pixels | A validated **call-site hook** on the one internal call to `CopyPixelsToTexture` — the frame arrives as arguments, already synchronized, exactly once per repaint |
| View list | Reading PrismaUI's own view map under its own SRWLOCK |
| Keeping pages off your monitor | A call-site hook on `DrawSingleTexture` that skips the flat blit for views shown on the wrist |
| 16:9 pages in VR | A call-site hook on the screen-size query (the HMD render target is square) + CSS zoom for supersampling |
| Input | **Public API only** — clicks, scrolling, and typing are injected as real DOM events through PrismaUI's `Invoke`, marshalled to the correct thread by PrismaUI itself |

### SKSE Menu Framework source

SMF draws its menus with ImGui on the game's own render thread. Five validated call-site hooks inside SMF's own frame function redirect that work to the wrist, and every one is **pure passthrough** unless a wrist session is active:

| Need | Mechanism |
|---|---|
| Pixels | Hooks on the two mutually exclusive draw calls (`ImGui_ImplDX11_RenderDrawData` when the ImGui VR Helper is absent, `Client::RenderToPanel` when it is connected) redirect the draw into a shared D3D11 render target on the game's device |
| Input | A hook on the `ImGui::NewFrame` call site feeds the wrist raycast in as mouse position, button and wheel events through SMF's **exported** cimgui event API. It runs after SMF's own backends, so the wrist pointer wins |
| Correct pointer alignment | The render target is sized to **SMF's own layout space**, read from `io.DisplaySize` at runtime, so the panel geometry and the injected coordinates share one coordinate system |
| Not pausing the game | None — SMF's pause check is "is any window open with `BlockUserInput` set", and both are public atomics on its exported window interface. Wrist sessions open the menu with `BlockUserInput` clear, so the game never freezes. `F1` sessions leave the flag alone and keep SMF's own `FreezeTimeOnMenu` behaviour |
| Helper coexistence | Two hooks on the helper's focus-sync calls report "no window open" during a wrist session, so the helper neither steals the menu nor closes it |

Captured frames from both sources go to SteamVR overlays over a private D3D11 shared texture. The controller raycast, icon drawing, cursor, and panel placement are all done by this plugin directly against OpenVR.

Every hook site is verified before patching (the instruction must be a call resolving to the expected target); any mismatch disables that feature and leaves the target untouched.

### SKSE Menu Framework limitations

| | |
|---|---|
| Text input | SMF text fields don't accept typing from the wrist yet. PrismaUI text fields do |
| Right-click | Not forwarded on SMF panels |
| Event dispatch | The wrist opens the menu by setting SMF's public `IsOpen` flag, which skips SMF's own open/close notifications to consumer mods. `F1` sessions dispatch normally |

## Version support

The plugin identifies the exact build of each framework at startup from a compiled-in offset table. PrismaUI builds are keyed by version **plus** the DLL's PE `TimeDateStamp` and `SizeOfImage` — the version word alone is not unique (1.5.0 RC and 1.5.0 final both report `0x01050000` with every address moved).

| PrismaUI | Status |
|---|---|
| 1.4.1 | ✅ Supported |
| 1.5.0 RC | ✅ Supported (seams re-verified against the 1.5 source; exactly one call site per hook, same lock/signature contracts) |
| 1.5.0 (final, Nexus "Prisma UI 1.5") | ✅ Supported since 2.0.1 (decorated signatures of every hooked function and global identical to the RC; exactly one call site per hook) |
| anything else | Fails closed — icons appear, panels show a test pattern, PrismaUI untouched |

| SKSE Menu Framework | Status |
|---|---|
| 3.13.0 | ✅ Supported |
| anything else | Fails closed — the SMF source disables itself, logs the version it saw, and patches nothing |

Supporting a new release takes the matching offline generators — [`tools/gen_offsets.py`](tools/gen_offsets.py) + [`tools/find_prisma_callsites.py`](tools/find_prisma_callsites.py) for PrismaUI (functions/globals, then the call sites and PE identity), [`tools/gen_smf_offsets.py`](tools/gen_smf_offsets.py) for SMF — run against that release's DLL + PDB (both ship their PDB alongside), pasting the emitted row into `kPrismaBuilds` / `kSmfBuilds` in `src/main.cpp`, and rebuilding. Validate `find_prisma_callsites.py` against a build whose row is already known (`--expect`) before trusting it on a new one. Offsets are generated offline — nothing is parsed at runtime.

## Building from source

Requires [xmake](https://xmake.io) and a checkout of [CommonLibVR](https://github.com/alandtse/CommonLibVR) (the `ng` branch, which bundles the OpenVR headers/lib).

```bash
set COMMONLIB_VR_DIR=C:/path/to/CommonLibVR
xmake
```

Output: `build/windows/x64/release/PrismaUISteamVR.dll`. Drop it into the mod's `SKSE/Plugins/`.

## Repository layout

| Path | What |
|---|---|
| `src/main.cpp` | The entire plugin — overlays, raycast, input, hooks, MCM integration |
| `src/PrismaUI_API.h` | PrismaUI's public consumer API header (copied per the permission in its own header comment) |
| `tools/gen_offsets.py` | Offline offset generator for new PrismaUI versions (pure-Python PE/PDB parser) |
| `tools/gen_smf_offsets.py` | Offline offset generator for new SKSE Menu Framework versions — enumerates call sites and names the function each one sits in |
| `tools/smfpdb.py` | Shared PE + PDB parsing used by the SMF generator |
| `tools/disasm_smf.py` | Disassembles one SMF function with call targets resolved to symbols (for verifying a new build's seams) |
| `package/` | The installable mod, exactly as shipped in the release zip |

## Credits

- **[PrismaUI](https://github.com/PrismaUI-SKSE/framework)** by the PrismaUI-SKSE team — the framework this rides on. This project is not affiliated with them; it hooks PrismaUI's internals read-only-on-disk and disables itself on any unsupported version.
- **[SKSE Menu Framework](https://github.com/QTR-Modding/SKSE-Menu-Framework-3)** by Thiago099 / QTR-Modding — the ImGui menu registry that makes one integration cover every SMF-based mod. Same deal: nothing of it is bundled or modified, and unsupported versions disable the feature.
- **[ImGui VR Helper](https://github.com/alandtse/imgui-vr-helper)** by alandtse — not a dependency, but this plugin is built to coexist with it rather than fight it, and its source was the reference for how SMF's VR client path behaves.
- CommonLibVR, SKSE, MCM Helper, SkyUI — the shoulders everything VR stands on.

## License

[MIT](LICENSE) for everything in this repository except `src/PrismaUI_API.h`, which is PrismaUI's API header, redistributed as its own header text permits ("For modders: Copy this file into your own project if you wish to use this API").
