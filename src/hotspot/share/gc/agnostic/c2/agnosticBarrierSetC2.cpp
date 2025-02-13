/*
 * Copyright (c) 2024 Oracle and/or its affiliates. All rights reserved.
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
#include "classfile/javaClasses.hpp"
#include "code/vmreg.inline.hpp"
#include "gc/agnostic/c2/agnosticBarrierSetC2.hpp"
#include "gc/g1/g1BarrierSet.hpp"
#include "gc/g1/g1BarrierSetAssembler.hpp"
#include "gc/g1/g1BarrierSetRuntime.hpp"
#include "gc/g1/g1CardTable.hpp"
#include "gc/g1/g1ThreadLocalData.hpp"
#include "gc/g1/g1HeapRegion.hpp"
#include "gc/z/zBarrierSet.hpp"
#include "opto/arraycopynode.hpp"
#include "opto/block.hpp"
#include "opto/compile.hpp"
#include "opto/escape.hpp"
#include "opto/graphKit.hpp"
#include "opto/idealKit.hpp"
#include "opto/machnode.hpp"
#include "opto/macro.hpp"
#include "opto/memnode.hpp"
#include "opto/node.hpp"
#include "opto/output.hpp"
#include "opto/regalloc.hpp"
#include "opto/rootnode.hpp"
#include "opto/runtime.hpp"
#include "opto/type.hpp"
#include "utilities/growableArray.hpp"
#include "utilities/macros.hpp"

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

bool AgnosticBarrierSetC2::use_ReduceInitialCardMarks() const {
  return ReduceInitialCardMarks;
}

bool AgnosticBarrierSetC2::g1_can_remove_pre_barrier(GraphKit* kit,
                                               PhaseValues* phase,
                                               Node* adr,
                                               BasicType bt,
                                               uint adr_idx) const {
  intptr_t offset = 0;
  Node* base = AddPNode::Ideal_base_and_offset(adr, phase, offset);
  AllocateNode* alloc = AllocateNode::Ideal_allocation(base);

  if (offset == Type::OffsetBot) {
    return false; // Cannot unalias unless there are precise offsets.
  }
  if (alloc == nullptr) {
    return false; // No allocation found.
  }

  intptr_t size_in_bytes = type2aelembytes(bt);
  Node* mem = kit->memory(adr_idx); // Start searching here.

  for (int cnt = 0; cnt < 50; cnt++) {
    if (mem->is_Store()) {
      Node* st_adr = mem->in(MemNode::Address);
      intptr_t st_offset = 0;
      Node* st_base = AddPNode::Ideal_base_and_offset(st_adr, phase, st_offset);

      if (st_base == nullptr) {
        break; // Inscrutable pointer.
      }
      if (st_base == base && st_offset == offset) {
        // We have found a store with same base and offset as ours.
        break;
      }
      if (st_offset != offset && st_offset != Type::OffsetBot) {
        const int MAX_STORE = BytesPerLong;
        if (st_offset >= offset + size_in_bytes ||
            st_offset <= offset - MAX_STORE ||
            st_offset <= offset - mem->as_Store()->memory_size()) {
          // Success:  The offsets are provably independent.
          // (You may ask, why not just test st_offset != offset and be done?
          // The answer is that stores of different sizes can co-exist
          // in the same sequence of RawMem effects.  We sometimes initialize
          // a whole 'tile' of array elements with a single jint or jlong.)
          mem = mem->in(MemNode::Memory);
          continue; // Advance through independent store memory.
        }
      }
      if (st_base != base
          && MemNode::detect_ptr_independence(base, alloc, st_base,
                                              AllocateNode::Ideal_allocation(st_base),
                                              phase)) {
        // Success: the bases are provably independent.
        mem = mem->in(MemNode::Memory);
        continue; // Advance through independent store memory.
      }
    } else if (mem->is_Proj() && mem->in(0)->is_Initialize()) {
      InitializeNode* st_init = mem->in(0)->as_Initialize();
      AllocateNode* st_alloc = st_init->allocation();

      // Make sure that we are looking at the same allocation site.
      // The alloc variable is guaranteed to not be null here from earlier check.
      if (alloc == st_alloc) {
        // Check that the initialization is storing null so that no previous store
        // has been moved up and directly write a reference.
        Node* captured_store = st_init->find_captured_store(offset,
                                                            type2aelembytes(T_OBJECT),
                                                            phase);
        if (captured_store == nullptr || captured_store == st_init->zero_memory()) {
          return true;
        }
      }
    }
    // Unless there is an explicit 'continue', we must bail out here,
    // because 'mem' is an inscrutable memory state (e.g., a call).
    break;
  }
  return false;
}

bool AgnosticBarrierSetC2::g1_can_remove_post_barrier(GraphKit* kit,
                                                PhaseValues* phase, Node* store_ctrl,
                                                Node* adr) const {
  intptr_t      offset = 0;
  Node*         base   = AddPNode::Ideal_base_and_offset(adr, phase, offset);
  AllocateNode* alloc  = AllocateNode::Ideal_allocation(base);

  if (offset == Type::OffsetBot) {
    return false; // Cannot unalias unless there are precise offsets.
  }
  if (alloc == nullptr) {
    return false; // No allocation found.
  }

  Node* mem = store_ctrl;   // Start search from Store node.
  if (mem->is_Proj() && mem->in(0)->is_Initialize()) {
    InitializeNode* st_init = mem->in(0)->as_Initialize();
    AllocateNode*  st_alloc = st_init->allocation();
    // Make sure we are looking at the same allocation
    if (alloc == st_alloc) {
      return true;
    }
  }

  return false;
}

Node* AgnosticBarrierSetC2::load_at_resolved(C2Access& access, const Type* val_type) const {
  // G1
  DecoratorSet decorators = access.decorators();
  bool on_weak = (decorators & ON_WEAK_OOP_REF) != 0;
  bool on_phantom = (decorators & ON_PHANTOM_OOP_REF) != 0;
  bool no_keepalive = (decorators & AS_NO_KEEPALIVE) != 0;
  // If we are reading the value of the referent field of a Reference object, we
  // need to record the referent in an SATB log buffer using the pre-barrier
  // mechanism. Also we need to add a memory barrier to prevent commoning reads
  // from this field across safepoints, since GC can change its value.
  bool need_read_barrier = ((on_weak || on_phantom) && !no_keepalive);
  if (access.is_oop() && need_read_barrier) {
    access.set_barrier_data(G1C2BarrierPre);
  }

  z_set_barrier_data(access);
  return BarrierSetC2::load_at_resolved(access, val_type);
}

Node* AgnosticBarrierSetC2::atomic_cmpxchg_val_at_resolved(C2AtomicParseAccess& access, Node* expected_val,
                                                     Node* new_val, const Type* value_type) const {
  z_set_barrier_data(access);

  GraphKit* kit = access.kit();
  if (!access.is_oop()) {
    return BarrierSetC2::atomic_cmpxchg_val_at_resolved(access, expected_val, new_val, value_type);
  }
  access.set_barrier_data(G1C2BarrierPre | G1C2BarrierPost);
  return BarrierSetC2::atomic_cmpxchg_val_at_resolved(access, expected_val, new_val, value_type);
}

Node* AgnosticBarrierSetC2::atomic_cmpxchg_bool_at_resolved(C2AtomicParseAccess& access, Node* expected_val,
                                                      Node* new_val, const Type* value_type) const {
  z_set_barrier_data(access);

  GraphKit* kit = access.kit();
  if (!access.is_oop()) {
    return BarrierSetC2::atomic_cmpxchg_bool_at_resolved(access, expected_val, new_val, value_type);
  }
  access.set_barrier_data(G1C2BarrierPre | G1C2BarrierPost);
  return BarrierSetC2::atomic_cmpxchg_bool_at_resolved(access, expected_val, new_val, value_type);
}

Node* AgnosticBarrierSetC2::atomic_xchg_at_resolved(C2AtomicParseAccess& access, Node* new_val, const Type* value_type) const {
  z_set_barrier_data(access);

  GraphKit* kit = access.kit();
  if (!access.is_oop()) {
    return BarrierSetC2::atomic_xchg_at_resolved(access, new_val, value_type);
  }
  access.set_barrier_data(G1C2BarrierPre | G1C2BarrierPost);
  return BarrierSetC2::atomic_xchg_at_resolved(access, new_val, value_type);
}

// == Dominating barrier elision ==

static bool block_has_safepoint(const Block* block, uint from, uint to) {
  for (uint i = from; i < to; i++) {
    if (block->get_node(i)->is_MachSafePoint()) {
      // Safepoint found
      return true;
    }
  }

  // Safepoint not found
  return false;
}

static bool block_has_safepoint(const Block* block) {
  return block_has_safepoint(block, 0, block->number_of_nodes());
}

static uint block_index(const Block* block, const Node* node) {
  for (uint j = 0; j < block->number_of_nodes(); ++j) {
    if (block->get_node(j) == node) {
      return j;
    }
  }
  ShouldNotReachHere();
  return 0;
}

// Look through various node aliases
static const Node* look_through_node(const Node* node) {
  while (node != nullptr) {
    const Node* new_node = node;
    if (node->is_Mach()) {
      const MachNode* const node_mach = node->as_Mach();
      if (node_mach->ideal_Opcode() == Op_CheckCastPP) {
        new_node = node->in(1);
      }
      if (node_mach->is_SpillCopy()) {
        new_node = node->in(1);
      }
    }
    if (new_node == node || new_node == nullptr) {
      break;
    } else {
      node = new_node;
    }
  }

  return node;
}

// Whether the given offset is undefined.
static bool is_undefined(intptr_t offset) {
  return offset == Type::OffsetTop;
}

// Whether the given offset is unknown.
static bool is_unknown(intptr_t offset) {
  return offset == Type::OffsetBot;
}

// Whether the given offset is concrete (defined and compile-time known).
static bool is_concrete(intptr_t offset) {
  return !is_undefined(offset) && !is_unknown(offset);
}

// Compute base + offset components of the memory address accessed by mach.
// Return a node representing the base address, or null if the base cannot be
// found or the offset is undefined or a concrete negative value. If a non-null
// base is returned, the offset is a concrete, nonnegative value or unknown.
static const Node* get_base_and_offset(const MachNode* mach, intptr_t& offset) {
  const TypePtr* adr_type = nullptr;
  offset = 0;
  const Node* base = mach->get_base_and_disp(offset, adr_type);

  if (base == nullptr || base == NodeSentinel) {
    return nullptr;
  }

  if (offset == 0 && base->is_Mach() && base->as_Mach()->ideal_Opcode() == Op_AddP) {
    // The memory address is computed by 'base' and fed to 'mach' via an
    // indirect memory operand (indicated by offset == 0). The ultimate base and
    // offset can be fetched directly from the inputs and Ideal type of 'base'.
    offset = base->bottom_type()->isa_oopptr()->offset();
    // Even if 'base' is not an Ideal AddP node anymore, Matcher::ReduceInst()
    // guarantees that the base address is still available at the same slot.
    base = base->in(AddPNode::Base);
    assert(base != nullptr, "");
  }

  if (is_undefined(offset) || (is_concrete(offset) && offset < 0)) {
    return nullptr;
  }

  return look_through_node(base);
}

// Whether a phi node corresponds to an array allocation.
// This test is incomplete: in some edge cases, it might return false even
// though the node does correspond to an array allocation.
static bool is_array_allocation(const Node* phi) {
  precond(phi->is_Phi());
  // Check whether phi has a successor cast (CheckCastPP) to Java array pointer,
  // possibly below spill copies and other cast nodes. Limit the exploration to
  // a single path from the phi node consisting of these node types.
  const Node* current = phi;
  while (true) {
    const Node* next = nullptr;
    for (DUIterator_Fast imax, i = current->fast_outs(imax); i < imax; i++) {
      if (!current->fast_out(i)->isa_Mach()) {
        continue;
      }
      const MachNode* succ = current->fast_out(i)->as_Mach();
      if (succ->ideal_Opcode() == Op_CheckCastPP) {
        if (succ->get_ptr_type()->isa_aryptr()) {
          // Cast to Java array pointer: phi corresponds to an array allocation.
          return true;
        }
        // Other cast: record as candidate for further exploration.
        next = succ;
      } else if (succ->is_SpillCopy() && next == nullptr) {
        // Spill copy, and no better candidate found: record as candidate.
        next = succ;
      }
    }
    if (next == nullptr) {
      // No evidence found that phi corresponds to an array allocation, and no
      // candidates available to continue exploring.
      return false;
    }
    // Continue exploring from the best candidate found.
    current = next;
  }
  ShouldNotReachHere();
}

// Match the phi node that connects a TLAB allocation fast path with its slowpath
static bool is_allocation(const Node* node) {
  if (node->req() != 3) {
    return false;
  }
  const Node* const fast_node = node->in(2);
  if (!fast_node->is_Mach()) {
    return false;
  }
  const MachNode* const fast_mach = fast_node->as_Mach();
  if (fast_mach->ideal_Opcode() != Op_LoadP) {
    return false;
  }
  const TypePtr* const adr_type = nullptr;
  intptr_t offset;
  const Node* const base = get_base_and_offset(fast_mach, offset);
  if (base == nullptr || !base->is_Mach() || !is_concrete(offset)) {
    return false;
  }
  const MachNode* const base_mach = base->as_Mach();
  if (base_mach->ideal_Opcode() != Op_ThreadLocal) {
    return false;
  }
  return offset == in_bytes(Thread::tlab_top_offset());
}

static void elide_mach_barrier(MachNode* mach) {
  mach->set_barrier_data(ZBarrierElided);
}

void AgnosticBarrierSetC2::analyze_dominating_barriers_impl(Node_List& accesses, Node_List& access_dominators) const {
  Compile* const C = Compile::current();
  PhaseCFG* const cfg = C->cfg();

  for (uint i = 0; i < accesses.size(); i++) {
    MachNode* const access = accesses.at(i)->as_Mach();
    intptr_t access_offset;
    const Node* const access_obj = get_base_and_offset(access, access_offset);
    Block* const access_block = cfg->get_block_for_node(access);
    const uint access_index = block_index(access_block, access);

    if (access_obj == nullptr) {
      // No information available
      continue;
    }

    for (uint j = 0; j < access_dominators.size(); j++) {
     const  Node* const mem = access_dominators.at(j);
      if (mem->is_Phi()) {
        // Allocation node
        if (mem != access_obj) {
          continue;
        }
        if (is_unknown(access_offset) && !is_array_allocation(mem)) {
          // The accessed address has an unknown offset, but the allocated
          // object cannot be determined to be an array. Avoid eliding in this
          // case, to be on the safe side.
          continue;
        }
        assert((is_concrete(access_offset) && access_offset >= 0) || (is_unknown(access_offset) && is_array_allocation(mem)),
               "candidate allocation-dominated access offsets must be either concrete and nonnegative, or unknown (for array allocations only)");
      } else {
        // Access node
        const MachNode* const mem_mach = mem->as_Mach();
        intptr_t mem_offset;
        const Node* const mem_obj = get_base_and_offset(mem_mach, mem_offset);

        if (mem_obj == nullptr ||
            !is_concrete(access_offset) ||
            !is_concrete(mem_offset)) {
          // No information available
          continue;
        }

        if (mem_obj != access_obj || mem_offset != access_offset) {
          // Not the same addresses, not a candidate
          continue;
        }
        assert(is_concrete(access_offset) && access_offset >= 0,
               "candidate non-allocation-dominated access offsets must be concrete and nonnegative");
      }

      Block* mem_block = cfg->get_block_for_node(mem);
      const uint mem_index = block_index(mem_block, mem);

      if (access_block == mem_block) {
        // Earlier accesses in the same block
        if (mem_index < access_index && !block_has_safepoint(mem_block, mem_index + 1, access_index)) {
          elide_mach_barrier(access);
        }
      } else if (mem_block->dominates(access_block)) {
        // Dominating block? Look around for safepoints
        ResourceMark rm;
        Block_List stack;
        VectorSet visited;
        stack.push(access_block);
        bool safepoint_found = block_has_safepoint(access_block);
        while (!safepoint_found && stack.size() > 0) {
          const Block* const block = stack.pop();
          if (visited.test_set(block->_pre_order)) {
            continue;
          }
          if (block_has_safepoint(block)) {
            safepoint_found = true;
            break;
          }
          if (block == mem_block) {
            continue;
          }

          // Push predecessor blocks
          for (uint p = 1; p < block->num_preds(); ++p) {
            Block* const pred = cfg->get_block_for_node(block->pred(p));
            stack.push(pred);
          }
        }

        if (!safepoint_found) {
          elide_mach_barrier(access);
        }
      }
    }
  }
}

void AgnosticBarrierSetC2::analyze_dominating_barriers() const {
  ResourceMark rm;
  Compile* const C = Compile::current();
  PhaseCFG* const cfg = C->cfg();

  Node_List loads;
  Node_List load_dominators;

  Node_List stores;
  Node_List store_dominators;

  Node_List atomics;
  Node_List atomic_dominators;

  // Step 1 - Find accesses and allocations, and track them in lists
  for (uint i = 0; i < cfg->number_of_blocks(); ++i) {
    const Block* const block = cfg->get_block(i);
    for (uint j = 0; j < block->number_of_nodes(); ++j) {
      Node* const node = block->get_node(j);
      if (node->is_Phi()) {
        if (is_allocation(node)) {
          load_dominators.push(node);
          store_dominators.push(node);
          // An allocation can't be considered to "dominate" an atomic operation.
          // For example a CAS requires the memory location to be store-good.
          // When you have a dominating store or atomic instruction, that is
          // indeed ensured to be the case. However, as for allocations, the
          // initialized memory location could be raw null, which isn't store-good.
        }
        continue;
      } else if (!node->is_Mach()) {
        continue;
      }

      MachNode* const mach = node->as_Mach();
      switch (mach->ideal_Opcode()) {
      case Op_LoadP:
        if ((mach->barrier_data() & ZBarrierStrong) != 0 &&
            (mach->barrier_data() & ZBarrierNoKeepalive) == 0) {
          loads.push(mach);
          load_dominators.push(mach);
        }
        break;
      case Op_StoreP:
        if (mach->barrier_data() != 0) {
          stores.push(mach);
          load_dominators.push(mach);
          store_dominators.push(mach);
          atomic_dominators.push(mach);
        }
        break;
      case Op_CompareAndExchangeP:
      case Op_CompareAndSwapP:
      case Op_GetAndSetP:
        if (mach->barrier_data() != 0) {
          atomics.push(mach);
          load_dominators.push(mach);
          store_dominators.push(mach);
          atomic_dominators.push(mach);
        }
        break;

      default:
        break;
      }
    }
  }

  // Step 2 - Find dominating accesses or allocations for each access
  analyze_dominating_barriers_impl(loads, load_dominators);
  analyze_dominating_barriers_impl(stores, store_dominators);
  analyze_dominating_barriers_impl(atomics, atomic_dominators);
}

void AgnosticBarrierSetC2::eliminate_gc_barrier(PhaseMacroExpand* macro, Node* node) const {
  eliminate_gc_barrier_data(node);
}

void AgnosticBarrierSetC2::eliminate_gc_barrier_data(Node* node) const {
  if (node->is_LoadStore()) {
    LoadStoreNode* loadstore = node->as_LoadStore();
    loadstore->set_barrier_data(0);
  } else if (node->is_Mem()) {
    MemNode* mem = node->as_Mem();
    mem->set_barrier_data(0);
  }
}

static void refine_barrier_by_new_val_type(const Node* n) {
  if (n->Opcode() != Op_StoreP &&
      n->Opcode() != Op_StoreN) {
    return;
  }
  MemNode* store = n->as_Mem();
  const Node* newval = n->in(MemNode::ValueIn);
  assert(newval != nullptr, "");
  const Type* newval_bottom = newval->bottom_type();
  TypePtr::PTR newval_type = newval_bottom->make_ptr()->ptr();
  BarrierData barrier_data = store->barrier_data();
  if (!newval_bottom->isa_oopptr() &&
      !newval_bottom->isa_narrowoop() &&
      newval_type != TypePtr::Null) {
    // newval is neither an OOP nor null, so there is no barrier to refine.
    assert(barrier_data == 0, "non-OOP stores should have no barrier data");
    return;
  }
  if (barrier_data == 0) {
    // No barrier to refine.
    return;
  }
  if (newval_type == TypePtr::Null) {
    // Simply elide post-barrier if writing null.
    barrier_data &= ~G1C2BarrierPost;
    barrier_data &= ~G1C2BarrierPostNotNull;
  } else if (((barrier_data & G1C2BarrierPost) != 0) &&
             newval_type == TypePtr::NotNull) {
    // If the post-barrier has not been elided yet (e.g. due to newval being
    // freshly allocated), mark it as not-null (simplifies barrier tests and
    // compressed OOPs logic).
    barrier_data |= G1C2BarrierPostNotNull;
  }
  store->set_barrier_data(barrier_data);
  return;
}

// Refine (not really expand) G1 barriers by looking at the new value type
// (whether it is necessarily null or necessarily non-null).
bool AgnosticBarrierSetC2::expand_barriers(Compile* C, PhaseIterGVN& igvn) const {
  ResourceMark rm;
  VectorSet visited;
  Node_List worklist;
  worklist.push(C->root());
  while (worklist.size() > 0) {
    Node* n = worklist.pop();
    if (visited.test_set(n->_idx)) {
      continue;
    }
    refine_barrier_by_new_val_type(n);
    for (uint j = 0; j < n->req(); j++) {
      Node* in = n->in(j);
      if (in != nullptr) {
        worklist.push(in);
      }
    }
  }
  return false;
}

uint AgnosticBarrierSetC2::estimated_barrier_size(const Node* node) const {
  // G1 barrier size
  // These Ideal node counts are extracted from the pre-matching Ideal graph
  // generated when compiling the following method with early barrier expansion:
  //   static void write(MyObject obj1, Object o) {
  //     obj1.o1 = o;
  //   }
  BarrierData barrier_data = MemNode::barrier_data(node);
  assert(barrier_data != 0, "should be a barrier node");
  uint nodes = 0;
  if ((barrier_data & G1C2BarrierPre) != 0) {
    nodes += 50;
  }
  if ((barrier_data & G1C2BarrierPost) != 0) {
    // Approximate the number of nodes needed with the number of Assembly instructions
    // since we do not have real nodes.
    nodes += 12;
  }

  // ZGC barrier size
  uint uncolor_or_color_size = node->is_Load() ? 1 : 2;
  if ((barrier_data & ZBarrierElided) != 0) {
    return nodes;
  }
  // A compare and branch corresponds to approximately four fast-path Ideal
  // nodes (Cmp, Bool, If, If projection). The slow path (If projection and
  // runtime call) is excluded since the corresponding code is laid out
  // separately and does not directly affect performance.
  return nodes + 4;
}

bool AgnosticBarrierSetC2::can_initialize_object(const StoreNode* store) const {
  assert(store->Opcode() == Op_StoreP || store->Opcode() == Op_StoreN, "OOP store expected");
  // It is OK to move the store across the object initialization boundary only
  // if it does not have any barrier, or if it has barriers that can be safely
  // elided (because of the compensation steps taken on the allocation slow path
  // when ReduceInitialCardMarks is enabled).
  return (MemNode::barrier_data(store) == 0) || use_ReduceInitialCardMarks();
}

bool AgnosticBarrierSetC2::array_copy_requires_gc_barriers(bool tightly_coupled_alloc, BasicType type,
                                                    bool is_clone, bool is_clone_instance,
                                                    ArrayCopyPhase phase) const {
  if (phase == ArrayCopyPhase::Parsing) {
    return false;
  }
  if (phase == ArrayCopyPhase::Optimization) {
    return is_clone_instance;
  }
  // else ArrayCopyPhase::Expansion
  return type == T_OBJECT || type == T_ARRAY;
}

BarrierData AgnosticBarrierSetC2::g1_get_store_barrier(C2Access& access) const {
  if (!access.is_parse_access()) {
    // Only support for eliding barriers at parse time for now.
    return G1C2BarrierPre | G1C2BarrierPost;
  }
  GraphKit* kit = (static_cast<C2ParseAccess&>(access)).kit();
  Node* ctl = kit->control();
  Node* adr = access.addr().node();
  uint adr_idx = kit->C->get_alias_index(access.addr().type());
  assert(adr_idx != Compile::AliasIdxTop, "use other store_to_memory factory");

  bool can_remove_pre_barrier = g1_can_remove_pre_barrier(kit, &kit->gvn(), adr, access.type(), adr_idx);

  // We can skip marks on a freshly-allocated object in Eden. Keep this code in
  // sync with CardTableBarrierSet::on_slowpath_allocation_exit. That routine
  // informs GC to take appropriate compensating steps, upon a slow-path
  // allocation, so as to make this card-mark elision safe.
  // The post-barrier can also be removed if null is written. This case is
  // handled by G1BarrierSetC2::expand_barriers, which runs at the end of C2's
  // platform-independent optimizations to exploit stronger type information.
  bool can_remove_post_barrier = use_ReduceInitialCardMarks() &&
    ((access.base() == kit->just_allocated_object(ctl)) ||
     g1_can_remove_post_barrier(kit, &kit->gvn(), ctl, adr));

  BarrierData barriers = 0;
  if (!can_remove_pre_barrier) {
    barriers |= G1C2BarrierPre;
  }
  if (!can_remove_post_barrier) {
    barriers |= G1C2BarrierPost;
  }

  return barriers;
}

Node* AgnosticBarrierSetC2::store_at_resolved(C2Access& access, C2AccessValue& val) const {
  // G1
  DecoratorSet decorators = access.decorators();
  bool anonymous = (decorators & ON_UNKNOWN_OOP_REF) != 0;
  bool in_heap = (decorators & IN_HEAP) != 0;
  bool tightly_coupled_alloc = (decorators & C2_TIGHTLY_COUPLED_ALLOC) != 0;
  bool need_store_barrier = !(tightly_coupled_alloc && use_ReduceInitialCardMarks()) && (in_heap || anonymous);
  bool no_keepalive = (decorators & AS_NO_KEEPALIVE) != 0;
  if (access.is_oop() && need_store_barrier) {
    access.set_barrier_data(g1_get_store_barrier(access));
    if (tightly_coupled_alloc) {
      assert(!use_ReduceInitialCardMarks(),
             "post-barriers are only needed for tightly-coupled initialization stores when ReduceInitialCardMarks is disabled");
      // Pre-barriers are unnecessary for tightly-coupled initialization stores.
      access.set_barrier_data(access.barrier_data() & ~G1C2BarrierPre);
    }
  }
  if (no_keepalive) {
    // No keep-alive means no need for the pre-barrier.
    access.set_barrier_data(access.barrier_data() & ~G1C2BarrierPre);
  }

  z_set_barrier_data(access); // ZGC
  return BarrierSetC2::store_at_resolved(access, val);
}

class AgnosticBarrierSetC2State : public BarrierSetC2State {
private:
  GrowableArray<AgnosticBarrierStubC2*>* _stubs;
  int                           _trampoline_stubs_count;
  int                           _stubs_start_offset;

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
  analyze_dominating_barriers();
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

#ifndef PRODUCT
void AgnosticBarrierSetC2::dump_barrier_data(const MachNode* mach, outputStream* st) const {
  if ((mach->barrier_data() & G1C2BarrierPre) != 0) {
    st->print("G1 pre ");
  }
  if ((mach->barrier_data() & G1C2BarrierPost) != 0) {
    st->print("G1 post ");
  }
  if ((mach->barrier_data() & G1C2BarrierPostNotNull) != 0) {
    st->print("G1 notnull ");
  }
  if ((mach->barrier_data() & ZBarrierStrong) != 0) {
    st->print("Z strong ");
  }
  if ((mach->barrier_data() & ZBarrierWeak) != 0) {
    st->print("Z weak ");
  }
  if ((mach->barrier_data() & ZBarrierPhantom) != 0) {
    st->print("Z phantom ");
  }
  if ((mach->barrier_data() & ZBarrierNoKeepalive) != 0) {
    st->print("Z nokeepalive ");
  }
  if ((mach->barrier_data() & ZBarrierNative) != 0) {
    st->print("Z native ");
  }
  if ((mach->barrier_data() & ZBarrierElided) != 0) {
    st->print("Z elided ");
  }
}
#endif // !PRODUCT
