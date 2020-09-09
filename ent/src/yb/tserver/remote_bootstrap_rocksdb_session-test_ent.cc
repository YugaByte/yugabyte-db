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

#include "yb/tserver/remote_bootstrap_session-test.h"

#include "yb/tablet/tablet_snapshots.h"
#include "yb/tablet/operations/snapshot_operation.h"

namespace yb {
namespace tserver {

using std::string;

using yb::tablet::Tablet;

static const string kSnapshotId = "0123456789ABCDEF0123456789ABCDEF";

class RemoteBootstrapRocksDBTest : public RemoteBootstrapTest {
 public:
  RemoteBootstrapRocksDBTest() : RemoteBootstrapTest(YQL_TABLE_TYPE) {}

  void InitSession() override {
    CreateSnapshot();
    RemoteBootstrapTest::InitSession();
  }

  void CreateSnapshot() {
    LOG(INFO) << "Creating Snapshot " << kSnapshotId << " ...";
    TabletSnapshotOpRequestPB request;
    request.set_snapshot_id(kSnapshotId);
    tablet::SnapshotOperationState tx_state(tablet().get(), &request);
    tx_state.set_hybrid_time(tablet()->clock()->Now());
    tablet_peer_->log()->GetLatestEntryOpId().ToPB(tx_state.mutable_op_id());
    ASSERT_OK(tablet()->snapshots().Create(&tx_state));

    // Create extra file to check that it will not break snapshot files collecting
    // inside RemoteBootstrapSession::InitSession().
    const string rocksdb_dir = tablet()->metadata()->rocksdb_dir();
    const string top_snapshots_dir = tablet()->metadata()->snapshots_dir();
    const string snapshot_dir = JoinPathSegments(top_snapshots_dir, kSnapshotId);
    ASSERT_TRUE(env_->FileExists(snapshot_dir));

    const string extra_file = snapshot_dir + ".sha256";
    ASSERT_FALSE(env_->FileExists(extra_file));
    {
      std::unique_ptr<WritableFile> writer;
      ASSERT_OK(env_->NewWritableFile(extra_file, &writer));
      ASSERT_OK(writer->Append(Slice("012345")));
      ASSERT_OK(writer->Flush(WritableFile::FLUSH_SYNC));
      ASSERT_OK(writer->Close());
    }
    ASSERT_TRUE(env_->FileExists(extra_file));
  }
};

TEST_F(RemoteBootstrapRocksDBTest, CheckSuperBlockHasSnapshotFields) {
  auto superblock = session_->tablet_superblock();
  LOG(INFO) << superblock.ShortDebugString();
  ASSERT_TRUE(superblock.obsolete_table_type() == YQL_TABLE_TYPE);

  const auto& kv_store = superblock.kv_store();
  ASSERT_TRUE(kv_store.has_rocksdb_dir());

  const string& rocksdb_dir = kv_store.rocksdb_dir();
  ASSERT_TRUE(env_->FileExists(rocksdb_dir));

  const string top_snapshots_dir = tablet::TabletSnapshots::SnapshotsDirName(rocksdb_dir);
  ASSERT_TRUE(env_->FileExists(top_snapshots_dir));

  const string snapshot_dir = JoinPathSegments(top_snapshots_dir, kSnapshotId);
  ASSERT_TRUE(env_->FileExists(snapshot_dir));

  vector<string> snapshot_files;
  ASSERT_OK(env_->GetChildren(snapshot_dir, &snapshot_files));

  // Ignore "." and ".." entries in snapshot_dir.
  ASSERT_EQ(kv_store.snapshot_files().size(), snapshot_files.size() - 2)
      << "KV store snapshot files: " << AsString(kv_store.snapshot_files())
      << ", filesystem: " << AsString(snapshot_files);

  for (int i = 0; i < kv_store.snapshot_files().size(); ++i) {
    const auto& snapshot_file = kv_store.snapshot_files(i);
    const string& snapshot_id = snapshot_file.snapshot_id();
    const string& snapshot_file_name = snapshot_file.file().name();
    const uint64_t snapshot_file_size_bytes = snapshot_file.file().size_bytes();

    ASSERT_EQ(snapshot_id, kSnapshotId);

    const string file_path = JoinPathSegments(snapshot_dir, snapshot_file_name);
    ASSERT_TRUE(env_->FileExists(file_path));

    uint64 file_size_bytes = ASSERT_RESULT(env_->GetFileSize(file_path));
    ASSERT_EQ(snapshot_file_size_bytes, file_size_bytes);
  }
}

}  // namespace tserver
}  // namespace yb
