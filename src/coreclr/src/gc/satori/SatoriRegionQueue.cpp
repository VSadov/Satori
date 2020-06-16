// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.
//
// SatoriRegionQueue.cpp
//

#include "common.h"

#include "gcenv.h"
#include "../env/gcenv.os.h"
#include "SatoriRegionQueue.h"

#include "SatoriLock.h"
#include "SatoriUtil.h"

#include "SatoriRegion.h"

void SatoriRegionQueue::Push(SatoriRegion* region)
{
    _ASSERTE(region->ValidateBlank());

    SatoriLockHolder<SatoriLock> holder(&m_lock);
    region->m_containingQueue = this;
    if (m_head == nullptr)
    {
        _ASSERTE(m_tail == nullptr);
        m_head = m_tail = region;
    }
    else
    {
        region->m_next = m_head;
        m_head->m_prev = region;
        m_head = region;
    }
}

void SatoriRegionQueue::Enqueue(SatoriRegion* region)
{
    _ASSERTE(region->ValidateBlank());

    SatoriLockHolder<SatoriLock> holder(&m_lock);
    region->m_containingQueue = this;
    if (m_tail == nullptr)
    {
        _ASSERTE(m_head == nullptr);
        m_head = m_tail = region;
    }
    else
    {
        region->m_prev = m_tail;
        m_tail->m_next = region;
        m_tail = region;
    }
}

SatoriRegion* SatoriRegionQueue::TryPop()
{
    SatoriRegion* result;
    {
        SatoriLockHolder<SatoriLock> holder(&m_lock);
        result = m_head;
        if (result == nullptr)
        {
            return nullptr;
        }

        result->m_containingQueue = nullptr;
        m_head = result->m_next;
        if (m_head == nullptr)
        {
            m_tail = nullptr;
        }
        else
        {
            m_head->m_prev = nullptr;
        }
    }

    _ASSERTE(result->m_prev == nullptr);
    result->m_next = nullptr;

    _ASSERTE(result->ValidateBlank());
    return result;
}

SatoriRegion* SatoriRegionQueue::TryPop(size_t regionSize, SatoriRegion*& putBack)
{
    m_lock.Enter();

    SatoriRegion* result = m_head;
    if (result == nullptr)
    {
        m_lock.Leave();
        return nullptr;
    }

    _ASSERTE(result->ValidateBlank());

    // TODO: VS  put back the assert when bucketized.
    // _ASSERTE(result->Size() >= regionSize);
    if (result->Size() < regionSize)
    {
        m_lock.Leave();
        return nullptr;
    }

    if (result->Size() - SatoriUtil::RoundDownPwr2(result->Size()) > regionSize)
    {
        // inplace case
        // split "size" and return as a new region
        size_t nextStart, nextCommitted, nextZeroInitedAfter;
        result->SplitCore(regionSize, nextStart, nextCommitted, nextZeroInitedAfter);
        _ASSERTE(result->ValidateBlank());
        m_lock.Leave();

        result = SatoriRegion::InitializeAt(result->m_containingPage, nextStart, regionSize, nextCommitted, nextZeroInitedAfter);
        _ASSERTE(result->ValidateBlank());
        putBack = nullptr;
    }
    else 
    {
        // take out the result
        result->m_containingQueue = nullptr;
        m_head = result->m_next;
        if (m_head == nullptr)
        {
            m_tail = nullptr;
        }
        else
        {
            m_head->m_prev = nullptr;
        }

        m_lock.Leave();

        if (result->Size() > regionSize)
        {
            // if there is a diff split it off and put back to appropriate queue.
            putBack = result->Split(result->Size() - regionSize);
        }

        _ASSERTE(result->m_prev == nullptr);
        result->m_next = nullptr;
    }

    return result;
}

SatoriRegion* SatoriRegionQueue::TryRemove(size_t regionSize, SatoriRegion*& putBack)
{
    m_lock.Enter();

    SatoriRegion* result = m_head;
    while (true)
    {
        if (result == nullptr)
        {
            m_lock.Leave();
            return nullptr;
        }

        if (result->Size() >= regionSize)
        {
            break;
        }

        result = result->m_next;
    }

    _ASSERTE(result->ValidateBlank());

    if (result->Size() - SatoriUtil::RoundDownPwr2(result->Size()) > regionSize)
    {
        // inplace case
        // split "size" and return as a new region
        size_t nextStart, nextCommitted, nextZeroInitedAfter;
        result->SplitCore(regionSize, nextStart, nextCommitted, nextZeroInitedAfter);
        _ASSERTE(result->ValidateBlank());
        m_lock.Leave();

        result = SatoriRegion::InitializeAt(result->m_containingPage, nextStart, regionSize, nextCommitted, nextZeroInitedAfter);
        _ASSERTE(result->ValidateBlank());
        putBack = nullptr;
    }
    else
    {
        // take out the result
        result->m_containingQueue = nullptr;
        if (result->m_prev == nullptr)
        {
            m_head = result->m_next;
        }
        else
        {
            result->m_prev->m_next = result->m_next;
        }

        if (result->m_next == nullptr)
        {
            m_tail = result->m_prev;
        }
        else
        {
            result->m_next->m_prev = result->m_prev;
        }

        m_lock.Leave();

        if (result->Size() > regionSize)
        {
            // if there is a diff split it off and put back to appropriate queue.
            putBack = result->Split(result->Size() - regionSize);
        }

        result->m_prev = nullptr;
        result->m_next = nullptr;
    }

    return result;
}

bool SatoriRegionQueue::Contains(SatoriRegion* region)
{
    return region->m_containingQueue == this;
}

bool SatoriRegionQueue::TryRemove(SatoriRegion* region)
{
    {
        SatoriLockHolder<SatoriLock> holder(&m_lock);
        if (!Contains(region))
        {
            return false;
        }

        region->m_containingQueue = nullptr;
        if (region->m_prev == nullptr)
        {
            m_head = region->m_next;
        }
        else
        {
            region->m_prev->m_next = region->m_next;
        }

        if (region->m_next == nullptr)
        {
            m_tail = region->m_prev;
        }
        else
        {
            region->m_next->m_prev = region->m_prev;
        }
    }

    region->m_next = nullptr;
    region->m_prev = nullptr;
    return true;
}
