// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.
//
// SatoriGCHeap.h
//

#ifndef __SATORI_REGION_QUEUE_H__
#define __SATORI_REGION_QUEUE_H__

#include "common.h"
#include "../gc.h"
#include "SatoriLock.h"

class SatoriRegion;

class SatoriRegionQueue
{
public:
    SatoriRegion* TryPop();
    SatoriRegion* TryPop(size_t regionSize, SatoriRegion* &putBack);

    bool TryRemove(SatoriRegion* region);
    SatoriRegion* TryRemove(size_t regionSize, SatoriRegion*& putBack);

    void Push(SatoriRegion* region);
    void Enqueue(SatoriRegion* region);
    bool Contains(SatoriRegion* region);

    SatoriRegionQueue() :
        m_lock(), m_head(), m_tail()
    {
        m_lock.Initialize();
    };

    bool CanPop()
    {
        return m_head != nullptr;
    }

    bool CanDequeue()
    {
        return m_tail != nullptr;
    }

private:
    SatoriLock m_lock;
    SatoriRegion* m_head;
    SatoriRegion* m_tail;
};

#endif
