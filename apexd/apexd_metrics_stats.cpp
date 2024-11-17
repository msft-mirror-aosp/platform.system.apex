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

#include "apexd_metrics_stats.h"

#include <android-base/logging.h>
#include <unistd.h>

#include "apex_constants.h"
#include "apexd_metrics.h"
#include "statslog_apex.h"

namespace android::apex {

namespace {

int Cast(InstallType install_type) {
  switch (install_type) {
    case InstallType::Staged:
      return stats::apex::
          APEX_INSTALLATION_REQUESTED__INSTALLATION_TYPE__STAGED;
    case InstallType::NonStaged:
      return stats::apex::
          APEX_INSTALLATION_REQUESTED__INSTALLATION_TYPE__REBOOTLESS;
  }
}

int Cast(InstallResult install_result) {
  switch (install_result) {
    case InstallResult::Success:
      return stats::apex::
          APEX_INSTALLATION_ENDED__INSTALLATION_RESULT__INSTALL_SUCCESSFUL;
    case InstallResult::Failure:
      return stats::apex::
          APEX_INSTALLATION_ENDED__INSTALLATION_RESULT__INSTALL_FAILURE_APEX_INSTALLATION;
  }
}

int Cast(ApexPartition partition) {
  switch (partition) {
    case ApexPartition::System:
      return stats::apex::
          APEX_INSTALLATION_REQUESTED__APEX_PREINSTALL_PARTITION__PARTITION_SYSTEM;
    case ApexPartition::SystemExt:
      return stats::apex::
          APEX_INSTALLATION_REQUESTED__APEX_PREINSTALL_PARTITION__PARTITION_SYSTEM_EXT;
    case ApexPartition::Product:
      return stats::apex::
          APEX_INSTALLATION_REQUESTED__APEX_PREINSTALL_PARTITION__PARTITION_PRODUCT;
    case ApexPartition::Vendor:
      return stats::apex::
          APEX_INSTALLATION_REQUESTED__APEX_PREINSTALL_PARTITION__PARTITION_VENDOR;
    case ApexPartition::Odm:
      return stats::apex::
          APEX_INSTALLATION_REQUESTED__APEX_PREINSTALL_PARTITION__PARTITION_ODM;
    default:
      return stats::apex::
          APEX_INSTALLATION_REQUESTED__APEX_PREINSTALL_PARTITION__PARTITION_UNKNOWN;
  }
}

}  // namespace

void StatsLog::InstallationRequested(
    const std::string& module_name, int64_t version_code,
    int64_t file_size_bytes, const std::string& file_hash,
    ApexPartition partition, InstallType install_type, bool is_rollback,
    bool shared_libs, const std::vector<std::string>& hals) {
  if (!IsAvailable()) {
    LOG(WARNING) << "Unable to send atom: libstatssocket is not available";
    return;
  }
  std::vector<const char*> hals_cstr;
  for (const auto& hal : hals) {
    hals_cstr.push_back(hal.c_str());
  }
  int ret = stats::apex::stats_write(
      stats::apex::APEX_INSTALLATION_REQUESTED, module_name.c_str(),
      version_code, file_size_bytes, file_hash.c_str(), Cast(partition),
      Cast(install_type), is_rollback, shared_libs, hals_cstr);
  if (ret < 0) {
    LOG(WARNING) << "Failed to report apex_installation_requested stats";
  }
}

void StatsLog::InstallationEnded(const std::string& file_hash,
                                 InstallResult result) {
  if (!IsAvailable()) {
    LOG(WARNING) << "Unable to send atom: libstatssocket is not available";
    return;
  }
  int ret = stats::apex::stats_write(stats::apex::APEX_INSTALLATION_ENDED,
                                     file_hash.c_str(), Cast(result));
  if (ret < 0) {
    LOG(WARNING) << "Failed to report apex_installation_ended stats";
  }
}

bool StatsLog::IsAvailable() {
  return access("/apex/com.android.os.statsd", F_OK) == 0;
}

}  // namespace android::apex
