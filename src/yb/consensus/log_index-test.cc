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

#include "yb/consensus/log_index.h"

#include "yb/consensus/opid_util.h"
#include "yb/fs/fs_manager.h"
#include "yb/util/test_util.h"

namespace yb {
namespace log {

using consensus::MakeOpId;

class LogIndexTest : public YBTest {
 public:
  void SetUp() override {
    YBTest::SetUp();
    index_ = new LogIndex(GetTestDataDirectory());
  }

 protected:
  Status AddEntry(const OpIdPB& op_id, int64_t segment, int64_t offset) {
    LogIndexEntry entry;
    entry.op_id = yb::OpId::FromPB(op_id);
    entry.segment_sequence_number = segment;
    entry.offset_in_segment = offset;
    return index_->AddEntry(entry);
  }

  void VerifyEntry(const OpIdPB& op_id, int64_t segment, int64_t offset) {
    SCOPED_TRACE(op_id);
    LogIndexEntry result;
    EXPECT_OK(index_->GetEntry(op_id.index(), &result));
    EXPECT_EQ(op_id.term(), result.op_id.term);
    EXPECT_EQ(op_id.index(), result.op_id.index);
    EXPECT_EQ(segment, result.segment_sequence_number);
    EXPECT_EQ(offset, result.offset_in_segment);
  }

  void VerifyNotFound(int64_t index) {
    SCOPED_TRACE(index);
    LogIndexEntry result;
    Status s = index_->GetEntry(index, &result);
    EXPECT_TRUE(s.IsNotFound()) << s.ToString();
  }

  scoped_refptr<LogIndex> index_;
};


TEST_F(LogIndexTest, TestBasic) {
  // Insert three entries.
  ASSERT_OK(AddEntry(MakeOpId(1, 1), 1, 12345));
  ASSERT_OK(AddEntry(MakeOpId(1, 999999), 1, 999));
  ASSERT_OK(AddEntry(MakeOpId(1, 1500000), 1, 54321));
  VerifyEntry(MakeOpId(1, 1), 1, 12345);
  VerifyEntry(MakeOpId(1, 999999), 1, 999);
  VerifyEntry(MakeOpId(1, 1500000), 1, 54321);

  // Overwrite one.
  ASSERT_OK(AddEntry(MakeOpId(5, 1), 1, 50000));
  VerifyEntry(MakeOpId(5, 1), 1, 50000);
}

// This test relies on kEntriesPerIndexChunk being 1000000, and that's no longer
// the case after D1719 (2fe27d886390038bc734ea28638a1b1435e7d0d4) on Mac.
#if !defined(__APPLE__)
TEST_F(LogIndexTest, TestMultiSegmentWithGC) {
  ASSERT_OK(AddEntry(MakeOpId(1, 1), 1, 12345));
  ASSERT_OK(AddEntry(MakeOpId(1, 1000000), 1, 54321));
  ASSERT_OK(AddEntry(MakeOpId(1, 1500000), 1, 54321));
  ASSERT_OK(AddEntry(MakeOpId(1, 2500000), 1, 12345));

  // GCing indexes < 1,000,000 shouldn't have any effect, because we can't remove any whole segment.
  for (int gc = 0; gc < 1000000; gc += 100000) {
    SCOPED_TRACE(gc);
    index_->GC(gc);
    VerifyEntry(MakeOpId(1, 1), 1, 12345);
    VerifyEntry(MakeOpId(1, 1000000), 1, 54321);
    VerifyEntry(MakeOpId(1, 1500000), 1, 54321);
    VerifyEntry(MakeOpId(1, 2500000), 1, 12345);
  }

  // If we GC index 1000000, we should lose the first op.
  index_->GC(1000000);
  VerifyNotFound(1);
  VerifyEntry(MakeOpId(1, 1000000), 1, 54321);
  VerifyEntry(MakeOpId(1, 1500000), 1, 54321);
  VerifyEntry(MakeOpId(1, 2500000), 1, 12345);

  // GC everything
  index_->GC(9000000);
  VerifyNotFound(1);
  VerifyNotFound(1000000);
  VerifyNotFound(1500000);
  VerifyNotFound(2500000);
}
#endif

} // namespace log
} // namespace yb
