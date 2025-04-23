/*
 * Copyright (c) 2024, 2025, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "gc/agnostic/agnosticBarrierSetAssembler.hpp"
#include "gc/agnostic/agnosticThreadLocalData.hpp"
#include "gc/g1/g1ThreadLocalData.hpp"
#include "gc/g1/g1BarrierSetRuntime.hpp"
#include "gc/z/zBarrierSetAssembler.hpp"
#include "gc/z/zBarrierSetRuntime.hpp"
#include "gc/z/zStoreBarrierBuffer.hpp"
#include "gc/z/zThreadLocalData.hpp"
#include "utilities/debug.hpp"

#ifdef PRODUCT
#define BLOCK_COMMENT(str) /* nothing */
#else
#define BLOCK_COMMENT(str) __ block_comment(str)
#endif

#undef __
#define __ masm->

#if defined(COMPILER2)
// Generate patchable code stub for inserting into either G1's mark queue or ZGC's store buffer.
static void generate_satb_enqueue(MacroAssembler* masm,
    const Address ref_addr,
    const Register tmp1,
    const Register tmp2,
    Label& slow_path,
    Label& continuation) {
  BLOCK_COMMENT("SATB Insertion");

  Address buffer(rthread, ZThreadLocalData::store_barrier_buffer_offset());
  assert_different_registers(ref_addr.base(), ref_addr.index(), tmp1, tmp2);

  __ ldr(tmp1, buffer);

  // Combined pointer bump and check if the buffer is disabled or full
  __ ldr(tmp2, Address(tmp1, ZStoreBarrierBuffer::current_offset()));
  __ cbz(tmp2, slow_path);

  // Bump the pointer
  __ sub(tmp2, tmp2, sizeof(ZStoreBarrierEntry));
  __ str(tmp2, Address(tmp1, ZStoreBarrierBuffer::current_offset()));

  // Compute the buffer entry address
  __ lea(tmp2, Address(tmp2, ZStoreBarrierBuffer::buffer_offset()));
  __ add(tmp2, tmp2, tmp1);

  // Compute and log the store address
  __ lea(tmp1, ref_addr);
  __ str(tmp1, Address(tmp2, in_bytes(ZStoreBarrierEntry::p_offset())));

  // Load and log the prev value
  __ ldr(tmp1, tmp1);
  __ str(tmp1, Address(tmp2, in_bytes(ZStoreBarrierEntry::prev_offset())));

  /*
     Label skip, skip2;
  // Calculate base address
  __ relocate(barrier_Relocation::spec(), AgnosticBarrierRelocationFormatSATBBaseAddressBeforeAdd);
  __ add(tmp1, rthread, in_bytes(Thread::gc_data_offset())); // tmp1 := thread-local GC data address

  // ZGC: Patch wordSize to be sizeof(ZStoreBarrierEntry)
  __ relocate(barrier_Relocation::spec(), AgnosticBarrierRelocationFormatPointerBumpScaleBeforeMov);
  __ movzw(rscratch1, wordSize); // rscratch1 := buffer entry size
  __ relocate(barrier_Relocation::spec(), AgnosticBarrierRelocationFormatSATBIndexOffsetBeforeMov);
  __ movzw(rscratch2, in_bytes(G1ThreadLocalData::satb_mark_queue_index_offset())); // rscratch2 := buffer index offset
  __ cmpw(rscratch1, wordSize);
  __ br(Assembler::EQ, skip);

  // Load base address (ZGC only)
  __ ldr(tmp1, tmp1); // tmp1 := *(ZStoreBarrierBuffer)

  // Combined pointer bump and check if the buffer is disabled or full
  __ bind(skip);

  __ ldr(tmp2, Address(tmp1, rscratch2)); // tmp2 := current index
  __ cbz(tmp2, slow_path);

  // Bump the pointer
  __ sub(tmp2, tmp2, rscratch1); // tmp2 := next index
  __ str(tmp2, Address(tmp1, rscratch2)); // current index := next index

  // Compute the buffer entry address
  __ relocate(barrier_Relocation::spec(), AgnosticBarrierRelocationFormatSATBBufferOffsetBeforeAdd);
  __ add(tmp2, tmp2, in_bytes(G1ThreadLocalData::satb_mark_queue_buffer_offset())); // tmp2 := buffer address
  __ add(tmp2, tmp2, tmp1); // tmp2 := buffer address + next index

  // Compute the store address
  __ lea(tmp1, ref_addr); // tmp1 := store address
  __ br(Assembler::EQ, skip2); // Skip to G1 log
                               // Log store address (ZGC only)
                               __ str(tmp1, Address(tmp2, ZStoreBarrierEntry::p_offset())); // *(buffer entry + pointer field) := store address

  // Load and log the prev value (ZGC)
  __ ldr(tmp1, tmp1); // tmp1 := *(store address)
  __ str(tmp1, Address(tmp2, ZStoreBarrierEntry::prev_offset())); // *(buffer entry + prev_val field) := *(store address)
  __ b(continuation);

  // Load and log the prev value (G1)
  __ bind(skip2);
  __ ldr(tmp1, tmp1); // tmp1 := *(store address)
  __ cbz(tmp1, continuation); // Is the previous value null?
  __ str(tmp1, Address(tmp2, 0)); // *(buffer address + next index) := *(store address)
  */
}

void generate_store_barrier_medium_path(MacroAssembler* masm,
    Address ref_addr,
    Register tmp1,
    Register tmp2,
    bool is_native,
    bool is_atomic,
    Label& medium_path_continuation,
    Label& slow_path,
    Label& slow_path_continuation) {
  if (is_native) {
    ShouldNotReachHere();
  } else if (is_atomic) {
    ShouldNotReachHere();
  } else {
    generate_satb_enqueue(masm, ref_addr, tmp1, tmp2, slow_path, medium_path_continuation);
    __ bind(slow_path_continuation);
    __ b(medium_path_continuation);
  }
}

static void generate_store_barrier_fast_path(MacroAssembler* masm,
    Register src,  // g1:new_val,  z:rnew_zaddress (PRESERVE FOR G1)
    Register dst,  // g1:obj,      z:ref_addr (PRESERVE FOR G1)
    Register aux,  // g1:pre_val,  z:rnew_zpointer
    Register tmp1, // g1:tmp1,     z:rtmp
    Register tmp2, // g1:tmp2
    AgnosticStoreBarrierStubC2* stub) {
  const Address ref_addr = Address(dst);
  // assert_different_registers(src, dst, aux, rthread, tmp1, tmp2, noreg);
  // assert(aux != noreg && tmp1 != noreg && tmp2 != noreg, "expecting a register");
  assert_different_registers(ref_addr.base(), aux, tmp1);
  assert_different_registers(ref_addr.index(), aux, tmp1);
  assert_different_registers(src, aux, tmp1);

  if (stub->is_atomic()) {
    ShouldNotReachHere();
  }

  // In G1:
  // - The value representing if SATB marking is active is loaded.
  //   - If marking is NOT active: (tmp2 & tmp1) == 0, since tmp1 = 0, thus Z=1
  //   - If marking is active:     (tmp2 & tmp1) != 0, since tmp1 = 1, thus Z=0
  // In ZGC:
  // - The value of ZPointerStoreBadMask is loaded and tested with the reference address.
  __ ldr(aux, Address(rthread, AgnosticThreadLocalData::satb_condition_offset()));
  __ ldr(tmp1, ref_addr);
  __ tst(tmp1, aux);                    // Z=1 if the result of the bitwise AND is 0
  __ br(Assembler::NE, *stub->entry()); // Jump if Z=0

  __ bind(*stub->continuation());

  // For all GCs, the source address to be stored will be in the "aux" register.
  // In G1, the value 0 will be moved into the "aux" register and subsequently bitwise OR'd with
  // the source address. This is semantically equivalent to a MOV (register) which is an
  // alias of ORR (shifted register) where the first source operand is the zero register and the shift
  // immediate is also zero.
  // In ZGC, the value of ZPointerStoreGoodMask is patched and the new zpointer is colored.
  assert_different_registers(src, aux);
  __ relocate(barrier_Relocation::spec(), ZBarrierRelocationFormatStoreGoodBeforeMov);
  __ movzw(aux, barrier_Relocation::unpatched);
  __ lsl(tmp2, aux, 1);
  __ relocate(barrier_Relocation::spec(), AgnosticBarrierRelocationFormatSrcPointerShiftBeforeOrr);
  __ orr(aux, aux, src, Assembler::LSL, (uint8_t)0);

  Label done;
  __ cbnz(tmp2, done); // Skip if on ZGC
                       // Storing region crossing non-null, is card young?
  __ lsr(tmp1, dst, CardTable::card_shift()); // tmp1 := card address relative to card table base
  __ load_byte_map_base(tmp2);                // tmp2 := card table base address
  __ add(tmp1, tmp1, tmp2);                   // tmp1 := card address
  __ ldrb(tmp2, Address(tmp1));             // tmp2 := card
                                            // Instead of loading clean_card_val and comparing, we exploit the fact that
                                            // the LSB of non-clean cards is always 0, and the LSB of clean cards 1.
  __ tbz(tmp2, 0, done);
  static_assert(CardTable::dirty_card_val() == 0, "must be to use zr");
  __ strb(zr, Address(tmp1));                 // *(card address) := dirty_card_val
  __ bind(done);
}

void AgnosticBarrierSetAssembler::generate_store_barrier_stub_c2(MacroAssembler* masm, AgnosticStoreBarrierStubC2* stub) const {
  Assembler::InlineSkippedInstructionsCounter skipped_counter(masm);
  BLOCK_COMMENT("AgnosticStoreBarrierStubC2");

  __ bind(*stub->entry());

  Label slow;
  Label slow_continuation;

  generate_store_barrier_medium_path(masm,
      stub->dst(),
      stub->tmp1(),
      stub->tmp2(),
      false /* is_native */,
      stub->is_atomic(),
      *stub->continuation(),
      slow,
      slow_continuation);

  __ bind(slow);

  {
    BLOCK_COMMENT("Z Runtime Call");
    SaveLiveRegisters save_live_registers(masm, stub);
    __ lea(c_rarg0, stub->dst());

    if (stub->is_atomic()) {
      __ lea(rscratch1, RuntimeAddress(ZBarrierSetRuntime::store_barrier_on_oop_field_with_healing_addr()));
    } else if (stub->is_nokeepalive()) {
      __ lea(rscratch1, RuntimeAddress(ZBarrierSetRuntime::no_keepalive_store_barrier_on_oop_field_without_healing_addr()));
    } else {
      __ lea(rscratch1, RuntimeAddress(ZBarrierSetRuntime::store_barrier_on_oop_field_without_healing_addr()));
    }
    __ blr(rscratch1);
  }

  // Stub exit
  __ b(slow_continuation);
}

void AgnosticBarrierSetAssembler::agnostic_store_barrier(MacroAssembler* masm,
    Register src,  // g1:new_val,  z:rnew_zaddress
    Register dst,  // g1:obj,      z:ref_addr
    Register aux,  // g1:pre_val,  z:rnew_zpointer
    Register tmp1, // g1:tmp1,     z:rtmp
    Register tmp2, // g1:tmp2
    AgnosticStoreBarrierStubC2* stub) const {
  stub->initialize_registers(src, dst, aux, tmp1, tmp2);
  generate_store_barrier_fast_path(masm, src, dst, aux, tmp1, tmp2, stub);
}

#endif
