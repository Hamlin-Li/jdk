/*
 * Copyright (c) 2019, 2021, Oracle and/or its affiliates. All rights reserved.
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
#include "gc/g1/g1CollectionSetCandidates.hpp"
#include "gc/g1/g1CollectionSetChooser.hpp"
#include "gc/g1/g1_globals.hpp"
#include "gc/g1/heapRegion.inline.hpp"

void G1CollectionSetCandidates::append_evac_failure_region(HeapRegion* hr) {
  assert(_regions != NULL, "must be");
  assert(hr != NULL, "must be");
  assert(_capacity >= _num_regions, "must be, %u, %u", _capacity, _num_regions);
  _regions[_num_regions++] = hr;
  _remaining_reclaimable_bytes += hr->reclaimable_bytes();
}

uint G1CollectionSetCandidates::calc_capacity(uint num_regions) {
  return num_regions + num_regions / G1EvacuationFailureRegionRatioAddedIntoCSet;
}

uint G1CollectionSetCandidates::defult_per_cycle_moved_candidates() {
  return _initial_candidates_num /
        (G1MaxTimesAddEvacuationFailureRegionToCSet * G1EvacuationFailureRegionRatioAddedIntoCSet);
}

void G1CollectionSetCandidates::remove(uint num_regions) {
  assert(num_regions <= num_remaining(), "Trying to remove more regions (%u) than available (%u)", num_regions, num_remaining());
  for (uint i = 0; i < num_regions; i++) {
    _remaining_reclaimable_bytes -= at(_front_idx)->reclaimable_bytes();
    _front_idx++;
  }
}

void G1CollectionSetCandidates::remove_from_end(uint num_remove, size_t wasted) {
  assert(num_remove <= num_remaining(), "trying to remove more regions than remaining");

#ifdef ASSERT
  size_t reclaimable = 0;

  for (uint i = 0; i < num_remove; i++) {
    uint cur_idx = _num_regions - i - 1;
    reclaimable += at(cur_idx)->reclaimable_bytes();
    // Make sure we crash if we access it.
    _regions[cur_idx] = NULL;
  }

  assert(reclaimable == wasted, "Recalculated reclaimable inconsistent");
#endif
  _num_regions -= num_remove;
  _initial_candidates_num = _num_regions;
  _remaining_reclaimable_bytes -= wasted;
}

void G1CollectionSetCandidates::iterate(HeapRegionClosure* cl) {
  for (uint i = _front_idx; i < _num_regions; i++) {
    HeapRegion* r = _regions[i];
    if (cl->do_heap_region(r)) {
      cl->set_incomplete();
      break;
    }
  }
}

void G1CollectionSetCandidates::iterate_backwards(HeapRegionClosure* cl) {
  for (uint i = _num_regions; i > _front_idx; i--) {
    HeapRegion* r = _regions[i - 1];
    if (cl->do_heap_region(r)) {
      cl->set_incomplete();
      break;
    }
  }
}

#ifndef PRODUCT
void G1CollectionSetCandidates::verify() const {
  guarantee(_front_idx <= _num_regions, "Index: %u Num_regions: %u", _front_idx, _num_regions);
  size_t sum_of_reclaimable_bytes = 0;
  uint idx_and_limit[2][2] = {
    {_front_idx, _initial_candidates_num},
    {_front_idx > _initial_candidates_num ? _front_idx : _initial_candidates_num, _num_regions}
  };
  for (int i = 0; i < 2; i++) {
    uint idx = idx_and_limit[i][0];
    uint limit = idx_and_limit[i][1];
    HeapRegion *prev = NULL;
    for (; idx < limit; idx++) {
      HeapRegion *cur = _regions[idx];
      guarantee(cur != NULL, "Regions after _front_idx %u cannot be NULL but %u is", _front_idx, idx);
      // The first disjunction filters out regions with objects that were explicitly
      // pinned after being added to the collection set candidates. Archive regions
      // should never have been added to the collection set though.
      guarantee((cur->is_pinned() && !cur->is_archive()) ||
                G1CollectionSetChooser::should_add(cur),
                "Region %u (at %u) should be eligible for addition. %s, %s, %s, %s",
                cur->hrm_index(), idx,
                BOOL_TO_STR(!cur->is_young()),
                BOOL_TO_STR(!cur->is_pinned()),
                BOOL_TO_STR(G1CollectionSetChooser::region_occupancy_low_enough_for_evac(cur->live_bytes())),
                BOOL_TO_STR(cur->rem_set()->is_complete()));
      if (i == 0 /* TODO: FIXME?? necessary?? */ && prev != NULL) {
        guarantee(prev->gc_efficiency() >= cur->gc_efficiency(),
                  "GC efficiency for region %u: %1.4f smaller than for region %u: %1.4f",
                  prev->hrm_index(), prev->gc_efficiency(), cur->hrm_index(), cur->gc_efficiency());
      }
      sum_of_reclaimable_bytes += cur->reclaimable_bytes();
      prev = cur;
    }
  }
  guarantee(sum_of_reclaimable_bytes == _remaining_reclaimable_bytes,
            "Inconsistent remaining_reclaimable bytes, remaining " SIZE_FORMAT " calculated " SIZE_FORMAT,
            _remaining_reclaimable_bytes, sum_of_reclaimable_bytes);
}
#endif // !PRODUCT
