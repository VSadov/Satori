;; Licensed to the .NET Foundation under one or more agreements.
;; The .NET Foundation licenses this file to you under the MIT license.

include AsmMacros.inc

ifndef FEATURE_SATORI_GC

;; Macro used to copy contents of newly updated GC heap locations to a shadow copy of the heap. This is used
;; during garbage collections to verify that object references where never written to the heap without using a
;; write barrier. Note that we're potentially racing to update the shadow heap while other threads are writing
;; new references to the real heap. Since this can't be solved perfectly without critical sections around the
;; entire update process, we instead update the shadow location and then re-check the real location (as two
;; ordered operations) and if there is a disparity we'll re-write the shadow location with a special value
;; (INVALIDGCVALUE) which disables the check for that location. Since the shadow heap is only validated at GC
;; time and these write barrier operations are atomic wrt to GCs this is sufficient to guarantee that the
;; shadow heap contains only valid copies of real heap values or INVALIDGCVALUE.
ifdef WRITE_BARRIER_CHECK

g_GCShadow      TEXTEQU <?g_GCShadow@@3PEAEEA>
g_GCShadowEnd   TEXTEQU <?g_GCShadowEnd@@3PEAEEA>
INVALIDGCVALUE  EQU 0CCCCCCCDh

EXTERN  g_GCShadow : QWORD
EXTERN  g_GCShadowEnd : QWORD

UPDATE_GC_SHADOW macro BASENAME, REFREG, DESTREG

    ;; If g_GCShadow is 0, don't perform the check.
    cmp     g_GCShadow, 0
    je      &BASENAME&_UpdateShadowHeap_Done_&REFREG&

    ;; Save DESTREG since we're about to modify it (and we need the original value both within the macro and
    ;; once we exit the macro). Note that this is naughty since we're altering the stack pointer outside of
    ;; the prolog inside a method without a frame. But given that this is only debug code and generally we
    ;; shouldn't be walking the stack at this point it seems preferable to recoding the all the barrier
    ;; variants to set up frames. Unlike RhpBulkWriteBarrier below which is treated as a helper call using the
    ;; usual calling convention, the compiler knows exactly which registers are trashed in the simple write
    ;; barrier case, so we don't have any more scratch registers to play with (and doing so would only make
    ;; things harder if at a later stage we want to allow multiple barrier versions based on the input
    ;; registers).
    push    DESTREG

    ;; Transform DESTREG into the equivalent address in the shadow heap.
    sub     DESTREG, g_lowest_address
    jb      &BASENAME&_UpdateShadowHeap_PopThenDone_&REFREG&
    add     DESTREG, [g_GCShadow]
    cmp     DESTREG, [g_GCShadowEnd]
    jae     &BASENAME&_UpdateShadowHeap_PopThenDone_&REFREG&

    ;; Update the shadow heap.
    mov     [DESTREG], REFREG

    ;; Now check that the real heap location still contains the value we just wrote into the shadow heap. This
    ;; read must be strongly ordered wrt to the previous write to prevent race conditions. We also need to
    ;; recover the old value of DESTREG for the comparison so use an xchg instruction (which has an implicit lock
    ;; prefix).
    xchg    [rsp], DESTREG
    cmp     [DESTREG], REFREG
    jne     &BASENAME&_UpdateShadowHeap_Invalidate_&REFREG&

    ;; The original DESTREG value is now restored but the stack has a value (the shadow version of the
    ;; location) pushed. Need to discard this push before we are done.
    add     rsp, 8
    jmp     &BASENAME&_UpdateShadowHeap_Done_&REFREG&

&BASENAME&_UpdateShadowHeap_Invalidate_&REFREG&:
    ;; Someone went and updated the real heap. We need to invalidate the shadow location since we can't
    ;; guarantee whose shadow update won.

    ;; Retrieve shadow location from the stack and restore original DESTREG to the stack. This is an
    ;; additional memory barrier we don't require but it's on the rare path and x86 doesn't have an xchg
    ;; variant that doesn't implicitly specify the lock prefix. Note that INVALIDGCVALUE is a 64-bit
    ;; immediate and therefore must be moved into a register before it can be written to the shadow
    ;; location.
    xchg    [rsp], DESTREG
    push    REFREG
    mov     REFREG, INVALIDGCVALUE
    mov     qword ptr [DESTREG], REFREG
    pop     REFREG

&BASENAME&_UpdateShadowHeap_PopThenDone_&REFREG&:
    ;; Restore original DESTREG value from the stack.
    pop     DESTREG

&BASENAME&_UpdateShadowHeap_Done_&REFREG&:
endm

else ; WRITE_BARRIER_CHECK

UPDATE_GC_SHADOW macro BASENAME, REFREG, DESTREG
endm

endif ; WRITE_BARRIER_CHECK

;; There are several different helpers used depending on which register holds the object reference. Since all
;; the helpers have identical structure we use a macro to define this structure. Two arguments are taken, the
;; name of the register that points to the location to be updated and the name of the register that holds the
;; object reference (this should be in upper case as it's used in the definition of the name of the helper).
DEFINE_UNCHECKED_WRITE_BARRIER_CORE macro BASENAME, REFREG

    ;; Update the shadow copy of the heap with the same value just written to the same heap. (A no-op unless
    ;; we're in a debug build and write barrier checking has been enabled).
    UPDATE_GC_SHADOW BASENAME, REFREG, rcx

ifdef FEATURE_USE_SOFTWARE_WRITE_WATCH_FOR_GC_HEAP
    mov     r11, [g_write_watch_table]
    cmp     r11, 0
    je      &BASENAME&_CheckCardTable_&REFREG&

    mov     r10, rcx
    shr     r10, 0Ch ;; SoftwareWriteWatch::AddressToTableByteIndexShift
    add     r10, r11
    cmp     byte ptr [r10], 0
    jne     &BASENAME&_CheckCardTable_&REFREG&
    mov     byte ptr [r10], 0FFh
endif

&BASENAME&_CheckCardTable_&REFREG&:

    ;; If the reference is to an object that's not in an ephemeral generation we have no need to track it
    ;; (since the object won't be collected or moved by an ephemeral collection).
    cmp     REFREG, [g_ephemeral_low]
    jb      &BASENAME&_NoBarrierRequired_&REFREG&
    cmp     REFREG, [g_ephemeral_high]
    jae     &BASENAME&_NoBarrierRequired_&REFREG&

    ;; We have a location on the GC heap being updated with a reference to an ephemeral object so we must
    ;; track this write. The location address is translated into an offset in the card table bitmap. We set
    ;; an entire byte in the card table since it's quicker than messing around with bitmasks and we only write
    ;; the byte if it hasn't already been done since writes are expensive and impact scaling.
    shr     rcx, 0Bh
    mov     r10, [g_card_table]
    cmp     byte ptr [rcx + r10], 0FFh
    je      &BASENAME&_NoBarrierRequired_&REFREG&

    ;; We get here if it's necessary to update the card table.
    mov     byte ptr [rcx + r10], 0FFh

ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
    ;; Shift rcx by 0Ah more to get the card bundle byte (we shifted by 0x0B already)
    shr     rcx, 0Ah
    add     rcx, [g_card_bundle_table]
    cmp     byte ptr [rcx], 0FFh
    je      &BASENAME&_NoBarrierRequired_&REFREG&

    mov     byte ptr [rcx], 0FFh
endif

&BASENAME&_NoBarrierRequired_&REFREG&:
    ret

endm

;; There are several different helpers used depending on which register holds the object reference. Since all
;; the helpers have identical structure we use a macro to define this structure. One argument is taken, the
;; name of the register that will hold the object reference (this should be in upper case as it's used in the
;; definition of the name of the helper).
DEFINE_UNCHECKED_WRITE_BARRIER macro REFREG, EXPORT_REG_NAME

;; Define a helper with a name of the form RhpAssignRefEAX etc. (along with suitable calling standard
;; decoration). The location to be updated is in DESTREG. The object reference that will be assigned into that
;; location is in one of the other general registers determined by the value of REFREG.

;; WARNING: Code in EHHelpers.cpp makes assumptions about write barrier code, in particular:
;; - Function "InWriteBarrierHelper" assumes an AV due to passed in null pointer will happen on the first instruction
;; - Function "UnwindSimpleHelperToCaller" assumes the stack contains just the pushed return address
LEAF_ENTRY RhpAssignRef&EXPORT_REG_NAME&, _TEXT

    ;; Export the canonical write barrier under unqualified name as well
    ifidni <REFREG>, <RDX>
    ALTERNATE_ENTRY RhpAssignRef
    ALTERNATE_ENTRY RhpAssignRefAVLocation
    endif

    ;; Write the reference into the location. Note that we rely on the fact that no GC can occur between here
    ;; and the card table update we may perform below.
    mov     qword ptr [rcx], REFREG

    DEFINE_UNCHECKED_WRITE_BARRIER_CORE RhpAssignRef, REFREG

LEAF_END RhpAssignRef&EXPORT_REG_NAME&, _TEXT
endm

;; One day we might have write barriers for all the possible argument registers but for now we have
;; just one write barrier that assumes the input register is RDX.
DEFINE_UNCHECKED_WRITE_BARRIER RDX, EDX

;;
;; Define the helpers used to implement the write barrier required when writing an object reference into a
;; location residing on the GC heap. Such write barriers allow the GC to optimize which objects in
;; non-ephemeral generations need to be scanned for references to ephemeral objects during an ephemeral
;; collection.
;;

DEFINE_CHECKED_WRITE_BARRIER_CORE macro BASENAME, REFREG

    ;; The location being updated might not even lie in the GC heap (a handle or stack location for instance),
    ;; in which case no write barrier is required.
    cmp     rcx, [g_lowest_address]
    jb      &BASENAME&_NoBarrierRequired_&REFREG&
    cmp     rcx, [g_highest_address]
    jae     &BASENAME&_NoBarrierRequired_&REFREG&

    DEFINE_UNCHECKED_WRITE_BARRIER_CORE BASENAME, REFREG

endm

;; There are several different helpers used depending on which register holds the object reference. Since all
;; the helpers have identical structure we use a macro to define this structure. One argument is taken, the
;; name of the register that will hold the object reference (this should be in upper case as it's used in the
;; definition of the name of the helper).
DEFINE_CHECKED_WRITE_BARRIER macro REFREG, EXPORT_REG_NAME

;; Define a helper with a name of the form RhpCheckedAssignRefEAX etc. (along with suitable calling standard
;; decoration). The location to be updated is always in RCX. The object reference that will be assigned into
;; that location is in one of the other general registers determined by the value of REFREG.

;; WARNING: Code in EHHelpers.cpp makes assumptions about write barrier code, in particular:
;; - Function "InWriteBarrierHelper" assumes an AV due to passed in null pointer will happen on the first instruction
;; - Function "UnwindSimpleHelperToCaller" assumes the stack contains just the pushed return address
LEAF_ENTRY RhpCheckedAssignRef&EXPORT_REG_NAME&, _TEXT

    ;; Export the canonical write barrier under unqualified name as well
    ifidni <REFREG>, <RDX>
    ALTERNATE_ENTRY RhpCheckedAssignRef
    ALTERNATE_ENTRY RhpCheckedAssignRefAVLocation
    endif

    ;; Write the reference into the location. Note that we rely on the fact that no GC can occur between here
    ;; and the card table update we may perform below.
    mov     qword ptr [rcx], REFREG

    DEFINE_CHECKED_WRITE_BARRIER_CORE RhpCheckedAssignRef, REFREG

LEAF_END RhpCheckedAssignRef&EXPORT_REG_NAME&, _TEXT
endm

;; One day we might have write barriers for all the possible argument registers but for now we have
;; just one write barrier that assumes the input register is RDX.
DEFINE_CHECKED_WRITE_BARRIER RDX, EDX

LEAF_ENTRY RhpCheckedLockCmpXchg, _TEXT
    mov             rax, r8
    lock cmpxchg    [rcx], rdx
    jne             RhpCheckedLockCmpXchg_NoBarrierRequired_RDX

    DEFINE_CHECKED_WRITE_BARRIER_CORE RhpCheckedLockCmpXchg, RDX

LEAF_END RhpCheckedLockCmpXchg, _TEXT

LEAF_ENTRY RhpCheckedXchg, _TEXT

    ;; Setup rax with the new object for the exchange, that way it will automatically hold the correct result
    ;; afterwards and we can leave rdx unaltered ready for the GC write barrier below.
    mov             rax, rdx
    xchg            [rcx], rax

    DEFINE_CHECKED_WRITE_BARRIER_CORE RhpCheckedXchg, RDX

LEAF_END RhpCheckedXchg, _TEXT

;;
;; RhpByRefAssignRef simulates movs instruction for object references.
;;
;; On entry:
;;      rdi: address of ref-field (assigned to)
;;      rsi: address of the data (source)
;;
;; On exit:
;;      rdi, rsi are incremented by 8,
;;      rcx, rax: trashed
;;
;; NOTE: Keep in sync with RBM_CALLEE_TRASH_WRITEBARRIER_BYREF and RBM_CALLEE_GCTRASH_WRITEBARRIER_BYREF
;;       if you add more trashed registers.
;;
;; WARNING: Code in EHHelpers.cpp makes assumptions about write barrier code, in particular:
;; - Function "InWriteBarrierHelper" assumes an AV due to passed in null pointer will happen at RhpByRefAssignRefAVLocation1/2
;; - Function "UnwindSimpleHelperToCaller" assumes the stack contains just the pushed return address
LEAF_ENTRY RhpByRefAssignRef, _TEXT
ALTERNATE_ENTRY RhpByRefAssignRefAVLocation1
    mov     rcx, [rsi]
ALTERNATE_ENTRY RhpByRefAssignRefAVLocation2
    mov     [rdi], rcx

    ;; Check whether the writes were even into the heap. If not there's no card update required.
    cmp     rdi, [g_lowest_address]
    jb      RhpByRefAssignRef_NoBarrierRequired
    cmp     rdi, [g_highest_address]
    jae     RhpByRefAssignRef_NoBarrierRequired

    ;; Update the shadow copy of the heap with the same value just written to the same heap. (A no-op unless
    ;; we're in a debug build and write barrier checking has been enabled).
    UPDATE_GC_SHADOW BASENAME, rcx, rdi

ifdef FEATURE_USE_SOFTWARE_WRITE_WATCH_FOR_GC_HEAP
    cmp     [g_write_watch_table], 0
    je      RhpByRefAssignRef_CheckCardTable

    mov     rax, rdi
    shr     rax, 0Ch ;; SoftwareWriteWatch::AddressToTableByteIndexShift
    add     rax, [g_write_watch_table]
    cmp     byte ptr [rax], 0
    jne     RhpByRefAssignRef_CheckCardTable
    mov     byte ptr [rax], 0FFh
endif

RhpByRefAssignRef_CheckCardTable:

    ;; If the reference is to an object that's not in an ephemeral generation we have no need to track it
    ;; (since the object won't be collected or moved by an ephemeral collection).
    cmp     rcx, [g_ephemeral_low]
    jb      RhpByRefAssignRef_NoBarrierRequired
    cmp     rcx, [g_ephemeral_high]
    jae     RhpByRefAssignRef_NoBarrierRequired

    ;; move current rdi value into rcx, we need to keep rdi and eventually increment by 8
    mov     rcx, rdi

    ;; We have a location on the GC heap being updated with a reference to an ephemeral object so we must
    ;; track this write. The location address is translated into an offset in the card table bitmap. We set
    ;; an entire byte in the card table since it's quicker than messing around with bitmasks and we only write
    ;; the byte if it hasn't already been done since writes are expensive and impact scaling.
    shr     rcx, 0Bh
    mov     rax, [g_card_table]
    cmp     byte ptr [rcx + rax], 0FFh
    je      RhpByRefAssignRef_NoBarrierRequired

;; We get here if it's necessary to update the card table.
    mov     byte ptr [rcx + rax], 0FFh

ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
    ;; Shift rcx by 0Ah more to get the card bundle byte (we shifted by 0Bh already)
    shr     rcx, 0Ah
    add     rcx, [g_card_bundle_table]
    cmp     byte ptr [rcx], 0FFh
    je      RhpByRefAssignRef_NoBarrierRequired

    mov     byte ptr [rcx], 0FFh
endif

RhpByRefAssignRef_NoBarrierRequired:
    ;; Increment the pointers before leaving
    add     rdi, 8h
    add     rsi, 8h
    ret
LEAF_END RhpByRefAssignRef, _TEXT

else  ;FEATURE_SATORI_GC

;
;   rcx - dest address 
;   rdx - object
;
LEAF_ENTRY RhpCheckedAssignRef, _TEXT

    ; See if dst is in GCHeap
        mov     rax, [g_card_bundle_table] ; fetch the page byte map
        mov     r8,  rcx
        shr     r8,  30                    ; dst page index
        cmp     byte ptr [rax + r8], 0
        jne     CheckedEntry

NotInHeap:
ALTERNATE_ENTRY RhpCheckedAssignRefAVLocation
    mov     [rcx], rdx
    ret
LEAF_END RhpCheckedAssignRef, _TEXT

;
;   rcx - dest address 
;   rdx - object
;
LEAF_ENTRY RhpAssignRef, _TEXT

ifdef FEATURE_SATORI_EXTERNAL_OBJECTS
    ; check if src is in heap
        mov     rax, [g_card_bundle_table] ; fetch the page byte map
    ALTERNATE_ENTRY CheckedEntry
        mov     r8,  rdx
        shr     r8,  30                    ; dst page index
        cmp     byte ptr [rax + r8], 0
        je      JustAssign              ; src not in heap
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
        jnz     RecordEscape            ; cross region assignment. definitely escaping

    ; 3) check if the target is exposed
        mov     rax, rcx
        and     rax, 01FFFFFh
        shr     rax, 3
        bt      qword ptr [r8], rax
        jb      RecordEscape            ; target is exposed. record an escape.

    JustAssign:
ALTERNATE_ENTRY RhpAssignRefAVLocationNotHeap
        mov     [rcx], rdx              ; threadlocal assignment of unescaped object
        ret

    AssignAndMarkCards:
ALTERNATE_ENTRY RhpAssignRefAVLocation
        mov     [rcx], rdx

    ; TUNING: barriers in different modes could be separate pieces of code, but barrier switch 
    ;         needs to suspend EE, not sure if skipping mode check would worth that much.
        mov     r11, qword ptr [g_write_watch_table]

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
        cmp     qword ptr [g_write_watch_table], 0h
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
LEAF_END RhpAssignRef, _TEXT

;;
;; RhpByRefAssignRef simulates movs instruction for object references.
;; Entry:
;;   RDI - address of ref-field (assigned to)
;;   RSI - address of the data  (source)
;; Exit:
;;   RCX is trashed
;;   RAX is trashed
;;   RDI, RSI are incremented by SIZEOF(LPVOID)
LEAF_ENTRY RhpByRefAssignRef, _TEXT
    ; See if dst is in GCHeap
        mov     rax, [g_card_bundle_table]  ; fetch the page byte map
        mov     rcx,  rdi
        shr     rcx,  30                    ; dst page index
        cmp     byte ptr [rax + rcx], 0
        jne     InHeap

ALTERNATE_ENTRY RhpByRefAssignRefAVLocation1
        mov     rcx, [rsi]
ALTERNATE_ENTRY RhpByRefAssignRefAVLocation2
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
LEAF_END RhpByRefAssignRef, _TEXT

LEAF_ENTRY RhpCheckedLockCmpXchg, _TEXT
    ;; Setup rax with the new object for the exchange, that way it will automatically hold the correct result
    ;; afterwards and we can leave rdx unaltered ready for the GC write barrier below.
    mov         rax, r8
    mov         r11, [g_card_bundle_table] ; fetch the page byte map

    ; check if dst is in heap
        mov     r8, rcx
        shr     r8, 30                    ; round to page size ( >> PAGE_BITS )
        cmp     byte ptr [r11 + r8], 0
        je      JustAssign              ; dst not in heap

    ; check for escaping assignment
    ; 1) check if we own the source region
ifdef FEATURE_SATORI_EXTERNAL_OBJECTS
        mov     r8, rdx
        shr     r8, 30                  ; round to page size ( >> PAGE_BITS )
        cmp     byte ptr [r11 + r8], 0
        je      JustAssign              ; src not in heap
endif

        mov     r8, rdx
        and     r8, 0FFFFFFFFFFE00000h  ; source region

ifndef FEATURE_SATORI_EXTERNAL_OBJECTS
        jz      JustAssign              ; assigning null
endif

        mov     r11,  gs:[30h]          ; thread tag, TEB on NT
        cmp     qword ptr [r8], r11     
        jne     AssignAndMarkCards      ; not local to this thread

    ; 2) check if the src and dst are from the same region
        mov     r11, rcx
        and     r11, 0FFFFFFFFFFE00000h ; target aligned to region
        cmp     r11, r8
        jnz     RecordEscape            ; cross region assignment. definitely escaping

    ; 3) check if the target is exposed
        mov     r11, rcx
        and     r11, 01FFFFFh
        shr     r11, 3
        bt      qword ptr [r8], r11
        jb      RecordEscape            ; target is exposed. record an escape.

    JustAssign:
        lock cmpxchg    [rcx], rdx      ; no card marking, src is not a heap object
        ret

    AssignAndMarkCards:
        lock cmpxchg    [rcx], rdx
        jne             Exit

    ; TUNING: barriers in different modes could be separate pieces of code, but barrier switch 
    ;         needs to suspend EE, not sure if skipping mode check would worth that much.
        mov     r11, qword ptr [g_write_watch_table]

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
        mov     r10, qword ptr [r9 + rcx * 8] ; page
        sub     r8, r10   ; offset in page
        mov     rdx,r8
        shr     r8, 9     ; card offset
        shr     rdx, 20   ; group index
        lea     rdx, [r10 + rdx * 2 + 80h] ; group offset

    ; check if concurrent marking is in progress
        cmp     r11, 0h
        jne     DirtyCard

    ; SETTING CARD FOR RCX
     SetCard:
        cmp     byte ptr [r10 + r8], 0
        jne     Exit
        mov     byte ptr [r10 + r8], 1
     SetGroup:
        cmp     byte ptr [rdx], 0
        jne     CardSet
        mov     byte ptr [rdx], 1
     SetPage:
        cmp     byte ptr [r10], 0
        jne     CardSet
        mov     byte ptr [r10], 1

     CardSet:
    ; check if concurrent marking is still not in progress
        cmp     qword ptr [g_write_watch_table], 0h
        jne     DirtyCard
        ret

    ; DIRTYING CARD FOR RCX
     DirtyCard:
        mov     byte ptr [r10 + r8], 4
     DirtyGroup:
        cmp     byte ptr [rdx], 4
        je      Exit
        mov     byte ptr [rdx], 4
     DirtyPage:
        cmp     byte ptr [r10], 4
        je      Exit
        mov     byte ptr [r10], 4
        ret

    ; this is expected to be rare.
    RecordEscape:

        ; 4) check if the source is escaped
        mov     r11, rdx
        add     r11, 8                        ; escape bit is MT + 1
        and     r11, 01FFFFFh
        shr     r11, 3
        bt      qword ptr [r8], r11
        jb      AssignAndMarkCards            ; source is already escaped.

        ; Align rsp
        mov  r9, rsp
        and  rsp, -16

        ; save rsp, rax, rcx, rdx, r8 and have enough stack for the callee
        push r9
        push rax
        push rcx
        push rdx
        push r8
        sub  rsp, 28h

        ; void SatoriRegion::EscapeFn(SatoriObject** dst, SatoriObject* src, SatoriRegion* region)
        call    qword ptr [r8 + 8]

        add     rsp, 28h
        pop     r8
        pop     rdx
        pop     rcx
        pop     rax
        pop     rsp
        jmp     AssignAndMarkCards
LEAF_END RhpCheckedLockCmpXchg, _TEXT

LEAF_ENTRY RhpCheckedXchg, _TEXT
    ;; Setup rax with the new object for the exchange, that way it will automatically hold the correct result
    ;; afterwards and we can leave rdx unaltered ready for the GC write barrier below.
    mov         rax, rdx
    mov         r11, [g_card_bundle_table] ; fetch the page byte map

        ; check if dst is in heap
        mov     r8, rcx
        shr     r8, 30                    ; round to page size ( >> PAGE_BITS )
        cmp     byte ptr [r11 + r8], 0
        je      JustAssign              ; dst not in heap

    ; check for escaping assignment
    ; 1) check if we own the source region
ifdef FEATURE_SATORI_EXTERNAL_OBJECTS
        mov     r8, rdx
        shr     r8, 30                 ; round to page size ( >> PAGE_BITS )
        cmp     byte ptr [r11 + r8], 0
        je      JustAssign              ; src not in heap
endif

        mov     r8, rdx
        and     r8, 0FFFFFFFFFFE00000h  ; source region

ifndef FEATURE_SATORI_EXTERNAL_OBJECTS
        jz      JustAssign              ; assigning null
endif
        mov     r11,  gs:[30h]          ; thread tag, TEB on NT
        cmp     qword ptr [r8], r11     
        jne     AssignAndMarkCards      ; not local to this thread

    ; 2) check if the src and dst are from the same region
        mov     r11, rcx
        and     r11, 0FFFFFFFFFFE00000h ; target aligned to region
        cmp     r11, r8
        jnz     RecordEscape            ; cross region assignment. definitely escaping

    ; 3) check if the target is exposed
        mov     r11, rcx
        and     r11, 01FFFFFh
        shr     r11, 3
        bt      qword ptr [r8], r11
        jb      RecordEscape            ; target is exposed. record an escape.

    JustAssign:
        xchg    [rcx], rax              ; no card marking, src is not a heap object
        ret

    AssignAndMarkCards:
        xchg    [rcx], rax

    ; TUNING: barriers in different modes could be separate pieces of code, but barrier switch 
    ;         needs to suspend EE, not sure if skipping mode check would worth that much.
        mov     r11, qword ptr [g_write_watch_table]

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
        mov     r10, qword ptr [r9 + rcx * 8] ; page
        sub     r8, r10   ; offset in page
        mov     rdx,r8
        shr     r8, 9     ; card offset
        shr     rdx, 20   ; group index
        lea     rdx, [r10 + rdx * 2 + 80h] ; group offset

    ; check if concurrent marking is in progress
        cmp     r11, 0h
        jne     DirtyCard

    ; SETTING CARD FOR RCX
     SetCard:
        cmp     byte ptr [r10 + r8], 0
        jne     Exit
        mov     byte ptr [r10 + r8], 1
     SetGroup:
        cmp     byte ptr [rdx], 0
        jne     CardSet
        mov     byte ptr [rdx], 1
     SetPage:
        cmp     byte ptr [r10], 0
        jne     CardSet
        mov     byte ptr [r10], 1

     CardSet:
    ; check if concurrent marking is still not in progress
        cmp     qword ptr [g_write_watch_table], 0h
        jne     DirtyCard
        ret

    ; DIRTYING CARD FOR RCX
     DirtyCard:
        mov     byte ptr [r10 + r8], 4
     DirtyGroup:
        cmp     byte ptr [rdx], 4
        je      Exit
        mov     byte ptr [rdx], 4
     DirtyPage:
        cmp     byte ptr [r10], 4
        je      Exit
        mov     byte ptr [r10], 4
        ret

    ; this is expected to be rare.
    RecordEscape:

        ; 4) check if the source is escaped
        mov     r11, rdx
        add     r11, 8                        ; escape bit is MT + 1
        and     r11, 01FFFFFh
        shr     r11, 3
        bt      qword ptr [r8], r11
        jb      AssignAndMarkCards            ; source is already escaped.

        ; Align rsp
        mov  r9, rsp
        and  rsp, -16

        ; save rsp, rax, rcx, rdx, r8 and have enough stack for the callee
        push r9
        push rax
        push rcx
        push rdx
        push r8
        sub  rsp, 28h

        ; void SatoriRegion::EscapeFn(SatoriObject** dst, SatoriObject* src, SatoriRegion* region)
        call    qword ptr [r8 + 8]

        add     rsp, 28h
        pop     r8
        pop     rdx
        pop     rcx
        pop     rax
        pop     rsp
        jmp     AssignAndMarkCards
LEAF_END RhpCheckedXchg, _TEXT


endif  ; FEATURE_SATORI_GC

    end
