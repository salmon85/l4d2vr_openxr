## L4D2VR OpenXR build V.0.8.4

### Fix

- The packaged `vr/config.sample` now uses the known-good runtime configuration from testing instead of the development source config.
- This restores the original hand-pose and two-handed aim defaults that the previous release changed, including:
  - left/right hand pose offsets and rotations
  - two-handed aim strength, smoothing, offhand offset, grip target and virtual-stock strength
  - native viewmodel hand pose offsets and glove curl defaults
  - first-person body and hands-only defaults
- HUD defaults remain: wrist HUD and gun-hand ammo HUD enabled, manual magazine reload disabled.

### Binaries

- `d3d9.dll` and `OpenXRHelper64.exe` are unchanged from V.0.8.3.

### Known limitations

- **There is no visible laser pointer in the menus.** Menus still work with the controller stick and A/B/Y, and with the invisible pointer hit area.
- **The in-headset VR config menu is disabled** because opening it crashes the game under Wine/WiVRn. Edit `vr/config.txt` on the desktop instead.
