/*
 * Copyright (c) 2021, Oracle and/or its affiliates. All rights reserved.
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
#include "gc/g1/g1ConcurrentMarkThread.inline.hpp"
#include "gc/g1/g1CollectedHeap.hpp"
#include "gc/g1/g1CollectorState.hpp"
#include "gc/g1/g1GCPauseType.hpp"
#include "gc/g1/g1Policy.hpp"
#include "gc/shared/concurrentGCBreakpoints.hpp"


#define state_must_be(state_assert) \
  assert((state_assert), "Must be, %s", to_string(_state))

G1GCPauseType G1CollectorState::young_gc_pause_type(bool concurrent_operation_is_full_mark) const {
  assert(!in_full_gc(), "must be");
  if (in_concurrent_start_gc()) {
    assert(!in_young_gc_before_mixed(), "must be");
    return concurrent_operation_is_full_mark ? G1GCPauseType::ConcurrentStartMarkGC :
                                               G1GCPauseType::ConcurrentStartUndoGC;
  } else if (in_young_gc_before_mixed()) {
    assert(!in_concurrent_start_gc(), "must be");
    return G1GCPauseType::LastYoungGC;
  } else if (in_mixed_phase()) {
    assert(!in_concurrent_start_gc(), "must be");
    assert(!in_young_gc_before_mixed(), "must be");
    return G1GCPauseType::MixedGC;
  } else {
    assert(!in_concurrent_start_gc(), "must be");
    assert(!in_young_gc_before_mixed(), "must be");
    return G1GCPauseType::YoungGC;
  }
}

G1CollectorState::G1CollectorState(G1Policy* policy) :
  _state(PureYoungGC),
  _prev_state(PureYoungGC),
  _policy(policy),

  _initiate_conc_mark_if_possible(false),
  DEBUG_ONLY(_in_young_only_phase(true)),
  DEBUG_ONLY(_in_young_gc_before_mixed(false)),
  DEBUG_ONLY(_in_concurrent_start_gc(false)),
  DEBUG_ONLY(_mark_or_rebuild_in_progress(false)),
  DEBUG_ONLY(_in_full_gc(false)),
  _clearing_next_bitmap(false),
  _mark_or_rebuild_previously(false) { }


const char* G1CollectorState::to_string(GCState state) const {
  switch (state) {
    case PureYoungGC: return "pure young";
    case CMStartGC: return "cm start";
    case CMInProgressYoungGC: return "cm in progress";
    case BeforeMixedYoungGC: return "before mixed";
    case MixedGC: return "mixed";
    case FullGC: return "full";
    default:
      ShouldNotReachHere();
      return "";
  }
}

void G1CollectorState::set_state(GCState from, GCState to) {
  _prev_state = from;
  _state = to;
}

bool G1CollectorState::transform(GCState from, GCState to) {
  log_error(gc) ("==== %s -> %s", to_string(from), to_string(to));
  assert_at_safepoint();
  state_must_be(_state == from);

  switch (from) {
    case PureYoungGC:
      switch (to) {
        case PureYoungGC:
        case CMStartGC:
        case FullGC:
          set_state(from, to);
          return true;
        default:
          assert(false, "%s", to_string(to));
          ShouldNotReachHere();
      }
      break;
    case CMStartGC:
      switch (to) {
        case PureYoungGC: // when set_mark_or_rebuild_in_progress(false); at young collection end
        // case CMStartGC: // It will go through PureYoungGC, rather than go directly to CMStartGC
        case CMInProgressYoungGC:
        case FullGC:
          set_state(from, to);
          return true;
        default:
          assert(false, "%s", to_string(to));
          ShouldNotReachHere();
      }
      break;
    case CMInProgressYoungGC:
      switch (to) {
        case PureYoungGC: // when there is no candidates to start a [before] mixed gc.
        case CMStartGC:
        case CMInProgressYoungGC:
        case BeforeMixedYoungGC:
        case FullGC:
          set_state(from, to);
          return true;
        default:
          assert(false, "%s", to_string(to));
          ShouldNotReachHere();
      }
      break;
    case BeforeMixedYoungGC:
      switch (to) {
        case CMStartGC:
        case MixedGC:
        case FullGC:
          set_state(from, to);
          return true;
        default:
          assert(false, "%s", to_string(to));
          ShouldNotReachHere();
      }
      break;
    case MixedGC:
      switch (to) {
        case MixedGC:
        case PureYoungGC:
        // case CMStartGC: // At Mixed end (there is no candidates), it will go through PureYoungGC, rather than go directly to CMStartGC
        case CMStartGC: // But, before mixed ene (there is still candidates), it will go to CMStartGC directly
        case FullGC:
          set_state(from, to);
          return true;
        default:
          assert(false, "%s", to_string(to));
          ShouldNotReachHere();
      }
      break;
    case FullGC:
      switch (to) {
        case PureYoungGC:
        // case CMStartGC: // It will go through PureYoungGC, rather than go directly to CMStartGC
        case FullGC:
          set_state(from, to);
          return true;
        default:
          assert(false, "%s", to_string(to));
          ShouldNotReachHere();
      }
      break;
    default:
      ShouldNotReachHere();
  }
  ShouldNotReachHere();
  return false;
}

void G1CollectorState::transform_after_full_gc() {
  DEBUG_ONLY(
  set_in_young_only_phase(true);
  set_in_young_gc_before_mixed(false);
  set_in_concurrent_start_gc(false);
  set_mark_or_rebuild_in_progress(false);
  set_clearing_next_bitmap(false);
  set_in_full_gc(false);
  )
  // It will go through PureYoungGC, rather than go directly to CMStartGC
  // transform(FullGC, conc_mark ? CMStartGC : PureYoungGC);
  transform(FullGC, PureYoungGC);
  bool conc_mark = need_to_start_conc_mark("end of Full GC");
  set_initiate_conc_mark_if_possible(conc_mark);
  _mark_or_rebuild_previously = false;
}

void G1CollectorState::transform_to_full_gc() {
  DEBUG_ONLY(
  set_in_young_only_phase(false);
  set_in_full_gc(true);

  // TODO: double check
  set_mark_or_rebuild_in_progress(false);
  )
  transform(_state, FullGC);

  _mark_or_rebuild_previously = _state == CMInProgressYoungGC;
}

bool G1CollectorState::force_concurrent_start_if_outside_cycle(GCCause::Cause gc_cause) {
  // We actually check whether we are marking here and not if we are in a
  // reclamation phase. This means that we will schedule a concurrent mark
  // even while we are still in the process of reclaiming memory.
  bool during_cycle = _policy->_g1h->concurrent_mark()->cm_thread()->in_progress();
  if (!during_cycle) {
    log_debug(gc, ergo)("Request concurrent cycle initiation (requested by GC cause). "
                        "GC cause: %s",
                        GCCause::to_string(gc_cause));
    set_initiate_conc_mark_if_possible(true);
    return true;
  } else {
    log_debug(gc, ergo)("Do not request concurrent cycle initiation "
                        "(concurrent cycle already in progress). GC cause: %s",
                        GCCause::to_string(gc_cause));
    return false;
  }
}

void G1CollectorState::initiate_conc_mark() {
  DEBUG_ONLY(set_in_concurrent_start_gc(true);)
  set_initiate_conc_mark_if_possible(false);

  state_must_be(_state == PureYoungGC ||
                _state == BeforeMixedYoungGC || _state == MixedGC);
}

void G1CollectorState::transform_to_cm_start() {
  state_must_be(_state == PureYoungGC ||
                _state == BeforeMixedYoungGC || _state == MixedGC);
  if (_state == PureYoungGC) {
    initiate_conc_mark();
  } else if (_state == BeforeMixedYoungGC || _state == MixedGC) {
    DEBUG_ONLY(
    set_in_young_only_phase(true);
    set_in_young_gc_before_mixed(false);
    )
    initiate_conc_mark();
  }
  transform(_state, CMStartGC);
}

void G1CollectorState::decide_on_concurrent_start_pause() {
  // We are about to decide on whether this pause will be a
  // concurrent start pause.

  // First, in_concurrent_start_gc() should not be already set. We
  // will set it here if we have to. However, it should be cleared by
  // the end of the pause (it's only set for the duration of a
  // concurrent start pause).
  assert(!in_concurrent_start_gc(), "pre-condition");
  state_must_be(_state == PureYoungGC || _state == CMInProgressYoungGC ||
                _state == BeforeMixedYoungGC || _state == MixedGC);

  // We should not be starting a concurrent start pause if the concurrent mark
  // thread is terminating.
  if (_policy->_g1h->concurrent_mark_is_terminating()) {
    return;
  }

  if (initiate_conc_mark_if_possible()) {
    state_must_be(_state == PureYoungGC || _state == BeforeMixedYoungGC ||
                  _state == MixedGC);
    // We had noticed on a previous pause that the heap occupancy has
    // gone over the initiating threshold and we should start a
    // concurrent marking cycle.  Or we've been explicitly requested
    // to start a concurrent marking cycle.  Either way, we initiate
    // one if not inhibited for some reason.

    GCCause::Cause cause = _policy->_g1h->gc_cause();
    if ((cause != GCCause::_wb_breakpoint) &&
        ConcurrentGCBreakpoints::is_controlled()) {
      log_debug(gc, ergo)("Do not initiate concurrent cycle (whitebox controlled)");
    } else if (!about_to_start_mixed_phase() && in_young_only_phase()) {
      // Initiate a new concurrent start if there is no marking or reclamation going on.
      state_must_be(_state == PureYoungGC);
      transform_to_cm_start();
      log_debug(gc, ergo)("Initiate concurrent cycle (concurrent cycle initiation requested)");
    } else if (_policy->_g1h->is_user_requested_concurrent_full_gc(cause) ||
               (cause == GCCause::_wb_breakpoint)) {
      state_must_be(_state == BeforeMixedYoungGC || _state == MixedGC);
      // Initiate a user requested concurrent start or run to a breakpoint.
      // A concurrent start must be young only GC, so the collector state
      // must be updated to reflect this.
      transform_to_cm_start();

      // We might have ended up coming here about to start a mixed phase with a collection set
      // active. The following remark might change the change the "evacuation efficiency" of
      // the regions in this set, leading to failing asserts later.
      // Since the concurrent cycle will recreate the collection set anyway, simply drop it here.
      _policy->clear_collection_set_candidates();
      _policy->abort_time_to_mixed_tracking();
      log_debug(gc, ergo)("Initiate concurrent cycle (%s requested concurrent cycle)",
                          (cause == GCCause::_wb_breakpoint) ? "run_to breakpoint" : "user");
    } else {
      // The concurrent marking thread is still finishing up the
      // previous cycle. If we start one right now the two cycles
      // overlap. In particular, the concurrent marking thread might
      // be in the process of clearing the next marking bitmap (which
      // we will use for the next cycle if we start one). Starting a
      // cycle now will be bad given that parts of the marking
      // information might get cleared by the marking thread. And we
      // cannot wait for the marking thread to finish the cycle as it
      // periodically yields while clearing the next marking bitmap
      // and, if it's in a yield point, it's waiting for us to
      // finish. So, at this point we will not start a cycle and we'll
      // let the concurrent marking thread complete the last one.
      log_debug(gc, ergo)("Do not initiate concurrent cycle (concurrent cycle already in progress)");

      state_must_be(_state == BeforeMixedYoungGC || _state == MixedGC);
      /*
      if (_state == BeforeMixedYoungGC) {
        transform(BeforeMixedYoungGC, CMStartGC);
      }*/
    }
  }
  // Result consistency checks.
  // We do not allow concurrent start to be piggy-backed on a mixed GC.
  assert(!in_concurrent_start_gc() ||
         in_young_only_phase(), "sanity");
  // We also do not allow mixed GCs during marking.
  assert(!mark_or_rebuild_in_progress_or_previously() || in_young_only_phase(), "sanity");
  state_must_be(_state == PureYoungGC || _state == CMStartGC ||
                _state == BeforeMixedYoungGC || _state == CMInProgressYoungGC ||
                _state == MixedGC);
}

void G1CollectorState::record_concurrent_mark_init_end() {
  assert(!initiate_conc_mark_if_possible(), "we should have cleared it by now");
  DEBUG_ONLY(set_in_concurrent_start_gc(false);)
}

void G1CollectorState::transform_from_cm_in_progress(bool mixed_gc_pending) {
  DEBUG_ONLY(
  set_in_young_gc_before_mixed(mixed_gc_pending);
  set_mark_or_rebuild_in_progress(false);
  )
  if (mixed_gc_pending) {
    transform(CMInProgressYoungGC, BeforeMixedYoungGC);
  } else {
    // when there is no candidates to start a [before] mixed gc.
    transform(CMInProgressYoungGC, PureYoungGC);
  }
}

void G1CollectorState::transform_from_before_mixed() {
  // This has been the young GC before we start doing mixed GCs. We already
  // decided to start mixed GCs much earlier, so there is nothing to do except
  // advancing the state.
  DEBUG_ONLY(
  set_in_young_only_phase(false);
  set_in_young_gc_before_mixed(false);
  )

  transform(BeforeMixedYoungGC, MixedGC);
}

void G1CollectorState::maybe_start_marking(const char* source) {
  if (need_to_start_conc_mark(source)) {
    state_must_be(_state == PureYoungGC);
    set_initiate_conc_mark_if_possible(true);
  }
}

void G1CollectorState::transform_from_mixed() {
  // This is a mixed GC. Here we decide whether to continue doing more
  // mixed GCs or not.
  state_must_be(_state == MixedGC);
  if (!_policy->next_gc_should_be_mixed("continue mixed GCs",
                                        "do not continue mixed GCs")) {
    DEBUG_ONLY(set_in_young_only_phase(true);)
    transform(MixedGC, PureYoungGC);

    _policy->clear_collection_set_candidates();
    maybe_start_marking("end of GC");
  } else {
    transform(MixedGC, MixedGC);
  }
}

void G1CollectorState::transform_at_young_gc_end(G1GCPauseType this_pause) {
  if (G1GCPauseTypeHelper::is_concurrent_start_pause(this_pause)) {
    state_must_be(_state == CMStartGC);
    record_concurrent_mark_init_end();
  } else {
    state_must_be(_state == PureYoungGC || _state == CMInProgressYoungGC ||
                  _state == BeforeMixedYoungGC || _state == MixedGC);
    maybe_start_marking("end of GC");
  }

  if (G1GCPauseTypeHelper::is_last_young_pause(this_pause)) {
    assert(!G1GCPauseTypeHelper::is_concurrent_start_pause(this_pause),
           "The young GC before mixed is not allowed to be concurrent start GC");

    state_must_be(_state == BeforeMixedYoungGC);
    transform_from_before_mixed();
  } else if (G1GCPauseTypeHelper::is_mixed_pause(this_pause)) {
    state_must_be(_state == MixedGC);
    transform_from_mixed();
  } else {
    assert(G1GCPauseTypeHelper::is_young_only_pause(this_pause), "Must be");
    state_must_be(_state == PureYoungGC || _state == CMStartGC ||
                  _state == CMInProgressYoungGC);
  }
}

void G1CollectorState::transform_to_mark_in_progress_at_young_gc_end(G1GCPauseType this_pause,
                                                                     bool concurrent_operation_is_full_mark) {
  assert(!(G1GCPauseTypeHelper::is_concurrent_start_pause(this_pause) && mark_or_rebuild_in_progress_or_previously()),
         "If the last pause has been concurrent start, we should not have been in the marking window");
  if (G1GCPauseTypeHelper::is_concurrent_start_pause(this_pause)) {
    state_must_be(_state == CMStartGC);
    DEBUG_ONLY(set_mark_or_rebuild_in_progress(concurrent_operation_is_full_mark);)
    if (concurrent_operation_is_full_mark) {
      transform(CMStartGC, CMInProgressYoungGC);
    } else {
      // TODO: transform to which state ??
      transform(CMStartGC, PureYoungGC);
    }
  }
}

bool G1CollectorState::about_to_start_mixed_phase() const {
  bool res = _policy->_g1h->concurrent_mark()->cm_thread()->in_progress() ||
              _state == CMInProgressYoungGC || _state == BeforeMixedYoungGC;
  DEBUG_ONLY(
  bool res2 = _policy->_g1h->concurrent_mark()->cm_thread()->in_progress() ||
            in_young_gc_before_mixed();
  assert(res2 == res, "Must be, %s, %s", BOOL_TO_STR(res), to_string(_state));
  )
  return res;
}

bool G1CollectorState::need_to_start_conc_mark(const char* source, size_t alloc_word_size) {
  if (about_to_start_mixed_phase()) {
    return false;
  }

  size_t marking_initiating_used_threshold = _policy->get_conc_mark_start_threshold();

  size_t cur_used_bytes = _policy->_g1h->non_young_capacity_bytes();
  size_t alloc_byte_size = alloc_word_size * HeapWordSize;
  size_t marking_request_bytes = cur_used_bytes + alloc_byte_size;

  bool result = false;
  if (marking_request_bytes > marking_initiating_used_threshold) {
    result = in_young_only_phase() && !in_young_gc_before_mixed();
    log_debug(gc, ergo, ihop)("%s occupancy: " SIZE_FORMAT "B allocation request: " SIZE_FORMAT "B threshold: " SIZE_FORMAT "B (%1.2f) source: %s",
                              result ? "Request concurrent cycle initiation (occupancy higher than threshold)"
                                     : "Do not request concurrent cycle initiation (still doing mixed collections)",
                              cur_used_bytes,
                              alloc_byte_size,
                              marking_initiating_used_threshold,
                              (double) marking_initiating_used_threshold / _policy->_g1h->capacity() * 100,
                              source);
  }
  return result;
}

bool G1CollectorState::concurrent_operation_is_full_mark(const char* msg) {
  return in_concurrent_start_gc() &&
         ((_policy->_g1h->gc_cause() != GCCause::_g1_humongous_allocation) || need_to_start_conc_mark(msg));
}
