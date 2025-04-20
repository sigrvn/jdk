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

#include "gc/shared/c2/agnosticBarrierSetC2.hpp"
#include "gc/g1/c2/g1BarrierSetC2.hpp"
#include "gc/shared/agnosticBarrierSetAssembler.hpp"
#include "gc/shared/c2/modRefBarrierSetC2.hpp"
#include "gc/z/c2/zBarrierSetC2.hpp"
#include "opto/output.hpp"
#include <cstdio>


class AgnosticBarrierSetC2State : public BarrierSetC2State {
private:
  GrowableArray<AgnosticBarrierStubC2*>* _stubs;
  int                                    _trampoline_stubs_count;
  int                                    _stubs_start_offset;

public:
  AgnosticBarrierSetC2State(Arena* arena)
    : BarrierSetC2State(arena),
      _stubs(new (arena) GrowableArray<AgnosticBarrierStubC2*>(arena, 8,  0, nullptr)),
      _trampoline_stubs_count(0),
      _stubs_start_offset(0) {}

  GrowableArray<AgnosticBarrierStubC2*>* stubs() {
    return _stubs;
  }

  bool needs_liveness_data(const MachNode* mach) const {
    // Don't need liveness data for nodes without barriers
    return mach->barrier_data() != ZBarrierElided;
  }

  bool needs_livein_data() const {
    return true;
  }

  void inc_trampoline_stubs_count() {
    assert(_trampoline_stubs_count != INT_MAX, "Overflow");
    ++_trampoline_stubs_count;
  }

  int trampoline_stubs_count() {
    return _trampoline_stubs_count;
  }

  void set_stubs_start_offset(int offset) {
    _stubs_start_offset = offset;
  }

  int stubs_start_offset() {
    return _stubs_start_offset;
  }
};

Node* AgnosticBarrierSetC2::store_at_resolved(C2Access& access, C2AccessValue& val) const {
  // return ModRefBarrierSetC2::store_at_resolved(access, val);
  // DecoratorSet decorators = access.decorators();
  
  // bool anonymous = (decorators & ON_UNKNOWN_OOP_REF) != 0;
  // bool in_heap = (decorators & IN_HEAP) != 0;
  // bool tightly_coupled_alloc = (decorators & C2_TIGHTLY_COUPLED_ALLOC) != 0;
  // bool need_store_barrier = !(tightly_coupled_alloc && use_ReduceInitialCardMarks()) && (in_heap || anonymous);
  // bool no_keepalive = (decorators & AS_NO_KEEPALIVE) != 0;
  // if (access.is_oop() && need_store_barrier) {
  //   access.set_barrier_data(G1C2BarrierPre | G1C2BarrierPost);
  // }
  // return BarrierSetC2::store_at_resolved(access, val);
  DecoratorSet decorators = access.decorators();

  const TypePtr* adr_type = access.addr().type();
  Node* adr = access.addr().node();

  bool is_array = (decorators & IS_ARRAY) != 0;
  bool anonymous = (decorators & ON_UNKNOWN_OOP_REF) != 0;
  bool in_heap = (decorators & IN_HEAP) != 0;
  bool use_precise = is_array || anonymous;
  bool tightly_coupled_alloc = (decorators & C2_TIGHTLY_COUPLED_ALLOC) != 0;

  if (!access.is_oop() || (!in_heap && !anonymous)) {
    return BarrierSetC2::store_at_resolved(access, val);
  }

  if (tightly_coupled_alloc) {
    access.set_barrier_data(AgnosticElided);
  } else {
    access.set_barrier_data(AgnosticBarrier);
  }

  return BarrierSetC2::store_at_resolved(access, val);
}

void AgnosticBarrierSetC2::late_barrier_analysis() const {
  compute_liveness_at_stubs();
}

void* AgnosticBarrierSetC2::create_barrier_state(Arena* comp_arena) const {
  return new (comp_arena) AgnosticBarrierSetC2State(comp_arena);
}

void AgnosticBarrierSetC2::eliminate_gc_barrier(PhaseMacroExpand* macro, Node* node) const {
  eliminate_gc_barrier_data(node);
}

void AgnosticBarrierSetC2::eliminate_gc_barrier_data(Node* node) const {
  if (node->is_LoadStore()) {
    LoadStoreNode* loadstore = node->as_LoadStore();
    loadstore->set_barrier_data(AgnosticElided);
  } else if (node->is_Mem()) {
    MemNode* mem = node->as_Mem();
    mem->set_barrier_data(AgnosticElided);
  }
}

static AgnosticBarrierSetC2State* barrier_set_state() {
  return reinterpret_cast<AgnosticBarrierSetC2State*>(Compile::current()->barrier_set_state());
}

void AgnosticBarrierSetC2::emit_stubs(CodeBuffer& cb) const {
  MacroAssembler masm(&cb);
  GrowableArray<AgnosticBarrierStubC2*>* const stubs = barrier_set_state()->stubs();
  barrier_set_state()->set_stubs_start_offset(masm.offset());

  for (int i = 0; i < stubs->length(); i++) {
    // Make sure there is enough space in the code buffer
    if (cb.insts()->maybe_expand_to_ensure_remaining(PhaseOutput::MAX_inst_size) && cb.blob() == nullptr) {
      ciEnv::current()->record_failure("CodeCache is full");
      return;
    }

    stubs->at(i)->emit_code(masm);
  }

  masm.flush();
}

AgnosticStoreBarrierStubC2* AgnosticStoreBarrierStubC2::create(const MachNode* node, Address ref_addr, Register prev_val, Register tmp, bool is_native, bool is_atomic, Register n) {
  AgnosticStoreBarrierStubC2* const stub = new (Compile::current()->comp_arena()) AgnosticStoreBarrierStubC2(node, ref_addr, prev_val, tmp, is_native, is_atomic, n);
  if (!Compile::current()->output()->in_scratch_emit_size()) {
    barrier_set_state()->stubs()->append(stub);
  }

  return stub;
}

AgnosticStoreBarrierStubC2::AgnosticStoreBarrierStubC2(const MachNode* node, Address ref_addr, Register prev_val, Register tmp,
                                                       bool is_native, bool is_atomic, Register n)
  : AgnosticBarrierStubC2(node),
    _ref_addr(ref_addr),
    _prev_val(prev_val),
    _tmp(tmp),
    _new(n),
    _is_native(is_native),
    _is_atomic(is_atomic) {}

void AgnosticStoreBarrierStubC2::emit_code(MacroAssembler& masm) {
  AgnosticBarrierSetAssembler* bs = static_cast<AgnosticBarrierSetAssembler*>(BarrierSet::barrier_set()->barrier_set_assembler());
  bs->generate_store_barrier_stub_c2(&masm, static_cast<AgnosticStoreBarrierStubC2*>(this));
}