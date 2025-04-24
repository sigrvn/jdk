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
#include "gc/agnostic/c2/agnosticBarrierSetC2.hpp"
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
    const Register pre_val,
    const Register tmp1,
    const Register tmp2,
    Label& slow_path,
    Label& continuation) {
  BLOCK_COMMENT("SATB Insertion");

  // Load the base address for the SATB structure.
  __ ldr(tmp1, Address(rthread, AgnosticThreadLocalData::satb_base_address_offset()));

  // Combined pointer bump and check if the buffer is disabled or full
  __ relocate(barrier_Relocation::spec(), AgnosticBarrierRelocationFormatSATBIndexOffsetBeforeLdr);
  __ ldr(tmp2, Address(tmp1, in_bytes(SATBMarkQueue::byte_offset_of_index()))); // ZGC: patch to ZStoreBarrierBuffer::current_offset()
  __ cbz(tmp2, slow_path);

  // Bump the pointer
  __ relocate(barrier_Relocation::spec(), AgnosticBarrierRelocationFormatSATBPointerBumpBeforeSub);
  __ sub(tmp2, tmp2, wordSize); // ZGC: patch to sizeof(ZStoreBarrierEntry)
  __ relocate(barrier_Relocation::spec(), AgnosticBarrierRelocationFormatSATBIndexOffsetBeforeLdr);
  __ str(tmp2, Address(tmp1, in_bytes(SATBMarkQueue::byte_offset_of_index())));

  // Compute the buffer entry address
  __ relocate(barrier_Relocation::spec(), AgnosticBarrierRelocationFormatSATBBufferOffsetBeforeAdd);
  __ add(tmp2, tmp2, in_bytes(SATBMarkQueue::byte_offset_of_buf())); // ZGC: patch to ZStoreBarrierBuffer::buffer_offset()
  __ add(tmp2, tmp2, tmp1);

  // In G1, the previous value has already been loaded and can be inserted into the address in tmp2.
  // In ZGC, we must load the previous value and log both it and the store address.
  Label skip_load;
  __ cbnz(pre_val, skip_load);

  // Compute and log the store address
  __ lea(tmp1, ref_addr);
  __ str(tmp1, Address(tmp2, in_bytes(ZStoreBarrierEntry::p_offset())));

  // Load and log the prev value
  __ ldr(tmp1, ref_addr);
  __ str(tmp1, Address(tmp2, in_bytes(ZStoreBarrierEntry::prev_offset())));
  __ b(continuation);

  __ bind(skip_load);
  __ str(pre_val, Address(tmp2));
}

static void generate_store_barrier_fast_path(MacroAssembler* masm,
    Register src,  // g1:new_val,  z:rnew_zaddress (PRESERVE FOR G1)
    Register dst,  // g1:obj,      z:ref_addr (PRESERVE FOR G1)
    Register aux,  // g1:pre_val,  z:rnew_zpointer
    Register tmp1, // g1:tmp1,     z:rtmp
    Register tmp2, // g1:tmp2
    AgnosticStoreBarrierStubC2* stub) {
  BLOCK_COMMENT("Agnostic Store Barrier Fast Path");
  const Address ref_addr = Address(dst);
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
  // In Serial and Parallel:
  // - The SATB condition is always 0, therefore the branch to the medium/slow path is never taken.
  //   Proceed to card marking.
  // __ relocate(barrier_Relocation::spec(), ZBarrierRelocationFormatStoreBadBeforeMov);
  // __ movzw(aux, barrier_Relocation::unpatched);
  __ ldr(aux, Address(rthread, AgnosticThreadLocalData::satb_condition_offset()));
  __ ldr(tmp1, ref_addr);
  __ tst(tmp1, aux);                    // Z=1 if the result of the bitwise AND is 0
  __ br(Assembler::NE, *stub->entry()); // Jump if Z=0

  __ bind(*stub->continuation());

  // For all GCs, the source address to be stored will be in the "aux" register.
  // In G1, Serial, and Parallel, the value 0 will be moved into the "aux" register and
  // subsequently bitwise OR'd with the source address. This is semantically equivalent to a MOV (register)
  // which is an alias of ORR (shifted register) where the first source operand is the zero register and
  // the shift immediate is also zero.
  // In ZGC, the value of ZPointerStoreGoodMask is patched and the pointer is colored.
  assert_different_registers(src, aux);
  __ relocate(barrier_Relocation::spec(), ZBarrierRelocationFormatStoreGoodBeforeMov);
  __ movzw(aux, barrier_Relocation::unpatched);
  __ lsl(tmp2, aux, 1); // Set register to skip card marking if on ZGC
  __ relocate(barrier_Relocation::spec(), AgnosticBarrierRelocationFormatSrcPointerShiftBeforeOrr);
  __ orr(aux, aux, src, Assembler::LSL, (uint8_t)0);

  Label done;
  __ cbnz(tmp2, done);
  __ lsr(tmp1, dst, CardTable::card_shift()); // tmp1 := card address relative to card table base
  __ ldr(tmp2, Address(rthread, in_bytes(CardTableThreadLocalData::byte_map_base_offset()))); // tmp2 := card table base address
  __ add(tmp1, tmp1, tmp2);                   // tmp1 := card address
  if (UseCondCardMark) {
    __ ldrb(tmp2, Address(tmp1));             // tmp2 := card
    // Instead of loading clean_card_val and comparing, we exploit the fact that
    // the LSB of non-clean cards is always 0, and the LSB of clean cards 1.
    __ tbz(tmp2, 0, done);
  }
  static_assert(G1CardTable::dirty_card_val() == 0, "must be to use zr");
  __ strb(zr, Address(tmp1));                            // *(card address) := dirty_card_val
  __ bind(done);

  // Do card marking if on G1, Serial, or Parallel.
  //Label done;
  //__ cbnz(tmp2, done);
  //__ lsr(tmp1, dst, CardTable::card_shift()); // tmp1 := card address relative to card table base
  //__ load_byte_map_base(tmp2);                // tmp2 := card table base address
  //__ add(tmp1, tmp1, tmp2);                   // tmp1 := card address
  //__ ldrb(tmp2, Address(tmp1));               // tmp2 := card
  //// Instead of loading clean_card_val and comparing, we exploit the fact that
  //// the LSB of non-clean cards is always 0, and the LSB of clean cards 1.
  //__ tbz(tmp2, 0, done);
  //static_assert(CardTable::dirty_card_val() == 0, "must be to use zr");
  //__ strb(zr, Address(tmp1));                 // *(card address) := dirty_card_val
  //__ bind(done);
}

static void generate_store_barrier_medium_path(MacroAssembler* masm,
    Register dst,
    Register aux,
    Register tmp1,
    Register tmp2,
    bool is_native,
    bool is_atomic,
    Label& medium_path_continuation,
    Label& slow_path,
    Label& slow_path_continuation) {
  BLOCK_COMMENT("Agnostic Store Barrier Medium Path");
  if (is_native) {
    ShouldNotReachHere();
  } else if (is_atomic) {
    ShouldNotReachHere();
  } else {
    Label zpre, enqueue;
    // At this point in G1, pre_val will have the value of the active flag in SATBMarkQueue, which is 1.
    // Thus, we can use the pre_val register to skip G1 specific operations if we're on ZGC.
    __ cmp(aux, (uint8_t)1);
    __ br(Assembler::NE, zpre);

    // Load the previous value for G1.
    if (dst != noreg) {
      __ load_heap_oop(aux, Address(dst, 0), noreg, noreg, AS_RAW);
    }
    // We don't want to enqueue if the previous value was null.
    __ cbz(aux, medium_path_continuation);
    __ b(enqueue);

    // Set aux to 0 to branch to ZGC specific code for SATB
    __ bind(zpre);
    __ movzw(aux, (uint8_t)0);

    __ bind(enqueue);
    generate_satb_enqueue(masm, dst, aux, tmp1, tmp2, slow_path, medium_path_continuation);
    __ bind(slow_path_continuation);
    __ b(medium_path_continuation);
  }
}


void generate_store_barrier_slow_path(MacroAssembler* masm, Label& slow_continuation, AgnosticStoreBarrierStubC2* stub) {
  BLOCK_COMMENT("Agnostic Store Barrier Slow Path");
  SaveLiveRegisters save_live_registers(masm, stub);
  Label z_runtime;
  // Conditionally select either the reference address if on ZGC or the previous value if on G1
  // from previously-set flag in generate_store_barrier_medium_path.
  __ csel(c_rarg0, stub->dst(), stub->aux(), Assembler::NE);
  __ br(Assembler::NE, z_runtime);

  BLOCK_COMMENT("G1 Runtime Call");
  __ lea(c_rarg1, rthread);
  __ mov(rscratch1, CAST_FROM_FN_PTR(address, G1BarrierSetRuntime::write_ref_field_pre_entry));
  __ blr(rscratch1);

  // Stub exit
  __ b(slow_continuation);

  BLOCK_COMMENT("Z Runtime Call");
  __ bind(z_runtime);
  if (stub->is_native()) {
    __ lea(rscratch1, RuntimeAddress(ZBarrierSetRuntime::store_barrier_on_native_oop_field_without_healing_addr()));
  } else if (stub->is_atomic()) {
    __ lea(rscratch1, RuntimeAddress(ZBarrierSetRuntime::store_barrier_on_oop_field_with_healing_addr()));
  } else if (stub->is_nokeepalive()) {
    __ lea(rscratch1, RuntimeAddress(ZBarrierSetRuntime::no_keepalive_store_barrier_on_oop_field_without_healing_addr()));
  } else {
    __ lea(rscratch1, RuntimeAddress(ZBarrierSetRuntime::store_barrier_on_oop_field_without_healing_addr()));
  }
  __ blr(rscratch1);
}

void AgnosticBarrierSetAssembler::generate_store_barrier_stub_c2(MacroAssembler* masm, AgnosticStoreBarrierStubC2* stub) const {
  Assembler::InlineSkippedInstructionsCounter skipped_counter(masm);
  BLOCK_COMMENT("AgnosticStoreBarrierStubC2");

  __ bind(*stub->entry());

  Label slow;
  Label slow_continuation;

  generate_store_barrier_medium_path(masm,
      stub->dst(),
      stub->aux(),
      stub->tmp1(),
      stub->tmp2(),
      stub->is_native(),
      stub->is_atomic(),
      *stub->continuation(),
      slow,
      slow_continuation);

  __ bind(slow);
  generate_store_barrier_slow_path(masm, slow_continuation, stub);

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
