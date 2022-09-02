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

#include "apexutil.h"

#include <list>
#include <string>

#include <android-base/result.h>

namespace android {
namespace apex {
namespace util {

// APEX types
enum class ApexType {
  kSystem,
  kProduct,
  kVendor,
  kOdm,
};

// ApexInfo -- APEX active information
class ApexInfo {
public:
  ApexInfo(const std::string &manifest_name, const std::string &path,
           ApexType type);

  // Manifest name from apex-info
  const std::string &ManifestName() const { return manifest_name_; }
  // Active mount path
  const std::string &Path() const { return path_; }

  ApexType Type() const { return type_; }

private:
  std::string manifest_name_;
  std::string path_;
  ApexType type_;
};

// Get list of active APEXs
using ApexInfoData = std::vector<ApexInfo>;
android::base::Result<ApexInfoData>
GetApexes(const std::string &apex_root = kApexRoot,
          const std::string &info_file_list = kApexInfoFile);

} // namespace util
} // namespace apex
} // namespace android
