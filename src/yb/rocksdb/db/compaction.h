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

#ifndef YB_ROCKSDB_DB_COMPACTION_H
#define YB_ROCKSDB_DB_COMPACTION_H

#pragma once

#include <atomic>

#include "yb/rocksdb/util/arena.h"
#include "yb/rocksdb/util/autovector.h"
#include "yb/rocksdb/util/mutable_cf_options.h"
#include "yb/rocksdb/db/version_edit.h"
#include "yb/util/result.h"

namespace yb {

class PriorityThreadPoolSuspender;

}

namespace rocksdb {

using yb::Result;

// The structure that manages compaction input files associated
// with the same physical level.
struct CompactionInputFiles {
  int level;
  std::vector<FileMetaData*> files;
  inline bool empty() const { return files.empty(); }
  inline size_t size() const { return files.size(); }
  inline void clear() { files.clear(); }
  inline FileMetaData* operator[](size_t i) const { return files[i]; }
};

class Version;
class ColumnFamilyData;
class VersionStorageInfo;
class CompactionFilter;

struct LightweightBoundaries {
  Slice key;
  size_t num_user_values;
  UserBoundaryTag* user_tags;
  Slice* user_values;

  Slice user_key() const { return ExtractUserKey(key); }

  // Allocates appropriate objects in provided arena, so arena should live longer than this object.
  LightweightBoundaries(Arena* arena, const FileMetaData::BoundaryValues& source);

  const Slice* user_value_with_tag(UserBoundaryTag tag) const {
    for (size_t i = 0; i != num_user_values; ++i) {
      if (user_tags[i] == tag) {
        return user_values + i;
      }
    }
    return nullptr;
  }
};

// A compressed copy of file meta data that just contains smallest and largest key's slice.
struct FdWithBoundaries {
  FileDescriptor fd;
  LightweightBoundaries smallest; // smallest boundaries in this file
  LightweightBoundaries largest;  // largest boundaries in this file
  Slice user_filter_data; // Data provided by user that will be passed to iterator replacer.

  FdWithBoundaries(Arena* arena, const FileMetaData& source);
};

// Data structure to store an array of FdWithKeyRange in one level
// Actual data is guaranteed to be stored closely
struct LevelFilesBrief {
  size_t num_files;
  FdWithBoundaries* files;

  LevelFilesBrief() {
    num_files = 0;
    files = nullptr;
  }
};

// A Compaction encapsulates information about a compaction.
class Compaction {
 public:
  Compaction(VersionStorageInfo* input_version,
             const MutableCFOptions& mutable_cf_options,
             std::vector<CompactionInputFiles> inputs, int output_level,
             uint64_t target_file_size, uint64_t max_grandparent_overlap_bytes,
             uint32_t output_path_id, CompressionType compression,
             std::vector<FileMetaData*> grandparents,
             bool manual_compaction = false, double score = -1,
             bool deletion_compaction = false,
             CompactionReason compaction_reason = CompactionReason::kUnknown);

  // No copying allowed
  Compaction(const Compaction&) = delete;
  void operator=(const Compaction&) = delete;

  ~Compaction();

  // Returns the level associated to the specified compaction input level.
  // If compaction_input_level is not specified, then input_level is set to 0.
  int level(size_t compaction_input_level = 0) const {
    return inputs_[compaction_input_level].level;
  }

  int start_level() const { return start_level_; }

  // Outputs will go to this level
  int output_level() const { return output_level_; }

  // Returns the number of input levels in this compaction.
  size_t num_input_levels() const { return inputs_.size(); }

  // Return the object that holds the edits to the descriptor done
  // by this compaction.
  VersionEdit* edit() { return &edit_; }

  // Returns the number of input files associated to the specified
  // compaction input level.
  // The function will return 0 if when "compaction_input_level" < 0
  // or "compaction_input_level" >= "num_input_levels()".
  size_t num_input_files(size_t compaction_input_level) const {
    if (compaction_input_level < inputs_.size()) {
      return inputs_[compaction_input_level].size();
    }
    return 0;
  }

  uint64_t input_version_number() const { return input_version_number_; }

  // Returns the ColumnFamilyData associated with the compaction.
  ColumnFamilyData* column_family_data() const { return cfd_; }

  // Returns the file meta data of the 'i'th input file at the
  // specified compaction input level.
  // REQUIREMENT: "compaction_input_level" must be >= 0 and
  //              < "input_levels()"
  FileMetaData* input(size_t compaction_input_level, size_t i) const {
    assert(compaction_input_level < inputs_.size());
    return inputs_[compaction_input_level][i];
  }

  // Returns the list of file meta data of the specified compaction
  // input level.
  // REQUIREMENT: "compaction_input_level" must be >= 0 and
  //              < "input_levels()"
  const std::vector<FileMetaData*>* inputs(size_t compaction_input_level) {
    assert(compaction_input_level < inputs_.size());
    return &inputs_[compaction_input_level].files;
  }

  // Returns the LevelFilesBrief of the specified compaction input level.
  LevelFilesBrief* input_levels(size_t compaction_input_level) {
    return &input_levels_[compaction_input_level];
  }

  // Maximum size of files to build during this compaction.
  uint64_t max_output_file_size() const { return max_output_file_size_; }

  // What compression for output
  CompressionType output_compression() const { return output_compression_; }

  // Whether need to write output file to second DB path.
  uint32_t output_path_id() const { return output_path_id_; }

  // Is this a trivial compaction that can be implemented by just
  // moving a single input file to the next level (no merging or splitting)
  bool IsTrivialMove() const;

  // If true, then the compaction can be done by simply deleting input files.
  bool deletion_compaction() const { return deletion_compaction_; }

  // Add all inputs to this compaction as delete operations to *edit.
  void AddInputDeletions(VersionEdit* edit);

  // Returns true if the available information we have guarantees that
  // the input "user_key" does not exist in any level beyond "output_level()".
  bool KeyNotExistsBeyondOutputLevel(const Slice& user_key,
                                     std::vector<size_t>* level_ptrs) const;

  // Returns true iff we should stop building the current output
  // before processing "internal_key".
  bool ShouldStopBefore(const Slice& internal_key);

  // Clear all files to indicate that they are not being compacted
  // Delete this compaction from the list of running compactions.
  //
  // Requirement: DB mutex held
  void ReleaseCompactionFiles(Status status);

  // Returns the summary of the compaction in "output" with maximum "len"
  // in bytes.  The caller is responsible for the memory management of
  // "output".
  void Summary(char* output, int len);

  // Return the score that was used to pick this compaction run.
  double score() const { return score_; }

  // Is this compaction creating a file in the bottom most level?
  bool bottommost_level() { return bottommost_level_; }

  // Does this compaction include all sst files?
  bool is_full_compaction() { return is_full_compaction_; }

  // Was this compaction triggered manually by the client?
  bool is_manual_compaction() { return is_manual_compaction_; }

  // Used when allow_trivial_move option is set in
  // Universal compaction. If all the input files are
  // non overlapping, then is_trivial_move_ variable
  // will be set true, else false
  void set_is_trivial_move(bool trivial_move) {
    is_trivial_move_ = trivial_move;
  }

  // Used when allow_trivial_move option is set in
  // Universal compaction. Returns true, if the input files
  // are non-overlapping and can be trivially moved.
  bool is_trivial_move() { return is_trivial_move_; }

  // How many total levels are there?
  int number_levels() const { return number_levels_; }

  // Return the MutableCFOptions that should be used throughout the compaction
  // procedure
  const MutableCFOptions* mutable_cf_options() { return &mutable_cf_options_; }

  // Returns the size in bytes that the output file should be preallocated to.
  // In level compaction, that is max_file_size_. In universal compaction, that
  // is the sum of all input file sizes.
  uint64_t OutputFilePreallocationSize();

  void SetInputVersion(Version* input_version);

  struct InputLevelSummaryBuffer {
    char buffer[128];
  };

  const char* InputLevelSummary(InputLevelSummaryBuffer* scratch) const;

  uint64_t CalculateTotalInputSize() const;

  // In case of compaction error, reset the nextIndex that is used to pick up the next file to be
  // compacted from files_by_size_. Does nothing for universal compaction, since nextIndex is not
  // used in this case.
  void ResetNextCompactionIndex();

  // Create a CompactionFilter from compaction_filter_factory
  std::unique_ptr<CompactionFilter> CreateCompactionFilter() const;

  // Is the input level corresponding to output_level_ empty?
  bool IsOutputLevelEmpty() const;

  // Should this compaction be broken up into smaller ones run in parallel?
  bool ShouldFormSubcompactions() const;

  // test function to validate the functionality of IsBottommostLevel()
  // function -- determines if compaction with inputs and storage is bottommost
  static bool TEST_IsBottommostLevel(
      int output_level, VersionStorageInfo* vstorage,
      const std::vector<CompactionInputFiles>& inputs);

  TablePropertiesCollection GetOutputTableProperties() const {
    return output_table_properties_;
  }

  void SetOutputTableProperties(TablePropertiesCollection tp) {
    output_table_properties_ = std::move(tp);
  }

  Slice GetLargestUserKey() const { return largest_user_key_; }

  CompactionReason compaction_reason() { return compaction_reason_; }

  yb::PriorityThreadPoolSuspender* suspender() { return suspender_; }
  void SetSuspender(yb::PriorityThreadPoolSuspender* value) { suspender_ = value; }

 private:
  // mark (or clear) all files that are being compacted
  void MarkFilesBeingCompacted(bool mark_as_compacted);

  // get the smallest and largest key present in files to be compacted
  static void GetBoundaryKeys(VersionStorageInfo* vstorage,
                              const std::vector<CompactionInputFiles>& inputs,
                              Slice* smallest_key, Slice* largest_key);

  // helper function to determine if compaction with inputs and storage is
  // bottommost
  static bool IsBottommostLevel(
      int output_level, VersionStorageInfo* vstorage,
      const std::vector<CompactionInputFiles>& inputs);

  static bool IsFullCompaction(VersionStorageInfo* vstorage,
                               const std::vector<CompactionInputFiles>& inputs);

  const int start_level_;    // the lowest level to be compacted
  const int output_level_;  // levels to which output files are stored
  uint64_t max_output_file_size_ = 0;
  uint64_t max_grandparent_overlap_bytes_ = 0;
  MutableCFOptions mutable_cf_options_;
  Version* input_version_ = nullptr;
  uint64_t input_version_number_ = 0;
  bool input_version_level0_non_overlapping_ = false;
  VersionSet* vset_ = nullptr;
  VersionEdit edit_;
  const int number_levels_;
  ColumnFamilyData* cfd_;
  Arena arena_;          // Arena used to allocate space for file_levels_

  const uint32_t output_path_id_;
  CompressionType output_compression_;
  // If true, then the comaction can be done by simply deleting input files.
  const bool deletion_compaction_;

  // Compaction input files organized by level. Constant after construction
  const std::vector<CompactionInputFiles> inputs_;

  // A copy of inputs_, organized more closely in memory
  autovector<LevelFilesBrief, 2> input_levels_;

  // State used to check for number of of overlapping grandparent files
  // (grandparent == "output_level_ + 1")
  std::vector<FileMetaData*> grandparents_;
  size_t grandparent_index_;   // Index in grandparent_starts_
  std::atomic<bool> seen_key_; // Some output key has been seen
  uint64_t overlapped_bytes_;  // Bytes of overlap between current output
                               // and grandparent files
  const double score_;         // score that was used to pick this compaction.

  // Is this compaction creating a file in the bottom most level?
  const bool bottommost_level_;
  // Does this compaction include all sst files?
  const bool is_full_compaction_;

  // Is this compaction requested by the client?
  const bool is_manual_compaction_;

  // True if we can do trivial move in Universal multi level
  // compaction
  bool is_trivial_move_ = false;

  bool IsCompactionStyleUniversal() const;

  // Does input compression match the output compression?
  bool InputCompressionMatchesOutput() const;

  // table properties of output files
  TablePropertiesCollection output_table_properties_;

  // largest user keys in compaction
  Slice largest_user_key_;

  // Reason for compaction
  CompactionReason compaction_reason_;

  yb::PriorityThreadPoolSuspender* suspender_ = nullptr;
};

// Utility function
extern uint64_t TotalFileSize(const std::vector<FileMetaData*>& files);

}  // namespace rocksdb

#endif // YB_ROCKSDB_DB_COMPACTION_H
