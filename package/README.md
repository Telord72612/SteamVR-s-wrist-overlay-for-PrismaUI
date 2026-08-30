# SteamVR's PrismaUI Wrist Overlays

Puts PrismaUI web overlays (SkyrimNet dashboard, SeverActions, IntelEngine, …)
on your **left wrist** in native SteamVR. Turn the left wrist palm-down and
three numbered icons appear; click one with the right controller and that
overlay opens as a floating panel above your forearm — clickable, scrollable,
typeable (SteamVR keyboard), resizable.

Works on the **stock, unmodified PrismaUI from Nexus** ("addon mode"). No
patched `PrismaUI.dll` is bundled or required — this mod ships only its own
plugin, which is what makes it distributable under PrismaUI's license (§3.2
forbids redistributing a modified PrismaUI).

## Contents

| File | Purpose |
|---|---|
| `SKSE/Plugins/PrismaUISteamVR.dll` | The whole mod — draws the wrist icons, captures PrismaUI's rendered pages, submits them to SteamVR overlays, and forwards controller input back into the pages. |
| `PrismaUIWristOverlays.esp` | ESL-flagged registration plugin — a quest with the stock `MCM_ConfigBase` script attached. Makes the MCM menu appear. No world edits. |
| `MCM/Config/PrismaUIWristOverlays/config.json` | The menu layout ("SteamVR's PrismaUI Wrist Overlays"). Regenerated at runtime by the DLL so the icon dropdowns list every overlay found. |
| `MCM/Config/PrismaUIWristOverlays/settings.ini` | **Author defaults — required.** See below. |
| `MCM/Settings/PrismaUIWristOverlays.ini` | Your saved values. MCM Helper writes this when you change a setting; the DLL reads it live (~1/s). |

## Requirements

- **PrismaUI 1.4.1** (stock, from Nexus) — the supported build. Other versions
  are refused safely (see "Version safety" below).
- SkyUI VR + **MCM Helper (the VR build)** — the SE build will not load under SKSE VR.
  Load MCM Helper **after** SkyUI VR — both ship a loose `SKI_ConfigMenu.pex`
  and MCM Helper's copy must win.
- **SkyrimVR ESL Support (`skyrimvresl`)** — required for the ESL-flagged plugin.
- A **SteamVR-native** runtime. Under OpenComposite this mod stays dormant by
  design — PrismaUI's own OCU path handles VR there.
- At least one PrismaUI overlay mod to display (SkyrimNet, SeverActions, …).

## How it works (one paragraph)

Stock PrismaUI renders each overlay page into a CPU bitmap but offers no API to
read it. This mod hooks the one internal call site where those pixels pass by
as arguments, copies the frame for the view you're looking at, and submits it
to a SteamVR overlay over a D3D11 shared texture. The view list is read from
PrismaUI's own map; clicks, scrolling and typing are injected as DOM events
through PrismaUI's **public** API. PrismaUI itself is never modified on disk.

## Version safety (why it's safe to leave enabled)

The DLL identifies the exact PrismaUI build before touching anything. An
unknown version — including **PrismaUI 1.5.0 RC** until an offset row is
generated for it — installs **nothing**: PrismaUI is left byte-identical, every
other PrismaUI mod keeps working, the wrist icons still appear, and the panel
shows a test pattern instead of content. Each new PrismaUI release needs one
regenerated offset row (from its DLL+PDB pair) before the panel lights up on it.

Startup lines in `My Games/Skyrim VR/SKSE/PrismaUISteamVR.log`:

| Line | Meaning |
|---|---|
| `addon mode ACTIVE: pixel hook installed …` | Good — wrist panels will show real PrismaUI content. |
| `addon mode: PrismaUI version X is NOT a known build — installing NOTHING` | Unsupported PrismaUI version; icons appear, panels show a test pattern. |
| `Patched PrismaUI exports resolved (…)` | A forked PrismaUI with the `PrismaVR_*` API is installed (legacy fork mode). |
| `OpenComposite runtime detected — … staying DORMANT` | OCU; PrismaUI's own VR path handles the UI. |

Leave the mod enabled permanently — it carries `PrismaUIWristOverlays.esp`, and
pulling an ESP out of a save's load order is what actually breaks saves.

## Settings (MCM: "SteamVR's PrismaUI Wrist Overlays")

- **Resolution** — `0` = 1920x1080, `1` = 2560x1440, `2` = 3840x2160. Pure
  supersampling: layout is identical at every preset, only sharpness changes.
  **Takes effect on the next game launch** (a view's surface size is fixed when
  it is created; stock PrismaUI cannot resize). 1440p is the sweet spot.
- **Icon 1 / 2 / 3** — which overlay each wrist icon opens. `Auto` matches the
  slot's default mod by URL, `Off` hides just that icon, or pick an overlay by
  name. The list rebuilds itself each launch as overlays are discovered, and
  your choice is stored **by name**, so installing or removing other overlay
  mods will not shuffle your bindings.

While a panel is open, the right stick scrolls the page and **jump is
temporarily disabled** so scrolling doesn't make you hop; it comes back the
moment the panel closes (and is force-restored on every save load, so a save
made mid-panel can never leave jump stuck off).

## The `settings.ini` requirement (do not delete it)

A `ModSetting*` control **does not exist until an INI declares it.** `defaultValue`
in `config.json` only defines what "reset to default" restores — it does **not**
register the setting. MCM Helper builds its setting store from INI files only:
`MCM/Config/<mod>/settings.ini` (author defaults) then `MCM/Settings/<mod>.ini`
(user values). Ship a menu without the first file and every option reads `-1`,
setters silently no-op, dropdowns render blank, and nothing persists.

Section names in `settings.ini` must match the `:Section` suffix on each control
`id` in `config.json` (`iResolution:Display` → `[Display]`, `sIconSN:Icons` → `[Icons]`).

## Naming

- **Mod / MCM display name:** *SteamVR's PrismaUI Wrist Overlays*
- **Plugin:** `PrismaUIWristOverlays.esp` (ESL-flagged). MCM Helper keys its config
  folder off the plugin filename, so `MCM/Config/PrismaUIWristOverlays/` must keep
  that exact name. The apostrophe/spaces live only in the *display* name.
- **SKSE plugin:** `PrismaUISteamVR.dll` — deliberately unchanged. It is the plugin
  identity and the log filename (`SKSE/PrismaUISteamVR.log`).

## Renamed from "PrismaUI SteamVR Bridge" (2026-08-29)

The old plugin was `PrismaUISteamVR.esp`. A save made with it will report that
plugin as missing on first load after the switch. It is safe to continue: the ESP
is ESL-flagged and contains only an MCM registration quest with **no world edits**.
Accept the warning once and re-save; the new plugin registers the menu again
under the new name.
