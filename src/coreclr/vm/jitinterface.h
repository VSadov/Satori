// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// ===========================================================================
// File: JITinterface.H
//

// ===========================================================================


#ifndef JITINTERFACE_H
#define JITINTERFACE_H

#include "corjit.h"

#ifndef TARGET_UNIX
#define MAX_UNCHECKED_OFFSET_FOR_NULL_OBJECT ((32*1024)-1)   // when generating JIT code
#else // !TARGET_UNIX
#define MAX_UNCHECKED_OFFSET_FOR_NULL_OBJECT ((GetOsPageSize() / 2) - 1)
#endif // !TARGET_UNIX
#include "pgo.h"

enum StompWriteBarrierCompletionAction
{
    SWB_PASS = 0x0,
    SWB_ICACHE_FLUSH = 0x1,
    SWB_EE_RESTART = 0x2
};

enum SignatureKind
{
    SK_NOT_CALLSITE,
    SK_CALLSITE,
    SK_VIRTUAL_CALLSITE,
    SK_STATIC_VIRTUAL_CODEPOINTER_CALLSITE,
};

class Stub;
class MethodDesc;
class NativeCodeVersion;
class FieldDesc;
enum RuntimeExceptionKind;
class AwareLock;
class PtrArray;
#if defined(FEATURE_GDBJIT)
class CalledMethod;
#endif

#include "genericdict.h"

inline FieldDesc* GetField(CORINFO_FIELD_HANDLE fieldHandle)
{
    LIMITED_METHOD_CONTRACT;
    return (FieldDesc*) fieldHandle;
}

inline
bool SigInfoFlagsAreValid (CORINFO_SIG_INFO *sig)
{
    LIMITED_METHOD_CONTRACT;
    return !(sig->flags & ~(  CORINFO_SIGFLAG_IS_LOCAL_SIG
                            | CORINFO_SIGFLAG_IL_STUB
                            ));
}


void InitJITHelpers1();
void InitJITHelpers2();

PCODE UnsafeJitFunction(PrepareCodeConfig* config,
                        COR_ILMETHOD_DECODER* header,
                        CORJIT_FLAGS* pJitFlags,
                        ULONG* pSizeOfCode);

void getMethodInfoILMethodHeaderHelper(
    COR_ILMETHOD_DECODER* header,
    CORINFO_METHOD_INFO* methInfo
    );


BOOL LoadDynamicInfoEntry(Module *currentModule,
                          RVA fixupRva,
                          SIZE_T *entry,
                          BOOL mayUsePrecompiledNDirectMethods = TRUE);

//
// The legacy x86 monitor helpers do not need a state argument
//
#if !defined(TARGET_X86)

#define FCDECL_MONHELPER(funcname, arg) FCDECL2(void, funcname, arg, BYTE* pbLockTaken)
#define HCIMPL_MONHELPER(funcname, arg) HCIMPL2(void, funcname, arg, BYTE* pbLockTaken)
#define MONHELPER_STATE(x) x
#define MONHELPER_ARG pbLockTaken

#else

#define FCDECL_MONHELPER(funcname, arg) FCDECL1(void, funcname, arg)
#define HCIMPL_MONHELPER(funcname, arg) HCIMPL1(void, funcname, arg)
#define MONHELPER_STATE(x)
#define MONHELPER_ARG NULL

#endif // TARGET_X86

//
// JIT HELPER ALIASING FOR PORTABILITY.
//
// The portable helper is used if the platform does not provide optimized implementation.
//

#ifndef JIT_MonEnter
#define JIT_MonEnter JIT_MonEnter_Portable
#endif
EXTERN_C FCDECL1(void, JIT_MonEnter, Object *obj);
EXTERN_C FCDECL1(void, JIT_MonEnter_Portable, Object *obj);

#ifndef JIT_MonEnterWorker
#define JIT_MonEnterWorker JIT_MonEnterWorker_Portable
#endif
EXTERN_C FCDECL_MONHELPER(JIT_MonEnterWorker, Object *obj);
EXTERN_C FCDECL_MONHELPER(JIT_MonEnterWorker_Portable, Object *obj);

#ifndef JIT_MonReliableEnter
#define JIT_MonReliableEnter JIT_MonReliableEnter_Portable
#endif
EXTERN_C FCDECL2(void, JIT_MonReliableEnter, Object* obj, BYTE *tookLock);
EXTERN_C FCDECL2(void, JIT_MonReliableEnter_Portable, Object* obj, BYTE *tookLock);

#ifndef JIT_MonTryEnter
#define JIT_MonTryEnter JIT_MonTryEnter_Portable
#endif
EXTERN_C FCDECL3(void, JIT_MonTryEnter, Object *obj, INT32 timeout, BYTE* pbLockTaken);
EXTERN_C FCDECL3(void, JIT_MonTryEnter_Portable, Object *obj, INT32 timeout, BYTE* pbLockTaken);

#ifndef JIT_MonExit
#define JIT_MonExit JIT_MonExit_Portable
#endif
EXTERN_C FCDECL1(void, JIT_MonExit, Object *obj);
EXTERN_C FCDECL1(void, JIT_MonExit_Portable, Object *obj);

#ifndef JIT_MonExitWorker
#define JIT_MonExitWorker JIT_MonExitWorker_Portable
#endif
EXTERN_C FCDECL_MONHELPER(JIT_MonExitWorker, Object *obj);
EXTERN_C FCDECL_MONHELPER(JIT_MonExitWorker_Portable, Object *obj);

#ifndef JIT_MonEnterStatic
#define JIT_MonEnterStatic JIT_MonEnterStatic_Portable
#endif
EXTERN_C FCDECL_MONHELPER(JIT_MonEnterStatic, AwareLock *lock);
EXTERN_C FCDECL_MONHELPER(JIT_MonEnterStatic_Portable, AwareLock *lock);

#ifndef JIT_MonExitStatic
#define JIT_MonExitStatic JIT_MonExitStatic_Portable
#endif
EXTERN_C FCDECL_MONHELPER(JIT_MonExitStatic, AwareLock *lock);
EXTERN_C FCDECL_MONHELPER(JIT_MonExitStatic_Portable, AwareLock *lock);

#ifndef JIT_GetGCStaticBase
#define JIT_GetGCStaticBase JIT_GetGCStaticBase_Portable
#endif
EXTERN_C FCDECL1(void*, JIT_GetGCStaticBase, MethodTable *pMT);
EXTERN_C FCDECL1(void*, JIT_GetGCStaticBase_Portable, MethodTable *pMT);

#ifndef JIT_GetNonGCStaticBase
#define JIT_GetNonGCStaticBase JIT_GetNonGCStaticBase_Portable
#endif
EXTERN_C FCDECL1(void*, JIT_GetNonGCStaticBase, MethodTable *pMT);
EXTERN_C FCDECL1(void*, JIT_GetNonGCStaticBase_Portable, MethodTable *pMT);

#ifndef JIT_GetGCStaticBaseNoCtor
#define JIT_GetGCStaticBaseNoCtor JIT_GetGCStaticBaseNoCtor_Portable
#endif
EXTERN_C FCDECL1(void*, JIT_GetGCStaticBaseNoCtor, MethodTable *pMT);
EXTERN_C FCDECL1(void*, JIT_GetGCStaticBaseNoCtor_Portable, MethodTable *pMT);

#ifndef JIT_GetNonGCStaticBaseNoCtor
#define JIT_GetNonGCStaticBaseNoCtor JIT_GetNonGCStaticBaseNoCtor_Portable
#endif
EXTERN_C FCDECL1(void*, JIT_GetNonGCStaticBaseNoCtor, MethodTable *pMT);
EXTERN_C FCDECL1(void*, JIT_GetNonGCStaticBaseNoCtor_Portable, MethodTable *pMT);

#ifndef JIT_GetDynamicGCStaticBase
#define JIT_GetDynamicGCStaticBase JIT_GetDynamicGCStaticBase_Portable
#endif
EXTERN_C FCDECL1(void*, JIT_GetDynamicGCStaticBase, DynamicStaticsInfo* pStaticsInfo);
EXTERN_C FCDECL1(void*, JIT_GetDynamicGCStaticBase_Portable, DynamicStaticsInfo* pStaticsInfo);

#ifndef JIT_GetDynamicNonGCStaticBase
#define JIT_GetDynamicNonGCStaticBase JIT_GetDynamicNonGCStaticBase_Portable
#endif
EXTERN_C FCDECL1(void*, JIT_GetDynamicNonGCStaticBase, DynamicStaticsInfo* pStaticsInfo);
EXTERN_C FCDECL1(void*, JIT_GetDynamicNonGCStaticBase_Portable, DynamicStaticsInfo* pStaticsInfo);

#ifndef JIT_GetDynamicGCStaticBaseNoCtor
#define JIT_GetDynamicGCStaticBaseNoCtor JIT_GetDynamicGCStaticBaseNoCtor_Portable
#endif
EXTERN_C FCDECL1(void*, JIT_GetDynamicGCStaticBaseNoCtor, DynamicStaticsInfo* pStaticsInfo);
EXTERN_C FCDECL1(void*, JIT_GetDynamicGCStaticBaseNoCtor_Portable, DynamicStaticsInfo* pStaticsInfo);

#ifndef JIT_GetDynamicNonGCStaticBaseNoCtor
#define JIT_GetDynamicNonGCStaticBaseNoCtor JIT_GetDynamicNonGCStaticBaseNoCtor_Portable
#endif
EXTERN_C FCDECL1(void*, JIT_GetDynamicNonGCStaticBaseNoCtor, DynamicStaticsInfo* pStaticsInfo);
EXTERN_C FCDECL1(void*, JIT_GetDynamicNonGCStaticBaseNoCtor_Portable, DynamicStaticsInfo* pStaticsInfo);

extern FCDECL1(Object*, JIT_NewS_MP_FastPortable, CORINFO_CLASS_HANDLE typeHnd_);
extern FCDECL1(Object*, JIT_New, CORINFO_CLASS_HANDLE typeHnd_);

extern FCDECL1(StringObject*, AllocateString_MP_FastPortable, DWORD stringLength);
extern FCDECL1(StringObject*, FramedAllocateString, DWORD stringLength);

extern FCDECL2(Object*, JIT_NewArr1VC_MP_FastPortable, CORINFO_CLASS_HANDLE arrayMT, INT_PTR size);
extern FCDECL2(Object*, JIT_NewArr1OBJ_MP_FastPortable, CORINFO_CLASS_HANDLE arrayMT, INT_PTR size);
extern FCDECL2(Object*, JIT_NewArr1, CORINFO_CLASS_HANDLE arrayMT, INT_PTR size);

EXTERN_C FCDECL_MONHELPER(JITutil_MonEnterWorker, Object* obj);
EXTERN_C FCDECL2(void, JITutil_MonReliableEnter, Object* obj, BYTE* pbLockTaken);
EXTERN_C FCDECL3(void, JITutil_MonTryEnter, Object* obj, INT32 timeOut, BYTE* pbLockTaken);
EXTERN_C FCDECL_MONHELPER(JITutil_MonExitWorker, Object* obj);
EXTERN_C FCDECL_MONHELPER(JITutil_MonSignal, AwareLock* lock);
EXTERN_C FCDECL_MONHELPER(JITutil_MonContention, AwareLock* awarelock);
EXTERN_C FCDECL2(void, JITutil_MonReliableContention, AwareLock* awarelock, BYTE* pbLockTaken);

EXTERN_C FCDECL1(void*, JIT_GetNonGCStaticBase_Helper, MethodTable *pMT);
EXTERN_C FCDECL1(void*, JIT_GetGCStaticBase_Helper, MethodTable *pMT);

EXTERN_C void DoJITFailFast ();
EXTERN_C FCDECL0(void, JIT_FailFast);

FCDECL0(int, JIT_GetCurrentManagedThreadId);

#if !defined(FEATURE_USE_ASM_GC_WRITE_BARRIERS) && defined(FEATURE_COUNT_GC_WRITE_BARRIERS)
// Extra argument for the classification of the checked barriers.
extern "C" FCDECL3(VOID, JIT_CheckedWriteBarrier, Object **dst, Object *ref, CheckedWriteBarrierKinds kind);
#else
// Regular checked write barrier.
extern "C" FCDECL2(VOID, JIT_CheckedWriteBarrier, Object **dst, Object *ref);
#endif

extern "C" FCDECL2(VOID, JIT_WriteBarrier, Object **dst, Object *ref);
extern "C" FCDECL2(VOID, JIT_WriteBarrierEnsureNonHeapTarget, Object **dst, Object *ref);

extern "C" FCDECL2(Object*, ChkCastAny_NoCacheLookup, CORINFO_CLASS_HANDLE type, Object* obj);
extern "C" FCDECL2(Object*, IsInstanceOfAny_NoCacheLookup, CORINFO_CLASS_HANDLE type, Object* obj);
extern "C" FCDECL2(LPVOID, Unbox_Helper, CORINFO_CLASS_HANDLE type, Object* obj);
extern "C" FCDECL2(void, JIT_Unbox_TypeTest, CORINFO_CLASS_HANDLE type, CORINFO_CLASS_HANDLE boxType);
extern "C" FCDECL3(void, JIT_Unbox_Nullable, void * destPtr, CORINFO_CLASS_HANDLE type, Object* obj);

// ARM64 JIT_WriteBarrier uses speciall ABI and thus is not callable directly
// Copied write barriers must be called at a different location
extern "C" FCDECL2(VOID, JIT_WriteBarrier_Callable, Object **dst, Object *ref);
#define WriteBarrier_Helper JIT_WriteBarrier_Callable

extern "C" FCDECL1(void, JIT_InternalThrow, unsigned exceptNum);
extern "C" FCDECL1(void*, JIT_InternalThrowFromHelper, unsigned exceptNum);

#ifdef TARGET_AMD64


class WriteBarrierManager
{
public:
    enum WriteBarrierType
    {
        WRITE_BARRIER_UNINITIALIZED,
        WRITE_BARRIER_PREGROW64,
        WRITE_BARRIER_POSTGROW64,
#ifdef FEATURE_SVR_GC
        WRITE_BARRIER_SVR64,
#endif // FEATURE_SVR_GC
        WRITE_BARRIER_BYTE_REGIONS64,
        WRITE_BARRIER_BIT_REGIONS64,
#ifdef FEATURE_SATORI_GC
        WRITE_BARRIER_SATORI,
#endif // FEATURE_SATORI_GC
#ifdef FEATURE_USE_SOFTWARE_WRITE_WATCH_FOR_GC_HEAP
        WRITE_BARRIER_WRITE_WATCH_PREGROW64,
        WRITE_BARRIER_WRITE_WATCH_POSTGROW64,
#ifdef FEATURE_SVR_GC
        WRITE_BARRIER_WRITE_WATCH_SVR64,
#endif // FEATURE_SVR_GC
        WRITE_BARRIER_WRITE_WATCH_BYTE_REGIONS64,
        WRITE_BARRIER_WRITE_WATCH_BIT_REGIONS64,
#endif // FEATURE_USE_SOFTWARE_WRITE_WATCH_FOR_GC_HEAP
        WRITE_BARRIER_BUFFER
    };

    WriteBarrierManager();
    void Initialize();

    int UpdateEphemeralBounds(bool isRuntimeSuspended);
    int UpdateWriteWatchAndCardTableLocations(bool isRuntimeSuspended, bool bReqUpperBoundsCheck);

#ifdef FEATURE_USE_SOFTWARE_WRITE_WATCH_FOR_GC_HEAP
    int SwitchToWriteWatchBarrier(bool isRuntimeSuspended);
    int SwitchToNonWriteWatchBarrier(bool isRuntimeSuspended);
#endif // FEATURE_USE_SOFTWARE_WRITE_WATCH_FOR_GC_HEAP
    size_t GetCurrentWriteBarrierSize();

protected:
    size_t GetSpecificWriteBarrierSize(WriteBarrierType writeBarrier);
    PBYTE  CalculatePatchLocation(LPVOID base, LPVOID label, int offset);
    PCODE  GetCurrentWriteBarrierCode();
    int ChangeWriteBarrierTo(WriteBarrierType newWriteBarrier, bool isRuntimeSuspended);
    bool   NeedDifferentWriteBarrier(bool bReqUpperBoundsCheck, bool bUseBitwiseWriteBarrier, WriteBarrierType* pNewWriteBarrierType);

private:
    void Validate();

    WriteBarrierType    m_currentWriteBarrier;

    PBYTE   m_pWriteWatchTableImmediate;    // PREGROW | POSTGROW | SVR | WRITE_WATCH | REGION
    PBYTE   m_pLowerBoundImmediate;         // PREGROW | POSTGROW |     | WRITE_WATCH | REGION
    PBYTE   m_pCardTableImmediate;          // PREGROW | POSTGROW | SVR | WRITE_WATCH | REGION
    PBYTE   m_pCardBundleTableImmediate;    // PREGROW | POSTGROW | SVR | WRITE_WATCH | REGION
    PBYTE   m_pUpperBoundImmediate;         //         | POSTGROW |     | WRITE_WATCH | REGION
    PBYTE   m_pRegionToGenTableImmediate;   //         |          |     | WRITE_WATCH | REGION
    PBYTE   m_pRegionShrDest;               //         |          |     | WRITE_WATCH | REGION
    PBYTE   m_pRegionShrSrc;                //         |          |     | WRITE_WATCH | RETION
};

#endif // TARGET_AMD64

EXTERN_C FCDECL2_VV(INT64, JIT_LMul, INT64 val1, INT64 val2);

#ifndef HOST_64BIT
#ifdef TARGET_X86
// JIThelp.asm
EXTERN_C void STDCALL JIT_LLsh();
EXTERN_C void STDCALL JIT_LRsh();
EXTERN_C void STDCALL JIT_LRsz();
#else // TARGET_X86
EXTERN_C FCDECL2_VV(UINT64, JIT_LLsh, UINT64 num, int shift);
EXTERN_C FCDECL2_VV(INT64, JIT_LRsh, INT64 num, int shift);
EXTERN_C FCDECL2_VV(UINT64, JIT_LRsz, UINT64 num, int shift);
#endif // !TARGET_X86
#endif // !HOST_64BIT

#ifdef TARGET_X86

#define ENUM_X86_WRITE_BARRIER_REGISTERS() \
    X86_WRITE_BARRIER_REGISTER(EAX) \
    X86_WRITE_BARRIER_REGISTER(ECX) \
    X86_WRITE_BARRIER_REGISTER(EBX) \
    X86_WRITE_BARRIER_REGISTER(ESI) \
    X86_WRITE_BARRIER_REGISTER(EDI) \
    X86_WRITE_BARRIER_REGISTER(EBP)

extern "C"
{

// JIThelp.asm/JIThelp.s
#define X86_WRITE_BARRIER_REGISTER(reg) \
    void STDCALL JIT_CheckedWriteBarrier##reg(); \
    void STDCALL JIT_DebugWriteBarrier##reg(); \
    void STDCALL JIT_WriteBarrier##reg();

    ENUM_X86_WRITE_BARRIER_REGISTERS()
#undef X86_WRITE_BARRIER_REGISTER

    void STDCALL JIT_WriteBarrierGroup();
    void STDCALL JIT_WriteBarrierGroup_End();

    void STDCALL JIT_PatchedWriteBarrierGroup();
    void STDCALL JIT_PatchedWriteBarrierGroup_End();
}

void ValidateWriteBarrierHelpers();

#endif //TARGET_X86

extern "C"
{
#ifndef FEATURE_EH_FUNCLETS
    void STDCALL JIT_EndCatch();               // JIThelp.asm/JIThelp.s
#endif // TARGET_X86

    void STDCALL JIT_ByRefWriteBarrier();      // JIThelp.asm/JIThelp.s

#if defined(TARGET_AMD64) || defined(TARGET_ARM)

    FCDECL2VA(void, JIT_TailCall, PCODE copyArgs, PCODE target);

#else // TARGET_AMD64 || TARGET_ARM

    void STDCALL JIT_TailCall();                    // JIThelp.asm

#endif // TARGET_AMD64 || TARGET_ARM

    void STDMETHODCALLTYPE JIT_ProfilerEnterLeaveTailcallStub(UINT_PTR ProfilerHandle);
#if !defined(TARGET_ARM64) && !defined(TARGET_LOONGARCH64) && !defined(TARGET_RISCV64)
    void STDCALL JIT_StackProbe();
#endif // TARGET_ARM64
};

/*********************************************************************/
/*********************************************************************/

// Transient data for a MethodDesc involved
// in the current JIT compilation.
struct TransientMethodDetails final
{
    MethodDesc* Method;
    COR_ILMETHOD_DECODER* Header;
    CORINFO_MODULE_HANDLE Scope;

    TransientMethodDetails() = default;
    TransientMethodDetails(MethodDesc* pMD, _In_opt_ COR_ILMETHOD_DECODER* header, CORINFO_MODULE_HANDLE scope);
    TransientMethodDetails(const TransientMethodDetails&) = delete;
    TransientMethodDetails(TransientMethodDetails&&);
    ~TransientMethodDetails();

    TransientMethodDetails& operator=(const TransientMethodDetails&) = delete;
    TransientMethodDetails& operator=(TransientMethodDetails&&);
};

class CEEInfo : public ICorJitInfo
{
    friend class CEEDynamicCodeInfo;

    void GetTypeContext(const CORINFO_SIG_INST* info, SigTypeContext* pTypeContext);
    MethodDesc* GetMethodFromContext(CORINFO_CONTEXT_HANDLE context);
    TypeHandle GetTypeFromContext(CORINFO_CONTEXT_HANDLE context);
    void GetTypeContext(CORINFO_CONTEXT_HANDLE context, SigTypeContext* pTypeContext);

    void HandleException(struct _EXCEPTION_POINTERS* pExceptionPointers);
public:
#include "icorjitinfoimpl_generated.h"
    uint32_t getClassAttribsInternal (CORINFO_CLASS_HANDLE cls);
    bool isObjectImmutableInteral(OBJECTREF obj);

    static unsigned getClassAlignmentRequirementStatic(TypeHandle clsHnd);

    static unsigned getClassGClayoutStatic(TypeHandle th, BYTE* gcPtrs);
    static CorInfoHelpFunc getNewHelperStatic(MethodTable * pMT, bool * pHasSideEffects);
    static CorInfoHelpFunc getNewArrHelperStatic(TypeHandle clsHnd);
    static CorInfoHelpFunc getCastingHelperStatic(TypeHandle clsHnd, bool fThrowing);

    // Returns that compilation flags that are shared between JIT and NGen
    static CORJIT_FLAGS GetBaseCompileFlags(MethodDesc * ftn);

    static CorInfoHelpFunc getSharedStaticsHelper(FieldDesc * pField, MethodTable * pFieldMT);

    static size_t findNameOfToken (Module* module, mdToken metaTOK,
                            _Out_writes_ (FQNameCapacity) char * szFQName, size_t FQNameCapacity);

    DWORD getMethodAttribsInternal (CORINFO_METHOD_HANDLE ftnHnd);

    bool resolveVirtualMethodHelper(CORINFO_DEVIRTUALIZATION_INFO * info);

    CORINFO_CLASS_HANDLE getDefaultComparerClassHelper(
        CORINFO_CLASS_HANDLE elemType
        );

    CORINFO_CLASS_HANDLE getDefaultEqualityComparerClassHelper(
        CORINFO_CLASS_HANDLE elemType
        );

    CorInfoType getFieldTypeInternal (CORINFO_FIELD_HANDLE field, CORINFO_CLASS_HANDLE* structType = NULL,CORINFO_CLASS_HANDLE owner = NULL);

protected:
    void freeArrayInternal(void* array);

public:

    bool getTailCallHelpersInternal(
        CORINFO_RESOLVED_TOKEN* callToken,
        CORINFO_SIG_INFO* sig,
        CORINFO_GET_TAILCALL_HELPERS_FLAGS flags,
        CORINFO_TAILCALL_HELPERS* pResult);

    bool getStaticObjRefContent(OBJECTREF obj, uint8_t* buffer, bool ignoreMovableObjects);

    // This normalizes EE type information into the form expected by the JIT.
    //
    // If typeHnd contains exact type information, then *clsRet will contain
    // the normalized CORINFO_CLASS_HANDLE information on return.
    static CorInfoType asCorInfoType (CorElementType cet,
                                      TypeHandle typeHnd = TypeHandle() /* optional in */,
                                      CORINFO_CLASS_HANDLE *clsRet = NULL /* optional out */ );

    CEEInfo(MethodDesc * fd = NULL, bool fAllowInlining = true) :
        m_pJitHandles(nullptr),
        m_pMethodBeingCompiled(fd),
        m_transientDetails(NULL),
        m_pThread(GetThreadNULLOk()),
        m_hMethodForSecurity_Key(NULL),
        m_pMethodForSecurity_Value(NULL),
#if defined(FEATURE_GDBJIT)
        m_pCalledMethods(NULL),
#endif
        m_allowInlining(fAllowInlining)
    {
        LIMITED_METHOD_CONTRACT;
    }

    virtual ~CEEInfo()
    {
        LIMITED_METHOD_CONTRACT;

#if !defined(DACCESS_COMPILE)
        // Free all handles used by JIT
        if (m_pJitHandles != nullptr)
        {
            OBJECTHANDLE* elements = m_pJitHandles->GetElements();
            unsigned count = m_pJitHandles->GetCount();
            for (unsigned i = 0; i < count; i++)
            {
                DestroyHandle(elements[i]);
            }
            delete m_pJitHandles;
        }

        delete m_transientDetails;
#endif
    }

    // Performs any work JIT-related work that should be performed at process shutdown.
    void JitProcessShutdownWork();

    void setJitFlags(const CORJIT_FLAGS& jitFlags);

private:
    // Shrinking these buffers drastically reduces the amount of stack space
    // required for each instance of the interpreter, and thereby reduces SOs.
#ifdef FEATURE_INTERPRETER
#define CLS_STRING_SIZE 8  // force heap allocation
#define CLS_BUFFER_SIZE SBUFFER_PADDED_SIZE(8)
#else
#define CLS_STRING_SIZE MAX_CLASSNAME_LENGTH
#define CLS_BUFFER_SIZE MAX_CLASSNAME_LENGTH
#endif

#ifdef _DEBUG
    InlineSString<MAX_CLASSNAME_LENGTH> ssClsNameBuff;
    InlineSString<MAX_CLASSNAME_LENGTH> ssClsNameBuffUTF8;
#endif

public:
    MethodDesc * GetMethodForSecurity(CORINFO_METHOD_HANDLE callerHandle);

    // Prepare the information about how to do a runtime lookup of the handle with shared
    // generic variables.
    void ComputeRuntimeLookupForSharedGenericToken(DictionaryEntryKind entryKind,
                                                   CORINFO_RESOLVED_TOKEN * pResolvedToken,
                                                   CORINFO_RESOLVED_TOKEN * pConstrainedResolvedToken /* for ConstrainedMethodEntrySlot */,
                                                   MethodDesc * pTemplateMD /* for method-based slots */,
                                                   MethodDesc * pCallerMD,
                                                   CORINFO_LOOKUP *pResultLookup);

#if defined(FEATURE_GDBJIT)
    CalledMethod * GetCalledMethods() { return m_pCalledMethods; }
#endif

    // Add/Remove/Find transient method details.
    void AddTransientMethodDetails(TransientMethodDetails details);
    TransientMethodDetails RemoveTransientMethodDetails(MethodDesc* pMD);
    bool FindTransientMethodDetails(MethodDesc* pMD, TransientMethodDetails** details);

protected:
    SArray<OBJECTHANDLE>*   m_pJitHandles;          // GC handles used by JIT
    MethodDesc*             m_pMethodBeingCompiled; // Top-level method being compiled
    SArray<TransientMethodDetails, FALSE>* m_transientDetails;   // Transient details for dynamic codegen scenarios.
    Thread *                m_pThread;              // Cached current thread for faster JIT-EE transitions
    CORJIT_FLAGS            m_jitFlags;

    CORINFO_METHOD_HANDLE getMethodBeingCompiled()
    {
        LIMITED_METHOD_CONTRACT;
        return (CORINFO_METHOD_HANDLE)m_pMethodBeingCompiled;
    }

    CORINFO_OBJECT_HANDLE getJitHandleForObject(OBJECTREF objref, bool knownFrozen = false);
    OBJECTREF getObjectFromJitHandle(CORINFO_OBJECT_HANDLE handle);

    // Cache of last GetMethodForSecurity() lookup
    CORINFO_METHOD_HANDLE   m_hMethodForSecurity_Key;
    MethodDesc *            m_pMethodForSecurity_Value;

#if defined(FEATURE_GDBJIT)
    CalledMethod *          m_pCalledMethods;
#endif

    bool                    m_allowInlining;

    void EnsureActive(TypeHandle th, MethodDesc * pMD = NULL);
};


/*********************************************************************/

class  EEJitManager;
struct  HeapList;
struct _hpCodeHdr;
typedef struct _hpCodeHdr CodeHeader;

// CEEJitInfo is the concrete implementation of callbacks that the EE must provide for the JIT to do its
// work.   See code:ICorJitInfo#JitToEEInterface for more on this interface.
class CEEJitInfo : public CEEInfo
{
public:
    // ICorJitInfo stuff

    void allocMem (AllocMemArgs *pArgs) override final;

    void reserveUnwindInfo(bool isFunclet, bool isColdCode, uint32_t unwindSize) override final;

    void allocUnwindInfo (
            uint8_t * pHotCode,              /* IN */
            uint8_t * pColdCode,             /* IN */
            uint32_t  startOffset,           /* IN */
            uint32_t  endOffset,             /* IN */
            uint32_t  unwindSize,            /* IN */
            uint8_t * pUnwindBlock,          /* IN */
            CorJitFuncKind funcKind       /* IN */
            ) override final;

    void * allocGCInfo (size_t  size) override final;

    void setEHcount (unsigned cEH) override final;

    void setEHinfo (
            unsigned      EHnumber,
            const CORINFO_EH_CLAUSE* clause) override final;

    void getEHinfo(
            CORINFO_METHOD_HANDLE ftn,              /* IN  */
            unsigned      EHnumber,                 /* IN */
            CORINFO_EH_CLAUSE* clause               /* OUT */
            ) override final;

    HRESULT allocPgoInstrumentationBySchema(
            CORINFO_METHOD_HANDLE ftnHnd, /* IN */
            PgoInstrumentationSchema* pSchema, /* IN/OUT */
            uint32_t countSchemaItems, /* IN */
            uint8_t** pInstrumentationData /* OUT */
            ) override final;

    HRESULT getPgoInstrumentationResults(
            CORINFO_METHOD_HANDLE ftnHnd, /* IN */
            PgoInstrumentationSchema** pSchema, /* OUT */
            uint32_t* pCountSchemaItems, /* OUT */
            uint8_t**pInstrumentationData, /* OUT */
            PgoSource *pPgoSource, /* OUT */
            bool* pDynamicPgo /* OUT */
            ) override final;

    void recordCallSite(
            uint32_t                     instrOffset,  /* IN */
            CORINFO_SIG_INFO *        callSig,      /* IN */
            CORINFO_METHOD_HANDLE     methodHandle  /* IN */
            ) override final;

    void recordRelocation(
            void                    *location,
            void                    *locationRW,
            void                    *target,
            uint16_t                 fRelocType,
            int32_t                  addlDelta) override final;

    uint16_t getRelocTypeHint(void * target) override final;

    uint32_t getExpectedTargetArchitecture() override final;

    void ResetForJitRetry()
    {
        CONTRACTL {
            NOTHROW;
            GC_NOTRIGGER;
        } CONTRACTL_END;

        if (m_CodeHeaderRW != m_CodeHeader)
            freeArrayInternal(m_CodeHeaderRW);

        m_CodeHeader = NULL;
        m_CodeHeaderRW = NULL;

        m_codeWriteBufferSize = 0;
        m_pRealCodeHeader = NULL;
        m_pCodeHeap = NULL;

        if (m_pOffsetMapping != NULL)
            freeArrayInternal(m_pOffsetMapping);

        if (m_pNativeVarInfo != NULL)
            freeArrayInternal(m_pNativeVarInfo);

        m_iOffsetMapping = 0;
        m_pOffsetMapping = NULL;
        m_iNativeVarInfo = 0;
        m_pNativeVarInfo = NULL;

        if (m_inlineTreeNodes != NULL)
            freeArrayInternal(m_inlineTreeNodes);
        if (m_richOffsetMappings != NULL)
            freeArrayInternal(m_richOffsetMappings);

        m_inlineTreeNodes = NULL;
        m_numInlineTreeNodes = 0;
        m_richOffsetMappings = NULL;
        m_numRichOffsetMappings = 0;

#ifdef FEATURE_ON_STACK_REPLACEMENT
        if (m_pPatchpointInfoFromJit != NULL)
            freeArrayInternal(m_pPatchpointInfoFromJit);

        m_pPatchpointInfoFromJit = NULL;
#endif

#ifdef FEATURE_EH_FUNCLETS
        m_moduleBase = (TADDR)0;
        m_totalUnwindSize = 0;
        m_usedUnwindSize = 0;
        m_theUnwindBlock = NULL;
        m_totalUnwindInfos = 0;
        m_usedUnwindInfos = 0;
#endif // FEATURE_EH_FUNCLETS
    }

#ifdef TARGET_AMD64
    void SetAllowRel32(BOOL fAllowRel32)
    {
        LIMITED_METHOD_CONTRACT;
        m_fAllowRel32 = fAllowRel32;
    }
#endif

#if defined(TARGET_AMD64) || defined(TARGET_ARM64)
    void SetJumpStubOverflow(BOOL fJumpStubOverflow)
    {
        LIMITED_METHOD_CONTRACT;
        m_fJumpStubOverflow = fJumpStubOverflow;
    }

    BOOL IsJumpStubOverflow()
    {
        LIMITED_METHOD_CONTRACT;
        return m_fJumpStubOverflow;
    }

    BOOL JitAgain()
    {
        LIMITED_METHOD_CONTRACT;
        return m_fJumpStubOverflow;
    }

    size_t GetReserveForJumpStubs()
    {
        LIMITED_METHOD_CONTRACT;
        return m_reserveForJumpStubs;
    }

    void SetReserveForJumpStubs(size_t value)
    {
        LIMITED_METHOD_CONTRACT;
        m_reserveForJumpStubs = value;
    }
#else
    BOOL JitAgain()
    {
        LIMITED_METHOD_CONTRACT;
        return FALSE;
    }

    size_t GetReserveForJumpStubs()
    {
        LIMITED_METHOD_CONTRACT;
        return 0;
    }
#endif // defined(TARGET_AMD64) || defined(TARGET_ARM64)

#ifdef FEATURE_ON_STACK_REPLACEMENT
    // Called by the runtime to supply patchpoint information to the jit.
    void SetOSRInfo(PatchpointInfo* patchpointInfo, unsigned ilOffset)
    {
        _ASSERTE(m_pPatchpointInfoFromRuntime == NULL);
        _ASSERTE(patchpointInfo != NULL);
        m_pPatchpointInfoFromRuntime = patchpointInfo;
        m_ilOffset = ilOffset;
    }
#endif

    CEEJitInfo(MethodDesc* fd, COR_ILMETHOD_DECODER* header,
               EEJitManager* jm, bool allowInlining = true)
        : CEEInfo(fd, allowInlining),
          m_jitManager(jm),
          m_CodeHeader(NULL),
          m_CodeHeaderRW(NULL),
          m_codeWriteBufferSize(0),
          m_pRealCodeHeader(NULL),
          m_pCodeHeap(NULL),
          m_ILHeader(header),
#ifdef FEATURE_EH_FUNCLETS
          m_moduleBase(0),
          m_totalUnwindSize(0),
          m_usedUnwindSize(0),
          m_theUnwindBlock(NULL),
          m_totalUnwindInfos(0),
          m_usedUnwindInfos(0),
#endif
#ifdef TARGET_AMD64
          m_fAllowRel32(FALSE),
#endif
#if defined(TARGET_AMD64) || defined(TARGET_ARM64)
          m_fJumpStubOverflow(FALSE),
          m_reserveForJumpStubs(0),
#endif
          m_GCinfo_len(0),
          m_EHinfo_len(0),
          m_iOffsetMapping(0),
          m_pOffsetMapping(NULL),
          m_iNativeVarInfo(0),
          m_pNativeVarInfo(NULL),
          m_inlineTreeNodes(NULL),
          m_numInlineTreeNodes(0),
          m_richOffsetMappings(NULL),
          m_numRichOffsetMappings(0),
#ifdef FEATURE_ON_STACK_REPLACEMENT
          m_pPatchpointInfoFromJit(NULL),
          m_pPatchpointInfoFromRuntime(NULL),
          m_ilOffset(0),
#endif
          m_gphCache()
    {
        CONTRACTL
        {
            NOTHROW;
            GC_NOTRIGGER;
            MODE_ANY;
        } CONTRACTL_END;
    }

    ~CEEJitInfo()
    {
        CONTRACTL
        {
            NOTHROW;
            GC_NOTRIGGER;
            MODE_ANY;
        } CONTRACTL_END;

        if (m_CodeHeaderRW != m_CodeHeader)
            freeArrayInternal(m_CodeHeaderRW);

        if (m_pOffsetMapping != NULL)
            freeArrayInternal(m_pOffsetMapping);

        if (m_pNativeVarInfo != NULL)
            freeArrayInternal(m_pNativeVarInfo);

#ifdef FEATURE_ON_STACK_REPLACEMENT
        if (m_pPatchpointInfoFromJit != NULL)
            freeArrayInternal(m_pPatchpointInfoFromJit);
#endif
#ifdef FEATURE_PGO
        if (m_foundPgoData != NULL)
        {
            ComputedPgoData* current = m_foundPgoData;
            while (current != NULL)
            {
                ComputedPgoData* next = current->m_next;
                delete current;
                current = next;
            }
        }
#endif
    }

    // ICorDebugInfo stuff.
    void setBoundaries(CORINFO_METHOD_HANDLE ftn,
                       ULONG32 cMap, ICorDebugInfo::OffsetMapping *pMap) override final;
    void setVars(CORINFO_METHOD_HANDLE ftn, ULONG32 cVars,
                 ICorDebugInfo::NativeVarInfo *vars) override final;
    void CompressDebugInfo();

    void reportRichMappings(
        ICorDebugInfo::InlineTreeNode*    inlineTreeNodes,
        uint32_t                          numInlineTreeNodes,
        ICorDebugInfo::RichOffsetMapping* mappings,
        uint32_t                          numMappings) override final;

    void reportMetadata(const char* key, const void* value, size_t length) override final;

    void* getHelperFtn(CorInfoHelpFunc    ftnNum,                         /* IN  */
                       void **            ppIndirection) override final;  /* OUT */
    static PCODE getHelperFtnStatic(CorInfoHelpFunc ftnNum);

    // Override of CEEInfo::GetProfilingHandle.  The first time this is called for a
    // method desc, it calls through to CEEInfo::GetProfilingHandle and caches the
    // result in CEEJitInfo::GetProfilingHandleCache.  Thereafter, this wrapper regurgitates the cached values
    // rather than calling into CEEInfo::GetProfilingHandle each time.  This avoids
    // making duplicate calls into the profiler's FunctionIDMapper callback.
    void GetProfilingHandle(
                    bool                      *pbHookFunction,
                    void                     **pProfilerHandle,
                    bool                      *pbIndirectedHandles
                    ) override final;

    InfoAccessType constructStringLiteral(CORINFO_MODULE_HANDLE scopeHnd, mdToken metaTok, void **ppValue) override final;
    InfoAccessType emptyStringLiteral(void ** ppValue) override final;
    CORINFO_CLASS_HANDLE getStaticFieldCurrentClass(CORINFO_FIELD_HANDLE field, bool* pIsSpeculative) override final;
    void* getMethodSync(CORINFO_METHOD_HANDLE ftnHnd, void **ppIndirection) override final;

    void BackoutJitData(EEJitManager * jitMgr);

    void WriteCode(EEJitManager * jitMgr);

    void setPatchpointInfo(PatchpointInfo* patchpointInfo) override final;
    PatchpointInfo* getOSRInfo(unsigned* ilOffset) override final;

protected :

    void WriteCodeBytes();

#ifdef FEATURE_PGO
    // PGO data
    struct ComputedPgoData
    {
        ComputedPgoData(MethodDesc* pMD) : m_pMD(pMD) {}

        ComputedPgoData* m_next = nullptr;
        MethodDesc *m_pMD;
        NewArrayHolder<BYTE> m_allocatedData;
        PgoInstrumentationSchema* m_schema = nullptr;
        UINT32 m_cSchemaElems;
        BYTE *m_pInstrumentationData = nullptr;
        HRESULT m_hr = E_NOTIMPL;
        PgoSource m_pgoSource = PgoSource::Unknown;
    };
    ComputedPgoData*        m_foundPgoData = nullptr;
#endif


    EEJitManager*           m_jitManager;   // responsible for allocating memory
    CodeHeader*             m_CodeHeader;   // descriptor for JITTED code - read/execute address
    CodeHeader*             m_CodeHeaderRW; // descriptor for JITTED code - code write scratch buffer address
    size_t                  m_codeWriteBufferSize;
    BYTE*                   m_pRealCodeHeader;
    HeapList*               m_pCodeHeap;
    COR_ILMETHOD_DECODER *  m_ILHeader;     // the code header as exist in the file
#ifdef FEATURE_EH_FUNCLETS
    TADDR                   m_moduleBase;       // Base for unwind Infos
    ULONG                   m_totalUnwindSize;  // Total reserved unwind space
    uint32_t                m_usedUnwindSize;   // used space in m_theUnwindBlock
    BYTE *                  m_theUnwindBlock;   // start of the unwind memory block
    ULONG                   m_totalUnwindInfos; // Number of RUNTIME_FUNCTION needed
    ULONG                   m_usedUnwindInfos;
#endif

#ifdef TARGET_AMD64
    BOOL                    m_fAllowRel32;      // Use 32-bit PC relative address modes
#endif
#if defined(TARGET_AMD64) || defined(TARGET_ARM64)
    BOOL                    m_fJumpStubOverflow;   // Overflow while trying to alocate jump stub slot within PC relative branch region
                                                   // The code will need to be regenerated (with m_fRel32Allowed == FALSE for AMD64).
    size_t                  m_reserveForJumpStubs; // Space to reserve for jump stubs when allocating code
#endif

#if defined(_DEBUG)
    ULONG                   m_codeSize;     // Code size requested via allocMem
#endif

    size_t                  m_GCinfo_len;   // Cached copy of GCinfo_len so we can backout in BackoutJitData()
    size_t                  m_EHinfo_len;   // Cached copy of EHinfo_len so we can backout in BackoutJitData()

    ULONG32                 m_iOffsetMapping;
    ICorDebugInfo::OffsetMapping * m_pOffsetMapping;

    ULONG32                 m_iNativeVarInfo;
    ICorDebugInfo::NativeVarInfo * m_pNativeVarInfo;

    ICorDebugInfo::InlineTreeNode    *m_inlineTreeNodes;
    ULONG32                           m_numInlineTreeNodes;
    ICorDebugInfo::RichOffsetMapping *m_richOffsetMappings;
    ULONG32                           m_numRichOffsetMappings;

#ifdef FEATURE_ON_STACK_REPLACEMENT
    PatchpointInfo        * m_pPatchpointInfoFromJit;
    PatchpointInfo        * m_pPatchpointInfoFromRuntime;
    unsigned                m_ilOffset;
#endif

    // The first time a call is made to CEEJitInfo::GetProfilingHandle() from this thread
    // for this method, these values are filled in.   Thereafter, these values are used
    // in lieu of calling into the base CEEInfo::GetProfilingHandle() again.  This protects the
    // profiler from duplicate calls to its FunctionIDMapper() callback.
    struct GetProfilingHandleCache
    {
        GetProfilingHandleCache() :
            m_bGphIsCacheValid(false),
            m_bGphHookFunction(false),
            m_pvGphProfilerHandle(NULL)
        {
            LIMITED_METHOD_CONTRACT;
        }

        bool                    m_bGphIsCacheValid : 1;        // Tells us whether below values are valid
        bool                    m_bGphHookFunction : 1;
        void*                   m_pvGphProfilerHandle;
    } m_gphCache;

};

/*********************************************************************/
/*********************************************************************/

typedef struct {
    void * pfnHelper;
#ifdef _DEBUG
    const char* name;
#endif
} VMHELPDEF;

#if defined(DACCESS_COMPILE)

GARY_DECL(VMHELPDEF, hlpFuncTable, CORINFO_HELP_COUNT);

#else

extern "C" const VMHELPDEF hlpFuncTable[CORINFO_HELP_COUNT];

#endif

#if defined(_DEBUG) && (defined(TARGET_AMD64) || defined(TARGET_X86)) && !defined(TARGET_UNIX)
typedef struct {
    void*       pfnRealHelper;
    const char* helperName;
    LONG        count;
    LONG        helperSize;
} VMHELPCOUNTDEF;

extern "C" VMHELPCOUNTDEF hlpFuncCountTable[CORINFO_HELP_COUNT+1];

void InitJitHelperLogging();
void WriteJitHelperCountToSTRESSLOG();
#else
inline void InitJitHelperLogging() { }
inline void WriteJitHelperCountToSTRESSLOG() { }
#endif

// enum for dynamically assigned helper calls
enum DynamicCorInfoHelpFunc {
#define JITHELPER(code, pfnHelper, binderId)
#define DYNAMICJITHELPER(code, pfnHelper, binderId) DYNAMIC_##code,
#include "jithelpers.h"
    DYNAMIC_CORINFO_HELP_COUNT
};

#ifdef _MSC_VER
// GCC complains about duplicate "extern". And it is not needed for the GCC build
extern "C"
#endif
GARY_DECL(VMHELPDEF, hlpDynamicFuncTable, DYNAMIC_CORINFO_HELP_COUNT);

#define SetJitHelperFunction(ftnNum, pFunc) _SetJitHelperFunction(DYNAMIC_##ftnNum, (void*)(pFunc))
void    _SetJitHelperFunction(DynamicCorInfoHelpFunc ftnNum, void * pFunc);

VMHELPDEF LoadDynamicJitHelper(DynamicCorInfoHelpFunc ftnNum, MethodDesc** methodDesc = NULL);

void *GenFastGetSharedStaticBase(bool bCheckCCtor);

#ifdef HAVE_GCCOVER
void SetupGcCoverage(NativeCodeVersion nativeCodeVersion, BYTE* nativeCode);
void SetupGcCoverageForNativeImage(Module* module);
BOOL OnGcCoverageInterrupt(PT_CONTEXT regs);
void DoGcStress (PT_CONTEXT regs, NativeCodeVersion nativeCodeVersion);
#endif //HAVE_GCCOVER

// ppPinnedString: If the string is pinned (e.g. allocated in frozen heap),
// the pointer to the pinned string is returned in *ppPinnedPointer. ppPinnedPointer == nullptr
// means that the caller does not care whether the string is pinned or not.
OBJECTHANDLE ConstructStringLiteral(CORINFO_MODULE_HANDLE scopeHnd, mdToken metaTok, void** ppPinnedString = nullptr);

FCDECL2(Object*, JIT_Box_MP_FastPortable, CORINFO_CLASS_HANDLE type, void* data);
FCDECL2(Object*, JIT_Box, CORINFO_CLASS_HANDLE type, void* data);
FCDECL0(VOID, JIT_PollGC);

BOOL ObjIsInstanceOf(Object *pObject, TypeHandle toTypeHnd, BOOL throwCastException = FALSE);
BOOL ObjIsInstanceOfCore(Object* pObject, TypeHandle toTypeHnd, BOOL throwCastException = FALSE);

EXTERN_C TypeHandle::CastResult STDCALL ObjIsInstanceOfCached(Object *pObject, TypeHandle toTypeHnd);

#ifdef HOST_64BIT
class InlinedCallFrame;
Thread * JIT_InitPInvokeFrame(InlinedCallFrame *pFrame);
#endif

#ifdef _DEBUG
extern LONG g_JitCount;
#endif

struct VirtualFunctionPointerArgs
{
    CORINFO_CLASS_HANDLE classHnd;
    CORINFO_METHOD_HANDLE methodHnd;
};

FCDECL2(CORINFO_MethodPtr, JIT_VirtualFunctionPointer_Dynamic, Object * objectUNSAFE, VirtualFunctionPointerArgs * pArgs);

typedef HCCALL1_PTR(TADDR, FnStaticBaseHelper, TADDR arg0);

struct StaticFieldAddressArgs
{
    FnStaticBaseHelper staticBaseHelper;
    TADDR arg0;
    SIZE_T offset;
};

FCDECL1(TADDR, JIT_StaticFieldAddress_Dynamic, StaticFieldAddressArgs * pArgs);
FCDECL1(TADDR, JIT_StaticFieldAddressUnbox_Dynamic, StaticFieldAddressArgs * pArgs);

struct GenericHandleArgs
{
    LPVOID signature;
    CORINFO_MODULE_HANDLE module;
    DWORD dictionaryIndexAndSlot;
};

FCDECL2(CORINFO_GENERIC_HANDLE, JIT_GenericHandleMethodWithSlotAndModule, CORINFO_METHOD_HANDLE  methodHnd, GenericHandleArgs * pArgs);
FCDECL2(CORINFO_GENERIC_HANDLE, JIT_GenericHandleClassWithSlotAndModule, CORINFO_CLASS_HANDLE classHnd, GenericHandleArgs * pArgs);

CORINFO_GENERIC_HANDLE JIT_GenericHandleWorker(MethodDesc   *pMD,
                                               MethodTable  *pMT,
                                               LPVOID        signature,
                                               DWORD         dictionaryIndexAndSlot = -1,
                                               Module *      pModule = NULL);

void ClearJitGenericHandleCache();

CORJIT_FLAGS GetDebuggerCompileFlags(Module* pModule, CORJIT_FLAGS flags);

bool __stdcall TrackAllocationsEnabled();


extern Volatile<int64_t> g_cbILJitted;
extern Volatile<int64_t> g_cMethodsJitted;
extern Volatile<int64_t> g_c100nsTicksInJit;
extern thread_local int64_t t_cbILJittedForThread;
extern thread_local int64_t t_cMethodsJittedForThread;
extern thread_local int64_t t_c100nsTicksInJitForThread;

FCDECL1(INT64, GetCompiledILBytes, FC_BOOL_ARG currentThread);
FCDECL1(INT64, GetCompiledMethodCount, FC_BOOL_ARG currentThread);
FCDECL1(INT64, GetCompilationTimeInTicks, FC_BOOL_ARG currentThread);

#endif // JITINTERFACE_H
