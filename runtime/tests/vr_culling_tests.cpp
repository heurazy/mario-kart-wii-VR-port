#include "vr/mkw_vr_culling.h"

#include <iostream>

int main() {
    using namespace mkw::vr::culling;
    // A 90-degree camera pointing along +Z, with its side planes meeting at
    // the origin. A banana behind the kart is outside the original frustum,
    // but remains close enough to be visible when the player turns their head.
    const Frustum frustum{
        {{-100, -100, 0}, {100, 100, 100}},
        {{{{1, 0, 1}, 0}, {{-1, 0, 1}, 0}, {{0, 1, 1}, 0},
          {{0, -1, 1}, 0}, {{0, 0, 1}, 0}, {{0, 0, -1}, 100}}},
    };
    const Aabb ahead{{-2, -2, 40}, {2, 2, 44}};
    const Aabb crossing{{-2, -2, -1}, {2, 2, 1}};
    const Aabb behind{{-2, -2, -70}, {2, 2, -66}};
    const Aabb distant{{-2, -2, -400}, {2, 2, -396}};
    Vec3 apex{};
    if (!CameraApex(frustum, apex) || apex.x != 0 || apex.y != 0 || apex.z != 0 ||
        Classify(frustum, ahead) != 1 || Classify(frustum, crossing) != 2 ||
        Classify(frustum, behind) != 0 ||
        !WithinOmnidirectionalRange(frustum, behind) ||
        WithinOmnidirectionalRange(frustum, distant)) {
        std::cerr << "VR culling must retain the original frustum result and keep nearby rear objects\n";
        return 1;
    }

    Frustum degenerate = frustum;
    degenerate.planes[0] = {{0, 0, 0}, 0};
    if (CameraApex(degenerate, apex) || !WithinOmnidirectionalRange(degenerate, behind) ||
        WithinOmnidirectionalRange(degenerate, distant)) {
        std::cerr << "Degenerate side planes need a bounded fallback\n";
        return 1;
    }
    if (!RestoreClipInfoDirectionCull(0x01, 100.0f, 400.0f, false) ||
        RestoreClipInfoDirectionCull(0x03, 500.0f, 400.0f, false) ||
        RestoreClipInfoDirectionCull(0x01, 100.0f, 400.0f, true) ||
        RestoreClipInfoDirectionCull(0x21, 100.0f, 400.0f, false) ||
        RestoreClipInfoDirectionCull(0x81, 100.0f, 400.0f, false) ||
        RestoreClipInfoDirectionCull(0x01, 500.0f, 400.0f, false)) {
        std::cerr << "VR clipping must restore only nearby direction-culled objects\n";
        return 1;
    }
    return 0;
}
