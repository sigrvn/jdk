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

#ifndef SHARE_GC_SHARED_AGNOSTICBARRIERSETRUNTIME_HPP
#define SHARE_GC_SHARED_AGNOSTICBARRIERSETRUNTIME_HPP

#include "gc/g1/g1BarrierSet.hpp"
#include "gc/parallel/psCardTable.hpp"
#include "gc/serial/cardTableRS.hpp"
#include "gc/z/zAddress.hpp"
#include "gc/shared/agnosticStoreBarrierBuffer.hpp"
#include "gc/shared/agnosticThreadLocalData.hpp"
#include "gc/g1/g1ThreadLocalData.hpp"
#include "gc/serial/serialHeap.hpp"
#include "gc/parallel/parallelScavengeHeap.hpp"
#include "gc/z/zStoreBarrierBuffer.hpp"
#include "gc/z/zStoreBarrierBuffer.inline.hpp"
#include "gc/z/zThreadLocalAllocBuffer.hpp"
#include "gc/z/zThreadLocalData.hpp"
#include "memory/allStatic.hpp"
#include "memory/universe.hpp"
#include "oops/accessDecorators.hpp"
#include "oops/oop.hpp"
#include "oops/oopsHierarchy.hpp"
#include "runtime/javaThread.hpp"
#include "runtime/threads.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"

class AgnosticBarrierSetRuntime : public AllStatic {
private:
  static void buffer_full(oopDesc* oop);
  static void g1_slow_path(oopDesc* oop, Thread* thread);
  static void ct_slow_path(oopDesc* oop, Thread* thread);
  static void z_slow_path(oopDesc* oop, Thread* thread);
public:
  static address buffer_full_addr();
  static void do_magic();
};

class G1AgnosticBarrierSetFlush : public ThreadClosure {
public:
  virtual void do_thread(Thread* thread) {
    assert(Universe::heap()->kind() == CollectedHeap::G1, "must be");
    AgnosticStoreBarrierBuffer* buffer = AgnosticThreadLocalData::agnostic_store_barrier_buffer(thread);
    CardTable::CardValue* table = G1ThreadLocalData::byte_map_base(thread);
    SATBMarkQueueSet* _qset = &G1BarrierSet::satb_mark_queue_set();
    SATBMarkQueue& queue = _qset->satb_queue_for_thread(thread);

    while (buffer->is_empty() == false) {
      AgnosticStoreBarrierEntry* entry = buffer->pop();
      oopDesc* pre_val = entry->_prev;
      oopDesc* ref_addr = entry->_p;
      assert(ref_addr != nullptr, "must be");

      if (pre_val != nullptr && queue.is_active()) {
        G1BarrierSet::satb_mark_queue_set().enqueue_known_active(queue, pre_val);
      }
  
      G1CardTable::CardValue* result = &table[uintptr_t(ref_addr) >> G1CardTable::card_shift()];
      *result = G1CardTable::dirty_card_val();
    }
  }
};

class CardTableAgnosticBarrierSetFlush : public ThreadClosure {
public:
  virtual void do_thread(Thread* thread) {
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
  }
};

class ZAgnosticBarrierSetFlush : public ThreadClosure {
public:
  virtual void do_thread(Thread* thread) {
    assert(Universe::heap()->kind() == CollectedHeap::Z, "must be");
    ZStoreBarrierBuffer* zbuffer = ZThreadLocalData::store_barrier_buffer(thread);
    AgnosticStoreBarrierBuffer* buffer = AgnosticThreadLocalData::agnostic_store_barrier_buffer(thread);

    // Flush the buffer
    while (buffer->is_empty() == false) {
      AgnosticStoreBarrierEntry* entry = buffer->pop();
      oopDesc* pre_val = entry->_prev;
      oopDesc* ref_addr = entry->_p;
      assert(ref_addr != nullptr, "must be");

      if (ZPointer::is_store_bad(static_cast<zpointer>((uintptr_t)pre_val))) {
        zbuffer->add((zpointer *)ref_addr, static_cast<zpointer>((uintptr_t)pre_val));
      }
    }
  }
};

class AgnosticBarrierSetFlush : public ThreadClosure {
public:
  virtual void do_thread(Thread* thread) {
    // There's nothing to do if we are not using agnostic barriers
    if (GCASB == false) return;

    // Le funk!
    switch (Universe::heap()->kind()) {
    case CollectedHeap::Serial:
    case CollectedHeap::Parallel:
      {
        CardTableAgnosticBarrierSetFlush closure;
        closure.do_thread(thread);
      }
      break;
    case CollectedHeap::G1:
      {
        G1AgnosticBarrierSetFlush closure;
        closure.do_thread(thread);
      }
      break;
    case CollectedHeap::Z:
    {
      ZAgnosticBarrierSetFlush closure;
      closure.do_thread(thread);
    }
      break;
    case CollectedHeap::None:
    case CollectedHeap::Epsilon:
    case CollectedHeap::Shenandoah:
      ShouldNotReachHere();
      break;
    }
  }
};

#endif // SHARE_GC_SHARED_AGNOSTICBARRIERSETRUNTIME_HPP
