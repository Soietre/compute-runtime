/*
 * Copyright (C) 2017-2020 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#pragma once
#include "shared/source/memory_manager/graphics_allocation.h"

#include <cinttypes>

namespace NEO {

struct HwTimeStamps {
    void initialize() {
        GlobalStartTS = 0;
        ContextStartTS = 0;
        GlobalEndTS = 0;
        ContextEndTS = 0;
        GlobalCompleteTS = 0;
        ContextCompleteTS = 0;
    }
    bool isCompleted() const { return true; }
    uint32_t getImplicitGpuDependenciesCount() const { return 0; }

    static GraphicsAllocation::AllocationType getAllocationType() {
        return GraphicsAllocation::AllocationType::PROFILING_TAG_BUFFER;
    }
    uint64_t GlobalStartTS;
    uint64_t ContextStartTS;
    uint64_t GlobalEndTS;
    uint64_t ContextEndTS;
    uint64_t GlobalCompleteTS;
    uint64_t ContextCompleteTS;
};
} // namespace NEO
