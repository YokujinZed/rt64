//
// RT64
//

#include "rt64_projection_processor.h"

#include <cmath>

#include "common/rt64_math.h"
#include "hle/rt64_workload_queue.h"

namespace RT64 {
    inline void adjustProjectionMatrix(interop::float4x4 &matrix, const float aspectRatioScale) {
        matrix[0][0] *= aspectRatioScale;
        matrix[1][0] *= aspectRatioScale;
        matrix[2][0] *= aspectRatioScale;
        matrix[3][0] *= aspectRatioScale;
    }
    
    // ProjectionProcessor

    ProjectionProcessor::ProjectionProcessor() { }

    ProjectionProcessor::~ProjectionProcessor() {
        bufferUploader.reset(nullptr);
    }

    void ProjectionProcessor::setup(RenderWorker *worker) {
        bufferUploader = std::make_unique<BufferUploader>(worker->device);
    }

    void ProjectionProcessor::process(const ProcessParams &p) {
        for (uint32_t w : p.curFrame->workloads) {
            Workload &workload = p.workloadQueue->workloads[w];
            DrawData &drawData = workload.drawData;

            // Copy the data.
            drawData.modViewTransforms = drawData.viewTransforms;
            drawData.modProjTransforms = drawData.projTransforms;
            drawData.modViewProjTransforms = drawData.viewProjTransforms;
            drawData.prevViewTransforms = drawData.viewTransforms;
            drawData.prevProjTransforms = drawData.projTransforms;
            drawData.prevViewProjTransforms = drawData.viewProjTransforms;
        }

        for (size_t s = 0; s < p.curFrame->perspectiveScenes.size(); s++) {
            processScene(p, p.curFrame->perspectiveScenes[s], s);
        }

        for (size_t s = 0; s < p.curFrame->orthographicScenes.size(); s++) {
            processScene(p, p.curFrame->orthographicScenes[s], s);
        }
    }

    void ProjectionProcessor::processScene(const ProcessParams &p, const GameScene &scene, size_t sceneIndex) {
        for (size_t i = 0; i < scene.projections.size(); i++) {
            const GameIndices::Projection &sceneProj = scene.projections[i];
            Workload &workload = p.workloadQueue->workloads[sceneProj.workloadIndex];
            DrawData &drawData = workload.drawData;
            const FramebufferPair &fbPair = workload.fbPairs[sceneProj.fbPairIndex];
            const Projection &proj = fbPair.projections[sceneProj.projectionIndex];
            const uint16_t viewportOrigin = drawData.viewportOrigins[proj.transformsIndex];
            assert(proj.transformsIndex > 0);

            // Skip projections that didn't actually draw anything.
            if (proj.scissorRect.isNull()) {
                continue;
            }

            // Check the current mapping for the projection.
            const interop::float4x4 *prevProjMatrix = nullptr;
            const interop::float4x4 *prevViewMatrix = nullptr;
            const RigidBody *rigidBody = nullptr;
            const GameFrameMap::WorkloadMap &workloadMap = p.curFrame->frameMap.workloads[sceneProj.workloadIndex];
            if ((p.prevFrame != nullptr) && workloadMap.mapped && !workload.debuggerCamera.enabled) {
                const GameFrameMap::ViewProjectionMap &viewProjMap = workloadMap.viewProjections[proj.transformsIndex];
                if (viewProjMap.mapped) {
                    const Workload &prevWorkload = p.workloadQueue->workloads[workloadMap.prevWorkloadIndex];
                    prevViewMatrix = &prevWorkload.drawData.viewTransforms[viewProjMap.prevTransformIndex];
                    prevProjMatrix = &prevWorkload.drawData.projTransforms[viewProjMap.prevTransformIndex];
                    rigidBody = &viewProjMap.rigidBody;
                }
            }

            const uint32_t curProjGroupIndex = workload.drawData.viewProjTransformGroups[proj.transformsIndex];
            const TransformGroup &curProjGroup = workload.drawData.transformGroups[curProjGroupIndex];
            bool adjustAspectRatio = (curProjGroup.aspectMode == G_EX_ASPECT_ADJUST);
            if (curProjGroup.aspectMode == G_EX_ASPECT_AUTO) {
                FixedRect intersectionRect = proj.scissorRect;
                if (proj.usesViewport()) {
                    const interop::RSPViewport &viewport = drawData.rspViewports[proj.transformsIndex];
                    const int16_t *viewportClipRatios = &drawData.viewportClipRatios[proj.transformsIndex * 4];
                    intersectionRect = intersectionRect.intersection(viewport.rect(viewportClipRatios));
                }

                if (!intersectionRect.isEmpty()) {
                    bool coversWholeWidth = (intersectionRect.ulx <= fbPair.scissorRect.ulx) && (intersectionRect.lrx >= fbPair.scissorRect.lrx);
                    bool horizontalRatio = (intersectionRect.width(true, true) > intersectionRect.height(true, true));
                    adjustAspectRatio = (viewportOrigin == G_EX_ORIGIN_NONE) && coversWholeWidth && horizontalRatio;
                }
            }
 
            float projRatioScale = adjustAspectRatio ? (1.0f / p.aspectRatioScale) : 1.0f;
            interop::float4x4 &viewMatrix = drawData.modViewTransforms[proj.transformsIndex];
            interop::float4x4 &projMatrix = drawData.modProjTransforms[proj.transformsIndex];
            interop::float4x4 &viewProjMatrix = drawData.modViewProjTransforms[proj.transformsIndex];
            viewMatrix = drawData.viewTransforms[proj.transformsIndex];
            projMatrix = drawData.projTransforms[proj.transformsIndex];
            viewProjMatrix = drawData.viewProjTransforms[proj.transformsIndex];

            // Debugger camera.
            if (workload.debuggerCamera.enabled && (proj.type == Projection::Type::Perspective) && (workload.debuggerCamera.sceneIndex == sceneIndex)) {
                viewMatrix = workload.debuggerCamera.viewMatrix;
                projMatrix = workload.debuggerCamera.projMatrix;
            }

            adjustProjectionMatrix(projMatrix, projRatioScale);

            interop::float4x4 &prevViewTransform = drawData.prevViewTransforms[proj.transformsIndex];
            interop::float4x4 &prevProjTransform = drawData.prevProjTransforms[proj.transformsIndex];
            if ((prevProjMatrix != nullptr) && (prevViewMatrix != nullptr) && (rigidBody != nullptr)) {
                const interop::float4x4 curViewTransform = viewMatrix;
                const interop::float4x4 curProjTransform = projMatrix;
                interop::float4x4 adjustedPrevProj = *prevProjMatrix;
                adjustProjectionMatrix(adjustedPrevProj, projRatioScale);
                viewMatrix = rigidBody->lerp(p.curFrameWeight, *prevViewMatrix, curViewTransform, true);
                prevViewTransform = rigidBody->lerp(p.prevFrameWeight, *prevViewMatrix, curViewTransform, true);

                // We only interpolate the projection if the view matrix has been interpolated.
                const bool interpolateProjection = rigidBody->lerpTranslation || rigidBody->lerpRotation;
                if (interpolateProjection) {
                    projMatrix = lerpMatrix(adjustedPrevProj, curProjTransform, p.curFrameWeight);
                    prevProjTransform = lerpMatrix(adjustedPrevProj, curProjTransform, p.prevFrameWeight);
                }
                else {
                    projMatrix = curProjTransform;
                    prevProjTransform = curProjTransform;
                }
            }
            else {
                prevViewTransform = viewMatrix;
                prevProjTransform = projMatrix;
            }

            // Stereo eye override (VR): shift the VIEW by the per-eye offset
            // and keep the game's own projection matrix untouched. This mirrors
            // the shipped free-camera (rt64_debugger_inspector.cpp:1710, which
            // replaces only the view and keeps projMatrix), guaranteeing the
            // projection stays in the N64 clip convention the RSP compute pass
            // expects. Stereo depth comes from the parallax of the two views.
            // Applied after interpolation so the offset rides the smooth camera.
            // Camera observation, independent of VR: the follow controller and
            // the first-person calibration both need the world camera, and
            // sampling it here means a flat run can calibrate too. Several
            // perspective projections share a frame and the ones that are not
            // the scene camera carry an identity view, so the strongest
            // translation wins rather than the last one processed.
            if ((proj.type == Projection::Type::Perspective) && !workload.debuggerCamera.enabled &&
                ((p.outCameraYaw != nullptr) || (p.outCameraPos != nullptr))) {
                const hlslpp::float4x4 invViewForYaw = hlslpp::inverse(viewMatrix);
                const float posX = float(invViewForYaw[3].x);
                const float posY = float(invViewForYaw[3].y);
                const float posZ = float(invViewForYaw[3].z);

                if (p.outPerspectiveCount != nullptr) {
                    *p.outPerspectiveCount += 1;
                }

                const float posLenSq = posX * posX + posY * posY + posZ * posZ;
                float bestLenSq = 0.0f;
                if (p.outCameraPos != nullptr) {
                    bestLenSq = p.outCameraPos[0] * p.outCameraPos[0] +
                                p.outCameraPos[1] * p.outCameraPos[1] +
                                p.outCameraPos[2] * p.outCameraPos[2];
                }

                if (posLenSq >= bestLenSq) {
                    if (p.outCameraPos != nullptr) {
                        p.outCameraPos[0] = posX;
                        p.outCameraPos[1] = posY;
                        p.outCameraPos[2] = posZ;
                    }
                    if (p.outCameraYaw != nullptr) {
                        const float backX = float(invViewForYaw[2].x);
                        const float backZ = float(invViewForYaw[2].z);
                        if ((backX * backX + backZ * backZ) > 1e-8f) {
                            *p.outCameraYaw = std::atan2(backX, backZ);
                        }
                    }
                }
            }

            if (p.eyeOverrideEnabled && (proj.type == Projection::Type::Perspective) && !workload.debuggerCamera.enabled) {
                if (p.eyeLevelAnchor) {

                    // Head tracking: compose the eye offset against a LEVEL
                    // (gravity-aligned) frame at the game camera — position and
                    // yaw only, with the head supplying pitch/roll. Composing
                    // in the raw (pitched) camera frame rotates head yaw about
                    // a tilted axis, which rolls the horizon and corrupts the
                    // turn direction; the level frame also matches the
                    // gravity-aligned anchor space the layer is submitted in.
                    // Conventions per lookAtPerspective: invView rows are
                    // side / up / backward / position.
                    auto levelViewOf = [](const interop::float4x4 &view, interop::float4x4 &outLevel) {
                        const hlslpp::float4x4 invView = hlslpp::inverse(view);
                        hlslpp::float3 backward = invView[2].xyz;
                        backward.y = 0.0f;
                        const float backLength = float(hlslpp::length(backward));
                        if (backLength < 1e-4f) {
                            // Near-vertical camera: no stable yaw, keep as-is.
                            outLevel = view;
                            return;
                        }

                        backward = backward / backLength;
                        const hlslpp::float3 up = hlslpp::float3(0.0f, 1.0f, 0.0f);
                        const hlslpp::float3 side = hlslpp::normalize(hlslpp::cross(up, backward));
                        hlslpp::float4x4 invLevel;
                        invLevel[0] = hlslpp::float4(side, 0.0f);
                        invLevel[1] = hlslpp::float4(up, 0.0f);
                        invLevel[2] = hlslpp::float4(backward, 0.0f);
                        invLevel[3] = hlslpp::float4(invView[3].xyz, 1.0f);
                        outLevel = hlslpp::inverse(invLevel);
                    };

                    // First person moves the eye forward (and vertically) in
                    // the camera's own space, applied before the head offset
                    // so looking around still pivots about the new eye point.
                    const hlslpp::float4x4 fpDolly = p.fpEnabled
                        ? matrixTranslation(hlslpp::float3(0.0f, p.fpHeight, p.fpForward))
                        : hlslpp::float4x4::identity();

                    interop::float4x4 levelView;
                    levelViewOf(viewMatrix, levelView);
                    viewMatrix = hlslpp::mul(hlslpp::mul(levelView, fpDolly), p.eyeViewOffset);

                    interop::float4x4 prevLevelView;
                    levelViewOf(prevViewTransform, prevLevelView);
                    prevViewTransform = hlslpp::mul(hlslpp::mul(prevLevelView, fpDolly), p.eyeViewOffset);
                }
                else {
                    // Stereo without head tracking: pure IPD offset in the
                    // camera's own frame, game pitch preserved.
                    viewMatrix = hlslpp::mul(viewMatrix, p.eyeViewOffset);
                    prevViewTransform = hlslpp::mul(prevViewTransform, p.eyeViewOffset);
                }

                // Report the game's symmetric FOV so the XR projection layer
                // echoes the frustum it was rendered with (m[0][0]/m[1][1] are
                // the x/y scales = cot(halfFov)).
                if ((p.outTanX != nullptr) && (p.outTanY != nullptr)) {
                    const float tanY = std::tan(0.5f * fovFromProj(projMatrix));
                    const float scaleX = std::abs(projMatrix[0][0]);
                    const float scaleY = std::abs(projMatrix[1][1]);
                    *p.outTanY = tanY;
                    *p.outTanX = (scaleX > 1e-6f) ? (tanY * scaleY / scaleX) : tanY;
                }
            }

            viewProjMatrix = hlslpp::mul(viewMatrix, projMatrix);

            interop::float4x4 &prevViewProjTransform = drawData.prevViewProjTransforms[proj.transformsIndex];
            prevViewProjTransform = hlslpp::mul(prevViewTransform, prevProjTransform);
        }
    }

    void ProjectionProcessor::upload(const ProcessParams &p) {
        uploads.clear();

        for (uint32_t w : p.curFrame->workloads) {
            Workload &workload = p.workloadQueue->workloads[w];
            const DrawData &drawData = workload.drawData;
            DrawBuffers &drawBuffers = workload.drawBuffers;
            std::pair<size_t, size_t> uploadRange = { 0, drawData.viewProjTransforms.size() };
            uploads.emplace_back(BufferUploader::Upload{ drawData.modViewProjTransforms.data(), uploadRange, sizeof(interop::float4x4), RenderBufferFlag::STORAGE, { }, &drawBuffers.viewProjTransformsBuffer });
        }

        bufferUploader->submit(p.worker, uploads);
    }
};