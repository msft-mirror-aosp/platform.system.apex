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

#include <android-base/result.h>

#include <string>

#include "apex_blocklist.pb.h"

namespace android::apex {
// Parses and validates APEX blocklist. The blocklist is used only to block
// brand-new APEX. A brand-new APEX is blocked when the name exactly matches the
// block item and the version is smaller than or equal to the configured
// version.
android::base::Result<::apex::proto::ApexBlocklist> ParseBlocklist(
    const std::string& content);
// Reads and parses APEX blocklist from the file on disk.
android::base::Result<::apex::proto::ApexBlocklist> ReadBlocklist(
    const std::string& path);
}  // namespace android::apex
