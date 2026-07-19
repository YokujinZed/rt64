//
// RT64 - XR frame metadata.
//
// Plain-float pose metadata that travels with rendered stereo frames from the
// workload thread to the layer submission, so the compositor is always told
// the exact pose a frame was rendered with (the anti-swim invariant once head
// poses are dynamic). Kept free of OpenXR types so renderer-side headers can
// include it cheaply.
//

#pragma once

#ifdef RT64_XR_SUPPORT

namespace RT64 {
    struct XRStereoFrameMeta {
        bool valid = false;
        // Per eye (0 = left, 1 = right), in the submitted layer's space.
        float posePosition[2][3] = {};
        float poseOrientation[2][4] = {}; // x, y, z, w
    };
};

#endif // RT64_XR_SUPPORT
