// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.
//
// SatoriRecycler.cpp
//

#include "common.h"

#include "gcenv.h"
#include "../env/gcenv.os.h"

#include "SatoriHeap.h"
#include "SatoriRecycler.h"
#include "SatoriRegion.h"
#include "SatoriRegion.inl"

void SatoriRecycler::Initialize(SatoriHeap* heap)
{
    m_heap = heap;
}

void SatoriRecycler::AddRegion(SatoriRegion* region)
{
    // TODO: VS make end parsable?

    // TODO: VS verify

    region->Publish();
    // TODO: VS leak the region for now

    // TODO: VS for now count and once have 5, lets mark.
}
