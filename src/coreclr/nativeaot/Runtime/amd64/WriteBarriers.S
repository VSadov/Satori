// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

.intel_syntax noprefix
#include <unixasmmacros.inc>

#ifndef FEATURE_SATORI_GC

#ifdef WRITE_BARRIER_CHECK

.macro UPDATE_GC_SHADOW BASENAME, REFREG, DESTREG

    // If g_GCShadow is 0, don't perform the check.
    cmp     qword ptr [C_VAR(g_GCShadow)], 0
    je      LOCAL_LABEL(\BASENAME\()_UpdateShadowHeap_Done_\REFREG)

    // Save DESTREG since we're about to modify it (and we need the original value both within the macro and
    // once we exit the macro). Note that this is naughty since we're altering the stack pointer outside of
    // the prolog inside a method without a frame. But given that this is only debug code and generally we
    // shouldn't be walking the stack at this point it seems preferable to recoding the all the barrier
    // variants to set up frames. The compiler knows exactly which registers are trashed in the simple write
    // barrier case, so we don't have any more scratch registers to play with (and doing so would only make
    // things harder if at a later stage we want to allow multiple barrier versions based on the input
    // registers).
    push    \DESTREG

    // Transform DESTREG into the equivalent address in the shadow heap.
    sub     \DESTREG, [C_VAR(g_lowest_address)]
    jb      LOCAL_LABEL(\BASENAME\()_UpdateShadowHeap_PopThenDone_\REFREG)
    add     \DESTREG, [C_VAR(g_GCShadow)]
    cmp     \DESTREG, [C_VAR(g_GCShadowEnd)]
    jae     LOCAL_LABEL(\BASENAME\()_UpdateShadowHeap_PopThenDone_\REFREG)

    // Update the shadow heap.
    mov     [\DESTREG], \REFREG

    // Now check that the real heap location still contains the value we just wrote into the shadow heap. This
    // read must be strongly ordered wrt to the previous write to prevent race conditions. We also need to
    // recover the old value of DESTREG for the comparison so use an xchg instruction (which has an implicit lock
    // prefix).
    xchg    [rsp], \DESTREG
    cmp     [\DESTREG], \REFREG
    jne     LOCAL_LABEL(\BASENAME\()_UpdateShadowHeap_Invalidate_\REFREG)

    // The original DESTREG value is now restored but the stack has a value (the shadow version of the
    // location) pushed. Need to discard this push before we are done.
    add     rsp, 8
    jmp     LOCAL_LABEL(\BASENAME\()_UpdateShadowHeap_Done_\REFREG)

LOCAL_LABEL(\BASENAME\()_UpdateShadowHeap_Invalidate_\REFREG):
    // Someone went and updated the real heap. We need to invalidate the shadow location since we can't
    // guarantee whose shadow update won.

    // Retrieve shadow location from the stack and restore original DESTREG to the stack. This is an
    // additional memory barrier we don't require but it's on the rare path and x86 doesn't have an xchg
    // variant that doesn't implicitly specify the lock prefix. Note that INVALIDGCVALUE is a 64-bit
    // immediate and therefore must be moved into a register before it can be written to the shadow
    // location.
    xchg    [rsp], \DESTREG
    push    \REFREG
    movabs  \REFREG, INVALIDGCVALUE
    mov     qword ptr [\DESTREG], \REFREG
    pop     \REFREG

LOCAL_LABEL(\BASENAME\()_UpdateShadowHeap_PopThenDone_\REFREG):
    // Restore original DESTREG value from the stack.
    pop     \DESTREG

LOCAL_LABEL(\BASENAME\()_UpdateShadowHeap_Done_\REFREG):
.endm

#else // WRITE_BARRIER_CHECK

.macro UPDATE_GC_SHADOW BASENAME, REFREG, DESTREG
.endm

#endif // WRITE_BARRIER_CHECK

// There are several different helpers used depending on which register holds the object reference. Since all
// the helpers have identical structure we use a macro to define this structure. Two arguments are taken, the
// name of the register that points to the location to be updated and the name of the register that holds the
// object reference (this should be in upper case as it's used in the definition of the name of the helper).
.macro DEFINE_UNCHECKED_WRITE_BARRIER_CORE BASENAME, REFREG

    // Update the shadow copy of the heap with the same value just written to the same heap. (A no-op unless
    // we're in a debug build and write barrier checking has been enabled).
    UPDATE_GC_SHADOW \BASENAME, \REFREG, rdi

#ifdef FEATURE_USE_SOFTWARE_WRITE_WATCH_FOR_GC_HEAP
    mov     r11, [C_VAR(g_write_watch_table)]
    cmp     r11, 0x0
    je      LOCAL_LABEL(\BASENAME\()_CheckCardTable_\REFREG)

    mov     r10, rdi
    shr     r10, 0xC // SoftwareWriteWatch::AddressToTableByteIndexShift
    add     r10, r11
    cmp     byte ptr [r10], 0x0
    jne     LOCAL_LABEL(\BASENAME\()_CheckCardTable_\REFREG)
    mov     byte ptr [r10], 0xFF
#endif

LOCAL_LABEL(\BASENAME\()_CheckCardTable_\REFREG):

    // If the reference is to an object that's not in an ephemeral generation we have no need to track it
    // (since the object won't be collected or moved by an ephemeral collection).
    cmp     \REFREG, [C_VAR(g_ephemeral_low)]
    jb      LOCAL_LABEL(\BASENAME\()_NoBarrierRequired_\REFREG)
    cmp     \REFREG, [C_VAR(g_ephemeral_high)]
    jae     LOCAL_LABEL(\BASENAME\()_NoBarrierRequired_\REFREG)

    // We have a location on the GC heap being updated with a reference to an ephemeral object so we must
    // track this write. The location address is translated into an offset in the card table bitmap. We set
    // an entire byte in the card table since it's quicker than messing around with bitmasks and we only write
    // the byte if it hasn't already been done since writes are expensive and impact scaling.
    shr     rdi, 0x0B
    mov     r10, [C_VAR(g_card_table)]
    cmp     byte ptr [rdi + r10], 0x0FF
    je      LOCAL_LABEL(\BASENAME\()_NoBarrierRequired_\REFREG)

// We get here if it's necessary to update the card table.
    mov     byte ptr [rdi + r10], 0xFF

#ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
    // Shift rdi by 0x0A more to get the card bundle byte (we shifted by 0x0B already)
    shr     rdi, 0x0A
    add     rdi, [C_VAR(g_card_bundle_table)]
    cmp     byte ptr [rdi], 0xFF
    je      LOCAL_LABEL(\BASENAME\()_NoBarrierRequired_\REFREG)

    mov     byte ptr [rdi], 0xFF
#endif

LOCAL_LABEL(\BASENAME\()_NoBarrierRequired_\REFREG):
    ret

.endm

// There are several different helpers used depending on which register holds the object reference. Since all
// the helpers have identical structure we use a macro to define this structure. One argument is taken, the
// name of the register that will hold the object reference (this should be in upper case as it's used in the
// definition of the name of the helper).
.macro DEFINE_UNCHECKED_WRITE_BARRIER REFREG, EXPORT_REG_NAME

// Define a helper with a name of the form RhpAssignRefEAX etc. (along with suitable calling standard
// decoration). The location to be updated is in DESTREG. The object reference that will be assigned into that
// location is in one of the other general registers determined by the value of REFREG.

// WARNING: Code in EHHelpers.cpp makes assumptions about write barrier code, in particular:
// - Function "InWriteBarrierHelper" assumes an AV due to passed in null pointer will happen on the first instruction
// - Function "UnwindSimpleHelperToCaller" assumes the stack contains just the pushed return address
LEAF_ENTRY RhpAssignRef\EXPORT_REG_NAME, _TEXT

    // Export the canonical write barrier under unqualified name as well
    .ifc \REFREG, RSI
    ALTERNATE_ENTRY RhpAssignRef
    ALTERNATE_ENTRY RhpAssignRefAVLocation
    .endif

    // Write the reference into the location. Note that we rely on the fact that no GC can occur between here
    // and the card table update we may perform below.
    mov     qword ptr [rdi], \REFREG

    DEFINE_UNCHECKED_WRITE_BARRIER_CORE RhpAssignRef, \REFREG

LEAF_END RhpAssignRef\EXPORT_REG_NAME, _TEXT
.endm

// One day we might have write barriers for all the possible argument registers but for now we have
// just one write barrier that assumes the input register is RSI.
DEFINE_UNCHECKED_WRITE_BARRIER RSI, ESI

//
// Define the helpers used to implement the write barrier required when writing an object reference into a
// location residing on the GC heap. Such write barriers allow the GC to optimize which objects in
// non-ephemeral generations need to be scanned for references to ephemeral objects during an ephemeral
// collection.
//

.macro DEFINE_CHECKED_WRITE_BARRIER_CORE BASENAME, REFREG

    // The location being updated might not even lie in the GC heap (a handle or stack location for instance),
    // in which case no write barrier is required.
    cmp     rdi, [C_VAR(g_lowest_address)]
    jb      LOCAL_LABEL(\BASENAME\()_NoBarrierRequired_\REFREG)
    cmp     rdi, [C_VAR(g_highest_address)]
    jae     LOCAL_LABEL(\BASENAME\()_NoBarrierRequired_\REFREG)

    DEFINE_UNCHECKED_WRITE_BARRIER_CORE \BASENAME, \REFREG

.endm

// There are several different helpers used depending on which register holds the object reference. Since all
// the helpers have identical structure we use a macro to define this structure. One argument is taken, the
// name of the register that will hold the object reference (this should be in upper case as it's used in the
// definition of the name of the helper).
.macro DEFINE_CHECKED_WRITE_BARRIER REFREG, EXPORT_REG_NAME

// Define a helper with a name of the form RhpCheckedAssignRefEAX etc. (along with suitable calling standard
// decoration). The location to be updated is always in RDI. The object reference that will be assigned into
// that location is in one of the other general registers determined by the value of REFREG.

// WARNING: Code in EHHelpers.cpp makes assumptions about write barrier code, in particular:
// - Function "InWriteBarrierHelper" assumes an AV due to passed in null pointer will happen on the first instruction
// - Function "UnwindSimpleHelperToCaller" assumes the stack contains just the pushed return address
LEAF_ENTRY RhpCheckedAssignRef\EXPORT_REG_NAME, _TEXT

    // Export the canonical write barrier under unqualified name as well
    .ifc \REFREG, RSI
    ALTERNATE_ENTRY RhpCheckedAssignRef
    ALTERNATE_ENTRY RhpCheckedAssignRefAVLocation
    .endif

    // Write the reference into the location. Note that we rely on the fact that no GC can occur between here
    // and the card table update we may perform below.
    mov     qword ptr [rdi], \REFREG

    DEFINE_CHECKED_WRITE_BARRIER_CORE RhpCheckedAssignRef, \REFREG

LEAF_END RhpCheckedAssignRef\EXPORT_REG_NAME, _TEXT
.endm

// One day we might have write barriers for all the possible argument registers but for now we have
// just one write barrier that assumes the input register is RSI.
DEFINE_CHECKED_WRITE_BARRIER RSI, ESI

LEAF_ENTRY RhpCheckedLockCmpXchg, _TEXT
    mov             rax, rdx
    lock cmpxchg    [rdi], rsi
    jne             LOCAL_LABEL(RhpCheckedLockCmpXchg_NoBarrierRequired_RSI)

    DEFINE_CHECKED_WRITE_BARRIER_CORE RhpCheckedLockCmpXchg, RSI

LEAF_END RhpCheckedLockCmpXchg, _TEXT

LEAF_ENTRY RhpCheckedXchg, _TEXT

    // Setup rax with the new object for the exchange, that way it will automatically hold the correct result
    // afterwards and we can leave rdx unaltered ready for the GC write barrier below.
    mov             rax, rsi
    xchg            [rdi], rax

    DEFINE_CHECKED_WRITE_BARRIER_CORE RhpCheckedXchg, RSI

LEAF_END RhpCheckedXchg, _TEXT

//
// RhpByRefAssignRef simulates movs instruction for object references.
//
// On entry:
//      rdi: address of ref-field (assigned to)
//      rsi: address of the data (source)
//
// On exit:
//      rdi, rsi are incremented by 8,
//      rcx, rax: trashed
//
// NOTE: Keep in sync with RBM_CALLEE_TRASH_WRITEBARRIER_BYREF and RBM_CALLEE_GCTRASH_WRITEBARRIER_BYREF
//       if you add more trashed registers.
//
// WARNING: Code in EHHelpers.cpp makes assumptions about write barrier code, in particular:
// - Function "InWriteBarrierHelper" assumes an AV due to passed in null pointer will happen at RhpByRefAssignRefAVLocation1/2
// - Function "UnwindSimpleHelperToCaller" assumes the stack contains just the pushed return address
LEAF_ENTRY RhpByRefAssignRef, _TEXT
ALTERNATE_ENTRY RhpByRefAssignRefAVLocation1
    mov     rcx, [rsi]
ALTERNATE_ENTRY RhpByRefAssignRefAVLocation2
    mov     [rdi], rcx

    // Check whether the writes were even into the heap. If not there's no card update required.
    cmp     rdi, [C_VAR(g_lowest_address)]
    jb      LOCAL_LABEL(RhpByRefAssignRef_NoBarrierRequired)
    cmp     rdi, [C_VAR(g_highest_address)]
    jae     LOCAL_LABEL(RhpByRefAssignRef_NoBarrierRequired)

    // Update the shadow copy of the heap with the same value just written to the same heap. (A no-op unless
    // we're in a debug build and write barrier checking has been enabled).
    UPDATE_GC_SHADOW BASENAME, rcx, rdi

#ifdef FEATURE_USE_SOFTWARE_WRITE_WATCH_FOR_GC_HEAP
    cmp     qword ptr [C_VAR(g_write_watch_table)], 0x0
    je      LOCAL_LABEL(RhpByRefAssignRef_CheckCardTable)

    mov     rax, rdi
    shr     rax, 0xC // SoftwareWriteWatch::AddressToTableByteIndexShift
    add     rax, [C_VAR(g_write_watch_table)]
    cmp     byte ptr [rax], 0x0
    jne     LOCAL_LABEL(RhpByRefAssignRef_CheckCardTable)
    mov     byte ptr [rax], 0xFF
#endif

LOCAL_LABEL(RhpByRefAssignRef_CheckCardTable):

    // If the reference is to an object that's not in an ephemeral generation we have no need to track it
    // (since the object won't be collected or moved by an ephemeral collection).
    cmp     rcx, [C_VAR(g_ephemeral_low)]
    jb      LOCAL_LABEL(RhpByRefAssignRef_NoBarrierRequired)
    cmp     rcx, [C_VAR(g_ephemeral_high)]
    jae     LOCAL_LABEL(RhpByRefAssignRef_NoBarrierRequired)

    // move current rdi value into rcx, we need to keep rdi and eventually increment by 8
    mov     rcx, rdi

    // We have a location on the GC heap being updated with a reference to an ephemeral object so we must
    // track this write. The location address is translated into an offset in the card table bitmap. We set
    // an entire byte in the card table since it's quicker than messing around with bitmasks and we only write
    // the byte if it hasn't already been done since writes are expensive and impact scaling.
    shr     rcx, 0x0B
    mov     rax, [C_VAR(g_card_table)]
    cmp     byte ptr [rcx + rax], 0x0FF
    je      LOCAL_LABEL(RhpByRefAssignRef_NoBarrierRequired)

// We get here if it's necessary to update the card table.
    mov     byte ptr [rcx + rax], 0xFF

#ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
    // Shift rcx by 0x0A more to get the card bundle byte (we shifted by 0x0B already)
    shr     rcx, 0x0A
    add     rcx, [C_VAR(g_card_bundle_table)]
    cmp     byte ptr [rcx], 0xFF
    je      LOCAL_LABEL(RhpByRefAssignRef_NoBarrierRequired)

    mov     byte ptr [rcx], 0xFF
#endif

LOCAL_LABEL(RhpByRefAssignRef_NoBarrierRequired):
    // Increment the pointers before leaving
    add     rdi, 0x8
    add     rsi, 0x8
    ret
LEAF_END RhpByRefAssignRef, _TEXT

#else  //FEATURE_SATORI_GC   ######################################################

//
//   rdi - dest address 
//   rsi - object
//
LEAF_ENTRY RhpCheckedAssignRef, _TEXT
        // See if dst is in GCHeap
        mov     rax, [C_VAR(g_card_bundle_table)] // fetch the page byte map
        mov     r8, rdi
        shr     r8,  30                           // dst page index
        cmp     byte ptr [rax + r8], 0
        jne     C_FUNC(CheckedEntry)

NotInHeap:
ALTERNATE_ENTRY RhpCheckedAssignRefAVLocation
    mov     [rdi], rsi
    ret
LEAF_END RhpCheckedAssignRef, _TEXT

//
//   rdi - dest address 
//   rsi - object
//
LEAF_ENTRY RhpAssignRef, _TEXT

#ifdef FEATURE_SATORI_EXTERNAL_OBJECTS
    // check if src is in heap
        mov     rax, [C_VAR(g_card_bundle_table)] // fetch the page byte map

    ALTERNATE_ENTRY CheckedEntry
        mov     r8,  rsi
        shr     r8,  30                           // src page index
        cmp     byte ptr [rax + r8], 0
        je      JustAssign                        // src not in heap
#else
    ALTERNATE_ENTRY CheckedEntry
#endif

    // check for escaping assignment
    // 1) check if we own the source region
        mov     rdx, rsi
        and     rdx, 0xFFFFFFFFFFE00000  // source region

#ifndef FEATURE_SATORI_EXTERNAL_OBJECTS
        jz      JustAssign              // assigning null
#endif

#ifdef TARGET_OSX
        mov     rax,  gs:[0]            // thread tag
#else
        mov     rax,  fs:[0]            // thread tag
#endif
        cmp     qword ptr [rdx], rax     
        jne     AssignAndMarkCards      // no card marking, src is not a heap object

    // 2) check if the src and dst are from the same region
        mov     rax, rdi
        and     rax, 0xFFFFFFFFFFE00000
        cmp     rax, rdx
        jnz     RecordEscape            // cross region assignment. definitely escaping

    // 3) check if the target is exposed
        mov     rax, rdi
        and     rax, 0x1FFFFF
        shr     rax, 3
        bt      qword ptr [rdx], rax
        jb      RecordEscape            // target is exposed. record an escape.

    JustAssign:
ALTERNATE_ENTRY RhpAssignRefAVLocationNotHeap
        mov     [rdi], rsi              // no card marking, src is not a heap object
        // set rdi, rsi per contract with JIT_ByRefWriteBarrier
        add     rdi, 8
        mov     rsi, r10
        ret

    AssignAndMarkCards:
ALTERNATE_ENTRY RhpAssignRefAVLocation
        mov     [rdi], rsi

        // TUNING: barriers in different modes could be separate pieces of code, but barrier switch 
        //         needs to suspend EE, not sure if skipping mode check would worth that much.
        mov     r11, [C_VAR(g_write_watch_table)]

        // set rdi per contract with JIT_ByRefWriteBarrier
        mov     rax, rdi
        add     rdi, 8

        // check the barrier state. this must be done after the assignment (in program order)
        // if state == 2 we do not set or dirty cards.
        cmp     r11, 2
        jne     DoCards
        // set rsi per contract with JIT_ByRefWriteBarrier
        mov     rsi, r10
    Exit:
        ret

    DoCards:
    // if same region, just check if barrier is not concurrent
        xor     rsi, rax
        shr     rsi, 21
        // set rsi per contract with JIT_ByRefWriteBarrier
        mov     rsi, r10
        jz      CheckConcurrent         // same region, just check if barrier is not concurrent

    // if src is in gen2/3 and the barrier is not concurrent we do not need to mark cards
        cmp     dword ptr [rdx + 16], 2
        jl      MarkCards

    CheckConcurrent:
    // if concurrent, load card location
        cmp     r11, 0
        je      Exit

    MarkCards:
        mov     r9 , [C_VAR(g_card_table)]     // fetch the page map
        mov     rdx, rax
        shr     rax, 30
        mov     rax, qword ptr [r9 + rax * 8] // page
        sub     rdx, rax   // offset in page
        mov     r8,  rdx
        shr     rdx, 9     // card offset
        shr     r8,  20    // group index
        lea     r8,  [rax + r8 * 2 + 0x80] // group offset

     // check if concurrent marking is in progress
        cmp     r11, 0
        jne     DirtyCard

     // SETTING CARD
     SetCard:
        cmp     byte ptr [rax + rdx], 0
        jne     Exit
        mov     byte ptr [rax + rdx], 1
     SetGroup:
        cmp     byte ptr [r8], 0
        jne     CardSet
        mov     byte ptr [r8], 1
     SetPage:
        cmp     byte ptr [rax], 0
        jne     CardSet
        mov     byte ptr [rax], 1

     CardSet:
     // check if concurrent marking is still not in progress
        cmp     qword ptr [C_VAR(g_write_watch_table)], 0
        jne     DirtyCard
        ret

     // DIRTYING CARD
     DirtyCard:
        mov     byte ptr [rax + rdx], 4
     DirtyGroup:
        cmp     byte ptr [r8], 4
        je      Exit
        mov     byte ptr [r8], 4
     DirtyPage:
        cmp     byte ptr [rax], 4
        je      Exit
        mov     byte ptr [rax], 4
        ret

    // this is expected to be rare.
    RecordEscape:

        // 4) check if the source is escaped
        mov     rax, rsi
        add     rax, 8                             // escape bit is MT + 1
        and     rax, 0x1FFFFF
        shr     rax, 3
        bt      qword ptr [rdx], rax
        jb      AssignAndMarkCards            // source is already escaped.

        // Align rsp
        mov  r9, rsp
        and  rsp, -16
        sub  rsp, 8

        // save rsp, rdi, rsi, rdx and r10 (possibly preadjusted rsi)
        push r9
        push rdi
        push rsi
        push rdx
        push r10

        // also save xmm0, in case it is used for stack clearing, as JIT_ByRefWriteBarrier should not trash xmm0
        // Hopefully EscapeFn cannot corrupt other xmm regs, since there is no float math or vectorizable code in there.
        sub     rsp, 16
        movdqa  [rsp], xmm0

        // void SatoriRegion::EscapeFn(SatoriObject** dst, SatoriObject* src, SatoriRegion* region)
        call    qword ptr [rdx + 8]

        movdqa  xmm0, [rsp]
        add     rsp, 16

        pop     r10
        pop     rdx
        pop     rsi
        pop     rdi
        pop     rsp
        jmp     AssignAndMarkCards
LEAF_END RhpAssignRef, _TEXT

//
// RhpByRefAssignRef simulates movs instruction for object references.
//
// Entry:
//   RDI - address of ref-field (assigned to)
//   RSI - address of the data  (source)
// Exit:
//   RCX is trashed
//   RAX is trashed
//   RDI, RSI are incremented by SIZEOF(LPVOID)
LEAF_ENTRY RhpByRefAssignRef, _TEXT
     // See if dst is in GCHeap
        mov     rax, [C_VAR(g_card_bundle_table)] // fetch the page byte map
        mov     rcx,  rdi
        shr     rcx,  30                    // dst page index
        cmp     byte ptr [rax + rcx], 0
        jne     InHeap

ALTERNATE_ENTRY RhpByRefAssignRefAVLocation1
        mov     rcx, [rsi]
ALTERNATE_ENTRY RhpByRefAssignRefAVLocation2
        mov     [rdi], rcx
        add     rdi, 8
        add     rsi, 8
        ret

    InHeap:
        // JIT_WriteBarrier may trash these registers 
        push    rdx
        push    r8
        push    r9
        push    r10
        push    r11

        // save preadjusted rsi
        lea     r10, [rsi + 8]
        mov     rsi, [rsi]

        call    CheckedEntry

        pop     r11
        pop     r10
        pop     r9
        pop     r8
        pop     rdx
        ret
LEAF_END RhpByRefAssignRef, _TEXT

LEAF_ENTRY RhpCheckedLockCmpXchg, _TEXT
    // Setup rax with the new object for the exchange, that way it will automatically hold the correct result
    // afterwards and we can leave rsi unaltered ready for the GC write barrier below.
        mov     rax, rdx
        mov     r11, [C_VAR(g_card_bundle_table)] // fetch the page byte map

    // check if dst is in heap
        mov     rdx, rdi
        shr     rdx, 30                    // round to page size ( >> PAGE_BITS )
        cmp     byte ptr [r11 + rdx], 0
        je      JustAssign_CmpXchg              // dst not in heap

    // check for escaping assignment
    // 1) check if we own the source region
#ifdef FEATURE_SATORI_EXTERNAL_OBJECTS
        mov     rdx, rsi
        shr     rdx, 30                  // round to page size ( >> PAGE_BITS )
        cmp     byte ptr [r11 + rdx], 0
        je      JustAssign_CmpXchg              // src not in heap
#endif

        mov     rdx, rsi
        and     rdx, 0xFFFFFFFFFFE00000  // source region

#ifndef FEATURE_SATORI_EXTERNAL_OBJECTS
        jz      JustAssign_CmpXchg              // assigning null
#endif

#ifdef TARGET_OSX
        mov     r11,  gs:[0]            // thread tag
#else
        mov     r11,  fs:[0]            // thread tag
#endif
        cmp     qword ptr [rdx], r11     
        jne     AssignAndMarkCards_CmpXchg      // not local to this thread

    // 2) check if the src and dst are from the same region
        mov     r11, rdi
        and     r11, 0xFFFFFFFFFFE00000
        cmp     r11, rdx
        jnz     RecordEscape_CmpXchg            // cross region assignment. definitely escaping

    // 3) check if the target is exposed
        mov     r11, rdi
        and     r11, 0x1FFFFF
        shr     r11, 3
        bt      qword ptr [rdx], r11
        jb      RecordEscape_CmpXchg            // target is exposed. record an escape.

    JustAssign_CmpXchg:
        lock cmpxchg    [rdi], rsi     // no card marking, src is not a heap object
        ret

    AssignAndMarkCards_CmpXchg:
        lock cmpxchg    [rdi], rsi
        jne             Exit_CmpXchg

        // TUNING: barriers in different modes could be separate pieces of code, but barrier switch 
        //         needs to suspend EE, not sure if skipping mode check would worth that much.
        mov     r10, [C_VAR(g_write_watch_table)]

        // check the barrier state. this must be done after the assignment (in program order)
        // if state == 2 we do not set or dirty cards.
        cmp     r10, 2
        jne     DoCards_CmpXchg
    Exit_CmpXchg:
        ret

    DoCards_CmpXchg:
    // if same region, just check if barrier is not concurrent
        xor     rsi, rdi
        shr     rsi, 21
        jz      CheckConcurrent_CmpXchg         // same region, just check if barrier is not concurrent

    // if src is in gen2/3 and the barrier is not concurrent we do not need to mark cards
        cmp     dword ptr [rdx + 16], 2
        jl      MarkCards_CmpXchg

    CheckConcurrent_CmpXchg:
    // if concurrent, load card location
        cmp     r10, 0
        jne     MarkCards_CmpXchg
        ret

    MarkCards_CmpXchg:
    // fetch card location for rdi
        mov     r9 , [C_VAR(g_card_table)]     // fetch the page map
        mov     rdx,  rdi
        shr     rdi, 30
        mov     r11, qword ptr [r9 + rdi * 8] // page
        sub     rdx, r11   // offset in page
        mov     rsi,rdx
        shr     rdx, 9     // card offset
        shr     rsi, 20    // group index
        lea     rsi, [r11 + rsi * 2 + 0x80] // group offset

     // check if concurrent marking is in progress
        cmp     r10, 0
        jne     DirtyCard_CmpXchg

    // SETTING CARD FOR rdi
     SetCard_CmpXchg:
        cmp     byte ptr [r11 + rdx], 0
        jne     Exit_CmpXchg
        mov     byte ptr [r11 + rdx], 1
     SetGroup_CmpXchg:
        cmp     byte ptr [rsi], 0
        jne     CardSet_CmpXchg
        mov     byte ptr [rsi], 1
     SetPage_CmpXchg:
        cmp     byte ptr [r11], 0
        jne     CardSet_CmpXchg
        mov     byte ptr [r11], 1

     CardSet_CmpXchg:
     // check if concurrent marking is still not in progress
        cmp     qword ptr [C_VAR(g_write_watch_table)], 0
        jne     DirtyCard_CmpXchg
        ret

    // DIRTYING CARD FOR rdi
     DirtyCard_CmpXchg:
        cmp     byte ptr [r11 + rdx], 4
        je      Exit_CmpXchg
        mov     byte ptr [r11 + rdx], 4
     DirtyGroup_CmpXchg:
        cmp     byte ptr [rsi], 4
        je      Exit_CmpXchg
        mov     byte ptr [rsi], 4
     DirtyPage_CmpXchg:
        cmp     byte ptr [r11], 4
        je      Exit_CmpXchg
        mov     byte ptr [r11], 4
        ret

    // this is expected to be rare.
    RecordEscape_CmpXchg:

        // 4) check if the source is escaped
        mov     r11, rsi
        add     r11, 8                             // escape bit is MT + 1
        and     r11, 0x1FFFFF
        shr     r11, 3
        bt      qword ptr [rdx], r11
        jb      AssignAndMarkCards_CmpXchg            // source is already escaped.

        // Align rsp
        mov  r9, rsp
        and  rsp, -16
        sub  rsp, 8

        // save rsp, rax, rdi, rsi, rdx and have enough stack for the callee
        push r9
        push rax
        push rdi
        push rsi
        push rdx

        // void SatoriRegion::EscapeFn(SatoriObject** dst, SatoriObject* src, SatoriRegion* region)
        call    qword ptr [rdx + 8]

        pop     rdx
        pop     rsi
        pop     rdi
        pop     rax
        pop     rsp
        jmp     AssignAndMarkCards_CmpXchg
LEAF_END RhpCheckedLockCmpXchg, _TEXT

LEAF_ENTRY RhpCheckedXchg, _TEXT
    // Setup rax with the new object for the exchange, that way it will automatically hold the correct result
    // afterwards and we can leave rsi unaltered ready for the GC write barrier below.
        mov         rax, rsi
        mov         r11, [C_VAR(g_card_bundle_table)] // fetch the page byte map

        // check if dst is in heap
        mov     rdx, rdi
        shr     rdx, 30                    // round to page size ( >> PAGE_BITS )
        cmp     byte ptr [r11 + rdx], 0
        je      JustAssign_Xchg              // dst not in heap

    // check for escaping assignment
    // 1) check if we own the source region
#ifdef FEATURE_SATORI_EXTERNAL_OBJECTS
        mov     rdx, rsi
        shr     rdx, 30                 // round to page size ( >> PAGE_BITS )
        cmp     byte ptr [r11 + rdx], 0
        je      JustAssign_Xchg              // src not in heap
#endif

        mov     rdx, rsi
        and     rdx, 0xFFFFFFFFFFE00000  // source region

#ifndef FEATURE_SATORI_EXTERNAL_OBJECTS
        jz      JustAssign_Xchg              // assigning null
#endif
#ifdef TARGET_OSX
        mov     r11,  gs:[0]            // thread tag
#else
        mov     r11,  fs:[0]            // thread tag
#endif
        cmp     qword ptr [rdx], r11     
        jne     AssignAndMarkCards_Xchg      // not local to this thread

    // 2) check if the src and dst are from the same region
        mov     r11, rdi
        and     r11, 0xFFFFFFFFFFE00000
        cmp     r11, rdx
        jnz     RecordEscape_Xchg            // cross region assignment. definitely escaping

    // 3) check if the target is exposed
        mov     r11, rdi
        and     r11, 0x1FFFFF
        shr     r11, 3
        bt      qword ptr [rdx], r11
        jb      RecordEscape_Xchg            // target is exposed. record an escape.

    JustAssign_Xchg:
        xchg    [rdi], rax              // no card marking, src is not a heap object
        ret

    AssignAndMarkCards_Xchg:
        xchg    [rdi], rax

        // TUNING: barriers in different modes could be separate pieces of code, but barrier switch 
        //         needs to suspend EE, not sure if skipping mode check would worth that much.
        mov     r10, [C_VAR(g_write_watch_table)]

        // check the barrier state. this must be done after the assignment (in program order)
        // if state == 2 we do not set or dirty cards.
        cmp     r10, 2
        jne     DoCards_Xchg
    Exit_Xchg:
        ret

    DoCards_Xchg:
    // if same region, just check if barrier is not concurrent
        xor     rsi, rdi
        shr     rsi, 21
        jz      CheckConcurrent_Xchg         // same region, just check if barrier is not concurrent

    // if src is in gen2/3 and the barrier is not concurrent we do not need to mark cards
        cmp     dword ptr [rdx + 16], 2
        jl      MarkCards_Xchg

    CheckConcurrent_Xchg:
    // if concurrent, load card location
        cmp     r10, 0
        jne     MarkCards_Xchg
        ret

    MarkCards_Xchg:
    // fetch card location for rdi
        mov     r9 , [C_VAR(g_card_table)]     // fetch the page map
        mov     rdx,  rdi
        shr     rdi, 30
        mov     r11, qword ptr [r9 + rdi * 8] // page
        sub     rdx, r11   // offset in page
        mov     rsi,rdx
        shr     rdx, 9     // card offset
        shr     rsi, 20    // group index
        lea     rsi, [r11 + rsi * 2 + 0x80] // group offset

     // check if concurrent marking is in progress
        cmp     r10, 0
        jne     DirtyCard_Xchg

    // SETTING CARD FOR rdi
     SetCard_Xchg:
        cmp     byte ptr [r11 + rdx], 0
        jne     Exit_Xchg
        mov     byte ptr [r11 + rdx], 1
     SetGroup_Xchg:
        cmp     byte ptr [rsi], 0
        jne     CardSet_Xchg
        mov     byte ptr [rsi], 1
     SetPage_Xchg:
        cmp     byte ptr [r11], 0
        jne     CardSet_Xchg
        mov     byte ptr [r11], 1

     CardSet_Xchg:
     // check if concurrent marking is still not in progress
        cmp     qword ptr [C_VAR(g_write_watch_table)], 0
        jne     DirtyCard_Xchg
        ret

    // DIRTYING CARD FOR rdi
     DirtyCard_Xchg:
        cmp     byte ptr [r11 + rdx], 4
        je      Exit_Xchg
        mov     byte ptr [r11 + rdx], 4
     DirtyGroup_Xchg:
        cmp     byte ptr [rsi], 4
        je      Exit_Xchg
        mov     byte ptr [rsi], 4
     DirtyPage_Xchg:
        cmp     byte ptr [r11], 4
        je      Exit_Xchg
        mov     byte ptr [r11], 4
        ret

    // this is expected to be rare.
    RecordEscape_Xchg:

        // 4) check if the source is escaped
        mov     r11, rsi
        add     r11, 8                             // escape bit is MT + 1
        and     r11, 0x1FFFFF
        shr     r11, 3
        bt      qword ptr [rdx], r11
        jb      AssignAndMarkCards_Xchg            // source is already escaped.

        // Align rsp
        mov  r9, rsp
        and  rsp, -16
        sub  rsp, 8

        // save rsp, rax, rdi, rsi, rdx and have enough stack for the callee
        push r9
        push rax
        push rdi
        push rsi
        push rdx

        // void SatoriRegion::EscapeFn(SatoriObject** dst, SatoriObject* src, SatoriRegion* region)
        call    qword ptr [rdx + 8]

        pop     rdx
        pop     rsi
        pop     rdi
        pop     rax
        pop     rsp
        jmp     AssignAndMarkCards_Xchg
LEAF_END RhpCheckedXchg, _TEXT


#endif  // FEATURE_SATORI_GC
