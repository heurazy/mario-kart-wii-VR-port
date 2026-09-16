// SPDX-License-Identifier: GPL-3.0-or-later

// The runtime source tree is globbed even in explicitly non-VR builds. Keep
// this translation unit dependency-free in that configuration; callers that
// include the public OpenXR headers must use the same feature guard.
#if defined(MKW_ENABLE_OPENXR)

#include "vr/openxr_runtime.h"
#include "vr/mkw_vr_policy.h"
#include "vr/openxr_android.h"
#include "vr/quest_input.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>
#include <thread>
#include <utility>

namespace mkw::vr {
namespace {

template <typename T>
bool Contains(const std::vector<T>& values, const T& value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

void CopyOpenXRName(char* destination, size_t destination_size, std::string_view source) {
    if (destination_size == 0) {
        return;
    }
    const size_t length = std::min(destination_size - 1, source.size());
    std::memcpy(destination, source.data(), length);
    destination[length] = '\0';
}

XrPosef IdentityPose() {
    return {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
}

bool IsFinitePositive(float value) noexcept {
    // mkw_runtime_common uses -ffast-math, where std::isfinite may be folded
    // away. Check the IEEE-754 representation before using the scale in
    // rounding and integer conversion.
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return (bits & 0x80000000u) == 0 && (bits & 0x7fffffffu) != 0 &&
           (bits & 0x7f800000u) != 0x7f800000u;
}

uint32_t ScaledDimension(uint32_t recommended, uint32_t maximum, float scale) {
    const double scaled = std::round(static_cast<double>(recommended) *
                                     static_cast<double>(scale));
    const double clamped = std::clamp(
        scaled, 1.0, static_cast<double>(std::max(maximum, 1u)));
    return static_cast<uint32_t>(clamped);
}

const char* SessionStateName(XrSessionState state) {
    switch (state) {
    case XR_SESSION_STATE_UNKNOWN:
        return "UNKNOWN";
    case XR_SESSION_STATE_IDLE:
        return "IDLE";
    case XR_SESSION_STATE_READY:
        return "READY";
    case XR_SESSION_STATE_SYNCHRONIZED:
        return "SYNCHRONIZED";
    case XR_SESSION_STATE_VISIBLE:
        return "VISIBLE";
    case XR_SESSION_STATE_FOCUSED:
        return "FOCUSED";
    case XR_SESSION_STATE_STOPPING:
        return "STOPPING";
    case XR_SESSION_STATE_LOSS_PENDING:
        return "LOSS_PENDING";
    case XR_SESSION_STATE_EXITING:
        return "EXITING";
    default:
        return "INVALID";
    }
}

} // namespace

OpenXRRuntime::OpenXRRuntime(OpenXRLogCallback logger)
    : m_logger(std::move(logger)) {}

OpenXRRuntime::~OpenXRRuntime() {
    Shutdown();
}

bool OpenXRRuntime::Initialize(const OpenXRConfig& config) {
    ClearError();
    if (IsInitialized()) {
        return Fail(XR_ERROR_CALL_ORDER_INVALID, "Initialize",
                    "OpenXR is already initialized");
    }
    if (config.application_name.empty()) {
        return Fail(XR_ERROR_VALIDATION_FAILURE, "Initialize",
                    "application_name must not be empty");
    }
    if (!IsFinitePositive(config.resolution_scale)) {
        return Fail(XR_ERROR_VALIDATION_FAILURE, "Initialize",
                    "resolution_scale must be finite and greater than zero");
    }

    // On Android the loader is unusable - including the extension enumeration
    // below - until it has been handed the process's JavaVM and Activity. On
    // every other platform this succeeds without doing anything.
    std::string loader_error;
    if (!OpenXRAndroidInitializeLoader(&loader_error)) {
        return Fail(XR_ERROR_INITIALIZATION_FAILED, "xrInitializeLoaderKHR",
                    loader_error);
    }

    m_config = config;
    if (!EnumerateInstanceCapabilities() || !CreateInstance() ||
        !InitializeSystem() || !EnumerateViewConfiguration() ||
        !SelectEnvironmentBlendMode()) {
        if (m_instance != XR_NULL_HANDLE) {
            xrDestroyInstance(m_instance);
        }
        ResetInstanceState();
        return false;
    }

    std::ostringstream message;
    message << "OpenXR initialized: runtime '" << m_runtime_info.runtime_name
            << "', system '" << m_runtime_info.system_name << "'";
    Log(OpenXRLogLevel::Info, message.str());
    return true;
}

bool OpenXRRuntime::EnumerateInstanceCapabilities() {
    uint32_t extension_count = 0;
    if (!Check(xrEnumerateInstanceExtensionProperties(
                   nullptr, 0, &extension_count, nullptr),
               "xrEnumerateInstanceExtensionProperties(count)")) {
        return false;
    }

    std::vector<XrExtensionProperties> extension_properties(
        extension_count, XrExtensionProperties{XR_TYPE_EXTENSION_PROPERTIES});
    if (extension_count != 0 &&
        !Check(xrEnumerateInstanceExtensionProperties(
                   nullptr, extension_count, &extension_count,
                   extension_properties.data()),
               "xrEnumerateInstanceExtensionProperties")) {
        return false;
    }

    m_available_extensions.clear();
    m_available_extensions.reserve(extension_count);
    for (const XrExtensionProperties& extension : extension_properties) {
        m_available_extensions.emplace_back(extension.extensionName);
    }

    uint32_t layer_count = 0;
    if (!Check(xrEnumerateApiLayerProperties(0, &layer_count, nullptr),
               "xrEnumerateApiLayerProperties(count)")) {
        return false;
    }
    std::vector<XrApiLayerProperties> layer_properties(
        layer_count, XrApiLayerProperties{XR_TYPE_API_LAYER_PROPERTIES});
    if (layer_count != 0 &&
        !Check(xrEnumerateApiLayerProperties(
                   layer_count, &layer_count, layer_properties.data()),
               "xrEnumerateApiLayerProperties")) {
        return false;
    }

    m_available_api_layers.clear();
    m_available_api_layers.reserve(layer_count);
    for (const XrApiLayerProperties& layer : layer_properties) {
        m_available_api_layers.emplace_back(layer.layerName);
    }
    return true;
}

void OpenXRRuntime::RequestDisplayRefreshRate(float hz) {
    if (!HasSession() || !Contains(m_enabled_extensions, std::string(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME))) {
        Log(OpenXRLogLevel::Info, "Display refresh is controlled by the PC VR runtime; select the desired rate there");
        return;
    }
    PFN_xrEnumerateDisplayRefreshRatesFB enumerate = nullptr;
    PFN_xrRequestDisplayRefreshRateFB request = nullptr;
    if (XR_FAILED(xrGetInstanceProcAddr(m_instance, "xrEnumerateDisplayRefreshRatesFB", reinterpret_cast<PFN_xrVoidFunction*>(&enumerate))) ||
        XR_FAILED(xrGetInstanceProcAddr(m_instance, "xrRequestDisplayRefreshRateFB", reinterpret_cast<PFN_xrVoidFunction*>(&request))) || !enumerate || !request) return;
    uint32_t count = 0;
    if (XR_FAILED(enumerate(m_session, 0, &count, nullptr)) || count == 0) return;
    std::vector<float> rates(count);
    if (XR_FAILED(enumerate(m_session, count, &count, rates.data()))) return;
    // A standalone headset advertises a fixed set of panel rates and rejects
    // anything else, so an unavailable preference is resolved to the nearest
    // rate at or below it rather than abandoned. That also lets hz = 0 mean
    // "use this headset's default", which is what the standalone target wants
    // before the user has expressed a preference.
    const float selected = QuestSelectRefreshRate(rates.data(), rates.size(),
                                                  m_device_profile, hz);
    if (selected <= 0.0f) {
        Log(OpenXRLogLevel::Info, "The runtime advertises no display refresh rates; retaining its refresh rate");
        return;
    }
    if (hz > 0.0f && selected != hz) {
        Log(OpenXRLogLevel::Info,
            "Requested " + std::to_string(hz) + " Hz is not exposed by this headset; using " +
                std::to_string(selected) + " Hz");
    }
    const auto result = request(m_session, selected);
    Log(XR_SUCCEEDED(result) ? OpenXRLogLevel::Info : OpenXRLogLevel::Warning,
        XR_SUCCEEDED(result) ? "Requested display refresh: " + std::to_string(selected) + " Hz" : "The runtime rejected the refresh request");
}

bool OpenXRRuntime::CreateInstance() {
    m_enabled_extensions.clear();
    if (Contains(m_available_extensions, std::string(XR_EXT_HAND_TRACKING_EXTENSION_NAME)) &&
        Contains(m_available_extensions, std::string(XR_FB_HAND_TRACKING_MESH_EXTENSION_NAME))) {
        m_enabled_extensions.emplace_back(XR_EXT_HAND_TRACKING_EXTENSION_NAME);
        m_enabled_extensions.emplace_back(XR_FB_HAND_TRACKING_MESH_EXTENSION_NAME);
    }
    if (Contains(m_available_extensions, std::string(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME))) {
        m_enabled_extensions.emplace_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    }
    // The Android instance-creation extension is a property of the platform,
    // not of the graphics backend, so it is added here rather than being left
    // to every caller that builds an OpenXRConfig. Both lists are empty on a
    // desktop build.
    for (const std::string& extension : OpenXRAndroidRequiredExtensions()) {
        if (!Contains(m_available_extensions, extension)) {
            return Fail(XR_ERROR_EXTENSION_NOT_PRESENT, "xrCreateInstance",
                        "the Android OpenXR loader does not advertise the "
                        "required extension: " + extension);
        }
        if (!Contains(m_enabled_extensions, extension)) {
            m_enabled_extensions.push_back(extension);
        }
    }
    for (const std::string& extension : OpenXRAndroidOptionalExtensions()) {
        if (Contains(m_available_extensions, extension) &&
            !Contains(m_enabled_extensions, extension)) {
            m_enabled_extensions.push_back(extension);
        }
    }
    for (const std::string& extension : m_config.required_extensions) {
        if (!Contains(m_available_extensions, extension)) {
            return Fail(XR_ERROR_EXTENSION_NOT_PRESENT, "xrCreateInstance",
                        "required extension is unavailable: " + extension);
        }
        if (!Contains(m_enabled_extensions, extension)) {
            m_enabled_extensions.push_back(extension);
        }
    }
    for (const std::string& extension : m_config.optional_extensions) {
        if (Contains(m_available_extensions, extension) &&
            !Contains(m_enabled_extensions, extension)) {
            m_enabled_extensions.push_back(extension);
        }
    }

    m_enabled_api_layers.clear();
    for (const std::string& layer : m_config.required_api_layers) {
        if (!Contains(m_available_api_layers, layer)) {
            return Fail(XR_ERROR_API_LAYER_NOT_PRESENT, "xrCreateInstance",
                        "required API layer is unavailable: " + layer);
        }
        if (!Contains(m_enabled_api_layers, layer)) {
            m_enabled_api_layers.push_back(layer);
        }
    }
    for (const std::string& layer : m_config.optional_api_layers) {
        if (Contains(m_available_api_layers, layer) &&
            !Contains(m_enabled_api_layers, layer)) {
            m_enabled_api_layers.push_back(layer);
        }
    }

    std::vector<const char*> extension_names;
    extension_names.reserve(m_enabled_extensions.size());
    for (const std::string& extension : m_enabled_extensions) {
        extension_names.push_back(extension.c_str());
    }
    std::vector<const char*> layer_names;
    layer_names.reserve(m_enabled_api_layers.size());
    for (const std::string& layer : m_enabled_api_layers) {
        layer_names.push_back(layer.c_str());
    }

    XrInstanceCreateInfo create_info{XR_TYPE_INSTANCE_CREATE_INFO};
    // Android needs the JavaVM/Activity pair a second time here. The pointee
    // has static storage duration; nullptr on every other platform leaves the
    // chain exactly as it was.
    create_info.next = OpenXRAndroidInstanceCreateInfoChain();
    CopyOpenXRName(create_info.applicationInfo.applicationName,
                   XR_MAX_APPLICATION_NAME_SIZE, m_config.application_name);
    create_info.applicationInfo.applicationVersion = m_config.application_version;
    CopyOpenXRName(create_info.applicationInfo.engineName,
                   XR_MAX_ENGINE_NAME_SIZE, m_config.engine_name);
    create_info.applicationInfo.engineVersion = m_config.engine_version;
    create_info.applicationInfo.apiVersion = m_config.api_version;
    create_info.enabledExtensionCount =
        static_cast<uint32_t>(extension_names.size());
    create_info.enabledExtensionNames = extension_names.data();
    create_info.enabledApiLayerCount = static_cast<uint32_t>(layer_names.size());
    create_info.enabledApiLayerNames = layer_names.data();

    if (!Check(xrCreateInstance(&create_info, &m_instance), "xrCreateInstance")) {
        return false;
    }

    XrInstanceProperties instance_properties{XR_TYPE_INSTANCE_PROPERTIES};
    if (!Check(xrGetInstanceProperties(m_instance, &instance_properties),
               "xrGetInstanceProperties")) {
        return false;
    }
    m_runtime_info.runtime_name = instance_properties.runtimeName;
    m_runtime_info.runtime_version = instance_properties.runtimeVersion;
    return true;
}

bool OpenXRRuntime::InitializeSystem() {
    XrSystemGetInfo get_info{XR_TYPE_SYSTEM_GET_INFO};
    get_info.formFactor = m_config.form_factor;
    if (!Check(xrGetSystem(m_instance, &get_info, &m_system_id), "xrGetSystem")) {
        return false;
    }

    XrSystemProperties properties{XR_TYPE_SYSTEM_PROPERTIES};
    if (!Check(xrGetSystemProperties(m_instance, m_system_id, &properties),
               "xrGetSystemProperties")) {
        return false;
    }
    m_runtime_info.system_name = properties.systemName;
    m_runtime_info.vendor_id = properties.vendorId;
    m_runtime_info.max_layer_count = properties.graphicsProperties.maxLayerCount;
    m_runtime_info.supports_orientation_tracking =
        properties.trackingProperties.orientationTracking == XR_TRUE;
    m_runtime_info.supports_position_tracking =
        properties.trackingProperties.positionTracking == XR_TRUE;
    return true;
}

bool OpenXRRuntime::EnumerateViewConfiguration() {
    uint32_t view_count = 0;
    if (!Check(xrEnumerateViewConfigurationViews(
                   m_instance, m_system_id, m_config.view_configuration,
                   0, &view_count, nullptr),
               "xrEnumerateViewConfigurationViews(count)")) {
        return false;
    }
    if (view_count != kOpenXREyeCount) {
        std::ostringstream detail;
        detail << "PRIMARY_STEREO must expose exactly " << kOpenXREyeCount
               << " views, runtime returned " << view_count;
        return Fail(XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED,
                    "xrEnumerateViewConfigurationViews", detail.str());
    }

    std::array<XrViewConfigurationView, kOpenXREyeCount> properties{};
    for (XrViewConfigurationView& property : properties) {
        property.type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
    }
    if (!Check(xrEnumerateViewConfigurationViews(
                   m_instance, m_system_id, m_config.view_configuration,
                   view_count, &view_count, properties.data()),
               "xrEnumerateViewConfigurationViews")) {
        return false;
    }

    for (uint32_t eye = 0; eye < kOpenXREyeCount; ++eye) {
        OpenXRViewConfiguration& destination = m_view_configuration[eye];
        destination.properties = properties[eye];
        destination.render_width = ScaledDimension(
            properties[eye].recommendedImageRectWidth,
            properties[eye].maxImageRectWidth, m_config.resolution_scale);
        destination.render_height = ScaledDimension(
            properties[eye].recommendedImageRectHeight,
            properties[eye].maxImageRectHeight, m_config.resolution_scale);
    }
    return true;
}

bool OpenXRRuntime::SelectEnvironmentBlendMode() {
    uint32_t blend_mode_count = 0;
    if (!Check(xrEnumerateEnvironmentBlendModes(
                   m_instance, m_system_id, m_config.view_configuration,
                   0, &blend_mode_count, nullptr),
               "xrEnumerateEnvironmentBlendModes(count)")) {
        return false;
    }
    if (blend_mode_count == 0) {
        return Fail(XR_ERROR_ENVIRONMENT_BLEND_MODE_UNSUPPORTED,
                    "xrEnumerateEnvironmentBlendModes",
                    "runtime returned no environment blend modes");
    }

    m_supported_blend_modes.resize(blend_mode_count);
    if (!Check(xrEnumerateEnvironmentBlendModes(
                   m_instance, m_system_id, m_config.view_configuration,
                   blend_mode_count, &blend_mode_count,
                   m_supported_blend_modes.data()),
               "xrEnumerateEnvironmentBlendModes")) {
        return false;
    }
    m_supported_blend_modes.resize(blend_mode_count);

    if (Contains(m_supported_blend_modes, m_config.preferred_blend_mode)) {
        m_blend_mode = m_config.preferred_blend_mode;
        return true;
    }
    if (Contains(m_supported_blend_modes, XR_ENVIRONMENT_BLEND_MODE_OPAQUE)) {
        m_blend_mode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    } else {
        m_blend_mode = m_supported_blend_modes.front();
    }
    Log(OpenXRLogLevel::Warning,
        "preferred environment blend mode is unavailable; using runtime fallback");
    return true;
}

bool OpenXRRuntime::CreateSession(const void* graphics_binding) {
    ClearError();
    if (!IsInitialized()) {
        return Fail(XR_ERROR_CALL_ORDER_INVALID, "CreateSession",
                    "Initialize must succeed first");
    }
    if (m_instance_loss_pending) {
        return Fail(XR_ERROR_INSTANCE_LOST, "CreateSession",
                    "the OpenXR instance is loss-pending");
    }
    if (m_session_loss_pending) {
        return Fail(XR_ERROR_SESSION_LOST, "CreateSession",
                    "session loss requires a full OpenXR reinitialization");
    }
    if (HasSession()) {
        return Fail(XR_ERROR_CALL_ORDER_INVALID, "CreateSession",
                    "a session already exists");
    }
    if (graphics_binding == nullptr) {
        return Fail(XR_ERROR_GRAPHICS_DEVICE_INVALID, "CreateSession",
                    "graphics binding must not be null");
    }

    XrSessionCreateInfo create_info{XR_TYPE_SESSION_CREATE_INFO};
    create_info.next = graphics_binding;
    create_info.systemId = m_system_id;
    if (!Check(xrCreateSession(m_instance, &create_info, &m_session),
               "xrCreateSession")) {
        m_session = XR_NULL_HANDLE;
        return false;
    }

    if (!CreateReferenceSpaces() || !EnumerateSwapchainFormats()) {
        DestroyReferenceSpaces();
        xrDestroySession(m_session);
        m_session = XR_NULL_HANDLE;
        ResetSessionState();
        return false;
    }

    if (!CreateControllerActions()) {
        Log(OpenXRLogLevel::Warning, "VR controller actions unavailable; continuing without hand HUD/camera button");
        DestroyControllerActions();
    }
    m_session_state = XR_SESSION_STATE_UNKNOWN;
    m_exit_requested = false;
    ApplyStandaloneDeviceProfile();
    Log(OpenXRLogLevel::Info,
        "OpenXR session created; waiting for the runtime READY event");
    return true;
}

const QuestDeviceProfile& OpenXRRuntime::DeviceProfile() const {
    return m_device_profile;
}

void OpenXRRuntime::ApplyStandaloneDeviceProfile() {
    // The profile is resolved on every platform so the diagnostics tab can name
    // the headset, but only a standalone build owns the governor: on a tethered
    // session the CPU/GPU levels belong to the streaming compositor.
    m_device_profile = QuestProfileFromSystemName(m_runtime_info.system_name,
                                                  OpenXRAndroidTransport());

    std::ostringstream detected;
    detected << "VR device profile: " << m_device_profile.display_name
             << (m_device_profile.recognized ? "" : " (unrecognized; using "
                                                    "conservative defaults)");
    Log(OpenXRLogLevel::Info, detected.str());

    if (!OpenXRAndroidIsStandaloneBuild()) {
        return;
    }

    std::string error;
    if (!OpenXRAndroidApplyPerformanceLevels(m_instance, m_session,
                                             m_device_profile, &error)) {
        // Not fatal: the runtime keeps its own governor behaviour and the game
        // still renders, just with less headroom.
        Log(OpenXRLogLevel::Warning,
            "Could not raise the headset performance levels: " + error);
    }
}

bool OpenXRRuntime::CreateControllerActions() {
    XrActionSetCreateInfo set{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strcpy(set.actionSetName, "vr_controls");
    std::strcpy(set.localizedActionSetName, "VR camera and hand HUD");
    if (XR_FAILED(xrCreateActionSet(m_instance, &set, &m_controller_actions))) return false;
    XrActionCreateInfo action{XR_TYPE_ACTION_CREATE_INFO};
    action.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    std::strcpy(action.actionName, "cycle_camera");
    std::strcpy(action.localizedActionName, "Cycle camera");
    if (XR_FAILED(xrCreateAction(m_controller_actions, &action, &m_camera_action))) return false;
    action.actionType = XR_ACTION_TYPE_POSE_INPUT;
    std::strcpy(action.actionName, "left_grip");
    std::strcpy(action.localizedActionName, "Left hand HUD");
    if (XR_FAILED(xrCreateAction(m_controller_actions, &action, &m_grip_action))) return false;
    std::strcpy(action.actionName, "right_grip");
    std::strcpy(action.localizedActionName, "Right hand");
    if (XR_FAILED(xrCreateAction(m_controller_actions, &action, &m_right_grip_action))) return false;
    action.actionType = XR_ACTION_TYPE_VIBRATION_OUTPUT;
    for (size_t hand = 0; hand < 2; ++hand) {
        std::strcpy(action.actionName, hand ? "wheel_haptic_right" : "wheel_haptic_left");
        std::strcpy(action.localizedActionName, hand ? "Right wheel feedback" : "Left wheel feedback");
        if (XR_FAILED(xrCreateAction(m_controller_actions, &action, &m_haptic_actions[hand]))) return false;
    }
    action.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
    for (size_t hand = 0; hand < 2; ++hand) {
        std::strcpy(action.actionName, hand ? "wheel_grab_right" : "wheel_grab_left");
        std::strcpy(action.localizedActionName, hand ? "Grab wheel right" : "Grab wheel left");
        if (XR_FAILED(xrCreateAction(m_controller_actions, &action, &m_squeeze_actions[hand]))) return false;
    }
    std::strcpy(action.actionName, "cockpit_item");
    std::strcpy(action.localizedActionName, "Cockpit item trigger");
    if (XR_FAILED(xrCreateAction(m_controller_actions, &action, &m_item_trigger_action))) return false;
    action.actionType=XR_ACTION_TYPE_BOOLEAN_INPUT;
    std::strcpy(action.actionName,"steamvr_trick_pause_v1");
    std::strcpy(action.localizedActionName,"Trick (tap X), Mario Kart pause (hold X)");
    if(XR_FAILED(xrCreateAction(m_controller_actions,&action,&m_steam_trick_action))) return false;
    action.actionType=XR_ACTION_TYPE_POSE_INPUT;
    std::strcpy(action.actionName,"vr_ui_pointer");
    std::strcpy(action.localizedActionName,"Point at VR interface");
    if(XR_FAILED(xrCreateAction(m_controller_actions,&action,&m_ui_pointer_action))) return false;
    std::strcpy(action.actionName,"vr_ui_left_pointer");
    std::strcpy(action.localizedActionName,"Point at VR interface (left)");
    if(XR_FAILED(xrCreateAction(m_controller_actions,&action,&m_ui_left_pointer_action))) return false;
    constexpr const char* names[kOpenXRControllerActionCount]{
        "steering", "tricks", "accelerate", "item", "drift", "drift_click",
        "confirm", "brake", "trick", "look_back", "pause", "reverse"};
    for (size_t i = 0; i < kOpenXRControllerActionCount; ++i) {
        XrActionCreateInfo game_action{XR_TYPE_ACTION_CREATE_INFO};
        game_action.actionType = i <= static_cast<size_t>(OpenXRControllerAction::Tricks)
                                     ? XR_ACTION_TYPE_VECTOR2F_INPUT
                                 : i <= static_cast<size_t>(OpenXRControllerAction::Drift)
                                     ? XR_ACTION_TYPE_FLOAT_INPUT
                                     : XR_ACTION_TYPE_BOOLEAN_INPUT;
        std::strcpy(game_action.actionName, names[i]);
        std::strcpy(game_action.localizedActionName, names[i]);
        if (XR_FAILED(xrCreateAction(m_controller_actions, &game_action, &m_game_actions[i]))) return false;
    }

    size_t accepted_profiles = 0;
    for (const auto& profile : kOpenXRControllerProfiles) {
        XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        if (XR_FAILED(xrStringToPath(m_instance, profile.interaction_profile.data(),
                                     &suggested.interactionProfile))) {
            continue;
        }

        std::vector<XrActionSuggestedBinding> profile_bindings;
        profile_bindings.reserve(2 + kOpenXRControllerActionCount * 2);
        const auto bind_path = [&](XrAction action, std::string_view path) {
            if (path.empty()) return;
            XrPath xr_path;
            if (XR_SUCCEEDED(xrStringToPath(m_instance, path.data(), &xr_path))) {
                profile_bindings.push_back({action, xr_path});
            }
        };

        bind_path(m_camera_action, profile.camera_click);
        bind_path(m_haptic_actions[0], "/user/hand/left/output/haptic");
        bind_path(m_haptic_actions[1], "/user/hand/right/output/haptic");
        bind_path(m_grip_action, profile.left_grip_pose);
        bind_path(m_right_grip_action, "/user/hand/right/input/grip/pose");
        // Only analog squeeze profiles (Quest/PICO/Index) provide physical wheel input.
        if (profile.actions.drift == "/user/hand/right/input/squeeze/value") {
            bind_path(m_squeeze_actions[0], "/user/hand/left/input/squeeze/value");
            bind_path(m_squeeze_actions[1], "/user/hand/right/input/squeeze/value");
        }
        bind_path(m_item_trigger_action, profile.actions.item[0]);
        bind_path(m_steam_trick_action, profile.actions.trick);
        bind_path(m_ui_pointer_action,"/user/hand/right/input/aim/pose");
        bind_path(m_ui_left_pointer_action,"/user/hand/left/input/aim/pose");
        const std::array<std::array<std::string_view, 2>, kOpenXRControllerActionCount> actions{{
            {profile.actions.steering, {}},
            {profile.actions.tricks, {}},
            {profile.actions.accelerate, {}},
            profile.actions.item,
            {profile.actions.drift, {}},
            {profile.actions.drift_click, {}},
            {profile.actions.confirm, {}},
            {profile.actions.brake, {}},
            {profile.actions.trick, {}},
            {profile.actions.look_back, {}},
            {profile.actions.pause[0], {}},
            {profile.actions.pause[1], {}}, // Left trigger: brake, then reverse when held.
        }};
        for (size_t action = 0; action < actions.size(); ++action) {
            for (const auto path : actions[action]) {
                bind_path(m_game_actions[action], path);
            }
        }

        if (profile_bindings.empty()) continue;
        suggested.countSuggestedBindings = static_cast<uint32_t>(profile_bindings.size());
        suggested.suggestedBindings = profile_bindings.data();
        const auto result = xrSuggestInteractionProfileBindings(m_instance, &suggested);
        if (XR_FAILED(result)) {
            Log(OpenXRLogLevel::Warning,
                "Controller profile bindings rejected: " + std::string(profile.interaction_profile));
        } else {
            ++accepted_profiles;
            Log(OpenXRLogLevel::Info,
                "Controller profile bindings enabled: " + std::string(profile.interaction_profile));
        }
    }
    if (accepted_profiles == 0) {
        Log(OpenXRLogLevel::Warning, "No known OpenXR controller profile was accepted");
    }
    XrActionSpaceCreateInfo space{XR_TYPE_ACTION_SPACE_CREATE_INFO};
    space.action = m_grip_action;
    space.poseInActionSpace.orientation.w = 1.0f;
    if (XR_FAILED(xrCreateActionSpace(m_session, &space, &m_grip_space))) return false;
    space.action = m_right_grip_action;
    if (XR_FAILED(xrCreateActionSpace(m_session, &space, &m_right_grip_space))) return false;
    space.action=m_ui_pointer_action;
    if(XR_FAILED(xrCreateActionSpace(m_session,&space,&m_ui_pointer_space))) return false;
    space.action=m_ui_left_pointer_action;
    if(XR_FAILED(xrCreateActionSpace(m_session,&space,&m_ui_left_pointer_space))) return false;
    XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets = 1;
    attach.actionSets = &m_controller_actions;
    return XR_SUCCEEDED(xrAttachSessionActionSets(m_session, &attach));
}

void OpenXRRuntime::DestroyControllerActions() {
    if(m_ui_left_pointer_space!=XR_NULL_HANDLE) xrDestroySpace(m_ui_left_pointer_space);
    m_ui_left_pointer_space=XR_NULL_HANDLE;m_ui_left_pointer_action=XR_NULL_HANDLE;
    if(m_ui_pointer_space!=XR_NULL_HANDLE) xrDestroySpace(m_ui_pointer_space);
    m_ui_pointer_space=XR_NULL_HANDLE;m_ui_pointer_action=XR_NULL_HANDLE;
    if (m_right_grip_space != XR_NULL_HANDLE) xrDestroySpace(m_right_grip_space);
    m_right_grip_space = XR_NULL_HANDLE;
    m_right_grip_action = m_item_trigger_action = XR_NULL_HANDLE;
    m_squeeze_actions.fill(XR_NULL_HANDLE);
    m_squeeze_values = {};
    m_raw_input = {};
    m_cockpit_input = m_wheel_held = m_right_grip_valid = false;
    if (m_grip_space != XR_NULL_HANDLE) xrDestroySpace(m_grip_space);
    if (m_controller_actions != XR_NULL_HANDLE) xrDestroyActionSet(m_controller_actions);
    m_grip_space = XR_NULL_HANDLE;
    m_controller_actions = XR_NULL_HANDLE;
    m_camera_action = m_grip_action = XR_NULL_HANDLE;
    m_game_actions.fill(XR_NULL_HANDLE);
    m_haptic_actions.fill(XR_NULL_HANDLE);
    PublishQuestInput({});
    m_left_grip_valid = m_camera_clicked = false;
    m_camera_latch = {};
    m_steam_trick_action=XR_NULL_HANDLE;
    m_steam_trick_pause={};
}

void OpenXRRuntime::PulseGrip(size_t hand, bool grabbed) {
    if (hand >= 2 || !IsSessionFocused() || m_haptic_actions[hand] == XR_NULL_HANDLE) return;
    XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
    info.action=m_haptic_actions[hand];
    XrHapticVibration pulse{XR_TYPE_HAPTIC_VIBRATION};
    pulse.duration=grabbed?25000000:15000000;
    pulse.frequency=XR_FREQUENCY_UNSPECIFIED;
    pulse.amplitude=grabbed?0.25f:0.12f;
    xrApplyHapticFeedback(m_session, &info, reinterpret_cast<const XrHapticBaseHeader*>(&pulse));
}

void OpenXRRuntime::PollControllers(XrTime time) {
    m_left_grip_valid = m_camera_clicked = false;
    m_right_grip_valid = false;
    m_squeeze_values = {};
    m_raw_input = {};
    if (m_controller_actions == XR_NULL_HANDLE || !IsSessionFocused()) {
        m_steam_trick_pause.Update(false,false,false,time);
        m_camera_latch.Update(false, false);
        PublishQuestInput({});
        return;
    }
    XrActiveActionSet active{m_controller_actions, XR_NULL_PATH};
    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    if (xrSyncActions(m_session, &sync) != XR_SUCCESS) {
        m_steam_trick_pause.Update(false,false,false,time);
        m_camera_latch.Update(false, false);
        PublishQuestInput({});
        return;
    }
    XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
    get.action = m_camera_action;
    XrActionStateBoolean click{XR_TYPE_ACTION_STATE_BOOLEAN};
    if (XR_SUCCEEDED(xrGetActionStateBoolean(m_session, &get, &click)) && click.isActive) {
        m_camera_clicked = m_camera_latch.Update(true, click.currentState);
    } else m_camera_latch.Update(false, false);
    const auto vector_state = [&](OpenXRControllerAction action) {
        get.action = m_game_actions[static_cast<size_t>(action)];
        XrActionStateVector2f state{XR_TYPE_ACTION_STATE_VECTOR2F};
        if (XR_FAILED(xrGetActionStateVector2f(m_session, &get, &state)) || !state.isActive)
            state = {XR_TYPE_ACTION_STATE_VECTOR2F};
        return state;
    };
    const auto float_state = [&](OpenXRControllerAction action) {
        get.action = m_game_actions[static_cast<size_t>(action)];
        XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
        if (XR_FAILED(xrGetActionStateFloat(m_session, &get, &state)) || !state.isActive)
            state = {XR_TYPE_ACTION_STATE_FLOAT};
        return state;
    };
    const auto boolean_state = [&](OpenXRControllerAction action) {
        get.action = m_game_actions[static_cast<size_t>(action)];
        XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
        if (XR_FAILED(xrGetActionStateBoolean(m_session, &get, &state)) || !state.isActive)
            state = {XR_TYPE_ACTION_STATE_BOOLEAN};
        return state;
    };
    const auto steering = vector_state(OpenXRControllerAction::Steering);
    const auto tricks = vector_state(OpenXRControllerAction::Tricks);
    const auto accelerate = float_state(OpenXRControllerAction::Accelerate);
    const auto item = float_state(OpenXRControllerAction::Item);
    const auto drift = float_state(OpenXRControllerAction::Drift);
    const auto drift_click = boolean_state(OpenXRControllerAction::DriftClick);
    const auto confirm = boolean_state(OpenXRControllerAction::Confirm);
    const auto brake = boolean_state(OpenXRControllerAction::Brake);
    const auto trick = boolean_state(OpenXRControllerAction::Trick);
    const auto look_back = boolean_state(OpenXRControllerAction::LookBack);
    const auto pause = boolean_state(OpenXRControllerAction::Pause);
    const auto reverse = boolean_state(OpenXRControllerAction::Reverse);
    QuestInput input{};
    // Some fallback profiles (for example Khronos Simple Controller) do not
    // expose analog controls. Keep them active for menu navigation while
    // still allowing full driving on profiles with sticks and triggers.
    input.active = steering.isActive || tricks.isActive || accelerate.isActive || item.isActive ||
                   drift.isActive || drift_click.isActive || confirm.isActive || brake.isActive ||
                   trick.isActive || look_back.isActive || pause.isActive || reverse.isActive;
    input.steering_x = steering.currentState.x;
    input.steering_y = steering.currentState.y;
    input.tricks_x = tricks.currentState.x;
    input.tricks_y = tricks.currentState.y;
    input.accelerate = accelerate.currentState;
    input.item = item.currentState;
    input.drift = std::max(drift.currentState, drift_click.currentState ? 1.0f : 0.0f);
    input.confirm = confirm.currentState;
    input.brake = brake.currentState;
    input.trick = trick.currentState;
    input.look_back = look_back.currentState;
    input.pause = pause.currentState;
    input.reverse = reverse.currentState;
    if(m_runtime_info.runtime_name.find("SteamVR")!=std::string::npos) {
        get.action=m_steam_trick_action;
        XrActionStateBoolean physicalX{XR_TYPE_ACTION_STATE_BOOLEAN};
        const bool valid=XR_SUCCEEDED(xrGetActionStateBoolean(m_session,&get,&physicalX)) && physicalX.isActive;
        const auto buttons=m_steam_trick_pause.Update(valid,physicalX.currentState,
            QuestAxis(input.item)>0.5f,time);
        input.trick=buttons.trick;
        input.pause=buttons.pause;
    }
    input.steamvr=m_runtime_info.runtime_name.find("SteamVR")!=std::string::npos;
    const auto panelPolicy=MkwVRPolicyGetSnapshot();
    // Keep the same upright world-space screen throughout menu navigation.
    // A transient missing race camera is not a menu and must not create an anchor.
    const bool showPanel=panelPolicy.settings_visible ||
        (panelPolicy.presentation==VRPresentationMode::VirtualScreen &&
         panelPolicy.scene.mode!=VRSceneMode::Race);
    if (!showPanel) { m_panel_anchored=false;m_panel_tracking_since=0; }
    else if (!m_panel_anchored) {
        XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
        const auto flags=XR_SPACE_LOCATION_POSITION_VALID_BIT|XR_SPACE_LOCATION_ORIENTATION_VALID_BIT|
            XR_SPACE_LOCATION_POSITION_TRACKED_BIT|XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
        if(XR_SUCCEEDED(xrLocateSpace(ViewSpace(),AppSpace(),time,&head)) && (head.locationFlags&flags)==flags) {
            // Keep the menu upright and stationary where it was opened.
            const auto& q=head.pose.orientation;
            const auto forward=RotateUiVector(q.x,q.y,q.z,q.w,{0,0,-1});
            const auto up=RotateUiVector(q.x,q.y,q.z,q.w,{0,1,0});
            const float yaw=std::atan2(-forward[0],-forward[2]);
            const bool ready=IsSessionFocused() && up[1]>.5f &&
                forward[0]*forward[0]+forward[2]*forward[2]>.25f;
            if(!ready) m_panel_tracking_since=0;
            else {
                if(!m_panel_tracking_since) m_panel_tracking_since=time;
                if(time-m_panel_tracking_since>=400000000) {
                    m_panel_origin=head.pose;
                    m_panel_origin.orientation={0,std::sin(yaw*.5f),0,std::cos(yaw*.5f)};
                    m_panel_anchored=true;
                }
            }
        } else m_panel_tracking_since=0;
    }
    const auto uiPose=[&](XrSpace space) {
        UiHandPose out;
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        constexpr XrSpaceLocationFlags required=XR_SPACE_LOCATION_POSITION_VALID_BIT|XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        if(space==XR_NULL_HANDLE || XR_FAILED(xrLocateSpace(space,m_panel_anchored?AppSpace():ViewSpace(),time,&location)) ||
            (location.locationFlags&required)!=required) return out;
        const auto& p=location.pose.position;const auto& q=location.pose.orientation;
        out.position={p.x,p.y,p.z};
        out.right=RotateUiVector(q.x,q.y,q.z,q.w,{1,0,0});
        out.up=RotateUiVector(q.x,q.y,q.z,q.w,{0,1,0});
        out.forward=RotateUiVector(q.x,q.y,q.z,q.w,{0,0,-1});out.valid=true;
        if(m_panel_anchored) {
            const auto& a=m_panel_origin;
            const auto rotate=[&](std::array<float,3> v) { return RotateUiVector(-a.orientation.x,-a.orientation.y,-a.orientation.z,a.orientation.w,v); };
            out.position=rotate({p.x-a.position.x,p.y-a.position.y,p.z-a.position.z});
            out.right=rotate(out.right);out.up=rotate(out.up);out.forward=rotate(out.forward);
        }
        return out;
    };
    input.ui_pointer=uiPose(m_ui_pointer_space);
    input.ui_left_pointer=uiPose(m_ui_left_pointer_space);
    input.ui_hands={uiPose(m_grip_space),uiPose(m_right_grip_space)};
    if(m_view_configuration[0].render_height)
        input.ui_aspect=float(m_view_configuration[0].render_width)/m_view_configuration[0].render_height;
    m_raw_input = input;
    for (size_t hand = 0; hand < 2; ++hand) {
        get.action = m_squeeze_actions[hand];
        XrActionStateFloat squeeze{XR_TYPE_ACTION_STATE_FLOAT};
        if (XR_SUCCEEDED(xrGetActionStateFloat(m_session, &get, &squeeze)) && squeeze.isActive)
            m_squeeze_values[hand] = squeeze.currentState;
        m_raw_input.ui_grips[hand] = m_squeeze_values[hand];
        get.action = hand ? m_right_grip_action : m_grip_action;
        XrActionStatePose pose{XR_TYPE_ACTION_STATE_POSE};
        if (XR_FAILED(xrGetActionStatePose(m_session, &get, &pose)) || !pose.isActive) continue;
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        constexpr XrSpaceLocationFlags required = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        if (time > 0 && XR_SUCCEEDED(xrLocateSpace(hand ? m_right_grip_space : m_grip_space, m_app_space, time, &location)) &&
            (location.locationFlags & required) == required) {
            (hand ? m_right_grip_pose : m_left_grip_pose) = location.pose;
            (hand ? m_right_grip_valid : m_left_grip_valid) = true;
        }
    }
    get.action = m_item_trigger_action;
    XrActionStateFloat trigger{XR_TYPE_ACTION_STATE_FLOAT};
    m_item_trigger = XR_SUCCEEDED(xrGetActionStateFloat(m_session, &get, &trigger)) && trigger.isActive
        ? trigger.currentState : 0;
    PublishDrivingInput(m_cockpit_input, m_wheel_steering, m_wheel_held, m_wheel_angle);
}

void OpenXRRuntime::PublishDrivingInput(bool cockpit, float steering, bool held, float angle) {
    m_cockpit_input = cockpit;
    m_wheel_held = cockpit && held; // SteeringWheel owns the bounded tracking-loss grace period.
    m_wheel_steering = steering;
    m_wheel_angle = cockpit ? angle : 0;
    auto input = m_raw_input;
    input.cockpit_controls = cockpit;
    input.wheel_active = m_wheel_held;
    input.wheel_steering = steering;
    input.wheel_angle = m_wheel_angle;
    if (cockpit) {
        input.item = m_item_trigger;
        input.drift = 0; // A is mapped to hop/drift by MapQuestInput. Grips only grab.
    }
    PublishQuestInput(input);
}

bool OpenXRRuntime::GetInstanceProcAddress(
    const char* name, PFN_xrVoidFunction* function) {
    ClearError();
    if (!IsInitialized()) {
        return Fail(XR_ERROR_CALL_ORDER_INVALID, "xrGetInstanceProcAddr",
                    "OpenXR is not initialized");
    }
    if (name == nullptr || name[0] == '\0' || function == nullptr) {
        return Fail(XR_ERROR_VALIDATION_FAILURE, "xrGetInstanceProcAddr",
                    "function name and output pointer must be valid");
    }
    *function = nullptr;
    return Check(xrGetInstanceProcAddr(m_instance, name, function),
                 "xrGetInstanceProcAddr");
}

bool OpenXRRuntime::CreateReferenceSpaces() {
    uint32_t space_count = 0;
    if (!Check(xrEnumerateReferenceSpaces(m_session, 0, &space_count, nullptr),
               "xrEnumerateReferenceSpaces(count)")) {
        return false;
    }
    m_supported_reference_spaces.resize(space_count);
    if (space_count != 0 &&
        !Check(xrEnumerateReferenceSpaces(
                   m_session, space_count, &space_count,
                   m_supported_reference_spaces.data()),
               "xrEnumerateReferenceSpaces")) {
        return false;
    }
    m_supported_reference_spaces.resize(space_count);

    XrReferenceSpaceCreateInfo view_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    view_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    view_info.poseInReferenceSpace = IdentityPose();
    if (!Check(xrCreateReferenceSpace(m_session, &view_info, &m_view_space),
               "xrCreateReferenceSpace(VIEW)")) {
        return false;
    }

    m_app_space_type = m_config.reference_space;
    if (!Contains(m_supported_reference_spaces, m_app_space_type)) {
        if (Contains(m_supported_reference_spaces, XR_REFERENCE_SPACE_TYPE_LOCAL)) {
            m_app_space_type = XR_REFERENCE_SPACE_TYPE_LOCAL;
            Log(OpenXRLogLevel::Warning,
                "requested reference space is unavailable; using LOCAL");
        } else if (Contains(m_supported_reference_spaces,
                            XR_REFERENCE_SPACE_TYPE_STAGE)) {
            m_app_space_type = XR_REFERENCE_SPACE_TYPE_STAGE;
            Log(OpenXRLogLevel::Warning,
                "requested reference space is unavailable; using STAGE");
        } else {
            return Fail(XR_ERROR_REFERENCE_SPACE_UNSUPPORTED,
                        "xrCreateReferenceSpace",
                        "runtime exposes neither requested, LOCAL, nor STAGE space");
        }
    }

    XrReferenceSpaceCreateInfo app_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    app_info.referenceSpaceType = m_app_space_type;
    app_info.poseInReferenceSpace = IdentityPose();
    return Check(xrCreateReferenceSpace(m_session, &app_info, &m_app_space),
                 "xrCreateReferenceSpace(application)");
}

bool OpenXRRuntime::EnumerateSwapchainFormats() {
    uint32_t format_count = 0;
    if (!Check(xrEnumerateSwapchainFormats(
                   m_session, 0, &format_count, nullptr),
               "xrEnumerateSwapchainFormats(count)")) {
        return false;
    }
    if (format_count == 0) {
        return Fail(XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED,
                    "xrEnumerateSwapchainFormats",
                    "runtime returned no swapchain formats");
    }
    m_swapchain_formats.resize(format_count);
    if (!Check(xrEnumerateSwapchainFormats(
                   m_session, format_count, &format_count,
                   m_swapchain_formats.data()),
               "xrEnumerateSwapchainFormats")) {
        return false;
    }
    m_swapchain_formats.resize(format_count);
    return true;
}

OpenXREventStatus OpenXRRuntime::PollEvents() {
    if (!IsInitialized()) {
        Fail(XR_ERROR_CALL_ORDER_INVALID, "PollEvents",
             "OpenXR is not initialized");
        return OpenXREventStatus::Error;
    }

    for (;;) {
        XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
        const XrResult result = xrPollEvent(m_instance, &event);
        if (result == XR_EVENT_UNAVAILABLE) {
            return ShouldExit() ? OpenXREventStatus::ExitRequested
                                : OpenXREventStatus::Continue;
        }
        if (XR_FAILED(result)) {
            Check(result, "xrPollEvent");
            if (result == XR_ERROR_INSTANCE_LOST) {
                m_instance_loss_pending = true;
            }
            return OpenXREventStatus::Error;
        }

        switch (event.type) {
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
            const auto& state_event =
                *reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
            if (state_event.session == m_session &&
                !HandleSessionStateChanged(state_event)) {
                return OpenXREventStatus::Error;
            }
            break;
        }
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
            m_instance_loss_pending = true;
            m_session_running = false;
            ResetFrameState();
            Log(OpenXRLogLevel::Warning,
                "OpenXR runtime reported instance loss pending");
            break;
        case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING: {
            const auto& space_event =
                *reinterpret_cast<const XrEventDataReferenceSpaceChangePending*>(&event);
            // This slot is consumed to invalidate transforms located in the
            // application space. Events for VIEW or another supported type
            // must not overwrite a pending LOCAL/STAGE change.
            if (space_event.session == m_session &&
                space_event.referenceSpaceType == m_app_space_type) {
                if (++m_reference_space_change.serial == 0) {
                    ++m_reference_space_change.serial;
                }
                m_reference_space_change.type = space_event.referenceSpaceType;
                m_reference_space_change.change_time = space_event.changeTime;
                m_reference_space_change.pose_in_previous_space_valid =
                    space_event.poseValid == XR_TRUE;
                m_reference_space_change.pose_in_previous_space =
                    space_event.poseInPreviousSpace;
                m_pending_app_space_changes.push_back(m_reference_space_change);
            }
            break;
        }
        case XR_TYPE_EVENT_DATA_EVENTS_LOST: {
            const auto& lost_event =
                *reinterpret_cast<const XrEventDataEventsLost*>(&event);
            std::ostringstream message;
            message << "OpenXR runtime lost " << lost_event.lostEventCount
                    << " event(s)";
            Log(OpenXRLogLevel::Warning, message.str());
            break;
        }
        default:
            break;
        }
    }
}

bool OpenXRRuntime::HandleSessionStateChanged(
    const XrEventDataSessionStateChanged& event) {
    m_session_state = event.state;
    std::ostringstream message;
    message << "OpenXR session state -> " << SessionStateName(event.state);
    Log(OpenXRLogLevel::Info, message.str());

    switch (event.state) {
    case XR_SESSION_STATE_READY: {
        if (m_shutting_down_session || m_session_running) {
            return true;
        }
        XrSessionBeginInfo begin_info{XR_TYPE_SESSION_BEGIN_INFO};
        begin_info.primaryViewConfigurationType = m_config.view_configuration;
        if (!Check(xrBeginSession(m_session, &begin_info), "xrBeginSession")) {
            return false;
        }
        ResetFrameState();
        m_session_running = true;
        if (++m_session_run_serial == 0) {
            ++m_session_run_serial;
        }
        return true;
    }
    case XR_SESSION_STATE_STOPPING: {
        if (m_session_running) {
            const XrResult result = xrEndSession(m_session);
            // The OpenXR session is no longer running after any xrEndSession
            // call, including one that returns an error.
            m_session_running = false;
            ResetFrameState();
            if (!Check(result, "xrEndSession")) {
                return false;
            }
        }
        return true;
    }
    case XR_SESSION_STATE_EXITING:
        m_exit_requested = true;
        m_session_running = false;
        ResetFrameState();
        return true;
    case XR_SESSION_STATE_LOSS_PENDING:
        m_session_loss_pending = true;
        m_session_running = false;
        ResetFrameState();
        return true;
    default:
        return true;
    }
}

bool OpenXRRuntime::RequestExitSession() {
    ClearError();
    if (!HasSession() || !m_session_running) {
        return true;
    }
    return Check(xrRequestExitSession(m_session), "xrRequestExitSession");
}

void OpenXRRuntime::ObserveResult(XrResult result) noexcept {
    if (result == XR_SESSION_LOSS_PENDING) {
        if (!m_session_loss_pending) {
            Log(OpenXRLogLevel::Warning,
                "OpenXR reported XR_SESSION_LOSS_PENDING");
        }
        m_session_loss_pending = true;
    } else if (result == XR_ERROR_SESSION_LOST) {
        m_session_loss_pending = true;
        m_session_running = false;
        ResetFrameState();
    } else if (result == XR_ERROR_INSTANCE_LOST) {
        m_instance_loss_pending = true;
        m_session_running = false;
        ResetFrameState();
    }
}

OpenXRFrameStatus OpenXRRuntime::WaitFrame(OpenXRFrame& frame) {
    ClearError();
    if (ShouldExit()) {
        return OpenXRFrameStatus::ExitRequested;
    }
    if (!HasSession() || !m_session_running) {
        return OpenXRFrameStatus::SessionNotRunning;
    }
    if (m_frame_phase != FramePhase::Idle) {
        Fail(XR_ERROR_CALL_ORDER_INVALID, "xrWaitFrame",
             "the previous frame has not been ended");
        return OpenXRFrameStatus::Error;
    }

    XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState state{XR_TYPE_FRAME_STATE};
    if (!Check(xrWaitFrame(m_session, &wait_info, &state), "xrWaitFrame")) {
        return OpenXRFrameStatus::Error;
    }

    frame = {};
    frame.serial = m_next_frame_serial++;
    frame.predicted_display_time = state.predictedDisplayTime;
    frame.predicted_display_period = state.predictedDisplayPeriod;
    frame.should_render = state.shouldRender == XR_TRUE;
    m_active_frame_serial = frame.serial;
    m_active_frame_display_time = frame.predicted_display_time;
    m_frame_phase = FramePhase::Waited;
    return OpenXRFrameStatus::Ready;
}

bool OpenXRRuntime::BeginFrame(const OpenXRFrame& frame) {
    ClearError();
    if (!IsFrameTokenCurrent(frame, FramePhase::Waited)) {
        return Fail(XR_ERROR_CALL_ORDER_INVALID, "xrBeginFrame",
                    "frame token is stale or xrWaitFrame was not called");
    }

    XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
    const XrResult result = xrBeginFrame(m_session, &begin_info);
    if (XR_FAILED(result)) {
        m_frame_phase = FramePhase::Idle;
        m_active_frame_serial = 0;
        m_active_frame_display_time = 0;
        return Check(result, "xrBeginFrame");
    }
    m_frame_phase = FramePhase::Begun;
    return true;
}

bool OpenXRRuntime::LocateViews(OpenXRFrame& frame) {
    ClearError();
    if (!IsFrameTokenCurrent(frame, FramePhase::Begun)) {
        return Fail(XR_ERROR_CALL_ORDER_INVALID, "xrLocateViews",
                    "frame token is stale or xrBeginFrame was not called");
    }
    frame.views_valid = false;
    frame.view_state_flags = 0;
    if (!frame.should_render) {
        return true;
    }

    for (XrView& view : frame.views) {
        view = {XR_TYPE_VIEW};
    }
    XrViewLocateInfo locate_info{XR_TYPE_VIEW_LOCATE_INFO};
    locate_info.viewConfigurationType = m_config.view_configuration;
    locate_info.displayTime = frame.predicted_display_time;
    locate_info.space = m_app_space;
    XrViewState view_state{XR_TYPE_VIEW_STATE};
    uint32_t view_count = 0;
    if (!Check(xrLocateViews(m_session, &locate_info, &view_state,
                             kOpenXREyeCount, &view_count, frame.views.data()),
               "xrLocateViews")) {
        return false;
    }
    if (view_count != kOpenXREyeCount) {
        return Fail(XR_ERROR_RUNTIME_FAILURE, "xrLocateViews",
                    "runtime returned an unexpected stereo view count");
    }

    frame.view_state_flags = view_state.viewStateFlags;
    frame.views_valid =
        (view_state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;
    return true;
}

bool OpenXRRuntime::EndFrame(
    const OpenXRFrame& frame,
    const XrCompositionLayerBaseHeader* const* layers,
    uint32_t layer_count) {
    ClearError();
    if (!IsFrameTokenCurrent(frame, FramePhase::Begun)) {
        return Fail(XR_ERROR_CALL_ORDER_INVALID, "xrEndFrame",
                    "frame token is stale or xrBeginFrame was not called");
    }
    if (layer_count != 0 && layers == nullptr) {
        return Fail(XR_ERROR_VALIDATION_FAILURE, "xrEndFrame",
                    "non-zero layer_count requires a layer array");
    }

    // The runtime explicitly requested no application rendering. Ending with an
    // empty layer list preserves the frame protocol without presenting stale work.
    if (!frame.should_render) {
        layers = nullptr;
        layer_count = 0;
    }

    XrFrameEndInfo end_info{XR_TYPE_FRAME_END_INFO};
    end_info.displayTime = frame.predicted_display_time;
    end_info.environmentBlendMode = m_blend_mode;
    end_info.layerCount = layer_count;
    end_info.layers = layers;
    const XrResult result = xrEndFrame(m_session, &end_info);
    m_frame_phase = FramePhase::Idle;
    m_active_frame_serial = 0;
    m_active_frame_display_time = 0;
    return Check(result, "xrEndFrame");
}

bool OpenXRRuntime::EndFrame(
    const OpenXRFrame& frame,
    const std::vector<const XrCompositionLayerBaseHeader*>& layers) {
    return EndFrame(frame, layers.data(), static_cast<uint32_t>(layers.size()));
}

bool OpenXRRuntime::EndFrameWithoutLayers(const OpenXRFrame& frame) {
    return EndFrame(frame, nullptr, 0);
}

bool OpenXRRuntime::ResetAppSpace(const XrPosef& pose_in_reference_space) {
    m_panel_anchored=false;
    ClearError();
    if (!HasSession()) {
        return Fail(XR_ERROR_CALL_ORDER_INVALID, "ResetAppSpace",
                    "no OpenXR session exists");
    }
    if (m_frame_phase != FramePhase::Idle) {
        return Fail(XR_ERROR_CALL_ORDER_INVALID, "ResetAppSpace",
                    "reference space cannot change during a frame");
    }

    XrReferenceSpaceCreateInfo create_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    create_info.referenceSpaceType = m_app_space_type;
    create_info.poseInReferenceSpace = pose_in_reference_space;
    XrSpace replacement = XR_NULL_HANDLE;
    if (!Check(xrCreateReferenceSpace(m_session, &create_info, &replacement),
               "xrCreateReferenceSpace(recenter)")) {
        return false;
    }
    if (m_app_space != XR_NULL_HANDLE) {
        xrDestroySpace(m_app_space);
    }
    m_app_space = replacement;
    if (++m_reference_space_change.serial == 0) {
        ++m_reference_space_change.serial;
    }
    m_reference_space_change.type = m_app_space_type;
    m_reference_space_change.change_time = 0;
    m_reference_space_change.pose_in_previous_space_valid = false;
    m_reference_space_change.pose_in_previous_space = IdentityPose();
    m_pending_app_space_changes.push_back(m_reference_space_change);
    return true;
}

bool OpenXRRuntime::ConsumeAppSpaceChangesThrough(XrTime display_time) {
    bool consumed = false;
    std::erase_if(m_pending_app_space_changes,
                  [&](const OpenXRReferenceSpaceChange& change) {
                      const bool due = change.change_time == 0 ||
                                       display_time >= change.change_time;
                      consumed = consumed || due;
                      return due;
                  });
    if(consumed) { m_panel_anchored=false;m_panel_tracking_since=0; }
    return consumed;
}

void OpenXRRuntime::DestroySession() {
    if (!HasSession()) {
        ResetSessionState();
        return;
    }

    if (m_frame_phase == FramePhase::Begun) {
        Log(OpenXRLogLevel::Warning,
            "ending an active OpenXR frame without layers during teardown");
        XrFrameEndInfo end_info{XR_TYPE_FRAME_END_INFO};
        end_info.displayTime = m_active_frame_display_time;
        end_info.environmentBlendMode = m_blend_mode;
        const XrResult end_result = xrEndFrame(m_session, &end_info);
        if (XR_FAILED(end_result)) {
            Log(OpenXRLogLevel::Warning,
                "xrEndFrame failed during session teardown");
        }
        m_frame_phase = FramePhase::Idle;
        m_active_frame_serial = 0;
        m_active_frame_display_time = 0;
    }

    m_shutting_down_session = true;
    if (m_session_running) {
        const XrResult request_result = xrRequestExitSession(m_session);
        if (XR_FAILED(request_result)) {
            Log(OpenXRLogLevel::Warning,
                "xrRequestExitSession failed during bounded teardown");
        } else {
            const auto timeout =
                std::chrono::milliseconds(m_config.shutdown_timeout_ms);
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            bool event_error = false;
            while (m_session_running &&
                   std::chrono::steady_clock::now() < deadline) {
                const OpenXREventStatus status = PollEvents();
                if (status != OpenXREventStatus::Continue) {
                    event_error = status == OpenXREventStatus::Error;
                    break;
                }
                if (m_session_running) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
            if (m_session_running) {
                Log(OpenXRLogLevel::Warning, event_error
                    ? "OpenXR event processing failed during session teardown"
                    : "OpenXR runtime did not finish session exit before timeout");
            }
        }
    }

    DestroyReferenceSpaces();
    DestroyControllerActions();
    const XrResult result = xrDestroySession(m_session);
    if (XR_FAILED(result)) {
        Log(OpenXRLogLevel::Warning, "xrDestroySession failed");
    }
    m_session = XR_NULL_HANDLE;
    ResetSessionState();
}

void OpenXRRuntime::Shutdown() {
    DestroySession();
    if (m_instance != XR_NULL_HANDLE) {
        const XrResult result = xrDestroyInstance(m_instance);
        if (XR_FAILED(result)) {
            Log(OpenXRLogLevel::Warning, "xrDestroyInstance failed");
        }
    }
    ResetInstanceState();
}

void OpenXRRuntime::DestroyReferenceSpaces() {
    if (m_view_space != XR_NULL_HANDLE) {
        xrDestroySpace(m_view_space);
        m_view_space = XR_NULL_HANDLE;
    }
    if (m_app_space != XR_NULL_HANDLE) {
        xrDestroySpace(m_app_space);
        m_app_space = XR_NULL_HANDLE;
    }
}

void OpenXRRuntime::ResetSessionState() {
    m_session_state = XR_SESSION_STATE_UNKNOWN;
    m_app_space_type = m_config.reference_space;
    m_session_running = false;
    m_exit_requested = false;
    m_shutting_down_session = false;
    ResetFrameState();
    m_supported_reference_spaces.clear();
    m_swapchain_formats.clear();
    m_reference_space_change = {};
    m_pending_app_space_changes.clear();
}

void OpenXRRuntime::ResetInstanceState() {
    m_instance = XR_NULL_HANDLE;
    m_system_id = XR_NULL_SYSTEM_ID;
    m_session_loss_pending = false;
    m_instance_loss_pending = false;
    m_session_run_serial = 0;
    m_runtime_info = {};
    m_view_configuration = {};
    m_available_extensions.clear();
    m_available_api_layers.clear();
    m_enabled_extensions.clear();
    m_enabled_api_layers.clear();
    m_supported_blend_modes.clear();
    ResetSessionState();
}

bool OpenXRRuntime::IsFrameTokenCurrent(
    const OpenXRFrame& frame, FramePhase expected) const {
    return m_frame_phase == expected && frame.serial != 0 &&
           frame.serial == m_active_frame_serial;
}

void OpenXRRuntime::ResetFrameState() {
    m_frame_phase = FramePhase::Idle;
    m_active_frame_serial = 0;
    m_active_frame_display_time = 0;
}

bool OpenXRRuntime::Check(XrResult result, std::string_view operation) {
    ObserveResult(result);
    if (XR_SUCCEEDED(result)) {
        return true;
    }
    return Fail(result, operation, {});
}

bool OpenXRRuntime::Fail(
    XrResult result, std::string_view operation, std::string_view detail) {
    m_last_error.result = result;
    m_last_error.operation.assign(operation);
    std::ostringstream message;
    message << operation << " failed: " << ResultString(result);
    if (!detail.empty()) {
        message << " (" << detail << ')';
    }
    m_last_error.message = message.str();
    Log(OpenXRLogLevel::Error, m_last_error.message);
    return false;
}

void OpenXRRuntime::ClearError() {
    m_last_error = {};
}

void OpenXRRuntime::Log(
    OpenXRLogLevel level, std::string_view message) const noexcept {
    if (!m_logger) {
        return;
    }
    try {
        m_logger(level, message);
    } catch (...) {
        // Diagnostic callbacks must never make XR teardown throw.
    }
}

std::string OpenXRRuntime::ResultString(XrResult result) const {
    if (m_instance != XR_NULL_HANDLE) {
        char text[XR_MAX_RESULT_STRING_SIZE]{};
        if (XR_SUCCEEDED(xrResultToString(m_instance, result, text))) {
            return text;
        }
    }
    return std::to_string(static_cast<int32_t>(result));
}

} // namespace mkw::vr

#endif // MKW_ENABLE_OPENXR
