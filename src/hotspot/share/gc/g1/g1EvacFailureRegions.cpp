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

#include "gc/g1/g1CollectedHeap.hpp"
#include "gc/g1/g1EvacFailureRegions.inline.hpp"
#include "gc/g1/g1HeapRegionChunk.hpp"
#include "gc/g1/heapRegion.hpp"
#include "gc/shared/markBitMap.inline.hpp"
#include "memory/allocation.hpp"
#include "runtime/atomic.hpp"

G1EvacFailureRegions::G1EvacFailureRegions() :
  _regions_failed_evacuation(mtGC),
  _evac_failure_regions(nullptr),
  _live_words_in_evac_failure_regions(nullptr),
  _chunk_claimers(nullptr),
  _evac_failure_regions_cur_length(0),
  _max_regions(0) { }

G1EvacFailureRegions::~G1EvacFailureRegions() {
  assert(_evac_failure_regions == nullptr, "not cleaned up");
  assert(_live_words_in_evac_failure_regions == nullptr, "not cleaned up");
  assert(_chunk_claimers == nullptr, "not cleaned up");
}

void G1EvacFailureRegions::pre_collection(uint max_regions) {
  Atomic::store(&_evac_failure_regions_cur_length, 0u);
  _max_regions = max_regions;
  _regions_failed_evacuation.resize(_max_regions);
  _evac_failure_regions = NEW_C_HEAP_ARRAY(uint, _max_regions, mtGC);
  _live_words_in_evac_failure_regions = NEW_C_HEAP_ARRAY(uint, _max_regions, mtGC);
  _chunk_claimers = NEW_C_HEAP_ARRAY(G1HeapRegionChunkClaimer*, _max_regions, mtGC);
  memset(_live_words_in_evac_failure_regions, 0, sizeof(uint) * _max_regions);
}

void G1EvacFailureRegions::post_collection() {
  _regions_failed_evacuation.resize(0);

  for (uint i = 0; i < _evac_failure_regions_cur_length; i++) {
    FREE_C_HEAP_OBJ(_chunk_claimers[_evac_failure_regions[i]]);
  }
  FREE_C_HEAP_ARRAY(uint, _chunk_claimers);
  _chunk_claimers = nullptr;

  FREE_C_HEAP_ARRAY(uint, _evac_failure_regions);
  _evac_failure_regions = nullptr;
  FREE_C_HEAP_ARRAY(uint, _live_words_in_evac_failure_regions);
  _live_words_in_evac_failure_regions = nullptr;
  _max_regions = 0; // To have any record() attempt fail in the future.
}

void G1EvacFailureRegions::par_iterate(HeapRegionClosure* closure,
                                       HeapRegionClaimer* _hrclaimer,
                                       uint worker_id) const {
  G1CollectedHeap::heap()->par_iterate_regions_array(closure,
                                                     _hrclaimer,
                                                     _evac_failure_regions,
                                                     Atomic::load(&_evac_failure_regions_cur_length),
                                                     worker_id);
}

class HeapRegionChunksClosure : public HeapRegionClosure {
  G1HeapRegionChunkClaimer** _chunk_claimers;
  G1HeapRegionChunkClosure* _closure;

public:
  HeapRegionChunksClosure(G1HeapRegionChunkClaimer** chunk_claimers,
                          G1HeapRegionChunkClosure* closure) :
    _chunk_claimers(chunk_claimers),
    _closure(closure) {
  }

  bool do_heap_region(HeapRegion* r) override {
    G1HeapRegionChunkClaimer* claimer = _chunk_claimers[r->hrm_index()];
    for (uint i = 0; i < claimer->chunk_num(); i++) {
      if (claimer->claim_chunk(i)) {
        G1HeapRegionChunk chunk(r, i, const_cast<G1CMBitMap*>(G1CollectedHeap::heap()->concurrent_mark()->prev_mark_bitmap()));
        if (chunk.empty()) {
          continue;
        }
        _closure->do_heap_region_chunk(&chunk);
      }
    }
    return false;
  }
};

void G1EvacFailureRegions::par_iterate_chunks(G1HeapRegionChunkClosure* chunk_closure,
                                              HeapRegionClaimer* _hrclaimer,
                                              uint worker_id) const {
  HeapRegionChunksClosure closure(_chunk_claimers, chunk_closure);

  G1CollectedHeap::heap()->par_iterate_regions_array(&closure,
                                                     _hrclaimer,
                                                     _evac_failure_regions,
                                                     Atomic::load(&_evac_failure_regions_cur_length),
                                                     worker_id);
}

bool G1EvacFailureRegions::contains(uint region_idx) const {
  assert(region_idx < _max_regions, "must be");
  return _regions_failed_evacuation.par_at(region_idx, memory_order_relaxed);
}
