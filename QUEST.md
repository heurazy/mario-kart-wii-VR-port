# Standalone Meta Quest target

This document covers the **standalone** Android target: the game running on the
headset itself, with no PC. For playing the existing Windows build on a Quest
over Quest Link, Air Link or Virtual Desktop, see [README.md](README.md) and
[OPENXR.md](OPENXR.md) — that path works through the desktop build and is not
what this file describes.

> **Status: not yet runnable on a headset.** The platform, OpenXR and packaging
> layers described below are implemented and cross-compile for `arm64-v8a`, but
> three dependencies listed under [Remaining blockers](#remaining-blockers) must
> be resolved before an APK produced by `Launcher/Build-Quest.sh` renders
> anything. Nothing here has been run on a device. Do not read the sections
> below as a claim that the game boots on a Quest — they describe the parts that
> exist and how they were checked.

## Why this is a port and not a build flag

The game is statically recompiled PowerPC, not emulated, so "run it on Android"
decomposes into three independent problems:

1. **The translated game code.** This is the easy part, and it is already done.
   The translator emits architecture-neutral C++ — there is not one x86 or
   AArch64 assumption anywhere in `translator/` — and every host-ISA detail
   lives behind `__x86_64__` / `__aarch64__` guards in `runtime/include/isa/`
   and `runtime/include/guest_flat_memory.h`, which already carry AArch64 paths
   for the macOS and Linux arm64 targets. One translated project directory
   therefore serves both the desktop products and the Quest one; that is why
   `Build-Quest.sh` takes a `--project` produced by `local-build.sh` instead of
   translating again.

2. **The runtime's host services.** Guest memory, fibers, and paths. Android is
   a Linux kernel with bionic, so the flat 4 GiB guest mapping (`memfd_create`
   plus a fixed `mmap` base, API 30+) and the libco fiber backend port across
   unchanged. What does not port is the assumption that a process has an
   executable directory and a writable working directory; Android has neither.

3. **Graphics.** This is the hard part, and it is where the remaining blockers
   are. Aurora renders through Dawn, and OpenXR requires eye images to be
   submitted on the *same* graphics device the compositor was told about. On
   Windows that is solved by `aurora_d3d12_get_native_handles` plus a D3D12
   stereo bridge. Android has no D3D12, no prebuilt Dawn, and no equivalent
   Vulkan bridge yet.

## What is implemented

| Area | Where | State |
| --- | --- | --- |
| Android platform detection, arm64 baseline, shared-library product | `runtime/CMakeLists.txt`, `runtime/cmake/PublicProducts.cmake` | Implemented |
| Application context: JavaVM, Activity, storage paths, device model | `runtime/include/platform/android_app.h`, `runtime/src/platform/android/android_app.cpp` | Implemented |
| `ANativeActivity` entry point and lifecycle | `runtime/src/platform/android/quest_entry_point.cpp` | Implemented |
| Per-user data directory on Android | `runtime/src/platform/host_platform.cpp` | Implemented |
| OpenXR loader init (`XR_KHR_loader_init_android`) | `runtime/src/vr/openxr_android.cpp` | Implemented |
| Instance chaining (`XR_KHR_android_create_instance`) | `runtime/src/vr/openxr_runtime.cpp` | Implemented |
| Quest extension selection (foveation, swapchain state, perf settings, colour space) | `runtime/src/vr/openxr_android.cpp` | Implemented |
| Fixed-foveated rendering and CPU/GPU performance levels | `runtime/src/vr/openxr_android.cpp` | Implemented, not exercised on device |
| Per-headset defaults and refresh-rate selection | `runtime/include/vr/quest_device_profile.h` | Implemented, unit-tested |
| APK packaging | `android/` | Implemented |
| Build orchestration | `Launcher/Build-Quest.sh` | Implemented |
| Dawn Vulkan native-handle export | `aurora-main/cmake/patches/dawn-vulkan-native-handles.patch` | Implemented |
| Vulkan ↔ OpenXR stereo bridge (AHardwareBuffer handoff, native copy, fencing) | `aurora-main/lib/webgpu/vulkan_interop.cpp` | Implemented, never executed |
| Presentation loop driving that bridge | `runtime/src/vr/openxr_integration.cpp` | **Not written — D3D12 only** |

### How the above was checked

Everything marked *Implemented* compiles for `aarch64-linux-android`, API 32,
with NDK r27c (Clang 18), at `-march=armv8.2-a+fp16+dotprod`. `openxr_runtime.cpp`
and `openxr_android.cpp` were compiled against the pinned OpenXR SDK
(`release-1.1.61`) for both the Android and desktop targets, so the desktop
build is unaffected by the Android branches. A CMake configure with the NDK
toolchain file reaches Aurora's dependency resolution, which is where the
blockers below begin.

`vulkan_interop.cpp` compiles for the same target against the real headers it
will use at build time: the NDK's Vulkan and `AHardwareBuffer` headers, Dawn's
generated WebGPU headers (so the `SharedTextureMemory` descriptors and the
`ChainedStructOut` layout-handoff types are the compiler's, not hand-written),
and Dawn's `VulkanBackend.h` with the patch applied. The resulting object's
undefined symbols were checked to confirm it references `AHardwareBuffer_allocate`
and all four patched Dawn exports, rather than silently compiling the
not-available stub. The patch was verified to apply cleanly to an unmodified
Dawn checkout.

None of it has been executed. Compiling is not running: the bridge's image
layouts, memory-type selection and fence retirement are the kind of thing only
a device validates, and the Vulkan validation layers have never seen this code.

`mkw_quest_device_profile_tests` is a new CTest target and passes on the host,
alongside the existing VR tests (`mkw_vr_first_person_tests`,
`mkw_quest_input_tests`, `mkw_steering_wheel_tests`,
`mkw_openxr_controller_profiles_tests`, `mkw_vr_submission_tests`,
`mkw_vr_delivery_tests`, `mkw_vr_settings_policy_tests`).

**No part of this has run on a Quest.** There is no on-device evidence for any
row above.

## Remaining blockers

These are ordered by how much work they are, cheapest first.

### 1. Dawn for Android

`AuroraDawnProvider.cmake` resolves Dawn from prebuilt packages published for
Windows, Linux and macOS only; there is no Android artifact, so the Quest target
falls through to `AURORA_DAWN_PROVIDER=vendor` and builds Dawn from source.
Dawn supports Vulkan on Android upstream, and the provider now disables its X11,
Wayland and GLFW surface support for this target, but the source build has not
been run. Expect this to be the first thing that fails and the longest single
step in the build.

### 2. Aurora's SDL3 dependency

Aurora uses SDL3 for windowing, input and audio. SDL3's Android video driver
expects SDL's own `SDLActivity` Java layer, and this package is a pure
`NativeActivity` with `android:hasCode="false"`. A standalone VR application has
no window — it presents through the OpenXR compositor — so the intended fix is
to run SDL with the `offscreen` or `dummy` video driver and keep only its audio
and timing subsystems, or to give Aurora a headless initialisation path. Neither
is implemented. Audio in particular has not been looked at.

### 3. The presentation loop for Vulkan

The device bridge itself is now implemented; what is missing is the code that
drives it frame to frame.

What exists: `aurora-main/lib/webgpu/vulkan_interop.cpp` is a full Vulkan
sibling of `d3d12_interop.cpp`. It obtains Dawn's `VkInstance`,
`VkPhysicalDevice`, `VkDevice`, `VkQueue` and queue family (through the Dawn
patch described below), allocates an `AHardwareBuffer` per eye, imports it into
Dawn as a `SharedTextureMemory` and into Vulkan as an aliasing `VkImage`, takes
the Dawn `BeginAccess`/`EndAccess` layout handoff, and submits a native
`vkCmdCopyImage` into the OpenXR swapchain image on Dawn's own queue, retiring
each submission through a `VkFence`.

Dawn's stock `VulkanBackend.h` publishes only `VkInstance`, so
`cmake/patches/dawn-vulkan-native-handles.patch` exposes the rest. Every handle
it needs is already reachable internally and `GetVkDevice` is in fact already
defined upstream but never declared; the patch is additive and mirrors the
D3D12 backend, which already exports its device and command queue. It is
applied to the vendored Dawn tree automatically, and idempotently, at configure
time.

What is missing: `runtime/src/vr/openxr_integration.cpp` — the ~1000-line loop
that acquires swapchain images, paces frames against the compositor, and drives
the sink — is written directly against `OpenXRD3D12Backend` and its frame
types. A Vulkan backend exposing the same interface shape (the pimpl'd
`BeginFrame`/`WaitForSubmission`/`FinishFrame` contract in
`runtime/include/vr/openxr_d3d12.h`) has to exist before that loop can be made
platform-neutral. `runtime/src/vr/openxr_vulkan_backend.cpp` owns the session
and swapchains but does not present that interface.

Until then nothing calls `aurora_vulkan_enable_stereo_bridge`, so the bridge is
compiled and linked but never runs.

### 4. Android lifecycle in the frame loop

`quest_entry_point.cpp` installs the lifecycle callbacks and runs the game on
its own thread, but pause/resume is currently handled only by whatever
`XR_SESSION_STATE_*` transitions the compositor sends. Taking the headset off
mid-race has not been reasoned through end to end.

### 5. Getting game data onto the headset

The desktop installer compiles the game beside the executable. On a Quest the
compiled game and its assets have to be pushed to the app's external files
directory (`/sdcard/Android/data/org.wiicompiled.mkwvr/files`). There is no
tooling for this yet.

### 6. Whether it is fast enough

Unknown, and it is the question that decides whether this target is worth
finishing. A statically recompiled Wii title runs its guest code on one thread
with byte swapping on every memory access, and the GX-to-WebGPU translation was
written for desktop GPUs. The device profiles in
`runtime/include/vr/quest_device_profile.h` default to 72 Hz with reduced
resolution and non-zero foveation precisely because that headroom is not
expected to be there. No measurement exists.

## Building

You need: a translated project directory, an Android NDK (r26+), the OpenXR SDK
source for headers, an `arm64-v8a` `libopenxr_loader.so`, and — to package — the
Android SDK, JDK 17 and Gradle.

```bash
# 1. Translate your own clean PAL RMCP01 image (architecture-neutral output,
#    reusable by every target). This is the multi-hour step.
Launcher/local-build.sh --iso /path/to/RMCP01.iso --project build/mkwii

# 2. Extract the Quest OpenXR loader from the Khronos AAR.
#    org.khronos.openxr:openxr_loader_for_android is a zip; the loader is at
#    prefab/modules/openxr_loader/libs/android.arm64-v8a/libopenxr_loader.so

# 3. Cross-compile and package.
Launcher/Build-Quest.sh \
  --project build/mkwii \
  --ndk "$ANDROID_NDK_HOME" \
  --sdk "$ANDROID_HOME" \
  --openxr-sdk /path/to/OpenXR-SDK \
  --openxr-loader /path/to/libopenxr_loader.so

adb install -r android/app/build/outputs/apk/release/app-release.apk
```

`--no-package` stops after `libmkwquest.so`, which is the useful mode while the
blockers above are being worked through.

## Device support

Quest 2, Quest Pro, Quest 3 and Quest 3S. All four run Android 12L (API 32) and
an ARMv8.2-A SoC, which is the compile baseline, so one binary covers them.

Quest 1 is deliberately excluded from `com.oculus.supportedDevices`: its
Snapdragon 835 is not a realistic target for this workload. It is still
recognised by `QuestModelFromSystemName` so a tethered session can name it.

Defaults per device live in `runtime/src/vr/quest_device_profile.cpp`. They are
engineering estimates chosen to favour holding a frame rate over sharpness, not
measurements, and they should be replaced with measured values once the target
runs.

## Controls

Unchanged from the desktop build — the OpenXR action set and the Touch
interaction profiles in `runtime/include/vr/openxr_controller_profiles.h` are
shared. One difference is worth noting: the long-press-X pause gesture exists
only because SteamVR takes the system button for its dashboard. On a standalone
Quest the left menu button is delivered to the application, so it pauses
directly, which is the binding the non-SteamVR path already uses.
