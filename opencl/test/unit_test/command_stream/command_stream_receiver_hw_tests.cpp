/*
 * Copyright (C) 2018-2020 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "shared/source/built_ins/built_ins.h"
#include "shared/source/command_stream/command_stream_receiver.h"
#include "shared/source/command_stream/linear_stream.h"
#include "shared/source/command_stream/preemption.h"
#include "shared/source/command_stream/scratch_space_controller.h"
#include "shared/source/command_stream/scratch_space_controller_base.h"
#include "shared/source/debug_settings/debug_settings_manager.h"
#include "shared/source/helpers/aligned_memory.h"
#include "shared/source/helpers/cache_policy.h"
#include "shared/source/helpers/preamble.h"
#include "shared/source/helpers/ptr_math.h"
#include "shared/source/memory_manager/graphics_allocation.h"
#include "shared/source/memory_manager/memory_manager.h"
#include "shared/source/memory_manager/unified_memory_manager.h"
#include "shared/source/os_interface/debug_env_reader.h"
#include "shared/source/os_interface/os_context.h"
#include "shared/test/unit_test/cmd_parse/hw_parse.h"
#include "shared/test/unit_test/helpers/debug_manager_state_restore.h"
#include "shared/test/unit_test/helpers/dispatch_flags_helper.h"
#include "shared/test/unit_test/utilities/base_object_utils.h"

#include "opencl/source/command_queue/command_queue_hw.h"
#include "opencl/source/command_queue/gpgpu_walker.h"
#include "opencl/source/event/user_event.h"
#include "opencl/source/helpers/cl_blit_properties.h"
#include "opencl/source/mem_obj/buffer.h"
#include "opencl/source/mem_obj/mem_obj_helper.h"
#include "opencl/test/unit_test/fixtures/built_in_fixture.h"
#include "opencl/test/unit_test/fixtures/cl_device_fixture.h"
#include "opencl/test/unit_test/fixtures/ult_command_stream_receiver_fixture.h"
#include "opencl/test/unit_test/helpers/raii_hw_helper.h"
#include "opencl/test/unit_test/helpers/unit_test_helper.h"
#include "opencl/test/unit_test/libult/ult_command_stream_receiver.h"
#include "opencl/test/unit_test/mocks/mock_allocation_properties.h"
#include "opencl/test/unit_test/mocks/mock_buffer.h"
#include "opencl/test/unit_test/mocks/mock_command_queue.h"
#include "opencl/test/unit_test/mocks/mock_context.h"
#include "opencl/test/unit_test/mocks/mock_csr.h"
#include "opencl/test/unit_test/mocks/mock_event.h"
#include "opencl/test/unit_test/mocks/mock_hw_helper.h"
#include "opencl/test/unit_test/mocks/mock_internal_allocation_storage.h"
#include "opencl/test/unit_test/mocks/mock_kernel.h"
#include "opencl/test/unit_test/mocks/mock_memory_manager.h"
#include "opencl/test/unit_test/mocks/mock_submissions_aggregator.h"
#include "opencl/test/unit_test/mocks/mock_timestamp_container.h"
#include "test.h"

#include "reg_configs_common.h"

#include <memory>

using namespace NEO;

HWCMDTEST_F(IGFX_GEN8_CORE, UltCommandStreamReceiverTest, givenPreambleSentAndThreadArbitrationPolicyNotChangedWhenEstimatingPreambleCmdSizeThenReturnItsValue) {
    auto &commandStreamReceiver = pDevice->getUltCommandStreamReceiver<FamilyType>();
    commandStreamReceiver.isPreambleSent = true;
    commandStreamReceiver.requiredThreadArbitrationPolicy = commandStreamReceiver.lastSentThreadArbitrationPolicy;
    auto expectedCmdSize = sizeof(typename FamilyType::PIPE_CONTROL) + sizeof(typename FamilyType::MEDIA_VFE_STATE);
    EXPECT_EQ(expectedCmdSize, commandStreamReceiver.getRequiredCmdSizeForPreamble(*pDevice));
}

HWCMDTEST_F(IGFX_GEN8_CORE, UltCommandStreamReceiverTest, givenNotSentStateSipWhenFirstTaskIsFlushedThenStateSipCmdIsAddedAndIsStateSipSentSetToTrue) {
    using STATE_SIP = typename FamilyType::STATE_SIP;

    auto mockDevice = std::make_unique<MockClDevice>(MockDevice::createWithNewExecutionEnvironment<MockDevice>(nullptr));

    if (mockDevice->getHardwareInfo().capabilityTable.defaultPreemptionMode == PreemptionMode::MidThread) {
        mockDevice->setPreemptionMode(PreemptionMode::MidThread);

        auto &csr = mockDevice->getUltCommandStreamReceiver<FamilyType>();
        csr.isPreambleSent = true;

        CommandQueueHw<FamilyType> commandQueue(nullptr, mockDevice.get(), 0, false);
        auto &commandStream = commandQueue.getCS(4096u);

        DispatchFlags dispatchFlags = DispatchFlagsHelper::createDefaultDispatchFlags();
        dispatchFlags.preemptionMode = PreemptionMode::MidThread;

        MockGraphicsAllocation allocation(nullptr, 0);
        IndirectHeap heap(&allocation);

        csr.flushTask(commandStream,
                      0,
                      heap,
                      heap,
                      heap,
                      0,
                      dispatchFlags,
                      mockDevice->getDevice());

        EXPECT_TRUE(csr.isStateSipSent);

        HardwareParse hwParser;
        hwParser.parseCommands<FamilyType>(csr.getCS(0));

        auto stateSipItor = find<STATE_SIP *>(hwParser.cmdList.begin(), hwParser.cmdList.end());
        EXPECT_NE(hwParser.cmdList.end(), stateSipItor);
    }
}

HWTEST_F(UltCommandStreamReceiverTest, givenCsrWhenProgramStateSipIsCalledThenIsStateSipCalledIsSetToTrue) {
    auto &commandStreamReceiver = pDevice->getUltCommandStreamReceiver<FamilyType>();

    auto requiredSize = PreemptionHelper::getRequiredStateSipCmdSize<FamilyType>(*pDevice);
    StackVec<char, 4096> buffer(requiredSize);
    LinearStream cmdStream(buffer.begin(), buffer.size());

    commandStreamReceiver.programStateSip(cmdStream, *pDevice);
    EXPECT_TRUE(commandStreamReceiver.isStateSipSent);
}

HWTEST_F(UltCommandStreamReceiverTest, givenSentStateSipFlagSetWhenGetRequiredStateSipCmdSizeIsCalledThenStateSipCmdSizeIsNotIncluded) {
    auto &commandStreamReceiver = pDevice->getUltCommandStreamReceiver<FamilyType>();
    DispatchFlags dispatchFlags = DispatchFlagsHelper::createDefaultDispatchFlags();

    commandStreamReceiver.isStateSipSent = false;
    auto sizeWithStateSipIsNotSent = commandStreamReceiver.getRequiredCmdStreamSize(dispatchFlags, *pDevice);

    commandStreamReceiver.isStateSipSent = true;
    auto sizeWhenSipIsSent = commandStreamReceiver.getRequiredCmdStreamSize(dispatchFlags, *pDevice);

    auto sizeForStateSip = PreemptionHelper::getRequiredStateSipCmdSize<FamilyType>(*pDevice);
    EXPECT_EQ(sizeForStateSip, sizeWithStateSipIsNotSent - sizeWhenSipIsSent);
}

HWTEST_F(UltCommandStreamReceiverTest, givenSentStateSipFlagSetAndSourceLevelDebuggerIsActiveWhenGetRequiredStateSipCmdSizeIsCalledThenStateSipCmdSizeIsIncluded) {
    auto &commandStreamReceiver = pDevice->getUltCommandStreamReceiver<FamilyType>();
    DispatchFlags dispatchFlags = DispatchFlagsHelper::createDefaultDispatchFlags();

    commandStreamReceiver.isStateSipSent = true;
    auto sizeWithoutSourceKernelDebugging = commandStreamReceiver.getRequiredCmdStreamSize(dispatchFlags, *pDevice);

    pDevice->setDebuggerActive(true);
    commandStreamReceiver.isStateSipSent = true;
    auto sizeWithSourceKernelDebugging = commandStreamReceiver.getRequiredCmdStreamSize(dispatchFlags, *pDevice);

    auto sizeForStateSip = PreemptionHelper::getRequiredStateSipCmdSize<FamilyType>(*pDevice);
    EXPECT_EQ(sizeForStateSip, sizeWithSourceKernelDebugging - sizeWithoutSourceKernelDebugging - PreambleHelper<FamilyType>::getKernelDebuggingCommandsSize(true));
    pDevice->setDebuggerActive(false);
}

HWTEST_F(UltCommandStreamReceiverTest, givenPreambleSentAndThreadArbitrationPolicyChangedWhenEstimatingPreambleCmdSizeThenResultDependsOnPolicyProgrammingCmdSize) {
    auto &commandStreamReceiver = pDevice->getUltCommandStreamReceiver<FamilyType>();
    commandStreamReceiver.isPreambleSent = true;

    commandStreamReceiver.requiredThreadArbitrationPolicy = commandStreamReceiver.lastSentThreadArbitrationPolicy;
    auto policyNotChanged = commandStreamReceiver.getRequiredCmdSizeForPreamble(*pDevice);

    commandStreamReceiver.requiredThreadArbitrationPolicy = commandStreamReceiver.lastSentThreadArbitrationPolicy + 1;
    auto policyChanged = commandStreamReceiver.getRequiredCmdSizeForPreamble(*pDevice);

    auto actualDifference = policyChanged - policyNotChanged;
    auto expectedDifference = PreambleHelper<FamilyType>::getThreadArbitrationCommandsSize();
    EXPECT_EQ(expectedDifference, actualDifference);
}

HWTEST_F(UltCommandStreamReceiverTest, givenPreambleSentWhenEstimatingPreambleCmdSizeThenResultDependsOnPolicyProgrammingAndAdditionalCmdsSize) {
    auto &commandStreamReceiver = pDevice->getUltCommandStreamReceiver<FamilyType>();
    commandStreamReceiver.requiredThreadArbitrationPolicy = commandStreamReceiver.lastSentThreadArbitrationPolicy;

    commandStreamReceiver.isPreambleSent = false;
    auto preambleNotSent = commandStreamReceiver.getRequiredCmdSizeForPreamble(*pDevice);

    commandStreamReceiver.isPreambleSent = true;
    auto preambleSent = commandStreamReceiver.getRequiredCmdSizeForPreamble(*pDevice);

    auto actualDifference = preambleNotSent - preambleSent;
    auto expectedDifference = PreambleHelper<FamilyType>::getThreadArbitrationCommandsSize() + PreambleHelper<FamilyType>::getAdditionalCommandsSize(*pDevice);

    EXPECT_EQ(expectedDifference, actualDifference);
}

HWTEST_F(UltCommandStreamReceiverTest, givenPerDssBackBufferProgrammingEnabledWhenEstimatingPreambleCmdSizeThenResultIncludesPerDssBackBufferProgramingCommandsSize) {
    DebugManagerStateRestore restore;
    DebugManager.flags.ForcePerDssBackedBufferProgramming.set(true);

    auto &commandStreamReceiver = pDevice->getUltCommandStreamReceiver<FamilyType>();
    commandStreamReceiver.requiredThreadArbitrationPolicy = commandStreamReceiver.lastSentThreadArbitrationPolicy;

    commandStreamReceiver.isPreambleSent = false;
    auto preambleNotSent = commandStreamReceiver.getRequiredCmdSizeForPreamble(*pDevice);

    commandStreamReceiver.isPreambleSent = true;
    auto preambleSent = commandStreamReceiver.getRequiredCmdSizeForPreamble(*pDevice);

    auto actualDifference = preambleNotSent - preambleSent;
    auto expectedDifference = PreambleHelper<FamilyType>::getThreadArbitrationCommandsSize() + PreambleHelper<FamilyType>::getAdditionalCommandsSize(*pDevice) + PreambleHelper<FamilyType>::getPerDssBackedBufferCommandsSize(pDevice->getHardwareInfo());

    EXPECT_EQ(expectedDifference, actualDifference);
}

HWCMDTEST_F(IGFX_GEN8_CORE, UltCommandStreamReceiverTest, givenMediaVfeStateDirtyEstimatingPreambleCmdSizeThenResultDependsVfeStateProgrammingCmdSize) {
    typedef typename FamilyType::MEDIA_VFE_STATE MEDIA_VFE_STATE;
    typedef typename FamilyType::PIPE_CONTROL PIPE_CONTROL;

    auto &commandStreamReceiver = pDevice->getUltCommandStreamReceiver<FamilyType>();

    commandStreamReceiver.setMediaVFEStateDirty(false);
    auto notDirty = commandStreamReceiver.getRequiredCmdSizeForPreamble(*pDevice);

    commandStreamReceiver.setMediaVFEStateDirty(true);
    auto dirty = commandStreamReceiver.getRequiredCmdSizeForPreamble(*pDevice);

    auto actualDifference = dirty - notDirty;
    auto expectedDifference = sizeof(PIPE_CONTROL) + sizeof(MEDIA_VFE_STATE);
    EXPECT_EQ(expectedDifference, actualDifference);
}

HWTEST_F(UltCommandStreamReceiverTest, givenCommandStreamReceiverInInitialStateWhenHeapsAreAskedForDirtyStatusThenTrueIsReturned) {
    auto &commandStreamReceiver = pDevice->getUltCommandStreamReceiver<FamilyType>();

    EXPECT_EQ(0u, commandStreamReceiver.peekTaskCount());
    EXPECT_EQ(0u, commandStreamReceiver.peekTaskLevel());

    EXPECT_TRUE(commandStreamReceiver.dshState.updateAndCheck(&dsh));
    EXPECT_TRUE(commandStreamReceiver.iohState.updateAndCheck(&ioh));
    EXPECT_TRUE(commandStreamReceiver.sshState.updateAndCheck(&ssh));
}

HWTEST_F(UltCommandStreamReceiverTest, givenPreambleSentAndForceSemaphoreDelayBetweenWaitsFlagWhenEstimatingPreambleCmdSizeThenResultIsExpected) {
    auto &commandStreamReceiver = pDevice->getUltCommandStreamReceiver<FamilyType>();
    commandStreamReceiver.requiredThreadArbitrationPolicy = commandStreamReceiver.lastSentThreadArbitrationPolicy;
    DebugManagerStateRestore debugManagerStateRestore;

    DebugManager.flags.ForceSemaphoreDelayBetweenWaits.set(-1);
    commandStreamReceiver.isPreambleSent = false;

    auto preambleNotSentAndSemaphoreDelayNotReprogrammed = commandStreamReceiver.getRequiredCmdSizeForPreamble(*pDevice);

    DebugManager.flags.ForceSemaphoreDelayBetweenWaits.set(0);
    commandStreamReceiver.isPreambleSent = false;

    auto preambleNotSentAndSemaphoreDelayReprogrammed = commandStreamReceiver.getRequiredCmdSizeForPreamble(*pDevice);

    commandStreamReceiver.isPreambleSent = true;
    auto preambleSent = commandStreamReceiver.getRequiredCmdSizeForPreamble(*pDevice);

    auto actualDifferenceWhenSemaphoreDelayNotReprogrammed = preambleNotSentAndSemaphoreDelayNotReprogrammed - preambleSent;
    auto expectedDifference = PreambleHelper<FamilyType>::getThreadArbitrationCommandsSize() + PreambleHelper<FamilyType>::getAdditionalCommandsSize(*pDevice);

    EXPECT_EQ(expectedDifference, actualDifferenceWhenSemaphoreDelayNotReprogrammed);

    auto actualDifferenceWhenSemaphoreDelayReprogrammed = preambleNotSentAndSemaphoreDelayReprogrammed - preambleSent;
    expectedDifference = PreambleHelper<FamilyType>::getThreadArbitrationCommandsSize() + PreambleHelper<FamilyType>::getAdditionalCommandsSize(*pDevice) + PreambleHelper<FamilyType>::getSemaphoreDelayCommandSize();

    EXPECT_EQ(expectedDifference, actualDifferenceWhenSemaphoreDelayReprogrammed);
}

typedef UltCommandStreamReceiverTest CommandStreamReceiverFlushTests;

HWTEST_F(CommandStreamReceiverFlushTests, WhenAddingBatchBufferEndThenBatchBufferEndIsAppendedCorrectly) {
    auto usedPrevious = commandStream.getUsed();

    CommandStreamReceiverHw<FamilyType>::addBatchBufferEnd(commandStream, nullptr);

    EXPECT_EQ(commandStream.getUsed(), usedPrevious + sizeof(typename FamilyType::MI_BATCH_BUFFER_END));

    auto batchBufferEnd = genCmdCast<typename FamilyType::MI_BATCH_BUFFER_END *>(
        ptrOffset(commandStream.getCpuBase(), usedPrevious));
    EXPECT_NE(nullptr, batchBufferEnd);
}

HWTEST_F(CommandStreamReceiverFlushTests, WhenAligningCommandStreamReceiverToCacheLineSizeThenItIsAlignedCorrectly) {
    commandStream.getSpace(sizeof(uint32_t));
    CommandStreamReceiverHw<FamilyType>::alignToCacheLine(commandStream);

    EXPECT_EQ(0u, commandStream.getUsed() % MemoryConstants::cacheLineSize);
}

typedef Test<ClDeviceFixture> CommandStreamReceiverHwTest;

HWTEST_F(CommandStreamReceiverHwTest, givenCsrHwWhenTypeIsCheckedThenCsrHwIsReturned) {
    auto csr = std::unique_ptr<CommandStreamReceiver>(CommandStreamReceiverHw<FamilyType>::create(*pDevice->executionEnvironment, pDevice->getRootDeviceIndex()));

    EXPECT_EQ(CommandStreamReceiverType::CSR_HW, csr->getType());
}

HWCMDTEST_F(IGFX_GEN8_CORE, CommandStreamReceiverHwTest, WhenCommandStreamReceiverHwIsCreatedThenDefaultSshSizeIs64KB) {
    auto &commandStreamReceiver = pDevice->getGpgpuCommandStreamReceiver();
    EXPECT_EQ(64 * KB, commandStreamReceiver.defaultSshSize);
}

HWTEST_F(CommandStreamReceiverHwTest, WhenScratchSpaceIsNotRequiredThenScratchAllocationIsNotCreated) {
    auto commandStreamReceiver = std::make_unique<MockCsrHw<FamilyType>>(*pDevice->executionEnvironment, pDevice->getRootDeviceIndex());
    auto scratchController = commandStreamReceiver->getScratchSpaceController();

    bool stateBaseAddressDirty = false;
    bool cfeStateDirty = false;
    scratchController->setRequiredScratchSpace(reinterpret_cast<void *>(0x2000), 0u, 0u, 0u, *pDevice->getDefaultEngine().osContext, stateBaseAddressDirty, cfeStateDirty);
    EXPECT_FALSE(cfeStateDirty);
    EXPECT_FALSE(stateBaseAddressDirty);
    EXPECT_EQ(nullptr, scratchController->getScratchSpaceAllocation());
    EXPECT_EQ(nullptr, scratchController->getPrivateScratchSpaceAllocation());
}

HWTEST_F(CommandStreamReceiverHwTest, WhenScratchSpaceIsRequiredThenCorrectAddressIsReturned) {
    auto commandStreamReceiver = std::make_unique<MockCsrHw<FamilyType>>(*pDevice->executionEnvironment, pDevice->getRootDeviceIndex());
    commandStreamReceiver->setupContext(*pDevice->getDefaultEngine().osContext);
    auto scratchController = commandStreamReceiver->getScratchSpaceController();

    bool cfeStateDirty = false;
    bool stateBaseAddressDirty = false;

    std::unique_ptr<void, std::function<decltype(alignedFree)>> surfaceHeap(alignedMalloc(0x1000, 0x1000), alignedFree);
    scratchController->setRequiredScratchSpace(surfaceHeap.get(), 0x1000u, 0u, 0u, *pDevice->getDefaultEngine().osContext, stateBaseAddressDirty, cfeStateDirty);

    uint64_t expectedScratchAddress = 0xAAABBBCCCDDD000ull;
    auto scratchAllocation = scratchController->getScratchSpaceAllocation();
    scratchAllocation->setCpuPtrAndGpuAddress(scratchAllocation->getUnderlyingBuffer(), expectedScratchAddress);
    EXPECT_TRUE(UnitTestHelper<FamilyType>::evaluateGshAddressForScratchSpace((scratchAllocation->getGpuAddress() - MemoryConstants::pageSize), scratchController->calculateNewGSH()));
}

HWTEST_F(CommandStreamReceiverHwTest, WhenScratchSpaceIsNotRequiredThenGshAddressZeroIsReturned) {
    auto commandStreamReceiver = std::make_unique<MockCsrHw<FamilyType>>(*pDevice->executionEnvironment, pDevice->getRootDeviceIndex());
    auto scratchController = commandStreamReceiver->getScratchSpaceController();

    EXPECT_EQ(nullptr, scratchController->getScratchSpaceAllocation());
    EXPECT_EQ(0u, scratchController->calculateNewGSH());
}

struct BcsTests : public CommandStreamReceiverHwTest {
    void SetUp() override {
        CommandStreamReceiverHwTest::SetUp();

        auto &csr = pDevice->getGpgpuCommandStreamReceiver();
        auto engine = csr.getMemoryManager()->getRegisteredEngineForCsr(&csr);
        auto contextId = engine->osContext->getContextId();

        delete engine->osContext;
        engine->osContext = OsContext::create(nullptr, contextId, pDevice->getDeviceBitfield(), aub_stream::EngineType::ENGINE_BCS, PreemptionMode::Disabled,
                                              false, false, false);
        engine->osContext->incRefInternal();
        csr.setupContext(*engine->osContext);

        context = std::make_unique<MockContext>(pClDevice);
    }

    void TearDown() override {
        context.reset();
        CommandStreamReceiverHwTest::TearDown();
    }

    uint32_t blitBuffer(CommandStreamReceiver *bcsCsr, const BlitProperties &blitProperties, bool blocking) {
        BlitPropertiesContainer container;
        container.push_back(blitProperties);

        return bcsCsr->blitBuffer(container, blocking, false);
    }

    TimestampPacketContainer timestampPacketContainer;
    CsrDependencies csrDependencies;
    std::unique_ptr<MockContext> context;
};

HWTEST_F(BcsTests, givenBltSizeWhenEstimatingCommandSizeThenAddAllRequiredCommands) {
    constexpr auto max2DBlitSize = BlitterConstants::maxBlitWidth * BlitterConstants::maxBlitHeight;
    constexpr size_t cmdsSizePerBlit = sizeof(typename FamilyType::XY_COPY_BLT) + sizeof(typename FamilyType::MI_ARB_CHECK);
    size_t notAlignedBltSize = (3 * max2DBlitSize) + 1;
    size_t alignedBltSize = (3 * max2DBlitSize);
    uint32_t alignedNumberOfBlts = 3;
    uint32_t notAlignedNumberOfBlts = 4;

    auto expectedAlignedSize = cmdsSizePerBlit * alignedNumberOfBlts;
    auto expectedNotAlignedSize = cmdsSizePerBlit * notAlignedNumberOfBlts;

    auto alignedEstimatedSize = BlitCommandsHelper<FamilyType>::estimateBlitCommandsSize(
        {alignedBltSize, 1, 1}, csrDependencies, false, false, pClDevice->getRootDeviceEnvironment());
    auto notAlignedEstimatedSize = BlitCommandsHelper<FamilyType>::estimateBlitCommandsSize(
        {notAlignedBltSize, 1, 1}, csrDependencies, false, false, pClDevice->getRootDeviceEnvironment());

    EXPECT_EQ(expectedAlignedSize, alignedEstimatedSize);
    EXPECT_EQ(expectedNotAlignedSize, notAlignedEstimatedSize);
}

HWTEST_F(BcsTests, givenDebugCapabilityWhenEstimatingCommandSizeThenAddAllRequiredCommands) {
    constexpr auto max2DBlitSize = BlitterConstants::maxBlitWidth * BlitterConstants::maxBlitHeight;
    constexpr size_t cmdsSizePerBlit = sizeof(typename FamilyType::XY_COPY_BLT) + sizeof(typename FamilyType::MI_ARB_CHECK);
    const size_t debugCommandsSize = (EncodeMiFlushDW<FamilyType>::getMiFlushDwCmdSizeForDataWrite() + EncodeSempahore<FamilyType>::getSizeMiSemaphoreWait()) * 2;

    constexpr uint32_t numberOfBlts = 3;
    constexpr size_t bltSize = (numberOfBlts * max2DBlitSize);

    auto expectedSize = (cmdsSizePerBlit * numberOfBlts) + debugCommandsSize + MemorySynchronizationCommands<FamilyType>::getSizeForAdditonalSynchronization(pDevice->getHardwareInfo()) +
                        EncodeMiFlushDW<FamilyType>::getMiFlushDwCmdSizeForDataWrite() + sizeof(typename FamilyType::MI_BATCH_BUFFER_END);
    expectedSize = alignUp(expectedSize, MemoryConstants::cacheLineSize);

    BlitProperties blitProperties;
    blitProperties.copySize = {bltSize, 1, 1};
    BlitPropertiesContainer blitPropertiesContainer;
    blitPropertiesContainer.push_back(blitProperties);

    auto estimatedSize = BlitCommandsHelper<FamilyType>::estimateBlitCommandsSize(
        blitPropertiesContainer, false, true, pClDevice->getRootDeviceEnvironment());

    EXPECT_EQ(expectedSize, estimatedSize);
}

HWTEST_F(BcsTests, givenBltSizeWhenEstimatingCommandSizeForReadBufferRectThenAddAllRequiredCommands) {
    constexpr auto max2DBlitSize = BlitterConstants::maxBlitWidth * BlitterConstants::maxBlitHeight;
    constexpr size_t cmdsSizePerBlit = sizeof(typename FamilyType::XY_COPY_BLT) + sizeof(typename FamilyType::MI_ARB_CHECK);
    Vec3<size_t> notAlignedBltSize = {(3 * max2DBlitSize) + 1, 4, 2};
    Vec3<size_t> alignedBltSize = {(3 * max2DBlitSize), 4, 2};
    size_t alignedNumberOfBlts = 3 * alignedBltSize.y * alignedBltSize.z;
    size_t notAlignedNumberOfBlts = 4 * notAlignedBltSize.y * notAlignedBltSize.z;

    auto expectedAlignedSize = cmdsSizePerBlit * alignedNumberOfBlts;
    auto expectedNotAlignedSize = cmdsSizePerBlit * notAlignedNumberOfBlts;

    auto alignedEstimatedSize = BlitCommandsHelper<FamilyType>::estimateBlitCommandsSize(
        alignedBltSize, csrDependencies, false, false, pClDevice->getRootDeviceEnvironment());
    auto notAlignedEstimatedSize = BlitCommandsHelper<FamilyType>::estimateBlitCommandsSize(
        notAlignedBltSize, csrDependencies, false, false, pClDevice->getRootDeviceEnvironment());

    EXPECT_EQ(expectedAlignedSize, alignedEstimatedSize);
    EXPECT_EQ(expectedNotAlignedSize, notAlignedEstimatedSize);
}

HWTEST_F(BcsTests, whenAskingForCmdSizeForMiFlushDwWithMemoryWriteThenReturnCorrectValue) {
    size_t waSize = EncodeMiFlushDW<FamilyType>::getMiFlushDwWaSize();
    size_t totalSize = EncodeMiFlushDW<FamilyType>::getMiFlushDwCmdSizeForDataWrite();
    constexpr size_t miFlushDwSize = sizeof(typename FamilyType::MI_FLUSH_DW);

    size_t additionalSize = UnitTestHelper<FamilyType>::additionalMiFlushDwRequired ? miFlushDwSize : 0;

    EXPECT_EQ(additionalSize, waSize);
    EXPECT_EQ(miFlushDwSize + additionalSize, totalSize);
}

HWTEST_F(BcsTests, givenBlitPropertiesContainerWhenExstimatingCommandsSizeThenCalculateForAllAttachedProperites) {
    const auto max2DBlitSize = BlitterConstants::maxBlitWidth * BlitterConstants::maxBlitHeight;
    const uint32_t numberOfBlts = 3;
    const size_t bltSize = (3 * max2DBlitSize);
    const uint32_t numberOfBlitOperations = 4;

    auto baseSize = EncodeMiFlushDW<FamilyType>::getMiFlushDwCmdSizeForDataWrite() + sizeof(typename FamilyType::MI_BATCH_BUFFER_END);
    constexpr size_t cmdsSizePerBlit = sizeof(typename FamilyType::XY_COPY_BLT) + sizeof(typename FamilyType::MI_ARB_CHECK);
    auto expectedBlitInstructionsSize = cmdsSizePerBlit * numberOfBlts;

    auto expectedAlignedSize = baseSize + MemorySynchronizationCommands<FamilyType>::getSizeForAdditonalSynchronization(pDevice->getHardwareInfo());

    BlitPropertiesContainer blitPropertiesContainer;
    for (uint32_t i = 0; i < numberOfBlitOperations; i++) {
        BlitProperties blitProperties;
        blitProperties.copySize = {bltSize, 1, 1};
        blitPropertiesContainer.push_back(blitProperties);

        expectedAlignedSize += expectedBlitInstructionsSize;
    }

    expectedAlignedSize = alignUp(expectedAlignedSize, MemoryConstants::cacheLineSize);

    auto alignedEstimatedSize = BlitCommandsHelper<FamilyType>::estimateBlitCommandsSize(
        blitPropertiesContainer, false, false, pClDevice->getRootDeviceEnvironment());

    EXPECT_EQ(expectedAlignedSize, alignedEstimatedSize);
}

HWTEST_F(BcsTests, givenBlitPropertiesContainerWhenExstimatingCommandsSizeForWriteReadBufferRectThenCalculateForAllAttachedProperites) {
    const auto max2DBlitSize = BlitterConstants::maxBlitWidth * BlitterConstants::maxBlitHeight;
    const Vec3<size_t> bltSize = {(3 * max2DBlitSize), 4, 2};
    const size_t numberOfBlts = 3 * bltSize.y * bltSize.z;
    const size_t numberOfBlitOperations = 4 * bltSize.y * bltSize.z;
    const size_t cmdsSizePerBlit = sizeof(typename FamilyType::XY_COPY_BLT) + sizeof(typename FamilyType::MI_ARB_CHECK);

    auto baseSize = EncodeMiFlushDW<FamilyType>::getMiFlushDwCmdSizeForDataWrite() + sizeof(typename FamilyType::MI_BATCH_BUFFER_END);
    auto expectedBlitInstructionsSize = cmdsSizePerBlit * numberOfBlts;

    auto expectedAlignedSize = baseSize + MemorySynchronizationCommands<FamilyType>::getSizeForAdditonalSynchronization(pDevice->getHardwareInfo());

    BlitPropertiesContainer blitPropertiesContainer;
    for (uint32_t i = 0; i < numberOfBlitOperations; i++) {
        BlitProperties blitProperties;
        blitProperties.copySize = bltSize;
        blitPropertiesContainer.push_back(blitProperties);

        expectedAlignedSize += expectedBlitInstructionsSize;
    }

    expectedAlignedSize = alignUp(expectedAlignedSize, MemoryConstants::cacheLineSize);

    auto alignedEstimatedSize = BlitCommandsHelper<FamilyType>::estimateBlitCommandsSize(
        blitPropertiesContainer, false, false, pClDevice->getRootDeviceEnvironment());

    EXPECT_EQ(expectedAlignedSize, alignedEstimatedSize);
}

HWTEST_F(BcsTests, givenTimestampPacketWriteRequestWhenEstimatingSizeForCommandsThenAddMiFlushDw) {
    constexpr size_t expectedBaseSize = sizeof(typename FamilyType::XY_COPY_BLT) + sizeof(typename FamilyType::MI_ARB_CHECK);

    auto expectedSizeWithTimestampPacketWrite = expectedBaseSize + EncodeMiFlushDW<FamilyType>::getMiFlushDwCmdSizeForDataWrite();
    auto expectedSizeWithoutTimestampPacketWrite = expectedBaseSize;

    auto estimatedSizeWithTimestampPacketWrite = BlitCommandsHelper<FamilyType>::estimateBlitCommandsSize(
        {1, 1, 1}, csrDependencies, true, false, pClDevice->getRootDeviceEnvironment());
    auto estimatedSizeWithoutTimestampPacketWrite = BlitCommandsHelper<FamilyType>::estimateBlitCommandsSize(
        {1, 1, 1}, csrDependencies, false, false, pClDevice->getRootDeviceEnvironment());

    EXPECT_EQ(expectedSizeWithTimestampPacketWrite, estimatedSizeWithTimestampPacketWrite);
    EXPECT_EQ(expectedSizeWithoutTimestampPacketWrite, estimatedSizeWithoutTimestampPacketWrite);
}

HWTEST_F(BcsTests, givenBltSizeAndCsrDependenciesWhenEstimatingCommandSizeThenAddAllRequiredCommands) {
    uint32_t numberOfBlts = 1;
    size_t numberNodesPerContainer = 5;
    auto &csr = pDevice->getUltCommandStreamReceiver<FamilyType>();

    MockTimestampPacketContainer timestamp0(*csr.getTimestampPacketAllocator(), numberNodesPerContainer);
    MockTimestampPacketContainer timestamp1(*csr.getTimestampPacketAllocator(), numberNodesPerContainer);
    csrDependencies.push_back(&timestamp0);
    csrDependencies.push_back(&timestamp1);

    constexpr size_t cmdsSizePerBlit = sizeof(typename FamilyType::XY_COPY_BLT) + sizeof(typename FamilyType::MI_ARB_CHECK);
    size_t expectedSize = (cmdsSizePerBlit * numberOfBlts) +
                          TimestampPacketHelper::getRequiredCmdStreamSize<FamilyType>(csrDependencies);

    auto estimatedSize = BlitCommandsHelper<FamilyType>::estimateBlitCommandsSize(
        {1, 1, 1}, csrDependencies, false, false, pClDevice->getRootDeviceEnvironment());

    EXPECT_EQ(expectedSize, estimatedSize);
}

HWTEST_F(BcsTests, givenBltSizeWithLeftoverWhenDispatchedThenProgramAllRequiredCommands) {
    using MI_FLUSH_DW = typename FamilyType::MI_FLUSH_DW;
    using MI_SEMAPHORE_WAIT = typename FamilyType::MI_SEMAPHORE_WAIT;
    constexpr auto max2DBlitSize = BlitterConstants::maxBlitWidth * BlitterConstants::maxBlitHeight;

    auto &csr = pDevice->getUltCommandStreamReceiver<FamilyType>();
    static_cast<OsAgnosticMemoryManager *>(csr.getMemoryManager())->turnOnFakingBigAllocations();

    uint32_t bltLeftover = 17;
    size_t bltSize = (2 * max2DBlitSize) + bltLeftover;
    uint32_t numberOfBlts = 3;

    cl_int retVal = CL_SUCCESS;
    auto buffer = clUniquePtr<Buffer>(Buffer::create(context.get(), CL_MEM_READ_WRITE, static_cast<size_t>(bltSize), nullptr, retVal));
    void *hostPtr = reinterpret_cast<void *>(0x12340000);

    uint32_t newTaskCount = 19;
    csr.taskCount = newTaskCount - 1;
    EXPECT_EQ(0u, csr.recursiveLockCounter.load());
    auto blitProperties = BlitProperties::constructPropertiesForReadWriteBuffer(BlitterConstants::BlitDirection::HostPtrToBuffer,
                                                                                csr, buffer->getGraphicsAllocation(pDevice->getRootDeviceIndex()), nullptr, hostPtr,
                                                                                buffer->getGraphicsAllocation(pDevice->getRootDeviceIndex())->getGpuAddress(), 0,
                                                                                0, 0, {bltSize, 1, 1}, 0, 0, 0, 0);

    blitBuffer(&csr, blitProperties, true);
    EXPECT_EQ(newTaskCount, csr.taskCount);
    EXPECT_EQ(newTaskCount, csr.latestFlushedTaskCount);
    EXPECT_EQ(newTaskCount, csr.latestSentTaskCount);
    EXPECT_EQ(newTaskCount, csr.latestSentTaskCountValueDuringFlush);
    EXPECT_EQ(1u, csr.recursiveLockCounter.load());

    HardwareParse hwParser;
    hwParser.parseCommands<FamilyType>(csr.commandStream);
    auto &cmdList = hwParser.cmdList;

    auto cmdIterator = find<typename FamilyType::XY_COPY_BLT *>(hwParser.cmdList.begin(), hwParser.cmdList.end());
    ASSERT_NE(hwParser.cmdList.end(), cmdIterator);

    for (uint32_t i = 0; i < numberOfBlts; i++) {
        auto bltCmd = genCmdCast<typename FamilyType::XY_COPY_BLT *>(*(cmdIterator++));
        EXPECT_NE(nullptr, bltCmd);

        uint32_t expectedWidth = static_cast<uint32_t>(BlitterConstants::maxBlitWidth);
        uint32_t expectedHeight = static_cast<uint32_t>(BlitterConstants::maxBlitHeight);
        if (i == (numberOfBlts - 1)) {
            expectedWidth = bltLeftover;
            expectedHeight = 1;
        }
        EXPECT_EQ(expectedWidth, bltCmd->getTransferWidth());
        EXPECT_EQ(expectedHeight, bltCmd->getTransferHeight());
        EXPECT_EQ(expectedWidth, bltCmd->getDestinationPitch());
        EXPECT_EQ(expectedWidth, bltCmd->getSourcePitch());

        auto miArbCheckCmd = genCmdCast<typename FamilyType::MI_ARB_CHECK *>(*(cmdIterator++));
        EXPECT_NE(nullptr, miArbCheckCmd);
        EXPECT_TRUE(memcmp(&FamilyType::cmdInitArbCheck, miArbCheckCmd, sizeof(typename FamilyType::MI_ARB_CHECK)) == 0);
    }

    if (UnitTestHelper<FamilyType>::isAdditionalSynchronizationRequired(pDevice->getHardwareInfo())) {
        if (UnitTestHelper<FamilyType>::isAdditionalMiSemaphoreWaitRequired(pDevice->getHardwareInfo())) {
            auto miSemaphoreWaitCmd = genCmdCast<MI_SEMAPHORE_WAIT *>(*(cmdIterator++));
            EXPECT_NE(nullptr, miSemaphoreWaitCmd);
            EXPECT_TRUE(UnitTestHelper<FamilyType>::isAdditionalMiSemaphoreWait(*miSemaphoreWaitCmd));
        } else {
            cmdIterator++;
        }
    }

    auto miFlushCmd = genCmdCast<MI_FLUSH_DW *>(*(cmdIterator++));

    if (UnitTestHelper<FamilyType>::additionalMiFlushDwRequired) {
        uint64_t gpuAddress = 0x0;
        uint64_t immData = 0;

        EXPECT_NE(nullptr, miFlushCmd);
        EXPECT_EQ(MI_FLUSH_DW::POST_SYNC_OPERATION_NO_WRITE, miFlushCmd->getPostSyncOperation());
        EXPECT_EQ(gpuAddress, miFlushCmd->getDestinationAddress());
        EXPECT_EQ(immData, miFlushCmd->getImmediateData());

        miFlushCmd = genCmdCast<MI_FLUSH_DW *>(*(cmdIterator++));
    }

    EXPECT_NE(cmdIterator, cmdList.end());
    EXPECT_EQ(MI_FLUSH_DW::POST_SYNC_OPERATION_WRITE_IMMEDIATE_DATA_QWORD, miFlushCmd->getPostSyncOperation());
    EXPECT_EQ(csr.getTagAllocation()->getGpuAddress(), miFlushCmd->getDestinationAddress());
    EXPECT_EQ(newTaskCount, miFlushCmd->getImmediateData());

    if (UnitTestHelper<FamilyType>::isAdditionalSynchronizationRequired(pDevice->getHardwareInfo())) {
        if (UnitTestHelper<FamilyType>::isAdditionalMiSemaphoreWaitRequired(pDevice->getHardwareInfo())) {
            auto miSemaphoreWaitCmd = genCmdCast<MI_SEMAPHORE_WAIT *>(*(cmdIterator++));
            EXPECT_NE(nullptr, miSemaphoreWaitCmd);
            EXPECT_TRUE(UnitTestHelper<FamilyType>::isAdditionalMiSemaphoreWait(*miSemaphoreWaitCmd));
        } else {
            cmdIterator++;
        }
    }

    EXPECT_NE(nullptr, genCmdCast<typename FamilyType::MI_BATCH_BUFFER_END *>(*(cmdIterator++)));

    // padding
    while (cmdIterator != cmdList.end()) {
        EXPECT_NE(nullptr, genCmdCast<typename FamilyType::MI_NOOP *>(*(cmdIterator++)));
    }
}

struct BcsTestParam {
    Vec3<size_t> copySize;

    Vec3<size_t> hostPtrOffset;
    Vec3<size_t> copyOffset;

    size_t dstRowPitch;
    size_t dstSlicePitch;
    size_t srcRowPitch;
    size_t srcSlicePitch;
} BlitterProperties[] = {
    {{(2 * BlitterConstants::maxBlitWidth * BlitterConstants::maxBlitHeight) + 17, 1, 1},
     {0, 1, 1},
     {BlitterConstants::maxBlitWidth, 1, 1},
     (2 * BlitterConstants::maxBlitWidth * BlitterConstants::maxBlitHeight) + 17,
     (2 * BlitterConstants::maxBlitWidth * BlitterConstants::maxBlitHeight) + 17,
     (2 * BlitterConstants::maxBlitWidth * BlitterConstants::maxBlitHeight) + 17,
     (2 * BlitterConstants::maxBlitWidth * BlitterConstants::maxBlitHeight) + 17},
    {{(2 * BlitterConstants::maxBlitWidth * BlitterConstants::maxBlitHeight) + 17, 2, 1},
     {BlitterConstants::maxBlitWidth, 2, 2},
     {BlitterConstants::maxBlitWidth, 1, 1},
     0,
     ((2 * BlitterConstants::maxBlitWidth * BlitterConstants::maxBlitHeight) + 17) * 2,
     0,
     ((2 * BlitterConstants::maxBlitWidth * BlitterConstants::maxBlitHeight) + 17) * 2},
    {{(2 * BlitterConstants::maxBlitWidth * BlitterConstants::maxBlitHeight) + 17, 1, 3},
     {BlitterConstants::maxBlitWidth, 2, 2},
     {BlitterConstants::maxBlitWidth, 1, 1},
     0,
     ((2 * BlitterConstants::maxBlitWidth * BlitterConstants::maxBlitHeight) + 17) * 2,
     0,
     ((2 * BlitterConstants::maxBlitWidth * BlitterConstants::maxBlitHeight) + 17) * 2},
    {{(2 * BlitterConstants::maxBlitWidth * BlitterConstants::maxBlitHeight) + 17, 4, 2},
     {0, 0, 0},
     {0, 0, 0},
     (2 * BlitterConstants::maxBlitWidth * BlitterConstants::maxBlitHeight) + 17,
     ((2 * BlitterConstants::maxBlitWidth * BlitterConstants::maxBlitHeight) + 17) * 4,
     (2 * BlitterConstants::maxBlitWidth * BlitterConstants::maxBlitHeight) + 17,
     ((2 * BlitterConstants::maxBlitWidth * BlitterConstants::maxBlitHeight) + 17) * 4},
    {{(2 * BlitterConstants::maxBlitWidth * BlitterConstants::maxBlitHeight) + 17, 3, 2},
     {BlitterConstants::maxBlitWidth, 2, 2},
     {BlitterConstants::maxBlitWidth, 1, 1},
     ((2 * BlitterConstants::maxBlitWidth * BlitterConstants::maxBlitHeight) + 17) + 2,
     (((2 * BlitterConstants::maxBlitWidth * BlitterConstants::maxBlitHeight) + 17) * 3) + 2,
     ((2 * BlitterConstants::maxBlitWidth * BlitterConstants::maxBlitHeight) + 17) + 2,
     (((2 * BlitterConstants::maxBlitWidth * BlitterConstants::maxBlitHeight) + 17) * 3) + 2}};

template <typename ParamType>
struct BcsDetaliedTests : public BcsTests,
                          public ::testing::WithParamInterface<ParamType> {
    void SetUp() override {
        BcsTests::SetUp();
    }

    void TearDown() override {
        BcsTests::TearDown();
    }
};

using BcsDetaliedTestsWithParams = BcsDetaliedTests<std::tuple<BcsTestParam, BlitterConstants::BlitDirection>>;

HWTEST_P(BcsDetaliedTestsWithParams, givenBltSizeWithLeftoverWhenDispatchedThenProgramAddresseForWriteReadBufferRect) {
    auto &csr = pDevice->getUltCommandStreamReceiver<FamilyType>();
    static_cast<OsAgnosticMemoryManager *>(csr.getMemoryManager())->turnOnFakingBigAllocations();

    uint32_t bltLeftover = 17;
    Vec3<size_t> bltSize = std::get<0>(GetParam()).copySize;

    size_t numberOfBltsForSingleBltSizeProgramm = 3;
    size_t totalNumberOfBits = numberOfBltsForSingleBltSizeProgramm * bltSize.y * bltSize.z;

    cl_int retVal = CL_SUCCESS;
    auto buffer = clUniquePtr<Buffer>(Buffer::create(context.get(), CL_MEM_READ_WRITE, static_cast<size_t>(8 * BlitterConstants::maxBlitWidth * BlitterConstants::maxBlitHeight), nullptr, retVal));
    void *hostPtr = reinterpret_cast<void *>(0x12340000);

    Vec3<size_t> hostPtrOffset = std::get<0>(GetParam()).hostPtrOffset;
    Vec3<size_t> copyOffset = std::get<0>(GetParam()).copyOffset;

    size_t dstRowPitch = std::get<0>(GetParam()).dstRowPitch;
    size_t dstSlicePitch = std::get<0>(GetParam()).dstSlicePitch;
    size_t srcRowPitch = std::get<0>(GetParam()).srcRowPitch;
    size_t srcSlicePitch = std::get<0>(GetParam()).srcSlicePitch;
    auto allocation = buffer->getGraphicsAllocation(pDevice->getRootDeviceIndex());

    auto blitProperties = BlitProperties::constructPropertiesForReadWriteBuffer(std::get<1>(GetParam()),     //blitDirection
                                                                                csr, allocation,             //commandStreamReceiver
                                                                                nullptr,                     //memObjAllocation
                                                                                hostPtr,                     //preallocatedHostAllocation
                                                                                allocation->getGpuAddress(), //memObjGpuVa
                                                                                0,                           //hostAllocGpuVa
                                                                                hostPtrOffset,               //hostPtrOffset
                                                                                copyOffset,                  //copyOffset
                                                                                bltSize,                     //copySize
                                                                                dstRowPitch,                 //hostRowPitch
                                                                                dstSlicePitch,               //hostSlicePitch
                                                                                srcRowPitch,                 //gpuRowPitch
                                                                                srcSlicePitch                //gpuSlicePitch
    );
    blitBuffer(&csr, blitProperties, true);

    HardwareParse hwParser;
    hwParser.parseCommands<FamilyType>(csr.commandStream);

    auto cmdIterator = find<typename FamilyType::XY_COPY_BLT *>(hwParser.cmdList.begin(), hwParser.cmdList.end());
    ASSERT_NE(hwParser.cmdList.end(), cmdIterator);

    uint64_t offset = 0;
    for (uint32_t i = 0; i < totalNumberOfBits; i++) {
        auto bltCmd = genCmdCast<typename FamilyType::XY_COPY_BLT *>(*(cmdIterator++));
        EXPECT_NE(nullptr, bltCmd);

        uint32_t expectedWidth = static_cast<uint32_t>(BlitterConstants::maxBlitWidth);
        uint32_t expectedHeight = static_cast<uint32_t>(BlitterConstants::maxBlitHeight);
        if (i % numberOfBltsForSingleBltSizeProgramm == numberOfBltsForSingleBltSizeProgramm - 1) {
            expectedWidth = bltLeftover;
            expectedHeight = 1;
        }

        if (i % numberOfBltsForSingleBltSizeProgramm == 0) {
            offset = 0;
        }

        auto rowIndex = (i / numberOfBltsForSingleBltSizeProgramm) % blitProperties.copySize.y;
        auto sliceIndex = i / (numberOfBltsForSingleBltSizeProgramm * blitProperties.copySize.y);

        auto expectedDstAddr = blitProperties.dstGpuAddress + blitProperties.dstOffset.x + offset +
                               blitProperties.dstOffset.y * blitProperties.dstRowPitch +
                               blitProperties.dstOffset.z * blitProperties.dstSlicePitch +
                               rowIndex * blitProperties.dstRowPitch +
                               sliceIndex * blitProperties.dstSlicePitch;
        auto expectedSrcAddr = blitProperties.srcGpuAddress + blitProperties.srcOffset.x + offset +
                               blitProperties.srcOffset.y * blitProperties.srcRowPitch +
                               blitProperties.srcOffset.z * blitProperties.srcSlicePitch +
                               rowIndex * blitProperties.srcRowPitch +
                               sliceIndex * blitProperties.srcSlicePitch;

        auto dstAddr = NEO::BlitCommandsHelper<FamilyType>::calculateBlitCommandDestinationBaseAddress(blitProperties, offset, rowIndex, sliceIndex);
        auto srcAddr = NEO::BlitCommandsHelper<FamilyType>::calculateBlitCommandSourceBaseAddress(blitProperties, offset, rowIndex, sliceIndex);

        EXPECT_EQ(dstAddr, expectedDstAddr);
        EXPECT_EQ(srcAddr, expectedSrcAddr);

        offset += (expectedWidth * expectedHeight);

        auto miArbCheckCmd = genCmdCast<typename FamilyType::MI_ARB_CHECK *>(*(cmdIterator++));
        EXPECT_NE(nullptr, miArbCheckCmd);
        EXPECT_TRUE(memcmp(&FamilyType::cmdInitArbCheck, miArbCheckCmd, sizeof(typename FamilyType::MI_ARB_CHECK)) == 0);
    }
}

HWTEST_P(BcsDetaliedTestsWithParams, givenBltSizeWithLeftoverWhenDispatchedThenProgramAllRequiredCommandsForWriteReadBufferRect) {
    auto &csr = pDevice->getUltCommandStreamReceiver<FamilyType>();
    static_cast<OsAgnosticMemoryManager *>(csr.getMemoryManager())->turnOnFakingBigAllocations();

    uint32_t bltLeftover = 17;
    Vec3<size_t> bltSize = std::get<0>(GetParam()).copySize;

    size_t numberOfBltsForSingleBltSizeProgramm = 3;
    size_t totalNumberOfBits = numberOfBltsForSingleBltSizeProgramm * bltSize.y * bltSize.z;

    cl_int retVal = CL_SUCCESS;
    auto buffer = clUniquePtr<Buffer>(Buffer::create(context.get(), CL_MEM_READ_WRITE, static_cast<size_t>(8 * BlitterConstants::maxBlitWidth * BlitterConstants::maxBlitHeight), nullptr, retVal));
    void *hostPtr = reinterpret_cast<void *>(0x12340000);

    Vec3<size_t> hostPtrOffset = std::get<0>(GetParam()).hostPtrOffset;
    Vec3<size_t> copyOffset = std::get<0>(GetParam()).copyOffset;

    size_t dstRowPitch = std::get<0>(GetParam()).dstRowPitch;
    size_t dstSlicePitch = std::get<0>(GetParam()).dstSlicePitch;
    size_t srcRowPitch = std::get<0>(GetParam()).srcRowPitch;
    size_t srcSlicePitch = std::get<0>(GetParam()).srcSlicePitch;
    auto allocation = buffer->getGraphicsAllocation(pDevice->getRootDeviceIndex());

    auto blitProperties = BlitProperties::constructPropertiesForReadWriteBuffer(std::get<1>(GetParam()),     //blitDirection
                                                                                csr, allocation,             //commandStreamReceiver
                                                                                nullptr,                     //memObjAllocation
                                                                                hostPtr,                     //preallocatedHostAllocation
                                                                                allocation->getGpuAddress(), //memObjGpuVa
                                                                                0,                           //hostAllocGpuVa
                                                                                hostPtrOffset,               //hostPtrOffset
                                                                                copyOffset,                  //copyOffset
                                                                                bltSize,                     //copySize
                                                                                dstRowPitch,                 //hostRowPitch
                                                                                dstSlicePitch,               //hostSlicePitch
                                                                                srcRowPitch,                 //gpuRowPitch
                                                                                srcSlicePitch                //gpuSlicePitch
    );
    blitBuffer(&csr, blitProperties, true);

    HardwareParse hwParser;
    hwParser.parseCommands<FamilyType>(csr.commandStream);

    auto cmdIterator = find<typename FamilyType::XY_COPY_BLT *>(hwParser.cmdList.begin(), hwParser.cmdList.end());
    ASSERT_NE(hwParser.cmdList.end(), cmdIterator);

    uint64_t offset = 0;
    for (uint32_t i = 0; i < totalNumberOfBits; i++) {
        auto bltCmd = genCmdCast<typename FamilyType::XY_COPY_BLT *>(*(cmdIterator++));
        EXPECT_NE(nullptr, bltCmd);

        uint32_t expectedWidth = static_cast<uint32_t>(BlitterConstants::maxBlitWidth);
        uint32_t expectedHeight = static_cast<uint32_t>(BlitterConstants::maxBlitHeight);
        if (i % numberOfBltsForSingleBltSizeProgramm == numberOfBltsForSingleBltSizeProgramm - 1) {
            expectedWidth = bltLeftover;
            expectedHeight = 1;
        }

        if (i % numberOfBltsForSingleBltSizeProgramm == 0) {
            offset = 0;
        }

        EXPECT_EQ(expectedWidth, bltCmd->getTransferWidth());
        EXPECT_EQ(expectedHeight, bltCmd->getTransferHeight());
        EXPECT_EQ(expectedWidth, bltCmd->getDestinationPitch());
        EXPECT_EQ(expectedWidth, bltCmd->getSourcePitch());

        auto rowIndex = (i / numberOfBltsForSingleBltSizeProgramm) % blitProperties.copySize.y;
        auto sliceIndex = i / (numberOfBltsForSingleBltSizeProgramm * blitProperties.copySize.y);

        auto dstAddr = NEO::BlitCommandsHelper<FamilyType>::calculateBlitCommandDestinationBaseAddress(blitProperties, offset, rowIndex, sliceIndex);
        auto srcAddr = NEO::BlitCommandsHelper<FamilyType>::calculateBlitCommandSourceBaseAddress(blitProperties, offset, rowIndex, sliceIndex);

        EXPECT_EQ(dstAddr, bltCmd->getDestinationBaseAddress());
        EXPECT_EQ(srcAddr, bltCmd->getSourceBaseAddress());

        offset += (expectedWidth * expectedHeight);

        auto miArbCheckCmd = genCmdCast<typename FamilyType::MI_ARB_CHECK *>(*(cmdIterator++));
        EXPECT_NE(nullptr, miArbCheckCmd);
        EXPECT_TRUE(memcmp(&FamilyType::cmdInitArbCheck, miArbCheckCmd, sizeof(typename FamilyType::MI_ARB_CHECK)) == 0);
    }
}

HWTEST_P(BcsDetaliedTestsWithParams, givenBltSizeWithLeftoverWhenDispatchedThenProgramAllRequiredCommandsForCopyBufferRect) {
    auto &csr = pDevice->getUltCommandStreamReceiver<FamilyType>();
    static_cast<OsAgnosticMemoryManager *>(csr.getMemoryManager())->turnOnFakingBigAllocations();

    uint32_t bltLeftover = 17;
    Vec3<size_t> bltSize = std::get<0>(GetParam()).copySize;

    size_t numberOfBltsForSingleBltSizeProgramm = 3;
    size_t totalNumberOfBits = numberOfBltsForSingleBltSizeProgramm * bltSize.y * bltSize.z;

    cl_int retVal = CL_SUCCESS;
    auto buffer1 = clUniquePtr<Buffer>(Buffer::create(context.get(), CL_MEM_READ_WRITE, static_cast<size_t>(8 * BlitterConstants::maxBlitWidth * BlitterConstants::maxBlitHeight), nullptr, retVal));

    Vec3<size_t> buffer1Offset = std::get<0>(GetParam()).hostPtrOffset;
    Vec3<size_t> buffer2Offset = std::get<0>(GetParam()).copyOffset;

    size_t buffer1RowPitch = std::get<0>(GetParam()).dstRowPitch;
    size_t buffer1SlicePitch = std::get<0>(GetParam()).dstSlicePitch;
    size_t buffer2RowPitch = std::get<0>(GetParam()).srcRowPitch;
    size_t buffer2SlicePitch = std::get<0>(GetParam()).srcSlicePitch;
    auto allocation = buffer1->getGraphicsAllocation(pDevice->getRootDeviceIndex());

    auto blitProperties = BlitProperties::constructPropertiesForCopyBuffer(allocation,        //dstAllocation
                                                                           allocation,        //srcAllocation
                                                                           buffer1Offset,     //dstOffset
                                                                           buffer2Offset,     //srcOffset
                                                                           bltSize,           //copySize
                                                                           buffer1RowPitch,   //srcRowPitch
                                                                           buffer1SlicePitch, //srcSlicePitch
                                                                           buffer2RowPitch,   //dstRowPitch
                                                                           buffer2SlicePitch  //dstSlicePitch
    );
    blitBuffer(&csr, blitProperties, true);

    HardwareParse hwParser;
    hwParser.parseCommands<FamilyType>(csr.commandStream);

    auto cmdIterator = find<typename FamilyType::XY_COPY_BLT *>(hwParser.cmdList.begin(), hwParser.cmdList.end());
    ASSERT_NE(hwParser.cmdList.end(), cmdIterator);

    uint64_t offset = 0;
    for (uint32_t i = 0; i < totalNumberOfBits; i++) {
        auto bltCmd = genCmdCast<typename FamilyType::XY_COPY_BLT *>(*(cmdIterator++));
        EXPECT_NE(nullptr, bltCmd);

        uint32_t expectedWidth = static_cast<uint32_t>(BlitterConstants::maxBlitWidth);
        uint32_t expectedHeight = static_cast<uint32_t>(BlitterConstants::maxBlitHeight);
        if (i % numberOfBltsForSingleBltSizeProgramm == numberOfBltsForSingleBltSizeProgramm - 1) {
            expectedWidth = bltLeftover;
            expectedHeight = 1;
        }

        if (i % numberOfBltsForSingleBltSizeProgramm == 0) {
            offset = 0;
        }

        EXPECT_EQ(expectedWidth, bltCmd->getTransferWidth());
        EXPECT_EQ(expectedHeight, bltCmd->getTransferHeight());
        EXPECT_EQ(expectedWidth, bltCmd->getDestinationPitch());
        EXPECT_EQ(expectedWidth, bltCmd->getSourcePitch());

        auto rowIndex = (i / numberOfBltsForSingleBltSizeProgramm) % blitProperties.copySize.y;
        auto sliceIndex = i / (numberOfBltsForSingleBltSizeProgramm * blitProperties.copySize.y);

        auto dstAddr = NEO::BlitCommandsHelper<FamilyType>::calculateBlitCommandDestinationBaseAddress(blitProperties, offset, rowIndex, sliceIndex);
        auto srcAddr = NEO::BlitCommandsHelper<FamilyType>::calculateBlitCommandSourceBaseAddress(blitProperties, offset, rowIndex, sliceIndex);

        EXPECT_EQ(dstAddr, bltCmd->getDestinationBaseAddress());
        EXPECT_EQ(srcAddr, bltCmd->getSourceBaseAddress());

        offset += (expectedWidth * expectedHeight);

        auto miArbCheckCmd = genCmdCast<typename FamilyType::MI_ARB_CHECK *>(*(cmdIterator++));
        EXPECT_NE(nullptr, miArbCheckCmd);
        EXPECT_TRUE(memcmp(&FamilyType::cmdInitArbCheck, miArbCheckCmd, sizeof(typename FamilyType::MI_ARB_CHECK)) == 0);
    }
}

INSTANTIATE_TEST_CASE_P(BcsDetaliedTest,
                        BcsDetaliedTestsWithParams,
                        ::testing::Combine(
                            ::testing::ValuesIn(BlitterProperties),
                            ::testing::Values(BlitterConstants::BlitDirection::HostPtrToBuffer, BlitterConstants::BlitDirection::BufferToHostPtr)));

HWTEST_F(BcsTests, givenCsrDependenciesWhenProgrammingCommandStreamThenAddSemaphoreAndAtomic) {
    auto &csr = pDevice->getUltCommandStreamReceiver<FamilyType>();

    cl_int retVal = CL_SUCCESS;
    auto buffer = clUniquePtr<Buffer>(Buffer::create(context.get(), CL_MEM_READ_WRITE, 1, nullptr, retVal));
    void *hostPtr = reinterpret_cast<void *>(0x12340000);
    uint32_t numberOfDependencyContainers = 2;
    size_t numberNodesPerContainer = 5;
    auto graphicsAllocation = buffer->getGraphicsAllocation(pDevice->getRootDeviceIndex());

    auto blitProperties = BlitProperties::constructPropertiesForReadWriteBuffer(BlitterConstants::BlitDirection::HostPtrToBuffer,
                                                                                csr, graphicsAllocation, nullptr, hostPtr,
                                                                                graphicsAllocation->getGpuAddress(), 0,
                                                                                0, 0, {1, 1, 1}, 0, 0, 0, 0);

    MockTimestampPacketContainer timestamp0(*csr.getTimestampPacketAllocator(), numberNodesPerContainer);
    MockTimestampPacketContainer timestamp1(*csr.getTimestampPacketAllocator(), numberNodesPerContainer);
    blitProperties.csrDependencies.push_back(&timestamp0);
    blitProperties.csrDependencies.push_back(&timestamp1);

    blitBuffer(&csr, blitProperties, true);

    HardwareParse hwParser;
    hwParser.parseCommands<FamilyType>(csr.commandStream);
    auto &cmdList = hwParser.cmdList;
    bool xyCopyBltCmdFound = false;
    bool dependenciesFound = false;

    for (auto cmdIterator = cmdList.begin(); cmdIterator != cmdList.end(); cmdIterator++) {
        if (genCmdCast<typename FamilyType::XY_COPY_BLT *>(*cmdIterator)) {
            xyCopyBltCmdFound = true;
            continue;
        }
        auto miSemaphore = genCmdCast<typename FamilyType::MI_SEMAPHORE_WAIT *>(*cmdIterator);
        if (miSemaphore) {
            if (UnitTestHelper<FamilyType>::isAdditionalMiSemaphoreWait(*miSemaphore)) {
                continue;
            }
            dependenciesFound = true;
            EXPECT_FALSE(xyCopyBltCmdFound);
            auto miAtomic = genCmdCast<typename FamilyType::MI_ATOMIC *>(*(++cmdIterator));
            EXPECT_NE(nullptr, miAtomic);

            for (uint32_t i = 1; i < numberOfDependencyContainers * numberNodesPerContainer; i++) {
                EXPECT_NE(nullptr, genCmdCast<typename FamilyType::MI_SEMAPHORE_WAIT *>(*(++cmdIterator)));
                EXPECT_NE(nullptr, genCmdCast<typename FamilyType::MI_ATOMIC *>(*(++cmdIterator)));
            }
        }
    }
    EXPECT_TRUE(xyCopyBltCmdFound);
    EXPECT_TRUE(dependenciesFound);
}

HWTEST_F(BcsTests, givenMultipleBlitPropertiesWhenDispatchingThenProgramCommandsInCorrectOrder) {
    auto &csr = pDevice->getUltCommandStreamReceiver<FamilyType>();

    cl_int retVal = CL_SUCCESS;
    auto buffer1 = clUniquePtr<Buffer>(Buffer::create(context.get(), CL_MEM_READ_WRITE, 1, nullptr, retVal));
    auto buffer2 = clUniquePtr<Buffer>(Buffer::create(context.get(), CL_MEM_READ_WRITE, 1, nullptr, retVal));
    void *hostPtr1 = reinterpret_cast<void *>(0x12340000);
    void *hostPtr2 = reinterpret_cast<void *>(0x12340000);
    auto graphicsAllocation1 = buffer1->getGraphicsAllocation(pDevice->getRootDeviceIndex());
    auto graphicsAllocation2 = buffer2->getGraphicsAllocation(pDevice->getRootDeviceIndex());

    auto blitProperties1 = BlitProperties::constructPropertiesForReadWriteBuffer(BlitterConstants::BlitDirection::HostPtrToBuffer,
                                                                                 csr, graphicsAllocation1, nullptr, hostPtr1,
                                                                                 graphicsAllocation1->getGpuAddress(), 0,
                                                                                 0, 0, {1, 1, 1}, 0, 0, 0, 0);
    auto blitProperties2 = BlitProperties::constructPropertiesForReadWriteBuffer(BlitterConstants::BlitDirection::HostPtrToBuffer,
                                                                                 csr, graphicsAllocation2, nullptr, hostPtr2,
                                                                                 graphicsAllocation2->getGpuAddress(), 0,
                                                                                 0, 0, {1, 1, 1}, 0, 0, 0, 0);

    MockTimestampPacketContainer timestamp1(*csr.getTimestampPacketAllocator(), 1);
    MockTimestampPacketContainer timestamp2(*csr.getTimestampPacketAllocator(), 1);
    blitProperties1.csrDependencies.push_back(&timestamp1);
    blitProperties2.csrDependencies.push_back(&timestamp2);

    BlitPropertiesContainer blitPropertiesContainer;
    blitPropertiesContainer.push_back(blitProperties1);
    blitPropertiesContainer.push_back(blitProperties2);

    csr.blitBuffer(blitPropertiesContainer, true, false);

    HardwareParse hwParser;
    hwParser.parseCommands<FamilyType>(csr.commandStream);
    auto &cmdList = hwParser.cmdList;

    uint32_t xyCopyBltCmdFound = 0;
    uint32_t dependenciesFound = 0;

    for (auto cmdIterator = cmdList.begin(); cmdIterator != cmdList.end(); cmdIterator++) {
        if (genCmdCast<typename FamilyType::XY_COPY_BLT *>(*cmdIterator)) {
            xyCopyBltCmdFound++;
            EXPECT_EQ(xyCopyBltCmdFound, dependenciesFound);

            continue;
        }
        auto miSemaphore = genCmdCast<typename FamilyType::MI_SEMAPHORE_WAIT *>(*cmdIterator);
        if (miSemaphore) {
            if (UnitTestHelper<FamilyType>::isAdditionalMiSemaphoreWait(*miSemaphore)) {
                continue;
            }
            dependenciesFound++;
            EXPECT_EQ(xyCopyBltCmdFound, dependenciesFound - 1);
        }
    }
    EXPECT_EQ(2u, xyCopyBltCmdFound);
    EXPECT_EQ(2u, dependenciesFound);
}

HWTEST_F(BcsTests, givenProfilingEnabledWhenBlitBufferThenCommandBufferIsConstructedProperly) {
    auto bcsOsContext = std::unique_ptr<OsContext>(OsContext::create(nullptr, 0, pDevice->getDeviceBitfield(), aub_stream::ENGINE_BCS, PreemptionMode::Disabled,
                                                                     false, false, false));
    auto bcsCsr = std::make_unique<UltCommandStreamReceiver<FamilyType>>(*pDevice->getExecutionEnvironment(), pDevice->getRootDeviceIndex());
    bcsCsr->setupContext(*bcsOsContext);
    bcsCsr->initializeTagAllocation();

    cl_int retVal = CL_SUCCESS;
    auto buffer = clUniquePtr<Buffer>(Buffer::create(context.get(), CL_MEM_READ_WRITE, 1, nullptr, retVal));
    void *hostPtr = reinterpret_cast<void *>(0x12340000);
    auto graphicsAllocation = buffer->getGraphicsAllocation(pDevice->getRootDeviceIndex());

    auto blitProperties = BlitProperties::constructPropertiesForReadWriteBuffer(BlitterConstants::BlitDirection::HostPtrToBuffer,
                                                                                *bcsCsr, graphicsAllocation, nullptr, hostPtr,
                                                                                graphicsAllocation->getGpuAddress(), 0,
                                                                                0, 0, {1, 1, 1}, 0, 0, 0, 0);

    MockTimestampPacketContainer timestamp(*bcsCsr->getTimestampPacketAllocator(), 1u);
    blitProperties.outputTimestampPacket = timestamp.getNode(0);

    BlitPropertiesContainer blitPropertiesContainer;
    blitPropertiesContainer.push_back(blitProperties);

    bcsCsr->blitBuffer(blitPropertiesContainer, false, true);

    HardwareParse hwParser;
    hwParser.parseCommands<FamilyType>(bcsCsr->commandStream);
    auto &cmdList = hwParser.cmdList;

    auto cmdIterator = find<typename FamilyType::MI_STORE_REGISTER_MEM *>(cmdList.begin(), cmdList.end());
    ASSERT_NE(cmdList.end(), cmdIterator);
    cmdIterator = find<typename FamilyType::MI_STORE_REGISTER_MEM *>(++cmdIterator, cmdList.end());
    ASSERT_NE(cmdList.end(), cmdIterator);
    cmdIterator = find<typename FamilyType::XY_COPY_BLT *>(++cmdIterator, cmdList.end());
    ASSERT_NE(cmdList.end(), cmdIterator);
    cmdIterator = find<typename FamilyType::MI_STORE_REGISTER_MEM *>(++cmdIterator, cmdList.end());
    ASSERT_NE(cmdList.end(), cmdIterator);
    cmdIterator = find<typename FamilyType::MI_STORE_REGISTER_MEM *>(++cmdIterator, cmdList.end());
    ASSERT_NE(cmdList.end(), cmdIterator);
}

HWTEST_F(BcsTests, givenInputAllocationsWhenBlitDispatchedThenMakeAllAllocationsResident) {
    auto &csr = pDevice->getUltCommandStreamReceiver<FamilyType>();
    csr.storeMakeResidentAllocations = true;

    cl_int retVal = CL_SUCCESS;
    auto buffer1 = clUniquePtr<Buffer>(Buffer::create(context.get(), CL_MEM_READ_WRITE, 1, nullptr, retVal));
    auto buffer2 = clUniquePtr<Buffer>(Buffer::create(context.get(), CL_MEM_READ_WRITE, 1, nullptr, retVal));
    void *hostPtr1 = reinterpret_cast<void *>(0x12340000);
    void *hostPtr2 = reinterpret_cast<void *>(0x43210000);

    EXPECT_EQ(0u, csr.makeSurfacePackNonResidentCalled);
    auto graphicsAllocation1 = buffer1->getGraphicsAllocation(pDevice->getRootDeviceIndex());
    auto graphicsAllocation2 = buffer2->getGraphicsAllocation(pDevice->getRootDeviceIndex());

    auto blitProperties1 = BlitProperties::constructPropertiesForReadWriteBuffer(BlitterConstants::BlitDirection::HostPtrToBuffer,
                                                                                 csr, graphicsAllocation1, nullptr, hostPtr1,
                                                                                 graphicsAllocation1->getGpuAddress(), 0,
                                                                                 0, 0, {1, 1, 1}, 0, 0, 0, 0);

    auto blitProperties2 = BlitProperties::constructPropertiesForReadWriteBuffer(BlitterConstants::BlitDirection::HostPtrToBuffer,
                                                                                 csr, graphicsAllocation2, nullptr, hostPtr2,
                                                                                 graphicsAllocation2->getGpuAddress(), 0,
                                                                                 0, 0, {1, 1, 1}, 0, 0, 0, 0);

    BlitPropertiesContainer blitPropertiesContainer;
    blitPropertiesContainer.push_back(blitProperties1);
    blitPropertiesContainer.push_back(blitProperties2);

    csr.blitBuffer(blitPropertiesContainer, false, false);

    EXPECT_TRUE(csr.isMadeResident(graphicsAllocation1));
    EXPECT_TRUE(csr.isMadeResident(graphicsAllocation2));
    EXPECT_TRUE(csr.isMadeResident(csr.getTagAllocation()));
    EXPECT_EQ(1u, csr.makeSurfacePackNonResidentCalled);

    EXPECT_EQ(csr.globalFenceAllocation ? 6u : 5u, csr.makeResidentAllocations.size());
}

HWTEST_F(BcsTests, givenFenceAllocationIsRequiredWhenBlitDispatchedThenMakeAllAllocationsResident) {
    RAIIHwHelperFactory<MockHwHelperWithFenceAllocation<FamilyType>> hwHelperBackup{pDevice->getHardwareInfo().platform.eRenderCoreFamily};

    auto bcsOsContext = std::unique_ptr<OsContext>(OsContext::create(nullptr, 0, pDevice->getDeviceBitfield(), aub_stream::ENGINE_BCS, PreemptionMode::Disabled,
                                                                     false, false, false));
    auto bcsCsr = std::make_unique<UltCommandStreamReceiver<FamilyType>>(*pDevice->getExecutionEnvironment(), pDevice->getRootDeviceIndex());
    bcsCsr->setupContext(*bcsOsContext);
    bcsCsr->initializeTagAllocation();
    bcsCsr->createGlobalFenceAllocation();
    bcsCsr->storeMakeResidentAllocations = true;

    cl_int retVal = CL_SUCCESS;
    auto buffer1 = clUniquePtr<Buffer>(Buffer::create(context.get(), CL_MEM_READ_WRITE, 1, nullptr, retVal));
    auto buffer2 = clUniquePtr<Buffer>(Buffer::create(context.get(), CL_MEM_READ_WRITE, 1, nullptr, retVal));
    void *hostPtr1 = reinterpret_cast<void *>(0x12340000);
    void *hostPtr2 = reinterpret_cast<void *>(0x43210000);

    EXPECT_EQ(0u, bcsCsr->makeSurfacePackNonResidentCalled);
    auto graphicsAllocation1 = buffer1->getGraphicsAllocation(pDevice->getRootDeviceIndex());
    auto graphicsAllocation2 = buffer2->getGraphicsAllocation(pDevice->getRootDeviceIndex());

    auto blitProperties1 = BlitProperties::constructPropertiesForReadWriteBuffer(BlitterConstants::BlitDirection::HostPtrToBuffer,
                                                                                 *bcsCsr, graphicsAllocation1, nullptr, hostPtr1,
                                                                                 graphicsAllocation1->getGpuAddress(), 0,
                                                                                 0, 0, {1, 1, 1}, 0, 0, 0, 0);

    auto blitProperties2 = BlitProperties::constructPropertiesForReadWriteBuffer(BlitterConstants::BlitDirection::HostPtrToBuffer,
                                                                                 *bcsCsr, graphicsAllocation2, nullptr, hostPtr2,
                                                                                 graphicsAllocation2->getGpuAddress(), 0,
                                                                                 0, 0, {1, 1, 1}, 0, 0, 0, 0);

    BlitPropertiesContainer blitPropertiesContainer;
    blitPropertiesContainer.push_back(blitProperties1);
    blitPropertiesContainer.push_back(blitProperties2);

    bcsCsr->blitBuffer(blitPropertiesContainer, false, false);

    EXPECT_TRUE(bcsCsr->isMadeResident(graphicsAllocation1));
    EXPECT_TRUE(bcsCsr->isMadeResident(graphicsAllocation2));
    EXPECT_TRUE(bcsCsr->isMadeResident(bcsCsr->getTagAllocation()));
    EXPECT_TRUE(bcsCsr->isMadeResident(bcsCsr->globalFenceAllocation));
    EXPECT_EQ(1u, bcsCsr->makeSurfacePackNonResidentCalled);

    EXPECT_EQ(6u, bcsCsr->makeResidentAllocations.size());
}

HWTEST_F(BcsTests, givenBufferWhenBlitCalledThenFlushCommandBuffer) {
    auto &csr = pDevice->getUltCommandStreamReceiver<FamilyType>();
    csr.recordFlusheBatchBuffer = true;

    cl_int retVal = CL_SUCCESS;
    auto buffer = clUniquePtr<Buffer>(Buffer::create(context.get(), CL_MEM_READ_WRITE, 1, nullptr, retVal));
    void *hostPtr = reinterpret_cast<void *>(0x12340000);
    auto graphicsAllocation = buffer->getGraphicsAllocation(pDevice->getRootDeviceIndex());

    auto &commandStream = csr.getCS(MemoryConstants::pageSize);
    size_t commandStreamOffset = 4;
    commandStream.getSpace(commandStreamOffset);

    uint32_t newTaskCount = 17;
    csr.taskCount = newTaskCount - 1;

    auto blitProperties = BlitProperties::constructPropertiesForReadWriteBuffer(BlitterConstants::BlitDirection::HostPtrToBuffer,
                                                                                csr, graphicsAllocation, nullptr, hostPtr,
                                                                                graphicsAllocation->getGpuAddress(), 0,
                                                                                0, 0, {1, 1, 1}, 0, 0, 0, 0);

    blitBuffer(&csr, blitProperties, true);

    EXPECT_EQ(commandStream.getGraphicsAllocation(), csr.latestFlushedBatchBuffer.commandBufferAllocation);
    EXPECT_EQ(commandStreamOffset, csr.latestFlushedBatchBuffer.startOffset);
    EXPECT_EQ(0u, csr.latestFlushedBatchBuffer.chainedBatchBufferStartOffset);
    EXPECT_EQ(nullptr, csr.latestFlushedBatchBuffer.chainedBatchBuffer);
    EXPECT_FALSE(csr.latestFlushedBatchBuffer.requiresCoherency);
    EXPECT_FALSE(csr.latestFlushedBatchBuffer.low_priority);
    EXPECT_EQ(QueueThrottle::MEDIUM, csr.latestFlushedBatchBuffer.throttle);
    EXPECT_EQ(commandStream.getUsed(), csr.latestFlushedBatchBuffer.usedSize);
    EXPECT_EQ(&commandStream, csr.latestFlushedBatchBuffer.stream);

    EXPECT_EQ(newTaskCount, csr.latestWaitForCompletionWithTimeoutTaskCount.load());
}

HWTEST_F(BcsTests, whenBlitFromHostPtrCalledThenCallWaitWithKmdFallback) {
    class MyMockCsr : public UltCommandStreamReceiver<FamilyType> {
      public:
        using UltCommandStreamReceiver<FamilyType>::UltCommandStreamReceiver;

        void waitForTaskCountWithKmdNotifyFallback(uint32_t taskCountToWait, FlushStamp flushStampToWait,
                                                   bool useQuickKmdSleep, bool forcePowerSavingMode) override {
            waitForTaskCountWithKmdNotifyFallbackCalled++;
            taskCountToWaitPassed = taskCountToWait;
            flushStampToWaitPassed = flushStampToWait;
            useQuickKmdSleepPassed = useQuickKmdSleep;
            forcePowerSavingModePassed = forcePowerSavingMode;
        }

        uint32_t taskCountToWaitPassed = 0;
        FlushStamp flushStampToWaitPassed = 0;
        bool useQuickKmdSleepPassed = false;
        bool forcePowerSavingModePassed = false;
        uint32_t waitForTaskCountWithKmdNotifyFallbackCalled = 0;
    };

    auto myMockCsr = std::make_unique<::testing::NiceMock<MyMockCsr>>(*pDevice->getExecutionEnvironment(), pDevice->getRootDeviceIndex());
    auto &bcsOsContext = pDevice->getUltCommandStreamReceiver<FamilyType>().getOsContext();
    myMockCsr->initializeTagAllocation();
    myMockCsr->setupContext(bcsOsContext);

    cl_int retVal = CL_SUCCESS;
    auto buffer = clUniquePtr<Buffer>(Buffer::create(context.get(), CL_MEM_READ_WRITE, 1, nullptr, retVal));
    void *hostPtr = reinterpret_cast<void *>(0x12340000);
    auto graphicsAllocation = buffer->getGraphicsAllocation(pDevice->getRootDeviceIndex());

    auto blitProperties = BlitProperties::constructPropertiesForReadWriteBuffer(BlitterConstants::BlitDirection::HostPtrToBuffer,
                                                                                *myMockCsr, graphicsAllocation, nullptr,
                                                                                hostPtr,
                                                                                graphicsAllocation->getGpuAddress(), 0,
                                                                                0, 0, {1, 1, 1}, 0, 0, 0, 0);

    blitBuffer(myMockCsr.get(), blitProperties, false);

    EXPECT_EQ(0u, myMockCsr->waitForTaskCountWithKmdNotifyFallbackCalled);

    blitBuffer(myMockCsr.get(), blitProperties, true);

    EXPECT_EQ(1u, myMockCsr->waitForTaskCountWithKmdNotifyFallbackCalled);
    EXPECT_EQ(myMockCsr->taskCount, myMockCsr->taskCountToWaitPassed);
    EXPECT_EQ(myMockCsr->flushStamp->peekStamp(), myMockCsr->flushStampToWaitPassed);
    EXPECT_FALSE(myMockCsr->useQuickKmdSleepPassed);
    EXPECT_FALSE(myMockCsr->forcePowerSavingModePassed);
}

HWTEST_F(BcsTests, whenBlitFromHostPtrCalledThenCleanTemporaryAllocations) {
    auto &bcsCsr = pDevice->getUltCommandStreamReceiver<FamilyType>();
    auto mockInternalAllocationsStorage = new MockInternalAllocationStorage(bcsCsr);
    bcsCsr.internalAllocationStorage.reset(mockInternalAllocationsStorage);

    cl_int retVal = CL_SUCCESS;
    auto buffer = clUniquePtr<Buffer>(Buffer::create(context.get(), CL_MEM_READ_WRITE, 1, nullptr, retVal));
    void *hostPtr = reinterpret_cast<void *>(0x12340000);
    auto graphicsAllocation = buffer->getGraphicsAllocation(pDevice->getRootDeviceIndex());

    bcsCsr.taskCount = 17;

    EXPECT_EQ(0u, mockInternalAllocationsStorage->cleanAllocationsCalled);

    auto blitProperties = BlitProperties::constructPropertiesForReadWriteBuffer(BlitterConstants::BlitDirection::HostPtrToBuffer,
                                                                                bcsCsr, graphicsAllocation, nullptr, hostPtr,
                                                                                graphicsAllocation->getGpuAddress(), 0,
                                                                                0, 0, {1, 1, 1}, 0, 0, 0, 0);

    blitBuffer(&bcsCsr, blitProperties, false);

    EXPECT_EQ(0u, mockInternalAllocationsStorage->cleanAllocationsCalled);

    blitBuffer(&bcsCsr, blitProperties, true);

    EXPECT_EQ(1u, mockInternalAllocationsStorage->cleanAllocationsCalled);
    EXPECT_EQ(bcsCsr.taskCount, mockInternalAllocationsStorage->lastCleanAllocationsTaskCount);
    EXPECT_TRUE(TEMPORARY_ALLOCATION == mockInternalAllocationsStorage->lastCleanAllocationUsage);
}

HWTEST_F(BcsTests, givenBufferWhenBlitOperationCalledThenProgramCorrectGpuAddresses) {
    auto &csr = pDevice->getUltCommandStreamReceiver<FamilyType>();

    cl_int retVal = CL_SUCCESS;
    auto buffer1 = clUniquePtr<Buffer>(Buffer::create(context.get(), CL_MEM_READ_WRITE, 100, nullptr, retVal));
    auto buffer2 = clUniquePtr<Buffer>(Buffer::create(context.get(), CL_MEM_READ_WRITE, 100, nullptr, retVal));
    auto graphicsAllocation1 = buffer1->getGraphicsAllocation(pDevice->getRootDeviceIndex());
    auto graphicsAllocation2 = buffer2->getGraphicsAllocation(pDevice->getRootDeviceIndex());

    void *hostPtr = reinterpret_cast<void *>(0x12340000);
    const size_t hostPtrOffset = 0x1234;

    const size_t subBuffer1Offset = 0x23;
    cl_buffer_region subBufferRegion1 = {subBuffer1Offset, 1};
    auto subBuffer1 = clUniquePtr<Buffer>(buffer1->createSubBuffer(CL_MEM_READ_WRITE, 0, &subBufferRegion1, retVal));

    {
        // from hostPtr
        HardwareParse hwParser;
        auto blitProperties = BlitProperties::constructPropertiesForReadWriteBuffer(BlitterConstants::BlitDirection::HostPtrToBuffer,
                                                                                    csr, graphicsAllocation1,
                                                                                    nullptr, hostPtr,
                                                                                    graphicsAllocation1->getGpuAddress() +
                                                                                        subBuffer1->getOffset(),
                                                                                    0, {hostPtrOffset, 0, 0}, 0, {1, 1, 1}, 0, 0, 0, 0);

        blitBuffer(&csr, blitProperties, true);

        hwParser.parseCommands<FamilyType>(csr.commandStream);

        auto cmdIterator = find<typename FamilyType::XY_COPY_BLT *>(hwParser.cmdList.begin(), hwParser.cmdList.end());
        ASSERT_NE(hwParser.cmdList.end(), cmdIterator);

        auto bltCmd = genCmdCast<typename FamilyType::XY_COPY_BLT *>(*cmdIterator);
        ASSERT_NE(nullptr, bltCmd);
        if (pDevice->isFullRangeSvm()) {
            EXPECT_EQ(reinterpret_cast<uint64_t>(ptrOffset(hostPtr, hostPtrOffset)), bltCmd->getSourceBaseAddress());
        }
        EXPECT_EQ(graphicsAllocation1->getGpuAddress() + subBuffer1Offset, bltCmd->getDestinationBaseAddress());
    }
    {
        // to hostPtr
        HardwareParse hwParser;
        auto offset = csr.commandStream.getUsed();
        auto blitProperties = BlitProperties::constructPropertiesForReadWriteBuffer(BlitterConstants::BlitDirection::BufferToHostPtr,
                                                                                    csr, graphicsAllocation1,
                                                                                    nullptr, hostPtr,
                                                                                    graphicsAllocation1->getGpuAddress() +
                                                                                        subBuffer1->getOffset(),
                                                                                    0, {hostPtrOffset, 0, 0}, 0, {1, 1, 1}, 0, 0, 0, 0);

        blitBuffer(&csr, blitProperties, true);

        hwParser.parseCommands<FamilyType>(csr.commandStream, offset);

        auto cmdIterator = find<typename FamilyType::XY_COPY_BLT *>(hwParser.cmdList.begin(), hwParser.cmdList.end());
        ASSERT_NE(hwParser.cmdList.end(), cmdIterator);

        auto bltCmd = genCmdCast<typename FamilyType::XY_COPY_BLT *>(*cmdIterator);
        ASSERT_NE(nullptr, bltCmd);
        if (pDevice->isFullRangeSvm()) {
            EXPECT_EQ(reinterpret_cast<uint64_t>(ptrOffset(hostPtr, hostPtrOffset)), bltCmd->getDestinationBaseAddress());
        }
        EXPECT_EQ(graphicsAllocation1->getGpuAddress() + subBuffer1Offset, bltCmd->getSourceBaseAddress());
    }

    {
        // Buffer to Buffer
        HardwareParse hwParser;
        auto offset = csr.commandStream.getUsed();
        auto blitProperties = BlitProperties::constructPropertiesForCopyBuffer(graphicsAllocation1,
                                                                               graphicsAllocation2, 0, 0, {1, 1, 1}, 0, 0, 0, 0);

        blitBuffer(&csr, blitProperties, true);

        hwParser.parseCommands<FamilyType>(csr.commandStream, offset);

        auto cmdIterator = find<typename FamilyType::XY_COPY_BLT *>(hwParser.cmdList.begin(), hwParser.cmdList.end());
        ASSERT_NE(hwParser.cmdList.end(), cmdIterator);

        auto bltCmd = genCmdCast<typename FamilyType::XY_COPY_BLT *>(*cmdIterator);
        ASSERT_NE(nullptr, bltCmd);
        EXPECT_EQ(graphicsAllocation1->getGpuAddress(), bltCmd->getDestinationBaseAddress());
        EXPECT_EQ(graphicsAllocation2->getGpuAddress(), bltCmd->getSourceBaseAddress());
    }

    {
        // Buffer to Buffer - with object offset
        const size_t subBuffer2Offset = 0x20;
        cl_buffer_region subBufferRegion2 = {subBuffer2Offset, 1};
        auto subBuffer2 = clUniquePtr<Buffer>(buffer2->createSubBuffer(CL_MEM_READ_WRITE, 0, &subBufferRegion2, retVal));

        BuiltinOpParams builtinOpParams = {};
        builtinOpParams.dstMemObj = subBuffer2.get();
        builtinOpParams.srcMemObj = subBuffer1.get();
        builtinOpParams.size.x = 1;

        auto blitProperties = ClBlitProperties::constructProperties(BlitterConstants::BlitDirection::BufferToBuffer, csr, builtinOpParams);

        auto offset = csr.commandStream.getUsed();
        blitBuffer(&csr, blitProperties, true);

        HardwareParse hwParser;
        hwParser.parseCommands<FamilyType>(csr.commandStream, offset);

        auto cmdIterator = find<typename FamilyType::XY_COPY_BLT *>(hwParser.cmdList.begin(), hwParser.cmdList.end());
        ASSERT_NE(hwParser.cmdList.end(), cmdIterator);

        auto bltCmd = genCmdCast<typename FamilyType::XY_COPY_BLT *>(*cmdIterator);
        EXPECT_NE(nullptr, bltCmd);
        EXPECT_EQ(graphicsAllocation2->getGpuAddress() + subBuffer2Offset, bltCmd->getDestinationBaseAddress());
        EXPECT_EQ(graphicsAllocation1->getGpuAddress() + subBuffer1Offset, bltCmd->getSourceBaseAddress());
    }
}

HWTEST_F(BcsTests, givenMapAllocationWhenDispatchReadWriteOperationThenSetValidGpuAddress) {
    auto &csr = pDevice->getUltCommandStreamReceiver<FamilyType>();
    auto memoryManager = csr.getMemoryManager();

    AllocationProperties properties{csr.getRootDeviceIndex(), false, 1234, GraphicsAllocation::AllocationType::MAP_ALLOCATION, false, pDevice->getDeviceBitfield()};
    GraphicsAllocation *mapAllocation = memoryManager->allocateGraphicsMemoryWithProperties(properties, reinterpret_cast<void *>(0x12340000));

    auto mapAllocationOffset = 0x1234;
    auto mapPtr = reinterpret_cast<void *>(mapAllocation->getGpuAddress() + mapAllocationOffset);

    cl_int retVal = CL_SUCCESS;
    auto buffer = clUniquePtr<Buffer>(Buffer::create(context.get(), CL_MEM_READ_WRITE, 100, nullptr, retVal));
    auto graphicsAllocation = buffer->getGraphicsAllocation(pDevice->getRootDeviceIndex());

    const size_t hostPtrOffset = 0x1234;

    {
        // from hostPtr
        HardwareParse hwParser;
        auto blitProperties = BlitProperties::constructPropertiesForReadWriteBuffer(BlitterConstants::BlitDirection::HostPtrToBuffer,
                                                                                    csr, graphicsAllocation,
                                                                                    mapAllocation, mapPtr,
                                                                                    graphicsAllocation->getGpuAddress(),
                                                                                    castToUint64(mapPtr),
                                                                                    {hostPtrOffset, 0, 0}, 0, {1, 1, 1}, 0, 0, 0, 0);

        blitBuffer(&csr, blitProperties, true);

        hwParser.parseCommands<FamilyType>(csr.commandStream);

        auto cmdIterator = find<typename FamilyType::XY_COPY_BLT *>(hwParser.cmdList.begin(), hwParser.cmdList.end());
        ASSERT_NE(hwParser.cmdList.end(), cmdIterator);

        auto bltCmd = genCmdCast<typename FamilyType::XY_COPY_BLT *>(*cmdIterator);
        EXPECT_NE(nullptr, bltCmd);
        if (pDevice->isFullRangeSvm()) {
            EXPECT_EQ(reinterpret_cast<uint64_t>(ptrOffset(mapPtr, hostPtrOffset)), bltCmd->getSourceBaseAddress());
        }
        EXPECT_EQ(graphicsAllocation->getGpuAddress(), bltCmd->getDestinationBaseAddress());
    }

    {
        // to hostPtr
        HardwareParse hwParser;
        auto offset = csr.commandStream.getUsed();
        auto blitProperties = BlitProperties::constructPropertiesForReadWriteBuffer(BlitterConstants::BlitDirection::BufferToHostPtr,
                                                                                    csr, graphicsAllocation,
                                                                                    mapAllocation, mapPtr,
                                                                                    graphicsAllocation->getGpuAddress(),
                                                                                    castToUint64(mapPtr), {hostPtrOffset, 0, 0}, 0, {1, 1, 1}, 0, 0, 0, 0);

        blitBuffer(&csr, blitProperties, true);

        hwParser.parseCommands<FamilyType>(csr.commandStream, offset);

        auto cmdIterator = find<typename FamilyType::XY_COPY_BLT *>(hwParser.cmdList.begin(), hwParser.cmdList.end());
        ASSERT_NE(hwParser.cmdList.end(), cmdIterator);

        auto bltCmd = genCmdCast<typename FamilyType::XY_COPY_BLT *>(*cmdIterator);
        EXPECT_NE(nullptr, bltCmd);
        if (pDevice->isFullRangeSvm()) {
            EXPECT_EQ(reinterpret_cast<uint64_t>(ptrOffset(mapPtr, hostPtrOffset)), bltCmd->getDestinationBaseAddress());
        }
        EXPECT_EQ(graphicsAllocation->getGpuAddress(), bltCmd->getSourceBaseAddress());
    }

    {
        // bufferRect to hostPtr
        HardwareParse hwParser;
        auto offset = csr.commandStream.getUsed();
        auto blitProperties = BlitProperties::constructPropertiesForReadWriteBuffer(BlitterConstants::BlitDirection::BufferToHostPtr,
                                                                                    csr, graphicsAllocation,
                                                                                    mapAllocation, mapPtr,
                                                                                    graphicsAllocation->getGpuAddress(),
                                                                                    castToUint64(mapPtr), {hostPtrOffset, 0, 0}, 0, {4, 2, 1}, 0, 0, 0, 0);

        blitBuffer(&csr, blitProperties, true);

        hwParser.parseCommands<FamilyType>(csr.commandStream, offset);

        auto cmdIterator = find<typename FamilyType::XY_COPY_BLT *>(hwParser.cmdList.begin(), hwParser.cmdList.end());
        ASSERT_NE(hwParser.cmdList.end(), cmdIterator);

        auto bltCmd = genCmdCast<typename FamilyType::XY_COPY_BLT *>(*cmdIterator);
        EXPECT_NE(nullptr, bltCmd);
        if (pDevice->isFullRangeSvm()) {
            EXPECT_EQ(reinterpret_cast<uint64_t>(ptrOffset(mapPtr, hostPtrOffset)), bltCmd->getDestinationBaseAddress());
        }
        EXPECT_EQ(graphicsAllocation->getGpuAddress(), bltCmd->getSourceBaseAddress());
    }
    {
        // bufferWrite from hostPtr
        HardwareParse hwParser;
        auto blitProperties = BlitProperties::constructPropertiesForReadWriteBuffer(BlitterConstants::BlitDirection::HostPtrToBuffer,
                                                                                    csr, graphicsAllocation,
                                                                                    mapAllocation, mapPtr,
                                                                                    graphicsAllocation->getGpuAddress(),
                                                                                    castToUint64(mapPtr),
                                                                                    {hostPtrOffset, 0, 0}, 0, {4, 2, 1}, 0, 0, 0, 0);
        blitBuffer(&csr, blitProperties, true);

        hwParser.parseCommands<FamilyType>(csr.commandStream);

        auto cmdIterator = find<typename FamilyType::XY_COPY_BLT *>(hwParser.cmdList.begin(), hwParser.cmdList.end());
        ASSERT_NE(hwParser.cmdList.end(), cmdIterator);

        auto bltCmd = genCmdCast<typename FamilyType::XY_COPY_BLT *>(*cmdIterator);
        EXPECT_NE(nullptr, bltCmd);
        if (pDevice->isFullRangeSvm()) {
            EXPECT_EQ(reinterpret_cast<uint64_t>(ptrOffset(mapPtr, hostPtrOffset)), bltCmd->getSourceBaseAddress());
        }
        EXPECT_EQ(graphicsAllocation->getGpuAddress(), bltCmd->getDestinationBaseAddress());
    }

    memoryManager->freeGraphicsMemory(mapAllocation);
}

HWTEST_F(BcsTests, givenMapAllocationInBuiltinOpParamsWhenConstructingThenUseItAsSourceOrDstAllocation) {
    auto &csr = pDevice->getUltCommandStreamReceiver<FamilyType>();
    auto memoryManager = csr.getMemoryManager();

    AllocationProperties properties{csr.getRootDeviceIndex(), false, 1234, GraphicsAllocation::AllocationType::MAP_ALLOCATION, false, pDevice->getDeviceBitfield()};
    GraphicsAllocation *mapAllocation = memoryManager->allocateGraphicsMemoryWithProperties(properties, reinterpret_cast<void *>(0x12340000));

    auto mapAllocationOffset = 0x1234;
    auto mapPtr = reinterpret_cast<void *>(mapAllocation->getGpuAddress() + mapAllocationOffset);

    cl_int retVal = CL_SUCCESS;
    auto buffer = clUniquePtr<Buffer>(Buffer::create(context.get(), CL_MEM_READ_WRITE, 100, nullptr, retVal));

    {
        // from hostPtr
        BuiltinOpParams builtinOpParams = {};
        builtinOpParams.dstMemObj = buffer.get();
        builtinOpParams.srcPtr = mapPtr;
        builtinOpParams.size = {1, 1, 1};
        builtinOpParams.transferAllocation = mapAllocation;

        auto blitProperties = ClBlitProperties::constructProperties(BlitterConstants::BlitDirection::HostPtrToBuffer,
                                                                    csr, builtinOpParams);
        EXPECT_EQ(mapAllocation, blitProperties.srcAllocation);
    }
    {
        // to hostPtr
        BuiltinOpParams builtinOpParams = {};
        builtinOpParams.srcMemObj = buffer.get();
        builtinOpParams.dstPtr = mapPtr;
        builtinOpParams.size = {1, 1, 1};
        builtinOpParams.transferAllocation = mapAllocation;

        auto blitProperties = ClBlitProperties::constructProperties(BlitterConstants::BlitDirection::BufferToHostPtr,
                                                                    csr, builtinOpParams);
        EXPECT_EQ(mapAllocation, blitProperties.dstAllocation);
    }

    memoryManager->freeGraphicsMemory(mapAllocation);
}

HWTEST_F(BcsTests, givenNonZeroCopySvmAllocationWhenConstructingBlitPropertiesForReadWriteBufferCallThenSetValidAllocations) {
    auto &csr = pDevice->getUltCommandStreamReceiver<FamilyType>();
    MockMemoryManager mockMemoryManager(true, true);
    SVMAllocsManager svmAllocsManager(&mockMemoryManager);

    auto svmAllocationProperties = MemObjHelper::getSvmAllocationProperties(CL_MEM_READ_WRITE);
    auto svmAlloc = svmAllocsManager.createSVMAlloc(csr.getRootDeviceIndex(), 1, svmAllocationProperties, pDevice->getDeviceBitfield());
    auto svmData = svmAllocsManager.getSVMAlloc(svmAlloc);

    auto gpuAllocation = svmData->gpuAllocations.getGraphicsAllocation(pDevice->getRootDeviceIndex());

    EXPECT_NE(nullptr, gpuAllocation);
    EXPECT_NE(nullptr, svmData->cpuAllocation);
    EXPECT_NE(gpuAllocation, svmData->cpuAllocation);

    {
        // from hostPtr
        BuiltinOpParams builtinOpParams = {};
        builtinOpParams.dstSvmAlloc = gpuAllocation;
        builtinOpParams.srcSvmAlloc = svmData->cpuAllocation;
        builtinOpParams.srcPtr = reinterpret_cast<void *>(svmData->cpuAllocation->getGpuAddress());
        builtinOpParams.size = {1, 1, 1};

        auto blitProperties = ClBlitProperties::constructProperties(BlitterConstants::BlitDirection::HostPtrToBuffer,
                                                                    csr, builtinOpParams);
        EXPECT_EQ(svmData->cpuAllocation, blitProperties.srcAllocation);
        EXPECT_EQ(gpuAllocation, blitProperties.dstAllocation);
    }
    {
        // to hostPtr
        BuiltinOpParams builtinOpParams = {};
        builtinOpParams.srcSvmAlloc = gpuAllocation;
        builtinOpParams.dstSvmAlloc = svmData->cpuAllocation;
        builtinOpParams.dstPtr = reinterpret_cast<void *>(svmData->cpuAllocation->getGpuAddress());
        builtinOpParams.size = {1, 1, 1};

        auto blitProperties = ClBlitProperties::constructProperties(BlitterConstants::BlitDirection::BufferToHostPtr,
                                                                    csr, builtinOpParams);
        EXPECT_EQ(svmData->cpuAllocation, blitProperties.dstAllocation);
        EXPECT_EQ(gpuAllocation, blitProperties.srcAllocation);
    }

    svmAllocsManager.freeSVMAlloc(svmAlloc);
}

HWTEST_F(BcsTests, givenSvmAllocationWhenBlitCalledThenUsePassedPointers) {
    auto &csr = pDevice->getUltCommandStreamReceiver<FamilyType>();
    MockMemoryManager mockMemoryManager(true, true);
    SVMAllocsManager svmAllocsManager(&mockMemoryManager);

    auto svmAllocationProperties = MemObjHelper::getSvmAllocationProperties(CL_MEM_READ_WRITE);
    auto svmAlloc = svmAllocsManager.createSVMAlloc(csr.getRootDeviceIndex(), 1, svmAllocationProperties, pDevice->getDeviceBitfield());
    auto svmData = svmAllocsManager.getSVMAlloc(svmAlloc);
    auto gpuAllocation = svmData->gpuAllocations.getGraphicsAllocation(pDevice->getRootDeviceIndex());

    EXPECT_NE(nullptr, gpuAllocation);
    EXPECT_NE(nullptr, svmData->cpuAllocation);
    EXPECT_NE(gpuAllocation, svmData->cpuAllocation);

    uint64_t srcOffset = 2;
    uint64_t dstOffset = 3;

    {
        // from hostPtr
        BuiltinOpParams builtinOpParams = {};
        builtinOpParams.dstSvmAlloc = svmData->cpuAllocation;
        builtinOpParams.srcSvmAlloc = gpuAllocation;
        builtinOpParams.srcPtr = reinterpret_cast<void *>(svmData->cpuAllocation->getGpuAddress() + srcOffset);
        builtinOpParams.dstPtr = reinterpret_cast<void *>(svmData->cpuAllocation->getGpuAddress() + dstOffset);
        builtinOpParams.size = {1, 1, 1};

        auto blitProperties = ClBlitProperties::constructProperties(BlitterConstants::BlitDirection::HostPtrToBuffer,
                                                                    csr, builtinOpParams);
        EXPECT_EQ(gpuAllocation, blitProperties.srcAllocation);
        EXPECT_EQ(svmData->cpuAllocation, blitProperties.dstAllocation);

        blitBuffer(&csr, blitProperties, true);

        HardwareParse hwParser;
        hwParser.parseCommands<FamilyType>(csr.commandStream, 0);

        auto cmdIterator = find<typename FamilyType::XY_COPY_BLT *>(hwParser.cmdList.begin(), hwParser.cmdList.end());
        ASSERT_NE(hwParser.cmdList.end(), cmdIterator);

        auto bltCmd = genCmdCast<typename FamilyType::XY_COPY_BLT *>(*cmdIterator);

        EXPECT_EQ(castToUint64(builtinOpParams.dstPtr), bltCmd->getDestinationBaseAddress());
        EXPECT_EQ(castToUint64(builtinOpParams.srcPtr), bltCmd->getSourceBaseAddress());
    }
    {
        // to hostPtr
        BuiltinOpParams builtinOpParams = {};
        builtinOpParams.srcSvmAlloc = gpuAllocation;
        builtinOpParams.dstSvmAlloc = svmData->cpuAllocation;
        builtinOpParams.dstPtr = reinterpret_cast<void *>(svmData->cpuAllocation + dstOffset);
        builtinOpParams.srcPtr = reinterpret_cast<void *>(gpuAllocation + srcOffset);
        builtinOpParams.size = {1, 1, 1};

        auto blitProperties = ClBlitProperties::constructProperties(BlitterConstants::BlitDirection::BufferToHostPtr,
                                                                    csr, builtinOpParams);

        auto offset = csr.commandStream.getUsed();
        blitBuffer(&csr, blitProperties, true);

        HardwareParse hwParser;
        hwParser.parseCommands<FamilyType>(csr.commandStream, offset);

        auto cmdIterator = find<typename FamilyType::XY_COPY_BLT *>(hwParser.cmdList.begin(), hwParser.cmdList.end());
        ASSERT_NE(hwParser.cmdList.end(), cmdIterator);

        auto bltCmd = genCmdCast<typename FamilyType::XY_COPY_BLT *>(*cmdIterator);

        EXPECT_EQ(castToUint64(builtinOpParams.dstPtr), bltCmd->getDestinationBaseAddress());
        EXPECT_EQ(castToUint64(builtinOpParams.srcPtr), bltCmd->getSourceBaseAddress());
    }
    svmAllocsManager.freeSVMAlloc(svmAlloc);
}

HWTEST_F(BcsTests, givenBufferWithOffsetWhenBlitOperationCalledThenProgramCorrectGpuAddresses) {
    auto &csr = pDevice->getUltCommandStreamReceiver<FamilyType>();

    cl_int retVal = CL_SUCCESS;
    auto buffer1 = clUniquePtr<Buffer>(Buffer::create(context.get(), CL_MEM_READ_WRITE, 1, nullptr, retVal));
    auto buffer2 = clUniquePtr<Buffer>(Buffer::create(context.get(), CL_MEM_READ_WRITE, 1, nullptr, retVal));
    void *hostPtr = reinterpret_cast<void *>(0x12340000);
    auto graphicsAllocation1 = buffer1->getGraphicsAllocation(pDevice->getRootDeviceIndex());
    auto graphicsAllocation2 = buffer2->getGraphicsAllocation(pDevice->getRootDeviceIndex());

    size_t addressOffsets[] = {0, 1, 1234};

    for (auto buffer1Offset : addressOffsets) {
        {
            // from hostPtr
            HardwareParse hwParser;
            auto offset = csr.commandStream.getUsed();
            auto blitProperties = BlitProperties::constructPropertiesForReadWriteBuffer(BlitterConstants::BlitDirection::HostPtrToBuffer,
                                                                                        csr, graphicsAllocation1,
                                                                                        nullptr, hostPtr,
                                                                                        graphicsAllocation1->getGpuAddress(),
                                                                                        0, 0, {buffer1Offset, 0, 0}, {1, 1, 1}, 0, 0, 0, 0);

            blitBuffer(&csr, blitProperties, true);

            hwParser.parseCommands<FamilyType>(csr.commandStream, offset);

            auto cmdIterator = find<typename FamilyType::XY_COPY_BLT *>(hwParser.cmdList.begin(), hwParser.cmdList.end());
            ASSERT_NE(hwParser.cmdList.end(), cmdIterator);

            auto bltCmd = genCmdCast<typename FamilyType::XY_COPY_BLT *>(*cmdIterator);
            EXPECT_NE(nullptr, bltCmd);
            if (pDevice->isFullRangeSvm()) {
                EXPECT_EQ(reinterpret_cast<uint64_t>(hostPtr), bltCmd->getSourceBaseAddress());
            }
            EXPECT_EQ(ptrOffset(graphicsAllocation1->getGpuAddress(), buffer1Offset), bltCmd->getDestinationBaseAddress());
        }
        {
            // to hostPtr
            HardwareParse hwParser;
            auto offset = csr.commandStream.getUsed();
            auto blitProperties = BlitProperties::constructPropertiesForReadWriteBuffer(BlitterConstants::BlitDirection::BufferToHostPtr,
                                                                                        csr, graphicsAllocation1, nullptr,
                                                                                        hostPtr,
                                                                                        graphicsAllocation1->getGpuAddress(),
                                                                                        0, 0, {buffer1Offset, 0, 0}, {1, 1, 1}, 0, 0, 0, 0);

            blitBuffer(&csr, blitProperties, true);

            hwParser.parseCommands<FamilyType>(csr.commandStream, offset);

            auto cmdIterator = find<typename FamilyType::XY_COPY_BLT *>(hwParser.cmdList.begin(), hwParser.cmdList.end());
            ASSERT_NE(hwParser.cmdList.end(), cmdIterator);

            auto bltCmd = genCmdCast<typename FamilyType::XY_COPY_BLT *>(*cmdIterator);
            EXPECT_NE(nullptr, bltCmd);
            if (pDevice->isFullRangeSvm()) {
                EXPECT_EQ(reinterpret_cast<uint64_t>(hostPtr), bltCmd->getDestinationBaseAddress());
            }
            EXPECT_EQ(ptrOffset(graphicsAllocation1->getGpuAddress(), buffer1Offset), bltCmd->getSourceBaseAddress());
        }
        for (auto buffer2Offset : addressOffsets) {
            // Buffer to Buffer
            HardwareParse hwParser;
            auto offset = csr.commandStream.getUsed();
            auto blitProperties = BlitProperties::constructPropertiesForCopyBuffer(graphicsAllocation1,
                                                                                   graphicsAllocation2,
                                                                                   {buffer1Offset, 0, 0}, {buffer2Offset, 0, 0}, {1, 1, 1}, 0, 0, 0, 0);

            blitBuffer(&csr, blitProperties, true);

            hwParser.parseCommands<FamilyType>(csr.commandStream, offset);

            auto cmdIterator = find<typename FamilyType::XY_COPY_BLT *>(hwParser.cmdList.begin(), hwParser.cmdList.end());
            ASSERT_NE(hwParser.cmdList.end(), cmdIterator);

            auto bltCmd = genCmdCast<typename FamilyType::XY_COPY_BLT *>(*cmdIterator);
            EXPECT_NE(nullptr, bltCmd);
            EXPECT_EQ(ptrOffset(graphicsAllocation1->getGpuAddress(), buffer1Offset), bltCmd->getDestinationBaseAddress());
            EXPECT_EQ(ptrOffset(graphicsAllocation2->getGpuAddress(), buffer2Offset), bltCmd->getSourceBaseAddress());
        }
    }
}

HWTEST_F(BcsTests, givenAuxTranslationRequestWhenBlitCalledThenProgramCommandCorrectly) {
    auto &csr = pDevice->getUltCommandStreamReceiver<FamilyType>();

    cl_int retVal = CL_SUCCESS;
    auto buffer = clUniquePtr<Buffer>(Buffer::create(context.get(), CL_MEM_READ_WRITE, 123, nullptr, retVal));
    auto graphicsAllocation = buffer->getGraphicsAllocation(pDevice->getRootDeviceIndex());
    auto allocationGpuAddress = graphicsAllocation->getGpuAddress();
    auto allocationSize = graphicsAllocation->getUnderlyingBufferSize();

    AuxTranslationDirection translationDirection[] = {AuxTranslationDirection::AuxToNonAux, AuxTranslationDirection::NonAuxToAux};

    for (int i = 0; i < 2; i++) {
        auto blitProperties = BlitProperties::constructPropertiesForAuxTranslation(translationDirection[i],
                                                                                   graphicsAllocation);

        auto offset = csr.commandStream.getUsed();
        blitBuffer(&csr, blitProperties, false);

        HardwareParse hwParser;
        hwParser.parseCommands<FamilyType>(csr.commandStream, offset);
        uint32_t xyCopyBltCmdFound = 0;

        for (auto &cmd : hwParser.cmdList) {
            if (auto bltCmd = genCmdCast<typename FamilyType::XY_COPY_BLT *>(cmd)) {
                xyCopyBltCmdFound++;
                EXPECT_EQ(static_cast<uint32_t>(allocationSize), bltCmd->getTransferWidth());
                EXPECT_EQ(1u, bltCmd->getTransferHeight());

                EXPECT_EQ(allocationGpuAddress, bltCmd->getDestinationBaseAddress());
                EXPECT_EQ(allocationGpuAddress, bltCmd->getSourceBaseAddress());
            }
        }
        EXPECT_EQ(1u, xyCopyBltCmdFound);
    }
}

HWTEST_F(BcsTests, givenInvalidBlitDirectionWhenConstructPropertiesThenExceptionIsThrow) {
    auto &csr = pDevice->getUltCommandStreamReceiver<FamilyType>();

    EXPECT_THROW(ClBlitProperties::constructProperties(static_cast<BlitterConstants::BlitDirection>(7), csr, {}), std::exception);
}

struct MockScratchSpaceController : ScratchSpaceControllerBase {
    using ScratchSpaceControllerBase::privateScratchAllocation;
    using ScratchSpaceControllerBase::ScratchSpaceControllerBase;
};

using ScratchSpaceControllerTest = Test<ClDeviceFixture>;

TEST_F(ScratchSpaceControllerTest, whenScratchSpaceControllerIsDestroyedThenItReleasePrivateScratchSpaceAllocation) {
    MockScratchSpaceController scratchSpaceController(pDevice->getRootDeviceIndex(), *pDevice->getExecutionEnvironment(), *pDevice->getGpgpuCommandStreamReceiver().getInternalAllocationStorage());
    scratchSpaceController.privateScratchAllocation = pDevice->getExecutionEnvironment()->memoryManager->allocateGraphicsMemoryInPreferredPool(MockAllocationProperties{pDevice->getRootDeviceIndex(), MemoryConstants::pageSize}, nullptr);
    EXPECT_NE(nullptr, scratchSpaceController.privateScratchAllocation);
    //no memory leak is expected
}

TEST(BcsConstantsTests, givenBlitConstantsThenTheyHaveDesiredValues) {
    EXPECT_EQ(BlitterConstants::maxBlitWidth, 0x3FC0u);
    EXPECT_EQ(BlitterConstants::maxBlitHeight, 0x3FC0u);
}
