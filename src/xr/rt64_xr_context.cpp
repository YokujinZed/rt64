//
// RT64 - OpenXR context implementation.
//
// Failure philosophy: XR problems must never take down the host application.
// Every failure logs, cleans up and leaves the renderer running flat.
//

#include "rt64_xr_context.h"

#ifdef RT64_XR_SUPPORT

#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace RT64 {
    static bool xrCheck(XrInstance instance, XrResult result, const char *what) {
        if (XR_SUCCEEDED(result)) {
            return true;
        }

        char resultName[XR_MAX_RESULT_STRING_SIZE] = "XR_ERROR_UNKNOWN";
        if (instance != XR_NULL_HANDLE) {
            xrResultToString(instance, result, resultName);
        }

        fprintf(stderr, "XR: %s failed (%s).\n", what, resultName);
        return false;
    }

    // XRContext

    XRContext::~XRContext() {
        shutdown();
    }

    bool XRContext::initInstance() {
        const char *requestedExtensions[] = {
#   ifdef _WIN32
            XR_KHR_D3D12_ENABLE_EXTENSION_NAME,
#   endif
        };

        // Verify the runtime offers the graphics binding extension before creating the instance.
        uint32_t extensionCount = 0;
        XrResult result = xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionCount, nullptr);
        if (!xrCheck(XR_NULL_HANDLE, result, "xrEnumerateInstanceExtensionProperties")) {
            // A missing loader/runtime lands here. Not an error worth more than one line.
            return false;
        }

        std::vector<XrExtensionProperties> extensions(extensionCount, { XR_TYPE_EXTENSION_PROPERTIES });
        xrEnumerateInstanceExtensionProperties(nullptr, extensionCount, &extensionCount, extensions.data());
        for (const char *requested : requestedExtensions) {
            bool found = false;
            for (const XrExtensionProperties &props : extensions) {
                if (strcmp(props.extensionName, requested) == 0) {
                    found = true;
                    break;
                }
            }

            if (!found) {
                fprintf(stderr, "XR: runtime does not support %s.\n", requested);
                return false;
            }
        }

        XrInstanceCreateInfo createInfo = { XR_TYPE_INSTANCE_CREATE_INFO };
        createInfo.enabledExtensionCount = uint32_t(std::size(requestedExtensions));
        createInfo.enabledExtensionNames = requestedExtensions;
        snprintf(createInfo.applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE, "RT64");
        snprintf(createInfo.applicationInfo.engineName, XR_MAX_ENGINE_NAME_SIZE, "RT64");
        createInfo.applicationInfo.applicationVersion = 1;
        createInfo.applicationInfo.engineVersion = 1;
        createInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0;
        result = xrCreateInstance(&createInfo, &instance);
        if (!xrCheck(XR_NULL_HANDLE, result, "xrCreateInstance")) {
            return false;
        }

        XrSystemGetInfo systemInfo = { XR_TYPE_SYSTEM_GET_INFO };
        systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        result = xrGetSystem(instance, &systemInfo, &systemId);
        if (!xrCheck(instance, result, "xrGetSystem")) {
            fprintf(stderr, "XR: no HMD available (is the headset connected and the runtime active?).\n");
            destroyHandles();
            return false;
        }

        XrSystemProperties systemProps = { XR_TYPE_SYSTEM_PROPERTIES };
        if (XR_SUCCEEDED(xrGetSystemProperties(instance, systemId, &systemProps))) {
            systemName = systemProps.systemName;
            fprintf(stderr, "XR: system \"%s\".\n", systemProps.systemName);
        }

        uint32_t blendModeCount = 0;
        xrEnumerateEnvironmentBlendModes(instance, systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 1, &blendModeCount, &blendMode);
        return true;
    }

#   ifdef _WIN32
    bool XRContext::beginSessionD3D12(ID3D12Device *device, ID3D12CommandQueue *queue) {
        if ((instance == XR_NULL_HANDLE) || (device == nullptr) || (queue == nullptr)) {
            return false;
        }

        // Mandatory before session creation with the D3D12 binding.
        PFN_xrGetD3D12GraphicsRequirementsKHR getRequirements = nullptr;
        XrResult result = xrGetInstanceProcAddr(instance, "xrGetD3D12GraphicsRequirementsKHR", reinterpret_cast<PFN_xrVoidFunction *>(&getRequirements));
        if (!xrCheck(instance, result, "xrGetInstanceProcAddr(xrGetD3D12GraphicsRequirementsKHR)")) {
            return false;
        }

        XrGraphicsRequirementsD3D12KHR requirements = { XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR };
        result = getRequirements(instance, systemId, &requirements);
        if (!xrCheck(instance, result, "xrGetD3D12GraphicsRequirementsKHR")) {
            return false;
        }

        // The runtime names the adapter it wants. RT64 picked its own; warn on
        // mismatch (multi-GPU machines) but attempt the session anyway.
        const LUID deviceLuid = device->GetAdapterLuid();
        if (memcmp(&deviceLuid, &requirements.adapterLuid, sizeof(LUID)) != 0) {
            fprintf(stderr, "XR: renderer adapter LUID does not match the XR runtime's adapter. "
                "If session creation fails, force the same GPU for both.\n");
        }

        XrGraphicsBindingD3D12KHR binding = { XR_TYPE_GRAPHICS_BINDING_D3D12_KHR };
        binding.device = device;
        binding.queue = queue;

        XrSessionCreateInfo sessionInfo = { XR_TYPE_SESSION_CREATE_INFO };
        sessionInfo.next = &binding;
        sessionInfo.systemId = systemId;
        result = xrCreateSession(instance, &sessionInfo, &session);
        if (!xrCheck(instance, result, "xrCreateSession")) {
            return false;
        }

        if (!createActions() || !createSpaces()) {
            destroyHandles();
            return false;
        }

        quitRequested = false;
        frameThread = std::thread(&XRContext::frameLoop, this);
        return true;
    }
#   endif

    bool XRContext::createSpaces() {
        XrReferenceSpaceCreateInfo spaceInfo = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
        spaceInfo.poseInReferenceSpace = { { 0.0f, 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, 0.0f } };
        spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        XrResult result = xrCreateReferenceSpace(session, &spaceInfo, &baseSpace);
        if (!xrCheck(instance, result, "xrCreateReferenceSpace(LOCAL)")) {
            return false;
        }

        result = xrCreateReferenceSpace(session, &spaceInfo, &quadSpace);
        if (!xrCheck(instance, result, "xrCreateReferenceSpace(LOCAL quad)")) {
            return false;
        }

        spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        result = xrCreateReferenceSpace(session, &spaceInfo, &viewSpace);
        return xrCheck(instance, result, "xrCreateReferenceSpace(VIEW)");
    }

    void XRContext::requestRecenter() {
        recenterRequested = true;
    }

    void XRContext::setStereoEnabled(bool enabled) {
        stereoEnabled = enabled;
    }

    bool XRContext::isStereoEnabled() const {
        return stereoEnabled && sessionRunning;
    }

    void XRContext::setUnitsPerMeter(float units) {
        unitsPerMeter = units;
    }

    XREyeParams XRContext::buildEyeParams(uint32_t eyeIndex) const {
        XREyeParams params;
        if ((eyeIndex > 1) || !isStereoEnabled()) {
            return params;
        }

        XrView view = { XR_TYPE_VIEW };
        {
            const std::lock_guard<std::mutex> lock(eyeViewsMutex);
            if (!eyeViewsValid) {
                return params;
            }
            view = eyeViews[eyeIndex];
        }

        // Head-to-eye inverse as a row-vector matrix (v' = v * M, translation
        // in row 3). With column-convention rotation R from the quaternion and
        // head-relative eye position t: rows 0-2 = rows of R, row 3 = -t * R.
        const XrQuaternionf &q = view.pose.orientation;
        const float scale = unitsPerMeter.load();
        const float tx = view.pose.position.x * scale;
        const float ty = view.pose.position.y * scale;
        const float tz = view.pose.position.z * scale;

        float r[3][3];
        r[0][0] = 1.0f - 2.0f * (q.y * q.y + q.z * q.z);
        r[0][1] = 2.0f * (q.x * q.y - q.w * q.z);
        r[0][2] = 2.0f * (q.x * q.z + q.w * q.y);
        r[1][0] = 2.0f * (q.x * q.y + q.w * q.z);
        r[1][1] = 1.0f - 2.0f * (q.x * q.x + q.z * q.z);
        r[1][2] = 2.0f * (q.y * q.z - q.w * q.x);
        r[2][0] = 2.0f * (q.x * q.z - q.w * q.y);
        r[2][1] = 2.0f * (q.y * q.z + q.w * q.x);
        r[2][2] = 1.0f - 2.0f * (q.x * q.x + q.y * q.y);

        for (int i = 0; i < 3; i++) {
            for (int j = 0; j < 3; j++) {
                params.viewOffset[i][j] = r[i][j];
            }
            params.viewOffset[i][3] = 0.0f;
        }
        for (int j = 0; j < 3; j++) {
            params.viewOffset[3][j] = -(tx * r[0][j] + ty * r[1][j] + tz * r[2][j]);
        }
        params.viewOffset[3][3] = 1.0f;

        params.tanLeft = std::tan(view.fov.angleLeft);
        params.tanRight = std::tan(view.fov.angleRight);
        params.tanDown = std::tan(view.fov.angleDown);
        params.tanUp = std::tan(view.fov.angleUp);
        params.valid = true;
        return params;
    }

    void XRContext::recenterQuadSpace(XrTime time) {
        XrSpaceLocation location = { XR_TYPE_SPACE_LOCATION };
        if (XR_FAILED(xrLocateSpace(viewSpace, baseSpace, time, &location))) {
            return;
        }

        constexpr XrSpaceLocationFlags required = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        if ((location.locationFlags & required) != required) {
            return;
        }

        // Yaw-flatten the view orientation so the screen stays upright.
        const XrQuaternionf &q = location.pose.orientation;
        const float yaw = std::atan2(2.0f * (q.x * q.z + q.w * q.y), 1.0f - 2.0f * (q.x * q.x + q.y * q.y));
        XrPosef pose;
        pose.orientation = { 0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f) };
        pose.position = location.pose.position;

        XrReferenceSpaceCreateInfo spaceInfo = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
        spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        spaceInfo.poseInReferenceSpace = pose;
        XrSpace newSpace = XR_NULL_HANDLE;
        if (XR_FAILED(xrCreateReferenceSpace(session, &spaceInfo, &newSpace))) {
            return;
        }

        const std::lock_guard<std::mutex> lock(quadMutex);
        if (quadSpace != XR_NULL_HANDLE) {
            xrDestroySpace(quadSpace);
        }
        quadSpace = newSpace;
    }

    bool XRContext::createActions() {
        xrStringToPath(instance, "/user/hand/left", &handPaths[0]);
        xrStringToPath(instance, "/user/hand/right", &handPaths[1]);

        XrActionSetCreateInfo setInfo = { XR_TYPE_ACTION_SET_CREATE_INFO };
        snprintf(setInfo.actionSetName, XR_MAX_ACTION_SET_NAME_SIZE, "gameplay");
        snprintf(setInfo.localizedActionSetName, XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE, "Gameplay");
        XrResult result = xrCreateActionSet(instance, &setInfo, &actionSet);
        if (!xrCheck(instance, result, "xrCreateActionSet")) {
            return false;
        }

        struct ActionDesc {
            XrAction *action;
            const char *name;
            const char *localized;
            XrActionType type;
        };

        const ActionDesc descs[] = {
            { &stickAction, "stick", "Thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT },
            { &stickClickAction, "stick_click", "Thumbstick Click", XR_ACTION_TYPE_BOOLEAN_INPUT },
            { &triggerAction, "trigger", "Trigger", XR_ACTION_TYPE_FLOAT_INPUT },
            { &squeezeAction, "squeeze", "Grip Squeeze", XR_ACTION_TYPE_FLOAT_INPUT },
            { &primaryAction, "primary", "Primary Button", XR_ACTION_TYPE_BOOLEAN_INPUT },
            { &secondaryAction, "secondary", "Secondary Button", XR_ACTION_TYPE_BOOLEAN_INPUT },
            { &menuAction, "menu", "Menu Button", XR_ACTION_TYPE_BOOLEAN_INPUT },
        };

        for (const ActionDesc &desc : descs) {
            XrActionCreateInfo actionInfo = { XR_TYPE_ACTION_CREATE_INFO };
            actionInfo.actionType = desc.type;
            snprintf(actionInfo.actionName, XR_MAX_ACTION_NAME_SIZE, "%s", desc.name);
            snprintf(actionInfo.localizedActionName, XR_MAX_LOCALIZED_ACTION_NAME_SIZE, "%s", desc.localized);
            actionInfo.countSubactionPaths = 2;
            actionInfo.subactionPaths = handPaths;
            result = xrCreateAction(actionSet, &actionInfo, desc.action);
            if (!xrCheck(instance, result, "xrCreateAction")) {
                return false;
            }
        }

        auto path = [&](const char *str) {
            XrPath p = XR_NULL_PATH;
            xrStringToPath(instance, str, &p);
            return p;
        };

        // Quest Touch controllers are the primary target.
        const XrActionSuggestedBinding touchBindings[] = {
            { stickAction, path("/user/hand/left/input/thumbstick") },
            { stickAction, path("/user/hand/right/input/thumbstick") },
            { stickClickAction, path("/user/hand/left/input/thumbstick/click") },
            { stickClickAction, path("/user/hand/right/input/thumbstick/click") },
            { triggerAction, path("/user/hand/left/input/trigger/value") },
            { triggerAction, path("/user/hand/right/input/trigger/value") },
            { squeezeAction, path("/user/hand/left/input/squeeze/value") },
            { squeezeAction, path("/user/hand/right/input/squeeze/value") },
            { primaryAction, path("/user/hand/left/input/x/click") },
            { primaryAction, path("/user/hand/right/input/a/click") },
            { secondaryAction, path("/user/hand/left/input/y/click") },
            { secondaryAction, path("/user/hand/right/input/b/click") },
            { menuAction, path("/user/hand/left/input/menu/click") },
        };

        XrInteractionProfileSuggestedBinding suggested = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
        suggested.interactionProfile = path("/interaction_profiles/oculus/touch_controller");
        suggested.suggestedBindings = touchBindings;
        suggested.countSuggestedBindings = uint32_t(std::size(touchBindings));
        result = xrSuggestInteractionProfileBindings(instance, &suggested);
        if (!xrCheck(instance, result, "xrSuggestInteractionProfileBindings(touch)")) {
            return false;
        }

        // Minimal fallback so unknown controllers still get trigger + menu.
        const XrActionSuggestedBinding simpleBindings[] = {
            { triggerAction, path("/user/hand/left/input/select/click") },
            { triggerAction, path("/user/hand/right/input/select/click") },
            { menuAction, path("/user/hand/left/input/menu/click") },
            { menuAction, path("/user/hand/right/input/menu/click") },
        };
        suggested.interactionProfile = path("/interaction_profiles/khr/simple_controller");
        suggested.suggestedBindings = simpleBindings;
        suggested.countSuggestedBindings = uint32_t(std::size(simpleBindings));
        xrSuggestInteractionProfileBindings(instance, &suggested);

        XrSessionActionSetsAttachInfo attachInfo = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
        attachInfo.countActionSets = 1;
        attachInfo.actionSets = &actionSet;
        result = xrAttachSessionActionSets(session, &attachInfo);
        return xrCheck(instance, result, "xrAttachSessionActionSets");
    }

#   ifdef _WIN32
    bool XRContext::ensureQuadSwapchain(uint32_t width, uint32_t height) {
        if ((quadSwapchain != XR_NULL_HANDLE) && (width == quadWidth) && (height == quadHeight)) {
            return true;
        }

        if ((width == 0) || (height == 0)) {
            return false;
        }

        destroyQuadSwapchain();

        if (quadFormat == 0) {
            uint32_t formatCount = 0;
            xrEnumerateSwapchainFormats(session, 0, &formatCount, nullptr);
            std::vector<int64_t> formats(formatCount);
            xrEnumerateSwapchainFormats(session, formatCount, &formatCount, formats.data());

            // The window swap chain is B8G8R8A8_UNORM; only same-family copies
            // are legal. Prefer the sRGB view so the compositor interprets the
            // game's sRGB-encoded output correctly.
            const int64_t preferred[] = { DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM };
            for (int64_t want : preferred) {
                for (int64_t have : formats) {
                    if ((have == want) && (quadFormat == 0)) {
                        quadFormat = want;
                    }
                }
            }

            if (quadFormat == 0) {
                fprintf(stderr, "XR: runtime does not offer a B8G8R8A8 swapchain format; cinema layer disabled. Formats offered:");
                for (int64_t have : formats) {
                    fprintf(stderr, " %" PRId64, have);
                }
                fprintf(stderr, "\n");
                quadFormat = -1;
            }
        }

        if (quadFormat < 0) {
            return false;
        }

        XrSwapchainCreateInfo createInfo = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
        createInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        createInfo.format = quadFormat;
        createInfo.sampleCount = 1;
        createInfo.width = width;
        createInfo.height = height;
        createInfo.faceCount = 1;
        createInfo.arraySize = 1;
        createInfo.mipCount = 1;

        XrSwapchain newSwapchain = XR_NULL_HANDLE;
        if (!xrCheck(instance, xrCreateSwapchain(session, &createInfo, &newSwapchain), "xrCreateSwapchain")) {
            return false;
        }

        uint32_t imageCount = 0;
        xrEnumerateSwapchainImages(newSwapchain, 0, &imageCount, nullptr);
        std::vector<XrSwapchainImageD3D12KHR> images(imageCount, { XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR });
        XrResult result = xrEnumerateSwapchainImages(newSwapchain, imageCount, &imageCount, reinterpret_cast<XrSwapchainImageBaseHeader *>(images.data()));
        if (!xrCheck(instance, result, "xrEnumerateSwapchainImages")) {
            xrDestroySwapchain(newSwapchain);
            return false;
        }

        const std::lock_guard<std::mutex> lock(quadMutex);
        quadSwapchain = newSwapchain;
        quadImages = std::move(images);
        quadWidth = width;
        quadHeight = height;
        fprintf(stderr, "XR: cinema swapchain %ux%u (%u images, format %" PRId64 ").\n", width, height, imageCount, quadFormat);
        return true;
    }

    void XRContext::destroyQuadSwapchain() {
        const std::lock_guard<std::mutex> lock(quadMutex);
        quadReady = false;
        quadImageAcquired = false;
        quadPendingRelease = false;
        quadImages.clear();
        if (quadSwapchain != XR_NULL_HANDLE) {
            xrDestroySwapchain(quadSwapchain);
            quadSwapchain = XR_NULL_HANDLE;
        }
        quadWidth = 0;
        quadHeight = 0;
    }

    bool XRContext::ensureStereoSwapchain(uint32_t width, uint32_t height, int64_t format) {
        if ((stereoSwapchain != XR_NULL_HANDLE) && (width == stereoWidth) && (height == stereoHeight) && (format == stereoFormat)) {
            return true;
        }

        if ((width == 0) || (height == 0)) {
            return false;
        }

        destroyStereoSwapchain();

        // The eye images are raw-copied, so the swapchain format must match the
        // source format family exactly. Verify the runtime offers it.
        uint32_t formatCount = 0;
        xrEnumerateSwapchainFormats(session, 0, &formatCount, nullptr);
        std::vector<int64_t> formats(formatCount);
        xrEnumerateSwapchainFormats(session, formatCount, &formatCount, formats.data());
        bool formatAvailable = false;
        for (int64_t have : formats) {
            formatAvailable = formatAvailable || (have == format);
        }

        if (!formatAvailable) {
            fprintf(stderr, "XR: runtime does not offer swapchain format %" PRId64 " for stereo; stereo layer disabled.\n", format);
            return false;
        }

        XrSwapchainCreateInfo createInfo = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
        createInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        createInfo.format = format;
        createInfo.sampleCount = 1;
        createInfo.width = width;
        createInfo.height = height;
        createInfo.faceCount = 1;
        createInfo.arraySize = 2;
        createInfo.mipCount = 1;

        XrSwapchain newSwapchain = XR_NULL_HANDLE;
        if (!xrCheck(instance, xrCreateSwapchain(session, &createInfo, &newSwapchain), "xrCreateSwapchain(stereo)")) {
            return false;
        }

        uint32_t imageCount = 0;
        xrEnumerateSwapchainImages(newSwapchain, 0, &imageCount, nullptr);
        std::vector<XrSwapchainImageD3D12KHR> images(imageCount, { XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR });
        XrResult result = xrEnumerateSwapchainImages(newSwapchain, imageCount, &imageCount, reinterpret_cast<XrSwapchainImageBaseHeader *>(images.data()));
        if (!xrCheck(instance, result, "xrEnumerateSwapchainImages(stereo)")) {
            xrDestroySwapchain(newSwapchain);
            return false;
        }

        const std::lock_guard<std::mutex> lock(quadMutex);
        stereoSwapchain = newSwapchain;
        stereoImages = std::move(images);
        stereoWidth = width;
        stereoHeight = height;
        stereoFormat = format;
        fprintf(stderr, "XR: stereo swapchain %ux%ux2 (%u images, format %" PRId64 ").\n", width, height, imageCount, format);
        return true;
    }

    void XRContext::destroyStereoSwapchain() {
        const std::lock_guard<std::mutex> lock(quadMutex);
        stereoReady = false;
        stereoImageAcquired = false;
        stereoPendingRelease = false;
        stereoImages.clear();
        if (stereoSwapchain != XR_NULL_HANDLE) {
            xrDestroySwapchain(stereoSwapchain);
            stereoSwapchain = XR_NULL_HANDLE;
        }
        stereoWidth = 0;
        stereoHeight = 0;
        stereoFormat = 0;
    }

    bool XRContext::pt_recordStereoCopy(ID3D12GraphicsCommandList *commandList, ID3D12Resource *leftTexture, ID3D12Resource *rightTexture, uint32_t width, uint32_t height) {
        if (!sessionRunning || (commandList == nullptr) || (leftTexture == nullptr) || (rightTexture == nullptr)) {
            return false;
        }

        const DXGI_FORMAT sourceFormat = leftTexture->GetDesc().Format;
        if (!ensureStereoSwapchain(width, height, int64_t(sourceFormat))) {
            return false;
        }

        if (!stereoImageAcquired) {
            XrSwapchainImageAcquireInfo acquireInfo = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
            if (XR_FAILED(xrAcquireSwapchainImage(stereoSwapchain, &acquireInfo, &stereoImageIndex))) {
                return false;
            }
            stereoImageAcquired = true;
        }

        XrSwapchainImageWaitInfo waitInfo = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
        waitInfo.timeout = 100'000'000; // 100ms
        if (xrWaitSwapchainImage(stereoSwapchain, &waitInfo) != XR_SUCCESS) {
            return false;
        }

        ID3D12Resource *destTexture = stereoImages[stereoImageIndex].texture;

        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = destTexture;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        commandList->ResourceBarrier(1, &barrier);

        ID3D12Resource *sources[2] = { leftTexture, rightTexture };
        for (uint32_t eye = 0; eye < 2; eye++) {
            D3D12_TEXTURE_COPY_LOCATION destLocation = {};
            destLocation.pResource = destTexture;
            destLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            destLocation.SubresourceIndex = eye; // mipCount 1: subresource == array slice
            D3D12_TEXTURE_COPY_LOCATION srcLocation = {};
            srcLocation.pResource = sources[eye];
            srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            srcLocation.SubresourceIndex = 0;
            commandList->CopyTextureRegion(&destLocation, 0, 0, 0, &srcLocation, nullptr);
        }

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        commandList->ResourceBarrier(1, &barrier);

        stereoPendingRelease = true;
        return true;
    }

    void XRContext::pt_releaseStereoFrame() {
        if (!stereoPendingRelease) {
            return;
        }

        XrSwapchainImageReleaseInfo releaseInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
        if (XR_SUCCEEDED(xrReleaseSwapchainImage(stereoSwapchain, &releaseInfo))) {
            stereoImageAcquired = false;
            stereoPendingRelease = false;
            stereoReady = true;
        }
    }

    bool XRContext::pt_recordFrameCopy(ID3D12GraphicsCommandList *commandList, ID3D12Resource *sourceTexture, uint32_t width, uint32_t height) {
        if (!sessionRunning || (commandList == nullptr) || (sourceTexture == nullptr)) {
            return false;
        }

        if (!ensureQuadSwapchain(width, height)) {
            return false;
        }

        if (!quadImageAcquired) {
            XrSwapchainImageAcquireInfo acquireInfo = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
            if (XR_FAILED(xrAcquireSwapchainImage(quadSwapchain, &acquireInfo, &quadImageIndex))) {
                return false;
            }
            quadImageAcquired = true;
        }

        // On timeout the image stays acquired and is re-waited next frame; the
        // compositor keeps showing the last released image meanwhile.
        XrSwapchainImageWaitInfo waitInfo = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
        waitInfo.timeout = 100'000'000; // 100ms
        const XrResult waitResult = xrWaitSwapchainImage(quadSwapchain, &waitInfo);
        if (waitResult != XR_SUCCESS) {
            return false;
        }

        ID3D12Resource *destTexture = quadImages[quadImageIndex].texture;

        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = destTexture;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        commandList->ResourceBarrier(1, &barrier);

        D3D12_TEXTURE_COPY_LOCATION destLocation = {};
        destLocation.pResource = destTexture;
        destLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destLocation.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION srcLocation = {};
        srcLocation.pResource = sourceTexture;
        srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        srcLocation.SubresourceIndex = 0;
        commandList->CopyTextureRegion(&destLocation, 0, 0, 0, &srcLocation, nullptr);

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        commandList->ResourceBarrier(1, &barrier);

        quadPendingRelease = true;
        return true;
    }

    void XRContext::pt_releaseFrame() {
        if (!quadPendingRelease) {
            return;
        }

        XrSwapchainImageReleaseInfo releaseInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
        if (XR_SUCCEEDED(xrReleaseSwapchainImage(quadSwapchain, &releaseInfo))) {
            quadImageAcquired = false;
            quadPendingRelease = false;
            quadReady = true;
        }
    }

    bool XRContext::pt_waitForDisplayTick() {
        std::unique_lock<std::mutex> lock(tickMutex);
        const bool ticked = tickCondition.wait_for(lock, std::chrono::milliseconds(50), [&]() {
            return (tickCounter != lastConsumedTick) || quitRequested || !sessionRunning;
        }) && (tickCounter != lastConsumedTick);
        lastConsumedTick = tickCounter;
        return ticked;
    }

    uint32_t XRContext::displayRateHz() const {
        const int64_t period = displayPeriodNs.load();
        if (period <= 0) {
            return 0;
        }
        return uint32_t((1'000'000'000LL + period / 2) / period);
    }
#   endif

    void XRContext::pollEvents() {
        XrEventDataBuffer event = { XR_TYPE_EVENT_DATA_BUFFER };
        while (xrPollEvent(instance, &event) == XR_SUCCESS) {
            switch (event.type) {
            case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
                const auto &stateEvent = *reinterpret_cast<XrEventDataSessionStateChanged *>(&event);
                switch (stateEvent.state) {
                case XR_SESSION_STATE_READY: {
                    XrSessionBeginInfo beginInfo = { XR_TYPE_SESSION_BEGIN_INFO };
                    beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    if (xrCheck(instance, xrBeginSession(session, &beginInfo), "xrBeginSession")) {
                        sessionRunning = true;
                        fprintf(stderr, "XR: session running.\n");
                    }
                    break;
                }
                case XR_SESSION_STATE_STOPPING:
                    sessionRunning = false;
                    xrCheck(instance, xrEndSession(session), "xrEndSession");
                    break;
                case XR_SESSION_STATE_EXITING:
                case XR_SESSION_STATE_LOSS_PENDING:
                    sessionRunning = false;
                    quitRequested = true;
                    break;
                default:
                    break;
                }
                break;
            }
            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                sessionRunning = false;
                quitRequested = true;
                break;
            default:
                break;
            }

            event = { XR_TYPE_EVENT_DATA_BUFFER };
        }
    }

    void XRContext::syncActions(XrTime predictedDisplayTime) {
        XrActiveActionSet activeSet = { actionSet, XR_NULL_PATH };
        XrActionsSyncInfo syncInfo = { XR_TYPE_ACTIONS_SYNC_INFO };
        syncInfo.countActiveActionSets = 1;
        syncInfo.activeActionSets = &activeSet;
        if (XR_FAILED(xrSyncActions(session, &syncInfo))) {
            return;
        }

        XRInputSnapshot next;
        for (uint32_t hand = 0; hand < 2; hand++) {
            XRControllerState &state = (hand == 0) ? next.left : next.right;

            XrActionStateGetInfo getInfo = { XR_TYPE_ACTION_STATE_GET_INFO };
            getInfo.subactionPath = handPaths[hand];

            XrActionStateVector2f stickState = { XR_TYPE_ACTION_STATE_VECTOR2F };
            getInfo.action = stickAction;
            if (XR_SUCCEEDED(xrGetActionStateVector2f(session, &getInfo, &stickState)) && stickState.isActive) {
                state.active = true;
                state.stickX = stickState.currentState.x;
                state.stickY = stickState.currentState.y;
            }

            XrActionStateFloat floatState = { XR_TYPE_ACTION_STATE_FLOAT };
            getInfo.action = triggerAction;
            if (XR_SUCCEEDED(xrGetActionStateFloat(session, &getInfo, &floatState)) && floatState.isActive) {
                state.active = true;
                state.trigger = floatState.currentState;
            }

            floatState = { XR_TYPE_ACTION_STATE_FLOAT };
            getInfo.action = squeezeAction;
            if (XR_SUCCEEDED(xrGetActionStateFloat(session, &getInfo, &floatState)) && floatState.isActive) {
                state.squeeze = floatState.currentState;
            }

            XrActionStateBoolean boolState = { XR_TYPE_ACTION_STATE_BOOLEAN };
            getInfo.action = stickClickAction;
            if (XR_SUCCEEDED(xrGetActionStateBoolean(session, &getInfo, &boolState)) && boolState.isActive) {
                state.stickClick = boolState.currentState;
            }

            boolState = { XR_TYPE_ACTION_STATE_BOOLEAN };
            getInfo.action = primaryAction;
            if (XR_SUCCEEDED(xrGetActionStateBoolean(session, &getInfo, &boolState)) && boolState.isActive) {
                state.primaryButton = boolState.currentState;
            }

            boolState = { XR_TYPE_ACTION_STATE_BOOLEAN };
            getInfo.action = secondaryAction;
            if (XR_SUCCEEDED(xrGetActionStateBoolean(session, &getInfo, &boolState)) && boolState.isActive) {
                state.secondaryButton = boolState.currentState;
            }

            boolState = { XR_TYPE_ACTION_STATE_BOOLEAN };
            getInfo.action = menuAction;
            if (XR_SUCCEEDED(xrGetActionStateBoolean(session, &getInfo, &boolState)) && boolState.isActive) {
                state.menuButton = boolState.currentState;
            }
        }

        {
            const std::lock_guard<std::mutex> lock(snapshotMutex);
            next.sampleCount = snapshot.sampleCount + 1;
            snapshot = next;
        }
    }

    void XRContext::frameLoop() {
        while (!quitRequested) {
            pollEvents();

            if (!sessionRunning) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }

            XrFrameWaitInfo waitInfo = { XR_TYPE_FRAME_WAIT_INFO };
            XrFrameState frameState = { XR_TYPE_FRAME_STATE };
            if (XR_FAILED(xrWaitFrame(session, &waitInfo, &frameState))) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }

            // Publish the display tick that paces the present thread.
            displayPeriodNs = frameState.predictedDisplayPeriod;
            {
                const std::lock_guard<std::mutex> lock(tickMutex);
                tickCounter++;
            }
            tickCondition.notify_all();

            // Refresh the head-relative eye views for stereo rendering.
            if (stereoEnabled) {
                XrViewLocateInfo locateInfo = { XR_TYPE_VIEW_LOCATE_INFO };
                locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                locateInfo.displayTime = frameState.predictedDisplayTime;
                locateInfo.space = viewSpace;
                XrViewState viewState = { XR_TYPE_VIEW_STATE };
                XrView views[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
                uint32_t viewCount = 0;
                if (XR_SUCCEEDED(xrLocateViews(session, &locateInfo, &viewState, 2, &viewCount, views)) && (viewCount == 2)) {
                    constexpr XrViewStateFlags required = XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
                    const std::lock_guard<std::mutex> lock(eyeViewsMutex);
                    eyeViewsValid = (viewState.viewStateFlags & required) == required;
                    eyeViews[0] = views[0];
                    eyeViews[1] = views[1];
                }
            }

            XrFrameBeginInfo beginInfo = { XR_TYPE_FRAME_BEGIN_INFO };
            if (XR_FAILED(xrBeginFrame(session, &beginInfo))) {
                continue;
            }

            syncActions(frameState.predictedDisplayTime);

            // Recenter the cinema screen on left-stick-click (edge) or host request.
            bool recenterClick = false;
            {
                const std::lock_guard<std::mutex> lock(snapshotMutex);
                recenterClick = snapshot.left.stickClick;
            }
            if ((recenterClick && !prevRecenterClick) || recenterRequested.exchange(false)) {
                recenterQuadSpace(frameState.predictedDisplayTime);
            }
            prevRecenterClick = recenterClick;

            // Submit the stereo projection layer when eye frames are flowing;
            // otherwise fall back to the cinema quad. In both cases the
            // compositor keeps showing the last released image, holding a
            // stable picture through menus, loads and static frames.
            XrCompositionLayerQuad quadLayer = { XR_TYPE_COMPOSITION_LAYER_QUAD };
            XrCompositionLayerProjection projectionLayer = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
            XrCompositionLayerProjectionView projectionViews[2] = { { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW }, { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW } };
            const XrCompositionLayerBaseHeader *layers[1] = {};
            uint32_t layerCount = 0;

            XrFrameEndInfo endInfo = { XR_TYPE_FRAME_END_INFO };
            endInfo.displayTime = frameState.predictedDisplayTime;
            endInfo.environmentBlendMode = blendMode;

            {
                const std::lock_guard<std::mutex> lock(quadMutex);
                if (stereoReady && frameState.shouldRender && (stereoSwapchain != XR_NULL_HANDLE)) {
                    // M3 stereo is head-relative: the layer lives in VIEW space
                    // and the per-eye poses are the head-relative eye offsets
                    // the frames were rendered with (static IPD geometry, so
                    // the current sample echoes the rendered pose). World
                    // stabilization arrives with head tracking in M4.
                    const std::lock_guard<std::mutex> eyeLock(eyeViewsMutex);
                    for (uint32_t eye = 0; eye < 2; eye++) {
                        projectionViews[eye].pose = eyeViews[eye].pose;
                        projectionViews[eye].fov = eyeViews[eye].fov;
                        projectionViews[eye].subImage.swapchain = stereoSwapchain;
                        projectionViews[eye].subImage.imageRect = { { 0, 0 }, { int32_t(stereoWidth), int32_t(stereoHeight) } };
                        projectionViews[eye].subImage.imageArrayIndex = eye;
                    }
                    projectionLayer.layerFlags = 0;
                    projectionLayer.space = viewSpace;
                    projectionLayer.viewCount = 2;
                    projectionLayer.views = projectionViews;
                    layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader *>(&projectionLayer);
                    layerCount = 1;
                }
                else if (quadReady && frameState.shouldRender && (quadSwapchain != XR_NULL_HANDLE)) {
                    quadLayer.layerFlags = 0;
                    quadLayer.space = quadSpace;
                    quadLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                    quadLayer.subImage.swapchain = quadSwapchain;
                    quadLayer.subImage.imageRect = { { 0, 0 }, { int32_t(quadWidth), int32_t(quadHeight) } };
                    quadLayer.subImage.imageArrayIndex = 0;
                    quadLayer.pose = { { 0.0f, 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, -2.5f } };
                    // ~4m wide at 2.5m; height follows the image aspect so the
                    // letterboxed frame is never stretched.
                    quadLayer.size = { 4.0f, 4.0f * float(quadHeight) / float(quadWidth) };
                    layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader *>(&quadLayer);
                    layerCount = 1;
                }

                endInfo.layerCount = layerCount;
                endInfo.layers = (layerCount > 0) ? layers : nullptr;
                xrEndFrame(session, &endInfo);
            }
        }

        // Clear controller state so hosts stop seeing stale input.
        const std::lock_guard<std::mutex> lock(snapshotMutex);
        snapshot.left = XRControllerState();
        snapshot.right = XRControllerState();
    }

    void XRContext::destroyHandles() {
#   ifdef _WIN32
        destroyQuadSwapchain();
        destroyStereoSwapchain();
#   endif

        for (XrSpace *space : { &baseSpace, &viewSpace, &quadSpace }) {
            if (*space != XR_NULL_HANDLE) {
                xrDestroySpace(*space);
                *space = XR_NULL_HANDLE;
            }
        }

        if (actionSet != XR_NULL_HANDLE) {
            xrDestroyActionSet(actionSet); // destroys child actions
            actionSet = XR_NULL_HANDLE;
        }

        if (session != XR_NULL_HANDLE) {
            xrDestroySession(session);
            session = XR_NULL_HANDLE;
        }

        if (instance != XR_NULL_HANDLE) {
            xrDestroyInstance(instance);
            instance = XR_NULL_HANDLE;
        }
    }

    void XRContext::shutdown() {
        if (shutdownDone) {
            return;
        }

        shutdownDone = true;
        quitRequested = true;

        // Unblock a frame thread parked inside xrWaitFrame: ask the runtime to
        // wind the session down (STOPPING -> xrEndSession in pollEvents ->
        // EXITING -> loop exit). Without this, runtimes that throttle
        // xrWaitFrame (e.g. when the stream ends) can hang the join and freeze
        // the host's entire teardown chain.
        if ((session != XR_NULL_HANDLE) && sessionRunning) {
            xrRequestExitSession(session);
        }

        tickCondition.notify_all();
        if (frameThread.joinable()) {
            frameThread.join();
        }

        destroyHandles();
    }

    XRInputSnapshot XRContext::sampleInput() const {
        const std::lock_guard<std::mutex> lock(snapshotMutex);
        return snapshot;
    }

    bool XRContext::isSessionRunning() const {
        return sessionRunning;
    }
};

#endif // RT64_XR_SUPPORT
