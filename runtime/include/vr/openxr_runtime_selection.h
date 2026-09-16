// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Which OpenXR runtime this process should use.
//
// The port originally selected SteamVR unconditionally and threw when it was
// absent, which made a Quest unusable over Quest Link or Virtual Desktop even
// though both expose a perfectly good OpenXR runtime. A well-behaved OpenXR
// application uses whichever runtime the user has made active and only
// overrides that on request, so this is the policy:
//
//   auto (default) - use the system's active runtime. Only if the system has
//                    no active runtime at all does this fall back to probing
//                    for a known one.
//   meta / virtualdesktop / steamvr - force that runtime when it is installed.
//   system         - never override, even if nothing is active.
//
// Nothing here can fail the launch. A preference that cannot be honoured is
// reported and downgraded, and a process with no usable runtime simply runs on
// the desktop mirror, which is what `[vr] required = false` already means.
//
// The policy is deliberately free of Win32 and OpenXR headers so it can be
// unit-tested without a headset, a registry or a loader; discovery lives in
// the platform code that calls it.

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace mkw::vr {

enum class OpenXRRuntimeKind {
    Unknown,
    // Meta's PC runtime, used by Quest Link and Air Link.
    Meta,
    // Virtual Desktop's own runtime (VDXR).
    VirtualDesktop,
    SteamVR,
};

struct OpenXRRuntimeCandidate {
    OpenXRRuntimeKind kind = OpenXRRuntimeKind::Unknown;
    // Path to the runtime's OpenXR manifest. Never empty for a candidate.
    std::string manifest_path;
};

struct OpenXRRuntimeSelection {
    // Empty means "change nothing": the system's active runtime is used as-is.
    std::string manifest_path;
    OpenXRRuntimeKind kind = OpenXRRuntimeKind::Unknown;
    // Set when a preference could not be honoured, for the log. Not an error:
    // the selection is still usable.
    std::string downgrade_reason;
};

// Parses a `[vr] runtime` value. Unrecognised text is treated as "auto" and
// reported through `recognized` so the caller can log it once.
OpenXRRuntimeKind OpenXRRuntimePreferenceFromString(std::string_view preference,
                                                    bool& is_auto, bool& is_system,
                                                    bool& recognized);

// Chooses a runtime.
//
// `candidates` are the runtimes actually found installed, in any order.
// `system_default_present` is whether the system has an active OpenXR runtime
// configured at all.
OpenXRRuntimeSelection SelectOpenXRRuntime(const std::vector<OpenXRRuntimeCandidate>& candidates,
                                           std::string_view preference,
                                           bool system_default_present);

// Name for logs and diagnostics.
std::string_view OpenXRRuntimeKindName(OpenXRRuntimeKind kind);

} // namespace mkw::vr
