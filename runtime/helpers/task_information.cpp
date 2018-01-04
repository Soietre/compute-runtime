/*
 * Copyright (c) 2017, Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "runtime/command_stream/linear_stream.h"
#include "runtime/command_stream/command_stream_receiver.h"
#include "runtime/command_queue/command_queue.h"
#include "runtime/command_queue/enqueue_common.h"
#include "runtime/device/device.h"
#include "runtime/device_queue/device_queue.h"
#include "runtime/mem_obj/mem_obj.h"
#include "runtime/memory_manager/surface.h"
#include "runtime/helpers/aligned_memory.h"
#include "runtime/helpers/string.h"
#include "runtime/helpers/task_information.h"

namespace OCLRT {
KernelOperation::~KernelOperation() {
    alignedFree(dsh->getBase());
    alignedFree(ish->getBase());
    if (doNotFreeISH) {
        ioh.release();
    } else {
        alignedFree(ioh->getBase());
    }
    alignedFree(ssh->getBase());
    alignedFree(commandStream->getBase());
}

CompletionStamp &CommandMapUnmap::submit(uint32_t taskLevel, bool terminated) {
    if (terminated) {
        return completionStamp;
    }

    bool blocking = true;
    TakeOwnershipWrapper<Device> deviceOwnership(cmdQ.getDevice());

    auto &queueCommandStream = cmdQ.getCS(0);
    size_t offset = queueCommandStream.getUsed();

    DispatchFlags dispatchFlags;
    dispatchFlags.blocking = blocking;
    dispatchFlags.dcFlush = true;
    dispatchFlags.useSLM = true;
    dispatchFlags.guardCommandBufferWithPipeControl = true;
    dispatchFlags.low_priority = cmdQ.low_priority;
    dispatchFlags.preemptionMode = PreemptionHelper::taskPreemptionMode(cmdQ.getDevice(), nullptr);

    DEBUG_BREAK_IF(taskLevel >= Event::eventNotReady);

    completionStamp = csr.flushTask(queueCommandStream,
                                    offset,
                                    cmdQ.getIndirectHeap(IndirectHeap::DYNAMIC_STATE),
                                    cmdQ.getIndirectHeap(IndirectHeap::INSTRUCTION),
                                    cmdQ.getIndirectHeap(IndirectHeap::INDIRECT_OBJECT),
                                    cmdQ.getIndirectHeap(IndirectHeap::SURFACE_STATE),
                                    taskLevel,
                                    dispatchFlags);

    cmdQ.waitUntilComplete(completionStamp.taskCount, completionStamp.flushStamp);

    if (m.isMemObjZeroCopy() == false) {
        if (op == MAP) {
            m.transferDataToHostPtr();
        } else {
            DEBUG_BREAK_IF(op != UNMAP);
            m.transferDataFromHostPtrToMemoryStorage();
        }
    }

    return completionStamp;
}

CommandComputeKernel::CommandComputeKernel(CommandQueue &commandQueue, CommandStreamReceiver &commandStreamReceiver,
                                           std::unique_ptr<KernelOperation> kernelOperation, std::vector<Surface *> &surfaces,
                                           bool flushDC, bool usesSLM, bool ndRangeKernel, std::unique_ptr<PrintfHandler> printfHandler, Kernel *kernel, uint32_t kernelCount)
    : commandQueue(commandQueue),
      commandStreamReceiver(commandStreamReceiver),
      kernelOperation(std::move(kernelOperation)),
      flushDC(flushDC),
      slmUsed(usesSLM),
      NDRangeKernel(ndRangeKernel),
      printfHandler(std::move(printfHandler)),
      kernel(nullptr),
      kernelCount(0) {
    for (auto surface : surfaces) {
        this->surfaces.push_back(surface);
    }
    this->kernel = kernel;
    this->kernelCount = kernelCount;
}

CommandComputeKernel::~CommandComputeKernel() {
    for (auto surface : surfaces) {
        delete surface;
    }
    surfaces.clear();
    if (kernelOperation->ioh.get() == kernelOperation->dsh.get()) {
        kernelOperation->doNotFreeISH = true;
    }
}

CompletionStamp &CommandComputeKernel::submit(uint32_t taskLevel, bool terminated) {
    if (terminated) {
        return completionStamp;
    }
    bool executionModelKernel = kernel != nullptr ? kernel->isParentKernel : false;
    auto devQueue = commandQueue.getContext().getDefaultDeviceQueue();

    TakeOwnershipWrapper<Device> deviceOwnership(commandQueue.getDevice());

    if (executionModelKernel) {
        while (!devQueue->isEMCriticalSectionFree())
            ;

        devQueue->resetDeviceQueue();
        devQueue->acquireEMCriticalSection();
    }

    auto &commandStream = *kernelOperation->commandStream;
    size_t commandsSize = commandStream.getUsed();
    auto &queueCommandStream = commandQueue.getCS(commandStream.getUsed());
    size_t offset = queueCommandStream.getUsed();
    void *pDst = queueCommandStream.getSpace(commandsSize);
    //transfer the memory to commandStream of the queue.
    memcpy_s(pDst, commandsSize, commandStream.getBase(), commandsSize);

    size_t requestedDshSize = kernelOperation->dsh->getUsed();
    size_t requestedIshSize = kernelOperation->ish->getUsed() + kernelOperation->instructionHeapSizeEM;
    size_t requestedIohSize = kernelOperation->ioh->getUsed();
    size_t requestedSshSize = kernelOperation->ssh->getUsed() + kernelOperation->surfaceStateHeapSizeEM;

    IndirectHeap *dsh = nullptr;
    IndirectHeap *ioh = nullptr;

    IndirectHeap::Type trackedHeaps[] = {IndirectHeap::SURFACE_STATE, IndirectHeap::INDIRECT_OBJECT, IndirectHeap::DYNAMIC_STATE, IndirectHeap::INSTRUCTION};

    for (auto trackedHeap = 0u; trackedHeap < ARRAY_COUNT(trackedHeaps); trackedHeap++) {
        if (commandQueue.getIndirectHeap(trackedHeaps[trackedHeap], 0).getUsed() > 0) {
            commandQueue.releaseIndirectHeap(trackedHeaps[trackedHeap]);
        }
    }

    if (executionModelKernel) {
        dsh = devQueue->getIndirectHeap(IndirectHeap::DYNAMIC_STATE);
        // In ExecutionModel IOH is the same as DSH to eliminate StateBaseAddress reprogramming for scheduler kernel and blocks.
        ioh = dsh;

        memcpy_s(dsh->getSpace(0), dsh->getAvailableSpace(), ptrOffset(kernelOperation->dsh->getBase(), devQueue->colorCalcStateSize), kernelOperation->dsh->getUsed() - devQueue->colorCalcStateSize);
        dsh->getSpace(kernelOperation->dsh->getUsed() - devQueue->colorCalcStateSize);
    } else {
        dsh = &commandQueue.getIndirectHeap(IndirectHeap::DYNAMIC_STATE, requestedDshSize);
        ioh = &commandQueue.getIndirectHeap(IndirectHeap::INDIRECT_OBJECT, requestedIohSize);

        memcpy_s(dsh->getBase(), requestedDshSize, kernelOperation->dsh->getBase(), kernelOperation->dsh->getUsed());
        dsh->getSpace(requestedDshSize);

        memcpy_s(ioh->getBase(), requestedIohSize, kernelOperation->ioh->getBase(), kernelOperation->ioh->getUsed());
        ioh->getSpace(requestedIohSize);
    }

    IndirectHeap &ish = commandQueue.getIndirectHeap(IndirectHeap::INSTRUCTION, requestedIshSize);
    IndirectHeap &ssh = commandQueue.getIndirectHeap(IndirectHeap::SURFACE_STATE, requestedSshSize);

    memcpy_s(ish.getBase(), requestedIshSize, kernelOperation->ish->getBase(), kernelOperation->ish->getUsed());
    ish.getSpace(kernelOperation->ish->getUsed());

    memcpy_s(ssh.getBase(), requestedSshSize, kernelOperation->ssh->getBase(), kernelOperation->ssh->getUsed());
    ssh.getSpace(kernelOperation->ssh->getUsed());

    auto requiresCoherency = false;
    for (auto &surface : surfaces) {
        DEBUG_BREAK_IF(!surface);
        surface->makeResident(commandStreamReceiver);
        requiresCoherency |= surface->IsCoherent;
    }

    if (printfHandler) {
        printfHandler.get()->makeResident(commandStreamReceiver);
    }

    if (executionModelKernel) {
        uint32_t taskCount = commandStreamReceiver.peekTaskCount() + 1;
        devQueue->setupExecutionModelDispatch(ish, ssh, kernel, kernelCount, taskCount, timestamp);

        BuiltIns &builtIns = BuiltIns::getInstance();
        SchedulerKernel &scheduler = builtIns.getSchedulerKernel(commandQueue.getContext());

        scheduler.setArgs(devQueue->getQueueBuffer(),
                          devQueue->getStackBuffer(),
                          devQueue->getEventPoolBuffer(),
                          devQueue->getSlbBuffer(),
                          devQueue->getDshBuffer(),
                          kernel->getKernelReflectionSurface(),
                          devQueue->getQueueStorageBuffer(),
                          ssh.getGraphicsAllocation(),
                          devQueue->getDebugQueue());

        devQueue->dispatchScheduler(
            commandQueue,
            scheduler);

        scheduler.makeResident(commandStreamReceiver);

        // Update SLM usage
        slmUsed |= scheduler.slmTotalSize > 0;
    }

    DispatchFlags dispatchFlags;
    dispatchFlags.blocking = true;
    dispatchFlags.dcFlush = flushDC;
    dispatchFlags.useSLM = slmUsed;
    dispatchFlags.guardCommandBufferWithPipeControl = true;
    dispatchFlags.GSBA32BitRequired = NDRangeKernel;
    dispatchFlags.requiresCoherency = requiresCoherency;
    dispatchFlags.low_priority = commandQueue.low_priority;
    dispatchFlags.preemptionMode = PreemptionHelper::taskPreemptionMode(commandQueue.getDevice(), kernel);

    DEBUG_BREAK_IF(taskLevel >= Event::eventNotReady);

    completionStamp = commandStreamReceiver.flushTask(queueCommandStream,
                                                      offset,
                                                      *dsh,
                                                      ish,
                                                      *ioh,
                                                      ssh,
                                                      taskLevel,
                                                      dispatchFlags);

    commandQueue.waitUntilComplete(completionStamp.taskCount, completionStamp.flushStamp);

    for (auto &surface : surfaces) {
        surface->setCompletionStamp(completionStamp, nullptr, nullptr);
    }

    if (printfHandler) {
        printfHandler.get()->printEnqueueOutput();
    }

    return completionStamp;
}

CompletionStamp &CommandMarker::submit(uint32_t taskLevel, bool terminated) {
    if (terminated) {
        return completionStamp;
    }

    bool blocking = true;
    TakeOwnershipWrapper<Device> deviceOwnership(cmdQ.getDevice());

    auto &queueCommandStream = cmdQ.getCS(this->commandSize);
    size_t offset = queueCommandStream.getUsed();

    DispatchFlags dispatchFlags;
    dispatchFlags.blocking = blocking;
    dispatchFlags.dcFlush = shouldFlushDC(clCommandType, nullptr);
    dispatchFlags.low_priority = cmdQ.low_priority;
    dispatchFlags.preemptionMode = PreemptionHelper::taskPreemptionMode(cmdQ.getDevice(), nullptr);

    DEBUG_BREAK_IF(taskLevel >= Event::eventNotReady);

    completionStamp = csr.flushTask(queueCommandStream,
                                    offset,
                                    cmdQ.getIndirectHeap(IndirectHeap::DYNAMIC_STATE),
                                    cmdQ.getIndirectHeap(IndirectHeap::INSTRUCTION),
                                    cmdQ.getIndirectHeap(IndirectHeap::INDIRECT_OBJECT),
                                    cmdQ.getIndirectHeap(IndirectHeap::SURFACE_STATE),
                                    taskLevel,
                                    dispatchFlags);

    cmdQ.waitUntilComplete(completionStamp.taskCount, completionStamp.flushStamp);

    return completionStamp;
}
} // namespace OCLRT
