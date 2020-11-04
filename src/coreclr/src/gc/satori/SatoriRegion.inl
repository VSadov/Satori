// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.
//
// SatoriRegion.inl
//

#ifndef __SATORI_REGION_INL__
#define __SATORI_REGION_INL__

#include "common.h"

#include "gcenv.h"
#include "../env/gcenv.os.h"
#include "../env/gcenv.ee.h"
#include "SatoriRegion.h"

inline bool SatoriRegion::IsThreadLocal()
{
    return m_ownerThreadTag;
}

inline bool SatoriRegion::OwnedByCurrentThread()
{
    return m_ownerThreadTag == SatoriUtil::GetCurrentThreadTag();
}

inline int SatoriRegion::Generation()
{
    return m_generation;
}

inline void SatoriRegion::SetGeneration(int generation)
{
    m_generation = generation;
}

inline bool SatoriRegion::IsAllocating()
{
    return m_allocEnd != 0;
}

inline size_t SatoriRegion::Start()
{
    return (size_t)this;
}

inline size_t SatoriRegion::End()
{
    return m_end;
}

inline size_t SatoriRegion::Size()
{
    return End() - Start();
}

inline size_t SatoriRegion::AllocStart()
{
    return m_allocStart;
}

inline size_t SatoriRegion::AllocRemaining()
{
    // reserve Satori::MIN_FREE_SIZE to be able to make the unused space parseable
    ptrdiff_t diff = m_allocEnd - m_allocStart - Satori::MIN_FREE_SIZE;
    return diff > 0 ? (size_t)diff : 0;
}

inline SatoriObject* SatoriRegion::FirstObject()
{
    return &m_firstObject;
}

inline void SatoriRegion::StopEscapeTracking()
{
    m_ownerThreadTag = 0;
    m_escapeFunc = nullptr;
}

template<typename F>
void SatoriRegion::ForEachFinalizable(F& lambda)
{
    size_t items = 0;
    size_t nulls = 0;
    SatoriMarkChunk* chunk = m_finalizables;
    while (chunk)
    {
        items += chunk->Count();
        for (size_t i = 0; i < chunk->Count(); i++)
        {
            SatoriObject* finalizable = chunk->Item(i);
            if (finalizable == nullptr)
            {
                nulls++;
                continue;
            }

            SatoriObject* newFinalizable = lambda(finalizable);
            if (newFinalizable != finalizable)
            {
                chunk->Item(i) = newFinalizable;
                if (newFinalizable == nullptr)
                {
                    nulls++;
                }
            }
        }

        chunk = chunk->Next();
    }

    // typically the list is short, so having some dead entries is not a big deal.
    // compacting at 1/2 occupancy should be good enough (
    // (50% overhead limit at O(N) maintenance cost)
    if (nulls * 2 >= items)
    {
       CompactFinalizables();
    }
}

inline bool SatoriRegion::HasFinalizables()
{
    return m_finalizables;
}

inline bool& SatoriRegion::HasPendingFinalizables()
{
    return m_hasPendingFinalizables;
}

#endif
