// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Per-headset defaults for the standalone Meta Quest target.
//
// A standalone build has to choose a refresh rate, an eye resolution, a
// foveation strength and CPU/GPU performance levels before the first frame, and
// it has to choose them differently on a Quest 2 than on a Quest 3. The only
// identification available at that point is the OpenXR system name, which the
// runtime reports for both standalone and PC VR (Link) sessions.
//
// This header is deliberately free of OpenXR and Android headers so the policy
// can be reviewed and unit-tested without a loader, an SDK or a headset, the
// same way vr/openxr_controller_profiles.h is. openxr_android.cpp is the only
// place that turns these values into API calls.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace mkw::vr {

enum class QuestModel {
    Unknown,
    Quest1,
    Quest2,
    QuestPro,
    Quest3,
    Quest3S,
};

// How the session reaches the headset. The same Quest reports the same system
// name over Link as it does standalone, so the transport has to come from the
// build/runtime rather than from the name, and it changes the defaults: a Link
// session renders on a desktop GPU and must not apply standalone's reduced
// resolution or its foveation and performance-level policy.
enum class QuestTransport {
    // The application is running on the headset itself (Android build).
    Standalone,
    // The application is running on a PC and streaming to the headset
    // (Quest Link, Air Link, Virtual Desktop, Steam Link).
    Tethered,
};

// XR_FB_foveation strength. Standalone Quest rendering is fragment-bound at
// stereo resolutions, so a non-zero default is the difference between holding
// the target refresh and missing it.
enum class QuestFoveation {
    None,
    Low,
    Medium,
    High,
    HighTop,
};

inline constexpr size_t kQuestMaxRefreshRates = 5;

struct QuestDeviceProfile {
    QuestModel model = QuestModel::Unknown;
    QuestTransport transport = QuestTransport::Standalone;

    // Human-readable name for logs and the VR settings diagnostics tab.
    std::string_view display_name = "Unknown OpenXR headset";

    // Refresh rates the headset exposes through XR_FB_display_refresh_rate,
    // ascending, with refresh_rate_count entries used.
    std::array<float, kQuestMaxRefreshRates> refresh_rates{};
    size_t refresh_rate_count = 0;

    // The rate this port asks for when the user has expressed no preference.
    // Deliberately not the headset maximum: a statically recompiled Wii title
    // rendered in stereo has far more per-frame work than a native Quest title,
    // and a missed 90 Hz frame is worse than a held 72 Hz one.
    float default_refresh_rate = 72.0f;

    // Multiplier applied to the runtime-recommended eye dimensions. Standalone
    // values are below 1.0 on purpose; see default_refresh_rate.
    float default_render_scale = 1.0f;

    QuestFoveation default_foveation = QuestFoveation::None;

    // XR_EXT_performance_settings levels, expressed as a simple 0-3 boost where
    // 0 is the runtime default and 3 is the sustained maximum.
    uint32_t default_cpu_level = 0;
    uint32_t default_gpu_level = 0;

    // True when the device is known to this table. False means the profile is
    // the conservative fallback and the values are guesses, which the caller
    // logs rather than presenting as a detected configuration.
    bool recognized = false;
};

// Classifies an OpenXR system name. Matching is case-insensitive and
// substring-based because the exact string differs between the standalone
// runtime, the PC Link runtime and third-party streamers, all of which carry
// the model somewhere in the name ("Oculus Quest2", "Meta Quest 3",
// "SteamVR/OpenXR : oculus" and so on).
QuestModel QuestModelFromSystemName(std::string_view system_name);

// The profile for a model/transport pair. Always returns a usable profile;
// check QuestDeviceProfile::recognized to tell a detected device from the
// fallback.
QuestDeviceProfile QuestProfileFor(QuestModel model, QuestTransport transport);

// Convenience wrapper over the two calls above.
QuestDeviceProfile QuestProfileFromSystemName(std::string_view system_name,
                                              QuestTransport transport);

// Picks the rate to request from the rates the runtime actually advertises.
//
// `preferred` is the user's choice, or 0 when unset. The result is always one
// of `available`, because XR_FB_display_refresh_rate rejects anything else:
//   * an exact match for `preferred` wins;
//   * otherwise the highest advertised rate not exceeding `preferred` wins,
//     because overshooting the user's choice costs frames;
//   * a `preferred` below every advertised rate takes the lowest;
//   * with no preference the profile default is resolved the same way.
// Returns 0 when `available` is empty, which tells the caller to leave the
// runtime's own rate alone.
float QuestSelectRefreshRate(const float* available, size_t available_count,
                             const QuestDeviceProfile& profile,
                             float preferred);

// Name for logs and diagnostics.
std::string_view QuestFoveationName(QuestFoveation level);

} // namespace mkw::vr
