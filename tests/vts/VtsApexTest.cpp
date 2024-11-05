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

#define LOG_TAG "VtsApexTest"

#include <android-base/file.h>
#include <fcntl.h>
#include <gtest/gtest.h>

#include <filesystem>

#include "apex_constants.h"

using android::base::unique_fd;

namespace android::apex {

static void ForEachPreinstalledApex(auto fn) {
  namespace fs = std::filesystem;
  std::error_code ec;
  for (const auto &dir : kApexPackageBuiltinDirs) {
    if (!fs::exists(dir, ec)) {
      if (ec) {
        FAIL() << "Can't to access " << dir << ": " << ec.message();
      }
      continue;
    }
    auto it = fs::directory_iterator(dir, ec);
    auto end = fs::directory_iterator();
    for (; !ec && it != end; it.increment(ec)) {
      fs::path path = *it;
      if (path.extension() != kApexPackageSuffix) {
        continue;
      }
      fn(path);
    }
    if (ec) {
      FAIL() << "Can't read " << dir << ": " << ec.message();
    }
  }
}

// Preinstalled APEX files (.apex) should be okay when opening with O_DIRECT
TEST(VtsApexTest, OpenPreinstalledApex) {
  ForEachPreinstalledApex([](auto path) {
    unique_fd fd(open(path.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECT));
    ASSERT_NE(fd.get(), -1)
        << "Can't open an APEX file " << path << ": " << strerror(errno);
  });
}

}  // namespace android::apex
