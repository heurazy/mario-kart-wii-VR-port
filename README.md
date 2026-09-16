# Mario Kart Wii VR Port

Mario Kart Wii VR Port is a native Windows VR port of Mario Kart Wii. It combines WiiCompiled's
static recompilation with a native OpenXR renderer and tracked-controller input. The game runs as
native x86-64 code; no Wii emulator, interpreter, JIT, or PowerPC CPU is used at runtime.

This repository distributes the VR port, its installer, and its build tools. It does not distribute
Nintendo code, game assets, a translated game executable, or a ROM. You must provide your own
clean PAL `RMCP01` disc image and compile the game locally.

[Download the latest release](https://github.com/heurazy/mario-kart-wii-VR-port/releases/latest)

## What the port adds

- Native OpenXR rendering through D3D12 with a desktop mirror and a safe desktop fallback.
- Three race cameras: the original game camera, a true first-person cockpit camera, and a distant
  diorama camera. The right-stick click cycles cameras during a race.
- A seated cockpit aligned to the driver's evaluated eye position. The driver is hidden in first
  person, and the view adapts to small, tall, and temporarily resized characters.
- Physical steering: grab the real kart wheel or motorcycle handlebar with either tracked hand.
  Native vehicle animation is enabled by default and can be disabled in VR settings.
- Two-hand steering with centre-crossing and brief tracking-loss tolerance, adaptive smoothing,
  full-turn hand continuity, and a left-stick fallback after both hands release the control.
- Tracked hands depth-tested against the kart and the track, with SteamVR controller models and
  per-controller button animations in the VR tutorials and menus.
- A left-hand race HUD with the circuit map and item panel. In first person, the HUD is anchored in
  front of the seat so it stays readable while the head moves.
- An anchored, spatial VR presentation for the camera chooser, VR settings, controller guides, and
  Mario Kart menus. The first launch offers a camera choice through a VR pointer.
- A first-race controller tutorial, plus a separate first-person tutorial. Both can be reset from
  VR settings.
- Configurable eye resolution, sharpness, refresh-rate preference, adaptive resolution, world scale,
  HUD placement, camera trim, steering response, grab assistance, deadzone, haptics, and diagnostics.
- Retro Rewind VR support through the integrated WheelWizard launcher.

## Requirements

- Windows 10 or 11, 64-bit.
- An OpenXR runtime and a connected headset. On a Quest that means Quest Link or Air Link (the
  Meta desktop app), Virtual Desktop, or SteamVR - whichever you already use. The game uses
  whatever runtime you have made active and no longer requires SteamVR specifically; set
  `[vr] runtime` in `Config.toml` to force a particular one.
- A D3D12-capable GPU and a driver accepted by SteamVR, OpenXR, and Dawn. GTX 1650 / RX 6400 /
  Arc A310 or better is a practical starting point.
- About 20 GB free while installing and compiling. The compiled game is about 5 GB, excluding
  optional Retro Rewind files.
- Your own clean, unmodified PAL `RMCP01` disc image. ISO, GCM, GCZ, CISO, WBFS, WIA, and RVZ are
  accepted. Other regions and modified executables are rejected.

## Installation

### Guided installer

1. Download `WiiCompiled-Setup.exe` from the [v1.0 release](https://github.com/heurazy/mario-kart-wii-VR-port/releases/tag/v1.0).
2. Start SteamVR, then run the installer.
3. Select your clean PAL `RMCP01` image and choose an installation folder.
4. Leave **Download and install Retro Rewind automatically** enabled if you want both games.
5. Select **Install**. The setup translates and compiles the game on this PC; the image never leaves
   the computer.

The installer installs WheelWizard in the `WheelWizard` subfolder, creates a **Mario Kart Wii VR
Launcher** shortcut on the desktop and in the Start Menu, and opens the launcher when installation
finishes. Select **Mario Kart Wii VR** or **Retro Rewind VR** from that launcher.

### Portable bundle

Download `WiiCompiled-VR-Portable-v1.0.zip`, extract the complete folder, and run
`WiiCompiled-VR-Setup.exe`. Keep **portable installation** enabled and keep the folder together.
The launcher is at `WheelWizard/WheelWizard.exe`; the `UserData` folder keeps configuration, NAND,
cache, and logs beside the portable installation. `Update-VR.cmd` updates the VR runtime while
preserving personal and compiled game data.

No release contains a ROM or a translated game executable. Compilation is intentionally performed
locally from the disc image you select.

## Running on a Meta Quest

There are two ways a Quest can run this port, and they are at very different
stages:

- **Tethered (works today).** Quest 2/3/Pro/3S connected to a Windows PC over
  Quest Link, Air Link or Virtual Desktop, running the Windows build described
  above. This is the supported path, and it needs no SteamVR: the game uses
  whichever OpenXR runtime you have active.
- **Standalone on the headset (not yet runnable).** A native Android/arm64 build
  with no PC. The platform, OpenXR and packaging layers are implemented and
  cross-compile, but it does not render on a device yet. See
  [QUEST.md](QUEST.md) for exactly what is done, what is blocked, and how to
  build it.

## Quest and OpenXR controls

The runtime uses standard OpenXR actions, so Quest 2, Quest 3, Touch Pro, Valve Index, PICO, Vive,
Windows Mixed Reality, Samsung Odyssey, and other advertised profiles use the same in-game actions.
Vive trackpads substitute for thumbsticks where necessary. The left menu/system button keeps its
headset function; on SteamVR it opens the SteamVR dashboard.

| Controller input | Original / diorama camera | First-person cockpit |
| --- | --- | --- |
| Left stick | Steer; navigate menus | Steer after releasing the wheel; navigate menus |
| Right trigger | Accelerate | Accelerate |
| Left trigger (hold) | Brake, then reverse | Brake, then reverse |
| A | Accelerate / confirm | Hop and drift / confirm |
| B | Brake / cancel | Brake / cancel |
| Y | Use or hold item | Use or hold item |
| X | Trick / motorcycle wheelie | Trick / motorcycle wheelie |
| Right stick directions | Directional tricks and wheelies | Directional tricks and wheelies |
| Left or right grip | Right grip hops/drifts | Grab the wheel or handlebar |
| Right stick click | Cycle the three VR cameras | Cycle the three VR cameras |
| Left menu/system button | Pause outside SteamVR | Pause outside SteamVR |
| X + Y | Open or close VR settings | Open or close VR settings |

On SteamVR, tap X for a trick. Hold X for about 0.65 seconds to send one Mario Kart pause press;
the SteamVR system/menu button remains available for the dashboard. X + Y always opens VR settings
without forwarding either button to the game. The left trigger always brakes and reverses, including
when the wheel is held.

In the cockpit, squeeze a grip near the wheel or handlebar to grab it. One or both hands can steer;
joining or releasing a hand preserves the current steering target. Releasing both grips returns
steering to the left stick. Open **VR settings > Driving** to change native wheel animation,
acquisition range, steering response, tracking-loss tolerance, and grab assistance.

## Cameras, HUD, and comfort

The first-launch VR panel asks for the default camera with a pointer and trigger. The choice is
saved and used at the beginning of every race. Races begin in the selected camera; the original
game camera is used for the short race-start launch animation and returns to the selected camera
when the animation ends or is skipped.

The original and diorama cameras show the race HUD, minimap, and item roulette on a panel that can
follow the left hand. First person anchors the same HUD in front of the seat. **VR settings >
Display** controls panel distance and width; **Camera** controls seat trim and world scale.

The physical wheel and camera are stabilised during impacts, spins, airborne tricks, and Lightning
scale changes. This keeps the view and hand controls attached to the kart without making the world
rotate with a damage animation. **Show control tutorials again** resets the first-race and
first-person guides.

## VR settings

Open VR settings with **X + Y** in the headset or **F10** on the desktop mirror. The menu includes:

- **Graphics:** per-eye resolution, sharp rendering, preferred headset refresh rate, FPS display,
  adaptive resolution, and menu shader quality (`Off`, `Low`, `Balanced`, `High`).
- **Camera and Display:** default camera, first-person head offsets, world scale, HUD width and
  distance, and seat reset.
- **Driving:** native steering-wheel animation, kart and motorcycle steering range, acquisition
  depth, grab assistance, smoothing, tracking-loss tolerance, and haptics.
- **Input:** controller profile diagnostics and stick deadzone calibration.
- **Tutorials and Diagnostics:** replay guides and export `VR-diagnostics.txt` beside `Config.toml`.

Changes are saved to `Config.toml`. Resolution and some refresh-rate changes take effect after a
restart; camera, HUD, steering, shader quality, and diagnostic options apply immediately.

## WheelWizard and Retro Rewind

WheelWizard is bundled as the launch and mod-selection front end. It has two local VR entries:

- **Mario Kart Wii VR**, using the base compiled game;
- **Retro Rewind VR**, using the separate Retro Rewind static profile.

The upstream WheelWizard updater is disabled for these local VR bundles so it cannot replace the
VR-specific launcher integration. Retro Rewind is optional during setup and requires its own local
compilation from the same clean PAL image.

## Troubleshooting

- **The headset stays on the desktop mirror:** start your runtime first (the Meta desktop app for
  Quest Link/Air Link, the Virtual Desktop streamer, or SteamVR), connect the headset, and restart
  the game. `required = false` falls back to desktop mode when OpenXR cannot create a session. The
  run log names the runtime that was selected; `[vr] runtime = "meta"`, `"virtualdesktop"`,
  `"steamvr"` or `"system"` forces the choice.
- **The game is not visible in WheelWizard:** run the copied setup from the installation folder,
  select the clean PAL `RMCP01` image, and let the local compilation finish.
- **The wheel is difficult to grab:** use **VR settings > Driving** to increase acquisition depth
  and grab assistance, then reset the seat position. Both grips can be used independently.
- **Tracking or performance problems:** lower eye resolution or menu shader quality, disable
  adaptive resolution if frame pacing is unstable, and export `VR-diagnostics.txt`.
- **Logs:** the runtime writes OpenXR and compilation diagnostics under
  `%LOCALAPPDATA%\\WiiCompiled\\Logs` (or `UserData\\Logs` in a portable installation).

## Building from source

Building requires .NET 8, CMake, Ninja, LLVM-MinGW, and a clean PAL `RMCP01` image. The supported
release target is Windows/D3D12 with OpenXR enabled. The main build scripts are:

```powershell
dotnet build translator/Translator.sln -c Release
powershell -ExecutionPolicy Bypass -File Launcher/Build-Installer.ps1
powershell -ExecutionPolicy Bypass -File Launcher/Build-Portable.ps1 `
  -SetupExecutable Launcher/dist/WiiCompiled-Setup.exe -Version 1.0
```

The build boundary deliberately excludes translated game code and game data from Git and releases.
See [OPENXR.md](OPENXR.md) for the implementation details, configuration keys, controller profiles,
and validation notes, and [QUEST.md](QUEST.md) for the standalone Meta Quest
(Android) target and `Launcher/Build-Quest.sh`.

## Credits
- **[Wiicompiled VR](https://github.com/iChris4/Wiicompiled_VR)** by Ichris4, all the openxr render system was taken from his project 
- **[WheelWizard](https://github.com/TeamWheelWizard/WheelWizard)** by Team WheelWizard, integrated
  as the local launcher and Retro Rewind front end.
- **[BigWalkVRInstaller](https://github.com/CircuitLord/BigWalkVRInstaller)** by CircuitLord, whose
  controller tutorial presentation inspired the attached callouts and pointer flow.
- **[@XorDev](https://x.com/XorDev)** for the **Dielectric** shader used as the animated spatial
  background around the VR menus. This port adapts the shader from the
  [FragCoord.xyz reference](https://t.co/kdebpbDcaQ) for stereo, translated rendering and exposes
  quality levels in VR settings. See the [attribution note](licenses/Dielectric-menu-background.md).
- **[aurora](https://github.com/encounter/aurora)** for the native GameCube/Wii graphics and windowing
  layer used by the port.
- **[OpenXR](https://www.khronos.org/openxr/)**, **Dawn**, **Dolphin Emulator**, **Pulsar**,
  **[AnimalCrossing-VR-MR-Standalone](https://github.com/heurazy/AnimalCrossing-VR-MR-Standalone)**,
  and **[Cyberpunk VR port](https://github.com/dariulone/cyberpunk-vr-port)** for APIs, references,
  and implementation ideas used during development.

Bundled component licenses are listed in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md). The
controller tutorial notice is in [licenses/BigWalkVR-MIT.txt](licenses/BigWalkVR-MIT.txt).

## License and legal notice

The WiiCompiled source in this repository is licensed under the
[GNU General Public License v3](LICENSE). Mario Kart Wii is a trademark of Nintendo. This project
is not affiliated with or endorsed by Nintendo and does not contain Nintendo intellectual property.
Dump and use your own game disc according to the laws that apply where you live.
