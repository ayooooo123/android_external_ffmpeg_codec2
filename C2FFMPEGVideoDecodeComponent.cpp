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

#define LOG_TAG "C2FFMPEGVideoDecodeComponent"
#include <android-base/properties.h>
#include <log/log.h>
#include <algorithm>

#include <SimpleC2Interface.h>
#include "C2FFMPEGVideoDecodeComponent.h"
#include "ffmpeg_hwaccel.h"
#include "C2FFMPEGZeroCopyRegistry.h"
#include <drm/drm_fourcc.h>
#include <cros_gralloc/cros_gralloc_handle.h>
#include <cros_gralloc/cros_gralloc_buffer_metadata.h>
#include <ui/GraphicBuffer.h>
#include <ui/GraphicBufferMapper.h>
#include <ui/Fence.h>
#include <C2AllocatorGralloc.h>
#include <C2BqBufferPriv.h>
#include <C2BlockInternal.h>
#include <C2PlatformSupport.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <chrono>
#include <atomic>
#include <inttypes.h>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/pixfmt.h>
}

#ifndef DRM_FORMAT_P030
#define DRM_FORMAT_P030 fourcc_code('P', '0', '3', '0')
#endif

#ifndef HAL_PIXEL_FORMAT_YCBCR_P010
#define HAL_PIXEL_FORMAT_YCBCR_P010 0x36
#endif

#define DEBUG_FRAMES 0
#ifndef FFMPEG_HW_HEVC_THREADS
#define FFMPEG_HW_HEVC_THREADS 2
#endif
#define DEBUG_WORKQUEUE 0
#define DEBUG_EXTRADATA 0

namespace android {
using ::android::hardware::Return;

static inline bool isFenceSignalled(int fd) {
    if (fd < 0) return true;
    sp<Fence> f = new Fence(dup(fd));
    return f->wait(0) == NO_ERROR;
}

C2FFMPEGVideoDecodeComponent::C2FFMPEGVideoDecodeComponent(
        const C2FFMPEGComponentInfo* componentInfo,
        const std::shared_ptr<C2FFMPEGVideoDecodeInterface>& intf)
    : SimpleC2Component(std::make_shared<SimpleInterface<C2FFMPEGVideoDecodeInterface>>(componentInfo->name, 0, intf)),
      mInfo(componentInfo),
      mIntf(intf),
      mCodecID(componentInfo->codecID),
      mCtx(NULL),
      mImgConvertCtx(NULL),
      mFrame(NULL),
      mPacket(NULL),
      mFFMPEGInitialized(false),
      mCodecAlreadyOpened(false),
      mExtradataReady(false),
      mEOSSignalled(false),
      mDirectTransferFailed(false),
      mZeroCopyMode(false),
      mZeroCopySupported(true),
      mLastProducerChangeSeq(0),
      mLastGeneration(0),
      mLastReportedChangeSeq(0),
      mOutputPoolId(0),
      mLastHwFramesCtxData(nullptr),
      mReclaimSeq(0),
      mHeldCap(6),
      mReclaimedCount(0),
      mAttachFailCount(0),
      mQuarantinedCount(0),
      mLastStatLogUs(0),
      mLiveHalRefs(0),
      mOutputCount(0) {
    for (int i = 0; i < 64; ++i) {
        mSlotToBufIndex[i] = -1;
    }
    ALOGD("C2FFMPEGVideoDecodeComponent: mediaType = %s", componentInfo->mediaType);
}

C2FFMPEGVideoDecodeComponent::~C2FFMPEGVideoDecodeComponent() {
    ALOGD("~C2FFMPEGVideoDecodeComponent: mCtx = %p", mCtx);
    destroyZeroCopyBuffers();
    onRelease();
}

c2_status_t C2FFMPEGVideoDecodeComponent::initDecoder() {
    if (! mFFMPEGInitialized) {
        if (initFFmpeg() != C2_OK) {
            ALOGE("initDecoder: FFMPEG initialization failed.");
            return C2_NO_INIT;
        }
        mFFMPEGInitialized = true;
    }

    mCtx = avcodec_alloc_context3(NULL);
    if (! mCtx) {
        ALOGE("initDecoder: avcodec_alloc_context failed.");
        return C2_NO_MEMORY;
    }

    C2StreamPictureSizeInfo::output size(0u, 320, 240);
    c2_status_t err = mIntf->query({ &size }, {}, C2_DONT_BLOCK, nullptr);
    if (err != C2_OK) {
        ALOGE("initDecoder: cannot query picture size, err = %d", err);
    }

    mCtx->codec_type = AVMEDIA_TYPE_VIDEO;
    mCtx->codec_id = mCodecID;
    mCtx->extradata_size = 0;
    mCtx->extradata = NULL;
    mCtx->width = size.width;
    mCtx->height = size.height;

    ALOGD("initDecoder: %p [%s], %d x %d, %s",
          mCtx, avcodec_get_name(mCtx->codec_id), size.width, size.height, mInfo->mediaType);

    return C2_OK;
}

c2_status_t C2FFMPEGVideoDecodeComponent::openDecoder() {
    if (mCodecAlreadyOpened) {
        return C2_OK;
    }

    // Can't change extradata after opening the decoder.
#if DEBUG_EXTRADATA
    ALOGD("openDecoder: extradata_size = %d", mCtx->extradata_size);
#endif
    mExtradataReady = true;

    // Find decoder again as codec_id may have changed.
    if (mCtx->codec_id == AV_CODEC_ID_H264 &&
            base::GetBoolProperty("persist.vendor.ffmpeg_codec2.v4l2.h264", false)) {
        mCtx->codec = avcodec_find_decoder_by_name("h264_v4l2m2m");
    } else {
        mCtx->codec = avcodec_find_decoder(mCtx->codec_id);
    }

    if (! mCtx->codec) {
        ALOGE("openDecoder: ffmpeg video decoder failed to find codec %d", mCtx->codec_id);
        return C2_NOT_FOUND;
    }

    // Configure decoder.
    mCtx->workaround_bugs   = 1;
    mCtx->idct_algo         = 0;
    mCtx->skip_frame        = AVDISCARD_DEFAULT;
    mCtx->skip_idct         = AVDISCARD_DEFAULT;
    mCtx->skip_loop_filter  = AVDISCARD_DEFAULT;
    mCtx->error_concealment = 3;
    mCtx->thread_count      = base::GetIntProperty("debug.ffmpeg_codec2.threads", 0);

    if (base::GetBoolProperty("debug.ffmpeg_codec2.fast", false)) {
        mCtx->flags2 |= AV_CODEC_FLAG2_FAST;
    }

    ffmpeg_hwaccel_init(mCtx);
    // HEVC through the V4L2 request hwaccel supports frame threads
    // (HWACCEL_CAP_THREAD_SAFE): with two, parsing and submitting the next
    // picture runs on FFmpeg's thread while this thread converts the last one
    // into the output buffer. ffmpeg_hwaccel_init forces one thread.
    if (mCtx->hw_device_ctx && mCtx->codec_id == AV_CODEC_ID_HEVC) {
        mCtx->thread_count = FFMPEG_HW_HEVC_THREADS;
        mCtx->thread_type = FF_THREAD_FRAME;
        bool zerocopy_prop = base::GetBoolProperty("persist.vendor.ffmpeg_codec2.zerocopy", true);
        SurfaceContext surfaceCtx;
        bool hasComposerSurface = false;
        if (mOutputPoolId != 0 && SurfaceRegistry::get().getSurface(mOutputPoolId, &surfaceCtx)) {
            if (surfaceCtx.producer &&
                (surfaceCtx.consumerUsage & GRALLOC_USAGE_HW_COMPOSER) &&
                !(surfaceCtx.consumerUsage & (GRALLOC_USAGE_SW_READ_MASK | GRALLOC_USAGE_SW_WRITE_MASK))) {
                hasComposerSurface = true;
            }
        }
        mCtx->extra_hw_frames = (zerocopy_prop && hasComposerSurface) ? 8 : 2;
        mHeldCap = mCtx->extra_hw_frames > 2 ? (mCtx->extra_hw_frames - 2) : 2;
        ALOGD("openDecoder: extra_hw_frames=%d cap=%zu zerocopy_candidate=%s",
              mCtx->extra_hw_frames, mHeldCap, hasComposerSurface ? "yes" : "no");
    }

    ALOGD("openDecoder: opening ffmpeg decoder(%s): threads = %d, hw = %s",
          avcodec_get_name(mCtx->codec_id), mCtx->thread_count, mCtx->hw_device_ctx ? "yes" : "no");

    int err = avcodec_open2(mCtx, mCtx->codec, NULL);
    if (err < 0) {
        ALOGE("openDecoder: ffmpeg video decoder failed to initialize. (%s)", av_err2str(err));
        return C2_NO_INIT;
    }
    mCodecAlreadyOpened = true;

    ALOGD("openDecoder: open ffmpeg video decoder(%s) success, caps = %08x",
          avcodec_get_name(mCtx->codec_id), mCtx->codec->capabilities);

    mFrame = av_frame_alloc();
    if (! mFrame) {
        ALOGE("openDecoder: oom for video frame");
        return C2_NO_MEMORY;
    }

    return C2_OK;
}

void C2FFMPEGVideoDecodeComponent::deInitDecoder() {
    ALOGD("%p deInitDecoder: %p", this, mCtx);
    destroyZeroCopyBuffers();
    // A hardware picture kept for the direct transfer holds one of the
    // decoder's buffers; drop it first so flushing and freeing the decoder
    // never wait on a buffer this component still owns.
    if (mFrame) {
        av_frame_unref(mFrame);
    }
    if (mCtx) {
        if (avcodec_is_open(mCtx)) {
            ALOGD("deInitDecoder: flush");
            avcodec_flush_buffers(mCtx);
        }
        ffmpeg_hwaccel_deinit(mCtx);
        ALOGD("deInitDecoder: free context");
        avcodec_free_context(&mCtx);
        ALOGD("deInitDecoder: context freed");
        mCodecAlreadyOpened = false;
    }
    if (mFrame) {
        av_frame_free(&mFrame);
        mFrame = NULL;
    }
    if (mPacket) {
        av_packet_free(&mPacket);
        mPacket = NULL;
    }
    if (mImgConvertCtx) {
        sws_freeContext(mImgConvertCtx);
        mImgConvertCtx = NULL;
    }
    mEOSSignalled = false;
    mExtradataReady = false;
    mPendingWorkQueue.clear();
    mPendingConfigUpdate.clear();
    mDirectTransferFailed = false;
}

c2_status_t C2FFMPEGVideoDecodeComponent::processCodecConfig(C2ReadView* inBuffer) {
    int orig_extradata_size = mCtx->extradata_size;
    int add_extradata_size = inBuffer->capacity();

#if DEBUG_EXTRADATA
    ALOGD("processCodecConfig: add = %u, current = %d", add_extradata_size, orig_extradata_size);
#endif
    if (! mExtradataReady) {
        mCtx->extradata_size += add_extradata_size;
        mCtx->extradata = (uint8_t *) realloc(mCtx->extradata, mCtx->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (! mCtx->extradata) {
            ALOGE("processCodecConfig: ffmpeg video decoder failed to alloc extradata memory.");
            return C2_NO_MEMORY;
        }
        memcpy(mCtx->extradata + orig_extradata_size, inBuffer->data(), add_extradata_size);
        memset(mCtx->extradata + mCtx->extradata_size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    }
    else {
        ALOGW("processCodecConfig: decoder is already opened, ignoring...");
    }

    return C2_OK;
}

c2_status_t C2FFMPEGVideoDecodeComponent::sendInputBuffer(
        C2ReadView *inBuffer, int64_t timestamp) {
    if (mZeroCopyMode && mOutputPoolId != 0) {
        int idleRounds = 0;
        while (true) {
            SurfaceContext surfaceCtx;
            bool hasSurface = SurfaceRegistry::get().getSurface(mOutputPoolId, &surfaceCtx);
            if (!hasSurface || !surfaceCtx.producer || !mZeroCopyMode) {
                break;
            }
            drainDetachNextBuffer(surfaceCtx.producer, surfaceCtx.syncMem);
            size_t held = getHeldFrameCount();
            if (held < mHeldCap) {
                break;
            }
            idleRounds++;
            usleep(idleRounds > 5 ? 20000 : 5000);
        }
    }
    if (!mPacket) {
        mPacket = av_packet_alloc();
        if (!mPacket) {
            ALOGE("sendInputBuffer: oom for video packet");
            return C2_NO_MEMORY;
        }
    }

    mPacket->data = inBuffer ? const_cast<uint8_t *>(inBuffer->data()) : NULL;
    mPacket->size = inBuffer ? inBuffer->capacity() : 0;
    mPacket->pts = timestamp;
    mPacket->dts = AV_NOPTS_VALUE;

    int err = avcodec_send_packet(mCtx, mPacket);
    av_packet_unref(mPacket);

    if (err < 0) {
        ALOGE("sendInputBuffer: failed to send data (%d) to decoder: %s (%08x)",
              inBuffer ? (int)inBuffer->capacity() : 0, av_err2str(err), err);
        if (err == AVERROR(EAGAIN)) {
            // Frames must be read first, notify main decoding loop.
            ALOGD("sendInputBuffer: returning C2_BAD_STATE");
            return C2_BAD_STATE;
        }
        if (err == AVERROR(ENOMEM)) {
            // The hardware decoder could not get picture memory (CMA). Report
            // it: a silently consumed packet leaves pending work that never
            // produces output, which grows the output delay and the number of
            // output buffers, using more of the same memory.
            return C2_NO_MEMORY;
        }
        // Otherwise don't send error to client.
    }

    return C2_OK;
}

c2_status_t C2FFMPEGVideoDecodeComponent::receiveFrame(bool* hasPicture) {
    int err = avcodec_receive_frame(mCtx, mFrame);

    *hasPicture = false;
    if (err == 0) {
        // Hardware pictures stay in hardware until getOutputBuffer, which
        // converts them straight into the output buffer.
        bool stayInHw = (mFrame->hw_frames_ctx && (mZeroCopyMode || isZeroCopyCandidate() || !mDirectTransferFailed));
        err = stayInHw ? 0 : ffmpeg_hwaccel_get_frame(mCtx, mFrame);
        if (err == 0) {
            *hasPicture = true;
        } else {
            ALOGE("receiveFrame: failed to receive frame from HW decoder err = %d", err);
            av_frame_unref(mFrame);
            if (err == AVERROR(ENOMEM)) {
                return C2_NO_MEMORY;
            }
            // Don't send error to client, skip frame!
        }
    } else if (err == AVERROR(EAGAIN)) {
        if (mZeroCopyMode && mCurrentProducer) {
            drainDetachNextBuffer(mCurrentProducer, mCurrentSyncMem);
        }
    } else if (err != AVERROR_EOF) {
        ALOGE("receiveFrame: failed to receive frame from decoder err = %d", err);
        // With frame threads a decode error surfaces here, not on send.
        if (err == AVERROR(ENOMEM)) {
            return C2_NO_MEMORY;
        }
        // Don't report error to client.
    }

    return C2_OK;
}

static void noFree(void* /* opaque */, uint8_t* /* data */) {}

c2_status_t C2FFMPEGVideoDecodeComponent::getOutputBuffer(C2GraphicView* outBuffer) {
    uint8_t* data[4];
    int linesize[4];
    C2PlanarLayout layout = outBuffer->layout();
    struct SwsContext* currentImgConvertCtx = mImgConvertCtx;

    data[0] = outBuffer->data()[C2PlanarLayout::PLANE_Y];
    data[1] = outBuffer->data()[C2PlanarLayout::PLANE_U];
    data[2] = outBuffer->data()[C2PlanarLayout::PLANE_V];
    linesize[0] = layout.planes[C2PlanarLayout::PLANE_Y].rowInc;
    linesize[1] = layout.planes[C2PlanarLayout::PLANE_U].rowInc;
    linesize[2] = layout.planes[C2PlanarLayout::PLANE_V].rowInc;

    if (mFrame->hw_frames_ctx) {
        // One pass from the decoder's picture into the planar output buffer,
        // instead of a copy to a temporary NV12 picture and sws_scale. Only
        // for planar layouts with equal chroma strides and a luma stride the
        // ROM's NEON 8-bit routine handles (width, or width rounded up to
        // 128); checked per picture, as the size can change mid-stream.
        const int w128 = (mFrame->width + 127) & ~127;
        const bool direct = layout.planes[C2PlanarLayout::PLANE_Y].colInc == 1 &&
                            layout.planes[C2PlanarLayout::PLANE_U].colInc == 1 &&
                            layout.planes[C2PlanarLayout::PLANE_V].colInc == 1 &&
                            linesize[1] == linesize[2] &&
                            (linesize[0] == mFrame->width || linesize[0] == w128);
        if (direct) {
            AVFrame* dst = av_frame_alloc();
            int err = AVERROR(ENOMEM);
            if (dst) {
                dst->format = AV_PIX_FMT_YUV420P;
                dst->width = mFrame->width;
                dst->height = mFrame->height;
                for (int i = 0; i < 3; i++) {
                    dst->data[i] = data[i];
                    dst->linesize[i] = linesize[i];
                }
                // Marks dst as allocated so the transfer writes into the view.
                dst->buf[0] = av_buffer_create(data[0], 1, noFree, NULL, 0);
                err = dst->buf[0] ? av_hwframe_transfer_data(dst, mFrame, 0) : AVERROR(ENOMEM);
                av_frame_free(&dst);
            }
            if (err == 0) {
                return C2_OK;
            }
            if (err == AVERROR(ENOMEM)) {
                ALOGE("getOutputBuffer: direct transfer out of memory");
                return C2_NO_MEMORY;
            }
            // An older libavutil without this conversion: use the
            // copy-and-convert path from now on.
            ALOGW("getOutputBuffer: direct transfer failed (%s), using sws_scale", av_err2str(err));
            mDirectTransferFailed = true;
        }
        int err = ffmpeg_hwaccel_get_frame(mCtx, mFrame);
        if (err != 0) {
            ALOGE("getOutputBuffer: failed to receive frame from HW decoder err = %d", err);
            if (err == AVERROR(ENOMEM)) {
                return C2_NO_MEMORY;
            }
            return C2_OMITTED; // skip the picture, as receiveFrame does
        }
    }

    mImgConvertCtx = sws_getCachedContext(currentImgConvertCtx,
           mFrame->width, mFrame->height, (AVPixelFormat)mFrame->format,
           mFrame->width, mFrame->height, AV_PIX_FMT_YUV420P,
           SWS_BICUBIC, NULL, NULL, NULL);
    if (mImgConvertCtx && mImgConvertCtx != currentImgConvertCtx) {
        ALOGD("getOutputBuffer: created video converter - %s => %s",
              av_get_pix_fmt_name((AVPixelFormat)mFrame->format), av_get_pix_fmt_name(AV_PIX_FMT_YUV420P));

    } else if (! mImgConvertCtx) {
        ALOGE("getOutputBuffer: cannot initialize the conversion context");
        return C2_NO_MEMORY;
    }

    sws_scale(mImgConvertCtx, mFrame->data, mFrame->linesize,
              0, mFrame->height, data, linesize);

    return C2_OK;
}

static void fillEmptyWork(const std::unique_ptr<C2Work>& work) {
    work->worklets.front()->output.flags =
        (C2FrameData::flags_t)(work->input.flags & C2FrameData::FLAG_END_OF_STREAM);
    work->worklets.front()->output.buffers.clear();
    work->worklets.front()->output.ordinal = work->input.ordinal;
    work->workletsProcessed = 1u;
    work->result = C2_OK;
#if DEBUG_WORKQUEUE
    ALOGD("WorkQueue: drop idx=%" PRIu64 ", ts=%" PRIu64,
          work->input.ordinal.frameIndex.peeku(), work->input.ordinal.timestamp.peeku());
#endif
}

static bool comparePendingWork(const PendingWork& w1, const PendingWork& w2) {
    return w1.second < w2.second;
}

void C2FFMPEGVideoDecodeComponent::pushPendingWork(const std::unique_ptr<C2Work>& work) {
    uint32_t outputDelay = mIntf->getOutputDelay();

    if (mPendingWorkQueue.size() >= outputDelay) {
        uint32_t newOutputDelay = outputDelay;
        std::vector<std::unique_ptr<C2Param>> configUpdate;

        switch (mCtx->codec_id) {
            case AV_CODEC_ID_HEVC:
            case AV_CODEC_ID_H264:
                // Increase output delay step-wise. Hardware HEVC output
                // buffers are 4K scanout buffers from the same CMA as the
                // decoder's; 34 of them (~400 MB) exhaust it, so it stops at 12.
                if (mCtx->hw_device_ctx && mCtx->codec_id == AV_CODEC_ID_HEVC) {
                    newOutputDelay = outputDelay >= 8u ? 12u : 8u;
                } else if (outputDelay >= 18u) {
                    newOutputDelay = 34u;
                } else if (outputDelay >= 8u) {
                    newOutputDelay = 18u;
                } else {
                    newOutputDelay = 8u;
                }
                break;
            default:
                // Other codecs use constant output delay.
                break;
        }

        if (newOutputDelay != outputDelay) {
            C2PortActualDelayTuning::output delay(newOutputDelay);
            std::vector<std::unique_ptr<C2SettingResult>> failures;
            int err;

            err = mIntf->config({ &delay }, C2_MAY_BLOCK, &failures);
            if (err == C2_OK) {
                ALOGD("WorkQueue: queue full, output delay set to %u", newOutputDelay);
                configUpdate.push_back(C2Param::Copy(delay));
            } else {
                ALOGE("WorkQueue: output delay update to %u failed err = %d",
                      newOutputDelay, err);
            }
        }

        auto fillEmptyWorkWithConfigUpdate = [&configUpdate](const std::unique_ptr<C2Work>& work) {
            fillEmptyWork(work);
            work->worklets.front()->output.configUpdate = std::move(configUpdate);
        };

        finish(mPendingWorkQueue.front().first, fillEmptyWorkWithConfigUpdate);
        mPendingWorkQueue.pop_front();
    }
#if DEBUG_WORKQUEUE
    ALOGD("WorkQueue: push idx=%" PRIu64 ", ts=%" PRIu64,
          work->input.ordinal.frameIndex.peeku(), work->input.ordinal.timestamp.peeku());
#endif
    mPendingWorkQueue.push_back(PendingWork(work->input.ordinal.frameIndex.peeku(),
                                            work->input.ordinal.timestamp.peeku()));
    std::sort(mPendingWorkQueue.begin(), mPendingWorkQueue.end(), comparePendingWork);
}

void C2FFMPEGVideoDecodeComponent::popPendingWork(const std::unique_ptr<C2Work>& work) {
    uint64_t index = work->input.ordinal.frameIndex.peeku();
    auto it = std::find_if(mPendingWorkQueue.begin(), mPendingWorkQueue.end(),
                           [index](const PendingWork& pWork) { return index == pWork.first; });

#if DEBUG_WORKQUEUE
    ALOGD("WorkQueue: pop idx=%" PRIu64 ", ts=%" PRIu64,
          work->input.ordinal.frameIndex.peeku(), work->input.ordinal.timestamp.peeku());
#endif

    if (it != mPendingWorkQueue.end()) {
        mPendingWorkQueue.erase(it);
    }
#if DEBUG_WORKQUEUE
    else {
        ALOGD("WorkQueue: pop work not found idx=%" PRIu64 ", ts=%" PRIu64,
              work->input.ordinal.frameIndex.peeku(), work->input.ordinal.timestamp.peeku());
    }
#endif
    prunePendingWorksUntil(work);
}

void C2FFMPEGVideoDecodeComponent::prunePendingWorksUntil(const std::unique_ptr<C2Work>& work) {
#if DEBUG_WORKQUEUE
    ALOGD("WorkQueue: prune until idx=%" PRIu64 ", ts=%" PRIu64,
          work->input.ordinal.frameIndex.peeku(), work->input.ordinal.timestamp.peeku());
#endif
    // Drop all works with a PTS earlier than provided argument.
    while (mPendingWorkQueue.size() > 0 &&
           mPendingWorkQueue.front().second < work->input.ordinal.timestamp.peeku()) {
        finish(mPendingWorkQueue.front().first, fillEmptyWork);
        mPendingWorkQueue.pop_front();
    }
}

c2_status_t C2FFMPEGVideoDecodeComponent::onInit() {
    ALOGD("onInit");
    return initDecoder();
}

c2_status_t C2FFMPEGVideoDecodeComponent::onStop() {
    ALOGD("onStop");
    clearZeroCopyBuffers();
    mZeroCopyMode = false;
    return C2_OK;
}

void C2FFMPEGVideoDecodeComponent::onReset() {
    ALOGD("onReset");
    deInitDecoder();
    initDecoder();
}

void C2FFMPEGVideoDecodeComponent::onRelease() {
    ALOGD("onRelease");
    deInitDecoder();
    if (mFFMPEGInitialized) {
        deInitFFmpeg();
        mFFMPEGInitialized = false;
    }
}

c2_status_t C2FFMPEGVideoDecodeComponent::onFlush_sm() {
    ALOGD("onFlush_sm");
    // On flush with the same producer, do NOT release buffers that are
    // AT_CLIENT_OR_BQ or DETACHED_FENCE_PENDING: they are still on screen
    // or in the BufferQueue and will return via detachNextBuffer / fence signal.
    // Only drop HELD_UNSENT frames that were never dispatched.
    for (size_t i = 0; i < mDrmBuffers.size(); ++i) {
        if (mDrmBuffers[i].state == DrmBufEntry::HELD_UNSENT && mDrmBuffers[i].heldFrame != nullptr) {
            av_frame_unref(mDrmBuffers[i].heldFrame);
            av_frame_free(&mDrmBuffers[i].heldFrame);
            mDrmBuffers[i].heldFrame = nullptr;
            mDrmBuffers[i].state = DrmBufEntry::FREE_IN_FFMPEG;
            mLiveHalRefs--;
        }
    }
    if (mFrame) {
        av_frame_unref(mFrame);
    }
    // The framework drops its pending work on flush; entries left here would
    // never be finished, and after a backward seek they sort after every new
    // timestamp, so the queue fills and new pictures get dropped.
    mPendingWorkQueue.clear();
    mPendingConfigUpdate.clear();
    if (mCtx && avcodec_is_open(mCtx)) {
        // Make sure that the next buffer output does not still
        // depend on fragments from the last one decoded.
        avcodec_flush_buffers(mCtx);
        mEOSSignalled = false;
    }
    return C2_OK;
}
size_t C2FFMPEGVideoDecodeComponent::getHeldFrameCount() const {
    size_t count = 0;
    for (const auto& b : mDrmBuffers) {
        if (b.heldFrame != nullptr) count++;
    }
    count += mQuarantine.size();
    return count;
}

bool C2FFMPEGVideoDecodeComponent::isZeroCopyCandidate() const {
    if (!base::GetBoolProperty("persist.vendor.ffmpeg_codec2.zerocopy", true)) return false;
    if (!mZeroCopySupported) return false;
    SurfaceContext surfaceCtx;
    if (mOutputPoolId != 0 && SurfaceRegistry::get().getSurface(mOutputPoolId, &surfaceCtx)) {
        if (surfaceCtx.producer &&
            (surfaceCtx.consumerUsage & GRALLOC_USAGE_HW_COMPOSER) &&
            !(surfaceCtx.consumerUsage & (GRALLOC_USAGE_SW_READ_MASK | GRALLOC_USAGE_SW_WRITE_MASK))) {
            return true;
        }
    }
    return false;
}

void C2FFMPEGVideoDecodeComponent::reclaimBufferById(uint32_t bufId, bool hasRealFence) {
    for (size_t i = 0; i < mDrmBuffers.size(); ++i) {
        if (mDrmBuffers[i].id == bufId) {
            if (mDrmBuffers[i].staleInstances > 0) {
                mDrmBuffers[i].staleInstances--;
                if (mDrmBuffers[i].heldFrame != nullptr) {
                    mQuarantine.onQuarantine(mDrmBuffers[i].heldFrame);
                    mDrmBuffers[i].heldFrame = nullptr;
                    mDrmBuffers[i].state = DrmBufEntry::FREE_IN_FFMPEG;
                    if (mDrmBuffers[i].currentSlot >= 0 && mDrmBuffers[i].currentSlot < 64) {
                        if (mSlotToBufIndex[mDrmBuffers[i].currentSlot] == (int)i) {
                            mSlotToBufIndex[mDrmBuffers[i].currentSlot] = -1;
                        }
                    }
                    mDrmBuffers[i].currentSlot = -1;
                    mQuarantinedCount++;
                    ALOGW("ZeroCopy: quarantined stale instance slot");
                }
                return;
            }
            if (mDrmBuffers[i].heldFrame != nullptr) {
                av_frame_unref(mDrmBuffers[i].heldFrame);
                av_frame_free(&mDrmBuffers[i].heldFrame);
                mDrmBuffers[i].heldFrame = nullptr;
                mLiveHalRefs--;
            }
            if (mDrmBuffers[i].currentSlot >= 0 && mDrmBuffers[i].currentSlot < 64) {
                if (mSlotToBufIndex[mDrmBuffers[i].currentSlot] == (int)i) {
                    mSlotToBufIndex[mDrmBuffers[i].currentSlot] = -1;
                }
            }
            mDrmBuffers[i].currentSlot = -1;
            mDrmBuffers[i].state = DrmBufEntry::FREE_IN_FFMPEG;
            mReclaimedCount++;
            mReclaimSeq++;
            mQuarantine.onReclaim(hasRealFence, /*signalled=*/true);
            break;
        }
    }
}

void C2FFMPEGVideoDecodeComponent::drainDetachNextBuffer(
        const sp<::android::hardware::graphics::bufferqueue::V2_0::IGraphicBufferProducer>& producer,
        const std::shared_ptr<C2SurfaceSyncMemory>& syncMem) {
    if (!producer) return;

    C2SyncVariables *syncVar = syncMem ? syncMem->mem() : nullptr;

    // Check pending fences first with non-blocking sync_wait(fd, 0)
    for (auto it = mPendingFences.begin(); it != mPendingFences.end(); ) {
        if (it->fenceFd < 0 || isFenceSignalled(it->fenceFd)) {
            if (it->fenceFd >= 0) close(it->fenceFd);
            reclaimBufferById(it->bufId, it->hasRealFence);
            it = mPendingFences.erase(it);
        } else {
            ++it;
        }
    }

    while (true) {
        ::android::hardware::graphics::bufferqueue::V2_0::Status detachStatus =
                ::android::hardware::graphics::bufferqueue::V2_0::Status::NO_MEMORY;
        ::android::hardware::graphics::common::V1_2::HardwareBuffer detachedBuffer{};
        ::android::hardware::hidl_handle detachedFence{};

        Return<void> trans = producer->detachNextBuffer(
                [&detachStatus, &detachedBuffer, &detachedFence](
                        ::android::hardware::graphics::bufferqueue::V2_0::Status s,
                        const ::android::hardware::graphics::common::V1_2::HardwareBuffer& b,
                        const ::android::hardware::hidl_handle& f) {
                    detachStatus = s;
                    detachedBuffer = b;
                    detachedFence = f;
                });

        if (!trans.isOk() || detachStatus == ::android::hardware::graphics::bufferqueue::V2_0::Status::NO_INIT) {
            ALOGW("ZeroCopy: producer gone (%d), leaving zero-copy", (int)detachStatus);
            mZeroCopyMode = false;
            clearZeroCopyBuffers();
            return;
        }
        if (detachStatus != ::android::hardware::graphics::bufferqueue::V2_0::Status::OK) {
            break;
        }

        const native_handle_t* nh = detachedBuffer.nativeHandle.getNativeHandle();
        if (nh != nullptr) {
            if (sizeof(native_handle_t) + (sizeof(int) * (nh->numFds + nh->numInts)) ==
                sizeof(struct cros_gralloc_handle)) {
                const struct cros_gralloc_handle* hnd =
                        reinterpret_cast<const struct cros_gralloc_handle*>(nh);
                if (hnd->magic == 0xABCDDCBA) {
                    uint32_t bufId = hnd->id;

                    const native_handle_t* fh = detachedFence.getNativeHandle();
                    int fenceFd = (fh != nullptr && fh->numFds > 0) ? fh->data[0] : -1;
                    bool hasRealFence = (fenceFd >= 0);

                    if (!hasRealFence || isFenceSignalled(fenceFd)) {
                        reclaimBufferById(bufId, hasRealFence);
                    } else {
                        for (size_t i = 0; i < mDrmBuffers.size(); ++i) {
                            if (mDrmBuffers[i].id == bufId) {
                                mDrmBuffers[i].state = DrmBufEntry::DETACHED_FENCE_PENDING;
                                if (mDrmBuffers[i].currentSlot >= 0 && mDrmBuffers[i].currentSlot < 64) {
                                    if (mSlotToBufIndex[mDrmBuffers[i].currentSlot] == (int)i) {
                                        mSlotToBufIndex[mDrmBuffers[i].currentSlot] = -1;
                                    }
                                }
                                mDrmBuffers[i].currentSlot = -1;
                                break;
                            }
                        }
                        mPendingFences.push_back({bufId, dup(fenceFd), true});
                    }
                }
            }
        }

        if (syncVar) {
            syncVar->lock();
            syncVar->notifyDequeuedLocked();
            syncVar->notifyQueuedLocked();
            syncVar->unlock();
        }
    }

    // Age out quarantined frames: release once at least 3 real signalled fence reclaims occurred
    std::vector<AVFrame*> releasable = mQuarantine.popReleasable(3);
    for (AVFrame* frame : releasable) {
        if (frame != nullptr) {
            av_frame_unref(frame);
            av_frame_free(&frame);
            mLiveHalRefs--;
        }
    }
}

void C2FFMPEGVideoDecodeComponent::clearZeroCopyBuffers() {
    for (size_t i = 0; i < mDrmBuffers.size(); ++i) {
        if (mDrmBuffers[i].state == DrmBufEntry::AT_CLIENT_OR_BQ) {
            mDrmBuffers[i].staleInstances++;
        }
        if (mDrmBuffers[i].heldFrame != nullptr) {
            av_frame_unref(mDrmBuffers[i].heldFrame);
            av_frame_free(&mDrmBuffers[i].heldFrame);
            mDrmBuffers[i].heldFrame = nullptr;
            mLiveHalRefs--;
        }
        mDrmBuffers[i].state = DrmBufEntry::FREE_IN_FFMPEG;
        mDrmBuffers[i].currentSlot = -1;
    }
    std::vector<AVFrame*> allQuarantined = mQuarantine.popAll();
    for (AVFrame* frame : allQuarantined) {
        if (frame != nullptr) {
            av_frame_unref(frame);
            av_frame_free(&frame);
            mLiveHalRefs--;
        }
    }
    for (auto& pf : mPendingFences) {
        if (pf.fenceFd >= 0) close(pf.fenceFd);
    }
    mPendingFences.clear();
    for (int i = 0; i < 64; ++i) {
        mSlotToBufIndex[i] = -1;
    }
    mCurrentProducer.clear();
    mCurrentSyncMem.reset();
}

void C2FFMPEGVideoDecodeComponent::destroyZeroCopyBuffers() {
    clearZeroCopyBuffers();
    for (size_t i = 0; i < mDrmBuffers.size(); ++i) {
        if (mDrmBuffers[i].cros_handle != nullptr) {
            if (mDrmBuffers[i].cros_handle->fds[0] >= 0) close(mDrmBuffers[i].cros_handle->fds[0]);
            if (mDrmBuffers[i].cros_handle->fds[1] >= 0) close(mDrmBuffers[i].cros_handle->fds[1]);
            if (mDrmBuffers[i].cros_handle->fds[2] >= 0) close(mDrmBuffers[i].cros_handle->fds[2]);
            free(mDrmBuffers[i].cros_handle);
            mDrmBuffers[i].cros_handle = nullptr;
        }
        if (mDrmBuffers[i].dma_buf_fd >= 0) {
            close(mDrmBuffers[i].dma_buf_fd);
            mDrmBuffers[i].dma_buf_fd = -1;
        }
        if (mDrmBuffers[i].meta_fd >= 0) {
            close(mDrmBuffers[i].meta_fd);
            mDrmBuffers[i].meta_fd = -1;
        }
    }
    mDrmBuffers.clear();
    mInoToBufIndex.clear();
}


c2_status_t C2FFMPEGVideoDecodeComponent::outputFrame(
    const std::unique_ptr<C2Work>& work,
    const std::shared_ptr<C2BlockPool> &pool
) {
    c2_status_t err;
    // A size change parked by a skipped picture goes out with this one.
    std::vector<std::unique_ptr<C2Param>> configUpdate = std::move(mPendingConfigUpdate);
    mPendingConfigUpdate.clear();
    uint64_t frameIndex = (mFrame->best_effort_timestamp != AV_NOPTS_VALUE)
            ? (uint64_t)mFrame->best_effort_timestamp
            : (work ? work->input.ordinal.frameIndex.peeku() : 0);


#if DEBUG_FRAMES
    ALOGD("outputFrame: pts=%" PRId64 " dts=%" PRId64 " ts=%" PRId64 " - %d x %d (%x)",
          mFrame->pts, mFrame->pkt_dts, mFrame->best_effort_timestamp, mFrame->width, mFrame->height, mFrame->format);
#endif

    if (mFrame->width != mIntf->getWidth() || mFrame->height != mIntf->getHeight()) {
        ALOGD("outputFrame: video params changed - %d x %d (%x)", mFrame->width, mFrame->height, mFrame->format);

        C2StreamPictureSizeInfo::output size(0u, mFrame->width, mFrame->height);
        std::vector<std::unique_ptr<C2SettingResult>> failures;

        err = mIntf->config({ &size }, C2_MAY_BLOCK, &failures);
        if (err == OK) {
            configUpdate.push_back(C2Param::Copy(size));
            mCtx->width = mFrame->width;
            mCtx->height = mFrame->height;
        } else {
            ALOGE("outputFrame: config update failed err = %d", err);
            return C2_CORRUPTED;
        }
    }

    // Color aspects mapping
    C2Color::range_t colorRange = C2Color::RANGE_UNSPECIFIED;
    if (mFrame->color_range == AVCOL_RANGE_JPEG) {
        colorRange = C2Color::RANGE_FULL;
    } else if (mFrame->color_range == AVCOL_RANGE_MPEG) {
        colorRange = C2Color::RANGE_LIMITED;
    }

    C2Color::primaries_t colorPrimaries = C2Color::PRIMARIES_UNSPECIFIED;
    switch (mFrame->color_primaries) {
        case AVCOL_PRI_BT709: colorPrimaries = C2Color::PRIMARIES_BT709; break;
        case AVCOL_PRI_BT2020: colorPrimaries = C2Color::PRIMARIES_BT2020; break;
        case AVCOL_PRI_BT470M: colorPrimaries = C2Color::PRIMARIES_BT470_M; break;
        case AVCOL_PRI_BT470BG: colorPrimaries = C2Color::PRIMARIES_BT601_625; break;
        case AVCOL_PRI_SMPTE170M: colorPrimaries = C2Color::PRIMARIES_BT601_525; break;
        case AVCOL_PRI_SMPTE240M: colorPrimaries = C2Color::PRIMARIES_BT601_525; break;
        default:
            colorPrimaries = (mFrame->width >= 3840) ? C2Color::PRIMARIES_BT2020 : C2Color::PRIMARIES_BT709;
            break;
    }

    C2Color::transfer_t colorTransfer = C2Color::TRANSFER_UNSPECIFIED;
    switch (mFrame->color_trc) {
        case AVCOL_TRC_BT709:
        case AVCOL_TRC_SMPTE170M:
            colorTransfer = C2Color::TRANSFER_170M;
            break;
        case AVCOL_TRC_SMPTE2084:
        case AVCOL_TRC_ARIB_STD_B67:
            // Display has no HDR; clamp to 170M in BOTH modes so SF composites DEVICE
            colorTransfer = C2Color::TRANSFER_170M;
            break;
        case AVCOL_TRC_LINEAR: colorTransfer = C2Color::TRANSFER_LINEAR; break;
        case AVCOL_TRC_GAMMA22: colorTransfer = C2Color::TRANSFER_GAMMA22; break;
        case AVCOL_TRC_GAMMA28: colorTransfer = C2Color::TRANSFER_GAMMA28; break;
        default:
            colorTransfer = C2Color::TRANSFER_170M;
            break;
    }

    C2Color::matrix_t colorMatrix = C2Color::MATRIX_UNSPECIFIED;
    switch (mFrame->colorspace) {
        case AVCOL_SPC_BT709: colorMatrix = C2Color::MATRIX_BT709; break;
        case AVCOL_SPC_BT2020_NCL:
        case AVCOL_SPC_BT2020_CL:
            colorMatrix = C2Color::MATRIX_BT2020;
            break;
        case AVCOL_SPC_BT470BG:
        case AVCOL_SPC_SMPTE170M:
            colorMatrix = C2Color::MATRIX_BT601;
            break;
        case AVCOL_SPC_SMPTE240M:
            colorMatrix = C2Color::MATRIX_240M;
            break;
        default:
            colorMatrix = (mFrame->width >= 3840) ? C2Color::MATRIX_BT2020 : C2Color::MATRIX_BT709;
            break;
    }

    SurfaceContext surfaceCtx;
    mOutputPoolId = pool->getLocalId();
    if (mFrame->hw_frames_ctx && mFrame->hw_frames_ctx->data != mLastHwFramesCtxData) {
        mLastHwFramesCtxData = mFrame->hw_frames_ctx->data;
        destroyZeroCopyBuffers();
    }
    bool hasSurface = SurfaceRegistry::get().getSurface(pool->getLocalId(), &surfaceCtx);

    if (hasSurface && (surfaceCtx.changeSeq != mLastProducerChangeSeq || surfaceCtx.generation != mLastGeneration)) {
        mLastProducerChangeSeq = surfaceCtx.changeSeq;
        mLastGeneration = surfaceCtx.generation;
        clearZeroCopyBuffers();
    }

    bool zeroCopyEligible = false;
    const char* reason = nullptr;

    bool zerocopy_prop = base::GetBoolProperty("persist.vendor.ffmpeg_codec2.zerocopy", true);
    if (!zerocopy_prop) {
        reason = "disabled_by_property";
    } else if (!mFrame->hw_frames_ctx) {
        reason = "not_hw_frames_ctx";
    } else if (mFrame->format != AV_PIX_FMT_DRM_PRIME) {
        reason = "not_drm_prime";
    } else if (pool->getAllocatorId() != C2PlatformAllocatorStore::BUFFERQUEUE) {
        reason = "not_bufferqueue_pool";
    } else if (!hasSurface || surfaceCtx.producer == nullptr) {
        reason = "no_surface_producer";
    } else if ((mIntf->getConsumerUsage() & (C2MemoryUsage::CPU_READ | C2MemoryUsage::CPU_WRITE)) != 0) {
        reason = "stream_usage_has_cpu_bits";
    } else if ((surfaceCtx.consumerUsage & (GRALLOC_USAGE_SW_READ_MASK | GRALLOC_USAGE_SW_WRITE_MASK)) != 0) {
        reason = "consumer_usage_has_cpu_bits";
    } else if ((surfaceCtx.consumerUsage & GRALLOC_USAGE_HW_COMPOSER) == 0) {
        reason = "consumer_not_hwcomposer";
    } else if (!mZeroCopySupported) {
        reason = "mapper_import_failed";
    } else {
        const AVDRMFrameDescriptor* desc = (const AVDRMFrameDescriptor*)mFrame->data[0];
        if (!desc || desc->nb_objects != 1 || desc->nb_layers != 1) {
            reason = "unsupported_drm_descriptor";
        } else {
            uint32_t drm_fmt = desc->layers[0].format;
            if (drm_fmt != DRM_FORMAT_NV12 && drm_fmt != DRM_FORMAT_P030) {
                reason = "unsupported_drm_format";
            } else if (fourcc_mod_broadcom_mod(desc->objects[0].format_modifier) != DRM_FORMAT_MOD_BROADCOM_SAND128) {
                reason = "not_sand128_modifier";
            } else {
                zeroCopyEligible = true;
            }
        }
    }

    if (zeroCopyEligible != mZeroCopyMode || surfaceCtx.changeSeq != mLastReportedChangeSeq) {
        mZeroCopyMode = zeroCopyEligible;
        mLastReportedChangeSeq = surfaceCtx.changeSeq;
        if (mZeroCopyMode) {
            ALOGD("ZeroCopy: mode=on reason=direct_display_attached");
        } else {
            ALOGD("ZeroCopy: mode=off reason=%s", reason ? reason : "unknown");
            clearZeroCopyBuffers();
        }
    }

    std::shared_ptr<C2GraphicBlock> block;
    std::shared_ptr<C2Buffer> buffer;

    if (mZeroCopyMode) {
        mCurrentProducer = surfaceCtx.producer;
        mCurrentSyncMem = surfaceCtx.syncMem;

        const AVDRMFrameDescriptor* desc = (const AVDRMFrameDescriptor*)mFrame->data[0];
        int dma_buf_fd = desc->objects[0].fd;
        struct stat st;
        if (fstat(dma_buf_fd, &st) != 0) {
            ALOGE("ZeroCopy: fstat failed on dma-buf fd %d: %s", dma_buf_fd, strerror(errno));
            av_frame_unref(mFrame);
            mPendingConfigUpdate = std::move(configUpdate);
            return C2_OK;
        }
        uint64_t ino = st.st_ino;

        size_t entryIdx = 0;
        auto it = mInoToBufIndex.find(ino);
        if (it == mInoToBufIndex.end()) {
            DrmBufEntry entry;
            static std::atomic<uint16_t> gNextBufId{1};
            uint16_t seq = gNextBufId.fetch_add(1);
            uint32_t pidPart = static_cast<uint32_t>(getpid() & 0x7fff) << 16;
            entry.id = 0x80000000u | pidPart | seq;
            entry.ino = ino;
            entry.dma_buf_fd = dup(dma_buf_fd);
            if (entry.dma_buf_fd < 0) {
                ALOGE("ZeroCopy: dup failed on dma-buf fd: %s", strerror(errno));
                av_frame_unref(mFrame);
                mPendingConfigUpdate = std::move(configUpdate);
                return C2_OK;
            }

            entry.meta_fd = memfd_create("minigbm_meta", MFD_CLOEXEC);
            if (entry.meta_fd < 0) {
                ALOGE("ZeroCopy: memfd_create failed: %s", strerror(errno));
                close(entry.dma_buf_fd);
                av_frame_unref(mFrame);
                mPendingConfigUpdate = std::move(configUpdate);
                return C2_OK;
            }
            if (ftruncate(entry.meta_fd, sizeof(struct cros_gralloc_buffer_metadata)) != 0) {
                ALOGE("ZeroCopy: ftruncate failed on meta_fd: %s", strerror(errno));
                close(entry.meta_fd);
                close(entry.dma_buf_fd);
                av_frame_unref(mFrame);
                mPendingConfigUpdate = std::move(configUpdate);
                return C2_OK;
            }
            struct cros_gralloc_buffer_metadata meta;
            memset(&meta, 0, sizeof(meta));
            if (write(entry.meta_fd, &meta, sizeof(meta)) < 0) {
                ALOGW("ZeroCopy: write meta failed: %s", strerror(errno));
            }

            const AVHWFramesContext *hwfc = mFrame->hw_frames_ctx ?
                    (const AVHWFramesContext*)mFrame->hw_frames_ctx->data : nullptr;
            uint32_t coded_width = hwfc ? hwfc->width : ((mFrame->width + 127) & ~127);
            uint32_t coded_height = hwfc ? hwfc->height : ((mFrame->height + 31) & ~31);

            uint32_t drm_format = desc->layers[0].format;
            bool is_p030 = (drm_format == DRM_FORMAT_P030);
            int32_t droid_format = is_p030 ? HAL_PIXEL_FORMAT_YCBCR_P010 : HAL_PIXEL_FORMAT_YCbCr_420_888;

            uint32_t pitch = desc->layers[0].planes[0].pitch;
            if (pitch == 0) {
                pitch = is_p030 ? (coded_width * 4 / 3) : coded_width;
            }
            uint32_t luma_col_size = (desc->layers[0].nb_planes > 1 && desc->layers[0].planes[1].offset > 0)
                    ? (uint32_t)desc->layers[0].planes[1].offset : (coded_height * 128);
            uint64_t total_size = desc->objects[0].size;
            if (total_size == 0) {
                total_size = luma_col_size * 3 / 2;
            }

            entry.cros_handle = (struct cros_gralloc_handle*)calloc(1, sizeof(struct cros_gralloc_handle));
            if (!entry.cros_handle) {
                ALOGE("ZeroCopy: calloc cros_gralloc_handle failed");
                close(entry.meta_fd);
                close(entry.dma_buf_fd);
                av_frame_unref(mFrame);
                mPendingConfigUpdate = std::move(configUpdate);
                return C2_OK;
            }
            entry.cros_handle->version = sizeof(native_handle_t);
            entry.cros_handle->numFds = 3;
            entry.cros_handle->numInts = ((sizeof(struct cros_gralloc_handle) - sizeof(native_handle_t)) / sizeof(int)) - entry.cros_handle->numFds;

            entry.cros_handle->fds[0] = dup(entry.dma_buf_fd);
            entry.cros_handle->fds[1] = dup(entry.dma_buf_fd);
            entry.cros_handle->fds[2] = dup(entry.meta_fd);
            for (int i = 3; i < DRV_MAX_FDS; i++) entry.cros_handle->fds[i] = -1;

            entry.cros_handle->strides[0] = pitch;
            entry.cros_handle->strides[1] = pitch;
            entry.cros_handle->offsets[0] = 0;
            entry.cros_handle->offsets[1] = luma_col_size;
            entry.cros_handle->sizes[0] = luma_col_size;
            entry.cros_handle->sizes[1] = (total_size > luma_col_size) ? (total_size - luma_col_size) : luma_col_size;
            entry.cros_handle->id = entry.id;
            entry.cros_handle->width = coded_width;
            entry.cros_handle->height = coded_height;
            entry.cros_handle->format = drm_format;
            entry.cros_handle->tiling = 0;
            entry.cros_handle->format_modifier = desc->objects[0].format_modifier;
            entry.cros_handle->use_flags = (1ull << 0) | (1ull << 13);
            entry.cros_handle->magic = 0xABCDDCBA;
            entry.cros_handle->pixel_stride = pitch;
            entry.cros_handle->droid_format = droid_format;
            entry.cros_handle->usage = GRALLOC_USAGE_HW_COMPOSER | GRALLOC_USAGE_HW_TEXTURE;
            entry.cros_handle->num_planes = 2;
            entry.cros_handle->reserved_region_size = sizeof(struct cros_gralloc_buffer_metadata);
            entry.cros_handle->total_size = total_size;

            sp<GraphicBuffer> gb = new GraphicBuffer(
                    reinterpret_cast<const native_handle_t*>(entry.cros_handle),
                    GraphicBuffer::HandleWrapMethod::CLONE_HANDLE,
                    coded_width, coded_height,
                    static_cast<PixelFormat>(droid_format),
                    1u,
                    static_cast<uint64_t>(entry.cros_handle->usage),
                    pitch);
            status_t import_err = gb->initCheck();
            if (import_err != NO_ERROR) {
                ALOGW("ZeroCopy: mapper self-import failed (%d), falling back to copy path", import_err);
                mZeroCopySupported = false;
                mZeroCopyMode = false;
                ALOGD("ZeroCopy: mode=off reason=mapper_import_failed");
                close(entry.cros_handle->fds[0]);
                close(entry.cros_handle->fds[1]);
                close(entry.cros_handle->fds[2]);
                free(entry.cros_handle);
                close(entry.meta_fd);
                close(entry.dma_buf_fd);
                clearZeroCopyBuffers();
                goto fallback_copy;
            }

            entryIdx = mDrmBuffers.size();
            mDrmBuffers.push_back(entry);
            mInoToBufIndex[ino] = entryIdx;
        } else {
            entryIdx = it->second;
        }

        DrmBufEntry &entry = mDrmBuffers[entryIdx];

        int idleRounds = 0;
        while (getHeldFrameCount() >= mHeldCap) {
            SurfaceContext curCtx;
            bool hasSurface = SurfaceRegistry::get().getSurface(mOutputPoolId, &curCtx);
            if (!hasSurface || !curCtx.producer || !mZeroCopyMode || curCtx.changeSeq != surfaceCtx.changeSeq) {
                ALOGW("ZeroCopy: surface changed or gone while waiting on attach cap");
                av_frame_unref(mFrame);
                mPendingConfigUpdate = std::move(configUpdate);
                return C2_OK;
            }
            drainDetachNextBuffer(curCtx.producer, curCtx.syncMem);
            if (getHeldFrameCount() < mHeldCap) {
                break;
            }
            idleRounds++;
            usleep(idleRounds > 5 ? 20000 : 5000);
        }

        AVFrame* held = av_frame_clone(mFrame);
        if (!held) {
            ALOGE("ZeroCopy: av_frame_clone failed");
            av_frame_unref(mFrame);
            return C2_NO_MEMORY;
        }
        mLiveHalRefs++;

        ::android::hardware::graphics::common::V1_2::HardwareBuffer hBuffer{};
        AHardwareBuffer_Desc* aDesc = reinterpret_cast<AHardwareBuffer_Desc*>(hBuffer.description.data());
        aDesc->width = entry.cros_handle->width;
        aDesc->height = entry.cros_handle->height;
        aDesc->layers = 1;
        aDesc->format = entry.cros_handle->droid_format;
        aDesc->usage = entry.cros_handle->usage;
        aDesc->stride = entry.cros_handle->pixel_stride;
        aDesc->rfu0 = 0;
        aDesc->rfu1 = 0;
        hBuffer.nativeHandle.setTo(reinterpret_cast<native_handle_t*>(entry.cros_handle), false);

        C2SyncVariables *syncVar = surfaceCtx.syncMem ? surfaceCtx.syncMem->mem() : nullptr;
        int32_t slot = -1;
        ::android::hardware::graphics::bufferqueue::V2_0::Status attachStatus =
                ::android::hardware::graphics::bufferqueue::V2_0::Status::NO_MEMORY;

        int retries = 0;
        while (retries < 50) {
            drainDetachNextBuffer(surfaceCtx.producer, surfaceCtx.syncMem);

            if (syncVar) {
                syncVar->lock();
                bool dequeueable = syncVar->isDequeueableLocked();
                syncVar->unlock();
                if (!dequeueable) {
                    usleep(10000);
                    retries++;
                    continue;
                }
            }

            Return<void> trans = surfaceCtx.producer->attachBuffer(
                    hBuffer, surfaceCtx.generation,
                    [&attachStatus, &slot](
                            ::android::hardware::graphics::bufferqueue::V2_0::Status s,
                            int32_t sSlot,
                            bool /* releaseAll */) {
                        attachStatus = s;
                        slot = sSlot;
                    });
            if (!trans.isOk() || attachStatus == ::android::hardware::graphics::bufferqueue::V2_0::Status::NO_INIT) {
                ALOGW("ZeroCopy: producer gone in attach (%d), leaving zero-copy", (int)attachStatus);
                mZeroCopyMode = false;
                clearZeroCopyBuffers();
                break;
            }
            if (attachStatus == ::android::hardware::graphics::bufferqueue::V2_0::Status::OK) {
                break;
            }
            usleep(10000);
            retries++;
        }

        if (attachStatus != ::android::hardware::graphics::bufferqueue::V2_0::Status::OK || slot < 0) {
            mAttachFailCount++;
            ALOGE("ZeroCopy: attachBuffer failed after %d retries, status=%d", retries, (int)attachStatus);
            av_frame_unref(held);
            av_frame_free(&held);
            mLiveHalRefs--;
            mPendingConfigUpdate = std::move(configUpdate);
            return C2_OK;
        }

        if (syncVar) {
            syncVar->lock();
            syncVar->notifyDequeuedLocked();
            syncVar->unlock();
        }

        if (slot >= 0 && slot < 64) {
            int prevIdx = mSlotToBufIndex[slot];
            if (prevIdx >= 0 && prevIdx < (int)mDrmBuffers.size()) {
                auto &prevEntry = mDrmBuffers[prevIdx];
                if (prevEntry.heldFrame != nullptr) {
                    mQuarantine.onQuarantine(prevEntry.heldFrame);
                    prevEntry.heldFrame = nullptr;
                    prevEntry.state = DrmBufEntry::FREE_IN_FFMPEG;
                    prevEntry.currentSlot = -1;
                    mQuarantinedCount++;
                    ALOGW("ZeroCopy: quarantined slot=%d", slot);
                }
            }
            mSlotToBufIndex[slot] = (int)entryIdx;
        }
        entry.currentSlot = slot;
        entry.heldFrame = held;
        entry.state = DrmBufEntry::AT_CLIENT_OR_BQ;

        C2Handle *c2Handle = android::WrapNativeCodec2GrallocHandle(
                reinterpret_cast<const native_handle_t*>(entry.cros_handle),
                entry.cros_handle->width,
                entry.cros_handle->height,
                entry.cros_handle->droid_format,
                entry.cros_handle->usage,
                entry.cros_handle->pixel_stride,
                surfaceCtx.generation,
                surfaceCtx.bqId,
                slot);
        if (!c2Handle) {
            ALOGE("ZeroCopy: WrapNativeCodec2GrallocHandle failed");
            surfaceCtx.producer->cancelBuffer(slot, ::android::hardware::hidl_handle());
            if (syncVar) {
                syncVar->lock();
                syncVar->notifyQueuedLocked();
                syncVar->unlock();
            }
            av_frame_unref(held);
            av_frame_free(&held);
            mLiveHalRefs--;
            entry.heldFrame = nullptr;
            entry.state = DrmBufEntry::FREE_IN_FFMPEG;
            entry.currentSlot = -1;
            mSlotToBufIndex[slot] = -1;
            mPendingConfigUpdate = std::move(configUpdate);
            return C2_OK;
        }
        std::shared_ptr<C2Allocator> grallocAllocator;
        err = GetCodec2PlatformAllocatorStore()->fetchAllocator(
                C2PlatformAllocatorStore::GRALLOC, &grallocAllocator);
        if (err != C2_OK || !grallocAllocator) {
            ALOGE("ZeroCopy: fetchAllocator GRALLOC failed: %d", err);
            native_handle_close(c2Handle);
            native_handle_delete(c2Handle);
            surfaceCtx.producer->cancelBuffer(slot, ::android::hardware::hidl_handle());
            if (syncVar) {
                syncVar->lock();
                syncVar->notifyQueuedLocked();
                syncVar->unlock();
            }
            av_frame_unref(held);
            av_frame_free(&held);
            mLiveHalRefs--;
            entry.heldFrame = nullptr;
            entry.state = DrmBufEntry::FREE_IN_FFMPEG;
            entry.currentSlot = -1;
            mSlotToBufIndex[slot] = -1;
            mPendingConfigUpdate = std::move(configUpdate);
            return C2_OK;
        }

        std::shared_ptr<C2GraphicAllocation> alloc;
        err = grallocAllocator->priorGraphicAllocation(c2Handle, &alloc);
        if (err != C2_OK) {
            ALOGE("ZeroCopy: priorGraphicAllocation failed: %d", err);
            native_handle_close(c2Handle);
            native_handle_delete(c2Handle);
            surfaceCtx.producer->cancelBuffer(slot, ::android::hardware::hidl_handle());
            if (syncVar) {
                syncVar->lock();
                syncVar->notifyQueuedLocked();
                syncVar->unlock();
            }
            av_frame_unref(held);
            av_frame_free(&held);
            mLiveHalRefs--;
            entry.heldFrame = nullptr;
            entry.state = DrmBufEntry::FREE_IN_FFMPEG;
            entry.currentSlot = -1;
            mSlotToBufIndex[slot] = -1;
            mPendingConfigUpdate = std::move(configUpdate);
            return C2_OK;
        }

        std::shared_ptr<C2BufferQueueBlockPoolData> poolData =
                std::make_shared<C2BufferQueueBlockPoolData>(
                        surfaceCtx.generation,
                        surfaceCtx.bqId,
                        slot,
                        surfaceCtx.ownerToken,
                        surfaceCtx.producer,
                        surfaceCtx.syncMem);

        block = _C2BlockFactory::CreateGraphicBlock(alloc, poolData);
        if (!block) {
            ALOGE("ZeroCopy: CreateGraphicBlock failed");
            surfaceCtx.producer->cancelBuffer(slot, ::android::hardware::hidl_handle());
            if (syncVar) {
                syncVar->lock();
                syncVar->notifyQueuedLocked();
                syncVar->unlock();
            }
            av_frame_unref(held);
            av_frame_free(&held);
            mLiveHalRefs--;
            entry.heldFrame = nullptr;
            entry.state = DrmBufEntry::FREE_IN_FFMPEG;
            entry.currentSlot = -1;
            mSlotToBufIndex[slot] = -1;
            mPendingConfigUpdate = std::move(configUpdate);
            return C2_OK;
        }

        uint32_t crop_l = mFrame->crop_left;
        uint32_t crop_t = mFrame->crop_top;
        uint32_t crop_w = (mFrame->crop_left + mFrame->crop_right < (uint32_t)mFrame->width) ?
                (mFrame->width - mFrame->crop_left - mFrame->crop_right) : mFrame->width;
        uint32_t crop_h = (mFrame->crop_top + mFrame->crop_bottom < (uint32_t)mFrame->height) ?
                (mFrame->height - mFrame->crop_top - mFrame->crop_bottom) : mFrame->height;
        C2Rect crop = C2Rect(crop_w, crop_h).at(crop_l, crop_t);
        buffer = createGraphicBuffer(std::move(block), crop);

        buffer->setInfo(std::make_shared<C2StreamPixelFormatInfo::output>(
                0u, entry.cros_handle->droid_format));
        buffer->setInfo(std::make_shared<C2StreamColorAspectsInfo::output>(
                0u, colorRange, colorPrimaries, colorTransfer, colorMatrix));

        int64_t nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        if (nowUs - mLastStatLogUs >= 1000000) {
            mLastStatLogUs = nowUs;
            size_t heldCount = 0;
            size_t inBQCount = 0;
            size_t freeCount = 0;
            for (const auto& b : mDrmBuffers) {
                if (b.heldFrame != nullptr) heldCount++;
                if (b.state == DrmBufEntry::AT_CLIENT_OR_BQ) inBQCount++;
                if (b.state == DrmBufEntry::FREE_IN_FFMPEG) freeCount++;
            }
            ALOGD("ZeroCopy: held=%zu inBQ=%zu free=%zu reclaimed=%zu attachFail=%zu quarantined=%zu pending=%zu cap=%zu refs=%d",
                  heldCount, inBQCount, freeCount, mReclaimedCount, mAttachFailCount, mQuarantine.size(),
                  mPendingFences.size(), mHeldCap, mLiveHalRefs.load());
        }

        mOutputCount++;
        if (mOutputCount <= 30) {
            ALOGD("ZeroCopy: [frame %u] refs=%d slot=%d id=0x%08x held=%zu",
                  mOutputCount, mLiveHalRefs.load(), slot, entry.id, getHeldFrameCount());
        }
    } else {
fallback_copy:
        err = pool->fetchGraphicBlock(mFrame->width, mFrame->height, HAL_PIXEL_FORMAT_YV12,
                                      { C2MemoryUsage::CPU_READ, C2MemoryUsage::CPU_WRITE }, &block);

        if (err != C2_OK) {
            ALOGE("outputFrame: failed to fetch graphic block %d x %d (%x) err = %d",
                  mFrame->width, mFrame->height, HAL_PIXEL_FORMAT_YV12, err);
            av_frame_unref(mFrame);
            return C2_CORRUPTED;
        }

        C2GraphicView wView = block->map().get();

        err = wView.error();
        if (err != C2_OK) {
            ALOGE("outputFrame: graphic view map failed err = %d", err);
            av_frame_unref(mFrame);
            return C2_CORRUPTED;
        }

        const uint32_t outW = mFrame->width;
        const uint32_t outH = mFrame->height;
        err = getOutputBuffer(&wView);
        if (mFrame->hw_frames_ctx) {
            av_frame_unref(mFrame);
        }
        if (err == C2_OMITTED) {
            mPendingConfigUpdate = std::move(configUpdate);
            return C2_OK;
        }
        if (err != C2_OK) {
            return err;
        }
        buffer = createGraphicBuffer(std::move(block), C2Rect(outW, outH));
        buffer->setInfo(mIntf->getPixelFormatInfo());
        buffer->setInfo(std::make_shared<C2StreamColorAspectsInfo::output>(
                0u, colorRange, colorPrimaries, colorTransfer, colorMatrix));
    }
    if (buffer) {
        if (work && c2_cntr64_t(frameIndex) == work->input.ordinal.frameIndex) {
            prunePendingWorksUntil(work);
            work->worklets.front()->output.configUpdate = std::move(configUpdate);
            work->worklets.front()->output.buffers.clear();
            work->worklets.front()->output.buffers.push_back(buffer);
            work->worklets.front()->output.ordinal = work->input.ordinal;
            work->workletsProcessed = 1u;
            work->result = C2_OK;
        } else {
            auto fillWork = [buffer, &configUpdate, this](const std::unique_ptr<C2Work>& work) {
                popPendingWork(work);
                work->worklets.front()->output.configUpdate = std::move(configUpdate);
                work->worklets.front()->output.flags = (C2FrameData::flags_t)0;
                work->worklets.front()->output.buffers.clear();
                work->worklets.front()->output.buffers.push_back(buffer);
                work->worklets.front()->output.ordinal = work->input.ordinal;
                work->workletsProcessed = 1u;
                work->result = C2_OK;
#if DEBUG_FRAMES
                ALOGD("outputFrame: work(finish) idx=%" PRIu64 ", processed=%u, result=%d",
                      work->input.ordinal.frameIndex.peeku(), work->workletsProcessed, work->result);
#endif
            };

            finish(frameIndex, fillWork);
        }
    } else {
        return C2_CORRUPTED;
    }
    if (mFrame) {
        av_frame_unref(mFrame);
    }
    return C2_OK;
}

void C2FFMPEGVideoDecodeComponent::process(
    const std::unique_ptr<C2Work> &work,
    const std::shared_ptr<C2BlockPool> &pool
) {
    mOutputPoolId = pool->getLocalId();
    size_t inSize = 0u;
    bool eos = (work->input.flags & C2FrameData::FLAG_END_OF_STREAM);
    C2ReadView rView = mDummyReadView;
    bool hasInputBuffer = false;

    if (! work->input.buffers.empty()) {
        rView = work->input.buffers[0]->data().linearBlocks().front().map().get();
        inSize = rView.capacity();
        hasInputBuffer = true;
    }

#if DEBUG_FRAMES
    ALOGD("process: input flags=%08x ts=%lu idx=%lu #buf=%lu[%lu] #conf=%lu #info=%lu",
          work->input.flags, work->input.ordinal.timestamp.peeku(), work->input.ordinal.frameIndex.peeku(),
          work->input.buffers.size(), inSize, work->input.configUpdate.size(), work->input.infoBuffers.size());
#endif

    if (mEOSSignalled) {
        ALOGE("process: ignoring work while EOS reached");
        work->workletsProcessed = 0u;
        work->result = C2_BAD_VALUE;
        return;
    }

    if (hasInputBuffer && rView.error()) {
        ALOGE("process: read view map failed err = %d", rView.error());
        work->workletsProcessed = 0u;
        work->result = rView.error();
        return;
    }

    // In all cases the work is marked as completed.
    //
    // There is not always a 1:1 mapping between input and output frames, in particular for
    // interlaced content. Keeping the corresponding worklets in the queue quickly fills it
    // in and stalls the decoder. But there's no obvious mechanism to determine, from
    // FFMPEG API, whether a given packet will produce an output frame and the worklet should
    // be kept around so it can be completed when the frame is produced.
    //
    // NOTE: This has an impact on the drain operation.

    work->result = C2_OK;
    work->worklets.front()->output.flags = (C2FrameData::flags_t)0;
    work->workletsProcessed = 0u;

    if (inSize || (eos && mCodecAlreadyOpened)) {
        c2_status_t err = C2_OK;

        if (work->input.flags & C2FrameData::FLAG_CODEC_CONFIG) {
            work->workletsProcessed = 1u;
            work->result = processCodecConfig(&rView);
            return;
        }

        if (! mCodecAlreadyOpened) {
            err = openDecoder();
            if (err != C2_OK) {
                work->workletsProcessed = 1u;
                work->result = err;
                return;
            }
        }

        bool inputConsumed = false;
        bool outputAvailable = true;
        bool hasPicture = false;
#if DEBUG_FRAMES
        int outputFrameCount = 0;
#endif

        while (!inputConsumed || outputAvailable) {
            if (!inputConsumed) {
                err = sendInputBuffer(&rView, work->input.ordinal.frameIndex.peekll());
                if (err == C2_OK) {
                    inputConsumed = true;
                    outputAvailable = true;
                    work->input.buffers.clear();
                } else if (err != C2_BAD_STATE) {
                    work->workletsProcessed = 1u;
                    work->result = err;
                    return;
                }
            }

            if (outputAvailable) {
                hasPicture = false;
                err = receiveFrame(&hasPicture);
                if (err != C2_OK) {
                    work->workletsProcessed = 1u;
                    work->result = err;
                    return;
                }

                if (hasPicture) {
                    err = outputFrame(work, pool);
                    if (err != C2_OK) {
                        work->workletsProcessed = 1u;
                        work->result = err;
                        return;
                    }
#if DEBUG_FRAMES
                    else {
                        outputFrameCount++;
                    }
#endif
                }
                else {
#if DEBUG_FRAMES
                    if (!outputFrameCount) {
                        ALOGD("process: no frame");
                    }
#endif
                    outputAvailable = false;
                }
            }
        }
    }
#if DEBUG_FRAMES
    else {
        ALOGD("process: empty work");
    }
#endif

    if (eos) {
        mEOSSignalled = true;
        work->worklets.front()->output.flags = C2FrameData::FLAG_END_OF_STREAM;
        work->workletsProcessed = 1u;
    }

    if (work->workletsProcessed == 0u) {
        pushPendingWork(work);
    }

#if DEBUG_FRAMES
    ALOGD("process: work(end) idx=%" PRIu64 ", processed=%u, result=%d",
          work->input.ordinal.frameIndex.peeku(), work->workletsProcessed, work->result);
#endif
}

c2_status_t C2FFMPEGVideoDecodeComponent::drain(
    uint32_t drainMode,
    const std::shared_ptr<C2BlockPool>& pool
) {
    ALOGD("drain: mode = %u", drainMode);

    if (drainMode == NO_DRAIN) {
        ALOGW("drain: NO_DRAIN is no-op");
        return C2_OK;
    }
    if (drainMode == DRAIN_CHAIN) {
        ALOGW("drain: DRAIN_CHAIN not supported");
        return C2_OMITTED;
    }
    if (! mCodecAlreadyOpened) {
        ALOGW("drain: codec not opened yet");
        return C2_OK;
    }

    bool hasPicture = false;
    c2_status_t err = C2_OK;

    err = sendInputBuffer(NULL, 0);
    while (err == C2_OK) {
        hasPicture = false;
        err = receiveFrame(&hasPicture);
        if (hasPicture) {
            // Ignore errors at this point, just drain the decoder.
            outputFrame(nullptr, pool);
        } else {
            err = C2_NOT_FOUND;
        }
    }

    return C2_OK;
}

} // namespace android
