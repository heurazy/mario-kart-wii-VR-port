// SPDX-License-Identifier: GPL-3.0-or-later

// ANativeActivity entry point for the standalone Meta Quest product.
//
// The desktop products are executables whose main() runs the game on the
// process's initial thread. An Android package has no such thread to take over:
// the Android runtime owns the main thread, delivers lifecycle callbacks on it,
// and will ANR the process if a callback blocks. So this file does three things
// and nothing else:
//
//   * captures the JavaVM, Activity and storage paths the rest of the runtime
//     needs (RuntimePlatform::Android::SetAppContext), before anything else
//     runs, because the OpenXR loader cannot be touched until it has them;
//   * runs RuntimeMain on its own thread;
//   * forwards the Android lifecycle to that thread without blocking the
//     main one.
//
// This translation unit is compiled only into the Android product target.

#if defined(__ANDROID__)

#include "platform/android_app.h"

#include <android/api-level.h>
#include <android/log.h>
#include <android/native_activity.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

// Defined in runtime/src/main.cpp. Declared here rather than in a header
// because the desktop products reach it through main() and nothing else in the
// tree calls it.
int RuntimeMain(int argc, char** argv);

namespace {

// The Android runtime may create and destroy the activity more than once within
// one process (a configuration change, or the user removing and re-donning the
// headset past the timeout). The guest runtime is not re-entrant, so the game
// thread is started exactly once per process and the later activities attach to
// the running instance.
std::atomic<bool> g_game_thread_started{false};
ANativeActivity* g_activity = nullptr;

constexpr const char* kLogTag = "MarioKartWiiVR";

// The runtime reports through stdout/stderr, which on Android go to /dev/null.
// Without this there is no way at all to see a startup failure on a headset:
// there is no console, and a crash before the log file path is resolved leaves
// nothing behind. A pipe plus a reader thread is the standard way to recover
// them, and it costs one thread for the process lifetime.
void RedirectStandardStreamsToLogcat() {
    static std::atomic<bool> installed{false};
    bool expected = false;
    if (!installed.compare_exchange_strong(expected, true)) {
        return;
    }

    int pipe_fds[2]{};
    if (pipe(pipe_fds) != 0) {
        return;
    }

    // Unbuffered, so a message is visible at the moment it is written rather
    // than when a crash would have discarded the buffer.
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    dup2(pipe_fds[1], STDOUT_FILENO);
    dup2(pipe_fds[1], STDERR_FILENO);
    close(pipe_fds[1]);

    const int read_fd = pipe_fds[0];
    std::thread([read_fd] {
        std::string line;
        char buffer[512];
        for (;;) {
            const ssize_t count = read(read_fd, buffer, sizeof(buffer));
            if (count <= 0) {
                break;
            }
            for (ssize_t index = 0; index < count; ++index) {
                if (buffer[index] == '\n') {
                    __android_log_write(ANDROID_LOG_INFO, kLogTag, line.c_str());
                    line.clear();
                } else if (buffer[index] != '\r') {
                    line.push_back(buffer[index]);
                    // A runaway line must not grow without bound.
                    if (line.size() >= 4000) {
                        __android_log_write(ANDROID_LOG_INFO, kLogTag,
                                            line.c_str());
                        line.clear();
                    }
                }
            }
        }
        close(read_fd);
    }).detach();
}

std::string SystemProperty(const char* name) {
    char value[PROP_VALUE_MAX]{};
    const int length = __system_property_get(name, value);
    return length > 0 ? std::string(value, static_cast<size_t>(length))
                      : std::string();
}

void PublishAppContext(ANativeActivity* activity) {
    RuntimePlatform::Android::AppContext context;
    context.vm = activity->vm;
    // ANativeActivity::clazz is the Activity instance. The framework owns this
    // reference for the activity's lifetime, which is what
    // XR_KHR_loader_init_android and XR_KHR_android_create_instance require.
    context.activity = activity->clazz;
    context.asset_manager = activity->assetManager;

    if (activity->internalDataPath != nullptr) {
        context.internal_data_path = activity->internalDataPath;
    }
    if (activity->externalDataPath != nullptr) {
        context.external_data_path = activity->externalDataPath;
    }

    // Read through the property service rather than JNI: this runs before the
    // game thread exists, and attaching the main thread to the JVM just to read
    // two constants would be the only JNI call in the whole runtime.
    context.device_model = SystemProperty("ro.product.model");
    context.sdk_version = android_get_device_api_level();

    RuntimePlatform::Android::SetAppContext(context);
}

void StartGameThread() {
    bool expected = false;
    if (!g_game_thread_started.compare_exchange_strong(expected, true)) {
        return;
    }

    std::thread([] {
        // RuntimeMain rejects any command line, so it is given exactly the
        // program name the desktop products would see.
        char program[] = "mkwquest";
        char* argv[] = {program, nullptr};
        RuntimeMain(1, argv);

        // The game returned. Ask Android to tear the activity down; the process
        // exits with it. Finishing must be requested on the main thread.
        if (g_activity != nullptr) {
            ANativeActivity_finish(g_activity);
        }
    }).detach();
}

// Lifecycle callbacks. They run on the Android main thread and must return
// promptly. OpenXR session state is driven by the runtime's own event loop on
// the game thread, which observes XR_SESSION_STATE_* transitions the compositor
// sends in response to these; nothing here needs to block on it.

void OnStart(ANativeActivity*) {}

void OnResume(ANativeActivity* activity) {
    // Starting here rather than in onCreate: the OpenXR runtime will not bring
    // a session to the focused state until the activity is resumed, and
    // starting earlier only means the game thread spins waiting for it.
    g_activity = activity;
    StartGameThread();
}

void OnPause(ANativeActivity*) {}

void OnStop(ANativeActivity*) {}

void OnDestroy(ANativeActivity* activity) {
    // Deliberately does not try to stop the game thread. Driving the guest
    // runtime to an orderly shutdown from a lifecycle callback is unsolved -
    // see QUEST.md, "Android lifecycle in the frame loop" - and a flag that
    // nothing reads would look like it were handled.
    if (g_activity == activity) {
        g_activity = nullptr;
    }
}

void OnWindowFocusChanged(ANativeActivity*, int) {}

// A VR application presents through the OpenXR compositor, never through the
// activity's own surface, so the window callbacks are accepted and ignored.
// They must still be installed: NativeActivity will not deliver input or
// lifecycle correctly if the callback table is partially populated.
void OnNativeWindowCreated(ANativeActivity*, ANativeWindow*) {}
void OnNativeWindowDestroyed(ANativeActivity*, ANativeWindow*) {}
void OnNativeWindowResized(ANativeActivity*, ANativeWindow*) {}
void OnNativeWindowRedrawNeeded(ANativeActivity*, ANativeWindow*) {}
void OnInputQueueCreated(ANativeActivity*, AInputQueue*) {}
void OnInputQueueDestroyed(ANativeActivity*, AInputQueue*) {}
void OnConfigurationChanged(ANativeActivity*) {}
void OnLowMemory(ANativeActivity*) {}

void* OnSaveInstanceState(ANativeActivity*, size_t* outSize) {
    *outSize = 0;
    return nullptr;
}

} // namespace

extern "C" __attribute__((visibility("default"))) void ANativeActivity_onCreate(
    ANativeActivity* activity, void* savedState, size_t savedStateSize) {
    (void)savedState;
    (void)savedStateSize;
    if (activity == nullptr) {
        return;
    }

    g_activity = activity;
    RedirectStandardStreamsToLogcat();
    PublishAppContext(activity);
    __android_log_print(ANDROID_LOG_INFO, kLogTag,
                        "Mario Kart Wii VR (standalone): %s, API %d",
                        RuntimePlatform::Android::GetAppContext()
                            .device_model.c_str(),
                        RuntimePlatform::Android::GetAppContext().sdk_version);

    activity->callbacks->onStart = OnStart;
    activity->callbacks->onResume = OnResume;
    activity->callbacks->onPause = OnPause;
    activity->callbacks->onStop = OnStop;
    activity->callbacks->onDestroy = OnDestroy;
    activity->callbacks->onWindowFocusChanged = OnWindowFocusChanged;
    activity->callbacks->onNativeWindowCreated = OnNativeWindowCreated;
    activity->callbacks->onNativeWindowDestroyed = OnNativeWindowDestroyed;
    activity->callbacks->onNativeWindowResized = OnNativeWindowResized;
    activity->callbacks->onNativeWindowRedrawNeeded = OnNativeWindowRedrawNeeded;
    activity->callbacks->onInputQueueCreated = OnInputQueueCreated;
    activity->callbacks->onInputQueueDestroyed = OnInputQueueDestroyed;
    activity->callbacks->onConfigurationChanged = OnConfigurationChanged;
    activity->callbacks->onLowMemory = OnLowMemory;
    activity->callbacks->onSaveInstanceState = OnSaveInstanceState;
}

#endif // defined(__ANDROID__)
