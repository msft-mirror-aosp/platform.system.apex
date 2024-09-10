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
 *
 * This file contains the vendor-apex-specific functions of apexd
 */

#include "apexd_vendor_apex.h"

#include <android-base/strings.h>
#include <vintf/VintfObject.h>

#include "apex_file_repository.h"
#include "apexd_private.h"
#include "statslog_apex.h"

using android::base::Error;
using android::base::StartsWith;

namespace android {
namespace apex {

bool InVendorPartition(const std::string& path) {
  return StartsWith(path, "/vendor/apex/") ||
         StartsWith(path, "/system/vendor/apex/");
}

bool InOdmPartition(const std::string& path) {
  return StartsWith(path, "/odm/apex/") ||
         StartsWith(path, "/vendor/odm/apex/") ||
         StartsWith(path, "/system/vendor/odm/apex/");
}

// Returns if apex is a vendor apex, works by testing path of its preinstalled
// version.
bool IsVendorApex(const ApexFile& apex_file) {
  const auto& instance = ApexFileRepository::GetInstance();
  const auto& preinstalled =
      instance.GetPreInstalledApex(apex_file.GetManifest().name());
  const auto& path = preinstalled.get().GetPath();
  return InVendorPartition(path) || InOdmPartition(path);
}

// Checks Compatibility for incoming vendor apex.
//    Adds the data from apex's vintf_fragment(s) and tests compatibility.
base::Result<void> CheckVendorApexUpdate(const ApexFile& apex_file,
                                         const std::string& apex_mount_point) {
  std::string error;

  const std::string apex_name = apex_file.GetManifest().name();

  std::string path_to_replace =
      apexd_private::GetActiveMountPoint(apex_file.GetManifest());

  // Create PathReplacingFileSystem instance containing caller's path
  // substitution
  std::unique_ptr<vintf::FileSystem> path_replaced_fs =
      std::make_unique<vintf::details::PathReplacingFileSystem>(
          std::move(path_to_replace), apex_mount_point,
          std::make_unique<vintf::details::FileSystemImpl>());

  // Create a new VintfObject that uses our path-replacing FileSystem instance
  auto vintf_with_replaced_path =
      vintf::VintfObject::Builder()
          .setFileSystem(std::move(path_replaced_fs))
          .build();

  // Disable RuntimeInfo components. Allows callers to run check
  // without requiring read permission of restricted resources
  auto flags = vintf::CheckFlags::DEFAULT;
  flags = flags.disableRuntimeInfo();

  // checkCompatibility on vintfObj using the replacement vintf directory
  int ret = vintf_with_replaced_path->checkCompatibility(&error, flags);
  LOG(DEBUG) << "CheckVendorApexUpdate: check on vendor apex " << apex_name
             << " returned " << ret << " (want " << vintf::COMPATIBLE
             << " == COMPATIBLE)";
  if (ret == vintf::INCOMPATIBLE) {
    return Error() << "vendor apex is not compatible, error=" << error;
  } else if (ret != vintf::COMPATIBLE) {
    return Error() << "Check of vendor apex failed, error=" << error;
  }

  return {};
}

// GetPreinstallPartitionEnum returns the enumeration value of the preinstall-
//    partition of the passed apex_file
int GetPreinstallPartitionEnum(const ApexFile& apex_file) {
  const auto& instance = ApexFileRepository::GetInstance();
  // We must test if this apex has a pre-installed version before calling
  // GetPreInstalledApex() - throws an exception if apex doesn't have one
  if (!instance.IsPreInstalledApex(apex_file)) {
    return stats::apex::
        APEX_INSTALLATION_REQUESTED__APEX_PREINSTALL_PARTITION__PARTITION_OTHER;
  }
  const auto& preinstalled =
      instance.GetPreInstalledApex(apex_file.GetManifest().name());
  const auto& preinstalled_path = preinstalled.get().GetPath();
  if (InVendorPartition(preinstalled_path)) {
    return stats::apex::
        APEX_INSTALLATION_REQUESTED__APEX_PREINSTALL_PARTITION__PARTITION_VENDOR;
  }
  if (InOdmPartition(preinstalled_path)) {
    return stats::apex::
        APEX_INSTALLATION_REQUESTED__APEX_PREINSTALL_PARTITION__PARTITION_ODM;
  }
  if (StartsWith(preinstalled_path, "/system_ext/apex/")) {
    return stats::apex::
        APEX_INSTALLATION_REQUESTED__APEX_PREINSTALL_PARTITION__PARTITION_SYSTEM_EXT;
  }
  if (StartsWith(preinstalled_path, "/system/apex/")) {
    return stats::apex::
        APEX_INSTALLATION_REQUESTED__APEX_PREINSTALL_PARTITION__PARTITION_SYSTEM;
  }
  if (StartsWith(preinstalled_path, "/product/apex/")) {
    return stats::apex::
        APEX_INSTALLATION_REQUESTED__APEX_PREINSTALL_PARTITION__PARTITION_PRODUCT;
  }
  return stats::apex::
      APEX_INSTALLATION_REQUESTED__APEX_PREINSTALL_PARTITION__PARTITION_OTHER;
}

}  // namespace apex
}  // namespace android
