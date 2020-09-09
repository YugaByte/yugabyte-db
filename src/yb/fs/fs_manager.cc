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

#include "yb/fs/fs_manager.h"

#include <deque>
#include <iostream>
#include <map>
#include <unordered_set>

#include <boost/algorithm/string/predicate.hpp>

#include <glog/logging.h>
#include <glog/stl_logging.h>
#include <google/protobuf/message.h>

#include "yb/fs/fs.pb.h"
#include "yb/gutil/map-util.h"
#include "yb/gutil/strings/join.h"
#include "yb/gutil/strings/numbers.h"
#include "yb/gutil/strings/split.h"
#include "yb/gutil/strings/strip.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/gutil/strings/util.h"
#include "yb/gutil/strtoint.h"
#include "yb/gutil/walltime.h"
#include "yb/util/env_util.h"
#include "yb/util/flag_tags.h"
#include "yb/util/net/net_util.h"
#include "yb/util/oid_generator.h"
#include "yb/util/path_util.h"
#include "yb/util/pb_util.h"

DEFINE_bool(enable_data_block_fsync, true,
            "Whether to enable fsync() of data blocks, metadata, and their parent directories. "
            "Disabling this flag may cause data loss in the event of a system crash.");
TAG_FLAG(enable_data_block_fsync, unsafe);

DECLARE_string(fs_data_dirs);

DEFINE_string(fs_wal_dirs, "",
              "Comma-separated list of directories for write-ahead logs. This is an optional "
                  "argument. If this is not specified, fs_data_dirs is used for write-ahead logs "
                  "also and that's a reasonable default for most use cases.");
TAG_FLAG(fs_wal_dirs, stable);

DEFINE_string(instance_uuid_override, "",
              "When creating local instance metadata (for master or tserver) in an empty data "
              "directory, use this UUID instead of randomly-generated one. Can be used to replace "
              "a node that had its disk wiped in some scenarios.");

DEFINE_test_flag(bool, simulate_fs_create_failure, false,
                 "Simulate failure during initial creation of fs during the first time "
                 "process creation.");

using google::protobuf::Message;
using yb::env_util::ScopedFileDeleter;
using std::map;
using std::unordered_set;
using strings::Substitute;

namespace yb {

// ==========================================================================
//  FS Paths
// ==========================================================================
const char *FsManager::kWalDirName = "wals";
const char *FsManager::kWalFileNamePrefix = "wal";
const char *FsManager::kWalsRecoveryDirSuffix = ".recovery";
const char *FsManager::kRocksDBDirName = "rocksdb";
const char *FsManager::kRaftGroupMetadataDirName = "tablet-meta";
const char *FsManager::kDataDirName = "data";
const char *FsManager::kCorruptedSuffix = ".corrupted";
const char *FsManager::kInstanceMetadataFileName = "instance";
const char *FsManager::kFsLockFileName = "fs-lock";
const char *FsManager::kConsensusMetadataDirName = "consensus-meta";
const char *FsManager::kLogsDirName = "logs";

static const char* const kTmpInfix = ".tmp";

FsManagerOpts::FsManagerOpts()
    : read_only(false) {
  if (FLAGS_fs_wal_dirs.empty() && !FLAGS_fs_data_dirs.empty()) {
    // It is sufficient if user sets the data dirs. By default we use the same
    // directories for WALs as well.
    FLAGS_fs_wal_dirs = FLAGS_fs_data_dirs;
  }
  wal_paths = strings::Split(FLAGS_fs_wal_dirs, ",", strings::SkipEmpty());
  data_paths = strings::Split(FLAGS_fs_data_dirs, ",", strings::SkipEmpty());
}

FsManagerOpts::~FsManagerOpts() {
}

FsManager::FsManager(Env* env, const string& root_path, const std::string& server_type)
    : env_(DCHECK_NOTNULL(env)),
      read_only_(false),
      wal_fs_roots_({ root_path }),
      data_fs_roots_({ root_path }),
      server_type_(server_type),
      metric_entity_(nullptr),
      initted_(false) {
}

FsManager::FsManager(Env* env,
                     const FsManagerOpts& opts)
    : env_(DCHECK_NOTNULL(env)),
      read_only_(opts.read_only),
      wal_fs_roots_(opts.wal_paths),
      data_fs_roots_(opts.data_paths),
      server_type_(opts.server_type),
      metric_entity_(opts.metric_entity),
      parent_mem_tracker_(opts.parent_mem_tracker),
      initted_(false) {
}

FsManager::~FsManager() {
}

Status FsManager::Init() {
  if (initted_) {
    return Status::OK();
  }

  // The wal root must be set.
  if (data_fs_roots_.empty()) {
    return STATUS(IOError, "List of data directories (fs_data_dirs) not provided");
  }

  // Deduplicate all of the roots.
  set<string> all_roots;
  for (const string& wal_fs_root : wal_fs_roots_) {
    all_roots.insert(wal_fs_root);
  }
  for (const string& data_fs_root : data_fs_roots_) {
    all_roots.insert(data_fs_root);
  }

  // Build a map of original root --> canonicalized root, sanitizing each
  // root a bit as we go.
  typedef map<string, string> RootMap;
  RootMap canonicalized_roots;
  for (const string& root : all_roots) {
    if (root.empty()) {
      return STATUS(IOError, "Empty string provided for filesystem root");
    }
    if (root[0] != '/') {
      return STATUS(IOError,
          Substitute("Relative path $0 provided for filesystem root", root));
    }
    {
      string root_copy = root;
      StripWhiteSpace(&root_copy);
      if (root != root_copy) {
        return STATUS(IOError,
                  Substitute("Filesystem root $0 contains illegal whitespace", root));
      }
    }

    // Strip the basename when canonicalizing, as it may not exist. The
    // dirname, however, must exist.
    string canonicalized;
    Status s = env_->Canonicalize(DirName(root), &canonicalized);
    if (!s.ok()) {
      return STATUS(
          InvalidArgument, strings::Substitute(
          "Cannot create directory for YB data, please check the --fs_data_dirs parameter "
          "(Passed: $0). Path does not exist: $1\nDetails: $2",
          FLAGS_fs_data_dirs, root, s.ToString()));
    }
    canonicalized = JoinPathSegments(canonicalized, BaseName(root));
    InsertOrDie(&canonicalized_roots, root, canonicalized);
  }

  // All done, use the map to set the canonicalized state.
  for (const auto& wal_fs_root : wal_fs_roots_) {
    canonicalized_wal_fs_roots_.insert(FindOrDie(canonicalized_roots, wal_fs_root));
  }
  if (!data_fs_roots_.empty()) {
    canonicalized_metadata_fs_root_ = FindOrDie(canonicalized_roots, data_fs_roots_[0]);
    for (const string& data_fs_root : data_fs_roots_) {
      canonicalized_data_fs_roots_.insert(FindOrDie(canonicalized_roots, data_fs_root));
    }
  } else {
    LOG(FATAL) << "Data directories (fs_data_dirs) must be specified";
  }

  for (const RootMap::value_type& e : canonicalized_roots) {
    canonicalized_all_fs_roots_.insert(e.second);
  }

  if (VLOG_IS_ON(1)) {
    VLOG(1) << "WAL roots: " << canonicalized_wal_fs_roots_;
    VLOG(1) << "Metadata root: " << canonicalized_metadata_fs_root_;
    VLOG(1) << "Data roots: " << canonicalized_data_fs_roots_;
    VLOG(1) << "All roots: " << canonicalized_all_fs_roots_;
  }

  initted_ = true;
  return Status::OK();
}

Status FsManager::Open() {
  RETURN_NOT_OK(Init());

  if (HasAnyLockFiles()) {
    return STATUS(Corruption, "Lock file is present, filesystem may be in inconsistent state");
  }

  for (const string& root : canonicalized_all_fs_roots_) {
    gscoped_ptr<InstanceMetadataPB> pb(new InstanceMetadataPB);
    RETURN_NOT_OK(pb_util::ReadPBContainerFromPath(env_, GetInstanceMetadataPath(root), pb.get()));
    if (!metadata_) {
      metadata_.reset(pb.release());
    } else if (pb->uuid() != metadata_->uuid()) {
      return STATUS(Corruption, Substitute(
          "Mismatched UUIDs across filesystem roots: $0 vs. $1",
          metadata_->uuid(), pb->uuid()));
    }
  }

  LOG(INFO) << "Opened local filesystem: " << JoinStrings(canonicalized_all_fs_roots_, ",")
            << std::endl << metadata_->DebugString();
  return Status::OK();
}

bool FsManager::HasAnyLockFiles() {
  for (const string& root : canonicalized_all_fs_roots_) {
    if (Exists(GetFsLockFilePath(root))) {
      LOG(INFO) << "Found lock file in dir " << root;
      return true;
    }
  }

  return false;
}

Status FsManager::DeleteLockFiles() {
  CHECK(!read_only_);
  vector<string> removal_list;
  for (const string& root : canonicalized_all_fs_roots_) {
    std::string lock_file_path = GetFsLockFilePath(root);
    if (Exists(lock_file_path)) {
      removal_list.push_back(lock_file_path);
    }
  }

  for (const string& target : removal_list) {
    RETURN_NOT_OK_PREPEND(env_->DeleteFile(target), "Lock file delete failed");
  }

  return Status::OK();
}

Status FsManager::DeleteFileSystemLayout(ShouldDeleteLogs also_delete_logs) {
  CHECK(!read_only_);
  set<string> removal_set;
  if (also_delete_logs) {
    removal_set = canonicalized_all_fs_roots_;
  } else {
    auto removal_list = GetWalRootDirs();
    removal_list.push_back(GetRaftGroupMetadataDir());
    removal_list.push_back(GetConsensusMetadataDir());
    for (const string& root : canonicalized_all_fs_roots_) {
      removal_list.push_back(GetInstanceMetadataPath(root));
    }
    auto data_dirs = GetDataRootDirs();
    removal_list.insert(removal_list.begin(), data_dirs.begin(), data_dirs.end());
    removal_set.insert(removal_list.begin(), removal_list.end());
  }

  for (const string& target : removal_set) {
    bool is_dir = false;
    Status s = env_->IsDirectory(target, &is_dir);
    if (!s.ok()) {
      LOG(WARNING) << "Error: " << s.ToString() << " when checking if " << target
                   << " is a directory.";
      continue;
    }
    if (is_dir) {
      RETURN_NOT_OK(env_->DeleteRecursively(target));
    } else {
      RETURN_NOT_OK(env_->DeleteFile(target));
    }
  }

  RETURN_NOT_OK(DeleteLockFiles());

  return Status::OK();
}

Status FsManager::CreateInitialFileSystemLayout(bool delete_fs_if_lock_found) {
  CHECK(!read_only_);

  RETURN_NOT_OK(Init());

  bool fs_cleaned = false;

  // If lock file is present, delete existing filesystem layout before continuing.
  if (delete_fs_if_lock_found && HasAnyLockFiles()) {
    RETURN_NOT_OK(DeleteFileSystemLayout());
    fs_cleaned = true;
  }

  // It's OK if a root already exists as long as there's nothing in it.
  for (const string& root : canonicalized_all_fs_roots_) {
    if (!env_->FileExists(GetServerTypeDataPath(root, server_type_))) {
      // We'll create the directory below.
      continue;
    }
    bool is_empty;
    RETURN_NOT_OK_PREPEND(IsDirectoryEmpty(GetServerTypeDataPath(root, server_type_), &is_empty),
                          "Unable to check if FSManager root is empty");
    if (!is_empty) {
      return STATUS(AlreadyPresent, "FSManager root is not empty", root);
    }
  }

  // All roots are either empty or non-existent. Create missing roots and all
  // subdirectories.
  //
  // In the event of failure, delete everything we created.
  std::deque<std::unique_ptr<ScopedFileDeleter>> delete_on_failure;
  unordered_set<string> to_sync;

  for (const string& root : canonicalized_all_fs_roots_) {
    bool created;
    std::string out_dir;
    RETURN_NOT_OK(SetupRootDir(env_, root, server_type_, &out_dir, &created));
    if (created) {
      delete_on_failure.emplace_front(new ScopedFileDeleter(env_, out_dir));
      to_sync.insert(DirName(out_dir));
    }
    const string lock_file_path = GetFsLockFilePath(root);
    if (fs_cleaned || !Exists(lock_file_path)) {
      std::unique_ptr<WritableFile> file;
      RETURN_NOT_OK_PREPEND(env_->NewWritableFile(lock_file_path, &file),
                            "Unable to create lock file.");
      // Do not delete lock file on error. It is used to detect failed initial create.
    }
  }

  InstanceMetadataPB metadata;
  CreateInstanceMetadata(&metadata);
  for (const string& root : canonicalized_all_fs_roots_) {
    const string instance_metadata_path = GetInstanceMetadataPath(root);
    RETURN_NOT_OK_PREPEND(WriteInstanceMetadata(metadata, instance_metadata_path),
                          "Unable to write instance metadata");
    delete_on_failure.emplace_front(new ScopedFileDeleter(env_, instance_metadata_path));
  }

  // Initialize ancillary directories.
  auto ancillary_dirs = GetWalRootDirs();
  ancillary_dirs.push_back(GetRaftGroupMetadataDir());
  ancillary_dirs.push_back(GetConsensusMetadataDir());

  for (const string& dir : ancillary_dirs) {
    bool created;
    RETURN_NOT_OK_PREPEND(CreateDirIfMissing(dir, &created),
                          Substitute("Unable to create directory $0", dir));
    if (created) {
      delete_on_failure.emplace_front(new ScopedFileDeleter(env_, dir));
      to_sync.insert(DirName(dir));
    }
  }

  // Ensure newly created directories are synchronized to disk.
  if (FLAGS_enable_data_block_fsync) {
    for (const string& dir : to_sync) {
      RETURN_NOT_OK_PREPEND(env_->SyncDir(dir),
                            Substitute("Unable to synchronize directory $0", dir));
    }
  }

  // Create the RocksDB directory under each data directory.
  for (const string& data_root : GetDataRootDirs()) {
    bool created = false;
    RETURN_NOT_OK_PREPEND(CreateDirIfMissing(data_root, &created),
                          Substitute("Unable to create directory $0", data_root));
    if (created) {
      delete_on_failure.emplace_front(new ScopedFileDeleter(env_, data_root));
      to_sync.insert(DirName(data_root));
    }

    const string dir = JoinPathSegments(data_root, kRocksDBDirName);
    created = false;
    RETURN_NOT_OK_PREPEND(CreateDirIfMissing(dir, &created),
                          Substitute("Unable to create directory $0", dir));
    if (created) {
      delete_on_failure.emplace_front(new ScopedFileDeleter(env_, dir));
      to_sync.insert(DirName(dir));
    }
  }

  if (FLAGS_TEST_simulate_fs_create_failure) {
    return STATUS(IOError, "Simulated fs creation error");
  }

  // Success: don't delete any files.
  for (const auto& deleter : delete_on_failure) {
    deleter->Cancel();
  }

  RETURN_NOT_OK(DeleteLockFiles());

  return Status::OK();
}

void FsManager::CreateInstanceMetadata(InstanceMetadataPB* metadata) {
  ObjectIdGenerator oid_generator;
  if (!FLAGS_instance_uuid_override.empty()) {
    metadata->set_uuid(FLAGS_instance_uuid_override);
  } else {
    metadata->set_uuid(oid_generator.Next());
  }

  string time_str;
  StringAppendStrftime(&time_str, "%Y-%m-%d %H:%M:%S", time(nullptr), false);
  string hostname;
  if (!GetHostname(&hostname).ok()) {
    hostname = "<unknown host>";
  }
  metadata->set_format_stamp(Substitute("Formatted at $0 on $1", time_str, hostname));
}

Status FsManager::WriteInstanceMetadata(const InstanceMetadataPB& metadata,
                                        const string& path) {
  // The instance metadata is written effectively once per TS, so the
  // durability cost is negligible.
  RETURN_NOT_OK(pb_util::WritePBContainerToPath(env_, path,
                                                metadata,
                                                pb_util::NO_OVERWRITE,
                                                pb_util::SYNC));
  LOG(INFO) << "Generated new instance metadata in path " << path << ":\n"
            << metadata.DebugString();
  return Status::OK();
}

Status FsManager::IsDirectoryEmpty(const string& path, bool* is_empty) {
  vector<string> children;
  RETURN_NOT_OK(env_->GetChildren(path, &children));
  for (const string& child : children) {
    // Excluding logs directory from the list of things to check for.
    if (child == "." || child == ".." || child == kLogsDirName) {
      continue;
    } else {
      LOG(INFO) << "Found data " << child;
      *is_empty = false;
      return Status::OK();
    }
  }
  *is_empty = true;
  return Status::OK();
}

Status FsManager::CreateDirIfMissing(const string& path, bool* created) {
  return env_util::CreateDirIfMissing(env_, path, created);
}

Status FsManager::CreateDirIfMissingAndSync(const std::string& path, bool* created) {
  RETURN_NOT_OK_PREPEND(CreateDirIfMissing(path, created),
                        Substitute("Failed to create directory $0", path));
  RETURN_NOT_OK_PREPEND(env_->SyncDir(DirName(path)),
                        Substitute("Failed to sync root directory $0", DirName(path)));
  return Status::OK();
}

const string& FsManager::uuid() const {
  return CHECK_NOTNULL(metadata_.get())->uuid();
}

vector<string> FsManager::GetDataRootDirs() const {
  // Add the data subdirectory to each data root.
  vector<string> data_paths;
  for (const string& data_fs_root : canonicalized_data_fs_roots_) {
    data_paths.push_back(
        JoinPathSegments(GetServerTypeDataPath(data_fs_root, server_type_), kDataDirName));
  }
  return data_paths;
}

vector<string> FsManager::GetWalRootDirs() const {
  DCHECK(initted_);
  vector<string> wal_dirs;
  for (const auto& canonicalized_wal_fs_root : canonicalized_wal_fs_roots_) {
    wal_dirs.push_back(JoinPathSegments(
          GetServerTypeDataPath(canonicalized_wal_fs_root, server_type_), kWalDirName));
  }
  return wal_dirs;
}

std::string FsManager::GetRaftGroupMetadataDir(const std::string& data_dir) {
  return JoinPathSegments(data_dir, kRaftGroupMetadataDirName);
}

string FsManager::GetRaftGroupMetadataDir() const {
  DCHECK(initted_);
  return GetRaftGroupMetadataDir(
      GetServerTypeDataPath(canonicalized_metadata_fs_root_, server_type_));
}

string FsManager::GetRaftGroupMetadataPath(const string& tablet_id) const {
  return JoinPathSegments(GetRaftGroupMetadataDir(), tablet_id);
}

namespace {
// Return true if 'fname' is a valid tablet ID.
bool IsValidTabletId(const std::string& fname) {
  if (fname.find(kTmpInfix) != string::npos) {
    LOG(WARNING) << "Ignoring tmp file in tablet metadata dir: " << fname;
    return false;
  }

  if (HasPrefixString(fname, ".")) {
    // Hidden file or ./..
    VLOG(1) << "Ignoring hidden file in tablet metadata dir: " << fname;
    return false;
  }

  return true;
}
} // anonymous namespace

Status FsManager::ListTabletIds(vector<string>* tablet_ids) {
  string dir = GetRaftGroupMetadataDir();
  vector<string> children;
  RETURN_NOT_OK_PREPEND(ListDir(dir, &children),
                        Substitute("Couldn't list tablets in metadata directory $0", dir));

  vector<string> tablets;
  for (const string& child : children) {
    if (!IsValidTabletId(child)) {
      continue;
    }
    tablet_ids->push_back(child);
  }
  return Status::OK();
}

std::string FsManager::GetInstanceMetadataPath(const string& root) const {
  return JoinPathSegments(GetServerTypeDataPath(root, server_type_), kInstanceMetadataFileName);
}

std::string FsManager::GetFsLockFilePath(const string& root) const {
  return JoinPathSegments(GetServerTypeDataPath(root, server_type_), kFsLockFileName);
}

std::string FsManager::GetConsensusMetadataDir() const {
  DCHECK(initted_);
  return GetConsensusMetadataDir(
      GetServerTypeDataPath(canonicalized_metadata_fs_root_, server_type_));
}

std::string FsManager::GetConsensusMetadataDir(const std::string& data_dir) {
  return JoinPathSegments(data_dir, kConsensusMetadataDirName);
}

std::string FsManager::GetFirstTabletWalDirOrDie(const std::string& table_id,
                                                 const std::string& tablet_id) const {
  auto wal_root_dirs = GetWalRootDirs();
  CHECK(!wal_root_dirs.empty()) << "No WAL directories specified";
  auto table_wal_dir = JoinPathSegments(wal_root_dirs[0], Substitute("table-$0", table_id));
  return JoinPathSegments(table_wal_dir, Substitute("tablet-$0", tablet_id));
}

std::string FsManager::GetTabletWalRecoveryDir(const string& tablet_wal_path) {
  return tablet_wal_path + kWalsRecoveryDirSuffix;
}

namespace {

const auto kWalFileNameFullPrefix = std::string(FsManager::kWalFileNamePrefix) + "-";

} // namespace

std::string FsManager::GetWalSegmentFileName(uint64_t sequence_number) {
  return Format("$0$1", kWalFileNameFullPrefix, StringPrintf("%09" PRIu64, sequence_number));
}

bool FsManager::IsWalSegmentFileName(const std::string& file_name) {
  return boost::starts_with(file_name, kWalFileNameFullPrefix);
}

// ==========================================================================
//  Dump/Debug utils
// ==========================================================================

void FsManager::DumpFileSystemTree(ostream& out) {
  DCHECK(initted_);

  for (const string& root : canonicalized_all_fs_roots_) {
    out << "File-System Root: " << root << std::endl;

    std::vector<string> objects;
    Status s = env_->GetChildren(root, &objects);
    if (!s.ok()) {
      LOG(ERROR) << "Unable to list the fs-tree: " << s.ToString();
      return;
    }

    DumpFileSystemTree(out, "|-", root, objects);
  }
}

void FsManager::DumpFileSystemTree(ostream& out, const string& prefix,
                                   const string& path, const vector<string>& objects) {
  for (const string& name : objects) {
    if (name == "." || name == "..") continue;

    std::vector<string> sub_objects;
    string sub_path = JoinPathSegments(path, name);
    Status s = env_->GetChildren(sub_path, &sub_objects);
    if (s.ok()) {
      out << prefix << name << "/" << std::endl;
      DumpFileSystemTree(out, prefix + "---", sub_path, sub_objects);
    } else {
      out << prefix << name << std::endl;
    }
  }
}

} // namespace yb
