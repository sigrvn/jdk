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

#ifndef SHARE_GC_AGNOSTIC_C2_AGNOSTICBARRIERSETC2_HPP
#define SHARE_GC_AGNOSTIC_C2_AGNOSTICBARRIERSETC2_HPP

#include "gc/shared/barrierData.hpp"
#include "gc/shared/c2/barrierSetC2.hpp"

class AgnosticBarrierStubC2 : public BarrierStubC2 {
public:
  AgnosticBarrierStubC2(const MachNode* node);
  virtual void emit_code(MacroAssembler& masm) = 0;
};

class AgnosticPreBarrierStubC2 : public AgnosticBarrierStubC2 {
private:

protected:
  AgnosticPreBarrierStubC2(const MachNode* node);

public:
  virtual void emit_code(MacroAssembler& masm);
};

class AgnosticPostBarrierStubC2 : public AgnosticBarrierStubC2 {
private:

protected:
  AgnosticPostBarrierStubC2(const MachNode* node);

public:
  virtual void emit_code(MacroAssembler& masm);
};


// AgnosticBarrierSetC2 is an experimental universal barrier for all supported GC barriers for C2.
// This specialized barrier set is generated using the -XX:+UseAgnosticBarriers feature flag.
class AgnosticBarrierSetC2: public BarrierSetC2 {
private:
  void analyze_dominating_barriers_impl(Node_List& accesses, Node_List& access_dominators) const;
  void analyze_dominating_barriers() const;

protected:
  bool g1_can_remove_pre_barrier(GraphKit* kit,
                                 PhaseValues* phase,
                                 Node* adr,
                                 BasicType bt,
                                 uint adr_idx) const;

  bool g1_can_remove_post_barrier(GraphKit* kit,
                                  PhaseValues* phase, Node* store,
                                  Node* adr) const;

  BarrierData g1_get_store_barrier(C2Access& access) const;

  virtual Node* load_at_resolved(C2Access& access, const Type* val_type) const;
  virtual Node* store_at_resolved(C2Access& access, C2AccessValue& val) const;
  virtual Node* atomic_cmpxchg_val_at_resolved(C2AtomicParseAccess& access, Node* expected_val,
                                               Node* new_val, const Type* value_type) const;
  virtual Node* atomic_cmpxchg_bool_at_resolved(C2AtomicParseAccess& access, Node* expected_val,
                                                Node* new_val, const Type* value_type) const;
  virtual Node* atomic_xchg_at_resolved(C2AtomicParseAccess& access, Node* new_val, const Type* value_type) const;

public:
  virtual void eliminate_gc_barrier(PhaseMacroExpand* macro, Node* node) const;
  virtual void eliminate_gc_barrier_data(Node* node) const;
  virtual bool expand_barriers(Compile* C, PhaseIterGVN& igvn) const;
  virtual uint estimated_barrier_size(const Node* node) const;
  virtual bool can_initialize_object(const StoreNode* store) const;
  virtual bool array_copy_requires_gc_barriers(bool tightly_coupled_alloc,
                                               BasicType type,
                                               bool is_clone,
                                               bool is_clone_instance,
                                               ArrayCopyPhase phase) const;
  virtual void* create_barrier_state(Arena* comp_arena) const;
  virtual void emit_stubs(CodeBuffer& cb) const;
  virtual void late_barrier_analysis() const;

  bool use_ReduceInitialCardMarks() const;

#ifndef PRODUCT
  virtual void dump_barrier_data(const MachNode* mach, outputStream* st) const;
#endif
};

#endif // SHARE_GC_AGNOSTIC_C2_AGNOSTICBARRIERSETC2_HPP
