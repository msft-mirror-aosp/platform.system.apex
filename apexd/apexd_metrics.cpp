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
  gMetrics->SendInstallationEnded(*hash, install_result);
}

void SendSessionApexInstallationEndedAtom(const ApexSession& session,
                                          InstallResult install_result) {
  if (!gMetrics) {
    return;
  }

  for (const auto& hash : session.GetApexFileHashes()) {
    gMetrics->SendInstallationEnded(hash, install_result);
  }
}

InstallRequestedEvent::~InstallRequestedEvent() {
  if (!gMetrics) {
    return;
  }
  for (const auto& info : files_) {
    gMetrics->SendInstallationRequested(install_type_, is_rollback_, info);
  }
  // Staged installation ends later. No need to send "end" event now.
  if (succeeded_ && install_type_ == InstallType::Staged) {
    return;
  }
  auto result = succeeded_ ? InstallResult::Success : InstallResult::Failure;
  for (const auto& info : files_) {
    gMetrics->SendInstallationEnded(info.file_hash, result);
  }
}

void InstallRequestedEvent::MarkSucceeded() { succeeded_ = true; }

void InstallRequestedEvent::AddFiles(std::span<const ApexFile> files) {
  auto& repo = ApexFileRepository::GetInstance();
  files_.reserve(files.size());
  for (const auto& file : files) {
    Metrics::ApexFileInfo info;
    info.name = file.GetManifest().name();
    info.version = file.GetManifest().version();
    info.shared_libs = file.GetManifest().providesharedapexlibs();

    const auto& file_path = file.GetPath();
    struct stat stat_buf;
    if (stat(file_path.c_str(), &stat_buf) == 0) {
      info.file_size = stat_buf.st_size;
    } else {
      PLOG(WARNING) << "Failed to stat " << file_path;
      continue;
    }

    if (auto result = CalculateSha256(file_path); result.ok()) {
      info.file_hash = result.value();
    } else {
      LOG(WARNING) << "Unable to get sha256 of " << file_path << ": "
                   << result.error();
      continue;
    }

    if (auto result = repo.GetPartition(file); result.ok()) {
      info.partition = result.value();
    } else {
      LOG(WARNING) << "Failed to get partition of " << file_path << ": "
                   << result.error();
      continue;
    }

    files_.push_back(std::move(info));
  }
}

void InstallRequestedEvent::AddHals(
    const std::map<std::string, std::vector<std::string>>& hals) {
  for (auto& info : files_) {
    if (auto it = hals.find(info.name); it != hals.end()) {
      info.hals = it->second;
    }
  }
}

std::vector<std::string> InstallRequestedEvent::GetFileHashes() const {
  std::vector<std::string> hashes;
  hashes.reserve(files_.size());
  for (const auto& info : files_) {
    hashes.push_back(info.file_hash);
  }
  return hashes;
}

}  // namespace android::apex
