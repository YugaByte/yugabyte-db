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

#ifndef YB_DOCDB_REDIS_OPERATION_H
#define YB_DOCDB_REDIS_OPERATION_H

#include "yb/common/redis_protocol.pb.h"

#include "yb/docdb/deadline_info.h"
#include "yb/docdb/doc_key.h"
#include "yb/docdb/doc_operation.h"
#include "yb/docdb/expiration.h"

#include "yb/rocksdb/cache.h"

namespace yb {
namespace docdb {

// Redis value data with attached type of this value.
// Used internally by RedisWriteOperation.
struct RedisValue {
  RedisDataType type = static_cast<RedisDataType>(0);
  std::string value;
  Expiration exp;
  int64_t internal_index = 0;
};

class RedisWriteOperation :
    public DocOperationBase<DocOperationType::REDIS_WRITE_OPERATION, RedisWriteRequestPB> {
 public:
  // Construct a RedisWriteOperation. Content of request will be swapped out by the constructor.
  explicit RedisWriteOperation(RedisWriteRequestPB* request) {
    request_.Swap(request);
  }

  bool RequireReadSnapshot() const override { return false; }

  CHECKED_STATUS Apply(const DocOperationApplyData& data) override;

  CHECKED_STATUS GetDocPaths(
      GetDocPathsMode mode, DocPathsToLock *paths, IsolationLevel *level) const override;

  RedisResponsePB &response() { return response_; }

 private:
  void ClearResponse() override {
    response_.Clear();
  }

  void InitializeIterator(const DocOperationApplyData& data);
  Result<RedisDataType> GetValueType(const DocOperationApplyData& data,
      int subkey_index = kNilSubkeyIndex);
  Result<RedisValue> GetValue(const DocOperationApplyData& data,
      int subkey_index = kNilSubkeyIndex, Expiration* exp = nullptr);

  CHECKED_STATUS ApplySetTtl(const DocOperationApplyData& data);
  CHECKED_STATUS ApplySet(const DocOperationApplyData& data);
  CHECKED_STATUS ApplyGetSet(const DocOperationApplyData& data);
  CHECKED_STATUS ApplyAppend(const DocOperationApplyData& data);
  CHECKED_STATUS ApplyDel(const DocOperationApplyData& data);
  CHECKED_STATUS ApplySetRange(const DocOperationApplyData& data);
  CHECKED_STATUS ApplyIncr(const DocOperationApplyData& data);
  CHECKED_STATUS ApplyPush(const DocOperationApplyData& data);
  CHECKED_STATUS ApplyInsert(const DocOperationApplyData& data);
  CHECKED_STATUS ApplyPop(const DocOperationApplyData& data);
  CHECKED_STATUS ApplyAdd(const DocOperationApplyData& data);
  CHECKED_STATUS ApplyRemove(const DocOperationApplyData& data);

  RedisResponsePB response_;
  // TODO: Currently we have a separate iterator per operation, but in future, we leave the option
  // open for operations to share iterators.
  std::unique_ptr<IntentAwareIterator> iterator_;

  rocksdb::QueryId redis_query_id() { return reinterpret_cast<rocksdb::QueryId > (&request_); }
};

class RedisReadOperation {
 public:
  explicit RedisReadOperation(const yb::RedisReadRequestPB& request,
                              const DocDB& doc_db,
                              CoarseTimePoint deadline,
                              const ReadHybridTime& read_time)
      : request_(request), doc_db_(doc_db), deadline_(deadline), read_time_(read_time) {}

  CHECKED_STATUS Execute();

  const RedisResponsePB &response();

 private:
  Result<RedisDataType> GetValueType(int subkey_index = kNilSubkeyIndex);

  // GetValue when always_override should be true.
  // This is particularly relevant for the Timeseries datatype, for which
  // child TTLs override parent TTLs.
  // TODO: Once the timeseries bug is fixed, this function as well as the
  // corresponding field can be safely removed.
  Result<RedisValue>GetOverrideValue(int subkey_index = kNilSubkeyIndex);

  Result<RedisValue> GetValue(int subkey_index = kNilSubkeyIndex);

  int ApplyIndex(int32_t index, const int32_t len);
  CHECKED_STATUS ExecuteGet();
  CHECKED_STATUS ExecuteGet(const RedisGetRequestPB& get_request);
  CHECKED_STATUS ExecuteGet(RedisGetRequestPB::GetRequestType type);
  CHECKED_STATUS ExecuteGetForRename();
  CHECKED_STATUS ExecuteGetTtl();
  // Used to implement HGETALL, HKEYS, HVALS, SMEMBERS, HLEN, SCARD
  CHECKED_STATUS ExecuteHGetAllLikeCommands(
                                    ValueType value_type,
                                    bool add_keys,
                                    bool add_values);
  CHECKED_STATUS ExecuteStrLen();
  CHECKED_STATUS ExecuteExists();
  CHECKED_STATUS ExecuteGetRange();
  CHECKED_STATUS ExecuteCollectionGetRange();
  CHECKED_STATUS ExecuteCollectionGetRangeByBounds(
      RedisCollectionGetRangeRequestPB::GetRangeRequestType request_type, bool add_keys);
  CHECKED_STATUS ExecuteCollectionGetRangeByBounds(
      RedisCollectionGetRangeRequestPB::GetRangeRequestType request_type,
      const RedisSubKeyBoundPB& lower_bound, const RedisSubKeyBoundPB& upper_bound, bool add_keys);
  CHECKED_STATUS ExecuteKeys();

  rocksdb::QueryId redis_query_id() { return reinterpret_cast<rocksdb::QueryId> (&request_); }

  const RedisReadRequestPB& request_;
  RedisResponsePB response_;
  const DocDB doc_db_;
  CoarseTimePoint deadline_;
  ReadHybridTime read_time_;
  // TODO: Move iterator_ to a superclass of RedisWriteOperation RedisReadOperation
  // Make these two classes similar in terms of how rocksdb state is passed to them.
  // Currently ReadOperations get the state during construction, but Write operations get them when
  // calling Apply(). Apply() and Execute() should be more similar() in definition.
  std::unique_ptr<IntentAwareIterator> iterator_;

  boost::optional<DeadlineInfo> deadline_info_;
};

}  // namespace docdb
}  // namespace yb

#endif // YB_DOCDB_REDIS_OPERATION_H
