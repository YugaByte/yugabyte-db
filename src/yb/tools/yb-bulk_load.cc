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

#include <sched.h>
#include <iostream>
#include <thread>
#include <boost/algorithm/string.hpp>

#include <gflags/gflags.h>
#include <glog/logging.h>

#include "yb/client/client.h"
#include "yb/client/table.h"
#include "yb/common/entity_ids.h"
#include "yb/common/hybrid_time.h"
#include "yb/common/jsonb.h"
#include "yb/common/ql_protocol.pb.h"
#include "yb/common/ql_value.h"
#include "yb/common/schema.h"
#include "yb/common/wire_protocol.h"
#include "yb/docdb/cql_operation.h"
#include "yb/docdb/doc_operation.h"
#include "yb/docdb/docdb.h"
#include "yb/master/master_util.h"
#include "yb/rocksdb/db.h"
#include "yb/rocksdb/options.h"
#include "yb/rpc/messenger.h"
#include "yb/rpc/rpc_controller.h"
#include "yb/tools/bulk_load_docdb_util.h"
#include "yb/tools/bulk_load_utils.h"
#include "yb/tools/yb-generate_partitions.h"
#include "yb/tserver/tserver_service.proxy.h"
#include "yb/util/env.h"
#include "yb/util/status.h"
#include "yb/util/stol_utils.h"
#include "yb/util/stopwatch.h"
#include "yb/util/size_literals.h"
#include "yb/util/threadpool.h"
#include "yb/util/flags.h"
#include "yb/util/logging.h"
#include "yb/util/path_util.h"
#include "yb/util/subprocess.h"

using std::pair;
using std::string;
using std::shared_ptr;
using std::unique_ptr;
using std::vector;
using yb::client::YBClient;
using yb::client::YBClientBuilder;
using yb::client::YBTable;
using yb::client::YBTableName;
using yb::docdb::DocWriteBatch;
using yb::docdb::InitMarkerBehavior;
using yb::operator"" _GB;

DEFINE_string(master_addresses, "", "Comma-separated list of YB Master server addresses");
DEFINE_string(table_name, "", "Name of the table to generate partitions for");
DEFINE_string(namespace_name, "", "Namespace of the table");
DEFINE_string(base_dir, "", "Base directory where we will store all the SSTable files");
DEFINE_int64(memtable_size_bytes, 1_GB, "Amount of bytes to use for the rocksdb memtable");
DEFINE_int32(row_batch_size, 1000, "The number of rows to batch together in each rocksdb write");
DEFINE_bool(flush_batch_for_tests, false, "Option used only in tests to flush after each batch. "
    "Used to generate multiple SST files in conjuction with small row_batch_size");
DEFINE_string(bulk_load_helper_script, "./bulk_load_helper.sh", "Relative path for bulk load helper"
              " script");
DEFINE_string(bulk_load_cleanup_script, "./bulk_load_cleanup.sh", "Relative path for bulk load "
              "cleanup script");
DEFINE_string(ssh_key_file, "", "SSH key to push SSTable files to production cluster");
DEFINE_bool(export_files, false, "Whether or not the files should be exported to a production "
            "cluster.");
DEFINE_int32(bulk_load_num_threads, 16, "Number of threads to use for bulk load");
DEFINE_int32(bulk_load_threadpool_queue_size, 10000,
             "Maximum number of entries to queue in the threadpool");
DEFINE_int32(bulk_load_num_memtables, 3, "Number of memtables to use for rocksdb");
DEFINE_int32(bulk_load_max_background_flushes, 2, "Number of flushes to perform in the background");
DEFINE_uint64(bulk_load_num_files_per_tablet, 5,
              "Determines how to compact the data of a tablet to ensure we have only a certain "
              "number of sst files per tablet");

DECLARE_string(skipped_cols);

namespace yb {
namespace tools {

namespace {

class BulkLoadTask : public Runnable {
 public:
  BulkLoadTask(vector<pair<TabletId, string>> rows, BulkLoadDocDBUtil *db_fixture,
               const YBTable *table, YBPartitionGenerator *partition_generator);
  void Run();
 private:
  CHECKED_STATUS PopulateColumnValue(const string &column,
                                     const DataType data_type,
                                     QLExpressionPB *column_value);
  CHECKED_STATUS InsertRow(const string &row,
                           const Schema &schema,
                           const IndexMap& index_map,
                           BulkLoadDocDBUtil *const db_fixture,
                           docdb::DocWriteBatch *const doc_write_batch,
                           YBPartitionGenerator *const partition_generator);
  vector<pair<TabletId, string>> rows_;
  const std::set<int> skipped_cols_;
  BulkLoadDocDBUtil *const db_fixture_;
  const YBTable *const table_;
  YBPartitionGenerator *const partition_generator_;
};

class CompactionTask: public Runnable {
 public:
  CompactionTask(const vector<string>& sst_filenames, BulkLoadDocDBUtil* db_fixture);
  void Run();
 private:
  vector <string> sst_filenames_;
  BulkLoadDocDBUtil *const db_fixture_;
};

class BulkLoad {
 public:
  CHECKED_STATUS RunBulkLoad();

 private:
  CHECKED_STATUS InitYBBulkLoad();
  CHECKED_STATUS InitDBUtil(const TabletId &tablet_id);
  CHECKED_STATUS FinishTabletProcessing(const TabletId &tablet_id,
                                        vector<pair<TabletId, string>> rows);
  CHECKED_STATUS RetryableSubmit(vector<pair<TabletId, string>> rows);
  CHECKED_STATUS CompactFiles();

  std::unique_ptr<YBClient> client_;
  shared_ptr<YBTable> table_;
  unique_ptr<YBPartitionGenerator> partition_generator_;
  gscoped_ptr<ThreadPool> thread_pool_;
  unique_ptr<BulkLoadDocDBUtil> db_fixture_;
};

CompactionTask::CompactionTask(const vector<string>& sst_filenames, BulkLoadDocDBUtil* db_fixture)
    : sst_filenames_(sst_filenames),
      db_fixture_(db_fixture) {
}

void CompactionTask::Run() {
  if (sst_filenames_.size() == 1) {
    LOG(INFO) << "Skipping compaction since we have only a single file: " << sst_filenames_[0];
    return;
  }

  LOG(INFO) << "Compacting files: " << ToString(sst_filenames_);
  CHECK_OK(db_fixture_->rocksdb()->CompactFiles(rocksdb::CompactionOptions(),
                                                sst_filenames_,
                                                /* output_level */ 0));
}

BulkLoadTask::BulkLoadTask(vector<pair<TabletId, string>> rows,
                           BulkLoadDocDBUtil *db_fixture, const YBTable *table,
                           YBPartitionGenerator *partition_generator)
    : rows_(std::move(rows)),
      skipped_cols_(tools::SkippedColumns()),
      db_fixture_(db_fixture),
      table_(table),
      partition_generator_(partition_generator) {
}

void BulkLoadTask::Run() {
  DocWriteBatch doc_write_batch(docdb::DocDB::FromRegularUnbounded(db_fixture_->rocksdb()),
                                InitMarkerBehavior::kOptional);

  for (const auto &entry : rows_) {
    const string &row = entry.second;

    // Populate the row.
    CHECK_OK(InsertRow(row, table_->InternalSchema(), table_->index_map(), db_fixture_,
                       &doc_write_batch, partition_generator_));
  }

  // Flush the batch.
  CHECK_OK(db_fixture_->WriteToRocksDB(
      doc_write_batch, HybridTime::FromMicros(kYugaByteMicrosecondEpoch),
      /* decode_dockey */ false, /* increment_write_id */ false));

  if (FLAGS_flush_batch_for_tests) {
    CHECK_OK(db_fixture_->FlushRocksDbAndWait());
  }
}

Status BulkLoadTask::PopulateColumnValue(const string &column,
                                         const DataType data_type,
                                         QLExpressionPB *column_value) {
  auto ql_valuepb = column_value->mutable_value();
  switch (data_type) {
    YB_SET_INT_VALUE(ql_valuepb, column, 8);
    YB_SET_INT_VALUE(ql_valuepb, column, 16);
    YB_SET_INT_VALUE(ql_valuepb, column, 32);
    YB_SET_INT_VALUE(ql_valuepb, column, 64);
    case DataType::FLOAT: {
      auto value = CheckedStold(column);
      RETURN_NOT_OK(value);
      ql_valuepb->set_float_value(*value);
      break;
    }
    case DataType::DOUBLE: {
      auto value = CheckedStold(column);
      RETURN_NOT_OK(value);
      ql_valuepb->set_double_value(*value);
      break;
    }
    case DataType::STRING: {
      ql_valuepb->set_string_value(column);
      break;
    }
    case DataType::JSONB: {
      common::Jsonb jsonb;
      RETURN_NOT_OK(jsonb.FromString(column));
      ql_valuepb->set_jsonb_value(jsonb.MoveSerializedJsonb());
      break;
    }
    case DataType::TIMESTAMP: {
      auto ts = TimestampFromString(column);
      RETURN_NOT_OK(ts);
      ql_valuepb->set_timestamp_value(ts->ToInt64());
      break;
    }
    case DataType::BINARY: {
      ql_valuepb->set_binary_value(column);
      break;
    }
    default:
      FATAL_INVALID_ENUM_VALUE(DataType, data_type);
  }
  return Status::OK();
}

Status BulkLoadTask::InsertRow(const string &row,
                               const Schema &schema,
                               const IndexMap& index_map,
                               BulkLoadDocDBUtil *const db_fixture,
                               docdb::DocWriteBatch *const doc_write_batch,
                               YBPartitionGenerator *const partition_generator) {
  // Get individual columns.
  CsvTokenizer tokenizer = Tokenize(row);
  size_t ncolumns = std::distance(tokenizer.begin(), tokenizer.end());
  if (ncolumns != schema.num_columns()) {
    return STATUS_SUBSTITUTE(IllegalState, "row '$0' has $1 columns, need exactly $2", row,
                             ncolumns, schema.num_columns());
  }

  QLResponsePB resp;
  QLWriteRequestPB req;
  req.set_type(QLWriteRequestPB_QLStmtType_QL_STMT_INSERT);
  req.set_client(YQL_CLIENT_CQL);

  int col_id = 0;
  auto it = tokenizer.begin();
  // Process the hash keys first.
  for (int i = 0; i < schema.num_key_columns(); it++, col_id++) {
    if (skipped_cols_.find(col_id) != skipped_cols_.end()) {
      continue;
    }
    if (IsNull(*it)) {
      return STATUS_SUBSTITUTE(IllegalState, "Primary key cannot be null: $0", *it);
    }

    QLExpressionPB *column_value = nullptr;
    if (schema.is_hash_key_column(i)) {
      column_value = req.add_hashed_column_values();
    } else {
      column_value = req.add_range_column_values();
    }

    RETURN_NOT_OK(PopulateColumnValue(*it, schema.column(i).type_info()->type(), column_value));
    i++;  // Avoid this if we are skipping the column.
  }

  // Finally process the regular columns.
  for (int i = schema.num_key_columns(); i < schema.num_columns(); it++, col_id++) {
    if (skipped_cols_.find(col_id) != skipped_cols_.end()) {
      continue;
    }
    QLColumnValuePB *column_value = req.add_column_values();
    column_value->set_column_id(kFirstColumnId + i);
    if (IsNull(*it)) {
      // Use empty value for null.
      column_value->mutable_expr()->mutable_value();
    } else {
      RETURN_NOT_OK(PopulateColumnValue(*it, schema.column(i).type_info()->type(),
                                        column_value->mutable_expr()));
    }
    i++;  // Avoid this if we are skipping the column.
  }

  // Add the hash code to the operation.
  string tablet_id;
  string partition_key;
  RETURN_NOT_OK(partition_generator->LookupTabletIdWithTokenizer(
      tokenizer, skipped_cols_, &tablet_id, &partition_key));
  req.set_hash_code(PartitionSchema::DecodeMultiColumnHashValue(partition_key));

  // Finally apply the operation to the doc_write_batch.
  // TODO(dtxn) pass correct TransactionContext.
  // Comment from PritamD: Don't need cross shard transaction support in bulk load, but I guess
  // once we have secondary indexes we probably might need to ensure bulk load builds the indexes
  // as well.
  docdb::QLWriteOperation op(std::shared_ptr<const Schema>(&schema, [](const Schema*){}),
                             index_map, nullptr /* unique_index_key_schema */, boost::none);
  RETURN_NOT_OK(op.Init(&req, &resp));
  RETURN_NOT_OK(op.Apply({
      doc_write_batch,
      CoarseTimePoint::max() /* deadline */,
      ReadHybridTime::SingleTime(HybridTime::FromMicros(kYugaByteMicrosecondEpoch))}));
  return Status::OK();
}


Status BulkLoad::RetryableSubmit(vector<pair<TabletId, string>> rows) {
  auto runnable = std::make_shared<BulkLoadTask>(
      std::move(rows), db_fixture_.get(), table_.get(), partition_generator_.get());

  Status s;
  do {
    s = thread_pool_->Submit(runnable);

    if (!s.IsServiceUnavailable()) {
      return s;
    }

    LOG (ERROR) << "Failed submitting task, sleeping for a while: " << s.ToString();

    // If service is unavailable, the queue might be full. Sleep and try again.
    SleepFor(MonoDelta::FromSeconds(10));
  } while (!s.ok());

  return Status::OK();
}

Status BulkLoad::CompactFiles() {
  std::vector<rocksdb::LiveFileMetaData> live_files_metadata;
  db_fixture_->rocksdb()->GetLiveFilesMetaData(&live_files_metadata);
  if (live_files_metadata.empty()) {
    return STATUS(IllegalState, "Need atleast one sst file");
  }

  // Extract file names.
  vector<string> sst_files;
  sst_files.reserve(live_files_metadata.size());
  for (const rocksdb::LiveFileMetaData& file : live_files_metadata) {
    sst_files.push_back(file.name);
  }

  // Batch the files for compaction.
  size_t batch_size = sst_files.size() / FLAGS_bulk_load_num_files_per_tablet;
  // We need to perform compactions only if we have more than 'bulk_load_num_files_per_tablet'
  // files.
  if (batch_size != 0) {
    auto start_iter = sst_files.begin();
    for (size_t i = 0; i < FLAGS_bulk_load_num_files_per_tablet; i++) {
      // Sanity check.
      CHECK_GE(std::distance(start_iter, sst_files.end()), batch_size);

      // Include the remaining files for the last batch.
      auto end_iter = (i == FLAGS_bulk_load_num_files_per_tablet - 1) ? sst_files.end()
                                                                      : start_iter + batch_size;
      auto runnable = std::make_shared<CompactionTask>(vector<string>(start_iter, end_iter),
                                                       db_fixture_.get());
      RETURN_NOT_OK(thread_pool_->Submit(runnable));
      start_iter = end_iter;
    }

    // Finally wait for all compactions to finish.
    thread_pool_->Wait();

    // Reopen rocksdb to clean up deleted files.
    return db_fixture_->ReopenRocksDB();
  }
  return Status::OK();
}

Status BulkLoad::FinishTabletProcessing(const TabletId &tablet_id,
                                        vector<pair<TabletId, string>> rows) {
  if (!db_fixture_) {
    // Skip processing since db_fixture wasn't initialized indicating empty input.
    return Status::OK();
  }

  // Submit all the work.
  RETURN_NOT_OK(RetryableSubmit(std::move(rows)));

  // Wait for all tasks for the tablet to complete.
  thread_pool_->Wait();

  // Now flush the DB.
  RETURN_NOT_OK(db_fixture_->FlushRocksDbAndWait());

  // Perform the necessary compactions.
  RETURN_NOT_OK(CompactFiles());

  if (!FLAGS_export_files) {
    return Status::OK();
  }

  // Find replicas for the tablet.
  master::TabletLocationsPB tablet_locations;
  RETURN_NOT_OK(client_->GetTabletLocation(tablet_id, &tablet_locations));
  string csv_replicas;
  std::map<string, int32_t> host_to_rpcport;
  for (const master::TabletLocationsPB_ReplicaPB &replica : tablet_locations.replicas()) {
    if (!csv_replicas.empty()) {
      csv_replicas += ",";
    }
    const string &host = replica.ts_info().private_rpc_addresses(0).host();
    csv_replicas += host;
    host_to_rpcport[host] = replica.ts_info().private_rpc_addresses(0).port();
  }

  // Invoke the bulk_load_helper script.
  vector<string> argv = {FLAGS_bulk_load_helper_script, "-t", tablet_id, "-r", csv_replicas, "-i",
      FLAGS_ssh_key_file, "-d", db_fixture_->rocksdb_dir()};
  string bulk_load_helper_stdout;
  RETURN_NOT_OK(Subprocess::Call(argv, &bulk_load_helper_stdout));

  // Trim the output.
  boost::trim(bulk_load_helper_stdout);
  LOG(INFO) << "Helper script stdout: " << bulk_load_helper_stdout;

  // Finalize the import.
  rpc::MessengerBuilder bld("Client");
  std::unique_ptr<rpc::Messenger> client_messenger = VERIFY_RESULT(bld.Build());
  rpc::ProxyCache proxy_cache(client_messenger.get());
  vector<string> lines;
  boost::split(lines, bulk_load_helper_stdout, boost::is_any_of("\n"));
  for (const string &line : lines) {
    vector<string> tokens;
    boost::split(tokens, line, boost::is_any_of(","));
    if (tokens.size() != 2) {
      return STATUS_SUBSTITUTE(InvalidArgument, "Invalid line $0", line);
    }
    const string &replica_host = tokens[0];
    const string &directory = tokens[1];
    HostPort hostport(replica_host, host_to_rpcport[replica_host]);

    tserver::TabletServerServiceProxy proxy(&proxy_cache, hostport);
    tserver::ImportDataRequestPB req;
    req.set_tablet_id(tablet_id);
    req.set_source_dir(directory);

    tserver::ImportDataResponsePB resp;
    rpc::RpcController controller;
    LOG(INFO) << "Importing " << directory << " on " << replica_host << " for tablet_id: "
              << tablet_id;
    RETURN_NOT_OK(proxy.ImportData(req, &resp, &controller));
    if (resp.has_error()) {
      RETURN_NOT_OK(StatusFromPB(resp.error().status()));
    }

    // Now cleanup the files from the production tserver.
    vector<string> cleanup_script = {FLAGS_bulk_load_cleanup_script, "-d", directory, "-t",
        replica_host, "-i", FLAGS_ssh_key_file};
    RETURN_NOT_OK(Subprocess::Call(cleanup_script));
  }

  // Delete the data once the import is done.
  return yb::Env::Default()->DeleteRecursively(db_fixture_->rocksdb_dir());
}


CHECKED_STATUS BulkLoad::InitDBUtil(const TabletId &tablet_id) {
  db_fixture_.reset(new BulkLoadDocDBUtil(tablet_id, FLAGS_base_dir,
                                          FLAGS_memtable_size_bytes,
                                          FLAGS_bulk_load_num_memtables,
                                          FLAGS_bulk_load_max_background_flushes));
  RETURN_NOT_OK(db_fixture_->InitRocksDBOptions());
  RETURN_NOT_OK(db_fixture_->DisableCompactions()); // This opens rocksdb.
  return Status::OK();
}

Status BulkLoad::InitYBBulkLoad() {
  // Convert table_name to lowercase since we store table names in lowercase.
  string table_name_lower = boost::to_lower_copy(FLAGS_table_name);
  YBTableName table_name(
      master::GetDefaultDatabaseType(FLAGS_namespace_name), FLAGS_namespace_name, table_name_lower);

  YBClientBuilder builder;
  builder.add_master_server_addr(FLAGS_master_addresses);

  client_ = VERIFY_RESULT(builder.Build());
  RETURN_NOT_OK(client_->OpenTable(table_name, &table_));
  partition_generator_.reset(new YBPartitionGenerator(table_name, {FLAGS_master_addresses}));
  RETURN_NOT_OK(partition_generator_->Init());

  db_fixture_ = nullptr;
  CHECK_OK(
      ThreadPoolBuilder("bulk_load_tasks")
          .set_min_threads(FLAGS_bulk_load_num_threads)
          .set_max_threads(FLAGS_bulk_load_num_threads)
          .set_max_queue_size(FLAGS_bulk_load_threadpool_queue_size)
          .set_idle_timeout(MonoDelta::FromMilliseconds(5000))
          .Build(&thread_pool_));
  return Status::OK();
}


Status BulkLoad::RunBulkLoad() {

  RETURN_NOT_OK(InitYBBulkLoad());

  TabletId current_tablet_id;

  vector<pair<TabletId, string>> rows;
  for (string line; std::getline(std::cin, line);) {
    // Trim the line.
    boost::algorithm::trim(line);

    // Get the key and value.
    std::size_t index = line.find("\t");
    if (index == std::string::npos) {
      return STATUS_SUBSTITUTE(IllegalState, "Invalid line: $0", line);
    }
    const TabletId tablet_id = line.substr(0, index);
    const string row = line.substr(index + 1, line.size() - (index + 1));

    // Reinitialize rocksdb if needed.
    if (current_tablet_id.empty() || current_tablet_id != tablet_id) {
      // Flush all of the data before opening a new rocksdb.
      RETURN_NOT_OK(FinishTabletProcessing(current_tablet_id, std::move(rows)));
      RETURN_NOT_OK(InitDBUtil(tablet_id));
    }
    current_tablet_id = tablet_id;
    rows.emplace_back(std::move(tablet_id), std::move(row));

    // Flush the batch if necessary.
    if (rows.size() >= FLAGS_row_batch_size) {
      RETURN_NOT_OK(RetryableSubmit(std::move(rows)));
    }
  }

  // Process last tablet.
  RETURN_NOT_OK(FinishTabletProcessing(current_tablet_id, std::move(rows)));
  return Status::OK();
}

} // anonymous namespace

} // namespace tools
} // namespace yb

int main(int argc, char** argv) {
  yb::ParseCommandLineFlags(&argc, &argv, true);
  yb::InitGoogleLoggingSafe(argv[0]);
  if (FLAGS_master_addresses.empty() || FLAGS_table_name.empty() || FLAGS_namespace_name.empty()
      || FLAGS_base_dir.empty()) {
    LOG(FATAL) << "Need to specify --master_addresses, --table_name, --namespace_name, "
        "--base_dir";
  }

  if (FLAGS_export_files && FLAGS_ssh_key_file.empty()) {
    LOG(FATAL) << "Need to specify --ssh_key_file with --export_files";
  }

  // Verify the bulk load path exists.
  if (!yb::Env::Default()->FileExists(FLAGS_base_dir)) {
    LOG(FATAL) << "Bulk load directory doesn't exist: " << FLAGS_base_dir;
  }

  if (FLAGS_bulk_load_num_files_per_tablet <= 0) {
    LOG(FATAL) << "--bulk_load_num_files_per_tablet needs to be greater than 0";
  }

  yb::tools::BulkLoad bulk_load;
  yb::Status s = bulk_load.RunBulkLoad();
  if (!s.ok()) {
    LOG(FATAL) << "Error running bulk load: " << s.ToString();
  }
  return 0;
}
