// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.
//
// SatoriGCHeap.cpp
//

#include "common.h"

#include "gcenv.h"
#include "../env/gcenv.os.h"

#include "SatoriUtil.h"

#include "SatoriGCHeap.h"
#include "SatoriAllocationContext.h"
#include "SatoriHeap.h"

bool SatoriGCHeap::IsValidSegmentSize(size_t size)
{
    __UNREACHABLE();
    return false;
}

bool SatoriGCHeap::IsValidGen0MaxSize(size_t size)
{
    __UNREACHABLE();
    return false;
}

size_t SatoriGCHeap::GetValidSegmentSize(bool large_seg)
{
    __UNREACHABLE();
    return 0;
}

void SatoriGCHeap::SetReservedVMLimit(size_t vmlimit)
{
    __UNREACHABLE();
}

void SatoriGCHeap::WaitUntilConcurrentGCComplete()
{
}

bool SatoriGCHeap::IsConcurrentGCInProgress()
{
    return false;
}

void SatoriGCHeap::TemporaryEnableConcurrentGC()
{
}

void SatoriGCHeap::TemporaryDisableConcurrentGC()
{
}

bool SatoriGCHeap::IsConcurrentGCEnabled()
{
    return false;
}

HRESULT SatoriGCHeap::WaitUntilConcurrentGCCompleteAsync(int millisecondsTimeout)
{
    return S_OK;
}

void SatoriGCHeap::SetFinalizeQueueForShutdown(bool fHasLock)
{
    __UNREACHABLE();
}

size_t SatoriGCHeap::GetNumberOfFinalizable()
{
    __UNREACHABLE();
    return 0;
}

bool SatoriGCHeap::ShouldRestartFinalizerWatchDog()
{
    __UNREACHABLE();
    return false;
}

Object* SatoriGCHeap::GetNextFinalizable()
{
    return nullptr;
}

void SatoriGCHeap::SetFinalizeRunOnShutdown(bool value)
{
    __UNREACHABLE();
}

void SatoriGCHeap::GetMemoryInfo(uint64_t* highMemLoadThresholdBytes, uint64_t* totalPhysicalMemoryBytes, uint64_t* lastRecordedMemLoadBytes, uint32_t* lastRecordedMemLoadPct, size_t* lastRecordedHeapSizeBytes, size_t* lastRecordedFragmentationBytes)
{
    __UNREACHABLE();
}

int SatoriGCHeap::GetGcLatencyMode()
{
    return 0;
}

int SatoriGCHeap::SetGcLatencyMode(int newLatencyMode)
{
    return 0;
}

int SatoriGCHeap::GetLOHCompactionMode()
{
    return 0;
}

void SatoriGCHeap::SetLOHCompactionMode(int newLOHCompactionMode)
{
}

bool SatoriGCHeap::RegisterForFullGCNotification(uint32_t gen2Percentage, uint32_t lohPercentage)
{
    return false;
}

bool SatoriGCHeap::CancelFullGCNotification()
{
    return false;
}

int SatoriGCHeap::WaitForFullGCApproach(int millisecondsTimeout)
{
    __UNREACHABLE();
    return 0;
}

int SatoriGCHeap::WaitForFullGCComplete(int millisecondsTimeout)
{
    __UNREACHABLE();
    return 0;
}

unsigned SatoriGCHeap::WhichGeneration(Object* obj)
{
    return 2;
}

int SatoriGCHeap::CollectionCount(int generation, int get_bgc_fgc_coutn)
{
    __UNREACHABLE();
    return 0;
}

int SatoriGCHeap::StartNoGCRegion(uint64_t totalSize, bool lohSizeKnown, uint64_t lohSize, bool disallowFullBlockingGC)
{
    __UNREACHABLE();
    return 0;
}

int SatoriGCHeap::EndNoGCRegion()
{
    __UNREACHABLE();
    return 0;
}

size_t SatoriGCHeap::GetTotalBytesInUse()
{
    __UNREACHABLE();
    return 0;
}

uint64_t SatoriGCHeap::GetTotalAllocatedBytes()
{
    __UNREACHABLE();
    return 0;
}

HRESULT SatoriGCHeap::GarbageCollect(int generation, bool low_memory_p, int mode)
{
    // TODO: Satori
    return S_OK;
}

unsigned SatoriGCHeap::GetMaxGeneration()
{
    __UNREACHABLE();
    return 0;
}

void SatoriGCHeap::SetFinalizationRun(Object* obj)
{
    //TODO: Satori Finalizers;
}

bool SatoriGCHeap::RegisterForFinalization(int gen, Object* obj)
{
    __UNREACHABLE();
    return false;
}

int SatoriGCHeap::GetLastGCPercentTimeInGC()
{
    __UNREACHABLE();
    return 0;
}

size_t SatoriGCHeap::GetLastGCGenerationSize(int gen)
{
    __UNREACHABLE();
    return 0;
}

void InitWriteBarrier()
{
    WriteBarrierParameters args = {};
    args.operation = WriteBarrierOp::Initialize;
    args.is_runtime_suspended = true;
    args.requires_upper_bounds_check = false;
    args.card_table = nullptr;

#ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
    args.card_bundle_table = nullptr;
#endif

    args.lowest_address = (uint8_t*)-1;
    args.highest_address = (uint8_t*)-1;
    args.ephemeral_low = (uint8_t*)-1;
    args.ephemeral_high = (uint8_t*)-1;
    GCToEEInterface::StompWriteBarrier(&args);
}

HRESULT SatoriGCHeap::Initialize()
{
    m_perfCounterFrequency = GCToOSInterface::QueryPerformanceFrequency();
    InitWriteBarrier();
    SatoriUtil::Initialize();
    m_heap = SatoriHeap::Create();
    if (m_heap == nullptr)
    {
        return E_OUTOFMEMORY;
    }

    return S_OK;
}

bool SatoriGCHeap::IsPromoted(Object* object)
{
    __UNREACHABLE();
    return false;
}

bool SatoriGCHeap::IsHeapPointer(void* object, bool small_heap_only)
{
    return m_heap->IsHeapAddress((uint8_t*)object);

    //TODO: Satori small_heap_only ?
}

unsigned SatoriGCHeap::GetCondemnedGeneration()
{
    __UNREACHABLE();
    return 0;
}

bool SatoriGCHeap::IsGCInProgressHelper(bool bConsiderGCStart)
{
    return m_gcInProgress;
}

unsigned SatoriGCHeap::GetGcCount()
{
    //TODO: Satori Collect
    return 0;
}

bool SatoriGCHeap::IsThreadUsingAllocationContextHeap(gc_alloc_context* acontext, int thread_number)
{
    __UNREACHABLE();
    return false;
}

bool SatoriGCHeap::IsEphemeral(Object* object)
{
    __UNREACHABLE();
    return false;
}

uint32_t SatoriGCHeap::WaitUntilGCComplete(bool bConsiderGCStart)
{
    return NOERROR;
}

void SatoriGCHeap::FixAllocContext(gc_alloc_context* acontext, void* arg, void* heap)
{
    // this is only called when thread is terminating and about to clear its context.
    ((SatoriAllocationContext*)acontext)->OnTerminateThread(m_heap);
}

size_t SatoriGCHeap::GetCurrentObjSize()
{
    __UNREACHABLE();
    return 0;
}

void SatoriGCHeap::SetGCInProgress(bool fInProgress)
{
    m_gcInProgress = fInProgress;
}

bool SatoriGCHeap::RuntimeStructuresValid()
{
    return true;
}

void SatoriGCHeap::SetSuspensionPending(bool fSuspensionPending)
{
    m_suspensionPending = fSuspensionPending;
}

void SatoriGCHeap::SetYieldProcessorScalingFactor(float yieldProcessorScalingFactor)
{
}

void SatoriGCHeap::Shutdown()
{
}

size_t SatoriGCHeap::GetLastGCStartTime(int generation)
{
    return 0;
}

size_t SatoriGCHeap::GetLastGCDuration(int generation)
{
    return 0;
}

size_t SatoriGCHeap::GetNow()
{
    int64_t t = GCToOSInterface::QueryPerformanceCounter();
    return (size_t)(t / (m_perfCounterFrequency / 1000));
}

Object* SatoriGCHeap::Alloc(gc_alloc_context* acontext, size_t size, uint32_t flags)
{
    return m_heap->Allocator()->Alloc((SatoriAllocationContext*)acontext, size, flags);
}

void SatoriGCHeap::PublishObject(uint8_t* obj)
{
}

void SatoriGCHeap::SetWaitForGCEvent()
{
}

void SatoriGCHeap::ResetWaitForGCEvent()
{
}

bool SatoriGCHeap::IsLargeObject(Object* pObj)
{
    return false;
}

void SatoriGCHeap::ValidateObjectMember(Object* obj)
{
}

Object* SatoriGCHeap::NextObj(Object* object)
{
    return nullptr;
}

Object* SatoriGCHeap::GetContainingObject(void* pInteriorPtr, bool fCollectedGenOnly)
{
    __UNREACHABLE();
    return nullptr;
}

void SatoriGCHeap::DiagWalkObject(Object* obj, walk_fn fn, void* context)
{
}

void SatoriGCHeap::DiagWalkObject2(Object* obj, walk_fn2 fn, void* context)
{
}

void SatoriGCHeap::DiagWalkHeap(walk_fn fn, void* context, int gen_number, bool walk_large_object_heap_p)
{
}

void SatoriGCHeap::DiagWalkSurvivorsWithType(void* gc_context, record_surv_fn fn, void* diag_context, walk_surv_type type, int gen_number)
{
}

void SatoriGCHeap::DiagWalkFinalizeQueue(void* gc_context, fq_walk_fn fn)
{
}

void SatoriGCHeap::DiagScanFinalizeQueue(fq_scan_fn fn, ScanContext* context)
{
}

void SatoriGCHeap::DiagScanHandles(handle_scan_fn fn, int gen_number, ScanContext* context)
{
}

void SatoriGCHeap::DiagScanDependentHandles(handle_scan_fn fn, int gen_number, ScanContext* context)
{
}

void SatoriGCHeap::DiagDescrGenerations(gen_walk_fn fn, void* context)
{
}

void SatoriGCHeap::DiagTraceGCSegments()
{
}

bool SatoriGCHeap::StressHeap(gc_alloc_context* acontext)
{
    return false;
}

segment_handle SatoriGCHeap::RegisterFrozenSegment(segment_info* pseginfo)
{
    __UNREACHABLE();
    return segment_handle();
}

void SatoriGCHeap::UnregisterFrozenSegment(segment_handle seg)
{
    __UNREACHABLE();
}

bool SatoriGCHeap::IsInFrozenSegment(Object* object)
{
    return false;
}

void SatoriGCHeap::ControlEvents(GCEventKeyword keyword, GCEventLevel level)
{
}

void SatoriGCHeap::ControlPrivateEvents(GCEventKeyword keyword, GCEventLevel level)
{
}

int SatoriGCHeap::GetNumberOfHeaps()
{
    // TODO: Satori   we return the max degree of concurrency here
    // return g_SystemInfo.dwNumberOfProcessors;
    return 1;
}

int SatoriGCHeap::GetHomeHeapNumber()
{
    // TODO: Satori   this is a number in [0, procNum) associated with thread
    //                it is implementable, do 0 for now.
    return 0;
}

size_t SatoriGCHeap::GetPromotedBytes(int heap_index)
{
    __UNREACHABLE();
    return 0;
}
