// SPDX-License-Identifier: GPL-3.0-or-later

#include "vr/mkw_vr_first_person.h"
#include "vr/cockpit_stabilizer.h"
#include "vr/native_wheel_mesh.h"
#include "vr/quest_input.h"
#include <aurora/aurora.h>

#include "memory.h"
#include "isa/ppc_isa_context.h"
#include "runtime_config.h"
#include "runtime_log.h"
#include "vr/mkw_vr_policy.h"

#include <mutex>
#include <string>

extern "C" void func_805A6C58(CpuContext* context);
extern "C" void func_8055CB08(CpuContext* context);

namespace mkw::vr {
namespace {

// ---------------------------------------------------------------------------
// PAL RMCP01 object layout.
//
// Derived from the shipped StaticR.rel and cross-checked against the mkw
// decompilation. Each constant names the accessor that proves it, so a future
// region or a mod that moves these can be re-derived the same way. Keep in
// sync with projects/mkwii/MAP.txt and the generated translations.
// ---------------------------------------------------------------------------

// RaceCamera::GetViewMtx (0x805A6C58) writes the authoritative view matrix to
// its r4 output buffer. The adjacent RaceCamera fields are state vectors, not
// a view matrix, so call the game's getter instead of guessing an object offset.
constexpr uint32_t kRaceCameraScratchBytes = 0x300u;

// Kart::Manager's instance pointer. Its CreateInstance (0x8058FAA8) resolves
// the slot as 0x809C0000 + 6392 in the generated translation. Read directly
// rather than observed from Kart::Manager::Update's r3, so enabling the camera
// needs no change to the translated output: an entry observer only exists in a
// build whose translation was regenerated for it, and its absence is silent.
// This mirrors how the race scene's instance slot is reached in
// mkw_vr_instrumentation.cpp.
constexpr uint32_t kKartManagerInstanceAddress = 0x809C18F8u;
// Kart::Manager::GetKartPlayer (0x80590100): `lwz r3,0x20(r3)` then indexes.
constexpr uint32_t kKartManagerPlayersOffset = 0x20u;
// Kart::Link::GetKartPosition (0x8059020C) walks proxy -> accessor -> body ->
// physics -> dynamics; the first three links are shared by every kart accessor.
constexpr uint32_t kKartProxyAccessorOffset = 0x00u;
constexpr uint32_t kKartAccessorBodyOffset = 0x08u;
constexpr uint32_t kKartBodyPhysicsOffset = 0x90u;
// KartPhysics::pose (Kart::Link::GetMtx 0x80590264). This is the physics-driven
// pose, deliberately not the visual one: an animated frame would bob the
// camera. Kart::Link::GetKartBodyMtx (0x80590278) returns KartBody+0x1C, the
// visual pose, and is the alternative to try if the seat ever looks detached.
constexpr uint32_t kKartPhysicsPoseOffset = 0x9Cu;

// RaceCamera::Init (805A2034) reads the signed player byte at +0x9C and
// passes it to Kart::Manager::GetKartPlayer. Online local racers need not be
// player zero: follow the exact racer used by the active game's camera.
constexpr uint32_t kRaceCameraPlayerOffset = 0x9Cu;

// Frames the last good anchor survives a failed read before the camera returns
// to the game's own. Rides out a transient null during a respawn or transition
// without letting a genuinely broken anchor persist.
constexpr int kHoldFrames = 10;

// ---------------------------------------------------------------------------
// Guest reads. Everything is bounds-checked and exception-guarded so a pointer
// caught mid-teardown can only cost this frame's anchor.
// ---------------------------------------------------------------------------

bool ReadGuestPointer(uint32_t address, uint32_t& out) noexcept {
    return Memory::TryRead32(address, out) && out != 0;
}

constexpr uint32_t kMtx34Bytes = 12u * sizeof(float);

bool ReadGuestMtx34(uint32_t address, Mtx34& out) noexcept {
    if (address == 0 || !Memory::Contains(address, kMtx34Bytes)) {
        return false;
    }
    try {
        for (uint32_t i = 0; i < out.size(); ++i) {
            out[i] = Memory::ReadFloat32(address + i * static_cast<uint32_t>(sizeof(float)));
        }
    } catch (const Memory::AccessViolation&) {
        return false;
    }
    return detail::IsFiniteMtx34(out);
}

bool ReadRaceCameraViewMatrix(const CpuContext* context, uint32_t camera_address,
                              Mtx34& out) noexcept {
    if (context == nullptr || camera_address == 0 ||
        context->gpr[1] < kRaceCameraScratchBytes) {
        return false;
    }

    CpuContext call_context = *context;
    const uint32_t scratch = context->gpr[1] - kRaceCameraScratchBytes;
    call_context.gpr[3] = camera_address;
    call_context.gpr[4] = scratch;
    call_context.gpr[5] = scratch + 48u;
    // GetViewMtx's third argument is a floating point dolly amount, not an
    // implicit default. Inheriting the caller's f1 made the anchor use a
    // different camera from the GX world draws (and changed on impact).
    call_context.fpr[1].d = 0.0;
    try {
        CpuContextScope scope(&call_context);
        func_805A6C58(&call_context);
        return ReadGuestMtx34(scratch, out);
    } catch (const Memory::AccessViolation&) {
        return false;
    }
}

// The pointer walk, kept inspectable: on failure `failed_step` names the link
// that broke and the resolved pointers before it are still filled in. One log
// line then says exactly which offset needs revisiting.
struct KartPoseRead {
    const char* failed_step = nullptr;
    uint32_t manager = 0;
    uint32_t players = 0;
    uint32_t proxy = 0;
    uint32_t accessor = 0;
    uint32_t body = 0;
    uint32_t physics = 0;
};

KartPoseRead ReadPlayerKartPose(uint32_t camera, Mtx34& out) noexcept {
    KartPoseRead read{};
    if (!camera || !Memory::Contains(camera, kRaceCameraPlayerOffset + 1)) {
        read.failed_step = "race camera player";
        return read;
    }
    const uint32_t player = Memory::Read8(camera + kRaceCameraPlayerOffset);
    if (player >= 12) {
        read.failed_step = "invalid race camera player";
        return read;
    }
    if (!ReadGuestPointer(kKartManagerInstanceAddress, read.manager)) {
        read.failed_step = "Kart::Manager instance";
    } else if (!ReadGuestPointer(read.manager + kKartManagerPlayersOffset, read.players)) {
        read.failed_step = "Kart::Manager players array";
    } else if (!ReadGuestPointer(read.players + player * 4u, read.proxy)) {
        read.failed_step = "player kart object";
    } else if (!ReadGuestPointer(read.proxy + kKartProxyAccessorOffset, read.accessor)) {
        read.failed_step = "kart accessor";
    } else if (!ReadGuestPointer(read.accessor + kKartAccessorBodyOffset, read.body)) {
        read.failed_step = "kart body";
    } else if (!ReadGuestPointer(read.body + kKartBodyPhysicsOffset, read.physics)) {
        read.failed_step = "kart physics";
    } else if (!ReadGuestMtx34(read.physics + kKartPhysicsPoseOffset, out)) {
        read.failed_step = "kart pose matrix";
    }
    return read;
}

// ---------------------------------------------------------------------------

struct FirstPersonState {
    CockpitStabilizer stabilizer;
    uint64_t stabilized_frame = 0;
    SeatedEyeReference seated_eye{};
    uint32_t seated_driver=0;
    std::optional<float> cockpit_forward;
    CameraMode mode = CameraMode::Game;
    bool opening_pending = false;
    FirstPersonHeadOffsets offsets{};
    float units_per_meter = 100.0f;

    uint32_t camera_address = 0;

    FirstPersonAnchor anchor{};
    int hold_frames = 0;
    bool ever_valid_this_race = false;
    bool failure_logged = false;
    uint64_t logged_frame = 0;
};

std::mutex g_mutex;
FirstPersonState g_state;

// These are restored as soon as GX has recorded the frame, before simulation
// resumes. Do not retain guest model pointers across frames or scene teardown.
std::array<uint32_t, 6> g_hidden_models{};
void SetDriverDraw(uint32_t model, bool enabled) noexcept {
    auto* context = TryGetCpuContext();
    if (!context || !Memory::Contains(model, 0x4c)) return;
    try {
        CpuContext call = *context;
        call.gpr[3] = model;
        call.gpr[4] = enabled;
        CpuContextScope scope(&call);
        func_8055CB08(&call); // ModelDirector::EnableDraw (also updates ScnMdl options).
    } catch (const Memory::AccessViolation&) {}
}

uint32_t LocalDriver(const KartPoseRead& kart) noexcept {
    uint32_t driver = 0;
    // Kart::Link::GetDriverController, 80590A40.
    if (kart.accessor) ReadGuestPointer(kart.accessor + 0x14, driver);
    return Memory::Contains(driver, 0x144) ? driver : 0;
}

bool PublishNativeWheelMesh(const KartPoseRead& kart,const Mtx34& modelView,
                           const Mtx34& left,const Mtx34& right,
                           uint32_t part=0,const Mtx34* correction=nullptr) noexcept {
    uint32_t model=0,mdl=0;
    if(!ReadGuestPointer((part?part:kart.body)+0x7c,model) || !ReadGuestPointer(model+0xc,mdl) ||
       !Memory::Contains(mdl,0x40) || Memory::Read32(mdl)!=0x4d444c30) return false;
    const auto input=ReadQuestInputSnapshot();
    if(!input.active) return false;
    const float steering=QuestAxis(input.wheel_active?input.wheel_steering:input.steering_x);
    // In native model coordinates +X is the driver's left. Positive rotation
    // around +Z consequently looks clockwise to the seated driver.
    const float angle=input.cockpit_controls && detail::IsFiniteFloat(&input.wheel_angle)
        ? input.wheel_angle : steering*1.570796327f;
    const detail::Vec3 center{(left[3]+right[3])*0.5f,(left[7]+right[7])*0.5f,(left[11]+right[11])*0.5f};
    const float radius=std::abs(left[3]-right[3])*0.5f;
    bool published=false;
    try {
        const uint32_t version=Memory::Read32(mdl+8);
        if(version<8 || version>11) return false;
        const uint32_t dicOffset=Memory::Read32(mdl+0x18);
        if(!dicOffset || dicOffset>0x100000) return false;
        const uint32_t dic=mdl+dicOffset;
        if(!Memory::Contains(dic,8)) return false;
        const uint32_t count=Memory::Read32(dic+4);
        if(count>16 || !Memory::Contains(dic,8+(count+1)*16)) return false;
        for(uint32_t entry=1;entry<=count;++entry) {
            const uint32_t offset=Memory::Read32(dic+8+entry*16+12);
            if(offset>0x100000) continue;
            const uint32_t header=dic+offset;
            if(!Memory::Contains(header,0x40) || Memory::Read32(header+0x14)!=1) continue;
            const uint32_t dataOffset=Memory::Read32(header+8),type=Memory::Read32(header+0x18);
            const uint32_t stride=Memory::Read8(header+0x1d),num=Memory::Read16(header+0x1e);
            const uint32_t componentSize=type==4?4:(type==2 || type==3?2:1);
            if(type>4 || stride<3*componentSize || num>4096 || dataOffset>0x100000) continue;
            const uint32_t data=header+dataOffset,size=num*stride;
            if(!size || size>65536 || !Memory::Contains(data,size)) continue;
            const float scale=std::ldexp(1.0f,-int(Memory::Read8(header+0x1c)));
            std::vector<detail::Vec3> points(num);
            bool valid=true;
            for(uint32_t i=0;i<num;++i) for(int axis=0;axis<3;++axis) {
                const uint32_t at=data+i*stride+axis*componentSize;
                float value=type==4?Memory::ReadFloat32(at):
                    (type==3?float(int16_t(Memory::Read16(at))):type==2?float(Memory::Read16(at)):
                     type==1?float(int8_t(Memory::Read8(at))):float(Memory::Read8(at)))*scale;
                if(!detail::IsFiniteFloat(&value)) valid=false;
                if(axis==0) points[i].x=value;
                else if(axis==1) points[i].y=value;
                else points[i].z=value;
            }
            if(!valid) continue;
            if(correction && part) {
                for(auto& point:points) point=detail::TransformPoint(*correction,point.x,point.y,point.z);
            } else if(RotateNativeWheelVertices(points,center,radius,angle,correction)<8) continue;
            const auto* source=Memory::GetPointer(data,size);
            std::vector<uint8_t> bytes(source,source+size);
            for(uint32_t i=0;i<num;++i) for(int axis=0;axis<3;++axis) {
                const float value=axis==0?points[i].x:axis==1?points[i].y:points[i].z;
                uint32_t encoded=0;
                if(type==4) std::memcpy(&encoded,&value,4);
                else {
                    const float quantized=std::round(value/scale);
                    const float low=type==3?-32768.0f:type==1?-128.0f:0;
                    const float high=type==3?32767.0f:type==2?65535.0f:type==1?127.0f:255.0f;
                    if(quantized<low || quantized>high) { valid=false; break; }
                    encoded=uint32_t(int32_t(quantized));
                }
                for(uint32_t b=0;b<componentSize;++b)
                    bytes[i*stride+axis*componentSize+b]=uint8_t(encoded>>((componentSize-b-1)*8));
            }
            if(valid) { aurora_set_native_wheel_vertices(source,bytes.data(),size,modelView.data()); published=true; }
        }
    } catch(const Memory::AccessViolation&) {}
    return published;
}

void HideDriver(uint32_t driver) noexcept {
    uint32_t count = 0;
    if (!driver || !Memory::TryRead32(driver + 0xf0, count) || count > 6) return;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t model = 0, flags = 0;
        if (ReadGuestPointer(driver + 0xd8 + i * 4, model) &&
            Memory::TryRead32(model + 4, flags) && (flags & 0x04000000u)) {
            g_hidden_models[i] = model;
            SetDriverDraw(model, false);
        }
    }
}

// Use the driver's actual model placement and head bind bone. Bind pose keeps
// tricks and hit animations from violently rotating/bobbing the user's head.
// DriverModelBones::resNode is +18; ResNodeData::modelMtx is +70.
bool ReadEyeBounds(uint32_t driver, detail::Vec3& minimum, detail::Vec3& maximum) {
    uint32_t model=0,mdl=0;
    if(!ReadGuestPointer(driver+0x6c,model) || !ReadGuestPointer(model+0xc,mdl) ||
       !Memory::Contains(mdl,0x40) || Memory::Read32(mdl)!=0x4d444c30) return false;
    const uint32_t version=Memory::Read32(mdl+8),offset=Memory::Read32(mdl+0x18);
    if(version<8 || version>11 || !offset || offset>0x100000) return false;
    const uint32_t dic=mdl+offset;
    if(!Memory::Contains(dic,8)) return false;
    const uint32_t count=Memory::Read32(dic+4);
    if(count>64 || !Memory::Contains(dic,8+(count+1)*16)) return false;
    for(uint32_t i=1;i<=count;++i) {
        const uint32_t entry=dic+8+i*16;
        const uint32_t nameOffset=Memory::Read32(entry+8),dataOffset=Memory::Read32(entry+12);
        if(nameOffset>0x100000 || dataOffset>0x100000) continue;
        std::string name;
        for(uint32_t n=0;n<96 && Memory::Contains(dic+nameOffset+n);++n) {
            const char c=Memory::Read8(dic+nameOffset+n);
            if(!c) break;
            name+=c;
        }
        if(name.find("_eye")==std::string::npos) continue;
        const uint32_t positions=dic+dataOffset;
        if(!Memory::Contains(positions,0x38) || Memory::Read32(positions+0x14)!=1) continue;
        minimum={Memory::ReadFloat32(positions+0x20),Memory::ReadFloat32(positions+0x24),Memory::ReadFloat32(positions+0x28)};
        maximum={Memory::ReadFloat32(positions+0x2c),Memory::ReadFloat32(positions+0x30),Memory::ReadFloat32(positions+0x34)};
        return true;
    }
    return false;
}

std::array<float,3> ReadPlayerScale(const KartPoseRead& kart) noexcept {
    std::array<float,3> scale{1,1,1};
    uint32_t movement=0;
    if(ReadGuestPointer(kart.accessor+0x28,movement) && Memory::Contains(movement+0x164,12))
        for(int axis=0;axis<3;++axis) scale[axis]=ValidPlayerScale(Memory::ReadFloat32(movement+0x164+axis*4));
    return scale;
}

bool ReadDriverEye(const KartPoseRead& kart, std::array<float, 3>& eye) noexcept {
    const uint32_t driver=LocalDriver(kart);
    if(g_state.seated_driver!=driver) { g_state.seated_driver=driver;g_state.seated_eye={};g_state.cockpit_forward.reset(); }
    // The neutral seated reference is intentionally frozen until identity or
    // race reset. Do not rescan model dictionaries and palettes every frame.
    if(driver && g_state.seated_eye.valid) { eye=g_state.seated_eye.value;return true; }
    uint32_t bones = 0;
    Mtx34 placement{};
    if (!driver || !ReadGuestPointer(driver + 0x104, bones) ||
        !Memory::Contains(bones, 36 * 0x60) || !ReadGuestMtx34(driver + 0x78, placement)) return false;
    try {
        for (uint32_t i = 0; i < 36; ++i) {
            uint32_t name = 0, node = 0;
            if (!ReadGuestPointer(bones + i * 0x60 + 0x14, name) ||
                !ReadGuestPointer(bones + i * 0x60 + 0x18, node)) continue;
            std::string text;
            for (uint32_t n = 0; n < 32 && Memory::Contains(name + n); ++n) {
                const char c = Memory::Read8(name + n);
                if (!c) break;
                text += c;
            }
            // PAL DriverMgr's name table (808A7288) calls its head bone face_1.
            if (text != "face_1" && text != "head" && text != "head1" && text != "face") continue;
            Mtx34 bind{};
            if (!ReadGuestMtx34(node + 0x70, bind)) continue;
            detail::Vec3 minimum{},maximum{};
            const bool boundsFound=ReadEyeBounds(driver,minimum,maximum);
            // ModelCalcCallback::GetBoneWorldMtx (8055FA90) walks
            // ModelDirector+10 -> ScnMdlEx+0; ScnMdlSimple::GetScnMtxPos
            // (80071DC0, WORLD=1) returns palette+EC + node.matId*48.
            uint32_t model=0,ex=0,scn=0,palette=0,matId=0;
            Mtx34 faceWorld{},bodyWorld{};
            if(boundsFound && ReadGuestPointer(driver+0x6c,model) && ReadGuestPointer(model+0x10,ex) &&
               ReadGuestPointer(ex,scn) && ReadGuestPointer(scn+0xec,palette) &&
               Memory::TryRead32(node+0x10,matId) && matId<128 &&
               ReadGuestMtx34(palette+matId*48,faceWorld) && ReadGuestMtx34(kart.body+0x1c,bodyWorld)) {
                const detail::Vec3 localEye=boundsFound
                    ? detail::Vec3{(minimum.x+maximum.x)*0.5f,(minimum.y+maximum.y)*0.5f,(minimum.z+maximum.z)*0.5f}
                    : detail::Vec3{0,0,0};
                std::array<float,3> measured{};
                uint32_t damage=0,damageType=0;
                const auto controls=ReadQuestInputSnapshot();
                const bool safe=NeutralPlayerScale(ReadPlayerScale(kart)) && ReadGuestPointer(kart.accessor+0x2c,damage) &&
                    Memory::TryRead32(damage+0x1c,damageType) && damageType==UINT32_MAX &&
                    std::abs(controls.wheel_active?controls.wheel_steering:controls.steering_x)<0.15f &&
                    !controls.trick && std::abs(controls.tricks_y)<0.15f;
                if(ComputeSeatedEye(faceWorld,bodyWorld,localEye,measured)) {
                    const bool hadReference=g_state.seated_eye.valid;
                    g_state.seated_eye.Observe(measured,safe,true);
                    if(!hadReference && g_state.seated_eye.valid) {
                        g_state.cockpit_forward.reset();
                        RT_LOG(RT_TAG_RUNTIME) << "[mkw-vr] seated eye calibrated: (" << measured[0]
                            << ", " << measured[1] << ", " << measured[2] << ")" << std::endl;
                    }
                }
            }
            if(g_state.seated_eye.valid) { eye=g_state.seated_eye.value;return true; }
            if(boundsFound && ComputeDriverEyeFromBounds(bind,placement,minimum,maximum,eye)) {
                static uint32_t lastEyeModel=0;
                if(lastEyeModel!=node) {
                    lastEyeModel=node;
                    RT_LOG(RT_TAG_RUNTIME) << "[mkw-vr] driver eye geometry: seat=(" << eye[0]
                        << ", " << eye[1] << ", " << eye[2] << ")" << std::endl;
                }
                return true;
            }
            const std::array<float, 3> point{bind[3], bind[7] + 8.0f, bind[11] + 8.0f};
            for (int row = 0; row < 3; ++row)
                eye[row] = placement[row * 4 + 3] + placement[row * 4] * point[0] +
                    placement[row * 4 + 1] * point[1] + placement[row * 4 + 2] * point[2];
            if (std::abs(eye[0]) < 300 && eye[1] > 10 && eye[1] < 500 && std::abs(eye[2]) < 400)
                return true;
        }
    } catch (const Memory::AccessViolation&) {}
    return false;
}

void LogAnchorLocked(uint64_t frame, const Mtx34& anchor, const Mtx34& view_from_world,
                     const KartPoseRead& kart, const Mtx34& kart_from_local) noexcept {
    // One line per second at 60 Hz: enough to confirm the offsets on-device
    // without drowning the log during a race.
    if (g_state.logged_frame != 0 && frame - g_state.logged_frame < 60) {
        return;
    }
    g_state.logged_frame = frame;
    const auto& wheel=g_state.anchor.native_wheel;
    RT_LOG(RT_TAG_RUNTIME) << "[mkw-vr] native wheel: valid=" << wheel.valid
        << ", bike=" << g_state.anchor.bike
        << ", center metres=(" << wheel.center[0] << ", " << wheel.center[1] << ", " << wheel.center[2]
        << "), radius=" << wheel.radius << std::endl;
    // The anchor's translation is -R*a, so negating it gives the head's offset
    // from the recorded camera measured in the levelled camera's own axes.
    // While driving it should stay roughly constant: a little to the side, a
    // little below the chase camera, and well in front of it.
    RT_LOG(RT_TAG_RUNTIME) << "[mkw-vr] first-person anchor: frame=" << frame << ", camera=0x"
                           << std::hex << g_state.camera_address << std::dec
                           << ", head from camera (right, up, forward)=(" << -anchor[3] << ", "
                           << -anchor[7] << ", " << anchor[11] << ") units" << std::endl;
    RT_LOG(RT_TAG_RUNTIME) << "[mkw-vr] first-person view: rows=(" << view_from_world[0] << ", "
                           << view_from_world[1] << ", " << view_from_world[2] << "; "
                           << view_from_world[4] << ", " << view_from_world[5] << ", "
                           << view_from_world[6] << "; " << view_from_world[8] << ", "
                           << view_from_world[9] << ", " << view_from_world[10]
                           << "), translation=(" << view_from_world[3] << ", "
                           << view_from_world[7] << ", " << view_from_world[11] << ")"
                           << std::endl;
    RT_LOG(RT_TAG_RUNTIME) << "[mkw-vr] first-person pose: physics=0x" << std::hex << kart.physics
                           << ", pose=0x" << (kart.physics + kKartPhysicsPoseOffset) << std::dec
                           << ", rows=(" << kart_from_local[0] << ", " << kart_from_local[1]
                           << ", " << kart_from_local[2] << "; " << kart_from_local[4] << ", "
                           << kart_from_local[5] << ", " << kart_from_local[6] << "; "
                           << kart_from_local[8] << ", " << kart_from_local[9] << ", "
                           << kart_from_local[10] << "), translation=(" << kart_from_local[3]
                           << ", " << kart_from_local[7] << ", " << kart_from_local[11] << ")"
                           << std::endl;
    RT_LOG(RT_TAG_RUNTIME) << "[mkw-vr] first-person pose bits: translation=(0x"
                           << std::hex << std::bit_cast<uint32_t>(kart_from_local[3]) << ", 0x"
                           << std::bit_cast<uint32_t>(kart_from_local[7]) << ", 0x"
                           << std::bit_cast<uint32_t>(kart_from_local[11]) << ")" << std::dec
                           << std::endl;
}

} // namespace

void MkwVRFirstPersonConfigure(bool enabled, const FirstPersonHeadOffsets& offsets,
                               float units_per_meter) noexcept {
    std::lock_guard lock(g_mutex);
    g_state.mode = enabled ? CameraMode::FirstPerson : CameraMode::Game;
    g_state.anchor = {};
    g_state.hold_frames = 0;
    g_state.offsets = offsets;
    if (detail::IsFiniteFloat(&units_per_meter) && units_per_meter > 0.0f) {
        g_state.units_per_meter = units_per_meter;
    }
    if (!enabled) {
        g_state.anchor = {};
        g_state.hold_frames = 0;
    }
}

void MkwVRCycleCamera() noexcept {
    std::lock_guard lock(g_mutex);
    if(g_state.opening_pending) return;
    g_state.mode = NextCameraMode(g_state.mode);
    g_state.stabilizer = {};
    g_state.anchor = {};
    g_state.hold_frames = 0;
    g_state.failure_logged = false;
    RT_LOG(RT_TAG_RUNTIME) << "[mkw-vr] camera="
        << (g_state.mode == CameraMode::Game ? "game" :
            g_state.mode == CameraMode::FirstPerson ? "first-person" : "far") << std::endl;
}

CameraMode MkwVRGetCameraMode() noexcept {
    std::lock_guard lock(g_mutex);
    return g_state.mode;
}

bool MkwVRRaceIntroActive() noexcept {
    std::lock_guard lock(g_mutex);
    return g_state.opening_pending;
}
FirstPersonCameraSnapshot MkwVRFirstPersonGetSnapshot() noexcept {
    std::lock_guard lock(g_mutex);
    return {g_state.mode,g_state.anchor};
}

void MkwVRSetCameraMode(CameraMode mode) noexcept {
    std::lock_guard lock(g_mutex);
    g_state.mode = g_state.opening_pending ? CameraMode::Game : mode;
    g_state.anchor = {};
    g_state.hold_frames = 0;
    g_state.stabilizer = {};
}

void MkwVRFirstPersonApplyConfiguredSettings() noexcept {
    static bool configured = false;
    const auto mode = MkwVRGetCameraMode();
    const float units_per_meter = RuntimeConfigFile::VrFirstPersonUnitsPerMeter(100.0f);
    const FirstPersonHeadOffsets offsets{
        RuntimeConfigFile::VrFirstPersonHeadRightMeters(0.0f),
        RuntimeConfigFile::VrFirstPersonHeadUpMeters(1.1f),
        RuntimeConfigFile::VrFirstPersonHeadForwardMeters(1.2f),
    };
    MkwVRFirstPersonConfigure(RuntimeConfigFile::VrFirstPerson(false), offsets, units_per_meter);
    if (configured) MkwVRSetCameraMode(mode);
    configured = true;
    MkwVRPolicySetFirstPersonUnitsPerMeter(units_per_meter);
}

void MkwVRFirstPersonReset() noexcept {
    MkwVRFirstPersonRestoreDriver();
    std::lock_guard lock(g_mutex);
    g_state.mode = CameraMode::Game;
    g_state.opening_pending = true;
    g_state.seated_driver=0;
    g_state.seated_eye={};
    g_state.cockpit_forward.reset();
    g_state.camera_address = 0;
    g_state.anchor = {};
    g_state.hold_frames = 0;
    g_state.ever_valid_this_race = false;
    g_state.failure_logged = false;
    g_state.logged_frame = 0;
    g_state.stabilizer = {};
    g_state.stabilized_frame = 0;
}

void MkwVRFirstPersonUpdate(uint64_t guest_frame_index, uint32_t race_camera_address) noexcept {
    MkwVRFirstPersonRestoreDriver();
    std::lock_guard lock(g_mutex);
    if(g_state.opening_pending) {
        // Raceinfo::CreateInstance (80532084): singleton at 809BD730.
        // IsAtLeastStage (80536230): stage at +28. Update (805331B4)
        // leaves stage 0 after the opening pans OR the skip request.
        // A missing/uninitialized pan must never unlock the camera early.
        g_state.mode=CameraMode::Game;
        uint32_t raceInfo=0;
        if(!ReadGuestPointer(0x809BD730u,raceInfo) ||
           !Memory::Contains(raceInfo+0x28,4) || !race_camera_address) return;
        const uint32_t stage=Memory::Read32(raceInfo+0x28);
        if(stage==0 || stage>4) return;
        g_state.opening_pending=false;
        g_state.mode=static_cast<CameraMode>(RuntimeConfigFile::Get().vrDefaultCamera);
        RT_LOG(RT_TAG_RUNTIME) << "[mkw-vr] race introduction finished: stage=" << stage
                              << ", frame=" << guest_frame_index << std::endl;
    }
    if (g_state.mode == CameraMode::Game) {
        Mtx34 unused{};
        const auto kart=ReadPlayerKartPose(race_camera_address, unused);
        std::array<float,3> eye{};
        if(!kart.failed_step) ReadDriverEye(kart,eye);
        g_state.stabilizer = {};
        g_state.anchor = {};
        g_state.hold_frames = 0;
        return;
    }
    g_state.camera_address = race_camera_address;

    Mtx34 view_from_world{};
    Mtx34 kart_from_local{};
    Mtx34 anchor{};
    KartPoseRead kart{};
    float renderUnits=g_state.units_per_meter;
    std::array<float,3> playerScale{1,1,1};
    std::array<float, 3> eye{0.0f, g_state.offsets.up * g_state.units_per_meter, 0.0f};
    const char* failed_step = nullptr;
    if (race_camera_address == 0) {
        failed_step = "race camera (none updated this frame)";
    } else if (!ReadRaceCameraViewMatrix(TryGetCpuContext(), race_camera_address,
                                         view_from_world)) {
        failed_step = "race camera view matrix";
    } else if (kart = ReadPlayerKartPose(race_camera_address, kart_from_local); kart.failed_step != nullptr) {
        failed_step = kart.failed_step;
    } else if (g_state.mode == CameraMode::Far) {
        g_state.stabilizer = {};
        if (!ComputeKartDioramaAnchor(view_from_world, kart_from_local,
                RuntimeConfigFile::VrDioramaDistance(), RuntimeConfigFile::VrDioramaHeight(), anchor)) {
            failed_step = "diorama anchor math";
        }
    } else {
        // Kart::Link::GetKartPosition (8059020C) returns dynamics+68.
        // Movement::dir (+5C), unlike the physics pose, excludes damage spin,
        // trick rotations and visual pitch/roll. Accessor offsets are proven
        // by GetMovement 8059077C and GetDamage 80590D20.
        uint32_t dynamics=0,movement=0,damage=0,damageType=UINT32_MAX;
        if (ReadGuestPointer(kart.physics+4,dynamics) && Memory::Contains(dynamics+0x68,12))
            for (int row=0;row<3;++row) kart_from_local[row*4+3]=Memory::ReadFloat32(dynamics+0x68+row*4);
        if (ReadGuestPointer(kart.accessor+0x28,movement) && Memory::Contains(movement+0x5c,12)) {
            const float x=Memory::ReadFloat32(movement+0x5c), z=Memory::ReadFloat32(movement+0x64);
            if (detail::IsFiniteFloat(&x) && detail::IsFiniteFloat(&z) && x*x+z*z>0.01f) {
                const float inv=1.0f/std::sqrt(x*x+z*z);
                kart_from_local[2]=x*inv; kart_from_local[10]=z*inv;
            }
        }
        if (ReadGuestPointer(kart.accessor+0x2c,damage)) Memory::TryRead32(damage+0x1c,damageType);
        const float dt=g_state.stabilized_frame && guest_frame_index>g_state.stabilized_frame
            ? float(guest_frame_index-g_state.stabilized_frame)/60.0f : 1.0f/60.0f;
        g_state.stabilized_frame=guest_frame_index;
        if (detail::IsFiniteMtx34(kart_from_local))
            kart_from_local=g_state.stabilizer.Update(kart_from_local,damageType!=UINT32_MAX,dt);
        const bool head_found = ReadDriverEye(kart, eye);
        if (!head_found) {
            // KartDriverDispParam contains the character's seat Y/Z for this
            // particular vehicle. Never use the old chase-camera forward offset.
            uint32_t params = 0, seat = 0;
            if (ReadGuestPointer(kart.accessor, params) && ReadGuestPointer(params + 0x1c, seat) &&
                Memory::Contains(seat, 8)) {
                const float y = Memory::ReadFloat32(seat), z = Memory::ReadFloat32(seat + 4);
                if (detail::IsFiniteFloat(&y) && detail::IsFiniteFloat(&z) && std::abs(y) < 400 && std::abs(z) < 400) {
                    eye[1] += y;
                    eye[2] = z;
                }
            }
        }
        // Normalize tall drivers to a comfortable perceived cockpit height.
        // Movement::scale (+164, SetScale 80581720) includes lightning/mega
        // scaling. Keep the neutral seat reference, then scale it for this frame.
        const float characterScale=CharacterCockpitScale(eye[1]);
        renderUnits=g_state.units_per_meter*characterScale;
        playerScale=ReadPlayerScale(kart);
        eye[0] += g_state.offsets.right * renderUnits;
        // Keep controls ahead of the seated player even when a long face or
        // a leaned-forward riding animation puts its eye point over the bars.
        Mtx34 leftGrip{},rightGrip{},bodyPose{},inverseBody{},handlePose{};
        uint32_t handleVtable=0;
        if(ReadGuestMtx34(kart.body+0xa8,leftGrip) && ReadGuestMtx34(kart.body+0xd8,rightGrip)) {
            detail::Vec3 center{(leftGrip[3]+rightGrip[3])*0.5f,(leftGrip[7]+rightGrip[7])*0.5f,
                (leftGrip[11]+rightGrip[11])*0.5f};
            const bool bike=Memory::TryRead32(kart.body+0x244,handleVtable) && handleVtable==0x808b5314u;
            bool valid=true;
            if(bike) {
                valid=ReadGuestMtx34(kart.body+0x1c,bodyPose) && InvertMtx(bodyPose,inverseBody) &&
                    ReadGuestMtx34(kart.body+0x254,handlePose);
                if(valid) {
                    auto localHandle=ComposeMtx(inverseBody,handlePose);
                    for(int row=0;row<3;++row) localHandle[row*4+3]/=playerScale[row];
                    center=detail::TransformPoint(localHandle,center.x,center.y,center.z);
                }
            }
            if(valid && detail::IsFiniteFloat(&center.z) && !g_state.cockpit_forward)
                g_state.cockpit_forward=EyeBehindControls(eye[2],center.z,renderUnits,std::abs(leftGrip[3]-rightGrip[3])*0.5f);
        }
        if(g_state.cockpit_forward) eye[2]=*g_state.cockpit_forward;
        // Existing configs used 1.1/1.2 as their defaults. Those now mean zero
        // trim around the measured seat rather than 1.2m ahead of the kart.
        if (head_found) eye[1] += (g_state.offsets.up - 1.1f) * renderUnits;
        eye[2] += (g_state.offsets.forward - 1.2f) * renderUnits;
        for(int axis=0;axis<3;++axis) eye[axis]*=playerScale[axis];
        renderUnits*=playerScale[1];
        if (!ComputeFirstPersonAnchor(view_from_world, kart_from_local, eye[0], eye[1], eye[2],
                                      true, anchor, true))
            failed_step = "driver eye anchor math";
    }

    if (failed_step == nullptr) {
        g_state.anchor = {anchor, true, guest_frame_index, g_state.mode == CameraMode::Far
            ? RuntimeConfigFile::VrDioramaUnitsPerMeter() : renderUnits};
        g_state.anchor.vehicle_identity=kart.body;
        if (g_state.mode == CameraMode::FirstPerson) {
            uint32_t handleVtable=0;
            // BodyBike constructs BikeHandle at +238; its vtable is written
            // by 8056D858. Quacker inherits the same handle part.
            g_state.anchor.bike=Memory::TryRead32(kart.body+0x238+0xc,handleVtable) &&
                handleVtable==0x808b5314u;
            // Body::vf_0x58 (8056C500) builds the mirrored hand grip frames
            // at A8/D8 from KartDriverDispParam+8 via 80592BF8. The visual
            // animated body matrix (+1C) must not drive the interaction frame:
            // damage and tricks rotate the chassis independently of the seat.
            Mtx34 body{}, left{}, right{}, seatFromBody{};
            if (ReadGuestMtx34(kart.body+0x1c,body) &&
                ReadGuestMtx34(kart.body+0xa8,left) && ReadGuestMtx34(kart.body+0xd8,right)) {
                const auto stableBody=ScaleModelBasis(kart_from_local,playerScale);
                const auto toSeat=[&](float x,float y,float z) {
                    const auto world=detail::TransformPoint(stableBody,x,y,z);
                    const auto view=detail::TransformPoint(view_from_world,world.x,world.y,world.z);
                    return detail::TransformPoint(anchor,view.x,view.y,view.z);
                };
                const auto origin=toSeat(0,0,0);
                for (int col=0;col<3;++col) {
                    const auto axis=toSeat(col==0,col==1,col==2);
                    seatFromBody[col]=axis.x-origin.x;
                    seatFromBody[4+col]=axis.y-origin.y;
                    seatFromBody[8+col]=axis.z-origin.z;
                }
                seatFromBody[3]=origin.x; seatFromBody[7]=origin.y; seatFromBody[11]=origin.z;
                g_state.anchor.native_wheel=ComputeNativeWheelGeometry(seatFromBody,
                    {left[3],left[7],left[11]},{right[3],right[7],right[11]},renderUnits);
                if(g_state.anchor.bike) {
                    // BodyBike::vf_0x60 (8056DA0C) transforms the authored hand
                    // frames by BikeHandle+1C, NOT Body+1C as on karts.
                    Mtx34 handle{},seatFromHandle{};
                    g_state.anchor.native_wheel={};
                    if(ReadGuestMtx34(kart.body+0x238+0x1c,handle)) {
                        Mtx34 inverseBody{},inverseHandle{};
                        if(InvertMtx(body,inverseBody) && InvertMtx(handle,inverseHandle)) {
                            // Render and interaction share the same level handle
                            // pose. The motorcycle can bank without moving the
                            // bars away from the player's real hands.
                            const auto scaledStableBody=ScaleModelBasis(kart_from_local,playerScale);
                            const auto stableHandle=ScaleModelBasis(ComposeMtx(ComposeMtx(kart_from_local,inverseBody),handle),playerScale);
                            const auto renderedHandle=ScaleModelBasis(handle,playerScale);
                            InvertMtx(renderedHandle,inverseHandle);
                            if(RuntimeConfigFile::VrNativeSteeringWheel()) {
                                const auto correction=ComposeMtx(inverseHandle,stableHandle);
                                g_state.anchor.native_mesh_prepared=PublishNativeWheelMesh(kart,ComposeMtx(view_from_world,renderedHandle),left,right,
                                    kart.body+0x238,&correction);
                            }
                            handle=stableHandle;
                            seatFromBody=ComposeMtx(ComposeMtx(anchor,view_from_world),scaledStableBody);
                        }
                        Mtx34 viewHandle{};
                        for(int row=0;row<3;++row) for(int col=0;col<4;++col) {
                            viewHandle[row*4+col]=col==3?view_from_world[row*4+3]:0;
                            for(int k=0;k<3;++k) viewHandle[row*4+col]+=view_from_world[row*4+k]*handle[k*4+col];
                        }
                        for(int row=0;row<3;++row) for(int col=0;col<4;++col) {
                            seatFromHandle[row*4+col]=col==3?anchor[row*4+3]:0;
                            for(int k=0;k<3;++k) seatFromHandle[row*4+col]+=anchor[row*4+k]*viewHandle[k*4+col];
                        }
                        g_state.anchor.native_wheel=ComputeNativeHandlebarGeometry(seatFromHandle,seatFromBody,
                            {left[3],left[7],left[11]},{right[3],right[7],right[11]},renderUnits);
                    }
                }
                if(!g_state.anchor.bike && g_state.anchor.native_wheel.valid && RuntimeConfigFile::VrNativeSteeringWheel()) {
                    const auto renderedBody=ScaleModelBasis(body,playerScale);
                    Mtx34 inverseRendered{};
                    if(InvertMtx(renderedBody,inverseRendered)) {
                        const auto correction=ComposeMtx(inverseRendered,stableBody);
                        g_state.anchor.native_mesh_prepared=PublishNativeWheelMesh(kart,
                            ComposeMtx(view_from_world,renderedBody),left,right,0,&correction);
                    }
                }
            }
        }
        g_state.hold_frames = kHoldFrames;
        g_state.ever_valid_this_race = true;
        const auto policy = MkwVRPolicyGetSnapshot();
        if (g_state.mode == CameraMode::FirstPerson && policy.session_active && policy.scene.local_player_count == 1)
            HideDriver(LocalDriver(kart));
        LogAnchorLocked(guest_frame_index, anchor, view_from_world, kart, kart_from_local);
        return;
    }

    if (g_state.hold_frames > 0) {
        --g_state.hold_frames;
        g_state.anchor.guest_frame_index = guest_frame_index;
        return;
    }
    if (!g_state.ever_valid_this_race && !g_state.failure_logged) {
        // Once per race, naming the exact link that broke: every address below
        // is a PAL RMCP01 constant, so this is what says which one to revisit.
        g_state.failure_logged = true;
        RT_LOG(RT_TAG_RUNTIME)
            << "[mkw-vr] first-person camera is enabled but could not resolve the "
            << failed_step << "; staying on the game's own camera (camera=0x" << std::hex
            << race_camera_address << ", manager=0x" << kart.manager << ", players=0x"
            << kart.players << ", kart=0x" << kart.proxy << ", accessor=0x" << kart.accessor
            << ", body=0x" << kart.body << ", physics=0x" << kart.physics << std::dec << ")"
            << std::endl;
    }
    g_state.anchor = {};
}

FirstPersonAnchor MkwVRFirstPersonGetAnchor() noexcept {
    std::lock_guard lock(g_mutex);
    return g_state.anchor;
}

void MkwVRFirstPersonRestoreDriver() noexcept {
    aurora_clear_native_wheel_vertices();
    for (auto& model : g_hidden_models) {
        if (model) SetDriverDraw(model, true);
        model = 0;
    }
}

} // namespace mkw::vr
