/*
 * Copyright (C) 2025 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "apexd_image_manager.h"

#include <android-base/result-gmock.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "apexd_test_utils.h"

using namespace std::literals;

using android::base::testing::HasValue;
using android::base::testing::Ok;
using testing::IsEmpty;

namespace android::apex {

TEST(ApexImageManagerTest, EmptyWhenDirectoriesAreNotReady) {
  // For the first boot, apexd-bootstrap starts without /data or /metadata
  // directories.
  auto image_manager =
      ApexImageManager::Create("/no-metadata-images", "/no-data-images");
  ASSERT_THAT(image_manager->GetAllImages(), IsEmpty());
}

TEST(ApexImageManagerTest, PinApexFiles) {
  TemporaryDir metadata_dir;
  TemporaryDir data_dir;
  auto image_manager =
      ApexImageManager::Create(metadata_dir.path, data_dir.path);

  auto apex1 = ApexFile::Open(GetTestFile("apex.apexd_test.apex"));
  ASSERT_THAT(apex1, Ok());
  auto apex2 =
      ApexFile::Open(GetTestFile("apex.apexd_test_different_app.apex"));
  ASSERT_THAT(apex2, Ok());
  ASSERT_THAT(image_manager->PinApexFiles(std::vector{*apex1, *apex2}),
              HasValue(std::vector{"com.android.apex.test_pack_0.apex"s,
                                   "com.android.apex.test_pack_1.apex"s}));
}

TEST(ApexImageManagerTest, ManageApexList) {
  TemporaryDir metadata_dir;
  TemporaryDir data_dir;
  auto image_manager =
      ApexImageManager::Create(metadata_dir.path, data_dir.path);

  ASSERT_THAT(image_manager->GetApexList(ApexListType::ACTIVE),
              HasValue(IsEmpty()));

  std::vector<ApexListEntry> list;
  list.emplace_back("image1", "package1");
  list.emplace_back("image2", "package2");
  ASSERT_THAT(image_manager->UpdateApexList(ApexListType::ACTIVE, list), Ok());
  ASSERT_THAT(image_manager->GetApexList(ApexListType::ACTIVE), HasValue(list));
}

TEST(ApexImageManagerTest, UpdateApexListMultipleTimes) {
  TemporaryDir metadata_dir;
  TemporaryDir data_dir;
  auto image_manager =
      ApexImageManager::Create(metadata_dir.path, data_dir.path);

  // Write/read empty list
  ASSERT_THAT(image_manager->UpdateApexList(ApexListType::ACTIVE, {}), Ok());
  ASSERT_THAT(image_manager->GetApexList(ApexListType::ACTIVE),
              HasValue(IsEmpty()));

  // Update should overwrite the list
  auto list =
      std::vector<ApexListEntry>{{"image", "apex"}, {"image2", "apex2"}};
  ASSERT_THAT(image_manager->UpdateApexList(ApexListType::ACTIVE, list), Ok());
  ASSERT_THAT(image_manager->GetApexList(ApexListType::ACTIVE), HasValue(list));

  // Update the list again with empty list
  ASSERT_THAT(image_manager->UpdateApexList(ApexListType::ACTIVE, {}), Ok());
  ASSERT_THAT(image_manager->GetApexList(ApexListType::ACTIVE),
              HasValue(IsEmpty()));
}

TEST(UpdateApexListWithNewEntries, AddNew) {
  auto list = std::vector<ApexListEntry>{};
  auto new_entries = std::vector<ApexListEntry>{
      {"image1", "apex1"},
      {"image2", "apex2"},
  };
  auto updated = std::vector<ApexListEntry>{
      {"image1", "apex1"},
      {"image2", "apex2"},
  };
  ASSERT_EQ(UpdateApexListWithNewEntries(list, new_entries), updated);
}

TEST(UpdateApexListWithNewEntries, ReplaceAndAddNew) {
  auto list = std::vector<ApexListEntry>{
      {"image1", "apex1"},
      {"image2", "apex2"},
  };
  auto new_entries = std::vector<ApexListEntry>{
      {"image2_1", "apex2"},
      {"image3", "apex3"},
  };
  auto updated = std::vector<ApexListEntry>{
      {"image1", "apex1"},
      {"image2_1", "apex2"},
      {"image3", "apex3"},
  };
  ASSERT_EQ(UpdateApexListWithNewEntries(list, new_entries), updated);
}

TEST(UpdateApexListWithNewEntries, ReplaceAll) {
  auto list = std::vector<ApexListEntry>{
      {"image1", "apex1"},
      {"image2", "apex2"},
  };
  auto new_entries = std::vector<ApexListEntry>{
      {"image1_1", "apex1"},
      {"image2_1", "apex2"},
  };
  auto updated = std::vector<ApexListEntry>{
      {"image1_1", "apex1"},
      {"image2_1", "apex2"},
  };
  ASSERT_EQ(UpdateApexListWithNewEntries(list, new_entries), updated);
}

}  // namespace android::apex