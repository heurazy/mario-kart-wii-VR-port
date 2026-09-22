# VR stability and license fixes

## Wii Remote scanning

Continuous Bluetooth scanning now defaults to off. It used to cycle the SDL
Wii controller driver every two seconds while no Wii Remote was connected
(even faster during initial discovery), potentially interrupting rendering.
Already connected controllers remain usable.

Enable it in **VR settings > Driving > Continuously search for Wii Remotes**,
or the desktop Wii Remotes menu. The persisted setting is
`[controller] wii_continuous_scan = true`. An existing explicit preference is
respected. Quest, Index and other OpenXR controllers do not require this scan.

## OpenXR loading grace period

Both synchronous and asynchronous submission paths now allow ten seconds,
instead of 250 milliseconds, for a pending stereo image. This accommodates
course transitions and shader compilation. The asynchronous display loop
continues servicing the runtime while waiting.

The deadline is for a pending submission, not every interval without a new
guest frame. An unclaimed packet can still be safely canceled; an image
owned by the GPU cannot be released until completion. A genuinely stuck GPU
can still cause a safe desktop fallback after the grace period. Shutdown
polling remains independent of that period.

## Online cockpit

The cockpit, driver hiding, scale and wheel now resolve the racer from the
active RaceCamera instead of hardcoding racer zero. PAL RaceCamera::Init
(0x805A2034) reads the player byte at +0x9C and uses it to index
Kart::Manager's player array at +0x20; verified in the generated translation.
Invalid indices (including signed -1) do not select a different racer.
Split-screen remains outside the single-view immersive policy.

## WheelWizard license names

A valid RKPD license can be renamed without a matching NAND Mii. Its existing
UTF-16 name is used when the NAND lookup fails. Renaming updates that license's
name and save CRC while preserving Mii identity, friend code, rating and other
data. If a matching NAND Mii exists, its name is updated as before.
Empty license slots remain rejected, and editing while the VR game runs is
blocked. No Mii Channel or fabricated replacement Mii is required.

## Validation

Startup menu anchoring uses the same tracked eye poses as rendering, with a
400 ms settling interval (position within 40 cm, heading within 75 degrees).
Invalid or upside-down startup poses cannot establish the anchor. SteamVR can
report usable poses before setting both tracking-status bits, so valid stereo
poses are accepted after the settling interval.
Loss of tracking/focus, recenter and session restart clear it. An already
anchored menu remains stationary during normal head movement. Unanchored
cached images remain in view space instead of adopting a newer live anchor.
Regression cases cover a startup height jump, heading change, tracking
recovery, normal head movement, reset and invalid coordinates. Visual verification in a headset
is still required for the affected startup configuration.

Native regression tests cover slow image delivery through nine seconds,
ten-second timeout/cancellation, GPU ownership, completion races and prompt
shutdown. Configuration tests cover the disabled scan default and explicit
opt-in. WheelWizard tests rename synthetic saves with and without a NAND Mii,
verify CRC, and check every byte outside the name and CRC remains unchanged.

Headset-specific stutter/disconnection reports and online cockpit behavior
still require testing on the affected users' hardware/in an online race.
