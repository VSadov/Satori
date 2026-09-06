// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

//
// Unmanaged GC memory helpers
//

#include "common.h"
#include "gcenv.h"
#include "gcheaputilities.h"
#include "PalLimitedContext.h"
#include "CommonMacros.inl"
#include "GCMemoryHelpers.inl"

// This function clears a piece of memory in a GC safe way.
// Object-aligned memory is zeroed with no smaller than pointer-size granularity.
// We must make this guarantee whenever we clear memory in the GC heap that could contain object
// references.  The GC or other user threads can read object references at any time, clearing them bytewise can result
// in a read on another thread getting incorrect data.
// Unaligned memory at the beginning and remaining bytes at the end are written bytewise.
// USAGE:  The caller is responsible for null-checking the reference.
FCIMPL2(void *, RhpGcSafeZeroMemory, void * mem, size_t size)
{
    // The caller must do the null-check because we cannot take an AV in the runtime and translate it to managed.
    ASSERT(mem != nullptr);

    InlineGcSafeZeroMemory(mem, size);

    // memset returns the destination buffer
    return mem;
}
FCIMPLEND

#if defined(TARGET_X86) || defined(TARGET_AMD64)
    //
    // Memory writes are already ordered
    //
    #define GCHeapMemoryBarrier()
#else
    #define GCHeapMemoryBarrier() MemoryBarrier()
#endif

// Checks if the address may belong to the GC heap, without calling into the GC.
//
// "false" reliably means "not in the heap", "true" may be a false positive.
// NB: the two implementations differ in precision.
//     The segmented check is a [lowest, highest) range test - the range may contain
//     gaps that do not belong to the heap.
//     The Satori check is an exact page map lookup - Satori pages are reservation
//     units that are never shared with native/stack allocations.
FORCEINLINE bool IsPossiblyInHeap(void* address)
{
#ifdef FEATURE_SATORI_GC
    // Satori uses g_card_bundle_table to publish the page byte map - the same map that
    // the write barriers use to check if a location is in the heap.
    // (see: SatoriHeap::IsInHeap and the "check if dst is in heap" parts of the barriers)

    // must match Satori::PAGE_BITS, same as the shift that barriers use.
    const int SATORI_PAGE_BITS = 30;

    // one byte per page (1Gb), nonzero if the page is a part of the heap.
    uint8_t* pageByteMap = (uint8_t*)VolatileLoadWithoutBarrier(&g_card_bundle_table);
    return pageByteMap[(size_t)address >> SATORI_PAGE_BITS] != 0;
#else
    return (uint8_t*)address >= g_lowest_address && (uint8_t*)address < g_highest_address;
#endif
}

// Move memory, in a way that is compatible with a move onto the heap, but
// does not require the destination pointer to be on the heap.

FCIMPL3(void, RhBulkMoveWithWriteBarrier, uint8_t* pDest, uint8_t* pSrc, size_t cbDest)
{
    if (cbDest == 0 || pDest == pSrc)
        return;

    const bool inHeap = IsPossiblyInHeap(pDest);

#ifdef FEATURE_SATORI_GC
    if (inHeap)
    {
        GCHeapUtilities::GetGCHeap()->BulkMoveWithWriteBarrier(pDest, pSrc, cbDest);
        return;
    }

    // The destination is not in the heap - most likely the stack.
    // Nothing can be published this way, so there is no need for escape tracking,
    // ordering or cards. Just copy.
    // NB: the source may still be shared, so the copy must not tear references.
    if (pDest <= pSrc || pSrc + cbDest <= pDest)
        InlineForwardGCSafeCopy(pDest, pSrc, cbDest);
    else
        InlineBackwardGCSafeCopy(pDest, pSrc, cbDest);
#else
    if (inHeap)
    {
        // It is possible that the bulk write is publishing object references accessible so far only
        // by the current thread to shared memory.
        // The memory model requires that writes performed by current thread are observable no later
        // than the writes that will actually publish the references.
        GCHeapMemoryBarrier();
    }

    if (pDest <= pSrc || pSrc + cbDest <= pDest)
        InlineForwardGCSafeCopy(pDest, pSrc, cbDest);
    else
        InlineBackwardGCSafeCopy(pDest, pSrc, cbDest);

    if (inHeap)
    {
        InlinedBulkWriteBarrier(pDest, cbDest);
    }
#endif //FEATURE_SATORI_GC
}
FCIMPLEND
