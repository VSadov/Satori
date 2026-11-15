; Licensed to the .NET Foundation under one or more agreements.
; The .NET Foundation licenses this file to you under the MIT license.

; ***********************************************************************
; File: JitHelpers_FastWriteBarriers.asm
;
; Notes: these are the fast write barriers which are copied in to the
;        JIT_WriteBarrier buffer (found in JitHelpers_Fast.asm).
;        This code should never be executed at runtime and should end
;        up effectively being treated as data.
; ***********************************************************************

include AsmMacros.inc
include asmconstants.inc


; Two super fast helpers that together do the work of JIT_WriteBarrier.  These
; use inlined ephemeral region bounds and an inlined pointer to the card table.
;
; Until the GC does some major reshuffling, the ephemeral region will always be
; at the top of the heap, so given that we know the reference is inside the
; heap, we don't have to check against the upper bound of the ephemeral region
; (PreGrow version).  Once the GC moves the ephemeral region, this will no longer
; be valid, so we use the PostGrow version to check both the upper and lower
; bounds. The inlined bounds and card table pointers have to be patched
; whenever they change.
;
; At anyone time, the memory pointed to by JIT_WriteBarrier will contain one
; of these functions.  See StompWriteBarrierResize, StompWriteBarrierEphemeral
; and InitJITWriteBarrierHelpers in VM\AMD64\JITInterfaceAMD64.cpp for more info.
;
; READ THIS!!!!!!
; it is imperative that the addresses of the values that we overwrite
; (card table, ephemeral region ranges, etc) are naturally aligned since
; there are codepaths that will overwrite these values while the EE is running.
;
LEAF_ENTRY JIT_WriteBarrier_PreGrow64, _TEXT
        align 8
        ; Do the move into the GC .  It is correct to take an AV here, the EH code
        ; figures out that this came from a WriteBarrier and correctly maps it back
        ; to the managed method which called the WriteBarrier (see setup in
        ; InitializeExceptionHandling, vm\exceptionhandling.cpp).
        mov     [rcx], rdx

        NOP_3_BYTE ; padding for alignment of constant

        ; Can't compare a 64 bit immediate, so we have to move it into a
        ; register.  Value of this immediate will be patched at runtime.
PATCH_LABEL JIT_WriteBarrier_PreGrow64_Patch_Label_Lower
        mov     rax, 0F0F0F0F0F0F0F0F0h

        ; Check the lower ephemeral region bound.
        cmp     rdx, rax
        jb      Exit

        nop ; padding for alignment of constant

PATCH_LABEL JIT_WriteBarrier_PreGrow64_Patch_Label_CardTable
        mov     rax, 0F0F0F0F0F0F0F0F0h

        ; Touch the card table entry, if not already dirty.
        shr     rcx, 0Bh
        cmp     byte ptr [rcx + rax], 0FFh
        jne     UpdateCardTable
        REPRET

    UpdateCardTable:
        mov     byte ptr [rcx + rax], 0FFh
ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
        shr     rcx, 0Ah
        NOP_2_BYTE ; padding for alignment of constant
PATCH_LABEL JIT_WriteBarrier_PreGrow64_Patch_Label_CardBundleTable
        mov     rax, 0F0F0F0F0F0F0F0F0h
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
LEAF_END_MARKED JIT_WriteBarrier_PreGrow64, _TEXT


; See comments for JIT_WriteBarrier_PreGrow (above).
LEAF_ENTRY JIT_WriteBarrier_PostGrow64, _TEXT
        align 8
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
PATCH_LABEL JIT_WriteBarrier_PostGrow64_Patch_Label_Lower
        mov     rax, 0F0F0F0F0F0F0F0F0h

        ; Check the lower and upper ephemeral region bounds
        cmp     rdx, rax
        jb      Exit

        nop ; padding for alignment of constant

PATCH_LABEL JIT_WriteBarrier_PostGrow64_Patch_Label_Upper
        mov     r8, 0F0F0F0F0F0F0F0F0h

        cmp     rdx, r8
        jae     Exit

        nop ; padding for alignment of constant

PATCH_LABEL JIT_WriteBarrier_PostGrow64_Patch_Label_CardTable
        mov     rax, 0F0F0F0F0F0F0F0F0h

        ; Touch the card table entry, if not already dirty.
        shr     rcx, 0Bh
        cmp     byte ptr [rcx + rax], 0FFh
        jne     UpdateCardTable
        REPRET

    UpdateCardTable:
        mov     byte ptr [rcx + rax], 0FFh
ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
        shr     rcx, 0Ah
        NOP_2_BYTE ; padding for alignment of constant
PATCH_LABEL JIT_WriteBarrier_PostGrow64_Patch_Label_CardBundleTable
        mov     rax, 0F0F0F0F0F0F0F0F0h
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
LEAF_END_MARKED JIT_WriteBarrier_PostGrow64, _TEXT


ifdef FEATURE_SVR_GC

LEAF_ENTRY JIT_WriteBarrier_SVR64, _TEXT
        align 8
        ;
        ; SVR GC has multiple heaps, so it cannot provide one single
        ; ephemeral region to bounds check against, so we just skip the
        ; bounds checking all together and do our card table update
        ; unconditionally.
        ;

        ; Do the move into the GC .  It is correct to take an AV here, the EH code
        ; figures out that this came from a WriteBarrier and correctly maps it back
        ; to the managed method which called the WriteBarrier (see setup in
        ; InitializeExceptionHandling, vm\exceptionhandling.cpp).
        mov     [rcx], rdx

        NOP_3_BYTE ; padding for alignment of constant

PATCH_LABEL JIT_WriteBarrier_SVR64_PatchLabel_CardTable
        mov     rax, 0F0F0F0F0F0F0F0F0h

        shr     rcx, 0Bh

        cmp     byte ptr [rcx + rax], 0FFh
        jne     UpdateCardTable
        REPRET

    UpdateCardTable:
        mov     byte ptr [rcx + rax], 0FFh
ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
        shr     rcx, 0Ah
        NOP_2_BYTE ; padding for alignment of constant
PATCH_LABEL JIT_WriteBarrier_SVR64_PatchLabel_CardBundleTable
        mov     rax, 0F0F0F0F0F0F0F0F0h
        cmp     byte ptr [rcx + rax], 0FFh
        jne     UpdateCardBundleTable
        REPRET

    UpdateCardBundleTable:
        mov     byte ptr [rcx + rax], 0FFh
endif
        ret
LEAF_END_MARKED JIT_WriteBarrier_SVR64, _TEXT

LEAF_ENTRY JIT_WriteBarrier_Byte_Region64, _TEXT
        align 8

        ; Do the move into the GC .  It is correct to take an AV here, the EH code
        ; figures out that this came from a WriteBarrier and correctly maps it back
        ; to the managed method which called the WriteBarrier (see setup in
        ; InitializeExceptionHandling, vm\exceptionhandling.cpp).
        mov     [rcx], rdx

        mov     r8, rcx

PATCH_LABEL JIT_WriteBarrier_Byte_Region64_Patch_Label_RegionToGeneration
        mov     rax, 0F0F0F0F0F0F0F0F0h

PATCH_LABEL JIT_WriteBarrier_Byte_Region64_Patch_Label_RegionShrDest
        shr     rcx, 16h ; compute region index

        ; Check whether the region we're storing into is gen 0 - nothing to do in this case
        cmp     byte ptr [rcx + rax], 0
        jne     NotGen0
        REPRET

        NOP_2_BYTE ; padding for alignment of constant

    NotGen0:
PATCH_LABEL JIT_WriteBarrier_Byte_Region64_Patch_Label_Lower
        mov     r9, 0F0F0F0F0F0F0F0F0h
        cmp     rdx, r9
        jae     NotLow
        ret
    NotLow:
PATCH_LABEL JIT_WriteBarrier_Byte_Region64_Patch_Label_Upper
        mov     r9, 0F0F0F0F0F0F0F0F0h
        cmp     rdx, r9
        jb      NotHigh
        REPRET
    NotHigh:
PATCH_LABEL JIT_WriteBarrier_Byte_Region64_Patch_Label_RegionShrSrc
        shr     rdx, 16h ; compute region index
        mov     dl, [rdx + rax]
        cmp     dl, [rcx + rax]
        jb      isOldToYoung
        REPRET
        nop

    IsOldToYoung:
PATCH_LABEL JIT_WriteBarrier_Byte_Region64_Patch_Label_CardTable
        mov     rax, 0F0F0F0F0F0F0F0F0h

        shr     r8, 0Bh
        cmp     byte ptr [r8 + rax], 0FFh
        jne     UpdateCardTable
        REPRET

    UpdateCardTable:
        mov     byte ptr [r8 + rax], 0FFh
ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
        shr     r8, 0Ah
PATCH_LABEL JIT_WriteBarrier_Byte_Region64_Patch_Label_CardBundleTable
        mov     rax, 0F0F0F0F0F0F0F0F0h
        cmp     byte ptr [r8 + rax], 0FFh
        jne     UpdateCardBundleTable
        REPRET

    UpdateCardBundleTable:
        mov     byte ptr [r8 + rax], 0FFh
endif
        ret
LEAF_END_MARKED JIT_WriteBarrier_Byte_Region64, _TEXT

LEAF_ENTRY JIT_WriteBarrier_Bit_Region64, _TEXT
        align 8

        ; Do the move into the GC .  It is correct to take an AV here, the EH code
        ; figures out that this came from a WriteBarrier and correctly maps it back
        ; to the managed method which called the WriteBarrier (see setup in
        ; InitializeExceptionHandling, vm\exceptionhandling.cpp).
        mov     [rcx], rdx

        mov     r8, rcx

PATCH_LABEL JIT_WriteBarrier_Bit_Region64_Patch_Label_RegionToGeneration
        mov     rax, 0F0F0F0F0F0F0F0F0h

PATCH_LABEL JIT_WriteBarrier_Bit_Region64_Patch_Label_RegionShrDest
        shr     rcx, 16h ; compute region index

        ; Check whether the region we're storing into is gen 0 - nothing to do in this case
        cmp     byte ptr [rcx + rax], 0
        jne     NotGen0
        REPRET

        NOP_2_BYTE ; padding for alignment of constant

    NotGen0:
PATCH_LABEL JIT_WriteBarrier_Bit_Region64_Patch_Label_Lower
        mov     r9, 0F0F0F0F0F0F0F0F0h
        cmp     rdx, r9
        jae     NotLow
        ret
    NotLow:
PATCH_LABEL JIT_WriteBarrier_Bit_Region64_Patch_Label_Upper
        mov     r9, 0F0F0F0F0F0F0F0F0h
        cmp     rdx, r9
        jb      NotHigh
        REPRET
    NotHigh:
PATCH_LABEL JIT_WriteBarrier_Bit_Region64_Patch_Label_RegionShrSrc
        shr     rdx, 16h ; compute region index
        mov     dl, [rdx + rax]
        cmp     dl, [rcx + rax]
        jb      isOldToYoung
        REPRET
        nop

    IsOldToYoung:
PATCH_LABEL JIT_WriteBarrier_Bit_Region64_Patch_Label_CardTable
        mov     rax, 0F0F0F0F0F0F0F0F0h

        mov     ecx, r8d
        shr     r8, 0Bh
        shr     ecx, 8
        and     ecx, 7
        mov     dl, 1
        shl     dl, cl
        test    byte ptr [r8 + rax], dl
        je      UpdateCardTable
        REPRET

    UpdateCardTable:
        lock or byte ptr [r8 + rax], dl

ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
PATCH_LABEL JIT_WriteBarrier_Bit_Region64_Patch_Label_CardBundleTable
        mov     rax, 0F0F0F0F0F0F0F0F0h
        shr     r8, 0Ah
        cmp     byte ptr [r8 + rax], 0FFh
        jne     UpdateCardBundleTable
        REPRET

    UpdateCardBundleTable:
        mov     byte ptr [r8 + rax], 0FFh
endif
        ret
LEAF_END_MARKED JIT_WriteBarrier_Bit_Region64, _TEXT

endif


ifdef FEATURE_SATORI_GC

; The Satori barriers. WriteBarrierManager copies one of them into the JIT_WriteBarrier buffer (see patchedcode.asm)
; and patches the constants in the copy:
; - JIT_WriteBarrier_SATORI             - the regular barrier, it reads the barrier state (g_write_watch_table).
; - JIT_WriteBarrier_SATORI2            - the GC does not mark concurrently and the next GC is a full GC.
; - JIT_WriteBarrier_SATORI_CONCURRENT  - while the GC marks concurrently and the next GC is not a full GC.
; - JIT_WriteBarrier_SATORI_CONCURRENT2 - while the GC marks concurrently and the next GC is a full GC.
;
; The concurrent barriers do not check the barrier state, they assume that the GC marks concurrently.
; The GC switches barriers only when EE is suspended anyway: to a concurrent barrier at the start of
; the concurrent marking and back to a nonconcurrent barrier in the blocking collection that follows.
;
; The barriers have the same code ahead of the <Name>_CheckedEntry label, so that JIT_CheckedWriteBarrier
; can jump to CheckedEntry in the buffer, whichever barrier is there.
;
; Regarding patchable constants:
; - 64-bit constants have to be loaded into a register
; - The constants have to be aligned to 8 bytes so that they can be patched easily
; - The alignment is by construction: there is only fixed-size code between an alignment directive and a constant
;
;   rcx - dest address
;   rdx - object
;
LEAF_ENTRY JIT_WriteBarrier_SATORI, _TEXT

ifdef FEATURE_SATORI_EXTERNAL_OBJECTS
    ; check if src is in heap
        NOP_6_BYTE ; padding for alignment of constant
PATCH_LABEL JIT_WriteBarrier_SATORI_Patch_Label_CardBundleTable
        mov     rax, 0F0F0F0F0F0F0F0F0h    ; the page byte map
PATCH_LABEL JIT_WriteBarrier_SATORI_CheckedEntry
        mov     r8,  rdx
        shr     r8,  30                    ; src page index
        cmp     byte ptr [rax + r8], 0
        je      JustAssign                 ; src not in heap
else
PATCH_LABEL JIT_WriteBarrier_SATORI_CheckedEntry
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

    ; The barrier state may become concurrent while EE runs, thus this barrier reads it.
PATCH_LABEL JIT_WriteBarrier_SATORI_Patch_Label_WriteWatchTable
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
PATCH_LABEL JIT_WriteBarrier_SATORI_Patch_Label_CardTable
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
LEAF_END_MARKED JIT_WriteBarrier_SATORI, _TEXT

;
; The GC does not mark concurrently and the next GC is a full GC, thus cards are not needed.
; The barrier state may become concurrent while EE runs, so this barrier still checks it.
;
LEAF_ENTRY JIT_WriteBarrier_SATORI2, _TEXT

ifdef FEATURE_SATORI_EXTERNAL_OBJECTS
    ; check if src is in heap
        NOP_6_BYTE ; padding for alignment of constant
PATCH_LABEL JIT_WriteBarrier_SATORI2_Patch_Label_CardBundleTable
        mov     rax, 0F0F0F0F0F0F0F0F0h    ; the page byte map
PATCH_LABEL JIT_WriteBarrier_SATORI2_CheckedEntry
        mov     r8,  rdx
        shr     r8,  30                    ; src page index
        cmp     byte ptr [rax + r8], 0
        je      JustAssign                 ; src not in heap
else
PATCH_LABEL JIT_WriteBarrier_SATORI2_CheckedEntry
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

    ; The barrier state may become concurrent while EE runs, thus this barrier reads it.
PATCH_LABEL JIT_WriteBarrier_SATORI2_Patch_Label_WriteWatchTable
        mov     r10, 0F0F0F0F0F0F0F0F0h ; the address of the barrier state (g_write_watch_table)

    ; check the barrier state. this must be done after the assignment (in program order)
    ; if state == 2 we do not set or dirty cards. Otherwise the GC marks concurrently for a full GC,
    ; which marks through objects of any generation, so the card is dirtied regardless of src.
        cmp     qword ptr [r10], 2h
        jne     MarkCards
        ret

    ; Not executed. Aligns the constant below: the padding and the 2-byte opcode end on an 8-byte boundary.
        align   8
        NOP_6_BYTE
    MarkCards:
    ; fetch card location for rcx
PATCH_LABEL JIT_WriteBarrier_SATORI2_Patch_Label_CardTable
        mov     r9, 0F0F0F0F0F0F0F0F0h  ; the page map
        mov     r8,  rcx
        shr     rcx, 30
        mov     rax, qword ptr [r9 + rcx * 8] ; page
        sub     r8, rax   ; offset in page
        mov     rdx,r8
        shr     r8, 9     ; card offset
        shr     rdx, 20   ; group index
        lea     rdx, [rax + rdx * 2 + 80h] ; group offset

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
     Exit:
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
LEAF_END_MARKED JIT_WriteBarrier_SATORI2, _TEXT

;
; The GC marks concurrently and the next GC is not a full GC.
; The cards are dirtied, but only if src is ephemeral (gen < 2). A gen1 GC does not mark through tenured
; objects and references to them do not need to be remembered.
;
LEAF_ENTRY JIT_WriteBarrier_SATORI_CONCURRENT, _TEXT

ifdef FEATURE_SATORI_EXTERNAL_OBJECTS
    ; check if src is in heap
        NOP_6_BYTE ; padding for alignment of constant
PATCH_LABEL JIT_WriteBarrier_SATORI_CONCURRENT_Patch_Label_CardBundleTable
        mov     rax, 0F0F0F0F0F0F0F0F0h    ; the page byte map
PATCH_LABEL JIT_WriteBarrier_SATORI_CONCURRENT_CheckedEntry
        mov     r8,  rdx
        shr     r8,  30                    ; src page index
        cmp     byte ptr [rax + r8], 0
        je      JustAssign                 ; src not in heap
else
PATCH_LABEL JIT_WriteBarrier_SATORI_CONCURRENT_CheckedEntry
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
PATCH_LABEL JIT_WriteBarrier_SATORI_CONCURRENT_Patch_Label_CardTable
        mov     r9, 0F0F0F0F0F0F0F0F0h  ; the page map

    ; if src is tenured (gen >= 2), cards are not needed
        cmp     dword ptr [r8 + 16], 2
        jge     Exit

    ; fetch card location for rcx
        mov     r8,  rcx
        shr     rcx, 30
        mov     rax, qword ptr [r9 + rcx * 8] ; page
        sub     r8, rax   ; offset in page
        mov     rdx,r8
        shr     r8, 9     ; card offset
        shr     rdx, 20   ; group index
        lea     rdx, [rax + rdx * 2 + 80h] ; group offset

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
     Exit:
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
LEAF_END_MARKED JIT_WriteBarrier_SATORI_CONCURRENT, _TEXT

;
; The GC marks concurrently and the next GC is a full GC.
; The cards are dirtied for any src, since a full GC marks through objects of any generation.
;
LEAF_ENTRY JIT_WriteBarrier_SATORI_CONCURRENT2, _TEXT

ifdef FEATURE_SATORI_EXTERNAL_OBJECTS
    ; check if src is in heap
        NOP_6_BYTE ; padding for alignment of constant
PATCH_LABEL JIT_WriteBarrier_SATORI_CONCURRENT2_Patch_Label_CardBundleTable
        mov     rax, 0F0F0F0F0F0F0F0F0h    ; the page byte map
PATCH_LABEL JIT_WriteBarrier_SATORI_CONCURRENT2_CheckedEntry
        mov     r8,  rdx
        shr     r8,  30                    ; src page index
        cmp     byte ptr [rax + r8], 0
        je      JustAssign                 ; src not in heap
else
PATCH_LABEL JIT_WriteBarrier_SATORI_CONCURRENT2_CheckedEntry
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

    ; fetch card location for rcx
PATCH_LABEL JIT_WriteBarrier_SATORI_CONCURRENT2_Patch_Label_CardTable
        mov     r9, 0F0F0F0F0F0F0F0F0h  ; the page map
        mov     r8,  rcx
        shr     rcx, 30
        mov     rax, qword ptr [r9 + rcx * 8] ; page
        sub     r8, rax   ; offset in page
        mov     rdx,r8
        shr     r8, 9     ; card offset
        shr     rdx, 20   ; group index
        lea     rdx, [rax + rdx * 2 + 80h] ; group offset

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
     Exit:
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
LEAF_END_MARKED JIT_WriteBarrier_SATORI_CONCURRENT2, _TEXT

endif ; FEATURE_SATORI_GC


ifdef FEATURE_USE_SOFTWARE_WRITE_WATCH_FOR_GC_HEAP

LEAF_ENTRY JIT_WriteBarrier_WriteWatch_PreGrow64, _TEXT
        align 8

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
PATCH_LABEL JIT_WriteBarrier_WriteWatch_PreGrow64_Patch_Label_WriteWatchTable
        mov     r8, 0F0F0F0F0F0F0F0F0h
        shr     rax, 0Ch ; SoftwareWriteWatch::AddressToTableByteIndexShift
        NOP_2_BYTE ; padding for alignment of constant
PATCH_LABEL JIT_WriteBarrier_WriteWatch_PreGrow64_Patch_Label_Lower
        mov     r9, 0F0F0F0F0F0F0F0F0h
        add     rax, r8
        cmp     byte ptr [rax], 0h
        jne     CheckCardTable
        mov     byte ptr [rax], 0FFh

        ; Check the lower ephemeral region bound.
    CheckCardTable:
        cmp     rdx, r9
        jb      Exit

        ; Touch the card table entry, if not already dirty.
        shr     rcx, 0Bh
        NOP_2_BYTE ; padding for alignment of constant
PATCH_LABEL JIT_WriteBarrier_WriteWatch_PreGrow64_Patch_Label_CardTable
        mov     rax, 0F0F0F0F0F0F0F0F0h
        cmp     byte ptr [rcx + rax], 0FFh
        jne     UpdateCardTable
        REPRET

    UpdateCardTable:
        mov     byte ptr [rcx + rax], 0FFh
ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
        NOP_2_BYTE ; padding for alignment of constant
PATCH_LABEL JIT_WriteBarrier_WriteWatch_PreGrow64_Patch_Label_CardBundleTable
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
LEAF_END_MARKED JIT_WriteBarrier_WriteWatch_PreGrow64, _TEXT


LEAF_ENTRY JIT_WriteBarrier_WriteWatch_PostGrow64, _TEXT
        align 8

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
PATCH_LABEL JIT_WriteBarrier_WriteWatch_PostGrow64_Patch_Label_WriteWatchTable
        mov     r8, 0F0F0F0F0F0F0F0F0h
        shr     rax, 0Ch ; SoftwareWriteWatch::AddressToTableByteIndexShift
        NOP_2_BYTE ; padding for alignment of constant
PATCH_LABEL JIT_WriteBarrier_WriteWatch_PostGrow64_Patch_Label_Lower
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

PATCH_LABEL JIT_WriteBarrier_WriteWatch_PostGrow64_Patch_Label_Upper
        mov     r8, 0F0F0F0F0F0F0F0F0h

        cmp     rdx, r8
        jae     Exit

        nop ; padding for alignment of constant

PATCH_LABEL JIT_WriteBarrier_WriteWatch_PostGrow64_Patch_Label_CardTable
        mov     rax, 0F0F0F0F0F0F0F0F0h

        ; Touch the card table entry, if not already dirty.
        shr     rcx, 0Bh
        cmp     byte ptr [rcx + rax], 0FFh
        jne     UpdateCardTable
        REPRET

    UpdateCardTable:
        mov     byte ptr [rcx + rax], 0FFh
ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
        shr     rcx, 0Ah
        NOP_2_BYTE ; padding for alignment of constant
PATCH_LABEL JIT_WriteBarrier_WriteWatch_PostGrow64_Patch_Label_CardBundleTable
        mov     rax, 0F0F0F0F0F0F0F0F0h
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
LEAF_END_MARKED JIT_WriteBarrier_WriteWatch_PostGrow64, _TEXT


ifdef FEATURE_SVR_GC

LEAF_ENTRY JIT_WriteBarrier_WriteWatch_SVR64, _TEXT
        align 8

        ; Regarding patchable constants:
        ; - 64-bit constants have to be loaded into a register
        ; - The constants have to be aligned to 8 bytes so that they can be patched easily
        ; - The constant loads have been located to minimize NOP padding required to align the constants
        ; - Using different registers for successive constant loads helps pipeline better. Should we decide to use a special
        ;   non-volatile calling convention, this should be changed to use just one register.

        ;
        ; SVR GC has multiple heaps, so it cannot provide one single
        ; ephemeral region to bounds check against, so we just skip the
        ; bounds checking all together and do our card table update
        ; unconditionally.
        ;

        ; Do the move into the GC .  It is correct to take an AV here, the EH code
        ; figures out that this came from a WriteBarrier and correctly maps it back
        ; to the managed method which called the WriteBarrier (see setup in
        ; InitializeExceptionHandling, vm\exceptionhandling.cpp).
        mov     [rcx], rdx

        ; Update the write watch table if necessary
        mov     rax, rcx
PATCH_LABEL JIT_WriteBarrier_WriteWatch_SVR64_PatchLabel_WriteWatchTable
        mov     r8, 0F0F0F0F0F0F0F0F0h
        shr     rax, 0Ch ; SoftwareWriteWatch::AddressToTableByteIndexShift
        NOP_2_BYTE ; padding for alignment of constant
PATCH_LABEL JIT_WriteBarrier_WriteWatch_SVR64_PatchLabel_CardTable
        mov     r9, 0F0F0F0F0F0F0F0F0h
        add     rax, r8
        cmp     byte ptr [rax], 0h
        jne     CheckCardTable
        mov     byte ptr [rax], 0FFh

    CheckCardTable:
        shr     rcx, 0Bh
        cmp     byte ptr [rcx + r9], 0FFh
        jne     UpdateCardTable
        REPRET

    UpdateCardTable:
        mov     byte ptr [rcx + r9], 0FFh
ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
        nop ; padding for alignment of constant
PATCH_LABEL JIT_WriteBarrier_WriteWatch_SVR64_PatchLabel_CardBundleTable
        mov     rax, 0F0F0F0F0F0F0F0F0h
        shr     rcx, 0Ah
        cmp     byte ptr [rcx + rax], 0FFh
        jne     UpdateCardBundleTable
        REPRET

    UpdateCardBundleTable:
        mov     byte ptr [rcx + rax], 0FFh
endif
        ret
LEAF_END_MARKED JIT_WriteBarrier_WriteWatch_SVR64, _TEXT

endif

LEAF_ENTRY JIT_WriteBarrier_WriteWatch_Byte_Region64, _TEXT
        align 8

        ; Do the move into the GC .  It is correct to take an AV here, the EH code
        ; figures out that this came from a WriteBarrier and correctly maps it back
        ; to the managed method which called the WriteBarrier (see setup in
        ; InitializeExceptionHandling, vm\exceptionhandling.cpp).
        mov     [rcx], rdx

        ; Update the write watch table if necessary
        mov     rax, rcx
PATCH_LABEL JIT_WriteBarrier_WriteWatch_Byte_Region64_Patch_Label_WriteWatchTable
        mov     r8, 0F0F0F0F0F0F0F0F0h
        shr     rax, 0Ch ; SoftwareWriteWatch::AddressToTableByteIndexShift
        add     rax, r8
        mov     r8, rcx
PATCH_LABEL JIT_WriteBarrier_WriteWatch_Byte_Region64_Patch_Label_RegionShrDest
        shr     rcx, 16h ; compute region index
        cmp     byte ptr [rax], 0h
        jne     JIT_WriteBarrier_WriteWatch_Byte_Region64_Patch_Label_RegionToGeneration
        mov     byte ptr [rax], 0FFh

PATCH_LABEL JIT_WriteBarrier_WriteWatch_Byte_Region64_Patch_Label_RegionToGeneration
        mov     rax, 0F0F0F0F0F0F0F0F0h

        ; Check whether the region we're storing into is gen 0 - nothing to do in this case
        cmp     byte ptr [rcx + rax], 0
        jne     NotGen0
        REPRET

        NOP_2_BYTE ; padding for alignment of constant
        NOP_2_BYTE ; padding for alignment of constant
        NOP_2_BYTE ; padding for alignment of constant

    NotGen0:
PATCH_LABEL JIT_WriteBarrier_WriteWatch_Byte_Region64_Patch_Label_Lower
        mov     r9, 0F0F0F0F0F0F0F0F0h
        cmp     rdx, r9
        jae     NotLow
        ret
    NotLow:
PATCH_LABEL JIT_WriteBarrier_WriteWatch_Byte_Region64_Patch_Label_Upper
        mov     r9, 0F0F0F0F0F0F0F0F0h
        cmp     rdx, r9
        jb      NotHigh
        REPRET
    NotHigh:
PATCH_LABEL JIT_WriteBarrier_WriteWatch_Byte_Region64_Patch_Label_RegionShrSrc
        shr     rdx, 16h ; compute region index
        mov     dl, [rdx + rax]
        cmp     dl, [rcx + rax]
        jb      isOldToYoung
        REPRET
        nop

    IsOldToYoung:
PATCH_LABEL JIT_WriteBarrier_WriteWatch_Byte_Region64_Patch_Label_CardTable
        mov     rax, 0F0F0F0F0F0F0F0F0h

        shr     r8, 0Bh
        cmp     byte ptr [r8 + rax], 0FFh
        jne     UpdateCardTable
        REPRET

    UpdateCardTable:
        mov     byte ptr [r8 + rax], 0FFh
ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
        shr     r8, 0Ah
PATCH_LABEL JIT_WriteBarrier_WriteWatch_Byte_Region64_Patch_Label_CardBundleTable
        mov     rax, 0F0F0F0F0F0F0F0F0h
        cmp     byte ptr [r8 + rax], 0FFh
        jne     UpdateCardBundleTable
        REPRET

    UpdateCardBundleTable:
        mov     byte ptr [r8 + rax], 0FFh
endif
        ret
LEAF_END_MARKED JIT_WriteBarrier_WriteWatch_Byte_Region64, _TEXT

LEAF_ENTRY JIT_WriteBarrier_WriteWatch_Bit_Region64, _TEXT
        align 8

        ; Do the move into the GC .  It is correct to take an AV here, the EH code
        ; figures out that this came from a WriteBarrier and correctly maps it back
        ; to the managed method which called the WriteBarrier (see setup in
        ; InitializeExceptionHandling, vm\exceptionhandling.cpp).
        mov     [rcx], rdx

        ; Update the write watch table if necessary
        mov     rax, rcx
PATCH_LABEL JIT_WriteBarrier_WriteWatch_Bit_Region64_Patch_Label_WriteWatchTable
        mov     r8, 0F0F0F0F0F0F0F0F0h
        shr     rax, 0Ch ; SoftwareWriteWatch::AddressToTableByteIndexShift
        add     rax, r8
        mov     r8, rcx
PATCH_LABEL JIT_WriteBarrier_WriteWatch_Bit_Region64_Patch_Label_RegionShrDest
        shr     rcx, 16h ; compute region index
        cmp     byte ptr [rax], 0h
        jne     JIT_WriteBarrier_WriteWatch_Bit_Region64_Patch_Label_RegionToGeneration
        mov     byte ptr [rax], 0FFh

PATCH_LABEL JIT_WriteBarrier_WriteWatch_Bit_Region64_Patch_Label_RegionToGeneration
        mov     rax, 0F0F0F0F0F0F0F0F0h

        ; Check whether the region we're storing into is gen 0 - nothing to do in this case
        cmp     byte ptr [rcx + rax], 0
        jne     NotGen0
        REPRET

        NOP_2_BYTE ; padding for alignment of constant
        NOP_2_BYTE ; padding for alignment of constant
        NOP_2_BYTE ; padding for alignment of constant

    NotGen0:
PATCH_LABEL JIT_WriteBarrier_WriteWatch_Bit_Region64_Patch_Label_Lower
        mov     r9, 0F0F0F0F0F0F0F0F0h
        cmp     rdx, r9
        jae     NotLow
        ret
    NotLow:
PATCH_LABEL JIT_WriteBarrier_WriteWatch_Bit_Region64_Patch_Label_Upper
        mov     r9, 0F0F0F0F0F0F0F0F0h
        cmp     rdx, r9
        jb      NotHigh
        REPRET
    NotHigh:
PATCH_LABEL JIT_WriteBarrier_WriteWatch_Bit_Region64_Patch_Label_RegionShrSrc
        shr     rdx, 16h ; compute region index
        mov     dl, [rdx + rax]
        cmp     dl, [rcx + rax]
        jb      isOldToYoung
        REPRET
        nop

    IsOldToYoung:
PATCH_LABEL JIT_WriteBarrier_WriteWatch_Bit_Region64_Patch_Label_CardTable
        mov     rax, 0F0F0F0F0F0F0F0F0h

        mov     ecx, r8d
        shr     r8, 0Bh
        shr     ecx, 8
        and     ecx, 7
        mov     dl, 1
        shl     dl, cl
        test    byte ptr [r8 + rax], dl
        je      UpdateCardTable
        REPRET

    UpdateCardTable:
        lock or byte ptr [r8 + rax], dl
ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
PATCH_LABEL JIT_WriteBarrier_WriteWatch_Bit_Region64_Patch_Label_CardBundleTable
        mov     rax, 0F0F0F0F0F0F0F0F0h
        shr     r8, 0Ah
        cmp     byte ptr [r8 + rax], 0FFh
        jne     UpdateCardBundleTable
        REPRET

    UpdateCardBundleTable:
        mov     byte ptr [r8 + rax], 0FFh
endif
        ret
LEAF_END_MARKED JIT_WriteBarrier_WriteWatch_Bit_Region64, _TEXT

endif


        end
