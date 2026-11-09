// Copyright (c) 2025 Vladimir Sadov
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
// SatoriTestHooks.h
//

#ifndef __SATORI_TEST_HOOKS_H__
#define __SATORI_TEST_HOOKS_H__

#include "common.h"
#include "gcenv.h"

#if _DEBUG

class SatoriHeap;
class SatoriPage;
class SatoriRegion;

// TEST-HOOK
// Injection points for the tests of Checked builds, and the self-tests that use them. Every hook is off, and costs
// the test of a flag, unless the test process sets its environment variable. Release builds have none of this.
//
//   SATORI_TEST_SELF=reuse,absorb   runs the self-tests at the first GC of the process (see RunSelfTests)
//   SATORI_TEST_REFORMAT_ASSERT=1   a conservative marker that passes its revalidation asserts that the granule
//                                   of the region that it found was not formatted again since it looked the region up
//   SATORI_TEST_ASSERT_FREE_OUTSIDE_DRAIN=1
//                                   in the conservative mode FreeRegion may run only in the drain of the parked regions
//   SATORI_TEST_PAUSE_FREE_MS=n     a free of a region into the bitmap pauses for n ms before it starts
//   SATORI_TEST_PAUSE_TRIMMER_MS=n  the trimmer pauses for n ms after it claims a run of granules and before it publishes the run again
namespace SatoriTestHooks
{
    // Reads the environment. Called once, when the allocator is initialized.
    void Init();

    extern bool s_checkReformat;
    extern bool s_assertFreeOutsideDrain;
    extern int s_pauseFreeMs;
    extern int s_pauseTrimmerMs;

    // The marker of a self-test is held between its map lookup and its header reads.
    extern thread_local bool t_holdMarker;
    // The thread is draining the regions that were parked, see SatoriAllocator::DrainParkedRegions
    extern thread_local bool t_inParkDrain;

    // What a conservative marker knows from before its reads. (see BeginMarkerLookup)
    struct MarkerLookup
    {
        SatoriPage* page;
        size_t granule;
        uint32_t count;
        bool active;
    };

    // The marker has found the region in the region map, and is about to read its header.
    // Returns true if the marker must stop here.
    bool BeginMarkerLookup(MarkerLookup& lookup, SatoriHeap* heap, SatoriRegion* region, size_t location);

    // The marker has passed its revalidation. Returns true if the marker must stop here.
    bool EndMarkerLookup(const MarkerLookup& lookup);

    // A region is about to be freed into the bitmap.
    void OnFreeToBitmap();

    // FreeRegion starts.
    void OnFreeRegion();

    // The trimmer has claimed a run of granules and is about to publish it again.
    void OnTrimRunClaimed();

    // Runs the self-tests that the environment asks for, once.
    void RunSelfTests(SatoriHeap* heap);
}

#endif // _DEBUG

#endif // __SATORI_TEST_HOOKS_H__
