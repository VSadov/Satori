// Copyright (c) 2022 Vladimir Sadov
//
// Permission is hereby granted, free of charge, to any person
// obtaining a copy of this software and associated documentation
// files (the "Software"), to deal in the Software without
// restriction, including without limitation the rights to use,
// copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following
// conditions:
//
// The above copyright notice and this permission notice shall be
// included in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
// OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
// HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
// WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
// OTHER DEALINGS IN THE SOFTWARE.
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
#include "SatoriPage.h"
#include "SatoriWorkChunk.h"

inline bool SatoriRegion::CanSplitWithoutCommit(size_t size)
{
    return m_committed > (m_end - size + offsetof(SatoriRegion, m_syncBlock) + SatoriUtil::MinZeroInitSize());
}

inline bool SatoriRegion::IsEscapeTracking()
{
    _ASSERTE(!m_ownerThreadTag || m_generation == 0);
    return m_ownerThreadTag;
}

inline bool SatoriRegion::MaybeEscapeTrackingAcquire()
{
    // must check reusable level before the owner tag, before doing whatever follows
    return VolatileLoad((uint8_t*)&m_reusableFor) == (uint8_t)ReuseLevel::Gen0 ||
        VolatileLoad(&m_ownerThreadTag);
}

inline bool SatoriRegion::IsEscapeTrackedByCurrentThread()
{
    return m_ownerThreadTag == SatoriUtil::GetCurrentThreadTag();
}

inline int SatoriRegion::Generation()
{
    return m_generation;
}

inline int SatoriRegion::GenerationAcquire()
{
    return VolatileLoad(&m_generation);
}

inline void SatoriRegion::SetGeneration(int generation)
{
    // Gen0 is controlled by thread-local tracking
    _ASSERTE(generation != 0);
    _ASSERTE(m_generation != 0);

    m_generation = generation;
}

inline void SatoriRegion::SetGenerationRelease(int generation)
{
    // at the time when we set generation to 0+, we should make it certain if
    // the region is attached, since 0+ detached regions are assumed parseable.
    _ASSERTE(generation != 0 && generation != 2);
    _ASSERTE(m_generation == -1 || IsReusable());

    VolatileStore(&m_generation, generation);
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

inline size_t SatoriRegion::GetAllocStart()
{
    return m_allocStart;
}

inline size_t SatoriRegion::GetAllocRemaining()
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

inline void SatoriRegion::StartEscapeTrackingRelease(size_t threadTag)
{
    _ASSERTE(m_generation == -1 || m_reusableFor == ReuseLevel::Gen0);
    m_escapeFunc = EscapeFn;
    m_ownerThreadTag = threadTag;
    VolatileStore(&m_generation, 0);
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
        m_escapedSize = 0;
        m_allocBytesAtCollect = 0;
    }
}

template <typename F>
void SatoriRegion::ForEachFinalizable(F lambda)
{
    size_t items = 0;
    size_t nulls = 0;
    SatoriWorkChunk* chunk = m_finalizableTrackers;
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
template <typename F>
void SatoriRegion::ForEachFinalizableThreadLocal(F lambda)
{
    LockFinalizableTrackers();
    ForEachFinalizable(lambda);
    UnlockFinalizableTrackers();
}

template <bool updatePointers>
bool SatoriRegion::Sweep()
{
    // we should only sweep when we have marks
    _ASSERTE(HasMarksSet());
    _ASSERTE(!DoNotSweep());

    size_t objLimit = Start() + Satori::REGION_SIZE_GRANULARITY;

    // in a huge region there is only one real object. If it is unreachable the region is all empty.
    if (End() > objLimit)
    {
        SatoriObject* o = FirstObject()->IsFree() ? FirstObject()->Next() : FirstObject();
        if (!IsMarked(o))
        {
            _ASSERTE(this->NothingMarked());
            _ASSERTE(!this->HasPinnedObjects());
#if _DEBUG
            size_t fistObjStart = FirstObject()->Start();
            SatoriObject::FormatAsFree(fistObjStart, objLimit - fistObjStart);
            this->HasMarksSet() = false;
#endif
            this->DoNotSweep() = true;
            SetOccupancy(0, 0);
            return false;
        }
    }
    else
    {
        // we will be building new free lists
        ClearFreeLists();
    }

    m_escapedSize = 0;
    bool cannotRecycle = this->IsAttachedToContext();
    bool isEscapeTracking = this->IsEscapeTracking();
    size_t occupancy = 0;
    size_t objCount = 0;
    bool hasFinalizables = false;
    SatoriObject* o = FirstObject();
    do
    {
        if (!CheckAndClearMarked(o))
        {
            size_t lastMarkedEnd = o->Start();
            o = SkipUnmarkedAndClear(o);
            size_t skipped = o->Start() - lastMarkedEnd;
            SatoriObject* free = SatoriObject::FormatAsFree(lastMarkedEnd, skipped);
            SetIndicesForObject(free, o->Start());
            AddFreeSpace(free);

            if (o->Start() >= objLimit)
            {
                _ASSERTE(o->Start() == objLimit);
                break;
            }
        }

        _ASSERTE(!o->IsFree());
        cannotRecycle = true;

        if (isEscapeTracking)
        { 
            this->EscapeShallow(o);
        }

        if (updatePointers)
        {
            UpdatePointersInObject(o);
        }

        if (!hasFinalizables && o->RawGetMethodTable()->HasFinalizer())
        {
            hasFinalizables = true;
        }

        size_t size = o->Size();
        objCount++; 
        occupancy += size;
        o = (SatoriObject*)(o->Start() + size);
    } while (o->Start() < objLimit);

    _ASSERTE(o->Start() == objLimit || End() > objLimit);
    _ASSERTE(hasFinalizables || !m_finalizableTrackers);
#if _DEBUG
    this->HasMarksSet() = false;
#endif

    this->m_hasFinalizables = hasFinalizables;
    this->DoNotSweep() = true;
    this->HasPinnedObjects() = false;

    SetOccupancy(occupancy, objCount);
    return cannotRecycle;
}

inline bool SatoriRegion::HasFinalizables()
{
    return m_hasFinalizables;
}

inline bool& SatoriRegion::HasPendingFinalizables()
{
    return m_hasPendingFinalizables;
}

inline size_t SatoriRegion::Occupancy()
{
    _ASSERTE(!IsAllocating());
    return m_occupancy;
}

inline size_t &SatoriRegion::OccupancyAtReuse()
{
    _ASSERTE(!IsAllocating());
    return m_occupancyAtReuse;
}

inline size_t SatoriRegion::ObjCount()
{
    return m_objCount;
}

inline bool& SatoriRegion::HasPinnedObjects()
{
    return m_hasPinnedObjects;
}

#if _DEBUG
inline bool& SatoriRegion::HasMarksSet()
{
    return m_hasMarksSet;
}
#endif

inline bool& SatoriRegion::DoNotSweep()
{
    return m_doNotSweep;
}

inline bool& SatoriRegion::AcceptedPromotedObjects()
{
    return m_acceptedPromotedObjects;
}

inline bool SatoriRegion::IsReusable()
{
    return m_reusableFor != ReuseLevel::None;
}

inline SatoriRegion::ReuseLevel& SatoriRegion::ReusableFor()
{
    return m_reusableFor;
}

inline SatoriQueue<SatoriRegion>* SatoriRegion::ContainingQueue()
{
    return VolatileLoadWithoutBarrier(&m_containingQueue);
}

inline void SatoriRegion::AttachToContext(SatoriRegion** attachementPoint)
{
    _ASSERTE(!m_allocationContextAttachmentPoint);
    _ASSERTE(!*attachementPoint);

    *attachementPoint = this;
    m_allocationContextAttachmentPoint = attachementPoint;
}

inline void SatoriRegion::DetachFromContextRelease()
{
    _ASSERTE(*m_allocationContextAttachmentPoint == this);

    if (IsEscapeTracking())
    {
        StopEscapeTracking();
    }

    *m_allocationContextAttachmentPoint = nullptr;
    // all allocations must be committed prior to detachement.
    VolatileStore(&m_allocationContextAttachmentPoint, (SatoriRegion**)nullptr);
}

inline bool SatoriRegion::IsAttachedToContext()
{
    return m_allocationContextAttachmentPoint;
}

inline bool SatoriRegion::MaybeAttachedToContextAcquire()
{
    // must check reusable level before the attach point, before doing whatever follows
    return VolatileLoad((uint8_t*)&m_reusableFor) ||
        VolatileLoad(&m_allocationContextAttachmentPoint);
}

inline void SatoriRegion::ResetReusableForRelease()
{
    VolatileStore(&m_reusableFor, SatoriRegion::ReuseLevel::None);
}

inline bool SatoriRegion::IsDemoted()
{
    return m_gen2Objects;
}

inline SatoriWorkChunk* &SatoriRegion::DemotedObjects()
{
    return m_gen2Objects;
}

inline void SatoriRegion::SetMarked(SatoriObject* o)
{
    _ASSERTE(o->ContainingRegion() == this);

    size_t word = o->Start();
    size_t bitmapIndex = (word >> 9) & (SatoriRegion::BITMAP_LENGTH - 1);
    size_t mask = (size_t)1 << ((word >> 3) & 63);

    m_bitmap[bitmapIndex] |= mask;
}

inline void SatoriRegion::SetMarkedAtomic(SatoriObject* o)
{
    _ASSERTE(o->ContainingRegion() == this);
    _ASSERTE(!o->IsFree());

    size_t word = o->Start();
    size_t bitmapIndex = (word >> 9) & (SatoriRegion::BITMAP_LENGTH - 1);
    size_t mask = (size_t)1 << ((word >> 3) & 63);

    //NB: this does not need to be a full fence, if possible. 
    //    just need to set a bit atomically.
    //    ordering WRT other writes is unimportant, as long as the bit is set.
#ifdef _MSC_VER
#if defined(TARGET_AMD64)
    _InterlockedOr64((long long*)&m_bitmap[bitmapIndex], mask);
#else
    _InterlockedOr64_nf((long long*)&m_bitmap[bitmapIndex], mask);
#endif
#else
    __atomic_or_fetch(&m_bitmap[bitmapIndex], mask, __ATOMIC_RELAXED);
#endif
}

inline void SatoriRegion::ClearMarked(SatoriObject* o)
{
    _ASSERTE(o->ContainingRegion() == this);

    size_t word = o->Start();
    size_t bitmapIndex = (word >> 9) & (SatoriRegion::BITMAP_LENGTH - 1);
    size_t mask = (size_t)1 << ((word >> 3) & 63);

    m_bitmap[bitmapIndex] &= ~mask;
}

inline bool SatoriRegion::IsMarked(SatoriObject* o)
{
    _ASSERTE(o->ContainingRegion() == this);

    size_t word = o->Start();
    size_t bitmapIndex = (word >> 9) & (SatoriRegion::BITMAP_LENGTH - 1);
    size_t mask = (size_t)1 << ((word >> 3) & 63);

    return m_bitmap[bitmapIndex] & mask;
}

inline bool SatoriRegion::CheckAndClearMarked(SatoriObject* o)
{
    _ASSERTE(o->ContainingRegion() == this);

    size_t word = o->Start();
    size_t bitmapIndex = (word >> 9) & (SatoriRegion::BITMAP_LENGTH - 1);
    size_t mask = (size_t)1 << ((word >> 3) & 63);

    size_t& bitmapWord = m_bitmap[bitmapIndex];
    bool wasMarked = bitmapWord & mask;
    bitmapWord &= ~mask;
    return wasMarked;
}

inline bool SatoriRegion::IsPinned(SatoriObject* o)
{
    return IsMarked(o + MarkOffset::Pinned);
}

inline void SatoriRegion::SetPinned(SatoriObject* o)
{
    SetMarked(o + MarkOffset::Pinned);
}

inline void SatoriRegion::ClearPinnedAndMarked(SatoriObject* o)
{
    ClearMarked(o);
    if (IsPinned(o))
    {
        // this would be rare. do not inline.
        ClearPinned(o);
    }
}

inline bool SatoriRegion::IsEscaped(SatoriObject* o)
{
    return IsMarked(o + MarkOffset::Escaped);
}

inline void SatoriRegion::SetEscaped(SatoriObject* o)
{
    SetMarked(o + MarkOffset::Escaped);
}

inline bool SatoriRegion::IsEscapedOrPinned(SatoriObject* o)
{
    return IsEscaped(o) || IsPinned(o) || o->IsUnmovable();
}

inline void SatoriRegion::SetExposed(SatoriObject** location)
{
    // set the mark bit corresponding to the location to indicate that it is globally exposed
    // same as: SetMarked((SatoriObject*)location);
    SetMarked((SatoriObject*)location);
}

inline bool SatoriRegion::IsExposed(SatoriObject** location)
{
    // check the mark bit corresponding to the location
    //same as: return IsMarked((SatoriObject*)location);
    return IsMarked((SatoriObject*)location);
}

inline SatoriObject* SatoriRegion::ObjectForMarkBit(size_t bitmapIndex, int offset)
{
    size_t objOffset = bitmapIndex * sizeof(size_t) * 8 + offset;
    return (SatoriObject*)(Start()) + objOffset;
}

inline size_t SatoriRegion::LocationToIndex(size_t location)
{
    return (location - Start()) >> Satori::INDEX_GRANULARITY_BITS;
}

inline void SatoriRegion::SetIndicesForObject(SatoriObject* o, size_t end)
{
    _ASSERTE(end > o->Start());
    _ASSERTE(end <= Start() + Satori::REGION_SIZE_GRANULARITY);

    size_t start = o->Start();

    // if object straddles index granules, record the object in corresponding indices
    if ((start ^ end) >> Satori::INDEX_GRANULARITY_BITS)
    {
        SetIndicesForObjectCore(start, end);
    }
}

inline void SatoriRegion::ClearIndicesForAllocRange()
{
    _ASSERTE(m_allocStart < m_allocEnd);
    _ASSERTE(m_allocStart > Start());

    size_t start = m_allocStart;
    size_t end = min(m_allocEnd, Start() + Satori::REGION_SIZE_GRANULARITY);

    // if range straddles index granules, clear corresponding indices
    if ((start ^ end) >> Satori::INDEX_GRANULARITY_BITS)
    {
        size_t firstIndex = LocationToIndex(start) + 1;
        // if first is clean, all are clean
        if (m_index[firstIndex] != 0)
        {
            size_t lastIndex = LocationToIndex(end);
            memset((void*)&m_index[firstIndex], 0, (lastIndex - firstIndex + 1) * sizeof(int));
        }

#if _DEBUG
        for(size_t i = firstIndex, last = LocationToIndex(end); i <= last; i++)
        {
            _ASSERTE(m_index[i] == 0);
        }
#endif
    }
}

template <bool promotingAllRegions>
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
            _ASSERTE(o->Start() == objLimit);
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
                    if (!promotingAllRegions)
                    {
                        if (child->ContainingRegion()->Generation() < 2)
                        {
                            page->SetCardForAddress((size_t)ppObject);
                        }
                    }
                }
            }
        );

        o = o->Next();
    } while (o->Start() < objLimit);
}

#endif
