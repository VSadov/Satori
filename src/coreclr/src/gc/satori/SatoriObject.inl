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

//
// Implementation note on mark overflow and relocation - we could use temporary maps,
// but we will use unused bits in the syncblock instead.
//

#ifndef HOST_64BIT

fix the following for 32bit

#endif

inline void SatoriObject::SetBit(int offset)
{
    size_t objOffset = Start() & (Satori::REGION_SIZE_GRANULARITY - 1);
    size_t bitOffset = (objOffset >> 3) + offset;
    size_t wordOffset = bitOffset >> 6;
    size_t maskBit = bitOffset & 63;
    size_t mask = (size_t)1 << maskBit;

    ContainingRegion()->m_bitmap[wordOffset] |= mask;
}

inline void SatoriObject::ClearBit(int offset)
{
    size_t objOffset = Start() & (Satori::REGION_SIZE_GRANULARITY - 1);
    size_t bitOffset = (objOffset >> 3) + offset;
    size_t wordOffset = bitOffset >> 6;
    size_t maskBit = bitOffset & 63;
    size_t mask = (size_t)1 << maskBit;

    ContainingRegion()->m_bitmap[wordOffset] &= ~mask;
}

inline bool SatoriObject::CheckBit(int offset)
{
    size_t objOffset = Start() & (Satori::REGION_SIZE_GRANULARITY - 1);
    size_t bitOffset = (objOffset >> 3) + offset;
    size_t wordOffset = bitOffset >> 6;
    size_t maskBit = bitOffset & 63;
    size_t mask = (size_t)1 << maskBit;

    return ContainingRegion()->m_bitmap[wordOffset] & mask;
}

inline bool SatoriObject::IsMarked()
{
    return CheckBit(0);
}

inline void SatoriObject::SetMarked()
{
    SetBit(0);
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
    _ASSERTE(GetReloc() == 0);
    ClearBit(0);
    ClearBit(2);
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

    //TODO: VS the following is not faster
    /*size_t objOffset = Start() & (Satori::REGION_SIZE_GRANULARITY - 1);
    size_t bitOffset = (objOffset >> 3) + 1;
    size_t wordOffset = bitOffset >> 6;
    size_t maskBit = bitOffset & 63;
    size_t mask = (size_t)3 << maskBit;

    bool result = ContainingRegion()->m_bitmap[wordOffset] & mask;
    if (maskBit == 63)
    {
        result |= ContainingRegion()->m_bitmap[wordOffset + 1] & 1;
    }

    return result;*/
}

inline bool SatoriObject::IsFinalizationSuppressed()
{
    return GetHeader()->GetBits() & BIT_SBLK_FINALIZER_RUN;
}

// TODO: VS consolidate to common impl.
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

// TODO: VS same as [Get|Set]NextInMarkStack
// TODO: VS rename GetLocalReloc
inline int32_t SatoriObject::GetReloc()
{
    return ((int32_t*)this)[-2];
}

inline void SatoriObject::SetReloc(int32_t next)
{
    _ASSERTE(GetReloc() == 0);
    ((int32_t*)this)[-2] = next;
}

inline void SatoriObject::ClearMarkCompactStateForRelocation()
{
    _ASSERTE(!IsEscaped());
    ((int32_t*)this)[-2] = 0;
    ClearBit(0);
    ClearBit(2);
}

inline void SatoriObject::CleanSyncBlock()
{
    ((size_t*)this)[-1] = 0;
}

template<typename F>
inline void SatoriObject::ForEachObjectRef(F& lambda)
{
    MethodTable* mt = RawGetMethodTable();

    if (mt->Collectible())
    {
        uint8_t* loaderAllocator = GCToEEInterface::GetLoaderAllocatorObjectForGC(this);
        // NB: lambda my modify loaderAllocator for relocation, that is redundant, but ok.
        //     actual accesses to loader are via a week handle, which will be updated
        //     as necessary.
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

            // immediately check. this could be a zero-element array
            // TODO: VS cant't use simple "while" here. Compiler bug?
            if (refPtr < refPtrStop)
            {
                do
                {
                    lambda((SatoriObject**)refPtr);
                    refPtr += sizeof(size_t);
                } while (refPtr < refPtrStop);
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
#endif
