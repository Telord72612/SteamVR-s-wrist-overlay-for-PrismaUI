# SteamVR's PrismaUI Wrist Overlays

Puts [PrismaUI](https://www.nexusmods.com/skyrimspecialedition/mods/148718) web overlays on your **left wrist** in Skyrim VR, running native SteamVR.

Turn your left wrist palm-down (like checking a watch) and three numbered icons appear above your forearm. Click one with the right controller and that overlay opens as a floating panel — clickable, hoverable, scrollable, typeable (SteamVR keyboard), and resizable — while the game keeps running and you keep full movement.

Works on the **stock, unmodified PrismaUI from Nexus**. This mod ships only its own plugin — nothing of PrismaUI is bundled or modified on disk, which is what makes it distributable under PrismaUI's license.

## Features

- **Three wrist icons**, each bindable to any PrismaUI overlay in your load order by **name** via MCM — the dropdown list rebuilds itself as overlays are discovered, so installing or removing other mods never shuffles your bindings
- **Full interaction**: trigger = left click, A button = right click, right stick = scroll, hover works, and the SteamVR keyboard auto-summons when you click into a text field
- **Right-stick guard**: while your pointer is on the panel, stick-up/stick-down stop triggering jump/sneak — and come back the instant the pointer leaves
- **Resolution presets** (1080p / 1440p / 4K) as pure supersampling — identical layout, more sharpness
- **Zoom**: drag the panel's corner handle to change its physical size, independent of resolution
- **Fail-closed by design**: on an unknown PrismaUI version it installs nothing and PrismaUI is left byte-identical (see *Version support*)

## Requirements

- Skyrim VR with SKSE VR
- **PrismaUI 1.4.1** (stock, from Nexus)
- SkyUI VR + **MCM Helper (the VR build)** — load MCM Helper *after* SkyUI VR
- **SkyrimVR ESL Support** (`skyrimvresl`) — the plugin is ESL-flagged
- A **SteamVR-native** runtime. Under OpenComposite this mod stays dormant on purpose — PrismaUI 1.5+ has its own OCU path there.
- At least one PrismaUI-based overlay mod to display

## Installation

Grab the zip from [Releases](../../releases) and install it with your mod manager like any other mod. Enable `PrismaUIWristOverlays.esp`. Configure in MCM under **"SteamVR's PrismaUI Wrist Overlays"**.

Leave the mod enabled permanently once a save has seen it — it carries the ESP, and pulling an ESP out of a save's load order is what actually breaks saves. The plugin itself is safe on any setup because it fails closed.

## How it works

Stock PrismaUI renders each overlay page (via Ultralight) into a CPU-side BGRA bitmap, but exposes no API for the pixels or the view list. This plugin recovers both without touching PrismaUI on disk:

| Need | Mechanism |
|---|---|
| Pixels | A validated **call-site hook** on the one internal call to `CopyPixelsToTexture` — the frame arrives as arguments, already synchronized, exactly once per repaint |
| View list | Reading PrismaUI's own view map under its own SRWLOCK |
| Keeping pages off your monitor | A call-site hook on `DrawSingleTexture` that skips the flat blit for views shown on the wrist |
| 16:9 pages in VR | A call-site hook on the screen-size query (the HMD render target is square) + CSS zoom for supersampling |
| Input | **Public API only** — clicks, scrolling, and typing are injected as real DOM events through PrismaUI's `Invoke`, marshalled to the correct thread by PrismaUI itself |

Captured frames go to SteamVR overlays over a private D3D11 shared texture. The controller raycast, icon drawing, cursor, and panel placement are all done by this plugin directly against OpenVR.

Every hook site is verified before patching (the instruction must be a call resolving to the expected target); any mismatch disables that feature and leaves PrismaUI untouched.

## Version support

The plugin identifies the exact PrismaUI build at startup from a compiled-in offset table.

| PrismaUI | Status |
|---|---|
| 1.4.1 | ✅ Supported |
| anything else | Fails closed — icons appear, panels show a test pattern, PrismaUI untouched |

Supporting a new PrismaUI release takes one run of [`tools/gen_offsets.py`](tools/gen_offsets.py) against that release's `PrismaUI.dll` + `PrismaUI.pdb` (Nexus ships the PDB alongside), pasting the emitted row into `kPrismaBuilds` in `src/main.cpp`, and rebuilding. Offsets are generated offline — nothing is parsed at runtime.

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
| `package/` | The installable mod, exactly as shipped in the release zip |

## Credits

- **[PrismaUI](https://github.com/PrismaUI-SKSE/framework)** by the PrismaUI-SKSE team — the framework this rides on. This project is not affiliated with them; it hooks PrismaUI 1.4.1's internals read-only-on-disk and disables itself on any other version. 
- CommonLibVR, SKSE, MCM Helper, SkyUI — the shoulders everything VR stands on.

## License

[MIT](LICENSE) for everything in this repository except `src/PrismaUI_API.h`, which is PrismaUI's API header, redistributed as its own header text permits ("For modders: Copy this file into your own project if you wish to use this API").
