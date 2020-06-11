// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.
//
// SatoriGCHeap.cpp
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
         RegularRegion()->Deactivate(heap);
         RegularRegion() = nullptr;
     }

     if (LargeRegion() != nullptr)
     {
         LargeRegion()->Deactivate(heap);
         LargeRegion() = nullptr;
     }
     //TODO: VS also maybe allocated bytes accounting?
 }
