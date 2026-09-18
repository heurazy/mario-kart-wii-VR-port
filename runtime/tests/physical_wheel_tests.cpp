#include "physical_wheel.h"
#include <iostream>
using namespace physical_wheel;
int main() {
    int errors=0;
    auto check=[&](bool ok,const char* why){if(!ok){std::cerr<<why<<'\n';++errors;}};
    check(Steering(-32768,-32768,100,32767,.02f)==-1,"left endpoint");
    check(Steering(32767,-32768,100,32767,.02f)==1,"right endpoint");
    check(Steering(200,-32768,100,32767,.02f)==0,"calibrated center deadzone");
    check(Steering(-32768,32767,0,-32768,0)==1,"inverted steering");
    check(Steering(100,0,0,100,0)==0,"reject missing calibration");
    check(Pedal(-32768,32767,-32768)==1,"reversed pedals");
    check(Pedal(32767,32767,-32768)==0,"released reversed pedal");
    check(Pedal(-16000,0,32767)==0 && Pedal(16000,0,-32768)==0,"combined pedals isolate opposite direction");
    check(Pedal(30000,0,10000)==1,"pedal clamps beyond calibration");
    auto p=Map(.5f,1,0,true,true);
    check(p.stickX==50 && (p.button&PAD_BUTTON_A) && (p.button&PAD_TRIGGER_R) && (p.button&PAD_TRIGGER_L),"accelerate drift and item together");
    p=Map(-1,1,1,true,true);
    check(p.stickX==-100 && (p.button&PAD_BUTTON_B) && !(p.button&(PAD_BUTTON_A|PAD_TRIGGER_R)) && p.analogA==0 && p.triggerR==0 && (p.button&PAD_TRIGGER_L),"brake overrides throttle and drift while preserving item");
    return errors?1:0;
}
