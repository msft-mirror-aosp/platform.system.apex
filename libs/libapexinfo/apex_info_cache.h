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
#pragma once

#include "apex_info.h"

#include <string>
#include <sys/stat.h>

#include <android-base/result.h>

namespace android {
namespace apex {
namespace info {

//
// Cache current apex information
//
class ApexInfoCache {
public:
  ApexInfoCache(const std::string &info_file = kApexInfoFile);

  // Check if new data is available
  android::base::Result<bool> HasNewData() const;

  // Update ApexInfoData data held in the object, if change in
  // timestamp is detected.  Return true if data has been updated
  android::base::Result<bool> Update();

  // Get copy of stored information
  const ApexInfoData &Info() const { return apex_info_; }

  // Allow test function to override apex_ready
  void SetApexReady() { apex_ready_ = true; }

protected:
  // Get the modify time, return true if update detected
  android::base::Result<std::pair<struct timespec, bool>> ModifyTime() const;

protected:
  std::string dir_;
  std::string info_file_;
  struct timespec mtim_; // last modified time of the file
  ApexInfoData apex_info_;
  mutable bool apex_ready_;
};

} // namespace info
} // namespace apex
} // namespace android
