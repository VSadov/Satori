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
;
; Regarding patchable constants:
; - 64-bit constants have to be loaded into a register
; - The constants have to be aligned to 8 bytes so that they can be patched easily
; - The alignment is by construction: there is only fixed-size code between an alignment directive and a constant

; Mark start of the code region that we patch at runtime
LEAF_ENTRY JIT_PatchedCodeStart, _TEXT
        ret
LEAF_END JIT_PatchedCodeStart, _TEXT

; void JIT_CheckedWriteBarrier(Object** dst, Object* src)
LEAF_ENTRY JIT_CheckedWriteBarrier, _TEXT
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
LEAF_END_MARKED JIT_CheckedWriteBarrier, _TEXT

ALTERNATE_ENTRY macro Name

Name label proc
PUBLIC Name
        endm

;
;   rcx - dest address 
;   rdx - object
;
LEAF_ENTRY JIT_WriteBarrier, _TEXT

ifdef FEATURE_SATORI_EXTERNAL_OBJECTS
    ; check if src is in heap
        NOP_6_BYTE ; padding for alignment of constant
PATCH_LABEL JIT_WriteBarrier_Patch_Label_CardBundleTable
        mov     rax, 0F0F0F0F0F0F0F0F0h    ; the page byte map
    ALTERNATE_ENTRY CheckedEntry
        mov     r8,  rdx
        shr     r8,  30                    ; src page index
        cmp     byte ptr [rax + r8], 0
        je      JustAssign                 ; src not in heap
else
    ALTERNATE_ENTRY CheckedEntry
endif

    ; check for escaping assignment
    ; 1) check if we own the source region
        mov     r8, rdx
        and     r8, 0FFFFFFFFFFE00000h  ; source region

ifndef FEATURE_SATORI_EXTERNAL_OBJECTS
        jz      JustAssign              ; assigning null
endif

        mov     rax,  gs:[30h]          ; thread tag, TEB on NT
        cmp     qword ptr [r8], rax     
        jne     AssignAndMarkCards      ; not local to this thread

    ; 2) check if the src and dst are from the same region
        mov     rax, rcx
        and     rax, 0FFFFFFFFFFE00000h ; target aligned to region
        cmp     rax, r8
        jne     RecordEscape            ; cross region assignment. definitely escaping

    ; 3) check if the target is exposed
        mov     rax, rcx
        and     rax, 01FFFFFh
        shr     rax, 3
        bt      qword ptr [r8], rax
        jb      RecordEscape            ; target is exposed. record an escape.

    JustAssign:
        mov     [rcx], rdx              ; src is external/null, or the assignment is completely threadlocal
        ret

    ; Not executed. Aligns the constant below: the padding, the 3-byte store and the 2-byte opcode
    ; end on an 8-byte boundary.
        align   8
        NOP_3_BYTE
    AssignAndMarkCards:
        mov     [rcx], rdx

    ; TUNING: barriers in different modes could be separate pieces of code, but barrier switch 
    ;         needs to suspend EE, not sure if skipping mode check would worth that much.
PATCH_LABEL JIT_WriteBarrier_Patch_Label_WriteWatchTable
        mov     r10, 0F0F0F0F0F0F0F0F0h ; the address of the barrier state (g_write_watch_table)
        mov     r11, qword ptr [r10]

    ; check the barrier state. this must be done after the assignment (in program order)
    ; if state == 2 we do not set or dirty cards.
        cmp     r11, 2h
        jne     DoCards
    Exit:
        ret

    DoCards:
    ; if src and dst are in the same region, cards are not needed, unless concurrent
        xor     rdx, rcx
        shr     rdx, 21
        jz      CheckConcurrent         ; same region, just check if barrier is not concurrent

    ; dst is in another region - cards are needed if src is ephemeral (gen < 2)
        cmp     dword ptr [r8 + 16], 2
        jl      MarkCards

    CheckConcurrent:
        cmp     r11, 0h
        je      Exit                    ; if not concurrent, exit

    ; Aligns the constant below: the padding and the 2-byte opcode end on an 8-byte boundary.
    ; It is ahead of the label, so only the fall-through from CheckConcurrent runs it.
        align   8
        NOP_6_BYTE
    MarkCards:
    ; fetch card location for rcx
PATCH_LABEL JIT_WriteBarrier_Patch_Label_CardTable
        mov     r9, 0F0F0F0F0F0F0F0F0h  ; the page map
        mov     r8,  rcx
        shr     rcx, 30
        mov     rax, qword ptr [r9 + rcx * 8] ; page
        sub     r8, rax   ; offset in page
        mov     rdx,r8
        shr     r8, 9     ; card offset
        shr     rdx, 20   ; group index
        lea     rdx, [rax + rdx * 2 + 80h] ; group offset

    ; check if concurrent marking is in progress
        cmp     r11, 0h
        jne     DirtyCard

    ; SETTING CARD FOR RCX
     SetCard:
        cmp     byte ptr [rax + r8], 0
        jne     Exit
        mov     byte ptr [rax + r8], 1
     SetGroup:
        cmp     byte ptr [rdx], 0
        jne     CardSet
        mov     byte ptr [rdx], 1
     SetPage:
        cmp     byte ptr [rax], 0
        jne     CardSet
        ; DIRTY on purpose: a racing REMEMBERED store could overwrite another thread's DIRTY.
        mov     byte ptr [rax], 4

     CardSet:
    ; check if concurrent marking is still not in progress
        cmp     qword ptr [r10], 0h     ; r10 - the address of the barrier state
        jne     DirtyCard
        ret

    ; DIRTYING CARD FOR RCX
     DirtyCard:
        mov     byte ptr [rax + r8], 4
     DirtyGroup:
        cmp     byte ptr [rdx], 4
        je      Exit
        mov     byte ptr [rdx], 4
     DirtyPage:
        cmp     byte ptr [rax], 4
        je      Exit
        mov     byte ptr [rax], 4
        ret

    ; this is expected to be rare.
    RecordEscape:

        ; 4) check if the source is escaped
        mov     rax, rdx
        add     rax, 8                        ; escape bit is MT + 1
        and     rax, 01FFFFFh
        shr     rax, 3
        bt      qword ptr [r8], rax
        jb      AssignAndMarkCards            ; source is already escaped.

        ; Align rsp
        mov  r9, rsp
        and  rsp, -16

        ; save rsp, rcx, rdx, r8
        push r9
        push rcx
        push rdx
        push r8

        ; also save xmm0, in case it is used for stack clearing.
        ; Hopefully EscapeFn cannot corrupt other xmm regs, since there is no float math or vectorizable code in there.
        sub     rsp, 16
        movdqa  [rsp], xmm0

        ; shadow space
        sub  rsp, 20h

        ; void SatoriRegion::EscapeFn(SatoriObject** dst, SatoriObject* src, SatoriRegion* region)
        call    qword ptr [r8 + 8]

        add     rsp, 20h

        movdqa  xmm0, [rsp]
        add     rsp, 16

        pop     r8
        pop     rdx
        pop     rcx
        pop     rsp
        jmp     AssignAndMarkCards
LEAF_END_MARKED JIT_WriteBarrier, _TEXT

; Mark start of the code region that we patch at runtime
LEAF_ENTRY JIT_PatchedCodeLast, _TEXT
        ret
LEAF_END JIT_PatchedCodeLast, _TEXT

endif  ; FEATURE_SATORI_GC

        end
