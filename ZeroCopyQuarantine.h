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

#ifndef ZERO_COPY_QUARANTINE_H_
#define ZERO_COPY_QUARANTINE_H_

#include <cstdint>
#include <vector>
#include <algorithm>

namespace android {

template <typename FrameHandle = uint64_t>
class ZeroCopyQuarantine {
public:
    struct Entry {
        FrameHandle handle;
        uint64_t realFenceReclaimSeq;
    };

    ZeroCopyQuarantine() : mRealFenceReclaimSeq(0) {}

    // Record a quarantined frame
    void onQuarantine(FrameHandle handle) {
        mEntries.push_back({handle, mRealFenceReclaimSeq});
    }

    // Record a reclaim on the producer.
    // Only reclaims with a real, signalled fence advance the quarantine clock!
    void onReclaim(bool hasRealFence, bool signalled) {
        if (hasRealFence && signalled) {
            mRealFenceReclaimSeq++;
        }
    }

    // Returns all frame handles eligible for release (at least minRealFenceReclaims real,
    // signalled fence reclaims since quarantined).
    std::vector<FrameHandle> popReleasable(uint64_t minRealFenceReclaims = 3) {
        std::vector<FrameHandle> releasable;
        auto it = std::remove_if(mEntries.begin(), mEntries.end(),
            [this, minRealFenceReclaims, &releasable](const Entry& e) {
                if (mRealFenceReclaimSeq >= e.realFenceReclaimSeq + minRealFenceReclaims) {
                    releasable.push_back(e.handle);
                    return true;
                }
                return false;
            });
        mEntries.erase(it, mEntries.end());
        return releasable;
    }

    // On producer change / flush / teardown: pop everything remaining
    std::vector<FrameHandle> popAll() {
        std::vector<FrameHandle> all;
        all.reserve(mEntries.size());
        for (const auto& e : mEntries) {
            all.push_back(e.handle);
        }
        mEntries.clear();
        mRealFenceReclaimSeq = 0;
        return all;
    }

    size_t size() const {
        return mEntries.size();
    }

    bool empty() const {
        return mEntries.empty();
    }

    uint64_t currentRealFenceReclaimSeq() const {
        return mRealFenceReclaimSeq;
    }

private:
    uint64_t mRealFenceReclaimSeq;
    std::vector<Entry> mEntries;
};

} // namespace android

#endif // ZERO_COPY_QUARANTINE_H_
