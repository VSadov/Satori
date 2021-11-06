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

#if !defined(_DEBUG)
//#pragma optimize("gty", on)
#endif

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

const int SatoriRegion::MAX_LARGE_OBJ_SIZE = Satori::REGION_SIZE_GRANULARITY - Satori::MIN_FREE_SIZE - offsetof(SatoriRegion, m_firstObject);

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
    result->m_escapeFunc = nullptr;
    result->m_generation = -1;
    result->m_finalizableTrackersLock = 0;

    result->m_containingPage->RegionInitialized(result);
    return result;
}

SatoriAllocator* SatoriRegion::Allocator()
{
    return m_containingPage->Heap()->Allocator();
}

SatoriRecycler* SatoriRegion::Recycler()
{
    return m_containingPage->Heap()->Recycler();
}

void SatoriRegion::WipeCards()
{
    m_containingPage->WipeCardsForRange(Start(), End());
}

void SatoriRegion::MakeBlank()
{
    _ASSERTE(!m_hasPendingFinalizables);
    _ASSERTE(!m_finalizableTrackers);
    _ASSERTE(!m_acceptedPromotedObjects);
    _ASSERTE(!m_gen2Objects);
    _ASSERTE(NothingMarked());

    WipeCards();

    m_ownerThreadTag = 0;
    m_escapeFunc = EscapeFn;
    m_generation = -1;
    m_allocStart = (size_t)&m_firstObject;
    m_allocEnd = End();
    m_markStack = 0;
    m_escapedSize = 0;
    m_occupancy = 0;
    m_objCount = 0;
    m_allocBytesAtCollect = 0;

    m_everHadFinalizables = false;
    m_hasPinnedObjects = false;
    m_hasMarksSet = false;
    m_reusableFor = ReuseLevel::None;

    //clear index and free list
    ClearFreeLists();
    ClearIndex();

#ifdef JUNK_FILL_FREE_SPACE
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

    if (m_finalizableTrackers)
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
        _ASSERTE(m_index[i] == 0);
        if (m_index[i] != 0)
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
    // allocSize is smaller than size to make sure the span can always be made parseable
    // after allocating objects in it.
    ptrdiff_t allocSize = freeObj->Size() - Satori::MIN_FREE_SIZE;
    if (allocSize < Satori::MIN_FREELIST_SIZE)
    {
        return;
    }

    DWORD bucket;
    BitScanReverse64(&bucket, allocSize);
    bucket -= (Satori::MIN_FREELIST_SIZE_BITS);
    _ASSERTE(bucket >= 0);
    _ASSERTE(bucket < Satori::FREELIST_COUNT);

    *(SatoriObject**)(freeObj->Start() + FREE_LIST_NEXT_OFFSET) = m_freeLists[bucket];
    m_freeLists[bucket] = freeObj;
}

size_t SatoriRegion::StartAllocating(size_t minAllocSize)
{
    _ASSERTE(!IsAllocating());

    DWORD bucket;
    BitScanReverse64(&bucket, minAllocSize);

    // when minAllocSize is not a power of two we could search through the current bucket,
    // which may have a large enough obj,
    // but we will just use the next bucket, which guarantees it fits
    if (minAllocSize & (minAllocSize - 1))
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
            _ASSERTE(AllocRemaining() >= minAllocSize);
            return m_allocStart;
        }
    }

    return 0;
}

bool SatoriRegion::HasFreeSpaceInTopBucket()
{
    return m_freeLists[Satori::FREELIST_COUNT - 1];
}

bool SatoriRegion::HasFreeSpaceInTop3Buckets()
{
    for (int bucket = Satori::FREELIST_COUNT - 1; bucket >= 0; bucket--)
    {
        if (m_freeLists[bucket])
        {
            return true;
        }
    }

    return false;
}

size_t SatoriRegion::MaxAllocEstimate()
{
    size_t maxRemaining = AllocRemaining();
    for (int bucket = Satori::FREELIST_COUNT - 1; bucket >= 0; bucket--)
    {
        if (m_freeLists[bucket])
        {
            maxRemaining = max(maxRemaining, ((size_t)1 << (bucket + Satori::MIN_FREELIST_SIZE_BITS)));
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

SatoriPage* SatoriRegion::ContainingPage()
{
    return m_containingPage;
}

SatoriRegion* SatoriRegion::NextInPage()
{
    return m_containingPage->NextInPage(this);
}

bool SatoriRegion::CanCoalesceWithNext()
{
    SatoriRegion* next = NextInPage();
    if (next)
    {
        auto queue = VolatileLoadWithoutBarrier(&next->m_containingQueue);
        if (queue && queue->Kind() == QueueKind::Allocator)
        {
            return true;
        }
    }

    return false;
}

bool SatoriRegion::TryCoalesceWithNext()
{
    SatoriRegion* next = NextInPage();
    if (next)
    {
        auto queue = VolatileLoadWithoutBarrier(&next->m_containingQueue);
        if (queue && queue->Kind() == QueueKind::Allocator)
        {
            if (queue->TryRemove(next))
            {
                Coalesce(next);
                return true;
            }
        }
    }

    return false;
}

void SatoriRegion::Coalesce(SatoriRegion* next)
{
    _ASSERTE(next->m_containingQueue == nullptr);
    _ASSERTE(next->m_containingPage == m_containingPage);
    _ASSERTE(next->m_prev == next->m_next);
    _ASSERTE(m_end == next->Start());
    _ASSERTE(ValidateBlank());
    _ASSERTE(next->ValidateBlank());

    m_end = next->m_end;
    m_allocEnd = next->m_allocEnd;

    if (m_committed == next->Start())
    {
        m_committed = next->m_committed;
        m_used = next->m_used;
    }
    else
    {
        _ASSERTE(next->m_committed > next->Start());
        size_t toDecommit = next->m_committed - next->Start();
        _ASSERTE(toDecommit % Satori::CommitGranularity() == 0);
        GCToOSInterface::VirtualDecommit(next, toDecommit);
    }

    m_containingPage->RegionInitialized(this);
}

bool SatoriRegion::CanDecommit()
{
    size_t decommitStart = ALIGN_UP((size_t)&m_syncBlock, Satori::CommitGranularity());
    _ASSERTE(m_committed >= decommitStart);

    size_t decommitSize = m_committed - decommitStart;
    return (decommitSize > Satori::REGION_SIZE_GRANULARITY / 8);
}

bool SatoriRegion::TryDecommit()
{
    size_t decommitStart = ALIGN_UP((size_t)&m_syncBlock, Satori::CommitGranularity());
    _ASSERTE(m_committed >= decommitStart);

    size_t decommitSize = m_committed - decommitStart;
    if (decommitSize > Satori::REGION_SIZE_GRANULARITY / 8)
    {
        GCToOSInterface::VirtualDecommit((void*)decommitStart, decommitSize);
        m_committed = decommitStart;
        m_used = min(m_used, decommitStart);
        return true;
    }

    return false;
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

    size_t chunkStart = m_allocStart;
    size_t chunkEnd = chunkStart + size;

    // in rare cases the object does not cross into the last granule. (when it needs extra because of free obj padding).
    // in such case pad it in front.
    if (chunkEnd < End() - Satori::REGION_SIZE_GRANULARITY)
    {
        chunkStart += Satori::MIN_FREE_SIZE;
        chunkEnd += Satori::MIN_FREE_SIZE;
    }

    // huge allocation should cross the end of the first granule.
    _ASSERTE(chunkEnd > Start() + Satori::REGION_SIZE_GRANULARITY);
    _ASSERTE(chunkStart <= Start() + Satori::REGION_SIZE_GRANULARITY);

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

    m_used = max(m_used, chunkEnd);

    // if did not allocate from start, there could be MIN_FREE_SIZE fillable space before the obj.
    m_allocEnd = chunkStart;

    // space after chunkEnd is a waste, no normal object can start there.
    // if there is dirty space after the chunkEnd,
    // zero-out what would be the next obj MT in debug - to catch if we walk there by accident.
#ifdef _DEBUG
    if (m_used - chunkEnd > sizeof(size_t))
    {
        *((size_t*)chunkEnd) = 0;
    }
#endif
    _ASSERTE(m_allocEnd < Start() + Satori::REGION_SIZE_GRANULARITY);
    return chunkStart;
}

inline size_t LocationToIndex(size_t location)
{
    return (location >> Satori::INDEX_GRANULARITY_BITS) & (Satori::INDEX_LENGTH - 1);
}

// Finds an object that contains given location.
//
// Assumptions that caller must arrange or handle:
//  - we may return a Free object here.
//  - locations before the first object and after the last object match to the First/Last objects
//  - location must not be inside unparseable unconsumed budget when actively allocating.
//
// Typical usees:
//  - precise root marking.
//         not in allocating mode (not attached to allocation context)
//         always provides real refs into real objects.
//  - conservative root marking
//         not in allocating mode
//         can give refs pointing to Free
//  - escape checks for array copying.
//         always provides refs into real objects
//         may be in allocation mode, but uses the region owned by current thread, thus no allocations could happen concurrently.
//  - iterating over card table - 
//         not in allocating mode
//         can give refs pointing to Free. (because of card granularity)
SatoriObject* SatoriRegion::FindObject(size_t location)
{
    _ASSERTE(m_generation >= 0 && location >= Start() && location < End());

    location = min(location, Start() + Satori::REGION_SIZE_GRANULARITY);

    // start search from the first object or after unparseable alloc gap
    SatoriObject* o = (IsAllocating() && (location >= m_allocEnd)) ?
                                 (SatoriObject*)m_allocEnd :
                                 FirstObject();

    // use a better start obj if we have one in the index
    size_t limit = LocationToIndex(o->Start());
    for (size_t current = LocationToIndex(location); current > limit; current--)
    {
        int offset = m_index[current];
        if (offset)
        {
            SatoriObject* indexed = (SatoriObject*)(Start() + offset);
            if (indexed->Start() > o->Start())
            {
                o = indexed;
                if (o->End() > location)
                {
                    return o;
                }
            }

            break;
        }
    }

    // walk to the location and update index on the way
    SatoriObject* next = o->Next();
    while (next->Start() <= location)
    {
        // if object straddles index granules, record the object in corresponding indices
        if ((o->Start() ^ next->Start()) >> Satori::INDEX_GRANULARITY_BITS)
        {
            size_t i = LocationToIndex(o->Start()) + 1;
            size_t last = LocationToIndex(next->Start());
            do
            {
                m_index[i++] = (int)(o->Start() - Start());
            }
            while (i <= last);
        }

        o = next;
        next = next->Next();
    }

    return o;
}

template <bool isConservative>
void SatoriRegion::MarkFn(PTR_PTR_Object ppObject, ScanContext* sc, uint32_t flags)
{
    SatoriRegion* region = (SatoriRegion*)sc->_unused1;
    size_t location = (size_t)*ppObject;

    // ignore objects outside of the current region.
    // this also rejects nulls and byrefs pointing to stack.
    if (location < (size_t)region->FirstObject() || location >= region->End())
    {
        return;
    }

    SatoriObject* o = (SatoriObject*)location;
    if (flags & GC_CALL_INTERIOR)
    {
        o = region->FindObject(location);
        if (isConservative && o->IsFree())
        {
            return;
        }
    }

    if (!region->IsMarked(o))
    {
        region->SetMarked(o);
        region->PushToMarkStackIfHasPointers(o);
    }

    if (flags & GC_CALL_PINNED)
    {
        if (!region->IsEscaped(o))
        {
            region->SetPinned(o);
        }
    }
}

inline void SatoriRegion::PushToMarkStackIfHasPointers(SatoriObject* obj)
{
    _ASSERTE(obj->ContainingRegion() == this);
    _ASSERTE(!obj->GetNextInLocalMarkStack());

    if (obj->RawGetMethodTable()->ContainsPointersOrCollectible())
    {
        obj->SetNextInLocalMarkStack(m_markStack);
        _ASSERTE(m_markStack == obj->GetNextInLocalMarkStack());
        m_markStack = (int32_t)(obj->Start() - this->Start());
    }
}

inline SatoriObject* SatoriRegion::PopFromMarkStack()
{
    if (m_markStack != 0)
    {
        SatoriObject* o = (SatoriObject*)(this->Start() + m_markStack);
        m_markStack = o->GetNextInLocalMarkStack();
        o->ClearNextInLocalMarkStack();
        return o;
    }

    return nullptr;
}

bool SatoriRegion::AnyExposed(size_t first, size_t length)
{
    _ASSERTE(length % 8 == 0);

    size_t last = first + length - sizeof(size_t);
    _ASSERTE(((SatoriObject*)first)->ContainingRegion() == this);
    _ASSERTE(((SatoriObject*)last)->ContainingRegion() == this);

    size_t bitmapIndexF;
    size_t maskF = (size_t)-1 << ((SatoriObject*)first)->GetMarkBitAndWord(&bitmapIndexF);

    size_t bitmapIndexL;
    size_t maskL = (size_t)-1 >> (63 - ((SatoriObject*)last)->GetMarkBitAndWord(&bitmapIndexL));

    if (bitmapIndexF == bitmapIndexL)
    {
        return m_bitmap[bitmapIndexF] & (maskF & maskL);
    }

    if (m_bitmap[bitmapIndexF] & maskF)
    {
        return true;
    }

    for (size_t i = bitmapIndexF + 1; i < bitmapIndexL; i++)
    {
        if (m_bitmap[i])
        {
            return true;
        }
    }

    return m_bitmap[bitmapIndexL] & maskL;
}

void SatoriRegion::EscapeRecursively(SatoriObject* o)
{
    _ASSERTE(this->IsEscapeTrackedByCurrentThread());
    _ASSERTE(o->ContainingRegion() == this);

    if (IsEscaped(o))
    {
        return;
    }

    SetEscaped(o);
    m_escapedSize += o->Size();

    // now recursively mark all the objects reachable from escaped object.
    do
    {
        _ASSERTE(IsEscaped(o));
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
                if (child->ContainingRegion() == this && !IsEscaped(child))
                {
                    SetEscaped(child);
                    m_escapedSize += child->Size();
                    PushToMarkStackIfHasPointers(child);
                }
            }
        );

        o = PopFromMarkStack();
    } while (o);
}

void SatoriRegion::EscsapeAll()
{
    size_t objLimit = Start() + Satori::REGION_SIZE_GRANULARITY;
    for (SatoriObject* o = FirstObject(); o->Start() < objLimit; o = o->Next())
    {
        if (!o->IsFree())
        {
            EscapeShallow(o);
        }
    }
}

void SatoriRegion::EscapeShallow(SatoriObject* o)
{
    _ASSERTE(o->ContainingRegion() == this);
    _ASSERTE(!IsEscaped(o));
    _ASSERTE(!IsPinned(o));

    //NB: we are not checking if we exceed the escape limit here.
    //    it is possible, if we have a lot of stack roots, but highly unlikely and otherwise ok.
    //    typically objects have died and we have fewer escapes than before the GC,
    //    so we do not bother to check
    SetEscaped(o);
    m_escapedSize += o->Size();

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

void SatoriRegion::SetOccupancy(size_t occupancy, size_t objCount)
{
    _ASSERTE((occupancy == 0) == (objCount == 0));
    _ASSERTE(occupancy <= (Size() - offsetof(SatoriRegion, m_firstObject)));
    m_occupancy = occupancy;
    m_objCount = objCount;
}

// NB: dst is unused, it is just to avoid arg shuffle in x64 barriers
void SatoriRegion::EscapeFn(SatoriObject** dst, SatoriObject* src, SatoriRegion* region)
{
    region->EscapeRecursively(src);
    if (region->m_escapedSize > Satori::MAX_ESCAPE_SIZE)
    {
        region->StopEscapeTracking();
    }
}

bool SatoriRegion::ThreadLocalCollect(size_t allocBytes)
{
    // TUNING: 1/4 is not too greedy? maybe 1/8 ?
    if (allocBytes - m_allocBytesAtCollect < Satori::REGION_SIZE_GRANULARITY / 4)
    {
        // this is too soon. last collection did not buy us as much as we wanted.
        // either due to fragmentation or unfortunate allocation pattern in this thread.
        // we will not collect this region, lest it keeps coming back.
        return false;
    }

    m_allocBytesAtCollect = allocBytes;

    ThreadLocalMark();
    ThreadLocalPlan();
    ThreadLocalUpdatePointers();
    ThreadLocalCompact();

    return true;
}

void SatoriRegion::ThreadLocalMark()
{
    Verify();

    bool checkFinalizables = m_finalizableTrackers != nullptr;

    // since this region is thread-local, we have only two kinds of live roots:
    //- those that are reachable from the current stack and
    //- those that are reachable from outside of the region (escaped)

    // mark escaped objects:
    // for every set escape bit, set the corresponding mark bit
    // nothing should be marked in the region, so we can find escapes via bit scan
    size_t bitmapIndex = BITMAP_START;
    int markBitOffset = 0;

#ifdef _DEBUG
    size_t escaped = 0;
#endif
    while (bitmapIndex < BITMAP_LENGTH)
    {
        DWORD step;
        if (BitScanForward64(&step, m_bitmap[bitmapIndex] >> markBitOffset))
        {
            // got an escape bit. its mark bit is at -1
            markBitOffset += step - 1;

            // set the mark bit
            m_bitmap[bitmapIndex + (markBitOffset >> 6)] |= ((size_t)1 << (markBitOffset & 63));

            SatoriObject* o = ObjectForMarkBit(bitmapIndex, markBitOffset);
            o->Validate();

#ifdef _DEBUG
            escaped += o->Size();
#endif

            // skip the object
            markBitOffset = o->Next()->GetMarkBitAndWord(&bitmapIndex);
        }
        else
        {
            // skip empty mark words
            markBitOffset = 0;
            while (++bitmapIndex < BITMAP_LENGTH && m_bitmap[bitmapIndex] == 0) {}
        }
    }

#ifdef _DEBUG
    _ASSERTE(escaped == this->m_escapedSize);
#endif

    // mark roots for the current stack
    ScanContext sc;
    sc.promotion = TRUE;
    sc._unused1 = this;

    if (SatoriUtil::IsConservativeMode())
        GCToEEInterface::GcScanCurrentStackRoots((promote_func*)MarkFn<true>, &sc);
    else
        GCToEEInterface::GcScanCurrentStackRoots((promote_func*)MarkFn<false>, &sc);

    // now recursively mark all the objects reachable from the stack roots.
    SatoriObject* o = PopFromMarkStack();

 propagateMarks:
    while (o)
    {
        _ASSERTE(IsMarked(o));
        _ASSERTE(!IsEscaped(o));
        _ASSERTE(!o->IsFree());
        o->ForEachObjectRef(
            [this](SatoriObject** ref)
            {
                SatoriObject* child = *ref;
                if (child->ContainingRegion() == this && !IsMarked(child))
                {
                    SetMarked(child);
                    PushToMarkStackIfHasPointers(child);
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
        ForEachFinalizableThreadLocal(
            [this](SatoriObject* finalizable)
            {
                _ASSERTE(((size_t)finalizable & Satori::FINALIZATION_PENDING) == 0);
                if (!IsMarked(finalizable))
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
                        SetMarked(finalizable);
                        PushToMarkStackIfHasPointers(finalizable);
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

void SatoriRegion::ThreadLocalPlan()
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

    // planning will coalesce free objects.
    // that alone does not invalidate the index, unless we fill free objects with junk.
#ifdef JUNK_FILL_FREE_SPACE
    ClearIndex();
#endif

    // what we want to move
    SatoriObject* moveable;
    size_t moveableSize = 0;

    // space to move to
    size_t dstStart;
    size_t dstEnd = 0;

    // stats
    size_t occupancy = 0;
    size_t objCount = 0;

    // moveable: starts at first movable and reachable, as long as there is any free space to slide in
    size_t lastMarkedEnd = FirstObject()->Start();
    moveable = SkipUnmarked(FirstObject());
    bool sawFree = false;
    while (true)
    {
        size_t skipped = moveable->Start() - lastMarkedEnd;
        if (skipped)
        {
            if (!sawFree)
            {
                // new gap from here
                dstEnd = lastMarkedEnd;
            }

            sawFree = true;
            SatoriObject::FormatAsFree(lastMarkedEnd, skipped);
        }

        if (moveable->Start() >= End())
        {
            // nothing left to move
            _ASSERTE(moveable->Start() == End());
            goto Exit;
        }

        moveableSize = moveable->Size();
        objCount++;
        occupancy += moveableSize;

        if (sawFree && !IsEscapedOrPinned(moveable))
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
            if (!IsEscapedOrPinned((SatoriObject*)dstStart))
            {
                break;
            }
        }

        if (dstStart >= End())
        {
            // no more space could be found
            goto Exit;
        }

        // dst end: skip until next unmovable
        for (dstEnd = dstStart; dstEnd < End(); dstEnd = SkipUnmarked(((SatoriObject*)dstEnd)->Next())->Start())
        {
            if (IsEscapedOrPinned((SatoriObject*)dstEnd))
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
                moveable->SetLocalReloc(reloc);
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
                    SatoriObject::FormatAsFree(lastMarkedEnd, skipped);
                }

                if (moveable->Start() >= End())
                {
                    // nothing left to move
                    _ASSERTE(moveable->Start() == End());
                    goto Exit;
                }

                moveableSize = moveable->Size();
                objCount++;
                occupancy += moveableSize;

                if (!IsEscapedOrPinned(moveable))
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

Exit:
    SetOccupancy(occupancy, objCount);
}

template <bool isConservative>
void SatoriRegion::UpdateFn(PTR_PTR_Object ppObject, ScanContext* sc, uint32_t flags)
{
    SatoriRegion* region = (SatoriRegion*)sc->_unused1;
    size_t location = (size_t)*ppObject;

    // ignore objects otside of the current region.
    // this also rejects nulls and byrefs pointing to stack.
    if (location < (size_t)region->FirstObject() || location >= region->End())
    {
        return;
    }

    SatoriObject* o = (SatoriObject*)location;
    if (flags & GC_CALL_INTERIOR)
    {
        o = region->FindObject(location);
        if (isConservative && o->IsFree())
        {
            return;
        }
    }

    size_t reloc = o->GetLocalReloc();
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

    if (SatoriUtil::IsConservativeMode())
        GCToEEInterface::GcScanCurrentStackRoots((promote_func*)UpdateFn<true>, &sc);
    else
        GCToEEInterface::GcScanCurrentStackRoots((promote_func*)UpdateFn<false>, &sc);

    // go through all live objects and update pointers if targets are planned for relocation.
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
            SatoriObject* o = ObjectForMarkBit(bitmapIndex, markBitOffset);
            if (escaped)
            {
                _ASSERTE(ObjectForMarkBit(bitmapIndex, markBitOffset)->GetLocalReloc() == 0);
            }
            else
            {
                o->ForEachObjectRef(
                    [this](SatoriObject** ppObject)
                    {
                        SatoriObject* child = *ppObject;
                        // ignore objects otside of the current region, we are not relocating those.
                        // (this also rejects nulls)
                        if (child->ContainingRegion() == this)
                        {
                            size_t reloc = child->GetLocalReloc();
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
            markBitOffset = o->Next()->GetMarkBitAndWord(&bitmapIndex);
        }
        else
        {
            // skip empty mark words
            markBitOffset = 0;
            while (++bitmapIndex < BITMAP_LENGTH && m_bitmap[bitmapIndex] == 0) {}
        }
    }

    // update finalizables if we have them
    if (m_finalizableTrackers)
    {
        ForEachFinalizableThreadLocal(
            [&](SatoriObject* finalizable)
            {
                // save the pending bit, will reapply back later
                size_t finalizePending = (size_t)finalizable & Satori::FINALIZATION_PENDING;
                (size_t&)finalizable &= ~Satori::FINALIZATION_PENDING;

                size_t reloc = finalizable->GetLocalReloc();
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
    SatoriObject* s1;
    SatoriObject* s2 = FirstObject();
    SatoriObject* d1 = FirstObject();
    SatoriObject* d2 = FirstObject();

    size_t foundFree = 0;

    // we will be building new free lists
    ClearFreeLists();
    // compacting may invalidate indices, since objects move around.
    ClearIndex();

    // when d1 reaches the end everything is copied or swept
    while (d1->Start() < End())
    {
        // find next relocatable object
        int32_t reloc = 0;
        // "Next" is ok here because we coalesce unmarked objs when planning.
        for (s1 = s2; s1->Start() < End(); s1 = s1->Next())
        {
            reloc = s1->GetLocalReloc();
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
                _ASSERTE(d1->GetLocalReloc() == 0);
                ClearPinnedAndMarked(d1);
            }

            d1 = d1->Next();
            d2 = d1;
        }

        // find the end of the run of relocatable objects with the same reloc distance (we can copy all at once)
        for (s2 = s1; s2->Start() < End(); s2 = s2->Next())
        {
            if (reloc != s2->GetLocalReloc())
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
        d1 = (SatoriObject*)relocTargetEnd;
        while (d2->Start() < relocTargetEnd)
        {
            d2 = d2->Next();
        }

        if (reloc)
        {
            // moving objects with syncblocks, thus the "- sizeof(size_t)" adjustment
            memmove(
                (void*)(relocTarget - sizeof(size_t)),
                (void*)(s1->Start() - sizeof(size_t)),
                s2->Start() - s1->Start());
        }
    }

    // schedule pending finalizables if we have them
    // must be after compaction, since pend may escape objects.
    if (HasPendingFinalizables())
    {
        ThreadLocalPendFinalizables();
    }

    Verify();

    _ASSERTE((Satori::REGION_SIZE_GRANULARITY - offsetof(SatoriRegion, m_firstObject) - foundFree) == m_occupancy);
}

NOINLINE void SatoriRegion::ClearPinned(SatoriObject* o)
{
    if (!IsEscaped(o))
    {
        ClearMarked(o + MarkOffset::Pinned);
    }
}

enum class FinalizationPendState
{
    PendRegular,
    HasCritical,
    PendCritical,
};

void SatoriRegion::ThreadLocalPendFinalizables()
{
    HasPendingFinalizables() = false;

    FinalizationPendState pendState = FinalizationPendState::PendRegular;
    bool missedRegularPend = false;

tryAgain:
    ForEachFinalizableThreadLocal(
        [&](SatoriObject* finalizable)
        {
            if ((size_t)finalizable & Satori::FINALIZATION_PENDING)
            {
                SatoriObject* o = (SatoriObject*)((size_t)finalizable & ~Satori::FINALIZATION_PENDING);
                if (pendState == FinalizationPendState::PendRegular && o->RawGetMethodTable()->HasCriticalFinalizer())
                {
                    // within the same finalization set CF must finalize after ordinary F objects
                    pendState = FinalizationPendState::HasCritical;
                    return finalizable;
                }

                // by enqueing we are making the object available to the finalization thread
                EscapeRecursively(o);

                // in a rare case when an F object could not pend, we cannot pend any CFs from the same finalization set
                // lest they may run before corresponding F objects.
                // then we will just allow CFs to escape so they stay alive until global GC sorts this out.
                if (missedRegularPend && o->RawGetMethodTable()->HasCriticalFinalizer())
                { 
                    return (SatoriObject*)nullptr;
                }

                if (m_containingPage->Heap()->FinalizationQueue()->TryScheduleForFinalization(o))
                {
                    // this tracker has served its purpose.
                    finalizable = nullptr;
                }
                else
                {
                    if (pendState != FinalizationPendState::PendCritical)
                    {
                        missedRegularPend = true;
                    }

                    finalizable = o;
                }
            }

            return finalizable;
        }
    );

    if (pendState == FinalizationPendState::HasCritical)
    {
        pendState = FinalizationPendState::PendCritical;
        goto tryAgain;
    }

    GCToEEInterface::EnableFinalization(true);
}

bool SatoriRegion::RegisterForFinalization(SatoriObject* finalizable)
{
    _ASSERTE(finalizable->ContainingRegion() == this);
    _ASSERTE(this->m_everHadFinalizables || this->IsAttachedToContext());

    LockFinalizableTrackers();

    if (!m_finalizableTrackers || !m_finalizableTrackers->TryPush(finalizable))
    {
        m_everHadFinalizables = true;
        SatoriMarkChunk* markChunk = Allocator()->TryGetMarkChunk();
        if (!markChunk)
        {
            // OOM 
            UnlockFinalizableTrackers();
            return false;
        }

        markChunk->SetNext(m_finalizableTrackers);
        m_finalizableTrackers = markChunk;
        markChunk->Push(finalizable);
    }

    UnlockFinalizableTrackers();
    return true;
}

// Finalizable trackers are generally accessed exclusively when EE stopped
// The only case where we can have contention is when a user thread re-registers
// concurrently with another thread doing the same or
// concurrently with thread local collection.
// It is extremely unlikely to have such contention, so a simplest spinlock is ok
void SatoriRegion::LockFinalizableTrackers()
{
    while (Interlocked::CompareExchange(&m_finalizableTrackersLock, 1, 0) != 0)
    {
        YieldProcessor();
    };
}

void SatoriRegion::UnlockFinalizableTrackers()
{
    VolatileStore(&m_finalizableTrackersLock, 0);
}

void SatoriRegion::CompactFinalizableTrackers()
{
    SatoriMarkChunk* oldTrackers = m_finalizableTrackers;
    m_finalizableTrackers = nullptr;

    while (oldTrackers)
    {
        SatoriMarkChunk* current = oldTrackers;
        oldTrackers = oldTrackers->Next();

        if (m_finalizableTrackers)
        {
            while (m_finalizableTrackers->HasSpace() && current->Count())
            {
                SatoriObject* o = current->Pop();
                if (o)
                {
                    m_finalizableTrackers->Push(o);
                }
            }
        }

        if (current->Count() > 0 && current->Compact() > 0)
        {
            current->SetNext(m_finalizableTrackers);
            m_finalizableTrackers = current;
            continue;
        }

        current->SetNext(nullptr);
        Allocator()->ReturnMarkChunk(current);
    }
}

SatoriObject* SatoriRegion::SkipUnmarked(SatoriObject* from)
{
    _ASSERTE(from->Start() < End());
    size_t bitmapIndex;
    int markBitOffset = from->GetMarkBitAndWord(&bitmapIndex);

    DWORD offset;
    if (BitScanForward64(&offset, m_bitmap[bitmapIndex] >> markBitOffset))
    {
        // got mark bit
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
                // got mark bit
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
    int markBitOffset = from->GetMarkBitAndWord(&bitmapIndex);

    DWORD offset;
    if (BitScanForward64(&offset, m_bitmap[bitmapIndex] >> markBitOffset))
    {
        // got mark bit
        markBitOffset += offset;
    }
    else
    {
        size_t limit = (upTo - Start()) / Satori::BYTES_PER_CARD_BYTE;
        _ASSERTE(limit <= SatoriRegion::BITMAP_LENGTH);
        markBitOffset = 0;
        while (bitmapIndex < limit)
        {
            bitmapIndex++;
            if (BitScanForward64(&offset, m_bitmap[bitmapIndex]))
            {
                // got mark bit
                markBitOffset = offset;
                break;
            }
        }
    }

    SatoriObject* result = ObjectForMarkBit(bitmapIndex, markBitOffset);
    if (result->Start() > upTo)
    {
        result = (SatoriObject*)upTo;
    }

    _ASSERTE(result->Start() <= upTo);
    return result;
}


void SatoriRegion::UpdateFinalizableTrackers()
{
    // if any finalizable trackers point outside,
    // their objects have been relocated to this region
    if (m_finalizableTrackers)
    {
        ForEachFinalizable(
            [&](SatoriObject* finalizable)
            {
                if (finalizable->ContainingRegion() != this)
                {
                    ptrdiff_t ptr = ((ptrdiff_t*)finalizable)[-1];
                    _ASSERTE(finalizable->RawGetMethodTable() == ((SatoriObject*)-ptr)->RawGetMethodTable());
                    finalizable = (SatoriObject*)-ptr;
                    _ASSERTE(finalizable->ContainingRegion() == this);
                }

                return finalizable;
            }
        );
    }
}

void SatoriRegion::UpdatePointers()
{
    _ASSERTE(!HasMarksSet());

    size_t objLimit = Start() + Satori::REGION_SIZE_GRANULARITY;
    SatoriObject* o = FirstObject();
    do
    {
        o->ForEachObjectRef(
            [](SatoriObject** ppObject)
            {
                SatoriObject* child = *ppObject;
                if (child)
                {
                    ptrdiff_t ptr = ((ptrdiff_t*)child)[-1];
                    if (ptr < 0)
                    {
                        _ASSERTE(child->RawGetMethodTable() == ((SatoriObject*)-ptr)->RawGetMethodTable());
                        *ppObject = (SatoriObject*)-ptr;
                    }
                }
            }
        );

        o = o->Next();
    } while (o->Start() < objLimit);
}

void SatoriRegion::UpdatePointersInPromotedObjects()
{
    _ASSERTE(HasMarksSet());

    size_t objLimit = Start() + Satori::REGION_SIZE_GRANULARITY;
    SatoriObject* o = FirstObject();
    do
    {
        o = SkipUnmarked(o);
        if (o->Start() >= objLimit)
        {
            break;
        }

        _ASSERTE(IsMarked(o));

        ptrdiff_t r = ((ptrdiff_t*)o)[-1];
        _ASSERTE(r < 0);
        SatoriObject* relocated = (SatoriObject*)-r;
        _ASSERTE(relocated->RawGetMethodTable() == o->RawGetMethodTable());
        _ASSERTE(!relocated->IsFree());

        SatoriPage* page = relocated->ContainingRegion()->ContainingPage();
        relocated->ForEachObjectRef(
            [&](SatoriObject** ppObject)
            {
                // prevent re-reading o, UpdatePointersThroughCards could be doing the same update.
                SatoriObject* child = VolatileLoadWithoutBarrier(ppObject);
                if (child)
                {
                    ptrdiff_t ptr = ((ptrdiff_t*)child)[-1];
                    if (ptr < 0)
                    {
                        _ASSERTE(child->RawGetMethodTable() == ((SatoriObject*)-ptr)->RawGetMethodTable());
                        child = (SatoriObject*)-ptr;
                        VolatileStoreWithoutBarrier(ppObject, child);
                    }

                    // update the card as if the relocated object got a child assigned
                    if (child->ContainingRegion()->Generation() < 2)
                    {
                        page->SetCardForAddress((size_t)ppObject);
                    }
                }
            }
        );

        o = o->Next();
    } while (o->Start() < objLimit);
}

void SatoriRegion::TakeFinalizerInfoFrom(SatoriRegion* other)
{
    _ASSERTE(!other->HasPinnedObjects());
    m_everHadFinalizables |= other->m_everHadFinalizables;
    SatoriMarkChunk* otherFinalizables = other->m_finalizableTrackers;
    if (otherFinalizables)
    {
        SatoriMarkChunk* tail = otherFinalizables;
        while (tail->Next() != nullptr)
        {
            tail = tail->Next();
        }

        tail->SetNext(m_finalizableTrackers);
        m_finalizableTrackers = otherFinalizables;
        other->m_finalizableTrackers = nullptr;
    }
}

bool SatoriRegion::TryDemote()
{
    _ASSERTE(!HasMarksSet());
    _ASSERTE(Generation() == 2);
    _ASSERTE(ObjCount() != 0);

    if (ObjCount() > SatoriMarkChunk::Capacity())
    {
        return false;
    }

    SatoriMarkChunk* gen2Objects = Allocator()->TryGetMarkChunk();
    if (!gen2Objects)
    {
        return false;
    }

    this->m_generation = 1;
    size_t objLimit = Start() + Satori::REGION_SIZE_GRANULARITY;
    for (SatoriObject* o = FirstObject(); o->Start() < objLimit; o = o->Next())
    {
        if (!o->IsFree())
        {
            gen2Objects->Push(o);
        }
    }

    this->m_gen2Objects = gen2Objects;
    return true;
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

void SatoriRegion::ClearIndex()
{
    memset((void*)&m_index, 0, sizeof(m_index));
}

void SatoriRegion::ClearFreeLists()
{
    memset(m_freeLists, 0, sizeof(m_freeLists));
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
    SatoriObject* o = FirstObject();

    size_t objLimit = Start() + Satori::REGION_SIZE_GRANULARITY;
    while (o->Start() < objLimit)
    {
        o->Validate();

        if (o->Start() - Start() > Satori::REGION_SIZE_GRANULARITY)
        {
            _ASSERTE(o->IsFree());
        }
        else
        {
            _ASSERTE(allowMarked || !IsMarked(o));
        }

        prevPrevObj = prevObj;
        prevObj = o;
        o = o->Next();
    }

    _ASSERTE(o->Start() == End() ||
        (Size() > Satori::REGION_SIZE_GRANULARITY) && (End() - o->Start()) < Satori::REGION_SIZE_GRANULARITY);
#endif
}
