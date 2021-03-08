// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.
//
// SatoriObject.inl
//

#ifndef __SATORI_OBJECT_INL__
#define __SATORI_OBJECT_INL__

#include "common.h"
#include "../gc.h"
#include "../gcdesc.h"

#include "SatoriUtil.h"
#include "SatoriObject.h"
#include "SatoriRegion.h"

FORCEINLINE size_t SatoriObject::Size()
{
    MethodTable* mt = RawGetMethodTable();
    size_t size = mt->GetBaseSize();
    if (mt->HasComponentSize())
    {
        size += (size_t)((ArrayBase*)this)->GetNumComponents() * mt->RawGetComponentSize();
        size = ALIGN_UP(size, Satori::OBJECT_ALIGNMENT);
    }

    return size;
}

inline size_t SatoriObject::Start()
{
    return (size_t)this;
}

FORCEINLINE size_t SatoriObject::End()
{
    return Start() + Size();
}

FORCEINLINE SatoriObject* SatoriObject::Next()
{
    return (SatoriObject*)End();
}

inline SatoriRegion* SatoriObject::ContainingRegion()
{
    return (SatoriRegion*)((size_t)this & ~(Satori::REGION_SIZE_GRANULARITY - 1));
}

inline SatoriObject* SatoriObject::At(size_t location)
{
    return (SatoriObject*)location;
}

inline bool SatoriObject::IsFree()
{
    return RawGetMethodTable() == s_emptyObjectMt;
}

#ifndef HOST_64BIT

32bit is NYI

#endif


inline void SatoriObject::SetBit(int offset)
{
    size_t word = Start() + offset * sizeof(size_t);
    size_t bitmapIndex = (word >> 9) & (SatoriRegion::BITMAP_LENGTH - 1);
    size_t mask = (size_t)1 << ((word >> 3) & 63);

    ContainingRegion()->m_bitmap[bitmapIndex] |= mask;

    //TODO: VS consider on x64
    //size_t bitIndex = (Start() & Satori::REGION_SIZE_GRANULARITY - 1) / sizeof(size_t) + offset;
    //_bittestandset64((long long*)ContainingRegion()->m_bitmap, bitIndex);
}

inline void SatoriObject::SetBitAtomic(int offset)
{
    size_t word = Start() + offset * sizeof(size_t);
    size_t bitmapIndex = (word >> 9) & (SatoriRegion::BITMAP_LENGTH - 1);
    size_t bit = ((word >> 3) & 63);

#ifdef _MSC_VER
#ifdef HOST_64BIT
    _interlockedbittestandset64((long long*)&ContainingRegion()->m_bitmap[bitmapIndex], bit);
#else
    _interlockedbittestandset((long*)&ContainingRegion()->m_bitmap[bitmapIndex], bit);
#endif
#else
    //NB: this does not need to be a full fence, if possible. 
    //    just need to set a bit atomically. (i.e. LDSET on ARM 8.1)
    //    ordering WRT other writes is unimportant, as long as the bit is set.
    size_t mask = (size_t)1 << bit;
    __sync_or_and_fetch(&ContainingRegion()->m_bitmap[bitmapIndex], mask);
#endif
}

inline void SatoriObject::ClearBit(int offset)
{
    size_t word = Start() + offset * sizeof(size_t);
    size_t bitmapIndex = (word >> 9) & (SatoriRegion::BITMAP_LENGTH - 1);
    size_t mask = (size_t)1 << ((word >> 3) & 63);

    ContainingRegion()->m_bitmap[bitmapIndex] &= ~mask;
}

inline bool SatoriObject::CheckBit(int offset)
{
    size_t word = Start() + offset * sizeof(size_t);
    size_t bitmapIndex = (word >> 9) & (SatoriRegion::BITMAP_LENGTH - 1);
    size_t mask = (size_t)1 << ((word >> 3) & 63);

    return ContainingRegion()->m_bitmap[bitmapIndex] & mask;

    //TODO: VS consider on x64
    //size_t bitIndex = (Start() & Satori::REGION_SIZE_GRANULARITY - 1) / sizeof(size_t) + offset;
    //return _bittest64((long long*)ContainingRegion()->m_bitmap, bitIndex);
}

inline bool SatoriObject::IsMarked()
{
    return CheckBit(0);
}

FORCEINLINE bool SatoriObject::IsMarkedOrOlderThan(int generation)
{
    return ContainingRegion()->Generation() > generation || IsMarked();
}

inline void SatoriObject::SetMarked()
{
    SetBit(0);
}

inline void SatoriObject::ClearMarked()
{
    ClearBit(0);
}

inline void SatoriObject::SetMarkedAtomic()
{
    SetBitAtomic(0);
}

inline bool SatoriObject::IsPinned()
{
    return CheckBit(2);
}

inline void SatoriObject::SetPinned()
{
    SetBit(2);
}

inline void SatoriObject::ClearPinnedAndMarked()
{
    ClearMarked();
    if (IsPinned())
    {
        // this would be rare. do not inline.
        ClearPinned();
    }
}

inline bool SatoriObject::IsEscaped()
{
    return CheckBit(1);
}

inline void SatoriObject::SetEscaped()
{
    SetBit(1);
}

inline bool SatoriObject::IsEscapedOrPinned()
{
    return IsEscaped() || IsPinned();
}

inline bool SatoriObject::IsFinalizationSuppressed()
{
    return GetHeader()->GetBits() & BIT_SBLK_FINALIZER_RUN;
}

inline void SatoriObject::SuppressFinalization()
{
    GetHeader()->SetBit(BIT_SBLK_FINALIZER_RUN);
}

inline void SatoriObject::UnSuppressFinalization()
{
    GetHeader()->ClrBit(BIT_SBLK_FINALIZER_RUN);
}

//
// Implementation note on mark overflow and relocation - we could use temporary maps,
// but we will use unused bits in the syncblock instead.
//

inline int32_t SatoriObject::GetNextInMarkStack()
{
    return ((int32_t*)this)[-2];
}

inline void SatoriObject::SetNextInMarkStack(int32_t next)
{
    _ASSERTE(GetNextInMarkStack() == 0);
    ((int32_t*)this)[-2] = next;
}

inline void SatoriObject::ClearNextInMarkStack()
{
    ((int32_t*)this)[-2] = 0;
}

inline int32_t SatoriObject::GetReloc()
{
    // since relocation does not intersect with uses of mark stack
    // we will use the same bits.
    return GetNextInMarkStack();
}

inline void SatoriObject::SetReloc(int32_t next)
{
    // since relocation does not intersect with uses of mark stack
    // we will use the same bits.
    SetNextInMarkStack(next);
}

inline void SatoriObject::ClearMarkCompactStateForRelocation()
{
    _ASSERTE(!IsEscaped());
    // since relocation does not intersect with uses of mark stack
    // we will use the same bits.
    ClearNextInMarkStack();
    ClearBit(0);
    ClearBit(2);
}

inline void SatoriObject::CleanSyncBlock()
{
    ((size_t*)this)[-1] = 0;
}

inline int SatoriObject::GetMarkBitAndWord(size_t* bitmapIndex)
{
    size_t start = Start();
    *bitmapIndex = (start >> 9) & (SatoriRegion::BITMAP_LENGTH - 1); // % words in the bitmap
    return (start >> 3) & 63;     // % bits in a word
}

inline void SatoriObject::SetPermanentlyPinned()
{
    ((DWORD*)this)[-1] |= BIT_SBLK_GC_RESERVE;
}

inline bool SatoriObject::IsPermanentlyPinned()
{
    return ((DWORD*)this)[-1] & BIT_SBLK_GC_RESERVE;
}

template<typename F>
inline void SatoriObject::ForEachObjectRef(F& lambda, bool includeCollectibleAllocator)
{
    MethodTable* mt = RawGetMethodTable();

    if (includeCollectibleAllocator && mt->Collectible())
    {
        uint8_t* loaderAllocator = GCToEEInterface::GetLoaderAllocatorObjectForGC(this);
        // NB: Allocator ref location is fake. The actual location is a handle).
        //     For that same reason relocation callers should not care about the location.
        lambda((SatoriObject**)&loaderAllocator);
    }

    if (!mt->ContainsPointers())
    {
        return;
    }

    CGCDesc* map = CGCDesc::GetCGCDescFromMT(mt);
    CGCDescSeries* cur = map->GetHighestSeries();

    // GetNumSeries is actually signed.
    // Negative value means the pattern repeats -cnt times such as in a case of arrays
    ptrdiff_t cnt = (ptrdiff_t)map->GetNumSeries();
    if (cnt >= 0)
    {
        CGCDescSeries* last = map->GetLowestSeries();

        // series size is offset by the object size
        size_t size = mt->GetBaseSize();

        // object arrays are handled here too, so need to compensate for that.
        if (mt->HasComponentSize())
        {
            size += (size_t)((ArrayBase*)this)->GetNumComponents() * sizeof(size_t);
        }

        do
        {
            size_t refPtr = (size_t)this + cur->GetSeriesOffset();
            size_t refPtrStop = refPtr + size + cur->GetSeriesSize();

            // top check loop. this could be a zero-element array
            while (refPtr < refPtrStop)
            {
                lambda((SatoriObject**)refPtr);
                refPtr += sizeof(size_t);
            }
            cur--;
        } while (cur >= last);
    }
    else
    {
        // repeating patern - an array
        size_t refPtr = (size_t)this + cur->GetSeriesOffset();
        uint32_t componentNum = ((ArrayBase*)this)->GetNumComponents();
        while (componentNum-- > 0)
        {
            for (ptrdiff_t i = 0; i > cnt; i--)
            {
                val_serie_item item = cur->val_serie[i];
                size_t refPtrStop = refPtr + item.nptrs * sizeof(size_t);
                do
                {
                    lambda((SatoriObject**)refPtr);
                    refPtr += sizeof(size_t);
                } while (refPtr < refPtrStop);

                refPtr += item.skip;
            }
        }
    }
}

template<typename F>
inline void SatoriObject::ForEachObjectRef(F& lambda, size_t start, size_t end)
{
    MethodTable* mt = RawGetMethodTable();

    if (start <= Start() && mt->Collectible())
    {
        uint8_t* loaderAllocator = GCToEEInterface::GetLoaderAllocatorObjectForGC(this);
        // NB: Allocator ref location is fake. The actual location is a handle).
        //     it is ok to "update" the ref, but it will have no effect
        lambda((SatoriObject**)&loaderAllocator);
    }

    if (!mt->ContainsPointers())
    {
        return;
    }

    CGCDesc* map = CGCDesc::GetCGCDescFromMT(mt);
    CGCDescSeries* cur = map->GetHighestSeries();

    // GetNumSeries is actually signed.
    // Negative value means the pattern repeats -cnt times such as in a case of arrays
    ptrdiff_t cnt = (ptrdiff_t)map->GetNumSeries();
    if (cnt >= 0)
    {
        CGCDescSeries* last = map->GetLowestSeries();

        // series size is offset by the object size
        size_t size = mt->GetBaseSize();

        // object arrays are handled here too, so need to compensate for that.
        if (mt->HasComponentSize())
        {
            size += (size_t)((ArrayBase*)this)->GetNumComponents() * sizeof(size_t);
        }

        do
        {
            size_t refPtr = (size_t)this + cur->GetSeriesOffset();
            size_t refPtrStop = refPtr + size + cur->GetSeriesSize();

            refPtr = max(refPtr, start);

            // top check loop. this could be a zero-element array
            while (refPtr < refPtrStop)
            {
                if (refPtr >= end)
                {
                    return;
                }

                lambda((SatoriObject**)refPtr);
                refPtr += sizeof(size_t);
            }

            cur--;
        } while (cur >= last);
    }
    else
    {
        // repeating patern - an array of structs
        ptrdiff_t componentNum = ((ArrayBase*)this)->GetNumComponents();
        size_t elementSize = mt->RawGetComponentSize();
        size_t refPtr = (size_t)this + cur->GetSeriesOffset();

        if (refPtr < start)
        {
            size_t skip = (start - refPtr) / elementSize;
            componentNum -= skip;
            refPtr += skip * elementSize;
        }

        while (componentNum-- > 0)
        {
            for (ptrdiff_t i = 0; i > cnt; i--)
            {
                val_serie_item item = cur->val_serie[i];
                size_t refPtrStop = refPtr + item.nptrs * sizeof(size_t);

                if (refPtrStop <= start)
                {
                    // serie does not intersect with the range
                    refPtr = refPtrStop;
                }
                else
                {
                    refPtr = max(refPtr, start);
                    do
                    {
                        if (refPtr >= end)
                        {
                            return;
                        }

                        lambda((SatoriObject**)refPtr);
                        refPtr += sizeof(size_t);
                    } while (refPtr < refPtrStop);

                }

                refPtr += item.skip;
            }
        }
    }
}
#endif
