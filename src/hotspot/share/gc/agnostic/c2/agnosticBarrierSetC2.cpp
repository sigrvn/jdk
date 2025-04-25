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

void AgnosticBarrierSetC2Logic::determine_barrier_data(C2Access& access) {
  DecoratorSet decorators = access.decorators();
  bool anonymous = (decorators & ON_UNKNOWN_OOP_REF) != 0;
  bool is_dest_uninitialized = (decorators & IS_DEST_UNINITIALIZED) != 0;
  bool in_heap = (decorators & IN_HEAP) != 0;
  bool in_native = (decorators & IN_NATIVE) != 0;
  bool tightly_coupled_alloc = (decorators & C2_TIGHTLY_COUPLED_ALLOC) != 0;
  bool no_keepalive = (decorators & AS_NO_KEEPALIVE) != 0;

  if (access.is_oop() && (in_heap || anonymous)) {
    uint8_t barrier_data = tightly_coupled_alloc ? AgnosticBarrierElided : AgnosticBarrierRequired;
    if (no_keepalive) {
      barrier_data |= AgnosticBarrierNokeepalive;
    }
    access.set_barrier_data(barrier_data);
  }
}

void AgnosticBarrierSetC2Logic::eliminate_barrier_data(Node* node) {
  if (node->is_LoadStore()) {
    LoadStoreNode* loadstore = node->as_LoadStore();
    loadstore->set_barrier_data(AgnosticBarrierElided);
  } else if (node->is_Mem()) {
    MemNode* mem = node->as_Mem();
    mem->set_barrier_data(AgnosticBarrierElided);
  }
}

static AgnosticBarrierSetC2State* barrier_set_state() {
  return reinterpret_cast<AgnosticBarrierSetC2State*>(Compile::current()->barrier_set_state());
}

void* AgnosticCardTableBarrierSetC2::create_barrier_state(Arena* comp_arena) const {
  return new (comp_arena) AgnosticBarrierSetC2State(comp_arena);
}

// Emit stubs for Serial & Parallel collectors
void AgnosticCardTableBarrierSetC2::emit_stubs(CodeBuffer& cb) const {
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

void AgnosticStoreBarrierStubC2::emit_code(MacroAssembler& masm) {
  AgnosticBarrierSetAssembler* bsa = static_cast<AgnosticBarrierSetAssembler*>(BarrierSet::barrier_set()->barrier_set_assembler());
  bsa->generate_store_barrier_stub_c2(&masm, this);
}

AgnosticStoreBarrierStubC2::AgnosticStoreBarrierStubC2(const MachNode* node, bool is_atomic, bool is_native, bool is_nokeepalive)
  : BarrierStubC2(node),
  _is_atomic(is_atomic),
  _is_native(is_native),
  _is_nokeepalive(is_nokeepalive) {}

  AgnosticStoreBarrierStubC2* AgnosticStoreBarrierStubC2::create(const MachNode* node, bool is_atomic, bool is_native, bool is_nokeepalive) {
    AgnosticStoreBarrierStubC2* const stub = new (Compile::current()->comp_arena()) AgnosticStoreBarrierStubC2(node, is_atomic, is_native, is_nokeepalive);
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
Register AgnosticStoreBarrierStubC2::aux() const { return _aux; }
Register AgnosticStoreBarrierStubC2::tmp1() const { return _tmp1; }
Register AgnosticStoreBarrierStubC2::tmp2() const { return _tmp2; }

bool AgnosticStoreBarrierStubC2::is_atomic() const { return _is_atomic; }
bool AgnosticStoreBarrierStubC2::is_native() const { return _is_native; }
bool AgnosticStoreBarrierStubC2::is_nokeepalive() const { return _is_nokeepalive; }
