# Satori GC

An experimental concurrent garbage collector for .NET designed to enhance performance and responsiveness in C# applications by minimizing pause times and enabling smoother memory management. Suitable for latency-sensitive applications, real-time systems, and high-throughput server workloads.

For more information about pauseless garbage collectors in .NET, you can follow the discussion [here](https://github.com/dotnet/runtime/discussions/115627).

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
     
### Short term goals: ###
- auto-tuning, auto-scaling, mostly “knobless” design.
- avoid long pauses when user threads are not making progress.  

### Supported Platforms: ### 
|         | x64                 | arm64               |
| --------| ------------------- | ------------------- |
| Windows | <ul><li>- [x] </li> | <ul><li>- [x] </li> |
| Linux   | <ul><li>- [x] </li> | <ul><li>- [x] </li> |
| macOS   | <ul><li>- [x] </li> | <ul><li>- [x] </li> |


