// SPDX-License-Identifier: GPL-3.0-or-later

#if defined(MKW_ENABLE_OPENXR)

#if defined(__ANDROID__)
// Must precede openxr_platform.h: it is what declares the Android structures.
#define XR_USE_PLATFORM_ANDROID 1
#endif

#include "vr/openxr_android.h"

#if defined(__ANDROID__)
#include "platform/android_app.h"
// openxr_platform.h declares Android entry points in terms of jobject, so JNI
// must be visible first even though this file never calls into the JVM itself.
#include <jni.h>
#include <openxr/openxr_platform.h>
#endif

namespace mkw::vr {
namespace {

void SetError(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
}

#if defined(__ANDROID__)

std::string ResultText(XrResult result) {
    // No instance is available for xrResultToString at loader-init time, so the
    // numeric code is reported. It is the value the OpenXR headers document and
    // is enough to identify the failure in a logcat report.
    return std::to_string(static_cast<int32_t>(result));
}

XrFoveationLevelFB ToFoveationLevel(QuestFoveation level) {
    switch (level) {
    case QuestFoveation::None:
        return XR_FOVEATION_LEVEL_NONE_FB;
    case QuestFoveation::Low:
        return XR_FOVEATION_LEVEL_LOW_FB;
    case QuestFoveation::Medium:
        return XR_FOVEATION_LEVEL_MEDIUM_FB;
    case QuestFoveation::High:
    case QuestFoveation::HighTop:
        return XR_FOVEATION_LEVEL_HIGH_FB;
    }
    return XR_FOVEATION_LEVEL_NONE_FB;
}

// XR_FB_foveation expresses "high, biased upward" as a high level plus a
// vertical offset, not as a distinct level.
float FoveationVerticalOffset(QuestFoveation level) {
    return level == QuestFoveation::HighTop ? 45.0f : 0.0f;
}

XrPerfSettingsLevelEXT ToPerfLevel(uint32_t level) {
    switch (level) {
    case 0:
        return XR_PERF_SETTINGS_LEVEL_POWER_SAVINGS_EXT;
    case 1:
        return XR_PERF_SETTINGS_LEVEL_SUSTAINED_LOW_EXT;
    case 2:
        return XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT;
    default:
        return XR_PERF_SETTINGS_LEVEL_BOOST_EXT;
    }
}

// The loader may only be initialized once per process, and a failed attempt
// must not be retried with a different answer: the OpenXR loader keeps internal
// state either way.
struct LoaderState {
    bool attempted = false;
    bool succeeded = false;
    std::string error;
};

LoaderState& Loader() {
    static LoaderState state;
    return state;
}

#endif // defined(__ANDROID__)

} // namespace

bool OpenXRAndroidIsStandaloneBuild() {
#if defined(__ANDROID__)
    return true;
#else
    return false;
#endif
}

QuestTransport OpenXRAndroidTransport() {
    return OpenXRAndroidIsStandaloneBuild() ? QuestTransport::Standalone
                                            : QuestTransport::Tethered;
}

bool OpenXRAndroidInitializeLoader(std::string* error) {
#if !defined(__ANDROID__)
    // Desktop loaders need no initialization step; reporting success keeps the
    // shared bring-up path free of platform branches.
    (void)error;
    return true;
#else
    LoaderState& state = Loader();
    if (state.attempted) {
        if (!state.succeeded) {
            SetError(error, state.error);
        }
        return state.succeeded;
    }
    state.attempted = true;

    if (!RuntimePlatform::Android::HasAppContext()) {
        state.error =
            "the Android application context was not published before OpenXR "
            "startup";
        SetError(error, state.error);
        return false;
    }

    const auto& context = RuntimePlatform::Android::GetAppContext();
    if (context.vm == nullptr || context.activity == nullptr) {
        state.error =
            "the Android application context is missing its JavaVM or Activity";
        SetError(error, state.error);
        return false;
    }

    PFN_xrInitializeLoaderKHR initialize = nullptr;
    const XrResult lookup = xrGetInstanceProcAddr(
        XR_NULL_HANDLE, "xrInitializeLoaderKHR",
        reinterpret_cast<PFN_xrVoidFunction*>(&initialize));
    if (XR_FAILED(lookup) || initialize == nullptr) {
        state.error =
            "the OpenXR loader does not expose xrInitializeLoaderKHR; an "
            "Android loader supporting XR_KHR_loader_init_android is required "
            "(result " +
            ResultText(lookup) + ")";
        SetError(error, state.error);
        return false;
    }

    XrLoaderInitInfoAndroidKHR info{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
    info.applicationVM = context.vm;
    info.applicationContext = context.activity;

    const XrResult result = initialize(
        reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&info));
    if (XR_FAILED(result)) {
        state.error = "xrInitializeLoaderKHR failed (result " +
                      ResultText(result) + ")";
        SetError(error, state.error);
        return false;
    }

    state.succeeded = true;
    return true;
#endif
}

const void* OpenXRAndroidInstanceCreateInfoChain() {
#if !defined(__ANDROID__)
    return nullptr;
#else
    if (!RuntimePlatform::Android::HasAppContext()) {
        return nullptr;
    }
    const auto& context = RuntimePlatform::Android::GetAppContext();
    if (context.vm == nullptr || context.activity == nullptr) {
        return nullptr;
    }

    // Static storage: XrInstanceCreateInfo::next is read during
    // xrCreateInstance, but keeping this alive for the process removes any
    // question of lifetime at the call site.
    static XrInstanceCreateInfoAndroidKHR info{
        XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    info.applicationVM = context.vm;
    info.applicationActivity = context.activity;
    return &info;
#endif
}

std::vector<std::string> OpenXRAndroidRequiredExtensions() {
#if !defined(__ANDROID__)
    return {};
#else
    return {XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME};
#endif
}

std::vector<std::string> OpenXRAndroidOptionalExtensions() {
#if !defined(__ANDROID__)
    return {};
#else
    return {
        // Fixed-foveated rendering. The configuration extension is what exposes
        // the level enum; the base extension alone only allows a null profile.
        XR_FB_FOVEATION_EXTENSION_NAME,
        XR_FB_FOVEATION_CONFIGURATION_EXTENSION_NAME,
        // Required to attach a foveation profile to an existing swapchain.
        XR_FB_SWAPCHAIN_UPDATE_STATE_EXTENSION_NAME,
        // CPU/GPU governor levels.
        XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME,
        // The Quest panel is not sRGB; without this the image is noticeably
        // washed out compared with the desktop build.
        XR_FB_COLOR_SPACE_EXTENSION_NAME,
    };
#endif
}

bool OpenXRAndroidApplyPerformanceLevels(void* instance, void* session,
                                         const QuestDeviceProfile& profile,
                                         std::string* error) {
#if !defined(__ANDROID__)
    (void)instance;
    (void)session;
    (void)profile;
    SetError(error, "performance levels are an Android-only control");
    return false;
#else
    if (instance == nullptr || session == nullptr) {
        SetError(error, "a live OpenXR instance and session are required");
        return false;
    }

    auto xr_instance = static_cast<XrInstance>(instance);
    auto xr_session = static_cast<XrSession>(session);

    PFN_xrPerfSettingsSetPerformanceLevelEXT set_level = nullptr;
    const XrResult lookup = xrGetInstanceProcAddr(
        xr_instance, "xrPerfSettingsSetPerformanceLevelEXT",
        reinterpret_cast<PFN_xrVoidFunction*>(&set_level));
    if (XR_FAILED(lookup) || set_level == nullptr) {
        SetError(error,
                 "XR_EXT_performance_settings is unavailable; the runtime keeps "
                 "its own governor behaviour");
        return false;
    }

    const XrResult cpu =
        set_level(xr_session, XR_PERF_SETTINGS_DOMAIN_CPU_EXT,
                  ToPerfLevel(profile.default_cpu_level));
    const XrResult gpu =
        set_level(xr_session, XR_PERF_SETTINGS_DOMAIN_GPU_EXT,
                  ToPerfLevel(profile.default_gpu_level));
    if (XR_FAILED(cpu) || XR_FAILED(gpu)) {
        SetError(error, "the runtime rejected a performance level (cpu " +
                            ResultText(cpu) + ", gpu " + ResultText(gpu) + ")");
        return false;
    }
    return true;
#endif
}

bool OpenXRAndroidApplyFoveation(void* instance, void* session, void* swapchain,
                                 QuestFoveation level, std::string* error) {
#if !defined(__ANDROID__)
    (void)instance;
    (void)session;
    (void)swapchain;
    (void)level;
    SetError(error, "foveated rendering is an Android-only control");
    return false;
#else
    if (instance == nullptr || session == nullptr || swapchain == nullptr) {
        SetError(error,
                 "a live OpenXR instance, session and swapchain are required");
        return false;
    }
    if (level == QuestFoveation::None) {
        // Nothing to apply, and creating a null profile would still cost a
        // swapchain update. Report success: the requested state is the state.
        return true;
    }

    auto xr_instance = static_cast<XrInstance>(instance);
    auto xr_session = static_cast<XrSession>(session);
    auto xr_swapchain = static_cast<XrSwapchain>(swapchain);

    PFN_xrCreateFoveationProfileFB create_profile = nullptr;
    PFN_xrDestroyFoveationProfileFB destroy_profile = nullptr;
    PFN_xrUpdateSwapchainFB update_swapchain = nullptr;
    const bool resolved =
        XR_SUCCEEDED(xrGetInstanceProcAddr(
            xr_instance, "xrCreateFoveationProfileFB",
            reinterpret_cast<PFN_xrVoidFunction*>(&create_profile))) &&
        XR_SUCCEEDED(xrGetInstanceProcAddr(
            xr_instance, "xrDestroyFoveationProfileFB",
            reinterpret_cast<PFN_xrVoidFunction*>(&destroy_profile))) &&
        XR_SUCCEEDED(xrGetInstanceProcAddr(
            xr_instance, "xrUpdateSwapchainFB",
            reinterpret_cast<PFN_xrVoidFunction*>(&update_swapchain)));
    if (!resolved || create_profile == nullptr || destroy_profile == nullptr ||
        update_swapchain == nullptr) {
        SetError(error,
                 "XR_FB_foveation is unavailable; rendering continues without "
                 "fixed-foveated rendering");
        return false;
    }

    XrFoveationLevelProfileCreateInfoFB level_info{
        XR_TYPE_FOVEATION_LEVEL_PROFILE_CREATE_INFO_FB};
    level_info.level = ToFoveationLevel(level);
    level_info.verticalOffset = FoveationVerticalOffset(level);
    level_info.dynamic = XR_FOVEATION_DYNAMIC_DISABLED_FB;

    XrFoveationProfileCreateInfoFB profile_info{
        XR_TYPE_FOVEATION_PROFILE_CREATE_INFO_FB};
    profile_info.next = &level_info;

    XrFoveationProfileFB profile = XR_NULL_HANDLE;
    const XrResult created =
        create_profile(xr_session, &profile_info, &profile);
    if (XR_FAILED(created) || profile == XR_NULL_HANDLE) {
        SetError(error, "xrCreateFoveationProfileFB failed (result " +
                            ResultText(created) + ")");
        return false;
    }

    XrSwapchainStateFoveationFB state{XR_TYPE_SWAPCHAIN_STATE_FOVEATION_FB};
    state.profile = profile;
    const XrResult updated = update_swapchain(
        xr_swapchain,
        reinterpret_cast<const XrSwapchainStateBaseHeaderFB*>(&state));

    // The runtime copies the profile into the swapchain state, so it is
    // destroyed immediately whether or not the update was accepted.
    destroy_profile(profile);

    if (XR_FAILED(updated)) {
        SetError(error, "xrUpdateSwapchainFB rejected the foveation profile "
                        "(result " +
                            ResultText(updated) + ")");
        return false;
    }
    return true;
#endif
}

} // namespace mkw::vr

#endif // defined(MKW_ENABLE_OPENXR)
