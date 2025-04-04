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
#include "gc/z/c2/zBarrierSetC2.hpp"
#include "opto/machnode.hpp"
#include "register_aarch64.hpp"

const int AgnosticBarrier = 1;  // Async agnostic barriers are always pre

class AgnosticBarrierSetC2: public G1BarrierSetC2 {
protected:
  virtual Node* store_at_resolved(C2Access& access, C2AccessValue& val) const;

public:
  virtual void* create_barrier_state(Arena* comp_arena) const;
};

class AgnosticBarrierStubC2 : public BarrierStubC2 {
protected:
  AgnosticBarrierStubC2(const MachNode* node) : BarrierStubC2(node) {}
};

class AgnosticStoreBarrierStubC2 : public AgnosticBarrierStubC2 {
private:
  Address _ref_addr;
  Register _prev_val;
  Register _tmp;
  Register _new;
  bool _is_native;
  bool _is_atomic;

protected:
  AgnosticStoreBarrierStubC2(const MachNode* node, Address ref_addr, Register new_zaddress, Register new_zpointer, bool is_native, bool is_atomic, Register n);
  
public:
  static AgnosticStoreBarrierStubC2* create(const MachNode* node, Address ref_addr, Register new_zaddress, Register new_zpointer, bool is_native, bool is_atomic, Register n);
  void emit_code(MacroAssembler& masm);

  Address ref_addr() const  {    return _ref_addr;  }
  Register prev_val() const {    return _prev_val;  }
  Register tmp() const      {    return _tmp;  }
  Register n() const      {    return _new;  }
  bool is_native() const    {    return _is_native;  }
  bool is_atomic() const    {    return _is_atomic;  }
};

#endif // SHARE_GC_SHARED_C2_AGNOSTICBARRIERSETC2_HPP
