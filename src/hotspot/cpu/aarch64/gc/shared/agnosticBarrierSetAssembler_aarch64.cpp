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
#include "gc/z/zBarrierSetAssembler.hpp"
#include "gc/shared/agnosticBarrierSetRuntime.hpp"
#include "interpreter/interp_masm.hpp"
#include "oops/accessDecorators.hpp"
#include "precompiled.hpp"
#include "register_aarch64.hpp"
#include "runtime/javaThread.hpp"
#include "runtime/sharedRuntime.hpp"
#include "utilities/debug.hpp"
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

static void buffer_store(MacroAssembler* masm, Address ref_addr, Register tmp1, Register tmp2, Label& slow_path) {
  assert_different_registers(ref_addr.base(), ref_addr.index(), tmp1, tmp2);
 
  Address buffer(rthread, AgnosticThreadLocalData::agnostic_store_barrier_buffer_offset());

  __ block_comment("START buffer_store");

  // tmp1 <- agnostic buffer address
  __ ldr(tmp1, buffer);

  // Check if the buffer is full. ANTODO: compare and branch optimization
  __ ldr(tmp2, Address(tmp1, AgnosticStoreBarrierBuffer::current_offset()));
  // __ cmp(tmp2, (uint8_t)0);
  // __ br(Assembler::EQ, slow_path);
  __ cbz(tmp2, slow_path);

  // Bump the pointer
  __ sub(tmp2, tmp2, sizeof(AgnosticStoreBarrierEntry));
  __ str(tmp2, Address(tmp1, AgnosticStoreBarrierBuffer::current_offset()));

  // Compute the buffer entry address
  __ lea(tmp2, Address(tmp2, AgnosticStoreBarrierBuffer::buffer_offset()));
  __ add(tmp2, tmp2, tmp1);

  // Compute and log the store address
  __ str(ref_addr.base(), Address(tmp2, in_bytes(AgnosticStoreBarrierEntry::p_offset())));

  // Load and log the prev value
  __ load_heap_oop(tmp1, ref_addr, noreg, noreg, AS_RAW);
  __ str(tmp1, Address(tmp2, in_bytes(AgnosticStoreBarrierEntry::prev_offset())));

  __ block_comment("END buffer_store");
}

void AgnosticBarrierSetAssembler::agnostic_store_barrier_c2(MacroAssembler *masm, Address obj, Register pre_val, Register new_val,
                                                            Register thread, Register tmp1, Register tmp2, AgnosticStoreBarrierStubC2 *stub) {
  assert(thread == rthread, "must be");
  assert_different_registers(pre_val, new_val, tmp1, tmp2);
  assert(pre_val != noreg && tmp1 != noreg && tmp2 != noreg, "expecting a register");

  __ block_comment("START inline barrier");

  // Unconditionally buffer stores. Jump to stub where that happens
  buffer_store(masm, obj, pre_val, tmp1, *stub->entry());
  
  __ bind(*stub->continuation());

  __ block_comment("END inline barrier");
}

void AgnosticBarrierSetAssembler::generate_store_barrier_stub_c2(MacroAssembler* masm, AgnosticStoreBarrierStubC2* stub) const {
  Assembler::InlineSkippedInstructionsCounter skipped_counter(masm);

  __ block_comment("START agnostic stub");

  // Stub entry
  __ bind(*stub->entry());

  Address ref_addr = stub->ref_addr();

  // Runtime call happens in this scope
  {
    SaveLiveRegisters save_live_registers(masm, stub);

    __ lea(c_rarg0, ref_addr);
    __ lea(rscratch1, RuntimeAddress(AgnosticBarrierSetRuntime::buffer_full_addr()));
    __ blr(rscratch1);
  }

  // Stub exit
  __ b(*stub->continuation());

  __ block_comment("END agnostic stub");
}

#undef __
