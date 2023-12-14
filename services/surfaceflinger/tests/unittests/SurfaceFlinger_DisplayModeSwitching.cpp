/*
 * Copyright 2021 The Android Open Source Project
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
#include "mock/DisplayHardware/MockDisplayMode.h"
#include "mock/MockDisplayModeSpecs.h"

#include <ftl/fake_guard.h>
#include <scheduler/Fps.h>

namespace android {
namespace {

using android::hardware::graphics::composer::V2_4::Error;
using android::hardware::graphics::composer::V2_4::VsyncPeriodChangeTimeline;

class DisplayModeSwitchingTest : public DisplayTransactionTest {
public:
    void SetUp() override {
        injectFakeBufferQueueFactory();
        injectFakeNativeWindowSurfaceFactory();

        PrimaryDisplayVariant::setupHwcHotplugCallExpectations(this);
        PrimaryDisplayVariant::setupFramebufferConsumerBufferQueueCallExpectations(this);
        PrimaryDisplayVariant::setupFramebufferProducerBufferQueueCallExpectations(this);
        PrimaryDisplayVariant::setupNativeWindowSurfaceCreationCallExpectations(this);
        PrimaryDisplayVariant::setupHwcGetActiveConfigCallExpectations(this);

        auto selectorPtr = std::make_shared<scheduler::RefreshRateSelector>(kModes, kModeId60);

        setupScheduler(selectorPtr);

        mFlinger.onComposerHalHotplug(PrimaryDisplayVariant::HWC_DISPLAY_ID, Connection::CONNECTED);
        mFlinger.configureAndCommit();

        mDisplay = PrimaryDisplayVariant::makeFakeExistingDisplayInjector(this)
                           .setRefreshRateSelector(std::move(selectorPtr))
                           .inject();

        // isVsyncPeriodSwitchSupported should return true, otherwise the SF's HWC proxy
        // will call setActiveConfig instead of setActiveConfigWithConstraints.
        ON_CALL(*mComposer, isSupported(Hwc2::Composer::OptionalFeature::RefreshRateSwitching))
                .WillByDefault(Return(true));
    }

protected:
    void setupScheduler(std::shared_ptr<scheduler::RefreshRateSelector>);

    sp<DisplayDevice> mDisplay;
    mock::EventThread* mAppEventThread;

    static constexpr DisplayModeId kModeId60{0};
    static constexpr DisplayModeId kModeId90{1};
    static constexpr DisplayModeId kModeId120{2};
    static constexpr DisplayModeId kModeId90_4K{3};

    static inline const DisplayModePtr kMode60 = createDisplayMode(kModeId60, 60_Hz, 0);
    static inline const DisplayModePtr kMode90 = createDisplayMode(kModeId90, 90_Hz, 1);
    static inline const DisplayModePtr kMode120 = createDisplayMode(kModeId120, 120_Hz, 2);

    static constexpr ui::Size kResolution4K{3840, 2160};
    static inline const DisplayModePtr kMode90_4K =
            createDisplayMode(kModeId90_4K, 90_Hz, 3, kResolution4K);

    static inline const DisplayModes kModes = makeModes(kMode60, kMode90, kMode120, kMode90_4K);
};

void DisplayModeSwitchingTest::setupScheduler(
        std::shared_ptr<scheduler::RefreshRateSelector> selectorPtr) {
    auto eventThread = std::make_unique<mock::EventThread>();
    mAppEventThread = eventThread.get();
    auto sfEventThread = std::make_unique<mock::EventThread>();

    EXPECT_CALL(*eventThread, registerDisplayEventConnection(_));
    EXPECT_CALL(*eventThread, createEventConnection(_, _))
            .WillOnce(Return(sp<EventThreadConnection>::make(eventThread.get(),
                                                             mock::EventThread::kCallingUid,
                                                             ResyncCallback())));

    EXPECT_CALL(*sfEventThread, registerDisplayEventConnection(_));
    EXPECT_CALL(*sfEventThread, createEventConnection(_, _))
            .WillOnce(Return(sp<EventThreadConnection>::make(sfEventThread.get(),
                                                             mock::EventThread::kCallingUid,
                                                             ResyncCallback())));

    auto vsyncController = std::make_unique<mock::VsyncController>();
    auto vsyncTracker = std::make_shared<mock::VSyncTracker>();

    EXPECT_CALL(*vsyncTracker, nextAnticipatedVSyncTimeFrom(_)).WillRepeatedly(Return(0));
    EXPECT_CALL(*vsyncTracker, currentPeriod())
            .WillRepeatedly(
                    Return(TestableSurfaceFlinger::FakeHwcDisplayInjector::DEFAULT_VSYNC_PERIOD));
    EXPECT_CALL(*vsyncTracker, nextAnticipatedVSyncTimeFrom(_)).WillRepeatedly(Return(0));
    mFlinger.setupScheduler(std::move(vsyncController), std::move(vsyncTracker),
                            std::move(eventThread), std::move(sfEventThread),
                            std::move(selectorPtr),
                            TestableSurfaceFlinger::SchedulerCallbackImpl::kNoOp);
}

TEST_F(DisplayModeSwitchingTest, changeRefreshRate_OnActiveDisplay_WithRefreshRequired) {
    ftl::FakeGuard guard(kMainThreadContext);

    ASSERT_FALSE(mDisplay->getDesiredActiveMode().has_value());
    ASSERT_EQ(mDisplay->getActiveMode().modePtr->getId(), kModeId60);

    mFlinger.onActiveDisplayChanged(nullptr, *mDisplay);

    mFlinger.setDesiredDisplayModeSpecs(mDisplay->getDisplayToken().promote(),
                                        mock::createDisplayModeSpecs(kModeId90.value(), false, 0,
                                                                     120));

    ASSERT_TRUE(mDisplay->getDesiredActiveMode().has_value());
    ASSERT_EQ(mDisplay->getDesiredActiveMode()->modeOpt->modePtr->getId(), kModeId90);
    ASSERT_EQ(mDisplay->getActiveMode().modePtr->getId(), kModeId60);

    // Verify that next commit will call setActiveConfigWithConstraints in HWC
    const VsyncPeriodChangeTimeline timeline{.refreshRequired = true};
    EXPECT_CALL(*mComposer,
                setActiveConfigWithConstraints(PrimaryDisplayVariant::HWC_DISPLAY_ID,
                                               hal::HWConfigId(kModeId90.value()), _, _))
            .WillOnce(DoAll(SetArgPointee<3>(timeline), Return(Error::NONE)));

    mFlinger.commit();

    Mock::VerifyAndClearExpectations(mComposer);
    ASSERT_TRUE(mDisplay->getDesiredActiveMode().has_value());
    ASSERT_EQ(mDisplay->getActiveMode().modePtr->getId(), kModeId60);

    // Verify that the next commit will complete the mode change and send
    // a onModeChanged event to the framework.

    EXPECT_CALL(*mAppEventThread,
                onModeChanged(scheduler::FrameRateMode{90_Hz, ftl::as_non_null(kMode90)}));
    mFlinger.commit();
    Mock::VerifyAndClearExpectations(mAppEventThread);

    ASSERT_FALSE(mDisplay->getDesiredActiveMode().has_value());
    ASSERT_EQ(mDisplay->getActiveMode().modePtr->getId(), kModeId90);
}

TEST_F(DisplayModeSwitchingTest, changeRefreshRate_OnActiveDisplay_WithoutRefreshRequired) {
    ftl::FakeGuard guard(kMainThreadContext);

    ASSERT_FALSE(mDisplay->getDesiredActiveMode().has_value());

    mFlinger.onActiveDisplayChanged(nullptr, *mDisplay);

    mFlinger.setDesiredDisplayModeSpecs(mDisplay->getDisplayToken().promote(),
                                        mock::createDisplayModeSpecs(kModeId90.value(), true, 0,
                                                                     120));

    ASSERT_TRUE(mDisplay->getDesiredActiveMode().has_value());
    ASSERT_EQ(mDisplay->getDesiredActiveMode()->modeOpt->modePtr->getId(), kModeId90);
    ASSERT_EQ(mDisplay->getActiveMode().modePtr->getId(), kModeId60);

    // Verify that next commit will call setActiveConfigWithConstraints in HWC
    // and complete the mode change.
    const VsyncPeriodChangeTimeline timeline{.refreshRequired = false};
    EXPECT_CALL(*mComposer,
                setActiveConfigWithConstraints(PrimaryDisplayVariant::HWC_DISPLAY_ID,
                                               hal::HWConfigId(kModeId90.value()), _, _))
            .WillOnce(DoAll(SetArgPointee<3>(timeline), Return(Error::NONE)));

    EXPECT_CALL(*mAppEventThread,
                onModeChanged(scheduler::FrameRateMode{90_Hz, ftl::as_non_null(kMode90)}));

    mFlinger.commit();

    ASSERT_FALSE(mDisplay->getDesiredActiveMode().has_value());
    ASSERT_EQ(mDisplay->getActiveMode().modePtr->getId(), kModeId90);
}

TEST_F(DisplayModeSwitchingTest, twoConsecutiveSetDesiredDisplayModeSpecs) {
    ftl::FakeGuard guard(kMainThreadContext);

    // Test that if we call setDesiredDisplayModeSpecs while a previous mode change
    // is still being processed the later call will be respected.

    ASSERT_FALSE(mDisplay->getDesiredActiveMode().has_value());
    ASSERT_EQ(mDisplay->getActiveMode().modePtr->getId(), kModeId60);

    mFlinger.onActiveDisplayChanged(nullptr, *mDisplay);

    mFlinger.setDesiredDisplayModeSpecs(mDisplay->getDisplayToken().promote(),
                                        mock::createDisplayModeSpecs(kModeId90.value(), false, 0,
                                                                     120));

    const VsyncPeriodChangeTimeline timeline{.refreshRequired = true};
    EXPECT_CALL(*mComposer,
                setActiveConfigWithConstraints(PrimaryDisplayVariant::HWC_DISPLAY_ID,
                                               hal::HWConfigId(kModeId90.value()), _, _))
            .WillOnce(DoAll(SetArgPointee<3>(timeline), Return(Error::NONE)));

    mFlinger.commit();

    mFlinger.setDesiredDisplayModeSpecs(mDisplay->getDisplayToken().promote(),
                                        mock::createDisplayModeSpecs(kModeId120.value(), false, 0,
                                                                     180));

    ASSERT_TRUE(mDisplay->getDesiredActiveMode().has_value());
    ASSERT_EQ(mDisplay->getDesiredActiveMode()->modeOpt->modePtr->getId(), kModeId120);

    EXPECT_CALL(*mComposer,
                setActiveConfigWithConstraints(PrimaryDisplayVariant::HWC_DISPLAY_ID,
                                               hal::HWConfigId(kModeId120.value()), _, _))
            .WillOnce(DoAll(SetArgPointee<3>(timeline), Return(Error::NONE)));

    mFlinger.commit();

    ASSERT_TRUE(mDisplay->getDesiredActiveMode().has_value());
    ASSERT_EQ(mDisplay->getDesiredActiveMode()->modeOpt->modePtr->getId(), kModeId120);

    mFlinger.commit();

    ASSERT_FALSE(mDisplay->getDesiredActiveMode().has_value());
    ASSERT_EQ(mDisplay->getActiveMode().modePtr->getId(), kModeId120);
}

TEST_F(DisplayModeSwitchingTest, changeResolution_OnActiveDisplay_WithoutRefreshRequired) {
    ftl::FakeGuard guard(kMainThreadContext);

    ASSERT_FALSE(mDisplay->getDesiredActiveMode().has_value());
    ASSERT_EQ(mDisplay->getActiveMode().modePtr->getId(), kModeId60);

    mFlinger.onActiveDisplayChanged(nullptr, *mDisplay);

    mFlinger.setDesiredDisplayModeSpecs(mDisplay->getDisplayToken().promote(),
                                        mock::createDisplayModeSpecs(kModeId90_4K.value(), false, 0,
                                                                     120));

    ASSERT_TRUE(mDisplay->getDesiredActiveMode().has_value());
    ASSERT_EQ(mDisplay->getDesiredActiveMode()->modeOpt->modePtr->getId(), kModeId90_4K);
    ASSERT_EQ(mDisplay->getActiveMode().modePtr->getId(), kModeId60);

    // Verify that next commit will call setActiveConfigWithConstraints in HWC
    // and complete the mode change.
    const VsyncPeriodChangeTimeline timeline{.refreshRequired = false};
    EXPECT_CALL(*mComposer,
                setActiveConfigWithConstraints(PrimaryDisplayVariant::HWC_DISPLAY_ID,
                                               hal::HWConfigId(kModeId90_4K.value()), _, _))
            .WillOnce(DoAll(SetArgPointee<3>(timeline), Return(Error::NONE)));

    EXPECT_CALL(*mAppEventThread, onHotplugReceived(mDisplay->getPhysicalId(), true));

    // Misc expecations. We don't need to enforce these method calls, but since the helper methods
    // already set expectations we should add new ones here, otherwise the test will fail.
    EXPECT_CALL(*mConsumer,
                setDefaultBufferSize(static_cast<uint32_t>(kResolution4K.getWidth()),
                                     static_cast<uint32_t>(kResolution4K.getHeight())))
            .WillOnce(Return(NO_ERROR));
    EXPECT_CALL(*mConsumer, consumerConnect(_, false)).WillOnce(Return(NO_ERROR));
    EXPECT_CALL(*mComposer, setClientTargetSlotCount(_)).WillOnce(Return(hal::Error::NONE));

    // Create a new native surface to be used by the recreated display.
    mNativeWindowSurface = nullptr;
    injectFakeNativeWindowSurfaceFactory();
    PrimaryDisplayVariant::setupNativeWindowSurfaceCreationCallExpectations(this);

    const auto displayToken = mDisplay->getDisplayToken().promote();

    mFlinger.commit();

    // The DisplayDevice will be destroyed and recreated,
    // so we need to update with the new instance.
    mDisplay = mFlinger.getDisplay(displayToken);

    ASSERT_FALSE(mDisplay->getDesiredActiveMode().has_value());
    ASSERT_EQ(mDisplay->getActiveMode().modePtr->getId(), kModeId90_4K);
}

MATCHER_P2(ModeSwitchingTo, flinger, modeId, "") {
    if (!arg->getDesiredActiveMode()) {
        *result_listener << "No desired active mode";
        return false;
    }

    if (arg->getDesiredActiveMode()->modeOpt->modePtr->getId() != modeId) {
        *result_listener << "Unexpected desired active mode " << modeId;
        return false;
    }

    if (!flinger->scheduler()->vsyncModulator().isVsyncConfigEarly()) {
        *result_listener << "VsyncModulator did not shift to early phase";
        return false;
    }

    return true;
}

MATCHER_P(ModeSettledTo, modeId, "") {
    if (const auto desiredOpt = arg->getDesiredActiveMode()) {
        *result_listener << "Unsettled desired active mode "
                         << desiredOpt->modeOpt->modePtr->getId();
        return false;
    }

    ftl::FakeGuard guard(kMainThreadContext);

    if (arg->getActiveMode().modePtr->getId() != modeId) {
        *result_listener << "Settled to unexpected active mode " << modeId;
        return false;
    }

    return true;
}

TEST_F(DisplayModeSwitchingTest, multiDisplay) {
    constexpr HWDisplayId kInnerDisplayHwcId = PrimaryDisplayVariant::HWC_DISPLAY_ID;
    constexpr HWDisplayId kOuterDisplayHwcId = kInnerDisplayHwcId + 1;

    constexpr PhysicalDisplayId kOuterDisplayId = PhysicalDisplayId::fromPort(254u);

    constexpr bool kIsPrimary = false;
    TestableSurfaceFlinger::FakeHwcDisplayInjector(kOuterDisplayId, hal::DisplayType::PHYSICAL,
                                                   kIsPrimary)
            .setHwcDisplayId(kOuterDisplayHwcId)
            .inject(&mFlinger, mComposer);

    const auto outerDisplay = mFakeDisplayInjector.injectInternalDisplay(
            [&](FakeDisplayDeviceInjector& injector) {
                injector.setDisplayModes(mock::cloneForDisplay(kOuterDisplayId, kModes),
                                         kModeId120);
            },
            {.displayId = kOuterDisplayId,
             .hwcDisplayId = kOuterDisplayHwcId,
             .isPrimary = kIsPrimary});

    const auto& innerDisplay = mDisplay;

    EXPECT_THAT(innerDisplay, ModeSettledTo(kModeId60));
    EXPECT_THAT(outerDisplay, ModeSettledTo(kModeId120));

    mFlinger.onActiveDisplayChanged(nullptr, *innerDisplay);

    EXPECT_THAT(innerDisplay, ModeSettledTo(kModeId60));
    EXPECT_THAT(outerDisplay, ModeSettledTo(kModeId120));

    EXPECT_EQ(NO_ERROR,
              mFlinger.setDesiredDisplayModeSpecs(innerDisplay->getDisplayToken().promote(),
                                                  mock::createDisplayModeSpecs(kModeId90.value(),
                                                                               false, 0.f, 120.f)));

    EXPECT_EQ(NO_ERROR,
              mFlinger.setDesiredDisplayModeSpecs(outerDisplay->getDisplayToken().promote(),
                                                  mock::createDisplayModeSpecs(kModeId60.value(),
                                                                               false, 0.f, 120.f)));

    EXPECT_THAT(innerDisplay, ModeSwitchingTo(&mFlinger, kModeId90));
    EXPECT_THAT(outerDisplay, ModeSettledTo(kModeId120));

    const VsyncPeriodChangeTimeline timeline{.refreshRequired = true};
    EXPECT_CALL(*mComposer,
                setActiveConfigWithConstraints(kInnerDisplayHwcId,
                                               hal::HWConfigId(kModeId90.value()), _, _))
            .WillOnce(DoAll(SetArgPointee<3>(timeline), Return(Error::NONE)));

    mFlinger.commit();

    EXPECT_THAT(innerDisplay, ModeSwitchingTo(&mFlinger, kModeId90));
    EXPECT_THAT(outerDisplay, ModeSettledTo(kModeId120));

    mFlinger.commit();

    EXPECT_THAT(innerDisplay, ModeSettledTo(kModeId90));
    EXPECT_THAT(outerDisplay, ModeSettledTo(kModeId120));

    mFlinger.onActiveDisplayChanged(innerDisplay.get(), *outerDisplay);

    EXPECT_THAT(innerDisplay, ModeSettledTo(kModeId90));
    EXPECT_THAT(outerDisplay, ModeSwitchingTo(&mFlinger, kModeId60));

    EXPECT_CALL(*mComposer,
                setActiveConfigWithConstraints(kOuterDisplayHwcId,
                                               hal::HWConfigId(kModeId60.value()), _, _))
            .WillOnce(DoAll(SetArgPointee<3>(timeline), Return(Error::NONE)));

    mFlinger.commit();

    EXPECT_THAT(innerDisplay, ModeSettledTo(kModeId90));
    EXPECT_THAT(outerDisplay, ModeSwitchingTo(&mFlinger, kModeId60));

    mFlinger.commit();

    EXPECT_THAT(innerDisplay, ModeSettledTo(kModeId90));
    EXPECT_THAT(outerDisplay, ModeSettledTo(kModeId60));
}

} // namespace
} // namespace android
