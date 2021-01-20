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

#include "yb/client/table_creator.h"

#include "yb/client/client.h"
#include "yb/client/client-internal.h"

#include "yb/common/wire_protocol.h"
#include "yb/common/common_flags.h"

#include "yb/util/flag_tags.h"

#include "yb/yql/redis/redisserver/redis_constants.h"

DECLARE_bool(client_suppress_created_logs);

namespace yb {
namespace client {

YBTableCreator::YBTableCreator(YBClient* client)
  : client_(client) {
}

YBTableCreator::~YBTableCreator() {
}

YBTableCreator& YBTableCreator::table_name(const YBTableName& name) {
  table_name_ = name;
  return *this;
}

YBTableCreator& YBTableCreator::table_type(YBTableType table_type) {
  table_type_ = YBTable::ClientToPBTableType(table_type);
  return *this;
}

YBTableCreator& YBTableCreator::creator_role_name(const RoleName& creator_role_name) {
  creator_role_name_ = creator_role_name;
  return *this;
}

YBTableCreator& YBTableCreator::table_id(const std::string& table_id) {
  table_id_ = table_id;
  return *this;
}

YBTableCreator& YBTableCreator::is_pg_catalog_table() {
  is_pg_catalog_table_ = true;
  return *this;
}

YBTableCreator& YBTableCreator::is_pg_shared_table() {
  is_pg_shared_table_ = true;
  return *this;
}

YBTableCreator& YBTableCreator::hash_schema(YBHashSchema hash_schema) {
  switch (hash_schema) {
    case YBHashSchema::kMultiColumnHash:
      partition_schema_.set_hash_schema(PartitionSchemaPB::MULTI_COLUMN_HASH_SCHEMA);
      break;
    case YBHashSchema::kRedisHash:
      partition_schema_.set_hash_schema(PartitionSchemaPB::REDIS_HASH_SCHEMA);
      break;
    case YBHashSchema::kPgsqlHash:
      partition_schema_.set_hash_schema(PartitionSchemaPB::PGSQL_HASH_SCHEMA);
      break;
  }
  return *this;
}

YBTableCreator& YBTableCreator::num_tablets(int32_t count) {
  num_tablets_ = count;
  return *this;
}

YBTableCreator& YBTableCreator::colocated(const bool colocated) {
  colocated_ = colocated;
  return *this;
}

YBTableCreator& YBTableCreator::tablegroup_id(const std::string& tablegroup_id) {
  tablegroup_id_ = tablegroup_id;
  return *this;
}

YBTableCreator& YBTableCreator::schema(const YBSchema* schema) {
  schema_ = schema;
  return *this;
}

YBTableCreator& YBTableCreator::part_of_transaction(const TransactionMetadata* txn) {
  txn_ = txn;
  return *this;
}

YBTableCreator& YBTableCreator::add_hash_partitions(const std::vector<std::string>& columns,
                                                        int32_t num_buckets) {
  return add_hash_partitions(columns, num_buckets, 0);
}

YBTableCreator& YBTableCreator::add_hash_partitions(const std::vector<std::string>& columns,
                                                        int32_t num_buckets, int32_t seed) {
  PartitionSchemaPB::HashBucketSchemaPB* bucket_schema =
    partition_schema_.add_hash_bucket_schemas();
  for (const string& col_name : columns) {
    bucket_schema->add_columns()->set_name(col_name);
  }
  bucket_schema->set_num_buckets(num_buckets);
  bucket_schema->set_seed(seed);
  return *this;
}

YBTableCreator& YBTableCreator::set_range_partition_columns(
    const std::vector<std::string>& columns,
    const std::vector<std::string>& split_rows) {
  PartitionSchemaPB::RangeSchemaPB* range_schema =
    partition_schema_.mutable_range_schema();
  range_schema->Clear();
  for (const string& col_name : columns) {
    range_schema->add_columns()->set_name(col_name);
  }

  for (const auto& row : split_rows) {
    range_schema->add_splits()->set_column_bounds(row);
  }
  return *this;
}

YBTableCreator& YBTableCreator::replication_info(const master::ReplicationInfoPB& ri) {
  replication_info_ = ri;
  has_replication_info_ = true;
  return *this;
}

YBTableCreator& YBTableCreator::indexed_table_id(const std::string& id) {
  index_info_.set_indexed_table_id(id);
  return *this;
}

YBTableCreator& YBTableCreator::is_local_index(bool is_local_index) {
  index_info_.set_is_local(is_local_index);
  return *this;
}

YBTableCreator& YBTableCreator::is_unique_index(bool is_unique_index) {
  index_info_.set_is_unique(is_unique_index);
  return *this;
}

YBTableCreator& YBTableCreator::skip_index_backfill(const bool skip_index_backfill) {
  skip_index_backfill_ = skip_index_backfill;
  return *this;
}

YBTableCreator& YBTableCreator::use_mangled_column_name(bool value) {
  index_info_.set_use_mangled_column_name(value);
  return *this;
}

YBTableCreator& YBTableCreator::timeout(const MonoDelta& timeout) {
  timeout_ = timeout;
  return *this;
}

YBTableCreator& YBTableCreator::wait(bool wait) {
  wait_ = wait;
  return *this;
}

YBTableCreator& YBTableCreator::TEST_use_old_style_create_request() {
  TEST_use_old_style_create_request_ = true;
  return *this;
}

Status YBTableCreator::Create() {
  const char *object_type = index_info_.has_indexed_table_id() ? "index" : "table";
  if (table_name_.table_name().empty()) {
    return STATUS_SUBSTITUTE(InvalidArgument, "Missing $0 name", object_type);
  }
  // For a redis table, no external schema is passed to TableCreator, we make a unique schema
  // and manage its memory withing here.
  std::unique_ptr<YBSchema> redis_schema;
  // We create dummy schema for transaction status table, redis schema is quite lightweight for
  // this purpose.
  if (table_type_ == TableType::REDIS_TABLE_TYPE ||
      table_type_ == TableType::TRANSACTION_STATUS_TABLE_TYPE) {
    CHECK(!schema_) << "Schema should not be set for redis table creation";
    redis_schema.reset(new YBSchema());
    YBSchemaBuilder b;
    b.AddColumn(kRedisKeyColumnName)->Type(BINARY)->NotNull()->HashPrimaryKey();
    RETURN_NOT_OK(b.Build(redis_schema.get()));
    schema(redis_schema.get());
  }
  if (!schema_) {
    return STATUS(InvalidArgument, "Missing schema");
  }

  // Build request.
  master::CreateTableRequestPB req;
  req.set_name(table_name_.table_name());
  table_name_.SetIntoNamespaceIdentifierPB(req.mutable_namespace_());
  req.set_table_type(table_type_);
  req.set_colocated(colocated_);

  if (!creator_role_name_.empty()) {
    req.set_creator_role_name(creator_role_name_);
  }

  if (!table_id_.empty()) {
    req.set_table_id(table_id_);
  }
  if (is_pg_catalog_table_) {
    req.set_is_pg_catalog_table(*is_pg_catalog_table_);
  }
  if (is_pg_shared_table_) {
    req.set_is_pg_shared_table(*is_pg_shared_table_);
  }

  if (!tablegroup_id_.empty()) {
    req.set_tablegroup_id(tablegroup_id_);
  }

  // Note that the check that the sum of min_num_replicas for each placement block being less or
  // equal than the overall placement info num_replicas is done on the master side and an error is
  // naturally returned if you try to create a table and the numbers mismatch. As such, it is the
  // responsibility of the client to ensure that does not happen.
  if (has_replication_info_) {
    req.mutable_replication_info()->CopyFrom(replication_info_);
  }

  SchemaToPB(internal::GetSchema(*schema_), req.mutable_schema());

  if (txn_) {
    txn_->ToPB(req.mutable_transaction());
  }

  // Setup the number splits (i.e. number of splits).
  if (num_tablets_ > 0) {
    VLOG(1) << "num_tablets: number of tablets explicitly specified: " << num_tablets_;
  } else if (schema_->table_properties().num_tablets() > 0) {
    num_tablets_ = schema_->table_properties().num_tablets();
  } else {
    if (table_name_.is_system()) {
      num_tablets_ = 1;
      VLOG(1) << "num_tablets=1: using one tablet for a system table";
    } else {
      num_tablets_ = VERIFY_RESULT(client_->NumTabletsForUserTable(table_type_));
    }
  }
  req.mutable_schema()->mutable_table_properties()->set_num_tablets(num_tablets_);
  req.set_num_tablets(num_tablets_);

  req.mutable_partition_schema()->CopyFrom(partition_schema_);

  // Index mapping with data-table being indexed.
  if (index_info_.has_indexed_table_id()) {
    if (!TEST_use_old_style_create_request_) {
      req.mutable_index_info()->CopyFrom(index_info_);
    }

    // For compatibility reasons, set the old fields just in case we have new clients talking to
    // old master server during rolling upgrade.
    req.set_indexed_table_id(index_info_.indexed_table_id());
    req.set_is_local_index(index_info_.is_local());
    req.set_is_unique_index(index_info_.is_unique());
    req.set_skip_index_backfill(skip_index_backfill_);
  }

  auto deadline = CoarseMonoClock::Now() +
                  (timeout_.Initialized() ? timeout_ : client_->default_admin_operation_timeout());

  auto s = client_->data_->CreateTable(
      client_, req, *schema_, deadline, &table_id_);

  if (!s.ok() && !s.IsAlreadyPresent()) {
      RETURN_NOT_OK_PREPEND(s, strings::Substitute("Error creating $0 $1 on the master",
                                                   object_type, table_name_.ToString()));
  }

  // We are here because the create request succeeded or we received an IsAlreadyPresent error.
  // Although the table is already in the catalog manager, it doesn't mean that the table is
  // ready to receive requests. So we will call WaitForCreateTableToFinish to ensure that once
  // this request returns, the client can send operations without receiving a "Table Not Found"
  // error.

  // Spin until the table is fully created, if requested.
  if (wait_) {
    RETURN_NOT_OK(client_->data_->WaitForCreateTableToFinish(
        client_, YBTableName(), table_id_, deadline));
    if (s.ok() && table_type_ == TableType::YQL_TABLE_TYPE && index_info_.has_indexed_table_id()) {
      auto index_perm = client_->data_->GetIndexPermissions(
          client_, index_info_.indexed_table_id(), table_id_, deadline);
      VLOG(1) << "GetIndexPermissions returned " << index_perm;
      // If we know that the backfill has failed, then return an error.
      // Timeout is not considered an error because ycql does not wait for the backfill
      // to complete.
      if (!index_perm && index_perm.status().IsNotFound()) {
        return STATUS(RemoteError, "Create/backfill index failed.");
      }
    }
  }

  if (s.ok() && !FLAGS_client_suppress_created_logs) {
    LOG(INFO) << "Created " << object_type << " " << table_name_.ToString()
              << " of type " << TableType_Name(table_type_);
  }

  return s;
}

} // namespace client
} // namespace yb
