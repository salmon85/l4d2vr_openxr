/*
 * OpenXR helper bridge backend for L4D2VR.
 *
 * The game is a 32-bit D3D9 process.  WiVRn/Monado on Linux expose a 64-bit
 * OpenXR runtime, and a 32-bit Wine process cannot load it.  This backend
 * therefore talks to a 64-bit OpenXRHelper64.exe:
 *
 *   game (32-bit DXVK) -- KMT eye textures + shared memory --> helper (x64)
 *   helper -- HMD/controller poses + action states + haptics --> game
 *
 * The public xr:: facade is kept identical to the old in-process backend so
 * gameplay code does not change.
 */

#include "vr_api.h"

#include "d3d9_vr.h"
#include "game.h"
#include "openxr_helper_bridge.h"
#include "vr.h"
#include "sdk.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

static_assert(sizeof(xr::VRVulkanTextureData_t) == sizeof(D3D9_TEXTURE_VR_DESC),
    "VRVulkanTextureData_t and D3D9_TEXTURE_VR_DESC must have identical layouts");

namespace {

// ---------------------------------------------------------------------------
// Math / conversion helpers
// ---------------------------------------------------------------------------

struct Pose {
    float position[3] = { 0.0f, 0.0f, 0.0f };
    float orientation[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
};

Pose MakePose(const float* position, const float* orientation) {
    Pose out;
    if (position) {
        out.position[0] = position[0];
        out.position[1] = position[1];
        out.position[2] = position[2];
    }
    if (orientation) {
        float x = orientation[0], y = orientation[1], z = orientation[2], w = orientation[3];
        const float lenSq = x * x + y * y + z * z + w * w;
        if (lenSq > 1e-8f) {
            const float inv = 1.0f / std::sqrt(lenSq);
            x *= inv; y *= inv; z *= inv; w *= inv;
        } else {
            x = y = z = 0.0f; w = 1.0f;
        }
        out.orientation[0] = x;
        out.orientation[1] = y;
        out.orientation[2] = z;
        out.orientation[3] = w;
    }
    return out;
}

Pose FromController(const L4D2VROpenXrControllerPoseDesc& desc) {
    return MakePose(desc.position, desc.orientation);
}

Pose MultiplyPose(const Pose& a, const Pose& b) {
    Pose out;
    const float ax = a.orientation[0], ay = a.orientation[1], az = a.orientation[2], aw = a.orientation[3];
    const float bx = b.orientation[0], by = b.orientation[1], bz = b.orientation[2], bw = b.orientation[3];
    out.orientation[0] = aw * bx + ax * bw + ay * bz - az * by;
    out.orientation[1] = aw * by - ax * bz + ay * bw + az * bx;
    out.orientation[2] = aw * bz + ax * by - ay * bx + az * bw;
    out.orientation[3] = aw * bw - ax * bx - ay * by - az * bz;

    const float x = b.position[0], y = b.position[1], z = b.position[2];
    const float tx = 2.0f * (ay * z - az * y);
    const float ty = 2.0f * (az * x - ax * z);
    const float tz = 2.0f * (ax * y - ay * x);
    out.position[0] = a.position[0] + x + aw * tx + (ay * tz - az * ty);
    out.position[1] = a.position[1] + y + aw * ty + (az * tx - ax * tz);
    out.position[2] = a.position[2] + z + aw * tz + (ax * ty - ay * tx);
    return out;
}

Pose InversePose(const Pose& pose) {
    Pose out;
    out.orientation[0] = -pose.orientation[0];
    out.orientation[1] = -pose.orientation[1];
    out.orientation[2] = -pose.orientation[2];
    out.orientation[3] = pose.orientation[3];
    const float x = pose.position[0], y = pose.position[1], z = pose.position[2];
    const float qx = out.orientation[0], qy = out.orientation[1], qz = out.orientation[2], qw = out.orientation[3];
    const float tx = 2.0f * (qy * z - qz * y);
    const float ty = 2.0f * (qz * x - qx * z);
    const float tz = 2.0f * (qx * y - qy * x);
    out.position[0] = -(x + qw * tx + (qy * tz - qz * ty));
    out.position[1] = -(y + qw * ty + (qz * tx - qx * tz));
    out.position[2] = -(z + qw * tz + (qx * ty - qy * tx));
    return out;
}

xr::HmdMatrix34_t PoseToMatrix(const Pose& pose) {
    xr::HmdMatrix34_t out = {};
    const float x = pose.orientation[0], y = pose.orientation[1], z = pose.orientation[2], w = pose.orientation[3];
    out.m[0][0] = 1.0f - 2.0f * (y * y + z * z);
    out.m[0][1] = 2.0f * (x * y - z * w);
    out.m[0][2] = 2.0f * (x * z + y * w);
    out.m[1][0] = 2.0f * (x * y + z * w);
    out.m[1][1] = 1.0f - 2.0f * (x * x + z * z);
    out.m[1][2] = 2.0f * (y * z - x * w);
    out.m[2][0] = 2.0f * (x * z - y * w);
    out.m[2][1] = 2.0f * (y * z + x * w);
    out.m[2][2] = 1.0f - 2.0f * (x * x + y * y);
    out.m[0][3] = pose.position[0];
    out.m[1][3] = pose.position[1];
    out.m[2][3] = pose.position[2];
    return out;
}

Pose MatrixToPose(const xr::HmdMatrix34_t& mat) {
    Pose out;
    out.position[0] = mat.m[0][3];
    out.position[1] = mat.m[1][3];
    out.position[2] = mat.m[2][3];

    const float m00 = mat.m[0][0], m01 = mat.m[0][1], m02 = mat.m[0][2];
    const float m10 = mat.m[1][0], m11 = mat.m[1][1], m12 = mat.m[1][2];
    const float m20 = mat.m[2][0], m21 = mat.m[2][1], m22 = mat.m[2][2];
    const float trace = m00 + m11 + m22;
    float x, y, z, w;
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        w = 0.25f * s;
        x = (m21 - m12) / s;
        y = (m02 - m20) / s;
        z = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        w = (m21 - m12) / s;
        x = 0.25f * s;
        y = (m01 + m10) / s;
        z = (m02 + m20) / s;
    } else if (m11 > m22) {
        const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        w = (m02 - m20) / s;
        x = (m01 + m10) / s;
        y = 0.25f * s;
        z = (m12 + m21) / s;
    } else {
        const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        w = (m10 - m01) / s;
        x = (m02 + m20) / s;
        y = (m12 + m21) / s;
        z = 0.25f * s;
    }
    const float lenSq = x * x + y * y + z * z + w * w;
    const float inv = lenSq > 1e-8f ? 1.0f / std::sqrt(lenSq) : 0.0f;
    out.orientation[0] = x * inv;
    out.orientation[1] = y * inv;
    out.orientation[2] = z * inv;
    out.orientation[3] = inv > 0.0f ? w * inv : 1.0f;
    return out;
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

struct ActionPath {
    const char* path;
    int id;
};

// These ids are L4D2VROpenXrActionId values from openxr_bridge_protocol.h.
const ActionPath kActionPaths[] = {
    { "/actions/main/in/ActivateVR", static_cast<int>(L4D2VROpenXrActionId::ActivateVR) },
    { "/actions/main/in/Jump", static_cast<int>(L4D2VROpenXrActionId::Jump) },
    { "/actions/main/in/PrimaryAttack", static_cast<int>(L4D2VROpenXrActionId::PrimaryAttack) },
    { "/actions/main/in/Reload", static_cast<int>(L4D2VROpenXrActionId::Reload) },
    { "/actions/main/in/Use", static_cast<int>(L4D2VROpenXrActionId::Use) },
    { "/actions/main/in/Teleport", static_cast<int>(L4D2VROpenXrActionId::Teleport) },
    { "/actions/main/in/Walk", static_cast<int>(L4D2VROpenXrActionId::Walk) },
    { "/actions/main/in/Turn", static_cast<int>(L4D2VROpenXrActionId::Turn) },
    { "/actions/main/in/SecondaryAttack", static_cast<int>(L4D2VROpenXrActionId::SecondaryAttack) },
    { "/actions/main/in/NextItem", static_cast<int>(L4D2VROpenXrActionId::NextItem) },
    { "/actions/main/in/PrevItem", static_cast<int>(L4D2VROpenXrActionId::PrevItem) },
    { "/actions/main/in/ResetPosition", static_cast<int>(L4D2VROpenXrActionId::ResetPosition) },
    { "/actions/main/in/Crouch", static_cast<int>(L4D2VROpenXrActionId::Crouch) },
    { "/actions/main/in/Flashlight", static_cast<int>(L4D2VROpenXrActionId::Flashlight) },
    { "/actions/main/in/InventoryGripLeft", static_cast<int>(L4D2VROpenXrActionId::InventoryGripLeft) },
    { "/actions/main/in/InventoryGripRight", static_cast<int>(L4D2VROpenXrActionId::InventoryGripRight) },
    { "/actions/main/in/InventoryQuickSwitch", static_cast<int>(L4D2VROpenXrActionId::InventoryQuickSwitch) },
    { "/actions/main/in/SpecialInfectedAutoAimToggle", static_cast<int>(L4D2VROpenXrActionId::SpecialInfectedAutoAimToggle) },
    { "/actions/main/in/SpecialInfectedDodgeToggle", static_cast<int>(L4D2VROpenXrActionId::SpecialInfectedDodgeToggle) },
    { "/actions/main/in/LedgeGuardToggle", static_cast<int>(L4D2VROpenXrActionId::LedgeGuardToggle) },
    { "/actions/main/in/EffectiveAttackRangeAutoFireToggle", static_cast<int>(L4D2VROpenXrActionId::EffectiveAttackRangeAutoFireToggle) },
    { "/actions/main/in/SpeechToText", static_cast<int>(L4D2VROpenXrActionId::SpeechToText) },
    { "/actions/main/in/MenuSelect", static_cast<int>(L4D2VROpenXrActionId::MenuSelect) },
    { "/actions/main/in/MenuBack", static_cast<int>(L4D2VROpenXrActionId::MenuBack) },
    { "/actions/main/in/MenuUp", static_cast<int>(L4D2VROpenXrActionId::MenuUp) },
    { "/actions/main/in/MenuDown", static_cast<int>(L4D2VROpenXrActionId::MenuDown) },
    { "/actions/main/in/MenuLeft", static_cast<int>(L4D2VROpenXrActionId::MenuLeft) },
    { "/actions/main/in/MenuRight", static_cast<int>(L4D2VROpenXrActionId::MenuRight) },
    { "/actions/main/in/Spray", static_cast<int>(L4D2VROpenXrActionId::Spray) },
    { "/actions/main/in/Scoreboard", static_cast<int>(L4D2VROpenXrActionId::Scoreboard) },
    { "/actions/main/in/ShowHUD", static_cast<int>(L4D2VROpenXrActionId::ShowHUD) },
    { "/actions/main/in/Pause", static_cast<int>(L4D2VROpenXrActionId::Pause) },
    { "/actions/main/in/NonVRServerMovementAngleToggle", static_cast<int>(L4D2VROpenXrActionId::NonVRServerMovementAngleToggle) },
    { "/actions/main/in/ScopeToggle", static_cast<int>(L4D2VROpenXrActionId::ScopeToggle) },
    { "/actions/main/in/FriendlyFireBlockToggle", static_cast<int>(L4D2VROpenXrActionId::FriendlyFireBlockToggle) },
    { "/actions/main/in/CustomAction1", static_cast<int>(L4D2VROpenXrActionId::CustomAction1) },
    { "/actions/main/in/CustomAction2", static_cast<int>(L4D2VROpenXrActionId::CustomAction2) },
    { "/actions/main/in/CustomAction3", static_cast<int>(L4D2VROpenXrActionId::CustomAction3) },
    { "/actions/main/in/CustomAction4", static_cast<int>(L4D2VROpenXrActionId::CustomAction4) },
    { "/actions/main/in/CustomAction5", static_cast<int>(L4D2VROpenXrActionId::CustomAction5) },
    // Not native helper actions; synthesized below from Turn.
    { "/actions/main/in/boolean_turnleft", 1001 },
    { "/actions/main/in/boolean_turnright", 1002 },
};

constexpr int kSkeletonLeftHandle = 2001;
constexpr int kSkeletonRightHandle = 2002;

int ActionIdForPath(const char* path) {
    if (!path)
        return 0;
    for (const auto& entry : kActionPaths) {
        if (std::strcmp(entry.path, path) == 0)
            return entry.id;
    }
    if (std::strcmp(path, "/actions/base/in/skeleton_lefthand") == 0)
        return kSkeletonLeftHandle;
    if (std::strcmp(path, "/actions/base/in/skeleton_righthand") == 0)
        return kSkeletonRightHandle;
    return 0;
}

// ---------------------------------------------------------------------------
// Overlay / pose state
// ---------------------------------------------------------------------------

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
    uint32_t sortOrder = 0;

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

struct PoseSnapshot {
    bool valid = false;
    xr::HmdMatrix34_t hmd{};
    bool handsValid[2] = { false, false };
    Pose handGrip[2];
    Pose handAim[2];
    float trigger[2] = { 0.0f, 0.0f };
    float stickY[2] = { 0.0f, 0.0f };
    bool primary[2] = { false, false };
    bool secondary[2] = { false, false };
};

// Publish the game's menu/HUD overlays into the helper bridge.  Only these
// two slots exist in the x64 helper; other OpenVR overlays (scope, wrist HUD,
// config panel) stay game-side until the helper grows matching slots.
void PublishOverlayToBridge(const OverlayState& overlay) {
    int overlayIndex = -1;
    if (overlay.key == "MenuOverlayKey") {
        overlayIndex = L4D2VR_OPENXR_OVERLAY_MAIN_MENU;
    } else if (overlay.key == "HUDOverlayTopKey") {
        overlayIndex = L4D2VR_OPENXR_OVERLAY_HUD;
    } else {
        return;
    }

    const xr::VRVulkanTextureData_t& data = overlay.texture;
    const bool hasTexture =
        overlay.hasTexture &&
        data.m_nImage != 0 &&
        data.m_nWidth != 0 &&
        data.m_nHeight != 0 &&
        data.m_nSharedHandleValid != 0 &&
        data.m_nSharedHandle != 0;

    L4D2VROpenXrOverlayDesc desc{};
    desc.visible = overlay.visible ? 1u : 0u;
    desc.valid = (overlay.visible && hasTexture) ? 1u : 0u;
    if (hasTexture) {
        desc.texture.valid = 1u;
        desc.texture.width = data.m_nWidth;
        desc.texture.height = data.m_nHeight;
        desc.texture.format = data.m_nFormat;
        desc.texture.sampleCount = data.m_nSampleCount;
        desc.texture.handleType = data.m_nSharedHandleType;
        desc.texture.queueFamilyIndex = data.m_nQueueFamilyIndex;
        desc.texture.kmtHandle = data.m_nSharedHandle;
        desc.texture.image = data.m_nImage;
        desc.texture.uMin = overlay.bounds.uMin;
        desc.texture.vMin = overlay.bounds.vMin;
        desc.texture.uMax = overlay.bounds.uMax;
        desc.texture.vMax = overlay.bounds.vMax;
        desc.texture.renderFovXDeg = 90.0f;
        desc.texture.renderAspect = 1.0f;
    }

    const float textureAspect =
        (data.m_nWidth != 0) ? (static_cast<float>(data.m_nHeight) / static_cast<float>(data.m_nWidth)) : 0.5625f;

    if (overlayIndex == L4D2VR_OPENXR_OVERLAY_MAIN_MENU) {
        // The game's menu transform widens the overlay to cancel the eye-sized
        // backbuffer; the helper quad is already HMD-relative, so use a fixed
        // readable size instead.
        desc.widthMeters = std::clamp(overlay.widthMeters, 0.8f, 1.8f);
        desc.distanceMeters = 3.0f;
        desc.offsetMeters[0] = 0.0f;
        desc.offsetMeters[1] = -0.25f;
        desc.offsetMeters[2] = 0.0f;
        desc.reserved1 = 0;
    } else {
        // Keep the HUD layer available for the whole gameplay session. Source
        // toggles the top HUD overlay on and off around pause/scoreboard/VGUI
        // transitions; without this the helper drops the layer every other
        // frame and the HUD/pause menu flickers.
        const bool inGame =
            g_Game && g_Game->m_EngineClient && g_Game->m_EngineClient->IsInGame();
        if (inGame && hasTexture) {
            desc.visible = 1u;
            desc.valid = 1u;
        }

        desc.widthMeters = 1.30f;
        desc.distanceMeters = 1.20f;
        desc.offsetMeters[0] = 0.0f;
        desc.offsetMeters[1] = -0.12f;
        desc.offsetMeters[2] = 0.0f;
        desc.reserved1 = L4D2VR_OPENXR_OVERLAY_FLAG_BLEND_ALPHA;

    }
    desc.heightMeters = desc.widthMeters * textureAspect;
    desc.orientation[0] = 0.0f;
    desc.orientation[1] = 0.0f;
    desc.orientation[2] = 0.0f;
    desc.orientation[3] = 1.0f;

    L4D2VR_PublishOpenXrOverlay(static_cast<uint32_t>(overlayIndex), desc);

    static std::atomic<int> s_logBudget{ 8 };
    if (s_logBudget.fetch_sub(1, std::memory_order_relaxed) > 0) {
        Game::logMsg(
            "[VR][OverlayBridge] key=%s index=%d visible=%d valid=%d tex=%ux%u image=0x%llX shared=0x%llX sharedValid=%d size=%.3fx%.3fm",
            overlay.key.c_str(), overlayIndex,
            overlay.visible ? 1 : 0, hasTexture ? 1 : 0,
            data.m_nWidth, data.m_nHeight,
            static_cast<unsigned long long>(data.m_nImage),
            static_cast<unsigned long long>(data.m_nSharedHandle),
            data.m_nSharedHandleValid,
            desc.widthMeters, desc.heightMeters);
    }
}

// ---------------------------------------------------------------------------
// Backend
// ---------------------------------------------------------------------------

class HelperBackend {
public:
    static HelperBackend& Instance() {
        static HelperBackend s;
        return s;
    }

    bool Initialize();
    bool EnsureSession() { return m_initialized; }
    void Shutdown();
    bool IsInitialized() const { return m_initialized; }
    int InitError() const { return m_initError; }
    const char* InitErrorDescription() const { return m_initErrorText.c_str(); }
    void Log(const char* fmt, ...) const;

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
    xr::ETrackingUniverseOrigin GetTrackingSpace() const { return xr::TrackingUniverseStanding; }
    void SetExplicitTimingMode(int) {}
    void SubmitExplicitTimingData() {}
    void PostPresentHandoff() {}
    void ClearLastSubmittedFrame() {}

    xr::EVRInputError GetActionHandle(const char* path, xr::VRActionHandle_t* handle);
    xr::EVRInputError GetActionSetHandle(const char* path, xr::VRActionSetHandle_t* handle);
    xr::EVRInputError UpdateActionState();
    xr::EVRInputError GetDigitalActionData(xr::VRActionHandle_t action, xr::InputDigitalActionData_t* data, uint32_t size, xr::VRInputValueHandle_t restrictToDevice);
    xr::EVRInputError GetAnalogActionData(xr::VRActionHandle_t action, xr::InputAnalogActionData_t* data, uint32_t size, xr::VRInputValueHandle_t restrictToDevice);
    xr::EVRInputError GetSkeletalActionData(xr::VRActionHandle_t action, xr::InputSkeletalActionData_t* data, uint32_t size);
    xr::EVRInputError GetSkeletalSummaryData(xr::VRActionHandle_t action, int summaryType, xr::VRSkeletalSummaryData_t* data);
    xr::EVRInputError GetSkeletalBoneData(xr::VRActionHandle_t, int, int, xr::VRBoneTransform_t*, uint32_t) { return xr::VRInputError_Unsupported; }
    xr::EVRInputError GetSkeletalReferenceTransforms(xr::VRActionHandle_t, int, int, xr::VRBoneTransform_t*, uint32_t) { return xr::VRInputError_Unsupported; }
    xr::EVRInputError GetBoneCount(xr::VRActionHandle_t, uint32_t* count) { if (count) *count = 0; return xr::VRInputError_Unsupported; }
    xr::EVRInputError GetBoneHierarchy(xr::VRActionHandle_t, xr::BoneIndex_t*, uint32_t) { return xr::VRInputError_Unsupported; }
    xr::EVRInputError GetBoneName(xr::VRActionHandle_t, xr::BoneIndex_t, char* name, uint32_t size) { if (name && size) name[0] = 0; return xr::VRInputError_Unsupported; }
    xr::EVRInputError GetInputSourceHandle(const char* path, xr::VRInputValueHandle_t* handle);
    xr::EVRInputError GetActionBindingInfo(xr::VRActionHandle_t action, xr::InputBindingInfo_t* bindings, uint32_t bindingSize, uint32_t bindingCount, uint32_t* returnedCount);
    xr::EVRInputError GetOriginTrackedDeviceInfo(xr::VRInputValueHandle_t origin, xr::InputOriginInfo_t* info, uint32_t size);

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

    // Snapshot helpers used by the overlay ray code.
    bool ReadPoseSnapshot(PoseSnapshot& snapshot);

private:
    HelperBackend() = default;

    void SetError(int code, const char* fmt, ...);
    OverlayState* FindOverlayByHandle(xr::VROverlayHandle_t handle);
    bool ResolveOverlayPose(const OverlayState& overlay, const PoseSnapshot& snapshot, Pose& pose) const;
    bool IntersectOverlay(const OverlayState& overlay, const PoseSnapshot& snapshot, const Pose& rayPose, float& u, float& v) const;
    void GenerateOverlayEvents(OverlayState& overlay, const PoseSnapshot& snapshot);
    void BuildPoseSnapshot(const L4D2VROpenXrPoseDesc& hmd, const L4D2VROpenXrInputStateDesc& input, PoseSnapshot& out) const;

    bool m_initialized = false;
    int m_initError = -1;
    std::string m_initErrorText;
    mutable std::mutex m_viewConfigMutex;
    L4D2VROpenXrRuntimeViewConfigDesc m_viewConfig{};
    uint32_t m_viewConfigGeneration = 0;
    std::atomic<uint32_t> m_frameIndex{ 0 };
    std::atomic<uint32_t> m_submitFrameId{ 1 };
    std::atomic<uint32_t> m_lastPublishedFrameId{ 0 };

    std::mutex m_stateMutex;
    L4D2VROpenXrPoseDesc m_hmdPose{};
    uint32_t m_hmdGeneration = 0;
    L4D2VROpenXrInputStateDesc m_inputState{};
    uint32_t m_inputGeneration = 0;

    struct StagedEye {
        bool valid = false;
        xr::VRVulkanTextureData_t texture{};
        bool hasRenderPose = false;
        xr::HmdMatrix34_t renderPose{};
    };
    std::mutex m_submitMutex;
    StagedEye m_staged[2];

    std::mutex m_overlayMutex;
    std::vector<OverlayState> m_overlays;
    std::atomic<uint64_t> m_nextOverlayHandle{ 1 };
};

HelperBackend& B() { return HelperBackend::Instance(); }

void HelperBackend::Log(const char* fmt, ...) const {
    char buffer[2048] = {};
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    Game::logMsg("%s", buffer);
    OutputDebugStringA(buffer);
    OutputDebugStringA("\n");
}

void HelperBackend::SetError(int code, const char* fmt, ...) {
    m_initError = code;
    char buffer[1024] = {};
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    m_initErrorText = buffer;
    Log("[OpenXRHelper] ERROR: %s", buffer);
}

bool HelperBackend::Initialize() {
    if (m_initialized)
        return true;

    m_initError = -1;
    m_initErrorText.clear();

    // g_Game->m_VR is assigned only after this constructor returns, so the
    // caller sets the VR flag itself once Initialize succeeds.

    OpenXrHelperLaunchConfig config;
    config.enabled = true;
    config.submitTestFrames = 0;
    config.waitReadySeconds = 20;
    config.swapProjectionEyes = -1;
    config.swapProjectionViewOrder = false;
    config.mirrorProjectionHorizontal = false;
    config.swapGameEyeOrigins = false;
    config.disableQuadOverlays = false;
    config.enableHandTracking = false;
    config.disableProjectionLayer = false;
    config.useSymmetricProjectionFov = false;
    config.useGameRenderPoseForProjection = true;
    config.forceMonoProjectionEye = -1;
    config.forceMonoProjectionView = -1;
    config.flipSubmitY = -1;

    if (!L4D2VR_StartOpenXrHelper(config)) {
        SetError(30, "The 64-bit OpenXR helper could not be started. Expected it at VR\\openxr_helper64\\OpenXRHelper64.exe.");
        return false;
    }

    Log("[OpenXRHelper] helper launched; waiting for runtime view configuration");
    for (int i = 0; i < 400; ++i) {
        L4D2VROpenXrRuntimeViewConfigDesc configDesc{};
        uint32_t generation = 0;
        if (L4D2VR_ReadOpenXrRuntimeViewConfig(configDesc, &generation) &&
            configDesc.valid && configDesc.viewCount >= 2 &&
            configDesc.reserved0 == L4D2VR_OPENXR_RUNTIME_VIEW_FOV_LOCATED &&
            std::fabs(configDesc.views[0].angleLeft) + std::fabs(configDesc.views[0].angleRight) > 0.01f) {
            {
                std::lock_guard<std::mutex> lock(m_viewConfigMutex);
                m_viewConfig = configDesc;
                m_viewConfigGeneration = generation;
            }
            m_initialized = true;
            Log("[OpenXRHelper] runtime view config gen=%u views=%u size=%ux%u fov L=(%.3f %.3f %.3f %.3f) R=(%.3f %.3f %.3f %.3f)",
                generation,
                configDesc.viewCount,
                configDesc.views[0].width,
                configDesc.views[0].height,
                configDesc.views[0].angleLeft, configDesc.views[0].angleRight,
                configDesc.views[0].angleUp, configDesc.views[0].angleDown,
                configDesc.views[1].angleLeft, configDesc.views[1].angleRight,
                configDesc.views[1].angleUp, configDesc.views[1].angleDown);
            return true;
        }
        Sleep(50);
    }

    SetError(31, "Timed out waiting for the OpenXR helper to publish its runtime view configuration. See VR\\openxr_helper64\\openxr_helper64.log.");
    return false;
}

void HelperBackend::Shutdown() {
    // The helper watches the game process and exits with it.  Nothing owns
    // OpenXR state in this process.
    m_initialized = false;
}

void HelperBackend::GetRecommendedRenderTargetSize(uint32_t* w, uint32_t* h) const {
    uint32_t width = 0, height = 0;
    std::lock_guard<std::mutex> lock(m_viewConfigMutex);
    if (m_viewConfig.valid && m_viewConfig.viewCount >= 1) {
        width = m_viewConfig.views[0].width;
        height = m_viewConfig.views[0].height;
    }
    if (!width || !height) {
        width = 1280;
        height = 1440;
    }
    if (w) *w = width;
    if (h) *h = height;
}

void HelperBackend::GetProjectionRaw(int eye, float* left, float* right, float* top, float* bottom) const {
    float l = -0.785398f, r = 0.785398f, t = -0.785398f, b = 0.785398f;
    std::lock_guard<std::mutex> lock(m_viewConfigMutex);
    if (m_viewConfig.valid && m_viewConfig.viewCount > (uint32_t)eye &&
        std::fabs(m_viewConfig.views[eye].angleLeft) + std::fabs(m_viewConfig.views[eye].angleRight) > 0.01f) {
        const auto& view = m_viewConfig.views[eye];
        l = std::tan(view.angleLeft);
        r = std::tan(view.angleRight);
        t = -std::tan(view.angleUp);
        b = -std::tan(view.angleDown);
    }
    if (left) *left = l;
    if (right) *right = r;
    if (top) *top = t;
    if (bottom) *bottom = b;
}

xr::HmdMatrix34_t HelperBackend::GetEyeToHeadTransform(int eye) const {
    const float ipd = 0.063f;
    xr::HmdMatrix34_t out = {};
    out.m[0][0] = 1.0f; out.m[1][1] = 1.0f; out.m[2][2] = 1.0f;
    out.m[0][3] = (eye == xr::Eye_Left) ? -ipd * 0.5f : ipd * 0.5f;
    return out;
}

xr::TrackedDeviceIndex_t HelperBackend::GetTrackedDeviceIndexForControllerRole(int role) const {
    if (role == xr::TrackedControllerRole_LeftHand) return 1;
    if (role == xr::TrackedControllerRole_RightHand) return 2;
    return xr::k_unTrackedDeviceIndexInvalid;
}

void HelperBackend::BuildPoseSnapshot(const L4D2VROpenXrPoseDesc& hmd, const L4D2VROpenXrInputStateDesc& input, PoseSnapshot& out) const {
    out = {};
    if (hmd.valid) {
        out.valid = true;
        out.hmd = PoseToMatrix(MakePose(hmd.position, hmd.orientation));
    }

    const bool leftHanded = g_Game && g_Game->m_VR && g_Game->m_VR->m_LeftHanded;
    for (int physical = 0; physical < 2; ++physical) {
        const int logical = leftHanded ? (1 - physical) : physical;
        const auto& grip = input.controllerPoses[physical];
        const auto& aim = input.controllerAimPoses[physical];
        out.handsValid[logical] = grip.valid != 0;
        out.handGrip[logical] = FromController(grip);
        out.handAim[logical] = aim.valid != 0 ? FromController(aim) : out.handGrip[logical];
    }

    auto actionState = [&](L4D2VROpenXrActionId id) -> bool {
        const uint32_t index = static_cast<uint32_t>(id);
        return index < L4D2VR_OPENXR_ACTION_COUNT && input.digitalActions[index].active && input.digitalActions[index].state;
    };
    auto actionValue = [&](L4D2VROpenXrActionId id) -> float {
        const uint32_t index = static_cast<uint32_t>(id);
        return index < L4D2VR_OPENXR_ACTION_COUNT ? input.analogActions[index].x : 0.0f;
    };

    out.trigger[0] = std::clamp(-actionValue(L4D2VROpenXrActionId::SecondaryAttack), 0.0f, 1.0f);
    out.trigger[1] = std::clamp(actionValue(L4D2VROpenXrActionId::PrimaryAttack), 0.0f, 1.0f);
    out.stickY[0] = actionValue(L4D2VROpenXrActionId::Walk);
    out.stickY[1] = actionValue(L4D2VROpenXrActionId::Turn);
    out.primary[0] = actionState(L4D2VROpenXrActionId::Scoreboard);
    out.secondary[0] = actionState(L4D2VROpenXrActionId::Pause);
    out.primary[1] = actionState(L4D2VROpenXrActionId::ActivateVR);
    out.secondary[1] = actionState(L4D2VROpenXrActionId::Use);
}

bool HelperBackend::ReadPoseSnapshot(PoseSnapshot& snapshot) {
    // The helper republishes the view config once xrLocateViews has produced
    // the runtime FOV.  Refresh it here so projection updates after startup.
    {
        L4D2VROpenXrRuntimeViewConfigDesc latest{};
        uint32_t generation = 0;
        if (L4D2VR_ReadOpenXrRuntimeViewConfig(latest, &generation) &&
            latest.valid && generation != m_viewConfigGeneration) {
            std::lock_guard<std::mutex> lock(m_viewConfigMutex);
            m_viewConfig = latest;
            m_viewConfigGeneration = generation;
        }
    }
    L4D2VROpenXrPoseDesc hmd{};
    L4D2VROpenXrInputStateDesc input{};
    uint32_t hmdGen = 0, inputGen = 0;
    if (!L4D2VR_ReadOpenXrHmdPose(hmd, &hmdGen))
        return false;
    L4D2VR_ReadOpenXrInputState(input, &inputGen);
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_hmdPose = hmd;
        m_hmdGeneration = hmdGen;
        if (inputGen != 0) {
            m_inputState = input;
            m_inputGeneration = inputGen;
        } else {
            input = m_inputState;
        }
    }
    BuildPoseSnapshot(hmd, input, snapshot);
    return snapshot.valid;
}

void HelperBackend::GetDeviceToAbsoluteTrackingPose(int origin, float predictedSeconds, xr::TrackedDevicePose_t* poses, uint32_t count) {
    (void)origin;
    (void)predictedSeconds;
    if (!poses || !count)
        return;

    PoseSnapshot snapshot;
    ReadPoseSnapshot(snapshot);

    const uint32_t maxCount = (std::min)(count, xr::k_unMaxTrackedDeviceCount);
    for (uint32_t i = 0; i < maxCount; ++i)
        poses[i] = xr::TrackedDevicePose_t{};

    if (maxCount > xr::k_unTrackedDeviceIndex_Hmd) {
        xr::TrackedDevicePose_t& hmd = poses[xr::k_unTrackedDeviceIndex_Hmd];
        hmd.bDeviceIsConnected = true;
        hmd.bPoseIsValid = snapshot.valid;
        hmd.eTrackingResult = snapshot.valid ? 0 : 1;
        if (snapshot.valid)
            hmd.mDeviceToAbsoluteTracking = snapshot.hmd;
    }

    const bool leftHanded = g_Game && g_Game->m_VR && g_Game->m_VR->m_LeftHanded;
    for (int hand = 0; hand < 2; ++hand) {
        const int physical = leftHanded ? (1 - hand) : hand;
        const uint32_t index = physical == 0 ? 1u : 2u;
        if (index >= maxCount)
            continue;
        xr::TrackedDevicePose_t& pose = poses[index];
        pose.bDeviceIsConnected = snapshot.handsValid[hand];
        pose.bPoseIsValid = snapshot.handsValid[hand];
        pose.eTrackingResult = snapshot.handsValid[hand] ? 0 : 1;
        if (snapshot.handsValid[hand])
            pose.mDeviceToAbsoluteTracking = PoseToMatrix(snapshot.handGrip[hand]);
    }
}

bool HelperBackend::IsTrackedDeviceConnected(xr::TrackedDeviceIndex_t index) {
    if (index == xr::k_unTrackedDeviceIndex_Hmd)
        return m_initialized;
    PoseSnapshot snapshot;
    ReadPoseSnapshot(snapshot);
    return (index == 1 || index == 2) && snapshot.handsValid[index - 1];
}

float HelperBackend::GetFloatTrackedDeviceProperty(xr::TrackedDeviceIndex_t index, xr::ETrackedDeviceProperty prop, xr::ETrackedPropertyError* error) {
    (void)index;
    if (prop == xr::Prop_DisplayFrequency_Float) {
        if (error) *error = xr::TrackedProp_Success;
        return 90.0f;
    }
    if (error) *error = xr::TrackedProp_UnknownProperty;
    return 0.0f;
}

uint32_t HelperBackend::GetStringTrackedDeviceProperty(xr::TrackedDeviceIndex_t index, xr::ETrackedDeviceProperty prop, char* buffer, uint32_t bufferSize, xr::ETrackedPropertyError* error) {
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
    const uint32_t length = static_cast<uint32_t>(std::strlen(value)) + 1;
    if (buffer && bufferSize) {
        const uint32_t copy = (std::min)(length, bufferSize);
        std::memcpy(buffer, value, copy);
        buffer[copy - 1] = '\0';
    }
    return length;
}

void HelperBackend::TriggerHapticPulse(xr::TrackedDeviceIndex_t index, uint32_t axisId, uint16_t durationMicroSec) {
    (void)axisId;
    if (index != 1 && index != 2)
        return;
    const int hand = index == 1 ? 0 : 1;
    L4D2VR_PublishOpenXrHapticRequest(hand, durationMicroSec * 0.000001f, 0.0f, 1.0f);
}

xr::EVRCompositorError HelperBackend::WaitGetPoses(xr::TrackedDevicePose_t* renderPoses, uint32_t renderCount, xr::TrackedDevicePose_t* gamePoses, uint32_t gameCount) {
    PoseSnapshot snapshot;
    if (!ReadPoseSnapshot(snapshot))
        return xr::VRCompositorError_RequestFailed;

    auto fill = [&](xr::TrackedDevicePose_t* poses, uint32_t count) {
        if (!poses || !count)
            return;
        const uint32_t maxCount = (std::min)(count, xr::k_unMaxTrackedDeviceCount);
        for (uint32_t i = 0; i < maxCount; ++i)
            poses[i] = xr::TrackedDevicePose_t{};
        if (maxCount > 0) {
            xr::TrackedDevicePose_t& hmd = poses[0];
            hmd.bDeviceIsConnected = true;
            hmd.bPoseIsValid = true;
            hmd.eTrackingResult = 0;
            hmd.mDeviceToAbsoluteTracking = snapshot.hmd;
        }
        for (int hand = 0; hand < 2; ++hand) {
            const uint32_t index = hand == 0 ? 1u : 2u;
            if (index >= maxCount || !snapshot.handsValid[hand])
                continue;
            xr::TrackedDevicePose_t& pose = poses[index];
            pose.bDeviceIsConnected = true;
            pose.bPoseIsValid = true;
            pose.eTrackingResult = 0;
            pose.mDeviceToAbsoluteTracking = PoseToMatrix(snapshot.handGrip[hand]);
        }
    };
    fill(renderPoses, renderCount);
    fill(gamePoses, gameCount);
    return xr::VRCompositorError_None;
}

xr::EVRCompositorError HelperBackend::Submit(int eye, const xr::Texture_t* texture, const xr::VRTextureBounds_t* bounds, int flags) {
    (void)bounds;
    if (eye < 0 || eye > 1 || !texture || !texture->handle)
        return xr::VRCompositorError_RequestFailed;

    const xr::VRVulkanTextureData_t* data = reinterpret_cast<const xr::VRVulkanTextureData_t*>(texture->handle);
    if (!data->m_nImage || !data->m_nWidth || !data->m_nHeight || !data->m_nSharedHandleValid || !data->m_nSharedHandle) {
        static std::atomic<int> s_missingLogBudget{ 8 };
        if (s_missingLogBudget.fetch_sub(1) > 0) {
            Log("[OpenXRHelper] Submit eye=%d has no KMT shared handle (image=%llu size=%ux%u valid=%u) - letting the game continue",
                eye,
                (unsigned long long)data->m_nImage,
                data->m_nWidth,
                data->m_nHeight,
                data->m_nSharedHandleValid);
        }
        // Do not stall Source's present/render loop just because the helper
        // cannot consume this frame; the desktop game should remain usable.
        return xr::VRCompositorError_None;
    }

    // Make sure the D3D9 frame that produced this texture has reached the GPU
    // before the helper samples it through the shared KMT handle.
    if (g_D3DVR9)
        g_D3DVR9->WaitDeviceIdle();

    auto makeSharedDesc = [](const xr::VRVulkanTextureData_t& d) {
        L4D2VROpenXrSharedTextureDesc shared{};
        shared.valid = 1;
        shared.width = d.m_nWidth;
        shared.height = d.m_nHeight;
        shared.format = d.m_nFormat;
        shared.sampleCount = d.m_nSampleCount ? d.m_nSampleCount : 1;
        shared.handleType = d.m_nSharedHandleType;
        shared.queueFamilyIndex = d.m_nQueueFamilyIndex;
        shared.kmtHandle = d.m_nSharedHandle;
        shared.image = d.m_nImage;
        shared.uMin = 0.0f;
        shared.vMin = 0.0f;
        shared.uMax = 1.0f;
        shared.vMax = 1.0f;
        const float gameFov = (g_Game && g_Game->m_VR) ? g_Game->m_VR->m_Fov : 90.0f;
        const float gameAspect = (shared.height > 0)
            ? (static_cast<float>(shared.width) / static_cast<float>(shared.height))
            : ((g_Game && g_Game->m_VR) ? g_Game->m_VR->m_Aspect : 1.0f);
        shared.renderFovXDeg = (std::isfinite(gameFov) && gameFov > 1.0f && gameFov < 179.0f) ? gameFov : 90.0f;
        shared.renderAspect = (std::isfinite(gameAspect) && gameAspect > 0.1f && gameAspect < 10.0f)
            ? gameAspect
            : (shared.height > 0 ? static_cast<float>(shared.width) / static_cast<float>(shared.height) : 1.0f);
        return shared;
    };

    uint32_t frameId = 0;
    L4D2VROpenXrSharedTextureDesc leftShared{};
    L4D2VROpenXrSharedTextureDesc rightShared{};
    bool hasRenderPose = false;
    xr::HmdMatrix34_t renderPose{};
    {
        std::lock_guard<std::mutex> lock(m_submitMutex);
        StagedEye& staged = m_staged[eye];
        staged.valid = true;
        staged.texture = *data;
        staged.hasRenderPose = (flags & xr::Submit_TextureWithPose) != 0;
        if (staged.hasRenderPose) {
            const auto* withPose = reinterpret_cast<const xr::VRTextureWithPose_t*>(texture);
            staged.renderPose = withPose->mDeviceToAbsoluteTracking;
        }

        if (eye == 0)
            return xr::VRCompositorError_None;
        if (!m_staged[0].valid || !m_staged[1].valid)
            return xr::VRCompositorError_RequestFailed;

        leftShared = makeSharedDesc(m_staged[0].texture);
        rightShared = makeSharedDesc(m_staged[1].texture);
        frameId = m_submitFrameId.fetch_add(1);
        hasRenderPose = m_staged[1].hasRenderPose;
        renderPose = m_staged[1].renderPose;
        m_staged[0].valid = false;
        m_staged[1].valid = false;
    }

    L4D2VR_PublishOpenXrSharedTexturePair(leftShared, rightShared);
    L4D2VR_PublishOpenXrSharedTextureFrame(frameId);
    L4D2VR_PublishOpenXrOverlayFrame(frameId);
    m_lastPublishedFrameId.store(frameId, std::memory_order_release);

    // Projection must use the same stage-space HMD pose the game used for its
    // RenderView. The TextureWithPose matrix is player-relative in this mod and
    // is not an OpenXR stage pose.
    L4D2VROpenXrPoseDesc gameRenderPose{};
    {
        uint32_t gen = 0;
        if (L4D2VR_ReadOpenXrHmdPose(gameRenderPose, &gen)) {
            const float renderIpd = 0.063f;
            std::memcpy(&gameRenderPose.reserved0, &renderIpd, sizeof(renderIpd));
        }
    }
    (void)hasRenderPose;
    (void)renderPose;
    if (gameRenderPose.valid)
        L4D2VR_PublishOpenXrGameRenderPose(gameRenderPose);

    // The helper blits and waits for its GPU queue before advancing
    // helperConsumedFrameId.  Waiting here keeps the fixed eye RTs from being
    // overwritten while the helper still samples them.
    bool consumedFrame = false;
    for (int i = 0; i < 50; ++i) {
        uint32_t consuming = 0, consumed = 0, count = 0;
        if (L4D2VR_ReadOpenXrHelperConsumedFrame(consuming, consumed, count) && consumed >= frameId) {
            consumedFrame = true;
            break;
        }
        Sleep(1);
    }
    if (!consumedFrame) {
        static std::atomic<int> s_noConsumeLogBudget{ 8 };
        if (s_noConsumeLogBudget.fetch_sub(1) > 0)
            Log("[OpenXRHelper] helper did not consume frame=%u (timeout)", frameId);
    }

    m_frameIndex.fetch_add(1);
    return xr::VRCompositorError_None;
}

bool HelperBackend::GetFrameTiming(xr::Compositor_FrameTiming* timing, uint32_t framesAgo) {
    (void)framesAgo;
    if (!timing)
        return false;
    std::memset(timing, 0, sizeof(*timing));
    timing->m_nSize = sizeof(*timing);
    timing->m_nFrameIndex = m_frameIndex.load();
    return timing->m_nFrameIndex != 0;
}

float HelperBackend::GetFrameTimeRemaining() {
    return 0.005f;
}

xr::EVRInputError HelperBackend::GetActionHandle(const char* path, xr::VRActionHandle_t* handle) {
    if (!path || !handle)
        return xr::VRInputError_InvalidParam;
    const int id = ActionIdForPath(path);
    if (id == 0) {
        *handle = xr::k_ulInvalidActionHandle;
        return xr::VRInputError_NameNotFound;
    }
    *handle = static_cast<xr::VRActionHandle_t>(id);
    return xr::VRInputError_None;
}

xr::EVRInputError HelperBackend::GetActionSetHandle(const char* path, xr::VRActionSetHandle_t* handle) {
    if (!path || !handle)
        return xr::VRInputError_InvalidParam;
    if (std::strcmp(path, "/actions/main") == 0) { *handle = 1; return xr::VRInputError_None; }
    if (std::strcmp(path, "/actions/base") == 0) { *handle = 2; return xr::VRInputError_None; }
    *handle = xr::k_ulInvalidActionSetHandle;
    return xr::VRInputError_NameNotFound;
}

xr::EVRInputError HelperBackend::UpdateActionState() {
    L4D2VROpenXrPoseDesc hmd{};
    L4D2VROpenXrInputStateDesc input{};
    uint32_t hmdGen = 0, inputGen = 0;
    L4D2VR_ReadOpenXrHmdPose(hmd, &hmdGen);
    L4D2VR_ReadOpenXrInputState(input, &inputGen);
    if (inputGen == 0)
        return xr::VRInputError_NoActiveActionSet;
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_hmdPose = hmd;
    m_hmdGeneration = hmdGen;
    m_inputState = input;
    m_inputGeneration = inputGen;
    return xr::VRInputError_None;
}

xr::EVRInputError HelperBackend::GetDigitalActionData(xr::VRActionHandle_t action, xr::InputDigitalActionData_t* data, uint32_t size, xr::VRInputValueHandle_t restrictToDevice) {
    (void)restrictToDevice;
    if (!data || size < sizeof(*data))
        return xr::VRInputError_InvalidParam;
    *data = {};
    const int id = static_cast<int>(action);
    L4D2VROpenXrInputStateDesc input;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (m_inputGeneration == 0)
            return xr::VRInputError_NoActiveActionSet;
        input = m_inputState;
    }

    if (id == 1001 || id == 1002) {
        const float turn = input.analogActions[static_cast<uint32_t>(L4D2VROpenXrActionId::Turn)].x;
        data->bActive = true;
        data->bState = id == 1001 ? (turn < -0.7f) : (turn > 0.7f);
        data->bChanged = true;
        data->activeOrigin = xr::k_unInputSourceRight;
        return xr::VRInputError_None;
    }

    if (id <= 0 || id >= static_cast<int>(L4D2VR_OPENXR_ACTION_COUNT))
        return xr::VRInputError_InvalidHandle;
    const auto& state = input.digitalActions[id];
    data->bActive = state.active != 0;
    data->bState = state.state != 0;
    data->bChanged = state.changed != 0;
    data->activeOrigin = state.activeOrigin ? state.activeOrigin : xr::k_unInputSourceLeft;
    return xr::VRInputError_None;
}

xr::EVRInputError HelperBackend::GetAnalogActionData(xr::VRActionHandle_t action, xr::InputAnalogActionData_t* data, uint32_t size, xr::VRInputValueHandle_t restrictToDevice) {
    (void)restrictToDevice;
    if (!data || size < sizeof(*data))
        return xr::VRInputError_InvalidParam;
    *data = {};
    const int id = static_cast<int>(action);
    L4D2VROpenXrInputStateDesc input;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (m_inputGeneration == 0)
            return xr::VRInputError_NoActiveActionSet;
        input = m_inputState;
    }
    if (id <= 0 || id >= static_cast<int>(L4D2VR_OPENXR_ACTION_COUNT))
        return xr::VRInputError_InvalidHandle;
    const auto& state = input.analogActions[id];
    data->bActive = state.active != 0;
    data->x = state.x;
    data->y = state.y;
    data->activeOrigin = state.activeOrigin ? state.activeOrigin : xr::k_unInputSourceLeft;
    return xr::VRInputError_None;
}

xr::EVRInputError HelperBackend::GetSkeletalActionData(xr::VRActionHandle_t action, xr::InputSkeletalActionData_t* data, uint32_t size) {
    if (!data || size < sizeof(*data))
        return xr::VRInputError_InvalidParam;
    const int id = static_cast<int>(action);
    const int hand = id == kSkeletonLeftHandle ? 0 : (id == kSkeletonRightHandle ? 1 : -1);
    if (hand < 0)
        return xr::VRInputError_InvalidHandle;
    *data = {};
    L4D2VROpenXrInputStateDesc input;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (m_inputGeneration == 0)
            return xr::VRInputError_NoActiveActionSet;
        input = m_inputState;
    }
    data->bActive = input.handTracking[hand].active != 0 || input.controllerPoses[hand].valid != 0;
    data->activeOrigin = hand == 0 ? xr::k_unInputSourceLeft : xr::k_unInputSourceRight;
    return xr::VRInputError_None;
}

xr::EVRInputError HelperBackend::GetSkeletalSummaryData(xr::VRActionHandle_t action, int summaryType, xr::VRSkeletalSummaryData_t* data) {
    (void)summaryType;
    if (!data)
        return xr::VRInputError_InvalidParam;
    const int id = static_cast<int>(action);
    const int hand = id == kSkeletonLeftHandle ? 0 : (id == kSkeletonRightHandle ? 1 : -1);
    if (hand < 0)
        return xr::VRInputError_InvalidHandle;
    *data = {};
    L4D2VROpenXrInputStateDesc input;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (m_inputGeneration == 0)
            return xr::VRInputError_NoActiveActionSet;
        input = m_inputState;
    }
    for (int i = 0; i < 5; ++i)
        data->flFingerCurl[i] = input.handTracking[hand].fingerCurls[i];
    return xr::VRInputError_None;
}

xr::EVRInputError HelperBackend::GetInputSourceHandle(const char* path, xr::VRInputValueHandle_t* handle) {
    if (!path || !handle)
        return xr::VRInputError_InvalidParam;
    if (std::strcmp(path, "/user/hand/left") == 0) { *handle = xr::k_unInputSourceLeft; return xr::VRInputError_None; }
    if (std::strcmp(path, "/user/hand/right") == 0) { *handle = xr::k_unInputSourceRight; return xr::VRInputError_None; }
    if (std::strcmp(path, "/user/head") == 0) { *handle = xr::k_unInputSourceHead; return xr::VRInputError_None; }
    *handle = xr::k_ulInvalidInputValueHandle;
    return xr::VRInputError_NameNotFound;
}

xr::EVRInputError HelperBackend::GetActionBindingInfo(xr::VRActionHandle_t action, xr::InputBindingInfo_t* bindings, uint32_t bindingSize, uint32_t bindingCount, uint32_t* returnedCount) {
    (void)action;
    if (returnedCount) *returnedCount = 0;
    if (!bindings || bindingSize < sizeof(*bindings) || bindingCount == 0)
        return xr::VRInputError_InvalidParam;
    std::memset(bindings, 0, sizeof(*bindings));
    std::snprintf(bindings[0].rchDevicePathName, sizeof(bindings[0].rchDevicePathName), "/user/hand/right");
    std::snprintf(bindings[0].rchInputPathName, sizeof(bindings[0].rchInputPathName), "/input/trigger/value");
    std::snprintf(bindings[0].rchModeName, sizeof(bindings[0].rchModeName), "button");
    std::snprintf(bindings[0].rchSlotName, sizeof(bindings[0].rchSlotName), "trigger");
    std::snprintf(bindings[0].rchInputSourceType, sizeof(bindings[0].rchInputSourceType), "button");
    if (returnedCount) *returnedCount = 1;
    return xr::VRInputError_None;
}

xr::EVRInputError HelperBackend::GetOriginTrackedDeviceInfo(xr::VRInputValueHandle_t origin, xr::InputOriginInfo_t* info, uint32_t size) {
    if (!info || size < sizeof(*info))
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
// Overlays
// ---------------------------------------------------------------------------

OverlayState* HelperBackend::FindOverlayByHandle(xr::VROverlayHandle_t handle) {
    for (auto& overlay : m_overlays) {
        if (overlay.handle == handle)
            return &overlay;
    }
    return nullptr;
}

xr::EVROverlayError HelperBackend::CreateOverlay(const char* key, const char* name, xr::VROverlayHandle_t* handle) {
    if (!key || !name || !handle)
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

xr::EVROverlayError HelperBackend::DestroyOverlay(xr::VROverlayHandle_t handle) {
    std::lock_guard<std::mutex> lock(m_overlayMutex);
    for (auto it = m_overlays.begin(); it != m_overlays.end(); ++it) {
        if (it->handle == handle) {
            m_overlays.erase(it);
            return xr::VROverlayError_None;
        }
    }
    return xr::VROverlayError_UnknownOverlay;
}

xr::EVROverlayError HelperBackend::FindOverlay(const char* key, xr::VROverlayHandle_t* handle) {
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

xr::EVROverlayError HelperBackend::SetOverlayRaw(xr::VROverlayHandle_t handle, void* buffer, uint32_t width, uint32_t height, uint32_t depth) {
    // CPU overlay raw textures are not used by the helper path yet.  Keep the
    // call successful so the mod's fallback HUD upload path does not spam.
    (void)handle; (void)buffer; (void)width; (void)height; (void)depth;
    return xr::VROverlayError_RequestFailed;
}

xr::EVROverlayError HelperBackend::SetOverlaySortOrder(xr::VROverlayHandle_t handle, uint32_t sortOrder) {
    std::lock_guard<std::mutex> lock(m_overlayMutex);
    OverlayState* overlay = FindOverlayByHandle(handle);
    if (!overlay) return xr::VROverlayError_UnknownOverlay;
    overlay->sortOrder = sortOrder;
    return xr::VROverlayError_None;
}

xr::EVROverlayError HelperBackend::SetOverlayTexture(xr::VROverlayHandle_t handle, const xr::Texture_t* texture) {
    if (!texture || !texture->handle)
        return xr::VROverlayError_InvalidTexture;
    const auto* data = reinterpret_cast<const xr::VRVulkanTextureData_t*>(texture->handle);
    if (!data->m_nImage || !data->m_nWidth || !data->m_nHeight)
        return xr::VROverlayError_InvalidTexture;
    std::lock_guard<std::mutex> lock(m_overlayMutex);
    OverlayState* overlay = FindOverlayByHandle(handle);
    if (!overlay)
        return xr::VROverlayError_UnknownOverlay;
    overlay->texture = *data;
    overlay->hasTexture = true;
    PublishOverlayToBridge(*overlay);
    return xr::VROverlayError_None;
}

xr::EVROverlayError HelperBackend::SetOverlayTextureBounds(xr::VROverlayHandle_t handle, const xr::VRTextureBounds_t* bounds) {
    if (!bounds) return xr::VROverlayError_InvalidParameter;
    std::lock_guard<std::mutex> lock(m_overlayMutex);
    OverlayState* overlay = FindOverlayByHandle(handle);
    if (!overlay) return xr::VROverlayError_UnknownOverlay;
    overlay->bounds = *bounds;
    PublishOverlayToBridge(*overlay);
    return xr::VROverlayError_None;
}

xr::EVROverlayError HelperBackend::SetOverlayTransformAbsolute(xr::VROverlayHandle_t handle, int origin, const xr::HmdMatrix34_t* transform) {
    if (!transform) return xr::VROverlayError_InvalidParameter;
    std::lock_guard<std::mutex> lock(m_overlayMutex);
    OverlayState* overlay = FindOverlayByHandle(handle);
    if (!overlay) return xr::VROverlayError_UnknownOverlay;
    overlay->transformType = xr::VROverlayTransform_Absolute;
    overlay->transformOrigin = origin;
    overlay->transform = *transform;
    return xr::VROverlayError_None;
}

xr::EVROverlayError HelperBackend::SetOverlayTransformTrackedDeviceRelative(xr::VROverlayHandle_t handle, xr::TrackedDeviceIndex_t device, const xr::HmdMatrix34_t* transform) {
    if (!transform) return xr::VROverlayError_InvalidParameter;
    std::lock_guard<std::mutex> lock(m_overlayMutex);
    OverlayState* overlay = FindOverlayByHandle(handle);
    if (!overlay) return xr::VROverlayError_UnknownOverlay;
    overlay->transformType = xr::VROverlayTransform_TrackedDeviceRelative;
    overlay->transformDevice = device;
    overlay->transform = *transform;
    return xr::VROverlayError_None;
}

xr::EVROverlayError HelperBackend::GetOverlayTransformType(xr::VROverlayHandle_t handle, int* transformType) {
    if (!transformType) return xr::VROverlayError_InvalidParameter;
    std::lock_guard<std::mutex> lock(m_overlayMutex);
    OverlayState* overlay = FindOverlayByHandle(handle);
    if (!overlay) return xr::VROverlayError_UnknownOverlay;
    *transformType = overlay->transformType;
    return xr::VROverlayError_None;
}

xr::EVROverlayError HelperBackend::GetOverlayTransformAbsolute(xr::VROverlayHandle_t handle, int* origin, xr::HmdMatrix34_t* transform) {
    if (!origin || !transform) return xr::VROverlayError_InvalidParameter;
    std::lock_guard<std::mutex> lock(m_overlayMutex);
    OverlayState* overlay = FindOverlayByHandle(handle);
    if (!overlay || overlay->transformType != xr::VROverlayTransform_Absolute)
        return xr::VROverlayError_WrongTransformType;
    *origin = overlay->transformOrigin;
    *transform = overlay->transform;
    return xr::VROverlayError_None;
}

xr::EVROverlayError HelperBackend::GetOverlayTransformTrackedDeviceRelative(xr::VROverlayHandle_t handle, xr::TrackedDeviceIndex_t* device, xr::HmdMatrix34_t* transform) {
    if (!device || !transform) return xr::VROverlayError_InvalidParameter;
    std::lock_guard<std::mutex> lock(m_overlayMutex);
    OverlayState* overlay = FindOverlayByHandle(handle);
    if (!overlay || overlay->transformType != xr::VROverlayTransform_TrackedDeviceRelative)
        return xr::VROverlayError_WrongTransformType;
    *device = overlay->transformDevice;
    *transform = overlay->transform;
    return xr::VROverlayError_None;
}

xr::EVROverlayError HelperBackend::ShowOverlay(xr::VROverlayHandle_t handle) {
    std::lock_guard<std::mutex> lock(m_overlayMutex);
    OverlayState* overlay = FindOverlayByHandle(handle);
    if (!overlay) return xr::VROverlayError_UnknownOverlay;
    overlay->visible = true;
    PublishOverlayToBridge(*overlay);
    return xr::VROverlayError_None;
}

xr::EVROverlayError HelperBackend::HideOverlay(xr::VROverlayHandle_t handle) {
    std::lock_guard<std::mutex> lock(m_overlayMutex);
    OverlayState* overlay = FindOverlayByHandle(handle);
    if (!overlay) return xr::VROverlayError_UnknownOverlay;
    overlay->visible = false;
    overlay->events.clear();
    PublishOverlayToBridge(*overlay);
    return xr::VROverlayError_None;
}

bool HelperBackend::IsOverlayVisible(xr::VROverlayHandle_t handle) {
    std::lock_guard<std::mutex> lock(m_overlayMutex);
    OverlayState* overlay = FindOverlayByHandle(handle);
    return overlay && overlay->visible;
}

xr::EVROverlayError HelperBackend::SetOverlayWidthInMeters(xr::VROverlayHandle_t handle, float width) {
    std::lock_guard<std::mutex> lock(m_overlayMutex);
    OverlayState* overlay = FindOverlayByHandle(handle);
    if (!overlay) return xr::VROverlayError_UnknownOverlay;
    overlay->widthMeters = width;
    PublishOverlayToBridge(*overlay);
    return xr::VROverlayError_None;
}

xr::EVROverlayError HelperBackend::GetOverlayWidthInMeters(xr::VROverlayHandle_t handle, float* width) {
    if (!width) return xr::VROverlayError_InvalidParameter;
    std::lock_guard<std::mutex> lock(m_overlayMutex);
    OverlayState* overlay = FindOverlayByHandle(handle);
    if (!overlay) return xr::VROverlayError_UnknownOverlay;
    *width = overlay->widthMeters;
    return xr::VROverlayError_None;
}

xr::EVROverlayError HelperBackend::SetOverlayTexelAspect(xr::VROverlayHandle_t handle, float aspect) {
    std::lock_guard<std::mutex> lock(m_overlayMutex);
    OverlayState* overlay = FindOverlayByHandle(handle);
    if (!overlay) return xr::VROverlayError_UnknownOverlay;
    overlay->texelAspect = aspect;
    return xr::VROverlayError_None;
}

xr::EVROverlayError HelperBackend::SetOverlayAlpha(xr::VROverlayHandle_t handle, float alpha) {
    std::lock_guard<std::mutex> lock(m_overlayMutex);
    OverlayState* overlay = FindOverlayByHandle(handle);
    if (!overlay) return xr::VROverlayError_UnknownOverlay;
    overlay->alpha = alpha;
    return xr::VROverlayError_None;
}

xr::EVROverlayError HelperBackend::SetOverlayCurvature(xr::VROverlayHandle_t handle, float curvature) {
    std::lock_guard<std::mutex> lock(m_overlayMutex);
    OverlayState* overlay = FindOverlayByHandle(handle);
    if (!overlay) return xr::VROverlayError_UnknownOverlay;
    overlay->curvature = curvature;
    return xr::VROverlayError_None;
}

xr::EVROverlayError HelperBackend::SetOverlayInputMethod(xr::VROverlayHandle_t handle, int method) {
    std::lock_guard<std::mutex> lock(m_overlayMutex);
    OverlayState* overlay = FindOverlayByHandle(handle);
    if (!overlay) return xr::VROverlayError_UnknownOverlay;
    overlay->inputMethod = method;
    return xr::VROverlayError_None;
}

xr::EVROverlayError HelperBackend::SetOverlayFlag(xr::VROverlayHandle_t handle, uint32_t flag, bool enabled) {
    std::lock_guard<std::mutex> lock(m_overlayMutex);
    OverlayState* overlay = FindOverlayByHandle(handle);
    if (!overlay) return xr::VROverlayError_UnknownOverlay;
    if (enabled) overlay->flags |= flag;
    else overlay->flags &= ~flag;
    return xr::VROverlayError_None;
}

xr::EVROverlayError HelperBackend::SetOverlayMouseScale(xr::VROverlayHandle_t handle, const xr::HmdVector2_t* scale) {
    if (!scale) return xr::VROverlayError_InvalidParameter;
    std::lock_guard<std::mutex> lock(m_overlayMutex);
    OverlayState* overlay = FindOverlayByHandle(handle);
    if (!overlay) return xr::VROverlayError_UnknownOverlay;
    overlay->mouseScale = *scale;
    return xr::VROverlayError_None;
}

bool HelperBackend::ResolveOverlayPose(const OverlayState& overlay, const PoseSnapshot& snapshot, Pose& pose) const {
    if (overlay.transformType == xr::VROverlayTransform_Absolute) {
        pose = MatrixToPose(overlay.transform);
        return true;
    }
    if (overlay.transformType == xr::VROverlayTransform_TrackedDeviceRelative) {
        const Pose relative = MatrixToPose(overlay.transform);
        if (overlay.transformDevice == xr::k_unTrackedDeviceIndex_Hmd) {
            if (!snapshot.valid) return false;
            pose = MultiplyPose(MatrixToPose(snapshot.hmd), relative);
            return true;
        }
        if (overlay.transformDevice == 1 || overlay.transformDevice == 2) {
            const int hand = overlay.transformDevice == 1 ? 0 : 1;
            if (!snapshot.handsValid[hand]) return false;
            pose = MultiplyPose(snapshot.handGrip[hand], relative);
            return true;
        }
    }
    return false;
}

bool HelperBackend::IntersectOverlay(const OverlayState& overlay, const PoseSnapshot& snapshot, const Pose& rayPose, float& u, float& v) const {
    Pose overlayPose;
    if (!ResolveOverlayPose(overlay, snapshot, overlayPose))
        return false;

    const Pose local = MultiplyPose(InversePose(overlayPose), rayPose);
    const float qx = local.orientation[0], qy = local.orientation[1], qz = local.orientation[2], qw = local.orientation[3];
    const float dx = -2.0f * (qx * qz + qw * qy);
    const float dy = -2.0f * (qy * qz - qw * qx);
    const float dz = -(1.0f - 2.0f * (qx * qx + qy * qy));
    if (std::fabs(dz) < 1e-6f)
        return false;

    const float t = -local.position[2] / dz;
    if (t <= 0.0f)
        return false;

    const float x = local.position[0] + dx * t;
    const float y = local.position[1] + dy * t;

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
    if (std::fabs(x) > halfW || std::fabs(y) > halfH)
        return false;

    u = std::clamp((x + halfW) / width, 0.0f, 1.0f);
    v = std::clamp((halfH - y) / height, 0.0f, 1.0f);
    return true;
}

void HelperBackend::GenerateOverlayEvents(OverlayState& overlay, const PoseSnapshot& snapshot) {
    int bestHand = -1;
    float bestU = 0.0f, bestV = 0.0f;
    float bestDistance = 1e30f;
    for (int hand = 0; hand < 2; ++hand) {
        if (!snapshot.handsValid[hand])
            continue;
        float u = 0.0f, v = 0.0f;
        if (!IntersectOverlay(overlay, snapshot, snapshot.handAim[hand], u, v))
            continue;
        const Pose local = MultiplyPose(InversePose(snapshot.handAim[hand]), snapshot.handGrip[hand]);
        const float distance = std::sqrt(local.position[0] * local.position[0] +
            local.position[1] * local.position[1] + local.position[2] * local.position[2]);
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

    const float trigger = hovered ? snapshot.trigger[bestHand] : 0.0f;
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

    const bool scrollUp = hovered && snapshot.stickY[1] > 0.7f;
    const bool scrollDown = hovered && snapshot.stickY[1] < -0.7f;
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

    const bool facePressed = hovered && snapshot.primary[1];
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

xr::EVROverlayError HelperBackend::ComputeOverlayIntersection(xr::VROverlayHandle_t handle, const xr::VROverlayIntersectionParams_t* params, xr::VROverlayIntersectionResults_t* results) {
    if (!params || !results)
        return xr::VROverlayError_InvalidParameter;
    PoseSnapshot snapshot;
    ReadPoseSnapshot(snapshot);
    std::lock_guard<std::mutex> lock(m_overlayMutex);
    OverlayState* overlay = FindOverlayByHandle(handle);
    if (!overlay)
        return xr::VROverlayError_UnknownOverlay;

    Pose ray;
    ray.position[0] = params->vSource.v[0];
    ray.position[1] = params->vSource.v[1];
    ray.position[2] = params->vSource.v[2];
    const float dx = params->vDirection.v[0], dy = params->vDirection.v[1], dz = params->vDirection.v[2];
    const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (len < 1e-6f)
        return xr::VROverlayError_RequestFailed;
    // Build a quaternion whose -Z points along the ray.
    const float fx = dx / len, fy = dy / len, fz = dz / len;
    const float upx = 0.0f, upy = 1.0f, upz = 0.0f;
    float rx = fy * upz - fz * upy;
    float ry = fz * upx - fx * upz;
    float rz = fx * upy - fy * upx;
    float rlen = std::sqrt(rx * rx + ry * ry + rz * rz);
    if (rlen < 1e-6f) { rx = 1.0f; ry = 0.0f; rz = 0.0f; rlen = 1.0f; }
    rx /= rlen; ry /= rlen; rz /= rlen;
    const float ux = ry * fz - rz * fy;
    const float uy = rz * fx - rx * fz;
    const float uz = rx * fy - ry * fx;
    // Columns: right, up, -forward.
    xr::HmdMatrix34_t mat = {};
    mat.m[0][0] = rx; mat.m[1][0] = ry; mat.m[2][0] = rz;
    mat.m[0][1] = ux; mat.m[1][1] = uy; mat.m[2][1] = uz;
    mat.m[0][2] = -fx; mat.m[1][2] = -fy; mat.m[2][2] = -fz;
    mat.m[0][3] = ray.position[0]; mat.m[1][3] = ray.position[1]; mat.m[2][3] = ray.position[2];
    ray = MatrixToPose(mat);

    float u = 0.0f, v = 0.0f;
    if (!IntersectOverlay(*overlay, snapshot, ray, u, v))
        return xr::VROverlayError_RequestFailed;
    results->vUVs.v[0] = u;
    results->vUVs.v[1] = v;
    results->fDistance = 1.0f;
    return xr::VROverlayError_None;
}

bool HelperBackend::PollNextOverlayEvent(xr::VROverlayHandle_t handle, xr::VREvent_t* event, uint32_t eventSize) {
    if (!event || eventSize < sizeof(*event))
        return false;
    PoseSnapshot snapshot;
    ReadPoseSnapshot(snapshot);
    std::lock_guard<std::mutex> lock(m_overlayMutex);
    OverlayState* overlay = FindOverlayByHandle(handle);
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

bool HelperBackend::GetComponentStateForDevicePath(const char* devicePath, const char* componentName, xr::VRInputValueHandle_t inputValue, xr::RenderModel_ControllerMode_State_t* state, xr::RenderModel_ComponentState_t* componentState) {
    (void)devicePath; (void)componentName;
    if (state) *state = {};
    if (!componentState) return false;
    std::memset(componentState, 0, sizeof(*componentState));
    const int hand = inputValue == xr::k_unInputSourceLeft ? 0 : (inputValue == xr::k_unInputSourceRight ? 1 : -1);
    if (hand < 0) return false;
    PoseSnapshot snapshot;
    if (!ReadPoseSnapshot(snapshot) || !snapshot.handsValid[hand])
        return false;
    componentState->mTrackingToComponentRenderModel = PoseToMatrix(Pose{});
    componentState->mTrackingToComponentLocal = PoseToMatrix(snapshot.handGrip[hand]);
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// xr:: facade
// ---------------------------------------------------------------------------

namespace xr {

System* SystemApi() { static System s; return &s; }
Compositor* CompositorApi() { static Compositor c; return &c; }
Input* InputApi() { static Input i; return &i; }
Overlay* OverlayApi() { static Overlay o; return &o; }
Applications* ApplicationsApi() { static Applications a; return &a; }
RenderModels* RenderModelsApi() { static RenderModels r; return &r; }

bool Initialize(const char* applicationName) { (void)applicationName; return B().Initialize(); }
bool CreateSessionFromD3D9() { return B().EnsureSession(); }
void Shutdown() { B().Shutdown(); }
bool IsInitialized() { return B().IsInitialized(); }
int GetInitError() { return B().InitError(); }
const char* GetInitErrorDescription(int error) { (void)error; return B().InitErrorDescription(); }

HmdError VR_Init(HmdError* peError, EVRApplicationType eApplicationType) {
    (void)eApplicationType;
    const bool ok = B().Initialize();
    if (peError) *peError = ok ? VRInitError_None : static_cast<HmdError>(B().InitError() > 0 ? B().InitError() : 1);
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
EVROverlayError Overlay::ShowKeyboardForOverlay(VROverlayHandle_t, EGamepadTextInputMode, EGamepadTextInputLineMode, uint32_t, const char*, uint32_t, const char*, uint64_t) {
    B().Log("[OpenXRHelper] Overlay keyboard is not available. Edit VR/config.txt on the desktop instead.");
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
