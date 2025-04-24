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
 */

#ifndef SHARE_GC_SHARED_CARDTABLETHREADLOCALDATA_HPP
#define SHARE_GC_SHARED_CARDTABLETHREADLOCALDATA_HPP

#include "gc/agnostic/agnosticThreadLocalData.hpp"
#include "gc/shared/cardTable.hpp"

class CardTableThreadLocalData : public AgnosticThreadLocalData {
protected:
  CardTable::CardValue* _byte_map_base;

  CardTableThreadLocalData() {}

  static CardTableThreadLocalData* data(Thread* thread) {
    return thread->gc_data<CardTableThreadLocalData>();
  }

public:
  static void create(Thread* thread) {
    new (data(thread)) CardTableThreadLocalData();
  }

  static void destroy(Thread* thread) {
    data(thread)->~CardTableThreadLocalData();
  }

  static ByteSize byte_map_base_offset() {
    return Thread::gc_data_offset() + byte_offset_of(CardTableThreadLocalData, _byte_map_base);
  }

  static CardTable::CardValue* byte_map_base(Thread* thread) {
    return data(thread)->_byte_map_base;
  }

  static void set_byte_map_base(Thread* thread, CardTable::CardValue* new_byte_map_base) {
    data(thread)->_byte_map_base = new_byte_map_base;
  }
};

#endif //  SHARE_GC_SHARED_CARDTABLETHREADLOCALDATA_HPP

