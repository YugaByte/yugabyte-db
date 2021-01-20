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

#include "yb/client/table.h"

#include "yb/client/client.h"
#include "yb/client/client-internal.h"
#include "yb/client/yb_op.h"

#include "yb/master/master.pb.h"
#include "yb/master/master.proxy.h"

#include "yb/util/backoff_waiter.h"
#include "yb/util/status.h"

DEFINE_int32(
    max_num_tablets_for_table, 5000,
    "Max number of tablets that can be specified in a CREATE TABLE statement");

namespace yb {
namespace client {

Status YBTable::PBToClientTableType(
    TableType table_type_from_pb,
    YBTableType* client_table_type) {
  switch (table_type_from_pb) {
    case TableType::YQL_TABLE_TYPE:
      *client_table_type = YBTableType::YQL_TABLE_TYPE;
      return Status::OK();
    case TableType::REDIS_TABLE_TYPE:
      *client_table_type = YBTableType::REDIS_TABLE_TYPE;
      return Status::OK();
    case TableType::PGSQL_TABLE_TYPE:
      *client_table_type = YBTableType::PGSQL_TABLE_TYPE;
      return Status::OK();
    case TableType::TRANSACTION_STATUS_TABLE_TYPE:
      *client_table_type = YBTableType::TRANSACTION_STATUS_TABLE_TYPE;
      return Status::OK();
  }

  *client_table_type = YBTableType::UNKNOWN_TABLE_TYPE;
  return STATUS(InvalidArgument, strings::Substitute(
    "Invalid table type from master response: $0", table_type_from_pb));
}

TableType YBTable::ClientToPBTableType(YBTableType table_type) {
  switch (table_type) {
    case YBTableType::YQL_TABLE_TYPE:
      return TableType::YQL_TABLE_TYPE;
    case YBTableType::REDIS_TABLE_TYPE:
      return TableType::REDIS_TABLE_TYPE;
    case YBTableType::PGSQL_TABLE_TYPE:
      return TableType::PGSQL_TABLE_TYPE;
    case YBTableType::TRANSACTION_STATUS_TABLE_TYPE:
      return TableType::TRANSACTION_STATUS_TABLE_TYPE;
    case YBTableType::UNKNOWN_TABLE_TYPE:
      break;
  }
  FATAL_INVALID_ENUM_VALUE(YBTableType, table_type);
  // Returns a dummy value to avoid compilation warning.
  return TableType::DEFAULT_TABLE_TYPE;
}

YBTable::YBTable(client::YBClient* client, const YBTableInfo& info)
    : client_(client),
      // The table type is set after the table is opened.
      table_type_(YBTableType::UNKNOWN_TABLE_TYPE),
      info_(info) {
}

YBTable::~YBTable() {
}

//--------------------------------------------------------------------------------------------------

const YBTableName& YBTable::name() const {
  return info_.table_name;
}

YBTableType YBTable::table_type() const {
  return table_type_;
}

const string& YBTable::id() const {
  return info_.table_id;
}

YBClient* YBTable::client() const {
  return client_;
}

const YBSchema& YBTable::schema() const {
  return info_.schema;
}

const Schema& YBTable::InternalSchema() const {
  return internal::GetSchema(info_.schema);
}

const IndexMap& YBTable::index_map() const {
  return info_.index_map;
}

bool YBTable::IsIndex() const {
  return info_.index_info != boost::none;
}

bool YBTable::IsUniqueIndex() const {
  return info_.index_info.is_initialized() && info_.index_info->is_unique();
}

const IndexInfo& YBTable::index_info() const {
  static IndexInfo kEmptyIndexInfo;
  if (info_.index_info) {
    return *info_.index_info;
  }
  return kEmptyIndexInfo;
}

const bool YBTable::colocated() const {
  return info_.colocated;
}

const boost::optional<master::ReplicationInfoPB>& YBTable::replication_info() const {
  return info_.replication_info;
}

std::string YBTable::ToString() const {
  return strings::Substitute(
      "$0 $1 IndexInfo: $2 IndexMap $3", (IsIndex() ? "Index Table" : "Normal Table"), id(),
      yb::ToString(index_info()), yb::ToString(index_map()));
}

const PartitionSchema& YBTable::partition_schema() const {
  return info_.partition_schema;
}

bool YBTable::IsHashPartitioned() const {
  // TODO(neil) After fixing github #5832, "partition_schema" must be used here.
  // return info_.partition_schema.IsHashPartitioning();
  return info_.schema.num_hash_key_columns() > 0;
}

bool YBTable::IsRangePartitioned() const {
  // TODO(neil) After fixing github #5832, "partition_schema" must be used here.
  // return info_.partition_schema.IsRangePartitioning();
  return info_.schema.num_hash_key_columns() == 0;
}

std::shared_ptr<const TablePartitions> YBTable::GetPartitionsShared() const {
  SharedLock<decltype(mutex_)> lock(mutex_);
  return std::shared_ptr<const TablePartitions>(partitions_, &partitions_->keys);
}

std::shared_ptr<const VersionedTablePartitions> YBTable::GetVersionedPartitions() const {
  SharedLock<decltype(mutex_)> lock(mutex_);
  return partitions_;
}

TablePartitions YBTable::GetPartitionsCopy() const {
  TablePartitions result;

  SharedLock<decltype(mutex_)> lock(mutex_);
  result.reserve(partitions_->keys.size());
  for (const auto& key : partitions_->keys) {
    result.push_back(key);
  }
  return result;
}

int32_t YBTable::GetPartitionCount() const {
  SharedLock<decltype(mutex_)> lock(mutex_);
  return partitions_->keys.size();
}

int32_t YBTable::GetPartitionsVersion() const {
  SharedLock<decltype(mutex_)> lock(mutex_);
  return partitions_->version;
}

//--------------------------------------------------------------------------------------------------

std::unique_ptr<YBqlWriteOp> YBTable::NewQLWrite() {
  return std::unique_ptr<YBqlWriteOp>(new YBqlWriteOp(shared_from_this()));
}

std::unique_ptr<YBqlWriteOp> YBTable::NewQLInsert() {
  return YBqlWriteOp::NewInsert(shared_from_this());
}

std::unique_ptr<YBqlWriteOp> YBTable::NewQLUpdate() {
  return YBqlWriteOp::NewUpdate(shared_from_this());
}

std::unique_ptr<YBqlWriteOp> YBTable::NewQLDelete() {
  return YBqlWriteOp::NewDelete(shared_from_this());
}

std::unique_ptr<YBqlReadOp> YBTable::NewQLSelect() {
  return YBqlReadOp::NewSelect(shared_from_this());
}

std::unique_ptr<YBqlReadOp> YBTable::NewQLRead() {
  return std::unique_ptr<YBqlReadOp>(new YBqlReadOp(shared_from_this()));
}

size_t YBTable::FindPartitionStartIndex(const std::string& partition_key, size_t group_by) const {
  SharedLock<decltype(mutex_)> lock(mutex_);
  return client::FindPartitionStartIndex(partitions_->keys, partition_key, group_by);
}

std::shared_ptr<const std::string> YBTable::FindPartitionStart(
    const std::string& partition_key, size_t group_by) const {
  SharedLock<decltype(mutex_)> lock(mutex_);
  size_t idx = FindPartitionStartIndex(partition_key, group_by);
  return std::shared_ptr<const std::string>(partitions_, &partitions_->keys[idx]);
}

Result<bool> YBTable::MaybeRefreshPartitions() {
  if (!partitions_are_stale_) {
    return false;
  }
  std::unique_lock<decltype(partitions_refresh_mutex_)> refresh_lock(partitions_refresh_mutex_);
  if (!partitions_are_stale_) {
    // Has been refreshed by concurrent thread.
    return true;
  }
  const auto partitions_result = FetchPartitions();
  if (!partitions_result.ok()) {
    return partitions_result.status();
  }
  const auto& partitions = *partitions_result;
  {
    std::lock_guard<rw_spinlock> partitions_lock(mutex_);
    if (partitions->version < partitions_->version) {
      return STATUS_FORMAT(
          TryAgain, "Received table $0 partitions version: $1, ours is: $2", id(),
          partitions->version, partitions_->version);
    }
    partitions_ = partitions;
  }
  partitions_are_stale_ = false;
  return true;
}

void YBTable::MarkPartitionsAsStale() {
  partitions_are_stale_ = true;
}

bool YBTable::ArePartitionsStale() const {
  return partitions_are_stale_;
}

Result<std::shared_ptr<const VersionedTablePartitions>> YBTable::FetchPartitions() {
  // TODO: fetch the schema from the master here once catalog is available.
  auto partitions = std::make_shared<VersionedTablePartitions>();

  master::GetTableLocationsRequestPB req;
  req.set_max_returned_locations(std::numeric_limits<int32_t>::max());
  master::GetTableLocationsResponsePB resp;

  auto deadline = CoarseMonoClock::Now() + client_->default_admin_operation_timeout();

  req.mutable_table()->set_table_id(info_.table_id);
  // TODO(tsplit): consider optimizing this to not wait for all tablets to be running in case
  // of some tablet has been split and post-split tablets are not yet running.
  req.set_require_tablets_running(true);
  Status s;

  CoarseBackoffWaiter waiter(deadline, std::chrono::seconds(1) /* max_wait */);
  // TODO: replace this with Async RPC-retrier based RPC in the next revision,
  // adding exponential backoff and allowing this to be used safely in a
  // a reactor thread.
  while (true) {
    rpc::RpcController rpc;

    // Have we already exceeded our deadline?
    auto now = CoarseMonoClock::Now();

    // See YBClient::Data::SyncLeaderMasterRpc().
    auto rpc_deadline = now + client_->default_rpc_timeout();
    rpc.set_deadline(std::min(rpc_deadline, deadline));

    s = client_->data_->master_proxy()->GetTableLocations(req, &resp, &rpc);
    if (!s.ok()) {
      // Various conditions cause us to look for the leader master again.
      // It's ok if that eventually fails; we'll retry over and over until
      // the deadline is reached.

      if (s.IsNetworkError()) {
        LOG(WARNING) << "Network error talking to the leader master ("
                     << client_->data_->leader_master_hostport().ToString() << "): "
                     << s.ToString();
        if (client_->IsMultiMaster()) {
          LOG(INFO) << "Determining the leader master again and retrying.";
          WARN_NOT_OK(client_->data_->SetMasterServerProxy(deadline),
                      "Failed to determine new Master");
          continue;
        }
      }

      if (s.IsTimedOut() && CoarseMonoClock::Now() < deadline) {
        // If the RPC timed out and the operation deadline expired, we'll loop
        // again and time out for good above.
        LOG(WARNING) << "Timed out talking to the leader master ("
                     << client_->data_->leader_master_hostport().ToString() << "): "
                     << s.ToString();
        if (client_->IsMultiMaster()) {
          LOG(INFO) << "Determining the leader master again and retrying.";
          WARN_NOT_OK(client_->data_->SetMasterServerProxy(deadline),
                      "Failed to determine new Master");
          continue;
        }
      }
    }
    if (s.ok() && resp.has_error()) {
      if (resp.error().code() == master::MasterErrorPB::NOT_THE_LEADER ||
          resp.error().code() == master::MasterErrorPB::CATALOG_MANAGER_NOT_INITIALIZED) {
        LOG(WARNING) << "Master " << client_->data_->leader_master_hostport().ToString()
                     << " is no longer the leader master.";
        if (client_->IsMultiMaster()) {
          LOG(INFO) << "Determining the leader master again and retrying.";
          WARN_NOT_OK(client_->data_->SetMasterServerProxy(deadline),
                      "Failed to determine new Master");
          continue;
        }
      }
      if (s.ok()) {
        s = StatusFromPB(resp.error().status());
      }
    }
    if (!s.ok()) {
      YB_LOG_EVERY_N_SECS(WARNING, 10) << "Error getting table locations: " << s << ", retrying.";
    } else if (resp.tablet_locations_size() > 0) {
      partitions->version = resp.partitions_version();
      partitions->keys.reserve(resp.tablet_locations().size());
      for (const auto& tablet_location : resp.tablet_locations()) {
        partitions->keys.push_back(tablet_location.partition().partition_key_start());
      }
      std::sort(partitions->keys.begin(), partitions->keys.end());
      break;
    }

    if (!waiter.Wait()) {
      const char* msg = "OpenTable timed out";
      LOG(ERROR) << msg;
      return STATUS(TimedOut, msg);
    }
  }


  RETURN_NOT_OK_PREPEND(PBToClientTableType(resp.table_type(), &table_type_),
    strings::Substitute("Invalid table type for table '$0'", info_.table_name.ToString()));

  VLOG(2) << "Fetched partitions for table " << info_.table_name.ToString() << ", found "
          << resp.tablet_locations_size() << " tablets";
  return partitions;
}

Status YBTable::Open() {
  std::lock_guard<rw_spinlock> partitions_lock(mutex_);
  partitions_ = VERIFY_RESULT(FetchPartitions());
  partitions_are_stale_ = false;
  return Status::OK();
}

//--------------------------------------------------------------------------------------------------

std::unique_ptr<YBPgsqlWriteOp> YBTable::NewPgsqlWrite() {
  return std::unique_ptr<YBPgsqlWriteOp>(new YBPgsqlWriteOp(shared_from_this()));
}

std::unique_ptr<YBPgsqlWriteOp> YBTable::NewPgsqlInsert() {
  return YBPgsqlWriteOp::NewInsert(shared_from_this());
}

std::unique_ptr<YBPgsqlWriteOp> YBTable::NewPgsqlUpdate() {
  return YBPgsqlWriteOp::NewUpdate(shared_from_this());
}

std::unique_ptr<YBPgsqlWriteOp> YBTable::NewPgsqlDelete() {
  return YBPgsqlWriteOp::NewDelete(shared_from_this());
}

std::unique_ptr<YBPgsqlWriteOp> YBTable::NewPgsqlTruncateColocated() {
  return YBPgsqlWriteOp::NewTruncateColocated(shared_from_this());
}

std::unique_ptr<YBPgsqlReadOp> YBTable::NewPgsqlSelect() {
  return YBPgsqlReadOp::NewSelect(shared_from_this());
}

std::unique_ptr<YBPgsqlReadOp> YBTable::NewPgsqlRead() {
  return std::unique_ptr<YBPgsqlReadOp>(new YBPgsqlReadOp(shared_from_this()));
}

size_t FindPartitionStartIndex(const std::vector<std::string>& partitions,
                               const std::string& partition_key,
                               size_t group_by) {
  auto it = std::lower_bound(partitions.begin(), partitions.end(), partition_key);
  if (it == partitions.end() || *it > partition_key) {
    DCHECK(it != partitions.begin());
    --it;
  }
  return group_by <= 1 ? it - partitions.begin() :
                         (it - partitions.begin()) / group_by * group_by;
}

} // namespace client
} // namespace yb
