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

#include <algorithm>
#include <mutex>
#include <vector>

#include "yb/consensus/log-test-base.h"
#include "yb/consensus/log_index.h"
#include "yb/gutil/algorithm.h"
#include "yb/gutil/ref_counted.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/util/locks.h"
#include "yb/util/random.h"
#include "yb/util/thread.h"

// TODO: Semantics of the Log and Appender thread interactions changed and now multi-threaded
// writing is no longer allowed, or to be more precise, does no longer guarantee the ordering of
// events being written, across threads.
DEFINE_int32(num_writer_threads, 1, "Number of threads writing to the log");
DEFINE_int32(num_batches_per_thread, 2000, "Number of batches per thread");
DEFINE_int32(num_ops_per_batch_avg, 5, "Target average number of ops per batch");

namespace yb {
namespace log {

using std::vector;
using consensus::ReplicateMsgPtr;

namespace {

class CustomLatchCallback : public RefCountedThreadSafe<CustomLatchCallback> {
 public:
  CustomLatchCallback(CountDownLatch* latch, vector<Status>* errors)
      : latch_(latch),
        errors_(errors) {
  }

  void StatusCB(const Status& s) {
    if (!s.ok()) {
      errors_->push_back(s);
    }
    latch_->CountDown();
  }

  StatusCallback AsStatusCallback() {
    return Bind(&CustomLatchCallback::StatusCB, this);
  }

 private:
  CountDownLatch* latch_;
  vector<Status>* errors_;
};

} // anonymous namespace

extern const char *kTestTablet;

class MultiThreadedLogTest : public LogTestBase {
 public:
  MultiThreadedLogTest()
      : random_(SeedRandom()) {
  }

  void SetUp() override {
    LogTestBase::SetUp();
  }

  void LogWriterThread(int thread_id) {
    CountDownLatch latch(FLAGS_num_batches_per_thread);
    vector<Status> errors;
    for (int i = 0; i < FLAGS_num_batches_per_thread; i++) {
      LogEntryBatch* entry_batch;
      ReplicateMsgs batch_replicates;
      int num_ops = static_cast<int>(random_.Normal(
          static_cast<double>(FLAGS_num_ops_per_batch_avg), 1.0));
      DVLOG(1) << num_ops << " ops in this batch";
      num_ops =  std::max(num_ops, 1);
      {
        std::lock_guard<simple_spinlock> lock_guard(lock_);
        for (int j = 0; j < num_ops; j++) {
          auto replicate = std::make_shared<ReplicateMsg>();
          int32_t index = current_index_++;
          OpIdPB* op_id = replicate->mutable_id();
          op_id->set_term(0);
          op_id->set_index(index);

          replicate->set_op_type(WRITE_OP);
          replicate->set_hybrid_time(clock_->Now().ToUint64());

          tserver::WriteRequestPB* request = replicate->mutable_write_request();
          AddTestRowInsert(index, 0, "this is a test insert", request);
          request->set_tablet_id(kTestTablet);
          batch_replicates.push_back(replicate);
        }

        auto entry_batch_pb = CreateBatchFromAllocatedOperations(batch_replicates);

        ASSERT_OK(log_->Reserve(REPLICATE, &entry_batch_pb, &entry_batch));
      } // lock_guard scope
      auto cb = new CustomLatchCallback(&latch, &errors);
      ASSERT_OK(log_->TEST_AsyncAppendWithReplicates(
          entry_batch, batch_replicates, cb->AsStatusCallback()));
    }
    LOG_TIMING(INFO, strings::Substitute("thread $0 waiting to append and sync $1 batches",
                                        thread_id, FLAGS_num_batches_per_thread)) {
      latch.Wait();
    }
    for (const Status& status : errors) {
      WARN_NOT_OK(status, "Unexpected failure during AsyncAppend");
    }
    ASSERT_EQ(0, errors.size());
  }

  void Run() {
    for (int i = 0; i < FLAGS_num_writer_threads; i++) {
      scoped_refptr<yb::Thread> new_thread;
      CHECK_OK(yb::Thread::Create("test", "inserter",
          &MultiThreadedLogTest::LogWriterThread, this, i, &new_thread));
      threads_.push_back(new_thread);
    }
    for (scoped_refptr<yb::Thread>& thread : threads_) {
      ASSERT_OK(ThreadJoiner(thread.get()).Join());
    }
  }
 private:
  ThreadSafeRandom random_;
  simple_spinlock lock_;
  vector<scoped_refptr<yb::Thread> > threads_;
};

TEST_F(MultiThreadedLogTest, TestAppends) {
  BuildLog();
  int start_current_id = current_index_;
  LOG_TIMING(INFO, strings::Substitute("inserting $0 batches($1 threads, $2 per-thread)",
                                      FLAGS_num_writer_threads * FLAGS_num_batches_per_thread,
                                      FLAGS_num_batches_per_thread, FLAGS_num_writer_threads)) {
    ASSERT_NO_FATALS(Run());
  }
  ASSERT_OK(log_->Close());

  std::unique_ptr<LogReader> reader;
  ASSERT_OK(LogReader::Open(fs_manager_->env(), NULL, kTestTablet,
                            fs_manager_->GetFirstTabletWalDirOrDie(kTestTable, kTestTablet),
                            fs_manager_->uuid(),
                            NULL, &reader));
  SegmentSequence segments;
  ASSERT_OK(reader->GetSegmentsSnapshot(&segments));

  std::vector<int64_t> ids;
  for (const SegmentSequence::value_type& entry : segments) {
    auto read_entries = entry->ReadEntries();
    ASSERT_OK(read_entries.status);
    for (const auto& entry : read_entries.entries) {
      if (entry->type() == REPLICATE) {
        ids.push_back(entry->replicate().id().index());
      }
    }
  }
  DVLOG(1) << "Wrote total of " << current_index_ - start_current_id << " ops";
  ASSERT_EQ(current_index_ - start_current_id, ids.size());
  ASSERT_TRUE(std::is_sorted(ids.begin(), ids.end()));
}

} // namespace log
} // namespace yb
