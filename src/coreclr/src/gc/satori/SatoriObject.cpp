// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.
//
// SatoriObject.cpp
//

#include "common.h"

#include "gcenv.h"
#include "../env/gcenv.os.h"
#include "SatoriObject.h"
#include "SatoriObject.inl"

MethodTable* SatoriObject::s_emptyObjectMt;

void SatoriObject::Initialize()
{
    s_emptyObjectMt = GCToEEInterface::GetFreeObjectMethodTable();
}

SatoriObject* SatoriObject::FormatAsFree(size_t location, size_t size)
{
    _ASSERTE(size >= sizeof(Object) + sizeof(size_t));

    SatoriObject* obj = SatoriObject::At(location);
    obj->CleanSyncBlock();
    obj->RawSetMethodTable(s_emptyObjectMt);

    // Note: we allow empty objects to be more than 4Gb on 64bit and use the "pad" for higher bits.
#if BIGENDIAN
#error "This won't work on big endian platforms"
#endif
    ((size_t*)obj)[ArrayBase::GetOffsetOfNumComponents() / sizeof(size_t)] = size - sizeof(ArrayBase);

    return obj;
}

inline void SatoriObject::Validate()
{
    //TODO: VS
}
