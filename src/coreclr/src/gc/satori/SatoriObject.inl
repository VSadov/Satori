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

inline size_t SatoriObject::Size()
{
    MethodTable* mt = RawGetMethodTable();
    size_t size = mt->GetBaseSize();
    if (mt->HasComponentSize())
    {
        size += ((size_t*)this)[ArrayBase::GetOffsetOfNumComponents() / sizeof(size_t)] * mt->RawGetComponentSize();
    }

    return ALIGN_UP(size, Satori::OBJECT_ALIGNMENT);
}

inline size_t SatoriObject::Start()
{
    return (size_t)this;
}

inline size_t SatoriObject::End()
{
    return Start() + Size();
}

inline SatoriObject* SatoriObject::Next()
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

inline bool SatoriObject::IsEscaped()
{
    return ((int8_t*)this)[-5];
}

inline void SatoriObject::SetEscaped()
{
    ((int8_t*)this)[-5] = 1;
}

inline bool SatoriObject::IsMarked()
{
    return !!(((int8_t*)this)[-6] & (1 << 7));
}

inline void SatoriObject::SetMarked()
{
    ((int8_t*)this)[-6] = (int8_t)(1 << 7);
}

inline bool SatoriObject::IsPinned()
{
    return !!(((int8_t*)this)[-6] & (1 << 6));
}

inline void SatoriObject::SetPinned()
{
    // pinned is always marked
    ((int8_t*)this)[-6] = (int8_t)(3 << 6);
}

inline bool SatoriObject::IsEscapedOrMarked()
{
    // TODO: VS more efficient
    return IsEscaped() || IsMarked();
    // return ((int32_t*)this)[-2] & (3 << 6);
}

inline int32_t SatoriObject::GetNextInMarkStack()
{
    // upper 10 bits are taken by escaped/marked/pinned
    // but we only need REGION_BITS, since we are recording distance
    return ((int32_t*)this)[-2] & ((1 << Satori::REGION_BITS) - 1);
}

inline void SatoriObject::SetNextInMarkStack(int32_t next)
{
    ASSERT(GetNextInMarkStack() == 0);
    ((int32_t*)this)[-2] |= next;
}

inline void SatoriObject::ClearNextInMarkStack()
{
    ((int32_t*)this)[-2] &= ~((1 << Satori::REGION_BITS) - 1);
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

    ptrdiff_t cnt = (ptrdiff_t)map->GetNumSeries();
    if (cnt >= 0)
    {
        // singular patterm. Either a regular type or an array of object references.
        CGCDescSeries* last = map->GetLowestSeries();
        size_t size = Size();

        do
        {
            SatoriObject** refPtr = (SatoriObject**)((size_t)this + cur->GetSeriesOffset());
            SatoriObject** refPtrStop = (SatoriObject**)((size_t)refPtr + cur->GetSeriesSize() + size);

            do
            {
                lambda(refPtr);
                refPtr++;
            } while (refPtr < refPtrStop);

            cur--;
        } while (cur >= last);
    }
    else
    {
        // repeating patern - array of structs
        SatoriObject** refPtr = (SatoriObject**)((size_t)this + cur->GetSeriesOffset());
        SatoriObject** end = (SatoriObject**)End();

        while (refPtr < end)
        {
            for (ptrdiff_t i = 0; i > cnt; i--)
            {
                val_serie_item item = cur->val_serie[i];
                SatoriObject** srcPtrStop = refPtr + item.nptrs;
                do
                {
                    lambda(refPtr);
                    refPtr++;
                } while (refPtr < srcPtrStop);

                refPtr = srcPtrStop + item.skip;
            }
        }
    }
}

#endif
