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

#ifndef SHARE_GC_SHARED_C2_AGNOSTICBARRIERSETC2_HPP
#define SHARE_GC_SHARED_C2_AGNOSTICBARRIERSETC2_HPP

#include "gc/shared/c2/barrierSetC2.hpp"
#include "gc/shared/c2/cardTableBarrierSetC2.hpp"
#include "gc/g1/c2/g1BarrierSetC2.hpp"
#include "gc/shared/gc_globals.hpp"
#include "gc/z/c2/zBarrierSetC2.hpp"
#include "opto/machnode.hpp"
#include "register_aarch64.hpp"

const uint8_t AgnosticBarrier = 1;  // Async agnostic barriers are always pre
const uint8_t AgnosticElided  = 2;

class AgnosticBarrierSetC2State;
class AgnosticBarrierSetC2Logic : public AllStatic {
friend class AgnosticStoreBarrierStubC2;

public:
  static void write_barrier_data(C2Access& access);
  static void* create_barrier_state(Arena* comp_arena);
  static void emit_stubs(CodeBuffer& cb);
  static void eliminate_gc_barrier_data(Node* node);

private:
  static AgnosticBarrierSetC2State* barrier_set_state();
};

class ZAgnosticBarrierSetC2 : public ZBarrierSetC2 {
protected:
  virtual Node* store_at_resolved(C2Access& access, C2AccessValue& val) const {
    if (GCASB) {
      AgnosticBarrierSetC2Logic::write_barrier_data(access);
      return BarrierSetC2::store_at_resolved(access, val);
    }
    return ZBarrierSetC2::store_at_resolved(access, val);
  }

public:
  virtual void* create_barrier_state(Arena* comp_arena) const;
  virtual void emit_stubs(CodeBuffer& cb) const;
  virtual void eliminate_gc_barrier(PhaseMacroExpand* macro, Node* node) const;
  virtual void eliminate_gc_barrier_data(Node* node) const;
  virtual void late_barrier_analysis() const;
};

class G1AgnosticBarrierSetC2 : public G1BarrierSetC2 {
protected:
  virtual Node* store_at_resolved(C2Access& access, C2AccessValue& val) const {
    if (GCASB) {
      AgnosticBarrierSetC2Logic::write_barrier_data(access);
      return BarrierSetC2::store_at_resolved(access, val);
    }
    return G1BarrierSetC2::store_at_resolved(access, val);
  }

public:
  virtual void* create_barrier_state(Arena* comp_arena) const;
  virtual void emit_stubs(CodeBuffer& cb) const;
  virtual void eliminate_gc_barrier(PhaseMacroExpand* macro, Node* node) const;
  virtual void eliminate_gc_barrier_data(Node* node) const;
  virtual void late_barrier_analysis() const;
};

class CardTableAgnosticBarrierSetC2 : public CardTableBarrierSetC2 {
protected:
  virtual Node* store_at_resolved(C2Access& access, C2AccessValue& val) const {
    if (GCASB) {
      AgnosticBarrierSetC2Logic::write_barrier_data(access);
      return BarrierSetC2::store_at_resolved(access, val);
    }
    return BarrierSetC2::store_at_resolved(access, val);
  }

public:
  virtual void* create_barrier_state(Arena* comp_arena) const;
  virtual void emit_stubs(CodeBuffer& cb) const;
  virtual void eliminate_gc_barrier(PhaseMacroExpand* macro, Node* node) const;
  virtual void eliminate_gc_barrier_data(Node* node) const;
  virtual void late_barrier_analysis() const;
};

class AgnosticBarrierStubC2 : public BarrierStubC2 {
protected:
  AgnosticBarrierStubC2(const MachNode* node) : BarrierStubC2(node) {}
};

class AgnosticStoreBarrierStubC2 : public AgnosticBarrierStubC2 {
private:
  Address _ref_addr;

protected:
  AgnosticStoreBarrierStubC2(const MachNode* node, Address ref_addr);
  
public:
  static AgnosticStoreBarrierStubC2* create(const MachNode* node, Address ref_addr);
  void emit_code(MacroAssembler& masm);

  Address ref_addr() const  { return _ref_addr; }
};

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
    // Don't need liveness data for nodes without barriers
    return mach->barrier_data() != ZBarrierElided;
  }

  bool needs_livein_data() const {
    return true;
  }
};

#endif // SHARE_GC_SHARED_C2_AGNOSTICBARRIERSETC2_HPP
