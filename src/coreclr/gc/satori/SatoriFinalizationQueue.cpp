// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.
//
// SatoriFinalizationQueue.cpp
//

#include "common.h"
#include "gcenv.h"
#include "../env/gcenv.os.h"
#include "../env/gcenv.interlocked.h"

#include "SatoriFinalizationQueue.h"
#include "SatoriObject.h"

void SatoriFinalizationQueue::Initialize()
{
    m_enqueue = 0;
    m_dequeue = 0;
    m_scanTicket = 0;
    m_sizeMask = (1 << 20) - 1;
    m_data = new Entry[1 << 20];
    for (int i = 0; i <= m_sizeMask; i++)
    {
        m_data[i].version = i;
    }
}

bool SatoriFinalizationQueue::TryUpdateScanTicket(int currentScanTicket)
{
    int queueScanTicket = VolatileLoadWithoutBarrier(&m_scanTicket);
    if (queueScanTicket != currentScanTicket)
    {
        // claim the queue for scanning
        if (Interlocked::CompareExchange(&m_scanTicket, currentScanTicket, queueScanTicket) == queueScanTicket)
        {
            return true;
        }
    }

    return false;
}


// We are using MPSC circular queue with item versioning inspired by the algorithm outlined at:
// http://www.1024cores.net/home/lock-free-algorithms/queues/bounded-mpmc-queue

// called by multiple producers concurrently with a consumer
bool SatoriFinalizationQueue::TryScheduleForFinalization(SatoriObject* finalizable)
{
    Entry* e;
    int enq = m_enqueue;
    for (;;)
    {
        e = &m_data[enq & m_sizeMask];
        int version = VolatileLoad(&e->version);
        int diff = version - enq;

        if (diff == 0)
        {
            if (Interlocked::CompareExchange(&m_enqueue, enq + 1, enq) == enq)
            {
                break;
            }
        }
        else if (diff < 0)
        {
            return false;
        }

        YieldProcessor();
        enq = m_enqueue;
    }

    e->value = finalizable;
    VolatileStore(&e->version, enq + 1);
    return true;
}

// called by STW GC when no other producers or consumers are running.
bool SatoriFinalizationQueue::TryScheduleForFinalizationSTW(SatoriObject* finalizable)
{
    int enq = m_enqueue;
    Entry* e = &m_data[enq & m_sizeMask];
    int diff = e->version - enq;

    if (diff < 0)
    {
        // TODO: VS handle resize?
        return false;
    }

    // we are the only enqueuer, we cannot be behind.
    _ASSERTE(diff == 0);
    m_enqueue = enq + 1;
    e->value = finalizable;
    e->version = enq + 1;
    return true;
}

// called by a single consuming thread
SatoriObject* SatoriFinalizationQueue::TryGetNextItem()
{
    int deq = m_dequeue;
    Entry* e = &m_data[deq & m_sizeMask];

    int version = VolatileLoad(&e->version);
    SatoriObject* value = e->value;

    int diff = version - (deq + 1);
    if (diff < 0)
    {
        value = nullptr;
    }
    else
    {
        // we are the only dequeuer, we cannot be behind.
        _ASSERTE(diff == 0);
        m_dequeue = deq + 1;
        VolatileStore(&e->version, deq + m_sizeMask + 1);
    }

    return value;
}

// returns whether the queue has items
// only makes sense in quiescent state
bool SatoriFinalizationQueue::HasItems()
{
    return m_enqueue != m_dequeue;
}
