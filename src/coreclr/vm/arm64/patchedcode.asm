; Licensed to the .NET Foundation under one or more agreements.
; The .NET Foundation licenses this file to you under the MIT license.

#include "ksarm64.h"
#include "asmconstants.h"
#include "asmmacros.h"

    ;;like TEXTAREA, but with 64 byte alignment so that we can align the patchable pool below to 64 without warning
    AREA    |.text|,ALIGN=6,CODE,READONLY

;-----------------------------------------------------------------------------
; The following Macros help in WRITE_BARRIER Implementations
    ; WRITE_BARRIER_ENTRY
    ;
    ; Declare the start of a write barrier function. Use similarly to NESTED_ENTRY. This is the only legal way
    ; to declare a write barrier function.
    ;
    MACRO
      WRITE_BARRIER_ENTRY $name

      LEAF_ENTRY $name
    MEND

    ; WRITE_BARRIER_END
    ;
    ; The partner to WRITE_BARRIER_ENTRY, used like NESTED_END.
    ;
    MACRO
      WRITE_BARRIER_END $__write_barrier_name

      LEAF_END_MARKED $__write_barrier_name

    MEND

; ------------------------------------------------------------------
; Start of the writeable code region
    LEAF_ENTRY JIT_PatchedCodeStart
        ret      lr
    LEAF_END

        ; Begin patchable literal pool
        ALIGN 64  ; Align to power of two at least as big as patchable literal pool so that it fits optimally in cache line
    WRITE_BARRIER_ENTRY JIT_WriteBarrier_Table
wbs_begin
wbs_card_table
        DCQ 0
wbs_card_bundle_table
        DCQ 0
wbs_sw_ww_table
        DCQ 0
wbs_ephemeral_low
        DCQ 0
wbs_ephemeral_high
        DCQ 0
wbs_lowest_address
        DCQ 0
wbs_highest_address
        DCQ 0
#ifdef WRITE_BARRIER_CHECK
wbs_GCShadow
        DCQ 0
wbs_GCShadowEnd
        DCQ 0
#endif
    WRITE_BARRIER_END JIT_WriteBarrier_Table

; void JIT_ByRefWriteBarrier
; On entry:
;   x13  : the source address (points to object reference to write)
;   x14  : the destination address (object reference written here)
;
; On exit:
;   x12  : trashed
;   x13  : incremented by 8
;   x14  : incremented by 8
;   x15  : trashed
;   x17  : trashed (ip1) if FEATURE_USE_SOFTWARE_WRITE_WATCH_FOR_GC_HEAP
;
;   NOTE: Keep in sync with RBM_CALLEE_TRASH_WRITEBARRIER_BYREF and RBM_CALLEE_GCTRASH_WRITEBARRIER_BYREF
;         if you add more trashed registers.
;
    WRITE_BARRIER_ENTRY JIT_ByRefWriteBarrier

        ldr      x15, [x13], 8
        b        JIT_CheckedWriteBarrier

    WRITE_BARRIER_END JIT_ByRefWriteBarrier

#ifndef FEATURE_SATORI_GC

;-----------------------------------------------------------------------------
; Simple WriteBarriers
; void JIT_CheckedWriteBarrier(Object** dst, Object* src)
; On entry:
;   x14  : the destination address (LHS of the assignment)
;   x15  : the object reference (RHS of the assignment)
;
; On exit:
;   x12  : trashed
;   x14  : incremented by 8
;   x15  : trashed
;   x17  : trashed (ip1) if FEATURE_USE_SOFTWARE_WRITE_WATCH_FOR_GC_HEAP
;
    WRITE_BARRIER_ENTRY JIT_CheckedWriteBarrier
        ldr      x12,  wbs_lowest_address
        cmp      x14,  x12

        ldr      x12,  wbs_highest_address
        ccmphs   x14,  x12, #0x2
        blo      JIT_WriteBarrier

NotInHeap
        str      x15, [x14], 8
        ret      lr
    WRITE_BARRIER_END JIT_CheckedWriteBarrier

; void JIT_WriteBarrier(Object** dst, Object* src)
; On entry:
;   x14  : the destination address (LHS of the assignment)
;   x15  : the object reference (RHS of the assignment)
;
; On exit:
;   x12  : trashed
;   x14  : incremented by 8
;   x15  : trashed
;   x17  : trashed (ip1) if FEATURE_USE_SOFTWARE_WRITE_WATCH_FOR_GC_HEAP
;
    WRITE_BARRIER_ENTRY JIT_WriteBarrier
        stlr     x15, [x14]

#ifdef WRITE_BARRIER_CHECK
        ; Update GC Shadow Heap

        ; Do not perform the work if g_GCShadow is 0
        ldr      x12, wbs_GCShadow
        cbz      x12, ShadowUpdateDisabled

        ; need temporary register. Save before using.
        str      x13, [sp, #-16]!

        ; Compute address of shadow heap location:
        ;   pShadow = $g_GCShadow + (x14 - g_lowest_address)
        ldr      x13, wbs_lowest_address
        sub      x13, x14, x13
        add      x12, x13, x12

        ; if (pShadow >= $g_GCShadowEnd) goto end
        ldr      x13, wbs_GCShadowEnd
        cmp      x12, x13
        bhs      ShadowUpdateEnd

        ; *pShadow = x15
        str      x15, [x12]

        ; Ensure that the write to the shadow heap occurs before the read from the GC heap so that race
        ; conditions are caught by INVALIDGCVALUE.
        dmb      ish

        ; if ([x14] == x15) goto end
        ldr      x13, [x14]
        cmp      x13, x15
        beq ShadowUpdateEnd

        ; *pShadow = INVALIDGCVALUE (0xcccccccd)
        movz     x13, #0xcccd
        movk     x13, #0xcccc, LSL #16
        str      x13, [x12]

ShadowUpdateEnd
        ldr      x13, [sp], #16
ShadowUpdateDisabled
#endif

#ifdef FEATURE_USE_SOFTWARE_WRITE_WATCH_FOR_GC_HEAP
        ; Update the write watch table if necessary
        ldr      x12,  wbs_sw_ww_table
        cbz      x12,  CheckCardTable
        add      x12,  x12, x14, LSR #0xC  // SoftwareWriteWatch::AddressToTableByteIndexShift
        ldrb     w17,  [x12]
        cbnz     x17,  CheckCardTable
        mov      w17,  0xFF
        strb     w17,  [x12]
#endif

CheckCardTable
        ; Branch to Exit if the reference is not in the Gen0 heap
        ;
        ldr      x12,  wbs_ephemeral_low
        cbz      x12,  SkipEphemeralCheck
        cmp      x15,  x12

        ldr      x12,  wbs_ephemeral_high

        ; Compare against the upper bound if the previous comparison indicated
        ; that the destination address is greater than or equal to the lower
        ; bound. Otherwise, set the C flag (specified by the 0x2) so that the
        ; branch to exit is taken.
        ccmp     x15,  x12, #0x2, hs

        bhs      Exit

SkipEphemeralCheck
        ; Check if we need to update the card table
        ldr      x12, wbs_card_table

        ; x15 := pointer into card table
        add      x15, x12, x14, lsr #11

        ldrb     w12, [x15]
        cmp      x12, 0xFF
        beq      Exit

UpdateCardTable
        mov      x12, 0xFF
        strb     w12, [x15]

#ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
        ; Check if we need to update the card bundle table
        ldr      x12, wbs_card_bundle_table

        ; x15 := pointer into card bundle table
        add      x15, x12, x14, lsr #21

        ldrb     w12, [x15]
        cmp      x12, 0xFF
        beq      Exit

        mov      x12, 0xFF
        strb     w12, [x15]
#endif

Exit
        add      x14, x14, 8
        ret      lr
    WRITE_BARRIER_END JIT_WriteBarrier

#else  // FEATURE_SATORI_GC

;-----------------------------------------------------------------------------
; Simple WriteBarriers
; void JIT_CheckedWriteBarrier(Object** dst, Object* src)
; On entry:
;   x14  : the destination address (LHS of the assignment)
;   x15  : the object reference (RHS of the assignment)
;
; On exit:
;   x12  : trashed
;   x14  : trashed (incremented by 8 to implement JIT_ByRefWriteBarrier contract)
;   x15  : trashed
;   x17  : trashed (ip1) if FEATURE_USE_SOFTWARE_WRITE_WATCH_FOR_GC_HEAP
;
    WRITE_BARRIER_ENTRY JIT_CheckedWriteBarrier
    ; See if dst is in GCHeap
        ldr     x16, wbs_card_bundle_table
        lsr     x17, x14, #30                       ; source page index
        ldrb    w12, [x16, x17]
        cbnz    x12, CheckedEntry

NotInHeap
        str  x15, [x14], #8
        ret  lr
    WRITE_BARRIER_END JIT_CheckedWriteBarrier

; void JIT_WriteBarrier(Object** dst, Object* src)
; On entry:
;   x14  : the destination address (LHS of the assignment)
;   x15  : the object reference (RHS of the assignment)
;
; On exit:
;   x12  : trashed
;   x14  : trashed (incremented by 8 to implement JIT_ByRefWriteBarrier contract)
;   x15  : trashed
;   x16  : trashed (ip0)
;   x17  : trashed (ip1)
;
    WRITE_BARRIER_ENTRY JIT_WriteBarrier
    ; check for escaping assignment
    ; 1) check if we own the source region
#ifdef FEATURE_SATORI_EXTERNAL_OBJECTS
        ldr     x16, wbs_card_bundle_table
CheckedEntry
        lsr     x17, x15, #30                   ; source page index
        ldrb    w12, [x16, x17]
        cbz     x12, JustAssign                 ; null or external (immutable) object
#else
CheckedEntry
        cbz     x15, JustAssign                 ; assigning null
#endif
        and     x16,  x15, #0xFFFFFFFFFFE00000  ; source region
        ldr     x12, [x16]                      ; region tag
        cmp     x12, x18                        ; x18 - TEB
        bne     AssignAndMarkCards              ; not local to this thread

    ; 2) check if the src and dst are from the same region
        and     x12, x14, #0xFFFFFFFFFFE00000   ; target aligned to region
        cmp     x12, x16
        bne     RecordEscape                    ; cross region assignment. definitely escaping

    ; 3) check if the target is exposed
        ubfx        x17, x14,#9,#12             ; word index = (dst >> 9) & 0x1FFFFF
        ldr         x17, [x16, x17, lsl #3]     ; mark word = [region + index * 8]
        lsr         x12, x14, #3                ; bit = (dst >> 3) [& 63]
        lsr         x17, x17, x12
        tbnz        x17, #0, RecordEscape       ; target is exposed. record an escape.

    ; UNORDERED! assignment of unescaped, null or external (immutable) object
JustAssign
        str  x15, [x14], #8
        ret  lr

AssignAndMarkCards
        stlr    x15, [x14]

    ; TUNING: barriers in different modes could be separate pieces of code, but barrier switch 
    ;         needs to suspend EE, not sure if skipping mode check would worth that much.
        ldr     x17, wbs_sw_ww_table
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
        ldr     x12, wbs_card_table                  ; fetch the page map
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
        ldr     x12, wbs_sw_ww_table            ; !wbs_sw_ww_table -> !concurrent
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

    ; this is expected to be rare.
RecordEscape

    ; 4) check if the source is escaped (x16 has source region)
        add         x12, x15, #8                   ; escape bit is MT + 1
        ubfx        x17, x12, #9,#12               ; word index = (dst >> 9) & 0x1FFFFF
        ldr         x17, [x16, x17, lsl #3]        ; mark word = [region + index * 8]
        lsr         x12, x12, #3                   ; bit = (dst >> 3) [& 63]
        lsr         x17, x17, x12
        tbnz        x17, #0, AssignAndMarkCards    ; source is already escaped.

        ; because of the barrier call convention
        ; we need to preserve caller-saved x0 through x15 and x29/x30

        stp     x29,x30, [sp, -16 * 9]!
        stp     x0, x1,  [sp, 16 * 1]
        stp     x2, x3,  [sp, 16 * 2]
        stp     x4, x5,  [sp, 16 * 3]
        stp     x6, x7,  [sp, 16 * 4]
        stp     x8, x9,  [sp, 16 * 5]
        stp     x10,x11, [sp, 16 * 6]
        stp     x12,x13, [sp, 16 * 7]
        stp     x14,x15, [sp, 16 * 8]

        ; void SatoriRegion::EscapeFn(SatoriObject** dst, SatoriObject* src, SatoriRegion* region)
        ; mov  x0, x14  EscapeFn does not use dst, it is just to avoid arg shuffle on x64
        mov  x1, x15
        mov  x2, x16                        ; source region
        ldr  x12, [x16, #8]                 ; EscapeFn address
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

        and     x16, x15, #0xFFFFFFFFFFE00000  ; source region
        b       AssignAndMarkCards
    WRITE_BARRIER_END JIT_WriteBarrier

#endif  // FEATURE_SATORI_GC


; ------------------------------------------------------------------
; End of the writeable code region
    LEAF_ENTRY JIT_PatchedCodeLast
        ret      lr
    LEAF_END

; Must be at very end of file
    END
