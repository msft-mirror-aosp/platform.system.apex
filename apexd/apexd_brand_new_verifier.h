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

#include "apex_file.h"

namespace android::apex {

// Returns the verification result of a specific brand-new package against the
// pre-installed public keys and blocklists. Verifies a brand-new APEX in that
// 1. brand-new APEX is enabled
// 2. it matches exactly one certificate in one of the built-in partitions
// 3. its name and version are not blocked by the blocklist in the matching
// partition
//
// The function is called in
// |SubmitStagedSession| (brand-new apex becomes 'staged')
// |ScanStagedSessionsDirAndStage| ('staged' apex becomes 'active')
// |ApexFileRepository::AddDataApex| (add 'active' apex to repository)
android::base::Result<void> VerifyBrandNewPackageAgainstPreinstalled(
    const ApexFile& apex);

// Returns the verification result of a specific brand-new package.
// Verifies a brand-new APEX in that its public key is the same as the existing
// active version if any. Pre-installed APEX is skipped.
//
// The function is called in
// |SubmitStagedSession| (brand-new apex becomes 'staged')
android::base::Result<void> VerifyBrandNewPackageAgainstActive(
    const ApexFile& apex);

}  // namespace android::apex
