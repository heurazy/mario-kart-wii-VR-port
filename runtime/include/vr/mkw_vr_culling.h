// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace mkw::vr::culling {

struct Vec3 {
    float x, y, z;
};

struct Aabb {
    Vec3 min, max;
};

struct Plane {
    Vec3 normal;
    float distance;
};

struct Frustum {
    Aabb bounds;
    std::array<Plane, 6> planes;
};

inline float Dot(Vec3 a, Vec3 b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline Vec3 Cross(Vec3 a, Vec3 b) noexcept {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.y * b.x};
}

inline bool Finite(float value) noexcept {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return (bits & 0x7f800000u) != 0x7f800000u;
}

inline bool Valid(const Aabb& box) noexcept {
    return Finite(box.min.x) && Finite(box.min.y) && Finite(box.min.z) &&
           Finite(box.max.x) && Finite(box.max.y) && Finite(box.max.z) &&
           box.min.x <= box.max.x && box.min.y <= box.max.y && box.min.z <= box.max.z;
}

// ClipInfoMgr uses bit 0 for a rejected screen, bit 1 for an out-of-range
// object, and bits 5/7 for suspended or disabled screen entries. Keep its
// distance and area checks; only undo a direction-plane rejection in VR.
inline bool RestoreClipInfoDirectionCull(uint8_t flags, float scaled_distance_squared,
                                         float limit_squared, bool excluded_by_area) noexcept {
    return (flags & 0xA3u) == 0x01u && !excluded_by_area &&
           Finite(scaled_distance_squared) && Finite(limit_squared) &&
           limit_squared > 0.0f && scaled_distance_squared <= limit_squared;
}

// Faithful result codes of PAL nw4r::math::FRUSTUM::IntersectAABB_Ex:
// 0 outside, 1 completely inside, 2 intersecting a plane.
inline uint32_t Classify(const Frustum& frustum, const Aabb& box) noexcept {
    const auto& f = frustum.bounds;
    if (box.min.x > f.max.x || box.max.x < f.min.x ||
        box.min.y > f.max.y || box.max.y < f.min.y ||
        box.min.z > f.max.z || box.max.z < f.min.z) {
        return 0;
    }

    uint32_t result = 1;
    for (const Plane& plane : frustum.planes) {
        const Vec3 support_max{
            plane.normal.x >= 0 ? box.max.x : box.min.x,
            plane.normal.y >= 0 ? box.max.y : box.min.y,
            plane.normal.z >= 0 ? box.max.z : box.min.z,
        };
        const Vec3 support_min{
            plane.normal.x >= 0 ? box.min.x : box.max.x,
            plane.normal.y >= 0 ? box.min.y : box.max.y,
            plane.normal.z >= 0 ? box.min.z : box.max.z,
        };
        if (Dot(plane.normal, support_max) + plane.distance < 0) {
            return 0;
        }
        if (Dot(plane.normal, support_min) + plane.distance < 0) {
            result = 2;
        }
    }
    return result;
}

inline bool CameraApex(const Frustum& frustum, Vec3& result) noexcept {
    // Set(4 dirs) stores the four side planes before near/far. Their common
    // point is the game camera, independent of the kart's current heading.
    const auto& p = frustum.planes;
    const Vec3 c12 = Cross(p[1].normal, p[2].normal);
    const Vec3 c20 = Cross(p[2].normal, p[0].normal);
    const Vec3 c01 = Cross(p[0].normal, p[1].normal);
    const float determinant = Dot(p[0].normal, c12);
    if (!Finite(determinant) || std::abs(determinant) < 1.0e-5f) {
        return false;
    }
    const float inverse = 1.0f / determinant;
    result = {
        (-p[0].distance * c12.x - p[1].distance * c20.x - p[2].distance * c01.x) * inverse,
        (-p[0].distance * c12.y - p[1].distance * c20.y - p[2].distance * c01.y) * inverse,
        (-p[0].distance * c12.z - p[1].distance * c20.z - p[2].distance * c01.z) * inverse,
    };
    if (!Finite(result.x) || !Finite(result.y) || !Finite(result.z)) {
        return false;
    }

    const Vec3 span{frustum.bounds.max.x - frustum.bounds.min.x,
                    frustum.bounds.max.y - frustum.bounds.min.y,
                    frustum.bounds.max.z - frustum.bounds.min.z};
    const float diagonal = std::sqrt(Dot(span, span));
    const float fourth_length = std::sqrt(Dot(p[3].normal, p[3].normal));
    const float fourth_distance = Dot(p[3].normal, result) + p[3].distance;
    return Finite(diagonal) && diagonal > 0 && Finite(fourth_length) &&
           fourth_length > 1.0e-5f && Finite(fourth_distance) &&
           std::abs(fourth_distance) <= fourth_length * diagonal * 0.02f &&
           result.x >= frustum.bounds.min.x - diagonal && result.x <= frustum.bounds.max.x + diagonal &&
           result.y >= frustum.bounds.min.y - diagonal && result.y <= frustum.bounds.max.y + diagonal &&
           result.z >= frustum.bounds.min.z - diagonal && result.z <= frustum.bounds.max.z + diagonal;
}

inline bool WithinOmnidirectionalRange(const Frustum& frustum, const Aabb& box) noexcept {
    if (!Valid(frustum.bounds) || !Valid(box)) {
        return false;
    }

    Vec3 centre{};
    const bool has_apex = CameraApex(frustum, centre);
    if (!has_apex) {
        centre = {(frustum.bounds.min.x + frustum.bounds.max.x) * 0.5f,
                  (frustum.bounds.min.y + frustum.bounds.max.y) * 0.5f,
                  (frustum.bounds.min.z + frustum.bounds.max.z) * 0.5f};
    }

    // Keep the game's own far reach, but in every direction the headset can
    // face. This is a CPU superset; the stereo GPU projection still clips the
    // geometry that neither eye sees. Invalid side-plane intersections use a
    // conservative bound centred on the frustum AABB instead.
    const Vec3 extreme{
        std::max(std::abs(centre.x - frustum.bounds.min.x), std::abs(centre.x - frustum.bounds.max.x)),
        std::max(std::abs(centre.y - frustum.bounds.min.y), std::abs(centre.y - frustum.bounds.max.y)),
        std::max(std::abs(centre.z - frustum.bounds.min.z), std::abs(centre.z - frustum.bounds.max.z)),
    };
    float radius = std::sqrt(Dot(extreme, extreme));
    if (!has_apex) {
        radius *= 2.0f;
    }
    if (!Finite(radius) || radius <= 0) {
        return false;
    }
    radius += std::max(50.0f, radius * 0.05f);

    const Vec3 nearest{
        std::clamp(centre.x, box.min.x, box.max.x),
        std::clamp(centre.y, box.min.y, box.max.y),
        std::clamp(centre.z, box.min.z, box.max.z),
    };
    const Vec3 delta{nearest.x - centre.x, nearest.y - centre.y, nearest.z - centre.z};
    return Dot(delta, delta) <= radius * radius;
}

} // namespace mkw::vr::culling
