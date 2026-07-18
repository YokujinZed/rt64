//
// RT64 - OpenXR context.
//
// Generic OpenXR layer usable by any host application driving RT64. Owns the
// XrInstance/XrSession lifecycle and a frame loop thread. Hosts read controller
// state through a snapshot API and map it to their own input semantics.
//
// M1 scope: session + zero-layer frame loop + controller input. No rendering.
//

#pragma once

#ifdef RT64_XR_SUPPORT

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32
#   include <d3d12.h>
#   define XR_USE_GRAPHICS_API_D3D12
#endif

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

namespace RT64 {
    // Generic single-controller state. Hosts map this to game-specific inputs.
    struct XRControllerState {
        bool active = false;
        float stickX = 0.0f;
        float stickY = 0.0f;
        bool stickClick = false;
        float trigger = 0.0f;
        float squeeze = 0.0f;
        bool primaryButton = false;   // A (right) / X (left)
        bool secondaryButton = false; // B (right) / Y (left)
        bool menuButton = false;      // left-hand menu on Touch controllers
    };

    struct XRInputSnapshot {
        XRControllerState left;
        XRControllerState right;
        // Incremented once per successful xrSyncActions. Hosts can detect a
        // stalled session (e.g. headset asleep) by watching this stop moving.
        uint64_t sampleCount = 0;
    };

    struct XRContext {
        XRContext() = default;
        ~XRContext();
        XRContext(const XRContext &) = delete;
        XRContext &operator=(const XRContext &) = delete;

        // Creates the XrInstance and locates an HMD system. Returns false (and
        // logs) if no runtime or no HMD is available. Safe to call on any thread.
        bool initInstance();

#   ifdef _WIN32
        // Creates the session with a D3D12 binding and starts the frame loop
        // thread. Must be called after initInstance succeeds. The device and
        // queue must outlive this context (guaranteed when called from
        // Application::setup, torn down in Application::end).
        bool beginSessionD3D12(ID3D12Device *device, ID3D12CommandQueue *queue);
#   endif

        // Stops the frame loop thread and destroys session + instance.
        // Idempotent; called by the destructor if the host does not.
        void shutdown();

        // Thread-safe copy of the latest controller state.
        XRInputSnapshot sampleInput() const;

        // True while the runtime session is between xrBeginSession/xrEndSession.
        bool isSessionRunning() const;

    private:
        void frameLoop();
        bool createActions();
        void syncActions(XrTime predictedDisplayTime);
        void pollEvents();
        void destroyHandles();

        XrInstance instance = XR_NULL_HANDLE;
        XrSystemId systemId = XR_NULL_SYSTEM_ID;
        XrSession session = XR_NULL_HANDLE;
        XrEnvironmentBlendMode blendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        std::string systemName;

        XrActionSet actionSet = XR_NULL_HANDLE;
        XrAction stickAction = XR_NULL_HANDLE;
        XrAction stickClickAction = XR_NULL_HANDLE;
        XrAction triggerAction = XR_NULL_HANDLE;
        XrAction squeezeAction = XR_NULL_HANDLE;
        XrAction primaryAction = XR_NULL_HANDLE;
        XrAction secondaryAction = XR_NULL_HANDLE;
        XrAction menuAction = XR_NULL_HANDLE;
        XrPath handPaths[2] = { XR_NULL_PATH, XR_NULL_PATH }; // 0 = left, 1 = right

        std::thread frameThread;
        std::atomic<bool> quitRequested{ false };
        std::atomic<bool> sessionRunning{ false };
        std::atomic<bool> shutdownDone{ false };

        mutable std::mutex snapshotMutex;
        XRInputSnapshot snapshot;
    };
};

#endif // RT64_XR_SUPPORT
