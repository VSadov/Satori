// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.
//
// SatoriGCHeap.h
//

#ifndef __SATORI_UTIL_H__
#define __SATORI_UTIL_H__

#include "common.h"
#include "../gc.h"


//TODO: VS rename move somwhere
namespace Satori
{
    // page granularity is 1 Gb, but they can be bigger
    // (on 32 bit we will have smaller sizes)
    static const int PAGE_BITS = 30;
    static const size_t PAGE_SIZE_GRANULARITY = (size_t)1 << PAGE_BITS;

    // regions are aligned at 2 Mb
    // (TODO: VS our commit unit must be powerof 2. must be <= 2 Mb?, otherwise large mode?)
    // objects can be larger than that and straddle multiple region tiles.
    // the additional "tail" tiles cannot have object starts though (makes finding region for obj easier).
    const static int REGION_BITS = 21;
    const static size_t REGION_SIZE_GRANULARITY = 1 << REGION_BITS;

    const static int INDEX_GRANULARITY = 4096;
    const static int INDEX_ITEMS = REGION_SIZE_GRANULARITY / INDEX_GRANULARITY;

    static const int BUCKET_COUNT = PAGE_BITS - REGION_BITS;

    // objects smaller than this go into regular region. A random big number.
    static const int LARGE_OBJECT_THRESHOLD = 85000;

    // TODO: VS consider: in theory could be 2, but that would complicate walking.
    static const size_t MIN_FREE_SIZE = 3 * sizeof(size_t);

    static const size_t MIN_REGULAR_ALLOC = 1 << 12;
}

class SatoriUtil
{
private:
    static MethodTable* s_emptyObjectMt;

public:
    static void Initialize();

    //TODO: VS Free vs. Empty
    static void MakeFreeObject(Object* obj, size_t size);

    static size_t RoundDownPwr2(size_t value)
    {
        _ASSERTE(value > 0);
        DWORD highestBit;
#ifdef HOST_64BIT
        BitScanReverse64(&highestBit, value);
#else
        BitScanReverse(&highestBit, value);
#endif
        return (size_t)1 << highestBit;
    }

    static size_t Size(Object* obj)
    {
        MethodTable* mt = obj->RawGetMethodTable();

        size_t size = mt->GetBaseSize();
        if (mt->HasComponentSize())
        {
            size += (size_t)((ArrayBase*)mt)->GetNumComponents() * mt->RawGetComponentSize();
        }

        return size;
    }

    static Object* Next(Object* obj)
    {
        return (Object*)((size_t)obj + Size(obj));
    }

    static bool SatoriUtil::IsFreeObject(Object* obj)
    {
        return obj->RawGetMethodTable() == s_emptyObjectMt;
    }
};

#endif
