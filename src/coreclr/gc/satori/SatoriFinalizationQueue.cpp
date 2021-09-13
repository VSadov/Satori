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
#include "SatoriHeap.h"
#include "SatoriRegion.h"
#include "SatoriRegion.inl"

// limit the queue size to some large value just to have a limit.
// needing this many items is likely an indication that finalizer is not keeping up and nothing can help that.
static const int MAX_SIZE = 1 << 25;

#if _DEBUG
// smaller size in debug to have overflows
static const int INITIAL_SIZE = 1 << 5;
#else
// 4K items (65Kb) - roughly the size of 2 region headers
static const int INITIAL_SIZE = 1 << 12;
#endif

void SatoriFinalizationQueue::Initialize(SatoriHeap* heap)
{
    m_enqueue = 0;
    m_dequeue = 0;
    m_scanTicket = 0;
    m_overflowedGen = 0;
    m_heap = heap;

    int size = INITIAL_SIZE;

    m_sizeMask = size - 1;
    size_t allocSize = size * sizeof(Entry);
    size_t regionSize = SatoriRegion::RegionSizeForAlloc(allocSize);
    m_region = m_heap->Allocator()->GetRegion(regionSize);
    m_data = (Entry*)m_region->Allocate(allocSize, /*zeroInitialize*/false);

    for (int i = 0; i < size; i++)
    {
        m_data[i].version = i;
    }
}

int SatoriFinalizationQueue::OverflowedGen()
{
    return m_overflowedGen;
}


void SatoriFinalizationQueue::SetOverflow(int generation)
{
    if (generation > m_overflowedGen)
    {
        m_overflowedGen = generation;
    }
}

void SatoriFinalizationQueue::ResetOverflow(int generation)
{
    if (!m_overflowedGen || generation < m_overflowedGen)
    {
        return;
    }

    // try resizing
    int size = (m_sizeMask + 1) * 2;
    if (size <= MAX_SIZE)
    {
        size_t allocSize = size * sizeof(Entry);
        size_t regionSize = SatoriRegion::RegionSizeForAlloc(allocSize);
        SatoriRegion* newRegion = m_heap->Allocator()->GetRegion(regionSize);
        if (newRegion)
        {
            newRegion->TryDecommit();
            Entry* newData = (Entry*)newRegion->Allocate(allocSize, /*zeroInitialize*/false);
            if (!newData)
            {
                m_heap->Allocator()->AddRegion(newRegion);
                return;
            }

            // transfer items from the old queue
            int dst = 0;
            for (int src = m_dequeue; src != m_enqueue; src++)
            {
                newData[dst].value = m_data[src & m_sizeMask].value;
                newData[dst].version = dst + 1;
                dst++;
            }

            // format the rest of the items as empty
            for (int i = dst; i < size; i++)
            {
                newData[i].version = i;
            }

            m_data = newData;
            m_sizeMask = size - 1;
            m_enqueue = dst;
            m_dequeue = 0;

            m_region->MakeBlank();
            m_heap->Allocator()->AddRegion(m_region);
            m_region = newRegion;
        }
    }

    // either way we are not in an overflow unless we see a full queue again.
    m_overflowedGen = 0;
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
        // since there is only one dequeuer, it cannot fall behind another one.
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
