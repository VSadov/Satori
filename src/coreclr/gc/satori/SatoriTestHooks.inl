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
    thread_local bool t_holdMarker;
    thread_local bool t_inParkDrain;

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

    void RunSelfTests(SatoriHeap* heap)
    {
        static volatile int32_t s_started;
        if (s_selfTests[0] == 0 || Interlocked::Exchange(&s_started, 1) != 0)
        {
            return;
        }

        s_heap = heap;
        if (!SatoriUtil::IsConservativeMode() || GCConfig::GetRegionCacheCap() != 0)
        {
            Report("setup", "FAIL", "the self-tests need DOTNET_gcConservative=1 and DOTNET_gcRegionCacheCap=0");
            return;
        }

        bool pass = true;
        if (strstr(s_selfTests, "reuse"))
        {
            pass &= SelfTestReuse(heap);
        }

        if (strstr(s_selfTests, "absorb"))
        {
            pass &= SelfTestAbsorb(heap);
        }

        _ASSERTE(pass);
    }
}

#endif // _DEBUG
