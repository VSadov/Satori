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

### Experimental reuse-aware incremental relocation

On the development branch, `DOTNET_gcIncrReloc=1` enables bounded incremental
Gen2 relocation. `DOTNET_gcIncrRelocReuse` selects an experimental LowLatency-only
grading policy:

| Value | Reusable-space credit |
| --- | --- |
| `0` | Original sparse-region policy |
| `1` | All free-list capacity (spans of at least 2 KB) |
| `2` | Capacity in spans of at least 32 KB |
| `3` | Full credit for 32 KB+ spans, half credit for smaller free-list spans |
| `4` | Same as `2`, but sources must have survived at least two sweeps without allocation |
| `5` | Same as `2`, but credit is halved after two unused sweeps and quartered after four |
| `6` | Same as `5`, without the occupancy-relative minimum benefit |

Other values use the original policy. Set `DOTNET_GCLatencyMode=3` for these
experiments. With a nonzero reuse policy, normal-latency collections continue
using their regular policy; the hybrid incremental/regular experiment is not
used.

The grade is estimated relocation cost divided by stranded free bytes: total
free space minus the reusable-space credit. Sources must be at most half full,
have at least 64 KB of stranded space, and (except for policy `6`) recover at
least half their occupancy in stranded space. The same benefit is used when
recorded-reference counts require pruning candidates to fit the budget.

Receivers are existing non-source regions with a free span of at least 128 KB
and at least half their free space in 32 KB+ spans. By default no fresh target
is allocated; sources without a suitable receiver stay for ordinary sweeping.
`DOTNET_gcIncrRelocReuseFresh=1` tests allowing fresh receivers when necessary
to consolidate fragmented sources. Fully dead sources stay for sweeping rather
than consuming a receiver in either case.

`DOTNET_gcIncrRelocBudget` retains the calibrated added-pause budget (default
1000 microseconds). This is an estimate, not a hard pause bound: root costs,
scheduling, and calibration outliers can exceed it. `DOTNET_gcIncrStatsDir`
enables development-only measurements including the selected and kept benefit,
empty sources, and receiver misses. Verbose runtime events are not needed.

Recovery here means returning regions to the allocator; committed memory and
working set can remain high until those regions are reused or background
trimming decommits them. Satori's `GCMemoryInfo.HeapSizeBytes` currently reports
bytes in use, not the footprint of occupied regions, so it cannot by itself
measure fragmentation recovery. Compare region counts and process memory too.

Policies `5` and `6` use collection count as a proxy for missed reuse
opportunities. Forced collections with little allocation can age a region
without providing any real opportunity to reuse it; these policies remain
experimental rather than a demonstrated probability model.
