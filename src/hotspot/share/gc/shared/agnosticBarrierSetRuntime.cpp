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
#include "gc/g1/g1ThreadLocalData.hpp"
#include "gc/parallel/parallelScavengeHeap.hpp"
#include "gc/parallel/psCardTable.hpp"
#include "gc/serial/cardTableRS.hpp"
#include "gc/serial/serialHeap.hpp"
#include "gc/shared/agnosticStoreBarrierBuffer.hpp"
#include "gc/shared/agnosticThreadLocalData.hpp"
#include "gc/shared/cardTable.hpp"
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
#include <cstdint>
#include <cstdio>

void AgnosticBarrierSetRuntime::g1_slow_path(oopDesc* oop, Thread* thread, oopDesc* n) {
  SATBMarkQueue& queue = G1ThreadLocalData::satb_mark_queue(thread);
  CardTable::CardValue* table = G1ThreadLocalData::byte_map_base(thread);
  AgnosticStoreBarrierBuffer* buffer = AgnosticThreadLocalData::agnostic_store_barrier_buffer(thread);

  while (buffer->is_empty() == false) {
    AgnosticStoreBarrierEntry* entry = buffer->pop();
    oopDesc* pre_val = entry->_prev;
    oopDesc* ref_addr = entry->_p;
    if (ref_addr == nullptr) continue;

    oopDesc* new_val = ref_addr->obj_field_acquire(0);

    if (pre_val != nullptr) G1BarrierSet::satb_mark_queue_set().enqueue_known_active(queue, pre_val);

    printf("ref addr %p\n", (void*)ref_addr);
    printf("prev val %p\n", (void*)pre_val);
    if (new_val == nullptr) continue;
    printf("new val  %p\n", (void*)new_val);
    if (!G1HeapRegion::is_in_same_region(ref_addr, new_val)) {
      CardTable::CardValue* result = &table[uintptr_t(ref_addr) >> CardTable::card_shift()];
      printf("tarjeta  %p\n", (void*)result);
      *result = CardTable::dirty_card_val();
    }
  }

  oopDesc* new_val = n;
  oopDesc* pre_val = oop->obj_field_acquire(0);

  if (pre_val != nullptr) G1BarrierSet::satb_mark_queue_set().enqueue_known_active(queue, pre_val);

  printf("ref addr %p\n", (void*)oop);
  printf("prev val %p\n", (void*)pre_val);
  if (new_val == nullptr) return;
  printf("new_val  %p\n", (void*)new_val);
  if (!G1HeapRegion::is_in_same_region(oop, new_val)) {
    CardTable::CardValue* result = &table[uintptr_t(oop) >> CardTable::card_shift()];
    printf("tarjeta  %p\n", (void*)result);
    *result = CardTable::dirty_card_val();
  }
}

void AgnosticBarrierSetRuntime::ct_slow_path(oopDesc* oop, Thread* thread, oopDesc* n) {
  CardTable::CardValue* table;
  if (Universe::heap()->kind() == CollectedHeap::Serial) {
    table = SerialHeap::heap()->rem_set()->byte_map_base();
    assert((uint64_t)((CardTableBarrierSet*)(BarrierSet::barrier_set()))->card_table()->byte_map_base()==(uint64_t)table, "lolky");
  } else {
    assert(Universe::heap()->kind() == CollectedHeap::Parallel, "must be");
    table = ParallelScavengeHeap::heap()->card_table()->byte_map_base();
  }
  
  AgnosticStoreBarrierBuffer* buffer = AgnosticThreadLocalData::agnostic_store_barrier_buffer(thread);

  while (buffer->is_empty() == false) {
    AgnosticStoreBarrierEntry* entry = buffer->pop();
    oopDesc* pre_val = entry->_prev;
    oopDesc* ref_addr = entry->_p;
    if (ref_addr == nullptr) continue;

    oopDesc* new_val = ref_addr->obj_field_acquire(0);

    printf("ref addr %p\n", (void*)ref_addr);
    printf("prev val %p\n", (void*)pre_val);
    if (new_val == nullptr) continue;
    printf("new val  %p\n", (void*)new_val);
    CardTable::CardValue* result = &table[uintptr_t(ref_addr) >> CardTable::card_shift()];
    printf("tarjeta  %p\n", (void*)result);
    *result = CardTable::dirty_card_val();
  }

  oopDesc* new_val = n;

  if (Universe::is_in_heap(oop) == false) {
    printf("me muero");
  } else {
    ;
  }

  printf("ref addr %p\n", (void*)oop);
  if (new_val == nullptr) return;
  printf("new_val  %p\n", (void*)new_val);
  CardTable::CardValue* result = &table[uintptr_t(oop) >> CardTable::card_shift()];
  printf("tarjeta  %p\n", (void*)result);
  *result = CardTable::dirty_card_val();
}

void AgnosticBarrierSetRuntime::z_slow_path(oopDesc* oop, Thread* thread) {
}

JRT_LEAF(void, AgnosticBarrierSetRuntime::buffer_full(oopDesc* p, oopDesc* n))
  // Buffer is full, let's deal with its contents
  Thread* thread = Thread::current();
  assert(thread->is_Java_thread(), "needs to");
  switch (Universe::heap()->kind()) {
  case CollectedHeap::None:
  case CollectedHeap::Epsilon:
    break;
  case CollectedHeap::Serial:
  case CollectedHeap::Parallel:
    ct_slow_path(p, thread, n);
    break;
  case CollectedHeap::G1:
    g1_slow_path(p, thread, n);
    break;
  case CollectedHeap::Z:
    z_slow_path(p, thread);
    break;
  case CollectedHeap::Shenandoah:
    ShouldNotReachHere();
    break;
  }
  //ZBarrier::store_barrier_on_heap_oop_field((zpointer*)p, true /* heal */);
JRT_END

address AgnosticBarrierSetRuntime::buffer_full_addr() {
  return reinterpret_cast<address>(buffer_full);
}

void AgnosticBarrierSetRuntime::do_magic() {
  printf("super slow\n");
}