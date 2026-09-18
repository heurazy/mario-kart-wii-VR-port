// SPDX-License-Identifier: GPL-3.0-or-later

#if defined(MKW_ENABLE_OPENXR) && defined(_WIN32)

// OpenXR's D3D12 structures are selected when openxr_platform.h is parsed.
#define XR_USE_GRAPHICS_API_D3D12
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "vr/openxr_d3d12.h"

#include <aurora/d3d12_interop.h>

#include <d3d12.h>
#include <dxgi1_4.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <sstream>
#include <utility>
#include <vector>

namespace mkw::vr {
namespace {

bool SameDxgiCopyFamily(DXGI_FORMAT left, DXGI_FORMAT right) noexcept {
    const auto family = [](DXGI_FORMAT format) noexcept {
        switch (format) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            return 1;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            return 2;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
            return 3;
        default:
            return 0;
        }
    };
    const int left_family = family(left);
    return left_family != 0 && left_family == family(right);
}

DXGI_FORMAT SrgbSibling(DXGI_FORMAT format) noexcept {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

bool IsSrgbFormat(DXGI_FORMAT format) noexcept {
    return format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
           format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
}

const char* BeginStatusOperation(OpenXRFrameStatus status) noexcept {
    switch (status) {
    case OpenXRFrameStatus::Ready:
        return "ready";
    case OpenXRFrameStatus::SessionNotRunning:
        return "session is not running";
    case OpenXRFrameStatus::ExitRequested:
        return "runtime requested exit";
    case OpenXRFrameStatus::Error:
        return "xrWaitFrame failed";
    }
    return "unknown frame status";
}

} // namespace

class OpenXRD3D12Backend::Impl final {
public:
    explicit Impl(OpenXRLogCallback logger) : logger_(std::move(logger)) {}

    ~Impl() { Shutdown(); }

    struct EyeSwapchain {
        XrSwapchain handle = XR_NULL_HANDLE;
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<XrSwapchainImageD3D12KHR> images;
        uint32_t acquired_index = 0;
        bool acquired = false;
        bool waited = false;
        bool release_forbidden = false;
    };

    bool QueryGraphicsRequirements(OpenXRRuntime& runtime) {
        ClearError();
        if (!runtime.IsInitialized() || runtime.HasSession()) {
            return Fail("OpenXR must own an instance, but no session, before querying D3D12 requirements");
        }
        if (requirements_queried_ && runtime_ != &runtime) {
            return Fail("D3D12 graphics requirements were already queried from another OpenXR instance");
        }

        PFN_xrGetD3D12GraphicsRequirementsKHR get_requirements = nullptr;
        if (!runtime.LoadFunction("xrGetD3D12GraphicsRequirementsKHR", &get_requirements) ||
            get_requirements == nullptr) {
            return Fail("OpenXR runtime did not expose xrGetD3D12GraphicsRequirementsKHR");
        }

        XrGraphicsRequirementsD3D12KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
        const XrResult result = get_requirements(runtime.Instance(), runtime.SystemId(), &requirements);
        runtime.ObserveResult(result);
        if (XR_FAILED(result)) {
            std::ostringstream message;
            message << "xrGetD3D12GraphicsRequirementsKHR failed (" << result << ')';
            return Fail(message.str());
        }

        runtime_ = &runtime;
        {
            std::lock_guard lock(submission_mutex_);
            shutting_down_ = false;
            submission_unsafe_ = false;
        }
        requirements_ = {
            requirements.adapterLuid.LowPart,
            requirements.adapterLuid.HighPart,
            static_cast<uint32_t>(requirements.minFeatureLevel),
        };
        requirements_queried_ = true;

        std::ostringstream message;
        message << "OpenXR D3D12 adapter LUID " << std::hex
                << static_cast<uint32_t>(requirements_.adapter_luid_high) << ':'
                << requirements_.adapter_luid_low << ", minimum feature level 0x"
                << requirements_.minimum_feature_level;
        Log(OpenXRLogLevel::Info, message.str());
        return true;
    }

    bool BindAurora(OpenXRRuntime& runtime) {
        ClearError();
        if (!requirements_queried_ || runtime_ != &runtime || runtime.HasSession()) {
            return Fail("QueryGraphicsRequirements must succeed on this OpenXR instance before BindAurora");
        }
        if (bound_) {
            return Fail("OpenXR D3D12 backend is already bound");
        }

        AuroraD3D12NativeHandles handles{};
        if (!aurora_d3d12_get_native_handles(&handles) || handles.device == nullptr ||
            handles.queue == nullptr) {
            return Fail("Aurora did not expose a Dawn D3D12 device and command queue");
        }
        if (handles.adapterLuidLow != requirements_.adapter_luid_low ||
            handles.adapterLuidHigh != requirements_.adapter_luid_high) {
            std::ostringstream message;
            message << "Aurora selected D3D12 adapter " << std::hex
                    << static_cast<uint32_t>(handles.adapterLuidHigh) << ':'
                    << handles.adapterLuidLow << ", but OpenXR requires "
                    << static_cast<uint32_t>(requirements_.adapter_luid_high) << ':'
                    << requirements_.adapter_luid_low;
            return Fail(message.str());
        }

        auto* device = static_cast<ID3D12Device*>(handles.device);
        const D3D_FEATURE_LEVEL minimum =
            static_cast<D3D_FEATURE_LEVEL>(requirements_.minimum_feature_level);
        D3D12_FEATURE_DATA_FEATURE_LEVELS levels{1, &minimum, D3D_FEATURE_LEVEL_1_0_CORE};
        if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &levels,
                                               sizeof(levels))) ||
            levels.MaxSupportedFeatureLevel < minimum) {
            return Fail("Aurora's D3D12 device does not satisfy the OpenXR minimum feature level");
        }

        XrGraphicsBindingD3D12KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
        binding.device = device;
        binding.queue = static_cast<ID3D12CommandQueue*>(handles.queue);
        if (!runtime.CreateSession(&binding)) {
            return Fail("OpenXR rejected Aurora's D3D12 device/queue binding");
        }
        owns_session_ = true;
        aurora_format_ = static_cast<DXGI_FORMAT>(handles.colorDxgiFormat);

        if (!SelectSwapchainFormat() || !CreateSwapchains()) {
            DestroySwapchains();
            runtime.DestroySession();
            owns_session_ = false;
            return false;
        }
        if (runtime.ShouldExit()) {
            DestroySwapchains();
            runtime.DestroySession();
            owns_session_ = false;
            return Fail("OpenXR session became loss-pending while creating D3D12 swapchains");
        }
        if (!aurora_d3d12_enable_stereo_bridge(&Impl::OnAuroraSubmitted, this)) {
            DestroySwapchains();
            runtime.DestroySession();
            owns_session_ = false;
            return Fail("Aurora could not enable its zero-readback D3D12 stereo bridge");
        }
        bridge_enabled_ = true;
        bound_ = true;

        std::ostringstream message;
        message << "OpenXR D3D12 swapchains ready: DXGI format "
                << static_cast<int64_t>(swapchain_format_) << ", eyes "
                << eye_swapchains_[0].width << 'x' << eye_swapchains_[0].height << " / "
                << eye_swapchains_[1].width << 'x' << eye_swapchains_[1].height;
        Log(OpenXRLogLevel::Info, message.str());
        return true;
    }

    OpenXRD3D12BeginStatus BeginFrame(const OpenXRD3D12Presentation& presentation,
                                      OpenXRD3D12Frame& frame, bool acquire_targets = true) {
        frame = {};
        frame.presentation = presentation;
        if (!bound_ || runtime_ == nullptr) {
            Fail("BeginFrame called before the D3D12 backend was bound");
            return OpenXRD3D12BeginStatus::Error;
        }
        if (frame_active_) {
            Fail("BeginFrame called while another OpenXR frame is active");
            return OpenXRD3D12BeginStatus::Error;
        }

        const OpenXRFrameStatus status = runtime_->WaitFrame(frame.xr_frame);
        if (status != OpenXRFrameStatus::Ready) {
            if (status == OpenXRFrameStatus::Error) {
                Fail(BeginStatusOperation(status));
            }
            switch (status) {
            case OpenXRFrameStatus::SessionNotRunning:
                return OpenXRD3D12BeginStatus::SessionNotRunning;
            case OpenXRFrameStatus::ExitRequested:
                return OpenXRD3D12BeginStatus::ExitRequested;
            case OpenXRFrameStatus::Error:
                return OpenXRD3D12BeginStatus::Error;
            case OpenXRFrameStatus::Ready:
                break;
            }
        }
        if (!runtime_->BeginFrame(frame.xr_frame)) {
            Fail("xrBeginFrame failed");
            return OpenXRD3D12BeginStatus::Error;
        }
        frame_active_ = true;
        active_frame_serial_ = frame.xr_frame.serial;
        active_frame_ = frame.xr_frame;

        for (uint32_t eye = 0; eye < kOpenXREyeCount; ++eye) {
            frame.render_width[eye] = eye_swapchains_[eye].width;
            frame.render_height[eye] = eye_swapchains_[eye].height;
        }

        if (!frame.xr_frame.should_render) {
            return OpenXRD3D12BeginStatus::Ready;
        }
        if (!runtime_->LocateViews(frame.xr_frame)) {
            Fail("xrLocateViews failed");
            EndActiveFrameWithoutLayers(frame.xr_frame);
            return OpenXRD3D12BeginStatus::Error;
        }
        active_frame_ = frame.xr_frame;
        if (!frame.xr_frame.views_valid) {
            return OpenXRD3D12BeginStatus::Ready;
        }

        if (acquire_targets && !BeginSubmission(frame)) {
            EndActiveFrameWithoutLayers(frame.xr_frame);
            return OpenXRD3D12BeginStatus::Error;
        }
        return OpenXRD3D12BeginStatus::Ready;
    }

    bool BeginSubmission(OpenXRD3D12Frame& frame) {
        if (!frame_active_ || frame.xr_frame.serial != active_frame_serial_ ||
            !frame.xr_frame.should_render || !frame.xr_frame.views_valid) {
            return Fail("Cannot schedule an OpenXR image outside a renderable display frame");
        }
        {
            std::lock_guard lock(submission_mutex_);
            if (awaiting_token_ != 0) return Fail("An OpenXR GPU job is already pending");
        }
        // Controller polling may establish the menu anchor after acquisition
        // in the synchronous path. Reserve both eyes even on that first frame.
        const uint32_t target_count = kOpenXREyeCount;
        if (target_count == 1) {
            frame.render_width[1] = frame.render_width[0];
            frame.render_height[1] = frame.render_height[0];
        }

        std::array<AuroraD3D12StereoTarget, kOpenXREyeCount> targets{};
        for (uint32_t eye = 0; eye < target_count; ++eye) {
            auto& swapchain = eye_swapchains_[eye];
            if (!AcquireSwapchain(swapchain)) {
                ReleaseAcquiredSwapchains();
                return false;
            }
            targets[eye] = {
                swapchain.images[swapchain.acquired_index].texture,
                swapchain.width,
                swapchain.height,
                static_cast<int64_t>(swapchain_format_),
            };
        }

        {
            std::lock_guard lock(submission_mutex_);
            awaiting_token_ = frame.xr_frame.serial;
            submitted_token_ = 0;
            submission_arrived_ = false;
            submission_success_ = false;
            submission_unsafe_ = false;
        }
        if (!aurora_d3d12_set_stereo_targets(frame.xr_frame.serial, targets.data(), target_count)) {
            {
                std::lock_guard lock(submission_mutex_);
                awaiting_token_ = 0;
            }
            ReleaseAcquiredSwapchains();
            Fail("Aurora rejected the acquired OpenXR D3D12 swapchain target");
            return false;
        }
        frame.expects_gpu_submission = true;
        return true;
    }

    OpenXRD3D12SubmissionStatus WaitForSubmission(const OpenXRD3D12Frame& frame,
                                                  uint32_t timeout_ms) {
        if (!frame.expects_gpu_submission) {
            return OpenXRD3D12SubmissionStatus::Success;
        }
        std::unique_lock lock(submission_mutex_);
        const auto ready = [&] {
            return shutting_down_ ||
                   (submission_arrived_ && submitted_token_ == frame.xr_frame.serial);
        };
        if (timeout_ms == std::numeric_limits<uint32_t>::max()) {
            submission_cv_.wait(lock, ready);
        } else if (!submission_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), ready)) {
            return OpenXRD3D12SubmissionStatus::Timeout;
        }
        if (shutting_down_) {
            return OpenXRD3D12SubmissionStatus::ShuttingDown;
        }
        return submission_success_ ? OpenXRD3D12SubmissionStatus::Success
                                   : OpenXRD3D12SubmissionStatus::Failed;
    }

    bool TryCancelPendingFrame(OpenXRD3D12Frame& frame) {
        {
            std::lock_guard lock(submission_mutex_);
            if (!frame.expects_gpu_submission || frame.xr_frame.serial != awaiting_token_) return false;
        }
        if (!aurora_d3d12_cancel_stereo_targets(frame.xr_frame.serial)) {
            return false;
        }

        // A successful bridge cancellation is serialized against Encode and
        // never generates a callback, so this token has no GPU ownership.
        std::lock_guard lock(submission_mutex_);
        awaiting_token_ = 0;
        submitted_token_ = 0;
        submission_arrived_ = false;
        submission_success_ = false;
        submission_unsafe_ = false;
        frame.expects_gpu_submission = false;
        return true;
    }

    bool CommitSubmission(OpenXRD3D12Frame& frame) {
        if (!frame.expects_gpu_submission || WaitForSubmission(frame, 0) != OpenXRD3D12SubmissionStatus::Success) {
            return Fail("Cannot release an unfinished OpenXR GPU job");
        }
        if (!ReleaseAcquiredSwapchains()) return false;
        cached_frame_ = frame;
        cached_frame_valid_ = true;
        frame.expects_gpu_submission = false;
        std::lock_guard lock(submission_mutex_);
        awaiting_token_ = submitted_token_ = 0;
        submission_arrived_ = submission_success_ = submission_unsafe_ = false;
        return true;
    }

    bool DiscardCanceledSubmission() {
        // Releasing an unwritten acquired image changes the swapchain's latest
        // released image, so its previous cached layer must no longer be used.
        cached_frame_valid_ = false;
        return ReleaseAcquiredSwapchains();
    }

    void InvalidateCachedFrame() { cached_frame_valid_ = false; }

    bool FinishDisplayFrame(OpenXRD3D12Frame& frame, bool show_cached) {
        if (!frame_active_ || frame.xr_frame.serial != active_frame_serial_) {
            return Fail("Invalid asynchronous OpenXR display frame");
        }
        bool ok;
        const auto& image = cached_frame_;
        // Projection layers below use the cached image's render poses. A
        // transient invalid current pose must not turn that valid image black.
        // Still honor the runtime's explicit request to hide application layers.
        if (!show_cached || !cached_frame_valid_ || !frame.xr_frame.should_render) {
            ok = runtime_->EndFrameWithoutLayers(frame.xr_frame);
        } else if (image.presentation.mode == OpenXRD3D12FrameMode::VirtualScreen && !image.presentation.anchored) {
            XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
            quad.space = runtime_->ViewSpace();
            quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            quad.subImage.swapchain = eye_swapchains_[0].handle;
            quad.subImage.imageRect = {{0, 0}, {static_cast<int32_t>(eye_swapchains_[0].width),
                                               static_cast<int32_t>(eye_swapchains_[0].height)}};
            quad.pose.orientation.w = 1.0f;
            quad.pose.position.z = -std::max(0.25f, image.presentation.quad_distance_meters);
            AnchorPanel(quad);
            quad.size.width = std::max(0.25f, image.presentation.quad_width_meters);
            quad.size.height = quad.size.width * float(eye_swapchains_[0].height) / float(eye_swapchains_[0].width);
            const XrCompositionLayerBaseHeader* layers[]{reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad)};
            ok = runtime_->EndFrame(frame.xr_frame, layers, 1);
        } else {
            std::array<XrCompositionLayerProjectionView, kOpenXREyeCount> views{};
            for (uint32_t eye = 0; eye < kOpenXREyeCount; ++eye) {
                views[eye] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
                // These are the poses used to render the image, never the new
                // display pose. The compositor needs them to timewarp correctly.
                views[eye].pose = image.xr_frame.views[eye].pose;
                if (!(image.xr_frame.view_state_flags & XR_VIEW_STATE_POSITION_VALID_BIT))
                    views[eye].pose.position = {0.0f, 0.0f, 0.0f};
                views[eye].fov = image.xr_frame.views[eye].fov;
                views[eye].subImage.swapchain = eye_swapchains_[eye].handle;
                views[eye].subImage.imageRect = {{0, 0}, {static_cast<int32_t>(eye_swapchains_[eye].width),
                                                          static_cast<int32_t>(eye_swapchains_[eye].height)}};
            }
            XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
            projection.space = runtime_->AppSpace();
            projection.viewCount = kOpenXREyeCount;
            projection.views = views.data();
            const XrCompositionLayerBaseHeader* layers[]{reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection)};
            ok = runtime_->EndFrame(frame.xr_frame, layers, 1);
        }
        frame_active_ = false;
        active_frame_serial_ = 0;
        active_frame_ = {};
        // Do not clear the independent submission token or release its images.
        return ok;
    }

    bool FinishFrame(OpenXRD3D12Frame& frame, bool submit_layer) {
        if (!frame_active_ || runtime_ == nullptr ||
            frame.xr_frame.serial != active_frame_serial_) {
            return Fail("FinishFrame received a stale or inactive OpenXR frame token");
        }

        bool submission_unsafe = false;
        {
            std::lock_guard lock(submission_mutex_);
            submission_unsafe = submission_arrived_ &&
                                submitted_token_ == frame.xr_frame.serial &&
                                submission_unsafe_;
        }
        if (submission_unsafe) {
            AbandonAcquiredSwapchains();
            Fail("Aurora's D3D12 stereo submission failed after GPU work may have been queued");
        }
        bool release_ok = ReleaseAcquiredSwapchains();
        const bool position_valid =
            (frame.xr_frame.view_state_flags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0;
        const bool composition_pose_valid =
            frame.presentation.mode == OpenXRD3D12FrameMode::VirtualScreen || position_valid;
        const bool can_submit = submit_layer && release_ok && frame.xr_frame.should_render &&
                                frame.xr_frame.views_valid && frame.expects_gpu_submission &&
                                composition_pose_valid;
        bool end_ok = false;
        if (!runtime_->IsSessionRunning()) {
            // A session that is no longer running needs no compositor frame
            // completion call. Preserve the original backend failure instead
            // of replacing it with a stale-token error.
            end_ok = true;
        } else if (!can_submit) {
            end_ok = runtime_->EndFrameWithoutLayers(frame.xr_frame);
        } else if (frame.presentation.mode == OpenXRD3D12FrameMode::VirtualScreen && !frame.presentation.anchored) {
            XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
            quad.layerFlags = 0;
            quad.space = runtime_->ViewSpace();
            quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            quad.subImage.swapchain = eye_swapchains_[0].handle;
            quad.subImage.imageRect = {{0, 0},
                                       {static_cast<int32_t>(eye_swapchains_[0].width),
                                        static_cast<int32_t>(eye_swapchains_[0].height)}};
            quad.subImage.imageArrayIndex = 0;
            quad.pose.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
            quad.pose.position = {0.0f, 0.0f, -std::max(0.25f, frame.presentation.quad_distance_meters)};
            AnchorPanel(quad);
            quad.size.width = std::max(0.25f, frame.presentation.quad_width_meters);
            quad.size.height = quad.size.width * static_cast<float>(eye_swapchains_[0].height) /
                               static_cast<float>(eye_swapchains_[0].width);
            const XrCompositionLayerBaseHeader* layers[] = {
                reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad)};
            end_ok = runtime_->EndFrame(frame.xr_frame, layers, 1);
        } else {
            std::array<XrCompositionLayerProjectionView, kOpenXREyeCount> views{};
            for (uint32_t eye = 0; eye < kOpenXREyeCount; ++eye) {
                views[eye] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
                views[eye].pose.orientation = frame.xr_frame.views[eye].pose.orientation;
                views[eye].pose.position = position_valid
                                               ? frame.xr_frame.views[eye].pose.position
                                               : XrVector3f{0.0f, 0.0f, 0.0f};
                views[eye].fov = frame.xr_frame.views[eye].fov;
                views[eye].subImage.swapchain = eye_swapchains_[eye].handle;
                views[eye].subImage.imageRect = {
                    {0, 0},
                    {static_cast<int32_t>(eye_swapchains_[eye].width),
                     static_cast<int32_t>(eye_swapchains_[eye].height)}};
                views[eye].subImage.imageArrayIndex = 0;
            }
            XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
            projection.layerFlags = 0;
            projection.space = runtime_->AppSpace();
            projection.viewCount = kOpenXREyeCount;
            projection.views = views.data();
            const XrCompositionLayerBaseHeader* layers[] = {
                reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection)};
            end_ok = runtime_->EndFrame(frame.xr_frame, layers, 1);
        }

        frame_active_ = false;
        active_frame_serial_ = 0;
        active_frame_ = {};
        frame.expects_gpu_submission = false;
        {
            std::lock_guard lock(submission_mutex_);
            awaiting_token_ = 0;
            submission_arrived_ = false;
            submission_success_ = false;
            submission_unsafe_ = false;
        }
        return release_ok && end_ok;
    }

    bool Shutdown() {
        if (shutdown_unsafe_) {
            return false;
        }
        {
            std::lock_guard lock(submission_mutex_);
            shutting_down_ = true;
        }
        submission_cv_.notify_all();

        bool bridge_drained = true;
        if (bridge_enabled_) {
            bridge_drained = aurora_d3d12_disable_stereo_bridge();
            bridge_enabled_ = false;
        }
        if (!bridge_drained) {
            AbandonAcquiredSwapchains();
            shutdown_unsafe_ = true;
            Fail("D3D12 queue completion is unknown; retaining the OpenXR session and graphics owners");
            return false;
        }

        // A queue-tail fence proved that no bridge command can still reference
        // an acquired image. This also makes a conservatively abandoned image
        // releasable after an earlier submission failure.
        AllowAcquiredSwapchainsAfterGpuDrain();
        ReleaseAcquiredSwapchains();
        if (frame_active_ && runtime_ != nullptr) {
            // Shutdown is required to run on the XR owner thread after Aurora's
            // worker is idle, so it is safe to close an abandoned frame here.
            if (runtime_->IsSessionRunning()) {
                runtime_->EndFrameWithoutLayers(active_frame_);
            }
            frame_active_ = false;
            active_frame_serial_ = 0;
            active_frame_ = {};
        }
        DestroySwapchains();
        if (owns_session_ && runtime_ != nullptr) {
            runtime_->DestroySession();
            owns_session_ = false;
        }
        bound_ = false;
        requirements_queried_ = false;
        runtime_ = nullptr;
        return true;
    }

    bool IsBound() const { return bound_; }
    const OpenXRD3D12GraphicsRequirements& GraphicsRequirements() const { return requirements_; }
    int64_t SwapchainFormat() const { return static_cast<int64_t>(swapchain_format_); }
    const std::string& LastError() const { return last_error_; }

private:
    void AnchorPanel(XrCompositionLayerQuad& quad) const {
        if(!runtime_->PanelAnchored()) return;
        const auto anchor=runtime_->PanelOrigin();
        const auto& q=anchor.orientation;
        const auto offset=RotateUiVector(q.x,q.y,q.z,q.w,{0,0,quad.pose.position.z});
        quad.space=runtime_->AppSpace();
        quad.pose=anchor;
        quad.pose.position.x+=offset[0];quad.pose.position.y+=offset[1];quad.pose.position.z+=offset[2];
    }
    bool SelectSwapchainFormat() {
        const auto& formats = runtime_->SwapchainFormats();
        // Aurora's UNORM target contains the gamma-encoded bytes expected by
        // the desktop compositor. OpenXR must declare the compatible sRGB
        // sibling so the headset compositor decodes those raw bytes instead
        // of treating them as linear light (which appears severely washed out).
        const DXGI_FORMAT srgb = SrgbSibling(aurora_format_);
        if (srgb != DXGI_FORMAT_UNKNOWN &&
            std::find(formats.begin(), formats.end(), static_cast<int64_t>(srgb)) !=
                formats.end()) {
            swapchain_format_ = srgb;
            return true;
        }
        const auto exact = std::find(formats.begin(), formats.end(), static_cast<int64_t>(aurora_format_));
        if (exact != formats.end()) {
            swapchain_format_ = aurora_format_;
            return true;
        }
        const auto compatible = std::find_if(formats.begin(), formats.end(), [&](int64_t format) {
            return SameDxgiCopyFamily(aurora_format_, static_cast<DXGI_FORMAT>(format));
        });
        if (compatible == formats.end()) {
            return Fail("OpenXR offered no swapchain format copy-compatible with Aurora's D3D12 color format");
        }
        swapchain_format_ = static_cast<DXGI_FORMAT>(*compatible);
        return true;
    }

    bool CreateSwapchains() {
        for (uint32_t eye = 0; eye < kOpenXREyeCount; ++eye) {
            const auto& view = runtime_->ViewConfiguration()[eye];
            auto& swapchain = eye_swapchains_[eye];
            swapchain.width = view.render_width;
            swapchain.height = view.render_height;

            XrSwapchainCreateInfo create{XR_TYPE_SWAPCHAIN_CREATE_INFO};
            create.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                                XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
            if (IsSrgbFormat(swapchain_format_)) {
                // Matches DolphinXR's raw-UNORM-write/sRGB-compositor path and
                // asks D3D runtimes to expose a typeless-compatible resource.
                create.usageFlags |= XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT;
            }
            create.format = static_cast<int64_t>(swapchain_format_);
            create.sampleCount = 1;
            create.width = swapchain.width;
            create.height = swapchain.height;
            create.faceCount = 1;
            create.arraySize = 1;
            create.mipCount = 1;
            XrResult result = xrCreateSwapchain(runtime_->Session(), &create, &swapchain.handle);
            ObserveResult(result);
            if (XR_FAILED(result)) {
                std::ostringstream message;
                message << "xrCreateSwapchain failed for D3D12 eye " << eye << " (" << result << ')';
                return Fail(message.str());
            }

            uint32_t count = 0;
            result = xrEnumerateSwapchainImages(swapchain.handle, 0, &count, nullptr);
            ObserveResult(result);
            if (XR_FAILED(result) || count == 0) {
                return Fail("OpenXR returned no D3D12 swapchain images");
            }
            swapchain.images.resize(count);
            for (auto& image : swapchain.images) {
                image = {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR};
            }
            result = xrEnumerateSwapchainImages(
                swapchain.handle, count, &count,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(swapchain.images.data()));
            ObserveResult(result);
            if (XR_FAILED(result)) {
                return Fail("xrEnumerateSwapchainImages failed for a D3D12 eye swapchain");
            }
        }
        return true;
    }

    bool AcquireSwapchain(EyeSwapchain& swapchain) {
        XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        XrResult result = xrAcquireSwapchainImage(swapchain.handle, &acquire,
                                                   &swapchain.acquired_index);
        ObserveResult(result);
        if (XR_FAILED(result)) {
            return Fail("xrAcquireSwapchainImage failed for a D3D12 eye swapchain");
        }
        swapchain.acquired = true;
        swapchain.waited = false;
        swapchain.release_forbidden = false;
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait.timeout = XR_INFINITE_DURATION;
        result = xrWaitSwapchainImage(swapchain.handle, &wait);
        ObserveResult(result);
        if (result == XR_TIMEOUT_EXPIRED) {
            return Fail("xrWaitSwapchainImage unexpectedly timed out for a D3D12 eye swapchain");
        }
        if (XR_FAILED(result)) {
            return Fail("xrWaitSwapchainImage failed for a D3D12 eye swapchain");
        }
        swapchain.waited = true;
        if (swapchain.acquired_index >= swapchain.images.size()) {
            return Fail("OpenXR returned an out-of-range D3D12 swapchain image index");
        }
        return true;
    }

    bool ReleaseAcquiredSwapchains() {
        bool success = true;
        for (auto& swapchain : eye_swapchains_) {
            if (!swapchain.acquired || swapchain.handle == XR_NULL_HANDLE) {
                continue;
            }
            if (!swapchain.waited) {
                // OpenXR only permits release after a successful wait. Keep the
                // image acquired and let session teardown destroy the child.
                success = false;
                Log(OpenXRLogLevel::Warning,
                    "cannot release an OpenXR D3D12 image whose wait did not complete");
                continue;
            }
            if (swapchain.release_forbidden) {
                // Aurora reported a failed submission after it may already
                // have queued native D3D12 work. Without a trustworthy fence,
                // xrReleaseSwapchainImage could race that work. Leave the image
                // acquired and let xrDestroySession reclaim the child instead.
                success = false;
                Log(OpenXRLogLevel::Warning,
                    "deferring an OpenXR D3D12 image after an unsafe GPU submission");
                continue;
            }
            XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            const XrResult result = xrReleaseSwapchainImage(swapchain.handle, &release);
            ObserveResult(result);
            if (XR_FAILED(result)) {
                success = false;
                Fail("xrReleaseSwapchainImage failed for a D3D12 eye swapchain");
                continue;
            }
            swapchain.acquired = false;
            swapchain.waited = false;
        }
        return success;
    }

    void AbandonAcquiredSwapchains() noexcept {
        for (auto& swapchain : eye_swapchains_) {
            if (swapchain.acquired) {
                swapchain.release_forbidden = true;
            }
        }
    }

    void AllowAcquiredSwapchainsAfterGpuDrain() noexcept {
        for (auto& swapchain : eye_swapchains_) {
            if (swapchain.acquired) {
                swapchain.release_forbidden = false;
            }
        }
    }

    void DestroySwapchains() {
        for (auto& swapchain : eye_swapchains_) {
            if (swapchain.handle != XR_NULL_HANDLE && !swapchain.acquired) {
                xrDestroySwapchain(swapchain.handle);
            } else if (swapchain.acquired) {
                Log(OpenXRLogLevel::Warning,
                    "D3D12 swapchain still owns an acquired image; deferring its destruction to xrDestroySession");
            }
            swapchain = {};
        }
        swapchain_format_ = DXGI_FORMAT_UNKNOWN;
    }

    void EndActiveFrameWithoutLayers(const OpenXRFrame& frame) {
        if (runtime_ != nullptr && runtime_->IsSessionRunning()) {
            runtime_->EndFrameWithoutLayers(frame);
        }
        frame_active_ = false;
        active_frame_serial_ = 0;
        active_frame_ = {};
    }

    void ObserveResult(XrResult result) noexcept {
        if (runtime_ != nullptr) {
            runtime_->ObserveResult(result);
        }
    }

    static void OnAuroraSubmitted(uint64_t token, bool success, void* userdata) {
        auto* self = static_cast<Impl*>(userdata);
        if (self == nullptr) {
            return;
        }
        {
            std::lock_guard lock(self->submission_mutex_);
            if (token != self->awaiting_token_) {
                return;
            }
            self->submitted_token_ = token;
            self->submission_success_ = success;
            self->submission_arrived_ = true;
            self->submission_unsafe_ = !success;
        }
        self->submission_cv_.notify_all();
    }

    bool Fail(std::string message) {
        last_error_ = std::move(message);
        Log(OpenXRLogLevel::Error, last_error_);
        return false;
    }

    void ClearError() { last_error_.clear(); }

    void Log(OpenXRLogLevel level, std::string_view message) const noexcept {
        if (!logger_) {
            return;
        }
        try {
            logger_(level, message);
        } catch (...) {
        }
    }

    OpenXRRuntime* runtime_ = nullptr;
    OpenXRLogCallback logger_;
    OpenXRD3D12GraphicsRequirements requirements_{};
    std::array<EyeSwapchain, kOpenXREyeCount> eye_swapchains_{};
    DXGI_FORMAT aurora_format_ = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT swapchain_format_ = DXGI_FORMAT_UNKNOWN;
    std::string last_error_;

    std::mutex submission_mutex_;
    std::condition_variable submission_cv_;
    uint64_t awaiting_token_ = 0;
    uint64_t submitted_token_ = 0;
    bool submission_arrived_ = false;
    bool submission_success_ = false;
    bool submission_unsafe_ = false;
    bool shutting_down_ = false;

    uint64_t active_frame_serial_ = 0;
    OpenXRFrame active_frame_{};
    OpenXRD3D12Frame cached_frame_{};
    bool cached_frame_valid_ = false;
    bool requirements_queried_ = false;
    bool owns_session_ = false;
    bool bridge_enabled_ = false;
    bool bound_ = false;
    bool frame_active_ = false;
    bool shutdown_unsafe_ = false;
};

OpenXRD3D12Backend::OpenXRD3D12Backend(OpenXRLogCallback logger)
    : m_impl(std::make_unique<Impl>(std::move(logger))) {}

OpenXRD3D12Backend::~OpenXRD3D12Backend() = default;

bool OpenXRD3D12Backend::QueryGraphicsRequirements(OpenXRRuntime& runtime) {
    return m_impl->QueryGraphicsRequirements(runtime);
}

bool OpenXRD3D12Backend::BindAurora(OpenXRRuntime& runtime) {
    return m_impl->BindAurora(runtime);
}

OpenXRD3D12BeginStatus OpenXRD3D12Backend::BeginFrame(
    const OpenXRD3D12Presentation& presentation, OpenXRD3D12Frame& frame) {
    return m_impl->BeginFrame(presentation, frame);
}

OpenXRD3D12SubmissionStatus OpenXRD3D12Backend::WaitForSubmission(
    const OpenXRD3D12Frame& frame, uint32_t timeout_ms) {
    return m_impl->WaitForSubmission(frame, timeout_ms);
}

bool OpenXRD3D12Backend::TryCancelPendingFrame(OpenXRD3D12Frame& frame) {
    return m_impl->TryCancelPendingFrame(frame);
}

bool OpenXRD3D12Backend::FinishFrame(OpenXRD3D12Frame& frame, bool submit_layer) {
    return m_impl->FinishFrame(frame, submit_layer);
}

OpenXRD3D12BeginStatus OpenXRD3D12Backend::BeginDisplayFrame(const OpenXRD3D12Presentation& presentation,
                                                           OpenXRD3D12Frame& frame) {
    return m_impl->BeginFrame(presentation, frame, false);
}
bool OpenXRD3D12Backend::BeginSubmission(OpenXRD3D12Frame& frame) { return m_impl->BeginSubmission(frame); }
bool OpenXRD3D12Backend::CommitSubmission(OpenXRD3D12Frame& frame) { return m_impl->CommitSubmission(frame); }
bool OpenXRD3D12Backend::DiscardCanceledSubmission() { return m_impl->DiscardCanceledSubmission(); }
bool OpenXRD3D12Backend::FinishDisplayFrame(OpenXRD3D12Frame& frame, bool show_cached) {
    return m_impl->FinishDisplayFrame(frame, show_cached);
}
void OpenXRD3D12Backend::InvalidateCachedFrame() { m_impl->InvalidateCachedFrame(); }

bool OpenXRD3D12Backend::Shutdown() { return m_impl->Shutdown(); }

bool OpenXRD3D12Backend::IsBound() const { return m_impl->IsBound(); }

const OpenXRD3D12GraphicsRequirements& OpenXRD3D12Backend::GraphicsRequirements() const {
    return m_impl->GraphicsRequirements();
}

int64_t OpenXRD3D12Backend::SwapchainFormat() const { return m_impl->SwapchainFormat(); }

const std::string& OpenXRD3D12Backend::LastError() const { return m_impl->LastError(); }

} // namespace mkw::vr

#endif // defined(MKW_ENABLE_OPENXR) && defined(_WIN32)
