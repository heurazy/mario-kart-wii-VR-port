// SPDX-License-Identifier: GPL-3.0-or-later

#include "vr/mkw_vr_policy.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>

namespace mkw::vr {
namespace {

constexpr MkwVRPolicyConfig kDefaultConfig{};

struct PolicyState {
    MkwVRPolicyConfig config = kDefaultConfig;
    MkwVRSceneObservation scene{};
    MkwVRCameraObservation camera{};
    uint32_t available_bindings = MkwVRBindingNone;
    bool session_active = false;
    bool first_person_engaged = false;
    bool settings_visible = false;
    uint64_t safety_generation = 1;
};

std::mutex g_policy_mutex;
PolicyState g_policy;
std::atomic<bool> g_expand_race_culling{false};

uint32_t FloatBits(const float* value) noexcept {
    uint32_t bits = 0;
    std::memcpy(&bits, value, sizeof(bits));
    return bits;
}

bool IsFiniteFloat(const float* value) noexcept {
    // mkw_runtime_common is built with -ffast-math, which permits the compiler
    // to fold std::isfinite to true. Inspecting the object representation keeps
    // this validation effective under the target's real compile flags.
    return (FloatBits(value) & 0x7F800000u) != 0x7F800000u;
}

bool IsFinitePositive(const float* value) noexcept {
    const uint32_t bits = FloatBits(value);
    return (bits & 0x80000000u) == 0 && (bits & 0x7FFFFFFFu) != 0 &&
           (bits & 0x7F800000u) != 0x7F800000u;
}

MkwVRPolicyConfig SanitizeConfig(const MkwVRPolicyConfig& config) noexcept {
    MkwVRPolicyConfig sanitized = config;
    if (!IsFinitePositive(&sanitized.world_units_per_meter)) {
        sanitized.world_units_per_meter = kDefaultConfig.world_units_per_meter;
    }
    if (!IsFinitePositive(&sanitized.hud_distance_meters)) {
        sanitized.hud_distance_meters = kDefaultConfig.hud_distance_meters;
    }
    if (!IsFinitePositive(&sanitized.hud_width_meters)) {
        sanitized.hud_width_meters = kDefaultConfig.hud_width_meters;
    }
    if (!IsFinitePositive(&sanitized.hud_scale)) {
        sanitized.hud_scale = kDefaultConfig.hud_scale;
    }
    if (!IsFinitePositive(&sanitized.first_person_units_per_meter)) {
        sanitized.first_person_units_per_meter = kDefaultConfig.first_person_units_per_meter;
    }
    return sanitized;
}

bool IsFiniteCamera(const MkwVRCameraObservation& camera) noexcept {
    return camera.valid &&
           std::all_of(camera.view_from_world.begin(), camera.view_from_world.end(),
                       [](const float& value) { return IsFiniteFloat(&value); });
}

bool ObservationsAreCoherent(const MkwVRSceneObservation& scene,
                             const MkwVRCameraObservation& camera) noexcept {
    // RaceCamera::Update for guest frame N+1 can run before ScnMgrRace::Draw
    // publishes the scene observation for that frame. The OpenXR pacing thread
    // is independent and can legitimately sample that short interval. Accept
    // adjacent frames from the same live RaceScene; larger gaps still fail
    // closed, and scene transitions explicitly invalidate the camera.
    const uint64_t newer = std::max(scene.guest_frame_index, camera.guest_frame_index);
    const uint64_t older = std::min(scene.guest_frame_index, camera.guest_frame_index);
    return newer - older <= 1;
}

VRPresentationMode SelectPresentation(const PolicyState& state) noexcept {
    if (!state.config.enabled || !state.session_active) {
        return VRPresentationMode::Desktop;
    }

    // A virtual screen is the fail-safe for menus, split-screen, and any
    // incomplete instrumentation. It preserves the unmodified render path.
    if ((state.available_bindings & kMkwVRRequiredImmersiveBindings) !=
        kMkwVRRequiredImmersiveBindings ||
        state.settings_visible || !state.config.immersive_races || state.scene.mode != VRSceneMode::Race ||
        state.scene.local_player_count != 1 || !IsFiniteCamera(state.camera) ||
        !ObservationsAreCoherent(state.scene, state.camera)) {
        return VRPresentationMode::VirtualScreen;
    }

    return VRPresentationMode::ImmersiveRace;
}

VRPresentationMode SelectStablePresentation(const PolicyState& state) noexcept {
    if (!state.config.enabled || !state.session_active) {
        return VRPresentationMode::Desktop;
    }

    // Deliberately omit the per-frame scene/camera index comparison here.
    // Those observations are published by separate translated callbacks, so
    // their temporary mismatch is represented in content_tag's mode bits
    // without advancing the generation twice on every healthy race frame.
    if ((state.available_bindings & kMkwVRRequiredImmersiveBindings) !=
            kMkwVRRequiredImmersiveBindings ||
        state.settings_visible || !state.config.immersive_races || state.scene.mode != VRSceneMode::Race ||
        state.scene.local_player_count != 1 || !IsFiniteCamera(state.camera)) {
        return VRPresentationMode::VirtualScreen;
    }

    return VRPresentationMode::ImmersiveRace;
}

void AdvanceSafetyGeneration(PolicyState& state) noexcept {
    // Two low bits are reserved for VRPresentationMode in MakeContentTag().
    // Keep UINT64_MAX reserved as Aurora's explicit unknown-tag sentinel.
    constexpr uint64_t kMaxSafetyGeneration = (UINT64_MAX >> 2) - 1;
    if (state.safety_generation >= kMaxSafetyGeneration) {
        state.safety_generation = 1;
    } else {
        ++state.safety_generation;
    }
}

template <typename Mutation>
void ApplyPolicyMutation(Mutation&& mutation) noexcept {
    const VRPresentationMode previous = SelectStablePresentation(g_policy);
    mutation();
    if (SelectStablePresentation(g_policy) != previous) {
        AdvanceSafetyGeneration(g_policy);
    }
    // Culling runs during game simulation, before the separately published
    // scene/camera observations necessarily have matching frame indices.
    // The stable race state is the correct guard for this CPU-side superset.
    g_expand_race_culling.store(
        SelectStablePresentation(g_policy) == VRPresentationMode::ImmersiveRace,
        std::memory_order_relaxed);
}

uint64_t MakeContentTag(const PolicyState& state, VRPresentationMode presentation) noexcept {
    static_assert(static_cast<uint64_t>(VRPresentationMode::ImmersiveRace) < 4,
                  "VRPresentationMode must fit in the content tag's reserved bits");
    return (state.safety_generation << 2) | static_cast<uint64_t>(presentation);
}

constexpr MkwVRHookPoint kHookPoints[] = {
    {0x80562B34u, "ScnMgr::UpdateCameras", MkwVRHookCapability::SceneState,
     "Observe the active scene camera update boundary."},
    {0x80562BF0u, "GameCamera::GetViewMatrix", MkwVRHookCapability::RaceCamera,
     "Observe the common camera view matrix consumed by ScnMgr."},
    {0x805B2110u, "ScnMgrRace::UpdateCameras", MkwVRHookCapability::SceneState,
     "Identify a race camera update without relying on a scene object layout."},
    {0x805B1CD8u, "ScnMgrRace::Draw", MkwVRHookCapability::DrawClassification,
     "Bracket race-scene drawing for perspective-world classification."},
    {0x805A21D0u, "RaceCamera::Update", MkwVRHookCapability::RaceCamera,
     "Observe the stable post-update race camera for the current guest frame."},
    {0x805A6C58u, "RaceCamera::GetViewMtx", MkwVRHookCapability::RaceCamera,
     "Copy the returned 3x4 view matrix through a future translated observer."},
    {0x805A906Cu, "RaceCameraMgr::ApplyShaking", MkwVRHookCapability::RaceCamera,
     "Separate game camera shake from headset motion when a comfort policy is added."},
    {0x80565DA0u, "GameScreen::SetAndLoadOrthoProj",
     MkwVRHookCapability::DrawClassification,
     "Mark orthographic GameScreen work as HUD or flat-screen content."},
    {0x80566020u, "GameScreen::SetAndLoadProjection",
     MkwVRHookCapability::DrawClassification,
     "Observe GameScreen projection changes used by race and menu UI."},
    {0x805661E8u, "GameScreen::SetProjection", MkwVRHookCapability::DrawClassification,
     "Observe projection setup without assuming GameScreen member offsets."},
    {0x800640D0u, "nw4r::g3d::G3DState::SetCameraProjMtx",
     MkwVRHookCapability::DrawClassification,
     "Provide a renderer-level perspective/orthographic projection boundary."},
    {0x8006AA80u, "nw4r::g3d::Camera::GXSetProjection",
     MkwVRHookCapability::DrawClassification,
     "Observe the final NW4R camera projection submitted to GX."},
    {0x8054F41Cu, "GameScreenEffectsMgr::Draw", MkwVRHookCapability::PostProcess,
     "Bracket screen effects that must be evaluated per eye or composed flat."},
    {0x8054F7A4u, "GameScreenEffectsMgr::DrawCourseFilterEffects",
     MkwVRHookCapability::PostProcess,
     "Classify full-screen course filters as post-processing."},
    {0x8054F8E0u, "GameScreenEffectsMgr::CopyEFBToLensFlareTextures",
     MkwVRHookCapability::PostProcess,
     "Track EFB-dependent lens-flare capture separately from world geometry."},
    {0x80086610u, "nw4r::math::FRUSTUM::IntersectAABB_Ex", MkwVRHookCapability::Culling,
     "Keep nearby course objects eligible when the HMD looks outside the kart camera frustum."},
    {0x80787774u, "ClipInfoMgr::Update", MkwVRHookCapability::Culling,
     "Keep nearby items and map objects visible outside the kart-facing clip planes."},
};

} // namespace

void MkwVRPolicyReset() noexcept {
    std::lock_guard<std::mutex> lock(g_policy_mutex);
    g_policy = PolicyState{};
    g_expand_race_culling.store(false, std::memory_order_relaxed);
}

void MkwVRPolicyConfigure(const MkwVRPolicyConfig& config) noexcept {
    std::lock_guard<std::mutex> lock(g_policy_mutex);
    ApplyPolicyMutation([&] { g_policy.config = SanitizeConfig(config); });
}

void MkwVRPolicySetSessionActive(bool active) noexcept {
    std::lock_guard<std::mutex> lock(g_policy_mutex);
    ApplyPolicyMutation([&] { g_policy.session_active = active; });
}

void MkwVRPolicySetAvailableBindings(uint32_t bindings) noexcept {
    std::lock_guard<std::mutex> lock(g_policy_mutex);
    ApplyPolicyMutation([&] { g_policy.available_bindings = bindings; });
}

void MkwVRPolicyPublishScene(const MkwVRSceneObservation& scene) noexcept {
    std::lock_guard<std::mutex> lock(g_policy_mutex);
    ApplyPolicyMutation([&] {
        if (scene.mode != VRSceneMode::Race || g_policy.scene.mode != VRSceneMode::Race) {
            // Never carry a camera sample across a menu/replay-to-race transition.
            // A fresh RaceCamera observation must arrive before immersive mode can
            // become active again.
            g_policy.camera.valid = false;
        }
        g_policy.scene = scene;
    });
}

void MkwVRPolicyPublishRaceCamera(const MkwVRCameraObservation& camera) noexcept {
    std::lock_guard<std::mutex> lock(g_policy_mutex);
    ApplyPolicyMutation([&] {
        g_policy.camera = camera;
        g_policy.camera.valid = IsFiniteCamera(camera);
    });
}

void MkwVRPolicyInvalidateRaceCamera() noexcept {
    std::lock_guard<std::mutex> lock(g_policy_mutex);
    ApplyPolicyMutation([&] { g_policy.camera.valid = false; });
}

void MkwVRPolicySetSettingsVisible(bool visible) noexcept {
    std::lock_guard lock(g_policy_mutex);
    // UI is composed into the mono snapshot, then projected into both eyes.
    // Invalidate cached race images on entry and cached UI images on exit.
    ApplyPolicyMutation([&] { g_policy.settings_visible = visible; });
}

void MkwVRPolicySetFirstPersonEngaged(bool engaged) noexcept {
    std::lock_guard<std::mutex> lock(g_policy_mutex);
    // Not routed through ApplyPolicyMutation: where the camera sits does not
    // change which content is safe to present, and advancing the safety
    // generation here would drop a frame to mono on every engage.
    g_policy.first_person_engaged = engaged;
}

void MkwVRPolicySetFirstPersonUnitsPerMeter(float units_per_meter) noexcept {
    std::lock_guard<std::mutex> lock(g_policy_mutex);
    if (IsFinitePositive(&units_per_meter)) {
        g_policy.config.first_person_units_per_meter = units_per_meter;
    }
}

MkwVRPolicySnapshot MkwVRPolicyGetSnapshot() noexcept {
    std::lock_guard<std::mutex> lock(g_policy_mutex);
    MkwVRPolicySnapshot snapshot;
    snapshot.presentation = SelectPresentation(g_policy);
    snapshot.config = g_policy.config;
    snapshot.scene = g_policy.scene;
    snapshot.camera = g_policy.camera;
    snapshot.available_bindings = g_policy.available_bindings;
    snapshot.session_active = g_policy.session_active;
    snapshot.settings_visible = g_policy.settings_visible;
    snapshot.first_person_engaged =
        g_policy.first_person_engaged && snapshot.presentation == VRPresentationMode::ImmersiveRace;
    snapshot.safety_generation = g_policy.safety_generation;
    snapshot.content_tag = MakeContentTag(g_policy, snapshot.presentation);
    snapshot.display_content_tag = MakeContentTag(g_policy, SelectStablePresentation(g_policy));
    return snapshot;
}

bool MkwVRPolicyExpandRaceCulling() noexcept {
    return g_expand_race_culling.load(std::memory_order_relaxed);
}

VRDrawClass MkwVRPolicyClassifyDraw(const MkwVRDrawObservation& draw) noexcept {
    if (draw.pass == VRDrawPass::PostProcess) {
        return VRDrawClass::PostProcess;
    }
    switch (draw.projection) {
    case VRProjectionKind::Perspective:
        return VRDrawClass::PerspectiveWorld;
    case VRProjectionKind::Orthographic:
        return VRDrawClass::OrthographicHud;
    case VRProjectionKind::Unknown:
        return VRDrawClass::Unknown;
    }
    return VRDrawClass::Unknown;
}

VRDrawRoute MkwVRPolicyRouteDraw(VRDrawClass draw_class,
                                 const MkwVRPolicySnapshot& snapshot) noexcept {
    switch (snapshot.presentation) {
    case VRPresentationMode::Desktop:
        return VRDrawRoute::DesktopPassthrough;
    case VRPresentationMode::VirtualScreen:
        return VRDrawRoute::VirtualScreen;
    case VRPresentationMode::ImmersiveRace:
        break;
    }

    switch (draw_class) {
    case VRDrawClass::PerspectiveWorld:
        return VRDrawRoute::StereoWorld;
    case VRDrawClass::OrthographicHud:
        return VRDrawRoute::HeadLockedHud;
    case VRDrawClass::PostProcess:
        return VRDrawRoute::PerEyePostProcess;
    case VRDrawClass::Unknown:
        return VRDrawRoute::Unclassified;
    }
    return VRDrawRoute::Unclassified;
}

const MkwVRHookPoint* MkwVRPolicyHookPoints(std::size_t* count) noexcept {
    if (count != nullptr) {
        *count = sizeof(kHookPoints) / sizeof(kHookPoints[0]);
    }
    return kHookPoints;
}

} // namespace mkw::vr
