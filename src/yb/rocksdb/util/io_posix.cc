//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
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
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifdef ROCKSDB_LIB_IO_POSIX

#include "yb/rocksdb/util/io_posix.h"
#include <errno.h>
#include <fcntl.h>
#if defined(__linux__)
#include <linux/fs.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifdef __linux__
#include <sys/statfs.h>
#include <sys/syscall.h>
#endif
#include "yb/rocksdb/port/port.h"
#include "yb/rocksdb/util/coding.h"
#include "yb/rocksdb/util/posix_logger.h"
#include "yb/rocksdb/util/sync_point.h"

#include "yb/util/errno.h"
#include "yb/util/file_system_posix.h"
#include "yb/util/malloc.h"
#include "yb/util/slice.h"
#include "yb/util/stats/iostats_context_imp.h"
#include "yb/util/string_util.h"

namespace rocksdb {

// A wrapper for fadvise, if the platform doesn't support fadvise,
// it will simply return Status::NotSupport.
int Fadvise(int fd, off_t offset, size_t len, int advice) {
#ifdef __linux__
  return posix_fadvise(fd, offset, len, advice);
#else
  return 0;  // simply do nothing.
#endif
}

/*
 * PosixMmapReadableFile
 *
 * mmap() based random-access
 */
// base[0,length-1] contains the mmapped contents of the file.
PosixMmapReadableFile::PosixMmapReadableFile(const int fd,
                                             const std::string& fname,
                                             void* base, size_t length,
                                             const EnvOptions& options)
    : fd_(fd), filename_(fname), mmapped_region_(base), length_(length) {
  fd_ = fd_ + 0;  // suppress the warning for used variables
  assert(options.use_mmap_reads);
  assert(options.use_os_buffer);
}

PosixMmapReadableFile::~PosixMmapReadableFile() {
  int ret = munmap(mmapped_region_, length_);
  if (ret != 0) {
    fprintf(stdout, "failed to munmap %p length %" ROCKSDB_PRIszt " \n",
            mmapped_region_, length_);
  }
}

Status PosixMmapReadableFile::Read(uint64_t offset, size_t n, Slice* result,
                                   uint8_t* scratch) const {
  Status s;
  if (offset > length_) {
    *result = Slice();
    return STATUS_IO_ERROR(filename_, EINVAL);
  } else if (offset + n > length_) {
    n = static_cast<size_t>(length_ - offset);
  }
  *result = Slice(reinterpret_cast<char*>(mmapped_region_) + offset, n);
  return s;
}

Status PosixMmapReadableFile::InvalidateCache(size_t offset, size_t length) {
#ifndef __linux__
  return Status::OK();
#else
  // free OS pages
  int ret = Fadvise(fd_, offset, length, POSIX_FADV_DONTNEED);
  if (ret == 0) {
    return Status::OK();
  }
  return STATUS_IO_ERROR(filename_, errno);
#endif
}

yb::Result<uint64_t> PosixMmapReadableFile::INode() const {
  struct stat st;
  if (stat(filename_.c_str(), &st) != 0) {
    return STATUS_IO_ERROR(filename_, errno);
  } else {
    return st.st_ino;
  }
}

size_t PosixMmapReadableFile::memory_footprint() const {
  return malloc_usable_size(this) + filename_.capacity();
}

/*
 * PosixMmapFile
 *
 * We preallocate up to an extra megabyte and use memcpy to append new
 * data to the file.  This is safe since we either properly close the
 * file before reading from it, or for log files, the reading code
 * knows enough to skip zero suffixes.
 */
Status PosixMmapFile::UnmapCurrentRegion() {
  TEST_KILL_RANDOM("PosixMmapFile::UnmapCurrentRegion:0", rocksdb_kill_odds);
  if (base_ != nullptr) {
    int munmap_status = munmap(base_, limit_ - base_);
    if (munmap_status != 0) {
      return STATUS_IO_ERROR(filename_, munmap_status);
    }
    file_offset_ += limit_ - base_;
    base_ = nullptr;
    limit_ = nullptr;
    last_sync_ = nullptr;
    dst_ = nullptr;

    // Increase the amount we map the next time, but capped at 1MB
    if (map_size_ < (1 << 20)) {
      map_size_ *= 2;
    }
  }
  return Status::OK();
}

Status PosixMmapFile::MapNewRegion() {
#ifdef ROCKSDB_FALLOCATE_PRESENT
  assert(base_ == nullptr);

  TEST_KILL_RANDOM("PosixMmapFile::UnmapCurrentRegion:0", rocksdb_kill_odds);
  // we can't fallocate with FALLOC_FL_KEEP_SIZE here
  if (allow_fallocate_) {
    IOSTATS_TIMER_GUARD(allocate_nanos);
    int alloc_status = fallocate(fd_, 0, file_offset_, map_size_);
    if (alloc_status != 0) {
      // fallback to posix_fallocate
      alloc_status = posix_fallocate(fd_, file_offset_, map_size_);
    }
    yb::LogFatalForFallocateFailure(alloc_status, errno);
  }

  TEST_KILL_RANDOM("PosixMmapFile::Append:1", rocksdb_kill_odds);
  void* ptr = mmap(nullptr, map_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_,
                   file_offset_);
  if (ptr == MAP_FAILED) {
    return STATUS(IOError, "MMap failed on " + filename_);
  }
  TEST_KILL_RANDOM("PosixMmapFile::Append:2", rocksdb_kill_odds);

  base_ = reinterpret_cast<char*>(ptr);
  limit_ = base_ + map_size_;
  dst_ = base_;
  last_sync_ = base_;
  return Status::OK();
#else
  YB_LOG_FIRST_N(FATAL, 1) << "This platform does not support fallocate().";
#endif
}

Status PosixMmapFile::Msync() {
  if (dst_ == last_sync_) {
    return Status::OK();
  }
  // Find the beginnings of the pages that contain the first and last
  // bytes to be synced.
  size_t p1 = TruncateToPageBoundary(last_sync_ - base_);
  size_t p2 = TruncateToPageBoundary(dst_ - base_ - 1);
  last_sync_ = dst_;
  TEST_KILL_RANDOM("PosixMmapFile::Msync:0", rocksdb_kill_odds);
  if (msync(base_ + p1, p2 - p1 + page_size_, MS_SYNC) < 0) {
    return STATUS_IO_ERROR(filename_, errno);
  }
  return Status::OK();
}

PosixMmapFile::PosixMmapFile(const std::string& fname, int fd, size_t page_size,
                             const EnvOptions& options)
    : filename_(fname),
      fd_(fd),
      page_size_(page_size),
      map_size_(Roundup(65536, page_size)),
      base_(nullptr),
      limit_(nullptr),
      dst_(nullptr),
      last_sync_(nullptr),
      file_offset_(0) {
#ifdef ROCKSDB_FALLOCATE_PRESENT
  allow_fallocate_ = options.allow_fallocate;
  fallocate_with_keep_size_ = options.fallocate_with_keep_size;
#endif
  assert((page_size & (page_size - 1)) == 0);
  assert(options.use_mmap_writes);
}

PosixMmapFile::~PosixMmapFile() {
  if (fd_ >= 0) {
    WARN_NOT_OK(PosixMmapFile::Close(), "Failed to close posix mmap file");
  }
}

Status PosixMmapFile::Append(const Slice& data) {
  const char* src = data.cdata();
  size_t left = data.size();
  while (left > 0) {
    assert(base_ <= dst_);
    assert(dst_ <= limit_);
    size_t avail = limit_ - dst_;
    if (avail == 0) {
      Status s = UnmapCurrentRegion();
      if (!s.ok()) {
        return s;
      }
      s = MapNewRegion();
      if (!s.ok()) {
        return s;
      }
      TEST_KILL_RANDOM("PosixMmapFile::Append:0", rocksdb_kill_odds);
    }

    size_t n = (left <= avail) ? left : avail;
    memcpy(dst_, src, n);
    dst_ += n;
    src += n;
    left -= n;
  }
  return Status::OK();
}

Status PosixMmapFile::Close() {
  Status s;
  size_t unused = limit_ - dst_;

  s = UnmapCurrentRegion();
  if (!s.ok()) {
    s = STATUS_IO_ERROR(filename_, errno);
  } else if (unused > 0) {
    // Trim the extra space at the end of the file
    if (ftruncate(fd_, file_offset_ - unused) < 0) {
      s = STATUS_IO_ERROR(filename_, errno);
    }
  }

  if (close(fd_) < 0) {
    if (s.ok()) {
      s = STATUS_IO_ERROR(filename_, errno);
    }
  }

  fd_ = -1;
  base_ = nullptr;
  limit_ = nullptr;
  return s;
}

Status PosixMmapFile::Flush() { return Status::OK(); }

Status PosixMmapFile::Sync() {
  if (fdatasync(fd_) < 0) {
    return STATUS_IO_ERROR(filename_, errno);
  }

  return Msync();
}

/**
 * Flush data as well as metadata to stable storage.
 */
Status PosixMmapFile::Fsync() {
  if (fsync(fd_) < 0) {
    return STATUS_IO_ERROR(filename_, errno);
  }

  return Msync();
}

/**
 * Get the size of valid data in the file. This will not match the
 * size that is returned from the filesystem because we use mmap
 * to extend file by map_size every time.
 */
uint64_t PosixMmapFile::GetFileSize() {
  size_t used = dst_ - base_;
  return file_offset_ + used;
}

Status PosixMmapFile::InvalidateCache(size_t offset, size_t length) {
#ifndef __linux__
  return Status::OK();
#else
  // free OS pages
  int ret = Fadvise(fd_, offset, length, POSIX_FADV_DONTNEED);
  if (ret == 0) {
    return Status::OK();
  }
  return STATUS_IO_ERROR(filename_, errno);
#endif
}

#ifdef ROCKSDB_FALLOCATE_PRESENT
Status PosixMmapFile::Allocate(uint64_t offset, uint64_t len) {
  assert(offset <= std::numeric_limits<off_t>::max());
  assert(len <= std::numeric_limits<off_t>::max());
  TEST_KILL_RANDOM("PosixMmapFile::Allocate:0", rocksdb_kill_odds);
  int alloc_status = 0;
  if (allow_fallocate_) {
    alloc_status = fallocate(
        fd_, fallocate_with_keep_size_ ? FALLOC_FL_KEEP_SIZE : 0,
          static_cast<off_t>(offset), static_cast<off_t>(len));
  }
  yb::LogFatalForFallocateFailure(alloc_status, errno);
  return Status::OK();
}
#endif

/*
 * PosixWritableFile
 *
 * Use posix write to write data to a file.
 */
PosixWritableFile::PosixWritableFile(const std::string& fname, int fd,
                                     const EnvOptions& options)
    : filename_(fname), fd_(fd), filesize_(0) {
#ifdef ROCKSDB_FALLOCATE_PRESENT
  allow_fallocate_ = options.allow_fallocate;
  fallocate_with_keep_size_ = options.fallocate_with_keep_size;
#endif
  assert(!options.use_mmap_writes);
}

PosixWritableFile::~PosixWritableFile() {
  if (fd_ >= 0) {
    WARN_NOT_OK(PosixWritableFile::Close(), "Failed to close posix writable file");
  }
}

Status PosixWritableFile::Append(const Slice& data) {
  const char* src = data.cdata();
  size_t left = data.size();
  while (left != 0) {
    ssize_t done = write(fd_, src, left);
    if (done < 0) {
      if (errno == EINTR) {
        continue;
      }
      return STATUS_IO_ERROR(filename_, errno);
    }
    left -= done;
    src += done;
  }
  filesize_ += data.size();
  return Status::OK();
}

Status PosixWritableFile::Close() {
  Status s;

  size_t block_size;
  size_t last_allocated_block;
  GetPreallocationStatus(&block_size, &last_allocated_block);
  if (last_allocated_block > 0) {
    // trim the extra space preallocated at the end of the file
    // NOTE(ljin): we probably don't want to surface failure as an IOError,
    // but it will be nice to log these errors.
    int dummy __attribute__((unused));
    dummy = ftruncate(fd_, filesize_);
#ifdef ROCKSDB_FALLOCATE_PRESENT
    // in some file systems, ftruncate only trims trailing space if the
    // new file size is smaller than the current size. Calling fallocate
    // with FALLOC_FL_PUNCH_HOLE flag to explicitly release these unused
    // blocks. FALLOC_FL_PUNCH_HOLE is supported on at least the following
    // filesystems:
    //   XFS (since Linux 2.6.38)
    //   ext4 (since Linux 3.0)
    //   Btrfs (since Linux 3.7)
    //   tmpfs (since Linux 3.5)
    // We ignore error since failure of this operation does not affect
    // correctness.
    IOSTATS_TIMER_GUARD(allocate_nanos);
    if (allow_fallocate_) {
      fallocate(fd_, FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE, filesize_,
                block_size * last_allocated_block - filesize_);
    }
#endif
  }

  if (close(fd_) < 0) {
    s = STATUS_IO_ERROR(filename_, errno);
  }
  fd_ = -1;
  return s;
}

// write out the cached data to the OS cache
Status PosixWritableFile::Flush() { return Status::OK(); }

Status PosixWritableFile::Sync() {
  if (fdatasync(fd_) < 0) {
    return STATUS_IO_ERROR(filename_, errno);
  }
  return Status::OK();
}

Status PosixWritableFile::Fsync() {
  if (fsync(fd_) < 0) {
    return STATUS_IO_ERROR(filename_, errno);
  }
  return Status::OK();
}

bool PosixWritableFile::IsSyncThreadSafe() const { return true; }

uint64_t PosixWritableFile::GetFileSize() { return filesize_; }

Status PosixWritableFile::InvalidateCache(size_t offset, size_t length) {
#ifndef __linux__
  return Status::OK();
#else
  // free OS pages
  int ret = Fadvise(fd_, offset, length, POSIX_FADV_DONTNEED);
  if (ret == 0) {
    return Status::OK();
  }
  return STATUS_IO_ERROR(filename_, errno);
#endif
}

#ifdef ROCKSDB_FALLOCATE_PRESENT
Status PosixWritableFile::Allocate(uint64_t offset, uint64_t len) {
  assert(offset <= std::numeric_limits<off_t>::max());
  assert(len <= std::numeric_limits<off_t>::max());
  TEST_KILL_RANDOM("PosixWritableFile::Allocate:0", rocksdb_kill_odds);
  IOSTATS_TIMER_GUARD(allocate_nanos);
  int alloc_status = 0;
  if (allow_fallocate_) {
    alloc_status = fallocate(
        fd_, fallocate_with_keep_size_ ? FALLOC_FL_KEEP_SIZE : 0,
        static_cast<off_t>(offset), static_cast<off_t>(len));
  }
  yb::LogFatalForFallocateFailure(alloc_status, errno);
  return Status::OK();
}

Status PosixWritableFile::RangeSync(uint64_t offset, uint64_t nbytes) {
  assert(offset <= std::numeric_limits<off_t>::max());
  assert(nbytes <= std::numeric_limits<off_t>::max());
  if (sync_file_range(fd_, static_cast<off_t>(offset),
      static_cast<off_t>(nbytes), SYNC_FILE_RANGE_WRITE) == 0) {
    return Status::OK();
  } else {
    return STATUS_IO_ERROR(filename_, errno);
  }
}

size_t PosixWritableFile::GetUniqueId(char* id) const {
  return yb::GetUniqueIdFromFile(fd_, pointer_cast<uint8_t*>(id));
}
#endif

PosixDirectory::~PosixDirectory() { close(fd_); }

Status PosixDirectory::Fsync() {
  if (fsync(fd_) == -1) {
    return STATUS_IO_ERROR("directory", errno);
  }
  return Status::OK();
}
}  // namespace rocksdb
#endif
