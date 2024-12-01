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

#include <map>
#include <memory>
#include <span>
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
  struct ApexFileInfo {
    std::string name;
    int64_t version;
    bool shared_libs;
    int64_t file_size;
    std::string file_hash;
    ApexPartition partition;
    std::vector<std::string> hals;
  };

  virtual ~Metrics() = default;
  virtual void SendInstallationRequested(InstallType install_type,
                                         bool is_rollback,
                                         const ApexFileInfo& info) = 0;
  virtual void SendInstallationEnded(const std::string& file_hash,
                                     InstallResult result) = 0;
};

std::unique_ptr<Metrics> InitMetrics(std::unique_ptr<Metrics> metrics);

void SendSessionApexInstallationEndedAtom(const ApexSession& session,
                                          InstallResult install_result);

// Helper class to send "installation_requested" event. Events are
// sent in its destructor using Metrics::Send* methods.
class InstallRequestedEvent {
 public:
  InstallRequestedEvent(InstallType install_type, bool is_rollback)
      : install_type_(install_type), is_rollback_(is_rollback) {}
  // Sends the "requested" event.
  // Sends the "end" event if it's non-staged or failed.
  ~InstallRequestedEvent();

  void AddFiles(std::span<const ApexFile> files);

  // Adds HAL Information for each APEX.
  // Since the event can contain multiple APEX files, HAL information is
  // passed as a map of APEX name to a list of HAL names.
  void AddHals(const std::map<std::string, std::vector<std::string>>& hals);

  // Marks the current installation request has succeeded.
  void MarkSucceeded();

  // Returns file hashes for APEX files added by AddFile()
  std::vector<std::string> GetFileHashes() const;

 private:
  InstallType install_type_;
  bool is_rollback_;
  std::vector<Metrics::ApexFileInfo> files_;
  bool succeeded_ = false;
};

}  // namespace android::apex
