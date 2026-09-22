# Mario Kart Wii VR Port v1.1.1

This update fixes recurring controller-search hitches and incorrect startup placement of the spatial VR menus.

- A v1.0 or v1.1 configuration could keep Bluetooth Wii Remote rescanning enabled after an upgrade. That legacy preference is now ignored. Automatic Wii Remote searching is off until explicitly enabled with the checkbox at the top of **VR settings (F10)** or in the desktop Wii Remotes menu. Existing connected Wii Remotes remain usable. The new opt-in is stored as `[controller] wii_continuous_scan_opt_in = true`.
- Physical steering wheels and pedals are no longer enumerated every second during a race. Discovery runs when wheel settings open and when SDL reports a device change while a hardware wheel is enabled. A **Refresh USB devices** button is available in wheel settings.
- VR menus now retain their spatial anchor through temporary tracking gaps and use the rendered eye poses and application-space origin when positioning the startup screen. This addresses menus appearing inverted, too high, or too far away until the headset view is reset.

Download `WiiCompiled-Setup.exe` for an installed setup, or `WiiCompiled-VR-Portable-v1.1.1.zip` for a self-contained portable launcher. Both include WheelWizard. They contain no ROM, game assets, or translated game executable; select your own clean PAL RMCP01 disc image to compile the game locally.

The fixes passed the native Windows build and 13 runtime regression tests. Performance and headset placement still depend on the user's hardware and OpenXR runtime, so feedback from affected setups is welcome.
