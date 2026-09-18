#include <aurora/aurora.h>

#ifdef AURORA_ENABLE_GX
#include "gfx/common.hpp"
#include "gfx/efb_ram_copy.hpp"
#include "gfx/stereo_replay.hpp"
#include "gx/fifo.hpp"
#include "gx/shader_info.hpp"
#include "imgui.hpp"
#include "stereo.hpp"
#include "webgpu/gpu.hpp"
#include <webgpu/webgpu_cpp.h>
#endif

#include "input.hpp"
#include "internal.hpp"
#include "window.hpp"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_thread.h>
#include <magic_enum.hpp>

#include "system_info.hpp"
#include "tracy/Tracy.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <atomic>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifdef AURORA_ENABLE_GX
namespace aurora::gx {
// Producer pacing feedback for the adaptive slot count, defined in lib/gx/shader_info.cpp;
// declared here so the C entry point at the bottom of this file can forward to it.
void report_producer_paced(bool paced) noexcept;
} // namespace aurora::gx
#endif

namespace aurora {
AuroraConfig g_config;
uint32_t g_sdlCustomEventsStart;
char g_gameName[4];
std::atomic<AuroraFrameWorkerWaitCallback> g_frameWorkerWaitCallback{nullptr};
// Presentation schedule for the frame being sealed, set by the producer. Jobs carry absolute
// deadlines derived from it, so the presenter cannot drift. Zero means present when ready.
std::atomic<uint64_t> g_presentScheduleBaseNanos{0};
std::atomic<uint64_t> g_presentScheduleIntervalNanos{0};

namespace {
Module Log("aurora");

std::atomic<uint32_t> g_captureFrame{UINT32_MAX};
std::string g_captureOutputPath;

struct StereoProviderRegistration {
  AuroraStereoFrameProvider callback = nullptr;
  void* userdata = nullptr;
};
#ifdef AURORA_ENABLE_GX
struct StereoSinkRegistration {
  stereo::SinkCallback callback = nullptr;
  stereo::SubmitCallback submitted = nullptr;
  void* userdata = nullptr;
};
#endif
std::mutex g_stereoRegistrationMutex;
StereoProviderRegistration g_stereoProvider;
std::atomic_bool g_stereoProviderActive{false};
#ifdef AURORA_ENABLE_GX
StereoSinkRegistration g_stereoSink;
#endif

// First-person camera relocation for one sealed frame: a row-major affine 3x4
// from the game's recorded view space into the space to render from. `active`
// false means the identity transform, i.e. render from the recorded camera.
struct StereoSceneAnchor {
  std::array<float, 12> anchorFromScene{
      1.f, 0.f, 0.f, 0.f, //
      0.f, 1.f, 0.f, 0.f, //
      0.f, 0.f, 1.f, 0.f,
  };
  bool active = false;
  float unitsPerMeter=0;
};
// Producer thread only, between aurora_set_stereo_scene_anchor() and the seal
// that consumes it. Cleared at every seal so a producer that stops publishing
// falls back to the recorded camera instead of freezing on a stale anchor.
StereoSceneAnchor g_pendingSceneAnchor;

using PresentClock = std::chrono::steady_clock;

struct PresentTimingSample {
  PresentClock::time_point presentedAt{};
  std::chrono::nanoseconds interval{};
  // Slot carried a copy of the native image instead of replayed interpolation, so the present
  // counts toward the rate but shows no new motion.
  bool duplicated = false;
};

std::mutex g_presentTimingMutex;
std::array<PresentTimingSample, 512> g_presentTimingSamples{};
size_t g_presentTimingWriteIndex = 0;
size_t g_presentTimingSampleCount = 0;
std::optional<PresentClock::time_point> g_lastSuccessfulPresent;
uint64_t g_totalPresentCount = 0;

void wait_until_precise(PresentClock::time_point deadline) noexcept {
  // Busy-spin tail covering the waitable timer's wakeup jitter. 150 us was not enough in
  // practice; 400 us absorbs the observed overshoot, about 1.6 ms of one core at 240 Hz.
  constexpr auto kSpinWindow = std::chrono::microseconds(400);
  auto now = PresentClock::now();
  if (now >= deadline) {
    return;
  }

  const auto timerDeadline = deadline - now > kSpinWindow ? deadline - kSpinWindow : deadline;
#if defined(_WIN32)
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
  struct HighResolutionTimer {
    HANDLE handle = ::CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                             TIMER_MODIFY_STATE | SYNCHRONIZE);
    ~HighResolutionTimer() {
      if (handle != nullptr) {
        ::CloseHandle(handle);
      }
    }
  };
  static thread_local HighResolutionTimer timer;
  if (timer.handle != nullptr && timerDeadline > now) {
    const auto remaining100ns =
        std::chrono::duration_cast<std::chrono::duration<int64_t, std::ratio<1, 10000000>>>(timerDeadline - now);
    LARGE_INTEGER due{};
    due.QuadPart = -std::max<int64_t>(remaining100ns.count(), 1);
    if (::SetWaitableTimerEx(timer.handle, &due, 0, nullptr, nullptr, nullptr, 0) != FALSE) {
      ::WaitForSingleObject(timer.handle, INFINITE);
    }
  } else
#endif
  {
    std::this_thread::sleep_until(timerDeadline);
  }

  while (PresentClock::now() < deadline) {
#if defined(_WIN32)
    YieldProcessor();
#else
    std::this_thread::yield();
#endif
  }
}

void record_successful_present(bool, uint32_t, std::chrono::nanoseconds, std::chrono::nanoseconds,
                               std::chrono::nanoseconds, std::chrono::nanoseconds, std::chrono::nanoseconds,
                               std::chrono::nanoseconds, bool duplicated, uint32_t, std::chrono::nanoseconds) noexcept {
  const auto now = PresentClock::now();
  std::chrono::nanoseconds interval{};
  {
    std::lock_guard lock(g_presentTimingMutex);
    if (g_lastSuccessfulPresent) {
      interval = std::chrono::duration_cast<std::chrono::nanoseconds>(now - *g_lastSuccessfulPresent);
    }
    g_lastSuccessfulPresent = now;
    g_presentTimingSamples[g_presentTimingWriteIndex] = {
        .presentedAt = now,
        .interval = interval,
        .duplicated = duplicated,
    };
    g_presentTimingWriteIndex = (g_presentTimingWriteIndex + 1) % g_presentTimingSamples.size();
    g_presentTimingSampleCount = std::min(g_presentTimingSampleCount + 1, g_presentTimingSamples.size());
    ++g_totalPresentCount;
  }
}

AuroraPresentTiming snapshot_present_timing() noexcept {
  constexpr auto kWindow = std::chrono::seconds(1);
  const auto cutoff = PresentClock::now() - kWindow;
  std::vector<double> milliseconds;
  uint64_t totalPresentCount = 0;
  size_t newMotionSamples = 0;
  {
    std::lock_guard lock(g_presentTimingMutex);
    totalPresentCount = g_totalPresentCount;
    milliseconds.reserve(g_presentTimingSampleCount);
    for (size_t i = 0; i < g_presentTimingSampleCount; ++i) {
      const auto& sample = g_presentTimingSamples[i];
      if (sample.presentedAt >= cutoff && sample.interval.count() > 0) {
        milliseconds.push_back(std::chrono::duration<double, std::milli>(sample.interval).count());
        if (!sample.duplicated) {
          ++newMotionSamples;
        }
      }
    }
  }

  AuroraPresentTiming result{
      .totalPresentCount = totalPresentCount,
      .sampleCount = static_cast<uint32_t>(milliseconds.size()),
  };
  if (milliseconds.empty()) {
    return result;
  }

  double totalMilliseconds = 0.0;
  for (const double value : milliseconds) {
    totalMilliseconds += value;
  }
  result.averageFrameTimeMs = totalMilliseconds / static_cast<double>(milliseconds.size());
  result.framesPerSecond = result.averageFrameTimeMs > 0.0 ? 1000.0 / result.averageFrameTimeMs : 0.0;
  // Duplicated presentation slots keep the presented cadence but carry no new
  // motion; scale them out so this reads as the rate the eye actually sees.
  result.effectiveFramesPerSecond =
      result.framesPerSecond * (static_cast<double>(newMotionSamples) / static_cast<double>(milliseconds.size()));
  for (const double value : milliseconds) {
    const double difference = value - result.averageFrameTimeMs;
    result.jitterMs += difference * difference;
  }
  result.jitterMs = std::sqrt(result.jitterMs / static_cast<double>(milliseconds.size()));
  std::sort(milliseconds.begin(), milliseconds.end());
  result.p95FrameTimeMs =
      milliseconds[static_cast<size_t>(std::floor(static_cast<double>(milliseconds.size() - 1) * 0.95))];
  return result;
}

// ImGui draw data lives in the shared context, so starting the next ImGui frame destroys draw
// lists the sealed slots still replay. That is why ImGui callers wait for DONE, not SEALED.
enum class ImGuiFramePolicy {
  // Start ImGui's next frame as part of renderer preparation, as the
  // synchronous path always has.
  Immediate,
  // Leave it unstarted; the caller owes an imgui::new_frame() once it has
  // finished replaying this frame's draw data.
  Deferred,
};

bool begin_frame_impl(bool pumpEvents, ImGuiFramePolicy imguiPolicy = ImGuiFramePolicy::Immediate,
                      bool* imguiNewFrameOwed = nullptr) noexcept;
bool begin_frame_render_state_impl(ImGuiFramePolicy imguiPolicy, bool* imguiNewFrameOwed) noexcept;
void end_frame_impl(bool pumpEvents, bool drainFifo, uint64_t contentTag, const StereoSceneAnchor& sceneAnchor) noexcept;

// The two publication points of a frame-worker cycle, cleared together under `mutex`. Sealed:
// producer-shared renderer state is free again. Done: slots encoded, presented, ImGui restarted.
enum class FrameWorkerPhase {
  Sealed,
  Done,
};

struct FrameWorkerState {
  std::mutex mutex;
  std::condition_variable cv;
  std::thread thread;
  std::thread::id threadId{};
  bool started = false;
  bool stop = false;
  bool jobPending = false;
  // Written with jobPending and copied by the worker under this mutex. They
  // belong to that exact queued frame, not to the producer's next frame.
  uint64_t contentTag = AURORA_STEREO_CONTENT_TAG_UNKNOWN;
  StereoSceneAnchor sceneAnchor{};
  // Readiness is polled thousands of times per frame, so these flags double as a publication
  // barrier. `sealed` is released before `ready`, and both are cleared under `mutex`.
  std::atomic_bool sealed{true};
  std::atomic_bool ready{true};
  bool framePrepared = false;
  bool prepareAllowed = false;
};

FrameWorkerState g_frameWorker;

bool frame_worker_requested() noexcept {
#ifdef AURORA_ENABLE_GX
  static const bool enabled = [] {
#if defined(__APPLE__)
    // ImGui's SDL backend may raise an SDL window from ImGui::NewFrame(). On
    // macOS that reaches AppKit, whose window operations are main-thread-only;
    // doing it on the frame worker terminates the process with EXC_BREAKPOINT.
    // Keep all SDL/ImGui work on the calling thread until the worker no longer
    // owns frame preparation on Apple platforms.
    return false;
#endif
#if defined(_WIN32)
    // RenderDoc's D3D12 layer is injected before Aurora starts and needs device and command
    // ownership on one thread, so keep frame submission synchronous there.
    if (::GetModuleHandleW(L"renderdoc.dll") != nullptr) {
      return false;
    }
#endif
    return true;
  }();
#if defined(_WIN32)
  static const bool renderDocLoaded = ::GetModuleHandleW(L"renderdoc.dll") != nullptr;
  if (renderDocLoaded) {
    static const bool logged = [] {
      Log.info("Disabled asynchronous frame submission worker for RenderDoc capture");
      return true;
    }();
    (void)logged;
  }
#endif
  return enabled;
#else
  return false;
#endif
}

#ifdef AURORA_ENABLE_GX
// Returns false when a stop request was observed mid-cycle.
bool run_frame_worker_cycle(gfx::SealedFrame& sealedFrame, uint64_t contentTag,
                            const StereoSceneAnchor& sceneAnchor) noexcept;
#endif

void frame_worker_main() noexcept {
  {
    std::lock_guard lock(g_frameWorker.mutex);
    g_frameWorker.threadId = std::this_thread::get_id();
  }

#ifdef AURORA_ENABLE_GX
  // Owned by the worker for its whole lifetime so the sealed pass vector and
  // its pooled command lists keep their capacity across frames.
  gfx::SealedFrame sealedFrame;
#endif

  for (;;) {
    uint64_t contentTag = AURORA_STEREO_CONTENT_TAG_UNKNOWN;
    StereoSceneAnchor sceneAnchor{};
    {
      std::unique_lock lock(g_frameWorker.mutex);
      g_frameWorker.cv.wait(lock, [] { return g_frameWorker.stop || g_frameWorker.jobPending; });
      if (g_frameWorker.stop) {
        break;
      }
      contentTag = g_frameWorker.contentTag;
      g_frameWorker.contentTag = AURORA_STEREO_CONTENT_TAG_UNKNOWN;
      sceneAnchor = g_frameWorker.sceneAnchor;
      g_frameWorker.sceneAnchor = {};
      g_frameWorker.jobPending = false;
    }

    // The CPU already decoded the sealed frame at its GX boundary; the worker only owns
    // encode/submit/present, so it never touches the producer's next FIFO buffer.
#ifdef AURORA_ENABLE_GX
    if (!run_frame_worker_cycle(sealedFrame, contentTag, sceneAnchor)) {
      break;
    }
#else
    (void)contentTag;
    (void)sceneAnchor;
#endif
  }

  // A stop request can unblock either wait above mid-cycle, so release both phases before
  // stop_frame_worker() joins.
  {
    std::lock_guard lock(g_frameWorker.mutex);
    g_frameWorker.sealed.store(true, std::memory_order_release);
    g_frameWorker.ready.store(true, std::memory_order_release);
  }
  g_frameWorker.cv.notify_all();
}

void ensure_frame_worker_started() noexcept {
  if (!frame_worker_requested()) {
    return;
  }
  std::lock_guard lock(g_frameWorker.mutex);
  if (g_frameWorker.started) {
    return;
  }
  g_frameWorker.stop = false;
  g_frameWorker.jobPending = false;
  g_frameWorker.contentTag = AURORA_STEREO_CONTENT_TAG_UNKNOWN;
  g_frameWorker.sceneAnchor = {};
  g_frameWorker.sealed.store(true, std::memory_order_release);
  g_frameWorker.ready.store(true, std::memory_order_release);
  g_frameWorker.prepareAllowed = false;
  g_frameWorker.started = true;
  g_frameWorker.thread = std::thread(frame_worker_main);
  Log.info("Enabled bounded asynchronous frame submission worker");
}

bool frame_worker_phase_reached(FrameWorkerPhase phase) noexcept {
  return phase == FrameWorkerPhase::Sealed ? g_frameWorker.sealed.load(std::memory_order_acquire)
                                           : g_frameWorker.ready.load(std::memory_order_acquire);
}

bool wait_for_frame_worker_private_for(FrameWorkerPhase phase, std::chrono::microseconds timeout) noexcept;

void wait_for_frame_worker_private(FrameWorkerPhase phase) noexcept {
  constexpr auto kWaitServiceInterval = std::chrono::milliseconds(1);
  while (!wait_for_frame_worker_private_for(phase, kWaitServiceInterval)) {}
}

bool wait_for_frame_worker_private_for(FrameWorkerPhase phase, std::chrono::microseconds timeout) noexcept {
  // Each phase's state is published before its flag is set, which keeps the common
  // display-list path out of the worker control mutex.
  if (frame_worker_phase_reached(phase)) {
    return true;
  }

  std::unique_lock lock(g_frameWorker.mutex);
  if (!g_frameWorker.started || g_frameWorker.threadId == std::this_thread::get_id()) {
    return true;
  }
  if (timeout <= std::chrono::microseconds::zero()) {
    return frame_worker_phase_reached(phase);
  }

  const bool reached = g_frameWorker.cv.wait_for(lock, timeout, [phase] { return frame_worker_phase_reached(phase); });
  if (reached) {
    return true;
  }

  lock.unlock();
  // Both phases service the guest's alarm/retrace pump identically; the
  // producer must keep its own timing alive however long it waits.
  if (const auto callback = g_frameWorkerWaitCallback.load(std::memory_order_acquire)) {
    callback();
  }
  return false;
}

void stop_frame_worker() noexcept {
  {
    std::lock_guard lock(g_frameWorker.mutex);
    if (!g_frameWorker.started) {
      return;
    }
    g_frameWorker.stop = true;
    g_frameWorker.prepareAllowed = true;
  }
  g_frameWorker.cv.notify_all();
  if (g_frameWorker.thread.joinable()) {
    g_frameWorker.thread.join();
  }
  std::lock_guard lock(g_frameWorker.mutex);
  g_frameWorker.started = false;
  g_frameWorker.threadId = {};
  g_frameWorker.framePrepared = false;
  g_frameWorker.jobPending = false;
  g_frameWorker.contentTag = AURORA_STEREO_CONTENT_TAG_UNKNOWN;
  g_frameWorker.sceneAnchor = {};
}

uint32_t align_to(uint32_t value, uint32_t alignment) noexcept { return (value + alignment - 1) & ~(alignment - 1); }

void append_u16(std::ofstream& out, uint16_t value) {
  const char bytes[] = {static_cast<char>(value), static_cast<char>(value >> 8)};
  out.write(bytes, sizeof(bytes));
}

void append_u32(std::ofstream& out, uint32_t value) {
  const char bytes[] = {static_cast<char>(value), static_cast<char>(value >> 8), static_cast<char>(value >> 16),
                        static_cast<char>(value >> 24)};
  out.write(bytes, sizeof(bytes));
}

bool write_bmp(const char* path, const uint8_t* pixels, uint32_t width, uint32_t height, uint32_t bytesPerRow,
               bool bgra) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out)
    return false;
  constexpr uint32_t pixelOffset = 14 + 40;
  const uint32_t imageSize = width * height * 4;
  out.write("BM", 2);
  append_u32(out, pixelOffset + imageSize);
  append_u16(out, 0);
  append_u16(out, 0);
  append_u32(out, pixelOffset);
  append_u32(out, 40);
  append_u32(out, width);
  append_u32(out, height);
  append_u16(out, 1);
  append_u16(out, 32);
  append_u32(out, 0);
  append_u32(out, imageSize);
  append_u32(out, 2835);
  append_u32(out, 2835);
  append_u32(out, 0);
  append_u32(out, 0);
  std::vector<uint8_t> row(width * 4);
  for (uint32_t y = height; y-- > 0;) {
    const auto* source = pixels + static_cast<size_t>(y) * bytesPerRow;
    if (bgra) {
      out.write(reinterpret_cast<const char*>(source), row.size());
    } else {
      for (uint32_t x = 0; x < width; ++x) {
        row[x * 4 + 0] = source[x * 4 + 2];
        row[x * 4 + 1] = source[x * 4 + 1];
        row[x * 4 + 2] = source[x * 4 + 0];
        row[x * 4 + 3] = source[x * 4 + 3];
      }
      out.write(reinterpret_cast<const char*>(row.data()), row.size());
    }
  }
  return out.good();
}

#ifdef AURORA_ENABLE_GX
// GPU
using webgpu::g_device;
using webgpu::g_instance;
using webgpu::g_queue;
using webgpu::g_surface;
std::recursive_mutex g_rendererGpuMutex;
std::mutex g_queueSubmitMutex;
// Surface ownership, held apart from the renderer mutex: a presentation slot touches the surface,
// its image and the queue, nothing recorded. Lock order is surface then renderer.
std::mutex g_surfaceMutex;
std::atomic<bool> g_surfaceReconfigurePending{false};
std::atomic<bool> g_surfaceRecreatePending{false};

struct StereoEyeTarget {
  webgpu::TextureWithSampler color;
  webgpu::TextureWithSampler resolvedColor;
  webgpu::TextureWithSampler depth;
  uint32_t requestedWidth = 0;
  uint32_t requestedHeight = 0;
  uint32_t samples = 0;
  wgpu::TextureFormat colorFormat = wgpu::TextureFormat::Undefined;
  wgpu::TextureFormat depthFormat = wgpu::TextureFormat::Undefined;

  const webgpu::TextureWithSampler& output() const noexcept { return resolvedColor.texture ? resolvedColor : color; }
};
std::array<StereoEyeTarget, AURORA_STEREO_EYE_COUNT> g_stereoEyeTargets;
std::array<webgpu::TextureWithSampler,AURORA_STEREO_EYE_COUNT> g_stereoUpscaled;
std::array<wgpu::BindGroup,AURORA_STEREO_EYE_COUNT> g_stereoUpscaleBindings;
std::array<std::array<uint32_t,2>,AURORA_STEREO_EYE_COUNT> g_stereoOutputSizes{};

void ensure_stereo_eye_target(uint32_t eyeIndex, uint32_t width, uint32_t height) {
  auto& target = g_stereoEyeTargets[eyeIndex];
  const uint32_t samples = webgpu::g_graphicsConfig.msaaSamples;
  if (target.color.texture && target.requestedWidth == width && target.requestedHeight == height &&
      target.samples == samples && target.colorFormat == webgpu::g_graphicsConfig.surfaceConfiguration.format &&
      target.depthFormat == webgpu::g_graphicsConfig.depthFormat) {
    return;
  }

  target = {};
  target.color = webgpu::create_render_texture(width, height, samples > 1);
  if (samples > 1) {
    target.resolvedColor = webgpu::create_render_texture(target.color.size.width, target.color.size.height, false);
  }

  const wgpu::TextureDescriptor depthDescriptor{
      .label = eyeIndex == 0 ? "Stereo left eye depth" : "Stereo right eye depth",
      .usage = wgpu::TextureUsage::RenderAttachment,
      .dimension = wgpu::TextureDimension::e2D,
      .size = target.color.size,
      .format = webgpu::g_graphicsConfig.depthFormat,
      .mipLevelCount = 1,
      .sampleCount = samples,
  };
  target.depth.texture = g_device.CreateTexture(&depthDescriptor);
  target.depth.view = target.depth.texture.CreateView();
  target.depth.size = target.color.size;
  target.depth.format = webgpu::g_graphicsConfig.depthFormat;
  target.requestedWidth = width;
  target.requestedHeight = height;
  target.samples = samples;
  target.colorFormat = target.color.format;
  target.depthFormat = target.depth.format;
}

std::optional<AuroraStereoFrame> request_stereo_frame(uint32_t logicalFrame, uint64_t contentTag) noexcept {
  StereoProviderRegistration registration;
  {
    std::lock_guard lock(g_stereoRegistrationMutex);
    registration = g_stereoProvider;
  }
  if (registration.callback == nullptr) {
    return std::nullopt;
  }

  AuroraStereoFrame frame{};
  bool provided = false;
  try {
    provided = registration.callback(logicalFrame, &frame, registration.userdata);
  } catch (...) {
    Log.error("Stereo frame provider threw an exception; rendering frame {} in mono", logicalFrame);
    return std::nullopt;
  }
  if (!provided) {
    return std::nullopt;
  }
  if (frame.mode != AURORA_STEREO_FRAME_IMMERSIVE_REPLAY && frame.mode != AURORA_STEREO_FRAME_VIRTUAL_SCREEN) {
    Log.warn("Stereo frame {} has invalid mode {}; rendering in mono", logicalFrame, static_cast<uint32_t>(frame.mode));
    return std::nullopt;
  }
  // A virtual-screen packet only copies the completed mono image and remains
  // a safe fallback across a transition. Immersive replay changes the GX
  // transforms, so it requires the exact tag latched for this sealed content.
  if (frame.mode == AURORA_STEREO_FRAME_IMMERSIVE_REPLAY &&
      (contentTag == AURORA_STEREO_CONTENT_TAG_UNKNOWN || frame.contentTag != contentTag)) {
    static uint32_t mismatchedFrames = 0;
    ++mismatchedFrames;
    if ((mismatchedFrames & (mismatchedFrames - 1)) == 0) {
      Log.warn("Stereo content rejected: frame={}, count={}, packet-tag={}, sealed-tag={}",
               logicalFrame, mismatchedFrames, frame.contentTag, contentTag);
    }
    return std::nullopt;
  }

  const auto finite = [](const float* values, size_t count) {
    // aurora_core is built with -ffast-math, so std::isfinite may be folded to
    // true. Inspecting the IEEE-754 exponent keeps the provider boundary safe
    // under the target's real release flags.
    return std::all_of(values, values + count, [](const float& value) {
      uint32_t bits = 0;
      std::memcpy(&bits, &value, sizeof(bits));
      return (bits & 0x7f800000u) != 0x7f800000u;
    });
  };
  for (uint32_t eye = 0; eye < AURORA_STEREO_EYE_COUNT; ++eye) {
    const auto& input = frame.eyes[eye];
    const bool transformsValid = frame.mode == AURORA_STEREO_FRAME_VIRTUAL_SCREEN ||
                                 (finite(input.projection, 16) && finite(input.viewFromCenter, 12) &&
                                  (!frame.handHud || finite(frame.handHudViewFromPanel[eye], 12)));
    if (input.width == 0 || input.height == 0 || !transformsValid) {
      Log.warn("Stereo frame {} has invalid eye {} dimensions or transforms; rendering in mono", logicalFrame, eye);
      return std::nullopt;
    }
  }
  return frame;
}

gfx::StereoReplayFrame make_stereo_replay_frame(const AuroraStereoFrame& input, const StereoSceneAnchor& sceneAnchor) {
  Mat3x4<float> anchorFromScene;
  std::memcpy(&anchorFromScene, sceneAnchor.anchorFromScene.data(), sizeof(anchorFromScene));
  gfx::StereoReplayFrame replay{};
  replay.cockpit = input.cockpit;
  const float frameUnits=sceneAnchor.active && sceneAnchor.unitsPerMeter>0 ? sceneAnchor.unitsPerMeter:input.cockpit.unitsPerMeter;
  const float unitRatio=input.cockpit.unitsPerMeter>0 ? frameUnits/input.cockpit.unitsPerMeter:1;
  replay.cockpit.unitsPerMeter=frameUnits;
  for (uint32_t eye = 0; eye < AURORA_STEREO_EYE_COUNT; ++eye) {
    const float scale=input.mode==AURORA_STEREO_FRAME_IMMERSIVE_REPLAY && input.renderScale>=.5f && input.renderScale<1 ? input.renderScale:1;
    const uint32_t width=std::max(1u,uint32_t(input.eyes[eye].width*scale));
    const uint32_t height=std::max(1u,uint32_t(input.eyes[eye].height*scale));
    const auto previousView=g_stereoEyeTargets[eye].output().view.Get();
    ensure_stereo_eye_target(eye,width,height);
    g_stereoOutputSizes[eye]={input.eyes[eye].width,input.eyes[eye].height};
    if(previousView!=g_stereoEyeTargets[eye].output().view.Get()) g_stereoUpscaleBindings[eye]=nullptr;
    const auto& owned = g_stereoEyeTargets[eye];
    const auto& output = owned.output();
    auto& view = replay.eyes[eye];
    view.target = {
        .colorView = owned.color.view,
        .resolveView = owned.resolvedColor.view,
        .depthView = owned.depth.view,
        .copySourceTexture = output.texture,
        .copySourceView = output.view,
        .copySourceDepthView = owned.depth.view,
        .size = owned.color.size,
        .msaaSamples = webgpu::g_graphicsConfig.msaaSamples,
    };
    std::memcpy(&view.projection, input.eyes[eye].projection, sizeof(view.projection));
    std::memcpy(&view.viewFromCenter, input.eyes[eye].viewFromCenter, sizeof(view.viewFromCenter));
    // The sealed guest camera owns its scale. XR may have sampled just before
    // lightning or a camera transition; rescale only head/IPD translation.
    auto* eyeTransform=reinterpret_cast<float*>(&view.viewFromCenter);
    eyeTransform[3]*=unitRatio;eyeTransform[7]*=unitRatio;eyeTransform[11]*=unitRatio;
    view.handHud = input.handHud;
    if (input.handHud) {
      std::memcpy(&view.handHudViewFromPanel, input.handHudViewFromPanel[eye], sizeof(view.handHudViewFromPanel));
    }
    // World draws already carry the recorded camera, so they need the anchor
    // folded in; the virtual screen is authored in the anchored camera's space
    // and keeps viewFromCenter.
    view.viewFromScene = sceneAnchor.active
                             ? gfx::stereo_replay::compose_affine(view.viewFromCenter, anchorFromScene)
                             : view.viewFromCenter;
  }
  return replay;
}

#include "vr_ui.hpp"

void encode_virtual_screen_eye(wgpu::CommandEncoder& encoder, const webgpu::PresentSource& source, uint32_t eyeIndex) {
  const auto& output = g_stereoEyeTargets[eyeIndex].output();
  const std::array attachments{
      wgpu::RenderPassColorAttachment{
          .view = output.view,
          .loadOp = wgpu::LoadOp::Clear,
          .storeOp = wgpu::StoreOp::Store,
          .clearValue = {.r = 0.0, .g = 0.0, .b = 0.0, .a = 1.0},
      },
  };
  const wgpu::RenderPassDescriptor descriptor{
      .label = eyeIndex == 0 ? "Virtual screen left eye" : "Virtual screen right eye",
      .colorAttachmentCount = attachments.size(),
      .colorAttachments = attachments.data(),
  };
  {
    const auto pass = encoder.BeginRenderPass(&descriptor);
    if (source.bindGroup && source.size.width != 0 && source.size.height != 0) {
      const auto viewport = webgpu::calculate_present_viewport(output.size.width, output.size.height, source.size.width,
                                                               source.size.height);
      pass.SetPipeline(webgpu::g_CopyPipeline);
      pass.SetBindGroup(0, source.bindGroup, 0, nullptr);
      pass.SetViewport(viewport.left, viewport.top, viewport.width, viewport.height, viewport.znear, viewport.zfar);
      pass.Draw(3);
    }
    pass.End();
  }
}

struct PendingStereoSink {
  stereo::SinkFrame frame;
  stereo::SubmitCallback submitted = nullptr;
  void* userdata = nullptr;
};

std::optional<PendingStereoSink> run_stereo_sink(wgpu::CommandEncoder& encoder, uint64_t frameToken,
                                                 uint32_t logicalFrame, AuroraStereoFrameMode mode) noexcept {
  StereoSinkRegistration registration;
  {
    std::lock_guard lock(g_stereoRegistrationMutex);
    registration = g_stereoSink;
  }
  if (registration.callback == nullptr) {
    return std::nullopt;
  }

  stereo::SinkFrame frame{
      .frameToken = frameToken,
      .logicalFrame = logicalFrame,
      .mode = mode,
  };
  // The device probe selected this transform using GPU pixels, not scene contents.
  const int orientation = mode == AURORA_STEREO_FRAME_IMMERSIVE_REPLAY
      ? webgpu::g_RaceOutputCopyMode : 0;
  const bool correction = mode == AURORA_STEREO_FRAME_IMMERSIVE_REPLAY && webgpu::g_RaceOutputNeedsCopy;
  for (uint32_t eye = 0; eye < AURORA_STEREO_EYE_COUNT; ++eye) {
    const auto& output = g_stereoEyeTargets[eye].output();
    const webgpu::TextureWithSampler* image=&output;
    const auto size=g_stereoOutputSizes[eye];
    // Apply only to the final race image, after world, hands and HUD. Menus
    // retain their independent, room-anchored orientation.
    if(correction || output.size.width!=size[0] || output.size.height!=size[1]) {
      auto& upscale=g_stereoUpscaled[eye];
      if(!upscale.texture || upscale.size.width!=size[0] || upscale.size.height!=size[1] || upscale.format!=output.format)
        upscale=webgpu::create_render_texture(size[0],size[1],false);
      auto& binding=g_stereoUpscaleBindings[eye];
      if(!binding) binding=webgpu::create_copy_bind_group(output);
      const wgpu::RenderPassColorAttachment attachment{.view=upscale.view,.loadOp=wgpu::LoadOp::Clear,.storeOp=wgpu::StoreOp::Store};
      const wgpu::RenderPassDescriptor pd{.label="VR output orientation and upscale",.colorAttachmentCount=1,.colorAttachments=&attachment};
      auto pass=encoder.BeginRenderPass(&pd);pass.SetPipeline(webgpu::g_OrientedCopyPipelines[orientation]);pass.SetBindGroup(0,binding);pass.Draw(3);pass.End();
      image=&upscale;
    }
    frame.eyes[eye] = {
        .texture = &image->texture,
        .view = &image->view,
        .size = image->size,
        .format = image->format,
    };
  }
  if (!registration.callback(encoder, frame, registration.userdata)) {
    return std::nullopt;
  }
  return PendingStereoSink{
      .frame = frame,
      .submitted = registration.submitted,
      .userdata = registration.userdata,
  };
}

void request_surface_reconfigure() noexcept { g_surfaceReconfigurePending.store(true, std::memory_order_release); }

void request_surface_recreate() noexcept {
  g_surfaceRecreatePending.store(true, std::memory_order_release);
  g_surfaceReconfigurePending.store(true, std::memory_order_release);
}

struct PendingFrameCapture {
  wgpu::Buffer buffer;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t bytesPerRow = 0;
  uint64_t bufferSize = 0;
  bool bgra = false;
  std::string path;
};

std::optional<PendingFrameCapture> encode_frame_capture(const wgpu::CommandEncoder& encoder,
                                                        const webgpu::PresentSource& source) {
  const uint32_t requestedFrame = g_captureFrame.load(std::memory_order_acquire);
  const uint32_t currentFrame = gfx::current_frame();
  if (requestedFrame == UINT32_MAX || currentFrame < requestedFrame)
    return std::nullopt;
  g_captureFrame.store(UINT32_MAX, std::memory_order_release);
  if (currentFrame != requestedFrame) {
    Log.error("Missed requested frame capture {} (current frame {})", requestedFrame, currentFrame);
    return std::nullopt;
  }
  if (!source.texture || source.size.width == 0 || source.size.height == 0) {
    Log.error("Frame {} capture has no present-source texture", currentFrame);
    return std::nullopt;
  }
  const bool bgra =
      source.format == wgpu::TextureFormat::BGRA8Unorm || source.format == wgpu::TextureFormat::BGRA8UnormSrgb;
  const bool rgba =
      source.format == wgpu::TextureFormat::RGBA8Unorm || source.format == wgpu::TextureFormat::RGBA8UnormSrgb;
  if (!bgra && !rgba) {
    Log.error("Frame {} capture does not support texture format {}", currentFrame,
              magic_enum::enum_name(source.format));
    return std::nullopt;
  }
  const uint32_t bytesPerRow = align_to(source.size.width * 4, 256);
  const uint64_t bufferSize = static_cast<uint64_t>(bytesPerRow) * source.size.height;
  const wgpu::BufferDescriptor descriptor{
      .label = "Visual validation frame capture",
      .usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead,
      .size = bufferSize,
  };
  auto buffer = g_device.CreateBuffer(&descriptor);
  const wgpu::TexelCopyTextureInfo sourceInfo{
      .texture = source.texture,
      .mipLevel = 0,
      .origin = {0, 0, 0},
      .aspect = wgpu::TextureAspect::All,
  };
  const wgpu::TexelCopyBufferInfo destinationInfo{
      .layout = {.offset = 0, .bytesPerRow = bytesPerRow, .rowsPerImage = source.size.height},
      .buffer = buffer,
  };
  encoder.CopyTextureToBuffer(&sourceInfo, &destinationInfo, &source.size);
  return PendingFrameCapture{
      .buffer = std::move(buffer),
      .width = source.size.width,
      .height = source.size.height,
      .bytesPerRow = bytesPerRow,
      .bufferSize = bufferSize,
      .bgra = bgra,
      .path = g_captureOutputPath,
  };
}

void complete_frame_capture(PendingFrameCapture& capture) {
  wgpu::MapAsyncStatus mapStatus = wgpu::MapAsyncStatus::CallbackCancelled;
  wgpu::StringView mapMessage{};
  const auto future =
      capture.buffer.MapAsync(wgpu::MapMode::Read, 0, capture.bufferSize, wgpu::CallbackMode::WaitAnyOnly,
                              [&mapStatus, &mapMessage](wgpu::MapAsyncStatus status, wgpu::StringView message) {
                                mapStatus = status;
                                mapMessage = message;
                              });
  const auto waitStatus = g_instance.WaitAny(future, 5000000000);
  if (waitStatus != wgpu::WaitStatus::Success || mapStatus != wgpu::MapAsyncStatus::Success) {
    Log.error("Frame capture readback failed wait={} map={} message={}", magic_enum::enum_name(waitStatus),
              magic_enum::enum_name(mapStatus), mapMessage);
    return;
  }
  const auto* pixels = static_cast<const uint8_t*>(capture.buffer.GetConstMappedRange(0, capture.bufferSize));
  if (write_bmp(capture.path.c_str(), pixels, capture.width, capture.height, capture.bytesPerRow, capture.bgra)) {
    Log.info("Captured rendered frame to '{}' ({}x{})", capture.path, capture.width, capture.height);
  } else {
    Log.error("Failed to write rendered frame capture to '{}'", capture.path);
  }
  capture.buffer.Unmap();
}
#endif

// AuroraBackend is an anonymous C typedef enum, which magic_enum cannot
// reflect under this toolchain -- name it by hand for diagnostics.
constexpr const char* backend_name(AuroraBackend backend) noexcept {
  switch (backend) {
  case BACKEND_AUTO:
    return "Auto";
  case BACKEND_D3D11:
    return "D3D11";
  case BACKEND_D3D12:
    return "D3D12";
  case BACKEND_METAL:
    return "Metal";
  case BACKEND_VULKAN:
    return "Vulkan";
  case BACKEND_OPENGL:
    return "OpenGL";
  case BACKEND_OPENGLES:
    return "OpenGLES";
  case BACKEND_WEBGPU:
    return "WebGPU";
  case BACKEND_NULL:
    return "Null";
  }
  return "Unknown";
}

#ifdef AURORA_ENABLE_GX
constexpr std::array PreferredBackendOrder{
#ifdef ENABLE_BACKEND_WEBGPU
    BACKEND_WEBGPU,
#endif
#ifdef DAWN_ENABLE_BACKEND_D3D12
    BACKEND_D3D12,
#endif
#if defined(_WIN32) && defined(DAWN_ENABLE_BACKEND_D3D11)
    BACKEND_D3D11,
#endif
#ifdef DAWN_ENABLE_BACKEND_METAL
    BACKEND_METAL,
#endif
#ifdef DAWN_ENABLE_BACKEND_VULKAN
    BACKEND_VULKAN,
#endif
#if !defined(_WIN32) && defined(DAWN_ENABLE_BACKEND_D3D11)
    BACKEND_D3D11,
#endif
// #ifdef DAWN_ENABLE_BACKEND_DESKTOP_GL
//     BACKEND_OPENGL,
// #endif
// #ifdef DAWN_ENABLE_BACKEND_OPENGLES
//     BACKEND_OPENGLES,
// #endif
#ifdef DAWN_ENABLE_BACKEND_NULL
    BACKEND_NULL,
#endif
};
#else
constexpr std::array<AuroraBackend, 0> PreferredBackendOrder{};
#endif

bool g_initialFrame = false;

AuroraInfo initialize(int argc, char* argv[], const AuroraConfig& config) noexcept {
  g_config = config;
  Log.info("Aurora initializing");
  log_system_information();
  if (g_config.appName == nullptr) {
    g_config.appName = "Aurora";
  } else {
    g_config.appName = strdup(g_config.appName);
  }
  if (g_config.userPath == nullptr) {
    g_config.userPath = SDL_GetPrefPath(nullptr, g_config.appName);
  } else {
    g_config.userPath = strdup(g_config.userPath);
  }
  if (g_config.cachePath == nullptr) {
    g_config.cachePath = SDL_GetPrefPath(nullptr, g_config.appName);
  } else {
    g_config.cachePath = strdup(g_config.cachePath);
  }
  if (g_config.resourcesPath == nullptr) {
    g_config.resourcesPath = SDL_GetBasePath();
  } else {
    g_config.resourcesPath = strdup(g_config.resourcesPath);
  }
  if (g_config.pipelineCachePath == nullptr) {
    g_config.pipelineCachePath = g_config.cachePath;
  } else {
    g_config.pipelineCachePath = strdup(g_config.pipelineCachePath);
  }
  if (g_config.msaa == 0) {
    g_config.msaa = 1;
  }
  if (g_config.maxTextureAnisotropy == 0) {
    g_config.maxTextureAnisotropy = 16;
  }
  ASSERT(window::initialize(), "Error initializing window");

  g_sdlCustomEventsStart = SDL_RegisterEvents(2);
  ASSERT(g_sdlCustomEventsStart, "Failed to allocate user events: {}", SDL_GetError());
  ASSERT(window::initialize_event_watch(), "Error initializing SDL event watch");

#ifdef AURORA_ENABLE_GX
  /* Attempt to create a window using the calling application's desired backend */
  const AuroraBackend requestedBackend = config.desiredBackend;
  AuroraBackend selectedBackend = requestedBackend;
  bool windowCreated = false;
  if (selectedBackend != BACKEND_AUTO) {
    Log.info("Requested graphics backend: {}", backend_name(selectedBackend));
    if (window::create_window(selectedBackend)) {
      if (webgpu::initialize(selectedBackend)) {
        windowCreated = true;
      } else {
        window::destroy_window();
      }
    } else {
      Log.error("Failed to create a window for backend {}: {}", backend_name(selectedBackend), SDL_GetError());
    }
    if (!windowCreated) {
      /* An explicitly requested backend that cannot be brought up falls back to the BACKEND_AUTO
       * search instead of aborting, and the substitution is always reported. */
      Log.error(
          "Requested graphics backend {} is unavailable on this system; "
          "falling back to automatic selection",
          backend_name(requestedBackend));
    }
  }

  if (!windowCreated) {
    for (const auto backendType : PreferredBackendOrder) {
      selectedBackend = backendType;
      if (!window::create_window(selectedBackend)) {
        continue;
      }
      if (webgpu::initialize(selectedBackend)) {
        windowCreated = true;
        break;
      } else {
        window::destroy_window();
      }
    }
  }

  ASSERT(windowCreated, "Error creating window: {}", SDL_GetError());
  if (requestedBackend != BACKEND_AUTO && selectedBackend != requestedBackend) {
    Log.error(
        "Graphics backend fallback in effect: video.graphics_api requested {}, "
        "running on {}",
        backend_name(requestedBackend), backend_name(selectedBackend));
  }

  // Initialize SDL_Renderer for ImGui when we can't use a Dawn backend
  if (webgpu::g_backendType == wgpu::BackendType::Null) {
    ASSERT(window::create_renderer(), "Failed to initialize SDL renderer: {}", SDL_GetError());
  }
#else
  AuroraBackend selectedBackend = BACKEND_NULL;
  ASSERT(window::create_window(BACKEND_NULL), "Error creating window: {}", SDL_GetError());
  ASSERT(window::create_renderer(), "Failed to initialize SDL renderer: {}", SDL_GetError());
#endif

  window::show_window();

#ifdef AURORA_ENABLE_GX
  gfx::initialize();

  imgui::create_context();
#endif
  const auto size = window::get_window_size();
  Log.info("Using framebuffer size {}x{} scale {}", size.fb_width, size.fb_height, size.scale);
#ifdef AURORA_ENABLE_GX
  if (g_config.imGuiInitCallback != nullptr) {
    g_config.imGuiInitCallback(&size);
  }
  imgui::initialize();
#endif

  g_initialFrame = true;
  g_config.desiredBackend = selectedBackend;
  return {
      .backend = selectedBackend,
      .userPath = g_config.userPath,
      .cachePath = g_config.cachePath,
      .window = window::get_sdl_window(),
      .windowSize = size,
  };
}

#ifdef AURORA_ENABLE_GX
struct AcquiredSurfaceTexture {
  wgpu::Texture texture;
  wgpu::TextureView view;
};

std::optional<AcquiredSurfaceTexture> acquire_surface_texture() noexcept {
  if (!window::is_presentable()) {
    request_surface_reconfigure();
    return std::nullopt;
  }
  if (!g_surface) {
    request_surface_reconfigure();
    return std::nullopt;
  }

  wgpu::SurfaceTexture surfaceTexture;
  g_surface.GetCurrentTexture(&surfaceTexture);
  switch (surfaceTexture.status) {
  case wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal:
    return AcquiredSurfaceTexture{
        .texture = surfaceTexture.texture,
        .view = surfaceTexture.texture.CreateView(),
    };
  case wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal:
    Log.info("Surface texture is suboptimal, deferring swapchain reconfiguration");
    request_surface_reconfigure();
    return AcquiredSurfaceTexture{
        .texture = surfaceTexture.texture,
        .view = surfaceTexture.texture.CreateView(),
    };
  case wgpu::SurfaceGetCurrentTextureStatus::Timeout:
    Log.warn("Surface texture acquisition timed out");
    return std::nullopt;
  case wgpu::SurfaceGetCurrentTextureStatus::Outdated:
    Log.info("Surface texture is {}, reconfiguring swapchain", magic_enum::enum_name(surfaceTexture.status));
    request_surface_reconfigure();
    return std::nullopt;
  case wgpu::SurfaceGetCurrentTextureStatus::Lost:
    Log.warn("Surface texture is {}, requesting surface recreation", magic_enum::enum_name(surfaceTexture.status));
    request_surface_recreate();
    return std::nullopt;
  case wgpu::SurfaceGetCurrentTextureStatus::Error:
    Log.warn("Surface texture is {}, deferring surface recovery", magic_enum::enum_name(surfaceTexture.status));
    request_surface_reconfigure();
    return std::nullopt;
  default:
    Log.error("Failed to get surface texture: {}", magic_enum::enum_name(surfaceTexture.status));
    return std::nullopt;
  }
}

struct PresentationImage {
  webgpu::TextureWithSampler texture;
  wgpu::BindGroup bindGroup;
};

struct PresentationJob {
  std::shared_ptr<PresentationImage> image;
  uint32_t logicalFrame = 0;
  // Absolute deadline on PresentClock stamped by the producer's schedule.
  // Default (epoch) means "present as soon as ready" (no software pacing).
  PresentClock::time_point presentAt{};
  bool interpolated = false;
  // Slot carries a copy of the native image rather than a replayed interpolation. It counts as a
  // present but not toward effectiveFramesPerSecond.
  bool duplicated = false;
  // Whole display periods the group slid forward at encode time (late group).
  uint32_t slidPeriods = 0;
};

std::array<std::vector<std::shared_ptr<PresentationImage>>, gx::MaxInterpolatedFrames + 1> g_presentationImagePools;

std::shared_ptr<PresentationImage> acquire_presentation_image(size_t slot, uint32_t width, uint32_t height) {
  auto& pool = g_presentationImagePools.at(slot);
  // A use count of one means only the pool holds the image, so no job can be reading it. Idle
  // images from an older surface size are dropped here instead of leaking for the run.
  for (auto it = pool.begin(); it != pool.end();) {
    const auto& image = *it;
    if (image->texture.size.width == width && image->texture.size.height == height) {
      if (image.use_count() == 1) {
        return image;
      }
      ++it;
      continue;
    }
    if (image.use_count() == 1) {
      it = pool.erase(it);
    } else {
      ++it;
    }
  }

  auto image = std::make_shared<PresentationImage>();
  image->texture = webgpu::create_render_texture(width, height, false);
  image->bindGroup = webgpu::create_copy_bind_group(image->texture);
  pool.emplace_back(image);
  return image;
}

bool present_presentation_job(const PresentationJob& job) {
  ZoneScoped;
  const auto submissionStarted = PresentClock::now();
  // Keep the threshold far above compositor and scheduling jitter. The timings below separate a
  // real surface stall from a bad deadline, and only the former needs a rebuild.
  constexpr auto kSurfaceStallThreshold = std::chrono::milliseconds(250);
  std::chrono::nanoseconds surfaceLockDuration{};
  std::chrono::nanoseconds acquireDuration{};
  std::chrono::nanoseconds encodeDuration{};
  std::chrono::nanoseconds finishDuration{};
  std::chrono::nanoseconds submitDuration{};
  std::chrono::nanoseconds scheduleWaitDuration{};
  std::chrono::nanoseconds presentDuration{};
  bool presented = false;
  if (g_surfaceReconfigurePending.load(std::memory_order_acquire) || window::native_resize_pending() ||
      !window::is_presentable()) {
    return false;
  }
  std::chrono::nanoseconds lateBy{};
  if (job.presentAt != PresentClock::time_point{}) {
    // How expired the deadline already is at dequeue. Positive values mean the
    // slot cannot be paced and fires immediately, which is a burst symptom.
    lateBy = std::chrono::duration_cast<std::chrono::nanoseconds>(PresentClock::now() - job.presentAt);
  }
  {
    window::SurfaceLock surfaceLock;
    // Acquire, encode, submit and present are one unit against a configured swapchain, so the
    // surface lock covers all of them. The renderer mutex is deliberately not taken.
    const auto surfaceLockStarted = PresentClock::now();
    std::unique_lock surfaceOwnership(g_surfaceMutex);
    surfaceLockDuration =
        std::chrono::duration_cast<std::chrono::nanoseconds>(PresentClock::now() - surfaceLockStarted);
    // Surface contention is waiting, not encoding, so keep it out of the encode timing where a
    // reconfigure would look like GPU command recording.
    const auto workStarted = PresentClock::now();
    bool surfaceSizeChanged = window::native_resize_pending();
    if (!surfaceSizeChanged && !g_surfaceReconfigurePending.load(std::memory_order_acquire) &&
        window::is_presentable() && g_surface) {
      // native_window_size_matches compares the OS client size with the configured swapchain, which is
      // what native_fb_* reports. One window query instead of SDL's per-call ones.
      surfaceSizeChanged = !window::native_window_size_matches(webgpu::g_graphicsConfig.surfaceConfiguration.width,
                                                               webgpu::g_graphicsConfig.surfaceConfiguration.height);
    }
    if (!surfaceSizeChanged && window::is_presentable()) {
      const auto acquireStarted = PresentClock::now();
      auto acquired = acquire_surface_texture();
      acquireDuration = std::chrono::duration_cast<std::chrono::nanoseconds>(PresentClock::now() - acquireStarted);
      if (acquired) {
        const wgpu::CommandEncoderDescriptor encoderDescriptor{
            .label = "Presentation encoder",
        };
        const auto encoder = g_device.CreateCommandEncoder(&encoderDescriptor);
        const std::array attachments{
            wgpu::RenderPassColorAttachment{
                .view = acquired->view,
                .loadOp = wgpu::LoadOp::Clear,
                .storeOp = wgpu::StoreOp::Store,
            },
        };
        const wgpu::RenderPassDescriptor renderPassDescriptor{
            .label = "Presentation copy pass",
            .colorAttachmentCount = attachments.size(),
            .colorAttachments = attachments.data(),
        };
        const auto pass = encoder.BeginRenderPass(&renderPassDescriptor);
        pass.SetPipeline(webgpu::g_CopyPipeline);
        pass.SetBindGroup(0, job.image->bindGroup, 0, nullptr);
        pass.SetViewport(0.f, 0.f, static_cast<float>(webgpu::g_graphicsConfig.surfaceConfiguration.width),
                         static_cast<float>(webgpu::g_graphicsConfig.surfaceConfiguration.height), 0.f, 1.f);
        pass.Draw(3);
        pass.End();
        const auto encodeFinished = PresentClock::now();
        encodeDuration =
            std::chrono::duration_cast<std::chrono::nanoseconds>(encodeFinished - workStarted - acquireDuration);
        const wgpu::CommandBufferDescriptor cmdBufDescriptor{
            .label = "Presentation command buffer",
        };
        const auto finishStarted = PresentClock::now();
        const auto buffer = encoder.Finish(&cmdBufDescriptor);
        finishDuration = std::chrono::duration_cast<std::chrono::nanoseconds>(PresentClock::now() - finishStarted);
        const auto submitStarted = PresentClock::now();
        {
          std::lock_guard submitLock(g_queueSubmitMutex);
          g_queue.Submit(1, &buffer);
        }
        submitDuration = std::chrono::duration_cast<std::chrono::nanoseconds>(PresentClock::now() - submitStarted);
        // Pace the Present() call itself, not the whole unit, so acquire/encode/submit variance stays out
        // of the cadence. Holding the image across the wait is safe while the surface lock is held.
        if (job.presentAt != PresentClock::time_point{}) {
          const auto scheduleWaitStarted = PresentClock::now();
          wait_until_precise(job.presentAt);
          scheduleWaitDuration =
              std::chrono::duration_cast<std::chrono::nanoseconds>(PresentClock::now() - scheduleWaitStarted);
        }
        // A native resize can arrive after acquisition, so drop the obsolete image and let the render
        // worker reconfigure at its ordered frame boundary.
        if (!g_surfaceReconfigurePending.load(std::memory_order_acquire) && !window::native_resize_pending() &&
            window::is_presentable() &&
            window::native_window_size_matches(webgpu::g_graphicsConfig.surfaceConfiguration.width,
                                               webgpu::g_graphicsConfig.surfaceConfiguration.height)) {
          const auto presentStarted = PresentClock::now();
          wgpu::Status presentStatus;
          {
            std::lock_guard submitLock(g_queueSubmitMutex);
            presentStatus = g_surface.Present();
          }
          presentDuration = std::chrono::duration_cast<std::chrono::nanoseconds>(PresentClock::now() - presentStarted);
          if (presentStatus == wgpu::Status::Success) {
            presented = true;
            record_successful_present(
                job.interpolated, job.logicalFrame, acquireDuration, encodeDuration, finishDuration, submitDuration,
                presentDuration,
                std::chrono::duration_cast<std::chrono::nanoseconds>(PresentClock::now() - submissionStarted),
                job.duplicated, job.slidPeriods, lateBy);
          } else {
            Log.warn("Surface present failed: {}", static_cast<int>(presentStatus));
            request_surface_reconfigure();
          }
        }
        acquired.reset();
      }
    }
  }
  const auto totalDuration =
      std::chrono::duration_cast<std::chrono::nanoseconds>(PresentClock::now() - submissionStarted);
  constexpr int kStallRebuildThreshold = 3;
  constexpr auto kStallRebuildCooldown = std::chrono::seconds(5);
  static int s_consecutiveStalledPresents = 0;
  static PresentClock::time_point s_lastStallRebuild{};
  if (totalDuration >= kSurfaceStallThreshold) {
    const auto surfaceWorkDuration =
        acquireDuration + encodeDuration + finishDuration + submitDuration + presentDuration;
    const bool surfaceStalled = surfaceWorkDuration >= kSurfaceStallThreshold;
    bool rebuildRequested = false;
    if (surfaceStalled) {
      ++s_consecutiveStalledPresents;
      const auto now = PresentClock::now();
      if (s_consecutiveStalledPresents >= kStallRebuildThreshold &&
          (s_lastStallRebuild == PresentClock::time_point{} || now - s_lastStallRebuild >= kStallRebuildCooldown)) {
        s_lastStallRebuild = now;
        s_consecutiveStalledPresents = 0;
        rebuildRequested = true;
      }
    } else {
      s_consecutiveStalledPresents = 0;
    }
    Log.warn(
        "Presentation job took {:.1f} ms (surface lock {:.1f}, acquire {:.1f}, encode {:.1f}, "
        "finish {:.1f}, submit {:.1f}, schedule wait {:.1f}, present {:.1f}){}",
        std::chrono::duration<double, std::milli>(totalDuration).count(),
        std::chrono::duration<double, std::milli>(surfaceLockDuration).count(),
        std::chrono::duration<double, std::milli>(acquireDuration).count(),
        std::chrono::duration<double, std::milli>(encodeDuration).count(),
        std::chrono::duration<double, std::milli>(finishDuration).count(),
        std::chrono::duration<double, std::milli>(submitDuration).count(),
        std::chrono::duration<double, std::milli>(scheduleWaitDuration).count(),
        std::chrono::duration<double, std::milli>(presentDuration).count(),
        rebuildRequested ? "; rebuilding the surface" : "");
    if (rebuildRequested) {
      request_surface_reconfigure();
    }
  } else {
    s_consecutiveStalledPresents = 0;
  }
  return presented;
}

struct PresenterState {
  std::mutex mutex;
  std::condition_variable cv;
  std::thread thread;
  std::deque<PresentationJob> jobs;
  bool started = false;
  bool stop = false;
  bool presenting = false;
};

PresenterState g_presenter;
std::atomic<bool> g_presenterStarted{false};

void presenter_main() noexcept {
  if (!SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_HIGH)) {
    Log.warn("Could not raise the asynchronous presenter thread priority: {}", SDL_GetError());
  }
  for (;;) {
    PresentationJob job;
    {
      std::unique_lock lock(g_presenter.mutex);
      g_presenter.cv.wait(lock, [] { return g_presenter.stop || !g_presenter.jobs.empty(); });
      if (g_presenter.stop && g_presenter.jobs.empty()) {
        break;
      }
      job = std::move(g_presenter.jobs.front());
      g_presenter.jobs.pop_front();
      g_presenter.presenting = true;
    }
    g_presenter.cv.notify_all();

    present_presentation_job(job);

    {
      std::lock_guard lock(g_presenter.mutex);
      g_presenter.presenting = false;
    }
    g_presenter.cv.notify_all();
  }
}

void ensure_presenter_started() {
  std::lock_guard lock(g_presenter.mutex);
  if (g_presenter.started) {
    return;
  }
  g_presenter.stop = false;
  g_presenter.presenting = false;
  g_presenter.thread = std::thread(presenter_main);
  g_presenter.started = true;
  g_presenterStarted.store(true, std::memory_order_release);
  Log.info("Enabled bounded asynchronous presentation worker");
}

void wait_for_presenter_idle() noexcept {
  if (!g_presenterStarted.load(std::memory_order_acquire)) {
    return;
  }
  std::unique_lock lock(g_presenter.mutex);
  g_presenter.cv.wait(lock, [] { return g_presenter.jobs.empty() && !g_presenter.presenting; });
}

void enqueue_presentations(std::vector<PresentationJob>&& jobs) {
  ensure_presenter_started();
  // Scale the bound with the group being handed over: a 240 Hz frame enqueues four jobs at once,
  // and a bound sized for two groups blocked the frame worker on a draining one.
  const size_t maximumQueuedJobs =
      (std::max)(static_cast<size_t>(2 * (gx::MaxInterpolatedFrames + 1)), 3 * jobs.size());
  std::unique_lock lock(g_presenter.mutex);
  if (g_presenter.stop) {
    return;
  }
  if (g_presenter.jobs.size() + jobs.size() > maximumQueuedJobs) {
    // Presentation is a real-time stream, not a lossless queue: waiting for room couples the guest
    // and audio clocks to a blocked Present(). Keep the newest group and drop obsolete images.
    const size_t dropped = g_presenter.jobs.size();
    g_presenter.jobs.clear();
    if (dropped != 0) {
      // A stuck Present can keep replacement mode active for a while.  Aggregate
      // the warning instead of turning a driver stall into a log flood.
      static size_t droppedSinceWarning = 0;
      static PresentClock::time_point lastWarning{};
      droppedSinceWarning += dropped;
      const auto now = PresentClock::now();
      if (lastWarning == PresentClock::time_point{} || now - lastWarning >= std::chrono::seconds(1)) {
        Log.warn("Presenter fell behind; dropped {} stale presentation jobs", droppedSinceWarning);
        droppedSinceWarning = 0;
        lastWarning = now;
      }
    }
  }
  for (auto& job : jobs) {
    g_presenter.jobs.emplace_back(std::move(job));
  }
  lock.unlock();
  g_presenter.cv.notify_all();
}

void stop_presenter() noexcept {
  if (!g_presenterStarted.load(std::memory_order_acquire)) {
    return;
  }
  {
    std::lock_guard lock(g_presenter.mutex);
    g_presenter.stop = true;
  }
  g_presenter.cv.notify_all();
  if (g_presenter.thread.joinable()) {
    g_presenter.thread.join();
  }
  {
    std::lock_guard lock(g_presenter.mutex);
    g_presenter.jobs.clear();
    g_presenter.started = false;
    g_presenter.presenting = false;
  }
  g_presenterStarted.store(false, std::memory_order_release);
}

// `presentSource` is latched in the seal prologue: by the time this encodes, the producer's next
// gfx::begin_frame() may already have cleared the display-copy override.
void encode_presentation_snapshot(const wgpu::CommandEncoder& encoder, const webgpu::PresentSource& presentSource,
                                  const PresentationImage& image, bool includeImGui) {
  ZoneScoped;
  auto viewport = webgpu::calculate_present_viewport(image.texture.size.width, image.texture.size.height,
                                                     presentSource.size.width, presentSource.size.height);
  float presentAspect = 0.f;
  if (window::get_present_aspect_ratio(presentAspect)) {
    viewport = webgpu::calculate_present_viewport_for_aspect(image.texture.size.width, image.texture.size.height,
                                                             presentAspect);
  }
  wgpu::BindGroup presentBindGroup = presentSource.bindGroup;
  {
    const std::array attachments{
        wgpu::RenderPassColorAttachment{
            .view = image.texture.view,
            .loadOp = wgpu::LoadOp::Clear,
            .storeOp = wgpu::StoreOp::Store,
        },
    };
    const wgpu::RenderPassDescriptor renderPassDescriptor{
        .label = "Interpolation snapshot pass",
        .colorAttachmentCount = attachments.size(),
        .colorAttachments = attachments.data(),
    };
    const auto pass = encoder.BeginRenderPass(&renderPassDescriptor);
    pass.SetPipeline(webgpu::g_CopyPipeline);
    pass.SetBindGroup(0, presentBindGroup, 0, nullptr);
    pass.SetViewport(viewport.left, viewport.top, viewport.width, viewport.height, viewport.znear, viewport.zfar);
    pass.Draw(3);
    pass.End();
  }
  if (includeImGui) {
    const std::array attachments{
        wgpu::RenderPassColorAttachment{
            .view = image.texture.view,
            .loadOp = wgpu::LoadOp::Load,
            .storeOp = wgpu::StoreOp::Store,
        },
    };
    const wgpu::RenderPassDescriptor renderPassDescriptor{
        .label = "Snapshot ImGui pass",
        .colorAttachmentCount = attachments.size(),
        .colorAttachments = attachments.data(),
    };
    const auto pass = encoder.BeginRenderPass(&renderPassDescriptor);
    pass.SetViewport(0.f, 0.f, static_cast<float>(image.texture.size.width),
                     static_cast<float>(image.texture.size.height), 0.f, 1.f);
    imgui::render(pass);
    pass.End();
  }
}
#endif

void shutdown() noexcept {
  stop_frame_worker();
#ifdef AURORA_ENABLE_GX
  stop_presenter();
  g_stereoEyeTargets = {};
  g_stereoUpscaleBindings={};g_stereoUpscaled={};g_stereoOutputSizes={};
  g_presentationImagePools = {};
  imgui::shutdown();
  gfx::shutdown();
  webgpu::shutdown();
#endif
  {
    std::lock_guard lock(g_stereoRegistrationMutex);
    g_stereoProvider = {};
    g_stereoProviderActive.store(false, std::memory_order_release);
#ifdef AURORA_ENABLE_GX
    g_stereoSink = {};
#endif
  }
  input::shutdown();
  window::shutdown();
}

const AuroraEvent* update() noexcept {
  ZoneScoped;
  if (g_initialFrame) {
    g_initialFrame = false;
    input::initialize();
  }
  return window::poll_events();
}

bool begin_frame_impl(bool pumpEvents, ImGuiFramePolicy imguiPolicy, bool* imguiNewFrameOwed) noexcept {
  ZoneScoped;
#ifdef AURORA_ENABLE_GX
  webgpu::fail_if_device_lost();
  if (pumpEvents) {
    window::pump_events();
  }
  const bool surfaceReconfigurePending = g_surfaceReconfigurePending.load(std::memory_order_acquire);
  const bool surfaceMutationRequired =
      surfaceReconfigurePending || !window::is_presentable() || !g_surface || window::native_resize_pending() ||
      !window::native_window_size_matches(webgpu::g_graphicsConfig.surfaceConfiguration.width,
                                          webgpu::g_graphicsConfig.surfaceConfiguration.height);
  if (surfaceMutationRequired) {
    wait_for_presenter_idle();
    window::SurfaceLock surfaceLock;
    // Reconfiguring invalidates any image the presenter holds, so take the surface exclusively; a
    // drained queue does not stop a later job. Order is surface before renderer everywhere.
    std::lock_guard surfaceOwnership(g_surfaceMutex);
    std::lock_guard gpuLock(g_rendererGpuMutex);
    // Surface configuration belongs to the render thread; the SDL thread must not reconfigure Dawn
    // while this worker is acquiring or presenting.
    if (!window::is_presentable()) {
      webgpu::release_surface();
      return false;
    }
    if (window::is_paused()) {
      return false;
    }
    const bool consumeSurfaceReconfigure = g_surfaceReconfigurePending.exchange(false, std::memory_order_acq_rel);
    const bool consumeSurfaceRecreate = g_surfaceRecreatePending.exchange(false, std::memory_order_acq_rel);
    if (!g_surface || consumeSurfaceReconfigure) {
      // Reconfigure in place unless the surface was actually lost. See
      // g_surfaceRecreatePending for why destroying a live surface here is fatal
      // while a capture overlay is attached.
      webgpu::refresh_surface(consumeSurfaceRecreate);
      if (!g_surface) {
        return false;
      }
    }
    if (window::native_resize_pending() ||
        !window::native_window_size_matches(webgpu::g_graphicsConfig.surfaceConfiguration.width,
                                            webgpu::g_graphicsConfig.surfaceConfiguration.height)) {
      window::sync_frame_buffer_size();
    }
  } else if (window::is_paused()) {
    return false;
  }

  return begin_frame_render_state_impl(imguiPolicy, imguiNewFrameOwed);
#else
  (void)imguiPolicy;
  (void)imguiNewFrameOwed;
  return true;
#endif
}

bool begin_frame_render_state_impl(ImGuiFramePolicy imguiPolicy, bool* imguiNewFrameOwed) noexcept {
#ifdef AURORA_ENABLE_GX
  std::lock_guard gpuLock(g_rendererGpuMutex);
  // Note the debt before gfx::begin_frame() can fail: the synchronous path always started the
  // ImGui frame here, and the runtime's retry loop depends on that pairing.
  if (imguiPolicy == ImGuiFramePolicy::Immediate) {
    imgui::new_frame(window::get_window_size());
  } else if (imguiNewFrameOwed != nullptr) {
    *imguiNewFrameOwed = true;
  }
  if (!gfx::begin_frame()) {
    return false;
  }
#else
  (void)imguiPolicy;
  (void)imguiNewFrameOwed;
#endif
  return true;
}

#ifdef AURORA_ENABLE_GX
// Everything the mutex-free encode phase needs, latched while the renderer GPU mutex is held.
// None of it may be re-read from a global later; the producer has already begun the next frame.
struct SealedFrameContext {
  AuroraStereoFrame uiFrame{};
  AuroraVRUiGuide uiGuide{};
  double sealMs = 0, monoMs = 0, eyesMs = 0, bridgeMs = 0, submitMs = 0, prepareMs = 0, encodeMs = 0;
  wgpu::CommandEncoder encoder; // slot 0's encoder; already holds the staging copies
  webgpu::PresentSource presentSource{};
  std::optional<gfx::StereoReplayFrame> stereoReplay;
  uint64_t stereoFrameToken = 0;
  AuroraStereoFrameMode stereoFrameMode = AURORA_STEREO_FRAME_IMMERSIVE_REPLAY;
  bool immersiveStereoPrepared = false;
  uint64_t scheduleBaseNanos = 0;
  uint64_t scheduleIntervalNanos = 0;
  uint32_t interpolatedFrameCount = 0;
  uint32_t snapshotWidth = 1;
  uint32_t snapshotHeight = 1;
  uint32_t logicalFrame = 0;
  bool interpolationActive = false;
  bool replayInterpolatedFrames = false;
};

// Phase 1: everything that touches producer-shared renderer state. Needs g_rendererGpuMutex and
// a FIFO already drained into the recorded pass list.
void seal_frame_locked(gfx::SealedFrame& sealedFrame, SealedFrameContext& ctx, uint64_t contentTag,
                       const StereoSceneAnchor& sceneAnchor) {
  ZoneScopedN("Seal frame");
  const auto encoderDescriptor = wgpu::CommandEncoderDescriptor{
      .label = "Redraw encoder",
  };
  ctx.encoder = g_device.CreateCommandEncoder(&encoderDescriptor);
  // Probe-sized CPU-consumed copies read back asynchronously. Their downscale blits push uniforms,
  // so prepare them while the producer's staging buffers are still mapped.
  gfx::efb_ram::seal_async_downloads();
  // current_frame() advances inside gfx::end_frame; unsigned wrap maps the
  // pre-first-frame UINT32_MAX value to logical frame zero.
  ctx.logicalFrame = gfx::current_frame() + 1;
  if (const auto stereoInput = request_stereo_frame(ctx.logicalFrame, contentTag)) {
    ctx.uiFrame=*stereoInput;
    { std::lock_guard lock(vrui::mutex);ctx.uiGuide=vrui::guide; }
    ctx.stereoFrameToken = stereoInput->frameToken;
    ctx.stereoFrameMode = stereoInput->mode;
    ctx.stereoReplay = make_stereo_replay_frame(*stereoInput, sceneAnchor);
  }
  if (ctx.stereoReplay && ctx.stereoFrameMode == AURORA_STEREO_FRAME_IMMERSIVE_REPLAY) {
    // Keep the accepted frame alive even if the additional eye-uniform copies
    // do not fit. The encode phase will duplicate the completed mono image to
    // both eyes, allowing the sink to release/end the already-acquired XR
    // frame instead of orphaning it indefinitely.
    ctx.immersiveStereoPrepared = gfx::end_frame(ctx.encoder, *ctx.stereoReplay);
    static bool immersiveReplaySuccessLogged = false;
    static bool immersiveReplayFallbackLogged = false;
    if (ctx.immersiveStereoPrepared && !immersiveReplaySuccessLogged) {
      immersiveReplaySuccessLogged = true;
      Log.info("Immersive stereo GX replay prepared successfully for both eyes");
    } else if (!ctx.immersiveStereoPrepared && !immersiveReplayFallbackLogged) {
      immersiveReplayFallbackLogged = true;
      Log.warn("Immersive stereo GX replay preparation failed; duplicating the mono image into the projection layer");
    }
  } else {
    gfx::end_frame(ctx.encoder);
  }
  gfx::g_stats.presentedFrameCount = 0;
  gfx::g_stats.interpolatedFrameCount = 0;
  // Latched before the producer's next gfx::begin_frame() calls
  // gx::begin_frame_interpolation(), which resets both of these.
  ctx.interpolatedFrameCount = gx::interpolated_frame_count();
  ctx.interpolationActive = ctx.interpolatedFrameCount != 0;
  ctx.replayInterpolatedFrames = ctx.interpolationActive && gx::frame_interpolation_replay_safe();
  ctx.scheduleBaseNanos = g_presentScheduleBaseNanos.load(std::memory_order_acquire);
  ctx.scheduleIntervalNanos = g_presentScheduleIntervalNanos.load(std::memory_order_acquire);
  const auto windowSize = window::get_window_size();
  ctx.snapshotWidth = (std::max)(windowSize.native_fb_width, 1u);
  ctx.snapshotHeight = (std::max)(windowSize.native_fb_height, 1u);
  ctx.logicalFrame = gfx::current_frame();
  // Latched before webgpu::clear_present_source_override() in the producer's
  // next gfx::begin_frame().
  ctx.presentSource = webgpu::current_present_source();
  // ImGui draw lists are built once per frame and replayed by each slot's ImGui pass, which is why
  // the next ImGui frame cannot start until the encode phase is done.
  imgui::render_frame_data();
  // Drop the sealed frame's lazy RAM-readback requests while the producer is still excluded; it
  // starts registering the next frame's as soon as SEALED is published.
  gfx::efb_ram::cancel();
  // Detach the recorded passes. From here the producer's list is empty and the
  // encode phase reads only worker-private state.
  gfx::seal_frame(sealedFrame);
  gfx::expire_bind_group_cache();
}

// Phase 2: encode every presentation slot. Reads only `ctx` and the sealed passes, so it runs
// without the renderer GPU mutex while the producer records the next frame.
std::vector<PresentationJob> encode_sealed_frame(gfx::SealedFrame& sealedFrame, SealedFrameContext& ctx) {
  ZoneScopedN("Encode sealed frame");
  const auto encodeStart = PresentClock::now();
  const auto elapsedMs = [](auto start) { return std::chrono::duration<double, std::milli>(PresentClock::now() - start).count(); };
  auto encoder = std::move(ctx.encoder);
  const auto encoderDescriptor = wgpu::CommandEncoderDescriptor{
      .label = "Redraw encoder",
  };
  // Absolute slot deadlines: slot k of jobCount presents at base + k * interval / jobCount. With
  // interpolation off, pace to the boundary that just passed plus 6.5 ms; late frames free-run.
  constexpr uint64_t kNativePresentOffsetNanos = 6'500'000;
  const uint32_t presentationJobCount = ctx.interpolatedFrameCount + 1;
  const auto slotPresentDeadline = [&](uint32_t slot) -> PresentClock::time_point {
    if (ctx.scheduleBaseNanos == 0 || ctx.scheduleIntervalNanos == 0) {
      return {};
    }
    if (!ctx.interpolationActive) {
      return PresentClock::time_point{
          std::chrono::nanoseconds{ctx.scheduleBaseNanos - ctx.scheduleIntervalNanos + kNativePresentOffsetNanos}};
    }
    const uint64_t offsetNanos = (ctx.scheduleIntervalNanos * static_cast<uint64_t>(slot)) / presentationJobCount;
    return PresentClock::time_point{std::chrono::nanoseconds{ctx.scheduleBaseNanos + offsetNanos}};
  };
  std::vector<PresentationJob> presentationJobs;
  presentationJobs.reserve(presentationJobCount);
  std::optional<PendingStereoSink> pendingStereoSink;

  // Each slot is submitted as soon as it is encoded, so the GPU starts slot 0 while slot 1 is still
  // recording. Queue order preserves the ordering the single batched buffer gave.
  const wgpu::CommandBufferDescriptor cmdBufDescriptor{
      .label = "Presentation slot command buffer",
  };
  const auto submitEncodedSlot = [&](wgpu::CommandEncoder& target,
                                     const PendingStereoSink* stereoSubmission = nullptr) {
    const auto submitStart = PresentClock::now();
    const auto buffer = target.Finish(&cmdBufDescriptor);
    {
      std::lock_guard submitLock(g_queueSubmitMutex);
      g_queue.Submit(1, &buffer);
      // The native backend may enqueue follow-up work on Dawn's underlying
      // graphics queue. Keep it adjacent to this submission so presenter or
      // EFB work cannot interleave between the WebGPU copy and that handoff.
      if (stereoSubmission != nullptr && stereoSubmission->submitted != nullptr) {
        stereoSubmission->submitted(stereoSubmission->frame, stereoSubmission->userdata);
      }
    }
    ctx.submitMs += elapsedMs(submitStart);
  };

  if (ctx.replayInterpolatedFrames) {
    for (uint32_t interpolatedFrame = 0; interpolatedFrame < ctx.interpolatedFrameCount; ++interpolatedFrame) {
      gfx::render(sealedFrame, encoder, static_cast<int32_t>(interpolatedFrame), false);
      auto image = acquire_presentation_image(interpolatedFrame, ctx.snapshotWidth, ctx.snapshotHeight);
      encode_presentation_snapshot(encoder, ctx.presentSource, *image, true);
      presentationJobs.push_back({
          .image = std::move(image),
          .logicalFrame = ctx.logicalFrame,
          .presentAt = slotPresentDeadline(interpolatedFrame),
          .interpolated = true,
      });
      submitEncodedSlot(encoder);
      encoder = g_device.CreateCommandEncoder(&encoderDescriptor);
    }
  }

  // A demanded CPU-visible EFB readback submits a prefix of the frame, so replaying the resumed
  // stream would mutate an already-rendered EFB. Render once, then duplicate into the slots.
  const bool stereoOutput = ctx.stereoReplay.has_value();
  const bool immersiveReplay = stereoOutput && ctx.immersiveStereoPrepared;
  const auto monoStart = PresentClock::now();
  gfx::render(sealedFrame, encoder, -1, !immersiveReplay);
  ctx.monoMs = elapsedMs(monoStart);
  // The copy targets now hold this frame's resolves, so queue their readbacks on the same encoder;
  // completion is harvested in gfx::after_submit, never waited on here.
  gfx::efb_ram::encode_async_downloads(encoder);
  if (!ctx.replayInterpolatedFrames) {
    for (uint32_t interpolatedFrame = 0; interpolatedFrame < ctx.interpolatedFrameCount; ++interpolatedFrame) {
      auto image = acquire_presentation_image(interpolatedFrame, ctx.snapshotWidth, ctx.snapshotHeight);
      encode_presentation_snapshot(encoder, ctx.presentSource, *image, true);
      presentationJobs.push_back({
          .image = std::move(image),
          .logicalFrame = ctx.logicalFrame,
          .presentAt = slotPresentDeadline(interpolatedFrame),
          .interpolated = true,
          .duplicated = true,
      });
      submitEncodedSlot(encoder);
      encoder = g_device.CreateCommandEncoder(&encoderDescriptor);
    }
  }
  auto finalImage = acquire_presentation_image(ctx.interpolatedFrameCount, ctx.snapshotWidth, ctx.snapshotHeight);
  encode_presentation_snapshot(encoder, ctx.presentSource, *finalImage, true);

  // Exercise the actual VR environment pipeline without requiring an HMD.
  // Paired with MKW_VR_INTRO_PREVIEW, this also enables GPU validation locally.
  if (std::getenv("MKW_VR_ENV_PREVIEW") && std::getenv("MKW_VR_INTRO_PREVIEW")) {
    if(const char* quality=std::getenv("MKW_VR_QUALITY_TEST"))
      vrui::requestedQuality.store(std::clamp(std::atoi(quality),0,3),std::memory_order_relaxed);
    AuroraStereoFrame preview{};
    preview.ui.anchored=true;preview.ui.width=2.4f;preview.ui.distance=2.0f;
    const webgpu::PresentSource mono{.bindGroup=finalImage->bindGroup,.texture=finalImage->texture.texture,
      .size=finalImage->texture.size,.format=finalImage->texture.format};
    for(uint32_t eye=0;eye<2;++eye) {
      ensure_stereo_eye_target(eye,ctx.snapshotWidth,ctx.snapshotHeight);
      preview.ui.eyeFromPanel[eye][0]=preview.ui.eyeFromPanel[eye][5]=preview.ui.eyeFromPanel[eye][10]=1;
      preview.eyes[eye].projection[0]=0.8f;preview.eyes[eye].projection[5]=1.3f;
      vrui::render(encoder,mono,preview,{},eye);
    }
  }

  // Keep both eye replays and the sink copy in the final submission. The
  // duplicate-slot path above may submit and rotate the encoder several
  // times, so encoding stereo before it would pair the post-submit callback
  // with the wrong command buffer.
  const auto eyesStart = PresentClock::now();
  if (immersiveReplay) {
    for (uint32_t eye = 0; eye < AURORA_STEREO_EYE_COUNT; ++eye) {
      gfx::render_stereo_eye(sealedFrame, encoder, *ctx.stereoReplay, eye, eye + 1 == AURORA_STEREO_EYE_COUNT);
    }
  } else if (stereoOutput) {
    // Use the completed mono snapshot so virtual-screen XR includes ImGui at
    // the same scale and aspect as the desktop presentation. Rendering the
    // same ImGui draw data directly into differently-sized eye textures would
    // make the backend restore the desktop-sized viewport.
    const webgpu::PresentSource completedMono{
        .bindGroup = finalImage->bindGroup,
        .texture = finalImage->texture.texture,
        .size = finalImage->texture.size,
        .format = finalImage->texture.format,
    };
    for (uint32_t eye = 0; eye < AURORA_STEREO_EYE_COUNT; ++eye) {
      if(ctx.uiFrame.ui.anchored) vrui::render(encoder,completedMono,ctx.uiFrame,ctx.uiGuide,eye);
      else encode_virtual_screen_eye(encoder, completedMono, eye);
    }
  }
  ctx.eyesMs = elapsedMs(eyesStart);
  const auto bridgeStart = PresentClock::now();
  if (stereoOutput) {
    pendingStereoSink = run_stereo_sink(encoder, ctx.stereoFrameToken, ctx.logicalFrame, ctx.stereoFrameMode);
  }
  ctx.bridgeMs = elapsedMs(bridgeStart);
  // UI previews need the completed presentation, including ImGui. Ordinary
  // renderer regression captures intentionally retain the raw game image.
  webgpu::PresentSource captureSource = std::getenv("MKW_VR_INTRO_PREVIEW") != nullptr
      ? webgpu::PresentSource{.bindGroup = finalImage->bindGroup,
          .texture = finalImage->texture.texture, .size = finalImage->texture.size,
          .format = finalImage->texture.format}
      : ctx.presentSource;
  if(std::getenv("MKW_VR_ENV_PREVIEW") && std::getenv("MKW_VR_INTRO_PREVIEW")) {
    const auto& eye=g_stereoEyeTargets[0].output();
    captureSource={.bindGroup=webgpu::create_copy_bind_group(eye),.texture=eye.texture,.size=eye.size,.format=eye.format};
  }
  auto pendingFrameCapture = encode_frame_capture(encoder, captureSource);
  presentationJobs.push_back({
      .image = std::move(finalImage),
      .logicalFrame = ctx.logicalFrame,
      .presentAt = slotPresentDeadline(ctx.interpolatedFrameCount),
      .interpolated = false,
  });
  submitEncodedSlot(encoder, pendingStereoSink ? &*pendingStereoSink : nullptr);

  // A group that finished encoding past its anchor slides forward by whole display periods, never
  // per slot. The cursor keeps two groups off one anchor, which bursts then holds for a period.
  static PresentClock::time_point s_lastGroupAnchor{};
  if (ctx.interpolationActive && ctx.scheduleIntervalNanos != 0 && !presentationJobs.empty() &&
      presentationJobs.front().presentAt != PresentClock::time_point{}) {
    const std::chrono::nanoseconds interval{static_cast<int64_t>(ctx.scheduleIntervalNanos)};
    auto anchor = presentationJobs.front().presentAt;
    const auto now = PresentClock::now();
    if (now > anchor) {
      const auto behind = std::chrono::duration_cast<std::chrono::nanoseconds>(now - anchor);
      const uint64_t periods = static_cast<uint64_t>(behind.count()) / ctx.scheduleIntervalNanos + 1u;
      anchor += std::chrono::nanoseconds{static_cast<int64_t>(periods * ctx.scheduleIntervalNanos)};
    }
    if (s_lastGroupAnchor != PresentClock::time_point{} && anchor <= s_lastGroupAnchor) {
      anchor = s_lastGroupAnchor + interval;
    }
    const auto shift =
        std::chrono::duration_cast<std::chrono::nanoseconds>(anchor - presentationJobs.front().presentAt);
    if (shift.count() > 0) {
      const uint32_t slidPeriods = static_cast<uint32_t>(
          (static_cast<uint64_t>(shift.count()) + ctx.scheduleIntervalNanos - 1u) / ctx.scheduleIntervalNanos);
      for (auto& job : presentationJobs) {
        job.presentAt += shift;
        job.slidPeriods = slidPeriods;
      }
    }
    s_lastGroupAnchor = anchor;
  } else {
    // No schedule (interpolation off, boot/black presents): the grid is gone,
    // so the cursor must not constrain the next scheduled group.
    s_lastGroupAnchor = {};
  }

  if (pendingFrameCapture.has_value()) {
    complete_frame_capture(*pendingFrameCapture);
  }
  gfx::after_submit();
  gfx::g_stats.presentedFrameCount = static_cast<uint32_t>(presentationJobs.size());
  gfx::g_stats.interpolatedFrameCount = ctx.interpolatedFrameCount;
  ctx.encodeMs = elapsedMs(encodeStart);
  return presentationJobs;
}

// Phase 3: hand the encoded group to whoever owns presentation.
void publish_presentations(std::vector<PresentationJob>&& presentationJobs, bool interpolationActive) {
#if defined(__APPLE__)
  (void)interpolationActive;
  // Presenting reaches SDL/AppKit, whose window operations must stay on the
  // main thread. Interpolation normally starts the presenter worker, so keep
  // its jobs synchronous on Apple platforms.
  for (const auto& job : presentationJobs) {
    present_presentation_job(job);
  }
#else
  // Keep presentation on the presenter whenever the async frame worker runs, even with
  // interpolation off, so every mode shares one surface/resize path. RenderDoc keeps the sync path.
  if (frame_worker_requested() || interpolationActive || g_presenterStarted.load(std::memory_order_acquire)) {
    enqueue_presentations(std::move(presentationJobs));
  } else {
    for (const auto& job : presentationJobs) {
      present_presentation_job(job);
    }
  }
#endif
}

void record_frame_telemetry() {
  TracyPlotConfig("aurora: lastVertSize", tracy::PlotFormatType::Memory, false, true, 0);
  TracyPlotConfig("aurora: lastUniformSize", tracy::PlotFormatType::Memory, false, true, 0);
  TracyPlotConfig("aurora: lastIndexSize", tracy::PlotFormatType::Memory, false, true, 0);
  TracyPlotConfig("aurora: lastStorageSize", tracy::PlotFormatType::Memory, false, true, 0);
  TracyPlotConfig("aurora: lastTextureUploadSize", tracy::PlotFormatType::Memory, false, true, 0);

  TracyPlot("aurora: queuedPipelines", static_cast<int64_t>(gfx::g_stats.queuedPipelines));
  TracyPlot("aurora: createdPipelines", static_cast<int64_t>(gfx::g_stats.createdPipelines));
  TracyPlot("aurora: drawCallCount", static_cast<int64_t>(gfx::g_stats.drawCallCount));
  TracyPlot("aurora: mergedDrawCallCount", static_cast<int64_t>(gfx::g_stats.mergedDrawCallCount));
  TracyPlot("aurora: lastVertSize", static_cast<int64_t>(gfx::g_stats.lastVertSize));
  TracyPlot("aurora: lastUniformSize", static_cast<int64_t>(gfx::g_stats.lastUniformSize));
  TracyPlot("aurora: lastIndexSize", static_cast<int64_t>(gfx::g_stats.lastIndexSize));
  TracyPlot("aurora: lastStorageSize", static_cast<int64_t>(gfx::g_stats.lastStorageSize));
  TracyPlot("aurora: lastTextureUploadSize", static_cast<int64_t>(gfx::g_stats.lastTextureUploadSize));
  TracyPlot("aurora: frameIndex", static_cast<int64_t>(gfx::current_frame()));
#if defined(TRACY_ENABLE) && defined(_WIN32)
  const auto fileTimeValue = [](const FILETIME& value) noexcept {
    ULARGE_INTEGER result{};
    result.LowPart = value.dwLowDateTime;
    result.HighPart = value.dwHighDateTime;
    return result.QuadPart;
  };
  FILETIME creation{}, exit{}, processKernel{}, processUser{}, threadKernel{}, threadUser{};
  const bool processTimesAvailable =
      GetProcessTimes(GetCurrentProcess(), &creation, &exit, &processKernel, &processUser) != FALSE;
  const bool threadTimesAvailable =
      GetThreadTimes(GetCurrentThread(), &creation, &exit, &threadKernel, &threadUser) != FALSE;
  const uint64_t processCpu100ns =
      processTimesAvailable ? fileTimeValue(processKernel) + fileTimeValue(processUser) : 0;
  const uint64_t threadCpu100ns = threadTimesAvailable ? fileTimeValue(threadKernel) + fileTimeValue(threadUser) : 0;
  static uint64_t previousProcessCpu100ns = processCpu100ns;
  static uint64_t previousThreadCpu100ns = threadCpu100ns;
  TracyPlot("aurora: processCpuUsPerFrame", static_cast<int64_t>((processCpu100ns - previousProcessCpu100ns) / 10));
  TracyPlot("aurora: mainThreadCpuUsPerFrame", static_cast<int64_t>((threadCpu100ns - previousThreadCpu100ns) / 10));
  previousProcessCpu100ns = processCpu100ns;
  previousThreadCpu100ns = threadCpu100ns;
#endif
  FrameMarkNamed("Aurora frame");
}

// One complete frame-worker cycle. The scene encode only leaves the renderer mutex when
// interpolation actually inserts slots; otherwise both phases publish together.
bool run_frame_worker_cycle(gfx::SealedFrame& sealedFrame, uint64_t contentTag,
                            const StereoSceneAnchor& sceneAnchor) noexcept {
  ZoneScopedN("Frame worker cycle");
  webgpu::fail_if_device_lost();
  SealedFrameContext ctx;
  std::vector<PresentationJob> presentationJobs;
  bool overlapEncode = false;
  {
    std::lock_guard gpuLock(g_rendererGpuMutex);
    const auto sealStart = PresentClock::now();
    seal_frame_locked(sealedFrame, ctx, contentTag, sceneAnchor);
    ctx.sealMs = std::chrono::duration<double, std::milli>(PresentClock::now() - sealStart).count();
    overlapEncode = ctx.interpolationActive;
    if (!overlapEncode) {
      presentationJobs = encode_sealed_frame(sealedFrame, ctx);
    }
  }
  if (!overlapEncode) {
    publish_presentations(std::move(presentationJobs), ctx.interpolationActive);
  }

  {
    std::unique_lock lock(g_frameWorker.mutex);
    g_frameWorker.cv.wait(lock, [] { return g_frameWorker.stop || g_frameWorker.prepareAllowed; });
    if (g_frameWorker.stop) {
      return false;
    }
    g_frameWorker.prepareAllowed = false;
  }

  // Preparing the next frame belongs to the SEALED phase: without a fresh pass 0 and mapped
  // staging buffers the producer's drain has nowhere to put its commands.
  bool imguiNewFrameOwed = false;
  const auto prepareStart = PresentClock::now();
  const bool prepared = begin_frame_impl(
      false, overlapEncode ? ImGuiFramePolicy::Deferred : ImGuiFramePolicy::Immediate, &imguiNewFrameOwed);
  ctx.prepareMs = std::chrono::duration<double, std::milli>(PresentClock::now() - prepareStart).count();

  {
    std::lock_guard lock(g_frameWorker.mutex);
    g_frameWorker.framePrepared = prepared;
    g_frameWorker.sealed.store(true, std::memory_order_release);
    if (!overlapEncode) {
      g_frameWorker.ready.store(true, std::memory_order_release);
    }
  }
  g_frameWorker.cv.notify_all();

  if (overlapEncode) {
    // Mutex-free: the producer drains and records the next frame in parallel, taking the renderer
    // mutex per drain, and this phase never takes it.
    presentationJobs = encode_sealed_frame(sealedFrame, ctx);
    publish_presentations(std::move(presentationJobs), ctx.interpolationActive);
    if (imguiNewFrameOwed) {
      // Safe only now: every slot has replayed this frame's ImGui draw lists.
      std::lock_guard gpuLock(g_rendererGpuMutex);
      imgui::new_frame(window::get_window_size());
    }
    {
      std::lock_guard lock(g_frameWorker.mutex);
      g_frameWorker.ready.store(true, std::memory_order_release);
    }
    g_frameWorker.cv.notify_all();
  }

  if (ctx.immersiveStereoPrepared) {
    static uint32_t samples = 0;
    static std::array<double, 8> totals{};
    const std::array values{ctx.sealMs, ctx.monoMs, ctx.eyesMs, ctx.bridgeMs,
                            ctx.submitMs, ctx.prepareMs, ctx.encodeMs, double(overlapEncode)};
    for (size_t i = 0; i < totals.size(); ++i) totals[i] += values[i];
    if (++samples >= 120) {
      for (auto& total : totals) total /= samples;
      Log.info("VR render CPU wall ms: seal={:.3f}, mono={:.3f}, eyes={:.3f}, bridge={:.3f}, submit={:.3f}, next-frame={:.3f}, encode-total={:.3f}, overlap={:.1f}",
               totals[0], totals[1], totals[2], totals[3], totals[4], totals[5], totals[6], totals[7]);
      totals = {};
      samples = 0;
    }
  }
  record_frame_telemetry();
  return true;
}
#endif

// Synchronous frame submission: seal, encode and present inline on the calling thread. Used when
// the frame worker is disabled (RenderDoc captures) and on the boot path.
void end_frame_impl(bool pumpEvents, bool drainFifo, uint64_t contentTag,
                    const StereoSceneAnchor& sceneAnchor) noexcept {
  ZoneScoped;
#ifdef AURORA_ENABLE_GX
  webgpu::fail_if_device_lost();
  if (pumpEvents) {
    window::pump_events();
  }
  gfx::SealedFrame sealedFrame;
  SealedFrameContext ctx;
  std::vector<PresentationJob> presentationJobs;
  {
    std::lock_guard gpuLock(g_rendererGpuMutex);
    if (drainFifo) {
      gx::fifo::drain();
    }
    seal_frame_locked(sealedFrame, ctx, contentTag, sceneAnchor);
    presentationJobs = encode_sealed_frame(sealedFrame, ctx);
  }
  publish_presentations(std::move(presentationJobs), ctx.interpolationActive);
  record_frame_telemetry();
#else
  (void)pumpEvents;
  (void)drainFifo;
  (void)contentTag;
  (void)sceneAnchor;
#endif
}

bool begin_frame() noexcept {
#ifdef AURORA_ENABLE_GX
  // The asynchronous fast path below can return a logically prepared frame
  // without entering begin_frame_impl(), so loss must be checked before it.
  webgpu::fail_if_device_lost();
#endif
  if (!frame_worker_requested()) {
    return begin_frame_impl(true);
  }

  ensure_frame_worker_started();
  // SDL needs event pumping on the window-owning producer thread, and the worker passes
  // pumpEvents=false, so keep it here even when the fast path returns early.
  window::pump_events();
  bool waitForSurfacePreparation = false;
#ifdef AURORA_ENABLE_GX
  // A surface mutation can legitimately fail preparation, and optimistic success would let GX/ImGui
  // record into a frame that was never begun, so join this path and return its real result.
  waitForSurfacePreparation = !window::is_presentable() || !g_surface || window::native_resize_pending() ||
                              window::is_paused() ||
                              !window::native_window_size_matches(webgpu::g_graphicsConfig.surfaceConfiguration.width,
                                                                  webgpu::g_graphicsConfig.surfaceConfiguration.height);
#endif
  bool workerPreparationPending = false;
  {
    std::lock_guard lock(g_frameWorker.mutex);
    // The runtime begins right after an asynchronous end, so treat the worker's pending begin as an
    // active frame unless a resize needs the real preparation result.
    if (!g_frameWorker.ready.load(std::memory_order_acquire)) {
      g_frameWorker.prepareAllowed = true;
      g_frameWorker.cv.notify_one();
      if (!waitForSurfacePreparation) {
        return true;
      }
      workerPreparationPending = true;
    } else if (g_frameWorker.framePrepared) {
      return true;
    }
  }

  if (workerPreparationPending) {
    // DONE, not SEALED: this only runs while the window is changing and the caller is about to act on
    // the surface, so keep the resize path fully serialized.
    wait_for_frame_worker_private(FrameWorkerPhase::Done);
    std::lock_guard lock(g_frameWorker.mutex);
    return g_frameWorker.framePrepared;
  }

  {
    std::lock_guard lock(g_frameWorker.mutex);
    if (g_frameWorker.framePrepared) {
      return true;
    }
  }

  const bool prepared = begin_frame_impl(false);
  {
    std::lock_guard lock(g_frameWorker.mutex);
    g_frameWorker.framePrepared = prepared;
  }
  return prepared;
}

void end_frame(uint64_t contentTag) noexcept {
#ifdef AURORA_ENABLE_GX
  webgpu::fail_if_device_lost();
#endif
  // Claim the anchor published for this frame. Clearing it here is what makes a
  // producer that stops publishing fall back to the recorded camera.
  const StereoSceneAnchor sceneAnchor = g_pendingSceneAnchor;
  g_pendingSceneAnchor = {};
  if (!frame_worker_requested()) {
    end_frame_impl(true, true, contentTag, sceneAnchor);
    return;
  }

  ensure_frame_worker_started();
  // DONE: this seals another frame, which means reusing the worker's encoder
  // state and its SealedFrame. The previous cycle must be completely finished.
  wait_for_frame_worker_private(FrameWorkerPhase::Done);

  // Seal all current GX work on the CPU while the renderer is known ready.
  // Later FIFO writes belong exclusively to the next frame.
  {
    std::lock_guard gpuLock(g_rendererGpuMutex);
    gx::fifo::drain();
  }
  {
    std::lock_guard lock(g_frameWorker.mutex);
    g_frameWorker.framePrepared = false;
    g_frameWorker.sealed.store(false, std::memory_order_release);
    g_frameWorker.ready.store(false, std::memory_order_release);
    g_frameWorker.contentTag = contentTag;
    g_frameWorker.sceneAnchor = sceneAnchor;
    g_frameWorker.jobPending = true;
    g_frameWorker.prepareAllowed = false;
  }
  g_frameWorker.cv.notify_one();
}
} // namespace

void wait_for_frame_worker() noexcept { wait_for_frame_worker_private(FrameWorkerPhase::Done); }
std::chrono::nanoseconds wait_for_frame_worker_sealed() noexcept {
  if (g_frameWorker.sealed.load(std::memory_order_acquire)) {
    return std::chrono::nanoseconds::zero();
  }
  const auto started = std::chrono::steady_clock::now();
  wait_for_frame_worker_private(FrameWorkerPhase::Sealed);
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started);
}
bool wait_for_frame_worker_for(std::chrono::microseconds timeout) noexcept {
  return wait_for_frame_worker_private_for(FrameWorkerPhase::Done, timeout);
}
void quiesce_frame_worker() noexcept {
  {
    std::lock_guard lock(g_frameWorker.mutex);
    if (!g_frameWorker.started || g_frameWorker.threadId == std::this_thread::get_id() ||
        g_frameWorker.ready.load(std::memory_order_acquire)) {
      return;
    }
    // This may race the worker just after it passed the prepare wait. In that
    // case the permit is harmless and is cleared after DONE before another job
    // can be queued by this producer thread.
    g_frameWorker.prepareAllowed = true;
  }
  g_frameWorker.cv.notify_all();
  wait_for_frame_worker_private(FrameWorkerPhase::Done);
  {
    std::lock_guard lock(g_frameWorker.mutex);
    g_frameWorker.prepareAllowed = false;
  }
}
std::recursive_mutex& renderer_gpu_mutex() noexcept { return g_rendererGpuMutex; }
bool stereo_frame_provider_active() noexcept { return g_stereoProviderActive.load(std::memory_order_acquire); }

void set_stereo_frame_provider(AuroraStereoFrameProvider provider, void* userdata) noexcept {
  std::lock_guard lock(g_stereoRegistrationMutex);
  g_stereoProvider = {
      .callback = provider,
      .userdata = userdata,
  };
  g_stereoProviderActive.store(provider != nullptr, std::memory_order_release);
}

void set_stereo_scene_anchor(const float anchorFromScene[12]) noexcept {
  if (anchorFromScene == nullptr) {
    g_pendingSceneAnchor = {};
    return;
  }
  // aurora_core is built with -ffast-math, so std::isfinite may be folded to
  // true. Inspect the IEEE-754 exponent, matching request_stereo_frame().
  for (size_t i = 0; i < 12; ++i) {
    uint32_t bits = 0;
    std::memcpy(&bits, &anchorFromScene[i], sizeof(bits));
    if ((bits & 0x7f800000u) == 0x7f800000u) {
      static bool rejectionLogged = false;
      if (!rejectionLogged) {
        rejectionLogged = true;
        Log.warn("Rejected a non-finite stereo scene anchor; keeping the recorded camera");
      }
      g_pendingSceneAnchor = {};
      return;
    }
  }
  StereoSceneAnchor anchor{};
  std::memcpy(anchor.anchorFromScene.data(), anchorFromScene, sizeof(anchor.anchorFromScene));
  anchor.active = true;
  g_pendingSceneAnchor = anchor;
}

#ifdef AURORA_ENABLE_GX
namespace stereo {
void set_sink(SinkCallback callback, SubmitCallback submitted, void* userdata) noexcept {
  std::lock_guard lock(g_stereoRegistrationMutex);
  g_stereoSink = {
      .callback = callback,
      .submitted = submitted,
      .userdata = userdata,
  };
}
} // namespace stereo
#endif
} // namespace aurora

// C API bindings
AuroraInfo aurora_initialize(int argc, char* argv[], const AuroraConfig* config) {
  return aurora::initialize(argc, argv, *config);
}
void aurora_shutdown() { aurora::shutdown(); }
const AuroraEvent* aurora_update() { return aurora::update(); }
bool aurora_begin_frame() { return aurora::begin_frame(); }
void aurora_end_frame() { aurora::end_frame(AURORA_STEREO_CONTENT_TAG_UNKNOWN); }
void aurora_end_frame_tagged(uint64_t contentTag) { aurora::end_frame(contentTag); }
void aurora_set_stereo_scene_anchor(const float anchorFromScene[12]) {
    aurora::set_stereo_scene_anchor(anchorFromScene);
}
void aurora_set_stereo_scene_anchor_scaled(const float anchorFromScene[12], float unitsPerMeter) {
  aurora::set_stereo_scene_anchor(anchorFromScene);
  uint32_t bits=0;std::memcpy(&bits,&unitsPerMeter,4);
  if((bits&0x7f800000u)!=0x7f800000u && unitsPerMeter>0)
    aurora::g_pendingSceneAnchor.unitsPerMeter=unitsPerMeter;
}
void aurora_set_frame_worker_wait_callback(AuroraFrameWorkerWaitCallback callback) {
  aurora::g_frameWorkerWaitCallback.store(callback, std::memory_order_release);
}
void aurora_set_stereo_frame_provider(AuroraStereoFrameProvider provider, void* userdata) {
  aurora::set_stereo_frame_provider(provider, userdata);
}
void aurora_wait_for_frame_worker() { aurora::wait_for_frame_worker(); }
bool aurora_wait_for_frame_worker_for(uint32_t timeoutMicros) {
  return aurora::wait_for_frame_worker_for(std::chrono::microseconds(timeoutMicros));
}
void aurora_quiesce_frame_worker() { aurora::quiesce_frame_worker(); }
void aurora_set_present_schedule(uint64_t baseNanos, uint64_t intervalNanos) {
  aurora::g_presentScheduleBaseNanos.store(baseNanos, std::memory_order_release);
  aurora::g_presentScheduleIntervalNanos.store(intervalNanos, std::memory_order_release);
}
void aurora_report_producer_paced(bool paced) {
#ifdef AURORA_ENABLE_GX
  aurora::gx::report_producer_paced(paced);
#else
  (void)paced;
#endif
}
void aurora_get_frame_interpolation_diagnostics(AuroraFrameInterpolationDiagnostics* diagnostics) {
  if (diagnostics != nullptr) {
    aurora::gx::get_frame_interpolation_diagnostics(*diagnostics);
  }
}

void aurora_get_present_timing(AuroraPresentTiming* timing) {
  if (timing != nullptr) {
    *timing = aurora::snapshot_present_timing();
  }
}
void aurora_set_frame_interpolation_fps(uint32_t targetFps) {
#ifdef AURORA_ENABLE_GX
  aurora::gx::set_frame_interpolation_fps(targetFps);
#else
  (void)targetFps;
#endif
}
uint32_t aurora_get_frame_interpolation_fps() {
#ifdef AURORA_ENABLE_GX
  return aurora::gx::frame_interpolation_fps();
#else
  return 0;
#endif
}
void aurora_request_frame_capture(uint32_t frame, const char* outputPath) {
  aurora::g_captureOutputPath = outputPath != nullptr ? outputPath : "frame_capture.bmp";
  aurora::g_captureFrame.store(frame, std::memory_order_release);
}

void aurora_set_vr_controller_model(uint32_t hand,const AuroraVRControllerVertex* triangles,uint32_t count) {
  if(hand>=2 || count>300000 || count%3!=0) return;
  std::lock_guard lock(aurora::vrui::mutex);
  if(triangles&&count) aurora::vrui::models[hand].assign(triangles,triangles+count);
  else aurora::vrui::models[hand].clear();
}
float aurora_get_vr_panel_aspect(void) {
  return aurora::vrui::panelAspect.load(std::memory_order_relaxed);
}
void aurora_set_vr_controller_texture(uint32_t material,uint32_t width,uint32_t height,const uint8_t* rgba) {
  if(!material || !rgba || !width || !height || width>8192 || height>8192) return;
  std::lock_guard lock(aurora::vrui::mutex);
  auto& texture=aurora::vrui::controllerTextures[material];
  if(texture.binding || !texture.pixels.empty()) return;
  texture.width=width;texture.height=height;
  texture.pixels.assign(rgba,rgba+size_t(width)*height*4);
}
void aurora_set_vr_ui_pointer(float x,float y,bool active,bool down) {
    aurora::imgui::set_vr_pointer(x,y,active,down);
}
void aurora_set_vr_controller_anchors(uint32_t hand,const float* positions) {
  if(hand>=2 || !positions) return;
  std::lock_guard lock(aurora::vrui::mutex);
  for(int row=0;row<5;++row) for(int axis=0;axis<3;++axis)
    aurora::vrui::anchors[hand][row][axis]=positions[row*3+axis];
}
void aurora_set_vr_menu_shader_quality(int quality) {
  aurora::vrui::requestedQuality.store(std::clamp(quality,0,3),std::memory_order_relaxed);
}
void aurora_set_vr_ui_guide(const AuroraVRUiGuide* guide) {
  std::lock_guard lock(aurora::vrui::mutex);
  aurora::vrui::guide=guide?*guide:AuroraVRUiGuide{};
}
bool aurora_flush_efb_copies_to_ram() {
#ifdef AURORA_ENABLE_GX
  if (!aurora::gfx::efb_ram::has_pending()) {
    return true;
  }
  if (!aurora::gfx::efb_ram::prepare_downloads()) {
    return false;
  }

  // This finalizes the frame still being recorded, on the producer thread, so join the whole cycle
  // first: the encode phase owns the previous passes, EFB targets and image pool.
  aurora::wait_for_frame_worker();
  // The renderer is about to submit a prefix of the active frame. Its resumed
  // suffix cannot safely be replayed against the same mutable EFB resources.
  aurora::gx::mark_frame_interpolation_replay_unsafe();
  aurora::gx::fifo::drain();
  const wgpu::CommandEncoderDescriptor encoderDescriptor{
      .label = "GX CPU-visible EFB copy encoder",
  };
  auto encoder = aurora::webgpu::g_device.CreateCommandEncoder(&encoderDescriptor);
  aurora::gfx::end_batch(encoder);
  aurora::gfx::render(encoder);
  aurora::gfx::efb_ram::encode_downloads(encoder);
  const wgpu::CommandBufferDescriptor commandDescriptor{
      .label = "GX CPU-visible EFB copy command buffer",
  };
  const auto commandBuffer = encoder.Finish(&commandDescriptor);
  {
    std::lock_guard submitLock(aurora::g_queueSubmitMutex);
    aurora::webgpu::g_queue.Submit(1, &commandBuffer);
  }
  const bool copied = aurora::gfx::efb_ram::complete_downloads();
  aurora::gfx::after_submit();
  const bool resumed = aurora::gfx::resume_frame();
  return copied && resumed;
#else
  return true;
#endif
}
bool aurora_flush_efb_copy_to_ram(void* dest) {
#ifdef AURORA_ENABLE_GX
  if (dest == nullptr || !aurora::gfx::efb_ram::has_pending(dest) || !aurora::gfx::efb_ram::prepare_downloads(dest)) {
    return false;
  }

  // See aurora_flush_efb_copies_to_ram: this encodes the in-progress frame on
  // the producer thread, so the worker's overlapped encode has to be finished.
  aurora::wait_for_frame_worker();
  // Preserve the requested output cadence by duplicating the completed native
  // image instead of replaying this split frame.
  aurora::gx::mark_frame_interpolation_replay_unsafe();
  aurora::gx::fifo::drain();
  const wgpu::CommandEncoderDescriptor encoderDescriptor{
      .label = "GX demanded EFB copy encoder",
  };
  auto encoder = aurora::webgpu::g_device.CreateCommandEncoder(&encoderDescriptor);
  aurora::gfx::end_batch(encoder);
  aurora::gfx::render(encoder);
  aurora::gfx::efb_ram::encode_downloads(encoder, dest);
  const wgpu::CommandBufferDescriptor commandDescriptor{
      .label = "GX demanded EFB copy command buffer",
  };
  const auto commandBuffer = encoder.Finish(&commandDescriptor);
  {
    std::lock_guard submitLock(aurora::g_queueSubmitMutex);
    aurora::webgpu::g_queue.Submit(1, &commandBuffer);
  }
  const bool copied = aurora::gfx::efb_ram::complete_downloads();
  aurora::gfx::after_submit();
  const bool resumed = aurora::gfx::resume_frame();
  return copied && resumed;
#else
  (void)dest;
  return true;
#endif
}
AuroraBackend aurora_get_backend() { return aurora::g_config.desiredBackend; }
const AuroraBackend* aurora_get_available_backends(size_t* count) {
  if (count != nullptr) {
    *count = aurora::PreferredBackendOrder.size();
  }
  return aurora::PreferredBackendOrder.data();
}
void aurora_set_log_level(AuroraLogLevel level) { aurora::g_config.logLevel = level; }
void aurora_set_pause_on_focus_lost(bool value) { aurora::g_config.pauseOnFocusLost = value; }
void aurora_set_disable_copy_filter(bool disabled) { aurora::g_config.disableCopyFilter = disabled; }
bool aurora_get_disable_copy_filter() { return aurora::g_config.disableCopyFilter; }
void aurora_set_stereo_stop_at_display_copy(bool enabled) { aurora::gfx::set_stereo_stop_at_display_copy(enabled); }
bool aurora_get_stereo_stop_at_display_copy() { return aurora::gfx::get_stereo_stop_at_display_copy(); }
void aurora_set_stereo_skip_copy_clears(bool enabled) { aurora::gfx::set_stereo_skip_copy_clears(enabled); }
bool aurora_get_stereo_skip_copy_clears() { return aurora::gfx::get_stereo_skip_copy_clears(); }
void aurora_set_stereo_hud_screen(bool enabled, float width, float distance) {
  aurora::gfx::set_stereo_hud_screen(enabled, width, distance);
}
bool aurora_get_stereo_hud_screen_enabled() { return aurora::gfx::get_stereo_hud_screen_enabled(); }
void aurora_set_background_input(bool value) {
  aurora::g_config.allowJoystickBackgroundEvents = value;
  aurora::window::set_background_input(value);
}
void aurora_set_display_mode(AuroraDisplayMode mode) { aurora::window::set_display_mode(mode); }
AuroraDisplayMode aurora_get_display_mode() { return aurora::window::get_display_mode(); }
