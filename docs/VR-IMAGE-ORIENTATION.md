# Automatic GPU output orientation

At graphics-device initialization the application renders an invisible asymmetric
16x8 calibration image into an offscreen texture. It reads the actual GPU pixels
and checks their top/bottom and left/right order. It then checks the production
copy shader and chooses the output transform that restores the known order.

No orientation setting or headset recentering is needed. The old experimental
`[vr] race_image_orientation` key is ignored. Nothing is inferred from a screenshot
of the race, the player's head pose, the name of a headset or GPU, or Windows GPU
scheduling settings.

The result is logged as `VR GPU orientation probe`. A timeout, unsupported format,
or unrecognized pixel pattern leaves the orientation unchanged. The bounded
readback happens only during device initialization, never every frame. It is
repeated when the graphics device is recreated. A normal native-resolution race
keeps its direct copy path; adaptive upscaling uses the verified copy transform.
If the raster probe is inverted, the race output is corrected after the world,
hands and HUD are rendered. Both eyes use the same device calibration. Room-anchored
menus and tutorials are unchanged.

## Scope

This detects orientation in the application's GPU raster/copy path. It cannot
observe an inversion introduced afterwards by a streaming encoder, compositor or
headset display. Nor does an offscreen probe diagnose a bad game-camera matrix.
The users' reported race-only inversion has not been reproduced locally; its
association with Virtual Desktop or hardware GPU scheduling is unconfirmed.
If it persists with an `upright` probe result, collect the session log, a headset
capture and the affected runtime/GPU versions instead of adding a blanket flip.

## Validation

`output_orientation_gpu_test` reads the production WGSL and checks asymmetric
pixels through D3D12 rendering/readback at native and doubled sizes. It also runs
the production automatic probe with each possible default copy orientation
injected and verifies that the upright pipeline is selected. These GPU tests do
not reproduce a remote user's compositor/streaming environment.
