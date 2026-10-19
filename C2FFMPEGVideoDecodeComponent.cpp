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

#define DEBUG_FRAMES 0
#ifndef FFMPEG_HW_HEVC_THREADS
#define FFMPEG_HW_HEVC_THREADS 2
#endif
#define DEBUG_WORKQUEUE 0
#define DEBUG_EXTRADATA 0

namespace android {

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
      mDirectTransferFailed(false) {
    ALOGD("C2FFMPEGVideoDecodeComponent: mediaType = %s", componentInfo->mediaType);
}

C2FFMPEGVideoDecodeComponent::~C2FFMPEGVideoDecodeComponent() {
    ALOGD("~C2FFMPEGVideoDecodeComponent: mCtx = %p", mCtx);
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
        // Hardware pictures are converted into the output buffer and released
        // at once, so 2 spare decoder buffers are enough (FFmpeg's default is
        // 6). At 4K 10-bit each is ~16.5 MB of the Pi's 512 MB CMA.
        mCtx->extra_hw_frames = 2;
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
        err = (mFrame->hw_frames_ctx && !mDirectTransferFailed) ? 0 : ffmpeg_hwaccel_get_frame(mCtx, mFrame);
        if (err == 0) {
            *hasPicture = true;
        } else {
            ALOGE("receiveFrame: failed to receive frame from HW decoder err = %d", err);
            if (err == AVERROR(ENOMEM)) {
                return C2_NO_MEMORY;
            }
            // Don't send error to client, skip frame!
        }
    } else if (err != AVERROR(EAGAIN) && err != AVERROR_EOF) {
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
    if (mFrame) {
        av_frame_unref(mFrame);  // see deInitDecoder
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

c2_status_t C2FFMPEGVideoDecodeComponent::outputFrame(
    const std::unique_ptr<C2Work>& work,
    const std::shared_ptr<C2BlockPool> &pool
) {
    c2_status_t err;
    // A size change parked by a skipped picture goes out with this one.
    std::vector<std::unique_ptr<C2Param>> configUpdate = std::move(mPendingConfigUpdate);
    mPendingConfigUpdate.clear();

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

    std::shared_ptr<C2GraphicBlock> block;

    err = pool->fetchGraphicBlock(mFrame->width, mFrame->height, HAL_PIXEL_FORMAT_YV12,
                                  { C2MemoryUsage::CPU_READ, C2MemoryUsage::CPU_WRITE }, &block);

    if (err != C2_OK) {
        ALOGE("outputFrame: failed to fetch graphic block %d x %d (%x) err = %d",
              mFrame->width, mFrame->height, HAL_PIXEL_FORMAT_YV12, err);
        return C2_CORRUPTED;
    }

    C2GraphicView wView = block->map().get();

    err = wView.error();
    if (err != C2_OK) {
        ALOGE("outputFrame: graphic view map failed err = %d", err);
        return C2_CORRUPTED;
    }

    err = getOutputBuffer(&wView);
    if (err == C2_OMITTED) {
        // Picture lost in the hardware transfer: skip it, as before, and keep
        // any size change for the next picture.
        mPendingConfigUpdate = std::move(configUpdate);
        return C2_OK;
    }
    if (err == C2_OK) {
        std::shared_ptr<C2Buffer> buffer = createGraphicBuffer(std::move(block), C2Rect(mFrame->width, mFrame->height));

        buffer->setInfo(mIntf->getPixelFormatInfo());

        if (work && c2_cntr64_t(mFrame->best_effort_timestamp) == work->input.ordinal.frameIndex) {
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

            finish(mFrame->best_effort_timestamp, fillWork);
        }
    } else {
        return err;
    }

    return C2_OK;
}

void C2FFMPEGVideoDecodeComponent::process(
    const std::unique_ptr<C2Work> &work,
    const std::shared_ptr<C2BlockPool> &pool
) {
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
