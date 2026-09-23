// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace mkw::vr {
void MkwVRPolicySetSettingsVisible(bool visible) noexcept;

// Game-facing presentation policy. The OpenXR implementation owns session and
// swapchain state; this module only decides how Mario Kart Wii content should
// be presented once the integration layer publishes game observations.
enum class VRPresentationMode : uint8_t {
    Desktop,
    VirtualScreen,
    ImmersiveRace,
};

enum class VRSceneMode : uint8_t {
    Unknown,
    FrontEnd,
    Race,
    Replay,
    Awards,
    Other,
};

enum class VRProjectionKind : uint8_t {
    Unknown,
    Perspective,
    Orthographic,
};

enum class VRDrawPass : uint8_t {
    Scene,
    PostProcess,
};

enum class VRDrawClass : uint8_t {
    Unknown,
    PerspectiveWorld,
    OrthographicHud,
    PostProcess,
};

enum class VRDrawRoute : uint8_t {
    DesktopPassthrough,
    VirtualScreen,
    StereoWorld,
    HeadLockedHud,
    PerEyePostProcess,
    Unclassified,
};

// Instrumentation capabilities are explicit so a partial binding can never
// accidentally enable immersive rendering. In particular, a camera sample by
// itself is insufficient because orthographic UI would otherwise be rendered
// with the stereo world path.
enum MkwVRBinding : uint32_t {
    MkwVRBindingNone = 0,
    MkwVRBindingSceneState = 1u << 0,
    MkwVRBindingRaceCamera = 1u << 1,
    MkwVRBindingDrawClassification = 1u << 2,
    MkwVRBindingPostProcess = 1u << 3,
    MkwVRBindingCulling = 1u << 4,
};

inline constexpr uint32_t kMkwVRRequiredImmersiveBindings =
    MkwVRBindingSceneState | MkwVRBindingRaceCamera | MkwVRBindingDrawClassification;

struct MkwVRPolicyConfig {
    bool enabled = false;
    bool immersive_races = true;
    float world_units_per_meter = 500.0f;
    float hud_distance_meters = 2.0f;
    // Width of the virtual screen, shared by the menu quad layer and the
    // immersive race HUD so 2D content keeps its place across the transition.
    float hud_width_meters = 2.4f;
    float hud_scale = 1.0f;
    // World scale used while the first-person camera is engaged. Mario Kart
    // Wii is authored at roughly this many units per metre, so it is what
    // makes the race read life-size; the third-person default deliberately
    // does not, presenting the race as a small diorama instead.
    float first_person_units_per_meter = 100.0f;
};

struct MkwVRSceneObservation {
    VRSceneMode mode = VRSceneMode::Unknown;
    uint32_t local_player_count = 0;
    uint64_t guest_frame_index = 0;
};

struct MkwVRCameraObservation {
    // Mario Kart and NW4R expose affine view matrices as row-major 3x4 Mtx
    // values. The integration layer is responsible for reading/converting the
    // guest value; this policy never assumes a guest object layout.
    std::array<float, 12> view_from_world{};
    uint32_t guest_camera_address = 0;
    uint64_t guest_frame_index = 0;
    bool valid = false;
};

struct MkwVRDrawObservation {
    VRProjectionKind projection = VRProjectionKind::Unknown;
    VRDrawPass pass = VRDrawPass::Scene;
};

struct MkwVRPolicySnapshot {
    VRPresentationMode presentation = VRPresentationMode::Desktop;
    MkwVRPolicyConfig config{};
    MkwVRSceneObservation scene{};
    MkwVRCameraObservation camera{};
    uint32_t available_bindings = MkwVRBindingNone;
    bool session_active = false;
    bool settings_visible = false;
    // A first-person camera is actually driving the view this frame. Set by the
    // integration layer once the anchor it publishes to the renderer is valid,
    // so the world scale can never disagree with where the camera is.
    bool first_person_engaged = false;
    // Changes whenever the stable presentation-safety state changes. Ordinary
    // per-frame scene/camera publication does not advance it.
    uint64_t safety_generation = 1;
    // Opaque safety-state tag for the content being sealed. This also includes
    // the current presentation mode, so a transient scene/camera mismatch
    // cannot accept an immersive packet from an adjacent asynchronous frame.
    uint64_t content_tag = 0;
    // Reprojection of an already completed image does not read the new guest
    // camera. Keep it visible across incomplete scene/camera publication, but
    // invalidate it on actual scene, settings or session changes.
    uint64_t display_content_tag = 0;

    // The scale headset translation and IPD are converted at. Head offsets in
    // metres must use the same value, or the camera and the world disagree.
    float EffectiveUnitsPerMeter() const noexcept {
        return first_person_engaged ? config.first_person_units_per_meter
                                    : config.world_units_per_meter;
    }
};

// All policy functions are thread-safe. Publishing functions are intended for
// translated-game instrumentation on the guest thread; the renderer may take a
// snapshot without retaining references to mutable policy state.
void MkwVRPolicyReset() noexcept;
void MkwVRPolicyConfigure(const MkwVRPolicyConfig& config) noexcept;
void MkwVRPolicySetSessionActive(bool active) noexcept;
void MkwVRPolicySetAvailableBindings(uint32_t bindings) noexcept;
void MkwVRPolicyPublishScene(const MkwVRSceneObservation& scene) noexcept;
void MkwVRPolicyPublishRaceCamera(const MkwVRCameraObservation& camera) noexcept;
void MkwVRPolicyInvalidateRaceCamera() noexcept;
// Reported by the integration layer each time the first-person anchor engages
// or disengages. It selects the world scale and nothing else: presentation
// safety is unaffected, so this never advances the safety generation.
void MkwVRPolicySetFirstPersonEngaged(bool engaged) noexcept;
// Live world scale for the first-person camera. Separate from
// MkwVRPolicyConfigure so the F10 slider can retune it during a race without
// republishing (and revalidating) the whole configuration.
void MkwVRPolicySetFirstPersonUnitsPerMeter(float units_per_meter) noexcept;
MkwVRPolicySnapshot MkwVRPolicyGetSnapshot() noexcept;
// Cheap guest-thread gate for the many NW4R object visibility checks per frame.
// Mirrors immersive-race eligibility without taking the policy mutex per object.
bool MkwVRPolicyExpandRaceCulling() noexcept;

// This classifier is deliberately structural rather than heuristic: future
// hooks report whether the active projection is perspective/orthographic and
// whether execution is inside a post-processing boundary. No matrix-value or
// guest-address guessing occurs here.
VRDrawClass MkwVRPolicyClassifyDraw(const MkwVRDrawObservation& draw) noexcept;
VRDrawRoute MkwVRPolicyRouteDraw(VRDrawClass draw_class,
                                 const MkwVRPolicySnapshot& snapshot) noexcept;

enum class MkwVRHookCapability : uint8_t {
    SceneState,
    RaceCamera,
    DrawClassification,
    PostProcess,
    Culling,
};

// Validated PAL RMCP01 symbols from projects/mkwii/MAP.txt. Most entries are
// observation candidates. The two culling hooks wrap or extend the game's
// visibility results during immersive races.
struct MkwVRHookPoint {
    uint32_t address;
    const char* symbol;
    MkwVRHookCapability capability;
    const char* purpose;
};

const MkwVRHookPoint* MkwVRPolicyHookPoints(std::size_t* count) noexcept;

} // namespace mkw::vr
