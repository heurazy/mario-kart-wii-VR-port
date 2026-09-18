#include "settings_overlay.h"
#include "physical_wheel.h"
#include "abi_bridge.h"
#include "audio_backend.h"
#include "controller_mapping_wizard.h"
#include "game_graphics_options.h"
#include "music_attenuation.h"
#include "runtime_config.h"
#include "runtime_log.h"
#include "vr/mkw_vr_first_person.h"
#include "vr/mkw_vr_policy.h"
#include "vr/quest_input.h"
#include "vr/openxr_integration.h"
#include "ppc_runtime.h"
#include "wii_remote_input.h"

#include <imgui.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_scancode.h>

#include <array>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shellapi.h>
#define OPENVR_API_NODLL
#define USE_SDL
#include "../../Dependencies/SDL/src/video/openvr/openvr_capi.h"
#undef USE_SDL
#endif

#include <dolphin/pad.h>
#include <dolphin/vi.h>
#include <aurora/aurora.h>
#include <aurora/gfx.h>

extern "C" int g_gxFrameCount;

// Defined in runtime/src/hle/audio/ax_mix.cpp. That header is private to the HLE
// directory and is not on this target's include path.
namespace AxDspHle {
void SetMixWorkerEnabled(bool enabled);
}

namespace settings_overlay {
namespace {

const char* GraphicsApiDisplayName() {
    switch (aurora_get_backend()) {
    case BACKEND_D3D11: return "Direct3D 11";
    case BACKEND_D3D12: return "Direct3D 12";
    case BACKEND_METAL: return "Metal";
    case BACKEND_VULKAN: return "Vulkan";
    case BACKEND_OPENGL: return "OpenGL";
    case BACKEND_OPENGLES: return "OpenGL ES";
    case BACKEND_WEBGPU: return "WebGPU";
    case BACKEND_NULL: return "Null";
    case BACKEND_AUTO: return "Automatic";
    }
    return "Unknown";
}

bool g_topBarVisible = false;
bool g_vrSettingsVisible = false;
bool g_vrSettingsFocus = false;
mkw::vr::QuestStickCalibration g_vrStickCalibration = [] {
    const auto& config = RuntimeConfigFile::Get();
    return mkw::vr::QuestStickCalibration{
        std::clamp(mkw::vr::QuestAxis(config.vrStickDeadzone.value_or(0.15f)), 0.0f, 0.4f),
        std::clamp(mkw::vr::QuestAxis(config.vrStickOuter.value_or(1.0f)), 0.6f, 1.0f),
        std::clamp(mkw::vr::QuestAxis(config.vrStickCenterX.value_or(0.0f)), -0.3f, 0.3f),
        std::clamp(mkw::vr::QuestAxis(config.vrStickCenterY.value_or(0.0f)), -0.3f, 0.3f)};
}();
int g_controllerPort = 0;
float g_resolutionScale = RuntimeConfigFile::ResolutionMultiplier(1.0f);
int g_audioVolumePercent = static_cast<int>(std::lround(RuntimeConfigFile::AudioVolume(1.0f) * 100.0f));
int g_musicVolumePercent = static_cast<int>(std::lround(RuntimeConfigFile::MusicVolume(1.0f) * 100.0f));
int g_soundEffectsVolumePercent =
    static_cast<int>(std::lround(RuntimeConfigFile::SoundEffectsVolume(1.0f) * 100.0f));
int g_uiVolumePercent = static_cast<int>(std::lround(RuntimeConfigFile::UiVolume(1.0f) * 100.0f));
int g_voicesVolumePercent = static_cast<int>(std::lround(RuntimeConfigFile::VoicesVolume(1.0f) * 100.0f));
bool g_audioMuted = RuntimeConfigFile::AudioMuted(false);
bool g_audioMixWorker = RuntimeConfigFile::AudioMixWorkerEnabled(true);
bool g_attenuateMusicWhenMediaPlays = RuntimeConfigFile::AttenuateMusicWhenMediaPlays(false);
int g_frameInterpolationMode = [] {
    switch (RuntimeConfigFile::FrameInterpolationFps(0)) {
    case 120:
        return 1;
    case 180:
        return 2;
    default:
        return 0;
    }
}();
int g_displayMode = [] {
    const std::string mode = RuntimeConfigFile::DisplayMode("windowed");
    if (mode == "borderless") {
        return static_cast<int>(AURORA_DISPLAY_MODE_BORDERLESS);
    }
    if (mode == "exclusive") {
        return static_cast<int>(AURORA_DISPLAY_MODE_EXCLUSIVE);
    }
    return static_cast<int>(AURORA_DISPLAY_MODE_WINDOWED);
}();
bool g_skipUnreadyPipelines = RuntimeConfigFile::SkipUnreadyPipelines(true);
bool g_disableCopyFilter = RuntimeConfigFile::DisableCopyFilter(true);
bool g_showFps = RuntimeConfigFile::ShowFps(true);
bool g_vrEnabled = RuntimeConfigFile::VrEnabled(false);
bool g_vrStopAtDisplayCopy = RuntimeConfigFile::VrStopAtDisplayCopy(true);
bool g_vrSkipCopyClears = RuntimeConfigFile::VrSkipCopyClears(true);
bool g_vrHudVirtualScreen = RuntimeConfigFile::VrHudVirtualScreen(true);
bool g_vrFirstPerson = RuntimeConfigFile::VrFirstPerson(false);
float g_vrFirstPersonUnitsPerMeter = RuntimeConfigFile::VrFirstPersonUnitsPerMeter(100.0f);
float g_vrFirstPersonHeadUp = RuntimeConfigFile::VrFirstPersonHeadUpMeters(1.1f);
float g_vrFirstPersonHeadForward = RuntimeConfigFile::VrFirstPersonHeadForwardMeters(1.2f);
float g_vrFirstPersonHeadRight = RuntimeConfigFile::VrFirstPersonHeadRightMeters(0.0f);
uint32_t g_disabledPostProcessingPaths = RuntimeConfigFile::DisabledPostProcessingPaths(0);
std::array<int32_t, PAD_MAX_CONTROLLERS> g_configuredControllerIndices = [] {
    std::array<int32_t, PAD_MAX_CONTROLLERS> indices{};
    indices.fill(std::numeric_limits<int32_t>::min());
    return indices;
}();

struct ControllerButtonItem {
    const char* configKey;
    const char* label;
    PADButton padButton;
};

constexpr std::array<ControllerButtonItem, PAD_BUTTON_COUNT> kControllerButtons = {{
    {"a", "A", PAD_BUTTON_A},
    {"b", "B", PAD_BUTTON_B},
    {"x", "X", PAD_BUTTON_X},
    {"y", "Y", PAD_BUTTON_Y},
    {"start", "Start", PAD_BUTTON_START},
    {"z", "Z", PAD_TRIGGER_Z},
    {"l", "L", PAD_TRIGGER_L},
    {"r", "R", PAD_TRIGGER_R},
    {"up", "D-pad Up", PAD_BUTTON_UP},
    {"down", "D-pad Down", PAD_BUTTON_DOWN},
    {"left", "D-pad Left", PAD_BUTTON_LEFT},
    {"right", "D-pad Right", PAD_BUTTON_RIGHT},
}};

struct NativeButtonItem {
    const char* configName;
    const char* label;
    uint32_t nativeButton;
};

constexpr std::array<NativeButtonItem, SDL_GAMEPAD_BUTTON_COUNT + 1> kNativeButtons = {{
    {"unmapped", "Unmapped / analog trigger", PAD_NATIVE_BUTTON_INVALID},
    {"south", "South (A / Cross)", SDL_GAMEPAD_BUTTON_SOUTH},
    {"east", "East (B / Circle)", SDL_GAMEPAD_BUTTON_EAST},
    {"west", "West (X / Square)", SDL_GAMEPAD_BUTTON_WEST},
    {"north", "North (Y / Triangle)", SDL_GAMEPAD_BUTTON_NORTH},
    {"back", "Back / Select", SDL_GAMEPAD_BUTTON_BACK},
    {"guide", "Guide / Home", SDL_GAMEPAD_BUTTON_GUIDE},
    {"start", "Start / Options", SDL_GAMEPAD_BUTTON_START},
    {"left_stick", "Left stick click", SDL_GAMEPAD_BUTTON_LEFT_STICK},
    {"right_stick", "Right stick click", SDL_GAMEPAD_BUTTON_RIGHT_STICK},
    {"left_shoulder", "Left shoulder", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},
    {"right_shoulder", "Right shoulder", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
    {"dpad_up", "D-pad Up", SDL_GAMEPAD_BUTTON_DPAD_UP},
    {"dpad_down", "D-pad Down", SDL_GAMEPAD_BUTTON_DPAD_DOWN},
    {"dpad_left", "D-pad Left", SDL_GAMEPAD_BUTTON_DPAD_LEFT},
    {"dpad_right", "D-pad Right", SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
    {"misc1", "Misc 1 / Share", SDL_GAMEPAD_BUTTON_MISC1},
    {"right_paddle1", "Right paddle 1", SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1},
    {"left_paddle1", "Left paddle 1", SDL_GAMEPAD_BUTTON_LEFT_PADDLE1},
    {"right_paddle2", "Right paddle 2", SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2},
    {"left_paddle2", "Left paddle 2", SDL_GAMEPAD_BUTTON_LEFT_PADDLE2},
    {"touchpad", "Touchpad", SDL_GAMEPAD_BUTTON_TOUCHPAD},
    {"misc2", "Misc 2", SDL_GAMEPAD_BUTTON_MISC2},
    {"misc3", "Misc 3 / GC L click", SDL_GAMEPAD_BUTTON_MISC3},
    {"misc4", "Misc 4 / GC R click", SDL_GAMEPAD_BUTTON_MISC4},
    {"misc5", "Misc 5", SDL_GAMEPAD_BUTTON_MISC5},
    {"misc6", "Misc 6", SDL_GAMEPAD_BUTTON_MISC6},
}};

// Classic Controller Pro layout, indexed like kControllerButtons: the SNES-style
// diamond (A right, B bottom, X top, Y left) with digital bumpers driving the GC
// triggers and Z on Back/Select (the same home the NSO GC default gives it).
constexpr std::array<const char*, PAD_BUTTON_COUNT> kClassicProPreset = {
    "east",           // A
    "south",          // B
    "north",          // X
    "west",           // Y
    "start",          // Start
    "back",           // Z
    "left_shoulder",  // L
    "right_shoulder", // R
    "dpad_up", "dpad_down", "dpad_left", "dpad_right",
};

struct ResolutionItem {
    const char* label;
    float scale;
};

using Clock = std::chrono::steady_clock;

constexpr auto kCursorAutoHideDelay = std::chrono::seconds(5);
Clock::time_point g_lastMouseActivity{Clock::now()};
bool g_cursorHidden = false;

constexpr std::array<std::string_view, 3> kDisplayModeConfigNames = {
    "windowed", "borderless", "exclusive",
};

uint64_t g_presentedFrame = 0;
std::atomic_bool g_strapInputAccepted = false;
std::atomic_uint64_t g_startupDismissFrame = UINT64_MAX;
constexpr uint64_t kStrapTransitionCoverFrames = 60;

constexpr std::array<ResolutionItem, 8> kResolutions = {{
    {"Auto (window size)", 0.0f}, {"Native (1x)", 1.0f}, {"1.5x", 1.5f}, {"2x", 2.0f},
    {"3x", 3.0f}, {"4x", 4.0f}, {"6x", 6.0f}, {"8x", 8.0f},
}};

constexpr std::array<uint32_t, 3> kFrameInterpolationTargetFps{0, 120, 180};

bool IsHighResolutionScale(float scale) {
    return std::fabs(scale - 6.0f) < 0.001f || std::fabs(scale - 8.0f) < 0.001f;
}

bool IsHighFrameRateMode() {
    return kFrameInterpolationTargetFps[static_cast<size_t>(g_frameInterpolationMode)] > 60;
}

void SetResolutionScale(float scale) {
    g_resolutionScale = scale;
    VISetFrameBufferScale(scale);
    RuntimeConfigFile::SetResolutionMultiplier(scale);
}

void LimitResolutionForFrameRate() {
    if (IsHighFrameRateMode() && IsHighResolutionScale(g_resolutionScale)) {
        SetResolutionScale(4.0f);
    }
}

const NativeButtonItem* FindNativeButton(std::string value) {
    const auto it = std::find_if(kNativeButtons.begin(), kNativeButtons.end(), [&](const NativeButtonItem& item) {
        return value == item.configName;
    });
    return it == kNativeButtons.end() ? nullptr : &*it;
}

struct ControllerBindingPair {
    std::string primary;
    std::string secondary;
};

std::string TrimBindingToken(const std::string& token) {
    const size_t begin = token.find_first_not_of(" \t");
    if (begin == std::string::npos) {
        return {};
    }
    const size_t end = token.find_last_not_of(" \t");
    return token.substr(begin, end - begin + 1);
}

// Config values hold up to two comma-separated button names ("dpad_up" or
// "dpad_up,left_shoulder"); pressing either one counts as the GC button.
ControllerBindingPair SplitControllerBinding(const std::string& value) {
    const size_t comma = value.find(',');
    if (comma == std::string::npos) {
        return {TrimBindingToken(value), {}};
    }
    return {TrimBindingToken(value.substr(0, comma)), TrimBindingToken(value.substr(comma + 1))};
}

const NativeButtonItem& NativeButtonForValue(uint32_t nativeButton) {
    const auto it = std::find_if(kNativeButtons.begin(), kNativeButtons.end(), [&](const NativeButtonItem& item) {
        return nativeButton == item.nativeButton;
    });
    return it == kNativeButtons.end() ? kNativeButtons.front() : *it;
}

void SetTopBarVisible(bool visible) {
    if (g_topBarVisible == visible) {
        return;
    }
    g_topBarVisible = visible;
}

void ApplyConfiguredMappings() {
    for (uint32_t port = 0; port < PAD_MAX_CONTROLLERS; ++port) {
        const int32_t controllerIndex = PADGetIndexForPort(port);
        if (controllerIndex == g_configuredControllerIndices[port]) {
            continue;
        }
        g_configuredControllerIndices[port] = controllerIndex;
        if (controllerIndex < 0) {
            continue;
        }
        // The [controller] bindings are positional and shared by every port, so
        // they describe whatever pad the user set them up with (usually an Xbox
        // layout: a = south). A Wii U Pro Controller has a fixed, known layout
        // (A on the east position) that aurora already maps by name; applying
        // the shared bindings on top swaps A/B and X/Y. (Wii Remotes with any
        // extension never reach the PAD layer: the game reads them through KPAD.)
        if (WiiRemoteInput::KindForPort(port) == WiiRemoteInput::Kind::WiiUPro) {
            continue;
        }

        uint32_t count = 0;
        if (PADGetButtonMappings(port, &count) == nullptr || count != PAD_BUTTON_COUNT) {
            continue;
        }
        for (size_t i = 0; i < kControllerButtons.size(); ++i) {
            const auto& configured = RuntimeConfigFile::ControllerButton(i);
            if (!configured) {
                continue;
            }
            const ControllerBindingPair binding = SplitControllerBinding(*configured);
            if (const NativeButtonItem* native = FindNativeButton(binding.primary)) {
                PADSetButtonMapping(port, PADButtonMapping{native->nativeButton, kControllerButtons[i].padButton});
            } else {
                RT_LOG(RT_TAG_CONFIG) << "Unknown controller." << kControllerButtons[i].configKey
                          << " button '" << binding.primary << "'" << std::endl;
            }
            uint32_t altNative = PAD_NATIVE_BUTTON_INVALID;
            if (!binding.secondary.empty()) {
                if (const NativeButtonItem* native = FindNativeButton(binding.secondary)) {
                    altNative = native->nativeButton;
                } else {
                    RT_LOG(RT_TAG_CONFIG) << "Unknown controller." << kControllerButtons[i].configKey
                              << " secondary button '" << binding.secondary << "'" << std::endl;
                }
            }
            PADSetAltButtonMapping(port, PADButtonMapping{altNative, kControllerButtons[i].padButton});
        }
    }
}

bool g_wiiRemotesEnabled = RuntimeConfigFile::WiiRemotesEnabled(true);
bool g_wiiContinuousScan = RuntimeConfigFile::WiiContinuousScanEnabled(true);

// Accelerometer readout and zero-point calibration for a bare remote / remote + Nunchuk.
void DrawWiiRemoteAccelerometer(uint32_t port) {
    ImGui::SeparatorText("Accelerometer");
    float sdlG[3] = {};
    float kpad[3] = {};
    if (WiiRemoteInput::ReadAccelDebug(port, sdlG, kpad)) {
        ImGui::Text("KPAD acc: x %+.2f  y %+.2f  z %+.2f g", kpad[0], kpad[1], kpad[2]);
        ImGui::TextDisabled("Flat, buttons up: (0, -1, 0). Sideways as a wheel: (1, 0, 0); z follows the turn.");
    } else {
        ImGui::TextDisabled("No accelerometer data yet.");
    }
    // SDL's read of the remote's calibration block often times out over Bluetooth
    // and it falls back to a nominal zero point, leaving a small per-axis bias;
    // measured here with the remote at rest.
    if (WiiRemoteInput::IsAccelCalibrating()) {
        ImGui::ProgressBar(WiiRemoteInput::AccelCalibrationProgress(), ImVec2(220.0f, 0.0f), "Hold still...");
    } else if (ImGui::Button("Calibrate (remote lying flat, buttons up)")) {
        WiiRemoteInput::StartAccelCalibration(port);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Put the remote down on a flat surface with the buttons facing up and do not touch it\n"
                          "for about two seconds. Corrects the steering offset of a remote held sideways.");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!RuntimeConfigFile::HasWiiAccelOffset() || WiiRemoteInput::IsAccelCalibrating());
    if (ImGui::Button("Clear calibration")) {
        WiiRemoteInput::ClearAccelCalibration();
    }
    ImGui::EndDisabled();
    if (const char* message = WiiRemoteInput::AccelCalibrationMessage()) {
        ImGui::TextWrapped("%s", message);
    } else if (RuntimeConfigFile::HasWiiAccelOffset()) {
        const std::array<double, 3> offset = RuntimeConfigFile::WiiAccelOffset();
        ImGui::TextDisabled("Stored offset: x %+.3f  y %+.3f  z %+.3f g", offset[0], offset[1], offset[2]);
    } else {
        ImGui::TextDisabled("Not calibrated (using SDL's zero point; see console.log for \"fallback accelerometer calibration\").");
    }
}

// Wii Remotes (Bluetooth) menu: driver switch, pairing help, continuous scanning and the port's controller kind.
void DrawWiiRemoteSettings(uint32_t selectedGamePort) {
    if (!ImGui::BeginMenu("Wii Remotes (Bluetooth)")) {
        return;
    }
    if (ImGui::Checkbox("Use Wii Remotes / Wii U Pro Controllers", &g_wiiRemotesEnabled)) {
        RuntimeConfigFile::SetWiiRemotesEnabled(g_wiiRemotesEnabled);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Takes effect on the next launch. Turn this off if you use a Mayflash DolphinBar.");
    }
    ImGui::TextDisabled("Pairing: Windows Settings > Bluetooth > Add device, then press 1+2");
    ImGui::TextDisabled("(or the red SYNC button) on the remote. Leave the PIN empty.");
    ImGui::TextDisabled("A remote that was paired before also needs to be turned on with 1+2/SYNC.");
    if (ImGui::Checkbox("Keep scanning for Wii Remotes (like Dolphin's Continuous Scanning)",
                        &g_wiiContinuousScan)) {
        RuntimeConfigFile::SetWiiContinuousScanEnabled(g_wiiContinuousScan);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("While no Wii controller is connected, re-check Bluetooth every 2 seconds so a\n"
                          "remote that dropped out (\"Communications with the controller have been\n"
                          "interrupted\") or was turned on after launch comes back by itself.");
    }
    // The driver hint is only read at launch, so a rescan after the user turned
    // the setting off would still re-enumerate Wii devices in this session.
    ImGui::BeginDisabled(!g_wiiRemotesEnabled);
    if (ImGui::Button("Rescan now")) {
        WiiRemoteInput::RescanNow();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (WiiRemoteInput::IsScanning()) {
        ImGui::TextDisabled("Scanning... (%u so far) - press 1+2 on the remote", WiiRemoteInput::ScanCount());
    } else {
        ImGui::TextDisabled("Not scanning");
    }
    ImGui::Separator();

    const WiiRemoteInput::Kind kind = WiiRemoteInput::KindForPort(selectedGamePort);
    ImGui::Text("Port %u: %s", static_cast<unsigned>(selectedGamePort + 1), WiiRemoteInput::KindLabel(kind));
    if (kind == WiiRemoteInput::Kind::RemoteWithClassic) {
        WiiRemoteInput::KpadSample sample;
        if (WiiRemoteInput::ReadKpadSample(selectedGamePort, sample)) {
            // WPAD_CL_BUTTON_* bits, in the game's own layout (no mapping involved).
            const auto held = [&](uint32_t bit, const char* on, const char* off) { return (sample.clHold & bit) ? on : off; };
            ImGui::Text("Classic: %s %s %s %s  %s %s  %s %s  %s %s  %s %s %s %s", held(0x0010, "A", "a"),
                        held(0x0040, "B", "b"), held(0x0008, "X", "x"), held(0x0020, "Y", "y"), held(0x2000, "L", "l"),
                        held(0x0200, "R", "r"), held(0x0080, "ZL", "zl"), held(0x0004, "ZR", "zr"),
                        held(0x0400, "PLUS", "plus"), held(0x1000, "MINUS", "minus"), held(0x0001, "UP", "up"),
                        held(0x4000, "DOWN", "down"), held(0x0002, "LEFT", "left"), held(0x8000, "RIGHT", "right"));
            ImGui::Text("Sticks: L %+.2f %+.2f (WPAD %+d %+d)  R %+.2f %+.2f (WPAD %+d %+d)", sample.clLStick[0],
                        sample.clLStick[1], static_cast<int>(sample.clLStickRaw[0]),
                        static_cast<int>(sample.clLStickRaw[1]), sample.clRStick[0], sample.clRStick[1],
                        static_cast<int>(sample.clRStickRaw[0]), static_cast<int>(sample.clRStickRaw[1]));
            ImGui::TextDisabled("Capitals = held. The game reads this Classic Controller through KPAD, as on the");
            ImGui::TextDisabled("console: its buttons mean what the game says they mean, no mapping applies.");
        }
    }
    if (kind == WiiRemoteInput::Kind::WiiUPro) {
        if (SDL_Gamepad* gamepad = SDL_GetGamepadFromPlayerIndex(static_cast<int>(selectedGamePort))) {
            // SDL's Wii driver posts the D-pad as joystick buttons 11-14 (the
            // SDL_GAMEPAD_BUTTON_DPAD_* values) while its default HIDAPI mapping
            // expects a hat, so SDL_GetGamepadButton never sees them; read the
            // joystick directly, like the fallback in aurora's PADRead does.
            SDL_Joystick* joystick = SDL_GetGamepadJoystick(gamepad);
            const auto rawButton = [&](int index) {
                return joystick != nullptr && SDL_GetJoystickButton(joystick, index);
            };
            ImGui::Text("Raw D-pad: %s %s %s %s", rawButton(SDL_GAMEPAD_BUTTON_DPAD_UP) ? "UP" : "up",
                        rawButton(SDL_GAMEPAD_BUTTON_DPAD_DOWN) ? "DOWN" : "down",
                        rawButton(SDL_GAMEPAD_BUTTON_DPAD_LEFT) ? "LEFT" : "left",
                        rawButton(SDL_GAMEPAD_BUTTON_DPAD_RIGHT) ? "RIGHT" : "right");
            ImGui::Text("Raw face buttons: %s %s %s %s", rawButton(SDL_GAMEPAD_BUTTON_EAST) ? "A" : "a",
                        rawButton(SDL_GAMEPAD_BUTTON_SOUTH) ? "B" : "b", rawButton(SDL_GAMEPAD_BUTTON_NORTH) ? "X" : "x",
                        rawButton(SDL_GAMEPAD_BUTTON_WEST) ? "Y" : "y");
            ImGui::Text("Raw ZL/ZR: %d / %d (pressed above 0)",
                        SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER),
                        SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
            ImGui::TextDisabled("Capitals = held. If a button never turns to capitals while physically held,");
            ImGui::TextDisabled("that press is not reaching SDL at all (a driver-level issue, not a mapping one).");
            ImGui::TextDisabled("This pad uses Nintendo's own layout (a/b/x/y as labelled); the shared");
            ImGui::TextDisabled("button mapping above does not apply to it.");
        }
    }
    if (kind == WiiRemoteInput::Kind::Remote || kind == WiiRemoteInput::Kind::RemoteWithNunchuk ||
        kind == WiiRemoteInput::Kind::RemoteWithClassic) {
        DrawWiiRemoteAccelerometer(selectedGamePort);
    }

    ImGui::EndMenu();
}

// Controller settings menu: port selection, controller assignment and button mapping.
void DrawControllerSettings() {
    if (ImGui::CollapsingHeader("Hardware wheel and pedals")) physical_wheel::DrawSettings();
    for (int port = 0; port < PAD_MAX_CONTROLLERS; ++port) {
        const std::string label = "Port " + std::to_string(port + 1);
        ImGui::RadioButton(label.c_str(), &g_controllerPort, port);
        if (port + 1 < PAD_MAX_CONTROLLERS) {
            ImGui::SameLine();
        }
    }

    ImGui::Separator();
    const uint32_t selectedGamePort = static_cast<uint32_t>(g_controllerPort);
    const char* currentName = PADGetName(selectedGamePort);
    ImGui::Text("Assigned: %s", currentName != nullptr ? currentName : "None");
    if (ImGui::MenuItem("Unassign controller")) {
        PADClearPort(selectedGamePort);
        g_configuredControllerIndices.fill(std::numeric_limits<int32_t>::min());
    }
    ImGui::Separator();
    controller_mapping_wizard::DrawSetupList();
    DrawWiiRemoteSettings(selectedGamePort);
    const uint32_t controllerCount = PADCount();
    if (controllerCount == 0) {
        ImGui::TextDisabled("No controller connected");
        return;
    }

    if (ImGui::BeginMenu("Assign connected controller")) {
        for (uint32_t index = 0; index < controllerCount; ++index) {
            const char* name = PADGetNameForControllerIndex(index);
            ImGui::PushID(static_cast<int>(index));
            if (ImGui::MenuItem(name != nullptr ? name : "Unknown controller")) {
                PADSetPortForIndex(index, selectedGamePort);
                g_configuredControllerIndices.fill(std::numeric_limits<int32_t>::min());
                ApplyConfiguredMappings();
            }
            ImGui::PopID();
        }
        ImGui::EndMenu();
    }

    uint32_t mappingCount = 0;
    PADButtonMapping* mappings = PADGetButtonMappings(static_cast<uint32_t>(g_controllerPort), &mappingCount);
    if (mappings == nullptr || mappingCount != PAD_BUTTON_COUNT) {
        ImGui::TextDisabled("Assign a controller to edit its buttons");
        return;
    }

    uint32_t altMappingCount = 0;
    PADButtonMapping* altMappings =
        PADGetAltButtonMappings(static_cast<uint32_t>(g_controllerPort), &altMappingCount);

    const auto writeBinding = [](size_t index, uint32_t primaryNative, uint32_t altNative) {
        std::string value = NativeButtonForValue(primaryNative).configName;
        if (altNative != PAD_NATIVE_BUTTON_INVALID) {
            value += ',';
            value += NativeButtonForValue(altNative).configName;
        }
        RuntimeConfigFile::SetControllerButton(index, value);
    };

    // Which rows show the second-binding combo without one being bound yet;
    // reset when the user switches ports so a stale "+" click doesn't linger.
    static std::array<bool, PAD_BUTTON_COUNT> altRowExpanded{};
    static int altRowExpandedPort = -1;
    if (altRowExpandedPort != g_controllerPort) {
        altRowExpandedPort = g_controllerPort;
        altRowExpanded.fill(false);
    }

    ImGui::SeparatorText("Presets");
    if (ImGui::Button("GameCube")) {
        const uint32_t port = static_cast<uint32_t>(g_controllerPort);
        PADRestoreDefaultMapping(port);
        uint32_t restoredCount = 0;
        if (PADButtonMapping* restored = PADGetButtonMappings(port, &restoredCount)) {
            for (size_t i = 0; i < kControllerButtons.size(); ++i) {
                const auto it = std::find_if(restored, restored + restoredCount, [&](const PADButtonMapping& mapping) {
                    return mapping.padButton == kControllerButtons[i].padButton;
                });
                if (it != restored + restoredCount) {
                    RuntimeConfigFile::SetControllerButton(i, NativeButtonForValue(it->nativeButton).configName);
                }
            }
        }
        altRowExpanded.fill(false);
        PADSerializeMappings();
        mappings = PADGetButtonMappings(port, &mappingCount);
    }
    ImGui::SameLine();
    if (ImGui::Button("Classic Controller Pro")) {
        const uint32_t port = static_cast<uint32_t>(g_controllerPort);
        for (size_t i = 0; i < kControllerButtons.size(); ++i) {
            if (const NativeButtonItem* native = FindNativeButton(kClassicProPreset[i])) {
                PADSetButtonMapping(port, PADButtonMapping{native->nativeButton, kControllerButtons[i].padButton});
                PADSetAltButtonMapping(port,
                                       PADButtonMapping{PAD_NATIVE_BUTTON_INVALID, kControllerButtons[i].padButton});
                RuntimeConfigFile::SetControllerButton(i, kClassicProPreset[i]);
            }
        }
        altRowExpanded.fill(false);
        PADSerializeMappings();
        mappings = PADGetButtonMappings(port, &mappingCount);
    }

    ImGui::SeparatorText("Button mapping");
    for (size_t i = 0; i < kControllerButtons.size(); ++i) {
        auto mappingIt = std::find_if(mappings, mappings + mappingCount, [&](const PADButtonMapping& mapping) {
            return mapping.padButton == kControllerButtons[i].padButton;
        });
        if (mappingIt == mappings + mappingCount) {
            continue;
        }
        PADButtonMapping* altIt = nullptr;
        if (altMappings != nullptr && altMappingCount == PAD_BUTTON_COUNT) {
            const auto it = std::find_if(altMappings, altMappings + altMappingCount, [&](const PADButtonMapping& mapping) {
                return mapping.padButton == kControllerButtons[i].padButton;
            });
            if (it != altMappings + altMappingCount) {
                altIt = it;
            }
        }

        const NativeButtonItem& current = NativeButtonForValue(mappingIt->nativeButton);
        ImGui::PushID(static_cast<int>(i));
        ImGui::SetNextItemWidth(190.0f);
        if (ImGui::BeginCombo("##primary", current.label)) {
            for (const auto& candidate : kNativeButtons) {
                const bool selected = candidate.nativeButton == mappingIt->nativeButton;
                if (ImGui::Selectable(candidate.label, selected)) {
                    const uint32_t port = static_cast<uint32_t>(g_controllerPort);
                    PADSetButtonMapping(port, PADButtonMapping{candidate.nativeButton, kControllerButtons[i].padButton});
                    writeBinding(i, candidate.nativeButton,
                                 altIt != nullptr ? altIt->nativeButton : PAD_NATIVE_BUTTON_INVALID);
                    PADSerializeMappings();
                    mappings = PADGetButtonMappings(port, &mappingCount);
                }
                if (selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        if (altIt != nullptr) {
            const bool altBound = altIt->nativeButton != PAD_NATIVE_BUTTON_INVALID;
            if (!altBound && !altRowExpanded[i]) {
                ImGui::SameLine();
                if (ImGui::SmallButton("+")) {
                    altRowExpanded[i] = true;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Add a second binding; pressing either one works");
                }
            } else {
                ImGui::SameLine();
                ImGui::TextUnformatted("or");
                ImGui::SameLine();
                const char* altLabel = altBound ? NativeButtonForValue(altIt->nativeButton).label : "None";
                ImGui::SetNextItemWidth(190.0f);
                if (ImGui::BeginCombo("##alt", altLabel)) {
                    for (const auto& candidate : kNativeButtons) {
                        const bool isNone = candidate.nativeButton == PAD_NATIVE_BUTTON_INVALID;
                        const bool selected = candidate.nativeButton == altIt->nativeButton;
                        if (ImGui::Selectable(isNone ? "None" : candidate.label, selected)) {
                            const uint32_t port = static_cast<uint32_t>(g_controllerPort);
                            PADSetAltButtonMapping(
                                port, PADButtonMapping{candidate.nativeButton, kControllerButtons[i].padButton});
                            writeBinding(i, mappingIt->nativeButton, candidate.nativeButton);
                            if (isNone) {
                                altRowExpanded[i] = false;
                            }
                        }
                        if (selected) {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::EndCombo();
                }
            }
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(kControllerButtons[i].label);
        ImGui::PopID();
    }
}

void DrawAudioSettings() {
    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::SliderInt("Master", &g_audioVolumePercent, 0, 100, "%d%%")) {
        const float volume = static_cast<float>(g_audioVolumePercent) / 100.0f;
        AudioBackend::Instance().SetMasterVolume(volume);
        RuntimeConfigFile::SetAudioVolume(volume);
    }
    if (ImGui::SliderInt("Music", &g_musicVolumePercent, 0, 100, "%d%%")) {
        const float volume = static_cast<float>(g_musicVolumePercent) / 100.0f;
        MusicAttenuation::SetMusicVolume(volume);
        RuntimeConfigFile::SetMusicVolume(volume);
    }
    if (ImGui::SliderInt("Sound Effects", &g_soundEffectsVolumePercent, 0, 100, "%d%%")) {
        const float volume = static_cast<float>(g_soundEffectsVolumePercent) / 100.0f;
        MusicAttenuation::SetSoundEffectsVolume(volume);
        RuntimeConfigFile::SetSoundEffectsVolume(volume);
    }
    if (ImGui::SliderInt("UI", &g_uiVolumePercent, 0, 100, "%d%%")) {
        const float volume = static_cast<float>(g_uiVolumePercent) / 100.0f;
        MusicAttenuation::SetUiVolume(volume);
        RuntimeConfigFile::SetUiVolume(volume);
    }
    if (ImGui::SliderInt("Voices", &g_voicesVolumePercent, 0, 100, "%d%%")) {
        const float volume = static_cast<float>(g_voicesVolumePercent) / 100.0f;
        MusicAttenuation::SetVoicesVolume(volume);
        RuntimeConfigFile::SetVoicesVolume(volume);
    }
    if (ImGui::Checkbox("Mute", &g_audioMuted)) {
        AudioBackend::Instance().SetMuted(g_audioMuted);
        RuntimeConfigFile::SetAudioMuted(g_audioMuted);
    }
    ImGui::Separator();
    if (ImGui::Checkbox("Mix audio on a worker thread", &g_audioMixWorker)) {
        // Applies immediately: SetMixWorkerEnabled joins any in-flight mix
        // before switching, so the change never lands mid-frame.
        AxDspHle::SetMixWorkerEnabled(g_audioMixWorker);
        RuntimeConfigFile::SetAudioMixWorker(g_audioMixWorker);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Runs the AX/DSP voice mix off the game thread. Turn this off if you "
            "suspect an audio problem; the mix then runs inline as it used to.");
    }
    ImGui::Separator();
    if (ImGui::Checkbox("Mute game music while external media is playing",
                        &g_attenuateMusicWhenMediaPlays)) {
        MusicAttenuation::SetEnabled(g_attenuateMusicWhenMediaPlays);
        RuntimeConfigFile::SetAttenuateMusicWhenMediaPlays(g_attenuateMusicWhenMediaPlays);
    }
    if (g_attenuateMusicWhenMediaPlays) {
        if (MusicAttenuation::IsExternalMediaPlaying()) {
            ImGui::TextDisabled("External media is playing; game music is muted.");
        } else if (!MusicAttenuation::IsMediaControlInitializationComplete()) {
            ImGui::TextDisabled("Waiting for media controls...");
        } else if (!MusicAttenuation::IsMediaControlAvailable()) {
            ImGui::TextDisabled("Media controls are unavailable.");
        } else {
            ImGui::TextDisabled("No external media is currently playing.");
        }
    }
}

// The virtual screen's placement comes from the launch-time [vr] geometry, the
// same metres the menu quad is built from, converted into the world units the
// eye replay works in. Those units follow the camera: the first-person view
// renders at its own scale, and the screen has to be sized at the same one or
// it would not stay 2 m across in front of the player.
void ApplyVrHudVirtualScreen() {
    const float unitsPerMeter = mkw::vr::MkwVRPolicyGetSnapshot().EffectiveUnitsPerMeter();
    aurora_set_stereo_hud_screen(g_vrHudVirtualScreen,
                                 RuntimeConfigFile::VrHudWidthMeters(2.4f) * unitsPerMeter,
                                 RuntimeConfigFile::VrHudDistanceMeters(2.0f) * unitsPerMeter);
}

void SetVrSettingsVisible(bool visible) {
    g_vrSettingsFocus = visible && !g_vrSettingsVisible;
    g_vrSettingsVisible = visible;
    mkw::vr::MkwVRPolicySetSettingsVisible(visible);
}

void DrawVrStickSettings(const mkw::vr::QuestInput& input) {
    static bool calibrating = false;
    static auto started = Clock::now();
    static float sumX = 0, sumY = 0, minX = 1, maxX = -1, minY = 1, maxY = -1;
    static int samples = 0;
    static const char* message = "Release the stick to check its center, then push it left and right.";
    const auto mapped = mkw::vr::MapQuestInput(input, g_vrStickCalibration);
    ImGui::Text("Raw stick: X %+.2f   Y %+.2f", input.steering_x, input.steering_y);
    ImGui::Text("Steering sent to the game: %d %%", static_cast<int>(mapped.stickX));
    ImGui::ProgressBar((mapped.stickX + 100.0f) / 200.0f, ImVec2(-1, 0), "Left                   Center                   Right");
    ImGui::TextWrapped("%s", message);
    bool changed = false;
    if (!input.active) ImGui::TextDisabled("Quest controller is not being tracked.");
    ImGui::BeginDisabled(!input.active || calibrating);
    if (ImGui::Button("Calibrate center (release the stick)")) {
        started = Clock::now(); calibrating = true; samples = 0; sumX = sumY = 0;
        minX = minY = 1; maxX = maxY = -1;
        message = "Release the stick and wait for two seconds.";
    }
    ImGui::EndDisabled();
    if (calibrating) {
        const float elapsed = std::chrono::duration<float>(Clock::now() - started).count();
        if (!input.active) { calibrating = false; message = "Calibration canceled: controller is not being tracked."; }
        else if (elapsed >= 1 && elapsed < 2) {
            sumX += input.steering_x; sumY += input.steering_y; ++samples;
            minX = std::min(minX, input.steering_x); maxX = std::max(maxX, input.steering_x);
            minY = std::min(minY, input.steering_y); maxY = std::max(maxY, input.steering_y);
        } else if (elapsed >= 2) {
            calibrating = false;
            if (samples >= 10 && maxX - minX < 0.08f && maxY - minY < 0.08f &&
                std::abs(sumX / samples) <= 0.3f && std::abs(sumY / samples) <= 0.3f) {
                g_vrStickCalibration.center_x = sumX / samples;
                g_vrStickCalibration.center_y = sumY / samples;
                changed = true; message = "Stick center calibrated and saved.";
            } else message = "The stick moved too much or was unstable. Release it and try again.";
        }
    }
    ImGui::BeginDisabled(calibrating);
    float deadzone = g_vrStickCalibration.deadzone * 100;
    float outer = g_vrStickCalibration.outer * 100;
    if (ImGui::SliderFloat("Deadzone", &deadzone, 0, 40, "%.0f %%")) {
        g_vrStickCalibration.deadzone = deadzone / 100; changed = true;
    }
    if (ImGui::SliderFloat("Travel for full steering", &outer, 60, 100, "%.0f %%")) {
        g_vrStickCalibration.outer = outer / 100; changed = true;
    }
    ImGui::TextWrapped("Increase the deadzone if steering moves while the stick is at rest. Reduce travel if pushing the stick fully does not reach 100 %%.");
    if (ImGui::Button("Reset stick settings")) {
        g_vrStickCalibration = {}; changed = true; message = "Stick settings reset.";
    }
    ImGui::EndDisabled();
    if (changed) {
        mkw::vr::SetQuestStickCalibration(g_vrStickCalibration);
        RuntimeConfigFile::SetVrStickCalibration(g_vrStickCalibration.deadzone, g_vrStickCalibration.outer,
            g_vrStickCalibration.center_x, g_vrStickCalibration.center_y);
    }
}

#include "vr/onboarding_overlay.inl"

void DrawVrSettings() {
    const auto input = mkw::vr::ReadQuestInputSnapshot();
    if(g_vrSettingsVisible) VrPointer(input);
    static bool chordHeld = false;
    const bool chord = input.active && mkw::vr::QuestAxis(input.item) > 0.5f && input.trick;
    if (chord && !chordHeld && g_tutorial.stage!=mkw::vr::TutorialFlow::Stage::Showing) SetVrSettingsVisible(!g_vrSettingsVisible);
    chordHeld = chord;
    auto& io = ImGui::GetIO();
    static bool wasNavigating = false;
    const bool navigating = g_vrSettingsVisible && input.active;
    if (navigating || wasNavigating) {
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.AddKeyEvent(ImGuiKey_UpArrow, navigating && input.steering_y > 0.5f);
        io.AddKeyEvent(ImGuiKey_DownArrow, navigating && input.steering_y < -0.5f);
        io.AddKeyEvent(ImGuiKey_LeftArrow, navigating && input.steering_x < -0.5f);
        io.AddKeyEvent(ImGuiKey_RightArrow, navigating && input.steering_x > 0.5f);
        io.AddKeyEvent(ImGuiKey_Enter, navigating && input.confirm);
        io.AddKeyEvent(ImGuiKey_Escape, navigating && input.brake);
    }
    wasNavigating = navigating;
    if (!g_vrSettingsVisible) return;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(680.0f, io.DisplaySize.x * 0.94f), io.DisplaySize.y * 0.92f), ImGuiCond_Always);
    if (g_vrSettingsFocus) ImGui::SetNextWindowFocus();
    bool visible = true;
    if (ImGui::Begin("VR Settings", &visible, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize)) {
        ImGui::TextWrapped("Point with either controller and squeeze its trigger to select. Stick: navigate / adjust. Right A: confirm, B: back. Left %s + %s: close.",ControllerKey(0,1),ControllerKey(0,2));
        if (g_vrSettingsFocus) ImGui::SetKeyboardFocusHere();
        if (ImGui::Button("Return to game")) visible = false;
        ImGui::Separator();
        if (ImGui::BeginTabBar("VR categories")) {
            if (ImGui::BeginTabItem("Graphics")) {
                int shaderQuality=RuntimeConfigFile::Get().vrMenuShaderQuality;
                if(ImGui::Combo("Menu shader quality",&shaderQuality,"Off\0Low\0Balanced\0High\0")) {
                    if(RuntimeConfigFile::WriteSetting("vr","menu_shader_quality",std::to_string(shaderQuality))) {
                        RuntimeConfigFile::Mutable().vrMenuShaderQuality=shaderQuality;
                        aurora_set_vr_menu_shader_quality(shaderQuality);
                    }
                }
                ImGui::TextWrapped("Applies immediately. Lower quality reduces the GPU cost of the animated menu environment.");
                float scale = RuntimeConfigFile::VrRenderScale();
                constexpr float scales[]{0.65f, 0.80f, 1.0f, 1.20f};
                constexpr const char* names[]{"Performance", "Balanced", "Quality", "Ultra"};
                for (int i = 0; i < 4; ++i) {
                    if (i) ImGui::SameLine();
                    if (ImGui::RadioButton(names[i], std::abs(scale - scales[i]) < 0.001f)) {
                        scale = scales[i]; RuntimeConfigFile::SetVrRenderScale(scale);
                    }
                }
                float percent = scale * 100.0f;
                if (ImGui::SliderFloat("Resolution per eye", &percent, 50, 150, "%.0f %%"))
                    RuntimeConfigFile::SetVrRenderScale(percent / 100.0f);
                ImGui::TextWrapped("The resolution is saved for the next launch. A sharper image requires more GPU power.");
                bool adaptive=RuntimeConfigFile::Get().vrAdaptiveResolution;
                if(ImGui::Checkbox("Adaptive resolution (experimental)",&adaptive)) RuntimeConfigFile::SetVrAdaptiveResolution(adaptive);
                ImGui::TextWrapped("Optional: lowers internal race resolution to 70-100%% of the selected quality when new images fall behind, then upscales to the headset. Slow steps reduce oscillation. Does not fix CPU bottlenecks; disabled by default.");
                if (ImGui::Checkbox("Sharp image (disable Wii copy filter)", &g_disableCopyFilter)) {
                    aurora_set_disable_copy_filter(g_disableCopyFilter);
                    RuntimeConfigFile::SetDisableCopyFilter(g_disableCopyFilter);
                }
                if (ImGui::Checkbox("Show FPS", &g_showFps)) RuntimeConfigFile::SetShowFps(g_showFps);
                ImGui::TextWrapped("Set the headset refresh rate in Quest Link, SteamVR, or Virtual Desktop. Game speed stays normal.");
                const float rates[]{0,72,80,90,120};
                const char* rateNames[]{"Runtime default", "72 Hz", "80 Hz", "90 Hz", "120 Hz"};
                int rate=0;
                for(int i=1;i<5;++i) if(RuntimeConfigFile::Get().vrRefreshHz==rates[i]) rate=i;
                if(ImGui::Combo("Preferred refresh (next launch)", &rate, rateNames, 5)) RuntimeConfigFile::SetVrRefreshHz(rates[rate]);
                ImGui::TextWrapped("Only applied if supported by your runtime. Display FPS can include repeated images; new-image FPS in the log measures fresh rendering.");
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Cameras")) {
                int mode = static_cast<int>(mkw::vr::MkwVRGetCameraMode());
                const char* modes[]{"Original camera", "First person", "Diorama"};
                if (ImGui::Combo("View", &mode, modes, 3))
                    mkw::vr::MkwVRSetCameraMode(static_cast<mkw::vr::CameraMode>(mode));
                ImGui::TextDisabled("Press the right stick to switch views.");
                bool nativeWheel = RuntimeConfigFile::VrNativeSteeringWheel();
                if (ImGui::Checkbox("Use the vehicle's original steering wheel", &nativeWheel))
                    RuntimeConfigFile::SetVrNativeSteeringWheel(nativeWheel);
                ImGui::TextWrapped("First person: grab at the vehicle's original wheel or handlebars. Adds steering rotation to static kart wheel geometry. Turn off to use the VR wheel. Falls back to the VR wheel if vehicle grip positions are unavailable.");
                ImGui::TextWrapped("Motorbikes use handlebars with grip zones fitted to the bike. Turn horizontally: pull your right hand back to turn right, or your left hand back to turn left. 45 degrees gives full steering. With the original model disabled, a VR handlebar replaces the circular wheel.");
                ImGui::TextWrapped("Once grabbed, controls stay attached until you release the grip. Larger grab zones help with wide handlebars. Bike handlebars stay level during banking. The seat automatically stays behind the controls.");
                ImGui::TextWrapped("Large characters use an automatic cockpit world scale. Lightning also shrinks your VR viewpoint and makes the track appear larger. Small kart wheels have an enlarged grab area, including the centre.");
                ImGui::TextWrapped("Races start in your chosen default camera. Hold the left trigger to brake and reverse. Y: item. SteamVR: tap X for tricks, hold X to pause Mario Kart. X + Y: VR settings.");
                int defaultCamera=RuntimeConfigFile::Get().vrDefaultCamera;
                if(ImGui::Combo("Default race camera",&defaultCamera,"Original / third person\0First person\0Diorama\0")) {
                    if(RuntimeConfigFile::WriteSetting("vr","default_camera",std::to_string(defaultCamera))) RuntimeConfigFile::Mutable().vrDefaultCamera=defaultCamera;
                }
                if(ImGui::Button("Show control tutorials again")) {
                    const bool tutorialReset = RuntimeConfigFile::WriteSetting("vr","tutorial_completed","0");
                    const bool welcomeReset = RuntimeConfigFile::WriteSetting("vr","welcome_complete","false");
                    if(tutorialReset && welcomeReset) {
                        RuntimeConfigFile::Mutable().vrTutorialCompleted=0;
                        RuntimeConfigFile::Mutable().vrWelcomeComplete=false;
                        g_tutorial={};
                        SetVrSettingsVisible(false);
                    }
                }
                if (mode == 1) {
                    if (nativeWheel && (!mkw::vr::MkwVRFirstPersonGetAnchor().native_wheel.valid ||
                        !mkw::vr::MkwVRFirstPersonGetAnchor().native_mesh_prepared))
                        ImGui::TextWrapped("Original grip positions or animated mesh unavailable: using the VR control for now.");
                    bool changed = false;
                    float eyeUp = g_vrFirstPersonHeadUp - 1.1f;
                    float eyeForward = g_vrFirstPersonHeadForward - 1.2f;
                    changed |= ImGui::SliderFloat("Eye height adjustment", &eyeUp, -0.5f, 0.5f, "%.2f m");
                    changed |= ImGui::SliderFloat("Eye forward adjustment", &eyeForward, -0.5f, 0.5f, "%.2f m");
                    g_vrFirstPersonHeadUp = eyeUp + 1.1f;
                    g_vrFirstPersonHeadForward = eyeForward + 1.2f;
                    changed |= ImGui::SliderFloat("Horizontal offset", &g_vrFirstPersonHeadRight, -0.5f, 0.5f, "%.2f m");
                    if (ImGui::Button("Reset seat position")) {
                        g_vrFirstPersonHeadUp = 1.1f; g_vrFirstPersonHeadForward = 1.2f;
                        g_vrFirstPersonHeadRight = 0; changed = true;
                    }
                    if (changed) {
                        RuntimeConfigFile::SetVrFirstPersonHeadUpMeters(g_vrFirstPersonHeadUp);
                        RuntimeConfigFile::SetVrFirstPersonHeadForwardMeters(g_vrFirstPersonHeadForward);
                        RuntimeConfigFile::SetVrFirstPersonHeadRightMeters(g_vrFirstPersonHeadRight);
                        mkw::vr::MkwVRFirstPersonApplyConfiguredSettings();
                    }
                    ImGui::TextWrapped("Eye position is calibrated from the character's seated riding posture, including bikes. The reference stays fixed in first person during animations. These sliders are additional adjustments. Recenter while sitting upright and looking straight ahead.");
                    ImGui::TextWrapped("Hold either grip near the wheel rim or handlebar ends to steer with your hands. Release both grips to use the left stick. Right trigger: accelerate. Left trigger: brake / reverse. Y: item. X: trick. A: hop / drift. B: brake.");
                }
                if (mode == 2) {
                    float distance = RuntimeConfigFile::VrDioramaDistance() / 100;
                    float height = RuntimeConfigFile::VrDioramaHeight() / 100;
                    float miniature = RuntimeConfigFile::VrDioramaUnitsPerMeter() / 100;
                    if (ImGui::SliderFloat("Distance behind the kart", &distance, 2, 50, "%.1f m")) RuntimeConfigFile::SetVrDioramaDistance(distance * 100);
                    if (ImGui::SliderFloat("Height above the kart", &height, 1, 40, "%.1f m")) RuntimeConfigFile::SetVrDioramaHeight(height * 100);
                    if (ImGui::SliderFloat("World scale", &miniature, 1, 30, "1 / %.1f")) RuntimeConfigFile::SetVrDioramaUnitsPerMeter(miniature * 100);
                    if (ImGui::Button("Reset diorama")) {
                        RuntimeConfigFile::SetVrDioramaDistance(1600);
                        RuntimeConfigFile::SetVrDioramaHeight(1200);
                        RuntimeConfigFile::SetVrDioramaUnitsPerMeter(1000);
                    }
                    ImGui::TextWrapped("The view follows the center of the kart with an independent miniature scale.");
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Display")) {
                float distance=RuntimeConfigFile::VrHudDistanceMeters(2.0f);
                float width=RuntimeConfigFile::VrHudWidthMeters(2.4f);
                bool hudChanged=ImGui::SliderFloat("Forward HUD distance", &distance, .5f, 5, "%.2f m");
                hudChanged |= ImGui::SliderFloat("Forward HUD width", &width, .5f, 4, "%.2f m");
                if(hudChanged) {
                    RuntimeConfigFile::SetVrHudDistanceMeters(distance);
                    RuntimeConfigFile::SetVrHudWidthMeters(width);
                    auto config=mkw::vr::MkwVRPolicyGetSnapshot().config;
                    config.hud_distance_meters=distance;config.hud_width_meters=width;
                    mkw::vr::MkwVRPolicyConfigure(config);
                }
                if (ImGui::Checkbox("Map and items on the left hand", &g_vrHudVirtualScreen)) {
                    RuntimeConfigFile::SetVrHudVirtualScreen(g_vrHudVirtualScreen);
                    ApplyVrHudVirtualScreen();
                }
                ImGui::TextWrapped("Clear this option to show the HUD in front of your eyes. Camera and display changes apply when you return to the game.");
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Stick")) {
                DrawVrStickSettings(input);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Hardware wheel")) {
                physical_wheel::DrawSettings();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Driving")) {
                bool swapItem=RuntimeConfigFile::Get().vrSwapItemTrick;
                bool swapDrift=RuntimeConfigFile::Get().vrSwapCockpitDriftBrake;
                bool mappingChanged=ImGui::Checkbox("Item: X / trick: Y (default: item Y / trick X)", &swapItem);
                mappingChanged |= ImGui::Checkbox("Cockpit drift: B / brake: A (default: drift A)", &swapDrift);
                if(mappingChanged) {
                    RuntimeConfigFile::SetVrButtonMapping(swapItem,swapDrift);
                    mkw::vr::SetQuestButtonMapping({swapItem,swapDrift});
                }
                ImGui::TextWrapped("X + Y always opens settings. Left trigger always brakes / reverses. Menu navigation stays A / B.");
                auto tuning=RuntimeConfigFile::VrWheelTuning();
                bool changed=false;
                changed |= ImGui::SliderFloat("Kart rotation for full steering", &tuning.kartDegrees, 20, 180, "%.0f degrees");
                changed |= ImGui::SliderFloat("Bike rotation for full steering", &tuning.bikeDegrees, 20, 90, "%.0f degrees");
                changed |= ImGui::SliderFloat("Grab depth tolerance", &tuning.grabDistance, .15f, .8f, "%.2f m");
                changed |= ImGui::SliderFloat("Grab assistance", &tuning.grabAssist, .7f, 2, "%.2f x");
                changed |= ImGui::SliderFloat("Steering response", &tuning.response, .5f, 2, "%.2f x");
                changed |= ImGui::SliderFloat("Tracking loss tolerance", &tuning.trackingGrace, .05f, .5f, "%.2f s");
                changed |= ImGui::Checkbox("Vibration on grab / release", &tuning.haptics);
                if(ImGui::Button("Reset driving settings")) { tuning={}; changed=true; }
                if(changed) RuntimeConfigFile::SetVrWheelTuning(tuning);
                ImGui::TextWrapped("Applies immediately. Grab tolerance only affects acquisition: release the grip to let go. Lower rotation reaches full steering sooner. The left stick still aims items forward or backward while holding the wheel.");
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Diagnostics")) {
                const auto d=mkw::vr::OpenXRGetDiagnostics();
                ImGui::Text("OpenXR: %s",mkw::vr::OpenXRIsRunning()?"running":"inactive");
                ImGui::Text("Displayed: %.1f FPS   New images: %.1f FPS", d.displayFps,d.newImageFps);
                ImGui::Text("Runtime: %.1f Hz   Per eye: %u x %u",d.runtimeHz,d.width,d.height);
                ImGui::Text("Internal race resolution: %u x %u (%.0f%%)",uint32_t(d.width*d.rasterScale),uint32_t(d.height*d.rasterScale),d.rasterScale*100);
                const auto anchor=mkw::vr::MkwVRFirstPersonGetAnchor();
                ImGui::Text("Vehicle grip: %s / mesh prepared: %s",anchor.native_wheel.valid?"yes":"no",anchor.native_mesh_prepared?"yes":"no");
                ImGui::Text("Native wheel draw matches (last game frame): %u",aurora_native_wheel_draw_count());
                ImGui::TextWrapped("Zero matches can mean the wheel is outside the view or that this model needs additional support.");
                ImGui::Text("Presentation interval: p95 %.2f ms / p99 %.2f ms",d.intervalP95,d.intervalP99);
                ImGui::TextWrapped("Updated every five seconds in asynchronous mode. Intervals measure presentation regularity, not GPU execution time. Repeated images keep head tracking smooth but do not add new game motion.");
                static std::string exportStatus;
                if(ImGui::Button("Export VR diagnostics")) {
                    std::ostringstream report;
                    report<<"WiiCompiled VR diagnostics\nBuild: "<<__DATE__<<" "<<__TIME__
                        <<"\nDisplay FPS: "<<d.displayFps<<"\nNew-image FPS: "<<d.newImageFps
                        <<"\nRuntime Hz: "<<d.runtimeHz<<"\nEye size: "<<d.width<<" x "<<d.height
                        <<"\nPresentation p95/p99 ms: "<<d.intervalP95<<" / "<<d.intervalP99
                        <<"\nRender scale: "<<RuntimeConfigFile::VrRenderScale()
                        <<"\nNative wheel: "<<RuntimeConfigFile::VrNativeSteeringWheel()
                        <<"\nCamera: "<<int(mkw::vr::MkwVRGetCameraMode())<<"\n";
                    const auto path=RuntimeConfigFile::ResolveConfigPath().parent_path()/"VR-diagnostics.txt";
                    exportStatus=mkw::platform::AtomicWriteText(path,report.str())?"Saved VR-diagnostics.txt next to Config.toml (no ROM or personal paths included).":"Unable to save diagnostics.";
                }
                ImGui::TextWrapped("%s",exportStatus.c_str());
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
    g_vrSettingsFocus = false;
    if (!visible) SetVrSettingsVisible(false);
}

void DrawGraphicsSettings() {
    g_displayMode = static_cast<int>(aurora_get_display_mode());
    struct EffectFlag {
        const char* label;
        uint32_t flag;
    };
    static constexpr std::array<EffectFlag, 1> kEffectFlags = {{
        {"Disable bloom", 0x10u},
    }};

    for (const auto& effect : kEffectFlags) {
        bool disabled = (g_disabledPostProcessingPaths & effect.flag) != 0;
        if (ImGui::Checkbox(effect.label, &disabled)) {
            if (disabled) {
                g_disabledPostProcessingPaths |= effect.flag;
            } else {
                g_disabledPostProcessingPaths &= ~effect.flag;
            }
            RuntimeGameGraphicsOptions::SetDisabledPostProcessingPaths(g_disabledPostProcessingPaths);
            RuntimeConfigFile::SetDisabledPostProcessingPaths(g_disabledPostProcessingPaths);
        }
    }
    ImGui::TextDisabled("Applied when the next scene renderer is created.");
    ImGui::Separator();
    static constexpr const char* kDisplayModes[] = {
        "Windowed",
        "Borderless fullscreen",
        "Exclusive fullscreen",
    };
    if (ImGui::Combo("Display mode", &g_displayMode, kDisplayModes, static_cast<int>(std::size(kDisplayModes)))) {
        const auto mode = static_cast<AuroraDisplayMode>(g_displayMode);
        aurora_set_display_mode(mode);
        const AuroraDisplayMode activeMode = aurora_get_display_mode();
        if (activeMode == mode) {
            RuntimeConfigFile::SetDisplayMode(std::string(kDisplayModeConfigNames[static_cast<size_t>(g_displayMode)]));
        } else {
            g_displayMode = static_cast<int>(activeMode);
        }
    }
    if (g_displayMode == AURORA_DISPLAY_MODE_EXCLUSIVE) {
        ImGui::TextDisabled(
            "Requests the closest native-resolution display mode to the output frame "
            "rate (60 Hz, or the frame interpolation target).");
    }
    constexpr std::array<const char*, 3> kFrameInterpolationModes{
        "Off", "120 FPS", "180 FPS",
    };
    const char* currentFrameInterpolationMode =
        kFrameInterpolationModes[static_cast<size_t>(g_frameInterpolationMode)];
    bool frameInterpolationModeChanged = false;
    if (ImGui::BeginCombo("Race frame interpolation (experimental)", currentFrameInterpolationMode)) {
        for (int mode = 0; mode < static_cast<int>(kFrameInterpolationModes.size()); ++mode) {
            const bool selected = g_frameInterpolationMode == mode;
            if (ImGui::Selectable(kFrameInterpolationModes[static_cast<size_t>(mode)], selected)) {
                g_frameInterpolationMode = mode;
                frameInterpolationModeChanged = true;
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    if (frameInterpolationModeChanged) {
        const uint32_t targetFps = kFrameInterpolationTargetFps[static_cast<size_t>(g_frameInterpolationMode)];
        aurora_set_frame_interpolation_fps(targetFps);
        RuntimeConfigFile::SetFrameInterpolationFps(targetFps);
        LimitResolutionForFrameRate();
        if (aurora_get_display_mode() == AURORA_DISPLAY_MODE_EXCLUSIVE) {
            // Re-apply exclusive mode so the display refresh tracks the new target.
            aurora_set_display_mode(AURORA_DISPLAY_MODE_EXCLUSIVE);
        }
    }
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 380.0f);
    ImGui::TextDisabled("Frame interpolation is experimental, you might find visual artifacts");
    ImGui::PopTextWrapPos();
    if (ImGui::Checkbox("Disable copy filter", &g_disableCopyFilter)) {
        aurora_set_disable_copy_filter(g_disableCopyFilter);
        RuntimeConfigFile::SetDisableCopyFilter(g_disableCopyFilter);
    }
    if (ImGui::Checkbox("Skip draws while shaders compile", &g_skipUnreadyPipelines)) {
        aurora_set_skip_unready_pipelines(g_skipUnreadyPipelines);
        RuntimeConfigFile::SetSkipUnreadyPipelines(g_skipUnreadyPipelines);
    }
    if (ImGui::Checkbox("Show FPS", &g_showFps)) {
        RuntimeConfigFile::SetShowFps(g_showFps);
    }
    ImGui::Separator();
    ImGui::Text("Graphics API: %s", GraphicsApiDisplayName());
    if (ImGui::Checkbox("Enable OpenXR VR", &g_vrEnabled)) {
        RuntimeConfigFile::SetVrEnabled(g_vrEnabled);
    }
    ImGui::TextDisabled("OpenXR mode changes take effect after restarting the game.");

    if (ImGui::Button("VR Settings")) SetVrSettingsVisible(true);
}

void DrawFpsOverlay() {
    AuroraPresentTiming presentTiming{};
    aurora_get_present_timing(&presentTiming);
    if (!g_showFps) {
        return;
    }

    const ImGuiIO& io = ImGui::GetIO();
    constexpr float kMargin = 10.0f;
    const float top = g_topBarVisible ? ImGui::GetFrameHeight() + kMargin : kMargin;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - kMargin, top), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.55f);
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_AlwaysAutoResize |
                                         ImGuiWindowFlags_NoDecoration |
                                         ImGuiWindowFlags_NoFocusOnAppearing |
                                         ImGuiWindowFlags_NoInputs |
                                         ImGuiWindowFlags_NoMove |
                                         ImGuiWindowFlags_NoNav |
                                         ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("FPS Overlay", nullptr, kFlags)) {
        if (presentTiming.sampleCount == 0) {
            ImGui::TextUnformatted("FPS: --");
        } else {
            // Present timing includes the additional frames produced by
            // interpolation, so this remains the actual displayed FPS.
            ImGui::Text("FPS: %.1f", presentTiming.framesPerSecond);
            // Replay-unsafe frames hold the presented cadence with duplicated
            // slots, so the counter alone reads 180 while the motion on screen
            // is 60 Hz. Surface the divergence instead of hiding it.
            if (presentTiming.effectiveFramesPerSecond <
                presentTiming.framesPerSecond * 0.95) {
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f), "Motion: %.1f",
                                   presentTiming.effectiveFramesPerSecond);
            }
        }
    }
    ImGui::End();
}

void DrawShaderCompilationStatus() {
    const uint32_t queuedPipelines = aurora_get_queued_pipeline_count();
    if (queuedPipelines == 0) {
        return;
    }

    constexpr float kMargin = 10.0f;
    const float top = g_topBarVisible ? ImGui::GetFrameHeight() + kMargin : kMargin;
    ImGui::SetNextWindowPos(ImVec2(kMargin, top), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(7.0f, 4.0f));
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_AlwaysAutoResize |
                                        ImGuiWindowFlags_NoDecoration |
                                        ImGuiWindowFlags_NoFocusOnAppearing |
                                        ImGuiWindowFlags_NoInputs |
                                        ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoNav |
                                        ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("Shader Compilation Status", nullptr, kFlags)) {
        ImGui::SetWindowFontScale(0.85f);
        ImGui::Text("%u shader%s compiling", queuedPipelines, queuedPipelines == 1 ? "" : "s");
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void DrawStartupScreen() {
    if (!StartupScreenVisible()) {
        return;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(viewport->Size, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(1.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 255));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration |
                                        ImGuiWindowFlags_NoFocusOnAppearing |
                                        ImGuiWindowFlags_NoInputs |
                                        ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoNav |
                                        ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (ImGui::Begin("Wiicompiled Startup", nullptr, kFlags)) {
        ImGui::SetWindowFontScale(1.25f);
        constexpr const char* kTitle = "WiiCompiled";
        const ImVec2 titleSize = ImGui::CalcTextSize(kTitle);
        const float titleX = std::max(0.0f, (viewport->Size.x - titleSize.x) * 0.5f);
        const float startY = std::max(0.0f, (viewport->Size.y - titleSize.y) * 0.5f);
        ImGui::SetCursorPos(ImVec2(titleX, startY));
        ImGui::TextUnformatted(kTitle);
    }
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void DrawTopBar() {
    if (!g_topBarVisible || !ImGui::BeginMainMenuBar()) {
        return;
    }

    ImGui::TextUnformatted("WiiCompiled");
    ImGui::Separator();
    const auto resolutionIt = std::find_if(kResolutions.begin(), kResolutions.end(), [](const ResolutionItem& item) {
        return std::fabs(item.scale - g_resolutionScale) < 0.001f;
    });
    const char* resolutionLabel = resolutionIt != kResolutions.end() ? resolutionIt->label : "Custom";
    const std::string resolutionMenuLabel = std::string("Resolution: ") + resolutionLabel;
    if (ImGui::BeginMenu(resolutionMenuLabel.c_str())) {
        for (const auto& resolution : kResolutions) {
            const bool selected = std::fabs(resolution.scale - g_resolutionScale) < 0.001f;
            const bool disabled = IsHighFrameRateMode() && IsHighResolutionScale(resolution.scale);
            ImGui::BeginDisabled(disabled);
            const bool clicked = ImGui::MenuItem(resolution.label, nullptr, selected);
            ImGui::EndDisabled();
            if (clicked) {
                SetResolutionScale(resolution.scale);
            }
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Graphics")) {
        DrawGraphicsSettings();
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Controller settings")) {
        DrawControllerSettings();
        ImGui::EndMenu();
    }

    const std::string audioLabel = g_audioMuted
        ? "Audio: Muted"
        : "Audio: " + std::to_string(g_audioVolumePercent) + "%";
    // Keep the popup ID stable while the Master slider changes the visible
    // label. Without the ### suffix, ImGui treats every new percentage as a
    // different menu and closes the popup on the first drag update.
    const std::string audioMenuLabel = audioLabel + "###AudioSettingsMenu";
    if (ImGui::BeginMenu(audioMenuLabel.c_str())) {
        DrawAudioSettings();
        ImGui::EndMenu();
    }

    const float hideWidth = ImGui::CalcTextSize("Hide (F10)").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - hideWidth - 8.0f));
    if (ImGui::MenuItem("Hide (F10)")) {
        SetTopBarVisible(false);
    }
    ImGui::EndMainMenuBar();
}

bool IsToggleKey(const SDL_Event& event, SDL_Scancode code) {
    return event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && event.key.scancode == code;
}

bool IsMouseActivity(const SDL_Event& event) {
    switch (event.type) {
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
        return true;
    default:
        return false;
    }
}

// Runs on the thread that pumps SDL events (the same one that calls Draw), so
// the SDL cursor calls are safe here.
void UpdateCursorAutoHide() {
    const bool shouldHide =
        !g_topBarVisible && Clock::now() - g_lastMouseActivity >= kCursorAutoHideDelay;
    if (shouldHide == g_cursorHidden) {
        return;
    }
    g_cursorHidden = shouldHide;
    if (shouldHide) {
        SDL_HideCursor();
    } else {
        SDL_ShowCursor();
    }
}

// Alt+Enter toggles the display mode inside aurora without going through the
// F10 combo, so the active mode is compared against the last persisted one
// every frame and written back on change.
void PersistDisplayModeIfChanged() {
    const int active = static_cast<int>(aurora_get_display_mode());
    if (active == g_displayMode) {
        return;
    }
    g_displayMode = active;
    RuntimeConfigFile::SetDisplayMode(std::string(kDisplayModeConfigNames[static_cast<size_t>(active)]));
}
} // namespace

void InitializeRuntimeSettings() noexcept {
    controller_mapping_wizard::LoadPersistedMappings();
    ApplyConfiguredMappings();
    AudioBackend::Instance().SetMasterVolume(static_cast<float>(g_audioVolumePercent) / 100.0f);
    AudioBackend::Instance().SetMuted(g_audioMuted);
    MusicAttenuation::SetMusicVolume(static_cast<float>(g_musicVolumePercent) / 100.0f);
    MusicAttenuation::SetSoundEffectsVolume(static_cast<float>(g_soundEffectsVolumePercent) / 100.0f);
    MusicAttenuation::SetUiVolume(static_cast<float>(g_uiVolumePercent) / 100.0f);
    MusicAttenuation::SetVoicesVolume(static_cast<float>(g_voicesVolumePercent) / 100.0f);
    MusicAttenuation::SetEnabled(g_attenuateMusicWhenMediaPlays);
    RuntimeGameGraphicsOptions::SetDisabledPostProcessingPaths(g_disabledPostProcessingPaths);
    const uint32_t targetFps = kFrameInterpolationTargetFps[static_cast<size_t>(g_frameInterpolationMode)];
    LimitResolutionForFrameRate();
    aurora_set_frame_interpolation_fps(targetFps);
    aurora_set_display_mode(static_cast<AuroraDisplayMode>(g_displayMode));
    g_displayMode = static_cast<int>(aurora_get_display_mode());
    aurora_set_disable_copy_filter(g_disableCopyFilter);
    aurora_set_stereo_stop_at_display_copy(g_vrStopAtDisplayCopy);
    aurora_set_vr_menu_shader_quality(RuntimeConfigFile::Get().vrMenuShaderQuality);
    aurora_set_stereo_skip_copy_clears(g_vrSkipCopyClears);
    ApplyVrHudVirtualScreen();
    aurora_set_skip_unready_pipelines(g_skipUnreadyPipelines);
    mkw::vr::MkwVRFirstPersonApplyConfiguredSettings();
    g_strapInputAccepted.store(false, std::memory_order_relaxed);
    mkw::vr::SetQuestStickCalibration(g_vrStickCalibration);
    g_startupDismissFrame.store(UINT64_MAX, std::memory_order_relaxed);
    PADBlockInput(false);
}

void RefreshVrHudVirtualScreen() noexcept { ApplyVrHudVirtualScreen(); }

void HandleEvents(const AuroraEvent* events) noexcept {
    if (!events) {
        return;
    }
    for (const AuroraEvent* ev = events; ev->type != AURORA_NONE; ++ev) {
        if (ev->type == AURORA_CONTROLLER_ADDED || ev->type == AURORA_CONTROLLER_REMOVED) {
            g_configuredControllerIndices.fill(std::numeric_limits<int32_t>::min());
        }
        if (ev->type != AURORA_SDL_EVENT) {
            continue;
        }
        controller_mapping_wizard::HandleSdlEvent(ev->sdl);
        if (IsToggleKey(ev->sdl, SDL_SCANCODE_F10)) {
            if (g_tutorial.stage!=mkw::vr::TutorialFlow::Stage::Showing) {
                if (g_vrEnabled) SetVrSettingsVisible(!g_vrSettingsVisible); else SetTopBarVisible(!g_topBarVisible);
            }
        }
        if (IsMouseActivity(ev->sdl)) {
            g_lastMouseActivity = Clock::now();
        }
    }
}

void Draw() noexcept {
    // Wait for the frame worker's DONE phase: it has replayed the previous frame's ImGui draw lists
    // and started the next ImGui frame, so all overlay callers can now safely issue ImGui commands.
    aurora_wait_for_frame_worker();
    // Also drive the Wii Remote rescan from here: PADRead runs it too, but this
    // runs once per presented frame whatever the game is doing (e.g. sitting in
    // its "communications interrupted" prompt without polling pads). Same guest
    // thread as PADRead, so no concurrent access to the scanner's state.
    WiiRemoteInput::Poll();
    if(g_vrEnabled) {
        const auto input=mkw::vr::ReadQuestInputSnapshot();
        const auto policy=mkw::vr::MkwVRPolicyGetSnapshot();
        if(input.active && (g_vrSettingsVisible || policy.scene.mode!=mkw::vr::VRSceneMode::Race ||
            g_tutorial.stage==mkw::vr::TutorialFlow::Stage::Showing)) EnsureTutorialControllerModels(input.steamvr);
    }
    ApplyConfiguredMappings();
    PersistDisplayModeIfChanged();
    UpdateCursorAutoHide();
    if (!StartupScreenVisible()) {
        DrawShaderCompilationStatus();
    }
    DrawFpsOverlay();
    DrawTopBar();
    DrawVrSettings();
    controller_mapping_wizard::Draw();
    // The wizard captures raw presses; keep them out of the game.
    PADBlockInput(g_vrSettingsVisible || controller_mapping_wizard::IsActive());
    DrawStartupScreen();
    DrawRaceTutorial();
}

bool VrWelcomePending() noexcept { return g_vrEnabled && !RuntimeConfigFile::Get().vrWelcomeComplete; }
void DrawVrWelcome() noexcept {
    aurora_wait_for_frame_worker();
    DrawWelcomePanel();
}
void DrawVrIntroPreview(int kind) noexcept {
    aurora_wait_for_frame_worker();
    if(kind==0) { DrawWelcomePanel();return; }
    mkw::vr::QuestInput example{};example.active=true;example.steamvr=true;
    for(int hand=0;hand<2;++hand) {
        example.ui_hands[hand].valid=true;
        example.ui_hands[hand].position={hand?.2f:-.2f,-.2f,-.5f};
    }
    if(BeginIntroduction(kind==2?"First-person driving controls":"Third-person and diorama controls")) {
        ImGui::TextWrapped("Mario Kart is paused. Point at Continue with your right controller and pull its trigger when you are ready.");
        DrawControllerGuide(example,kind==2);
        ImGui::Button("Continue racing",ImVec2(-1,48));
    }
    EndIntroduction();
}

bool StartupScreenVisible() noexcept {
    return !g_strapInputAccepted.load(std::memory_order_acquire) ||
           g_presentedFrame < g_startupDismissFrame.load(std::memory_order_relaxed);
}

void NotifyStrapInputAccepted() noexcept {
    bool expected = false;
    if (g_strapInputAccepted.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        g_startupDismissFrame.store(g_presentedFrame + kStrapTransitionCoverFrames,
                                    std::memory_order_release);
    }
}

void AdvancePresentedFrame() noexcept { ++g_presentedFrame; }
} // namespace settings_overlay
