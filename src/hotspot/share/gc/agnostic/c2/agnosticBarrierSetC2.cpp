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

#include "gc/shared/c2/barrierSetC2.hpp"
#include "oops/accessDecorators.hpp"
#include "gc/shared/barrierSet.hpp"
#include "gc/agnostic/agnosticBarrierSetAssembler.hpp"
#include "gc/agnostic/c2/agnosticBarrierSetC2.hpp"
#include "opto/c2_globals.hpp"
#include "opto/compile.hpp"
#include "opto/escape.hpp"
#include "opto/graphKit.hpp"
#include "opto/machnode.hpp"
#include "opto/macro.hpp"
#include "opto/node.hpp"
#include "opto/output.hpp"
#include "opto/regalloc.hpp"
#include "utilities/growableArray.hpp"

// Important properties to consider for stores:
// 1. Uninitialized oop or initialized oop (if initialized we should use SATB, if not we don’t)
// 2. In heap or not in heap (if in the heap we need to do remset maintenance, otherwise we don’t)
Node* AgnosticBarrierSetC2::store_at_resolved(C2Access& access, C2AccessValue& val) const {
  DecoratorSet decorators = access.decorators();
  bool is_dest_uninitialized = (decorators & IS_DEST_UNINITIALIZED) != 0;
  bool in_heap = (decorators & IN_HEAP) != 0;
  bool tightly_coupled_alloc = (decorators & C2_TIGHTLY_COUPLED_ALLOC) != 0;
  bool as_no_keepalive = (decorators & AS_NO_KEEPALIVE) != 0;

  uint8_t barrier_data = 0;
  if (!is_dest_uninitialized) {
    barrier_data |= AgnosticBarrierIsInitialized;
  }

  if (in_heap) {
    barrier_data |= AgnosticBarrierInHeap;
  }

  if (tightly_coupled_alloc) {
    barrier_data |= ZBarrierElided;
  }

  if (as_no_keepalive) {
    barrier_data |= ZBarrierNoKeepalive;
  }

  access.set_barrier_data(barrier_data);
  return BarrierSetC2::store_at_resolved(access, val);
}

class AgnosticBarrierSetC2State : public BarrierSetC2State {
  private:
    GrowableArray<AgnosticBarrierStubC2*>* _stubs;

  public:
    AgnosticBarrierSetC2State(Arena* arena)
      : BarrierSetC2State(arena),
      _stubs(new (arena) GrowableArray<AgnosticBarrierStubC2*>(arena, 8,  0, nullptr)) {}

    GrowableArray<AgnosticBarrierStubC2*>* stubs() {
      return _stubs;
    }

    bool needs_liveness_data(const MachNode* mach) const {
      return true;
    }

    bool needs_livein_data() const {
      // TODO: ZGC needs live-in data but G1 does not
      return true;
    }
};

static AgnosticBarrierSetC2State* barrier_set_state() {
  return reinterpret_cast<AgnosticBarrierSetC2State*>(Compile::current()->barrier_set_state());
}

void* AgnosticBarrierSetC2::create_barrier_state(Arena* comp_arena) const {
  return new (comp_arena) AgnosticBarrierSetC2State(comp_arena);
}

void AgnosticBarrierSetC2::emit_stubs(CodeBuffer& cb) const {
  MacroAssembler masm(&cb);
  GrowableArray<AgnosticBarrierStubC2*>* const stubs = barrier_set_state()->stubs();
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

void AgnosticStoreBarrierStubC2::emit_code(MacroAssembler& masm) {
  AgnosticBarrierSetAssembler* bs_asm = static_cast<AgnosticBarrierSetAssembler*>(BarrierSet::barrier_set()->barrier_set_assembler());
  bs_asm->generate_store_barrier_stub(&masm, this);
}

AgnosticBarrierStubC2::AgnosticBarrierStubC2(const MachNode* node) : BarrierStubC2(node) {}

AgnosticStoreBarrierStubC2::AgnosticStoreBarrierStubC2(const MachNode* node, bool is_initialized, bool in_heap, bool is_atomic, bool is_nokeepalive)
  : AgnosticBarrierStubC2(node),
  _is_initialized(is_initialized),
  _in_heap(in_heap),
  _is_atomic(is_atomic),
  _is_nokeepalive(is_nokeepalive) {}

AgnosticStoreBarrierStubC2* AgnosticStoreBarrierStubC2::create(const MachNode* node, bool is_initialized, bool in_heap, bool is_atomic, bool is_nokeepalive) {
  AgnosticStoreBarrierStubC2* const stub = new (Compile::current()->comp_arena()) AgnosticStoreBarrierStubC2(node, is_initialized, in_heap, is_atomic, is_nokeepalive);
  if (!Compile::current()->output()->in_scratch_emit_size()) {
    barrier_set_state()->stubs()->append(stub);
  }
  return stub;
}

void AgnosticStoreBarrierStubC2::initialize_registers(Register src,
                                                      Register dst,
                                                      Register aux,
                                                      Register tmp1,
                                                      Register tmp2) {
  _src = src;
  _dst = dst;
  _aux = aux;
  _tmp1 = tmp1;
  _tmp2 = tmp2;
}

Register AgnosticStoreBarrierStubC2::src() const { return _src; }
Register AgnosticStoreBarrierStubC2::dst() const { return _dst; }
Register AgnosticStoreBarrierStubC2::pre_val() const { return _aux; }
Register AgnosticStoreBarrierStubC2::new_zaddress() const { return _aux; }
Register AgnosticStoreBarrierStubC2::tmp1() const { return _tmp1; }
Register AgnosticStoreBarrierStubC2::tmp2() const { return _tmp2; }

bool AgnosticStoreBarrierStubC2::is_initialized() const { return _is_initialized; }
bool AgnosticStoreBarrierStubC2::in_heap() const { return _in_heap; }
bool AgnosticStoreBarrierStubC2::is_atomic() const { return _is_atomic; }
bool AgnosticStoreBarrierStubC2::is_nokeepalive() const { return _is_nokeepalive; }
