// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Android/Meta Quest application context.
//
// A standalone Quest build has no executable directory, no working directory
// worth resolving against, and no OpenXR loader that can be used before the
// process hands the loader its JavaVM and Activity. All of that arrives from
// the Android entry point, which publishes it here exactly once, before any
// runtime subsystem starts.
//
// JNI and NDK types are deliberately kept out of this header: it is included by
// the platform layer, the VR bring-up and the tests, and only the Android entry
// point and the Android implementation files may depend on <jni.h>. The handles
// are therefore carried as void* and converted at the single call site that
// owns them, the same way vr/openxr_vulkan_backend.h carries Vulkan handles.

#include <cstdint>
#include <filesystem>
#include <string>

namespace RuntimePlatform::Android {

// Everything the runtime needs from the Android application object. The two
// JNI handles must stay valid for the whole process lifetime; the entry point
// owns a global reference to the activity for exactly that reason.
struct AppContext {
    // JavaVM*. Required by XR_KHR_loader_init_android and by any JNI call the
    // runtime makes from its own threads.
    void* vm = nullptr;

    // jobject global reference to the Activity. Required by
    // XR_KHR_loader_init_android and XR_KHR_android_create_instance.
    void* activity = nullptr;

    // AAssetManager*. The read-only APK asset tree, used for bundled runtime
    // assets. Game data does not live here: it is installed to
    // internal_data_path or external_data_path by the on-device setup step.
    void* asset_manager = nullptr;

    // Private app storage (Context.getFilesDir()). Configuration, NAND, the
    // shader cache and logs live here. Always writable, never user-visible.
    std::filesystem::path internal_data_path;

    // App-scoped external storage (Context.getExternalFilesDir(null)). This is
    // the path reachable over MTP/adb, so it is where the compiled game and a
    // user-supplied disc image are expected. May be empty when the volume is
    // unavailable, in which case internal storage is used for everything.
    std::filesystem::path external_data_path;

    // The device model reported by android.os.Build.MODEL, used only for
    // diagnostics. Headset identification for behaviour decisions comes from
    // the OpenXR system name (see vr/quest_device_profile.h), which is the
    // runtime's own answer and does not depend on JNI being reachable.
    std::string device_model;

    // android.os.Build.VERSION.SDK_INT.
    int32_t sdk_version = 0;
};

// Publishes the context. The Android entry point calls this once, before the
// runtime starts. A second call replaces the stored value and is only expected
// in tests.
void SetAppContext(const AppContext& context);

// True once SetAppContext has run. Every Android-only path checks this instead
// of assuming the entry point ran, so a unit test or a desktop build linking
// this file sees a clean "no Android context" answer rather than a crash.
bool HasAppContext();

// The published context, or a default-constructed one when HasAppContext() is
// false. Never returns a dangling reference.
const AppContext& GetAppContext();

// Preferred writable root for game data: external_data_path when it is set,
// otherwise internal_data_path. Empty when no context has been published.
std::filesystem::path PreferredDataRoot();

} // namespace RuntimePlatform::Android
