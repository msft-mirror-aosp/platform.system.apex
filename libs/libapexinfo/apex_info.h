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

#include <list>
#include <string>

#include <android-base/result.h>

namespace android {
namespace apex {
namespace info {

// Exposing these in a nested namespace for testing
namespace details {
// These partitions can be symlinked to subdirs of /system.
extern std::string kOdmRealPath;
extern std::string kProductRealPath;
extern std::string kSystemExtRealPath;
extern std::string kVendorRealPath;
}  // namespace details

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
  ApexInfo(const std::string &manifest_name, ApexType type);

  // Manifest name from apex-info
  const std::string &ManifestName() const { return manifest_name_; }
  // Active mount path
  std::string Path() const;

  ApexType Type() const { return type_; }

private:
  std::string manifest_name_;
  ApexType type_;
};

constexpr const char *const kApexInfoFileName = "apex-info-list.xml";
constexpr const char *const kApexInfoFile = "/apex/apex-info-list.xml";

// Get list of active APEXs
using ApexInfoData = std::vector<ApexInfo>;
android::base::Result<ApexInfoData>
GetApexes(const std::string &info_file_list = kApexInfoFile);

} // namespace info
} // namespace apex
} // namespace android
