/*
 * Copyright 2022 Michael Goffioul <michael.goffioul@gmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef C2_FFMPEG_VIDEO_DECODE_COMPONENT_H
#define C2_FFMPEG_VIDEO_DECODE_COMPONENT_H

#include <deque>
#include <utility>
#include <SimpleC2Component.h>
#include "C2FFMPEGCommon.h"
#include "C2FFMPEGVideoDecodeInterface.h"
#include "C2FFMPEGZeroCopyRegistry.h"
#include "ZeroCopyQuarantine.h"
#include <cros_gralloc/cros_gralloc_handle.h>
#include <unordered_map>
#include <vector>

namespace android {

typedef std::pair<uint64_t, uint64_t> PendingWork;
struct DrmBufEntry {
    uint32_t id = 0;
    uint64_t ino = 0;
    int dma_buf_fd = -1;
    int meta_fd = -1;
    struct cros_gralloc_handle* cros_handle = nullptr;
    AVFrame* heldFrame = nullptr;
    enum State {
        FREE_IN_FFMPEG = 0,
        HELD_UNSENT = 1,
        AT_CLIENT_OR_BQ = 2,
        DETACHED_FENCE_PENDING = 3,
    } state = FREE_IN_FFMPEG;
    int32_t currentSlot = -1;
    uint32_t staleInstances = 0;
};

struct PendingFenceBuffer {
    uint32_t bufId = 0;
    int fenceFd = -1;
    bool hasRealFence = false;
};


class C2FFMPEGVideoDecodeComponent : public SimpleC2Component {
public:
    explicit C2FFMPEGVideoDecodeComponent(
        const C2FFMPEGComponentInfo* componentInfo,
        const std::shared_ptr<C2FFMPEGVideoDecodeInterface>& intf);
    virtual ~C2FFMPEGVideoDecodeComponent();

protected:
    c2_status_t onInit() override;
    c2_status_t onStop() override;
    void onReset() override;
    void onRelease() override;
    c2_status_t onFlush_sm() override;
    void process(
        const std::unique_ptr<C2Work> &work,
        const std::shared_ptr<C2BlockPool> &pool) override;
    c2_status_t drain(
        uint32_t drainMode,
        const std::shared_ptr<C2BlockPool> &pool) override;

private:
    c2_status_t initDecoder();
    c2_status_t openDecoder();
    void deInitDecoder();
    c2_status_t processCodecConfig(C2ReadView* inBuffer);
    c2_status_t sendInputBuffer(C2ReadView* inBuffer, int64_t timestamp);
    c2_status_t receiveFrame(bool* hasPicture);
    c2_status_t getOutputBuffer(C2GraphicView* outBuffer);
    c2_status_t outputFrame(
        const std::unique_ptr<C2Work> &work,
        const std::shared_ptr<C2BlockPool> &pool);

    void pushPendingWork(const std::unique_ptr<C2Work>& work);
    void popPendingWork(const std::unique_ptr<C2Work>& work);
    void prunePendingWorksUntil(const std::unique_ptr<C2Work>& work);
    void drainDetachNextBuffer(
            const sp<::android::hardware::graphics::bufferqueue::V2_0::IGraphicBufferProducer>& producer,
            const std::shared_ptr<C2SurfaceSyncMemory>& syncMem);
    void clearZeroCopyBuffers();
    void destroyZeroCopyBuffers();
    size_t getHeldFrameCount() const;
    void reclaimBufferById(uint32_t bufId, bool hasRealFence);
    bool isZeroCopyCandidate() const;


private:
    const C2FFMPEGComponentInfo* mInfo;
    std::shared_ptr<C2FFMPEGVideoDecodeInterface> mIntf;
    enum AVCodecID mCodecID;
    AVCodecContext* mCtx;
    struct SwsContext *mImgConvertCtx;
    AVFrame* mFrame;
    AVPacket* mPacket;
    bool mFFMPEGInitialized;
    bool mCodecAlreadyOpened;
    bool mExtradataReady;
    bool mEOSSignalled;
    // Set once the one-pass transfer into the output buffer fails; later
    // pictures use the temporary-copy + sws_scale path.
    bool mDirectTransferFailed;
    // configUpdate of a picture skipped after its size change was applied;
    // sent with the next output picture.
    std::vector<std::unique_ptr<C2Param>> mPendingConfigUpdate;
    std::deque<PendingWork> mPendingWorkQueue;
    // Zero-copy state
    bool mZeroCopyMode;
    bool mZeroCopySupported;
    uint32_t mLastProducerChangeSeq;
    uint32_t mLastGeneration;
    uint32_t mLastReportedChangeSeq;
    std::vector<DrmBufEntry> mDrmBuffers;
    std::unordered_map<uint64_t, size_t> mInoToBufIndex;
    int mSlotToBufIndex[64];
    ZeroCopyQuarantine<AVFrame*> mQuarantine;
    std::vector<PendingFenceBuffer> mPendingFences;
    uint64_t mOutputPoolId;
    void* mLastHwFramesCtxData;
    uint64_t mReclaimSeq;
    size_t mHeldCap;

    sp<::android::hardware::graphics::bufferqueue::V2_0::IGraphicBufferProducer> mCurrentProducer;
    std::shared_ptr<C2SurfaceSyncMemory> mCurrentSyncMem;

    // Zero-copy counters
    size_t mReclaimedCount;
    size_t mAttachFailCount;
    size_t mQuarantinedCount;
    int64_t mLastStatLogUs;
    std::atomic<int> mLiveHalRefs;
    uint32_t mOutputCount;
};

} // namespace android

#endif // C2_FFMPEG_VIDEO_DECODE_COMPONENT_H
