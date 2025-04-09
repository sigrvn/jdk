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
#include "gc/z/c2/zBarrierSetC2.hpp"

const uint8_t AgnosticBarrierIsInitialized = 1;
const uint8_t AgnosticBarrierInHeap        = 2;

class AgnosticBarrierStubC2 : public BarrierStubC2 {
public:
  static bool needs_store_barrier(const MachNode* node);

  AgnosticBarrierStubC2(const MachNode* node);
  virtual void emit_code(MacroAssembler& masm) = 0;
};

class AgnosticStoreBarrierStubC2 : public AgnosticBarrierStubC2 {
private:
  Register _src;  // g1:new_val,  z:rnew_zpointer
  Register _dst;  // g1:obj,      z:ref_addr
  Register _aux;  // g1:pre_val,  z:rnew_zaddress
  Register _tmp1; // g1:tmp1,     z:rtmp
  Register _tmp2; // g1:tmp2

  bool _is_initialized;
  bool _in_heap;
  bool _is_atomic;
  bool _is_nokeepalive;

protected:
  AgnosticStoreBarrierStubC2(const MachNode* node, bool is_initialized, bool in_heap, bool is_atomic, bool is_nokeepalive);

public:
  static AgnosticStoreBarrierStubC2* create(const MachNode* node, bool is_initialized, bool in_heap, bool is_atomic, bool is_nokeepalive);
  void initialize_registers(Register src,
                            Register dst,
                            Register aux,
                            Register tmp1,
                            Register tmp2);

  Register src() const;
  Register dst() const;
  Register pre_val() const;
  Register new_zaddress() const;
  Register tmp1() const;
  Register tmp2() const;

  bool is_initialized() const;
  bool in_heap() const;
  bool is_atomic() const;
  bool is_nokeepalive() const;

  virtual void emit_code(MacroAssembler& masm);
};

// AgnosticBarrierSetC2 is an experimental universal barrier for all supported GC barriers for C2.
// This specialized barrier set is generated using the -XX:+UseAgnosticBarriers feature flag.
class AgnosticBarrierSetC2 : public ZBarrierSetC2 {
protected:
  virtual Node* store_at_resolved(C2Access& access, C2AccessValue& val) const;

public:
  virtual void* create_barrier_state(Arena* comp_arena) const;
  virtual void emit_stubs(CodeBuffer& cb) const;
};

#endif // SHARE_GC_AGNOSTIC_C2_AGNOSTICBARRIERSETC2_HPP
