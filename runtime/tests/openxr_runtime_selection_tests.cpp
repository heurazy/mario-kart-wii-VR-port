// SPDX-License-Identifier: GPL-3.0-or-later

#include "vr/openxr_runtime_selection.h"

#include <iostream>
#include <vector>

namespace {

int failures = 0;

void Check(bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAILED: " << message << '\n';
    }
}

using namespace mkw::vr;

std::vector<OpenXRRuntimeCandidate> All() {
    return {
        {OpenXRRuntimeKind::SteamVR, "C:/steam/steamxr_win64.json"},
        {OpenXRRuntimeKind::Meta, "C:/oculus/oculus_openxr_64.json"},
        {OpenXRRuntimeKind::VirtualDesktop, "C:/vd/virtualdesktop-openxr-64.json"},
    };
}

void AutoRespectsTheUsersActiveRuntime() {
    // The whole point of the change: an OpenXR app uses whatever the user made
    // active. Nothing is overridden, whichever runtimes happen to be installed.
    const auto selection = SelectOpenXRRuntime(All(), "auto", true);
    Check(selection.manifest_path.empty(),
          "auto does not override the system's active runtime");
    Check(selection.downgrade_reason.empty(), "respecting the active runtime is not a downgrade");

    const auto empty = SelectOpenXRRuntime(All(), "", true);
    Check(empty.manifest_path.empty(), "an unset preference behaves as auto");
}

void ExplicitPreferencesAreHonoured() {
    const auto meta = SelectOpenXRRuntime(All(), "meta", true);
    Check(meta.kind == OpenXRRuntimeKind::Meta, "meta is selected when requested");
    Check(meta.manifest_path == "C:/oculus/oculus_openxr_64.json",
          "the selected runtime carries its manifest path");

    // An explicit preference must win even when the system has something else
    // active: that is the only reason to set it.
    const auto steam = SelectOpenXRRuntime(All(), "steamvr", true);
    Check(steam.kind == OpenXRRuntimeKind::SteamVR, "steamvr is selected when requested");

    const auto vd = SelectOpenXRRuntime(All(), "virtualdesktop", true);
    Check(vd.kind == OpenXRRuntimeKind::VirtualDesktop,
          "virtual desktop is selected when requested");

    // Spellings people actually use.
    Check(SelectOpenXRRuntime(All(), "oculus", true).kind == OpenXRRuntimeKind::Meta,
          "the legacy 'oculus' spelling still selects Meta");
    Check(SelectOpenXRRuntime(All(), "VDXR", true).kind == OpenXRRuntimeKind::VirtualDesktop,
          "runtime names are case-insensitive");
}

void SystemNeverOverrides() {
    const auto selection = SelectOpenXRRuntime(All(), "system", false);
    Check(selection.manifest_path.empty(),
          "system never overrides, even with no active runtime");
    Check(selection.kind == OpenXRRuntimeKind::Unknown, "system selects nothing");
}

void AMissingPreferenceDowngradesInsteadOfFailing() {
    // This is the bug the change exists to fix: asking for a runtime that is
    // not installed must not stop the game from starting.
    std::vector<OpenXRRuntimeCandidate> onlyMeta{
        {OpenXRRuntimeKind::Meta, "C:/oculus/oculus_openxr_64.json"}};
    const auto selection = SelectOpenXRRuntime(onlyMeta, "steamvr", true);
    Check(selection.manifest_path.empty(),
          "an uninstalled preference falls back to the active runtime");
    Check(!selection.downgrade_reason.empty(), "the downgrade is reported");
}

void NoActiveRuntimeProbesForOne() {
    // A machine with a headset but no configured active runtime should still
    // work rather than dropping to the desktop mirror.
    const auto selection = SelectOpenXRRuntime(All(), "auto", false);
    Check(selection.kind == OpenXRRuntimeKind::Meta,
          "with nothing active, Meta is probed first");
    Check(!selection.manifest_path.empty(), "the probed runtime is actually selected");
    Check(!selection.downgrade_reason.empty(), "probing is reported");

    std::vector<OpenXRRuntimeCandidate> noMeta{
        {OpenXRRuntimeKind::SteamVR, "C:/steam/steamxr_win64.json"},
        {OpenXRRuntimeKind::VirtualDesktop, "C:/vd/virtualdesktop-openxr-64.json"},
    };
    Check(SelectOpenXRRuntime(noMeta, "auto", false).kind == OpenXRRuntimeKind::VirtualDesktop,
          "Virtual Desktop is probed before SteamVR");

    std::vector<OpenXRRuntimeCandidate> onlySteam{
        {OpenXRRuntimeKind::SteamVR, "C:/steam/steamxr_win64.json"}};
    Check(SelectOpenXRRuntime(onlySteam, "auto", false).kind == OpenXRRuntimeKind::SteamVR,
          "SteamVR is still used when it is all there is");
}

void NothingInstalledIsNotFatal() {
    const auto selection = SelectOpenXRRuntime({}, "auto", false);
    Check(selection.manifest_path.empty(), "nothing is selected when nothing exists");
    Check(!selection.downgrade_reason.empty(),
          "the desktop-mirror fallback is explained rather than thrown");
}

void UnrecognisedValuesAreReportedNotObeyed() {
    const auto selection = SelectOpenXRRuntime(All(), "wmr", true);
    Check(selection.manifest_path.empty(), "an unrecognised value behaves as auto");
    Check(!selection.downgrade_reason.empty(), "an unrecognised value is reported");
}

} // namespace

int main() {
    AutoRespectsTheUsersActiveRuntime();
    ExplicitPreferencesAreHonoured();
    SystemNeverOverrides();
    AMissingPreferenceDowngradesInsteadOfFailing();
    NoActiveRuntimeProbesForOne();
    NothingInstalledIsNotFatal();
    UnrecognisedValuesAreReportedNotObeyed();
    if (failures == 0) {
        std::cout << "openxr runtime selection tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
