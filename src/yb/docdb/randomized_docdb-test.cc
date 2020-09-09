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

#include <algorithm>
#include <utility>
#include <vector>

#include <gflags/gflags.h>

#include "yb/docdb/doc_reader.h"
#include "yb/docdb/docdb.h"
#include "yb/docdb/docdb_test_base.h"
#include "yb/docdb/docdb_test_util.h"
#include "yb/util/scope_exit.h"

// Use a lower default number of tests when running on ASAN/TSAN so as not to exceed the test time
// limit.
#if defined(THREAD_SANITIZER) || defined(ADDRESS_SANITIZER)
static constexpr int kDefaultTestNumIter = 2000;
static constexpr int kDefaultSnapshotVerificationTestNumIter = 2000;
#else
static constexpr int kDefaultTestNumIter = 20000;
static constexpr int kDefaultSnapshotVerificationTestNumIter = 15000;
#endif

DEFINE_int32(snapshot_verification_test_num_iter, kDefaultSnapshotVerificationTestNumIter,
             "Number iterations for randomized history cleanup DocDB tests.");

DEFINE_int32(test_num_iter, kDefaultTestNumIter,
             "Number iterations for randomized DocDB tests, except those involving logical DocDB "
             "snapshots.");

constexpr int kNumDocKeys = 50;
constexpr int kNumUniqueSubKeys = 500;

using std::vector;
using std::pair;
using std::sort;

namespace yb {
namespace docdb {

namespace {

void RemoveEntriesWithSecondComponentHigherThan(vector<pair<int, int>> *v,
                                                int max_second_component) {
  // See https://en.wikipedia.org/wiki/Erase-remove_idiom.
  v->erase(
      std::remove_if(v->begin(), v->end(), [&](const pair<int, int>& p) {
          return p.second > max_second_component;
      }),
      v->end());
}

}  // anonymous namespace

class RandomizedDocDBTest : public DocDBTestBase,
    public ::testing::WithParamInterface<ResolveIntentsDuringRead> {
 protected:
  RandomizedDocDBTest() : verify_history_cleanup_(true) {
  }

  void Init(const UseHash use_hash) {
    // This test was created when this was the only supported init marker behavior.
    SetInitMarkerBehavior(InitMarkerBehavior::kRequired);
    if (load_gen_.get() != nullptr) {
      ClearLogicalSnapshots();
      ASSERT_OK(DestroyRocksDB());
      ASSERT_OK(ReopenRocksDB());
    }
    load_gen_.reset(new DocDBLoadGenerator(this, kNumDocKeys, kNumUniqueSubKeys, use_hash,
        resolve_intents_));
    SeedRandom();
  }

  ~RandomizedDocDBTest() override {}
  void RunWorkloadWithSnaphots(bool enable_history_cleanup);

  int num_iterations_divider() {
    // GetSubDocument is slower when trying to resolve intents, so we reduce number of iterations
    // in order to respect the timeout.
    return resolve_intents_ ? 2 : 1;
  }

  void CompactionWithCleanup(HybridTime cleanup_ht) {
    const auto start_time = MonoTime::Now();
    ASSERT_NO_FATALS(FullyCompactHistoryBefore(cleanup_ht));
    const auto elapsed_time_ms = (MonoTime::Now() - start_time).ToMilliseconds();
    total_compaction_time_ms_ += elapsed_time_ms;
    LOG(INFO) << "Compaction with cleanup_ht=" << cleanup_ht << " took "
              << elapsed_time_ms << " ms, all compactions so far: "
              << total_compaction_time_ms_ << " ms";
  }

  ResolveIntentsDuringRead resolve_intents_ = ResolveIntentsDuringRead::kTrue;
  bool verify_history_cleanup_;
  std::unique_ptr<DocDBLoadGenerator> load_gen_;
  int64_t total_compaction_time_ms_ = 0;
};

void RandomizedDocDBTest::RunWorkloadWithSnaphots(bool enable_history_cleanup) {
  auto scope_exit = ScopeExit([this]() {
    LOG(INFO) << "Total compaction time: " << total_compaction_time_ms_ << " ms";
  });
  // We start doing snapshots every other iterations, but make it less frequent after a number of
  // iterations (kIterationToSwitchToInfrequentSnapshots to be precise, see the loop below).
  int snapshot_frequency = 2;
  int verification_frequency = 1;

  constexpr int kEventualSnapshotFrequency = 1000;
  constexpr int kEventualVerificationFrequency = 250;
  constexpr int kFlushFrequency = 100;
  constexpr int kIterationToSwitchToInfrequentSnapshots = 300;

  constexpr int kHistoryCleanupChance = 500;

  vector<pair<int, int>> cleanup_ht_and_iteration;

  HybridTime max_history_cleanup_ht(0);

  const int kNumIter = FLAGS_snapshot_verification_test_num_iter / num_iterations_divider();

  while (load_gen_->next_iteration() <= kNumIter) {
    const int current_iteration = load_gen_->next_iteration();
    if (current_iteration == kIterationToSwitchToInfrequentSnapshots) {
      // This is where we make snapshot/verification less frequent so the test can actually finish.
      snapshot_frequency = kEventualSnapshotFrequency;
      verification_frequency = kEventualVerificationFrequency;
    }
    ASSERT_NO_FATALS(load_gen_->PerformOperation()) << "at iteration " << current_iteration;
    if (current_iteration % kFlushFrequency == 0) {
      ASSERT_OK(FlushRocksDbAndWait());
    }
    if (current_iteration % snapshot_frequency == 0) {
      load_gen_->CaptureDocDbSnapshot();
    }
    if (current_iteration % verification_frequency == 0) {
      ASSERT_NO_FATALS(load_gen_->VerifyRandomDocDbSnapshot());
    }

    if (enable_history_cleanup && load_gen_->NextRandomInt(kHistoryCleanupChance) == 0) {
      // Pick a random cleanup hybrid_time from 0 to the last operation hybrid_time inclusively.
      const HybridTime cleanup_ht = HybridTime(
          load_gen_->NextRandom() % (load_gen_->last_operation_ht().value() + 1));
      if (cleanup_ht.CompareTo(max_history_cleanup_ht) <= 0) {
        // We are performing cleanup at an old hybrid_time, and don't expect it to have any effect.
        InMemDocDbState snapshot_before_cleanup;
        snapshot_before_cleanup.CaptureAt(doc_db(), HybridTime::kMax);
        ASSERT_NO_FATALS(CompactionWithCleanup(cleanup_ht));

        InMemDocDbState snapshot_after_cleanup;
        snapshot_after_cleanup.CaptureAt(doc_db(), HybridTime::kMax);
        ASSERT_TRUE(snapshot_after_cleanup.EqualsAndLogDiff(snapshot_before_cleanup));
      } else {
        max_history_cleanup_ht = cleanup_ht;
        cleanup_ht_and_iteration.emplace_back(cleanup_ht.value(),
                                              load_gen_->last_operation_ht().value());
        ASSERT_NO_FATALS(CompactionWithCleanup(cleanup_ht));

        // We expect some snapshots at hybrid_times earlier than cleanup_ht to no longer be
        // recoverable.
        ASSERT_NO_FATALS(load_gen_->CheckIfOldestSnapshotIsStillValid(cleanup_ht));

        load_gen_->RemoveSnapshotsBefore(cleanup_ht);

        // Now that we're removed all snapshots that could have been affected by history cleanup,
        // we expect the oldest remaining snapshot to match the RocksDB-backed DocDB state.
        ASSERT_NO_FATALS(load_gen_->VerifyOldestSnapshot());
      }
    }
  }

  LOG(INFO) << "Finished the primary part of the randomized DocDB test.\n"
            << "  enable_history_cleanup: " << enable_history_cleanup << "\n"
            << "  last_operation_ht: " << load_gen_->last_operation_ht() << "\n"
            << "  max_history_cleanup_ht: " << max_history_cleanup_ht.value();

  if (!enable_history_cleanup || !verify_history_cleanup_) return;

  if (FLAGS_snapshot_verification_test_num_iter > kDefaultSnapshotVerificationTestNumIter) {
    LOG(WARNING)
        << "Number of iterations specified for the history cleanup test is greater than "
        << kDefaultSnapshotVerificationTestNumIter << ", and therefore this test is "
        << "NOT CHECKING THAT OLD SNAPSHOTS ARE INVALIDATED BY HISTORY CLEANUP.";
    return;
  }

  // Verify that some old snapshots got invalidated by history cleanup at a higher hybrid_time.

  // First we verify that history cleanup is happening at expected times, so that we can validate
  // that the maximum history cleanup hybrid_time (max_history_cleanup_ht) is as expected.

  // An entry (t, i) here says that after iteration i there was a history cleanup with a history
  // cutoff hybrid_time of t. The iteration here corresponds one to one to the operation
  // hybrid_time. We always have t < i because we perform cleanup at a past hybrid_time,
  // not a future one.
  //                                                        cleanup_ht | iteration (last op. ts.)
  //
  // These numbers depend on DocDB load generator parameters (random seed, frequencies of various
  // events) and will need to be replaced in such cases. Ideally, we should come up with a way to
  // either re-generate those quickly, or not rely on hard-coded expected results for validation.
  // However, we do handle variations in the number of iterations here, up a certain limit.
  vector<pair<int, int>> expected_cleanup_ht_and_iteration{{1,           85},
                                                           {40,          121},
                                                           {46,          255},
                                                           {245,         484},
                                                           {774,         2246},
                                                           {2341,        3417},
                                                           {2741,        5248},
                                                           {4762,        5652},
                                                           {5049,        6377},
                                                           {6027,        7573},
                                                           {8423,        9531},
                                                           {8829,        10413},
                                                           {10061,       10610},
                                                           {13137,       13920}};

  // Remove expected (cleanup_hybrid_time, iteration) entries that don't apply to our test run in
  // case we did fewer than 15000 iterations.
  RemoveEntriesWithSecondComponentHigherThan(
      &expected_cleanup_ht_and_iteration,
      load_gen_->last_operation_ht().value());

  ASSERT_FALSE(expected_cleanup_ht_and_iteration.empty());
  ASSERT_EQ(expected_cleanup_ht_and_iteration, cleanup_ht_and_iteration);

  if (kNumIter > 2000) {
    ASSERT_GT(load_gen_->num_divergent_old_snapshot(), 0);
  } else {
    ASSERT_EQ(0, load_gen_->num_divergent_old_snapshot());
  }

  // Expected hybrid_times of snapshots invalidated by history cleanup, and actual history cutoff
  // hybrid_times at which that happened. This is deterministic, but highly dependent on the
  // parameters at the top of this test.
  vector<pair<int, int>> expected_divergent_snapshot_and_cleanup_ht{
      {298,   774},
      {2000,  2341},
      {4000,  4762},
      {5000,  5049},
      {6000,  6027},
      {8000,  8423},
      {10000, 10061},
      {13000, 13137}
  };

  // Remove entries that don't apply to us because we did not get to do a cleanup at that
  // hybrid_time.
  RemoveEntriesWithSecondComponentHigherThan(&expected_divergent_snapshot_and_cleanup_ht,
                                             max_history_cleanup_ht.value());

  ASSERT_EQ(expected_divergent_snapshot_and_cleanup_ht,
            load_gen_->divergent_snapshot_ht_and_cleanup_ht());
}

TEST_P(RandomizedDocDBTest, TestNoFlush) {
  resolve_intents_ = GetParam();
  const int num_iter = FLAGS_test_num_iter / num_iterations_divider();
  for (auto use_hash : UseHash::kValues) {
    Init(use_hash);
    while (load_gen_->next_iteration() <= num_iter) {
      ASSERT_NO_FATALS(load_gen_->PerformOperation()) << "at iteration " <<
          load_gen_->next_iteration();
    }
  }
}

TEST_P(RandomizedDocDBTest, TestWithFlush) {
  resolve_intents_ = GetParam();
  const int num_iter = FLAGS_test_num_iter / num_iterations_divider();
  for (auto use_hash : UseHash::kValues) {
    Init(use_hash);
    while (load_gen_->next_iteration() <= num_iter) {
      ASSERT_NO_FATALS(load_gen_->PerformOperation()) << "at iteration "
                                                      << load_gen_->next_iteration();
      if (load_gen_->next_iteration() % 250 == 0) {
        ASSERT_NO_FATALS(load_gen_->FlushRocksDB());
      }
    }
  }
}

TEST_P(RandomizedDocDBTest, Snapshots) {
  resolve_intents_ = GetParam();
  for (auto use_hash : UseHash::kValues) {
    Init(use_hash);
    RunWorkloadWithSnaphots(/* enable_history_cleanup = */ false);
  }
}

TEST_P(RandomizedDocDBTest, SnapshotsWithHistoryCleanup) {
  resolve_intents_ = GetParam();
  for (auto use_hash : UseHash::kValues) {
    Init(use_hash);
    // Don't verify history cleanup in case we use hashed components, since hardcoded expected
    // values doesn't work for that use case.
    // TODO: update expected values or find a better way to test it.
    verify_history_cleanup_ = !use_hash;
    RunWorkloadWithSnaphots(/* enable_history_cleanup = */ true);
  }
}

INSTANTIATE_TEST_CASE_P(bool, RandomizedDocDBTest, ::testing::Values(
    ResolveIntentsDuringRead::kFalse, ResolveIntentsDuringRead::kTrue));

// This is a bit different from SnapshotsWithHistoryCleanup. Here, we perform history cleanup within
// DocDBLoadGenerator::PerformOperation itself, reading the document being modified both before
// and after the history cleanup.
TEST_F(RandomizedDocDBTest, ImmediateHistoryCleanup) {
  for (auto use_hash : UseHash::kValues) {
    Init(use_hash);
    while (load_gen_->next_iteration() <= FLAGS_test_num_iter) {
      if (load_gen_->next_iteration() % 250 == 0) {
        ASSERT_NO_FATALS(load_gen_->FlushRocksDB());
        ASSERT_NO_FATALS(load_gen_->PerformOperation(/* history_cleanup = */ true));
      } else {
        ASSERT_NO_FATALS(load_gen_->PerformOperation());
      }
    }
  }
}

}  // namespace docdb
}  // namespace yb
