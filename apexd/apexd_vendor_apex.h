/*
 * Copyright (C) 2023 The Android Open Source Project
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

#include <android-base/result.h>

#include <map>
#include <span>
#include <string>
#include <vector>

#include "apex_file.h"

namespace android::apex {

// Checks VINTF for incoming apex updates.
// Returns a map with APEX name and its HAL list.
base::Result<std::map<std::string, std::vector<std::string>>> CheckVintf(
    std::span<const ApexFile> apex_files,
    std::span<const std::string> mount_points);

}  // namespace android::apex
