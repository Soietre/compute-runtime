/*
 * Copyright (C) 2018-2019 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#pragma once
#include "common/helpers/bit_helpers.h"
#include "public/cl_ext_private.h"
#include "runtime/context/context_type.h"
#include "runtime/helpers/mem_properties_parser_helper.h"
#include "runtime/mem_obj/mem_obj.h"
#include "runtime/memory_manager/memory_manager.h"
#include "runtime/memory_manager/unified_memory_manager.h"

#include "CL/cl.h"
#include "mem_obj_types.h"
#include "memory_properties_flags.h"

namespace NEO {

class MemObjHelper {
  public:
    static bool validateMemoryPropertiesForBuffer(const MemoryProperties &properties) {
        if (!MemObjHelper::checkUsedFlagsForBuffer(properties)) {
            return false;
        }

        /* Check all the invalid flags combination. */
        if ((isValueSet(properties.flags, CL_MEM_READ_WRITE | CL_MEM_READ_ONLY)) ||
            (isValueSet(properties.flags, CL_MEM_READ_WRITE | CL_MEM_WRITE_ONLY)) ||
            (isValueSet(properties.flags, CL_MEM_READ_ONLY | CL_MEM_WRITE_ONLY)) ||
            (isValueSet(properties.flags, CL_MEM_ALLOC_HOST_PTR | CL_MEM_USE_HOST_PTR)) ||
            (isValueSet(properties.flags, CL_MEM_COPY_HOST_PTR | CL_MEM_USE_HOST_PTR)) ||
            (isValueSet(properties.flags, CL_MEM_HOST_READ_ONLY | CL_MEM_HOST_NO_ACCESS)) ||
            (isValueSet(properties.flags, CL_MEM_HOST_READ_ONLY | CL_MEM_HOST_WRITE_ONLY)) ||
            (isValueSet(properties.flags, CL_MEM_HOST_WRITE_ONLY | CL_MEM_HOST_NO_ACCESS))) {
            return false;
        }

        return validateExtraMemoryProperties(properties);
    }

    static bool validateMemoryPropertiesForImage(const MemoryProperties &properties, cl_mem parent) {
        if (!MemObjHelper::checkUsedFlagsForImage(properties)) {
            return false;
        }

        /* Check all the invalid flags combination. */
        if ((!isValueSet(properties.flags, CL_MEM_ACCESS_FLAGS_UNRESTRICTED_INTEL)) &&
            (isValueSet(properties.flags, CL_MEM_READ_WRITE | CL_MEM_WRITE_ONLY) ||
             isValueSet(properties.flags, CL_MEM_READ_WRITE | CL_MEM_READ_ONLY) ||
             isValueSet(properties.flags, CL_MEM_WRITE_ONLY | CL_MEM_READ_ONLY) ||
             isValueSet(properties.flags, CL_MEM_ALLOC_HOST_PTR | CL_MEM_USE_HOST_PTR) ||
             isValueSet(properties.flags, CL_MEM_COPY_HOST_PTR | CL_MEM_USE_HOST_PTR) ||
             isValueSet(properties.flags, CL_MEM_HOST_WRITE_ONLY | CL_MEM_HOST_READ_ONLY) ||
             isValueSet(properties.flags, CL_MEM_HOST_WRITE_ONLY | CL_MEM_HOST_NO_ACCESS) ||
             isValueSet(properties.flags, CL_MEM_HOST_READ_ONLY | CL_MEM_HOST_NO_ACCESS) ||
             isValueSet(properties.flags, CL_MEM_NO_ACCESS_INTEL | CL_MEM_READ_WRITE) ||
             isValueSet(properties.flags, CL_MEM_NO_ACCESS_INTEL | CL_MEM_WRITE_ONLY) ||
             isValueSet(properties.flags, CL_MEM_NO_ACCESS_INTEL | CL_MEM_READ_ONLY))) {
            return false;
        }

        auto parentMemObj = castToObject<MemObj>(parent);
        if (parentMemObj != nullptr && properties.flags) {
            auto parentFlags = parentMemObj->getFlags();
            /* Check whether flags are compatible with parent. */
            if (isValueSet(properties.flags, CL_MEM_ALLOC_HOST_PTR) ||
                isValueSet(properties.flags, CL_MEM_COPY_HOST_PTR) ||
                isValueSet(properties.flags, CL_MEM_USE_HOST_PTR) ||
                ((!isValueSet(parentFlags, CL_MEM_ACCESS_FLAGS_UNRESTRICTED_INTEL)) &&
                 (!isValueSet(properties.flags, CL_MEM_ACCESS_FLAGS_UNRESTRICTED_INTEL)) &&
                 ((isValueSet(parentFlags, CL_MEM_WRITE_ONLY) && isValueSet(properties.flags, CL_MEM_READ_WRITE)) ||
                  (isValueSet(parentFlags, CL_MEM_WRITE_ONLY) && isValueSet(properties.flags, CL_MEM_READ_ONLY)) ||
                  (isValueSet(parentFlags, CL_MEM_READ_ONLY) && isValueSet(properties.flags, CL_MEM_READ_WRITE)) ||
                  (isValueSet(parentFlags, CL_MEM_READ_ONLY) && isValueSet(properties.flags, CL_MEM_WRITE_ONLY)) ||
                  (isValueSet(parentFlags, CL_MEM_NO_ACCESS_INTEL) && isValueSet(properties.flags, CL_MEM_READ_WRITE)) ||
                  (isValueSet(parentFlags, CL_MEM_NO_ACCESS_INTEL) && isValueSet(properties.flags, CL_MEM_WRITE_ONLY)) ||
                  (isValueSet(parentFlags, CL_MEM_NO_ACCESS_INTEL) && isValueSet(properties.flags, CL_MEM_READ_ONLY)) ||
                  (isValueSet(parentFlags, CL_MEM_HOST_NO_ACCESS) && isValueSet(properties.flags, CL_MEM_HOST_WRITE_ONLY)) ||
                  (isValueSet(parentFlags, CL_MEM_HOST_NO_ACCESS) && isValueSet(properties.flags, CL_MEM_HOST_READ_ONLY))))) {
                return false;
            }
        }

        return validateExtraMemoryProperties(properties);
    }

    static AllocationProperties getAllocationPropertiesWithImageInfo(ImageInfo &imgInfo, bool allocateMemory, const MemoryPropertiesFlags &memoryProperties) {
        AllocationProperties allocationProperties{allocateMemory, imgInfo, GraphicsAllocation::AllocationType::IMAGE};
        MemoryPropertiesParser::fillPoliciesInProperties(allocationProperties, memoryProperties);
        return allocationProperties;
    }

    static bool checkMemFlagsForSubBuffer(cl_mem_flags flags) {
        const cl_mem_flags allValidFlags =
            CL_MEM_READ_WRITE | CL_MEM_WRITE_ONLY | CL_MEM_READ_ONLY |
            CL_MEM_HOST_WRITE_ONLY | CL_MEM_HOST_READ_ONLY | CL_MEM_HOST_NO_ACCESS;

        return isFieldValid(flags, allValidFlags);
    }

    static SVMAllocsManager::SvmAllocationProperties getSvmAllocationProperties(cl_mem_flags flags) {
        SVMAllocsManager::SvmAllocationProperties svmProperties;
        svmProperties.coherent = isValueSet(flags, CL_MEM_SVM_FINE_GRAIN_BUFFER);
        svmProperties.hostPtrReadOnly = isValueSet(flags, CL_MEM_HOST_READ_ONLY) || isValueSet(flags, CL_MEM_HOST_NO_ACCESS);
        svmProperties.readOnly = isValueSet(flags, CL_MEM_READ_ONLY);
        return svmProperties;
    }

    static bool isSuitableForRenderCompression(bool renderCompressed, const MemoryPropertiesFlags &properties, ContextType contextType, bool preferCompression);

  protected:
    static bool checkUsedFlagsForBuffer(const MemoryProperties &properties) {
        MemoryProperties acceptedProperties;
        addCommonMemoryProperties(acceptedProperties);
        addBufferMemoryProperties(acceptedProperties);
        addExtraMemoryProperties(acceptedProperties);

        return (isFieldValid(properties.flags, acceptedProperties.flags) &&
                isFieldValid(properties.flags_intel, acceptedProperties.flags_intel));
    }

    static bool checkUsedFlagsForImage(const MemoryProperties &properties) {
        MemoryProperties acceptedProperties;
        addCommonMemoryProperties(acceptedProperties);
        addImageMemoryProperties(acceptedProperties);
        addExtraMemoryProperties(acceptedProperties);

        return (isFieldValid(properties.flags, acceptedProperties.flags) &&
                isFieldValid(properties.flags_intel, acceptedProperties.flags_intel));
    }

    static inline void addCommonMemoryProperties(MemoryProperties &properties) {
        properties.flags |=
            CL_MEM_READ_WRITE | CL_MEM_WRITE_ONLY | CL_MEM_READ_ONLY |
            CL_MEM_ALLOC_HOST_PTR | CL_MEM_COPY_HOST_PTR | CL_MEM_USE_HOST_PTR |
            CL_MEM_HOST_WRITE_ONLY | CL_MEM_HOST_READ_ONLY | CL_MEM_HOST_NO_ACCESS;
        properties.flags_intel |= CL_MEM_LOCALLY_UNCACHED_RESOURCE;
    }

    static inline void addImageMemoryProperties(MemoryProperties &properties) {
        properties.flags |= CL_MEM_NO_ACCESS_INTEL | CL_MEM_ACCESS_FLAGS_UNRESTRICTED_INTEL;
    }

    static inline void addBufferMemoryProperties(MemoryProperties &properties) {
        properties.flags |= CL_MEM_ALLOW_UNRESTRICTED_SIZE_INTEL;
        properties.flags_intel |= CL_MEM_ALLOW_UNRESTRICTED_SIZE_INTEL;
    }

    static void addExtraMemoryProperties(MemoryProperties &properties);
    static bool validateExtraMemoryProperties(const MemoryProperties &properties);
};
} // namespace NEO
