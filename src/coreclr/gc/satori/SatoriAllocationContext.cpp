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
    if (RegularRegion() != nullptr)
    {
        SatoriRegion* region = RegularRegion();
        size_t allocPtr = (size_t)this->alloc_ptr;

        this->alloc_bytes -= this->alloc_limit - this->alloc_ptr;
        this->alloc_limit = this->alloc_ptr = nullptr;

        if (region->IsAllocating())
        {
            region->StopAllocating(allocPtr);
        }

        if (detach || !region->IsThreadLocal())
        {
            region->PromoteToGen1();
            RegularRegion() = nullptr;
        }

        recycler->AddEphemeralRegion(region);
    }
    else
    {
        _ASSERTE(this->alloc_limit == nullptr);
        _ASSERTE(this->alloc_ptr == nullptr);
    }

    if (LargeRegion() != nullptr)
    {
        SatoriRegion* region = LargeRegion();

        if (region->IsAllocating())
        {
            region->StopAllocating(/* allocPtr */ 0);
        }

        if (detach)
        {
            region->PromoteToGen1();
            LargeRegion() = nullptr;
        }

        recycler->AddEphemeralRegion(region);
    }
 }
