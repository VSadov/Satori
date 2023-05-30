;; Licensed to the .NET Foundation under one or more agreements.
;; The .NET Foundation licenses this file to you under the MIT license.

;;
;; Define the helpers used to implement the write barrier required when writing an object reference into a
;; location residing on the GC heap. Such write barriers allow the GC to optimize which objects in
;; non-ephemeral generations need to be scanned for references to ephemeral objects during an ephemeral
;; collection.
;;

#include "AsmMacros_Shared.h"

    TEXTAREA

#ifndef FEATURE_SATORI_GC

;; Macro used to copy contents of newly updated GC heap locations to a shadow copy of the heap. This is used
;; during garbage collections to verify that object references where never written to the heap without using a
;; write barrier. Note that we're potentially racing to update the shadow heap while other threads are writing
;; new references to the real heap. Since this can't be solved perfectly without critical sections around the
;; entire update process, we instead update the shadow location and then re-check the real location (as two
;; ordered operations) and if there is a disparity we'll re-write the shadow location with a special value
;; (INVALIDGCVALUE) which disables the check for that location. Since the shadow heap is only validated at GC
;; time and these write barrier operations are atomic wrt to GCs this is sufficient to guarantee that the
;; shadow heap contains only valid copies of real heap values or INVALIDGCVALUE.
#ifdef WRITE_BARRIER_CHECK

    SETALIAS    g_GCShadow, ?g_GCShadow@@3PEAEEA
    SETALIAS    g_GCShadowEnd, ?g_GCShadowEnd@@3PEAEEA
    EXTERN      $g_GCShadow
    EXTERN      $g_GCShadowEnd

INVALIDGCVALUE  EQU 0xCCCCCCCD

    MACRO
        ;; On entry:
        ;;  $destReg: location to be updated (cannot be x12,x17)
        ;;  $refReg: objectref to be stored (cannot be x12,x17)
        ;;
        ;; On exit:
        ;;  x12,x17: trashed
        ;;  other registers are preserved
        ;;
        UPDATE_GC_SHADOW $destReg, $refReg

        ;; If g_GCShadow is 0, don't perform the check.
        PREPARE_EXTERNAL_VAR_INDIRECT $g_GCShadow, x12
        cbz     x12, %ft1

        ;; Save $destReg since we're about to modify it (and we need the original value both within the macro and
        ;; once we exit the macro).
        mov     x17, $destReg

        ;; Transform $destReg into the equivalent address in the shadow heap.
        PREPARE_EXTERNAL_VAR_INDIRECT g_lowest_address, x12
        subs    $destReg, $destReg, x12
        blo     %ft0

        PREPARE_EXTERNAL_VAR_INDIRECT $g_GCShadow, x12
        add     $destReg, $destReg, x12

        PREPARE_EXTERNAL_VAR_INDIRECT $g_GCShadowEnd, x12
        cmp     $destReg, x12
        bhs     %ft0

        ;; Update the shadow heap.
        str     $refReg, [$destReg]

        ;; The following read must be strongly ordered wrt to the write we've just performed in order to
        ;; prevent race conditions.
        dmb     ish

        ;; Now check that the real heap location still contains the value we just wrote into the shadow heap.
        mov     x12, x17
        ldr     x12, [x12]
        cmp     x12, $refReg
        beq     %ft0

        ;; Someone went and updated the real heap. We need to invalidate the shadow location since we can't
        ;; guarantee whose shadow update won.
        MOVL64  x12, INVALIDGCVALUE, 0
        str     x12, [$destReg]

0
        ;; Restore original $destReg value
        mov     $destReg, x17

1
    MEND

#else // WRITE_BARRIER_CHECK

    MACRO
        UPDATE_GC_SHADOW $destReg, $refReg
    MEND

#endif // WRITE_BARRIER_CHECK

;; There are several different helpers used depending on which register holds the object reference. Since all
;; the helpers have identical structure we use a macro to define this structure. Two arguments are taken, the
;; name of the register that points to the location to be updated and the name of the register that holds the
;; object reference (this should be in upper case as it's used in the definition of the name of the helper).

;; Define a sub-macro first that expands to the majority of the barrier implementation. This is used below for
;; some interlocked helpers that need an inline barrier.
    MACRO
        ;; On entry:
        ;;   $destReg:  location to be updated (cannot be x12,x17)
        ;;   $refReg:   objectref to be stored (cannot be x12,x17)
        ;;
        ;; On exit:
        ;;   x12,x17: trashed
        ;;
        INSERT_UNCHECKED_WRITE_BARRIER_CORE $destReg, $refReg

        ;; Update the shadow copy of the heap with the same value just written to the same heap. (A no-op unless
        ;; we're in a debug build and write barrier checking has been enabled).
        UPDATE_GC_SHADOW $destReg, $refReg

#ifdef FEATURE_USE_SOFTWARE_WRITE_WATCH_FOR_GC_HEAP
        // Update the write watch table if necessary
        PREPARE_EXTERNAL_VAR_INDIRECT g_write_watch_table, x12

        cbz     x12, %ft2
        add     x12, x12, $destReg, lsr #0xc  // SoftwareWriteWatch::AddressToTableByteIndexShift
        ldrb    w17, [x12]
        cbnz    x17, %ft2
        mov     w17, #0xFF
        strb    w17, [x12]
#endif

2
        ;; We can skip the card table write if the reference is to
        ;; an object not on the epehemeral segment.
        PREPARE_EXTERNAL_VAR_INDIRECT g_ephemeral_low,  x12
        PREPARE_EXTERNAL_VAR_INDIRECT g_ephemeral_high, x17
        cmp     $refReg, x12
        ccmp    $refReg, x17, #0x2, hs
        bhs     %ft0

        ;; Set this object's card, if it hasn't already been set.
        PREPARE_EXTERNAL_VAR_INDIRECT g_card_table, x12
        add     x17, x12, $destReg lsr #11

        ;; Check that this card hasn't already been written. Avoiding useless writes is a big win on
        ;; multi-proc systems since it avoids cache trashing.
        ldrb    w12, [x17]
        cmp     x12, 0xFF
        beq     %ft0

        mov     x12, 0xFF
        strb    w12, [x17]

#ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
        // Check if we need to update the card bundle table
        PREPARE_EXTERNAL_VAR_INDIRECT g_card_bundle_table, x12
        add     x17, x12, $destReg, lsr #21
        ldrb    w12, [x17]
        cmp     x12, 0xFF
        beq     %ft0

        mov     x12, 0xFF
        strb    w12, [x17]
#endif

0
        ;; Exit label
    MEND

    MACRO
        ;; On entry:
        ;;   $destReg:  location to be updated (cannot be x12,x17)
        ;;   $refReg:   objectref to be stored (cannot be x12,x17)
        ;;
        ;; On exit:
        ;;   x12, x17:       trashed
        ;;
        INSERT_CHECKED_WRITE_BARRIER_CORE $destReg, $refReg

        ;; The "check" of this checked write barrier - is $destReg
        ;; within the heap? if no, early out.
        PREPARE_EXTERNAL_VAR_INDIRECT g_lowest_address,  x12
        PREPARE_EXTERNAL_VAR_INDIRECT g_highest_address, x17
        cmp     $destReg, x12
        ccmp    $destReg, x17, #0x2, hs
        bhs     %ft0

        INSERT_UNCHECKED_WRITE_BARRIER_CORE $destReg, $refReg

0
        ;; Exit label
    MEND

;; void JIT_ByRefWriteBarrier
;; On entry:
;;   x13 : the source address (points to object reference to write)
;;   x14 : the destination address (object reference written here)
;;
;; On exit:
;;   x13 : incremented by 8
;;   x14 : incremented by 8
;;   x15  : trashed
;;   x12, x17  : trashed
;;
;;   NOTE: Keep in sync with RBM_CALLEE_TRASH_WRITEBARRIER_BYREF and RBM_CALLEE_GCTRASH_WRITEBARRIER_BYREF
;;         if you add more trashed registers.
;;
;; WARNING: Code in EHHelpers.cpp makes assumptions about write barrier code, in particular:
;; - Function "InWriteBarrierHelper" assumes an AV due to passed in null pointer will happen at RhpByRefAssignRefAVLocation1
;; - Function "UnwindSimpleHelperToCaller" assumes no registers were pushed and LR contains the return address
    LEAF_ENTRY RhpByRefAssignRefArm64

    ALTERNATE_ENTRY RhpByRefAssignRefAVLocation1
    ALTERNATE_ENTRY RhpByRefAssignRefAVLocation2
        ldr     x15, [x13], 8
        b       RhpCheckedAssignRefArm64

    LEAF_END RhpByRefAssignRefArm64


;; JIT_CheckedWriteBarrier(Object** dst, Object* src)
;;
;; Write barrier for writes to objects that may reside
;; on the managed heap.
;;
;; On entry:
;;   x14  : the destination address (LHS of the assignment).
;;          May not be a heap location (hence the checked).
;;   x15  : the object reference (RHS of the assignment)
;;
;; On exit:
;;   x12, x17 : trashed
;;   x14      : incremented by 8
    LEAF_ENTRY RhpCheckedAssignRefArm64

        ;; is destReg within the heap?
        PREPARE_EXTERNAL_VAR_INDIRECT g_lowest_address,  x12
        PREPARE_EXTERNAL_VAR_INDIRECT g_highest_address, x17
        cmp     x14, x12
        ccmp    x14, x17, #0x2, hs
        blo     RhpAssignRefArm64

NotInHeap
    ALTERNATE_ENTRY RhpCheckedAssignRefAVLocation
        str     x15, [x14], 8
        ret

    LEAF_END RhpCheckedAssignRefArm64

;; JIT_WriteBarrier(Object** dst, Object* src)
;;
;; Write barrier for writes to objects that are known to
;; reside on the managed heap.
;;
;; On entry:
;;   x14  : the destination address (LHS of the assignment)
;;   x15  : the object reference (RHS of the assignment)
;;
;; On exit:
;;   x12, x17 : trashed
;;   x14 : incremented by 8
    LEAF_ENTRY RhpAssignRefArm64

    ALTERNATE_ENTRY RhpAssignRefAVLocation
        stlr    x15, [x14]

        INSERT_UNCHECKED_WRITE_BARRIER_CORE x14, x15

        add     x14, x14, 8
        ret

    LEAF_END RhpAssignRefArm64

;; same as RhpAssignRefArm64, but with standard ABI.
    LEAF_ENTRY RhpAssignRef
        mov     x14, x0             ; x14 = dst
        mov     x15, x1             ; x15 = val
        b       RhpAssignRefArm64
    LEAF_END RhpAssignRef

#ifdef FEATURE_NATIVEAOT

;; Interlocked operation helpers where the location is an objectref, thus requiring a GC write barrier upon
;; successful updates.

;; RhpCheckedLockCmpXchg(Object** dest, Object* value, Object* comparand)
;;
;; Interlocked compare exchange on objectref.
;;
;; On entry:
;;   x0  : pointer to objectref
;;   x1  : exchange value
;;   x2  : comparand
;;
;; On exit:
;;  x0: original value of objectref
;;  x10, x12, x16, x17: trashed
;;
    LEAF_ENTRY RhpCheckedLockCmpXchg

#ifndef LSE_INSTRUCTIONS_ENABLED_BY_DEFAULT
        PREPARE_EXTERNAL_VAR_INDIRECT_W g_cpuFeatures, 16
        tbz    x16, #ARM64_ATOMICS_FEATURE_FLAG_BIT, CmpXchgRetry
#endif

        mov    x10, x2
        casal  x10, x1, [x0]                  ;; exchange
        cmp    x2, x10
        bne    CmpXchgNoUpdate

#ifndef LSE_INSTRUCTIONS_ENABLED_BY_DEFAULT
        b      DoCardsCmpXchg
CmpXchgRetry
        ;; Check location value is what we expect.
        ldaxr   x10, [x0]
        cmp     x10, x2
        bne     CmpXchgNoUpdate

        ;; Current value matches comparand, attempt to update with the new value.
        stlxr   w12, x1, [x0]
        cbnz    w12, CmpXchgRetry
#endif

DoCardsCmpXchg
        ;; We have successfully updated the value of the objectref so now we need a GC write barrier.
        ;; The following barrier code takes the destination in x0 and the value in x1 so the arguments are
        ;; already correctly set up.

        INSERT_CHECKED_WRITE_BARRIER_CORE x0, x1

CmpXchgNoUpdate
        ;; x10 still contains the original value.
        mov     x0, x10

#ifndef LSE_INSTRUCTIONS_ENABLED_BY_DEFAULT
        tbnz    x16, #ARM64_ATOMICS_FEATURE_FLAG_BIT, NoBarrierCmpXchg
        InterlockedOperationBarrier
NoBarrierCmpXchg
#endif
        ret     lr

    LEAF_END RhpCheckedLockCmpXchg

;; RhpCheckedXchg(Object** destination, Object* value)
;;
;; Interlocked exchange on objectref.
;;
;; On entry:
;;   x0  : pointer to objectref
;;   x1  : exchange value
;;
;; On exit:
;;  x0: original value of objectref
;;  x10: trashed
;;  x12, x16, x17: trashed
;;
    LEAF_ENTRY RhpCheckedXchg

#ifndef LSE_INSTRUCTIONS_ENABLED_BY_DEFAULT
        PREPARE_EXTERNAL_VAR_INDIRECT_W g_cpuFeatures, 16
        tbz    x16, #ARM64_ATOMICS_FEATURE_FLAG_BIT, ExchangeRetry
#endif

        swpal  x1, x10, [x0]                   ;; exchange

#ifndef LSE_INSTRUCTIONS_ENABLED_BY_DEFAULT
        b      DoCardsXchg
ExchangeRetry
        ;; Read the existing memory location.
        ldaxr   x10,  [x0]

        ;; Attempt to update with the new value.
        stlxr   w12, x1, [x0]
        cbnz    w12, ExchangeRetry
#endif

DoCardsXchg
        ;; We have successfully updated the value of the objectref so now we need a GC write barrier.
        ;; The following barrier code takes the destination in x0 and the value in x1 so the arguments are
        ;; already correctly set up.

        INSERT_CHECKED_WRITE_BARRIER_CORE x0, x1

        ;; x10 still contains the original value.
        mov     x0, x10

#ifndef LSE_INSTRUCTIONS_ENABLED_BY_DEFAULT
        tbnz    x16, #ARM64_ATOMICS_FEATURE_FLAG_BIT, NoBarrierXchg
        InterlockedOperationBarrier
NoBarrierXchg
#endif
        ret

    LEAF_END RhpCheckedXchg
#endif // FEATURE_NATIVEAOT

#else  ;;FEATURE_SATORI_GC   ######################################################

;; void JIT_ByRefWriteBarrier
;; On entry:
;;   x13  : the source address (points to object reference to write)
;;   x14  : the destination address (object reference written here)
;;
;; On exit:
;;   x13  : incremented by 8
;;   x14  : incremented by 8
;;   x15  : trashed
;;   x12, x17  : trashed
;;
    LEAF_ENTRY RhpByRefAssignRefArm64, _TEXT

   RhpByRefAssignRefAVLocation1
        ldr     x15, [x13], 8
        b       RhpCheckedAssignRefArm64

    LEAF_END RhpByRefAssignRefArm64

;; JIT_CheckedWriteBarrier(Object** dst, Object* src)
;;
;; Write barrier for writes to objects that may reside
;; on the managed heap.
;;
;; On entry:
;;   x14 : the destination address (LHS of the assignment).
;;         May not be a heap location (hence the checked).
;;   x15 : the object reference (RHS of the assignment).
;;
;; On exit:
;;   x12, x17 : trashed
;;   x14      : incremented by 8
    LEAF_ENTRY RhpCheckedAssignRefArm64, _TEXT
        PREPARE_EXTERNAL_VAR_INDIRECT g_card_bundle_table, x12
        add     x12, x12, x14, lsr #30
        ldrb    w12, [x12]
        cbnz    x12, RhpAssignRefArm64

NotInHeap
    ALTERNATE_ENTRY RhpCheckedAssignRefAVLocation
        str  x15, [x14], #8
        ret  lr
    LEAF_END RhpCheckedAssignRefArm64

;; JIT_WriteBarrier(Object** dst, Object* src)
;;
;; Write barrier for writes to objects that are known to
;; reside on the managed heap.
;;
;; On entry:
;;  x14 : the destination address (LHS of the assignment).
;;  x15 : the object reference (RHS of the assignment).
;;
;; On exit:
;;  x12, x17 : trashed
;;  x14 : incremented by 8
    LEAF_ENTRY RhpAssignRefArm64, _TEXT
    ;; check for escaping assignment
    ;; 1) check if we own the source region
#ifdef FEATURE_SATORI_EXTERNAL_OBJECTS
        PREPARE_EXTERNAL_VAR_INDIRECT g_card_bundle_table, x12
        add     x12, x12, x15, lsr #30
        ldrb    w12, [x12]
        cbz     x12, JustAssign
#else
        cbz     x15, JustAssign    ;; assigning null
#endif
        and     x12,  x15, #0xFFFFFFFFFFE00000  ;; source region
        ldr     x12, [x12]                      ; region tag
        cmp     x12, x18                        ; x18 - TEB
        bne     AssignAndMarkCards              ; not local to this thread

    ;; 2) check if the src and dst are from the same region
        eor     x12, x14, x15
        lsr     x12, x12, #21
        cbnz    x12, RecordEscape  ;; cross region assignment. definitely escaping

    ;; 3) check if the target is exposed
        ubfx    x17, x14,#9,#12                 ;; word index = (dst >> 9) & 0x1FFFFF
        and     x12, x15, #0xFFFFFFFFFFE00000   ;; source region
        ldr     x17, [x12, x17, lsl #3]         ;; mark word = [region + index * 8]
        lsr     x12, x14, #3                    ;; bit = (dst >> 3) [& 63]
        lsr     x17, x17, x12
        tbnz    x17, #0, RecordEscape ;; target is exposed. record an escape.

        str     x15, [x14], #8                  ;; UNORDERED assignment of unescaped object
        ret     lr

JustAssign
    ALTERNATE_ENTRY RhpAssignRefAVLocationNotHeap
        stlr    x15, [x14]                      ;; no card marking, src is not a heap object
        add     x14, x14, 8
        ret     lr

AssignAndMarkCards
    ALTERNATE_ENTRY RhpAssignRefAVLocation
        stlr    x15, [x14]

    ;; need couple temps. Save before using.
        stp     x2,  x3,  [sp, -16]!

        eor     x12, x14, x15
        lsr     x12, x12, #21
        cbz     x12, CheckConcurrent ;; same region, just check if barrier is not concurrent

    ;; if src is in gen2/3 and the barrier is not concurrent we do not need to mark cards
        and     x2,  x15, #0xFFFFFFFFFFE00000     ;; source region
        ldr     w12, [x2, 16]
        tbz     x12, #1, MarkCards

CheckConcurrent
        PREPARE_EXTERNAL_VAR_INDIRECT g_write_watch_table, x12  ;; !g_write_watch_table -> !concurrent
        cbnz    x12, MarkCards
        
Exit
        ldp  x2,  x3, [sp], 16
        add  x14, x14, 8
        ret  lr

MarkCards
    ;; fetch card location for x14
        PREPARE_EXTERNAL_VAR_INDIRECT g_card_table, x12  ;; fetch the page map
        lsr     x17, x14, #30
        ldr     x17, [x12, x17, lsl #3]              ;; page
        sub     x2,  x14, x17   ;; offset in page
        lsr     x15, x2,  #21   ;; group index
        lsl     x15, x15, #1    ;; group offset (index * 2)
        lsr     x2,  x2,  #9    ;; card offset

    ;; check if concurrent marking is in progress
        PREPARE_EXTERNAL_VAR_INDIRECT g_write_watch_table, x12  ;; !g_write_watch_table -> !concurrent
        cbnz    x12, DirtyCard

    ;; SETTING CARD FOR X14
SetCard
        ldrb    w3, [x17, x2]
        cbnz    w3, CardSet
        mov     w3, #1
        strb    w3, [x17, x2]
SetGroup
        add     x12, x17, #0x80
        ldrb    w3, [x12, x15]
        cbnz    w3, CardSet
        mov     w3, #1
        strb    w3, [x12, x15]
SetPage
        ldrb    w3, [x17]
        cbnz    w3, CardSet
        mov     w3, #1
        strb    w3, [x17]

CardSet
    ;; check if concurrent marking is still not in progress
        PREPARE_EXTERNAL_VAR_INDIRECT g_write_watch_table, x12  ;; !g_write_watch_table -> !concurrent
        cbnz    x12, DirtyCard
        b       Exit

    ;; DIRTYING CARD FOR X14
DirtyCard
        mov     w3, #3
        strb    w3, [x17, x2]
DirtyGroup
        add     x12, x17, #0x80
        strb    w3, [x12, x15]
DirtyPage
        strb    w3, [x17]
        b       Exit

    ;; this is expected to be rare.
RecordEscape

    ;; 4) check if the source is escaped
        and         x12, x15, #0xFFFFFFFFFFE00000  ;; source region
        add         x15, x15, #8                   ;; escape bit is MT + 1
        ubfx        x17, x15, #9,#12               ;; word index = (dst >> 9) & 0x1FFFFF
        ldr         x17, [x12, x17, lsl #3]        ;; mark word = [region + index * 8]
        lsr         x12, x15, #3                   ;; bit = (dst >> 3) [& 63]
        sub         x15, x15, #8     ;; undo MT + 1
        lsr         x17, x17, x12
        tbnz        x17, #0, AssignAndMarkCards        ;; source is already escaped.

        ;; because of the barrier call convention
        ;; we need to preserve caller-saved x0 through x18 and x29/x30

        stp     x29,x30, [sp, -16 * 10]!
        stp     x0, x1,  [sp, 16 * 1]
        stp     x2, x3,  [sp, 16 * 2]
        stp     x4, x5,  [sp, 16 * 3]
        stp     x6, x7,  [sp, 16 * 4]
        stp     x8, x9,  [sp, 16 * 5]
        stp     x10,x11, [sp, 16 * 6]
        stp     x12,x13, [sp, 16 * 7]
        stp     x14,x15, [sp, 16 * 8]
        stp     x16,x17, [sp, 16 * 9]

        ;; void SatoriRegion::EscapeFn(SatoriObject** dst, SatoriObject* src, SatoriRegion* region)
        ;; mov  x0, x14  EscapeFn does not use dst, it is just to avoid arg shuffle on x64
        mov  x1, x15
        and  x2, x15, #0xFFFFFFFFFFE00000  ;; source region
        ldr  x12, [x2, #8]                 ;; EscapeFn address
        blr  x12

        ldp     x0, x1,  [sp, 16 * 1]
        ldp     x2, x3,  [sp, 16 * 2]
        ldp     x4, x5,  [sp, 16 * 3]
        ldp     x6, x7,  [sp, 16 * 4]
        ldp     x8, x9,  [sp, 16 * 5]
        ldp     x10,x11, [sp, 16 * 6]
        ldp     x12,x13, [sp, 16 * 7]
        ldp     x14,x15, [sp, 16 * 8]
        ldp     x16,x17, [sp, 16 * 9]
        ldp     x29,x30, [sp], 16 * 10

        b       AssignAndMarkCards
    LEAF_END RhpAssignRefArm64

;; Same as RhpAssignRefArm64, but with standard ABI.
    LEAF_ENTRY RhpAssignRef, _TEXT
        mov     x14, x0                     ;; x14 = dst
        mov     x15, x1                     ;; x15 = val
        b       RhpAssignRefArm64
    LEAF_END RhpAssignRef

;; RhpCheckedLockCmpXchg(Object** dest, Object* value, Object* comparand)
;;
;; Interlocked compare exchange on objectref.
;;
;; On entry:
;;  x0: pointer to objectref
;;  x1: exchange value
;;  x2: comparand
;;
;; On exit:
;;  x0: original value of objectref
;;  x10, x12, x17: trashed
;;
    LEAF_ENTRY RhpCheckedLockCmpXchg
    ;; check if dst is in heap
        PREPARE_EXTERNAL_VAR_INDIRECT g_card_bundle_table, x12
        add     x12, x12, x0, lsr #30
        ldrb    w12, [x12]
        cbz     x12, JustAssign_Cmp_Xchg

    ;; check for escaping assignment
    ;; 1) check if we own the source region
#ifdef FEATURE_SATORI_EXTERNAL_OBJECTS
        PREPARE_EXTERNAL_VAR_INDIRECT g_card_bundle_table, x12
        add     x12, x12, x1, lsr #30
        ldrb    w12, [x12]
        cbz     x12, JustAssign_Cmp_Xchg
#else
        cbz     x1, JustAssign_Cmp_Xchg    ;; assigning null
#endif
        and     x12,  x1, #0xFFFFFFFFFFE00000   ; source region
        ldr     x12, [x12]                      ; region tag
        cmp     x12, x18                        ; x18 - TEB
        bne     AssignAndMarkCards_Cmp_Xchg     ; not local to this thread

    ;; 2) check if the src and dst are from the same region
        eor     x12, x0, x1
        lsr     x12, x12, #21
        cbnz    x12, RecordEscape_Cmp_Xchg  ;; cross region assignment. definitely escaping

    ;; 3) check if the target is exposed
        ubfx        x17, x0,#9,#12              ;; word index = (dst >> 9) & 0x1FFFFF
        and         x12, x1, #0xFFFFFFFFFFE00000  ;; source region
        ldr         x17, [x12, x17, lsl #3]     ;; mark word = [region + index * 8]
        lsr         x12, x0, #3                 ;; bit = (dst >> 3) [& 63]
        lsr         x17, x17, x12
        tbnz        x17, #0, RecordEscape_Cmp_Xchg ;; target is exposed. record an escape.

JustAssign_Cmp_Xchg
        mov     x14, x0
TryAgain_Cmp_Xchg
    ALTERNATE_ENTRY RhpCheckedLockCmpXchgAVLocationNotHeap
        ldaxr   x0, [x14]
        cmp     x0, x2
        bne     NoUpdate_Cmp_Xchg

        ;; Current value matches comparand, attempt to update with the new value.
        stlxr   w12, x1, [x14]
        cbnz    w12, TryAgain_Cmp_Xchg  ;; if failed, try again

NoUpdate_Cmp_Xchg
        dmb     ish
        ret     lr

AssignAndMarkCards_Cmp_Xchg
        mov    x14, x0                       ;; x14 = dst
        mov    x15, x1                       ;; x15 = val
TryAgain1_Cmp_Xchg
    ALTERNATE_ENTRY RhpCheckedLockCmpXchgAVLocation
        ldaxr   x0, [x14]
        cmp     x0, x2
        bne     NoUpdate_Cmp_Xchg

        ;; Current value matches comparand, attempt to update with the new value.
        stlxr   w12, x1, [x14]
        cbnz    w12, TryAgain1_Cmp_Xchg  ;; if failed, try again
        dmb     ish

        eor     x12, x14, x15
        lsr     x12, x12, #21
        cbz     x12, CheckConcurrent_Cmp_Xchg ;; same region, just check if barrier is not concurrent

    ;; we will trash x2 and x3, this is a regular call, so it is ok
    ;; if src is in gen2/3 and the barrier is not concurrent we do not need to mark cards
        and     x2,  x15, #0xFFFFFFFFFFE00000     ;; source region
        ldr     w12, [x2, 16]
        tbz     x12, #1, MarkCards_Cmp_Xchg

CheckConcurrent_Cmp_Xchg
        PREPARE_EXTERNAL_VAR_INDIRECT g_write_watch_table, x12  ;; !g_write_watch_table -> !concurrent
        cbnz    x12, MarkCards_Cmp_Xchg
        
Exit_Cmp_Xchg
        ret  lr

MarkCards_Cmp_Xchg
    ;; fetch card location for x14
        PREPARE_EXTERNAL_VAR_INDIRECT g_card_table, x12  ;; fetch the page map
        lsr     x17, x14, #30
        ldr     x17, [x12, x17, lsl #3]              ;; page
        sub     x2,  x14, x17   ;; offset in page
        lsr     x15, x2,  #21   ;; group index
        lsl     x15, x15, #1    ;; group offset (index * 2)
        lsr     x2,  x2,  #9    ;; card offset

    ;; check if concurrent marking is in progress
        PREPARE_EXTERNAL_VAR_INDIRECT g_write_watch_table, x12  ;; !g_write_watch_table -> !concurrent
        cbnz    x12, DirtyCard_Cmp_Xchg

    ;; SETTING CARD FOR X14
SetCard_Cmp_Xchg
        ldrb    w3, [x17, x2]
        cbnz    w3, CardSet_Cmp_Xchg
        mov     w3, #1
        strb    w3, [x17, x2]
SetGroup_Cmp_Xchg
        add     x12, x17, #0x80
        ldrb    w3, [x12, x15]
        cbnz    w3, CardSet_Cmp_Xchg
        mov     w3, #1
        strb    w3, [x12, x15]
SetPage_Cmp_Xchg
        ldrb    w3, [x17]
        cbnz    w3, CardSet_Cmp_Xchg
        mov     w3, #1
        strb    w3, [x17]

CardSet_Cmp_Xchg
    ;; check if concurrent marking is still not in progress
        PREPARE_EXTERNAL_VAR_INDIRECT g_write_watch_table, x12  ;; !g_write_watch_table -> !concurrent
        cbnz    x12, DirtyCard_Cmp_Xchg
        b       Exit_Cmp_Xchg

    ;; DIRTYING CARD FOR X14
DirtyCard_Cmp_Xchg
        mov     w3, #3
        strb    w3, [x17, x2]
DirtyGroup_Cmp_Xchg
        add     x12, x17, #0x80
        strb    w3, [x12, x15]
DirtyPage_Cmp_Xchg
        strb    w3, [x17]
        b       Exit_Cmp_Xchg

    ;; this is expected to be rare.
RecordEscape_Cmp_Xchg

    ;; 4) check if the source is escaped
        and         x12, x1, #0xFFFFFFFFFFE00000  ;; source region
        add         x1, x1, #8                   ;; escape bit is MT + 1
        ubfx        x17, x1, #9,#12               ;; word index = (dst >> 9) & 0x1FFFFF
        ldr         x17, [x12, x17, lsl #3]        ;; mark word = [region + index * 8]
        lsr         x12, x1, #3                   ;; bit = (dst >> 3) [& 63]
        sub         x1, x1, #8     ;; undo MT + 1
        lsr         x17, x17, x12
        tbnz        x17, #0, AssignAndMarkCards_Cmp_Xchg        ;; source is already escaped.

        ;; we need to preserve our parameters x0, x1, x2 and x29/x30

        stp     x29,x30, [sp, -16 * 3]!
        stp     x0, x1,  [sp, 16 * 1]
        str     x2,      [sp, 16 * 2]

        ;; void SatoriRegion::EscapeFn(SatoriObject** dst, SatoriObject* src, SatoriRegion* region)
        and  x2, x1, #0xFFFFFFFFFFE00000  ;; source region
        ldr  x12, [x2, #8]                 ;; EscapeFn address
        blr  x12

        ldp     x0, x1,  [sp, 16 * 1]
        ldr     x2,      [sp, 16 * 2]
        ldp     x29,x30, [sp], 16 * 3

        b       AssignAndMarkCards_Cmp_Xchg
    LEAF_END RhpCheckedLockCmpXchg

;; WARNING: Code in EHHelpers.cpp makes assumptions about write barrier code, in particular:
;; - Function "InWriteBarrierHelper" assumes an AV due to passed in null pointer will happen within at RhpCheckedXchgAVLocation
;; - Function "UnwindSimpleHelperToCaller" assumes no registers were pushed and LR contains the return address

;; RhpCheckedXchg(Object** destination, Object* value)
;;
;; Interlocked exchange on objectref.
;;
;; On entry:
;;  x0: pointer to objectref
;;  x1: exchange value
;;
;; On exit:
;;  x0: original value of objectref
;;  x10: trashed
;;  x12, x17: trashed
;;
    LEAF_ENTRY RhpCheckedXchg, _TEXT

    ;; check if dst is in heap
        PREPARE_EXTERNAL_VAR_INDIRECT g_card_bundle_table, x12
        add     x12, x12, x0, lsr #30
        ldrb    w12, [x12]
        cbz     x12, JustAssign_Xchg

    ;; check for escaping assignment
    ;; 1) check if we own the source region
#ifdef FEATURE_SATORI_EXTERNAL_OBJECTS
        PREPARE_EXTERNAL_VAR_INDIRECT g_card_bundle_table, x12
        add     x12, x12, x1, lsr #30
        ldrb    w12, [x12]
        cbz     x12, JustAssign_Xchg
#else
        cbz     x1, JustAssign_Xchg    ;; assigning null
#endif
        and     x12,  x1, #0xFFFFFFFFFFE00000   ; source region
        ldr     x12, [x12]                      ; region tag
        cmp     x12, x18                        ; x18 - TEB
        bne     AssignAndMarkCards_Xchg         ; not local to this thread

    ;; 2) check if the src and dst are from the same region
        eor     x12, x0, x1
        lsr     x12, x12, #21
        cbnz    x12, RecordEscape_Xchg  ;; cross region assignment. definitely escaping

    ;; 3) check if the target is exposed
        ubfx        x17, x0,#9,#12              ;; word index = (dst >> 9) & 0x1FFFFF
        and         x12, x1, #0xFFFFFFFFFFE00000  ;; source region
        ldr         x17, [x12, x17, lsl #3]      ;; mark word = [region + index * 8]
        lsr         x12, x0, #3                 ;; bit = (dst >> 3) [& 63]
        lsr         x17, x17, x12
        tbnz        x17, #0, RecordEscape_Xchg ;; target is exposed. record an escape.

JustAssign_Xchg
TryAgain_Xchg
    ALTERNATE_ENTRY RhpCheckedXchgAVLocationNotHeap
        ldaxr   x17, [x0]
        stlxr   w12, x1, [x0]
        cbnz    w12, TryAgain_Xchg
        mov     x0, x17
        dmb     ish
        ret    lr

AssignAndMarkCards_Xchg
        mov    x14, x0                        ;; x14 = dst
        mov    x15, x1                        ;; x15 = val    ;; TODO: VS not needed, can use x1
TryAgain1_Xchg
    ALTERNATE_ENTRY RhpCheckedXchgAVLocation
        ldaxr   x17, [x0]
        stlxr   w12, x1, [x0]
        cbnz    w12, TryAgain1_Xchg
        mov     x0, x17
        dmb     ish

        eor     x12, x14, x15
        lsr     x12, x12, #21
        cbz     x12, CheckConcurrent_Xchg ;; same region, just check if barrier is not concurrent

    ;; we will trash x2 and x3, this is a regular call, so it is ok
    ;; if src is in gen2/3 and the barrier is not concurrent we do not need to mark cards
        and     x2,  x15, #0xFFFFFFFFFFE00000     ;; source region
        ldr     w12, [x2, 16]
        tbz     x12, #1, MarkCards_Xchg

CheckConcurrent_Xchg
        PREPARE_EXTERNAL_VAR_INDIRECT g_write_watch_table, x12  ;; !g_write_watch_table -> !concurrent
        cbnz    x12, MarkCards_Xchg
        
Exit_Xchg
        ret  lr

MarkCards_Xchg
    ;; fetch card location for x14
        PREPARE_EXTERNAL_VAR_INDIRECT g_card_table, x12  ;; fetch the page map
        lsr     x17, x14, #30
        ldr     x17, [x12, x17, lsl #3]              ;; page
        sub     x2,  x14, x17   ;; offset in page
        lsr     x15, x2,  #21   ;; group index
        lsl     x15, x15, #1    ;; group offset (index * 2)
        lsr     x2,  x2,  #9    ;; card offset

    ;; check if concurrent marking is in progress
        PREPARE_EXTERNAL_VAR_INDIRECT g_write_watch_table, x12  ;; !g_write_watch_table -> !concurrent
        cbnz    x12, DirtyCard_Xchg

    ;; SETTING CARD FOR X14
SetCard_Xchg
        ldrb    w3, [x17, x2]
        cbnz    w3, CardSet_Xchg
        mov     w3, #1
        strb    w3, [x17, x2]
SetGroup_Xchg
        add     x12, x17, #0x80
        ldrb    w3, [x12, x15]
        cbnz    w3, CardSet_Xchg
        mov     w3, #1
        strb    w3, [x12, x15]
SetPage_Xchg
        ldrb    w3, [x17]
        cbnz    w3, CardSet_Xchg
        mov     w3, #1
        strb    w3, [x17]

CardSet_Xchg
    ;; check if concurrent marking is still not in progress
        PREPARE_EXTERNAL_VAR_INDIRECT g_write_watch_table, x12  ;; !g_write_watch_table -> !concurrent
        cbnz    x12, DirtyCard_Xchg
        b       Exit_Xchg

    ;; DIRTYING CARD FOR X14
DirtyCard_Xchg
        mov     w3, #3
        strb    w3, [x17, x2]
DirtyGroup_Xchg
        add     x12, x17, #0x80
        strb    w3, [x12, x15]
DirtyPage_Xchg
        strb    w3, [x17]
        b       Exit_Xchg

    ;; this is expected to be rare.
RecordEscape_Xchg

    ;; 4) check if the source is escaped
        and         x12, x1, #0xFFFFFFFFFFE00000  ;; source region
        add         x1, x1, #8                   ;; escape bit is MT + 1
        ubfx        x17, x1, #9,#12               ;; word index = (dst >> 9) & 0x1FFFFF
        ldr         x17, [x12, x17, lsl #3]        ;; mark word = [region + index * 8]
        lsr         x12, x1, #3                   ;; bit = (dst >> 3) [& 63]
        sub         x1, x1, #8     ;; undo MT + 1
        lsr         x17, x17, x12
        tbnz        x17, #0, AssignAndMarkCards_Xchg        ;; source is already escaped.

        ;; we need to preserve our parameters x0, x1 and x29/x30
        stp     x29,x30, [sp, -16 * 2]!
        stp     x0, x1,  [sp, 16 * 1]

        ;; void SatoriRegion::EscapeFn(SatoriObject** dst, SatoriObject* src, SatoriRegion* region)
        and  x2, x1, #0xFFFFFFFFFFE00000  ;; source region
        ldr  x12, [x2, #8]                 ;; EscapeFn address
        blr  x12

        ldp     x0, x1,  [sp, 16 * 1]
        ldp     x29,x30, [sp], 16 * 2

        b       AssignAndMarkCards_Xchg
    LEAF_END RhpCheckedXchg

#endif

    end
