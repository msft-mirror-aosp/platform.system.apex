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

#include <statssocket_lazy.h>
#include <sys/stat.h>

#include "apex_sha.h"
#include "apexd.h"
#include "apexd_vendor_apex.h"
#include "statslog_apex.h"

using android::base::Result;

namespace android::apex {

// Ties sessions to their apex file, assists reporting installation metrics
std::unordered_map<int, std::vector<std::string>> gSessionApexSha;

void SendApexInstallationRequestedAtom(const std::string& package_path,
                                       bool is_rollback,
                                       unsigned int install_type) {
  if (!statssocket::lazy::IsAvailable()) {
    LOG(WARNING) << "Unable to send Apex Install Atom for " << package_path
                 << " ; libstatssocket is not available";
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
    apex_file_size = 0;
  }
  Result<std::string> apex_file_sha256_str = CalculateSha256(package_path);
  if (!apex_file_sha256_str.ok()) {
    LOG(WARNING) << "Unable to get sha256 of ApexFile: "
                 << apex_file_sha256_str.error();
    return;
  }
  const std::vector<const char*>
      hal_cstr_list;  // TODO(b/366217822): Populate HAL information
  int ret = stats::apex::stats_write(
      stats::apex::APEX_INSTALLATION_REQUESTED, module_name.c_str(),
      apex_file->GetManifest().version(), apex_file_size,
      apex_file_sha256_str->c_str(), GetPreinstallPartitionEnum(*apex_file),
      install_type, is_rollback,
      apex_file->GetManifest().providesharedapexlibs(), hal_cstr_list);
  if (ret < 0) {
    LOG(WARNING) << "Failed to report apex_installation_requested stats";
  }
}

void SendApexInstallationStagedAtom(const std::string& package_path) {
  if (!statssocket::lazy::IsAvailable()) {
    LOG(WARNING) << "Unable to send Apex Staged Atom for " << package_path
                 << " ; libstatssocket is not available";
    return;
  }
  Result<std::string> apex_file_sha256_str = CalculateSha256(package_path);
  if (!apex_file_sha256_str.ok()) {
    LOG(WARNING) << "Unable to get sha256 of ApexFile: "
                 << apex_file_sha256_str.error();
    return;
  }
  int ret = stats::apex::stats_write(stats::apex::APEX_INSTALLATION_STAGED,
                                     apex_file_sha256_str->c_str());
  if (ret < 0) {
    LOG(WARNING) << "Failed to report apex_installation_staged stats";
  }
}

void SendApexInstallationEndedAtom(const std::string& package_path,
                                   int install_result) {
  if (!statssocket::lazy::IsAvailable()) {
    LOG(WARNING) << "Unable to send Apex Ended Atom for " << package_path
                 << " ; libstatssocket is not available";
    return;
  }
  Result<std::string> apex_file_sha256_str = CalculateSha256(package_path);
  if (!apex_file_sha256_str.ok()) {
    LOG(WARNING) << "Unable to get sha256 of ApexFile: "
                 << apex_file_sha256_str.error();
    return;
  }
  int ret =
      stats::apex::stats_write(stats::apex::APEX_INSTALLATION_ENDED,
                               apex_file_sha256_str->c_str(), install_result);
  if (ret < 0) {
    LOG(WARNING) << "Failed to report apex_installation_ended stats";
  }
}

void SendSessionApexInstallationEndedAtom(int session_id, int install_result) {
  if (!statssocket::lazy::IsAvailable()) {
    LOG(WARNING) << "Unable to send Apex Ended Atom for session " << session_id
                 << " ; libstatssocket is not available";
    return;
  }
  if (gSessionApexSha.find(session_id) == gSessionApexSha.end()) {
    LOG(WARNING) << "Unable to send Apex Ended Atom for session " << session_id
                 << " ; apex_sha for session was not found";
    return;
  }
  for (const auto& apex_sha : gSessionApexSha[session_id]) {
    int ret = stats::apex::stats_write(stats::apex::APEX_INSTALLATION_ENDED,
                                       apex_sha.c_str(), install_result);
    if (ret < 0) {
      LOG(WARNING) << "Failed to report apex_installation_ended stats";
    }
  }
}

void SendApexInstallationStagedAtoms(
    const std::vector<std::string>& package_paths) {
  for (const std::string& path : package_paths) {
    SendApexInstallationStagedAtom(path);
  }
}

void SendApexInstallationEndedAtoms(
    const std::vector<std::string>& package_paths, int install_result) {
  for (const std::string& path : package_paths) {
    SendApexInstallationEndedAtom(path, install_result);
  }
}

void RegisterSessionApexSha(int session_id, const std::string apex_file_sha) {
  gSessionApexSha[session_id].push_back(apex_file_sha);
}

}  // namespace android::apex
