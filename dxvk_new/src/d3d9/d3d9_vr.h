#pragma once

#include <d3d9.h>

#define VK_USE_PLATFORM_WIN32_KHR 1
#include <vulkan/vulkan.h>
#undef VK_USE_PLATFORM_WIN32_KHR

class IDirect3DVR9;
class D3D9DeviceEx;
class SharedTextureHolder;
class VR;
inline IDirect3DVR9* g_D3DVR9;

struct D3D9_TEXTURE_VR_DESC {
  uint64_t         Image;
  VkDevice         Device;
  VkPhysicalDevice PhysicalDevice;
  VkInstance       Instance;
  VkQueue          Queue;
  uint32_t         QueueFamilyIndex;

  uint32_t         Width;
  uint32_t         Height;
  VkFormat         Format;
  uint32_t         SampleCount;

  // Appended for the native OpenXR backend.  Kept after the original
  // OpenVR-compatible prefix so SharedTextureHolder can memcpy the whole
  // descriptor without changing the historical field order.
  uint32_t         QueueIndex;
  VkImageLayout    Layout;
  VkImageUsageFlags Usage;

  // KMT export consumed by the x64 OpenXR helper.
  uint64_t         SharedHandle;
  uint32_t         SharedHandleType;
  uint32_t         SharedHandleValid;
};

// One source -> destination blit requested by the OpenXR backend.  The source
// is a DXVK-backed D3D9 texture; the destination is an OpenXR Vulkan
// swapchain image.  SrcU0..SrcV1 select a sub-rectangle of the source.
struct D3D9_VR_BLIT_DESC {
  uint64_t         SrcImage;
  uint32_t         SrcWidth;
  uint32_t         SrcHeight;
  VkFormat         SrcFormat;
  VkImageLayout    SrcLayout;
  VkImageUsageFlags SrcUsage;
  float            SrcU0;
  float            SrcV0;
  float            SrcU1;
  float            SrcV1;

  uint64_t         DstImage;
  uint32_t         DstWidth;
  uint32_t         DstHeight;
  VkFormat         DstFormat;
};

MIDL_INTERFACE("7e272b32-a49c-46c7-b1a4-ef52936bec87")
IDirect3DVR9 : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE GetVRDesc(IDirect3DSurface9* pSurface, D3D9_TEXTURE_VR_DESC* pDesc) = 0;
  virtual HRESULT STDMETHODCALLTYPE TransferSurface(IDirect3DSurface9* pSurface, BOOL waitResourceIdle) = 0;
  virtual HRESULT STDMETHODCALLTYPE LockDevice() = 0;
  virtual HRESULT STDMETHODCALLTYPE UnlockDevice() = 0;
  virtual HRESULT STDMETHODCALLTYPE LockSubmissionQueue() = 0;
  virtual HRESULT STDMETHODCALLTYPE LockPreparedSubmissionQueue() = 0;
  virtual HRESULT STDMETHODCALLTYPE UnlockSubmissionQueue() = 0;
  virtual HRESULT STDMETHODCALLTYPE DrawQueuedEyeSubmitOverlays(VR* vr) = 0;
  virtual HRESULT STDMETHODCALLTYPE WaitDeviceIdle() = 0;
  virtual HRESULT STDMETHODCALLTYPE GetBackBufferData(SharedTextureHolder* backBufferData) = 0;
  virtual HRESULT STDMETHODCALLTYPE BeginSourceFrameOwnership() = 0;
  virtual HRESULT STDMETHODCALLTYPE EndSourceFrameOwnership() = 0;
  // Diagnostics need the live D3D9 device even when -nohmd skips all VR
  // render-target allocation.  Keep this at the end of the private interface
  // so existing method indices remain stable.
  virtual HRESULT STDMETHODCALLTYPE GetD3DDevice(IDirect3DDevice9** ppDevice) = 0;

  // Native OpenXR support.  These are appended so existing method indices
  // remain stable for the rest of the fork.
  virtual HRESULT STDMETHODCALLTYPE GetVulkanHandles(
      uint64_t* pInstance,
      uint64_t* pPhysicalDevice,
      uint64_t* pDevice,
      uint64_t* pQueue,
      uint32_t* pQueueFamilyIndex,
      uint32_t* pQueueIndex) = 0;
  virtual HRESULT STDMETHODCALLTYPE BlitImages(
      const D3D9_VR_BLIT_DESC* pDescs,
      uint32_t unDescCount) = 0;
};

#ifdef _MSC_VER
struct __declspec(uuid("7e272b32-a49c-46c7-b1a4-ef52936bec87")) IDirect3DVR9;
#else
__CRT_UUID_DECL(IDirect3DVR9, 0x7e272b32, 0xa49c, 0x46c7, 0xb1, 0xa4, 0xef, 0x52, 0x93, 0x6b, 0xec, 0x87);
#endif

HRESULT __stdcall Direct3DCreateVRImpl(IDirect3DDevice9* pDevice, IDirect3DVR9** pInterface);
