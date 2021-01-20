// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
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

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fts.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#include <set>
#include <vector>
#include "yb/util/status.h"

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <sys/sysctl.h>
#else
#include <linux/falloc.h>
#include <sys/sysinfo.h>
#endif  // defined(__APPLE__)
#include <sys/resource.h>

#include <glog/logging.h>

#include "yb/gutil/atomicops.h"
#include "yb/gutil/bind.h"
#include "yb/gutil/callback.h"
#include "yb/gutil/map-util.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/util/alignment.h"
#include "yb/util/debug/trace_event.h"
#include "yb/util/env.h"
#include "yb/util/errno.h"
#include "yb/util/file_system_posix.h"
#include "yb/util/flag_tags.h"
#include "yb/util/locks.h"
#include "yb/util/logging.h"
#include "yb/util/malloc.h"
#include "yb/util/monotime.h"
#include "yb/util/path_util.h"
#include "yb/util/slice.h"
#include "yb/util/stopwatch.h"
#include "yb/util/thread_restrictions.h"

// Copied from falloc.h. Useful for older kernels that lack support for
// hole punching; fallocate(2) will return EOPNOTSUPP.
#ifndef FALLOC_FL_KEEP_SIZE
#define FALLOC_FL_KEEP_SIZE 0x01 /* default is extend size */
#endif
#ifndef FALLOC_FL_PUNCH_HOLE
#define FALLOC_FL_PUNCH_HOLE  0x02 /* de-allocates range */
#endif

// For platforms without fdatasync (like OS X)
#ifndef fdatasync
#define fdatasync fsync
#endif

// For platforms without unlocked_stdio (like OS X)
#ifndef fread_unlocked
#define fread_unlocked fread
#endif

// See KUDU-588 for details.
DEFINE_bool(writable_file_use_fsync, false,
            "Use fsync(2) instead of fdatasync(2) for synchronizing dirty "
            "data to disk.");
TAG_FLAG(writable_file_use_fsync, advanced);

DEFINE_bool(suicide_on_eio, true,
            "Kill the process if an I/O operation results in EIO");
TAG_FLAG(suicide_on_eio, advanced);

#ifdef __APPLE__
// Never fsync on Mac OS X as we are getting many slow fsync errors in Jenkins and the fsync
// implementation is very different in production (on Linux) anyway.
#define FLAGS_never_fsync_default true
#else
#define FLAGS_never_fsync_default false
#endif

DEFINE_bool(never_fsync, FLAGS_never_fsync_default,
            "Never fsync() anything to disk. This is used by certain test cases to "
            "speed up runtime. This is very unsafe to use in production.");

TAG_FLAG(never_fsync, advanced);
TAG_FLAG(never_fsync, unsafe);

DEFINE_int32(o_direct_block_size_bytes, 4096,
             "Size of the block to use when flag durable_wal_write is set.");
TAG_FLAG(o_direct_block_size_bytes, advanced);

DEFINE_int32(o_direct_block_alignment_bytes, 4096,
             "Alignment (in bytes) for blocks used for O_DIRECT operations.");
TAG_FLAG(o_direct_block_alignment_bytes, advanced);

DEFINE_test_flag(bool, simulate_fs_without_fallocate, false,
    "If true, the system simulates a file system that doesn't support fallocate.");

DEFINE_test_flag(int64, simulate_free_space_bytes, -1,
    "If a non-negative value, GetFreeSpaceBytes will return the specified value.");

using base::subtle::Atomic64;
using base::subtle::Barrier_AtomicIncrement;
using std::vector;
using strings::Substitute;

static __thread uint64_t thread_local_id;
static Atomic64 cur_thread_local_id_;

namespace yb {

Status IOError(const std::string& context, int err_number, const char* file, int line) {
  Errno err(err_number);
  switch (err_number) {
    case ENOENT:
      return Status(Status::kNotFound, file, line, context, err);
    case EEXIST:
      return Status(Status::kAlreadyPresent, file, line, context, err);
    case EOPNOTSUPP:
      return Status(Status::kNotSupported, file, line, context, err);
    case EIO:
      if (FLAGS_suicide_on_eio) {
        // TODO: This is very, very coarse-grained. A more comprehensive
        // approach is described in KUDU-616.
        LOG(FATAL) << "Fatal I/O error, context: " << context;
      }
  }
  return Status(Status::kIOError, file, line, context, err);
}

namespace {

#if defined(__APPLE__)
// Simulates Linux's fallocate file preallocation API on OS X.
int fallocate(int fd, int mode, off_t offset, off_t len) {
  CHECK_EQ(mode, 0);
  off_t size = offset + len;

  struct stat stat;
  int ret = fstat(fd, &stat);
  if (ret < 0) {
    return ret;
  }

  if (stat.st_blocks * 512 < size) {
    // The offset field seems to have no effect; the file is always allocated
    // with space from 0 to the size. This is probably because OS X does not
    // support sparse files.
    fstore_t store = {F_ALLOCATECONTIG, F_PEOFPOSMODE, 0, size};
    if (fcntl(fd, F_PREALLOCATE, &store) < 0) {
      LOG(INFO) << "Unable to allocate contiguous disk space, attempting non-contiguous allocation";
      store.fst_flags = F_ALLOCATEALL;
      ret = fcntl(fd, F_PREALLOCATE, &store);
      if (ret < 0) {
        return ret;
      }
    }
  }

  if (stat.st_size < size) {
    // fcntl does not change the file size, so set it if necessary.
    return ftruncate(fd, size);
  }
  return 0;
}
#endif

// Close file descriptor when object goes out of scope.
class ScopedFdCloser {
 public:
  explicit ScopedFdCloser(int fd)
    : fd_(fd) {
  }

  ~ScopedFdCloser() {
    ThreadRestrictions::AssertIOAllowed();
    int err = ::close(fd_);
    if (PREDICT_FALSE(err != 0)) {
      PLOG(WARNING) << "Failed to close fd " << fd_;
    }
  }

 private:
  int fd_;
};

#define STATUS_IO_ERROR(context, err_number) IOError(context, err_number, __FILE__, __LINE__)

static Status DoSync(int fd, const string& filename) {
  ThreadRestrictions::AssertIOAllowed();
  if (FLAGS_never_fsync) return Status::OK();
  if (FLAGS_writable_file_use_fsync) {
    if (fsync(fd) < 0) {
      return STATUS_IO_ERROR(filename, errno);
    }
  } else {
    if (fdatasync(fd) < 0) {
      return STATUS_IO_ERROR(filename, errno);
    }
  }
  return Status::OK();
}

static Status DoOpen(const string& filename, Env::CreateMode mode, int* fd, int extra_flags = 0) {
  ThreadRestrictions::AssertIOAllowed();
  int flags = O_RDWR;
  switch (mode) {
    case Env::CREATE_IF_NON_EXISTING_TRUNCATE:
      flags |= O_CREAT | O_TRUNC;
      break;
    case Env::CREATE_NON_EXISTING:
      flags |= O_CREAT | O_EXCL;
      break;
    case Env::OPEN_EXISTING:
      break;
    default:
      return STATUS(NotSupported, Substitute("Unknown create mode $0", mode));
  }

  const int f = open(filename.c_str(), flags | extra_flags, 0644);
  if (f < 0) {
    return STATUS_IO_ERROR(filename, errno);
  }
  *fd = f;
  return Status::OK();
}

template <class Extractor>
Result<uint64_t> GetFileStat(const std::string& fname, const char* event, Extractor extractor) {
  TRACE_EVENT1("io", event, "path", fname);
  ThreadRestrictions::AssertIOAllowed();
  struct stat sbuf;
  if (stat(fname.c_str(), &sbuf) != 0) {
    return STATUS_IO_ERROR(fname, errno);
  }
  return extractor(sbuf);
}

// Use non-memory mapped POSIX files to write data to a file.
//
// TODO (perf) investigate zeroing a pre-allocated allocated area in
// order to further improve Sync() performance.
class PosixWritableFile : public WritableFile {
 public:
  PosixWritableFile(std::string fname, int fd, uint64_t file_size,
                    bool sync_on_close)
      : filename_(std::move(fname)),
        fd_(fd),
        sync_on_close_(sync_on_close),
        filesize_(file_size),
        pre_allocated_size_(0),
        pending_sync_(false) {}

  ~PosixWritableFile() {
    if (fd_ >= 0) {
      WARN_NOT_OK(Close(), "Failed to close " + filename_);
    }
  }

  Status Append(const Slice& data) override {
    vector<Slice> data_vector;
    data_vector.push_back(data);
    return AppendVector(data_vector);
  }

  Status AppendVector(const vector<Slice>& data_vector) override {
    ThreadRestrictions::AssertIOAllowed();
    static const size_t kIovMaxElements = IOV_MAX;

    Status s;
    for (size_t i = 0; i < data_vector.size() && s.ok(); i += kIovMaxElements) {
      size_t n = std::min(data_vector.size() - i, kIovMaxElements);
      s = DoWritev(data_vector, i, n);
    }

    pending_sync_ = true;
    return s;
  }

  Status PreAllocate(uint64_t size) override {
    TRACE_EVENT1("io", "PosixWritableFile::PreAllocate", "path", filename_);
    ThreadRestrictions::AssertIOAllowed();
    uint64_t offset = std::max(filesize_, pre_allocated_size_);
    if (PREDICT_FALSE(FLAGS_TEST_simulate_fs_without_fallocate)) {
      YB_LOG_FIRST_N(WARNING, 1) << "Simulating a filesystem without fallocate() support";
      return Status::OK();
    }
    if (fallocate(fd_, 0, offset, size) < 0) {
      if (errno == EOPNOTSUPP) {
        YB_LOG_FIRST_N(WARNING, 1) << "The filesystem does not support fallocate().";
      } else if (errno == ENOSYS) {
        YB_LOG_FIRST_N(WARNING, 1) << "The kernel does not implement fallocate().";
      } else {
        return STATUS_IO_ERROR(filename_, errno);
      }
      // We don't want to modify pre_allocated_size_ since nothing was pre-allocated.
      return Status::OK();
    }
    pre_allocated_size_ = offset + size;
    return Status::OK();
  }

  Status Close() override {
    TRACE_EVENT1("io", "PosixWritableFile::Close", "path", filename_);
    ThreadRestrictions::AssertIOAllowed();
    Status s;

    // If we've allocated more space than we used, truncate to the
    // actual size of the file and perform Sync().
    if (filesize_ < pre_allocated_size_) {
      if (ftruncate(fd_, filesize_) < 0) {
        s = STATUS_IO_ERROR(filename_, errno);
        pending_sync_ = true;
      }
    }

    if (sync_on_close_) {
      Status sync_status = Sync();
      if (!sync_status.ok()) {
        LOG(ERROR) << "Unable to Sync " << filename_ << ": " << sync_status.ToString();
        if (s.ok()) {
          s = sync_status;
        }
      }
    }

    if (close(fd_) < 0) {
      if (s.ok()) {
        s = STATUS_IO_ERROR(filename_, errno);
      }
    }

    fd_ = -1;
    return s;
  }

  Status Flush(FlushMode mode) override {
    TRACE_EVENT1("io", "PosixWritableFile::Flush", "path", filename_);
    ThreadRestrictions::AssertIOAllowed();
#if defined(__linux__)
    int flags = SYNC_FILE_RANGE_WRITE;
    if (mode == FLUSH_SYNC) {
      flags |= SYNC_FILE_RANGE_WAIT_AFTER;
    }
    if (sync_file_range(fd_, 0, 0, flags) < 0) {
      return STATUS_IO_ERROR(filename_, errno);
    }
#else
    if (fsync(fd_) < 0) {
      return STATUS_IO_ERROR(filename_, errno);
    }
#endif
    return Status::OK();
  }

  Status Sync() override {
    TRACE_EVENT1("io", "PosixWritableFile::Sync", "path", filename_);
    ThreadRestrictions::AssertIOAllowed();
    LOG_SLOW_EXECUTION(WARNING, 1000, Substitute("sync call for $0", filename_)) {
      if (pending_sync_) {
        pending_sync_ = false;
        RETURN_NOT_OK(DoSync(fd_, filename_));
      }
    }
    return Status::OK();
  }

  uint64_t Size() const override {
    return filesize_;
  }

  const string& filename() const override { return filename_; }

 protected:
    const std::string filename_;
    int fd_;
    bool sync_on_close_;
    uint64_t filesize_;
    uint64_t pre_allocated_size_;
    bool pending_sync_;

 private:

  Status DoWritev(const vector<Slice>& data_vector,
                  size_t offset, size_t n) {
    ThreadRestrictions::AssertIOAllowed();
#if defined(__linux__)
    DCHECK_LE(n, IOV_MAX);

    struct iovec iov[n];
    size_t j = 0;
    size_t nbytes = 0;

    for (size_t i = offset; i < offset + n; i++) {
      const Slice& data = data_vector[i];
      iov[j].iov_base = const_cast<uint8_t*>(data.data());
      iov[j].iov_len = data.size();
      nbytes += data.size();
      ++j;
    }

    ssize_t written = pwritev(fd_, iov, n, filesize_);

    if (PREDICT_FALSE(written == -1)) {
      int err = errno;
      return STATUS_IO_ERROR(filename_, err);
    }

    filesize_ += written;

    if (PREDICT_FALSE(written != nbytes)) {
      return STATUS(IOError,
          Substitute("pwritev error: expected to write $0 bytes, wrote $1 bytes instead",
                     nbytes, written));
    }
#else
    for (size_t i = offset; i < offset + n; i++) {
      const Slice& data = data_vector[i];
      ssize_t written = pwrite(fd_, data.data(), data.size(), filesize_);
      if (PREDICT_FALSE(written == -1)) {
        int err = errno;
        return STATUS_IO_ERROR("pwrite error", err);
      }

      filesize_ += written;

      if (PREDICT_FALSE(written != data.size())) {
        return STATUS(IOError,
            Substitute("pwrite error: expected to write $0 bytes, wrote $1 bytes instead",
                       data.size(), written));
      }
    }
#endif

    return Status::OK();
  }
};

#if defined(__linux__)
class PosixDirectIOWritableFile final : public PosixWritableFile {
 public:
  PosixDirectIOWritableFile(const std::string &fname, int fd, uint64_t file_size,
                            bool sync_on_close)
      : PosixWritableFile(fname, fd, file_size, false /* sync_on_close */) {

    if (file_size != 0) {
      // For now, we don't support appending to an already existing file (of non-zero size).
      // TODO(hector): If file size is not a multiple of block_size_, read the
      // last aligned block.
      LOG(FATAL) << "file size != 0";
    }

    next_write_offset_ = 0;
    last_block_used_bytes_ = 0;
    block_size_ = FLAGS_o_direct_block_size_bytes;
    last_block_idx_ = 0;
    has_new_data_ = false;
    real_size_ = 0;
    CHECK_GE(block_size_, 512);
  }

  ~PosixDirectIOWritableFile() {
    if (fd_ >= 0) {
      WARN_NOT_OK(Close(), "Failed to close " + filename_);
    }
  }

  Status Append(const Slice &const_data_slice) override {
    ThreadRestrictions::AssertIOAllowed();
    Slice data_slice = const_data_slice;

    while (data_slice.size() > 0) {
      size_t max_data = IOV_MAX * block_size_ - BufferedByteCount();
      CHECK_GT(IOV_MAX, 0);
      CHECK_GT(IOV_MAX * block_size_, BufferedByteCount());
      CHECK_GT(max_data, 0);
      const auto data = Slice(data_slice.data(), std::min(data_slice.size(), max_data));

      RETURN_NOT_OK(MaybeAllocateMemory(data.size()));
      RETURN_NOT_OK(WriteToBuffer(data));

      if (data_slice.size() >= max_data) {
        data_slice.remove_prefix(max_data);
        RETURN_NOT_OK(Sync());
      } else {
        break;
      }
    }
    real_size_ += const_data_slice.size();
    return Status::OK();
  }

  Status AppendVector(const vector<Slice> &data_vector) override {
    ThreadRestrictions::AssertIOAllowed();
    for (auto const &slice : data_vector) {
      RETURN_NOT_OK(Append(slice));
    }
    return Status::OK();
  }

  Status Close() override {
    TRACE_EVENT1("io", "PosixDirectIOWritableFile::Close", "path", filename_);
    ThreadRestrictions::AssertIOAllowed();
    RETURN_NOT_OK(Sync());
    LOG(INFO) << "Closing file " << filename_ << " with " << block_ptr_vec_.size() << " blocks";
    off_t fsize;
    fsize = lseek(fd_, 0, SEEK_END);
    CHECK_EQ(fsize, std::max(filesize_, pre_allocated_size_));

    if (real_size_ < filesize_ || real_size_ < pre_allocated_size_) {
      LOG(INFO) << filename_ << ": Truncating file from size: " << filesize_
                << " to size: " << real_size_
                << ". Preallocated size: " << pre_allocated_size_;
      if (ftruncate(fd_, real_size_) != 0) {
        return STATUS_IO_ERROR(filename_, errno);
      }
    }

    if (close(fd_) < 0) {
      return STATUS_IO_ERROR(filename_, errno);
    }

    fd_ = -1;
    return Status::OK();
  }

  Status Flush(FlushMode mode) override {
    ThreadRestrictions::AssertIOAllowed();
    return Sync();
  }

  Status Sync() override {
    ThreadRestrictions::AssertIOAllowed();
    return DoWrite();
  }

  uint64_t Size() const override {
    return real_size_;
  }

 private:
  // The number of bytes buffered so far (that will be written out as part of the next write).
  size_t BufferedByteCount() {
    return last_block_idx_ * block_size_ + last_block_used_bytes_;
  }

  Status WriteToBuffer(Slice data) {
    auto last_block_used_bytes = last_block_used_bytes_;
    auto last_block_idx = last_block_idx_;
    auto total_bytes_cached = BufferedByteCount();
    auto request_size = data.size();

    CHECK_GT(data.size(), 0);
    // Used only for the first block. Reset to 0 after the first memcpy.
    size_t block_offset = last_block_used_bytes_;
    auto i = last_block_idx_;
    if (last_block_used_bytes_ == block_size_) {
      // Start writing in a new block if the last block is full.
      i++;
      block_offset = 0;
    }
    while (data.size() > 0) {
      last_block_used_bytes_ = block_size_;
      size_t block_data_size = std::min(data.size(), block_size_ - block_offset);
      if (block_data_size + block_offset < block_size_) {
        // Writing the last block.
        last_block_used_bytes_ = block_data_size + block_offset;
        memset(&block_ptr_vec_[i].get()[last_block_used_bytes_], 0,
               block_size_ - last_block_used_bytes_);
      }
      CHECK(i < block_ptr_vec_.size());
      memcpy(&block_ptr_vec_[i].get()[block_offset], data.data(), block_data_size);
      block_offset = 0;
      data.remove_prefix(block_data_size);
      last_block_idx_ = i++;
      has_new_data_ = true;
    }

    CHECK_GE(last_block_idx_, last_block_idx);
    if (last_block_idx_ == last_block_idx) {
      CHECK_GE(last_block_used_bytes_, last_block_used_bytes);
    }
    CHECK_EQ(BufferedByteCount(), total_bytes_cached + request_size);

    return Status::OK();
  }

  Status DoWrite() {
    if (!has_new_data_) {
      return Status::OK();
    }
    CHECK_LE(last_block_used_bytes_, block_size_);
    CHECK_LT(last_block_idx_, block_ptr_vec_.size());
    auto blocks_to_write = last_block_idx_ + 1;
    CHECK_LE(blocks_to_write, IOV_MAX);

    struct iovec iov[blocks_to_write];
    for (int j = 0; j < blocks_to_write; j++) {
      iov[j].iov_base = block_ptr_vec_[j].get();
      iov[j].iov_len = block_size_;
    }
    auto bytes_to_write = blocks_to_write * block_size_;
    ssize_t written = pwritev(fd_, iov, blocks_to_write, next_write_offset_);

    if (PREDICT_FALSE(written == -1)) {
      int err = errno;
      return STATUS_IO_ERROR(filename_, err);
    }

    if (PREDICT_FALSE(written != bytes_to_write)) {
      return STATUS(IOError,
                    Substitute("pwritev error: expected to write $0 bytes, wrote $1 bytes instead",
                               bytes_to_write, written));
    }

    filesize_ = next_write_offset_ + written;
    CHECK_EQ(filesize_, align_up(filesize_, block_size_));

    next_write_offset_ = filesize_;

    if (last_block_used_bytes_ != block_size_) {
      // Next write will happen at filesize_ - block_size_ offset in the file if the last block is
      // not full.
      next_write_offset_ -= block_size_;

      // Since the last block is only partially full, make it the first block so we can append to
      // it in the next call.
      if (last_block_idx_ > 0) {
        std::swap(block_ptr_vec_[0], block_ptr_vec_[last_block_idx_]);
      }

    } else {
      last_block_used_bytes_ = 0;
    }
    last_block_idx_ = 0;
    has_new_data_ = false;
    return Status::OK();
  }

  Status MaybeAllocateMemory(size_t data_size) {
    auto buffered_data_size = last_block_idx_ * block_size_ + last_block_used_bytes_;
    auto bytes_to_write = align_up(buffered_data_size + data_size, block_size_);
    auto blocks_to_write = bytes_to_write / block_size_;

    if (blocks_to_write > block_ptr_vec_.size()) {
      auto nblocks = blocks_to_write - block_ptr_vec_.size();
      for (auto i = 0; i < nblocks; i++) {
        void *temp_buf = nullptr;
        auto err = posix_memalign(&temp_buf, FLAGS_o_direct_block_alignment_bytes, block_size_);
        if (err) {
          return STATUS(RuntimeError, "Unable to allocate memory", Errno(err));
        }

        uint8_t *start = static_cast<uint8_t *>(temp_buf);
        block_ptr_vec_.push_back(std::shared_ptr<uint8_t>(start, [](uint8_t *p) { free(p); }));
      }

      CHECK_EQ(block_ptr_vec_.size() * block_size_, bytes_to_write);
    }
    return Status::OK();
  }

  size_t next_write_offset_;
  vector<std::shared_ptr<uint8_t>> block_ptr_vec_;
  size_t last_block_used_bytes_;
  size_t last_block_idx_;
  int block_size_;
  bool has_new_data_;
  size_t real_size_;
};
#endif

class PosixRWFile final : public RWFile {
// is not employed.
 public:
  PosixRWFile(string fname, int fd, bool sync_on_close)
      : filename_(std::move(fname)),
        fd_(fd),
        sync_on_close_(sync_on_close),
        pending_sync_(false) {}

  ~PosixRWFile() {
    if (fd_ >= 0) {
      // Virtual method call in destructor.
      WARN_NOT_OK(Close(), "Failed to close " + filename_);
    }
  }

  virtual Status Read(uint64_t offset, size_t length,
                      Slice* result, uint8_t* scratch) const override {
    ThreadRestrictions::AssertIOAllowed();
    int rem = length;
    uint8_t* dst = scratch;
    while (rem > 0) {
      ssize_t r = pread(fd_, dst, rem, offset);
      if (r < 0) {
        // An error: return a non-ok status.
        return STATUS_IO_ERROR(filename_, errno);
      }
      Slice this_result(dst, r);
      DCHECK_LE(this_result.size(), rem);
      if (this_result.size() == 0) {
        // EOF
        return STATUS_FORMAT(IOError, "EOF trying to read $0 bytes at offset $1", length, offset);
      }
      dst += this_result.size();
      rem -= this_result.size();
      offset += this_result.size();
    }
    DCHECK_EQ(0, rem);
    *result = Slice(scratch, length);
    return Status::OK();
  }

  Status Write(uint64_t offset, const Slice& data) override {
    ThreadRestrictions::AssertIOAllowed();
    ssize_t written = pwrite(fd_, data.data(), data.size(), offset);

    if (PREDICT_FALSE(written == -1)) {
      int err = errno;
      return STATUS_IO_ERROR(filename_, err);
    }

    if (PREDICT_FALSE(written != data.size())) {
      return STATUS(IOError,
          Substitute("pwrite error: expected to write $0 bytes, wrote $1 bytes instead",
                     data.size(), written));
    }

    pending_sync_ = true;
    return Status::OK();
  }

  Status PreAllocate(uint64_t offset, size_t length) override {
    TRACE_EVENT1("io", "PosixRWFile::PreAllocate", "path", filename_);
    ThreadRestrictions::AssertIOAllowed();
    if (fallocate(fd_, 0, offset, length) < 0) {
      if (errno == EOPNOTSUPP) {
        YB_LOG_FIRST_N(WARNING, 1) << "The filesystem does not support fallocate().";
      } else if (errno == ENOSYS) {
        YB_LOG_FIRST_N(WARNING, 1) << "The kernel does not implement fallocate().";
      } else {
        return STATUS_IO_ERROR(filename_, errno);
      }
    }
    return Status::OK();
  }

  Status PunchHole(uint64_t offset, size_t length) override {
#if defined(__linux__)
    TRACE_EVENT1("io", "PosixRWFile::PunchHole", "path", filename_);
    ThreadRestrictions::AssertIOAllowed();
    if (fallocate(fd_, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, offset, length) < 0) {
      return STATUS_IO_ERROR(filename_, errno);
    }
    return Status::OK();
#else
    return STATUS(NotSupported, "Hole punching not supported on this platform");
#endif
  }

  Status Flush(FlushMode mode, uint64_t offset, size_t length) override {
    TRACE_EVENT1("io", "PosixRWFile::Flush", "path", filename_);
    ThreadRestrictions::AssertIOAllowed();
#if defined(__linux__)
    int flags = SYNC_FILE_RANGE_WRITE;
    if (mode == FLUSH_SYNC) {
      flags |= SYNC_FILE_RANGE_WAIT_AFTER;
    }
    if (sync_file_range(fd_, offset, length, flags) < 0) {
      return STATUS_IO_ERROR(filename_, errno);
    }
#else
    if (fsync(fd_) < 0) {
      return STATUS_IO_ERROR(filename_, errno);
    }
#endif
    return Status::OK();
  }

  Status Sync() override {
    TRACE_EVENT1("io", "PosixRWFile::Sync", "path", filename_);
    ThreadRestrictions::AssertIOAllowed();
    LOG_SLOW_EXECUTION(WARNING, 1000, Substitute("sync call for $0", filename())) {
      if (pending_sync_) {
        pending_sync_ = false;
        RETURN_NOT_OK(DoSync(fd_, filename_));
      }
    }
    return Status::OK();
  }

  Status Close() override {
    TRACE_EVENT1("io", "PosixRWFile::Close", "path", filename_);
    ThreadRestrictions::AssertIOAllowed();
    Status s;

    if (sync_on_close_) {
      // Virtual function call in destructor.
      s = Sync();
      if (!s.ok()) {
        LOG(ERROR) << "Unable to Sync " << filename_ << ": " << s.ToString();
      }
    }

    if (close(fd_) < 0) {
      if (s.ok()) {
        s = STATUS_IO_ERROR(filename_, errno);
      }
    }

    fd_ = -1;
    return s;
  }

  Status Size(uint64_t* size) const override {
    TRACE_EVENT1("io", "PosixRWFile::Size", "path", filename_);
    ThreadRestrictions::AssertIOAllowed();
    struct stat st;
    if (fstat(fd_, &st) == -1) {
      return STATUS_IO_ERROR(filename_, errno);
    }
    *size = st.st_size;
    return Status::OK();
  }

  const string& filename() const override {
    return filename_;
  }

 private:
  const std::string filename_;
  int fd_;
  bool sync_on_close_;
  bool pending_sync_;
};

// Set of pathnames that are locked, and a mutex for protecting changes to the set.
static std::set<std::string> locked_files;
static std::mutex mutex_locked_files;

// Helper function to lock/unlock file whose filename and file descriptor are passed in. Returns -1
// on failure and a value other than -1 (as mentioned in fcntl() doc) on success.
static int LockOrUnlock(const std::string& fname,
                        int fd,
                        bool lock,
                        bool recursive_lock_ok) {
  std::lock_guard<std::mutex> guard(mutex_locked_files);
  if (lock) {
    // If recursive locks on the same file must be disallowed, but the specified file name already
    // exists in the locked_files set, then it is already locked, so we fail this lock attempt.
    // Otherwise, we insert the specified file name into locked_files. This check is needed because
    // fcntl() does not detect lock conflict if the fcntl is issued by the same thread that earlier
    // acquired this lock.
    if (!locked_files.insert(fname).second && !recursive_lock_ok) {
      errno = ENOLCK;
      return -1;
    }
  } else {
    // If we are unlocking, then verify that we had locked it earlier, it should already exist in
    // locked_files. Remove it from locked_files.
    if (locked_files.erase(fname) != 1) {
      errno = ENOLCK;
      return -1;
    }
  }
  errno = 0;
  struct flock f;
  memset(&f, 0, sizeof(f));
  f.l_type = (lock ? F_WRLCK : F_UNLCK);
  f.l_whence = SEEK_SET;
  f.l_start = 0;
  f.l_len = 0; // Lock/unlock entire file.
  int value = fcntl(fd, F_SETLK, &f);
  if (value == -1 && lock) {
    // If there is an error in locking, then remove the pathname from locked_files.
    locked_files.erase(fname);
  }
  return value;
}

class PosixFileLock : public FileLock {
 public:
  int fd_;
  std::string filename;
};

class PosixEnv : public Env {
 public:
  PosixEnv();
  explicit PosixEnv(std::unique_ptr<FileFactory> file_factory);
  virtual ~PosixEnv() = default;

  virtual Status NewSequentialFile(const std::string& fname,
                                   std::unique_ptr<SequentialFile>* result) override {
    return file_factory_->NewSequentialFile(fname, result);
  }

  virtual Status NewRandomAccessFile(const std::string& fname,
                                     std::unique_ptr<RandomAccessFile>* result) override {
    return file_factory_->NewRandomAccessFile(fname, result);
  }

  virtual Status NewWritableFile(const std::string& fname,
                                 std::unique_ptr<WritableFile>* result) override {
    return file_factory_->NewWritableFile(fname, result);
  }

  virtual Status NewWritableFile(const WritableFileOptions& opts,
                                 const std::string& fname,
                                 std::unique_ptr<WritableFile>* result) override {
    return file_factory_->NewWritableFile(opts, fname, result);
  }

  virtual Status NewTempWritableFile(const WritableFileOptions& opts,
                                     const std::string& name_template,
                                     std::string* created_filename,
                                     std::unique_ptr<WritableFile>* result) override {
    return file_factory_->NewTempWritableFile(opts, name_template, created_filename, result);
  }

  virtual Status NewRWFile(const string& fname,
                           std::unique_ptr<RWFile>* result) override {
    return file_factory_->NewRWFile(fname, result);
  }

  virtual Status NewRWFile(const RWFileOptions& opts,
                           const string& fname,
                           std::unique_ptr<RWFile>* result) override {
    return file_factory_->NewRWFile(opts, fname, result);
  }

  bool FileExists(const std::string& fname) override {
    TRACE_EVENT1("io", "PosixEnv::FileExists", "path", fname);
    ThreadRestrictions::AssertIOAllowed();
    return access(fname.c_str(), F_OK) == 0;
  }

  virtual bool DirExists(const std::string& dname) override {
    TRACE_EVENT1("io", "PosixEnv::DirExists", "path", dname);
    ThreadRestrictions::AssertIOAllowed();
    struct stat statbuf;
    if (stat(dname.c_str(), &statbuf) == 0) {
      return S_ISDIR(statbuf.st_mode);
    }
    return false;
  }

  CHECKED_STATUS GetChildren(const std::string& dir,
                             ExcludeDots exclude_dots,
                             std::vector<std::string>* result) override {
    TRACE_EVENT1("io", "PosixEnv::GetChildren", "path", dir);
    ThreadRestrictions::AssertIOAllowed();
    result->clear();
    DIR* d = opendir(dir.c_str());
    if (d == nullptr) {
      return STATUS_IO_ERROR(dir, errno);
    }
    struct dirent* entry;
    // TODO: lint: Consider using readdir_r(...) instead of readdir(...) for improved thread safety.
    while ((entry = readdir(d)) != nullptr) {
      if (exclude_dots && (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)) {
        continue;
      }
      result->push_back(entry->d_name);
    }
    closedir(d);
    return Status::OK();
  }

  Status DeleteFile(const std::string& fname) override {
    TRACE_EVENT1("io", "PosixEnv::DeleteFile", "path", fname);
    ThreadRestrictions::AssertIOAllowed();
    Status result;
    if (unlink(fname.c_str()) != 0) {
      result = STATUS_IO_ERROR(fname, errno);
    }
    return result;
  };

  Status CreateDir(const std::string& name) override {
    TRACE_EVENT1("io", "PosixEnv::CreateDir", "path", name);
    ThreadRestrictions::AssertIOAllowed();
    Status result;
    if (mkdir(name.c_str(), 0755) != 0) {
      result = STATUS_IO_ERROR(name, errno);
    }
    return result;
  };

  Status DeleteDir(const std::string& name) override {
    TRACE_EVENT1("io", "PosixEnv::DeleteDir", "path", name);
    ThreadRestrictions::AssertIOAllowed();
    Status result;
    if (rmdir(name.c_str()) != 0) {
      result = STATUS_IO_ERROR(name, errno);
    }
    return result;
  };

  Status SyncDir(const std::string& dirname) override {
    TRACE_EVENT1("io", "SyncDir", "path", dirname);
    ThreadRestrictions::AssertIOAllowed();
    if (FLAGS_never_fsync) return Status::OK();
    int dir_fd;
    if ((dir_fd = open(dirname.c_str(), O_DIRECTORY|O_RDONLY)) == -1) {
      return STATUS_IO_ERROR(dirname, errno);
    }
    ScopedFdCloser fd_closer(dir_fd);
    if (fsync(dir_fd) != 0) {
      return STATUS_IO_ERROR(dirname, errno);
    }
    return Status::OK();
  }

  Status DeleteRecursively(const std::string &name) override {
    return Walk(name, POST_ORDER, Bind(&PosixEnv::DeleteRecursivelyCb,
                                       Unretained(this)));
  }

  Result<uint64_t> GetFileSize(const std::string& fname) override {
    return file_factory_->GetFileSize(fname);
  }

  Result<uint64_t> GetFileINode(const std::string& fname) override {
    return GetFileStat(
        fname, "PosixEnv::GetFileINode", [](const struct stat& sbuf) { return sbuf.st_ino; });
  }

  Result<uint64_t> GetFileSizeOnDisk(const std::string& fname) override {
    return GetFileStat(
        fname, "PosixEnv::GetFileSizeOnDisk", [](const struct stat& sbuf) {
          // From stat(2):
          //
          //   The st_blocks field indicates the number of blocks allocated to
          //   the file, 512-byte units. (This may be smaller than st_size/512
          //   when the file has holes.)
          return sbuf.st_blocks * 512;
        });
  }

  Result<uint64_t> GetBlockSize(const string& fname) override {
    return GetFileStat(
        fname, "PosixEnv::GetBlockSize", [](const struct stat& sbuf) { return sbuf.st_blksize; });
  }

  CHECKED_STATUS LinkFile(const std::string& src,
                          const std::string& target) override {
    if (link(src.c_str(), target.c_str()) != 0) {
      if (errno == EXDEV) {
        return STATUS(NotSupported, "No cross FS links allowed");
      }
      return STATUS_IO_ERROR(src, errno);
    }
    return Status::OK();
  }

  Result<std::string> ReadLink(const std::string& link) override {
    char buf[PATH_MAX];
    const auto len = readlink(link.c_str(), buf, sizeof(buf));
    if (len > -1) {
      return std::string(buf, buf + len);
    }
    return STATUS_IO_ERROR(link, errno);
  }

  Status RenameFile(const std::string& src, const std::string& target) override {
    TRACE_EVENT2("io", "PosixEnv::RenameFile", "src", src, "dst", target);
    ThreadRestrictions::AssertIOAllowed();
    Status result;
    if (rename(src.c_str(), target.c_str()) != 0) {
      result = STATUS_IO_ERROR(Format("Rename $0 => $1", src, target), errno);
    }
    return result;
  }

  virtual Status LockFile(const std::string& fname,
                          FileLock** lock,
                          bool recursive_lock_ok) override {
    TRACE_EVENT1("io", "PosixEnv::LockFile", "path", fname);
    ThreadRestrictions::AssertIOAllowed();
    *lock = nullptr;
    Status result;
    int fd = open(fname.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
      result = STATUS_IO_ERROR(fname, errno);
    } else if (LockOrUnlock(fname, fd, true /* lock */, recursive_lock_ok) == -1) {
      result = STATUS_IO_ERROR("lock " + fname, errno);
      close(fd);
    } else {
      auto my_lock = new PosixFileLock;
      my_lock->fd_ = fd;
      my_lock->filename = fname;
      *lock = my_lock;
    }
    return result;
  }

  Status UnlockFile(FileLock* lock) override {
    TRACE_EVENT0("io", "PosixEnv::UnlockFile");
    ThreadRestrictions::AssertIOAllowed();
    PosixFileLock* my_lock = reinterpret_cast<PosixFileLock*>(lock);
    Status result;
    if (LockOrUnlock(my_lock->filename,
                     my_lock->fd_,
                     false /* lock */,
                     false /* recursive_lock_ok (unused when lock = false) */) == -1) {
      result = STATUS_IO_ERROR("unlock", errno);
    }
    close(my_lock->fd_);
    delete my_lock;
    return result;
  }

  Status GetTestDirectory(std::string* result) override {
    string dir;
    const char* env = getenv("TEST_TMPDIR");
    if (env && env[0] != '\0') {
      dir = env;
    } else {
      char buf[100];
      snprintf(buf, sizeof(buf), "/tmp/ybtest-%d", static_cast<int>(geteuid()));
      dir = buf;
    }
    // Directory may already exist
    ignore_result(CreateDir(dir));
    // /tmp may be a symlink, so canonicalize the path.
    return Canonicalize(dir, result);
  }

  uint64_t gettid() override {
    // Platform-independent thread ID.  We can't use pthread_self here,
    // because that function returns a totally opaque ID, which can't be
    // compared via normal means.
    if (thread_local_id == 0) {
      thread_local_id = Barrier_AtomicIncrement(&cur_thread_local_id_, 1);
    }
    return thread_local_id;
  }

  uint64_t NowMicros() override {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return static_cast<uint64_t>(tv.tv_sec) * 1000000 + tv.tv_usec;
  }

  uint64_t NowNanos() override {
#if defined(__linux__) || defined(OS_FREEBSD)
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000 + ts.tv_nsec;
#elif defined(__MACH__)
    clock_serv_t cclock;
    mach_timespec_t ts;
    host_get_clock_service(mach_host_self(), CALENDAR_CLOCK, &cclock);
    clock_get_time(cclock, &ts);
    mach_port_deallocate(mach_task_self(), cclock);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000 + ts.tv_nsec;
#else
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
       std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
  }

  void SleepForMicroseconds(int micros) override {
    ThreadRestrictions::AssertWaitAllowed();
    SleepFor(MonoDelta::FromMicroseconds(micros));
  }

  Status GetExecutablePath(string* path) override {
    uint32_t size = 64;
    uint32_t len = 0;
    while (true) {
      std::unique_ptr<char[]> buf(new char[size]);
#if defined(__linux__)
      int rc = readlink("/proc/self/exe", buf.get(), size);
      if (rc == -1) {
        return STATUS(IOError, "Unable to determine own executable path", "", Errno(errno));
      } else if (rc >= size) {
        // The buffer wasn't large enough
        size *= 2;
        continue;
      }
      len = rc;
#elif defined(__APPLE__)
      if (_NSGetExecutablePath(buf.get(), &size) != 0) {
        // The buffer wasn't large enough; 'size' has been updated.
        continue;
      }
      len = strlen(buf.get());
#else
#error Unsupported platform
#endif

      path->assign(buf.get(), len);
      break;
    }
    return Status::OK();
  }

  Status IsDirectory(const string& path, bool* is_dir) override {
    TRACE_EVENT1("io", "PosixEnv::IsDirectory", "path", path);
    ThreadRestrictions::AssertIOAllowed();
    Status s;
    struct stat sbuf;
    if (stat(path.c_str(), &sbuf) != 0) {
      s = STATUS_IO_ERROR(path, errno);
    } else {
      *is_dir = S_ISDIR(sbuf.st_mode);
    }
    return s;
  }

  Result<bool> IsExecutableFile(const std::string& path) override {
    TRACE_EVENT1("io", "PosixEnv::IsExecutableFile", "path", path);
    ThreadRestrictions::AssertIOAllowed();
    Status s;
    struct stat sbuf;
    if (stat(path.c_str(), &sbuf) != 0) {
      if (errno == ENOENT) {
        // If the file does not exist, we just return false.
        return false;
      }
      return STATUS_IO_ERROR(path, errno);
    }

    return !S_ISDIR(sbuf.st_mode) && (sbuf.st_mode & S_IXUSR);
  }

  Status Walk(const string& root, DirectoryOrder order, const WalkCallback& cb) override {
    TRACE_EVENT1("io", "PosixEnv::Walk", "path", root);
    ThreadRestrictions::AssertIOAllowed();
    // Some sanity checks
    CHECK_NE(root, "/");
    CHECK_NE(root, "./");
    CHECK_NE(root, ".");
    CHECK_NE(root, "");

    // FTS requires a non-const copy of the name. strdup it and free() when
    // we leave scope.
    gscoped_ptr<char, FreeDeleter> name_dup(strdup(root.c_str()));
    char *paths[] = { name_dup.get(), nullptr };

    // FTS_NOCHDIR is important here to make this thread-safe.
    gscoped_ptr<FTS, FtsCloser> tree(
        fts_open(paths, FTS_PHYSICAL | FTS_XDEV | FTS_NOCHDIR, nullptr));
    if (!tree.get()) {
      return STATUS_IO_ERROR(root, errno);
    }

    FTSENT *ent = nullptr;
    bool had_errors = false;
    while ((ent = fts_read(tree.get())) != nullptr) {
      bool doCb = false;
      FileType type = DIRECTORY_TYPE;
      switch (ent->fts_info) {
        case FTS_D:         // Directory in pre-order
          if (order == PRE_ORDER) {
            doCb = true;
          }
          break;
        case FTS_DP:        // Directory in post-order
          if (order == POST_ORDER) {
            doCb = true;
          }
          break;
        case FTS_F:         // A regular file
        case FTS_SL:        // A symbolic link
        case FTS_SLNONE:    // A broken symbolic link
        case FTS_DEFAULT:   // Unknown type of file
          doCb = true;
          type = FILE_TYPE;
          break;

        case FTS_ERR:
          LOG(WARNING) << "Unable to access file " << ent->fts_path
                       << " during walk: " << strerror(ent->fts_errno);
          had_errors = true;
          break;

        default:
          LOG(WARNING) << "Unable to access file " << ent->fts_path
                       << " during walk (code " << ent->fts_info << ")";
          break;
      }
      if (doCb) {
        if (!cb.Run(type, DirName(ent->fts_path), ent->fts_name).ok()) {
          had_errors = true;
        }
      }
    }

    if (had_errors) {
      return STATUS(IOError, root, "One or more errors occurred");
    }
    return Status::OK();
  }

  Status Canonicalize(const string& path, string* result) override {
    TRACE_EVENT1("io", "PosixEnv::Canonicalize", "path", path);
    ThreadRestrictions::AssertIOAllowed();
    gscoped_ptr<char[], FreeDeleter> r(realpath(path.c_str(), nullptr));
    if (!r) {
      return STATUS_IO_ERROR(path, errno);
    }
    *result = string(r.get());
    return Status::OK();
  }

  Status GetTotalRAMBytes(int64_t* ram) override {
#if defined(__APPLE__)
    int mib[2];
    size_t length = sizeof(*ram);

    // Get the Physical memory size
    mib[0] = CTL_HW;
    mib[1] = HW_MEMSIZE;
    CHECK_ERR(sysctl(mib, 2, ram, &length, nullptr, 0)) << "sysctl CTL_HW HW_MEMSIZE failed";
#else
    struct sysinfo info;
    if (sysinfo(&info) < 0) {
      return STATUS_IO_ERROR("sysinfo() failed", errno);
    }
    *ram = info.totalram;
#endif
    return Status::OK();
  }

  FileFactory* GetFileFactory() {
    return file_factory_.get();
  }

  Result<uint64_t> GetFreeSpaceBytes(const std::string& path) override {
    if (PREDICT_FALSE(FLAGS_TEST_simulate_free_space_bytes >= 0)) {
      return FLAGS_TEST_simulate_free_space_bytes;
    }
    struct statvfs stat;
    auto ret = statvfs(path.c_str(), &stat);
    if (ret != 0) {
      if (errno == EACCES) {
        return STATUS_SUBSTITUTE(NotAuthorized,
            "Caller doesn't have the required permission on a component of the path $0",
            path);
      } else if (errno == EIO) {
        return STATUS_SUBSTITUTE(IOError,
            "I/O error occurred while reading from '$0' filesystem",
            path);
      } else if (errno == ELOOP) {
        return STATUS_SUBSTITUTE(InternalError,
            "Too many symbolic links while translating '$0' path",
            path);
      } else if (errno == ENAMETOOLONG) {
        return STATUS_SUBSTITUTE(NotSupported,
            "Path '$0' is too long",
            path);
      } else if (errno == ENOENT) {
        return STATUS_SUBSTITUTE(NotFound,
            "File specified by path '$0' doesn't exist",
            path);
      } else if (errno == ENOMEM) {
        return STATUS(InternalError, "Insufficient memory");
      } else if (errno == ENOSYS) {
        return STATUS_SUBSTITUTE(NotSupported,
            "Filesystem for path '$0' doesn't support statvfs",
            path);
      } else if (errno == ENOTDIR) {
        return STATUS_SUBSTITUTE(InvalidArgument,
            "A component of the path '$0' is not a directory",
            path);
      } else {
        return STATUS_SUBSTITUTE(InternalError,
            "Failed to read information about filesystem for path '%s': errno=$0: $1",
            path,
            errno,
            ErrnoToString(errno));
      }
    }
    uint64_t block_size = stat.f_frsize > 0 ? static_cast<uint64_t>(stat.f_frsize) :
                                              static_cast<uint64_t>(stat.f_bsize);
    uint64_t available_blocks = static_cast<uint64_t>(stat.f_bavail);

    return available_blocks * block_size;
  }

  Result<ResourceLimits> GetUlimit(int resource) override {
    struct rlimit lim;
    if (getrlimit(resource, &lim) != 0) {
      return STATUS_IO_ERROR("getrlimit() failed", errno);
    }
    ResourceLimit soft(lim.rlim_cur);
    ResourceLimit hard(lim.rlim_max);
    ResourceLimits limits { soft, hard };
    return limits;
  }

  CHECKED_STATUS SetUlimit(int resource, ResourceLimit value) override {
    return SetUlimit(resource, value, strings::Substitute("resource no. $0", resource));
  }

  CHECKED_STATUS SetUlimit(
      int resource, ResourceLimit value, const std::string& resource_name) override {

    auto limits = VERIFY_RESULT(GetUlimit(resource));
    if (limits.soft == value) {
      return Status::OK();
    }
    if (limits.hard < value) {
      return STATUS_FORMAT(
        InvalidArgument,
        "Resource limit value $0 for resource $1 greater than hard limit $2",
        value, resource, limits.hard.ToString());
    }
    struct rlimit lim;
    lim.rlim_cur = value.RawValue();
    lim.rlim_max = limits.hard.RawValue();
    LOG(INFO)
        << "Modifying limit for " << resource_name
        << " from " << limits.soft.ToString()
        << " to " << value.ToString();
    if (setrlimit(resource, &lim) != 0) {
      return STATUS(RuntimeError, "Unable to set rlimit", Errno(errno));
    }
    return Status::OK();
  }

 private:
  // gscoped_ptr Deleter implementation for fts_close
  struct FtsCloser {
    void operator()(FTS *fts) const {
      if (fts) { fts_close(fts); }
    }
  };

  Status DeleteRecursivelyCb(FileType type, const string& dirname, const string& basename) {
    string full_path = JoinPathSegments(dirname, basename);
    Status s;
    switch (type) {
      case FILE_TYPE:
        s = DeleteFile(full_path);
        WARN_NOT_OK(s, "Could not delete file");
        return s;
      case DIRECTORY_TYPE:
        s = DeleteDir(full_path);
        WARN_NOT_OK(s, "Could not delete directory");
        return s;
      default:
        LOG(FATAL) << "Unknown file type: " << type;
        return Status::OK();
    }
  }

  std::unique_ptr<FileFactory> file_factory_;
};

class PosixFileFactory : public FileFactory {
#if defined(__linux__)
  static constexpr int kODirectFlags = O_DIRECT | O_NOATIME | O_SYNC;
#else
  static constexpr int kODirectFlags = 0;
#endif

 public:
  PosixFileFactory() {}
  ~PosixFileFactory() {}

  Status NewSequentialFile(
      const std::string& fname, std::unique_ptr<SequentialFile>* result) override {
    TRACE_EVENT1("io", "PosixEnv::NewSequentialFile", "path", fname);
    ThreadRestrictions::AssertIOAllowed();
    FILE* f = fopen(fname.c_str(), "r");
    if (f == nullptr) {
      return STATUS_IO_ERROR(fname, errno);
    } else {
      result->reset(new yb::PosixSequentialFile(fname, f, yb::FileSystemOptions::kDefault));
      return Status::OK();
    }
  }

  Status NewRandomAccessFile(const std::string& fname,
                             std::unique_ptr<RandomAccessFile>* result) override {
    TRACE_EVENT1("io", "PosixEnv::NewRandomAccessFile", "path", fname);
    ThreadRestrictions::AssertIOAllowed();
    int fd = open(fname.c_str(), O_RDONLY);
    if (fd < 0) {
      return STATUS_IO_ERROR(fname, errno);
    }

    result->reset(new yb::PosixRandomAccessFile(fname, fd, yb::FileSystemOptions::kDefault));
    return Status::OK();
  }

  Status NewWritableFile(const std::string& fname,
                         std::unique_ptr<WritableFile>* result) override {
    return NewWritableFile(WritableFileOptions(), fname, result);
  }

  Status NewWritableFile(const WritableFileOptions& opts,
                         const std::string& fname,
                         std::unique_ptr<WritableFile>* result) override {
    TRACE_EVENT1("io", "PosixEnv::NewWritableFile", "path", fname);
    int fd = -1;
    int extra_flags = 0;
    if (UseODirect(opts.o_direct)) {
      extra_flags = kODirectFlags;
    }
    RETURN_NOT_OK(DoOpen(fname, opts.mode, &fd, extra_flags));
    return InstantiateNewWritableFile(fname, fd, opts, result);
  }

  Status NewTempWritableFile(const WritableFileOptions& opts,
                             const std::string& name_template,
                             std::string* created_filename,
                             std::unique_ptr<WritableFile>* result) override {
    TRACE_EVENT1("io", "PosixEnv::NewTempWritableFile", "template", name_template);
    ThreadRestrictions::AssertIOAllowed();
    std::unique_ptr<char[]> fname(new char[name_template.size() + 1]);
    ::snprintf(fname.get(), name_template.size() + 1, "%s", name_template.c_str());
    int fd = -1;
    if (UseODirect(opts.o_direct)) {
      fd = ::mkostemp(fname.get(), kODirectFlags);
    } else {
      fd = ::mkstemp(fname.get());
    }
    if (fd < 0) {
      return STATUS_IO_ERROR(Format("Call to mkstemp() failed on name template $0", name_template),
                             errno);
    }
    *created_filename = fname.get();
    return InstantiateNewWritableFile(*created_filename, fd, opts, result);
  }

  Status NewRWFile(const string& fname, std::unique_ptr<RWFile>* result) override {
    return NewRWFile(RWFileOptions(), fname, result);
  }

  Status NewRWFile(const RWFileOptions& opts, const string& fname,
                   std::unique_ptr<RWFile>* result) override {
    TRACE_EVENT1("io", "PosixEnv::NewRWFile", "path", fname);
    int fd = -1;
    RETURN_NOT_OK(DoOpen(fname, opts.mode, &fd));
    result->reset(new PosixRWFile(fname, fd, opts.sync_on_close));
    return Status::OK();
  }

  Result<uint64_t> GetFileSize(const std::string& fname) override {
    return GetFileStat(
        fname, "PosixEnv::GetFileSize", [](const struct stat& sbuf) { return sbuf.st_size; });
  }

 private:
  bool UseODirect(bool o_direct) {
#if defined(__linux__)
    return o_direct;
#else
    return false;
#endif
  }

  Status InstantiateNewWritableFile(const std::string& fname,
                                    int fd,
                                    const WritableFileOptions& opts,
                                    std::unique_ptr<WritableFile>* result) {
    uint64_t file_size = 0;
    if (opts.mode == PosixEnv::OPEN_EXISTING) {
      file_size = VERIFY_RESULT(GetFileSize(fname));
    }
    PosixWritableFile *posix_writable_file;
#if defined(__linux__)
    if (opts.o_direct)
      posix_writable_file = new PosixDirectIOWritableFile(fname, fd, file_size, opts.sync_on_close);
    else
#endif
    posix_writable_file = new PosixWritableFile(fname, fd, file_size, opts.sync_on_close);
    result->reset(posix_writable_file);
    return Status::OK();
  }
};

PosixEnv::PosixEnv() : file_factory_(std::make_unique<PosixFileFactory>()) {}
PosixEnv::PosixEnv(std::unique_ptr<FileFactory> file_factory) :
  file_factory_(std::move(file_factory)) {}

}  // namespace

static pthread_once_t once = PTHREAD_ONCE_INIT;
static Env* default_env;
static void InitDefaultEnv() { default_env = new PosixEnv; }

Env* Env::Default() {
  pthread_once(&once, InitDefaultEnv);
  return default_env;
}

FileFactory* Env::DefaultFileFactory() {
  return down_cast<PosixEnv*>(Env::Default())->GetFileFactory();
}

std::unique_ptr<Env> Env::NewDefaultEnv(std::unique_ptr<FileFactory> file_factory) {
  return std::make_unique<PosixEnv>(std::move(file_factory));
}


}  // namespace yb
