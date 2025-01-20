/*
 * Copyright (c) 2024, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This barrier is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This barrier is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this barrier).
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

#ifndef SHARE_GC_FRANKENSTEIN_FRANKENSTEINBARRIERSET_HPP
#define SHARE_GC_FRANKENSTEIN_FRANKENSTEINBARRIERSET_HPP

#include "gc/g1/g1SATBMarkQueueSet.hpp"
#include "gc/shared/cardTable.hpp"
#include "gc/shared/cardTableBarrierSet.hpp"
#include "gc/shared/bufferNode.hpp"

class G1CardTable;
class PSCardTable;
class CardTableRS;

// This experimental barrier is an amalgamation of all currently-supported GC barriers.
// FrankensteinBarrierSet is built off of the G1BarrierSet class.

class FrankensteinBarrierSet: public CardTableBarrierSet {
  friend class VMStructs;

 private:
  // G1 members
  BufferNode::Allocator _satb_mark_queue_buffer_allocator;
  G1SATBMarkQueueSet _satb_mark_queue_set;
  G1CardTable* _refinement_table;

  // Common members
  BarrierSet::Name _barrier_in_use;

 public:
  FrankensteinBarrierSet(G1CardTable* card_table, G1CardTable* refinement_table); // G1
  FrankensteinBarrierSet(PSCardTable* card_table); // Parallel
  // FrankensteinBarrierSet(CardTableRS* rem_set); // Serial 
  virtual ~FrankensteinBarrierSet();

  template <typename BarrierSetT>
  static BarrierSetT* as_barrier_set() {
    return barrier_set_cast<BarrierSetT>(BarrierSet::barrier_set());
  }

  // G1 barrier start
  G1CardTable* refinement_table() const {
      assert(_barrier_in_use == BarrierSet::G1BarrierSet, 
              "G1BarrierSet is not in use!");
      return _refinement_table;
  }

  // Swap the global card table references, without synchronization.
  void swap_global_card_table();

  virtual bool card_mark_must_follow_store() const { return true; }

  // Add "pre_val" to a set of objects that may have been disconnected from the
  // pre-marking object graph. Prefer the version that takes location, as it
  // can avoid touching the heap unnecessarily.
  template <class T> static void enqueue(T* dst);
  static void enqueue_preloaded(oop pre_val);

  static void enqueue_preloaded_if_weak(DecoratorSet decorators, oop value);

  template <class T> void write_ref_array_pre_work(T* dst, size_t count);
  virtual void write_ref_array_pre(oop* dst, size_t count, bool dest_uninitialized);
  virtual void write_ref_array_pre(narrowOop* dst, size_t count, bool dest_uninitialized);

  template <DecoratorSet decorators, typename T>
  void write_ref_field_pre(T* field);

  inline void write_region(MemRegion mr);
  void write_region(JavaThread* thread, MemRegion mr);

  template <DecoratorSet decorators = DECORATORS_NONE, typename T>
  void write_ref_field_post(T* field);

  virtual void on_thread_create(Thread* thread);
  virtual void on_thread_destroy(Thread* thread);
  virtual void on_thread_attach(Thread* thread);
  virtual void on_thread_detach(Thread* thread);

  static G1SATBMarkQueueSet& satb_mark_queue_set() {
    return as_barrier_set<FrankensteinBarrierSet>()->_satb_mark_queue_set;
  }

  virtual void print_on(outputStream* st) const;

  // Callbacks for runtime accesses.
  template <DecoratorSet decorators, typename BarrierSetT = FrankensteinBarrierSet>
  class AccessBarrier: public ModRefBarrierSet::AccessBarrier<decorators, BarrierSetT> {
    typedef ModRefBarrierSet::AccessBarrier<decorators, BarrierSetT> ModRef;
    typedef BarrierSet::AccessBarrier<decorators, BarrierSetT> Raw;

  public:
    // Needed for loads on non-heap weak references
    template <typename T>
    static oop oop_load_not_in_heap(T* addr);

    // Needed for non-heap stores
    template <typename T>
    static void oop_store_not_in_heap(T* addr, oop new_value);

    // Needed for weak references
    static oop oop_load_in_heap_at(oop base, ptrdiff_t offset);

    // Defensive: will catch weak oops at addresses in heap
    template <typename T>
    static oop oop_load_in_heap(T* addr);
  };
  // G1 barrier end
};

template<>
struct BarrierSet::GetName<FrankensteinBarrierSet> {
  static const BarrierSet::Name value = BarrierSet::FrankensteinBarrierSet;
};

template<>
struct BarrierSet::GetType<BarrierSet::FrankensteinBarrierSet> {
  typedef ::FrankensteinBarrierSet type;
};

#endif // SHARE_GC_FRANKENSTEIN_FRANKENSTEINBARRIERSET_HPP
