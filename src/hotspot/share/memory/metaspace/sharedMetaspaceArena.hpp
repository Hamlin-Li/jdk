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

#ifndef SHARE_MEMORY_METASPACE_SHAREDMETASPACEARENA_HPP
#define SHARE_MEMORY_METASPACE_SHAREDMETASPACEARENA_HPP

#include "memory/metaspace/metaspaceArena.hpp"
#include "utilities/bitMap.hpp"
#include "utilities/hashtable.hpp"
#include "utilities/macros.hpp"

namespace metaspace {

// This class is a shared version of MetaspaceArena, it's intended to reduce
// the memory footprint by sharing an underlying MetaspaceArena among cld's.
// This class tracks the allocations in different cld's, and try to return
// chunks back to chunk manager when deallocation of single allocation or
// deallocation of a whole cld. To do this (return chunks back to chunk manager)
// it needs assistance of several data structures:
//   whether a chunk is still occupied by at least one cld;
//     it's sufficient to use a allocation count to tell, but it's might be helpful
//     to catch exceptions with a bitmap, bitmap should only be used in debug mode.
//   all allocations in a cld;
//   mapping from allocation to chunk;
class SharedMetaspaceArena : public MetaspaceArena {
  template<typename Key, typename Value>
  class Entry : public BasicHashtableEntry<mtMetaspace> {
    friend class VMStructs;
  private:
    Key _key;
    Value _value;

  public:
    Entry(Key key, Value value, uint hash) :
      BasicHashtableEntry(hash),
      _key(key), _value(value) { }

    Key key() { return _key; }
    Value value() { return _value; }
    void set_key(Key key) { _key = key; }
    void set_value(Value value) { _value = value; }

    Entry* next() const {
      return (Entry*)BasicHashtableEntry<mtMetaspace>::next();
    }

    Entry** next_addr() {
      return (Entry**)BasicHashtableEntry<mtMetaspace>::next_addr();
    }
  };


  template<typename Key, typename Value>
  class Map : public BasicHashtable<mtMetaspace> {
    friend class VMStructs;
  private:
    Entry<Key, Value>** bucket_addr(int i) {
      return (Entry<Key, Value>**) BasicHashtable<mtMetaspace>::bucket_addr(i);
    }

    Entry<Key, Value>* new_entry(Key key, Value value) {
      Entry<Key, Value>* entry =
        ::new (NEW_C_HEAP_ARRAY(char, this->entry_size(), mtMetaspace))
              Entry<Key, Value>(key, value, compute_hash(key));
      return entry;
    }

    int index_for(Key key) {
      return hash_to_index(compute_hash(key));
    }

    Entry<Key, Value>* bucket(int i) {
      return (Entry<Key, Value>*) BasicHashtable<mtMetaspace>::bucket(i);
    }

    static uint32_t address_to_uint32(uintptr_t key) {
      uint32_t hash = (uint32_t)(key >> 3);
      hash = ~hash + (hash << 15);
      hash = hash ^ (hash >> 12);
      hash = hash + (hash << 2);
      hash = hash ^ (hash >> 4);
      hash = hash * 2057;
      hash = hash ^ (hash >> 16);
      return hash;
    }

    static unsigned int compute_hash(intptr_t key) {
      return address_to_uint32(key);
    }
    static unsigned int compute_hash(Metachunk* key) {
      return address_to_uint32(p2i(key));
    }
    static unsigned int compute_hash(ClassLoaderData* key) {
      return address_to_uint32(p2i(key));
    }
    static unsigned int compute_hash(MetaWord* key) {
      return address_to_uint32(p2i(key));
    }

  public:
    Map(int table_size) :
      BasicHashtable<mtMetaspace>(table_size, (int)sizeof(Entry<Key, Value>)) { }
    ~Map() { }

    Entry<Key, Value>* find_entry(Key key);
    Entry<Key, Value>* remove_entry(Key key);
    void add_entry(Key key, Value value);
    bool add_entry_if_absent(Key key, Value value);
    void put_entry(Key key, Value value);
  };

  // Basic data structures:
  //  map<addr, chunk>
  //  map<chunk, count>
  //  map<chunk, bitmap>
  //  map<cld, set<addr>>
  Map<intptr_t, Metachunk*> _addr_to_chunk;
  Map<Metachunk*, uint> _chunk_to_count;
#ifdef ASSERT
  Map<Metachunk*, CHeapBitMap*> _chunk_to_bitmap;
#endif //ASSERT
  Map<ClassLoaderData*, Map<MetaWord*, bool>*> _cld_to_addr;

  class CLDAllocationsClosure {
    SharedMetaspaceArena* _arena;
  public:
    CLDAllocationsClosure(SharedMetaspaceArena* arena) : _arena(arena) { }
    void visit(BasicHashtableEntry<mtMetaspace>* cur, ClassLoaderData* cld);
  };
  class CLDsClosure {
    SharedMetaspaceArena* _arena;
  public:
    CLDsClosure(SharedMetaspaceArena* arena) : _arena(arena) { }
    void visit(BasicHashtableEntry<mtMetaspace>* cur, void*);
  };
  static const uint LOG2_OF_HIGHEST_CHUNK_LEVEL;

protected:
  // Virtual methods declared in MetaspaceArena.
  virtual void add_allocation_to_fbl(MetaWord* p, size_t word_size);
  virtual Metachunk* allocate_new_chunk(size_t requested_word_size);
  virtual MetaWord* allocate_in_current_chunk(size_t word_size, ClassLoaderData* cld);
  virtual bool attempt_enlarge_current_chunk(size_t requested_word_size);

  void deallocate_no_lock(MetaWord* p, ClassLoaderData* cld, bool remove_entry = true);

  void return_non_first_chunk(Metachunk* chunk);
public:
  SharedMetaspaceArena(ChunkManager *chunk_manager, const ArenaGrowthPolicy *growth_policy,
                       Mutex *lock, SizeAtomicCounter *total_used_words_counter,
                       const char *name);
  virtual ~SharedMetaspaceArena();

  void record_cld(ClassLoaderData* cld);

  void deallocate(MetaWord* p, ClassLoaderData* cld);
  void deallocate(ClassLoaderData* cld);
};

} // namespace metaspace

#endif //SHARE_MEMORY_METASPACE_SHAREDMETASPACEARENA_HPP
