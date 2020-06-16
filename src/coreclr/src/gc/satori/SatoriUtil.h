// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.
//
// SatoriUtil.h
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

    // object starts are aligned to this
    static const size_t OBJECT_ALIGNMENT = sizeof(size_t);

    // in theory min size could be 2 heap words, but that would complicate walking.
    static const size_t MIN_FREE_SIZE = 3 * sizeof(size_t);

    // when doing rgular allocation we clean this much memory
    // if we do cleaning, and if available
    static const size_t MIN_REGULAR_ALLOC = 1 << 12;
}

class SatoriUtil
{
public:
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

};

#endif
