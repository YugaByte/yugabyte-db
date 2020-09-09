// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
//
// The following only applies to changes made to this file as part of YugaByte development.
//
// Portions Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//
// Simple pool/freelist for objects of the same type, typically used
// in local context.
#ifndef YB_UTIL_OBJECT_POOL_H
#define YB_UTIL_OBJECT_POOL_H

#include <stdint.h>

#include <thread>
#include <functional>

#include <boost/container/stable_vector.hpp>
#include <boost/lockfree/stack.hpp>

#include <glog/logging.h>

#include "yb/gutil/gscoped_ptr.h"
#include "yb/gutil/manual_constructor.h"
#include "yb/gutil/sysinfo.h"

namespace yb {

using base::ManualConstructor;

template<class T>
class ReturnToPool;

// An object pool allocates and destroys a single class of objects
// off of a free-list.
//
// Upon destruction of the pool, any objects allocated from this pool are
// destroyed, regardless of whether they have been explicitly returned to the
// pool.
//
// This class is similar to the boost::pool::object_pool, except that the boost
// implementation seems to have O(n) deallocation performance and benchmarked
// really poorly.
//
// This class is not thread-safe.
template<typename T>
class ObjectPool {
 public:
  typedef ReturnToPool<T> deleter_type;
  typedef gscoped_ptr<T, deleter_type> scoped_ptr;

  ObjectPool() :
    free_list_head_(NULL),
    alloc_list_head_(NULL),
    deleter_(this) {
  }

  ~ObjectPool() {
    // Delete all objects ever allocated from this pool
    ListNode *node = alloc_list_head_;
    while (node != NULL) {
      ListNode *tmp = node;
      node = node->next_on_alloc_list;
      if (!tmp->is_on_freelist) {
        // Have to run the actual destructor if the user forgot to free it.
        tmp->Destroy();
      }
      delete tmp;
    }
  }

  // Construct a new object instance from the pool.
  T *Construct() {
    ManualConstructor<T> *obj = GetObject();
    obj->Init();
    return obj->get();
  }

  template<class Arg1>
  T *Construct(Arg1 arg1) {
    ManualConstructor<T> *obj = GetObject();
    obj->Init(arg1);
    return obj->get();
  }

  // Destroy an object, running its destructor and returning it to the
  // free-list.
  void Destroy(T *t) {
    CHECK_NOTNULL(t);
    ListNode *node = static_cast<ListNode *>(
      reinterpret_cast<ManualConstructor<T> *>(t));

    node->Destroy();

    DCHECK(!node->is_on_freelist);
    node->is_on_freelist = true;
    node->next_on_free_list = free_list_head_;
    free_list_head_ = node;
  }

  // Create a scoped_ptr wrapper around the given pointer which came from this
  // pool.
  // When the scoped_ptr goes out of scope, the object will get released back
  // to the pool.
  scoped_ptr make_scoped_ptr(T *ptr) {
    return scoped_ptr(ptr, deleter_);
  }

 private:
  class ListNode : ManualConstructor<T> {
    friend class ObjectPool<T>;

    ListNode *next_on_free_list;
    ListNode *next_on_alloc_list;

    bool is_on_freelist;
  };


  ManualConstructor<T> *GetObject() {
    if (free_list_head_ != NULL) {
      ListNode *tmp = free_list_head_;
      free_list_head_ = tmp->next_on_free_list;
      tmp->next_on_free_list = NULL;
      DCHECK(tmp->is_on_freelist);
      tmp->is_on_freelist = false;

      return static_cast<ManualConstructor<T> *>(tmp);
    }
    auto new_node = new ListNode();
    new_node->next_on_free_list = NULL;
    new_node->next_on_alloc_list = alloc_list_head_;
    new_node->is_on_freelist = false;
    alloc_list_head_ = new_node;
    return new_node;
  }

  // Keeps track of free objects in this pool.
  ListNode *free_list_head_;

  // Keeps track of all objects ever allocated by this pool.
  ListNode *alloc_list_head_;

  deleter_type deleter_;
};

// Functor which returns the passed objects to a specific object pool.
// This can be used in conjunction with scoped_ptr to automatically release
// an object back to a pool when it goes out of scope.
template<class T>
class ReturnToPool {
 public:
  explicit ReturnToPool(ObjectPool<T> *pool) :
    pool_(pool) {
  }

  inline void operator()(T *ptr) const {
    pool_->Destroy(ptr);
  }

 private:
  ObjectPool<T> *pool_;
};

template <class T>
struct DefaultFactory {
  T* operator()() const {
    return new T;
  }
};

template <class T>
class ThreadSafeObjectPool {
 public:
  typedef std::function<T*()> Factory;
  typedef std::function<void(T*)> Deleter;

  explicit ThreadSafeObjectPool(Factory factory = DefaultFactory<T>(),
                                Deleter deleter = std::default_delete<T>())
      : factory_(std::move(factory)), deleter_(std::move(deleter)) {
    // Need the actual number of CPUs, so we do not use the Gflag value
    auto num_cpus = base::RawNumCPUs();
    pools_.reserve(num_cpus);
    while (pools_.size() != num_cpus) {
      pools_.emplace_back(50);
    }
  }

  ~ThreadSafeObjectPool() {
    T* object;
    for (auto& pool : pools_) {
      while (pool.pop(object)) {
        deleter_(object);
      }
    }
  }

  T* Take() {
    auto& pool = pools_[GetCPU()];
    T* result;
    if (pool.pop(result)) {
      return result;
    }
    return factory_();
  }

  void Release(T* value) {
    auto& pool = pools_[GetCPU()];
    if (!pool.bounded_push(value)) {
      deleter_(value);
    }
  }

 private:
  size_t GetCPU() const {
#if defined(__APPLE__)
    // OSX doesn't have a way to get the CPU, so we'll pick a random one.
    return std::hash<std::thread::id>()(std::this_thread::get_id()) % pools_.size();
#else
    size_t cpu = sched_getcpu();
    DCHECK_LT(cpu, pools_.size());
    return cpu;
#endif // defined(__APPLE__)
  }

  typedef boost::lockfree::stack<T*> Pool;

  Factory factory_;
  Deleter deleter_;
  boost::container::stable_vector<Pool> pools_;
};

} // namespace yb

#endif // YB_UTIL_OBJECT_POOL_H
