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
#include "SatoriRegion.inl"

SatoriRegion* SatoriRegionQueue::TryPopWithSize(size_t regionSize, SatoriRegion*& putBack)
{
    m_lock.Enter();

    SatoriRegion* result = m_head;
    if (result == nullptr)
    {
        m_lock.Leave();
        return nullptr;
    }

    _ASSERTE(result->ValidateBlank());
    _ASSERTE(result->Size() >= regionSize);

    if (result->Size() - SatoriUtil::RoundDownPwr2(result->Size()) > regionSize)
    {
        // inplace case
        // split "size" and return as a new region
        size_t nextStart, nextCommitted, nextUsed;
        result->SplitCore(regionSize, nextStart, nextCommitted, nextUsed);
        SatoriPage* containgPage = result->ContainingPage();
        _ASSERTE(result->ValidateBlank());
        m_lock.Leave();

        result = SatoriRegion::InitializeAt(containgPage, nextStart, regionSize, nextCommitted, nextUsed);
        _ASSERTE(result->ValidateBlank());
        putBack = nullptr;
    }
    else 
    {
        // take out the result
        m_count--;
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

        _ASSERTE(result->m_prev == nullptr);
        result->m_next = nullptr;

        if (result->Size() > regionSize)
        {
            // if there is a diff, split what is needed and put the rest back to appropriate queue.
            putBack = result;
            result = putBack->Split(regionSize);
        }
    }

    return result;
}

SatoriRegion* SatoriRegionQueue::TryRemoveWithSize(size_t regionSize, SatoriRegion*& putBack)
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
        size_t nextStart, nextCommitted, nextUsed;
        result->SplitCore(regionSize, nextStart, nextCommitted, nextUsed);
        SatoriPage* containgPage = result->ContainingPage();
        _ASSERTE(result->ValidateBlank());
        m_lock.Leave();

        result = SatoriRegion::InitializeAt(containgPage, nextStart, regionSize, nextCommitted, nextUsed);
        _ASSERTE(result->ValidateBlank());
        putBack = nullptr;
    }
    else
    {
        // take out the result
        m_count--;
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

        result->m_prev = nullptr;
        result->m_next = nullptr;

        if (result->Size() > regionSize)
        {
             // if there is a diff, split what is needed and put the rest back to appropriate queue.
            putBack = result;
            result = putBack->Split(regionSize);
        }
    }

    return result;
}
