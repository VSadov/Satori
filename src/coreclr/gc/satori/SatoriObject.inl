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

inline bool SatoriObject::IsFree()
{
    return RawGetMethodTable() == s_emptyObjectMt;
}

#ifndef HOST_64BIT

32bit is NYI

#endif

inline bool SatoriObject::IsEscaped()
{
    return ContainingRegion()->IsEscaped(this);
}

inline bool SatoriObject::IsMarked()
{
    return ContainingRegion()->IsMarked(this);
}

FORCEINLINE bool SatoriObject::IsMarkedOrOlderThan(int generation)
{
    _ASSERTE(this->Size() > 0);
    SatoriRegion* r = ContainingRegion();
    return r->Generation() > generation || r->IsMarked(this);
}

inline void SatoriObject::SetMarked()
{
    ContainingRegion()->SetMarked(this);
}

inline void SatoriObject::SetMarkedAtomic()
{
    return ContainingRegion()->SetMarkedAtomic(this);
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

inline int32_t SatoriObject::GetNextInLocalMarkStack()
{
    return ((int32_t*)this)[-2];
}

inline void SatoriObject::SetNextInLocalMarkStack(int32_t next)
{
    _ASSERTE(GetNextInLocalMarkStack() == 0);
    ((int32_t*)this)[-2] = next;
}

inline void SatoriObject::ClearNextInLocalMarkStack()
{
    ((int32_t*)this)[-2] = 0;
}

inline int32_t SatoriObject::GetLocalReloc()
{
    // since local relocation does not intersect with uses of mark stack
    // we will use the same bits.
    return GetNextInLocalMarkStack();
}

inline void SatoriObject::SetLocalReloc(int32_t next)
{
    // since local relocation does not intersect with uses of mark stack
    // we will use the same bits.
    SetNextInLocalMarkStack(next);
}

inline void SatoriObject::ClearMarkCompactStateForRelocation()
{
    // since local relocation does not intersect with uses of mark stack
    // we will use the same bits.
    ClearNextInLocalMarkStack();

    SatoriRegion* r = ContainingRegion();
    _ASSERTE(!r->IsEscaped(this));
    r->ClearMarked(this);
    r->ClearPinned(this);
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

inline void SatoriObject::SetUnmovable()
{
    ((DWORD*)this)[-1] |= BIT_SBLK_GC_RESERVE;
}

inline bool SatoriObject::IsUnmovable()
{
    return ((DWORD*)this)[-1] & BIT_SBLK_GC_RESERVE;
}

template <typename F>
inline void SatoriObject::ForEachObjectRef(F lambda, bool includeCollectibleAllocator)
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

template <typename F>
inline void SatoriObject::ForEachObjectRef(F lambda, size_t start, size_t end)
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
