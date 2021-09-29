// Copyright 2017 The Ray Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// clang-format off
#include "gtest/gtest.h"
#include "gmock/gmock.h"
#include "ray/gcs/gcs_server/gcs_placement_group_manager.h"
#include "mock/ray/gcs/gcs_server/gcs_placement_group_manager.h"
#include "mock/ray/gcs/gcs_server/gcs_placement_group_scheduler.h"
#include "mock/ray/gcs/gcs_server/gcs_resource_manager.h"
#include "mock/ray/gcs/store_client/store_client.h"
#include "ray/gcs/test/gcs_test_util.h"
// clang-format on

using namespace ::testing;
using namespace ray;
using namespace ray::gcs;
namespace ray {
namespace gcs {

class GcsPlacementGroupManagerMockTest : public  Test {
 public:
  void SetUp() override {
    store_client_ = std::make_shared<MockStoreClient>();
    gcs_table_storage_ = std::make_shared<GcsTableStorage>(store_client_);
    gcs_placement_group_scheduler_ = std::make_shared<MockGcsPlacementGroupSchedulerInterface>();
    resource_manager_ = std::make_shared<MockGcsResourceManager>(
        io_context_,
        nullptr,
        nullptr,
        true);

    gcs_placement_group_manager_ = std::make_unique<GcsPlacementGroupManager>(
        io_context_,
        gcs_placement_group_scheduler_,
        gcs_table_storage_,
        *resource_manager_,
        [](auto&) {
          return "";
        });
  }
  std::unique_ptr<GcsPlacementGroupManager> gcs_placement_group_manager_;
  std::shared_ptr<MockGcsPlacementGroupSchedulerInterface> gcs_placement_group_scheduler_;
  std::shared_ptr<gcs::GcsTableStorage> gcs_table_storage_;
  std::shared_ptr<MockStoreClient> store_client_;
  std::shared_ptr<GcsResourceManager> resource_manager_;
  instrumented_io_context io_context_;
};


TEST_F(GcsPlacementGroupManagerMockTest, PendingQueuePriorityReschedule) {
  // Test priority works
  //   When return with reschedule, it should be given with the highest pri
  auto req = Mocker::GenCreatePlacementGroupRequest("", rpc::PlacementStrategy::SPREAD, 1);
  auto pg = std::make_shared<GcsPlacementGroup>(req, "");
  auto cb = [](Status s) {
            };
  PGSchedulingFailureCallback failure_callback;
  PGSchedulingSuccessfulCallback success_callback;
  StatusCallback put_cb;
  EXPECT_CALL(*store_client_, AsyncPut(_, _, _, _))
      .WillOnce(DoAll(SaveArg<3>(&put_cb), Return(Status::OK())));
  EXPECT_CALL(*gcs_placement_group_scheduler_, ScheduleUnplacedBundles(_, _, _))
      .WillOnce(DoAll(SaveArg<1>(&failure_callback), SaveArg<2>(&success_callback)));
  auto now = absl::GetCurrentTimeNanos();
  gcs_placement_group_manager_->RegisterPlacementGroup(pg, cb);
  auto& pending_queue = gcs_placement_group_manager_->pending_placement_groups_;
  ASSERT_EQ(1, pending_queue.size());
  ASSERT_LE(now, pending_queue.begin()->first);
  ASSERT_GE(absl::GetCurrentTimeNanos(), pending_queue.begin()->first);
  put_cb(Status::OK());
  pg->UpdateState(rpc::PlacementGroupTableData::RESCHEDULING);
  failure_callback(pg, true);
  ASSERT_EQ(1, pending_queue.size());
  ASSERT_GE(0, pending_queue.begin()->first);
}

TEST_F(GcsPlacementGroupManagerMockTest, PendingQueuePriorityFailed) {
  // Test priority works
  //   When return with a failure, exp backoff should work
  auto req = Mocker::GenCreatePlacementGroupRequest("", rpc::PlacementStrategy::SPREAD, 1);
  auto pg = std::make_shared<GcsPlacementGroup>(req, "");
  auto cb = [](Status s) {
            };
  PGSchedulingFailureCallback failure_callback;
  PGSchedulingSuccessfulCallback success_callback;
  StatusCallback put_cb;
  EXPECT_CALL(*store_client_, AsyncPut(_, _, _, _))
      .WillOnce(DoAll(SaveArg<3>(&put_cb), Return(Status::OK())));
  EXPECT_CALL(*gcs_placement_group_scheduler_, ScheduleUnplacedBundles(_, _, _))
      .Times(2)
      .WillRepeatedly(DoAll(SaveArg<1>(&failure_callback), SaveArg<2>(&success_callback)));
  auto now = absl::GetCurrentTimeNanos();
  gcs_placement_group_manager_->RegisterPlacementGroup(pg, cb);
  auto& pending_queue = gcs_placement_group_manager_->pending_placement_groups_;
  ASSERT_EQ(1, pending_queue.size());
  ASSERT_LE(now, pending_queue.begin()->first);
  ASSERT_GE(absl::GetCurrentTimeNanos(), pending_queue.begin()->first);
  put_cb(Status::OK());
  pg->UpdateState(rpc::PlacementGroupTableData::PENDING);
  now = absl::GetCurrentTimeNanos();
  failure_callback(pg, true);
  auto exp_backer = ExponentialBackOff(
      RayConfig::instance().gcs_create_placement_group_retry_min_interval_ms(),
      RayConfig::instance().gcs_create_placement_group_retry_multiplier(),
      1000000 *
      RayConfig::instance().gcs_create_placement_group_retry_max_interval_ms());
  auto next = exp_backer.Next();
  EXPECT_DOUBLE_EQ(next, RayConfig::instance().gcs_create_placement_group_retry_min_interval_ms());
  ASSERT_EQ(1, pending_queue.size());
  ASSERT_LT(now + next, pending_queue.begin()->first);

  gcs_placement_group_manager_->SchedulePendingPlacementGroups();
  pg->UpdateState(rpc::PlacementGroupTableData::PENDING);
  now = absl::GetCurrentTimeNanos();
  failure_callback(pg, true);
  next = RayConfig::instance().gcs_create_placement_group_retry_multiplier() * next;
  EXPECT_DOUBLE_EQ(next, RayConfig::instance().gcs_create_placement_group_retry_min_interval_ms());
  ASSERT_EQ(1, pending_queue.size());
  ASSERT_LT(now + next, pending_queue.begin()->first);
}

}
}