// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.
//
// SatoriRegion.cpp
//

#include "common.h"

#include "gcenv.h"
#include "../env/gcenv.os.h"
#include "../env/gcenv.ee.h"
#include "SatoriGC.h"
#include "SatoriAllocator.h"
#include "SatoriRecycler.h"
#include "SatoriUtil.h"
#include "SatoriObject.h"
#include "SatoriObject.inl"
#include "SatoriRegion.h"
#include "SatoriRegion.inl"

#ifdef memcpy
#undef memcpy
#endif //memcpy

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
    // Since the actual object data is shifted by 1 syncblock, a region could fit sizeof(size_t) more,
    // but that would complicate math in few places so we would rather ignore 0.0001% space loss.
    result->m_allocEnd = result->End();

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
    m_allocEnd = End();

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

    if (m_allocStart != (size_t)&m_firstObject || m_allocEnd != m_end)
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

    SatoriObject::FormatAsFree(m_allocStart, AllocSize());
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
    return (size_t)m_firstObject.Next() > m_end;
}

void SatoriRegion::SplitCore(size_t regionSize, size_t& nextStart, size_t& nextCommitted, size_t& nextZeroInitedAfter)
{
    _ASSERTE(regionSize % Satori::REGION_SIZE_GRANULARITY == 0);
    _ASSERTE(regionSize < this->Size());
    _ASSERTE(m_allocEnd == m_end);
    _ASSERTE((size_t)m_allocStart < m_end - regionSize - Satori::MIN_FREE_SIZE);

    size_t newEnd = m_end - regionSize;
    nextStart = newEnd;
    nextCommitted = m_committed;
    nextZeroInitedAfter = m_zeroInitedAfter;

    m_end = newEnd;
    m_committed = min(newEnd, m_committed);
    m_zeroInitedAfter = min(newEnd, m_zeroInitedAfter);
    m_allocEnd = min(newEnd, m_allocEnd);

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

SatoriObject* SatoriRegion::FindObject(size_t location)
{
    _ASSERTE(location >= (size_t)FirstObject() && location <= End());

    // TODO: VS use index to start
    SatoriObject* obj = FirstObject();
    SatoriObject* next = obj->Next();

    while (next->Start() < location)
    {
        obj = next;
        next = next->Next();
    }

    return obj->IsFree() ? nullptr : obj;
}

void SatoriRegion::MarkFn(PTR_PTR_Object ppObject, ScanContext* sc, uint32_t flags)
{
    SatoriRegion* region = (SatoriRegion*)sc->_unused1;
    size_t location = (size_t)*ppObject;

    // ignore objects outside of the current region.
    // this also rejects nulls.
    if (location < (size_t)region->FirstObject() || location > region->End())
    {
        return;
    }

    SatoriObject* o;
    if (flags & GC_CALL_INTERIOR)
    {
        o = region->FindObject(location);
        if (o == nullptr)
        {
            return;
        }
    }
    else
    {
        o = SatoriObject::At(location);
    }

    if (flags & GC_CALL_PINNED)
    {
        o->SetPinnedAndMarked();
    }
    else
    {
        o->SetMarked();
    }
};

void SatoriRegion::PushToMarkStack(SatoriObject* obj)
{
    _ASSERTE(obj->ContainingRegion() == this);

    if (obj->GetNextInMarkStack())
    {
        // already in the stack
        return;
    }

    obj->SetNextInMarkStack(m_markStack);
    int32_t t = obj->GetNextInMarkStack();
    ASSERT(t == m_markStack);
    m_markStack = (int32_t)((size_t)obj - (size_t)this);
}

SatoriObject* SatoriRegion::PopFromMarkStack()
{
    if (m_markStack != 0)
    {
        SatoriObject* obj = (SatoriObject*)((size_t)this + m_markStack);
        m_markStack = obj->GetNextInMarkStack();
        obj->ClearNextInMarkStack();
        return obj;
    }

    return nullptr;
}

void SatoriRegion::ThreadLocalMark()
{
    ScanContext sc;
    sc.promotion = TRUE;
    sc._unused1 = this;

    Verify();
    GCToEEInterface::GcScanCurrentStackRoots((promote_func*)MarkFn, &sc); 

    Verify();
    for (SatoriObject* obj = FirstObject(); obj->Start() < End(); obj = obj->Next())
    {
        if (obj->IsFree())
        {
            continue;
        }

        SatoriObject* o = obj;
        _ASSERTE(!o->IsFree());

        do
        {
            if (o->IsEscaped())
            {
                o->ForEachObjectRef(
                    [=](SatoriObject** ref)
                    {
                        SatoriObject* child = *ref;
                        if (child->ContainingRegion() == this && !child->IsEscaped())
                        {
                            child->SetEscaped();
                            if (child->Start() < o->Start())
                            {
                                PushToMarkStack(child);
                            }
                        }
                    }
                );
            }
            else if (o->IsMarked())
            {
                o->ForEachObjectRef(
                    [=](SatoriObject** ref)
                    {
                        SatoriObject* child = *ref;
                        if (child->ContainingRegion() == this && !child->IsEscapedOrMarked())
                        {
                            child->SetMarked();
                            if (child->Start() < o->Start())
                            {
                                PushToMarkStack(child);
                            }
                        }
                    }
                );
            }

            o = PopFromMarkStack();
        }
        while (o);
    }

    Verify();
}

size_t SatoriRegion::ThreadLocalPlan()
{
    // object can be in 3 states here:
    //  - Free:
    //      not marked or escaped (incl. free objects, which are never marked or escaped)
    //
    //  - Unmovable:
    //      escaped or pinned
    //
    //  - Movable
    //      marked, but not escaped or pinned
    //
    // we will slide left movable objects into free - as long as they fit.
    //

    size_t dst_end = 0;
    size_t dst_start;
    size_t cur_free = 0;
    SatoriObject* moveable;
    size_t relocated = 0;
    size_t free = 0;
    size_t moveableSize = 0;

    // moveable: starts at first movable and reachable, as long as there is any free space to slide in
    for (moveable = FirstObject(); moveable->Start() < End(); moveable = (SatoriObject*)(moveable->Start() + moveableSize))
    {
        moveableSize = moveable->Size();
        if (!moveable->IsEscapedOrMarked())
        {
            // This is not reachable.
            if (free == 0)
            {
                // new gap from here
                dst_end = moveable->Start();
            }

            free += moveableSize;
            if (!cur_free)
            {
                cur_free = moveable->Start();
            }
        }
        else
        {
            if (cur_free)
            {
                SatoriObject::FormatAsFree(cur_free, moveable->Start() - cur_free);
                cur_free = 0;
            }

            if (!moveable->IsEscapedOrPinned() && free > 0)
            {
                // reachable and moveable and saw free space. we can move.
                break;
            }
        }
    }

    if (moveable->Start() >= End())
    {
        // nothing left to move
        return relocated;
    }

    while (true)
    {
        // dst start: skip unmovables
        for (dst_start = dst_end; dst_start < End(); dst_start+= ((SatoriObject*)dst_start)->Size())
        {
            if (!((SatoriObject*)dst_start)->IsEscapedOrPinned())
            {
                break;
            }
        }

        if (dst_start >= End())
        {
            // no more space could be found
            return relocated;
        }

        // dst end: skip untill next unmovable
        for (dst_end = dst_start; dst_end < End(); dst_end += ((SatoriObject*)dst_end)->Size())
        {
            if (((SatoriObject*)dst_end)->IsEscapedOrPinned())
            {
                break;
            }
        }

        while (dst_end - dst_start >= moveableSize + Satori::MIN_FREE_SIZE ||
                dst_end - dst_start == moveableSize)
        {
            ASSERT(moveable->Start() >= dst_start);
            int32_t reloc = (int32_t)(moveable->Start() - dst_start);
            if (reloc != 0)
            {
                relocated += moveableSize;
                moveable->SetReloc(reloc);
            }

            dst_start += moveableSize;

            // moveable: skip unmarked or pinned
            while (true)
            {
                // moveable = moveable->Next();
                moveable = (SatoriObject*)(moveable->Start() + moveableSize);
                if (moveable->Start() >= End())
                {
                    // nothing left to move
                    return relocated;
                }

                moveableSize = moveable->Size();
                if (!moveable->IsEscapedOrMarked())
                {
                    // this is not reachable.
                    free += moveableSize;
                    if (!cur_free)
                    {
                        cur_free = moveable->Start();
                    }
                }
                else
                {
                    if (cur_free)
                    {
                        SatoriObject::FormatAsFree(cur_free, moveable->Start() - cur_free);
                        cur_free = 0;
                    }

                    if (!moveable->IsEscapedOrPinned())
                    {
                        // reachable and moveable. we can move.
                        break;
                    }
                }
            }
        }

        // here we have something to move, but it does not fit in the current gap.
        // we need to find another gap
        // Note: gap will not cross over moveable, because moveables are valid gaps.
    }
}

void SatoriRegion::UpdateFn(Object** ppObject, ScanContext* sc, uint32_t flags)
{
    SatoriRegion* region = (SatoriRegion*)sc->_unused1;
    size_t location = (size_t)*ppObject;

    // ignore objects otside current region.
    // this also rejects nulls.
    if (location < (size_t)region->FirstObject() || location > region->End())
    {
        return;
    }

    SatoriObject* o;
    if (flags & GC_CALL_INTERIOR)
    {
        o = region->FindObject(location);
        if (o == nullptr)
        {
            return;
        }
    }
    else
    {
        o = SatoriObject::At(location);
    }

    size_t reloc = o->GetReloc();
    if (reloc)
    {
        *ppObject = (Object*)(((size_t)*ppObject) - reloc);
        ASSERT(((SatoriObject*)(*ppObject))->ContainingRegion() == region);
    }
};

void SatoriRegion::ThreadLocalUpdatePointers()
{
    ScanContext sc;
    sc.promotion = FALSE;
    sc._unused1 = this;

    GCToEEInterface::GcScanCurrentStackRoots((promote_func*)UpdateFn, &sc);

    for (SatoriObject* obj = FirstObject(); obj->Start() < End(); obj = obj->Next())
    {
        // we are not supposed to touch escaped objects
        // we are not interested in unreachable ones
        if (!obj->IsMarked() || obj->IsEscaped())
        {
            ASSERT(obj->GetReloc() == 0);
            continue;
        }

        obj->ForEachObjectRef(
            [&](SatoriObject** ppObject)
            {
                SatoriObject* o = *ppObject;
                // ignore objects otside current region.
                // this also rejects nulls.
                if (o->Start() >= (size_t)this->FirstObject() && o->Start() < this->End())
                {
                    size_t reloc = o->GetReloc();
                    if (reloc)
                    {
                        *ppObject = (SatoriObject*)(((size_t)*ppObject) - reloc);
                        ASSERT(((SatoriObject*)(*ppObject))->ContainingRegion() == this);
                    }
                }
            }
        );
    }
}

bool SatoriRegion::ThreadLocalCompact(size_t desiredFreeSpace)
{
    SatoriObject* s1;
    SatoriObject* s2 = FirstObject();
    SatoriObject* d1 = FirstObject();
    SatoriObject* d2 = FirstObject();

    size_t foundFree = 0;

    // when d1 reaches the end everything is copied or swept
    while(d1->Start() < End())
    {
        // find next relocatable object
        int32_t reloc = 0;
        for (s1 = s2; s1->Start() < End(); s1 = s1->Next())
        {
            reloc = s1->GetReloc();
            if (reloc != 0)
            {
                break;
            }
        }

        // sweep all objects up to the relocation target
        size_t relocTarget = s1->Start() - reloc;
        while (d1->Start() < relocTarget)
        {
            while (d2->Start() < relocTarget)
            {
                if (d2->IsEscapedOrMarked())
                {
                    break;
                }

                d2 = d2->Next();
            }

            if (d1 != d2)
            {
                size_t freeSpace = d2->Start() - d1->Start();
                SatoriObject::FormatAsFree(d1->Start(), freeSpace);
                if (freeSpace > foundFree)
                {
                    this->m_allocStart = d1->Start();
                    this->m_allocEnd = d2->Start();
                    foundFree = freeSpace;
                }
            }
            else
            {
                // clear Mark/Pinned, keep escaped, reloc should be 0, this object will stay around
                d1->ClearPinnedAndMarked();
            }

            d1 = d1->Next();
            d2 = d1;
        }

        // find the end of the run of relocatable objects with the same reloc distance (we can copy all at once)
        for (s2 = s1; s2->Start() < End(); s2 = s2->Next())
        {
            if (reloc != s2->GetReloc())
            {
                break;
            }

            // clear mark and reloc. (escape should not be set, since we are relocating)
            // pre-relocation copy of object need to clear the mark, so that the object could be swept if not overwritten.
            // the relocated copy is not swept so clearing is also correct.
            s2->ClearMarkCompactState();
        }

        // move d2 to the first object after the future end of the relocated run
        // [d1, d2) will be the gap that the run is targeting, but it may not fill the whole gap.
        // for our purposes the extra space is as good as a free object, we will move d1 to that
        size_t relocTargetEnd = s2->Start() - reloc;
        d1 = SatoriObject::At(relocTargetEnd);
        while (d2->Start() < relocTargetEnd)
        {
            d2 = d2->Next();
        }

        if (reloc)
        {
            // copying objects with syncblocks, thus the src/dst adjustment
            memcpy(
                (void*)(relocTarget - sizeof(size_t)),
                (void*)(s1->Start() - sizeof(size_t)),
                s2->Start() - s1->Start());
        }
    }

    return (foundFree >= desiredFreeSpace + Satori::MIN_FREE_SIZE || foundFree == desiredFreeSpace);
}

void SatoriRegion::Verify()
{
#ifdef _DEBUG
    SatoriObject* obj = FirstObject();

    while (obj->Start() < End())
    {
        ASSERT(obj->GetReloc() == 0);
        obj = obj->Next();
    }

    ASSERT(obj->Start() == End());
#endif
}
