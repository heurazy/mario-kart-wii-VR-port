// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#if defined(MKW_ENABLE_OPENXR)

#include "vr/openxr_config.h"
#include "vr/quest_device_profile.h"

#include <string>
#include <vector>

namespace mkw::vr {

// Android/Meta Quest specifics of the OpenXR bring-up.
//
// Three things separate a standalone Quest session from the desktop one this
// port already implements:
//
//   1. The loader cannot be used at all until it has been handed the process's
//      JavaVM and Activity through XR_KHR_loader_init_android. On Android this
//      must happen before the first xrEnumerateInstanceExtensionProperties,
//      not merely before xrCreateInstance.
//   2. xrCreateInstance needs the same pair again, chained through
//      XR_KHR_android_create_instance.
//   3. The application owns the frame budget. On a PC the user picks a refresh
//      rate in their runtime and the GPU is oversized; on a Quest the process
//      chooses the refresh rate, the foveation strength and the CPU/GPU
//      performance levels, and holding the target rate depends on all three.
//
// Everything here is a no-op returning false on a non-Android build, so the
// desktop path compiles and links unchanged and callers need no #if of their
// own around the call sites.

// True for a build targeting Android (that is, the standalone Quest target).
// A tethered Quest session runs the desktop build and reports false.
bool OpenXRAndroidIsStandaloneBuild();

// The transport this build represents, for vr/quest_device_profile.h.
QuestTransport OpenXRAndroidTransport();

// Hands the loader the JavaVM and Activity published through
// RuntimePlatform::Android::SetAppContext. Must be called before any other
// OpenXR entry point. Idempotent: a second call returns the first result.
//
// Returns false and fills `error` when the Android context was never published,
// when the loader does not expose xrInitializeLoaderKHR, or when the loader
// rejects the initialization. On a non-Android build it returns true and does
// nothing, because there is no loader state to establish.
bool OpenXRAndroidInitializeLoader(std::string* error);

// The XrInstanceCreateInfoAndroidKHR to chain into XrInstanceCreateInfo::next,
// or nullptr when this is not an Android build or no context was published.
// The pointee has static storage duration and stays valid for the process.
const void* OpenXRAndroidInstanceCreateInfoChain();

// Extensions the Android target must have. Currently
// XR_KHR_android_create_instance; empty elsewhere. A missing entry is a hard
// failure, because the instance cannot be created without it.
std::vector<std::string> OpenXRAndroidRequiredExtensions();

// Extensions the Quest target uses when the runtime advertises them: the
// foveation pair, swapchain state updates, performance settings and the colour
// space control. All are optional by construction - a runtime without them
// renders correctly, just without that tuning.
std::vector<std::string> OpenXRAndroidOptionalExtensions();

// Applies the profile's CPU and GPU performance levels through
// XR_EXT_performance_settings. `instance` and `session` are XrInstance and
// XrSession; they are taken as void* so this header stays usable from code that
// has not included the platform headers. Returns false when the extension is
// absent or the runtime rejects a level, which is not fatal - the runtime
// simply keeps its own governor behaviour.
bool OpenXRAndroidApplyPerformanceLevels(void* instance, void* session,
                                         const QuestDeviceProfile& profile,
                                         std::string* error);

// Applies fixed-foveated rendering to one swapchain through XR_FB_foveation and
// XR_FB_swapchain_update_state. Call once per swapchain after creation; the
// swapchain must have been created with XR_SWAPCHAIN_CREATE_FOVEATION_FB in its
// createFlags for the runtime to accept the update. Returns false when the
// extensions are absent or the update is rejected; rendering is unaffected.
bool OpenXRAndroidApplyFoveation(void* instance, void* session, void* swapchain,
                                 QuestFoveation level, std::string* error);

} // namespace mkw::vr

#endif // defined(MKW_ENABLE_OPENXR)
