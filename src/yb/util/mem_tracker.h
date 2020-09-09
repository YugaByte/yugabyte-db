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
#ifndef YB_UTIL_MEM_TRACKER_H
#define YB_UTIL_MEM_TRACKER_H

#include <stdint.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

#ifdef TCMALLOC_ENABLED
#include <gperftools/malloc_extension.h>
#endif

#include <boost/optional.hpp>

#include "yb/gutil/ref_counted.h"
#include "yb/util/high_water_mark.h"
#include "yb/util/locks.h"
#include "yb/util/mutex.h"
#include "yb/util/random.h"
#include "yb/util/strongly_typed_bool.h"

namespace yb {

class Status;
class MemTracker;
typedef std::shared_ptr<MemTracker> MemTrackerPtr;

// Garbage collector is used by MemTracker to free memory allocated by caches when reached
// soft memory limit.
class GarbageCollector {
 public:
  virtual void CollectGarbage(size_t required) = 0;

 protected:
  ~GarbageCollector() {}
};

YB_STRONGLY_TYPED_BOOL(MayExist);
YB_STRONGLY_TYPED_BOOL(AddToParent);
YB_STRONGLY_TYPED_BOOL(CreateMetrics);
YB_STRONGLY_TYPED_BOOL(OnlyChildren);

typedef std::function<int64_t()> ConsumptionFunctor;
typedef std::function<void()> PollChildrenConsumptionFunctors;

struct SoftLimitExceededResult {
  bool exceeded;
  double current_capacity_pct;
};

// A MemTracker tracks memory consumption; it contains an optional limit and is
// arranged into a tree structure such that the consumption tracked by a
// MemTracker is also tracked by its ancestors.
//
// The MemTracker hierarchy is rooted in a single static MemTracker whose limi
// is set via gflag. The root MemTracker always exists, and it is the common
// ancestor to all MemTrackers. All operations that discover MemTrackers begin
// at the root and work their way down the tree, while operations that deal
// with adjusting memory consumption begin at a particular MemTracker and work
// their way up the tree to the root. The tree structure is strictly enforced:
// all MemTrackers (except the root) must have a parent, and all children
// belonging to a parent must have unique ids.
//
// When a MemTracker begins its life, it has a strong reference to its parent
// and the parent has a weak reference to it. The strong reference remains for
// the lifetime of the MemTracker, but the weak reference can be dropped via
// UnregisterFromParent(). A MemTracker in this state may continue servicing
// memory consumption operations while allowing a new MemTracker with the same
// id to be created on the old parent.
//
// By default, memory consumption is tracked via calls to Consume()/Release(), either to
// the tracker itself or to one of its descendents. Alternatively, a consumption function
// can be specified, and then the function's value is used as the consumption rather than the
// tally maintained by Consume() and Release(). A tcmalloc function is used to track process
// memory consumption, since the process memory usage may be higher than the computed
// total memory (tcmalloc does not release deallocated memory immediately).
//
// GcFunctions can be attached to a MemTracker in order to free up memory if the limit is
// reached. If LimitExceeded() is called and the limit is exceeded, it will first call the
// GcFunctions to try to free memory and recheck the limit. For example, the process
// tracker has a GcFunction that releases any unused memory still held by tcmalloc, so
// this will be called before the process limit is reported as exceeded. GcFunctions are
// called in the order they are added, so expensive functions should be added last.
//
// This class is thread-safe.
//
// NOTE: this class has been partially ported over from Impala with
// several changes, and as a result the style differs somewhat from
// the YB style.
//
// Changes from Impala:
// 1) Id a string vs. a TUniqueId
// 2) There is no concept of query trackers vs. pool trackers -- trackers are instead
//    associated with objects. Parent hierarchy is preserved, with the assumption that,
//    e.g., a tablet server's memtracker will have as its children the tablets' memtrackers,
//    which in turn will have memtrackers for their caches, logs, and so forth.
//
// TODO: this classes uses a lot of statics fields and methods, which
// isn't common in YB. It is probably wise to later move the
// 'registry' of trackers to a separate class, but it's better to
// start using the 'class' *first* and then change this functionality,
// depending on how MemTracker ends up being used in YB.
class MemTracker : public std::enable_shared_from_this<MemTracker> {
 public:
  // byte_limit < 0 means no limit
  // 'id' is the label for LogUsage() and web UI.
  //
  // add_to_parent could be set to false in cases when we want to track memory usage of
  // some subsystem, but don't want this subsystem to take effect on parent mem tracker.
  MemTracker(int64_t byte_limit, const std::string& id,
             ConsumptionFunctor consumption_functor,
             std::shared_ptr<MemTracker> parent,
             AddToParent add_to_parent, CreateMetrics create_metrics);

  ~MemTracker();

  #ifdef TCMALLOC_ENABLED
  static int64_t GetTCMallocProperty(const char* prop) {
    size_t value;
    if (!MallocExtension::instance()->GetNumericProperty(prop, &value)) {
      LOG(DFATAL) << "Failed to get tcmalloc property " << prop;
      value = 0;
    }
    return value;
  }

  static int64_t GetTCMallocCurrentAllocatedBytes() {
    return GetTCMallocProperty("generic.current_allocated_bytes");
  }

  static int64_t GetTCMallocCurrentHeapSizeBytes() {
    return GetTCMallocProperty("generic.heap_size");
  }

  static int64_t GetTCMallocActualHeapSizeBytes() {
    return GetTCMallocCurrentHeapSizeBytes() -
           GetTCMallocProperty("tcmalloc.pageheap_unmapped_bytes");
  }
  #endif

  // Removes this tracker from its parent's children. This tracker retains its
  // link to its parent. Must be called on a tracker with a parent.
  //
  // Automatically called in the MemTracker destructor, but should be called
  // explicitly when an object is destroyed if that object is also the "primary
  // owner" of a tracker (i.e. the object that originally created the tracker).
  // This orphans the tracker so that if the object is recreated, its new
  // tracker won't collide with the now orphaned tracker.
  //
  // Is thread-safe on the parent but not the child. Meaning, multiple trackers
  // that share the same parent can all UnregisterFromParent() at the same
  // time, but all UnregisterFromParent() calls on a given tracker must be
  // externally synchronized.
  void UnregisterFromParent();

  void UnregisterChild(const std::string& id);

  // Creates and adds the tracker to the tree so that it can be retrieved with
  // FindTracker/FindOrCreateTracker.
  //
  // byte_limit < 0 means no limit; 'id' is a used as a label for LogUsage()
  // and web UI and must be unique for the given parent. Use the two-argument
  // form if there is no parent.
  static std::shared_ptr<MemTracker> CreateTracker(
      int64_t byte_limit,
      const std::string& id,
      const std::shared_ptr<MemTracker>& parent = std::shared_ptr<MemTracker>(),
      AddToParent add_to_parent = AddToParent::kTrue,
      CreateMetrics create_metrics = CreateMetrics::kTrue) {
    return CreateTracker(
        byte_limit, id, ConsumptionFunctor(), parent, add_to_parent, create_metrics);
  }

  static std::shared_ptr<MemTracker> CreateTracker(
      const std::string& id,
      const std::shared_ptr<MemTracker>& parent = std::shared_ptr<MemTracker>(),
      AddToParent add_to_parent = AddToParent::kTrue,
      CreateMetrics create_metrics = CreateMetrics::kTrue) {
    return CreateTracker(-1 /* byte_limit */, id, parent, add_to_parent, create_metrics);
  }

  static std::shared_ptr<MemTracker> CreateTracker(
      int64_t byte_limit,
      const std::string& id,
      ConsumptionFunctor consumption_functor,
      const std::shared_ptr<MemTracker>& parent = std::shared_ptr<MemTracker>(),
      AddToParent add_to_parent = AddToParent::kTrue,
      CreateMetrics create_metrics = CreateMetrics::kTrue);

  // If a tracker with the specified 'id' and 'parent' exists in the tree, sets
  // 'tracker' to reference that instance. Use the two-argument form if there
  // is no parent. Returns false if no such tracker exists.
  static MemTrackerPtr FindTracker(
      const std::string& id,
      const MemTrackerPtr& parent = MemTrackerPtr());

  MemTrackerPtr FindChild(const std::string& id);

  // If a tracker with the specified 'id' and 'parent' exists in the tree,
  // returns a shared_ptr to that instance. Otherwise, creates a new
  // MemTracker with the specified byte_limit, id, and parent. Use the two
  // argument form if there is no parent.
  static std::shared_ptr<MemTracker> FindOrCreateTracker(
      int64_t byte_limit,
      const std::string& id,
      const std::shared_ptr<MemTracker>& parent = std::shared_ptr<MemTracker>(),
      AddToParent add_to_parent = AddToParent::kTrue,
      CreateMetrics create_metrics = CreateMetrics::kTrue);

  static std::shared_ptr<MemTracker> FindOrCreateTracker(
      const std::string& id,
      const std::shared_ptr<MemTracker>& parent = std::shared_ptr<MemTracker>(),
      AddToParent add_to_parent = AddToParent::kTrue,
      CreateMetrics create_metrics = CreateMetrics::kTrue) {
    return FindOrCreateTracker(-1 /* byte_limit */, id, parent, add_to_parent, create_metrics);
  }

  void ListDescendantTrackers(
      std::vector<MemTrackerPtr>* trackers, OnlyChildren only_children = OnlyChildren::kFalse);

  // Returns a list of all children of this tracker.
  std::vector<MemTrackerPtr> ListChildren();

  // Returns a list of all the valid trackers.
  static std::vector<MemTrackerPtr> ListTrackers();

  // Gets a shared_ptr to the "root" tracker, creating it if necessary.
  static MemTrackerPtr GetRootTracker();

  // Tries to update consumption from external source.
  // Returns true if consumption was updated, false otherwise.
  //
  // Currently it uses totally allocated bytes by tcmalloc for root mem tracker when available.
  bool UpdateConsumption(bool force = false);

  // Increases consumption of this tracker and its ancestors by 'bytes'.
  void Consume(int64_t bytes);

  // Try to expand the limit (by asking the resource broker for more memory) by at least
  // 'bytes'. Returns false if not possible, true if the request succeeded. May allocate
  // more memory than was requested.
  // TODO: always returns false for now, not yet implemented.
  bool ExpandLimit(int64_t /* unused: bytes */) { return false; }

  // Increases consumption of this tracker and its ancestors by 'bytes' only if
  // they can all consume 'bytes'. If this brings any of them over, none of them
  // are updated.
  // Returns true if the try succeeded.
  // In case of failure mem tracker that prevented consumption will be stored to
  // blocking_mem_tracker.
  bool TryConsume(int64_t bytes, MemTracker** blocking_mem_tracker = nullptr);

  // Decreases consumption of this tracker and its ancestors by 'bytes'.
  void Release(int64_t bytes);

  // Returns true if a valid limit of this tracker or one of its ancestors is
  // exceeded.
  bool AnyLimitExceeded();

  // If this tracker has a limit, checks the limit and attempts to free up some memory if
  // the limit is exceeded by calling any added GC functions. Returns true if the limit is
  // exceeded after calling the GC functions. Returns false if there is no limit.
  bool LimitExceeded();

  // Like LimitExceeded() but may also return true if the soft memory limit is exceeded.
  // The greater the excess, the higher the chance that it returns true.
  // If score is not 0, then it is used to determine positive result.
  SoftLimitExceededResult SoftLimitExceeded(double* score);

  SoftLimitExceededResult SoftLimitExceeded(double score) {
    return SoftLimitExceeded(&score);
  }

  // Combines the semantics of AnyLimitExceeded() and SoftLimitExceeded().
  //
  // Note: if there's more than one soft limit defined, the probability of it being
  // exceeded in at least one tracker is much higher (as each soft limit check is an
  // independent event).
  SoftLimitExceededResult AnySoftLimitExceeded(double* score);

  SoftLimitExceededResult AnySoftLimitExceeded(double score) {
    return AnySoftLimitExceeded(&score);
  }

  // Returns the maximum consumption that can be made without exceeding the limit on
  // this tracker or any of its parents. Returns int64_t::max() if there are no
  // limits and a negative value if any limit is already exceeded.
  int64_t SpareCapacity() const;


  int64_t limit() const { return limit_; }
  bool has_limit() const { return limit_ >= 0; }
  const std::string& id() const { return id_; }

  // Returns the memory consumed in bytes.
  int64_t consumption() const {
    return consumption_.current_value();
  }

  int64_t GetUpdatedConsumption(bool force = false) {
    UpdateConsumption(force);
    return consumption();
  }

  // Note that if consumption_ is based on consumption_func_, this
  // will be the max value we've recorded in consumption(), not
  // necessarily the highest value consumption_func_ has ever
  // reached.
  int64_t peak_consumption() const { return consumption_.max_value(); }

  // Retrieve the parent tracker, or NULL If one is not set.
  std::shared_ptr<MemTracker> parent() const { return parent_; }

  // Add a function 'f' to be called if the limit is reached.
  // 'f' does not need to be thread-safe as long as it is added to only one MemTracker.
  // Note that 'f' must be valid for the lifetime of this MemTracker.
  void AddGarbageCollector(const std::shared_ptr<GarbageCollector>& gc) {
    std::lock_guard<simple_spinlock> lock(gc_mutex_);
    gcs_.push_back(gc);
  }

  // Logs the usage of this tracker and all of its children (recursively).
  std::string LogUsage(
      const std::string& prefix = "", size_t usage_threshold = 0, int indent = 0) const;

  void EnableLogging(bool enable, bool log_stack) {
    enable_logging_ = enable;
    log_stack_ = log_stack;
  }

  // Returns a textual representation of the tracker that is guaranteed to be
  // globally unique.
  std::string ToString() const;

  void SetMetricEntity(const scoped_refptr<MetricEntity>& metric_entity,
                       const std::string& name_suffix = std::string());
  scoped_refptr<MetricEntity> metric_entity() const;

  bool add_to_parent() const {
    return add_to_parent_;
  }

  void SetPollChildrenConsumptionFunctors(
      PollChildrenConsumptionFunctors poll_children_consumption_functors) {
    poll_children_consumption_functors_ = std::move(poll_children_consumption_functors);
  }

 private:
  bool CheckLimitExceeded() const {
    return limit_ >= 0 && limit_ < consumption();
  }

  // If consumption is higher than max_consumption, attempts to free memory by calling any
  // added GC functions.  Returns true if max_consumption is still exceeded. Takes
  // gc_lock. Updates metrics if initialized.
  bool GcMemory(int64_t max_consumption);

  // Called when the total release memory is larger than GC_RELEASE_SIZE.
  // TcMalloc holds onto released memory and very slowly (if ever) releases it back to
  // the OS. This is problematic since it is memory we are not constantly tracking which
  // can cause us to go way over mem limits.
  void GcTcmalloc();

  // Logs the stack of the current consume/release. Used for debugging only.
  void LogUpdate(bool is_consume, int64_t bytes) const;

  // Variant of CreateTracker() that:
  // 1. Must be called with a non-NULL parent, and
  // 2. Must be called with parent->child_trackers_lock_ held.
  std::shared_ptr<MemTracker> CreateChild(
      int64_t byte_limit,
      const std::string& id,
      ConsumptionFunctor consumption_functor,
      MayExist may_exist,
      AddToParent add_to_parent,
      CreateMetrics create_metrics);

  // Variant of FindTracker() that:
  // 1. Must be called with a non-NULL parent, and
  // 2. Must be called with parent->child_trackers_lock_ held.
  MemTrackerPtr FindChildUnlocked(const std::string& id);

  // Creates the root tracker.
  static void CreateRootTracker();

  // Size, in bytes, that is considered a large value for Release() (or Consume() with
  // a negative value). If tcmalloc is used, this can trigger it to GC.
  // A higher value will make us call into tcmalloc less often (and therefore more
  // efficient). A lower value will mean our memory overhead is lower.
  // TODO: this is a stopgap.
  static const int64_t GC_RELEASE_SIZE = 128 * 1024L * 1024L;

  const int64_t limit_;
  const int64_t soft_limit_;
  const std::string id_;
  const ConsumptionFunctor consumption_functor_;
  PollChildrenConsumptionFunctors poll_children_consumption_functors_;
  const std::string descr_;
  std::shared_ptr<MemTracker> parent_;
  CoarseMonoClock::time_point last_consumption_update_ = CoarseMonoClock::time_point::min();

  class TrackerMetrics;
  std::unique_ptr<TrackerMetrics> metrics_;

  HighWaterMark consumption_{0};

  // this tracker plus all of its ancestors
  std::vector<MemTracker*> all_trackers_;
  // all_trackers_ with valid limits
  std::vector<MemTracker*> limit_trackers_;

  // All the child trackers of this tracker. Used for error reporting and
  // listing only (i.e. updating the consumption of a parent tracker does not
  // update that of its children).
  // Note: This lock must never be taken on a parent before on a child.
  // It is taken in the opposite order during UnregisterFromParentIfNoChildren()
  mutable std::mutex child_trackers_mutex_;
  std::unordered_map<std::string, std::weak_ptr<MemTracker>> child_trackers_;

  simple_spinlock gc_mutex_;

  // Functions to call after the limit is reached to free memory.
  std::vector<std::weak_ptr<GarbageCollector>> gcs_;

  ThreadSafeRandom rand_;

  // If true, logs to INFO every consume/release called. Used for debugging.
  bool enable_logging_;

  // If true, log the stack as well.
  bool log_stack_;

  AddToParent add_to_parent_;
};

// An std::allocator that manipulates a MemTracker during allocation
// and deallocation.
template<typename T, typename Alloc = std::allocator<T> >
class MemTrackerAllocator : public Alloc {
 public:
  typedef typename Alloc::pointer pointer;
  typedef typename Alloc::const_pointer const_pointer;
  typedef typename Alloc::size_type size_type;

  explicit MemTrackerAllocator(std::shared_ptr<MemTracker> mem_tracker)
      : mem_tracker_(std::move(mem_tracker)) {}

  // This constructor is used for rebinding.
  template <typename U>
  MemTrackerAllocator(const MemTrackerAllocator<U>& allocator)
      : Alloc(allocator),
        mem_tracker_(allocator.mem_tracker()) {
  }

  ~MemTrackerAllocator() {
  }

  pointer allocate(size_type n, const_pointer hint = 0) {
    // Ideally we'd use TryConsume() here to enforce the tracker's limit.
    // However, that means throwing bad_alloc if the limit is exceeded, and
    // it's not clear that the rest of YB can handle that.
    mem_tracker_->Consume(n * sizeof(T));
    return Alloc::allocate(n, hint);
  }

  void deallocate(pointer p, size_type n) {
    Alloc::deallocate(p, n);
    mem_tracker_->Release(n * sizeof(T));
  }

  // This allows an allocator<T> to be used for a different type.
  template <class U>
  struct rebind {
    typedef MemTrackerAllocator<U, typename Alloc::template rebind<U>::other> other;
  };

  const std::shared_ptr<MemTracker>& mem_tracker() const { return mem_tracker_; }

 private:
  std::shared_ptr<MemTracker> mem_tracker_;
};

YB_STRONGLY_TYPED_BOOL(AlreadyConsumed);

// Convenience class that adds memory consumption to a tracker when declared,
// releasing it when the end of scope is reached.
class ScopedTrackedConsumption {
 public:
  ScopedTrackedConsumption() : consumption_(0) {}

  ScopedTrackedConsumption(MemTrackerPtr tracker,
                           int64_t to_consume,
                           AlreadyConsumed already_consumed = AlreadyConsumed::kFalse)
      : tracker_(std::move(tracker)), consumption_(to_consume) {
    DCHECK(*this);
    if (!already_consumed) {
      tracker_->Consume(consumption_);
    }
  }

  ScopedTrackedConsumption(const ScopedTrackedConsumption&) = delete;
  void operator=(const ScopedTrackedConsumption&) = delete;

  ScopedTrackedConsumption(ScopedTrackedConsumption&& rhs)
      : tracker_(std::move(rhs.tracker_)), consumption_(rhs.consumption_) {
    rhs.consumption_ = 0;
  }

  void operator=(ScopedTrackedConsumption&& rhs) {
    if (rhs) {
      DCHECK(!*this);
      tracker_ = std::move(rhs.tracker_);
      consumption_ = rhs.consumption_;
      rhs.consumption_ = 0;
    } else if (tracker_) {
      tracker_->Release(consumption_);
      tracker_ = nullptr;
    }
  }

  void Reset(int64_t new_consumption) {
    // Consume(-x) is the same as Release(x).
    tracker_->Consume(new_consumption - consumption_);
    consumption_ = new_consumption;
  }

  void Swap(ScopedTrackedConsumption* rhs) {
    std::swap(tracker_, rhs->tracker_);
    std::swap(consumption_, rhs->consumption_);
  }

  explicit operator bool() const {
    return tracker_ != nullptr;
  }

  ~ScopedTrackedConsumption() {
    if (tracker_) {
      tracker_->Release(consumption_);
    }
  }

  void Add(int64_t delta) {
    tracker_->Consume(delta);
    consumption_ += delta;
  }

  int64_t consumption() const { return consumption_; }

  const MemTrackerPtr& mem_tracker() { return tracker_; }

 private:
  MemTrackerPtr tracker_;
  int64_t consumption_;
};

template <class F>
int64_t AbsRelMemLimit(int64_t value, const F& f) {
  if (value < 0) {
    return f() * std::min<int64_t>(-value, 100) / 100;
  }
  if (value == 0) {
    return -1;
  }
  return value;
}

struct MemTrackerData {
  MemTrackerPtr tracker;
  // Depth of this tracker in hierarchy, i.e. root have depth = 0, his children 1 and so on.
  int depth;
  // Some mem trackers does not report their consumption to parent, so their consumption does not
  // participate in limit calculation or parent. We accumulate such consumption in field below.
  size_t consumption_excluded_from_ancestors = 0;
};

const MemTrackerData& CollectMemTrackerData(const MemTrackerPtr& tracker, int depth,
                                            std::vector<MemTrackerData>* output);

std::string DumpMemoryUsage();

// Checks whether it is ok to proceed with action having specified score under current memory
// conditions.
// Returns true when action should proceed, false if it should be rejected.
// score - score to reject action, should be in the range (0.0, 1.0],
// the higher the score - the greater probability that action should be rejected.
// Score 0.0 is a special value, meaning that random value should be picked for score.
//
// Suppose we have soft limit A and hard limit B. Where A < B.
// And current memory usage X.
//
// If X < A => this function always return true.
// If X >= B => this function always return false.
// If A < X < B, then we reject if used score > (B - X) / (B - A).
bool CheckMemoryPressureWithLogging(
    const MemTrackerPtr& mem_tracker, double score, const char* error_prefix);

} // namespace yb

#endif // YB_UTIL_MEM_TRACKER_H
