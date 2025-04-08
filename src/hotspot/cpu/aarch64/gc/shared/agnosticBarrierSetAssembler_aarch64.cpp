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
  assert_different_registers(pre_val, new_val, tmp1, tmp2);
  assert(pre_val != noreg && tmp1 != noreg && tmp2 != noreg,
         "expecting a register");

  //stub->initialize_registers(obj, pre_val, thread, tmp1, tmp2);

  __ block_comment("Antón es trilisto");
  // If we are storing NULL, there is nothing to be done; otherwise jump to slow path
  //__ cbnzw(pre_val, *stub->entry());
  __ b(*stub->entry());
  
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
    ShouldNotReachHere();
    __ b(slow_path);
    __ bind(slow_path_continuation);
    __ b(medium_path_continuation);
  } else if (is_atomic) {
    ShouldNotReachHere();
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

    // Paranoia: we're sure c_rarg0, c_rarg1 and c_rarg2 are different, but they
    // may be n or ref_addr.base() or ref_addr.index().
    // Solution:
    if (c_rarg0 != n) {
      // If we write into c_rarg0 first we don't lose n
      __ lea(c_rarg0, ref_addr);
      // Now write n into c_rarg1. We may be overwritting ref_addr fields but we don't care
      __ mov(c_rarg1, n);
    } else {
      // c_rarg0 is n. We need to put n into c_rarg1. But c_rarg1 may be a field of ref_addr
      if (c_rarg1 != ref_addr.base() && c_rarg1 != ref_addr.index()) {
        // Okay, it is not. So we can put n into there without overwritting things
        __ mov(c_rarg1, n);
        // By definition of things, ref_addr fields cannot be in c_rarg0 (as it is n)
        __ lea(c_rarg0, ref_addr);
        // Sanity check:
        assert_different_registers(c_rarg1, c_rarg0, ref_addr.base(), ref_addr.index());
      } else {
        // Now, one of the ref_addr fields is in c_rarg1. And also n == c_rarg0, so we can put
        // the address into rscratch1 (rscratch1 != c_rarg0 != c_rarg1)
        __ lea(rscratch1, ref_addr);
        // ref_addr is "saved" into rscratch1, so we can move n (c_rarg0) into c_rarg1
        __ mov(c_rarg1, n);
        // And bring back the address into c_rarg0
        __ mov(c_rarg0, rscratch1);
        // Sanity check:
        assert_different_registers(c_rarg0, c_rarg1, rscratch1);
      }
    }

    __ lea(rscratch1, RuntimeAddress(AgnosticBarrierSetRuntime::buffer_full_addr()));
    __ blr(rscratch1);
  }

  // Stub exit
  __ b(slow_continuation);
}

#undef __
