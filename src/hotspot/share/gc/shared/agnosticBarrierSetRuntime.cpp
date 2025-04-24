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

#include "gc/g1/g1BarrierSet.hpp"
#include "gc/g1/g1CollectedHeap.hpp"
#include "gc/g1/g1ThreadLocalData.hpp"
#include "gc/parallel/parallelScavengeHeap.hpp"
#include "gc/parallel/psCardTable.hpp"
#include "gc/serial/cardTableRS.hpp"
#include "gc/serial/serialHeap.hpp"
#include "gc/shared/agnosticStoreBarrierBuffer.hpp"
#include "gc/shared/agnosticThreadLocalData.hpp"
#include "gc/shared/cardTable.hpp"
#include "gc/z/c2/zBarrierSetC2.hpp"
#include "gc/z/zStoreBarrierBuffer.hpp"
#include "memory/resourceArea.hpp"
#include "memory/universe.hpp"
#include "oops/compressedOops.hpp"
#include "oops/oop.hpp"
#include "precompiled.hpp"
#include "gc/shared/agnosticBarrierSetRuntime.hpp"
#include "oops/access.hpp"
#include "runtime/interfaceSupport.inline.hpp"
#include "runtime/javaThread.hpp"
#include "utilities/debug.hpp"
#include <cstddef>
#include <cstdint>
#include <cstdio>

void AgnosticBarrierSetRuntime::g1_slow_path(oopDesc* oop, Thread* thread) {
  SATBMarkQueue& queue = G1ThreadLocalData::satb_mark_queue(thread);
  CardTable::CardValue* table = G1ThreadLocalData::byte_map_base(thread);
  AgnosticStoreBarrierBuffer* buffer = AgnosticThreadLocalData::agnostic_store_barrier_buffer(thread);

  // Flush the buffer
  while (buffer->is_empty() == false) {
    AgnosticStoreBarrierEntry* entry = buffer->pop();
    oopDesc* pre_val = entry->_prev;
    oopDesc* ref_addr = entry->_p;
    assert(ref_addr != nullptr, "must be");

    // SATB
    if (pre_val != nullptr && queue.is_active()) {
      G1BarrierSet::satb_mark_queue_set().enqueue_known_active(queue, pre_val);
    }

    // CT
    G1CardTable::CardValue* result = &table[uintptr_t(ref_addr) >> G1CardTable::card_shift()];
    *result = G1CardTable::dirty_card_val();
  }

  // Deal with the last item that we could not buffer
  oopDesc* pre_val = oop->obj_field_acquire(0);

  // SATB
  if (pre_val != nullptr && queue.is_active()) {
    G1BarrierSet::satb_mark_queue_set().enqueue_known_active(queue, pre_val);
  }

  // CT
  G1CardTable::CardValue* result = &table[uintptr_t(oop) >> G1CardTable::card_shift()];
  *result = G1CardTable::dirty_card_val();
}

void AgnosticBarrierSetRuntime::ct_slow_path(oopDesc* oop, Thread* thread) {
  CardTable::CardValue* table;
  if (Universe::heap()->kind() == CollectedHeap::Serial) {
    table = SerialHeap::heap()->rem_set()->byte_map_base();
  } else {
    assert(Universe::heap()->kind() == CollectedHeap::Parallel, "must be");
    table = ParallelScavengeHeap::heap()->card_table()->byte_map_base();
  }
  assert((uint64_t)((CardTableBarrierSet*)(BarrierSet::barrier_set()))->card_table()->byte_map_base()==(uint64_t)table, "sanity check");
  
  AgnosticStoreBarrierBuffer* buffer = AgnosticThreadLocalData::agnostic_store_barrier_buffer(thread);

  // Flush the buffer
  while (buffer->is_empty() == false) {
    AgnosticStoreBarrierEntry* entry = buffer->pop();
    oopDesc* pre_val = entry->_prev;
    oopDesc* ref_addr = entry->_p;
    assert(ref_addr != nullptr, "must be");

    // CT
    CardTable::CardValue* result = &table[uintptr_t(ref_addr) >> CardTable::card_shift()];
    *result = CardTable::dirty_card_val();
  }

  // Deal with the last item we could not buffer
  CardTable::CardValue* result = &table[uintptr_t(oop) >> CardTable::card_shift()];
  *result = CardTable::dirty_card_val();
}

void AgnosticBarrierSetRuntime::z_slow_path(oopDesc* oop, Thread* thread) {
  AgnosticStoreBarrierBuffer* buffer = AgnosticThreadLocalData::agnostic_store_barrier_buffer(thread);
  ZStoreBarrierBuffer* zbuffer = ZStoreBarrierBuffer::buffer_for_store(false);
  assert(zbuffer != nullptr, "must be");

  // Flush the buffer
  while (buffer->is_empty() == false) {
    AgnosticStoreBarrierEntry* entry = buffer->pop();
    oopDesc* pre_val = entry->_prev;
    oopDesc* ref_addr = entry->_p;
    assert(ref_addr != nullptr, "must be");

    // Push to ZStoreBarrierBuffer only if the store is bad
    if (ZPointer::is_store_bad(static_cast<zpointer>((uintptr_t)pre_val))) {
      zbuffer->add((zpointer *)ref_addr, static_cast<zpointer>((uintptr_t)pre_val));
    }
  }
  
  // Deal with the last item we could not buffer
  ZBarrier::store_barrier_on_heap_oop_field((zpointer *)oop, false);
}

JRT_LEAF(void, AgnosticBarrierSetRuntime::buffer_full(oopDesc* oop))
  // Buffer is full, let's deal with its contents
  Thread* thread = Thread::current();
  assert(thread->is_Java_thread(), "needs to");
  switch (Universe::heap()->kind()) {
  case CollectedHeap::Serial:
  case CollectedHeap::Parallel:
    ct_slow_path(oop, thread);
    break;
  case CollectedHeap::G1:
    g1_slow_path(oop, thread);
    break;
  case CollectedHeap::Z:
    z_slow_path(oop, thread);
    break;
  case CollectedHeap::None:
  case CollectedHeap::Epsilon:
  case CollectedHeap::Shenandoah:
    ShouldNotReachHere();
    break;
  }
JRT_END

address AgnosticBarrierSetRuntime::buffer_full_addr() {
  return reinterpret_cast<address>(buffer_full);
}