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

#ifndef SHARE_GC_SHARED_BARRIERDATA_HPP
#define SHARE_GC_SHARED_BARRIERDATA_HPP

#include "utilities/globalDefinitions.hpp"

// BarrierData holds internal barrier metadata used by C2 for each supported GC.
typedef uint16_t BarrierData;

const BarrierData G1C2BarrierPre          =   1;
const BarrierData G1C2BarrierPost         =   2;
const BarrierData G1C2BarrierPostNotNull  =   4;

const BarrierData ZBarrierStrong          =   8;
const BarrierData ZBarrierWeak            =  16;
const BarrierData ZBarrierPhantom         =  32;
const BarrierData ZBarrierNoKeepalive     =  64;
const BarrierData ZBarrierNative          = 128;
const BarrierData ZBarrierElided          = 256;

#endif // SHARE_GC_SHARED_BARRIERDATA_HPP
