// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#ifndef _GCINTERFACE_EE_H_
#define _GCINTERFACE_EE_H_

// Implement pure virtual for NativeAOT Unix (for -p:LinkStandardCPlusPlusLibrary=false the default),
// to avoid linker requiring __cxa_pure_virtual.
#if defined(FEATURE_NATIVEAOT) && !defined(TARGET_WINDOWS)
// `while(true);` is to satisfy the missing `return` statement. It will be optimized away by the compiler.
#define PURE_VIRTUAL { assert(!"pure virtual function called"); while(true); }
#else
#define PURE_VIRTUAL = 0;
#endif

enum EtwGCRootFlags: int32_t
{
    kEtwGCRootFlagsPinning =            0x1,
    kEtwGCRootFlagsWeakRef =            0x2,
    kEtwGCRootFlagsInterior =           0x4,
    kEtwGCRootFlagsRefCounted =         0x8,
};

enum EtwGCRootKind: int32_t
{
    kEtwGCRootKindStack =               0,
    kEtwGCRootKindFinalizer =           1,
    kEtwGCRootKindHandle =              2,
    kEtwGCRootKindOther =               3,
};

struct StressLogMsg;

// This interface provides functions that the GC can use to fire events.
// Events fired on this interface are split into two categories: "known"
// events and "dynamic" events. Known events are events that are baked-in
// to the hosting runtime's event manifest and are part of the GC/EE interface.
// There is one callback on IGCToCLREventSink for each known event.
//
// Dynamic events are constructed at runtime by the GC and are not known
// to the EE. ([LOCALGC TODO dynamic event implementation])
class IGCToCLREventSink
{
public:
    // Fires a dynamic event with the given event name and payload. Dynamic
    // events are not known to the EE and are fired as an unschematized event
    // to the underlying eventing implementation.
    virtual
    void FireDynamicEvent(
        const char* eventName,
        void* payload,
        uint32_t payloadSize) PURE_VIRTUAL
    virtual
    void FireGCStart_V2(uint32_t count, uint32_t depth, uint32_t reason, uint32_t type) PURE_VIRTUAL

    virtual
    void FireGCEnd_V1(uint32_t count, uint32_t depth) PURE_VIRTUAL

    virtual
    void FireGCGenerationRange(uint8_t generation, void* rangeStart, uint64_t rangeUsedLength, uint64_t rangeReservedLength) PURE_VIRTUAL

    virtual
    void FireGCHeapStats_V2(
        uint64_t generationSize0,
        uint64_t totalPromotedSize0,
        uint64_t generationSize1,
        uint64_t totalPromotedSize1,
        uint64_t generationSize2,
        uint64_t totalPromotedSize2,
        uint64_t generationSize3,
        uint64_t totalPromotedSize3,
        uint64_t generationSize4,
        uint64_t totalPromotedSize4,
        uint64_t finalizationPromotedSize,
        uint64_t finalizationPromotedCount,
        uint32_t pinnedObjectCount,
        uint32_t sinkBlockCount,
        uint32_t gcHandleCount) PURE_VIRTUAL

    virtual
    void FireGCCreateSegment_V1(void* address, size_t size, uint32_t type) PURE_VIRTUAL

    virtual
    void FireGCFreeSegment_V1(void* address) PURE_VIRTUAL

    virtual
    void FireGCCreateConcurrentThread_V1() PURE_VIRTUAL

    virtual
    void FireGCTerminateConcurrentThread_V1() PURE_VIRTUAL

    virtual
    void FireGCTriggered(uint32_t reason) PURE_VIRTUAL

    virtual
    void FireGCMarkWithType(uint32_t heapNum, uint32_t type, uint64_t bytes) PURE_VIRTUAL

    virtual
    void FireGCJoin_V2(uint32_t heap, uint32_t joinTime, uint32_t joinType, uint32_t joinId) PURE_VIRTUAL

    virtual
    void FireGCGlobalHeapHistory_V4(uint64_t finalYoungestDesired,
                                    int32_t numHeaps,
                                    uint32_t condemnedGeneration,
                                    uint32_t gen0reductionCount,
                                    uint32_t reason,
                                    uint32_t globalMechanisms,
                                    uint32_t pauseMode,
                                    uint32_t memoryPressure,
                                    uint32_t condemnReasons0,
                                    uint32_t condemnReasons1,
                                    uint32_t count,
                                    uint32_t valuesLen,
                                    void *values) PURE_VIRTUAL

    virtual
    void FireGCAllocationTick_V1(uint32_t allocationAmount, uint32_t allocationKind) PURE_VIRTUAL

    virtual
    void FireGCAllocationTick_V4(uint64_t allocationAmount, 
                                 uint32_t allocationKind, 
                                 uint32_t heapIndex, 
                                 void* objectAddress, 
                                 uint64_t objectSize) PURE_VIRTUAL

    virtual
    void FirePinObjectAtGCTime(void* object, uint8_t** ppObject) PURE_VIRTUAL

    virtual
    void FirePinPlugAtGCTime(uint8_t* plug_start, uint8_t* plug_end, uint8_t* gapBeforeSize) PURE_VIRTUAL

    virtual
    void FireGCPerHeapHistory_V3(void *freeListAllocated,
                                 void *freeListRejected,
                                 void *endOfSegAllocated,
                                 void *condemnedAllocated,
                                 void *pinnedAllocated,
                                 void *pinnedAllocatedAdvance,
                                 uint32_t runningFreeListEfficiency,
                                 uint32_t condemnReasons0,
                                 uint32_t condemnReasons1,
                                 uint32_t compactMechanisms,
                                 uint32_t expandMechanisms,
                                 uint32_t heapIndex,
                                 void *extraGen0Commit,
                                 uint32_t count,
                                 uint32_t valuesLen,
                                 void *values) PURE_VIRTUAL

    virtual
    void FireGCLOHCompact(uint16_t count, uint32_t valuesLen, void *values) PURE_VIRTUAL

    virtual
    void FireGCFitBucketInfo(uint16_t bucketKind, size_t size, uint16_t count, uint32_t valuesLen, void *values) PURE_VIRTUAL

    virtual
    void FireBGCBegin() PURE_VIRTUAL
    virtual
    void FireBGC1stNonConEnd() PURE_VIRTUAL
    virtual
    void FireBGC1stConEnd() PURE_VIRTUAL
    virtual
    void FireBGC1stSweepEnd(uint32_t genNumber) PURE_VIRTUAL
    virtual
    void FireBGC2ndNonConBegin() PURE_VIRTUAL
    virtual
    void FireBGC2ndNonConEnd() PURE_VIRTUAL
    virtual
    void FireBGC2ndConBegin() PURE_VIRTUAL
    virtual
    void FireBGC2ndConEnd() PURE_VIRTUAL
    virtual
    void FireBGCDrainMark(uint64_t objects) PURE_VIRTUAL
    virtual
    void FireBGCRevisit(uint64_t pages, uint64_t objects, uint32_t isLarge) PURE_VIRTUAL
    virtual
    void FireBGCOverflow_V1(uint64_t min, uint64_t max, uint64_t objects, uint32_t isLarge, uint32_t genNumber) PURE_VIRTUAL
    virtual
    void FireBGCAllocWaitBegin(uint32_t reason) PURE_VIRTUAL
    virtual
    void FireBGCAllocWaitEnd(uint32_t reason) PURE_VIRTUAL
    virtual
    void FireGCFullNotify_V1(uint32_t genNumber, uint32_t isAlloc) PURE_VIRTUAL
    virtual
    void FireSetGCHandle(void *handleID, void *objectID, uint32_t kind, uint32_t generation) PURE_VIRTUAL
    virtual
    void FirePrvSetGCHandle(void *handleID, void *objectID, uint32_t kind, uint32_t generation) PURE_VIRTUAL
    virtual
    void FireDestroyGCHandle(void *handleID) PURE_VIRTUAL
    virtual
    void FirePrvDestroyGCHandle(void *handleID) PURE_VIRTUAL
};

// This interface provides the interface that the GC will use to speak to the rest
// of the execution engine. Everything that the GC does that requires the EE
// to be informed or that requires EE action must go through this interface.
//
// When BUILD_AS_STANDALONE is defined, this class is named IGCToCLR and is
// an abstract class. The EE will provide a class that fulfills this interface,
// and the GC will dispatch virtually on it to call into the EE. When BUILD_AS_STANDALONE
// is not defined, this class is named GCToEEInterface and the GC will dispatch statically on it.
class IGCToCLR {
public:
    // Suspends the EE for the given reason.
    virtual
    void SuspendEE(SUSPEND_REASON reason) PURE_VIRTUAL

    // Resumes all paused threads, with a boolean indicating
    // if the EE is being restarted because a GC is complete.
    virtual
    void RestartEE(bool bFinishedGC) PURE_VIRTUAL

    // Performs a stack walk of all managed threads and invokes the given promote_func
    // on all GC roots encountered on the stack. Depending on the condemned generation,
    // this function may also enumerate all static GC refs if necessary.
    virtual
    void GcScanRoots(promote_func* fn, int condemned, int max_gen, ScanContext* sc) PURE_VIRTUAL

    // Performs a stack walk of current thread and invokes the given promote_func
    // on all GC roots encountered on the stack.
    virtual
    void GcScanCurrentStackRoots(promote_func* fn, ScanContext* sc) = 0;

    // Callback from the GC informing the EE that it is preparing to start working.
    virtual
    void GcStartWork(int condemned, int max_gen) PURE_VIRTUAL

    // Callback from the GC informing the EE that the scanning of roots is about
    // to begin.
    virtual
    void BeforeGcScanRoots(int condemned, bool is_bgc, bool is_concurrent) PURE_VIRTUAL

    // Callback from the GC informing the EE that it has completed the managed stack
    // scan. User threads are still suspended at this point.
    virtual
    void AfterGcScanRoots(int condemned, int max_gen, ScanContext* sc) PURE_VIRTUAL

    // Callback from the GC informing the EE that a GC has completed.
    virtual
    void GcDone(int condemned) PURE_VIRTUAL

    // Predicate for the GC to query whether or not a given refcounted handle should
    // be promoted.
    virtual
    bool RefCountedHandleCallbacks(Object * pObject) PURE_VIRTUAL

    // Performs a weak pointer scan of the sync block cache.
    virtual
    void SyncBlockCacheWeakPtrScan(HANDLESCANPROC scanProc, uintptr_t lp1, uintptr_t lp2) PURE_VIRTUAL

    // Indicates to the EE that the GC intends to demote objects in the sync block cache.
    virtual
    void SyncBlockCacheDemote(int max_gen) PURE_VIRTUAL

    // Indicates to the EE that the GC has granted promotion to objects in the sync block cache.
    virtual
    void SyncBlockCachePromotionsGranted(int max_gen) PURE_VIRTUAL

    virtual
    uint32_t GetActiveSyncBlockCount() PURE_VIRTUAL

    // Queries whether or not the current thread has preemptive GC disabled.
    virtual
    bool IsPreemptiveGCDisabled() PURE_VIRTUAL

    // Enables preemptive GC on the current thread. Returns true if the thread mode
    // was changed and false if the thread mode wasn't changed or the thread is not
    // a managed thread.
    virtual
    bool EnablePreemptiveGC() PURE_VIRTUAL

    // Disables preemptive GC on the current thread.
    virtual
    void DisablePreemptiveGC() PURE_VIRTUAL

    // If suspension is requiested suspends the current thread.
    virtual
    void GcPoll() = 0;

    // Gets the Thread instance for the current thread, or null if no thread
    // instance is associated with this thread.
    //
    // If the GC created the current thread, GetThread returns null for threads
    // that were not created as suspendable (see `IGCHeap::CreateThread`).
    virtual
    Thread* GetThread() PURE_VIRTUAL

    // Retrieves the alloc context associated with the current thread.
    virtual
    gc_alloc_context * GetAllocContext() PURE_VIRTUAL

    // Calls the given enum_alloc_context_func with every active alloc context.
    virtual
    void GcEnumAllocContexts(enum_alloc_context_func* fn, void* param) PURE_VIRTUAL

    // Get the Allocator for objects from collectible assemblies
    virtual
    uint8_t* GetLoaderAllocatorObjectForGC(Object* pObject) PURE_VIRTUAL

    // Creates and returns a new thread.
    // Parameters:
    //  threadStart - The function that will serve as the thread stub for the
    //                new thread. It will be invoked immediately upon the
    //                new thread upon creation.
    //  arg - The argument that will be passed verbatim to threadStart.
    //  is_suspendable - Whether or not the thread that is created should be suspendable
    //                   from a runtime perspective. Threads that are suspendable have
    //                   a VM Thread object associated with them that can be accessed
    //                   using `IGCHeap::GetThread`.
    //  name - The name of this thread, optionally used for diagnostic purposes.
    // Returns:
    //  true if the thread was started successfully, false if not.
    virtual
    bool CreateThread(void (*threadStart)(void*), void* arg, bool is_suspendable, const char* name) PURE_VIRTUAL

    // When a GC starts, gives the diagnostics code a chance to run.
    virtual
    void DiagGCStart(int gen, bool isInduced) PURE_VIRTUAL

    // When GC heap segments change, gives the diagnostics code a chance to run.
    virtual
    void DiagUpdateGenerationBounds() PURE_VIRTUAL

    // When a GC ends, gives the diagnostics code a chance to run.
    virtual
    void DiagGCEnd(size_t index, int gen, int reason, bool fConcurrent) PURE_VIRTUAL

    // During a GC after we discover what objects' finalizers should run, gives the diagnostics code a chance to run.
    virtual
    void DiagWalkFReachableObjects(void* gcContext) PURE_VIRTUAL

    // During a GC after we discover the survivors and the relocation info,
    // gives the diagnostics code a chance to run. This includes LOH if we are
    // compacting LOH.
    virtual
    void DiagWalkSurvivors(void* gcContext, bool fCompacting) PURE_VIRTUAL

    // During a full GC after we discover what objects to survive on UOH,
    // gives the diagnostics code a chance to run.
    virtual
    void DiagWalkUOHSurvivors(void* gcContext, int gen) PURE_VIRTUAL

    // At the end of a background GC, gives the diagnostics code a chance to run.
    virtual
    void DiagWalkBGCSurvivors(void* gcContext) PURE_VIRTUAL

    // Informs the EE of changes to the location of the card table, potentially updating the write
    // barrier if it needs to be updated.
    virtual
    void StompWriteBarrier(WriteBarrierParameters* args) PURE_VIRTUAL

    // Signals to the finalizer thread that there are objects ready to
    // be finalized.
    virtual
    void EnableFinalization(bool gcHasWorkForFinalizerThread) PURE_VIRTUAL

    // Signals to the EE that the GC encountered a fatal error and can't recover.
    virtual
    void HandleFatalError(unsigned int exitCode) PURE_VIRTUAL

    // Offers the EE the option to finalize the given object eagerly, i.e.
    // not on the finalizer thread but on the current thread. The
    // EE returns true if it finalized the object eagerly and the GC does not
    // need to do so, and false if it chose not to eagerly finalize the object
    // and it's up to the GC to finalize it later.
    virtual
    bool EagerFinalized(Object* obj) PURE_VIRTUAL

    // Retrieves the method table for the free object, a special kind of object used by the GC
    // to keep the heap traversable. Conceptually, the free object is similar to a managed array
    // of bytes: it consists of an object header (like all objects) and a "numComponents" field,
    // followed by some number of bytes of space that's free on the heap.
    //
    // The free object allows the GC to traverse the heap because it can inspect the numComponents
    // field to see how many bytes to skip before the next object on a heap segment begins.
    virtual
    MethodTable* GetFreeObjectMethodTable() PURE_VIRTUAL

    // Asks the EE for the value of a given configuration key. If the EE does not know or does not
    // have a value for the requeested config key, false is returned and the value of the passed-in
    // pointer is undefined. Otherwise, true is returned and the config key's value is written to
    // the passed-in pointer.
    virtual
    bool GetBooleanConfigValue(const char* privateKey, const char* publicKey, bool* value) PURE_VIRTUAL

    virtual
    bool GetIntConfigValue(const char* privateKey, const char* publicKey, int64_t* value) PURE_VIRTUAL

    virtual
    bool GetStringConfigValue(const char* privateKey, const char* publicKey, const char** value) PURE_VIRTUAL

    virtual
    void FreeStringConfigValue(const char* value) PURE_VIRTUAL

    // Returns true if this thread is a "GC thread", or a thread capable of
    // doing GC work. Threads are either /always/ GC threads
    // (if they were created for this purpose - background GC threads
    // and server GC threads) or they became GC threads by suspending the EE
    // and initiating a collection.
    virtual
    bool IsGCThread() PURE_VIRTUAL

    // Returns true if the current thread is either a background GC thread
    // or a server GC thread.
    virtual
    bool WasCurrentThreadCreatedByGC() PURE_VIRTUAL

    // Given an object, if this object is an instance of `System.Threading.OverlappedData`,
    // and the runtime treats instances of this class specially, traverses the objects that
    // are directly or (once) indirectly pinned by this object and reports them to the GC for
    // the purposes of relocation and promotion.
    //
    // Overlapped objects are very special and as such the objects they wrap can't be promoted in
    // the same manner as normal objects. This callback gives the EE the opportunity to hide these
    // details, if they are implemented at all.
    //
    // This function is a no-op if "object" is not an OverlappedData object.
    virtual
    void WalkAsyncPinnedForPromotion(Object* object, ScanContext* sc, promote_func* callback) PURE_VIRTUAL

    // Given an object, if this object is an instance of `System.Threading.OverlappedData` and the
    // runtime treats instances of this class specially, traverses the objects that are directly
    // or once indirectly pinned by this object and invokes the given callback on them. The callback
    // is passed the following arguments:
    //     Object* "from" - The object that "caused" the "to" object to be pinned. If a single object
    //                      is pinned directly by this OverlappedData, this object will be the
    //                      OverlappedData object itself. If an array is pinned by this OverlappedData,
    //                      this object will be the pinned array.
    //     Object* "to"   - The object that is pinned by the "from" object. If a single object is pinned
    //                      by an OverlappedData, "to" will be that single object. If an array is pinned
    //                      by an OverlappedData, the callback will be invoked on all elements of that
    //                      array and each element will be a "to" object.
    //     void* "context" - Passed verbatim from "WalkOverlappedObject" to the callback function.
    // The "context" argument will be passed directly to the callback without modification or inspection.
    //
    // This function is a no-op if "object" is not an OverlappedData object.
    virtual
    void WalkAsyncPinned(Object* object, void* context, void(*callback)(Object*, Object*, void*)) PURE_VIRTUAL

    // Returns an IGCToCLREventSink instance that can be used to fire events.
    virtual
    IGCToCLREventSink* EventSink() PURE_VIRTUAL

    virtual
    uint32_t GetTotalNumSizedRefHandles() PURE_VIRTUAL

    virtual
    bool AnalyzeSurvivorsRequested(int condemnedGeneration) PURE_VIRTUAL

    virtual
    void AnalyzeSurvivorsFinished(size_t gcIndex, int condemnedGeneration, uint64_t promoted_bytes, void (*reportGenerationBounds)()) PURE_VIRTUAL

    virtual
    void VerifySyncTableEntry() PURE_VIRTUAL

    virtual
    void UpdateGCEventStatus(int publicLevel, int publicKeywords, int privateLEvel, int privateKeywords) PURE_VIRTUAL

    virtual
    void LogStressMsg(unsigned level, unsigned facility, const StressLogMsg& msg) PURE_VIRTUAL

    virtual
    uint32_t GetCurrentProcessCpuCount() PURE_VIRTUAL

    virtual
    void DiagAddNewRegion(int generation, uint8_t* rangeStart, uint8_t* rangeEnd, uint8_t* rangeEndReserved) PURE_VIRTUAL

    // The following method is available only with EE_INTERFACE_MAJOR_VERSION >= 1
    virtual
    void LogErrorToHost(const char *message) PURE_VIRTUAL
};

#endif // _GCINTERFACE_EE_H_
