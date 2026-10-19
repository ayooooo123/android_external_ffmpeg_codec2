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

#ifndef C2_FFMPEG_ZERO_COPY_REGISTRY_H_
#define C2_FFMPEG_ZERO_COPY_REGISTRY_H_

#include <android/hardware/graphics/bufferqueue/2.0/IGraphicBufferProducer.h>
#include <C2SurfaceSyncObj.h>
#include <utils/RefBase.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace android {

struct SurfaceContext {
    sp<::android::hardware::graphics::bufferqueue::V2_0::IGraphicBufferProducer> producer;
    std::shared_ptr<C2SurfaceSyncMemory> syncMem;
    uint64_t bqId = 0;
    uint32_t generation = 0;
    uint64_t consumerUsage = 0;
    std::shared_ptr<int> ownerToken = std::make_shared<int>(0);
    uint32_t changeSeq = 0;
};

class SurfaceRegistry {
public:
    static SurfaceRegistry& get();

    void registerSurface(
            uint64_t blockPoolId,
            const sp<::android::hardware::graphics::bufferqueue::V2_0::IGraphicBufferProducer>& surface,
            const std::shared_ptr<C2SurfaceSyncMemory>& syncMem,
            uint64_t bqId,
            uint32_t generation,
            uint64_t consumerUsage);

    void unregisterSurface(uint64_t blockPoolId);

    bool getSurface(uint64_t blockPoolId, SurfaceContext* outContext);


private:
    SurfaceRegistry() = default;
    ~SurfaceRegistry() = default;
    SurfaceRegistry(const SurfaceRegistry&) = delete;
    SurfaceRegistry& operator=(const SurfaceRegistry&) = delete;

    std::mutex mLock;
    uint32_t mNextChangeSeq = 1;
    std::unordered_map<uint64_t, SurfaceContext> mSurfaces;
};

} // namespace android

#endif // C2_FFMPEG_ZERO_COPY_REGISTRY_H_
