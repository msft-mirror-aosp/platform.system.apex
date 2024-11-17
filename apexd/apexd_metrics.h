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

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "apex_constants.h"

namespace android::apex {

class ApexFile;
class ApexSession;

enum class InstallType {
  Staged,
  NonStaged,
};

enum class InstallResult {
  Success,
  Failure,
};

class Metrics {
 public:
  virtual ~Metrics() = default;
  virtual void InstallationRequested(
      const std::string& module_name, int64_t version_code,
      int64_t file_size_bytes, const std::string& file_hash,
      ApexPartition partition, InstallType install_type, bool is_rollback,
      bool shared_libs, const std::vector<std::string>& hals) = 0;
  virtual void InstallationEnded(const std::string& file_hash,
                                 InstallResult result) = 0;
};

std::unique_ptr<Metrics> InitMetrics(std::unique_ptr<Metrics> metrics);

void SendApexInstallationRequestedAtom(const std::string& package_path,
                                       bool is_rollback,
                                       InstallType install_type);

void SendApexInstallationEndedAtom(const std::string& package_path,
                                   InstallResult install_result);

void SendSessionApexInstallationEndedAtom(const ApexSession& session,
                                          InstallResult install_result);

}  // namespace android::apex
