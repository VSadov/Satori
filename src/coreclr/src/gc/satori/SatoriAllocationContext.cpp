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

class SatoriHeap;

void SatoriAllocationContext::OnTerminateThread(SatoriHeap* heap)
{
    if (RegularRegion() != nullptr)
    {
        this->alloc_bytes -= this->alloc_limit - this->alloc_ptr;

        //TODO: VS make parseable
        //TODO: VS check for emptiness, mark, sweep, compact, slice, ...

        RegularRegion()->Deactivate(heap);
        RegularRegion() = nullptr;
    }

    if (LargeRegion() != nullptr)
    {
        //TODO: VS check for emptiness, mark, sweep, compact, slice, ...
        LargeRegion()->Deactivate(heap);
        LargeRegion() = nullptr;
    }
 }
