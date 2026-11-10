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
// SatoriTestHooks.inl
//
// TEST-HOOK: the implementation of the hooks and the self-tests of Checked builds. It is included once, at the end of SatoriGC.cpp,
// so that a new source file does not have to be added to the build of each of the runtimes that have the GC.
//

#include "common.h"

#include "gcenv.h"
#include "../env/gcenv.os.h"

#include "SatoriUtil.h"
#include "SatoriTestHooks.h"

#if _DEBUG

#include "SatoriHeap.h"
#include "SatoriPage.h"
#include "SatoriPage.inl"
#include "SatoriRegion.h"
#include "SatoriRegion.inl"
#include "SatoriAllocator.h"
#include "SatoriRecycler.h"
#include <minipal/log.h>

// TEST-HOOK
namespace SatoriTestHooks
{
    bool s_checkReformat;
    bool s_assertFreeOutsideDrain;
    int s_pauseFreeMs;
    int s_pauseTrimmerMs;
    int s_pauseTrimmerDrainMs;
    int s_pageAddLockTimeoutMs = -1;
    thread_local bool t_holdMarker;
    thread_local bool t_inParkDrain;
    thread_local bool t_lockGiveUpAfterWake;

    // how long a marker of a self-test waits for the test to let it go
    static int s_holdMarkerMs = 5000;
    static char s_selfTests[128];

    // The state that the marker of a self-test and the test share.
    static volatile bool s_markerHeld;
    static volatile bool s_releaseMarker;
    static volatile bool s_markerDone;
    static volatile bool s_lookupActive;
    static volatile bool s_reformatSeen;
    static volatile bool s_headerDecommitted;
    static SatoriHeap* s_heap;
    static size_t s_markerLocation;

    static int EnvInt(const char* name)
    {
        char buf[16];
        DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
        return (n == 0 || n >= sizeof(buf)) ? 0 : atoi(buf);
    }

    void Init()
    {
        s_checkReformat = EnvInt("SATORI_TEST_REFORMAT_ASSERT") != 0;
        s_assertFreeOutsideDrain = EnvInt("SATORI_TEST_ASSERT_FREE_OUTSIDE_DRAIN") != 0;
        s_pauseFreeMs = EnvInt("SATORI_TEST_PAUSE_FREE_MS");
        s_pauseTrimmerMs = EnvInt("SATORI_TEST_PAUSE_TRIMMER_MS");
        s_pauseTrimmerDrainMs = EnvInt("SATORI_TEST_PAUSE_TRIMMER_DRAIN_MS");
        char timeoutBuf[16];
        DWORD timeoutLen = GetEnvironmentVariableA("SATORI_TEST_PAGE_ADD_LOCK_TIMEOUT_MS", timeoutBuf, sizeof(timeoutBuf));
        s_pageAddLockTimeoutMs = (timeoutLen == 0 || timeoutLen >= sizeof(timeoutBuf)) ? -1 : atoi(timeoutBuf);
        int holdMs = EnvInt("SATORI_TEST_HOLD_MARKER_MS");
        if (holdMs > 0)
        {
            s_holdMarkerMs = holdMs;
        }

        DWORD n = GetEnvironmentVariableA("SATORI_TEST_SELF", s_selfTests, sizeof(s_selfTests));
        if (n == 0 || n >= sizeof(s_selfTests))
        {
            s_selfTests[0] = 0;
        }
    }

    static bool WaitUntil(volatile bool& flag, int timeoutMs)
    {
        int64_t deadline = SatoriUtil::GetTimeStamp() + (int64_t)timeoutMs * SatoriUtil::GetTimeStampFrequency() / 1000;
        while (!flag)
        {
            if (SatoriUtil::GetTimeStamp() > deadline)
            {
                return false;
            }

            GCToOSInterface::YieldThread(0);
        }

        return true;
    }

    static bool IsCommitted(void* address)
    {
#ifdef _WIN32
        MEMORY_BASIC_INFORMATION info;
        return VirtualQuery(address, &info, sizeof(info)) != 0 && info.State == MEM_COMMIT;
#else
        return true;
#endif
    }

    bool BeginMarkerLookup(MarkerLookup& lookup, SatoriHeap* heap, SatoriRegion* region, size_t location)
    {
        lookup.active = false;
        if (!s_checkReformat && !t_holdMarker)
        {
            return false;
        }

        // The page and the granule come from addresses, nothing is read from the header that the marker has not read yet.
        SatoriPage* page = heap->PageForAddressChecked(location);
        if (page == nullptr || (size_t)region < page->Start())
        {
            return false;
        }

        size_t granule = ((size_t)region - page->Start()) >> Satori::REGION_BITS;
        if (granule >= page->GranuleCount())
        {
            return false;
        }

        lookup.page = page;
        lookup.granule = granule;
        lookup.count = page->FormatCount(granule);
        lookup.active = true;

        if (t_holdMarker)
        {
            s_lookupActive = true;
            s_markerHeld = true;
            WaitUntil(s_releaseMarker, s_holdMarkerMs);
            if (!IsCommitted(region))
            {
                // the marker is about to read the header of a region that was freed, absorbed into a run and decommitted
                s_headerDecommitted = true;
                return true;
            }
        }

        return false;
    }

    bool EndMarkerLookup(const MarkerLookup& lookup)
    {
        if (!lookup.active)
        {
            return false;
        }

        if (lookup.page->FormatCount(lookup.granule) != lookup.count)
        {
            // The region that the marker found was freed, and another one was formatted at its address.
            // What the marker has read may come from either of them, and the revalidation cannot tell, since the address is the same.
            if (t_holdMarker)
            {
                s_reformatSeen = true;
            }
            else
            {
                _ASSERTE(!"a conservative marker passed its revalidation, but the granule of its region was formatted again");
            }
        }

        return t_holdMarker;
    }

    void OnFreeToBitmap()
    {
        if (s_pauseFreeMs)
        {
            GCToOSInterface::Sleep(s_pauseFreeMs);
        }
    }

    void OnFreeRegion()
    {
        if (s_assertFreeOutsideDrain && SatoriUtil::IsConservativeMode() && !t_inParkDrain)
        {
            _ASSERTE(!"FreeRegion runs in the conservative mode outside the drain of the parked regions");
        }
    }

    void OnTrimRunClaimed()
    {
        if (s_pauseTrimmerMs)
        {
            GCToOSInterface::Sleep(s_pauseTrimmerMs);
        }
    }

    // Not the trimmer's own pause: a GC that asks the trimmer to stop does not cut it short.
    void OnTrimmerDrainRegion()
    {
        if (s_pauseTrimmerDrainMs)
        {
            GCToOSInterface::Sleep(s_pauseTrimmerDrainMs);
        }
    }

    thread_local bool t_skipAgeCheck;
    uint32_t s_trimAgeMin = UINT32_MAX;
    uint32_t s_trimAgeMax;
    size_t s_trimAgeCount;

    // Only the trimmer takes granules for their age, so the range needs no interlocked updates.
    void OnTrimmedByAge(uint32_t ageSeconds)
    {
        size_t minAge = SatoriUtil::TrimMinFreeAgeSeconds();
        if (t_skipAgeCheck || minAge == 0)
        {
            return;
        }

        if ((size_t)ageSeconds + 1 < minAge)
        {
            minipal_log_print_error("Satori: the trimmer took a granule that was seen free %u s ago, the minimum free age is %zu s\n", ageSeconds, minAge);
            _ASSERTE(!"the trimmer took a granule that is younger than the minimum free age");
        }

        s_trimAgeMin = min(s_trimAgeMin, ageSeconds);
        s_trimAgeMax = max(s_trimAgeMax, ageSeconds);
        s_trimAgeCount++;
    }

    static void MarkerThreadFn(void*)
    {
        t_holdMarker = true;
        s_heap->Recycler()->TestMarkConservativeConcurrent(s_markerLocation);
        s_markerDone = true;
    }

    // Starts a conservative marker that looks at the location, and waits until it is held after its map lookup.
    static bool StartMarker(size_t location)
    {
        s_markerHeld = false;
        s_releaseMarker = false;
        s_markerDone = false;
        s_lookupActive = false;
        s_reformatSeen = false;
        s_headerDecommitted = false;
        s_markerLocation = location;
        if (!GCToEEInterface::CreateThread(MarkerThreadFn, nullptr, false, "Satori Test Marker"))
        {
            return false;
        }

        return WaitUntil(s_markerHeld, 10000);
    }

    // Lets the held marker go, and waits until it is done.
    static bool FinishMarker()
    {
        s_releaseMarker = true;
        return WaitUntil(s_markerDone, 10000);
    }

    static void Report(const char* name, const char* verdict, const char* details)
    {
        minipal_log_print_error("SELFTEST %s: %s (%s)\n", name, verdict, details);
    }

    // The same address is used again.
    // A conservative marker looks up a region, and is held before it reads the header. The region is freed and the lowest free
    // granule is claimed again, which is the one that the region has just left. The region that is formatted there is published.
    // Then the marker goes on. The address is the same, so its revalidation passes, although it is not the region that it found.
    // With the freed regions parked, they cannot be claimed while a marker may be looking at them, and the address is not used again.
    static bool SelfTestReuse(SatoriHeap* heap)
    {
        SatoriAllocator* allocator = heap->Allocator();
        SatoriRegion* region = allocator->GetRegion(Satori::REGION_SIZE_GRANULARITY);
        if (region == nullptr || !StartMarker(region->Start() + Satori::REGION_SIZE_GRANULARITY / 2))
        {
            Report("reuse", "FAIL", "the test could not start its marker");
            return false;
        }

        size_t address = region->Start();
        allocator->AddRegion(region);

        SatoriRegion* kept[64];
        int keptCount = 0;
        SatoriRegion* again = nullptr;
        for (int i = 0; i < 64 && again == nullptr; i++)
        {
            SatoriRegion* next = allocator->GetRegion(Satori::REGION_SIZE_GRANULARITY);
            if (next == nullptr)
            {
                break;
            }

            if (next->Start() == address)
            {
                again = next;
            }
            else
            {
                kept[keptCount++] = next;
            }
        }

        if (again != nullptr)
        {
            // published, as it is when an allocator takes the region
            again->SetGenerationRelease(1);
        }

        bool finished = FinishMarker();

        char details[160];
        snprintf(details, sizeof(details), "the address was used again: %d, the marker passed its revalidation after the granule was formatted again: %d, marker finished: %d, marker looked up a region: %d",
            again != nullptr, (int)s_reformatSeen, finished, (int)s_lookupActive);

        bool pass = finished && s_lookupActive && !s_reformatSeen && again == nullptr;
        Report("reuse", pass ? "PASS" : "FAIL", details);

        // The region that was formatted again stays published and out of every queue: the test leaves it where it is.
        for (int i = 0; i < keptCount; i++)
        {
            allocator->AddRegion(kept[i]);
        }

        return pass;
    }

    // A freed region is absorbed into a run, and the run is decommitted.
    // Two regions next to each other are freed while a marker holds the second one. Then a run of three granules that starts
    // at the first one is claimed and formatted, and the part of it after its header is decommitted, which is what the finalization queue
    // does with the region that it gets when it grows. Then the marker goes on, and the header that it is about to read is
    // in the decommitted part. With the freed regions parked, the run cannot include them.
    static bool SelfTestAbsorb(SatoriHeap* heap)
    {
        SatoriAllocator* allocator = heap->Allocator();
        SatoriRegion* first = nullptr;
        SatoriRegion* second = nullptr;
        SatoriRegion* kept[32];
        int keptCount = 0;
        for (int i = 0; i < 16 && first == nullptr; i++)
        {
            SatoriRegion* a = allocator->GetRegion(Satori::REGION_SIZE_GRANULARITY);
            SatoriRegion* b = allocator->GetRegion(Satori::REGION_SIZE_GRANULARITY);
            if (a != nullptr && b != nullptr && b->Start() == a->End())
            {
                first = a;
                second = b;
            }
            else
            {
                if (a != nullptr)
                {
                    kept[keptCount++] = a;
                }

                if (b != nullptr)
                {
                    kept[keptCount++] = b;
                }
            }
        }

        if (first == nullptr || !StartMarker(second->Start() + Satori::REGION_SIZE_GRANULARITY / 2))
        {
            Report("absorb", "FAIL", "the test could not get two regions next to each other, or start its marker");
            return false;
        }

        SatoriPage* page = first->ContainingPage();
        size_t firstGranule = (first->Start() - page->Start()) >> Satori::REGION_BITS;
        allocator->AddRegion(first);
        allocator->AddRegion(second);

        // a run of three granules from the first one
        size_t got = 0;
        while (got < 3 && page->TryClaimGranuleAt(firstGranule + got))
        {
            got++;
        }

        SatoriRegion* run = nullptr;
        if (got == 3)
        {
            run = page->FormatClaimed(firstGranule, 3);
            if (run != nullptr)
            {
                run->TryDecommit();
            }
        }
        else if (got > 0)
        {
            page->PublishFreeGranules(firstGranule, got);
        }

        bool finished = FinishMarker();

        char details[160];
        snprintf(details, sizeof(details), "the run that covers the freed regions was claimed: %d, the marker found its header decommitted: %d, marker finished: %d, marker looked up a region: %d",
            got == 3, (int)s_headerDecommitted, finished, (int)s_lookupActive);

        bool pass = finished && s_lookupActive && !s_headerDecommitted && got != 3;
        Report("absorb", pass ? "PASS" : "FAIL", details);

        if (run != nullptr)
        {
            allocator->AddRegion(run);
        }

        for (int i = 0; i < keptCount; i++)
        {
            allocator->AddRegion(kept[i]);
        }

        return pass;
    }

    // ---- Step 3: the fallbacks when a commit fails ----

    // The claimer that a test holds after it has found a warm granule.
    static volatile bool s_claimerPaused;
    static volatile bool s_releaseClaimer;
    static volatile bool s_claimerDone;
    static SatoriPage* s_pausedPage;
    static size_t s_pausedGranule;
    static SatoriRegion* s_claimerResult;
    static int s_claimerRequestKind;

    volatile int32_t s_failFormatCommits;
    volatile int32_t s_failPageCommits;
    thread_local bool t_claimMiss;
    thread_local bool t_pauseWarmClaim;

    static bool ConsumeFailure(volatile int32_t& counter)
    {
        while (true)
        {
            int32_t n = counter;
            if (n == 0)
            {
                return false;
            }

            if (n < 0 || Interlocked::CompareExchange(&counter, n - 1, n) == n)
            {
                return true;
            }
        }
    }

    bool FailFormatCommit()
    {
        return ConsumeFailure(s_failFormatCommits);
    }

    bool FailPageCommit()
    {
        return ConsumeFailure(s_failPageCommits);
    }

    volatile int32_t s_failSliceCommits;

    bool FailSliceCommit()
    {
        return ConsumeFailure(s_failSliceCommits);
    }

    void OnWarmGranuleFound(SatoriPage* page, size_t granule)
    {
        if (!t_pauseWarmClaim)
        {
            return;
        }

        t_pauseWarmClaim = false;
        s_pausedPage = page;
        s_pausedGranule = granule;
        s_claimerPaused = true;
        WaitUntil(s_releaseClaimer, 10000);
    }

    // A thread that gets a region of the given kind: 0 - a long-lived region of one granule.
    static void ClaimerThreadFn(void*)
    {
        t_pauseWarmClaim = true;
        s_claimerResult = s_heap->Allocator()->GetRegionLow(Satori::REGION_SIZE_GRANULARITY);
        s_claimerDone = true;
    }

    static void ReportCommit(const char* name, bool pass, SatoriRegion* region, int32_t commitsLeft, const char* what)
    {
        char details[200];
        snprintf(details, sizeof(details), "%s; region %p; commits left of the failing ones: %d", what, (void*)region, (int)commitsLeft);
        Report(name, pass ? "PASS" : "FAIL", details);
    }

    // The warm region that a free of a region leaves: committed in full, and free.
    static SatoriRegion* FreeWarm(SatoriHeap* heap, size_t granules)
    {
        SatoriAllocator* allocator = heap->Allocator();
        SatoriRegion* region = allocator->GetRegion(granules << Satori::REGION_BITS);
        if (region != nullptr)
        {
            region->TryCommit();
            allocator->AddRegion(region);
        }

        return region;
    }

    // A request for a long-lived region claims the highest free granule, which is never used, and cannot be committed. A warm granule is free.
    // The request must get the warm one, and must not try to commit again.
    static bool SelfTestWarmLow(SatoriHeap* heap)
    {
        SatoriAllocator* allocator = heap->Allocator();
        SatoriRegion* warm = FreeWarm(heap, 1);
        if (warm == nullptr)
        {
            Report("warmlow", "FAIL", "the test could not get a region");
            return false;
        }

        size_t warmStart = warm->Start();
        s_failFormatCommits = 2;
        SatoriRegion* region = allocator->GetRegionLow(Satori::REGION_SIZE_GRANULARITY);
        int32_t left = s_failFormatCommits;
        s_failFormatCommits = 0;
        bool pass = region != nullptr && region->Start() == warmStart && left == 1;
        ReportCommit("warmlow", pass, region, left, pass ? "the request got the warm region" : "the request did not get the warm region");
        return pass;
    }

    // The same for a run: the lowest free run of two granules is cold (its first granule is trimmed, the second was never used),
    // and the run above it is warm.
    static bool SelfTestWarmRun(SatoriHeap* heap)
    {
        SatoriAllocator* allocator = heap->Allocator();
        SatoriRegion* cold = allocator->GetRegion(2 * Satori::REGION_SIZE_GRANULARITY);
        SatoriRegion* warm = cold != nullptr ? FreeWarm(heap, 2) : nullptr;
        if (cold == nullptr || warm == nullptr || warm->Start() != cold->End())
        {
            Report("warmrun", "FAIL", "the test could not get two runs next to each other");
            return false;
        }

        SatoriPage* page = cold->ContainingPage();
        size_t coldFirst = (cold->Start() - page->Start()) >> Satori::REGION_BITS;
        size_t warmStart = warm->Start();
        allocator->AddRegion(cold);
        size_t count;
        bool trimmed = page->TryTrimRun(coldFirst, 1, /* onlyIfSeenFree */ false, count);

        s_failFormatCommits = 2;
        SatoriRegion* region = allocator->GetRegion(2 * Satori::REGION_SIZE_GRANULARITY);
        int32_t left = s_failFormatCommits;
        s_failFormatCommits = 0;
        bool pass = trimmed && region != nullptr && region->Start() == warmStart && left == 1;
        ReportCommit("warmrun", pass, region, left, trimmed ? "the cold run was trimmed" : "the cold run could not be trimmed");
        return pass;
    }

    // The trimmer takes a granule between a claimer's look at its warm bit and its claim. The claimer that retries for a warm granule
    // gets the granule cold, and must give it back and go on. With a warm granule below it, that is the one to take. Without, the request fails.
    static bool SelfTestRaceTrim(SatoriHeap* heap, bool withOtherWarm)
    {
        const char* name = withOtherWarm ? "racetrim" : "racetrim-none";
        SatoriAllocator* allocator = heap->Allocator();
        // both regions are claimed before either is freed, otherwise the second claim takes the granule of the first one again
        SatoriRegion* lower = withOtherWarm ? allocator->GetRegion(Satori::REGION_SIZE_GRANULARITY) : nullptr;
        SatoriRegion* upper = allocator->GetRegion(Satori::REGION_SIZE_GRANULARITY);
        if (upper == nullptr || (withOtherWarm && lower == nullptr))
        {
            Report(name, "FAIL", "the test could not get a region");
            return false;
        }

        if (lower != nullptr)
        {
            lower->TryCommit();
            allocator->AddRegion(lower);
        }

        upper->TryCommit();
        allocator->AddRegion(upper);

        // the claimer takes the highest warm granule first
        _ASSERTE(lower == nullptr || lower->Start() < upper->Start());
        SatoriPage* page = upper->ContainingPage();
        size_t upperGranule = (upper->Start() - page->Start()) >> Satori::REGION_BITS;
        size_t lowerStart = lower != nullptr ? lower->Start() : 0;

        s_claimerPaused = false;
        s_releaseClaimer = false;
        s_claimerDone = false;
        s_claimerResult = nullptr;
        s_failFormatCommits = 2;
        if (!GCToEEInterface::CreateThread(ClaimerThreadFn, nullptr, false, "Satori Test Claimer"))
        {
            Report(name, "FAIL", "the test could not start its claimer");
            return false;
        }

        // the claimer finds the granule of the upper region warm, and is held. The trimmer takes the granule.
        bool paused = WaitUntil(s_claimerPaused, 5000);
        bool trimmed = false;
        if (paused)
        {
            size_t count;
            trimmed = page->TryTrimRun(upperGranule, 1, /* onlyIfSeenFree */ false, count);
        }

        s_releaseClaimer = true;
        bool done = WaitUntil(s_claimerDone, 10000);
        int32_t left = s_failFormatCommits;
        s_failFormatCommits = 0;

        bool right = withOtherWarm ? (s_claimerResult != nullptr && s_claimerResult->Start() == lowerStart) : (s_claimerResult == nullptr);
        bool pass = paused && trimmed && done && right && left == 1 && s_pausedGranule == upperGranule;
        char what[120];
        snprintf(what, sizeof(what), "the claimer was held on the upper granule: %d, the trimmer took it: %d, done: %d", (int)(paused && s_pausedGranule == upperGranule), (int)trimmed, (int)done);
        ReportCommit(name, pass, s_claimerResult, left, what);
        return pass;
    }

    // A request for a long-lived region fails in the three ways that the review found, and there is a region in the cache.
    // The request must get it. Then the cache count is right and the page-add lock is free.
    static bool SelfTestCacheExit(SatoriHeap* heap, int exit)
    {
        char name[16];
        snprintf(name, sizeof(name), "cacheexit%d", exit);
        SatoriAllocator* allocator = heap->Allocator();
        allocator->DrainRegionCache();
        SatoriRegion* cached = allocator->GetRegion(Satori::REGION_SIZE_GRANULARITY);
        if (cached == nullptr)
        {
            Report(name, "FAIL", "the test could not get a region");
            return false;
        }

        allocator->AddRegion(cached);
        if (!allocator->TestCacheCountIs(1))
        {
            Report(name, "FAIL", "the region did not go to the cache");
            return false;
        }

        switch (exit)
        {
        case 1:
            // the claim in the pages that we have fails to format
            s_failFormatCommits = -1;
            break;
        case 2:
            // nothing is free in the pages that we have, the added page is fine, and what is claimed in it fails to format
            t_claimMiss = true;
            s_failFormatCommits = -1;
            break;
        default:
            // nothing is free in the pages that we have, and the page cannot be added
            t_claimMiss = true;
            s_failPageCommits = -1;
            break;
        }

        SatoriRegion* region = allocator->GetRegionLow(Satori::REGION_SIZE_GRANULARITY);
        s_failFormatCommits = 0;
        s_failPageCommits = 0;
        t_claimMiss = false;

        bool got = region == cached;
        bool countRight = allocator->TestCacheCountIs(0);
        bool lockFree = allocator->TestPageAddLockIsFree();
        char what[120];
        snprintf(what, sizeof(what), "got the cached region: %d, the cache count is right: %d, the page-add lock is free: %d", (int)got, (int)countRight, (int)lockFree);
        ReportCommit(name, got && countRight && lockFree, region, 0, what);
        return got && countRight && lockFree;
    }

    // ---- Milestone 3, step 4: the card slices ----

    // A request has to add a page (t_claimMiss), and the commit of the card slices of what it claims in the new page fails.
    // The claim gives the granules back as they were, without their cards; the request ends as it does when a format commit fails
    // (out of memory without a fallback, and this test runs without a cache). The page-add lock is free after it, and the next request,
    // without the failure, gets a region. The GC that follows the self-tests (Checked) checks the granules and the committed counter.
    static bool SelfTestSliceFail(SatoriHeap* heap)
    {
        SatoriAllocator* allocator = heap->Allocator();
        t_claimMiss = true;
        s_failSliceCommits = 1;
        SatoriRegion* failed = allocator->GetRegionLow(Satori::REGION_SIZE_GRANULARITY);
        bool consumed = s_failSliceCommits == 0;
        s_failSliceCommits = 0;
        t_claimMiss = false;
        bool lockFree = allocator->TestPageAddLockIsFree();

        SatoriRegion* region = allocator->GetRegion(Satori::REGION_SIZE_GRANULARITY);
        bool next = region != nullptr;

        if (failed != nullptr)
        {
            allocator->AddRegion(failed);
        }

        if (region != nullptr)
        {
            allocator->AddRegion(region);
        }

        bool pass = consumed && lockFree && next;
        char details[200];
        snprintf(details, sizeof(details), "the slice commit failed: %d, the request got a region anyway: %d, the page-add lock is free: %d, the next request got a region: %d",
            (int)consumed, (int)(failed != nullptr), (int)lockFree, (int)next);
        Report("slicefail", pass ? "PASS" : "FAIL", details);
        return pass;
    }

    // ---- Step 5: the minimum free age ----

    // Gets a region of one granule, commits it in full and frees it (in the conservative mode, parks it).
    // Returns the page and the granule that the region had.
    static bool FreeOneWarmGranule(SatoriHeap* heap, SatoriPage*& page, size_t& granule)
    {
        SatoriAllocator* allocator = heap->Allocator();
        SatoriRegion* region = allocator->GetRegion(Satori::REGION_SIZE_GRANULARITY);
        if (region == nullptr)
        {
            return false;
        }

        region->TryCommit();
        page = region->ContainingPage();
        granule = (region->Start() - page->Start()) >> Satori::REGION_BITS;
        allocator->AddRegion(region);
        return true;
    }

    // The trimmer takes a free granule for its age only after a look of its has seen the granule free, and two rotations of the age
    // snapshots since then (see SatoriTrimmer::MaybeRotateAges). The look that first sees a granule free starts its age.
    // A trim on request (TrimNow) does not look at it. The rotations are made here without waiting, so the check of the age at the trim is off.
    static bool SelfTestAge(SatoriHeap* heap)
    {
        size_t minAge = SatoriUtil::TrimMinFreeAgeSeconds();
        SatoriPage* page;
        size_t granule;
        if (minAge == 0 || !FreeOneWarmGranule(heap, page, granule))
        {
            Report("age", "FAIL", minAge == 0 ? "the minimum free age is off, the test needs it on" : "the test could not get a region");
            return false;
        }

        t_skipAgeCheck = true;
        size_t count;

        // freed: no look has seen it, so the trimmer does not find it for its age
        bool freed = page->NextGranuleToTrim(granule, false) == granule && !page->IsSeenFree(granule) && page->NextGranuleToTrim(granule, true) != granule;

        // The look starts the age: the granule is in neither snapshot. It is too young: the search of the trimmer does not find it,
        // unless it asks for the work that is left, and the claim of the trimmer, if it got that far, gives the granule back.
        page->RememberFreeGranules();
        bool looked = page->IsSeenFree(granule) && !page->IsAging(granule) && !page->IsAged(granule);
        bool youngSkipped = page->NextGranuleToTrim(granule, true) != granule;
        bool youngIsWork = page->NextGranuleToTrim(granule, true, true) == granule;
        bool youngRefused = !page->TryTrimRun(granule, 1, true, count) && page->NextGranuleToTrim(granule, false) == granule;

        // one rotation: aging, still too young
        page->RotateAges();
        bool oneRotation = page->IsAging(granule) && !page->IsAged(granule) && page->NextGranuleToTrim(granule, true) != granule;

        // two rotations: aged, old enough, the trimmer takes it
        page->RotateAges();
        bool oldFound = page->IsAged(granule) && page->NextGranuleToTrim(granule, true) == granule;
        bool oldTaken = page->TryTrimRun(granule, 1, true, count) && page->NextGranuleToTrim(granule, false) != granule;

        // a trim on request takes a granule that is too young
        bool onRequest = false;
        SatoriPage* page2;
        size_t granule2;
        if (FreeOneWarmGranule(heap, page2, granule2))
        {
            page2->RememberFreeGranules();
            onRequest = page2->NextGranuleToTrim(granule2, true) != granule2 &&
                page2->TryTrimRun(granule2, 1, false, count) &&
                page2->NextGranuleToTrim(granule2, false) != granule2;
        }

        t_skipAgeCheck = false;
        bool pass = freed && looked && youngSkipped && youngIsWork && youngRefused && oneRotation && oldFound && oldTaken && onRequest;
        char details[300];
        snprintf(details, sizeof(details), "minimum age %d s; freed has no age: %d, the look starts it: %d, too young is skipped: %d, is work left: %d, is refused: %d, after one rotation still young: %d, after two old is found: %d, is taken: %d, a trim on request takes a young one: %d",
            (int)minAge, (int)freed, (int)looked, (int)youngSkipped, (int)youngIsWork, (int)youngRefused, (int)oneRotation, (int)oldFound, (int)oldTaken, (int)onRequest);
        Report("age", pass ? "PASS" : "FAIL", details);
        return pass;
    }

    // A parked region (the conservative mode) is not free, so no look sees it and it has no age. The drain frees it, and the first look
    // after that starts its age.
    static bool SelfTestAgeParked(SatoriHeap* heap)
    {
        SatoriPage* page;
        size_t granule;
        if (!FreeOneWarmGranule(heap, page, granule))
        {
            Report("ageparked", "FAIL", "the test could not get a region");
            return false;
        }

        bool parked = page->NextGranuleToTrim(granule, false) != granule;
        page->RememberFreeGranules();
        bool parkedNotSeen = !page->IsSeenFree(granule);

        heap->Allocator()->DrainParkedRegions();
        bool drainedFree = page->NextGranuleToTrim(granule, false) == granule;
        bool drainedNoAge = !page->IsSeenFree(granule);

        page->RememberFreeGranules();
        bool aging = page->IsSeenFree(granule) && !page->IsAging(granule) && !page->IsAged(granule);

        bool pass = parked && parkedNotSeen && drainedFree && drainedNoAge && aging;
        char details[200];
        snprintf(details, sizeof(details), "parked is not free: %d, a look does not see it: %d, the drain frees it: %d, with no age: %d, the next look starts the age: %d",
            (int)parked, (int)parkedNotSeen, (int)drainedFree, (int)drainedNoAge, (int)aging);
        Report("ageparked", pass ? "PASS" : "FAIL", details);
        return pass;
    }

    // A stale interior pointer into a region that is not in use (generation -1, still in the region map), as the ETW heap walk
    // with conservative stack reporting gives it to GetContainingObject. Such a region has no objects to look for: the answer is null.
    // The region is put in the cache, or parked if the cache is full: either way it keeps its region map bytes.
    static bool SelfTestContaining(SatoriHeap* heap)
    {
        SatoriAllocator* allocator = heap->Allocator();
        SatoriRegion* region = allocator->GetRegion(Satori::REGION_SIZE_GRANULARITY);
        if (region == nullptr)
        {
            Report("containing", "FAIL", "the test could not get a region");
            return false;
        }

        size_t location = region->Start() + Satori::REGION_SIZE_GRANULARITY / 2;
        allocator->AddRegion(region);
        bool inMap = heap->RegionForAddressChecked(location) == region && region->Generation() < 0;

        Object* any = g_theGCHeap->GetContainingObject((void*)location, /* fCollectedGenOnly */ false);
        Object* collected = g_theGCHeap->GetContainingObject((void*)location, /* fCollectedGenOnly */ true);

        bool pass = inMap && any == nullptr && collected == nullptr;
        char details[160];
        snprintf(details, sizeof(details), "the free region is in the map: %d, no object for any generation: %d, for the collected ones: %d",
            (int)inMap, (int)(any == nullptr), (int)(collected == nullptr));
        Report("containing", pass ? "PASS" : "FAIL", details);
        return pass;
    }

    // ---- Milestone 3, step 3: the timed enter of SatoriLock ----

    static SatoriLock* s_testLock;
    static volatile bool s_plainDone;
    static volatile bool s_plainGot;
    static volatile int64_t s_plainTicks;
    static volatile bool s_w1Done;
    static volatile bool s_w1Got;
    static volatile bool s_w2Done;
    static volatile int32_t s_lockFinished;
    static volatile int32_t s_lockFirst;

    static uint32_t TestLockWaiters()
    {
        return (s_testLock->TestState() & 0x7FFFFFFCu) >> 2;
    }

    static bool WaitForTestLockWaiters(uint32_t count)
    {
        int64_t deadline = SatoriUtil::GetTimeStamp() + 5 * SatoriUtil::GetTimeStampFrequency();
        while (TestLockWaiters() < count)
        {
            if (SatoriUtil::GetTimeStamp() > deadline)
            {
                return false;
            }

            GCToOSInterface::YieldThread(0);
        }

        return true;
    }

    static void LockPlainFn(void*)
    {
        int64_t start = SatoriUtil::GetTimeStamp();
        bool got = s_testLock->TryEnterFor(20);
        s_plainTicks = SatoriUtil::GetTimeStamp() - start;
        if (got)
        {
            s_testLock->Leave();
        }

        s_plainGot = got;
        s_plainDone = true;
    }

    // gives up right after its wake, holding it
    static void LockW1Fn(void*)
    {
        t_lockGiveUpAfterWake = true;
        bool got = s_testLock->TryEnterFor(10000);
        if (got)
        {
            s_testLock->Leave();
        }

        s_w1Got = got;
        Interlocked::CompareExchange(&s_lockFirst, 1, 0);
        s_w1Done = true;
    }

    // waits for as long as it takes: it has the lock only if somebody wakes it
    static void LockW2Fn(void*)
    {
        s_testLock->Enter();
        s_testLock->Leave();
        Interlocked::CompareExchange(&s_lockFirst, 2, 0);
        s_w2Done = true;
    }

    // The lock is held. A timed enter of 20 ms gives up, and leaves no waiter behind.
    // Then two waiters, W1 with a timed enter that gives up right after it is woken (holding the wake), and W2 with an enter
    // that waits for as long as it takes. The lock is left: whichever of them is woken, W2 must get the lock, W1 must report that it did not,
    // and the lock must be left free with no waiter and no wake outstanding. When W1 is woken first, only its handoff of the wake can wake W2.
    static bool SelfTestLockTimeout()
    {
        s_testLock = new (nothrow) SatoriLock();
        if (s_testLock == nullptr)
        {
            Report("locktimeout", "FAIL", "no memory");
            return false;
        }

        s_testLock->Initialize();

        s_testLock->Enter();
        s_plainDone = false;
        bool started = GCToEEInterface::CreateThread(LockPlainFn, nullptr, false, "Satori Test Lock");
        bool plainDone = started && WaitUntil(s_plainDone, 10000);
        s_testLock->Leave();
        double plainMs = (double)s_plainTicks * 1000.0 / SatoriUtil::GetTimeStampFrequency();
        bool plainOk = plainDone && !s_plainGot && plainMs >= 19.0 && s_testLock->TestState() == 0;

        int rounds = 10;
        int handoffs = 0;
        bool handoffOk = true;
        for (int i = 0; i < rounds && handoffOk; i++)
        {
            s_w1Done = s_w2Done = s_w1Got = false;
            s_lockFirst = 0;
            s_testLock->Enter();
            bool ok = GCToEEInterface::CreateThread(LockW1Fn, nullptr, false, "Satori Test Lock W1") && WaitForTestLockWaiters(1);
            ok = ok && GCToEEInterface::CreateThread(LockW2Fn, nullptr, false, "Satori Test Lock W2") && WaitForTestLockWaiters(2);
            GCToOSInterface::Sleep(5);
            s_testLock->Leave();
            ok = ok && WaitUntil(s_w1Done, 5000) && WaitUntil(s_w2Done, 5000);
            ok = ok && !s_w1Got && s_testLock->TestState() == 0;
            if (ok && s_lockFirst == 1)
            {
                handoffs++;
            }

            handoffOk = ok;
        }

        bool pass = plainOk && handoffOk;
        char details[260];
        snprintf(details, sizeof(details), "the timed enter of 20 ms gave up: %d after %.1f ms, no waiter left: %d; the rounds with two waiters: %s, in %d of %d the waiter that held the wake handed it on",
            (int)(plainDone && !s_plainGot), plainMs, (int)(s_testLock->TestState() == 0), handoffOk ? "ok" : "FAILED", handoffs, rounds);
        Report("locktimeout", pass ? "PASS" : "FAIL", details);
        return pass;
    }

    // The names of the self-tests in SATORI_TEST_SELF are separated by commas.
    static bool Wants(const char* name)
    {
        char key[48];
        snprintf(key, sizeof(key), ",%s,", name);
        char list[160];
        snprintf(list, sizeof(list), ",%s,", s_selfTests);
        return strstr(list, key) != nullptr;
    }

    void RunSelfTests(SatoriHeap* heap)
    {
        static volatile int32_t s_started;
        if (s_selfTests[0] == 0 || Interlocked::Exchange(&s_started, 1) != 0)
        {
            return;
        }

        s_heap = heap;
        bool conservative = SatoriUtil::IsConservativeMode();
        bool noCache = GCConfig::GetRegionCacheCap() == 0;
        bool noTrimmer = !SatoriUtil::IsTrimmingEnabled();
        bool pass = true;

        // The quarantine of the freed regions: the conservative mode, and the freed regions are not cached.
        // Their marker runs on an app thread, outside the handshake with the trimmer, which frees parked regions: no trimmer.
        if (Wants("reuse") || Wants("absorb"))
        {
            if (!conservative || !noCache || !noTrimmer)
            {
                Report("reuse/absorb", "FAIL", "the self-tests need DOTNET_gcConservative=1, DOTNET_gcRegionCacheCap=0 and DOTNET_gcTrim=0");
                pass = false;
            }
            else
            {
                pass &= !Wants("reuse") || SelfTestReuse(heap);
                pass &= !Wants("absorb") || SelfTestAbsorb(heap);
            }
        }

        // the fallbacks to a region that is committed in full: the precise mode, so that a free is a free, and no cache
        if (Wants("warmlow") || Wants("warmrun") || Wants("racetrim") || Wants("racetrim-none"))
        {
            if (conservative || !noCache)
            {
                Report("warm", "FAIL", "the self-tests need the precise mode and DOTNET_gcRegionCacheCap=0");
                pass = false;
            }
            else
            {
                pass &= !Wants("warmlow") || SelfTestWarmLow(heap);
                pass &= !Wants("warmrun") || SelfTestWarmRun(heap);
                pass &= !Wants("racetrim") || SelfTestRaceTrim(heap, true);
                pass &= !Wants("racetrim-none") || SelfTestRaceTrim(heap, false);
            }
        }

        // the fallbacks to the cache: a cache
        if (Wants("cacheexit1") || Wants("cacheexit2") || Wants("cacheexit3"))
        {
            if (conservative || noCache)
            {
                Report("cacheexit", "FAIL", "the self-tests need the precise mode and a cache (not DOTNET_gcRegionCacheCap=0)");
                pass = false;
            }
            else
            {
                pass &= !Wants("cacheexit1") || SelfTestCacheExit(heap, 1);
                pass &= !Wants("cacheexit2") || SelfTestCacheExit(heap, 2);
                pass &= !Wants("cacheexit3") || SelfTestCacheExit(heap, 3);
            }
        }

        // the minimum free age: the precise mode, so that a free is a free, and no cache. The parked regions need the conservative mode.
        if (Wants("age"))
        {
            if (conservative || !noCache)
            {
                Report("age", "FAIL", "the self-test needs the precise mode and DOTNET_gcRegionCacheCap=0");
                pass = false;
            }
            else
            {
                pass &= SelfTestAge(heap);
            }
        }

        if (Wants("ageparked"))
        {
            if (!conservative || !noCache || !noTrimmer)
            {
                Report("ageparked", "FAIL", "the self-test needs DOTNET_gcConservative=1, DOTNET_gcRegionCacheCap=0 and DOTNET_gcTrim=0");
                pass = false;
            }
            else
            {
                pass &= SelfTestAgeParked(heap);
            }
        }

        // a stale pointer into a free region: the conservative mode, and no trimmer that could free the region while the test looks
        if (Wants("containing"))
        {
            if (!conservative || !noTrimmer)
            {
                Report("containing", "FAIL", "the self-test needs DOTNET_gcConservative=1 and DOTNET_gcTrim=0");
                pass = false;
            }
            else
            {
                pass &= SelfTestContaining(heap);
            }
        }

        // the timed enter of the lock: any mode
        if (Wants("locktimeout"))
        {
            pass &= SelfTestLockTimeout();
        }

        // a failed commit of card slices: the precise mode, so that a free is a free, and no cache
        if (Wants("slicefail"))
        {
            if (conservative || !noCache)
            {
                Report("slicefail", "FAIL", "the self-test needs the precise mode and DOTNET_gcRegionCacheCap=0");
                pass = false;
            }
            else
            {
                pass &= SelfTestSliceFail(heap);
            }
        }

        _ASSERTE(pass);
    }
}

#endif // _DEBUG
