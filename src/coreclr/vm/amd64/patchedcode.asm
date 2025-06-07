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

EXTERN g_card_table:QWORD
EXTERN g_card_bundle_table:QWORD
EXTERN g_sw_ww_table:QWORD

ifdef FEATURE_USE_SOFTWARE_WRITE_WATCH_FOR_GC_HEAP
EXTERN  g_sw_ww_enabled_for_gc_heap:BYTE
endif

; Mark start of the code region that we patch at runtime
LEAF_ENTRY JIT_PatchedCodeStart, _TEXT
        ret
LEAF_END JIT_PatchedCodeStart, _TEXT

; void JIT_CheckedWriteBarrier(Object** dst, Object* src)
LEAF_ENTRY JIT_CheckedWriteBarrier, _TEXT
    ; See if dst is in GCHeap
        mov     rax, [g_card_bundle_table] ; fetch the page byte map
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
        mov     rax, [g_card_bundle_table] ; fetch the page byte map
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
        mov     [rcx], rdx              ; no card marking, src is not a heap object
        ret

    AssignAndMarkCards:
        mov     [rcx], rdx

    ; TUNING: barriers in different modes could be separate pieces of code, but barrier switch 
    ;         needs to suspend EE, not sure if skipping mode check would worth that much.
        mov     r11, qword ptr [g_sw_ww_table]

    ; check the barrier state. this must be done after the assignment (in program order)
    ; if state == 2 we do not set or dirty cards.
        cmp     r11, 2h
        jne     DoCards
    Exit:
        ret

    DoCards:
    ; if same region, just check if barrier is not concurrent
        xor     rdx, rcx
        shr     rdx, 21
        jz      CheckConcurrent

    ; if src is in gen2/3 and the barrier is not concurrent we do not need to mark cards
        cmp     dword ptr [r8 + 16], 2
        jl      MarkCards

    CheckConcurrent:
        cmp     r11, 0h
        je      Exit

    MarkCards:
    ; fetch card location for rcx
        mov     r9 , [g_card_table]     ; fetch the page map
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
        mov     byte ptr [rax], 1

     CardSet:
    ; check if concurrent marking is still not in progress
        cmp     qword ptr [g_sw_ww_table], 0h
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

        ; also save xmm0, in case it is used for stack clearing, as JIT_ByRefWriteBarrier should not trash xmm0
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

; JIT_ByRefWriteBarrier has weird symantics, see usage in StubLinkerX86.cpp
;
; Entry:
;   RDI - address of ref-field (assigned to)
;   RSI - address of the data  (source)
; Exit:
;   RCX is trashed
;   RAX is trashed
;   RDI, RSI are incremented by SIZEOF(LPVOID)
LEAF_ENTRY JIT_ByRefWriteBarrier, _TEXT
    ; See if dst is in GCHeap
        mov     rax, [g_card_bundle_table]  ; fetch the page byte map
        mov     rcx,  rdi
        shr     rcx,  30                    ; dst page index
        cmp     byte ptr [rax + rcx], 0
        jne     InHeap

        mov     rcx, [rsi]
        mov     [rdi], rcx
        add     rdi, 8h
        add     rsi, 8h
        ret

    InHeap:

        ; JIT_WriteBarrier may trash these registers 
        push    rdx
        push    r8
        push    r9
        push    r10
        push    r11

        mov     rcx, rdi
        mov     rdx, [rsi]
        add     rdi, 8h
        add     rsi, 8h

        call    CheckedEntry

        pop     r11
        pop     r10
        pop     r9
        pop     r8
        pop     rdx
        ret
LEAF_END_MARKED JIT_ByRefWriteBarrier, _TEXT

; Mark start of the code region that we patch at runtime
LEAF_ENTRY JIT_PatchedCodeLast, _TEXT
        ret
LEAF_END JIT_PatchedCodeLast, _TEXT

endif  ; FEATURE_SATORI_GC

        end
