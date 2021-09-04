// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.
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
}

void SatoriGC::TemporaryDisableConcurrentGC()
{
}

bool SatoriGC::IsConcurrentGCEnabled()
{
    return true;
}

HRESULT SatoriGC::WaitUntilConcurrentGCCompleteAsync(int millisecondsTimeout)
{
    // TODO: VS wait until blocking gc state or none
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
    return 0;
}

int SatoriGC::SetGcLatencyMode(int newLatencyMode)
{
    return 0;
}

int SatoriGC::GetLOHCompactionMode()
{
    return 0;
}

void SatoriGC::SetLOHCompactionMode(int newLOHCompactionMode)
{
}

bool SatoriGC::RegisterForFullGCNotification(uint32_t gen2Percentage, uint32_t lohPercentage)
{
    return false;
}

bool SatoriGC::CancelFullGCNotification()
{
    return false;
}

int SatoriGC::WaitForFullGCApproach(int millisecondsTimeout)
{
    __UNREACHABLE();
    return 0;
}

int SatoriGC::WaitForFullGCComplete(int millisecondsTimeout)
{
    __UNREACHABLE();
    return 0;
}

unsigned SatoriGC::WhichGeneration(Object* obj)
{
    SatoriObject* so = (SatoriObject*)obj;
    return (unsigned)so->ContainingRegion()->Generation();
}

int SatoriGC::CollectionCount(int generation, int get_bgc_fgc_coutn)
{
    //TODO: VS get_bgc_fgc_coutn ?.
    return (int)m_heap->Recycler()->GetCollectionCount(generation);
}

int SatoriGC::StartNoGCRegion(uint64_t totalSize, bool lohSizeKnown, uint64_t lohSize, bool disallowFullBlockingGC)
{
    __UNREACHABLE();
    return 0;
}

int SatoriGC::EndNoGCRegion()
{
    __UNREACHABLE();
    return 0;
}

size_t SatoriGC::GetTotalBytesInUse()
{
    //TODO: VS, bytes used by objects? What is GetCurrentObjSize then?
    return Satori::REGION_SIZE_GRANULARITY * 10;
}

uint64_t SatoriGC::GetTotalAllocatedBytes()
{
    // monotonically increasing number ever produced by allocator. (only objects?)

    //TODO: VS would need some kind of counter incremented when allocating from regions 
    return Satori::REGION_SIZE_GRANULARITY * 10;
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
    __UNREACHABLE();
    return 0;
}

size_t SatoriGC::GetLastGCGenerationSize(int gen)
{
    __UNREACHABLE();
    return 0;
}

HRESULT SatoriGC::Initialize()
{
    m_perfCounterFrequency = GCToOSInterface::QueryPerformanceFrequency();
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
    //TODO: Satori small_heap_only ?
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
    //NB: bConsiderGCStart does not make much sense for Satori

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
}

size_t SatoriGC::GetCurrentObjSize()
{
    //TODO: Satori this should be implementable
    return 0;
}

void SatoriGC::SetGCInProgress(bool fInProgress)
{
    m_gcInProgress = fInProgress;
}

bool SatoriGC::RuntimeStructuresValid()
{
    return true;
}

void SatoriGC::SetSuspensionPending(bool fSuspensionPending)
{
    // noop, it makes no difference.
}

void SatoriGC::SetYieldProcessorScalingFactor(float yieldProcessorScalingFactor)
{
}

void SatoriGC::Shutdown()
{
    m_shuttingDown = true;
    m_heap->Recycler()->ShutDown();
}

size_t SatoriGC::GetLastGCStartTime(int generation)
{
    return 0;
}

size_t SatoriGC::GetLastGCDuration(int generation)
{
    return 0;
}

size_t SatoriGC::GetNow()
{
    int64_t t = GCToOSInterface::QueryPerformanceCounter();
    return (size_t)(t / (m_perfCounterFrequency / 1000));
}

Object* SatoriGC::Alloc(gc_alloc_context* acontext, size_t size, uint32_t flags)
{
    return m_heap->Allocator()->Alloc((SatoriAllocationContext*)acontext, size, flags);
}

void SatoriGC::PublishObject(uint8_t* obj)
{
    SatoriObject* so = (SatoriObject*)obj;
    SatoriRegion* region = so->ContainingRegion();

    // we do not retain huge regions in the nursery,
    // but we can't promote them until the object has a MethodTable.
    // do that here.
    if (!region->IsAllocating())
    {
        _ASSERTE(region->Size() > Satori::REGION_SIZE_GRANULARITY);
        if (!so->RawGetMethodTable()->ContainsPointers())
        {
            // this is a single-object region with no pointers.
            // it is rather cheap to have in gen1, so give it a chance to collect early.
            m_heap->Recycler()->AddEphemeralRegion(region, /* keep */ true);
        }
        else
        {
            // the region has seen no writes, so no need to worry about cards.
            // unless the obj has a collectible type.
            // in such case we simulate retroactive write by dirtying the card for the MT location.
            if (so->RawGetMethodTable()->Collectible())
            {
                region->ContainingPage()->DirtyCardForAddress(so->Start());
            }

            region->SetOccupancy(so->Size());
            m_heap->Recycler()->AddTenuredRegion(region);
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
    return false;
}

void SatoriGC::ValidateObjectMember(Object* obj)
{
}

Object* SatoriGC::NextObj(Object* object)
{
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
    // __UNREACHABLE();
}

void SatoriGC::DiagWalkObject2(Object* obj, walk_fn2 fn, void* context)
{
    // __UNREACHABLE();
}

void SatoriGC::DiagWalkHeap(walk_fn fn, void* context, int gen_number, bool walk_large_object_heap_p)
{
    // __UNREACHABLE();
}

void SatoriGC::DiagWalkSurvivorsWithType(void* gc_context, record_surv_fn fn, void* diag_context, walk_surv_type type, int gen_number)
{
    // __UNREACHABLE();
}

void SatoriGC::DiagWalkFinalizeQueue(void* gc_context, fq_walk_fn fn)
{
    // __UNREACHABLE();
}

void SatoriGC::DiagScanFinalizeQueue(fq_scan_fn fn, ScanContext* context)
{
    // __UNREACHABLE();
}

void SatoriGC::DiagScanHandles(handle_scan_fn fn, int gen_number, ScanContext* context)
{
    // __UNREACHABLE();
}

void SatoriGC::DiagScanDependentHandles(handle_scan_fn fn, int gen_number, ScanContext* context)
{
    __UNREACHABLE();
}

void SatoriGC::DiagDescrGenerations(gen_walk_fn fn, void* context)
{
    // __UNREACHABLE();
}

void SatoriGC::DiagTraceGCSegments()
{
    // __UNREACHABLE();
}

bool SatoriGC::StressHeap(gc_alloc_context* acontext)
{
    return false;
}

segment_handle SatoriGC::RegisterFrozenSegment(segment_info* pseginfo)
{
    __UNREACHABLE();
    return segment_handle();
}

void SatoriGC::UnregisterFrozenSegment(segment_handle seg)
{
    __UNREACHABLE();
}

bool SatoriGC::IsInFrozenSegment(Object* object)
{
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
    // TODO: Satori some of this makes sense and implementable.
    *highMemLoadThresholdBytes = (uint64_t)1 << 30;
    *totalAvailableMemoryBytes = (uint64_t)1 << 31;
    *lastRecordedMemLoadBytes = 0;
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
    // TODO: Satori this should be implementable
    return 0;
}

void SatoriGC::DiagGetGCSettings(EtwGCSettingsInfo* etw_settings)
{
}
