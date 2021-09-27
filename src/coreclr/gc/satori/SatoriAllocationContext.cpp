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

class SatoriRecycler;

void SatoriAllocationContext::Deactivate(SatoriRecycler* recycler, bool detach)
{
    SatoriRegion* region = RegularRegion();
    if (region != nullptr)
    {
        size_t allocPtr = (size_t)this->alloc_ptr;

        this->alloc_bytes -= this->alloc_limit - this->alloc_ptr;
        this->alloc_limit = this->alloc_ptr = nullptr;

        if (region->IsAllocating())
        {
            region->StopAllocating(allocPtr);
        }

        if (detach)
        {
            region->DetachFromContext();
        }

        recycler->AddEphemeralRegion(region);
    }
    else
    {
        _ASSERTE(this->alloc_limit == nullptr);
        _ASSERTE(this->alloc_ptr == nullptr);
    }

    region = LargeRegion();
    if (region != nullptr)
    {
        if (region->IsAllocating())
        {
            region->StopAllocating(/* allocPtr */ 0);
        }

        if (detach)
        {
            region->DetachFromContext();
        }

        recycler->AddEphemeralRegion(region);
    }
 }
