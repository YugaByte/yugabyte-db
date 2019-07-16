// Copyright (c) YugaByte, Inc.

#include "yb/tserver/remote_bootstrap_session-test.h"
#include "yb/tablet/operations/snapshot_operation.h"

namespace yb {
namespace tserver {

using std::string;

using yb::tablet::enterprise::Tablet;

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
    ASSERT_OK(tablet()->CreateSnapshot(&tx_state));
  }
};

TEST_F(RemoteBootstrapRocksDBTest, CheckSuperBlockHasSnapshotFields) {
  auto superblock = session_->tablet_superblock();
  LOG(INFO) << superblock.ShortDebugString();
  ASSERT_TRUE(superblock.table_type() == YQL_TABLE_TYPE);
  ASSERT_TRUE(superblock.has_rocksdb_dir());

  const string& rocksdb_dir = superblock.rocksdb_dir();
  ASSERT_TRUE(env_->FileExists(rocksdb_dir));

  const string top_snapshots_dir = Tablet::SnapshotsDirName(rocksdb_dir);
  ASSERT_TRUE(env_->FileExists(top_snapshots_dir));

  const string snapshot_dir = JoinPathSegments(top_snapshots_dir, kSnapshotId);
  ASSERT_TRUE(env_->FileExists(snapshot_dir));

  vector<string> snapshot_files;
  ASSERT_OK(env_->GetChildren(snapshot_dir, &snapshot_files));

  // Ignore "." and ".." entries in snapshot_dir.
  ASSERT_EQ(superblock.snapshot_files().size(), snapshot_files.size() - 2);

  for (int i = 0; i < superblock.snapshot_files().size(); ++i) {
    const string& snapshot_id = superblock.snapshot_files(i).snapshot_id();
    const string& snapshot_file_name = superblock.snapshot_files(i).name();
    const uint64_t snapshot_file_size_bytes = superblock.snapshot_files(i).size_bytes();

    ASSERT_EQ(snapshot_id, kSnapshotId);

    const string file_path = JoinPathSegments(snapshot_dir, snapshot_file_name);
    ASSERT_TRUE(env_->FileExists(file_path));

    uint64 file_size_bytes = 0;
    ASSERT_OK(env_->GetFileSize(file_path, &file_size_bytes));
    ASSERT_EQ(snapshot_file_size_bytes, file_size_bytes);
  }
}

}  // namespace tserver
}  // namespace yb
