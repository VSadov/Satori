// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.
//
// SatoriAllocationContext.h
//

#ifndef __SATORI_ALLOCATION_CONTEXT_H__
#define __SATORI_ALLOCATION_CONTEXT_H__

#include "common.h"
#include "../gc.h"

class SatoriRegion;
class SatoriRecycler;

class SatoriAllocationContext : public gc_alloc_context
{
public:
    SatoriRegion*& RegularRegion()
    {
        return (SatoriRegion*&)this->gc_reserved_1;
    }

    SatoriRegion*& LargeRegion()
    {
        return (SatoriRegion*&)this->gc_reserved_2;
    }

    // stop allocating on all associated regions and pass them to recycler.
    void Deactivate(SatoriRecycler* recycler, bool detach);

private:

};

#endif
