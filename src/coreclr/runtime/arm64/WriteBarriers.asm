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

   ALTERNATE_ENTRY RhpByRefAssignRefAVLocation1
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
;;   x12  : trashed
;;   x14  : trashed (incremented by 8 to implement JIT_ByRefWriteBarrier contract)
;;   x15  : trashed
;;   x16  : trashed (ip0)
;;   x17  : trashed (ip1)
    LEAF_ENTRY RhpCheckedAssignRefArm64, _TEXT
    ;; See if dst is in GCHeap
        PREPARE_EXTERNAL_VAR_INDIRECT g_card_bundle_table, x16
        lsr     x17, x14, #30                       ;; dst page index
        ldrb    w12, [x16, x17]
        cbnz    x12, CheckedEntry

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
;;   x14  : the destination address (LHS of the assignment)
;;   x15  : the object reference (RHS of the assignment)
;;
;; On exit:
;;   x12  : trashed
;;   x14  : trashed (incremented by 8 to implement JIT_ByRefWriteBarrier contract)
;;   x15  : trashed
;;   x16  : trashed (ip0)
;;   x17  : trashed (ip1)
    LEAF_ENTRY RhpAssignRefArm64, _TEXT
    ;; check for escaping assignment
    ;; 1) check if we own the source region
#ifdef FEATURE_SATORI_EXTERNAL_OBJECTS
        PREPARE_EXTERNAL_VAR_INDIRECT g_card_bundle_table, x16
    ALTERNATE_ENTRY CheckedEntry
        lsr     x17, x15, #30                   ;; source page index
        ldrb    w12, [x16, x17]
        cbz     x12, JustAssign                 ;; null or external (immutable) object
#else
    ALTERNATE_ENTRY CheckedEntry
        cbz     x15, JustAssign                 ;; assigning null
#endif
        and     x16,  x15, #0xFFFFFFFFFFE00000  ;; source region
        ldr     x12, [x16]                      ;; region tag

        cmp     x12, x18                        ;; x18 - TEB
        bne     AssignAndMarkCards              ;; not local to this thread

    ;; 2) check if the src and dst are from the same region
        and     x12, x14, #0xFFFFFFFFFFE00000   ;; target aligned to region
        cmp     x12, x16
        bne     RecordEscape                    ;; cross region assignment. definitely escaping

    ;; 3) check if the target is exposed
        ubfx    x17, x14,#9,#12                 ;; word index = (dst >> 9) & 0x1FFFFF
        ldr     x17, [x16, x17, lsl #3]         ;; mark word = [region + index * 8]
        lsr     x12, x14, #3                    ;; bit = (dst >> 3) [& 63]
        lsr     x17, x17, x12
        tbnz    x17, #0, RecordEscape           ;; target is exposed. record an escape.

    ;; UNORDERED! assignment of unescaped, null or external (immutable) object
JustAssign
    ALTERNATE_ENTRY RhpAssignRefAVLocationNotHeap
        str      x15, [x14], #8
        ret      lr

AssignAndMarkCards
    ALTERNATE_ENTRY RhpAssignRefAVLocation
        stlr    x15, [x14]

    ; TUNING: barriers in different modes could be separate pieces of code, but barrier switch 
    ;         needs to suspend EE, not sure if skipping mode check would worth that much.
        PREPARE_EXTERNAL_VAR_INDIRECT g_write_watch_table, x17
    ; check the barrier state. this must be done after the assignment (in program order
    ; if state == 2 we do not set or dirty cards.
        tbz     x17, #1, DoCards

ExitNoCards
        add     x14, x14, 8
        ret     lr

DoCards
    ; if same region, just check if barrier is not concurrent
        and     x12, x14, #0xFFFFFFFFFFE00000   ; target aligned to region
        cmp     x12, x16
        beq     CheckConcurrent    ; same region, just check if barrier is not concurrent

    ; if src is in gen2/3 and the barrier is not concurrent we do not need to mark cards
        ldr     w12, [x16, 16]                  ; source region + 16 -> generation
        tbz     x12, #1, MarkCards

CheckConcurrent
    ; if not concurrent, exit
        cbz     x17, ExitNoCards

MarkCards
    ; need couple temps. Save before using.
        stp     x2,  x3,  [sp, -16]!

    ; fetch card location for x14
        PREPARE_EXTERNAL_VAR_INDIRECT g_card_table, x12  ; fetch the page map
        lsr     x16, x14, #30
        ldr     x16, [x12, x16, lsl #3]              ; page
        sub     x2,  x14, x16   ; offset in page
        lsr     x15, x2,  #20   ; group index
        lsr     x2,  x2,  #9    ; card offset
        lsl     x15, x15, #1    ; group offset (index * 2)

    ; check if concurrent marking is in progress
        cbnz    x17, DirtyCard

    ; SETTING CARD FOR X14
SetCard
        ldrb    w3, [x16, x2]
        cbnz    w3, Exit
        mov     w17, #1
        strb    w17, [x16, x2]
SetGroup
        add     x12, x16, #0x80
        ldrb    w3, [x12, x15]
        cbnz    w3, CardSet
        strb    w17, [x12, x15]
SetPage
        ldrb    w3, [x16]
        cbnz    w3, CardSet
        strb    w17, [x16]

CardSet
    ; check if concurrent marking is still not in progress
        PREPARE_EXTERNAL_VAR_INDIRECT g_write_watch_table, x12
        cbnz    x12, DirtyCard

Exit
        ldp  x2,  x3, [sp], 16
        add  x14, x14, 8
        ret  lr

    ; DIRTYING CARD FOR X14
DirtyCard
        mov     w17, #4
        add     x2, x2, x16
        ; must be after the field write to allow concurrent clean
        stlrb   w17, [x2]
DirtyGroup
        add     x12, x16, #0x80
        ldrb    w3, [x12, x15]
        tbnz    w3, #2, Exit
        strb    w17, [x12, x15]
DirtyPage
        ldrb    w3, [x16]
        tbnz    w3, #2, Exit
        strb    w17, [x16]
        b       Exit

    ;; this is expected to be rare.
RecordEscape

    ;; 4) check if the source is escaped (x16 has source region)
        add         x12, x15, #8                   ;; escape bit is MT + 1
        ubfx        x17, x12, #9,#12               ;; word index = (dst >> 9) & 0x1FFFFF
        ldr         x17, [x16, x17, lsl #3]        ;; mark word = [region + index * 8]
        lsr         x12, x12, #3                   ;; bit = (dst >> 3) [& 63]
        lsr         x17, x17, x12
        tbnz        x17, #0, AssignAndMarkCards        ;; source is already escaped.

        ;; because of the barrier call convention
        ;; we need to preserve caller-saved x0 through x15 and x29/x30

        stp     x29,x30, [sp, -16 * 9]!
        stp     x0, x1,  [sp, 16 * 1]
        stp     x2, x3,  [sp, 16 * 2]
        stp     x4, x5,  [sp, 16 * 3]
        stp     x6, x7,  [sp, 16 * 4]
        stp     x8, x9,  [sp, 16 * 5]
        stp     x10,x11, [sp, 16 * 6]
        stp     x12,x13, [sp, 16 * 7]
        stp     x14,x15, [sp, 16 * 8]

        ;; void SatoriRegion::EscapeFn(SatoriObject** dst, SatoriObject* src, SatoriRegion* region)
        ;; mov  x0, x14  EscapeFn does not use dst, it is just to avoid arg shuffle on x64
        mov  x1, x15
        mov  x2, x16                       ;; source region
        ldr  x12, [x16, #8]                 ;; EscapeFn address
        blr  x12

        ldp     x0, x1,  [sp, 16 * 1]
        ldp     x2, x3,  [sp, 16 * 2]
        ldp     x4, x5,  [sp, 16 * 3]
        ldp     x6, x7,  [sp, 16 * 4]
        ldp     x8, x9,  [sp, 16 * 5]
        ldp     x10,x11, [sp, 16 * 6]
        ldp     x12,x13, [sp, 16 * 7]
        ldp     x14,x15, [sp, 16 * 8]
        ldp     x29,x30, [sp], 16 * 9

        and     x16, x15, #0xFFFFFFFFFFE00000  ;; source region
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
;;  x10, x12, x16, x17: trashed
;;
    LEAF_ENTRY RhpCheckedLockCmpXchg
    ;; check if dst is in heap
    ;; x10 contains region map, also, nonzero x10 means do not skip cards 
        PREPARE_EXTERNAL_VAR_INDIRECT g_card_bundle_table, x10
        add     x12, x10, x0, lsr #30
        ldrb    w12, [x12]
        cbz     x12, JustAssign_Cmp_Xchg

    ;; check for escaping assignment
    ;; 1) check if we own the source region
#ifdef FEATURE_SATORI_EXTERNAL_OBJECTS
        add     x12, x10, x1, lsr #30
        ldrb    w12, [x12]
        cbz     x12, JustAssign_Cmp_Xchg
#else
        cbz     x1, JustAssign_Cmp_Xchg    ;; assigning null
#endif
        and     x16,  x1, #0xFFFFFFFFFFE00000   ;; source region
        ldr     x12, [x16]                      ;; region tag

        cmp     x12, x18                        ;; x18 - TEB
        bne     AssignAndMarkCards_Cmp_Xchg     ;; not local to this thread

    ;; 2) check if the src and dst are from the same region
        and     x12, x0, #0xFFFFFFFFFFE00000    ;; target aligned to region
        cmp     x12, x16
        bne     RecordEscape_Cmp_Xchg           ;; cross region assignment. definitely escaping

    ;; 3) check if the target is exposed
        ubfx        x17, x0,#9,#12              ;; word index = (dst >> 9) & 0x1FFFFF
        ldr         x17, [x16, x17, lsl #3]     ;; mark word = [region + index * 8]
        lsr         x12, x0, #3                 ;; bit = (dst >> 3) [& 63]
        lsr         x17, x17, x12
        tbnz        x17, #0, RecordEscape_Cmp_Xchg ;; target is exposed. record an escape.

JustAssign_Cmp_Xchg
        ;; skip setting cards
    ALTERNATE_ENTRY RhpCheckedLockCmpXchgAVLocationNotHeap
        mov     x10, #0

AssignAndMarkCards_Cmp_Xchg
        mov    x14, x0                       ;; x14 = dst
        mov    x15, x1                       ;; x15 = val

#ifndef LSE_INSTRUCTIONS_ENABLED_BY_DEFAULT
        PREPARE_EXTERNAL_VAR_INDIRECT_W g_cpuFeatures, 17
        tbz    w17, #ARM64_ATOMICS_FEATURE_FLAG_BIT, TryAgain1_Cmp_Xchg
#endif

        mov    x17, x2
    ALTERNATE_ENTRY RhpCheckedLockCmpXchgAVLocation
        casal  x2, x1, [x0]                  ;; exchange
        mov    x0, x2                        ;; x0 = result
        cmp    x2, x17
        bne    Exit_Cmp_XchgNoCards

#ifndef LSE_INSTRUCTIONS_ENABLED_BY_DEFAULT
        b      SkipLLScCmpXchg
NoUpdate_Cmp_Xchg
        dmb     ish
        ret     lr

TryAgain1_Cmp_Xchg
    ALTERNATE_ENTRY RhpCheckedLockCmpXchgAVLocation2
        ldaxr   x0, [x14]
        cmp     x0, x2
        bne     NoUpdate_Cmp_Xchg

        ;; Current value matches comparand, attempt to update with the new value.
        stlxr   w12, x1, [x14]
        cbnz    w12, TryAgain1_Cmp_Xchg  ;; if failed, try again
        dmb     ish

SkipLLScCmpXchg
#endif

        cbnz    x10, DoCardsCmpXchg
Exit_Cmp_XchgNoCards
        ret     lr

DoCardsCmpXchg

    ; TUNING: barriers in different modes could be separate pieces of code, but barrier switch 
    ;         needs to suspend EE, not sure if skipping mode check would worth that much.
        PREPARE_EXTERNAL_VAR_INDIRECT g_write_watch_table, x17

    ; check the barrier state. this must be done after the assignment (in program order
    ; if state == 2 we do not set or dirty cards.
        tbnz     x17, #1, Exit_Cmp_XchgNoCards

    ; if same region, just check if barrier is not concurrent
        and     x12, x14, #0xFFFFFFFFFFE00000   ; target aligned to region
        cmp     x12, x16
        beq     CheckConcurrentCmpXchg    ; same region, just check if barrier is not concurrent

    ; if src is in gen2/3 and the barrier is not concurrent we do not need to mark cards
        ldr     w12, [x16, 16]                  ; source region + 16 -> generation
        tbz     x12, #1, MarkCardsCmpXchg

CheckConcurrentCmpXchg
    ; if not concurrent, exit
        cbz     x17, Exit_Cmp_XchgNoCards

MarkCardsCmpXchg
    ; need couple temps. Save before using.
        stp     x2,  x3,  [sp, -16]!

    ; fetch card location for x14
        PREPARE_EXTERNAL_VAR_INDIRECT g_card_table, x12  ; fetch the page map
        lsr     x16, x14, #30
        ldr     x16, [x12, x16, lsl #3]              ; page
        sub     x2,  x14, x16   ; offset in page
        lsr     x15, x2,  #20   ; group index
        lsr     x2,  x2,  #9    ; card offset
        lsl     x15, x15, #1    ; group offset (index * 2)

    ; check if concurrent marking is in progress
        cbnz    x17, DirtyCardCmpXchg

    ; SETTING CARD FOR X14
SetCardCmpXchg
        ldrb    w3, [x16, x2]
        cbnz    w3, ExitCmpXchg
        mov     w17, #1
        strb    w17, [x16, x2]
SetGroupCmpXchg
        add     x12, x16, #0x80
        ldrb    w3, [x12, x15]
        cbnz    w3, CardSetCmpXchg
        strb    w17, [x12, x15]
SetPageCmpXchg
        ldrb    w3, [x16]
        cbnz    w3, CardSetCmpXchg
        strb    w17, [x16]

CardSetCmpXchg
    ; check if concurrent marking is still not in progress
        PREPARE_EXTERNAL_VAR_INDIRECT g_write_watch_table, x12
        cbnz    x12, DirtyCardCmpXchg

ExitCmpXchg
        ldp  x2,  x3, [sp], 16
        ret  lr

    ; DIRTYING CARD FOR X14
DirtyCardCmpXchg
        mov     w17, #4
        add     x2, x2, x16
        ; must be after the field write to allow concurrent clean
        stlrb   w17, [x2]
DirtyGroupCmpXchg
        add     x12, x16, #0x80
        ldrb    w3, [x12, x15]
        tbnz    w3, #2, ExitCmpXchg
        strb    w17, [x12, x15]
DirtyPageCmpXchg
        ldrb    w3, [x16]
        tbnz    w3, #2, ExitCmpXchg
        strb    w17, [x16]
        b       Exit

    ;; this is expected to be rare.
RecordEscape_Cmp_Xchg

    ;; 4) check if the source is escaped
        add         x12, x1, #8                    ;; escape bit is MT + 1
        ubfx        x17, x12, #9,#12               ;; word index = (dst >> 9) & 0x1FFFFF
        ldr         x17, [x16, x17, lsl #3]        ;; mark word = [region + index * 8]
        lsr         x12, x12, #3                   ;; bit = (dst >> 3) [& 63]
        lsr         x17, x17, x12
        tbnz        x17, #0, AssignAndMarkCards_Cmp_Xchg        ;; source is already escaped.

        ;; we need to preserve our parameters x0, x1, x2 and x29/x30

        stp     x29,x30, [sp, -16 * 3]!
        stp     x0, x1,  [sp, 16 * 1]
        str     x2,      [sp, 16 * 2]

        ;; void SatoriRegion::EscapeFn(SatoriObject** dst, SatoriObject* src, SatoriRegion* region)
        mov  x2, x16                        ;; source region
        ldr  x12, [x16, #8]                 ;; EscapeFn address
        blr  x12

        ldp     x0, x1,  [sp, 16 * 1]
        ldr     x2,      [sp, 16 * 2]
        ldp     x29,x30, [sp], 16 * 3

        ;; x10 should be not 0 to indicate that can`t skip cards.
        mov     x10,#1
        and     x16,  x1, #0xFFFFFFFFFFE00000  ;; source region
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

    PREPARE_EXTERNAL_VAR_INDIRECT g_card_bundle_table, x10
    ;; check if dst is in heap
        add     x12, x10, x0, lsr #30
        ldrb    w12, [x12]
        cbz     x12, JustAssign_Xchg

    ;; check for escaping assignment
    ;; 1) check if we own the source region
#ifdef FEATURE_SATORI_EXTERNAL_OBJECTS
        add     x12, x10, x1, lsr #30
        ldrb    w12, [x12]
        cbz     x12, JustAssign_Xchg
#else
        cbz     x1, JustAssign_Xchg    ;; assigning null
#endif
        and     x16,  x1, #0xFFFFFFFFFFE00000   ;; source region
        ldr     x12, [x16]                      ;; region tag

        cmp     x12, x18                        ;; x18 - TEB
        bne     AssignAndMarkCards_Xchg         ;; not local to this thread

    ;; 2) check if the src and dst are from the same region
        and     x12, x0, #0xFFFFFFFFFFE00000    ;; target aligned to region
        cmp     x12, x16
        bne     RecordEscape_Xchg               ;; cross region assignment. definitely escaping

    ;; 3) check if the target is exposed
        ubfx        x17, x0,#9,#12              // word index = (dst >> 9) & 0x1FFFFF
        ldr         x17, [x16, x17, lsl #3]     // mark word = [region + index * 8]
        lsr         x12, x0, #3                 // bit = (dst >> 3) [& 63]
        lsr         x17, x17, x12
        tbnz        x17, #0, RecordEscape_Xchg ;; target is exposed. record an escape.

JustAssign_Xchg
TryAgain_Xchg
    ALTERNATE_ENTRY RhpCheckedXchgAVLocationNotHeap
   ;; TODO: VS use LSE_INSTRUCTIONS_ENABLED_BY_DEFAULT instead
        ldaxr   x17, [x0]
        stlxr   w12, x1, [x0]
        cbnz    w12, TryAgain_Xchg
        mov     x0, x17
        dmb     ish
        ret    lr

AssignAndMarkCards_Xchg
        mov    x14, x0                        ;; x14 = dst
TryAgain1_Xchg
    ALTERNATE_ENTRY RhpCheckedXchgAVLocation
    ALTERNATE_ENTRY RhpCheckedXchgAVLocation2
        ldaxr   x17, [x0]
        stlxr   w12, x1, [x0]
        cbnz    w12, TryAgain1_Xchg
        mov     x0, x17
        dmb     ish

    ; TUNING: barriers in different modes could be separate pieces of code, but barrier switch 
    ;         needs to suspend EE, not sure if skipping mode check would worth that much.
        PREPARE_EXTERNAL_VAR_INDIRECT g_write_watch_table, x17

    ; check the barrier state. this must be done after the assignment (in program order
    ; if state == 2 we do not set or dirty cards.
        tbz     x17, #1, DoCardsXchg

ExitNoCardsXchg
        ret     lr

DoCardsXchg
    ; if same region, just check if barrier is not concurrent
        and     x12, x14, #0xFFFFFFFFFFE00000   ; target aligned to region
        cmp     x12, x16
        beq     CheckConcurrentXchg    ; same region, just check if barrier is not concurrent

    ; if src is in gen2/3 and the barrier is not concurrent we do not need to mark cards
        ldr     w12, [x16, 16]                  ; source region + 16 -> generation
        tbz     x12, #1, MarkCardsXchg

CheckConcurrentXchg
    ; if not concurrent, exit
        cbz     x17, ExitNoCardsXchg

MarkCardsXchg
    ; need couple temps. Save before using.
        stp     x2,  x3,  [sp, -16]!

    ; fetch card location for x14
        PREPARE_EXTERNAL_VAR_INDIRECT g_card_table, x12  ; fetch the page map
        lsr     x16, x14, #30
        ldr     x16, [x12, x16, lsl #3]              ; page
        sub     x2,  x14, x16   ; offset in page
        lsr     x15, x2,  #20   ; group index
        lsr     x2,  x2,  #9    ; card offset
        lsl     x15, x15, #1    ; group offset (index * 2)

    ; check if concurrent marking is in progress
        cbnz    x17, DirtyCardXchg

    ; SETTING CARD FOR X14
SetCardXchg
        ldrb    w3, [x16, x2]
        cbnz    w3, ExitXchg
        mov     w17, #1
        strb    w17, [x16, x2]
SetGroupXchg
        add     x12, x16, #0x80
        ldrb    w3, [x12, x15]
        cbnz    w3, CardSetXchg
        strb    w17, [x12, x15]
SetPageXchg
        ldrb    w3, [x16]
        cbnz    w3, CardSetXchg
        strb    w17, [x16]

CardSetXchg
    ; check if concurrent marking is still not in progress
        PREPARE_EXTERNAL_VAR_INDIRECT g_write_watch_table, x12
        cbnz    x12, DirtyCardXchg

ExitXchg
        ldp  x2,  x3, [sp], 16
        ret  lr

    ; DIRTYING CARD FOR X14
DirtyCardXchg
        mov     w17, #4
        add     x2, x2, x16
        ; must be after the field write to allow concurrent clean
        stlrb   w17, [x2]
DirtyGroupXchg
        add     x12, x16, #0x80
        ldrb    w3, [x12, x15]
        tbnz    w3, #2, ExitXchg
        strb    w17, [x12, x15]
DirtyPageXchg
        ldrb    w3, [x16]
        tbnz    w3, #2, ExitXchg
        strb    w17, [x16]
        b       ExitXchg

    ;; this is expected to be rare.
RecordEscape_Xchg

    ;; 4) check if the source is escaped
        add         x12, x1, #8                    ;; escape bit is MT + 1
        ubfx        x17, x12, #9,#12               ;; word index = (dst >> 9) & 0x1FFFFF
        ldr         x17, [x16, x17, lsl #3]        ;; mark word = [region + index * 8]
        lsr         x12, x12, #3                   ;; bit = (dst >> 3) [& 63]
        lsr         x17, x17, x12
        tbnz        x17, #0, AssignAndMarkCards_Xchg        ;; source is already escaped.

        ;; we need to preserve our parameters x0, x1 and x29/x30
        stp     x29,x30, [sp, -16 * 2]!
        stp     x0, x1,  [sp, 16 * 1]

        ;; void SatoriRegion::EscapeFn(SatoriObject** dst, SatoriObject* src, SatoriRegion* region)
        mov  x2, x16                       ;; source region
        ldr  x12, [x16, #8]                ;; EscapeFn address
        blr  x12

        ldp     x0, x1,  [sp, 16 * 1]
        ldp     x29,x30, [sp], 16 * 2

  ;;    and     x16,  x1, #0xFFFFFFFFFFE00000   ;; source region
        b       AssignAndMarkCards_Xchg
    LEAF_END RhpCheckedXchg

#endif

    end
