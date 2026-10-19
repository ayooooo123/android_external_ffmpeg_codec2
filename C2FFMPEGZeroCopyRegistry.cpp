/*
 * Copyright (C) 2026 The Android Open Source Project
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

#define LOG_TAG "C2FFMPEGZeroCopyRegistry"

#include "C2FFMPEGZeroCopyRegistry.h"
#include <log/log.h>
#include <inttypes.h>

namespace android {

SurfaceRegistry& SurfaceRegistry::get() {
    static SurfaceRegistry instance;
    return instance;
}

void SurfaceRegistry::registerSurface(
        uint64_t blockPoolId,
        const sp<::android::hardware::graphics::bufferqueue::V2_0::IGraphicBufferProducer>& surface,
        const std::shared_ptr<C2SurfaceSyncMemory>& syncMem,
        uint64_t bqId,
        uint32_t generation,
        uint64_t consumerUsage) {
    std::lock_guard<std::mutex> lock(mLock);
    SurfaceContext ctx;
    ctx.producer = surface;
    ctx.syncMem = syncMem;
    ctx.bqId = bqId;
    ctx.generation = generation;
    ctx.consumerUsage = consumerUsage;
    ctx.ownerToken = std::make_shared<int>(0);
    ctx.changeSeq = mNextChangeSeq++;
    mSurfaces[blockPoolId] = ctx;
    ALOGD("ZeroCopyRegistry: registered pool=%" PRIu64 " bqId=%" PRIu64 " gen=%u usage=0x%" PRIx64 " seq=%u",
          blockPoolId, bqId, generation, consumerUsage, ctx.changeSeq);
}

void SurfaceRegistry::unregisterSurface(uint64_t blockPoolId) {
    std::lock_guard<std::mutex> lock(mLock);
    auto it = mSurfaces.find(blockPoolId);
    if (it != mSurfaces.end()) {
        ALOGD("ZeroCopyRegistry: unregistered pool=%" PRIu64, blockPoolId);
        mSurfaces.erase(it);
    }
}

bool SurfaceRegistry::getSurface(uint64_t blockPoolId, SurfaceContext* outContext) {
    std::lock_guard<std::mutex> lock(mLock);
    auto it = mSurfaces.find(blockPoolId);
    if (it != mSurfaces.end()) {
        if (outContext) *outContext = it->second;
        return true;
    }
    return false;
}


} // namespace android
