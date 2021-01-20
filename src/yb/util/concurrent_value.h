//
// Copyright (c) YugaByte, Inc.
//

#ifndef YB_UTIL_CONCURRENT_VALUE_H
#define YB_UTIL_CONCURRENT_VALUE_H

#include <atomic>
#include <mutex>
#include <thread>

#if defined(__APPLE__) && __clang_major__ < 8 || YB_ZAPCC
#define YB_CONCURRENT_VALUE_USE_BOOST_THREAD_SPECIFIC_PTR 1
#else
#define YB_CONCURRENT_VALUE_USE_BOOST_THREAD_SPECIFIC_PTR 0
#endif

#if YB_CONCURRENT_VALUE_USE_BOOST_THREAD_SPECIFIC_PTR
#include <boost/thread/tss.hpp>
#endif

#include "yb/util/logging.h"

namespace yb {

namespace internal {
typedef decltype(std::this_thread::get_id()) ThreadId;

const auto kNullThreadId = ThreadId();

// Tracks list of threads that is using URCU.
template<class T>
class ThreadList {
 public:
  typedef T Data;

  ~ThreadList() {
    const auto current_thread_id = std::this_thread::get_id();
    destructor_thread_id_.store(current_thread_id, std::memory_order_relaxed);

    // Check if the current thread has an associated URCUThreadData object that has not been
    // retired yet. We are doing it by traversing the linked list because the thread local
    // variable might have been destructed already.
    size_t desired_allocated_threads = 0;
    for (auto* p = head_.load(std::memory_order_acquire); p;) {
      if (p->owner.load(std::memory_order_acquire) == current_thread_id) {
        desired_allocated_threads = 1;
        break;
      }
      p = p->next.load(std::memory_order_acquire);
    }

    // Wait for all threads holding URCUThreadData objects, except maybe for this thread, to call
    // Retire(). This thread might have to do that later, depending on the destruction orders of
    // statics.
    while (allocated_.load(std::memory_order_acquire) != desired_allocated_threads) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // At this point no other threads should be touching the linked list. We are not enforcing that,
    // but we assume that if the ThreadList destructor is being called, the system has already
    // almost shut down.
    for (auto* p = head_.exchange(nullptr, std::memory_order_acquire); p;) {
      auto* n = p->next.load(std::memory_order_relaxed);
      if (p->owner.load(std::memory_order_relaxed) != current_thread_id) {
        // If the current thread has not called Retire() on its URCUThreadData object, then we will
        // defer deleting that object until Retire() is called.
        delete p;
      }
      p = n;
    }
  }

  Data* Alloc() {
    allocated_.fetch_add(1, std::memory_order_relaxed);
    Data* data;
    const auto current_thread_id = std::this_thread::get_id();

    // First, try to reuse a retired (non-active) HP record.
    for (data = head_.load(std::memory_order_acquire); data;
         data = data->next.load(std::memory_order_relaxed)) {
      auto old_value = kNullThreadId;
      if (data->owner.compare_exchange_strong(old_value,
                                              current_thread_id,
                                              std::memory_order_seq_cst,
                                              std::memory_order_relaxed)) {
        return data;
      }
    }

    data = new Data(current_thread_id);

    auto old_head = head_.load(std::memory_order_acquire);
    do {
      data->next.store(old_head, std::memory_order_relaxed);
    } while (!head_.compare_exchange_weak(old_head,
                                          data,
                                          std::memory_order_acq_rel,
                                          std::memory_order_acquire));

    return data;
  }

  void Retire(Data* data) {
    DCHECK_ONLY_NOTNULL(data);
    auto old_thread_id = data->owner.exchange(kNullThreadId, std::memory_order_acq_rel);
    allocated_.fetch_sub(1, std::memory_order_release);

    // Using relaxed memory order because we only need to delete the URCUThreadData object here
    // in case we set destructor_thread_id_ earlier on the same thread. If we are in a different
    // thread, then the thread id will not match anyway.
    if (old_thread_id == destructor_thread_id_.load(std::memory_order_relaxed)) {
      delete data;
    }
  }

  Data* Head(std::memory_order mo) const {
    return head_.load(mo);
  }

  static ThreadList<T>& Instance() {
    static ThreadList<T> result;
    return result;
  }

 private:
  ThreadList() {}

  std::atomic<Data*> head_{nullptr};
  std::atomic<size_t> allocated_{0};
  std::atomic<ThreadId> destructor_thread_id_{kNullThreadId};
};

// URCU data associated with thread.
struct URCUThreadData {
  std::atomic<uint32_t> access_control{0};
  std::atomic<URCUThreadData*> next{nullptr};
  std::atomic<ThreadId> owner;

  explicit URCUThreadData(ThreadId owner_) : owner(owner_) {}
};

constexpr uint32_t kControlBit = 0x80000000;
constexpr uint32_t kNestMask = kControlBit - 1;

// Userspace Read-copy-update.
// Full description https://en.wikipedia.org/wiki/Read-copy-update
// In computer science, read-copy-update (RCU) is a synchronization mechanism based on mutual
// exclusion. It is used when performance of reads is crucial and is an example of space-time
// tradeoff, enabling fast operations at the cost of more space.
//
// Read-copy-update allows multiple threads to efficiently read from shared memory by deferring
// updates after pre-existing reads to a later time while simultaneously marking the data,
// ensuring new readers will read the updated data. This makes all readers proceed as if there
// were no synchronization involved, hence they will be fast, but also making updates more
// difficult.
class URCU {
 public:
  URCU() {}

  URCU(const URCU&) = delete;
  void operator=(const URCU&) = delete;

  void AccessLock() {
    auto* data = DCHECK_NOTNULL(ThreadData());

    uint32_t tmp = data->access_control.load(std::memory_order_relaxed);
    if ((tmp & kNestMask) == 0) {
      data->access_control.store(global_control_word_.load(std::memory_order_relaxed),
          std::memory_order_relaxed);

      std::atomic_thread_fence(std::memory_order_seq_cst);
    } else {
      // nested lock
      data->access_control.store(tmp + 1, std::memory_order_relaxed);
    }
  }

  void AccessUnlock() {
    auto* data = DCHECK_NOTNULL(ThreadData());

    uint32_t tmp = data->access_control.load(std::memory_order_relaxed);
    CHECK_GT(tmp & kNestMask, 0);

    data->access_control.store(tmp - 1, std::memory_order_release);
  }

  void Synchronize() {
    std::lock_guard<std::mutex> lock(mutex_);
    FlipAndWait();
    FlipAndWait();
  }

 private:
  URCUThreadData* ThreadData() {
    auto result = data_.get();
    if (!result) {
      data_.reset(result = ThreadList<URCUThreadData>::Instance().Alloc());
    }
    return result;
  }

  void FlipAndWait() {
    global_control_word_.fetch_xor(kControlBit, std::memory_order_seq_cst);

    for (auto* data = ThreadList<URCUThreadData>::Instance().Head(std::memory_order_acquire);
         data;
         data = data->next.load(std::memory_order_acquire)) {
      while (data->owner.load(std::memory_order_acquire) != kNullThreadId &&
             CheckGracePeriod(data)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        std::atomic_thread_fence(std::memory_order_seq_cst);
      }
    }
  }

  bool CheckGracePeriod(URCUThreadData* data) {
    const uint32_t v = data->access_control.load(std::memory_order_acquire);
    return (v & kNestMask) &&
           ((v ^ global_control_word_.load(std::memory_order_relaxed)) & ~kNestMask);
  }

  std::atomic <uint32_t> global_control_word_{1};
  std::mutex mutex_;
#if YB_CONCURRENT_VALUE_USE_BOOST_THREAD_SPECIFIC_PTR
  static void CleanupThreadData(URCUThreadData* data) {
    ThreadList<URCUThreadData>::Instance().Retire(data);
  }

  static boost::thread_specific_ptr<URCUThreadData> data_;
#else
  struct CleanupThreadData {
    void operator()(URCUThreadData* data) {
      ThreadList<URCUThreadData>::Instance().Retire(data);
    }
  };

  static thread_local std::unique_ptr<URCUThreadData, CleanupThreadData> data_;
#endif
};

// Reference to concurrent value. Provides read access to concurrent value.
// Should have short life time period.
template<class T>
class ConcurrentValueReference {
 public:
  explicit ConcurrentValueReference(std::atomic<T*>* value, URCU* urcu)
      : urcu_(urcu) {
    urcu_->AccessLock();
    value_ = value->load(std::memory_order_acquire);
  }

  ~ConcurrentValueReference() {
    if (urcu_) {
      urcu_->AccessUnlock();
    }
  }

  ConcurrentValueReference(const ConcurrentValueReference&) = delete;
  void operator=(const ConcurrentValueReference&) = delete;

  ConcurrentValueReference(ConcurrentValueReference&& rhs)
      : value_(rhs.value_), urcu_(rhs.urcu_) {
    rhs.urcu_ = nullptr;
  }

  const T& operator*() const {
    return get();
  }

  const T* operator->() const {
    return &get();
  }

  const T& get() const {
    DCHECK_ONLY_NOTNULL(urcu_);
    return *value_;
  }
 private:
  const T* value_;
  URCU* urcu_;
};

// Concurrent value is used for cases when some object has a lot of reads with small amount of
// writes.
template<class T>
class ConcurrentValue {
 public:
  template<class... Args>
  explicit ConcurrentValue(Args&&... args) : value_(new T(std::forward<Args>(args)...)) {}

  ~ConcurrentValue() {
    delete value_.load(std::memory_order_relaxed);
  }

  ConcurrentValueReference<T> get() {
    return ConcurrentValueReference<T>(&value_, &urcu_);
  }

  template<class... Args>
  void Emplace(Args&& ... args) {
    DoSet(new T(std::forward<Args>(args)...));
  }

  void Set(const T& t) {
    DoSet(new T(t));
  }

  void Set(T&& t) {
    DoSet(new T(std::move(t)));
  }

 private:
  void DoSet(T* new_value) {
    auto* old_value = value_.exchange(new_value, std::memory_order_acq_rel);
    urcu_.Synchronize();
    delete old_value;
  }

  std::atomic<T*> value_ = {nullptr};
  URCU urcu_;
};

} // namespace internal

using internal::ConcurrentValue;

} // namespace yb

#endif // YB_UTIL_CONCURRENT_VALUE_H
