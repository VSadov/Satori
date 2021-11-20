// Copyright (c) 2022 Vladimir Sadov
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
// SatoriGC.cpp
//

#include "common.h"

#include "gcenv.h"
#include "../env/gcenv.os.h"

#include "SatoriHandlePartitioner.h"
#include "SatoriObject.h"
#include "SatoriObject.inl"
#include "SatoriGC.h"
#include "SatoriAllocationContext.h"
#include "SatoriHeap.h"
#include "SatoriRegion.h"
#include "SatoriPage.h"
#include "SatoriRegion.inl"
#include "../gceventstatus.h"

bool SatoriGC::IsValidSegmentSize(size_t size)
{
    __UNREACHABLE();
    return false;
}

bool SatoriGC::IsValidGen0MaxSize(size_t size)
{
    __UNREACHABLE();
    return false;
}

size_t SatoriGC::GetValidSegmentSize(bool large_seg)
{
    // Satori has no concept of a segment. This may be close enough.
    return Satori::REGION_SIZE_GRANULARITY;
}

void SatoriGC::SetReservedVMLimit(size_t vmlimit)
{
    __UNREACHABLE();
}

void SatoriGC::WaitUntilConcurrentGCComplete()
{
    WaitUntilGCComplete();
}

bool SatoriGC::IsConcurrentGCInProgress()
{
    // Satori may move thread local objects asynchronously,
    // but noone should see that (that is the point).
    //
    // The only thing that may get to TL objects is object verification.
    // Return "true" for now.
    return true;
}

void SatoriGC::TemporaryEnableConcurrentGC()
{
    // N/A
}

void SatoriGC::TemporaryDisableConcurrentGC()
{
    // N/A
}

bool SatoriGC::IsConcurrentGCEnabled()
{
    // N/A
    return true;
}

HRESULT SatoriGC::WaitUntilConcurrentGCCompleteAsync(int millisecondsTimeout)
{
    WaitUntilGCComplete();
    return S_OK;
}

size_t SatoriGC::GetNumberOfFinalizable()
{
    __UNREACHABLE();
    return 0;
}

Object* SatoriGC::GetNextFinalizable()
{
    SatoriFinalizationQueue* queue = m_heap->FinalizationQueue();
    Object* f = queue->TryGetNextItem();
    if (f == nullptr)
    {
        // transient failure to enqueue finalizables in ephemeral GC is unobservable since objects
        // could be promoted nondeterministicaly and stay alive anyways.
        // with gen2 we need to see a clean collection before we can claim there is nothing pending.
        if (queue->OverflowedGen() == 2)
        {
            m_heap->Recycler()->Collect(2, true, true);
            f = queue->TryGetNextItem();
        }
    }

    return f;
}

int SatoriGC::GetGcLatencyMode()
{
    // NYI
    return 0;
}

int SatoriGC::SetGcLatencyMode(int newLatencyMode)
{
    // NYI
    return 0;
}

int SatoriGC::GetLOHCompactionMode()
{
    // N/A
    return 0;
}

void SatoriGC::SetLOHCompactionMode(int newLOHCompactionMode)
{
    // N/A
}

bool SatoriGC::RegisterForFullGCNotification(uint32_t gen2Percentage, uint32_t lohPercentage)
{
    // NYI
    return false;
}

bool SatoriGC::CancelFullGCNotification()
{
    // NYI
    return false;
}

int SatoriGC::WaitForFullGCApproach(int millisecondsTimeout)
{
    // NYI
    // 1 - failed
    return 1;
}

int SatoriGC::WaitForFullGCComplete(int millisecondsTimeout)
{
    // NYI
    // 1 - failed
    return 1;
}

unsigned SatoriGC::WhichGeneration(Object* obj)
{
    SatoriObject* so = (SatoriObject*)obj;
    return (unsigned)so->ContainingRegion()->Generation();
}

int SatoriGC::CollectionCount(int generation, int get_bgc_fgc_coutn)
{
    //get_bgc_fgc_coutn N/A
    if ((unsigned)generation > (unsigned)2)
    {
        return 0;
    }

    return (int)m_heap->Recycler()->GetCollectionCount(generation);
}

int SatoriGC::StartNoGCRegion(uint64_t totalSize, bool lohSizeKnown, uint64_t lohSize, bool disallowFullBlockingGC)
{
    // NYI
    // 1 - NotEnoughMemory
    return 1;
}

int SatoriGC::EndNoGCRegion()
{
    // NYI
    return 0;
}

size_t SatoriGC::GetTotalBytesInUse()
{
    // Returns the total number of bytes currently in use by live objects in
    // the GC heap.  This does not return the total size of the GC heap, but
    // only the live objects in the GC heap.
    return m_heap->Recycler()->GetTotalOccupancy();
}

size_t SatoriGC::GetCurrentObjSize()
{
    // Used only by mem pressure heuristic
    // It seems some rough estimate of "managed heap size"
    return GetTotalBytesInUse();
}

uint64_t SatoriGC::GetTotalAllocatedBytes()
{
    // monotonically increasing number produced by allocator when allocating objects.
    // threads know the number and we update the total when doing GCs
    return m_heap->Recycler()->GetTotalAllocatedBytes();
}

HRESULT SatoriGC::GarbageCollect(int generation, bool low_memory_p, int mode)
{
    // we do either Gen1 or Gen2 for now.
    generation = (generation < 0) ? 2 : min(generation, 2);
    generation = max(1, generation);

    // TODO: VS forced compaction
    m_heap->Recycler()->Collect(
        generation,
        !(mode & collection_mode::collection_optimized),
        mode & collection_mode::collection_blocking
    );

    return S_OK;
}

unsigned SatoriGC::GetMaxGeneration()
{
    return 2;
}

void SatoriGC::SetFinalizationRun(Object* obj)
{
    ((SatoriObject*)obj)->SuppressFinalization();
}

bool SatoriGC::RegisterForFinalization(int gen, Object* obj)
{
    SatoriObject* so = (SatoriObject*)obj;
    _ASSERTE(so->RawGetMethodTable()->HasFinalizer());

    if (so->IsFinalizationSuppressed())
    {
        so->UnSuppressFinalization();
        return true;
    }
    else
    {        
        return so->ContainingRegion()->RegisterForFinalization(so);
    }
}

int SatoriGC::GetLastGCPercentTimeInGC()
{
    // NYI
    return 0;
}

size_t SatoriGC::GetLastGCGenerationSize(int gen)
{
    // NYI
    __UNREACHABLE();
    return 0;
}

HRESULT SatoriGC::Initialize()
{
    SatoriObject::Initialize();
    SatoriHandlePartitioner::Initialize();
    m_heap = SatoriHeap::Create();
    if (m_heap == nullptr)
    {
        return E_OUTOFMEMORY;
    }

    m_waitForGCEvent = new (nothrow) GCEvent;
    if (!m_waitForGCEvent)
    {
        return E_OUTOFMEMORY;
    }

    if (!m_waitForGCEvent->CreateManualEventNoThrow(TRUE))
    {
        return E_FAIL;
    }

    return S_OK;
}

// actually checks if object is considered reachable as a result of a marking phase.
bool SatoriGC::IsPromoted(Object* object)
{
    _ASSERTE(object == nullptr || m_heap->IsHeapAddress((size_t)object));
    SatoriObject* o = (SatoriObject*)object;

    // objects outside of the collected generation (including null) are considered marked.
    // (existing behavior)
    return o == nullptr ||
        o->IsMarkedOrOlderThan(m_heap->Recycler()->CondemnedGeneration());
}

bool SatoriGC::IsHeapPointer(void* object, bool small_heap_only)
{
    //small_heap_only is unused - there is no special heap for large objects.
    return m_heap->IsHeapAddress((size_t)object);
}

unsigned SatoriGC::GetCondemnedGeneration()
{
    return m_heap->Recycler()->CondemnedGeneration();
}

bool SatoriGC::IsGCInProgressHelper(bool bConsiderGCStart)
{
    return m_gcInProgress;
}

unsigned SatoriGC::GetGcCount()
{
    if (!m_heap)
    {
        return 0;
    }

    return (unsigned)(int)m_heap->Recycler()->GetCollectionCount(/*gen*/ 1);
}

bool SatoriGC::IsThreadUsingAllocationContextHeap(gc_alloc_context* acontext, int thread_number)
{
    // TODO: VS should prefer when running on the same core as recorded in alloc region, if present.
    //       negative thread_number could indicate "do not care"
    //       also need to assign numbers to threads when scanning.
    //       at very least there is dependency on 0 being unique.

    // for now we just return true if given context has not been scanned for the current scan ticket. 
    int currentScanTicket = m_heap->Recycler()->GetRootScanTicket();
    int threadScanTicket = VolatileLoadWithoutBarrier(&acontext->alloc_count);
    if (threadScanTicket != currentScanTicket)
    {
        if (Interlocked::CompareExchange(&acontext->alloc_count, currentScanTicket, threadScanTicket) == threadScanTicket)
        {
            return true;
        }
    }

    return false;
}

bool SatoriGC::IsEphemeral(Object* object)
{
    return WhichGeneration(object) != GetMaxGeneration();
}

uint32_t SatoriGC::WaitUntilGCComplete(bool bConsiderGCStart)
{
    //bConsiderGCStart does not apply for Satori
    if (m_gcInProgress)
    {
        _ASSERTE(m_waitForGCEvent->IsValid());
        return m_waitForGCEvent->Wait(INFINITE, FALSE);
    }

    return NOERROR;
}

void SatoriGC::FixAllocContext(gc_alloc_context* acontext, void* arg, void* heap)
{
    // this is only called when thread is terminating and about to clear its context.
    ((SatoriAllocationContext*)acontext)->Deactivate(m_heap->Recycler(), /*detach*/ true);
    m_heap->Recycler()->ReportThreadAllocBytes(acontext->alloc_bytes + acontext->alloc_bytes_uoh, /*isLive*/ false);
}

void SatoriGC::SetGCInProgress(bool fInProgress)
{
    m_gcInProgress = fInProgress;
}

bool SatoriGC::RuntimeStructuresValid()
{
    // N/A
    return true;
}

void SatoriGC::SetSuspensionPending(bool fSuspensionPending)
{
    // N/A
    // noop, it makes no difference.
}

void SatoriGC::SetYieldProcessorScalingFactor(float yieldProcessorScalingFactor)
{
    // N/A
}

void SatoriGC::Shutdown()
{
    m_shuttingDown = true;
    m_heap->Recycler()->ShutDown();
}

size_t SatoriGC::GetLastGCStartTime(int generation)
{
    return m_heap->Recycler()->GetGcStartMillis(generation);
}

size_t SatoriGC::GetLastGCDuration(int generation)
{
    // NYI  (this is used for mem pressure aggressiveness)
    return 10;
}

size_t SatoriGC::GetNow()
{
    return m_heap->Recycler()->GetNowMillis();
}

Object* SatoriGC::Alloc(gc_alloc_context* acontext, size_t size, uint32_t flags)
{
    return m_heap->Allocator()->Alloc((SatoriAllocationContext*)acontext, size, flags);
}

void SatoriGC::PublishObject(uint8_t* obj)
{
    SatoriObject* so = (SatoriObject*)obj;
    SatoriRegion* region = so->ContainingRegion();

    // we do not attach huge regions to thread contexts,
    // but the region is not parseable until the object has a MethodTable,
    // so we delay taking the region out of generation -1 and passing to recycler
    // until we get here.
    if (!region->IsAttachedToContext())
    {
        region->SetGenerationRelease(1);
        _ASSERTE(region->Size() > Satori::REGION_SIZE_GRANULARITY);
        if (!so->RawGetMethodTable()->ContainsPointers())
        {
            // this is a single-object region and it's body has no pointers.
            // it is relatively cheap to have it in gen1, so give it a chance to collect early.
            m_heap->Recycler()->AddEphemeralRegion(region);
        }
        else
        {
            // promote to gen2
            m_heap->Recycler()->AddTenuredRegion(region);

            // the region has seen no writes, so no need to worry about cards.
            // unless the obj has a collectible type.
            // in such case we simulate retroactive write by dirtying the card for the MT location.
            if (so->RawGetMethodTable()->Collectible())
            {
                region->ContainingPage()->DirtyCardForAddressUnordered(so->Start());
            }

            region->SetOccupancy(so->Size(), 1);
        }
    }
}

void SatoriGC::SetWaitForGCEvent()
{
    m_waitForGCEvent->Set();
}

void SatoriGC::ResetWaitForGCEvent()
{
    m_waitForGCEvent->Reset();
}

bool SatoriGC::IsLargeObject(Object* pObj)
{
    // N/A
    return false;
}

void SatoriGC::ValidateObjectMember(Object* obj)
{
    // NYI
}

Object* SatoriGC::NextObj(Object* object)
{
    // N/A
    return nullptr;
}

Object* SatoriGC::GetContainingObject(void* pInteriorPtr, bool fCollectedGenOnly)
{
    SatoriRegion* region = m_heap->RegionForAddressChecked((size_t)pInteriorPtr);
    if (!region)
    {
        return nullptr;
    }

    if (fCollectedGenOnly &&
        region->Generation() > m_heap->Recycler()->CondemnedGeneration())
    {
        return nullptr;
    }

    return region->FindObject((size_t)pInteriorPtr);
}

void SatoriGC::DiagWalkObject(Object* obj, walk_fn fn, void* context)
{
    // NYI
}

void SatoriGC::DiagWalkObject2(Object* obj, walk_fn2 fn, void* context)
{
    // NYI
}

void SatoriGC::DiagWalkHeap(walk_fn fn, void* context, int gen_number, bool walk_large_object_heap_p)
{
    // NYI
}

void SatoriGC::DiagWalkSurvivorsWithType(void* gc_context, record_surv_fn fn, void* diag_context, walk_surv_type type, int gen_number)
{
    // NYI
}

void SatoriGC::DiagWalkFinalizeQueue(void* gc_context, fq_walk_fn fn)
{
    // NYI
}

void SatoriGC::DiagScanFinalizeQueue(fq_scan_fn fn, ScanContext* context)
{
    // NYI
}

void SatoriGC::DiagScanHandles(handle_scan_fn fn, int gen_number, ScanContext* context)
{
    // NYI
}

void SatoriGC::DiagScanDependentHandles(handle_scan_fn fn, int gen_number, ScanContext* context)
{
    // NYI
}

void SatoriGC::DiagDescrGenerations(gen_walk_fn fn, void* context)
{
    // NYI
}

void SatoriGC::DiagTraceGCSegments()
{
    // NYI
}

bool SatoriGC::StressHeap(gc_alloc_context* acontext)
{
    // N/A
    return false;
}

segment_handle SatoriGC::RegisterFrozenSegment(segment_info* pseginfo)
{
    // N/A
    return NULL;
}

void SatoriGC::UnregisterFrozenSegment(segment_handle seg)
{
    // N/A
}

bool SatoriGC::IsInFrozenSegment(Object* object)
{
    // N/A
    return false;
}

void SatoriGC::ControlEvents(GCEventKeyword keyword, GCEventLevel level)
{
    GCEventStatus::Set(GCEventProvider_Default, keyword, level);
}

void SatoriGC::ControlPrivateEvents(GCEventKeyword keyword, GCEventLevel level)
{
    GCEventStatus::Set(GCEventProvider_Private, keyword, level);
}

int SatoriGC::GetNumberOfHeaps()
{
    return SatoriHandlePartitioner::PartitionCount();
}

int SatoriGC::GetHomeHeapNumber()
{
    return SatoriHandlePartitioner::CurrentThreadPartition();
}

size_t SatoriGC::GetPromotedBytes(int heap_index)
{
    __UNREACHABLE();
    return 0;
}

void SatoriGC::GetMemoryInfo(uint64_t* highMemLoadThresholdBytes, uint64_t* totalAvailableMemoryBytes, uint64_t* lastRecordedMemLoadBytes, uint64_t* lastRecordedHeapSizeBytes, uint64_t* lastRecordedFragmentationBytes, uint64_t* totalCommittedBytes, uint64_t* promotedBytes, uint64_t* pinnedObjectCount, uint64_t* finalizationPendingCount, uint64_t* index, uint32_t* generation, uint32_t* pauseTimePct, bool* isCompaction, bool* isConcurrent, uint64_t* genInfoRaw, uint64_t* pauseInfoRaw, int kind)
{
    uint64_t totalLimit = GCToOSInterface::GetPhysicalMemoryLimit();
    *highMemLoadThresholdBytes = totalLimit * 99 / 100; // just say 99% for now
    *totalAvailableMemoryBytes = totalLimit;

    uint32_t memLoad;
    uint64_t availPhysical, availPage;
    GCToOSInterface::GetMemoryStatus(totalLimit, &memLoad, &availPhysical, &availPage);
    *lastRecordedMemLoadBytes = memLoad * totalLimit / 100;

    // the rest seems implementation specific and not strictly required.
    *lastRecordedHeapSizeBytes = 0;
    *lastRecordedFragmentationBytes = 0;
    *totalCommittedBytes = 0;
    *promotedBytes = 0;
    *pinnedObjectCount = 0;
    *finalizationPendingCount = 0;
    *index = 0;
    *generation = 0;
    *pauseTimePct = 0;
    *isCompaction = 0;
    *isConcurrent = 0;
    *genInfoRaw = 0;
    *pauseInfoRaw = 0;
}

uint32_t SatoriGC::GetMemoryLoad()
{
    uint32_t memLoad;
    uint64_t availPhysical, availPage;
    GCToOSInterface::GetMemoryStatus(0, &memLoad, &availPhysical, &availPage);

    return memLoad;
}

void SatoriGC::DiagGetGCSettings(EtwGCSettingsInfo* etw_settings)
{
    // NYI
}
