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
// SatoriAllocator.cpp
//

#include "common.h"
#include "gcenv.h"
#include "../env/gcenv.os.h"
#include "../gceventstatus.h"

#include "SatoriUtil.h"
#include "SatoriHeap.h"
#include "SatoriObject.h"
#include "SatoriObject.inl"
#include "SatoriPage.h"
#include "SatoriPage.inl"
#include "SatoriAllocator.h"
#include "SatoriRegion.h"
#include "SatoriRegion.inl"
#include "SatoriRegionQueue.h"
#include "SatoriAllocationContext.h"
#include "SatoriWorkChunk.h"
#include "SatoriWorkList.h"
#include <minipal/log.h>

// TEMP-TRACE-BEGIN
#include <stdio.h>
#include <stdlib.h>
#include "SatoriPageAddTrace.h"
#include "SatoriTestHooks.h" // TEST-HOOK

bool g_traceOn;
volatile size_t g_trimOut;
volatile size_t g_missWhileOut;
volatile size_t g_missEmpty;
volatile size_t g_missPasses;
volatile size_t g_timedOutAdds;
volatile size_t g_trimRounds;
volatile size_t g_decommitBytes;
volatile size_t g_recommit120Bytes;
volatile size_t g_addWaitEpisodes;
volatile size_t g_addWaitTicks;
volatile size_t g_addWaitMaxTicks;
volatile size_t g_addCount;
volatile size_t g_addTicks;
volatile size_t g_addMaxTicks;
thread_local int64_t t_addWaitedTicks;
// The waiters of a page add: the calls that got the page-add lock after an add completed, and looked for regions again.
// The delay is from the page being published to the call getting its region. A convoy is the waiters of one add: its length is the longest delay.
volatile size_t g_lockRetries;
volatile size_t g_convoyWaiters;
volatile size_t g_convoyTicks;
volatile size_t g_convoyMaxTicks;
volatile size_t g_convoyEpochs;
volatile size_t g_convoyEpochMaxSumTicks;
static volatile size_t g_convoyEpochTs;
static volatile size_t g_convoyEpochMax;
static volatile size_t g_pagePublishTs;
static thread_local size_t t_retryTs;
static volatile size_t s_convoyHist[20];
// More experiments of step 2. SATORI_COMMIT_STEP_KB: memory for allocation is committed in steps of this size, if it is bigger than
// the commit granularity. SATORI_CLAIM_TOPDOWN=1: regular regions are claimed from the top of a page, SATORI_CLAIM_NOHINT=1: from the first page.
static size_t g_commitStep;
static volatile int32_t g_commitDoubling;
static volatile int32_t g_claimTopDown;
static volatile int32_t g_claimNoHint;

// Step 5 of Milestone 2b: what the regular region requests (TryGetRegularRegion) take. (see [nbrstats])
static volatile size_t g_nbrReqs;
static volatile size_t g_nbrWithNext;
static volatile size_t g_nbrNextNoQueue;
static volatile size_t g_nbrNextReusable;
static volatile size_t g_nbrNextOther;
static volatile size_t g_nbrReusableTaken;
static volatile size_t g_nbrReusableHit;
static volatile size_t g_nbrGetRegion;

// Step 1 of Milestone 2c: the conservative mode parks the regions that it frees until the drain after the GC. (see [parkstats])
// What was parked since the last drain: regions, their size and what they have committed.
static size_t g_parkCycleRegions;
static size_t g_parkCycleBytes;
static size_t g_parkCycleCommitted;
static size_t g_parkRegions;
static size_t g_parkBytes;
static size_t g_parkMaxCycleRegions;
static size_t g_parkMaxCycleBytes;
// the biggest share of the heap commit that a drain has freed, in thousandths
static size_t g_parkMaxSharePermille;
static size_t g_drainCount;
static size_t g_drainTicks;
static size_t g_drainMaxTicks;

// Step 2 of Milestone 2c: the time that a blocking GC of a Checked build waits for the trimmer. (see [verifywait])
static size_t g_verifyWaits;
static size_t g_verifyWaitsOver1Ms;
static size_t g_verifyWaitTicks;
static size_t g_verifyWaitMaxTicks;

void TraceVerifyWait(int64_t startTicks)
{
    if (!SatoriTrace::s_enabled)
    {
        return;
    }

    size_t ticks = (size_t)(SatoriUtil::GetTimeStamp() - startTicks);
    g_verifyWaits++;
    g_verifyWaitTicks += ticks;
    g_verifyWaitMaxTicks = max(g_verifyWaitMaxTicks, ticks);
    if (ticks * 1000 > (size_t)SatoriUtil::GetTimeStampFrequency())
    {
        g_verifyWaitsOver1Ms++;
    }
}

static void TraceCount(volatile size_t* counter)
{
    if (g_traceOn)
    {
        Interlocked::Increment(counter);
    }
}

// Where the memory for allocation is committed up to, when allocation needs more than a region has committed.
// SATORI_COMMIT_STEP_KB: in steps of that size. SATORI_COMMIT_DOUBLING=1: each step is as much as the region has committed already, up to 2 MB.
size_t TraceCommitTo(size_t regionStart, size_t committed, size_t ensureCommitted)
{
    size_t step = g_commitStep ? g_commitStep : SatoriUtil::CommitGranularity();
    if (g_commitDoubling)
    {
        size_t have = committed > regionStart ? committed - regionStart : 0;
        DWORD bit;
        if (have && BitScanReverse64(&bit, have))
        {
            step = max(step, min((size_t)1 << bit, (size_t)Satori::REGION_SIZE_GRANULARITY));
        }
    }

    return ALIGN_UP(ensureCommitted, step);
}

// The commits and decommits that are asked of the OS, sharded by thread to keep the counting cheap.
struct DECLSPEC_ALIGN(128) TraceOsShard
{
    size_t calls;
    size_t bytes;
    size_t ticks;
    size_t maxTicks;
    size_t hist[20];
};

static TraceOsShard s_osCommit[64];
static TraceOsShard s_osDecommit[64];
volatile size_t g_getRegionCalls;
volatile size_t g_getRegionTicks;
volatile size_t g_getRegionMaxTicks;
volatile size_t g_suspendCount;
volatile size_t g_suspendTicks;
volatile size_t g_suspendMaxTicks;
// Bucket 0 is < 1 usec, bucket k is [2^(k-1), 2^k) usec.
static volatile size_t s_addWaitHist[20];
static volatile size_t s_addHist[20];
static volatile size_t s_getRegionHist[20];
static volatile size_t s_suspendHist[20];

// C, B, A, E, Dg, Di
static volatile size_t s_traceAdds[6];
static const char* const s_traceClassNames[6] = { "C", "B", "A", "E", "Dg", "Di" };
static const char s_traceRequesterTags[] = "?RLSHPIWFT";

// What the failed claims of the current call have run into: lost CAS races and rolled back runs.
static thread_local size_t t_traceContention;

void TraceTrimInc()
{
    Interlocked::Increment(&g_trimOut);
}

void TraceTrimDec()
{
    size_t result = Interlocked::Decrement(&g_trimOut);
    _ASSERTE((ptrdiff_t)result >= 0);
    (void)result;
}

void TraceTrimRound()
{
    Interlocked::Increment(&g_trimRounds);
}

// Step 4 of Milestone 2b: why the rounds ran, the decommit calls of the trimmer, and what a call covered. (see [trimstats])
static volatile size_t g_trimRoundsGen2;
static volatile size_t g_trimRoundsIdle;
static volatile size_t g_trimCalls;
static volatile size_t g_trimCallGranules;
static volatile size_t g_trimCallBytes;
// runs of 1, 2-3, 4-7, 8-15, 16-31, 32 granules
static volatile size_t g_trimRunHist[6];
static volatile size_t g_trimSkippedForReader;
static volatile size_t g_trimSkippedGranules;

void TraceTrimRoundWhy(bool idle)
{
    Interlocked::Increment(idle ? &g_trimRoundsIdle : &g_trimRoundsGen2);
}

void TraceTrimCall(size_t granules, size_t bytes)
{
    Interlocked::Increment(&g_trimCalls);
    Interlocked::ExchangeAdd64(&g_trimCallGranules, granules);
    Interlocked::ExchangeAdd64(&g_trimCallBytes, bytes);
    DWORD bucket;
    BitScanReverse64(&bucket, granules);
    Interlocked::Increment(&g_trimRunHist[min((size_t)bucket, (size_t)5)]);
}

void TraceTrimSkippedForReader(size_t granules)
{
    Interlocked::Increment(&g_trimSkippedForReader);
    Interlocked::ExchangeAdd64(&g_trimSkippedGranules, granules);
}

// When something was decommitted in a 2 MB granule, and how much of it is not committed again yet.
// The slots are per block of 1 GB, found in a small open addressing table, allocated when the first decommit in the block is seen.
struct TraceSlot
{
    int64_t stamp;
    size_t pending;
};

struct TraceBlock
{
    size_t key;
    TraceSlot* slots;
};

static const int TRACE_BLOCK_BITS = 30;
static const int TRACE_TABLE_BITS = 13;
static const size_t TRACE_TABLE_SIZE = (size_t)1 << TRACE_TABLE_BITS;
static TraceBlock s_traceBlocks[TRACE_TABLE_SIZE];
static volatile int32_t s_traceBlocksLock;

static TraceSlot* TraceSlotsForBlock(size_t address, bool create)
{
    size_t key = (address >> TRACE_BLOCK_BITS) + 1;
    size_t i = (size_t)((key * 11400714819323198485llu) >> (64 - TRACE_TABLE_BITS));
    for (size_t probe = 0; probe < TRACE_TABLE_SIZE; probe++, i = (i + 1) & (TRACE_TABLE_SIZE - 1))
    {
        size_t k = VolatileLoad(&s_traceBlocks[i].key);
        if (k == 0 && create)
        {
            while (Interlocked::CompareExchange(&s_traceBlocksLock, 1, 0) != 0)
            {
                GCToOSInterface::YieldThread(0);
            }

            k = s_traceBlocks[i].key;
            if (k == 0)
            {
                s_traceBlocks[i].slots = (TraceSlot*)calloc((size_t)1 << (TRACE_BLOCK_BITS - Satori::REGION_BITS), sizeof(TraceSlot));
                VolatileStore(&s_traceBlocks[i].key, key);
                k = key;
            }

            Interlocked::Exchange(&s_traceBlocksLock, 0);
        }

        if (k == key)
        {
            return s_traceBlocks[i].slots;
        }

        if (k == 0)
        {
            return nullptr;
        }
    }

    return nullptr;
}

void TraceNoteDecommitSlow(size_t address, size_t bytes)
{
    Interlocked::ExchangeAdd64(&g_decommitBytes, bytes);

    int64_t now = SatoriUtil::GetTimeStamp();
    int64_t window = 120 * SatoriUtil::GetTimeStampFrequency();
    size_t end = address + bytes;
    for (size_t a = address; a < end; )
    {
        size_t pieceEnd = min(end, ((a >> Satori::REGION_BITS) + 1) << Satori::REGION_BITS);
        TraceSlot* slots = TraceSlotsForBlock(a, /* create */ true);
        if (slots)
        {
            TraceSlot& slot = slots[(a & (((size_t)1 << TRACE_BLOCK_BITS) - 1)) >> Satori::REGION_BITS];
            if (now - slot.stamp > window)
            {
                slot.pending = 0;
            }

            slot.pending += pieceEnd - a;
            slot.stamp = now;
        }

        a = pieceEnd;
    }
}

void TraceNoteCommitSlow(size_t address, size_t bytes)
{
    int64_t now = SatoriUtil::GetTimeStamp();
    int64_t window = 120 * SatoriUtil::GetTimeStampFrequency();
    size_t end = address + bytes;
    for (size_t a = address; a < end; )
    {
        size_t pieceEnd = min(end, ((a >> Satori::REGION_BITS) + 1) << Satori::REGION_BITS);
        TraceSlot* slots = TraceSlotsForBlock(a, /* create */ false);
        if (slots)
        {
            TraceSlot& slot = slots[(a & (((size_t)1 << TRACE_BLOCK_BITS) - 1)) >> Satori::REGION_BITS];
            if (slot.pending != 0)
            {
                if (now - slot.stamp <= window)
                {
                    size_t again = min(pieceEnd - a, slot.pending);
                    slot.pending -= again;
                    Interlocked::ExchangeAdd64(&g_recommit120Bytes, again);
                }
                else
                {
                    slot.pending = 0;
                }
            }
        }

        a = pieceEnd;
    }
}

static int64_t s_traceStartTs;
static int64_t s_traceIntervalTicks;
static volatile size_t s_traceNextStateTs;
static SatoriAllocator* s_traceAllocator;
static void* volatile s_traceFile;

static double TraceTicksToUs(int64_t ticks)
{
    return (double)ticks * 1e6 / (double)SatoriUtil::GetTimeStampFrequency();
}

static void TraceWrite(const char* line, int len)
{
    if (len <= 0 || !g_traceOn)
    {
        return;
    }

    void* file = s_traceFile;
    if (file == nullptr)
    {
        char dir[MAX_PATH];
        DWORD n = GetEnvironmentVariableA("SATORI_TRACE_DIR", dir, sizeof(dir));
        if (n == 0 || n >= sizeof(dir))
        {
            return;
        }

        char path[MAX_PATH + 32];
        snprintf(path, sizeof(path), "%s\\%lu.log", dir, (unsigned long)GetCurrentProcessId());
        void* opened = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        void* prior = Interlocked::CompareExchangePointer((void**)&s_traceFile, opened, (void*)nullptr);
        if (prior != nullptr)
        {
            if (opened != INVALID_HANDLE_VALUE)
            {
                CloseHandle(opened);
            }

            opened = prior;
        }

        file = opened;
    }

    if (file != INVALID_HANDLE_VALUE)
    {
        DWORD written;
        WriteFile(file, line, (DWORD)len, &written, nullptr);
    }
}

static bool TraceStateDue()
{
    if (!g_traceOn)
    {
        return false;
    }

    size_t next = s_traceNextStateTs;
    int64_t now = SatoriUtil::GetTimeStamp();
    if (now - (int64_t)next < 0)
    {
        return false;
    }

    return Interlocked::CompareExchange(&s_traceNextStateTs, (size_t)(now + s_traceIntervalTicks), next) == next;
}

static void TraceInit(SatoriAllocator* allocator)
{
    char dir[MAX_PATH];
    char preAdd[16];
    if (GetEnvironmentVariableA("SATORI_COMMIT_STEP_KB", preAdd, sizeof(preAdd)) != 0)
    {
        g_commitStep = max(SatoriUtil::CommitGranularity(), (size_t)atoi(preAdd) * 1024);
    }

    if (GetEnvironmentVariableA("SATORI_COMMIT_DOUBLING", preAdd, sizeof(preAdd)) != 0)
    {
        g_commitDoubling = atoi(preAdd);
    }

    if (GetEnvironmentVariableA("SATORI_CLAIM_TOPDOWN", preAdd, sizeof(preAdd)) != 0)
    {
        g_claimTopDown = atoi(preAdd);
    }

    if (GetEnvironmentVariableA("SATORI_CLAIM_NOHINT", preAdd, sizeof(preAdd)) != 0)
    {
        g_claimNoHint = atoi(preAdd);
    }

    DWORD n = GetEnvironmentVariableA("SATORI_TRACE_DIR", dir, sizeof(dir));
    if (n == 0 || n >= sizeof(dir))
    {
        return;
    }

    g_traceOn = true;
    SatoriTrace::s_enabled = true;
    s_traceAllocator = allocator;
    s_traceStartTs = SatoriUtil::GetTimeStamp();

    int seconds = 30;
    char value[16];
    if (GetEnvironmentVariableA("SATORI_TRACE_STATE_SEC", value, sizeof(value)) != 0)
    {
        seconds = max(1, atoi(value));
    }

    s_traceIntervalTicks = (int64_t)seconds * SatoriUtil::GetTimeStampFrequency();
    s_traceNextStateTs = (size_t)(s_traceStartTs + s_traceIntervalTicks);
    atexit(&SatoriAllocator::TraceAtExit);
}

// What is free: the free granules of the pages, except one, and the regions in the cache. A free granule is a region of 2 MB,
// and the free granules that touch make a run.
void SatoriAllocator::TraceTakeSnapshot(TraceSnap& s, SatoriPage* excludePage, size_t request)
{
    memset(&s, 0, sizeof(s));
    if (!g_traceOn)
    {
        return;
    }

    m_heap->ForEachPage(
        [&](SatoriPage* page)
        {
            if (page != excludePage)
            {
                page->TraceCollect(s, request);
            }
        }
    );

    // A cached region is formatted and out of the bitmap, so it is a run of its own.
    m_regionCache->ForEachRegionLocked(
        [&](SatoriRegion* r)
        {
            size_t size = r->Size();
            size_t committed = r->CommittedEnd() > r->Start() ? min(r->CommittedEnd(), r->End()) - r->Start() : 0;
            s.regions++;
            s.n2++;
            s.freeBytes += size;
            s.freeCommitted += committed;
            s.runs++;
            s.maxRun = max(s.maxRun, size);
            s.maxRegion = max(s.maxRegion, size);
            if (committed == size)
            {
                s.warmFree++;
            }
            if (size >= request)
            {
                s.singleGE++;
                s.runsGE++;
                if (committed == size)
                {
                    s.runsGECommitted++;
                }
            }
        }
    );
}

static int TraceUsBucket(double us)
{
    int k = 0;
    while (k < 19 && us >= (double)((size_t)1 << k))
    {
        k++;
    }

    return k;
}

static void TraceRaiseMax(volatile size_t* target, size_t value)
{
    size_t current;
    while (value > (current = *target) && Interlocked::CompareExchange(target, value, current) != current)
    {
    }
}

// Enters the page-add lock. A wait is an enter that found the lock held, and its time is the time that the enter took.
static void TraceEnterPageAddLock(SatoriLock* lock)
{
    if (!g_traceOn)
    {
        lock->Enter();
        return;
    }

    if (lock->TryEnterOneShot())
    {
        return;
    }

    int64_t waitStart = SatoriUtil::GetTimeStamp();
    lock->Enter();
    int64_t ticks = max((int64_t)0, SatoriUtil::GetTimeStamp() - waitStart);
    Interlocked::Increment(&g_addWaitEpisodes);
    Interlocked::ExchangeAdd64(&g_addWaitTicks, (size_t)ticks);
    TraceRaiseMax(&g_addWaitMaxTicks, (size_t)ticks);
    Interlocked::Increment(&s_addWaitHist[TraceUsBucket(TraceTicksToUs(ticks))]);
    t_addWaitedTicks += ticks;

    if (TraceTicksToUs(ticks) >= 1000.0)
    {
        char line[160];
        int len = snprintf(line, sizeof(line), "[addwait] t=%.3f tid=%lu waitUs=%.0f\n",
            TraceTicksToUs(SatoriUtil::GetTimeStamp() - s_traceStartTs) / 1000.0, (unsigned long)GetCurrentThreadId(), TraceTicksToUs(ticks));
        TraceWrite(line, len);
    }
}

// A call got the page-add lock after an add completed, and is to look for regions again.
static void TraceNoteLockRetry()
{
    if (g_traceOn)
    {
        Interlocked::Increment(&g_lockRetries);
        t_retryTs = g_pagePublishTs;
    }
}

// A call that looked again after an add has its region, or has failed. The convoy of an add is its waiters, and the delay of its last one is the length.
static void TraceNoteRetryClaimed()
{
    if (!g_traceOn || t_retryTs == 0)
    {
        return;
    }

    size_t published = t_retryTs;
    t_retryTs = 0;
    size_t ticks = (size_t)max((int64_t)0, SatoriUtil::GetTimeStamp() - (int64_t)published);
    Interlocked::Increment(&g_convoyWaiters);
    Interlocked::ExchangeAdd64(&g_convoyTicks, ticks);
    TraceRaiseMax(&g_convoyMaxTicks, ticks);
    Interlocked::Increment(&s_convoyHist[TraceUsBucket(TraceTicksToUs((int64_t)ticks))]);

    // The waiters of the add that was published at `published`. When they are of another add, the one before is complete.
    size_t epoch = g_convoyEpochTs;
    if (epoch != published && Interlocked::CompareExchange(&g_convoyEpochTs, published, epoch) == epoch)
    {
        size_t finished = g_convoyEpochMax;
        g_convoyEpochMax = 0;
        if (epoch != 0)
        {
            Interlocked::Increment(&g_convoyEpochs);
            Interlocked::ExchangeAdd64(&g_convoyEpochMaxSumTicks, finished);
        }
    }

    TraceRaiseMax(&g_convoyEpochMax, ticks);
}

static void TraceNoteOsCall(TraceOsShard* shards, size_t bytes, int64_t ticks)
{
    ticks = max((int64_t)0, ticks);
    TraceOsShard& s = shards[GetCurrentThreadId() & 63];
    Interlocked::Increment(&s.calls);
    Interlocked::ExchangeAdd64(&s.bytes, bytes);
    Interlocked::ExchangeAdd64(&s.ticks, (size_t)ticks);
    TraceRaiseMax(&s.maxTicks, (size_t)ticks);
    Interlocked::Increment(&s.hist[TraceUsBucket(TraceTicksToUs(ticks))]);
}

bool TraceVirtualCommit(void* address, size_t size)
{
    if (!g_traceOn)
    {
        return GCToOSInterface::VirtualCommit(address, size);
    }

    int64_t start = SatoriUtil::GetTimeStamp();
    bool result = GCToOSInterface::VirtualCommit(address, size);
    TraceNoteOsCall(s_osCommit, size, SatoriUtil::GetTimeStamp() - start);
    return result;
}

bool TraceVirtualDecommit(void* address, size_t size)
{
    if (!g_traceOn)
    {
        return GCToOSInterface::VirtualDecommit(address, size);
    }

    int64_t start = SatoriUtil::GetTimeStamp();
    bool result = GCToOSInterface::VirtualDecommit(address, size);
    TraceNoteOsCall(s_osDecommit, size, SatoriUtil::GetTimeStamp() - start);
    return result;
}

// The page is published, so others can find it. The time since the decision is the duration of the add.
static void TraceNoteAddPublished(TracePageAddCtx& c)
{
    if (!g_traceOn)
    {
        return;
    }

    c.publishedTs = SatoriUtil::GetTimeStamp();
    g_pagePublishTs = (size_t)c.publishedTs;
    int64_t ticks = max((int64_t)0, c.publishedTs - c.decisionTs);
    Interlocked::Increment(&g_addCount);
    Interlocked::ExchangeAdd64(&g_addTicks, (size_t)ticks);
    TraceRaiseMax(&g_addMaxTicks, (size_t)ticks);
    Interlocked::Increment(&s_addHist[TraceUsBucket(TraceTicksToUs(ticks))]);
}

// A GetRegion call took this long.
void TraceNoteGetRegion(int64_t ticks)
{
    ticks = max((int64_t)0, ticks);
    Interlocked::Increment(&g_getRegionCalls);
    Interlocked::ExchangeAdd64(&g_getRegionTicks, (size_t)ticks);
    TraceRaiseMax(&g_getRegionMaxTicks, (size_t)ticks);
    Interlocked::Increment(&s_getRegionHist[TraceUsBucket(TraceTicksToUs(ticks))]);
}

// A blocking GC waited this long for the world to stop.
void TraceNoteSuspend(int64_t ticks)
{
    ticks = max((int64_t)0, ticks);
    Interlocked::Increment(&g_suspendCount);
    Interlocked::ExchangeAdd64(&g_suspendTicks, (size_t)ticks);
    TraceRaiseMax(&g_suspendMaxTicks, (size_t)ticks);
    Interlocked::Increment(&s_suspendHist[TraceUsBucket(TraceTicksToUs(ticks))]);
}

// A lookup found nothing.
void SatoriAllocator::TraceMiss(TracePageAddCtx& c)
{
    if (!g_traceOn)
    {
        return;
    }

    c.passes++;
    Interlocked::Increment(&g_missPasses);
    bool out = g_trimOut > 0;
    if (out)
    {
        c.sawOut = true;
    }

    if (!c.missed)
    {
        c.missed = true;
        c.firstMissTs = SatoriUtil::GetTimeStamp();
        Interlocked::Increment(out ? &g_missWhileOut : &g_missEmpty);
    }
}

// We are about to add a page. Everything that must be seen before the add.
void SatoriAllocator::TraceBeforeAdd(TracePageAddCtx& c, size_t regionSize)
{
    if (!g_traceOn)
    {
        return;
    }

    c.decisionTs = SatoriUtil::GetTimeStamp();
    c.out = t_traceContention;
    c.trim = g_trimOut;
    c.commitBefore = m_heap->GetBytesCommitted();
    c.peakBefore = m_heap->GetPeakBytesCommitted();

    TraceSnap before;
    TraceTakeSnapshot(before, nullptr, regionSize);
    c.preCount = before.regions;

    m_heap->Recycler()->TraceGetState(c.gcState, c.condemned, c.defSweep, c.relocated);
}

// A regular page add is about to start, under the lock. The decision to add is stamped here, after the wait for the lock, so that the duration
// of an add is the time that it holds the lock. What the call has waited for the lock is in the line of the add.
void SatoriAllocator::TraceRegularAdd(TracePageAddCtx& c)
{
    if (!g_traceOn)
    {
        return;
    }

    c.decisionTs = SatoriUtil::GetTimeStamp();
    c.waitedTicks = t_addWaitedTicks;
}

// We did add a page, and released the lock. The new page is not in the snapshot.
void SatoriAllocator::TraceAfterAdd(TracePageAddCtx& c, SatoriPage* page, size_t regionSize, bool low, SatoriRegionRequester requester)
{
    if (!g_traceOn)
    {
        return;
    }

    TraceSnap s;
    TraceTakeSnapshot(s, page, regionSize);

    bool gcActive = c.gcState != 0 || c.defSweep != 0;
    int cls;
    if (c.out > 0)
    {
        cls = 0;
    }
    else if (s.singleGE > 0)
    {
        cls = 1;
    }
    else if (s.runsGE > 0)
    {
        cls = 2;
    }
    else if (s.freeBytes >= regionSize)
    {
        cls = 3;
    }
    else
    {
        cls = gcActive ? 4 : 5;
    }

    Interlocked::Increment(&s_traceAdds[cls]);

    size_t pages = 0;
    m_heap->ForEachPage([&](SatoriPage*) { pages++; });

    const char* why = c.out > 0 ? "out" : "-";
    char tag = s_traceRequesterTags[(int)requester];
    double waitUs = c.missed ? TraceTicksToUs(c.decisionTs - c.firstMissTs) : 0;

    char buf[1536];
    int len = snprintf(buf, sizeof(buf),
        "[pageadd] t=%.3f tid=%lu tag=%c reqKB=%zu low=%d large=%d cls=%s why=%s sawOut=%d passes=%d waitUs=%.0f "
        "out=%zu trim=%zu preCount=%zu gc=%d cgen=%d defsweep=%zu relocq=%zu "
        "free=%zu n2=%zu n46=%zu nbig=%zu freeMB=%.1f freeCommMB=%.1f maxregKB=%zu maxrunKB=%zu runs=%zu "
        "singleGE=%zu runsGE=%zu runsGEc=%zu trunc=%d pages=%zu committedMB=%.1f commBeforeMB=%.1f peakBeforeMB=%.1f waitedUs=%.0f addUs=%.0f\n",
        TraceTicksToUs(c.decisionTs - s_traceStartTs) / 1000.0, (unsigned long)GetCurrentThreadId(), tag,
        regionSize >> 10, (int)low, (int)(regionSize >= Satori::PAGE_SIZE_GRANULARITY / 2), s_traceClassNames[cls], why,
        (int)(c.sawOut || c.out > 0), c.passes, waitUs,
        c.out, c.trim, c.preCount, c.gcState, c.condemned, c.defSweep, c.relocated,
        s.regions, s.n2, s.n46, s.nBig, (double)s.freeBytes / (1 << 20), (double)s.freeCommitted / (1 << 20),
        s.maxRegion >> 10, s.maxRun >> 10, s.runs,
        s.singleGE, s.runsGE, s.runsGECommitted, (int)s.truncated, pages, (double)m_heap->GetBytesCommitted() / (1 << 20),
        (double)c.commitBefore / (1 << 20), (double)c.peakBefore / (1 << 20),
        TraceTicksToUs(c.waitedTicks), c.publishedTs ? TraceTicksToUs(c.publishedTs - c.decisionTs) : 0.0);
    TraceWrite(buf, len);
}

void SatoriAllocator::TraceWriteState(bool isFinal, const char* ev)
{
    // at exit other threads may be dead with locks held, so the final line has no snapshot.
    TraceSnap s;
    if (isFinal)
    {
        memset(&s, 0, sizeof(s));
    }
    else
    {
        TraceTakeSnapshot(s, nullptr, 2 * Satori::REGION_SIZE_GRANULARITY);
    }

    size_t pages = 0;
    m_heap->ForEachPage([&](SatoriPage*) { pages++; });

    int gcState;
    int condemned;
    size_t defSweep;
    size_t relocated;
    m_heap->Recycler()->TraceGetState(gcState, condemned, defSweep, relocated);

    size_t occPublished[3];
    size_t occAccumulating[3];
    m_heap->Recycler()->TraceGetOccupancy(occPublished, occAccumulating);

    char buf[2048];
    int len = snprintf(buf, sizeof(buf),
        "[state] t=%.3f final=%d pages=%zu committedMB=%.1f gc=%d cgen=%d defsweep=%zu relocq=%zu "
        "free=%zu n2=%zu n46=%zu nbig=%zu freeMB=%.1f freeCommMB=%.1f maxregKB=%zu maxrunKB=%zu runs=%zu runsGE4=%zu runsGE4c=%zu trunc=%d "
        "out=%zu trim=%zu missOut=%zu missEmpty=%zu missPasses=%zu timedOutAdds=%zu "
        "adds=%zu C=%zu B=%zu A=%zu E=%zu Dg=%zu Di=%zu outWin=0 "
        "ev=%s peakMB=%.1f trimRounds=%zu decommitMB=%.1f recommit120MB=%.1f warmFree=%zu "
        "occ0MB=%.1f occ1MB=%.1f occ2MB=%.1f accMB=%.1f\n",
        TraceTicksToUs(SatoriUtil::GetTimeStamp() - s_traceStartTs) / 1000.0, (int)isFinal, pages,
        (double)m_heap->GetBytesCommitted() / (1 << 20), gcState, condemned, defSweep, relocated,
        s.regions, s.n2, s.n46, s.nBig, (double)s.freeBytes / (1 << 20), (double)s.freeCommitted / (1 << 20),
        s.maxRegion >> 10, s.maxRun >> 10, s.runs, s.runsGE, s.runsGECommitted, (int)s.truncated,
        (size_t)0, g_trimOut, g_missWhileOut, g_missEmpty, g_missPasses, g_timedOutAdds,
        s_traceAdds[0] + s_traceAdds[1] + s_traceAdds[2] + s_traceAdds[3] + s_traceAdds[4] + s_traceAdds[5],
        s_traceAdds[0], s_traceAdds[1], s_traceAdds[2], s_traceAdds[3], s_traceAdds[4], s_traceAdds[5],
        ev, (double)m_heap->GetPeakBytesCommitted() / (1 << 20), g_trimRounds,
        (double)g_decommitBytes / (1 << 20), (double)g_recommit120Bytes / (1 << 20), s.warmFree,
        (double)occPublished[0] / (1 << 20), (double)occPublished[1] / (1 << 20), (double)occPublished[2] / (1 << 20),
        (double)(occAccumulating[0] + occAccumulating[1] + occAccumulating[2]) / (1 << 20));
    TraceWrite(buf, len);

    // the work of the allocator of regions, as far as it is counted
    SatoriTraceCounters& c = SatoriTrace::s_counters;
    size_t totalPageAdds = 0;
    char byRequester[128];
    int pos = 0;
    byRequester[0] = 0;
    for (int i = 0; i < (int)SatoriRegionRequester::Count; i++)
    {
        totalPageAdds += c.pageAdds[i];
        if (c.pageAdds[i] && pos < (int)sizeof(byRequester) - 16)
        {
            pos += snprintf(byRequester + pos, sizeof(byRequester) - pos, "%c%zu", s_traceRequesterTags[i], c.pageAdds[i]);
        }
    }

    size_t claimN, warmN, coldN, getN;
    double claimP50 = TraceTicksToUs(SatoriTrace::Percentile(c.claimTicks, 0.50, claimN));
    double claimP99 = TraceTicksToUs(SatoriTrace::Percentile(c.claimTicks, 0.99, claimN));
    double warmP50 = TraceTicksToUs(SatoriTrace::Percentile(c.claimWarmTicks, 0.50, warmN));
    double warmP99 = TraceTicksToUs(SatoriTrace::Percentile(c.claimWarmTicks, 0.99, warmN));
    double coldP50 = TraceTicksToUs(SatoriTrace::Percentile(c.claimColdTicks, 0.50, coldN));
    double coldP99 = TraceTicksToUs(SatoriTrace::Percentile(c.claimColdTicks, 0.99, coldN));
    double getP50 = TraceTicksToUs(SatoriTrace::Percentile(c.getRegionTicks, 0.50, getN));
    double getP99 = TraceTicksToUs(SatoriTrace::Percentile(c.getRegionTicks, 0.99, getN));

    char counters[2048];
    len = snprintf(counters, sizeof(counters),
        "[counters] t=%.3f final=%d ev=%s cacheCap=%zu cacheNow=%zu padd=%zu paddBy=%s "
        "claimsSingle=%zu claimsRun=%zu casRetries=%zu runRollbacks=%zu adjTried=%zu adjClaimed=%zu "
        "cacheHits=%zu cacheMisses=%zu cacheFrees=%zu cacheOverflows=%zu freesBitmap=%zu freesBitmapGranules=%zu gapCommitMB=%.1f "
        "claimN=%zu claimP50us=%.2f claimP99us=%.2f "
        "claimWarmN=%zu claimWarmP50us=%.2f claimWarmP99us=%.2f claimWarmMs=%.1f "
        "claimColdN=%zu claimColdP50us=%.2f claimColdP99us=%.2f claimColdMs=%.1f "
        "getN=%zu getP50us=%.2f getP99us=%.2f\n",
        TraceTicksToUs(SatoriUtil::GetTimeStamp() - s_traceStartTs) / 1000.0, (int)isFinal, ev, m_regionCacheCap, (size_t)m_regionCacheCount,
        totalPageAdds, byRequester[0] ? byRequester : "-",
        c.claimsSingle, c.claimsRun, c.casRetries, c.runRollbacks, c.adjacentTries, c.adjacentClaims,
        c.cacheHits, c.cacheMisses, c.freesToCache, c.cacheOverflows, c.freesToBitmap, c.freesToBitmapGranules,
        (double)c.gapCommitBytes / (1 << 20),
        claimN, claimP50, claimP99,
        warmN, warmP50, warmP99, TraceTicksToUs((int64_t)c.claimWarmTotalTicks) / 1000.0,
        coldN, coldP50, coldP99, TraceTicksToUs((int64_t)c.claimColdTotalTicks) / 1000.0,
        getN, getP50, getP99);
    TraceWrite(counters, len);

    char waitHist[256];
    char addHist[256];
    char convoyHist[256];
    int waitPos = 0;
    int addPos = 0;
    int convoyPos = 0;
    for (int k = 0; k < 20; k++)
    {
        waitPos += snprintf(waitHist + waitPos, sizeof(waitHist) - waitPos, k ? ",%zu" : "%zu", s_addWaitHist[k]);
        addPos += snprintf(addHist + addPos, sizeof(addHist) - addPos, k ? ",%zu" : "%zu", s_addHist[k]);
        convoyPos += snprintf(convoyHist + convoyPos, sizeof(convoyHist) - convoyPos, k ? ",%zu" : "%zu", s_convoyHist[k]);
    }

    // waits: the enters of the page-add lock that found it held, retries: the calls that got it after an add completed and looked again.
    // The convoy of an add is its waiters, and its length is the longest delay from the page being published to a waiter having its region.
    size_t openEpoch = g_convoyEpochMax != 0 ? 1 : 0;
    size_t epochs = g_convoyEpochs + openEpoch;
    double convoyMeanUs = epochs ? TraceTicksToUs((int64_t)(g_convoyEpochMaxSumTicks + g_convoyEpochMax)) / (double)epochs : 0.0;
    len = snprintf(buf, sizeof(buf),
        "[addstats] t=%.3f ev=%s waits=%zu waitMs=%.3f waitMaxUs=%.0f retries=%zu adds=%zu addMs=%.3f addMaxUs=%.0f "
        "convoys=%zu convoyMeanUs=%.0f convoyMaxUs=%.0f waiters=%zu waiterMeanUs=%.0f waitHist=%s addHist=%s convoyHist=%s\n",
        TraceTicksToUs(SatoriUtil::GetTimeStamp() - s_traceStartTs) / 1000.0, ev,
        g_addWaitEpisodes, TraceTicksToUs((int64_t)g_addWaitTicks) / 1000.0, TraceTicksToUs((int64_t)g_addWaitMaxTicks),
        g_lockRetries, g_addCount, TraceTicksToUs((int64_t)g_addTicks) / 1000.0, TraceTicksToUs((int64_t)g_addMaxTicks),
        epochs, convoyMeanUs, TraceTicksToUs((int64_t)g_convoyMaxTicks), g_convoyWaiters,
        g_convoyWaiters ? TraceTicksToUs((int64_t)g_convoyTicks) / (double)g_convoyWaiters : 0.0,
        waitHist, addHist, convoyHist);
    TraceWrite(buf, len);

    char getHist[256];
    char suspendHist[256];
    int getPos = 0;
    int suspendPos = 0;
    for (int k = 0; k < 20; k++)
    {
        getPos += snprintf(getHist + getPos, sizeof(getHist) - getPos, k ? ",%zu" : "%zu", s_getRegionHist[k]);
        suspendPos += snprintf(suspendHist + suspendPos, sizeof(suspendHist) - suspendPos, k ? ",%zu" : "%zu", s_suspendHist[k]);
    }

    // The accumulated pause of blocking GCs. (GetGcAccumulatingDurationMillis returns usec)
    len = snprintf(buf, sizeof(buf),
        "[gcstats] t=%.3f ev=%s pauseUs1=%zu pauseUs2=%zu suspends=%zu suspendMs=%.3f suspendMaxUs=%.0f "
        "getRegionCalls=%zu getRegionMs=%.3f getRegionMaxUs=%.0f suspendHist=%s getRegionHist=%s\n",
        TraceTicksToUs(SatoriUtil::GetTimeStamp() - s_traceStartTs) / 1000.0, ev,
        m_heap->Recycler()->GetGcAccumulatingDurationMillis(1), m_heap->Recycler()->GetGcAccumulatingDurationMillis(2),
        g_suspendCount, TraceTicksToUs((int64_t)g_suspendTicks) / 1000.0, TraceTicksToUs((int64_t)g_suspendMaxTicks),
        g_getRegionCalls, TraceTicksToUs((int64_t)g_getRegionTicks) / 1000.0, TraceTicksToUs((int64_t)g_getRegionMaxTicks),
        suspendHist, getHist);
    TraceWrite(buf, len);

    size_t os[2][4] = {};
    size_t osHist[2][20] = {};
    for (int i = 0; i < 64; i++)
    {
        TraceOsShard* shards[2] = { &s_osCommit[i], &s_osDecommit[i] };
        for (int j = 0; j < 2; j++)
        {
            os[j][0] += shards[j]->calls;
            os[j][1] += shards[j]->bytes;
            os[j][2] += shards[j]->ticks;
            os[j][3] = max(os[j][3], shards[j]->maxTicks);
            for (int k = 0; k < 20; k++)
            {
                osHist[j][k] += shards[j]->hist[k];
            }
        }
    }

    char commitHist[256];
    char decommitHist[256];
    int commitPos = 0;
    int decommitPos = 0;
    for (int k = 0; k < 20; k++)
    {
        commitPos += snprintf(commitHist + commitPos, sizeof(commitHist) - commitPos, k ? ",%zu" : "%zu", osHist[0][k]);
        decommitPos += snprintf(decommitHist + decommitPos, sizeof(decommitHist) - decommitPos, k ? ",%zu" : "%zu", osHist[1][k]);
    }

    len = snprintf(buf, sizeof(buf),
        "[osstats] t=%.3f ev=%s commitCalls=%zu commitMB=%.1f commitMs=%.3f commitMaxUs=%.0f "
        "decommitCalls=%zu decommitMB=%.1f decommitMs=%.3f decommitMaxUs=%.0f commitHist=%s decommitHist=%s\n",
        TraceTicksToUs(SatoriUtil::GetTimeStamp() - s_traceStartTs) / 1000.0, ev,
        os[0][0], (double)os[0][1] / (1 << 20), TraceTicksToUs((int64_t)os[0][2]) / 1000.0, TraceTicksToUs((int64_t)os[0][3]),
        os[1][0], (double)os[1][1] / (1 << 20), TraceTicksToUs((int64_t)os[1][2]) / 1000.0, TraceTicksToUs((int64_t)os[1][3]),
        commitHist, decommitHist);
    TraceWrite(buf, len);

    len = snprintf(buf, sizeof(buf),
        "[nbrstats] t=%.3f ev=%s reqs=%zu withNext=%zu nextNoQueue=%zu nextReusable=%zu nextOther=%zu reusableTaken=%zu "
        "reusableHit=%zu adjTried=%zu adjClaimed=%zu getRegion=%zu\n",
        TraceTicksToUs(SatoriUtil::GetTimeStamp() - s_traceStartTs) / 1000.0, ev,
        g_nbrReqs, g_nbrWithNext, g_nbrNextNoQueue, g_nbrNextReusable, g_nbrNextOther, g_nbrReusableTaken,
        g_nbrReusableHit, SatoriTrace::s_counters.adjacentTries, SatoriTrace::s_counters.adjacentClaims, g_nbrGetRegion);
    TraceWrite(buf, len);

    len = snprintf(buf, sizeof(buf),
        "[trimstats] t=%.3f ev=%s roundsGen2=%zu roundsIdle=%zu calls=%zu granules=%zu MB=%.1f "
        "runHist=%zu,%zu,%zu,%zu,%zu,%zu skippedForReader=%zu skippedGranules=%zu\n",
        TraceTicksToUs(SatoriUtil::GetTimeStamp() - s_traceStartTs) / 1000.0, ev,
        g_trimRoundsGen2, g_trimRoundsIdle, g_trimCalls, g_trimCallGranules, (double)g_trimCallBytes / (1 << 20),
        g_trimRunHist[0], g_trimRunHist[1], g_trimRunHist[2], g_trimRunHist[3], g_trimRunHist[4], g_trimRunHist[5],
        g_trimSkippedForReader, g_trimSkippedGranules);
    TraceWrite(buf, len);

    if (SatoriUtil::IsConservativeMode())
    {
        len = snprintf(buf, sizeof(buf),
            "[parkstats] t=%.3f ev=%s parkedRegions=%zu parkedMB=%.1f maxCycleRegions=%zu maxCycleMB=%.1f maxSharePct=%.1f "
            "drains=%zu drainMsTotal=%.3f drainMsMax=%.3f\n",
            TraceTicksToUs(SatoriUtil::GetTimeStamp() - s_traceStartTs) / 1000.0, ev,
            g_parkRegions, (double)g_parkBytes / (1 << 20), g_parkMaxCycleRegions, (double)g_parkMaxCycleBytes / (1 << 20),
            (double)g_parkMaxSharePermille / 10.0, g_drainCount,
            TraceTicksToUs((int64_t)g_drainTicks) / 1000.0, TraceTicksToUs((int64_t)g_drainMaxTicks) / 1000.0);
        TraceWrite(buf, len);
    }

    if (g_verifyWaits)
    {
        len = snprintf(buf, sizeof(buf),
            "[verifywait] t=%.3f ev=%s waits=%zu over1ms=%zu totalMs=%.3f maxMs=%.3f\n",
            TraceTicksToUs(SatoriUtil::GetTimeStamp() - s_traceStartTs) / 1000.0, ev,
            g_verifyWaits, g_verifyWaitsOver1Ms, TraceTicksToUs((int64_t)g_verifyWaitTicks) / 1000.0, TraceTicksToUs((int64_t)g_verifyWaitMaxTicks) / 1000.0);
        TraceWrite(buf, len);
    }
}

void SatoriAllocator::TraceStateAtGen2End()
{
    if (g_traceOn)
    {
        TraceWriteState(false, "gen2");
    }
}

void SatoriAllocator::TraceAtExit()
{
    if (s_traceAllocator)
    {
        s_traceAllocator->TraceWriteState(true, "final");
    }
}
// TEMP-TRACE-END

// Zeroes [start + sizeof(size_t), end - sizeof(size_t)), but only the part of it in [dirtyStart, dirtyEnd).
// The rest is known to be zeroed already. (see SatoriRegion::AllocateDirty)
// NB: the first word is the MethodTable and the last one is the syncblock of whatever follows. Neither is zeroed here.
static void ZeroInitDirtyBody(size_t start, size_t end, size_t dirtyStart, size_t dirtyEnd)
{
    size_t zeroFrom = max(start + sizeof(size_t), dirtyStart);
    size_t zeroUpTo = min(end - sizeof(size_t), dirtyEnd);
    if (zeroUpTo > zeroFrom)
    {
        memset((void*)zeroFrom, 0, zeroUpTo - zeroFrom);
    }
}

// Where threads on different processors start their search for free granules.
static size_t ProcessorHint()
{
    if (GCToOSInterface::CanGetCurrentProcessorNumber())
    {
        // on SMT systems consecutive processors are the logical processors of one core, mix them in
        uint32_t processor = GCToOSInterface::GetCurrentProcessorNumber();
        return processor ^ (processor >> 1);
    }

    return (size_t)((SatoriUtil::GetCurrentThreadTag() * 11400714819323198485llu) >> 32);
}

void SatoriAllocator::Initialize(SatoriHeap* heap)
{
#if _DEBUG
    SatoriTestHooks::Init(); // TEST-HOOK
#endif
    m_heap = heap;
    TraceInit(this); // TEMP-TRACE

    int processorCount = (int)GCToOSInterface::GetTotalProcessorCount();

    m_workChunks = SatoriShardedWorkList::AllocAlignedForProcessors(4);

    m_immortalRegion = nullptr;
    m_immortalAllocLock.Initialize();

    m_pinnedRegion = nullptr;
    m_pinnedAllocLock.Initialize();

    m_workChunkRefillLock.Initialize();
    m_workChunkRefillShard = 0;

    m_runClaimLock.Initialize();

    m_regionCache = SatoriShardedRegionQueue::AllocAlignedForProcessors(QueueKind::Allocator, 4);
    m_regionCacheCount = 0;
    // TEMP-TRACE: the cap defaults to a couple of regions for every CPU that this process can use, and can be set to 0.
    int64_t configuredCap = GCConfig::GetRegionCacheCap();
    m_regionCacheCap = configuredCap >= 0 ?
        (size_t)configuredCap :
        max((size_t)4, 2 * (size_t)GCToEEInterface::GetCurrentProcessCpuCount());

    // The conservative mode parks the regions that are freed until the drain after a GC, see FreeToBitmap.
    m_parkedRegions = SatoriUtil::IsConservativeMode() ?
        SatoriRegionQueue::AllocAligned(QueueKind::Parked) :
        nullptr;

    m_largeRegion = nullptr;
    m_largeAllocLock.Initialize();

    m_regularRegion = nullptr;
    m_regularAllocLock.Initialize();

    m_pageAddLock.Initialize();
    m_pageAddsCompleted = 0;

    // As if the event was on before. If it is on from the start, there is nothing to catch up.
    // If it is off, the first check will notice that.
    m_allocationTickWasEnabled = true;

    int counterCount = (processorCount + Satori::ALLOCATED_BYTES_COUNTER_PROCESSORS - 1) / Satori::ALLOCATED_BYTES_COUNTER_PROCESSORS;
    m_smallAllocatedBytesCounterCount = max(1, min(counterCount, Satori::MAX_ALLOCATED_BYTES_COUNTERS));

    for (int i = 0; i < Satori::MAX_ALLOCATED_BYTES_COUNTERS; i++)
    {
        m_smallAllocatedBytes[i] = {};
    }

    m_largeAllocatedBytes = {};
    m_pinnedAllocatedBytes = {};
    m_deadThreadsAllocatedBytes = 0;
    m_lastPreciseAllocatedBytes = 0;
}

SatoriRegion* SatoriAllocator::GetRegion(size_t regionSize, SatoriRegionRequester requester)
{
    return GetRegionCore(regionSize, /* low */ false, requester);
}

// Same as GetRegion, but for the regions that are long-lived and rarely freed, like the ones for immortal allocations,
// work chunks or the finalization queue. Such regions are placed at the opposite end of a page from the regular ones,
// at the highest free granules, so that they do not fragment the space that the regular regions fill.
SatoriRegion* SatoriAllocator::GetRegionLow(size_t regionSize, SatoriRegionRequester requester)
{
    return GetRegionCore(regionSize, /* low */ true, requester);
}

SatoriRegion* SatoriAllocator::GetRegionCore(size_t regionSize, bool low, SatoriRegionRequester requester)
{
    if (!SatoriTrace::s_enabled)
    {
        return GetRegionImpl(regionSize, low, requester);
    }

    bool traceState = TraceStateDue(); // TEMP-TRACE
    int64_t start = SatoriUtil::GetTimeStamp();
    SatoriRegion* region = GetRegionImpl(regionSize, low, requester);
    int64_t getRegionTicks = SatoriUtil::GetTimeStamp() - start; // TEMP-TRACE
    SatoriTrace::CountTime(SatoriTrace::s_counters.getRegionTicks, getRegionTicks);
    TraceNoteGetRegion(getRegionTicks); // TEMP-TRACE
    TraceNoteRetryClaimed(); // TEMP-TRACE
    if (traceState) TraceWriteState(false, "tick"); // TEMP-TRACE
    return region;
}

// Gets a region of the given size from the free granules of the pages, and adds a page if there are none.
// A regular request first takes a region from the cache, and then claims the lowest free granules,
// the ones that are committed in full first. A request for a long-lived region takes the highest ones.
SatoriRegion* SatoriAllocator::GetRegionImpl(size_t regionSize, bool low, SatoriRegionRequester requester)
{
    _ASSERTE(regionSize % Satori::REGION_SIZE_GRANULARITY == 0);
    size_t granules = regionSize >> Satori::REGION_BITS;
    TracePageAddCtx traceCtx = {}; // TEMP-TRACE
    t_traceContention = 0; // TEMP-TRACE
    t_addWaitedTicks = 0; // TEMP-TRACE
    t_retryTs = 0; // TEMP-TRACE

    SatoriRegion* region;
    bool outOfMemory;

tryAgain:
    // The regular page adds that complete after this are not seen by the search for regions below. (see the add of a page)
    int32_t pageAddsCompleted = VolatileLoad(&m_pageAddsCompleted);
    if (granules == 1 && !low && m_regionCacheCap != 0)
    {
        region = m_regionCache->TryPop();
        if (region)
        {
            Interlocked::Decrement(&m_regionCacheCount);
            SatoriTrace::Count(SatoriTrace::s_counters.cacheHits);
            return region;
        }

        SatoriTrace::Count(SatoriTrace::s_counters.cacheMisses);
    }

    outOfMemory = false;
    region = TryClaimRegion(granules, low, outOfMemory);
    if (region)
    {
        return region;
    }

    if (outOfMemory)
    {
        goto failed;
    }

    TraceMiss(traceCtx); // TEMP-TRACE

    // for regions < 1/2 page granularity, just allocate a regular page
    // this is the most common case.
    // for bigger ones, use more complex helper that may allocate a larger
    // page, if needed.
    if (regionSize < Satori::PAGE_SIZE_GRANULARITY / 2)
    {
        // Adding a regular-sized Page.
        // We will often come here on multiple threads and we do not want all threads to add a page.
        // A page has regions for all of them, and an add takes as long for one thread as it does for many.
        // A thread that adds a page while another add is in flight gets a region no sooner than by waiting,
        // and the page that it adds is not needed. So the adds are one at a time, under a lock.
        // A thread that gets the lock after another one has added a page finds that the count of completed adds
        // is not the one that it saw before it looked for regions, and looks again.
        TraceBeforeAdd(traceCtx, regionSize); // TEMP-TRACE
        TraceEnterPageAddLock(&m_pageAddLock); // TEMP-TRACE (m_pageAddLock.Enter())
        if (VolatileLoad(&m_pageAddsCompleted) != pageAddsCompleted)
        {
            TraceNoteLockRetry(); // TEMP-TRACE
            m_pageAddLock.Leave();
            goto tryAgain;
        }

        SatoriPage* page = nullptr;
        TraceRegularAdd(traceCtx); // TEMP-TRACE
        if (m_heap->TryAddRegularPage(page))
        {
            if (page)
            {
                SatoriTrace::Count(SatoriTrace::s_counters.pageAdds[(int)requester]);
                TraceNoteAddPublished(traceCtx); // TEMP-TRACE

                // The page is ours. Claim from it right away, before the others take what we need.
                region = TryClaimRegionInPage(page, granules, low, outOfMemory);

                // The free granules of the page can be claimed, and we have ours. The waiters look for theirs when they see the change.
                Interlocked::Increment(&m_pageAddsCompleted);
                m_pageAddLock.Leave();
                TraceAfterAdd(traceCtx, page, regionSize, low, requester); // TEMP-TRACE
                if (region)
                {
                    return region;
                }

                if (outOfMemory)
                {
                    goto failed;
                }

                goto tryAgain;
            }

            // Someone added a page, but not us. There could be more regions.
            Interlocked::Increment(&m_pageAddsCompleted);
            m_pageAddLock.Leave();
            goto tryAgain;
        }

        m_pageAddLock.Leave();
    }
    else
    {
        // A region of half a page or more may need a larger page. It is claimed in the page before the page is published.
        TraceBeforeAdd(traceCtx, regionSize); // TEMP-TRACE
        SatoriPage* page = m_heap->AddLargePage(regionSize, low, region);
        if (page)
        {
            _ASSERTE(region);
            SatoriTrace::Count(SatoriTrace::s_counters.pageAdds[(int)requester]);
            TraceAfterAdd(traceCtx, page, regionSize, low, requester); // TEMP-TRACE
            return region;
        }
    }

    // Most likely out of memory, and the page-add lock is not held here. A request for one granule can still get a region
    // from the cache: it is formatted, and needs no commit. A long-lived region has not tried the cache before, and the regular one only did at the start.
failed:
    if (granules == 1)
    {
        region = TryGetCachedRegion();
        if (region)
        {
            return region;
        }
    }

    return nullptr;
}

// A region from the cache. It is formatted, so it needs no commit.
SatoriRegion* SatoriAllocator::TryGetCachedRegion()
{
    SatoriRegion* region = m_regionCache->TryPop();
    if (region)
    {
        Interlocked::Decrement(&m_regionCacheCount);
        SatoriTrace::Count(SatoriTrace::s_counters.cacheHits);
    }

    return region;
}

// The region that a claim got could not be committed, but that does not mean that nothing can be had: a region that is committed in full
// needs no commit. Looks for one in the order of the claims. The candidate that failed went back to its page as it was and is not warm.
// A granule or a run that is not committed in full after its claim goes back, and the search goes on: the trimmer can decommit a granule between
// the look at its warm bit and the claim, and then the trimmer has cleared the bit, so the granule is not found again.
SatoriRegion* SatoriAllocator::TryClaimWarmRegion(size_t granules, bool low)
{
    SatoriRegion* result = nullptr;
    if (granules > 1)
    {
        SatoriPage* claimedPage = nullptr;
        size_t first = SIZE_MAX;
        size_t rollbacks = 0;

        m_runClaimLock.Enter();
        m_heap->ForEachPageFrom(0,
            [&](SatoriPage* page)
            {
                while ((first = page->TryClaimRun(granules, /* fromTop */ low, rollbacks, /* warmOnly */ true)) != SIZE_MAX)
                {
                    if (page->IsCommittedInFull(first, granules))
                    {
                        claimedPage = page;
                        return true;
                    }

                    page->PublishFreeGranules(first, granules);
                }

                return false;
            }
        );
        m_runClaimLock.Leave();

        if (claimedPage)
        {
            result = claimedPage->FormatClaimed(first, granules);
        }
    }
    else
    {
        size_t retries = 0;
        size_t startHint = low ? 0 : ProcessorHint();
        m_heap->ForEachPageFrom(startHint,
            [&](SatoriPage* page)
            {
                size_t granule;
                while ((granule = page->TryClaimGranule(/* warmOnly */ true, /* fromTop */ low || g_claimTopDown, retries)) != SIZE_MAX)
                {
                    if (page->IsCommittedInFull(granule, 1))
                    {
                        result = page->FormatClaimed(granule, 1);
                        return result != nullptr;
                    }

                    page->PublishFreeGranules(granule, 1);
                }

                return false;
            }
        );
    }

    return result;
}

// Claims free granules for a region of the given number of granules, in the pages that we have.
// Returns nullptr if there is nothing free, or if what we claimed cannot be committed. (outOfMemory)
SatoriRegion* SatoriAllocator::TryClaimRegion(size_t granules, bool low, bool& outOfMemory)
{
#if _DEBUG
    if (SatoriTestHooks::t_claimMiss) // TEST-HOOK
    {
        return nullptr;
    }
#endif

    int64_t start = SatoriTrace::s_enabled ? SatoriUtil::GetTimeStamp() : 0;
    SatoriRegion* result = nullptr;
    size_t retries = 0;
    size_t rollbacks = 0;

    if (granules > 1)
    {
        // Runs are rare, all of them are for huge objects. The first page that has room is as good as any.
        SatoriPage* claimedPage = nullptr;
        size_t first = SIZE_MAX;

        m_runClaimLock.Enter();
        m_heap->ForEachPageFrom(0,
            [&](SatoriPage* page)
            {
                first = page->TryClaimRun(granules, /* fromTop */ low, rollbacks);
                if (first == SIZE_MAX)
                {
                    return false;
                }

                claimedPage = page;
                return true;
            }
        );
        m_runClaimLock.Leave();

        if (claimedPage)
        {
            result = claimedPage->FormatClaimed(first, granules);
            outOfMemory = (result == nullptr);
        }
    }
    else
    {
        // Threads on different processors start with different pages.
        // Requests for long-lived regions start with the first page, to keep those regions together.
        size_t startHint = low ? 0 : ProcessorHint();
        if (g_claimNoHint) // TEMP-TRACE
        { // TEMP-TRACE
            startHint = 0; // TEMP-TRACE
        } // TEMP-TRACE

        // Regular requests take a region that is committed in full, if there is one. Others take any.
        for (int pass = low ? 1 : 0; pass < 2 && !result && !outOfMemory; pass++)
        {
            bool warmOnly = pass == 0;
            m_heap->ForEachPageFrom(startHint,
                [&](SatoriPage* page)
                {
                    size_t granule = page->TryClaimGranule(warmOnly, /* fromTop */ low || g_claimTopDown, retries); // TEMP-TRACE
                    if (granule == SIZE_MAX)
                    {
                        return false;
                    }

                    result = page->FormatClaimed(granule, 1);
                    outOfMemory = (result == nullptr);
                    return true;
                }
            );
        }
    }

    if (outOfMemory)
    {
        // What was claimed first could not be committed, and a claim that fails to format ends the search above.
        // A region that is committed in full needs no commit, and there may be one.
        result = TryClaimWarmRegion(granules, low);
        outOfMemory = (result == nullptr);
    }

    TraceClaim(result, granules, retries, rollbacks, start);
    return result;
}

// The same as TryClaimRegion, but for one page.
SatoriRegion* SatoriAllocator::TryClaimRegionInPage(SatoriPage* page, size_t granules, bool low, bool& outOfMemory)
{
    int64_t start = SatoriTrace::s_enabled ? SatoriUtil::GetTimeStamp() : 0;
    size_t retries = 0;
    size_t rollbacks = 0;

    size_t first;
    if (granules > 1)
    {
        m_runClaimLock.Enter();
        first = page->TryClaimRun(granules, /* fromTop */ low, rollbacks);
        m_runClaimLock.Leave();
    }
    else
    {
        first = page->TryClaimGranule(/* warmOnly */ false, /* fromTop */ low || g_claimTopDown, retries); // TEMP-TRACE
    }

    SatoriRegion* result = nullptr;
    if (first != SIZE_MAX)
    {
        result = page->FormatClaimed(first, granules);
        outOfMemory = (result == nullptr);
    }

    TraceClaim(result, granules, retries, rollbacks, start);
    return result;
}

// TEMP-TRACE
void SatoriAllocator::TraceClaim(SatoriRegion* region, size_t granules, size_t retries, size_t rollbacks, int64_t startTime)
{
    if (!SatoriTrace::s_enabled)
    {
        return;
    }

    SatoriTraceCounters& counters = SatoriTrace::s_counters;
    SatoriTrace::Count(counters.casRetries, retries);
    SatoriTrace::Count(counters.runRollbacks, rollbacks);
    t_traceContention += retries + rollbacks;
    if (region)
    {
        SatoriTrace::Count(granules > 1 ? counters.claimsRun : counters.claimsSingle);
        int64_t ticks = SatoriUtil::GetTimeStamp() - startTime;
        SatoriTrace::CountTime(counters.claimTicks, ticks);

        // warm: the region is committed in full
        bool warm = region->CommittedEnd() >= region->End();
        SatoriTrace::CountTime(warm ? counters.claimWarmTicks : counters.claimColdTicks, ticks);
        SatoriTrace::Count(warm ? counters.claimWarmTotalTicks : counters.claimColdTotalTicks, (size_t)ticks);
    }
}

void SatoriAllocator::AddRegion(SatoriRegion* region)
{
    _ASSERTE(region->Generation() == -1);
    if (!TryCacheRegion(region, /* recentlyUsed */ false))
    {
        FreeToBitmap(region);
    }
}

// same as AddRegion, but this is for recently used regions
void SatoriAllocator::ReturnRegion(SatoriRegion* region)
{
    _ASSERTE(region->IsAttachedToAllocatingOwner() == false);
    _ASSERTE(region->Generation() == -1);
    if (!TryCacheRegion(region, /* recentlyUsed */ true))
    {
        FreeToBitmap(region);
    }
}

// Puts a 2 MB region in the cache, if the cache is not full. The regions that are used most recently go to the front.
bool SatoriAllocator::TryCacheRegion(SatoriRegion* region, bool recentlyUsed)
{
    if (region->Size() != Satori::REGION_SIZE_GRANULARITY)
    {
        return false;
    }

    if (m_regionCacheCount >= m_regionCacheCap)
    {
        SatoriTrace::Count(SatoriTrace::s_counters.cacheOverflows);
        return false;
    }

    // The count is ahead of the queue, so a pop never takes the count below 0.
    Interlocked::Increment(&m_regionCacheCount);
    SatoriTrace::Count(SatoriTrace::s_counters.freesToCache);
    if (recentlyUsed)
    {
        m_regionCache->Push(region);
    }
    else
    {
        m_regionCache->Enqueue(region);
    }

    return true;
}

// Frees the region into the free granules of its page.
// In the conservative mode the region is parked instead, see ParkRegion.
void SatoriAllocator::FreeToBitmap(SatoriRegion* region)
{
#if _DEBUG
    SatoriTestHooks::OnFreeToBitmap(); // TEST-HOOK
#endif

    if (SatoriTrace::s_enabled)
    {
        SatoriTrace::Count(SatoriTrace::s_counters.freesToBitmap);
        SatoriTrace::Count(SatoriTrace::s_counters.freesToBitmapGranules, region->Size() >> Satori::REGION_BITS);
    }

    if (SatoriUtil::IsConservativeMode())
    {
        ParkRegion(region);
        return;
    }

    region->ContainingPage()->FreeRegion(region);
}

// The conservative mode. A concurrent marker finds the region of a candidate pointer in the region map, and then reads its header.
// If the region is freed in between, its granules could be claimed and formatted again at the same address, or become a part of a bigger
// run that is claimed and decommitted, (the finalization queue does that when it grows) while the marker is still reading.
// So every free is parked: the region keeps its granules, its region map bytes and its committed header, and nothing can claim it.
// The GC frees the parked regions after it has marked, when no marker can be running. (see DrainParkedRegions)
// The cost is that what is freed cannot be used before the next GC. Only the conservative mode pays it.
NOINLINE void SatoriAllocator::ParkRegion(SatoriRegion* region)
{
    _ASSERTE(SatoriUtil::IsConservativeMode());
    _ASSERTE(region->Generation() == -1);

    SatoriTrace::Count(g_parkCycleRegions); // TEMP-TRACE
    SatoriTrace::Count(g_parkCycleBytes, region->Size()); // TEMP-TRACE
    SatoriTrace::Count(g_parkCycleCommitted, region->CommittedEnd() - region->Start()); // TEMP-TRACE

    m_parkedRegions->Enqueue(region);
}

// Frees the regions that were parked. Called by a GC after Update, with the EE stopped, when every concurrent marker is done:
// no marker has a region that it found earlier, and the regions that this frees can be claimed again only after the EE restarts.
void SatoriAllocator::DrainParkedRegions()
{
    _ASSERTE(SatoriUtil::IsConservativeMode());

    int64_t start = 0; // TEMP-TRACE
    if (SatoriTrace::s_enabled) // TEMP-TRACE
    { // TEMP-TRACE
        start = SatoriUtil::GetTimeStamp(); // TEMP-TRACE
        size_t regions = g_parkCycleRegions; // TEMP-TRACE
        size_t bytes = g_parkCycleBytes; // TEMP-TRACE
        size_t committed = g_parkCycleCommitted; // TEMP-TRACE
        g_parkCycleRegions = g_parkCycleBytes = g_parkCycleCommitted = 0; // TEMP-TRACE
        g_parkRegions += regions; // TEMP-TRACE
        g_parkBytes += bytes; // TEMP-TRACE
        g_parkMaxCycleRegions = max(g_parkMaxCycleRegions, regions); // TEMP-TRACE
        g_parkMaxCycleBytes = max(g_parkMaxCycleBytes, bytes); // TEMP-TRACE
        size_t heapCommit = m_heap->GetBytesCommitted(); // TEMP-TRACE
        if (heapCommit) // TEMP-TRACE
        { // TEMP-TRACE
            g_parkMaxSharePermille = max(g_parkMaxSharePermille, committed * 1000 / heapCommit); // TEMP-TRACE
        } // TEMP-TRACE
    } // TEMP-TRACE

#if _DEBUG
    SatoriTestHooks::t_inParkDrain = true; // TEST-HOOK
#endif

    while (SatoriRegion* region = m_parkedRegions->TryPop())
    {
        region->ContainingPage()->FreeRegion(region);
    }

#if _DEBUG
    SatoriTestHooks::t_inParkDrain = false; // TEST-HOOK
#endif

    if (SatoriTrace::s_enabled) // TEMP-TRACE
    { // TEMP-TRACE
        size_t ticks = (size_t)(SatoriUtil::GetTimeStamp() - start); // TEMP-TRACE
        g_drainCount++; // TEMP-TRACE
        g_drainTicks += ticks; // TEMP-TRACE
        g_drainMaxTicks = max(g_drainMaxTicks, ticks); // TEMP-TRACE
    } // TEMP-TRACE
}

void SatoriAllocator::DrainRegionCache()
{
    while (SatoriRegion* region = m_regionCache->TryPop())
    {
        Interlocked::Decrement(&m_regionCacheCount);
        FreeToBitmap(region);
    }
}

#if _DEBUG
// TEST-HOOK
bool SatoriAllocator::TestCacheCountIs(size_t count)
{
    return m_regionCacheCount == count && m_regionCache->Count() == count;
}

// TEST-HOOK
bool SatoriAllocator::TestPageAddLockIsFree()
{
    if (!m_pageAddLock.TryEnter())
    {
        return false;
    }

    m_pageAddLock.Leave();
    return true;
}
#endif

// TEMP-TRACE: what the allocator of regions has counted, and the layout of the pages.
void SatoriAllocator::PrintTrace()
{
    SatoriTraceCounters& c = SatoriTrace::s_counters;
    static const char requesterTags[] = "?RLSHPIWFT";
    static_assert(sizeof(requesterTags) == (int)SatoriRegionRequester::Count + 1, "a tag for every requester");

    size_t totalPageAdds = 0;
    for (int i = 0; i < (int)SatoriRegionRequester::Count; i++)
    {
        totalPageAdds += c.pageAdds[i];
    }

    double microsPerTick = 1000000.0 / (double)SatoriUtil::GetTimeStampFrequency();
    size_t claimCount, getCount;
    int64_t claimP50 = SatoriTrace::Percentile(c.claimTicks, 0.50, claimCount);
    int64_t claimP99 = SatoriTrace::Percentile(c.claimTicks, 0.99, claimCount);
    int64_t getP50 = SatoriTrace::Percentile(c.getRegionTicks, 0.50, getCount);
    int64_t getP99 = SatoriTrace::Percentile(c.getRegionTicks, 0.99, getCount);

    minipal_log_print_error("Satori TEMP-TRACE allocator counters (cache cap %zu):\n", m_regionCacheCap);
    minipal_log_print_error("  page adds: total=%zu by requester:", totalPageAdds);
    for (int i = 0; i < (int)SatoriRegionRequester::Count; i++)
    {
        if (c.pageAdds[i])
        {
            minipal_log_print_error(" %c=%zu", requesterTags[i], c.pageAdds[i]);
        }
    }

    minipal_log_print_error("\n");
    minipal_log_print_error("  claims: single=%zu run=%zu  cas retries=%zu  run rollbacks=%zu\n",
        c.claimsSingle, c.claimsRun, c.casRetries, c.runRollbacks);
    minipal_log_print_error("  adjacent claims: tried=%zu claimed=%zu\n", c.adjacentTries, c.adjacentClaims);
    minipal_log_print_error("  cache: hits=%zu misses=%zu frees=%zu overflows to bitmap=%zu (in cache now %zu)\n",
        c.cacheHits, c.cacheMisses, c.freesToCache, c.cacheOverflows, (size_t)m_regionCacheCount);
    minipal_log_print_error("  frees into bitmap: regions=%zu granules=%zu  gap commit bytes=%zu\n",
        c.freesToBitmap, c.freesToBitmapGranules, c.gapCommitBytes);
    minipal_log_print_error("  claim time (us): p50=%.2f p99=%.2f of %zu claims\n",
        claimP50 * microsPerTick, claimP99 * microsPerTick, claimCount);
    minipal_log_print_error("  GetRegion time (us): p50=%.2f p99=%.2f of %zu calls\n",
        getP50 * microsPerTick, getP99 * microsPerTick, getCount);
}

// Allocated bytes accounting.
//
// Threads count their allocations in their allocation contexts and publish the counts to the shared
// counters in batches, once per PACE_BUDGET or so - the same granularity as pacing.
// That happens after allocations that are not satisfied from the allocation context, and when a thread
// terminates. Thus the shared counters are kept current without relying on GCs, and the cost of
// updating them is amortized over many allocations.
//
// Each kind of allocations (small, large, pinned) is published on its own, by allocations of that kind.
// Allocation ticks are fired when publishing and must report an object of the published kind. If one kind
// published for another, a kind that is always published along with another one would never tick.
//
// The approximate total is the sum of the shared counters, or the last precise total, whichever is greater.
// It only grows, but can lag behind live allocations by up to PACE_BUDGET or so per thread and kind.
// NB: the precise total does not use the shared counters. With EE suspended, it sums up the counts in allocation
//     contexts of live threads and adds what terminated threads have allocated. (see GetTotalAllocatedBytesPrecise)
//
// Allocation tick events are driven by the same counters. (see MaybeFireAllocationTick)

// What the current thread has allocated, but not yet published.
thread_local
struct UnpublishedAllocatedBytes
{
    // Small objects are counted in the allocation context. This is how much of that is already published.
    int64_t smallPublished;

    // Large and pinned objects that are not published yet.
    // NB: the allocation context counts them together in alloc_bytes_uoh, but we want them apart.
    size_t large;
    size_t pinned;
} t_unpublishedAllocatedBytes;

// Bytes taken by small objects allocated in the context.
// NB: alloc_bytes also includes the unused remainder of the current allocation buffer, which is not counted here.
static int64_t SmallAllocatedBytes(SatoriAllocationContext* context)
{
    return context->alloc_bytes - (context->alloc_limit - context->alloc_ptr);
}

// All bytes allocated in the context, precisely. (same as GC.GetAllocatedBytesForCurrentThread)
static int64_t AllocatedBytes(SatoriAllocationContext* context)
{
    return SmallAllocatedBytes(context) + context->alloc_bytes_uoh;
}

// The counter for the small objects allocated on the current processor.
int SatoriAllocator::SmallAllocatedBytesCounterIndex()
{
    int counterCount = m_smallAllocatedBytesCounterCount;
    if (counterCount == 1)
    {
        return 0;
    }

    uint32_t processor;
    if (GCToOSInterface::CanGetCurrentProcessorNumber())
    {
        processor = GCToOSInterface::GetCurrentProcessorNumber();
    }
    else
    {
        // no processor number, just spread the threads
        size_t thread = SatoriUtil::GetCurrentThreadTag();
        processor = (uint32_t)((thread * 11400714819323198485llu) >> 32);
    }

    return (int)((processor / Satori::ALLOCATED_BYTES_COUNTER_PROCESSORS) % (uint32_t)counterCount);
}

size_t SatoriAllocator::GetTotalAllocatedBytes()
{
    size_t total =
        VolatileLoadWithoutBarrier(&m_largeAllocatedBytes.bytes) +
        VolatileLoadWithoutBarrier(&m_pinnedAllocatedBytes.bytes);

    for (int i = 0; i < m_smallAllocatedBytesCounterCount; i++)
    {
        total += VolatileLoadWithoutBarrier(&m_smallAllocatedBytes[i].bytes);
    }

    return max(total, VolatileLoadWithoutBarrier(&m_lastPreciseAllocatedBytes));
}

static void AddAllocatedBytesFn(gc_alloc_context* gcContext, void* param)
{
    *(size_t*)param += (size_t)AllocatedBytes((SatoriAllocationContext*)gcContext);
}

// The precise total. EE must be suspended, so that the counts in allocation contexts do not change.
// What terminated threads have allocated is not in any context. (see PublishAllocatedBytesOnThreadExit)
size_t SatoriAllocator::GetTotalAllocatedBytesPrecise()
{
    size_t total = m_deadThreadsAllocatedBytes;
    GCToEEInterface::GcEnumAllocContexts(AddAllocatedBytesFn, &total);
    VolatileStore(&m_lastPreciseAllocatedBytes, total);
    return total;
}

// Moves the last tick of the counter up to ALLOCATION_TICK_BYTES behind the counter, if it is further behind.
// Ticks may happen concurrently, so this only moves the last tick forward.
/* static */
void SatoriAllocator::CatchUpAllocationTick(AllocatedBytesCounter* counter)
{
    size_t bytes = VolatileLoadWithoutBarrier(&counter->bytes);
    size_t target = bytes > Satori::ALLOCATION_TICK_BYTES ? bytes - Satori::ALLOCATION_TICK_BYTES : 0;
    size_t atLastTick = VolatileLoadWithoutBarrier(&counter->bytesAtLastTick);
    while ((ptrdiff_t)(target - atLastTick) > 0)
    {
        size_t observed = Interlocked::CompareExchange(&counter->bytesAtLastTick, target, atLastTick);
        if (observed == atLastTick)
        {
            break;
        }

        atLastTick = observed;
    }
}

// Tells if the allocation tick event is enabled.
// While the event is off, the last ticks of the counters are not maintained, so when we see that it was
// turned on, we bring them up to date - as if the event was on for the last ALLOCATION_TICK_BYTES or so.
// NB: this must be called after publishing. Then, if a thread sees the event on, what it has just published
//     is either from the time the event was on, or the last ticks are caught up with it.
//     Checking before publishing could report bytes allocated while the event was off.
bool SatoriAllocator::IsAllocationTickEnabled()
{
#ifdef FEATURE_EVENT_TRACE
    bool isEnabled = EVENT_ENABLED(GCAllocationTick_V4);
    if (isEnabled != m_allocationTickWasEnabled)
    {
        if (isEnabled)
        {
            for (int i = 0; i < m_smallAllocatedBytesCounterCount; i++)
            {
                CatchUpAllocationTick(&m_smallAllocatedBytes[i]);
            }

            CatchUpAllocationTick(&m_largeAllocatedBytes);
            CatchUpAllocationTick(&m_pinnedAllocatedBytes);
        }

        m_allocationTickWasEnabled = isEnabled;
    }

    return isEnabled;
#else
    return false;
#endif
}

// Fires an allocation tick event if ALLOCATION_TICK_BYTES or more were published to the counter since the last tick.
// The event reports the bytes published to the counter since the last tick and the object that was just allocated.
// "total" is the value of the counter right after the current thread published to it. That includes the object.
/* static */
void SatoriAllocator::MaybeFireAllocationTick(AllocatedBytesCounter* counter, size_t total, AllocationTickKind kind, SatoriObject* obj, size_t size)
{
    size_t atLastTick = VolatileLoadWithoutBarrier(&counter->bytesAtLastTick);

    // Other threads may publish and tick after we have published, then atLastTick is ahead of our total and
    // what we have published is already reported. The comparison is signed, so that such case does not look
    // like a large amount.
    ptrdiff_t sinceLastTick = (ptrdiff_t)(total - atLastTick);
    if (sinceLastTick <= 0)
    {
        return;
    }

    // What we have just published includes the object, so the bytes since the last tick are normally at least
    // the size of the object. They can be less only if catching up after the event was turned on moved the last
    // tick past some of our bytes. (see IsAllocationTickEnabled) That should not discard the object that is
    // being reported, thus we report at least its size. Nothing is reported twice - catching up reports nothing.
    size_t amount = max((size_t)sinceLastTick, size);
    if (amount >= Satori::ALLOCATION_TICK_BYTES &&
        Interlocked::CompareExchange(&counter->bytesAtLastTick, total, atLastTick) == atLastTick)
    {
        FIRE_EVENT(GCAllocationTick_V4,
            amount,
            /*kind*/ (uint32_t)kind,
            /*heap_number*/ 0,
            (void*)obj,
            size);
    }
}

// What the current thread has allocated of the given kind, but has not published yet.
static size_t UnpublishedBytes(SatoriAllocationContext* context, AllocationTickKind kind)
{
    UnpublishedAllocatedBytes& unpublished = t_unpublishedAllocatedBytes;
    switch (kind)
    {
    case AllocationTickKind::Small:
    {
        int64_t smallAllocated = SmallAllocatedBytes(context);
        _ASSERTE(smallAllocated >= unpublished.smallPublished);
        return (size_t)(smallAllocated - unpublished.smallPublished);
    }
    case AllocationTickKind::Large:
        return unpublished.large;
    default:
        return unpublished.pinned;
    }
}

// Publishes what the current thread has allocated of the given kind since the last time.
// "obj" is the object of that kind that was just allocated. It is reported if an allocation tick is due.
// If "obj" is null, no ticks are fired.
void SatoriAllocator::PublishAllocatedBytes(SatoriAllocationContext* context, AllocationTickKind kind, SatoriObject* obj, size_t size)
{
    size_t bytes = UnpublishedBytes(context, kind);
    if (bytes == 0)
    {
        return;
    }

    UnpublishedAllocatedBytes& unpublished = t_unpublishedAllocatedBytes;
    AllocatedBytesCounter* counter;
    switch (kind)
    {
    case AllocationTickKind::Small:
        unpublished.smallPublished += (int64_t)bytes;
        counter = &m_smallAllocatedBytes[SmallAllocatedBytesCounterIndex()];
        break;
    case AllocationTickKind::Large:
        unpublished.large = 0;
        counter = &m_largeAllocatedBytes;
        break;
    default:
        unpublished.pinned = 0;
        counter = &m_pinnedAllocatedBytes;
        break;
    }

    size_t total = Interlocked::ExchangeAdd64(&counter->bytes, bytes) + bytes;

    // NB: the event is checked after publishing. (see IsAllocationTickEnabled)
    if (IsAllocationTickEnabled() && obj != nullptr)
    {
        MaybeFireAllocationTick(counter, total, kind, obj, size);
    }
}

// Publishes what the current thread has allocated of the given kind, if enough has accumulated since the last time.
// Called after allocations that are not satisfied from the allocation context.
// "obj" is the object of the given kind that was just allocated. It is reported if an allocation tick is due.
void SatoriAllocator::PublishAllocatedBytesIfNeeded(SatoriAllocationContext* context, AllocationTickKind kind, SatoriObject* obj, size_t size)
{
    // same granularity as pacing.
    if (UnpublishedBytes(context, kind) >= Satori::PACE_BUDGET)
    {
        PublishAllocatedBytes(context, kind, obj, size);
    }
}

// Accounts for a large or pinned object that the current thread has just allocated.
void SatoriAllocator::OnLargeOrPinnedAllocated(SatoriAllocationContext* context, uint32_t flags, SatoriObject* obj, size_t size)
{
    context->alloc_bytes_uoh += size;

    AllocationTickKind kind;
    // immortal objects do not move either, we count them as pinned.
    if (flags & (GC_ALLOC_PINNED_OBJECT_HEAP | GC_ALLOC_IMMORTAL))
    {
        kind = AllocationTickKind::Pinned;
        t_unpublishedAllocatedBytes.pinned += size;
    }
    else
    {
        kind = AllocationTickKind::Large;
        t_unpublishedAllocatedBytes.large += size;
    }

    PublishAllocatedBytesIfNeeded(context, kind, obj, size);
}

// The current thread is terminating and its allocation context is about to be cleared.
// Publishes all that is not published yet, moves the precise count from the context to the total
// for terminated threads, and resets the accounting to match the cleared context.
// NB: this runs on the terminating thread itself, after its allocation context is deactivated.
void SatoriAllocator::PublishAllocatedBytesOnThreadExit(SatoriAllocationContext* context)
{
    PublishAllocatedBytes(context, AllocationTickKind::Small, /*obj*/ nullptr, /*size*/ 0);
    PublishAllocatedBytes(context, AllocationTickKind::Large, /*obj*/ nullptr, /*size*/ 0);
    PublishAllocatedBytes(context, AllocationTickKind::Pinned, /*obj*/ nullptr, /*size*/ 0);
    t_unpublishedAllocatedBytes = {};

    // The runtime calls this when EE cannot be suspended (in cooperative mode or holding the thread store lock),
    // so GetTotalAllocatedBytesPrecise sees the count either in the context or in the total, never in both.
    // Other threads may be terminating concurrently though.
    Interlocked::ExchangeAdd64(&m_deadThreadsAllocatedBytes, (size_t)AllocatedBytes(context));
    context->alloc_bytes = 0;
    context->alloc_bytes_uoh = 0;
}

Object* SatoriAllocator::Alloc(SatoriAllocationContext* context, size_t size, uint32_t flags)
{
    size = ALIGN_UP(size, Satori::OBJECT_ALIGNMENT);

    if ((flags & (GC_ALLOC_IMMORTAL | GC_ALLOC_PINNED_OBJECT_HEAP | GC_ALLOC_LARGE_OBJECT_HEAP)) == 0)
    {
        if (context->alloc_ptr + size <= context->alloc_limit)
        {
            SatoriObject* result = (SatoriObject*)context->alloc_ptr;
            if ((flags & GC_ALLOC_FINALIZE) && !result->ContainingRegion()->RegisterForFinalization(result))
            {
                return nullptr;
            }

            context->alloc_ptr += size;
            return result;
        }

        if (size < Satori::LARGE_OBJECT_THRESHOLD)
        {
            return AllocRegular(context, size, flags);
        }

        flags |= GC_ALLOC_LARGE_OBJECT_HEAP;
    }

    if (flags & GC_ALLOC_PINNED_OBJECT_HEAP)
    {
        SatoriObject* result = AllocPinned(context, size, flags);
        if (result != nullptr)
        {
            result->SetUnmovable();
        }

        return result;
    }

    if (flags & GC_ALLOC_LARGE_OBJECT_HEAP)
    {
        return AllocLarge(context, size, flags);
    }

    if (flags & GC_ALLOC_IMMORTAL)
    {
        return AllocImmortal(context, size, flags);
    }

    return nullptr;
}

#ifdef _DEBUG
// allow more frequent allocs from shared in debug
const size_t minSharedAllocDelay = 5;
#else
// This is how often would we want to take a lock in usecs for 2k or more.
// If we come for more sooner, arrange an own region.
// Shared is for slow allocators. This is basically the threshold for slow.
const size_t minSharedAllocDelay = 50;
#endif

thread_local
size_t t_lastAllocBytes;

thread_local
size_t t_lastAllocBytesAtGCcheck;

thread_local
struct AllocStats
{
    size_t lastRegularAllocBytes;
    size_t lastAllocRecordUsec;
    // in byte/microsecond, same as MB/sec
    size_t regularAllocRate;
} t_allocStats;

void SatoriAllocator::UpdateAllocStatsAndHelpIfNeeded(SatoriAllocationContext* context)
{
    size_t curAllocBytes = context->alloc_bytes + context->alloc_bytes_uoh;
    if (curAllocBytes == 0)
    {
        m_heap->Recycler()->MaybeTriggerGC(gc_reason::reason_alloc_soh);
        return;
    }

    size_t lastAllocBytes = t_lastAllocBytes;
    // sometimes we release bytes back, wait until we go forward.
    if (curAllocBytes < lastAllocBytes)
        return;

    size_t change = curAllocBytes - lastAllocBytes;
    if (change < Satori::PACE_BUDGET)
    {
        // too soon. 
        return;
    }

    if (curAllocBytes - t_lastAllocBytesAtGCcheck >= Satori::REGION_SIZE_GRANULARITY)
    {
        m_heap->Recycler()->MaybeTriggerGC(gc_reason::reason_alloc_soh);
        t_lastAllocBytesAtGCcheck = curAllocBytes;
    }
    else
    {
        m_heap->Recycler()->HelpOnce();
    }

    t_lastAllocBytes = curAllocBytes;

    // This is after possible helping or doing GC, but since this is sampling
    // of overal alloc rate it does not matter if we take time before or after.
    size_t curUsec = m_heap->Recycler()->GetNowUsecs();
    size_t curRegularAllocBytes = context->alloc_bytes;
    if (curRegularAllocBytes > t_allocStats.lastRegularAllocBytes &&
        curUsec > t_allocStats.lastAllocRecordUsec)
    {
        t_allocStats.regularAllocRate =
            (curRegularAllocBytes - t_allocStats.lastRegularAllocBytes) /
            (curUsec - t_allocStats.lastAllocRecordUsec);
        t_allocStats.lastRegularAllocBytes = curRegularAllocBytes;
        t_allocStats.lastAllocRecordUsec = curUsec; 
    }
}

SatoriObject* SatoriAllocator::AllocRegular(SatoriAllocationContext* context, size_t size, uint32_t flags)
{
    // when allocations cross certain thresholds, check if GC should start or help is needed.
    UpdateAllocStatsAndHelpIfNeeded(context);

    // The region we have just given up, if any. Used as a hint when getting the next one.
    SatoriRegion* prevRegion = nullptr;

tryAgain: 

    if (!context->RegularRegion())
    {
        // If was allocating from shared, close the alloc.
        // If we continue from the same shared region, we will return the remainder, once we have the lock.
        // If not we will just leave the free obj unreturned. It is probably too small anyways.
        SatoriObject* freeObj = context->alloc_ptr != 0 ?
            context->FinishAllocFromShared() : nullptr;

        // TUNING: may be 10 or 50 ? Is it important?
        // we will consider 100+ MB/s as fast-allocating and try getting a non-shared region
        if (t_allocStats.regularAllocRate < 100)
        {
            //m_regularAllocLock.Enter();
            if (m_regularAllocLock.TryEnter())
            {
                if (freeObj && freeObj->ContainingRegion() == m_regularRegion)
                {
                    size_t freeSize = freeObj->FreeObjSize();
                    m_regularRegion->SetOccupancy(m_regularRegion->Occupancy() - freeSize);
                    m_regularRegion->ReturnFreeSpace(freeObj, freeSize);
                }

                return AllocRegularShared(context, size, flags);
            }
        }
    }

    SatoriRegion* region = context->RegularRegion();
    _ASSERTE(region == nullptr || region->IsAttachedToAllocatingOwner());

    if (region != nullptr)
    {
        // if a GC took the region while it stayed attached, start a new accounting period.
        region->RebaselineUsableIfStale(context->alloc_bytes);

        if (region->ClearStaleEscapeMarks())
        {
            // the barrier stopped tracking because of too many escapes.
            SatoriRegion::OnEscapeTrackingEnded();
        }
    }

    while (true)
    {
        if (region != nullptr)
        {
            _ASSERTE((size_t)context->alloc_limit == region->GetAllocStart());
            size_t moreSpace = context->alloc_ptr + size - context->alloc_limit;

            // try allocate more to form a contiguous chunk to fit size
            size_t allocRemaining = region->GetTlgcAllocRemaining();
            if (moreSpace <= allocRemaining)
            {
                bool zeroInitialize = !(flags & GC_ALLOC_ZEROING_OPTIONAL);
                if (zeroInitialize)
                {
                    // Take more than needed, so that we come here less often, but zero ahead at most MinZeroInitSize,
                    // so that the zeroed memory is still in cache when we use it and not much is wasted if we stop
                    // allocating here before using it all. (the unused part goes back to the free list with a note
                    // that it is zeroed, but that is lost if the region is swept before the space is reused)
                    // With MinZeroInitSize or more remaining, this takes MinZeroInitSize. Closer to the end of the span,
                    // it takes the largest power of two that fits, to use up the rest in a few trips.
                    // These are multiples of INDEX_GRANULARITY, so alloc_limit stays misaligned as described below.
                    if (allocRemaining >= Satori::INDEX_GRANULARITY)
                    {
                        size_t quantum = min(SatoriUtil::MinZeroInitSize(), SatoriUtil::RoundDownPwr2(allocRemaining));
                        moreSpace = max(moreSpace, quantum);
                    }
                    else
                    {
                        // The tail of the span is smaller than that. Taking just what is needed would send every
                        // allocation in the tail here again, so take all of it, as long as the object would fit in it
                        // by itself. When it would not (the object hangs over from the current allocation context),
                        // what is left after it is unlikely to fit the next object of a similar size, and zeroing it
                        // would likely be wasted. (it is also too small for a free list)
                        if (size <= allocRemaining)
                        {
                            moreSpace = allocRemaining;
                        }
                    }

                    // What is already known to be zeroed costs nothing to take.
                    // In fresh memory that can be the rest of the region, so take at most PACE_BUDGET / 2 (64 KB):
                    // the fast-path array allocator assumes the allocation context never exceeds LARGE_OBJECT_SIZE,
                    // and allocation pacing runs when we come here.
                    static_assert(Satori::PACE_BUDGET / 2 + Satori::INDEX_GRANULARITY < LARGE_OBJECT_SIZE);
                    size_t zeroedRemaining = min(region->GetAllocZeroedRemaining(), (size_t)(Satori::PACE_BUDGET / 2));
                    moreSpace = max(moreSpace, min(zeroedRemaining, allocRemaining));

                    // " +/- sizeof(size_t)" here is to intentionally misalign alloc_limit on the index granularity
                    // to improve chances that the object that is allocated here will be indexed
                    size_t misAlignedOnIndexEnd = ALIGN_UP(region->GetAllocStart() + moreSpace + sizeof(size_t), Satori::INDEX_GRANULARITY) - sizeof(size_t);
                    size_t misAlignedMoreSpace = misAlignedOnIndexEnd - region->GetAllocStart();

                    if (misAlignedMoreSpace <= allocRemaining)
                    {
                        moreSpace = misAlignedMoreSpace;
                    }
                }

                if (region->Allocate(moreSpace, zeroInitialize))
                {
                    context->alloc_limit += moreSpace;
                    context->alloc_bytes += moreSpace;

                    SatoriObject* result = (SatoriObject*)(size_t)context->alloc_ptr;
                    if ((flags & GC_ALLOC_FINALIZE) &&
                        !region->RegisterForFinalization(result))
                    {
                        return nullptr;
                    }

                    context->alloc_ptr += size;
                    _ASSERTE((size_t)(context->alloc_limit - context->alloc_ptr) < LARGE_OBJECT_SIZE);
                    result->CleanSyncBlock();
                    region->SetIndicesForObject(result, result->Start() + size);

                    PublishAllocatedBytesIfNeeded(context, AllocationTickKind::Small, result, size);
                    return result;
                }
                else
                {
                    //OOM
                    return nullptr;
                }
            }

            // unclaim unused.
            size_t unused = context->alloc_limit - context->alloc_ptr;
            context->alloc_bytes -= unused;

            bool collectAtTlgcLimit = region->ShouldCollectAtTlgcLimit(context->alloc_bytes);

            if (region->IsAllocating())
            {
                // the unused part of the allocation context is zeroed.
                region->StopAllocating((size_t)context->alloc_ptr, (size_t)context->alloc_limit);
            }
            else
            {
                _ASSERTE(context->alloc_ptr == nullptr);
            }

            if (collectAtTlgcLimit)
            {
                if (!region->ThreadLocalCollect(context->alloc_bytes, /* atTlgcLimit */ true))
                {
                    SatoriRegion::OnEscapeTrackingEnded();
                    region->StopEscapeTracking();
                }
            }

            // try get from the free list
            if (region->StartAllocating(size, context->alloc_bytes))
            {
                // we have enough free space in the region to continue
                context->alloc_ptr = context->alloc_limit = (uint8_t*)region->GetAllocStart();
                continue;
            }

            if (region->IsEscapeTracking())
            {
                // try performing thread local collection and see if we have enough space after that.
                if (region->ThreadLocalCollect(context->alloc_bytes))
                {
                    if (region->StartAllocating(size, context->alloc_bytes))
                    {
                        // we have enough free space in the region to continue
                        context->alloc_ptr = context->alloc_limit = (uint8_t*)region->GetAllocStart();
                        continue;
                    }
                }
                else
                {
                    SatoriRegion::OnEscapeTrackingEnded(/* outOfSpace */ true);
                    region->StopEscapeTracking();
                }
            }

            // A pointer-free medium object that does not fit, while the region still has a lot of space in smaller
            // holes. Retiring the region would waste that space (and charge for it), so serve this object from the
            // large allocator instead and keep the region for the smaller allocations.
            // Pointer-free, so the object does not care about the region and its escape tracking. Finalizable
            // objects are registered with their region, so they stay out of this.
            // Only 8 KB or more: the large allocator serves each object individually, without an allocation context,
            // which is reasonable for objects that are a sizable fraction of LARGE_OBJECT_THRESHOLD, but too costly
            // per byte for smaller ones. Those retire the region as before and continue in another one.
            if (size >= 8 * 1024 &&
                !(flags & (GC_ALLOC_CONTAINS_REF | GC_ALLOC_FINALIZE)) &&
                region->UsableCapacity() > Satori::REGION_SIZE_GRANULARITY / 32)
            {
                // the region stays attached, but not allocating.
                context->alloc_ptr = context->alloc_limit = nullptr;
                return AllocLarge(context, size, flags | GC_ALLOC_LARGE_OBJECT_HEAP);
            }

            context->alloc_ptr = context->alloc_limit = nullptr;
            if (region->IsEscapeTracking())
            {
                // the region ran out of space while tracking.
                SatoriRegion::OnEscapeTrackingEnded(/* outOfSpace */ true);
            }

            region->DetachFromAlocatingOwnerRelease();
            m_heap->Recycler()->AddEphemeralRegion(region);

            // could not allocate, start over with trying to get a region
            prevRegion = region;
            goto tryAgain;
        }

        // Prefer a neighbor of the region we have just given up. (see TryGetRegularRegion)
        // The previous region is detached and owned by the recycler now, but, same as in AllocRegularShared,
        // we only use it to look at its neighbor in the page.
        _ASSERTE(region == nullptr);
        region = prevRegion;
        TryGetRegularRegion(region, size);

        if (region == nullptr)
        {
            //OOM
            return nullptr;
        }

        // the order of assignments:
        // 1) Attach  (optional: set escape tag)
        // 2) Set Generation to 0 or 1
        // 3) Reset ReusableFor
        // <objects allocated>
        // 4) (optional: clear escape tag) Detach       

        region->AttachToAllocatingOwner(&context->RegularRegion());
        if (SatoriUtil::IsGen0Enabled())
        {
            switch (region->ReusableFor())
            {
            case SatoriRegion::ReuseLevel::Gen0:
                if (!SatoriRegion::ShouldStartEscapeTracking())
                {
                    region->SetGenerationRelease(1);
                    break;
                }

                region->EscapeAll();
                goto fallthrough;
            case SatoriRegion::ReuseLevel::None:
                if (!SatoriRegion::ShouldStartEscapeTracking())
                {
                    region->SetGenerationRelease(1);
                    break;
                }

            fallthrough:
                //NB: sets Generation to 0
                region->StartEscapeTrackingRelease(SatoriUtil::GetCurrentThreadTag(), context->alloc_bytes, size);
                break;
            case SatoriRegion::ReuseLevel::Gen1:
                region->SetGenerationRelease(1);
                break;
            }
        }
        else
        {
            region->SetGenerationRelease(1);
        }

        region->ResetReusableForRelease();
        context->alloc_ptr = context->alloc_limit = (uint8_t*)region->GetAllocStart();
    }
}

SatoriObject* SatoriAllocator::AllocRegularShared(SatoriAllocationContext* context, size_t size, uint32_t flags)
{
    SatoriRegion* region = m_regularRegion;
    _ASSERTE(region == nullptr || region->IsAttachedToAllocatingOwner());

    while (true)
    {
        if (region != nullptr)
        {
            // get extra MIN_FREE_SIZE as other threads will allocate after us
            // and when we are done we need a parseability separator.
            size_t moreSpace = size + Satori::MIN_FREE_SIZE;
            size_t allocRemaining = region->GetAllocRemaining();
            if (allocRemaining >= moreSpace)
            {
                // we have enough free space in the region to continue
                bool zeroInitialize = !(flags & GC_ALLOC_ZEROING_OPTIONAL);
                if (zeroInitialize)
                {
                    // We do not want to pre-zero too much in shared case.
                    // Just aligning on index is good enough.
                    // Also we do not want the threads to hold on to unfinished allocs for too long.

                    // " +/- sizeof(size_t)" here is to intentionally misalign alloc_limit on the index granularity
                    // to improve chances that the object that is allocated here will be indexed
                    size_t misAlignedOnIndexEnd = ALIGN_UP(region->GetAllocStart() + moreSpace + sizeof(size_t), Satori::INDEX_GRANULARITY) - sizeof(size_t);
                    size_t misAlignedMoreSpace = misAlignedOnIndexEnd - region->GetAllocStart();

                    if (misAlignedMoreSpace <= allocRemaining)
                    {
                        moreSpace = misAlignedMoreSpace;
                    }
                    else
                    {
                        // In nonshared we can pick the tail with single object allocs.
                        // In shared, just pick the tail. We do not want allocs in shared that are too small.
                        moreSpace = allocRemaining;
                    }
                }

                // do not zero-initialize just yet, we will do that after leaving the lock.
                size_t dirtyStart, dirtyEnd;
                SatoriObject* result = (SatoriObject*)region->AllocateDirty(moreSpace, &dirtyStart, &dirtyEnd);
                if (!result)
                {
                    //OOM, nothing to undo.
                    m_regularAllocLock.Leave();
                    return nullptr;
                }

                if (flags & GC_ALLOC_FINALIZE)
                {
                    if (!region->RegisterForFinalization(result))
                    {
                        // OOM, undo the allocation
                        region->StopAllocating(result->Start());
                        m_regularAllocLock.Leave();
                        return nullptr;
                    }
                }

                // Conservatively assume the region may get finalizable objects.
                // This is for initial placement into queues, which may happen before
                // all objects in the buffer are allocated.
                // It is ok to be conservative, also this will self-correct after sweeping.
                region->SetHasFinalizables();

                // region stays unparsable until "moreSpace" is consumed.
                region->IncrementUnfinishedAlloc();

                // done with region modifications.
                m_regularAllocLock.Leave();

                context->alloc_ptr = (uint8_t*)result + size;
                context->alloc_bytes += moreSpace;
                // leave some space at the end for the free object
                moreSpace -= Satori::MIN_FREE_SIZE;
                context->alloc_limit = (uint8_t*)result + moreSpace;

                result->CleanSyncBlock();
                region->SetIndicesForObject(result, result->Start() + size);
                if (zeroInitialize)
                {
                    ZeroInitDirtyBody(result->Start(), result->Start() + moreSpace, dirtyStart, dirtyEnd);
                }

                PublishAllocatedBytesIfNeeded(context, AllocationTickKind::Small, result, size);
                return result;
            }

            if (region->IsAllocating())
            {
                region->StopAllocating();
            }

            // if a GC took the region while it stayed attached, start a new accounting period.
            region->RebaselineUsableIfStale(0);

            // try get from the free list
            size_t desiredFreeSpace = size + Satori::MIN_FREE_SIZE;
            if (region->StartAllocating(desiredFreeSpace))
            {
                // we have enough free space in the region to continue
                continue;
            }

            region->DetachFromAlocatingOwnerRelease();
            m_heap->Recycler()->AddEphemeralRegion(region);
        }

        TryGetRegularRegion(region, size);
        if (region == nullptr)
        {
            //OOM
            m_regularAllocLock.Leave();
            return nullptr;
        }

        region->TryCommit();

        region->AttachToAllocatingOwner(&m_regularRegion);
        region->SetGenerationRelease(1);
        region->ResetReusableForRelease();
    }
}

// The region that follows `region` in its page, if it is in a reusable queue and fits the request, taken out of that queue.
//
// The next region is not ours to look at. It can be freed meanwhile, and the granule of a free region is decommitted by the trimmer.
// This is the only place where a thread reads the header of a region that it does not own, and it is safe for these reasons:
//  - The thread registers with the page (EnterUnownedHeaderRead) before it reads the region map, and leaves after its last read
//    of the header. The trimmer does not decommit while a thread is registered with the page.
//  - A region is removed from the map before its granules are marked free, and the trimmer claims only free granules, and looks
//    at the registrations after it claims. So if the map has a region at `next` after we registered, then either the region is
//    still there, or it was freed and the trimmer sees us and leaves the memory committed.
//  - The header of a region that was freed holds what it held, and FreeRegion asserts that its queue is null, so the read finds no queue.
//    A region that was freed and is in use again by somebody else is a region, whatever it is used for, and its queue is what it is.
//    The removal checks the queue under its lock.
// The free granule after the region, which the caller claims, has no header. It is a bit operation, and reads no memory of the granule.
// Other readers get to headers through the region map too (conservative marking, interior pointers, the card groups).
// They run only on regions that are live: in the precise mode a reference is to a live object, a card group is set only for a live region,
// and in the conservative mode the trimmer is stopped while the GC marks (see DrainDeferredSweepQueueConcurrent).
static SatoriRegion* TryTakeReusableNext(SatoriPage* page, SatoriRegion* region, size_t minFree)
{
    SatoriRegion* result = nullptr;

    page->EnterUnownedHeaderRead();
    SatoriRegion* next = region->NextInPage();
    if (next)
    {
#if _DEBUG
        page->AssertHeaderIsReadable((next->Start() - page->Start()) >> Satori::REGION_BITS);
#endif
        TraceCount(&g_nbrWithNext); // TEMP-TRACE
        SatoriQueue<SatoriRegion>* nextContainingQueue = next->ContainingQueue();
        if (!nextContainingQueue) TraceCount(&g_nbrNextNoQueue); // TEMP-TRACE
        else if (nextContainingQueue->Kind() == QueueKind::RecyclerReusable) TraceCount(&g_nbrNextReusable); // TEMP-TRACE
        else TraceCount(&g_nbrNextOther); // TEMP-TRACE
        if (nextContainingQueue &&
            nextContainingQueue->Kind() == QueueKind::RecyclerReusable &&
            next->CanFitAllocation(minFree) &&
            nextContainingQueue->TryRemove(next))
        {
            TraceCount(&g_nbrReusableTaken); // TEMP-TRACE
            result = next;
        }
    }

    page->LeaveUnownedHeaderRead();
    return result;
}

// Gets a region for a regular allocation of the given size. Prefers reusable regions, but a reusable region
// that cannot serve the request from its free lists would just be retired on the spot while charging
// the budget for its unused space. Such regions are skipped and left to be used for smaller requests.
void SatoriAllocator::TryGetRegularRegion(SatoriRegion*& region, size_t size)
{
    size_t minFree = size + Satori::MIN_FREE_SIZE;
    TraceCount(&g_nbrReqs); // TEMP-TRACE

    // opportunistically try get the next region, if available
    SatoriPage* page = nullptr;
    size_t nextStart = 0;

    if (region)
    {
        page = region->ContainingPage();
        nextStart = region->End();

        SatoriRegion* next = TryTakeReusableNext(page, region, minFree);
        if (next)
        {
            region = next;
            _ASSERTE(region->Occupancy() < Satori::REGION_SIZE_GRANULARITY);
            return;
        }
    }

    region = m_heap->Recycler()->TryGetReusable(minFree);
    if (region) TraceCount(&g_nbrReusableHit); // TEMP-TRACE

    if (region == nullptr)
    {
        // The granule right after the region, if it is free. That keeps the regions of a thread together.
        // Free memory has no header to read, so this takes a claim and nothing else.
        if (page && nextStart < page->End())
        {
            SatoriTrace::Count(SatoriTrace::s_counters.adjacentTries);
            size_t granule = (nextStart - page->Start()) >> Satori::REGION_BITS;
            if (page->TryClaimGranuleAt(granule))
            {
                region = page->FormatClaimed(granule, 1);
                if (region)
                {
                    SatoriTrace::Count(SatoriTrace::s_counters.adjacentClaims);
                    _ASSERTE(region->NothingMarked());
                    return;
                }
            }
        }

        region = GetRegion(Satori::REGION_SIZE_GRANULARITY, SatoriRegionRequester::Regular);
        TraceCount(&g_nbrGetRegion); // TEMP-TRACE
        _ASSERTE(region == nullptr || region->NothingMarked());
    }
}

SatoriObject* SatoriAllocator::AllocLarge(SatoriAllocationContext* context, size_t size, uint32_t flags)
{
    // handle huge allocation. It can't be from the current region.
    if (size > SatoriRegion::MAX_LARGE_OBJ_SIZE)
    {
        return AllocHuge(context, size, flags);
    }

    // when allocations cross certain thresholds, check if GC should start or help is needed.
    UpdateAllocStatsAndHelpIfNeeded(context);

tryAgain:

    if (!context->LargeRegion() &&
        size < Satori::REGION_SIZE_GRANULARITY / 2)
    {
        //m_largeAllocLock.Enter();
        if (m_largeAllocLock.TryEnter())
        {
            return AllocLargeShared(context, size, flags);
        }
    }

    SatoriRegion* region = context->LargeRegion();
    while (true)
    {
        if (region)
        {
            size_t allocRemaining = region->GetAllocRemaining();
            if (allocRemaining >= size)
            {
                bool zeroInitialize = !(flags & GC_ALLOC_ZEROING_OPTIONAL);
                SatoriObject* result = (SatoriObject*)region->Allocate(size, zeroInitialize);
                if (!result)
                {
                    // OOM, nothing to undo
                    return nullptr;
                }

                if (flags & GC_ALLOC_FINALIZE)
                {
                    if (!region->RegisterForFinalization(result))
                    {
                        // OOM, undo allocation
                        region->StopAllocating(result->Start());
                        return nullptr;
                    }

                    region->SetHasFinalizables();
                }

                // success
                result->CleanSyncBlock();
                region->SetIndicesForObject(result, result->Start() + size);

                OnLargeOrPinnedAllocated(context, flags, result, size);
                return result;
            }
        }

        // check if existing region has space or drop it into recycler
        if (region)
        {
            if (region->IsAllocating())
            {
                region->StopAllocating();
            }

            // if a GC took the region while it stayed attached, start a new accounting period.
            region->RebaselineUsableIfStale(0);

            // try get from the free list
            if (region->StartAllocating(size))
            {
                // we have enough free space in the region to continue
                continue;
            }

            region->DetachFromAlocatingOwnerRelease();
            m_heap->Recycler()->AddEphemeralRegion(region);

            // retry
            goto tryAgain;
        }

        // get a new regular region.
        _ASSERTE(SatoriRegion::RegionSizeForAlloc(size) == Satori::REGION_SIZE_GRANULARITY);

        region = nullptr;
        if (size < Satori::REGION_SIZE_GRANULARITY / 2)
        {
            region = m_heap->Recycler()->TryGetReusableForLarge();
        }

        if (!region)
        {
            region = GetRegion(Satori::REGION_SIZE_GRANULARITY, SatoriRegionRequester::Large);
            if (!region)
            {
                //OOM
                return (SatoriObject*)nullptr;
            }

            _ASSERTE(region->NothingMarked());
        }

        region->AttachToAllocatingOwner(&context->LargeRegion());
        region->SetGenerationRelease(1);
        region->ResetReusableForRelease();
    }
}

SatoriObject* SatoriAllocator::AllocLargeShared(SatoriAllocationContext* context, size_t size, uint32_t flags)
{
    _ASSERTE(flags & (GC_ALLOC_LARGE_OBJECT_HEAP | GC_ALLOC_PINNED_OBJECT_HEAP));

    SatoriRegion* region = m_largeRegion;
    _ASSERTE(region == nullptr || region->IsAttachedToAllocatingOwner());

    while (true)
    {
        if (region != nullptr)
        {
            if (region->GetAllocRemaining() >= size)
            {
                // do not zero-initialize just yet, we will do that after leaving the lock.
                size_t dirtyStart, dirtyEnd;
                SatoriObject* result = (SatoriObject*)region->AllocateDirty(size, &dirtyStart, &dirtyEnd);
                if (!result)
                {
                    //OOM, nothing to undo
                    m_largeAllocLock.Leave();
                    return nullptr;
                }

                if (flags & GC_ALLOC_FINALIZE)
                {
                    if (!region->RegisterForFinalization(result))
                    {
                        // OOM, undo the allocation
                        region->StopAllocating(result->Start());
                        m_largeAllocLock.Leave();
                        return nullptr;
                    }

                    region->SetHasFinalizables();
                }

                // region stays unparsable until allocation is complete.
                region->IncrementUnfinishedAlloc();
                // done with region modifications.
                m_largeAllocLock.Leave();

                result->CleanSyncBlockAndSetUnfinished();
                region->SetIndicesForObject(result, result->Start() + size);
                if (!(flags & GC_ALLOC_ZEROING_OPTIONAL))
                {
                    ZeroInitDirtyBody(result->Start(), result->Start() + size, dirtyStart, dirtyEnd);
                }

                OnLargeOrPinnedAllocated(context, flags, result, size);
                return result;
            }

            if (region->IsAllocating())
            {
                region->StopAllocating();
            }

            // if a GC took the region while it stayed attached, start a new accounting period.
            region->RebaselineUsableIfStale(0);

            // try get from the free list
            if (region->StartAllocating(size))
            {
                // we have enough free space in the region to continue
                continue;
            }

            region->DetachFromAlocatingOwnerRelease();
            m_heap->Recycler()->AddEphemeralRegion(region);
        }

        // get a new regular region.
        _ASSERTE(SatoriRegion::RegionSizeForAlloc(size) == Satori::REGION_SIZE_GRANULARITY);

        region = nullptr;
        if (size < Satori::REGION_SIZE_GRANULARITY / 2)
        {
            region = m_heap->Recycler()->TryGetReusableForLarge();
            _ASSERTE(region == nullptr || !region->IsAllocating());
        }

        if (!region)
        {
            region = GetRegion(Satori::REGION_SIZE_GRANULARITY, SatoriRegionRequester::SharedLarge);
            if (!region)
            {
                //OOM
                m_largeAllocLock.Leave();
                return nullptr;
            }

            _ASSERTE(region->NothingMarked());
        }

        region->TryCommit();

        region->AttachToAllocatingOwner(&m_largeRegion);
        region->SetGenerationRelease(1);
        region->ResetReusableForRelease();
    }
}

SatoriObject* SatoriAllocator::AllocHuge(SatoriAllocationContext* context, size_t size, uint32_t flags)
{
    // when allocations cross certain thresholds, check if GC should start or help is needed.
    UpdateAllocStatsAndHelpIfNeeded(context);

    size_t regionSize = SatoriRegion::RegionSizeForAlloc(size);
    _ASSERTE(regionSize > Satori::REGION_SIZE_GRANULARITY);
    SatoriRegion* hugeRegion = GetRegion(regionSize, SatoriRegionRequester::Huge);
    if (!hugeRegion)
    {
        //OOM
        return nullptr;
    }

    _ASSERTE(hugeRegion->NothingMarked());
    bool zeroInitialize = !(flags & GC_ALLOC_ZEROING_OPTIONAL);
    SatoriObject* result = (SatoriObject*)hugeRegion->AllocateHuge(size, zeroInitialize);
    if (!result)
    {
        ReturnRegion(hugeRegion);
        // OOM
        return nullptr;
    }

    result->CleanSyncBlockAndSetUnfinished();
    hugeRegion->SetIndicesForObject(result, hugeRegion->Start() + Satori::REGION_SIZE_GRANULARITY);

    // huge regions are not attached to contexts and in gen0+ would appear parseable,
    // but this one is not parseable yet since the new object has no MethodTable
    // we will keep the region in gen -1 for now and make it gen1 or gen2 in PublishObject.
    hugeRegion->StopAllocating();

    if (flags & GC_ALLOC_FINALIZE)
    {
        hugeRegion->SetHasFinalizables();
        if (!hugeRegion->RegisterForFinalization(result))
        {
            result->CleanSyncBlock();
            hugeRegion->MakeBlank();
            ReturnRegion(hugeRegion);
            return nullptr;
        }
    }

    OnLargeOrPinnedAllocated(context, flags, result, size);
    return result;
}

SatoriObject* SatoriAllocator::AllocPinned(SatoriAllocationContext* context, size_t size, uint32_t flags)
{
    _ASSERTE(flags & GC_ALLOC_PINNED_OBJECT_HEAP);

    if (size > SatoriRegion::MAX_LARGE_OBJ_SIZE)
    {
        return AllocHuge(context, size, flags);
    }

    // when allocations cross certain thresholds, check if GC should start or help is needed.
    // NB: not while holding the lock. This may trigger a GC or do some GC work and, while the lock is held,
    //     all other pinned allocations would be diverted to AllocLarge.
    UpdateAllocStatsAndHelpIfNeeded(context);

    // if can't get a lock, let AllocLarge handle this.
    if (!m_pinnedAllocLock.TryEnter())
    {
        return AllocLarge(context, size, flags);
    }

    SatoriRegion* region = m_pinnedRegion;
    while (true)
    {
        if (region != nullptr)
        {
            if (region->GetAllocRemaining() >= size)
            {
                // do not zero-initialize just yet, we will do that after leaving the lock.
                size_t dirtyStart, dirtyEnd;
                SatoriObject* result = (SatoriObject*)region->AllocateDirty(size, &dirtyStart, &dirtyEnd);
                if (!result)
                {
                    //OOM, nothing to undo
                    m_pinnedAllocLock.Leave();
                    return nullptr;
                }

                if (flags & GC_ALLOC_FINALIZE)
                {
                    if (!region->RegisterForFinalization(result))
                    {
                        // OOM, undo the allocation
                        region->StopAllocating(result->Start());
                        m_pinnedAllocLock.Leave();
                        return nullptr;
                    }

                    region->SetHasFinalizables();
                }

                // region stays unparsable until allocation is complete.
                region->IncrementUnfinishedAlloc();
                // done with region modifications.
                m_pinnedAllocLock.Leave();

                result->CleanSyncBlockAndSetUnfinished();
                region->SetIndicesForObject(result, result->Start() + size);
                if (!(flags & GC_ALLOC_ZEROING_OPTIONAL))
                {
                    ZeroInitDirtyBody(result->Start(), result->Start() + size, dirtyStart, dirtyEnd);
                }

                OnLargeOrPinnedAllocated(context, flags, result, size);
                return result;
            }

            if (region->IsAllocating())
            {
                region->StopAllocating();
            }

            // if a GC took the region while it stayed attached, start a new accounting period.
            region->RebaselineUsableIfStale(0);

            // try get from the free list
            if (region->StartAllocating(size))
            {
                // we have enough free space in the region to continue
                continue;
            }

            region->DetachFromAlocatingOwnerRelease();
            m_heap->Recycler()->AddEphemeralRegion(region);
        }

        // get a new regular region.
        _ASSERTE(SatoriRegion::RegionSizeForAlloc(size) == Satori::REGION_SIZE_GRANULARITY);

        region = nullptr;
        if (size < Satori::REGION_SIZE_GRANULARITY / 2)
        {
            region = m_heap->Recycler()->TryGetReusableForLarge();
            _ASSERTE(region == nullptr || !region->IsAllocating());
        }

        if (!region)
        {
            region = GetRegion(Satori::REGION_SIZE_GRANULARITY, SatoriRegionRequester::Pinned);
            if (!region)
            {
                //OOM
                m_pinnedAllocLock.Leave();
                return nullptr;
            }

            _ASSERTE(region->NothingMarked());
        }

        region->TryCommit();

        region->AttachToAllocatingOwner(&m_pinnedRegion);
        region->SetGenerationRelease(1);
        region->ResetReusableForRelease();
    }
}

SatoriObject* SatoriAllocator::AllocImmortal(SatoriAllocationContext* context, size_t size, uint32_t flags)
{
    // immortal allocs should be way less than region size.
    _ASSERTE(size < Satori::REGION_SIZE_GRANULARITY / 2);

    SatoriLockHolder holder(&m_immortalAllocLock);
    SatoriRegion* region = m_immortalRegion;

    while (true)
    {
        if (region)
        {
            size_t allocRemaining = region->GetAllocRemaining();
            if (allocRemaining >= size)
            {
                bool zeroInitialize = !(flags & GC_ALLOC_ZEROING_OPTIONAL);
                SatoriObject* result = (SatoriObject*)region->Allocate(size, zeroInitialize);
                if (result)
                {
                    region->SetIndicesForObject(result, result->Start() + size);
                    OnLargeOrPinnedAllocated(context, flags, result, size);
                }
                else
                {
                    // OOM
                }

                return result;
            }

            if (region->IsAllocating())
            {
                region->StopAllocating();
            }
            else
            {
                if (region->StartAllocating(size))
                {
                    // we have enough free space in the region to continue
                    continue;
                }
            }
        }

        // get a new regular region. It will stay forever, so place it near the beginning of a page.
        region = GetRegionLow(Satori::REGION_SIZE_GRANULARITY, SatoriRegionRequester::Immortal);
        if (!region)
        {
            //OOM
            return nullptr;
        }

        _ASSERTE(region->NothingMarked());
        // Clear the dirty part of the region eagerly to not clear for each allocated (and typically small) object.
        // And, while at that, link the previous one in case we have a reason to iterate old regions.
        region->ZeroInitAndLink(m_immortalRegion);
        if (m_immortalRegion)
        {
            m_immortalRegion->DetachFromAlocatingOwnerRelease();
        }

        region->AttachToAllocatingOwner(&m_immortalRegion);
        region->SetGenerationRelease(3);
    }

    return nullptr;
}

void SatoriAllocator::DeactivateSharedRegion(SatoriRegion* region, bool promoteAllRegions)
{
    if (region)
    {
        if (region->IsAllocating())
        {
            region->StopAllocating();
        }

        if (promoteAllRegions)
        {
            region->DetachFromAlocatingOwnerRelease();
        }

        m_heap->Recycler()->AddEphemeralRegion(region);
    }
}

void SatoriAllocator::DeactivateSharedRegions(bool promoteAllRegions)
{
    SatoriRegion* immortalRegion = m_immortalRegion;
    if (immortalRegion && immortalRegion->IsAllocating())
    {
        immortalRegion->StopAllocating();
    }

    DeactivateSharedRegion(m_pinnedRegion, promoteAllRegions);
    DeactivateSharedRegion(m_largeRegion, promoteAllRegions);
    DeactivateSharedRegion(m_regularRegion, promoteAllRegions);
}

SatoriWorkChunk* SatoriAllocator::TryGetWorkChunk()
{
#if _DEBUG
    static int i = 0;
    // simulate low memory case once in a while
    // This is just to force more overflows. Otherwise they are very rare.
    if (i++ % 2 == 0)
    {
        return nullptr;
    }
#endif
    SatoriWorkChunk* chunk = m_workChunks->TryPop();

    while (!chunk && AddMoreWorkChunks())
    {
        chunk = m_workChunks->TryPop();
    }

    // chunks may be returned not cleared (see ReturnWorkChunks)
    if (chunk)
    {
        chunk->Clear();
    }

    return chunk;
}

// returns NULL only in OOM case
SatoriWorkChunk* SatoriAllocator::GetWorkChunk()
{
    SatoriWorkChunk* chunk = m_workChunks->TryPop();
    while (!chunk && AddMoreWorkChunks())
    {
        chunk = m_workChunks->TryPop();
    }

    // chunks may be returned not cleared (see ReturnWorkChunks)
    if (chunk)
    {
        chunk->Clear();
    }

    return chunk;
}

// Refills the pool of work chunks, one thread at a time.
//
// Many threads run out of chunks together, e.g. when marking starts. If each of them took a region for chunks,
// we would have as many regions as there were waiters and most of the chunks would sit unused.
// Instead the one that gets the lock refills and the others poll for chunks, which they do as long as we return true.
// The chunks are published in chains as they are made, so the waiters do not wait for the whole region to be formatted,
// and the chains go to different shards, so that every shard has some.
//
// Returns false if there is no memory to get chunks.
bool SatoriAllocator::AddMoreWorkChunks()
{
    // One shot - TryEnter would spin in the lock, and the waiters need to be looking for the chunks as they are published.
    if (!m_workChunkRefillLock.TryEnterOneShot())
    {
        // Someone else is refilling and publishes chunks as it goes. Look for them or for the end of the refill.
        for (int i = 0; i < 32; i++)
        {
            if (!m_workChunks->IsEmpty() || !m_workChunkRefillLock.IsLocked())
            {
                return true;
            }

            YieldProcessor();
        }

        // Still nothing. Let the refilling thread run, in case it was preempted.
        GCToOSInterface::YieldThread(0);
        return true;
    }

    SatoriLockHolder holder(&m_workChunkRefillLock, /* isLocked */ true);

    // The pool could have been refilled, or chunks returned, since the caller looked.
    if (!m_workChunks->IsEmpty())
    {
        return true;
    }

    // The chunks stay forever, so place the region near the beginning of a page.
    SatoriRegion* region = GetRegionLow(Satori::REGION_SIZE_GRANULARITY, SatoriRegionRequester::WorkChunks);
    if (!region)
    {
        return false;
    }

    // Commit all at once, not in small steps as chunks are allocated.
    region->TryCommit();

    SatoriWorkChunk* first = nullptr;
    SatoriWorkChunk* last = nullptr;
    size_t chainLength = 0;
    size_t totalLength = 0;

    while (true)
    {
        size_t mem = region->Allocate(Satori::MARK_CHUNK_SIZE, /*zeroInitialize*/ false);
        if (!mem)
        {
            break;
        }

        SatoriWorkChunk* chunk = SatoriWorkChunk::InitializeAt(mem);
        if (last)
        {
            last->SetNext(chunk);
        }
        else
        {
            first = chunk;
        }

        last = chunk;
        totalLength++;
        if (++chainLength == Satori::WORK_CHUNK_REFILL_CHAIN)
        {
            m_workChunks->PushChainToShard(m_workChunkRefillShard++, first, last);
            first = last = nullptr;
            chainLength = 0;
        }
    }

    if (first)
    {
        m_workChunks->PushChainToShard(m_workChunkRefillShard++, first, last);
    }

    if (totalLength == 0)
    {
        // could not commit anything.
        ReturnRegion(region);
        return false;
    }

    return true;
}

void SatoriAllocator::ReturnWorkChunk(SatoriWorkChunk* chunk)
{
    _ASSERTE(chunk->Count() == 0);
    m_workChunks->Push(chunk);
}

// Returns a chain of chunks at once, linked through Next() from the first to the last.
// Clearing every chunk here would be a cache miss per chunk. Chunks are cleared when taken instead.
void SatoriAllocator::ReturnWorkChunks(SatoriWorkChunk* first, SatoriWorkChunk* last)
{
    m_workChunks->PushChain(first, last);
}

// Same as above, when the last chunk is not known.
void SatoriAllocator::ReturnWorkChunks(SatoriWorkChunk* first)
{
    SatoriWorkChunk* last = first;
    while (last->Next())
    {
        last = last->Next();
    }

    ReturnWorkChunks(first, last);
}
