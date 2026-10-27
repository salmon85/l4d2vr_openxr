#include "../dxvk/dxvk_include.h"

#include "d3d9_vr.h"

#include "d3d9_include.h"
#include "d3d9_surface.h"

#include "d3d9_device.h"

#include "L4D2VR/game.h"
#include "L4D2VR/vr.h"

#include <cstring>

namespace dxvk {

  class D3D9VR final : public ComObjectClamp<IDirect3DVR9> {

  public:

    D3D9VR(IDirect3DDevice9* pDevice)
      : m_device(static_cast<D3D9DeviceEx*>(pDevice)) {
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(
            REFIID riid,
            void** ppvObject) {
      if (ppvObject == nullptr)
        return E_POINTER;

      *ppvObject = nullptr;

      if (riid == __uuidof(IUnknown)
       || riid == __uuidof(IDirect3DVR9)) {
        *ppvObject = ref(this);
        return S_OK;
      }

      Logger::warn("D3D9VR::QueryInterface: Unknown interface query");
      Logger::warn(str::format(riid));
      return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE GetVRDesc(
            IDirect3DSurface9*   pSurface,
            D3D9_TEXTURE_VR_DESC* pDesc) {
      if (unlikely(pSurface == nullptr || pDesc == nullptr))
        return D3DERR_INVALIDCALL;

      D3D9Surface* surface = static_cast<D3D9Surface*>(pSurface);

      const auto* tex = surface->GetCommonTexture();

      const auto& desc = tex->Desc();
      const auto& image = tex->GetImage();
      if (unlikely(image == nullptr))
        return D3DERR_INVALIDCALL;

      const auto& device = tex->Device()->GetDXVKDevice();

      // I don't know why the image randomly is a uint64_t in OpenVR.
      pDesc->Image = uint64_t(image->handle());
      pDesc->Device = device->handle();
      pDesc->PhysicalDevice = device->adapter()->handle();
      pDesc->Instance = device->instance()->handle();
      pDesc->Queue = device->queues().graphics.queueHandle;
      pDesc->QueueFamilyIndex = device->queues().graphics.queueFamily;
      pDesc->QueueIndex = device->queues().graphics.queueIndex;

      pDesc->Width = desc->Width;
      pDesc->Height = desc->Height;
      pDesc->Format = tex->GetFormatMapping().FormatColor;
      pDesc->SampleCount = uint32_t(image->info().sampleCount);
      pDesc->Layout = image->info().layout;
      pDesc->Usage = image->info().usage;

      HANDLE sharedHandle = image->sharedHandle();
      pDesc->SharedHandle = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(sharedHandle));
      pDesc->SharedHandleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT;
      pDesc->SharedHandleValid = (sharedHandle != nullptr && sharedHandle != INVALID_HANDLE_VALUE) ? 1u : 0u;

      return D3D_OK;
    }

    HRESULT STDMETHODCALLTYPE TransferSurface(
            IDirect3DSurface9* pSurface,
            BOOL               waitResourceIdle) {
      if (unlikely(pSurface == nullptr))
        return D3DERR_INVALIDCALL;

      D3D9DeviceLock lock = m_device->LockDeviceExclusive();

      auto* tex = static_cast<D3D9Surface*>(pSurface)->GetCommonTexture();
      const auto& image = tex->GetImage();

      VR* vr = (g_Game != nullptr) ? g_Game->m_VR : nullptr;
      if (vr && vr->m_ReShadeVRCompat) {
        IDirect3DSurface9* rightEyeSurface = vr->m_D9RightEyeSubmitSurface
          ? vr->m_D9RightEyeSubmitSurface
          : vr->m_D9RightEyeSurface;
        if (pSurface == rightEyeSurface)
          m_device->UpdateReShadeVrDepthAtlas(vr);
      }

      // The native OpenXR backend performs its own GPU blit into an
      // XrSwapchain image.  Leave the source image in its default layout so
      // DXVK's layout tracking stays consistent; only flush/synchronize the
      // D3D9 command stream here.
      m_device->Flush();
      m_device->SynchronizeCsThread(DxvkCsThread::SynchronizeAll);

      // This wait may need to be on all Faces and Mip Levels (2 loops).
      if (waitResourceIdle)
        m_device->WaitForResource(*image, tex->GetMappingBufferSequenceNumber(0u), D3DLOCK_READONLY);

      return D3D_OK;
    }

    HRESULT STDMETHODCALLTYPE LockDevice() {
      m_lock = m_device->LockDeviceExclusive();
      return D3D_OK;
    }

    HRESULT STDMETHODCALLTYPE UnlockDevice() {
      m_lock = D3D9DeviceLock();
      return D3D_OK;
    }

    HRESULT STDMETHODCALLTYPE LockSubmissionQueue() {
      // The native OpenXR backend does not submit through the OpenVR
      // compositor, so it does not need to hold DXVK's VkQueue across a
      // compositor call.  Flush and synchronize the D3D9 command stream so a
      // following BlitImages() sees a complete frame.  Taking the device lock
      // here and releasing it at the end of this call is deliberate: the
      // OpenXR frame thread later takes the same lock for its blit.
      D3D9DeviceLock lock = m_device->LockDeviceExclusive();
      m_device->Flush();
      m_device->SynchronizeCsThread(DxvkCsThread::SynchronizeAll);
      return D3D_OK;
    }

    HRESULT STDMETHODCALLTYPE LockPreparedSubmissionQueue() {
      // The producer already flushed/synchronized before publishing.  The
      // OpenXR blit path waits for GPU idle under the device lock, so no
      // external VkQueue gate is required.
      return D3D_OK;
    }

    HRESULT STDMETHODCALLTYPE DrawQueuedEyeSubmitOverlays(VR* vr) {
      if (unlikely(vr == nullptr))
        return D3DERR_INVALIDCALL;

      // The caller owns the VR texture mutex and the recursive D3D9 device lock.
      // Keep these draws in the same copy -> decorate -> transition transaction.
      m_device->DrawQueuedEyeSubmitOverlays(vr);
      return D3D_OK;
    }

    HRESULT STDMETHODCALLTYPE UnlockSubmissionQueue() {
      return D3D_OK;
    }

    HRESULT STDMETHODCALLTYPE WaitDeviceIdle() {
      // This can be called from the VR/Present path while Source's queued material
      // thread is active. DXVK's D3D9 command chunk is device-owned mutable state, so
      // flushing/synchronizing without the same device lock used by draw calls can race
      // DrawIndexedPrimitive and leave m_csChunk null/moved.
      D3D9DeviceLock lock = m_device->LockDeviceExclusive();

      m_device->Flush();
      // Not clear if we need all here, perhaps...
      m_device->SynchronizeCsThread(DxvkCsThread::SynchronizeAll);
      m_device->GetDXVKDevice()->waitForIdle();
      return D3D_OK;
    }

    HRESULT STDMETHODCALLTYPE GetBackBufferData(SharedTextureHolder* backBufferData) {
      if (unlikely(backBufferData == nullptr))
        return D3DERR_INVALIDCALL;

      IDirect3DSurface9* backBufferSurface = nullptr;

      HRESULT res = m_device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBufferSurface);
      if (FAILED(res) || backBufferSurface == nullptr)
        return FAILED(res) ? res : D3DERR_INVALIDCALL;

      D3D9_TEXTURE_VR_DESC textureDesc = {};

      res = GetVRDesc(backBufferSurface, &textureDesc);

      if (backBufferSurface != nullptr)
        backBufferSurface->Release();

      if (FAILED(res))
        return res;

      std::memcpy(&backBufferData->m_VulkanData, &textureDesc, sizeof(xr::VRVulkanTextureData_t));
      backBufferData->m_VRTexture.handle = &backBufferData->m_VulkanData;
      backBufferData->m_VRTexture.eColorSpace = xr::ColorSpace_Auto;
      backBufferData->m_VRTexture.eType = xr::TextureType_Vulkan;

      return res;
    }

    HRESULT STDMETHODCALLTYPE BeginSourceFrameOwnership() {
      return m_device->BeginSourceFrameOwnership() ? D3D_OK : D3DERR_INVALIDCALL;
    }

    HRESULT STDMETHODCALLTYPE EndSourceFrameOwnership() {
      return m_device->EndSourceFrameOwnership() ? D3D_OK : D3DERR_INVALIDCALL;
    }

    HRESULT STDMETHODCALLTYPE GetD3DDevice(IDirect3DDevice9** ppDevice) {
      if (unlikely(ppDevice == nullptr))
        return E_POINTER;

      *ppDevice = m_device;
      m_device->AddRef();
      return D3D_OK;
    }

    HRESULT STDMETHODCALLTYPE GetVulkanHandles(
            uint64_t* pInstance,
            uint64_t* pPhysicalDevice,
            uint64_t* pDevice,
            uint64_t* pQueue,
            uint32_t* pQueueFamilyIndex,
            uint32_t* pQueueIndex) {
      if (unlikely(!pInstance || !pPhysicalDevice || !pDevice || !pQueue ||
                   !pQueueFamilyIndex || !pQueueIndex))
        return E_POINTER;

      const auto& device = m_device->GetDXVKDevice();
      *pInstance = uint64_t(device->instance()->handle());
      *pPhysicalDevice = uint64_t(device->adapter()->handle());
      *pDevice = uint64_t(device->handle());
      *pQueue = uint64_t(device->queues().graphics.queueHandle);
      *pQueueFamilyIndex = device->queues().graphics.queueFamily;
      *pQueueIndex = device->queues().graphics.queueIndex;
      return D3D_OK;
    }

    HRESULT STDMETHODCALLTYPE BlitImages(
            const D3D9_VR_BLIT_DESC* pDescs,
            uint32_t unDescCount) {
      if (unlikely(pDescs == nullptr || unDescCount == 0))
        return D3DERR_INVALIDCALL;

      D3D9DeviceLock lock = m_device->LockDeviceExclusive();
      m_device->BlitImages(pDescs, unDescCount);
      return D3D_OK;
    }

  private:

    D3D9DeviceEx* m_device;
    D3D9DeviceLock m_lock;

  };

}

HRESULT __stdcall Direct3DCreateVRImpl(IDirect3DDevice9* pDevice, IDirect3DVR9** pInterface) {
  if (pDevice == nullptr || pInterface == nullptr)
    return D3DERR_INVALIDCALL;

  *pInterface = new dxvk::D3D9VR(pDevice);

  return D3D_OK;
}
