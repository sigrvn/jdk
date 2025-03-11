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

#ifndef CPU_AARCH64_GC_AGNOSTIC_AGNOSTICBARRIERSETASSEMBLER_AARCH64_HPP
#define CPU_AARCH64_GC_AGNOSTIC_AGNOSTICBARRIERSETASSEMBLER_AARCH64_HPP

#include "asm/macroAssembler.hpp"
#include "gc/shared/barrierSetAssembler.hpp"
#include "utilities/macros.hpp"

class ZStoreBarrierStubC2;
class G1PreBarrierStubC2;

class AgnosticBarrierSetAssembler : public BarrierSetAssembler {
public:
#ifdef COMPILER2
  void pre_write_barrier(MacroAssembler* masm,
                         Address ref_addr,
                         Register a, // g1:obj,     z:rnew_zaddress
                         Register b, // g1:pre_val, z:rnew_zpointer
                         Register c, // g1:thread
                         Register d, // g1:tmp1,    z:rtmp
                         Register e, // g1:tmp2
                         G1PreBarrierStubC2* g1_stub,
                         ZStoreBarrierStubC2* z_stub);

  void post_write_barrier(MacroAssembler* masm,
                          Register store_addr,
                          Register new_val,
                          Register thread,
                          Register tmp1,
                          Register tmp2,
                          bool new_val_maybe_null);
#endif
};

#endif // CPU_AARCH64_GC_AGNOSTIC_AGNOSTICBARRIERSETASSEMBLER_AARCH64_HPP
