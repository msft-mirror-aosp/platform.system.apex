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
#include <android-base/scopeguard.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "apexd_image_manager_private.h"
#include "apexd_test_utils.h"
#include "apexd_utils.h"

using namespace std::literals;

using android::base::make_scope_guard;
using android::base::testing::HasError;
using android::base::testing::HasValue;
using android::base::testing::Ok;
using android::base::testing::WithMessage;
using testing::Eq;
using testing::HasSubstr;
using testing::IsEmpty;
using testing::Optional;
using testing::ResultOf;
using testing::SizeIs;

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

TEST(ApexImageManagerTest, PinAndMapPreservesMtime) {
  TemporaryDir metadata_dir;
  TemporaryDir data_dir;
  auto image_manager =
      ApexImageManager::Create(metadata_dir.path, data_dir.path);

  auto apex = ApexFile::Open(GetTestFile("apex.apexd_test.apex"));
  ASSERT_THAT(apex, Ok());
  auto mtime = GetLastModifiedTime(apex->GetPath());
  ASSERT_THAT(mtime, Ok());

  auto images = image_manager->PinApexFiles(std::vector{*apex});
  ASSERT_THAT(images, HasValue(SizeIs(1)));
  auto image = images->at(0);

  auto dev = image_manager->MapImage(image);
  ASSERT_THAT(dev, Ok());
  auto guard = make_scope_guard(
      [&]() { ASSERT_THAT(image_manager->UnmapImage(image), Ok()); });

  ASSERT_THAT(GetLastModifiedTime(*dev), HasValue(*mtime));
}

TEST(ApexImageManagerTest, FindPinnedApex) {
  TemporaryDir metadata_dir;
  TemporaryDir data_dir;
  auto image_manager =
      ApexImageManager::Create(metadata_dir.path, data_dir.path);

  auto apex = ApexFile::Open(GetTestFile("apex.apexd_test.apex"));
  ASSERT_THAT(apex, Ok());
  auto images = image_manager->PinApexFiles(std::vector{*apex});
  ASSERT_THAT(images, HasValue(SizeIs(1)));
  auto image = images->at(0);

  auto dev = image_manager->MapImage(image);
  ASSERT_THAT(dev, Ok());
  auto guard = make_scope_guard(
      [&]() { ASSERT_THAT(image_manager->UnmapImage(image), Ok()); });

  auto apex_from_mapped = ApexFile::Open(dev.value());
  ASSERT_THAT(apex_from_mapped, Ok());

  // Find() works with ApexFile opened from the mapped image.
  ASSERT_THAT(image_manager->FindPinnedApex(*apex), Eq(std::nullopt));
  ASSERT_THAT(image_manager->FindPinnedApex(*apex_from_mapped),
              Optional(image));
}

TEST(ApexImageManagerTest, GetMappedPath) {
  TemporaryDir metadata_dir;
  TemporaryDir data_dir;
  auto image_manager =
      ApexImageManager::Create(metadata_dir.path, data_dir.path);

  auto apex = ApexFile::Open(GetTestFile("apex.apexd_test.apex"));
  ASSERT_THAT(apex, Ok());
  auto images = image_manager->PinApexFiles(std::vector{*apex});
  ASSERT_THAT(images, HasValue(SizeIs(1)));
  auto image = images->at(0);

  ASSERT_THAT(image_manager->GetMappedPath(image), Eq(std::nullopt));

  auto dev = image_manager->MapImage(image);
  ASSERT_THAT(dev, Ok());
  auto guard = make_scope_guard(
      [&]() { ASSERT_THAT(image_manager->UnmapImage(image), Ok()); });

  ASSERT_THAT(image_manager->GetMappedPath(image), Optional(dev.value()));
}

TEST(ApexImageManagerTest, AddApexFilesMultipleTimes) {
  TemporaryDir metadata_dir;
  TemporaryDir data_dir;
  auto image_manager =
      ApexImageManager::Create(metadata_dir.path, data_dir.path);

  auto apex = ApexFile::Open(GetTestFile("apex.apexd_test.apex"));
  ASSERT_THAT(image_manager->PinApexFiles(std::vector{*apex}),
              HasValue(SizeIs(1)));
  ASSERT_THAT(image_manager->PinApexFiles(std::vector{*apex}),
              HasValue(SizeIs(1)));
  ASSERT_THAT(image_manager->GetAllImages(), SizeIs(2));
}

TEST(ApexImageManagerTest, AddDeleteAndAdd) {
  TemporaryDir metadata_dir;
  TemporaryDir data_dir;
  auto image_manager =
      ApexImageManager::Create(metadata_dir.path, data_dir.path);

  auto apex = ApexFile::Open(GetTestFile("apex.apexd_test.apex"));
  auto images = image_manager->PinApexFiles(std::vector{*apex});
  ASSERT_THAT(images, HasValue(SizeIs(1)));

  ASSERT_THAT(image_manager->DeleteImage(images->at(0)), Ok());
  ASSERT_THAT(image_manager->GetAllImages(), SizeIs(0));

  ASSERT_THAT(image_manager->PinApexFiles(std::vector{*apex}),
              HasValue(SizeIs(1)));
  ASSERT_THAT(image_manager->GetAllImages(), SizeIs(1));
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

TEST(FreeSpaceAllocator, CreateImage_AllocateFromStart) {
  FreeSpaceAllocator alloc{{{0, 100}}};
  EXPECT_THAT(alloc.CreateImage("a", 30),
              HasValue(std::vector<Interval>{{0, 30}}));
  EXPECT_THAT(alloc.CreateImage("b", 30),
              HasValue(std::vector<Interval>{{30, 30}}));
}

TEST(FreeSpaceAllocator, CreateImage_NoSpace) {
  FreeSpaceAllocator alloc{{{0, 100}}};
  EXPECT_THAT(alloc.CreateImage("a", 200),
              HasError(WithMessage(HasSubstr("Failed to allocate"))));
}

TEST(FreeSpaceAllocator, CreateImage_AllocateTheBiggestExtentFirst) {
  //            0    30        100                200
  // free:      [    ]         [                  ]
  // alloc(50):                ##########
  // alloc(30):                          ######
  // alloc(40): ######                         ##
  FreeSpaceAllocator alloc{{{0, 30}, {100, 100}}};
  EXPECT_THAT(alloc.CreateImage("a", 50),
              HasValue(std::vector<Interval>{{100, 50}}));
  EXPECT_THAT(alloc.CreateImage("b", 30),
              HasValue(std::vector<Interval>{{150, 30}}));
  EXPECT_THAT(alloc.CreateImage("c", 40),
              HasValue(std::vector<Interval>{{0, 30}, {180, 10}}));
}

TEST(ApexStoragePerImageCreator, CreateImage) {
  TemporaryDir data_dir;
  auto creator = ApexStoragePerImageCreator(data_dir.path);
  EXPECT_THAT(creator.CreateImage("a", 100), Ok());
  EXPECT_THAT(creator.CreateImage("b", 100), Ok());
  EXPECT_TRUE(std::filesystem::exists(data_dir.path + "/a"s));
  EXPECT_TRUE(std::filesystem::exists(data_dir.path + "/b"s));
}

TEST(ApexStoragePerImageCreator, CreateImage_Overwrite) {
  TemporaryDir data_dir;
  auto creator = ApexStoragePerImageCreator(data_dir.path);
  EXPECT_THAT(creator.CreateImage("a", 100),
              HasValue(ResultOf("length", &IntervalsGetLength, Eq(100))));
  EXPECT_THAT(creator.CreateImage("a", 200),
              HasValue(ResultOf("length", &IntervalsGetLength, Eq(200))));
}

TEST(ApexStoragePerImageCreator, CleanUpOnExit) {
  TemporaryDir data_dir;
  {
    auto creator = ApexStoragePerImageCreator(data_dir.path);
    EXPECT_THAT(creator.CreateImage("a", 100), Ok());
  }
  EXPECT_FALSE(std::filesystem::exists(data_dir.path + "/a"s));

  {
    auto creator = ApexStoragePerImageCreator(data_dir.path);
    EXPECT_THAT(creator.CreateImage("a", 100), Ok());
    creator.MarkDone();
  }
  EXPECT_TRUE(std::filesystem::exists(data_dir.path + "/a"s));
}

}  // namespace android::apex
