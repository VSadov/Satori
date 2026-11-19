// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

.intel_syntax noprefix
#include "unixasmmacros.inc"
#include "asmconstants.h"

// On Apple platforms we emit the whole patched region as single function
// with .alt_entry labels for individual write barrier helpers. This ensures
// the linker doesn't relocate or split the code and treats it as single
// atom. We also need to be careful to produce the correct unwinding
// information.

//-----------------------------------------------------------------------------
// The following Macros help in WRITE_BARRIER Implementations
// WRITE_BARRIER_ENTRY
//
// Declare the start of a write barrier function. Use similarly to NESTED_ENTRY. This is the only legal way
// to declare a write barrier function.
//
.macro WRITE_BARRIER_ENTRY name
#if defined(__APPLE__)
    // .cfi_startproc/.cfi_endproc is workaround for https://github.com/dotnet/runtime/issues/119005
    .cfi_endproc
    .p2align        4
    .alt_entry C_FUNC(\name)
    .private_extern C_FUNC(\name)
C_FUNC(\name):
    .cfi_startproc
#else
    LEAF_ENTRY \name, _TEXT
#endif
.endm

// WRITE_BARRIER_END
//
// The partner to WRITE_BARRIER_ENTRY, used like NESTED_END.
//
.macro WRITE_BARRIER_END name
#if defined(__APPLE__)
    // .cfi_startproc/.cfi_endproc is workaround for https://github.com/dotnet/runtime/issues/119005
    .cfi_endproc
    .alt_entry C_FUNC(\name\()_End)
    .private_extern C_FUNC(\name\()_End)
C_FUNC(\name\()_End):
    .cfi_startproc
    // make sure this symbol gets its own address
    nop
#else
    LEAF_END_MARKED \name, _TEXT
#endif
.endm

// WRITE_BARRIER_PATCH_LABEL
//
// Marks a patchable constant within a write barrier function. On Apple platforms it is an .alt_entry, so that it
// does not split the patched region (see above).
//
.macro WRITE_BARRIER_PATCH_LABEL name
#if defined(__APPLE__)
    // .cfi_startproc/.cfi_endproc is workaround for https://github.com/dotnet/runtime/issues/119005
    .cfi_endproc
    .alt_entry C_FUNC(\name)
    .private_extern C_FUNC(\name)
C_FUNC(\name):
    .cfi_startproc
#else
    PATCH_LABEL \name
#endif
.endm

#ifndef FEATURE_SATORI_GC

// Mark start of the code region that we patch at runtime
#if defined(__APPLE__)
        .global C_FUNC(JIT_PatchedCodeStart)
C_FUNC(JIT_PatchedCodeStart):
        .cfi_startproc
        ret
#else
LEAF_ENTRY JIT_PatchedCodeStart, _TEXT
        ret
LEAF_END JIT_PatchedCodeStart, _TEXT
#endif


// There is an even more optimized version of these helpers possible which takes
// advantage of knowledge of which way the ephemeral heap is growing to only do 1/2
// that check (this is more significant in the JIT_WriteBarrier case).
//
// Additionally we can look into providing helpers which will take the src/dest from
// specific registers (like x86) which _could_ (??) make for easier register allocation
// for the JIT64, however it might lead to having to have some nasty code that treats
// these guys really special like... :(.
//
// Version that does the move, checks whether or not it's in the GC and whether or not
// it needs to have it's card updated
//
// void JIT_CheckedWriteBarrier(Object** dst, Object* src)
WRITE_BARRIER_ENTRY JIT_CheckedWriteBarrier

        // When WRITE_BARRIER_CHECK is defined _NotInHeap will write the reference
        // but if it isn't then it will just return.
        //
        // See if this is in GCHeap
        PREPARE_EXTERNAL_VAR g_lowest_address, rax
        cmp     rdi, [rax]
        // jb      LOCAL_LABEL(NotInHeap)
        .byte 0x72, 0x12
        PREPARE_EXTERNAL_VAR g_highest_address, rax
        cmp     rdi, [rax]

        // jnb     LOCAL_LABEL(NotInHeap)
        .byte 0x73, 0x06
        jmp     [rip + C_FUNC(JIT_WriteBarrier_Loc)]

    LOCAL_LABEL(NotInHeap):
        // See comment above about possible AV
        mov     [rdi], rsi
        ret
WRITE_BARRIER_END JIT_CheckedWriteBarrier


// This is used by the mechanism to hold either the JIT_WriteBarrier_PreGrow
// or JIT_WriteBarrier_PostGrow code (depending on the state of the GC). It _WILL_
// change at runtime as the GC changes. Initially it should simply be a copy of the
// larger of the two functions (JIT_WriteBarrier_PostGrow) to ensure we have created
// enough space to copy that code in.
.balign 16
WRITE_BARRIER_ENTRY JIT_WriteBarrier
#ifdef _DEBUG
        // In debug builds, this just contains jump to the debug version of the write barrier by default
        jmp C_FUNC(JIT_WriteBarrier_Debug)
#endif

#ifdef FEATURE_USE_SOFTWARE_WRITE_WATCH_FOR_GC_HEAP
        // JIT_WriteBarrier_WriteWatch_PostGrow64

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
        movabs  r10, 0xF0F0F0F0F0F0F0F0
        shr     rax, 0xC // SoftwareWriteWatch::AddressToTableByteIndexShift
        NOP_2_BYTE // padding for alignment of constant
        movabs  r11, 0xF0F0F0F0F0F0F0F0
        add     rax, r10
        cmp     byte ptr [rax], 0x0
        .byte 0x75, 0x06
        // jne     LOCAL_LABEL(CheckCardTable)
        mov     byte ptr [rax], 0xFF

        NOP_3_BYTE // padding for alignment of constant

        // Check the lower and upper ephemeral region bounds
    LOCAL_LABEL(CheckCardTable):
        cmp     rsi, r11
        .byte 0x72,0x3D
        // jb      LOCAL_LABEL(Exit)

        NOP_3_BYTE // padding for alignment of constant

        movabs  r10, 0xF0F0F0F0F0F0F0F0

        cmp     rsi, r10
        .byte 0x73,0x2B
        // jae     LOCAL_LABEL(Exit)

        nop // padding for alignment of constant

        movabs  rax, 0xF0F0F0F0F0F0F0F0

        // Touch the card table entry, if not already dirty.
        shr     rdi, 0x0B
        cmp     byte ptr [rdi + rax], 0xFF
        .byte 0x75, 0x02
        // jne     LOCAL_LABEL(UpdateCardTable)
        REPRET

    LOCAL_LABEL(UpdateCardTable):
        mov     byte ptr [rdi + rax], 0xFF

#ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
        NOP_2_BYTE // padding for alignment of constant
        shr     rdi, 0x0A

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
    LOCAL_LABEL(Exit):
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

#else
        // JIT_WriteBarrier_PostGrow64

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

        movabs  rax, 0xF0F0F0F0F0F0F0F0

        // Check the lower and upper ephemeral region bounds
        cmp     rsi, rax
        // jb      LOCAL_LABEL(Exit)
        .byte 0x72, 0x36

        nop // padding for alignment of constant

        movabs  r8, 0xF0F0F0F0F0F0F0F0

        cmp     rsi, r8
        // jae     LOCAL_LABEL(Exit)
        .byte 0x73, 0x26

        nop // padding for alignment of constant

        movabs  rax, 0xF0F0F0F0F0F0F0F0

        // Touch the card table entry, if not already dirty.
        shr     rdi, 0Bh
        cmp     byte ptr [rdi + rax], 0FFh
        .byte 0x75, 0x02
        // jne     LOCAL_LABEL(UpdateCardTable)
        REPRET

    LOCAL_LABEL(UpdateCardTable):
        mov     byte ptr [rdi + rax], 0FFh

#ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
        NOP_6_BYTE // padding for alignment of constant

        movabs  rax, 0xF0F0F0F0F0F0F0F0

        // Touch the card bundle, if not already dirty.
        // rdi is already shifted by 0xB, so shift by 0xA more
        shr     rdi, 0x0A
        cmp     byte ptr [rdi + rax], 0FFh

        .byte 0x75, 0x02
        // jne     LOCAL_LABEL(UpdateCardBundle)
        REPRET

    LOCAL_LABEL(UpdateCardBundle):
        mov     byte ptr [rdi + rax], 0FFh
#endif

        ret

    .balign 16
    LOCAL_LABEL(Exit):
        REPRET
#endif

    // make sure this is bigger than any of the others
    .balign 16
        nop
WRITE_BARRIER_END JIT_WriteBarrier

// Mark start of the code region that we patch at runtime
#if defined(__APPLE__)
        .private_extern C_FUNC(JIT_PatchedCodeLast)
        .alt_entry C_FUNC(JIT_PatchedCodeLast)
C_FUNC(JIT_PatchedCodeLast):
        .cfi_endproc
        ret
#else
LEAF_ENTRY JIT_PatchedCodeLast, _TEXT
        ret
LEAF_END JIT_PatchedCodeLast, _TEXT
#endif

#else  //FEATURE_SATORI_GC      ##############################################################################

// The Satori barriers run from the copy of the patched code region (see GetWriteBarrierCodeLocation), so they
// cannot refer to globals RIP-relatively. Instead they embed the values that do not change after the GC
// initializes the write barrier - the page byte map (g_card_bundle_table), the page map (g_card_table) and the
// address of g_write_watch_table, which holds the barrier state - as constants that WriteBarrierManager patches.
// JIT_WriteBarrier is a buffer for one of the barriers in jithelpers_fastwritebarriers.S.
//
// Regarding patchable constants:
// - 64-bit constants have to be loaded into a register
// - The constants have to be aligned to 8 bytes so that they can be patched easily
// - The alignment is by construction: there is only fixed-size code between an alignment directive and a constant
//
// The region and the barriers in it start on cache line boundaries, so that the barriers are cache line aligned
// also where the region is copied, which keeps their offsets from the region start.

// Mark start of the code region that we patch at runtime
        .balign 64
#if defined(__APPLE__)
        .global C_FUNC(JIT_PatchedCodeStart)
C_FUNC(JIT_PatchedCodeStart):
        .cfi_startproc
        ret
#else
LEAF_ENTRY JIT_PatchedCodeStart, _TEXT
        ret
LEAF_END JIT_PatchedCodeStart, _TEXT
#endif


// void JIT_CheckedWriteBarrier(Object** dst, Object* src)
        .balign 64
WRITE_BARRIER_ENTRY JIT_CheckedWriteBarrier

     // See if dst is in GCHeap
        NOP_6_BYTE // padding for alignment of constant
WRITE_BARRIER_PATCH_LABEL JIT_CheckedWriteBarrier_Patch_Label_CardBundleTable
        movabs  rax, 0xF0F0F0F0F0F0F0F0                 // the page byte map
        mov     r8, rdi
        shr     r8, 30                                  // dst page index
        cmp     byte ptr [rax + r8], 0
        jne     LOCAL_LABEL(CheckedEntryLocal)

    LOCAL_LABEL(NotInHeap):
        // See comment above about possible AV
        mov     [rdi], rsi
        ret
WRITE_BARRIER_END JIT_CheckedWriteBarrier


// void JIT_WriteBarrier(Object** dst, Object* src)
//
// Space for one of the Satori barriers (see JIT_WriteBarrier_SATORI_DYNAMIC in jithelpers_fastwritebarriers.S), which
// WriteBarrierManager copies here when the GC initializes the write barrier and when the GC switches barriers.
// The barriers have the same code ahead of CheckedEntry, so JIT_CheckedWriteBarrier can jump there in any of them.
// It jumps to a local label, which the assembler resolves, because the linker could route a jump to the global
// symbol outside the region (e.g. through the PLT), and then the jump would be wrong in the copy.
//
//   rdi - dest address
//   rsi - object
//
        .balign 64
WRITE_BARRIER_ENTRY JIT_WriteBarrier
#ifdef FEATURE_SATORI_EXTERNAL_OBJECTS
        NOP_6_BYTE
        movabs  rax, 0xF0F0F0F0F0F0F0F0
#endif
    LOCAL_LABEL(CheckedEntryLocal):
    ALTERNATE_ENTRY CheckedEntry

        // make sure this is bigger than any of the barriers
        .fill 0x1F0, 1, 0xCC
WRITE_BARRIER_END JIT_WriteBarrier


// Mark start of the code region that we patch at runtime
#if defined(__APPLE__)
        .private_extern C_FUNC(JIT_PatchedCodeLast)
        .alt_entry C_FUNC(JIT_PatchedCodeLast)
C_FUNC(JIT_PatchedCodeLast):
        .cfi_endproc
        ret
#else
LEAF_ENTRY JIT_PatchedCodeLast, _TEXT
        ret
LEAF_END JIT_PatchedCodeLast, _TEXT
#endif

#endif  // FEATURE_SATORI_GC
