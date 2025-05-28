// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#include "asmconstants.h"
#include "unixasmmacros.inc"

//-----------------------------------------------------------------------------
// The following Macros help in WRITE_BARRIER Implementations
// WRITE_BARRIER_ENTRY
//
// Declare the start of a write barrier function. Use similarly to NESTED_ENTRY. This is the only legal way
// to declare a write barrier function.
//
.macro WRITE_BARRIER_ENTRY name
    LEAF_ENTRY \name, _TEXT
.endm

// WRITE_BARRIER_END
//
// The partner to WRITE_BARRIER_ENTRY, used like NESTED_END.
//
.macro WRITE_BARRIER_END name
    LEAF_END_MARKED \name, _TEXT
.endm

.balign 64  // Align to power of two at least as big as patchable literal pool so that it fits optimally in cache line
//------------------------------------------
// Start of the writeable code region
LEAF_ENTRY JIT_PatchedCodeStart, _TEXT
    ret  lr
LEAF_END JIT_PatchedCodeStart, _TEXT

// void JIT_ByRefWriteBarrier
// On entry:
//   x13  : the source address (points to object reference to write)
//   x14  : the destination address (object reference written here)
//
// On exit:
//   x12  : trashed
//   x13  : incremented by 8
//   x14  : incremented by 8
//   x15  : trashed
//   x17  : trashed (ip1) if FEATURE_USE_SOFTWARE_WRITE_WATCH_FOR_GC_HEAP
//
//   NOTE: Keep in sync with RBM_CALLEE_TRASH_WRITEBARRIER_BYREF and RBM_CALLEE_GCTRASH_WRITEBARRIER_BYREF
//         if you add more trashed registers.
//
WRITE_BARRIER_ENTRY JIT_ByRefWriteBarrier

    ldr  x15, [x13], 8
    b C_FUNC(JIT_CheckedWriteBarrier)

WRITE_BARRIER_END JIT_ByRefWriteBarrier

#ifndef FEATURE_SATORI_GC

//-----------------------------------------------------------------------------
// Simple WriteBarriers
// void JIT_CheckedWriteBarrier(Object** dst, Object* src)
// On entry:
//   x14  : the destination address (LHS of the assignment)
//   x15  : the object reference (RHS of the assignment)
//
// On exit:
//   x12  : trashed
//   x14  : trashed (incremented by 8 to implement JIT_ByRefWriteBarrier contract)
//   x15  : trashed
//   x17  : trashed (ip1) if FEATURE_USE_SOFTWARE_WRITE_WATCH_FOR_GC_HEAP
//
WRITE_BARRIER_ENTRY JIT_CheckedWriteBarrier
    ldr  x12,  LOCAL_LABEL(wbs_lowest_address)
    cmp  x14,  x12

    ldr  x12, LOCAL_LABEL(wbs_highest_address)

    // Compare against the upper bound if the previous comparison indicated
    // that the destination address is greater than or equal to the lower
    // bound. Otherwise, set the C flag (specified by the 0x2) so that the
    // branch below is not taken.
    ccmp x14, x12, #0x2, hs

    bhs  LOCAL_LABEL(NotInHeap)

    b C_FUNC(JIT_WriteBarrier)

LOCAL_LABEL(NotInHeap):
    str  x15, [x14], 8
    ret  lr
WRITE_BARRIER_END JIT_CheckedWriteBarrier

// void JIT_WriteBarrier(Object** dst, Object* src)
// On entry:
//   x14  : the destination address (LHS of the assignment)
//   x15  : the object reference (RHS of the assignment)
//
// On exit:
//   x12  : trashed
//   x14  : trashed (incremented by 8 to implement JIT_ByRefWriteBarrier contract)
//   x15  : trashed
//   x17  : trashed (ip1) if FEATURE_USE_SOFTWARE_WRITE_WATCH_FOR_GC_HEAP
//
WRITE_BARRIER_ENTRY JIT_WriteBarrier
    stlr  x15, [x14]

#ifdef WRITE_BARRIER_CHECK
    // Update GC Shadow Heap

    // Do not perform the work if g_GCShadow is 0
    ldr  x12, LOCAL_LABEL(wbs_GCShadow)
    cbz  x12, LOCAL_LABEL(ShadowUpdateDisabled)

    // need temporary register. Save before using.
    str  x13, [sp, #-16]!

    // Compute address of shadow heap location:
    //   pShadow = g_GCShadow + (x14 - g_lowest_address)
    ldr  x13, LOCAL_LABEL(wbs_lowest_address)
    sub  x13, x14, x13
    add  x12, x13, x12

    // if (pShadow >= g_GCShadowEnd) goto end
    ldr  x13, LOCAL_LABEL(wbs_GCShadowEnd)
    cmp  x12, x13
    bhs  LOCAL_LABEL(ShadowUpdateEnd)

    // *pShadow = x15
    str  x15, [x12]

    // Ensure that the write to the shadow heap occurs before the read from the GC heap so that race
    // conditions are caught by INVALIDGCVALUE.
    dmb  ish

    // if ([x14] == x15) goto end
    ldr  x13, [x14]
    cmp  x13, x15
    beq LOCAL_LABEL(ShadowUpdateEnd)

    // *pShadow = INVALIDGCVALUE (0xcccccccd)
    movz x13, #0xcccd
    movk x13, #0xcccc, LSL #16
    str  x13, [x12]

LOCAL_LABEL(ShadowUpdateEnd):
    ldr  x13, [sp], #16
LOCAL_LABEL(ShadowUpdateDisabled):
#endif

#ifdef FEATURE_USE_SOFTWARE_WRITE_WATCH_FOR_GC_HEAP
    // Update the write watch table if necessary
    ldr  x12, LOCAL_LABEL(wbs_sw_ww_table)
    cbz  x12, LOCAL_LABEL(CheckCardTable)
    add  x12, x12, x14, lsr #0xc  // SoftwareWriteWatch::AddressToTableByteIndexShift
    ldrb w17, [x12]
    cbnz x17, LOCAL_LABEL(CheckCardTable)
    mov  w17, #0xFF
    strb w17, [x12]
#endif

LOCAL_LABEL(CheckCardTable):
    // Branch to Exit if the reference is not in the Gen0 heap
    //
    ldr  x12, LOCAL_LABEL(wbs_ephemeral_low)
    cbz  x12, LOCAL_LABEL(SkipEphemeralCheck)
    cmp  x15,  x12

    ldr  x12, LOCAL_LABEL(wbs_ephemeral_high)

    // Compare against the upper bound if the previous comparison indicated
    // that the destination address is greater than or equal to the lower
    // bound. Otherwise, set the C flag (specified by the 0x2) so that the
    // branch to exit is taken.
    ccmp x15, x12, #0x2, hs

    bhs  LOCAL_LABEL(Exit)

LOCAL_LABEL(SkipEphemeralCheck):
    // Check if we need to update the card table
    ldr  x12, LOCAL_LABEL(wbs_card_table)
    add  x15, x12, x14, lsr #11
    ldrb w12, [x15]
    cmp  x12, 0xFF
    beq  LOCAL_LABEL(Exit)

LOCAL_LABEL(UpdateCardTable):
    mov  x12, 0xFF
    strb w12, [x15]

#ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
    // Check if we need to update the card bundle table
    ldr  x12, LOCAL_LABEL(wbs_card_bundle_table)
    add  x15, x12, x14, lsr #21
    ldrb w12, [x15]
    cmp  x12, 0xFF
    beq  LOCAL_LABEL(Exit)

LOCAL_LABEL(UpdateCardBundle):
    mov  x12, 0xFF
    strb w12, [x15]
#endif

LOCAL_LABEL(Exit):
    add  x14, x14, 8
    ret  lr
WRITE_BARRIER_END JIT_WriteBarrier

#else  // FEATURE_SATORI_GC

//-----------------------------------------------------------------------------
// Simple WriteBarriers
// void JIT_CheckedWriteBarrier(Object** dst, Object* src)
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
//
WRITE_BARRIER_ENTRY JIT_CheckedWriteBarrier
    // See if dst is in GCHeap
        ldr     x16, LOCAL_LABEL(wbs_card_bundle_table)
        lsr     x17, x14, #30                       // dst page index
        ldrb    w12, [x16, x17]
        cbz     x12 , LOCAL_LABEL(NotInHeap)
        b       C_FUNC(CheckedEntry)

    LOCAL_LABEL(NotInHeap):
        str  x15, [x14], #8
        ret  lr
WRITE_BARRIER_END JIT_CheckedWriteBarrier

// void JIT_WriteBarrier(Object** dst, Object* src)
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
//
WRITE_BARRIER_ENTRY JIT_WriteBarrier
    // check for escaping assignment
    // 1) check if we own the source region
#ifdef FEATURE_SATORI_EXTERNAL_OBJECTS
        ldr     x16, LOCAL_LABEL(wbs_card_bundle_table)
    C_FUNC(CheckedEntry):
        lsr     x17, x15, #30                   // source page index
        ldrb    w12, [x16, x17]
        cbz     x12, LOCAL_LABEL(JustAssign)    // null or external (immutable) object
#else
    C_FUNC(CheckedEntry):
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
        str  x15, [x14], #8
        ret  lr

    LOCAL_LABEL(AssignAndMarkCards):
        stlr    x15, [x14]

    // TUNING: barriers in different modes could be separate pieces of code, but barrier switch 
    //         needs to suspend EE, not sure if skipping mode check would worth that much.
        ldr     x17, LOCAL_LABEL(wbs_sw_ww_table)
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
        ldr     x12, LOCAL_LABEL(wbs_card_table)     // fetch the page map
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
        ldr     x12, LOCAL_LABEL(wbs_sw_ww_table)    // !wbs_sw_ww_table -> !concurrent
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
WRITE_BARRIER_END JIT_WriteBarrier

#endif  // FEATURE_SATORI_GC

    // Begin patchable literal pool
    .balign 64  // Align to power of two at least as big as patchable literal pool so that it fits optimally in cache line
WRITE_BARRIER_ENTRY JIT_WriteBarrier_Table
LOCAL_LABEL(wbs_begin):
LOCAL_LABEL(wbs_card_table):
    .quad 0
LOCAL_LABEL(wbs_card_bundle_table):
    .quad 0
LOCAL_LABEL(wbs_sw_ww_table):
    .quad 0
LOCAL_LABEL(wbs_ephemeral_low):
    .quad 0
LOCAL_LABEL(wbs_ephemeral_high):
    .quad 0
LOCAL_LABEL(wbs_lowest_address):
    .quad 0
LOCAL_LABEL(wbs_highest_address):
    .quad 0
#ifdef WRITE_BARRIER_CHECK
LOCAL_LABEL(wbs_GCShadow):
    .quad 0
LOCAL_LABEL(wbs_GCShadowEnd):
    .quad 0
#endif
WRITE_BARRIER_END JIT_WriteBarrier_Table


// ------------------------------------------------------------------
// End of the writeable code region
LEAF_ENTRY JIT_PatchedCodeLast, _TEXT
    ret  lr
LEAF_END JIT_PatchedCodeLast, _TEXT
