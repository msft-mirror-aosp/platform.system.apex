/*
 * Copyright (C) 2025 The Android Open Source Project
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
#include "apexd_checkpoint.h"

#include <android-base/file.h>

#include <filesystem>
#include <string>

#include "apexd.h"
#include "apexd_utils.h"

using android::base::ErrnoError;
using android::base::Error;
using android::base::Result;

namespace android::apex {

Result<void> AbortChanges() {
  CHECK(GetConfig().mount_before_data);

  auto checkpoint_file = GetConfig().checkpoint_file;
  std::error_code ec;
  if (!std::filesystem::exists(checkpoint_file, ec)) {
    if (ec) {
      return Error() << "Failed to check if checkpoint file exists: "
                     << ec.message();
    }
    // Not in the checkpoint mode.
    return {};
  }
  std::string content;
  if (!base::ReadFileToString(checkpoint_file, &content)) {
    return ErrnoError() << "Failed to read checkpoint file";
  }
  if (content == "0") {
    // Not in the checkpoint mode.
    return {};
  }
  // Abort the checkpoint mode by writing 0
  if (!base::WriteStringToFile("0", checkpoint_file)) {
    return ErrnoError() << "Failed to write to checkpoint file";
  }
  Reboot();
  return {};
}

bool InCheckpointMode() {
  CHECK(GetConfig().mount_before_data);
  auto checkpoint_file = GetConfig().checkpoint_file;
  std::string content;
  return base::ReadFileToString(checkpoint_file, &content) && content != "0";
}

}  // namespace android::apex