#include "vr/onboarding.h"
#include "vr/menu_anchor.h"
#include <iostream>
#include <limits>
using namespace mkw::vr;
int main() {
    int failures=0;
    const auto check=[&](bool result,const char* message) { if(!result) { ++failures;std::cerr<<message<<'\n'; } };
    UiHandPose ray;ray.valid=true;float u=0,v=0;
    check(ProjectUiRay(ray,2,1,2,u,v)&&u==.5f&&v==.5f,"forward ray hits centre");
    ray.position={1,.5f,0};
    check(ProjectUiRay(ray,2,1,2,u,v)&&u==1&&v==0,"ray maps top right correctly");
    ray.position[0]=1.1f;check(!ProjectUiRay(ray,2,1,2,u,v),"outside panel rejected");
    ray={};ray.valid=true;ray.forward={0,0,1};check(!ProjectUiRay(ray,2,1,2,u,v),"backward ray rejected");
    ray.forward={0,0,-1};ray.position[0]=std::numeric_limits<float>::quiet_NaN();
    check(!ProjectUiRay(ray,2,1,2,u,v),"invalid tracking rejected");
    for(bool first : {false,true}) {
        TutorialFlow flow;
        check(!flow.Update(true,first,false,0,0),"guide waits for playable race");
        check(flow.Update(true,first,false,0,2),"first active camera requests pause");
        check(flow.stage!=TutorialFlow::Stage::Showing,"guide cannot show before pause acknowledgement");
        check(!flow.Update(true,first,true,0,2.1)&&flow.stage==TutorialFlow::Stage::Showing,"actual pause opens guide");
        const unsigned completed=flow.bit;flow={};
        check(!flow.Update(true,first,false,completed,3),"seen controls do not repeat");
        flow.Update(true,!first,false,completed,4);
        check(flow.Update(true,!first,false,completed,6),"other driving mode requests its own guide");
        flow.Update(false,!first,false,completed,7);
        check(flow.stage==TutorialFlow::Stage::Idle,"race exit resets pending tutorial");
    }
    MenuAnchorStability anchor;
    UiHandPose head; head.valid=true; head.position={0,1.65f,0};
    check(!anchor.Update(head,1000000000),"first tracked pose cannot anchor immediately");
    head.position[1]=1.1f;
    check(!anchor.Update(head,1400000000),"startup height jump restarts settling");
    check(!anchor.Update(head,1700000000),"new origin needs its own stable interval");
    check(anchor.Update(head,1800000000),"stable tracked pose anchors");
    anchor.Reset();
    check(!anchor.Update(head,2000000000),"focus/recenter reset discards old stability");
    head.up={0,-1,0};
    check(!anchor.Update(head,2400000000),"upside down headset cannot establish menu origin");
    head.up={0,1,0}; head.valid=false;
    check(!anchor.Update(head,2500000000),"untracked pose rejected");
    head.valid=true;
    check(!anchor.Update(head,2600000000),"tracking recovery restarts settling");
    head.forward={1,0,0};
    check(!anchor.Update(head,3000000000),"turn during startup restarts settling");
    check(anchor.Update(head,3400000000),"stable new heading anchors");
    anchor.Reset();
    head.position={0,1.1f,0}; head.forward={0,0,-1};
    check(!anchor.Update(head,3500000000),"menu begins settling while head moves");
    head.position[0]=.12f;
    check(anchor.Update(head,3900000000),"normal head movement does not block the menu");
    head.position[0]=std::numeric_limits<float>::quiet_NaN();
    check(!anchor.Update(head,3500000000),"invalid coordinates rejected");
    TutorialFlow flow;
    check(!flow.Update(true,false,true,0,0),"existing player pause is not claimed by tutorial");
    int requests=0;
    for(int i=1;i<=60;++i) requests+=flow.Update(true,false,false,0,i);
    check(requests==8&&flow.stage!=TutorialFlow::Stage::Showing,"unpausable session has bounded retries and no blocking guide");
    check(TutorialBit(false)==1&&TutorialBit(true)==2,"third person/diorama and cockpit have independent persistence bits");
    std::cout<<(failures?"FAIL":"PASS")<<": onboarding\n";
    return failures?1:0;
}
