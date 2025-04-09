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
#include "gc/g1/g1ThreadLocalData.hpp"
#include "gc/g1/g1BarrierSetRuntime.hpp"
#include "gc/z/zBarrierSetAssembler.hpp"
#include "gc/z/zBarrierSetRuntime.hpp"
#include "gc/z/zStoreBarrierBuffer.hpp"

#ifdef PRODUCT
#define BLOCK_COMMENT(str) /* nothing */
#else
#define BLOCK_COMMENT(str) __ block_comment(str)
#endif

#undef __
#define __ masm->

#if defined(COMPILER2)
// Generate patchable code stub for inserting into either G1's mark queue or ZGC's store buffer.
static void generate_satb_insertion(MacroAssembler* masm,
    const Address ref_addr,
    const Register pre_val,
    const Register tmp1,
    const Register tmp2,
    Label& runtime,
    Label& continuation) {
  assert_different_registers(ref_addr.base(), ref_addr.index(), tmp1, tmp2);
  uint16_t index_offset = in_bytes(G1ThreadLocalData::satb_mark_queue_index_offset());
  // ZGC: Patch 'index_offset' to be ZThreadLocalData::store_barrier_buffer_offset()
  __ relocate(barrier_Relocation::spec(), AgnosticBarrierRelocationFormatAddrOffsetSlowBeforeLdr);
  __ ldr(tmp1, Address(rthread, index_offset));

  // ZGC: Patch '0' to be ZStoreBarrierBuffer::current_offset()
  __ relocate(barrier_Relocation::spec(), AgnosticBarrierRelocationFormatAddrOffsetSlowIndexBeforeLdr);
  __ ldr(tmp2, Address(tmp1, (uint16_t)0));
  __ cbz(tmp2, runtime);

  // ZGC: Patch 'wordSize' to be sizeof(ZStoreBarrierEntry)
  __ relocate(barrier_Relocation::spec(), AgnosticBarrierRelocationFormatPointerBumpScaleBeforeSub);
  __ sub(tmp2, tmp2, wordSize);
  // ZGC: Patch 'index_offset' to be ZThreadLocalData::store_barrier_buffer_offset() + ZStoreBarrierBuffer::current_offset()
  __ relocate(barrier_Relocation::spec(), AgnosticBarrierRelocationFormatPointerBumpOffsetBeforeStr);
  __ str(tmp2, Address(rthread, index_offset));

  // Compute the buffer entry address
   __ lea(tmp2, Address(tmp2, ZStoreBarrierBuffer::buffer_offset()));
   __ add(tmp2, tmp2, tmp1);

  // Compute and log the store address
   __ lea(tmp1, ref_addr);
   __ str(tmp1, Address(tmp2, in_bytes(ZStoreBarrierEntry::p_offset())));

  // Load and log the prev value
   __ ldr(tmp1, tmp1);
   __ str(tmp1, Address(tmp2, in_bytes(ZStoreBarrierEntry::prev_offset())));
   __ b(continuation);

  // G1: Store to SATB
  __ ldr(tmp2, Address(rthread, in_bytes(G1ThreadLocalData::satb_mark_queue_buffer_offset())));
  __ str(pre_val, Address(tmp2, tmp1));
}

static void generate_store_barrier_slow_path(MacroAssembler* masm, Label& runtime,
                                             AgnosticStoreBarrierStubC2* stub) {
  Register dst = stub->dst();
  Register pre_val = stub->pre_val();
  Register tmp1 = stub->tmp1();
  Register tmp2 = stub->tmp2();

  if (stub->is_initialized()) {
    // // Do we need to load the previous value?
    // if (dst != noreg) {
    //   __ load_heap_oop(pre_val, Address(dst, 0), noreg, noreg, AS_RAW);
    // }
    // // Is the previous value null?
    // __ cbz(pre_val, *stub->continuation());
    generate_satb_insertion(masm, dst, pre_val, tmp1, tmp2, runtime, *stub->continuation());
  }

  __ b(*stub->continuation());
}

static void generate_store_barrier_runtime_call(MacroAssembler* masm, AgnosticStoreBarrierStubC2* stub) {
  SaveLiveRegisters save_live_registers(masm, stub);

  // Generate G1 runtime call to insert into the SATB mark queue if the current queue is full.
  if (stub->is_initialized()) {
    Register arg = stub->pre_val();
    if (c_rarg0 != arg) {
      __ mov(c_rarg0, arg);
    }
    __ mov(c_rarg1, rthread);
    __ mov(rscratch1, CAST_FROM_FN_PTR(address, G1BarrierSetRuntime::write_ref_field_pre_entry));
    __ blr(rscratch1);
  }

  // Generate ZGC runtime call to insert into store buffer if full or inactive and maintain remsets.
  if (stub->in_heap()) {
    Address ref_addr = stub->dst();
    __ lea(c_rarg0, ref_addr);
    if (stub->is_atomic()) {
      __ lea(rscratch1, RuntimeAddress(ZBarrierSetRuntime::store_barrier_on_oop_field_with_healing_addr()));
    } else if (stub->is_nokeepalive()) {
      __ lea(rscratch1, RuntimeAddress(ZBarrierSetRuntime::no_keepalive_store_barrier_on_oop_field_without_healing_addr()));
    } else {
      __ lea(rscratch1, RuntimeAddress(ZBarrierSetRuntime::store_barrier_on_oop_field_without_healing_addr()));
    }
    __ blr(rscratch1);
  }
}

void AgnosticBarrierSetAssembler::generate_store_barrier_stub(MacroAssembler* masm, 
                                                              AgnosticStoreBarrierStubC2* stub) const {
  Assembler::InlineSkippedInstructionsCounter skipped_counter(masm);
  BLOCK_COMMENT("AgnosticStoreBarrierStubC2");

  Label runtime;
  __ bind(*stub->entry());
  generate_store_barrier_slow_path(masm, runtime, stub);

  __ bind(runtime);
  generate_store_barrier_runtime_call(masm, stub);
  __ b(*stub->continuation());
}

static void generate_store_barrier_fast_path(MacroAssembler* masm,
    Register src,  // g1:new_val,  z:rnew_zaddress
    Register dst,  // g1:obj,      z:ref_addr
    Register aux,  // g1:pre_val,  z:rnew_zpointer
    Register tmp1, // g1:tmp1,     z:rtmp
    Register tmp2, // g1:tmp2
    AgnosticStoreBarrierStubC2* stub) {
  const Address ref_addr = Address(dst);
  assert_different_registers(src, dst, aux, rthread, tmp1, tmp2, noreg);
  assert(aux != noreg && tmp1 != noreg && tmp2 != noreg, "expecting a register");
  assert_different_registers(ref_addr.base(), aux, tmp1);
  assert_different_registers(ref_addr.index(), aux, tmp1);

  // In G1:
  // - If marking is NOT active: (tmp2 & tmp1) == 0, since tmp1 = 0, thus Z=1
  // - If marking is active:     (tmp2 & tmp1) != 0, since tmp1 = 1, thus Z=0
  // In ZGC:
  // - The value of ZPointerStoreBadMask is loaded and compared with the reference address.
  __ ldr(tmp1, Address(rthread, Thread::gc_agnostic_data_offset()));
  __ ldr(tmp2, ref_addr);
  __ tst(tmp2, tmp1);                   // Z=1 if the result of the bitwise AND is 0
  __ br(Assembler::NE, *stub->entry()); // Jump if Z=0

  __ bind(*stub->continuation());

  // For all GCs, the source address to be stored will be in the "aux" register.
  // In G1, the value 0 will be moved into the "aux" register and subsequently bitwise OR'd with
  // the source address. This is semantically equivalent to a MOV (register) which is an
  // alias of ORR (shifted register) where the first source operand is the zero register and the shift
  // immediate is also zero.
  // In ZGC, the value of ZPointerStoreGoodMask is patched and bitwise OR'd with the source address
  // shifted by ZPointerLoadShift.
  __ relocate(barrier_Relocation::spec(), ZBarrierRelocationFormatStoreGoodBeforeMov);
  __ movzw(aux, barrier_Relocation::unpatched);
  __ lsl(tmp1, aux, 1);
  __ relocate(barrier_Relocation::spec(), AgnosticBarrierRelocationFormatSrcPointerShiftBeforeOrr);
  __ orr(aux, aux, src, Assembler::LSL, (uint8_t)0);

  if (stub->in_heap()) {
    Label done;
    __ cbnz(tmp1, done);
    // Storing region crossing non-null, is card young?
    __ lsr(tmp1, dst, CardTable::card_shift()); // tmp1 := card address relative to card table base
    __ load_byte_map_base(tmp2);                // tmp2 := card table base address
    __ add(tmp1, tmp1, tmp2);                   // tmp1 := card address
    if (UseCondCardMark) {
      __ ldrb(tmp2, Address(tmp1));             // tmp2 := card
      // Instead of loading clean_card_val and comparing, we exploit the fact that
      // the LSB of non-clean cards is always 0, and the LSB of clean cards 1.
      __ tbz(tmp2, 0, done);
    }
    static_assert(CardTable::dirty_card_val() == 0, "must be to use zr");
    __ strb(zr, Address(tmp1));                 // *(card address) := dirty_card_val
    __ bind(done);
  }
}

void AgnosticBarrierSetAssembler::store_barrier(MacroAssembler* masm,
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
