// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.
//
// SatoriAllocationContext.cpp
//

#include "common.h"
#include "gcenv.h"
#include "../env/gcenv.os.h"

#include "SatoriAllocationContext.h"
#include "SatoriRegion.h"
#include "SatoriRegion.inl"

class SatoriHeap;

void SatoriAllocationContext::Deactivate(SatoriHeap* heap)
{
    if (RegularRegion() != nullptr)
    {
        SatoriRegion* region = RegularRegion();
        size_t allocPtr = (size_t)this->alloc_ptr;

        this->alloc_bytes -= this->alloc_limit - this->alloc_ptr;
        this->alloc_limit = this->alloc_ptr = nullptr;
        RegularRegion() = nullptr;

        region->Deactivate(heap, allocPtr);
    }
    else
    {
        _ASSERTE(this->alloc_limit == nullptr);
        _ASSERTE(this->alloc_ptr == nullptr);
    }

    if (LargeRegion() != nullptr)
    {
        SatoriRegion* region = LargeRegion();
        LargeRegion() = nullptr;

        region->Deactivate(heap, /* allocPtr */ 0);
    }
 }
