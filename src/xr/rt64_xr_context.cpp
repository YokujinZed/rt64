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

        if (!createActions()) {
            destroyHandles();
            return false;
        }

        quitRequested = false;
        frameThread = std::thread(&XRContext::frameLoop, this);
        return true;
    }
#   endif

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

            XrFrameBeginInfo beginInfo = { XR_TYPE_FRAME_BEGIN_INFO };
            if (XR_FAILED(xrBeginFrame(session, &beginInfo))) {
                continue;
            }

            syncActions(frameState.predictedDisplayTime);

            // M1: no layers. The headset shows the runtime's idle environment.
            XrFrameEndInfo endInfo = { XR_TYPE_FRAME_END_INFO };
            endInfo.displayTime = frameState.predictedDisplayTime;
            endInfo.environmentBlendMode = blendMode;
            endInfo.layerCount = 0;
            endInfo.layers = nullptr;
            xrEndFrame(session, &endInfo);
        }

        // Clear controller state so hosts stop seeing stale input.
        const std::lock_guard<std::mutex> lock(snapshotMutex);
        snapshot.left = XRControllerState();
        snapshot.right = XRControllerState();
    }

    void XRContext::destroyHandles() {
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
