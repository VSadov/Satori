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


//TODO: VS rename move somewhere
namespace Satori
{
    // page granularity is 1 Gb, but they can be bigger
    // (on 32 bit we will have smaller sizes)
    static const int PAGE_BITS = 30;
    static const size_t PAGE_SIZE_GRANULARITY = (size_t)1 << PAGE_BITS;

    // regions are aligned at 2 Mb
    // objects can be larger than that and straddle multiple region "tiles".
    // all real objects start in the first tile though to allow for fixed size of the metadata.
    const static int REGION_BITS = 21;
    const static size_t REGION_SIZE_GRANULARITY = 1 << REGION_BITS;

    const static int INDEX_GRANULARITY = 4096;
    const static int INDEX_ITEMS = REGION_SIZE_GRANULARITY / INDEX_GRANULARITY;

    static const int BUCKET_COUNT = PAGE_BITS - REGION_BITS;

    // objects smaller than this go into regular region. A random big number.
    static const int LARGE_OBJECT_THRESHOLD = 85000;

    // object starts are aligned to this
    static const size_t OBJECT_ALIGNMENT = sizeof(size_t);

    // minimal free size that can be made parseable.
    // we use a trivial array object to fill holes, thus this is the size of an array object.
    static const size_t MIN_FREE_SIZE = 3 * sizeof(size_t);

    // when doing regular allocation we clean this much memory
    // if we do cleaning, and if available
    // TODO: VS should this be a constant or be 1/2 L0 ?
    static const size_t MIN_REGULAR_ALLOC = 16 << 10;

    // 8K for now, we can fiddle with size a bit later
    const static size_t MARK_CHUNK_SIZE = 8 * 1024;

    // address bits set to track finalizable that needs to be scheduled to F-queue
    const static size_t FINALIZATION_PENDING = 1;

    // TODO: VS move to something like SatoriConfig, MIN_REGULAR_ALLOC too. These are not constants.
    static size_t CommitGranularity()
    {
        // we can support sizes that are binary fractions of REGION_SIZE_GRANULARITY.
        // we can also support 1G
        // TODO: VS this can be configured or computed at start up, but cannot change dynamically.
        return 4096;
    }
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

    static size_t GetCurrentThreadTag()
    {
        // must match what is used in barriers.
        // we use linear address of TEB on NT
        return (size_t)__readgsqword(0x30);
    }
};

#endif
