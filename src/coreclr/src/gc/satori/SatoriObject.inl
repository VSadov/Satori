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

#include "SatoriUtil.h"
#include "SatoriObject.h"

inline size_t SatoriObject::Size()
{
    MethodTable* mt = RawGetMethodTable();

    size_t size = mt->GetBaseSize();
    if (mt->HasComponentSize())
    {
        size += (size_t)((ArrayBase*)mt)->GetNumComponents() * mt->RawGetComponentSize();
    }

    return ALIGN_UP(size, Satori::OBJECT_ALIGNMENT);
}

inline SatoriObject* SatoriObject::Next()
{
    return (SatoriObject*)((size_t)this + Size());
}

inline SatoriRegion* SatoriObject::ContainingRegion()
{
    return (SatoriRegion*)((size_t)this >> Satori::REGION_BITS);
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

}

inline void SatoriObject::SetEscaped()
{

}

inline bool SatoriObject::IsMarked()
{

}

inline void SatoriObject::SetMarked()
{

}

inline bool SatoriObject::IsPinned()
{

}

inline void SatoriObject::SetPinned()
{

}

inline void SatoriObject::CleanSyncBlock()
{
    ((size_t*)this)[-1] = 0;
}

#endif
