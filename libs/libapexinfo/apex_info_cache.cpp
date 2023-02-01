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

#include "apex_info_cache.h"

#include <android-base/properties.h>

namespace android {
namespace apex {
namespace info {

// Check apexd.all.ready property before reading any APEX data, this
// is only needed on the target.
#ifdef __ANDROID__
static const bool CheckStatus = true;
#else
static const bool CheckStatus = false;
#endif

using ::android::base::ErrnoError;
using ::android::base::Error;
using ::android::base::GetProperty;
using ::android::base::Result;
using namespace std::literals;

ApexInfoCache::ApexInfoCache(const std::string &info_file)
    : info_file_(info_file), apex_ready_(!CheckStatus) {
  memset(&mtim_, 0, sizeof(mtim_));
}

// Read the current modify time of the file, check if any update to stored time
Result<std::pair<struct timespec, bool>> ApexInfoCache::ModifyTime() const {

  // Check if apex is ready prior to attempting to read data
  if (!apex_ready_ &&
      !(apex_ready_ = ("true" == GetProperty("apex.all.ready", "")))) {
    return Error() << "apex not ready";
  }
  struct stat cur;
  auto ret = stat(info_file_.c_str(), &cur);
  if (ret != 0) {
    return ErrnoError() << " info file " << info_file_;
  }
  const bool is_new_file = (memcmp(&mtim_, &cur.st_mtim, sizeof(mtim_)) != 0);
  return std::make_pair(cur.st_mtim, is_new_file);
}

// Check if new data is available
Result<bool> ApexInfoCache::HasNewData() const {
  auto ret = ModifyTime();
  if (!ret.ok()) {
    return ret.error();
  }
  return (*ret).second;
}
// Update the stored info held in object along with
// the current modification timestamp of the info file.
Result<bool> ApexInfoCache::Update() {
  auto ret = ModifyTime();
  if (!ret.ok()) {
    return ret.error();
  }
  if (!(*ret).second) {
    return false;
  }

  // Get latest
  auto latest = GetApexes(info_file_);
  if (!latest.ok()) {
    return Error() << "Failed to read apexes from " << info_file_ << " "
                   << latest.error();
  }

  // stat timestamp could have changed between check and
  // read this will get picked up on next poll
  mtim_ = (*ret).first;
  apex_info_ = std::move(*latest);
  return true;
}

} // namespace info
} // namespace apex
} // namespace android
