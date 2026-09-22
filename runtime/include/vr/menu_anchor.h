// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "vr/onboarding.h"

namespace mkw::vr {
// Wait for a coherent, upright pose, rather than accepting the placeholder
// origin some runtimes report while the headset wakes up. Natural head motion
// must not prevent a menu from appearing indefinitely.
class MenuAnchorStability {
public:
    void Reset() { since_ = 0; }
    bool Update(const UiHandPose& head, int64_t time) {
        const auto finite=[](float f) {
            uint32_t bits; std::memcpy(&bits,&f,sizeof(bits));
            return (bits&0x7f800000u)!=0x7f800000u;
        };
        bool valid=head.valid && time>0 && head.up[1]>.5f &&
            head.forward[0]*head.forward[0]+head.forward[2]*head.forward[2]>.25f;
        for(int i=0;i<3;++i)
            valid=valid && finite(head.position[i]) && finite(head.up[i]) && finite(head.forward[i]);
        if(!valid) { Reset(); return false; }
        float distance2=0, direction=0;
        for(int i=0;i<3;++i) {
            const float delta=head.position[i]-candidate_.position[i];
            distance2+=delta*delta;
            direction+=head.forward[i]*candidate_.forward[i];
        }
        if(!since_ || time<since_ || distance2>.16f || direction<.258819f) {
            candidate_=head; since_=time;
        }
        return time-since_>=400000000;
    }
private:
    int64_t since_=0;
    UiHandPose candidate_{};
};
}
