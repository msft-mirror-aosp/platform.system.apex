//
// Copyright (C) 2025 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#pragma once

#include <android-base/result.h>
#include <fcntl.h>
#include <sys/stat.h>

#include <functional>
#include <string>
#include <vector>

struct Entry {
  // file mode
  mode_t mode;

  // path to this entry.
  // - each entry should start with './'
  // - directory entry should end with '/'
  std::string path;

  std::string security_context;
};

// Generic lister
template <typename ReadEntry, typename ReadDir>
android::base::Result<std::vector<Entry>> List(ReadEntry read_entry,
                                               ReadDir read_dir) {
  namespace fs = std::filesystem;
  using namespace android::base;

  std::vector<Entry> entries;

  // Recursive visitor
  std::function<Result<void>(const fs::path& path)> visit =
      [&](const fs::path& path) -> Result<void> {
    auto entry = OR_RETURN(read_entry(path));
    entries.push_back(entry);

    if (S_ISDIR(entry.mode)) {
      auto names = OR_RETURN(read_dir(path));
      std::ranges::sort(names);
      for (auto name : names) {
        // Skip . and ..
        if (name == "." || name == "..") continue;
        OR_RETURN(visit(path / name));
      }
    }
    return {};
  };

  // Visit each path entry recursively starting from root
  OR_RETURN(visit("."));

  return entries;
}