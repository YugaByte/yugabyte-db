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

#include "yb/util/thread.h"

#include <string>

#include <gtest/gtest.h>

#include "yb/gutil/ref_counted.h"
#include "yb/util/env.h"
#include "yb/util/test_util.h"
#include "yb/util/thread_restrictions.h"

using std::string;
using namespace std::literals;

namespace yb {

class ThreadTest : public YBTest {};

// Join with a thread and emit warnings while waiting to join.
// This has to be manually verified.
TEST_F(ThreadTest, TestJoinAndWarn) {
  if (!AllowSlowTests()) {
    LOG(INFO) << "Skipping test in quick test mode, since this sleeps";
    return;
  }

  scoped_refptr<Thread> holder;
  ASSERT_OK(Thread::Create("test", "sleeper thread", usleep, 1000*1000, &holder));
  ASSERT_OK(ThreadJoiner(holder.get())
                   .warn_after(10ms)
                   .warn_every(100ms)
                   .Join());
}

TEST_F(ThreadTest, TestFailedJoin) {
  if (!AllowSlowTests()) {
    LOG(INFO) << "Skipping test in quick test mode, since this sleeps";
    return;
  }

  scoped_refptr<Thread> holder;
  ASSERT_OK(Thread::Create("test", "sleeper thread", usleep, 1000*1000, &holder));
  Status s = ThreadJoiner(holder.get())
    .give_up_after(50ms)
    .Join();
  ASSERT_STR_CONTAINS(s.ToString(), "Timed out after 50ms joining on sleeper thread");
}

static void TryJoinOnSelf() {
  Status s = ThreadJoiner(Thread::current_thread()).Join();
  // Use CHECK instead of ASSERT because gtest isn't thread-safe.
  CHECK(s.IsInvalidArgument());
}

// Try to join on the thread that is currently running.
TEST_F(ThreadTest, TestJoinOnSelf) {
  scoped_refptr<Thread> holder;
  ASSERT_OK(Thread::Create("test", "test", TryJoinOnSelf, &holder));
  holder->Join();
  // Actual assertion is done by the thread spawned above.
}

TEST_F(ThreadTest, TestDoubleJoinIsNoOp) {
  scoped_refptr<Thread> holder;
  ASSERT_OK(Thread::Create("test", "sleeper thread", usleep, 0, &holder));
  ThreadJoiner joiner(holder.get());
  ASSERT_OK(joiner.Join());
  ASSERT_OK(joiner.Join());
}


namespace {

void ExitHandler(string* s, const char* to_append) {
  *s += to_append;
}

void CallAtExitThread(string* s) {
  Thread::current_thread()->CallAtExit(Bind(&ExitHandler, s, Unretained("hello 1, ")));
  Thread::current_thread()->CallAtExit(Bind(&ExitHandler, s, Unretained("hello 2")));
}

} // anonymous namespace

TEST_F(ThreadTest, TestCallOnExit) {
  scoped_refptr<Thread> holder;
  string s;
  ASSERT_OK(Thread::Create("test", "TestCallOnExit", CallAtExitThread, &s, &holder));
  holder->Join();
  ASSERT_EQ("hello 1, hello 2", s);
}

TEST_F(ThreadTest, TestThreadNameWithoutPadding) {
  ThreadPtr t;
  string name = string(25, 'a');
  t = CHECK_RESULT(Thread::Make("test", name, [](){}));
  ASSERT_EQ(t->name(), name);
}

TEST_F(ThreadTest, TestThreadNameWithPadding) {
  ThreadPtr t;
  string name = string(5, 'a');
  t = CHECK_RESULT(Thread::Make("test", name, [](){}));
  ASSERT_EQ(t->name(), name + string(10, Thread::kPaddingChar));
}

// The following tests only run in debug mode, since thread restrictions are no-ops
// in release builds.
#ifndef NDEBUG
TEST_F(ThreadTest, TestThreadRestrictions_IO) {
  // Default should be to allow IO
  ThreadRestrictions::AssertIOAllowed();

  ThreadRestrictions::SetIOAllowed(false);
  {
    ThreadRestrictions::ScopedAllowIO allow_io;
    ASSERT_TRUE(Env::Default()->FileExists("/"));
  }
  ThreadRestrictions::SetIOAllowed(true);

  // Disallow IO - doing IO should crash the process.
  ASSERT_DEATH({
      ThreadRestrictions::SetIOAllowed(false);
      ignore_result(Env::Default()->FileExists("/"));
    },
    "Function marked as IO-only was called from a thread that disallows IO");
}

TEST_F(ThreadTest, TestThreadRestrictions_Waiting) {
  // Default should be to allow IO
  ThreadRestrictions::AssertWaitAllowed();

  ThreadRestrictions::SetWaitAllowed(false);
  {
    ThreadRestrictions::ScopedAllowWait allow_wait;
    CountDownLatch l(0);
    l.Wait();
  }
  ThreadRestrictions::SetWaitAllowed(true);

  // Disallow waiting - blocking on a latch should crash the process.
  ASSERT_DEATH({
      ThreadRestrictions::SetWaitAllowed(false);
      CountDownLatch l(1);
      l.WaitFor(std::chrono::seconds(1s));
    },
    "Waiting is not allowed to be used on this thread");
}
#endif // NDEBUG

} // namespace yb
