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

#include <gtest/gtest.h>
#include "ZeroCopyQuarantine.h"
#include "ZeroCopyHeldSet.h"

using namespace android;

// Test (a): one quarantined frame + three reclaims with empty fences -> still pinned
TEST(ZeroCopyQuarantineTest, EmptyFencesDoNotAdvanceClock) {
    ZeroCopyQuarantine<uint64_t> q;
    uint64_t frame1 = 1001;

    q.onQuarantine(frame1);
    EXPECT_EQ(q.size(), 1u);

    // 3 reclaims with empty fence (hasRealFence = false)
    q.onReclaim(false, false);
    q.onReclaim(false, true);
    q.onReclaim(false, true);

    auto releasable = q.popReleasable(3);
    EXPECT_TRUE(releasable.empty());
    EXPECT_EQ(q.size(), 1u);
}

// Test (b): one quarantined frame + one unsignalled real fence + three empty-fence reclaims -> still pinned
TEST(ZeroCopyQuarantineTest, UnsignalledRealFenceDoesNotAdvanceClock) {
    ZeroCopyQuarantine<uint64_t> q;
    uint64_t frame1 = 1002;

    q.onQuarantine(frame1);

    // 1 reclaim with real fence but not signalled
    q.onReclaim(true, false);

    // 3 reclaims with empty fences
    q.onReclaim(false, true);
    q.onReclaim(false, true);
    q.onReclaim(false, true);

    auto releasable = q.popReleasable(3);
    EXPECT_TRUE(releasable.empty());
    EXPECT_EQ(q.size(), 1u);
}

// Test (c): three signalled real-fence reclaims -> releasable
TEST(ZeroCopyQuarantineTest, ThreeSignalledRealFencesReleaseFrame) {
    ZeroCopyQuarantine<uint64_t> q;
    uint64_t frame1 = 1003;

    q.onQuarantine(frame1);

    // 2 signalled real fences
    q.onReclaim(true, true);
    q.onReclaim(true, true);
    EXPECT_TRUE(q.popReleasable(3).empty());
    EXPECT_EQ(q.size(), 1u);

    // 3rd signalled real fence
    q.onReclaim(true, true);

    auto releasable = q.popReleasable(3);
    ASSERT_EQ(releasable.size(), 1u);
    EXPECT_EQ(releasable[0], frame1);
    EXPECT_EQ(q.size(), 0u);
}

// Test (d): producer change -> everything released by caller's clear path (popAll)
TEST(ZeroCopyQuarantineTest, ProducerChangeReleasesAll) {
    ZeroCopyQuarantine<uint64_t> q;
    uint64_t frame1 = 1004;
    uint64_t frame2 = 1005;

    q.onQuarantine(frame1);
    q.onReclaim(true, true); // 1 real reclaim
    q.onQuarantine(frame2);

    EXPECT_EQ(q.size(), 2u);

    auto all = q.popAll();
    ASSERT_EQ(all.size(), 2u);
    EXPECT_EQ(all[0], frame1);
    EXPECT_EQ(all[1], frame2);
    EXPECT_EQ(q.size(), 0u);
    EXPECT_EQ(q.currentRealFenceReclaimSeq(), 0u);
}

// Test (e): an entry with an unsignalled pending fence stays pinned across onFlush and is released only after the fence signals
TEST(ZeroCopyHeldSetTest, PendingFenceStaysPinnedAcrossFlushUntilSignalled) {
    ZeroCopyHeldSet heldSet;
    uint32_t bufId = 2001;

    heldSet.onAttach(bufId);
    EXPECT_TRUE(heldSet.isPinned(bufId));

    // Detached with unsignalled real fence -> moves to DETACHED_FENCE_PENDING
    heldSet.onDetach(bufId, /*hasRealFence=*/true, /*isSignalled=*/false);
    EXPECT_TRUE(heldSet.isPinned(bufId));

    // Flush occurs -> MUST STAY PINNED
    heldSet.onFlush();
    EXPECT_TRUE(heldSet.isPinned(bufId));
    EXPECT_EQ(heldSet.getHeldCount(), 1u);

    // Fence signals -> released
    heldSet.onFenceSignalled(bufId);
    EXPECT_FALSE(heldSet.isPinned(bufId));
    EXPECT_EQ(heldSet.getHeldCount(), 0u);
}

// Test (f): an AT_CLIENT_OR_BQ entry stays pinned across onFlush
TEST(ZeroCopyHeldSetTest, InFlightBufferStaysPinnedAcrossFlush) {
    ZeroCopyHeldSet heldSet;
    uint32_t bufId = 2002;

    heldSet.onAttach(bufId);
    EXPECT_TRUE(heldSet.isPinned(bufId));

    // Flush occurs -> MUST STAY PINNED
    heldSet.onFlush();
    EXPECT_TRUE(heldSet.isPinned(bufId));
    EXPECT_EQ(heldSet.getHeldCount(), 1u);

    // Later returns via detachNextBuffer with signalled fence
    heldSet.onDetach(bufId, /*hasRealFence=*/true, /*isSignalled=*/true);
    EXPECT_FALSE(heldSet.isPinned(bufId));
    EXPECT_EQ(heldSet.getHeldCount(), 0u);
}

// Test (g): onProducerChange / onStop release all
TEST(ZeroCopyHeldSetTest, ProducerChangeAndStopReleaseAll) {
    ZeroCopyHeldSet heldSet;
    heldSet.onAttach(2003);
    heldSet.onAttach(2004);
    heldSet.onDetach(2004, /*hasRealFence=*/true, /*isSignalled=*/false); // pending fence
    EXPECT_EQ(heldSet.getHeldCount(), 2u);

    // Producer change -> releases all
    heldSet.onProducerChange();
    EXPECT_EQ(heldSet.getHeldCount(), 0u);
    EXPECT_FALSE(heldSet.isPinned(2003));
    EXPECT_FALSE(heldSet.isPinned(2004));

    // Attach again and verify onStop releases all
    heldSet.onAttach(2005);
    EXPECT_EQ(heldSet.getHeldCount(), 1u);
    heldSet.onStop();
    EXPECT_EQ(heldSet.getHeldCount(), 0u);
    EXPECT_FALSE(heldSet.isPinned(2005));
}
