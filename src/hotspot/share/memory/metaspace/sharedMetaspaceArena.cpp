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

#include "memory/allocation.hpp"
#include "memory/metaspace/chunklevel.hpp"
#include "memory/metaspace/freeBlocks.hpp"
#include "memory/metaspace/metaspaceCommon.hpp"
#include "memory/metaspace/metaspaceSettings.hpp"
#include "memory/metaspace/sharedMetaspaceArena.inline.hpp"

namespace metaspace {

#define LOGFMT         "Arena @" PTR_FORMAT " (%s)"
#define LOGFMT_ARGS    p2i(this), this->_name

const uint SharedMetaspaceArena::LOG2_OF_HIGHEST_CHUNK_LEVEL =
  log2i((chunklevel::MAX_CHUNK_BYTE_SIZE >> chunklevel::HIGHEST_CHUNK_LEVEL));

void SharedMetaspaceArena::add_allocation_to_fbl(MetaWord* p, size_t word_size) {
  assert(Settings::handle_deallocations(), "Sanity");
  // Do nothing, this is to skip fbl related logic.
}

Metachunk* SharedMetaspaceArena::allocate_new_chunk(size_t requested_word_size) {
  assert(lock()->is_locked(), "Must be");

  Metachunk* chunk = MetaspaceArena::allocate_new_chunk(requested_word_size);
  // Basic steps: initializes chunk related data
  //  map<chunk, count>.put(chunk, 0);
  //  map<chunk, bitmap>.put(chunk, empty bitmap);
  _chunk_to_count.put_entry(chunk, 0);
  // TODO: set bitmap (DEBUG_ONLY)
  return chunk;
}

MetaWord* SharedMetaspaceArena::allocate_in_current_chunk(size_t word_size, ClassLoaderData* cld) {
  assert(lock()->is_locked(), "Must be");

  Metachunk* current = current_chunk();
  MetaWord* p = current->allocate(word_size);
  assert(p != NULL, "");

  // Basic steps: records allocation in a chunk and cld
  //  map<addr, chunk>.put(p >> LOG2_OF_HIGHEST_CHUNK_LEVEL, current);
  //  map<chunk, count>.put(chunk, xxx + 1);
  //  map<chunk, bitmap>.get(chunk).par_set_bit(...);
  //  map<cld, set<addr>>.get(cld).add(p);
  _addr_to_chunk.add_entry_if_absent(p2i(p) >> LOG2_OF_HIGHEST_CHUNK_LEVEL, current);
  _chunk_to_count.add_entry_if_absent(current, 0);
  _chunk_to_count.put_entry(current, _chunk_to_count.find_entry(current)->value() + 1);
  // TODO: set bitmap (DEBUG_ONLY)

  Map<MetaWord*, bool>* set = _cld_to_addr.find_entry(cld)->value();
  set->add_entry(p, true);

  return p;
}

bool SharedMetaspaceArena::attempt_enlarge_current_chunk(size_t requested_word_size) {
  // Do NOT enlarge the current chunk, this simplifies the logic related to
  // _addr_to_chunk, _chunk_to_count, _chunk_to_bitmap.
  return false;
}

void SharedMetaspaceArena::return_non_first_chunk(Metachunk* chunk) {
  assert(lock()->is_locked(), "Must be");

  assert(chunk != current_chunk(), "Must be");
  _chunks.remove_non_first_chunk(chunk);

  DEBUG_ONLY(chunk->set_prev(NULL);)
  DEBUG_ONLY(chunk->set_next(NULL);)
  chunk_manager()->return_chunk(chunk);
  _total_used_words_counter->decrement_by(chunk->used_words());
  DEBUG_ONLY(chunk_manager()->verify();)
}

SharedMetaspaceArena::SharedMetaspaceArena(ChunkManager* chunk_manager,
                                           const ArenaGrowthPolicy* growth_policy,
                                           Mutex* lock,
                                           SizeAtomicCounter* total_used_words_counter,
                                           const char* name) :
                                           MetaspaceArena(chunk_manager,
                                                          growth_policy,
                                                          lock,
                                                          total_used_words_counter,
                                                          name),
                                           _addr_to_chunk(16),
                                           _chunk_to_count(16),
                                           _chunk_to_bitmap(16),
                                           _cld_to_addr(16) {
}

SharedMetaspaceArena::~SharedMetaspaceArena() {
  AllCLDsVisitor visitor(this);
  _cld_to_addr.iterate(&visitor, (void*)NULL);
}

void SharedMetaspaceArena::record_cld(ClassLoaderData* cld) {
  // No need to protect it with lock explicitly, as it's already protected in
  // ClassLoaderData::metaspace_non_null
  assert(lock()->is_locked(), "Must be");

  // Basic steps: initialize the set of allocations in a cld.
  //  map<cld, set<addr>>.put(cld, empty set);
  Map<MetaWord*, bool>* set =
    ::new (NEW_C_HEAP_ARRAY(char, sizeof(Map<MetaWord*, bool>), mtMetaspace))
          Map<MetaWord*, bool>(16);
  assert(_cld_to_addr.find_entry(cld) == NULL, "Must be");
  _cld_to_addr.add_entry(cld, set);
}

void SharedMetaspaceArena::deallocate(MetaWord* p, ClassLoaderData* cld) {
  MutexLocker cl(lock(), Mutex::_no_safepoint_check_flag);
  deallocate_no_lock(p, cld);
}

void SharedMetaspaceArena::deallocate_no_lock(MetaWord* p, ClassLoaderData* cld, bool remove_entry) {
  assert(lock()->is_locked(), "Must be");

  // Basic steps: deallocate one allocation.
  //  map<cld, set<addr>>.get(cld).remove(p);
  //
  //  chunk = map<addr, chunk>.get(p >> LOG2_OF_HIGHEST_CHUNK_LEVEL);
  //  map<chunk, count>.put(chunk, xxx - 1);
  //  map<chunk, bitmap>.get(chunk).clear_bit(...);
  //
  //  if map<chunk, count>.get(chunk) == 0
  //    assert(map<chunk, bitmap>.get(chunk).is_empty());
  //    if the chunk is current_chunk()
  //      reset the chunk
  //      map<chunk, count>.put(chunk, 0)
  //    else
  //      map<chunk, count>.remove(chunk);
  //      map<chunk, bitmap>.remove(chunk);
  //
  //      return_non_first_chunk(chunk);
  if (remove_entry) {
    _cld_to_addr.find_entry(cld)->value()->remove_entry(p);
  }

  Metachunk* chunk = _addr_to_chunk.find_entry(p2i(p) >> LOG2_OF_HIGHEST_CHUNK_LEVEL)->value();
  assert(chunk != NULL, "Must be");
  assert(_chunk_to_count.find_entry(chunk) != NULL, "Must be");
  _chunk_to_count.put_entry(chunk, _chunk_to_count.find_entry(chunk)->value() - 1);
  // TODO: set bitmap (DEBUG_ONLY)

  if (_chunk_to_count.find_entry(chunk)->value() == 0) {
    // TODO: assert bitmap empty (DEBUG_ONLY)

    for (MetaWord* start = chunk->base();
        start < chunk->end();
        start += ((1 << LOG2_OF_HIGHEST_CHUNK_LEVEL) / BytesPerWord)) {
      _addr_to_chunk.remove_entry(p2i(start) >> LOG2_OF_HIGHEST_CHUNK_LEVEL);
    }

    if (chunk == current_chunk()) {
      _total_used_words_counter->decrement_by(chunk->used_words());
      chunk->reset_used_words();
      _chunk_to_count.put_entry(chunk, 0);
      // TODO: set bitmap (DEBUG_ONLY)
      return;
    }
    _chunk_to_count.remove_entry(chunk);
    // TODO: clear bitmap (DEBUG_ONLY)

    return_non_first_chunk(chunk);
  }
}

void SharedMetaspaceArena::deallocate(ClassLoaderData* cld) {
  MutexLocker cl(lock(), Mutex::_no_safepoint_check_flag);
  // Basic steps: deallocate all allocations in a cld
  //  for addr in map<cld, set<addr>.remove(cld)
  //    deallocate(addr, cld);
  Entry<ClassLoaderData*, Map<MetaWord*, bool>*>* entry = _cld_to_addr.remove_entry(cld);
  assert(entry != NULL, "Must be");
  Map<MetaWord*, bool>* set = entry->value();
  assert(set != NULL, "Must be");
  SingleCLDVisitor visitor(this);
  set->iterate(&visitor, cld);
  delete set;
}

void SharedMetaspaceArena::SingleCLDVisitor::visit(BasicHashtableEntry<mtMetaspace>* cur,
                                                   ClassLoaderData* cld) {
  assert(_arena->lock()->is_locked(), "Must be");
  Entry<MetaWord*, bool>* entry = (Entry<MetaWord*, bool>*)cur;
  assert(entry != NULL, "Must be");
  assert(entry->key() != NULL, "Must be");
  // Do NOT remove single allocation entry during iteration, allocation entries
  // for one cld should be removed together later after iteration.
  _arena->deallocate_no_lock(entry->key(), cld, false);
}

void SharedMetaspaceArena::AllCLDsVisitor::visit(BasicHashtableEntry<mtMetaspace>* cur, void*) {
  assert(_arena->lock()->is_locked(), "Must be");
  Entry<ClassLoaderData*, Map<MetaWord*, bool>*>* entry =
    (Entry<ClassLoaderData*, Map<MetaWord*, bool>*>*)cur;
  assert(entry != NULL, "Must be");
  assert(entry->key() != NULL, "Must be");
  _arena->deallocate(entry->key());
}

} // namespace metaspace
