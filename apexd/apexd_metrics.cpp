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

#include "apexd_metrics.h"

#include <android-base/logging.h>
#include <android-base/result.h>
#include <android-base/strings.h>
#include <sys/stat.h>

#include <utility>

#include "apex_constants.h"
#include "apex_file.h"
#include "apex_file_repository.h"
#include "apex_sha.h"
#include "apexd_session.h"
#include "apexd_vendor_apex.h"

using android::base::Result;
using android::base::StartsWith;

namespace android::apex {

namespace {

std::unique_ptr<Metrics> gMetrics;

}  // namespace

std::unique_ptr<Metrics> InitMetrics(std::unique_ptr<Metrics> metrics) {
  std::swap(gMetrics, metrics);
  return metrics;
}

void SendApexInstallationRequestedAtom(const std::string& package_path,
                                       bool is_rollback,
                                       InstallType install_type) {
  if (!gMetrics) {
    return;
  }
  auto apex_file = ApexFile::Open(package_path);
  if (!apex_file.ok()) {
    LOG(WARNING) << "Unable to send Apex Atom; Failed to open ApexFile "
                 << package_path << ": " << apex_file.error();
    return;
  }
  const std::string& module_name = apex_file->GetManifest().name();
  struct stat stat_buf;
  intmax_t apex_file_size;
  if (stat(package_path.c_str(), &stat_buf) == 0) {
    apex_file_size = stat_buf.st_size;
  } else {
    PLOG(WARNING) << "Failed to stat " << package_path;
    return;
  }
  Result<std::string> hash = CalculateSha256(package_path);
  if (!hash.ok()) {
    LOG(WARNING) << "Unable to get sha256 of ApexFile: " << hash.error();
    return;
  }

  const auto& instance = ApexFileRepository::GetInstance();
  auto partition = instance.GetPartition(*apex_file);
  if (!partition.ok()) {
    LOG(WARNING) << partition.error();
    return;
  }

  std::vector<std::string> hal_list;
  // TODO(b/366217822): Populate HAL information

  gMetrics->InstallationRequested(
      module_name, apex_file->GetManifest().version(), apex_file_size, *hash,
      *partition, install_type, is_rollback,
      apex_file->GetManifest().providesharedapexlibs(), hal_list);
}

void SendApexInstallationEndedAtom(const std::string& package_path,
                                   InstallResult install_result) {
  if (!gMetrics) {
    return;
  }
  Result<std::string> hash = CalculateSha256(package_path);
  if (!hash.ok()) {
    LOG(WARNING) << "Unable to get sha256 of ApexFile: " << hash.error();
    return;
  }
  gMetrics->InstallationEnded(*hash, install_result);
}

void SendSessionApexInstallationEndedAtom(const ApexSession& session,
                                          InstallResult install_result) {
  if (!gMetrics) {
    return;
  }

  for (const auto& hash : session.GetApexFileHashes()) {
    gMetrics->InstallationEnded(hash, install_result);
  }
}

}  // namespace android::apex
