#include "vr/mkw_vr_policy.h"
#include "vr/frame_delivery.h"
#include <iostream>

int main() {
    using namespace mkw::vr;
    MkwVRPolicyReset();
    MkwVRPolicyConfig config;
    config.enabled = true;
    MkwVRPolicyConfigure(config);
    MkwVRPolicySetSessionActive(true);
    MkwVRPolicySetAvailableBindings(kMkwVRRequiredImmersiveBindings);
    MkwVRPolicyPublishScene({VRSceneMode::Race, 1, 10});
    MkwVRCameraObservation camera;
    camera.valid = true;
    camera.guest_frame_index = 10;
    camera.view_from_world = {1,0,0,0,0,1,0,0,0,0,1,0};
    MkwVRPolicyPublishRaceCamera(camera);
    const auto race = MkwVRPolicyGetSnapshot();
    detail::FrameDelivery delivery;
    delivery.Start(1, race.content_tag, 1);
    delivery.Complete(1);
    // Simulate the display thread sampling between independent scene/camera
    // callbacks. Fresh replay is unsafe; the completed race image remains safe.
    camera.guest_frame_index = 13;
    MkwVRPolicyPublishRaceCamera(camera);
    const auto gap = MkwVRPolicyGetSnapshot();
    if (gap.presentation != VRPresentationMode::VirtualScreen ||
        gap.content_tag == gap.display_content_tag ||
        !delivery.CanDisplay(gap.display_content_tag, 1)) {
        std::cerr << "Incomplete guest observations must not blank the last completed race image\n";
        return 1;
    }
    MkwVRPolicyPublishScene({VRSceneMode::Race, 1, 13});
    const auto coherent = MkwVRPolicyGetSnapshot();
    if (coherent.content_tag != coherent.display_content_tag ||
        !delivery.CanDisplay(coherent.display_content_tag, 1)) {
        std::cerr << "Coherent observation recovery changed the display generation\n";
        return 1;
    }
    MkwVRPolicySetSettingsVisible(true);
    const auto menu = MkwVRPolicyGetSnapshot();
    MkwVRPolicySetSettingsVisible(true);
    const auto menuAgain = MkwVRPolicyGetSnapshot();
    MkwVRPolicySetSettingsVisible(false);
    const auto resumed = MkwVRPolicyGetSnapshot();
    if (race.presentation != VRPresentationMode::ImmersiveRace ||
        menu.presentation != VRPresentationMode::VirtualScreen ||
        menu.content_tag == race.content_tag || menuAgain.content_tag != menu.content_tag ||
        resumed.presentation != VRPresentationMode::ImmersiveRace || resumed.content_tag == menu.content_tag ||
        resumed.content_tag == race.content_tag) {
        std::cerr << "VR settings must show a mono UI and invalidate cached images at each transition\n";
        return 1;
    }
    if (delivery.CanDisplay(menu.display_content_tag, 1) ||
        delivery.CanDisplay(resumed.display_content_tag, 1) ||
        delivery.CanDisplay(coherent.display_content_tag, 2)) {
        std::cerr << "Cached race image escaped a real presentation or session transition\n";
        return 1;
    }
    return 0;
}
