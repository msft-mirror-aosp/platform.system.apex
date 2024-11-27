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

#include <string>

#include "apex_constants.h"
#include "apexd_metrics.h"

namespace android::apex {

class StatsLog : public Metrics {
 public:
  StatsLog() = default;
  ~StatsLog() override = default;

  void SendInstallationRequested(InstallType install_type, bool is_rollback,
                                 const ApexFileInfo& info) override;
  void SendInstallationEnded(const std::string& file_hash,
                             InstallResult result) override;

 private:
  bool IsAvailable();
};

}  // namespace android::apex
