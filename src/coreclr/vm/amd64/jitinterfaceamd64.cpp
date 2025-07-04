// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// ===========================================================================
// File: JITinterfaceCpu.CPP
// ===========================================================================

// This contains JITinterface routines that are specific to the
// AMD64 platform. They are modeled after the X86 specific routines
// found in JITinterfaceX86.cpp or JIThelp.asm


#include "common.h"
#include "jitinterface.h"
#include "eeconfig.h"
#include "excep.h"
#include "threadsuspend.h"

extern uint8_t* g_ephemeral_low;
extern uint8_t* g_ephemeral_high;
extern uint32_t* g_card_table;
extern uint32_t* g_card_bundle_table;

// Patch Labels for the various write barriers
EXTERN_C void JIT_WriteBarrier_End();

EXTERN_C void JIT_WriteBarrier_PreGrow64(Object **dst, Object *ref);
EXTERN_C void JIT_WriteBarrier_PreGrow64_Patch_Label_Lower();
EXTERN_C void JIT_WriteBarrier_PreGrow64_Patch_Label_CardTable();
#ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
EXTERN_C void JIT_WriteBarrier_PreGrow64_Patch_Label_CardBundleTable();
#endif
EXTERN_C void JIT_WriteBarrier_PreGrow64_End();

EXTERN_C void JIT_WriteBarrier_PostGrow64(Object **dst, Object *ref);
EXTERN_C void JIT_WriteBarrier_PostGrow64_Patch_Label_Lower();
EXTERN_C void JIT_WriteBarrier_PostGrow64_Patch_Label_Upper();
EXTERN_C void JIT_WriteBarrier_PostGrow64_Patch_Label_CardTable();
#ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
EXTERN_C void JIT_WriteBarrier_PostGrow64_Patch_Label_CardBundleTable();
#endif
EXTERN_C void JIT_WriteBarrier_PostGrow64_End();

#ifdef FEATURE_SVR_GC
EXTERN_C void JIT_WriteBarrier_SVR64(Object **dst, Object *ref);
EXTERN_C void JIT_WriteBarrier_SVR64_PatchLabel_CardTable();
#ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
EXTERN_C void JIT_WriteBarrier_SVR64_PatchLabel_CardBundleTable();
#endif
EXTERN_C void JIT_WriteBarrier_SVR64_End();
#endif // FEATURE_SVR_GC

EXTERN_C void JIT_WriteBarrier_Byte_Region64(Object **dst, Object *ref);
EXTERN_C void JIT_WriteBarrier_Byte_Region64_Patch_Label_RegionToGeneration();
EXTERN_C void JIT_WriteBarrier_Byte_Region64_Patch_Label_RegionShrDest();
EXTERN_C void JIT_WriteBarrier_Byte_Region64_Patch_Label_Lower();
EXTERN_C void JIT_WriteBarrier_Byte_Region64_Patch_Label_Upper();
EXTERN_C void JIT_WriteBarrier_Byte_Region64_Patch_Label_RegionShrSrc();
EXTERN_C void JIT_WriteBarrier_Byte_Region64_Patch_Label_CardTable();
#ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
EXTERN_C void JIT_WriteBarrier_Byte_Region64_Patch_Label_CardBundleTable();
#endif
EXTERN_C void JIT_WriteBarrier_Byte_Region64_End();

EXTERN_C void JIT_WriteBarrier_Bit_Region64(Object **dst, Object *ref);
EXTERN_C void JIT_WriteBarrier_Bit_Region64_Patch_Label_RegionToGeneration();
EXTERN_C void JIT_WriteBarrier_Bit_Region64_Patch_Label_RegionShrDest();
EXTERN_C void JIT_WriteBarrier_Bit_Region64_Patch_Label_Lower();
EXTERN_C void JIT_WriteBarrier_Bit_Region64_Patch_Label_Upper();
EXTERN_C void JIT_WriteBarrier_Bit_Region64_Patch_Label_RegionShrSrc();
EXTERN_C void JIT_WriteBarrier_Bit_Region64_Patch_Label_CardTable();
#ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
EXTERN_C void JIT_WriteBarrier_Bit_Region64_Patch_Label_CardBundleTable();
#endif
EXTERN_C void JIT_WriteBarrier_Bit_Region64_End();


#ifdef FEATURE_SATORI_GC
EXTERN_C void JIT_WriteBarrier_SATORI(Object** dst, Object* ref);
EXTERN_C void JIT_WriteBarrier_SATORI_End();
#endif // FEATURE_SATORI_GC

#ifdef FEATURE_USE_SOFTWARE_WRITE_WATCH_FOR_GC_HEAP
EXTERN_C void JIT_WriteBarrier_WriteWatch_PreGrow64(Object **dst, Object *ref);
EXTERN_C void JIT_WriteBarrier_WriteWatch_PreGrow64_Patch_Label_WriteWatchTable();
EXTERN_C void JIT_WriteBarrier_WriteWatch_PreGrow64_Patch_Label_Lower();
EXTERN_C void JIT_WriteBarrier_WriteWatch_PreGrow64_Patch_Label_CardTable();
#ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
EXTERN_C void JIT_WriteBarrier_WriteWatch_PreGrow64_Patch_Label_CardBundleTable();
#endif
EXTERN_C void JIT_WriteBarrier_WriteWatch_PreGrow64_End();

EXTERN_C void JIT_WriteBarrier_WriteWatch_PostGrow64(Object **dst, Object *ref);
EXTERN_C void JIT_WriteBarrier_WriteWatch_PostGrow64_Patch_Label_WriteWatchTable();
EXTERN_C void JIT_WriteBarrier_WriteWatch_PostGrow64_Patch_Label_Lower();
EXTERN_C void JIT_WriteBarrier_WriteWatch_PostGrow64_Patch_Label_Upper();
EXTERN_C void JIT_WriteBarrier_WriteWatch_PostGrow64_Patch_Label_CardTable();
#ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
EXTERN_C void JIT_WriteBarrier_WriteWatch_PostGrow64_Patch_Label_CardBundleTable();
#endif
EXTERN_C void JIT_WriteBarrier_WriteWatch_PostGrow64_End();

#ifdef FEATURE_SVR_GC
EXTERN_C void JIT_WriteBarrier_WriteWatch_SVR64(Object **dst, Object *ref);
EXTERN_C void JIT_WriteBarrier_WriteWatch_SVR64_PatchLabel_WriteWatchTable();
EXTERN_C void JIT_WriteBarrier_WriteWatch_SVR64_PatchLabel_CardTable();
#ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
EXTERN_C void JIT_WriteBarrier_WriteWatch_SVR64_PatchLabel_CardBundleTable();
#endif
EXTERN_C void JIT_WriteBarrier_WriteWatch_SVR64_End();
#endif // FEATURE_SVR_GC

EXTERN_C void JIT_WriteBarrier_WriteWatch_Byte_Region64(Object **dst, Object *ref);
EXTERN_C void JIT_WriteBarrier_WriteWatch_Byte_Region64_Patch_Label_WriteWatchTable();
EXTERN_C void JIT_WriteBarrier_WriteWatch_Byte_Region64_Patch_Label_RegionToGeneration();
EXTERN_C void JIT_WriteBarrier_WriteWatch_Byte_Region64_Patch_Label_RegionShrDest();
EXTERN_C void JIT_WriteBarrier_WriteWatch_Byte_Region64_Patch_Label_Lower();
EXTERN_C void JIT_WriteBarrier_WriteWatch_Byte_Region64_Patch_Label_Upper();
EXTERN_C void JIT_WriteBarrier_WriteWatch_Byte_Region64_Patch_Label_RegionShrSrc();
EXTERN_C void JIT_WriteBarrier_WriteWatch_Byte_Region64_Patch_Label_CardTable();
#ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
EXTERN_C void JIT_WriteBarrier_WriteWatch_Byte_Region64_Patch_Label_CardBundleTable();
#endif
EXTERN_C void JIT_WriteBarrier_WriteWatch_Byte_Region64_End();

EXTERN_C void JIT_WriteBarrier_WriteWatch_Bit_Region64(Object **dst, Object *ref);
EXTERN_C void JIT_WriteBarrier_WriteWatch_Bit_Region64_Patch_Label_WriteWatchTable();
EXTERN_C void JIT_WriteBarrier_WriteWatch_Bit_Region64_Patch_Label_RegionToGeneration();
EXTERN_C void JIT_WriteBarrier_WriteWatch_Bit_Region64_Patch_Label_RegionShrDest();
EXTERN_C void JIT_WriteBarrier_WriteWatch_Bit_Region64_Patch_Label_Lower();
EXTERN_C void JIT_WriteBarrier_WriteWatch_Bit_Region64_Patch_Label_Upper();
EXTERN_C void JIT_WriteBarrier_WriteWatch_Bit_Region64_Patch_Label_RegionShrSrc();
EXTERN_C void JIT_WriteBarrier_WriteWatch_Bit_Region64_Patch_Label_CardTable();
#ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
EXTERN_C void JIT_WriteBarrier_WriteWatch_Bit_Region64_Patch_Label_CardBundleTable();
#endif
EXTERN_C void JIT_WriteBarrier_WriteWatch_Bit_Region64_End();

#endif // FEATURE_USE_SOFTWARE_WRITE_WATCH_FOR_GC_HEAP

WriteBarrierManager g_WriteBarrierManager;

// Use this somewhat hokey macro to concatenate the function start with the patch
// label. This allows the code below to look relatively nice, but relies on the
// naming convention which we have established for these helpers.
#define CALC_PATCH_LOCATION(func,label,offset)      CalculatePatchLocation((PVOID)func, (PVOID)func##_##label, offset)

WriteBarrierManager::WriteBarrierManager() :
    m_currentWriteBarrier(WRITE_BARRIER_UNINITIALIZED)
{
    LIMITED_METHOD_CONTRACT;
}

#ifndef CODECOVERAGE        // Deactivate alignment validation for code coverage builds
                            // because the instrumentation tool will not preserve alignment
                            // constraints and we will fail.

void WriteBarrierManager::Validate()
{
    CONTRACTL
    {
        MODE_ANY;
        GC_NOTRIGGER;
        NOTHROW;
    }
    CONTRACTL_END;

    // we have an invariant that the addresses of all the values that we update in our write barrier
    // helpers must be naturally aligned, this is so that the update can happen atomically since there
    // are places where these values are updated while the EE is running
    // NOTE: we can't call this from the ctor since our infrastructure isn't ready for assert dialogs

    PBYTE pLowerBoundImmediate, pUpperBoundImmediate, pCardTableImmediate;

#ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
    PBYTE pCardBundleTableImmediate;
#endif

    pLowerBoundImmediate      = CALC_PATCH_LOCATION(JIT_WriteBarrier_PreGrow64, Patch_Label_Lower, 2);
    pCardTableImmediate       = CALC_PATCH_LOCATION(JIT_WriteBarrier_PreGrow64, Patch_Label_CardTable, 2);

    _ASSERTE_ALL_BUILDS((reinterpret_cast<UINT64>(pLowerBoundImmediate) & 0x7) == 0);
    _ASSERTE_ALL_BUILDS((reinterpret_cast<UINT64>(pCardTableImmediate) & 0x7) == 0);

#ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
    pCardBundleTableImmediate = CALC_PATCH_LOCATION(JIT_WriteBarrier_PreGrow64, Patch_Label_CardBundleTable, 2);
    _ASSERTE_ALL_BUILDS((reinterpret_cast<UINT64>(pCardBundleTableImmediate) & 0x7) == 0);
#endif

    pLowerBoundImmediate      = CALC_PATCH_LOCATION(JIT_WriteBarrier_PostGrow64, Patch_Label_Lower, 2);
    pUpperBoundImmediate      = CALC_PATCH_LOCATION(JIT_WriteBarrier_PostGrow64, Patch_Label_Upper, 2);
    pCardTableImmediate       = CALC_PATCH_LOCATION(JIT_WriteBarrier_PostGrow64, Patch_Label_CardTable, 2);
    _ASSERTE_ALL_BUILDS((reinterpret_cast<UINT64>(pLowerBoundImmediate) & 0x7) == 0);
    _ASSERTE_ALL_BUILDS((reinterpret_cast<UINT64>(pUpperBoundImmediate) & 0x7) == 0);
    _ASSERTE_ALL_BUILDS((reinterpret_cast<UINT64>(pCardTableImmediate) & 0x7) == 0);

#ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
    pCardBundleTableImmediate = CALC_PATCH_LOCATION(JIT_WriteBarrier_PostGrow64, Patch_Label_CardBundleTable, 2);
    _ASSERTE_ALL_BUILDS((reinterpret_cast<UINT64>(pCardBundleTableImmediate) & 0x7) == 0);
#endif

#ifdef FEATURE_SVR_GC
    pCardTableImmediate        = CALC_PATCH_LOCATION(JIT_WriteBarrier_SVR64, PatchLabel_CardTable, 2);
    _ASSERTE_ALL_BUILDS((reinterpret_cast<UINT64>(pCardTableImmediate) & 0x7) == 0);

#ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
    pCardBundleTableImmediate  = CALC_PATCH_LOCATION(JIT_WriteBarrier_SVR64, PatchLabel_CardBundleTable, 2);
    _ASSERTE_ALL_BUILDS((reinterpret_cast<UINT64>(pCardBundleTableImmediate) & 0x7) == 0);
#endif // FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
#endif // FEATURE_SVR_GC

    PBYTE pRegionToGenTableImmediate = CALC_PATCH_LOCATION(JIT_WriteBarrier_Byte_Region64, Patch_Label_RegionToGeneration, 2);
    _ASSERTE_ALL_BUILDS((reinterpret_cast<UINT64>(pRegionToGenTableImmediate) & 0x7) == 0);

    pLowerBoundImmediate      = CALC_PATCH_LOCATION(JIT_WriteBarrier_Byte_Region64, Patch_Label_Lower, 2);
    pUpperBoundImmediate      = CALC_PATCH_LOCATION(JIT_WriteBarrier_Byte_Region64, Patch_Label_Upper, 2);
    pCardTableImmediate       = CALC_PATCH_LOCATION(JIT_WriteBarrier_Byte_Region64, Patch_Label_CardTable, 2);
    _ASSERTE_ALL_BUILDS((reinterpret_cast<UINT64>(pLowerBoundImmediate) & 0x7) == 0);
    _ASSERTE_ALL_BUILDS((reinterpret_cast<UINT64>(pUpperBoundImmediate) & 0x7) == 0);
    _ASSERTE_ALL_BUILDS((reinterpret_cast<UINT64>(pCardTableImmediate) & 0x7) == 0);

#ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
    pCardBundleTableImmediate = CALC_PATCH_LOCATION(JIT_WriteBarrier_Byte_Region64, Patch_Label_CardBundleTable, 2);
    _ASSERTE_ALL_BUILDS((reinterpret_cast<UINT64>(pCardBundleTableImmediate) & 0x7) == 0);
#endif

    pRegionToGenTableImmediate = CALC_PATCH_LOCATION(JIT_WriteBarrier_Bit_Region64, Patch_Label_RegionToGeneration, 2);
    _ASSERTE_ALL_BUILDS((reinterpret_cast<UINT64>(pRegionToGenTableImmediate) & 0x7) == 0);

    pLowerBoundImmediate      = CALC_PATCH_LOCATION(JIT_WriteBarrier_Bit_Region64, Patch_Label_Lower, 2);
    pUpperBoundImmediate      = CALC_PATCH_LOCATION(JIT_WriteBarrier_Bit_Region64, Patch_Label_Upper, 2);
    pCardTableImmediate       = CALC_PATCH_LOCATION(JIT_WriteBarrier_Bit_Region64, Patch_Label_CardTable, 2);
    _ASSERTE_ALL_BUILDS((reinterpret_cast<UINT64>(pLowerBoundImmediate) & 0x7) == 0);
    _ASSERTE_ALL_BUILDS((reinterpret_cast<UINT64>(pUpperBoundImmediate) & 0x7) == 0);
    _ASSERTE_ALL_BUILDS((reinterpret_cast<UINT64>(pCardTableImmediate) & 0x7) == 0);

#ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
    pCardBundleTableImmediate = CALC_PATCH_LOCATION(JIT_WriteBarrier_Bit_Region64, Patch_Label_CardBundleTable, 2);
    _ASSERTE_ALL_BUILDS((reinterpret_cast<UINT64>(pCardBundleTableImmediate) & 0x7) == 0);
#endif

#ifdef FEATURE_USE_SOFTWARE_WRITE_WATCH_FOR_GC_HEAP
    PBYTE pWriteWatchTableImmediate;

    pWriteWatchTableImmediate = CALC_PATCH_LOCATION(JIT_WriteBarrier_WriteWatch_PreGrow64, Patch_Label_WriteWatchTable, 2);
    pLowerBoundImmediate = CALC_PATCH_LOCATION(JIT_WriteBarrier_WriteWatch_PreGrow64, Patch_Label_Lower, 2);
    pCardTableImmediate = CALC_PATCH_LOCATION(JIT_WriteBarrier_WriteWatch_PreGrow64, Patch_Label_CardTable, 2);

    _ASSERTE_ALL_BUILDS((reinterpret_cast<UINT64>(pWriteWatchTableImmediate) & 0x7) == 0);
    _ASSERTE_ALL_BUILDS((reinterpret_cast<UINT64>(pLowerBoundImmediate) & 0x7) == 0);
    _ASSERTE_ALL_BUILDS((reinterpret_cast<UINT64>(pCardTableImmediate) & 0x7) == 0);

#ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
    pCardBundleTableImmediate = CALC_PATCH_LOCATION(JIT_WriteBarrier_WriteWatch_PreGrow64, Patch_Label_CardBundleTable, 2);
    _ASSERTE_ALL_BUILDS((reinterpret_cast<UINT64>(pCardBundleTableImmediate) & 0x7) == 0);
#endif

    pWriteWatchTableImmediate = CALC_PATCH_LOCATION(JIT_WriteBarrier_WriteWatch_PostGrow64, Patch_Label_WriteWatchTable, 2);
    pLowerBoundImmediate = CALC_PATCH_LOCATION(JIT_WriteBarrier_WriteWatch_PostGrow64, Patch_Label_Lower, 2);
    pUpperBoundImmediate = CALC_PATCH_LOCATION(JIT_WriteBarrier_WriteWatch_PostGrow64, Patch_Label_Upper, 2);
    pCardTableImmediate = CALC_PATCH_LOCATION(JIT_WriteBarrier_WriteWatch_PostGrow64, Patch_Label_CardTable, 2);

    _ASSERTE_ALL_BUILDS((reinterpret_cast<UINT64>(pWriteWatchTableImmediate) & 0x7) == 0);
    _ASSERTE_ALL_BUILDS((reinterpret_cast<UINT64>(pLowerBoundImmediate) & 0x7) == 0);
    _ASSERTE_ALL_BUILDS((reinterpret_cast<UINT64>(pUpperBoundImmediate) & 0x7) == 0);
    _ASSERTE_ALL_BUILDS((reinterpret_cast<UINT64>(pCardTableImmediate) & 0x7) == 0);

#ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
    pCardBundleTableImmediate = CALC_PATCH_LOCATION(JIT_WriteBarrier_WriteWatch_PostGrow64, Patch_Label_CardBundleTable, 2);
    _ASSERTE_ALL_BUILDS((reinterpret_cast<UINT64>(pCardBundleTableImmediate) & 0x7) == 0);
#endif

#ifdef FEATURE_SVR_GC
    pWriteWatchTableImmediate = CALC_PATCH_LOCATION(JIT_WriteBarrier_WriteWatch_SVR64, PatchLabel_WriteWatchTable, 2);
    pCardTableImmediate = CALC_PATCH_LOCATION(JIT_WriteBarrier_WriteWatch_SVR64, PatchLabel_CardTable, 2);
    _ASSERTE_ALL_BUILDS((reinterpret_cast<UINT64>(pWriteWatchTableImmediate) & 0x7) == 0);
    _ASSERTE_ALL_BUILDS((reinterpret_cast<UINT64>(pCardTableImmediate) & 0x7) == 0);

#ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
    pCardBundleTableImmediate = CALC_PATCH_LOCATION(JIT_WriteBarrier_WriteWatch_SVR64, PatchLabel_CardBundleTable, 2);
    _ASSERTE_ALL_BUILDS((reinterpret_cast<UINT64>(pCardBundleTableImmediate) & 0x7) == 0);
#endif // FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
#endif // FEATURE_SVR_GC

    pRegionToGenTableImmediate = CALC_PATCH_LOCATION(JIT_WriteBarrier_WriteWatch_Byte_Region64, Patch_Label_RegionToGeneration, 2);
    _ASSERTE_ALL_BUILDS((reinterpret_cast<UINT64>(pRegionToGenTableImmediate) & 0x7) == 0);

    pLowerBoundImmediate      = CALC_PATCH_LOCATION(JIT_WriteBarrier_WriteWatch_Byte_Region64, Patch_Label_Lower, 2);
    pUpperBoundImmediate      = CALC_PATCH_LOCATION(JIT_WriteBarrier_WriteWatch_Byte_Region64, Patch_Label_Upper, 2);
    pCardTableImmediate       = CALC_PATCH_LOCATION(JIT_WriteBarrier_WriteWatch_Byte_Region64, Patch_Label_CardTable, 2);
    _ASSERTE_ALL_BUILDS((reinterpret_cast<UINT64>(pLowerBoundImmediate) & 0x7) == 0);
    _ASSERTE_ALL_BUILDS((reinterpret_cast<UINT64>(pUpperBoundImmediate) & 0x7) == 0);
    _ASSERTE_ALL_BUILDS((reinterpret_cast<UINT64>(pCardTableImmediate) & 0x7) == 0);

#ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
    pCardBundleTableImmediate = CALC_PATCH_LOCATION(JIT_WriteBarrier_WriteWatch_Byte_Region64, Patch_Label_CardBundleTable, 2);
    _ASSERTE_ALL_BUILDS((reinterpret_cast<UINT64>(pCardBundleTableImmediate) & 0x7) == 0);
#endif

    pRegionToGenTableImmediate = CALC_PATCH_LOCATION(JIT_WriteBarrier_WriteWatch_Bit_Region64, Patch_Label_RegionToGeneration, 2);
    _ASSERTE_ALL_BUILDS((reinterpret_cast<UINT64>(pRegionToGenTableImmediate) & 0x7) == 0);

    pLowerBoundImmediate      = CALC_PATCH_LOCATION(JIT_WriteBarrier_WriteWatch_Bit_Region64, Patch_Label_Lower, 2);
    pUpperBoundImmediate      = CALC_PATCH_LOCATION(JIT_WriteBarrier_WriteWatch_Bit_Region64, Patch_Label_Upper, 2);
    pCardTableImmediate       = CALC_PATCH_LOCATION(JIT_WriteBarrier_WriteWatch_Bit_Region64, Patch_Label_CardTable, 2);
    _ASSERTE_ALL_BUILDS((reinterpret_cast<UINT64>(pLowerBoundImmediate) & 0x7) == 0);
    _ASSERTE_ALL_BUILDS((reinterpret_cast<UINT64>(pUpperBoundImmediate) & 0x7) == 0);
    _ASSERTE_ALL_BUILDS((reinterpret_cast<UINT64>(pCardTableImmediate) & 0x7) == 0);

#ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
    pCardBundleTableImmediate = CALC_PATCH_LOCATION(JIT_WriteBarrier_WriteWatch_Bit_Region64, Patch_Label_CardBundleTable, 2);
    _ASSERTE_ALL_BUILDS((reinterpret_cast<UINT64>(pCardBundleTableImmediate) & 0x7) == 0);
#endif

#endif // FEATURE_USE_SOFTWARE_WRITE_WATCH_FOR_GC_HEAP
}

#endif // CODECOVERAGE


PCODE WriteBarrierManager::GetCurrentWriteBarrierCode()
{
    LIMITED_METHOD_CONTRACT;

    switch (m_currentWriteBarrier)
    {
        case WRITE_BARRIER_PREGROW64:
            return GetEEFuncEntryPoint(JIT_WriteBarrier_PreGrow64);
        case WRITE_BARRIER_POSTGROW64:
            return GetEEFuncEntryPoint(JIT_WriteBarrier_PostGrow64);
#ifdef FEATURE_SVR_GC
        case WRITE_BARRIER_SVR64:
            return GetEEFuncEntryPoint(JIT_WriteBarrier_SVR64);
#endif // FEATURE_SVR_GC
        case WRITE_BARRIER_BYTE_REGIONS64:
            return GetEEFuncEntryPoint(JIT_WriteBarrier_Byte_Region64);
        case WRITE_BARRIER_BIT_REGIONS64:
            return GetEEFuncEntryPoint(JIT_WriteBarrier_Bit_Region64);
#ifdef FEATURE_SATORI_GC
        case WRITE_BARRIER_SATORI:
            return GetEEFuncEntryPoint(JIT_WriteBarrier_SATORI);
#endif // FEATURE_SATORI_GC
#ifdef FEATURE_USE_SOFTWARE_WRITE_WATCH_FOR_GC_HEAP
        case WRITE_BARRIER_WRITE_WATCH_PREGROW64:
            return GetEEFuncEntryPoint(JIT_WriteBarrier_WriteWatch_PreGrow64);
        case WRITE_BARRIER_WRITE_WATCH_POSTGROW64:
            return GetEEFuncEntryPoint(JIT_WriteBarrier_WriteWatch_PostGrow64);
#ifdef FEATURE_SVR_GC
        case WRITE_BARRIER_WRITE_WATCH_SVR64:
            return GetEEFuncEntryPoint(JIT_WriteBarrier_WriteWatch_SVR64);
#endif // FEATURE_SVR_GC
        case WRITE_BARRIER_WRITE_WATCH_BYTE_REGIONS64:
            return GetEEFuncEntryPoint(JIT_WriteBarrier_WriteWatch_Byte_Region64);
        case WRITE_BARRIER_WRITE_WATCH_BIT_REGIONS64:
            return GetEEFuncEntryPoint(JIT_WriteBarrier_WriteWatch_Bit_Region64);
#endif // FEATURE_USE_SOFTWARE_WRITE_WATCH_FOR_GC_HEAP
        default:
            UNREACHABLE_MSG("unexpected m_currentWriteBarrier!");
    };
}

size_t WriteBarrierManager::GetSpecificWriteBarrierSize(WriteBarrierType writeBarrier)
{
// marked asm functions are those which use the LEAF_END_MARKED macro to end them which
// creates a public Name_End label which can be used to figure out their size without
// having to create unwind info.
#define MARKED_FUNCTION_SIZE(pfn)    (size_t)((LPBYTE)GetEEFuncEntryPoint(pfn##_End) - (LPBYTE)GetEEFuncEntryPoint(pfn))

    switch (writeBarrier)
    {
        case WRITE_BARRIER_PREGROW64:
            return MARKED_FUNCTION_SIZE(JIT_WriteBarrier_PreGrow64);
        case WRITE_BARRIER_POSTGROW64:
            return MARKED_FUNCTION_SIZE(JIT_WriteBarrier_PostGrow64);
#ifdef FEATURE_SVR_GC
        case WRITE_BARRIER_SVR64:
            return MARKED_FUNCTION_SIZE(JIT_WriteBarrier_SVR64);
#endif // FEATURE_SVR_GC
        case WRITE_BARRIER_BYTE_REGIONS64:
            return MARKED_FUNCTION_SIZE(JIT_WriteBarrier_Byte_Region64);
        case WRITE_BARRIER_BIT_REGIONS64:
            return MARKED_FUNCTION_SIZE(JIT_WriteBarrier_Bit_Region64);
#ifdef FEATURE_SATORI_GC
        case WRITE_BARRIER_SATORI:
            return MARKED_FUNCTION_SIZE(JIT_WriteBarrier_SATORI);
#endif // FEATURE_SATORI_GC
#ifdef FEATURE_USE_SOFTWARE_WRITE_WATCH_FOR_GC_HEAP
        case WRITE_BARRIER_WRITE_WATCH_PREGROW64:
            return MARKED_FUNCTION_SIZE(JIT_WriteBarrier_WriteWatch_PreGrow64);
        case WRITE_BARRIER_WRITE_WATCH_POSTGROW64:
            return MARKED_FUNCTION_SIZE(JIT_WriteBarrier_WriteWatch_PostGrow64);
#ifdef FEATURE_SVR_GC
        case WRITE_BARRIER_WRITE_WATCH_SVR64:
            return MARKED_FUNCTION_SIZE(JIT_WriteBarrier_WriteWatch_SVR64);
#endif // FEATURE_SVR_GC
        case WRITE_BARRIER_WRITE_WATCH_BYTE_REGIONS64:
            return MARKED_FUNCTION_SIZE(JIT_WriteBarrier_WriteWatch_Byte_Region64);
        case WRITE_BARRIER_WRITE_WATCH_BIT_REGIONS64:
            return MARKED_FUNCTION_SIZE(JIT_WriteBarrier_WriteWatch_Bit_Region64);
#endif // FEATURE_USE_SOFTWARE_WRITE_WATCH_FOR_GC_HEAP
        case WRITE_BARRIER_BUFFER:
            return MARKED_FUNCTION_SIZE(JIT_WriteBarrier);
        default:
            UNREACHABLE_MSG("unexpected m_currentWriteBarrier!");
    };
#undef MARKED_FUNCTION_SIZE
}

size_t WriteBarrierManager::GetCurrentWriteBarrierSize()
{
    return GetSpecificWriteBarrierSize(m_currentWriteBarrier);
}

PBYTE WriteBarrierManager::CalculatePatchLocation(LPVOID base, LPVOID label, int offset)
{
    // the label should always come after the entrypoint for this funtion
    _ASSERTE_ALL_BUILDS((LPBYTE)label > (LPBYTE)base);

    return (GetWriteBarrierCodeLocation((void*)JIT_WriteBarrier) + ((LPBYTE)GetEEFuncEntryPoint(label) - (LPBYTE)GetEEFuncEntryPoint(base) + offset));
}


int WriteBarrierManager::ChangeWriteBarrierTo(WriteBarrierType newWriteBarrier, bool isRuntimeSuspended)
{
    GCX_MAYBE_COOP_NO_THREAD_BROKEN((!isRuntimeSuspended && GetThreadNULLOk() != NULL));
    int stompWBCompleteActions = SWB_PASS;
    if (!isRuntimeSuspended && m_currentWriteBarrier != WRITE_BARRIER_UNINITIALIZED)
    {
        ThreadSuspend::SuspendEE(ThreadSuspend::SUSPEND_FOR_GC_PREP);
        stompWBCompleteActions |= SWB_EE_RESTART;
    }

    _ASSERTE(m_currentWriteBarrier != newWriteBarrier);
    m_currentWriteBarrier = newWriteBarrier;

    //TODO: Satori   we do not need to replace the barrier body for now
    //// the memcpy must come before the switch statement because the asserts inside the switch
    //// are actually looking into the JIT_WriteBarrier buffer
    //{
    //    ExecutableWriterHolder<void> writeBarrierWriterHolder(GetWriteBarrierCodeLocation((void*)JIT_WriteBarrier), GetCurrentWriteBarrierSize());
    //    memcpy(writeBarrierWriterHolder.GetRW(), (LPVOID)GetCurrentWriteBarrierCode(), GetCurrentWriteBarrierSize());
    //    stompWBCompleteActions |= SWB_ICACHE_FLUSH;
    //}

    switch (newWriteBarrier)
    {
#ifdef FEATURE_SATORI_GC
        case WRITE_BARRIER_SATORI:
            return stompWBCompleteActions;
#endif
        case WRITE_BARRIER_PREGROW64:
        {
            m_pLowerBoundImmediate      = CALC_PATCH_LOCATION(JIT_WriteBarrier_PreGrow64, Patch_Label_Lower, 2);
            m_pCardTableImmediate       = CALC_PATCH_LOCATION(JIT_WriteBarrier_PreGrow64, Patch_Label_CardTable, 2);

            // Make sure that we will be bashing the right places (immediates should be hardcoded to 0x0f0f0f0f0f0f0f0f0).
            _ASSERTE_ALL_BUILDS(0xf0f0f0f0f0f0f0f0 == *(UINT64*)m_pLowerBoundImmediate);
            _ASSERTE_ALL_BUILDS(0xf0f0f0f0f0f0f0f0 == *(UINT64*)m_pCardTableImmediate);

#ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
            m_pCardBundleTableImmediate = CALC_PATCH_LOCATION(JIT_WriteBarrier_PreGrow64, Patch_Label_CardBundleTable, 2);
            _ASSERTE_ALL_BUILDS(0xf0f0f0f0f0f0f0f0 == *(UINT64*)m_pCardBundleTableImmediate);
#endif
            break;
        }

        case WRITE_BARRIER_POSTGROW64:
        {
            m_pLowerBoundImmediate      = CALC_PATCH_LOCATION(JIT_WriteBarrier_PostGrow64, Patch_Label_Lower, 2);
            m_pUpperBoundImmediate      = CALC_PATCH_LOCATION(JIT_WriteBarrier_PostGrow64, Patch_Label_Upper, 2);
            m_pCardTableImmediate       = CALC_PATCH_LOCATION(JIT_WriteBarrier_PostGrow64, Patch_Label_CardTable, 2);

            // Make sure that we will be bashing the right places (immediates should be hardcoded to 0x0f0f0f0f0f0f0f0f0).
            _ASSERTE_ALL_BUILDS(0xf0f0f0f0f0f0f0f0 == *(UINT64*)m_pLowerBoundImmediate);
            _ASSERTE_ALL_BUILDS(0xf0f0f0f0f0f0f0f0 == *(UINT64*)m_pCardTableImmediate);
            _ASSERTE_ALL_BUILDS(0xf0f0f0f0f0f0f0f0 == *(UINT64*)m_pUpperBoundImmediate);

#ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
            m_pCardBundleTableImmediate = CALC_PATCH_LOCATION(JIT_WriteBarrier_PostGrow64, Patch_Label_CardBundleTable, 2);
            _ASSERTE_ALL_BUILDS(0xf0f0f0f0f0f0f0f0 == *(UINT64*)m_pCardBundleTableImmediate);
#endif
            break;
        }

#ifdef FEATURE_SVR_GC
        case WRITE_BARRIER_SVR64:
        {
            m_pCardTableImmediate       = CALC_PATCH_LOCATION(JIT_WriteBarrier_SVR64, PatchLabel_CardTable, 2);

            // Make sure that we will be bashing the right places (immediates should be hardcoded to 0x0f0f0f0f0f0f0f0f0).
            _ASSERTE_ALL_BUILDS(0xf0f0f0f0f0f0f0f0 == *(UINT64*)m_pCardTableImmediate);

#ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
            m_pCardBundleTableImmediate = CALC_PATCH_LOCATION(JIT_WriteBarrier_SVR64, PatchLabel_CardBundleTable, 2);
            _ASSERTE_ALL_BUILDS(0xf0f0f0f0f0f0f0f0 == *(UINT64*)m_pCardBundleTableImmediate);
#endif
            break;
        }
#endif // FEATURE_SVR_GC

        case WRITE_BARRIER_BYTE_REGIONS64:
            m_pRegionToGenTableImmediate = CALC_PATCH_LOCATION(JIT_WriteBarrier_Byte_Region64, Patch_Label_RegionToGeneration, 2);
            m_pRegionShrDest             = CALC_PATCH_LOCATION(JIT_WriteBarrier_Byte_Region64, Patch_Label_RegionShrDest, 3);
            m_pRegionShrSrc              = CALC_PATCH_LOCATION(JIT_WriteBarrier_Byte_Region64, Patch_Label_RegionShrSrc, 3);
            m_pLowerBoundImmediate       = CALC_PATCH_LOCATION(JIT_WriteBarrier_Byte_Region64, Patch_Label_Lower, 2);
            m_pUpperBoundImmediate       = CALC_PATCH_LOCATION(JIT_WriteBarrier_Byte_Region64, Patch_Label_Upper, 2);
            m_pCardTableImmediate        = CALC_PATCH_LOCATION(JIT_WriteBarrier_Byte_Region64, Patch_Label_CardTable, 2);

            // Make sure that we will be bashing the right places (immediates should be hardcoded to 0x0f0f0f0f0f0f0f0f0).
            _ASSERTE_ALL_BUILDS(0xf0f0f0f0f0f0f0f0 == *(UINT64*)m_pRegionToGenTableImmediate);
            _ASSERTE_ALL_BUILDS(0xf0f0f0f0f0f0f0f0 == *(UINT64*)m_pLowerBoundImmediate);
            _ASSERTE_ALL_BUILDS(0xf0f0f0f0f0f0f0f0 == *(UINT64*)m_pUpperBoundImmediate);
            _ASSERTE_ALL_BUILDS(0xf0f0f0f0f0f0f0f0 == *(UINT64*)m_pCardTableImmediate);
            _ASSERTE_ALL_BUILDS(              0x16 == *(UINT8 *)m_pRegionShrDest);
            _ASSERTE_ALL_BUILDS(              0x16 == *(UINT8 *)m_pRegionShrSrc);

#ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
            m_pCardBundleTableImmediate = CALC_PATCH_LOCATION(JIT_WriteBarrier_Byte_Region64, Patch_Label_CardBundleTable, 2);
            _ASSERTE_ALL_BUILDS(0xf0f0f0f0f0f0f0f0 == *(UINT64*)m_pCardBundleTableImmediate);
#endif
            break;

        case WRITE_BARRIER_BIT_REGIONS64:
            m_pRegionToGenTableImmediate = CALC_PATCH_LOCATION(JIT_WriteBarrier_Bit_Region64, Patch_Label_RegionToGeneration, 2);
            m_pRegionShrDest             = CALC_PATCH_LOCATION(JIT_WriteBarrier_Bit_Region64, Patch_Label_RegionShrDest, 3);
            m_pRegionShrSrc              = CALC_PATCH_LOCATION(JIT_WriteBarrier_Bit_Region64, Patch_Label_RegionShrSrc, 3);
            m_pLowerBoundImmediate       = CALC_PATCH_LOCATION(JIT_WriteBarrier_Bit_Region64, Patch_Label_Lower, 2);
            m_pUpperBoundImmediate       = CALC_PATCH_LOCATION(JIT_WriteBarrier_Bit_Region64, Patch_Label_Upper, 2);
            m_pCardTableImmediate        = CALC_PATCH_LOCATION(JIT_WriteBarrier_Bit_Region64, Patch_Label_CardTable, 2);

            // Make sure that we will be bashing the right places (immediates should be hardcoded to 0x0f0f0f0f0f0f0f0f0).
            _ASSERTE_ALL_BUILDS(0xf0f0f0f0f0f0f0f0 == *(UINT64*)m_pRegionToGenTableImmediate);
            _ASSERTE_ALL_BUILDS(0xf0f0f0f0f0f0f0f0 == *(UINT64*)m_pLowerBoundImmediate);
            _ASSERTE_ALL_BUILDS(0xf0f0f0f0f0f0f0f0 == *(UINT64*)m_pUpperBoundImmediate);
            _ASSERTE_ALL_BUILDS(0xf0f0f0f0f0f0f0f0 == *(UINT64*)m_pCardTableImmediate);
            _ASSERTE_ALL_BUILDS(              0x16 == *(UINT8 *)m_pRegionShrDest);
            _ASSERTE_ALL_BUILDS(              0x16 == *(UINT8 *)m_pRegionShrSrc);

#ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
            m_pCardBundleTableImmediate = CALC_PATCH_LOCATION(JIT_WriteBarrier_Bit_Region64, Patch_Label_CardBundleTable, 2);
            _ASSERTE_ALL_BUILDS(0xf0f0f0f0f0f0f0f0 == *(UINT64*)m_pCardBundleTableImmediate);
#endif
            break;

#ifdef FEATURE_USE_SOFTWARE_WRITE_WATCH_FOR_GC_HEAP
        case WRITE_BARRIER_WRITE_WATCH_PREGROW64:
        {
            m_pWriteWatchTableImmediate = CALC_PATCH_LOCATION(JIT_WriteBarrier_WriteWatch_PreGrow64, Patch_Label_WriteWatchTable, 2);
            m_pLowerBoundImmediate = CALC_PATCH_LOCATION(JIT_WriteBarrier_WriteWatch_PreGrow64, Patch_Label_Lower, 2);
            m_pCardTableImmediate = CALC_PATCH_LOCATION(JIT_WriteBarrier_WriteWatch_PreGrow64, Patch_Label_CardTable, 2);

            // Make sure that we will be bashing the right places (immediates should be hardcoded to 0x0f0f0f0f0f0f0f0f0).
            _ASSERTE_ALL_BUILDS(0xf0f0f0f0f0f0f0f0 == *(UINT64*)m_pWriteWatchTableImmediate);
            _ASSERTE_ALL_BUILDS(0xf0f0f0f0f0f0f0f0 == *(UINT64*)m_pLowerBoundImmediate);
            _ASSERTE_ALL_BUILDS(0xf0f0f0f0f0f0f0f0 == *(UINT64*)m_pCardTableImmediate);

#ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
            m_pCardBundleTableImmediate = CALC_PATCH_LOCATION(JIT_WriteBarrier_WriteWatch_PreGrow64, Patch_Label_CardBundleTable, 2);
            _ASSERTE_ALL_BUILDS(0xf0f0f0f0f0f0f0f0 == *(UINT64*)m_pCardBundleTableImmediate);
#endif
            break;
        }

        case WRITE_BARRIER_WRITE_WATCH_POSTGROW64:
        {
            m_pWriteWatchTableImmediate = CALC_PATCH_LOCATION(JIT_WriteBarrier_WriteWatch_PostGrow64, Patch_Label_WriteWatchTable, 2);
            m_pLowerBoundImmediate = CALC_PATCH_LOCATION(JIT_WriteBarrier_WriteWatch_PostGrow64, Patch_Label_Lower, 2);
            m_pUpperBoundImmediate = CALC_PATCH_LOCATION(JIT_WriteBarrier_WriteWatch_PostGrow64, Patch_Label_Upper, 2);
            m_pCardTableImmediate = CALC_PATCH_LOCATION(JIT_WriteBarrier_WriteWatch_PostGrow64, Patch_Label_CardTable, 2);

            // Make sure that we will be bashing the right places (immediates should be hardcoded to 0x0f0f0f0f0f0f0f0f0).
            _ASSERTE_ALL_BUILDS(0xf0f0f0f0f0f0f0f0 == *(UINT64*)m_pWriteWatchTableImmediate);
            _ASSERTE_ALL_BUILDS(0xf0f0f0f0f0f0f0f0 == *(UINT64*)m_pLowerBoundImmediate);
            _ASSERTE_ALL_BUILDS(0xf0f0f0f0f0f0f0f0 == *(UINT64*)m_pCardTableImmediate);
            _ASSERTE_ALL_BUILDS(0xf0f0f0f0f0f0f0f0 == *(UINT64*)m_pUpperBoundImmediate);

#ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
            m_pCardBundleTableImmediate = CALC_PATCH_LOCATION(JIT_WriteBarrier_WriteWatch_PostGrow64, Patch_Label_CardBundleTable, 2);
            _ASSERTE_ALL_BUILDS(0xf0f0f0f0f0f0f0f0 == *(UINT64*)m_pCardBundleTableImmediate);
#endif
            break;
        }

#ifdef FEATURE_SVR_GC
        case WRITE_BARRIER_WRITE_WATCH_SVR64:
        {
            m_pWriteWatchTableImmediate = CALC_PATCH_LOCATION(JIT_WriteBarrier_WriteWatch_SVR64, PatchLabel_WriteWatchTable, 2);
            m_pCardTableImmediate = CALC_PATCH_LOCATION(JIT_WriteBarrier_WriteWatch_SVR64, PatchLabel_CardTable, 2);

            // Make sure that we will be bashing the right places (immediates should be hardcoded to 0x0f0f0f0f0f0f0f0f0).
            _ASSERTE_ALL_BUILDS(0xf0f0f0f0f0f0f0f0 == *(UINT64*)m_pWriteWatchTableImmediate);
            _ASSERTE_ALL_BUILDS(0xf0f0f0f0f0f0f0f0 == *(UINT64*)m_pCardTableImmediate);

#ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
            m_pCardBundleTableImmediate = CALC_PATCH_LOCATION(JIT_WriteBarrier_WriteWatch_SVR64, PatchLabel_CardBundleTable, 2);
            _ASSERTE_ALL_BUILDS(0xf0f0f0f0f0f0f0f0 == *(UINT64*)m_pCardBundleTableImmediate);
#endif
            break;
        }
#endif // FEATURE_SVR_GC

        case WRITE_BARRIER_WRITE_WATCH_BYTE_REGIONS64:
            m_pWriteWatchTableImmediate  = CALC_PATCH_LOCATION(JIT_WriteBarrier_WriteWatch_Byte_Region64, Patch_Label_WriteWatchTable, 2);
            m_pRegionToGenTableImmediate = CALC_PATCH_LOCATION(JIT_WriteBarrier_WriteWatch_Byte_Region64, Patch_Label_RegionToGeneration, 2);
            m_pRegionShrDest             = CALC_PATCH_LOCATION(JIT_WriteBarrier_WriteWatch_Byte_Region64, Patch_Label_RegionShrDest, 3);
            m_pRegionShrSrc              = CALC_PATCH_LOCATION(JIT_WriteBarrier_WriteWatch_Byte_Region64, Patch_Label_RegionShrSrc, 3);
            m_pLowerBoundImmediate       = CALC_PATCH_LOCATION(JIT_WriteBarrier_WriteWatch_Byte_Region64, Patch_Label_Lower, 2);
            m_pUpperBoundImmediate       = CALC_PATCH_LOCATION(JIT_WriteBarrier_WriteWatch_Byte_Region64, Patch_Label_Upper, 2);
            m_pCardTableImmediate        = CALC_PATCH_LOCATION(JIT_WriteBarrier_WriteWatch_Byte_Region64, Patch_Label_CardTable, 2);

            // Make sure that we will be bashing the right places (immediates should be hardcoded to 0x0f0f0f0f0f0f0f0f0).
            _ASSERTE_ALL_BUILDS(0xf0f0f0f0f0f0f0f0 == *(UINT64*)m_pWriteWatchTableImmediate);
            _ASSERTE_ALL_BUILDS(0xf0f0f0f0f0f0f0f0 == *(UINT64*)m_pRegionToGenTableImmediate);
            _ASSERTE_ALL_BUILDS(0xf0f0f0f0f0f0f0f0 == *(UINT64*)m_pLowerBoundImmediate);
            _ASSERTE_ALL_BUILDS(0xf0f0f0f0f0f0f0f0 == *(UINT64*)m_pUpperBoundImmediate);
            _ASSERTE_ALL_BUILDS(0xf0f0f0f0f0f0f0f0 == *(UINT64*)m_pCardTableImmediate);
            _ASSERTE_ALL_BUILDS(              0x16 == *(UINT8 *)m_pRegionShrDest);
            _ASSERTE_ALL_BUILDS(              0x16 == *(UINT8 *)m_pRegionShrSrc);

#ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
            m_pCardBundleTableImmediate = CALC_PATCH_LOCATION(JIT_WriteBarrier_WriteWatch_Byte_Region64, Patch_Label_CardBundleTable, 2);
            _ASSERTE_ALL_BUILDS(0xf0f0f0f0f0f0f0f0 == *(UINT64*)m_pCardBundleTableImmediate);
#endif
            break;

        case WRITE_BARRIER_WRITE_WATCH_BIT_REGIONS64:
            m_pWriteWatchTableImmediate  = CALC_PATCH_LOCATION(JIT_WriteBarrier_WriteWatch_Bit_Region64, Patch_Label_WriteWatchTable, 2);
            m_pRegionToGenTableImmediate = CALC_PATCH_LOCATION(JIT_WriteBarrier_WriteWatch_Bit_Region64, Patch_Label_RegionToGeneration, 2);
            m_pRegionShrDest             = CALC_PATCH_LOCATION(JIT_WriteBarrier_WriteWatch_Bit_Region64, Patch_Label_RegionShrDest, 3);
            m_pRegionShrSrc              = CALC_PATCH_LOCATION(JIT_WriteBarrier_WriteWatch_Bit_Region64, Patch_Label_RegionShrSrc, 3);
            m_pLowerBoundImmediate       = CALC_PATCH_LOCATION(JIT_WriteBarrier_WriteWatch_Bit_Region64, Patch_Label_Lower, 2);
            m_pUpperBoundImmediate       = CALC_PATCH_LOCATION(JIT_WriteBarrier_WriteWatch_Bit_Region64, Patch_Label_Upper, 2);
            m_pCardTableImmediate        = CALC_PATCH_LOCATION(JIT_WriteBarrier_WriteWatch_Bit_Region64, Patch_Label_CardTable, 2);

            // Make sure that we will be bashing the right places (immediates should be hardcoded to 0x0f0f0f0f0f0f0f0f0).
            _ASSERTE_ALL_BUILDS(0xf0f0f0f0f0f0f0f0 == *(UINT64*)m_pWriteWatchTableImmediate);
            _ASSERTE_ALL_BUILDS(0xf0f0f0f0f0f0f0f0 == *(UINT64*)m_pRegionToGenTableImmediate);
            _ASSERTE_ALL_BUILDS(0xf0f0f0f0f0f0f0f0 == *(UINT64*)m_pLowerBoundImmediate);
            _ASSERTE_ALL_BUILDS(0xf0f0f0f0f0f0f0f0 == *(UINT64*)m_pUpperBoundImmediate);
            _ASSERTE_ALL_BUILDS(0xf0f0f0f0f0f0f0f0 == *(UINT64*)m_pCardTableImmediate);
            _ASSERTE_ALL_BUILDS(              0x16 == *(UINT8 *)m_pRegionShrDest);
            _ASSERTE_ALL_BUILDS(              0x16 == *(UINT8 *)m_pRegionShrSrc);

#ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
            m_pCardBundleTableImmediate = CALC_PATCH_LOCATION(JIT_WriteBarrier_WriteWatch_Bit_Region64, Patch_Label_CardBundleTable, 2);
            _ASSERTE_ALL_BUILDS(0xf0f0f0f0f0f0f0f0 == *(UINT64*)m_pCardBundleTableImmediate);
#endif
            break;


#endif // FEATURE_USE_SOFTWARE_WRITE_WATCH_FOR_GC_HEAP

        default:
            UNREACHABLE_MSG("unexpected write barrier type!");
    }

    stompWBCompleteActions |= UpdateEphemeralBounds(true);
    stompWBCompleteActions |= UpdateWriteWatchAndCardTableLocations(true, false);

    return stompWBCompleteActions;
}

#undef CALC_PATCH_LOCATION

void WriteBarrierManager::Initialize()
{
    CONTRACTL
    {
        MODE_ANY;
        GC_NOTRIGGER;
        NOTHROW;
    }
    CONTRACTL_END;


    // Ensure that the generic JIT_WriteBarrier function buffer is large enough to hold any of the more specific
    // write barrier implementations.
    size_t cbWriteBarrierBuffer = GetSpecificWriteBarrierSize(WRITE_BARRIER_BUFFER);

    //TODO: Satori
#ifdef FEATURE_SATORI_GC
    _ASSERTE_ALL_BUILDS(cbWriteBarrierBuffer >= GetSpecificWriteBarrierSize(WRITE_BARRIER_SATORI));

#else

    _ASSERTE_ALL_BUILDS(cbWriteBarrierBuffer >= GetSpecificWriteBarrierSize(WRITE_BARRIER_PREGROW64));
    _ASSERTE_ALL_BUILDS(cbWriteBarrierBuffer >= GetSpecificWriteBarrierSize(WRITE_BARRIER_POSTGROW64));
#ifdef FEATURE_SVR_GC
    _ASSERTE_ALL_BUILDS(cbWriteBarrierBuffer >= GetSpecificWriteBarrierSize(WRITE_BARRIER_SVR64));
#endif // FEATURE_SVR_GC
#ifdef FEATURE_SATORI_GC
    _ASSERTE_ALL_BUILDS(cbWriteBarrierBuffer >= GetSpecificWriteBarrierSize(WRITE_BARRIER_SATORI));
#endif // FEATURE_SATORI_GC
    _ASSERTE_ALL_BUILDS(cbWriteBarrierBuffer >= GetSpecificWriteBarrierSize(WRITE_BARRIER_BYTE_REGIONS64));
    _ASSERTE_ALL_BUILDS(cbWriteBarrierBuffer >= GetSpecificWriteBarrierSize(WRITE_BARRIER_BIT_REGIONS64));
#ifdef FEATURE_USE_SOFTWARE_WRITE_WATCH_FOR_GC_HEAP
    _ASSERTE_ALL_BUILDS(cbWriteBarrierBuffer >= GetSpecificWriteBarrierSize(WRITE_BARRIER_WRITE_WATCH_PREGROW64));
    _ASSERTE_ALL_BUILDS(cbWriteBarrierBuffer >= GetSpecificWriteBarrierSize(WRITE_BARRIER_WRITE_WATCH_POSTGROW64));
#ifdef FEATURE_SVR_GC
    _ASSERTE_ALL_BUILDS(cbWriteBarrierBuffer >= GetSpecificWriteBarrierSize(WRITE_BARRIER_WRITE_WATCH_SVR64));
#endif // FEATURE_SVR_GC
    _ASSERTE_ALL_BUILDS(cbWriteBarrierBuffer >= GetSpecificWriteBarrierSize(WRITE_BARRIER_WRITE_WATCH_BYTE_REGIONS64));
    _ASSERTE_ALL_BUILDS(cbWriteBarrierBuffer >= GetSpecificWriteBarrierSize(WRITE_BARRIER_WRITE_WATCH_BIT_REGIONS64));
#endif // FEATURE_USE_SOFTWARE_WRITE_WATCH_FOR_GC_HEAP

#endif //FEATURE_SATORI_GC

#if !defined(CODECOVERAGE)
    Validate();
#endif
}

bool WriteBarrierManager::NeedDifferentWriteBarrier(bool bReqUpperBoundsCheck, bool bUseBitwiseWriteBarrier, WriteBarrierType* pNewWriteBarrierType)
{
    // Init code for the JIT_WriteBarrier assembly routine.  Since it will be bashed everytime the GC Heap
    // changes size, we want to do most of the work just once.
    //
    // The actual JIT_WriteBarrier routine will only be called in free builds, but we keep this code (that
    // modifies it) around in debug builds to check that it works (with assertions).


    WriteBarrierType writeBarrierType = m_currentWriteBarrier;

    for(;;)
    {
        switch (writeBarrierType)
        {
        case WRITE_BARRIER_UNINITIALIZED:
#ifdef FEATURE_SATORI_GC
            writeBarrierType = WRITE_BARRIER_SATORI;
#else
#ifdef _DEBUG
            // The default slow write barrier has some good asserts
            if ((g_pConfig->GetHeapVerifyLevel() & EEConfig::HEAPVERIFY_BARRIERCHECK)) {
                break;
            }
#endif

            if (g_region_shr != 0)
            {
                writeBarrierType = bUseBitwiseWriteBarrier ? WRITE_BARRIER_BIT_REGIONS64: WRITE_BARRIER_BYTE_REGIONS64;
            }
            else
            {
                writeBarrierType = GCHeapUtilities::IsServerHeap() ? WRITE_BARRIER_SVR64 : WRITE_BARRIER_PREGROW64;
            }
#endif // FEATURE_SATORI_GC
            continue;

        case WRITE_BARRIER_PREGROW64:
            if (bReqUpperBoundsCheck)
            {
                writeBarrierType = WRITE_BARRIER_POSTGROW64;
            }
            break;

        case WRITE_BARRIER_POSTGROW64:
            break;

#ifdef FEATURE_SVR_GC
        case WRITE_BARRIER_SVR64:
            break;
#endif // FEATURE_SVR_GC

        case WRITE_BARRIER_BYTE_REGIONS64:
        case WRITE_BARRIER_BIT_REGIONS64:
            break;

#ifdef FEATURE_SATORI_GC
        case WRITE_BARRIER_SATORI:
            break;
#endif // FEATURE_SATORI_GC

#ifdef FEATURE_USE_SOFTWARE_WRITE_WATCH_FOR_GC_HEAP
        case WRITE_BARRIER_WRITE_WATCH_PREGROW64:
            if (bReqUpperBoundsCheck)
            {
                writeBarrierType = WRITE_BARRIER_WRITE_WATCH_POSTGROW64;
            }
            break;

        case WRITE_BARRIER_WRITE_WATCH_POSTGROW64:
            break;

#ifdef FEATURE_SVR_GC
        case WRITE_BARRIER_WRITE_WATCH_SVR64:
            break;
#endif // FEATURE_SVR_GC
        case WRITE_BARRIER_WRITE_WATCH_BYTE_REGIONS64:
        case WRITE_BARRIER_WRITE_WATCH_BIT_REGIONS64:
            break;
#endif // FEATURE_USE_SOFTWARE_WRITE_WATCH_FOR_GC_HEAP

        default:
            UNREACHABLE_MSG("unexpected write barrier type!");
        }
        break;
    }

    *pNewWriteBarrierType = writeBarrierType;
    return m_currentWriteBarrier != writeBarrierType;
}

int WriteBarrierManager::UpdateEphemeralBounds(bool isRuntimeSuspended)
{
    WriteBarrierType newType;
    if (NeedDifferentWriteBarrier(false, g_region_use_bitwise_write_barrier, &newType))
    {
        return ChangeWriteBarrierTo(newType, isRuntimeSuspended);
    }

    int stompWBCompleteActions = SWB_PASS;

#ifdef _DEBUG
    // Using debug-only write barrier?
    if (m_currentWriteBarrier == WRITE_BARRIER_UNINITIALIZED)
        return stompWBCompleteActions;
#endif

    switch (m_currentWriteBarrier)
    {
        case WRITE_BARRIER_POSTGROW64:
        case WRITE_BARRIER_BYTE_REGIONS64:
        case WRITE_BARRIER_BIT_REGIONS64:
#ifdef FEATURE_USE_SOFTWARE_WRITE_WATCH_FOR_GC_HEAP
        case WRITE_BARRIER_WRITE_WATCH_POSTGROW64:
        case WRITE_BARRIER_WRITE_WATCH_BYTE_REGIONS64:
        case WRITE_BARRIER_WRITE_WATCH_BIT_REGIONS64:
#endif // FEATURE_USE_SOFTWARE_WRITE_WATCH_FOR_GC_HEAP
        {
            // Change immediate if different from new g_ephermeral_high.
            if (*(UINT64*)m_pUpperBoundImmediate != (size_t)g_ephemeral_high)
            {
                ExecutableWriterHolder<UINT64> upperBoundWriterHolder((UINT64*)m_pUpperBoundImmediate, sizeof(UINT64));
                *upperBoundWriterHolder.GetRW() = (size_t)g_ephemeral_high;
                stompWBCompleteActions |= SWB_ICACHE_FLUSH;
            }
        }
        FALLTHROUGH;
        case WRITE_BARRIER_PREGROW64:
#ifdef FEATURE_USE_SOFTWARE_WRITE_WATCH_FOR_GC_HEAP
        case WRITE_BARRIER_WRITE_WATCH_PREGROW64:
#endif // FEATURE_USE_SOFTWARE_WRITE_WATCH_FOR_GC_HEAP
        {
            // Change immediate if different from new g_ephermeral_low.
            if (*(UINT64*)m_pLowerBoundImmediate != (size_t)g_ephemeral_low)
            {
                ExecutableWriterHolder<UINT64> lowerBoundImmediateWriterHolder((UINT64*)m_pLowerBoundImmediate, sizeof(UINT64));
                *lowerBoundImmediateWriterHolder.GetRW() = (size_t)g_ephemeral_low;
                stompWBCompleteActions |= SWB_ICACHE_FLUSH;
            }
            break;
        }

#ifdef FEATURE_SVR_GC
        case WRITE_BARRIER_SVR64:
#ifdef FEATURE_USE_SOFTWARE_WRITE_WATCH_FOR_GC_HEAP
        case WRITE_BARRIER_WRITE_WATCH_SVR64:
#endif // FEATURE_USE_SOFTWARE_WRITE_WATCH_FOR_GC_HEAP
        {
            break;
        }
#endif // FEATURE_SVR_GC

#ifdef FEATURE_SATORI_GC
        case WRITE_BARRIER_SATORI:
        {
            break;
        }
#endif
        default:
            UNREACHABLE_MSG("unexpected m_currentWriteBarrier in UpdateEphemeralBounds");
    }

    return stompWBCompleteActions;
}

int WriteBarrierManager::UpdateWriteWatchAndCardTableLocations(bool isRuntimeSuspended, bool bReqUpperBoundsCheck)
{
    // If we are told that we require an upper bounds check (GC did some heap reshuffling),
    // we need to switch to the WriteBarrier_PostGrow function for good.

#ifdef FEATURE_SATORI_GC
    // as of now satori does not patch barriers on x64, no need to go further.
    return SWB_PASS;
#endif

    WriteBarrierType newType;
    if (NeedDifferentWriteBarrier(bReqUpperBoundsCheck, g_region_use_bitwise_write_barrier, &newType))
    {
        return ChangeWriteBarrierTo(newType, isRuntimeSuspended);
    }

    int stompWBCompleteActions = SWB_PASS;

#ifdef _DEBUG
    // Using debug-only write barrier?
    if (m_currentWriteBarrier == WRITE_BARRIER_UNINITIALIZED)
        return stompWBCompleteActions;
#endif

#ifdef FEATURE_USE_SOFTWARE_WRITE_WATCH_FOR_GC_HEAP
    switch (m_currentWriteBarrier)
    {
        case WRITE_BARRIER_WRITE_WATCH_PREGROW64:
        case WRITE_BARRIER_WRITE_WATCH_POSTGROW64:
#ifdef FEATURE_SVR_GC
        case WRITE_BARRIER_WRITE_WATCH_SVR64:
#endif // FEATURE_SVR_GC
        case WRITE_BARRIER_WRITE_WATCH_BYTE_REGIONS64:
        case WRITE_BARRIER_WRITE_WATCH_BIT_REGIONS64:
            if (*(UINT64*)m_pWriteWatchTableImmediate != (size_t)g_sw_ww_table)
            {
                ExecutableWriterHolder<UINT64> writeWatchTableImmediateWriterHolder((UINT64*)m_pWriteWatchTableImmediate, sizeof(UINT64));
                *writeWatchTableImmediateWriterHolder.GetRW() = (size_t)g_sw_ww_table;
                stompWBCompleteActions |= SWB_ICACHE_FLUSH;
            }
            break;

        default:
            break; // clang seems to require all enum values to be covered for some reason
    }
#endif // FEATURE_USE_SOFTWARE_WRITE_WATCH_FOR_GC_HEAP

    switch (m_currentWriteBarrier)
    {
        case WRITE_BARRIER_BYTE_REGIONS64:
        case WRITE_BARRIER_BIT_REGIONS64:
        case WRITE_BARRIER_WRITE_WATCH_BYTE_REGIONS64:
        case WRITE_BARRIER_WRITE_WATCH_BIT_REGIONS64:
            if (*(UINT64*)m_pRegionToGenTableImmediate != (size_t)g_region_to_generation_table)
            {
                ExecutableWriterHolder<UINT64> writeWatchTableImmediateWriterHolder((UINT64*)m_pRegionToGenTableImmediate, sizeof(UINT64));
                *writeWatchTableImmediateWriterHolder.GetRW() = (size_t)g_region_to_generation_table;
                stompWBCompleteActions |= SWB_ICACHE_FLUSH;
            }
            if (*m_pRegionShrDest != g_region_shr)
            {
                ExecutableWriterHolder<UINT8> writeWatchTableImmediateWriterHolder(m_pRegionShrDest, sizeof(UINT8));
                *writeWatchTableImmediateWriterHolder.GetRW() = g_region_shr;
                stompWBCompleteActions |= SWB_ICACHE_FLUSH;
            }
            if (*m_pRegionShrSrc != g_region_shr)
            {
                ExecutableWriterHolder<UINT8> writeWatchTableImmediateWriterHolder(m_pRegionShrSrc, sizeof(UINT8));
                *writeWatchTableImmediateWriterHolder.GetRW() = g_region_shr;
                stompWBCompleteActions |= SWB_ICACHE_FLUSH;
            }
            break;

        default:
            break; // clang seems to require all enum values to be covered for some reason
    }

    if (*(UINT64*)m_pCardTableImmediate != (size_t)g_card_table)
    {
         ExecutableWriterHolder<UINT64> cardTableImmediateWriterHolder((UINT64*)m_pCardTableImmediate, sizeof(UINT64));
        *cardTableImmediateWriterHolder.GetRW() = (size_t)g_card_table;
        stompWBCompleteActions |= SWB_ICACHE_FLUSH;
    }

#ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
    if (*(UINT64*)m_pCardBundleTableImmediate != (size_t)g_card_bundle_table)
    {
         ExecutableWriterHolder<UINT64> cardBundleTableImmediateWriterHolder((UINT64*)m_pCardBundleTableImmediate, sizeof(UINT64));
        *cardBundleTableImmediateWriterHolder.GetRW() = (size_t)g_card_bundle_table;
        stompWBCompleteActions |= SWB_ICACHE_FLUSH;
    }
#endif

    return stompWBCompleteActions;
}

#ifdef FEATURE_USE_SOFTWARE_WRITE_WATCH_FOR_GC_HEAP
int WriteBarrierManager::SwitchToWriteWatchBarrier(bool isRuntimeSuspended)
{
#ifdef FEATURE_SATORI_GC
    // Noop for Satori
    return SWB_PASS;
#endif // FEATURE_SATORI_GC

    WriteBarrierType newWriteBarrierType;
    switch (m_currentWriteBarrier)
    {
        case WRITE_BARRIER_UNINITIALIZED:
            // Using the debug-only write barrier
            return SWB_PASS;

        case WRITE_BARRIER_PREGROW64:
            newWriteBarrierType = WRITE_BARRIER_WRITE_WATCH_PREGROW64;
            break;

        case WRITE_BARRIER_POSTGROW64:
            newWriteBarrierType = WRITE_BARRIER_WRITE_WATCH_POSTGROW64;
            break;

#ifdef FEATURE_SVR_GC
        case WRITE_BARRIER_SVR64:
            newWriteBarrierType = WRITE_BARRIER_WRITE_WATCH_SVR64;
            break;
#endif // FEATURE_SVR_GC

        case WRITE_BARRIER_BYTE_REGIONS64:
            newWriteBarrierType = WRITE_BARRIER_WRITE_WATCH_BYTE_REGIONS64;
            break;

        case WRITE_BARRIER_BIT_REGIONS64:
            newWriteBarrierType = WRITE_BARRIER_WRITE_WATCH_BIT_REGIONS64;
            break;

        default:
            UNREACHABLE();
    }

    return ChangeWriteBarrierTo(newWriteBarrierType, isRuntimeSuspended);
}

int WriteBarrierManager::SwitchToNonWriteWatchBarrier(bool isRuntimeSuspended)
{
#ifdef FEATURE_SATORI_GC
    // Noop for Satori
    return SWB_PASS;
#endif // FEATURE_SATORI_GC

    WriteBarrierType newWriteBarrierType;
    switch (m_currentWriteBarrier)
    {
        case WRITE_BARRIER_UNINITIALIZED:
            // Using the debug-only write barrier
            return SWB_PASS;

        case WRITE_BARRIER_WRITE_WATCH_PREGROW64:
            newWriteBarrierType = WRITE_BARRIER_PREGROW64;
            break;

        case WRITE_BARRIER_WRITE_WATCH_POSTGROW64:
            newWriteBarrierType = WRITE_BARRIER_POSTGROW64;
            break;

#ifdef FEATURE_SVR_GC
        case WRITE_BARRIER_WRITE_WATCH_SVR64:
            newWriteBarrierType = WRITE_BARRIER_SVR64;
            break;
#endif // FEATURE_SVR_GC

        case WRITE_BARRIER_WRITE_WATCH_BYTE_REGIONS64:
            newWriteBarrierType = WRITE_BARRIER_BYTE_REGIONS64;
            break;

        case WRITE_BARRIER_WRITE_WATCH_BIT_REGIONS64:
            newWriteBarrierType = WRITE_BARRIER_BIT_REGIONS64;
            break;

        default:
            UNREACHABLE();
    }

    return ChangeWriteBarrierTo(newWriteBarrierType, isRuntimeSuspended);
}
#endif // FEATURE_USE_SOFTWARE_WRITE_WATCH_FOR_GC_HEAP

// This function bashes the super fast amd64 version of the JIT_WriteBarrier
// helper.  It should be called by the GC whenever the ephermeral region
// bounds get changed, but still remain on the top of the GC Heap.
int StompWriteBarrierEphemeral(bool isRuntimeSuspended)
{
    WRAPPER_NO_CONTRACT;

    return g_WriteBarrierManager.UpdateEphemeralBounds(isRuntimeSuspended);
}

// This function bashes the super fast amd64 versions of the JIT_WriteBarrier
// helpers.  It should be called by the GC whenever the ephermeral region gets moved
// from being at the top of the GC Heap, and/or when the cards table gets moved.
int StompWriteBarrierResize(bool isRuntimeSuspended, bool bReqUpperBoundsCheck)
{
    WRAPPER_NO_CONTRACT;

    return g_WriteBarrierManager.UpdateWriteWatchAndCardTableLocations(isRuntimeSuspended, bReqUpperBoundsCheck);
}

void FlushWriteBarrierInstructionCache()
{
    FlushInstructionCache(GetCurrentProcess(), GetWriteBarrierCodeLocation((PVOID)JIT_WriteBarrier), g_WriteBarrierManager.GetCurrentWriteBarrierSize());
}

#ifdef FEATURE_USE_SOFTWARE_WRITE_WATCH_FOR_GC_HEAP
int SwitchToWriteWatchBarrier(bool isRuntimeSuspended)
{
    WRAPPER_NO_CONTRACT;

    return g_WriteBarrierManager.SwitchToWriteWatchBarrier(isRuntimeSuspended);
}

int SwitchToNonWriteWatchBarrier(bool isRuntimeSuspended)
{
    WRAPPER_NO_CONTRACT;

    return g_WriteBarrierManager.SwitchToNonWriteWatchBarrier(isRuntimeSuspended);
}
#endif // FEATURE_USE_SOFTWARE_WRITE_WATCH_FOR_GC_HEAP
