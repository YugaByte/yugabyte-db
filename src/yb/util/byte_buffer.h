// Copyright (c) YugaByte, Inc.
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

#ifndef YB_UTIL_BYTE_BUFFER_H
#define YB_UTIL_BYTE_BUFFER_H

#include <cstddef>
#include <string>

#include <boost/container_hash/extensions.hpp>

#include "yb/util/slice.h"

namespace yb {

// Utility class to store arbitrary amount of byte with inplace buffer of specified size.
template <size_t SmallLen>
class ByteBuffer {
 public:
  static_assert(SmallLen >= sizeof(void*), "Too small buffer");

  ByteBuffer() : size_(0) {}

  explicit ByteBuffer(const std::string& str) {
    Assign(str.c_str(), str.size());
  }

  void operator=(const std::string& str) {
    Assign(str.c_str(), str.size());
  }

  explicit ByteBuffer(const Slice& slice) {
    Assign(slice.cdata(), slice.cend());
  }

  void operator=(const Slice& slice) {
    Assign(slice.cdata(), slice.cend());
  }

  ByteBuffer(const ByteBuffer<SmallLen>& rhs) {
    Assign(rhs.ptr(), rhs.size_);
  }

  void operator=(const ByteBuffer<SmallLen>& rhs) {
    Assign(rhs.ptr(), rhs.size_);
  }

  ByteBuffer(ByteBuffer<SmallLen>&& rhs) {
    if (!rhs.big()) {
      memcpy(small_buffer_, rhs.small_buffer_, rhs.size_);
    } else {
      capacity_ = rhs.capacity_;
      big_buffer_ = rhs.big_buffer_;
      rhs.capacity_ = SmallLen;
    }

    size_ = rhs.size_;
    rhs.size_ = 0;
  }

  void operator=(ByteBuffer<SmallLen>&& rhs) {
    if (!rhs.big()) {
      memcpy(ptr(), rhs.small_buffer_, rhs.size_);
    } else {
      if (big()) {
        free(big_buffer_);
      }
      capacity_ = rhs.capacity_;
      big_buffer_ = rhs.big_buffer_;
      rhs.capacity_ = SmallLen;
    }

    size_ = rhs.size_;
    rhs.size_ = 0;
  }

  ~ByteBuffer() {
    if (big()) {
      free(big_buffer_);
    }
  }

  bool empty() const {
    return size_ == 0;
  }

  size_t size() const {
    return size_;
  }

  void Clear() {
    size_ = 0;
  }

  char& Back() {
    return ptr()[size_ - 1];
  }

  char Back() const {
    return ptr()[size_ - 1];
  }

  char& operator[](size_t len) {
    return ptr()[len];
  }

  char operator[](size_t len) const {
    return ptr()[len];
  }

  void PopBack() {
    --size_;
  }

  void Truncate(size_t new_size) {
    size_ = new_size;
  }

  void Assign(const Slice& slice) {
    Assign(slice.cdata(), slice.cend());
  }

  void Assign(const char* a, const char* b) {
    Assign(a, b - a);
  }

  void Assign(const char* a, size_t size) {
    DoAppend(0, a, size);
  }

  void Append(const Slice& slice) {
    Append(slice.cdata(), slice.cend());
  }

  template <size_t OtherSmallLen>
  void Append(const ByteBuffer<OtherSmallLen>& rhs) {
    Append(rhs.ptr(), rhs.size_);
  }

  void Append(const char* a, const char* b) {
    Append(a, b - a);
  }

  void Append(const char* a, size_t size) {
    DoAppend(size_, a, size);
  }

  void Reserve(size_t capacity) {
    EnsureCapacity(capacity, size_);
  }

  void PushBack(char ch) {
    *(EnsureCapacity(size_ + 1, size_) + size_) = ch;
    ++size_;
  }

  std::string ToString() const {
    return AsSlice().ToBuffer();
  }

  Slice AsSlice() const {
    return Slice(ptr(), size_);
  }

  // STL container compatibility
  void clear() {
    Clear();
  }

  template <class... Args>
  void append(Args&&... args) {
    Append(std::forward<Args>(args)...);
  }

  template <class... Args>
  void assign(Args&&... args) {
    Assign(std::forward<Args>(args)...);
  }

  void reserve(size_t capacity) {
    Reserve(capacity);
  }

  void push_back(char ch) {
    PushBack(ch);
  }

  char& back() {
    return Back();
  }

  char back() const {
    return Back();
  }

  void pop_back() {
    PopBack();
  }

 private:
  void DoAppend(size_t keep_size, const char* a, size_t len) {
    size_t new_size = keep_size + len;
    memcpy(EnsureCapacity(new_size, keep_size) + keep_size, a, len);
    size_ = new_size;
  }

  // Ensures that buffer could contain at least capacity bytes.
  // In case of relocation, keep_size bytes will be copied.
  char* EnsureCapacity(size_t capacity, size_t keep_size) {
    if (capacity <= capacity_) {
      return ptr();
    }

    bool was_big = big();
    while ((capacity_ <<= 1ULL) < capacity) {}
    char* new_buffer = static_cast<char*>(malloc(capacity_));
    char*& big_buffer = big_buffer_;
    if (was_big) {
      memcpy(new_buffer, big_buffer, keep_size);
      free(big_buffer);
    } else {
      memcpy(new_buffer, small_buffer_, keep_size);
    }
    return big_buffer = new_buffer;
  }

  bool big() const {
    return capacity_ > SmallLen;
  }

  char* ptr() {
    return !big() ? small_buffer_ : big_buffer_;
  }

  const char* ptr() const {
    return !big() ? small_buffer_ : big_buffer_;
  }

  size_t capacity_ = SmallLen;
  size_t size_;
  union {
    char small_buffer_[SmallLen];
    char* big_buffer_;
  };
};

template <size_t SmallLenLhs, size_t SmallLenRhs>
bool operator<(const ByteBuffer<SmallLenLhs>& lhs, const ByteBuffer<SmallLenRhs>& rhs) {
  return lhs.AsSlice().compare(rhs.AsSlice()) < 0;
}

template <size_t SmallLenLhs, size_t SmallLenRhs>
bool operator==(const ByteBuffer<SmallLenLhs>& lhs, const ByteBuffer<SmallLenRhs>& rhs) {
  return lhs.AsSlice() == rhs.AsSlice();
}

struct ByteBufferHash {
  typedef std::size_t result_type;

  template <size_t SmallLen>
  result_type operator()(const ByteBuffer<SmallLen>& buffer) const {
    return buffer.AsSlice().hash();
  }
};

} // namespace yb

#endif // YB_UTIL_BYTE_BUFFER_H
