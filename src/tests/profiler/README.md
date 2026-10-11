# Profiler Tests

## Architecture
A profiler attaches to a managed process via environment variables that need to be set before the process is launched. Our profiler tests do the test verification in the native profiler code, so the output of the profiler needs to be verified. To achieve both of these things the profiler tests are a wrapper that re-launch themselves with the correct environment variables set and also verify that the profiler succeeded in its verification.

## How to run one to investigate
Because of this two layer architecture just running the managed test executable will not run the test. You have to set the appropriate environment variables and pass the RunTest argument. The easiest way to create a script like the following

```
#~/bin/bash

export CORECLR_ENABLE_PROFILING=1
export CORECLR_PROFILER={2726B5B4-3F88-462D-AEC0-4EFDC8D7B921}
export CORECLR_PROFILER_PATH=<Path to test binaries>/profiler/eventpipe/eventpipe/libProfiler.so

<Path to test binaries>/Core_Root/corerun <Path to test binaries>/profiler/eventpipe/eventpipe/eventpipe.dll RunTest
```

## Object-reference walking

The default profiler runner includes `gcheapenumeration`. Under Satori, it runs the object-reference walking checks without an extra argument: object fields, reference arrays, struct arrays, empty arrays, null filtering, and early callback termination. The other profiler projects remain excluded from the merged runner.

Satori skips the three whole-heap enumeration scenarios because profiler suspension alone does not provide the blocking-GC phase needed to walk allocating regions safely. Whole-heap walking during a forced blocking collection is covered by the `gcdump` and `gcdump_async` tests in the tracing runner.

The explicit `EnumerateObjectReferences` mode remains available for focused runs. Non-Satori runtimes retain the existing whole-heap enumeration scenarios.
