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

#include <glog/logging.h>
#include <glog/stl_logging.h>
#include <gtest/gtest.h>

#include "yb/fs/fs_manager.h"
#include "yb/gutil/strings/util.h"
#include "yb/util/metrics.h"
#include "yb/util/test_macros.h"
#include "yb/util/test_util.h"

using std::shared_ptr;

DECLARE_string(fs_data_dirs);
DECLARE_string(fs_wal_dirs);

namespace yb {

class FsManagerTestBase : public YBTest {
 public:
  void SetUp() override {
    YBTest::SetUp();

    // Initialize File-System Layout
    ReinitFsManager();
    ASSERT_OK(fs_manager_->CreateInitialFileSystemLayout());
    ASSERT_OK(fs_manager_->Open());
  }

  void ReinitFsManager() {
    ReinitFsManager({ GetTestPath("fs_root") }, { GetTestPath("fs_root")} );
  }

  void ReinitFsManager(const vector<string>& wal_paths, const vector<string>& data_paths) {
    // Blow away the old memtrackers first.
    fs_manager_.reset();

    FsManagerOpts opts;
    opts.wal_paths = wal_paths;
    opts.data_paths = data_paths;
    opts.server_type = kServerType;
    fs_manager_.reset(new FsManager(env_.get(), opts));
  }

  void ReinitFsManager(const FsManagerOpts& opts) {
    fs_manager_.reset(new FsManager(env_.get(), opts));
  }

  void ValidateRootDataPaths(const string& data_path, const string& wal_path) {
    ASSERT_TRUE(HasPrefixString(fs_manager()->GetConsensusMetadataDir(), data_path));
    ASSERT_TRUE(HasPrefixString(fs_manager()->GetRaftGroupMetadataDir(), data_path));
    vector<string> data_dirs = fs_manager()->GetDataRootDirs();
    ASSERT_EQ(1, data_dirs.size());
    ASSERT_TRUE(HasPrefixString(data_dirs[0], data_path));
    if (!wal_path.empty()) {
      vector<string> wal_dirs = fs_manager()->GetWalRootDirs();
      ASSERT_EQ(1, wal_dirs.size());
      ASSERT_TRUE(HasPrefixString(wal_dirs[0], wal_path));
    }
  }

  void SetupFlagsAndBaseDirs(
      const string& data_path, const string& wal_path, FsManagerOpts* out_opts) {
    // Setup data in case empty.
    FLAGS_fs_data_dirs = data_path;
    if (!data_path.empty()) {
      string path = GetTestPath(data_path);
      ASSERT_OK(env_->CreateDir(path));
      FLAGS_fs_data_dirs = path;
    }
    // Setup wal in case empty.
    FLAGS_fs_wal_dirs = wal_path;
    if (!wal_path.empty()) {
      string path = GetTestPath(wal_path);
      ASSERT_OK(env_->CreateDir(path));
      FLAGS_fs_wal_dirs = path;
    }
    // Setup opts from flags and add server type.
    FsManagerOpts opts;
    opts.server_type = kServerType;
    *out_opts = opts;
  }

  void SetupForDelete(ShouldDeleteLogs delete_logs_dir = ShouldDeleteLogs::kFalse) {
    string path = GetTestPath("new_fs_root");
    ASSERT_OK(env_->CreateDir(path));

    ReinitFsManager(vector <string> (), {path});
    ASSERT_OK(fs_manager()->CreateInitialFileSystemLayout());
    ValidateRootDataPaths(path, "");

    log_dir_ = path + "/yb-data/logs";
    ASSERT_OK(env_->CreateDir(log_dir_));
    ASSERT_OK(fs_manager()->DeleteFileSystemLayout(delete_logs_dir));
  }

  void EnsureDataDirNotPresent() {
    std::vector <std::string> data_dirs = fs_manager()->GetDataRootDirs();
    for (auto data_dir : data_dirs) {
      bool is_dir = false;
      ASSERT_NOK(env_->IsDirectory(data_dir, &is_dir));
      ASSERT_FALSE(is_dir);
    }
  }

  FsManager *fs_manager() const { return fs_manager_.get(); }

  string log_dir() const { return log_dir_; }

  const char* kServerType = "tserver_test";

 private:
  std::unique_ptr<FsManager> fs_manager_;
  string log_dir_ = "";
};

TEST_F(FsManagerTestBase, TestIllegalPaths) {
  vector<string> illegal = { "", "asdf", "/foo\n\t" };
  for (const string& path : illegal) {
    ReinitFsManager({ path }, { path });
    ASSERT_TRUE(fs_manager()->CreateInitialFileSystemLayout().IsIOError());
  }
}

TEST_F(FsManagerTestBase, TestMultiplePaths) {
  vector<string> wal_paths = { GetTestPath("a"), GetTestPath("b"), GetTestPath("c") };
  vector<string> data_paths = { GetTestPath("a"), GetTestPath("b"), GetTestPath("c") };
  ReinitFsManager(wal_paths, data_paths);
  ASSERT_OK(fs_manager()->CreateInitialFileSystemLayout());
  ASSERT_OK(fs_manager()->Open());
}

TEST_F(FsManagerTestBase, TestMatchingPathsWithMismatchedSlashes) {
  vector<string> wal_paths = { GetTestPath("foo") };
  vector<string> data_paths = { wal_paths[0] + "/" };
  ReinitFsManager(wal_paths, data_paths);
  ASSERT_OK(fs_manager()->CreateInitialFileSystemLayout());
}

TEST_F(FsManagerTestBase, TestDuplicatePaths) {
  const string wal_path = GetTestPath("foo");
  const string data_path = GetTestPath("bar");
  ReinitFsManager({ wal_path, wal_path, wal_path }, { data_path, data_path, data_path });
  ASSERT_OK(fs_manager()->CreateInitialFileSystemLayout());
  ASSERT_EQ(
      vector<string>({ JoinPathSegments(
          GetServerTypeDataPath(wal_path, kServerType),
          fs_manager()->kWalDirName) }),
      fs_manager()->GetWalRootDirs());
  ASSERT_EQ(
      vector<string>({ JoinPathSegments(
          GetServerTypeDataPath(data_path, kServerType),
          fs_manager()->kDataDirName) }),
      fs_manager()->GetDataRootDirs());
}

TEST_F(FsManagerTestBase, TestListTablets) {
  vector<string> tablet_ids;
  ASSERT_OK(fs_manager()->ListTabletIds(&tablet_ids));
  ASSERT_EQ(0, tablet_ids.size());

  string path = fs_manager()->GetRaftGroupMetadataDir();
  std::unique_ptr<WritableFile> writer;
  ASSERT_OK(env_->NewWritableFile(
      JoinPathSegments(path, "foo.tmp"), &writer));
  ASSERT_OK(env_->NewWritableFile(
      JoinPathSegments(path, "foo.tmp.abc123"), &writer));
  ASSERT_OK(env_->NewWritableFile(
      JoinPathSegments(path, ".hidden"), &writer));
  ASSERT_OK(env_->NewWritableFile(
      JoinPathSegments(path, "a_tablet_sort_of"), &writer));

  ASSERT_OK(fs_manager()->ListTabletIds(&tablet_ids));
  ASSERT_EQ(1, tablet_ids.size()) << tablet_ids;
}

TEST_F(FsManagerTestBase, TestCannotUseNonEmptyFsRoot) {
  string path = GetTestPath("new_fs_root");
  ReinitFsManager({ path }, { path });
  ASSERT_OK(fs_manager()->CreateInitialFileSystemLayout());
  {
    std::unique_ptr<WritableFile> writer;
    ASSERT_OK(env_->NewWritableFile(
        JoinPathSegments(GetServerTypeDataPath(path, kServerType), "some_file"), &writer));
  }

  // Try to create the FS layout. It should fail.
  ASSERT_TRUE(fs_manager()->CreateInitialFileSystemLayout().IsAlreadyPresent());
}

TEST_F(FsManagerTestBase, TestEmptyDataPath) {
  ReinitFsManager(vector<string>(), vector<string>());
  Status s = fs_manager()->CreateInitialFileSystemLayout();
  ASSERT_TRUE(s.IsIOError());
  ASSERT_STR_CONTAINS(s.ToString(),
                      "List of data directories (fs_data_dirs) not provided");
}

TEST_F(FsManagerTestBase, TestOnlyDataPath) {
  string path = GetTestPath("new_fs_root");
  ASSERT_OK(env_->CreateDir(path));

  ReinitFsManager(vector<string>(), { path });
  ASSERT_OK(fs_manager()->CreateInitialFileSystemLayout());
  ValidateRootDataPaths(path, "");
}

TEST_F(FsManagerTestBase, TestPathsFromFlags) {
  FsManagerOpts opts;
  // If no wal specified, we inherit from fs_data_dirs
  {
    SetupFlagsAndBaseDirs("new_data_root_1", "", &opts);
    ReinitFsManager(opts);
    Status s = fs_manager()->CreateInitialFileSystemLayout();
    ASSERT_OK(s);
    ValidateRootDataPaths(FLAGS_fs_data_dirs, FLAGS_fs_data_dirs);
  }
  // If they are both set, we expect them to be whatever they are set to.
  {
    SetupFlagsAndBaseDirs("new_data_root_2", "new_wal_root_2", &opts);
    ReinitFsManager(opts);
    Status s = fs_manager()->CreateInitialFileSystemLayout();
    ASSERT_OK(s);
    ValidateRootDataPaths(FLAGS_fs_data_dirs, FLAGS_fs_wal_dirs);
  }

  // If no data_dirs is set, we expect a failure!
  {
    SetupFlagsAndBaseDirs("", "new_wal_root_3", &opts);
    ReinitFsManager(opts);
    Status s = fs_manager()->CreateInitialFileSystemLayout();
    ASSERT_TRUE(s.IsIOError());
    ASSERT_STR_CONTAINS(s.ToString(),
                        "List of data directories (fs_data_dirs) not provided");
  }
}

TEST_F(FsManagerTestBase, TestDataDirDeletedAndNotLogDir) {
  SetupForDelete();

  EnsureDataDirNotPresent();

  bool is_dir = false;
  ASSERT_OK(env_->IsDirectory(log_dir(), &is_dir));
  ASSERT_TRUE(is_dir);
}

TEST_F(FsManagerTestBase, TestLogDirAlsoDeleted) {
  SetupForDelete(ShouldDeleteLogs::kTrue);

  EnsureDataDirNotPresent();

  bool is_dir = false;
  ASSERT_NOK(env_->IsDirectory(log_dir(), &is_dir));
  ASSERT_FALSE(is_dir);
}

} // namespace yb
