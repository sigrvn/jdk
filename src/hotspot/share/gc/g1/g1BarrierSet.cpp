/*
 * Copyright (c) 2001, 2023, Oracle and/or its affiliates. All rights reserved.
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

#include "gc/shared/c2/agnosticBarrierSetC2.hpp"
#include "precompiled.hpp"
#include "gc/g1/g1BarrierSet.inline.hpp"
#include "gc/g1/g1BarrierSetAssembler.hpp"
#include "gc/g1/g1CardTable.inline.hpp"
#include "gc/g1/g1CollectedHeap.inline.hpp"
#include "gc/g1/g1HeapRegion.hpp"
#include "gc/g1/g1RegionPinCache.inline.hpp"
#include "gc/g1/g1SATBMarkQueueSet.hpp"
#include "gc/g1/g1ThreadLocalData.hpp"
#include "gc/shared/satbMarkQueue.hpp"
#include "logging/log.hpp"
#include "memory/iterator.hpp"
#include "oops/access.inline.hpp"
#include "oops/compressedOops.inline.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/interfaceSupport.inline.hpp"
#include "runtime/javaThread.hpp"
#include "runtime/orderAccess.hpp"
#include "runtime/threads.hpp"
#include "utilities/macros.hpp"
#ifdef COMPILER1
#include "gc/g1/c1/g1BarrierSetC1.hpp"
#endif
#ifdef COMPILER2
#include "gc/g1/c2/g1BarrierSetC2.hpp"
#endif

class G1BarrierSetC1;
class G1BarrierSetC2;

G1BarrierSet::G1BarrierSet(G1CardTable* card_table,
                           G1CardTable* refinement_table) :
  CardTableBarrierSet(make_barrier_set_assembler<G1BarrierSetAssembler>(),
                      make_barrier_set_c1<G1BarrierSetC1>(),
                      make_barrier_set_c2</*G1BarrierSetC2*/AgnosticBarrierSetC2>(),
                      card_table,
                      BarrierSet::FakeRtti(BarrierSet::G1BarrierSet)),
  _satb_mark_queue_buffer_allocator("SATB Buffer Allocator", G1SATBBufferSize),
  _satb_mark_queue_set(&_satb_mark_queue_buffer_allocator),
  _refinement_table(refinement_table)
{}

G1BarrierSet::~G1BarrierSet() {
  delete _refinement_table;
}

void G1BarrierSet::swap_global_card_table() {
  class DoThings : public ThreadClosure {
    SATBMarkQueueSet* _qset;
    bool _active;
  public:
    DoThings(bool active) {}
    virtual void do_thread(Thread* t) {
      AgnosticStoreBarrierBuffer* buffer = AgnosticThreadLocalData::agnostic_store_barrier_buffer(t);
      CardTable::CardValue* table = G1ThreadLocalData::byte_map_base(t);
      SATBMarkQueueSet& satb_mq_set = G1BarrierSet::satb_mark_queue_set();
      _qset = &satb_mq_set;
      SATBMarkQueue& queue = _qset->satb_queue_for_thread(t);

      while (buffer->is_empty() == false) {
        AgnosticStoreBarrierEntry* entry = buffer->pop();
        oopDesc* pre_val = entry->_prev;
        oopDesc* ref_addr = entry->_p;
        assert(ref_addr != nullptr, "must be");
    
        //oopDesc* new_val = Atomic::load(ref_addr);
    
        if (pre_val != nullptr && queue.is_active()) G1BarrierSet::satb_mark_queue_set().enqueue_known_active(queue, pre_val);
    
        printf("ref addr %p\n", (void*)ref_addr);
        printf("prev val %p\n", (void*)pre_val);
        //if (new_val == nullptr) continue;
        //printf("new val  %p\n", (void*)new_val);
        //if (!G1HeapRegion::is_in_same_region(ref_addr, new_val)) {
          CardTable::CardValue* result = &table[uintptr_t(ref_addr) >> CardTable::card_shift()];
          printf("tarjeta  %p\n", (void*)result);
          *result = CardTable::dirty_card_val();
        //}
      }
    }
  } closure(true);
  if (Threads_lock->owner() == Thread::current()) {
    Threads::threads_do(&closure);
  } else {
    assert(false, "couldn't flush");
  }

  G1CardTable* temp = static_cast<G1CardTable*>(_card_table);
  _card_table = _refinement_table;
  _refinement_table = temp;
}

template <class T> void
G1BarrierSet::write_ref_array_pre_work(T* dst, size_t count) {
  G1SATBMarkQueueSet& queue_set = G1BarrierSet::satb_mark_queue_set();
  if (!queue_set.is_active()) return;

  SATBMarkQueue& queue = G1ThreadLocalData::satb_mark_queue(Thread::current());

  T* elem_ptr = dst;
  for (size_t i = 0; i < count; i++, elem_ptr++) {
    T heap_oop = RawAccess<>::oop_load(elem_ptr);
    if (!CompressedOops::is_null(heap_oop)) {
      queue_set.enqueue_known_active(queue, CompressedOops::decode_not_null(heap_oop));
    }
  }
}

void G1BarrierSet::write_ref_array_pre(oop* dst, size_t count, bool dest_uninitialized) {
  if (!dest_uninitialized) {
    write_ref_array_pre_work(dst, count);
  }
}

void G1BarrierSet::write_ref_array_pre(narrowOop* dst, size_t count, bool dest_uninitialized) {
  if (!dest_uninitialized) {
    write_ref_array_pre_work(dst, count);
  }
}

void G1BarrierSet::write_region(JavaThread* thread, MemRegion mr) {
  if (mr.is_empty()) {
    return;
  }

  // Skip writes to young gen.
  if (G1CollectedHeap::heap()->heap_region_containing(mr.start())->is_young()) {
    // MemRegion should not span multiple regions for the young gen.
    DEBUG_ONLY(G1HeapRegion* containing_hr = G1CollectedHeap::heap()->heap_region_containing(mr.start());)
    assert(containing_hr->is_young(), "it should be young");
    assert(containing_hr->is_in(mr.start()), "it should contain start");
    assert(containing_hr->is_in(mr.last()), "it should also contain last");
    return;
  }

  volatile CardValue* byte = _card_table->byte_for(mr.start());
  CardValue* last_byte = _card_table->byte_for(mr.last());

  // Dirty cards if necessary.
  for (; byte <= last_byte; byte++) {
    CardValue bv = *byte;
    if (bv != G1CardTable::dirty_card_val()) {
      *byte = G1CardTable::dirty_card_val();
    }
  }
}

void G1BarrierSet::on_thread_create(Thread* thread) {
  // Create thread local data
  G1ThreadLocalData::create(thread);
}

void G1BarrierSet::on_thread_destroy(Thread* thread) {
  // Destroy thread local data
  G1ThreadLocalData::destroy(thread);
}

void G1BarrierSet::on_thread_attach(Thread* thread) {
  BarrierSet::on_thread_attach(thread);
  SATBMarkQueue& satbq = G1ThreadLocalData::satb_mark_queue(thread);
  assert(!satbq.is_active(), "SATB queue should not be active");
  assert(satbq.buffer() == nullptr, "SATB queue should not have a buffer");
  assert(satbq.index() == 0, "SATB queue index should be zero");
  // If we are creating the thread during a marking cycle, we should
  // set the active field of the SATB queue to true.  That involves
  // copying the global is_active value to this thread's queue.
  satbq.set_active(_satb_mark_queue_set.is_active());

  // Between creation and attaching there may be a safepoint (and/or the thread
  // is not visible to the handshake), so (re-)do the card table base address
  // assignment here.
  G1ThreadLocalData::set_byte_map_base(thread, G1CollectedHeap::heap()->card_table_base());
}

void G1BarrierSet::on_thread_detach(Thread* thread) {
  // Flush any deferred card marks.
  CardTableBarrierSet::on_thread_detach(thread);
  {
    SATBMarkQueue& queue = G1ThreadLocalData::satb_mark_queue(thread);
    G1BarrierSet::satb_mark_queue_set().flush_queue(queue);
  }
  {
    G1RegionPinCache& cache = G1ThreadLocalData::pin_count_cache(thread);
    cache.flush();
  }
}

void G1BarrierSet::print_on(outputStream* st) const {
  _card_table->print_on(st, "Card");
  _refinement_table->print_on(st, "Refinement");
}
