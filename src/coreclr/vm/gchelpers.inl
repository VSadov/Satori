// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

/*
* GCHELPERS.INL
*
* GC Allocation and Write Barrier Helpers
*
*
*/

#ifndef _GCHELPERS_INL_
#define _GCHELPERS_INL_

//========================================================================
//
//      WRITE BARRIER HELPERS
//
//========================================================================

#if defined(HOST_64BIT)
static const int card_byte_shift = 11;
static const int card_bundle_byte_shift = 21;
#else
static const int card_byte_shift = 10;

#ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
#error Manually managed card bundles are currently only implemented for AMD64.
#endif
#endif

const static int REGION_BITS = 21;
const static size_t REGION_SIZE_GRANULARITY = 1 << REGION_BITS;

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
#if FEATURE_SATORI_GC
    // Satori uses g_card_bundle_table to publish the page byte map - the same map that
    // the write barriers use to check if a location is in the heap.
    // (see: SatoriHeap::IsInHeap and the "check if dst is in heap" parts of the barriers)

    // must match Satori::PAGE_BITS, same as the shift that barriers use.
    const int SATORI_PAGE_BITS = 30;

    // one byte per page (1Gb), nonzero if the page is a part of the heap.
    // NB: the map is a fixed-size array inside the heap instance. Satori publishes it
    //     once, at init, and never moves or reallocates it, thus an ordinary read.
    //     (the later StompResize only re-publishes the highest address)
    uint8_t* pageByteMap = (uint8_t*)g_card_bundle_table;
    return pageByteMap[(size_t)address >> SATORI_PAGE_BITS] != 0;
#else
    return (BYTE*)address >= g_lowest_address && (BYTE*)address < g_highest_address;
#endif
}

#if !FEATURE_SATORI_GC
FORCEINLINE void InlinedSetCardsAfterBulkCopyHelper(Object** start, size_t len)
{
    // Check whether the writes were even into the heap. If not there's no card update required.
    // Also if the size is smaller than a pointer, no write barrier is required.
    _ASSERTE(len >= sizeof(uintptr_t));
    if ((BYTE*)start < g_lowest_address || (BYTE*)start > g_highest_address)
    {
        return;
    }
    
    // Don't optimize the Generation 0 case if we are checking for write barrier violations
    // since we need to update the shadow heap even in the generation 0 case.
#if defined (WRITE_BARRIER_CHECK) && !defined (SERVER_GC)
    if (g_pConfig->GetHeapVerifyLevel() & EEConfig::HEAPVERIFY_BARRIERCHECK)
    {
        for(unsigned i=0; i < len / sizeof(Object*); i++)
        {
            updateGCShadow(&start[i], start[i]);
        }
    }
#endif //WRITE_BARRIER_CHECK && !SERVER_GC
    
#ifdef FEATURE_USE_SOFTWARE_WRITE_WATCH_FOR_GC_HEAP
    if (GCHeapUtilities::SoftwareWriteWatchIsEnabled())
    {
        GCHeapUtilities::SoftwareWriteWatchSetDirtyRegion(start, len);
    }
#endif // FEATURE_USE_SOFTWARE_WRITE_WATCH_FOR_GC_HEAP
    
    size_t startAddress = (size_t)start;
    size_t endAddress = startAddress + len;
    size_t startingClump = startAddress >> card_byte_shift;
    size_t endingClump = (endAddress + (1 << card_byte_shift) - 1) >> card_byte_shift;
    
    // calculate the number of clumps to mark (round_up(end) - start)
    size_t clumpCount = endingClump - startingClump;
    
    // VolatileLoadWithoutBarrier() is used here to prevent fetch of g_card_table from being reordered
    // with g_lowest/highest_address check above. See comment in StompWriteBarrier.
    BYTE* card = (BYTE*)VolatileLoadWithoutBarrier(&g_card_table) + startingClump;
    
    // Fill the cards. To avoid cache line thrashing we check whether the cards have already been set before
    // writing.
    do
    {
        if (*card != 0xff)
        {
            *card = 0xff;
        }
    
        card++;
        clumpCount--;
    }
    while (clumpCount != 0);
    
#ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
    size_t startBundleByte = startAddress >> card_bundle_byte_shift;
    size_t endBundleByte = (endAddress + (1 << card_bundle_byte_shift) - 1) >> card_bundle_byte_shift;
    size_t bundleByteCount = endBundleByte - startBundleByte;
    
    uint8_t* pBundleByte = ((uint8_t*)VolatileLoadWithoutBarrier(&g_card_bundle_table)) + startBundleByte;
    
    do
    {
        if (*pBundleByte != 0xFF)
        {
            *pBundleByte = 0xFF;
        }
    
        pBundleByte++;
        bundleByteCount--;
    }
    while (bundleByteCount != 0);
#endif
}
#endif // !FEATURE_SATORI_GC

#endif // !_GCHELPERS_INL_
