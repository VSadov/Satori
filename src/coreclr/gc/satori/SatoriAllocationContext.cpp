// Copyright (c) 2022 Vladimir Sadov
//
// Permission is hereby granted, free of charge, to any person
// obtaining a copy of this software and associated documentation
// files (the "Software"), to deal in the Software without
// restriction, including without limitation the rights to use,
// copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following
// conditions:
//
// The above copyright notice and this permission notice shall be
// included in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
// OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
// HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
// WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
// OTHER DEALINGS IN THE SOFTWARE.
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
            region->DetachFromContextRelease();
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
            region->DetachFromContextRelease();
        }

        recycler->AddEphemeralRegion(region);
    }
 }
