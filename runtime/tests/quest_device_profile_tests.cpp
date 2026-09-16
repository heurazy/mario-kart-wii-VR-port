// SPDX-License-Identifier: GPL-3.0-or-later

#include "vr/quest_device_profile.h"

#include <array>
#include <iostream>
#include <string_view>

namespace {

int failures = 0;

void Check(bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAILED: " << message << '\n';
    }
}

using namespace mkw::vr;

void ModelDetection() {
    // The standalone runtime, the PC Link runtime and third-party streamers all
    // spell the same headset differently. Every spelling seen in the wild has to
    // land on the same model.
    Check(QuestModelFromSystemName("Oculus Quest2") == QuestModel::Quest2,
          "Link reports Quest 2 as 'Oculus Quest2'");
    Check(QuestModelFromSystemName("Meta Quest 2") == QuestModel::Quest2,
          "standalone reports Quest 2 with a space");
    Check(QuestModelFromSystemName("Meta Quest 3") == QuestModel::Quest3,
          "Quest 3 is detected");
    Check(QuestModelFromSystemName("Meta Quest 3S") == QuestModel::Quest3S,
          "Quest 3S is detected");
    Check(QuestModelFromSystemName("Meta Quest Pro") == QuestModel::QuestPro,
          "Quest Pro is detected");
    Check(QuestModelFromSystemName("Oculus Quest") == QuestModel::Quest1,
          "the first-generation headset is detected");

    // The 3S contains the 3's pattern; a naive ordering would report a Quest 3.
    Check(QuestModelFromSystemName("quest3s") == QuestModel::Quest3S,
          "Quest 3S is not shadowed by the Quest 3 pattern");
    // "Quest" alone must not shadow the specific models.
    Check(QuestModelFromSystemName("Meta Quest 3") != QuestModel::Quest1,
          "the generic Quest pattern does not shadow Quest 3");

    Check(QuestModelFromSystemName("SteamVR/OpenXR : oculus quest2") ==
              QuestModel::Quest2,
          "a SteamVR system name still carries the model");
    Check(QuestModelFromSystemName("Index") == QuestModel::Unknown,
          "a non-Quest headset is Unknown");
    Check(QuestModelFromSystemName("") == QuestModel::Unknown,
          "an empty system name is Unknown");
}

void ProfileDefaults() {
    const auto quest3 = QuestProfileFor(QuestModel::Quest3,
                                        QuestTransport::Standalone);
    Check(quest3.recognized, "a known model is reported as recognized");
    Check(quest3.refresh_rate_count > 0, "a known model lists refresh rates");
    Check(quest3.default_render_scale > 0.0f &&
              quest3.default_render_scale <= 1.0f,
          "the standalone render scale is a sane multiplier");
    Check(quest3.default_foveation != QuestFoveation::None,
          "standalone rendering defaults to non-zero foveation");
    Check(quest3.default_cpu_level > 0 && quest3.default_gpu_level > 0,
          "standalone raises both performance levels off the runtime default");
    Check(quest3.default_cpu_level < 3 && quest3.default_gpu_level < 3,
          "standalone does not default to the unsustainable maximum level");

    // A tethered session renders on the desktop GPU: the headset-side knobs
    // belong to the streamer, not to this process.
    const auto tethered = QuestProfileFor(QuestModel::Quest3,
                                          QuestTransport::Tethered);
    Check(tethered.default_render_scale == 1.0f,
          "a tethered session does not apply the standalone resolution cut");
    Check(tethered.default_foveation == QuestFoveation::None,
          "a tethered session does not apply foveation itself");
    Check(tethered.default_cpu_level == 0 && tethered.default_gpu_level == 0,
          "a tethered session leaves performance levels to the runtime");

    const auto unknown = QuestProfileFor(QuestModel::Unknown,
                                         QuestTransport::Standalone);
    Check(!unknown.recognized, "the fallback profile reports itself as such");
    Check(unknown.default_render_scale <= quest3.default_render_scale,
          "unknown hardware assumes the weaker configuration, not the stronger");

    // The weaker headsets must not be given the stronger one's budget.
    const auto quest2 = QuestProfileFor(QuestModel::Quest2,
                                        QuestTransport::Standalone);
    Check(quest2.default_render_scale < quest3.default_render_scale,
          "Quest 2 gets a smaller resolution budget than Quest 3");
}

void RefreshRateSelection() {
    const auto profile = QuestProfileFor(QuestModel::Quest3,
                                         QuestTransport::Standalone);
    const std::array<float, 4> rates{72.0f, 80.0f, 90.0f, 120.0f};

    Check(QuestSelectRefreshRate(rates.data(), rates.size(), profile, 90.0f) ==
              90.0f,
          "an exact preferred rate is selected");
    Check(QuestSelectRefreshRate(rates.data(), rates.size(), profile, 0.0f) ==
              profile.default_refresh_rate,
          "no preference resolves to the profile default");

    // Overshooting the user's choice costs frames, so round down, never up.
    Check(QuestSelectRefreshRate(rates.data(), rates.size(), profile, 100.0f) ==
              90.0f,
          "an unavailable rate rounds down to the next advertised rate");
    Check(QuestSelectRefreshRate(rates.data(), rates.size(), profile, 60.0f) ==
              72.0f,
          "a preference below every advertised rate takes the lowest");

    // XR_FB_display_refresh_rate rejects anything the runtime did not list, so
    // the result must always come from the advertised set.
    const std::array<float, 2> sparse{72.0f, 90.0f};
    const float chosen =
        QuestSelectRefreshRate(sparse.data(), sparse.size(), profile, 80.0f);
    Check(chosen == 72.0f, "selection is restricted to advertised rates");

    Check(QuestSelectRefreshRate(nullptr, 0, profile, 90.0f) == 0.0f,
          "an empty rate list leaves the runtime rate alone");

    // A runtime that reports a bad entry must not make the whole call fail.
    const std::array<float, 3> dirty{0.0f, -1.0f, 72.0f};
    Check(QuestSelectRefreshRate(dirty.data(), dirty.size(), profile, 90.0f) ==
              72.0f,
          "non-positive advertised rates are ignored");

    // Runtimes report 72.0 and 72.000004 interchangeably.
    const std::array<float, 1> jittered{72.000004f};
    Check(QuestSelectRefreshRate(jittered.data(), jittered.size(), profile,
                                 72.0f) == 72.000004f,
          "an exact match tolerates float representation jitter");
}

void FoveationNames() {
    Check(QuestFoveationName(QuestFoveation::None) == "off",
          "foveation levels have diagnostic names");
    Check(QuestFoveationName(QuestFoveation::HighTop) == "high-top",
          "the high-top foveation level is named");
}

} // namespace

int main() {
    ModelDetection();
    ProfileDefaults();
    RefreshRateSelection();
    FoveationNames();
    if (failures == 0) {
        std::cout << "quest device profile tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
