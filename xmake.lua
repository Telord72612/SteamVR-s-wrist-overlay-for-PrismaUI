-- SteamVR's PrismaUI Wrist Overlays — SKSE plugin that renders PrismaUI
-- HTML/JS views onto native-SteamVR OpenVR overlays attached to the player's
-- wrist. Runs against a stock, unmodified PrismaUI ("addon mode": call-site
-- hooks for pixels + view size, view-map walk for discovery, public Invoke
-- for input), or against a patched fork exposing the PrismaVR_* ABI.

set_xmakever("3.0.0")

-- Path to your CommonLibVR checkout (commonlibsse-ng with VR support; it
-- bundles the OpenVR headers at extern/openvr/headers and the import lib at
-- extern/openvr/lib/win64). Override with the COMMONLIB_VR_DIR env var.
local commonlibvr = os.getenv("COMMONLIB_VR_DIR") or "D:/Games/My Skyrim/Tools/CommonLibVR-4.14.0"
includes(commonlibvr)

set_project("PrismaUISteamVR")
set_version("1.1.1")
set_license("MIT")
set_languages("c++23")
set_warnings("allextra")
set_encodings("utf-8")

add_rules("mode.release")
add_rules("plugin.vsxmake.autoupdate")

target("PrismaUISteamVR")
    add_rules("commonlibsse-ng.plugin", {
        name        = "PrismaUISteamVR",
        author      = "PrismaUI SteamVR",
        description = "Bridges PrismaUI views onto native-SteamVR overlays attached to the player's wrist."
    })

    add_files("src/*.cpp")
    add_headerfiles("src/*.h")
    add_includedirs("src")
    set_pcxxheader("src/pch.h")

    -- PrismaUI_API.h uses GetModuleHandle(L"...") so we need wide-char APIs.
    add_defines("UNICODE", "_UNICODE", "NOMINMAX", "WIN32_LEAN_AND_MEAN")

    -- OpenVR import lib (headers are auto-added by commonlibsse-ng's skyrim_vr config)
    add_linkdirs(commonlibvr .. "/extern/openvr/lib/win64")
    add_links("openvr_api")

    -- D3D11 + DXGI for the shared-texture path that bypasses SetOverlayRaw's
    -- per-process 197-submission ceiling.
    add_syslinks("d3d11", "dxgi", "dxguid")
