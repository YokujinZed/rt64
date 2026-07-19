//
// RT64 - OpenXR context.
//
// Generic OpenXR layer usable by any host application driving RT64. Owns the
// XrInstance/XrSession lifecycle and a frame loop thread. Hosts read controller
// state through a snapshot API and map it to their own input semantics.
//
// M1: session + zero-layer frame loop + controller input.
// M2: cinema layer - the present thread copies the final composed frame into an
//     XR swapchain (pt_* API) and the frame loop submits it as a quad layer,
//     pacing the present thread from xrWaitFrame ticks.
//

#pragma once

#ifdef RT64_XR_SUPPORT

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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

    // Per-eye stereo parameters in renderer-friendly form: a row-vector
    // head-to-eye view offset (translation already in game units) and the
    // frustum tangent half-angles. No XR types so render code stays generic.
    struct XREyeParams {
        bool valid = false;
        float viewOffset[4][4] = {};
        float tanLeft = -1.0f;
        float tanRight = 1.0f;
        float tanDown = -1.0f;
        float tanUp = 1.0f;
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

        // ---- Present-thread API (cinema layer). ----
        // All pt_* methods must be called from a single thread (RT64's present
        // thread). The XR swapchain image cycle lives entirely on that thread;
        // the frame loop thread only submits the last released image.

        // Ensures the XR swapchain matches (width, height), acquires + waits an
        // image and records a raw D3D12 copy of sourceTexture into it. Returns
        // false to skip this frame (no session, wait timeout, format issue);
        // callers must then not call pt_releaseFrame for this frame.
        // sourceTexture must be in D3D12_RESOURCE_STATE_COPY_SOURCE.
        bool pt_recordFrameCopy(ID3D12GraphicsCommandList *commandList, ID3D12Resource *sourceTexture, uint32_t width, uint32_t height);

        // Releases the acquired image. Call only after the recorded copy has
        // been submitted and its fence waited on (GPU-complete).
        void pt_releaseFrame();

        // Stereo variants: an arraySize=2 swapchain fed by two eye images of
        // identical size/format (matched to the sources' DXGI format). Same
        // contract as the mono pair above.
        bool pt_recordStereoCopy(ID3D12GraphicsCommandList *commandList, ID3D12Resource *leftTexture, ID3D12Resource *rightTexture, uint32_t width, uint32_t height);
        void pt_releaseStereoFrame();

        // Blocks until the frame loop's next xrWaitFrame tick (or a short
        // timeout / session death). Returns true if a tick paced this call.
        bool pt_waitForDisplayTick();

        // HMD refresh rate estimated from predictedDisplayPeriod. 0 if unknown.
        uint32_t displayRateHz() const;
#   endif

        // Stops the frame loop thread and destroys session + instance.
        // Idempotent; called by the destructor if the host does not.
        void shutdown();

        // Thread-safe copy of the latest controller state.
        XRInputSnapshot sampleInput() const;

        // True while the runtime session is between xrBeginSession/xrEndSession.
        bool isSessionRunning() const;

        // Queue a recenter of the cinema quad to the current view (also bound
        // to left-stick-click on the controllers).
        void requestRecenter();

        // Stereo (M3). Eye views are located head-relative (VIEW space), so
        // buildEyeParams yields pure stereo offsets — head tracking composes
        // separately in a later milestone.
        void setStereoEnabled(bool enabled);
        bool isStereoEnabled() const;
        void setUnitsPerMeter(float units);
        XREyeParams buildEyeParams(uint32_t eyeIndex) const; // 0 = left, 1 = right
        // The game's symmetric FOV half-tangents, so the projection layer
        // submits a frustum matching what was rendered (stereo fusion).
        void setRenderedFov(float tanX, float tanY);

    private:
        void frameLoop();
        bool createActions();
        bool createSpaces();
        void syncActions(XrTime predictedDisplayTime);
        void pollEvents();
        void destroyHandles();
        void recenterQuadSpace(XrTime time);
#   ifdef _WIN32
        bool ensureQuadSwapchain(uint32_t width, uint32_t height);
        void destroyQuadSwapchain();
        bool ensureStereoSwapchain(uint32_t width, uint32_t height, int64_t format);
        void destroyStereoSwapchain();
#   endif

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

        // Spaces: baseSpace is a pristine LOCAL space used only for locating
        // the view; quadSpace is LOCAL with the latest recenter pose baked in.
        XrSpace baseSpace = XR_NULL_HANDLE;
        XrSpace viewSpace = XR_NULL_HANDLE;
        XrSpace quadSpace = XR_NULL_HANDLE;

        // Cinema quad swapchain. Handles are guarded by quadMutex against the
        // frame loop's xrEndFrame; the image cycle state is present-thread-only.
        std::mutex quadMutex;
        XrSwapchain quadSwapchain = XR_NULL_HANDLE;
        int64_t quadFormat = 0; // 0 = not chosen yet, -1 = unavailable
        uint32_t quadWidth = 0;
        uint32_t quadHeight = 0;
#   ifdef _WIN32
        std::vector<XrSwapchainImageD3D12KHR> quadImages;
#   endif
        bool quadImageAcquired = false;
        uint32_t quadImageIndex = 0;
        bool quadPendingRelease = false;
        std::atomic<bool> quadReady{ false };

        // Stereo projection swapchain (arraySize = 2), same ownership rules as
        // the quad swapchain: handles under quadMutex, image cycle on the
        // present thread only.
        XrSwapchain stereoSwapchain = XR_NULL_HANDLE;
        int64_t stereoFormat = 0;
        int64_t stereoRefusedFormat = 0;
        uint32_t stereoWidth = 0;
        uint32_t stereoHeight = 0;
#   ifdef _WIN32
        std::vector<XrSwapchainImageD3D12KHR> stereoImages;
#   endif
        bool stereoImageAcquired = false;
        uint32_t stereoImageIndex = 0;
        bool stereoPendingRelease = false;
        std::atomic<bool> stereoReady{ false };
        bool stereoTimeoutLogged = false;

        // Freshness stamps: the frame loop submits whichever layer was fed most
        // recently, so a stalled stereo path hands off to the live cinema copy
        // (and back) automatically.
        std::atomic<uint64_t> layerSerial{ 0 };
        std::atomic<uint64_t> quadSerial{ 0 };
        std::atomic<uint64_t> stereoSerial{ 0 };

        // Display pacing ticks, one per xrWaitFrame.
        std::mutex tickMutex;
        std::condition_variable tickCondition;
        uint64_t tickCounter = 0;
        uint64_t lastConsumedTick = 0;
        std::atomic<int64_t> displayPeriodNs{ 0 };

        std::atomic<bool> recenterRequested{ false };
        bool prevRecenterClick = false;

        // Head-relative eye views, refreshed once per XR frame.
        mutable std::mutex eyeViewsMutex;
        XrView eyeViews[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
        bool eyeViewsValid = false;
        std::atomic<bool> stereoEnabled{ false };
        std::atomic<float> unitsPerMeter{ 100.0f };
        std::atomic<float> renderedTanX{ 0.0f };
        std::atomic<float> renderedTanY{ 0.0f };

        std::thread frameThread;
        std::atomic<bool> quitRequested{ false };
        std::atomic<bool> sessionRunning{ false };
        std::atomic<bool> shutdownDone{ false };

        mutable std::mutex snapshotMutex;
        XRInputSnapshot snapshot;
    };
};

#endif // RT64_XR_SUPPORT
