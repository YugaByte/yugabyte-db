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

#include "yb/tserver/remote_bootstrap_snapshots.h"

#include <unordered_set>

#include "yb/fs/fs_manager.h"

#include "yb/tablet/tablet_metadata.h"
#include "yb/tablet/tablet_peer.h"
#include "yb/tablet/tablet_snapshots.h"

namespace yb {
namespace tserver {

namespace {

CHECKED_STATUS AddDirToSnapshotFiles(
    const std::string& dir, const std::string& prefix, const std::string& snapshot_id,
    google::protobuf::RepeatedPtrField<tablet::SnapshotFilePB>* out) {
  auto files = VERIFY_RESULT_PREPEND(
      Env::Default()->GetChildren(dir, ExcludeDots::kTrue),
      Format("Unable to list directory $0", dir));

  for (const string& file : files) {
    LOG(INFO) << "Adding file " << file << " for snapshot " << snapshot_id;
    const auto path = JoinPathSegments(dir, file);
    const auto fname = prefix.empty() ? file : JoinPathSegments(prefix, file);

    if (VERIFY_RESULT(Env::Default()->IsDirectory(path))) {
      RETURN_NOT_OK(AddDirToSnapshotFiles(path, fname, snapshot_id, out));
      continue;
    }

    const uint64_t file_size = VERIFY_RESULT_PREPEND(
        Env::Default()->GetFileSize(path),
        Format("Unable to get file size for file $0", path));

    auto snapshot_file_pb = out->Add();
    auto& file_pb = *snapshot_file_pb->mutable_file();
    snapshot_file_pb->set_snapshot_id(snapshot_id);
    file_pb.set_name(fname);
    file_pb.set_size_bytes(file_size);
    file_pb.set_inode(VERIFY_RESULT(Env::Default()->GetFileINode(path)));
  }

  return Status::OK();
}

} // namespace

RemoteBootstrapSnapshotsComponent::RemoteBootstrapSnapshotsComponent(
    RemoteBootstrapFileDownloader* downloader, tablet::RaftGroupReplicaSuperBlockPB* new_superblock)
    : downloader_(*downloader), new_superblock_(*new_superblock) {}

Status RemoteBootstrapSnapshotsComponent::CreateDirectories(
    const std::string& db_dir, FsManager* fs) {
  const std::string top_snapshots_dir = tablet::TabletSnapshots::SnapshotsDirName(db_dir);
  // Create the snapshots directory.
  RETURN_NOT_OK_PREPEND(fs->CreateDirIfMissingAndSync(top_snapshots_dir),
                        Format("Failed to create & sync top snapshots directory $0",
                               top_snapshots_dir));
  return Status::OK();
}

Status RemoteBootstrapSnapshotsComponent::Download() {
  const auto& kv_store = new_superblock_.kv_store();
  const string& rocksdb_dir = kv_store.rocksdb_dir();
  const string top_snapshots_dir = tablet::TabletSnapshots::SnapshotsDirName(rocksdb_dir);
  // Create the snapshots directory first.
  RETURN_NOT_OK_PREPEND(fs_manager().CreateDirIfMissingAndSync(top_snapshots_dir),
                        Format("Failed to create & sync top snapshots directory $0",
                               top_snapshots_dir));

  DataIdPB data_id;
  data_id.set_type(DataIdPB::SNAPSHOT_FILE);
  // Used to skip snapshot files from a failed download in a previous iteration of the loop.
  std::unordered_set<SnapshotId> failed_snapshot_ids;
  for (auto const& file_pb : kv_store.snapshot_files()) {
    if (failed_snapshot_ids.find(file_pb.snapshot_id()) != failed_snapshot_ids.end()) {
      LOG(WARNING) << "Skipping download for file " << file_pb.file().name()
                   << " because it is part of failed snapshot " << file_pb.snapshot_id();
      continue;
    }
    const string snapshot_dir = JoinPathSegments(top_snapshots_dir, file_pb.snapshot_id());

    RETURN_NOT_OK_PREPEND(fs_manager().CreateDirIfMissingAndSync(snapshot_dir),
                          Format("Failed to create & sync snapshot directory $0", snapshot_dir));

    const std::string file_path = JoinPathSegments(snapshot_dir, file_pb.file().name());
    data_id.set_snapshot_id(file_pb.snapshot_id());
    auto s = downloader_.DownloadFile(file_pb.file(), snapshot_dir, &data_id);
    if (!s.ok()) {
      // If we fail to fetch a snapshot file, delete the snapshot directory, log the error,
      // but don't fail the remote bootstrap as snapshot files are not needed for running
      // the tablet.
      LOG(ERROR) << "Error downloading snapshot file " << file_path << ": " << s;
      failed_snapshot_ids.insert(file_pb.snapshot_id());
      LOG(INFO) << "Deleting snapshot dir " << snapshot_dir;
      auto delete_status = Env::Default()->DeleteRecursively(snapshot_dir);
      if (!delete_status.ok()) {
        LOG(ERROR) << "Error deleting corrupted snapshot directory "
                   << snapshot_dir << ": " << delete_status;
      }
    } else {
      LOG(INFO) << "Downloaded file " << file_path << " for snapshot " << file_pb.snapshot_id();
    }
  }

  return Status::OK();
}

Status RemoteBootstrapSnapshotsSource::Init() {
  const auto& metadata = tablet_peer_->tablet_metadata();
  auto* kv_store = tablet_superblock_.mutable_kv_store();
  kv_store->clear_snapshot_files();

  // Add snapshot files to tablet superblock.
  const std::string top_snapshots_dir = tablet::TabletSnapshots::SnapshotsDirName(
      kv_store->rocksdb_dir());
  std::vector<std::string> snapshots;

  if (metadata->fs_manager()->env()->FileExists(top_snapshots_dir)) {
    snapshots = VERIFY_RESULT_PREPEND(
        metadata->fs_manager()->ListDir(top_snapshots_dir),
        Format("Unable to list directory $0", top_snapshots_dir));
  }

  for (const string& dir_name : snapshots) {
    const std::string snapshot_dir = JoinPathSegments(top_snapshots_dir, dir_name);
    if (tablet::TabletSnapshots::IsTempSnapshotDir(snapshot_dir)) {
      continue;
    }

    // Ignore any non-directories (folder check-sum files, for example).
    if (!VERIFY_RESULT(Env::Default()->IsDirectory(snapshot_dir))) {
      continue;
    }

    RETURN_NOT_OK(AddDirToSnapshotFiles(
        snapshot_dir, "", dir_name, kv_store->mutable_snapshot_files()));
  }

  return Status::OK();
}

Status RemoteBootstrapSnapshotsSource::GetDataPiece(
    const DataIdPB& data_id, GetDataPieceInfo* info) {
  const string snapshots_dir =
      tablet::TabletSnapshots::SnapshotsDirName(tablet_superblock_.kv_store().rocksdb_dir());
  return RemoteBootstrapSession::GetFilePiece(
      JoinPathSegments(snapshots_dir, data_id.snapshot_id()), data_id.file_name(),
      tablet_peer_->tablet_metadata()->fs_manager()->env(), info);
}

Status RemoteBootstrapSnapshotsSource::ValidateDataId(const DataIdPB& data_id) {
  if (data_id.snapshot_id().empty()) {
    return STATUS(InvalidArgument,
                  "snapshot id must be specified for type == SNAPSHOT_FILE",
                  data_id.ShortDebugString());
  }
  if (data_id.file_name().empty()) {
    return STATUS(InvalidArgument,
                  "file name must be specified for type == SNAPSHOT_FILE",
                  data_id.ShortDebugString());
  }
  return Status::OK();
}

} // namespace tserver
} // namespace yb
