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
#include "SatoriMarkChunk.h"

inline bool SatoriRegion::IsEscapeTracking()
{
    _ASSERTE(!m_ownerThreadTag || m_generation == 0);
    return m_ownerThreadTag;
}

inline bool SatoriRegion::IsEscapeTrackingAcquire()
{
    return VolatileLoad(&m_ownerThreadTag);
}

inline bool SatoriRegion::IsEscapeTrackedByCurrentThread()
{
    return m_ownerThreadTag == SatoriUtil::GetCurrentThreadTag();
}

inline int SatoriRegion::Generation()
{
    return m_generation;
}

inline void SatoriRegion::SetGeneration(int generation)
{
    // Gen0 is controlled by thread-local tracking
    _ASSERTE(generation != 0);
    _ASSERTE(m_generation != 0);

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

inline size_t SatoriRegion::RegionSizeForAlloc(size_t allocSize)
{
    return ALIGN_UP(allocSize + offsetof(SatoriRegion, m_firstObject) + Satori::MIN_FREE_SIZE, Satori::REGION_SIZE_GRANULARITY);
}

inline SatoriObject* SatoriRegion::FirstObject()
{
    return &m_firstObject;
}

inline void SatoriRegion::StartEscapeTracking(size_t threadTag)
{
    _ASSERTE(m_generation = -1);
    m_escapeFunc = EscapeFn;
    m_ownerThreadTag = threadTag;
    m_generation = 0;
}

inline void SatoriRegion::StopEscapeTracking()
{
    if (IsEscapeTracking())
    {
        _ASSERTE(!HasPinnedObjects());
        ClearMarks();

        // must clear ownership after clearing marks
        // to make sure concurrent marking does not use dirty mark table
        VolatileStore(&m_ownerThreadTag, (size_t)0);

        m_escapeFunc = nullptr;
        m_generation = 1;
    }
}

template<typename F>
void SatoriRegion::ForEachFinalizable(F lambda)
{
    size_t items = 0;
    size_t nulls = 0;
    SatoriMarkChunk* chunk = m_finalizableTrackers;
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

        // unfilled slots in the tail chunks count as nulls
        if (chunk != m_finalizableTrackers)
        {
            nulls += chunk->FreeSpace();
        }

        chunk = chunk->Next();
    }

    // typically the list is short, so having some dead entries is not a big deal.
    // compacting at 1/2 occupancy should be good enough.
    // (50% max overhead at O(N) maintenance cost)
    if (nulls * 2 > items)
    {
       CompactFinalizableTrackers();
    }
}

// Used by threadlocal GC concurrently with user threads,
// thus must lock - in case a user thread tries to reregister an object for finalization
template<typename F>
void SatoriRegion::ForEachFinalizableThreadLocal(F lambda)
{
    LockFinalizableTrackers();
    ForEachFinalizable(lambda);
    UnlockFinalizableTrackers();
}

inline bool SatoriRegion::EverHadFinalizables()
{
    return m_everHadFinalizables;
}

inline bool& SatoriRegion::HasPendingFinalizables()
{
    return m_hasPendingFinalizables;
}

inline size_t SatoriRegion::Occupancy()
{
    return m_occupancy;
}

inline bool& SatoriRegion::HasPinnedObjects()
{
    return m_hasPinnedObjects;
}

inline bool& SatoriRegion::HasMarksSet()
{
    return m_hasMarksSet;
}

inline bool& SatoriRegion::AcceptedPromotedObjects()
{
    return m_acceptedPromotedObjects;
}

inline bool& SatoriRegion::IsReusable()
{
    return m_isReusable;
}

inline SatoriQueue<SatoriRegion>* SatoriRegion::ContainingQueue()
{
    return VolatileLoadWithoutBarrier(&m_containingQueue);
}

inline void SatoriRegion::Attach(SatoriRegion** attachementPoint)
{
    _ASSERTE(!m_allocationContextAttachmentPoint);
    _ASSERTE(!*attachementPoint);

    *attachementPoint = this;
    m_allocationContextAttachmentPoint = attachementPoint;
}

inline void SatoriRegion::DetachFromContext()
{
    _ASSERTE(*m_allocationContextAttachmentPoint == this);

    *m_allocationContextAttachmentPoint = nullptr;
    // all allocations must be committed prior to detachement.
    VolatileStore(&m_allocationContextAttachmentPoint, (SatoriRegion**)nullptr);
}

inline bool SatoriRegion::IsAttachedToContext()
{
    return m_allocationContextAttachmentPoint;
}

inline bool SatoriRegion::IsAttachedToContextAcquire()
{
    return m_isReusable || VolatileLoad(&m_allocationContextAttachmentPoint);
}

#endif
