/*
 * Copyright (C) 2023 The Android Open Source Project
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

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <android/apexsupport.h>

// TODO(b/288341340) Enable tests when we can compare __ANDROID_API__ against
// __ANDROID_API_V__.
#if 0

TEST(libapexsupport, AApexInfo_with_no_error_code) {
  EXPECT_EQ(AApexInfo_create(nullptr), AAPEXINFO_NULL);
}

TEST(libapexsupport, AApexInfo) {
  AApexInfo *info;
  EXPECT_EQ(AApexInfo_create(&info), AAPEXINFO_OK);
  ASSERT_NE(info, nullptr);

  // Name/version should match with the values in manifest.json
  auto name = AApexInfo_getName(info);
  EXPECT_STREQ("com.android.libapexsupport.tests", name);
  EXPECT_EQ(42, AApexInfo_getVersion(info));

  AApexInfo_destroy(info);
}

#endif // 0

// TODO(b/271488212) Add tests for error cases

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
