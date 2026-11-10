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
//   SATORI_TEST_PAUSE_TRIMMER_DRAIN_MS=n
//                                   the trimmer pauses for n ms in its drain of the regions, so that GCs start in the middle of it
//   SATORI_TEST_SELF=containing     (conservative, DOTNET_gcTrim=0) a stale interior pointer into a free region, as the ETW heap walk
//                                   of a conservative stack gives it to GetContainingObject
//   SATORI_TEST_SELF=locktimeout    the timed enter of SatoriLock: a timeout, and the handoff of a wake that a waiter held when it gave up
//   SATORI_TEST_PAGE_ADD_LOCK_TIMEOUT_MS=n
//                                   the timeout of the page-add lock (6 ms otherwise); 0 adds without the lock whenever it is held
//
// The self-tests of the commit failures run in the precise mode, with DOTNET_gcRegionCacheCap=0 for the ones that need the freed regions
// to be free granules, and without it for the ones that need a region in the cache:
//   SATORI_TEST_SELF=warmlow,warmrun,racetrim,racetrim-none   (the fallback to a region that is committed in full)
//   SATORI_TEST_SELF=cacheexit1,cacheexit2,cacheexit3         (the fallback to the cache)
namespace SatoriTestHooks
{
    // Reads the environment. Called once, when the allocator is initialized.
    void Init();

    extern bool s_checkReformat;
    extern bool s_assertFreeOutsideDrain;
    extern int s_pauseFreeMs;
    extern int s_pauseTrimmerMs;
    extern int s_pauseTrimmerDrainMs;
    // The timeout of the page-add lock in ms, instead of the 6 ms of Release builds, when not -1. 0 gives up at once when the lock is held,
    // so that the add without the lock runs whenever adds contend. (SATORI_TEST_PAGE_ADD_LOCK_TIMEOUT_MS)
    extern int s_pageAddLockTimeoutMs;

    // The commits that a region needs when it is formatted fail: the next n of them when positive, all of them when -1. (see FailFormatCommit)
    extern volatile int32_t s_failFormatCommits;
    // The same for the commits that a page add needs.
    extern volatile int32_t s_failPageCommits;
    bool FailFormatCommit();
    bool FailPageCommit();

    // The thread finds no free granule in the pages that it has, so a request has to add a page. (see SatoriAllocator::TryClaimRegion)
    extern thread_local bool t_claimMiss;
    // The thread is held once, after it has found a warm granule and before it claims it. (see SatoriPage::TryClaimGranule)
    extern thread_local bool t_pauseWarmClaim;
    void OnWarmGranuleFound(SatoriPage* page, size_t granule);

    // The marker of a self-test is held between its map lookup and its header reads.
    extern thread_local bool t_holdMarker;
    // The thread is draining the regions that were parked, see SatoriAllocator::DrainParkedRegions
    extern thread_local bool t_inParkDrain;

    // A timed enter of a SatoriLock by this thread gives up right after its wait is woken, as if its time were up then,
    // and makes no attempt to take the lock after that. (see SatoriLock::EnterSlow, and the self-test locktimeout)
    extern thread_local bool t_lockGiveUpAfterWake;

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

    // The trimmer takes a granule for its age, which is the time since the first look that saw it free.
    // Asserts that it is at least the minimum free age (the clock is in whole seconds, one is allowed for that), and keeps the range.
    // A self-test that makes a granule old without waiting sets t_skipAgeCheck.
    void OnTrimmedByAge(uint32_t ageSeconds);
    extern thread_local bool t_skipAgeCheck;
    extern uint32_t s_trimAgeMin;
    extern uint32_t s_trimAgeMax;
    extern size_t s_trimAgeCount;

    // The trimmer is in its drain of the regions, between two regions. It holds no region.
    void OnTrimmerDrainRegion();

    // Runs the self-tests that the environment asks for, once.
    void RunSelfTests(SatoriHeap* heap);
}

#endif // _DEBUG

#endif // __SATORI_TEST_HOOKS_H__
