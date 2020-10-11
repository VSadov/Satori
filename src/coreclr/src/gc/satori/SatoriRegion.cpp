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
#include "SatoriPage.h"
#include "SatoriPage.inl"
#include "SatoriQueue.h"
#include "SatoriMarkChunk.h"

#ifdef memcpy
#undef memcpy
#endif //memcpy

#if !defined(_DEBUG)
// #pragma optimize("gty", on)
#endif

SatoriRegion* SatoriRegion::InitializeAt(SatoriPage* containingPage, size_t address, size_t regionSize, size_t committed, size_t used)
{
    _ASSERTE(used <= committed);
    _ASSERTE(regionSize % Satori::REGION_SIZE_GRANULARITY == 0);

    SatoriRegion* result = (SatoriRegion*)address;

    committed = max(address, committed);
    used = max(address, used);

    // make sure the header is comitted
    ptrdiff_t toCommit = (size_t)&result->m_syncBlock - committed;
    if (toCommit > 0)
    {
        toCommit = ALIGN_UP(toCommit, Satori::CommitGranularity());

        if (!GCToOSInterface::VirtualCommit((void*)committed, toCommit))
        {
            return nullptr;
        }
        committed += toCommit;
    }

    _ASSERTE(BITMAP_START * sizeof(size_t) == offsetof(SatoriRegion, m_firstObject) / sizeof(size_t) / 8);

    // clear the header if was used before
    size_t zeroUpTo = min(used, (size_t)&result->m_syncBlock);
    memset((void*)address, 0, zeroUpTo - address);

    // consider the header dirty for the future zeroing.
    used = max((size_t)&result->m_syncBlock, used);

    result->m_end = address + regionSize;
    result->m_committed = min(committed, address + regionSize);
    result->m_used = min(used, address + regionSize);
    result->m_containingPage = containingPage;

    result->m_allocStart = (size_t)&result->m_firstObject;
    result->m_allocEnd = result->End();

    result->m_containingPage->RegionInitialized(result);
    result->m_escapeFunc = EscapeFn;
    return result;
}

SatoriAllocator* SatoriRegion::Allocator()
{
    return m_containingPage->Heap()->Allocator();
}

void SatoriRegion::WipeCards()
{
    m_containingPage->WipeCardsForRange(Start(), End());
}

void SatoriRegion::MakeBlank()
{
    m_ownerThreadTag = 0;
    m_escapeFunc = EscapeFn;
    m_generation = 0;
    m_allocStart = (size_t)&m_firstObject;
    m_allocEnd = End();
    m_occupancy = 0;
    m_markStack = 0;

    _ASSERT(!m_hasPendingFinalizables);
    _ASSERT(!m_finalizables);
    _ASSERTE(NothingMarked());

    //clear index
    if (m_used > (size_t)&m_index)
    {
        memset(&m_index, 0, sizeof(m_index));
    }

#ifdef _DEBUG
    // memset(&m_syncBlock, 0xFE, m_used - (size_t)&m_syncBlock);
#endif
}

bool SatoriRegion::ValidateBlank()
{
    if (!IsAllocating())
    {
        _ASSERTE(false);
        return false;
    }

    if (Start() < m_containingPage->RegionsStart() || End() > m_containingPage->End())
    {
        _ASSERTE(false);
        return false;
    }

    if (Size() % Satori::REGION_SIZE_GRANULARITY != 0)
    {
        _ASSERTE(false);
        return false;
    }

    if (m_committed > End() || m_committed < (size_t)&m_syncBlock)
    {
        _ASSERTE(false);
        return false;
    }

    if (m_used > m_committed || m_used < (size_t)&m_syncBlock)
    {
        _ASSERTE(false);
        return false;
    }

    if (m_allocStart != (size_t)&m_firstObject || m_allocEnd != m_end)
    {
        _ASSERTE(false);
        return false;
    }

    for (int i = 0; i < Satori::INDEX_LENGTH; i += Satori::INDEX_LENGTH / 4)
    {
        _ASSERTE(m_index[i] == nullptr);
        if (m_index[i] != nullptr)
        {
            return false;
        }
    }

    if (m_finalizables)
    {
        _ASSERTE(false);
        return false;
    }

    return true;
}

void SatoriRegion::StopAllocating(size_t allocPtr)
{
    _ASSERTE(IsAllocating());

    if (allocPtr)
    {
        _ASSERTE(allocPtr <= m_allocStart);
        m_allocStart = allocPtr;
    }

    // make unused allocation span parseable
    if (m_allocStart != m_allocEnd)
    {
        m_used = max(m_used, m_allocStart + Satori::MIN_FREE_SIZE);
        SatoriObject::FormatAsFree(m_allocStart, m_allocEnd - m_allocStart);
    }

    m_allocStart = m_allocEnd = 0;
}

void SatoriRegion::Deactivate(SatoriHeap* heap, size_t allocPtr)
{
    StopAllocating(allocPtr);
    heap->Recycler()->AddRegion(this);
}

void SatoriRegion::SplitCore(size_t regionSize, size_t& nextStart, size_t& nextCommitted, size_t& nextUsed)
{
    _ASSERTE(regionSize % Satori::REGION_SIZE_GRANULARITY == 0);
    _ASSERTE(regionSize < this->Size());
    _ASSERTE(m_allocEnd == 0 || m_allocEnd == m_end);
    _ASSERTE((size_t)m_allocStart < m_end - regionSize - Satori::MIN_FREE_SIZE);

    size_t newEnd = m_end - regionSize;
    nextStart = newEnd;
    nextCommitted = m_committed;
    nextUsed = m_used;

    m_end = newEnd;
    m_committed = min(newEnd, m_committed);
    m_used = min(newEnd, m_used);
    m_allocEnd = min(m_allocEnd, newEnd);

    _ASSERTE(Size() >= Satori::REGION_SIZE_GRANULARITY);
}

SatoriRegion* SatoriRegion::Split(size_t regionSize)
{
    size_t nextStart, nextCommitted, nextUsed;
    SplitCore(regionSize, nextStart, nextCommitted, nextUsed);

    // format the rest as a new region
    SatoriRegion* result = InitializeAt(m_containingPage, nextStart, regionSize, nextCommitted, nextUsed);
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

    m_end = next->m_end;
    m_committed = next->m_committed;
    m_used = next->m_used;
    m_allocEnd = next->m_allocEnd;

    m_containingPage->RegionInitialized(this);
}

size_t SatoriRegion::Allocate(size_t size, bool zeroInitialize)
{
    _ASSERTE(m_containingQueue == nullptr);

    size_t chunkStart = m_allocStart;
    size_t chunkEnd = chunkStart + size;
    if (chunkEnd <= m_allocEnd - Satori::MIN_FREE_SIZE)
    {
        // commit a bit more - in case we need to append a parseability terminator.
        size_t ensureCommitted = chunkEnd + Satori::MIN_FREE_SIZE;
        if (ensureCommitted > m_committed)
        {
            size_t newComitted = ALIGN_UP(ensureCommitted, Satori::CommitGranularity());
            if (!GCToOSInterface::VirtualCommit((void*)m_committed, newComitted - m_committed))
            {
                return 0;
            }

            m_committed = newComitted;
        }

        if (zeroInitialize)
        {
            size_t zeroUpTo = min(m_used, chunkEnd);
            ptrdiff_t zeroInitCount = zeroUpTo - chunkStart;
            if (zeroInitCount > 0)
            {
                memset((void*)chunkStart, 0, zeroInitCount);
            }
        }

        m_allocStart = chunkEnd;
        m_used = max(m_used, chunkEnd);
        return chunkStart;
    }

    return 0;
}

size_t SatoriRegion::AllocateHuge(size_t size, bool zeroInitialize)
{
    _ASSERTE(ValidateBlank());
    _ASSERTE(AllocRemaining() >= size);
    _ASSERTE(m_allocEnd > Start() + Satori::REGION_SIZE_GRANULARITY);

    size_t chunkStart;
    size_t chunkEnd;

    // if we can have enough space in front for a large object without committing,
    // put the object as far as possible without committing.
    if (AllocStart() + Satori::LARGE_OBJECT_THRESHOLD + size + Satori::MIN_FREE_SIZE < m_committed)
    {
        chunkStart = m_committed - Satori::MIN_FREE_SIZE - size;
        // ensure enough space at start to be replacable with a free obj in the same tile.
        chunkStart = min(chunkStart, Start() + Satori::REGION_SIZE_GRANULARITY - Satori::MIN_FREE_SIZE);
    }
    else
    {
        chunkStart = m_allocStart;
    }

    // in rare cases the object does not cross into the last tile. (when free obj padding is on the edge).
    // in such case force the object to cross.
    if (End() - (chunkStart + size) >= Satori::REGION_SIZE_GRANULARITY)
    {
        chunkStart += Satori::LARGE_OBJECT_THRESHOLD + Satori::MIN_FREE_SIZE;
    }

    chunkEnd = chunkStart + size;

    // huge allocation should cross the end of the first tile, but have enough space between
    // to be replacable with a free obj without using the next tile.
    _ASSERTE(chunkEnd > Start() + Satori::REGION_SIZE_GRANULARITY);
    _ASSERTE(chunkStart <= Start() + Satori::REGION_SIZE_GRANULARITY - Satori::MIN_FREE_SIZE);

    size_t ensureCommitted = chunkEnd + Satori::MIN_FREE_SIZE;
    if (ensureCommitted > m_committed)
    {
        size_t newComitted = ALIGN_UP(ensureCommitted, Satori::CommitGranularity());
        if (!GCToOSInterface::VirtualCommit((void*)m_committed, newComitted - m_committed))
        {
            return 0;
        }

        m_committed = newComitted;
    }

    if (zeroInitialize)
    {
        size_t zeroInitStart = chunkStart;
        size_t zeroUpTo = min(m_used, chunkEnd);
        ptrdiff_t zeroInitCount = zeroUpTo - zeroInitStart;
        if (zeroInitCount > 0)
        {
            memset((void*)zeroInitStart, 0, zeroInitCount);
        }
    }

    // if did not allocate from start, there could be some fillable space before the obj.
    m_allocEnd = chunkStart;

    // space after chunkEnd is a waste, no normal object can start there.
    // just make a free gap there for parseability.
    m_used = max(m_used, chunkEnd + Satori::MIN_FREE_SIZE);
    SatoriObject::FormatAsFreeAfterHuge(chunkEnd, End() - chunkEnd);

    _ASSERTE(m_allocEnd < Start() + Satori::REGION_SIZE_GRANULARITY);
    return chunkStart;
}

inline int LocationToIndex(size_t location)
{
    return (location >> Satori::INDEX_GRANULARITY_BITS) % Satori::INDEX_LENGTH;
}

//TODO: VS need FindObjectChecked when a real object must be found.
SatoriObject* SatoriRegion::FindObject(size_t location)
{
    _ASSERTE(location >= Start() && location <= End());

    // start search from the first object or after unparseable alloc gap
    SatoriObject* obj = (IsAllocating() && (location >= m_allocEnd)) ?
                                 (SatoriObject*)m_allocEnd :
                                 FirstObject();

    // use a better start obj if we have that in the index 
    int limit = LocationToIndex(obj->Start());
    for (int current = LocationToIndex(location); current > limit; current--)
    {
        if (m_index[current] != 0)
        {
            obj = m_index[current];
            if (obj->End() > location)
            {
                return obj;
            }

            break;
        }
    }

    // walk to the location and update index on the way
    SatoriObject* next = obj->Next();
    while (next->Start() <= location)
    {
        // if object straddles index granules, record it in the indices
        if ((obj->Start() ^ next->Start()) >> Satori::INDEX_GRANULARITY_BITS)
        {
            for (size_t l = obj->Start(); l < next->Start(); l += Satori::INDEX_GRANULARITY)
            {
                m_index[LocationToIndex(l) + 1] = obj;
            }
        }

        obj = next;
        next = next->Next();
    }

    return obj;
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

    if (!o->IsMarked())
    {
        region->PushToMarkStack(o);
        o->SetMarked();
    }

    if (flags & GC_CALL_PINNED)
    {
        if (!o->IsEscaped())
        {
            o->SetPinned();
        }
    }
}

inline void SatoriRegion::PushToMarkStack(SatoriObject* obj)
{
    _ASSERTE(obj->ContainingRegion() == this);
    _ASSERTE(!obj->GetNextInMarkStack());

    if (obj->RawGetMethodTable()->ContainsPointersOrCollectible())
    {
        obj->SetNextInMarkStack(m_markStack);
        _ASSERTE(m_markStack == obj->GetNextInMarkStack());
        m_markStack = (int32_t)(obj->Start() - this->Start());
    }
}

inline SatoriObject* SatoriRegion::PopFromMarkStack()
{
    if (m_markStack != 0)
    {
        SatoriObject* obj = SatoriObject::At(this->Start() + m_markStack);
        m_markStack = obj->GetNextInMarkStack();
        obj->ClearNextInMarkStack();
        return obj;
    }

    return nullptr;
}

SatoriObject* SatoriRegion::ObjectForMarkBit(size_t bitmapIndex, int offset)
{
    size_t objOffset = (bitmapIndex * sizeof(size_t) * 8 + offset) * sizeof(size_t);
    return SatoriObject::At(Start() + objOffset);
}

bool SatoriRegion::IsExposed(SatoriObject** location)
{
    _ASSERTE(((SatoriObject*)location)->ContainingRegion() == this);

    //TODO: VS optimize by getting the bit directly?
    return ((SatoriObject*)location)->IsMarked();
}

void SatoriRegion::SetExposed(SatoriObject** location)
{
    _ASSERTE(((SatoriObject*)location)->ContainingRegion() == this);

    //TODO: VS optimize by getting the bit directly?
    return ((SatoriObject*)location)->SetMarked();
}

void SatoriRegion::EscapeRecursively(SatoriObject* o)
{
    _ASSERTE(this->OwnedByCurrentThread());
    _ASSERTE(o->ContainingRegion() == this);

    if (o->IsEscaped())
    {
        return;
    }

    o->SetEscaped();

    // now recursively mark all the objects reachable from escaped object.
    do
    {
        _ASSERTE(o->IsEscaped());
        o->ForEachObjectRef(
            [&](SatoriObject** ref)
            {
                // no refs should be exposed yet, except if the ref is the first field,
                // we use that to escape whole object.
                _ASSERTE(!IsExposed(ref) || ((size_t)o == (size_t)ref - sizeof(size_t)));

                // mark ref location as exposed
                SetExposed(ref);

                // recursively escape all currently reachable objects
                SatoriObject* child = *ref;
                if (child->ContainingRegion() == this && !child->IsEscaped())
                {
                    child->SetEscaped();
                    PushToMarkStack(child);
                }
            }
        );
        o = PopFromMarkStack();
    } while (o);
}

void SatoriRegion::EscapeFn(SatoriObject** dst, SatoriObject* src, SatoriRegion* region)
{
    region->EscapeRecursively(src);
}

void SatoriRegion::ThreadLocalMark()
{
    Verify();

    bool checkFinalizables = m_finalizables != nullptr;

    // since this region is thread-local, we have only two kinds of live roots:
    //- those that are reachable from the current stack and
    //- those that are reachable from outside of the region (escaped)

    // mark escaped objects:
    // for every set escape bit, set the corresponding mark bit
    // nothing should be marked in the region, so we can find escapes via bit scan
    size_t bitmapIndex = BITMAP_START;
    int markBitOffset = 0;
    while (bitmapIndex < BITMAP_LENGTH)
    {
        DWORD step;
        if (BitScanForward64(&step, m_bitmap[bitmapIndex] >> markBitOffset))
        {
            // got an escape bit. its mark bit is at -1
            markBitOffset += step - 1;

            // set the mark bit
            m_bitmap[bitmapIndex + (markBitOffset >> 6)] |= ((size_t)1 << (markBitOffset & 63));

            SatoriObject* obj = ObjectForMarkBit(bitmapIndex, markBitOffset);
            obj->Validate();

            // skip the object
            markBitOffset = obj->Next()->MarkBitOffset(&bitmapIndex);
        }
        else
        {
            // skip empty mark words
            markBitOffset = 0;
            while (++bitmapIndex < BITMAP_LENGTH && m_bitmap[bitmapIndex] == 0) {}
        }
    }

    // mark roots for the current stack
    ScanContext sc;
    sc.promotion = TRUE;
    sc._unused1 = this;
    GCToEEInterface::GcScanCurrentStackRoots((promote_func*)MarkFn, &sc);

    // now recursively mark all the objects reachable from the stack roots.
    SatoriObject* o = PopFromMarkStack();

 propagateMarks:
    while (o)
    {
        _ASSERTE(o->IsMarked());
        _ASSERTE(!o->IsEscaped());
        _ASSERTE(!o->IsFree());
        o->ForEachObjectRef(
            [this](SatoriObject** ref)
            {
                SatoriObject* child = *ref;
                if (child->ContainingRegion() == this && !child->IsMarked())
                {
                    child->SetMarked();
                    PushToMarkStack(child);
                }
            },
            /* includeCollectibleAllocator */ true
        );
        o = PopFromMarkStack();
    }

    if (checkFinalizables)
    {
        // unreachable finalizables:
        // - if finalized eagerly or suppressed -> drop from the list, this is garbage now.
        // else
        // - mark ref as finalizer pending to be queued after compaction.
        // - mark obj as reachable
        // - push to mark stack
        // - re-trace to mark children that are now F-reachable
        ForEachFinalizable(
            [this](SatoriObject* finalizable)
            {
                _ASSERTE(((size_t)finalizable & Satori::FINALIZATION_PENDING) == 0);
                if (!finalizable->IsMarked())
                {
                    // NOTE: if finalization is suppressed or runs eagerly, we do not need to track any longer.
                    //       The object was never scheduled (or it'd be escaped) and now unreachable so noone can re-register it.
                    if (finalizable->IsFinalizationSuppressed() ||
                        GCToEEInterface::EagerFinalized(finalizable))
                    {
                        // stop tracking
                        return (SatoriObject*)nullptr;
                    }
                    else
                    {
                        // keep alive
                        finalizable->SetMarked();
                        PushToMarkStack(finalizable);
                        (size_t&)finalizable |= Satori::FINALIZATION_PENDING;
                        HasPendingFinalizables() = true;
                    }
                }

                return finalizable;
            }
        );

        o = PopFromMarkStack();
        if (o)
        {
            checkFinalizables = false;
            goto propagateMarks;
        }
    }

    Verify(/*allowMarked*/ true);
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
    // we will shift left the movable objects - as long as they fit between unmovables
    //

    // what we want to move
    SatoriObject* moveable;
    size_t moveableSize = 0;

    // space to move to
    size_t dstStart;
    size_t dstEnd = 0;

    size_t relocated = 0;
    size_t free = 0;

    // moveable: starts at first movable and reachable, as long as there is any free space to slide in
    size_t lastMarkedEnd = FirstObject()->Start();
    moveable = SkipUnmarked(FirstObject());
    while (true)
    {
        size_t skipped = moveable->Start() - lastMarkedEnd;
        if (skipped)
        {
            if (free == 0)
            {
                // new gap from here
                dstEnd = lastMarkedEnd;
            }

            free += skipped;
            SatoriObject::FormatAsFree(lastMarkedEnd, skipped);
        }

        if (moveable->Start() >= End())
        {
            // nothing left to move
            _ASSERTE(moveable->Start() == End());
            return relocated;
        }

        moveableSize = moveable->Size();
        if (!moveable->IsEscapedOrPinned() && free > 0)
        {
            // reachable and moveable and saw free space. we can move this one.
            break;
        }

        lastMarkedEnd = moveable->Start() + moveableSize;
        moveable = SkipUnmarked((SatoriObject*)lastMarkedEnd);
    }

    while (true)
    {
        // dst start: skip unmovables
        for (dstStart = dstEnd; dstStart < End(); dstStart += ((SatoriObject*)dstStart)->Size())
        {
            if (!((SatoriObject*)dstStart)->IsEscapedOrPinned())
            {
                break;
            }
        }

        if (dstStart >= End())
        {
            // no more space could be found
            return relocated;
        }

        // dst end: skip until next unmovable
        for (dstEnd = dstStart; dstEnd < End(); dstEnd = SkipUnmarked(((SatoriObject*)dstEnd)->Next())->Start())
        {
            if (((SatoriObject*)dstEnd)->IsEscapedOrPinned())
            {
                break;
            }
        }

        // plan relocs for movables as long as there is space
        while (dstEnd - dstStart >= moveableSize + Satori::MIN_FREE_SIZE ||
            dstEnd - dstStart == moveableSize)
        {
            _ASSERTE(moveable->Start() >= dstStart);
            int32_t reloc = (int32_t)(moveable->Start() - dstStart);
            if (reloc != 0)
            {
                relocated += moveableSize;
                moveable->SetReloc(reloc);
            }

            dstStart += moveableSize;

            // moveable: skip unmarked or pinned
            while (true)
            {
                lastMarkedEnd = moveable->Start() + moveableSize;
                moveable = SkipUnmarked((SatoriObject*)lastMarkedEnd);

                size_t skipped = moveable->Start() - lastMarkedEnd;
                if (skipped)
                {
                    free += skipped;
                    SatoriObject::FormatAsFree(lastMarkedEnd, skipped);
                }

                if (moveable->Start() >= End())
                {
                    // nothing left to move
                    _ASSERTE(moveable->Start() == End());
                    return relocated;
                }

                moveableSize = moveable->Size();
                if (!moveable->IsEscapedOrPinned())
                {
                    // reachable and moveable. we can move this.
                    break;
                }
            }
        }

        // here we have something to move, but it does not fit in the current gap.
        // we need to find another gap
        // Note: gap will not cross ahead of the current moveable, because moveables are valid gaps.
        //       the gap may reach the current movable, which would result in a trivial 0-distance move
    }
}

void SatoriRegion::UpdateFn(PTR_PTR_Object ppObject, ScanContext* sc, uint32_t flags)
{
    SatoriRegion* region = (SatoriRegion*)sc->_unused1;
    size_t location = (size_t)*ppObject;

    // ignore objects otside of the current region.
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
        _ASSERTE(((SatoriObject*)(*ppObject))->ContainingRegion() == region);
    }
};

void SatoriRegion::ThreadLocalUpdatePointers()
{
    // update stack roots
    ScanContext sc;
    sc.promotion = FALSE;
    sc._unused1 = this;
    GCToEEInterface::GcScanCurrentStackRoots((promote_func*)UpdateFn, &sc);

    // go through all live objets and update what they reference.
    size_t bitmapIndex = BITMAP_START;
    int markBitOffset = 0;
    while (bitmapIndex < BITMAP_LENGTH)
    {
        DWORD step;
        if (BitScanForward64(&step, m_bitmap[bitmapIndex] >> markBitOffset))
        {
            // got reachable object
            markBitOffset += step;

            int escapedOffset = markBitOffset + 1;
            bool escaped = m_bitmap[bitmapIndex + (escapedOffset >> 6)] & ((size_t)1 << (escapedOffset & 63));
            SatoriObject* obj = ObjectForMarkBit(bitmapIndex, markBitOffset);
            if (escaped)
            {
                _ASSERTE(ObjectForMarkBit(bitmapIndex, markBitOffset)->GetReloc() == 0);
            }
            else
            {
                obj->ForEachObjectRef(
                    [this](SatoriObject** ppObject)
                    {
                        SatoriObject* o = *ppObject;
                        // ignore objects otside of the current region, we are not relocating those.
                        // (this also rejects nulls)
                        if (o->ContainingRegion() == this)
                        {
                            size_t reloc = o->GetReloc();
                            if (reloc)
                            {
                                *ppObject = (SatoriObject*)((size_t)*ppObject - reloc);
                                _ASSERTE((*ppObject)->ContainingRegion() == this);
                            }
                        }
                    }
                );
            }

            // skip the object
            markBitOffset = obj->Next()->MarkBitOffset(&bitmapIndex);
        }
        else
        {
            // skip empty mark words
            markBitOffset = 0;
            while (++bitmapIndex < BITMAP_LENGTH && m_bitmap[bitmapIndex] == 0) {}
        }
    }

    // update finalizables if we have them
    if (m_finalizables)
    {
        ForEachFinalizable(
            [this](SatoriObject* finalizable)
            {
                // save the pending bit, will reapply back later
                size_t finalizePending = (size_t)finalizable & Satori::FINALIZATION_PENDING;
                (size_t&)finalizable &= ~Satori::FINALIZATION_PENDING;

                size_t reloc = finalizable->GetReloc();
                if (reloc)
                {
                    finalizable = (SatoriObject*)((size_t)finalizable - reloc);
                    _ASSERTE(finalizable->ContainingRegion() == this);
                }

                (size_t&)finalizable |= finalizePending;
                return finalizable;
            }
        );
    }
}

SatoriObject* SatoriRegion::SkipUnmarked(SatoriObject* from)
{
    _ASSERTE(from->Start() < End());
    size_t bitmapIndex;
    int markBitOffset = from->MarkBitOffset(&bitmapIndex);

    DWORD offset;
    if (BitScanForward64(&offset, m_bitmap[bitmapIndex] >> markBitOffset))
    {
        // got reachable object.
        markBitOffset += offset;
    }
    else
    {
        markBitOffset = 0;
        while (bitmapIndex < SatoriRegion::BITMAP_LENGTH)
        {
            bitmapIndex++;
            if (BitScanForward64(&offset, m_bitmap[bitmapIndex]))
            {
                // got reachable object.
                markBitOffset = offset;
                break;
            }
        }
    }

    return ObjectForMarkBit(bitmapIndex, markBitOffset);
}

SatoriObject* SatoriRegion::SkipUnmarked(SatoriObject* from, size_t upTo)
{
    _ASSERTE(from->Start() < End());
    size_t bitmapIndex;
    int markBitOffset = from->MarkBitOffset(&bitmapIndex);

    DWORD offset;
    if (BitScanForward64(&offset, m_bitmap[bitmapIndex] >> markBitOffset))
    {
        // got reachable object.
        markBitOffset += offset;
    }
    else
    {
        size_t limit = (upTo - Start()) >> 9;
        _ASSERTE(limit <= SatoriRegion::BITMAP_LENGTH);
        markBitOffset = 0;
        while (bitmapIndex < limit)
        {
            bitmapIndex++;
            if (BitScanForward64(&offset, m_bitmap[bitmapIndex]))
            {
                // got reachable object.
                markBitOffset = offset;
                break;
            }
        }
    }

    SatoriObject* result = ObjectForMarkBit(bitmapIndex, markBitOffset);
    if (result->Start() > upTo)
    {
        result = SatoriObject::At(upTo);
    }

    _ASSERTE(result->Start() <= upTo);
    return result;
}

bool SatoriRegion::Sweep()
{
    size_t limit = Start() + Satori::REGION_SIZE_GRANULARITY;
    if (End() > limit)
    {
        SatoriObject* last = FindObject(limit - 1);
        if (!last->IsMarked())
        {
            SatoriObject::FormatAsFree(last->Start(), limit - last->Start());
            SatoriRegion* other = this->Split(Size() - Satori::REGION_SIZE_GRANULARITY);
            other->WipeCards();
            other->MakeBlank();
            Allocator()->ReturnRegion(other);
        }
    }

    bool sawMarked = false;
    SatoriObject* cur = FirstObject();

    do
    {
        if (cur->IsMarked())
        {
            sawMarked = true;
            cur = cur->Next();
            continue;
        }

        size_t lastMarkedEnd = cur->Start();
        cur = SkipUnmarked(cur);
        size_t skipped = cur->Start() - lastMarkedEnd;
        if (skipped)
        {
            SatoriObject::FormatAsFree(lastMarkedEnd, skipped);
        }
    }
    while (cur->Start() < limit);

    // clean index
    memset(&m_index, 0, sizeof(m_index));

    return !sawMarked;
}


bool SatoriRegion::NothingMarked()
{
    for (size_t bitmapIndex = BITMAP_START; bitmapIndex < BITMAP_LENGTH; bitmapIndex++)
    {
        size_t chunk = m_bitmap[bitmapIndex];
        if (chunk)
        {
            return false;
        }
    }

    return true;
}

bool SatoriRegion::ThreadLocalCompact(size_t desiredFreeSpace)
{
    SatoriObject* s1;
    SatoriObject* s2 = FirstObject();
    SatoriObject* d1 = FirstObject();
    SatoriObject* d2 = FirstObject();

    size_t foundFree = 0;

    // when d1 reaches the end everything is copied or swept
    while (d1->Start() < End())
    {
        // find next relocatable object
        int32_t reloc = 0;
        // "Next" is ok here because we coalesce unmarked objs when planning.
        for (s1 = s2; s1->Start() < End(); s1 = s1->Next())
        {
            reloc = s1->GetReloc();
            _ASSERTE(reloc >= 0);
            if (reloc != 0)
            {
                break;
            }
        }

        // sweep all objects up to the relocation target
        size_t relocTarget = s1->Start() - reloc;
        while (d1->Start() < relocTarget)
        {
            _ASSERTE(d1->Start() <= d2->Start());
            d2 = SkipUnmarked(d2, relocTarget);

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
            s2->ClearMarkCompactStateForRelocation();
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

    // schedule pending finalizables if we have them
    if (HasPendingFinalizables())
    {
        HasPendingFinalizables() = false;

        // TODO: VS do we have to pend CF after regular finalizables?
        //       can it be observed?
        ForEachFinalizable(
            [this](SatoriObject* finalizable)
            {
                if ((size_t)finalizable & Satori::FINALIZATION_PENDING)
                {
                    (size_t&)finalizable &= ~Satori::FINALIZATION_PENDING;

                    // we are escaping the finalizable to the queue from which it is globally reachable
                    EscapeRecursively(finalizable);

                    _ASSERTE(!finalizable->IsFinalizationSuppressed());
                    if (m_containingPage->Heap()->FinalizationQueue()->TryScheduleForFinalization(finalizable))
                    {
                        // this tracker has served its purpose.
                        finalizable = nullptr;
                    }
                }

                return finalizable;
            }
        );

        GCToEEInterface::EnableFinalization(true);
    }

    Verify();

    memset(&m_index, 0, sizeof(m_index));

    m_occupancy = Satori::REGION_SIZE_GRANULARITY - foundFree;

    return (foundFree >= desiredFreeSpace + Satori::MIN_FREE_SIZE);
}

bool SatoriRegion::RegisterForFinalization(SatoriObject* finalizable)
{
    if (!m_finalizables || !m_finalizables->TryPush(finalizable))
    {
        SatoriMarkChunk* markChunk = Allocator()->TryGetMarkChunk();
        if (!markChunk)
        {
            // OOM
            return false;
        }

        markChunk->SetNext(m_finalizables);
        m_finalizables = markChunk;
        markChunk->Push(finalizable);
    }

    return true;
}

void SatoriRegion::CompactFinalizables()
{
    SatoriMarkChunk* oldFinalizables = m_finalizables;
    m_finalizables = nullptr;

    while (oldFinalizables)
    {
        SatoriMarkChunk* current = oldFinalizables;
        oldFinalizables = oldFinalizables->Next();

        if (m_finalizables)
        {
            while (m_finalizables->HasSpace() && current->Count())
            {
                SatoriObject* o = current->Pop();
                if (o)
                {
                    m_finalizables->Push(o);
                }
            }
        }

        if (current->Count() > 0 && current->Compact() > 0)
        {
            current->SetNext(m_finalizables);
            m_finalizables = current;
            continue;
        }

        Allocator()->ReturnMarkChunk(current);
    }
}

void SatoriRegion::CleanMarks()
{
    memset(&m_bitmap[BITMAP_START], 0, (BITMAP_LENGTH - BITMAP_START) * sizeof(size_t));
}

void SatoriRegion::Verify(bool allowMarked)
{
#ifdef _DEBUG
    for (size_t i = m_used; i < m_committed; i++)
    {
        _ASSERTE(*(uint8_t*)i == 0);
    }

    SatoriObject* prevPrevObj = nullptr;
    SatoriObject* prevObj = nullptr;
    SatoriObject* obj = FirstObject();

    while (obj->Start() < End())
    {
        obj->Validate();

        if (obj->Start() - Start() > Satori::REGION_SIZE_GRANULARITY)
        {
            _ASSERTE(obj->IsFree());
        }
        else
        {
            _ASSERTE(allowMarked || !obj->IsMarked());
        }

        prevPrevObj = prevObj;
        prevObj = obj;
        obj = obj->Next();
    }

    _ASSERTE(obj->Start() == End());
#endif
}
