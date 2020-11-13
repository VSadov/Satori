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
    _ASSERT(!m_hasPendingFinalizables);
    _ASSERT(!m_finalizables);
    _ASSERTE(NothingMarked());

    WipeCards();

    m_ownerThreadTag = 0;
    m_escapeFunc = EscapeFn;
    m_generation = 0;
    m_allocStart = (size_t)&m_firstObject;
    m_allocEnd = End();
    m_occupancy = 0;
    m_escapeCounter = 0;
    m_markStack = 0;

    //clear index and free list
    memset(&m_freeLists, 0, sizeof(m_freeLists));
    memset(&m_index, 0, sizeof(m_index));

#ifdef _DEBUG
    memset(&m_syncBlock, 0xFE, m_used - (size_t)&m_syncBlock);
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

    if (!ValidateIndexEmpty())
    {
        return false;
    }

    if (m_finalizables)
    {
        _ASSERTE(false);
        return false;
    }

    return true;
}

bool SatoriRegion::ValidateIndexEmpty()
{
    for (int i = 0; i < Satori::INDEX_LENGTH; i += Satori::INDEX_LENGTH / 4)
    {
        _ASSERTE(m_index[i] == nullptr);
        if (m_index[i] != nullptr)
        {
            return false;
        }
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
        SatoriObject* freeObj = SatoriObject::FormatAsFree(m_allocStart, m_allocEnd - m_allocStart);
        AddFreeSpace(freeObj);
    }

    m_allocStart = m_allocEnd = 0;
}

static const int FREE_LIST_NEXT_OFFSET = sizeof(ArrayBase);

void SatoriRegion::AddFreeSpace(SatoriObject* freeObj)
{
    size_t size = freeObj->Size();
    if (size < Satori::MIN_FREELIST_SIZE)
    {
        return;
    }

    DWORD bucket;
    BitScanReverse64(&bucket, size);
    bucket -= (Satori::MIN_FREELIST_SIZE_BITS);
    _ASSERTE(bucket >= 0);
    _ASSERTE(bucket < Satori::FREELIST_COUNT);

    *(SatoriObject**)(freeObj->Start() + FREE_LIST_NEXT_OFFSET) = m_freeLists[bucket];
    m_freeLists[bucket] = freeObj;
}

size_t SatoriRegion::StartAllocating(size_t minSize)
{
    _ASSERTE(!IsAllocating());

    DWORD bucket;
    BitScanReverse64(&bucket, minSize);

    // when minSize is not a power of two we could search through the current bucket,
    // which may have a large enough obj,
    // but we will just use the next bucket, which guarantees that any obj will fit
    if (minSize & (minSize - 1))
    {
        bucket++;
    }

    bucket = bucket > Satori::MIN_FREELIST_SIZE_BITS ?
        bucket - Satori::MIN_FREELIST_SIZE_BITS :
        0;

    for (; bucket < Satori::FREELIST_COUNT; bucket++)
    {
        SatoriObject* freeObj = m_freeLists[bucket];
        if (freeObj)
        {
            m_freeLists[bucket] = *(SatoriObject**)(freeObj->Start() + FREE_LIST_NEXT_OFFSET);
            m_allocStart = freeObj->Start();
            m_allocEnd = freeObj->End();
            return m_allocStart;
        }
    }

    return 0;
}

size_t SatoriRegion::MaxAllocEstimate()
{
    size_t maxRemaining = AllocRemaining();
    for (int bucket = Satori::FREELIST_COUNT - 1; bucket > 0; bucket--)
    {
        if (m_freeLists[bucket])
        {
            maxRemaining = max(maxRemaining, ((size_t)1 << (bucket + Satori::MIN_FREELIST_SIZE_BITS)) - Satori::MIN_FREE_SIZE);
        }
    }

    return maxRemaining;
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

SatoriRegion* SatoriRegion::NextInPage()
{
    return m_containingPage->NextInPage(this);
}

void SatoriRegion::TryCoalesceWithNext()
{
    SatoriRegion* next = NextInPage();
    if (next && CanCoalesce(next))
    {
        auto queue = next->m_containingQueue;
        if (queue && queue->Kind() == QueueKind::Allocator)
        {
            if (queue->TryRemove(next))
            {
                Coalesce(next);
            }
        }
    }
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
        // ensure enough space at start to fit a free obj in the first granule
        // That is useful in case if we split the region after the huge obj is unreachable.
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
    m_used = max(m_used, chunkEnd);

    // space after chunkEnd is a waste, no normal object can start there.
    // if there is dirty space after the chunkEnd,
    // zero-out what would be the next obj MT in debug - to catch if we walk there by accident.
#if _DEBUG
    if (m_used - chunkEnd > sizeof(size_t))
    {
        *((size_t*)chunkEnd) = 0;
    }
#endif
    _ASSERTE(m_allocEnd < Start() + Satori::REGION_SIZE_GRANULARITY);
    return chunkStart;
}

inline int LocationToIndex(size_t location)
{
    return (location >> Satori::INDEX_GRANULARITY_BITS) % Satori::INDEX_LENGTH;
}

SatoriObject* SatoriRegion::FindObject(size_t location)
{
    _ASSERTE(location >= Start() && location <= End());

    // start search from the first object or after unparseable alloc gap
    SatoriObject* obj = (IsAllocating() && (location >= m_allocEnd)) ?
                                 (SatoriObject*)m_allocEnd :
                                 FirstObject();

    // use a better start obj if we have one in the index
    int limit = LocationToIndex(obj->Start());
    for (int current = LocationToIndex(location); current > limit; current--)
    {
        SatoriObject* indexed = m_index[current];
        if (indexed != nullptr)
        {
            if (indexed->Start() > obj->Start())
            {
                obj = indexed;
                if (obj->End() > location)
                {
                    return obj;
                }
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

void SatoriRegion::SetExposed(SatoriObject** location)
{
    _ASSERTE(((SatoriObject*)location)->ContainingRegion() == this);

    // set the mark bit corresponding to the location to indicate that it is globally exposed
    // same as: ((SatoriObject*)location)->SetMarked();

    size_t word = (size_t)location;
    size_t bitmapIndex = (word >> 9) & (SatoriRegion::BITMAP_LENGTH - 1);
    size_t mask = (size_t)1 << ((word >> 3) & 63);

    m_bitmap[bitmapIndex] |= mask;
}

bool SatoriRegion::IsExposed(SatoriObject** location)
{
    _ASSERTE(((SatoriObject*)location)->ContainingRegion() == this);

    // check the mark bit corresponding to the location
    //same as: return ((SatoriObject*)location)->IsMarked();

    size_t word = (size_t)location;
    size_t bitmapIndex = (word >> 9) & (SatoriRegion::BITMAP_LENGTH - 1);
    size_t mask = (size_t)1 << ((word >> 3) & 63);

    return m_bitmap[bitmapIndex] & mask;
}

void SatoriRegion::EscapeRecursively(SatoriObject* o)
{
    _ASSERTE(this->OwnedByCurrentThread());
    _ASSERTE(o->ContainingRegion() == this);

    if (o->IsEscaped())
    {
        return;
    }

    m_escapeCounter++;
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
                    m_escapeCounter++;
                    child->SetEscaped();
                    PushToMarkStack(child);
                }
            }
        );
        o = PopFromMarkStack();
    } while (o);
}

void SatoriRegion::EscapeShallow(SatoriObject* o)
{
    _ASSERTE(o->ContainingRegion() == this);
    _ASSERTE(!o->IsEscaped());

    //NB: we are not checking is we exceed escape limit here.
    //    it is possible, if we have a lot of stack roots, but unlikely.
    //    most likely we will have fewer escapes than before the GC

    m_escapeCounter++;
    o->SetEscaped();
    o->ForEachObjectRef(
        [&](SatoriObject** ref)
        {
            // no refs should be exposed yet, except if the ref is the first field,
            // we use that to escape whole object.
            _ASSERTE(!IsExposed(ref) || ((size_t)o == (size_t)ref - sizeof(size_t)));

            // mark ref location as exposed
            SetExposed(ref);
        }
    );
}

void SatoriRegion::ReportOccupancy(size_t occupancy)
{
    _ASSERTE(occupancy < (Size() - offsetof(SatoriRegion, m_firstObject)));
    m_occupancy = occupancy;

    // TUNING: heuristic needed
    //       when there is 10% "sediment" we want to release this to recycler.
    //       All this can be tuned once full GC works.
    if (m_occupancy >= (Satori::REGION_SIZE_GRANULARITY * 1 / 10))
    {
        // Stop escape tracking. The region is too full
        StopEscapeTracking();
    }
}

void SatoriRegion::EscapeFn(SatoriObject** dst, SatoriObject* src, SatoriRegion* region)
{
    region->EscapeRecursively(src);
    if (region->m_escapeCounter > Satori::MAX_TRACKED_ESCAPES)
    {
        // stop escape tracking.
        region->StopEscapeTracking();
    }
}

void SatoriRegion::PromoteToGen1()
{
    StopEscapeTracking();
    SetGeneration(1);
}

void SatoriRegion::ThreadLocalCollect()
{
    ThreadLocalMark();
    ThreadLocalPlan();
    ThreadLocalUpdatePointers();
    ThreadLocalCompact();
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
            markBitOffset = obj->Next()->GetMarkBitAndOffset(&bitmapIndex);
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
    //      not marked (incl. free objects, which are never marked)
    //
    //  - Unmovable:
    //      escaped or pinned
    //
    //  - Movable
    //      marked, but not escaped or pinned
    //
    // we will shift left all movable objects - as long as they fit between unmovables
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
            markBitOffset = obj->Next()->GetMarkBitAndOffset(&bitmapIndex);
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

void SatoriRegion::ThreadLocalCompact()
{
    memset(m_freeLists, 0, sizeof(m_freeLists));
    memset(&m_index, 0, sizeof(m_index));

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
                SatoriObject* freeObj = SatoriObject::FormatAsFree(d1->Start(), freeSpace);
                AddFreeSpace(freeObj);
                foundFree += freeSpace;
            }
            else
            {
                // clear Mark/Pinned, keep escaped, reloc should be 0, this object will stay around
                _ASSERTE(d1->GetReloc() == 0);
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
            // moving objects with syncblocks, thus the src/dst adjustment
            memmove(
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
    ReportOccupancy(Satori::REGION_SIZE_GRANULARITY - offsetof(SatoriRegion, m_firstObject) - foundFree);
}

//TODO: VS this can be called concurrently, need a spinlock
bool SatoriRegion::RegisterForFinalization(SatoriObject* finalizable)
{
    _ASSERTE(finalizable->ContainingRegion() == this);
    _ASSERTE(this->m_everHadFinalizables || this->Generation() == 0);

    if (!m_finalizables || !m_finalizables->TryPush(finalizable))
    {
        m_everHadFinalizables = true;
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

SatoriObject* SatoriRegion::SkipUnmarked(SatoriObject* from)
{
    _ASSERTE(from->Start() < End());
    size_t bitmapIndex;
    int markBitOffset = from->GetMarkBitAndOffset(&bitmapIndex);

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
    int markBitOffset = from->GetMarkBitAndOffset(&bitmapIndex);

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

bool SatoriRegion::Sweep(bool turnMarkedIntoEscaped)
{
    size_t objLimit = Start() + Satori::REGION_SIZE_GRANULARITY;
    size_t largeObjTailSize = 0;

    // if region is huge and last object is unreachable,
    // we can split off extra regions and return to allocator.
    if (End() > objLimit)
    {
        SatoriObject* last = FindObject(objLimit - 1);
        if (!last->IsMarked())
        {
            SatoriObject* free = SatoriObject::FormatAsFree(last->Start(), objLimit - last->Start());
            SatoriRegion* tail = this->Split(Size() - Satori::REGION_SIZE_GRANULARITY);
            tail->WipeCards();
            Allocator()->ReturnRegion(tail);
        }
        else
        {
            largeObjTailSize = last->End() - objLimit;
        }
    }

    memset(&m_freeLists, 0, sizeof(m_freeLists));
    memset(&m_index, 0, sizeof(m_index));
    m_escapeCounter = 0;

    size_t foundFree = 0;
    bool sawMarked = false;
    m_hasPinnedObjects = false;

    SatoriObject* cur = FirstObject();
    do
    {
        if (cur->IsMarked())
        {
            _ASSERTE(!cur->IsFree());
            sawMarked = true;
            if (turnMarkedIntoEscaped)
            {
                cur->ClearPinnedAndMarked();
                this->EscapeShallow(cur);
            }
            else if (!m_hasPinnedObjects &&
                (cur->IsPinned() || cur->IsPermanentlyPinned()))
            {
                m_hasPinnedObjects = true;
            }

            cur = cur->Next();
            continue;
        }

        size_t lastMarkedEnd = cur->Start();
        cur = SkipUnmarked(cur);
        size_t skipped = cur->Start() - lastMarkedEnd;
        if (skipped)
        {
            foundFree += skipped;
            SatoriObject* free = SatoriObject::FormatAsFree(lastMarkedEnd, skipped);
            AddFreeSpace(free);
        }
    }
    while (cur->Start() < objLimit);

    ReportOccupancy(Satori::REGION_SIZE_GRANULARITY - offsetof(SatoriRegion, m_firstObject) - foundFree + largeObjTailSize);
    return !sawMarked;
}

void SatoriRegion::UpdateReferences()
{
    size_t objLimit = Start() + Satori::REGION_SIZE_GRANULARITY;

    // TODO: VS assert these
    // memset(&m_freeLists, 0, sizeof(m_freeLists));
    // memset(&m_index, 0, sizeof(m_index));

    SatoriObject* obj = FirstObject();
    do
    {
        //TODO: VS walking objects without bitmap.
        //      - we need to get MTs for all live objects anyways
        //      - free objects are near optimal since we have just swept
        //      - besides, relocated objects are not marked (we could, but there is little point if we do not use that here)
        obj->ForEachObjectRef(
            [](SatoriObject** ppObject)
            {
                SatoriObject* o = *ppObject;
                if (o)
                {
                    ptrdiff_t ptr = ((ptrdiff_t*)o)[-1];
                    if (ptr < 0)
                    {
                        *ppObject = (SatoriObject*)-ptr;
                    }
                }
            }
        );
        obj = obj->Next();
    } while (obj->Start() < objLimit);

    // update finalizable trackers, if there are any
    if (m_finalizables)
    {
        ForEachFinalizable(
            [&](SatoriObject* finalizable)
            {
                if (finalizable)
                {
                    ptrdiff_t ptr = ((ptrdiff_t*)finalizable)[-1];
                    if (ptr < 0)
                    {
                        finalizable = (SatoriObject*)-ptr;
                    }

                    _ASSERTE(finalizable->ContainingRegion() == this);
                }

                return finalizable;
            }
        );
    }
}

void SatoriRegion::TakeFinalizerInfoFrom(SatoriRegion* other)
{
    m_everHadFinalizables |= other->m_everHadFinalizables;
    SatoriMarkChunk* otherFinalizables = other->m_finalizables;
    if (otherFinalizables)
    {
        SatoriMarkChunk* tail = otherFinalizables;
        while (tail->Next() != nullptr)
        {
            tail = tail->Next();
        }

        tail->SetNext(m_finalizables);
        m_finalizables = otherFinalizables;
        other->m_finalizables = nullptr;
    }
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

void SatoriRegion::ClearMarks()
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

    size_t objLimit = Start() + Satori::REGION_SIZE_GRANULARITY;
    while (obj->Start() < objLimit)
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

    _ASSERTE(obj->Start() == End() ||
        (Size() > Satori::REGION_SIZE_GRANULARITY) && (End() - obj->Start()) < Satori::REGION_SIZE_GRANULARITY);
#endif
}
