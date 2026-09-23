// SPDX-License-Identifier: GPL-3.0-or-later

#include "vr/mkw_vr_instrumentation.h"

#include "memory.h"
#include "ppc_runtime.h"
#include "runtime_log.h"
#include "vr/mkw_vr_first_person.h"
#include "vr/mkw_vr_policy.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <mutex>

extern "C" int g_gxFrameCount;

namespace mkw::vr {
namespace {

constexpr uint32_t kRaceSceneOnEnter = 0x80553C50u;
constexpr uint32_t kRaceSceneOnExit = 0x805549B0u;
constexpr uint32_t kRaceCameraUpdate = 0x805A21D0u;
constexpr uint32_t kScnMgrRaceDraw = 0x805B1CD8u;
// PAL RMCP01 RaceScene::GetScreenCount (0x80554F68) reads the active
// RaceScene pointer from this SDA-backed slot, then returns byte 0x25. Keep
// this synchronized with projects/mkwii/MAP.txt and the generated function.
constexpr uint32_t kRaceSceneInstanceAddress = 0x809BD728u;
constexpr uint32_t kRaceSceneScreenCountOffset = 0x25u;

struct InstrumentationState {
    uint64_t guest_frame = 0;
    std::array<uint32_t, 4> cameras{};
    uint32_t camera_count = 0;
    uint32_t last_reported_camera_count = UINT32_MAX;
    uint32_t last_reported_screen_count = UINT32_MAX;
};

std::mutex g_instrumentation_mutex;
InstrumentationState g_instrumentation;

uint64_t GuestFrame() noexcept {
    return static_cast<uint64_t>(static_cast<uint32_t>(g_gxFrameCount));
}

void ResetCamerasForFrameLocked(uint64_t frame) noexcept {
    if (g_instrumentation.guest_frame == frame) {
        return;
    }
    g_instrumentation.guest_frame = frame;
    g_instrumentation.cameras = {};
    g_instrumentation.camera_count = 0;
}

uint32_t ObserveCamera(uint64_t frame, uint32_t camera) noexcept {
    std::lock_guard lock(g_instrumentation_mutex);
    ResetCamerasForFrameLocked(frame);
    const auto first = g_instrumentation.cameras.begin();
    const auto last = first + g_instrumentation.camera_count;
    if (camera != 0 && std::find(first, last, camera) == last &&
        g_instrumentation.camera_count < g_instrumentation.cameras.size()) {
        g_instrumentation.cameras[g_instrumentation.camera_count++] = camera;
    }
    return g_instrumentation.camera_count;
}

uint32_t CameraCount(uint64_t frame) noexcept {
    std::lock_guard lock(g_instrumentation_mutex);
    ResetCamerasForFrameLocked(frame);
    return g_instrumentation.camera_count;
}

// The first RaceCamera updated this frame. The game also updates cameras for
// transitions and effects, so first-wins is what keeps the first-person anchor
// deterministic: Mario Kart updates the racers' cameras before those.
uint32_t FirstCamera(uint64_t frame) noexcept {
    std::lock_guard lock(g_instrumentation_mutex);
    ResetCamerasForFrameLocked(frame);
    return g_instrumentation.camera_count != 0 ? g_instrumentation.cameras[0] : 0;
}

uint32_t RaceScreenCount() noexcept {
    uint32_t race_scene = 0;
    if (!Memory::TryRead32(kRaceSceneInstanceAddress, race_scene) || race_scene == 0 ||
        !Memory::Contains(race_scene + kRaceSceneScreenCountOffset)) {
        return 0;
    }

    try {
        const uint32_t screen_count =
            Memory::Read8(race_scene + kRaceSceneScreenCountOffset);
        return screen_count <= 4 ? screen_count : 0;
    } catch (const Memory::AccessViolation&) {
        return 0;
    }
}

void LogRaceEvidenceIfChanged(uint64_t frame, uint32_t screen_count,
                              uint32_t camera_count) noexcept {
    bool changed = false;
    {
        std::lock_guard lock(g_instrumentation_mutex);
        changed = g_instrumentation.last_reported_screen_count != screen_count ||
                  g_instrumentation.last_reported_camera_count != camera_count;
        g_instrumentation.last_reported_screen_count = screen_count;
        g_instrumentation.last_reported_camera_count = camera_count;
    }
    if (changed) {
        RT_LOG(RT_TAG_RUNTIME) << "[mkw-vr] race evidence: frame=" << frame
                               << ", screens=" << screen_count
                               << ", updated cameras=" << camera_count << std::endl;
    }
}

void PublishRaceScene(uint64_t frame, uint32_t local_players) noexcept {
    MkwVRSceneObservation scene{};
    scene.mode = VRSceneMode::Race;
    scene.local_player_count = local_players;
    scene.guest_frame_index = frame;
    MkwVRPolicyPublishScene(scene);
}

void PublishObservedCamera(uint64_t frame, uint32_t address) noexcept {
    // Aurora's stereo replay composes the OpenXR eye transform with the GX
    // matrices recorded by the original game. The policy only needs evidence
    // that the RaceCamera path ran; it never consumes this identity sample to
    // replace the game's view matrix.
    MkwVRCameraObservation camera{};
    camera.view_from_world = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
    };
    camera.guest_camera_address = address;
    camera.guest_frame_index = frame;
    camera.valid = address != 0;
    MkwVRPolicyPublishRaceCamera(camera);
}

} // namespace

void MkwVRInstrumentationInitialize() noexcept {
    MkwVRPolicySetAvailableBindings(
        MkwVRBindingSceneState | MkwVRBindingRaceCamera |
        MkwVRBindingDrawClassification | MkwVRBindingCulling);
}

} // namespace mkw::vr

extern "C" void MkwVRObserveTranslatedFunctionEntry(uint32_t address,
                                                      const CpuContext* context) noexcept {
    using namespace mkw::vr;
    const uint64_t frame = GuestFrame();
    switch (address) {
    case kRaceSceneOnEnter: {
        {
            std::lock_guard lock(g_instrumentation_mutex);
            g_instrumentation = {};
            g_instrumentation.guest_frame = frame;
        }
        PublishRaceScene(frame, 0);
        MkwVRPolicyInvalidateRaceCamera();
        MkwVRFirstPersonReset();
        RT_LOG(RT_TAG_RUNTIME) << "[mkw-vr] entered RaceScene at frame " << frame
                               << std::endl;
        break;
    }
    case kRaceCameraUpdate: {
        const uint32_t camera_address = context != nullptr ? context->gpr[3] : 0;
        ObserveCamera(frame, camera_address);
        PublishObservedCamera(frame, camera_address);
        break;
    }
    case kScnMgrRaceDraw: {
        // RaceCamera objects are not a player-count source: the game may update
        // additional cameras for transitions and effects. Use the same exact
        // screen count as RaceScene::GetScreenCount, while retaining the camera
        // hook as independent evidence that a usable race camera is current.
        const uint32_t camera_count = CameraCount(frame);
        const uint32_t screen_count = RaceScreenCount();
        LogRaceEvidenceIfChanged(frame, screen_count, camera_count);
        PublishRaceScene(frame, screen_count);
        // Every kart and camera has been updated for this frame and none of
        // the frame's draws have been issued yet, so the values behind these
        // pointers are exactly the ones those draws will use.
        MkwVRFirstPersonUpdate(frame, FirstCamera(frame));
        break;
    }
    case kRaceSceneOnExit: {
        MkwVRSceneObservation scene{};
        scene.mode = VRSceneMode::Other;
        scene.guest_frame_index = frame;
        MkwVRPolicyPublishScene(scene);
        MkwVRPolicyInvalidateRaceCamera();
        MkwVRFirstPersonReset();
        RT_LOG(RT_TAG_RUNTIME) << "[mkw-vr] exited RaceScene at frame " << frame
                               << std::endl;
        break;
    }
    default:
        break;
    }
}
