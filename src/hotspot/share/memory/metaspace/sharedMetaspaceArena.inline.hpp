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

#ifndef SHARE_MEMORY_METASPACE_SHAREDMETASPACEARENA_INLINE_HPP
#define SHARE_MEMORY_METASPACE_SHAREDMETASPACEARENA_INLINE_HPP

#include "memory/metaspace/sharedMetaspaceArena.hpp"

namespace metaspace {

  template<typename Key, typename Value>
  SharedMetaspaceArena::Entry<Key, Value>* SharedMetaspaceArena::Map<Key, Value>::find_entry(Key key) {
    uint hash = compute_hash(key);
    int index = index_for(key);
    for (Entry<Key, Value>* p = bucket(index); p != NULL; p = p->next()) {
      if (p->hash() == hash && p->key() == key) {
        return p;
      }
    }
    return NULL;
  }

  template<typename Key, typename Value>
  SharedMetaspaceArena::Entry<Key, Value>* SharedMetaspaceArena::Map<Key, Value>::remove_entry(Key key) {
    uint hash = compute_hash(key);
    int index = index_for(key);
    Entry<Key, Value>* prev = NULL;
    for (Entry<Key, Value>* p = bucket(index); p != NULL; p = p->next()) {
      if (p->hash() == hash && p->key() == key) {
        if (prev == NULL) {
          set_entry(index, p->next());
        } else {
          prev->set_next(p->next());
        }
        return p;
      }
      prev = p;
    }
    return NULL;
  }

  template<typename Key, typename Value>
  void SharedMetaspaceArena::Map<Key, Value>::add_entry(Key key, Value value) {
    int idx = index_for(key);
    BasicHashtable<mtMetaspace>::add_entry(idx, new_entry(key, value));
  }

  template<typename Key, typename Value>
  bool SharedMetaspaceArena::Map<Key, Value>::add_entry_if_absent(Key key, Value value) {
    if (find_entry(key) != NULL) {
      return false;
    }
    add_entry(key, value);
    return true;
  }

  template<typename Key, typename Value>
  void SharedMetaspaceArena::Map<Key, Value>::put_entry(Key key, Value value) {
    uint hash = compute_hash(key);
    int index = index_for(key);
    for (Entry<Key, Value>* p = bucket(index); p != NULL; p = p->next()) {
      if (p->hash() == hash && p->key() == key) {
        p->set_key(key);
        p->set_value(value);
        break;
      }
    }
  }
}

#endif //SHARE_MEMORY_METASPACE_SHAREDMETASPACEARENA_INLINE_HPP
