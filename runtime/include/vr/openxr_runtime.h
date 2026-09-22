// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "vr/openxr_config.h"
#include "vr/openxr_controller_profiles.h"
#include "vr/vr_controls.h"
#include "vr/quest_input.h"
#include "vr/menu_anchor.h"

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace mkw::vr {

inline constexpr uint32_t kOpenXREyeCount = 2;

enum class OpenXRLogLevel {
    Info,
    Warning,
    Error,
};

using OpenXRLogCallback =
    std::function<void(OpenXRLogLevel level, std::string_view message)>;

struct OpenXRError {
    XrResult result = XR_SUCCESS;
    std::string operation;
    std::string message;

    explicit operator bool() const {
        return XR_FAILED(result) || !message.empty();
    }
};

struct OpenXRViewConfiguration {
    XrViewConfigurationView properties{XR_TYPE_VIEW_CONFIGURATION_VIEW};
    uint32_t render_width = 0;
    uint32_t render_height = 0;
};

struct OpenXRRuntimeInfo {
    std::string runtime_name;
    XrVersion runtime_version = 0;
    std::string system_name;
    uint32_t vendor_id = 0;
    uint32_t max_layer_count = 0;
    bool supports_orientation_tracking = false;
    bool supports_position_tracking = false;
};

struct OpenXRReferenceSpaceChange {
    uint64_t serial = 0;
    XrReferenceSpaceType type = XR_REFERENCE_SPACE_TYPE_LOCAL;
    XrTime change_time = 0;
    bool pose_in_previous_space_valid = false;
    XrPosef pose_in_previous_space{{0.0f, 0.0f, 0.0f, 1.0f},
                                   {0.0f, 0.0f, 0.0f}};
};

// A frame token is produced by WaitFrame and must be passed back to the other
// frame functions. This prevents an old pose/timestamp from being paired with a
// newer compositor frame.
struct OpenXRFrame {
    uint64_t serial = 0;
    XrTime predicted_display_time = 0;
    XrDuration predicted_display_period = 0;
    bool should_render = false;
    bool views_valid = false;
    XrViewStateFlags view_state_flags = 0;
    std::array<XrView, kOpenXREyeCount> views{};
};

enum class OpenXREventStatus {
    Continue,
    ExitRequested,
    Error,
};

enum class OpenXRFrameStatus {
    Ready,
    SessionNotRunning,
    ExitRequested,
    Error,
};

// Owns the backend-neutral OpenXR object graph and session state machine.
//
// A graphics backend performs its requirements/device work after Initialize(),
// then passes its XrGraphicsBinding* structure to CreateSession(). Swapchains are
// intentionally not owned here: D3D12 and Vulkan need different image wrapping
// and synchronization strategies.
//
// OpenXRRuntime is not internally synchronized. PollEvents and all frame calls
// must be serialized by one XR-owner thread. Read-only handles may be handed to
// the graphics integration only while the corresponding object is alive.
class OpenXRRuntime final {
public:
    explicit OpenXRRuntime(OpenXRLogCallback logger = {});
    ~OpenXRRuntime();

    OpenXRRuntime(const OpenXRRuntime&) = delete;
    OpenXRRuntime& operator=(const OpenXRRuntime&) = delete;
    OpenXRRuntime(OpenXRRuntime&&) = delete;
    OpenXRRuntime& operator=(OpenXRRuntime&&) = delete;

    // Creates the instance, resolves the HMD system, and enumerates the stereo
    // view configuration. Returns false without terminating the application;
    // the caller should continue in non-VR mode.
    bool Initialize(const OpenXRConfig& config);

    // graphics_binding is the address of an XrGraphicsBinding* structure and is
    // forwarded through XrSessionCreateInfo::next. It must remain valid only for
    // the duration of this call.
    bool CreateSession(const void* graphics_binding);

    // Loads an extension entry point from this instance. Backends use this to
    // query graphics requirements before Aurora creates/selects its device.
    bool GetInstanceProcAddress(const char* name, PFN_xrVoidFunction* function);

    template <typename Function>
    bool LoadFunction(const char* name, Function* function) {
        static_assert(std::is_pointer_v<Function>,
                      "OpenXR PFN typedefs must be pointer types");
        return GetInstanceProcAddress(
            name, reinterpret_cast<PFN_xrVoidFunction*>(function));
    }

    // Requests an orderly exit when running, processes STOPPING for a bounded
    // period, then releases spaces and the session. The instance/system remain
    // available so a backend session can be recreated.
    void DestroySession();

    // Releases every owned object. Safe to call repeatedly.
    void Shutdown();

    // Polls every currently queued event and owns xrBeginSession/xrEndSession
    // transitions for READY/STOPPING. EXITING and LOSS_PENDING are surfaced to
    // the application rather than terminating it here.
    OpenXREventStatus PollEvents();
    bool RequestExitSession();

    // Graphics backends call OpenXR entry points directly for swapchain work.
    // Feed their results back here so positive loss-pending and negative
    // session/instance-loss results update the central lifecycle state.
    void ObserveResult(XrResult result) noexcept;

    // Explicit frame protocol. A successful WaitFrame must be followed by
    // BeginFrame and EndFrame using the same token. LocateViews is optional when
    // should_render is false; it is otherwise normally called after BeginFrame.
    OpenXRFrameStatus WaitFrame(OpenXRFrame& frame);
    bool BeginFrame(const OpenXRFrame& frame);
    bool LocateViews(OpenXRFrame& frame);
    bool EndFrame(
        const OpenXRFrame& frame,
        const XrCompositionLayerBaseHeader* const* layers,
        uint32_t layer_count);
    bool EndFrame(
        const OpenXRFrame& frame,
        const std::vector<const XrCompositionLayerBaseHeader*>& layers);
    bool EndFrameWithoutLayers(const OpenXRFrame& frame);

    // Recreates the application reference space with a caller-provided offset.
    // This is the application-side recenter primitive; call only between frames.
    bool ResetAppSpace(const XrPosef& pose_in_reference_space);

    // Called only on the XR owner thread, at the predicted display time.
    void PollControllers(XrTime time, const OpenXRFrame* frame = nullptr);
    void RequestDisplayRefreshRate(float hz);
    void PulseGrip(size_t hand, bool grabbed);
    bool ConsumeCameraClick() { const bool click = m_camera_clicked; m_camera_clicked = false; return click; }
    bool LeftGripValid() const { return m_left_grip_valid; }
    const XrPosef& LeftGripPose() const { return m_left_grip_pose; }
    bool RightGripValid() const { return m_right_grip_valid; }
    const XrPosef& RightGripPose() const { return m_right_grip_pose; }
    float HandSqueeze(size_t hand) const { return m_squeeze_values[hand]; }
    void PublishDrivingInput(bool cockpit, float steering, bool held, float angle);

    bool IsInitialized() const { return m_instance != XR_NULL_HANDLE; }
    bool HasSession() const { return m_session != XR_NULL_HANDLE; }
    bool IsSessionRunning() const { return m_session_running; }
    bool IsSessionFocused() const {
        return m_session_state == XR_SESSION_STATE_FOCUSED;
    }
    bool IsSessionVisible() const {
        return m_session_state == XR_SESSION_STATE_VISIBLE ||
               m_session_state == XR_SESSION_STATE_FOCUSED;
    }
    bool ShouldExit() const {
        return m_exit_requested || m_session_loss_pending || m_instance_loss_pending;
    }

    XrInstance Instance() const { return m_instance; }
    XrSystemId SystemId() const { return m_system_id; }
    XrSession Session() const { return m_session; }
    XrSpace AppSpace() const { return m_app_space; }
    XrSpace ViewSpace() const { return m_view_space; }
    bool PanelAnchored() const { return m_panel_anchored; }
    XrPosef PanelOrigin() const { return m_panel_origin; }
    XrSessionState SessionState() const { return m_session_state; }
    uint64_t SessionRunSerial() const { return m_session_run_serial; }
    XrReferenceSpaceType AppSpaceType() const { return m_app_space_type; }
    XrEnvironmentBlendMode EnvironmentBlendMode() const { return m_blend_mode; }

    const OpenXRConfig& Config() const { return m_config; }
    const OpenXRRuntimeInfo& RuntimeInfo() const { return m_runtime_info; }
    const std::array<OpenXRViewConfiguration, kOpenXREyeCount>& ViewConfiguration() const {
        return m_view_configuration;
    }
    const std::vector<std::string>& EnabledExtensions() const {
        return m_enabled_extensions;
    }
    const std::vector<int64_t>& SwapchainFormats() const { return m_swapchain_formats; }
    const OpenXRReferenceSpaceChange& LastReferenceSpaceChange() const {
        return m_reference_space_change;
    }
    // Consumes every queued application-space change whose effective time is
    // no later than display_time. Multiple future recenter events are retained
    // independently rather than overwriting one another.
    bool ConsumeAppSpaceChangesThrough(XrTime display_time);
    const OpenXRError& LastError() const { return m_last_error; }

private:
    bool m_panel_anchored = false;
    MenuAnchorStability m_panel_stability;
    XrPosef m_panel_origin{{0,0,0,1},{0,0,0}};
    enum class FramePhase {
        Idle,
        Waited,
        Begun,
    };

    bool CreateControllerActions();
    void DestroyControllerActions();
    XrActionSet m_controller_actions = XR_NULL_HANDLE;
    XrAction m_camera_action = XR_NULL_HANDLE;
    XrAction m_grip_action = XR_NULL_HANDLE;
    XrAction m_right_grip_action = XR_NULL_HANDLE;
    std::array<XrAction, 2> m_squeeze_actions{};
    std::array<XrAction, 2> m_haptic_actions{};
    XrAction m_item_trigger_action = XR_NULL_HANDLE;
    XrSpace m_right_grip_space = XR_NULL_HANDLE;
    XrPosef m_right_grip_pose{};
    bool m_right_grip_valid = false;
    std::array<float, 2> m_squeeze_values{};
    QuestInput m_raw_input{};
    float m_item_trigger = 0;
    bool m_cockpit_input = false, m_wheel_held = false;
    float m_wheel_steering = 0;
    float m_wheel_angle = 0;
    std::array<XrAction, kOpenXRControllerActionCount> m_game_actions{};
    XrSpace m_grip_space = XR_NULL_HANDLE;
    XrPosef m_left_grip_pose{};
    bool m_left_grip_valid = false;
    bool m_camera_clicked = false;
    CameraClickLatch m_camera_latch;
    XrAction m_steam_trick_action=XR_NULL_HANDLE;
    XrAction m_ui_pointer_action=XR_NULL_HANDLE;
    XrSpace m_ui_pointer_space=XR_NULL_HANDLE;
    XrAction m_ui_left_pointer_action=XR_NULL_HANDLE;
    XrSpace m_ui_left_pointer_space=XR_NULL_HANDLE;
    SteamVrTrickPause m_steam_trick_pause;

    bool EnumerateInstanceCapabilities();
    bool CreateInstance();
    bool InitializeSystem();
    bool EnumerateViewConfiguration();
    bool SelectEnvironmentBlendMode();
    bool CreateReferenceSpaces();
    bool EnumerateSwapchainFormats();
    bool HandleSessionStateChanged(const XrEventDataSessionStateChanged& event);
    bool IsFrameTokenCurrent(const OpenXRFrame& frame, FramePhase expected) const;
    void ResetFrameState();
    void DestroyReferenceSpaces();
    void ResetSessionState();
    void ResetInstanceState();

    bool Check(XrResult result, std::string_view operation);
    bool Fail(XrResult result, std::string_view operation, std::string_view detail);
    void ClearError();
    void Log(OpenXRLogLevel level, std::string_view message) const noexcept;
    std::string ResultString(XrResult result) const;

    OpenXRLogCallback m_logger;
    OpenXRConfig m_config;
    OpenXRError m_last_error;

    XrInstance m_instance = XR_NULL_HANDLE;
    XrSystemId m_system_id = XR_NULL_SYSTEM_ID;
    XrSession m_session = XR_NULL_HANDLE;
    XrSpace m_app_space = XR_NULL_HANDLE;
    XrSpace m_view_space = XR_NULL_HANDLE;

    XrSessionState m_session_state = XR_SESSION_STATE_UNKNOWN;
    XrReferenceSpaceType m_app_space_type = XR_REFERENCE_SPACE_TYPE_LOCAL;
    XrEnvironmentBlendMode m_blend_mode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    bool m_session_running = false;
    bool m_exit_requested = false;
    bool m_session_loss_pending = false;
    bool m_instance_loss_pending = false;
    bool m_shutting_down_session = false;

    FramePhase m_frame_phase = FramePhase::Idle;
    uint64_t m_session_run_serial = 0;
    uint64_t m_next_frame_serial = 1;
    uint64_t m_active_frame_serial = 0;
    XrTime m_active_frame_display_time = 0;

    OpenXRRuntimeInfo m_runtime_info;
    std::array<OpenXRViewConfiguration, kOpenXREyeCount> m_view_configuration{};
    std::vector<std::string> m_available_extensions;
    std::vector<std::string> m_available_api_layers;
    std::vector<std::string> m_enabled_extensions;
    std::vector<std::string> m_enabled_api_layers;
    std::vector<XrReferenceSpaceType> m_supported_reference_spaces;
    std::vector<XrEnvironmentBlendMode> m_supported_blend_modes;
    std::vector<int64_t> m_swapchain_formats;
    OpenXRReferenceSpaceChange m_reference_space_change;
    std::vector<OpenXRReferenceSpaceChange> m_pending_app_space_changes;
};

} // namespace mkw::vr
