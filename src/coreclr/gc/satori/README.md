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
   - blocking compaction and aggressive-trim calls may share a collection, but return only after that collection finishes the requested work.
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

### Explicit Collections: ###

- Forced blocking collections mark with the application stopped and finish sweeping before returning, including in low-latency mode.
- A running concurrent collection is waited for without changing its marking or sweeping policy. Blocking callers help even when allocation pacing is disabled; other callers help when pacing allows or no workers are active. Forced blocking requests wait for a collection that starts with their requested work; they do not force an early blocking phase in the running collection.
- Requests may share a collection only when it covers their generations and completes their requested blocking, compaction, and trim work.
- Nonblocking requests still permit concurrent marking and deferred sweeping. Synchronous sweeping does not itself request compaction or OS decommit; aggressive mode additionally trims free regions inside the pause.

### Roadmap: ###
- [x]  explicit memory limits
- [x]  immortal allocations
- [x]  preallocated objects
- [ ]  perf tuning (possibly a lot of opportunities)
- [ ]  more and better diagnostics (support for debuggers and profilers)
- [ ]  NUMA awareness

### Diagnostics: ###

- Object-reference and whole-heap walking, plus scanning finalizer-queue, handle, and dependent-handle roots, are implemented.
- Requested ETW/EventPipe heap dumps run inside a forced blocking full collection, after sweeping and any aggressive trim. The object walker waits for the background trimmer before traversing regions; it remains quiescent throughout the object walk. Heap events precede the GC-end event.
- Whole-heap walks include immortal objects and skip free space. Satori has no separate large-object heap, so that heap-selection flag does not select a distinct set of regions.
- GC lifecycle, survivor/movement, and finalizable-object-queued profiler notifications are not implemented. Heap walking outside a GC with allocation-context handling is also not yet implemented.
- Satori NativeAOT also limits snapshots to diagnostic requests. Its bulk type events contain type IDs but no names; Task-based async dump coverage verifies that captured state-machine payloads remain in the snapshot.
