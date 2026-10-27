/*
 * Native OpenXR backend for L4D2VR.
 *
 * This file replaces the OpenVR runtime.  The 32-bit game (d3d9.dll proxy)
 * creates the OpenXR instance/session directly and shares DXVK's Vulkan
 * instance/device/queue with the runtime through XR_KHR_vulkan_enable2 (or
 * XR_KHR_vulkan_enable on runtimes that only expose the v1 entry points).
 *
 * Eye and overlay textures are D3D9 render targets backed by DXVK Vulkan
 * images.  D3D9VR::BlitImages() copies them into the XrSwapchain images; the
 * frame thread then submits an XrCompositionLayerProjection plus quad layers
 * for the mod's overlays.
 */

#include "vr_api.h"

#include "d3d9_vr.h"
#include "game.h"

#include <vulkan/vulkan.h>

#define XR_USE_GRAPHICS_API_VULKAN 1
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// Loader
// ---------------------------------------------------------------------------

#define L4D2VR_XR_GLOBAL_FUNCTIONS(X) \
    X(xrEnumerateInstanceExtensionProperties) \
    X(xrCreateInstance)

#define L4D2VR_XR_INSTANCE_FUNCTIONS(X) \
    X(xrDestroyInstance) \
    X(xrGetInstanceProperties) \
    X(xrGetSystem) \
    X(xrGetSystemProperties) \
    X(xrEnumerateViewConfigurations) \
    X(xrEnumerateViewConfigurationViews) \
    X(xrEnumerateEnvironmentBlendModes) \
    X(xrCreateSession) \
    X(xrDestroySession) \
    X(xrBeginSession) \
    X(xrEndSession) \
    X(xrWaitFrame) \
    X(xrBeginFrame) \
    X(xrEndFrame) \
    X(xrLocateViews) \
    X(xrCreateReferenceSpace) \
    X(xrDestroySpace) \
    X(xrLocateSpace) \
    X(xrCreateActionSet) \
    X(xrDestroyActionSet) \
    X(xrCreateAction) \
    X(xrDestroyAction) \
    X(xrSuggestInteractionProfileBindings) \
    X(xrAttachSessionActionSets) \
    X(xrSyncActions) \
    X(xrGetActionStateBoolean) \
    X(xrGetActionStateFloat) \
    X(xrGetActionStateVector2f) \
    X(xrCreateActionSpace) \
    X(xrApplyHapticFeedback) \
    X(xrStringToPath) \
    X(xrPollEvent) \
    X(xrCreateSwapchain) \
    X(xrDestroySwapchain) \
    X(xrEnumerateSwapchainFormats) \
    X(xrEnumerateSwapchainImages) \
    X(xrAcquireSwapchainImage) \
    X(xrWaitSwapchainImage) \
    X(xrReleaseSwapchainImage) \
    X(xrGetVulkanGraphicsRequirementsKHR) \
    X(xrGetVulkanGraphicsDeviceKHR) \
    X(xrGetVulkanGraphicsRequirements2KHR) \
    X(xrGetVulkanGraphicsDevice2KHR) \
    X(xrGetDisplayRefreshRateFB)

namespace {

static_assert(sizeof(xr::VRVulkanTextureData_t) == sizeof(D3D9_TEXTURE_VR_DESC),
    "VRVulkanTextureData_t and D3D9_TEXTURE_VR_DESC must have identical layouts");

HMODULE g_loaderModule = nullptr;
PFN_xrGetInstanceProcAddr g_getInstanceProcAddr = nullptr;

#define L4D2VR_DECLARE_XR_FN(name) PFN_##name p_##name = nullptr;
L4D2VR_XR_GLOBAL_FUNCTIONS(L4D2VR_DECLARE_XR_FN)
L4D2VR_XR_INSTANCE_FUNCTIONS(L4D2VR_DECLARE_XR_FN)
#undef L4D2VR_DECLARE_XR_FN

template <typename T>
bool LoadGlobalFunction(const char* name, T& out) {
    out = nullptr;
    if (!g_getInstanceProcAddr)
        return false;
    PFN_xrVoidFunction fn = nullptr;
    if (XR_FAILED(g_getInstanceProcAddr(XR_NULL_HANDLE, name, &fn)) || !fn)
        return false;
    out = reinterpret_cast<T>(fn);
    return true;
}

template <typename T>
bool LoadInstanceFunction(XrInstance instance, const char* name, T& out) {
    out = nullptr;
    if (!g_getInstanceProcAddr || instance == XR_NULL_HANDLE)
        return false;
    PFN_xrVoidFunction fn = nullptr;
    if (XR_FAILED(g_getInstanceProcAddr(instance, name, &fn)) || !fn)
        return false;
    out = reinterpret_cast<T>(fn);
    return true;
}

bool LoadLoader() {
    if (g_getInstanceProcAddr)
        return true;

    const char* candidates[] = {
        "openxr_loader.dll",
        "wineopenxr.dll",
        "openxr_loader-1.dll",
    };

    for (const char* name : candidates) {
        HMODULE module = ::LoadLibraryA(name);
        if (!module)
            continue;
        auto proc = reinterpret_cast<PFN_xrGetInstanceProcAddr>(
            ::GetProcAddress(module, "xrGetInstanceProcAddr"));
        if (!proc) {
            ::FreeLibrary(module);
            continue;
        }
        g_loaderModule = module;
        g_getInstanceProcAddr = proc;
        break;
    }

    if (!g_getInstanceProcAddr)
        return false;

#define L4D2VR_LOAD_GLOBAL_FN(name) \
    if (!LoadGlobalFunction(#name, p_##name)) return false;
    L4D2VR_XR_GLOBAL_FUNCTIONS(L4D2VR_LOAD_GLOBAL_FN)
#undef L4D2VR_LOAD_GLOBAL_FN
    return true;
}

template <typename T>
void LoadOptionalInstanceFunction(XrInstance instance, const char* name, T& out) {
    LoadInstanceFunction(instance, name, out);
}

// ---------------------------------------------------------------------------
// Math
// ---------------------------------------------------------------------------

constexpr XrPosef kIdentityPose = { { 0.0f, 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, 0.0f } };

template <typename T>
T HandleFromU64(uint64_t value) {
    static_assert(sizeof(T) == sizeof(uint64_t), "Vulkan/OpenXR handle is not 64-bit");
    T out = {};
    std::memcpy(&out, &value, sizeof(out));
    return out;
}

template <typename T>
uint64_t HandleToU64(const T& value) {
    static_assert(sizeof(T) == sizeof(uint64_t), "Vulkan/OpenXR handle is not 64-bit");
    uint64_t out = 0;
    std::memcpy(&out, &value, sizeof(value));
    return out;
}

XrQuaternionf NormalizeQuat(const XrQuaternionf& q) {
    const float lenSq = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
    if (lenSq < 1e-8f)
        return { 0.0f, 0.0f, 0.0f, 1.0f };
    const float inv = 1.0f / std::sqrt(lenSq);
    return { q.x * inv, q.y * inv, q.z * inv, q.w * inv };
}

XrQuaternionf MultiplyQuat(const XrQuaternionf& a, const XrQuaternionf& b) {
    return NormalizeQuat({
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    });
}

XrVector3f RotateVector(const XrQuaternionf& q, const XrVector3f& v) {
    const XrVector3f qv = { q.x, q.y, q.z };
    const XrVector3f t = {
        2.0f * (qv.y * v.z - qv.z * v.y),
        2.0f * (qv.z * v.x - qv.x * v.z),
        2.0f * (qv.x * v.y - qv.y * v.x),
    };
    return {
        v.x + q.w * t.x + (qv.y * t.z - qv.z * t.y),
        v.y + q.w * t.y + (qv.z * t.x - qv.x * t.z),
        v.z + q.w * t.z + (qv.x * t.y - qv.y * t.x),
    };
}

XrPosef InversePose(const XrPosef& pose) {
    XrPosef out = {};
    out.orientation = { -pose.orientation.x, -pose.orientation.y, -pose.orientation.z, pose.orientation.w };
    const XrVector3f rotated = RotateVector(out.orientation, pose.position);
    out.position = { -rotated.x, -rotated.y, -rotated.z };
    return out;
}

XrPosef MultiplyPose(const XrPosef& a, const XrPosef& b) {
    XrPosef out = {};
    out.orientation = MultiplyQuat(a.orientation, b.orientation);
    const XrVector3f rotated = RotateVector(a.orientation, b.position);
    out.position = {
        a.position.x + rotated.x,
        a.position.y + rotated.y,
        a.position.z + rotated.z,
    };
    return out;
}

XrVector3f TransformPoint(const XrPosef& pose, const XrVector3f& point) {
    const XrVector3f rotated = RotateVector(pose.orientation, point);
    return {
        pose.position.x + rotated.x,
        pose.position.y + rotated.y,
        pose.position.z + rotated.z,
    };
}

xr::HmdMatrix34_t PoseToMatrix(const XrPosef& pose) {
    const XrQuaternionf q = NormalizeQuat(pose.orientation);
    xr::HmdMatrix34_t out = {};

    const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    const float xw = q.x * q.w, yw = q.y * q.w, zw = q.z * q.w;

    out.m[0][0] = 1.0f - 2.0f * (yy + zz);
    out.m[0][1] = 2.0f * (xy - zw);
    out.m[0][2] = 2.0f * (xz + yw);
    out.m[1][0] = 2.0f * (xy + zw);
    out.m[1][1] = 1.0f - 2.0f * (xx + zz);
    out.m[1][2] = 2.0f * (yz - xw);
    out.m[2][0] = 2.0f * (xz - yw);
    out.m[2][1] = 2.0f * (yz + xw);
    out.m[2][2] = 1.0f - 2.0f * (xx + yy);

    out.m[0][3] = pose.position.x;
    out.m[1][3] = pose.position.y;
    out.m[2][3] = pose.position.z;
    return out;
}

XrPosef MatrixToPose(const xr::HmdMatrix34_t& mat) {
    XrPosef out = kIdentityPose;
    out.position = { mat.m[0][3], mat.m[1][3], mat.m[2][3] };

    const float m00 = mat.m[0][0], m01 = mat.m[0][1], m02 = mat.m[0][2];
    const float m10 = mat.m[1][0], m11 = mat.m[1][1], m12 = mat.m[1][2];
    const float m20 = mat.m[2][0], m21 = mat.m[2][1], m22 = mat.m[2][2];
    const float trace = m00 + m11 + m22;

    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        out.orientation.w = 0.25f * s;
        out.orientation.x = (m21 - m12) / s;
        out.orientation.y = (m02 - m20) / s;
        out.orientation.z = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        out.orientation.w = (m21 - m12) / s;
        out.orientation.x = 0.25f * s;
        out.orientation.y = (m01 + m10) / s;
        out.orientation.z = (m02 + m20) / s;
    } else if (m11 > m22) {
        const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        out.orientation.w = (m02 - m20) / s;
        out.orientation.x = (m01 + m10) / s;
        out.orientation.y = 0.25f * s;
        out.orientation.z = (m12 + m21) / s;
    } else {
        const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        out.orientation.w = (m10 - m01) / s;
        out.orientation.x = (m02 + m20) / s;
        out.orientation.y = (m12 + m21) / s;
        out.orientation.z = 0.25f * s;
    }

    out.orientation = NormalizeQuat(out.orientation);
    return out;
}

// ---------------------------------------------------------------------------
// Backend types
// ---------------------------------------------------------------------------

enum class ActionKind {
    Boolean,
    FloatDigital,
    Float,
    Vector2f,
    Pose,
    Vibration,
    Skeleton,
};

struct ActionBindingDesc {
    std::string profilePath;
    std::string inputPath;
    XrAction xrAction = XR_NULL_HANDLE;
};

struct BindingInfoRecord {
    std::string devicePath;
    std::string inputPath;
    std::string mode;
    std::string slot;
    std::string sourceType;
};

struct Action {
    std::string path;
    std::string name;
    ActionKind kind = ActionKind::Boolean;
    XrAction handle = XR_NULL_HANDLE;
    XrAction handleBoolean = XR_NULL_HANDLE;
    XrActionSet actionSet = XR_NULL_HANDLE;
    float floatThreshold = 0.5f;
    bool isVirtual = false;
    bool prevDigital = false;

    std::vector<ActionBindingDesc> bindings;
    std::vector<BindingInfoRecord> bindingInfo;

    xr::InputDigitalActionData_t digital{};
    xr::InputAnalogActionData_t analog{};
    bool skeletalActive = false;
    xr::VRSkeletalSummaryData_t skeletalSummary{};
    bool virtualState = false;
    bool virtualPrevState = false;
};

struct HandInput {
    bool poseValid = false;
    bool aimValid = false;
    XrPosef gripPose = kIdentityPose;
    XrPosef aimPose = kIdentityPose;
    XrPosef aimInGrip = kIdentityPose;
    XrVector3f linearVelocity = {};
    XrVector3f angularVelocity = {};
    float trigger = 0.0f;
    float grip = 0.0f;
    float stickX = 0.0f;
    float stickY = 0.0f;
    bool primary = false;
    bool secondary = false;
    bool stickClick = false;
};

struct PoseSnapshot {
    uint64_t seq = 0;
    bool valid = false;
    XrTime predictedDisplayTime = 0;
    std::array<XrView, 2> views{};
    uint32_t viewCount = 0;
    xr::TrackedDevicePose_t poses[xr::k_unMaxTrackedDeviceCount]{};
    HandInput hands[2];
    XrPosef eyeToHead[2] = { kIdentityPose, kIdentityPose };
    std::chrono::steady_clock::time_point wallTime{};
};

struct EyeSwapchain {
    XrSwapchain handle = XR_NULL_HANDLE;
    uint32_t width = 0;
    uint32_t height = 0;
    int64_t format = 0;
    std::vector<XrSwapchainImageVulkanKHR> images;
};

struct OverlaySwapchain {
    XrSwapchain handle = XR_NULL_HANDLE;
    uint32_t width = 0;
    uint32_t height = 0;
    int64_t format = 0;
    std::vector<XrSwapchainImageVulkanKHR> images;
};

struct OverlayState {
    xr::VROverlayHandle_t handle = xr::k_ulOverlayHandleInvalid;
    std::string key;
    std::string name;
    bool visible = false;
    bool hasTexture = false;
    xr::VRVulkanTextureData_t texture{};
    xr::VRTextureBounds_t bounds{ 0.0f, 0.0f, 1.0f, 1.0f };
    float widthMeters = 1.0f;
    float texelAspect = 1.0f;
    float alpha = 1.0f;
    float curvature = 0.0f;
    xr::HmdVector2_t mouseScale{ 1.0f, 1.0f };
    int inputMethod = xr::VROverlayInputMethod_None;
    uint32_t flags = 0;
    int transformType = xr::VROverlayTransform_Absolute;
    int transformOrigin = xr::TrackingUniverseStanding;
    xr::TrackedDeviceIndex_t transformDevice = xr::k_unTrackedDeviceIndexInvalid;
    xr::HmdMatrix34_t transform{};
    std::shared_ptr<OverlaySwapchain> swapchain;
    uint64_t textureGeneration = 0;
    uint32_t sortOrder = 0;

    // CPU-uploaded overlay (SetOverlayRaw).  Kept alive so the Vulkan
    // descriptor in `texture` remains valid.
    IDirect3DTexture9* rawTexture = nullptr;
    IDirect3DSurface9* rawSurface = nullptr;
    uint32_t rawWidth = 0;
    uint32_t rawHeight = 0;

    bool pointerHovered = false;
    int pointerHand = -1;
    float pointerU = 0.0f;
    float pointerV = 0.0f;
    bool pointerDown = false;
    bool scrollUpPrev = false;
    bool scrollDownPrev = false;
    bool facePressedPrev = false;
    std::vector<xr::VREvent_t> events;
};

void ReleaseOverlayRaw(OverlayState& overlay);

struct FrameEyeTexture {
    bool valid = false;
    xr::VRVulkanTextureData_t texture{};
    xr::VRTextureBounds_t bounds{ 0.0f, 0.0f, 1.0f, 1.0f };
    bool hasBounds = false;
    xr::HmdMatrix34_t renderPose{};
    bool hasRenderPose = false;
};

struct FrameJob {
    uint64_t id = 0;
    FrameEyeTexture eyes[2];
};

struct FrameOverlaySnapshot {
    OverlayState state;
    std::shared_ptr<OverlaySwapchain> swapchain;
};

struct ProfileDesc {
    const char* path;
    const char* stick;
};

struct ActionDef {
    const char* path;
    ActionKind kind;
    float threshold;
};

const ProfileDesc kProfiles[] = {
    { "/interaction_profiles/oculus/touch_controller", "thumbstick" },
    { "/interaction_profiles/facebook/touch_controller_pro", "thumbstick" },
    { "/interaction_profiles/meta/touch_controller_plus", "thumbstick" },
    { "/interaction_profiles/bytedance/pico4_controller", "thumbstick" },
    { "/interaction_profiles/hp/mixed_reality_controller", "thumbstick" },
    { "/interaction_profiles/valve/index_controller", "thumbstick" },
    { "/interaction_profiles/microsoft/motion_controller", "thumbstick" },
    { "/interaction_profiles/htc/vive_cosmos_controller", "joystick" },
    { "/interaction_profiles/htc/vive_focus3_controller", "joystick" },
    { "/interaction_profiles/htc/vive_controller", "trackpad" },
    { "/interaction_profiles/khr/simple_controller", nullptr },
};

const ActionDef kActionDefs[] = {
    { "/actions/main/in/PrimaryAttack", ActionKind::FloatDigital, 0.45f },
    { "/actions/main/in/SecondaryAttack", ActionKind::FloatDigital, 0.45f },
    { "/actions/main/in/Jump", ActionKind::Boolean, 0.5f },
    { "/actions/main/in/Walk", ActionKind::Vector2f, 0.5f },
    { "/actions/main/in/Turn", ActionKind::Vector2f, 0.5f },
    { "/actions/main/in/Reload", ActionKind::FloatDigital, 0.50f },
    { "/actions/main/in/Use", ActionKind::Boolean, 0.5f },
    { "/actions/main/in/Teleport", ActionKind::Boolean, 0.5f },
    { "/actions/main/in/NextItem", ActionKind::Boolean, 0.5f },
    { "/actions/main/in/PrevItem", ActionKind::Boolean, 0.5f },
    { "/actions/main/in/ResetPosition", ActionKind::Boolean, 0.5f },
    { "/actions/main/in/Crouch", ActionKind::FloatDigital, 0.50f },
    { "/actions/main/in/Flashlight", ActionKind::Boolean, 0.5f },
    { "/actions/main/in/SpecialInfectedAutoAimToggle", ActionKind::Boolean, 0.5f },
    { "/actions/main/in/SpecialInfectedDodgeToggle", ActionKind::Boolean, 0.5f },
    { "/actions/main/in/LedgeGuardToggle", ActionKind::Boolean, 0.5f },
    { "/actions/main/in/EffectiveAttackRangeAutoFireToggle", ActionKind::Boolean, 0.5f },
    { "/actions/main/in/ActivateVR", ActionKind::Boolean, 0.5f },
    { "/actions/main/in/MenuSelect", ActionKind::Boolean, 0.5f },
    { "/actions/main/in/MenuBack", ActionKind::Boolean, 0.5f },
    { "/actions/main/in/MenuUp", ActionKind::Boolean, 0.5f },
    { "/actions/main/in/MenuDown", ActionKind::Boolean, 0.5f },
    { "/actions/main/in/MenuLeft", ActionKind::Boolean, 0.5f },
    { "/actions/main/in/MenuRight", ActionKind::Boolean, 0.5f },
    { "/actions/main/in/Spray", ActionKind::Boolean, 0.5f },
    { "/actions/main/in/Scoreboard", ActionKind::Boolean, 0.5f },
    { "/actions/main/in/ShowHUD", ActionKind::Boolean, 0.5f },
    { "/actions/main/in/Pause", ActionKind::Boolean, 0.5f },
    { "/actions/main/in/NonVRServerMovementAngleToggle", ActionKind::Boolean, 0.5f },
    { "/actions/main/in/CustomAction1", ActionKind::Boolean, 0.5f },
    { "/actions/main/in/CustomAction2", ActionKind::Boolean, 0.5f },
    { "/actions/main/in/CustomAction3", ActionKind::Boolean, 0.5f },
    { "/actions/main/in/CustomAction4", ActionKind::Boolean, 0.5f },
    { "/actions/main/in/CustomAction5", ActionKind::Boolean, 0.5f },
    { "/actions/main/in/ScopeToggle", ActionKind::Boolean, 0.5f },
    { "/actions/main/in/ScopeMagnificationToggle", ActionKind::Boolean, 0.5f },
    { "/actions/main/in/FriendlyFireBlockToggle", ActionKind::Boolean, 0.5f },
    { "/actions/main/in/InventoryQuickSwitch", ActionKind::Boolean, 0.5f },
    { "/actions/main/in/InventoryGripLeft", ActionKind::FloatDigital, 0.50f },
    { "/actions/main/in/InventoryGripRight", ActionKind::FloatDigital, 0.50f },
    { "/actions/main/in/SpeechToText", ActionKind::Boolean, 0.5f },
    { "/actions/main/in/boolean_turnleft", ActionKind::Boolean, 0.5f },
    { "/actions/main/in/boolean_turnright", ActionKind::Boolean, 0.5f },
    { "/actions/base/in/skeleton_lefthand", ActionKind::Skeleton, 0.5f },
    { "/actions/base/in/skeleton_righthand", ActionKind::Skeleton, 0.5f },
    { "internal/TriggerLeft", ActionKind::Float, 0.5f },
    { "internal/TriggerRight", ActionKind::Float, 0.5f },
    { "internal/GripLeft", ActionKind::Float, 0.5f },
    { "internal/GripRight", ActionKind::Float, 0.5f },
    { "internal/GripPoseLeft", ActionKind::Pose, 0.5f },
    { "internal/GripPoseRight", ActionKind::Pose, 0.5f },
    { "internal/AimPoseLeft", ActionKind::Pose, 0.5f },
    { "internal/AimPoseRight", ActionKind::Pose, 0.5f },
    { "internal/VibrationLeft", ActionKind::Vibration, 0.5f },
    { "internal/VibrationRight", ActionKind::Vibration, 0.5f },
};

bool EndsWith(const std::string& value, const char* suffix) {
    const size_t len = std::strlen(suffix);
    return value.size() >= len && value.compare(value.size() - len, len, suffix) == 0;
}

std::string JoinPath(const char* hand, const char* component) {
    std::string out = "/user/hand/";
    out += hand;
    out += "/input/";
    out += component;
    return out;
}

std::string OutputPath(const char* hand) {
    std::string out = "/user/hand/";
    out += hand;
    out += "/output/haptic";
    return out;
}

std::string PosePath(const char* hand, const char* kind) {
    std::string out = "/user/hand/";
    out += hand;
    out += "/input/";
    out += kind;
    out += "/pose";
    return out;
}

// Conservative profile feature table.  Unsupported paths are dropped by the
// retry logic in SuggestProfile().
struct ProfileFeatures {
    bool touchLike = false;
    bool index = false;
    bool wmr = false;
    bool cosmos = false;
    bool vive = false;
    bool simple = false;
};

ProfileFeatures FeaturesFor(const std::string& profile) {
    ProfileFeatures f = {};
    if (profile.find("oculus") != std::string::npos ||
        profile.find("facebook") != std::string::npos ||
        profile.find("meta") != std::string::npos ||
        profile.find("pico4") != std::string::npos ||
        profile.find("hp/mixed_reality") != std::string::npos) {
        f.touchLike = true;
    } else if (profile.find("valve/index") != std::string::npos) {
        f.index = true;
    } else if (profile.find("microsoft/motion") != std::string::npos) {
        f.wmr = true;
    } else if (profile.find("vive_cosmos") != std::string::npos ||
               profile.find("vive_focus3") != std::string::npos) {
        f.cosmos = true;
    } else if (profile.find("htc/vive_controller") != std::string::npos) {
        f.vive = true;
    } else if (profile.find("khr/simple") != std::string::npos) {
        f.simple = true;
    }
    return f;
}

class Backend {
public:
    static Backend& Instance() {
        static Backend s_instance;
        return s_instance;
    }

    bool Initialize(const char* appName);
    bool CreateSessionFromD3D9();
    void Shutdown();
    bool IsInitialized() const { return m_initialized.load(); }
    int InitError() const { return m_initError; }
    const char* InitErrorDescription() const { return m_initErrorText.c_str(); }

    void GetRecommendedRenderTargetSize(uint32_t* w, uint32_t* h) const;
    void GetProjectionRaw(int eye, float* left, float* right, float* top, float* bottom) const;
    xr::HmdMatrix34_t GetEyeToHeadTransform(int eye) const;
    xr::TrackedDeviceIndex_t GetTrackedDeviceIndexForControllerRole(int role) const;
    void GetDeviceToAbsoluteTrackingPose(int origin, float predictedSeconds, xr::TrackedDevicePose_t* poses, uint32_t count);
    bool IsTrackedDeviceConnected(xr::TrackedDeviceIndex_t index);
    float GetFloatTrackedDeviceProperty(xr::TrackedDeviceIndex_t index, xr::ETrackedDeviceProperty prop, xr::ETrackedPropertyError* error);
    uint32_t GetStringTrackedDeviceProperty(xr::TrackedDeviceIndex_t index, xr::ETrackedDeviceProperty prop, char* buffer, uint32_t bufferSize, xr::ETrackedPropertyError* error);
    void TriggerHapticPulse(xr::TrackedDeviceIndex_t index, uint32_t axisId, uint16_t durationMicroSec);

    xr::EVRCompositorError WaitGetPoses(xr::TrackedDevicePose_t* renderPoses, uint32_t renderCount, xr::TrackedDevicePose_t* gamePoses, uint32_t gameCount);
    xr::EVRCompositorError Submit(int eye, const xr::Texture_t* texture, const xr::VRTextureBounds_t* bounds, int flags);
    bool GetFrameTiming(xr::Compositor_FrameTiming* timing, uint32_t framesAgo);
    float GetFrameTimeRemaining();
    xr::ETrackingUniverseOrigin GetTrackingSpace() const;
    void SetExplicitTimingMode(int mode);
    void SubmitExplicitTimingData();
    void PostPresentHandoff();
    void ClearLastSubmittedFrame();

    xr::EVRInputError GetActionHandle(const char* path, xr::VRActionHandle_t* handle);
    xr::EVRInputError GetActionSetHandle(const char* path, xr::VRActionSetHandle_t* handle);
    xr::EVRInputError UpdateActionState();
    xr::EVRInputError GetDigitalActionData(xr::VRActionHandle_t action, xr::InputDigitalActionData_t* data, uint32_t size, xr::VRInputValueHandle_t restrictToDevice);
    xr::EVRInputError GetAnalogActionData(xr::VRActionHandle_t action, xr::InputAnalogActionData_t* data, uint32_t size, xr::VRInputValueHandle_t restrictToDevice);
    xr::EVRInputError GetSkeletalActionData(xr::VRActionHandle_t action, xr::InputSkeletalActionData_t* data, uint32_t size);
    xr::EVRInputError GetSkeletalSummaryData(xr::VRActionHandle_t action, int summaryType, xr::VRSkeletalSummaryData_t* data);
    xr::EVRInputError GetSkeletalBoneData(xr::VRActionHandle_t action, int transformSpace, int motionRange, xr::VRBoneTransform_t* transforms, uint32_t count);
    xr::EVRInputError GetSkeletalReferenceTransforms(xr::VRActionHandle_t action, int transformSpace, int referencePose, xr::VRBoneTransform_t* transforms, uint32_t count);
    xr::EVRInputError GetBoneCount(xr::VRActionHandle_t action, uint32_t* boneCount);
    xr::EVRInputError GetBoneHierarchy(xr::VRActionHandle_t action, xr::BoneIndex_t* parentIndices, uint32_t count);
    xr::EVRInputError GetBoneName(xr::VRActionHandle_t action, xr::BoneIndex_t boneIndex, char* name, uint32_t nameSize);
    xr::EVRInputError GetInputSourceHandle(const char* path, xr::VRInputValueHandle_t* handle);
    xr::EVRInputError GetActionBindingInfo(xr::VRActionHandle_t action, xr::InputBindingInfo_t* bindings, uint32_t bindingSize, uint32_t bindingCount, uint32_t* returnedCount);
    xr::EVRInputError GetOriginTrackedDeviceInfo(xr::VRInputValueHandle_t origin, xr::InputOriginInfo_t* info, uint32_t infoSize);

    xr::EVROverlayError CreateOverlay(const char* key, const char* name, xr::VROverlayHandle_t* handle);
    xr::EVROverlayError DestroyOverlay(xr::VROverlayHandle_t handle);
    xr::EVROverlayError FindOverlay(const char* key, xr::VROverlayHandle_t* handle);
    xr::EVROverlayError SetOverlayRaw(xr::VROverlayHandle_t handle, void* buffer, uint32_t width, uint32_t height, uint32_t depth);
    xr::EVROverlayError SetOverlaySortOrder(xr::VROverlayHandle_t handle, uint32_t sortOrder);
    xr::EVROverlayError SetOverlayTexture(xr::VROverlayHandle_t handle, const xr::Texture_t* texture);
    xr::EVROverlayError SetOverlayTextureBounds(xr::VROverlayHandle_t handle, const xr::VRTextureBounds_t* bounds);
    xr::EVROverlayError SetOverlayTransformAbsolute(xr::VROverlayHandle_t handle, int origin, const xr::HmdMatrix34_t* transform);
    xr::EVROverlayError SetOverlayTransformTrackedDeviceRelative(xr::VROverlayHandle_t handle, xr::TrackedDeviceIndex_t device, const xr::HmdMatrix34_t* transform);
    xr::EVROverlayError GetOverlayTransformType(xr::VROverlayHandle_t handle, int* transformType);
    xr::EVROverlayError GetOverlayTransformAbsolute(xr::VROverlayHandle_t handle, int* origin, xr::HmdMatrix34_t* transform);
    xr::EVROverlayError GetOverlayTransformTrackedDeviceRelative(xr::VROverlayHandle_t handle, xr::TrackedDeviceIndex_t* device, xr::HmdMatrix34_t* transform);
    xr::EVROverlayError ShowOverlay(xr::VROverlayHandle_t handle);
    xr::EVROverlayError HideOverlay(xr::VROverlayHandle_t handle);
    bool IsOverlayVisible(xr::VROverlayHandle_t handle);
    xr::EVROverlayError SetOverlayWidthInMeters(xr::VROverlayHandle_t handle, float width);
    xr::EVROverlayError GetOverlayWidthInMeters(xr::VROverlayHandle_t handle, float* width);
    xr::EVROverlayError SetOverlayTexelAspect(xr::VROverlayHandle_t handle, float aspect);
    xr::EVROverlayError SetOverlayAlpha(xr::VROverlayHandle_t handle, float alpha);
    xr::EVROverlayError SetOverlayCurvature(xr::VROverlayHandle_t handle, float curvature);
    xr::EVROverlayError SetOverlayInputMethod(xr::VROverlayHandle_t handle, int method);
    xr::EVROverlayError SetOverlayFlag(xr::VROverlayHandle_t handle, uint32_t flag, bool enabled);
    xr::EVROverlayError SetOverlayMouseScale(xr::VROverlayHandle_t handle, const xr::HmdVector2_t* scale);
    xr::EVROverlayError ComputeOverlayIntersection(xr::VROverlayHandle_t handle, const xr::VROverlayIntersectionParams_t* params, xr::VROverlayIntersectionResults_t* results);
    bool PollNextOverlayEvent(xr::VROverlayHandle_t handle, xr::VREvent_t* event, uint32_t eventSize);

    bool GetComponentStateForDevicePath(const char* devicePath, const char* componentName, xr::VRInputValueHandle_t inputValue, xr::RenderModel_ControllerMode_State_t* state, xr::RenderModel_ComponentState_t* componentState);

    void Log(const char* fmt, ...) const;

private:
    Backend() = default;
    ~Backend() = default;
    Backend(const Backend&) = delete;
    Backend& operator=(const Backend&) = delete;

    bool CreateInstance();
    bool CreateActionSets();
    bool CreateSession();
    bool CreateReferenceSpaces();
    bool CreateActionSpaces();
    bool CreateEyeSwapchains();
    std::shared_ptr<OverlaySwapchain> CreateOverlaySwapchain(uint32_t width, uint32_t height);
    void DestroyEyeSwapchain(EyeSwapchain& sc);
    void DestroyOverlaySwapchain(std::shared_ptr<OverlaySwapchain>& sc);
    int64_t ChooseColorFormat() const;
    XrAction CreateXrAction(const char* name, XrActionType type, bool bothHands, XrActionSet actionSet);
    void RegisterAction(const char* path, ActionKind kind, float threshold, bool isVirtual = false);
    void BuildBindings();
    void Bind(const char* actionPath, const char* profile, const std::string& inputPath);
    void AddPoseHaptics(const char* profile);
    void AddStick(const char* profile, const char* stickName);
    void AddTriggers(const char* profile);
    void AddGrip(const char* profile, const char* valuePath, const char* clickPath);
    void AddFace(const char* profile, const char* leftA, const char* leftB, const char* rightA, const char* rightB);
    void AddMenu(const char* profile);
    void SuggestProfile(const char* profilePath, std::vector<XrActionSuggestedBinding> bindings);
    void RefreshBindingInfo(Action& action);

    Action* FindAction(const char* path);
    Action* FindActionByHandle(xr::VRActionHandle_t handle);
    bool QueryBoolean(XrAction action, XrPath subactionPath, bool& value, bool& changed, bool& active);
    bool QueryFloat(XrAction action, XrPath subactionPath, float& value, bool& active);
    bool QueryVector2(XrAction action, XrPath subactionPath, XrVector2f& value, bool& active);
    void UpdateHandInput();
    void UpdateVirtualActions();
    bool ApplyHaptic(int hand, uint16_t durationMicroSec);
    void QueryRefreshRate();

    void FrameThreadMain();
    void PumpEvents();
    void PublishPoseSnapshot(XrTime predictedDisplayTime, const XrView* views, uint32_t viewCount, const XrViewState& viewState);
    bool WaitForFrameJob(FrameJob& job, std::chrono::milliseconds timeout);
    void CompleteFrameJob(uint64_t jobId);
    void CopyAndSubmitFrame(const FrameJob* job, const XrView* views, uint32_t viewCount, XrTime predictedDisplayTime);
    void BuildOverlaySnapshots(std::vector<FrameOverlaySnapshot>& out);
    bool ResolveOverlayPose(const OverlayState& overlay, const PoseSnapshot& snapshot, XrPosef& pose) const;
    bool IntersectOverlay(const OverlayState& overlay, const PoseSnapshot& snapshot, const XrPosef& rayPose, XrVector3f& hitPoint, float& distance, float& u, float& v) const;
    void GenerateOverlayEvents(OverlayState& overlay, const PoseSnapshot& snapshot);
    void WaitForFrameIdle();

    void SetError(int code, const char* fmt, ...);

    std::atomic<bool> m_initialized{ false };
    std::atomic<bool> m_sessionRunning{ false };
    std::atomic<bool> m_stopFrameThread{ false };
    int m_initError = -1;
    std::string m_initErrorText;
    std::string m_appName;

    XrInstance m_instance = XR_NULL_HANDLE;
    XrSystemId m_systemId = XR_NULL_SYSTEM_ID;
    XrSession m_session = XR_NULL_HANDLE;
    XrViewConfigurationType m_viewConfigType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    XrEnvironmentBlendMode m_blendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    std::vector<XrViewConfigurationView> m_viewConfigs;
    uint32_t m_renderWidth = 0;
    uint32_t m_renderHeight = 0;
    uint32_t m_maxSwapchainWidth = 0;
    uint32_t m_maxSwapchainHeight = 0;
    bool m_hasVulkanEnable2 = false;
    bool m_hasVulkanEnable1 = false;
    bool m_hasDisplayRefreshRate = false;
    int64_t m_colorFormat = 0;
    std::vector<int64_t> m_swapchainFormats;

    XrSpace m_stageSpace = XR_NULL_HANDLE;
    XrSpace m_viewSpace = XR_NULL_HANDLE;
    xr::ETrackingUniverseOrigin m_trackingOrigin = xr::TrackingUniverseStanding;
    EyeSwapchain m_eyeSwapchains[2];

    XrActionSet m_mainActionSet = XR_NULL_HANDLE;
    XrActionSet m_baseActionSet = XR_NULL_HANDLE;
    XrPath m_leftPath = XR_NULL_PATH;
    XrPath m_rightPath = XR_NULL_PATH;
    std::vector<std::unique_ptr<Action>> m_actions;
    std::unordered_map<std::string, xr::VRActionHandle_t> m_actionHandles;

    Action* m_triggerActions[2] = { nullptr, nullptr };
    Action* m_gripActions[2] = { nullptr, nullptr };
    Action* m_vibrationActions[2] = { nullptr, nullptr };
    Action* m_gripPoseActions[2] = { nullptr, nullptr };
    Action* m_aimPoseActions[2] = { nullptr, nullptr };
    XrSpace m_gripSpaces[2] = { XR_NULL_HANDLE, XR_NULL_HANDLE };
    XrSpace m_aimSpaces[2] = { XR_NULL_HANDLE, XR_NULL_HANDLE };

    Action* m_walkAction = nullptr;
    Action* m_turnAction = nullptr;
    Action* m_primaryAction = nullptr;
    Action* m_secondaryAction = nullptr;
    Action* m_activateAction = nullptr;
    Action* m_useAction = nullptr;
    Action* m_scoreboardAction = nullptr;
    Action* m_pauseAction = nullptr;
    Action* m_resetAction = nullptr;
    Action* m_flashlightAction = nullptr;
    Action* m_reloadAction = nullptr;
    Action* m_crouchAction = nullptr;

    mutable std::mutex m_inputMutex;
    HandInput m_handInputCache[2];

    std::vector<OverlayState> m_overlays;
    std::vector<std::pair<IDirect3DTexture9*, IDirect3DSurface9*>> m_retiredRawTextures;
    mutable std::mutex m_overlayMutex;
    std::atomic<uint64_t> m_nextOverlayHandle{ 1 };

    mutable std::mutex m_poseMutex;
    std::condition_variable m_poseCv;
    PoseSnapshot m_poseSnapshot;
    uint64_t m_lastWaitPoseSeq = 0;
    std::atomic<uint32_t> m_frameIndex{ 0 };
    std::atomic<float> m_refreshRateHz{ 90.0f };
    std::atomic<uint32_t> m_refreshRateQueryMs{ 0 };
    std::atomic<float> m_lastFrameIntervalSeconds{ 1.0f / 90.0f };

    std::mutex m_submitMutex;
    FrameEyeTexture m_stagedEyes[2];
    bool m_stagedEyeValid[2] = { false, false };

    std::mutex m_jobMutex;
    std::condition_variable m_jobCv;
    std::condition_variable m_jobDoneCv;
    FrameJob m_pendingJob;
    bool m_jobPending = false;
    uint64_t m_nextJobId = 1;
    uint64_t m_completedJobId = 0;

    std::mutex m_frameMutex;
    std::condition_variable m_frameCv;
    bool m_frameInFlight = false;

    std::thread m_frameThread;
    mutable std::mutex m_xrCallMutex;
};

Backend& B() { return Backend::Instance(); }

} // namespace

// ---------------------------------------------------------------------------
// Public facades
// ---------------------------------------------------------------------------

namespace xr {

System* SystemApi() { static System s; return &s; }
Compositor* CompositorApi() { static Compositor c; return &c; }
Input* InputApi() { static Input i; return &i; }
Overlay* OverlayApi() { static Overlay o; return &o; }
Applications* ApplicationsApi() { static Applications a; return &a; }
RenderModels* RenderModelsApi() { static RenderModels r; return &r; }

bool Initialize(const char* applicationName) { return B().Initialize(applicationName); }
bool CreateSessionFromD3D9() { return B().CreateSessionFromD3D9(); }
void Shutdown() { B().Shutdown(); }
bool IsInitialized() { return B().IsInitialized(); }
int GetInitError() { return B().InitError(); }
const char* GetInitErrorDescription(int error) { (void)error; return B().InitErrorDescription(); }

HmdError VR_Init(HmdError* peError, EVRApplicationType eApplicationType) {
    (void)eApplicationType;
    const bool ok = B().Initialize("Left 4 Dead 2 VR");
    if (peError)
        *peError = ok ? VRInitError_None : static_cast<HmdError>(B().InitError() > 0 ? B().InitError() : 1);
    return peError ? *peError : (ok ? VRInitError_None : static_cast<HmdError>(1));
}
void VR_Shutdown() { B().Shutdown(); }
const char* VR_GetVRInitErrorAsEnglishDescription(HmdError eError) { (void)eError; return B().InitErrorDescription(); }

void System::GetRecommendedRenderTargetSize(uint32_t* pnWidth, uint32_t* pnHeight) { B().GetRecommendedRenderTargetSize(pnWidth, pnHeight); }
void System::GetProjectionRaw(EVREye eEye, float* pfLeft, float* pfRight, float* pfTop, float* pfBottom) { B().GetProjectionRaw((int)eEye, pfLeft, pfRight, pfTop, pfBottom); }
HmdMatrix34_t System::GetEyeToHeadTransform(EVREye eEye) { return B().GetEyeToHeadTransform((int)eEye); }
TrackedDeviceIndex_t System::GetTrackedDeviceIndexForControllerRole(ETrackedControllerRole role) { return B().GetTrackedDeviceIndexForControllerRole((int)role); }
void System::GetDeviceToAbsoluteTrackingPose(ETrackingUniverseOrigin eOrigin, float fPredictedSecondsToPhotonsFromNow, TrackedDevicePose_t* pTrackedDevicePoseArray, uint32_t unTrackedDevicePoseArrayCount) {
    B().GetDeviceToAbsoluteTrackingPose((int)eOrigin, fPredictedSecondsToPhotonsFromNow, pTrackedDevicePoseArray, unTrackedDevicePoseArrayCount);
}
bool System::IsTrackedDeviceConnected(TrackedDeviceIndex_t unDeviceIndex) { return B().IsTrackedDeviceConnected(unDeviceIndex); }
float System::GetFloatTrackedDeviceProperty(TrackedDeviceIndex_t unDeviceIndex, ETrackedDeviceProperty prop, ETrackedPropertyError* pError) { return B().GetFloatTrackedDeviceProperty(unDeviceIndex, prop, pError); }
uint32_t System::GetStringTrackedDeviceProperty(TrackedDeviceIndex_t unDeviceIndex, ETrackedDeviceProperty prop, char* pchBuffer, uint32_t unBufferSize, ETrackedPropertyError* pError) { return B().GetStringTrackedDeviceProperty(unDeviceIndex, prop, pchBuffer, unBufferSize, pError); }
void System::TriggerHapticPulse(TrackedDeviceIndex_t unDeviceIndex, uint32_t unAxisId, uint16_t usDurationMicroSec) { B().TriggerHapticPulse(unDeviceIndex, unAxisId, usDurationMicroSec); }

EVRCompositorError Compositor::WaitGetPoses(TrackedDevicePose_t* pRenderPoseArray, uint32_t unRenderPoseArrayCount, TrackedDevicePose_t* pGamePoseArray, uint32_t unGamePoseArrayCount) { return B().WaitGetPoses(pRenderPoseArray, unRenderPoseArrayCount, pGamePoseArray, unGamePoseArrayCount); }
EVRCompositorError Compositor::Submit(EVREye eEye, const Texture_t* pTexture, const VRTextureBounds_t* pBounds, EVRSubmitFlags nSubmitFlags) { return B().Submit((int)eEye, pTexture, pBounds, (int)nSubmitFlags); }
bool Compositor::GetFrameTiming(Compositor_FrameTiming* pTiming, uint32_t unFramesAgo) { return B().GetFrameTiming(pTiming, unFramesAgo); }
float Compositor::GetFrameTimeRemaining() { return B().GetFrameTimeRemaining(); }
ETrackingUniverseOrigin Compositor::GetTrackingSpace() { return B().GetTrackingSpace(); }
void Compositor::SetTrackingSpace(ETrackingUniverseOrigin eOrigin) { (void)eOrigin; }
void Compositor::SetExplicitTimingMode(EVRCompositorTimingMode eMode) { B().SetExplicitTimingMode((int)eMode); }
EVRCompositorError Compositor::SubmitExplicitTimingData() { B().SubmitExplicitTimingData(); return VRCompositorError_None; }
void Compositor::PostPresentHandoff() { B().PostPresentHandoff(); }
void Compositor::ClearLastSubmittedFrame() { B().ClearLastSubmittedFrame(); }

EVRInputError Input::SetActionManifestPath(const char* pchActionManifestPath) { (void)pchActionManifestPath; return VRInputError_None; }
EVRInputError Input::GetActionHandle(const char* pchActionName, VRActionHandle_t* pHandle) { return B().GetActionHandle(pchActionName, pHandle); }
EVRInputError Input::GetActionSetHandle(const char* pchActionSetName, VRActionSetHandle_t* pHandle) { return B().GetActionSetHandle(pchActionSetName, pHandle); }
EVRInputError Input::UpdateActionState(VRActiveActionSet_t* pSets, uint32_t ulSizeOfActionSet, uint32_t unActionSetCount) { (void)pSets; (void)ulSizeOfActionSet; (void)unActionSetCount; return B().UpdateActionState(); }
EVRInputError Input::GetDigitalActionData(VRActionHandle_t action, InputDigitalActionData_t* pActionData, uint32_t unActionDataSize, VRInputValueHandle_t ulRestrictToDevice) { return B().GetDigitalActionData(action, pActionData, unActionDataSize, ulRestrictToDevice); }
EVRInputError Input::GetAnalogActionData(VRActionHandle_t action, InputAnalogActionData_t* pActionData, uint32_t unActionDataSize, VRInputValueHandle_t ulRestrictToDevice) { return B().GetAnalogActionData(action, pActionData, unActionDataSize, ulRestrictToDevice); }
EVRInputError Input::GetSkeletalActionData(VRActionHandle_t action, InputSkeletalActionData_t* pActionData, uint32_t unActionDataSize) { return B().GetSkeletalActionData(action, pActionData, unActionDataSize); }
EVRInputError Input::GetSkeletalSummaryData(VRActionHandle_t action, EVRSummaryType summaryType, VRSkeletalSummaryData_t* pSkeletalSummaryData) { return B().GetSkeletalSummaryData(action, (int)summaryType, pSkeletalSummaryData); }
EVRInputError Input::GetSkeletalBoneData(VRActionHandle_t action, EVRSkeletalTransformSpace eTransformSpace, EVRSkeletalMotionRange eMotionRange, VRBoneTransform_t* pTransformArray, uint32_t unTransformArrayCount) { return B().GetSkeletalBoneData(action, (int)eTransformSpace, (int)eMotionRange, pTransformArray, unTransformArrayCount); }
EVRInputError Input::GetSkeletalReferenceTransforms(VRActionHandle_t action, EVRSkeletalTransformSpace eTransformSpace, EVRSkeletalReferencePose eReferencePose, VRBoneTransform_t* pTransformArray, uint32_t unTransformArrayCount) { return B().GetSkeletalReferenceTransforms(action, (int)eTransformSpace, (int)eReferencePose, pTransformArray, unTransformArrayCount); }
EVRInputError Input::GetBoneCount(VRActionHandle_t action, uint32_t* pBoneCount) { return B().GetBoneCount(action, pBoneCount); }
EVRInputError Input::GetBoneHierarchy(VRActionHandle_t action, BoneIndex_t* pParentIndices, uint32_t unIndexArrayCount) { return B().GetBoneHierarchy(action, pParentIndices, unIndexArrayCount); }
EVRInputError Input::GetBoneName(VRActionHandle_t action, BoneIndex_t nBoneIndex, char* pchBoneName, uint32_t unNameBufferSize) { return B().GetBoneName(action, nBoneIndex, pchBoneName, unNameBufferSize); }
EVRInputError Input::GetInputSourceHandle(const char* pchInputSourcePath, VRInputValueHandle_t* pHandle) { return B().GetInputSourceHandle(pchInputSourcePath, pHandle); }
EVRInputError Input::GetActionBindingInfo(VRActionHandle_t action, InputBindingInfo_t* pBindings, uint32_t unBindingInfoSize, uint32_t unBindingInfoCount, uint32_t* punReturnedBindingInfoCount) { return B().GetActionBindingInfo(action, pBindings, unBindingInfoSize, unBindingInfoCount, punReturnedBindingInfoCount); }
EVRInputError Input::GetOriginTrackedDeviceInfo(VRInputValueHandle_t origin, InputOriginInfo_t* pOriginInfo, uint32_t unOriginInfoSize) { return B().GetOriginTrackedDeviceInfo(origin, pOriginInfo, unOriginInfoSize); }

EVROverlayError Overlay::CreateOverlay(const char* pchOverlayKey, const char* pchOverlayName, VROverlayHandle_t* pOverlayHandle) { return B().CreateOverlay(pchOverlayKey, pchOverlayName, pOverlayHandle); }
EVROverlayError Overlay::DestroyOverlay(VROverlayHandle_t ulOverlayHandle) { return B().DestroyOverlay(ulOverlayHandle); }
EVROverlayError Overlay::FindOverlay(const char* pchOverlayKey, VROverlayHandle_t* pOverlayHandle) { return B().FindOverlay(pchOverlayKey, pOverlayHandle); }
EVROverlayError Overlay::SetOverlayRaw(VROverlayHandle_t ulOverlayHandle, void* pvBuffer, uint32_t unWidth, uint32_t unHeight, uint32_t unDepth) { return B().SetOverlayRaw(ulOverlayHandle, pvBuffer, unWidth, unHeight, unDepth); }
EVROverlayError Overlay::SetOverlaySortOrder(VROverlayHandle_t ulOverlayHandle, uint32_t unSortOrder) { return B().SetOverlaySortOrder(ulOverlayHandle, unSortOrder); }
EVROverlayError Overlay::SetOverlayTexture(VROverlayHandle_t ulOverlayHandle, const Texture_t* pTexture) { return B().SetOverlayTexture(ulOverlayHandle, pTexture); }
EVROverlayError Overlay::SetOverlayTextureBounds(VROverlayHandle_t ulOverlayHandle, const VRTextureBounds_t* pOverlayTextureBounds) { return B().SetOverlayTextureBounds(ulOverlayHandle, pOverlayTextureBounds); }
EVROverlayError Overlay::SetOverlayTransformAbsolute(VROverlayHandle_t ulOverlayHandle, ETrackingUniverseOrigin eTrackingOrigin, const HmdMatrix34_t* pmatTrackingOriginToOverlayTransform) { return B().SetOverlayTransformAbsolute(ulOverlayHandle, (int)eTrackingOrigin, pmatTrackingOriginToOverlayTransform); }
EVROverlayError Overlay::SetOverlayTransformTrackedDeviceRelative(VROverlayHandle_t ulOverlayHandle, TrackedDeviceIndex_t unTrackedDevice, const HmdMatrix34_t* pmatTrackedDeviceToOverlayTransform) { return B().SetOverlayTransformTrackedDeviceRelative(ulOverlayHandle, unTrackedDevice, pmatTrackedDeviceToOverlayTransform); }
EVROverlayError Overlay::GetOverlayTransformType(VROverlayHandle_t ulOverlayHandle, VROverlayTransformType* peTransformType) { return B().GetOverlayTransformType(ulOverlayHandle, reinterpret_cast<int*>(peTransformType)); }
EVROverlayError Overlay::GetOverlayTransformAbsolute(VROverlayHandle_t ulOverlayHandle, ETrackingUniverseOrigin* peTrackingOrigin, HmdMatrix34_t* pmatTrackingOriginToOverlayTransform) { return B().GetOverlayTransformAbsolute(ulOverlayHandle, reinterpret_cast<int*>(peTrackingOrigin), pmatTrackingOriginToOverlayTransform); }
EVROverlayError Overlay::GetOverlayTransformTrackedDeviceRelative(VROverlayHandle_t ulOverlayHandle, TrackedDeviceIndex_t* punTrackedDevice, HmdMatrix34_t* pmatTrackedDeviceToOverlayTransform) { return B().GetOverlayTransformTrackedDeviceRelative(ulOverlayHandle, punTrackedDevice, pmatTrackedDeviceToOverlayTransform); }
EVROverlayError Overlay::ShowOverlay(VROverlayHandle_t ulOverlayHandle) { return B().ShowOverlay(ulOverlayHandle); }
EVROverlayError Overlay::HideOverlay(VROverlayHandle_t ulOverlayHandle) { return B().HideOverlay(ulOverlayHandle); }
bool Overlay::IsOverlayVisible(VROverlayHandle_t ulOverlayHandle) { return B().IsOverlayVisible(ulOverlayHandle); }
EVROverlayError Overlay::SetOverlayWidthInMeters(VROverlayHandle_t ulOverlayHandle, float fWidthInMeters) { return B().SetOverlayWidthInMeters(ulOverlayHandle, fWidthInMeters); }
EVROverlayError Overlay::GetOverlayWidthInMeters(VROverlayHandle_t ulOverlayHandle, float* pfWidthInMeters) { return B().GetOverlayWidthInMeters(ulOverlayHandle, pfWidthInMeters); }
EVROverlayError Overlay::SetOverlayTexelAspect(VROverlayHandle_t ulOverlayHandle, float fTexelAspect) { return B().SetOverlayTexelAspect(ulOverlayHandle, fTexelAspect); }
EVROverlayError Overlay::SetOverlayAlpha(VROverlayHandle_t ulOverlayHandle, float fAlpha) { return B().SetOverlayAlpha(ulOverlayHandle, fAlpha); }
EVROverlayError Overlay::SetOverlayCurvature(VROverlayHandle_t ulOverlayHandle, float fCurvature) { return B().SetOverlayCurvature(ulOverlayHandle, fCurvature); }
EVROverlayError Overlay::SetOverlayInputMethod(VROverlayHandle_t ulOverlayHandle, VROverlayInputMethod eInputMethod) { return B().SetOverlayInputMethod(ulOverlayHandle, (int)eInputMethod); }
EVROverlayError Overlay::SetOverlayFlag(VROverlayHandle_t ulOverlayHandle, VROverlayFlags eOverlayFlag, bool bEnabled) { return B().SetOverlayFlag(ulOverlayHandle, (uint32_t)eOverlayFlag, bEnabled); }
EVROverlayError Overlay::SetOverlayMouseScale(VROverlayHandle_t ulOverlayHandle, const HmdVector2_t* pvecMouseScale) { return B().SetOverlayMouseScale(ulOverlayHandle, pvecMouseScale); }
EVROverlayError Overlay::ComputeOverlayIntersection(VROverlayHandle_t ulOverlayHandle, const VROverlayIntersectionParams_t* pParams, VROverlayIntersectionResults_t* pResults) { return B().ComputeOverlayIntersection(ulOverlayHandle, pParams, pResults); }
bool Overlay::PollNextOverlayEvent(VROverlayHandle_t ulOverlayHandle, VREvent_t* pEvent, uint32_t unEventSize) { return B().PollNextOverlayEvent(ulOverlayHandle, pEvent, unEventSize); }
EVROverlayError Overlay::ShowKeyboardForOverlay(VROverlayHandle_t ulOverlayHandle, EGamepadTextInputMode eInputMode, EGamepadTextInputLineMode eLineInputMode, uint32_t unFlags, const char* pchDescription, uint32_t unCharMax, const char* pchExistingText, uint64_t ulUserValue) {
    (void)ulOverlayHandle; (void)eInputMode; (void)eLineInputMode; (void)unFlags; (void)pchDescription; (void)unCharMax; (void)pchExistingText; (void)ulUserValue;
    B().Log("[OpenXR] Overlay keyboard is not available. Edit VR/config.txt on the desktop instead.");
    return VROverlayError_RequestFailed;
}
uint32_t Overlay::GetKeyboardText(char* pchText, uint32_t cchText) { if (pchText && cchText) pchText[0] = '\0'; return 0; }
void Overlay::HideKeyboard() {}

int Applications::AddApplicationManifest(const char* pchApplicationManifestFullPath, bool bTemporary) { (void)pchApplicationManifestFullPath; (void)bTemporary; return 0; }
int Applications::RemoveApplicationManifest(const char* pchApplicationManifestFullPath) { (void)pchApplicationManifestFullPath; return 0; }

bool RenderModels::GetComponentStateForDevicePath(const char* pchDevicePath, const char* pchComponentName, VRInputValueHandle_t ulDevicePath, RenderModel_ControllerMode_State_t* pState, RenderModel_ComponentState_t* pComponentState) {
    return B().GetComponentStateForDevicePath(pchDevicePath, pchComponentName, ulDevicePath, pState, pComponentState);
}

} // namespace xr

// ---------------------------------------------------------------------------
// Backend: initialization
// ---------------------------------------------------------------------------

namespace {

void Backend::Log(const char* fmt, ...) const {
    char buffer[2048] = {};
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    Game::logMsg("%s", buffer);
    OutputDebugStringA(buffer);
    OutputDebugStringA("\n");
}

void Backend::SetError(int code, const char* fmt, ...) {
    m_initError = code;
    char buffer[1024] = {};
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    m_initErrorText = buffer;
    Log("[OpenXR] ERROR: %s", buffer);
}

bool Backend::Initialize(const char* appName) {
    if (m_initialized.load())
        return true;
    m_initError = -1;
    m_initErrorText.clear();
    m_appName = appName ? appName : "Left 4 Dead 2 VR";

    if (!LoadLoader()) {
        SetError(2, "OpenXR loader not found. Install the 32-bit openxr_loader.dll next to left4dead2.exe or in the Wine prefix.");
        return false;
    }

    if (!CreateInstance())
        return false;
    if (!CreateActionSets())
        return false;

    m_initialized.store(true);
    Log("[OpenXR] initialized (%s)", m_appName.c_str());
    return true;
}

bool Backend::CreateInstance() {
    uint32_t extensionCount = 0;
    if (XR_FAILED(p_xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionCount, nullptr))) {
        SetError(3, "xrEnumerateInstanceExtensionProperties failed");
        return false;
    }

    std::vector<XrExtensionProperties> extensions(extensionCount);
    for (auto& ext : extensions)
        ext = { XR_TYPE_EXTENSION_PROPERTIES };
    if (extensionCount > 0 &&
        XR_FAILED(p_xrEnumerateInstanceExtensionProperties(nullptr, extensionCount, &extensionCount, extensions.data()))) {
        SetError(3, "xrEnumerateInstanceExtensionProperties(query) failed");
        return false;
    }

    std::vector<const char*> enabledExtensions;
    for (const auto& ext : extensions) {
        const std::string name = ext.extensionName;
        if (name == XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME)
            m_hasVulkanEnable2 = true;
        else if (name == XR_KHR_VULKAN_ENABLE_EXTENSION_NAME)
            m_hasVulkanEnable1 = true;
        else if (name == XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME)
            m_hasDisplayRefreshRate = true;
    }

    if (m_hasVulkanEnable2)
        enabledExtensions.push_back(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
    else if (m_hasVulkanEnable1)
        enabledExtensions.push_back(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);
    else {
        SetError(4, "The OpenXR runtime exposes neither XR_KHR_vulkan_enable2 nor XR_KHR_vulkan_enable.");
        return false;
    }

    if (m_hasDisplayRefreshRate)
        enabledExtensions.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);

    XrApplicationInfo appInfo = {};
    std::snprintf(appInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE, "%s", m_appName.c_str());
    appInfo.applicationVersion = 1;
    std::snprintf(appInfo.engineName, XR_MAX_ENGINE_NAME_SIZE, "Source");
    appInfo.engineVersion = 1;
    appInfo.apiVersion = XR_API_VERSION_1_0;

    XrInstanceCreateInfo createInfo = {};
    createInfo.type = XR_TYPE_INSTANCE_CREATE_INFO;
    createInfo.applicationInfo = appInfo;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size());
    createInfo.enabledExtensionNames = enabledExtensions.data();

    const XrResult result = p_xrCreateInstance(&createInfo, &m_instance);
    if (XR_FAILED(result)) {
        SetError(5, "xrCreateInstance failed (%d)", (int)result);
        return false;
    }

#define L4D2VR_LOAD_INSTANCE_FN(name) LoadInstanceFunction(m_instance, #name, p_##name);
    L4D2VR_XR_INSTANCE_FUNCTIONS(L4D2VR_LOAD_INSTANCE_FN)
#undef L4D2VR_LOAD_INSTANCE_FN

    XrSystemGetInfo systemInfo = {};
    systemInfo.type = XR_TYPE_SYSTEM_GET_INFO;
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (XR_FAILED(p_xrGetSystem(m_instance, &systemInfo, &m_systemId))) {
        SetError(6, "No OpenXR HMD system available");
        return false;
    }

    XrSystemProperties systemProps = {};
    systemProps.type = XR_TYPE_SYSTEM_PROPERTIES;
    if (XR_SUCCEEDED(p_xrGetSystemProperties(m_instance, m_systemId, &systemProps))) {
        m_maxSwapchainWidth = systemProps.graphicsProperties.maxSwapchainImageWidth;
        m_maxSwapchainHeight = systemProps.graphicsProperties.maxSwapchainImageHeight;
        Log("[OpenXR] system: %s vendor=%u maxSwapchain=%ux%u",
            systemProps.systemName, systemProps.vendorId,
            m_maxSwapchainWidth, m_maxSwapchainHeight);
    }

    uint32_t viewConfigCount = 0;
    if (XR_FAILED(p_xrEnumerateViewConfigurations(m_instance, m_systemId, 0, &viewConfigCount, nullptr)) || viewConfigCount == 0) {
        SetError(7, "No OpenXR view configurations");
        return false;
    }

    std::vector<XrViewConfigurationType> viewConfigs(viewConfigCount);
    if (XR_FAILED(p_xrEnumerateViewConfigurations(m_instance, m_systemId, viewConfigCount, &viewConfigCount, viewConfigs.data()))) {
        SetError(7, "xrEnumerateViewConfigurations failed");
        return false;
    }

    m_viewConfigType = viewConfigs[0];
    for (auto type : viewConfigs) {
        if (type == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) {
            m_viewConfigType = type;
            break;
        }
    }

    uint32_t configViewCount = 0;
    if (XR_FAILED(p_xrEnumerateViewConfigurationViews(m_instance, m_systemId, m_viewConfigType, 0, &configViewCount, nullptr)) || configViewCount < 2) {
        SetError(8, "The OpenXR runtime does not expose a stereo view configuration");
        return false;
    }

    XrViewConfigurationView viewConfigTemplate = {};
    viewConfigTemplate.type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
    m_viewConfigs.assign(configViewCount, viewConfigTemplate);
    if (XR_FAILED(p_xrEnumerateViewConfigurationViews(m_instance, m_systemId, m_viewConfigType, configViewCount, &configViewCount, m_viewConfigs.data()))) {
        SetError(8, "xrEnumerateViewConfigurationViews failed");
        return false;
    }

    m_renderWidth = 0;
    m_renderHeight = 0;
    for (const auto& view : m_viewConfigs) {
        m_renderWidth = (std::max)(m_renderWidth, view.recommendedImageRectWidth);
        m_renderHeight = (std::max)(m_renderHeight, view.recommendedImageRectHeight);
    }
    if (m_renderWidth == 0 || m_renderHeight == 0) {
        m_renderWidth = 1024;
        m_renderHeight = 1024;
    }

    uint32_t blendCount = 0;
    if (XR_SUCCEEDED(p_xrEnumerateEnvironmentBlendModes(m_instance, m_systemId, m_viewConfigType, 0, &blendCount, nullptr)) && blendCount > 0) {
        std::vector<XrEnvironmentBlendMode> blends(blendCount);
        if (XR_SUCCEEDED(p_xrEnumerateEnvironmentBlendModes(m_instance, m_systemId, m_viewConfigType, blendCount, &blendCount, blends.data()))) {
            m_blendMode = blends[0];
            for (auto blend : blends) {
                if (blend == XR_ENVIRONMENT_BLEND_MODE_OPAQUE) {
                    m_blendMode = blend;
                    break;
                }
            }
        }
    }

    if (XR_FAILED(p_xrStringToPath(m_instance, "/user/hand/left", &m_leftPath)) ||
        XR_FAILED(p_xrStringToPath(m_instance, "/user/hand/right", &m_rightPath))) {
        SetError(9, "Failed to resolve OpenXR hand paths");
        return false;
    }

    Log("[OpenXR] stereo views=%u recommended=%ux%u blend=%d vulkanEnable2=%d displayRefresh=%d",
        configViewCount, m_renderWidth, m_renderHeight, (int)m_blendMode,
        m_hasVulkanEnable2 ? 1 : 0, m_hasDisplayRefreshRate ? 1 : 0);
    return true;
}

XrAction Backend::CreateXrAction(const char* name, XrActionType type, bool bothHands, XrActionSet actionSet) {
    XrActionCreateInfo info = {};
    info.type = XR_TYPE_ACTION_CREATE_INFO;
    info.actionType = type;
    std::snprintf(info.actionName, XR_MAX_ACTION_NAME_SIZE, "%s", name);
    std::snprintf(info.localizedActionName, XR_MAX_LOCALIZED_ACTION_NAME_SIZE, "%s", name);

    XrPath subactions[2] = { m_leftPath, m_rightPath };
    if (bothHands) {
        info.countSubactionPaths = 2;
        info.subactionPaths = subactions;
    }

    XrAction action = XR_NULL_HANDLE;
    if (XR_FAILED(p_xrCreateAction(actionSet, &info, &action))) {
        Log("[OpenXR] xrCreateAction failed for %s", name);
        return XR_NULL_HANDLE;
    }
    return action;
}

void Backend::RegisterAction(const char* path, ActionKind kind, float threshold, bool isVirtual) {
    if (m_actionHandles.find(path) != m_actionHandles.end())
        return;

    auto action = std::make_unique<Action>();
    action->path = path;
    action->name = path;
    const size_t slash = action->name.find_last_of('/');
    if (slash != std::string::npos)
        action->name = action->name.substr(slash + 1);
    action->kind = kind;
    action->floatThreshold = threshold;
    action->isVirtual = isVirtual;
    action->actionSet = path && std::strstr(path, "/actions/base/") ? m_baseActionSet : m_mainActionSet;

    XrActionType xrType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    switch (kind) {
    case ActionKind::Boolean:
    case ActionKind::Skeleton:
        xrType = XR_ACTION_TYPE_BOOLEAN_INPUT;
        break;
    case ActionKind::FloatDigital:
    case ActionKind::Float:
        xrType = XR_ACTION_TYPE_FLOAT_INPUT;
        break;
    case ActionKind::Vector2f:
        xrType = XR_ACTION_TYPE_VECTOR2F_INPUT;
        break;
    case ActionKind::Pose:
        xrType = XR_ACTION_TYPE_POSE_INPUT;
        break;
    case ActionKind::Vibration:
        xrType = XR_ACTION_TYPE_VIBRATION_OUTPUT;
        break;
    }

    if (!isVirtual)
        action->handle = CreateXrAction(action->name.c_str(), xrType, true, action->actionSet);
    if (!isVirtual && kind == ActionKind::FloatDigital) {
        std::string booleanName = action->name + "_click";
        action->handleBoolean = CreateXrAction(booleanName.c_str(), XR_ACTION_TYPE_BOOLEAN_INPUT, true, action->actionSet);
    }

    m_actions.push_back(std::move(action));
    const xr::VRActionHandle_t handle = static_cast<xr::VRActionHandle_t>(m_actions.size());
    m_actionHandles[path] = handle;
}

bool Backend::CreateActionSets() {
    XrActionSetCreateInfo mainInfo = {};
    mainInfo.type = XR_TYPE_ACTION_SET_CREATE_INFO;
    std::snprintf(mainInfo.actionSetName, XR_MAX_ACTION_SET_NAME_SIZE, "main");
    std::snprintf(mainInfo.localizedActionSetName, XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE, "Main");
    mainInfo.priority = 0;
    if (XR_FAILED(p_xrCreateActionSet(m_instance, &mainInfo, &m_mainActionSet))) {
        SetError(10, "xrCreateActionSet(main) failed");
        return false;
    }

    XrActionSetCreateInfo baseInfo = {};
    baseInfo.type = XR_TYPE_ACTION_SET_CREATE_INFO;
    std::snprintf(baseInfo.actionSetName, XR_MAX_ACTION_SET_NAME_SIZE, "base");
    std::snprintf(baseInfo.localizedActionSetName, XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE, "Base");
    baseInfo.priority = 0;
    if (XR_FAILED(p_xrCreateActionSet(m_instance, &baseInfo, &m_baseActionSet))) {
        SetError(10, "xrCreateActionSet(base) failed");
        return false;
    }

    for (const auto& def : kActionDefs)
        RegisterAction(def.path, def.kind, def.threshold);

    const char* virtualPaths[] = {
        "/actions/main/in/MenuUp",
        "/actions/main/in/MenuDown",
        "/actions/main/in/MenuLeft",
        "/actions/main/in/MenuRight",
        "/actions/main/in/NextItem",
        "/actions/main/in/PrevItem",
        "/actions/main/in/boolean_turnleft",
        "/actions/main/in/boolean_turnright",
    };
    for (const char* path : virtualPaths) {
        Action* action = FindAction(path);
        if (!action)
            continue;
        action->isVirtual = true;
        if (action->handle != XR_NULL_HANDLE) {
            p_xrDestroyAction(action->handle);
            action->handle = XR_NULL_HANDLE;
        }
    }

    auto find = [&](const char* path) -> Action* { return FindAction(path); };
    m_triggerActions[0] = find("internal/TriggerLeft");
    m_triggerActions[1] = find("internal/TriggerRight");
    m_gripActions[0] = find("internal/GripLeft");
    m_gripActions[1] = find("internal/GripRight");
    m_vibrationActions[0] = find("internal/VibrationLeft");
    m_vibrationActions[1] = find("internal/VibrationRight");
    m_gripPoseActions[0] = find("internal/GripPoseLeft");
    m_gripPoseActions[1] = find("internal/GripPoseRight");
    m_aimPoseActions[0] = find("internal/AimPoseLeft");
    m_aimPoseActions[1] = find("internal/AimPoseRight");
    m_walkAction = find("/actions/main/in/Walk");
    m_turnAction = find("/actions/main/in/Turn");
    m_primaryAction = find("/actions/main/in/PrimaryAttack");
    m_secondaryAction = find("/actions/main/in/SecondaryAttack");
    m_activateAction = find("/actions/main/in/ActivateVR");
    m_useAction = find("/actions/main/in/Use");
    m_scoreboardAction = find("/actions/main/in/Scoreboard");
    m_pauseAction = find("/actions/main/in/Pause");
    m_resetAction = find("/actions/main/in/ResetPosition");
    m_flashlightAction = find("/actions/main/in/Flashlight");
    m_reloadAction = find("/actions/main/in/Reload");
    m_crouchAction = find("/actions/main/in/Crouch");

    BuildBindings();
    return true;
}

Action* Backend::FindAction(const char* path) {
    if (!path)
        return nullptr;
    auto it = m_actionHandles.find(path);
    if (it == m_actionHandles.end() || it->second == 0)
        return nullptr;
    const size_t index = static_cast<size_t>(it->second - 1);
    return index < m_actions.size() ? m_actions[index].get() : nullptr;
}

Action* Backend::FindActionByHandle(xr::VRActionHandle_t handle) {
    if (handle == xr::k_ulInvalidActionHandle)
        return nullptr;
    const size_t index = static_cast<size_t>(handle - 1);
    return index < m_actions.size() ? m_actions[index].get() : nullptr;
}

// ---------------------------------------------------------------------------
// Backend: bindings
// ---------------------------------------------------------------------------

void Backend::RefreshBindingInfo(Action& action) {
    action.bindingInfo.clear();
    for (const auto& binding : action.bindings) {
        BindingInfoRecord info;
        const std::string& path = binding.inputPath;
        const size_t inputPos = path.find("/input/");
        const size_t outputPos = path.find("/output/");
        if (inputPos != std::string::npos)
            info.devicePath = path.substr(0, inputPos);
        else if (outputPos != std::string::npos)
            info.devicePath = path.substr(0, outputPos);
        else
            info.devicePath = path;
        info.inputPath = path;
        info.mode = action.kind == ActionKind::Vector2f ? "joystick" : (action.kind == ActionKind::Pose ? "pose" : "button");
        const size_t slash = path.find_last_of('/');
        info.slot = slash == std::string::npos ? path : path.substr(slash + 1);
        info.sourceType = action.kind == ActionKind::Vector2f ? "joystick" : "button";
        action.bindingInfo.push_back(std::move(info));
    }
}

void Backend::Bind(const char* actionPath, const char* profile, const std::string& inputPath) {
    Action* action = FindAction(actionPath);
    if (!action || !profile || inputPath.empty())
        return;

    XrAction xrAction = action->handle;
    if (action->kind == ActionKind::FloatDigital) {
        // Use the analog action for /value components and the boolean twin for
        // clicks/face buttons.  The boolean twin also makes WMR's
        // squeeze/click work where the profile has no analog squeeze value.
        const bool analog = EndsWith(inputPath, "/value") || inputPath.find("/input/trigger") != std::string::npos;
        if (!analog && action->handleBoolean != XR_NULL_HANDLE)
            xrAction = action->handleBoolean;
    }
    if (xrAction == XR_NULL_HANDLE)
        return;

    for (const auto& existing : action->bindings) {
        if (existing.profilePath == profile && existing.inputPath == inputPath)
            return;
    }
    action->bindings.push_back({ profile, inputPath, xrAction });
}

void Backend::AddPoseHaptics(const char* profile) {
    Bind("internal/GripPoseLeft", profile, PosePath("left", "grip"));
    Bind("internal/GripPoseRight", profile, PosePath("right", "grip"));
    Bind("internal/AimPoseLeft", profile, PosePath("left", "aim"));
    Bind("internal/AimPoseRight", profile, PosePath("right", "aim"));
    Bind("internal/VibrationLeft", profile, OutputPath("left"));
    Bind("internal/VibrationRight", profile, OutputPath("right"));
}

void Backend::AddStick(const char* profile, const char* stickName) {
    if (!stickName)
        return;
    const std::string left = JoinPath("left", stickName);
    const std::string right = JoinPath("right", stickName);
    Bind("/actions/main/in/Walk", profile, left);
    Bind("/actions/main/in/Turn", profile, right);
    Bind("/actions/main/in/ResetPosition", profile, left + "/click");
    Bind("/actions/main/in/Flashlight", profile, right + "/click");
}

void Backend::AddTriggers(const char* profile) {
    Bind("/actions/main/in/PrimaryAttack", profile, "/user/hand/right/input/trigger/value");
    Bind("/actions/main/in/SecondaryAttack", profile, "/user/hand/left/input/trigger/value");
    Bind("internal/TriggerLeft", profile, "/user/hand/left/input/trigger/value");
    Bind("internal/TriggerRight", profile, "/user/hand/right/input/trigger/value");
}

void Backend::AddGrip(const char* profile, const char* valuePath, const char* clickPath) {
    if (valuePath) {
        Bind("/actions/main/in/Reload", profile, JoinPath("left", valuePath));
        Bind("/actions/main/in/Crouch", profile, JoinPath("right", valuePath));
        Bind("/actions/main/in/InventoryGripLeft", profile, JoinPath("left", valuePath));
        Bind("/actions/main/in/InventoryGripRight", profile, JoinPath("right", valuePath));
        Bind("internal/GripLeft", profile, JoinPath("left", valuePath));
        Bind("internal/GripRight", profile, JoinPath("right", valuePath));
    }
    if (clickPath) {
        Bind("/actions/main/in/Reload", profile, JoinPath("left", clickPath));
        Bind("/actions/main/in/Crouch", profile, JoinPath("right", clickPath));
        Bind("/actions/main/in/InventoryGripLeft", profile, JoinPath("left", clickPath));
        Bind("/actions/main/in/InventoryGripRight", profile, JoinPath("right", clickPath));
    }
}

void Backend::AddFace(const char* profile, const char* leftA, const char* leftB, const char* rightA, const char* rightB) {
    if (rightA) {
        Bind("/actions/main/in/ActivateVR", profile, JoinPath("right", rightA));
        Bind("/actions/main/in/Jump", profile, JoinPath("right", rightA));
        Bind("/actions/main/in/MenuSelect", profile, JoinPath("right", rightA));
    }
    if (rightB) {
        Bind("/actions/main/in/Use", profile, JoinPath("right", rightB));
        Bind("/actions/main/in/MenuBack", profile, JoinPath("right", rightB));
    }
    if (leftA)
        Bind("/actions/main/in/Scoreboard", profile, JoinPath("left", leftA));
    if (leftB)
        Bind("/actions/main/in/Pause", profile, JoinPath("left", leftB));
}

void Backend::AddMenu(const char* profile) {
    Bind("/actions/main/in/ActivateVR", profile, "/user/hand/right/input/menu/click");
    Bind("/actions/main/in/MenuSelect", profile, "/user/hand/right/input/menu/click");
    Bind("/actions/main/in/MenuBack", profile, "/user/hand/left/input/menu/click");
    Bind("/actions/main/in/Pause", profile, "/user/hand/left/input/menu/click");
    Bind("/actions/main/in/Pause", profile, "/user/hand/left/input/system/click");
}

void Backend::BuildBindings() {
    for (const auto& profile : kProfiles) {
        const ProfileFeatures features = FeaturesFor(profile.path);
        AddPoseHaptics(profile.path);

        if (features.touchLike) {
            AddStick(profile.path, "thumbstick");
            AddTriggers(profile.path);
            AddGrip(profile.path, "squeeze/value", "squeeze/click");
            AddFace(profile.path, "x/click", "y/click", "a/click", "b/click");
            AddMenu(profile.path);
        } else if (features.index) {
            AddStick(profile.path, "thumbstick");
            AddTriggers(profile.path);
            AddGrip(profile.path, "squeeze/value", "squeeze/click");
            AddFace(profile.path, "a/click", "b/click", "a/click", "b/click");
            AddMenu(profile.path);
        } else if (features.wmr) {
            AddStick(profile.path, "thumbstick");
            AddTriggers(profile.path);
            AddGrip(profile.path, nullptr, "squeeze/click");
            AddMenu(profile.path);
        } else if (features.cosmos) {
            AddStick(profile.path, "joystick");
            AddTriggers(profile.path);
            AddGrip(profile.path, "grip/value", "grip/click");
            AddFace(profile.path, "x/click", "y/click", "a/click", "b/click");
            AddMenu(profile.path);
        } else if (features.vive) {
            AddStick(profile.path, "trackpad");
            AddTriggers(profile.path);
            Bind("/actions/main/in/ActivateVR", profile.path, "/user/hand/right/input/trackpad/click");
            Bind("/actions/main/in/Jump", profile.path, "/user/hand/right/input/trackpad/click");
            Bind("/actions/main/in/MenuSelect", profile.path, "/user/hand/right/input/trackpad/click");
            Bind("/actions/main/in/Use", profile.path, "/user/hand/left/input/trackpad/click");
            Bind("/actions/main/in/MenuBack", profile.path, "/user/hand/left/input/trackpad/click");
            Bind("/actions/main/in/Reload", profile.path, "/user/hand/left/input/trackpad/click");
            Bind("/actions/main/in/Crouch", profile.path, "/user/hand/right/input/trackpad/click");
            AddMenu(profile.path);
        } else if (features.simple) {
            Bind("/actions/main/in/ActivateVR", profile.path, "/user/hand/right/input/select/click");
            Bind("/actions/main/in/Jump", profile.path, "/user/hand/right/input/select/click");
            Bind("/actions/main/in/MenuSelect", profile.path, "/user/hand/right/input/select/click");
            Bind("/actions/main/in/Use", profile.path, "/user/hand/left/input/select/click");
            Bind("/actions/main/in/MenuBack", profile.path, "/user/hand/left/input/select/click");
            Bind("/actions/main/in/Pause", profile.path, "/user/hand/left/input/menu/click");
        }
    }

    for (auto& actionPtr : m_actions) {
        Action& action = *actionPtr;
        if (!action.isVirtual)
            RefreshBindingInfo(action);
    }

    // Rebuild per-profile suggestion lists.
    for (const auto& profile : kProfiles) {
        std::vector<XrActionSuggestedBinding> suggested;
        for (auto& actionPtr : m_actions) {
            for (const auto& binding : actionPtr->bindings) {
                if (binding.profilePath != profile.path)
                    continue;
                XrPath path = XR_NULL_PATH;
                if (XR_FAILED(p_xrStringToPath(m_instance, binding.inputPath.c_str(), &path)))
                    continue;
                bool duplicate = false;
                for (const auto& existing : suggested) {
                    if (existing.action == binding.xrAction && existing.binding == path) {
                        duplicate = true;
                        break;
                    }
                }
                if (!duplicate)
                    suggested.push_back({ binding.xrAction, path });
            }
        }
        SuggestProfile(profile.path, std::move(suggested));
    }
}

void Backend::SuggestProfile(const char* profilePath, std::vector<XrActionSuggestedBinding> bindings) {
    if (!profilePath || bindings.empty())
        return;

    XrPath profile = XR_NULL_PATH;
    if (XR_FAILED(p_xrStringToPath(m_instance, profilePath, &profile)))
        return;

    auto trySuggest = [&](const std::vector<XrActionSuggestedBinding>& list) -> XrResult {
        if (list.empty())
            return XR_ERROR_VALIDATION_FAILURE;
        XrInteractionProfileSuggestedBinding suggested = {};
        suggested.type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING;
        suggested.interactionProfile = profile;
        suggested.countSuggestedBindings = static_cast<uint32_t>(list.size());
        suggested.suggestedBindings = list.data();
        return p_xrSuggestInteractionProfileBindings(m_instance, &suggested);
    };

    XrResult result = trySuggest(bindings);
    if (XR_SUCCEEDED(result)) {
        Log("[OpenXR] bindings %s ok count=%u", profilePath, (unsigned)bindings.size());
        return;
    }

    Log("[OpenXR] bindings %s failed (%d); retrying without unsupported paths",
        profilePath, (int)result);

    std::vector<XrActionSuggestedBinding> accepted;
    accepted.reserve(bindings.size());
    uint32_t dropped = 0;
    for (const auto& binding : bindings) {
        std::vector<XrActionSuggestedBinding> trial = accepted;
        trial.push_back(binding);
        if (XR_FAILED(trySuggest(trial))) {
            ++dropped;
            continue;
        }
        accepted.swap(trial);
    }

    if (accepted.empty()) {
        Log("[OpenXR] bindings %s unsupported; profile skipped", profilePath);
        return;
    }
    Log("[OpenXR] bindings %s ok count=%u dropped=%u",
        profilePath, (unsigned)accepted.size(), dropped);
}

// ---------------------------------------------------------------------------
// Backend: session
// ---------------------------------------------------------------------------

int64_t Backend::ChooseColorFormat() const {
    if (m_session == XR_NULL_HANDLE || m_swapchainFormats.empty())
        return static_cast<int64_t>(VK_FORMAT_B8G8R8A8_SRGB);

    // sRGB is important: the D3D9 render targets contain sRGB-encoded bytes.
    // DXVK samples a mutable sRGB view of the source and writes an sRGB
    // swapchain image, leaving the compositor with correct linear values.
    const int64_t preferred[] = {
        static_cast<int64_t>(VK_FORMAT_B8G8R8A8_SRGB),
        static_cast<int64_t>(VK_FORMAT_R8G8B8A8_SRGB),
        static_cast<int64_t>(VK_FORMAT_B8G8R8A8_UNORM),
        static_cast<int64_t>(VK_FORMAT_R8G8B8A8_UNORM),
    };
    for (int64_t want : preferred) {
        if (std::find(m_swapchainFormats.begin(), m_swapchainFormats.end(), want) != m_swapchainFormats.end())
            return want;
    }
    return m_swapchainFormats[0];
}

bool Backend::CreateSession() {
    uint64_t instance = 0, physicalDevice = 0, device = 0, queue = 0;
    uint32_t queueFamily = 0, queueIndex = 0;
    if (!g_D3DVR9 || FAILED(g_D3DVR9->GetVulkanHandles(&instance, &physicalDevice, &device,
            &queue, &queueFamily, &queueIndex))) {
        SetError(20, "Failed to query the DXVK Vulkan handles");
        return false;
    }

    if (m_hasVulkanEnable2 && p_xrGetVulkanGraphicsRequirements2KHR) {
        XrGraphicsRequirementsVulkanKHR requirements = { XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR };
        if (XR_FAILED(p_xrGetVulkanGraphicsRequirements2KHR(m_instance, m_systemId, &requirements))) {
            SetError(21, "xrGetVulkanGraphicsRequirements2KHR failed");
            return false;
        }
    } else if (p_xrGetVulkanGraphicsRequirementsKHR) {
        XrGraphicsRequirementsVulkanKHR requirements = { XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR };
        if (XR_FAILED(p_xrGetVulkanGraphicsRequirementsKHR(m_instance, m_systemId, &requirements))) {
            SetError(21, "xrGetVulkanGraphicsRequirementsKHR failed");
            return false;
        }
    } else {
        SetError(21, "No Vulkan graphics requirements entry point");
        return false;
    }

    // Query the runtime's preferred physical device.  The mod uses DXVK's
    // device regardless (they must match for xrCreateSession to succeed), but
    // XR_KHR_vulkan_enable expects this call before session creation.
    if (m_hasVulkanEnable2 && p_xrGetVulkanGraphicsDevice2KHR) {
        VkPhysicalDevice runtimeDevice = VK_NULL_HANDLE;
        XrVulkanGraphicsDeviceGetInfoKHR getInfo = { XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR };
        getInfo.systemId = m_systemId;
        getInfo.vulkanInstance = reinterpret_cast<VkInstance>(static_cast<uintptr_t>(instance));
        const XrResult deviceResult = p_xrGetVulkanGraphicsDevice2KHR(m_instance, &getInfo, &runtimeDevice);
        if (XR_SUCCEEDED(deviceResult) && runtimeDevice &&
            reinterpret_cast<uintptr_t>(runtimeDevice) != static_cast<uintptr_t>(physicalDevice)) {
            Log("[OpenXR] runtime physical device differs from DXVK; using DXVK device");
        }
    } else if (p_xrGetVulkanGraphicsDeviceKHR) {
        VkPhysicalDevice runtimeDevice = VK_NULL_HANDLE;
        const XrResult deviceResult = p_xrGetVulkanGraphicsDeviceKHR(m_instance, m_systemId,
            reinterpret_cast<VkInstance>(static_cast<uintptr_t>(instance)), &runtimeDevice);
        if (XR_SUCCEEDED(deviceResult) && runtimeDevice &&
            reinterpret_cast<uintptr_t>(runtimeDevice) != static_cast<uintptr_t>(physicalDevice)) {
            Log("[OpenXR] runtime physical device differs from DXVK; using DXVK device");
        }
    }

    XrGraphicsBindingVulkanKHR binding = { XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR };
    binding.instance = reinterpret_cast<VkInstance>(static_cast<uintptr_t>(instance));
    binding.physicalDevice = reinterpret_cast<VkPhysicalDevice>(static_cast<uintptr_t>(physicalDevice));
    binding.device = reinterpret_cast<VkDevice>(static_cast<uintptr_t>(device));
    binding.queueFamilyIndex = queueFamily;
    binding.queueIndex = queueIndex;

    XrSessionCreateInfo sessionInfo = {};
    sessionInfo.type = XR_TYPE_SESSION_CREATE_INFO;
    sessionInfo.next = &binding;
    sessionInfo.systemId = m_systemId;

    const XrResult result = p_xrCreateSession(m_instance, &sessionInfo, &m_session);
    if (XR_FAILED(result)) {
        SetError(22, "xrCreateSession failed (%d). The runtime rejected the DXVK Vulkan device.", (int)result);
        return false;
    }

    XrSessionActionSetsAttachInfo attach = {};
    attach.type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO;
    XrActionSet sets[2] = { m_mainActionSet, m_baseActionSet };
    attach.countActionSets = 2;
    attach.actionSets = sets;
    if (XR_FAILED(p_xrAttachSessionActionSets(m_session, &attach)))
        Log("[OpenXR] xrAttachSessionActionSets failed");

    uint32_t formatCount = 0;
    if (XR_SUCCEEDED(p_xrEnumerateSwapchainFormats(m_session, 0, &formatCount, nullptr)) && formatCount > 0) {
        m_swapchainFormats.resize(formatCount);
        if (XR_SUCCEEDED(p_xrEnumerateSwapchainFormats(m_session, formatCount, &formatCount, m_swapchainFormats.data())))
            m_swapchainFormats.resize(formatCount);
    }
    m_colorFormat = ChooseColorFormat();

    if (!CreateReferenceSpaces())
        return false;
    if (!CreateActionSpaces())
        return false;
    if (!CreateEyeSwapchains())
        return false;

    if (m_hasDisplayRefreshRate)
        LoadOptionalInstanceFunction(m_instance, "xrGetDisplayRefreshRateFB", p_xrGetDisplayRefreshRateFB);

    // Wait for READY, then begin.  The frame thread is started afterwards.
    const auto readyDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < readyDeadline) {
        PumpEvents();
        if (m_sessionRunning.load())
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!m_sessionRunning.load()) {
        XrSessionBeginInfo beginInfo = {};
        beginInfo.type = XR_TYPE_SESSION_BEGIN_INFO;
        beginInfo.primaryViewConfigurationType = m_viewConfigType;
        if (XR_FAILED(p_xrBeginSession(m_session, &beginInfo))) {
            SetError(23, "xrBeginSession failed; session state never reached READY");
            return false;
        }
        m_sessionRunning.store(true);
    }

    m_stopFrameThread.store(false);
    m_frameThread = std::thread(&Backend::FrameThreadMain, this);

    {
        std::unique_lock<std::mutex> lock(m_poseMutex);
        m_poseCv.wait_for(lock, std::chrono::seconds(2), [&] {
            return m_poseSnapshot.seq != 0 || m_stopFrameThread.load();
        });
    }

    Log("[OpenXR] session running (%ux%u format=%lld)", m_renderWidth, m_renderHeight,
        (long long)m_colorFormat);
    return true;
}

bool Backend::CreateSessionFromD3D9() {
    if (m_session != XR_NULL_HANDLE)
        return true;
    if (!m_initialized.load()) {
        SetError(24, "OpenXR backend is not initialized");
        return false;
    }
    return CreateSession();
}

bool Backend::CreateReferenceSpaces() {
    XrReferenceSpaceCreateInfo info = {};
    info.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
    info.poseInReferenceSpace = kIdentityPose;

    info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    XrResult result = p_xrCreateReferenceSpace(m_session, &info, &m_stageSpace);
    if (XR_FAILED(result)) {
        Log("[OpenXR] stage space unavailable (%d); using local space", (int)result);
        info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        result = p_xrCreateReferenceSpace(m_session, &info, &m_stageSpace);
        m_trackingOrigin = xr::TrackingUniverseSeated;
    } else {
        m_trackingOrigin = xr::TrackingUniverseStanding;
    }
    if (XR_FAILED(result)) {
        SetError(25, "xrCreateReferenceSpace(stage/local) failed");
        return false;
    }

    info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    if (XR_FAILED(p_xrCreateReferenceSpace(m_session, &info, &m_viewSpace))) {
        Log("[OpenXR] view space unavailable; HMD-relative overlays use stage space");
        m_viewSpace = XR_NULL_HANDLE;
    }
    return true;
}

bool Backend::CreateActionSpaces() {
    XrActionSpaceCreateInfo info = {};
    info.type = XR_TYPE_ACTION_SPACE_CREATE_INFO;
    info.poseInActionSpace = kIdentityPose;

    for (int hand = 0; hand < 2; ++hand) {
        const XrPath path = hand == 0 ? m_leftPath : m_rightPath;
        if (m_gripPoseActions[hand] && m_gripPoseActions[hand]->handle != XR_NULL_HANDLE) {
            info.action = m_gripPoseActions[hand]->handle;
            info.subactionPath = path;
            if (XR_FAILED(p_xrCreateActionSpace(m_session, &info, &m_gripSpaces[hand])))
                Log("[OpenXR] grip action space %d failed", hand);
        }
        if (m_aimPoseActions[hand] && m_aimPoseActions[hand]->handle != XR_NULL_HANDLE) {
            info.action = m_aimPoseActions[hand]->handle;
            info.subactionPath = path;
            if (XR_FAILED(p_xrCreateActionSpace(m_session, &info, &m_aimSpaces[hand])))
                Log("[OpenXR] aim action space %d failed", hand);
        }
    }
    return true;
}

bool Backend::CreateEyeSwapchains() {
    const uint32_t maxW = m_maxSwapchainWidth ? m_maxSwapchainWidth : m_renderWidth;
    const uint32_t maxH = m_maxSwapchainHeight ? m_maxSwapchainHeight : m_renderHeight;
    const uint32_t width = (std::min)(m_renderWidth, maxW);
    const uint32_t height = (std::min)(m_renderHeight, maxH);

    for (int eye = 0; eye < 2; ++eye) {
        EyeSwapchain& sc = m_eyeSwapchains[eye];
        XrSwapchainCreateInfo info = {};
        info.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
        info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                          XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        info.format = m_colorFormat;
        info.sampleCount = 1;
        info.width = width;
        info.height = height;
        info.faceCount = 1;
        info.arraySize = 1;
        info.mipCount = 1;

        XrResult result = p_xrCreateSwapchain(m_session, &info, &sc.handle);
        if (XR_FAILED(result)) {
            SetError(26, "xrCreateSwapchain(eye %d) failed (%d)", eye, (int)result);
            return false;
        }
        sc.width = width;
        sc.height = height;
        sc.format = m_colorFormat;

        uint32_t imageCount = 0;
        if (XR_FAILED(p_xrEnumerateSwapchainImages(sc.handle, 0, &imageCount, nullptr)) || imageCount == 0) {
            SetError(26, "xrEnumerateSwapchainImages(eye %d) failed", eye);
            return false;
        }
        XrSwapchainImageVulkanKHR imageTemplate = {};
        imageTemplate.type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
        sc.images.assign(imageCount, imageTemplate);
        if (XR_FAILED(p_xrEnumerateSwapchainImages(sc.handle, imageCount, &imageCount,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(sc.images.data())))) {
            SetError(26, "xrEnumerateSwapchainImages(eye %d) query failed", eye);
            return false;
        }
        sc.images.resize(imageCount);
    }

    Log("[OpenXR] eye swapchains %ux%u format=%lld", width, height, (long long)m_colorFormat);
    return true;
}

std::shared_ptr<OverlaySwapchain> Backend::CreateOverlaySwapchain(uint32_t width, uint32_t height) {
    if (m_session == XR_NULL_HANDLE || width == 0 || height == 0)
        return nullptr;

    const uint32_t maxW = m_maxSwapchainWidth ? m_maxSwapchainWidth : width;
    const uint32_t maxH = m_maxSwapchainHeight ? m_maxSwapchainHeight : height;
    width = (std::min)(width, maxW);
    height = (std::min)(height, maxH);

    auto sc = std::make_shared<OverlaySwapchain>();
    XrSwapchainCreateInfo info = {};
    info.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                      XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    info.format = m_colorFormat;
    info.sampleCount = 1;
    info.width = width;
    info.height = height;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;

    if (XR_FAILED(p_xrCreateSwapchain(m_session, &info, &sc->handle)))
        return nullptr;
    sc->width = width;
    sc->height = height;
    sc->format = m_colorFormat;

    uint32_t imageCount = 0;
    if (XR_FAILED(p_xrEnumerateSwapchainImages(sc->handle, 0, &imageCount, nullptr)) || imageCount == 0) {
        p_xrDestroySwapchain(sc->handle);
        return nullptr;
    }
    XrSwapchainImageVulkanKHR imageTemplate = {};
    imageTemplate.type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
    sc->images.assign(imageCount, imageTemplate);
    if (XR_FAILED(p_xrEnumerateSwapchainImages(sc->handle, imageCount, &imageCount,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(sc->images.data())))) {
        p_xrDestroySwapchain(sc->handle);
        return nullptr;
    }
    sc->images.resize(imageCount);
    return sc;
}

void Backend::DestroyEyeSwapchain(EyeSwapchain& sc) {
    if (sc.handle != XR_NULL_HANDLE) {
        p_xrDestroySwapchain(sc.handle);
        sc.handle = XR_NULL_HANDLE;
    }
    sc.images.clear();
}

void Backend::DestroyOverlaySwapchain(std::shared_ptr<OverlaySwapchain>& sc) {
    if (sc && sc->handle != XR_NULL_HANDLE) {
        p_xrDestroySwapchain(sc->handle);
        sc->handle = XR_NULL_HANDLE;
    }
    sc.reset();
}

void Backend::Shutdown() {
    if (!m_initialized.load() && m_instance == XR_NULL_HANDLE)
        return;

    m_stopFrameThread.store(true);
    m_jobCv.notify_all();
    m_poseCv.notify_all();
    m_frameCv.notify_all();

    if (m_frameThread.joinable())
        m_frameThread.join();

    for (auto& sc : m_eyeSwapchains)
        DestroyEyeSwapchain(sc);

    for (auto& overlay : m_overlays) {
        ReleaseOverlayRaw(overlay);
        DestroyOverlaySwapchain(overlay.swapchain);
    }
    m_overlays.clear();
    for (auto& retired : m_retiredRawTextures) {
        if (retired.first)
            retired.first->Release();
        if (retired.second)
            retired.second->Release();
    }
    m_retiredRawTextures.clear();

    for (auto& space : m_gripSpaces) {
        if (space != XR_NULL_HANDLE)
            p_xrDestroySpace(space);
        space = XR_NULL_HANDLE;
    }
    for (auto& space : m_aimSpaces) {
        if (space != XR_NULL_HANDLE)
            p_xrDestroySpace(space);
        space = XR_NULL_HANDLE;
    }
    if (m_stageSpace != XR_NULL_HANDLE) {
        p_xrDestroySpace(m_stageSpace);
        m_stageSpace = XR_NULL_HANDLE;
    }
    if (m_viewSpace != XR_NULL_HANDLE) {
        p_xrDestroySpace(m_viewSpace);
        m_viewSpace = XR_NULL_HANDLE;
    }

    for (auto& action : m_actions) {
        if (action->handle != XR_NULL_HANDLE)
            p_xrDestroyAction(action->handle);
        if (action->handleBoolean != XR_NULL_HANDLE)
            p_xrDestroyAction(action->handleBoolean);
    }
    m_actions.clear();
    m_actionHandles.clear();

    if (m_session != XR_NULL_HANDLE) {
        if (m_sessionRunning.exchange(false))
            p_xrEndSession(m_session);
        p_xrDestroySession(m_session);
        m_session = XR_NULL_HANDLE;
    }
    if (m_mainActionSet != XR_NULL_HANDLE) {
        p_xrDestroyActionSet(m_mainActionSet);
        m_mainActionSet = XR_NULL_HANDLE;
    }
    if (m_baseActionSet != XR_NULL_HANDLE) {
        p_xrDestroyActionSet(m_baseActionSet);
        m_baseActionSet = XR_NULL_HANDLE;
    }
    if (m_instance != XR_NULL_HANDLE) {
        p_xrDestroyInstance(m_instance);
        m_instance = XR_NULL_HANDLE;
    }

    m_initialized.store(false);
    m_sessionRunning.store(false);
}

// ---------------------------------------------------------------------------
// Backend: frame thread
// ---------------------------------------------------------------------------

void Backend::PumpEvents() {
    if (m_instance == XR_NULL_HANDLE)
        return;

    XrEventDataBuffer event = { XR_TYPE_EVENT_DATA_BUFFER };
    while (p_xrPollEvent && XR_SUCCEEDED(p_xrPollEvent(m_instance, &event))) {
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto* changed = reinterpret_cast<XrEventDataSessionStateChanged*>(&event);
            switch (changed->state) {
            case XR_SESSION_STATE_READY:
                if (!m_sessionRunning.load()) {
                    XrSessionBeginInfo beginInfo = {};
                    beginInfo.type = XR_TYPE_SESSION_BEGIN_INFO;
                    beginInfo.primaryViewConfigurationType = m_viewConfigType;
                    if (XR_SUCCEEDED(p_xrBeginSession(m_session, &beginInfo))) {
                        m_sessionRunning.store(true);
                        Log("[OpenXR] session READY -> running");
                    }
                }
                break;
            case XR_SESSION_STATE_STOPPING:
                if (m_sessionRunning.exchange(false)) {
                    p_xrEndSession(m_session);
                    Log("[OpenXR] session STOPPING -> ended");
                }
                break;
            case XR_SESSION_STATE_EXITING:
            case XR_SESSION_STATE_LOSS_PENDING:
                m_sessionRunning.store(false);
                break;
            default:
                break;
            }
        }
        event = { XR_TYPE_EVENT_DATA_BUFFER };
    }
}

void Backend::PublishPoseSnapshot(XrTime predictedDisplayTime, const XrView* views, uint32_t viewCount, const XrViewState& viewState) {
    PoseSnapshot snapshot = {};
    snapshot.predictedDisplayTime = predictedDisplayTime;
    snapshot.wallTime = std::chrono::steady_clock::now();
    snapshot.viewCount = (std::min)(viewCount, 2u);

    for (uint32_t i = 0; i < snapshot.viewCount; ++i) {
        snapshot.views[i] = views[i];
        snapshot.views[i].type = XR_TYPE_VIEW;
        snapshot.views[i].next = nullptr;
    }

    XrPosef headPose = kIdentityPose;
    bool headValid = false;
    if ((viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0 && snapshot.viewCount >= 2) {
        headPose.position = {
            (snapshot.views[0].pose.position.x + snapshot.views[1].pose.position.x) * 0.5f,
            (snapshot.views[0].pose.position.y + snapshot.views[1].pose.position.y) * 0.5f,
            (snapshot.views[0].pose.position.z + snapshot.views[1].pose.position.z) * 0.5f,
        };
        headPose.orientation = snapshot.views[0].pose.orientation;
        headValid = true;
    } else if (snapshot.viewCount == 1) {
        headPose = snapshot.views[0].pose;
        headValid = true;
    }

    snapshot.valid = headValid;
    xr::TrackedDevicePose_t& hmd = snapshot.poses[xr::k_unTrackedDeviceIndex_Hmd];
    hmd.bPoseIsValid = headValid;
    hmd.bDeviceIsConnected = true;
    hmd.eTrackingResult = 0;
    if (headValid) {
        hmd.mDeviceToAbsoluteTracking = PoseToMatrix(headPose);
        for (int eye = 0; eye < 2; ++eye) {
            if ((uint32_t)eye >= snapshot.viewCount)
                continue;
            snapshot.eyeToHead[eye] = MultiplyPose(InversePose(headPose), snapshot.views[eye].pose);
        }
    }

    for (int hand = 0; hand < 2; ++hand) {
        HandInput& out = snapshot.hands[hand];
        xr::TrackedDevicePose_t& pose = snapshot.poses[hand == 0 ? 1 : 2];
        pose.bPoseIsValid = false;
        pose.bDeviceIsConnected = false;
        pose.eTrackingResult = 1;

        if (m_gripSpaces[hand] != XR_NULL_HANDLE) {
            XrSpaceLocation location = { XR_TYPE_SPACE_LOCATION };
            XrSpaceVelocity velocity = { XR_TYPE_SPACE_VELOCITY };
            location.next = &velocity;
            if (XR_SUCCEEDED(p_xrLocateSpace(m_gripSpaces[hand], m_stageSpace, predictedDisplayTime, &location))) {
                const XrSpaceLocationFlags needed =
                    XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
                if ((location.locationFlags & needed) == needed) {
                    out.poseValid = true;
                    out.gripPose = location.pose;
                    out.linearVelocity = velocity.linearVelocity;
                    out.angularVelocity = velocity.angularVelocity;
                    pose.bPoseIsValid = true;
                    pose.bDeviceIsConnected = true;
                    pose.eTrackingResult = 0;
                    pose.mDeviceToAbsoluteTracking = PoseToMatrix(location.pose);
                    pose.vVelocity = { velocity.linearVelocity.x, velocity.linearVelocity.y, velocity.linearVelocity.z };
                    pose.vAngularVelocity = { velocity.angularVelocity.x, velocity.angularVelocity.y, velocity.angularVelocity.z };
                }
            }
        }

        if (m_aimSpaces[hand] != XR_NULL_HANDLE) {
            XrSpaceLocation location = { XR_TYPE_SPACE_LOCATION };
            if (XR_SUCCEEDED(p_xrLocateSpace(m_aimSpaces[hand], m_stageSpace, predictedDisplayTime, &location))) {
                const XrSpaceLocationFlags needed =
                    XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
                if ((location.locationFlags & needed) == needed) {
                    out.aimValid = true;
                    out.aimPose = location.pose;
                }
            }
            if (m_gripSpaces[hand] != XR_NULL_HANDLE) {
                XrSpaceLocation relative = { XR_TYPE_SPACE_LOCATION };
                if (XR_SUCCEEDED(p_xrLocateSpace(m_aimSpaces[hand], m_gripSpaces[hand], predictedDisplayTime, &relative))) {
                    const XrSpaceLocationFlags needed =
                        XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
                    if ((relative.locationFlags & needed) == needed)
                        out.aimInGrip = relative.pose;
                }
            }
        }
    }

    {
        std::lock_guard<std::mutex> inputLock(m_inputMutex);
        for (int hand = 0; hand < 2; ++hand) {
            snapshot.hands[hand].trigger = m_handInputCache[hand].trigger;
            snapshot.hands[hand].grip = m_handInputCache[hand].grip;
            snapshot.hands[hand].stickX = m_handInputCache[hand].stickX;
            snapshot.hands[hand].stickY = m_handInputCache[hand].stickY;
            snapshot.hands[hand].primary = m_handInputCache[hand].primary;
            snapshot.hands[hand].secondary = m_handInputCache[hand].secondary;
            snapshot.hands[hand].stickClick = m_handInputCache[hand].stickClick;
        }

        std::lock_guard<std::mutex> poseLock(m_poseMutex);
        snapshot.seq = m_poseSnapshot.seq + 1;
        m_poseSnapshot = snapshot;
    }
    m_poseCv.notify_all();
}

void Backend::FrameThreadMain() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    while (!m_stopFrameThread.load(std::memory_order_acquire)) {
        PumpEvents();

        if (!m_sessionRunning.load(std::memory_order_acquire) || m_session == XR_NULL_HANDLE) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        XrFrameWaitInfo waitInfo = {};
        waitInfo.type = XR_TYPE_FRAME_WAIT_INFO;
        XrFrameState frameState = { XR_TYPE_FRAME_STATE };
        const XrResult waitResult = p_xrWaitFrame(m_session, &waitInfo, &frameState);
        if (XR_FAILED(waitResult)) {
            if (waitResult == XR_ERROR_SESSION_NOT_RUNNING)
                m_sessionRunning.store(false);
            else
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

        const auto now = std::chrono::steady_clock::now();
        static auto lastFrameTime = now;
        const float interval = std::chrono::duration<float>(now - lastFrameTime).count();
        lastFrameTime = now;
        if (interval > 0.001f && interval < 0.25f)
            m_lastFrameIntervalSeconds.store(interval, std::memory_order_relaxed);

        XrFrameBeginInfo beginInfo = {};
        beginInfo.type = XR_TYPE_FRAME_BEGIN_INFO;
        const XrResult beginResult = p_xrBeginFrame(m_session, &beginInfo);

        XrViewLocateInfo locateInfo = {};
        locateInfo.type = XR_TYPE_VIEW_LOCATE_INFO;
        locateInfo.viewConfigurationType = m_viewConfigType;
        locateInfo.displayTime = frameState.predictedDisplayTime;
        locateInfo.space = m_stageSpace;

        std::array<XrView, 2> views = {};
        for (auto& view : views) {
            view = {};
            view.type = XR_TYPE_VIEW;
        }
        XrViewState viewState = {};
        viewState.type = XR_TYPE_VIEW_STATE;
        uint32_t viewCount = static_cast<uint32_t>(views.size());
        const XrResult locateResult = p_xrLocateViews(m_session, &locateInfo, &viewState,
            static_cast<uint32_t>(views.size()), &viewCount, views.data());
        if (XR_FAILED(locateResult))
            viewCount = 0;

        PublishPoseSnapshot(frameState.predictedDisplayTime, views.data(), viewCount, viewState);

        if (!frameState.shouldRender || XR_FAILED(beginResult)) {
            if (XR_SUCCEEDED(beginResult)) {
                XrFrameEndInfo endInfo = {};
                endInfo.type = XR_TYPE_FRAME_END_INFO;
                endInfo.displayTime = frameState.predictedDisplayTime;
                endInfo.environmentBlendMode = m_blendMode;
                p_xrEndFrame(m_session, &endInfo);
            }
            m_frameIndex.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        FrameJob job;
        const bool haveJob = WaitForFrameJob(job, std::chrono::milliseconds(10));

        {
            std::lock_guard<std::mutex> frameLock(m_frameMutex);
            m_frameInFlight = true;
        }

        CopyAndSubmitFrame(haveJob ? &job : nullptr, views.data(), viewCount, frameState.predictedDisplayTime);

        m_frameIndex.fetch_add(1, std::memory_order_relaxed);
        if (haveJob)
            CompleteFrameJob(job.id);

        {
            std::lock_guard<std::mutex> frameLock(m_frameMutex);
            m_frameInFlight = false;
        }
        m_frameCv.notify_all();
    }
}

bool Backend::WaitForFrameJob(FrameJob& job, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(m_jobMutex);
    if (!m_jobCv.wait_for(lock, timeout, [&] { return m_jobPending || m_stopFrameThread.load(); }))
        return false;
    if (!m_jobPending)
        return false;
    job = m_pendingJob;
    m_pendingJob = {};
    m_jobPending = false;
    return true;
}

void Backend::CompleteFrameJob(uint64_t jobId) {
    {
        std::lock_guard<std::mutex> lock(m_jobMutex);
        if (jobId > m_completedJobId)
            m_completedJobId = jobId;
    }
    m_jobDoneCv.notify_all();
}

void Backend::WaitForFrameIdle() {
    std::unique_lock<std::mutex> lock(m_frameMutex);
    m_frameCv.wait_for(lock, std::chrono::milliseconds(100), [&] { return !m_frameInFlight; });
}

void Backend::BuildOverlaySnapshots(std::vector<FrameOverlaySnapshot>& out) {
    std::lock_guard<std::mutex> lock(m_overlayMutex);
    out.reserve(m_overlays.size());
    for (const auto& overlay : m_overlays) {
        if (!overlay.visible || !overlay.hasTexture || !overlay.swapchain)
            continue;
        FrameOverlaySnapshot snapshot;
        snapshot.state = overlay;
        snapshot.swapchain = overlay.swapchain;
        out.push_back(std::move(snapshot));
    }
}

bool Backend::ResolveOverlayPose(const OverlayState& overlay, const PoseSnapshot& snapshot, XrPosef& pose) const {
    if (overlay.transformType == xr::VROverlayTransform_Absolute) {
        pose = MatrixToPose(overlay.transform);
        return true;
    }
    if (overlay.transformType == xr::VROverlayTransform_TrackedDeviceRelative) {
        const xr::TrackedDeviceIndex_t device = overlay.transformDevice;
        const XrPosef relative = MatrixToPose(overlay.transform);
        if (device == xr::k_unTrackedDeviceIndex_Hmd) {
            if (!snapshot.poses[xr::k_unTrackedDeviceIndex_Hmd].bPoseIsValid)
                return false;
            pose = MultiplyPose(MatrixToPose(snapshot.poses[xr::k_unTrackedDeviceIndex_Hmd].mDeviceToAbsoluteTracking), relative);
            return true;
        }
        if (device == 1 || device == 2) {
            const int hand = device == 1 ? 0 : 1;
            if (!snapshot.hands[hand].poseValid)
                return false;
            pose = MultiplyPose(snapshot.hands[hand].gripPose, relative);
            return true;
        }
    }
    return false;
}

bool Backend::IntersectOverlay(const OverlayState& overlay, const PoseSnapshot& snapshot, const XrPosef& rayPose, XrVector3f& hitPoint, float& distance, float& u, float& v) const {
    XrPosef overlayPose = kIdentityPose;
    if (!ResolveOverlayPose(overlay, snapshot, overlayPose))
        return false;

    const XrPosef rayInOverlay = MultiplyPose(InversePose(overlayPose), rayPose);
    const XrVector3f dir = RotateVector(rayInOverlay.orientation, { 0.0f, 0.0f, -1.0f });
    if (std::fabs(dir.z) < 1e-6f)
        return false;

    const float t = -rayInOverlay.position.z / dir.z;
    if (t <= 0.0f)
        return false;

    const XrVector3f local = {
        rayInOverlay.position.x + dir.x * t,
        rayInOverlay.position.y + dir.y * t,
        0.0f,
    };

    float regionW = 1.0f, regionH = 1.0f;
    if (overlay.hasTexture && overlay.texture.m_nWidth && overlay.texture.m_nHeight) {
        regionW = std::fabs(overlay.bounds.uMax - overlay.bounds.uMin) * float(overlay.texture.m_nWidth);
        regionH = std::fabs(overlay.bounds.vMax - overlay.bounds.vMin) * float(overlay.texture.m_nHeight);
    }
    regionW = (std::max)(1.0f, regionW);
    regionH = (std::max)(1.0f, regionH);

    const float width = overlay.widthMeters > 0.0f ? overlay.widthMeters : 1.0f;
    const float height = width * (regionH / regionW) * (overlay.texelAspect > 0.0f ? overlay.texelAspect : 1.0f);
    const float halfW = width * 0.5f;
    const float halfH = height * 0.5f;

    if (std::fabs(local.x) > halfW || std::fabs(local.y) > halfH)
        return false;

    hitPoint = TransformPoint(overlayPose, local);
    distance = t;
    u = std::clamp((local.x + halfW) / width, 0.0f, 1.0f);
    v = std::clamp((halfH - local.y) / height, 0.0f, 1.0f);
    return true;
}

void Backend::GenerateOverlayEvents(OverlayState& overlay, const PoseSnapshot& snapshot) {
    int bestHand = -1;
    float bestDistance = 1e30f;
    float bestU = 0.0f, bestV = 0.0f;

    for (int hand = 0; hand < 2; ++hand) {
        if (!snapshot.hands[hand].aimValid)
            continue;
        XrVector3f point = {};
        float distance = 0.0f, u = 0.0f, v = 0.0f;
        if (!IntersectOverlay(overlay, snapshot, snapshot.hands[hand].aimPose, point, distance, u, v))
            continue;
        if (distance < bestDistance) {
            bestDistance = distance;
            bestHand = hand;
            bestU = u;
            bestV = v;
        }
    }

    const bool hovered = bestHand >= 0;
    const float mouseX = bestU * (overlay.mouseScale.v[0] > 0.0f ? overlay.mouseScale.v[0] : 1.0f);
    const float mouseY = (1.0f - bestV) * (overlay.mouseScale.v[1] > 0.0f ? overlay.mouseScale.v[1] : 1.0f);

    if (hovered && (!overlay.pointerHovered || overlay.pointerHand != bestHand ||
            std::fabs(bestU - overlay.pointerU) > 0.0005f ||
            std::fabs(bestV - overlay.pointerV) > 0.0005f)) {
        xr::VREvent_t event = {};
        event.eventType = xr::VREvent_MouseMove;
        event.trackedDeviceIndex = bestHand == 0 ? 1 : 2;
        event.data.mouse.x = mouseX;
        event.data.mouse.y = mouseY;
        overlay.events.push_back(event);
    }

    const float trigger = hovered ? snapshot.hands[bestHand].trigger : 0.0f;
    const bool down = hovered && trigger >= 0.65f;
    if (down && !overlay.pointerDown) {
        xr::VREvent_t event = {};
        event.eventType = xr::VREvent_MouseButtonDown;
        event.trackedDeviceIndex = bestHand == 0 ? 1 : 2;
        event.data.mouse.x = mouseX;
        event.data.mouse.y = mouseY;
        overlay.events.push_back(event);
        overlay.pointerDown = true;
    } else if (!down && overlay.pointerDown) {
        xr::VREvent_t event = {};
        event.eventType = xr::VREvent_MouseButtonUp;
        event.trackedDeviceIndex = overlay.pointerHand == 0 ? 1 : 2;
        event.data.mouse.x = mouseX;
        event.data.mouse.y = mouseY;
        overlay.events.push_back(event);
        overlay.pointerDown = false;
    }

    const bool scrollUp = hovered && snapshot.hands[1].stickY > 0.7f;
    const bool scrollDown = hovered && snapshot.hands[1].stickY < -0.7f;
    if (scrollUp && !overlay.scrollUpPrev) {
        xr::VREvent_t event = {};
        event.eventType = xr::VREvent_ScrollDiscrete;
        event.data.scroll.ydelta = 1.0f;
        overlay.events.push_back(event);
    } else if (scrollDown && !overlay.scrollDownPrev) {
        xr::VREvent_t event = {};
        event.eventType = xr::VREvent_ScrollDiscrete;
        event.data.scroll.ydelta = -1.0f;
        overlay.events.push_back(event);
    }
    overlay.scrollUpPrev = scrollUp;
    overlay.scrollDownPrev = scrollDown;

    const bool facePressed = hovered && snapshot.hands[1].primary;
    if (facePressed && !overlay.facePressedPrev) {
        xr::VREvent_t event = {};
        event.eventType = xr::VREvent_ButtonPress;
        event.trackedDeviceIndex = 2;
        event.data.controller.button = xr::k_EButton_IndexController_A;
        overlay.events.push_back(event);
    }
    overlay.facePressedPrev = facePressed;

    overlay.pointerHovered = hovered;
    overlay.pointerU = bestU;
    overlay.pointerV = bestV;
    overlay.pointerHand = bestHand;

    if (overlay.events.size() > 32)
        overlay.events.erase(overlay.events.begin(), overlay.events.begin() + (overlay.events.size() - 32));
}

void Backend::CopyAndSubmitFrame(const FrameJob* job, const XrView* views, uint32_t viewCount, XrTime predictedDisplayTime) {
    if (m_session == XR_NULL_HANDLE)
        return;

    PoseSnapshot snapshot;
    {
        std::lock_guard<std::mutex> lock(m_poseMutex);
        snapshot = m_poseSnapshot;
    }

    std::vector<FrameOverlaySnapshot> overlaySnapshots;
    BuildOverlaySnapshots(overlaySnapshots);
    std::stable_sort(overlaySnapshots.begin(), overlaySnapshots.end(),
        [](const FrameOverlaySnapshot& a, const FrameOverlaySnapshot& b) {
            return a.state.sortOrder < b.state.sortOrder;
        });

    struct Acquired {
        XrSwapchain swapchain = XR_NULL_HANDLE;
        uint32_t index = 0;
        bool ok = false;
    };

    std::array<Acquired, 2> eyeImages = {};
    std::vector<Acquired> overlayImages(overlaySnapshots.size());
    std::vector<D3D9_VR_BLIT_DESC> blits;

    auto acquire = [&](XrSwapchain swapchain, Acquired& out) -> bool {
        out.swapchain = swapchain;
        XrSwapchainImageAcquireInfo acquireInfo = {};
        acquireInfo.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
        if (XR_FAILED(p_xrAcquireSwapchainImage(swapchain, &acquireInfo, &out.index)))
            return false;
        XrSwapchainImageWaitInfo waitInfo = {};
        waitInfo.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
        waitInfo.timeout = XR_INFINITE_DURATION;
        if (XR_FAILED(p_xrWaitSwapchainImage(swapchain, &waitInfo)))
            return false;
        out.ok = true;
        return true;
    };

    if (job) {
        for (int eye = 0; eye < 2; ++eye) {
            if (!job->eyes[eye].valid || m_eyeSwapchains[eye].images.empty())
                continue;
            if (!acquire(m_eyeSwapchains[eye].handle, eyeImages[eye]))
                continue;

            const auto& src = job->eyes[eye].texture;
            const XrSwapchainImageVulkanKHR& dst = m_eyeSwapchains[eye].images[eyeImages[eye].index];
            float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
            if (job->eyes[eye].hasBounds) {
                u0 = std::clamp((std::min)(job->eyes[eye].bounds.uMin, job->eyes[eye].bounds.uMax), 0.0f, 1.0f);
                u1 = std::clamp((std::max)(job->eyes[eye].bounds.uMin, job->eyes[eye].bounds.uMax), 0.0f, 1.0f);
                v0 = std::clamp((std::min)(job->eyes[eye].bounds.vMin, job->eyes[eye].bounds.vMax), 0.0f, 1.0f);
                v1 = std::clamp((std::max)(job->eyes[eye].bounds.vMin, job->eyes[eye].bounds.vMax), 0.0f, 1.0f);
            }

            D3D9_VR_BLIT_DESC desc = {};
            desc.SrcImage = src.m_nImage;
            desc.SrcWidth = src.m_nWidth;
            desc.SrcHeight = src.m_nHeight;
            desc.SrcFormat = (VkFormat)src.m_nFormat;
            desc.SrcLayout = (VkImageLayout)src.m_nImageLayout;
            desc.SrcUsage = (VkImageUsageFlags)src.m_nImageUsage;
            desc.SrcU0 = u0;
            desc.SrcV0 = v0;
            desc.SrcU1 = u1;
            desc.SrcV1 = v1;
            desc.DstImage = HandleToU64(dst.image);
            desc.DstWidth = m_eyeSwapchains[eye].width;
            desc.DstHeight = m_eyeSwapchains[eye].height;
            desc.DstFormat = (VkFormat)m_eyeSwapchains[eye].format;
            if (desc.SrcImage && desc.SrcWidth && desc.SrcHeight)
                blits.push_back(desc);
        }
    }

    for (size_t i = 0; i < overlaySnapshots.size(); ++i) {
        FrameOverlaySnapshot& overlay = overlaySnapshots[i];
        if (!overlay.swapchain || overlay.swapchain->handle == XR_NULL_HANDLE || overlay.swapchain->images.empty())
            continue;
        if (!acquire(overlay.swapchain->handle, overlayImages[i]))
            continue;

        const auto& src = overlay.state.texture;
        const XrSwapchainImageVulkanKHR& dst = overlay.swapchain->images[overlayImages[i].index];
        D3D9_VR_BLIT_DESC desc = {};
        desc.SrcImage = src.m_nImage;
        desc.SrcWidth = src.m_nWidth;
        desc.SrcHeight = src.m_nHeight;
        desc.SrcFormat = (VkFormat)src.m_nFormat;
        desc.SrcLayout = (VkImageLayout)src.m_nImageLayout;
        desc.SrcUsage = (VkImageUsageFlags)src.m_nImageUsage;
        desc.SrcU0 = 0.0f;
        desc.SrcV0 = 0.0f;
        desc.SrcU1 = 1.0f;
        desc.SrcV1 = 1.0f;
        desc.DstImage = HandleToU64(dst.image);
        desc.DstWidth = overlay.swapchain->width;
        desc.DstHeight = overlay.swapchain->height;
        desc.DstFormat = (VkFormat)overlay.swapchain->format;
        if (desc.SrcImage && desc.SrcWidth && desc.SrcHeight)
            blits.push_back(desc);
    }

    if (!blits.empty() && g_D3DVR9) {
        const HRESULT hr = g_D3DVR9->BlitImages(blits.data(), (uint32_t)blits.size());
        if (FAILED(hr))
            Log("[OpenXR] BlitImages failed 0x%08lX", (unsigned long)hr);
    }

    for (auto& image : overlayImages) {
        if (image.ok) {
            XrSwapchainImageReleaseInfo releaseInfo = {};
            releaseInfo.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
            p_xrReleaseSwapchainImage(image.swapchain, &releaseInfo);
        }
    }
    for (auto& image : eyeImages) {
        if (image.ok) {
            XrSwapchainImageReleaseInfo releaseInfo = {};
            releaseInfo.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
            p_xrReleaseSwapchainImage(image.swapchain, &releaseInfo);
        }
    }

    std::vector<XrCompositionLayerProjectionView> projectionViews;
    XrCompositionLayerProjection projection = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    const bool haveProjection = job && eyeImages[0].ok && eyeImages[1].ok;
    if (haveProjection) {
        projectionViews.resize(2);
        for (int eye = 0; eye < 2; ++eye) {
            XrCompositionLayerProjectionView& pv = projectionViews[eye];
            pv = {};
            pv.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
            pv.pose = (eye < (int)viewCount) ? views[eye].pose : kIdentityPose;
            pv.fov = (eye < (int)viewCount) ? views[eye].fov : XrFovf{ -0.785f, 0.785f, 0.785f, -0.785f };
            if (job->eyes[eye].hasRenderPose) {
                const XrPosef renderPose = MatrixToPose(job->eyes[eye].renderPose);
                pv.pose = MultiplyPose(renderPose, snapshot.eyeToHead[eye]);
            }
            pv.subImage.swapchain = m_eyeSwapchains[eye].handle;
            pv.subImage.imageRect.offset = { 0, 0 };
            pv.subImage.imageRect.extent = { (int32_t)m_eyeSwapchains[eye].width, (int32_t)m_eyeSwapchains[eye].height };
            pv.subImage.imageArrayIndex = 0;
        }
        projection.space = m_stageSpace;
        projection.viewCount = 2;
        projection.views = projectionViews.data();
    }

    std::vector<XrCompositionLayerQuad> quads;
    quads.reserve(overlaySnapshots.size());
    for (size_t i = 0; i < overlaySnapshots.size(); ++i) {
        FrameOverlaySnapshot& overlay = overlaySnapshots[i];
        if (!overlayImages[i].ok || !overlay.swapchain)
            continue;

        XrPosef pose = kIdentityPose;
        if (!ResolveOverlayPose(overlay.state, snapshot, pose))
            continue;
        if (m_stageSpace == XR_NULL_HANDLE)
            continue;

        XrCompositionLayerQuad quad = { XR_TYPE_COMPOSITION_LAYER_QUAD };
        quad.space = m_stageSpace;
        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quad.subImage.swapchain = overlay.swapchain->handle;
        const float u0 = std::clamp((std::min)(overlay.state.bounds.uMin, overlay.state.bounds.uMax), 0.0f, 1.0f);
        const float u1 = std::clamp((std::max)(overlay.state.bounds.uMin, overlay.state.bounds.uMax), 0.0f, 1.0f);
        const float v0 = std::clamp((std::min)(overlay.state.bounds.vMin, overlay.state.bounds.vMax), 0.0f, 1.0f);
        const float v1 = std::clamp((std::max)(overlay.state.bounds.vMin, overlay.state.bounds.vMax), 0.0f, 1.0f);
        quad.subImage.imageRect.offset = {
            (int32_t)(u0 * overlay.swapchain->width),
            (int32_t)(v0 * overlay.swapchain->height)
        };
        quad.subImage.imageRect.extent = {
            (std::max)(1, (int32_t)((u1 - u0) * overlay.swapchain->width)),
            (std::max)(1, (int32_t)((v1 - v0) * overlay.swapchain->height))
        };
        quad.subImage.imageArrayIndex = 0;
        quad.pose = pose;

        const float regionW = (float)quad.subImage.imageRect.extent.width;
        const float regionH = (float)quad.subImage.imageRect.extent.height;
        const float width = overlay.state.widthMeters > 0.0f ? overlay.state.widthMeters : 1.0f;
        const float texelAspect = overlay.state.texelAspect > 0.0f ? overlay.state.texelAspect : 1.0f;
        quad.size.width = width;
        quad.size.height = width * (regionH / (std::max)(1.0f, regionW)) * texelAspect;

        if ((overlay.state.flags & xr::VROverlayFlags_IgnoreTextureAlpha) == 0) {
            quad.layerFlags |= XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            if ((overlay.state.flags & xr::VROverlayFlags_IsPremultiplied) == 0)
                quad.layerFlags |= XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
        }
        quads.push_back(quad);
    }

    std::vector<const XrCompositionLayerBaseHeader*> layers;
    layers.reserve(1 + quads.size());
    if (haveProjection)
        layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection));
    for (auto& quad : quads)
        layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad));

    XrFrameEndInfo endInfo = {};
    endInfo.type = XR_TYPE_FRAME_END_INFO;
    endInfo.displayTime = predictedDisplayTime;
    endInfo.environmentBlendMode = m_blendMode;
    endInfo.layerCount = (uint32_t)layers.size();
    endInfo.layers = layers.empty() ? nullptr : layers.data();
    const XrResult endResult = p_xrEndFrame(m_session, &endInfo);
    if (XR_FAILED(endResult))
        Log("[OpenXR] xrEndFrame failed (%d) layers=%u", (int)endResult, (unsigned)layers.size());
}

// ---------------------------------------------------------------------------
// Backend: system / compositor
// ---------------------------------------------------------------------------

void Backend::GetRecommendedRenderTargetSize(uint32_t* w, uint32_t* h) const {
    if (w) *w = m_renderWidth;
    if (h) *h = m_renderHeight;
}

void Backend::GetProjectionRaw(int eye, float* left, float* right, float* top, float* bottom) const {
    float l = -0.785398f, r = 0.785398f, t = -0.785398f, b = 0.785398f;
    PoseSnapshot snapshot;
    {
        std::lock_guard<std::mutex> lock(m_poseMutex);
        snapshot = m_poseSnapshot;
    }
    if (snapshot.valid && eye >= 0 && eye < 2 && (uint32_t)eye < snapshot.viewCount) {
        const XrFovf& fov = snapshot.views[eye].fov;
        l = std::tan(fov.angleLeft);
        r = std::tan(fov.angleRight);
        t = -std::tan(fov.angleUp);
        b = -std::tan(fov.angleDown);
    }
    if (left) *left = l;
    if (right) *right = r;
    if (top) *top = t;
    if (bottom) *bottom = b;
}

xr::HmdMatrix34_t Backend::GetEyeToHeadTransform(int eye) const {
    PoseSnapshot snapshot;
    {
        std::lock_guard<std::mutex> lock(m_poseMutex);
        snapshot = m_poseSnapshot;
    }
    return PoseToMatrix((eye >= 0 && eye < 2) ? snapshot.eyeToHead[eye] : kIdentityPose);
}

xr::TrackedDeviceIndex_t Backend::GetTrackedDeviceIndexForControllerRole(int role) const {
    if (role == xr::TrackedControllerRole_LeftHand) return 1;
    if (role == xr::TrackedControllerRole_RightHand) return 2;
    return xr::k_unTrackedDeviceIndexInvalid;
}

void Backend::GetDeviceToAbsoluteTrackingPose(int origin, float predictedSeconds, xr::TrackedDevicePose_t* poses, uint32_t count) {
    (void)origin;
    if (!poses || !count)
        return;

    PoseSnapshot snapshot;
    {
        std::lock_guard<std::mutex> lock(m_poseMutex);
        snapshot = m_poseSnapshot;
    }

    const float dt = std::clamp(predictedSeconds, 0.0f, 0.05f);
    const uint32_t maxCount = (std::min)(count, xr::k_unMaxTrackedDeviceCount);
    for (uint32_t i = 0; i < maxCount; ++i)
        poses[i] = snapshot.poses[i];

    if (dt > 0.0f) {
        for (uint32_t i = 0; i < maxCount; ++i) {
            if (!poses[i].bPoseIsValid)
                continue;
            poses[i].mDeviceToAbsoluteTracking.m[0][3] += poses[i].vVelocity.v[0] * dt;
            poses[i].mDeviceToAbsoluteTracking.m[1][3] += poses[i].vVelocity.v[1] * dt;
            poses[i].mDeviceToAbsoluteTracking.m[2][3] += poses[i].vVelocity.v[2] * dt;
        }
    }
}

bool Backend::IsTrackedDeviceConnected(xr::TrackedDeviceIndex_t index) {
    if (index == xr::k_unTrackedDeviceIndex_Hmd)
        return true;
    PoseSnapshot snapshot;
    {
        std::lock_guard<std::mutex> lock(m_poseMutex);
        snapshot = m_poseSnapshot;
    }
    return index < xr::k_unMaxTrackedDeviceCount && snapshot.poses[index].bDeviceIsConnected;
}

void Backend::QueryRefreshRate() {
    const uint32_t now = GetTickCount();
    const uint32_t last = m_refreshRateQueryMs.load(std::memory_order_relaxed);
    if (last != 0 && now - last < 2000)
        return;
    m_refreshRateQueryMs.store(now, std::memory_order_relaxed);

    float rate = 0.0f;
    if (m_session != XR_NULL_HANDLE && m_sessionRunning.load() && p_xrGetDisplayRefreshRateFB) {
        if (XR_SUCCEEDED(p_xrGetDisplayRefreshRateFB(m_session, &rate)) && rate > 1.0f && rate < 1000.0f)
            m_refreshRateHz.store(rate, std::memory_order_relaxed);
    }
}

float Backend::GetFloatTrackedDeviceProperty(xr::TrackedDeviceIndex_t index, xr::ETrackedDeviceProperty prop, xr::ETrackedPropertyError* error) {
    (void)index;
    if (prop == xr::Prop_DisplayFrequency_Float) {
        QueryRefreshRate();
        if (error) *error = xr::TrackedProp_Success;
        const float rate = m_refreshRateHz.load(std::memory_order_relaxed);
        return (rate > 1.0f) ? rate : 90.0f;
    }
    if (error) *error = xr::TrackedProp_UnknownProperty;
    return 0.0f;
}

uint32_t Backend::GetStringTrackedDeviceProperty(xr::TrackedDeviceIndex_t index, xr::ETrackedDeviceProperty prop, char* buffer, uint32_t bufferSize, xr::ETrackedPropertyError* error) {
    (void)index;
    const char* value = nullptr;
    if (prop == xr::Prop_RenderModelName_String)
        value = "openxr_controller";
    else if (prop == xr::Prop_ModelNumber_String)
        value = "OpenXR";
    else if (prop == xr::Prop_SerialNumber_String)
        value = "OpenXR";

    if (!value) {
        if (error) *error = xr::TrackedProp_UnknownProperty;
        return 0;
    }
    if (error) *error = xr::TrackedProp_Success;
    const uint32_t length = (uint32_t)std::strlen(value) + 1;
    if (buffer && bufferSize) {
        const uint32_t copy = (std::min)(length, bufferSize);
        std::memcpy(buffer, value, copy);
        buffer[copy - 1] = '\0';
    }
    return length;
}

bool Backend::ApplyHaptic(int hand, uint16_t durationMicroSec) {
    if (hand < 0 || hand > 1 || !m_sessionRunning.load() ||
        !m_vibrationActions[hand] || m_vibrationActions[hand]->handle == XR_NULL_HANDLE)
        return false;

    XrHapticActionInfo info = {};
    info.type = XR_TYPE_HAPTIC_ACTION_INFO;
    info.action = m_vibrationActions[hand]->handle;
    info.subactionPath = hand == 0 ? m_leftPath : m_rightPath;

    XrHapticVibration vibration = { XR_TYPE_HAPTIC_VIBRATION };
    vibration.duration = (XrDuration)durationMicroSec * 1000;
    vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
    vibration.amplitude = 1.0f;

    XrResult result = XR_ERROR_FUNCTION_UNSUPPORTED;
    {
        std::lock_guard<std::mutex> lock(m_xrCallMutex);
        result = p_xrApplyHapticFeedback(m_session, &info, reinterpret_cast<XrHapticBaseHeader*>(&vibration));
    }
    return XR_SUCCEEDED(result);
}

void Backend::TriggerHapticPulse(xr::TrackedDeviceIndex_t index, uint32_t axisId, uint16_t durationMicroSec) {
    (void)axisId;
    if (index == 1)
        ApplyHaptic(0, durationMicroSec);
    else if (index == 2)
        ApplyHaptic(1, durationMicroSec);
}

xr::EVRCompositorError Backend::WaitGetPoses(xr::TrackedDevicePose_t* renderPoses, uint32_t renderCount, xr::TrackedDevicePose_t* gamePoses, uint32_t gameCount) {
    PoseSnapshot snapshot;
    {
        std::unique_lock<std::mutex> lock(m_poseMutex);
        m_poseCv.wait_for(lock, std::chrono::milliseconds(100), [&] {
            return m_poseSnapshot.seq != m_lastWaitPoseSeq || m_stopFrameThread.load();
        });
        snapshot = m_poseSnapshot;
        m_lastWaitPoseSeq = snapshot.seq;
    }

    if (!snapshot.valid || snapshot.seq == 0)
        return xr::VRCompositorError_RequestFailed;

    if (renderPoses && renderCount) {
        const uint32_t count = (std::min)(renderCount, xr::k_unMaxTrackedDeviceCount);
        std::memcpy(renderPoses, snapshot.poses, sizeof(xr::TrackedDevicePose_t) * count);
        for (uint32_t i = count; i < renderCount; ++i)
            std::memset(&renderPoses[i], 0, sizeof(xr::TrackedDevicePose_t));
    }
    if (gamePoses && gameCount) {
        const uint32_t count = (std::min)(gameCount, xr::k_unMaxTrackedDeviceCount);
        std::memcpy(gamePoses, snapshot.poses, sizeof(xr::TrackedDevicePose_t) * count);
    }
    return xr::VRCompositorError_None;
}

xr::EVRCompositorError Backend::Submit(int eye, const xr::Texture_t* texture, const xr::VRTextureBounds_t* bounds, int flags) {
    if (eye < 0 || eye > 1 || !texture || !texture->handle)
        return xr::VRCompositorError_RequestFailed;

    uint64_t jobId = 0;
    {
        std::lock_guard<std::mutex> lock(m_submitMutex);
        FrameEyeTexture& staged = m_stagedEyes[eye];
        staged = {};
        staged.valid = true;
        staged.texture = *reinterpret_cast<const xr::VRVulkanTextureData_t*>(texture->handle);
        if (!staged.texture.m_nImage || !staged.texture.m_nWidth || !staged.texture.m_nHeight) {
            m_stagedEyeValid[eye] = false;
            return xr::VRCompositorError_RequestFailed;
        }
        if (bounds) {
            staged.bounds = *bounds;
            staged.hasBounds = true;
        } else {
            staged.bounds = { 0.0f, 0.0f, 1.0f, 1.0f };
            staged.hasBounds = false;
        }
        if (flags & xr::Submit_TextureWithPose) {
            const auto* withPose = reinterpret_cast<const xr::VRTextureWithPose_t*>(texture);
            staged.renderPose = withPose->mDeviceToAbsoluteTracking;
            staged.hasRenderPose = true;
        }
        m_stagedEyeValid[eye] = true;

        if (eye == 0)
            return xr::VRCompositorError_None;
        if (!m_stagedEyeValid[0])
            return xr::VRCompositorError_RequestFailed;

        {
            std::lock_guard<std::mutex> jobLock(m_jobMutex);
            if (m_jobPending)
                return xr::VRCompositorError_AlreadySubmitted;

            m_pendingJob = {};
            m_pendingJob.id = m_nextJobId++;
            m_pendingJob.eyes[0] = m_stagedEyes[0];
            m_pendingJob.eyes[1] = m_stagedEyes[1];
            jobId = m_pendingJob.id;
            m_jobPending = true;
        }
        m_stagedEyeValid[0] = false;
        m_stagedEyeValid[1] = false;
        m_jobCv.notify_all();
    }

    std::unique_lock<std::mutex> lock(m_jobMutex);
    const bool completed = m_jobDoneCv.wait_for(lock, std::chrono::milliseconds(250), [&] {
        return m_completedJobId >= jobId || m_stopFrameThread.load();
    });
    return completed ? xr::VRCompositorError_None : xr::VRCompositorError_RequestFailed;
}

bool Backend::GetFrameTiming(xr::Compositor_FrameTiming* timing, uint32_t framesAgo) {
    (void)framesAgo;
    if (!timing)
        return false;
    std::memset(timing, 0, sizeof(*timing));
    timing->m_nSize = sizeof(*timing);
    timing->m_nFrameIndex = m_frameIndex.load(std::memory_order_relaxed);
    return timing->m_nFrameIndex != 0;
}

float Backend::GetFrameTimeRemaining() {
    return std::clamp(m_lastFrameIntervalSeconds.load(std::memory_order_relaxed) * 0.5f, 0.0f, 0.030f);
}

xr::ETrackingUniverseOrigin Backend::GetTrackingSpace() const {
    return m_trackingOrigin;
}

void Backend::SetExplicitTimingMode(int mode) { (void)mode; }
void Backend::SubmitExplicitTimingData() {}
void Backend::PostPresentHandoff() {}
void Backend::ClearLastSubmittedFrame() {}

// ---------------------------------------------------------------------------
// Backend: input
// ---------------------------------------------------------------------------

xr::EVRInputError Backend::GetActionHandle(const char* path, xr::VRActionHandle_t* handle) {
    if (!handle || !path)
        return xr::VRInputError_InvalidParam;
    auto it = m_actionHandles.find(path);
    if (it == m_actionHandles.end()) {
        *handle = xr::k_ulInvalidActionHandle;
        return xr::VRInputError_NameNotFound;
    }
    *handle = it->second;
    return xr::VRInputError_None;
}

xr::EVRInputError Backend::GetActionSetHandle(const char* path, xr::VRActionSetHandle_t* handle) {
    if (!handle || !path)
        return xr::VRInputError_InvalidParam;
    if (std::strcmp(path, "/actions/main") == 0) {
        *handle = 1;
        return xr::VRInputError_None;
    }
    if (std::strcmp(path, "/actions/base") == 0) {
        *handle = 2;
        return xr::VRInputError_None;
    }
    *handle = xr::k_ulInvalidActionSetHandle;
    return xr::VRInputError_NameNotFound;
}

bool Backend::QueryBoolean(XrAction action, XrPath subactionPath, bool& value, bool& changed, bool& active) {
    value = false;
    changed = false;
    active = false;
    if (action == XR_NULL_HANDLE || !m_sessionRunning.load())
        return false;

    XrActionStateGetInfo getInfo = {};
    getInfo.type = XR_TYPE_ACTION_STATE_GET_INFO;
    getInfo.action = action;
    getInfo.subactionPath = subactionPath;

    XrActionStateBoolean state = { XR_TYPE_ACTION_STATE_BOOLEAN };
    XrResult result = XR_ERROR_SESSION_NOT_RUNNING;
    {
        std::lock_guard<std::mutex> lock(m_xrCallMutex);
        result = p_xrGetActionStateBoolean(m_session, &getInfo, &state);
    }
    if (XR_FAILED(result))
        return false;

    value = state.currentState != XR_FALSE;
    changed = state.changedSinceLastSync != XR_FALSE;
    active = state.isActive != XR_FALSE;
    return true;
}

bool Backend::QueryFloat(XrAction action, XrPath subactionPath, float& value, bool& active) {
    value = 0.0f;
    active = false;
    if (action == XR_NULL_HANDLE || !m_sessionRunning.load())
        return false;

    XrActionStateGetInfo getInfo = {};
    getInfo.type = XR_TYPE_ACTION_STATE_GET_INFO;
    getInfo.action = action;
    getInfo.subactionPath = subactionPath;

    XrActionStateFloat state = { XR_TYPE_ACTION_STATE_FLOAT };
    XrResult result = XR_ERROR_SESSION_NOT_RUNNING;
    {
        std::lock_guard<std::mutex> lock(m_xrCallMutex);
        result = p_xrGetActionStateFloat(m_session, &getInfo, &state);
    }
    if (XR_FAILED(result))
        return false;
    value = state.currentState;
    active = state.isActive != XR_FALSE;
    return true;
}

bool Backend::QueryVector2(XrAction action, XrPath subactionPath, XrVector2f& value, bool& active) {
    value = { 0.0f, 0.0f };
    active = false;
    if (action == XR_NULL_HANDLE || !m_sessionRunning.load())
        return false;

    XrActionStateGetInfo getInfo = {};
    getInfo.type = XR_TYPE_ACTION_STATE_GET_INFO;
    getInfo.action = action;
    getInfo.subactionPath = subactionPath;

    XrActionStateVector2f state = { XR_TYPE_ACTION_STATE_VECTOR2F };
    XrResult result = XR_ERROR_SESSION_NOT_RUNNING;
    {
        std::lock_guard<std::mutex> lock(m_xrCallMutex);
        result = p_xrGetActionStateVector2f(m_session, &getInfo, &state);
    }
    if (XR_FAILED(result))
        return false;
    value = state.currentState;
    active = state.isActive != XR_FALSE;
    return true;
}

xr::EVRInputError Backend::UpdateActionState() {
    std::lock_guard<std::mutex> inputLock(m_inputMutex);
    if (!m_sessionRunning.load() || m_session == XR_NULL_HANDLE)
        return xr::VRInputError_NoActiveActionSet;

    {
        std::lock_guard<std::mutex> lock(m_xrCallMutex);
        XrActiveActionSet sets[2] = {};
        sets[0].actionSet = m_mainActionSet;
        sets[1].actionSet = m_baseActionSet;
        XrActionsSyncInfo syncInfo = {};
        syncInfo.type = XR_TYPE_ACTIONS_SYNC_INFO;
        syncInfo.countActiveActionSets = 2;
        syncInfo.activeActionSets = sets;
        if (XR_FAILED(p_xrSyncActions(m_session, &syncInfo)))
            return xr::VRInputError_NoActiveActionSet;
    }

    for (auto& actionPtr : m_actions) {
        Action& action = *actionPtr;
        action.digital = {};
        action.analog = {};
        action.skeletalActive = false;

        if (action.isVirtual || action.handle == XR_NULL_HANDLE)
            continue;

        if (action.kind == ActionKind::Boolean) {
            bool lv = false, lc = false, la = false;
            bool rv = false, rc = false, ra = false;
            QueryBoolean(action.handle, m_leftPath, lv, lc, la);
            QueryBoolean(action.handle, m_rightPath, rv, rc, ra);
            action.digital.bState = lv || rv;
            action.digital.bChanged = lc || rc;
            action.digital.bActive = la || ra;
            action.digital.activeOrigin = rv ? xr::k_unInputSourceRight :
                (lv ? xr::k_unInputSourceLeft : (rc ? xr::k_unInputSourceRight : xr::k_unInputSourceLeft));
        } else if (action.kind == ActionKind::FloatDigital) {
            float lf = 0.0f, rf = 0.0f;
            bool lfa = false, rfa = false;
            QueryFloat(action.handle, m_leftPath, lf, lfa);
            QueryFloat(action.handle, m_rightPath, rf, rfa);

            bool lb = false, lc = false, lba = false;
            bool rb = false, rc = false, rba = false;
            if (action.handleBoolean != XR_NULL_HANDLE) {
                QueryBoolean(action.handleBoolean, m_leftPath, lb, lc, lba);
                QueryBoolean(action.handleBoolean, m_rightPath, rb, rc, rba);
            }

            const bool lDown = (lfa && lf >= action.floatThreshold) || lb;
            const bool rDown = (rfa && rf >= action.floatThreshold) || rb;
            const bool down = lDown || rDown;
            action.digital.bState = down;
            action.digital.bChanged = (down != action.prevDigital) || lc || rc;
            action.digital.bActive = lfa || rfa || lba || rba;
            action.digital.activeOrigin = rDown ? xr::k_unInputSourceRight :
                (lDown ? xr::k_unInputSourceLeft : (rc ? xr::k_unInputSourceRight : xr::k_unInputSourceLeft));
            action.analog.x = (std::fabs(rf) >= std::fabs(lf)) ? rf : lf;
            action.analog.bActive = action.digital.bActive;
            action.analog.activeOrigin = action.digital.activeOrigin;
            action.prevDigital = down;
        } else if (action.kind == ActionKind::Float) {
            float lf = 0.0f, rf = 0.0f;
            bool la = false, ra = false;
            QueryFloat(action.handle, m_leftPath, lf, la);
            QueryFloat(action.handle, m_rightPath, rf, ra);
            if (std::fabs(rf) >= std::fabs(lf)) {
                action.analog.x = rf;
                action.analog.bActive = ra;
                action.analog.activeOrigin = xr::k_unInputSourceRight;
            } else {
                action.analog.x = lf;
                action.analog.bActive = la;
                action.analog.activeOrigin = xr::k_unInputSourceLeft;
            }
        } else if (action.kind == ActionKind::Vector2f) {
            XrVector2f lv = {}, rv = {};
            bool la = false, ra = false;
            QueryVector2(action.handle, m_leftPath, lv, la);
            QueryVector2(action.handle, m_rightPath, rv, ra);
            const float lm = lv.x * lv.x + lv.y * lv.y;
            const float rm = rv.x * rv.x + rv.y * rv.y;
            if (rm >= lm) {
                action.analog.x = rv.x;
                action.analog.y = rv.y;
                action.analog.bActive = ra;
                action.analog.activeOrigin = xr::k_unInputSourceRight;
            } else {
                action.analog.x = lv.x;
                action.analog.y = lv.y;
                action.analog.bActive = la;
                action.analog.activeOrigin = xr::k_unInputSourceLeft;
            }
        } else if (action.kind == ActionKind::Skeleton) {
            const bool left = action.path.find("lefthand") != std::string::npos;
            PoseSnapshot snapshot;
            {
                std::lock_guard<std::mutex> poseLock(m_poseMutex);
                snapshot = m_poseSnapshot;
            }
            const HandInput& hand = snapshot.hands[left ? 0 : 1];
            action.skeletalActive = hand.poseValid;
            const float trigger = std::clamp(hand.trigger, 0.0f, 1.0f);
            const float grip = std::clamp(hand.grip, 0.0f, 1.0f);
            action.skeletalSummary.flFingerCurl[0] = std::clamp(0.15f + grip * 0.35f, 0.0f, 1.0f);
            action.skeletalSummary.flFingerCurl[1] = trigger;
            action.skeletalSummary.flFingerCurl[2] = std::clamp(0.10f + grip * 0.75f, 0.0f, 1.0f);
            action.skeletalSummary.flFingerCurl[3] = std::clamp(0.10f + grip * 0.70f, 0.0f, 1.0f);
            action.skeletalSummary.flFingerCurl[4] = std::clamp(0.10f + grip * 0.65f, 0.0f, 1.0f);
        }
    }

    UpdateHandInput();
    UpdateVirtualActions();
    return xr::VRInputError_None;
}

void Backend::UpdateHandInput() {
    for (int hand = 0; hand < 2; ++hand) {
        HandInput& input = m_handInputCache[hand];
        input.trigger = 0.0f;
        input.grip = 0.0f;
        input.stickX = 0.0f;
        input.stickY = 0.0f;
        input.primary = false;
        input.secondary = false;
        input.stickClick = false;

        if (m_triggerActions[hand] && m_triggerActions[hand]->handle != XR_NULL_HANDLE) {
            float value = 0.0f;
            bool active = false;
            QueryFloat(m_triggerActions[hand]->handle, hand == 0 ? m_leftPath : m_rightPath, value, active);
            if (active)
                input.trigger = std::clamp(value, 0.0f, 1.0f);
        }
        if (m_gripActions[hand] && m_gripActions[hand]->handle != XR_NULL_HANDLE) {
            float value = 0.0f;
            bool active = false;
            QueryFloat(m_gripActions[hand]->handle, hand == 0 ? m_leftPath : m_rightPath, value, active);
            if (active)
                input.grip = std::clamp(value, 0.0f, 1.0f);
        }

        // Fallbacks for runtimes/profiles without analog squeeze.
        if (input.trigger <= 0.0f) {
            Action* triggerAction = hand == 0 ? m_secondaryAction : m_primaryAction;
            if (triggerAction && triggerAction->analog.bActive)
                input.trigger = std::clamp(triggerAction->analog.x, 0.0f, 1.0f);
        }
        if (input.grip <= 0.0f) {
            Action* gripAction = hand == 0 ? m_reloadAction : m_crouchAction;
            if (gripAction && gripAction->digital.bState)
                input.grip = 1.0f;
        }

        if (hand == 0 && m_walkAction) {
            input.stickX = m_walkAction->analog.x;
            input.stickY = m_walkAction->analog.y;
            input.stickClick = m_resetAction && m_resetAction->digital.bState;
        }
        if (hand == 1 && m_turnAction) {
            input.stickX = m_turnAction->analog.x;
            input.stickY = m_turnAction->analog.y;
            input.stickClick = m_flashlightAction && m_flashlightAction->digital.bState;
        }

        if (hand == 0) {
            input.primary = m_scoreboardAction && m_scoreboardAction->digital.bState;
            input.secondary = m_pauseAction && m_pauseAction->digital.bState;
        } else {
            input.primary = m_activateAction && m_activateAction->digital.bState;
            input.secondary = m_useAction && m_useAction->digital.bState;
        }
    }

    std::lock_guard<std::mutex> poseLock(m_poseMutex);
    for (int hand = 0; hand < 2; ++hand) {
        m_poseSnapshot.hands[hand].trigger = m_handInputCache[hand].trigger;
        m_poseSnapshot.hands[hand].grip = m_handInputCache[hand].grip;
        m_poseSnapshot.hands[hand].stickX = m_handInputCache[hand].stickX;
        m_poseSnapshot.hands[hand].stickY = m_handInputCache[hand].stickY;
        m_poseSnapshot.hands[hand].primary = m_handInputCache[hand].primary;
        m_poseSnapshot.hands[hand].secondary = m_handInputCache[hand].secondary;
        m_poseSnapshot.hands[hand].stickClick = m_handInputCache[hand].stickClick;
    }
}

void Backend::UpdateVirtualActions() {
    const float leftX = m_walkAction ? m_walkAction->analog.x : 0.0f;
    const float leftY = m_walkAction ? m_walkAction->analog.y : 0.0f;
    const float rightX = m_turnAction ? m_turnAction->analog.x : 0.0f;
    const float rightY = m_turnAction ? m_turnAction->analog.y : 0.0f;

    struct Update {
        const char* path;
        bool state;
        uint64_t origin;
    };
    const Update updates[] = {
        { "/actions/main/in/MenuUp", leftY > 0.7f, xr::k_unInputSourceLeft },
        { "/actions/main/in/MenuDown", leftY < -0.7f, xr::k_unInputSourceLeft },
        { "/actions/main/in/MenuLeft", leftX < -0.7f, xr::k_unInputSourceLeft },
        { "/actions/main/in/MenuRight", leftX > 0.7f, xr::k_unInputSourceLeft },
        { "/actions/main/in/PrevItem", rightY > 0.7f, xr::k_unInputSourceRight },
        { "/actions/main/in/NextItem", rightY < -0.7f, xr::k_unInputSourceRight },
        { "/actions/main/in/boolean_turnleft", rightX < -0.7f, xr::k_unInputSourceRight },
        { "/actions/main/in/boolean_turnright", rightX > 0.7f, xr::k_unInputSourceRight },
    };

    for (const auto& update : updates) {
        Action* action = FindAction(update.path);
        if (!action || !action->isVirtual)
            continue;
        action->virtualPrevState = action->virtualState;
        action->virtualState = update.state;
        action->digital.bState = update.state;
        action->digital.bChanged = action->virtualState != action->virtualPrevState;
        action->digital.bActive = true;
        action->digital.activeOrigin = update.origin;
    }
}

xr::EVRInputError Backend::GetDigitalActionData(xr::VRActionHandle_t handle, xr::InputDigitalActionData_t* data, uint32_t size, xr::VRInputValueHandle_t restrictToDevice) {
    if (!data || size < sizeof(xr::InputDigitalActionData_t))
        return xr::VRInputError_InvalidParam;
    std::lock_guard<std::mutex> lock(m_inputMutex);
    Action* action = FindActionByHandle(handle);
    if (!action)
        return xr::VRInputError_InvalidHandle;
    xr::InputDigitalActionData_t out = action->digital;
    if ((restrictToDevice == xr::k_unInputSourceLeft || restrictToDevice == xr::k_unInputSourceRight) &&
        out.activeOrigin != 0 && out.activeOrigin != restrictToDevice) {
        out.bState = false;
        out.bChanged = false;
        out.bActive = false;
    }
    *data = out;
    return xr::VRInputError_None;
}

xr::EVRInputError Backend::GetAnalogActionData(xr::VRActionHandle_t handle, xr::InputAnalogActionData_t* data, uint32_t size, xr::VRInputValueHandle_t restrictToDevice) {
    if (!data || size < sizeof(xr::InputAnalogActionData_t))
        return xr::VRInputError_InvalidParam;
    std::lock_guard<std::mutex> lock(m_inputMutex);
    Action* action = FindActionByHandle(handle);
    if (!action)
        return xr::VRInputError_InvalidHandle;
    xr::InputAnalogActionData_t out = action->analog;
    if ((restrictToDevice == xr::k_unInputSourceLeft || restrictToDevice == xr::k_unInputSourceRight) &&
        out.activeOrigin != 0 && out.activeOrigin != restrictToDevice) {
        out.x = out.y = out.z = 0.0f;
        out.bActive = false;
    }
    *data = out;
    return xr::VRInputError_None;
}

xr::EVRInputError Backend::GetSkeletalActionData(xr::VRActionHandle_t handle, xr::InputSkeletalActionData_t* data, uint32_t size) {
    if (!data || size < sizeof(xr::InputSkeletalActionData_t))
        return xr::VRInputError_InvalidParam;
    std::lock_guard<std::mutex> lock(m_inputMutex);
    Action* action = FindActionByHandle(handle);
    if (!action || action->kind != ActionKind::Skeleton)
        return xr::VRInputError_InvalidHandle;
    const bool left = action->path.find("lefthand") != std::string::npos;
    data->bActive = action->skeletalActive;
    data->activeOrigin = left ? xr::k_unInputSourceLeft : xr::k_unInputSourceRight;
    return xr::VRInputError_None;
}

xr::EVRInputError Backend::GetSkeletalSummaryData(xr::VRActionHandle_t handle, int summaryType, xr::VRSkeletalSummaryData_t* data) {
    (void)summaryType;
    if (!data)
        return xr::VRInputError_InvalidParam;
    std::lock_guard<std::mutex> lock(m_inputMutex);
    Action* action = FindActionByHandle(handle);
    if (!action || action->kind != ActionKind::Skeleton)
        return xr::VRInputError_InvalidHandle;
    *data = action->skeletalSummary;
    return xr::VRInputError_None;
}

xr::EVRInputError Backend::GetSkeletalBoneData(xr::VRActionHandle_t handle, int transformSpace, int motionRange, xr::VRBoneTransform_t* transforms, uint32_t count) {
    (void)handle; (void)transformSpace; (void)motionRange; (void)transforms; (void)count;
    return xr::VRInputError_Unsupported;
}

xr::EVRInputError Backend::GetSkeletalReferenceTransforms(xr::VRActionHandle_t handle, int transformSpace, int referencePose, xr::VRBoneTransform_t* transforms, uint32_t count) {
    (void)handle; (void)transformSpace; (void)referencePose; (void)transforms; (void)count;
    return xr::VRInputError_Unsupported;
}

xr::EVRInputError Backend::GetBoneCount(xr::VRActionHandle_t handle, uint32_t* boneCount) {
    (void)handle;
    if (boneCount)
        *boneCount = 0;
    return xr::VRInputError_Unsupported;
}

xr::EVRInputError Backend::GetBoneHierarchy(xr::VRActionHandle_t handle, xr::BoneIndex_t* parentIndices, uint32_t count) {
    (void)handle; (void)parentIndices; (void)count;
    return xr::VRInputError_Unsupported;
}

xr::EVRInputError Backend::GetBoneName(xr::VRActionHandle_t handle, xr::BoneIndex_t boneIndex, char* name, uint32_t nameSize) {
    (void)handle; (void)boneIndex;
    if (name && nameSize)
        name[0] = '\0';
    return xr::VRInputError_Unsupported;
}

xr::EVRInputError Backend::GetInputSourceHandle(const char* path, xr::VRInputValueHandle_t* handle) {
    if (!handle || !path)
        return xr::VRInputError_InvalidParam;
    if (std::strcmp(path, "/user/hand/left") == 0) {
        *handle = xr::k_unInputSourceLeft;
        return xr::VRInputError_None;
    }
    if (std::strcmp(path, "/user/hand/right") == 0) {
        *handle = xr::k_unInputSourceRight;
        return xr::VRInputError_None;
    }
    if (std::strcmp(path, "/user/head") == 0) {
        *handle = xr::k_unInputSourceHead;
        return xr::VRInputError_None;
    }
    *handle = xr::k_ulInvalidInputValueHandle;
    return xr::VRInputError_NameNotFound;
}

xr::EVRInputError Backend::GetActionBindingInfo(xr::VRActionHandle_t handle, xr::InputBindingInfo_t* bindings, uint32_t bindingSize, uint32_t bindingCount, uint32_t* returnedCount) {
    if (returnedCount)
        *returnedCount = 0;
    if (!bindings || bindingSize < sizeof(xr::InputBindingInfo_t))
        return xr::VRInputError_InvalidParam;
    Action* action = FindActionByHandle(handle);
    if (!action)
        return xr::VRInputError_InvalidHandle;
    uint32_t written = 0;
    for (const auto& info : action->bindingInfo) {
        if (written >= bindingCount)
            break;
        std::memset(&bindings[written], 0, sizeof(bindings[written]));
        std::snprintf(bindings[written].rchDevicePathName, sizeof(bindings[written].rchDevicePathName), "%s", info.devicePath.c_str());
        std::snprintf(bindings[written].rchInputPathName, sizeof(bindings[written].rchInputPathName), "%s", info.inputPath.c_str());
        std::snprintf(bindings[written].rchModeName, sizeof(bindings[written].rchModeName), "%s", info.mode.c_str());
        std::snprintf(bindings[written].rchSlotName, sizeof(bindings[written].rchSlotName), "%s", info.slot.c_str());
        std::snprintf(bindings[written].rchInputSourceType, sizeof(bindings[written].rchInputSourceType), "%s", info.sourceType.c_str());
        ++written;
    }
    if (returnedCount)
        *returnedCount = written;
    return xr::VRInputError_None;
}

xr::EVRInputError Backend::GetOriginTrackedDeviceInfo(xr::VRInputValueHandle_t origin, xr::InputOriginInfo_t* info, uint32_t size) {
    if (!info || size < sizeof(xr::InputOriginInfo_t))
        return xr::VRInputError_InvalidParam;
    std::memset(info, 0, sizeof(*info));
    if (origin == xr::k_unInputSourceLeft) {
        info->trackedDeviceIndex = 1;
        std::snprintf(info->rchRenderModelComponentName, sizeof(info->rchRenderModelComponentName), "left");
        return xr::VRInputError_None;
    }
    if (origin == xr::k_unInputSourceRight) {
        info->trackedDeviceIndex = 2;
        std::snprintf(info->rchRenderModelComponentName, sizeof(info->rchRenderModelComponentName), "right");
        return xr::VRInputError_None;
    }
    return xr::VRInputError_NameNotFound;
}

// ---------------------------------------------------------------------------
// Backend: overlays
// ---------------------------------------------------------------------------

OverlayState* FindOverlayByHandle(std::vector<OverlayState>& overlays, xr::VROverlayHandle_t handle) {
    for (auto& overlay : overlays) {
        if (overlay.handle == handle)
            return &overlay;
    }
    return nullptr;
}

void ReleaseOverlayRaw(OverlayState& overlay) {
    if (overlay.rawSurface) {
        overlay.rawSurface->Release();
        overlay.rawSurface = nullptr;
    }
    if (overlay.rawTexture) {
        overlay.rawTexture->Release();
        overlay.rawTexture = nullptr;
    }
    overlay.rawWidth = 0;
    overlay.rawHeight = 0;
}

xr::EVROverlayError Backend::CreateOverlay(const char* key, const char* name, xr::VROverlayHandle_t* handle) {
    if (!handle || !key || !name)
        return xr::VROverlayError_InvalidParameter;
    std::lock_guard<std::mutex> lock(m_overlayMutex);
    OverlayState overlay;
    overlay.handle = m_nextOverlayHandle.fetch_add(1);
    overlay.key = key;
    overlay.name = name;
    overlay.transform.m[0][0] = 1.0f;
    overlay.transform.m[1][1] = 1.0f;
    overlay.transform.m[2][2] = 1.0f;
    m_overlays.push_back(std::move(overlay));
    *handle = m_overlays.back().handle;
    return xr::VROverlayError_None;
}

xr::EVROverlayError Backend::DestroyOverlay(xr::VROverlayHandle_t handle) {
    // A frame may still be sampling this overlay's source texture.  Let it
    // finish before we release the CPU-uploaded D3D9 texture or swapchain.
    WaitForFrameIdle();
    std::shared_ptr<OverlaySwapchain> stale;
    {
        std::lock_guard<std::mutex> lock(m_overlayMutex);
        for (auto it = m_overlays.begin(); it != m_overlays.end(); ++it) {
            if (it->handle == handle) {
                stale = it->swapchain;
                if (it->rawTexture || it->rawSurface)
                    m_retiredRawTextures.emplace_back(it->rawTexture, it->rawSurface);
                it->rawTexture = nullptr;
                it->rawSurface = nullptr;
                it->rawWidth = 0;
                it->rawHeight = 0;
                m_overlays.erase(it);
                break;
            }
        }
    }
    if (stale)
        DestroyOverlaySwapchain(stale);
    return xr::VROverlayError_None;
}

xr::EVROverlayError Backend::FindOverlay(const char* key, xr::VROverlayHandle_t* handle) {
    if (!key || !handle)
        return xr::VROverlayError_InvalidParameter;
    std::lock_guard<std::mutex> lock(m_overlayMutex);
    for (const auto& overlay : m_overlays) {
        if (overlay.key == key) {
            *handle = overlay.handle;
            return xr::VROverlayError_None;
        }
    }
    *handle = xr::k_ulOverlayHandleInvalid;
    return xr::VROverlayError_UnknownOverlay;
}

xr::EVROverlayError Backend::SetOverlaySortOrder(xr::VROverlayHandle_t handle, uint32_t sortOrder) {
    std::lock_guard<std::mutex> lock(m_overlayMutex);
    OverlayState* overlay = FindOverlayByHandle(m_overlays, handle);
    if (!overlay)
        return xr::VROverlayError_UnknownOverlay;
    overlay->sortOrder = sortOrder;
    return xr::VROverlayError_None;
}

xr::EVROverlayError Backend::SetOverlayRaw(xr::VROverlayHandle_t handle, void* buffer, uint32_t width, uint32_t height, uint32_t depth) {
    if (!buffer || !width || !height || depth != 4)
        return xr::VROverlayError_InvalidParameter;

    // Serialize the CPU upload against the frame thread.  Like OpenVR's
    // SetOverlayRaw, this is a synchronous upload.
    WaitForFrameIdle();

    if (!g_D3DVR9)
        return xr::VROverlayError_RequestFailed;

    IDirect3DDevice9* device = nullptr;
    if (FAILED(g_D3DVR9->GetD3DDevice(&device)) || !device)
        return xr::VROverlayError_RequestFailed;

    IDirect3DTexture9* newTexture = nullptr;
    IDirect3DSurface9* newSurface = nullptr;
    bool createdTexture = false;
    HRESULT hr = E_FAIL;

    g_D3DVR9->LockDevice();
    {
        // Reuse the current texture when the dimensions match.
        std::lock_guard<std::mutex> lock(m_overlayMutex);
        OverlayState* overlay = FindOverlayByHandle(m_overlays, handle);
        if (!overlay) {
            g_D3DVR9->UnlockDevice();
            device->Release();
            return xr::VROverlayError_UnknownOverlay;
        }

        if (overlay->rawTexture && overlay->rawSurface &&
            overlay->rawWidth == width && overlay->rawHeight == height) {
            newTexture = overlay->rawTexture;
            newSurface = overlay->rawSurface;
        }
    }

    if (!newTexture) {
        hr = device->CreateTexture(width, height, 1, D3DUSAGE_DYNAMIC,
            D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &newTexture, nullptr);
        if (SUCCEEDED(hr) && newTexture)
            hr = newTexture->GetSurfaceLevel(0, &newSurface);
        createdTexture = true;
        if (FAILED(hr) || !newTexture || !newSurface) {
            if (newSurface) { newSurface->Release(); newSurface = nullptr; }
            if (newTexture) { newTexture->Release(); newTexture = nullptr; }
            g_D3DVR9->UnlockDevice();
            device->Release();
            return xr::VROverlayError_RequestFailed;
        }
    }

    D3DLOCKED_RECT locked = {};
    hr = newTexture->LockRect(0, &locked, nullptr, D3DLOCK_DISCARD);
    if (SUCCEEDED(hr) && locked.pBits) {
        const uint8_t* src = static_cast<const uint8_t*>(buffer);
        uint8_t* dst = static_cast<uint8_t*>(locked.pBits);
        for (uint32_t y = 0; y < height; ++y) {
            const uint8_t* srcRow = src + size_t(y) * size_t(width) * 4u;
            uint8_t* dstRow = dst + size_t(y) * size_t(locked.Pitch);
            for (uint32_t x = 0; x < width; ++x) {
                // HUD pixels are RGBA; D3DFMT_A8R8G8B8 stores BGRA.
                dstRow[x * 4u + 0u] = srcRow[x * 4u + 2u];
                dstRow[x * 4u + 1u] = srcRow[x * 4u + 1u];
                dstRow[x * 4u + 2u] = srcRow[x * 4u + 0u];
                dstRow[x * 4u + 3u] = srcRow[x * 4u + 3u];
            }
        }
        newTexture->UnlockRect(0);
    } else {
        if (SUCCEEDED(hr))
            newTexture->UnlockRect(0);
        hr = E_FAIL;
    }
    g_D3DVR9->UnlockDevice();

    xr::VRVulkanTextureData_t vulkanData = {};
    D3D9_TEXTURE_VR_DESC desc = {};
    if (SUCCEEDED(hr) && SUCCEEDED(g_D3DVR9->TransferSurface(newSurface, FALSE))) {
        g_D3DVR9->LockDevice();
        const HRESULT descHr = g_D3DVR9->GetVRDesc(newSurface, &desc);
        g_D3DVR9->UnlockDevice();
        if (SUCCEEDED(descHr))
            std::memcpy(&vulkanData, &desc, sizeof(vulkanData));
        else
            hr = E_FAIL;
    } else {
        hr = E_FAIL;
    }

    if (FAILED(hr)) {
        if (createdTexture) {
            if (newSurface)
                newSurface->Release();
            if (newTexture)
                newTexture->Release();
        } else {
            // The reused texture failed to lock/upload (typically after a
            // device reset).  Retire it so the next call creates a fresh one.
            std::lock_guard<std::mutex> lock(m_overlayMutex);
            OverlayState* overlay = FindOverlayByHandle(m_overlays, handle);
            if (overlay && overlay->rawTexture == newTexture) {
                if (overlay->rawTexture || overlay->rawSurface)
                    m_retiredRawTextures.emplace_back(overlay->rawTexture, overlay->rawSurface);
                overlay->rawTexture = nullptr;
                overlay->rawSurface = nullptr;
                overlay->rawWidth = 0;
                overlay->rawHeight = 0;
            }
        }
        device->Release();
        return xr::VROverlayError_RequestFailed;
    }

    {
        std::lock_guard<std::mutex> lock(m_overlayMutex);
        OverlayState* overlay = FindOverlayByHandle(m_overlays, handle);
        if (!overlay) {
            if (createdTexture) {
                if (newSurface)
                    newSurface->Release();
                if (newTexture)
                    newTexture->Release();
            }
            device->Release();
            return xr::VROverlayError_UnknownOverlay;
        }

        if (overlay->rawTexture != newTexture) {
            if (overlay->rawTexture || overlay->rawSurface) {
                // A frame may still have the previous descriptor in flight.
                // Keep the D3D9 resources alive until shutdown rather than
                // risking a use-after-free on a resize.
                m_retiredRawTextures.emplace_back(overlay->rawTexture, overlay->rawSurface);
            }
            overlay->rawTexture = newTexture;
            overlay->rawSurface = newSurface;
            overlay->rawWidth = width;
            overlay->rawHeight = height;
        }
        overlay->texture = vulkanData;
        overlay->hasTexture = true;
        overlay->textureGeneration++;
    }
    device->Release();

    xr::Texture_t texture = {};
    texture.handle = &vulkanData;
    texture.eType = xr::TextureType_Vulkan;
    texture.eColorSpace = xr::ColorSpace_Auto;
    return SetOverlayTexture(handle, &texture);
}

xr::EVROverlayError Backend::SetOverlayTexture(xr::VROverlayHandle_t handle, const xr::Texture_t* texture) {
    if (!texture || !texture->handle)
        return xr::VROverlayError_InvalidTexture;
    const auto* data = reinterpret_cast<const xr::VRVulkanTextureData_t*>(texture->handle);
    if (!data->m_nImage || !data->m_nWidth || !data->m_nHeight)
        return xr::VROverlayError_InvalidTexture;

    bool changed = false;
    bool hasSwapchain = false;
    {
        std::lock_guard<std::mutex> lock(m_overlayMutex);
        OverlayState* overlay = FindOverlayByHandle(m_overlays, handle);
        if (!overlay)
            return xr::VROverlayError_UnknownOverlay;

        changed =
            !overlay->hasTexture ||
            overlay->texture.m_nImage != data->m_nImage ||
            overlay->texture.m_nWidth != data->m_nWidth ||
            overlay->texture.m_nHeight != data->m_nHeight;

        overlay->texture = *data;
        overlay->hasTexture = true;
        overlay->textureGeneration++;

        if (!overlay->swapchain ||
            overlay->swapchain->width != data->m_nWidth ||
            overlay->swapchain->height != data->m_nHeight) {
            std::shared_ptr<OverlaySwapchain> old = overlay->swapchain;
            overlay->swapchain = CreateOverlaySwapchain(data->m_nWidth, data->m_nHeight);
            if (!overlay->swapchain)
                overlay->swapchain = old;
        }
        hasSwapchain = overlay->swapchain != nullptr;
    }

    if (changed)
        WaitForFrameIdle();
    return hasSwapchain ? xr::VROverlayError_None : xr::VROverlayError_RequestFailed;
}

xr::EVROverlayError Backend::SetOverlayTextureBounds(xr::VROverlayHandle_t handle, const xr::VRTextureBounds_t* bounds) {
    if (!bounds) return xr::VROverlayError_InvalidParameter;
    std::lock_guard<std::mutex> lock(m_overlayMutex);
    OverlayState* overlay = FindOverlayByHandle(m_overlays, handle);
    if (!overlay) return xr::VROverlayError_UnknownOverlay;
    overlay->bounds = *bounds;
    return xr::VROverlayError_None;
}

xr::EVROverlayError Backend::SetOverlayTransformAbsolute(xr::VROverlayHandle_t handle, int origin, const xr::HmdMatrix34_t* transform) {
    if (!transform) return xr::VROverlayError_InvalidParameter;
    std::lock_guard<std::mutex> lock(m_overlayMutex);
    OverlayState* overlay = FindOverlayByHandle(m_overlays, handle);
    if (!overlay) return xr::VROverlayError_UnknownOverlay;
    overlay->transformType = xr::VROverlayTransform_Absolute;
    overlay->transformOrigin = origin;
    overlay->transform = *transform;
    return xr::VROverlayError_None;
}

xr::EVROverlayError Backend::SetOverlayTransformTrackedDeviceRelative(xr::VROverlayHandle_t handle, xr::TrackedDeviceIndex_t device, const xr::HmdMatrix34_t* transform) {
    if (!transform) return xr::VROverlayError_InvalidParameter;
    std::lock_guard<std::mutex> lock(m_overlayMutex);
    OverlayState* overlay = FindOverlayByHandle(m_overlays, handle);
    if (!overlay) return xr::VROverlayError_UnknownOverlay;
    overlay->transformType = xr::VROverlayTransform_TrackedDeviceRelative;
    overlay->transformDevice = device;
    overlay->transform = *transform;
    return xr::VROverlayError_None;
}

xr::EVROverlayError Backend::GetOverlayTransformType(xr::VROverlayHandle_t handle, int* transformType) {
    if (!transformType) return xr::VROverlayError_InvalidParameter;
    std::lock_guard<std::mutex> lock(m_overlayMutex);
    OverlayState* overlay = FindOverlayByHandle(m_overlays, handle);
    if (!overlay) return xr::VROverlayError_UnknownOverlay;
    *transformType = overlay->transformType;
    return xr::VROverlayError_None;
}

xr::EVROverlayError Backend::GetOverlayTransformAbsolute(xr::VROverlayHandle_t handle, int* origin, xr::HmdMatrix34_t* transform) {
    if (!origin || !transform) return xr::VROverlayError_InvalidParameter;
    std::lock_guard<std::mutex> lock(m_overlayMutex);
    OverlayState* overlay = FindOverlayByHandle(m_overlays, handle);
    if (!overlay || overlay->transformType != xr::VROverlayTransform_Absolute)
        return xr::VROverlayError_WrongTransformType;
    *origin = overlay->transformOrigin;
    *transform = overlay->transform;
    return xr::VROverlayError_None;
}

xr::EVROverlayError Backend::GetOverlayTransformTrackedDeviceRelative(xr::VROverlayHandle_t handle, xr::TrackedDeviceIndex_t* device, xr::HmdMatrix34_t* transform) {
    if (!device || !transform) return xr::VROverlayError_InvalidParameter;
    std::lock_guard<std::mutex> lock(m_overlayMutex);
    OverlayState* overlay = FindOverlayByHandle(m_overlays, handle);
    if (!overlay || overlay->transformType != xr::VROverlayTransform_TrackedDeviceRelative)
        return xr::VROverlayError_WrongTransformType;
    *device = overlay->transformDevice;
    *transform = overlay->transform;
    return xr::VROverlayError_None;
}

xr::EVROverlayError Backend::ShowOverlay(xr::VROverlayHandle_t handle) {
    std::lock_guard<std::mutex> lock(m_overlayMutex);
    OverlayState* overlay = FindOverlayByHandle(m_overlays, handle);
    if (!overlay) return xr::VROverlayError_UnknownOverlay;
    overlay->visible = true;
    return xr::VROverlayError_None;
}

xr::EVROverlayError Backend::HideOverlay(xr::VROverlayHandle_t handle) {
    std::lock_guard<std::mutex> lock(m_overlayMutex);
    OverlayState* overlay = FindOverlayByHandle(m_overlays, handle);
    if (!overlay) return xr::VROverlayError_UnknownOverlay;
    overlay->visible = false;
    overlay->events.clear();
    return xr::VROverlayError_None;
}

bool Backend::IsOverlayVisible(xr::VROverlayHandle_t handle) {
    std::lock_guard<std::mutex> lock(m_overlayMutex);
    OverlayState* overlay = FindOverlayByHandle(m_overlays, handle);
    return overlay && overlay->visible;
}

xr::EVROverlayError Backend::SetOverlayWidthInMeters(xr::VROverlayHandle_t handle, float width) {
    std::lock_guard<std::mutex> lock(m_overlayMutex);
    OverlayState* overlay = FindOverlayByHandle(m_overlays, handle);
    if (!overlay) return xr::VROverlayError_UnknownOverlay;
    overlay->widthMeters = width;
    return xr::VROverlayError_None;
}

xr::EVROverlayError Backend::GetOverlayWidthInMeters(xr::VROverlayHandle_t handle, float* width) {
    if (!width) return xr::VROverlayError_InvalidParameter;
    std::lock_guard<std::mutex> lock(m_overlayMutex);
    OverlayState* overlay = FindOverlayByHandle(m_overlays, handle);
    if (!overlay) return xr::VROverlayError_UnknownOverlay;
    *width = overlay->widthMeters;
    return xr::VROverlayError_None;
}

xr::EVROverlayError Backend::SetOverlayTexelAspect(xr::VROverlayHandle_t handle, float aspect) {
    std::lock_guard<std::mutex> lock(m_overlayMutex);
    OverlayState* overlay = FindOverlayByHandle(m_overlays, handle);
    if (!overlay) return xr::VROverlayError_UnknownOverlay;
    overlay->texelAspect = aspect;
    return xr::VROverlayError_None;
}

xr::EVROverlayError Backend::SetOverlayAlpha(xr::VROverlayHandle_t handle, float alpha) {
    std::lock_guard<std::mutex> lock(m_overlayMutex);
    OverlayState* overlay = FindOverlayByHandle(m_overlays, handle);
    if (!overlay) return xr::VROverlayError_UnknownOverlay;
    overlay->alpha = alpha;
    return xr::VROverlayError_None;
}

xr::EVROverlayError Backend::SetOverlayCurvature(xr::VROverlayHandle_t handle, float curvature) {
    std::lock_guard<std::mutex> lock(m_overlayMutex);
    OverlayState* overlay = FindOverlayByHandle(m_overlays, handle);
    if (!overlay) return xr::VROverlayError_UnknownOverlay;
    overlay->curvature = curvature;
    return xr::VROverlayError_None;
}

xr::EVROverlayError Backend::SetOverlayInputMethod(xr::VROverlayHandle_t handle, int method) {
    std::lock_guard<std::mutex> lock(m_overlayMutex);
    OverlayState* overlay = FindOverlayByHandle(m_overlays, handle);
    if (!overlay) return xr::VROverlayError_UnknownOverlay;
    overlay->inputMethod = method;
    return xr::VROverlayError_None;
}

xr::EVROverlayError Backend::SetOverlayFlag(xr::VROverlayHandle_t handle, uint32_t flag, bool enabled) {
    std::lock_guard<std::mutex> lock(m_overlayMutex);
    OverlayState* overlay = FindOverlayByHandle(m_overlays, handle);
    if (!overlay) return xr::VROverlayError_UnknownOverlay;
    if (enabled) overlay->flags |= flag;
    else overlay->flags &= ~flag;
    return xr::VROverlayError_None;
}

xr::EVROverlayError Backend::SetOverlayMouseScale(xr::VROverlayHandle_t handle, const xr::HmdVector2_t* scale) {
    if (!scale) return xr::VROverlayError_InvalidParameter;
    std::lock_guard<std::mutex> lock(m_overlayMutex);
    OverlayState* overlay = FindOverlayByHandle(m_overlays, handle);
    if (!overlay) return xr::VROverlayError_UnknownOverlay;
    overlay->mouseScale = *scale;
    return xr::VROverlayError_None;
}

xr::EVROverlayError Backend::ComputeOverlayIntersection(xr::VROverlayHandle_t handle, const xr::VROverlayIntersectionParams_t* params, xr::VROverlayIntersectionResults_t* results) {
    if (!params || !results) return xr::VROverlayError_InvalidParameter;

    PoseSnapshot snapshot;
    {
        std::lock_guard<std::mutex> lock(m_poseMutex);
        snapshot = m_poseSnapshot;
    }

    std::lock_guard<std::mutex> lock(m_overlayMutex);
    OverlayState* overlay = FindOverlayByHandle(m_overlays, handle);
    if (!overlay) return xr::VROverlayError_UnknownOverlay;

    XrVector3f direction = { params->vDirection.v[0], params->vDirection.v[1], params->vDirection.v[2] };
    const float len = std::sqrt(direction.x * direction.x + direction.y * direction.y + direction.z * direction.z);
    if (len < 1e-6f) return xr::VROverlayError_RequestFailed;
    direction.x /= len; direction.y /= len; direction.z /= len;

    const XrVector3f up = { 0.0f, 1.0f, 0.0f };
    XrVector3f right = {
        direction.y * up.z - direction.z * up.y,
        direction.z * up.x - direction.x * up.z,
        direction.x * up.y - direction.y * up.x,
    };
    float rightLen = std::sqrt(right.x * right.x + right.y * right.y + right.z * right.z);
    if (rightLen < 1e-6f) {
        right = { 1.0f, 0.0f, 0.0f };
    } else {
        right.x /= rightLen; right.y /= rightLen; right.z /= rightLen;
    }
    const XrVector3f realUp = {
        right.y * direction.z - right.z * direction.y,
        right.z * direction.x - right.x * direction.z,
        right.x * direction.y - right.y * direction.x,
    };
    const XrVector3f back = { -direction.x, -direction.y, -direction.z };

    xr::HmdMatrix34_t mat = {};
    mat.m[0][0] = right.x; mat.m[1][0] = right.y; mat.m[2][0] = right.z;
    mat.m[0][1] = realUp.x; mat.m[1][1] = realUp.y; mat.m[2][1] = realUp.z;
    mat.m[0][2] = back.x; mat.m[1][2] = back.y; mat.m[2][2] = back.z;
    mat.m[0][3] = params->vSource.v[0];
    mat.m[1][3] = params->vSource.v[1];
    mat.m[2][3] = params->vSource.v[2];
    const XrPosef rayPose = MatrixToPose(mat);

    XrVector3f point = {};
    float distance = 0.0f, u = 0.0f, v = 0.0f;
    if (!IntersectOverlay(*overlay, snapshot, rayPose, point, distance, u, v))
        return xr::VROverlayError_RequestFailed;

    results->vPoint = { point.x, point.y, point.z };
    results->vNormal = { 0.0f, 0.0f, 1.0f };
    results->vUVs = { u, v };
    results->fDistance = distance;
    return xr::VROverlayError_None;
}

bool Backend::PollNextOverlayEvent(xr::VROverlayHandle_t handle, xr::VREvent_t* event, uint32_t eventSize) {
    if (!event || eventSize < sizeof(xr::VREvent_t))
        return false;

    PoseSnapshot snapshot;
    {
        std::lock_guard<std::mutex> lock(m_poseMutex);
        snapshot = m_poseSnapshot;
    }

    std::lock_guard<std::mutex> lock(m_overlayMutex);
    OverlayState* overlay = FindOverlayByHandle(m_overlays, handle);
    if (!overlay || !overlay->visible || overlay->inputMethod != xr::VROverlayInputMethod_Mouse)
        return false;

    if (overlay->events.empty())
        GenerateOverlayEvents(*overlay, snapshot);
    if (overlay->events.empty())
        return false;

    *event = overlay->events.front();
    overlay->events.erase(overlay->events.begin());
    return true;
}

bool Backend::GetComponentStateForDevicePath(const char* devicePath, const char* componentName, xr::VRInputValueHandle_t inputValue, xr::RenderModel_ControllerMode_State_t* state, xr::RenderModel_ComponentState_t* componentState) {
    (void)devicePath;
    (void)componentName;
    if (state) *state = {};
    if (!componentState) return false;
    std::memset(componentState, 0, sizeof(*componentState));
    const int hand = inputValue == xr::k_unInputSourceLeft ? 0 : (inputValue == xr::k_unInputSourceRight ? 1 : -1);
    if (hand < 0) return false;

    PoseSnapshot snapshot;
    {
        std::lock_guard<std::mutex> lock(m_poseMutex);
        snapshot = m_poseSnapshot;
    }
    if (!snapshot.hands[hand].poseValid)
        return false;
    componentState->mTrackingToComponentRenderModel = PoseToMatrix(kIdentityPose);
    componentState->mTrackingToComponentLocal = PoseToMatrix(snapshot.hands[hand].aimInGrip);
    return true;
}

} // namespace
