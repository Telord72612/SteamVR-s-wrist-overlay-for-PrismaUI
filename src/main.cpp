// PrismaUI SteamVR — wrist-menu / palm-down picker
//
// Universal PrismaUI VR adapter for native-SteamVR overlay rendering.
//
// Activation: when the player turns the LEFT wrist palm-DOWN (back of the
// forearm facing up — natural wristwatch-look pose), three semi-transparent
// blue circular icons appear floating just above the forearm:
//   [ SN ] [ SA ] [ IE ]
// labeled for SkyrimNet, SeverActions, IntelEngine respectively.
//
// Clicking an icon with the RIGHT controller toggles the corresponding
// PrismaUI overlay: the active panel floats above the forearm where it can
// be reached with the right hand. Clicking the icon of the currently-shown
// panel hides it. Clicking a different icon swaps which panel is active.
//
// When NO slot is active the bitmap pipeline is fully idle — no
// SetOverlayRaw calls — so we don't sustain bandwidth on SteamVR's overlay
// compositor when the user isn't actively reading a panel.

#include "pch.h"
#include "PrismaUI_API.h"
#include <cfloat>

#include <algorithm>  // std::find — slot-family duplicate-membership check
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>   // direct INI parse (bypasses PrivateProfileRedirector cache)
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <d3d11.h>
#include <d3d11_4.h>   // ID3D11Multithread (immediate-context lock)

namespace {
    // ---- Overlay configuration -------------------------------------------
    constexpr const char* kPanelKey  = "prismauisvr.panel";
    constexpr const char* kPanelName = "PrismaUI SteamVR Panel";

    // ---- Cursor (drawn onto the RGBA bitmap before submission) -----------
    // Bright yellow disc with black outline. Sized so its physical extent on
    // the panel matches the 3D cursor overlay shown on the icons.
    // 35 px outer / 14 px inner at 1920 px panel width × 35 cm physical
    // width ≈ 1.27 cm outer diameter, matching the 1.2 cm 3D cursor below.
    constexpr int kCursorOuterRadius = 15;
    constexpr int kCursorInnerRadius = 6;

    // ---- Slot system ------------------------------------------------------
    // Three fixed slots, each backed by a PrismaUI view discovered at runtime
    // by matching the view's originalUrl against a substring (case-insensitive).
    // Order is the visual order of icons on the forearm (left→right when
    // looking down at the palm-down hand).
    enum SlotId : int { Slot_None = -1, Slot_SN = 0, Slot_SA = 1, Slot_IE = 2, Slot_Count = 3 };

    struct SlotDef {
        const char* match;        // url substring used to bind a PrismaUI view
        const char* overlayKey;   // OpenVR overlay key for the icon
        const char* overlayName;  // OpenVR overlay friendly name
        char        glyph1;       // first character of the icon label
        char        glyph2;       // second character of the icon label
    };
    // Match strings are carefully scoped to avoid two distinct collision modes:
    //
    //   - PREFIX collision: `SeverActions` is a prefix of `SeverActionsDiary` —
    //     a bare "SeverActions" substring match would let the diary view steal
    //     the SA slot whenever PrismaUI enumerated it first. Trailing slash
    //     forces the path-segment boundary so only the directory we want hits.
    //
    //   - PARENT-FOLDER collision: SkyrimNet ships multiple sister views inside
    //     the same parent folder (SkyrimNet/dashboard/, SkyrimNet/chat/, …).
    //     A bare "SkyrimNet/" substring would match all of them and bind
    //     whichever was created first — non-deterministic across SkyrimNet
    //     versions. Match the FULL leaf-directory path ("SkyrimNet/dashboard/")
    //     so the SN slot deterministically picks dashboard regardless of which
    //     other views SkyrimNet may add.
    constexpr SlotDef kSlots[Slot_Count] = {
        { "SkyrimNet/dashboard/", "prismauisvr.icon.sn", "PrismaUI Icon 1", '1', ' ' },
        { "SeverActions/",        "prismauisvr.icon.sa", "PrismaUI Icon 2", '2', ' ' },
        { "IntelEngine/",         "prismauisvr.icon.ie", "PrismaUI Icon 3", '3', ' ' },
    };

    // ---- Slot family prefixes -------------------------------------------
    // BROADER match strings used to discover *all* sister-mod views that
    // belong to a slot, not just the primary one. Each sister mod typically
    // ships several views (a main panel + popup dialogs / sub-pages); when
    // the user clicks a tab in the primary view the sister mod calls Show()
    // on a different view (e.g. SeverActions opens SeverActionsDiary). We
    // discover those siblings at bind time, mark them ALL as externalConsumer
    // so PrismaUI doesn't 2D-draw them into the swap chain (would otherwise
    // appear as cut overlays on the desktop mirror / in the HMD's flat
    // compositing path), and watch their IsHidden state in the pump loop so
    // the wrist panel can FOLLOW whichever family member the sister mod has
    // shown — sub-views render at 1920×1080 on the wrist exactly like the
    // primary view does.
    //
    // Family prefix MUST stay broader than the primary kSlots[].match: e.g.
    // primary "SeverActions/" only matches the main view, but family
    // "SeverActions" also catches SeverActionsDiary, SeverActionsPrompt, etc.
    constexpr const char* kSlotFamilyPrefix[Slot_Count] = {
        "SkyrimNet/",     // matches SkyrimNet/dashboard/, SkyrimNet/chat/, …
        "SeverActions",   // matches SeverActions/, SeverActionsDiary/, SeverActionsPrompt/, SeverActionsArrestPrompt/, SeverActionsBrawlPrompt/
        "IntelEngine/",   // matches IntelEngine/dashboard/, plus any future sub-views
    };

    // ---- 5x7 pixel font (only the characters we need: S, N, A, I, E) -----
    // Each row is a 5-bit pattern, MSB-first (bit 4 = leftmost pixel).
    struct Glyph5x7 { uint8_t rows[7]; };
    constexpr Glyph5x7 kGlyphS = { { 0b01110, 0b10001, 0b10000, 0b01110, 0b00001, 0b10001, 0b01110 } };
    constexpr Glyph5x7 kGlyphN = { { 0b10001, 0b11001, 0b10101, 0b10101, 0b10011, 0b10001, 0b10001 } };
    constexpr Glyph5x7 kGlyphA = { { 0b01110, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001 } };
    constexpr Glyph5x7 kGlyphI = { { 0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b11111 } };
    constexpr Glyph5x7 kGlyphE = { { 0b11110, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b11110 } };
    constexpr Glyph5x7 kGlyph1 = { { 0b00100, 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110 } };
    constexpr Glyph5x7 kGlyph2 = { { 0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b01000, 0b11111 } };
    constexpr Glyph5x7 kGlyph3 = { { 0b11111, 0b00010, 0b00100, 0b00010, 0b00001, 0b10001, 0b01110 } };

    constexpr const Glyph5x7* GlyphFor(char c) {
        return (c == 'S') ? &kGlyphS :
               (c == 'N') ? &kGlyphN :
               (c == 'A') ? &kGlyphA :
               (c == 'I') ? &kGlyphI :
               (c == 'E') ? &kGlyphE :
               (c == '1') ? &kGlyph1 :
               (c == '2') ? &kGlyph2 :
               (c == '3') ? &kGlyph3 : nullptr;
    }

    // ---- Icon overlay sizing / positioning -------------------------------
    // Mirrors SpellWheelVR's "Left Wrist Top" placement:
    //   - parented to LEFT controller (TrackedDeviceRelative)
    //   - positioned in controller-LOCAL space along +Y (back-of-hand) and
    //     +Z (toward elbow), so the icons rotate with the wrist naturally
    //   - orientation faces controller +Y (the "watch face" direction)
    //   - visibility gated by HMD-look-at-wrist (not palm orientation) —
    //     SpellWheelVR's BarViewAngle equivalent
    // This way the icons stay anchored to the forearm in physical space and
    // simply DISAPPEAR when you look away — same feel as SpellWheelVR's bars.
    constexpr uint32_t kIconTexW = 64;
    constexpr uint32_t kIconTexH = 64;
    constexpr float    kIconWidthMeters     = 0.025f;  // 2.5 cm icon diameter (50% of previous 5 cm)
    constexpr float    kIconAboveWrist      = 0.04f;   // controller-local +Y above the wrist
    constexpr float    kIconForearmBase     = 0.03f;   // first icon, controller-local +Z toward elbow
    constexpr float    kIconSpacingMeters   = 0.03f;   // adjacent-icon spacing (now ACROSS the wrist, see below) — halved with icon size
    // After the user-tuned rotation chain, spacing icons along controller +Z
    // collapses them visually because that direction ends up nearly parallel
    // to the line of sight from the HMD. Spread them along controller +X
    // instead so the row is horizontal in the icon-face plane (side-by-side).
    constexpr bool     kIconSpreadAlongX    = true;

    // Post-rotation translation offsets added in controller-local space AFTER
    // the calibration rotation is applied. Directions match calibration
    // phases 2 (DOWN = -Y) and 6 (BACK toward elbow = +Z). User-tuned to
    // shift the whole rig (icons + panel) from the rotated rest pose.
    constexpr float    kIconPostRotOffsetY  = -0.05f;  // -5 cm (phase 2 direction +5 units)
    constexpr float    kIconPostRotOffsetZ  = 0.15f;   // +15 cm (phase 6: was 10, now +5 more)

    // PANEL-only post-rotation offsets. Applied IN ADDITION to the icon
    // offsets above, but ONLY to the panel — the icons stay on the forearm
    // while the panel can be repositioned independently. Same controller-
    // local axes as the calibration phases:
    //   X = phase 3/4 (LEFT/RIGHT)
    //   Y = phase 1/2 (UP/DOWN)
    //   Z = phase 5/6 (FRONT/BACK toward elbow)
    constexpr float    kPanelExtraOffsetX =  0.10f;  // was 0.20, -10 (phase 4, +X right)
    constexpr float    kPanelExtraOffsetY =  0.12f;  // was 0.09, +3 (phase 1, +Y up)
    constexpr float    kPanelExtraOffsetZ = -0.12f;  // was -0.09, +3 (phase 5, -Z toward fingertips)

    // PANEL-only extra rotation. Applied to the wrist-anchor matrix BEFORE
    // the translation offsets above, on top of the global rotation chain
    // (which still applies to both icons and panel). Same axis convention as
    // calibration phases 7/8/9. Composition: extrinsic X → Y → Z (for column
    // vectors: v' = R_z · R_y · R_x · v).
    constexpr float    kPanelExtraRotXDeg = 180.0f;  // phase 7 — flips panel face-down in its own local frame
    constexpr float    kPanelExtraRotYDeg = 30.0f;   // phase 8 — was 15, +15
    constexpr float    kPanelExtraRotZDeg = 0.0f;    // phase 9 — retune via F12 in-place rotation

    // Panel global alpha (0 = invisible, 1 = fully opaque). Applied via
    // SetOverlayAlpha at panel creation — independent of icon textures.
    constexpr float    kPanelOpacity = 1.0f;

    // Active panel size / position. Higher above the wrist + slightly further
    // toward the elbow than the icons — pops up "above and slightly behind"
    // the icons like a sci-fi forearm-computer screen.
    constexpr float    kPanelWidthMeters     = 0.35f;  // physical width at scale 1.0
    constexpr float    kPanelAboveWrist      = 0.14f;  // controller-local +Y (higher than icons)
    constexpr float    kPanelForearmCenter   = 0.10f;  // controller-local +Z, centered over icon row

    // ---- In-game resizable panel ----------------------------------------
    // The panel can be enlarged by grabbing a handle in its bottom-right
    // corner with the pointer trigger and pulling outward. Dragging grows
    // BOTH the HTML viewport (so more content fits — fixes overflow/cut) AND
    // the physical panel size, together. Default is 1920×1080 @ 0.35m (low
    // GPU bandwidth → no stutter); the user opts into the bigger/heavier
    // size per slot, remembered for the session.
    constexpr uint32_t kViewportBaseW   = 1920;
    constexpr uint32_t kViewportBaseH   = 1080;

    // ---- Two independent controls -----------------------------------------
    // ZOOM (the corner handle): physical panel size only — bigger/closer in
    // VR space, SAME content. A per-slot multiplier on kPanelWidthMeters.
    constexpr float    kPanelZoomMin   = 0.70f;   // pull in  → smaller
    constexpr float    kPanelZoomMax   = 2.00f;   // pull out → bigger
    // RESOLUTION (the MCM picker): how many pixels the page renders into —
    // i.e. how much CONTENT can fit (responsive pages reflow). Three presets;
    // the index is read from the MCM Helper settings INI (see ReadResolution).
    struct ResPreset { uint32_t w; uint32_t h; const char* name; };
    constexpr ResPreset kResPresets[] = {
        { 1920, 1080, "1920x1080 (Low)"  },
        { 2560, 1440, "2560x1440 (Medium)" },
        { 3840, 2160, "3840x2160 (High)" },
    };
    constexpr int kResPresetCount = static_cast<int>(sizeof(kResPresets) / sizeof(kResPresets[0]));

    // Grab-handle size as a FRACTION of the panel (resolution-independent:
    // the handle + its hit-test are defined in overlay UV space, so they
    // always sit at the visual top-right corner regardless of the bitmap /
    // texture / overlay size relationship). ~1/3 of the original 0.13 so it
    // doesn't cover panel content.
    constexpr float    kResizeHandleFrac = 0.045f;
    // Gesture sensitivity: zoom units gained per meter of right-controller
    // distance change relative to the left wrist (pull apart = bigger).
    constexpr float    kResizeSensitivity = 1.8f;

    // ---- Cursor (billboarded; only shows when raycast hits an icon) ------
    // Sized to match the bitmap-drawn cursor's apparent diameter on the
    // panel (~1.27 cm). Reduced from 2.2 cm so the icon hover indicator
    // and the panel click indicator look like the same pointer.
    constexpr uint32_t kCursorTexW = 32;
    constexpr uint32_t kCursorTexH = 32;
    constexpr float    kCursorWidthMeters     = 0.0125f;  // visible disc ≈ 1.1 cm, matched to the panel's 15-px cursor on a 960×0.35 m panel
    constexpr float    kCursorTowardHmdOffset = 0.006f;

    // ---- View-angle visibility (SpellWheelVR-style BarViewAngle) ---------
    // Show the wrist menu when the HMD is looking AT the left controller
    // within this half-cone angle. Originally 35° (cos≈0.819) — but the
    // panel now sits ~30 cm offset from the controller, so looking AT the
    // panel pushed the HMD-forward outside the cone and the gate kept
    // hiding everything mid-interaction. Widened to 60° (cos≈0.5) to cover
    // both the icons at the wrist AND the displaced panel.
    constexpr float    kLookAtWristCosThreshold = 0.5f;  // cos(60°)

    // ---- PrismaUI view shrinkdown / resize (DISABLED) --------------------
    // Two failed experiments live in this flag:
    //
    // 1. ORIGINALLY enabled at 960×540 to shrink per-submit byte cost in hopes
    //    of dodging SteamVR's ~197-submission SetOverlayRaw wedge. Wedge was
    //    empirically per-submit, not per-byte, so shrinking didn't help.
    //    Disabled.
    //
    // 2. RE-ENABLED 2026-06-05 at 1920×1080 to normalize PrismaUI's view-size
    //    output across user monitors, on the theory that:
    //      - 4K users were seeing only top-left quadrant of UI because
    //        view-create produced a 3840×2160 bitmap that UpdateSubresource
    //        clamped to our fixed 1920×1080 shared texture.
    //      - Overscan users saw missing edges from sub-1920×1080 bitmaps.
    //    Forcing every view to 1920×1080 via PrismaVR_ResizeView on bind
    //    would have made the bitmap match the shared texture exactly.
    //    REGRESSION DISCOVERED FOR THE 1080P USER: the sister-mod HTML
    //    (SkyrimNet, SeverActions, IntelEngine) is authored with breakpoints
    //    that lay out differently at exactly 1920×1080 than they did at the
    //    user's previous "screen-sized" render. Even though no pixels were
    //    being clipped after the fix, the HTML re-flowed in ways that cut
    //    UI elements off the visible canvas. Disabled and reverted.
    //
    // The cross-monitor normalization problem remains unsolved. Better
    // future direction: dynamically resize kSharedTex to match the bitmap
    // PrismaUI reports, instead of forcing PrismaUI to match the texture.
    // That way each user gets a render at their natural HTML viewport and
    // PrismaUISteamVR's texture follows along — no HTML re-flow surprises.
    constexpr bool     kPrismaResizeEnabled = false;
    constexpr uint32_t kPrismaTargetWidth   = 960;
    constexpr uint32_t kPrismaTargetHeight  = 540;

    // ---- Pre-emptive overlay handle recycling (legacy SetOverlayRaw path)
    // SteamVR wedges at ~197 submits per handle. We recycle (destroy +
    // recreate the panel overlay) at this submit count, well below the
    // ceiling, so each handle stays within its budget. The user sees a
    // single-frame flicker on swap. The transform is reapplied on the
    // next AnchorAllToLeftController call and the next submit re-shows.
    //
    // NOTE: recycle was empirically PROVEN to not help — the SteamVR wedge
    // is per-PROCESS (not per-handle): 150 submits on handle A + 47 on
    // handle B = same 197 wedge point. Kept around in case the texture
    // path also wedges; otherwise unused once kUseTexturePath = true.
    constexpr int      kPanelRecycleAt = 150;

    // ---- Texture-sharing path --------------------------------------------
    // Bypass SetOverlayRaw entirely. Create a single shared D3D11 texture in
    // our own device, hand its NT shared handle to SteamVR via
    // SetOverlayTexture once. Per-tick "updates" are just UpdateSubresource
    // calls on our texture — vrcompositor.exe reads from the same shared GPU
    // memory and doesn't accumulate per-submission internal resources.
    //
    // OpenVR docs explicitly recommend this path for "dynamic content";
    // SetOverlayRaw is documented as "high-overhead, static overlays only".
    constexpr bool     kUseTexturePath        = true;
    // MAX cap for the shared-texture allocation. The texture is reallocated to
    // EXACTLY the current bitmap size (see CreateSharedTexAtSize) so the
    // overlay shows it 1:1; this is just the ceiling the resolution presets +
    // realloc clamp respect. Sized for the "High" (3840×2160) preset. At the
    // default Low preset the texture is only 1920×1080 (~8 MB/frame).
    constexpr uint32_t kSharedTexWidth        = 3840;
    constexpr uint32_t kSharedTexHeight       = 2160;

    // ---- User-tuned orientation (from F12 calibration sequence) ----------
    // Extrinsic rotations applied to the wrist-anchor matrix, around the
    // controller's ORIGINAL local axes. Application order: X → Z → Y → X_final.
    // For a column vector v, that means: v' = R_xFinal · R_y · R_z · R_x · v.
    //
    // X_final was added after the XZY chain settled the orientation roughly
    // "on top of the hand" — the +180° flips the icon to its working pose
    // from there. It's a separate constant (not folded into the first X) so
    // the chain order remains explicit and tunable.
    constexpr float    kIconExtraRotXDeg      = -90.0f;  // calibration phase 7 (first X)
    constexpr float    kIconExtraRotYDeg      = 90.0f;   // calibration phase 8
    constexpr float    kIconExtraRotZDeg      = 135.0f;  // calibration phase 9
    constexpr float    kIconExtraRotXFinalDeg = 180.0f;  // final X applied after the XZY chain

    // ---- Interaction tuning ---------------------------------------------
    // Trigger axis threshold for treating the analog trigger as a "click".
    // Vive/Index/Oculus all expose the analog trigger on axis 1 .x in
    // VRControllerState_t::rAxis[1].x (0.0 = released, 1.0 = full pull).
    constexpr float kTriggerPressThreshold   = 0.55f;
    constexpr float kTriggerReleaseThreshold = 0.40f;  // hysteresis

    // Hover proximity gate — pointer-to-overlay distance threshold (meters)
    // beyond which the yellow cursor dot is hidden. Clicks still register
    // at any range; this just prevents the cursor from showing when the
    // user isn't actually reaching toward the wrist menu.
    constexpr float kCursorMaxDistance       = 0.15f;

    // Right-controller joystick → panel scroll + slider arrow keys.
    // |axis| below kJoystickDeadzone is treated as zero. Joystick Y is
    // multiplied by kScrollPixelsPerTick to produce the per-tick scroll
    // delta. We mirror PrismaUI's own laser-scroll convention here:
    // negative Y times the multiplier = push-up scrolls the page content
    // DOWN (i.e. the visible portion moves down, which "feels right").
    // Joystick X drives VK_LEFT / VK_RIGHT events at one keypress per
    // pump tick — works for HTML <input type=range> sliders, text caret
    // motion, focused list navigation, etc.
    constexpr float kJoystickDeadzone        = 0.30f;
    constexpr float kScrollPixelsPerTick     = 30.0f;
    constexpr int   kArrowKeyAxis            = 0;  // rAxis[0] = thumbstick (Index/Touch); trackpad on Vive wand

    // SteamVR overlay keyboard — uUserValue tags our keyboard requests so the
    // event drain can be sure the chars it's forwarding came from us.
    constexpr uint64_t kKbdUserValue = 0xDEADBEEFCAFEBABEULL;

    // ---- PrismaUI ABI (patched fork) -------------------------------------
    using PrismaVR_GetViewBitmap_t       = bool     (*)(uint64_t, void*, uint32_t, uint32_t*, uint32_t*, uint32_t*);
    using PrismaVR_GetViewBitmapIfNew_t  = bool     (*)(uint64_t, void*, uint32_t, uint32_t*, uint32_t*, uint32_t*);
    using PrismaVR_SetViewVREnabled_t    = void     (*)(uint64_t, bool);
    using PrismaVR_GetViewCount_t        = uint32_t (*)();
    using PrismaVR_GetViewInfo_t         = bool     (*)(uint32_t, uint64_t*, char*, uint32_t);
    using PrismaVR_FireMouseEvent_t      = bool     (*)(uint64_t, int, int, int, int);
    using PrismaVR_ResizeView_t          = bool     (*)(uint64_t, uint32_t, uint32_t);
    using PrismaVR_DeliverChar_t         = void     (*)(wchar_t);
    using PrismaVR_DeliverVKey_t         = void     (*)(int);
    using PrismaVR_DeliverCharToView_t   = bool     (*)(uint64_t, wchar_t);
    using PrismaVR_DeliverVKeyToView_t   = bool     (*)(uint64_t, int);
    using PrismaVR_FireScrollToView_t    = bool     (*)(uint64_t, int, int);

    PrismaVR_GetViewBitmap_t       gGetBitmap       = nullptr;
    PrismaVR_GetViewBitmapIfNew_t  gGetBitmapIfNew  = nullptr;
    PrismaVR_SetViewVREnabled_t gSetVREnab    = nullptr;
    PrismaVR_GetViewCount_t     gGetCount     = nullptr;
    PrismaVR_GetViewInfo_t      gGetViewInfo  = nullptr;
    PrismaVR_FireMouseEvent_t   gFireMouse    = nullptr;
    PrismaVR_ResizeView_t       gResizeView   = nullptr;  // optional — older PrismaUI builds lack this
    PrismaVR_DeliverChar_t      gDeliverChar  = nullptr;  // gated path (old PrismaUI). Avoid — fails for sister-plugin views.
    PrismaVR_DeliverVKey_t      gDeliverVKey  = nullptr;  // gated path (old PrismaUI). Avoid — fails for sister-plugin views.
    PrismaVR_DeliverCharToView_t gDeliverCharToView = nullptr;  // bypasses gates, takes explicit viewId — what we actually use
    PrismaVR_DeliverVKeyToView_t gDeliverVKeyToView = nullptr;  // bypasses gates, takes explicit viewId — what we actually use
    PrismaVR_FireScrollToView_t  gFireScrollToView  = nullptr;  // joystick-Y → scroll wheel

    // V2 interface — added for the JS console message callback used to
    // diagnose why sister-mod pages (e.g. SkyrimNet dashboard) don't connect
    // to their backing local HTTP server. nullptr means an older PrismaUI is
    // loaded that doesn't support V2; everything else still works.
    PRISMA_UI_API::IVPrismaUI2* gPrismaUI2 = nullptr;

    // ---- State ----------------------------------------------------------
    vr::VROverlayHandle_t       gPanelOvl     = vr::k_ulOverlayHandleInvalid;  // active PrismaUI panel
    vr::VROverlayHandle_t       gCursorOvl    = vr::k_ulOverlayHandleInvalid;  // floating cursor (shown only on icon hits)
    bool                        gCursorShownLast = false;
    vr::VROverlayHandle_t       gIconOvls[Slot_Count] = { vr::k_ulOverlayHandleInvalid, vr::k_ulOverlayHandleInvalid, vr::k_ulOverlayHandleInvalid };
    uint64_t                    gSlotViewIds[Slot_Count] = { 0, 0, 0 };       // PrismaUI view id bound to each slot (PRIMARY view per slot, semantically fixed after binding)

    // ---- Per-slot view families & active-view tracking ------------------
    // Each slot has a FAMILY of sister-mod views (primary + any siblings
    // discovered by kSlotFamilyPrefix URL match). At any moment exactly one
    // family member is the "active" view we display on the wrist panel:
    //   - default: primary (gSlotViewIds[i])
    //   - if a sibling becomes !IsHidden (sister mod called PrismaUI->Show()
    //     on it, e.g. user clicked a tab), pump loop switches the active to
    //     that sibling. The wrist panel then renders the sibling's bitmap
    //     and forwards clicks / scroll / keyboard to the sibling.
    //   - when the sibling goes hidden again, active falls back to primary.
    //
    // gSlotActiveViewIds[i] is what every consumer (pump bitmap fetch,
    // raycast click forwarding, keyboard delivery) should read. Updated
    // once per pump tick by UpdateActiveViewsForSlots().
    std::vector<uint64_t>       gSlotFamilyViewIds[Slot_Count];
    uint64_t                    gSlotActiveViewIds[Slot_Count] = { 0, 0, 0 };

    // ---- MCM "Icon -> Overlay" binding (by NAME) ------------------------
    // Read from Data/MCM/Settings/PrismaUIWristOverlays.ini  [Icons] sIconSN/SA/IE,
    // each a STRING chosen from the auto-generated menu: "Off" (hide THIS icon),
    // "Auto" (bind the slot by kSlots[].match URL substring), or an overlay
    // SHORT-NAME (e.g. "SkyrimNet/dashboard"). Binding by NAME is stable across
    // overlay-set changes — unlike a position index it never shifts when other
    // overlays are added/removed. Empty string is treated as "Auto".
    std::string                 gIconBind[Slot_Count];

    // Per-slot family-discovery prefix, resolved at primary-bind time. For an
    // auto binding this equals kSlotFamilyPrefix[i]; for an MCM-overridden
    // binding it is derived from the chosen view's URL so family-follow still
    // works if the user picked a known sister-mod view (else it is restricted
    // to just that one view). Empty string until the slot's primary is bound.
    std::string                 gSlotFamilyPrefixActive[Slot_Count];

    // Dedups the "overlays found" log + config.json regeneration: only redo
    // them when the live PrismaUI view-set size changes (sister mods create
    // their views gradually over the first few seconds of a load).
    int                         gLoggedOverlayListCount = -1;

    // Last MCM values actually applied. The pump live-poll re-reads the INI
    // ~1/s and compares against these to detect mid-session MCM changes:
    // resolution differences resize the bound views; icon-override
    // differences reset the slot so the rediscover path re-binds the icon.
    int                         gAppliedResolutionIndex = -1;
    std::string                 gAppliedIconBind[Slot_Count];   // live-poll watermark (set from snapshot)
    bool                        gIconBindSnapshotDone = false;

    // Per-slot ZOOM (physical size multiplier on kPanelWidthMeters). Driven
    // by the corner handle; remembered per slot for the session. 1.0 = default.
    float                       gSlotScale[Slot_Count] = { 1.0f, 1.0f, 1.0f };

    // Global RESOLUTION preset index (into kResPresets) — how many pixels the
    // page renders into. Read from the MCM Helper settings INI on each panel
    // activation. Default 0 (1920×1080).
    int                         gResolutionIndex = 0;

    // ---- Reload-on-activate state ---------------------------------------
    // When the user clicks an icon, we Invoke('location.reload()') on the
    // slot's primary view. That forces fresh page state, fresh initial-
    // fetch chain, back to main view. But it ALSO wipes our injected JS
    // hooks (netdiag, focus tracker) — they're regular window-scope code,
    // gone the moment the page reloads.
    //
    // Per-slot counter: > 0 means a reload was triggered N pump ticks ago
    // and we need to re-inject in (N - elapsed_ticks) ticks. When it hits
    // zero we re-run InstallNetworkDiagnostic + InstallFocusTracker on the
    // primary view. ~30 ticks @ 10 Hz = 3 s, generous for the reload to
    // finish even on slow loads.
    int                         gSlotReinjectCountdown[Slot_Count] = { 0, 0, 0 };
    static constexpr int        kReinjectCountdownTicks            = 30;

    PRISMA_UI_API::IVPrismaUI1* gPrismaUI     = nullptr;
    std::atomic<int>            gActiveSlot{ Slot_None };  // currently-shown panel slot
    std::atomic<bool>           gPumpRunning{false};
    std::atomic<bool>           gGameLoaded{false}; // true only between kPostLoadGame and kPreLoadGame
    std::thread                 gPumpThread;
    bool                        gOverlayCreated   = false;
    bool                        gIconsShownLast   = false;  // edge tracking for icon visibility
    bool                        gPanelShownLast   = false;  // edge tracking for panel visibility

    // Synthetic-panel state — the overlay self-test drawn when a slot has no
    // bound PrismaUI view. Lets the wrist UI be validated end-to-end without
    // depending on where the pixels come from.
    uint64_t                    gTestPatternTick      = 0;
    bool                        gLoggedSyntheticPanel = false;
    bool                        gLoggedFirstCapture   = false;  // addon-mode first real frame
    bool                        gLoggedCapTooSmall    = false;  // addon-mode oversized-frame warning
    bool                        gSlotIsSmf[Slot_Count] = { false, false, false };  // slot bound to the SKSE Menu Framework source
    bool                        gSmfPanelBound         = false;  // SetOverlayTexture done for the current SMF texture
    bool                        gSmfSubmitErrLogged    = false;  // dedupe the per-tick submit error
    void*                       gSmfBoundTex           = nullptr;// last texture handed to SteamVR (identity only, not owned)
    bool                        gJumpSuppressed       = false;  // stick guard active (jump+sneak masked while the dot is on the panel)
    uint32_t                    gStickSuppressedFlags = 0;      // UEFlag bits WE masked (snapshot at suppress time)
    int                         gStickLingerTicks     = 0;      // keeps the guard alive briefly after the dot leaves
    uint64_t                    gStickHealCount       = 0;      // diagnostic: external re-enables we overrode
    static constexpr int        kStickLingerHold      = 5;      // ~0.5 s at the 10 Hz pump

    // ---- Calibration mode -----------------------------------------------
    // Triggered by F12. Plays a 9-phase animation moving the SN icon through
    // all six controller-local cardinal translations + three local rotations
    // so the user can identify, by visible direction, exactly which axis
    // each named movement corresponds to.
    std::atomic<bool>                     gCalibrationMode{ false };
    std::chrono::steady_clock::time_point gCalibrationStart;

    // ---- Submission-health diagnostics ----------------------------------
    // We've been chasing a persistent VROverlayError_RequestFailed (error 23)
    // that hits SetOverlayRaw after sustained texture uploads. These counters
    // get dumped on first error + periodically while running so we can see
    // the rate / volume of submissions leading up to the failure.
    std::atomic<int64_t>                  gSubmitSuccessCount{ 0 };
    std::atomic<int64_t>                  gSubmitTotalBytes{ 0 };
    std::chrono::steady_clock::time_point gFirstSubmitTime{};
    std::chrono::steady_clock::time_point gLastSubmitTime{};
    bool                                  gFirstSubmitRecorded = false;

    // Per-handle submit counter for the recycle logic — resets to 0 every
    // time we destroy + recreate gPanelOvl.
    int                                   gPanelSubmitCount  = 0;
    // Set after a recycle to force the next pump iteration to submit a
    // frame to the new handle even if PrismaUI hasn't repainted.
    bool                                  gForceNextSubmit   = false;
    // Global recycle counter (for stats — how many times have we recycled
    // since the pump started).
    int                                   gPanelRecycleCount = 0;

    // ---- D3D11 shared-texture path state --------------------------------
    ID3D11Device*           gD3DDevice      = nullptr;
    ID3D11DeviceContext*    gD3DContext     = nullptr;
    ID3D11Texture2D*        gSharedTex      = nullptr;
    HANDLE                  gSharedTexHandle = nullptr;
    bool                    gTextureBoundToOverlay = false;  // SetOverlayTexture called for current handle
    // Current shared-texture allocation size. We keep the texture EXACTLY the
    // size of the bitmap being displayed (reallocating when it changes), so
    // the overlay shows the texture 1:1 with no dead region and no need for
    // SetOverlayTextureBounds (which SteamVR was not honoring for our DXGI
    // shared-handle texture). kSharedTexWidth/Height is just the MAX cap.
    uint32_t                gSharedTexW = 0;
    uint32_t                gSharedTexH = 0;

    // ---- Interaction state (pumped on the pump thread only) ------------
    uint32_t gLeftCtrlIdx     = vr::k_unTrackedDeviceIndexInvalid;
    bool     gTriggerHeld     = false;   // last trigger state (with hysteresis)
    bool     gAButtonHeld    = false;   // last A-button state (right-click)
    uint64_t gLoggedButtons  = 0;       // buttons already reported, for discovery logging
    bool     gHadHitLast      = false;   // last raycast hit on the PANEL specifically
    int      gLastPixelX      = -1;
    int      gLastPixelY      = -1;
    int      gIconHitLast     = Slot_None;  // last raycast hit on an icon (Slot_None if none / panel)
    bool     gIconClickPending = false;     // edge-trigger for icon click on trigger press
    // Last raycast 3D intersection in tracking space — used directly to
    // place the cursor overlay. Storing the geometric point (rather than
    // re-deriving it from UV via GetTransformForOverlayCoordinates) sidesteps
    // any UV-convention mismatch between OpenVR's intersection and transform
    // APIs that caused the cursor to be visually offset from where clicks
    // were landing.
    vr::HmdVector3_t gLastHitPoint  { 0.0f, 0.0f, 0.0f };
    vr::HmdVector3_t gLastHitNormal { 0.0f, 0.0f, 1.0f };
    // Last panel hit in raw overlay UV (v[0]=U: 0 left→1 right; v[1]=V: 0
    // bottom→1 top). The cursor is drawn from these at the DISPLAYED bitmap's
    // size each frame, so it can never exceed the bitmap (which caused the
    // "yellow dot disappears before the corner" bug when the raycast size and
    // the drawn-bitmap size disagreed).
    float gLastHitU = 0.0f;
    float gLastHitV = 0.0f;
    // Forward-axis auto-detection. Different controller hardware emits its
    // "barrel" along different local axes (Vive wand: -Z, Index: -Z+tilt,
    // Touch: -Z+30°tilt, custom rigs: anything). We test all 6 cardinal axes
    // each tick until one of them hits our overlay; that axis index is then
    // locked in and used for subsequent clicks.
    //  0=-Z (typical) 1=+Z 2=-Y 3=+Y 4=-X 5=+X 6=tip-component-state
    int      gPointerAxis     = -1;

    // ---- SteamVR overlay-keyboard state --------------------------------
    // gKeyboardOpen tracks whether the SteamVR overlay keyboard is currently
    // up for our panel. gFocusedTextInputView is the VIEW id whose active
    // element is a text input (0 = none). It is set from a JS focus tracker we
    // inject into EVERY family view (primary + siblings); each view bakes its
    // own id into the report ("<id>:1/0") so this single atomic identifies the
    // exact focused view — no per-slot flag to clobber. The pump summons the
    // keyboard only when gFocusedTextInputView == the currently DISPLAYED view
    // (gSlotActiveViewIds[activeSlot]), so detection view == delivery view and
    // a hidden/background view can never drive it. gKbdAutoSuppressed is set
    // when the user explicitly dismisses the keyboard (clicks the X) so we
    // don't immediately re-summon while the same input is still focused —
    // suppression clears the next time focus rises again (i.e. they click a
    // different input or re-focus the same one).
    bool                  gKeyboardOpen           = false;
    bool                  gKbdAutoSuppressed      = false;
    uint32_t              gKbdCharsThisSession    = 0;      // per-char events seen (incl. empty ones)
    uint32_t              gKbdRealCharsThisSession = 0;     // events carrying an actual character
    std::string           gKbdLastLiveText;                 // last seen live buffer (change detection)
    std::string           gKbdHarvested;                    // string rebuilt from the 1-char buffer
    bool                  gFocusLastForActiveSlot = false;
    std::atomic<uint64_t> gFocusedTextInputView{ 0 };

    // ---- helpers ---------------------------------------------------------

    // Case-insensitive substring search.
    bool ContainsIcase(const std::string& haystack, const char* needle)
    {
        if (!needle || !*needle) return false;
        std::string h = haystack;
        std::string n = needle;
        for (auto& c : h) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
        for (auto& c : n) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
        return h.find(n) != std::string::npos;
    }

    // Allocate executable memory as close as possible to `target`, so a 5-byte
    // rel32 branch can reach it. SKSE's own allocator searches from
    // (address - 2GB) upward and takes the first free block, which lands ~2GB
    // BELOW the hook site when hooking a foreign module — out of rel32 range.
    void* AllocNearAddress(uintptr_t target, size_t size)
    {
        SYSTEM_INFO si{};
        ::GetSystemInfo(&si);
        const uintptr_t gran = si.dwAllocationGranularity ? si.dwAllocationGranularity : 0x10000;
        constexpr uintptr_t kMaxDelta = 0x7FF00000;   // ~2GB with slack
        for (uintptr_t delta = gran; delta < kMaxDelta; delta += gran) {
            for (int dir = 0; dir < 2; ++dir) {
                uintptr_t addr = (dir == 0) ? (target - delta) : (target + delta);
                if (addr < gran) continue;
                addr &= ~(gran - 1);
                MEMORY_BASIC_INFORMATION mbi{};
                if (!::VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi))) continue;
                if (mbi.State != MEM_FREE) continue;
                if (void* mem = ::VirtualAlloc(reinterpret_cast<void*>(addr), size,
                                               MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE))
                    return mem;
            }
        }
        return nullptr;
    }

    // Snapshot the current view set and log it. Used both for diagnostics
    // and for choosing which view to display.
    struct ViewEntry { uint64_t id; std::string url; };

    // ======================================================================
    // STOCK-PRISMAUI SUPPORT ("addon mode")
    //
    // A stock PrismaUI.dll exports only RequestPluginAPI + the SKSE entry
    // points — none of the fork's PrismaVR_* API. The public vtable still
    // gives us CreateView/Show/Hide/IsHidden/Focus/Invoke, but NOT the two
    // things the wrist panel needs: the rendered PIXELS and a VIEW LIST.
    //
    // We recover both without modifying PrismaUI:
    //   * PIXELS  — intercept the single call site of
    //               ViewRenderer::CopyPixelsToTexture. PrismaUI calls it while
    //               already holding that view's bufferMutex and only after
    //               newFrameReady flipped, so the pixels arrive as ARGUMENTS,
    //               correctly synchronised, exactly once per new frame. No
    //               struct offsets are needed for the pixel data at all.
    //   * VIEWS   — read PrismaUI::Core::views (a std::map) under its own
    //               std::shared_mutex, which is a bare SRWLOCK (no CRT
    //               coupling: PrismaUI is /MT with a private CRT, so we must
    //               never touch its std::mutex objects — an SRWLOCK is safe).
    //
    // Everything is keyed off a table of KNOWN builds; an unrecognised
    // PrismaUI installs nothing and leaves the DLL byte-identical.
    // Offsets are generated by tools/_research/prismaui_addon/gen_offsets.py.
    // ======================================================================
    struct PrismaBuild {
        uint32_t    version;            // SKSEPlugin_Version pluginVersion
        const char* label;
        uintptr_t   rvaCopyPixels;      // ViewRenderer::CopyPixelsToTexture (callee)
        uintptr_t   rvaCopyPixelsCall;  // the ONE E8 call site invoking it
        uintptr_t   rvaViews;           // PrismaUI::Core::views  (std::map)
        uintptr_t   rvaViewsMutex;      // PrismaUI::Core::viewsMutex (SRWLOCK)
        uint32_t    offOriginalUrl;     // PrismaView::originalUrl (std::string)
        uintptr_t   rvaDrawSingle;      // ViewRenderer::DrawSingleTexture (callee)
        uintptr_t   rvaDrawSingleCall;  // the ONE E8 call site invoking it
        uintptr_t   rvaScreenSize;      // the GetScreenSize thunk PrismaUI calls
        uintptr_t   rvaScreenSizeCall;  // its call site, feeding Core::screenSize
        bool        hasNativeVROverlays; // 1.5+: PrismaUI's own floating VR quads
    };

    constexpr PrismaBuild kPrismaBuilds[] = {
        // 1.4.1.0 — Nexus "Prisma UI - Next-Gen Web UI Framework"
        // sha256 5C6DA41F…, pdb {1DF71091-2A56-470A-A9F4-738E2759F1A4} age 16
        { 0x01040010, "1.4.1.0", 0x91B40, 0x93880, 0x1AA108, 0x1AA128, 0x038,
          0x926C0, 0x92F39, 0xBFA50, 0x5D56F, false },
        // 1.5.0.0 RC — adds upstream's own OCU VR path + gamepad support.
        // sha256 5D76BF9E…, pdb {1DF71091-2A56-470A-A9F4-738E2759F1A4} age 24.
        // Seams re-verified against the 1.5 source (upstream dev branch):
        // UpdateSingleTextureFromBuffer still calls CopyPixelsToTexture under
        // bufferMutex after the newFrameReady exchange (same 5-arg signature),
        // DrawSingleTexture still takes shared_ptr by value, viewsMutex is
        // still a bare SRWLOCK (imports Acquire* but not InitializeSRWLock),
        // PrismaView still has id at +0 / originalUrl at +0x38 / sizeof 760.
        // Exhaustive E8 scans: exactly ONE caller for each callee below; the
        // screen-size call site is followed by `mov [rip+…], rax` targeting
        // Core::screenSize (0x1CD570) — the one write that sizes new views.
        { 0x01050000, "1.5.0 RC", 0xACD70, 0xAEAB0, 0x1CEA38, 0x1CEA58, 0x038,
          0xAD8F0, 0xAE169, 0xDA600, 0x5D9DF, true },
    };

    // ---- view resolution ------------------------------------------------
    // Stock PrismaUI creates every view at Core::screenSize, which it refreshes
    // each frame from the game's GetScreenSize(). In VR that reports the HMD
    // render target — 1024x1024 on this rig — so pages lay out SQUARE. The fork
    // solved this by patching Core.cpp to force 16:9; with no fork we hook the
    // ONE call site that feeds screenSize and substitute our own size.
    //
    // BSGraphics::ScreenSize is { uint32 width; uint32 height; } = 8 bytes,
    // returned in RAX, so the packed value is (height << 32) | width.
    //
    // NOTE: a view's surface size is fixed when the view is CREATED, and stock
    // exposes no resize. So a resolution change applies to views created after
    // it — i.e. on the next launch — not to ones already on screen.
    using GetScreenSize_t = uint64_t (*)(void*);
    GetScreenSize_t       gOrigGetScreenSize = nullptr;
    std::atomic<uint32_t> gForcedViewW{ 0 };   // 0 = don't override
    std::atomic<uint32_t> gForcedViewH{ 0 };

    uint64_t HookedGetScreenSize(void* self)
    {
        const uint64_t real = gOrigGetScreenSize(self);
        const uint32_t w = gForcedViewW.load(std::memory_order_relaxed);
        const uint32_t h = gForcedViewH.load(std::memory_order_relaxed);
        if (w && h) return (static_cast<uint64_t>(h) << 32) | static_cast<uint64_t>(w);
        return real;
    }

    // Views we render ourselves — PrismaUI must NOT also blit them across the
    // flat screen. This is the stock-mode equivalent of the fork's
    // externalConsumer flag. Written by the pump, read by the draw hook on the
    // render thread; a fixed array of atomics so the hook never allocates.
    constexpr int         kMaxClaimed = 24;
    std::atomic<uint64_t> gClaimedViews[kMaxClaimed];
    std::atomic<int>      gClaimedCount{ 0 };

    inline bool IsClaimedView(uint64_t id)
    {
        const int n = gClaimedCount.load(std::memory_order_acquire);
        for (int i = 0; i < n && i < kMaxClaimed; ++i)
            if (gClaimedViews[i].load(std::memory_order_relaxed) == id) return true;
        return false;
    }

    // MSVC container layout, confirmed against the shipped PDB:
    //   std::map        : _Myhead +0x00, _Mysize +0x08
    //   std::_Tree_node : _Left +0x00, _Parent +0x08, _Right +0x10,
    //                     _Color +0x18, _Isnil +0x19, _Myval +0x20
    //   value_type      : key(uint64) +0x00, shared_ptr +0x08  (=> node+0x28)
    //   std::shared_ptr : _Ptr +0x00, _Rep +0x08
    constexpr uintptr_t kNodeLeft = 0x00, kNodeParent = 0x08, kNodeRight = 0x10;
    constexpr uintptr_t kNodeIsNil = 0x19, kNodeKey = 0x20, kNodeSharedPtr = 0x28;

    const PrismaBuild* gStockBuild = nullptr;   // non-null => addon mode active
    uintptr_t          gStockBase  = 0;

    // ---- captured frame (hook writes, pump reads) ------------------------
    // Double-buffered so the pump never reads a half-written frame. The hook
    // runs on the render thread inside Present while PrismaUI holds its own
    // bufferMutex: it must NEVER allocate, log, lock, or call OpenVR/D3D.
    std::vector<uint8_t>  gCapBuf[2];
    std::atomic<int>      gCapReady{ -1 };      // index of the last complete frame
    std::atomic<uint64_t> gCapSeq{ 0 };         // bumped per captured frame
    std::atomic<uint32_t> gCapW{ 0 }, gCapH{ 0 }, gCapStride{ 0 };
    std::atomic<uint64_t> gCapWantView{ 0 };    // pump -> hook: the view we display
    std::atomic<size_t>   gCapNeedBytes{ 0 };   // hook -> pump: "grow the buffers"
    std::atomic<uint64_t> gCapDrops{ 0 };       // frames skipped because too small

    using CopyPixels_t = void (*)(void*, void*, uint32_t, uint32_t, uint32_t);
    CopyPixels_t gOrigCopyPixels = nullptr;

    // THE PIXEL HOOK. Hard rules: no allocation, no logging, no locks.
    void HookedCopyPixelsToTexture(void* viewData, void* pixels,
                                   uint32_t width, uint32_t height, uint32_t stride)
    {
        if (viewData && pixels && width && height && stride) {
            const uint64_t id = *reinterpret_cast<const uint64_t*>(viewData);  // id is at +0
            if (id != 0 && id == gCapWantView.load(std::memory_order_relaxed)) {
                const size_t need = static_cast<size_t>(height) * stride;
                const int    idx  = 1 - (gCapReady.load(std::memory_order_relaxed) == 0 ? 0 : 1);
                if (gCapBuf[idx].size() >= need) {
                    std::memcpy(gCapBuf[idx].data(), pixels, need);
                    gCapW.store(width, std::memory_order_relaxed);
                    gCapH.store(height, std::memory_order_relaxed);
                    gCapStride.store(stride, std::memory_order_relaxed);
                    gCapReady.store(idx, std::memory_order_release);
                    gCapSeq.fetch_add(1, std::memory_order_release);
                } else {
                    // Too small — ask the pump to grow and drop this frame.
                    gCapNeedBytes.store(need, std::memory_order_relaxed);
                    gCapDrops.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
        gOrigCopyPixels(viewData, pixels, width, height, stride);
    }

    // Suppress PrismaUI's own flat-screen blit for views we display on the
    // wrist overlay — otherwise the page is ALSO painted full-size over the
    // game, because Show()ing a view (which is what makes it render at all on
    // stock) also makes DrawViews paint it. Reproduces the fork's
    // externalConsumer "skip flat composite" behaviour with no fork.
    //
    // DrawSingleTexture takes std::shared_ptr<PrismaView> BY VALUE, so on x64
    // rcx is a POINTER to the caller's shared_ptr; _Ptr sits at its +0x00.
    using DrawSingle_t = void (*)(void*);
    DrawSingle_t gOrigDrawSingle = nullptr;

    void HookedDrawSingleTexture(void* sharedPtrArg)
    {
        if (sharedPtrArg) {
            void* view = *reinterpret_cast<void**>(sharedPtrArg);   // shared_ptr::_Ptr
            if (view && IsClaimedView(*reinterpret_cast<const uint64_t*>(view))) {
                return;   // ours — draw it on the wrist, not across the screen
            }
        }
        gOrigDrawSingle(sharedPtrArg);
    }

    // Read a PrismaUI std::string (MSVC SSO) into a fixed buffer. Kept free of
    // C++ objects so it can be SEH-guarded.
    __declspec(noinline) bool ReadStdStringRaw(const void* base, uint32_t off,
                                               char* dst, size_t dstCap) noexcept
    {
        __try {
            if (!dst || dstCap == 0) return false;   // dstCap-1 would underflow
            const auto* p    = reinterpret_cast<const uint8_t*>(base) + off;
            const size_t len = *reinterpret_cast<const size_t*>(p + 16);
            const size_t cap = *reinterpret_cast<const size_t*>(p + 24);
            if (len > 0x4000 || cap < len) return false;
            const char* src = (cap < 16) ? reinterpret_cast<const char*>(p)
                                         : *reinterpret_cast<const char* const*>(p);
            if (!src) return false;
            const size_t n = (len < dstCap - 1) ? len : (dstCap - 1);
            std::memcpy(dst, src, n);
            dst[n] = '\0';
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // Walk PrismaUI's view map under its SRWLOCK. Collects {id, PrismaView*}
    // only — string decoding happens after the lock is released so we hold it
    // for as little time as possible and never allocate underneath it.
    // The URL is decoded HERE, while the shared lock is still held. PrismaUI's
    // ViewManager::Destroy takes the same mutex exclusively before it can free
    // a view, so holding it shared makes every PrismaView* we touch guaranteed
    // alive. Decoding after release would be a use-after-free window.
    __declspec(noinline) size_t CollectViewNodesRaw(uintptr_t mapAddr, uintptr_t lockAddr,
                                                    uint64_t* outIds, char* outUrls,
                                                    size_t urlStride, uint32_t offUrl,
                                                    size_t maxOut) noexcept
    {
        size_t n = 0;
        ::AcquireSRWLockShared(reinterpret_cast<PSRWLOCK>(lockAddr));
        __try {
            const auto head = *reinterpret_cast<uint8_t* const*>(mapAddr);
            if (head) {
                // Iterative in-order traversal (no recursion, bounded work).
                uint8_t* stack[64];
                int      sp   = 0;
                uint8_t* cur  = *reinterpret_cast<uint8_t**>(head + kNodeParent);  // root
                auto isNil = [](uint8_t* p) { return !p || *(p + kNodeIsNil) != 0; };
                while ((!isNil(cur) || sp > 0) && n < maxOut) {
                    while (!isNil(cur) && sp < 64) {
                        stack[sp++] = cur;
                        cur = *reinterpret_cast<uint8_t**>(cur + kNodeLeft);
                    }
                    if (sp == 0) break;
                    uint8_t* node = stack[--sp];
                    outIds[n] = *reinterpret_cast<uint64_t*>(node + kNodeKey);
                    void* view = *reinterpret_cast<void**>(node + kNodeSharedPtr);
                    char* dst  = outUrls + n * urlStride;
                    dst[0] = '\0';
                    if (view) ReadStdStringRaw(view, offUrl, dst, urlStride);
                    ++n;
                    cur = *reinterpret_cast<uint8_t**>(node + kNodeRight);
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            n = 0;
        }
        ::ReleaseSRWLockShared(reinterpret_cast<PSRWLOCK>(lockAddr));
        return n;
    }

    // Stock-mode replacement for PrismaVR_GetViewCount/GetViewInfo.
    std::vector<ViewEntry> EnumerateViewsStock()
    {
        std::vector<ViewEntry> out;
        if (!gStockBuild || !gStockBase) return out;

        constexpr size_t kMaxViews  = 48;
        constexpr size_t kUrlStride = 256;
        uint64_t ids[kMaxViews]{};
        static thread_local char urls[kMaxViews * kUrlStride];   // 12 KB, off the stack
        const size_t n = CollectViewNodesRaw(gStockBase + gStockBuild->rvaViews,
                                             gStockBase + gStockBuild->rvaViewsMutex,
                                             ids, urls, kUrlStride,
                                             gStockBuild->offOriginalUrl, kMaxViews);
        out.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            const char* u = urls + i * kUrlStride;
            if (ids[i] && u[0]) out.push_back({ ids[i], std::string(u) });
        }
        return out;
    }

    // ---- PrismaUI 1.5+ native-VR quad suppression ------------------------
    // 1.5 grew its own VR display path (designed for OpenComposite): every
    // SHOWN view gets a world-locked floating quad in front of the player,
    // keyed "prisma_vr_<viewId>_<n>". It runs on SteamVR native too — where
    // its laser INPUT cannot work (needs OC aim-pose data) — so the quads are
    // uninteractable clutter duplicating what the wrist panel shows. Suppress
    // them with the PUBLIC OpenVR API only: find the quad by key, HideOverlay.
    //
    // Their SyncOverlays DESTROYS a quad when its view hides and RECREATES it
    // (with a fresh key counter) when the view is shown again, but never
    // re-Shows an existing overlay — so a Hide sticks until a recreation.
    // Recreations happen on OUR OWN panel-open flow (binding force-Shows a
    // view, sister mods Show sub-views), so this runs EVERY pump tick
    // (~100 ms): a newborn quad is caught within a tick or two instead of
    // floating for seconds. Two things keep the per-tick cost tiny:
    //   * only SHOWN views are hunted (quads exist only for those; IsHidden
    //     is one cheap in-process API call per view), and
    //   * their key counter is a global that ONLY INCREMENTS, so a fresh
    //     hunt starts at the highest counter seen so far (gQuadCounterFloor)
    //     — a recreated quad is found in the first probe burst.
    struct QuadState { bool found = false; int huntOffset = 0; char key[64] = {}; };
    std::unordered_map<uint64_t, QuadState> gQuadStates;  // pump thread only
    std::vector<ViewEntry> gQuadViewCache;   // last discovery snapshot (pump thread)
    int      gQuadCounterFloor = 0;
    bool     gBeamHidden[2]    = { false, false };
    uint64_t gQuadTick         = 0;

    void HideNativePrismaQuads()
    {
        if (!gStockBuild || !gStockBuild->hasNativeVROverlays) return;
        if (gQuadViewCache.empty()) return;
        auto* ov = vr::VROverlay();
        if (!ov) return;
        ++gQuadTick;
        constexpr int kCounterSpace  = 512;  // sweep wraps the whole space over time
        constexpr int kProbesPerTick = 16;
        const bool verifyTick = (gQuadTick % 5) == 0;  // found-key re-check ~2/s

        for (const auto& v : gQuadViewCache) {
            auto& st = gQuadStates[v.id];

            // Hidden view => their sync destroyed (or never made) its quad.
            if (gPrismaUI && gPrismaUI->IsHidden(v.id)) {
                st.found = false;
                st.huntOffset = 0;
                continue;
            }

            if (st.found) {
                if (!verifyTick) continue;
                vr::VROverlayHandle_t h = vr::k_ulOverlayHandleInvalid;
                if (ov->FindOverlay(st.key, &h) == vr::VROverlayError_None &&
                    h != vr::k_ulOverlayHandleInvalid) {
                    ov->HideOverlay(h);       // idempotent re-assert
                    continue;
                }
                st.found = false;             // destroyed; recreation lands at >= floor
                st.huntOffset = 0;
            }

            for (int i = 0; i < kProbesPerTick; ++i) {
                const int n = (gQuadCounterFloor + st.huntOffset + i) % kCounterSpace;
                char key[64];
                std::snprintf(key, sizeof(key), "prisma_vr_%llu_%d",
                              static_cast<unsigned long long>(v.id), n);
                vr::VROverlayHandle_t h = vr::k_ulOverlayHandleInvalid;
                if (ov->FindOverlay(key, &h) == vr::VROverlayError_None &&
                    h != vr::k_ulOverlayHandleInvalid) {
                    ov->HideOverlay(h);
                    std::memcpy(st.key, key, sizeof(st.key));
                    st.found = true;
                    st.huntOffset = 0;
                    if (n + 1 > gQuadCounterFloor) gQuadCounterFloor = n + 1;
                    SKSE::log::info("Hid PrismaUI's own floating VR quad '{}' — the wrist "
                                    "panel is the presentation on SteamVR native.", key);
                    break;
                }
            }
            if (!st.found) st.huntOffset = (st.huntOffset + kProbesPerTick) % kCounterSpace;
        }

        // Their laser-beam overlays (fixed keys). Uninteractable on native.
        static const char* kBeamKeys[2] = { "prisma_laser_beam_L", "prisma_laser_beam_R" };
        for (int i = 0; i < 2; ++i) {
            if (gBeamHidden[i]) continue;
            vr::VROverlayHandle_t h = vr::k_ulOverlayHandleInvalid;
            if (ov->FindOverlay(kBeamKeys[i], &h) == vr::VROverlayError_None &&
                h != vr::k_ulOverlayHandleInvalid) {
                ov->HideOverlay(h);
                gBeamHidden[i] = true;
                SKSE::log::info("Hid PrismaUI's native laser beam '{}'", kBeamKeys[i]);
            }
        }
    }

    // Bring up addon mode against a stock PrismaUI: identify the build, verify
    // the call site, install the pixel hook. FAILS CLOSED — any mismatch and we
    // install nothing and leave PrismaUI byte-identical, so every other
    // PrismaUI mod in the load order keeps working normally.
    // Defined further down (with the other MCM-settings readers); needed here to
    // pick the view resolution before any view is created.
    int ReadIniIntDirect(const char* section, const char* key, int def);

    bool InitStockPrismaMode()
    {
        HMODULE mod = ::GetModuleHandleA("PrismaUI.dll");
        if (!mod) { SKSE::log::warn("addon mode: PrismaUI.dll not loaded"); return false; }

        auto* verData = reinterpret_cast<const uint8_t*>(::GetProcAddress(mod, "SKSEPlugin_Version"));
        if (!verData) { SKSE::log::warn("addon mode: no SKSEPlugin_Version export"); return false; }
        uint32_t ver = 0;
        std::memcpy(&ver, verData + 4, sizeof(ver));   // PluginVersionData::pluginVersion

        const PrismaBuild* build = nullptr;
        for (const auto& b : kPrismaBuilds) if (b.version == ver) { build = &b; break; }
        if (!build) {
            SKSE::log::error("addon mode: PrismaUI version {}.{}.{}.{} (raw {:#010x}) is NOT a known "
                "build — installing NOTHING. Regenerate offsets with gen_offsets.py and add a row.",
                (ver >> 24) & 0xFF, (ver >> 16) & 0xFF, (ver >> 4) & 0xFFF, ver & 0xF, ver);
            return false;
        }

        const uintptr_t base     = reinterpret_cast<uintptr_t>(mod);
        const uintptr_t callee   = base + build->rvaCopyPixels;
        const uintptr_t callSite = base + build->rvaCopyPixelsCall;
        SKSE::log::info("addon mode: PrismaUI {} at base {:#x}", build->label, base);

        // Self-validating: the site must be an E8 whose rel32 resolves to the
        // expected callee. Verifies opcode, both RVAs and the base at once.
        const auto* code = reinterpret_cast<const uint8_t*>(callSite);
        if (code[0] != 0xE8) {
            SKSE::log::error("addon mode: call site {:#x} is not an E8 (found {:02X}) — aborting.",
                             callSite, code[0]);
            return false;
        }
        int32_t rel = 0;
        std::memcpy(&rel, code + 1, sizeof(rel));
        const uintptr_t resolved = callSite + 5 + static_cast<intptr_t>(rel);
        if (resolved != callee) {
            SKSE::log::error("addon mode: call site resolves to {:#x}, expected {:#x} — aborting. "
                             "(If it points far outside the DLL, another plugin has already hooked "
                             "this call — disable it; two plugins cannot patch one instruction.)",
                             resolved, callee);
            return false;
        }

        // Second hook: the flat-draw suppressor. Validate it the same way.
        const uintptr_t drawCallee   = base + build->rvaDrawSingle;
        const uintptr_t drawCallSite = base + build->rvaDrawSingleCall;
        bool drawHookOk = false;
        {
            const auto* dcode = reinterpret_cast<const uint8_t*>(drawCallSite);
            if (dcode[0] == 0xE8) {
                int32_t drel = 0;
                std::memcpy(&drel, dcode + 1, sizeof(drel));
                drawHookOk = (drawCallSite + 5 + static_cast<intptr_t>(drel)) == drawCallee;
            }
            if (!drawHookOk) {
                SKSE::log::warn("addon mode: DrawSingleTexture call site {:#x} did not validate — "
                                "flat-screen suppression DISABLED (the page will also show on your "
                                "monitor). Pixel capture is unaffected.", drawCallSite);
            }
        }

        // One trampoline for BOTH hooks: set_trampoline() RELEASES any previous
        // buffer, so it must be called exactly once. Each write_call<5> needs a
        // 14-byte veneer; 128 bytes is ample for two.
        void* mem = AllocNearAddress(callSite, 128);
        if (!mem) { SKSE::log::error("addon mode: no trampoline memory in reach — aborting."); return false; }
        const auto disp = static_cast<intptr_t>(reinterpret_cast<uintptr_t>(mem)) -
                          static_cast<intptr_t>(callSite + 5);
        if (disp < (std::numeric_limits<int32_t>::min)() || disp > (std::numeric_limits<int32_t>::max)()) {
            SKSE::log::error("addon mode: trampoline displacement {} out of rel32 range — aborting.", disp);
            ::VirtualFree(mem, 0, MEM_RELEASE);
            return false;
        }

        // Third hook: force the view creation size (fixes the square panel).
        const uintptr_t ssCallee   = base + build->rvaScreenSize;
        const uintptr_t ssCallSite = base + build->rvaScreenSizeCall;
        bool ssHookOk = false;
        {
            const auto* sc = reinterpret_cast<const uint8_t*>(ssCallSite);
            if (sc[0] == 0xE8) {
                int32_t srel = 0;
                std::memcpy(&srel, sc + 1, sizeof(srel));
                ssHookOk = (ssCallSite + 5 + static_cast<intptr_t>(srel)) == ssCallee;
            }
            if (!ssHookOk) {
                SKSE::log::warn("addon mode: GetScreenSize call site {:#x} did not validate — view "
                                "resolution override DISABLED (panels will use PrismaUI's own size, "
                                "which is square in VR).", ssCallSite);
            }
        }

        // Pick the view resolution from the MCM preset. This must be settled
        // BEFORE the capture buffers are sized, and before any view is created.
        {
            int presetIdx = ReadIniIntDirect("Display", "iResolution", 0);
            if (presetIdx < 0) presetIdx = 0;
            if (presetIdx >= static_cast<int>(std::size(kResPresets)))
                presetIdx = static_cast<int>(std::size(kResPresets)) - 1;
            const ResPreset& r = kResPresets[presetIdx];
            if (ssHookOk) {
                gForcedViewW.store(r.w, std::memory_order_relaxed);
                gForcedViewH.store(r.h, std::memory_order_relaxed);
                SKSE::log::info("addon mode: forcing view size {}x{} ({}) — applies to views created "
                                "from now on; change the MCM Resolution and relaunch to alter it.",
                                r.w, r.h, r.name);
            }
        }

        // Size the capture buffers ONCE, before the hook can ever fire, large
        // enough for any frame PrismaUI can hand us. Stock creates views at the
        // game's screen size and addon mode cannot resize them (that needs the
        // fork's ResizeView), so 1080p is not a safe assumption. They are then
        // IMMUTABLE for the process lifetime, which is what makes the hook's
        // `size() >= need` check race-free — growing a std::vector while the
        // render thread is inside memcpy on it is a use-after-free.
        // An unexpectedly larger frame degrades to a dropped frame, never a crash.
        {
            // Big enough for whichever is larger: the desktop (PrismaUI's own
            // size if our override is off) or the forced view resolution.
            uint32_t sw = static_cast<uint32_t>(::GetSystemMetrics(SM_CXSCREEN));
            uint32_t sh = static_cast<uint32_t>(::GetSystemMetrics(SM_CYSCREEN));
            const uint32_t fw = gForcedViewW.load(std::memory_order_relaxed);
            const uint32_t fh = gForcedViewH.load(std::memory_order_relaxed);
            if (fw > sw) sw = fw;
            if (fh > sh) sh = fh;
            const uint32_t capW = (sw > 3840u) ? 3840u : ((sw < 1920u) ? 1920u : sw);
            const uint32_t capH = (sh > 2160u) ? 2160u : ((sh < 1080u) ? 1080u : sh);
            const size_t   initial = static_cast<size_t>(capW) * capH * 4 + (64u << 10);  // slack for row padding
            gCapBuf[0].assign(initial, 0);
            gCapBuf[1].assign(initial, 0);
            SKSE::log::info("addon mode: capture buffers sized {}x{} -> {} bytes each (screen {}x{})",
                            capW, capH, initial, sw, sh);
        }

        // Chain target comes from the ALREADY-VERIFIED callee and is set BEFORE
        // the call site is patched. Never null it afterwards: once the site is
        // patched the hook WILL run, and a null chain target is a call to
        // address 0 on the render thread — which would take down every PrismaUI
        // mod in the load order, the exact opposite of failing closed.
        gOrigCopyPixels = reinterpret_cast<CopyPixels_t>(callee);
        gOrigDrawSingle = reinterpret_cast<DrawSingle_t>(drawCallee);
        try {
            auto& tramp = SKSE::GetTrampoline();
            tramp.set_trampoline(mem, 128, [](void* p, std::size_t) { ::VirtualFree(p, 0, MEM_RELEASE); });
            const auto orig = tramp.write_call<5>(callSite,
                                  reinterpret_cast<uintptr_t>(&HookedCopyPixelsToTexture));
            if (orig != callee) {
                // Site is already patched — leave the chain intact and simply
                // refuse to CAPTURE (gStockBuild stays null => synthetic panel).
                SKSE::log::error("addon mode: write_call returned {:#x}, expected {:#x} — capture "
                                 "DISABLED (hook still chains through correctly).", orig, callee);
                return false;
            }
            if (drawHookOk) {
                const auto dorig = tramp.write_call<5>(drawCallSite,
                                       reinterpret_cast<uintptr_t>(&HookedDrawSingleTexture));
                if (dorig != drawCallee) {
                    SKSE::log::error("addon mode: draw-hook write_call returned {:#x}, expected {:#x} "
                                     "— flat-screen suppression may misbehave.", dorig, drawCallee);
                } else {
                    SKSE::log::info("addon mode: flat-draw suppression installed at {:#x}", drawCallSite);
                }
            }
            if (ssHookOk) {
                gOrigGetScreenSize = reinterpret_cast<GetScreenSize_t>(ssCallee);
                const auto sorig = tramp.write_call<5>(ssCallSite,
                                       reinterpret_cast<uintptr_t>(&HookedGetScreenSize));
                if (sorig != ssCallee) {
                    SKSE::log::error("addon mode: screen-size hook returned {:#x}, expected {:#x} — "
                                     "disabling the override.", sorig, ssCallee);
                    gForcedViewW.store(0, std::memory_order_relaxed);
                    gForcedViewH.store(0, std::memory_order_relaxed);
                } else {
                    SKSE::log::info("addon mode: view-size override installed at {:#x}", ssCallSite);
                }
            }
        } catch (const std::exception& e) {
            // The veneer is allocated before the patch is written, so nothing
            // was modified on this path — safe to drop the chain target.
            SKSE::log::error("addon mode: hook install failed: {} — PrismaUI untouched.", e.what());
            gOrigCopyPixels = nullptr;
            return false;
        }

        gStockBuild = build;
        gStockBase  = base;
        SKSE::log::info("addon mode ACTIVE: pixel hook installed at call site {:#x} "
                        "(callee {:#x}); view map at {:#x}. Wrist panel will show real PrismaUI "
                        "content from a STOCK PrismaUI.", callSite, callee, base + build->rvaViews);
        return true;
    }

    std::vector<ViewEntry> EnumerateViews()
    {
        std::vector<ViewEntry> out;
        // Addon mode: no PrismaVR_* API, read PrismaUI's own view map instead.
        if (!gGetCount || !gGetViewInfo) return EnumerateViewsStock();

        const uint32_t count = gGetCount();
        out.reserve(count);
        char pathBuf[1024];
        for (uint32_t i = 0; i < count; ++i) {
            uint64_t id = 0;
            pathBuf[0] = '\0';
            if (gGetViewInfo(i, &id, pathBuf, sizeof(pathBuf)) && id != 0) {
                out.push_back({ id, std::string(pathBuf) });
            }
        }
        return out;
    }

    // ======================================================================
    // SKSE MENU FRAMEWORK (SMF) AS A NATIVE PANEL SOURCE  (v1.4.0)
    //
    // No ImGui VR Helper involved. SMF's own frame function (?Render@@YAXXZ,
    // driven from the game's render thread) is re-pointed at five E8 call
    // sites (doc 08: call sites, never prologues; validate; fail closed):
    //
    //   siteNewFrame    ImGui::NewFrame     -> inject wrist pointer/wheel/chars
    //                                          via SMF's exported cimgui event
    //                                          API, clear ImGuiConfigFlags_NoMouse
    //                                          (SMF sets it when "unpaused"), and
    //                                          override io.DisplaySize to the
    //                                          panel resolution. Runs AFTER the
    //                                          Win32 backend queued the OS cursor
    //                                          and after SMF wrote DisplaySize, so
    //                                          ours wins on both.
    //   siteFlatDraw    ImGui_ImplDX11_RenderDrawData  (helper NOT connected)
    //   siteHelperDraw  Client::RenderToPanel          (helper connected)
    //                   -> while OUR session is active, both redirect the draw
    //                      into a game-device MISC_SHARED texture the wrist
    //                      overlay reads live; otherwise pure passthrough, so
    //                      F1 keeps behaving exactly as stock (coexistence).
    //   siteFocusSyncA/B  the two WindowManager::IsAnyWindowOpen calls in the
    //                      helper focus-sync block -> return false during our
    //                      session so the helper neither steals the window
    //                      (RequestFocus -> duplicate panel) nor closes it
    //                      (focus mismatch -> WindowManager::Close every frame).
    //                      The third IsAnyWindowOpen call (the render gate) is
    //                      deliberately NOT hooked - it must see "open".
    //
    // Pause: never hooked at all. WindowManager::ShouldTheGameBePaused() is
    // "any window open with BlockUserInput set", and both flags are PUBLIC
    // atomics on the exported WindowInterface. Our session opens the main
    // window with BlockUserInput=false, so all four pause consumers (game
    // lock, input swallow, ImGui input enable, blur) see "not paused" and the
    // game keeps running - the wrist menu stays dynamic by construction.
    // F1 sessions leave the flag alone and keep SMF's ini behaviour.
    //
    // Threading: render-thread code (the detours) touches only this module's
    // atomics, SMF exports, and the game's immediate context FROM the thread
    // that owns it. The pump touches only device-object creation (free-
    // threaded per D3D11 spec) and our OWN device. The v1.3.x helper-mirror
    // deadlock (device critical section held across a compositor round-trip)
    // is structurally impossible here: nothing crosses threads.
    // ======================================================================
    // NOTE (V2.0.0): the vendored ImGuiVRHelper* client SDK was REMOVED -- the
    // native path below needs none of it (our own typedefs describe the two
    // hooked helper functions), so carrying it only added an LGPL attribution
    // obligation and a nlohmann_json dependency. Helper COEXISTENCE is handled
    // entirely by the siteHelperDraw / siteFocusSync hooks, not by its API.
    namespace smf {
        constexpr const char* kSourceName = "SKSE Menu Framework";

        // ---- known builds (generated offline by gen_smf_offsets.py) ------
        struct SmfBuild {
            uint32_t    version;             // SKSEPlugin_Version pluginVersion
            const char* label;
            uintptr_t   rvaRenderDrawData;   // ImGui_ImplDX11_RenderDrawData (callee)
            uintptr_t   rvaNewFrame;         // ImGui::NewFrame (callee)
            uintptr_t   rvaGetDrawData;      // ImGui::GetDrawData
            uintptr_t   rvaIsAnyWindowOpen;  // WindowManager::IsAnyWindowOpen
            uintptr_t   rvaRenderToPanel;    // helper Client::RenderToPanel (callee)
            uintptr_t   siteNewFrame;        // E8 sites inside ?Render@@YAXXZ:
            uintptr_t   siteFlatDraw;
            uintptr_t   siteHelperDraw;
            uintptr_t   siteFocusSyncA;      // IsAnyWindowOpen @ Render+0x5D
            uintptr_t   siteFocusSyncB;      // IsAnyWindowOpen @ Render+0xC1
            uint32_t    offMouseDrawCursor;  // ImGuiIO::MouseDrawCursor (PDB LF_MEMBER)
        };
        constexpr SmfBuild kSmfBuilds[] = {
            // 3.13.0 - sha256 B0FAB9D77BB522D1..., ImGui 1.90.8, 52 MB PDB shipped
            { 0x030D0000, "3.13.0",
              0x00019C0, 0x0011DD0, 0x0010EA0, 0x0140E40, 0x0118470,
              0x0119DB0, 0x0119E3F, 0x0119E30, 0x0119CBD, 0x0119D21, 0x58 },
        };

        // ---- SMF's exported surface (SDK-sanctioned; layouts per its own
        // header: ImGuiIO ConfigFlags at +0, DisplaySize at +8; WindowInterface
        // is two std::atomic<bool>, IsOpen then BlockUserInput). Gated on the
        // exact build above, so the 1.90.8 layout assumption is version-safe.
        struct NativeIO { int ConfigFlags; int BackendFlags; float DisplaySizeX; float DisplaySizeY; };
        struct WindowIface { std::atomic<bool> IsOpen; std::atomic<bool> BlockUserInput; };
        using GetIO_t         = NativeIO* (*)();
        using GetMainWin_t    = WindowIface* (*)();
        using AddMousePos_t   = void (*)(NativeIO*, float, float);
        using AddMouseBtn_t   = void (*)(NativeIO*, int, bool);
        using AddWheel_t      = void (*)(NativeIO*, float, float);
        using AddChar_t       = void (*)(NativeIO*, unsigned int);
        using RenderDD_t      = void (*)(void*);
        using GetDrawData_t   = void* (*)();
        using NewFrame_t      = void (*)();
        using IsAnyOpen_t     = bool (*)();
        using RenderToPanel_t = void (*)(void*, ID3D11DeviceContext*);

        uintptr_t        gBase   = 0;
        const SmfBuild*  gBuild  = nullptr;
        bool             gTried  = false;
        bool             gHooked = false;
        GetIO_t          gGetIO        = nullptr;
        GetMainWin_t     gGetMainWin   = nullptr;
        AddMousePos_t    gAddMousePos  = nullptr;
        AddMouseBtn_t    gAddMouseBtn  = nullptr;
        AddWheel_t       gAddWheel     = nullptr;
        AddChar_t        gAddChar      = nullptr;
        RenderDD_t       gRenderDrawData  = nullptr;   // callee, called directly by our redirect
        GetDrawData_t    gGetDrawData     = nullptr;
        IsAnyOpen_t      gIsAnyWindowOpen = nullptr;   // real function (sites are hooked, callee is not)
        NewFrame_t       gOrigNewFrame      = nullptr; // originals returned by write_call
        RenderDD_t       gOrigFlatDraw      = nullptr;
        RenderToPanel_t  gOrigRenderToPanel = nullptr;

        // ---- pump -> render-thread mailbox (atomics only; no locks) ------
        std::atomic<bool>     gSession{ false };
        std::atomic<uint32_t> gMousePacked{ 0xFFFFFFFFu };  // x<<16|y in panel px; sentinel = off-panel
        std::atomic<bool>     gMouseDown{ false };
        // Scroll is a HELD DEFLECTION (stick * 1000), not an accumulator.
        // v1.4.0 dumped one big wheel delta on the single frame following each
        // ~10 Hz pump tick; ImGui applies a wheel event on exactly one frame and
        // only if a scrollable window is hovered THAT frame, so a mistimed dump
        // is silently discarded. Emitting a small delta EVERY frame instead is
        // both robust to that race and smoother (90 Hz vs 10 Hz granularity).
        std::atomic<int>      gScrollStick{ 0 };            // stick deflection * 1000
        std::atomic<uint32_t> gCharQ{ 0 };                  // single-slot char queue (0 = empty)
        bool gPrevDownRT    = false;   // render-thread-only edge state
        bool gPrevOnPanelRT = false;
        bool gCursorForcedRT = false;  // we turned MouseDrawCursor on; restore when the session ends
        bool gSavedBlockInput = true;  // restored on session end
        // v1.4.7: SMF lays out in its OWN space (MEASURED: 1024x1024, square).
        // Forcing 1920x1080 made ImGui compose in 16:9 while SMF sized and
        // placed its window for 1024x1024, so the menu landed in a sub-rect
        // whose relationship to the panel is not the identity our pointer
        // injection assumes -- it agreed only near centre. We now ADOPT SMF's
        // space: the render thread publishes it, the pump sizes the RT to it,
        // and pointer pixels are computed against the same numbers.
        std::atomic<uint32_t> gWantW{ 0 }, gWantH{ 0 };   // what SMF asked for
        uint32_t gRtW = 0, gRtH = 0;                      // what the RT currently is

        // ---- render target: game-device texture, MISC_SHARED -------------
        // SMF's DX11 backend paints into this on the render thread; SteamVR
        // reads it live through the DXGI share opened on OUR device (pump).
        ID3D11Texture2D*        gTex = nullptr;
        ID3D11RenderTargetView* gRTV = nullptr;
        HANDLE                  gShareHandle = nullptr;
        ID3D11DeviceContext*    gGameCtx = nullptr;   // AddRef'd once, kept for the plugin lifetime

        // ================== render-thread code ============================
        // Discipline (doc 08): no allocation, no logging, no locking. ImGui's
        // own internals may allocate - that is the same single ImGui thread
        // SMF always runs, not a cross-thread hazard of ours.
        void RenderIntoWrist(void* dd)
        {
            if (!dd || !gRTV || !gGameCtx || !gRenderDrawData) return;
            ID3D11RenderTargetView* oldRTV = nullptr;
            ID3D11DepthStencilView* oldDSV = nullptr;
            gGameCtx->OMGetRenderTargets(1, &oldRTV, &oldDSV);
            UINT nvp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
            D3D11_VIEWPORT vps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
            gGameCtx->RSGetViewports(&nvp, vps);
            const float clear[4] = { 0.f, 0.f, 0.f, 0.f };
            gGameCtx->OMSetRenderTargets(1, &gRTV, nullptr);
            gGameCtx->ClearRenderTargetView(gRTV, clear);
            gRenderDrawData(dd);   // the backend sets its own viewport from DisplaySize
            gGameCtx->OMSetRenderTargets(1, &oldRTV, oldDSV);
            if (nvp) gGameCtx->RSSetViewports(nvp, vps);
            if (oldRTV) oldRTV->Release();
            if (oldDSV) oldDSV->Release();
        }
        void HookedFlatDraw(void* dd)
        {
            if (gSession.load(std::memory_order_relaxed)) RenderIntoWrist(dd);
            else if (gOrigFlatDraw) gOrigFlatDraw(dd);
        }
        void HookedRenderToPanel(void* self, ID3D11DeviceContext* ctx)
        {
            if (!gSession.load(std::memory_order_relaxed)) {
                if (gOrigRenderToPanel) gOrigRenderToPanel(self, ctx);
                return;
            }
            RenderIntoWrist(gGetDrawData ? gGetDrawData() : nullptr);
        }
        bool HookedIsAnyOpenFocusSync()
        {
            // Only the two helper focus-sync sites land here. Lying "closed"
            // during our session keeps the helper's panel out of the scene and
            // stops its focus mismatch from closing our window every frame.
            if (gSession.load(std::memory_order_relaxed)) return false;
            return gIsAnyWindowOpen ? gIsAnyWindowOpen() : false;
        }
        void HookedNewFrame()
        {
            if (gSession.load(std::memory_order_relaxed) && gGetIO) {
                NativeIO* io = gGetIO();
                // Report what SMF/the Win32 backend set BEFORE we override it.
                // If this is not 1920x1080, our forced DisplaySize changes the
                // aspect ImGui lays out at -- the prime suspect for a pointer
                // that diverges from the laser away from panel centre.
                // Publish SMF's own layout space; the pump sizes the RT to it.
                // We deliberately do NOT overwrite DisplaySize any more.
                const uint32_t dw = static_cast<uint32_t>(io->DisplaySizeX);
                const uint32_t dh = static_cast<uint32_t>(io->DisplaySizeY);
                if (dw && dh &&
                    (dw != gWantW.load(std::memory_order_relaxed) ||
                     dh != gWantH.load(std::memory_order_relaxed))) {
                    gWantW.store(dw, std::memory_order_relaxed);
                    gWantH.store(dh, std::memory_order_relaxed);
                    SKSE::log::info("SMF native: layout space is {}x{} - sizing the wrist RT to match", dw, dh);
                }
                io->ConfigFlags &= ~(1 << 4);   // ImGuiConfigFlags_NoMouse - DisableImGuiInput set it this frame
                // ImGui's own software cursor: the wrist panel has no OS cursor and
                // the composite dot is PrismaUI-path-only. This arrow is drawn by the
                // same system consuming our injected coords - a live mapping check.
                reinterpret_cast<bool*>(io)[gBuild->offMouseDrawCursor] = true;
                gCursorForcedRT = true;
                const uint32_t m  = gMousePacked.load(std::memory_order_relaxed);
                const bool     on = (m != 0xFFFFFFFFu);
                if (on)                   gAddMousePos(io, float(m >> 16), float(m & 0xFFFFu));
                else if (gPrevOnPanelRT)  gAddMousePos(io, -FLT_MAX, -FLT_MAX);
                gPrevOnPanelRT = on;
                const bool dn = gMouseDown.load(std::memory_order_relaxed);
                if (dn != gPrevDownRT) { gAddMouseBtn(io, 0, dn); gPrevDownRT = dn; }
                const int sk = gScrollStick.load(std::memory_order_relaxed);
                if (sk) {
                    // Sign matches the PrismaUI path: stick UP scrolls the page DOWN.
                    // 1000 (full stick) -> 0.1 notches/frame ~= 9 notches/s at 90 Hz.
                    gAddWheel(io, 0.0f, float(sk) * -0.0001f);
                }
                const uint32_t c = gCharQ.exchange(0, std::memory_order_relaxed);
                if (c) gAddChar(io, c);
            } else if (gCursorForcedRT && gGetIO && gBuild) {
                reinterpret_cast<bool*>(gGetIO())[gBuild->offMouseDrawCursor] = false;
                gCursorForcedRT = false;   // F1/helper sessions keep their stock look
            }
            if (gOrigNewFrame) gOrigNewFrame();
        }

        // ================== install (pump / lifecycle thread) =============
        bool ValidSite(uintptr_t site, uintptr_t callee)
        {
            const auto* c = reinterpret_cast<const uint8_t*>(site);
            if (c[0] != 0xE8) return false;
            int32_t rel = 0;
            std::memcpy(&rel, c + 1, sizeof(rel));
            return site + 5 + static_cast<intptr_t>(rel) == callee;
        }
        bool EnsureNative()
        {
            if (gHooked) return true;
            HMODULE mod = ::GetModuleHandleW(L"SKSEMenuFramework");
            if (!mod) return false;          // not installed (or not yet mapped) - cheap retry
            if (gTried) return false;        // installed but we already failed - permanent for this session
            gTried = true;
            gBase = reinterpret_cast<uintptr_t>(mod);

            uint32_t ver = 0;
            if (const auto* pv = reinterpret_cast<const uint8_t*>(::GetProcAddress(mod, "SKSEPlugin_Version")))
                std::memcpy(&ver, pv + 4, sizeof(ver));
            for (const auto& b : kSmfBuilds)
                if (b.version == ver) { gBuild = &b; break; }
            if (!gBuild) {
                SKSE::log::warn("SMF native: unknown SKSEMenuFramework version {:#010x} - source disabled "
                                "(known: 3.13.0). Regenerate offsets with gen_smf_offsets.py.", ver);
                return false;
            }

            gGetIO       = reinterpret_cast<GetIO_t>(::GetProcAddress(mod, "igGetIO"));
            gGetMainWin  = reinterpret_cast<GetMainWin_t>(::GetProcAddress(mod, "GetMainWindow"));
            gAddMousePos = reinterpret_cast<AddMousePos_t>(::GetProcAddress(mod, "ImGuiIO_AddMousePosEvent"));
            gAddMouseBtn = reinterpret_cast<AddMouseBtn_t>(::GetProcAddress(mod, "ImGuiIO_AddMouseButtonEvent"));
            gAddWheel    = reinterpret_cast<AddWheel_t>(::GetProcAddress(mod, "ImGuiIO_AddMouseWheelEvent"));
            gAddChar     = reinterpret_cast<AddChar_t>(::GetProcAddress(mod, "ImGuiIO_AddInputCharacter"));
            if (!gGetIO || !gGetMainWin || !gAddMousePos || !gAddMouseBtn || !gAddWheel || !gAddChar) {
                SKSE::log::error("SMF native: required exports missing - source disabled.");
                return false;
            }

            const uintptr_t sites[5]   = { gBuild->siteNewFrame, gBuild->siteFlatDraw, gBuild->siteHelperDraw,
                                           gBuild->siteFocusSyncA, gBuild->siteFocusSyncB };
            const uintptr_t callees[5] = { gBuild->rvaNewFrame, gBuild->rvaRenderDrawData, gBuild->rvaRenderToPanel,
                                           gBuild->rvaIsAnyWindowOpen, gBuild->rvaIsAnyWindowOpen };
            for (int i = 0; i < 5; ++i) {
                if (!ValidSite(gBase + sites[i], gBase + callees[i])) {
                    SKSE::log::error("SMF native: call site {} at {:#x} did not validate (another plugin "
                                     "patched it, or a repack) - source disabled, nothing written.",
                                     i, gBase + sites[i]);
                    return false;   // fail closed: zero of the five hooks installed
                }
            }

            auto* dev = reinterpret_cast<::ID3D11Device*>(RE::BSGraphics::Renderer::GetDevice());
            if (!dev) { SKSE::log::warn("SMF native: no game D3D11 device yet - will retry."); gTried = false; return false; }
            dev->GetImmediateContext(&gGameCtx);   // AddRef; device methods are free-threaded, context is used only on the render thread
            // The RT is created lazily by the pump (EnsureRT) once SMF's first
            // frame tells us its layout space - we cannot know it before then.

            // SEPARATE trampoline: the global one was seeded once for the
            // PrismaUI hooks and set_trampoline releases the prior buffer.
            void* mem = AllocNearAddress(gBase + gBuild->siteNewFrame, 256);
            if (!mem) { SKSE::log::error("SMF native: no trampoline memory in rel32 reach - source disabled."); return false; }
            static SKSE::Trampoline sTramp;
            sTramp.set_trampoline(mem, 256, [](void* p, std::size_t) { ::VirtualFree(p, 0, MEM_RELEASE); });

            gIsAnyWindowOpen = reinterpret_cast<IsAnyOpen_t>(gBase + gBuild->rvaIsAnyWindowOpen);
            gGetDrawData     = reinterpret_cast<GetDrawData_t>(gBase + gBuild->rvaGetDrawData);
            gRenderDrawData  = reinterpret_cast<RenderDD_t>(gBase + gBuild->rvaRenderDrawData);
            gOrigNewFrame = reinterpret_cast<NewFrame_t>(
                sTramp.write_call<5>(gBase + gBuild->siteNewFrame, reinterpret_cast<std::uintptr_t>(&HookedNewFrame)));
            gOrigFlatDraw = reinterpret_cast<RenderDD_t>(
                sTramp.write_call<5>(gBase + gBuild->siteFlatDraw, reinterpret_cast<std::uintptr_t>(&HookedFlatDraw)));
            gOrigRenderToPanel = reinterpret_cast<RenderToPanel_t>(
                sTramp.write_call<5>(gBase + gBuild->siteHelperDraw, reinterpret_cast<std::uintptr_t>(&HookedRenderToPanel)));
            sTramp.write_call<5>(gBase + gBuild->siteFocusSyncA, reinterpret_cast<std::uintptr_t>(&HookedIsAnyOpenFocusSync));
            sTramp.write_call<5>(gBase + gBuild->siteFocusSyncB, reinterpret_cast<std::uintptr_t>(&HookedIsAnyOpenFocusSync));

            gHooked = true;
            SKSE::log::info("SMF native: armed on SKSEMenuFramework {} (5 call-site hooks, 1920x1080 shared RT).",
                            gBuild->label);
            return true;
        }

        // ================== pump-side session + input =====================
        // Create/resize the shared render target to SMF's own layout space.
        // Pump thread only. D3D11 device methods are free-threaded, and the
        // render thread simply does not draw while the RT is absent.
        bool EnsureRT(uint32_t w, uint32_t h)
        {
            if (!w || !h) return gRTV != nullptr;
            if (w > 4096) w = 4096;
            if (h > 4096) h = 4096;
            if (gRTV && w == gRtW && h == gRtH) return true;
            auto* dev = reinterpret_cast<::ID3D11Device*>(RE::BSGraphics::Renderer::GetDevice());
            if (!dev) return false;
            ID3D11Texture2D*        tex = nullptr;
            ID3D11RenderTargetView* rtv = nullptr;
            HANDLE                  sh  = nullptr;
            D3D11_TEXTURE2D_DESC td{};
            td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            td.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
            if (FAILED(dev->CreateTexture2D(&td, nullptr, &tex)) ||
                FAILED(dev->CreateRenderTargetView(tex, nullptr, &rtv))) {
                if (rtv) rtv->Release();
                if (tex) tex->Release();
                SKSE::log::error("SMF native: render-target {}x{} creation FAILED.", w, h);
                return false;
            }
            IDXGIResource* res = nullptr;
            if (SUCCEEDED(tex->QueryInterface(__uuidof(IDXGIResource), reinterpret_cast<void**>(&res))) && res) {
                res->GetSharedHandle(&sh);
                res->Release();
            }
            if (!sh) {
                rtv->Release(); tex->Release();
                SKSE::log::error("SMF native: no shared handle for {}x{}.", w, h);
                return false;
            }
            ID3D11RenderTargetView* oldRtv = gRTV;
            ID3D11Texture2D*        oldTex = gTex;
            gRTV = rtv; gTex = tex; gShareHandle = sh; gRtW = w; gRtH = h;
            if (oldRtv) oldRtv->Release();
            if (oldTex) oldTex->Release();
            SKSE::log::info("SMF native: wrist render target now {}x{} (matches SMF layout space).", w, h);
            return true;
        }
        uint32_t WantW()   { return gWantW.load(std::memory_order_relaxed); }
        uint32_t WantH()   { return gWantH.load(std::memory_order_relaxed); }
        uint32_t RtW()     { return gRtW; }
        uint32_t RtH()     { return gRtH; }
        bool     RtReady() { return gRTV != nullptr && gRtW && gRtH; }

        bool Available()     { return gHooked; }
        bool SessionActive() { return gSession.load(std::memory_order_relaxed); }
        inline bool IsSourceName(const std::string& s_) { return _stricmp(s_.c_str(), kSourceName) == 0; }
        HANDLE ShareHandle() { return gShareHandle; }

        void PointerMove(int x, int y)
        {
            if (x < 0) x = 0; if (y < 0) y = 0;
            gMousePacked.store((uint32_t(x & 0xFFFF) << 16) | uint32_t(y & 0xFFFF), std::memory_order_relaxed);
        }
        void PointerOff()          { gMousePacked.store(0xFFFFFFFFu, std::memory_order_relaxed); }
        void PointerButton(bool d) { gMouseDown.store(d, std::memory_order_relaxed); }
        void SetScrollStick(float y)
        {
            const int v = static_cast<int>(y * 1000.0f);
            const int prev = gScrollStick.exchange(v, std::memory_order_relaxed);
            if ((prev == 0) != (v == 0))
                SKSE::log::info("SMF native: scroll stick {}", v ? "engaged" : "released");
        }
        void PushChar(uint32_t c)  { if (c) gCharQ.store(c, std::memory_order_relaxed); }

        bool WindowStillOpen()
        {
            auto* w = gGetMainWin ? gGetMainWin() : nullptr;
            return w && w->IsOpen.load(std::memory_order_relaxed);
        }
        void BeginSession()
        {
            if (!gHooked || gSession.load(std::memory_order_relaxed)) return;
            if (auto* w = gGetMainWin()) {
                gSavedBlockInput = w->BlockUserInput.load(std::memory_order_relaxed);
                w->BlockUserInput.store(false, std::memory_order_relaxed);  // never pause: wrist menu is used live
                w->IsOpen.store(true, std::memory_order_relaxed);           // the SDK-sanctioned open (mods do exactly this)
            }
            gPrevDownRT = false;  // benign: render thread reads these only once gSession flips below
            gPrevOnPanelRT = false;
            gMouseDown.store(false, std::memory_order_relaxed);
            gMousePacked.store(0xFFFFFFFFu, std::memory_order_relaxed);
            gScrollStick.store(0, std::memory_order_relaxed);
            gSession.store(true, std::memory_order_release);
            SKSE::log::info("SMF native: session BEGIN (window opened, pause suppressed for this session)");
        }
        void EndSession()
        {
            if (!gSession.load(std::memory_order_relaxed)) return;
            gSession.store(false, std::memory_order_release);
            if (gGetMainWin) {
                if (auto* w = gGetMainWin()) {
                    w->IsOpen.store(false, std::memory_order_relaxed);
                    w->BlockUserInput.store(gSavedBlockInput, std::memory_order_relaxed);
                }
            }
            gMouseDown.store(false, std::memory_order_relaxed);
            gMousePacked.store(0xFFFFFFFFu, std::memory_order_relaxed);
            gScrollStick.store(0, std::memory_order_relaxed);
            SKSE::log::info("SMF native: session END");
        }
    }

    // ---- MCM setting sources, in MCM Helper's own precedence order --------
    // MCM Helper builds its setting store from INI files ONLY (a ModSetting
    // does not exist until an INI declares it — `defaultValue` in config.json
    // merely defines what "reset to default" restores). It reads:
    //   1. Data/MCM/Config/<mod>/settings.ini  — author defaults, shipped
    //   2. Data/MCM/Settings/<mod>.ini         — user values, written on first change
    // We must read BOTH in that same order, later file winning. Reading only
    // the user file means we see nothing until the player touches the menu —
    // and on a fresh install that file does not exist at all.
    static const char* const kMcmDefaultsIni =
        ".\\Data\\MCM\\Config\\PrismaUIWristOverlays\\settings.ini";
    static const char* const kMcmUserIni =
        ".\\Data\\MCM\\Settings\\PrismaUIWristOverlays.ini";

    // Parse ONE ini file for [section] key. Returns true (and fills `out`) only
    // if the key is present in that file.
    //
    // We deliberately avoid GetPrivateProfile* here: on this load order
    // PrivateProfileRedirector intercepts that API and serves a value cached at
    // startup, so a mid-session MCM write (which MCM Helper makes straight to
    // disk) would be invisible to us. std::ifstream goes through MO2's USVFS
    // (CreateFile) to the mod's real file, so it sees the up-to-date value.
    bool ReadIniFileValue(const char* path, const char* section, const char* key,
                          std::string& out)
    {
        std::ifstream f(path);
        if (!f) return false;

        auto trim = [](std::string& s) {
            const size_t a = s.find_first_not_of(" \t\r\n");
            if (a == std::string::npos) { s.clear(); return; }
            const size_t b = s.find_last_not_of(" \t\r\n");
            s = s.substr(a, b - a + 1);
        };
        std::string line, curSection;
        while (std::getline(f, line)) {
            // Strip a leading UTF-8 BOM (MCM Helper writes the settings INI as
            // UTF-8-with-BOM). Without this the FIRST line — "[Display]" — is
            // read as "\xEF\xBB\xBF[Display]", so that section is never entered
            // and iResolution silently falls back to its default. ([Icons] sits
            // on a later, BOM-free line, which is why icon overrides worked but
            // resolution stayed stuck at preset 0 / 1080p.)
            if (line.size() >= 3 &&
                static_cast<unsigned char>(line[0]) == 0xEF &&
                static_cast<unsigned char>(line[1]) == 0xBB &&
                static_cast<unsigned char>(line[2]) == 0xBF) {
                line.erase(0, 3);
            }
            trim(line);
            if (line.empty() || line[0] == ';' || line[0] == '#') continue;
            if (line.size() >= 2 && line.front() == '[' && line.back() == ']') {
                curSection = line.substr(1, line.size() - 2);
                trim(curSection);
                continue;
            }
            const size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string k = line.substr(0, eq);
            std::string v = line.substr(eq + 1);
            trim(k); trim(v);
            if (_stricmp(curSection.c_str(), section) == 0 && _stricmp(k.c_str(), key) == 0) {
                out = v;
                return true;
            }
        }
        return false;
    }

    // Read a [section] key as a raw string, defaults-then-user (user wins).
    // Used for the per-icon NAME binding (a `menu` control stores the selected
    // overlay name as a string).
    std::string ReadIniStringDirect(const char* section, const char* key, const char* def)
    {
        std::string result = def ? def : "";
        std::string v;
        if (ReadIniFileValue(kMcmDefaultsIni, section, key, v)) result = v;  // author defaults
        if (ReadIniFileValue(kMcmUserIni,     section, key, v)) result = v;  // user overrides
        return result;
    }

    // Read a [section] key as an integer, same two-file precedence. Falls back
    // to GetPrivateProfileInt only if NEITHER file could be opened at all.
    int ReadIniIntDirect(const char* section, const char* key, int def)
    {
        std::string raw;
        bool found = false;
        std::string v;
        if (ReadIniFileValue(kMcmDefaultsIni, section, key, v)) { raw = v; found = true; }
        if (ReadIniFileValue(kMcmUserIni,     section, key, v)) { raw = v; found = true; }

        if (!found) {
            std::ifstream probeDefaults(kMcmDefaultsIni);
            std::ifstream probeUser(kMcmUserIni);
            if (!probeDefaults && !probeUser) {
                return static_cast<int>(::GetPrivateProfileIntA(section, key, def, kMcmUserIni));
            }
            return def;
        }
        try { return std::stoi(raw); } catch (...) { return def; }
    }

    // Derive a readable, stable short-name for an overlay from its view URL:
    //   file:///views/SkyrimNet/dashboard/index.html  ->  "SkyrimNet/dashboard"
    //   file:///views/SeverActions/index.html         ->  "SeverActions"
    // This name is what the MCM stores and what we bind against — it is STABLE
    // (does not shift when other overlays are added/removed), unlike a position
    // index into the sorted list.
    std::string OverlayShortName(const std::string& url)
    {
        std::string s = url;
        const std::string pfx = "file:///views/";
        const size_t p = s.find(pfx);
        if (p != std::string::npos) s = s.substr(p + pfx.size());
        // strip a trailing "/index.htm[l]"
        for (const char* tail : { "/index.html", "/index.htm" }) {
            const size_t q = s.rfind(tail);
            if (q != std::string::npos && q == s.size() - std::string(tail).size()) { s = s.substr(0, q); break; }
        }
        while (!s.empty() && (s.back() == '/' || s.back() == '\\')) s.pop_back();
        return s;
    }

    // Regenerate the MCM config.json each launch so the three icon selectors
    // are `menu` dropdowns whose options are the CURRENT set of overlays (by
    // name) plus "Off" and "Auto". A `menu` control stores the selected option
    // TEXT (the overlay name) as a string, which we then bind against — so the
    // binding is stable across overlay-set changes and new overlay mods appear
    // in the list automatically (on the next launch; MCM Helper reads the file
    // at load). Written through MO2's VFS to the same place the settings INI is
    // read from. Dedup count logged so we only rewrite when the set changes.
    int gLastConfigGenCount = -1;

    // ★ The option list must only ever GROW.
    //
    // PrismaUI views register LAZILY — SeverActions, for one, does not create
    // its views until something needs them. So the live view set a few minutes
    // into a session is a SUBSET of what exists over a full session. An earlier
    // version rebuilt the dropdown purely from that snapshot and overwrote the
    // richer list, silently deleting names (observed 2026-09-06: nine
    // SeverActions entries vanished). That is worse than cosmetic — MCM Helper
    // renders a stored value that is not in its options list as blank, so a
    // user whose icon was bound to a dropped name loses the binding in the UI.
    //
    // Fix: union the freshly-discovered names with whatever the existing
    // config.json already offers. Names accumulate and are never removed.
    std::vector<std::string> ReadExistingConfigOptions()
    {
        std::vector<std::string> out;
        std::ifstream in(".\\Data\\MCM\\Config\\PrismaUIWristOverlays\\config.json",
                         std::ios::binary);
        if (!in) return out;
        std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const std::string key = "\"options\": [";
        const size_t b = all.find(key);
        if (b == std::string::npos) return out;
        const size_t e = all.find(']', b);
        if (e == std::string::npos) return out;
        const std::string arr = all.substr(b + key.size(), e - b - key.size());
        // Pull each "quoted" entry.
        size_t i = 0;
        while (true) {
            const size_t q1 = arr.find('"', i);
            if (q1 == std::string::npos) break;
            const size_t q2 = arr.find('"', q1 + 1);
            if (q2 == std::string::npos) break;
            std::string v = arr.substr(q1 + 1, q2 - q1 - 1);
            // Auto/Off/SMF are re-added unconditionally below; skip them here.
            if (!v.empty() && v != "Auto" && v != "Off" && v != smf::kSourceName) {
                if (std::find(out.begin(), out.end(), v) == out.end()) out.push_back(v);
            }
            i = q2 + 1;
        }
        return out;
    }

    void WriteMcmConfigJson(const std::vector<ViewEntry>& views)
    {
        // Start from what the menu ALREADY offers so nothing is ever dropped.
        std::vector<std::string> names = ReadExistingConfigOptions();
        const size_t carriedOver = names.size();
        for (const auto& v : views) {
            std::string n = OverlayShortName(v.url);
            if (n.empty()) continue;
            if (std::find(names.begin(), names.end(), n) == names.end()) names.push_back(n);
        }
        std::sort(names.begin(), names.end());

        auto esc = [](const std::string& s) {
            std::string o; o.reserve(s.size() + 4);
            for (char c : s) { if (c == '"' || c == '\\') o.push_back('\\'); o.push_back(c); }
            return o;
        };
        std::string opts = "\"Auto\", \"Off\"";
        // The SKSE Menu Framework is a permanent extra source whenever the
        // ImGui VR Helper is present — it is not a PrismaUI view, so it never
        // appears in `views`; it is one entry covering every mod that
        // registers a menu inside SMF.
        if (smf::Available()) { opts += ", \""; opts += esc(smf::kSourceName); opts += "\""; }
        for (const auto& n : names) { opts += ", \""; opts += esc(n); opts += "\""; }

        auto iconCtl = [&](const char* id, const char* text, const char* autoDesc) {
            std::string s;
            s += "        {\n";
            s += std::string("          \"id\": \"") + id + "\",\n";
            s += "          \"type\": \"menu\",\n";
            s += std::string("          \"text\": \"") + text + "\",\n";
            s += std::string("          \"help\": \"Overlay this wrist icon opens. 'Off' hides just this icon. 'Auto' = ") + autoDesc +
                 ". Otherwise pick an overlay by name - the list rebuilds itself each launch as overlays are found, and your choice stays bound to that overlay by name. Re-launch after installing a new overlay mod for it to appear here.\",\n";
            s += "          \"valueOptions\": { \"sourceType\": \"ModSettingString\", \"options\": [ " + opts + " ], \"defaultValue\": \"Auto\" }\n";
            s += "        }";
            return s;
        };

        std::string j;
        j += "{\n";
        j += "  \"modName\": \"PrismaUIWristOverlays\",\n";
        j += "  \"displayName\": \"SteamVR's PrismaUI Wrist Overlays\",\n";
        j += "  \"minMcmVersion\": 2,\n";
        j += "  \"pages\": [\n    {\n      \"pageDisplayName\": \"Wrist Panel\",\n      \"content\": [\n";
        j += "        { \"type\": \"header\", \"text\": \"Wrist Panel\" },\n";
        j += "        {\n          \"id\": \"iResolution:Display\",\n          \"type\": \"slider\",\n";
        j += "          \"text\": \"Resolution  (0=1080 1=1440 2=2160)\",\n";
        j += "          \"help\": \"Render resolution of the wrist panel. 0 = 1920x1080, 1 = 2560x1440, 2 = 3840x2160. Higher fits more but costs GPU. Re-open a panel after changing.\",\n";
        j += "          \"valueOptions\": { \"min\": 0, \"max\": 2, \"step\": 1, \"defaultValue\": 0, \"formatString\": \"{0}\", \"sourceType\": \"ModSettingInt\" }\n        },\n";
        j += "        { \"type\": \"header\", \"text\": \"Wrist Icons  (pick an overlay by name)\" },\n";
        j += iconCtl("sIconSN:Icons", "Icon 1", "SkyrimNet dashboard") + ",\n";
        j += iconCtl("sIconSA:Icons", "Icon 2", "SeverActions") + ",\n";
        j += iconCtl("sIconIE:Icons", "Icon 3", "IntelEngine") + "\n";
        j += "      ]\n    }\n  ]\n}\n";

        std::ofstream out(".\\Data\\MCM\\Config\\PrismaUIWristOverlays\\config.json",
                          std::ios::binary | std::ios::trunc);
        if (out) {
            out << j;
            out.close();
            SKSE::log::info("Regenerated MCM config.json ({} overlay options; {} carried over "
                            "from the previous list, {} live views this pass)",
                            names.size(), carriedOver, views.size());
        } else {
            SKSE::log::warn("Could not open config.json to regenerate the overlay list");
        }
    }

    // Read the per-icon overlay overrides from the MCM Helper settings INI.
    // Read the per-icon NAME binding from the MCM settings INI. Each value is
    // "Off" / "Auto" / an overlay short-name. Read directly so mid-session MCM
    // changes are seen (the pump live-poll calls this ~1/s and re-binds/hides
    // any slot whose value changed). Default "Auto" (show the icon).
    void ReadIconBindings()
    {
        static const char* const kIconIniKeys[Slot_Count] = { "sIconSN", "sIconSA", "sIconIE" };
        for (int i = 0; i < Slot_Count; ++i) {
            std::string v = ReadIniStringDirect("Icons", kIconIniKeys[i], "Auto");
            if (v.empty()) v = "Auto";
            gIconBind[i] = v;
        }
    }
    inline bool IconIsOff(int i)   { return _stricmp(gIconBind[i].c_str(), "Off")  == 0; }
    inline bool IconIsAuto(int i)  { return _stricmp(gIconBind[i].c_str(), "Auto") == 0; }

    // Forward decls — defined below ResolvePrismaUI but called from BindSlotsToViews.
    void InstallFocusTracker(uint64_t viewId);
    void ApplyViewZoom(uint64_t viewId);   // keeps layout 1920x1080-equivalent
    void SetStickSuppressed(bool suppress); // masks jump+sneak while the pointer dot is on the panel
    void InstallNetworkDiagnostic(int slot, uint64_t viewId);
    void DispatchActivationRefresh(int slot, uint64_t viewId);
    void OnConsoleMessage(PrismaView view,
                          PRISMA_UI_API::ConsoleMessageLevel level,
                          const char* message);

    // Bind a slot's PRIMARY view (the main panel + focus-tracker host + the
    // default display/resolution target). Shared by both binding paths: the
    // legacy URL-substring auto-match and the MCM index override. Also resolves
    // the slot's family-discovery prefix used by Pass 2:
    //   - auto    : kSlotFamilyPrefix[i] (unchanged behaviour).
    //   - manual  : if the chosen URL contains a known family prefix, reuse it
    //               (keeps sibling-follow working for a re-bound sister-mod
    //               view regardless of which icon); otherwise restrict the
    //               family to exactly this one view (full URL as the prefix).
    void BindSlotPrimary(int i, uint64_t viewId, const std::string& url, bool manual)
    {
        gSlotViewIds[i]       = viewId;
        gSlotActiveViewIds[i] = viewId;  // default active = primary

        if (!manual) {
            gSlotFamilyPrefixActive[i] = kSlotFamilyPrefix[i];
        } else {
            std::string fam;
            for (int k = 0; k < Slot_Count; ++k) {
                if (ContainsIcase(url, kSlotFamilyPrefix[k])) { fam = kSlotFamilyPrefix[k]; break; }
            }
            gSlotFamilyPrefixActive[i] = fam.empty() ? url : fam;
        }

        SKSE::log::info("Slot[{}]='{}{}' PRIMARY bound to view {} ({}){}",
            i, kSlots[i].glyph1, kSlots[i].glyph2, viewId, url,
            manual ? " [MCM override]" : "");

        // Show() on PRIMARY: forces PrismaUI to keep rendering it so the bitmap
        // stays fresh for the wrist panel even when sister-mod logic hides it.
        if (gPrismaUI) gPrismaUI->Show(viewId);

        // Optional resize — disabled by default; see kPrismaResize* comment.
        if (kPrismaResizeEnabled && gResizeView) {
            if (gResizeView(viewId, kPrismaTargetWidth, kPrismaTargetHeight)) {
                SKSE::log::info("Slot[{}] view {} resized to {}x{}",
                    i, viewId, kPrismaTargetWidth, kPrismaTargetHeight);
            } else {
                SKSE::log::warn("Slot[{}] view {} resize FAILED — staying at default size",
                    i, viewId);
            }
        }
        InstallFocusTracker(viewId);
    }

    // Bind each fixed slot (SN / SA / IE) to a PrismaUI view discovered at
    // runtime by url-substring matching. Returns the number of slots that
    // got bound this call. Once a slot is bound it stays bound; subsequent
    // calls only fill in the unbound ones. Each newly-bound view also gets
    // VR-opt-out + Show() so PrismaUI keeps painting it for our consumption.
    int BindSlotsToViews(const std::vector<ViewEntry>& views)
    {
        int newlyBound = 0;

        // SMF slots resolve before the PrismaUI matching below: the source is
        // not a view, so there is nothing to match by URL. A slot bound to SMF
        // simply flags itself; the pump then takes the helper-panel path.
        for (int i = 0; i < Slot_Count; ++i) {
            const bool wantSmf = smf::IsSourceName(gIconBind[i]) && smf::Available();
            if (wantSmf != gSlotIsSmf[i]) {
                gSlotIsSmf[i] = wantSmf;
                SKSE::log::info("Slot[{}] source = {}", i,
                                wantSmf ? "SKSE Menu Framework (via ImGui VR Helper)"
                                        : "PrismaUI view");
            }
            if (wantSmf) {
                // Mark it satisfied so the discovery loop stops hunting for a
                // PrismaUI view for this slot, and clear any stale view state.
                gSlotViewIds[i]       = 0;
                gSlotActiveViewIds[i] = 0;
            }
        }

        // Refresh the per-icon NAME bindings from the MCM settings INI.
        ReadIconBindings();

        const int curCount = static_cast<int>(views.size());

        // Sorted-by-URL copy, id as tie-break — used only for the human-readable
        // launch log now (binding is by name, not by this position).
        std::vector<ViewEntry> sorted = views;
        std::sort(sorted.begin(), sorted.end(), [](const ViewEntry& a, const ViewEntry& b) {
            if (a.url != b.url) return a.url < b.url;
            return a.id < b.id;
        });

        // When the overlay set changes: (1) log the current list, and
        // (2) regenerate config.json so the three icon dropdowns list every
        // overlay found (by name). MCM Helper reads config.json at load, so a
        // freshly-installed overlay mod appears as a selectable option on the
        // next launch — no code change, index, or manual edit needed.
        if (curCount > 0 && curCount != gLoggedOverlayListCount) {
            SKSE::log::info("==== PrismaUI overlays found ({} total) ====", sorted.size());
            for (int n = 0; n < static_cast<int>(sorted.size()); ++n)
                SKSE::log::info("  {} -> {}", OverlayShortName(sorted[n].url), sorted[n].url);
            WriteMcmConfigJson(views);
            gLoggedOverlayListCount = curCount;
            gLastConfigGenCount     = curCount;
        }

        // ---- Pass 1: bind the PRIMARY view per slot ----------------------
        // gSlotViewIds[i] is the primary; this is the fallback display target
        // and the focus-tracker host. Each slot binds either by MCM index
        // override (>=0) or by legacy URL-substring auto-match (-1).
        for (int i = 0; i < Slot_Count; ++i) {
            if (gSlotViewIds[i] != 0) continue;  // primary already bound

            if (IconIsOff(i)) continue;          // icon is OFF — leave its slot unbound (hidden by the pump)
            if (IconIsAuto(i)) {
                // Auto: URL-substring match against the live view set.
                for (const auto& v : views) {
                    if (ContainsIcase(v.url, kSlots[i].match)) {
                        BindSlotPrimary(i, v.id, v.url, /*manual=*/false);
                        ++newlyBound;
                        break;
                    }
                }
            } else {
                // Manual: bind the view whose SHORT-NAME equals the chosen name.
                // Matches by identity, not list position, so it is stable across
                // overlay-set changes and needs no enumeration-stability gate.
                for (const auto& v : views) {
                    if (_stricmp(OverlayShortName(v.url).c_str(), gIconBind[i].c_str()) == 0) {
                        BindSlotPrimary(i, v.id, v.url, /*manual=*/true);
                        ++newlyBound;
                        break;
                    }
                }
                // If not found the slot stays unbound (icon shows but empty) and
                // the pump keeps retrying, so it binds the moment that overlay
                // appears (sister-mod views register a few seconds into a load).
            }
        }

        // ---- Pass 2: discover ALL family members per slot ---------------
        // For each slot, walk the full view list and pick up every view whose
        // URL contains kSlotFamilyPrefix[i]. Add to gSlotFamilyViewIds[i] if
        // not already present. For each NEWLY-discovered family member:
        //   1. gSetVREnab(viewId, false) — sets externalConsumer so PrismaUI
        //      doesn't 2D-draw it (was causing the "cut overlay" symptom when
        //      a SeverActions tab opened a secondary view).
        //   2. RegisterConsoleCallback (V2 only) — surfaces JS console output
        //      to SKSE log. Used to diagnose page-side failures like the
        //      SkyrimNet dashboard's fetch-from-localhost:8080 not connecting.
        //
        // We do NOT call Show() on non-primary family members. Their hide/show
        // state is owned by the sister mod and is the signal pump loop uses
        // to detect "user navigated to this view" → switch active.
        for (int i = 0; i < Slot_Count; ++i) {
            // Use the prefix resolved at primary-bind time (auto -> canonical
            // kSlotFamilyPrefix[i]; MCM-overridden -> derived from chosen URL).
            // Empty for slots whose primary isn't bound yet -> ContainsIcase
            // returns false, so no family is discovered until the slot binds.
            const std::string& famPrefix = gSlotFamilyPrefixActive[i];
            for (const auto& v : views) {
                if (!ContainsIcase(v.url, famPrefix.c_str())) continue;

                auto& family = gSlotFamilyViewIds[i];
                if (std::find(family.begin(), family.end(), v.id) != family.end()) continue;
                family.push_back(v.id);

                const bool isPrimary = (v.id == gSlotViewIds[i]);
                if (!isPrimary) {
                    // Suppress 2D draw for the sibling. Primary was already
                    // suppressed during Pass 1's slot-binding flow via the
                    // legacy code path; we re-assert here for safety.
                    if (gSetVREnab) gSetVREnab(v.id, false);
                    SKSE::log::info("Slot[{}] family SIBLING discovered: view {} ({})",
                        i, v.id, v.url);
                } else {
                    // Primary needs the explicit externalConsumer flag too —
                    // covers the case where someone binds primary outside the
                    // Pass-1 path in the future.
                    if (gSetVREnab) gSetVREnab(v.id, false);
                }

                // Console-log diagnostic on every family member (primary
                // included). Only active if V2 interface was resolved.
                if (gPrismaUI2) {
                    gPrismaUI2->RegisterConsoleCallback(v.id, &OnConsoleMessage);
                }

                // Network-activity diagnostic JS hook. Captures every fetch /
                // XHR / WebSocket / EventSource call + unhandled rejection.
                // Also fires a one-shot reachability probe to http://localhost:8080/
                // so we know whether Ultralight can reach the SkyrimNet
                // server at all, regardless of what the dashboard tries.
                InstallNetworkDiagnostic(i, v.id);
            }
        }

        // Re-arm the focus tracker on EVERY current family member of EVERY slot
        // (primary + siblings). The keyboard must summon for a text input in
        // whichever family view the panel is displaying, so all of them need the
        // tracker — not just the primary. Idempotent (window-flag guarded), so
        // this both installs newly-discovered siblings and auto-heals any view
        // whose page reloaded (its injected script was wiped; re-Invoking
        // re-installs it). Cheap: a no-op JS eval per already-armed live view.
        for (int i = 0; i < Slot_Count; ++i)
            for (uint64_t vid : gSlotFamilyViewIds[i]) {
                InstallFocusTracker(vid);
                ApplyViewZoom(vid);   // idempotent; re-applies after a page reload
            }

        return newlyBound;
    }

    // ----------------------------------------------------------------------
    // Refresh gSlotActiveViewIds[] from each slot's family hide/show state.
    //
    // Per slot, the active view = the family member currently NOT-hidden,
    // with priority:
    //   1. Most-recently-discovered non-primary sibling that is !IsHidden
    //      (i.e. sister mod has Show()'d it — most likely the user clicked
    //      a tab and the sister mod opened a secondary view).
    //   2. Else: primary (fallback / default display target).
    //
    // Called once per pump tick. Cheap — three slots × small family × one
    // virtual call per family member.
    // ----------------------------------------------------------------------
    void UpdateActiveViewsForSlots()
    {
        if (!gPrismaUI) return;
        for (int i = 0; i < Slot_Count; ++i) {
            const uint64_t primary = gSlotViewIds[i];
            if (primary == 0) continue;  // slot not yet bound

            uint64_t chosen = primary;  // default fallback

            // Walk family members in REVERSE so the most-recently-added
            // sibling wins ties (sister mods that show multiple at once —
            // most-recent is most likely what the user just navigated to).
            const auto& family = gSlotFamilyViewIds[i];
            for (auto it = family.rbegin(); it != family.rend(); ++it) {
                const uint64_t memberId = *it;
                if (memberId == primary) continue;  // primary checked last (fallback)
                if (!gPrismaUI->IsHidden(memberId)) {
                    chosen = memberId;
                    break;
                }
            }
            gSlotActiveViewIds[i] = chosen;
        }
    }

    // ----------------------------------------------------------------------
    // Single focus-changed callback, shared by EVERY view.
    //
    // PrismaUI's RegisterJSListener takes a `void(const char*)` C callback with
    // no per-view state, so the view carries its own identity in the argument:
    // the injected tracker reports "<viewId>:<0|1>". We record the focused view
    // id in one atomic; the pump only acts on it when it equals the currently
    // displayed view, so a hidden/background view (or an orphaned old view after
    // a rebind) can never spuriously drive the keyboard. Clearing uses a
    // compare-exchange so a stale view's focus-out can't wipe a DIFFERENT view's
    // focus (the multi-writer clobber this design exists to avoid).
    //
    // Runs on PrismaUI's JS-dispatch thread; only touches the atomic (safe).
    // ----------------------------------------------------------------------
    void OnFocusChanged(const char* arg) {
        if (!arg) return;
        // Parse a leading decimal view id, then expect ':' and a '0'/'1' flag.
        uint64_t vid = 0;
        const char* p = arg;
        for (; *p >= '0' && *p <= '9'; ++p) vid = vid * 10 + static_cast<uint64_t>(*p - '0');
        if (vid == 0 || *p != ':') return;
        if (p[1] == '1') {
            gFocusedTextInputView.store(vid);
        } else {
            uint64_t expected = vid;                        // clear ONLY if we are the recorded view
            gFocusedTextInputView.compare_exchange_strong(expected, 0);
        }
    }

    // ----------------------------------------------------------------------
    // JS console-message callback (PrismaUI V2). Diagnostic: surface page-side
    // console.log / warn / error / debug / info into the SKSE log so we can
    // see what's failing inside sister-mod pages — particularly the SkyrimNet
    // dashboard's fetch attempts to http://localhost:8080 from a file:// page
    // (CORS, network, or Ultralight policy errors are otherwise invisible).
    //
    // Registered per-view at bind time on every family member so we capture
    // logs from whichever view is active. PrismaUI guarantees this fires on
    // the rendering thread; we just forward to the SKSE log unmodified.
    // ----------------------------------------------------------------------
    void OnConsoleMessage(PrismaView view,
                          PRISMA_UI_API::ConsoleMessageLevel level,
                          const char* message)
    {
        if (!message) return;
        const char* lvl =
            (level == PRISMA_UI_API::ConsoleMessageLevel::Error)   ? "ERR" :
            (level == PRISMA_UI_API::ConsoleMessageLevel::Warning) ? "WRN" :
            (level == PRISMA_UI_API::ConsoleMessageLevel::Debug)   ? "DBG" :
            (level == PRISMA_UI_API::ConsoleMessageLevel::Info)    ? "INF" : "LOG";
        // Only LOG/INF at info level keeps the SKSE log sane. Errors and
        // warnings always surface as warn/error so they stand out.
        if (level == PRISMA_UI_API::ConsoleMessageLevel::Error) {
            SKSE::log::error("[js view={} {}] {}", view, lvl, message);
        } else if (level == PRISMA_UI_API::ConsoleMessageLevel::Warning) {
            SKSE::log::warn("[js view={} {}] {}", view, lvl, message);
        } else {
            SKSE::log::info("[js view={} {}] {}", view, lvl, message);
        }
    }

    // ----------------------------------------------------------------------
    // ======================================================================
    // ADDON-MODE INPUT — synthesised through the PUBLIC API only.
    //
    // Stock PrismaUI exports no FireMouseEvent/scroll/keyboard entry points, so
    // instead of injecting native Ultralight events we dispatch real DOM events
    // with IVPrismaUI1::Invoke — which is public, needs no hook or offset, and
    // marshals to PrismaUI's Ultralight thread for us (the one thread allowed
    // to touch a View). That makes this the most robust part of addon mode: it
    // cannot break when PrismaUI changes its internals.
    //
    // Coordinates arrive in VIEW PIXELS (from the overlay raycast's UV math).
    // The page reasons in CSS pixels, so the script rescales using the view's
    // own innerWidth/innerHeight — self-correcting for any device-pixel ratio
    // rather than assuming 1:1.
    // ======================================================================
    void InjectPointerEvent(uint64_t viewId, const char* kind,
                            int px, int py, uint32_t bmpW, uint32_t bmpH)
    {
        if (!gPrismaUI || viewId == 0 || px < 0 || py < 0 || !bmpW || !bmpH) return;

        // "click" = mousedown + mouseup + click, so pages that listen for any
        // of them respond, and React's synthetic layer (which delegates at the
        // document root) sees a properly bubbling event.
        std::string js =
            "(function(){try{"
            "var BW=" + std::to_string(bmpW) + ",BH=" + std::to_string(bmpH) + ";"
            "var sx=(window.innerWidth||BW)/BW, sy=(window.innerHeight||BH)/BH;"
            "var X=Math.round(" + std::to_string(px) + "*sx), Y=Math.round(" + std::to_string(py) + "*sy);"
            "var el=document.elementFromPoint(X,Y); if(!el)return;"
            "function ev(t){return new MouseEvent(t,{bubbles:true,cancelable:true,view:window,"
            "clientX:X,clientY:Y,button:0,buttons:(t==='mousedown'?1:0)});}";

        const std::string k = kind;
        if (k == "click") {
            js += "el.dispatchEvent(ev('mousedown'));"
                  "el.dispatchEvent(ev('mouseup'));"
                  "el.dispatchEvent(ev('click'));"
                  // Give keyboard focus to inputs so the SteamVR keyboard has a target.
                  "var f=el.closest?el.closest('input,textarea,select,[contenteditable=\"\"],[contenteditable=\"true\"]'):null;if(f&&f.focus)f.focus();else if(el.focus&&/^(INPUT|TEXTAREA|SELECT)$/.test(el.tagName||''))el.focus();";
        } else if (k == "down") {
            js += "el.dispatchEvent(ev('mousedown'));";
        } else if (k == "up") {
            js += "el.dispatchEvent(ev('mouseup'));el.dispatchEvent(ev('click'));"
                  // Focus a text field so the SteamVR keyboard has a target. This is
                  // the branch that actually runs (we send down-then-up, never "click").
                  "var f=el.closest?el.closest('input,textarea,select,[contenteditable=\"\"],[contenteditable=\"true\"]'):null;if(f&&f.focus)f.focus();else if(el.focus&&/^(INPUT|TEXTAREA|SELECT)$/.test(el.tagName||''))el.focus();";
        } else if (k == "rclick") {
            // Right-click: contextmenu is what web UIs actually listen for, but
            // send the button-2 mouse pair too for handlers that watch those.
            js += "el.dispatchEvent(new MouseEvent('mousedown',{bubbles:true,cancelable:true,"
                  "view:window,clientX:X,clientY:Y,button:2,buttons:2}));"
                  "el.dispatchEvent(new MouseEvent('mouseup',{bubbles:true,cancelable:true,"
                  "view:window,clientX:X,clientY:Y,button:2,buttons:0}));"
                  "el.dispatchEvent(new MouseEvent('contextmenu',{bubbles:true,cancelable:true,"
                  "view:window,clientX:X,clientY:Y,button:2}));";
        } else {  // "move" — drives :hover and mouseover-based UI
            js += "if(window.__pvr_hover!==el){"
                  "if(window.__pvr_hover)window.__pvr_hover.dispatchEvent("
                  "new MouseEvent('mouseout',{bubbles:true,view:window,clientX:X,clientY:Y}));"
                  "el.dispatchEvent(new MouseEvent('mouseover',{bubbles:true,view:window,clientX:X,clientY:Y}));"
                  "window.__pvr_hover=el;}"
                  "el.dispatchEvent(ev('mousemove'));";
        }
        js += "}catch(e){}})();";
        gPrismaUI->Invoke(viewId, js.c_str(), nullptr);
    }

    // Keep the page's LAYOUT at a 1920x1080-equivalent no matter what surface
    // resolution we forced, by applying a CSS zoom of (surfaceWidth / 1920).
    //
    // Without this, raising the resolution enlarges the CSS VIEWPORT rather
    // than the pixel density: a 4K view lays the page out in a 3840-wide space,
    // so every control renders at its normal px size inside a viewport twice as
    // wide and ends up half the apparent size — sharp, but too small to aim at
    // on a wrist panel. With the zoom, higher presets are pure supersampling:
    // identical layout, more pixels.
    //
    // CSS zoom does not change viewport coordinates, so pointer mapping and
    // elementFromPoint keep working unchanged.
    void ApplyViewZoom(uint64_t viewId)
    {
        if (!gPrismaUI || viewId == 0) return;
        const uint32_t fw = gForcedViewW.load(std::memory_order_relaxed);
        if (fw == 0) return;                       // not overriding the size
        const double factor = static_cast<double>(fw) / 1920.0;
        if (factor <= 1.001 && factor >= 0.999) return;   // 1080p — nothing to do

        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.4f", factor);
        const std::string js =
            std::string("(function(){try{var z='") + buf + "';"
            "if(window.__pvr_zoom===z)return;window.__pvr_zoom=z;"
            "var d=document.documentElement;if(d&&d.style)d.style.zoom=z;"
            "if(document.body&&document.body.style)document.body.style.zoom=z;"
            "}catch(e){}})();";
        gPrismaUI->Invoke(viewId, js.c_str(), nullptr);
    }

    // ---- addon-mode keyboard delivery -----------------------------------
    // Stock exposes no DeliverChar/DeliverVKey, so typed text goes in through
    // the page instead. execCommand('insertText') is the right primitive: it
    // respects the caret and selection and raises a proper `input` event, so
    // frameworks (React's onChange included) see a genuine edit — whereas a
    // synthetic keydown inserts nothing, and assigning .value directly wipes
    // the caret and bypasses change tracking.
    void InjectCharJS(uint64_t viewId, wchar_t ch)
    {
        if (!gPrismaUI || viewId == 0) return;
        // UTF-8 encode + JSON-escape the single character.
        char utf8[8]{};
        wchar_t buf[2] = { ch, 0 };
        ::WideCharToMultiByte(CP_UTF8, 0, buf, -1, utf8, sizeof(utf8), nullptr, nullptr);
        std::string esc;
        for (const char* p = utf8; *p; ++p) {
            const unsigned char c = static_cast<unsigned char>(*p);
            if (c == '\\' || c == '\'' ) { esc.push_back('\\'); esc.push_back(static_cast<char>(c)); }
            else if (c < 0x20)           { char t[8]; std::snprintf(t, sizeof(t), "\\u%04X", c); esc += t; }
            else                          esc.push_back(static_cast<char>(c));
        }
        const std::string js =
            "(function(){try{var e=document.activeElement;if(!e)return;"
            "if(document.execCommand){document.execCommand('insertText',false,'" + esc + "');return;}"
            "if('value' in e){var s=e.selectionStart==null?e.value.length:e.selectionStart,"
            "t=e.selectionEnd==null?s:e.selectionEnd;"
            "e.value=e.value.slice(0,s)+'" + esc + "'+e.value.slice(t);"
            "e.selectionStart=e.selectionEnd=s+1;"
            "e.dispatchEvent(new Event('input',{bubbles:true}));}"
            "}catch(err){}})();";
        gPrismaUI->Invoke(viewId, js.c_str(), nullptr);
    }

    // Control / navigation keys. Backspace and Enter are the ones that matter
    // for chat-style inputs; both are driven through the page rather than as
    // synthetic key events, which Ultralight would not action.
    void InjectVKeyJS(uint64_t viewId, int vk)
    {
        if (!gPrismaUI || viewId == 0) return;
        std::string body;
        switch (vk) {
            case 0x08:  // VK_BACK
                body = "if(document.execCommand){document.execCommand('delete',false,null);}"
                       "else if('value' in e && e.selectionStart>0){var s=e.selectionStart;"
                       "e.value=e.value.slice(0,s-1)+e.value.slice(e.selectionEnd);"
                       "e.selectionStart=e.selectionEnd=s-1;"
                       "e.dispatchEvent(new Event('input',{bubbles:true}));}";
                break;
            case 0x0D:  // VK_RETURN — let the page act on it (chat send, submit)
                body = "var o={bubbles:true,cancelable:true,key:'Enter',code:'Enter',keyCode:13,which:13};"
                       "e.dispatchEvent(new KeyboardEvent('keydown',o));"
                       "e.dispatchEvent(new KeyboardEvent('keypress',o));"
                       "e.dispatchEvent(new KeyboardEvent('keyup',o));"
                       "if(e.form&&e.form.requestSubmit)e.form.requestSubmit();";
                break;
            case 0x1B:  // VK_ESCAPE
                body = "var o={bubbles:true,cancelable:true,key:'Escape',code:'Escape',keyCode:27,which:27};"
                       "e.dispatchEvent(new KeyboardEvent('keydown',o));"
                       "e.dispatchEvent(new KeyboardEvent('keyup',o));if(e.blur)e.blur();";
                break;
            case 0x09:  // VK_TAB
                body = "var o={bubbles:true,cancelable:true,key:'Tab',code:'Tab',keyCode:9,which:9};"
                       "e.dispatchEvent(new KeyboardEvent('keydown',o));";
                break;
            default: return;
        }
        const std::string js =
            "(function(){try{var e=document.activeElement;if(!e)return;" + body + "}catch(err){}})();";
        gPrismaUI->Invoke(viewId, js.c_str(), nullptr);
    }

    // Scroll the element under the pointer. Dispatches a wheel event first (for
    // pages that handle it themselves), then walks up to the nearest actually
    // scrollable ancestor and moves it — Ultralight does not apply default
    // scrolling from a synthesised wheel event.
    void InjectScroll(uint64_t viewId, int px, int py, int dy,
                      uint32_t bmpW, uint32_t bmpH)
    {
        if (!gPrismaUI || viewId == 0 || dy == 0 || !bmpW || !bmpH) return;
        std::string js =
            "(function(){try{"
            "var BW=" + std::to_string(bmpW) + ",BH=" + std::to_string(bmpH) + ",D=" + std::to_string(dy) + ";"
            "var sx=(window.innerWidth||BW)/BW, sy=(window.innerHeight||BH)/BH;"
            "var X=Math.round(" + std::to_string(px < 0 ? 0 : px) + "*sx),"
            "    Y=Math.round(" + std::to_string(py < 0 ? 0 : py) + "*sy);"
            "var el=document.elementFromPoint(X,Y)||document.body; if(!el)return;"
            "el.dispatchEvent(new WheelEvent('wheel',{bubbles:true,cancelable:true,view:window,"
            "clientX:X,clientY:Y,deltaY:D}));"
            "var n=el;"
            "while(n&&n!==document.body){"
            "var s=getComputedStyle(n);"
            "if(n.scrollHeight>n.clientHeight+1&&/(auto|scroll|overlay)/.test(s.overflowY)){"
            "n.scrollTop+=D;return;}"
            "n=n.parentElement;}"
            "(document.scrollingElement||document.documentElement||document.body).scrollTop+=D;"
            "}catch(e){}})();";
        gPrismaUI->Invoke(viewId, js.c_str(), nullptr);
    }

    // Inject a JS focus tracker into ONE view + bind the shared listener so
    // PrismaUI calls us back whenever a text input gains or loses focus inside
    // that view. Installed on EVERY family member (primary + siblings), because
    // the wrist panel displays and forwards input to whichever family member
    // the sister mod has open — so the keyboard must be able to summon for a
    // text input in ANY of them, not just the primary.
    //
    // Deliberately NO C-side "already installed" guard: both RegisterJSListener
    // (keyed by (viewId, name), replace-on-recall) and the script Invoke (the
    // window-level flag makes it a no-op on an already-armed page) are safe to
    // repeat. Calling this every discovery tick therefore auto-heals a view
    // whose window was wiped by a location.reload() (the flag is gone, so the
    // script re-arms itself) — a C-side "installed" set would WRONGLY report it
    // installed and never re-arm it. Caller runs on the pump thread.
    // ----------------------------------------------------------------------
    void InstallFocusTracker(uint64_t viewId)
    {
        if (viewId == 0) return;
        if (!gPrismaUI) return;

        // One shared JS name/callback for every view. Safe to re-register.
        gPrismaUI->RegisterJSListener(viewId, "__prismauisvr_focus_changed", &OnFocusChanged);

        // The JS tracker hooks focusin/focusout on the document and reports
        // whether the new activeElement is a text input. It gates positive
        // reports ('1') on a userInteracted flag set by a mousedown listener —
        // so an autofocused input on page load (SkyrimNet's chat box, for
        // instance) does NOT summon the keyboard until the player has actually
        // clicked into the panel. The view bakes its OWN id into every report
        // ("<viewId>:<0|1>") so the single shared atomic identifies the exact
        // focused view. The window-level flag makes re-injection idempotent.
        const std::string vid = std::to_string(viewId);
        std::string script =
            "(function(){"
            "var FN='__prismauisvr_focus_changed';"
            "var VID='" + vid + "';"
            "if(window.__prismauisvr_focus_tracking_installed)return;"
            "window.__prismauisvr_focus_tracking_installed=true;"
            "var userInteracted=false;"
            "function isTI(el){"
              "if(!el||el.disabled||el.readOnly)return false;"
              "if(el.isContentEditable)return true;"
              "var tag=(el.tagName||'').toUpperCase();"
              "if(tag==='TEXTAREA')return true;"
              "if(tag!=='INPUT')return false;"
              "var type=((el.type||'text')+'').toLowerCase();"
              "switch(type){"
                "case '':case 'text':case 'search':case 'url':case 'tel':"
                "case 'password':case 'email':case 'number':return true;"
                "default:return false;"
              "}"
            "}"
            "function notify(){"
              "var v=(userInteracted&&isTI(document.activeElement))?'1':'0';"
              "if(typeof window[FN]==='function')window[FN](VID+':'+v);"
            "}"
            "document.addEventListener('mousedown',function(){"
              "userInteracted=true;"
              "setTimeout(notify,0);"
            "},true);"
            "document.addEventListener('focusin',notify,true);"
            "document.addEventListener('focusout',function(){setTimeout(notify,0);},true);"
            // No initial notify(): we explicitly ignore the page-load focus
            // state. The first user click sets userInteracted and triggers
            // the first notify via the mousedown handler.
            "})();";
        gPrismaUI->Invoke(viewId, script.c_str(), nullptr);
    }

    // ----------------------------------------------------------------------
    // Inject a network-activity diagnostic into a view. Hooks fetch / XHR /
    // WebSocket / EventSource / unhandledrejection and logs every event via
    // console.log/error — which our V2 ConsoleMessageCallback then forwards
    // into the SKSE log.
    //
    // Diagnostic for sister-mod pages whose production-build JS has its own
    // console output stripped or whose fetch failures are silently swallowed
    // by Promise.catch / React error boundaries (the SkyrimNet dashboard fits
    // this description — we get ZERO console output from it on a working
    // RegisterConsoleCallback wiring because the bundled code doesn't log).
    //
    // Also fires one explicit test fetch to http://localhost:8080/ at install
    // time so we KNOW whether Ultralight can reach the SkyrimNet server at
    // all, independent of whatever the dashboard's own fetch chain does.
    //
    // Idempotent — uses a window-level flag so re-running is harmless.
    // ----------------------------------------------------------------------
    void InstallNetworkDiagnostic(int slot, uint64_t viewId)
    {
        if (viewId == 0) return;
        if (!gPrismaUI)  return;

        // Single big JS string. Concise and defensive — works even if a hook
        // target (fetch / XHR / etc.) doesn't exist in this view's Ultralight.
        const std::string script = std::string(
            "(function(){"
            "if(window.__prismauisvr_netdiag_installed)return;"
            "window.__prismauisvr_netdiag_installed=true;"
            "var tag='[netdiag slot=") + std::to_string(slot) + std::string("]';"

            // ---- fetch ----
            "if(window.fetch){"
              "var of=window.fetch;"
              "window.fetch=function(input,init){"
                "var u=(typeof input==='string')?input:(input&&input.url)||'?';"
                "console.log(tag+' FETCH-> '+u+(init&&init.method?' method='+init.method:''));"
                "return of.apply(this,arguments).then(function(r){"
                  "console.log(tag+' FETCH-OK '+u+' status='+r.status);"
                  "return r;"
                "}).catch(function(err){"
                  "console.error(tag+' FETCH-ERR '+u+': '+((err&&err.message)||err));"
                  "throw err;"
                "});"
              "};"
            "}"

            // ---- XHR ----
            "if(window.XMLHttpRequest){"
              "var oo=XMLHttpRequest.prototype.open;"
              "var os=XMLHttpRequest.prototype.send;"
              "XMLHttpRequest.prototype.open=function(method,url){"
                "this.__nd_url=url;this.__nd_method=method;"
                "return oo.apply(this,arguments);"
              "};"
              "XMLHttpRequest.prototype.send=function(){"
                "var self=this;"
                "console.log(tag+' XHR-> '+(self.__nd_method||'?')+' '+(self.__nd_url||'?'));"
                "self.addEventListener('load',function(){"
                  "console.log(tag+' XHR-OK '+self.__nd_url+' status='+self.status);"
                "});"
                "self.addEventListener('error',function(){"
                  "console.error(tag+' XHR-ERR '+self.__nd_url+' (network error)');"
                "});"
                "return os.apply(this,arguments);"
              "};"
            "}"

            // ---- WebSocket ----
            "if(window.WebSocket){"
              "var OW=window.WebSocket;"
              "var nws=function(url,protocols){"
                "console.log(tag+' WS-> '+url);"
                "var ws=new OW(url,protocols);"
                "ws.addEventListener('open',function(){console.log(tag+' WS-OPEN '+url);});"
                "ws.addEventListener('error',function(){console.error(tag+' WS-ERR '+url);});"
                "ws.addEventListener('close',function(e){console.log(tag+' WS-CLOSE '+url+' code='+e.code);});"
                "return ws;"
              "};"
              "for(var k in OW)nws[k]=OW[k];"
              "window.WebSocket=nws;"
            "}"

            // ---- EventSource (Server-Sent Events) ----
            "if(window.EventSource){"
              "var OE=window.EventSource;"
              "var nes=function(url,init){"
                "console.log(tag+' SSE-> '+url);"
                "var es=new OE(url,init);"
                "es.addEventListener('open',function(){console.log(tag+' SSE-OPEN '+url);});"
                "es.addEventListener('error',function(){console.error(tag+' SSE-ERR '+url);});"
                "return es;"
              "};"
              "for(var k in OE)nes[k]=OE[k];"
              "window.EventSource=nes;"
            "}"

            // ---- Unhandled rejections (React/Promise error sink) ----
            "window.addEventListener('unhandledrejection',function(e){"
              "var r=e&&e.reason;"
              "console.error(tag+' UNHANDLED-REJ '+((r&&r.message)||r));"
            "});"

            // ---- One-shot localhost:8080 reachability test ----
            "try{"
              "if(window.fetch){"
                "console.log(tag+' probe http://localhost:8080/');"
                "fetch('http://localhost:8080/').then(function(r){"
                  "console.log(tag+' probe-OK status='+r.status);"
                "}).catch(function(e){"
                  "console.error(tag+' probe-ERR '+((e&&e.message)||e));"
                "});"
              "}"
            "}catch(e){"
              "console.error(tag+' probe-THROW '+e.message);"
            "}"

            // ---- JS-API availability snapshot ----
            // Sister mod plugins inject bridges into views — e.g. SeverActions
            // sets window.SKSE_API. If a bridge is missing or stale, the
            // dashboard's refresh button has nothing to call.
            "console.log(tag+' API-PROBE'"
              "+' SKSE_API='+(typeof window.SKSE_API)"
              "+' SN_API='+(typeof window.SN_API)"
              "+' SN_BASE='+JSON.stringify(window.__SN_BASE__)"
              "+' invokeNative='+(typeof window.invokeNative)"
              "+' prismaUI='+(typeof window.prismaUI)"
              "+' PrismaUI='+(typeof window.PrismaUI)"
            ");"

            "console.log(tag+' netdiag installed (location='+window.location.href+')');"
            "})();"
        );
        gPrismaUI->Invoke(viewId, script.c_str(), nullptr);
        SKSE::log::info("Slot[{}] network diagnostic installed on view {}", slot, viewId);
    }

    // ----------------------------------------------------------------------
    // Slot activation: tell the page "you're visible/focused now". Mimics
    // what flat Skyrim's input flow does when the user opens a PrismaUI menu
    // via key press — sister mod's OnShow handler re-fires, JS APIs may
    // reinitialize, and pages that lazy-load data on visibility change get
    // their refresh trigger.
    //
    // Sends three signals in one shot:
    //   1. window.dispatchEvent('focus')              — common refresh hook
    //   2. document.dispatchEvent('visibilitychange') — what real browsers
    //      fire when a tab becomes visible
    //   3. Re-probe and log JS APIs so we can see if anything changed
    //
    // All output goes through the V2 console callback into the SKSE log.
    // ----------------------------------------------------------------------
    void DispatchActivationRefresh(int slot, uint64_t viewId)
    {
        if (!gPrismaUI || viewId == 0) return;
        const std::string script = std::string(
            "(function(){"
            "var tag='[activation slot=") + std::to_string(slot) + std::string("]';"
            "try{"
              "if(document.dispatchEvent)document.dispatchEvent(new Event('visibilitychange'));"
              "if(window.dispatchEvent)window.dispatchEvent(new Event('focus'));"
              "console.log(tag+' dispatched focus + visibilitychange.'"
                "+' APIs:'"
                "+' SKSE_API='+(typeof window.SKSE_API)"
                "+' SN_API='+(typeof window.SN_API)"
                "+' SN_BASE='+JSON.stringify(window.__SN_BASE__)"
                "+' invokeNative='+(typeof window.invokeNative)"
                "+' prismaUI='+(typeof window.prismaUI)"
                "+' PrismaUI='+(typeof window.PrismaUI)"
              ");"

              // Try common refresh function names — many SPAs expose one.
              // We only LOG the attempt; if the function exists, we call it.
              "var FNS=['refresh','reload','reinit','init','loadData','updateData','fetchData'];"
              "for(var i=0;i<FNS.length;i++){"
                "var fn=FNS[i];"
                "if(typeof window[fn]==='function'){"
                  "console.log(tag+' calling window.'+fn+'()');"
                  "try{window[fn]();}catch(e){console.error(tag+' window.'+fn+' threw: '+e.message);}"
                "}"
              "}"
            "}catch(e){console.error(tag+' activation script threw: '+e.message);}"
            "})();"
        );
        gPrismaUI->Invoke(viewId, script.c_str(), nullptr);
    }

    // ----------------------------------------------------------------------
    // Resolve API entry points.
    // ----------------------------------------------------------------------
    bool ResolvePrismaUI()
    {
        HMODULE h = GetModuleHandleW(L"PrismaUI.dll");
        if (!h) {
            SKSE::log::error("PrismaUI.dll not loaded. Is 'PrismaUI SteamVR' higher priority than 'PrismaUI_VR Alpha' in MO2?");
            return false;
        }

        gPrismaUI = PRISMA_UI_API::RequestPluginAPI<PRISMA_UI_API::IVPrismaUI1>();
        if (!gPrismaUI) { SKSE::log::error("RequestPluginAPI returned null"); return false; }

        // V2 is optional — older PrismaUI builds don't have it. Without V2 we
        // simply don't register console callbacks; the rest of the plugin
        // still works.
        gPrismaUI2 = PRISMA_UI_API::RequestPluginAPI<PRISMA_UI_API::IVPrismaUI2>();
        if (!gPrismaUI2) {
            SKSE::log::warn("PrismaUI V2 interface unavailable (older PrismaUI.dll?) — console-message diagnostic disabled");
        }

        gGetBitmap      = reinterpret_cast<PrismaVR_GetViewBitmap_t     >(GetProcAddress(h, "PrismaVR_GetViewBitmap"));
        gGetBitmapIfNew = reinterpret_cast<PrismaVR_GetViewBitmapIfNew_t>(GetProcAddress(h, "PrismaVR_GetViewBitmapIfNew"));
        gSetVREnab      = reinterpret_cast<PrismaVR_SetViewVREnabled_t  >(GetProcAddress(h, "PrismaVR_SetViewVREnabled"));
        gGetCount       = reinterpret_cast<PrismaVR_GetViewCount_t      >(GetProcAddress(h, "PrismaVR_GetViewCount"));
        gGetViewInfo    = reinterpret_cast<PrismaVR_GetViewInfo_t       >(GetProcAddress(h, "PrismaVR_GetViewInfo"));
        gFireMouse      = reinterpret_cast<PrismaVR_FireMouseEvent_t    >(GetProcAddress(h, "PrismaVR_FireMouseEvent"));
        gResizeView     = reinterpret_cast<PrismaVR_ResizeView_t        >(GetProcAddress(h, "PrismaVR_ResizeView"));
        gDeliverChar       = reinterpret_cast<PrismaVR_DeliverChar_t      >(GetProcAddress(h, "PrismaVR_DeliverChar"));
        gDeliverVKey       = reinterpret_cast<PrismaVR_DeliverVKey_t      >(GetProcAddress(h, "PrismaVR_DeliverVKey"));
        gDeliverCharToView = reinterpret_cast<PrismaVR_DeliverCharToView_t>(GetProcAddress(h, "PrismaVR_DeliverCharToView"));
        gDeliverVKeyToView = reinterpret_cast<PrismaVR_DeliverVKeyToView_t>(GetProcAddress(h, "PrismaVR_DeliverVKeyToView"));
        gFireScrollToView  = reinterpret_cast<PrismaVR_FireScrollToView_t >(GetProcAddress(h, "PrismaVR_FireScrollToView"));

        // gGetBitmapIfNew + gResizeView + gDeliverChar/VKey are OPTIONAL — older PrismaUI builds don't have them.
        if (!gGetBitmap || !gSetVREnab || !gGetCount || !gGetViewInfo || !gFireMouse) {
            SKSE::log::error("Patched PrismaUI exports missing — not a fork build; trying addon mode.");
            // Null everything resolved so far. Stock 1.5+ exports a PARTIAL
            // PrismaVR_* set (the legacy OCU-gated DeliverChar/DeliverVKey) —
            // leaving those set after a failed resolve would let future code
            // gated on them call into a path that no-ops (or worse) outside
            // OCU. A failed resolve must leave NO fork pointers behind.
            gGetBitmap = nullptr;  gGetBitmapIfNew = nullptr;  gSetVREnab = nullptr;
            gGetCount = nullptr;   gGetViewInfo = nullptr;     gFireMouse = nullptr;
            gResizeView = nullptr; gDeliverChar = nullptr;     gDeliverVKey = nullptr;
            gDeliverCharToView = nullptr;  gDeliverVKeyToView = nullptr;
            gFireScrollToView = nullptr;
            return false;
        }
        SKSE::log::info("Patched PrismaUI exports resolved (if-new={}, resize={}, kbd-gated={}, kbd-toview={}, scroll={})",
            gGetBitmapIfNew ? "yes" : "no",
            gResizeView ? "yes" : "no",
            (gDeliverChar && gDeliverVKey) ? "yes" : "no",
            (gDeliverCharToView && gDeliverVKeyToView) ? "yes" : "no",
            gFireScrollToView ? "yes" : "no");
        return true;
    }

    // ----------------------------------------------------------------------
    // 5x7 glyph blit at (x, y) onto an RGBA buffer, scaled NxN per glyph
    // pixel. Used to draw the 2-letter labels on the icon textures.
    // ----------------------------------------------------------------------
    void DrawGlyph(uint8_t* rgba, uint32_t w, uint32_t h,
                   int x, int y, int scale, const Glyph5x7& g,
                   uint8_t r, uint8_t gg, uint8_t b, uint8_t a)
    {
        for (int gy = 0; gy < 7; ++gy) {
            uint8_t row = g.rows[gy];
            for (int gx = 0; gx < 5; ++gx) {
                if (!(row & (1u << (4 - gx)))) continue;
                for (int dy = 0; dy < scale; ++dy) {
                    for (int dx = 0; dx < scale; ++dx) {
                        int px = x + gx * scale + dx;
                        int py = y + gy * scale + dy;
                        if (px < 0 || py < 0 || px >= (int)w || py >= (int)h) continue;
                        uint8_t* p = rgba + (static_cast<size_t>(py) * w + px) * 4;
                        p[0] = r; p[1] = gg; p[2] = b; p[3] = a;
                    }
                }
            }
        }
    }

    // ----------------------------------------------------------------------
    // Build the 64x64 RGBA texture for a single icon: semi-transparent blue
    // disc with two white letters centered. Transparent outside the disc.
    // ----------------------------------------------------------------------
    void GenerateIconTexture(std::vector<uint8_t>& tex, char letter1, char letter2)
    {
        tex.assign(static_cast<size_t>(kIconTexW) * kIconTexH * 4, 0);
        const float cx = kIconTexW * 0.5f;
        const float cy = kIconTexH * 0.5f;
        const float r  = 30.0f;     // disc radius
        const float ringW = 2.0f;   // opaque blue rim width

        for (uint32_t y = 0; y < kIconTexH; ++y) {
            for (uint32_t x = 0; x < kIconTexW; ++x) {
                float dx = x + 0.5f - cx;
                float dy = y + 0.5f - cy;
                float d  = std::sqrt(dx * dx + dy * dy);
                uint8_t* p = tex.data() + (static_cast<size_t>(y) * kIconTexW + x) * 4;
                if (d <= r - ringW) {
                    // Inner fill: 75% transparent blue (was 41% transparent)
                    p[0] = 30; p[1] = 110; p[2] = 230; p[3] = 64;
                } else if (d <= r) {
                    // Rim: lighter than before but still readable as outline
                    p[0] = 0; p[1] = 40; p[2] = 130; p[3] = 96;
                }
            }
        }

        // Label: black glyph(s) on the semi-transparent blue disc.
        const Glyph5x7* g1 = GlyphFor(letter1);
        const Glyph5x7* g2 = GlyphFor(letter2);
        if (g1 && !g2) {
            // Single glyph (e.g. a digit 1/2/3): draw it bigger + centered.
            const int scale = 5;                     // 5x7 font x5 -> 25x35
            const int gw = 5 * scale, gh = 7 * scale;
            const int x0 = (static_cast<int>(kIconTexW) - gw) / 2;
            const int y0 = (static_cast<int>(kIconTexH) - gh) / 2;
            DrawGlyph(tex.data(), kIconTexW, kIconTexH, x0, y0, scale, *g1, 0, 0, 0, 220);
        } else {
            // Two-letter label: 5x7 font x4 -> each letter 20x28; 4px gap.
            const int scale = 4;
            const int letterW = 5 * scale;
            const int letterH = 7 * scale;
            const int gap = scale;
            const int totalW = letterW * 2 + gap;
            const int x0 = (static_cast<int>(kIconTexW) - totalW) / 2;
            const int y0 = (static_cast<int>(kIconTexH) - letterH) / 2;
            if (g1) DrawGlyph(tex.data(), kIconTexW, kIconTexH, x0, y0, scale, *g1, 0, 0, 0, 200);
            if (g2) DrawGlyph(tex.data(), kIconTexW, kIconTexH, x0 + letterW + gap, y0, scale, *g2, 0, 0, 0, 200);
        }
    }

    // ----------------------------------------------------------------------
    // Compose a world transform that places an overlay at world position
    // (wx, wy, wz) with its FRONT face (local +Z) pointing at the HMD —
    // standard billboard. Local +Y is world-up projected onto the plane
    // perpendicular to forward, so the overlay's "up" direction tracks the
    // sky regardless of head tilt. Returns true on success.
    // ----------------------------------------------------------------------
    bool MakeBillboardTransform(float wx, float wy, float wz,
                                vr::IVRSystem* system,
                                vr::HmdMatrix34_t& out)
    {
        vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount]{};
        system->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0.0f,
            poses, vr::k_unMaxTrackedDeviceCount);
        const auto& hmd = poses[vr::k_unTrackedDeviceIndex_Hmd];
        if (!hmd.bPoseIsValid) return false;

        const float hx = hmd.mDeviceToAbsoluteTracking.m[0][3];
        const float hy = hmd.mDeviceToAbsoluteTracking.m[1][3];
        const float hz = hmd.mDeviceToAbsoluteTracking.m[2][3];

        // Forward = from overlay → HMD (so the front of the overlay faces HMD)
        float fx = hx - wx, fy = hy - wy, fz = hz - wz;
        float flen = std::sqrt(fx * fx + fy * fy + fz * fz);
        if (flen < 1e-4f) return false;
        fx /= flen; fy /= flen; fz /= flen;

        // Up = world up projected perpendicular to forward.
        float ux = 0.0f, uy = 1.0f, uz = 0.0f;
        const float dot = ux * fx + uy * fy + uz * fz;
        ux -= dot * fx;  uy -= dot * fy;  uz -= dot * fz;
        float ulen = std::sqrt(ux * ux + uy * uy + uz * uz);
        if (ulen < 1e-4f) {
            // Forward parallel to world-up — pick world +X as the up reference instead.
            ux = 1.0f; uy = 0.0f; uz = 0.0f;
            const float d2 = ux * fx + uy * fy + uz * fz;
            ux -= d2 * fx; uy -= d2 * fy; uz -= d2 * fz;
            ulen = std::sqrt(ux * ux + uy * uy + uz * uz);
            if (ulen < 1e-4f) return false;
        }
        ux /= ulen; uy /= ulen; uz /= ulen;

        // Right = up × forward
        const float rx = uy * fz - uz * fy;
        const float ry = uz * fx - ux * fz;
        const float rz = ux * fy - uy * fx;

        // Pack as HmdMatrix34_t (column0=right, column1=up, column2=forward, column3=translation).
        out.m[0][0] = rx;  out.m[0][1] = ux;  out.m[0][2] = fx;  out.m[0][3] = wx;
        out.m[1][0] = ry;  out.m[1][1] = uy;  out.m[1][2] = fy;  out.m[1][3] = wy;
        out.m[2][0] = rz;  out.m[2][1] = uz;  out.m[2][2] = fz;  out.m[2][3] = wz;
        return true;
    }

    // ----------------------------------------------------------------------
    // Create one icon overlay for slot `i` and upload its label texture.
    // ----------------------------------------------------------------------
    bool CreateIconOverlay(int i)
    {
        auto* overlay = vr::VROverlay();
        if (!overlay) return false;
        const SlotDef& s = kSlots[i];

        auto err = overlay->CreateOverlay(s.overlayKey, s.overlayName, &gIconOvls[i]);
        if (err != vr::VROverlayError_None) {
            SKSE::log::error("Icon[{}] CreateOverlay failed: {} ({})",
                i, static_cast<int>(err), overlay->GetOverlayErrorNameFromEnum(err));
            return false;
        }
        overlay->SetOverlayInputMethod(gIconOvls[i], vr::VROverlayInputMethod_None);
        overlay->SetOverlayWidthInMeters(gIconOvls[i], kIconWidthMeters);
        overlay->SetOverlaySortOrder(gIconOvls[i], 2);  // above the panel

        std::vector<uint8_t> tex;
        GenerateIconTexture(tex, s.glyph1, s.glyph2);
        auto rerr = overlay->SetOverlayRaw(gIconOvls[i], tex.data(), kIconTexW, kIconTexH, 4);
        if (rerr != vr::VROverlayError_None) {
            SKSE::log::error("Icon[{}] SetOverlayRaw failed: {} ({})",
                i, static_cast<int>(rerr), overlay->GetOverlayErrorNameFromEnum(rerr));
        }
        SKSE::log::info("Icon[{}] created: '{}{}' handle={}", i, s.glyph1, s.glyph2, gIconOvls[i]);
        return true;
    }

    // Forward declaration for Mul34 (defined later, used by TickCalibration).
    vr::HmdMatrix34_t Mul34(const vr::HmdMatrix34_t& a, const vr::HmdMatrix34_t& b);

    // ----------------------------------------------------------------------
    // Calibration sequence definitions. Each phase lasts kCalPhaseDuration_s
    // seconds, in order. Translation phases move from (0 → kCalMaxOffset) on
    // the named axis. Rotation phases rotate from (0 → 360°) around the
    // named axis. After all phases, calibration mode ends and we return to
    // normal positioning.
    constexpr float kCalPhaseDuration_s = 3.0f;
    constexpr float kCalMaxOffset       = 0.20f;   // 20 cm
    constexpr float kCalMaxRotRadians   = 6.2831853f;  // 2π

    struct CalPhase {
        const char* label;
        int  type;  // 0 = translation, 1 = rotation
        int  axis;  // 0 = X, 1 = Y, 2 = Z (controller-local)
        int  sign;  // +1 or -1
    };
    constexpr CalPhase kCalPhases[] = {
        { "1: UP    (controller +Y, away from back of hand)",     0, 1, +1 },
        { "2: DOWN  (controller -Y, into back of hand)",          0, 1, -1 },
        { "3: LEFT  (controller -X)",                             0, 0, -1 },
        { "4: RIGHT (controller +X)",                             0, 0, +1 },
        { "5: FRONT (controller -Z, toward fingertips)",          0, 2, -1 },
        { "6: BACK  (controller +Z, toward elbow)",               0, 2, +1 },
        { "7: ROLL around X (pitch — local X axis)",              1, 0, +1 },
        { "8: ROLL around Y (yaw  — local Y axis)",               1, 1, +1 },
        { "9: ROLL around Z (roll — local Z axis)",               1, 2, +1 },
    };
    constexpr int   kCalPhaseCount     = sizeof(kCalPhases) / sizeof(kCalPhases[0]);
    constexpr float kCalTotalDuration  = kCalPhaseCount * kCalPhaseDuration_s;

    // ----------------------------------------------------------------------
    // Build a rotation matrix around a controller-local axis. Used by the
    // calibration sequence's rotation phases.
    // ----------------------------------------------------------------------
    vr::HmdMatrix34_t MakeRotationMatrix(int axis, float theta)
    {
        const float c = std::cos(theta);
        const float s = std::sin(theta);
        vr::HmdMatrix34_t r{};
        r.m[0][0] = 1.0f; r.m[1][1] = 1.0f; r.m[2][2] = 1.0f;  // identity
        if (axis == 0) {                   // around local X
            r.m[1][1] =  c;  r.m[1][2] = -s;
            r.m[2][1] =  s;  r.m[2][2] =  c;
        } else if (axis == 1) {            // around local Y
            r.m[0][0] =  c;  r.m[0][2] =  s;
            r.m[2][0] = -s;  r.m[2][2] =  c;
        } else {                           // around local Z
            r.m[0][0] =  c;  r.m[0][1] = -s;
            r.m[1][0] =  s;  r.m[1][1] =  c;
        }
        return r;
    }

    // ----------------------------------------------------------------------
    // Controller-local anchoring matrix (RAW — no user-calibration rotation
    // applied). Used by the calibration sequence itself so the F12 animation
    // tests the controller's ORIGINAL axes, not the post-calibration ones.
    //
    //   icon local +X = controller +X
    //   icon local +Y = controller -Z   (icon's "up" reads toward fingertips)
    //   icon local +Z = controller +Y   (icon faces away from back of hand)
    // ----------------------------------------------------------------------
    vr::HmdMatrix34_t MakeWristAnchorRaw(float ox, float oy, float oz)
    {
        vr::HmdMatrix34_t m = { {
            { 1.0f,  0.0f,  0.0f,  ox },
            { 0.0f,  0.0f,  1.0f,  oy },
            { 0.0f, -1.0f,  0.0f,  oz }
        } };
        return m;
    }

    // Compose + cache the user-tuned extrinsic XYZ rotation derived from
    // the F12 calibration sequence (phases 7, 8, 9). Lazily initialized on
    // first call. Re-computed never (constants are compile-time).
    const vr::HmdMatrix34_t& GetCalibratedRotation()
    {
        static bool init = false;
        static vr::HmdMatrix34_t cached{};
        if (!init) {
            constexpr float deg2rad = 3.14159265358979323846f / 180.0f;
            vr::HmdMatrix34_t Rx  = MakeRotationMatrix(0, kIconExtraRotXDeg      * deg2rad);
            vr::HmdMatrix34_t Ry  = MakeRotationMatrix(1, kIconExtraRotYDeg      * deg2rad);
            vr::HmdMatrix34_t Rz  = MakeRotationMatrix(2, kIconExtraRotZDeg      * deg2rad);
            vr::HmdMatrix34_t Rxf = MakeRotationMatrix(0, kIconExtraRotXFinalDeg * deg2rad);
            // Extrinsic order X → Z → Y → X_final. For column vectors:
            //   v' = R_xFinal · R_y · R_z · R_x · v.
            cached = Mul34(Rxf, Mul34(Ry, Mul34(Rz, Rx)));
            init = true;
        }
        return cached;
    }

    // Panel-only extra rotation. Lazily computed, cached. Same composition
    // as the global rotation chain but using kPanelExtraRot*Deg constants.
    // Applied AFTER MakeWristAnchor for the panel only.
    const vr::HmdMatrix34_t& GetPanelExtraRotation()
    {
        static bool init = false;
        static vr::HmdMatrix34_t cached{};
        if (!init) {
            constexpr float deg2rad = 3.14159265358979323846f / 180.0f;
            vr::HmdMatrix34_t Rx = MakeRotationMatrix(0, kPanelExtraRotXDeg * deg2rad);
            vr::HmdMatrix34_t Ry = MakeRotationMatrix(1, kPanelExtraRotYDeg * deg2rad);
            vr::HmdMatrix34_t Rz = MakeRotationMatrix(2, kPanelExtraRotZDeg * deg2rad);
            // Extrinsic XYZ: v' = R_z · R_y · R_x · v.
            cached = Mul34(Rz, Mul34(Ry, Rx));
            init = true;
        }
        return cached;
    }

    // ----------------------------------------------------------------------
    // Controller-local anchor with the user-tuned calibration rotation
    // applied AND the post-rotation +Z shift baked in. This is what icons
    // and the panel actually use at runtime.
    //
    // Mirrors SpellWheelVR's "Wrist Top" position math (parented to the
    // controller, rotates with the wrist), with our F12-calibration rotation
    // baked in on top of the raw base + a controller-local Z translation
    // applied AFTER the rotation (so it moves the whole rig "back toward the
    // elbow" in the same way calibration phase 6 demonstrated).
    // ----------------------------------------------------------------------
    vr::HmdMatrix34_t MakeWristAnchor(float ox, float oy, float oz)
    {
        vr::HmdMatrix34_t m = Mul34(GetCalibratedRotation(), MakeWristAnchorRaw(ox, oy, oz));
        m.m[1][3] += kIconPostRotOffsetY;
        m.m[2][3] += kIconPostRotOffsetZ;
        return m;
    }

    // ----------------------------------------------------------------------
    // Anchor icons + panel to LEFT controller using TrackedDeviceRelative.
    // Idempotent — safe to call every tick.
    //
    // Icons sit on TOP of the wrist (local +Y above), arrayed along the
    // forearm (local +Z toward elbow). Panel sits HIGHER above the wrist
    // AND further toward the elbow, so it floats above the icon row like a
    // pop-up screen.
    // ----------------------------------------------------------------------
    void AnchorAllToLeftController(vr::IVROverlay* overlay, uint32_t leftIdx)
    {
        if (leftIdx == vr::k_unTrackedDeviceIndexInvalid) return;

        for (int i = 0; i < Slot_Count; ++i) {
            if (gIconOvls[i] == vr::k_ulOverlayHandleInvalid) continue;
            // Spread icons along controller +X (across the wrist) so after
            // the calibration rotation chain they remain in the icon-face
            // plane (side-by-side from the viewer's POV), not collapsed
            // along the line of sight.
            const float ox = kIconSpreadAlongX
                ? (static_cast<float>(i) - 1.0f) * kIconSpacingMeters   // -spacing, 0, +spacing
                : 0.0f;
            const float oz = kIconSpreadAlongX
                ? kIconForearmBase
                : kIconForearmBase + i * kIconSpacingMeters;
            vr::HmdMatrix34_t m = MakeWristAnchor(ox, kIconAboveWrist, oz);
            overlay->SetOverlayTransformTrackedDeviceRelative(gIconOvls[i], leftIdx, &m);
        }
        if (gPanelOvl != vr::k_ulOverlayHandleInvalid) {
            vr::HmdMatrix34_t pm = MakeWristAnchor(0.0f, kPanelAboveWrist, kPanelForearmCenter);
            // Panel-only EXTRA rotation, applied in the panel's OWN local
            // frame via right-multiply. This rotates the orientation only —
            // the translation column of pm is untouched, so the panel
            // pivots around its anchor instead of swinging around the
            // controller origin. Means a 180° flip lands the panel face-
            // down WITHOUT relocating it, and calibration phases 8/9 trace
            // a circle around the panel's center rather than a big arc.
            pm = Mul34(pm, GetPanelExtraRotation());
            // Panel-only translation in controller-local space, applied
            // AFTER the rotation. Same axes as the calibration translation
            // phases (X = phases 3/4, Y = phases 1/2, Z = phases 5/6).
            pm.m[0][3] += kPanelExtraOffsetX;
            pm.m[1][3] += kPanelExtraOffsetY;
            pm.m[2][3] += kPanelExtraOffsetZ;
            overlay->SetOverlayTransformTrackedDeviceRelative(gPanelOvl, leftIdx, &pm);
        }
    }

    // ----------------------------------------------------------------------
    // Palm-down test: returns true when the LEFT controller's local +Y axis
    // (back of the hand for Vive-style grips, also broadly Index/Touch when
    // held naturally) is pointing UP in world space. That's the
    // "wristwatch-look" pose where the back of the forearm is visible to the
    // player and a wrist-menu makes sense to surface.
    // ----------------------------------------------------------------------
    // ----------------------------------------------------------------------
    // Run one tick of the calibration animation. Returns true while
    // calibration is still active, false when finished (caller should drop
    // the flag and resume normal positioning).
    // Only the SN icon is shown; SA / IE / panel are hidden so the
    // movement is unambiguous to the viewer.
    // ----------------------------------------------------------------------
    // ----------------------------------------------------------------------
    // Override the panel's anchor transform with the current calibration
    // phase's translation or rotation, applied to the panel's normal rest
    // pose (rotation chain + panel-extra offsets). Called AFTER the normal
    // AnchorAllToLeftController so this override wins.
    //
    // Icons stay in their normal positions — user watches the panel move
    // relative to a static icon row, picks which phase + magnitude maps to
    // the desired panel placement, and tells us the kPanelExtraOffset
    // values to bake in.
    // ----------------------------------------------------------------------
    void OverridePanelCalibrationTransform(vr::IVROverlay* overlay, uint32_t leftIdx,
                                           const CalPhase& ph, float phaseT)
    {
        if (gPanelOvl == vr::k_ulOverlayHandleInvalid) return;
        if (leftIdx == vr::k_unTrackedDeviceIndexInvalid) return;

        // Start from the panel's CURRENT rest pose: global rotation chain +
        // panel-only rotation (right-multiplied = in-place) + panel-only
        // translation. This way calibration always animates a delta from
        // where the panel actually sits, so every iteration tells us
        // "what's the next nudge from here?"
        vr::HmdMatrix34_t m = MakeWristAnchor(0.0f, kPanelAboveWrist, kPanelForearmCenter);
        m = Mul34(m, GetPanelExtraRotation());
        m.m[0][3] += kPanelExtraOffsetX;
        m.m[1][3] += kPanelExtraOffsetY;
        m.m[2][3] += kPanelExtraOffsetZ;

        if (ph.type == 0) {
            const float d = phaseT * kCalMaxOffset * ph.sign;
            m.m[ph.axis][3] += d;
        } else {
            // Right-multiply the rotation so the panel pivots in place
            // around its anchor — phases 8/9 trace a small circle of the
            // panel's orientation, NOT a giant arc through space.
            const float theta = phaseT * kCalMaxRotRadians * ph.sign;
            vr::HmdMatrix34_t rot = MakeRotationMatrix(ph.axis, theta);
            m = Mul34(m, rot);
        }

        overlay->SetOverlayTransformTrackedDeviceRelative(gPanelOvl, leftIdx, &m);
    }

    // SpellWheelVR's "BarViewAngle" equivalent: are we looking AT the wrist?
    // True when the angle between the HMD's forward direction and the vector
    // from HMD to the left controller is small. Decouples visibility from
    // how the wrist is oriented — works whether the player has palm-down,
    // palm-up, or sideways. Naturally hides everything when the player looks
    // forward into the world.
    bool DetectLookAtWrist(uint32_t leftIdx, vr::IVRSystem* system)
    {
        if (leftIdx == vr::k_unTrackedDeviceIndexInvalid || !system) return false;
        vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount]{};
        system->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0.0f,
            poses, vr::k_unMaxTrackedDeviceCount);
        const auto& hmd = poses[vr::k_unTrackedDeviceIndex_Hmd];
        const auto& ctl = poses[leftIdx];
        if (!hmd.bPoseIsValid || !ctl.bPoseIsValid || !ctl.bDeviceIsConnected) return false;

        // HMD forward = -Z of HMD world transform (standard OpenVR convention).
        const auto& hm = hmd.mDeviceToAbsoluteTracking;
        const float fx = -hm.m[0][2], fy = -hm.m[1][2], fz = -hm.m[2][2];

        // Vector HMD → controller.
        const auto& cm = ctl.mDeviceToAbsoluteTracking;
        float dx = cm.m[0][3] - hm.m[0][3];
        float dy = cm.m[1][3] - hm.m[1][3];
        float dz = cm.m[2][3] - hm.m[2][3];
        const float dlen = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (dlen < 1e-4f) return false;
        dx /= dlen; dy /= dlen; dz /= dlen;

        const float cosAngle = fx * dx + fy * dy + fz * dz;
        return cosAngle > kLookAtWristCosThreshold;
    }

    // ----------------------------------------------------------------------
    // "Wristwatch-look" pose: returns true when the LEFT controller's
    // fingertip direction (controller -Z, the direction the hand is
    // pointing) aligns with the HMD's right direction (HMD +X) within a
    // 30° half-angle cone. That's the pose a left-handed watch wearer
    // strikes — arm across the chest, palm-down, fingers pointing to the
    // player's right.
    //
    // Combined with DetectLookAtWrist this means icons only surface when
    // BOTH the player is looking at the hand AND the hand is held in the
    // expected "show me the menu" pose. Holding the hand by your side or
    // pointing it forward (e.g. during gameplay) keeps icons hidden.
    // ----------------------------------------------------------------------
    // 60° half-angle cone. Started at 30° per the user's spec but Index/
    // Touch grips tilt controller -Z ~20-30° off the actual finger axis,
    // which eats most of the budget before hand jitter kicks in. 60°
    // matches the look-at-wrist cone and holds the pose steady.
    constexpr float kWristwatchPoseCosThreshold = 0.5f;  // cos(60°)
    bool DetectWristwatchPose(uint32_t leftIdx, vr::IVRSystem* system, float* outDot = nullptr)
    {
        if (outDot) *outDot = 0.0f;
        if (leftIdx == vr::k_unTrackedDeviceIndexInvalid || !system) return false;
        vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount]{};
        system->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0.0f,
            poses, vr::k_unMaxTrackedDeviceCount);
        const auto& hmd  = poses[vr::k_unTrackedDeviceIndex_Hmd];
        const auto& left = poses[leftIdx];
        if (!hmd.bPoseIsValid || !left.bPoseIsValid || !left.bDeviceIsConnected) return false;

        const auto& hm = hmd.mDeviceToAbsoluteTracking;
        const auto& lm = left.mDeviceToAbsoluteTracking;
        // HMD right direction in world = first column of HMD pose (HMD +X).
        const float hrx = hm.m[0][0], hry = hm.m[1][0], hrz = hm.m[2][0];
        // Left controller fingertip direction in world = -Z (negative third column).
        const float lfx = -lm.m[0][2], lfy = -lm.m[1][2], lfz = -lm.m[2][2];
        const float d = hrx * lfx + hry * lfy + hrz * lfz;
        if (outDot) *outDot = d;
        return d >= kWristwatchPoseCosThreshold;
    }

    // ----------------------------------------------------------------------
    // Create the small floating cursor overlay (yellow disc + black ring).
    // Texture is uploaded once; transform is set each tick to follow the ray
    // hit point. Shown only when the raycast hits an icon — when it hits the
    // panel we still draw a cursor INTO the bitmap (which already aligns
    // perfectly with the click pixel by construction).
    // ----------------------------------------------------------------------
    bool CreateCursorOverlay()
    {
        auto* overlay = vr::VROverlay();
        if (!overlay) return false;

        auto err = overlay->CreateOverlay("prismauisvr.cursor", "PrismaUI SteamVR Cursor", &gCursorOvl);
        if (err != vr::VROverlayError_None) {
            SKSE::log::error("Cursor CreateOverlay failed: {} ({})",
                static_cast<int>(err), overlay->GetOverlayErrorNameFromEnum(err));
            return false;
        }
        overlay->SetOverlayInputMethod(gCursorOvl, vr::VROverlayInputMethod_None);
        overlay->SetOverlayWidthInMeters(gCursorOvl, kCursorWidthMeters);
        overlay->SetOverlaySortOrder(gCursorOvl, 3);  // above panel(1) and icons(2)

        // Generate disc texture.
        std::vector<uint8_t> tex(static_cast<size_t>(kCursorTexW) * kCursorTexH * 4, 0);
        const float cx = kCursorTexW * 0.5f, cy = kCursorTexH * 0.5f;
        const float outerR = 14.0f, innerR = 9.0f;
        for (uint32_t y = 0; y < kCursorTexH; ++y) {
            for (uint32_t x = 0; x < kCursorTexW; ++x) {
                float dx = x + 0.5f - cx;
                float dy = y + 0.5f - cy;
                float d  = std::sqrt(dx * dx + dy * dy);
                uint8_t* p = tex.data() + (static_cast<size_t>(y) * kCursorTexW + x) * 4;
                if      (d <= innerR) { p[0] = 255; p[1] = 220; p[2] = 0;   p[3] = 255; }
                else if (d <= outerR) { p[0] = 0;   p[1] = 0;   p[2] = 0;   p[3] = 255; }
            }
        }
        overlay->SetOverlayRaw(gCursorOvl, tex.data(), kCursorTexW, kCursorTexH, 4);
        SKSE::log::info("Cursor overlay created, handle = {}", gCursorOvl);
        return true;
    }

    // Position the cursor at a 3D hit point, slightly toward the HMD so it
    // doesn't z-fight with the hit surface, billboarded so it faces the user.
    void ShowCursorAt(vr::IVROverlay* overlay, vr::IVRSystem* system,
                     const vr::HmdVector3_t& hitPoint,
                     float towardHmd = kCursorTowardHmdOffset)
    {
        if (gCursorOvl == vr::k_ulOverlayHandleInvalid) return;

        // Compute offset toward HMD.
        vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount]{};
        system->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0.0f,
            poses, vr::k_unMaxTrackedDeviceCount);
        const auto& hmd = poses[vr::k_unTrackedDeviceIndex_Hmd];
        float wx = hitPoint.v[0], wy = hitPoint.v[1], wz = hitPoint.v[2];
        if (hmd.bPoseIsValid) {
            const float hx = hmd.mDeviceToAbsoluteTracking.m[0][3];
            const float hy = hmd.mDeviceToAbsoluteTracking.m[1][3];
            const float hz = hmd.mDeviceToAbsoluteTracking.m[2][3];
            float tx = hx - wx, ty = hy - wy, tz = hz - wz;
            float l = std::sqrt(tx*tx + ty*ty + tz*tz);
            if (l > 1e-4f) {
                wx += towardHmd * (tx / l);
                wy += towardHmd * (ty / l);
                wz += towardHmd * (tz / l);
            }
        }

        vr::HmdMatrix34_t m{};
        if (MakeBillboardTransform(wx, wy, wz, system, m)) {
            overlay->SetOverlayTransformAbsolute(gCursorOvl, vr::TrackingUniverseStanding, &m);
            if (!gCursorShownLast) {
                overlay->ShowOverlay(gCursorOvl);
                gCursorShownLast = true;
            }
        }
    }

    void HideCursor(vr::IVROverlay* overlay)
    {
        if (gCursorOvl == vr::k_ulOverlayHandleInvalid) return;
        if (!gCursorShownLast) return;
        overlay->HideOverlay(gCursorOvl);
        gCursorShownLast = false;
    }

    // ----------------------------------------------------------------------
    // D3D11 + shared texture initialization for the SetOverlayTexture path.
    //
    // Creates our own D3D11 device (separate from Skyrim's render device —
    // avoids needing a Present hook or render-thread coordination), allocates
    // a single 1920×1080 BGRA texture flagged for cross-process NT-handle
    // sharing, and obtains a global HANDLE pointing at it. The handle is
    // what we'll hand to SteamVR via SetOverlayTexture once at panel-show
    // time; subsequent per-tick updates just UpdateSubresource into our
    // local texture and vrcompositor.exe sees the new pixels through the
    // shared GPU memory mapping. No per-submit byte upload, no internal
    // staging-pool churn — should bypass the ~197 SetOverlayRaw wedge.
    // ----------------------------------------------------------------------
    // (Re)allocate the shared texture at exactly w×h. Releases any previous
    // texture + handle. Requires gD3DDevice already created. On success sets
    // gSharedTex/Handle + gSharedTexW/H and clears gTextureBoundToOverlay so
    // the pump re-binds the (new) handle to the overlay via SetOverlayTexture.
    bool CreateSharedTexAtSize(uint32_t w, uint32_t h)
    {
        if (!gD3DDevice) return false;
        if (w == 0 || h == 0) return false;
        if (w > kSharedTexWidth)  w = kSharedTexWidth;
        if (h > kSharedTexHeight) h = kSharedTexHeight;

        // Release the old texture (its DXGI shared handle dies with it).
        if (gSharedTex) { gSharedTex->Release(); gSharedTex = nullptr; }
        gSharedTexHandle = nullptr;
        gTextureBoundToOverlay = false;  // force re-bind of the new handle

        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width            = w;
        desc.Height           = h;
        desc.MipLevels        = 1;
        desc.ArraySize        = 1;
        desc.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;  // PrismaUI's native BGRA
        desc.SampleDesc.Count = 1;
        desc.Usage            = D3D11_USAGE_DEFAULT;
        desc.BindFlags        = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags   = 0;
        desc.MiscFlags        = D3D11_RESOURCE_MISC_SHARED;  // legacy shared (implicit DXGI sync)
        HRESULT hr = gD3DDevice->CreateTexture2D(&desc, nullptr, &gSharedTex);
        if (FAILED(hr)) {
            SKSE::log::error("CreateTexture2D (shared {}x{}) failed: 0x{:08x}", w, h, static_cast<uint32_t>(hr));
            return false;
        }
        IDXGIResource* dxgi = nullptr;
        hr = gSharedTex->QueryInterface(__uuidof(IDXGIResource), reinterpret_cast<void**>(&dxgi));
        if (FAILED(hr) || !dxgi) {
            SKSE::log::error("QI for IDXGIResource failed: 0x{:08x}", static_cast<uint32_t>(hr));
            return false;
        }
        hr = dxgi->GetSharedHandle(&gSharedTexHandle);
        dxgi->Release();
        if (FAILED(hr) || !gSharedTexHandle) {
            SKSE::log::error("GetSharedHandle failed: 0x{:08x}", static_cast<uint32_t>(hr));
            return false;
        }
        gSharedTexW = w; gSharedTexH = h;
        SKSE::log::info("Shared texture created: {}x{} BGRA8, legacy shared handle = 0x{:x}",
            w, h, reinterpret_cast<uintptr_t>(gSharedTexHandle));
        return true;
    }

    bool InitSharedTexture()
    {
        if (gSharedTex && gSharedTexHandle) return true;  // already initialized

        // Create a D3D11 device (once). HARDWARE driver type, FL11.
        const D3D_FEATURE_LEVEL want[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
        D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_11_0;
        HRESULT hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
            want, _countof(want), D3D11_SDK_VERSION,
            &gD3DDevice, &got, &gD3DContext);
        if (FAILED(hr)) {
            SKSE::log::error("D3D11CreateDevice failed: 0x{:08x}", static_cast<uint32_t>(hr));
            return false;
        }
        SKSE::log::info("D3D11 device created at feature level {}.{}",
            (got >> 12) & 0xF, (got >> 8) & 0xF);

        // Allocate the initial shared texture at the base viewport size. The
        // pump reallocates it to match the bitmap whenever the size changes
        // (e.g. the user resizes a panel), so the overlay always shows the
        // texture 1:1 — no dead region, no texture-bounds cropping needed.
        return CreateSharedTexAtSize(kViewportBaseW, kViewportBaseH);
    }

    // Ensure the shared texture is exactly w×h (reallocate if not). Called
    // from the pump right before binding/updating so the texture tracks the
    // current bitmap size.
    bool EnsureSharedTexSize(uint32_t w, uint32_t h)
    {
        if (w > kSharedTexWidth)  w = kSharedTexWidth;
        if (h > kSharedTexHeight) h = kSharedTexHeight;
        if (w == gSharedTexW && h == gSharedTexH && gSharedTex) return true;
        SKSE::log::info("Shared texture resize {}x{} -> {}x{}", gSharedTexW, gSharedTexH, w, h);
        return CreateSharedTexAtSize(w, h);
    }

    void ShutdownSharedTexture()
    {
        // Legacy DXGI shared handles are process-scoped values, not OS
        // handles — DON'T call CloseHandle on them. Released when the
        // owning texture is released.
        gSharedTexHandle = nullptr;
        if (gSharedTex)    { gSharedTex->Release();    gSharedTex    = nullptr; }
        if (gD3DContext)   { gD3DContext->Release();   gD3DContext   = nullptr; }
        if (gD3DDevice)    { gD3DDevice->Release();    gD3DDevice    = nullptr; }
        gTextureBoundToOverlay = false;
        gSharedTexW = 0; gSharedTexH = 0;
    }

    // ----------------------------------------------------------------------
    // Bind the shared texture to gPanelOvl via SetOverlayTexture. Called
    // once per panel-handle-lifetime (i.e. at first activation, or after
    // a handle recycle). Subsequent per-tick updates only need to refresh
    // the texture's pixel contents — SetOverlayTexture is NOT called again.
    // ----------------------------------------------------------------------
    bool BindSharedTextureToPanel(vr::IVROverlay* overlay)
    {
        if (!overlay || gPanelOvl == vr::k_ulOverlayHandleInvalid) return false;
        if (!gSharedTexHandle) return false;

        vr::Texture_t tex{};
        tex.handle     = gSharedTexHandle;
        tex.eType      = vr::TextureType_DXGISharedHandle;
        tex.eColorSpace = vr::ColorSpace_Auto;
        auto err = overlay->SetOverlayTexture(gPanelOvl, &tex);
        if (err != vr::VROverlayError_None) {
            SKSE::log::error("SetOverlayTexture failed: {} ({})", static_cast<int>(err),
                overlay->GetOverlayErrorNameFromEnum(err));
            return false;
        }
        gTextureBoundToOverlay = true;
        SKSE::log::info("Shared texture bound to panel overlay (handle={})", gPanelOvl);
        return true;
    }

    // ----------------------------------------------------------------------
    // Copy a BGRA pixel buffer into the shared texture. With the legacy
    // D3D11_RESOURCE_MISC_SHARED flag (no keyed mutex), DXGI provides
    // implicit cross-process synchronization — vrcompositor's reads and
    // our writes are coordinated by the driver. Small tearing windows are
    // possible during rapid updates but typically not visible at human-
    // interaction speed.
    // ----------------------------------------------------------------------
    bool UpdateSharedTexture(const uint8_t* bgra, uint32_t w, uint32_t h, uint32_t stride)
    {
        if (!gSharedTex || !gD3DContext) return false;

        const uint32_t cw = (w < gSharedTexW) ? w : gSharedTexW;
        const uint32_t ch = (h < gSharedTexH) ? h : gSharedTexH;
        D3D11_BOX box = { 0, 0, 0, cw, ch, 1 };
        gD3DContext->UpdateSubresource(gSharedTex, 0, &box, bgra, stride, 0);
        gD3DContext->Flush();  // ensure the copy is committed before vrcompositor's next read
        return true;
    }

    // ----------------------------------------------------------------------
    // Create the active-panel overlay. Texture is uploaded by the pump every
    // time PrismaUI emits a new frame, and the transform is re-anchored to
    // the LEFT controller via AnchorAllToLeftController.
    // ----------------------------------------------------------------------
    bool CreatePanelOverlay()
    {
        auto* overlay = vr::VROverlay();
        if (!overlay) { SKSE::log::error("vr::VROverlay() returned null"); return false; }

        auto err = overlay->CreateOverlay(kPanelKey, kPanelName, &gPanelOvl);
        if (err != vr::VROverlayError_None) {
            SKSE::log::error("Panel CreateOverlay failed: {} ({})", static_cast<int>(err),
                overlay->GetOverlayErrorNameFromEnum(err));
            return false;
        }
        SKSE::log::info("Panel overlay created, handle = {}", gPanelOvl);
        overlay->SetOverlayInputMethod(gPanelOvl, vr::VROverlayInputMethod_None);
        overlay->SetOverlayWidthInMeters(gPanelOvl, kPanelWidthMeters);
        overlay->SetOverlaySortOrder(gPanelOvl, 1);
        overlay->SetOverlayAlpha(gPanelOvl, kPanelOpacity);
        return true;
    }

    // ----------------------------------------------------------------------
    // Zoom + resolution helpers (two independent controls).
    // ----------------------------------------------------------------------
    // ZOOM: physical panel width = kPanelWidthMeters × the slot's zoom factor.
    // Cheap, smooth — used for live drag feedback from the corner handle. Does
    // NOT touch the HTML render resolution (that's the MCM picker's job).
    void ApplyZoom(int slot, vr::IVROverlay* overlay)
    {
        if (slot < 0 || slot >= Slot_Count) return;
        if (overlay && gPanelOvl != vr::k_ulOverlayHandleInvalid) {
            overlay->SetOverlayWidthInMeters(gPanelOvl, kPanelWidthMeters * gSlotScale[slot]);
        }
    }

    // RESOLUTION: read the MCM-chosen preset index from the MCM Helper
    // settings INI. MCM Helper stores ModSettingInt values at
    //   Data/MCM/Settings/<modName>.ini  ->  [Section] key=value
    // We use modName "PrismaUIWristOverlays", key "iResolution", section "Display".
    // Falls back to 0 (Low/1920×1080) if the file/key is missing. Cheap enough
    // to call on each activation so MCM changes apply on the next panel open.
    void ReadResolution()
    {
        // Read the file directly (bypasses PrivateProfileRedirector's startup
        // cache so mid-session MCM changes are seen). Resolves through MO2 VFS.
        const int idx = ReadIniIntDirect("Display", "iResolution", 0);
        gResolutionIndex = (idx < 0) ? 0 : (idx >= kResPresetCount ? kResPresetCount - 1 : idx);
    }

    // Apply the current resolution preset to a slot's active view by resizing
    // its HTML viewport via PrismaVR_ResizeView. The shared texture reallocs
    // to match in the pump (EnsureSharedTexSize), so the overlay shows the new
    // resolution 1:1. This is what makes more CONTENT fit (responsive pages).
    void ApplyResolution(int slot)
    {
        if (slot < 0 || slot >= Slot_Count) return;
        if (!gResizeView) return;
        const uint64_t view = gSlotActiveViewIds[slot];
        if (view == 0) return;
        const ResPreset& r = kResPresets[gResolutionIndex];
        gResizeView(view, r.w, r.h);
        SKSE::log::info("ApplyResolution: slot {} view {} -> {}x{} ({})",
            slot, view, r.w, r.h, r.name);
    }

    // ----------------------------------------------------------------------
    // Pre-emptive panel-handle recycling. Called from the pump once a
    // handle has racked up kPanelRecycleAt successful submits (well below
    // the empirically-measured ~197-submit SteamVR wedge threshold).
    //
    // DestroyOverlay frees SteamVR's internal bookkeeping for that handle.
    // The new handle from CreatePanelOverlay starts with a fresh submit
    // budget. The transform gets reapplied on the next AnchorAllToLeftController
    // call, and gForceNextSubmit ensures we push a frame to the new handle
    // on the very next pump iteration so the user only sees a single-frame
    // black panel before content returns.
    // ----------------------------------------------------------------------
    void RecyclePanelHandle()
    {
        auto* overlay = vr::VROverlay();
        if (!overlay) return;
        if (gPanelOvl == vr::k_ulOverlayHandleInvalid) return;

        ++gPanelRecycleCount;
        SKSE::log::info("Recycling panel handle #{} (submits={}, totalSinceStart={})",
            gPanelRecycleCount, gPanelSubmitCount, gSubmitSuccessCount.load());

        overlay->DestroyOverlay(gPanelOvl);
        gPanelOvl = vr::k_ulOverlayHandleInvalid;
        gPanelShownLast = false;
        gPanelSubmitCount = 0;
        gTextureBoundToOverlay = false;  // need to re-bind shared texture to the new handle

        if (!CreatePanelOverlay()) {
            SKSE::log::error("RecyclePanelHandle: re-create FAILED — panel will be unavailable");
            return;
        }
        gForceNextSubmit = true;  // push a frame to the new handle ASAP
    }

    // ----------------------------------------------------------------------
    // Cast a ray from the pointer controller along its forward axis and, if
    // it hits our overlay, forward mouse-move / click events to the PrismaUI
    // view backing it. Bitmap (w x h) is the current view's pixel resolution;
    // intersection UVs are in [0,1] and map onto those pixels.
    //
    // Pointer hand = RIGHT controller. The LEFT controller hosts the wrist
    // menu (icons + panel anchor), so the natural pointing motion is to
    // aim with the dominant-eye-side controller and use its analog trigger
    // as the click.
    //
    // Trigger detection uses analog axis 1 (legacy controller state). The
    // game still sees the trigger normally — we only READ the axis value,
    // we don't consume the input.
    // ----------------------------------------------------------------------
    // BGRA8 variant of DrawCursor for the shared-texture path (where we
    // keep PrismaUI's native BGRA pixels and write them straight into a
    // BGRA8 D3D11 texture without an intermediate RGBA conversion). Uses
    // `stride` instead of assuming tight packing because PrismaUI's bitmap
    // can have a row stride > w*4.
    // ----------------------------------------------------------------------
    // Synthetic panel content — the overlay-pipeline self-test.
    //
    // Drawn when a slot has no bound PrismaUI view (e.g. a stock PrismaUI with
    // no PrismaVR_* API). It pushes a live, obviously-synthetic image through
    // the EXACT same path real content uses: shared D3D11 texture ->
    // SetOverlayTexture -> wrist overlay. So if this shows up and animates,
    // every part of our SteamVR wiring is proven good and any remaining
    // problem is purely "how do we get PrismaUI's pixels", not "is our overlay
    // plumbed correctly".
    //
    // Deliberately unmistakable: blue field, white border, the slot's big digit,
    // and a bar that sweeps across so a FROZEN image is visibly different from
    // a LIVE one (a static image would mean submits stopped).
    void DrawTestPatternBGRA(uint8_t* bgra, uint32_t w, uint32_t h, uint32_t stride,
                             int slot, uint64_t tick)
    {
        if (!bgra || w == 0 || h == 0) return;

        // Blue vertical gradient (BGRA: B,G,R,A)
        for (uint32_t y = 0; y < h; ++y) {
            const uint8_t g = static_cast<uint8_t>(40 + (110 * y) / (h ? h : 1));
            uint8_t* row = bgra + static_cast<size_t>(y) * stride;
            for (uint32_t x = 0; x < w; ++x) {
                uint8_t* px = row + x * 4;
                px[0] = 200;  // B — strongly blue so it cannot be mistaken for HTML
                px[1] = g;    // G
                px[2] = 20;   // R
                px[3] = 255;  // A
            }
        }

        // White border so the panel's exact extent is visible in the headset
        const uint32_t bw = 6;
        for (uint32_t y = 0; y < h; ++y) {
            uint8_t* row = bgra + static_cast<size_t>(y) * stride;
            const bool edgeRow = (y < bw) || (y >= h - bw);
            for (uint32_t x = 0; x < w; ++x) {
                if (!edgeRow && x >= bw && x < w - bw) continue;
                uint8_t* px = row + x * 4;
                px[0] = 255; px[1] = 255; px[2] = 255; px[3] = 255;
            }
        }

        // Sweeping bar — proves the panel is being RE-SUBMITTED, not frozen
        const uint32_t barW = w / 24 ? w / 24 : 1;
        const uint32_t span = (w > barW) ? (w - barW) : 1;
        const uint32_t barX = static_cast<uint32_t>((tick * 7) % span);
        for (uint32_t y = h / 4; y < (h * 3) / 4 && y < h; ++y) {
            uint8_t* row = bgra + static_cast<size_t>(y) * stride;
            for (uint32_t x = barX; x < barX + barW && x < w; ++x) {
                uint8_t* px = row + x * 4;
                px[0] = 0; px[1] = 230; px[2] = 255; px[3] = 255;  // yellow
            }
        }

        // The slot's digit, big and centered, using our 5x7 font
        const char digit = static_cast<char>('1' + (slot >= 0 && slot < 9 ? slot : 0));
        if (const Glyph5x7* g = GlyphFor(digit)) {
            const uint32_t scale = (h / 14) ? (h / 14) : 1;   // ~half the panel height
            const uint32_t gw = 5 * scale, gh = 7 * scale;
            const uint32_t x0 = (w > gw) ? (w - gw) / 2 : 0;
            const uint32_t y0 = (h > gh) ? (h - gh) / 2 : 0;
            for (uint32_t ry = 0; ry < 7; ++ry) {
                for (uint32_t rx = 0; rx < 5; ++rx) {
                    if (!((g->rows[ry] >> (4 - rx)) & 1)) continue;
                    for (uint32_t sy = 0; sy < scale; ++sy) {
                        const uint32_t py = y0 + ry * scale + sy;
                        if (py >= h) continue;
                        uint8_t* row = bgra + static_cast<size_t>(py) * stride;
                        for (uint32_t sx = 0; sx < scale; ++sx) {
                            const uint32_t px_ = x0 + rx * scale + sx;
                            if (px_ >= w) continue;
                            uint8_t* px = row + px_ * 4;
                            px[0] = 255; px[1] = 255; px[2] = 255; px[3] = 255;
                        }
                    }
                }
            }
        }
    }

    void DrawCursorBGRA(uint8_t* bgra, uint32_t w, uint32_t h, uint32_t stride, int cx, int cy)
    {
        if (cx < 0 || cy < 0) return;
        const int R = kCursorOuterRadius;
        const int r = kCursorInnerRadius;
        const int R2 = R * R;
        const int r2 = r * r;
        for (int dy = -R; dy <= R; ++dy) {
            int y = cy + dy;
            if (y < 0 || y >= static_cast<int>(h)) continue;
            for (int dx = -R; dx <= R; ++dx) {
                int x = cx + dx;
                if (x < 0 || x >= static_cast<int>(w)) continue;
                int d2 = dx * dx + dy * dy;
                if (d2 > R2) continue;
                uint8_t* px = bgra + static_cast<size_t>(y) * stride + x * 4;
                if (d2 < r2) {
                    // Yellow in BGRA: B=0, G=220, R=255
                    px[0] = 0;   px[1] = 220; px[2] = 255; px[3] = 255;
                } else {
                    px[0] = 0;   px[1] = 0;   px[2] = 0;   px[3] = 255;
                }
            }
        }
    }

    // Draw the resize grab-handle in the TOP-RIGHT corner of a BGRA8 buffer:
    // a translucent light-blue triangle (dog-ear) with white diagonal grip
    // lines, so the user can see where to grab to enlarge the panel.
    // `active` brightens it while a resize drag is in progress.
    void DrawResizeHandleBGRA(uint8_t* bgra, uint32_t w, uint32_t h, uint32_t stride, bool active)
    {
        // Handle size = a fraction of the bitmap (so it always sits at the
        // visual top-right corner of whatever is displayed). Use a square
        // dog-ear sized by the smaller dimension for a consistent shape.
        const int hs = static_cast<int>((w < h ? w : h) * kResizeHandleFrac);
        if (hs < 8) return;
        const int x0 = static_cast<int>(w) - hs;  // box spans x:[w-hs, w)
        const int y0 = 0;                          // y:[0, hs)  → TOP-right
        if (x0 < 0) return;
        const uint8_t baseA = active ? 200 : 120;  // fill alpha
        for (int dy = 0; dy < hs; ++dy) {
            const int y = y0 + dy;
            if (y < 0 || y >= static_cast<int>(h)) continue;
            for (int dx = 0; dx < hs; ++dx) {
                // Top-right corner triangle: keep points with dy <= dx
                // (right-angle vertex at the panel's top-right corner).
                if (dy > dx) continue;
                const int x = x0 + dx;
                if (x < 0 || x >= static_cast<int>(w)) continue;
                uint8_t* px = bgra + static_cast<size_t>(y) * stride + x * 4;
                // Diagonal grip stripes: bright white bands, measured from
                // the hypotenuse (dx == dy).
                const int band = dx - dy;
                if (band % 22 < 5) {
                    px[0] = 255; px[1] = 255; px[2] = 255; px[3] = 230;  // white grip
                } else {
                    px[0] = 255; px[1] = 180; px[2] = 60; px[3] = baseA;  // light-blue fill
                }
            }
        }
    }

    // Draw a yellow disc with a black outline at (cx, cy) onto an RGBA8 buffer.
    // This is the pointer indicator — visible exactly where the user's ray
    // intersects the panel, matching the pixel coordinates we forward as
    // mouse events to PrismaUI. The buffer is OUR converted RGBA copy;
    // PrismaUI's source bitmap is not touched, so the cursor draws ON TOP
    // of the page and doesn't persist into the next frame.
    void DrawCursor(uint8_t* rgba, uint32_t w, uint32_t h, int cx, int cy)
    {
        if (cx < 0 || cy < 0) return;
        const int R = kCursorOuterRadius;
        const int r = kCursorInnerRadius;
        const int R2 = R * R;
        const int r2 = r * r;
        for (int dy = -R; dy <= R; ++dy) {
            int y = cy + dy;
            if (y < 0 || y >= static_cast<int>(h)) continue;
            for (int dx = -R; dx <= R; ++dx) {
                int x = cx + dx;
                if (x < 0 || x >= static_cast<int>(w)) continue;
                int d2 = dx * dx + dy * dy;
                if (d2 > R2) continue;
                uint8_t* px = rgba + (static_cast<size_t>(y) * w + x) * 4;
                if (d2 < r2) {
                    // Inner fill: bright yellow, fully opaque so it's always
                    // visible regardless of page background.
                    px[0] = 255; px[1] = 220; px[2] = 0; px[3] = 255;
                } else {
                    // Outer ring: black, opaque — contrast against any color.
                    px[0] = 0; px[1] = 0; px[2] = 0; px[3] = 255;
                }
            }
        }
    }

    // Multiply two 3x4 transforms: result = a * b (translation included).
    // Used to compose device-world * tip-local -> tip-world.
    vr::HmdMatrix34_t Mul34(const vr::HmdMatrix34_t& a, const vr::HmdMatrix34_t& b) {
        vr::HmdMatrix34_t r{};
        for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 3; ++col) {
                r.m[row][col] = a.m[row][0] * b.m[0][col]
                              + a.m[row][1] * b.m[1][col]
                              + a.m[row][2] * b.m[2][col];
            }
            r.m[row][3] = a.m[row][0] * b.m[0][3]
                        + a.m[row][1] * b.m[1][3]
                        + a.m[row][2] * b.m[2][3]
                        + a.m[row][3];
        }
        return r;
    }

    // ---- Press tracking ---------------------------------------------------
    // To handle the "pressed on icon vs. pressed on panel" distinction, we
    // remember WHAT was hit at the moment the trigger edge-pressed. On
    // release we use this to decide whether to send a mouseUp to the panel.
    enum HitKind { Hit_None = 0, Hit_Panel = 1, Hit_Icon = 2, Hit_Resize = 3 };
    HitKind gPressedOn = Hit_None;
    int     gPressedIconSlot = Slot_None;

    // ---- In-game panel resize drag --------------------------------------
    // Set true while the user holds the trigger after grabbing the panel's
    // bottom-right corner handle. The gesture maps right→left controller
    // distance change to a scale delta (pull apart = bigger).
    bool    gResizing         = false;
    int     gResizeSlot       = Slot_None;
    float   gResizeStartDist  = 0.0f;   // right→left controller distance at grab
    float   gResizeStartScale = 1.0f;

    // ----------------------------------------------------------------------
    // Forward one SteamVR overlay-keyboard event to PrismaUI.
    //
    // cNewInput is 7 UTF-8 bytes + null (struct VREvent_Keyboard_t). The
    // SteamVR keyboard sends:
    //   - One printable Unicode codepoint as a 1-4 byte UTF-8 sequence
    //   - Control bytes (BS=0x08, TAB=0x09, LF=0x0A, CR=0x0D, ESC=0x1B) when
    //     the user taps those keys
    //
    // We use the *ToView exports added in our PrismaUI fork. The original
    // PrismaVR_DeliverChar / DeliverVKey are gated on PrismaUI owning the
    // VR keyboard target, which doesn't apply when WE own the overlay — they
    // silently drop the input. The *ToView versions take an explicit viewId
    // and bypass every gate, firing RawKeyDown + Char + KeyUp (printable) or
    // RawKeyDown + KeyUp with a proper key_identifier (control keys) on the
    // Ultralight thread for the view we specify.
    //
    // Control bytes route through DeliverVKeyToView so HTML inputs see real
    // virtual-key events (backspace deletes a char, Enter submits a form);
    // printable codepoints route through DeliverCharToView after a UTF-8 →
    // UTF-16 decode.
    // ----------------------------------------------------------------------
    void ForwardKeyboardInput(uint64_t viewId, const char* cNewInput)
    {
        if (viewId == 0 || !cNewInput) return;
        if ((!gDeliverCharToView || !gDeliverVKeyToView) && !gStockBuild) return;
        const uint8_t b0 = static_cast<uint8_t>(cNewInput[0]);
        if (b0 == 0) return;

        switch (b0) {
            case 0x08: if (gDeliverVKeyToView) gDeliverVKeyToView(viewId, 0x08); else InjectVKeyJS(viewId, 0x08); return;  // VK_BACK
            case 0x09: if (gDeliverVKeyToView) gDeliverVKeyToView(viewId, 0x09); else InjectVKeyJS(viewId, 0x09); return;  // VK_TAB
            case 0x0A: case 0x0D: if (gDeliverVKeyToView) gDeliverVKeyToView(viewId, 0x0D); else InjectVKeyJS(viewId, 0x0D); return;  // VK_RETURN
            case 0x1B: if (gDeliverVKeyToView) gDeliverVKeyToView(viewId, 0x1B); else InjectVKeyJS(viewId, 0x1B); return;  // VK_ESCAPE
            default: break;
        }

        // Printable codepoint — UTF-8 → UTF-16. cNewInput is null-terminated
        // (the struct guarantees byte[7] is 0), so MultiByteToWideChar with
        // length -1 is safe.
        wchar_t wide[8] = {};
        const int n = ::MultiByteToWideChar(CP_UTF8, 0, cNewInput, -1, wide, static_cast<int>(std::size(wide)));
        if (n <= 0) return;
        for (int i = 0; i < n; ++i) {
            if (wide[i] == 0) break;
            if (gDeliverCharToView) gDeliverCharToView(viewId, wide[i]);
            else                    InjectCharJS(viewId, wide[i]);
        }
    }

    void ProcessRaycast(vr::IVROverlay* overlay, uint32_t bmpW, uint32_t bmpH)
    {
        // Heartbeat
        static int hb = 0;
        const bool logThisTick = ((hb++ % 10) == 0);

        // NOTE: do NOT bail when gFireMouse is null. That export only matters
        // for forwarding clicks INTO a PrismaUI view; everything else this
        // function does — controller raycast, icon hit-testing, opening and
        // closing panels, the cursor dot, the resize handle — is pure OpenVR
        // and owes nothing to PrismaUI. Returning early here disabled the whole
        // pointer whenever a PrismaUI without the PrismaVR_* API was installed,
        // which also made the wrist icons unclickable. Every gFireMouse call
        // site is individually guarded (they sit behind `viewId != 0`, and no
        // view binds without the API), so leaving it null is safe.
        static bool loggedNoFireMouse = false;
        if (!gFireMouse && !loggedNoFireMouse) {
            SKSE::log::warn("gFireMouse unavailable — pointer, icons and panel still work; "
                            "only click-forwarding into a PrismaUI view is disabled.");
            loggedNoFireMouse = true;
        }
        auto* system = vr::VRSystem();
        if (!system) return;

        // Pointer hand = RIGHT controller (wrist menu is on LEFT).
        uint32_t pointerIdx = system->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_RightHand);
        if (pointerIdx == vr::k_unTrackedDeviceIndexInvalid) return;

        // Pointer pose in world space.
        vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount]{};
        system->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0.0f, poses, vr::k_unMaxTrackedDeviceCount);
        const auto& pose = poses[pointerIdx];
        if (!pose.bPoseIsValid || !pose.bDeviceIsConnected) return;
        const auto& m = pose.mDeviceToAbsoluteTracking;

        // Try to grab the manufacturer-intended "tip" component pose. This
        // gives the device-local transform for the controller's barrel/ray
        // emission point, which is what we actually want to raycast from.
        // Falls back gracefully if the runtime doesn't return a tip component.
        vr::HmdMatrix34_t tipWorld = m;  // default to raw device pose
        bool haveTip = false;
        if (auto* rm = vr::VRRenderModels()) {
            char rmName[256] = {};
            vr::ETrackedPropertyError propErr = vr::TrackedProp_Success;
            uint32_t rmLen = system->GetStringTrackedDeviceProperty(
                pointerIdx, vr::Prop_RenderModelName_String, rmName, sizeof(rmName), &propErr);
            if (propErr == vr::TrackedProp_Success && rmLen > 0) {
                vr::RenderModel_ControllerMode_State_t modeState{};
                vr::RenderModel_ComponentState_t compState{};
                if (rm->GetComponentState(rmName, "tip", nullptr, &modeState, &compState)) {
                    tipWorld = Mul34(m, compState.mTrackingToComponentLocal);
                    haveTip = true;
                }
            }
        }

        // Trigger axis (read once, used after intersection).
        vr::VRControllerState_t state{};
        const bool stateOk = system->GetControllerState(pointerIdx, &state, sizeof(state));
        const float trig = stateOk ? state.rAxis[1].x : 0.0f;

        // Candidate forward axes. Tested IN ORDER until one hits. Index of
        // first hit is cached in gPointerAxis so subsequent ticks skip
        // straight to the working axis.
        //   0 = tip -Z  (manufacturer-correct if tip-component lookup worked)
        //   1 = device -Z (Vive wand convention)
        //   2 = device -Y (some wand grips)
        //   3 = device +Y
        //   4 = device +Z
        //   5 = device -X
        //   6 = device +X
        struct Candidate { const char* name; vr::HmdVector3_t src; vr::HmdVector3_t dir; };
        Candidate axes[7] = {
            {"tip-Z", {tipWorld.m[0][3], tipWorld.m[1][3], tipWorld.m[2][3]},
                      {-tipWorld.m[0][2], -tipWorld.m[1][2], -tipWorld.m[2][2]}},
            {"dev-Z", {m.m[0][3], m.m[1][3], m.m[2][3]}, {-m.m[0][2], -m.m[1][2], -m.m[2][2]}},
            {"dev-Y", {m.m[0][3], m.m[1][3], m.m[2][3]}, {-m.m[0][1], -m.m[1][1], -m.m[2][1]}},
            {"dev+Y", {m.m[0][3], m.m[1][3], m.m[2][3]}, { m.m[0][1],  m.m[1][1],  m.m[2][1]}},
            {"dev+Z", {m.m[0][3], m.m[1][3], m.m[2][3]}, { m.m[0][2],  m.m[1][2],  m.m[2][2]}},
            {"dev-X", {m.m[0][3], m.m[1][3], m.m[2][3]}, {-m.m[0][0], -m.m[1][0], -m.m[2][0]}},
            {"dev+X", {m.m[0][3], m.m[1][3], m.m[2][3]}, { m.m[0][0],  m.m[1][0],  m.m[2][0]}},
        };

        // Test a (handle, axis) intersection. Rejects back-side hits so
        // the cursor and click logic only respond when the user is aimed
        // at the visible face of the overlay. OpenVR's vNormal points
        // out of the FRONT face, so a front-side ray (direction going
        // INTO the surface) has dot(ray_dir, normal) < 0; a back-side
        // ray (coming through the rear) has dot > 0.
        auto testOverlay = [&](vr::VROverlayHandle_t h, int axisIdx,
                               vr::VROverlayIntersectionResults_t& out) -> bool {
            if (h == vr::k_ulOverlayHandleInvalid) return false;
            vr::VROverlayIntersectionParams_t params{};
            params.eOrigin    = vr::TrackingUniverseStanding;
            params.vSource    = axes[axisIdx].src;
            params.vDirection = axes[axisIdx].dir;
            if (!overlay->ComputeOverlayIntersection(h, &params, &out)) return false;
            const float d = params.vDirection.v[0] * out.vNormal.v[0]
                          + params.vDirection.v[1] * out.vNormal.v[1]
                          + params.vDirection.v[2] * out.vNormal.v[2];
            if (d > 0.0f) return false;  // ray entered through the back face
            return true;
        };

        // Pick the axis to use. tip-Z if available, else cached, else scan.
        // Skip ICON hit-testing entirely when icons are hidden — otherwise the
        // player could "click" an invisible icon by aiming at where it would
        // be sitting. The PANEL is always testable when an active slot is
        // bound (it's always visible while a slot is active, by design).
        int axisToUse = -1;
        const bool iconsClickable = gIconsShownLast;
        auto firstHitOnAnyTarget = [&](int axisIdx) -> bool {
            vr::VROverlayIntersectionResults_t dummy{};
            if (iconsClickable) {
                for (int i = 0; i < Slot_Count; ++i) {
                    if (IconIsOff(i)) continue;   // hidden icon — not clickable
                    if (testOverlay(gIconOvls[i], axisIdx, dummy)) return true;
                }
            }
            if (gActiveSlot.load() != Slot_None && testOverlay(gPanelOvl, axisIdx, dummy)) return true;
            return false;
        };
        if (haveTip && firstHitOnAnyTarget(0)) {
            axisToUse = 0;
            if (gPointerAxis != 0) { gPointerAxis = 0; SKSE::log::info("Pointer axis locked to tip-Z (manufacturer-correct)"); }
        }
        if (axisToUse < 0 && gPointerAxis > 0 && gPointerAxis < 7 && firstHitOnAnyTarget(gPointerAxis)) {
            axisToUse = gPointerAxis;
        }
        if (axisToUse < 0) {
            for (int a = 0; a < 7; ++a) {
                if (firstHitOnAnyTarget(a)) {
                    axisToUse = a;
                    if (!haveTip && gPointerAxis < 0) {
                        gPointerAxis = a;
                        SKSE::log::info("Auto-detected forward axis (no tip): [{}]={}", a, axes[a].name);
                    }
                    break;
                }
            }
        }

        // Compute hits on each target with the chosen axis.
        vr::VROverlayIntersectionResults_t iconRes[Slot_Count]{};
        bool iconHit[Slot_Count] = { false, false, false };
        vr::VROverlayIntersectionResults_t panelRes{};
        bool panelHit = false;
        if (axisToUse >= 0) {
            if (iconsClickable) {
                for (int i = 0; i < Slot_Count; ++i) {
                    if (IconIsOff(i)) continue;   // hidden icon — not clickable
                    iconHit[i] = testOverlay(gIconOvls[i], axisToUse, iconRes[i]);
                }
            }
            if (gActiveSlot.load() != Slot_None) {
                panelHit = testOverlay(gPanelOvl, axisToUse, panelRes);
            }
        }

        // Pick closest hit. Icons take priority over panel only when they're
        // actually closer (so user can still hit the panel even if an icon
        // is geometrically on the ray's path further away).
        int   bestIcon = Slot_None;
        float bestIconDist = std::numeric_limits<float>::max();
        for (int i = 0; i < Slot_Count; ++i) {
            if (iconHit[i] && iconRes[i].fDistance < bestIconDist) {
                bestIcon = i;
                bestIconDist = iconRes[i].fDistance;
            }
        }
        // Apply the cursor-visibility proximity gate at the hit-decision
        // step so that ALL downstream interaction (cursor render, mouse
        // events, click toggles) is gated identically. If the pointer is
        // farther than kCursorMaxDistance, treat as no hit — no cursor,
        // no clicks, no hover events.
        const bool iconClose  = (bestIcon != Slot_None) && (bestIconDist           <= kCursorMaxDistance);
        const bool panelClose =  panelHit                 && (panelRes.fDistance    <= kCursorMaxDistance);
        HitKind hitKind = Hit_None;
        if (iconClose && (!panelClose || bestIconDist <= panelRes.fDistance)) {
            hitKind = Hit_Icon;
        } else if (panelClose) {
            hitKind = Hit_Panel;
        }

        if (logThisTick) {
            SKSE::log::info("Raycast tick: ptr={} haveTip={} axis={} icons=({},{},{}) panel={} kind={} trig={:.2f} bmp={}x{}",
                pointerIdx, haveTip, axisToUse,
                iconHit[0], iconHit[1], iconHit[2], panelHit, static_cast<int>(hitKind), trig, bmpW, bmpH);
        }

        const bool triggerEdgePress   = !gTriggerHeld && trig >= kTriggerPressThreshold;
        const bool triggerEdgeRelease =  gTriggerHeld && trig <= kTriggerReleaseThreshold;

        // ---- A button = right-click ------------------------------------
        // OpenVR's legacy button IDs put A at bit 7 on Touch-style controllers.
        // Button layouts differ per device, so ALSO log the first press of any
        // button we don't already know about: if A turns out to sit elsewhere on
        // this hardware, the log names the exact mask to use (and this is the
        // groundwork for making it MCM-bindable rather than hard-coded).
        constexpr uint64_t kMaskAButton = 1ull << 7;   // vr::k_EButton_A
        const uint64_t buttons   = stateOk ? state.ulButtonPressed : 0ull;
        const bool     aHeldNow  = (buttons & kMaskAButton) != 0;
        const bool     aEdgePress = aHeldNow && !gAButtonHeld;
        gAButtonHeld = aHeldNow;

        if (buttons && gIconsShownLast) {
            const uint64_t fresh = buttons & ~gLoggedButtons;
            if (fresh) {
                gLoggedButtons |= fresh;
                SKSE::log::info("Controller buttons pressed: mask={:#x} (new bits {:#x}) — "
                                "A/right-click expects {:#x}", buttons, fresh, kMaskAButton);
            }
        }

        // ---- Precompute panel pixel + resize-handle hit + gesture distance
        // Panel pixel under the ray (valid only when hitKind==Hit_Panel).
        int panelPx = -1, panelPy = -1;
        if (hitKind == Hit_Panel) {
            panelPx = static_cast<int>(panelRes.vUVs.v[0] * bmpW);
            panelPy = static_cast<int>((1.0f - panelRes.vUVs.v[1]) * bmpH);
            if (panelPx < 0) panelPx = 0; else if (panelPx >= (int)bmpW) panelPx = (int)bmpW - 1;
            if (panelPy < 0) panelPy = 0; else if (panelPy >= (int)bmpH) panelPy = (int)bmpH - 1;
        }
        // Top-right grab-handle region, in overlay UV space (resolution-
        // independent). v[0]=U (1 = right edge), v[1]=V (1 = TOP edge).
        const bool onResizeHandle = (hitKind == Hit_Panel)
            && panelRes.vUVs.v[0] > (1.0f - kResizeHandleFrac)
            && panelRes.vUVs.v[1] > (1.0f - kResizeHandleFrac);
        // Right→left controller distance for the resize gesture.
        float ctrlDist = 0.0f;
        if (gLeftCtrlIdx != vr::k_unTrackedDeviceIndexInvalid
            && gLeftCtrlIdx < vr::k_unMaxTrackedDeviceCount
            && poses[gLeftCtrlIdx].bPoseIsValid) {
            const auto& lm = poses[gLeftCtrlIdx].mDeviceToAbsoluteTracking;
            const float ddx = m.m[0][3] - lm.m[0][3];
            const float ddy = m.m[1][3] - lm.m[1][3];
            const float ddz = m.m[2][3] - lm.m[2][3];
            ctrlDist = std::sqrt(ddx * ddx + ddy * ddy + ddz * ddz);
        }

        // ---- Press edge: decide what got grabbed/clicked ----------------
        if (triggerEdgePress) {
            if (onResizeHandle) {
                // Start a RESIZE DRAG instead of forwarding a click.
                gResizing         = true;
                gResizeSlot       = gActiveSlot.load();
                gResizeStartScale = (gResizeSlot >= 0 && gResizeSlot < Slot_Count)
                                    ? gSlotScale[gResizeSlot] : 1.0f;
                gResizeStartDist  = ctrlDist;
                gPressedOn        = Hit_Resize;
                SKSE::log::info("Resize drag START: slot {} startScale {:.3f} dist {:.3f}",
                    gResizeSlot, gResizeStartScale, ctrlDist);
            } else if (hitKind == Hit_Icon) {
                // Toggle slot. Clicking the active icon deactivates; clicking
                // a different icon swaps to it.
                int cur = gActiveSlot.load();
                int nxt = (cur == bestIcon) ? Slot_None : bestIcon;
                gActiveSlot.store(nxt);
                gPressedOn = Hit_Icon;
                gPressedIconSlot = bestIcon;
                SKSE::log::info("Icon[{}] clicked: slot {} -> {}", bestIcon, cur, nxt);
                if (smf::SessionActive() && (nxt < 0 || nxt >= Slot_Count || !gSlotIsSmf[nxt]))
                    smf::EndSession();   // switched away from the SMF slot
            } else if (hitKind == Hit_Panel) {
                // Forward mouseDown to the slot's CURRENTLY-DISPLAYED view.
                int activeSlot = gActiveSlot.load();
                uint64_t viewId = (activeSlot >= 0 && activeSlot < Slot_Count) ? gSlotActiveViewIds[activeSlot] : 0;
                if (viewId != 0 && panelPx >= 0 && !gFireMouse && gStockBuild) {
                    // Addon mode: synthesise the DOM event through the public API.
                    InjectPointerEvent(viewId, "down", panelPx, panelPy, bmpW, bmpH);
                    SKSE::log::info("Panel click DOWN (addon/JS) at ({},{})", panelPx, panelPy);
                }
                if (viewId != 0 && panelPx >= 0 && gFireMouse) {
                    gFireMouse(viewId, 1 /*down*/, panelPx, panelPy, 1 /*left*/);
                    SKSE::log::info("Panel click DOWN at ({},{}) trig={:.2f}", panelPx, panelPy, trig);
                }
                if (viewId == 0 && activeSlot >= 0 && gSlotIsSmf[activeSlot] &&
                    smf::SessionActive() && panelPx >= 0) {
                    smf::PointerMove(panelPx, panelPy);
                    smf::PointerButton(true);
                    SKSE::log::info("Panel click DOWN (SMF native) at ({},{})", panelPx, panelPy);
                }
                gPressedOn = Hit_Panel;
            }
            gTriggerHeld = true;
        }

        // ---- While trigger held -----------------------------------------
        if (gResizing) {
            // Map controller-distance delta → scale (pull apart = bigger).
            // Live update PHYSICAL size only; the expensive HTML reflow is
            // deferred to release (see triggerEdgeRelease below).
            if (gResizeSlot >= 0 && gResizeSlot < Slot_Count) {
                float ns = gResizeStartScale + (ctrlDist - gResizeStartDist) * kResizeSensitivity;
                if (ns < kPanelZoomMin) ns = kPanelZoomMin;
                if (ns > kPanelZoomMax) ns = kPanelZoomMax;
                gSlotScale[gResizeSlot] = ns;
                ApplyZoom(gResizeSlot, overlay);  // physical zoom only
            }
            gHadHitLast = false;  // suppress the panel cursor while resizing
            HideCursor(overlay);
        } else if (hitKind == Hit_Panel) {
            // Proximity already enforced by hitKind (kCursorMaxDistance gate).
            int activeSlot = gActiveSlot.load();
            uint64_t viewId = (activeSlot >= 0 && activeSlot < Slot_Count) ? gSlotActiveViewIds[activeSlot] : 0;
            if (viewId != 0 && panelPx >= 0 && gFireMouse) gFireMouse(viewId, 0 /*move*/, panelPx, panelPy, 0 /*none*/);
            else if (viewId != 0 && panelPx >= 0 && gStockBuild) {
                // Addon mode hover: only when the pointer actually moved to a
                // new pixel, so we don't queue a JS eval every single tick.
                if (panelPx != gLastPixelX || panelPy != gLastPixelY)
                    InjectPointerEvent(viewId, "move", panelPx, panelPy, bmpW, bmpH);
            }

            // A button while pointing at the panel = right-click.
            if (aEdgePress && viewId != 0 && panelPx >= 0 && gStockBuild) {
                InjectPointerEvent(viewId, "rclick", panelPx, panelPy, bmpW, bmpH);
                SKSE::log::info("Panel RIGHT-click (A button) at ({},{})", panelPx, panelPy);
            }
            if (viewId == 0 && gSlotIsSmf[activeSlot] && smf::SessionActive() && panelPx >= 0) {
                smf::PointerMove(panelPx, panelPy);
            }
            gLastPixelX    = panelPx;
            gLastPixelY    = panelPy;
            gLastHitU      = panelRes.vUVs.v[0];   // for the resolution-safe cursor draw
            gLastHitV      = panelRes.vUVs.v[1];
            gLastHitPoint  = panelRes.vPoint;
            gLastHitNormal = panelRes.vNormal;
            gHadHitLast    = true;  // bitmap cursor draws on next panel submit
            // SMF native panels have no CPU bitmap to composite the yellow dot
            // into, so reuse the world-space cursor overlay (the same dot the
            // icons use) at the raycast hit point instead.
            if (activeSlot >= 0 && activeSlot < Slot_Count && gSlotIsSmf[activeSlot] && smf::SessionActive())
                ShowCursorAt(overlay, system, panelRes.vPoint, 0.0025f);
            else
                HideCursor(overlay);
        } else if (hitKind == Hit_Icon) {
            gHadHitLast = false;
            ShowCursorAt(overlay, system, iconRes[bestIcon].vPoint);
        } else {
            gHadHitLast = false;
            HideCursor(overlay);
        }
        gIconHitLast = (hitKind == Hit_Icon) ? bestIcon : Slot_None;

        // ---- Release edge: send mouseUp to panel if press was on panel --
        if (triggerEdgeRelease) {
            if (gResizing) {
                // Finalize the ZOOM (physical only — resolution/content is the
                // MCM picker's job, not the handle's). Just re-assert the
                // physical size at the final zoom factor.
                if (gResizeSlot >= 0 && gResizeSlot < Slot_Count) {
                    ApplyZoom(gResizeSlot, overlay);
                    SKSE::log::info("Resize drag END: slot {} finalZoom {:.3f}",
                        gResizeSlot, gSlotScale[gResizeSlot]);
                }
                gResizing   = false;
                gResizeSlot = Slot_None;
            } else if (gPressedOn == Hit_Panel && gLastPixelX >= 0) {
                int activeSlot = gActiveSlot.load();
                uint64_t viewId = (activeSlot >= 0 && activeSlot < Slot_Count) ? gSlotActiveViewIds[activeSlot] : 0;
                if (viewId != 0 && !gFireMouse && gStockBuild) {
                    InjectPointerEvent(viewId, "up", gLastPixelX, gLastPixelY, bmpW, bmpH);
                    SKSE::log::info("Panel click UP (addon/JS) at ({},{})", gLastPixelX, gLastPixelY);
                }
                if (viewId != 0 && gFireMouse) {
                    gFireMouse(viewId, 2 /*up*/, gLastPixelX, gLastPixelY, 1 /*left*/);
                    SKSE::log::info("Panel click UP at ({},{}) trig={:.2f}", gLastPixelX, gLastPixelY, trig);
                }
            }
            if (gPressedOn == Hit_Panel) smf::PointerButton(false);   // harmless no-op for PrismaUI slots
            gPressedOn = Hit_None;
            gPressedIconSlot = Slot_None;
            gTriggerHeld = false;
        }

        // ---- Joystick → panel scroll ---------------------------------
        // Only active while the cursor is on the PANEL (cursor visibility
        // already gated the hitKind by proximity, so this is implicit-safe).
        // Vertical axis drives mouse-wheel scroll. Horizontal-axis arrow
        // key forwarding was REMOVED — it conflicted with tab-list
        // keyboard nav inside SN/SA/IE pages, causing accidental tab
        // switches and 10 Hz-rate UI lag overlapping player movement.
        if (stateOk && hitKind == Hit_Panel && !gResizing) {
            const int activeSlot = gActiveSlot.load();
            if (activeSlot >= 0 && activeSlot < Slot_Count && gSlotIsSmf[activeSlot] && smf::SessionActive()) {
                const float jySmf = state.rAxis[kArrowKeyAxis].y;
                smf::SetScrollStick(std::fabs(jySmf) > kJoystickDeadzone ? jySmf : 0.0f);
            }
            const uint64_t viewId = (activeSlot >= 0 && activeSlot < Slot_Count) ? gSlotActiveViewIds[activeSlot] : 0;
            if (viewId != 0) {
                const float jy = state.rAxis[kArrowKeyAxis].y;
                // Scroll: |jy| past deadzone produces a pixel-delta this tick.
                // Sign convention mirrors PrismaUI's own laser-scroll path:
                // pushing the stick UP scrolls the page DOWN (negative delta_y).
                if (!gFireScrollToView && gStockBuild && std::fabs(jy) > kJoystickDeadzone) {
                    const int dy = static_cast<int>(jy * -kScrollPixelsPerTick);
                    if (dy != 0) InjectScroll(viewId, panelPx, panelPy, dy, bmpW, bmpH);
                }
                if (gFireScrollToView && std::fabs(jy) > kJoystickDeadzone) {
                    const int dy = static_cast<int>(jy * -kScrollPixelsPerTick);
                    if (dy != 0) gFireScrollToView(viewId, 0, dy);
                }
            }
        }
    }

    // ----------------------------------------------------------------------
    // Pump loop (Stage 2.3 — wrist menu).
    //
    // Each tick at 10 Hz:
    //  1) Cache left controller index. Bind any unbound slot view IDs from
    //     the current PrismaUI view enumeration.
    //  2) Detect palm-down. If false: hide everything, skip the rest.
    //  3) Anchor icons + panel to the left controller (transform-only).
    //  4) Show icons (state edge).
    //  5) Run raycast — sets active slot if user clicked an icon, forwards
    //     mouse events to the active panel if any.
    //  6) If no active slot, hide the panel and skip the bitmap work.
    //  7) Otherwise fetch the panel's view bitmap (if a new frame is ready
    //     or the cursor moved), draw the cursor on top, SetOverlayRaw, show.
    // ----------------------------------------------------------------------
    void PumpLoop()
    {
        SKSE::log::info("Pump thread starting (10 Hz, gated on save-loaded)");
        gPumpRunning = true;

        auto* overlay = vr::VROverlay();
        auto* system  = vr::VRSystem();

        bool everShownPanel = false;
        int  rediscoverCountdown = 0;
        int  mcmPollCountdown = 10;   // live-poll the MCM settings INI ~1/s
        bool loggedNoViews = false;
        uint64_t lastCapSeq = 0;   // addon-mode: last captured-frame sequence consumed
        int  loggedLiveViewCount = -1;  // dedup the verbose "Live PrismaUI views" log

        std::vector<uint8_t> rgbaBuf;
        std::vector<uint8_t> bgraBuf;

        // Per-tick cursor / submission state (kept across iterations).
        int  lastSubmitCursorX = -2;
        int  lastSubmitCursorY = -2;
        auto lastCursorSubmit  = std::chrono::steady_clock::time_point{};
        int      lastActiveSlotSeen   = Slot_None;
        // Tracks which VIEW (within the active slot's family) we were
        // displaying last tick. When a sister mod opens a sub-view (e.g.
        // SeverActions tab → SeverActionsDiary) UpdateActiveViewsForSlots
        // promotes that view to gSlotActiveViewIds[slot]; this mismatch
        // triggers the same "force fresh frame" reset as a slot transition.
        uint64_t lastActiveViewIdSeen = 0;
        int  consecutiveSORErr = 0;
        auto nextSORAttempt = std::chrono::steady_clock::time_point{};

        // F12 edge detector for triggering calibration mode. GetAsyncKeyState
        // bypasses Skyrim's input system so the key works regardless of
        // game focus / menu state. F12 is rarely bound elsewhere in Skyrim.
        bool wasF12Down = false;

        // Snapshot the MCM settings already in effect from the initial bind so
        // the live-poll below only reacts to genuine mid-session changes (no
        // spurious re-bind on the first poll).
        ReadResolution();    gAppliedResolutionIndex = gResolutionIndex;
        ReadIconBindings(); for (int i = 0; i < Slot_Count; ++i) gAppliedIconBind[i] = gIconBind[i];

        while (gPumpRunning.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (!gGameLoaded.load()) continue;
            if (!overlay || !system) continue;
            if (gPanelOvl == vr::k_ulOverlayHandleInvalid) continue;

            // --- F12 trigger: start calibration sequence ------------------
            const bool nowF12Down = (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
            if (nowF12Down && !wasF12Down && !gCalibrationMode.load()) {
                gCalibrationMode.store(true);
                gCalibrationStart = std::chrono::steady_clock::now();
                SKSE::log::info("=== CALIBRATION SEQUENCE STARTED (F12) ===");
                SKSE::log::info("9 phases × 3 s each = 27 s. Watch the SN icon, each phase logged.");
            }
            wasF12Down = nowF12Down;

            // --- Calibration setup (fall-through to normal flow) ----------
            // We no longer hijack the entire pump for calibration. Instead
            // we compute which phase we're in (if any), let the normal flow
            // run, and then OVERRIDE the panel transform AFTER AnchorAll
            // with the phase animation matrix. That way the panel content
            // (PrismaUI bitmap, cursor, raycast, clicks) keeps working
            // normally while the panel ITSELF translates / rotates through
            // the 9 axes so the user can visualize where it should sit.
            gLeftCtrlIdx = system->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_LeftHand);
            const CalPhase* curCalPhase = nullptr;
            float curCalPhaseT = 0.0f;
            if (gCalibrationMode.load()) {
                const auto elapsed = std::chrono::duration_cast<std::chrono::duration<float>>(
                    std::chrono::steady_clock::now() - gCalibrationStart).count();
                if (elapsed >= kCalTotalDuration) {
                    gCalibrationMode.store(false);
                    SKSE::log::info("=== CALIBRATION COMPLETE — returning to normal panel positioning ===");
                } else {
                    const int   phaseIdx = static_cast<int>(elapsed / kCalPhaseDuration_s);
                    curCalPhaseT = (elapsed - phaseIdx * kCalPhaseDuration_s) / kCalPhaseDuration_s;
                    curCalPhase = &kCalPhases[phaseIdx];

                    static int lastLoggedPhase = -1;
                    if (phaseIdx != lastLoggedPhase) {
                        SKSE::log::info("--- Panel calibration phase {} ---", curCalPhase->label);
                        lastLoggedPhase = phaseIdx;
                    }

                    // Auto-activate first valid slot so panel content renders
                    // during calibration. Without this, an unset slot leaves
                    // the panel hidden and the user can't see what's moving.
                    if (gActiveSlot.load() == Slot_None) {
                        for (int i = 0; i < Slot_Count; ++i) {
                            if (gSlotViewIds[i] != 0) {
                                gActiveSlot.store(i);
                                SKSE::log::info("Calibration: auto-activated slot {}", i);
                                break;
                            }
                        }
                    }
                }
            }

            // --- 0) Live-apply MCM settings (resolution + icon bindings) ---
            // MCM Helper writes Data/MCM/Settings/PrismaUIWristOverlays.ini whenever
            // the user changes a setting in the menu. Poll it ~1/s (read
            // directly, bypassing PrivateProfileRedirector's stale startup
            // cache) and apply changes live so the user sees them with no
            // relaunch.
            if (--mcmPollCountdown <= 0) {
                mcmPollCountdown = 10;  // ~1 s at the 100 ms tick

                // Resolution: re-read; on change, resize every bound slot's
                // active view to the new preset.
                ReadResolution();
                if (gResolutionIndex != gAppliedResolutionIndex) {
                    SKSE::log::info("MCM live: resolution {} -> {} ({}); applying to bound slots",
                        gAppliedResolutionIndex, gResolutionIndex, kResPresets[gResolutionIndex].name);
                    gAppliedResolutionIndex = gResolutionIndex;
                    for (int i = 0; i < Slot_Count; ++i)
                        if (gSlotViewIds[i] != 0) ApplyResolution(i);
                }

                // Icon bindings: re-read; for any slot whose override changed,
                // reset it (clear primary + family) so the rediscover/bind path
                // below immediately re-binds the icon to the new overlay.
                // gSlotActiveViewIds[i] is intentionally left pointing at the
                // old view so the panel keeps showing it until the re-bind
                // completes on the next tick.
                ReadIconBindings();
                for (int i = 0; i < Slot_Count; ++i) {
                    if (_stricmp(gIconBind[i].c_str(), gAppliedIconBind[i].c_str()) == 0) continue;
                    const std::string prev = gAppliedIconBind[i];
                    const std::string now  = gIconBind[i];
                    const bool prevOff = _stricmp(prev.c_str(), "Off") == 0;
                    SKSE::log::info("MCM live: icon slot {} binding '{}' -> '{}'", i, prev, now);
                    gAppliedIconBind[i] = now;
                    // Hide the outgoing primary BEFORE un-claiming it. Binding
                    // force-Show()ed it so stock PrismaUI keeps painting it for
                    // the wrist panel, with only the claim suppressing the flat
                    // blit. Resetting the slot rebuilds the claim list without
                    // it, so a still-shown view would be blitted across the
                    // game window permanently (the "SeverActions stuck on my
                    // flat screen" bug when an icon was turned Off). Hide()
                    // returns it to sister-mod-managed visibility — the mod
                    // re-Shows it whenever IT wants it on screen.
                    const uint64_t prevPrimary = gSlotViewIds[i];
                    gSlotViewIds[i] = 0;
                    gSlotFamilyViewIds[i].clear();
                    gSlotFamilyPrefixActive[i].clear();
                    if (prevPrimary != 0 && gPrismaUI) {
                        bool primaryElsewhere = false;
                        for (int k = 0; k < Slot_Count; ++k)
                            if (k != i && gSlotViewIds[k] == prevPrimary) primaryElsewhere = true;
                        if (!primaryElsewhere) {
                            gPrismaUI->Hide(prevPrimary);
                            SKSE::log::info("Slot[{}] reset: hid outgoing primary view {} "
                                            "(no longer claimed — would flat-draw otherwise)",
                                            i, prevPrimary);
                        }
                    }
                    rediscoverCountdown = 0;  // re-bind on the next tick
                    if (IconIsOff(i)) {
                        // Turned OFF: hide this icon immediately + dismiss its panel if it was active.
                        if (gIconOvls[i] != vr::k_ulOverlayHandleInvalid) overlay->HideOverlay(gIconOvls[i]);
                        if (gActiveSlot.load() == i) gActiveSlot.store(Slot_None);
                    } else if (prevOff && gIconsShownLast) {
                        // Turned back ON while the wrist menu is showing: reveal it now.
                        if (gIconOvls[i] != vr::k_ulOverlayHandleInvalid) overlay->ShowOverlay(gIconOvls[i]);
                    }
                }
            }

            // --- 1) Left controller + slot view bindings ------------------

            // Rediscover views every ~2s — ALWAYS, even once all slots are bound.
            // Beyond the initial bind this does two things: (a) discovers a NEW
            // sibling a sister mod registers seconds into play (e.g. opening a
            // SeverActions sub-prompt) so the panel can display it AND its focus
            // tracker gets armed; (b) re-arms every family member's focus tracker
            // (auto-heals a view whose page reloaded). Binding is idempotent —
            // Pass 1 skips already-bound slots — so repeating it is safe. The
            // verbose per-view log is gated on a count change so it doesn't spam.
            // SMF source: keep retrying the helper handshake and client
            // discovery — the helper may register after us, and SMF only
            // gets a panel once it has drawn its first frame.
            smf::EnsureNative();

            if (--rediscoverCountdown <= 0) {
                rediscoverCountdown = 20;  // 2s
                auto views = EnumerateViews();
                if (views.empty()) {
                    if (!loggedNoViews) {
                        SKSE::log::info("No PrismaUI views yet — retrying every 2s");
                        loggedNoViews = true;
                    }
                } else {
                    loggedNoViews = false;
                    if (static_cast<int>(views.size()) != loggedLiveViewCount) {
                        SKSE::log::info("==== Live PrismaUI views ({} total) ====", views.size());
                        for (const auto& v : views) SKSE::log::info("  id={}  url={}", v.id, v.url);
                        loggedLiveViewCount = static_cast<int>(views.size());
                    }
                    BindSlotsToViews(views);
                    gQuadViewCache = views;   // feeds the per-tick quad suppressor
                }
            }

            // --- 1a½) 1.5+: keep PrismaUI's own floating VR quads hidden.
            // EVERY tick, not just on discovery — their sync recreates a quad
            // the moment a view is (re)shown, which is exactly what our own
            // panel-open flow does, and a 2s sweep let it float visibly.
            HideNativePrismaQuads();

            // --- 1b) Refresh active-view-per-slot (Option B follow path) ---
            // Walk each slot's family; for any sibling that's currently
            // !IsHidden (sister mod did Show() on it, e.g. user clicked a
            // tab) promote it to gSlotActiveViewIds[slot] so the wrist
            // panel displays the new view and downstream consumers (bitmap
            // fetch, click forwarding, keyboard) target it.
            UpdateActiveViewsForSlots();

            // --- 1c) Publish the CLAIMED view set (addon mode) ------------
            // Every view we render on the wrist — each slot's primary plus its
            // discovered family — is claimed so PrismaUI's DrawSingleTexture
            // hook skips blitting it across the flat screen. Rebuilt each tick
            // (cheap: <= 24 relaxed stores) so it tracks binds and re-binds.
            if (gStockBuild) {
                int c = 0;
                for (int i = 0; i < Slot_Count && c < kMaxClaimed; ++i) {
                    if (gSlotViewIds[i] != 0) gClaimedViews[c++].store(gSlotViewIds[i], std::memory_order_relaxed);
                    for (uint64_t fid : gSlotFamilyViewIds[i]) {
                        if (c >= kMaxClaimed) break;
                        if (fid != 0 && fid != gSlotViewIds[i])
                            gClaimedViews[c++].store(fid, std::memory_order_relaxed);
                    }
                }
                gClaimedCount.store(c, std::memory_order_release);
            }

            // --- 2) Look-at-wrist detection (gates ICONS only now) --------
            // The PANEL deliberately stays visible whenever a slot is active
            // so the player can keep tracking it through combat, looking
            // around, etc. — it's an explicit "I want this open" choice and
            // is only dismissed by clicking its icon. Icons themselves still
            // hide when not looked at so they don't clutter the periphery.
            // Icon-visibility gate has TWO required conditions (both must
            // be true to surface the icons):
            //   1. Player is looking at the wrist (DetectLookAtWrist cone)
            //   2. Hand is in the wristwatch-look pose (fingertips aimed
            //      at the player's right, within a 30° cone)
            // Calibration bypasses both so the user can see what they're
            // tuning even with the controller in any pose.
            const bool lookingAtWrist  = (curCalPhase != nullptr)
                ? true
                : DetectLookAtWrist(gLeftCtrlIdx, system);
            float wristwatchPoseDot = 0.0f;
            const bool wristwatchPose  = (curCalPhase != nullptr)
                ? true
                : DetectWristwatchPose(gLeftCtrlIdx, system, &wristwatchPoseDot);
            const bool iconsShouldShow = lookingAtWrist && wristwatchPose;

            // --- 3) Anchor everything to the left controller --------------
            // We always re-anchor so the panel + icon transforms track the
            // wrist even when icons are hidden. Cost is trivial.
            AnchorAllToLeftController(overlay, gLeftCtrlIdx);

            // --- 3b) Calibration: override panel transform with phase ----
            // Done AFTER the normal anchor so the override is the final
            // transform applied to the panel. Icons stay where AnchorAll
            // put them.
            if (curCalPhase != nullptr) {
                OverridePanelCalibrationTransform(overlay, gLeftCtrlIdx, *curCalPhase, curCalPhaseT);
            }

            // --- 3c) Icon visibility (state edge) -------------------------
            // Show icons only when BOTH look-at-wrist and wristwatch-pose
            // are satisfied. Panel visibility is decided later (independent
            // of look direction / pose).
            if (iconsShouldShow) {
                if (!gIconsShownLast) {
                    for (int i = 0; i < Slot_Count; ++i) {
                        if (gIconOvls[i] == vr::k_ulOverlayHandleInvalid) continue;
                        if (IconIsOff(i))
                            overlay->HideOverlay(gIconOvls[i]);   // OFF icon stays hidden
                        else
                            overlay->ShowOverlay(gIconOvls[i]);
                    }
                    gIconsShownLast = true;
                    SKSE::log::info("Wrist menu surfaced — icons visible (look + watch-pose)");
                }
            } else {
                if (gIconsShownLast) {
                    for (int i = 0; i < Slot_Count; ++i) {
                        if (gIconOvls[i] != vr::k_ulOverlayHandleInvalid) overlay->HideOverlay(gIconOvls[i]);
                    }
                    gIconsShownLast = false;
                    SKSE::log::info("Icons hidden (look={}, pose={} dot={:.2f}; panel stays if active)",
                        lookingAtWrist, wristwatchPose, wristwatchPoseDot);
                }
                HideCursor(overlay);
            }

            // --- 5) Raycast + click forwarding ----------------------------
            // We need bitmap dimensions for the panel UV→pixel math even
            // when no panel is active (so a click going from no-panel to
            // panel-just-shown still computes correctly). Use a sane default
            // when no view is bound yet.
            const int activeSlot = gActiveSlot.load();
            const uint64_t activeViewId = (activeSlot >= 0 && activeSlot < Slot_Count) ? gSlotActiveViewIds[activeSlot] : 0;

            uint32_t w = 1920, h = 1080, stride = 1920 * 4;
            if (activeViewId != 0 && gGetBitmap) {
                // Fork mode: ask PrismaUI for the view's current bitmap size.
                gGetBitmap(activeViewId, nullptr, 0, &w, &h, &stride);
                if (w == 0 || h == 0) { w = 1920; h = 1080; stride = 1920 * 4; }
            } else if (activeViewId != 0 && gStockBuild) {
                // Addon mode: there is no size-query export, so seed the
                // dimensions from the last captured frame. ProcessRaycast needs
                // the REAL view size for its UV->pixel math; stock creates views
                // at the game's screen size and we cannot resize them, so
                // assuming 1920x1080 would skew every pointer coordinate on any
                // user whose window is not exactly 1080p.
                const uint32_t cw = gCapW.load(std::memory_order_relaxed);
                const uint32_t ch = gCapH.load(std::memory_order_relaxed);
                const uint32_t cs = gCapStride.load(std::memory_order_relaxed);
                if (cw && ch && cs) { w = cw; h = ch; stride = cs; }
            }

            // SMF slots map the pointer against SMF's own layout space, not the
            // PrismaUI bitmap size -- see the v1.4.7 note in namespace smf.
            if (activeSlot >= 0 && activeSlot < Slot_Count && gSlotIsSmf[activeSlot] &&
                smf::RtReady()) {
                w = smf::RtW(); h = smf::RtH(); stride = w * 4;
            }
            ProcessRaycast(overlay, w, h);

            // --- 5a½) Right-stick guard, gated on the yellow dot ----------
            // gHadHitLast is this tick's "pointer is on the panel" state —
            // exactly the state in which the right stick scrolls the page. A
            // short linger swallows the frames where a scroll flick slides
            // off the panel edge mid-gesture. Calling every tick is the
            // point: the steady-state path inside SetStickSuppressed heals
            // any external re-enable of the masked flags.
            if (gHadHitLast) gStickLingerTicks = kStickLingerHold;
            else if (gStickLingerTicks > 0) --gStickLingerTicks;
            SetStickSuppressed(gStickLingerTicks > 0);

            // --- 5b) Auto-summon SteamVR keyboard on text-input focus -----
            // EVERY family view has a JS focus tracker (see InstallFocusTracker).
            // When the user clicks into an input/textarea/contentEditable, that
            // view reports its own id into gFocusedTextInputView. We summon the
            // keyboard only when the reported view is the one currently being
            // DISPLAYED by the active slot (gSlotActiveViewIds[activeSlot]) — so
            // a text input in whichever sibling the panel shows works, while a
            // hidden/background/orphaned view can never drive it. Detection view
            // == delivery view (chars go to the same gSlotActiveViewIds below).
            //
            // Rising edge (no-focus → focus): show the keyboard. Falling edge or
            // panel dismissal: hide it. gKbdAutoSuppressed is set when the user
            // X's out of the keyboard manually so we don't re-summon while they
            // stay focused on the same input; it clears on the next rising edge.
            const int  kbdSlotActive   = gActiveSlot.load();
            const bool kbdPanelActive  = (kbdSlotActive != Slot_None) &&
                                         (kbdSlotActive >= 0 && kbdSlotActive < Slot_Count &&
                                          gSlotViewIds[kbdSlotActive] != 0);
            const uint64_t kbdActiveView = kbdPanelActive ? gSlotActiveViewIds[kbdSlotActive] : 0;
            const bool focusedNow      = kbdPanelActive && kbdActiveView != 0 &&
                                         gFocusedTextInputView.load() == kbdActiveView;
            const bool focusRisingEdge = focusedNow && !gFocusLastForActiveSlot;
            if (focusRisingEdge) gKbdAutoSuppressed = false;
            gFocusLastForActiveSlot = focusedNow;

            if (focusedNow && !gKeyboardOpen && !gKbdAutoSuppressed &&
                ((gDeliverCharToView && gDeliverVKeyToView) || gStockBuild))
            {
                // ★★ FLAGS MUST BE 0 — never KeyboardFlag_Minimal.
                //
                // SteamVR's system keyboard is a SINGLETON owned by vrserver, not
                // by the calling process. KeyboardFlag_Minimal switches that shared
                // keyboard to "send keys immediately, accumulate no buffer", which
                // (a) removes the text-preview band along its top and (b) makes
                // GetKeyboardText() return an empty string.
                //
                // That broke every OTHER mod's text entry system-wide (reported
                // 2026-09-07: AddItemMenu VR + RaceMenu). Those mods follow the
                // normal contract — show the keyboard, wait for Done, then read the
                // accumulated buffer — so an empty buffer means their Papyrus poll
                // loop (VRKeyboard.psc: `while ! resultText`) never exits. And
                // because the mode lives in vrserver, DISABLING our mod does not
                // undo it; only a SteamVR restart or a normal-mode show does.
                //
                // 0 = the default, well-behaved keyboard: band visible, buffer kept.
                // We still receive VREvent_KeyboardCharInput per keystroke, and the
                // Done handler below also reads the whole buffer as a fallback, so
                // our own typing works in either mode.
                auto err = overlay->ShowKeyboardForOverlay(
                    gPanelOvl,
                    vr::k_EGamepadTextInputModeNormal,
                    vr::k_EGamepadTextInputLineModeSingleLine,
                    vr::KeyboardFlag_Modal /* see the 2026-09-07 note below */,
                    "PrismaUI",
                    256,
                    "",
                    kKbdUserValue);
                if (err == vr::VROverlayError_None) {
                    gKeyboardOpen = true;
                    gKbdCharsThisSession = 0;
                    gKbdRealCharsThisSession = 0;
                    gKbdLastLiveText.clear();
                    gKbdHarvested.clear();
                    SKSE::log::info("Keyboard: auto-summoned for slot {} (input focused)",
                        kbdSlotActive);
                } else {
                    SKSE::log::error("ShowKeyboardForOverlay failed: {} ({})",
                        static_cast<int>(err), overlay->GetOverlayErrorNameFromEnum(err));
                }
            }
            if (!focusedNow && gKeyboardOpen) {
                overlay->HideKeyboard();
                gKeyboardOpen = false;
                SKSE::log::info("Keyboard: hidden (input lost focus)");
            }

            // --- 5c) Drain panel-overlay events for keyboard input ---------
            // PollNextOverlayEvent is scoped to gPanelOvl, so we don't
            // disturb anything else in the process that might poll the global
            // event stream via PollNextEvent. ShowKeyboardForOverlay routes
            // keyboard events to the targeted overlay's queue.
            //
            // ★★ ONLY drain while WE actually have a keyboard session open.
            //
            // This used to run every tick unconditionally, and that broke OTHER
            // mods' text entry (reported 2026-09-06: AddItemMenu VR accepted no
            // typed text). Once we have called ShowKeyboardForOverlay(gPanelOvl)
            // even once, SteamVR keeps routing system-keyboard events to that
            // overlay's queue; draining it while idle meant we SWALLOWED
            // characters meant for whoever summoned the keyboard next, and then
            // threw them away (ForwardKeyboardInput returns immediately on a
            // zero target view). Silent, total input loss for the other mod.
            //
            // Not polling is the correct fix rather than filtering: an event we
            // never consume stays in the queue for its rightful owner.
            if (gKeyboardOpen && gPanelOvl != vr::k_ulOverlayHandleInvalid) {
                // Keyboard events route to the view of the slot active when
                // the keyboard was summoned. If the user switches slots mid-
                // typing we still send to the current active slot — but the
                // focus tracker will hide the kbd anyway on the slot switch
                // since the new slot's text-focus flag won't be set.
                const uint64_t kbdTargetView =
                    (kbdSlotActive >= 0 && kbdSlotActive < Slot_Count) ? gSlotActiveViewIds[kbdSlotActive] : 0;
                // --- LIVE BUFFER PROBE (2026-09-07) -------------------------
                // The user reports the SteamVR keyboard clicks audibly on the
                // FIRST keypress only, and GetKeyboardText always ends up holding
                // exactly the LAST character typed ("SOFIA"->'a', "abc"->'c').
                // Two very different causes produce that end state:
                //   (A) no accumulation  -- buffer REPLACES per key:  s, o, f, i, a
                //   (B) something clears -- buffer grows then resets: s, so, '', o
                // Polling the live buffer every tick and logging only on CHANGE
                // distinguishes them; the Done snapshot alone cannot.
                vr::VREvent_t ev{};
                while (overlay->PollNextOverlayEvent(gPanelOvl, &ev, sizeof(ev))) {
                    switch (ev.eventType) {
                        case vr::VREvent_KeyboardCharInput: {
                            ++gKbdCharsThisSession;   // events seen (real or empty)
                            // DIAGNOSTIC (2026-09-07): SteamVR's keyboard lost its text
                            // band system-wide, which empties the GetKeyboardText buffer.
                            // The per-keystroke event path is independent of that buffer,
                            // so log the first few to prove whether it still fires.
                            // 2026-09-07: observed 8 events ALL carrying 0x00 in byte 0
                            // while GetKeyboardText still held real text. Dump all 8 bytes
                            // (cNewInput is 7 UTF-8 bytes + NUL) to see whether the char
                            // simply lands at another offset, plus uUserValue to confirm
                            // the event is ours.
                            if (gKbdCharsThisSession <= 12) {
                                const unsigned char* b =
                                    reinterpret_cast<const unsigned char*>(ev.data.keyboard.cNewInput);
                                SKSE::log::info(
                                    "Keyboard: CharInput #{} bytes=[{:02x} {:02x} {:02x} {:02x} "
                                    "{:02x} {:02x} {:02x} {:02x}] user={} age={:.3f}",
                                    gKbdCharsThisSession,
                                    b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
                                    ev.data.keyboard.uUserValue, ev.eventAgeSeconds);
                            }
                            // Only a REAL character counts. An empty event must not
                            // suppress the Done-buffer fallback below -- that bug made us
                            // discard text SteamVR had actually accumulated for us.
                            if (ev.data.keyboard.cNewInput[0] == '\0')
                                break;
                            ++gKbdRealCharsThisSession;
                            ForwardKeyboardInput(kbdTargetView, ev.data.keyboard.cNewInput);
                            break;
                        }
                        case vr::VREvent_KeyboardClosed:
                            // User explicitly dismissed — suppress auto-resummon
                            // while the same input is still focused. Next rising
                            // edge clears the suppression.
                            gKeyboardOpen      = false;
                            gKbdAutoSuppressed = true;
                            SKSE::log::info("Keyboard: closed by user (X button); auto-summon suppressed until next focus");
                            break;
                        case vr::VREvent_KeyboardDone: {
                            // Normal (non-minimal) mode keeps a buffer, and some
                            // runtimes deliver the text ONLY that way. Read it and
                            // forward anything we did not already receive as
                            // per-character events, so typing works in both modes.
                            char buf[512]{};
                            const uint32_t n = overlay->GetKeyboardText(buf, sizeof(buf));
                            // DIAGNOSTIC: unconditional, so an EMPTY buffer is visible too.
                            // charEvents>0 + len=0  => per-key path alive, buffer dead
                            //                          (we can rebuild text ourselves).
                            // charEvents=0 + len=0  => SteamVR sends no text at all.
                            {
                                const uint32_t shown = (n < sizeof(buf) - 1) ? n : sizeof(buf) - 1;
                                SKSE::log::info(
                                    "Keyboard: DIAG on Done -- CharInput events={}, "
                                    "GetKeyboardText len={}, text='{}'",
                                    gKbdCharsThisSession, n, std::string(buf, shown));
                                SKSE::log::info("Keyboard: DIAG real (non-empty) chars={}",
                                    gKbdRealCharsThisSession);
                            }
                            if (n > 0 && kbdTargetView != 0 && gKbdRealCharsThisSession == 0) {
                                buf[(n < sizeof(buf)) ? n : sizeof(buf) - 1] = '\0';
                                for (const char* c = buf; *c; ++c) {
                                    const char one[2] = { *c, '\0' };
                                    ForwardKeyboardInput(kbdTargetView, one);
                                }
                                SKSE::log::info("Keyboard: delivered {} buffered chars on Done", n);
                            }
                            // HideKeyboard() is GLOBAL — it dismisses whatever
                            // system keyboard is up, whoever owns it. Reaching
                            // here already implies gKeyboardOpen, i.e. the
                            // session is ours; clear the flag FIRST so no later
                            // path can call it again on someone else's keyboard.
                            gKeyboardOpen = false;
                            overlay->HideKeyboard();
                            SKSE::log::info("Keyboard: done (Enter/submit)");
                            break;
                        }
                        default:
                            break;
                    }
                }
                // NOTE (2026-09-07): this MUST run AFTER the event drain above.
                // The harvest is gated on gKbdRealCharsThisSession, which the drain
                // increments. Running it first reads a stale zero, so on a healthy
                // runtime BOTH paths deliver the same keystroke -- double-typing.
                // --- PER-TICK CHARACTER HARVEST (2026-09-07) -----------------
                // MEASURED on this machine with a native probe, Skyrim closed and
                // NO mods loaded (scratchpad/kbprobe):
                //   * VREvent_KeyboardCharInput fires per key but cNewInput is ALL
                //     ZERO -- no character is ever delivered in the event. The
                //     neighbouring uUserValue reads back perfectly, so the struct
                //     is unpacked correctly; SteamVR simply sends no payload.
                //   * GetKeyboardText NEVER ACCUMULATES. It holds exactly ONE
                //     character -- the newest. Typing "abcd" gives 'a','b','c','d'
                //     on successive polls, never "ab" or "abcd". That is
                //     KeyboardFlag_Minimal behaviour even though we ask for Modal.
                // So neither documented path yields a string. What DOES work is
                // sampling the one-character buffer every tick and appending each
                // change -- the characters are real, they just live for one poll.
                // LIMITATION: a key pressed and replaced between two ticks is
                // missed. Acceptable for laser-clicked VR typing; would not be for
                // a fast physical typist.
                {
                    char live[512]{};
                    const uint32_t ln = overlay->GetKeyboardText(live, sizeof(live));
                    const uint32_t lshow = (ln < sizeof(live) - 1) ? ln : sizeof(live) - 1;
                    std::string cur(live, lshow);
                    if (cur != gKbdLastLiveText) {
                        // MEASURED 2026-09-07 (native probe, no game, no mods):
                        // under KeyboardFlag_Modal, Enter does NOT raise
                        // VREvent_KeyboardDone here -- it lands as a newline in the
                        // one-character buffer like any other key. So the newline IS
                        // the submit signal; waiting on Done would hang forever.
                        if (cur.size() == 1 && (cur[0] == '\n' || cur[0] == '\r')) {
                            SKSE::log::info("Keyboard: newline == submit; delivered {} chars",
                                gKbdHarvested.size());
                            gKeyboardOpen = false;
                            overlay->HideKeyboard();
                            gKbdAutoSuppressed = true;
                            gKbdLastLiveText.clear();
                            gKbdHarvested.clear();
                        } else if (gKbdRealCharsThisSession == 0) {
                            // Only harvest when the runtime is NOT delivering
                            // characters in the events. Two buffer shapes exist:
                            //   healthy  : ACCUMULATES  "a" -> "ab" -> "abc"
                            //              (SteamVR beta, and stable before the
                            //              2026-09 regression) -> take the new tail
                            //   broken   : holds ONE char, replaced every key
                            //              (SteamVR stable 2026-09) -> take that char
                            // Handling both means one build works on either runtime.
                            std::string add;
                            if (cur.size() > gKbdHarvested.size() &&
                                cur.compare(0, gKbdHarvested.size(), gKbdHarvested) == 0) {
                                add = cur.substr(gKbdHarvested.size());
                            } else if (cur.size() == 1) {
                                add = cur;
                            }
                            for (const char c : add) {
                                const char one[2] = { c, '\0' };
                                if (kbdTargetView != 0)
                                    ForwardKeyboardInput(kbdTargetView, one);
                                gKbdHarvested += c;
                            }
                        }
                        SKSE::log::info("Keyboard: harvested '{}' -> \"{}\"",
                            cur, gKbdHarvested);
                        gKbdLastLiveText = cur;
                    }
                }
            }

            // --- 5d) Process pending re-inject countdowns -----------------
            // When a slot was activated, we Invoke('location.reload()') on its
            // primary view. That wipes our injected hooks AND the Ultralight
            // console-message callback binding (Ultralight resets the console
            // binding when the document is replaced). Wait N pump ticks for
            // the reload to complete, then re-install:
            //   1. V2 ConsoleMessageCallback — without this, ALL post-reload
            //      JS output (netdiag probes, sister-mod logs) is silent and
            //      we have no visibility into what's happening.
            //   2. Focus tracker — keyboard auto-summon on text-input focus.
            //   3. Network diagnostic — fetch/XHR/WS hooks + API probe.
            for (int s = 0; s < Slot_Count; ++s) {
                if (gSlotReinjectCountdown[s] <= 0) continue;
                if (--gSlotReinjectCountdown[s] != 0) continue;
                const uint64_t primary = gSlotViewIds[s];
                if (primary == 0) continue;
                // Re-register V2 console callback first — otherwise the
                // diagnostic re-injects below will succeed silently and we
                // won't see the API-PROBE output that tells us whether the
                // sister-mod JS bridge is present post-reload.
                if (gPrismaUI2) {
                    gPrismaUI2->RegisterConsoleCallback(primary, &OnConsoleMessage);
                }
                // Re-arm the focus tracker on the reloaded primary. location.reload()
                // wiped the injected script; re-Invoke restores it (idempotent —
                // the window flag is gone after the reload so it actually re-runs).
                // The 2s rediscovery also re-arms all family members, but do the
                // primary here so typing works the instant the reload settles.
                InstallFocusTracker(primary);
                InstallNetworkDiagnostic(s, primary);

                // ---- THE SkyrimNet-dashboard fix --------------------------
                // The SkyrimNet dashboard gates its ENTIRE page body (the Home
                // page + the right-side live-data modules: StatusModule,
                // NearbyModule, SceneModule) behind a React `hasOpened` flag.
                // Under PrismaUI that flag starts false (useState(!isPrisma))
                // and ONLY flips true when the host calls
                //   window.dashboardSetVisible('true')
                // — which SkyrimNet's own DashboardUIManager::Toggle() does on
                // desktop, but our SteamVR overlay bridge never did. Result:
                // the page chrome/background renders but the live-data panel
                // never MOUNTS, so its pollers (/?api=status, /?api=nearby-npcs,
                // /?api=game-state, /?api=gamemaster-status, /omnisight?api=list)
                // never start. The SAME flag also gates usePollingEffect's
                // fetch loop (it skips while dataset.dashboardHidden==='true'),
                // so this ONE call fixes both the mount gate and the poll gate.
                // (Verified via deep static analysis of the dashboard bundle:
                // App.jsx:479 `{hasOpened && ...}`, useVisibility.js, and the
                // banners poller working only because it's mounted OUTSIDE the
                // gate via ToastRegion.)
                //
                // We run this AFTER the netdiag install above so our fetch
                // hooks are in place to capture the now-firing pollers in the
                // log (confirms the fix + reveals any privileged-token issue).
                // Guarded by typeof check so it's a harmless no-op on sister
                // mods (SeverActions / IntelEngine) that don't define it.
                gPrismaUI->Invoke(primary,
                    "(function(){try{"
                    "if(typeof window.dashboardSetVisible==='function'){"
                    "window.dashboardSetVisible('true');"
                    "console.log('[prismauisvr] dashboardSetVisible(true) called');"
                    "}}catch(e){console.error('[prismauisvr] dashboardSetVisible threw: '+e.message);}})();",
                    nullptr);

                // Re-assert this slot's ZOOM (physical size) + the current
                // RESOLUTION preset on the freshly reloaded view. ReadResolution
                // refreshes the MCM choice from the INI so a menu change applies
                // on the next panel open.
                ApplyZoom(s, overlay);
                ReadResolution();
                ApplyResolution(s);

                SKSE::log::info("Slot[{}] post-reload re-inject complete on view {} "
                    "(V2 console callback re-registered, dashboardSetVisible(true) sent)", s, primary);
            }

            // --- 6) No active slot → hide panel, skip bitmap work ---------
            // This is the "overlay off" path — zero SetOverlayRaw calls.
            const int curActive = gActiveSlot.load();
            // NOTE: this used to also bail when gSlotViewIds[curActive] == 0,
            // i.e. whenever the active slot had no bound PrismaUI view. That
            // made the panel impossible to show at all on a PrismaUI without
            // the PrismaVR_* API (no view ever binds), so clicking an icon lit
            // up gActiveSlot and then nothing appeared. An active-but-unbound
            // slot now falls through to the synthetic test panel below; only a
            // genuinely inactive slot turns the panel off. The transition code
            // beneath is already guarded on prevPrimary/newPrimary != 0.
            if (curActive == Slot_None) {
                // Real → None transition: Unfocus the primary view. We do NOT
                // Hide it — keeping it shown lets PrismaUI keep rendering its
                // bitmap so the next activation has fresh content waiting.
                // (The Hide+Show cycle did not trigger sister-mod re-init in
                // practice — confirmed via the user's "still on the tab when
                // I re-open" observation. Reload is what actually resets.)
                if (lastActiveSlotSeen != Slot_None) {
                    const uint64_t prevPrimary = gSlotViewIds[lastActiveSlotSeen];
                    if (gPrismaUI && prevPrimary != 0) {
                        // Tell SkyrimNet-style dashboards they're hidden so they
                        // stop their background pollers (matches dashboardHidden
                        // contract). Harmless no-op on sister mods. Done BEFORE
                        // Unfocus while the view's JS state is still live.
                        gPrismaUI->Invoke(prevPrimary,
                            "(function(){try{if(typeof window.dashboardSetVisible==='function')"
                            "window.dashboardSetVisible('false');}catch(e){}})();", nullptr);
                        gPrismaUI->Unfocus(prevPrimary);
                        SKSE::log::info("Slot deactivation: dashboardSetVisible(false)+Unfocus(view {}) for slot {}",
                            prevPrimary, lastActiveSlotSeen);
                    }
                }
                if (gPanelShownLast) {
                    overlay->HideOverlay(gPanelOvl);
                    gPanelShownLast = false;
                    SKSE::log::info("Panel hidden (slot deactivated)");
                }
                // Addon mode: stop the hook capturing for a panel nobody is
                // looking at — otherwise it keeps doing a multi-MB memcpy
                // inside Present on the render thread forever.
                gCapWantView.store(0, std::memory_order_relaxed);
                gStickLingerTicks = 0;
                SetStickSuppressed(false);  // panel closed — stick guard off
                // Give the system keyboard back. Holding a session open while
                // our panel is closed is what let us intercept other mods'
                // typing; release it the moment we stop showing anything.
                if (gKeyboardOpen) {
                    gKeyboardOpen = false;
                    overlay->HideKeyboard();
                    SKSE::log::info("Keyboard: released (panel closed)");
                }
                gKbdAutoSuppressed      = false;
                gFocusLastForActiveSlot = false;
                // End any native-SMF session and force the PrismaUI path to
                // re-bind its own shared texture (they share one panel overlay).
                smf::EndSession();
                gTextureBoundToOverlay = false;
                lastActiveSlotSeen = Slot_None;
                continue;
            }

            // Slot just changed — force a fresh bitmap fetch on next submit
            // (otherwise we'd display the previous slot's frozen frame).
            if (curActive != lastActiveSlotSeen) {
                SKSE::log::info("Slot transition: {} -> {}", lastActiveSlotSeen, curActive);

                // Different real slot → tell its dashboard it's hidden (stop
                // pollers) + Unfocus its primary before activating the new one.
                // PrismaUI's focus is global per the API.
                if (lastActiveSlotSeen != Slot_None) {
                    const uint64_t prevPrimary = gSlotViewIds[lastActiveSlotSeen];
                    if (gPrismaUI && prevPrimary != 0) {
                        gPrismaUI->Invoke(prevPrimary,
                            "(function(){try{if(typeof window.dashboardSetVisible==='function')"
                            "window.dashboardSetVisible('false');}catch(e){}})();", nullptr);
                        gPrismaUI->Unfocus(prevPrimary);
                    }
                }

                // Activation: RELOAD the page + Focus + schedule re-inject.
                //
                // Page reload is the only mechanism that actually resets the
                // sister-mod HTML's runtime state — Hide/Show doesn't touch
                // the DOM, and dispatching focus/visibility events relies on
                // sister mod listening, which they typically don't.
                //
                // location.reload() forces:
                //   - Fresh window/DOM (back to main view, no tab nav state)
                //   - Re-run of the page's initial-load JS (which is where
                //     the dashboard does its initial data fetches against
                //     localhost:8080 — now we know that endpoint works)
                //   - Re-binding of refresh-button onclick handlers
                //
                // Our injected hooks (netdiag, focus tracker) die on reload —
                // they live in window scope, gone on navigation. We schedule
                // a re-inject N ticks (3 s) after reload to restore them.
                //
                // Focus is also called — even though page state is fresh, the
                // PrismaUI-side focus flag is what some sister mods check for
                // "user is interacting" gating.
                const uint64_t newPrimary = gSlotViewIds[curActive];
                if (gPrismaUI && newPrimary != 0) {
                    gPrismaUI->Invoke(newPrimary, "location.reload();", nullptr);
                    // Do NOT call Focus() in addon mode. Stock PrismaUI's Focus
                    // takes its FLATSCREEN path (it has no VR branch at all) and
                    // disables kMovement, kLooking, kJumping and kActivate — so
                    // opening a panel would freeze the player's left stick until
                    // it closed. We do not need PrismaUI's focus anyway: input
                    // reaches the page as DOM events, and text fields get real
                    // DOM focus from the .focus() our click injection performs.
                    if (!gStockBuild) {
                        gPrismaUI->Focus(newPrimary, false /*pauseGame*/, true /*disableFocusMenu*/);
                    }
                    gSlotReinjectCountdown[curActive] = kReinjectCountdownTicks;
                    SKSE::log::info("Slot activation: location.reload(){} for slot {} "
                        "(re-inject scheduled in {} ticks)",
                        gStockBuild ? " (Focus skipped — keeps player movement)"
                                    : std::format(" + Focus(view {})", newPrimary),
                        curActive, kReinjectCountdownTicks);
                }

                lastActiveSlotSeen = curActive;
                lastSubmitCursorX = -2;  // force cursor-driven resubmit too
                lastSubmitCursorY = -2;
            }

            // Active view within the slot may have shifted too (sister mod
            // opened a sub-view via tab click — see UpdateActiveViewsForSlots).
            // Same fresh-fetch reset applies.
            const uint64_t viewId = gSlotActiveViewIds[curActive];
            if (viewId != lastActiveViewIdSeen) {
                if (lastActiveViewIdSeen != 0 && viewId != 0) {
                    SKSE::log::info("Active-view transition (slot {}): {} -> {}",
                        curActive, lastActiveViewIdSeen, viewId);
                }
                lastActiveViewIdSeen = viewId;
                lastSubmitCursorX = -2;
                lastSubmitCursorY = -2;
                gForceNextSubmit = true;  // push the new view's bitmap immediately
                // Apply zoom + the current resolution to the newly-active view
                // (a sub-view opened via tab starts at the fork default size).
                ApplyZoom(curActive, overlay);
                ApplyResolution(curActive);
            }

            // --- 6a½) SMF SOURCE (native): SMF's hooked frame paints into a
            // game-device MISC_SHARED texture; the wrist overlay reads it LIVE
            // through the DXGI share opened on OUR device. The pump never
            // touches the game device context - the v1.3.x deadlock class
            // (device lock held across a compositor call) cannot recur.
            if (gSlotIsSmf[curActive]) {
                if (!smf::Available()) {
                    if (gPanelShownLast) { overlay->HideOverlay(gPanelOvl); gPanelShownLast = false; }
                    continue;
                }
                if (!smf::SessionActive()) {
                    smf::BeginSession();
                } else if (!smf::WindowStillOpen()) {
                    // Closed from inside SMF (its own close button / Escape on
                    // the desktop) - release the slot instead of forcing it back.
                    smf::EndSession();
                    gActiveSlot.store(Slot_None);
                    if (gPanelShownLast) { overlay->HideOverlay(gPanelOvl); gPanelShownLast = false; }
                    SKSE::log::info("SMF native: window closed from inside - slot released");
                    continue;
                }
                if (!gHadHitLast) { smf::PointerOff(); smf::SetScrollStick(0.0f); }  // laser left the panel
                // Size the wrist RT to SMF's own layout space (learned on its
                // first frame). Until that exists there is nothing to show.
                smf::EnsureRT(smf::WantW(), smf::WantH());
                if (!smf::RtReady()) {
                    if (gPanelShownLast) { overlay->HideOverlay(gPanelOvl); gPanelShownLast = false; }
                    continue;
                }
                // Open the share on OUR device once; each tick we then PULL the
                // latest frame into gSharedTex (same-device GPU copy) so the SMF
                // panel rides the one overlay binding whose live-update path every
                // PrismaUI slot already proves.
                // Re-open the alias whenever the share handle changes (an RT
                // resize makes a new texture, hence a new handle).
                static ID3D11Texture2D* sAlias = nullptr;
                static HANDLE           sAliasFor = nullptr;
                if (sAlias && sAliasFor != smf::ShareHandle()) {
                    sAlias->Release(); sAlias = nullptr;
                }
                if (!sAlias && gD3DDevice && smf::ShareHandle()) {
                    sAliasFor = smf::ShareHandle();
                    if (FAILED(gD3DDevice->OpenSharedResource(smf::ShareHandle(),
                            __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&sAlias)))) {
                        static bool sOpenFailLogged = false;
                        if (!sOpenFailLogged) {
                            sOpenFailLogged = true;
                            SKSE::log::error("SMF native: OpenSharedResource failed - panel stays blank.");
                        }
                        sAlias = nullptr;
                    } else {
                        SKSE::log::info("SMF native: shared render target opened on the overlay device.");
                    }
                }
                if (sAlias) {
                    // v1.4.2 FIELD FIX: SteamVR dedups SetOverlayTexture by handle
                    // and takes its copy of a foreign-device texture ONCE - binding
                    // the game-device alias froze the panel at its first frame (it
                    // only refreshed when a slot switch swapped the binding). So:
                    // same-device GPU copy into gSharedTex + Flush, per tick.
                    if (EnsureSharedTexSize(smf::RtW(), smf::RtH()) && gSharedTex && gD3DContext) {
                        gD3DContext->CopyResource(gSharedTex, sAlias);
                        gD3DContext->Flush();   // commit before the compositor's next read
                        if (!gTextureBoundToOverlay) {
                            BindSharedTextureToPanel(overlay);
                        }
                        if (!gPanelShownLast) {
                            overlay->ShowOverlay(gPanelOvl);
                            gPanelShownLast = true;
                            SKSE::log::info("Panel shown (slot {} = SKSE Menu Framework, native)", curActive);
                        }
                    }
                }
                continue;   // SMF owns this slot's frame; skip the PrismaUI path
            }

            // --- 6b) No bound view -> SYNTHETIC panel content -------------
            // True when the slot hasn't bound yet, or when the installed
            // PrismaUI has no PrismaVR_* API at all (stock build). Rather than
            // showing nothing, we draw a test pattern and push it through the
            // IDENTICAL texture/overlay path real content uses — so clicking an
            // icon always opens a visible panel, and the overlay pipeline can be
            // validated independently of where the pixels come from.
            //
            // It also keeps the NULL exports unreachable: gGetBitmap /
            // gGetBitmapIfNew are only touched in the non-synthetic branches
            // below (note !gGetBitmapIfNew would otherwise force needSubmit).
            // Addon mode: ask the hook to capture THIS view, and grow the
            // capture buffers if it reported a frame too big to fit.
            // (The right-stick guard is NOT armed here — it is gated on the
            // pointer dot actually being on the panel; see the block after
            // ProcessRaycast above.)

            const bool stockCapture = (gStockBuild != nullptr && gOrigCopyPixels != nullptr);
            if (stockCapture && viewId != 0) {
                gCapWantView.store(viewId, std::memory_order_relaxed);
                // NEVER resize the capture buffers here: the hook may be inside
                // memcpy on them right now, on the render thread. They are sized
                // once at init; an oversized frame is dropped and reported.
                const size_t need = gCapNeedBytes.exchange(0, std::memory_order_relaxed);
                if (need > gCapBuf[0].size() && !gLoggedCapTooSmall) {
                    SKSE::log::error("addon mode: frame needs {} bytes but capture buffers are {} — "
                                     "dropping frames ({} so far); the panel will stay blank.",
                                     need, gCapBuf[0].size(),
                                     gCapDrops.load(std::memory_order_relaxed));
                    gLoggedCapTooSmall = true;
                }
            }

            // Synthetic only when we have NO way to get real pixels for this
            // slot: no bound view at all, or neither the fork API nor the hook.
            const bool syntheticPanel = (viewId == 0) || (!gGetBitmap && !stockCapture);
            if (syntheticPanel) {
                w = 1280; h = 720; stride = w * 4;   // modest size; this is a test image
            }

            // --- 7) Decide whether to push a fresh frame ------------------
            const auto nowFrame = std::chrono::steady_clock::now();
            // Cursor position from the stored hit-UV, scaled to the CURRENT
            // bitmap size (recomputed again at draw time below in case the
            // bitmap size changed between here and the pixel fetch). UV-based
            // so the dot can never fall outside the drawn bitmap.
            auto cursorFromUV = [&](uint32_t bw, uint32_t bh, int& ox, int& oy) {
                if (!gHadHitLast) { ox = -1; oy = -1; return; }
                ox = static_cast<int>(gLastHitU * bw);
                oy = static_cast<int>((1.0f - gLastHitV) * bh);
                if (ox < 0) ox = 0; else if (ox >= (int)bw) ox = (int)bw - 1;
                if (oy < 0) oy = 0; else if (oy >= (int)bh) oy = (int)bh - 1;
            };
            int cx = -1, cy = -1;
            cursorFromUV(w, h, cx, cy);
            const bool cursorMoved   = (cx != lastSubmitCursorX || cy != lastSubmitCursorY);
            // Rate cap was 333 ms when we were on SetOverlayRaw and worried
            // about the 197-submit wedge. The texture path (UpdateSubresource
            // → shared memory) has no such ceiling, so we can submit at full
            // pump tick (~10 Hz) for smooth cursor tracking on the panel.
            const bool cursorRateOk  = (nowFrame - lastCursorSubmit) >= std::chrono::milliseconds(33);
            const bool cursorWantsSubmit = cursorMoved && cursorRateOk;

            const size_t srcSize = static_cast<size_t>(h) * stride;
            if (bgraBuf.size() < srcSize) bgraBuf.resize(srcSize);

            bool prismaHasNew = false;
            bool capCopied    = false;
            if (!syntheticPanel && stockCapture) {
                // Addon mode. We ALWAYS re-copy the published frame into bgraBuf
                // before drawing, not only when the sequence advanced. The cursor
                // and resize handle are composited INTO this buffer, so reusing
                // last tick's buffer would paint each new cursor position on top
                // of the old ones — the dots accumulate into a trail and only
                // clear when the page happens to repaint.
                const uint64_t seq = gCapSeq.load(std::memory_order_acquire);
                const int      idx = gCapReady.load(std::memory_order_acquire);
                if (idx >= 0) {
                    const uint32_t cw = gCapW.load(std::memory_order_relaxed);
                    const uint32_t ch = gCapH.load(std::memory_order_relaxed);
                    const uint32_t cs = gCapStride.load(std::memory_order_relaxed);
                    const size_t   n  = static_cast<size_t>(ch) * cs;
                    if (cw && ch && cs && gCapBuf[idx].size() >= n) {
                        if (bgraBuf.size() < n) bgraBuf.resize(n);
                        std::memcpy(bgraBuf.data(), gCapBuf[idx].data(), n);
                        // Seqlock: if the hook published again mid-copy it may
                        // have reclaimed this buffer (there are only two), so
                        // discard the torn copy and retake it next tick.
                        if (gCapSeq.load(std::memory_order_acquire) != seq) {
                            lastCapSeq = 0;
                            continue;
                        }
                        w = cw; h = ch; stride = cs;
                        capCopied = true;
                        if (seq != lastCapSeq) {
                            prismaHasNew = true;
                            lastCapSeq   = seq;
                            if (!gLoggedFirstCapture) {
                                SKSE::log::info("addon mode: FIRST REAL FRAME captured from view {} "
                                                "({}x{} stride={}) — stock PrismaUI content is now on "
                                                "the wrist panel.", viewId, cw, ch, cs);
                                gLoggedFirstCapture = true;
                            }
                        }
                    }
                }
            } else if (!syntheticPanel && gGetBitmapIfNew) {
                prismaHasNew = gGetBitmapIfNew(viewId, bgraBuf.data(),
                    static_cast<uint32_t>(bgraBuf.size()), &w, &h, &stride);
            }

            // Synthetic content animates, so submit every tick. In addon mode
            // we submit only on a genuinely new captured frame (or a cursor
            // move) — the hook fires exactly once per repaint, so this is the
            // cheapest possible change-detection.
            const bool needSubmit = syntheticPanel
                ? true
                : stockCapture
                    ? (prismaHasNew || cursorWantsSubmit || gForceNextSubmit)
                    : (prismaHasNew || cursorWantsSubmit || !gGetBitmapIfNew || gForceNextSubmit);
            if (!needSubmit) continue;

            // Addon mode with nothing captured yet (view still painting its
            // first frame): skip rather than showing an empty buffer.
            // Addon mode: nothing to show until at least one frame is captured.
            // (capCopied means bgraBuf holds a clean, current copy of the page.)
            if (!syntheticPanel && stockCapture && !capCopied) continue;

            if (syntheticPanel) {
                ++gTestPatternTick;
                DrawTestPatternBGRA(bgraBuf.data(), w, h, stride, curActive, gTestPatternTick);
                if (!gLoggedSyntheticPanel) {
                    SKSE::log::info("Panel showing SYNTHETIC test pattern for slot {} ({}x{}) — "
                                    "no PrismaUI view bound. If you can see this in VR, the entire "
                                    "overlay pipeline (shared texture -> SteamVR -> wrist) is WORKING.",
                                    curActive, w, h);
                    gLoggedSyntheticPanel = true;
                }
            } else if (!prismaHasNew && !stockCapture) {
                if (!gGetBitmap(viewId, bgraBuf.data(),
                        static_cast<uint32_t>(bgraBuf.size()), &w, &h, &stride))
                    continue;
            }

            // ==== TEXTURE-SHARING PATH (Stage 2.4) ========================
            // Bypass SetOverlayRaw entirely by updating a shared D3D11
            // texture that vrcompositor reads cross-process. No per-submit
            // staging upload — should defeat the ~197-submission wedge.
            if (kUseTexturePath && gSharedTex && gSharedTexHandle && gD3DContext) {
                // Re-derive the cursor from UV at the FINAL bitmap size (w,h may
                // have changed since section 7 via the pixel fetch). This is
                // what guarantees the dot stays inside the drawn bitmap.
                cursorFromUV(w, h, cx, cy);
                // Cursor goes straight into the BGRA buffer (no RGBA conversion).
                if (cx >= 0 && cy >= 0) {
                    DrawCursorBGRA(bgraBuf.data(), w, h, stride, cx, cy);
                }
                // Resize grab-handle in the top-right corner (brighter while a
                // resize drag is in progress).
                DrawResizeHandleBGRA(bgraBuf.data(), w, h, stride, gResizing);
                lastSubmitCursorX = cx;
                lastSubmitCursorY = cy;
                lastCursorSubmit  = nowFrame;

                // Keep the shared texture EXACTLY the bitmap size, so the
                // overlay shows it 1:1 with no dead region (no texture-bounds
                // cropping — SteamVR wasn't honoring bounds on our DXGI shared
                // texture). Reallocating clears gTextureBoundToOverlay, so the
                // (re)bind below runs with the fresh handle.
                EnsureSharedTexSize(w, h);

                // Bind the shared texture to the panel overlay on first use
                // (or after a reallocation / handle recycle).
                if (!gTextureBoundToOverlay) {
                    BindSharedTextureToPanel(overlay);
                }

                if (gTextureBoundToOverlay && UpdateSharedTexture(bgraBuf.data(), w, h, stride)) {
                    // Diagnostic bookkeeping (mirrors the SetOverlayRaw path).
                    if (!gFirstSubmitRecorded) {
                        gFirstSubmitTime = nowFrame;
                        gFirstSubmitRecorded = true;
                    }
                    gLastSubmitTime = nowFrame;
                    gSubmitSuccessCount.fetch_add(1);
                    gSubmitTotalBytes.fetch_add(static_cast<int64_t>(w) * h * 4);
                    ++gPanelSubmitCount;
                    gForceNextSubmit = false;

                    // Periodic stats every ~30 s.
                    static auto lastTexStatsLog = std::chrono::steady_clock::time_point{};
                    if (lastTexStatsLog == std::chrono::steady_clock::time_point{}) lastTexStatsLog = nowFrame;
                    if (nowFrame - lastTexStatsLog >= std::chrono::seconds(30)) {
                        const auto totalBytes = gSubmitTotalBytes.load();
                        const auto totalCount = gSubmitSuccessCount.load();
                        const double elapsedSec = std::chrono::duration_cast<std::chrono::duration<double>>(
                            nowFrame - gFirstSubmitTime).count();
                        SKSE::log::info("[tex] Submit stats @ {:.0f}s: count={} totalMB={:.1f} avgRate={:.2f}MB/s",
                            elapsedSec, totalCount, totalBytes / (1024.0 * 1024.0),
                            (elapsedSec > 0.1 ? (totalBytes / (1024.0 * 1024.0) / elapsedSec) : 0.0));
                        lastTexStatsLog = nowFrame;
                    }
                }

                if (!gPanelShownLast) {
                    overlay->ShowOverlay(gPanelOvl);
                    gPanelShownLast = true;
                    if (!everShownPanel) {
                        SKSE::log::info("First panel show [tex]: slot={} view={} bitmap={}x{}", curActive, viewId, w, h);
                        everShownPanel = true;
                    } else {
                        SKSE::log::info("Panel shown [tex] (slot {} activated)", curActive);
                    }
                }
                continue;  // skip the legacy SetOverlayRaw path
            }
            // ==== END TEXTURE-SHARING PATH ================================

            const size_t tightSize = static_cast<size_t>(w) * h * 4;
            if (rgbaBuf.size() < tightSize) rgbaBuf.resize(tightSize);

            // BGRA(stride) → RGBA(tight).
            for (uint32_t y = 0; y < h; ++y) {
                const uint8_t* srcRow = bgraBuf.data() + static_cast<size_t>(y) * stride;
                uint8_t*       dstRow = rgbaBuf.data() + static_cast<size_t>(y) * w * 4;
                for (uint32_t x = 0; x < w; ++x) {
                    dstRow[x * 4 + 0] = srcRow[x * 4 + 2];
                    dstRow[x * 4 + 1] = srcRow[x * 4 + 1];
                    dstRow[x * 4 + 2] = srcRow[x * 4 + 0];
                    dstRow[x * 4 + 3] = srcRow[x * 4 + 3];
                }
            }

            // Stamp cursor on top.
            if (cx >= 0 && cy >= 0) {
                DrawCursor(rgbaBuf.data(), w, h, cx, cy);
            }
            lastSubmitCursorX = cx;
            lastSubmitCursorY = cy;
            lastCursorSubmit  = nowFrame;

            // Submit (time-gated to recover from sticky error-23 runs).
            if (nowFrame >= nextSORAttempt) {
                auto err = overlay->SetOverlayRaw(gPanelOvl, rgbaBuf.data(), w, h, 4);
                if (err != vr::VROverlayError_None) {
                    ++consecutiveSORErr;

                    // Dump submission-health stats on first failure so we can
                    // see the volume / rate leading up to the crash.
                    if (consecutiveSORErr == 1) {
                        const auto totalBytes = gSubmitTotalBytes.load();
                        const auto totalCount = gSubmitSuccessCount.load();
                        double elapsedSec = 0.0;
                        double sinceLastSec = 0.0;
                        if (gFirstSubmitRecorded) {
                            elapsedSec = std::chrono::duration_cast<std::chrono::duration<double>>(
                                nowFrame - gFirstSubmitTime).count();
                            sinceLastSec = std::chrono::duration_cast<std::chrono::duration<double>>(
                                nowFrame - gLastSubmitTime).count();
                        }
                        SKSE::log::error(
                            "FIRST ERROR 23 — submit stats: count={} totalMB={:.1f} elapsed={:.1f}s "
                            "sinceLast={:.2f}s avgRate={:.1f}MB/s bmp={}x{}",
                            totalCount, totalBytes / (1024.0 * 1024.0), elapsedSec, sinceLastSec,
                            (elapsedSec > 0.1 ? (totalBytes / (1024.0 * 1024.0) / elapsedSec) : 0.0),
                            w, h);
                    }

                    if (consecutiveSORErr <= 3 || (consecutiveSORErr % 30) == 0) {
                        SKSE::log::error("SetOverlayRaw failed: {} ({}) [consecutive={}] bmp={}x{}",
                            static_cast<int>(err), overlay->GetOverlayErrorNameFromEnum(err),
                            consecutiveSORErr, w, h);
                    }
                    int delayMs = 500;
                    if      (consecutiveSORErr > 50) delayMs = 5000;
                    else if (consecutiveSORErr > 10) delayMs = 2000;
                    nextSORAttempt = nowFrame + std::chrono::milliseconds(delayMs);
                    continue;
                }

                // Successful submit — bookkeeping for the diagnostics.
                if (!gFirstSubmitRecorded) {
                    gFirstSubmitTime = nowFrame;
                    gFirstSubmitRecorded = true;
                }
                gLastSubmitTime = nowFrame;
                gSubmitSuccessCount.fetch_add(1);
                gSubmitTotalBytes.fetch_add(static_cast<int64_t>(w) * h * 4);
                ++gPanelSubmitCount;
                gForceNextSubmit = false;  // consumed by this submit

                if (consecutiveSORErr > 0) {
                    SKSE::log::info("SetOverlayRaw recovered after {} errors", consecutiveSORErr);
                    consecutiveSORErr = 0;
                }
                nextSORAttempt = nowFrame;

                // Pre-emptive recycle BEFORE hitting the ~197-submit wedge.
                // Doing it here (after a successful submit) means we never
                // touch a wedged handle — fresh handles only.
                // (Only relevant on the SetOverlayRaw path; the texture path
                //  shouldn't accumulate the same per-submit pressure.)
                if (!kUseTexturePath && gPanelSubmitCount >= kPanelRecycleAt) {
                    RecyclePanelHandle();
                    continue;  // next tick will mirror transform + submit on the new handle
                }

                // Periodic stats every ~30 s of wall-clock so we can see the
                // submission rate climb / sustain.
                static auto lastStatsLog = std::chrono::steady_clock::time_point{};
                if (lastStatsLog == std::chrono::steady_clock::time_point{}) lastStatsLog = nowFrame;
                if (nowFrame - lastStatsLog >= std::chrono::seconds(30)) {
                    const auto totalBytes = gSubmitTotalBytes.load();
                    const auto totalCount = gSubmitSuccessCount.load();
                    const double elapsedSec = std::chrono::duration_cast<std::chrono::duration<double>>(
                        nowFrame - gFirstSubmitTime).count();
                    SKSE::log::info("Submit stats @ {:.0f}s: count={} totalMB={:.1f} avgRate={:.2f}MB/s",
                        elapsedSec, totalCount, totalBytes / (1024.0 * 1024.0),
                        (elapsedSec > 0.1 ? (totalBytes / (1024.0 * 1024.0) / elapsedSec) : 0.0));
                    lastStatsLog = nowFrame;
                }
            } else {
                continue;
            }

            // Show panel on first successful submit for this activation.
            if (!gPanelShownLast) {
                overlay->ShowOverlay(gPanelOvl);
                gPanelShownLast = true;
                if (!everShownPanel) {
                    SKSE::log::info("First panel show: slot={} view={} bitmap={}x{}", curActive, viewId, w, h);
                    everShownPanel = true;
                } else {
                    SKSE::log::info("Panel shown (slot {} activated)", curActive);
                }
            }
        }
        SKSE::log::info("Pump thread exiting");
    }

    // ----------------------------------------------------------------------
    // OpenComposite detection — the dormancy gate.
    //
    // Under OpenComposite (OCU), requesting an interface version the runtime
    // doesn't implement makes it show an error box and hard-abort the game.
    // This plugin links a current OpenVR SDK generation (IVROverlay_028 etc.),
    // which OCU does not implement — so the FIRST vr::VROverlay() call under
    // OCU kills the session. OCU users don't need this bridge anyway:
    // PrismaUI's own native VR path is built for OCU (aim poses + VR keyboard
    // come from OCU itself). So: detect OCU and stay completely dormant.
    //
    // Detection deliberately never queries the runtime (that's the very thing
    // that aborts). Instead we read the loaded openvr_api.dll's file from disk
    // and scan it for the "OpenComposite" marker string — present in every OC
    // build (branding + log/error messages), absent from Valve's runtime DLL.
    // ----------------------------------------------------------------------
    bool gOpenCompositeMode = false;

    // Whether ResolvePrismaUI() found a PrismaUI.dll exposing the PrismaVR_*
    // consumer API this bridge needs. STOCK PrismaUI does not export it, so
    // without this gate we would still create wrist overlays + a pump thread
    // that can never bind to anything — three dead icons and a spinning
    // thread. Fail closed instead: no usable PrismaUI, no overlays, nothing
    // touched. That also lets this mod stay enabled permanently (it carries
    // the registration ESP + MCM) regardless of which PrismaUI is installed.
    bool gPrismaReady = false;

    bool DetectOpenComposite()
    {
        HMODULE h = ::GetModuleHandleA("openvr_api.dll");
        if (!h) {
            SKSE::log::warn("openvr_api.dll not loaded at kInputLoaded — assuming SteamVR native");
            return false;
        }
        char path[MAX_PATH]{};
        if (!::GetModuleFileNameA(h, path, MAX_PATH)) {
            SKSE::log::warn("GetModuleFileName(openvr_api.dll) failed — assuming SteamVR native");
            return false;
        }
        std::ifstream f(path, std::ios::binary);
        if (!f) {
            SKSE::log::warn("Could not read '{}' — assuming SteamVR native", path);
            return false;
        }
        std::string blob((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        const bool oc = blob.find("OpenComposite") != std::string::npos;
        SKSE::log::info("Runtime probe: '{}' ({} bytes) -> {}",
            path, blob.size(), oc ? "OpenComposite" : "SteamVR native");
        return oc;
    }

    void BootstrapLight()
    {
        if (gOpenCompositeMode) return;  // dormant under OCU — never touch IVROverlay
        if (gOverlayCreated) return;
        if (!CreatePanelOverlay()) { SKSE::log::error("Panel overlay creation failed"); return; }
        for (int i = 0; i < Slot_Count; ++i) {
            if (!CreateIconOverlay(i)) {
                SKSE::log::error("Icon[{}] creation failed; wrist menu will be partially unavailable", i);
            }
        }
        if (!CreateCursorOverlay()) {
            SKSE::log::warn("Cursor overlay creation failed; icons will not show a hover indicator");
        }
        if (kUseTexturePath) {
            if (!InitSharedTexture()) {
                SKSE::log::error("Shared-texture init FAILED — falling back to SetOverlayRaw path");
                // kUseTexturePath is constexpr; we still won't try the path
                // again at runtime if init failed. The pump checks
                // gSharedTexHandle before using the texture path.
            }
        }
        gOverlayCreated = true;
        if (!gPumpThread.joinable()) gPumpThread = std::thread(PumpLoop);
    }

    // ---- right-stick guard ----------------------------------------------
    // While the pointer (the yellow dot) is ON the panel, the right stick is
    // the page scroller — but Skyrim VR also reads stick-up as JUMP and
    // stick-down as SNEAK. Mask exactly those two user-event groups, and ONLY
    // while the dot is on (never kMovement/kLooking: the player must still be
    // able to walk, which is exactly what PrismaUI's own Focus() got wrong;
    // and per the KB rule, suppress on a consequence state the player can see
    // — "my pointer is inside the panel" — not a whole-panel-open blanket
    // that fights other mods' control toggles for minutes at a time).
    //
    // The mask must be HELD, not fire-and-forget: the game itself restores
    // enabledControls in places (LoadStoredControls on menu/scene
    // transitions) and other mods toggle controls too. The v1 blanket mask
    // set the flag once and early-returned forever after, so any external
    // re-enable silently won — the observed "still jumps while scrolling".
    // Now the steady-state path HEALS: every pump tick while suppressed it
    // re-checks enabledControls and re-disables anything that leaked back on,
    // logging each heal. Those log lines double as the diagnostic that
    // separates "flag got clobbered" from "flag doesn't gate this input"
    // if stick actions are ever seen again while the dot is on.
    //
    // Restore only re-enables the bits WE disabled (snapshot at suppress
    // time), so another mod's legitimate jump/sneak disable is never undone.
    // ControlMap is main-thread state and this runs on the pump thread, so
    // every ToggleControls goes through the task interface; the snapshot
    // read is a benign racy u32 load.
    void SetStickSuppressed(bool suppress)
    {
        using UEF = RE::UserEvents::USER_EVENT_FLAG;
        constexpr uint32_t kWantMask =
            static_cast<uint32_t>(UEF::kJumping) | static_cast<uint32_t>(UEF::kSneaking);
        auto* cm = RE::ControlMap::GetSingleton();

        if (suppress == gJumpSuppressed) {
            // Steady state. While suppressed, heal external re-enables.
            if (suppress && cm && gStickSuppressedFlags) {
                const uint32_t leaked =
                    cm->GetRuntimeData().enabledControls.underlying() & gStickSuppressedFlags;
                if (leaked) {
                    ++gStickHealCount;
                    SKSE::log::warn("Stick guard: flags {:#x} re-enabled externally while the dot "
                                    "is on the panel — re-suppressing (heal #{}).",
                                    leaked, gStickHealCount);
                    if (auto* task = SKSE::GetTaskInterface()) {
                        task->AddTask([leaked]() {
                            if (auto* c = RE::ControlMap::GetSingleton())
                                c->ToggleControls(static_cast<UEF>(leaked), false, false);
                        });
                    }
                }
            }
            return;
        }

        gJumpSuppressed = suppress;
        if (suppress) {
            gStickSuppressedFlags = cm
                ? (cm->GetRuntimeData().enabledControls.underlying() & kWantMask)
                : kWantMask;
            const uint32_t flags = gStickSuppressedFlags;
            if (flags) {
                if (auto* task = SKSE::GetTaskInterface()) {
                    task->AddTask([flags]() {
                        if (auto* c = RE::ControlMap::GetSingleton())
                            c->ToggleControls(static_cast<UEF>(flags), false, false);
                    });
                }
            }
            SKSE::log::info("Stick guard ON (dot on panel): masked {:#x} (jump|sneak)", flags);
        } else {
            const uint32_t flags = gStickSuppressedFlags;
            gStickSuppressedFlags = 0;
            if (flags) {
                if (auto* task = SKSE::GetTaskInterface()) {
                    task->AddTask([flags]() {
                        if (auto* c = RE::ControlMap::GetSingleton())
                            c->ToggleControls(static_cast<UEF>(flags), true, false);
                    });
                }
            }
            SKSE::log::info("Stick guard OFF (dot left panel): restored {:#x}", flags);
        }
    }

    // Hide every overlay we own. Called on pre-load-game so we don't leave
    // dangling visuals across save loads.

    void HideAllOverlays()
    {
        smf::EndSession();   // release the SMF window/pause state with the overlays
        gCapWantView.store(0, std::memory_order_relaxed);  // stop addon-mode capture
        gStickLingerTicks = 0;
        SetStickSuppressed(false);                        // never leave the stick guard on
        // Never leave a system-keyboard session held across a load — it would
        // keep routing other mods' typed characters into our (now dead) queue.
        if (gKeyboardOpen) {
            gKeyboardOpen = false;
            if (auto* ov2 = vr::VROverlay()) ov2->HideKeyboard();
        }
        gKbdAutoSuppressed      = false;
        gFocusLastForActiveSlot = false;
        // Nothing to hide if we never created overlays — and under OCU the
        // vr::VROverlay() call itself must never happen (IVROverlay_028 abort).
        if (!gOverlayCreated) return;
        auto* ov = vr::VROverlay();
        if (!ov) return;
        if (gPanelOvl != vr::k_ulOverlayHandleInvalid) ov->HideOverlay(gPanelOvl);
        for (int i = 0; i < Slot_Count; ++i) {
            if (gIconOvls[i] != vr::k_ulOverlayHandleInvalid) ov->HideOverlay(gIconOvls[i]);
        }
        if (gCursorOvl != vr::k_ulOverlayHandleInvalid) ov->HideOverlay(gCursorOvl);
        gPanelShownLast  = false;
        gIconsShownLast  = false;
        gCursorShownLast = false;
    }

    void OnSKSEMessage(SKSE::MessagingInterface::Message* msg)
    {
        if (!msg) return;
        switch (msg->type) {
        case SKSE::MessagingInterface::kPostLoad:
            SKSE::log::info("kPostLoad — resolving PrismaUI API");
            gPrismaReady = ResolvePrismaUI();
            if (!gPrismaReady) {
                // No PrismaVR_* API. Try ADDON MODE against a stock PrismaUI:
                // hook its pixel path and read its view map directly.
                SKSE::log::info("kPostLoad — no fork API; attempting stock-PrismaUI addon mode");
                if (InitStockPrismaMode()) gPrismaReady = true;
            }
            break;

        case SKSE::MessagingInterface::kPostPostLoad:
            // Documented safe point for the ImGui VR Helper handshake: it fires
            // after every plugin's kPostLoad, so the helper's listener exists
            // regardless of load order. Retryable — the pump re-tries below.
            SKSE::log::info("kPostPostLoad — arming the native SKSE Menu Framework source");
            smf::EnsureNative();
            break;

        case SKSE::MessagingInterface::kInputLoaded:
            if (!gPrismaReady) {
                // The wrist ICONS are ours: procedurally-drawn SteamVR overlays
                // that owe nothing to PrismaUI. Only the PANEL needs PrismaUI's
                // pixels. So we still create the overlays and run the pump —
                // the icons appear and are clickable, the panel simply has no
                // content to show until a PrismaUI with the PrismaVR_* API is
                // installed. (The pump's panel section early-outs on an unbound
                // view, which is also what keeps the null exports unreachable.)
                SKSE::log::warn("No PrismaUI with the PrismaVR_* consumer API — wrist icons will still "
                    "appear, but panels will have no content until a PrismaUI exposing the API is installed.");
            }
            gOpenCompositeMode = DetectOpenComposite();
            if (gOpenCompositeMode) {
                SKSE::log::warn("OpenComposite runtime detected — PrismaUISteamVR staying DORMANT. "
                    "PrismaUI's own native OCU path handles VR UI; this bridge is for SteamVR native only.");
                break;
            }
            SKSE::log::info("kInputLoaded — creating panel + icon overlays (hidden until palm-down)");
            BootstrapLight();
            break;

        case SKSE::MessagingInterface::kDataLoaded:
            SKSE::log::info("kDataLoaded");
            break;

        case SKSE::MessagingInterface::kPostLoadGame:
            SKSE::log::info("kPostLoadGame — save loaded, enabling pump");
            gGameLoaded = true;
            // Disabled-control state PERSISTS INTO SAVES (the same engine
            // mechanism as the classic "saved during a scripted scene, now I
            // can't move" bug). A save written while the stick guard was on —
            // an autosave on a cell change, say — carries jump/sneak disabled,
            // and the save's own control state loads AFTER our kPreLoadGame
            // restore, so that restore cannot win. No panel can possibly be
            // open at this instant: unconditionally give both back, ignoring
            // gJumpSuppressed (which tracks OUR toggle, not the save's).
            gJumpSuppressed       = false;
            gStickSuppressedFlags = 0;
            gStickLingerTicks     = 0;
            if (auto* task = SKSE::GetTaskInterface()) {
                task->AddTask([]() {
                    using UEF = RE::UserEvents::USER_EVENT_FLAG;
                    if (auto* cm = RE::ControlMap::GetSingleton())
                        cm->ToggleControls(static_cast<UEF>(
                            static_cast<uint32_t>(UEF::kJumping) |
                            static_cast<uint32_t>(UEF::kSneaking)), true, false);
                });
            }
            break;

        case SKSE::MessagingInterface::kPreLoadGame:
            SKSE::log::info("kPreLoadGame — pausing pump, hiding overlays");
            gGameLoaded = false;
            HideAllOverlays();
            break;

        case SKSE::MessagingInterface::kNewGame:
            SKSE::log::info("kNewGame — treating like a post-load");
            gGameLoaded = true;
            break;

        default:
            break;
        }
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* skse)
{
    SKSE::Init(skse);
    SKSE::log::info("PrismaUI SteamVR starting (wrist-menu picker)");

    auto* messaging = SKSE::GetMessagingInterface();
    if (!messaging) { SKSE::log::error("No messaging interface"); return false; }
    if (!messaging->RegisterListener(OnSKSEMessage)) {
        SKSE::log::error("Failed to register messaging listener"); return false;
    }
    SKSE::log::info("Listener registered. Awaiting kPostLoad.");
    return true;
}
