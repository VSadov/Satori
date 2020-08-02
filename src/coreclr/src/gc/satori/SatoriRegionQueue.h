// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.
//
// SatoriRegionQueue.h
//

#ifndef __SATORI_REGION_QUEUE_H__
#define __SATORI_REGION_QUEUE_H__

#include "common.h"
#include "../gc.h"
#include "SatoriQueue.h"

class SatoriRegion;

class SatoriRegionQueue : public SatoriQueue<SatoriRegion>
{
public:
    SatoriRegion* TryPopWithSize(size_t regionSize, SatoriRegion* &putBack);
    SatoriRegion* TryRemoveWithSize(size_t regionSize, SatoriRegion*& putBack);
};

#endif
