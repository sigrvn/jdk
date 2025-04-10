/*
 * Copyright (c) 2021, 2024, Oracle and/or its affiliates. All rights reserved.
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

#include "gc/shared/agnosticStoreBarrierBuffer.hpp"
#include "precompiled.hpp"
#include "gc/shared/gc_globals.hpp"
#include "gc/z/zAddress.inline.hpp"
#include "gc/z/zBarrier.inline.hpp"
#include "gc/z/zGeneration.inline.hpp"
#include "gc/shared/agnosticStoreBarrierBuffer.inline.hpp"
#include "gc/z/zUncoloredRoot.inline.hpp"
#include "memory/resourceArea.hpp"
#include "runtime/threadSMR.hpp"
#include "utilities/ostream.hpp"
#include "utilities/vmError.hpp"

ByteSize AgnosticStoreBarrierEntry::p_offset() {
  return byte_offset_of(AgnosticStoreBarrierEntry, _p);
}

ByteSize AgnosticStoreBarrierEntry::prev_offset() {
  return byte_offset_of(AgnosticStoreBarrierEntry, _prev);
}

ByteSize AgnosticStoreBarrierBuffer::buffer_offset() {
  return byte_offset_of(AgnosticStoreBarrierBuffer, _buffer);
}

ByteSize AgnosticStoreBarrierBuffer::current_offset() {
  return byte_offset_of(AgnosticStoreBarrierBuffer, _current);
}

AgnosticStoreBarrierBuffer::AgnosticStoreBarrierBuffer()
  : _buffer(),
    _last_processed_color(),
    _last_installed_color(),
    _base_pointer_lock(),
    _base_pointers(),
    _current(ZBufferStoreBarriers ? BufferSizeBytes : 0) {}

void AgnosticStoreBarrierBuffer::initialize() {
  _last_processed_color = ZPointerStoreGoodMask;
  _last_installed_color = ZPointerStoreGoodMask;
}

void AgnosticStoreBarrierBuffer::clear() {
  _current = BufferSizeBytes;
}

bool AgnosticStoreBarrierBuffer::is_empty() const {
  return _current == BufferSizeBytes;
}

AgnosticStoreBarrierEntry* AgnosticStoreBarrierBuffer::pop() {
  _current += sizeof(AgnosticStoreBarrierEntry);
  return &_buffer[current() - 1];
}

void AgnosticStoreBarrierBuffer::flush() {
  if (!ZBufferStoreBarriers) {
    return;
  }

  for (size_t i = current(); i < BufferLength; ++i) {
    const AgnosticStoreBarrierEntry& entry = _buffer[i];
    //const zaddress addr = ZBarrier::make_load_good(entry._prev);
    //ZBarrier::mark_and_remember(entry._p, addr);
  }

  clear();
}

bool AgnosticStoreBarrierBuffer::is_in(volatile zpointer* p) {
  if (!ZBufferStoreBarriers) {
    return false;
  }

  // for (JavaThreadIteratorWithHandle jtiwh; JavaThread * const jt = jtiwh.next(); ) {
  //   AgnosticStoreBarrierBuffer* const buffer = AgnosticThreadLocalData::store_barrier_buffer(jt);

  //   const uintptr_t  last_remap_bits = ZPointer::remap_bits(buffer->_last_processed_color) & ZPointerRemappedMask;
  //   const bool needs_remap = last_remap_bits != ZPointerRemapped;

  //   for (size_t i = buffer->current(); i < BufferLength; ++i) {
  //     const ZStoreBarrierEntry& entry = buffer->_buffer[i];
  //     volatile zpointer* entry_p = entry._p;

  //     // Potentially remap p
  //     if (needs_remap) {
  //       const zaddress_unsafe entry_p_base = buffer->_base_pointers[i];
  //       if (!is_null(entry_p_base)) {
  //         entry_p = make_load_good(entry_p, entry_p_base, buffer->_last_processed_color);
  //       }
  //     }

  //     // Check if p matches
  //     if (entry_p == p) {
  //       return true;
  //     }
  //   }
  // }

  return false;
}
