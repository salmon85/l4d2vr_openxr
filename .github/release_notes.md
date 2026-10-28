## L4D2VR OpenXR build V.0.8.3

### Highlights

- Native OpenXR scene rendering with menu, pause, loading-screen, wrist HUD, and gun ammo HUD overlays.
- Oculus Touch controls restored to the original manifest: right stick click = Flashlight, right grip = hold Crouch, left grip = Reload.
- Item switching only fires on a vertically dominant right-stick deflection, so rotating no longer flips weapons.
- Chainsaw fuel is shown on the right hand HUD.
- The flat desktop HUD is hidden while the wrist/ammo HUDs are active; it no longer lingers after closing the pause menu.
- Pause/menu navigation accepts A, B/Y, and the left stick from the controller.

### Known limitations

- **There is no visible laser pointer in the menus.** Menus still work with the controller stick and A/B/Y, and with the invisible pointer hit area.
- **The in-headset VR config menu is disabled** because opening it crashes the game under Wine/WiVRn. Edit `vr/config.txt` on the desktop instead.
