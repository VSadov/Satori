// Copyright (c) 2024 Vladimir Sadov
//
// Permission is hereby granted, free of charge, to any person
// obtaining a copy of this software and associated documentation
// files (the "Software"), to deal in the Software without
// restriction, including without limitation the rights to use,
// copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following
// conditions:
//
// The above copyright notice and this permission notice shall be
// included in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
// OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
// HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
// WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
// OTHER DEALINGS IN THE SOFTWARE.
//
// SatoriUtil.h
//

#ifndef __SATORI_UTIL_H__
#define __SATORI_UTIL_H__

#include "common.h"
#include "../gc.h"

namespace Satori
{
    class StackOnly {
    private:
        void* operator new(size_t size) throw() = delete;
        void* operator new[](size_t size) throw() = delete;
    };

    // page granularity is 1 Gb, but they can be bigger
    static const int PAGE_BITS = 30;
    static const size_t PAGE_SIZE_GRANULARITY = (size_t)1 << PAGE_BITS;

    // regions are aligned at 2 Mb
    // objects can be larger than that and straddle multiple region granules.
    // all real objects start in the first granule though to allow for fixed size of the metadata.
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
    // we use a trivial array object to fill holes, thus this is the size of a shortest array object.
    static const size_t MIN_FREE_SIZE = 3 * sizeof(size_t);

    // ~1024 items for now, we can fiddle with size a bit later
    const static size_t MARK_CHUNK_SIZE = 1024 * sizeof(size_t);

    // address bits set to track finalizable that needs to be scheduled to F-queue
    const static size_t FINALIZATION_PENDING = 1;

    static const int BYTES_PER_CARD_BYTE = 512;
    static const int CARD_BYTES_IN_CARD_GROUP = Satori::REGION_SIZE_GRANULARITY / BYTES_PER_CARD_BYTE;

    namespace CardState
    {
        static const int8_t EPHEMERAL = -128; // 0b10000000
        static const int8_t BLANK = 0;
        static const int8_t REMEMBERED = 1;
        static const int8_t PROCESSING = 2;
        static const int8_t DIRTY = 4;
    }

    // TUNING: this is just a threshold for cases when region has too much escaped content.
    //         The actual value may not matter a lot. Still may be worth revisiting.
    // When 1/4 escapes, we stop tracking escapes.
    static const int MAX_ESCAPE_SIZE = REGION_SIZE_GRANULARITY / 4;

    static const int MIN_FREELIST_SIZE_BITS = 12;
    static const size_t MIN_FREELIST_SIZE = 1 << MIN_FREELIST_SIZE_BITS;
    static const int FREELIST_COUNT = Satori::REGION_BITS - MIN_FREELIST_SIZE_BITS;

    // we will limit number of demoted objects to not use too many chunks
    // it will softly limit the occupancy as well.
    const static size_t MAX_DEMOTED_OBJECTS_IN_REGION = 2048;
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

    static void Prefetch(void* addr)
    {
#if TARGET_WINDOWS
#if defined(TARGET_ARM64)
        __prefetch(addr);
#else
        _mm_prefetch((char *) addr, _MM_HINT_T0);
#endif
#else
        __builtin_prefetch(addr);
#endif
    }

    static size_t CommitGranularity()
    {
        // we can support sizes that are > OS page and binary fractions of REGION_SIZE_GRANULARITY.
        // we can also support PAGE_SIZE_GRANULARITY
        size_t result = 1024 * 32;

        // result = Satori::REGION_SIZE_GRANULARITY;

        // result = Satori::PAGE_SIZE_GRANULARITY;

#if defined(TARGET_LINUX) && defined(TARGET_ARM64)
        result = max(result, GCToOSInterface::GetPageSize());
#endif

        return result;
    }

    // TUNING: Needs tuning?
    // When doing regular allocation we clean this much memory
    // if we do cleaning, and if available.
    // Set to 1/2 of a typical 32K L1 cache for now
    static const size_t MinZeroInitSize()
    {
        return 16 * 1024;
    }

    // COMPlus_gcConservative
    static bool IsConservativeMode()
    {
        return (GCConfig::GetConservativeGC());
    }

    // COMPlus_gcConcurrent
    static bool IsConcurrent()
    {
        return (GCConfig::GetConcurrentGC());
    }

    // COMPlus_gcRelocatingGen1
    static bool IsRelocatingInGen1()
    {
        return (GCConfig::GetRelocatingInGen1());
    }

    // COMPlus_gcRelocatingGen2
    static bool IsRelocatingInGen2()
    {
        return (GCConfig::GetRelocatingInGen2());
    }

    // COMPlus_gcThreadLocal
    static bool IsThreadLocalGCEnabled()
    {
        return (GCConfig::GetThreadLocalGC());
    }

    // COMPlus_gcTrim
    static bool IsTrimmingEnabled()
    {
        return (GCConfig::GetTrimmigGC());
    }

    // COMPlus_GCLatencyMode
    static bool IsLowLatencyMode()
    {
        return (GCConfig::GetLatencyMode()) >= 2;
    }

    static int HandlePartitionsCount()
    {
        int partitionCount = (int)GCConfig::GetHeapCount();
        if (partitionCount < 1)
        {
            partitionCount = GCToOSInterface::GetTotalProcessorCount();
        }

        return partitionCount;
    }

    // COMPlus_gcParallel
    static int MaxHelpersCount()
    {
        return (int)GCConfig::GetParallelGC();
    }
};

#endif
