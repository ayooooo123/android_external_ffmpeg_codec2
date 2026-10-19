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

#ifndef ZERO_COPY_HELD_SET_H_
#define ZERO_COPY_HELD_SET_H_

#include <cstdint>
#include <vector>
#include <unordered_map>
#include <algorithm>

namespace android {

class ZeroCopyHeldSet {
public:
    enum State {
        FREE = 0,
        AT_CLIENT_OR_BQ = 1,
        DETACHED_FENCE_PENDING = 2,
    };

    struct Entry {
        uint32_t bufId = 0;
        State state = FREE;
        bool hasRealFence = false;
    };

    ZeroCopyHeldSet() = default;

    void onAttach(uint32_t bufId) {
        mBuffers[bufId] = {bufId, AT_CLIENT_OR_BQ, false};
    }

    // Called when detachNextBuffer returns the buffer
    void onDetach(uint32_t bufId, bool hasRealFence, bool isSignalled) {
        auto it = mBuffers.find(bufId);
        if (it == mBuffers.end() || it->second.state == FREE) {
            return;
        }
        if (hasRealFence && !isSignalled) {
            it->second.state = DETACHED_FENCE_PENDING;
            it->second.hasRealFence = true;
        } else {
            it->second.state = FREE;
        }
    }

    // Called when a pending fence signals
    void onFenceSignalled(uint32_t bufId) {
        auto it = mBuffers.find(bufId);
        if (it != mBuffers.end() && it->second.state == DETACHED_FENCE_PENDING) {
            it->second.state = FREE;
        }
    }

    // On flush: preserve AT_CLIENT_OR_BQ and DETACHED_FENCE_PENDING
    void onFlush() {
        // In-flight buffers and pending fences remain pinned across flush
    }

    // On producer change: release all
    void onProducerChange() {
        mBuffers.clear();
    }

    // On stop: release all
    void onStop() {
        mBuffers.clear();
    }

    bool isPinned(uint32_t bufId) const {
        auto it = mBuffers.find(bufId);
        if (it == mBuffers.end()) return false;
        return it->second.state != FREE;
    }

    size_t getHeldCount() const {
        size_t count = 0;
        for (const auto& pair : mBuffers) {
            if (pair.second.state != FREE) {
                count++;
            }
        }
        return count;
    }

private:
    std::unordered_map<uint32_t, Entry> mBuffers;
};

} // namespace android

#endif // ZERO_COPY_HELD_SET_H_
