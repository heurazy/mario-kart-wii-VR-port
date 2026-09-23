// SPDX-License-Identifier: GPL-3.0-or-later

#include "hle_stubs.h"
#include "memory.h"
#include "vr/mkw_vr_culling.h"
#include "vr/mkw_vr_policy.h"

extern "C" void func_80086610(CpuContext* context);
extern "C" void func_80787774(CpuContext* context);

namespace {

mkw::vr::culling::Vec3 ReadVec3(uint32_t address) {
    return {MemoryInline::FlatReadFloat32(address),
            MemoryInline::FlatReadFloat32(address + 4),
            MemoryInline::FlatReadFloat32(address + 8)};
}

bool WithinGuestOmnidirectionalRange(uint32_t frustum_address, uint32_t box_address) {
    using namespace mkw::vr::culling;
    // PAL RMCP01 nw4r::math::FRUSTUM layout, confirmed against Set(4 dirs)
    // and IntersectAABB_Ex at 0x80086610. Bounds start at +120 and six
    // inward-facing planes start at +144. An AABB is min followed by max.
    Frustum frustum{};
    frustum.bounds = {ReadVec3(frustum_address + 120), ReadVec3(frustum_address + 132)};
    const Aabb box{ReadVec3(box_address), ReadVec3(box_address + 12)};
    if (!Valid(frustum.bounds) || !Valid(box)) {
        return false;
    }
    for (uint32_t index = 0; index < frustum.planes.size(); ++index) {
        const uint32_t address = frustum_address + 144 + index * 16;
        frustum.planes[index] = {ReadVec3(address), MemoryInline::FlatReadFloat32(address + 12)};
    }

    return WithinOmnidirectionalRange(frustum, box);
}

void UpdateClipInfoForVr(CpuContext* context) {
    const uint32_t manager = context->gpr[3];
    // Keep the game's distance, area and object-state calculations intact.
    // This function runs once per game tick, before objects use its screen flags.
    func_80787774(context);
    if (!mkw::vr::MkwVRPolicyExpandRaceCulling()) {
        return;
    }

    // PAL ClipInfoMgr: entries at +16 (36 bytes each), count at +24 and
    // ClipScreenInfo at +28. Immersive VR is only selected for one local screen.
    const uint32_t entries = MemoryInline::FlatRead32(manager + 16);
    const uint32_t count = MemoryInline::FlatRead32(manager + 24);
    const uint32_t screen = MemoryInline::FlatRead32(manager + 28);
    if (!entries || !screen) {
        return;
    }
    const uint16_t screen_area = MemoryInline::FlatRead16(screen + 92);
    for (uint32_t index = 0; index < count; ++index) {
        const uint32_t entry = entries + index * 36u;
        const uint8_t flags = MemoryInline::FlatRead8(entry + 32);
        if ((flags & 0xA3u) != 0x01u) {
            continue;
        }
        const bool excluded_by_area = MemoryInline::FlatRead8(entry + 30) != 2 &&
            (MemoryInline::FlatRead16(entry + 28) & screen_area) != 0;
        if (mkw::vr::culling::RestoreClipInfoDirectionCull(
                flags, MemoryInline::FlatReadFloat32(entry + 12),
                MemoryInline::FlatReadFloat32(entry + 8), excluded_by_area)) {
            MemoryInline::FlatWrite8(entry + 32, static_cast<uint8_t>(flags & ~1u));
        }
    }
}

} // namespace

extern "C" void IntersectAabbForVr(CpuContext* context) {
    const uint32_t frustum = context->gpr[3];
    const uint32_t box = context->gpr[4];
    // Preserve the game's exact answer in desktop/menus and for already
    // visible objects. Only extend a rejected box in an immersive race.
    func_80086610(context);
    if (context->gpr[3] == 0 && mkw::vr::MkwVRPolicyExpandRaceCulling() &&
        WithinGuestOmnidirectionalRange(frustum, box)) {
        // "Intersecting" keeps ScnObjGather's child tests active.
        context->gpr[3] = 2;
    }
}

REGISTER_NATIVE_FUNCTION_AS(0x80086610, IntersectAabbForVr,
                            "nw4r::math::FRUSTUM::IntersectAABB_Ex VR extension");
REGISTER_NATIVE_FUNCTION_AS(0x80787774, UpdateClipInfoForVr, "ClipInfoMgr::Update VR direction culling");
