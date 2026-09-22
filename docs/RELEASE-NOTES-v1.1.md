# Mario Kart Wii VR Port v1.1

## 1. Multiplayer first-person camera

First-person mode now follows the local racer selected by Mario Kart Wii's active race camera.
Online races no longer attach the cockpit, hidden driver, world scale, or steering wheel to another
player simply because that player occupies slot zero.

## New WheelWizard integration

- WheelWizard now acts as the full local VR launcher for Mario Kart Wii VR and Retro Rewind VR.
- Retro Rewind updates, patches, mods, saves, friends, Miis, repair, and launch diagnostics use the
  active installed or portable VR paths.
- Portable installations keep their data beside the bundle and survive launcher/runtime updates.
- Launch failures now distinguish a missing headset or inactive SteamVR/OpenXR runtime from a damaged
  game build, avoiding unnecessary repair prompts.
- License names can be changed even when a valid license has no matching Mii in the NAND. Friend code,
  ratings, statistics, Mii identity, and other save data remain intact.

## VR stability and compatibility fixes

- Spatial menus now wait for stable headset tracking at startup, anchor from the rendered eye poses,
  and reset their origin after tracking loss or a SteamVR focus/reference-space change. This addresses
  misplaced startup menus that previously required a manual view reset.
- Continuous Wii Remote Bluetooth scanning is disabled by default, removing the recurring two-second
  freeze reported on some systems. It can be re-enabled under **VR settings > Driving**.
- OpenXR image submission now allows a ten-second loading grace period instead of disconnecting after
  250 ms during course transitions, shader compilation, or slow GPU work.
- The race image is calibrated on the GPU so upside-down output from affected acceleration paths is
  corrected automatically without changing menu orientation.
- SteamVR/Pimax frame handling tolerates transient hidden and unavailable frames without prematurely
  ending the OpenXR session.
- Pending GPU images retain safe ownership during timeout, cancellation, shutdown, and completion
  races, preventing unsafe releases and stale-frame failures.

## Driving and hardware

- Added broad SDL raw-joystick support for real steering wheels, separate or combined pedals, and
  configurable paddles/buttons.
- Right paddle drifts and left paddle uses items by default after assignment; brake/reverse, tricks,
  confirm, and pause can also be mapped.
- Optional light rumble follows game rumble events and is capped to avoid strong force-feedback torque.
- The visible cockpit wheel follows hardware steering, while VR controllers remain available for menus,
  camera switching, pause, and item aiming.

## Other fixes

- Added clearer diagnostics for VR launch, output orientation, frame pacing, and WheelWizard paths.
- Updated the README and bundled portable documentation for the actual VR port and v1.1 controls.

Both downloads contain the updated WheelWizard launcher. No ROM, Nintendo game assets, translated game
executable, Code.pul, or Retro-WFC payload is distributed. Select your own clean PAL RMCP01 disc image;
the installer performs translation and compilation locally.

**Online service notice (September 22):** Retro WFC staff reported a provider outage on September 21–22.
Online connections may fail while that incident continues. This release does not fix a server outage;
the local diagnostic run for error 84020 received no replies from the matchmaking UDP endpoint.
