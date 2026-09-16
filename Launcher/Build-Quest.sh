#!/usr/bin/env bash
# Standalone Meta Quest build: configure -> compile -> stage -> package APK.
#
# This is the Android/arm64 counterpart to Launcher/local-build.sh. It does NOT
# translate the game: the translator's C++ output is architecture-neutral (it
# contains no x86 or AArch64 assumptions - those live entirely in
# runtime/include/isa/*.h), so one translated project directory serves both the
# desktop products and this one. Run local-build.sh or LocalBuild.ps1 first to
# produce that directory from your own disc image, then point this script at it
# with --project.
#
# See QUEST.md for what is and is not working on-device.
set -euo pipefail

log_step() {
    printf 'MKWCBUILD:STEP:%s %s\n' "$1" "$2"
}

fail() {
    echo "Build-Quest.sh: error: $*" >&2
    exit 1
}

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
workspace=$(cd "$script_dir/.." && pwd)

# ---------------------------------------------------------------------------
# Arguments
# ---------------------------------------------------------------------------
project=""
ndk_dir="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}"
sdk_dir="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}"
openxr_loader=""
openxr_sdk=""
api_level=32
build_dir=""
package=1
gradle_bin="${GRADLE:-gradle}"
parallel=""

usage() {
    cat <<'USAGE'
Usage: Build-Quest.sh --project DIR [options]

Required:
  --project DIR           Translated project directory (from local-build.sh)
  --ndk DIR               Android NDK r26+ (or set ANDROID_NDK_HOME)
  --openxr-sdk DIR        OpenXR-SDK source tree, for the headers
  --openxr-loader FILE    libopenxr_loader.so for arm64-v8a, extracted from
                          the org.khronos.openxr:openxr_loader_for_android AAR

Options:
  --sdk DIR               Android SDK (or set ANDROID_HOME); needed only to package
  --api-level N           Android API level to build against (default: 32)
  --build-dir DIR         Native build directory (default: <workspace>/build-quest)
  --parallel N            Ninja parallelism and translated-shard job cap
  --no-package            Build libmkwquest.so only; skip the Gradle APK step
  -h, --help              Show this message
USAGE
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --project) project="$2"; shift 2 ;;
        --ndk) ndk_dir="$2"; shift 2 ;;
        --sdk) sdk_dir="$2"; shift 2 ;;
        --openxr-sdk) openxr_sdk="$2"; shift 2 ;;
        --openxr-loader) openxr_loader="$2"; shift 2 ;;
        --api-level) api_level="$2"; shift 2 ;;
        --build-dir) build_dir="$2"; shift 2 ;;
        --parallel) parallel="$2"; shift 2 ;;
        --no-package) package=0; shift ;;
        -h|--help) usage; exit 0 ;;
        *) fail "unknown option: $1 (try --help)" ;;
    esac
done

[[ -n "$project" ]] || fail "--project is required (try --help)"
[[ -d "$project" ]] || fail "--project directory does not exist: $project"
[[ -n "$ndk_dir" ]] || fail "--ndk is required, or set ANDROID_NDK_HOME"
[[ -d "$ndk_dir" ]] || fail "the Android NDK directory does not exist: $ndk_dir"
[[ -n "$openxr_sdk" ]] || fail "--openxr-sdk is required (the OpenXR-SDK source tree)"
[[ -f "$openxr_sdk/include/openxr/openxr.h" ]] || \
    fail "--openxr-sdk does not look like an OpenXR-SDK tree: $openxr_sdk"
[[ -n "$openxr_loader" ]] || fail "--openxr-loader is required"
[[ -f "$openxr_loader" ]] || fail "the OpenXR loader does not exist: $openxr_loader"

toolchain="$ndk_dir/build/cmake/android.toolchain.cmake"
[[ -f "$toolchain" ]] || fail "the NDK has no CMake toolchain file: $toolchain"

build_dir="${build_dir:-$workspace/build-quest}"

if [[ -z "$parallel" ]]; then
    parallel=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
fi

# ---------------------------------------------------------------------------
# Configure
# ---------------------------------------------------------------------------
# AURORA_DAWN_PROVIDER: the pinned Dawn prebuilts at encounter/dawn-build have
# no Android arm64 artifact, so the Quest target has to build Dawn from source.
# This is the single most expensive part of the build and the most likely thing
# to fail first; QUEST.md documents its current state.
log_step configure-quest "Configuring the standalone Quest build for arm64-v8a"
cmake -S "$workspace/runtime" -B "$build_dir" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$toolchain" \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM="android-$api_level" \
    -DANDROID_STL=c++_shared \
    -DCMAKE_BUILD_TYPE=Release \
    -DMKW_ENABLE_OPENXR=ON \
    -DMKW_OPENXR_SDK_DIR="$openxr_sdk" \
    -DMKW_OPENXR_ANDROID_LOADER="$openxr_loader" \
    -DMKW_TRANSLATED_COMPILE_JOBS="$parallel" \
    -DAURORA_DAWN_PROVIDER=vendor \
    -DAURORA_SDL3_PROVIDER=vendor

# ---------------------------------------------------------------------------
# Compile
# ---------------------------------------------------------------------------
log_step build-quest "Compiling the translated game for arm64-v8a"
cmake --build "$build_dir" --target WiiCompiled --parallel "$parallel"

native_lib="$build_dir/libmkwquest.so"
[[ -f "$native_lib" ]] || native_lib=$(find "$build_dir" -name 'libmkwquest.so' -print -quit)
[[ -n "$native_lib" && -f "$native_lib" ]] || fail "the build did not produce libmkwquest.so"

# ---------------------------------------------------------------------------
# Stage
# ---------------------------------------------------------------------------
jni_dir="$workspace/android/app/src/main/jniLibs/arm64-v8a"
mkdir -p "$jni_dir"
log_step stage-quest "Staging native libraries into the Android package"
cp -f "$native_lib" "$jni_dir/libmkwquest.so"
cp -f "$openxr_loader" "$jni_dir/libopenxr_loader.so"

# c++_shared: the NDK's libc++ is not part of the platform, so it ships with the
# package. This must be the exact copy the build linked against.
stl_lib="$ndk_dir/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so"
if [[ -f "$stl_lib" ]]; then
    cp -f "$stl_lib" "$jni_dir/libc++_shared.so"
else
    echo "Build-Quest.sh: warning: libc++_shared.so was not found at $stl_lib;" >&2
    echo "  the package will fail to load unless it is staged manually." >&2
fi

echo "MKWCBUILD: staged $(du -h "$jni_dir/libmkwquest.so" | cut -f1) of native code into $jni_dir"

# ---------------------------------------------------------------------------
# Package
# ---------------------------------------------------------------------------
if [[ "$package" -eq 0 ]]; then
    log_step done-quest "Native build complete; APK packaging was skipped"
    exit 0
fi

[[ -n "$sdk_dir" ]] || fail "--sdk is required to package (or pass --no-package)"
[[ -d "$sdk_dir" ]] || fail "the Android SDK directory does not exist: $sdk_dir"
command -v "$gradle_bin" >/dev/null 2>&1 || \
    fail "gradle was not found on PATH (set GRADLE, or pass --no-package)"

log_step package-quest "Building the Quest APK"
ANDROID_HOME="$sdk_dir" "$gradle_bin" -p "$workspace/android" assembleRelease

apk=$(find "$workspace/android/app/build/outputs/apk" -name '*.apk' -print -quit 2>/dev/null || true)
if [[ -n "$apk" ]]; then
    log_step done-quest "Quest package built: $apk"
    echo "Install it with: adb install -r \"$apk\""
else
    fail "Gradle reported success but no APK was found"
fi
