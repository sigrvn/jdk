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

#ifndef CPU_AARCH64_GC_SHARED_AGNOSTICBARRIERSETASSEMBLER_AARCH64_HPP
#define CPU_AARCH64_GC_SHARED_AGNOSTICBARRIERSETASSEMBLER_AARCH64_HPP

#include "asm/macroAssembler.hpp"
#include "gc/g1/g1BarrierSetAssembler_aarch64.hpp"
#include "gc/shared/c2/agnosticBarrierSetC2.hpp"
#include "gc/z/c2/zBarrierSetC2.hpp"
#include "utilities/macros.hpp"

class AgnosticBarrierSetAssembler: public G1BarrierSetAssembler {
public:
#ifdef COMPILER2
  void agnostic_store_barrier_c2(MacroAssembler* masm,
                                 Address obj,
                                 Register pre_val,
                                 Register new_val, 
                                 Register thread,
                                 Register tmp1,
                                 Register tmp2,
                                 AgnosticStoreBarrierStubC2* c2_stub);
  void generate_store_barrier_stub_c2(MacroAssembler* masm, AgnosticStoreBarrierStubC2* stub) const;
#endif
};


#endif // CPU_AARCH64_GC_SHARED_AGNOSTICBARRIERSETASSEMBLER_AARCH64_HPP
