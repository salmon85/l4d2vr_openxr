# OpenXR SDK headers

`include/openxr` contains the Khronos OpenXR headers used by the native
OpenXR backend (`L4D2VR/vr_openxr.cpp`).

The game is a 32-bit process and loads the OpenXR loader dynamically at
runtime.  Install a **32-bit** `openxr_loader.dll` next to `left4dead2.exe`
(or in the Wine/Proton prefix) before launching the game.  The loader is not
vendored here.

The backend prefers `XR_KHR_vulkan_enable2` and falls back to
`XR_KHR_vulkan_enable`.  The runtime must support one of those Vulkan
graphics bindings; a D3D11-only OpenXR runtime cannot share DXVK's Vulkan
device.
