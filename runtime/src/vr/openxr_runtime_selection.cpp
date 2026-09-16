// SPDX-License-Identifier: GPL-3.0-or-later

#include "vr/openxr_runtime_selection.h"

#include <algorithm>

namespace mkw::vr {
namespace {

char LowerAscii(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool EqualsFold(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (size_t index = 0; index < left.size(); ++index) {
        if (LowerAscii(left[index]) != LowerAscii(right[index])) {
            return false;
        }
    }
    return true;
}

const OpenXRRuntimeCandidate* Find(const std::vector<OpenXRRuntimeCandidate>& candidates,
                                   OpenXRRuntimeKind kind) {
    const auto found = std::find_if(candidates.begin(), candidates.end(),
                                    [kind](const OpenXRRuntimeCandidate& candidate) {
                                        return candidate.kind == kind &&
                                               !candidate.manifest_path.empty();
                                    });
    return found == candidates.end() ? nullptr : &*found;
}

} // namespace

OpenXRRuntimeKind OpenXRRuntimePreferenceFromString(std::string_view preference, bool& is_auto,
                                                    bool& is_system, bool& recognized) {
    is_auto = false;
    is_system = false;
    recognized = true;

    if (preference.empty() || EqualsFold(preference, "auto")) {
        is_auto = true;
        return OpenXRRuntimeKind::Unknown;
    }
    if (EqualsFold(preference, "system")) {
        is_system = true;
        return OpenXRRuntimeKind::Unknown;
    }
    // "oculus" is accepted because that is what the runtime called itself for
    // years and what most existing guides still say.
    if (EqualsFold(preference, "meta") || EqualsFold(preference, "oculus") ||
        EqualsFold(preference, "link")) {
        return OpenXRRuntimeKind::Meta;
    }
    if (EqualsFold(preference, "virtualdesktop") || EqualsFold(preference, "vdxr") ||
        EqualsFold(preference, "virtual-desktop")) {
        return OpenXRRuntimeKind::VirtualDesktop;
    }
    if (EqualsFold(preference, "steamvr") || EqualsFold(preference, "steam")) {
        return OpenXRRuntimeKind::SteamVR;
    }

    recognized = false;
    is_auto = true;
    return OpenXRRuntimeKind::Unknown;
}

OpenXRRuntimeSelection SelectOpenXRRuntime(const std::vector<OpenXRRuntimeCandidate>& candidates,
                                           std::string_view preference,
                                           bool system_default_present) {
    bool is_auto = false;
    bool is_system = false;
    bool recognized = true;
    const OpenXRRuntimeKind wanted =
        OpenXRRuntimePreferenceFromString(preference, is_auto, is_system, recognized);

    OpenXRRuntimeSelection selection;
    if (!recognized) {
        selection.downgrade_reason =
            "unrecognised [vr] runtime value \"" + std::string(preference) + "\"; using auto";
    }

    // "system" is absolute: the user asked for no interference at all.
    if (is_system) {
        return selection;
    }

    if (!is_auto) {
        if (const OpenXRRuntimeCandidate* candidate = Find(candidates, wanted)) {
            selection.manifest_path = candidate->manifest_path;
            selection.kind = candidate->kind;
            return selection;
        }
        // The requested runtime is not installed. Say so and continue as auto
        // rather than refusing to start, which is what the old SteamVR-only
        // path did.
        std::string reason = std::string(OpenXRRuntimeKindName(wanted)) +
                             " was requested but is not installed; using the "
                             "system's active OpenXR runtime";
        if (!selection.downgrade_reason.empty()) {
            selection.downgrade_reason += "; " + reason;
        } else {
            selection.downgrade_reason = std::move(reason);
        }
    }

    // Auto. Respect whatever the user has made active; this is the correct
    // default for an OpenXR application and is what lets Quest Link, Virtual
    // Desktop, SteamVR and every other runtime work without configuration.
    if (system_default_present) {
        return selection;
    }

    // No active runtime at all. Rather than fail, probe for one that is
    // installed. Meta first because a Quest with the desktop app installed is
    // the most common case for this port; Virtual Desktop next because a user
    // running it has it deliberately; SteamVR last because it is the heaviest
    // and most likely to be present incidentally.
    for (const OpenXRRuntimeKind kind : {OpenXRRuntimeKind::Meta,
                                         OpenXRRuntimeKind::VirtualDesktop,
                                         OpenXRRuntimeKind::SteamVR}) {
        if (const OpenXRRuntimeCandidate* candidate = Find(candidates, kind)) {
            selection.manifest_path = candidate->manifest_path;
            selection.kind = candidate->kind;
            std::string reason = "no active OpenXR runtime is configured; selected " +
                                 std::string(OpenXRRuntimeKindName(kind));
            if (!selection.downgrade_reason.empty()) {
                selection.downgrade_reason += "; " + reason;
            } else {
                selection.downgrade_reason = std::move(reason);
            }
            return selection;
        }
    }

    if (selection.downgrade_reason.empty()) {
        selection.downgrade_reason =
            "no OpenXR runtime is active or installed; VR will fall back to the desktop mirror";
    }
    return selection;
}

std::string_view OpenXRRuntimeKindName(OpenXRRuntimeKind kind) {
    switch (kind) {
    case OpenXRRuntimeKind::Meta:
        return "Meta (Quest Link / Air Link)";
    case OpenXRRuntimeKind::VirtualDesktop:
        return "Virtual Desktop (VDXR)";
    case OpenXRRuntimeKind::SteamVR:
        return "SteamVR";
    case OpenXRRuntimeKind::Unknown:
        break;
    }
    return "the system's active OpenXR runtime";
}

} // namespace mkw::vr
