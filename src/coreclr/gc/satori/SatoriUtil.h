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
    class StackOnly {
    private:
        void* operator new(size_t size) throw() = delete;
        void* operator new[](size_t size) throw() = delete;
    };

    // page granularity is 1 Gb, but they can be bigger
    // (on 32 bit we will have smaller sizes)
    static const int PAGE_BITS = 30;
    static const size_t PAGE_SIZE_GRANULARITY = (size_t)1 << PAGE_BITS;

    // regions are aligned at 2 Mb
    // objects can be larger than that and straddle multiple region "tiles".
    // all real objects start in the first tile though to allow for fixed size of the metadata.
    const static int REGION_BITS = 21;
    const static size_t REGION_SIZE_GRANULARITY = 1 << REGION_BITS;

    const static int INDEX_GRANULARITY_BITS = 11;
    const static int INDEX_GRANULARITY = 1 << INDEX_GRANULARITY_BITS;
    const static int INDEX_LENGTH = REGION_SIZE_GRANULARITY / INDEX_GRANULARITY;

    static const int ALLOCATOR_BUCKET_COUNT = PAGE_BITS - REGION_BITS;

    // objects smaller than this go into regular region. 32K - to fit in bucket 3 and above
    static const int LARGE_OBJECT_THRESHOLD = 32 * 1024;

    // object starts are aligned to this
    static const size_t OBJECT_ALIGNMENT = sizeof(size_t);

    // minimal free size that can be made parseable.
    // we use a trivial array object to fill holes, thus this is the size of an array object.
    static const size_t MIN_FREE_SIZE = 3 * sizeof(size_t);

    // TUNING: Needs tuning?
    // When doing regular allocation we clean this much memory
    // if we do cleaning, and if available.
    // Set to 1/2 of a typical 32K L1 cache.
    static const size_t MIN_REGULAR_ALLOC = 16 * 1024;

    // 8K for now, we can fiddle with size a bit later
    const static size_t MARK_CHUNK_SIZE = 8 * 1024;

    // address bits set to track finalizable that needs to be scheduled to F-queue
    const static size_t FINALIZATION_PENDING = 1;

    // TODO: VS move to something like SatoriConfig, MIN_REGULAR_ALLOC too. These are not constants.
    static size_t CommitGranularity()
    {
        // we can support sizes that are > OS page and binary fractions of REGION_SIZE_GRANULARITY.
        // we can also support 1G
        // TODO: VS this should be configured or computed at start up, but should not change dynamically.
        return 4096 * 4;
    }

    static const int BYTES_PER_CARD_BYTE = 512;
    static const int CARD_BYTES_IN_CARD_GROUP = Satori::REGION_SIZE_GRANULARITY / BYTES_PER_CARD_BYTE;

    namespace CardState
    {
        static const int8_t BLANK = 0;
        static const int8_t REMEMBERED = 1;
        static const int8_t PROCESSING = 2;
        static const int8_t DIRTY = 3;
    }

    // TUNING: this is just a threshold for cases when region has too much escaped content.
    //         The actual value may not matter a lot. Still may be worth revisiting.
    // When 1/4 escapes, we stop tracking escapes.
    static const int MAX_ESCAPE_SIZE = REGION_SIZE_GRANULARITY / 4;

    static const int MIN_FREELIST_SIZE_BITS = 12;
    static const size_t MIN_FREELIST_SIZE = 1 << MIN_FREELIST_SIZE_BITS;
    static const int FREELIST_COUNT = Satori::REGION_BITS - MIN_FREELIST_SIZE_BITS;
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

    // must match what is used in barriers.
    static size_t GetCurrentThreadTag()
    {
#if TARGET_WINDOWS
        // we use linear address of TEB on NT
#if defined(TARGET_ARM64)
        return (size_t)__getReg(18);
#else
        return (size_t)__readgsqword(0x30);
#endif

#elif defined(TARGET_OSX)

        size_t tag;
#if defined(TARGET_ARM64)
        __asm__ ("mrs %0, tpidrro_el0" : "=r" (tag));
        tag &= (size_t)~7;
#else
        __asm__ ("movq %%gs:0, %0" : "=r" (tag));
#endif
        return tag;

#elif defined (TARGET_LINUX)

        size_t tag;
#if defined(TARGET_ARM64)
        __asm__ ("mrs %0, tpidr_el0" : "=r" (tag));
#else
        __asm__ ("movq %%fs:0, %0" : "=r" (tag));
#endif
        return tag;
#else
        UNSUPPORTED_PLATFORM
#endif
    }

    static bool IsConservativeMode()
    {
#ifdef FEATURE_CONSERVATIVE_GC
        return (GCConfig::GetConservativeGC());
#else
        return false;
#endif
    }
};

#endif
