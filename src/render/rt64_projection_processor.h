//
// RT64
//

#pragma once

#include "hle/rt64_game_frame.h"

#include "rt64_buffer_uploader.h"

namespace RT64 {
    struct ProjectionProcessor {
        std::unique_ptr<BufferUploader> bufferUploader;
        std::vector<BufferUploader::Upload> uploads;

        struct ProcessParams {
            RenderWorker *worker = nullptr;
            WorkloadQueue *workloadQueue = nullptr;
            GameFrame *curFrame = nullptr;
            const GameFrame *prevFrame = nullptr;
            float curFrameWeight = 1.0f;
            float prevFrameWeight = 0.0f;
            float aspectRatioScale = 1.0f;
            // Stereo eye override (VR): when enabled, perspective scenes get
            // their view right-multiplied by eyeViewOffset and their projection
            // replaced by an asymmetric frustum built from the tangent
            // half-angles (near/far preserved from the game's projection).
            bool eyeOverrideEnabled = false;
            hlslpp::float4x4 eyeViewOffset;
            float eyeTanLeft = -1.0f;
            float eyeTanRight = 1.0f;
            float eyeTanDown = -1.0f;
            float eyeTanUp = 1.0f;
        };

        ProjectionProcessor();
        ~ProjectionProcessor();
        void setup(RenderWorker *worker);
        void process(const ProcessParams &p);
        void processScene(const ProcessParams &p, const GameScene &scene, size_t sceneIndex);
        void upload(const ProcessParams &p);
    };
};