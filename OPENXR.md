# Experimental OpenXR VR

WiiCompiled includes an opt-in OpenXR renderer. The Windows implementation uses D3D12 and submits
both eyes to the active OpenXR runtime on the same graphics device as Aurora. There is no CPU
texture readback and no second graphics device.

VR is still experimental. It falls back to the normal desktop mirror when the runtime, headset,
GPU, or graphics binding is unavailable unless `required = true` is selected.

## Requirements

- Windows 10 or 11, 64-bit.
- An active Windows OpenXR runtime and a connected compatible headset. Any runtime works; on a
  Quest that is usually Quest Link/Air Link (Meta), Virtual Desktop, or SteamVR. SteamVR is not
  required, and a missing runtime falls back to the desktop mirror rather than refusing to start.
- A D3D12-capable GPU and driver accepted by both OpenXR and Dawn.
- A build made with `MKW_ENABLE_OPENXR=ON`, which is enabled by default on Windows.

The supported OpenXR distribution target is Windows with D3D12. Linux and other platforms are not
supported release targets.

## Configuration

The generic runtime defaults to OpenXR disabled; the VR installer enables it. The configuration file is next to the installed game and can contain:

```toml
[vr]
enabled = false
required = false
# auto (default) uses whichever OpenXR runtime you have made active, which is
# what makes Quest Link, Air Link, Virtual Desktop and SteamVR all work without
# configuration. Force one with "meta", "virtualdesktop", "steamvr", or pin the
# system default with "system".
runtime = "auto"
render_scale = 1.0
world_units_per_meter = 500.0
hud_distance_meters = 2.0
hud_width_meters = 2.4
hud_virtual_screen = true
stop_at_display_copy = true
skip_copy_clears = true
first_person = false
native_steering_wheel = true
first_person_units_per_meter = 100.0
first_person_head_up_meters = 1.1
first_person_head_forward_meters = 1.2
first_person_head_right_meters = 0.0
```

Set `enabled = true`, close the game completely, and launch it again. The enable switch in the F10
panel also requires a restart. `required = false` is the safe default: a missing runtime, detached
headset, unsupported GPU, or graphics-binding failure returns to desktop mode.

`render_scale` multiplies the runtime-recommended eye dimensions. `world_units_per_meter` controls
headset translation in the game world. `hud_distance_meters` and `hud_width_meters` size the
head-locked virtual screen. `hud_virtual_screen` controls whether the race's 2D layer uses that
screen. `stop_at_display_copy` and `skip_copy_clears` are diagnostic switches for frame replay.
The first-person values control the camera described below and are also available in the F10 panel.

## Camera and HUD

On the first VR launch, a controller-pointer panel lets you choose the default camera before
the game starts. Aim the right controller and pull its trigger to select; a mouse also works.
Every race starts with that saved camera. During an immersive race, click the right
thumbstick to cycle through the original camera, the first-person cockpit, and the distant diorama
camera. The click is latched, so holding the stick advances only once. Menus do not consume camera
changes. Profiles without a right-stick click keep their normal camera.

The first single-player race requests Mario Kart's pause and shows a controller guide once
the game acknowledges the pause. Third person and diorama share a guide; first person has its
own guide, shown when you first use that mode. Select **Continue racing** to resume.
The illustrated controllers have button callouts and follow tracked hand movement within the
panel; they are native drawings, not BigWalk's Unity controller models. Sessions that cannot
pause do not display a blocking guide. In VR options, change **Default camera** or choose
**Show control tutorials again** to replay both guides.

Progress is saved in `[vr]`: `welcome_complete`, `default_camera` (0 original, 1 cockpit,
2 diorama), and `tutorial_completed` (bit 1 external cameras, bit 2 cockpit). Missing settings
start the introduction, including when upgrading an existing installation.

With `hud_virtual_screen = true`, the complete race HUD, including the circuit minimap and item
roulette, follows the left controller as a 30 cm panel in the original and diorama cameras. In the
first-person cockpit it is anchored in front of the seat, independently of controller tracking and
head turns. If tracking or focus is lost, the HUD returns to the normal virtual screen. Menus retain
their existing presentation.

The first-person view uses Player 1's evaluated seated eye position and hides only that driver's model.
If eye geometry cannot be evaluated, authored head/driver-seat parameters provide the
fallback. Position follows the kart simulation exactly, while impact rotations are held and blended
back for comfort. The diorama uses a much larger world scale and follows the kart's centre and
driving direction. The game's own transforms are not modified; Aurora composes the VR view and eye
transforms around them.

Cockpit mode presents the vehicle's wheel or motorcycle handlebar and tracked controller hands in
the seated frame. Squeeze either grip near the control to grab it. One or both hands can steer;
joining or releasing a hand preserves the steering target, and common two-arm movement is ignored.
The grab tolerates broad forward/back movement, centre crossings and brief tracking loss. Adaptive
smoothing damps tracking tremor while keeping fast steering responsive at 72, 90 and 120 Hz.
Releasing both grips returns steering to the left stick. Native steering is enabled by default and
animates the vehicle's original control; it can be disabled to use the procedural VR control. Hands use the scene
depth buffer, so the kart and track correctly occlude them. The renderer uses the Meta hand mesh
extension when available and articulated glove models as a fallback.

The seated calibration uses the evaluated eye position for each driver and vehicle. Tall characters
receive a comfortable world scale, while lightning and other temporary player scaling resize the
viewpoint, control position and grab radius together. Temporary scale cannot replace the neutral
calibration when the camera is changed.

## Controller compatibility

The runtime suggests bindings for standard OpenXR interaction profiles and silently ignores profiles
that the active runtime does not advertise.

| Profile family | Analog controls | Digital controls | Hand HUD |
| --- | --- | --- | --- |
| Meta/Oculus Touch, Touch Plus, Touch Pro, Quest 1/Rift S, Quest 2, Rift CV1 | Left/right thumbsticks, triggers, squeeze | A/B/X/Y, menu, stick click | Left grip pose |
| ByteDance PICO Neo3, PICO 4, PICO G3, PICO Ultra | Left/right thumbsticks, triggers, squeeze | A/B/X/Y or menu, stick click | Left grip pose |
| Valve Index | Left/right thumbsticks, triggers, squeeze | A/B, system, stick click | Left grip pose |
| Microsoft Mixed Reality / Samsung Odyssey | Left/right thumbsticks, trigger | Menu, squeeze, stick click | Left grip pose |
| HTC Vive / Vive Cosmos / Vive Focus 3 | Left/right trackpads, triggers | Menu, trigger, squeeze, trackpad click | Left grip pose |
| Khronos Simple Controller | None | Select and menu | Left grip pose |

Equivalent layouts use the same in-game actions. Vive-style trackpads replace thumbsticks. A
controller without analog inputs remains usable for menu navigation. The profile table is kept in
`runtime/include/vr/openxr_controller_profiles.h` and checked by
`mkw_openxr_controller_profiles_tests`.

### Quest-style actions

| Controller control | In-game action |
| --- | --- |
| Left stick | Steer and navigate menus; in cockpit it steers after both hands release the wheel |
| Right trigger | Accelerate |
| Left trigger, held | Brake, then reverse in every camera; overrides held acceleration and drift |
| A | Accelerate / confirm outside cockpit; hop / drift in cockpit; confirm in menus |
| B | Brake / cancel |
| Y | Use or hold an item |
| X | Trick / bike wheelie |
| Right stick directions | Directional tricks; vertical input starts or ends bike wheelies |
| Left and right squeeze | Grab the physical wheel in cockpit; right squeeze hops/drifts outside cockpit |
| Left menu button | Pause |
| Right stick click | Cycle VR cameras |
| X + Y | Open or close VR settings without passing either action to gameplay |

The OpenXR system button keeps its system function. Player 1 uses the VR action set while the
session is focused; the normal keyboard/gamepad path resumes when VR input is unavailable. Players
2–4 are unaffected. A radial 15% stick deadzone filters drift, short button taps survive frame-rate
differences, and a stale XR snapshot expires after 250 ms. F10 suppresses gameplay input until held
controls are released after the panel closes.

The game continues to display GameCube prompts. The Driving tab can swap item/trick between Y/X
and cockpit drift/brake between A/B. X+Y always opens settings, menu navigation remains A/B, and
the left trigger always brakes/reverses. Optional short haptic pulses indicate grabbing/releasing.
These additions are included in v1.0.

## Version 1.0 settings

Under SteamVR, X still triggers tricks immediately. Holding X for 0.65 seconds sends
one Mario Kart pause press; release X before pausing/resuming again. The initial press
can still perform a trick before the hold opens pause. X+Y continues to open VR settings
and cancels the long-X pause. The system Menu button is left to SteamVR so it no longer
also pauses Mario Kart. Other OpenXR runtimes retain their existing Menu binding.

- **Driving:** kart and bike rotation for full steering, acquisition depth, grab assistance,
  steering response and tracking-loss tolerance. Defaults remain 90 degrees for karts and 45
  for bikes. Changes apply immediately. The left stick retains forward/back item aiming while
  the wheel controls left/right steering. Grip ownership survives brief missing vehicle data;
  it does not transfer to a different vehicle.
  Kart wheels can rotate beyond full steering and through complete turns. The game input
  saturates at the configured steering angle, but extra hand rotation is retained so
  retracing the gesture returns to the same centre. Handlebars keep limited visual travel
  with the same centre preservation. When two held hands are too close to define an angle,
  steering holds steady until they separate; releasing both grips recentres the control.
  The kart wheel and its grab reference use the stabilised cockpit frame during body
  spins and airborne tricks. Only the wheel vertices are compensated; the chassis keeps
  its original animation. Physical wheel rotation remains independent of that compensation.
  Multi-joint kart draws are supported when every modified wheel position uses the local
  body matrix. Other joints and opponents sharing the asset retain their original vertices;
  ambiguous ownership falls back to the original draw instead of deforming another part.
- **Graphics:** preferred refresh rate on the next launch, applied only if the runtime exposes
  it. Runtime default is respected when no preference is set. The existing resolution selector
  still requires restarting. Experimental adaptive resolution is off by default: it renders
  races at 70–100% of the configured eye dimensions and upscales to the unchanged XR swapchain.
  It reacts slowly to new-image rate; it is not a GPU-time measurement or a fix for CPU limits.
- **Display:** forward HUD width/distance apply immediately. First-person HUD remains anchored
  in front of the seat. Camera trim is separate from world scale; Reset seat position restores
  the neutral adjustments.
- **Diagnostics:** display FPS, new-image FPS, runtime frequency, eye dimensions, p95/p99
  presentation intervals, native grip/mesh preparation and last-frame replacement draw matches.
  Zero mesh matches can also mean the wheel is outside the view. Export writes
  `VR-diagnostics.txt` beside `Config.toml`, without a ROM or personal paths.

Configuration writes use a temporary file and replacement; a failed save preserves the old file.
Existing saved choices, including disabled native steering, remain respected.

Recognized controller profiles are not a list of physically validated devices. Quest-style PC VR
has been exercised during development; no exhaustive headset/vehicle compatibility matrix is
claimed. Automated tests cover input/math and synthetic stereo GPU occlusion. Real race tests are
still required for every supported vehicle, lightning/camera transition, and headset/runtime pair.

## First-person camera tuning

`first_person_head_up_meters`, `first_person_head_forward_meters`, and
`first_person_head_right_meters` tune the driver's head anchor. The diorama position uses the kart's
centre and heading, not the chase-camera origin. These values are live in the F10 panel and saved to
the configuration file.

## Backend status

| Backend | Status |
| --- | --- |
| Windows D3D12 | Implemented: same-adapter, same-device asynchronous OpenXR submission. |
| Android / Quest Vulkan | Session and swapchain code implemented; blocked on the Dawn native-handle bridge. See [QUEST.md](QUEST.md). |
| Linux / other platforms | Not supported by the current distribution. |

## Known limitations

- Only the PAL `RMCP01` translation has the race instrumentation required for immersive rendering.
- Wii Remote support is a separate input path and has its own documented limitations.
- Standalone Meta Quest (Android) packaging exists but does not yet run on a
  headset: the platform, OpenXR and APK layers are implemented and cross-compile
  for arm64-v8a, while Dawn-on-Android, Aurora's SDL3 windowing and the
  Vulkan/OpenXR device bridge are unresolved. See [QUEST.md](QUEST.md).
- Apple visionOS packaging is not implemented.
- Scene-specific comfort options, culling fixes, replay/spectator classification, and advanced VR
  remapping are future work.
- Full per-eye EFB post-processing is not implemented. Effects which sample the mono EFB remain
  excluded from immersive replay rather than being blindly re-enabled.
- Native kart wheel extraction remains geometric; unusual meshes and normal deformation require
  additional model-specific work. Preparation failure uses the procedural control.
- The desktop window remains available as a mirror and fallback.

OpenXR diagnostics are written to the normal run log under
`%LOCALAPPDATA%\\WiiCompiled\\Logs`. Search for `OpenXR` when reporting startup or submission
failures.

## Local validation

The runtime and generated PAL `RMCP01` game build compile and link with the pinned LLVM-MinGW
toolchain. The VR camera, input, delivery, policy, and controller-profile CTest targets pass. A
desktop smoke test initializes D3D12, loads game scenes, stays responsive, and exits normally.

A physical headset race is still required to verify controller tracking recovery, camera placement,
and minimap legibility on the target hardware.
