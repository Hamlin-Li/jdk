/*
 * Copyright (c) 2021, Huawei Technologies Co. Ltd. All rights reserved.
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

#include "precompiled.hpp"

#include "gc/g1/g1HeapRegionChunk.hpp"
#include "gc/g1/g1ConcurrentMarkBitMap.hpp"
#include "gc/g1/heapRegion.hpp"
#include "gc/shared/markBitMap.inline.hpp"

G1HeapRegionChunk::G1HeapRegionChunk(HeapRegion* region, uint chunk_idx, G1CMBitMap* bitmap) :
  _chunk_size(static_cast<uint>(MIN2(128 * K, G1HeapRegionSize))),
  _region(region),
  _chunk_idx(chunk_idx),
  _bitmap(bitmap) {
  HeapWord* top = _region->top();
  HeapWord* bottom = _region->bottom();
  _start = MIN2(top, bottom + _chunk_idx * _chunk_size);
  _limit = MIN2(top, bottom + (_chunk_idx + 1) * _chunk_size);
  _first_obj_in_chunk = _bitmap->get_next_marked_addr(_start, _limit);
  _next_obj_in_region = _bitmap->get_next_marked_addr(_limit, top);
  // there is marked obj in this chunk
  bool marked_obj_in_this_chunk = _start <= _first_obj_in_chunk && _first_obj_in_chunk < _limit;
  _include_first_obj_in_region = marked_obj_in_this_chunk
                                 && _bitmap->get_next_marked_addr(bottom, _limit) >= _start;
  _include_last_obj_in_region = marked_obj_in_this_chunk
                                && _bitmap->get_next_marked_addr(_limit, top) == top;
}

G1HeapRegionChunkClaimer::G1HeapRegionChunkClaimer(uint region_idx) :
  _chunk_size(static_cast<uint>(MIN2(128 * K, G1HeapRegionSize))),
  _chunk_num(static_cast<uint>(G1HeapRegionSize) / _chunk_size),
  _region_idx(region_idx),
  _chunks(mtGC) {
  _chunks.resize(_chunk_num);
}

bool G1HeapRegionChunkClaimer::claim_chunk(uint chunk_idx) {
  return _chunks.par_set_bit(chunk_idx);
}
