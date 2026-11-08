// TEMP-TRACE-BEGIN
//
// Temporary instrumentation for the page-add investigation (Milestone 2). Not for commit.
//
// SATORI_TRACE_DIR is the only switch. If it is set, a process writes <SATORI_TRACE_DIR>\<pid>.log with
//   [pageadd]  one line per page add,
//   [state]    every SATORI_TRACE_STATE_SEC (30 by default), at the end of every gen2 and at exit,
//   [counters] at exit and with every [state], the work of the allocator of regions.
//   [addstats] the waits for a page add in flight and the durations of adds, after every [state] line.
//   [gcstats]  the pause totals of blocking GCs, the time they wait for the world to stop, and the time in GetRegion, after every [state] line.
//   [osstats]  the commits and decommits that the GC asks of the OS, their number, size and time, after every [state] line.
// If it is not set, nothing is written and nothing is measured.
// The same lines are printed by the three builds of the milestone, so that one analyzer reads all of them.
//
#ifndef __SATORI_PAGEADD_TRACE_H__
#define __SATORI_PAGEADD_TRACE_H__

// SATORI_TRACE_DIR is set.
extern bool g_traceOn;

// Granules that the trimmer holds right now: claimed, and not given back yet.
extern volatile size_t g_trimOut;
// GetRegion calls whose first lookup found nothing, with the trimmer holding a granule / without.
extern volatile size_t g_missWhileOut;
extern volatile size_t g_missEmpty;
// All failed lookups, including the retries while waiting for a page to be added.
extern volatile size_t g_missPasses;
// Page adds made after the 1 msec deadline, while another thread was adding.
// (after the herd fix: adds through the valve, started while another add was in flight)
extern volatile size_t g_timedOutAdds;

// The trimmer. A round is one pass over the pages. A decommit is any decommit of the memory of a region or a granule.
// A commit is a recommit if it is in a 2 MB granule where something was decommitted not more than 120 sec before,
// and that decommitted memory has not been committed again yet.
extern volatile size_t g_trimRounds;
extern volatile size_t g_decommitBytes;
extern volatile size_t g_recommit120Bytes;

void TraceTrimInc();
void TraceTrimDec();
void TraceTrimRound();
void TraceNoteCommitSlow(size_t address, size_t bytes);
void TraceNoteDecommitSlow(size_t address, size_t bytes);

inline void TraceNoteCommit(size_t address, size_t bytes)
{
    if (g_traceOn)
    {
        TraceNoteCommitSlow(address, bytes);
    }
}

inline void TraceNoteDecommit(size_t address, size_t bytes)
{
    if (g_traceOn)
    {
        TraceNoteDecommitSlow(address, bytes);
    }
}

// Step 1 of Milestone 2b: waiting for a page add in flight, and how long adds take.
// A wait episode is one wait that ended, because an add completed or because the valve opened.
extern volatile size_t g_addWaitEpisodes;
extern volatile size_t g_addWaitTicks;
extern volatile size_t g_addWaitMaxTicks;
// Adds that started while another add was in flight, and the valve uses. They are the same thing, counted in two places.
extern volatile size_t g_addsOverlapped;
extern volatile size_t g_addValveUses;
// Regular page adds, and the time from the decision to add to the page being published.
extern volatile size_t g_addCount;
extern volatile size_t g_addTicks;
extern volatile size_t g_addMaxTicks;
// What the current GetRegionImpl call has waited for adds in flight.
extern thread_local int64_t t_addWaitedTicks;

void TraceNoteAddWaitEnd(int64_t waitStart);
void TraceNoteValveUse(int64_t waitStart);

// Step 2 of Milestone 2b: the commits and decommits that the GC asks the OS for, and the time they take. (see [osstats])
bool TraceVirtualCommit(void* address, size_t size);
bool TraceVirtualDecommit(void* address, size_t size);
// The size that memory for allocation is committed in. (SATORI_COMMIT_STEP_KB)
size_t TraceCommitStep();

// Step 2 of Milestone 2b: where the time of the growth phase goes.
// The GetRegion calls and the time in them, and the time that a blocking GC waits for the world to stop.
extern volatile size_t g_getRegionCalls;
extern volatile size_t g_getRegionTicks;
extern volatile size_t g_getRegionMaxTicks;
extern volatile size_t g_suspendCount;
extern volatile size_t g_suspendTicks;
extern volatile size_t g_suspendMaxTicks;

void TraceNoteGetRegion(int64_t ticks);
void TraceNoteSuspend(int64_t ticks);

// Per-call state of GetRegionImpl.
struct TracePageAddCtx
{
    bool missed;
    bool sawOut;
    bool timedOut;
    int passes;
    int64_t firstMissTs;
    int64_t decisionTs;
    size_t out;
    size_t trim;
    size_t preCount;
    int gcState;
    int condemned;
    size_t defSweep;
    size_t relocated;
    // the heap commit at the decision, and its peak so far
    size_t commitBefore;
    size_t peakBefore;
    // adds in flight at the decision, counting ours. The time this call has waited for adds in flight. When the page was published.
    int inFlight;
    int64_t waitedTicks;
    int64_t publishedTs;
};

// What is free: the free granules of the pages and the regions in the cache.
struct TraceSnap
{
    size_t regions;
    size_t n2;
    size_t n46;
    size_t nBig;
    size_t freeBytes;
    size_t freeCommitted;
    size_t maxRegion;
    size_t maxRun;
    size_t runs;
    size_t singleGE;
    size_t runsGE;
    size_t runsGECommitted;
    // free regions (granules) that are committed in full
    size_t warmFree;
    bool truncated;
};

#endif
// TEMP-TRACE-END
