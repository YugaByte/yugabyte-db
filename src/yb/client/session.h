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

#ifndef YB_CLIENT_SESSION_H
#define YB_CLIENT_SESSION_H

#include <unordered_set>

#include "yb/client/client_fwd.h"

#include "yb/common/common_fwd.h"
#include "yb/common/hybrid_time.h"

#include "yb/util/async_util.h"
#include "yb/util/locks.h"
#include "yb/util/monotime.h"

namespace yb {

class ConsistentReadPoint;

struct ReadHybridTime;

namespace client {

namespace internal {
class Batcher;
class ErrorCollector;
} // internal

YB_STRONGLY_TYPED_BOOL(VerifyResponse);
YB_STRONGLY_TYPED_BOOL(Restart);

// A YBSession belongs to a specific YBClient, and represents a context in
// which all read/write data access should take place. Within a session,
// multiple operations may be accumulated and batched together for better
// efficiency. Settings like timeouts, priorities, and trace IDs are also set
// per session.
//
// A YBSession's main purpose is for grouping together multiple data-access
// operations together into batches or transactions. It is important to note
// the distinction between these two:
//
// * A batch is a set of operations which are grouped together in order to
//   amortize fixed costs such as RPC call overhead and round trip times.
//   A batch DOES NOT imply any ACID-like guarantees. Within a batch, some
//   operations may succeed while others fail, and concurrent readers may see
//   partial results. If the client crashes mid-batch, it is possible that some
//   of the operations will be made durable while others were lost.
//
// * In contrast, a transaction is a set of operations which are treated as an
//   indivisible semantic unit, per the usual definitions of database transactions
//   and isolation levels.
//
// YBSession is separate from YBTable because a given batch or transaction
// may span multiple tables. Even in the context of batching and not multi-table
// ACID transactions, we may be able to coalesce writes to different tables hosted
// on the same server into the same RPC.
//
// YBSession is separate from YBClient because, in a multi-threaded
// application, different threads may need to concurrently execute
// transactions. Similar to a JDBC "session", transaction boundaries will be
// delineated on a per-session basis -- in between a "BeginTransaction" and
// "Commit" call on a given session, all operations will be part of the same
// transaction. Meanwhile another concurrent Session object can safely run
// non-transactional work or other transactions without interfering.
//
// Additionally, there is a guarantee that writes from different sessions do not
// get batched together into the same RPCs -- this means that latency-sensitive
// clients can run through the same YBClient object as throughput-oriented
// clients, perhaps by setting the latency-sensitive session's timeouts low and
// priorities high. Without the separation of batches, a latency-sensitive
// single-row insert might get batched along with 10MB worth of inserts from the
// batch writer, thus delaying the response significantly.
//
// Users who are familiar with the Hibernate ORM framework should find this
// concept of a Session familiar.
//
// This class is not thread-safe.
class YBSession : public std::enable_shared_from_this<YBSession> {
 public:
  explicit YBSession(YBClient* client, const scoped_refptr<ClockBase>& clock = nullptr);

  ~YBSession();

  // Set the consistent read point used by the non-transactional operations in this session. If the
  // operations are restarted and last read point indicates the operations do need to be restarted,
  // the read point will be updated to restart read-time. Otherwise, the read point will be set to
  // the current time.
  void SetReadPoint(Restart restart);

  void SetReadPoint(const ReadHybridTime& read_time);

  // Returns true if our current read point requires restart.
  bool IsRestartRequired();

  // Defer the read hybrid time to the global limit.  Since the global limit should not change for a
  // session, this call is idempotent.
  void DeferReadPoint();

  // Used for backfilling the index, where we may want to write with a historic timestamp.
  void SetHybridTimeForWrite(const HybridTime ht);

  // Changed transaction used by this session.
  void SetTransaction(YBTransactionPtr transaction);

  // Set the timeout for writes made in this session.
  void SetTimeout(MonoDelta timeout);

  MonoDelta timeout() const {
    return timeout_;
  }

  CHECKED_STATUS ReadSync(std::shared_ptr<YBOperation> yb_op);

  void ReadAsync(std::shared_ptr<YBOperation> yb_op, StatusFunctor callback);

  // TODO: add "doAs" ability here for proxy servers to be able to act on behalf of
  // other users, assuming access rights.

  // Apply the write operation.
  //
  // The behavior of this function depends on the current flush mode. Regardless
  // of flush mode, however, Apply may begin to perform processing in the background
  // for the call (e.g looking up the tablet, etc). Given that, an error may be
  // queued into the PendingErrors structure prior to flushing, even in MANUAL_FLUSH
  // mode.
  //
  // In case of any error, which may occur during flushing or because the write_op
  // is malformed, the write_op is stored in the session's error collector which
  // may be retrieved at any time.
  //
  // This is thread safe.
  CHECKED_STATUS Apply(YBOperationPtr yb_op);
  CHECKED_STATUS ApplyAndFlush(YBOperationPtr yb_op);

  // verify_response - supported only in auto flush mode. Checks that after flush operation
  // is succeeded. (i.e. op->succeeded() returns true).
  CHECKED_STATUS Apply(const std::vector<YBOperationPtr>& ops);
  CHECKED_STATUS ApplyAndFlush(const std::vector<YBOperationPtr>& ops,
                               VerifyResponse verify_response = VerifyResponse::kFalse);

  // Flush any pending writes.
  //
  // Returns a bad status if there are any pending errors after the rows have
  // been flushed. Callers should then use GetPendingErrors to determine which
  // specific operations failed.
  //
  // In AUTO_FLUSH_SYNC mode, this has no effect, since every Apply() call flushes
  // itself inline.
  //
  // In the case that the async version of this method is used, then the callback
  // will be called upon completion of the operations which were buffered since the
  // last flush. In other words, in the following sequence:
  //
  //    session->Insert(a);
  //    session->FlushAsync(callback_1);
  //    session->Insert(b);
  //    session->FlushAsync(callback_2);
  //
  // ... 'callback_2' will be triggered once 'b' has been inserted, regardless of whether
  // 'a' has completed or not.
  //
  // Note that this also means that, if FlushAsync is called twice in succession, with
  // no intervening operations, the second flush will return immediately. For example:
  //
  //    session->Insert(a);
  //    session->FlushAsync(callback_1); // called when 'a' is inserted
  //    session->FlushAsync(callback_2); // called immediately!
  //
  // Note that, as in all other async functions in YB, the callback may be called
  // either from an IO thread or the same thread which calls FlushAsync. The callback
  // should not block.
  //
  // For FlushAsync, 'cb' must remain valid until it is invoked.
  CHECKED_STATUS Flush() WARN_UNUSED_RESULT;
  void FlushAsync(StatusFunctor callback);
  std::future<Status> FlushFuture();

  // Abort the unflushed or in-flight operations in the session.
  void Abort();

  // Close the session.
  // Returns Status::IllegalState() if 'force' is false and there are still pending
  // operations. If 'force' is true batcher_ is aborted even if there are pending
  // operations.
  CHECKED_STATUS Close(bool force = false);

  // Return true if there are operations which have not yet been delivered to the
  // cluster. This may include buffered operations (i.e those that have not yet been
  // flushed) as well as in-flight operations (i.e those that are in the process of
  // being sent to the servers).
  // TODO: maybe "incomplete" or "undelivered" is clearer?
  bool HasPendingOperations() const;

  // Return the number of buffered operations. These are operations that have
  // not yet been flushed - i.e they are not en-route yet.
  //
  // Note that this is different than HasPendingOperations() above, which includes
  // operations which have been sent and not yet responded to.
  //
  // This is only relevant in MANUAL_FLUSH mode, where the result will not
  // decrease except for after a manual Flush, after which point it will be 0.
  // In the other flush modes, data is immediately put en-route to the destination,
  // so this will return 0.
  int CountBufferedOperations() const;

  // Return the number of errors which are pending.
  int CountPendingErrors() const;

  // Return any errors from previous calls. If there were more errors
  // than could be held in the session's error storage, then sets *overflowed to true.
  //
  // Caller takes ownership of the returned errors.
  CollectedErrors GetPendingErrors();

  // Allow local calls to run in the current thread.
  void set_allow_local_calls_in_curr_thread(bool flag);
  bool allow_local_calls_in_curr_thread() const;

  // Sets in transaction read limit for this session.
  void SetInTxnLimit(HybridTime value);

  YBClient* client() const;

  // Sets force consistent read mode, if true then consistent read point will be used even we have
  // only one command to flush.
  // It is useful when whole statement is executed using multiple flushes.
  void SetForceConsistentRead(ForceConsistentRead value);

  const internal::AsyncRpcMetricsPtr& async_rpc_metrics() const {
    return async_rpc_metrics_;
  }

  // Called by Batcher when a flush has finished.
  void FlushFinished(internal::BatcherPtr b);

  ConsistentReadPoint* read_point();

  void SetRejectionScoreSource(RejectionScoreSourcePtr rejection_score_source);

 private:
  friend class YBClient;
  friend class internal::Batcher;

  internal::Batcher& Batcher();

  // The client that this session is associated with.
  client::YBClient* const client_;

  std::unique_ptr<ConsistentReadPoint> read_point_;
  YBTransactionPtr transaction_;
  bool allow_local_calls_in_curr_thread_ = true;
  bool force_consistent_read_ = false;

  // Lock protecting flushed_batchers_.
  mutable simple_spinlock lock_;

  // Buffer for errors.
  scoped_refptr<internal::ErrorCollector> error_collector_;

  // The current batcher being prepared.
  scoped_refptr<internal::Batcher> batcher_;

  // Any batchers which have been flushed but not yet finished.
  //
  // Upon a batch finishing, it will call FlushFinished(), which removes the batcher from
  // this set. This set does not hold any reference count to the Batcher, since, while
  // the flush is active, the batcher manages its own refcount. The Batcher will always
  // call FlushFinished() before it destructs itself, so we're guaranteed that these
  // pointers stay valid.
  std::unordered_set<
      internal::BatcherPtr, ScopedRefPtrHashFunctor, ScopedRefPtrEqualsFunctor> flushed_batchers_;

  // Timeout for the next batch.
  MonoDelta timeout_;

  // HybridTime for Write. Used for Index Backfill.
  HybridTime hybrid_time_for_write_;

  internal::AsyncRpcMetricsPtr async_rpc_metrics_;

  RejectionScoreSourcePtr rejection_score_source_;

  DISALLOW_COPY_AND_ASSIGN(YBSession);
};

} // namespace client
} // namespace yb

#endif // YB_CLIENT_SESSION_H
