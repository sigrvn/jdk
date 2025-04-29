/*
 * Copyright (c) 2024, 2025 Oracle and/or its affiliates. All rights reserved.
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

#include "gc/g1/c2/g1BarrierSetC2.hpp"
#include "gc/shared/c2/cardTableBarrierSetC2.hpp"
#include "gc/shared/gc_globals.hpp"
#include "gc/z/c2/zBarrierSetC2.hpp"

const uint8_t AgnosticBarrierSATB        = G1C2BarrierPre; // 1
const uint8_t AgnosticBarrierCardMark    = G1C2BarrierPost; // 2
const uint8_t AgnosticBarrierNokeepalive = ZBarrierNoKeepalive; // 8
const uint8_t AgnosticBarrierNative      = ZBarrierNative; // 16
const uint8_t AgnosticBarrierElided      = ZBarrierElided; // 32

class AgnosticBarrierSetC2Logic : public AllStatic {
public:
  static void determine_barrier_data(C2Access& access);
  static void eliminate_barrier_data(Node* node);
};

class AgnosticZBarrierSetC2 : public ZBarrierSetC2 {
protected:
  virtual Node* store_at_resolved(C2Access& access, C2AccessValue& val) const {
    if (UseAgnosticBarriers) {
      AgnosticBarrierSetC2Logic::determine_barrier_data(access);
      return BarrierSetC2::store_at_resolved(access, val);
    }
    return ZBarrierSetC2::store_at_resolved(access, val);
  }

  virtual void eliminate_gc_barrier(PhaseMacroExpand* macro, Node* node) const {
    eliminate_gc_barrier_data(node);
  }

  virtual void eliminate_gc_barrier_data(Node* node) const {
    AgnosticBarrierSetC2Logic::eliminate_barrier_data(node);
  }

  virtual void late_barrier_analysis() const {
    compute_liveness_at_stubs();
  }
};

class AgnosticG1BarrierSetC2 : public G1BarrierSetC2 {
protected:
  virtual Node* store_at_resolved(C2Access& access, C2AccessValue& val) const {
    if (UseAgnosticBarriers) {
      AgnosticBarrierSetC2Logic::determine_barrier_data(access);
      return BarrierSetC2::store_at_resolved(access, val);
    }
    return G1BarrierSetC2::store_at_resolved(access, val);
  }

  virtual void eliminate_gc_barrier(PhaseMacroExpand* macro, Node* node) const {
    eliminate_gc_barrier_data(node);
  }

  virtual void eliminate_gc_barrier_data(Node* node) const {
    AgnosticBarrierSetC2Logic::eliminate_barrier_data(node);
  }
};

class AgnosticCardTableBarrierSetC2 : public CardTableBarrierSetC2 {
protected:
  virtual Node* store_at_resolved(C2Access& access, C2AccessValue& val) const {
    if (UseAgnosticBarriers) {
      AgnosticBarrierSetC2Logic::determine_barrier_data(access);
      return BarrierSetC2::store_at_resolved(access, val);
    }
    return CardTableBarrierSetC2::store_at_resolved(access, val);
  }

  virtual void eliminate_gc_barrier(PhaseMacroExpand* macro, Node* node) const {
    eliminate_gc_barrier_data(node);
  }

  virtual void eliminate_gc_barrier_data(Node* node) const {
    AgnosticBarrierSetC2Logic::eliminate_barrier_data(node);
  }

  void* create_barrier_state(Arena* comp_arena) const;
  virtual void emit_stubs(CodeBuffer& cb) const;
};

class AgnosticStoreBarrierStubC2 : public BarrierStubC2 {
private:
  Register _src;  // g1:new_val,  z:rnew_zpointer
  Register _dst;  // g1:obj,      z:ref_addr
  Register _aux;  // g1:pre_val,  z:rnew_zaddress
  Register _tmp1; // g1:tmp1,     z:rtmp
  Register _tmp2; // g1:tmp2

  const bool _is_atomic;
  const bool _is_native;
  const bool _is_nokeepalive;

protected:
  AgnosticStoreBarrierStubC2(const MachNode* node, bool is_atomic, bool is_native, bool is_nokeepalive);

public:
  static AgnosticStoreBarrierStubC2* create(const MachNode* node, bool is_atomic, bool is_native, bool is_nokeepalive);
  void initialize_registers(Register src,
      Register dst,
      Register aux,
      Register tmp1,
      Register tmp2);

  Register src() const;
  Register dst() const;
  Register aux() const;
  Register tmp1() const;
  Register tmp2() const;

  bool is_atomic() const;
  bool is_native() const;
  bool is_nokeepalive() const;

  virtual void emit_code(MacroAssembler& masm);
};

class AgnosticBarrierSetC2State : public BarrierSetC2State {
private:
  GrowableArray<BarrierStubC2*>* _stubs;
  int                            _trampoline_stubs_count;
  int                            _stubs_start_offset;

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
    return mach->barrier_data() != AgnosticBarrierElided;
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

#endif // SHARE_GC_AGNOSTIC_C2_AGNOSTICBARRIERSETC2_HPP
