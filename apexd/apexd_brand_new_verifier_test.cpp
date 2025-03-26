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

using android::base::testing::Ok;
using android::base::testing::WithMessage;
using ::testing::Not;

class BrandNewApexVerifierTest : public ::testing::Test {
 protected:
  void SetUp() override { ApexFileRepository::EnableBrandNewApex(); }
  void TearDown() override { ApexFileRepository::GetInstance().Reset(); }

  // Copy test file to the data dir and populate db with fake mount info
  void PrepareDataApex(const std::string& test_file) {
    fs::copy(GetTestFile(test_file), data_dir.path);
    auto data_apex_path = std::string(data_dir.path) + "/" + test_file;
    auto apex_file = ApexFile::Open(data_apex_path);
    ASSERT_THAT(apex_file, Ok());
    MountedApexDatabase::MountedApexData data;
    data.version = apex_file->GetManifest().version();
    data.full_path = data_apex_path;
    db.AddMountedApex(apex_file->GetManifest().name(), data);
  }

  TemporaryDir trusted_key_dir;
  TemporaryDir config_dir;
  TemporaryDir data_dir;
  TemporaryDir built_in_dir;
  MountedApexDatabase db;
};

TEST_F(BrandNewApexVerifierTest, SucceedPublicKeyMatch) {
  auto& file_repository = ApexFileRepository::GetInstance();
  const auto partition = ApexPartition::System;
  fs::copy(GetTestFile("apexd_testdata/com.android.apex.brand.new.avbpubkey"),
           trusted_key_dir.path);
  file_repository.AddBrandNewApexCredentialAndBlocklist(
      {{partition, trusted_key_dir.path}});

  auto apex = ApexFile::Open(GetTestFile("com.android.apex.brand.new.apex"));
  ASSERT_RESULT_OK(apex);

  auto ret = VerifyBrandNewPackageAgainstPreinstalled(*apex);
  ASSERT_RESULT_OK(ret);
  ASSERT_EQ(*ret, partition);
}

TEST_F(BrandNewApexVerifierTest, SucceedVersionBiggerThanBlocked) {
  auto& file_repository = ApexFileRepository::GetInstance();
  const auto partition = ApexPartition::System;
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
}

TEST_F(BrandNewApexVerifierTest, SucceedMatchActive) {
  auto& file_repository = ApexFileRepository::GetInstance();
  fs::copy(GetTestFile("apexd_testdata/com.android.apex.brand.new.avbpubkey"),
           trusted_key_dir.path);
  file_repository.AddBrandNewApexCredentialAndBlocklist(
      {{ApexPartition::System, trusted_key_dir.path}});
  PrepareDataApex("com.android.apex.brand.new.apex");

  auto apex = ApexFile::Open(GetTestFile("com.android.apex.brand.new.v2.apex"));
  ASSERT_RESULT_OK(apex);

  auto ret = VerifyBrandNewPackageAgainstActive(*apex, db);
  ASSERT_RESULT_OK(ret);
}

TEST_F(BrandNewApexVerifierTest, SucceedSkipPreinstalled) {
  auto& file_repository = ApexFileRepository::GetInstance();
  fs::copy(GetTestFile("apex.apexd_test.apex"), built_in_dir.path);
  file_repository.AddPreInstalledApex(
      {{ApexPartition::System, built_in_dir.path}});

  auto apex = ApexFile::Open(GetTestFile("apex.apexd_test.apex"));
  ASSERT_RESULT_OK(apex);

  auto ret = VerifyBrandNewPackageAgainstActive(*apex, db);
  ASSERT_RESULT_OK(ret);
}

TEST_F(BrandNewApexVerifierTest, SucceedSkipWithoutDataVersion) {
  auto& file_repository = ApexFileRepository::GetInstance();
  auto apex = ApexFile::Open(GetTestFile("com.android.apex.brand.new.apex"));
  ASSERT_RESULT_OK(apex);

  auto ret = VerifyBrandNewPackageAgainstActive(*apex, db);
  ASSERT_RESULT_OK(ret);
}

TEST_F(BrandNewApexVerifierTest, FailBrandNewApexDisabled) {
  auto& file_repository = ApexFileRepository::GetInstance();
  file_repository.Reset();  // Disable brand-new-apex
  const auto partition = ApexPartition::System;
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
      { VerifyBrandNewPackageAgainstActive(*apex, db); },
      "Brand-new APEX must be enabled in order to do verification.");
}

TEST_F(BrandNewApexVerifierTest, FailNoMatchingPublicKey) {
  auto apex = ApexFile::Open(GetTestFile("com.android.apex.brand.new.apex"));
  ASSERT_RESULT_OK(apex);

  auto ret = VerifyBrandNewPackageAgainstPreinstalled(*apex);
  ASSERT_THAT(
      ret,
      HasError(WithMessage(("No pre-installed public key found for the "
                            "brand-new APEX: com.android.apex.brand.new"))));
}

TEST_F(BrandNewApexVerifierTest, FailBlockedByVersion) {
  auto& file_repository = ApexFileRepository::GetInstance();
  const auto partition = ApexPartition::System;
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
}

TEST_F(BrandNewApexVerifierTest, FailPublicKeyNotMatchActive) {
  auto& file_repository = ApexFileRepository::GetInstance();
  fs::copy(GetTestFile("apexd_testdata/com.android.apex.brand.new.avbpubkey"),
           trusted_key_dir.path);
  fs::copy(GetTestFile(
               "apexd_testdata/com.android.apex.brand.new.another.avbpubkey"),
           trusted_key_dir.path);
  file_repository.AddBrandNewApexCredentialAndBlocklist(
      {{ApexPartition::System, trusted_key_dir.path}});
  PrepareDataApex("com.android.apex.brand.new.apex");

  auto apex =
      ApexFile::Open(GetTestFile("com.android.apex.brand.new.v2.diffkey.apex"));
  ASSERT_RESULT_OK(apex);

  auto ret = VerifyBrandNewPackageAgainstActive(*apex, db);
  ASSERT_THAT(
      ret,
      HasError(WithMessage(("Brand-new APEX public key doesn't match existing "
                            "active APEX: com.android.apex.brand.new"))));
}

}  // namespace android::apex
