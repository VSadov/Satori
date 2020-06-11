// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.
//
// SatoriGCHeap.cpp
//

#include "common.h"

#include "gcenv.h"
#include "../env/gcenv.os.h"
#include "SatoriUtil.h"

MethodTable* SatoriUtil::s_emptyObjectMt;

void SatoriUtil::Initialize()
{
    s_emptyObjectMt = GCToEEInterface::GetFreeObjectMethodTable();
}

void SatoriUtil::MakeFreeObject(Object* obj, size_t size)
{
    _ASSERTE(size >= sizeof(Object) + sizeof(size_t));

    // sync block
    ((size_t*)obj)[-1] = 0;

    // mt
    obj->RawSetMethodTable(s_emptyObjectMt);

    // size
    // Note: we allow empty objects to be more than 4Gb on 64bit and use the "pad" for higher bits.
#if BIGENDIAN
#error "This won't work on big endian platforms"
#endif
    ((size_t*)obj)[ArrayBase::GetOffsetOfNumComponents() / sizeof(size_t)] = size - sizeof(ArrayBase);
}

