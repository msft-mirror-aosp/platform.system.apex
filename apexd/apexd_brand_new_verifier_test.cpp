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

#include "apexd_brand_new_verifier.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/result-gmock.h>
#include <android-base/stringprintf.h>
#include <gtest/gtest.h>
#include <sys/stat.h>

#include <filesystem>
#include <string>

#include "apex_constants.h"
#include "apex_file_repository.h"
#include "apexd_test_utils.h"

namespace android::apex {

namespace fs = std::filesystem;

using android::base::GetExecutableDirectory;
using android::base::testing::Ok;
using android::base::testing::WithMessage;
using ::testing::Not;

static std::string GetTestDataDir() { return GetExecutableDirectory(); }
static std::string GetTestFile(const std::string& name) {
  return GetTestDataDir() + "/" + name;
}

TEST(BrandNewApexVerifierTest, SucceedPublicKeyMatch) {
  ApexFileRepository::EnableBrandNewApex();
  auto& file_repository = ApexFileRepository::GetInstance();
  const auto partition = ApexPartition::System;
  TemporaryDir trusted_key_dir;
  fs::copy(GetTestFile("apexd_testdata/com.android.apex.brand.new.avbpubkey"),
           trusted_key_dir.path);
  file_repository.AddBrandNewApexCredentialAndBlocklist(
      {{partition, trusted_key_dir.path}});

  auto apex = ApexFile::Open(GetTestFile("com.android.apex.brand.new.apex"));
  ASSERT_RESULT_OK(apex);

  auto ret = VerifyBrandNewPackageAgainstPreinstalled(*apex);
  ASSERT_RESULT_OK(ret);
  ASSERT_EQ(*ret, partition);

  file_repository.Reset();
}

TEST(BrandNewApexVerifierTest, SucceedVersionBiggerThanBlocked) {
  ApexFileRepository::EnableBrandNewApex();
  auto& file_repository = ApexFileRepository::GetInstance();
  const auto partition = ApexPartition::System;
  TemporaryDir config_dir;
  fs::copy(GetTestFile("apexd_testdata/com.android.apex.brand.new.avbpubkey"),
           config_dir.path);
  fs::copy(GetTestFile("apexd_testdata/blocklist.json"), config_dir.path);
  file_repository.AddBrandNewApexCredentialAndBlocklist(
      {{partition, config_dir.path}});

  auto apex = ApexFile::Open(GetTestFile("com.android.apex.brand.new.v2.apex"));
  ASSERT_RESULT_OK(apex);

  auto ret = VerifyBrandNewPackageAgainstPreinstalled(*apex);
  ASSERT_RESULT_OK(ret);
  ASSERT_EQ(*ret, partition);

  file_repository.Reset();
}

TEST(BrandNewApexVerifierTest, SucceedMatchActive) {
  ApexFileRepository::EnableBrandNewApex();
  auto& file_repository = ApexFileRepository::GetInstance();
  TemporaryDir trusted_key_dir, data_dir;
  fs::copy(GetTestFile("apexd_testdata/com.android.apex.brand.new.avbpubkey"),
           trusted_key_dir.path);
  fs::copy(GetTestFile("com.android.apex.brand.new.apex"), data_dir.path);
  file_repository.AddBrandNewApexCredentialAndBlocklist(
      {{ApexPartition::System, trusted_key_dir.path}});
  file_repository.AddDataApex(data_dir.path);

  auto apex = ApexFile::Open(GetTestFile("com.android.apex.brand.new.v2.apex"));
  ASSERT_RESULT_OK(apex);

  auto ret = VerifyBrandNewPackageAgainstActive(*apex);
  ASSERT_RESULT_OK(ret);

  file_repository.Reset();
}

TEST(BrandNewApexVerifierTest, SucceedSkipPreinstalled) {
  ApexFileRepository::EnableBrandNewApex();
  auto& file_repository = ApexFileRepository::GetInstance();
  TemporaryDir built_in_dir;
  fs::copy(GetTestFile("apex.apexd_test.apex"), built_in_dir.path);
  file_repository.AddPreInstalledApex(
      {{ApexPartition::System, built_in_dir.path}});

  auto apex = ApexFile::Open(GetTestFile("apex.apexd_test.apex"));
  ASSERT_RESULT_OK(apex);

  auto ret = VerifyBrandNewPackageAgainstActive(*apex);
  ASSERT_RESULT_OK(ret);

  file_repository.Reset();
}

TEST(BrandNewApexVerifierTest, SucceedSkipWithoutDataVersion) {
  ApexFileRepository::EnableBrandNewApex();
  auto& file_repository = ApexFileRepository::GetInstance();

  auto apex = ApexFile::Open(GetTestFile("com.android.apex.brand.new.apex"));
  ASSERT_RESULT_OK(apex);

  auto ret = VerifyBrandNewPackageAgainstActive(*apex);
  ASSERT_RESULT_OK(ret);

  file_repository.Reset();
}

TEST(BrandNewApexVerifierTest, FailBrandNewApexDisabled) {
  auto& file_repository = ApexFileRepository::GetInstance();
  const auto partition = ApexPartition::System;
  TemporaryDir trusted_key_dir;
  fs::copy(GetTestFile("apexd_testdata/com.android.apex.brand.new.avbpubkey"),
           trusted_key_dir.path);
  file_repository.AddBrandNewApexCredentialAndBlocklist(
      {{partition, trusted_key_dir.path}});

  auto apex = ApexFile::Open(GetTestFile("com.android.apex.brand.new.apex"));
  ASSERT_RESULT_OK(apex);

  ASSERT_DEATH(
      { VerifyBrandNewPackageAgainstPreinstalled(*apex); },
      "Brand-new APEX must be enabled in order to do verification.");
  ASSERT_DEATH(
      { VerifyBrandNewPackageAgainstActive(*apex); },
      "Brand-new APEX must be enabled in order to do verification.");

  file_repository.Reset();
}

TEST(BrandNewApexVerifierTest, FailNoMatchingPublicKey) {
  ApexFileRepository::EnableBrandNewApex();

  auto apex = ApexFile::Open(GetTestFile("com.android.apex.brand.new.apex"));
  ASSERT_RESULT_OK(apex);

  auto ret = VerifyBrandNewPackageAgainstPreinstalled(*apex);
  ASSERT_THAT(
      ret,
      HasError(WithMessage(("No pre-installed public key found for the "
                            "brand-new APEX: com.android.apex.brand.new"))));
}

TEST(BrandNewApexVerifierTest, FailBlockedByVersion) {
  ApexFileRepository::EnableBrandNewApex();
  auto& file_repository = ApexFileRepository::GetInstance();
  const auto partition = ApexPartition::System;
  TemporaryDir config_dir;
  fs::copy(GetTestFile("apexd_testdata/com.android.apex.brand.new.avbpubkey"),
           config_dir.path);
  fs::copy(GetTestFile("apexd_testdata/blocklist.json"), config_dir.path);
  file_repository.AddBrandNewApexCredentialAndBlocklist(
      {{partition, config_dir.path}});

  auto apex = ApexFile::Open(GetTestFile("com.android.apex.brand.new.apex"));
  ASSERT_RESULT_OK(apex);

  auto ret = VerifyBrandNewPackageAgainstPreinstalled(*apex);
  ASSERT_THAT(ret,
              HasError(WithMessage(
                  ("Brand-new APEX is blocked: com.android.apex.brand.new"))));

  file_repository.Reset();
}

TEST(BrandNewApexVerifierTest, FailPublicKeyNotMatchActive) {
  ApexFileRepository::EnableBrandNewApex();
  auto& file_repository = ApexFileRepository::GetInstance();
  TemporaryDir trusted_key_dir, data_dir;
  fs::copy(GetTestFile("apexd_testdata/com.android.apex.brand.new.avbpubkey"),
           trusted_key_dir.path);
  fs::copy(GetTestFile(
               "apexd_testdata/com.android.apex.brand.new.another.avbpubkey"),
           trusted_key_dir.path);
  fs::copy(GetTestFile("com.android.apex.brand.new.apex"), data_dir.path);
  file_repository.AddBrandNewApexCredentialAndBlocklist(
      {{ApexPartition::System, trusted_key_dir.path}});
  file_repository.AddDataApex(data_dir.path);

  auto apex =
      ApexFile::Open(GetTestFile("com.android.apex.brand.new.v2.diffkey.apex"));
  ASSERT_RESULT_OK(apex);

  auto ret = VerifyBrandNewPackageAgainstActive(*apex);
  ASSERT_THAT(
      ret,
      HasError(WithMessage(("Brand-new APEX public key doesn't match existing "
                            "active APEX: com.android.apex.brand.new"))));

  file_repository.Reset();
}

}  // namespace android::apex
