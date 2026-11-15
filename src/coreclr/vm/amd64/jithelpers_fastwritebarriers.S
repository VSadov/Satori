// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

.intel_syntax noprefix
#include "unixasmmacros.inc"


        .balign 16
LEAF_ENTRY JIT_WriteBarrier_PreGrow64, _TEXT
        // Do the move into the GC .  It is correct to take an AV here, the EH code
        // figures out that this came from a WriteBarrier and correctly maps it back
        // to the managed method which called the WriteBarrier (see setup in
        // InitializeExceptionHandling, vm\exceptionhandling.cpp).
        mov     [rdi], rsi

        NOP_3_BYTE // padding for alignment of constant

        // Can't compare a 64 bit immediate, so we have to move it into a
        // register.  Value of this immediate will be patched at runtime.
PATCH_LABEL JIT_WriteBarrier_PreGrow64_Patch_Label_Lower
        movabs  rax, 0xF0F0F0F0F0F0F0F0

        // Check the lower ephemeral region bound.
        cmp     rsi, rax

#ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
        .byte 0x72, 0x4B
#else
        .byte 0x72, 0x2b
#endif
        // jb      Exit_PreGrow64

        nop // padding for alignment of constant

PATCH_LABEL JIT_WriteBarrier_PreGrow64_Patch_Label_CardTable
        movabs  rax, 0xF0F0F0F0F0F0F0F0

        // Touch the card table entry, if not already dirty.
        shr     rdi, 0x0B
        cmp     byte ptr [rdi + rax], 0xFF
        .byte 0x75, 0x02
        // jne     LOCAL_LABEL(UpdateCardTable_PreGrow64)
        REPRET

    LOCAL_LABEL(UpdateCardTable_PreGrow64):
        mov     byte ptr [rdi + rax], 0xFF

#ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
        NOP_6_BYTE // padding for alignment of constant

PATCH_LABEL JIT_WriteBarrier_PreGrow64_Patch_Label_CardBundleTable
        movabs  rax, 0xF0F0F0F0F0F0F0F0

        // Touch the card bundle, if not already dirty.
        // rdi is already shifted by 0xB, so shift by 0xA more
        shr     rdi, 0x0A
        cmp     byte ptr [rdi + rax], 0xFF

        .byte 0x75, 0x02
        // jne     LOCAL_LABEL(UpdateCardBundle_PreGrow64)
        REPRET

    LOCAL_LABEL(UpdateCardBundle_PreGrow64):
        mov     byte ptr [rdi + rax], 0xFF
#endif

        ret

    .balign 16
PATCH_LABEL Exit_PreGrow64
        REPRET
LEAF_END_MARKED JIT_WriteBarrier_PreGrow64, _TEXT


        .balign 16
// See comments for JIT_WriteBarrier_PreGrow (above).
LEAF_ENTRY JIT_WriteBarrier_PostGrow64, _TEXT
        // Do the move into the GC .  It is correct to take an AV here, the EH code
        // figures out that this came from a WriteBarrier and correctly maps it back
        // to the managed method which called the WriteBarrier (see setup in
        // InitializeExceptionHandling, vm\exceptionhandling.cpp).
        mov     [rdi], rsi

        NOP_3_BYTE // padding for alignment of constant

        // Can't compare a 64 bit immediate, so we have to move them into a
        // register.  Values of these immediates will be patched at runtime.
        // By using two registers we can pipeline better.  Should we decide to use
        // a special non-volatile calling convention, this should be changed to
        // just one.
PATCH_LABEL JIT_WriteBarrier_PostGrow64_Patch_Label_Lower
        movabs  rax, 0xF0F0F0F0F0F0F0F0

        // Check the lower and upper ephemeral region bounds
        cmp     rsi, rax

#ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
        .byte 0x72, 0x5b
#else
        .byte 0x72, 0x3b
#endif
        // jb      Exit_PostGrow64

        nop // padding for alignment of constant

PATCH_LABEL JIT_WriteBarrier_PostGrow64_Patch_Label_Upper
        movabs  r8, 0xF0F0F0F0F0F0F0F0

        cmp     rsi, r8

#ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
        .byte 0x73, 0x4b
#else
        .byte 0x73, 0x2b
#endif
        // jae     Exit_PostGrow64

        nop // padding for alignment of constant

PATCH_LABEL JIT_WriteBarrier_PostGrow64_Patch_Label_CardTable
        movabs  rax, 0xF0F0F0F0F0F0F0F0

        // Touch the card table entry, if not already dirty.
        shr     rdi, 0x0B
        cmp     byte ptr [rdi + rax], 0xFF
        .byte 0x75, 0x02
        // jne     LOCAL_LABEL(UpdateCardTable_PostGrow64)
        REPRET

    LOCAL_LABEL(UpdateCardTable_PostGrow64):
        mov     byte ptr [rdi + rax], 0xFF

#ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
        NOP_6_BYTE // padding for alignment of constant

PATCH_LABEL JIT_WriteBarrier_PostGrow64_Patch_Label_CardBundleTable
        movabs  rax, 0xF0F0F0F0F0F0F0F0

        // Touch the card bundle, if not already dirty.
        // rdi is already shifted by 0xB, so shift by 0xA more
        shr     rdi, 0x0A
        cmp     byte ptr [rdi + rax], 0xFF

        .byte 0x75, 0x02
        // jne     LOCAL_LABEL(UpdateCardBundle_PostGrow64)
        REPRET

    LOCAL_LABEL(UpdateCardBundle_PostGrow64):
        mov     byte ptr [rdi + rax], 0xFF
#endif

        ret

    .balign 16
PATCH_LABEL Exit_PostGrow64
        REPRET
LEAF_END_MARKED JIT_WriteBarrier_PostGrow64, _TEXT


#ifdef FEATURE_SVR_GC

        .balign 16
LEAF_ENTRY JIT_WriteBarrier_SVR64, _TEXT
        //
        // SVR GC has multiple heaps, so it cannot provide one single
        // ephemeral region to bounds check against, so we just skip the
        // bounds checking all together and do our card table update
        // unconditionally.
        //

        // Do the move into the GC .  It is correct to take an AV here, the EH code
        // figures out that this came from a WriteBarrier and correctly maps it back
        // to the managed method which called the WriteBarrier (see setup in
        // InitializeExceptionHandling, vm\exceptionhandling.cpp).
        mov     [rdi], rsi

        NOP_3_BYTE // padding for alignment of constant

PATCH_LABEL JIT_WriteBarrier_SVR64_PatchLabel_CardTable
        movabs  rax, 0xF0F0F0F0F0F0F0F0

        shr     rdi, 0x0B

        cmp     byte ptr [rdi + rax], 0xFF
        .byte 0x75, 0x02
        // jne     LOCAL_LABEL(UpdateCardTable_SVR64)
        REPRET

    LOCAL_LABEL(UpdateCardTable_SVR64):
        mov     byte ptr [rdi + rax], 0xFF

#ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
        NOP_6_BYTE // padding for alignment of constant

PATCH_LABEL JIT_WriteBarrier_SVR64_PatchLabel_CardBundleTable
        movabs  rax, 0xF0F0F0F0F0F0F0F0

        // Shift the address by 0xA more since already shifted by 0xB
        shr     rdi, 0x0A
        cmp     byte ptr [rdi + rax], 0xFF

        .byte 0x75, 0x02
        // jne     LOCAL_LABEL(UpdateCardBundle_SVR64)
        REPRET

    LOCAL_LABEL(UpdateCardBundle_SVR64):
        mov     byte ptr [rdi + rax], 0xFF
#endif

        ret

LEAF_END_MARKED JIT_WriteBarrier_SVR64, _TEXT

#endif

        .balign 16
LEAF_ENTRY JIT_WriteBarrier_Byte_Region64, _TEXT

        // Do the move into the GC .  It is correct to take an AV here, the EH code
        // figures out that this came from a WriteBarrier and correctly maps it back
        // to the managed method which called the WriteBarrier (see setup in
        // InitializeExceptionHandling, vm\exceptionhandling.cpp).
        mov     [rdi], rsi

        mov     r8, rdi

PATCH_LABEL JIT_WriteBarrier_Byte_Region64_Patch_Label_RegionToGeneration
        movabs  rax, 0xF0F0F0F0F0F0F0F0

PATCH_LABEL JIT_WriteBarrier_Byte_Region64_Patch_Label_RegionShrDest
        shr     rdi, 0x16 // compute region index

        // Check whether the region we're storing into is gen 0 - nothing to do in this case
        cmp     byte ptr [rdi + rax], 0
        .byte 0x75, 0x04
        //jne     LOCAL_LABEL(NotGen0_Byte_Region64)
        REPRET

        NOP_2_BYTE // padding for alignment of constant

    LOCAL_LABEL(NotGen0_Byte_Region64):
PATCH_LABEL JIT_WriteBarrier_Byte_Region64_Patch_Label_Lower
        movabs  r9, 0xF0F0F0F0F0F0F0F0
        cmp     rsi, r9
        .byte 0x73, 0x01
        // jae     LOCAL_LABEL(NotLow_Byte_Region64)
        ret
    LOCAL_LABEL(NotLow_Byte_Region64):
PATCH_LABEL JIT_WriteBarrier_Byte_Region64_Patch_Label_Upper
        movabs  r9, 0xF0F0F0F0F0F0F0F0
        cmp     rsi, r9
        .byte 0x72, 0x02
        // jb      LOCAL_LABEL(NotHigh_Byte_Region64)
        REPRET
    LOCAL_LABEL(NotHigh_Byte_Region64):
PATCH_LABEL JIT_WriteBarrier_Byte_Region64_Patch_Label_RegionShrSrc
        shr     rsi, 0x16 // compute region index
        mov     dl, [rsi + rax]
        cmp     dl, [rdi + rax]
        .byte 0x72, 0x03
        // jb      LOCAL_LABEL(IsOldToYoung_Byte_Region64)
        REPRET
        nop

    LOCAL_LABEL(IsOldToYoung_Byte_Region64):
PATCH_LABEL JIT_WriteBarrier_Byte_Region64_Patch_Label_CardTable
        movabs  rax, 0xF0F0F0F0F0F0F0F0

        shr     r8, 0xB
        cmp     byte ptr [r8 + rax], 0xFF
        .byte 0x75, 0x02
        // jne      LOCAL_LABEL(UpdateCardTable_Byte_Region64)
        REPRET

    LOCAL_LABEL(UpdateCardTable_Byte_Region64):
        mov     byte ptr [r8 + rax], 0xFF
#ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
        shr     r8, 0x0A
PATCH_LABEL JIT_WriteBarrier_Byte_Region64_Patch_Label_CardBundleTable
        movabs  rax, 0xF0F0F0F0F0F0F0F0
        cmp     byte ptr [r8 + rax], 0xFF
        .byte 0x75, 0x02
        // jne     LOCAL_LABEL(UpdateCardBundleTable_Byte_Region64)
        REPRET

    LOCAL_LABEL(UpdateCardBundleTable_Byte_Region64):
        mov     byte ptr [r8 + rax], 0xFF
#endif
        ret
LEAF_END_MARKED JIT_WriteBarrier_Byte_Region64, _TEXT
        .balign 16
LEAF_ENTRY JIT_WriteBarrier_Bit_Region64, _TEXT

        // Do the move into the GC .  It is correct to take an AV here, the EH code
        // figures out that this came from a WriteBarrier and correctly maps it back
        // to the managed method which called the WriteBarrier (see setup in
        // InitializeExceptionHandling, vm\exceptionhandling.cpp).
        mov     [rdi], rsi

        mov     r8, rdi

PATCH_LABEL JIT_WriteBarrier_Bit_Region64_Patch_Label_RegionToGeneration
        movabs  rax, 0xF0F0F0F0F0F0F0F0

PATCH_LABEL JIT_WriteBarrier_Bit_Region64_Patch_Label_RegionShrDest
        shr     rdi, 0x16 // compute region index

        // Check whether the region we're storing into is gen 0 - nothing to do in this case
        cmp     byte ptr [rdi + rax], 0
        .byte 0x75, 0x04
        //jne     LOCAL_LABEL(NotGen0_Bit_Region64)
        REPRET

        NOP_2_BYTE // padding for alignment of constant

    LOCAL_LABEL(NotGen0_Bit_Region64):
PATCH_LABEL JIT_WriteBarrier_Bit_Region64_Patch_Label_Lower
        movabs  r9, 0xF0F0F0F0F0F0F0F0
        cmp     rsi, r9
        .byte 0x73, 0x01
        // jae     LOCAL_LABEL(NotLow_Bit_Region64)
        ret
    LOCAL_LABEL(NotLow_Bit_Region64):
PATCH_LABEL JIT_WriteBarrier_Bit_Region64_Patch_Label_Upper
        movabs  r9, 0xF0F0F0F0F0F0F0F0
        cmp     rsi, r9
        .byte 0x72, 0x02
        // jb      LOCAL_LABEL(NotHigh_Bit_Region64)
        REPRET
    LOCAL_LABEL(NotHigh_Bit_Region64):
PATCH_LABEL JIT_WriteBarrier_Bit_Region64_Patch_Label_RegionShrSrc
        shr     rsi, 0x16 // compute region index
        mov     dl, [rsi + rax]
        cmp     dl, [rdi + rax]
        .byte 0x72, 0x03
        // jb      LOCAL_LABEL(IsOldToYoung_Bit_Region64)
        REPRET
        nop

    LOCAL_LABEL(IsOldToYoung_Bit_Region64):
PATCH_LABEL JIT_WriteBarrier_Bit_Region64_Patch_Label_CardTable
        movabs  rax, 0xF0F0F0F0F0F0F0F0

        mov     ecx, r8d
        shr     r8, 0xB
        shr     ecx, 8
        and     ecx, 7
        mov     dl, 1
        shl     dl, cl
        test    byte ptr [r8 + rax], dl
        .byte 0x74, 0x02
        // je      LOCAL_LABEL(UpdateCardTable_Bit_Region64)
        REPRET

    LOCAL_LABEL(UpdateCardTable_Bit_Region64):
        lock or byte ptr [r8 + rax], dl
#ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
PATCH_LABEL JIT_WriteBarrier_Bit_Region64_Patch_Label_CardBundleTable
        movabs  rax, 0xF0F0F0F0F0F0F0F0
        shr     r8, 0x0A
        cmp     byte ptr [r8 + rax], 0xFF
        .byte 0x75, 0x02
        // jne     LOCAL_LABEL(UpdateCardBundleTable_Bit_Region64)
        REPRET

    LOCAL_LABEL(UpdateCardBundleTable_Bit_Region64):
        mov     byte ptr [r8 + rax], 0xFF
#endif
        ret
LEAF_END_MARKED JIT_WriteBarrier_Bit_Region64, _TEXT

#ifdef FEATURE_SATORI_GC

// The Satori barriers. WriteBarrierManager copies one of them into the JIT_WriteBarrier buffer (see patchedcode.S)
// and patches the constants in the copy:
// - JIT_WriteBarrier_SATORI             - the regular barrier, it reads the barrier state (g_write_watch_table).
// - JIT_WriteBarrier_SATORI_CONCURRENT  - while the GC marks concurrently and the next GC is not a full GC.
// - JIT_WriteBarrier_SATORI_CONCURRENT2 - while the GC marks concurrently and the next GC is a full GC.
//
// The concurrent barriers do not check the barrier state, they assume that the GC marks concurrently.
// The GC switches barriers only when EE is suspended anyway: to a concurrent barrier at the start of
// the concurrent marking and back to the regular barrier in the blocking collection that follows.
//
// The barriers have the same code ahead of the <Name>_CheckedEntry label, so that JIT_CheckedWriteBarrier
// can jump to CheckedEntry in the buffer, whichever barrier is there.
//
// Regarding patchable constants:
// - 64-bit constants have to be loaded into a register
// - The constants have to be aligned to 8 bytes so that they can be patched easily
// - The alignment is by construction: there is only fixed-size code between an alignment directive and a constant
//
//   rdi - dest address
//   rsi - object
//
        .balign 16
LEAF_ENTRY JIT_WriteBarrier_SATORI, _TEXT
#ifdef FEATURE_SATORI_EXTERNAL_OBJECTS
    // check if src is in heap
        NOP_6_BYTE // padding for alignment of constant
PATCH_LABEL JIT_WriteBarrier_SATORI_Patch_Label_CardBundleTable
        movabs  rax, 0xF0F0F0F0F0F0F0F0                 // the page byte map
PATCH_LABEL JIT_WriteBarrier_SATORI_CheckedEntry
        mov     r8, rsi
        shr     r8, 30                                  //  src page index
        cmp     byte ptr [rax + r8], 0
        je      LOCAL_LABEL(JustAssign_SATORI)                 // src not in heap
#else
PATCH_LABEL JIT_WriteBarrier_SATORI_CheckedEntry
#endif

    // check for escaping assignment
    // 1) check if we own the source region
        mov     rdx, rsi
        and     rdx, 0xFFFFFFFFFFE00000  // source region
#ifndef FEATURE_SATORI_EXTERNAL_OBJECTS
        jz      LOCAL_LABEL(JustAssign_SATORI)                 // assigning null
#endif
#ifdef TARGET_APPLE
        mov     rax,  gs:[0]            // thread tag
#else
        mov     rax,  fs:[0]            // thread tag
#endif
        cmp     qword ptr [rdx], rax
        jne     LOCAL_LABEL(AssignAndMarkCards_SATORI)         // not local to this thread

    // 2) check if the src and dst are from the same region
        mov     rax, rdi
        and     rax, 0xFFFFFFFFFFE00000 // target aligned to region
        cmp     rax, rdx
        jnz     LOCAL_LABEL(RecordEscape_SATORI)               // cross region assignment. definitely escaping

    // 3) check if the target is exposed
        mov     rax, rdi
        and     rax, 0x1FFFFF
        shr     rax, 3
        bt      qword ptr [rdx], rax
        jb      LOCAL_LABEL(RecordEscape_SATORI)               // target is exposed. record an escape.

    LOCAL_LABEL(JustAssign_SATORI):
        mov     [rdi], rsi              // src is external/null, or the assignment is completely threadlocal
        ret

        // Not executed. Aligns the constant below: the padding, the 3-byte store and the 2-byte opcode
        // end on an 8-byte boundary.
        .p2align 3
        NOP_3_BYTE
    LOCAL_LABEL(AssignAndMarkCards_SATORI):
        mov     [rdi], rsi

        // The barrier state may become concurrent while EE runs, thus this barrier reads it.
PATCH_LABEL JIT_WriteBarrier_SATORI_Patch_Label_WriteWatchTable
        movabs  rcx, 0xF0F0F0F0F0F0F0F0 // the address of the barrier state (g_write_watch_table)
        mov     r11, [rcx]

        // check the barrier state. this must be done after the assignment (in program order)
        // if state == 2 we do not set or dirty cards.
        cmp     r11, 2
        jne     LOCAL_LABEL(DoCards_SATORI)
    LOCAL_LABEL(Exit_SATORI):
        ret

    LOCAL_LABEL(DoCards_SATORI):
    // if src and dst are in the same region, cards are not needed, unless concurrent
        xor     rsi, rdi
        shr     rsi, 21
        jz      LOCAL_LABEL(CheckConcurrent_SATORI)            // same region, just check if barrier is not concurrent

    // dst is in another region - cards are needed if src is ephemeral (gen < 2)
        cmp     dword ptr [rdx + 16], 2
        jl     LOCAL_LABEL(MarkCards_SATORI)

    LOCAL_LABEL(CheckConcurrent_SATORI):
    // if not concurrent, exit
        cmp     r11, 0
        je      LOCAL_LABEL(Exit_SATORI)

        // Aligns the constant below: the padding and the 2-byte opcode end on an 8-byte boundary.
        // It is ahead of the label, so only the fall-through from CheckConcurrent runs it.
        .p2align 3
        NOP_6_BYTE
    LOCAL_LABEL(MarkCards_SATORI):
    // fetch card location for rdi
PATCH_LABEL JIT_WriteBarrier_SATORI_Patch_Label_CardTable
        movabs  r9, 0xF0F0F0F0F0F0F0F0  // the page map
        mov     rdx, rdi
        shr     rdi, 30
        mov     rax, qword ptr [r9 + rdi * 8] // page
        sub     rdx, rax   // offset in page
        mov     r8,  rdx
        shr     rdx, 9     // card offset
        shr     r8,  20    // group index
        lea     r8,  [rax + r8 * 2 + 0x80] // group offset

     // check if concurrent marking is in progress
        cmp     r11, 0
        jne     LOCAL_LABEL(DirtyCard_SATORI)

     // SETTING CARD
     LOCAL_LABEL(SetCard_SATORI):
        cmp     byte ptr [rax + rdx], 0
        jne     LOCAL_LABEL(Exit_SATORI)
        mov     byte ptr [rax + rdx], 1
     LOCAL_LABEL(SetGroup_SATORI):
        cmp     byte ptr [r8], 0
        jne     LOCAL_LABEL(CardSet_SATORI)
        mov     byte ptr [r8], 1
     LOCAL_LABEL(SetPage_SATORI):
        cmp     byte ptr [rax], 0
        jne     LOCAL_LABEL(CardSet_SATORI)
        // DIRTY on purpose: a racing REMEMBERED store could overwrite another thread's DIRTY.
        mov     byte ptr [rax], 4

     LOCAL_LABEL(CardSet_SATORI):
     // check if concurrent marking is still not in progress
        cmp     qword ptr [rcx], 0      // rcx - the address of the barrier state
        jne     LOCAL_LABEL(DirtyCard_SATORI)
        ret

     // DIRTYING CARD
     LOCAL_LABEL(DirtyCard_SATORI):
        mov     byte ptr [rax + rdx], 4
     LOCAL_LABEL(DirtyGroup_SATORI):
        cmp     byte ptr [r8], 4
        je      LOCAL_LABEL(Exit_SATORI)
        mov     byte ptr [r8], 4
     LOCAL_LABEL(DirtyPage_SATORI):
        cmp     byte ptr [rax], 4
        je      LOCAL_LABEL(Exit_SATORI)
        mov     byte ptr [rax], 4
        ret

    // this is expected to be rare.
    LOCAL_LABEL(RecordEscape_SATORI):

    // 4) check if the source is escaped
        mov     rax, rsi
        add     rax, 8                        // escape bit is MT + 1
        and     rax, 0x1FFFFF
        shr     rax, 3
        bt      qword ptr [rdx], rax
        jb      LOCAL_LABEL(AssignAndMarkCards_SATORI)         // source is already escaped.

        // Align rsp
        mov  r9, rsp
        and  rsp, -16

        // save rsp, rdi, rsi and rdx
        push r9
        push rdi
        push rsi
        push rdx

        // also save xmm0, in case it is used for stack clearing.
        // Hopefully EscapeFn cannot corrupt other xmm regs, since there is no float math or vectorizable code in there.
        sub     rsp, 16
        movdqa  [rsp], xmm0

        // void SatoriRegion::EscapeFn(SatoriObject** dst, SatoriObject* src, SatoriRegion* region)
        call    qword ptr [rdx + 8]

        movdqa  xmm0, [rsp]
        add     rsp, 16

        pop     rdx
        pop     rsi
        pop     rdi
        pop     rsp
        jmp     LOCAL_LABEL(AssignAndMarkCards_SATORI)
LEAF_END_MARKED JIT_WriteBarrier_SATORI, _TEXT

//
// The GC marks concurrently and the next GC is not a full GC.
// The cards are dirtied, but only if src is ephemeral (gen < 2). A gen1 GC does not mark through tenured
// objects and references to them do not need to be remembered.
//
        .balign 16
LEAF_ENTRY JIT_WriteBarrier_SATORI_CONCURRENT, _TEXT
#ifdef FEATURE_SATORI_EXTERNAL_OBJECTS
    // check if src is in heap
        NOP_6_BYTE // padding for alignment of constant
PATCH_LABEL JIT_WriteBarrier_SATORI_CONCURRENT_Patch_Label_CardBundleTable
        movabs  rax, 0xF0F0F0F0F0F0F0F0                 // the page byte map
PATCH_LABEL JIT_WriteBarrier_SATORI_CONCURRENT_CheckedEntry
        mov     r8, rsi
        shr     r8, 30                                  //  src page index
        cmp     byte ptr [rax + r8], 0
        je      LOCAL_LABEL(JustAssign_SATORI_CONCURRENT)                 // src not in heap
#else
PATCH_LABEL JIT_WriteBarrier_SATORI_CONCURRENT_CheckedEntry
#endif

    // check for escaping assignment
    // 1) check if we own the source region
        mov     rdx, rsi
        and     rdx, 0xFFFFFFFFFFE00000  // source region
#ifndef FEATURE_SATORI_EXTERNAL_OBJECTS
        jz      LOCAL_LABEL(JustAssign_SATORI_CONCURRENT)                 // assigning null
#endif
#ifdef TARGET_APPLE
        mov     rax,  gs:[0]            // thread tag
#else
        mov     rax,  fs:[0]            // thread tag
#endif
        cmp     qword ptr [rdx], rax
        jne     LOCAL_LABEL(AssignAndMarkCards_SATORI_CONCURRENT)         // not local to this thread

    // 2) check if the src and dst are from the same region
        mov     rax, rdi
        and     rax, 0xFFFFFFFFFFE00000 // target aligned to region
        cmp     rax, rdx
        jnz     LOCAL_LABEL(RecordEscape_SATORI_CONCURRENT)               // cross region assignment. definitely escaping

    // 3) check if the target is exposed
        mov     rax, rdi
        and     rax, 0x1FFFFF
        shr     rax, 3
        bt      qword ptr [rdx], rax
        jb      LOCAL_LABEL(RecordEscape_SATORI_CONCURRENT)               // target is exposed. record an escape.

    LOCAL_LABEL(JustAssign_SATORI_CONCURRENT):
        mov     [rdi], rsi              // src is external/null, or the assignment is completely threadlocal
        ret

        // Not executed. Aligns the constant below: the padding, the 3-byte store and the 2-byte opcode
        // end on an 8-byte boundary.
        .p2align 3
        NOP_3_BYTE
    LOCAL_LABEL(AssignAndMarkCards_SATORI_CONCURRENT):
        mov     [rdi], rsi
PATCH_LABEL JIT_WriteBarrier_SATORI_CONCURRENT_Patch_Label_CardTable
        movabs  r9, 0xF0F0F0F0F0F0F0F0  // the page map

    // if src is tenured (gen >= 2), cards are not needed
        cmp     dword ptr [rdx + 16], 2
        jge     LOCAL_LABEL(Exit_SATORI_CONCURRENT)

    // fetch card location for rdi
        mov     rdx, rdi
        shr     rdi, 30
        mov     rax, qword ptr [r9 + rdi * 8] // page
        sub     rdx, rax   // offset in page
        mov     r8,  rdx
        shr     rdx, 9     // card offset
        shr     r8,  20    // group index
        lea     r8,  [rax + r8 * 2 + 0x80] // group offset

     // DIRTYING CARD
     LOCAL_LABEL(DirtyCard_SATORI_CONCURRENT):
        mov     byte ptr [rax + rdx], 4
     LOCAL_LABEL(DirtyGroup_SATORI_CONCURRENT):
        cmp     byte ptr [r8], 4
        je      LOCAL_LABEL(Exit_SATORI_CONCURRENT)
        mov     byte ptr [r8], 4
     LOCAL_LABEL(DirtyPage_SATORI_CONCURRENT):
        cmp     byte ptr [rax], 4
        je      LOCAL_LABEL(Exit_SATORI_CONCURRENT)
        mov     byte ptr [rax], 4
     LOCAL_LABEL(Exit_SATORI_CONCURRENT):
        ret

    // this is expected to be rare.
    LOCAL_LABEL(RecordEscape_SATORI_CONCURRENT):

    // 4) check if the source is escaped
        mov     rax, rsi
        add     rax, 8                        // escape bit is MT + 1
        and     rax, 0x1FFFFF
        shr     rax, 3
        bt      qword ptr [rdx], rax
        jb      LOCAL_LABEL(AssignAndMarkCards_SATORI_CONCURRENT)         // source is already escaped.

        // Align rsp
        mov  r9, rsp
        and  rsp, -16

        // save rsp, rdi, rsi and rdx
        push r9
        push rdi
        push rsi
        push rdx

        // also save xmm0, in case it is used for stack clearing.
        // Hopefully EscapeFn cannot corrupt other xmm regs, since there is no float math or vectorizable code in there.
        sub     rsp, 16
        movdqa  [rsp], xmm0

        // void SatoriRegion::EscapeFn(SatoriObject** dst, SatoriObject* src, SatoriRegion* region)
        call    qword ptr [rdx + 8]

        movdqa  xmm0, [rsp]
        add     rsp, 16

        pop     rdx
        pop     rsi
        pop     rdi
        pop     rsp
        jmp     LOCAL_LABEL(AssignAndMarkCards_SATORI_CONCURRENT)
LEAF_END_MARKED JIT_WriteBarrier_SATORI_CONCURRENT, _TEXT

//
// The GC marks concurrently and the next GC is a full GC.
// The cards are dirtied for any src, since a full GC marks through objects of any generation.
//
        .balign 16
LEAF_ENTRY JIT_WriteBarrier_SATORI_CONCURRENT2, _TEXT
#ifdef FEATURE_SATORI_EXTERNAL_OBJECTS
    // check if src is in heap
        NOP_6_BYTE // padding for alignment of constant
PATCH_LABEL JIT_WriteBarrier_SATORI_CONCURRENT2_Patch_Label_CardBundleTable
        movabs  rax, 0xF0F0F0F0F0F0F0F0                 // the page byte map
PATCH_LABEL JIT_WriteBarrier_SATORI_CONCURRENT2_CheckedEntry
        mov     r8, rsi
        shr     r8, 30                                  //  src page index
        cmp     byte ptr [rax + r8], 0
        je      LOCAL_LABEL(JustAssign_SATORI_CONCURRENT2)                 // src not in heap
#else
PATCH_LABEL JIT_WriteBarrier_SATORI_CONCURRENT2_CheckedEntry
#endif

    // check for escaping assignment
    // 1) check if we own the source region
        mov     rdx, rsi
        and     rdx, 0xFFFFFFFFFFE00000  // source region
#ifndef FEATURE_SATORI_EXTERNAL_OBJECTS
        jz      LOCAL_LABEL(JustAssign_SATORI_CONCURRENT2)                 // assigning null
#endif
#ifdef TARGET_APPLE
        mov     rax,  gs:[0]            // thread tag
#else
        mov     rax,  fs:[0]            // thread tag
#endif
        cmp     qword ptr [rdx], rax
        jne     LOCAL_LABEL(AssignAndMarkCards_SATORI_CONCURRENT2)         // not local to this thread

    // 2) check if the src and dst are from the same region
        mov     rax, rdi
        and     rax, 0xFFFFFFFFFFE00000 // target aligned to region
        cmp     rax, rdx
        jnz     LOCAL_LABEL(RecordEscape_SATORI_CONCURRENT2)               // cross region assignment. definitely escaping

    // 3) check if the target is exposed
        mov     rax, rdi
        and     rax, 0x1FFFFF
        shr     rax, 3
        bt      qword ptr [rdx], rax
        jb      LOCAL_LABEL(RecordEscape_SATORI_CONCURRENT2)               // target is exposed. record an escape.

    LOCAL_LABEL(JustAssign_SATORI_CONCURRENT2):
        mov     [rdi], rsi              // src is external/null, or the assignment is completely threadlocal
        ret

        // Not executed. Aligns the constant below: the padding, the 3-byte store and the 2-byte opcode
        // end on an 8-byte boundary.
        .p2align 3
        NOP_3_BYTE
    LOCAL_LABEL(AssignAndMarkCards_SATORI_CONCURRENT2):
        mov     [rdi], rsi

    // fetch card location for rdi
PATCH_LABEL JIT_WriteBarrier_SATORI_CONCURRENT2_Patch_Label_CardTable
        movabs  r9, 0xF0F0F0F0F0F0F0F0  // the page map
        mov     rdx, rdi
        shr     rdi, 30
        mov     rax, qword ptr [r9 + rdi * 8] // page
        sub     rdx, rax   // offset in page
        mov     r8,  rdx
        shr     rdx, 9     // card offset
        shr     r8,  20    // group index
        lea     r8,  [rax + r8 * 2 + 0x80] // group offset

     // DIRTYING CARD
     LOCAL_LABEL(DirtyCard_SATORI_CONCURRENT2):
        mov     byte ptr [rax + rdx], 4
     LOCAL_LABEL(DirtyGroup_SATORI_CONCURRENT2):
        cmp     byte ptr [r8], 4
        je      LOCAL_LABEL(Exit_SATORI_CONCURRENT2)
        mov     byte ptr [r8], 4
     LOCAL_LABEL(DirtyPage_SATORI_CONCURRENT2):
        cmp     byte ptr [rax], 4
        je      LOCAL_LABEL(Exit_SATORI_CONCURRENT2)
        mov     byte ptr [rax], 4
     LOCAL_LABEL(Exit_SATORI_CONCURRENT2):
        ret

    // this is expected to be rare.
    LOCAL_LABEL(RecordEscape_SATORI_CONCURRENT2):

    // 4) check if the source is escaped
        mov     rax, rsi
        add     rax, 8                        // escape bit is MT + 1
        and     rax, 0x1FFFFF
        shr     rax, 3
        bt      qword ptr [rdx], rax
        jb      LOCAL_LABEL(AssignAndMarkCards_SATORI_CONCURRENT2)         // source is already escaped.

        // Align rsp
        mov  r9, rsp
        and  rsp, -16

        // save rsp, rdi, rsi and rdx
        push r9
        push rdi
        push rsi
        push rdx

        // also save xmm0, in case it is used for stack clearing.
        // Hopefully EscapeFn cannot corrupt other xmm regs, since there is no float math or vectorizable code in there.
        sub     rsp, 16
        movdqa  [rsp], xmm0

        // void SatoriRegion::EscapeFn(SatoriObject** dst, SatoriObject* src, SatoriRegion* region)
        call    qword ptr [rdx + 8]

        movdqa  xmm0, [rsp]
        add     rsp, 16

        pop     rdx
        pop     rsi
        pop     rdi
        pop     rsp
        jmp     LOCAL_LABEL(AssignAndMarkCards_SATORI_CONCURRENT2)
LEAF_END_MARKED JIT_WriteBarrier_SATORI_CONCURRENT2, _TEXT

#endif // FEATURE_SATORI_GC

#ifdef FEATURE_USE_SOFTWARE_WRITE_WATCH_FOR_GC_HEAP

        .balign 16
LEAF_ENTRY JIT_WriteBarrier_WriteWatch_PreGrow64, _TEXT
        // Regarding patchable constants:
        // - 64-bit constants have to be loaded into a register
        // - The constants have to be aligned to 8 bytes so that they can be patched easily
        // - The constant loads have been located to minimize NOP padding required to align the constants
        // - Using different registers for successive constant loads helps pipeline better. Should we decide to use a special
        //   non-volatile calling convention, this should be changed to use just one register.

        // Do the move into the GC .  It is correct to take an AV here, the EH code
        // figures out that this came from a WriteBarrier and correctly maps it back
        // to the managed method which called the WriteBarrier (see setup in
        // InitializeExceptionHandling, vm\exceptionhandling.cpp).
        mov     [rdi], rsi

        // Update the write watch table if necessary
        mov     rax, rdi
PATCH_LABEL JIT_WriteBarrier_WriteWatch_PreGrow64_Patch_Label_WriteWatchTable
        movabs  r10, 0xF0F0F0F0F0F0F0F0
        shr     rax, 0x0C // SoftwareWriteWatch::AddressToTableByteIndexShift
        NOP_2_BYTE // padding for alignment of constant
PATCH_LABEL JIT_WriteBarrier_WriteWatch_PreGrow64_Patch_Label_Lower
        movabs  r11, 0xF0F0F0F0F0F0F0F0
        add     rax, r10
        cmp     byte ptr [rax], 0x0
        .byte 0x75, 0x03
        // jne     LOCAL_LABEL(CheckCardTable_WriteWatch_PreGrow64)
        mov     byte ptr [rax], 0xFF

    LOCAL_LABEL(CheckCardTable_WriteWatch_PreGrow64):
        // Check the lower ephemeral region bound.
        cmp     rsi, r11

#ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
        .byte 0x72, 0x40
#else
        .byte 0x72, 0x20
#endif

        // jb      Exit_WriteWatch_PreGrow64

        // Touch the card table entry, if not already dirty.
        shr     rdi, 0x0B
        NOP_2_BYTE // padding for alignment of constant
PATCH_LABEL JIT_WriteBarrier_WriteWatch_PreGrow64_Patch_Label_CardTable
        movabs  rax, 0xF0F0F0F0F0F0F0F0
        cmp     byte ptr [rdi + rax], 0xFF
        .byte 0x75, 0x02
        // jne     LOCAL_LABEL(UpdateCardTable_WriteWatch_PreGrow64)
        REPRET

    LOCAL_LABEL(UpdateCardTable_WriteWatch_PreGrow64):
        mov     byte ptr [rdi + rax], 0xFF

#ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
        NOP_2_BYTE // padding for alignment of constant
PATCH_LABEL JIT_WriteBarrier_WriteWatch_PreGrow64_Patch_Label_CardBundleTable
        movabs  rax, 0xF0F0F0F0F0F0F0F0

        shr     rdi, 0x0A
        cmp     byte ptr [rdi + rax], 0xFF

        .byte 0x75, 0x02
        // jne     LOCAL_LABEL(UpdateCardBundle_WriteWatch_PreGrow64)
        REPRET

    LOCAL_LABEL(UpdateCardBundle_WriteWatch_PreGrow64):
        mov     byte ptr [rdi + rax], 0xFF
#endif

        ret

    .balign 16
PATCH_LABEL Exit_WriteWatch_PreGrow64
        REPRET
LEAF_END_MARKED JIT_WriteBarrier_WriteWatch_PreGrow64, _TEXT


        .balign 16
LEAF_ENTRY JIT_WriteBarrier_WriteWatch_PostGrow64, _TEXT
        // Regarding patchable constants:
        // - 64-bit constants have to be loaded into a register
        // - The constants have to be aligned to 8 bytes so that they can be patched easily
        // - The constant loads have been located to minimize NOP padding required to align the constants
        // - Using different registers for successive constant loads helps pipeline better. Should we decide to use a special
        //   non-volatile calling convention, this should be changed to use just one register.

        // Do the move into the GC .  It is correct to take an AV here, the EH code
        // figures out that this came from a WriteBarrier and correctly maps it back
        // to the managed method which called the WriteBarrier (see setup in
        // InitializeExceptionHandling, vm\exceptionhandling.cpp).
        mov     [rdi], rsi

        // Update the write watch table if necessary
        mov     rax, rdi
PATCH_LABEL JIT_WriteBarrier_WriteWatch_PostGrow64_Patch_Label_WriteWatchTable
        movabs  r10, 0xF0F0F0F0F0F0F0F0
        shr     rax, 0x0C // SoftwareWriteWatch::AddressToTableByteIndexShift
        NOP_2_BYTE // padding for alignment of constant
PATCH_LABEL JIT_WriteBarrier_WriteWatch_PostGrow64_Patch_Label_Lower
        movabs  r11, 0xF0F0F0F0F0F0F0F0
        add     rax, r10
        cmp     byte ptr [rax], 0x0
        .byte 0x75, 0x06
        // jne     LOCAL_LABEL(CheckCardTable_WriteWatch_PostGrow64)
        mov     byte ptr [rax], 0xFF

        NOP_3_BYTE // padding for alignment of constant

        // Check the lower and upper ephemeral region bounds
    LOCAL_LABEL(CheckCardTable_WriteWatch_PostGrow64):
        cmp     rsi, r11

#ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
        .byte 0x72, 0x4d
#else
        .byte 0x72, 0x3d
#endif
        // jb      Exit_WriteWatch_PostGrow64

        NOP_3_BYTE // padding for alignment of constant

PATCH_LABEL JIT_WriteBarrier_WriteWatch_PostGrow64_Patch_Label_Upper
        movabs  r10, 0xF0F0F0F0F0F0F0F0

        cmp     rsi, r10

#ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
        .byte 0x73, 0x3b
#else
        .byte 0x73, 0x2b
#endif
        // jae     Exit_WriteWatch_PostGrow64

        nop // padding for alignment of constant

PATCH_LABEL JIT_WriteBarrier_WriteWatch_PostGrow64_Patch_Label_CardTable
        movabs  rax, 0xF0F0F0F0F0F0F0F0

        // Touch the card table entry, if not already dirty.
        shr     rdi, 0x0B
        cmp     byte ptr [rdi + rax], 0xFF
        .byte 0x75, 0x02
        // jne     LOCAL_LABEL(UpdateCardTable_WriteWatch_PostGrow64)
        REPRET

    LOCAL_LABEL(UpdateCardTable_WriteWatch_PostGrow64):
        mov     byte ptr [rdi + rax], 0xFF

#ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
        NOP_2_BYTE // padding for alignment of constant
        shr     rdi, 0x0A

PATCH_LABEL JIT_WriteBarrier_WriteWatch_PostGrow64_Patch_Label_CardBundleTable
        movabs  rax, 0xF0F0F0F0F0F0F0F0
        cmp     byte ptr [rdi + rax], 0xFF

        .byte 0x75, 0x02
        // jne     LOCAL_LABEL(UpdateCardBundle_WriteWatch_PostGrow64)
        REPRET

    LOCAL_LABEL(UpdateCardBundle_WriteWatch_PostGrow64):
        mov     byte ptr [rdi + rax], 0xFF
#endif

        ret
    .balign 16
PATCH_LABEL Exit_WriteWatch_PostGrow64
        REPRET
LEAF_END_MARKED JIT_WriteBarrier_WriteWatch_PostGrow64, _TEXT


#ifdef FEATURE_SVR_GC

        .balign 16
LEAF_ENTRY JIT_WriteBarrier_WriteWatch_SVR64, _TEXT
        // Regarding patchable constants:
        // - 64-bit constants have to be loaded into a register
        // - The constants have to be aligned to 8 bytes so that they can be patched easily
        // - The constant loads have been located to minimize NOP padding required to align the constants
        // - Using different registers for successive constant loads helps pipeline better. Should we decide to use a special
        //   non-volatile calling convention, this should be changed to use just one register.

        //
        // SVR GC has multiple heaps, so it cannot provide one single
        // ephemeral region to bounds check against, so we just skip the
        // bounds checking altogether and do our card table update
        // unconditionally.
        //

        // Do the move into the GC .  It is correct to take an AV here, the EH code
        // figures out that this came from a WriteBarrier and correctly maps it back
        // to the managed method which called the WriteBarrier (see setup in
        // InitializeExceptionHandling, vm\exceptionhandling.cpp).
        mov     [rdi], rsi

        // Update the write watch table if necessary
        mov     rax, rdi
PATCH_LABEL JIT_WriteBarrier_WriteWatch_SVR64_PatchLabel_WriteWatchTable
        movabs  r10, 0xF0F0F0F0F0F0F0F0
        shr     rax, 0xC // SoftwareWriteWatch::AddressToTableByteIndexShift
        NOP_2_BYTE // padding for alignment of constant
PATCH_LABEL JIT_WriteBarrier_WriteWatch_SVR64_PatchLabel_CardTable
        movabs  r11, 0xF0F0F0F0F0F0F0F0
        add     rax, r10
        cmp     byte ptr [rax], 0x0
        .byte 0x75, 0x03
        // jne     LOCAL_LABEL(CheckCardTable_WriteWatch_SVR64)
        mov     byte ptr [rax], 0xFF

    LOCAL_LABEL(CheckCardTable_WriteWatch_SVR64):
        shr     rdi, 0x0B
        cmp     byte ptr [rdi + r11], 0xFF
        .byte 0x75, 0x02
        // jne     LOCAL_LABEL(UpdateCardTable_WriteWatch_SVR64)
        REPRET

    LOCAL_LABEL(UpdateCardTable_WriteWatch_SVR64):
        mov     byte ptr [rdi + r11], 0xFF

#ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
        NOP // padding for alignment of constant

PATCH_LABEL JIT_WriteBarrier_WriteWatch_SVR64_PatchLabel_CardBundleTable
        movabs  r11, 0xF0F0F0F0F0F0F0F0

        shr     rdi, 0x0A
        cmp     byte ptr [rdi + r11], 0xFF
        .byte 0x75, 0x02
        // jne     LOCAL_LABEL(UpdateCardBundle_WriteWatch_SVR64)
        REPRET

    LOCAL_LABEL(UpdateCardBundle_WriteWatch_SVR64):
        mov     byte ptr [rdi + r11], 0xFF
#endif

        ret

LEAF_END_MARKED JIT_WriteBarrier_WriteWatch_SVR64, _TEXT

#endif
        .balign 16
LEAF_ENTRY JIT_WriteBarrier_WriteWatch_Byte_Region64, _TEXT

        // Do the move into the GC .  It is correct to take an AV here, the EH code
        // figures out that this came from a WriteBarrier and correctly maps it back
        // to the managed method which called the WriteBarrier (see setup in
        // InitializeExceptionHandling, vm\exceptionhandling.cpp).
        mov     [rdi], rsi

        // Update the write watch table if necessary
        mov     rax, rdi
PATCH_LABEL JIT_WriteBarrier_WriteWatch_Byte_Region64_Patch_Label_WriteWatchTable
        movabs  r8, 0xF0F0F0F0F0F0F0F0
        shr     rax, 0x0C // SoftwareWriteWatch::AddressToTableByteIndexShift
        add     rax, r8
        mov     r8, rdi
PATCH_LABEL JIT_WriteBarrier_WriteWatch_Byte_Region64_Patch_Label_RegionShrDest
        shr     rdi, 0x16 // compute region index
        cmp     byte ptr [rax], 0x0
        .byte 0x75, 0x03
        // jne     LOCAL_LABEL(CheckGen0_WriteWatch_Byte_Region64)
        mov     byte ptr [rax], 0xFF
    LOCAL_LABEL(CheckGen0_WriteWatch_Byte_Region64):
PATCH_LABEL JIT_WriteBarrier_WriteWatch_Byte_Region64_Patch_Label_RegionToGeneration
        mov     rax, 0xF0F0F0F0F0F0F0F0

        // Check whether the region we're storing into is gen 0 - nothing to do in this case
        cmp     byte ptr [rdi + rax], 0
        .byte 0x75, 0x08
        // jne     LOCAL_LABEL(NotGen0_WriteWatch_Byte_Region64)
        REPRET

        NOP_2_BYTE // padding for alignment of constant
        NOP_2_BYTE // padding for alignment of constant
        NOP_2_BYTE // padding for alignment of constant

    LOCAL_LABEL(NotGen0_WriteWatch_Byte_Region64):
PATCH_LABEL JIT_WriteBarrier_WriteWatch_Byte_Region64_Patch_Label_Lower
        movabs  r9, 0xF0F0F0F0F0F0F0F0
        cmp     rsi, r9
        .byte 0x73, 0x01
        // jae     LOCAL_LABEL(NotLow_WriteWatch_Byte_Region64)
        ret
    LOCAL_LABEL(NotLow_WriteWatch_Byte_Region64):
PATCH_LABEL JIT_WriteBarrier_WriteWatch_Byte_Region64_Patch_Label_Upper
        mov     r9, 0xF0F0F0F0F0F0F0F0
        cmp     rsi, r9
        .byte 0x72, 0x02
        // jb      LOCAL_LABEL(NotHigh_WriteWatch_Byte_Region64)
        REPRET
    LOCAL_LABEL(NotHigh_WriteWatch_Byte_Region64):
PATCH_LABEL JIT_WriteBarrier_WriteWatch_Byte_Region64_Patch_Label_RegionShrSrc
        shr     rsi, 0x16 // compute region index
        mov     dl, [rsi + rax]
        cmp     dl, [rdi + rax]
        .byte 0x72, 0x03
        // jb      LOCAL_LABEL(IsOldToYoung_WriteWatch_Byte_Region64)
        REPRET
        nop

    LOCAL_LABEL(IsOldToYoung_WriteWatch_Byte_Region64):
PATCH_LABEL JIT_WriteBarrier_WriteWatch_Byte_Region64_Patch_Label_CardTable
        mov     rax, 0xF0F0F0F0F0F0F0F0

        shr     r8, 0xB
        cmp     byte ptr [r8 + rax], 0xFF
        .byte 0x75, 0x02
        // jne      LOCAL_LABEL(UpdateCardTable_WriteWatch_Byte_Region64)
        REPRET

    LOCAL_LABEL(UpdateCardTable_WriteWatch_Byte_Region64):
        mov     byte ptr [r8 + rax], 0xFF
#ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
        shr     r8, 0x0A
PATCH_LABEL JIT_WriteBarrier_WriteWatch_Byte_Region64_Patch_Label_CardBundleTable
        mov     rax, 0xF0F0F0F0F0F0F0F0
        cmp     byte ptr [r8 + rax], 0xFF
        .byte 0x75, 0x02
        // jne     LOCAL_LABEL(UpdateCardBundleTable_WriteWatch_Byte_Region64)
        REPRET

    LOCAL_LABEL(UpdateCardBundleTable_WriteWatch_Byte_Region64):
        mov     byte ptr [r8 + rax], 0xFF
#endif
        ret
LEAF_END_MARKED JIT_WriteBarrier_WriteWatch_Byte_Region64, _TEXT
        .balign 16
LEAF_ENTRY JIT_WriteBarrier_WriteWatch_Bit_Region64, _TEXT

        // Do the move into the GC .  It is correct to take an AV here, the EH code
        // figures out that this came from a WriteBarrier and correctly maps it back
        // to the managed method which called the WriteBarrier (see setup in
        // InitializeExceptionHandling, vm\exceptionhandling.cpp).
        mov     [rdi], rsi

        // Update the write watch table if necessary
        mov     rax, rdi
PATCH_LABEL JIT_WriteBarrier_WriteWatch_Bit_Region64_Patch_Label_WriteWatchTable
        movabs  r8, 0xF0F0F0F0F0F0F0F0
        shr     rax, 0x0C // SoftwareWriteWatch::AddressToTableByteIndexShift
        add     rax, r8
        mov     r8, rdi
PATCH_LABEL JIT_WriteBarrier_WriteWatch_Bit_Region64_Patch_Label_RegionShrDest
        shr     rdi, 0x16 // compute region index
        cmp     byte ptr [rax], 0x0
        .byte 0x75, 0x03
        // jne     LOCAL_LABEL(CheckGen0_WriteWatch_Bit_Region64)
        mov     byte ptr [rax], 0xFF
    LOCAL_LABEL(CheckGen0_WriteWatch_Bit_Region64):
PATCH_LABEL JIT_WriteBarrier_WriteWatch_Bit_Region64_Patch_Label_RegionToGeneration
        mov     rax, 0xF0F0F0F0F0F0F0F0

        // Check whether the region we're storing into is gen 0 - nothing to do in this case
        cmp     byte ptr [rdi + rax], 0
        .byte 0x75, 0x08
        // jne     LOCAL_LABEL(NotGen0_WriteWatch_Bit_Region64)
        REPRET

        NOP_2_BYTE // padding for alignment of constant
        NOP_2_BYTE // padding for alignment of constant
        NOP_2_BYTE // padding for alignment of constant

    LOCAL_LABEL(NotGen0_WriteWatch_Bit_Region64):
PATCH_LABEL JIT_WriteBarrier_WriteWatch_Bit_Region64_Patch_Label_Lower
        movabs  r9, 0xF0F0F0F0F0F0F0F0
        cmp     rsi, r9
        .byte 0x73, 0x01
        // jae     LOCAL_LABEL(NotLow_WriteWatch_Bit_Region64)
        ret
    LOCAL_LABEL(NotLow_WriteWatch_Bit_Region64):
PATCH_LABEL JIT_WriteBarrier_WriteWatch_Bit_Region64_Patch_Label_Upper
        mov     r9, 0xF0F0F0F0F0F0F0F0
        cmp     rsi, r9
        .byte 0x72, 0x02
        // jb      LOCAL_LABEL(NotHigh_WriteWatch_Bit_Region64)
        REPRET
    LOCAL_LABEL(NotHigh_WriteWatch_Bit_Region64):
PATCH_LABEL JIT_WriteBarrier_WriteWatch_Bit_Region64_Patch_Label_RegionShrSrc
        shr     rsi, 0x16 // compute region index
        mov     dl, [rsi + rax]
        cmp     dl, [rdi + rax]
        .byte 0x72, 0x03
        // jb      LOCAL_LABEL(IsOldToYoung_WriteWatch_Bit_Region64)
        REPRET
        nop

    LOCAL_LABEL(IsOldToYoung_WriteWatch_Bit_Region64):
PATCH_LABEL JIT_WriteBarrier_WriteWatch_Bit_Region64_Patch_Label_CardTable
        mov     rax, 0xF0F0F0F0F0F0F0F0

        mov     ecx, r8d
        shr     r8, 0xB
        shr     ecx, 8
        and     ecx, 7
        mov     dl, 1
        shl     dl, cl
        test    byte ptr [r8 + rax], dl
        .byte 0x74, 0x02
        // je      LOCAL_LABEL(UpdateCardTable_WriteWatch_Bit_Region64)
        REPRET

    LOCAL_LABEL(UpdateCardTable_WriteWatch_Bit_Region64):
        lock or byte ptr [r8 + rax], dl
#ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
PATCH_LABEL JIT_WriteBarrier_WriteWatch_Bit_Region64_Patch_Label_CardBundleTable
        mov     rax, 0xF0F0F0F0F0F0F0F0
        shr     r8, 0x0A
        cmp     byte ptr [r8 + rax], 0xFF
        .byte 0x75, 0x02
        // jne     LOCAL_LABEL(UpdateCardBundleTable_WriteWatch_Bit_Region64)
        REPRET

    LOCAL_LABEL(UpdateCardBundleTable_WriteWatch_Bit_Region64):
        mov     byte ptr [r8 + rax], 0xFF
#endif
        ret
LEAF_END_MARKED JIT_WriteBarrier_WriteWatch_Bit_Region64, _TEXT

#endif
