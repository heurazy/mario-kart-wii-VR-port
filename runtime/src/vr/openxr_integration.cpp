// SPDX-License-Identifier: GPL-3.0-or-later
#include "physical_wheel.h"

#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif

#include "vr/openxr_integration.h"

#include "runtime_config.h"
#include "runtime_log.h"
#include "vr/mkw_vr_first_person.h"
#include "vr/quest_input.h"
#include "vr/steering_wheel.h"
#include "vr/submission_wait.h"
#include "vr/frame_delivery.h"
#include "vr/frame_statistics.h"
#include "vr/adaptive_resolution.h"
#include "vr/mkw_vr_policy.h"
#include "vr/mkw_vr_instrumentation.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#if defined(MKW_ENABLE_OPENXR)
#include "vr/openxr_runtime.h"
#if defined(_WIN32)
#include "vr/openxr_d3d12.h"
#include "vr/openxr_hand_mesh.h"
#else
#include "vr/openxr_vulkan_backend.h"
#endif
#endif

namespace mkw::vr {
namespace {
std::mutex diagnosticsMutex;
OpenXRDiagnostics diagnostics;

void ConfigurePolicy(bool enabled) noexcept {
    SetQuestButtonMapping({RuntimeConfigFile::Get().vrSwapItemTrick,RuntimeConfigFile::Get().vrSwapCockpitDriftBrake});
    MkwVRPolicyReset();
    MkwVRPolicyConfig config{};
    config.enabled = enabled;
    config.immersive_races = true;
    config.world_units_per_meter = RuntimeConfigFile::VrWorldUnitsPerMeter(500.0f);
    config.hud_distance_meters = RuntimeConfigFile::VrHudDistanceMeters(2.0f);
    config.hud_width_meters = RuntimeConfigFile::VrHudWidthMeters(2.4f);
    config.first_person_units_per_meter = RuntimeConfigFile::VrFirstPersonUnitsPerMeter(100.0f);
    MkwVRPolicyConfigure(config);
    MkwVRInstrumentationInitialize();
    MkwVRFirstPersonApplyConfiguredSettings();
}

#if defined(MKW_ENABLE_OPENXR) && defined(_WIN32)

struct Quaternion {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;
};

struct Pose {
    Quaternion orientation{};
    std::array<float, 3> position{};
};

Quaternion Normalize(Quaternion value) noexcept {
    const float length_squared = value.x * value.x + value.y * value.y +
                                 value.z * value.z + value.w * value.w;
    if (!(length_squared > 1.0e-12f)) {
        return {};
    }
    const float inverse_length = 1.0f / std::sqrt(length_squared);
    value.x *= inverse_length;
    value.y *= inverse_length;
    value.z *= inverse_length;
    value.w *= inverse_length;
    return value;
}

Quaternion Conjugate(Quaternion value) noexcept {
    return {-value.x, -value.y, -value.z, value.w};
}

Quaternion Multiply(const Quaternion& left, const Quaternion& right) noexcept {
    return Normalize({
        left.w * right.x + left.x * right.w + left.y * right.z - left.z * right.y,
        left.w * right.y - left.x * right.z + left.y * right.w + left.z * right.x,
        left.w * right.z + left.x * right.y - left.y * right.x + left.z * right.w,
        left.w * right.w - left.x * right.x - left.y * right.y - left.z * right.z,
    });
}

std::array<float, 3> Rotate(const Quaternion& q, const std::array<float, 3>& value) noexcept {
    // Expanded q * [v,0] * conjugate(q), avoiding two temporary normalizations.
    const float tx = 2.0f * (q.y * value[2] - q.z * value[1]);
    const float ty = 2.0f * (q.z * value[0] - q.x * value[2]);
    const float tz = 2.0f * (q.x * value[1] - q.y * value[0]);
    return {
        value[0] + q.w * tx + (q.y * tz - q.z * ty),
        value[1] + q.w * ty + (q.z * tx - q.x * tz),
        value[2] + q.w * tz + (q.x * ty - q.y * tx),
    };
}

void RotationMatrix(const Quaternion& value, float matrix[9]) noexcept {
    const Quaternion q = Normalize(value);
    const float xx = q.x * q.x;
    const float yy = q.y * q.y;
    const float zz = q.z * q.z;
    const float xy = q.x * q.y;
    const float xz = q.x * q.z;
    const float yz = q.y * q.z;
    const float wx = q.w * q.x;
    const float wy = q.w * q.y;
    const float wz = q.w * q.z;
    matrix[0] = 1.0f - 2.0f * (yy + zz);
    matrix[1] = 2.0f * (xy - wz);
    matrix[2] = 2.0f * (xz + wy);
    matrix[3] = 2.0f * (xy + wz);
    matrix[4] = 1.0f - 2.0f * (xx + zz);
    matrix[5] = 2.0f * (yz - wx);
    matrix[6] = 2.0f * (xz - wy);
    matrix[7] = 2.0f * (yz + wx);
    matrix[8] = 1.0f - 2.0f * (xx + yy);
}

Pose CenterPose(const OpenXRFrame& frame, bool position_valid) noexcept {
    const auto& left = frame.views[0].pose;
    const auto& right = frame.views[1].pose;
    Quaternion l{left.orientation.x, left.orientation.y, left.orientation.z,
                 left.orientation.w};
    Quaternion r{right.orientation.x, right.orientation.y, right.orientation.z,
                 right.orientation.w};
    l = Normalize(l);
    r = Normalize(r);
    const float dot = l.x * r.x + l.y * r.y + l.z * r.z + l.w * r.w;
    if (dot < 0.0f) {
        r = {-r.x, -r.y, -r.z, -r.w};
    }
    Pose center;
    center.orientation = Normalize({l.x + r.x, l.y + r.y, l.z + r.z, l.w + r.w});
    if (position_valid) {
        center.position = {
            (left.position.x + right.position.x) * 0.5f,
            (left.position.y + right.position.y) * 0.5f,
            (left.position.z + right.position.z) * 0.5f,
        };
    }
    return center;
}

void IdentityEye(AuroraStereoEye& eye) noexcept {
    std::fill(std::begin(eye.projection), std::end(eye.projection), 0.0f);
    eye.projection[0] = 1.0f;
    eye.projection[5] = 1.0f;
    eye.projection[10] = 1.0f;
    eye.projection[15] = 1.0f;
    std::fill(std::begin(eye.viewFromCenter), std::end(eye.viewFromCenter), 0.0f);
    eye.viewFromCenter[0] = 1.0f;
    eye.viewFromCenter[5] = 1.0f;
    eye.viewFromCenter[10] = 1.0f;
}

void ProjectionFromFov(const XrFovf& fov, float output[16]) noexcept {
    const float left = std::tan(fov.angleLeft);
    const float right = std::tan(fov.angleRight);
    const float down = std::tan(fov.angleDown);
    const float up = std::tan(fov.angleUp);
    const float inverse_width = 1.0f / (right - left);
    const float inverse_height = 1.0f / (up - down);
    std::fill(output, output + 16, 0.0f);
    output[0] = 2.0f * inverse_width;
    output[2] = (right + left) * inverse_width;
    output[5] = 2.0f * inverse_height;
    output[6] = (up + down) * inverse_height;
}

void ViewFromBase(const XrPosef& eye_pose, const Pose& base, bool position_valid,
                  float units_per_meter, float output[12]) noexcept {
    const Quaternion eye = Normalize({eye_pose.orientation.x, eye_pose.orientation.y,
                                      eye_pose.orientation.z, eye_pose.orientation.w});
    const Quaternion inverse_eye = Conjugate(eye);
    const Quaternion delta = Multiply(inverse_eye, base.orientation);
    float rotation[9];
    RotationMatrix(delta, rotation);

    std::array<float, 3> translation{};
    if (position_valid) {
        const std::array<float, 3> base_to_eye{
            base.position[0] - eye_pose.position.x,
            base.position[1] - eye_pose.position.y,
            base.position[2] - eye_pose.position.z,
        };
        translation = Rotate(inverse_eye, base_to_eye);
    }
    output[0] = rotation[0];
    output[1] = rotation[1];
    output[2] = rotation[2];
    output[3] = translation[0] * units_per_meter;
    output[4] = rotation[3];
    output[5] = rotation[4];
    output[6] = rotation[5];
    output[7] = translation[1] * units_per_meter;
    output[8] = rotation[6];
    output[9] = rotation[7];
    output[10] = rotation[8];
    output[11] = translation[2] * units_per_meter;
}

class OpenXRIntegration final {
public:
    static OpenXRIntegration& Get() {
        static OpenXRIntegration integration;
        return integration;
    }

    OpenXRStartupResult Prepare(AuroraConfig& aurora_config) {
        Shutdown();
        {
            std::lock_guard lock(error_mutex_);
            last_error_.clear();
        }
        if (graphics_retained_) {
            SetError("OpenXR cannot be restarted after an unfenceable D3D12 submission");
            return OpenXRStartupResult::Unavailable;
        }
        requested_ = RuntimeConfigFile::VrEnabled(false);
        ConfigurePolicy(requested_);
        if (!requested_) {
            return OpenXRStartupResult::Disabled;
        }
        if (aurora_config.desiredBackend != BACKEND_AUTO &&
            aurora_config.desiredBackend != BACKEND_D3D12) {
            SetError("OpenXR currently requires the D3D12 graphics backend on Windows");
            return OpenXRStartupResult::Unavailable;
        }

        logger_ = [](OpenXRLogLevel level, std::string_view message) {
            const char* name = level == OpenXRLogLevel::Error ? "error" :
                               level == OpenXRLogLevel::Warning ? "warning" : "info";
            RT_LOG(RT_TAG_RUNTIME) << "[openxr::" << name << "] " << message << std::endl;
        };
        runtime_ = std::make_unique<OpenXRRuntime>(logger_);
        backend_ = std::make_unique<OpenXRD3D12Backend>(logger_);

        OpenXRConfig config{};
        config.application_name = aurora_config.appName != nullptr ? aurora_config.appName
                                                                    : "WiiCompiled";
        config.engine_name = "Aurora";
        config.resolution_scale = RuntimeConfigFile::VrRenderScale(1.0f);
        config.required_extensions = {"XR_KHR_D3D12_enable"};
        if (!runtime_->Initialize(config)) {
            SetError("OpenXR instance initialization failed: " + runtime_->LastError().message);
            ResetPreparedObjects();
            return OpenXRStartupResult::Unavailable;
        }
        if (!backend_->QueryGraphicsRequirements(*runtime_)) {
            SetError(backend_->LastError());
            ResetPreparedObjects();
            return OpenXRStartupResult::Unavailable;
        }

        const auto& requirements = backend_->GraphicsRequirements();
        aurora_config.desiredBackend = BACKEND_D3D12;
        aurora_config.xrInterop = true;
        aurora_config.hasD3D12AdapterLuid = true;
        aurora_config.d3d12AdapterLuidLow = requirements.adapter_luid_low;
        aurora_config.d3d12AdapterLuidHigh = requirements.adapter_luid_high;
        prepared_ = true;
        return OpenXRStartupResult::Prepared;
    }

    bool Start(AuroraBackend active_backend) {
        if (!prepared_ || runtime_ == nullptr || backend_ == nullptr) {
            return !requested_;
        }
        if (active_backend != BACKEND_D3D12) {
            SetError("Aurora could not create the OpenXR-required D3D12 backend");
            ResetPreparedObjects();
            return false;
        }
        if (!backend_->BindAurora(*runtime_)) {
            SetError(backend_->LastError());
            ResetPreparedObjects();
            return false;
        }

        stop_.store(false, std::memory_order_release);
        teardown_requested_.store(false, std::memory_order_release);
        WithdrawPublishedFrame();
        aurora_set_stereo_frame_provider(&OpenXRIntegration::ProvideStereoFrame, this);
        provider_registered_ = true;
        running_.store(true, std::memory_order_release);
        try {
            pacing_thread_ = std::thread([this] { PacingThread(); });
        } catch (const std::exception& exception) {
            running_.store(false, std::memory_order_release);
            aurora_set_stereo_frame_provider(nullptr, nullptr);
            provider_registered_ = false;
            SetError(std::string("could not start the OpenXR pacing thread: ") + exception.what());
            ResetPreparedObjects();
            return false;
        }
        RT_LOG(RT_TAG_RUNTIME) << "OpenXR asynchronous D3D12 presentation started" << std::endl;
        return true;
    }

    void Shutdown() noexcept {
        teardown_requested_.store(false, std::memory_order_release);
        if (pacing_thread_.joinable()) {
            // Registration changes are only safe while no sealed frame is in
            // flight. The caller invokes us before Aurora teardown.
            aurora_quiesce_frame_worker();
            aurora_set_stereo_frame_provider(nullptr, nullptr);
            provider_registered_ = false;
            WithdrawPublishedFrame();
            {
                // Pair the predicate update with the wait mutex. Otherwise a
                // terminal pacing thread can observe false, miss the notify,
                // and make join wait forever.
                std::lock_guard lock(stop_mutex_);
                stop_.store(true, std::memory_order_release);
            }
            stop_cv_.notify_all();
            pacing_thread_.join();
        } else {
            if (provider_registered_) {
                aurora_quiesce_frame_worker();
                aurora_set_stereo_frame_provider(nullptr, nullptr);
                provider_registered_ = false;
            }
            ShutdownOrRetainGraphicsObjects();
        }
        running_.store(false, std::memory_order_release);
        PublishQuestInput({});
        MkwVRPolicySetSessionActive(false);
        backend_.reset();
        runtime_.reset();
        prepared_ = false;
        ResetTrackingOrigin();
        applied_session_run_serial_ = 0;
        session_was_active_ = false;
    }

    bool IsRunning() const noexcept { return running_.load(std::memory_order_acquire); }

    void ServiceProducerFrameBoundary() noexcept {
        if (teardown_requested_.load(std::memory_order_acquire)) {
            Shutdown();
        }
    }

    std::string LastError() const {
        std::lock_guard lock(error_mutex_);
        return last_error_;
    }

private:
    struct PublishedFrame {
        AuroraStereoFrame frame{};
    };

    void ResetPreparedObjects() {
        ShutdownOrRetainGraphicsObjects();
        backend_.reset();
        runtime_.reset();
        prepared_ = false;
    }

    bool ShutdownOrRetainGraphicsObjects() noexcept {
        if (backend_ != nullptr && !backend_->Shutdown()) {
            RT_LOG(RT_TAG_RUNTIME)
                << "OpenXR D3D12 queue completion is unknown; retaining the backend, "
                   "runtime, session, and graphics resources until process exit"
                << std::endl;
            (void)backend_.release();
            (void)runtime_.release();
            graphics_retained_ = true;
            return false;
        }
        if (runtime_ != nullptr) {
            runtime_->Shutdown();
        }
        return true;
    }

    static bool ProvideStereoFrame(uint32_t, AuroraStereoFrame* output, void* userdata) {
        auto* self = static_cast<OpenXRIntegration*>(userdata);
        if (self == nullptr || output == nullptr) {
            return false;
        }
        // The packet storage is reused by the XR thread. Claim and copy it
        // under one short lock so cancellation cannot begin the next packet
        // while this callback is preempted between exchange and copy.
        std::lock_guard lock(self->published_mutex_);
        PublishedFrame* frame = self->published_.exchange(nullptr, std::memory_order_acq_rel);
        if (frame == nullptr) {
            return false;
        }
        *output = frame->frame;
        return true;
    }

    void PacingThread() noexcept {
        if (RuntimeConfigFile::Get().vrAsyncPresentation.value_or(true)) {
            PacingThreadAsynchronous();
        } else {
            PacingThreadSynchronous();
        }
    }

    void PacingThreadAsynchronous() noexcept {
        using Clock = std::chrono::steady_clock;
        detail::FrameDelivery delivery;
        OpenXRD3D12Frame pending{};
        Clock::time_point pending_start{};
        auto stats_start = Clock::now();
        uint32_t displays = 0, images = 0, ticks = 0, cancellations = 0;
        uint32_t runtime_hidden = 0, invalid_views = 0, policy_gaps = 0, cache_misses = 0;
        double period_ns = 0.0;
        bool fatal = false;
        uint64_t observed_session = 0;
        RT_LOG(RT_TAG_RUNTIME) << "[mkw-vr] asynchronous compositor enabled; guest VI rate unchanged" << std::endl;

        const auto invalidate = [&] {
            delivery.InvalidateCache();
            backend_->InvalidateCachedFrame();
        };
        const auto service_pending = [&]() -> bool {
            if (!delivery.PendingToken()) return true;
            const auto result = backend_->WaitForSubmission(pending, 0);
            if (result == OpenXRD3D12SubmissionStatus::Success) {
                WithdrawPublishedFrame();
                if (!backend_->CommitSubmission(pending)) {
                    SetError(backend_->LastError());
                    return false;
                }
                delivery.Complete(pending.xr_frame.serial);
                ++images;
                return true;
            }
            if (result != OpenXRD3D12SubmissionStatus::Timeout) {
                SetError("Asynchronous OpenXR GPU image submission failed");
                return false;
            }
            if (Clock::now() - pending_start < detail::kSubmissionGracePeriod) return true;
            WithdrawPublishedFrame();
            if (backend_->TryCancelPendingFrame(pending)) {
                if (!backend_->DiscardCanceledSubmission()) {
                    SetError(backend_->LastError());
                    return false;
                }
                delivery.Cancel();
                ++cancellations;
                return true;
            }
            // Recheck a completion racing the cancellation attempt. An image
            // still owned by the GPU cannot be released by the display loop.
            if (backend_->WaitForSubmission(pending, 0) == OpenXRD3D12SubmissionStatus::Success) {
                if (!backend_->CommitSubmission(pending)) {
                    SetError(backend_->LastError());
                    return false;
                }
                delivery.Complete(pending.xr_frame.serial);
                ++images;
                return true;
            }
            SetError("Asynchronous OpenXR GPU image exceeded its safe submission deadline");
            return false;
        };

        FrameStatistics displayIntervals;
        auto previousDisplay=Clock::now();
        while (!stop_.load(std::memory_order_acquire) && !fatal) {
            const auto events = runtime_->PollEvents();
            const bool active = runtime_->IsSessionRunning();
            MkwVRPolicySetSessionActive(active);
            if (events == OpenXREventStatus::ExitRequested || events == OpenXREventStatus::Error) {
                SetError("OpenXR session ended; continuing on the desktop mirror");
                break;
            }
            if (!service_pending()) break;
            const auto session = runtime_->SessionRunSerial();
            if (session != observed_session) {
                observed_session = session;
                applied_session_run_serial_ = session;
                ResetTrackingOrigin();
                invalidate();
                if (RuntimeConfigFile::Get().vrRefreshHz > 0)
                    runtime_->RequestDisplayRefreshRate(RuntimeConfigFile::Get().vrRefreshHz);
            }
            if (!active) {
                invalidate();
                runtime_->PollControllers(0);
                WaitForStopOrDelay(std::chrono::milliseconds(5));
                continue;
            }
            auto policy = MkwVRPolicyGetSnapshot();
            OpenXRD3D12Presentation presentation{};
            presentation.mode = policy.presentation == VRPresentationMode::ImmersiveRace
                ? OpenXRD3D12FrameMode::ImmersiveProjection : OpenXRD3D12FrameMode::VirtualScreen;
            presentation.quad_distance_meters = policy.config.hud_distance_meters;
            presentation.quad_width_meters = policy.config.hud_width_meters;
            presentation.anchored = runtime_->PanelAnchored();
            OpenXRD3D12Frame display{};
            const auto begin = backend_->BeginDisplayFrame(presentation, display);
            if (begin == OpenXRD3D12BeginStatus::SessionNotRunning) continue;
            if (begin != OpenXRD3D12BeginStatus::Ready) {
                SetError(backend_->LastError());
                break;
            }
            // A job can complete while xrWaitFrame sleeps. Harvest it before
            // presenting, without waiting for the next guest frame to exist.
            if (!service_pending()) {
                backend_->FinishDisplayFrame(display, false);
                break;
            }
            policy = MkwVRPolicyGetSnapshot();
            const bool immersive = policy.presentation == VRPresentationMode::ImmersiveRace;
            display.presentation.mode = immersive ? OpenXRD3D12FrameMode::ImmersiveProjection
                                                   : OpenXRD3D12FrameMode::VirtualScreen;
            ApplyPendingReferenceSpaceChange(display.xr_frame);
            runtime_->PollControllers(display.xr_frame.predicted_display_time, &display.xr_frame);
            display.presentation.anchored=runtime_->PanelAnchored();
            if (runtime_->ConsumeCameraClick() && immersive) MkwVRCycleCamera();
            UpdateDrivingFrame(display, immersive, policy.EffectiveUnitsPerMeter());
            const bool coherent = policy.content_tag == policy.display_content_tag;
            if (!coherent) ++policy_gaps;
            if (!display.xr_frame.should_render) ++runtime_hidden;
            if (display.xr_frame.should_render && !display.xr_frame.views_valid) ++invalid_views;
            if (!delivery.PendingToken() && coherent && display.xr_frame.should_render && display.xr_frame.views_valid) {
                if (!backend_->BeginSubmission(display)) {
                    SetError(backend_->LastError());
                    backend_->FinishDisplayFrame(display, false);
                    break;
                }
                pending = display;
                pending_start = Clock::now();
                delivery.Start(pending.xr_frame.serial, policy.content_tag, session);
                std::lock_guard lock(published_mutex_);
                BuildPublishedFrame(pending, immersive, policy.EffectiveUnitsPerMeter(), policy.content_tag);
                published_.store(&published_frame_, std::memory_order_release);
            }
            // A fresh pose is required to render, not to reproject the last
            // completed image with its original, valid render poses.
            const bool show = delivery.CanDisplay(policy.display_content_tag, session) &&
                              display.xr_frame.should_render;
            if (display.xr_frame.should_render && !show) ++cache_misses;
            if (!backend_->FinishDisplayFrame(display, show)) {
                SetError(backend_->LastError());
                fatal = true;
            }
            ++ticks;
            const auto nowDisplay=Clock::now();
            displayIntervals.Add(std::chrono::duration<float,std::milli>(nowDisplay-previousDisplay).count());
            previousDisplay=nowDisplay;
            displays += show ? 1 : 0;
            period_ns += static_cast<double>(display.xr_frame.predicted_display_period);
            const double seconds = std::chrono::duration<double>(Clock::now() - stats_start).count();
            if (seconds >= 5.0) {
                adaptive_resolution_.Observe(float(images/seconds),float(period_ns>0?1.0e9*ticks/period_ns:0),
                    RuntimeConfigFile::Get().vrAdaptiveResolution && immersive);
                {
                    std::lock_guard lock(diagnosticsMutex);
                    diagnostics={float(displays/seconds),float(images/seconds),float(period_ns>0?1.0e9*ticks/period_ns:0),
                        displayIntervals.Percentile(.95f),displayIntervals.Percentile(.99f),display.render_width[0],display.render_height[0],cancellations,adaptive_resolution_.Scale()};
                }
                RT_LOG(RT_TAG_RUNTIME) << "[mkw-vr] async timing: display-fps=" << displays / seconds
                    << ", new-image-fps=" << images / seconds
                    << ", runtime-hz=" << (period_ns > 0 ? 1.0e9 * ticks / period_ns : 0.0)
                    << ", interval-p95-ms=" << displayIntervals.Percentile(.95f)
                    << ", interval-p99-ms=" << displayIntervals.Percentile(.99f)
                    << ", canceled=" << cancellations
                    << ", runtime-hidden=" << runtime_hidden
                    << ", invalid-views=" << invalid_views
                    << ", policy-gaps=" << policy_gaps
                    << ", cache-misses=" << cache_misses << std::endl;
                stats_start = Clock::now();
                displays = images = ticks = cancellations = 0;
                runtime_hidden = invalid_views = policy_gaps = cache_misses = 0;
                period_ns = 0.0;
            }
        }

        WithdrawPublishedFrame();
        running_.store(false, std::memory_order_release);
        PublishQuestInput({});
        MkwVRPolicySetSessionActive(false);
        if (!stop_.load(std::memory_order_acquire)) {
            teardown_requested_.store(true, std::memory_order_release);
            std::unique_lock lock(stop_mutex_);
            stop_cv_.wait(lock, [this] { return stop_.load(std::memory_order_acquire); });
        }
        ShutdownOrRetainGraphicsObjects();
    }

    void PacingThreadSynchronous() noexcept {
        bool fatal = false;
        bool presentation_logged = false;
        VRPresentationMode logged_presentation = VRPresentationMode::Desktop;
        uint32_t presentation_log_count = 0;
        bool immersive_submission_logged = false;
        auto perf_start = std::chrono::steady_clock::now();
        uint32_t perf_frames = 0;
        double perf_wait_ms = 0.0;
        double perf_submit_ms = 0.0;
        uint32_t canceled_packets = 0;
        uint32_t nonrender_frames = 0;
        while (!stop_.load(std::memory_order_acquire) && !fatal) {
            const OpenXREventStatus events = runtime_->PollEvents();
            const bool session_active = runtime_->IsSessionRunning();
            MkwVRPolicySetSessionActive(session_active);
            const uint64_t session_run_serial = runtime_->SessionRunSerial();
            if (session_run_serial != applied_session_run_serial_) {
                applied_session_run_serial_ = session_run_serial;
                ResetTrackingOrigin();
            }
            if (session_active != session_was_active_) {
                session_was_active_ = session_active;
                if (!session_active) {
                    ResetTrackingOrigin();
                }
            }
            if (events == OpenXREventStatus::ExitRequested) {
                SetError("OpenXR runtime requested session exit; continuing on the desktop mirror");
                break;
            }
            if (events == OpenXREventStatus::Error) {
                SetError("OpenXR event processing failed: " + runtime_->LastError().message);
                break;
            }
            if (!session_active) {
                runtime_->PollControllers(0); // Reset the press latch while unfocused/stopped.
                WaitForStopOrDelay(std::chrono::milliseconds(5));
                continue;
            }

            const MkwVRPolicySnapshot policy = MkwVRPolicyGetSnapshot();
            if ((!presentation_logged || policy.presentation != logged_presentation) &&
                presentation_log_count < 16) {
                presentation_logged = true;
                logged_presentation = policy.presentation;
                ++presentation_log_count;
                RT_LOG(RT_TAG_RUNTIME)
                    << "[mkw-vr] presentation="
                    << (policy.presentation == VRPresentationMode::ImmersiveRace
                            ? "immersive-race"
                            : policy.presentation == VRPresentationMode::VirtualScreen
                                  ? "virtual-screen"
                                  : "desktop")
                    << ", scene=" << static_cast<unsigned>(policy.scene.mode)
                    << ", screens=" << policy.scene.local_player_count
                    << ", camera-valid=" << policy.camera.valid
                    << ", scene-frame=" << policy.scene.guest_frame_index
                    << ", camera-frame=" << policy.camera.guest_frame_index
                    << ", bindings=0x" << std::hex << policy.available_bindings
                    << std::dec << std::endl;
            }
            OpenXRD3D12Presentation presentation{};
            const bool immersive = policy.presentation == VRPresentationMode::ImmersiveRace;
            presentation.mode = immersive ? OpenXRD3D12FrameMode::ImmersiveProjection
                                           : OpenXRD3D12FrameMode::VirtualScreen;
            presentation.quad_distance_meters = policy.config.hud_distance_meters;
            presentation.quad_width_meters = policy.config.hud_width_meters;
            presentation.anchored = runtime_->PanelAnchored();

            OpenXRD3D12Frame frame{};
            const auto wait_start = std::chrono::steady_clock::now();
            const OpenXRD3D12BeginStatus begin = backend_->BeginFrame(presentation, frame);
            const auto wait_end = std::chrono::steady_clock::now();
            if (begin == OpenXRD3D12BeginStatus::SessionNotRunning) {
                MkwVRPolicySetSessionActive(false);
                continue;
            }
            if (begin == OpenXRD3D12BeginStatus::ExitRequested) {
                SetError("OpenXR runtime requested session exit; continuing on the desktop mirror");
                break;
            }
            if (begin == OpenXRD3D12BeginStatus::Error) {
                SetError(backend_->LastError());
                fatal = true;
                break;
            }

            ApplyPendingReferenceSpaceChange(frame.xr_frame);
            runtime_->PollControllers(frame.xr_frame.predicted_display_time, &frame.xr_frame);
            frame.presentation.anchored=runtime_->PanelAnchored();
            if (runtime_->ConsumeCameraClick() && immersive) {
                MkwVRCycleCamera();
            }

            if (!frame.expects_gpu_submission) {
                ++nonrender_frames;
                if ((nonrender_frames & (nonrender_frames - 1)) == 0) {
                    RT_LOG(RT_TAG_RUNTIME) << "[mkw-vr] runtime omitted image: count=" << nonrender_frames
                        << ", should-render=" << frame.xr_frame.should_render
                        << ", views-valid=" << frame.xr_frame.views_valid << std::endl;
                }
                if (!backend_->FinishFrame(frame, false)) {
                    SetError(backend_->LastError());
                    fatal = true;
                }
                continue;
            }

            {
                std::lock_guard lock(published_mutex_);
                // First person renders at life-size scale, third person at the
                // configured diorama scale. Head translation and IPD are the
                // only things this multiplies, so a one-frame disagreement with
                // the camera's own switch is not observable.
                BuildPublishedFrame(frame, immersive, policy.EffectiveUnitsPerMeter(),
                                    policy.content_tag);
                published_.store(&published_frame_, std::memory_order_release);
            }

            const auto submission_deadline =
                std::chrono::steady_clock::now() + detail::kSubmissionGracePeriod;
            const auto waited = detail::WaitForPublishedSubmission<OpenXRD3D12SubmissionStatus>(
                [this] { return stop_.load(std::memory_order_acquire); },
                [this, &frame](uint32_t ms) { return backend_->WaitForSubmission(frame, ms); },
                [&] { return std::chrono::steady_clock::now() >= submission_deadline; },
                [this] { WithdrawPublishedFrame(); },
                [this, &frame] { return backend_->TryCancelPendingFrame(frame); });
            const auto submission = waited.status;
            WithdrawPublishedFrame();
            if (stop_.load(std::memory_order_acquire)) {
                // Aurora has been drained by Shutdown(); backend shutdown below
                // cancels its pending target, then either safely releases the
                // XR image or retains the entire graph if GPU completion is unknown.
                break;
            }
            if (waited.canceled) {
                ++canceled_packets;
                if ((canceled_packets & (canceled_packets - 1)) == 0) {
                    RT_LOG(RT_TAG_RUNTIME) << "[mkw-vr] producer deadline expired after 10 s: canceled="
                        << canceled_packets << ", token=" << frame.xr_frame.serial << std::endl;
                }
                if (!backend_->FinishFrame(frame, false)) {
                    SetError(backend_->LastError());
                    fatal = true;
                }
                continue;
            }
            if (waited.stalled) {
                SetError("Aurora did not complete the OpenXR stereo submission; "
                         "requesting a safe desktop fallback");
                fatal = true;
                break;
            }
            const bool submit = submission == OpenXRD3D12SubmissionStatus::Success;
            if (submit && immersive) {
                const auto now = std::chrono::steady_clock::now();
                ++perf_frames;
                perf_wait_ms += std::chrono::duration<double, std::milli>(wait_end - wait_start).count();
                perf_submit_ms += std::chrono::duration<double, std::milli>(now - wait_end).count();
                const double seconds = std::chrono::duration<double>(now - perf_start).count();
                if (seconds >= 5.0) {
                    RT_LOG(RT_TAG_RUNTIME) << "[mkw-vr] timing: submitted-fps=" << perf_frames / seconds
                        << ", xr-wait-ms=" << perf_wait_ms / perf_frames
                        << ", producer-and-submit-ms=" << perf_submit_ms / perf_frames << std::endl;
                    perf_start = now;
                    perf_frames = 0;
                    perf_wait_ms = perf_submit_ms = 0.0;
                }
            } else {
                perf_start = std::chrono::steady_clock::now();
                perf_frames = 0;
                perf_wait_ms = perf_submit_ms = 0.0;
            }
            if (!backend_->FinishFrame(frame, submit)) {
                SetError(backend_->LastError());
                fatal = true;
            } else if (!submit) {
                SetError("Aurora's D3D12 stereo copy failed; continuing on the desktop mirror");
                fatal = true;
            } else if (immersive && !immersive_submission_logged) {
                immersive_submission_logged = true;
                RT_LOG(RT_TAG_RUNTIME)
                    << "[mkw-vr] first immersive packet consumed and submitted as "
                       "an OpenXR projection layer"
                    << std::endl;
            }
        }

        running_.store(false, std::memory_order_release);
        PublishQuestInput({});
        MkwVRPolicySetSessionActive(false);
        if (!stop_.load(std::memory_order_acquire)) {
            // A runtime/backend failure can happen while Aurora is submitting.
            // Ask the producer to reach a safe frame boundary, drain Aurora,
            // and unregister the provider before this XR owner destroys state.
            teardown_requested_.store(true, std::memory_order_release);
            std::unique_lock lock(stop_mutex_);
            stop_cv_.wait(lock, [this] { return stop_.load(std::memory_order_acquire); });
        }
        ShutdownOrRetainGraphicsObjects();
    }

    void UpdateDrivingFrame(const OpenXRD3D12Frame& source, bool immersive,
                            float units_per_meter) noexcept {
        ApplyPendingReferenceSpaceChange(source.xr_frame);
        auto& destination = driving_frame_;
        destination = {};
        if (!immersive) {
            last_immersive_ = false;
            wheel_.Update({}, false, 0);
            runtime_->PublishDrivingInput(false, 0, false, 0);
            return;
        }

        const bool position_valid =
            (source.xr_frame.view_state_flags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0;
        if (!base_pose_valid_ || !last_immersive_) {
            base_pose_ = CenterPose(source.xr_frame, position_valid);
            // Recentring establishes heading, never headset pitch or roll.
            // Otherwise looking down at the tutorial tilts the whole track.
            const auto forward=Rotate(base_pose_.orientation,{0.0f,0.0f,-1.0f});
            const float yaw=std::atan2(-forward[0],-forward[2]);
            base_pose_.orientation={0.0f,std::sin(yaw*0.5f),0.0f,std::cos(yaw*0.5f)};
            base_pose_valid_ = true;
            base_position_valid_ = position_valid;
        } else if (position_valid && !base_position_valid_) {
            base_pose_.position = CenterPose(source.xr_frame, true).position;
            base_position_valid_ = true;
        }
        last_immersive_ = true;
        if (!hand_meshes_loaded_) {
            hand_meshes_loaded_ = true;
            const bool loaded = LoadRuntimeHandMeshes(*runtime_);
            RT_LOG(RT_TAG_RUNTIME) << "[mkw-vr] cockpit hands: "
                << (loaded ? "Meta runtime mesh" : "controller glove fallback (Meta mesh unavailable)") << std::endl;
        }
        auto& cockpit = destination.cockpit;
        const auto camera = MkwVRFirstPersonGetSnapshot();
        driving_camera_mode_=camera.mode;
        const auto& driverAnchor = camera.anchor;
        if(camera.mode!=CameraMode::Game && driverAnchor.valid && driverAnchor.units_per_meter>0)
            units_per_meter=driverAnchor.units_per_meter;
        cockpit.unitsPerMeter=units_per_meter;
        cockpit.active = position_valid && base_position_valid_ && runtime_->IsSessionFocused() &&
            camera.mode == CameraMode::FirstPerson && driverAnchor.valid;
        cockpit.nativeWheel = cockpit.active && RuntimeConfigFile::VrNativeSteeringWheel() &&
            driverAnchor.native_wheel.valid && driverAnchor.native_mesh_prepared;
        cockpit.bike=driverAnchor.bike;
        WheelGeometry drivingGeometry=driverAnchor.native_wheel;
        const auto time = source.xr_frame.predicted_display_time;
        const float dt = last_wheel_time_ > 0 ? float(time - last_wheel_time_) * 1.0e-9f : 1.0f / 90.0f;
        last_wheel_time_ = time;
        cockpit.nativeWheel=wheel_reference_.Resolve(drivingGeometry,
            cockpit.active && RuntimeConfigFile::VrNativeSteeringWheel(),cockpit.nativeWheel,
            last_held_[0]||last_held_[1],cockpit.bike,driverAnchor.vehicle_identity,dt);
        if(cockpit.bike && !drivingGeometry.valid) {
            drivingGeometry.center={0,SteeringWheel::Height,SteeringWheel::Depth};
            drivingGeometry.right={1,0,0}; drivingGeometry.up={0,0,-1}; drivingGeometry.normal={0,1,0};
            drivingGeometry.radius=0.25f; drivingGeometry.valid=true;
        }
        if(cockpit.bike) {
            cockpit.handlebarRadius=drivingGeometry.radius;
            for(int row=0;row<3;++row) {
                cockpit.seatFromHandlebar[row*4]=drivingGeometry.right[row];
                cockpit.seatFromHandlebar[row*4+1]=drivingGeometry.up[row];
                cockpit.seatFromHandlebar[row*4+2]=drivingGeometry.normal[row];
                cockpit.seatFromHandlebar[row*4+3]=drivingGeometry.center[row];
            }
        }
        if (cockpit.nativeWheel != last_native_wheel_ || cockpit.bike!=last_bike_) {
            wheel_ = {};
            last_native_wheel_ = cockpit.nativeWheel;
            last_bike_=cockpit.bike;
        }
        std::array<WheelHand, 2> hands{};
        for (size_t hand = 0; hand < 2; ++hand) {
            auto& target = cockpit.hands[hand];
            target.tracked = hand ? runtime_->RightGripValid() : runtime_->LeftGripValid();
            const auto& grip = hand ? runtime_->RightGripPose() : runtime_->LeftGripPose();
            target.squeeze = runtime_->HandSqueeze(hand);
            const auto position = Rotate(Conjugate(base_pose_.orientation), {
                grip.position.x - base_pose_.position[0], grip.position.y - base_pose_.position[1],
                grip.position.z - base_pose_.position[2]});
            const auto rotation = Multiply(Conjugate(base_pose_.orientation),
                {grip.orientation.x, grip.orientation.y, grip.orientation.z, grip.orientation.w});
            float matrix[9];
            RotationMatrix(rotation, matrix);
            for (int row = 0; row < 3; ++row) {
                for (int col = 0; col < 3; ++col) target.seatFromGrip[row * 4 + col] = matrix[row * 3 + col];
                target.seatFromGrip[row * 4 + 3] = position[row];
            }
            hands[hand] = {position[0], position[1], position[2], target.squeeze, target.tracked};
        }
        if (cockpit.nativeWheel || cockpit.bike)
            for (auto& hand : hands) hand = drivingGeometry.ToWheel(hand);
        float hardwareSteering = 0;
        const bool hardwareWheel = physical_wheel::SteeringSnapshot(hardwareSteering);
        auto wheel = wheel_.Update(hands, cockpit.active && !hardwareWheel, dt,
            cockpit.nativeWheel || cockpit.bike ? drivingGeometry.radius : SteeringWheel::Radius,cockpit.bike,
            RuntimeConfigFile::VrWheelTuning());
        // A kart rim follows full hand rotation; handlebars retain their
        // limited visual travel, while the input accumulator keeps overtravel.
        if (hardwareWheel) {
            const auto tuning = RuntimeConfigFile::VrWheelTuning();
            wheel.steering = hardwareSteering;
            wheel.visualAngle = hardwareSteering * (cockpit.bike ? tuning.bikeDegrees : tuning.kartDegrees) * 0.01745329252f;
        }
        cockpit.wheelAngle = wheel.visualAngle;
        for (size_t hand=0;hand<2;++hand)
            if (wheel.held[hand]!=last_held_[hand] && cockpit.active && RuntimeConfigFile::VrWheelTuning().haptics)
                runtime_->PulseGrip(hand,wheel.held[hand]);
        last_held_=wheel.held;
        for (int hand = 0; hand < 2; ++hand) cockpit.hands[hand].held = wheel.held[hand];
        runtime_->PublishDrivingInput(cockpit.active, wheel.steering, hardwareWheel || wheel.held[0] || wheel.held[1],cockpit.wheelAngle);
    }

    void BuildPublishedFrame(const OpenXRD3D12Frame& source, bool immersive,
                             float units_per_meter, uint64_t content_tag) noexcept {
        // The synchronous path also updates once per XR frame. The asynchronous
        // path has already updated even when the renderer could not accept work.
        if (last_wheel_time_ != source.xr_frame.predicted_display_time || !immersive)
            UpdateDrivingFrame(source, immersive, units_per_meter);
        auto& destination = published_frame_.frame;
        destination = driving_frame_;
        destination.frameToken = source.xr_frame.serial;
        destination.contentTag = content_tag;
        destination.renderScale=RuntimeConfigFile::Get().vrAdaptiveResolution?adaptive_resolution_.Scale():1.0f;
        destination.mode = immersive ? AURORA_STEREO_FRAME_IMMERSIVE_REPLAY : AURORA_STEREO_FRAME_VIRTUAL_SCREEN;
        for (uint32_t eye = 0; eye < kOpenXREyeCount; ++eye) {
            destination.eyes[eye].width = source.render_width[eye];
            destination.eyes[eye].height = source.render_height[eye];
            IdentityEye(destination.eyes[eye]);
        }
        if (!immersive) {
            destination.ui.anchored=source.presentation.anchored;
            if(destination.ui.anchored) {
                const auto anchor=runtime_->PanelOrigin();
                const Pose panel{{anchor.orientation.x,anchor.orientation.y,anchor.orientation.z,anchor.orientation.w},
                    {anchor.position.x,anchor.position.y,anchor.position.z}};
                destination.ui.distance=source.presentation.quad_distance_meters;
                destination.ui.width=source.presentation.quad_width_meters;
                const auto input=ReadQuestInputSnapshot();
                for(int hand=0;hand<2;++hand) {
                    const auto& p=input.ui_hands[hand];destination.ui.tracked[hand]=p.valid;
                    const auto& aim=hand?input.ui_pointer:input.ui_left_pointer;
                    destination.ui.pointerTracked[hand]=aim.valid && input.active;
                    for(int axis=0;axis<3;++axis) {
                        destination.ui.pointerRay[hand][axis]=aim.position[axis];
                        destination.ui.pointerRay[hand][axis+3]=aim.forward[axis];
                    }
                    auto* m=destination.ui.panelFromGrip[hand];
                    for(int row=0;row<3;++row) { m[row*4]=p.right[row];m[row*4+1]=p.up[row];m[row*4+2]=-p.forward[row];m[row*4+3]=p.position[row]; }
                }
                for(int eye=0;eye<2;++eye) {
                    ViewFromBase(source.xr_frame.views[eye].pose,panel,true,1,destination.ui.eyeFromPanel[eye]);
                    ProjectionFromFov(source.xr_frame.views[eye].fov,destination.eyes[eye].projection);
                }
            }
            return;
        }
        units_per_meter=destination.cockpit.unitsPerMeter;
        const bool position_valid = (source.xr_frame.view_state_flags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0;
        auto& cockpit = destination.cockpit;
        Pose hand_panel{};
        // In the cockpit, use the existing fixed forward HUD plane (including
        // items). It follows the seated frame, not head turns or controller loss.
        destination.handHud = driving_camera_mode_ != CameraMode::FirstPerson &&
            position_valid && runtime_->LeftGripValid();
        if (destination.handHud) {
            const auto& grip = runtime_->LeftGripPose();
            const auto offset = Rotate({grip.orientation.x, grip.orientation.y,
                                        grip.orientation.z, grip.orientation.w}, {0.0f, 0.06f, -0.035f});
            hand_panel.position = {grip.position.x + offset[0], grip.position.y + offset[1],
                                   grip.position.z + offset[2]};
            // Billboard toward the cyclopean eye, upright in tracking space.
            const auto head = CenterPose(source.xr_frame, true);
            const float dx = head.position[0] - hand_panel.position[0];
            const float dy = head.position[1] - hand_panel.position[1];
            const float dz = head.position[2] - hand_panel.position[2];
            const float yaw = std::atan2(dx, dz);
            const float pitch = -std::atan2(dy, std::sqrt(dx * dx + dz * dz));
            hand_panel.orientation = Multiply({0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)},
                                             {std::sin(pitch * 0.5f), 0.0f, 0.0f, std::cos(pitch * 0.5f)});
            // HudScreen lifts NDC onto z=-1. Offset the panel origin so that
            // this plane, rather than its origin, sits above the controller.
            const auto normal = Rotate(hand_panel.orientation, {0.0f, 0.0f, 1.0f});
            for (int axis = 0; axis < 3; ++axis) hand_panel.position[axis] += normal[axis];
        }
        for (uint32_t eye = 0; eye < kOpenXREyeCount; ++eye) {
            ViewFromBase(source.xr_frame.views[eye].pose, base_pose_, position_valid, 1.0f,
                         cockpit.eyeFromSeat[eye]);
            if (destination.handHud) {
                ViewFromBase(source.xr_frame.views[eye].pose, hand_panel, true, 1.0f,
                             destination.handHudViewFromPanel[eye]);
            }
            ProjectionFromFov(source.xr_frame.views[eye].fov,
                              destination.eyes[eye].projection);
            ViewFromBase(source.xr_frame.views[eye].pose, base_pose_,
                         position_valid && base_position_valid_,
                         units_per_meter, destination.eyes[eye].viewFromCenter);
        }
    }

    void ApplyPendingReferenceSpaceChange(const OpenXRFrame& frame) noexcept {
        if (runtime_->ConsumeAppSpaceChangesThrough(frame.predicted_display_time)) {
            ResetTrackingOrigin();
        }
    }

    void ResetTrackingOrigin() noexcept {
        wheel_ = {};
        wheel_reference_={};last_held_={};
        last_wheel_time_ = 0;
        base_pose_ = {};
        base_pose_valid_ = false;
        base_position_valid_ = false;
        last_immersive_ = false;
    }

    void WaitForStopOrDelay(std::chrono::milliseconds delay) {
        std::unique_lock lock(stop_mutex_);
        stop_cv_.wait_for(lock, delay,
                          [this] { return stop_.load(std::memory_order_acquire); });
    }

    void WithdrawPublishedFrame() noexcept {
        std::lock_guard lock(published_mutex_);
        published_.store(nullptr, std::memory_order_release);
    }

    void SetError(std::string message) {
        {
            std::lock_guard lock(error_mutex_);
            last_error_ = std::move(message);
        }
        RT_LOG(RT_TAG_RUNTIME) << "OpenXR: " << LastError() << std::endl;
    }

    OpenXRLogCallback logger_;
    std::unique_ptr<OpenXRRuntime> runtime_;
    std::unique_ptr<OpenXRD3D12Backend> backend_;
    std::thread pacing_thread_;
    std::atomic_bool stop_{false};
    std::atomic_bool running_{false};
    std::atomic_bool teardown_requested_{false};
    std::atomic<PublishedFrame*> published_{nullptr};
    PublishedFrame published_frame_{};
    AuroraStereoFrame driving_frame_{};
    CameraMode driving_camera_mode_=CameraMode::Game;
    std::mutex published_mutex_;
    std::mutex stop_mutex_;
    std::condition_variable stop_cv_;
    mutable std::mutex error_mutex_;
    std::string last_error_;
    Pose base_pose_{};
    SteeringWheel wheel_;
    WheelReferenceLatch wheel_reference_;
    AdaptiveResolution adaptive_resolution_;
    std::array<bool,2> last_held_{};
    XrTime last_wheel_time_ = 0;
    bool last_native_wheel_ = false;
    bool last_bike_ = false;
    bool hand_meshes_loaded_ = false;
    bool base_pose_valid_ = false;
    bool base_position_valid_ = false;
    bool last_immersive_ = false;
    uint64_t applied_session_run_serial_ = 0;
    bool session_was_active_ = false;
    bool requested_ = false;
    bool prepared_ = false;
    bool provider_registered_ = false;
    bool graphics_retained_ = false;
};

#endif // defined(MKW_ENABLE_OPENXR) && defined(_WIN32)

} // namespace

OpenXRDiagnostics OpenXRGetDiagnostics() {
    std::lock_guard lock(diagnosticsMutex);
    return diagnostics;
}

OpenXRStartupResult OpenXRPrepareAurora(AuroraConfig& config) {
#if !defined(MKW_ENABLE_OPENXR)
    (void)config;
    ConfigurePolicy(false);
    return RuntimeConfigFile::VrEnabled(false) ? OpenXRStartupResult::Unavailable
                                               : OpenXRStartupResult::Disabled;
#elif defined(_WIN32)
    return OpenXRIntegration::Get().Prepare(config);
#else
    ConfigurePolicy(RuntimeConfigFile::VrEnabled(false));
    if (!RuntimeConfigFile::VrEnabled(false)) {
        return OpenXRStartupResult::Disabled;
    }
    const OpenXRVulkanCapabilityInfo capability = OpenXRVulkanBackend::DawnInteropCapability();
    RT_LOG(RT_TAG_RUNTIME) << "OpenXR Vulkan unavailable: " << capability.reason << std::endl;
    return OpenXRStartupResult::Unavailable;
#endif
}

bool OpenXRStartAfterAurora(AuroraBackend active_backend) {
#if defined(MKW_ENABLE_OPENXR) && defined(_WIN32)
    return OpenXRIntegration::Get().Start(active_backend);
#else
    (void)active_backend;
    return !RuntimeConfigFile::VrEnabled(false);
#endif
}

void OpenXRShutdownBeforeAurora() noexcept {
#if defined(MKW_ENABLE_OPENXR) && defined(_WIN32)
    OpenXRIntegration::Get().Shutdown();
#else
    MkwVRPolicySetSessionActive(false);
#endif
}

void OpenXRServiceProducerFrameBoundary() noexcept {
#if defined(MKW_ENABLE_OPENXR) && defined(_WIN32)
    OpenXRIntegration::Get().ServiceProducerFrameBoundary();
#endif
}

bool OpenXRIsRunning() noexcept {
#if defined(MKW_ENABLE_OPENXR) && defined(_WIN32)
    return OpenXRIntegration::Get().IsRunning();
#else
    return false;
#endif
}

std::string OpenXRLastError() {
#if !defined(MKW_ENABLE_OPENXR)
    return "this build was compiled without OpenXR support";
#elif defined(_WIN32)
    return OpenXRIntegration::Get().LastError();
#else
    return OpenXRVulkanBackend::DawnInteropCapability().reason;
#endif
}

} // namespace mkw::vr
