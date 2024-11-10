/*
 * Copyright (C) 2024 The Android Open Source Project
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

#include "apex_blocklist.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <google/protobuf/util/json_util.h>
#include <gtest/gtest.h>

#include <algorithm>

using ::apex::proto::ApexBlocklist;

namespace android::apex {

namespace {

std::string ToString(const ApexBlocklist& blocklist) {
  std::string out;
  google::protobuf::util::MessageToJsonString(blocklist, &out);
  return out;
}

}  // namespace

TEST(ApexBlocklistTest, SimpleValid) {
  ApexBlocklist blocklist;
  ApexBlocklist::ApexItem* item = blocklist.add_blocked_apex();
  item->set_name("com.android.example.apex");
  item->set_version(1);
  auto apex_blocklist = ParseBlocklist(ToString(blocklist));
  ASSERT_RESULT_OK(apex_blocklist);
  EXPECT_EQ(1, apex_blocklist->blocked_apex().size());
  EXPECT_EQ("com.android.example.apex", apex_blocklist->blocked_apex(0).name());
  EXPECT_EQ(1, apex_blocklist->blocked_apex(0).version());
}

TEST(ApexBlocklistTest, NameMissing) {
  ApexBlocklist blocklist;
  ApexBlocklist::ApexItem* item = blocklist.add_blocked_apex();
  item->set_version(1);
  auto apex_blocklist = ParseBlocklist(ToString(blocklist));
  ASSERT_FALSE(apex_blocklist.ok());
  EXPECT_EQ(apex_blocklist.error().message(),
            std::string("Missing required field \"name\" from APEX blocklist."))
      << apex_blocklist.error();
}

TEST(ApexBlocklistTest, VersionMissing) {
  ApexBlocklist blocklist;
  ApexBlocklist::ApexItem* item = blocklist.add_blocked_apex();
  item->set_name("com.android.example.apex");
  auto apex_blocklist = ParseBlocklist(ToString(blocklist));
  ASSERT_FALSE(apex_blocklist.ok());
  EXPECT_EQ(
      apex_blocklist.error().message(),
      std::string(
          "Missing positive value for field \"version\" from APEX blocklist."))
      << apex_blocklist.error();
}

}  // namespace android::apex
