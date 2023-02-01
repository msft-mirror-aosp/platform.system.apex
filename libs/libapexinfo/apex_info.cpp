/*
 * Copyright (C) 2022 The Android Open Source Project
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

#include "apex_info.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/strings.h>

#include <cstdlib>

#include "com_android_apex.h"

namespace android {
namespace apex {
namespace info {

using android::base::Error;
using android::base::Result;
using android::base::StartsWith;
using namespace std::literals;

namespace {

using namespace details;

std::string GetRealPath(const char *path) {
  std::string resolved;
  if (android::base::Realpath(path, &resolved)) {
    return resolved;
  }
  // fallback to the original path
  return path;
}

bool InSystem(const std::string &original_path) {
  return StartsWith(original_path, "/system/apex/") ||
         StartsWith(original_path, kSystemExtRealPath + "/apex/") ||
         // Guest mode Android may have system APEXes from host via block APEXes
         StartsWith(original_path, "/dev/block/vd");
}

bool InProduct(const std::string &original_path) {
  return StartsWith(original_path, kProductRealPath + "/apex/");
}

bool InVendor(const std::string &original_path) {
  return StartsWith(original_path, kVendorRealPath + "/apex/");
}

bool InOdm(const std::string &original_path) {
  return StartsWith(original_path, kOdmRealPath + "/apex/");
}

Result<ApexType> GetType(const std::string &original_path) {
  if (InSystem(original_path)) {
    return ApexType::kSystem;
  } else if (InProduct(original_path)) {
    return ApexType::kProduct;
  } else if (InVendor(original_path)) {
    return ApexType::kVendor;
  } else if (InOdm(original_path)) {
    return ApexType::kOdm;
  }

  return Error() << "Unknown type based on path " << original_path;
}

} // namespace

namespace details {
std::string kOdmRealPath = GetRealPath("/odm");
std::string kProductRealPath = GetRealPath("/product");
std::string kSystemExtRealPath = GetRealPath("/system_ext");
std::string kVendorRealPath = GetRealPath("/vendor");
}  // namespace details

ApexInfo::ApexInfo(const std::string &manifest_name, ApexType type)
    : manifest_name_(manifest_name), type_(type) {}

std::string ApexInfo::Path() const {
  // To avoid the overhead of parsing the apex data via GetActivePackages
  // we will form the /apex path directly here.
  return (std::string("/apex/").append(manifest_name_));
}
Result<ApexInfoData> GetApexes(const std::string &info_list_file) {

  ApexInfoData info;

  auto info_list =
      ::com::android::apex::readApexInfoList(info_list_file.c_str());
  if (!info_list.has_value()) {
    return Error() << "Failed to read apex info list " << info_list_file;
  }
  for (const auto &apex_info : info_list->getApexInfo()) {

    // Only include active apexes
    if (apex_info.hasIsActive() && apex_info.getIsActive()) {

      // Get the pre-installed path of the apex. Normally (i.e. in Android),
      // failing to find the pre-installed path is an assertion failure
      // because apexd demands that every apex to have a pre-installed one.
      // However, when this runs in a VM where apexes are seen as virtio block
      // devices, the situation is different. If the APEX in the host side is
      // an updated (or staged) one, the block device representing the APEX on
      // the VM side doesn't have the pre-installed path because the factory
      // version of the APEX wasn't exported to the VM. Therefore, we use the
      // module path as original_path when we are running in a VM which can be
      // guessed by checking if the path is /dev/block/vdN.
      std::string path;
      if (apex_info.hasPreinstalledModulePath()) {
        path = apex_info.getPreinstalledModulePath();
      } else if (StartsWith(apex_info.getModulePath(), "/dev/block/vd")) {
        path = apex_info.getModulePath();
      } else {
        return Error() << "Failed to determine original path for apex "
                       << apex_info.getModuleName() << " at " << info_list_file;
      }
      auto type = GetType(path);
      if (!type.ok()) {
        return type.error();
      }
      info.emplace_back(ApexInfo(apex_info.getModuleName(), *type));
    }
  }
  return info;
}

} // namespace info
} // namespace apex
} // namespace android
