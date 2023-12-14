/*
 * Copyright 2023 The Android Open Source Project
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

#undef LOG_TAG
#define LOG_TAG "LibSurfaceFlingerUnittests"

#include "DisplayTransactionTestHelpers.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace android {
namespace {

struct FoldableTest : DisplayTransactionTest {
    static constexpr bool kWithMockScheduler = false;
    FoldableTest() : DisplayTransactionTest(kWithMockScheduler) {}

    void SetUp() override {
        injectMockScheduler(kInnerDisplayId);

        // Inject inner and outer displays with uninitialized power modes.
        constexpr bool kInitPowerMode = false;
        {
            InnerDisplayVariant::injectHwcDisplay<kInitPowerMode>(this);
            auto injector = InnerDisplayVariant::makeFakeExistingDisplayInjector(this);
            injector.setPowerMode(std::nullopt);
            injector.setRefreshRateSelector(mFlinger.scheduler()->refreshRateSelector());
            mInnerDisplay = injector.inject();
        }
        {
            OuterDisplayVariant::injectHwcDisplay<kInitPowerMode>(this);
            auto injector = OuterDisplayVariant::makeFakeExistingDisplayInjector(this);
            injector.setPowerMode(std::nullopt);
            mOuterDisplay = injector.inject();
        }
    }

    static inline PhysicalDisplayId kInnerDisplayId = InnerDisplayVariant::DISPLAY_ID::get();
    static inline PhysicalDisplayId kOuterDisplayId = OuterDisplayVariant::DISPLAY_ID::get();

    sp<DisplayDevice> mInnerDisplay, mOuterDisplay;
};

TEST_F(FoldableTest, foldUnfold) {
    // When the device boots, the inner display should be the pacesetter.
    ASSERT_EQ(mFlinger.scheduler()->pacesetterDisplayId(), kInnerDisplayId);

    // ...and should still be after powering on.
    mFlinger.setPowerModeInternal(mInnerDisplay, PowerMode::ON);
    ASSERT_EQ(mFlinger.scheduler()->pacesetterDisplayId(), kInnerDisplayId);

    // The outer display should become the pacesetter after folding.
    mFlinger.setPowerModeInternal(mInnerDisplay, PowerMode::OFF);
    mFlinger.setPowerModeInternal(mOuterDisplay, PowerMode::ON);
    ASSERT_EQ(mFlinger.scheduler()->pacesetterDisplayId(), kOuterDisplayId);

    // The inner display should become the pacesetter after unfolding.
    mFlinger.setPowerModeInternal(mOuterDisplay, PowerMode::OFF);
    mFlinger.setPowerModeInternal(mInnerDisplay, PowerMode::ON);
    ASSERT_EQ(mFlinger.scheduler()->pacesetterDisplayId(), kInnerDisplayId);

    // The inner display should stay the pacesetter if both are powered on.
    // TODO(b/255635821): The pacesetter should depend on the displays' refresh rates.
    mFlinger.setPowerModeInternal(mOuterDisplay, PowerMode::ON);
    ASSERT_EQ(mFlinger.scheduler()->pacesetterDisplayId(), kInnerDisplayId);

    // The outer display should become the pacesetter if designated.
    mFlinger.scheduler()->setPacesetterDisplay(kOuterDisplayId);
    ASSERT_EQ(mFlinger.scheduler()->pacesetterDisplayId(), kOuterDisplayId);
}

TEST_F(FoldableTest, doesNotRequestHardwareVsyncIfPoweredOff) {
    // Both displays are powered off.
    EXPECT_CALL(mFlinger.mockSchedulerCallback(), requestHardwareVsync(kInnerDisplayId, _))
            .Times(0);
    EXPECT_CALL(mFlinger.mockSchedulerCallback(), requestHardwareVsync(kOuterDisplayId, _))
            .Times(0);

    EXPECT_FALSE(mInnerDisplay->isPoweredOn());
    EXPECT_FALSE(mOuterDisplay->isPoweredOn());

    auto& scheduler = *mFlinger.scheduler();
    scheduler.onHardwareVsyncRequest(kInnerDisplayId, true);
    scheduler.onHardwareVsyncRequest(kOuterDisplayId, true);
}

TEST_F(FoldableTest, requestsHardwareVsyncForInnerDisplay) {
    // Only inner display is powered on.
    EXPECT_CALL(mFlinger.mockSchedulerCallback(), requestHardwareVsync(kInnerDisplayId, true))
            .Times(1);
    EXPECT_CALL(mFlinger.mockSchedulerCallback(), requestHardwareVsync(kOuterDisplayId, _))
            .Times(0);

    // The injected VsyncSchedule uses TestableScheduler::mockRequestHardwareVsync, so no calls to
    // ISchedulerCallback::requestHardwareVsync are expected during setPowerModeInternal.
    mFlinger.setPowerModeInternal(mInnerDisplay, PowerMode::ON);

    EXPECT_TRUE(mInnerDisplay->isPoweredOn());
    EXPECT_FALSE(mOuterDisplay->isPoweredOn());

    auto& scheduler = *mFlinger.scheduler();
    scheduler.onHardwareVsyncRequest(kInnerDisplayId, true);
    scheduler.onHardwareVsyncRequest(kOuterDisplayId, true);
}

TEST_F(FoldableTest, requestsHardwareVsyncForOuterDisplay) {
    // Only outer display is powered on.
    EXPECT_CALL(mFlinger.mockSchedulerCallback(), requestHardwareVsync(kInnerDisplayId, _))
            .Times(0);
    EXPECT_CALL(mFlinger.mockSchedulerCallback(), requestHardwareVsync(kOuterDisplayId, true))
            .Times(1);

    // The injected VsyncSchedule uses TestableScheduler::mockRequestHardwareVsync, so no calls to
    // ISchedulerCallback::requestHardwareVsync are expected during setPowerModeInternal.
    mFlinger.setPowerModeInternal(mInnerDisplay, PowerMode::ON);
    mFlinger.setPowerModeInternal(mInnerDisplay, PowerMode::OFF);
    mFlinger.setPowerModeInternal(mOuterDisplay, PowerMode::ON);

    EXPECT_FALSE(mInnerDisplay->isPoweredOn());
    EXPECT_TRUE(mOuterDisplay->isPoweredOn());

    auto& scheduler = *mFlinger.scheduler();
    scheduler.onHardwareVsyncRequest(kInnerDisplayId, true);
    scheduler.onHardwareVsyncRequest(kOuterDisplayId, true);
}

TEST_F(FoldableTest, requestsHardwareVsyncForBothDisplays) {
    // Both displays are powered on.
    EXPECT_CALL(mFlinger.mockSchedulerCallback(), requestHardwareVsync(kInnerDisplayId, true))
            .Times(1);
    EXPECT_CALL(mFlinger.mockSchedulerCallback(), requestHardwareVsync(kOuterDisplayId, true))
            .Times(1);

    // The injected VsyncSchedule uses TestableScheduler::mockRequestHardwareVsync, so no calls to
    // ISchedulerCallback::requestHardwareVsync are expected during setPowerModeInternal.
    mFlinger.setPowerModeInternal(mInnerDisplay, PowerMode::ON);
    mFlinger.setPowerModeInternal(mOuterDisplay, PowerMode::ON);

    EXPECT_TRUE(mInnerDisplay->isPoweredOn());
    EXPECT_TRUE(mOuterDisplay->isPoweredOn());

    auto& scheduler = *mFlinger.scheduler();
    scheduler.onHardwareVsyncRequest(mInnerDisplay->getPhysicalId(), true);
    scheduler.onHardwareVsyncRequest(mOuterDisplay->getPhysicalId(), true);
}

} // namespace
} // namespace android
