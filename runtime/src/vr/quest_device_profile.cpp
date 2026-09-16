// SPDX-License-Identifier: GPL-3.0-or-later

#include "vr/quest_device_profile.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace mkw::vr {
namespace {

char LowerAscii(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// Substring search that ignores ASCII case. The system names this matches are
// all ASCII, and a locale-aware comparison would be both unnecessary and
// dependent on process state the VR bring-up must not rely on.
bool ContainsFold(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) {
        return true;
    }
    if (haystack.size() < needle.size()) {
        return false;
    }
    const size_t last = haystack.size() - needle.size();
    for (size_t start = 0; start <= last; ++start) {
        size_t offset = 0;
        while (offset < needle.size() &&
               LowerAscii(haystack[start + offset]) == LowerAscii(needle[offset])) {
            ++offset;
        }
        if (offset == needle.size()) {
            return true;
        }
    }
    return false;
}

// "Quest 3" and "Quest3" are both in use, and the Quest 3S must not be matched
// by the Quest 3 pattern, so each model is probed with both spellings and the
// more specific models are probed first.
bool MatchesModel(std::string_view name, std::string_view spaced,
                  std::string_view tight) {
    return ContainsFold(name, spaced) || ContainsFold(name, tight);
}

QuestDeviceProfile MakeProfile(QuestModel model, QuestTransport transport,
                               std::string_view display_name,
                               std::initializer_list<float> rates,
                               float default_refresh, float standalone_scale,
                               QuestFoveation standalone_foveation) {
    QuestDeviceProfile profile;
    profile.model = model;
    profile.transport = transport;
    profile.display_name = display_name;
    profile.recognized = model != QuestModel::Unknown;

    for (const float rate : rates) {
        if (profile.refresh_rate_count >= kQuestMaxRefreshRates) {
            break;
        }
        profile.refresh_rates[profile.refresh_rate_count++] = rate;
    }
    profile.default_refresh_rate = default_refresh;

    if (transport == QuestTransport::Standalone) {
        profile.default_render_scale = standalone_scale;
        profile.default_foveation = standalone_foveation;
        // The Quest governor idles both clocks aggressively. A recompiled Wii
        // title is CPU-heavy on the guest-execution thread and fragment-heavy
        // in stereo, so both levels are raised from the runtime default. Level
        // 3 is deliberately not the default: it is not sustainable thermally
        // and the runtime throttles out of it mid-race.
        profile.default_cpu_level = 2;
        profile.default_gpu_level = 2;
    } else {
        // A tethered session renders on the desktop GPU and is encoded by the
        // streamer. Foveation and performance levels belong to the headset-side
        // compositor there, not to this process, and the resolution budget is
        // the desktop build's.
        profile.default_render_scale = 1.0f;
        profile.default_foveation = QuestFoveation::None;
        profile.default_cpu_level = 0;
        profile.default_gpu_level = 0;
    }
    return profile;
}

} // namespace

QuestModel QuestModelFromSystemName(std::string_view system_name) {
    // Order matters: "Quest 3S" contains "Quest 3", and "Quest 2"/"Quest Pro"
    // both contain "Quest".
    if (MatchesModel(system_name, "quest 3s", "quest3s")) {
        return QuestModel::Quest3S;
    }
    if (MatchesModel(system_name, "quest 3", "quest3")) {
        return QuestModel::Quest3;
    }
    if (MatchesModel(system_name, "quest pro", "questpro")) {
        return QuestModel::QuestPro;
    }
    if (MatchesModel(system_name, "quest 2", "quest2")) {
        return QuestModel::Quest2;
    }
    // Plain "Oculus Quest" is the first-generation headset. This must stay last
    // so it cannot shadow a more specific model above.
    if (ContainsFold(system_name, "quest")) {
        return QuestModel::Quest1;
    }
    return QuestModel::Unknown;
}

QuestDeviceProfile QuestProfileFor(QuestModel model, QuestTransport transport) {
    switch (model) {
    case QuestModel::Quest1:
        // The first-generation headset has a 72 Hz panel and a Snapdragon 835.
        // It is listed so the runtime can recognize and report it; the profile
        // does not claim the device is fast enough to hold that rate.
        return MakeProfile(model, transport, "Meta Quest (1st generation)",
                           {60.0f, 72.0f}, 72.0f, 0.70f, QuestFoveation::High);
    case QuestModel::Quest2:
        return MakeProfile(model, transport, "Meta Quest 2",
                           {60.0f, 72.0f, 80.0f, 90.0f, 120.0f}, 72.0f, 0.80f,
                           QuestFoveation::Medium);
    case QuestModel::QuestPro:
        return MakeProfile(model, transport, "Meta Quest Pro",
                           {72.0f, 80.0f, 90.0f}, 72.0f, 0.85f,
                           QuestFoveation::Medium);
    case QuestModel::Quest3:
        return MakeProfile(model, transport, "Meta Quest 3",
                           {72.0f, 80.0f, 90.0f, 120.0f}, 72.0f, 0.90f,
                           QuestFoveation::Low);
    case QuestModel::Quest3S:
        return MakeProfile(model, transport, "Meta Quest 3S",
                           {72.0f, 80.0f, 90.0f, 120.0f}, 72.0f, 0.85f,
                           QuestFoveation::Medium);
    case QuestModel::Unknown:
        break;
    }
    // Unknown standalone hardware: assume the weakest configuration this port
    // targets rather than the strongest, and say so through `recognized`.
    return MakeProfile(QuestModel::Unknown, transport, "Unknown OpenXR headset",
                       {72.0f}, 72.0f, 0.75f, QuestFoveation::Medium);
}

QuestDeviceProfile QuestProfileFromSystemName(std::string_view system_name,
                                              QuestTransport transport) {
    return QuestProfileFor(QuestModelFromSystemName(system_name), transport);
}

float QuestSelectRefreshRate(const float* available, size_t available_count,
                             const QuestDeviceProfile& profile,
                             float preferred) {
    if (available == nullptr || available_count == 0) {
        return 0.0f;
    }

    const float target = (std::isfinite(preferred) && preferred > 0.0f)
                             ? preferred
                             : profile.default_refresh_rate;

    float best_at_or_below = 0.0f;
    float lowest = 0.0f;
    bool have_lowest = false;

    for (size_t index = 0; index < available_count; ++index) {
        const float rate = available[index];
        if (!std::isfinite(rate) || rate <= 0.0f) {
            continue;
        }
        if (!have_lowest || rate < lowest) {
            lowest = rate;
            have_lowest = true;
        }
        // Exact match wins outright; comparing with a small tolerance because
        // runtimes report 72.0 and 72.000004 interchangeably.
        if (std::fabs(rate - target) <= 0.01f) {
            return rate;
        }
        if (rate < target && rate > best_at_or_below) {
            best_at_or_below = rate;
        }
    }

    if (best_at_or_below > 0.0f) {
        return best_at_or_below;
    }
    return have_lowest ? lowest : 0.0f;
}

std::string_view QuestFoveationName(QuestFoveation level) {
    switch (level) {
    case QuestFoveation::None:
        return "off";
    case QuestFoveation::Low:
        return "low";
    case QuestFoveation::Medium:
        return "medium";
    case QuestFoveation::High:
        return "high";
    case QuestFoveation::HighTop:
        return "high-top";
    }
    return "off";
}

} // namespace mkw::vr
