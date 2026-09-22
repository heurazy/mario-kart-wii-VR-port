#pragma once
#include <algorithm>
#include <cmath>
#include <dolphin/pad.h>
union SDL_Event;

namespace physical_wheel {
// Endpoint calibration also handles reversed and combined pedal axes.
inline float Pedal(int raw, int released, int pressed) {
    if (std::abs(pressed - released) < 1024) return 0;
    return std::clamp(float(raw - released) / float(pressed - released), 0.f, 1.f);
}
inline float Steering(int raw, int left, int center, int right, float deadzone) {
    if (std::abs(left-center)<1024 || std::abs(right-center)<1024 ||
        (left<center)==(right<center)) return 0;
    const float direction=float(raw-center)/float(right-center);
    const float x=direction>=0 ? Pedal(raw,center,right) : -Pedal(raw,center,left);
    deadzone=std::clamp(deadzone,0.f,.25f);
    return std::copysign(std::max(0.f,(std::abs(x)-deadzone)/(1-deadzone)),x);
}
inline PADStatus Map(float steering,float throttle,float brake,bool drift,bool item) {
    PADStatus p{};p.err=PAD_ERR_NONE;
    p.stickX=static_cast<int8_t>(std::lround(std::clamp(steering,-1.f,1.f)*100));
    if(throttle>.1f) {p.button|=PAD_BUTTON_A;p.analogA=255;}
    if(drift) {p.button|=PAD_TRIGGER_R;p.triggerR=255;}
    if(item) {p.button|=PAD_TRIGGER_L;p.triggerL=255;}
    if(brake>.1f) {
        p.button&=~(PAD_BUTTON_A|PAD_TRIGGER_R);p.analogA=p.triggerR=0;
        p.button|=PAD_BUTTON_B;p.analogB=255;
    }
    return p;
}
// Device and UI operations run on the guest thread; XR reads a locked snapshot.
void Poll();
void HandleSdlEvent(const SDL_Event& event);
void DrawSettings();
bool ReadPad(PADStatus& pad,bool blocked,bool race);
bool SteeringSnapshot(float& steering);
bool Motor(int channel,unsigned command);
void Shutdown();
}
