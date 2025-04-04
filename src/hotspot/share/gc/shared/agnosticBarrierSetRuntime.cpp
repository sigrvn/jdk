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
#include "utilities/debug.hpp"
#include <cstdint>
#include <cstdio>

void AgnosticBarrierSetRuntime::g1_slow_path(oopDesc* oop, JavaThread* thread, oopDesc* n) {
  //printf("!!!!!!!!!! %lx\n", (long)n);
  SATBMarkQueue& queue = G1ThreadLocalData::satb_mark_queue(thread);
  CardTable::CardValue* table = G1ThreadLocalData::byte_map_base(thread);
  AgnosticStoreBarrierBuffer* buffer = AgnosticThreadLocalData::agnostic_store_barrier_buffer(thread);
  while (buffer->is_empty() == false) {
    AgnosticStoreBarrierEntry* entry = buffer->pop();
    oopDesc* lol = entry->_prev;
    oopDesc* ref_addr = entry->_p;
    if (ref_addr == nullptr) continue;
    // printf("pre %lx\n", (long)lol);
    // printf("ref %lx\n", (long)ref_addr);
    oopDesc* val = ref_addr->obj_field_acquire(0);
    // printf("loa %lx\n", (long)val);
    if (lol != nullptr) G1BarrierSet::satb_mark_queue_set().enqueue_known_active(queue, lol);
    if (val == nullptr) continue;
    if (!G1HeapRegion::is_in_same_region(ref_addr, val)) {
      CardTable::CardValue* result = &table[uintptr_t(ref_addr) >> CardTable::card_shift()];
      //printf("aaaaa %p\n", (void*)result);
      *result = CardTable::dirty_card_val();
    }
  }

  // printf("oop %lx\n", (long)oop);
  oopDesc* val = n;//oop->obj_field_acquire(0);
  oopDesc* prev = oop->obj_field_acquire(0);
  // printf("val %lx\n", (long)val);
  if (prev != nullptr) G1BarrierSet::satb_mark_queue_set().enqueue_known_active(queue, prev);
  if (val == nullptr) return;
  if (!G1HeapRegion::is_in_same_region(oop, val)) {
    CardTable::CardValue* result = &table[uintptr_t(oop) >> CardTable::card_shift()];
    printf("tarta %p\n", (void*)result);
    printf("sarta %p\n", (void*)oop);
    printf("marta %p\n", (void*)val);
    *result = CardTable::dirty_card_val();
  }
}

void AgnosticBarrierSetRuntime::g1_slow_path_post(oopDesc* oop, JavaThread* thread) {
  CardTable::CardValue* table = G1ThreadLocalData::byte_map_base(thread);
  AgnosticStoreBarrierBuffer* buffer = AgnosticThreadLocalData::agnostic_store_barrier_buffer(thread);
  while (buffer->is_empty() == false) {
    oopDesc* ref_addr = buffer->pop()->_p;
    if (ref_addr == nullptr) continue;
    printf("post %lx\n", (long)ref_addr);
    oopDesc* val = ref_addr->obj_field_acquire(0);
    printf("pest %lx\n", (long)val);
    if (G1HeapRegion::is_in_same_region(ref_addr, val)) {
      if (val != nullptr) {
        CardTable::CardValue* result = &table[uintptr_t(ref_addr) >> CardTable::card_shift()];
        printf("tarta %lx\n", (long)result);
        *result = CardTable::dirty_card_val();
      }
    }
  }

  if (oop == nullptr) return;
  printf("post %lx\n", (long)oop);
  oopDesc* val = oop->obj_field_acquire(0);
  printf("pest %lx\n", (long)val);
  if (G1HeapRegion::is_in_same_region(oop, val)) {
    if (val != nullptr) {
      CardTable::CardValue* result = &table[uintptr_t(oop) >> CardTable::card_shift()];
      printf("tarta %lx\n", (long)result);
      *result = CardTable::dirty_card_val();
    }
  }
}

void AgnosticBarrierSetRuntime::ct_slow_path(oopDesc* oop, JavaThread* thread) {
}

void AgnosticBarrierSetRuntime::z_slow_path(oopDesc* oop, JavaThread* thread) {
}

JRT_LEAF(void, AgnosticBarrierSetRuntime::buffer_full(oopDesc* p, JavaThread* thread, oopDesc* n, oopDesc* carta))
  printf("carta %p\n", (void*)carta);
  // Buffer is full, let's deal with its contents
  switch (Universe::heap()->kind()) {
  case CollectedHeap::None:
  case CollectedHeap::Epsilon:
    break;
  case CollectedHeap::Serial:
  case CollectedHeap::Parallel:
    ct_slow_path(p, thread);
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