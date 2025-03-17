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

#ifndef SHARE_GC_AGNOSTIC_C2_AGNOSTICBARRIERSETC2_HPP
#define SHARE_GC_AGNOSTIC_C2_AGNOSTICBARRIERSETC2_HPP

#include "gc/g1/c2/g1BarrierSetC2.hpp"
#include "gc/z/c2/zBarrierSetC2.hpp"

// AgnosticBarrierSetC2 is an experimental universal barrier for all supported GC barriers for C2.
// This specialized barrier set is generated using the -XX:+UseAgnosticBarriers feature flag.
class AgnosticBarrierSetC2 : public ZBarrierSetC2 {
protected:
  // Important properties to consider for stores:
  // 1. Uninitialized oop or initialized oop (if initialized we should use SATB, if not we don’t)
  // 2. In heap or not in heap (if in the heap we need to do remset maintenance, otherwise we don’t)
  virtual Node* store_at_resolved(C2Access& access, C2AccessValue& val) const;

public:
  virtual void* create_barrier_state(Arena* comp_arena) const;
  virtual void emit_stubs(CodeBuffer& cb) const;
};

#endif // SHARE_GC_AGNOSTIC_C2_AGNOSTICBARRIERSETC2_HPP
