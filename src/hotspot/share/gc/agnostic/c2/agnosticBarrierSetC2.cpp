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

#include "asm/macroAssembler.hpp"
#include "gc/g1/g1ThreadLocalData.hpp"
#include "gc/g1/g1BarrierSetRuntime.hpp"
#include "gc/shared/barrierSetAssembler_aarch64.hpp"
#include "gc/z/zBarrierSetRuntime.hpp"
#include "gc/z/zBarrierSetAssembler.hpp"
#include "gc/z/zThreadLocalData.hpp"
#include "gc/shared/c2/barrierSetC2.hpp"
#include "oops/accessDecorators.hpp"
#include "gc/shared/barrierSet.hpp"
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
#include "runtime/jfieldIDWorkaround.hpp"
#include "utilities/debug.hpp"
#include "utilities/growableArray.hpp"

void AgnosticBarrierSetC2Logic::determine_barrier_data(C2Access& access) {
  DecoratorSet decorators = access.decorators();
  bool anonymous = (decorators & ON_UNKNOWN_OOP_REF) != 0;
  bool in_heap = (decorators & IN_HEAP) != 0;
  bool in_native = (decorators & IN_NATIVE) != 0;
  bool tightly_coupled_alloc = (decorators & C2_TIGHTLY_COUPLED_ALLOC) != 0;
  bool no_keepalive = (decorators & AS_NO_KEEPALIVE) != 0;

  if (access.is_oop() && (in_heap || anonymous)) {
    uint8_t barrier_data = AgnosticBarrierSATB | AgnosticBarrierCardMark;
    if (tightly_coupled_alloc) {
      access.set_barrier_data(AgnosticBarrierElided);
      return;
    }

    if (in_native) {
      barrier_data |= AgnosticBarrierNative;
    }

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

void AgnosticStoreBarrierStubC2::register_stub(AgnosticStoreBarrierStubC2* stub) {
  if (!Compile::current()->output()->in_scratch_emit_size()) {
    barrier_set_state()->stubs()->append(stub);
  }
}

AgnosticStoreBarrierStubC2::AgnosticStoreBarrierStubC2(const MachNode* node, bool is_atomic, bool is_native, bool is_nokeepalive)
  : BarrierStubC2(node), 
  _is_atomic(is_atomic),
  _is_native(is_native),
  _is_nokeepalive(is_nokeepalive) {}

  AgnosticStoreBarrierStubC2* AgnosticStoreBarrierStubC2::create(const MachNode* node, bool is_atomic, bool is_native, bool is_nokeepalive) {
    AgnosticStoreBarrierStubC2* const stub = new (Compile::current()->comp_arena()) AgnosticStoreBarrierStubC2(node, is_atomic, is_native, is_nokeepalive);
    register_stub(stub);
    return stub;
  }

void AgnosticStoreBarrierStubC2::initialize_registers(Register src,
    Register dst,
    Register tmp1,
    Register tmp2,
    Register tmp3) {
  _src = src;
  _dst = dst;
  _tmp1 = tmp1;
  _tmp2 = tmp2;
  _tmp3 = tmp3;
}

Register AgnosticStoreBarrierStubC2::src() const { return _src; }
Register AgnosticStoreBarrierStubC2::dst() const { return _dst; }
Register AgnosticStoreBarrierStubC2::tmp1() const { return _tmp1; }
Register AgnosticStoreBarrierStubC2::tmp2() const { return _tmp2; }
Register AgnosticStoreBarrierStubC2::tmp3() const { return _tmp3; }

bool AgnosticStoreBarrierStubC2::is_atomic() const { return _is_atomic; }
bool AgnosticStoreBarrierStubC2::is_native() const { return _is_native; }
bool AgnosticStoreBarrierStubC2::is_nokeepalive() const { return _is_nokeepalive; }

struct SATB {
  static ByteSize index_offset() {
    assert(SATBMarkQueue::byte_offset_of_index() == ZStoreBarrierBuffer::current_offset(), "must be for agnostic store barriers");
    return SATBMarkQueue::byte_offset_of_index();
  }

  static ByteSize buffer_offset() {
    assert(SATBMarkQueue::byte_offset_of_buf() == ZStoreBarrierBuffer::buffer_offset(), "must be for agnostic store barriers");
    return SATBMarkQueue::byte_offset_of_buf();
  }
};

#define __ masm.
/*
   static void generate_satb_enqueue(MacroAssembler& masm,
   Address ref_addr,
   Register pre_val,
   Register tmp1,
   Register tmp2,
   Label& runtime,
   Label& continuation) {
   assert_different_registers(ref_addr.base(), ref_addr.index(), tmp1, tmp2);

   __ ldr(tmp1, Address(rthread, Thread::satb_base_address_offset()));

   __ ldr(tmp2, Address(tmp1, SATB::index_offset()));
   __ cbz(tmp2, runtime);

// Bump the pointer
__ relocate(barrier_Relocation::spec(), AgnosticBarrierRelocationFormatPointerBumpBeforeSub);
__ sub(tmp2, tmp2, wordSize); // ZGC: patch to sizeof(ZStoreBarrierEntry)
__ str(tmp2, Address(tmp1, SATB::index_offset()));

// Compute the buffer entry address
__ lea(tmp2, Address(tmp2, SATB::buffer_offset()));
__ add(tmp2, tmp2, tmp1);

Label skip;
__ br(Assembler::EQ, skip);

// Compute and log the store address
__ lea(tmp1, ref_addr);
__ str(tmp1, Address(tmp2, in_bytes(ZStoreBarrierEntry::p_offset())));

// Load and log the prev value
__ ldr(tmp1, tmp1);
__ str(tmp1, Address(tmp2, in_bytes(ZStoreBarrierEntry::prev_offset())));
__ b(continuation);

__ bind(skip);
__ str(pre_val, Address(tmp2, 0));
}
*/

static void generate_g1_slow_path(MacroAssembler& masm, AgnosticStoreBarrierStubC2* stub) {
  Register dst = stub->dst();
  Register tmp1 = stub->tmp1();
  Register tmp2 = stub->tmp2();
  Register tmp3 = stub->tmp3();

  if (dst != noreg) {
    __ load_heap_oop(tmp1, Address(dst, 0), noreg, noreg, AS_RAW);
  }
  __ cbz(tmp1, *stub->continuation());

  const ByteSize buffer_offset = G1ThreadLocalData::satb_mark_queue_buffer_offset();
  const ByteSize index_offset = G1ThreadLocalData::satb_mark_queue_index_offset();

  Label runtime;
  // Can we store a value in the given thread's buffer?
  // (The index field is typed as size_t.)
  __ ldr(tmp2, Address(rthread, in_bytes(index_offset)));  // tmp2 := *(index address)
  __ cbz(tmp2, runtime);                                   // jump to runtime if index == 0 (full buffer)

  // The buffer is not full, store value into it.
  __ sub(tmp2, tmp2, wordSize);                            // tmp2 := next index
  __ str(tmp2, Address(rthread, in_bytes(index_offset)));  // *(index address) := next index
  __ ldr(tmp3, Address(rthread, in_bytes(buffer_offset))); // tmp3 := buffer address
  __ str(tmp1, Address(tmp3, tmp2));                       // *(buffer address + next index) := value

  __ b(*stub->continuation());

  __ bind(runtime);
  {
    SaveLiveRegisters save_live_registers(&masm, stub);
    __ block_comment("G1 Runtime Call");
    __ mov(c_rarg0, stub->tmp1());
    __ mov(c_rarg1, rthread);
    __ mov(rscratch1, CAST_FROM_FN_PTR(address, G1BarrierSetRuntime::write_ref_field_pre_entry));
    __ blr(rscratch1);
  }

  __ b(*stub->continuation());
}

static void generate_z_slow_path(MacroAssembler& masm, AgnosticStoreBarrierStubC2* stub) {
  Address ref_addr = Address(stub->dst());
  Register tmp1 = stub->tmp1();
  Register tmp2 = stub->tmp2();
  Label runtime;

  assert_different_registers(ref_addr.base(), ref_addr.index(), tmp1, tmp2);

  __ ldr(tmp1, Address(rthread, ZThreadLocalData::store_barrier_buffer_offset()));

  // Combined pointer bump and check if the buffer is disabled or full
  __ ldr(tmp2, Address(tmp1, ZStoreBarrierBuffer::current_offset()));
  __ cbz(tmp2, runtime);

  // Bump the pointer
  __ sub(tmp2, tmp2, sizeof(ZStoreBarrierEntry));
  __ str(tmp2, Address(tmp1, ZStoreBarrierBuffer::current_offset()));

  // Compute the buffer entry address
  __ lea(tmp2, Address(tmp2, ZStoreBarrierBuffer::buffer_offset()));
  __ add(tmp2, tmp2, tmp1);

  // Compute and log the store address
  __ lea(tmp1, ref_addr);
  __ str(tmp1, Address(tmp2, in_bytes(ZStoreBarrierEntry::p_offset())));

  // Load and log the prev value
  __ ldr(tmp1, tmp1);
  __ str(tmp1, Address(tmp2, in_bytes(ZStoreBarrierEntry::prev_offset())));

  __ b(*stub->continuation());

  __ bind(runtime);
  {
    SaveLiveRegisters save_live_registers(&masm, stub);
    __ block_comment("Z Runtime Call");
    Address ref_addr = Address(stub->dst());
    __ lea(c_rarg0, ref_addr);
    if (stub->is_native()) {
      __ lea(rscratch1, RuntimeAddress(ZBarrierSetRuntime::store_barrier_on_native_oop_field_without_healing_addr()));
    } else if (stub->is_atomic()) {
      __ lea(rscratch1, RuntimeAddress(ZBarrierSetRuntime::store_barrier_on_oop_field_with_healing_addr()));
    } else if (stub->is_nokeepalive()) {
      __ lea(rscratch1, RuntimeAddress(ZBarrierSetRuntime::no_keepalive_store_barrier_on_oop_field_without_healing_addr()));
    } else {
      __ lea(rscratch1, RuntimeAddress(ZBarrierSetRuntime::store_barrier_on_oop_field_without_healing_addr()));
    }
    __ blr(rscratch1);
  }

  __ b(*stub->continuation());
}

static void generate_c2_store_barrier_stub(MacroAssembler& masm, AgnosticStoreBarrierStubC2* stub) {
  Register src = stub->src();
  Register dst = stub->dst();
  Register tmp1 = stub->tmp1();
  Register tmp2 = stub->tmp2();
  Register tmp3 = stub->tmp3();

  __ block_comment("AgnosticStoreBarrierStubC2");
  __ bind(*stub->entry());

  Label g1_slow_path;
  __ cmpw(tmp2, (uint32_t)G1ConcurrentMarkMask);
  __ br(Assembler::EQ, g1_slow_path);

  generate_z_slow_path(masm, stub);

  __ bind(g1_slow_path);
  generate_g1_slow_path(masm, stub);
}
#undef __

void AgnosticStoreBarrierStubC2::emit_code(MacroAssembler& masm) {
  if (_deferred_emit) {
    generate_c2_store_barrier_stub(masm, this);
    return;
  }
  // Defer emission of store barriers so that trampolines are emitted first
  _deferred_emit = true;
  register_stub(this);
}
