// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#include <unixasmmacros.inc>

#ifndef FEATURE_SATORI_GC

// Macro used to copy contents of newly updated GC heap locations to a shadow copy of the heap. This is used
// during garbage collections to verify that object references where never written to the heap without using a
// write barrier. Note that we are potentially racing to update the shadow heap while other threads are writing
// new references to the real heap. Since this can not be solved perfectly without critical sections around the
// entire update process, we instead update the shadow location and then re-check the real location (as two
// ordered operations) and if there is a disparity we will re-write the shadow location with a special value
// (INVALIDGCVALUE) which disables the check for that location. Since the shadow heap is only validated at GC
// time and these write barrier operations are atomic wrt to GCs this is sufficient to guarantee that the
// shadow heap contains only valid copies of real heap values or INVALIDGCVALUE.
#ifdef WRITE_BARRIER_CHECK

    .global     $g_GCShadow
    .global     $g_GCShadowEnd

        // On entry:
        //  $destReg: location to be updated
        //  $refReg: objectref to be stored
        //
        // On exit:
        //  x12,x17: trashed
        //  other registers are preserved
        //
        .macro UPDATE_GC_SHADOW destReg, refReg

        // If g_GCShadow is 0, don't perform the check.
        PREPARE_EXTERNAL_VAR_INDIRECT g_GCShadow, X12
        cbz     x12, 1f

        // Save destReg since we're about to modify it (and we need the original value both within the macro and
        // once we exit the macro).
        mov     x17, \destReg

        // Transform destReg into the equivalent address in the shadow heap.
        PREPARE_EXTERNAL_VAR_INDIRECT g_lowest_address, X12
        subs    \destReg, \destReg, x12
        blo     0f

        PREPARE_EXTERNAL_VAR_INDIRECT g_GCShadow, X12
        add     \destReg, \destReg, x12

        PREPARE_EXTERNAL_VAR_INDIRECT g_GCShadowEnd, X12
        cmp     \destReg, x12
        bhs     0f

        // Update the shadow heap.
        str     \refReg, [\destReg]

        // The following read must be strongly ordered wrt to the write we have just performed in order to
        // prevent race conditions.
        dmb     ish

        // Now check that the real heap location still contains the value we just wrote into the shadow heap.
        mov     x12, x17
        ldr     x12, [x12]
        cmp     x12, \refReg
        beq     0f

        // Someone went and updated the real heap. We need to invalidate INVALIDGCVALUE the shadow location since we can not
        // guarantee whose shadow update won.
        movz x12, (INVALIDGCVALUE & 0xFFFF) // #0xcccd
        movk x12, ((INVALIDGCVALUE >> 16) & 0xFFFF), LSL #16
        str     x12, [\destReg]

0:
        // Restore original destReg value
        mov     \destReg, x17

1:
    .endm

#else // WRITE_BARRIER_CHECK

    .macro UPDATE_GC_SHADOW destReg, refReg
    .endm

#endif // WRITE_BARRIER_CHECK

// There are several different helpers used depending on which register holds the object reference. Since all
// the helpers have identical structure we use a macro to define this structure. Two arguments are taken, the
// name of the register that points to the location to be updated and the name of the register that holds the
// object reference (this should be in upper case as it is used in the definition of the name of the helper).

// Define a sub-macro first that expands to the majority of the barrier implementation. This is used below for
// some interlocked helpers that need an inline barrier.

        // On entry:
        //   destReg: location to be updated (cannot be x12,x17)
        //   refReg:  objectref to be stored (cannot be x12,x17)
        //
        // On exit:
        //   x12,x17: trashed
        //
        .macro INSERT_UNCHECKED_WRITE_BARRIER_CORE destReg, refReg

        // Update the shadow copy of the heap with the same value just written to the same heap. (A no-op unless
        // we are in a debug build and write barrier checking has been enabled).
        UPDATE_GC_SHADOW \destReg, \refReg

#ifdef FEATURE_USE_SOFTWARE_WRITE_WATCH_FOR_GC_HEAP
        // Update the write watch table if necessary
        PREPARE_EXTERNAL_VAR_INDIRECT g_write_watch_table, x12

        cbz     x12, 2f
        add     x12, x12, \destReg, lsr #0xc  // SoftwareWriteWatch::AddressToTableByteIndexShift
        ldrb    w17, [x12]
        cbnz    x17, 2f
        mov     w17, #0xFF
        strb    w17, [x12]
#endif

2:
        // We can skip the card table write if the reference is to
        // an object not on the epehemeral segment.
        PREPARE_EXTERNAL_VAR_INDIRECT g_ephemeral_low, x12
        cmp     \refReg, x12
        blo     0f

        PREPARE_EXTERNAL_VAR_INDIRECT g_ephemeral_high, x12
        cmp     \refReg, x12
        bhs     0f

        // Set this objects card, if it has not already been set.
        PREPARE_EXTERNAL_VAR_INDIRECT g_card_table, x12
        add     x17, x12, \destReg, lsr #11

        // Check that this card has not already been written. Avoiding useless writes is a big win on
        // multi-proc systems since it avoids cache thrashing.
        ldrb    w12, [x17]
        cmp     x12, 0xFF
        beq     0f

        mov     x12, 0xFF
        strb    w12, [x17]

#ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
        // Check if we need to update the card bundle table
        PREPARE_EXTERNAL_VAR_INDIRECT g_card_bundle_table, x12
        add     x17, x12, \destReg, lsr #21
        ldrb    w12, [x17]
        cmp     x12, 0xFF
        beq     0f

        mov     x12, 0xFF
        strb    w12, [x17]
#endif

0:
        // Exit label
    .endm

        // On entry:
        //   destReg: location to be updated
        //   refReg:  objectref to be stored
        //
        // On exit:
        //   x12, x17:   trashed
        //
        .macro INSERT_CHECKED_WRITE_BARRIER_CORE destReg, refReg

        // The "check" of this checked write barrier - is destReg
        // within the heap? if no, early out.

        PREPARE_EXTERNAL_VAR_INDIRECT g_lowest_address, x12
        cmp     \destReg, x12

        PREPARE_EXTERNAL_VAR_INDIRECT g_highest_address, x12

        // If \destReg >= g_lowest_address, compare \destReg to g_highest_address.
        // Otherwise, set the C flag (0x2) to take the next branch.
        ccmp    \destReg, x12, #0x2, hs
        bhs     0f

        INSERT_UNCHECKED_WRITE_BARRIER_CORE \destReg, \refReg

0:
        // Exit label
    .endm

// void JIT_ByRefWriteBarrier
// On entry:
//   x13  : the source address (points to object reference to write)
//   x14  : the destination address (object reference written here)
//
// On exit:
//   x13  : incremented by 8
//   x14  : incremented by 8
//   x15  : trashed
//   x12, x17  : trashed
//
//   NOTE: Keep in sync with RBM_CALLEE_TRASH_WRITEBARRIER_BYREF and RBM_CALLEE_GCTRASH_WRITEBARRIER_BYREF
//         if you add more trashed registers.
//
// WARNING: Code in EHHelpers.cpp makes assumptions about write barrier code, in particular:
// - Function "InWriteBarrierHelper" assumes an AV due to passed in null pointer will happen at RhpByRefAssignRefAVLocation1
// - Function "UnwindSimpleHelperToCaller" assumes no registers were pushed and LR contains the return address
LEAF_ENTRY RhpByRefAssignRefArm64, _TEXT

    ALTERNATE_ENTRY RhpByRefAssignRefAVLocation1
        ldr     x15, [x13], 8
        b       C_FUNC(RhpCheckedAssignRefArm64)

LEAF_END RhpByRefAssignRefArm64, _TEXT

// JIT_CheckedWriteBarrier(Object** dst, Object* src)
//
// Write barrier for writes to objects that may reside
// on the managed heap.
//
// On entry:
//   x14 : the destination address (LHS of the assignment).
//         May not be a heap location (hence the checked).
//   x15 : the object reference (RHS of the assignment).
//
// On exit:
//   x12, x17 : trashed
//   x14      : incremented by 8
 LEAF_ENTRY RhpCheckedAssignRefArm64, _TEXT

        // is destReg within the heap?
        PREPARE_EXTERNAL_VAR_INDIRECT g_lowest_address, x12
        cmp     x14, x12

        PREPARE_EXTERNAL_VAR_INDIRECT g_highest_address, x12
        ccmp    x14, x12, #0x2, hs
        bhs     LOCAL_LABEL(NotInHeap)

        b       C_FUNC(RhpAssignRefArm64)

LOCAL_LABEL(NotInHeap):
    ALTERNATE_ENTRY RhpCheckedAssignRefAVLocation
        str     x15, [x14], 8
        ret

LEAF_END RhpCheckedAssignRefArm64, _TEXT

// JIT_WriteBarrier(Object** dst, Object* src)
//
// Write barrier for writes to objects that are known to
// reside on the managed heap.
//
// On entry:
//  x14 : the destination address (LHS of the assignment).
//  x15 : the object reference (RHS of the assignment).
//
// On exit:
//  x12, x17 : trashed
//  x14 : incremented by 8
LEAF_ENTRY RhpAssignRefArm64, _TEXT

    ALTERNATE_ENTRY RhpAssignRefAVLocation
        stlr    x15, [x14]

        INSERT_UNCHECKED_WRITE_BARRIER_CORE x14, x15

        add     x14, x14, 8
        ret

LEAF_END RhpAssignRefArm64, _TEXT

// Same as RhpAssignRefArm64, but with standard ABI.
LEAF_ENTRY RhpAssignRef, _TEXT
        mov     x14, x0                     ; x14 = dst
        mov     x15, x1                     ; x15 = val
        b       C_FUNC(RhpAssignRefArm64)
LEAF_END RhpAssignRef, _TEXT


// Interlocked operation helpers where the location is an objectref, thus requiring a GC write barrier upon
// successful updates.

// WARNING: Code in EHHelpers.cpp makes assumptions about write barrier code, in particular:
// - Function "InWriteBarrierHelper" assumes an AV due to passed in null pointer will happen at RhpCheckedLockCmpXchgAVLocation
// - Function "UnwindSimpleHelperToCaller" assumes no registers were pushed and LR contains the return address

#ifndef LSE_INSTRUCTIONS_ENABLED_BY_DEFAULT
.arch_extension lse
#endif

// RhpCheckedLockCmpXchg(Object** dest, Object* value, Object* comparand)
//
// Interlocked compare exchange on objectref.
//
// On entry:
//  x0: pointer to objectref
//  x1: exchange value
//  x2: comparand
//
// On exit:
//  x0: original value of objectref
//  x10, x12, x16, x17: trashed
//
    LEAF_ENTRY RhpCheckedLockCmpXchg

#ifndef LSE_INSTRUCTIONS_ENABLED_BY_DEFAULT
        PREPARE_EXTERNAL_VAR_INDIRECT_W g_cpuFeatures, 16
        tbz    w16, #ARM64_ATOMICS_FEATURE_FLAG_BIT, LOCAL_LABEL(CmpXchgRetry)
#endif

        mov    x10, x2
        casal  x10, x1, [x0]                  // exchange
        cmp    x2, x10
        bne    LOCAL_LABEL(CmpXchgNoUpdate)

#ifndef LSE_INSTRUCTIONS_ENABLED_BY_DEFAULT
        b      LOCAL_LABEL(DoCardsCmpXchg)
LOCAL_LABEL(CmpXchgRetry):
        // Check location value is what we expect.
        ldaxr   x10, [x0]
        cmp     x10, x2
        bne     LOCAL_LABEL(CmpXchgNoUpdate)

        // Current value matches comparand, attempt to update with the new value.
        stlxr   w12, x1, [x0]
        cbnz    w12, LOCAL_LABEL(CmpXchgRetry)
#endif

LOCAL_LABEL(DoCardsCmpXchg):
        // We have successfully updated the value of the objectref so now we need a GC write barrier.
        // The following barrier code takes the destination in x0 and the value in x1 so the arguments are
        // already correctly set up.

        INSERT_CHECKED_WRITE_BARRIER_CORE x0, x1

LOCAL_LABEL(CmpXchgNoUpdate):
        // x10 still contains the original value.
        mov     x0, x10

#ifndef LSE_INSTRUCTIONS_ENABLED_BY_DEFAULT
        tbnz    w16, #ARM64_ATOMICS_FEATURE_FLAG_BIT, LOCAL_LABEL(NoBarrierCmpXchg)
        InterlockedOperationBarrier
LOCAL_LABEL(NoBarrierCmpXchg):
#endif
        ret     lr

    LEAF_END RhpCheckedLockCmpXchg, _TEXT

// WARNING: Code in EHHelpers.cpp makes assumptions about write barrier code, in particular:
// - Function "InWriteBarrierHelper" assumes an AV due to passed in null pointer will happen within at RhpCheckedXchgAVLocation
// - Function "UnwindSimpleHelperToCaller" assumes no registers were pushed and LR contains the return address

// RhpCheckedXchg(Object** destination, Object* value)
//
// Interlocked exchange on objectref.
//
// On entry:
//  x0: pointer to objectref
//  x1: exchange value
//
// On exit:
//  x0: original value of objectref
//  x10: trashed
//  x12, x16, x17: trashed
//
    LEAF_ENTRY RhpCheckedXchg, _TEXT

#ifndef LSE_INSTRUCTIONS_ENABLED_BY_DEFAULT
        PREPARE_EXTERNAL_VAR_INDIRECT_W g_cpuFeatures, 16
        tbz    w16, #ARM64_ATOMICS_FEATURE_FLAG_BIT, LOCAL_LABEL(ExchangeRetry)
#endif

        swpal  x1, x10, [x0]                   // exchange

#ifndef LSE_INSTRUCTIONS_ENABLED_BY_DEFAULT
        b      LOCAL_LABEL(DoCardsXchg)
LOCAL_LABEL(ExchangeRetry):
        // Read the existing memory location.
        ldaxr   x10,  [x0]

        // Attempt to update with the new value.
        stlxr   w12, x1, [x0]
        cbnz    w12, LOCAL_LABEL(ExchangeRetry)
#endif

LOCAL_LABEL(DoCardsXchg):
        // We have successfully updated the value of the objectref so now we need a GC write barrier.
        // The following barrier code takes the destination in x0 and the value in x1 so the arguments are
        // already correctly set up.

        INSERT_CHECKED_WRITE_BARRIER_CORE x0, x1

        // x10 still contains the original value.
        mov     x0, x10

#ifndef LSE_INSTRUCTIONS_ENABLED_BY_DEFAULT
        tbnz    w16, #ARM64_ATOMICS_FEATURE_FLAG_BIT, LOCAL_LABEL(NoBarrierXchg)
        InterlockedOperationBarrier
LOCAL_LABEL(NoBarrierXchg):
#endif
        ret

    LEAF_END RhpCheckedXchg, _TEXT

#ifndef LSE_INSTRUCTIONS_ENABLED_BY_DEFAULT
.arch_extension nolse
#endif
    LEAF_END RhpCheckedXchg, _TEXT

#else  //FEATURE_SATORI_GC   ######################################################

#ifndef LSE_INSTRUCTIONS_ENABLED_BY_DEFAULT
.arch_extension lse
#endif

// void JIT_ByRefWriteBarrier
// On entry:
//   x13  : the source address (points to object reference to write)
//   x14  : the destination address (object reference written here)
//
// On exit:
//   x13  : incremented by 8
//   x14  : incremented by 8
//   x15  : trashed
//   x12, x17  : trashed
//
LEAF_ENTRY RhpByRefAssignRefArm64, _TEXT

    ALTERNATE_ENTRY RhpByRefAssignRefAVLocation1
    ALTERNATE_ENTRY RhpByRefAssignRefAVLocation2
        ldr     x15, [x13], 8
        b       C_FUNC(RhpCheckedAssignRefArm64)

LEAF_END RhpByRefAssignRefArm64, _TEXT

// JIT_CheckedWriteBarrier(Object** dst, Object* src)
//
// Write barrier for writes to objects that may reside
// on the managed heap.
//
// On entry:
//   x14  : the destination address (LHS of the assignment)
//         May not be a heap location (hence the checked).
//   x15  : the object reference (RHS of the assignment)
//
// On exit:
//   x12  : trashed
//   x14  : trashed (incremented by 8 to implement JIT_ByRefWriteBarrier contract)
//   x15  : trashed
//   x16  : trashed (ip0)
//   x17  : trashed (ip1)
LEAF_ENTRY RhpCheckedAssignRefArm64, _TEXT
    // See if dst is in GCHeap
        PREPARE_EXTERNAL_VAR_INDIRECT g_card_bundle_table, x16
        lsr     x17, x14, #30                       // dst page index
        ldrb    w12, [x16, x17]
        cbz     x12 , LOCAL_LABEL(NotInHeap)
        b       C_FUNC(CheckedEntry)

    LOCAL_LABEL(NotInHeap):
    ALTERNATE_ENTRY RhpCheckedAssignRefAVLocation
        str  x15, [x14], #8
        ret  lr
LEAF_END RhpCheckedAssignRefArm64, _TEXT

// JIT_WriteBarrier(Object** dst, Object* src)
//
// Write barrier for writes to objects that are known to
// reside on the managed heap.
//
// On entry:
//   x14  : the destination address (LHS of the assignment)
//   x15  : the object reference (RHS of the assignment)
//
// On exit:
//   x12  : trashed
//   x14  : trashed (incremented by 8 to implement JIT_ByRefWriteBarrier contract)
//   x15  : trashed
//   x16  : trashed (ip0)
//   x17  : trashed (ip1)
LEAF_ENTRY RhpAssignRefArm64, _TEXT
    // check for escaping assignment
    // 1) check if we own the source region
#ifdef FEATURE_SATORI_EXTERNAL_OBJECTS
        PREPARE_EXTERNAL_VAR_INDIRECT g_card_bundle_table, x16
ALTERNATE_ENTRY CheckedEntry
        lsr     x17, x15, #30                   // source page index
        ldrb    w12, [x16, x17]
        cbz     x12, LOCAL_LABEL(JustAssign)    // null or external (immutable) object
#else
ALTERNATE_ENTRY CheckedEntry
        cbz     x15, LOCAL_LABEL(JustAssign)    // assigning null
#endif
        and     x16,  x15, #0xFFFFFFFFFFE00000  // source region
        ldr     x12, [x16]                      // region tag
#ifdef TARGET_OSX
        mrs     x17, TPIDRRO_EL0
        and     x17, x17, #-8                   // thread tag on OSX
#else
        mrs     x17, TPIDR_EL0                  // thread tag on other Unix
#endif
        cmp     x12, x17     
        bne     LOCAL_LABEL(AssignAndMarkCards) // not local to this thread

    // 2) check if the src and dst are from the same region
        and     x12, x14, #0xFFFFFFFFFFE00000   // target aligned to region
        cmp     x12, x16
        bne     LOCAL_LABEL(RecordEscape)       // cross region assignment. definitely escaping

    // 3) check if the target is exposed
        ubfx        x17, x14,#9,#12             // word index = (dst >> 9) & 0x1FFFFF
        ldr         x17, [x16, x17, lsl #3]     // mark word = [region + index * 8]
        lsr         x12, x14, #3                // bit = (dst >> 3) [& 63]
        lsr         x17, x17, x12
        tbnz        x17, #0, LOCAL_LABEL(RecordEscape) // target is exposed. record an escape.

    // UNORDERED! assignment of unescaped, null or external (immutable) object
    LOCAL_LABEL(JustAssign):
ALTERNATE_ENTRY RhpAssignRefAVLocationNotHeap
        str  x15, [x14], #8
        ret  lr

    LOCAL_LABEL(AssignAndMarkCards):
ALTERNATE_ENTRY RhpAssignRefAVLocation
        stlr    x15, [x14]

    // TUNING: barriers in different modes could be separate pieces of code, but barrier switch 
    //         needs to suspend EE, not sure if skipping mode check would worth that much.
        PREPARE_EXTERNAL_VAR_INDIRECT g_write_watch_table, x17
    // check the barrier state. this must be done after the assignment (in program order)
    // if state == 2 we do not set or dirty cards.
        tbz     x17, #1, LOCAL_LABEL(DoCards)

    LOCAL_LABEL(ExitNoCards):
        add     x14, x14, 8
        ret     lr

    LOCAL_LABEL(DoCards):
    // if same region, just check if barrier is not concurrent
        and     x12, x14, #0xFFFFFFFFFFE00000   // target aligned to region
        cmp     x12, x16
        beq     LOCAL_LABEL(CheckConcurrent)    // same region, just check if barrier is not concurrent

    // if src is in gen2/3 and the barrier is not concurrent we do not need to mark cards
        ldr     w12, [x16, 16]                  // source region + 16 -> generation
        tbz     x12, #1, LOCAL_LABEL(MarkCards)

    LOCAL_LABEL(CheckConcurrent):
    // if not concurrent, exit
        cbz     x17, LOCAL_LABEL(ExitNoCards)

    LOCAL_LABEL(MarkCards):
    // need couple temps. Save before using.
        stp     x2,  x3,  [sp, -16]!

    // fetch card location for x14
        PREPARE_EXTERNAL_VAR_INDIRECT g_card_table, x12  // fetch the page map
        lsr     x16, x14, #30
        ldr     x16, [x12, x16, lsl #3]              // page
        sub     x2,  x14, x16   // offset in page
        lsr     x15, x2,  #20   // group index
        lsr     x2,  x2,  #9    // card offset
        lsl     x15, x15, #1    // group offset (index * 2)

    // check if concurrent marking is in progress
        cbnz    x17, LOCAL_LABEL(DirtyCard)

    // SETTING CARD FOR X14
    LOCAL_LABEL(SetCard):
        ldrb    w3, [x16, x2]
        cbnz    w3, LOCAL_LABEL(Exit)
        mov     w17, #1
        strb    w17, [x16, x2]
    LOCAL_LABEL(SetGroup):
        add     x12, x16, #0x80
        ldrb    w3, [x12, x15]
        cbnz    w3, LOCAL_LABEL(CardSet)
        strb    w17, [x12, x15]
    LOCAL_LABEL(SetPage):
        ldrb    w3, [x16]
        cbnz    w3, LOCAL_LABEL(CardSet)
        strb    w17, [x16]

    LOCAL_LABEL(CardSet):
    // check if concurrent marking is still not in progress
        PREPARE_EXTERNAL_VAR_INDIRECT g_write_watch_table, x12
        cbnz    x12, LOCAL_LABEL(DirtyCard)

    LOCAL_LABEL(Exit):
        ldp  x2,  x3, [sp], 16
        add  x14, x14, 8
        ret  lr

    // DIRTYING CARD FOR X14
    LOCAL_LABEL(DirtyCard):
        mov     w17, #4
        add     x2, x2, x16
        // must be after the field write to allow concurrent clean
        stlrb   w17, [x2]
    LOCAL_LABEL(DirtyGroup):
        add     x12, x16, #0x80
        ldrb    w3, [x12, x15]
        tbnz    w3, #2, LOCAL_LABEL(Exit)
        strb    w17, [x12, x15]
    LOCAL_LABEL(DirtyPage):
        ldrb    w3, [x16]
        tbnz    w3, #2, LOCAL_LABEL(Exit)
        strb    w17, [x16]
        b       LOCAL_LABEL(Exit)

    // this is expected to be rare.
    LOCAL_LABEL(RecordEscape):

    // 4) check if the source is escaped (x16 has source region)
        add         x12, x15, #8                   // escape bit is MT + 1
        ubfx        x17, x12, #9,#12               // word index = (dst >> 9) & 0x1FFFFF
        ldr         x17, [x16, x17, lsl #3]        // mark word = [region + index * 8]
        lsr         x12, x12, #3                   // bit = (dst >> 3) [& 63]
        lsr         x17, x17, x12
        tbnz        x17, #0, LOCAL_LABEL(AssignAndMarkCards)        // source is already escaped.

        // because of the barrier call convention
        // we need to preserve caller-saved x0 through x15 and x29/x30

        stp     x29,x30, [sp, -16 * 9]!
        stp     x0, x1,  [sp, 16 * 1]
        stp     x2, x3,  [sp, 16 * 2]
        stp     x4, x5,  [sp, 16 * 3]
        stp     x6, x7,  [sp, 16 * 4]
        stp     x8, x9,  [sp, 16 * 5]
        stp     x10,x11, [sp, 16 * 6]
        stp     x12,x13, [sp, 16 * 7]
        stp     x14,x15, [sp, 16 * 8]

        // void SatoriRegion::EscapeFn(SatoriObject** dst, SatoriObject* src, SatoriRegion* region)
        // mov  x0, x14  EscapeFn does not use dst, it is just to avoid arg shuffle on x64
        mov  x1, x15
        mov  x2, x16                       // source region
        ldr  x12, [x16, #8]                 // EscapeFn address
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

        and     x16, x15, #0xFFFFFFFFFFE00000  // source region
        b       LOCAL_LABEL(AssignAndMarkCards)
LEAF_END RhpAssignRefArm64, _TEXT

// Same as RhpAssignRefArm64, but with standard ABI.
LEAF_ENTRY RhpAssignRef, _TEXT
        mov     x14, x0                     // x14 = dst
        mov     x15, x1                     // x15 = val
        b       C_FUNC(RhpAssignRefArm64)
LEAF_END RhpAssignRef, _TEXT

// RhpCheckedLockCmpXchg(Object** dest, Object* value, Object* comparand)
//
// Interlocked compare exchange on objectref.
//
// On entry:
//  x0: pointer to objectref
//  x1: exchange value
//  x2: comparand
//
// On exit:
//  x0: original value of objectref
//  x10, x12, x16, x17: trashed
//
LEAF_ENTRY RhpCheckedLockCmpXchg
    // check if dst is in heap
    // x10 contains region map, also, nonzero x10 means do not skip cards 
        PREPARE_EXTERNAL_VAR_INDIRECT g_card_bundle_table, x10
        add     x12, x10, x0, lsr #30
        ldrb    w12, [x12]
        cbz     x12, LOCAL_LABEL(JustAssign_Cmp_Xchg)

    // check for escaping assignment
    // 1) check if we own the source region
#ifdef FEATURE_SATORI_EXTERNAL_OBJECTS
        add     x12, x10, x1, lsr #30
        ldrb    w12, [x12]
        cbz     x12, LOCAL_LABEL(JustAssign_Cmp_Xchg)
#else
        cbz     x1, LOCAL_LABEL(JustAssign_Cmp_Xchg)    // assigning null
#endif
        and     x16,  x1, #0xFFFFFFFFFFE00000   // source region
        ldr     x12, [x16]                      // region tag
#ifdef TARGET_OSX
        mrs     x17, TPIDRRO_EL0
        and     x17, x17, #-8                   // thread tag on OSX
#else
        mrs     x17, TPIDR_EL0                  // thread tag on other Unix
#endif
        cmp     x12, x17     
        bne     LOCAL_LABEL(AssignAndMarkCards_Cmp_Xchg) // not local to this thread

    // 2) check if the src and dst are from the same region
        and     x12, x0, #0xFFFFFFFFFFE00000    // target aligned to region
        cmp     x12, x16
        bne     LOCAL_LABEL(RecordEscape_Cmp_Xchg)  // cross region assignment. definitely escaping

    // 3) check if the target is exposed
        ubfx        x17, x0,#9,#12              // word index = (dst >> 9) & 0x1FFFFF
        ldr         x17, [x16, x17, lsl #3]     // mark word = [region + index * 8]
        lsr         x12, x0, #3                 // bit = (dst >> 3) [& 63]
        lsr         x17, x17, x12
        tbnz        x17, #0, LOCAL_LABEL(RecordEscape_Cmp_Xchg) // target is exposed. record an escape.

    LOCAL_LABEL(JustAssign_Cmp_Xchg):
        // skip setting cards
        mov      x10, #0

    LOCAL_LABEL(AssignAndMarkCards_Cmp_Xchg):
        mov    x14, x0                       // x14 = dst
        mov    x15, x1                       // x15 = val

#ifndef LSE_INSTRUCTIONS_ENABLED_BY_DEFAULT
        PREPARE_EXTERNAL_VAR_INDIRECT_W g_cpuFeatures, 17
        tbz    w17, #ARM64_ATOMICS_FEATURE_FLAG_BIT, LOCAL_LABEL(TryAgain1_Cmp_Xchg)
#endif

        mov    x17, x2
        casal  x2, x1, [x0]                  // exchange
        mov    x0, x2                        // x0 = result
        cmp    x2, x17
        bne    LOCAL_LABEL(Exit_Cmp_XchgNoCards)

#ifndef LSE_INSTRUCTIONS_ENABLED_BY_DEFAULT
        b      LOCAL_LABEL(SkipLLScCmpXchg)
LOCAL_LABEL(NoUpdate_Cmp_Xchg):
        dmb     ish
        ret     lr

    LOCAL_LABEL(TryAgain1_Cmp_Xchg):
        ldaxr   x0, [x14]
        cmp     x0, x2
        bne     LOCAL_LABEL(NoUpdate_Cmp_Xchg)

        // Current value matches comparand, attempt to update with the new value.
        stlxr   w12, x1, [x14]
        cbnz    w12, LOCAL_LABEL(TryAgain1_Cmp_Xchg)  // if failed, try again
        dmb     ish

LOCAL_LABEL(SkipLLScCmpXchg):
#endif

        cbnz    x10, LOCAL_LABEL(DoCardsCmpXchg)
LOCAL_LABEL(Exit_Cmp_XchgNoCards):
        ret     lr

LOCAL_LABEL(DoCardsCmpXchg):

    // TUNING: barriers in different modes could be separate pieces of code, but barrier switch 
    //         needs to suspend EE, not sure if skipping mode check would worth that much.
        PREPARE_EXTERNAL_VAR_INDIRECT g_write_watch_table, x17

    // check the barrier state. this must be done after the assignment (in program order)
    // if state == 2 we do not set or dirty cards.
        tbnz    x17, #1, LOCAL_LABEL(Exit_Cmp_XchgNoCards)

    LOCAL_LABEL(DoCardsCmpXchg1):
    // if same region, just check if barrier is not concurrent
        and     x12, x14, #0xFFFFFFFFFFE00000   // target aligned to region
        cmp     x12, x16
        beq     LOCAL_LABEL(CheckConcurrentCmpXchg)    // same region, just check if barrier is not concurrent

    // if src is in gen2/3 and the barrier is not concurrent we do not need to mark cards
        ldr     w12, [x16, 16]                  // source region + 16 -> generation
        tbz     x12, #1, LOCAL_LABEL(MarkCardsCmpXchg)

    LOCAL_LABEL(CheckConcurrentCmpXchg):
    // if not concurrent, exit
        cbz     x17, LOCAL_LABEL(Exit_Cmp_XchgNoCards)

    LOCAL_LABEL(MarkCardsCmpXchg):
    // need couple temps. Save before using.
        stp     x2,  x3,  [sp, -16]!

    // fetch card location for x14
        PREPARE_EXTERNAL_VAR_INDIRECT g_card_table, x12  // fetch the page map
        lsr     x16, x14, #30
        ldr     x16, [x12, x16, lsl #3]              // page
        sub     x2,  x14, x16   // offset in page
        lsr     x15, x2,  #20   // group index
        lsr     x2,  x2,  #9    // card offset
        lsl     x15, x15, #1    // group offset (index * 2)

    // check if concurrent marking is in progress
        cbnz    x17, LOCAL_LABEL(DirtyCardCmpXchg)

    // SETTING CARD FOR X14
    LOCAL_LABEL(SetCardCmpXchg):
        ldrb    w3, [x16, x2]
        cbnz    w3, LOCAL_LABEL(ExitCmpXchg)
        mov     w17, #1
        strb    w17, [x16, x2]
    LOCAL_LABEL(SetGroupCmpXchg):
        add     x12, x16, #0x80
        ldrb    w3, [x12, x15]
        cbnz    w3, LOCAL_LABEL(CardSetCmpXchg)
        strb    w17, [x12, x15]
    LOCAL_LABEL(SetPageCmpXchg):
        ldrb    w3, [x16]
        cbnz    w3, LOCAL_LABEL(CardSetCmpXchg)
        strb    w17, [x16]

    LOCAL_LABEL(CardSetCmpXchg):
    // check if concurrent marking is still not in progress
        PREPARE_EXTERNAL_VAR_INDIRECT g_write_watch_table, x12
        cbnz    x12, LOCAL_LABEL(DirtyCardCmpXchg)

    LOCAL_LABEL(ExitCmpXchg):
        ldp  x2,  x3, [sp], 16
        ret  lr

    // DIRTYING CARD FOR X14
    LOCAL_LABEL(DirtyCardCmpXchg):
        mov     w17, #4
        add     x2, x2, x16
        // must be after the field write to allow concurrent clean
        stlrb   w17, [x2]
    LOCAL_LABEL(DirtyGroupCmpXchg):
        add     x12, x16, #0x80
        ldrb    w3, [x12, x15]
        tbnz    w3, #2, LOCAL_LABEL(ExitCmpXchg)
        strb    w17, [x12, x15]
    LOCAL_LABEL(DirtyPagCmpXchge):
        ldrb    w3, [x16]
        tbnz    w3, #2, LOCAL_LABEL(ExitCmpXchg)
        strb    w17, [x16]
        b       LOCAL_LABEL(ExitCmpXchg)

    // this is expected to be rare.
    LOCAL_LABEL(RecordEscape_Cmp_Xchg):

    // 4) check if the source is escaped
        add         x12, x1, #8                    // escape bit is MT + 1
        ubfx        x17, x12, #9,#12               // word index = (dst >> 9) & 0x1FFFFF
        ldr         x17, [x16, x17, lsl #3]        // mark word = [region + index * 8]
        lsr         x12, x12, #3                   // bit = (dst >> 3) [& 63]
        lsr         x17, x17, x12
        tbnz        x17, #0, LOCAL_LABEL(AssignAndMarkCards_Cmp_Xchg)        // source is already escaped.

        // we need to preserve our parameters x0, x1, x2 and x29/x30

        stp     x29,x30, [sp, -16 * 3]!
        stp     x0, x1,  [sp, 16 * 1]
        str     x2,      [sp, 16 * 2]

        // void SatoriRegion::EscapeFn(SatoriObject** dst, SatoriObject* src, SatoriRegion* region)
        mov  x2, x16                       // source region
        ldr  x12, [x16, #8]                // EscapeFn address
        blr  x12

        ldp     x0, x1,  [sp, 16 * 1]
        ldr     x2,      [sp, 16 * 2]
        ldp     x29,x30, [sp], 16 * 3

        // x10 should be not 0 to indicate that can`t skip cards.
        mov     x10,#1
        and     x16,  x1, #0xFFFFFFFFFFE00000   // source region
        b       LOCAL_LABEL(AssignAndMarkCards_Cmp_Xchg)
LEAF_END RhpCheckedLockCmpXchg, _TEXT

// WARNING: Code in EHHelpers.cpp makes assumptions about write barrier code, in particular:
// - Function "InWriteBarrierHelper" assumes an AV due to passed in null pointer will happen within at RhpCheckedXchgAVLocation
// - Function "UnwindSimpleHelperToCaller" assumes no registers were pushed and LR contains the return address

// RhpCheckedXchg(Object** destination, Object* value)
//
// Interlocked exchange on objectref.
//
// On entry:
//  x0: pointer to objectref
//  x1: exchange value
//
// On exit:
//  x0: original value of objectref
//  x10: trashed
//  x12, x17: trashed
//
LEAF_ENTRY RhpCheckedXchg, _TEXT

    PREPARE_EXTERNAL_VAR_INDIRECT g_card_bundle_table, x10
    // check if dst is in heap
        add     x12, x10, x0, lsr #30
        ldrb    w12, [x12]
        cbz     x12, LOCAL_LABEL(JustAssign_Xchg)

    // check for escaping assignment
    // 1) check if we own the source region
#ifdef FEATURE_SATORI_EXTERNAL_OBJECTS
        add     x12, x10, x1, lsr #30
        ldrb    w12, [x12]
        cbz     x12, LOCAL_LABEL(JustAssign_Xchg)
#else
        cbz     x1, LOCAL_LABEL(JustAssign_Xchg)    // assigning null
#endif
        and     x16,  x1, #0xFFFFFFFFFFE00000   // source region
        ldr     x12, [x16]                      // region tag
#ifdef TARGET_OSX
        mrs     x17, TPIDRRO_EL0
        and     x17, x17, #-8                   // thread tag on OSX
#else
        mrs     x17, TPIDR_EL0                  // thread tag on other Unix
#endif
        cmp     x12, x17     
        bne     LOCAL_LABEL(AssignAndMarkCards_Xchg) // not local to this thread

    // 2) check if the src and dst are from the same region
        and     x12, x0, #0xFFFFFFFFFFE00000    // target aligned to region
        cmp     x12, x16
        bne     LOCAL_LABEL(RecordEscape_Xchg)  // cross region assignment. definitely escaping

    // 3) check if the target is exposed
        ubfx        x17, x0,#9,#12              // word index = (dst >> 9) & 0x1FFFFF
        ldr         x17, [x16, x17, lsl #3]     // mark word = [region + index * 8]
        lsr         x12, x0, #3                 // bit = (dst >> 3) [& 63]
        lsr         x17, x17, x12
        tbnz        x17, #0, LOCAL_LABEL(RecordEscape_Xchg) // target is exposed. record an escape.

    LOCAL_LABEL(JustAssign_Xchg):
// TODO: VS use LSE_INSTRUCTIONS_ENABLED_BY_DEFAULT instead
#ifdef TARGET_OSX
        swpal  x1, x0, [x0]                   // exchange
#else
    LOCAL_LABEL(TryAgain_Xchg):
        ldaxr   x17, [x0]
        stlxr   w12, x1, [x0]
        cbnz    w12, LOCAL_LABEL(TryAgain_Xchg)
        mov     x0, x17
        dmb     ish
#endif
        ret    lr

    LOCAL_LABEL(AssignAndMarkCards_Xchg):
        mov    x14, x0                        // x14 = dst
#ifdef TARGET_OSX
        swpal  x1, x0, [x0]                   // exchange
#else
    LOCAL_LABEL(TryAgain1_Xchg):
        ldaxr   x17, [x0]
        stlxr   w12, x1, [x0]
        cbnz    w12, LOCAL_LABEL(TryAgain1_Xchg)
        mov     x0, x17
        dmb     ish
#endif

    // TUNING: barriers in different modes could be separate pieces of code, but barrier switch 
    //         needs to suspend EE, not sure if skipping mode check would worth that much.
        PREPARE_EXTERNAL_VAR_INDIRECT g_write_watch_table, x17
    // check the barrier state. this must be done after the assignment (in program order)
    // if state == 2 we do not set or dirty cards.
        tbz     x17, #1, LOCAL_LABEL(DoCardsXchg)

    LOCAL_LABEL(ExitNoCardsXchg):
        ret     lr

    LOCAL_LABEL(DoCardsXchg):
    // if same region, just check if barrier is not concurrent
        and     x12, x14, #0xFFFFFFFFFFE00000   // target aligned to region
        cmp     x12, x16
        beq     LOCAL_LABEL(CheckConcurrentXchg)    // same region, just check if barrier is not concurrent

    // if src is in gen2/3 and the barrier is not concurrent we do not need to mark cards
        ldr     w12, [x16, 16]                  // source region + 16 -> generation
        tbz     x12, #1, LOCAL_LABEL(MarkCardsXchg)

    LOCAL_LABEL(CheckConcurrentXchg):
    // if not concurrent, exit
        cbz     x17, LOCAL_LABEL(ExitNoCardsXchg)

    LOCAL_LABEL(MarkCardsXchg):
    // need couple temps. Save before using.
        stp     x2,  x3,  [sp, -16]!

    // fetch card location for x14
        PREPARE_EXTERNAL_VAR_INDIRECT g_card_table, x12  // fetch the page map
        lsr     x16, x14, #30
        ldr     x16, [x12, x16, lsl #3]              // page
        sub     x2,  x14, x16   // offset in page
        lsr     x15, x2,  #20   // group index
        lsr     x2,  x2,  #9    // card offset
        lsl     x15, x15, #1    // group offset (index * 2)

    // check if concurrent marking is in progress
        cbnz    x17, LOCAL_LABEL(DirtyCardXchg)

    // SETTING CARD FOR X14
    LOCAL_LABEL(SetCardXchg):
        ldrb    w3, [x16, x2]
        cbnz    w3, LOCAL_LABEL(ExitXchg)
        mov     w17, #1
        strb    w17, [x16, x2]
    LOCAL_LABEL(SetGroupXchg):
        add     x12, x16, #0x80
        ldrb    w3, [x12, x15]
        cbnz    w3, LOCAL_LABEL(CardSetXchg)
        strb    w17, [x12, x15]
    LOCAL_LABEL(SetPageXchg):
        ldrb    w3, [x16]
        cbnz    w3, LOCAL_LABEL(CardSetXchg)
        strb    w17, [x16]

    LOCAL_LABEL(CardSetXchg):
    // check if concurrent marking is still not in progress
        PREPARE_EXTERNAL_VAR_INDIRECT g_write_watch_table, x12
        cbnz    x12, LOCAL_LABEL(DirtyCardXchg)

    LOCAL_LABEL(ExitXchg):
        ldp  x2,  x3, [sp], 16
        ret  lr

    // DIRTYING CARD FOR X14
    LOCAL_LABEL(DirtyCardXchg):
        mov     w17, #4
        add     x2, x2, x16
        // must be after the field write to allow concurrent clean
        stlrb   w17, [x2]
    LOCAL_LABEL(DirtyGroupXchg):
        add     x12, x16, #0x80
        ldrb    w3, [x12, x15]
        tbnz    w3, #2, LOCAL_LABEL(ExitXchg)
        strb    w17, [x12, x15]
    LOCAL_LABEL(DirtyPagXchge):
        ldrb    w3, [x16]
        tbnz    w3, #2, LOCAL_LABEL(ExitXchg)
        strb    w17, [x16]
        b       LOCAL_LABEL(ExitXchg)

    // this is expected to be rare.
    LOCAL_LABEL(RecordEscape_Xchg):

    // 4) check if the source is escaped
        add         x12, x1, #8                    // escape bit is MT + 1
        ubfx        x17, x12, #9,#12               // word index = (dst >> 9) & 0x1FFFFF
        ldr         x17, [x16, x17, lsl #3]        // mark word = [region + index * 8]
        lsr         x12, x12, #3                   // bit = (dst >> 3) [& 63]
        lsr         x17, x17, x12
        tbnz        x17, #0, LOCAL_LABEL(AssignAndMarkCards_Xchg)        // source is already escaped.

        // we need to preserve our parameters x0, x1 and x29/x30
        stp     x29,x30, [sp, -16 * 2]!
        stp     x0, x1,  [sp, 16 * 1]

        // void SatoriRegion::EscapeFn(SatoriObject** dst, SatoriObject* src, SatoriRegion* region)
        mov  x2, x16                       // source region
        ldr  x12, [x16, #8]                // EscapeFn address
        blr  x12

        ldp     x0, x1,  [sp, 16 * 1]
        ldp     x29,x30, [sp], 16 * 2

        and     x16,  x1, #0xFFFFFFFFFFE00000   // source region
        b       LOCAL_LABEL(AssignAndMarkCards_Xchg)
LEAF_END RhpCheckedXchg, _TEXT

#ifndef LSE_INSTRUCTIONS_ENABLED_BY_DEFAULT
.arch_extension nolse
#endif

#endif //FEATURE_SATORI_GC
