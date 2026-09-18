# Wheel Wizard with the VR port

This integration connects the existing Wheel Wizard features to the VR installation.
It does not add an emulator to the native port.

| Feature | VR integration |
| --- | --- |
| Mario Kart Wii / Retro Rewind selector | Launch through the VR setup host; preserve installation locks and validation. |
| Retro Rewind install, update and reinstall | Use Wheel Wizard's distribution service, then reconcile the compiled game through the VR setup. A changed Code.pul requires recompilation. |
| Mods and GameBanana downloads | Existing browsing, installation, enable/disable and priority workflow. Enabled assets are prepared in the actual RetroRewind6/Patches directory before Retro Rewind starts. |
| Online play | Use the existing Retro-WFC runtime support and setup payload policy. An offline-only build is offered an online-capable repair when the payload service returns. Enter online mode from Retro Rewind in the headset. |
| Rooms, players and leaderboards | Keep the existing online browsers. These pages inspect server data; they do not teleport the running game into a selected room. |
| Friends, licenses and statistics | Read/write the active Retro Rewind save rather than an unrelated Dolphin save. Close the game before editing. |
| Miis | Use the VR NAND, including the configured NAND override. Restore the upstream Mii rendering resource setup prompt. |
| Runtime settings | Edit the active installed or portable Config.toml. D3D12 remains required for VR; use in-game VR settings for eye resolution and controllers. |
| Startup launch and protocol mod downloads | Retain Wheel Wizard's existing workflows with the VR frontend selected. |
| Updates | Query the VR repository for setup upgrades and launcher update notifications. |

## Using the local build

The published launcher is self-contained for Windows x64. Keep it and `vr-local.txt`
inside the installation's existing `WheelWizard` directory. The bundle can use
`Base/` and `RetroRewind/` directly or place those products under `Install/`.
A portable marker selects the matching `UserData`; an ordinary installation uses
`%LOCALAPPDATA%/WiiCompiled`. Existing configuration and save data are preserved.

Select **Retro Rewind VR** on the home page. **Update** installs the current pack
and repairs affected compiled products. Configure the original PAL image in
Settings when upgrading the complete runtime. Add mods in the Mods page, then launch
Retro Rewind to prepare the enabled assets. The original Mario Kart Wii selector
does not apply Pulsar/Retro Rewind asset patches.

For an installed layout where Wheel Wizard lives inside the runtime directory,
a complete runtime upgrade must replace the launcher itself. The launcher downloads
the setup and gives its path; close Wheel Wizard before running that setup.
Pack updates and targeted product repairs keep their normal integrated workflow.

## Limits and validation

Local developer executables must be deployed with matching product build identities.
`Launcher/Deploy-LocalVrHotfix.ps1` verifies matching game/Retro-WFC inputs, keeps
backups, publishes the local binaries and updates their recorded hashes. Always
run the installed setup's `--check-products` afterwards. Copying only an executable
causes a legitimate repair request. The integrity check remains enabled.

An unavailable headset is a launch/runtime error, not proof of a broken install.
Local launch failures inspect only a newly created session log and explain missing
SteamVR headsets explicitly, with a linkable filesystem log path in the message.

Arbitrary executable/Gecko patches, other custom distributions and Retro Rewind beta
builds need their own native translation and validation. They are not made compatible
by exposing a launcher button. The Dolphin Mii Channel, Dolphin controller/video
settings and Dolphin-specific features do not execute in this native VR runtime.
Wheel Wizard's Mii manager is available instead.

The integration has automated layout and existing recomp-contract tests and has been
compiled as a self-contained launcher. Actual online races, each third-party mod and
every Mii editing workflow still require end-to-end testing. Server availability and
matching game versions remain prerequisites for online play.

`WheelWizard.exe --vr-diagnostics` prints the installation, user data, pack, patches,
save directory, executable presence and update repository without opening the UI,
changing settings, downloading anything or launching the game.
