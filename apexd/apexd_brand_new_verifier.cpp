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

#include <optional>
#include <string>

#include "android-base/logging.h"
#include "apex_file_repository.h"

using android::base::Error;
using android::base::Result;

namespace android::apex {

Result<void> VerifyBrandNewPackageAgainstPreinstalled(const ApexFile& apex) {
  CHECK(ApexFileRepository::IsBrandNewApexEnabled())
      << "Brand-new APEX must be enabled in order to do verification.";

  const std::string& name = apex.GetManifest().name();
  const auto& file_repository = ApexFileRepository::GetInstance();
  auto partition = file_repository.GetBrandNewApexPublicKeyPartition(
      apex.GetBundledPublicKey());
  if (!partition.has_value()) {
    return Error()
           << "No pre-installed public key found for the brand-new APEX: "
           << name;
  }

  if (apex.GetManifest().version() <=
      file_repository.GetBrandNewApexBlockedVersion(partition.value(), name)) {
    return Error() << "Brand-new APEX is blocked: " << name;
  }

  return {};
}

Result<void> VerifyBrandNewPackageAgainstActive(const ApexFile& apex) {
  CHECK(ApexFileRepository::IsBrandNewApexEnabled())
      << "Brand-new APEX must be enabled in order to do verification.";

  const std::string& name = apex.GetManifest().name();
  const auto& file_repository = ApexFileRepository::GetInstance();

  if (file_repository.HasPreInstalledVersion(name)) {
    return {};
  }

  if (file_repository.HasDataVersion(name)) {
    auto existing_package = file_repository.GetDataApex(name).get();
    if (apex.GetBundledPublicKey() != existing_package.GetBundledPublicKey()) {
      return Error()
             << "Brand-new APEX public key doesn't match existing active APEX: "
             << name;
    }
  }
  return {};
}

}  // namespace android::apex
