# Wheel Wizard VR integration

The portable release uses Wheel Wizard revision
`86618e7367df935d78401583136e492c6f00fa27` with the changes in
[`wheelwizard-vr.patch`](wheelwizard-vr.patch).

The patch integrates the VR installation with Wheel Wizard's recomp services,
including Retro Rewind updates, enabled-mod preparation, product validation and
repair, and Retro-WFC payload management. The base-game selector remains available.
Wheel Wizard settings stay beside the executable; runtime settings, Miis and
Retro Rewind saves resolve to the same paths the VR game uses.

To reproduce the bundled launcher:

```powershell
git clone https://github.com/TeamWheelWizard/WheelWizard.git
cd WheelWizard
git checkout 86618e7367df935d78401583136e492c6f00fa27
git apply ..\wheelwizard-vr.patch
dotnet publish WheelWizard/WheelWizard.csproj -c Release -r win-x64 --self-contained true `
  -p:PublishSingleFile=true -p:IncludeNativeLibrariesForSelfExtract=true `
  -p:EnableCompressionInSingleFile=true
```

Wheel Wizard is licensed under GPL-3.0. Its unmodified license is included in the
portable archive.

`Launcher/Build-Portable.ps1` now performs the pinned checkout, patch validation, self-contained
launcher build, ROM-free bundle checks and manifest/checksum generation. Use a clean tagged Git
checkout for releases; `-AllowDirty` is reserved for local development builds. The package workflow
publishes only the portable archive and checksum when a tag is pushed.

Retro Rewind's Update action uses the full upstream update-and-reconcile workflow:
asset-only changes do not force a rebuild, while changed compile inputs go through
the VR setup's product repair. The setup release resolver and launcher-update
notification use `heurazy/mario-kart-wii-VR-port`. A full runtime upgrade can still
require the original PAL disc image configured in Settings.

Installed and portable layouts are supported, including a pack beside `Install`,
external packs, and rebasing an internal pack after moving the portable directory.
The local frontend stays in recomp mode; incompatible Dolphin video controls and
the upstream recursive uninstaller cannot alter the shared VR installation.

See [feature coverage and limitations](../docs/WHEELWIZARD-VR.md). Run
`WheelWizard.exe --vr-diagnostics` for a read-only JSON report of the resolved paths.
Integration regression tests are included in the patch under
`WheelWizard.Test/Features/Recomp/LocalVrPathsTests.cs`.
