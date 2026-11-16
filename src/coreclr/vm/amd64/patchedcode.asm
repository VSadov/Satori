; Licensed to the .NET Foundation under one or more agreements.
; The .NET Foundation licenses this file to you under the MIT license.

; ***********************************************************************
; File: patchedcode.asm
;
; Notes: routinues which are patched at runtime and need to be linked in
;        their declared order.
; ***********************************************************************


include AsmMacros.inc
include asmconstants.inc

ifdef _DEBUG
extern JIT_WriteBarrier_Debug:proc
endif

ifndef FEATURE_SATORI_GC

; Mark start of the code region that we patch at runtime
LEAF_ENTRY JIT_PatchedCodeStart, _TEXT
        ret
LEAF_END JIT_PatchedCodeStart, _TEXT


; This is used by the mechanism to hold either the JIT_WriteBarrier_PreGrow
; or JIT_WriteBarrier_PostGrow code (depending on the state of the GC). It _WILL_
; change at runtime as the GC changes. Initially it should simply be a copy of the
; larger of the two functions (JIT_WriteBarrier_PostGrow) to ensure we have created
; enough space to copy that code in.
LEAF_ENTRY JIT_WriteBarrier, _TEXT
        align 16

ifdef _DEBUG
        ; In debug builds, this just contains jump to the debug version of the write barrier by default
        mov     rax, JIT_WriteBarrier_Debug
        jmp     rax
endif

ifdef FEATURE_USE_SOFTWARE_WRITE_WATCH_FOR_GC_HEAP
        ; JIT_WriteBarrier_WriteWatch_PostGrow64

        ; Regarding patchable constants:
        ; - 64-bit constants have to be loaded into a register
        ; - The constants have to be aligned to 8 bytes so that they can be patched easily
        ; - The constant loads have been located to minimize NOP padding required to align the constants
        ; - Using different registers for successive constant loads helps pipeline better. Should we decide to use a special
        ;   non-volatile calling convention, this should be changed to use just one register.

        ; Do the move into the GC .  It is correct to take an AV here, the EH code
        ; figures out that this came from a WriteBarrier and correctly maps it back
        ; to the managed method which called the WriteBarrier (see setup in
        ; InitializeExceptionHandling, vm\exceptionhandling.cpp).
        mov     [rcx], rdx

        ; Update the write watch table if necessary
        mov     rax, rcx
        mov     r8, 0F0F0F0F0F0F0F0F0h
        shr     rax, 0Ch ; SoftwareWriteWatch::AddressToTableByteIndexShift
        NOP_2_BYTE ; padding for alignment of constant
        mov     r9, 0F0F0F0F0F0F0F0F0h
        add     rax, r8
        cmp     byte ptr [rax], 0h
        jne     CheckCardTable
        mov     byte ptr [rax], 0FFh

        NOP_3_BYTE ; padding for alignment of constant

        ; Check the lower and upper ephemeral region bounds
    CheckCardTable:
        cmp     rdx, r9
        jb      Exit

        NOP_3_BYTE ; padding for alignment of constant

        mov     r8, 0F0F0F0F0F0F0F0F0h

        cmp     rdx, r8
        jae     Exit

        nop ; padding for alignment of constant

        mov     rax, 0F0F0F0F0F0F0F0F0h

        ; Touch the card table entry, if not already dirty.
        shr     rcx, 0Bh
        cmp     byte ptr [rcx + rax], 0FFh
        jne     UpdateCardTable
        REPRET

    UpdateCardTable:
        mov     byte ptr [rcx + rax], 0FFh
ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
        mov     rax, 0F0F0F0F0F0F0F0F0h
        shr     rcx, 0Ah
        cmp     byte ptr [rcx + rax], 0FFh
        jne     UpdateCardBundleTable
        REPRET

    UpdateCardBundleTable:
        mov     byte ptr [rcx + rax], 0FFh
endif
        ret

    align 16
    Exit:
        REPRET

        NOP_3_BYTE
        NOP_3_BYTE
        NOP_3_BYTE
        NOP_3_BYTE
        NOP_3_BYTE

        NOP_3_BYTE
        NOP_3_BYTE
        NOP_3_BYTE
        NOP_3_BYTE
        NOP_3_BYTE

        NOP_3_BYTE
        NOP_3_BYTE
        NOP_3_BYTE
        NOP_3_BYTE
        NOP_3_BYTE

        NOP_3_BYTE
        NOP_3_BYTE
        NOP_3_BYTE
        NOP_3_BYTE
        NOP_3_BYTE

else
        ; JIT_WriteBarrier_PostGrow64

        ; Do the move into the GC .  It is correct to take an AV here, the EH code
        ; figures out that this came from a WriteBarrier and correctly maps it back
        ; to the managed method which called the WriteBarrier (see setup in
        ; InitializeExceptionHandling, vm\exceptionhandling.cpp).
        mov     [rcx], rdx

        NOP_3_BYTE ; padding for alignment of constant

        ; Can't compare a 64 bit immediate, so we have to move them into a
        ; register.  Values of these immediates will be patched at runtime.
        ; By using two registers we can pipeline better.  Should we decide to use
        ; a special non-volatile calling convention, this should be changed to
        ; just one.

        mov     rax, 0F0F0F0F0F0F0F0F0h

        ; Check the lower and upper ephemeral region bounds
        cmp     rdx, rax
        jb      Exit

        nop ; padding for alignment of constant

        mov     r8, 0F0F0F0F0F0F0F0F0h

        cmp     rdx, r8
        jae     Exit

        nop ; padding for alignment of constant

        mov     rax, 0F0F0F0F0F0F0F0F0h

        ; Touch the card table entry, if not already dirty.
        shr     rcx, 0Bh
        cmp     byte ptr [rcx + rax], 0FFh
        jne     UpdateCardTable
        REPRET

    UpdateCardTable:
        mov     byte ptr [rcx + rax], 0FFh
ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
        mov     rax, 0F0F0F0F0F0F0F0F0h
        shr     rcx, 0Ah
        cmp     byte ptr [rcx + rax], 0FFh
        jne     UpdateCardBundleTable
        REPRET

    UpdateCardBundleTable:
        mov     byte ptr [rcx + rax], 0FFh
endif
        ret

    align 16
    Exit:
        REPRET
endif

    ; make sure this is bigger than any of the others
    align 16
        nop
LEAF_END_MARKED JIT_WriteBarrier, _TEXT

; Mark start of the code region that we patch at runtime
LEAF_ENTRY JIT_PatchedCodeLast, _TEXT
        ret
LEAF_END JIT_PatchedCodeLast, _TEXT

else  ;FEATURE_SATORI_GC     ##########################################################################

; The Satori barriers run from the copy of the patched code region (see GetWriteBarrierCodeLocation), so they
; cannot refer to globals RIP-relatively. Instead they embed the values that do not change after the GC
; initializes the write barrier - the page byte map (g_card_bundle_table), the page map (g_card_table) and the
; address of g_write_watch_table, which holds the barrier state - as constants that WriteBarrierManager patches.
; JIT_WriteBarrier is a buffer for one of the barriers in JitHelpers_FastWriteBarriers.asm.
;
; Regarding patchable constants:
; - 64-bit constants have to be loaded into a register
; - The constants have to be aligned to 8 bytes so that they can be patched easily
; - The alignment is by construction: there is only fixed-size code between an alignment directive and a constant
;
; The region and the barriers in it start on cache line boundaries, so that the barriers are cache line aligned
; also where the region is copied, which keeps their offsets from the region start.

; Mark start of the code region that we patch at runtime
LEAF_ENTRY_ALIGN64 JIT_PatchedCodeStart
        ret
LEAF_END_ALIGN64 JIT_PatchedCodeStart

; void JIT_CheckedWriteBarrier(Object** dst, Object* src)
LEAF_ENTRY_ALIGN64 JIT_CheckedWriteBarrier
    ; See if dst is in GCHeap
        NOP_6_BYTE ; padding for alignment of constant
PATCH_LABEL JIT_CheckedWriteBarrier_Patch_Label_CardBundleTable
        mov     rax, 0F0F0F0F0F0F0F0F0h    ; the page byte map
        mov     r8,  rcx
        shr     r8,  30                    ; dst page index
        cmp     byte ptr [rax + r8], 0
        jne     CheckedEntry

    NotInHeap:
        ; See comment above about possible AV
        mov     [rcx], rdx
        ret
LEAF_END_MARKED_ALIGN64 JIT_CheckedWriteBarrier

ALTERNATE_ENTRY macro Name

Name label proc
PUBLIC Name
        endm

; void JIT_WriteBarrier(Object** dst, Object* src)
;
; Space for one of the Satori barriers (see JIT_WriteBarrier_SATORI in JitHelpers_FastWriteBarriers.asm), which
; WriteBarrierManager copies here when the GC initializes the write barrier and when the GC switches barriers.
; The barriers have the same code ahead of CheckedEntry, so JIT_CheckedWriteBarrier can jump there in any of them.
;
;   rcx - dest address
;   rdx - object
;
LEAF_ENTRY_ALIGN64 JIT_WriteBarrier

ifdef FEATURE_SATORI_EXTERNAL_OBJECTS
        NOP_6_BYTE
        mov     rax, 0F0F0F0F0F0F0F0F0h
endif
    ALTERNATE_ENTRY CheckedEntry

        ; make sure this is bigger than any of the barriers
        db      1F0h dup (0CCh)
LEAF_END_MARKED_ALIGN64 JIT_WriteBarrier

; Mark start of the code region that we patch at runtime
LEAF_ENTRY_ALIGN64 JIT_PatchedCodeLast
        ret
LEAF_END_ALIGN64 JIT_PatchedCodeLast

endif  ; FEATURE_SATORI_GC

        end
