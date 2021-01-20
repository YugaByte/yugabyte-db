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

#include "yb/rocksdb/db/db_test_util.h"
#include "yb/util/path_util.h"
#include "yb/util/test_util.h"

using namespace std::literals;

namespace rocksdb {

namespace {
  constexpr auto kNumCompactionTrigger = 4;

  void AssertLoggedWaitFor(std::function<Result<bool>()> condition, const string& description) {
    yb::AssertLoggedWaitFor(condition, 60s, description, 100ms);
  }
}

class OnFileCreationListener : public EventListener {
 public:
  OnFileCreationListener() {}

  void OnTableFileCreated(
      const TableFileCreationInfo& info) override {
    LOG(INFO) << "Created SST file: " << info.file_path;

    auto file_name = yb::BaseName(info.file_path);

    bool do_pause;
    {
      std::lock_guard<std::mutex> l(mutex_);
      created_file_names_.push_back(file_name);
      do_pause = created_file_names_.size() > pause_after_num_files_created_;
    }

    if (do_pause) {
      AssertLoggedWaitFor(
          [this, &file_name] {
            std::lock_guard<std::mutex> l(mutex_);
            return file_names_to_resume_.erase(file_name) > 0;
          }, yb::Format("Pausing on $0 ...", file_name));
    }
  }

  void SetPauseAfterFilesCreated(size_t n) {
    pause_after_num_files_created_ = n;
  }

  // Disable pausing newly created files, but will hold already paused ones until they are resumed
  // by ResumeFileName call.
  void DisablePausing() {
    pause_after_num_files_created_ = std::numeric_limits<size_t>::max();
  }

  void ResumeFileName(const std::string& file_name) {
    std::lock_guard<std::mutex> l(mutex_);
    file_names_to_resume_.insert(file_name);
  }

  std::vector<std::string> CreatedFileNames() {
    std::lock_guard<std::mutex> l(mutex_);
    return created_file_names_;
  }

  const std::string& GetLastCreatedFileName() {
    std::lock_guard<std::mutex> l(mutex_);
    return created_file_names_.back();
  }

  size_t NumFilesCreated() {
    std::lock_guard<std::mutex> l(mutex_);
    return created_file_names_.size();
  }

 private:
  std::atomic<size_t> pause_after_num_files_created_{std::numeric_limits<size_t>::max()};
  std::mutex mutex_;
  std::unordered_set<std::string> file_names_to_resume_;
  std::vector<std::string> created_file_names_;
};

class DBTestUniversalCompactionDeletion : public DBTestBase {
 public:
  DBTestUniversalCompactionDeletion() :
      DBTestBase("/db_universal_compaction_deletion_test"), rnd_(301) {}

  // Creates SST file of size around, but not less than 1MB, uses key range
  // [num_sst_files_ * 50; num_sst_files_ * 50 + 100).
  void CreateSstFile(bool do_flush = true) {
    for (int j = 0; j < 100; ++j) {
      ASSERT_OK(Put(Key(num_sst_files_ * 50 + j), RandomString(&rnd_, 10_KB)));
    }
    if (do_flush) {
      ASSERT_OK(Flush());
    }
    ++num_sst_files_;
  }

  Options CurrentOptions() {
    Options options = DBTestBase::CurrentOptions();
    options.env = env_;
    options.compaction_style = kCompactionStyleUniversal;
    options.num_levels = 1;
    options.write_buffer_size = 2_MB;
    options.max_bytes_for_level_base = 1_MB;
    options.level0_file_num_compaction_trigger = kNumCompactionTrigger;
    options.max_background_flushes = 2;
    options.max_background_compactions = 2;
    options.listeners.push_back(file_create_listener_);

    return options;
  }

  void WaitForNumFilesCreated(const std::string& desc, size_t num_files) {
    AssertLoggedWaitFor(
        [this, num_files] { return file_create_listener_->NumFilesCreated() >= num_files; }, desc);
  }

  Random rnd_;
  int num_sst_files_ = 0;
  std::shared_ptr<OnFileCreationListener> file_create_listener_ =
      std::make_shared<OnFileCreationListener>();
};

// This reproduces an issue where we delete a file too late because when it was supposed to be
// deleted, it was blocked by concurrent flush.
// Consider following scenario which was possible before the issue was fixed:
// - Compaction (1) starts with base version #1 and input files #111-#114.
// - Flush (2) starts with base version #2 (which also includes files #111-#114) and increments ref
// counter of version #2.
// - Compaction (1) finishes, but input files #111 and #111-#114 are not deleted, because they are
// being held by version #2, which is being held by flush (2).
// - Flush (2) finishes and decrements ref counter of version #2.
// - Compaction (3) starts.
// - Compaction (3) finishes and purging obsolete SST files including #111-#114.
TEST_F(DBTestUniversalCompactionDeletion, DeleteObsoleteFilesDelayedByFlush) {
  Options options = CurrentOptions();
  Reopen(options);

  file_create_listener_->SetPauseAfterFilesCreated(kNumCompactionTrigger);
  for (int i = 0; i < kNumCompactionTrigger; ++i) {
    CreateSstFile();
  }

  std::vector<LiveFileMetaData> input_files;
  db_->GetLiveFilesMetaData(&input_files);
  for (auto file : input_files) {
    LOG(INFO) << "Input file: " << file.ToString();
  }

  WaitForNumFilesCreated("Waiting for compaction (1) delay ...", kNumCompactionTrigger + 1);
  const auto compaction_1_output = file_create_listener_->GetLastCreatedFileName();

  size_t num_files = file_create_listener_->NumFilesCreated();
  CreateSstFile(false /* do_flush */);
  std::thread flusher([this] {
    ASSERT_OK(Flush());
  });

  WaitForNumFilesCreated("Waiting for flush (2) delay ...", num_files + 1);
  const auto flush_2_output = file_create_listener_->GetLastCreatedFileName();
  file_create_listener_->DisablePausing();

  LOG(INFO) << "Resuming compaction (1) ...";
  file_create_listener_->ResumeFileName(compaction_1_output);
  AssertLoggedWaitFor(
      [this] { return dbfull()->TEST_NumTotalRunningCompactions() == 0; },
      "Waiting for compaction (1) to be completed ...");

  // Compaction (1) input files should be deleted before flush (2) is completed.
  for (auto file : input_files) {
    ASSERT_TRUE(env_->FileExists(dbname_ + file.name).IsNotFound());
  }

  LOG(INFO) << "Resuming flush (2) ...";
  file_create_listener_->ResumeFileName(flush_2_output);
  flusher.join();
}

// This reproduces an issue where we delete compacted files too late because when they were
// supposed to be deleted, it was blocked by concurrent huge compaction job with lower pending
// output file number.
// Consider following scenario which was possible before the issue was fixed:
// - Huge compaction (1) starts to write output file #110.
// - New files #111-#114 are written.
// - Compaction (2) starts with input files #111-#114.
// - Compaction (2) finishes, but input files #111-#114 are not deleted, because their numbers
// are bigger than #110.
// - Huge compaction (1) finishes.
// - Compaction (3) starts.
// - Compaction (3) finishes and purging obsolete SST files including #111-#114.
TEST_F(DBTestUniversalCompactionDeletion, DeleteObsoleteFilesMinPendingOutput) {
  Options options = CurrentOptions();
  Reopen(options);

  // Simulate huge long-running compaction (1).
  file_create_listener_->SetPauseAfterFilesCreated(kNumCompactionTrigger);
  for (int i = 0; i < kNumCompactionTrigger; ++i) {
    CreateSstFile();
  }
  WaitForNumFilesCreated("Waiting for compaction (1) delay ...", kNumCompactionTrigger + 1);
  const auto compaction_1_output = file_create_listener_->GetLastCreatedFileName();
  file_create_listener_->DisablePausing();

  std::vector<LiveFileMetaData> live_files_1;
  db_->GetLiveFilesMetaData(&live_files_1);
  // Write new files to be compacted by compaction (2).
  for (int i = 0; i < kNumCompactionTrigger; ++i) {
    CreateSstFile();
  }
  std::unordered_set<std::string> input_files_2;
  {
    std::vector<LiveFileMetaData> live_files;
    db_->GetLiveFilesMetaData(&live_files);
    for (auto file : live_files) {
      input_files_2.insert(file.name);
    }
    for (auto file : live_files_1) {
      input_files_2.erase(file.name);
    }
  }

  AssertLoggedWaitFor(
      [this] { return dbfull()->TEST_NumTotalRunningCompactions() == 1; },
      "Waiting for compaction (2) to be completed ...");

  // Compaction (2) input files should be deleted before compaction (1) is completed.
  for (auto file_name : input_files_2) {
    ASSERT_TRUE(env_->FileExists(dbname_ + file_name).IsNotFound());
  }

  LOG(INFO) << "Resuming compaction (1)  ...";
  file_create_listener_->ResumeFileName(compaction_1_output);
  dbfull()->TEST_WaitForCompact();
}

// This reproduces an issue where we delete compacted files too late because when they were
// supposed to be deleted, it was blocked by scheduled compaction holding input version
// referring these files.
// Consider following scenario which was possible before the issue was fixed:
// - Compaction (1) starts with input files #111-#114.
// - Flush job (2) starts with base version #10 including files #111-#114 and increments ref
// counter of version #10.
// - Right before finishing flush job (2) it schedules another compaction (3) with base version #10
// and due to this increments ref counter of version #10 again.
// - Flush job (2) finishes, but input files #111-#114 are not deleted, because they are being
// held by version #10 (blocked by scheduled compaction (3)).
// - Compaction (1) finishes, but input files #111-#114 are not deleted, because they are being
// held by version #10.
// - Compaction (3) starts.
// - Compaction (3) finishes and purging obsolete SST files including #111-#114.
TEST_F(DBTestUniversalCompactionDeletion, DeleteObsoleteFilesDelayedByScheduledCompaction) {
  Options options = CurrentOptions();
  Reopen(options);

  file_create_listener_->SetPauseAfterFilesCreated(kNumCompactionTrigger);
  // Trigger compaction (1).
  for (int i = 0; i < kNumCompactionTrigger; ++i) {
    CreateSstFile();
  }

  std::vector<LiveFileMetaData> input_files;
  db_->GetLiveFilesMetaData(&input_files);
  for (auto file : input_files) {
    LOG(INFO) << "Input file: " << file.ToString();
  }

  WaitForNumFilesCreated("Waiting for compaction (1) delay ...", kNumCompactionTrigger + 1);
  const auto compaction_1_output = file_create_listener_->GetLastCreatedFileName();

  // Allow kNumCompactionTrigger more files to be created without delay and enqueue compaction (3).
  file_create_listener_->SetPauseAfterFilesCreated(
      file_create_listener_->NumFilesCreated() + kNumCompactionTrigger);
  for (int i = 0; i < kNumCompactionTrigger; ++i) {
    CreateSstFile();
  }

  AssertLoggedWaitFor(
      [this] { return dbfull()->TEST_NumRunningFlushes() == 0; },
      "Waiting for flush (2) completion ...");

  AssertLoggedWaitFor(
      [this] { return dbfull()->TEST_NumBackgroundCompactionsScheduled() == 2; },
      "Waiting for compaction (3) to be enqueued ...");

  LOG(INFO) << "Resuming compaction (1)  ...";
  file_create_listener_->ResumeFileName(compaction_1_output);
  AssertLoggedWaitFor(
      [this, &compaction_1_output] {
        std::vector<LiveFileMetaData> files;
        db_->GetLiveFilesMetaData(&files);
        for (auto file : files) {
          if (file.name == '/' + compaction_1_output) {
            return true;
          }
        }
        return false;
      }, "Waiting for compaction (1) to be completed ...");

  // Compaction (1) input files should be deleted before compaction (3) is completed.
  for (auto file : input_files) {
    ASSERT_TRUE(env_->FileExists(dbname_ + file.name).IsNotFound());
  }

  // Need to wait for compaction 3 to actually generate its output file, before we try to get that
  // filename below.
  AssertLoggedWaitFor(
      [this]{ return file_create_listener_->NumFilesCreated() == 2 * kNumCompactionTrigger + 2; },
      "Waiting for compaction (3) to be completed");

  const auto compaction_3_output = file_create_listener_->GetLastCreatedFileName();
  file_create_listener_->ResumeFileName(compaction_3_output);
  dbfull()->TEST_WaitForCompact();
}

}  // namespace rocksdb

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
