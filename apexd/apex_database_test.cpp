/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "apex_database.h"

#include <android-base/macros.h>
#include <android-base/result-gmock.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <tuple>

using android::base::Error;
using android::base::Result;
using android::base::testing::HasError;
using android::base::testing::Ok;
using android::base::testing::WithMessage;

namespace android {
namespace apex {
namespace {

using MountedApexData = MountedApexDatabase::MountedApexData;

size_t CountPackages(const MountedApexDatabase& db) {
  size_t ret = 0;
  db.ForallMountedApexes([&ret](const std::string& a ATTRIBUTE_UNUSED,
                                const MountedApexData& b ATTRIBUTE_UNUSED,
                                bool c ATTRIBUTE_UNUSED) { ++ret; });
  return ret;
}

bool Contains(const MountedApexDatabase& db, const std::string& package,
              const MountedApexData& data) {
  bool found = false;
  db.ForallMountedApexes([&](const std::string& p, const MountedApexData& d,
                             bool b ATTRIBUTE_UNUSED) {
    if (package == p && data == d) {
      found = true;
    }
  });
  return found;
}

bool ContainsPackage(const MountedApexDatabase& db, const std::string& package,
                     const MountedApexData& data) {
  bool found = false;
  db.ForallMountedApexes(
      package, [&](const MountedApexData& d, bool b ATTRIBUTE_UNUSED) {
        if (data == d) {
          found = true;
        }
      });
  return found;
}

TEST(ApexDatabaseTest, AddRemovedMountedApex) {
  constexpr const char* kPackage = "package";
  constexpr const char* kLoopName = "loop";
  constexpr const char* kPath = "path";
  constexpr const char* kMountPoint = "mount";
  constexpr const char* kDeviceName = "dev";

  MountedApexDatabase db;
  ASSERT_EQ(CountPackages(db), 0u);

  MountedApexData data(0, kLoopName, kPath, kMountPoint, kDeviceName);
  db.AddMountedApex(kPackage, data);
  ASSERT_TRUE(Contains(db, kPackage, data));
  ASSERT_TRUE(ContainsPackage(db, kPackage, data));

  db.RemoveMountedApex(kPackage, kPath);
  EXPECT_FALSE(Contains(db, kPackage, data));
  EXPECT_FALSE(ContainsPackage(db, kPackage, data));
}

TEST(ApexDatabaseTest, MountMultiple) {
  constexpr const char* kPackage[] = {"package", "package", "package",
                                      "package"};
  constexpr const char* kLoopName[] = {"loop", "loop2", "loop3", "loop4"};
  constexpr const char* kPath[] = {"path", "path2", "path", "path4"};
  constexpr const char* kMountPoint[] = {"mount", "mount2", "mount", "mount4"};
  constexpr const char* kDeviceName[] = {"dev", "dev2", "dev3", "dev4"};

  MountedApexDatabase db;
  ASSERT_EQ(CountPackages(db), 0u);

  MountedApexData data[arraysize(kPackage)];
  for (size_t i = 0; i < arraysize(kPackage); ++i) {
    data[i] = MountedApexData(0, kLoopName[i], kPath[i], kMountPoint[i],
                              kDeviceName[i]);
    db.AddMountedApex(kPackage[i], data[i]);
  }

  ASSERT_EQ(CountPackages(db), 4u);
  for (size_t i = 0; i < arraysize(kPackage); ++i) {
    ASSERT_TRUE(Contains(db, kPackage[i], data[i]));
    ASSERT_TRUE(ContainsPackage(db, kPackage[i], data[i]));
  }

  db.RemoveMountedApex(kPackage[0], kPath[0]);
  EXPECT_FALSE(Contains(db, kPackage[0], data[0]));
  EXPECT_FALSE(ContainsPackage(db, kPackage[0], data[0]));
  EXPECT_TRUE(Contains(db, kPackage[1], data[1]));
  EXPECT_TRUE(ContainsPackage(db, kPackage[1], data[1]));
  EXPECT_TRUE(Contains(db, kPackage[2], data[2]));
  EXPECT_TRUE(ContainsPackage(db, kPackage[2], data[2]));
  EXPECT_TRUE(Contains(db, kPackage[3], data[3]));
  EXPECT_TRUE(ContainsPackage(db, kPackage[3], data[3]));
}

TEST(ApexDatabaseTest, DoIfLatest) {
  // Check by passing error-returning handler
  // When handler is triggered, DoIfLatest() returns the expected error.
  auto returnError = []() -> Result<void> { return Error() << "expected"; };

  MountedApexDatabase db;

  // With apex: [{version=0,path=path}]
  db.AddMountedApex("package", 0, "loop", "path", "mount", "dev");
  ASSERT_THAT(db.DoIfLatest("package", "path", returnError),
              HasError(WithMessage("expected")));

  // With apexes: [{version=0,path=path}, {version=5,path=path5}]
  db.AddMountedApex("package", 5, "loop5", "path5", "mount5", "dev5");
  ASSERT_THAT(db.DoIfLatest("package", "path", returnError), Ok());
  ASSERT_THAT(db.DoIfLatest("package", "path5", returnError),
              HasError(WithMessage("expected")));
}

TEST(ApexDatabaseTest, GetLatestMountedApex) {
  constexpr const char* kPackage = "package";
  constexpr const char* kLoopName = "loop";
  constexpr const char* kPath = "path";
  constexpr const char* kMountPoint = "mount";
  constexpr const char* kDeviceName = "dev";

  MountedApexDatabase db;
  ASSERT_EQ(CountPackages(db), 0u);

  MountedApexData data(0, kLoopName, kPath, kMountPoint, kDeviceName);
  db.AddMountedApex(kPackage, data);

  auto ret = db.GetLatestMountedApex(kPackage);
  ASSERT_TRUE(ret.has_value());
  ASSERT_EQ(ret.value(), data);
}

TEST(ApexDatabaseTest, GetLatestMountedApexReturnsNullopt) {
  MountedApexDatabase db;
  auto ret = db.GetLatestMountedApex("no-such-name");
  ASSERT_FALSE(ret.has_value());
}

}  // namespace
}  // namespace apex
}  // namespace android
