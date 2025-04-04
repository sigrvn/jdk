/*
 * Copyright (c) 2025, Oracle and/or its affiliates. All rights reserved.
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

#include "gc/shared/agnosticBarrierSetAssembler_aarch64.hpp"
#include "asm/assembler.hpp"
#include "asm/register.hpp"
#include "assembler_aarch64.hpp"
#include "gc/shared/agnosticThreadLocalData.hpp"
#include "asm/macroAssembler.inline.hpp"
#include "gc/g1/g1BarrierSet.hpp"
#include "gc/g1/g1BarrierSetAssembler.hpp"
#include "gc/g1/g1BarrierSetRuntime.hpp"
#include "gc/g1/g1CardTable.hpp"
#include "gc/g1/g1HeapRegion.hpp"
#include "gc/g1/g1ThreadLocalData.hpp"
#include "gc/shared/collectedHeap.hpp"
//#include "gc/z/zBarrierSetAssembler_aarch64.hpp"
#include "gc/shared/agnosticBarrierSetRuntime.hpp"
#include "interpreter/interp_masm.hpp"
#include "oops/accessDecorators.hpp"
#include "precompiled.hpp"
#include "register_aarch64.hpp"
#include "runtime/javaThread.hpp"
#include "runtime/sharedRuntime.hpp"
#include "utilities/globalDefinitions.hpp"
#ifdef COMPILER1
#include "c1/c1_LIRAssembler.hpp"
#include "c1/c1_MacroAssembler.hpp"
#include "gc/g1/c1/g1BarrierSetC1.hpp"
#endif // COMPILER1
#ifdef COMPILER2
#include "gc/g1/c2/g1BarrierSetC2.hpp"
#endif // COMPILER2

#define __ masm->

static void buffer_store(MacroAssembler* masm,
                         Address ref_addr,
                         Register tmp1,
                         Register tmp2,
                         Label& slow_path) {
  Address buffer(rthread, AgnosticThreadLocalData::agnostic_store_barrier_buffer_offset());
  assert_different_registers(ref_addr.base(), ref_addr.index(), tmp1, tmp2);

  __ block_comment("Buffering badger");

  __ ldr(tmp1, buffer);

  // Combined pointer bump and check if the buffer is disabled or full
  __ ldr(tmp2, Address(tmp1, AgnosticStoreBarrierBuffer::current_offset()));
  __ cmp(tmp2, (uint8_t)0);
  __ br(Assembler::EQ, slow_path);

  // Bump the pointer
  __ sub(tmp2, tmp2, sizeof(AgnosticStoreBarrierEntry));
  __ str(tmp2, Address(tmp1, AgnosticStoreBarrierBuffer::current_offset()));

  // Compute the buffer entry address
  __ lea(tmp2, Address(tmp2, AgnosticStoreBarrierBuffer::buffer_offset()));
  __ add(tmp2, tmp2, tmp1);

  // Compute and log the store address
  __ lea(tmp1, ref_addr);
  __ str(tmp1, Address(tmp2, in_bytes(AgnosticStoreBarrierEntry::p_offset())));

  // Load and log the prev value
  __ load_heap_oop(tmp1, ref_addr, noreg, noreg, AS_RAW);
  //__ ldr(tmp1, tmp1);
  __ str(tmp1, Address(tmp2, in_bytes(AgnosticStoreBarrierEntry::prev_offset())));
}

void AgnosticBarrierSetAssembler::agnostic_store_barrier_c2(MacroAssembler *masm, Address obj, Register pre_val, Register new_val,
                                                            Register thread, Register tmp1, Register tmp2, AgnosticStoreBarrierStubC2 *stub) {
  assert(thread == rthread, "must be");
  assert_different_registers(pre_val, tmp1, tmp2);
  assert(pre_val != noreg && tmp1 != noreg && tmp2 != noreg,
         "expecting a register");

  //stub->initialize_registers(obj, pre_val, thread, tmp1, tmp2);

  Label done;

  __ block_comment("Antón es trilisto");
  // If we are storing NULL, there is nothing to be done; otherwise jump to slow path
  //__ cbnzw(pre_val, *stub->entry());
  __ b(*stub->entry());
  

  __ bind(done);
  __ bind(*stub->continuation());
}

void AgnosticBarrierSetAssembler::generate_store_barrier_stub_c2(MacroAssembler* masm, AgnosticStoreBarrierStubC2* stub) const {
  Assembler::InlineSkippedInstructionsCounter skipped_counter(masm);

  // Stub entry
  __ bind(*stub->entry());

  Label slow;
  Label slow_continuation;
  Address ref_addr = stub->ref_addr();
  Register prev_val = stub->prev_val();
  Register tmp = stub->tmp();
  bool is_native=false;// = stub->is_native();
  bool is_atomic=false;// = stub->is_atomic();
  Register n = stub->n();
  Label& medium_path_continuation = *stub->continuation();
  Label& slow_path = slow;
  Label& slow_path_continuation = slow_continuation;

  assert_different_registers(ref_addr.base(), ref_addr.index(), prev_val, tmp, n);

  // The reason to end up in the medium path is that the pre-value was not 'good'.

  if (is_native) {
    __ b(slow_path);
    __ bind(slow_path_continuation);
    __ b(medium_path_continuation);
  } else if (is_atomic) {

  } else {
    // A non-atomic relocatable object won't get to the medium fast path due to a
    // raw null in the young generation. We only get here because the field is bad.
    // In this path we don't need any self healing, so we can avoid a runtime call
    // most of the time by buffering the store barrier to be applied lazily.
    buffer_store(masm, ref_addr, prev_val, tmp, slow_path);

    __ bind(slow_path_continuation);
    __ b(medium_path_continuation);
  }

  __ bind(slow);

  {
    SaveLiveRegisters save_live_registers(masm, stub);
    //__ load_heap_oop(prev_val, ref_addr, noreg, noreg, AS_RAW);
    //__ mov(c_rarg0, prev_val);
    __ lea(c_rarg0, ref_addr);
    __ mov(c_rarg1, rthread);
    assert_different_registers(c_rarg0, c_rarg1, c_rarg4);
    __ encode_heap_oop(c_rarg2, n);
    //__ mov(c_rarg2, n);

    __ lsr(c_rarg4, c_rarg0, CardTable::card_shift());     // tmp1 := card address relative to card table base

    Address card_table_addr(rthread, in_bytes(G1ThreadLocalData::card_table_base_offset()));
    __ ldr(c_rarg3, card_table_addr);                         // tmp2 := card table base address
    __ add(c_rarg3, c_rarg3, c_rarg4);

    // if (stub->is_native()) {
    //   __ lea(rscratch1, RuntimeAddress(ZBarrierSetRuntime::store_barrier_on_native_oop_field_without_healing_addr()));
    // } else if (stub->is_atomic()) {
    //   __ lea(rscratch1, RuntimeAddress(ZBarrierSetRuntime::store_barrier_on_oop_field_with_healing_addr()));
    // } else if (stub->is_nokeepalive()) {
    //   __ lea(rscratch1, RuntimeAddress(ZBarrierSetRuntime::no_keepalive_store_barrier_on_oop_field_without_healing_addr()));
    // } else {
      __ lea(rscratch1, RuntimeAddress(AgnosticBarrierSetRuntime::buffer_full_addr()));
    // }
    __ blr(rscratch1);
  }

  // Stub exit
  __ b(slow_continuation);
}

static void store_barrier_buffer_add(MacroAssembler* masm,
                                     Address ref_addr,
                                     Register tmp1,
                                     Register tmp2,
                                     Label& slow_path) {
  Address buffer(rthread, ZThreadLocalData::store_barrier_buffer_offset());
  assert_different_registers(ref_addr.base(), ref_addr.index(), tmp1, tmp2);

  __ ldr(tmp1, buffer);

  // Combined pointer bump and check if the buffer is disabled or full
  __ ldr(tmp2, Address(tmp1, AgnosticStoreBarrierBuffer::current_offset()));
  __ cmp(tmp2, (uint8_t)0);
  __ br(Assembler::EQ, slow_path);

  // Bump the pointer
  __ sub(tmp2, tmp2, sizeof(AgnosticStoreBarrierEntry));
  __ str(tmp2, Address(tmp1, ZStoreBarrierBuffer::current_offset()));

  // Compute the buffer entry address
  __ lea(tmp2, Address(tmp2, AgnosticStoreBarrierBuffer::buffer_offset()));
  __ add(tmp2, tmp2, tmp1);

  // Compute and log the store address
  __ lea(tmp1, ref_addr);
  __ str(tmp1, Address(tmp2, in_bytes(AgnosticStoreBarrierEntry::p_offset())));

  // Load and log the prev value
  __ ldr(tmp1, tmp1);
  __ str(tmp1, Address(tmp2, in_bytes(AgnosticStoreBarrierEntry::prev_offset())));
}

#undef __
