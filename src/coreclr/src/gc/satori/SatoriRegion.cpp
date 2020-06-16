// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.
//
// SatoriGCHeap.cpp
//

#include "common.h"

#include "gcenv.h"
#include "../env/gcenv.os.h"
#include "../env/gcenv.ee.h"
#include "SatoriGCHeap.h"
#include "SatoriAllocator.h"
#include "SatoriRecycler.h"
#include "SatoriUtil.h"
#include "SatoriRegion.h"

SatoriRegion* SatoriRegion::InitializeAt(SatoriPage* containingPage, size_t address, size_t regionSize, size_t committed, size_t zeroInitedAfter)
{
    _ASSERTE(zeroInitedAfter <= committed);
    _ASSERTE(regionSize % Satori::REGION_SIZE_GRANULARITY == 0);

    committed = max(address, committed);
    zeroInitedAfter = max(address, zeroInitedAfter);

    SatoriRegion* result = (SatoriRegion*)address;

    ptrdiff_t toCommit = address + sizeof(SatoriRegion) - committed;
    if (toCommit > 0)
    {
        _ASSERTE(GCToOSInterface::GetPageSize() < Satori::REGION_SIZE_GRANULARITY);
        toCommit = ALIGN_UP(toCommit, GCToOSInterface::GetPageSize());

        if (!GCToOSInterface::VirtualCommit((void*)committed, toCommit))
        {
            return nullptr;
        }
        committed += toCommit;
    }

    result->m_state = SatoriRegionState::allocating;
    result->m_end = address + regionSize;
    result->m_committed = min(committed, address + regionSize);
    result->m_zeroInitedAfter = min(max(zeroInitedAfter, address + sizeof(SatoriRegion)), result->End());
    result->m_containingPage = containingPage;

    result->m_next = result->m_prev = nullptr;
    result->m_containingQueue = nullptr;

    result->m_allocStart = (size_t)&result->m_firstObject;
    // +1 for syncblock
    result->m_allocEnd = result->End() + 1;

    if (zeroInitedAfter > (size_t)&result->m_index)
    {
        ZeroMemory(&result->m_index, offsetof(SatoriRegion, m_syncBlock) - offsetof(SatoriRegion, m_index));
    }

    result->m_containingPage->RegionAdded(address);
    return result;
}

void SatoriRegion::MakeBlank()
{
    m_state = SatoriRegionState::allocating;
    m_allocStart = (size_t)&m_firstObject;
    // +1 for syncblock
    m_allocEnd = m_end + 1;

    if (m_zeroInitedAfter > (size_t)&m_index)
    {
        ZeroMemory(&m_index, offsetof(SatoriRegion, m_syncBlock) - offsetof(SatoriRegion, m_index));
    }
}

bool SatoriRegion::ValidateBlank()
{
    if (!IsAllocating())
    {
        return false;
    }

    if (Start() < m_containingPage->RegionsStart() || End() > m_containingPage->End())
    {
        return false;
    }

    if (Size() % Satori::REGION_SIZE_GRANULARITY != 0)
    {
        return false;
    }

    if (m_committed > End() || m_committed < Start() + sizeof(SatoriRegion))
    {
        return false;
    }

    if (m_zeroInitedAfter > m_committed || m_zeroInitedAfter < Start() + sizeof(SatoriRegion))
    {
        return false;
    }

    if (m_allocStart != (size_t)&m_firstObject || m_allocEnd != m_end + 1)
    {
        return false;
    }

    for (int i = 0; i < Satori::INDEX_ITEMS; i += Satori::INDEX_ITEMS / 4)
    {
        if (m_index[i] != nullptr)
        {
            return false;
        }
    }

    if (m_syncBlock != 0)
    {
        return false;
    }

    return true;
}

void SatoriRegion::StopAllocating()
{
    _ASSERTE(IsAllocating());

    SatoriUtil::MakeFreeObject((Object*)m_allocStart, AllocSize());
    // TODO: VS update index
    m_allocEnd = 0;
}

void SatoriRegion::Deactivate(SatoriHeap* heap)
{
    StopAllocating();

    // local collect (leaves it parsable \w updated index)

    if (IsEmpty())
    {
        this->MakeBlank();
        heap->Allocator()->ReturnRegion(this);
        return;
    }

    // TODO: VS: if can be splitted, split and return tail
    heap->Recycler()->AddRegion(this);
}

bool SatoriRegion::IsEmpty()
{
    _ASSERTE(!IsAllocating());
    return (size_t)SatoriUtil::Next(&m_firstObject) > m_end;
}

void SatoriRegion::SplitCore(size_t regionSize, size_t& nextStart, size_t& nextCommitted, size_t& nextZeroInitedAfter)
{
    _ASSERTE(regionSize % Satori::REGION_SIZE_GRANULARITY == 0);
    _ASSERTE(regionSize < this->Size());
    _ASSERTE(m_allocEnd == m_end + 1);
    _ASSERTE((size_t)m_allocStart < m_end - regionSize - Satori::MIN_FREE_SIZE);

    size_t newEnd = m_end - regionSize;
    nextStart = newEnd;
    nextCommitted = m_committed;
    nextZeroInitedAfter = m_zeroInitedAfter;

    m_end = newEnd;
    m_committed = min(newEnd, m_committed);
    m_zeroInitedAfter = min(newEnd, m_zeroInitedAfter);
    m_allocEnd = min(newEnd + 1, m_allocEnd);

    _ASSERTE(Size() >= Satori::REGION_SIZE_GRANULARITY);
    _ASSERTE(IsAllocating());
}

SatoriRegion* SatoriRegion::Split(size_t regionSize)
{
    size_t nextStart, nextCommitted, nextZeroInitedAfter;
    SplitCore(regionSize, nextStart, nextCommitted, nextZeroInitedAfter);

    // format the rest as a new region
    SatoriRegion* result = InitializeAt(m_containingPage, nextStart, regionSize, nextCommitted, nextZeroInitedAfter);
    _ASSERTE(result->ValidateBlank());
    return result;
}

bool SatoriRegion::CanCoalesce(SatoriRegion* other)
{
    return m_committed == other->Start();
}

void SatoriRegion::Coalesce(SatoriRegion* next)
{
    _ASSERTE(ValidateBlank());
    _ASSERTE(next->ValidateBlank());
    _ASSERTE(next->m_prev == next->m_next);
    _ASSERTE(next->m_containingQueue == nullptr);
    _ASSERTE(next->m_containingPage == m_containingPage);
    _ASSERTE(CanCoalesce(next));

    m_containingPage->RegionDestroyed((size_t)next);

    m_end = next->m_end;
    m_committed = next->m_committed;
    m_zeroInitedAfter = next->m_zeroInitedAfter;
m_allocEnd = next->m_allocEnd;
}

size_t SatoriRegion::Allocate(size_t size, bool ensureZeroInited)
{
    _ASSERTE(m_containingQueue == nullptr);
    size_t allocStart = m_allocStart;
    size_t allocEnd = m_allocStart + size;
    if (allocEnd <= m_allocEnd - Satori::MIN_FREE_SIZE || allocEnd == m_allocEnd)
    {
        if (allocEnd > m_committed)
        {
            // TODO: VS commit quantum should be a static (ensure it is power of 2 and < 2Mb, otherwise committ whole pages)
            size_t newComitted = ALIGN_UP(allocEnd, GCToOSInterface::GetPageSize());
            if (!GCToOSInterface::VirtualCommit((void*)m_committed, newComitted - m_committed))
            {
                return 0;
            }

            m_committed = newComitted;
        }

        if (ensureZeroInited)
        {
            size_t zeroInitStart = allocStart;
            size_t zeroInitEnd = min(m_zeroInitedAfter, allocEnd);
            ptrdiff_t zeroInitCount = zeroInitEnd - zeroInitStart;
            if (zeroInitCount > 0)
            {
                ZeroMemory((void*)zeroInitStart, zeroInitCount);
            }
        }

        size_t result = m_allocStart;
        m_allocStart = allocEnd;
        m_zeroInitedAfter = max(m_zeroInitedAfter, allocEnd);
        return result;
    }

    return 0;
}

size_t SatoriRegion::AllocateHuge(size_t size, bool ensureZeroInited)
{
    _ASSERTE(ValidateBlank());
    _ASSERTE(AllocSize() >= size);

    size_t allocStart;
    size_t allocEnd;
    if (AllocSize() - size > Satori::LARGE_OBJECT_THRESHOLD)
    {
        allocEnd = max(m_allocStart + size, m_committed);
        allocStart = allocEnd - size;
    }
    else
    {
        allocStart = m_allocStart;
        allocEnd = m_allocStart + size;
    }

    _ASSERTE(allocEnd > Start() + Satori::REGION_SIZE_GRANULARITY);

    if (allocEnd > m_committed)
    {
        // TODO: VS commit quantum should be a static (ensure it is power of 2 and < 2Mb, otherwise commit whole pages)
        size_t newComitted = ALIGN_UP(allocEnd, GCToOSInterface::GetPageSize());
        if (!GCToOSInterface::VirtualCommit((void*)m_committed, newComitted - m_committed))
        {
            return 0;
        }

        m_committed = newComitted;
    }

    // if did not allocate from start, there could be some fillable space before the obj.
    m_allocEnd = allocStart;
    if (ensureZeroInited)
    {
        size_t zeroInitStart = allocStart;
        size_t zeroInitEnd = min(m_zeroInitedAfter, allocEnd);
        ptrdiff_t zeroInitCount = zeroInitEnd - zeroInitStart;
        if (zeroInitCount > 0)
        {
            ZeroMemory((void*)zeroInitStart, zeroInitCount);
        }
    }

    //TODO: VS update index?
    size_t result = m_allocStart;
    m_zeroInitedAfter = max(m_zeroInitedAfter, allocEnd);
    return result;
}

Object* SatoriRegion::FindObject(size_t location)
{
    _ASSERTE(location >= (size_t)FistObject() && location <= End());

    // TODO: VS use index
    Object* current = FistObject();
    Object* next = SatoriUtil::Next(current);
    while ((size_t)next <= location)
    {
        current = next;
        next = SatoriUtil::Next(current);
    }

    return SatoriUtil::IsFreeObject(current) ? nullptr : current;
}

void SatoriRegion::MarkFn(PTR_PTR_Object ppObject, ScanContext* sc, uint32_t flags)
{
    SatoriRegion* region = (SatoriRegion*)sc->_unused1;
    size_t location = (size_t)*ppObject;

    if (location < (size_t)region->FistObject() || location > region->End())
    {
        return;
    }

    Object* obj;
    if (flags & GC_CALL_INTERIOR)
    {
        obj = region->FindObject(location);
        if (obj == nullptr)
        {
            return;
        }
    }
    else
    {
        obj = (Object*)location;
    }


    if (flags & GC_CALL_PINNED)
    {
        // PinObject(obj);
    }

    // MarkObject(obj);
};

void SatoriRegion::MarkThreadLocal()
{
    ScanContext sc;
    sc.promotion = TRUE;
    sc._unused1 = this;

    GCToEEInterface::GcScanCurrentStackRoots((promote_func*)MarkFn, &sc);
}
