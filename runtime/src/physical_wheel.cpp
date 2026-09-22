#include "physical_wheel.h"
#include "runtime_config.h"
#include "vr/quest_input.h"
#include <SDL3/SDL.h>
#include <imgui.h>
#include <array>
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

namespace physical_wheel {
namespace {
using Clock=std::chrono::steady_clock;
struct Device { SDL_JoystickID id; SDL_Joystick* joystick; std::string key,name; };
struct Axis { std::string device; int index=-1,low=0,center=0,high=0; };
struct Button { std::string device; int index=-1; };
std::vector<Device> devices;
std::array<Axis,3> axes; // wheel, throttle, brake
std::array<Button,5> buttons; // right paddle, left paddle, trick, confirm, pause
const char* axisNames[]{"Steering wheel","Accelerator pedal","Brake / reverse pedal"};
const char* buttonNames[]{"Drift: RIGHT paddle","Item: LEFT paddle","Trick / wheelie","Confirm / accelerate","Pause"};
bool loaded=false,enabled=false,vibration=false,ready=false,focused=false,driving=false;
bool armed=false;
float deadzone=.02f,strength=.05f;
std::string error;
int learning=-1;
std::vector<std::pair<SDL_JoystickID,int>> previousButtons;
Clock::time_point rumbleTick{},motorTime{};
Clock::time_point settingsUntil{};
bool discoveryRequested=true,discoveryActive=false;
bool motorOn=false;
SDL_Haptic* haptic=nullptr;SDL_JoystickID hapticId=0,attemptedHaptic=0;
bool hapticSubsystem=false;
std::mutex snapshotMutex;
float snapshotSteering=0;bool snapshotActive=false;Clock::time_point snapshotTime{};
mkw::vr::QuestPadFilter inputFilter;

auto ConfigPath() {return RuntimeConfigFile::ResolveConfigPath().parent_path()/"PhysicalWheel.toml";}
Device* Find(const std::string& key) {
    Device* match=nullptr;
    for(auto& d:devices) if(d.key==key && SDL_JoystickConnected(d.joystick)) {
        if(match) return nullptr; // Ambiguous identical devices must not control the wrong pedal.
        match=&d;
    }
    return match;
}
bool AxisReady(const Axis& a) {
    const auto* d=Find(a.device);
    return d && a.index>=0 && a.index<SDL_GetNumJoystickAxes(d->joystick) && std::abs(a.high-a.low)>=1024;
}
int Raw(const Axis& a) {const auto* d=Find(a.device);return d && a.index>=0 && a.index<SDL_GetNumJoystickAxes(d->joystick)?SDL_GetJoystickAxis(d->joystick,a.index):0;}
bool ButtonReady(const Button& b) {const auto* d=Find(b.device);return d && b.index>=0 && b.index<SDL_GetNumJoystickButtons(d->joystick);}
bool Pressed(const Button& b) {const auto* d=Find(b.device);return ButtonReady(b) && SDL_GetJoystickButton(d->joystick,b.index);}
void StopFeedback() {
    if (!motorOn && !haptic) return;
    if(haptic) SDL_StopHapticRumble(haptic);
    if(auto* d=Find(axes[0].device)) SDL_RumbleJoystick(d->joystick,0,0,0);
    motorOn=false;
}
void CloseHaptic() {
    StopFeedback();
    if(haptic) SDL_CloseHaptic(haptic);
    haptic=nullptr;hapticId=attemptedHaptic=0;
}
void Save() {
    toml::value root(toml::table{});
    root["enabled"]=enabled;root["vibration"]=vibration;
    root["deadzone"]=double(deadzone);root["strength"]=double(strength);
    toml::array aa,bb;
    for(const auto& a:axes) aa.emplace_back(toml::table{{"device",a.device},{"axis",a.index},{"low",a.low},{"center",a.center},{"high",a.high}});
    for(const auto& b:buttons) bb.emplace_back(toml::table{{"device",b.device},{"button",b.index}});
    root["axes"]=aa;root["buttons"]=bb;
    std::error_code ec;std::filesystem::create_directories(ConfigPath().parent_path(),ec);
    if(!mkw::platform::AtomicWriteText(ConfigPath(),toml::format(root))) error="Could not save PhysicalWheel.toml.";
    else error.clear();
}
void Load() {
    loaded=true;
    if(!std::filesystem::exists(ConfigPath())) return;
    try {
        const auto c=toml::parse(RuntimeConfigFile::PathToUtf8(ConfigPath()));
        auto newAxes=axes;auto newButtons=buttons;
        const auto& aa=toml::find(c,"axes").as_array();const auto& bb=toml::find(c,"buttons").as_array();
        if(aa.size()!=3 || bb.size()!=5) throw std::runtime_error("Invalid wheel configuration");
        for(size_t i=0;i<3;++i) {
            auto& a=newAxes[i];a.device=toml::find<std::string>(aa[i],"device");a.index=toml::find<int>(aa[i],"axis");
            a.low=std::clamp(toml::find<int>(aa[i],"low"),-32768,32767);
            a.center=std::clamp(toml::find<int>(aa[i],"center"),-32768,32767);
            a.high=std::clamp(toml::find<int>(aa[i],"high"),-32768,32767);
        }
        for(size_t i=0;i<5;++i) {newButtons[i].device=toml::find<std::string>(bb[i],"device");newButtons[i].index=toml::find<int>(bb[i],"button");}
        axes=newAxes;buttons=newButtons;
        enabled=toml::find_or<bool>(c,"enabled",false);vibration=toml::find_or<bool>(c,"vibration",false);
        const double dz=toml::find_or<double>(c,"deadzone",.02),gain=toml::find_or<double>(c,"strength",.05);
        deadzone=std::isfinite(dz)?std::clamp(float(dz),0.f,.25f):.02f;
        strength=std::isfinite(gain)?std::clamp(float(gain),0.f,.15f):.05f;
    } catch(const std::exception& e) {enabled=false;error=std::string("Wheel configuration: ")+e.what();}
}
void Scan() {
    // Keep open handles stable; closing/reopening a wheel can disturb its driver.
    int count=0;auto* ids=SDL_GetJoysticks(&count);
    if(!ids) return;
    for(auto it=devices.begin();it!=devices.end();) {
        if(!SDL_JoystickConnected(it->joystick)) {
            if(hapticId==it->id || attemptedHaptic==it->id) CloseHaptic();
            SDL_CloseJoystick(it->joystick);it=devices.erase(it);
        } else ++it;
    }
    for(int i=0;i<count;++i) {
        if(std::any_of(devices.begin(),devices.end(),[&](const auto& d){return d.id==ids[i];})) continue;
        auto* j=SDL_OpenJoystick(ids[i]);if(!j) continue;
        char guid[33]{};SDL_GUIDToString(SDL_GetJoystickGUID(j),guid,sizeof(guid));
        const char* serial=SDL_GetJoystickSerial(j);const char* path=SDL_GetJoystickPath(j);
        std::string key=guid;key+='|';key+=serial&&*serial?serial:path?path:"";
        devices.push_back({ids[i],j,key,SDL_GetJoystickName(j)?SDL_GetJoystickName(j):"Unnamed device"});
    }
    SDL_free(ids);
}
void Feedback() {
    if(!enabled || !ready || !focused || !driving || PADIsInputBlocked() || Clock::now()<settingsUntil || !vibration ||
        Clock::now()-motorTime>std::chrono::milliseconds(250)) {StopFeedback();return;}
    if(!motorOn) return;
    if(Clock::now()-rumbleTick<std::chrono::milliseconds(40)) return;
    rumbleTick=Clock::now();auto* d=Find(axes[0].device);if(!d) return;
    // Short bounded effects only. No spring, damper or constant steering torque.
    const auto amplitude=static_cast<Uint16>(std::clamp(strength,0.f,.15f)*65535);
    if(SDL_RumbleJoystick(d->joystick,amplitude,amplitude,100)) return;
    if(!haptic && attemptedHaptic!=d->id) {
        attemptedHaptic=d->id;
        if(!hapticSubsystem) hapticSubsystem=SDL_InitSubSystem(SDL_INIT_HAPTIC);
        if(hapticSubsystem && SDL_IsJoystickHaptic(d->joystick)) {
            haptic=SDL_OpenHapticFromJoystick(d->joystick);hapticId=d->id;
            if(haptic && (!SDL_SetHapticGain(haptic,15) || !SDL_InitHapticRumble(haptic))) {SDL_CloseHaptic(haptic);haptic=nullptr;}
        }
        if(!haptic) error="This driver does not expose rumble. Driving still works.";
    }
    if(haptic) SDL_PlayHapticRumble(haptic,std::clamp(strength,0.f,.15f),100);
}
}
void Poll() {
    if(!loaded) Load();
    // SDL_GetJoysticks/OpenJoystick can probe HID/Bluetooth drivers and stall a
    // frame. Do not enumerate during ordinary races, especially with the wheel
    // disabled. SDL hotplug events request the next scan when a wheel is active.
    const bool discover=enabled || Clock::now()<settingsUntil;
    if (!discover) {
        if (discoveryActive) {
            CloseHaptic();
            for (auto& d:devices) SDL_CloseJoystick(d.joystick);
            devices.clear();
        }
        discoveryActive=false;discoveryRequested=true;
        ready=false;armed=false;inputFilter={};
        std::lock_guard lock(snapshotMutex);
        snapshotActive=false;snapshotSteering=0;
        return;
    }
    if (!discoveryActive) discoveryRequested=true;
    discoveryActive=true;
    if (discoveryRequested) {Scan();discoveryRequested=false;}
    focused=mkw::vr::ReadQuestInputSnapshot().active || SDL_GetKeyboardFocus()!=nullptr;
    ready=AxisReady(axes[0]) && AxisReady(axes[1]) && AxisReady(axes[2]) &&
        std::abs(axes[0].low-axes[0].center)>=1024 && std::abs(axes[0].high-axes[0].center)>=1024 &&
        (axes[0].low<axes[0].center)!=(axes[0].high<axes[0].center) && ButtonReady(buttons[0]) && ButtonReady(buttons[1]) &&
        (buttons[0].device!=buttons[1].device || buttons[0].index!=buttons[1].index);
    {
        std::lock_guard lock(snapshotMutex);
        snapshotActive=enabled&&ready&&focused&&driving&&!PADIsInputBlocked()&&Clock::now()>=settingsUntil;snapshotTime=Clock::now();
        snapshotSteering=snapshotActive?Steering(Raw(axes[0]),axes[0].low,axes[0].center,axes[0].high,deadzone):0;
    }
    Feedback();
}
void HandleSdlEvent(const SDL_Event& event) {
    if (event.type==SDL_EVENT_JOYSTICK_ADDED || event.type==SDL_EVENT_JOYSTICK_REMOVED)
        discoveryRequested=true;
}
bool ReadPad(PADStatus& pad,bool blocked,bool race) {
    blocked=blocked || Clock::now()<settingsUntil;
    driving=race;Poll();
    if(!enabled) {armed=false;inputFilter={};return false;}
    if(!ready || !focused) {armed=false;StopFeedback();inputFilter={};if(race) {const auto pause=pad.button&PAD_BUTTON_START;pad={};pad.err=PAD_ERR_NONE;pad.button=pause;}return true;}
    auto wheel=Map(Steering(Raw(axes[0]),axes[0].low,axes[0].center,axes[0].high,deadzone),
        Pedal(Raw(axes[1]),axes[1].low,axes[1].high),Pedal(Raw(axes[2]),axes[2].low,axes[2].high),Pressed(buttons[0]),Pressed(buttons[1]));
    if(Pressed(buttons[2])) wheel.button|=PAD_BUTTON_UP;
    if(Pressed(buttons[3]) && !(wheel.button&PAD_BUTTON_B)) {wheel.button|=PAD_BUTTON_A;wheel.analogA=255;}
    if(Pressed(buttons[4])) wheel.button|=PAD_BUTTON_START;
    if(!armed) {
        armed=wheel.button==0 && !blocked;
        if(!armed) {wheel={};wheel.err=PAD_ERR_NONE;StopFeedback();}
    }
    if(race) {
        // Keep XR pause/tutorial pulses and item aiming. Physical hardware owns driving.
        const auto pause=pad.button&PAD_BUTTON_START;const auto aim=pad.stickY;
        pad=inputFilter.Apply(wheel,blocked);pad.button|=pause;pad.stickY=blocked?0:aim;
    } else {
        // In menus keep pointer/gamepad navigation; pedals must not auto-confirm.
        wheel.button&=PAD_BUTTON_START|PAD_BUTTON_A;
        if(!Pressed(buttons[3])) wheel.button&=~PAD_BUTTON_A;
        wheel.stickX=wheel.stickY=0;wheel=inputFilter.Apply(wheel,blocked);
        pad.button|=wheel.button;pad.err=PAD_ERR_NONE;
    }
    return true;
}
bool SteeringSnapshot(float& steering) {
    std::lock_guard lock(snapshotMutex);steering=snapshotSteering;
    return snapshotActive && Clock::now()-snapshotTime<std::chrono::milliseconds(250);
}
bool Motor(int channel,unsigned command) {
    if(channel!=0 || !enabled) return false;
    motorTime=Clock::now();
    if(command!=PAD_MOTOR_RUMBLE) StopFeedback();else {motorOn=true;Feedback();}return true;
}
void Shutdown() {
    CloseHaptic();for(auto& d:devices) SDL_CloseJoystick(d.joystick);devices.clear();
    discoveryActive=false;discoveryRequested=true;
    if(hapticSubsystem) SDL_QuitSubSystem(SDL_INIT_HAPTIC);
    hapticSubsystem=false;ready=false;
    std::lock_guard lock(snapshotMutex);snapshotActive=false;
}
void DrawSettings() {
    settingsUntil=Clock::now()+std::chrono::milliseconds(200);
    Poll();
    if(ImGui::Button("Refresh USB devices")) discoveryRequested=true;
    ImGui::TextWrapped("USB wheel and pedals (player 1). Calibrate each axis, then bind RIGHT paddle to drift and LEFT paddle to item. Separate USB pedals and combined pedal axes are supported.");
    if(ImGui::Checkbox("Enable physical wheel",&enabled)) {armed=false;StopFeedback();Save();}
    ImGui::TextWrapped("After enabling or reconnecting, close settings and release all pedals and buttons to arm driving.");
    ImGui::TextWrapped(ready?"Devices and required bindings ready.":"Setup incomplete or device disconnected. Calibrate all axes and assign both paddles. Enabled wheel input stays neutral until ready.");
    for(size_t i=0;i<axes.size();++i) {
        ImGui::PushID(int(i));auto& a=axes[i];ImGui::Separator();ImGui::TextUnformatted(axisNames[i]);
        auto* selected=Find(a.device);
        if(ImGui::BeginCombo("Device",selected?selected->name.c_str():"Select device")) {
            for(const auto& d:devices) {ImGui::PushID(int(d.id));if(ImGui::Selectable(d.name.c_str(),a.device==d.key)) {if(i==0) CloseHaptic();a={d.key,-1,0,0,0};Save();}ImGui::PopID();}
            ImGui::EndCombo();
        }
        selected=Find(a.device);
        if(selected) {
            std::string label=a.index<0?"Select axis":std::to_string(a.index);
            if(ImGui::BeginCombo("Axis",label.c_str())) {
                for(int n=0;n<SDL_GetNumJoystickAxes(selected->joystick);++n) {
                    const std::string text=std::to_string(n)+" : "+std::to_string(SDL_GetJoystickAxis(selected->joystick,n));
                    if(ImGui::Selectable(text.c_str(),n==a.index)) {a.index=n;a.low=a.center=a.high=0;Save();}
                }ImGui::EndCombo();
            }
            ImGui::Text("Raw: %d",Raw(a));
            if(ImGui::Button(i==0?"Set full LEFT":"Set RELEASED")) {a.low=Raw(a);Save();}
            ImGui::SameLine();if(ImGui::Button(i==0?"Set full RIGHT":"Set fully PRESSED")) {a.high=Raw(a);Save();}
            if(i==0) {ImGui::SameLine();if(ImGui::Button("Set CENTER")) {a.center=Raw(a);Save();}}
            const float value=i==0?Steering(Raw(a),a.low,a.center,a.high,deadzone):Pedal(Raw(a),a.low,a.high);
            ImGui::Text("Calibrated: %.2f",value);
        }ImGui::PopID();
    }
    std::vector<std::pair<SDL_JoystickID,int>> down;
    for(const auto& d:devices) for(int n=0;n<SDL_GetNumJoystickButtons(d.joystick);++n)
        if(SDL_GetJoystickButton(d.joystick,n)) down.emplace_back(d.id,n);
    if(learning>=0) for(const auto& press:down) if(std::find(previousButtons.begin(),previousButtons.end(),press)==previousButtons.end()) {
        for(const auto& d:devices) if(d.id==press.first) {buttons[learning]={d.key,press.second};learning=-1;Save();break;}break;
    }
    previousButtons=down;
    for(size_t i=0;i<buttons.size();++i) {
        ImGui::PushID(100+int(i));ImGui::TextUnformatted(buttonNames[i]);ImGui::SameLine();
        if(ImGui::Button(learning==int(i)?"Press the hardware button...":"Assign")) learning=int(i);
        ImGui::SameLine();if(ImGui::Button("Clear")) {buttons[i]={};learning=-1;Save();}
        const auto* d=Find(buttons[i].device);if(d) ImGui::Text("%s / button %d",d->name.c_str(),buttons[i].index);
        ImGui::PopID();
    }
    if(learning>=0 && ImGui::Button("Cancel assignment")) learning=-1;
    if(ImGui::SliderFloat("Wheel deadzone",&deadzone,0,.25f)) Save();
    if(ImGui::Checkbox("Light game vibration (off by default)",&vibration)) {CloseHaptic();Save();}
    if(ImGui::SliderFloat("Vibration strength (maximum 15%)",&strength,0,.15f)) Save();
    ImGui::TextWrapped("No constant force or centering effect. Vibration follows Mario Kart's original rumble events; a curb vibrates only if the game emits rumble there. Hardware/driver support varies.");
    if(!error.empty()) ImGui::TextWrapped("%s",error.c_str());
}
}
