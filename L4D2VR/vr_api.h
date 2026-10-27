#pragma once
#ifndef L4D2VR_VR_API_H
#define L4D2VR_VR_API_H

/*
 * L4D2VR VR API
 *
 * This header replaces the OpenVR SDK surface that the mod historically used.
 * It deliberately keeps the small set of type/field names that the gameplay
 * code depends on, but every function below is implemented by the native
 * OpenXR backend in vr_openxr.cpp.  There is no openvr.h and no openvr_api.dll
 * dependency.
 */

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <cstdint>
#include <cstring>

namespace xr {

// ---------------------------------------------------------------------------
// Handles / limits
// ---------------------------------------------------------------------------
using TrackedDeviceIndex_t = uint32_t;
using VRActionHandle_t = uint64_t;
using VRActionSetHandle_t = uint64_t;
using VRInputValueHandle_t = uint64_t;
using VROverlayHandle_t = uint64_t;
using BoneIndex_t = uint32_t;

constexpr uint32_t k_unMaxTrackedDeviceCount = 64;
constexpr uint32_t k_unTrackedDeviceIndex_Hmd = 0;
constexpr uint32_t k_unTrackedDeviceIndexInvalid = 0xFFFFFFFFu;

constexpr uint64_t k_ulInvalidActionHandle = 0;
constexpr uint64_t k_ulInvalidActionSetHandle = 0;
constexpr uint64_t k_ulInvalidInputValueHandle = 0;
constexpr uint64_t k_ulOverlayHandleInvalid = 0;

constexpr uint32_t k_unMaxPropertyStringSize = 32768;
constexpr uint32_t k_unInvalidBoneIndex = 0xFFFFFFFFu;
constexpr int k_unMaxBoneNameLength = 64;
constexpr int VRFinger_Count = 5;

// Synthetic input-source handles.  These replace OpenVR's opaque
// VRInputValueHandle_t values; only equality and left/right mapping matter.
constexpr uint64_t k_unInputSourceHead = 0x1000;
constexpr uint64_t k_unInputSourceLeft = 0x1001;
constexpr uint64_t k_unInputSourceRight = 0x1002;

// ---------------------------------------------------------------------------
// Enumerations and constants
// ---------------------------------------------------------------------------
enum ETrackingUniverseOrigin : int {
    TrackingUniverseSeated = 0,
    TrackingUniverseStanding = 1,
    TrackingUniverseRawAndUncalibrated = 2,
};

enum ETrackedControllerRole : int {
    TrackedControllerRole_Invalid = 0,
    TrackedControllerRole_LeftHand = 1,
    TrackedControllerRole_RightHand = 2,
    TrackedControllerRole_OptOut = 3,
    TrackedControllerRole_Treadmill = 4,
    TrackedControllerRole_Stylus = 5,
    TrackedControllerRole_Max = 5,
};

enum EVREye : int {
    Eye_Left = 0,
    Eye_Right = 1,
};

enum EVRApplicationType : int {
    VRApplication_Other = 0,
    VRApplication_Scene = 1,
    VRApplication_Background = 2,
    VRApplication_Utility = 3,
    VRApplication_Overlay = 4,
};

enum HmdError : int {
    VRInitError_None = 0,
    VRInitError_Unknown = 1,
};

enum EVRCompositorError : int {
    VRCompositorError_None = 0,
    VRCompositorError_RequestFailed = 1,
    VRCompositorError_IncompatibleVersion = 100,
    VRCompositorError_AlreadySubmitted = 115,
};

enum EVRInputError : int {
    VRInputError_None = 0,
    VRInputError_NameNotFound = 1,
    VRInputError_WrongType = 2,
    VRInputError_InvalidHandle = 3,
    VRInputError_InvalidParam = 4,
    VRInputError_NoSteam = 5,
    VRInputError_MaxCapacityReached = 6,
    VRInputError_IPCError = 7,
    VRInputError_NoActiveActionSet = 8,
    VRInputError_InvalidDevice = 9,
    VRInputError_InvalidSkeleton = 10,
    VRInputError_InvalidBoneCount = 11,
    VRInputError_InvalidCompressedData = 12,
    VRInputError_NoData = 13,
    VRInputError_BufferTooSmall = 14,
    VRInputError_MismatchedActionManifest = 15,
    VRInputError_MissingSkeletonData = 16,
    VRInputError_Unsupported = 17,
    VRInputError_InvalidBoneIndex = 18,
};

enum EVROverlayError : int {
    VROverlayError_None = 0,
    VROverlayError_UnknownOverlay = 10,
    VROverlayError_InvalidHandle = 11,
    VROverlayError_PermissionDenied = 12,
    VROverlayError_OverlayLimitExceeded = 13,
    VROverlayError_WrongVisibilityType = 14,
    VROverlayError_RequestFailed = 20,
    VROverlayError_InvalidParameter = 21,
    VROverlayError_KeyTooLong = 24,
    VROverlayError_NameTooLong = 25,
    VROverlayError_KeyInUse = 26,
    VROverlayError_WrongTransformType = 27,
    VROverlayError_InvalidTrackedDevice = 28,
    VROverlayError_InvalidTexture = 30,
    VROverlayError_UnableToLoadFile = 31,
    VROverlayError_KeyboardAlreadyInUse = 32,
    VROverlayError_NoNeighbor = 33,
};

enum EVRSubmitFlags : int {
    Submit_Default = 0,
    Submit_LensDistortionAlreadyApplied = 1 << 0,
    Submit_GlRenderBuffer = 1 << 1,
    Submit_Reserved = 1 << 2,
    Submit_TextureWithPose = 1 << 3,
    Submit_TextureWithDepth = 1 << 4,
};

enum ETextureType : int {
    TextureType_DirectX = 0,
    TextureType_OpenGL = 1,
    TextureType_Vulkan = 2,
    TextureType_IOSurface = 3,
    TextureType_DirectX12 = 4,
};

enum EColorSpace : int {
    ColorSpace_Auto = 0,
    ColorSpace_Gamma = 1,
    ColorSpace_Linear = 2,
};

enum VROverlayInputMethod : int {
    VROverlayInputMethod_None = 0,
    VROverlayInputMethod_Mouse = 1,
};

enum VROverlayTransformType : int {
    VROverlayTransform_Absolute = 0,
    VROverlayTransform_TrackedDeviceRelative = 1,
    VROverlayTransform_SystemOverlay = 2,
    VROverlayTransform_TrackedComponent = 3,
};

enum VROverlayFlags : uint32_t {
    VROverlayFlags_None = 0,
    VROverlayFlags_Curved = 1 << 0,
    VROverlayFlags_RGSS4X = 1 << 1,
    VROverlayFlags_NoDashboardBackdrop = 1 << 2,
    VROverlayFlags_IgnoreTextureAlpha = 1 << 3,
    VROverlayFlags_VisibleInDashboard = 1 << 4,
    VROverlayFlags_MakeOverlaysInteractiveIfVisible = 1 << 5,
    VROverlayFlags_SendVRDiscreteScrollEvents = 1 << 6,
    VROverlayFlags_SendVRTouchpadEvents = 1 << 7,
    VROverlayFlags_ShowTouchPadScrollWheel = 1 << 8,
    VROverlayFlags_IsPremultiplied = 1 << 9,
};

enum VREventType : uint32_t {
    VREvent_None = 0,
    VREvent_ButtonPress = 100,
    VREvent_ButtonUnpress = 101,
    VREvent_MouseMove = 201,
    VREvent_MouseButtonDown = 202,
    VREvent_MouseButtonUp = 203,
    VREvent_ScrollDiscrete = 207,
    VREvent_KeyboardDone = 253,
    VREvent_KeyboardClosed = 254,
};

enum EVRSkeletalTransformSpace : int {
    VRSkeletalTransformSpace_Model = 0,
    VRSkeletalTransformSpace_Parent = 1,
    VRSkeletalTransformSpace_ModelRoot = 2,
};

enum EVRSkeletalReferencePose : int {
    VRSkeletalReferencePose_BindPose = 0,
    VRSkeletalReferencePose_OpenPose = 1,
    VRSkeletalReferencePose_GripPose = 2,
    VRSkeletalReferencePose_Tpose = 3,
};

enum EVRSkeletalMotionRange : int {
    VRSkeletalMotionRange_WithController = 0,
    VRSkeletalMotionRange_WithoutController = 1,
};

enum EVRSummaryType : int {
    VRSummaryType_FromAnimation = 0,
    VRSummaryType_FromDevice = 1,
};

enum ETrackedPropertyError : int {
    TrackedProp_Success = 0,
    TrackedProp_WrongDataType = 1,
    TrackedProp_WrongDeviceIndex = 2,
    TrackedProp_UnknownProperty = 3,
    TrackedProp_InvalidDevice = 4,
    TrackedProp_InvalidProperty = 5,
    TrackedProp_BufferTooSmall = 6,
};

enum ETrackedDeviceProperty : int {
    Prop_Invalid = 0,
    Prop_TrackingSystemName_String = 1000,
    Prop_ModelNumber_String = 1001,
    Prop_SerialNumber_String = 1002,
    Prop_RenderModelName_String = 1003,
    Prop_DisplayFrequency_Float = 2000,
    Prop_DeviceIsWireless_Bool = 2001,
    Prop_DeviceBatteryPercentage_Float = 2002,
};

enum EVRCompositorTimingMode : int {
    VRCompositorTimingMode_Implicit = 0,
    VRCompositorTimingMode_Explicit_RuntimePerformsPostPresentHandoff = 1,
    VRCompositorTimingMode_Explicit_ApplicationPerformsPostPresentHandoff = 2,
};

enum EGamepadTextInputMode : int {
    k_EGamepadTextInputModeNormal = 0,
    k_EGamepadTextInputModePassword = 1,
};

enum EGamepadTextInputLineMode : int {
    k_EGamepadTextInputLineModeSingleLine = 0,
    k_EGamepadTextInputLineModeMultipleLines = 1,
};

enum EKeyboardFlags : uint32_t {
    KeyboardFlag_Modal = 1 << 0,
};

enum EVRControllerEventOutputType : int {
    ControllerEventOutput_OSEvents = 0,
    ControllerEventOutput_VREvents = 1,
};

// Controller button bits used by the mod's overlay interactions.
enum EButton : uint32_t {
    k_EButton_System = 0,
    k_EButton_ApplicationMenu = 1,
    k_EButton_Grip = 2,
    k_EButton_DPad_Left = 3,
    k_EButton_DPad_Up = 4,
    k_EButton_DPad_Right = 5,
    k_EButton_DPad_Down = 6,
    k_EButton_A = 7,
    k_EButton_ProximitySensor = 31,
    k_EButton_Axis0 = 32,
    k_EButton_SteamVR_Touchpad = 32,
    k_EButton_SteamVR_Trigger = 33,
    k_EButton_IndexController_A = 35,
    k_EButton_IndexController_B = 36,
};

constexpr const char* k_pch_Controller_Component_Tip = "tip";

// ---------------------------------------------------------------------------
// POD types
// ---------------------------------------------------------------------------
struct HmdMatrix34_t {
    float m[3][4];
};

struct HmdVector2_t {
    float v[2];
};

struct HmdVector3_t {
    float v[3];
};

struct HmdVector4_t {
    float v[4];
};

struct HmdQuaternion_t {
    float w, x, y, z;
};

struct TrackedDevicePose_t {
    HmdMatrix34_t mDeviceToAbsoluteTracking;
    HmdVector3_t vVelocity;
    HmdVector3_t vAngularVelocity;
    int eTrackingResult;
    bool bPoseIsValid;
    bool bDeviceIsConnected;
};

struct Texture_t {
    void* handle;
    int eType;
    int eColorSpace;
};

struct VRTextureBounds_t {
    float uMin;
    float vMin;
    float uMax;
    float vMax;
};

struct VRTextureWithPose_t : public Texture_t {
    HmdMatrix34_t mDeviceToAbsoluteTracking;
};

struct VRVulkanTextureData_t {
    uint64_t m_nImage;
    void* m_pDevice;
    void* m_pPhysicalDevice;
    void* m_pInstance;
    void* m_pQueue;
    uint32_t m_nQueueFamilyIndex;
    uint32_t m_nWidth;
    uint32_t m_nHeight;
    uint32_t m_nFormat;
    uint32_t m_nSampleCount;

    // Extensions used by the OpenXR backend.  They are appended so the
    // original OpenVR-compatible prefix remains intact.
    uint32_t m_nQueueIndex;
    uint32_t m_nImageLayout;
    uint32_t m_nImageUsage;

    // KMT shared-memory handle for the x64 OpenXR helper.  Appended after the
    // existing fields so the D3D9 descriptor keeps the same layout.
    uint64_t m_nSharedHandle;
    uint32_t m_nSharedHandleType;
    uint32_t m_nSharedHandleValid;
};

struct VRActiveActionSet_t {
    uint64_t ulActionSet;
    uint64_t ulRestrictedToDevice;
    uint64_t ulSecondaryActionSet;
    uint32_t unPadding;
    int32_t nPriority;
};

struct InputDigitalActionData_t {
    bool bState;
    bool bActive;
    uint64_t activeOrigin;
    bool bChanged;
    float fUpdateTime;
};

struct InputAnalogActionData_t {
    float x;
    float y;
    float z;
    bool bActive;
    uint64_t activeOrigin;
    float deltaX;
    float deltaY;
    float deltaZ;
    float fUpdateTime;
};

struct InputSkeletalActionData_t {
    bool bActive;
    uint64_t activeOrigin;
};

struct VRSkeletalSummaryData_t {
    float flFingerCurl[5];
    float flFingerSplay[4];
};

struct VRBoneTransform_t {
    HmdVector4_t position;
    HmdQuaternion_t orientation;
};

struct InputBindingInfo_t {
    char rchDevicePathName[128];
    char rchInputPathName[128];
    char rchModeName[128];
    char rchSlotName[128];
    char rchInputSourceType[32];
};

struct InputOriginInfo_t {
    uint64_t devicePath;
    uint32_t trackedDeviceIndex;
    char rchRenderModelComponentName[128];
};

struct RenderModel_ControllerMode_State_t {
    bool bScrollWheelVisible;
};

struct RenderModel_ComponentState_t {
    HmdMatrix34_t mTrackingToComponentRenderModel;
    HmdMatrix34_t mTrackingToComponentLocal;
    uint32_t uProperties;
};

struct VROverlayIntersectionParams_t {
    ETrackingUniverseOrigin eOrigin;
    HmdVector3_t vSource;
    HmdVector3_t vDirection;
};

struct VROverlayIntersectionResults_t {
    HmdVector3_t vPoint;
    HmdVector3_t vNormal;
    HmdVector2_t vUVs;
    float fDistance;
};

struct VREvent_Mouse_t {
    float x;
    float y;
    uint32_t button;
};

struct VREvent_Scroll_t {
    float xdelta;
    float ydelta;
    uint32_t unused;
};

struct VREvent_Controller_t {
    uint32_t button;
};

struct VREvent_Keyboard_t {
    char cNewInput[64];
    uint64_t userValue;
    char cNewInput2[8];
    uint32_t unused;
};

union VREvent_Data_t {
    VREvent_Mouse_t mouse;
    VREvent_Scroll_t scroll;
    VREvent_Controller_t controller;
    VREvent_Keyboard_t keyboard;
    char padding[128];
};

struct VREvent_t {
    uint32_t eventType;
    uint32_t trackedDeviceIndex;
    float eventAgeSeconds;
    VREvent_Data_t data;
};

struct Compositor_FrameTiming {
    uint32_t m_nSize;
    uint32_t m_nFrameIndex;
    uint32_t m_nNumFramePresents;
    uint32_t m_nNumMisPresented;
    uint32_t m_nNumDroppedFrames;
    uint32_t m_nReprojectionFlags;
    double m_flSystemTimeInSeconds;
    float m_flPreSubmitGpuMs;
    float m_flPostSubmitGpuMs;
    float m_flTotalRenderGpuMs;
    float m_flCompositorRenderGpuMs;
    float m_flCompositorRenderCpuMs;
    float m_flCompositorIdleCpuMs;
    float m_flCompositorCpuMs;
    float m_flClientFrameIntervalMs;
    float m_flPresentCallCpuMs;
    float m_flWaitForPresentCpuMs;
    float m_flSubmitFrameMs;
    float m_flWaitGetPosesCalledMs;
    float m_flNewPosesReadyMs;
    float m_flNewFrameReadyMs;
    float m_flCompositorUpdateStartMs;
    float m_flCompositorUpdateEndMs;
    float m_flCompositorRenderStartMs;
    uint32_t m_nFrameId;
};

// ---------------------------------------------------------------------------
// Backend API
// ---------------------------------------------------------------------------
class System;
class Compositor;
class Input;
class Overlay;
class Applications;
class RenderModels;

System* SystemApi();
Compositor* CompositorApi();
Input* InputApi();
Overlay* OverlayApi();
Applications* ApplicationsApi();
RenderModels* RenderModelsApi();

// Initializes the OpenXR instance, system, actions and reference spaces.
// A session is created later, once the DXVK Vulkan device exists, by
// CreateSessionFromD3D9().
bool Initialize(const char* applicationName = "Left 4 Dead 2 VR");
bool CreateSessionFromD3D9();
void Shutdown();
bool IsInitialized();
int GetInitError();
const char* GetInitErrorDescription(int error);

// Kept for a small number of compatibility call sites.  These are native
// OpenXR wrappers, not OpenVR entry points.
HmdError VR_Init(HmdError* peError, EVRApplicationType eApplicationType);
void VR_Shutdown();
const char* VR_GetVRInitErrorAsEnglishDescription(HmdError eError);

class System {
public:
    void GetRecommendedRenderTargetSize(uint32_t* pnWidth, uint32_t* pnHeight);
    void GetProjectionRaw(EVREye eEye, float* pfLeft, float* pfRight, float* pfTop, float* pfBottom);
    HmdMatrix34_t GetEyeToHeadTransform(EVREye eEye);

    TrackedDeviceIndex_t GetTrackedDeviceIndexForControllerRole(ETrackedControllerRole role);
    void GetDeviceToAbsoluteTrackingPose(
        ETrackingUniverseOrigin eOrigin,
        float fPredictedSecondsToPhotonsFromNow,
        TrackedDevicePose_t* pTrackedDevicePoseArray,
        uint32_t unTrackedDevicePoseArrayCount);

    bool IsTrackedDeviceConnected(TrackedDeviceIndex_t unDeviceIndex);
    float GetFloatTrackedDeviceProperty(
        TrackedDeviceIndex_t unDeviceIndex,
        ETrackedDeviceProperty prop,
        ETrackedPropertyError* pError = nullptr);
    uint32_t GetStringTrackedDeviceProperty(
        TrackedDeviceIndex_t unDeviceIndex,
        ETrackedDeviceProperty prop,
        char* pchBuffer,
        uint32_t unBufferSize,
        ETrackedPropertyError* pError = nullptr);
    void TriggerHapticPulse(
        TrackedDeviceIndex_t unDeviceIndex,
        uint32_t unAxisId,
        uint16_t usDurationMicroSec);
};

class Compositor {
public:
    EVRCompositorError WaitGetPoses(
        TrackedDevicePose_t* pRenderPoseArray,
        uint32_t unRenderPoseArrayCount,
        TrackedDevicePose_t* pGamePoseArray,
        uint32_t unGamePoseArrayCount);

    EVRCompositorError Submit(
        EVREye eEye,
        const Texture_t* pTexture,
        const VRTextureBounds_t* pBounds = nullptr,
        EVRSubmitFlags nSubmitFlags = Submit_Default);

    bool GetFrameTiming(Compositor_FrameTiming* pTiming, uint32_t unFramesAgo = 0);
    float GetFrameTimeRemaining();
    ETrackingUniverseOrigin GetTrackingSpace();
    void SetTrackingSpace(ETrackingUniverseOrigin eOrigin);

    void SetExplicitTimingMode(EVRCompositorTimingMode eMode);
    EVRCompositorError SubmitExplicitTimingData();
    void PostPresentHandoff();
    void ClearLastSubmittedFrame();
};

class Input {
public:
    EVRInputError SetActionManifestPath(const char* pchActionManifestPath);
    EVRInputError GetActionHandle(const char* pchActionName, VRActionHandle_t* pHandle);
    EVRInputError GetActionSetHandle(const char* pchActionSetName, VRActionSetHandle_t* pHandle);
    EVRInputError UpdateActionState(VRActiveActionSet_t* pSets, uint32_t ulSizeOfActionSet, uint32_t unActionSetCount);
    EVRInputError GetDigitalActionData(
        VRActionHandle_t action,
        InputDigitalActionData_t* pActionData,
        uint32_t unActionDataSize,
        VRInputValueHandle_t ulRestrictToDevice);
    EVRInputError GetAnalogActionData(
        VRActionHandle_t action,
        InputAnalogActionData_t* pActionData,
        uint32_t unActionDataSize,
        VRInputValueHandle_t ulRestrictToDevice);
    EVRInputError GetSkeletalActionData(
        VRActionHandle_t action,
        InputSkeletalActionData_t* pActionData,
        uint32_t unActionDataSize);
    EVRInputError GetSkeletalSummaryData(
        VRActionHandle_t action,
        EVRSummaryType summaryType,
        VRSkeletalSummaryData_t* pSkeletalSummaryData);
    EVRInputError GetSkeletalBoneData(
        VRActionHandle_t action,
        EVRSkeletalTransformSpace eTransformSpace,
        EVRSkeletalMotionRange eMotionRange,
        VRBoneTransform_t* pTransformArray,
        uint32_t unTransformArrayCount);
    EVRInputError GetSkeletalReferenceTransforms(
        VRActionHandle_t action,
        EVRSkeletalTransformSpace eTransformSpace,
        EVRSkeletalReferencePose eReferencePose,
        VRBoneTransform_t* pTransformArray,
        uint32_t unTransformArrayCount);
    EVRInputError GetBoneCount(VRActionHandle_t action, uint32_t* pBoneCount);
    EVRInputError GetBoneHierarchy(VRActionHandle_t action, BoneIndex_t* pParentIndices, uint32_t unIndexArrayCount);
    EVRInputError GetBoneName(VRActionHandle_t action, BoneIndex_t nBoneIndex, char* pchBoneName, uint32_t unNameBufferSize);
    EVRInputError GetInputSourceHandle(const char* pchInputSourcePath, VRInputValueHandle_t* pHandle);
    EVRInputError GetActionBindingInfo(
        VRActionHandle_t action,
        InputBindingInfo_t* pBindings,
        uint32_t unBindingInfoSize,
        uint32_t unBindingInfoCount,
        uint32_t* punReturnedBindingInfoCount);
    EVRInputError GetOriginTrackedDeviceInfo(
        VRInputValueHandle_t origin,
        InputOriginInfo_t* pOriginInfo,
        uint32_t unOriginInfoSize);
};

class Overlay {
public:
    EVROverlayError CreateOverlay(const char* pchOverlayKey, const char* pchOverlayName, VROverlayHandle_t* pOverlayHandle);
    EVROverlayError DestroyOverlay(VROverlayHandle_t ulOverlayHandle);
    EVROverlayError FindOverlay(const char* pchOverlayKey, VROverlayHandle_t* pOverlayHandle);
    EVROverlayError SetOverlayRaw(VROverlayHandle_t ulOverlayHandle, void* pvBuffer, uint32_t unWidth, uint32_t unHeight, uint32_t unDepth);
    EVROverlayError SetOverlaySortOrder(VROverlayHandle_t ulOverlayHandle, uint32_t unSortOrder);

    EVROverlayError SetOverlayTexture(VROverlayHandle_t ulOverlayHandle, const Texture_t* pTexture);
    EVROverlayError SetOverlayTextureBounds(VROverlayHandle_t ulOverlayHandle, const VRTextureBounds_t* pOverlayTextureBounds);
    EVROverlayError SetOverlayTransformAbsolute(VROverlayHandle_t ulOverlayHandle, ETrackingUniverseOrigin eTrackingOrigin, const HmdMatrix34_t* pmatTrackingOriginToOverlayTransform);
    EVROverlayError SetOverlayTransformTrackedDeviceRelative(VROverlayHandle_t ulOverlayHandle, TrackedDeviceIndex_t unTrackedDevice, const HmdMatrix34_t* pmatTrackedDeviceToOverlayTransform);
    EVROverlayError GetOverlayTransformType(VROverlayHandle_t ulOverlayHandle, VROverlayTransformType* peTransformType);
    EVROverlayError GetOverlayTransformAbsolute(VROverlayHandle_t ulOverlayHandle, ETrackingUniverseOrigin* peTrackingOrigin, HmdMatrix34_t* pmatTrackingOriginToOverlayTransform);
    EVROverlayError GetOverlayTransformTrackedDeviceRelative(VROverlayHandle_t ulOverlayHandle, TrackedDeviceIndex_t* punTrackedDevice, HmdMatrix34_t* pmatTrackedDeviceToOverlayTransform);

    EVROverlayError ShowOverlay(VROverlayHandle_t ulOverlayHandle);
    EVROverlayError HideOverlay(VROverlayHandle_t ulOverlayHandle);
    bool IsOverlayVisible(VROverlayHandle_t ulOverlayHandle);

    EVROverlayError SetOverlayWidthInMeters(VROverlayHandle_t ulOverlayHandle, float fWidthInMeters);
    EVROverlayError GetOverlayWidthInMeters(VROverlayHandle_t ulOverlayHandle, float* pfWidthInMeters);
    EVROverlayError SetOverlayTexelAspect(VROverlayHandle_t ulOverlayHandle, float fTexelAspect);
    EVROverlayError SetOverlayAlpha(VROverlayHandle_t ulOverlayHandle, float fAlpha);
    EVROverlayError SetOverlayCurvature(VROverlayHandle_t ulOverlayHandle, float fCurvature);
    EVROverlayError SetOverlayInputMethod(VROverlayHandle_t ulOverlayHandle, VROverlayInputMethod eInputMethod);
    EVROverlayError SetOverlayFlag(VROverlayHandle_t ulOverlayHandle, VROverlayFlags eOverlayFlag, bool bEnabled);
    EVROverlayError SetOverlayMouseScale(VROverlayHandle_t ulOverlayHandle, const HmdVector2_t* pvecMouseScale);

    EVROverlayError ComputeOverlayIntersection(
        VROverlayHandle_t ulOverlayHandle,
        const VROverlayIntersectionParams_t* pParams,
        VROverlayIntersectionResults_t* pResults);
    bool PollNextOverlayEvent(VROverlayHandle_t ulOverlayHandle, VREvent_t* pEvent, uint32_t unEventSize);

    EVROverlayError ShowKeyboardForOverlay(
        VROverlayHandle_t ulOverlayHandle,
        EGamepadTextInputMode eInputMode,
        EGamepadTextInputLineMode eLineInputMode,
        uint32_t unFlags,
        const char* pchDescription,
        uint32_t unCharMax,
        const char* pchExistingText,
        uint64_t ulUserValue);
    uint32_t GetKeyboardText(char* pchText, uint32_t cchText);
    void HideKeyboard();
};

class Applications {
public:
    int AddApplicationManifest(const char* pchApplicationManifestFullPath, bool bTemporary = false);
    int RemoveApplicationManifest(const char* pchApplicationManifestFullPath);
};

class RenderModels {
public:
    bool GetComponentStateForDevicePath(
        const char* pchDevicePath,
        const char* pchComponentName,
        VRInputValueHandle_t ulDevicePath,
        RenderModel_ControllerMode_State_t* pState,
        RenderModel_ComponentState_t* pComponentState);
};

inline System* VRSystem() { return SystemApi(); }
inline Compositor* VRCompositor() { return CompositorApi(); }
inline Input* VRInput() { return InputApi(); }
inline Overlay* VROverlay() { return OverlayApi(); }
inline Applications* VRApplications() { return ApplicationsApi(); }
inline RenderModels* VRRenderModels() { return RenderModelsApi(); }

} // namespace xr

#endif // L4D2VR_VR_API_H
