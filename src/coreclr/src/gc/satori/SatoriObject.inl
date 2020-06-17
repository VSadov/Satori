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
    return (SatoriRegion*)ALIGN_DOWN((size_t)this, Satori::REGION_SIZE_GRANULARITY);
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
// Implementation note on Escaped/Marked/Pinned -
// We could use some kind of bitmap instead. We would only need 3bits or perhaps even 2 per object.
// However on 64bit there is a plenty of unused bits in the pad of the syncblock, so we will use those.
// On 32bit it may make more sense to use bitmaps.
//
// Same logic applies to mark overflow and relocation - we could use temporary maps,
// but we will use unused bits instead.
//

inline bool SatoriObject::IsEscaped()
{
    return ((int8_t*)this)[-5];
}

inline void SatoriObject::SetEscaped()
{
    ((int8_t*)this)[-5] = (int8_t)0xFF;
}

inline bool SatoriObject::IsMarked()
{
    return ((int8_t*)this)[-6] & 0b10000000;
}

inline void SatoriObject::SetMarked()
{
    ((int8_t*)this)[-6] |= (int8_t)0b10000000;
}

inline bool SatoriObject::IsPinned()
{
    return ((int8_t*)this)[-6] & 0b01000000;
}

inline void SatoriObject::SetPinnedAndMarked()
{
    ((int8_t*)this)[-6] |= (int8_t)0b11000000;
}

inline void SatoriObject::ClearPinnedAndMarked()
{
    ASSERT(GetReloc() == 0);
    ((int8_t*)this)[-6] = 0;
}

inline bool SatoriObject::IsEscapedOrMarked()
{
    // return IsEscaped() || IsMarked();
    return ((int32_t*)this)[-2] & 0b11111111'10000000'00000000'00000000;
}

inline bool SatoriObject::IsEscapedOrPinned()
{
    // return IsEscaped() || IsPinned();
    return ((int32_t*)this)[-2] & 0b11111111'01000000'00000000'00000000;
}

inline int32_t SatoriObject::GetNextInMarkStack()
{
    // upper 10 bits are taken by escaped byte and marked/pinned bits
    // but we only need REGION_BITS, since we are recording distance within the region
    return ((int32_t*)this)[-2] & ((1 << Satori::REGION_BITS) - 1);
}

inline void SatoriObject::SetNextInMarkStack(int32_t next)
{
    ASSERT(GetNextInMarkStack() == 0);
    ((int32_t*)this)[-2] |= next;
}

// TODO: VS same as [Get|Set]NextInMarkStack

inline int32_t SatoriObject::GetReloc()
{
    // upper 10 bits are taken by escaped/marked/pinned
    // but we only need REGION_BITS, since we are recording distance within the region
    return ((int32_t*)this)[-2] & ((1 << Satori::REGION_BITS) - 1);
}

inline void SatoriObject::SetReloc(int32_t next)
{
    ASSERT(GetReloc() == 0);
    ((int32_t*)this)[-2] |= next;
}

inline void SatoriObject::ClearNextInMarkStack()
{
    ((int32_t*)this)[-2] &= ~((1 << Satori::REGION_BITS) - 1);
}

inline void SatoriObject::ClearMarkCompactState()
{
    ASSERT(!IsEscaped());
    ((int32_t*)this)[-2] = 0;
}

inline void SatoriObject::CleanSyncBlock()
{
    ((size_t*)this)[-1] = 0;
}

template<typename F>
inline void SatoriObject::ForEachObjectRef(F& lambda)
{
    MethodTable* mt = RawGetMethodTable();
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
