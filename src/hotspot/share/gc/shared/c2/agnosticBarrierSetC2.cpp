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
#include "gc/shared/gc_globals.hpp"
#include "gc/z/c2/zBarrierSetC2.hpp"
#include "opto/output.hpp"
#include <cstdio>


void AgnosticBarrierSetC2Logic::write_barrier_data(C2Access& access) {
  DecoratorSet decorators = access.decorators();

  const TypePtr* adr_type = access.addr().type();
  Node* adr = access.addr().node();

  bool is_array = (decorators & IS_ARRAY) != 0;
  bool anonymous = (decorators & ON_UNKNOWN_OOP_REF) != 0;
  bool in_heap = (decorators & IN_HEAP) != 0;
  bool use_precise = is_array || anonymous;
  bool tightly_coupled_alloc = (decorators & C2_TIGHTLY_COUPLED_ALLOC) != 0;

  if (!access.is_oop() || (!in_heap && !anonymous)) {
    return;
  }
  
  if (tightly_coupled_alloc) {
    access.set_barrier_data(AgnosticElided);
  } else {
    access.set_barrier_data(AgnosticBarrier);
  }
}

void* AgnosticBarrierSetC2Logic::create_barrier_state(Arena* comp_arena) {
  return new (comp_arena) AgnosticBarrierSetC2State(comp_arena);
}

void AgnosticBarrierSetC2Logic::emit_stubs(CodeBuffer& cb) {
  MacroAssembler masm(&cb);
  GrowableArray<BarrierStubC2*>* const stubs = barrier_set_state()->stubs();

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

void AgnosticBarrierSetC2Logic::emit_zstubs(CodeBuffer& cb) {
  MacroAssembler masm(&cb);
  GrowableArray<ZBarrierStubC2*>* const zstubs = barrier_set_state()->zstubs();

  for (int i = 0; i < zstubs->length(); i++) {
    // Make sure there is enough space in the code buffer
    if (cb.insts()->maybe_expand_to_ensure_remaining(PhaseOutput::MAX_inst_size) && cb.blob() == nullptr) {
      ciEnv::current()->record_failure("CodeCache is full");
      return;
    }

    zstubs->at(i)->emit_code(masm);
  }

  masm.flush();
}

void AgnosticBarrierSetC2Logic::eliminate_gc_barrier_data(Node* node) {
  if (node->is_LoadStore()) {
    LoadStoreNode* loadstore = node->as_LoadStore();
    loadstore->set_barrier_data(AgnosticElided);
  } else if (node->is_Mem()) {
    MemNode* mem = node->as_Mem();
    mem->set_barrier_data(AgnosticElided);
  }
}

AgnosticBarrierSetC2State* AgnosticBarrierSetC2Logic::barrier_set_state() {
  return reinterpret_cast<AgnosticBarrierSetC2State*>(Compile::current()->barrier_set_state());
}

void* ZAgnosticBarrierSetC2::create_barrier_state(Arena* comp_arena) const {
  if (GCASB) {
    return AgnosticBarrierSetC2Logic::create_barrier_state(comp_arena);
  }
  return ZBarrierSetC2::create_barrier_state(comp_arena);
}

void ZAgnosticBarrierSetC2::emit_stubs(CodeBuffer& cb) const {
  if (GCASB) {
    // Emit ZGC stubs first to allow for trampolining calculation and emission
    AgnosticBarrierSetC2Logic::emit_zstubs(cb);
    AgnosticBarrierSetC2Logic::emit_stubs(cb);
    return;
  }
  ZBarrierSetC2::emit_stubs(cb);
}

void ZAgnosticBarrierSetC2::eliminate_gc_barrier(PhaseMacroExpand* macro, Node* node) const {
  if (GCASB) {
    AgnosticBarrierSetC2Logic::eliminate_gc_barrier_data(node);
    return;
  }
  ZBarrierSetC2::eliminate_gc_barrier(macro, node);
}

void ZAgnosticBarrierSetC2::eliminate_gc_barrier_data(Node* node) const {
  if (GCASB) {
    AgnosticBarrierSetC2Logic::eliminate_gc_barrier_data(node);
    return;
  }
  ZBarrierSetC2::eliminate_gc_barrier_data(node);
}

void ZAgnosticBarrierSetC2::late_barrier_analysis() const {
  compute_liveness_at_stubs();
}

void* G1AgnosticBarrierSetC2::create_barrier_state(Arena* comp_arena) const {
  if (GCASB) {
    return AgnosticBarrierSetC2Logic::create_barrier_state(comp_arena);
  }
  return G1BarrierSetC2::create_barrier_state(comp_arena);
}

void G1AgnosticBarrierSetC2::emit_stubs(CodeBuffer& cb) const {
  if (GCASB) {
    AgnosticBarrierSetC2Logic::emit_stubs(cb);
    return;
  }
  G1BarrierSetC2::emit_stubs(cb);
}

void G1AgnosticBarrierSetC2::eliminate_gc_barrier(PhaseMacroExpand* macro, Node* node) const {
  if (GCASB) {
    AgnosticBarrierSetC2Logic::eliminate_gc_barrier_data(node);
    return;
  }
  G1BarrierSetC2::eliminate_gc_barrier(macro, node);
}

void G1AgnosticBarrierSetC2::eliminate_gc_barrier_data(Node* node) const {
  if (GCASB) {
    AgnosticBarrierSetC2Logic::eliminate_gc_barrier_data(node);
    return;
  }
  G1BarrierSetC2::eliminate_gc_barrier_data(node);
}

void G1AgnosticBarrierSetC2::late_barrier_analysis() const {
  compute_liveness_at_stubs();
}

void* CardTableAgnosticBarrierSetC2::create_barrier_state(Arena* comp_arena) const {
  if (GCASB) {
    return AgnosticBarrierSetC2Logic::create_barrier_state(comp_arena);
  }
  return CardTableBarrierSetC2::create_barrier_state(comp_arena);
}

void CardTableAgnosticBarrierSetC2::emit_stubs(CodeBuffer& cb) const {
  if (GCASB) {
    AgnosticBarrierSetC2Logic::emit_stubs(cb);
    return;
  }
  CardTableBarrierSetC2::emit_stubs(cb);
}

void CardTableAgnosticBarrierSetC2::eliminate_gc_barrier(PhaseMacroExpand* macro, Node* node) const {
  if (GCASB) {
    AgnosticBarrierSetC2Logic::eliminate_gc_barrier_data(node);
    return;
  }
  CardTableBarrierSetC2::eliminate_gc_barrier(macro, node);
}

void CardTableAgnosticBarrierSetC2::eliminate_gc_barrier_data(Node* node) const {
  if (GCASB) {
    AgnosticBarrierSetC2Logic::eliminate_gc_barrier_data(node);
    return;
  }
  CardTableBarrierSetC2::eliminate_gc_barrier_data(node);
}

void CardTableAgnosticBarrierSetC2::late_barrier_analysis() const {
  if (GCASB) {
    compute_liveness_at_stubs();
  }
}

AgnosticStoreBarrierStubC2* AgnosticStoreBarrierStubC2::create(const MachNode* node, Address ref_addr) {
  AgnosticStoreBarrierStubC2* const stub = new (Compile::current()->comp_arena()) AgnosticStoreBarrierStubC2(node, ref_addr);
  if (!Compile::current()->output()->in_scratch_emit_size()) {
    AgnosticBarrierSetC2Logic::barrier_set_state()->stubs()->append(stub);
  }

  return stub;
}

AgnosticStoreBarrierStubC2::AgnosticStoreBarrierStubC2(const MachNode* node, Address ref_addr)
  : AgnosticBarrierStubC2(node),
    _ref_addr(ref_addr) {}

void AgnosticStoreBarrierStubC2::emit_code(MacroAssembler& masm) {
  AgnosticBarrierSetAssembler* bs = static_cast<AgnosticBarrierSetAssembler*>(BarrierSet::barrier_set()->barrier_set_assembler());
  bs->generate_store_barrier_stub_c2(&masm, static_cast<AgnosticStoreBarrierStubC2*>(this));
}