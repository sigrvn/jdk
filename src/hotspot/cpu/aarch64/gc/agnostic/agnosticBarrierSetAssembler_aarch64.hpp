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

#include "gc/shared/barrierSetAssembler.hpp"
#ifdef COMPILER2
#include "gc/agnostic/c2/agnosticBarrierSetC2.hpp"
#endif

const int AgnosticBarrierRelocationFormatPointerBumpScaleBeforeMov = 4;
const int AgnosticBarrierRelocationFormatSrcPointerShiftBeforeOrr = 5;
const int AgnosticBarrierRelocationFormatBufferEntryPointerOffsetBeforeStr = 6;
const int AgnosticBarrierRelocationFormatBufferEntryValueOffsetBeforeStr = 7;
const int AgnosticBarrierRelocationFormatSATBBaseAddressBeforeAdd  = 8;
const int AgnosticBarrierRelocationFormatSATBIndexOffsetBeforeMov  = 9;
const int AgnosticBarrierRelocationFormatSATBBufferOffsetBeforeAdd = 10;

class AgnosticBarrierSetAssembler : public BarrierSetAssembler {
public:
#ifdef COMPILER2
  void generate_store_barrier_stub_c2(MacroAssembler* masm, AgnosticStoreBarrierStubC2* stub) const;
  void agnostic_store_barrier(MacroAssembler* masm,
      Register src,
      Register dst,
      Register aux,
      Register tmp1,
      Register tmp2,
      AgnosticStoreBarrierStubC2* stub) const;
#endif
};
