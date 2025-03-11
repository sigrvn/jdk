/*
 * Copyright (c) 2024, Oracle and/or its affiliates. All rights reserved.
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

#include "precompiled.hpp"
#include "asm/macroAssembler.inline.hpp"
#include "gc/agnostic/agnosticBarrierSetAssembler.hpp"
#include "gc/g1/g1BarrierSetRuntime.hpp"
#include "gc/g1/g1ThreadLocalData.hpp"
#include "gc/z/zAddress.hpp"
#include "gc/z/zBarrierSetAssembler.hpp"
#ifdef COMPILER2
#include "gc/agnostic/c2/agnosticBarrierSetC2.hpp"
#endif // COMPILER2

#define __ masm->

#if defined(COMPILER2)

static void generate_g1_pre_barrier_fast_path(MacroAssembler* masm, 
                                              Register thread,
                                              Register tmp1,
                                              Register tmp2,
                                              G1PreBarrierStubC2* stub) {
  // Is marking active?
  Address in_progress(thread, in_bytes(G1ThreadLocalData::satb_mark_queue_active_offset()));
  if (in_bytes(SATBMarkQueue::byte_width_of_active()) == 4) {
    __ ldrw(tmp1, in_progress);
  } else {
    assert(in_bytes(SATBMarkQueue::byte_width_of_active()) == 1, "Assumption");
    __ ldrb(tmp1, in_progress);
  }
  // If marking is active (*(mark queue active address) != 0), jump to stub (slow path)
  __ cbnzw(tmp1, *stub->entry());
}

static void generate_g1_post_barrier_fast_path(MacroAssembler* masm,
                                               const Register store_addr,
                                               const Register new_val,
                                               const Register thread,
                                               const Register tmp1,
                                               const Register tmp2,
                                               Label& done,
                                               bool new_val_maybe_null) {
  assert(thread == rthread, "must be");
  assert_different_registers(store_addr, new_val, thread, tmp1, tmp2, noreg);

  // Does store cross heap regions?
  __ eor(tmp1, store_addr, new_val);                     // tmp1 := store address ^ new value
  __ lsr(tmp1, tmp1, G1HeapRegion::LogOfHRGrainBytes);   // tmp1 := ((store address ^ new value) >> LogOfHRGrainBytes)
  __ cbz(tmp1, done);

  // Crosses regions, storing null?
  if (new_val_maybe_null) {
    __ cbz(new_val, done);
  }
  // Storing region crossing non-null, is card young?

  __ lsr(tmp1, store_addr, CardTable::card_shift());     // tmp1 := card address relative to card table base

  Address card_table_addr(thread, in_bytes(G1ThreadLocalData::card_table_base_offset()));
  __ ldr(tmp2, card_table_addr);                         // tmp2 := card table base address
  __ add(tmp1, tmp1, tmp2);                              // tmp1 := card address
  if (UseCondCardMark) {
    __ ldrb(tmp2, Address(tmp1));                        // tmp2 := card
    // Instead of loading clean_card_val and comparing, we exploit the fact that
    // the LSB of non-clean cards is always 0, and the LSB of clean cards 1.
    __ tbz(tmp2, 0, done);
  }
  static_assert(G1CardTable::dirty_card_val() == 0, "must be to use zr");
  __ strb(zr, Address(tmp1));                            // *(card address) := dirty_card_val
}


static void generate_z_pre_barrier_fast_path(MacroAssembler* masm,
                                             Address ref_addr,
                                             Register rnew_zaddress,
                                             Register rnew_zpointer,
                                             Register rtmp,
                                             ZStoreBarrierStubC2* stub) {
  __ ldr(rtmp, ref_addr);
  __ relocate(barrier_Relocation::spec(), ZBarrierRelocationFormatStoreBadBeforeMov);
  __ movzw(rnew_zpointer, barrier_Relocation::unpatched);
  __ tst(rnew_zaddress, rnew_zpointer);
  __ br(Assembler::NE, *stub->entry());
  __ bind(*stub->continuation());
  assert_different_registers(rnew_zaddress, rnew_zpointer);
  __ relocate(barrier_Relocation::spec(), ZBarrierRelocationFormatStoreGoodBeforeMov);
  __ movzw(rnew_zpointer, barrier_Relocation::unpatched);
  __ orr(rnew_zpointer, rnew_zpointer, rnew_zaddress, Assembler::LSL, ZPointerLoadShift);
}

// Generates the GC-agnostic pre-write barrier fast paths and links the branches to the slow paths.
void AgnosticBarrierSetAssembler::pre_write_barrier(
  MacroAssembler* masm,
  Address ref_addr,
  Register a, // g1:obj,     z:rnew_zaddress
  Register b, // g1:pre_val, z:rnew_zpointer
  Register c, // g1:thread
  Register d, // g1:tmp1,    z:rtmp
  Register e, // g1:tmp2
  G1PreBarrierStubC2* g1_stub,
  ZStoreBarrierStubC2* z_stub
) {
  assert(c == rthread, "must be");
  assert_different_registers(a, b, d, e);
  assert(b != noreg && d != noreg && e != noreg, "expecting a register");
  g1_stub->initialize_registers(a, b, c, d, e);

  assert_different_registers(ref_addr.base(), b, d);
  assert_different_registers(ref_addr.index(), b, d);
  // assert_different_registers(a, b, d);

  generate_g1_pre_barrier_fast_path(masm, 
                                    c /* thread */, 
                                    d /* tmp1 */, 
                                    e /* tmp2 */, g1_stub);

  generate_z_pre_barrier_fast_path(masm, 
                                   ref_addr, 
                                   a /* rnew_zaddress */, 
                                   b /* rnew_zpointer */, 
                                   d /* rtmp */, z_stub);

  __ bind(*g1_stub->continuation()); 
}

void AgnosticBarrierSetAssembler::post_write_barrier(
    MacroAssembler* masm,
    Register store_addr,
    Register new_val,
    Register thread,
    Register tmp1,
    Register tmp2,
    bool new_val_maybe_null
) {
  Label done;
  generate_g1_post_barrier_fast_path(masm,
                                     store_addr,
                                     new_val,
                                     thread,
                                     tmp1,
                                     tmp2,
                                     done,
                                     new_val_maybe_null);
  __ bind(done);
}

#endif // COMPILER2
