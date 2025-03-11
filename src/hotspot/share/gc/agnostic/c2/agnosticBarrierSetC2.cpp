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

#include "oops/accessDecorators.hpp"
#include "gc/agnostic/agnosticBarrierSetAssembler.hpp"
#include "gc/agnostic/c2/agnosticBarrierSetC2.hpp"
#include "gc/z/zBarrierSet.hpp"
#include "opto/compile.hpp"
#include "opto/escape.hpp"
#include "opto/graphKit.hpp"
#include "opto/machnode.hpp"
#include "opto/macro.hpp"
#include "opto/node.hpp"
#include "opto/output.hpp"
#include "opto/regalloc.hpp"
#include "utilities/growableArray.hpp"

static void z_set_barrier_data(C2Access& access) {
  if (!ZBarrierSet::barrier_needed(access.decorators(), access.type())) {
    return;
  }

  if (access.decorators() & C2_TIGHTLY_COUPLED_ALLOC) {
    access.set_barrier_data(ZBarrierElided);
    return;
  }

  BarrierData barrier_data = 0;
  if (access.decorators() & ON_PHANTOM_OOP_REF) {
    barrier_data |= ZBarrierPhantom;
  } else if (access.decorators() & ON_WEAK_OOP_REF) {
    barrier_data |= ZBarrierWeak;
  } else {
    barrier_data |= ZBarrierStrong;
  }

  if (access.decorators() & IN_NATIVE) {
    barrier_data |= ZBarrierNative;
  }

  if (access.decorators() & AS_NO_KEEPALIVE) {
    barrier_data |= ZBarrierNoKeepalive;
  }

  access.set_barrier_data(barrier_data);
}

Node* AgnosticBarrierSetC2::store_at_resolved(C2Access& access, C2AccessValue& val) const {
  z_set_barrier_data(access);
  BarrierData barrier_data = access.barrier_data();
  DecoratorSet decorators = access.decorators();
  bool is_dest_uninitialized = (decorators & IS_DEST_UNINITIALIZED) != 0;
  bool anonymous = (decorators & ON_UNKNOWN_OOP_REF) != 0;
  bool in_heap = (decorators & IN_HEAP) != 0;
  bool tightly_coupled_alloc = (decorators & C2_TIGHTLY_COUPLED_ALLOC) != 0;
  bool need_store_barrier = !(tightly_coupled_alloc && use_ReduceInitialCardMarks()) && (in_heap || anonymous);
  bool no_keepalive = (decorators & AS_NO_KEEPALIVE) != 0;

  if (access.is_oop() && need_store_barrier) {
    barrier_data |= get_store_barrier(access);
    if (tightly_coupled_alloc) {
      assert(!use_ReduceInitialCardMarks(),
             "post-barriers are only needed for tightly-coupled initialization stores when ReduceInitialCardMarks is disabled");
      // Pre-barriers are unnecessary for tightly-coupled initialization stores.
      barrier_data &= ~G1C2BarrierPre;
    }
  }
  if (no_keepalive) {
    // No keep-alive means no need for the pre-barrier.
    barrier_data &= ~G1C2BarrierPre;
  }

  access.set_barrier_data(barrier_data);
  return BarrierSetC2::store_at_resolved(access, val);
}

class AgnosticBarrierSetC2State : public BarrierSetC2State {
private:
  GrowableArray<BarrierStubC2*>* _stubs;
  int _trampoline_stubs_count;
  int _stubs_start_offset;

public:
  AgnosticBarrierSetC2State(Arena* arena)
    : BarrierSetC2State(arena),
      _stubs(new (arena) GrowableArray<BarrierStubC2*>(arena, 8,  0, nullptr)),
      _trampoline_stubs_count(0),
      _stubs_start_offset(0) {}

  GrowableArray<BarrierStubC2*>* stubs() {
    return _stubs;
  }

  bool needs_liveness_data(const MachNode* mach) const {
    return G1BarrierStubC2::needs_pre_barrier(mach) ||
            G1BarrierStubC2::needs_post_barrier(mach) ||
            mach->barrier_data() != ZBarrierElided;
  }

  bool needs_livein_data() const {
    // TODO: ZGC needs live-in data but G1 does not
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

static AgnosticBarrierSetC2State* barrier_set_state() {
  return reinterpret_cast<AgnosticBarrierSetC2State*>(Compile::current()->barrier_set_state());
}

void* AgnosticBarrierSetC2::create_barrier_state(Arena* comp_arena) const {
  return new (comp_arena) AgnosticBarrierSetC2State(comp_arena);
}

void AgnosticBarrierSetC2::late_barrier_analysis() const {
  compute_liveness_at_stubs();
}

void AgnosticBarrierSetC2::emit_stubs(CodeBuffer& cb) const {
  MacroAssembler masm(&cb);
  GrowableArray<BarrierStubC2*>* const stubs = barrier_set_state()->stubs();
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
