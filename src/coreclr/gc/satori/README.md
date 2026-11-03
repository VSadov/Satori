# Satori Garbage Collector #

A simple garbage collector that incorporates various ideas that I had over time.

### Short term goals: ###
- auto-tuning, auto-scaling, mostly “knobless” design.
- avoid long pauses when user threads are not making progress.

### Supported Features: ###

- [x] All features expected in a fully functional Garbage Collector:
  - Mark-and-sweep,
  - Internal pointers,
  - Premortem Finalization,
  - Weak References,
  - Dependent Handles,
  - Unloadable types,
  -  ... etc...
- [x] Generational GC.
   - younger generations can be collected without touching older ones
- [x] Compacting GC.
   - can optionally relocate objects to reduce fragmentation.
- [x] Concurrent GC.
   - all major phases, except optional relocation, can be done concurrently with user code.
- [x] Parallel GC
  - GC can employ multiple threads as needed.
- [x] Thread-local GC.
  - generation 0 GC is an inline thread-local GC and does not stop other threads.
- [x] Pacing GC.
  - allocating threads help with concurrent GC to ensure allocations are not getting ahead.
- [x] Precise and Conservative modes
  - supports precise and conservative stack root reporting.
- [x] Low-latency mode.
  - "nearly pauseless" mode when blocking phases sensitive to the heap size are turned off.
- [x] Trimming of committed set.
  - lazy return of unused memory to the OS.

### Supported Platforms: ###
|         | x64                 | arm64               |
| --------| ------------------- | ------------------- |
| Windows | <ul><li>- [x] </li> | <ul><li>- [x] </li> |
| Linux   | <ul><li>- [x] </li> | <ul><li>- [x] </li> |
| macOS   | <ul><li>- [x] </li> | <ul><li>- [x] </li> |

### Roadmap: ###
- [x]  explicit memory limits
- [x]  immortal allocations
- [x]  preallocated objects
- [ ]  perf tuning (possibly a lot of opportunities)
- [ ]  more and better diagnostics (support for debuggers and profilers)
- [ ]  NUMA awareness

### Escape-tracking diagnostics ###

The public runtime provider (`Microsoft-Windows-DotNETRuntime`, GC keyword `0x1`,
Verbose level `5`) exposes two version-0 records. Both payloads are 86 bytes;
the manifest defines their field order and wire types.

* **237, GCTrackingLifecycle**: physical tracking starts and ends, backoff skips,
  and owner-thread backoff-history updates. `Kind` is Start=0, End=1,
  BackoffSkip=2, History=3. History updates are not physical stops.
* **236, GCTrackingCheckpoint**: nonoverlapping count intervals. `Kind` is
  Initialization=0, LocalCollection=1, BeforeRebuild=2, Rebuilt=3, Final=4.
  A local checkpoint follows collection work, including marking aborts and
  finalizer publication. A premarking rejection does not consume an interval.

Correlate a physical lifetime by process, `ClrInstanceID`, and `Lifetime`, not
`RegionID` alone: region addresses are reused. `ThreadTag` identifies its owner,
not necessarily the thread emitting a deferred record. `Sequence` spans both
record families within a lifetime; decision/history records use sequence zero.
`RelatedLifetime` connects a backoff skip to the preceding owner history.
Checkpoints with `HasCollectionIndex` also carry the existing local-GC index.

`EscapeFnCalls` and `EscapeRecursivelyCalls` count actual entries, including
already-escaped recursive entries. They overlap and must not be added together.
`NewlyEscapedObjects` counts unescaped-to-escaped transitions, including
transitive discovery. `SeedObjects` separately counts reused-region initialization
and global-GC escape-mark reconstruction. A blocking GC that preserves physical
tracking closes the current accounting segment and starts another for
reconstruction. Summing new escapes across segments does **not** give a
lifetime-distinct object total. No object-identity table or extra graph walk is used.

`EpochBegin` and `EpochEnd` identify diagnostic continuity, separately from
physical tracking. The runtime queries effective event enablement in its safe
provider-control path and publishes a scalar epoch. Barriers only read that
scalar; they do not call the runtime event sink. Every public provider
reconfiguration conservatively advances the epoch, even if another listener
keeps counting enabled. Provider callbacks never reset another thread's state.
A trace must observe start and end, contiguous sequences, and no partial,
unavailable, or overflow flags before treating a lifetime as complete. Runtime
continuity alone cannot establish what an individual late-attached trace saw.

`Flags` are LifetimePartial=1, IntervalPartial=2, Deferred=4, Overflow=8,
StartedEnabled=16, IntervalOverflow=32, Rebuilding=64, IdentityUnavailable=128.
Counters saturate rather than wrap. Lifetime identifiers are never recycled;
exhaustion uses zero with IdentityUnavailable. Epoch exhaustion disables further
counting. Observed data during an enablement gap cannot be recovered.
The existing 32-bit local-GC index retains its original wrapping behavior;
checkpoints set Overflow when that correlation index no longer fits.

End `Reason` values are None=0, BarrierEscapeLimit=1, CollectionEscapeLimit=2,
InsufficientProgress=3, SurvivalAbort=4, AllocationExhausted=5,
ConcurrentPreparation=6, ConcurrentOwnStack=7, DemotedRegion=8, OwnerDetach=9,
RegionReuse=10, AbandonedHistory=11, GlobalGCDetach=12, ThreadExit=13.
The physical stop preserves its original cause even when the owner updates
backoff history later.

A barrier stop captures identity, counts, and reason in the region before
releasing ownership. Safe owner continuations, GC, detach, and reuse drain that
record once, before publishing the region for reuse. Concurrent markers do not
consume owner state. Deferred emission time is **not** an exact physical-stop
timestamp, and delivery is not guaranteed if tracing stops before a safe drain
or the process exits abruptly.

These unreleased sink extensions retain EE interface major version 6 and require
matching runtime/GC builds. This independent branch appends lifecycle 237,
checkpoint 236, and the enablement query after existing collection 239. Integration
with allocation outcomes must append outcome 238 first, then 237, 236, and the
query; branch-local slot positions are not a cross-build compatibility contract.
